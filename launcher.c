/*
 * launcher.c — buscador propio con la tecla Windows, en lugar del Inicio de Windows.
 *
 * Al pulsar y soltar la tecla Windows sola aparece una caja de búsqueda con las apps
 * recientes y los archivos abiertos hace poco. Al escribir busca entre todas las apps
 * (las de shell:AppsFolder, también las de la Tienda) y los archivos de la carpeta del
 * usuario. Todo sale de índices propios en memoria: no se consulta el servicio de
 * búsqueda de Windows y cada pulsación tarda milisegundos.
 *
 *  - Tecla: gancho de teclado de bajo nivel en un hilo propio (así, si el hilo principal
 *    se entretiene, Windows no lo quita). Con Win sola se traga su "soltar" y se reinyecta
 *    detrás de una tecla sin asignar (0xE8): Windows ve una combinación y no abre Inicio.
 *    (No vale un Win+F alta como atajo propio: Windows reserva algunas para teclas de
 *    Office, Copilot… y abriría esas apps.)
 *    Con cualquier otra tecla (Win+E, Win+R…) todo sigue igual; Ctrl+Esc sigue abriendo
 *    el Inicio de Windows. Con una ventana elevada delante no se intercepta nada.
 *  - Archivos: un hilo de baja prioridad recorre %USERPROFILE% (sin AppData, carpetas
 *    ocultas, .git, node_modules… ni carpetas de OneDrive que sólo están en la nube) y
 *    vigila altas, bajas y renombrados con ReadDirectoryChangesW para rehacer el índice.
 *  - Apps: shell:AppsFolder, enumerado en otro hilo; se abren con explorer.exe.
 *  - Recientes: lo abierto desde aquí, lo más usado según Windows (UserAssist) y la
 *    carpeta Reciente.
 */
#define COBJMACROS
#include "app.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <sddl.h>
#include <limits.h>
#include <math.h>

#define LN_CLASS     L"OpenDock.Launcher"
#define VK_MASK      0xE8           /* sin asignar: sólo para que Windows no abra Inicio */
#define FILE_CAP     400000
#define APP_CAP      1500
#define MAX_RES      10             /* filas con texto escrito (la última, buscar en la web) */
#define GRID_N       8              /* apps recientes */
#define RECENT_N     5              /* archivos recientes */
#define MRU_CAP      24
#define ICON_CAP     160
#define TIMER_CARET  1
#define TIMER_FOCUS  2

#ifndef FILE_ATTRIBUTE_RECALL_ON_OPEN
#define FILE_ATTRIBUTE_RECALL_ON_OPEN 0x00040000
#endif

/* medidas lógicas (96 ppp) */
#define LW      680
#define LPAD    12
#define QH      58
#define ROWH    52
#define LABELH  28
#define TILEH   100
#define FROWH   48
#define FOOTH   54

/* ───────────────────────── Comparación sin mayúsculas ni acentos ───────────────────────── */
static WCHAR *volatile s_fold;      /* por carácter: minúscula y sin tilde */
#define FOLD(c) (s_fold ? s_fold[(WCHAR)(c)] : (WCHAR)(c))

static void BuildFold(void)
{
    if (s_fold) return;
    WCHAR *t = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, 65536 * sizeof(WCHAR));
    if (!t) return;
    for (int c = 0; c < 65536; ++c) {
        WCHAR s = (WCHAR)c, d[8], b = s;
        if (c >= 0xD800 && c <= 0xDFFF) { t[c] = s; continue; }
        if (c > 0x7F && FoldStringW(MAP_COMPOSITE, &s, 1, d, 8) > 0) b = d[0];
        CharLowerBuffW(&b, 1);
        t[c] = b;
    }
    if (InterlockedCompareExchangePointer((PVOID volatile *)&s_fold, t, NULL)) HeapFree(GetProcessHeap(), 0, t);
}

typedef struct { WCHAR t[4][64]; int len[4], n; } Query;

static void MakeQuery(Query *q, const WCHAR *s)
{
    q->n = 0;
    while (*s && q->n < 4) {
        while (*s == L' ') ++s;
        int k = 0;
        while (*s && *s != L' ') { if (k < 63) q->t[q->n][k++] = FOLD(*s); ++s; }
        if (k) { q->t[q->n][k] = 0; q->len[q->n++] = k; }
    }
}

static int FindFold(const WCHAR *h, int n, const WCHAR *q, int m)
{
    for (int i = 0; i + m <= n; ++i) {
        if (FOLD(h[i]) != q[0]) continue;
        int k = 1;
        while (k < m && FOLD(h[i + k]) == q[k]) ++k;
        if (k == m) return i;
    }
    return -1;
}

static BOOL WordStart(const WCHAR *h, int i)
{
    if (i == 0) return TRUE;
    const WCHAR p = h[i - 1];
    if (p == L' ' || p == L'-' || p == L'_' || p == L'.' || p == L'(' || p == L'[' || p == L'+') return TRUE;
    return IsCharLowerW(p) && IsCharUpperW(h[i]);           /* camelCase */
}

/* INT_MIN si no casa. Mejor si empieza por lo buscado o en principio de palabra, si es el
 * nombre exacto (sin extensión) y cuanto más corto. */
static int Score(const WCHAR *name, int n, const Query *q)
{
    if (!q->n) return INT_MIN;
    int s = 0, first = -1;
    for (int k = 0; k < q->n; ++k) {
        int at = FindFold(name, n, q->t[k], q->len[k]);
        if (at < 0) return INT_MIN;
        if (!k) first = at;
        if (at && !WordStart(name, at))                     /* otra aparición en principio de palabra */
            for (int j = at + 1; j + q->len[k] <= n; ++j)
                if (WordStart(name, j) && FindFold(name + j, q->len[k], q->t[k], q->len[k]) == 0) { at = j; break; }
        s += at == 0 ? 60 : WordStart(name, at) ? 35 : 0;
    }
    int base = n;
    for (int i = n - 1; i > 0; --i) if (name[i] == L'.') { base = i; break; }
    if (q->n == 1 && first == 0 && base == q->len[0]) s += 120;
    return s - n / 3;
}

/* Iniciales: "Visual Studio Code" → "vsc". */
static BOOL AcronymMatch(const WCHAR *name, const Query *q)
{
    if (q->n != 1 || q->len[0] < 2) return FALSE;
    int k = 0;
    for (int i = 0; name[i] && k < q->len[0]; ++i)
        if (name[i] != L' ' && WordStart(name, i) && (i == 0 || !IsCharUpperW(name[i - 1]))) {
            if (FOLD(name[i]) != q->t[0][k]) return FALSE;
            ++k;
        }
    return k == q->len[0];
}

/* ───────────────────────── Índice de archivos ───────────────────────── */
typedef struct { DWORD parent, name; WORD len; BYTE dir, depth; LONGLONG mt; } FEnt;   /* dir: 0 archivo · 1 carpeta · 2 carpeta sin recorrer */
typedef struct { FEnt *e; DWORD n, cap; WCHAR *pool; DWORD used, pcap; } FIdx;

static FIdx    *s_fidx;
static SRWLOCK  s_flock = SRWLOCK_INIT;
static HANDLE   s_stop;                 /* evento manual: OpenDock se cierra o el buscador se apaga */
static HANDLE   s_filesThread, s_appsThread;

static void FFree(FIdx *x)
{
    if (!x) return;
    if (x->e) HeapFree(GetProcessHeap(), 0, x->e);
    if (x->pool) HeapFree(GetProcessHeap(), 0, x->pool);
    HeapFree(GetProcessHeap(), 0, x);
}

static BOOL FAdd(FIdx *x, DWORD parent, const WCHAR *name, int len, BYTE dir, LONGLONG mt, BYTE depth)
{
    if (x->n >= FILE_CAP || len <= 0 || len > 1000) return FALSE;
    if (x->n == x->cap) {
        const DWORD nc = x->cap ? x->cap * 2 : 16384;
        FEnt *e = x->e ? (FEnt *)HeapReAlloc(GetProcessHeap(), 0, x->e, nc * sizeof(FEnt))
                       : (FEnt *)HeapAlloc(GetProcessHeap(), 0, nc * sizeof(FEnt));
        if (!e) return FALSE;
        x->e = e; x->cap = nc;
    }
    if (x->used + (DWORD)len + 1 > x->pcap) {
        const DWORD nc = max(x->pcap * 2, 1u << 18);
        WCHAR *p = x->pool ? (WCHAR *)HeapReAlloc(GetProcessHeap(), 0, x->pool, nc * sizeof(WCHAR))
                           : (WCHAR *)HeapAlloc(GetProcessHeap(), 0, nc * sizeof(WCHAR));
        if (!p) return FALSE;
        x->pool = p; x->pcap = nc;
    }
    FEnt *e = &x->e[x->n++];
    e->parent = parent; e->name = x->used; e->len = (WORD)len; e->dir = dir; e->depth = depth; e->mt = mt;
    CopyMemory(x->pool + x->used, name, len * sizeof(WCHAR));
    x->used += len;
    x->pool[x->used++] = 0;
    return TRUE;
}

static int FPath(const FIdx *x, DWORD i, WCHAR *out, int cap)
{
    DWORD chain[64];
    int d = 0, o = 0;
    for (DWORD k = i; k != (DWORD)-1 && d < 64; k = x->e[k].parent) chain[d++] = k;
    for (int j = d - 1; j >= 0; --j) {
        const FEnt *e = &x->e[chain[j]];
        if (o && out[o - 1] != L'\\') { if (o + 1 >= cap) return 0; out[o++] = L'\\'; }
        if (o + e->len >= cap) return 0;
        CopyMemory(out + o, x->pool + e->name, e->len * sizeof(WCHAR));
        o += e->len;
    }
    out[o] = 0;
    return o;
}

static BOOL SkipDir(const WCHAR *n)
{
    static const WCHAR *const k[] = { L"AppData", L"node_modules", L"__pycache__", L"venv", L"site-packages",
                                      L"bower_components", L"intermediates", L"Application Data", L"Local Settings" };
    if (n[0] == L'.' || n[0] == L'$') return TRUE;
    for (int i = 0; i < (int)(sizeof(k) / sizeof(k[0])); ++i) if (!lstrcmpiW(n, k[i])) return TRUE;
    return FALSE;
}

/* A lo ancho (las carpetas se añaden al final y se recorren después): con el tope de
 * entradas, lo que queda fuera es lo más profundo. */
