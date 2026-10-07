/*
 * update.c — actualizaciones de OpenDock desde GitHub Releases.
 *
 * Solo se conecta si el usuario lo pidió: con "Buscar actualizaciones" activado (lo propone el
 * panel al instalar y se cambia en Configuración), una vez al día; o al elegir "Buscar
 * actualizaciones" en el menú de la bandeja. Nada más sale del equipo: una petición a la API
 * pública de GitHub con "OpenDock/<versión>" como agente.
 *
 * - Comprobar: GET api.github.com/repos/JosueGM24/OpenDock/releases/latest, en otro hilo.
 *   Si la etiqueta es más nueva que APP_VERSION, el notch avisa ("Pulsa para actualizar") y
 *   la bandeja ofrece "Actualizar a X.Y.Z". Un aviso por versión y día.
 * - Actualizar: se descargan OpenDock.exe y su .sha256 (solo de las versiones de este repo)
 *   junto al .exe en marcha, se comprueba el SHA-256 y, si el .exe viene firmado, su firma
 *   Authenticode. El .exe en marcha se renombra a .old (Windows lo permite), el nuevo ocupa su
 *   sitio y se relanza con --after-update <pid>, que espera a que este proceso termine, borra
 *   el .old y avisa "Actualizado a X.Y.Z". Si algo falla, se deshace.
 */
#include "app.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <wintrust.h>
#include <softpub.h>

#define UPD_HOST        L"api.github.com"
#define UPD_PATH        L"/repos/JosueGM24/OpenDock/releases/latest"
#define UPD_DL_PREFIX   L"https://github.com/JosueGM24/OpenDock/releases/download/"
#define UPD_FIRST_MS    90000               /* primera comprobación tras arrancar */
#define UPD_EVERY_MS    (24u * 3600u * 1000u)
#define TIMER_UPD       0x55D
#define TIMER_UPD_HINT  0x55E

/* Lo que encuentra la comprobación (se pasa del hilo a la interfaz en un bloque propio). */
typedef struct {
    BOOL    interactive;            /* pedida desde el menú: avisar también si no hay nada */
    BOOL    newer;
    wchar_t ver[32];                /* "2.2.3" */
    wchar_t exeUrl[512], shaUrl[512];
    wchar_t err[160];
} UpdInfo;

/* Descarga y comprobación: el resultado es el .exe nuevo, ya verificado, junto al actual. */
typedef struct {
    UpdInfo info;
    wchar_t newExe[MAX_PATH];
    wchar_t err[160];
} UpdJob;

static UpdInfo       s_found;           /* versión disponible (solo la interfaz) */
static volatile LONG s_busy;            /* una comprobación o descarga en marcha */

/* ───────────────────────── HTTP ───────────────────────── */
/* GET https://… a memoria (como mucho cap bytes). Sigue las redirecciones de GitHub. */
static BYTE *HttpGet(LPCWSTR url, DWORD cap, DWORD *outSize, wchar_t *err, int errCap)
{
    *outSize = 0;
    URL_COMPONENTS uc = { sizeof(uc) };
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        lstrcpynW(err, L"Dirección no válida", errCap);
        return NULL;
    }
    wchar_t agent[64];
    wsprintfW(agent, L"OpenDock/%s", APP_VERSION);
    HINTERNET s = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { lstrcpynW(err, L"Sin conexión", errCap); return NULL; }
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);
    BYTE *buf = NULL;
    DWORD n = 0, have = 0;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = NULL;
    if (c) r = WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    const wchar_t *hdr = L"Accept: application/vnd.github+json\r\n";
    DWORD status = 0, sl = sizeof(status);
    if (!r || !WinHttpSendRequest(r, hdr, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(r, NULL)) {
        lstrcpynW(err, L"Sin conexión con GitHub", errCap);
    } else if (!WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                    &status, &sl, WINHTTP_NO_HEADER_INDEX) || status != 200) {
        wsprintfW(err, L"GitHub respondió %u", status);
    } else {
        have = 64 * 1024;
        buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, have + 1);
        for (DWORD got = 0; buf; n += got) {
            if (n + 32768 > have) {
                if (have >= cap) { HeapFree(GetProcessHeap(), 0, buf); buf = NULL; lstrcpynW(err, L"Respuesta demasiado grande", errCap); break; }
                have = min(cap, have * 2);
                BYTE *nb = (BYTE *)HeapReAlloc(GetProcessHeap(), 0, buf, have + 1);
                if (!nb) { HeapFree(GetProcessHeap(), 0, buf); buf = NULL; break; }
                buf = nb;
            }
            got = 0;
            if (!WinHttpReadData(r, buf + n, min(32768u, have - n), &got)) {
                HeapFree(GetProcessHeap(), 0, buf); buf = NULL;
                lstrcpynW(err, L"La descarga se cortó", errCap);
                break;
            }
            if (!got) break;
        }
        if (buf) { buf[n] = 0; *outSize = n; }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return buf;
}