static void Walk(FIdx *x, const WCHAR *root)
{
    static WCHAR path[2048];
    WIN32_FIND_DATAW fd;
    const DWORD r0 = x->n;
    if (!FAdd(x, (DWORD)-1, root, lstrlenW(root), 1, 0, 0)) return;
    for (DWORD i = r0; i < x->n; ++i) {
        if (x->e[i].dir != 1) continue;
        if (!(i & 31) && WaitForSingleObject(s_stop, 0) == WAIT_OBJECT_0) return;
        const int pl = FPath(x, i, path, 2000);
        if (!pl) continue;
        lstrcpyW(path + pl, path[pl - 1] == L'\\' ? L"*" : L"\\*");
        HANDLE h = FindFirstFileExW(path, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        const BYTE depth = x->e[i].depth;
        do {
            const WCHAR *n = fd.cFileName;
            if (n[0] == L'.' && (!n[1] || (n[1] == L'.' && !n[2]))) continue;
            const DWORD a = fd.dwFileAttributes;
            if (a & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            BYTE dir = 0;
            if (a & FILE_ATTRIBUTE_DIRECTORY) {
                if (SkipDir(n)) continue;
                if ((a & FILE_ATTRIBUTE_REPARSE_POINT) &&
                    (fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT || fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK)) continue;
                /* sólo en la nube (OneDrive): listarla la descargaría; se indexa, sin entrar */
                dir = (a & FILE_ATTRIBUTE_RECALL_ON_OPEN) || depth >= 40 ? 2 : 1;
            }
            const LONGLONG mt = (LONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime;
            if (!FAdd(x, i, n, lstrlenW(n), dir, mt, (BYTE)(depth + 1))) { FindClose(h); return; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

static void ProfileDir(WCHAR *out)
{
    out[0] = 0;
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", out, MAX_PATH);
    if (!n || n >= MAX_PATH) out[0] = 0;
}

static void Rebuild(void)
{
    WCHAR prof[MAX_PATH];
    ProfileDir(prof);
    if (!prof[0]) return;
    FIdx *x = (FIdx *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(FIdx));
    if (!x) return;
    Walk(x, prof);
    if (WaitForSingleObject(s_stop, 0) == WAIT_OBJECT_0) { FFree(x); return; }
    AcquireSRWLockExclusive(&s_flock);
    FIdx *old = s_fidx;
    s_fidx = x;
    ReleaseSRWLockExclusive(&s_flock);
    FFree(old);
}

/* ¿Un cambio de la carpeta del usuario afecta al índice? (no si es de AppData, de una
 * carpeta oculta o de un temporal de Office o del navegador) */
static BOOL Relevant(const WCHAR *rel, int n)
{
    int s = 0;
    for (int i = 0; i <= n; ++i) {
        if (i < n && rel[i] != L'\\') continue;
        WCHAR comp[MAX_PATH];
        const int len = i - s;
        if (len > 0 && len < MAX_PATH) {
            CopyMemory(comp, rel + s, len * sizeof(WCHAR));
            comp[len] = 0;
            if (i < n && SkipDir(comp)) return FALSE;
            if (i == n) {
                if (comp[0] == L'~') return FALSE;
                const WCHAR *dot = wcsrchr(comp, L'.');
                if (dot && (!lstrcmpiW(dot, L".tmp") || !lstrcmpiW(dot, L".crdownload") || !lstrcmpiW(dot, L".part"))) return FALSE;
            }
        }
        s = i + 1;
    }
    return TRUE;
}

static DWORD WINAPI FilesThread(LPVOID u)
{
    (void)u;
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    BuildFold();
    Rebuild();
    DWORD lastBuild = GetTickCount();
    WCHAR prof[MAX_PATH];
    ProfileDir(prof);
    HANDLE dir = prof[0] ? CreateFileW(prof, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                       OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL)
                         : INVALID_HANDLE_VALUE;
    if (dir == INVALID_HANDLE_VALUE) return 0;
    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    static DWORD buf[16384];                    /* 64 KB, alineado a DWORD */
    BOOL dirty = FALSE;
    DWORD dirtyAt = 0;
    while (ov.hEvent) {
        ResetEvent(ov.hEvent);
        if (!ReadDirectoryChangesW(dir, buf, sizeof(buf), TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
                                   NULL, &ov, NULL)) break;
        for (;;) {
            DWORD wait = INFINITE;
            if (dirty) {                        /* 10 s sin cambios, y no más de una vez por minuto */
                const DWORD now = GetTickCount(), due = max(dirtyAt + 10000, lastBuild + 60000);
                wait = (int)(due - now) > 0 ? due - now : 0;
            }
            HANDLE hs[2] = { ov.hEvent, s_stop };
            const DWORD r = WaitForMultipleObjects(2, hs, FALSE, wait);
            if (r == WAIT_OBJECT_0 + 1) goto out;
            if (r == WAIT_TIMEOUT) { Rebuild(); lastBuild = GetTickCount(); dirty = FALSE; continue; }
            DWORD got = 0;
            if (!GetOverlappedResult(dir, &ov, &got, FALSE)) goto out;
            if (!got) { dirty = TRUE; dirtyAt = GetTickCount(); }     /* se desbordó: rehacer */
            else {
                const BYTE *p = (const BYTE *)buf;
                for (;;) {
                    const FILE_NOTIFY_INFORMATION *fi = (const FILE_NOTIFY_INFORMATION *)p;
                    if (Relevant(fi->FileName, (int)(fi->FileNameLength / sizeof(WCHAR)))) { dirty = TRUE; dirtyAt = GetTickCount(); }
                    if (!fi->NextEntryOffset) break;
                    p += fi->NextEntryOffset;
                }
            }
            break;                              /* volver a pedir cambios */
        }
    }
out:
    CancelIo(dir);
    CloseHandle(dir);
    if (ov.hEvent) CloseHandle(ov.hEvent);
    return 0;
}

/* ───────────────────────── Índice de apps ───────────────────────── */
typedef struct { WCHAR name[96], id[256]; } AppEnt;
static AppEnt       *s_apps;
static int           s_napps;
static SRWLOCK       s_alock = SRWLOCK_INIT;
static volatile LONG s_appsBusy;
static DWORD         s_appsAt;

static BOOL StrRet(STRRET *r, WCHAR *out, int cap)
{
    out[0] = 0;
    if (r->uType == STRRET_WSTR && r->pOleStr) { lstrcpynW(out, r->pOleStr, cap); CoTaskMemFree(r->pOleStr); }
    else if (r->uType == STRRET_CSTR) MultiByteToWideChar(CP_ACP, 0, r->cStr, -1, out, cap);
    return out[0] != 0;
}

/* ¿Es una app? Las rutas que no son ejecutables (manuales en PDF, .bat, páginas…) no. */
static BOOL LooksApp(const WCHAR *id)
{
    if (!wcsncmp(id, L"http", 4)) return FALSE;
    if (!wcschr(id, L'\\')) return TRUE;                  /* AppUserModelID */
    const WCHAR *dot = wcsrchr(id, L'.');
    return dot && (!lstrcmpiW(dot, L".exe") || !lstrcmpiW(dot, L".msc") || !lstrcmpiW(dot, L".cpl"));
}

static DWORD WINAPI AppsThread(LPVOID u)
{
    (void)u;
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    AppEnt *v = (AppEnt *)HeapAlloc(GetProcessHeap(), 0, APP_CAP * sizeof(AppEnt));
    int n = 0;
    PIDLIST_ABSOLUTE root = NULL;
    IShellFolder *sf = NULL;
    IEnumIDList *en = NULL;
    if (v && SUCCEEDED(SHParseDisplayName(L"shell:AppsFolder", NULL, &root, 0, NULL)) &&
        SUCCEEDED(SHBindToObject(NULL, (PCIDLIST_ABSOLUTE)root, NULL, &IID_IShellFolder, (void **)&sf)) &&
        IShellFolder_EnumObjects(sf, NULL, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &en) == S_OK) {
        PITEMID_CHILD c = NULL;
        while (n < APP_CAP && IEnumIDList_Next(en, 1, &c, NULL) == S_OK) {
            STRRET s1, s2;
            WCHAR name[96], id[256];
            if (SUCCEEDED(IShellFolder_GetDisplayNameOf(sf, c, SHGDN_NORMAL, &s1)) && StrRet(&s1, name, 96) &&
                SUCCEEDED(IShellFolder_GetDisplayNameOf(sf, c, SHGDN_INFOLDER | SHGDN_FORPARSING, &s2)) && StrRet(&s2, id, 256) &&
                lstrlenW(id) < 255 && LooksApp(id)) {
                lstrcpyW(v[n].name, name);
                lstrcpyW(v[n].id, id);
                ++n;
            }
            CoTaskMemFree(c);
            if (WaitForSingleObject(s_stop, 0) == WAIT_OBJECT_0) break;
        }
    }
    if (en) IEnumIDList_Release(en);
    if (sf) IShellFolder_Release(sf);
    if (root) CoTaskMemFree(root);
    if (v && n) {
        AcquireSRWLockExclusive(&s_alock);
        AppEnt *old = s_apps;
        s_apps = v; s_napps = n;
        ReleaseSRWLockExclusive(&s_alock);
        if (old) HeapFree(GetProcessHeap(), 0, old);
        s_appsAt = GetTickCount();
    } else if (v) HeapFree(GetProcessHeap(), 0, v);
    CoUninitialize();
    InterlockedExchange(&s_appsBusy, 0);
    return 0;
}

static void RefreshApps(BOOL force)
{
    if (!force && s_apps && GetTickCount() - s_appsAt < 5 * 60 * 1000) return;
    if (InterlockedExchange(&s_appsBusy, 1)) return;
    if (s_appsThread) { CloseHandle(s_appsThread); s_appsThread = NULL; }
    s_appsThread = CreateThread(NULL, 0, AppsThread, NULL, 0, NULL);
    if (!s_appsThread) InterlockedExchange(&s_appsBusy, 0);
}

/* ───────────────────────── Recientes ───────────────────────── */
#define MRU_VALUE L"LauncherRecent"

static int MruRead(WCHAR list[][MAX_PATH + 8])
{
    static WCHAR buf[MRU_CAP * (MAX_PATH + 8) + 2];
    DWORD size = sizeof(buf) - 2 * sizeof(WCHAR);
    ZeroMemory(buf, sizeof(buf));
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, MRU_VALUE, RRF_RT_REG_MULTI_SZ, NULL, buf, &size) != ERROR_SUCCESS) return 0;
    int n = 0;
    for (const WCHAR *p = buf; *p && n < MRU_CAP; p += lstrlenW(p) + 1) lstrcpynW(list[n++], p, MAX_PATH + 8);
    return n;
}

static void MruAdd(const WCHAR *key)
{
    static WCHAR list[MRU_CAP][MAX_PATH + 8], buf[MRU_CAP * (MAX_PATH + 8) + 2];
    if (lstrlenW(key) >= MAX_PATH + 8) return;
    int n = MruRead(list);
    for (int i = 0; i < n; ++i)
        if (!lstrcmpiW(list[i], key)) { MoveMemory(list[i], list[i + 1], (n - i - 1) * sizeof(list[0])); --n; break; }
    if (n == MRU_CAP) --n;
    MoveMemory(list[1], list[0], n * sizeof(list[0]));
    lstrcpyW(list[0], key);
    ++n;
    int o = 0;
    for (int i = 0; i < n; ++i) { lstrcpyW(buf + o, list[i]); o += lstrlenW(list[i]) + 1; }
    buf[o++] = 0;
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, MRU_VALUE, 0, REG_MULTI_SZ, (const BYTE *)buf, (DWORD)(o * sizeof(WCHAR)));
        RegCloseKey(k);
    }
}

/* Lo más usado según Windows: UserAssist guarda por app (nombre en ROT13) las veces que se
 * abrió y la última vez; los nombres coinciden con los de shell:AppsFolder. */
typedef struct { WCHAR id[256]; LONGLONG when; } Used;
static int UserAssist(Used *out, int cap)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\UserAssist\\{CEBFF5CD-ACE2-4F4F-9178-9926F41749EA}\\Count",
                      0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return 0;
    int n = 0;
    for (DWORD i = 0; i < 4000; ++i) {
        WCHAR name[300];
        BYTE data[80];
        DWORD nl = 300, dl = sizeof(data), type = 0;
        const LONG r = RegEnumValueW(k, i, name, &nl, NULL, &type, data, &dl);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS || type != REG_BINARY || dl < 68 || nl >= 256) continue;
        LONGLONG when;
        CopyMemory(&when, data + 60, sizeof(when));
        if (when <= 0) continue;
        for (WCHAR *p = name; *p; ++p) {
            if (*p >= L'a' && *p <= L'z') *p = L'a' + (*p - L'a' + 13) % 26;
            else if (*p >= L'A' && *p <= L'Z') *p = L'A' + (*p - L'A' + 13) % 26;
        }
        /* "{GUID de carpeta conocida}\resto": como en shell:AppsFolder, con la ruta real */
        if (nl > 38 && name[0] == L'{' && name[37] == L'}' && name[38] == L'\\') {
            WCHAR g[39];
            GUID kf;
            PWSTR kp = NULL;
            lstrcpynW(g, name, 39);
            if (SUCCEEDED(CLSIDFromString(g, &kf)) && SUCCEEDED(SHGetKnownFolderPath(&kf, 0, NULL, &kp)) && kp) {
                WCHAR full[300];
                if (lstrlenW(kp) + lstrlenW(name + 38) < 256) { wsprintfW(full, L"%s%s", kp, name + 38); lstrcpyW(name, full); }
                CoTaskMemFree(kp);
            }
        }
        int at = n < cap ? n++ : cap;           /* los más recientes primero */
        while (at > 0 && out[at - 1].when < when) { if (at < cap) out[at] = out[at - 1]; --at; }
        if (at < cap) { lstrcpynW(out[at].id, name, 256); out[at].when = when; }
    }
    RegCloseKey(k);
    return n;
}

/* ───────────────────────── Iconos ───────────────────────── */
/* Caché de iconos (premultiplicados). Se cargan en un hilo aparte: la lista se pinta al
 * momento y cada icono aparece cuando está listo. Sólo el hilo de la ventana reutiliza
 * huecos; el de carga sólo rellena los pendientes. */
enum { IC_EMPTY, IC_PENDING, IC_DONE };
typedef struct { WCHAR key[300]; int px; DWORD *bits; int w, h; DWORD used; volatile LONG state; } IconC;
static IconC            s_icons[ICON_CAP];
static CRITICAL_SECTION s_iconLock;
static HANDLE           s_iconEvt, s_iconThread;
static int              s_iconQ[ICON_CAP], s_iconQn;
static volatile HWND    s_iconHwnd;     /* la ventana abierta, para avisarla */
#define WM_LN_ICON (WM_APP + 91)
#define WM_LN_BACKDROP (WM_APP + 92)   /* el buscador ya está a la vista: ahora su fondo */

static DWORD *BitmapBits(HBITMAP bmp, int *ow, int *oh)
{
    BITMAP bm;
    if (!GetObjectW(bmp, sizeof(bm), &bm) || bm.bmWidth < 1 || bm.bmWidth > 256 || abs(bm.bmHeight) > 256) return NULL;
    const int w = bm.bmWidth, h = abs(bm.bmHeight);
    DWORD *buf = (DWORD *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)w * h * 4);
    if (!buf) return NULL;
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    HDC dc = GetDC(NULL);
    const BOOL ok = GetDIBits(dc, bmp, 0, h, buf, &bi, DIB_RGB_COLORS) == h;
    ReleaseDC(NULL, dc);
    if (!ok) { HeapFree(GetProcessHeap(), 0, buf); return NULL; }
    BOOL premult = TRUE, anyAlpha = FALSE;
    for (int i = 0; i < w * h; ++i) {
        const DWORD v = buf[i], a = v >> 24;
        if (a) anyAlpha = TRUE;
        if (((v >> 16) & 255) > a || ((v >> 8) & 255) > a || (v & 255) > a) premult = FALSE;
    }
    for (int i = 0; i < w * h && !(premult && anyAlpha); ++i) {
        const DWORD v = buf[i], a = anyAlpha ? v >> 24 : 255;
        buf[i] = a << 24 | (((v >> 16) & 255) * a / 255) << 16 | (((v >> 8) & 255) * a / 255) << 8 | ((v & 255) * a / 255);
    }
    *ow = w; *oh = h;
    return buf;
}

static DWORD *ItemIcon(const WCHAR *parsing, int px, int *w, int *h, BOOL iconOnly)
{
    IShellItem *si = NULL;
    if (FAILED(SHCreateItemFromParsingName(parsing, NULL, &IID_IShellItem, (void **)&si))) return NULL;
    IShellItemImageFactory *f = NULL;
    HBITMAP bmp = NULL;
    if (SUCCEEDED(IShellItem_QueryInterface(si, &IID_IShellItemImageFactory, (void **)&f))) {
        SIZE sz = { px, px };
        IShellItemImageFactory_GetImage(f, sz, (iconOnly ? SIIGBF_ICONONLY : 0) | SIIGBF_RESIZETOFIT, &bmp);
        IShellItemImageFactory_Release(f);
    }
    IShellItem_Release(si);
    if (!bmp) return NULL;
    DWORD *bits = BitmapBits(bmp, w, h);
    DeleteObject(bmp);
    return bits;
}

/* Icono de un tipo de archivo sin tocar el disco (por extensión). */
static DWORD *TypeIcon(const WCHAR *name, BOOL dir, int px, int *w, int *h)
{
    SHFILEINFOW fi;
    ZeroMemory(&fi, sizeof(fi));
    if (!SHGetFileInfoW(name, dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &fi, sizeof(fi),
                        SHGFI_USEFILEATTRIBUTES | SHGFI_ICON | (px > 20 ? SHGFI_LARGEICON : SHGFI_SMALLICON)) || !fi.hIcon) return NULL;
    ICONINFO ii;
    DWORD *bits = NULL;
    if (GetIconInfo(fi.hIcon, &ii)) {
        BITMAP bm;
        if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm)) {
            const int iw = bm.bmWidth, ih = abs(bm.bmHeight);
            Canvas c;
            if (iw > 0 && iw <= 256 && Canvas_Init(&c, iw, ih)) {
                ZeroMemory(c.px, (SIZE_T)iw * ih * 4);
                DrawIconEx(c.dc, 0, 0, fi.hIcon, iw, ih, 0, NULL, DI_NORMAL);
                GdiFlush();
                bits = (DWORD *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)iw * ih * 4);
                if (bits) {
                    BOOL anyAlpha = FALSE;
                    for (int i = 0; i < iw * ih; ++i) if (c.px[i] >> 24) { anyAlpha = TRUE; break; }
                    for (int i = 0; i < iw * ih; ++i) bits[i] = anyAlpha ? c.px[i] : (c.px[i] ? 0xFF000000 | c.px[i] : 0);
                    *w = iw; *h = ih;
                }
                Canvas_Free(&c);
            }
        }
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
    }
    DestroyIcon(fi.hIcon);
    return bits;
}

static DWORD *LoadIconBits(const WCHAR *key, int px, int *w, int *h)
{
    if (!wcsncmp(key, L"app:", 4)) {
        WCHAR p[300];
        wsprintfW(p, L"shell:AppsFolder\\%.270s", key + 4);
        return ItemIcon(p, px, w, h, TRUE);
    }
    if (!wcsncmp(key, L"pic:", 4)) {          /* foto de la cuenta: miniatura recortada en círculo */
        DWORD *b = ItemIcon(key + 4, px, w, h, FALSE);
        if (b) {
            const float cx = *w * 0.5f, cy = *h * 0.5f, r = min(*w, *h) * 0.5f;
            for (int y = 0; y < *h; ++y)
                for (int x = 0; x < *w; ++x) {
                    const float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy)) - r;
                    const float cov = d < -0.5f ? 1.0f : d > 0.5f ? 0.0f : 0.5f - d;
                    if (cov >= 1.0f) continue;
                    const DWORD v = b[y * *w + x], k = (DWORD)(cov * 256.0f);
                    b[y * *w + x] = ((v >> 24) * k >> 8) << 24 | (((v >> 16) & 255) * k >> 8) << 16 | (((v >> 8) & 255) * k >> 8) << 8 | ((v & 255) * k >> 8);
                }
        }
        return b;
    }
    if (!wcsncmp(key, L"dir:", 4)) return TypeIcon(L"carpeta", TRUE, px, w, h);
    if (!wcsncmp(key, L"ext:", 4)) return TypeIcon(key[4] ? key + 4 : L"archivo", FALSE, px, w, h);
    return ItemIcon(key, px, w, h, TRUE);
}

static DWORD WINAPI IconThread(LPVOID u)
{
    (void)u;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    HANDLE hs[2] = { s_iconEvt, s_stop };
    while (WaitForMultipleObjects(2, hs, FALSE, INFINITE) == WAIT_OBJECT_0) {
        for (;;) {
            WCHAR key[300];
            int px, slot = -1;
            EnterCriticalSection(&s_iconLock);
            if (s_iconQn) {
                slot = s_iconQ[0];
                MoveMemory(&s_iconQ[0], &s_iconQ[1], --s_iconQn * sizeof(int));
                lstrcpyW(key, s_icons[slot].key);
                px = s_icons[slot].px;
            }
            LeaveCriticalSection(&s_iconLock);
            if (slot < 0) break;
            int w = 0, h = 0;
            DWORD *bits = LoadIconBits(key, px, &w, &h);
            EnterCriticalSection(&s_iconLock);
            IconC *c = &s_icons[slot];
            if (c->state == IC_PENDING && c->px == px && !lstrcmpW(c->key, key)) {
                c->bits = bits; c->w = w; c->h = h;     /* los fallos también: no se reintentan */
                InterlockedExchange(&c->state, IC_DONE);
                bits = NULL;
            }
            LeaveCriticalSection(&s_iconLock);
            if (bits) HeapFree(GetProcessHeap(), 0, bits);
            const HWND n = s_iconHwnd;
            if (n) PostMessageW(n, WM_LN_ICON, 0, 0);
        }
    }
    CoUninitialize();
    return 0;
}

/* key: "app:<id>" · "dir:" · "ext:.docx" · o la ruta de un .exe/.lnk (icono propio).
 * NULL si aún no está (se pide) o no tiene. */
static const IconC *GetIcon(const WCHAR *key, int px)
{
    static DWORD tick;
    if (!s_iconEvt) return NULL;
    IconC *slot = NULL;
    for (int i = 0; i < ICON_CAP; ++i) {
        IconC *c = &s_icons[i];
        if (c->state != IC_EMPTY && c->px == px && !lstrcmpiW(c->key, key)) {
            c->used = ++tick;
            return c->state == IC_DONE && c->bits ? c : NULL;
        }
        if (c->state != IC_PENDING && (!slot || c->used < slot->used)) slot = c;
    }
    if (!slot) return NULL;
    EnterCriticalSection(&s_iconLock);
    if (slot->bits) HeapFree(GetProcessHeap(), 0, slot->bits);
    slot->bits = NULL; slot->w = slot->h = 0;
    lstrcpynW(slot->key, key, 300);
    slot->px = px;
    slot->used = ++tick;
    slot->state = IC_PENDING;
    if (s_iconQn < ICON_CAP) s_iconQ[s_iconQn++] = (int)(slot - s_icons);
    LeaveCriticalSection(&s_iconLock);
    SetEvent(s_iconEvt);
    return NULL;
}

static void IconKeyForFile(const WCHAR *path, BOOL dir, WCHAR *out)
{
    if (dir) { lstrcpyW(out, L"dir:"); return; }
    const WCHAR *dot = wcsrchr(path, L'.'), *slash = wcsrchr(path, L'\\');
    if (!dot || (slash && dot < slash)) { lstrcpyW(out, L"ext:"); return; }
    if (!lstrcmpiW(dot, L".exe") || !lstrcmpiW(dot, L".lnk") || !lstrcmpiW(dot, L".ico") || !lstrcmpiW(dot, L".url"))
        lstrcpynW(out, path, 300);
    else
        wsprintfW(out, L"ext:%.20s", dot);
}

/* ───────────────────────── Ventana ───────────────────────── */
enum { RK_APP, RK_FILE, RK_DIR, RK_WEB };
enum { FB_USER, FB_SETTINGS, FB_LOCK, FB_SIGNOUT, FB_SLEEP, FB_RESTART, FB_POWER, FB_COUNT };

typedef struct {
    int   kind;
    WCHAR title[128], sub[300];
    WCHAR target[MAX_PATH + 32];    /* id de app, ruta o consulta web */
    WCHAR icon[300];
    RECT  rc;
    BOOL  tile;
    /* entrada con muelle: ap 0→1 (posición y opacidad), tras `delay`; el icono hace "pop" al llegar */
    float ap, av, delay, is, isv;
    float hs, hsv;                  /* escala del señalado (crece, como en el dock) */
    BOOL  iconUp;
} Res;

static struct {
    HWND     hwnd;
    Pop      pop;
    Canvas   cv, bg;
    PopGlass glass;
    UINT     dpi;
    RECT     mon;
    BOOL     light;
    DWORD    fg, fg2, fg3, line, accent;
    HFONT    fQuery, fTitle, fSub, fTile, fLabel, fIcon, fGlyph;
    WCHAR    q[256];
    int      qlen, caret;
    BOOL     selAll, caretOn;
    Res      res[MAX_RES + RECENT_N + GRID_N];
    int      nres, ngrid, sel;
    int      labelY[2];             /* sin texto: "Recientes" y "Archivos recientes" (-1 si no van) */
    int      w, h, maxH, ox, oy;
    DWORD    openedAt, closedAt;
    DWORD    bgBase;                /* color medio del fondo: hacia él se funde lo que entra */
    /* animación: alto del panel */
    float    hcur, hv;
    BOOL     anim, tracking;
    BOOL     menu;                  /* menú del clic derecho abierto: perder el foco no cierra */
    /* barra de sesión: huecos, señalado/pulsado y su escala */
    RECT     fb[FB_COUNT];
    int      fhot, fpress;
    float    fs[FB_COUNT], fsv[FB_COUNT];
    IconC    fglyph[FB_COUNT];
    LARGE_INTEGER last;
} L;

static int SS(int v) { return MulDiv(v, (int)L.dpi, 96); }

static void FreeFonts(void)
{
    HFONT *f[] = { &L.fQuery, &L.fTitle, &L.fSub, &L.fTile, &L.fLabel, &L.fIcon, &L.fGlyph };
    for (int i = 0; i < 7; ++i) if (*f[i]) { DeleteObject(*f[i]); *f[i] = NULL; }
}

static void MakeFonts(void)
{
    FreeFonts();
    LPCWSTR ui = Gfx_UiFace();
    L.fQuery = Gfx_Font(ui, SS(20), FW_NORMAL, CLEARTYPE_QUALITY);
    L.fTitle = Gfx_Font(ui, SS(15), FW_SEMIBOLD, CLEARTYPE_QUALITY);
    L.fSub   = Gfx_Font(ui, SS(13), FW_SEMIBOLD, CLEARTYPE_QUALITY);
    L.fTile  = Gfx_Font(ui, SS(12), FW_SEMIBOLD, CLEARTYPE_QUALITY);     /* nombres de la fila de apps: caben enteros */
    L.fLabel = Gfx_Font(ui, SS(12), FW_SEMIBOLD, CLEARTYPE_QUALITY);
    L.fIcon  = Gfx_Font(Gfx_IconFace(), SS(18), FW_NORMAL, CLEARTYPE_QUALITY);
    L.fGlyph = Gfx_Font(Gfx_IconFace(), SS(20), FW_NORMAL, CLEARTYPE_QUALITY);
}

/* Ruta para enseñar: dentro de la carpeta del usuario, sin ella. */
static void ShortDir(const WCHAR *path, WCHAR *out, int cap)
{
    WCHAR prof[MAX_PATH], dir[MAX_PATH + 32];
    lstrcpynW(dir, path, MAX_PATH + 32);
    WCHAR *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    ProfileDir(prof);
    const int pl = lstrlenW(prof);
    if (pl && !_wcsnicmp(dir, prof, pl) && (dir[pl] == L'\\' || !dir[pl])) {
        if (!dir[pl]) lstrcpynW(out, L"Carpeta personal", cap);
        else lstrcpynW(out, dir + pl + 1, cap);
    } else lstrcpynW(out, dir, cap);
}

static void SetFileRes(Res *r, const WCHAR *path, BOOL dir)
{
    ZeroMemory(r, sizeof(*r));
    r->kind = dir ? RK_DIR : RK_FILE;
    lstrcpynW(r->target, path, MAX_PATH + 32);
    const WCHAR *name = wcsrchr(path, L'\\');
    lstrcpynW(r->title, name ? name + 1 : path, 128);
    ShortDir(path, r->sub, 300);
    IconKeyForFile(path, dir, r->icon);
}

static void SetAppRes(Res *r, const AppEnt *a)
{
    ZeroMemory(r, sizeof(*r));
    r->kind = RK_APP;
    lstrcpynW(r->title, a->name, 128);
    lstrcpynW(r->target, a->id, MAX_PATH + 32);
    wsprintfW(r->icon, L"app:%.290s", a->id);
    lstrcpyW(r->sub, L"Aplicación");
}

/* Sin texto: apps recientes (lo abierto desde aquí y lo último según Windows) y archivos
 * de la carpeta Reciente. */
static void BuildEmpty(void)
{
    L.nres = L.ngrid = 0;
    static WCHAR mru[MRU_CAP][MAX_PATH + 8];
    static Used used[48];
    const int nm = MruRead(mru), nu = UserAssist(used, 48);
    AcquireSRWLockShared(&s_alock);
    for (int pass = 0; pass < 2 && L.ngrid < GRID_N; ++pass) {
        const int n = pass ? nu : nm;
        for (int i = 0; i < n && L.ngrid < GRID_N; ++i) {
            const WCHAR *id = pass ? used[i].id : !wcsncmp(mru[i], L"app:", 4) ? mru[i] + 4 : NULL;
            if (!id) continue;
            const AppEnt *a = NULL;
            for (int k = 0; k < s_napps && !a; ++k) if (!lstrcmpiW(s_apps[k].id, id)) a = &s_apps[k];
            if (!a) continue;
            BOOL dup = FALSE;
            for (int k = 0; k < L.ngrid && !dup; ++k) dup = !lstrcmpiW(L.res[k].target, a->id);
            if (dup) continue;
            SetAppRes(&L.res[L.ngrid], a);
            L.res[L.ngrid++].tile = TRUE;
        }
    }
    ReleaseSRWLockShared(&s_alock);
    L.nres = L.ngrid;

    /* carpeta Reciente: los accesos directos más nuevos */
    WCHAR dir[MAX_PATH], pat[MAX_PATH + 8];
    PWSTR rp = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_Recent, 0, NULL, &rp))) return;
    lstrcpynW(dir, rp, MAX_PATH);
    CoTaskMemFree(rp);
    wsprintfW(pat, L"%.250s\\*.lnk", dir);
    struct { WCHAR name[MAX_PATH]; LONGLONG t; } top[16];
    int nt = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const LONGLONG t = (LONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime;
            int at = nt < 16 ? nt++ : 16;
            while (at > 0 && top[at - 1].t < t) { if (at < 16) top[at] = top[at - 1]; --at; }
            if (at < 16) { lstrcpynW(top[at].name, fd.cFileName, MAX_PATH); top[at].t = t; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    IShellLinkW *sl = NULL;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) return;
    IPersistFile *pf = NULL;
    if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
        for (int i = 0; i < nt && L.nres - L.ngrid < RECENT_N; ++i) {
            WCHAR lnk[MAX_PATH * 2], target[MAX_PATH];
            wsprintfW(lnk, L"%.250s\\%.250s", dir, top[i].name);
            if (FAILED(IPersistFile_Load(pf, lnk, STGM_READ))) continue;
            target[0] = 0;
            if (FAILED(IShellLinkW_GetPath(sl, target, MAX_PATH, NULL, 0)) || !target[0]) continue;
            const DWORD a = GetFileAttributesW(target);
            if (a == INVALID_FILE_ATTRIBUTES) continue;
            SetFileRes(&L.res[L.nres++], target, (a & FILE_ATTRIBUTE_DIRECTORY) != 0);
        }
        IPersistFile_Release(pf);
    }
    IShellLinkW_Release(sl);
}