/* ───────────────────────── Versión ───────────────────────── */
static void ParseVer(const wchar_t *s, int v[3])
{
    v[0] = v[1] = v[2] = 0;
    if (*s == L'v' || *s == L'V') ++s;
    for (int i = 0; i < 3 && *s; ++i) {
        while (*s >= L'0' && *s <= L'9') v[i] = v[i] * 10 + (*s++ - L'0');
        if (*s != L'.') break;
        ++s;
    }
}

static BOOL IsNewer(const wchar_t *tag)
{
    int a[3], b[3];
    ParseVer(tag, a);
    ParseVer(APP_VERSION, b);
    for (int i = 0; i < 3; ++i) if (a[i] != b[i]) return a[i] > b[i];
    return FALSE;
}

/* Valor de texto de la primera "clave" a partir de from (JSON de GitHub: sin escapes en esto). */
static const char *JsonStr(const char *from, const char *key, char *out, int cap)
{
    char pat[64];
    wsprintfA(pat, "\"%s\"", key);
    const char *p = strstr(from, pat);
    if (!p) return NULL;
    p += lstrlenA(pat);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p != '"') return NULL;
    ++p;
    int n = 0;
    while (*p && *p != '"' && n < cap - 1) {
        if (*p == '\\') return NULL;                 /* lo que buscamos nunca lleva escapes */
        out[n++] = *p++;
    }
    out[n] = 0;
    return *p == '"' ? p + 1 : NULL;
}

static BOOL EndsWith(const char *s, const char *tail)
{
    const int a = lstrlenA(s), b = lstrlenA(tail);
    return a >= b && !lstrcmpiA(s + a - b, tail);
}

/* La comprobación (en cualquier hilo): llena info. */
static BOOL CheckNow(UpdInfo *info)
{
    DWORD n;
    char *json = (char *)HttpGet(L"https://" UPD_HOST UPD_PATH, 1024 * 1024, &n, info->err, 160);
    if (!json) return FALSE;
    char tag[64], url[512];
    BOOL ok = JsonStr(json, "tag_name", tag, 64) != NULL;
    if (ok) {
        MultiByteToWideChar(CP_UTF8, 0, tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag, -1, info->ver, 32);
        info->newer = IsNewer(info->ver);
        for (const char *p = json; (p = JsonStr(p, "browser_download_url", url, 512)) != NULL;) {
            wchar_t *dst = EndsWith(url, "/OpenDock.exe") ? info->exeUrl : EndsWith(url, "/OpenDock.exe.sha256") ? info->shaUrl : NULL;
            if (!dst) continue;
            MultiByteToWideChar(CP_UTF8, 0, url, -1, dst, 512);
            if (wcsncmp(dst, UPD_DL_PREFIX, lstrlenW(UPD_DL_PREFIX))) dst[0] = 0;   /* solo de este repositorio */
        }
        if (info->newer && (!info->exeUrl[0] || !info->shaUrl[0])) {
            info->newer = FALSE;
            lstrcpynW(info->err, L"La versión nueva aún no trae su .exe", 160);
        }
    } else lstrcpynW(info->err, L"Respuesta de GitHub inesperada", 160);
    HeapFree(GetProcessHeap(), 0, json);
    return ok;
}

/* ───────────────────────── Verificar ───────────────────────── */
static BOOL Sha256Hex(const BYTE *data, DWORD n, char out[65])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BYTE h[32];
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) return FALSE;
    const BOOL ok = BCryptHash(alg, NULL, 0, (PUCHAR)data, n, h, 32) == 0;
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return FALSE;
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) { out[i * 2] = hx[h[i] >> 4]; out[i * 2 + 1] = hx[h[i] & 15]; }
    out[64] = 0;
    return TRUE;
}