static void Insert(int *idx, int *sc, int *n, int cap, int i, int s)
{
    int at = *n < cap ? (*n)++ : cap;
    while (at > 0 && sc[at - 1] < s) { if (at < cap) { sc[at] = sc[at - 1]; idx[at] = idx[at - 1]; } --at; }
    if (at < cap) { sc[at] = s; idx[at] = i; }
}

static void BuildResults(void)
{
    L.nres = L.ngrid = 0;
    Query q;
    MakeQuery(&q, L.q);
    if (!q.n) return;
    static WCHAR mru[MRU_CAP][MAX_PATH + 8];
    const int nm = MruRead(mru);

    /* apps: primero, y las abiertas desde aquí hace poco, antes */
    int ai[5], as[5], na = 0;
    AcquireSRWLockShared(&s_alock);
    for (int i = 0; i < s_napps; ++i) {
        const AppEnt *a = &s_apps[i];
        int s = Score(a->name, lstrlenW(a->name), &q);
        if (s == INT_MIN && AcronymMatch(a->name, &q)) s = 40;
        if (s == INT_MIN) continue;
        if (FindFold(a->name, lstrlenW(a->name), L"uninstall", 9) >= 0 || FindFold(a->name, lstrlenW(a->name), L"desinstal", 9) >= 0) s -= 80;
        for (int k = 0; k < nm; ++k)
            if (!wcsncmp(mru[k], L"app:", 4) && !lstrcmpiW(mru[k] + 4, a->id)) { s += 40 - k; break; }
        Insert(ai, as, &na, 5, i, s);
    }
    for (int k = 0; k < na; ++k) SetAppRes(&L.res[L.nres++], &s_apps[ai[k]]);
    ReleaseSRWLockShared(&s_alock);

    /* archivos y carpetas */
    int fi[MAX_RES], fs[MAX_RES], nf = 0;
    const int room = MAX_RES - 1 - L.nres;
    AcquireSRWLockShared(&s_flock);
    const FIdx *x = s_fidx;
    if (x && room > 0) {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        const LONGLONG tnow = (LONGLONG)now.dwHighDateTime << 32 | now.dwLowDateTime, day = 864000000000LL;
        for (DWORD i = 1; i < x->n; ++i) {
            const FEnt *e = &x->e[i];
            int s = Score(x->pool + e->name, e->len, &q);
            if (s == INT_MIN) continue;
            if (tnow - e->mt < 7 * day) s += 20;
            else if (tnow - e->mt < 30 * day) s += 10;
            s -= 2 * e->depth;
            Insert(fi, fs, &nf, room, (int)i, s);
        }
        for (int k = 0; k < nf; ++k) {
            WCHAR path[MAX_PATH + 32];
            if (FPath(x, (DWORD)fi[k], path, MAX_PATH + 32)) SetFileRes(&L.res[L.nres++], path, x->e[fi[k]].dir != 0);
        }
    }
    ReleaseSRWLockShared(&s_flock);

    Res *w = &L.res[L.nres++];
    ZeroMemory(w, sizeof(*w));
    w->kind = RK_WEB;
    wsprintfW(w->title, L"Buscar \x201C%.100s\x201D en la web", L.q);
    lstrcpyW(w->sub, L"Google");
    lstrcpynW(w->target, L.q, MAX_PATH + 32);
}

/* Coloca los resultados y devuelve la altura del contenido. */
static int Layout(void)
{
    const int pad = SS(LPAD);
    int y = SS(QH) + SS(6);
    L.labelY[0] = L.labelY[1] = -1;
    if (!L.qlen) {
        if (L.ngrid) {
            L.labelY[0] = y; y += SS(LABELH);
            const int tw = (L.w - 2 * pad) / GRID_N;
            for (int i = 0; i < L.ngrid; ++i) SetRect(&L.res[i].rc, pad + i * tw, y, pad + (i + 1) * tw, y + SS(TILEH));
            y += SS(TILEH) + SS(4);
        }
        if (L.nres > L.ngrid) {
            L.labelY[1] = y; y += SS(LABELH);
            for (int i = L.ngrid; i < L.nres; ++i) { SetRect(&L.res[i].rc, pad, y, L.w - pad, y + SS(FROWH)); y += SS(FROWH); }
        }
        if (!L.nres) y += SS(30);
    } else {
        for (int i = 0; i < L.nres; ++i) { SetRect(&L.res[i].rc, pad, y, L.w - pad, y + SS(ROWH)); y += SS(ROWH); }
    }
    const int h = min(L.maxH, y + SS(8) + SS(FOOTH)), ft = h - SS(FOOTH), bw = SS(40);
    SetRect(&L.fb[FB_USER], pad, ft + SS(6), pad + SS(240), h - SS(6));
    for (int i = FB_COUNT - 1, x = L.w - pad; i >= FB_SETTINGS; --i, x -= bw) SetRect(&L.fb[i], x - bw, ft + SS(7), x, h - SS(7));
    return h;
}

/* Icono (premultiplicado) centrado en (cx, cy), escalado y con opacidad: bilineal. */
static void BlitIcon(Canvas *c, const IconC *ic, float cx, float cy, float scale, float alpha)
{
    if (!ic || !ic->bits || scale < 0.02f || alpha < 0.004f) return;
    alpha = min(1.0f, alpha);
    const float w = ic->w * scale, h = ic->h * scale, x0f = cx - w * 0.5f, y0f = cy - h * 0.5f, inv = 1.0f / scale;
    const int x0 = max(0, (int)floorf(x0f)), x1 = min(c->w, (int)ceilf(x0f + w));
    const int y0 = max(0, (int)floorf(y0f)), y1 = min(c->h, (int)ceilf(y0f + h));
    const int W = ic->w, H = ic->h;
    GdiFlush();
    for (int y = y0; y < y1; ++y) {
        const float fy = (y + 0.5f - y0f) * inv - 0.5f;
        const int iy = (int)floorf(fy);
        const float ty = fy - iy;
        for (int x = x0; x < x1; ++x) {
            const float fx = (x + 0.5f - x0f) * inv - 0.5f;
            const int ix = (int)floorf(fx);
            const float tx = fx - ix;
            float acc[4] = { 0, 0, 0, 0 };
            for (int q = 0; q < 4; ++q) {
                const int sx = ix + (q & 1), sy = iy + (q >> 1);
                if (sx < 0 || sy < 0 || sx >= W || sy >= H) continue;
                const float wq = ((q & 1) ? tx : 1 - tx) * ((q >> 1) ? ty : 1 - ty);
                const DWORD p = ic->bits[sy * W + sx];
                acc[0] += (p & 255) * wq; acc[1] += ((p >> 8) & 255) * wq; acc[2] += ((p >> 16) & 255) * wq; acc[3] += (p >> 24) * wq;
            }
            const float a = acc[3] * alpha;
            if (a < 0.5f) continue;
            DWORD *d = &c->px[y * c->w + x];
            const float ia = 1.0f - a / 255.0f;
            DWORD o = 0;
            for (int ch = 0; ch < 3; ++ch) {
                const float v = ((*d >> (ch * 8)) & 255) * ia + acc[ch] * alpha;
                o |= (DWORD)max(0, min(255, (int)(v + 0.5f))) << (ch * 8);
            }
            *d = o;
        }
    }
}

static float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* Pide el icono; la primera vez que llega empieza su "pop" con rebote. */
static const IconC *RowIcon(Res *r, int px)
{
    const IconC *ic = GetIcon(r->icon, px);
    if (ic && !r->iconUp) { r->iconUp = TRUE; r->is = 0.45f; r->isv = 0; }
    return ic;
}

/* ───────────────────────── Barra de sesión (abajo) ─────────────────────────
 * Como la del Inicio de Windows: la cuenta a la izquierda y, a la derecha, Configuración,
 * Bloquear, Cerrar sesión, Suspender, Reiniciar y Apagar. Lo señalado crece. */
#ifndef EWX_HYBRID_SHUTDOWN
#define EWX_HYBRID_SHUTDOWN 0x00400000
#endif
#ifndef SHTDN_REASON_FLAG_PLANNED
#define SHTDN_REASON_FLAG_PLANNED 0x80000000
#endif
static const WCHAR kFootGlyph[FB_COUNT] = { 0, 0xE713, 0xE72E, 0xF3B1, 0xE708, 0xE777, 0xE7E8 };
static const WCHAR *const kFootName[FB_COUNT] = {
    L"Cuenta", L"Configuraci\x00F3n", L"Bloquear", L"Cerrar sesi\x00F3n", L"Suspender", L"Reiniciar", L"Apagar"
};
static WCHAR s_userName[128], s_userPic[MAX_PATH];

/* Nombre para mostrar y foto de la cuenta (la que Windows guarda para el Inicio). */
static void UserInfo(void)
{
    if (s_userName[0]) return;
    HMODULE m = LoadLibraryExW(L"secur32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (m) {
        typedef BOOLEAN (WINAPI *GetNameEx)(int, LPWSTR, PULONG);
        GetNameEx f = (GetNameEx)(void *)GetProcAddress(m, "GetUserNameExW");
        ULONG n = 128;
        if (!f || !f(3 /* NameDisplay */, s_userName, &n)) s_userName[0] = 0;
        FreeLibrary(m);
    }
    if (!s_userName[0]) { DWORD n = 128; if (!GetUserNameW(s_userName, &n)) lstrcpyW(s_userName, L"Usuario"); }
    HANDLE tok = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        BYTE buf[256];
        DWORD sz = 0;
        LPWSTR sid = NULL;
        if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &sz) && ConvertSidToStringSidW(((TOKEN_USER *)buf)->User.Sid, &sid)) {
            WCHAR key[256];
            wsprintfW(key, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AccountPicture\\Users\\%.180s", sid);
            static const WCHAR *const kVals[] = { L"Image192", L"Image240", L"Image96" };
            for (int i = 0; i < 3 && !s_userPic[0]; ++i) {
                DWORD n = sizeof(s_userPic);
                if (RegGetValueW(HKEY_LOCAL_MACHINE, key, kVals[i], RRF_RT_REG_SZ, NULL, s_userPic, &n) != ERROR_SUCCESS ||
                    GetFileAttributesW(s_userPic) == INVALID_FILE_ATTRIBUTES) s_userPic[0] = 0;
            }
            LocalFree(sid);
        }
        CloseHandle(tok);
    }
}

static void FreeGlyphs(void)
{
    for (int i = 0; i < FB_COUNT; ++i) {
        if (L.fglyph[i].bits) HeapFree(GetProcessHeap(), 0, L.fglyph[i].bits);
        ZeroMemory(&L.fglyph[i], sizeof(L.fglyph[i]));
    }
}

/* Los iconos de la barra se dibujan una vez (a 1,25×, para crecer nítidos) y se escalan. */
#define GLYPH_K 1.25f
static void MakeGlyphs(void)
{
    FreeGlyphs();
    const int px = (int)(SS(17) * GLYPH_K + 0.5f), box = px + SS(6);
    HFONT f = Gfx_Font(Gfx_IconFace(), px, FW_NORMAL, ANTIALIASED_QUALITY);
    Canvas c;
    if (!f || !Canvas_Init(&c, box, box)) { if (f) DeleteObject(f); return; }
    const DWORD fg = L.fg;
    for (int i = 1; i < FB_COUNT; ++i) {
        const WCHAR g[2] = { kFootGlyph[i], 0 };
        Canvas_Clear(&c, 0);
        Gfx_Text(&c, f, g, 0, 0, box, box, 0xFFFFFF, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        GdiFlush();
        DWORD *bits = (DWORD *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)box * box * 4);
        if (!bits) continue;
        for (int k = 0; k < box * box; ++k) {
            const DWORD a = (c.px[k] >> 8) & 255;          /* blanco sobre negro: la cobertura */
            bits[k] = a << 24 | (((fg >> 16) & 255) * a / 255) << 16 | (((fg >> 8) & 255) * a / 255) << 8 | ((fg & 255) * a / 255);
        }
        L.fglyph[i].bits = bits; L.fglyph[i].w = L.fglyph[i].h = box;
    }
    Canvas_Free(&c);
    DeleteObject(f);
}

static void EnablePrivilege(LPCWSTR name)
{
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return;
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(NULL, name, &tp.Privileges[0].Luid)) AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
    CloseHandle(tok);
}

static void Close(void);
static void Backdrop_Close(void);
static void Backdrop_Open(HMONITOR hm, HWND above);
typedef struct { HWND notify; RECT r; int blur; Canvas full; } BdJob;     /* imagen del fondo, hecha en otro hilo */
static void Backdrop_Show(BdJob *j, HWND above);
static void FootAction(int i)
{
    AllowSetForegroundWindow(ASFW_ANY);
    Close();
    const DWORD why = SHTDN_REASON_FLAG_PLANNED;      /* "otro (planeado)", como el menú de Windows */
    switch (i) {
    case FB_USER:     App_ShellOpen(L"ms-settings:yourinfo"); break;
    case FB_SETTINGS: App_ShellOpen(L"ms-settings:"); break;
    case FB_LOCK:     LockWorkStation(); break;
    case FB_SIGNOUT:  ExitWindowsEx(EWX_LOGOFF, why); break;
    case FB_SLEEP: {
        HMODULE m = LoadLibraryExW(L"powrprof.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (m) {
            typedef BOOLEAN (WINAPI *Suspend)(BOOLEAN, BOOLEAN, BOOLEAN);
            Suspend f = (Suspend)(void *)GetProcAddress(m, "SetSuspendState");
            if (f) f(FALSE, FALSE, FALSE);
            FreeLibrary(m);
        }
        break;
    }
    case FB_RESTART:  EnablePrivilege(SE_SHUTDOWN_NAME); ExitWindowsEx(EWX_REBOOT, why); break;
    case FB_POWER:    EnablePrivilege(SE_SHUTDOWN_NAME); ExitWindowsEx(EWX_SHUTDOWN | EWX_POWEROFF | EWX_HYBRID_SHUTDOWN, why); break;
    }
}

static int HitFoot(int x, int y)
{
    POINT p = { x, y };
    for (int i = 0; i < FB_COUNT; ++i) if (PtInRect(&L.fb[i], p)) return i;
    return -1;
}