/* Firma Authenticode: válida si la trae; sin firma vale (el SHA-256 y HTTPS ya responden). */
static BOOL SignatureOk(LPCWSTR file, wchar_t *err, int errCap)
{
    WINTRUST_FILE_INFO fi = { sizeof(fi) };
    fi.pcwszFilePath = file;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd = { sizeof(wd) };
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    const LONG r = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    if (r == 0 || r == (LONG)TRUST_E_NOSIGNATURE) return TRUE;
    lstrcpynW(err, L"La firma del .exe nuevo no es válida", errCap);
    return FALSE;
}

/* Descarga y comprueba el .exe nuevo; lo deja como <exe en marcha>.new. */
static BOOL DownloadNow(UpdJob *j)
{
    wchar_t self[MAX_PATH];
    if (!GetModuleFileNameW(NULL, self, MAX_PATH)) { lstrcpynW(j->err, L"No se encuentra el .exe", 160); return FALSE; }
    DWORD ns, ne;
    char *sha = (char *)HttpGet(j->info.shaUrl, 4096, &ns, j->err, 160);
    if (!sha) return FALSE;
    char want[65] = { 0 };
    int k = 0;
    for (const char *p = sha; *p && k < 64; ++p) {
        const char ch = (char)(*p >= 'A' && *p <= 'F' ? *p + 32 : *p);
        if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')) want[k++] = ch;
        else if (k) break;
    }
    HeapFree(GetProcessHeap(), 0, sha);
    if (k != 64) { lstrcpynW(j->err, L"El .sha256 de la versión no es válido", 160); return FALSE; }

    BYTE *exe = HttpGet(j->info.exeUrl, 64u * 1024 * 1024, &ne, j->err, 160);
    if (!exe) return FALSE;
    char got[65];
    BOOL ok = ne > 4096 && exe[0] == 'M' && exe[1] == 'Z' && Sha256Hex(exe, ne, got) && !lstrcmpA(got, want);
    if (!ok) lstrcpynW(j->err, L"El .exe descargado no coincide con su SHA-256", 160);
    if (ok) {
        wsprintfW(j->newExe, L"%s.new", self);
        HANDLE f = CreateFileW(j->newExe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD w = 0;
        ok = f != INVALID_HANDLE_VALUE && WriteFile(f, exe, ne, &w, NULL) && w == ne;
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        if (!ok) { lstrcpynW(j->err, L"No se pudo guardar junto al .exe", 160); DeleteFileW(j->newExe); }
        else if (!SignatureOk(j->newExe, j->err, 160)) { DeleteFileW(j->newExe); ok = FALSE; }
    }
    HeapFree(GetProcessHeap(), 0, exe);
    return ok;
}

/* El .exe en marcha pasa a .old y el nuevo ocupa su sitio; si algo falla, se deshace. */
static BOOL SwapExe(LPCWSTR newExe, wchar_t *err, int errCap)
{
    wchar_t self[MAX_PATH], old[MAX_PATH + 8];
    if (!GetModuleFileNameW(NULL, self, MAX_PATH)) return FALSE;
    wsprintfW(old, L"%s.old", self);
    DeleteFileW(old);                                   /* de una actualización anterior */
    if (!MoveFileExW(self, old, MOVEFILE_REPLACE_EXISTING)) {
        lstrcpynW(err, L"No se pudo apartar el .exe actual", errCap);
        return FALSE;
    }
    if (!MoveFileExW(newExe, self, MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(old, self, MOVEFILE_REPLACE_EXISTING);   /* vuelta atrás */
        lstrcpynW(err, L"No se pudo colocar el .exe nuevo", errCap);
        return FALSE;
    }
    return TRUE;
}

/* ───────────────────────── Hilos ───────────────────────── */
static DWORD WINAPI CheckThread(LPVOID p)
{
    UpdInfo *info = (UpdInfo *)p;
    CheckNow(info);
    if (!PostMessageW(g_ctrl, WM_UPDATE, UPD_CHECKED, (LPARAM)info)) HeapFree(GetProcessHeap(), 0, info);
    InterlockedExchange(&s_busy, 0);
    return 0;
}

static DWORD WINAPI DownloadThread(LPVOID p)
{
    UpdJob *j = (UpdJob *)p;
    const BOOL ok = DownloadNow(j);
    if (!PostMessageW(g_ctrl, WM_UPDATE, ok ? UPD_READY : UPD_FAILED, (LPARAM)j)) {
        if (ok) DeleteFileW(j->newExe);
        HeapFree(GetProcessHeap(), 0, j);
    }
    InterlockedExchange(&s_busy, 0);
    return 0;
}

static void Spawn(LPTHREAD_START_ROUTINE fn, void *arg)
{
    HANDLE t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    if (t) CloseHandle(t);
    else { HeapFree(GetProcessHeap(), 0, arg); InterlockedExchange(&s_busy, 0); }
}

/* ───────────────────────── API ───────────────────────── */
void Upd_Check(BOOL interactive)
{
    if (InterlockedCompareExchange(&s_busy, 1, 0)) return;
    UpdInfo *info = (UpdInfo *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(UpdInfo));
    if (!info) { InterlockedExchange(&s_busy, 0); return; }
    info->interactive = interactive;
    Spawn(CheckThread, info);
}

void Upd_Apply(void)
{
    if (!s_found.newer || InterlockedCompareExchange(&s_busy, 1, 0)) return;
    UpdJob *j = (UpdJob *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(UpdJob));
    if (!j) { InterlockedExchange(&s_busy, 0); return; }
    j->info = s_found;
    wchar_t t[64];
    wsprintfW(t, L"Descargando OpenDock %s", s_found.ver);
    Notch_Show(NI_INFO, t, L"Se reinicia al terminar", -1, TRUE);
    Spawn(DownloadThread, j);
}

/* Versión disponible (para el menú de la bandeja), o NULL. */
LPCWSTR Upd_Available(void) { return s_found.newer ? s_found.ver : NULL; }

/* Un aviso por versión y día. */
static BOOL NotifiedToday(LPCWSTR ver)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    const DWORD today = (DWORD)st.wYear * 10000 + st.wMonth * 100 + st.wDay;
    wchar_t last[32] = L"";
    DWORD sz = sizeof(last);
    RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"UpdateNotifiedVer", RRF_RT_REG_SZ, NULL, last, &sz);
    if (!lstrcmpW(last, ver) && RegReadDword(L"UpdateNotifiedDay", 0) == today) return TRUE;
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, L"UpdateNotifiedVer", 0, REG_SZ, (const BYTE *)ver, (DWORD)((lstrlenW(ver) + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
    RegWriteDword(L"UpdateNotifiedDay", today);
    return FALSE;
}

/* Mensajes WM_UPDATE en el controlador. */
void Upd_OnMessage(WPARAM w, LPARAM l)
{
    wchar_t t[96];
    switch (w) {
    case UPD_CHECKED: {
        UpdInfo *info = (UpdInfo *)l;
        if (info->newer) {
            s_found = *info;
            wsprintfW(t, L"OpenDock %s disponible", info->ver);
            if (info->interactive || !NotifiedToday(info->ver))
                Notch_ShowAction(NI_INFO, t, L"Pulsa para actualizar", WM_UPDATE, UPD_APPLY);
        } else if (info->interactive) {
            if (info->err[0]) Notch_Show(NI_WARN, L"No se pudo buscar", info->err, -1, TRUE);
            else              Notch_Show(NI_CHECK, L"OpenDock está al día", APP_VERSION, -1, TRUE);
        }
        HeapFree(GetProcessHeap(), 0, info);
        break;
    }
    case UPD_APPLY:
        Upd_Apply();
        break;
    case UPD_READY: {
        UpdJob *j = (UpdJob *)l;
        wchar_t err[160] = L"", self[MAX_PATH], args[32];
        if (SwapExe(j->newExe, err, 160) && GetModuleFileNameW(NULL, self, MAX_PATH)) {
            wsprintfW(args, L"--after-update %lu", GetCurrentProcessId());
            App_Relaunch(self, args);            /* sale limpio (barra de tareas, reloj…) y arranca el nuevo */
        } else {
            DeleteFileW(j->newExe);
            Notch_Show(NI_WARN, L"No se pudo actualizar", err[0] ? err : L"Inténtalo más tarde", -1, TRUE);
        }
        HeapFree(GetProcessHeap(), 0, j);
        break;
    }
    case UPD_FAILED: {
        UpdJob *j = (UpdJob *)l;
        Notch_Show(NI_WARN, L"No se pudo actualizar", j->err[0] ? j->err : L"Inténtalo más tarde", -1, TRUE);
        HeapFree(GetProcessHeap(), 0, j);
        break;
    }
    }
}

/* Programa (o quita) la comprobación diaria según g_cfg.autoUpdate. */
void Upd_Schedule(void)
{
    if (!g_ctrl) return;
    if (g_cfg.autoUpdate == 1) SetTimer(g_ctrl, TIMER_UPD, UPD_FIRST_MS, NULL);
    else KillTimer(g_ctrl, TIMER_UPD);
}

/* Temporizadores del controlador: TRUE si era nuestro. */
BOOL Upd_Timer(UINT_PTR id)
{
    if (id == TIMER_UPD) {
        SetTimer(g_ctrl, TIMER_UPD, UPD_EVERY_MS, NULL);
        if (g_cfg.autoUpdate == 1) Upd_Check(FALSE);
        return TRUE;
    }
    if (id == TIMER_UPD_HINT) {
        KillTimer(g_ctrl, TIMER_UPD_HINT);
        if (g_cfg.autoUpdate < 0 && RegReadDword(L"UpdateHinted", 0) == 0) {
            RegWriteDword(L"UpdateHinted", 1);
            Notch_ShowAction(NI_INFO, L"Actualizaciones automáticas", L"Actívalas en Configuración", WM_SHOWPANEL, 0);
        }
        return TRUE;
    }
    return FALSE;
}

/* Al arrancar: limpieza de una actualización, aviso de que se hizo, y la programación. */
void Upd_Startup(DWORD afterPid)
{
    wchar_t self[MAX_PATH], old[MAX_PATH + 8], stale[MAX_PATH + 8];
    if (GetModuleFileNameW(NULL, self, MAX_PATH)) {
        wsprintfW(old, L"%s.old", self);
        wsprintfW(stale, L"%s.new", self);
        DeleteFileW(old);
        DeleteFileW(stale);
    }
    if (afterPid) {
        Inst_UpdateInfo();                       /* versión en Configuración → Aplicaciones */
        wchar_t t[64];
        wsprintfW(t, L"Actualizado a %s", APP_VERSION);
        Notch_Show(NI_CHECK, t, L"OpenDock se reinició", -1, TRUE);
    }
    Upd_Schedule();
    /* instalaciones de antes del actualizador: una sugerencia, una sola vez */
    if (g_cfg.autoUpdate < 0 && Inst_IsRunningInstalled() && RegReadDword(L"UpdateHinted", 0) == 0)
        SetTimer(g_ctrl, TIMER_UPD_HINT, 20000, NULL);
}

/* El proceso que se actualiza (--after-update <pid>): esperar a que termine antes de nada. */
void Upd_WaitFor(DWORD pid)
{
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (p) { WaitForSingleObject(p, 10000); CloseHandle(p); }
}

/* Prueba sin interfaz (--check-update-test <log>): comprobar, descargar, verificar y
 * reemplazar el propio .exe, apuntando cada paso en el registro de texto. */
int Upd_SelfTest(LPCWSTR logPath)
{
    HANDLE log = CreateFileW(logPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    char line[1024];
#define LOG(...) do { int n_ = wsprintfA(line, __VA_ARGS__); DWORD w_; if (log != INVALID_HANDLE_VALUE) WriteFile(log, line, n_, &w_, NULL); } while (0)
    UpdJob j;
    ZeroMemory(&j, sizeof(j));
    LOG("version actual: %ls\r\n", APP_VERSION);
    if (!CheckNow(&j.info)) { LOG("comprobar: ERROR %ls\r\n", j.info.err); goto done; }
    LOG("comprobar: ultima %ls, mas nueva: %d\r\nexe: %ls\r\nsha: %ls\r\n", j.info.ver, j.info.newer, j.info.exeUrl, j.info.shaUrl);
    if (!j.info.newer) goto done;
    if (!DownloadNow(&j)) { LOG("descargar: ERROR %ls\r\n", j.err); goto done; }
    LOG("descargar: OK, SHA-256 y firma comprobados -> %ls\r\n", j.newExe);
    wchar_t err[160] = L"";
    if (!SwapExe(j.newExe, err, 160)) { LOG("reemplazar: ERROR %ls\r\n", err); DeleteFileW(j.newExe); goto done; }
    LOG("reemplazar: OK (el anterior queda como .old)\r\n");
done:
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
#undef LOG
    return 0;
}