/* Dibuja la barra pegada al borde de abajo del alto actual (H). */
static void PaintFooter(Canvas *c, int H)
{
    const int pad = SS(LPAD), qh = SS(QH), ft = H - SS(FOOTH), dy = H - L.h;
    if (ft <= qh + 2) return;
    Gfx_FillRRect(c, (float)pad, (float)ft, (float)(L.w - 2 * pad), 1.0f, 0, L.line, 1.0f);
    RECT u = L.fb[FB_USER];
    OffsetRect(&u, 0, dy);
    const int av = SS(30), ax = u.left + SS(6);
    const float acy = (u.top + u.bottom) * 0.5f;
    WCHAR key[MAX_PATH + 8];
    wsprintfW(key, L"pic:%.260s", s_userPic);
    const IconC *pic = s_userPic[0] ? GetIcon(key, av) : NULL;
    if (pic) BlitIcon(c, pic, ax + av * 0.5f, acy, L.fs[FB_USER], 1.0f);
    else {
        const WCHAR ini[2] = { s_userName[0] ? (WCHAR)(ULONG_PTR)CharUpperW((LPWSTR)(ULONG_PTR)s_userName[0]) : L'?', 0 };
        Gfx_FillCircle(c, ax + av * 0.5f, acy, av * 0.5f * L.fs[FB_USER], L.accent, 1.0f);
        Gfx_Text(c, L.fLabel, ini, ax, u.top, av, u.bottom - u.top, 0xFFFFFF, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    const int nx = ax + av + SS(10);
    Gfx_Text(c, L.fTitle, s_userName, nx, u.top, u.right - nx, u.bottom - u.top, L.fg, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    for (int i = FB_SETTINGS; i < FB_COUNT; ++i) {
        RECT b = L.fb[i];
        OffsetRect(&b, 0, dy);
        BlitIcon(c, &L.fglyph[i], (b.left + b.right) * 0.5f, (b.top + b.bottom) * 0.5f, L.fs[i] / GLYPH_K, L.fhot == i ? 1.0f : 0.78f);
    }
    if (L.fhot >= FB_SETTINGS) {        /* el nombre de la acción, a su izquierda */
        RECT b = L.fb[FB_SETTINGS];
        OffsetRect(&b, 0, dy);
        Gfx_Text(c, L.fSub, kFootName[L.fhot], b.left - SS(170), b.top, SS(160), b.bottom - b.top, L.fg2, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}

static void Kick(void);

static void Paint(void)
{
    if (!L.hwnd) return;
    const int H = max(SS(QH) + 2, min(L.bg.h, (int)(L.hcur + 0.5f)));
    if (L.cv.w != L.w || L.cv.h != H) {
        Canvas_Free(&L.cv);
        if (!Canvas_Init(&L.cv, L.w, H)) return;
    }
    Canvas *c = &L.cv;
    GdiFlush();
    for (int y = 0; y < H; ++y) CopyMemory(&c->px[y * c->w], &L.bg.px[y * L.bg.w], (SIZE_T)L.w * 4);
    const int pad = SS(LPAD), qh = SS(QH);

    /* caja de búsqueda */
    Gfx_Text(c, L.fGlyph, L"\xE721", pad + SS(6), 0, SS(30), qh, L.fg2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    const int tx = pad + SS(46), tw = L.w - tx - pad - SS(90);
    if (L.qlen) {
        if (L.selAll) {
            const int sw = min(tw, Gfx_TextWidth(L.fQuery, L.q));
            Gfx_FillRRect(c, (float)tx - 2, (float)(qh - SS(28)) / 2, (float)sw + 4, (float)SS(28), (float)SS(4), L.accent, 0.35f);
        }
        Gfx_Text(c, L.fQuery, L.q, tx, 0, tw, qh, L.fg, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        Gfx_Text(c, L.fQuery, L"Busca apps y archivos", tx, 0, tw, qh, L.fg3, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    if (L.caretOn && !L.selAll) {
        WCHAR pre[256];
        lstrcpynW(pre, L.q, L.caret + 1);
        const int cx = tx + min(tw, Gfx_TextWidth(L.fQuery, pre));
        Gfx_FillRRect(c, (float)cx, (float)(qh - SS(24)) / 2, (float)max(1, SS(2)), (float)SS(24), 1.0f, L.accent, 1.0f);
    }
    {   /* a la derecha: el índice, si aún se está haciendo */
        AcquireSRWLockShared(&s_flock);
        const BOOL ready = s_fidx != NULL;
        ReleaseSRWLockShared(&s_flock);
        if (!ready) Gfx_Text(c, L.fSub, L"Indexando\x2026", L.w - pad - SS(100), 0, SS(96), qh, L.fg3, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    if (H > qh + 1) Gfx_FillRRect(c, (float)pad, (float)qh, (float)(L.w - 2 * pad), 1.0f, 0, L.line, Clamp01((L.hcur - qh) / SS(16)));

    /* rótulos: entran con la primera fila de su sección */
    if (L.labelY[0] >= 0 && L.ngrid)
        Gfx_Text(c, L.fLabel, L"Recientes", pad + SS(8), L.labelY[0], L.w, SS(LABELH), Gfx_Mix(L.bgBase, L.fg2, Clamp01(L.res[0].ap)), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (L.labelY[1] >= 0 && L.nres > L.ngrid)
        Gfx_Text(c, L.fLabel, L"Archivos recientes", pad + SS(8), L.labelY[1], L.w, SS(LABELH), Gfx_Mix(L.bgBase, L.fg2, Clamp01(L.res[L.ngrid].ap)), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (!L.qlen && !L.nres)
        Gfx_Text(c, L.fSub, L"Escribe para buscar entre tus apps y archivos", 0, qh + SS(6), L.w, SS(30), L.fg3, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    for (int i = 0; i < L.nres; ++i) {
        Res *r = &L.res[i];
        const float a = Clamp01(r->ap);
        if (a < 0.01f) continue;
        const int lift = (int)lroundf((1.0f - r->ap) * SS(14));     /* sube a su sitio (y rebota) */
        RECT rc = r->rc;
        OffsetRect(&rc, 0, lift);
        if (rc.top >= H) continue;
        const int rw = rc.right - rc.left, rh = rc.bottom - rc.top;
        if (r->tile) {
            const int is = SS(44);
            const float pop = (0.55f + 0.45f * r->ap) * (r->iconUp ? r->is : 1.0f) * r->hs;
            BlitIcon(c, RowIcon(r, is), rc.left + rw * 0.5f, rc.top + SS(10) + is * 0.5f, pop, a);
            Gfx_Text(c, L.fTile, r->title, rc.left + SS(1), rc.top + SS(10) + is + SS(4), rw - SS(2), SS(38), Gfx_Mix(L.bgBase, L.fg, a),
                     DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS | DT_EDITCONTROL);
            continue;
        }
        const int is = r->kind == RK_APP ? SS(32) : SS(28), ix = rc.left + SS(8);
        if (r->kind == RK_WEB) Gfx_Text(c, L.fIcon, L"\xE774", ix, rc.top, is, rh, Gfx_Mix(L.bgBase, L.fg2, a), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        else BlitIcon(c, RowIcon(r, is), ix + is * 0.5f, rc.top + rh * 0.5f, (r->iconUp ? r->is : 1.0f) * r->hs, a);
        /* el texto del señalado se aparta un poco, al ritmo de su icono */
        const int lx = ix + is + SS(12) + (int)lroundf((r->hs - 1.0f) * SS(22)), lw = rw - (lx - rc.left) - SS(80), half = rh / 2;
        Gfx_Text(c, L.fTitle, r->title, lx, rc.top + SS(3), lw, half, Gfx_Mix(L.bgBase, L.fg, a), DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
        Gfx_Text(c, L.fSub, r->sub, lx, rc.top + half + SS(1), lw, half - SS(3), Gfx_Mix(L.bgBase, L.fg2, a), DT_LEFT | DT_TOP | DT_SINGLELINE | DT_PATH_ELLIPSIS);
        static const WCHAR *const kKind[] = { L"App", L"Archivo", L"Carpeta", L"Web" };
        if (L.qlen) Gfx_Text(c, L.fSub, kKind[r->kind], rc.right - SS(80), rc.top, SS(70), rh, Gfx_Mix(L.bgBase, L.fg3, a), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    PaintFooter(c, H);
    Pop_Present(&L.pop, c);
    /* un icono que acaba de llegar empieza su "pop": que haya fotogramas */
    for (int i = 0; i < L.nres; ++i) if (L.res[i].iconUp && L.res[i].is < 0.999f && !L.anim) { Kick(); break; }
}

/* ── animación (el "Rebote" configurado decide cuánto rebota) ── */
static void Spring(float *x, float *v, float target, float dt, float k, float z)
{
    const float acc = (target - *x) * k - *v * 2.0f * sqrtf(k) * z;
    *v += acc * dt; *x += *v * dt;
}

static BOOL Settled(float *x, float *v, float t, float eps, float veps)
{
    if (fabsf(*x - t) < eps && fabsf(*v) < veps) { *x = t; *v = 0; return TRUE; }
    return FALSE;
}

static void Kick(void)
{
    if (!L.hwnd) return;
    /* el reloj sólo se pone a cero si estaba parado (si no, cada tecla frenaría los muelles) */
    if (!L.anim) { QueryPerformanceCounter(&L.last); L.anim = TRUE; }
    Pop_Hold(&L.pop, TRUE);
}

static void Backdrop_Step(void);
static BOOL Advance(void)
{
    Backdrop_Step();
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    float dt = (float)(t.QuadPart - L.last.QuadPart) / (float)f.QuadPart;
    L.last = t;
    if (dt > 0.05f) dt = 0.05f;
    const float z = Pop_Zeta();
    BOOL moving = FALSE;

    for (int k = 0; k < 2; ++k) Spring(&L.hcur, &L.hv, (float)L.h, dt * 0.5f, 240.0f, max(0.42f, z));
    if (!Settled(&L.hcur, &L.hv, (float)L.h, 0.4f, 3.0f)) moving = TRUE;


    for (int i = 0; i < L.nres; ++i) {
        Res *r = &L.res[i];
        if (r->delay > 0) { r->delay -= dt; moving = TRUE; continue; }
        for (int k = 0; k < 2; ++k) Spring(&r->ap, &r->av, 1.0f, dt * 0.5f, 300.0f, max(0.40f, z));
        if (!Settled(&r->ap, &r->av, 1.0f, 0.002f, 0.02f)) moving = TRUE;
        if (r->iconUp) {
            for (int k = 0; k < 2; ++k) Spring(&r->is, &r->isv, 1.0f, dt * 0.5f, 430.0f, max(0.30f, z * 0.8f));
            if (!Settled(&r->is, &r->isv, 1.0f, 0.002f, 0.02f)) moving = TRUE;
        }
        const float hg = i == L.sel ? (r->tile ? 1.16f : 1.22f) : 1.0f;     /* el señalado crece, con rebote */
        for (int k = 0; k < 2; ++k) Spring(&r->hs, &r->hsv, hg, dt * 0.5f, 420.0f, z);
        if (!Settled(&r->hs, &r->hsv, hg, 0.0008f, 0.01f)) moving = TRUE;
    }
    for (int i = 0; i < FB_COUNT; ++i) {
        const float tg = L.fpress == i ? 0.88f : L.fhot == i ? (i == FB_USER ? 1.08f : 1.2f) : 1.0f;
        for (int k = 0; k < 2; ++k) Spring(&L.fs[i], &L.fsv[i], tg, dt * 0.5f, 420.0f, z);
        if (!Settled(&L.fs[i], &L.fsv[i], tg, 0.0008f, 0.01f)) moving = TRUE;
    }
    return moving;
}

static void Relayout(void)
{
    L.h = Layout();
    Kick();
    Paint();
}

/* Nuevos resultados: los que siguen en su sitio conservan su estado; los que cambian
 * entran escalonados desde abajo. */
static void Rebuild_UI(void)
{
    static Res old[MAX_RES + RECENT_N + GRID_N];
    const int oldN = L.nres;
    CopyMemory(old, L.res, sizeof(Res) * oldN);
    if (L.qlen) BuildResults(); else BuildEmpty();
    int fresh = 0;
    for (int i = 0; i < L.nres; ++i) {
        Res *r = &L.res[i];
        if (i < oldN && old[i].kind == r->kind && old[i].tile == r->tile && !lstrcmpiW(old[i].target, r->target)) {
            r->ap = old[i].ap; r->av = old[i].av; r->delay = old[i].delay;
            r->is = old[i].is; r->isv = old[i].isv; r->iconUp = old[i].iconUp;
            r->hs = old[i].hs; r->hsv = old[i].hsv;
        } else {
            r->ap = r->av = 0; r->is = 1; r->isv = 0; r->iconUp = FALSE;
            r->hs = 1; r->hsv = 0;
            r->delay = 0.018f * fresh++;
        }
    }
    L.sel = L.nres ? 0 : -1;
    Relayout();
}

static void Close(void)
{
    if (L.hwnd) Pop_Close(&L.pop);
    Backdrop_Close();
}

static void UrlEncode(const WCHAR *s, WCHAR *out, int cap)
{
    char u8[1024];
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, u8, sizeof(u8), NULL, NULL);
    int o = 0;
    for (int i = 0; i < n - 1 && o < cap - 4; ++i) {
        const unsigned char ch = (unsigned char)u8[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~')
            out[o++] = ch;
        else if (ch == ' ') out[o++] = L'+';
        else { static const char hx[] = "0123456789ABCDEF"; out[o++] = L'%'; out[o++] = hx[ch >> 4]; out[o++] = hx[ch & 15]; }
    }
    out[o] = 0;
}

static void Run(int i, BOOL locate)
{
    if (i < 0 || i >= L.nres) return;
    const Res r = L.res[i];
    AllowSetForegroundWindow(ASFW_ANY);
    Close();
    WCHAR buf[MAX_PATH + 3200];
    switch (r.kind) {
    case RK_APP:
        wsprintfW(buf, L"shell:AppsFolder\\%.300s", r.target);
        if (App_ShellOpen(buf)) { wsprintfW(buf, L"app:%.300s", r.target); MruAdd(buf); }
        break;
    case RK_FILE: case RK_DIR:
        if (locate) App_ShellSelect(r.target);
        else if (App_ShellOpen(r.target)) { wsprintfW(buf, L"file:%.270s", r.target); MruAdd(buf); }
        break;
    case RK_WEB: {
        WCHAR enc[3000];
        UrlEncode(r.target, enc, 3000);
        wsprintfW(buf, L"https://www.google.com/search?q=%s", enc);
        App_ShellOpen(buf);
        break;
    }
    }
}

/* ───────────────────────── Clic derecho ─────────────────────────
 * Las opciones del menú de Windows, hechas aquí con el menú propio: el de verdad
 * (IContextMenu) cargaría extensiones de shell de terceros dentro del proceso, y las
 * mitigaciones de OpenDock solo dejan entrar binarios firmados por Microsoft. */
enum { LM_OPEN = 1, LM_ADMIN, LM_FOLDER, LM_PIN, LM_UNINSTALL, LM_COPY, LM_OPENWITH, LM_TERMINAL };

/* Ruta absoluta sin comillas ni caracteres de control (como App_ShellOpen). */
static BOOL SafePath(const WCHAR *p)
{
    const int n = lstrlenW(p);
    if (n < 3 || n >= MAX_PATH || p[1] != L':' || p[2] != L'\\') return FALSE;
    for (int i = 0; i < n; ++i) if (p[i] == L'"' || p[i] < 0x20 || p[i] == 0x7F) return FALSE;
    return TRUE;
}

/* Ruta real de una app de escritorio: su id es la ruta o empieza por una carpeta conocida
 * ({GUID}\...). Las empaquetadas (AppUserModelID, sin '\') no tienen. */
static BOOL AppPath(const WCHAR *id, WCHAR *out)
{
    out[0] = 0;
    if (!wcschr(id, L'\\')) return FALSE;
    if (id[0] == L'{') {
        const WCHAR *end = wcschr(id, L'}');
        if (!end || end[1] != L'\\' || end - id > 38) return FALSE;
        WCHAR g[40];
        lstrcpynW(g, id, (int)(end - id) + 2);
        GUID kf;
        PWSTR base = NULL;
        if (FAILED(CLSIDFromString(g, &kf)) || FAILED(SHGetKnownFolderPath(&kf, 0, NULL, &base))) return FALSE;
        const BOOL fits = lstrlenW(base) + lstrlenW(end + 1) < MAX_PATH;
        if (fits) { lstrcpyW(out, base); lstrcatW(out, end + 1); }
        CoTaskMemFree(base);
        if (!fits) return FALSE;
    } else lstrcpynW(out, id, MAX_PATH);
    return SafePath(out) && GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

static BOOL ExtIs(const WCHAR *path, const WCHAR *const *exts, int n)
{
    const WCHAR *dot = wcsrchr(path, L'.'), *slash = wcsrchr(path, L'\\');
    if (!dot || (slash && dot < slash)) return FALSE;
    for (int i = 0; i < n; ++i) if (!lstrcmpiW(dot, exts[i])) return TRUE;
    return FALSE;
}

static void RunAdmin(const WCHAR *path)
{
    if (!SafePath(path)) return;
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
}

static void CopyText(const WCHAR *t)
{
    const SIZE_T sz = (SIZE_T)(lstrlenW(t) + 1) * sizeof(WCHAR);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sz);
    if (!g) return;
    void *p = GlobalLock(g);
    if (p) { CopyMemory(p, t, sz); GlobalUnlock(g); }
    if (p && OpenClipboard(L.hwnd)) {
        EmptyClipboard();
        if (SetClipboardData(CF_UNICODETEXT, g)) g = NULL;      /* ahora es del portapapeles */
        CloseClipboard();
    }
    if (g) GlobalFree(g);
}

/* Terminal en la carpeta: la ruta va como directorio de trabajo, nunca en la línea de
 * órdenes; los ejecutables, por ruta completa (no se busca en el directorio actual). */
static void OpenTerminal(const WCHAR *dir)
{
    if (!SafePath(dir)) return;
    WCHAR exe[MAX_PATH];
    PWSTR la = NULL;
    exe[0] = 0;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &la))) {
        if (lstrlenW(la) < MAX_PATH - 40) wsprintfW(exe, L"%s\\Microsoft\\WindowsApps\\wt.exe", la);
        CoTaskMemFree(la);
    }
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpDirectory = dir;
    sei.nShow = SW_SHOWNORMAL;
    if (exe[0] && GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES) {
        sei.lpFile = exe;
        sei.lpParameters = L"-d .";
        if (ShellExecuteExW(&sei)) return;
    }
    const UINT n = GetSystemDirectoryW(exe, MAX_PATH);
    if (!n || n > MAX_PATH - 48) return;
    lstrcatW(exe, L"\\WindowsPowerShell\\v1.0\\powershell.exe");
    sei.lpFile = exe;
    sei.lpParameters = NULL;
    ShellExecuteExW(&sei);
}

static void ContextMenu(int i, POINT at)
{
    if (i < 0 || i >= L.nres || L.menu) return;
    const Res r = L.res[i];
    static const WCHAR *const kRunnable[] = { L".exe", L".bat", L".cmd", L".msi" };
    static const WCHAR *const kPinnable[] = { L".exe", L".lnk" };
    WCHAR path[MAX_PATH], pin[MAX_PATH + 8];
    path[0] = pin[0] = 0;
    BOOL admin = FALSE;
    MenuItem m[12];
    int n = 0;
    #define ADD(id_, text_, glyph_, flags_) do { if (n < 12) { m[n].id = (id_); m[n].text = (text_); m[n].glyph = (glyph_); m[n].flags = (flags_); ++n; } } while (0)

    switch (r.kind) {
    case RK_APP:
        if (AppPath(r.target, path)) { lstrcpynW(pin, path, MAX_PATH); admin = TRUE; }
        else if (!wcschr(r.target, L'\\')) wsprintfW(pin, L"aumid:%.250s", r.target);
        ADD(LM_OPEN, L"Abrir", 0xE8A7, 0);
        if (admin) ADD(LM_ADMIN, L"Ejecutar como administrador", 0xE7EF, 0);
        if (path[0]) ADD(LM_FOLDER, L"Abrir ubicaci\x00F3n del archivo", 0xE838, 0);
        break;
    case RK_FILE:
        if (!SafePath(r.target)) return;
        lstrcpynW(path, r.target, MAX_PATH);
        if (ExtIs(path, kPinnable, 2)) lstrcpynW(pin, path, MAX_PATH);
        admin = ExtIs(path, kRunnable, 4);
        ADD(LM_OPEN, L"Abrir", 0xE8E5, 0);
        ADD(LM_OPENWITH, L"Abrir con\x2026", 0xE7AC, 0);
        if (admin) ADD(LM_ADMIN, L"Ejecutar como administrador", 0xE7EF, 0);
        ADD(LM_FOLDER, L"Abrir ubicaci\x00F3n del archivo", 0xE838, 0);
        break;
    case RK_DIR:
        if (!SafePath(r.target)) return;
        lstrcpynW(path, r.target, MAX_PATH);
        ADD(LM_OPEN, L"Abrir", 0xE838, 0);
        ADD(LM_TERMINAL, L"Abrir en Terminal", 0xE756, 0);
        ADD(LM_FOLDER, L"Abrir ubicaci\x00F3n de la carpeta", 0xE8DA, 0);
        break;
    default:
        ADD(LM_OPEN, L"Buscar en la web", 0xE774, 0);
        break;
    }
    if (pin[0] && g_cfg.dock) {
        const BOOL pinned = Dock_IsPinned(pin);
        ADD(LM_PIN, pinned ? L"Quitar del dock" : L"Anclar al dock", pinned ? 0xE77A : 0xE718, 0);
    }
    if (path[0]) ADD(LM_COPY, L"Copiar ruta", 0xE8C8, 0);
    if (r.kind == RK_APP) {
        ADD(0, NULL, 0, MI_SEPARATOR);
        ADD(LM_UNINSTALL, L"Desinstalar\x2026", 0xE74D, 0);
    }
    #undef ADD

    L.menu = TRUE;
    const int cmd = Menu_Track(m, n, at);
    L.menu = FALSE;
    if (!L.hwnd) return;
    if (i >= L.nres || lstrcmpiW(L.res[i].target, r.target)) return;   /* la lista cambió mientras tanto */
    switch (cmd) {
    case LM_OPEN:      Run(i, FALSE); return;
    case LM_FOLDER:    AllowSetForegroundWindow(ASFW_ANY); Close(); App_ShellSelect(path); return;
    case LM_ADMIN:     Close(); RunAdmin(path); return;
    case LM_TERMINAL:  AllowSetForegroundWindow(ASFW_ANY); Close(); OpenTerminal(path); return;
    case LM_UNINSTALL: AllowSetForegroundWindow(ASFW_ANY); Close(); App_ShellOpen(L"ms-settings:appsfeatures"); return;
    case LM_COPY:      CopyText(path); Close(); return;
    case LM_OPENWITH: {
        Close();
        OPENASINFO oi = { path, NULL, OAIF_ALLOW_REGISTRATION | OAIF_REGISTER_EXT | OAIF_EXEC };
        SHOpenWithDialog(NULL, &oi);
        return;
    }
    case LM_PIN:       Dock_TogglePin(pin); break;     /* el buscador sigue abierto */
    }
    /* sin elegir nada, o tras anclar: si el clic fue a otra app, fuera; si no, vuelve el foco */
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (fg && pid != GetCurrentProcessId()) Close();
    else { SetForegroundWindow(L.hwnd); SetFocus(L.hwnd); }
}

/* Con el teclado (tecla de menú o Mayús+F10): sobre la fila seleccionada. */
static void KeyMenu(void)
{
    if (L.sel < 0 || L.sel >= L.nres) return;
    RECT wr;
    GetWindowRect(L.hwnd, &wr);
    const RECT *rc = &L.res[L.sel].rc;
    POINT at = { wr.left + L.pop.margin + (rc->left + rc->right) / 2, wr.top + L.pop.mtop + rc->top };
    ContextMenu(L.sel, at);
}

/* ── edición del texto ── */
static void Edit(int del0, int del1, const WCHAR *ins, int nins)
{
    if (L.selAll) { del0 = 0; del1 = L.qlen; L.selAll = FALSE; }
    del0 = max(0, min(L.qlen, del0)); del1 = max(del0, min(L.qlen, del1));
    nins = min(nins, 255 - (L.qlen - (del1 - del0)));
    if (nins < 0) nins = 0;
    MoveMemory(L.q + del0 + nins, L.q + del1, (L.qlen - del1 + 1) * sizeof(WCHAR));
    if (nins) CopyMemory(L.q + del0, ins, nins * sizeof(WCHAR));
    L.qlen = L.qlen - (del1 - del0) + nins;
    L.caret = del0 + nins;
    L.caretOn = TRUE;
    SetTimer(L.hwnd, TIMER_CARET, 530, NULL);
    Rebuild_UI();
}

static int WordLeft(int at)
{
    while (at > 0 && L.q[at - 1] == L' ') --at;
    while (at > 0 && L.q[at - 1] != L' ') --at;
    return at;
}

static void Paste(void)
{
    if (!OpenClipboard(L.hwnd)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    const WCHAR *s = h ? (const WCHAR *)GlobalLock(h) : NULL;
    if (s) {
        WCHAR t[256];
        int n = 0;
        for (; s[n] && n < 255 && s[n] != L'\r' && s[n] != L'\n'; ++n) t[n] = s[n] == L'\t' ? L' ' : s[n];
        GlobalUnlock(h);
        CloseClipboard();
        Edit(L.caret, L.caret, t, n);
        return;
    }
    CloseClipboard();
}

static void MoveSel(int dx, int dy)
{
    if (!L.nres) return;
    if (L.sel < 0) { L.sel = 0; Kick(); return; }
    int s = L.sel;
    if (L.sel < L.ngrid) {                          /* en la fila de apps */
        if (dx) s = max(0, min(L.ngrid - 1, s + dx));
        else if (dy > 0) s = L.nres > L.ngrid ? L.ngrid : s;
    } else {
        if (dy < 0) s = s - 1 >= L.ngrid ? s - 1 : (L.ngrid ? 0 : s);
        else if (dy > 0) s = min(L.nres - 1, s + 1);
    }
    if (s != L.sel) { L.sel = s; Kick(); }
}

static int HitRes(int x, int y)
{
    POINT p = { x, y };
    for (int i = 0; i < L.nres; ++i) if (PtInRect(&L.res[i].rc, p)) return i;
    return -1;
}

static LRESULT CALLBACK LnProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEWHEEL && m != WM_MOUSEHWHEEL) l = Pop_Mouse(&L.pop, l);
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        Paint();
        return 0;
    }
    case WM_NCHITTEST: return Pop_NcHit(&L.pop, l);
    case WM_MOUSEACTIVATE: return MA_ACTIVATE;
    case WM_POPFRAME:
        Pop_Step(&L.pop);
        if (!L.hwnd) return 0;
        if (L.anim) {
            const BOOL moving = Advance();
            Paint();
            if (!moving) { L.anim = FALSE; Pop_Hold(&L.pop, FALSE); }
        } else if (L.cv.px) Pop_Present(&L.pop, &L.cv);
        return 0;
    case WM_LN_BACKDROP: {
        BdJob *j = (BdJob *)l;
        if (!j) { if (!L.pop.closing) Backdrop_Open(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), h); return 0; }
        if (!L.pop.closing) Backdrop_Show(j, h);      /* llegó la imagen del otro hilo */
        else { Canvas_Free(&j->full); HeapFree(GetProcessHeap(), 0, j); }
        return 0;
    }
    case WM_LN_ICON: {                              /* llegaron iconos: un repintado por ráfaga */
        MSG more;
        while (PeekMessageW(&more, h, WM_LN_ICON, WM_LN_ICON, PM_REMOVE)) { }
        Paint();
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && !L.menu) Close();
        return 0;
    case WM_TIMER:
        if (w == TIMER_CARET) { L.caretOn = !L.caretOn; Paint(); }
        else if (w == TIMER_FOCUS) {
            /* si no llegó a tener el foco (Windows lo negó) o lo perdió sin avisar, fuera */
            if (!L.menu && GetForegroundWindow() != h && GetTickCount() - L.openedAt > 600) Close();
        }
        return 0;
    case WM_KEYDOWN: {
        const BOOL ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        switch (w) {
        case VK_ESCAPE:
            if (L.qlen) { L.selAll = FALSE; Edit(0, L.qlen, NULL, 0); }
            else Close();
            return 0;
        case VK_RETURN: Run(L.sel, ctrl || shift); return 0;
        case VK_APPS:   KeyMenu(); return 0;            /* tecla de menú contextual */
        case VK_DOWN:   MoveSel(0, 1); return 0;
        case VK_UP:     MoveSel(0, -1); return 0;
        case VK_TAB:    MoveSel(0, shift ? -1 : 1); return 0;
        case VK_LEFT: case VK_RIGHT:
            if (!L.qlen && L.sel >= 0 && L.sel < L.ngrid) { MoveSel(w == VK_LEFT ? -1 : 1, 0); return 0; }
            L.selAll = FALSE;
            if (w == VK_LEFT) L.caret = ctrl ? WordLeft(L.caret) : max(0, L.caret - 1);
            else {
                if (ctrl) { while (L.caret < L.qlen && L.q[L.caret] != L' ') ++L.caret; while (L.caret < L.qlen && L.q[L.caret] == L' ') ++L.caret; }
                else L.caret = min(L.qlen, L.caret + 1);
            }
            L.caretOn = TRUE;
            Paint();
            return 0;
        case VK_HOME: L.selAll = FALSE; L.caret = 0; L.caretOn = TRUE; Paint(); return 0;
        case VK_END:  L.selAll = FALSE; L.caret = L.qlen; L.caretOn = TRUE; Paint(); return 0;
        case VK_DELETE:
            if (L.selAll) Edit(0, L.qlen, NULL, 0);
            else if (L.caret < L.qlen) Edit(L.caret, L.caret + 1, NULL, 0);
            return 0;
        }
        return 0;
    }
    case WM_SYSKEYDOWN:
        if (w == VK_F10 && GetKeyState(VK_SHIFT) < 0) { KeyMenu(); return 0; }    /* Mayús+F10 */
        break;
    case WM_CONTEXTMENU:
        if (l == -1) KeyMenu();
        return 0;
    case WM_RBUTTONUP: {
        const int hit = HitRes((short)LOWORD(l), (short)HIWORD(l));
        if (hit < 0) return 0;
        if (hit != L.sel) { L.sel = hit; Kick(); }
        POINT at;
        GetCursorPos(&at);
        ContextMenu(hit, at);
        return 0;
    }
    case WM_CHAR: {
        const WCHAR ch = (WCHAR)w;
        if (ch == 1) { if (L.qlen) { L.selAll = TRUE; Paint(); } return 0; }      /* Ctrl+A */
        if (ch == 22) { Paste(); return 0; }                                       /* Ctrl+V */
        if (ch == 8) {                                                             /* Retroceso */
            if (L.selAll) Edit(0, L.qlen, NULL, 0);
            else if (L.caret > 0) Edit(L.caret - 1, L.caret, NULL, 0);
            return 0;
        }
        if (ch == 127) { if (L.selAll) Edit(0, L.qlen, NULL, 0); else Edit(WordLeft(L.caret), L.caret, NULL, 0); return 0; }  /* Ctrl+Retroceso */
        if (ch >= 0x20 && ch != 0x7F) Edit(L.caret, L.caret, &ch, 1);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!L.tracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            L.tracking = TrackMouseEvent(&tme);
        }
        const int fh = HitFoot((short)LOWORD(l), (short)HIWORD(l));
        if (fh != L.fhot) { L.fhot = fh; Kick(); }
        const int hit = HitRes((short)LOWORD(l), (short)HIWORD(l));
        if (hit >= 0 && hit != L.sel) { L.sel = hit; Kick(); }
        return 0;
    }
    case WM_MOUSELEAVE:
        L.tracking = FALSE;
        L.fhot = L.fpress = -1;
        Kick();
        return 0;
    case WM_LBUTTONDOWN:
        L.fpress = HitFoot((short)LOWORD(l), (short)HIWORD(l));
        if (L.fpress >= 0) Kick();
        return 0;
    case WM_LBUTTONUP: {
        const int fh = HitFoot((short)LOWORD(l), (short)HIWORD(l)), was = L.fpress;
        L.fpress = -1;
        if (fh >= 0 || was >= 0) { Kick(); if (fh >= 0 && fh == was) FootAction(fh); return 0; }
        const int hit = HitRes((short)LOWORD(l), (short)HIWORD(l));
        if (hit >= 0) Run(hit, GetKeyState(VK_CONTROL) < 0);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(h, TIMER_CARET);
        KillTimer(h, TIMER_FOCUS);
        s_iconHwnd = NULL;
        Pop_Destroyed(&L.pop);
        Pop_FreeGlass(&L.glass);
        Canvas_Free(&L.cv);
        Canvas_Free(&L.bg);
        FreeGlyphs();
        FreeFonts();
        L.hwnd = NULL;
        L.closedAt = GetTickCount();
        Backdrop_Close();
        {   /* una imagen del fondo que llegó tarde: se libera */
            MSG m2;
            while (PeekMessageW(&m2, h, WM_LN_BACKDROP, WM_LN_BACKDROP, PM_REMOVE))
                if (m2.lParam) { BdJob *j = (BdJob *)m2.lParam; Canvas_Free(&j->full); HeapFree(GetProcessHeap(), 0, j); }
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* Windows sólo deja pasar al frente a quien recibió la última entrada, y la tecla la
 * recibió la app de delante. Un evento de ratón vacío (sin moverlo ni pulsar) hace que la
 * última entrada sea nuestra, como hace PowerToys; si aun así lo niega, se pide prestada
 * la cola de entrada de la ventana de delante un momento. */
static void TakeFocus(HWND h)
{
    INPUT nudge;
    ZeroMemory(&nudge, sizeof(nudge));
    nudge.type = INPUT_MOUSE;
    SendInput(1, &nudge, sizeof(nudge));
    SetForegroundWindow(h);
    if (GetForegroundWindow() == h) return;
    HWND fg = GetForegroundWindow();
    const DWORD ft = fg ? GetWindowThreadProcessId(fg, NULL) : 0, me = GetCurrentThreadId();
    if (ft && ft != me && AttachThreadInput(me, ft, TRUE)) {
        BringWindowToTop(h);
        SetForegroundWindow(h);
        SetFocus(h);
        AttachThreadInput(me, ft, FALSE);
    }
}

/* ── Fondo del buscador ──
 * Como el vistazo del dock: la pantalla, tomada justo antes de abrir, desenfocada y
 * oscurecida, entra con un fundido por debajo del buscador y se va al cerrarlo. Los clics
 * la atraviesan (hacen perder el foco al buscador, que se cierra). 1 px menos de alto: una
 * ventana sin marco que cubre el monitor entero es "pantalla completa" para Windows. */
#define BD_CLASS L"OpenDock.SearchBackdrop"
static struct { HWND hwnd; Canvas sm, full; float a, target; DWORD last; } BD;

/* Un paso del fundido. Lo da cada fotograma del buscador (mientras anima, la cola nunca
 * queda vacía y WM_TIMER no llegaría) y, si no, el temporizador del propio fondo. */
static void Backdrop_Step(void)
{
    if (!BD.hwnd || BD.a == BD.target) return;
    const DWORD now = GetTickCount();
    const float dt = min(0.05f, (now - BD.last) / 1000.0f);
    if (dt <= 0.0f) return;
    BD.last = now;
    BD.a += (BD.target - BD.a) * min(1.0f, dt * (BD.target > BD.a ? 13.0f : 18.0f));
    if (fabsf(BD.a - BD.target) < 0.01f) BD.a = BD.target;
    SetLayeredWindowAttributes(BD.hwnd, 0, (BYTE)(255.0f * BD.a + 0.5f), LWA_ALPHA);
    if (BD.a == BD.target) {
        KillTimer(BD.hwnd, 1);
        if (BD.target == 0) DestroyWindow(BD.hwnd);
    }
}

static LRESULT CALLBACK BackdropProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND:    return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (BD.full.dc) BitBlt(dc, 0, 0, BD.full.w, BD.full.h, BD.full.dc, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        Backdrop_Step();
        return 0;
    case WM_DESTROY:
        if (h == BD.hwnd) BD.hwnd = NULL;
        Canvas_Free(&BD.sm);
        Canvas_Free(&BD.full);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* La imagen del fondo se prepara en otro hilo (captura, desenfoque y escalado llevan su
 * rato en monitores grandes): la interfaz no se para ni un fotograma. Al terminar se avisa
 * al buscador con WM_LN_BACKDROP y la imagen en lParam. */
static DWORD WINAPI BackdropWork(LPVOID p)
{
    BdJob *j = (BdJob *)p;
    const RECT r = j->r;
    const int w = r.right - r.left, h = r.bottom - r.top, sw = max(1, w / 4), sh = max(1, h / 4);
    Canvas sm = { 0 };
    BOOL ok = Canvas_Init(&sm, sw, sh);
    if (ok) {
        HDC screen = GetDC(NULL);
        SetStretchBltMode(sm.dc, COLORONCOLOR);      /* rápida: el desenfoque la suaviza */
        StretchBlt(sm.dc, 0, 0, sw, sh, screen, r.left, r.top, w, h, SRCCOPY);
        ReleaseDC(NULL, screen);
        GdiFlush();
        for (int i = 0; i < 2; ++i) Gfx_BoxBlur(sm.px, sw, sh, j->blur);
        for (int i = 0; i < sw * sh; ++i) {             /* menos color y más oscuro, como el vistazo */
            const DWORD v = sm.px[i];
            const float rr = (float)((v >> 16) & 255), gg = (float)((v >> 8) & 255), bb = (float)(v & 255);
            const float y = rr * 0.30f + gg * 0.59f + bb * 0.11f;
            sm.px[i] = (DWORD)((y + (rr - y) * 0.45f) * 0.42f) << 16 | (DWORD)((y + (gg - y) * 0.45f) * 0.42f) << 8
                     | (DWORD)((y + (bb - y) * 0.45f) * 0.42f);
        }
        /* a tamaño completo una sola vez (bilineal); al pintar solo se copia */
        ok = Canvas_Init(&j->full, w, h - 1);
        if (ok) { Gfx_BlitScaled(&j->full, &sm, w * 0.5f, h * 0.5f, (float)w / sw); GdiFlush(); }
        Canvas_Free(&sm);
    }
    if (!ok || !PostMessageW(j->notify, WM_LN_BACKDROP, 0, (LPARAM)j)) {
        Canvas_Free(&j->full);
        HeapFree(GetProcessHeap(), 0, j);
    }
    return 0;
}

/* Pide la imagen del fondo para el monitor hm (la recibe Backdrop_Show). */
static void Backdrop_Open(HMONITOR hm, HWND above)
{
    if (BD.hwnd) DestroyWindow(BD.hwnd);
    MONITORINFO mi = { sizeof(mi) };
    if (!above || !GetMonitorInfoW(hm, &mi)) return;
    BdJob *j = (BdJob *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(BdJob));
    if (!j) return;
    j->notify = above;
    j->r = mi.rcMonitor;
    j->blur = max(1, SS(14) / 4);
    HANDLE t = CreateThread(NULL, 0, BackdropWork, j, 0, NULL);
    if (t) CloseHandle(t);
    else HeapFree(GetProcessHeap(), 0, j);
}

/* La imagen está lista (en el hilo de la interfaz): su ventana, justo por debajo del buscador. */
static void Backdrop_Show(BdJob *j, HWND above)
{
    const RECT r = j->r;
    if (BD.hwnd) DestroyWindow(BD.hwnd);
    BD.full = j->full;                      /* la imagen pasa a ser del fondo */
    HeapFree(GetProcessHeap(), 0, j);
    BD.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                              BD_CLASS, L"", WS_POPUP, r.left, r.top, BD.full.w, BD.full.h, NULL, NULL, g_inst, NULL);
    if (!BD.hwnd) { Canvas_Free(&BD.full); return; }
    if (App_HideFromCapture(FALSE)) SetWindowDisplayAffinity(BD.hwnd, WDA_EXCLUDEFROMCAPTURE);
    BD.a = 0;
    BD.target = 1;
    BD.last = GetTickCount();
    SetLayeredWindowAttributes(BD.hwnd, 0, 0, LWA_ALPHA);
    SetWindowPos(BD.hwnd, above, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateWindow(BD.hwnd);
    SetTimer(BD.hwnd, 1, 10, NULL);
}

static void Backdrop_Close(void)
{
    if (!BD.hwnd || BD.target == 0) return;
    BD.target = 0;
    BD.last = GetTickCount();
    SetTimer(BD.hwnd, 1, 10, NULL);
}

static void Open(void)
{
    /* en el monitor de la ventana activa, como el Inicio de Windows (si no hay, el principal) */
    POINT pt = { 0, 0 };
    HWND fg = GetForegroundWindow();
    HMONITOR hm = fg ? MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hm, &mi);
    L.mon = mi.rcWork;
    L.dpi = MonitorDpi(hm);
    MakeFonts();

    Theme th;
    Theme_Load(&th);
    L.light = g_cfg.material == MAT_SYSTEM && !th.dark;
    if (L.light) { L.fg = 0x1C1C1E; L.fg2 = 0x55555A; L.fg3 = 0x7C7C82; L.line = 0xD1D1D6; }
    else         { L.fg = 0xFFFFFF; L.fg2 = 0xB8B8BD; L.fg3 = 0x8E8E93; L.line = 0x2C2C2E; }
    L.accent = g_cfg.accent ? kAccentPresets[g_cfg.accent] : th.accentOnBlack;
    UserInfo();
    MakeGlyphs();
    L.fhot = L.fpress = -1;
    L.tracking = FALSE;
    for (int i = 0; i < FB_COUNT; ++i) { L.fs[i] = 1.0f; L.fsv[i] = 0; }

    L.w = min(SS(LW), (int)(mi.rcWork.right - mi.rcWork.left) - SS(32));
    L.maxH = SS(QH) + SS(6) + MAX_RES * SS(ROWH) + SS(8);
    L.maxH = max(L.maxH, SS(QH) + SS(6) + 2 * SS(LABELH) + SS(TILEH) + SS(4) + RECENT_N * SS(FROWH) + SS(8));
    L.maxH += SS(FOOTH);
    L.ox = (mi.rcWork.left + mi.rcWork.right - L.w) / 2;
    L.oy = mi.rcWork.top + (int)((mi.rcWork.bottom - mi.rcWork.top) * 0.18f);

    /* fondo: el vidrio se captura antes de que aparezca la ventana, y se tiñe una vez */
    Canvas_Free(&L.bg);
    if (!Canvas_Init(&L.bg, L.w, L.maxH)) return;
    if (L.light)                              Canvas_Clear(&L.bg, 0xECECF0);
    else if (g_cfg.material == MAT_GLASS) {
        Pop_CaptureGlass(&L.glass, L.ox, L.oy, L.w, L.maxH, SS(28));
        Pop_PaintGlass(&L.bg, &L.glass, L.ox, L.oy, 0x1C1C1E, 0.62f);
    } else                                    Canvas_Clear(&L.bg, g_cfg.material == MAT_OLED ? 0x000000 : 0x161618);
    {   /* su color medio: lo que entra se funde desde él */
        GdiFlush();
        unsigned long long acc[3] = { 0, 0, 0 };
        int n = 0;
        for (int i = 0; i < L.bg.w * L.bg.h; i += 37, ++n)
            for (int ch = 0; ch < 3; ++ch) acc[ch] += (L.bg.px[i] >> (ch * 8)) & 255;
        L.bgBase = n ? (DWORD)(acc[2] / n) << 16 | (DWORD)(acc[1] / n) << 8 | (DWORD)(acc[0] / n) : 0;
    }

    L.q[0] = 0; L.qlen = L.caret = 0; L.selAll = FALSE; L.caretOn = TRUE;
    L.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, LN_CLASS, L"Buscar", WS_POPUP,
                             L.ox, L.oy, L.w, SS(QH), NULL, NULL, g_inst, NULL);
    if (!L.hwnd) return;
    if (App_HideFromCapture(FALSE)) SetWindowDisplayAffinity(L.hwnd, WDA_EXCLUDEFROMCAPTURE);
    s_iconHwnd = L.hwnd;
    Pop_Open(&L.pop, L.hwnd, SS(30), (float)SS(18), 0.5f, 0.0f);
    L.nres = 0;
    BuildEmpty();
    /* nace como la barra de búsqueda sola y se despliega; las apps entran una tras otra */
    for (int i = 0; i < L.nres; ++i) {
        Res *r = &L.res[i];
        r->ap = r->av = 0; r->is = 1; r->isv = 0; r->iconUp = FALSE;
        r->hs = 1; r->hsv = 0;
        r->delay = i < L.ngrid ? 0.06f + 0.032f * i : 0.10f + 0.03f * (i - L.ngrid);
    }
    L.sel = L.nres ? 0 : -1;
    L.anim = FALSE;
    L.h = Layout();
    L.hcur = (float)SS(QH) + 2; L.hv = 0;
    Pop_SetBounds(&L.pop, L.ox, L.oy, L.w, (int)L.hcur);
    Kick();
    Paint();
    ShowWindow(L.hwnd, SW_SHOW);
    TakeFocus(L.hwnd);
    PostMessageW(L.hwnd, WM_LN_BACKDROP, 0, 0);     /* el fondo, después: el buscador sale ya */
    L.openedAt = GetTickCount();
    SetTimer(L.hwnd, TIMER_CARET, 530, NULL);
    SetTimer(L.hwnd, TIMER_FOCUS, 250, NULL);
    RefreshApps(FALSE);
}

void Launcher_Toggle(void)
{
    if (L.hwnd) { if (!L.pop.closing) Close(); return; }
    if (!g_cfg.launcher) return;
    Open();
}

/* ───────────────────────── Tecla Windows ───────────────────────── */
static HANDLE s_hookThread;
static DWORD  s_hookTid;
static BOOL   s_winDown, s_winOther, s_winSkip, s_winEaten;   /* eaten: nos quedamos un atajo (Win+N) */

/* ¿La ventana de delante es de un proceso elevado? Entonces no se intercepta: la entrada
 * que reinyectamos no le llegaría (UIPI) y la tecla Windows se quedaría pulsada. */
static BOOL ForegroundElevated(void)
{
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!fg || !GetWindowThreadProcessId(fg, &pid) || !pid || pid == GetCurrentProcessId()) return FALSE;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return TRUE;
    HANDLE tok = NULL;
    BOOL elevated = TRUE;
    if (OpenProcessToken(p, TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION te;
        DWORD sz = 0;
        if (GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &sz)) elevated = te.TokenIsElevated != 0;
        CloseHandle(tok);
    }
    CloseHandle(p);
    return elevated;
}

/* Marca de lo que reinyecta OpenDock: así se distingue de lo que llega de un escritorio
 * remoto, que también es "inyectado" (Chrome Remote Desktop teclea con SendInput). */
#define OD_INJECTED ((ULONG_PTR)0x0D0C4B45)

#define WM_KB_MASK (WM_APP + 1)
static BOOL s_maskDue;          /* la tecla sin asignar aún no se mandó en esta pulsación */

static void SendMask(void)
{
    s_maskDue = FALSE;
    INPUT in[2];
    ZeroMemory(in, sizeof(in));
    for (int i = 0; i < 2; ++i) { in[i].type = INPUT_KEYBOARD; in[i].ki.wVk = VK_MASK; in[i].ki.dwExtraInfo = OD_INJECTED; }
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

static LRESULT CALLBACK KbProc(int code, WPARAM w, LPARAM l)
{
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)l;
        /* lo inyectado no cuenta (otras apps, lo nuestro), salvo en una sesión remota */
        if (!(k->flags & LLKHF_INJECTED) || (App_RemoteView() && k->dwExtraInfo != OD_INJECTED)) {
            const BOOL down = w == WM_KEYDOWN || w == WM_SYSKEYDOWN;
            if (k->vkCode == VK_LWIN || k->vkCode == VK_RWIN) {
                /* La tecla Windows siempre llega tal cual, al bajar y al subir: si se tragara y
                 * se reinyectara, cualquier fallo de la reinyección la dejaría pulsada para el
                 * sistema (y Ctrl, N... harían atajos de Windows). Para que Inicio no se abra
                 * basta una tecla sin asignar entre bajarla y soltarla: Windows ya no la ve sola.
                 * Se manda al salir del gancho (WM_KB_MASK): mandada dentro, Windows la colocaba
                 * antes que la propia tecla Windows y no servía de nada. */
                if (down) {
                    if (!s_winDown) {
                        s_winDown = TRUE;
                        s_winOther = s_winEaten = FALSE;
                        s_winSkip = ForegroundElevated();
                        s_maskDue = !s_winSkip;
                        if (s_maskDue) PostThreadMessageW(GetCurrentThreadId(), WM_KB_MASK, 0, 0);
                    }
                } else if (s_winDown) {
                    s_winDown = FALSE;
                    if (s_maskDue) SendMask();      /* soltada antes de llegar el aviso: va justo antes */
                    if (!s_winOther && !s_winSkip) PostMessageW(g_ctrl, WM_LAUNCHER, 0, 0);
                }
            } else if (s_winDown && k->vkCode == 'N' && !s_winSkip) {
                /* Win+N: nuestro centro de notificaciones (el de Windows no debe abrirse) */
                if (down && !s_winEaten) PostMessageW(g_ctrl, WM_LAUNCHER, 1, 0);
                if (down) s_winOther = s_winEaten = TRUE;
                return 1;
            } else if (down && s_winDown) s_winOther = TRUE;
        }
    }
    return CallNextHookEx(NULL, code, w, l);
}

static DWORD WINAPI HookThread(LPVOID u)
{
    (void)u;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HHOOK hk = SetWindowsHookExW(WH_KEYBOARD_LL, KbProc, g_inst, 0);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0)
        if (m.message == WM_KB_MASK && s_maskDue && s_winDown) SendMask();
    if (hk) UnhookWindowsHookEx(hk);
    return 0;
}

/* ───────────────────────── API ───────────────────────── */
void Launcher_Register(void)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = LnProc;
    wc.hInstance     = g_inst;
    wc.lpszClassName = LN_CLASS;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassExW(&wc);
    WNDCLASSEXW bd = { sizeof(bd) };
    bd.lpfnWndProc   = BackdropProc;
    bd.hInstance     = g_inst;
    bd.lpszClassName = BD_CLASS;
    RegisterClassExW(&bd);
}

static void StopHook(void)
{
    if (!s_hookThread) return;
    PostThreadMessageW(s_hookTid, WM_QUIT, 0, 0);
    WaitForSingleObject(s_hookThread, 1000);
    CloseHandle(s_hookThread);
    s_hookThread = NULL;
    s_hookTid = 0;
    s_winDown = FALSE;
}

/* Para los hilos de los índices y suelta su memoria (al apagar el buscador o al salir). */
static void StopIndex(void)
{
    if (s_stop) SetEvent(s_stop);
    HANDLE th[3];
    int n = 0;
    if (s_filesThread) th[n++] = s_filesThread;
    if (s_appsThread) th[n++] = s_appsThread;
    if (s_iconThread) th[n++] = s_iconThread;
    if (n) WaitForMultipleObjects(n, th, TRUE, 3000);
    if (s_iconThread) { CloseHandle(s_iconThread); s_iconThread = NULL; }
    if (s_iconEvt) {                    /* lo que quedó a medias se volverá a pedir */
        EnterCriticalSection(&s_iconLock);
        for (int i = 0; i < ICON_CAP; ++i) if (s_icons[i].state == IC_PENDING) s_icons[i].state = IC_EMPTY;
        s_iconQn = 0;
        LeaveCriticalSection(&s_iconLock);
    }
    if (s_filesThread) { CloseHandle(s_filesThread); s_filesThread = NULL; }
    if (s_appsThread) { CloseHandle(s_appsThread); s_appsThread = NULL; }
    InterlockedExchange(&s_appsBusy, 0);
    AcquireSRWLockExclusive(&s_flock);
    FIdx *old = s_fidx;
    s_fidx = NULL;
    ReleaseSRWLockExclusive(&s_flock);
    FFree(old);
    AcquireSRWLockExclusive(&s_alock);
    AppEnt *apps = s_apps;
    s_apps = NULL; s_napps = 0;
    ReleaseSRWLockExclusive(&s_alock);
    if (apps) HeapFree(GetProcessHeap(), 0, apps);
    if (s_stop) ResetEvent(s_stop);
}

void Launcher_Apply(void)
{
    if (!g_cfg.launcher) {
        StopHook();
        if (L.hwnd) DestroyWindow(L.hwnd);
        if (BD.hwnd) DestroyWindow(BD.hwnd);
        if (s_filesThread || s_apps) StopIndex();
        return;
    }
    if (!s_stop) s_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!s_stop) return;
    if (!s_hookThread) s_hookThread = CreateThread(NULL, 0, HookThread, NULL, 0, &s_hookTid);
    if (!s_filesThread) s_filesThread = CreateThread(NULL, 0, FilesThread, NULL, 0, NULL);
    if (!s_iconEvt) { InitializeCriticalSection(&s_iconLock); s_iconEvt = CreateEventW(NULL, FALSE, FALSE, NULL); }
    if (!s_iconThread && s_iconEvt) s_iconThread = CreateThread(NULL, 0, IconThread, NULL, 0, NULL);
    RefreshApps(FALSE);
}

void Launcher_Destroy(void)
{
    StopHook();
    if (L.hwnd) DestroyWindow(L.hwnd);
    if (BD.hwnd) DestroyWindow(BD.hwnd);
    StopIndex();
}
