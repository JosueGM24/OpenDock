/*
 * winnotif.c — notificaciones reales de Windows, mostradas en el notch.
 *
 * Windows guarda el centro de notificaciones del usuario en una base SQLite local:
 *   %LOCALAPPDATA%\Microsoft\Windows\Notifications\wpndatabase.db
 * La abrimos en SOLO LECTURA, vigilamos la carpeta para enterarnos de cambios y
 * extraemos título y texto del XML de cada toast. Nada sale del equipo.
 *
 * Versiones anteriores ocultaban los banners poniendo ShowBanner=0 por app; Windows
 * no relee ese valor en vivo, así que se abandonó. Wn_RestoreBanners deshace esos
 * cambios (solo los que hizo OpenDock) en equipos que los tengan.
 */
#define COBJMACROS
#include "app.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <sqlite3.h>
#include <propsys.h>
#include <wincodec.h>

#define NOTIF_SETTINGS L"Software\\Microsoft\\Windows\\CurrentVersion\\Notifications\\Settings"
#define HIDDEN_KEY     REG_KEY L"\\HiddenBanners"
#define MAX_APPS       64

typedef struct {
    wchar_t aumid[160];
    wchar_t name[64];
    HBITMAP icon[2];        /* dos tamaños: el normal y el de insignia (sobre una foto) */
    int     iconPx[2];      /* tamaño con el que se pidió cada uno; 0 = hueco libre */
    wchar_t siteIcon[MAX_PATH];     /* sitio web: su icono, el que registró el navegador */
    BOOL    siteTried;
} AppInfo;

static sqlite3  *s_db;
static WinNote   s_notes[WN_MAX];     /* lo que enseña el centro: base de Windows + historial */
static int       s_count;

/* Los navegadores (WhatsApp Web, Telegram…) usan la misma etiqueta para todo un chat y
 * Windows REEMPLAZA el aviso anterior por el nuevo: en su base sólo queda el último. Para
 * no perder los anteriores, guardamos aquí los que fueron reemplazados. Se van al
 * borrarlos, con "Borrar todo" o cuando el chat entero desaparece de Windows. */
#define KEY_CAP 64
typedef struct { wchar_t tag[KEY_CAP], group[KEY_CAP]; } NoteKey;
static WinNote   s_cur[WN_MAX];       /* filas de la base en la última lectura */
static NoteKey   s_curKey[WN_MAX];
static int       s_ncur;
static WinNote   s_hist[WN_MAX];      /* reemplazados */
static NoteKey   s_histKey[WN_MAX];
static int       s_nhist;
/* Ids propios del historial, lejos de los de Windows (no chocan ni con "Borrar todo"). */
static LONGLONG  s_histSeq = 0x4000000000000000LL;
static LONGLONG  s_lastSeen = -1;      /* -1 = primera carga: no avisar de lo antiguo */
static LONGLONG  s_cleared;            /* "Borrar todo": oculta ids <= este */
static LONGLONG  s_dismissed[64];
static int       s_ndismissed;
static AppInfo   s_apps[MAX_APPS];
static int       s_napps;
static WinNote   s_empty;

/* ───────────────────────── Información de la app ───────────────────────── */
static void FallbackName(const wchar_t *aumid, wchar_t *out, int cap)
{
    const wchar_t *s = aumid;
    for (const wchar_t *q = aumid; *q; ++q) if (*q == L'\\' || *q == L'.') s = q + 1;
    /* Paquetes: "Editor.App_hash!App" → "App" */
    const wchar_t *bang = aumid;
    while (*bang && *bang != L'!' && *bang != L'_') ++bang;
    if (*bang) {
        const wchar_t *dot = aumid;
        for (const wchar_t *q = aumid; q < bang; ++q) if (*q == L'.') dot = q + 1;
        lstrcpynW(out, dot, min(cap, (int)(bang - dot) + 1));
    } else {
        lstrcpynW(out, s, cap);
    }
    if (!out[0]) lstrcpynW(out, aumid, cap);
}

/* Los navegadores notifican por sitio: "…MicrosoftEdge.Stable_…!https://web.whatsapp.com/".
 * Para esas, el nombre es el del sitio recortado ("WhatsApp") y el icono, su inicial. */
static BOOL IsSiteNote(const wchar_t *aumid)
{
    return wcsstr(aumid, L"!http") || wcsstr(aumid, L"!chrome-extension://");
}
BOOL Wn_IsSite(LPCWSTR aumid) { return aumid && IsSiteNote(aumid); }

/* ms-appdata:///local/… de una app empaquetada (Edge lo es): su carpeta LocalState (o
 * RoamingState, TempState) dentro de %LOCALAPPDATA%\Packages\<familia del paquete>. */
static BOOL AppDataPath(const wchar_t *aumid, const wchar_t *uri, wchar_t *out)
{
    static const struct { LPCWSTR seg, dir; } kMap[] = { { L"local/", L"LocalState" }, { L"roaming/", L"RoamingState" }, { L"temp/", L"TempState" } };
    if (CompareStringOrdinal(uri, 14, L"ms-appdata:///", 14, TRUE) != CSTR_EQUAL) return FALSE;
    const wchar_t *rest = uri + 14, *bang = wcschr(aumid, L'!');
    if (!bang || bang - aumid >= 120 || wcsstr(rest, L"..") || wcschr(rest, L':') || rest[0] == L'/' || rest[0] == L'\\') return FALSE;
    for (const wchar_t *q = aumid; q < bang; ++q)     /* familia de paquete: letras, cifras, . _ - */
        if (!((*q >= L'a' && *q <= L'z') || (*q >= L'A' && *q <= L'Z') || (*q >= L'0' && *q <= L'9') || *q == L'.' || *q == L'_' || *q == L'-'))
            return FALSE;
    for (int k = 0; k < 3; ++k) {
        const int n = lstrlenW(kMap[k].seg);
        if (CompareStringOrdinal(rest, n, kMap[k].seg, n, TRUE) != CSTR_EQUAL) continue;
        wchar_t pkg[128], base[MAX_PATH];
        lstrcpynW(pkg, aumid, (int)(bang - aumid) + 1);
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return FALSE;
        if (lstrlenW(base) + lstrlenW(pkg) + lstrlenW(rest) + 32 >= MAX_PATH) return FALSE;
        wsprintfW(out, L"%s\\Packages\\%s\\%s\\%s", base, pkg, kMap[k].dir, rest + n);
        for (wchar_t *p = out; *p; ++p) if (*p == L'/') *p = L'\\';
        const DWORD at = GetFileAttributesW(out);
        return at != INVALID_FILE_ATTRIBUTES && !(at & FILE_ATTRIBUTE_DIRECTORY);
    }
    return FALSE;
}

/* Etiqueta principal del dominio: web.whatsapp.com → whatsapp, bbva.com.mx → bbva. */
static void MainLabel(const wchar_t *host, int len, wchar_t *out, int cap)
{
    int dots[16], nd = 0;
    for (int i = 0; i < len && nd < 16; ++i) if (host[i] == L'.') dots[nd++] = i;
    if (!nd) { lstrcpynW(out, host, min(cap, len + 1)); return; }
    int end = dots[nd - 1];                       /* quitar el TLD */
    /* segundo nivel genérico de país: .com.mx, .co.uk, .gob.mx… */
    if (nd >= 2 && len - dots[nd - 1] - 1 == 2) {
        static const wchar_t *kSld[] = { L"com", L"co", L"org", L"net", L"gob", L"gov", L"edu", L"ac", L"ne", L"or" };
        const int s = dots[nd - 2] + 1, sl = dots[nd - 1] - s;
        for (int k = 0; k < (int)(sizeof(kSld) / sizeof(kSld[0])); ++k)
            if (sl == lstrlenW(kSld[k]) && CompareStringOrdinal(host + s, sl, kSld[k], sl, TRUE) == CSTR_EQUAL) {
                end = dots[nd - 2]; --nd; break;
            }
    }
    const int start = nd >= 2 ? dots[nd - 2] + 1 : 0;
    lstrcpynW(out, host + start, min(cap, end - start + 1));
}

static void PrettySite(wchar_t *s)
{
    static const wchar_t *kNames[] = {
        L"WhatsApp", L"YouTube", L"LinkedIn", L"GitHub", L"GitLab", L"ChatGPT", L"OpenAI", L"TikTok",
        L"Outlook", L"Gmail", L"Facebook", L"Instagram", L"Telegram", L"Discord", L"Slack", L"Notion",
        L"Figma", L"Trello", L"Asana", L"Jira", L"Atlassian", L"Twitch", L"Spotify", L"Netflix",
        L"Zoom", L"Teams", L"Messenger", L"PayPal", L"MercadoLibre", L"Claude", L"X",
    };
    for (int k = 0; k < (int)(sizeof(kNames) / sizeof(kNames[0])); ++k)
        if (!lstrcmpiW(s, kNames[k])) { lstrcpyW(s, kNames[k]); return; }
    if (s[0]) CharUpperBuffW(s, 1);
}

static BOOL SiteName(const wchar_t *aumid, wchar_t *out, int cap)
{
    const wchar_t *p = wcsstr(aumid, L"!http");
    if (!p || !(p = wcsstr(p, L"://"))) {
        if (wcsstr(aumid, L"!chrome-extension://")) { lstrcpynW(out, L"Extensión del navegador", cap); return TRUE; }
        return FALSE;
    }
    p += 3;
    if (CompareStringOrdinal(p, 4, L"www.", 4, TRUE) == CSTR_EQUAL) p += 4;
    int n = 0;
    while (p[n] && p[n] != L'/' && p[n] != L':' && n < 120) ++n;
    if (!n) return FALSE;
    MainLabel(p, n, out, cap);
    /* teams.microsoft.com, outlook.live.com, meet.google.com…: el subdominio dice más */
    static const wchar_t *kPlatforms[] = { L"google", L"microsoft", L"live", L"office", L"office365" };
    for (int k = 0; k < (int)(sizeof(kPlatforms) / sizeof(kPlatforms[0])); ++k) {
        if (lstrcmpiW(out, kPlatforms[k])) continue;
        int sub = 0;
        while (sub < n && p[sub] != L'.') ++sub;
        if (sub < n && sub < cap && CompareStringOrdinal(p, sub, out, lstrlenW(out), TRUE) != CSTR_EQUAL) {
            if (sub == 4 && !lstrcmpiW(out, L"google") && CompareStringOrdinal(p, 4, L"mail", 4, TRUE) == CSTR_EQUAL)
                lstrcpynW(out, L"Gmail", cap);
            else
                lstrcpynW(out, p, sub + 1);
        }
        break;
    }
    if (!out[0]) lstrcpynW(out, p, min(cap, n + 1));
    PrettySite(out);
    return TRUE;
}

static const AppInfo *LookupApp(const wchar_t *aumid)
{
    for (int i = 0; i < s_napps; ++i)
        if (!lstrcmpiW(s_apps[i].aumid, aumid)) return &s_apps[i];

    AppInfo *a = &s_apps[s_napps < MAX_APPS ? s_napps++ : MAX_APPS - 1];
    for (int k = 0; k < 2; ++k) if (a->icon[k]) DeleteObject(a->icon[k]);
    ZeroMemory(a, sizeof(*a));
    lstrcpynW(a->aumid, aumid, 160);

    wchar_t path[200];
    if (SiteName(aumid, a->name, 64)) return a;     /* sitio web: no está en AppsFolder */
    if (lstrlenW(aumid) < 170) {
        wsprintfW(path, L"shell:AppsFolder\\%s", aumid);
        IShellItem *si = NULL;
        if (SUCCEEDED(SHCreateItemFromParsingName(path, NULL, &IID_IShellItem, (void **)&si))) {
            PWSTR dn = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(si, SIGDN_NORMALDISPLAY, &dn))) {
                lstrcpynW(a->name, dn, 64);
                CoTaskMemFree(dn);
            }
            IShellItem_Release(si);
        }
    }
    if (!a->name[0]) FallbackName(aumid, a->name, 64);
    return a;
}

/* ───────────────────────── XML del toast ───────────────────────── */
static int DecodeEntities(const wchar_t *s, int n, wchar_t *out, int cap)
{
    static const struct { const wchar_t *e; wchar_t c; } kEnt[] = {
        { L"&amp;", L'&' }, { L"&lt;", L'<' }, { L"&gt;", L'>' }, { L"&quot;", L'"' }, { L"&apos;", L'\'' },
    };
    int o = 0;
    for (int i = 0; i < n && o < cap - 1;) {
        if (s[i] == L'&') {
            BOOL done = FALSE;
            for (int k = 0; k < 5 && !done; ++k) {
                const int el = lstrlenW(kEnt[k].e);
                if (i + el <= n && CompareStringOrdinal(s + i, el, kEnt[k].e, el, FALSE) == CSTR_EQUAL) {
                    out[o++] = kEnt[k].c; i += el; done = TRUE;
                }
            }
            if (!done && i + 2 < n && s[i + 1] == L'#') {
                int j = i + 2, v = 0, hex = s[j] == L'x';
                if (hex) ++j;
                for (; j < n && s[j] != L';' && j - i < 10; ++j)
                    v = hex ? v * 16 + (s[j] <= L'9' ? s[j] - L'0' : (s[j] | 32) - L'a' + 10) : v * 10 + (s[j] - L'0');
                if (j < n && s[j] == L';' && v > 0 && v < 0xFFFF) { out[o++] = (wchar_t)v; i = j + 1; done = TRUE; }
            }
            if (done) continue;
        }
        out[o++] = (s[i] == L'\r' || s[i] == L'\n' || s[i] == L'\t') ? L' ' : s[i];
        ++i;
    }
    out[o] = 0;
    return o;
}

/* Valor del atributo name="…" dentro de la etiqueta [tag, end). */
static BOOL Attr(const wchar_t *tag, const wchar_t *end, LPCWSTR name, wchar_t *out, int cap)
{
    out[0] = 0;
    const int nl = lstrlenW(name);
    for (const wchar_t *p = tag; p + nl + 2 < end; ++p) {
        if ((p[-1] != L' ' && p[-1] != L'\t' && p[-1] != L'\n' && p[-1] != L'\r') ||
            CompareStringOrdinal(p, nl, name, nl, TRUE) != CSTR_EQUAL || p[nl] != L'=') continue;
        const wchar_t q = p[nl + 1];
        if (q != L'"' && q != L'\'') continue;
        const wchar_t *v = p + nl + 2, *e = v;
        while (e < end && *e != q) ++e;
        DecodeEntities(v, (int)(e - v), out, cap);
        return TRUE;
    }
    return FALSE;
}

static BYTE ActType(const wchar_t *v)
{
    if (!lstrcmpiW(v, L"protocol"))   return WA_PROTOCOL;
    if (!lstrcmpiW(v, L"background")) return WA_BACKGROUND;
    return WA_FOREGROUND;
}

/* <toast launch>, <action> y <input type="text">: lo que Windows ofrece en el aviso. */
static void ParseActions(const wchar_t *xml, WinNote *w)
{
    wchar_t v[48];
    const wchar_t *t = wcsstr(xml, L"<toast");
    if (t) {
        const wchar_t *e = wcschr(t, L'>');
        if (e) {
            Attr(t + 1, e, L"launch", w->launch, 400);
            if (Attr(t + 1, e, L"activationType", v, 48)) w->launchType = ActType(v);
        }
    }
    int guard = 0;      /* XML hostil (miles de etiquetas sin cerrar): tope de trabajo */
    for (const wchar_t *p = xml; guard++ < 64 && (p = wcsstr(p, L"<input")) != NULL; ++p) {
        const wchar_t *e = wcschr(p, L'>');
        if (!e) break;
        if (Attr(p + 1, e, L"type", v, 48) && !lstrcmpiW(v, L"text") && Attr(p + 1, e, L"id", w->inputId, 24)) {
            Attr(p + 1, e, L"placeHolderContent", w->inputHint, 48);
            w->hasInput = TRUE;
            break;
        }
    }
    guard = 0;
    for (const wchar_t *p = xml; w->nact < WN_ACTIONS && guard++ < 64 && (p = wcsstr(p, L"<action")) != NULL; ++p) {
        if (p[7] != L' ' && p[7] != L'/' && p[7] != L'\t') continue;      /* no <actions> */
        const wchar_t *e = wcschr(p, L'>');
        if (!e) break;
        WinAction *a = &w->act[w->nact];
        ZeroMemory(a, sizeof(*a));
        if (Attr(p + 1, e, L"placement", v, 48) && !lstrcmpiW(v, L"contextMenu")) continue;
        if (Attr(p + 1, e, L"activationType", v, 48)) {
            if (!lstrcmpiW(v, L"system")) continue;                       /* posponer / descartar */
            a->type = ActType(v);
        }
        if (!Attr(p + 1, e, L"content", a->label, 32) || !a->label[0]) continue;
        Attr(p + 1, e, L"arguments", a->args, 400);
        Attr(p + 1, e, L"hint-inputId", a->input, 24);
        w->nact++;
    }
}

/* El icono propio del aviso: <image placement="appLogoOverride" src="…">. Los navegadores
 * guardan ahí el icono del sitio como archivo local; solo se aceptan archivos locales. */
static void ParseLogo(const wchar_t *xml, WinNote *w)
{
    wchar_t v[48], src[MAX_PATH * 2];
    w->logo[0] = 0;
    int guard = 0;
    for (const wchar_t *p = xml; guard++ < 64 && (p = wcsstr(p, L"<image")) != NULL; ++p) {
        const wchar_t *e = wcschr(p, L'>');
        if (!e) break;
        if (!Attr(p + 1, e, L"placement", v, 48) || lstrcmpiW(v, L"appLogoOverride")) continue;
        if (!Attr(p + 1, e, L"src", src, MAX_PATH * 2)) continue;
        const wchar_t *s = src;
        if (AppDataPath(w->aumid, src, w->logo)) return;            /* Edge: dentro de su paquete */
        if (CompareStringOrdinal(s, 8, L"file:///", 8, TRUE) == CSTR_EQUAL) s += 8;
        else if (!(s[0] && s[1] == L':' && (s[2] == L'\\' || s[2] == L'/'))) continue;   /* nada de http ni ms-appx */
        /* %XX (UTF-8) y barras de URL a ruta de Windows */
        char u8[MAX_PATH * 3];
        int k = 0;
        for (; *s && k < (int)sizeof(u8) - 4; ++s) {
            if (*s == L'%' && iswxdigit(s[1]) && iswxdigit(s[2])) {
                const wchar_t hx[3] = { s[1], s[2], 0 };
                u8[k++] = (char)wcstol(hx, NULL, 16);
                s += 2;
            } else if (*s == L'/') u8[k++] = '\\';
            else k += WideCharToMultiByte(CP_UTF8, 0, s, 1, u8 + k, (int)sizeof(u8) - k - 1, NULL, NULL);
        }
        u8[k] = 0;
        wchar_t path[MAX_PATH];
        if (!MultiByteToWideChar(CP_UTF8, 0, u8, -1, path, MAX_PATH)) continue;
        if (!(((path[0] | 0x20) >= L'a' && (path[0] | 0x20) <= L'z') && path[1] == L':' && path[2] == L'\\') ||
            wcsstr(path, L"\\\\") || wcsstr(path, L"..") || GetDriveTypeW((wchar_t[]){ path[0], L':', L'\\', 0 }) == DRIVE_REMOTE)
            continue;                                     /* solo archivos locales: nada de rutas de red */
        const DWORD at = GetFileAttributesW(path);
        if (at == INVALID_FILE_ATTRIBUTES || (at & FILE_ATTRIBUTE_DIRECTORY)) continue;
        lstrcpynW(w->logo, path, MAX_PATH);
        return;
    }
}

/* Primer <text> = título; el resto se une como cuerpo. */
static void ParsePayload(const void *blob, int bytes, WinNote *w)
{
    wchar_t *title = w->title, *body = w->body;
    const int tcap = 128, bcap = 256;
    title[0] = body[0] = 0;
    if (!blob || bytes <= 0 || bytes > 256 * 1024) return;

    wchar_t *xml;
    int n;
    const BYTE *b = (const BYTE *)blob;
    if (bytes >= 2 && b[1] == 0) {             /* UTF-16LE */
        n = bytes / 2;
        xml = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof(wchar_t));
        if (!xml) return;
        CopyMemory(xml, b, n * sizeof(wchar_t));
    } else {                                   /* UTF-8 */
        n = MultiByteToWideChar(CP_UTF8, 0, (const char *)b, bytes, NULL, 0);
        xml = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof(wchar_t));
        if (!xml) return;
        MultiByteToWideChar(CP_UTF8, 0, (const char *)b, bytes, xml, n);
    }
    xml[n] = 0;

    int found = 0;
    for (wchar_t *p = xml; (p = wcsstr(p, L"<text")) != NULL;) {
        p += 5;
        if (*p != L' ' && *p != L'>') continue;
        wchar_t *gt = wcschr(p, L'>');
        if (!gt) break;
        if (gt[-1] == L'/') { p = gt + 1; continue; }   /* <text/> vacío */
        wchar_t *end = wcsstr(gt + 1, L"</text>");
        if (!end) break;

        wchar_t tmp[512];
        DecodeEntities(gt + 1, min((int)(end - gt - 1), 2000), tmp, 512);
        const wchar_t *t = tmp;
        while (*t == L' ') ++t;
        if (*t) {
            if (!found++) lstrcpynW(title, t, tcap);
            else {
                const int len = lstrlenW(body);
                if (len && len < bcap - 2) { body[len] = L' '; body[len + 1] = 0; }
                lstrcpynW(body + lstrlenW(body), t, bcap - lstrlenW(body));
            }
        }
        p = end + 7;
    }
    ParseActions(xml, w);
    if (IsSiteNote(w->aumid)) ParseLogo(xml, w);     /* el icono del sitio web, si lo trae */
    HeapFree(GetProcessHeap(), 0, xml);
}

/* ───────────────────────── Base de datos ───────────────────────── */
static BOOL NotifDir(wchar_t *out)
{
    PWSTR base = NULL;
    BOOL ok = SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &base)) && lstrlenW(base) < MAX_PATH - 40;
    if (ok) wsprintfW(out, L"%s\\Microsoft\\Windows\\Notifications", base);
    CoTaskMemFree(base);
    return ok;
}

static BOOL OpenDb(void)
{
    if (s_db) return TRUE;
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    char u8[MAX_PATH * 3];
    if (!NotifDir(dir)) return FALSE;
    wsprintfW(path, L"%s\\wpndatabase.db", dir);
    if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, u8, sizeof(u8), NULL, NULL)) return FALSE;
    if (sqlite3_open_v2(u8, &s_db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
        sqlite3_close(s_db);
        s_db = NULL;
        return FALSE;
    }
    sqlite3_busy_timeout(s_db, 250);
    return TRUE;
}

static BOOL IsDismissed(LONGLONG id)
{
    if (id <= s_cleared) return TRUE;
    /* s_ndismissed sigue contando pasado 64 (anillo): no leer fuera de la tabla */
    const int n = min(s_ndismissed, (int)(sizeof(s_dismissed) / sizeof(s_dismissed[0])));
    for (int i = 0; i < n; ++i) if (s_dismissed[i] == id) return TRUE;
    return FALSE;
}

static BOOL SameChat(const WinNote *a, const NoteKey *ka, const WinNote *b, const NoteKey *kb)
{
    return ka->tag[0] && !lstrcmpW(ka->tag, kb->tag) && !lstrcmpW(ka->group, kb->group) &&
           !lstrcmpiW(a->aumid, b->aumid);
}

static void CopyKey(NoteKey *k, const unsigned char *tag, const unsigned char *group)
{
    k->tag[0] = k->group[0] = 0;
    if (tag) MultiByteToWideChar(CP_UTF8, 0, (const char *)tag, -1, k->tag, KEY_CAP);
    if (group) MultiByteToWideChar(CP_UTF8, 0, (const char *)group, -1, k->group, KEY_CAP);
    k->tag[KEY_CAP - 1] = k->group[KEY_CAP - 1] = 0;
}

static void PushHistory(const WinNote *w, const NoteKey *k)
{
    if (s_nhist == WN_MAX) {            /* lleno: fuera el más antiguo (el último) */
        --s_nhist;
    }
    MoveMemory(&s_hist[1], &s_hist[0], s_nhist * sizeof(WinNote));
    MoveMemory(&s_histKey[1], &s_histKey[0], s_nhist * sizeof(NoteKey));
    s_hist[0] = *w;
    s_hist[0].id = ++s_histSeq;
    s_histKey[0] = *k;
    ++s_nhist;
}

static void DropHistory(int i)
{
    MoveMemory(&s_hist[i], &s_hist[i + 1], (s_nhist - i - 1) * sizeof(WinNote));
    MoveMemory(&s_histKey[i], &s_histKey[i + 1], (s_nhist - i - 1) * sizeof(NoteKey));
    --s_nhist;
}

/* s_notes = base + historial, del más nuevo al más antiguo (por llegada). */
static void Merge(void)
{
    int a = 0, b = 0, n = 0;
    while (n < WN_MAX && (a < s_ncur || b < s_nhist)) {
        const BOOL takeCur = b >= s_nhist || (a < s_ncur && s_cur[a].arrival >= s_hist[b].arrival);
        s_notes[n++] = takeCur ? s_cur[a++] : s_hist[b++];
    }
    s_count = n;
}

void Wn_Refresh(BOOL notify)
{
    if (!OpenDb()) return;
    static const char kSql[] =
        "SELECT n.Id, n.ArrivalTime, n.Payload, h.PrimaryId, n.Tag, n.\"Group\" "
        "FROM Notification n JOIN NotificationHandler h ON h.RecordId = n.HandlerId "
        "WHERE n.Type = 'toast' ORDER BY n.Id DESC LIMIT 60";
    /* Sin Tag/Group (esquema antiguo): igual que antes, sin historial. */
    static const char kSqlOld[] =
        "SELECT n.Id, n.ArrivalTime, n.Payload, h.PrimaryId, NULL, NULL "
        "FROM Notification n JOIN NotificationHandler h ON h.RecordId = n.HandlerId "
        "WHERE n.Type = 'toast' ORDER BY n.Id DESC LIMIT 60";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s_db, kSql, -1, &st, NULL) != SQLITE_OK &&
        sqlite3_prepare_v2(s_db, kSqlOld, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(s_db);    /* esquema distinto o base bloqueada: reintentar en el próximo cambio */
        s_db = NULL;
        return;
    }

    static WinNote fresh[WN_MAX];
    static NoteKey freshKey[WN_MAX];
    int n = 0;
    LONGLONG maxId = s_lastSeen, newestId = 0;
    while (n < WN_MAX && sqlite3_step(st) == SQLITE_ROW) {
        const LONGLONG id = sqlite3_column_int64(st, 0);
        if (id > maxId) maxId = id;
        if (IsDismissed(id)) continue;

        WinNote *w = &fresh[n];
        ZeroMemory(w, sizeof(*w));
        w->id = id;
        w->arrival = sqlite3_column_int64(st, 1);
        const wchar_t *aumid = (const wchar_t *)sqlite3_column_text16(st, 3);
        lstrcpynW(w->aumid, aumid ? aumid : L"", 160);
        ParsePayload(sqlite3_column_blob(st, 2), sqlite3_column_bytes(st, 2), w);
        if (!w->title[0] && !w->body[0]) continue;
        CopyKey(&freshKey[n], sqlite3_column_text(st, 4), sqlite3_column_text(st, 5));

        lstrcpynW(w->app, LookupApp(w->aumid)->name, 64);
        if (s_lastSeen >= 0 && id > s_lastSeen && !newestId) newestId = id;
        ++n;
    }
    sqlite3_finalize(st);

    /* Lo que había y ya no está porque otro del mismo chat lo reemplazó: al historial.
     * (También si Windows lo reescribió en el sitio con otro texto.) */
    for (int i = 0; i < s_ncur; ++i) {
        const WinNote *old = &s_cur[i];
        if (!s_curKey[i].tag[0] || IsDismissed(old->id)) continue;
        BOOL kept = FALSE, replaced = FALSE;
        for (int j = 0; j < n; ++j) {
            if (fresh[j].id == old->id) {
                kept = !lstrcmpW(fresh[j].title, old->title) && !lstrcmpW(fresh[j].body, old->body);
                replaced = !kept;
                break;
            }
            if (fresh[j].id > old->id && SameChat(&fresh[j], &freshKey[j], old, &s_curKey[i])) replaced = TRUE;
        }
        if (replaced && !kept) PushHistory(old, &s_curKey[i]);
    }
    /* Y fuera del historial lo de los chats que ya no tienen ningún aviso en Windows. */
    for (int i = s_nhist - 1; i >= 0; --i) {
        BOOL alive = FALSE;
        for (int j = 0; j < n && !alive; ++j) alive = SameChat(&fresh[j], &freshKey[j], &s_hist[i], &s_histKey[i]);
        if (!alive || IsDismissed(s_hist[i].id)) DropHistory(i);
    }

    CopyMemory(s_cur, fresh, n * sizeof(WinNote));
    CopyMemory(s_curKey, freshKey, n * sizeof(NoteKey));
    s_ncur = n;
    s_lastSeen = maxId < 0 ? 0 : maxId;
    Merge();

    if (notify && newestId)
        for (int i = 0; i < s_count; ++i)
            if (s_notes[i].id == newestId) { Notch_ShowWin(&s_notes[i]); break; }
    Notch_NotesChanged();
}

/* ¿Escribió Windows algo nuevo? data_version cambia solo cuando otra conexión
 * confirma cambios: es una consulta trivial, apta para sondear cada medio segundo. */
BOOL Wn_Changed(void)
{
    static LONGLONG last = -1;
    if (!OpenDb()) return FALSE;
    sqlite3_stmt *st = NULL;
    LONGLONG v = last;
    if (sqlite3_prepare_v2(s_db, "PRAGMA data_version", -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    const BOOL changed = last >= 0 && v != last;
    last = v;
    return changed;
}

int Wn_Count(void) { return s_count; }

/* Estado de "No molestar" (Asistente de concentración). Windows lo publica en el estado WNF
 * WNF_SHEL_QUIETHOURS_ACTIVE_PROFILE_CHANGED; solo se lee: escribirlo no lo cambia (probado
 * en 26200) y no hay API pública para activarlo, así que OpenDock solo lo muestra y avisa. */
int Wn_QuietHours(void)
{
    typedef LONG (NTAPI *QueryWnf)(const ULONGLONG *, const void *, const void *, ULONG *, void *, ULONG *);
    static QueryWnf q;
    static BOOL tried;
    if (!tried) { tried = TRUE; q = (QueryWnf)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryWnfStateData"); }
    if (!q) return -1;
    static const ULONGLONG kQuietHours = 0x0d83063ea3bf1c75ULL;
    DWORD v = 0;
    ULONG size = sizeof(v), stamp = 0;
    if (q(&kQuietHours, NULL, NULL, &stamp, &v, &size) < 0 || size != sizeof(v)) return -1;
    return v > 2 ? 1 : (int)v;
}

/* El icono propio de un aviso, decodificado con WIC (PNG, JPG, ICO, BMP…) y escalado a
 * px (encajado, sin deformar). Se guarda en caché: el navegador puede borrar el archivo. */
static const GUID kCLSID_WICFactory = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const GUID kIID_WICFactory   = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
static const GUID kWICPBGRA         = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10 } };

static HBITMAP DecodeLogo(LPCWSTR path, int px)
{
    IWICImagingFactory *f = NULL;
    if (FAILED(CoCreateInstance(&kCLSID_WICFactory, NULL, CLSCTX_INPROC_SERVER, &kIID_WICFactory, (void **)&f)) || !f) return NULL;
    HBITMAP out = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICFormatConverter *cv = NULL;
    IWICBitmapScaler *sc = NULL;
    UINT w0 = 0, h0 = 0;
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(f, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &fr)) &&
        SUCCEEDED(IWICBitmapFrameDecode_GetSize(fr, &w0, &h0)) && w0 && h0 && w0 <= 4096 && h0 <= 4096 &&
        SUCCEEDED(IWICImagingFactory_CreateFormatConverter(f, &cv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)fr, &kWICPBGRA, WICBitmapDitherTypeNone, NULL, 0,
                                                 WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(IWICImagingFactory_CreateBitmapScaler(f, &sc))) {
        const UINT w = w0 >= h0 ? (UINT)px : max(1u, (UINT)px * w0 / h0), h = h0 >= w0 ? (UINT)px : max(1u, (UINT)px * h0 / w0);
        if (SUCCEEDED(IWICBitmapScaler_Initialize(sc, (IWICBitmapSource *)cv, w, h, WICBitmapInterpolationModeFant))) {
            BITMAPINFO bi;
            ZeroMemory(&bi, sizeof(bi));
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = (LONG)w;
            bi.bmiHeader.biHeight = -(LONG)h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            void *bits = NULL;
            out = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
            if (out && FAILED(IWICBitmapScaler_CopyPixels(sc, NULL, w * 4, w * h * 4, (BYTE *)bits))) { DeleteObject(out); out = NULL; }
        }
    }
    if (sc) IWICBitmapScaler_Release(sc);
    if (cv) IWICFormatConverter_Release(cv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    IWICImagingFactory_Release(f);
    return out;
}

HBITMAP Wn_LogoIcon(LPCWSTR path, int px)
{
    /* Caché holgada y por uso (LRU): con hasta WN_MAX avisos, cada uno con su foto y el icono
     * del sitio en dos tamaños. Si se reciclara, al redibujar (hover, scroll) habría que leer
     * de nuevo el archivo, y el navegador suele haberlo borrado ya: el icono cambiaría. */
    #define LOGO_CACHE (WN_MAX * 2 + 16)
    static struct { wchar_t path[MAX_PATH]; int px; HBITMAP bmp; DWORD used; } cache[LOGO_CACHE];
    static DWORD tick;
    if (!path || !path[0]) return NULL;
    for (int i = 0; i < LOGO_CACHE; ++i)
        if (cache[i].px == px && !lstrcmpiW(cache[i].path, path)) { cache[i].used = ++tick; return cache[i].bmp; }
    HBITMAP b = DecodeLogo(path, px);
    int slot = 0;
    for (int i = 1; i < LOGO_CACHE; ++i) if (cache[i].used < cache[slot].used) slot = i;
    cache[slot].used = ++tick;
    if (cache[slot].bmp) DeleteObject(cache[slot].bmp);
    lstrcpynW(cache[slot].path, path, MAX_PATH);
    cache[slot].px = px;
    cache[slot].bmp = b;                /* también los fallos, para no reintentar */
    return b;
}

/* Pide el icono al sistema exactamente a px×px: se dibuja 1:1, sin escalar ni pixelar. */
HBITMAP Wn_AppIcon(LPCWSTR aumid, int px)
{
    AppInfo *a = (AppInfo *)LookupApp(aumid);
    if (IsSiteNote(aumid)) {    /* sitios web: su icono (el que registró el navegador), no el del navegador */
        if (!g_cfg.siteIcons) return NULL;
        if (!a->siteTried && s_db) {
            a->siteTried = TRUE;
            static const char *kSql = "SELECT a.AssetValue FROM HandlerAssets a JOIN NotificationHandler h ON h.RecordId = a.HandlerId "
                                      "WHERE h.PrimaryId = ?1 AND a.AssetKey = 'IconUri' LIMIT 1";
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(s_db, kSql, -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_text16(st, 1, aumid, -1, SQLITE_TRANSIENT);
                if (sqlite3_step(st) == SQLITE_ROW) {
                    const wchar_t *uri = (const wchar_t *)sqlite3_column_text16(st, 0);
                    if (uri && !AppDataPath(aumid, uri, a->siteIcon)) a->siteIcon[0] = 0;
                }
                sqlite3_finalize(st);
            }
        }
        return a->siteIcon[0] ? Wn_LogoIcon(a->siteIcon, px) : NULL;
    }

    for (int k = 0; k < 2; ++k) if (a->iconPx[k] == px) return a->icon[k];
    const int k = a->iconPx[0] ? 1 : 0;     /* un tercer tamaño (cambio de DPI) pisa el segundo */
    if (a->icon[k]) { DeleteObject(a->icon[k]); a->icon[k] = NULL; }
    a->iconPx[k] = px;

    wchar_t path[200];
    if (lstrlenW(aumid) >= 170) return NULL;
    wsprintfW(path, L"shell:AppsFolder\\%s", aumid);
    IShellItem *si = NULL;
    if (SUCCEEDED(SHCreateItemFromParsingName(path, NULL, &IID_IShellItem, (void **)&si))) {
        IShellItemImageFactory *f = NULL;
        if (SUCCEEDED(IShellItem_QueryInterface(si, &IID_IShellItemImageFactory, (void **)&f))) {
            SIZE sz = { px, px };
            IShellItemImageFactory_GetImage(f, sz, SIIGBF_RESIZETOFIT | SIIGBF_ICONONLY, &a->icon[k]);
            IShellItemImageFactory_Release(f);
        }
        IShellItem_Release(si);
    }
    return a->icon[k];
}

const WinNote *Wn_Get(int i)
{
    return i >= 0 && i < s_count ? &s_notes[i] : &s_empty;
}

void Wn_Dismiss(LONGLONG id)
{
    if (s_ndismissed < 64) s_dismissed[s_ndismissed++] = id;
    else s_dismissed[(s_ndismissed++) % 64] = id;
    for (int i = 0; i < s_nhist; ++i)
        if (s_hist[i].id == id) { DropHistory(i); break; }
    for (int i = 0; i < s_count; ++i)
        if (s_notes[i].id == id) {
            MoveMemory(&s_notes[i], &s_notes[i + 1], (s_count - i - 1) * sizeof(WinNote));
            --s_count;
            break;
        }
    Notch_NotesChanged();
}

void Wn_DismissAll(void)
{
    s_cleared = s_lastSeen;
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, L"ClearedId", 0, REG_QWORD, (const BYTE *)&s_cleared, sizeof(s_cleared));
        RegCloseKey(k);
    }
    s_count = 0;
    s_ncur = s_nhist = 0;
    Notch_NotesChanged();
}

/* Abre la app de la notificación (explorer resuelve el AUMID; nada se carga en nuestro proceso). */
void Wn_Open(const WinNote *w)
{
    if (!w->aumid[0] || lstrlenW(w->aumid) > 160) return;
    wchar_t target[200];
    wsprintfW(target, L"shell:AppsFolder\\%s", w->aumid);
    App_ShellOpen(target);
}

/* ───────────────────────── Acciones y respuestas rápidas ─────────────────────────
 * Windows entrega los clics de un aviso a la app por su "activador" COM
 * (INotificationActivationCallback): un servidor fuera de proceso que la app registra en
 * su acceso directo del menú Inicio, en el registro o en su manifiesto (MSIX). Lo buscamos
 * igual que Windows y le pasamos los mismos argumentos (y el texto de la respuesta).
 * Nada de la app se carga en nuestro proceso: se habla con ella por COM entre procesos. */
typedef struct { LPCWSTR Key, Value; } NotifInput;
typedef struct NotifCbVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(void *);
    ULONG   (STDMETHODCALLTYPE *Release)(void *);
    HRESULT (STDMETHODCALLTYPE *Activate)(void *, LPCWSTR aumid, LPCWSTR args, const NotifInput *data, ULONG count);
} NotifCbVtbl;
typedef struct { const NotifCbVtbl *lpVtbl; } NotifCb;
static const GUID kIID_NotifCb = { 0x53E31837, 0x6600, 0x4A81, { 0x93, 0x95, 0x75, 0xCF, 0xFE, 0x74, 0x6F, 0x94 } };
static const PROPERTYKEY kPK_AumId     = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 5 };
static const PROPERTYKEY kPK_Activator = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 26 };

/* Accesos directos del menú Inicio con activador: se leen una vez. */
#define MAX_LINKS 128
static struct { wchar_t id[128]; CLSID clsid; } s_links[MAX_LINKS];
static int  s_nlinks;
static BOOL s_linksRead;

static void ScanLinks(const wchar_t *dir, int depth)
{
    wchar_t pat[MAX_PATH];
    if (lstrlenW(dir) > MAX_PATH - 8 || depth > 3) return;
    wsprintfW(pat, L"%s\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.' || s_nlinks >= MAX_LINKS) continue;
        wchar_t path[MAX_PATH];
        if (lstrlenW(dir) + lstrlenW(fd.cFileName) + 2 >= MAX_PATH) continue;
        wsprintfW(path, L"%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) ScanLinks(path, depth + 1);
            continue;
        }
        const int n = lstrlenW(fd.cFileName);
        if (n < 5 || lstrcmpiW(fd.cFileName + n - 4, L".lnk")) continue;
        IShellLinkW *sl = NULL;
        if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) continue;
        IPersistFile *pf = NULL;
        IPropertyStore *ps = NULL;
        if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf)) &&
            SUCCEEDED(IPersistFile_Load(pf, path, STGM_READ)) &&
            SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPropertyStore, (void **)&ps))) {
            PROPVARIANT id, cl;
            PropVariantInit(&id); PropVariantInit(&cl);
            if (SUCCEEDED(IPropertyStore_GetValue(ps, &kPK_Activator, &cl)) && cl.vt == VT_CLSID && cl.puuid &&
                SUCCEEDED(IPropertyStore_GetValue(ps, &kPK_AumId, &id)) && id.vt == VT_LPWSTR && id.pwszVal) {
                lstrcpynW(s_links[s_nlinks].id, id.pwszVal, 128);
                s_links[s_nlinks].clsid = *cl.puuid;
                s_nlinks++;
            }
            PropVariantClear(&id); PropVariantClear(&cl);
        }
        if (ps) IPropertyStore_Release(ps);
        if (pf) IPersistFile_Release(pf);
        IShellLinkW_Release(sl);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* MSIX/AppX: el manifiesto del paquete declara ToastActivatorCLSID. */
static BOOL PackageActivator(const wchar_t *aumid, CLSID *out)
{
    typedef LONG (WINAPI *FindFn)(PCWSTR, UINT32, UINT32 *, PWSTR *, UINT32 *, WCHAR *, UINT32 *);
    typedef LONG (WINAPI *PathFn)(PCWSTR, UINT32 *, PWSTR);
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    FindFn find = k ? (FindFn)(void *)GetProcAddress(k, "FindPackagesByPackageFamily") : NULL;
    PathFn path = k ? (PathFn)(void *)GetProcAddress(k, "GetPackagePathByFullName") : NULL;
    const wchar_t *bang = wcschr(aumid, L'!');
    if (!find || !path || !bang || bang - aumid >= 120) return FALSE;
    wchar_t pfn[128];
    lstrcpynW(pfn, aumid, (int)(bang - aumid) + 1);
    UINT32 count = 1, len = 512;
    PWSTR names[1];
    WCHAR buf[512];
    if (find(pfn, 0x10 /* PACKAGE_FILTER_HEAD */, &count, names, &len, buf, NULL) != 0 || count < 1) return FALSE;
    WCHAR dir[MAX_PATH];
    UINT32 dl = MAX_PATH - 20;
    if (path(names[0], &dl, dir) != 0) return FALSE;
    lstrcatW(dir, L"\\AppxManifest.xml");
    HANDLE f = CreateFileW(dir, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    DWORD size = GetFileSize(f, NULL), got = 0;
    BOOL ok = FALSE;
    if (size > 0 && size < 4 * 1024 * 1024) {
        char *x = (char *)HeapAlloc(GetProcessHeap(), 0, size + 1);
        if (x && ReadFile(f, x, size, &got, NULL)) {
            x[got] = 0;
            const char *p = strstr(x, "ToastActivatorCLSID=\"");
            if (p) {
                wchar_t g[48];
                int i = 0;
                for (p += 21; *p && *p != '"' && i < 40; ++p) g[i++] = (wchar_t)*p;
                g[i] = 0;
                wchar_t br[48];
                wsprintfW(br, g[0] == L'{' ? L"%s" : L"{%s}", g);
                ok = SUCCEEDED(CLSIDFromString(br, out));
            }
        }
        if (x) HeapFree(GetProcessHeap(), 0, x);
    }
    CloseHandle(f);
    return ok;
}

static BOOL FindActivator(const wchar_t *aumid, CLSID *out)
{
    /* los avisos web los entrega el navegador */
    wchar_t id[160];
    if (IsSiteNote(aumid)) {
        if (wcsstr(aumid, L"MSEdge") || wcsstr(aumid, L"MicrosoftEdge")) lstrcpyW(id, L"MSEdge");
        else if (wcsstr(aumid, L"Chrome"))                               lstrcpyW(id, L"Chrome");
        else { const wchar_t *b = wcschr(aumid, L'!'); lstrcpynW(id, aumid, b ? (int)(b - aumid) + 1 : 160); }
    } else {
        lstrcpynW(id, aumid, 160);
    }
    wchar_t key[260], val[64];
    DWORD sz = sizeof(val);
    if (lstrlenW(id) < 200) {
        wsprintfW(key, L"Software\\Classes\\AppUserModelId\\%s", id);
        if (RegGetValueW(HKEY_CURRENT_USER, key, L"CustomActivator", RRF_RT_REG_SZ, NULL, val, &sz) == ERROR_SUCCESS &&
            SUCCEEDED(CLSIDFromString(val, out))) return TRUE;
    }
    if (!s_linksRead) {
        s_linksRead = TRUE;
        PWSTR d = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Programs, 0, NULL, &d))) { ScanLinks(d, 0); CoTaskMemFree(d); }
        if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_CommonPrograms, 0, NULL, &d))) { ScanLinks(d, 0); CoTaskMemFree(d); }
    }
    for (int i = 0; i < s_nlinks; ++i)
        if (!lstrcmpiW(s_links[i].id, id)) { *out = s_links[i].clsid; return TRUE; }
    return PackageActivator(aumid, out);
}

/* Un esquema de protocolo razonable: nada de rutas, ejecutables ni script. */
static BOOL SafeUri(const wchar_t *u)
{
    int i = 0;
    while (u[i] && ((u[i] >= L'a' && u[i] <= L'z') || (u[i] >= L'A' && u[i] <= L'Z') ||
                    (i && ((u[i] >= L'0' && u[i] <= L'9') || u[i] == L'+' || u[i] == L'-' || u[i] == L'.')))) ++i;
    if (i < 2 || u[i] != L':') return FALSE;
    /* los manejadores ms-* de Office, el instalador de apps o la ayuda abren cosas remotas:
     * de esa familia solo pasan los conocidos */
    if (i > 3 && CompareStringOrdinal(u, 3, L"ms-", 3, TRUE) == CSTR_EQUAL) {
        static const wchar_t *kOk[] = { L"ms-settings", L"ms-windows-store", L"ms-outlook", L"ms-teams", L"ms-clock", L"ms-photos" };
        BOOL ok = FALSE;
        for (int k = 0; k < (int)(sizeof(kOk) / sizeof(kOk[0])) && !ok; ++k) {
            const int n = lstrlenW(kOk[k]);
            ok = i == n && CompareStringOrdinal(u, n, kOk[k], n, TRUE) == CSTR_EQUAL;
        }
        if (!ok) return FALSE;
    }
    static const wchar_t *kBad[] = { L"file", L"shell", L"javascript", L"vbscript", L"data", L"search", L"search-ms",
                                     L"its", L"mk", L"hcp", L"res", L"jar", L"vbefile", L"jsfile" };
    for (int k = 0; k < (int)(sizeof(kBad) / sizeof(kBad[0])); ++k) {
        const int n = lstrlenW(kBad[k]);
        if (i == n && CompareStringOrdinal(u, n, kBad[k], n, TRUE) == CSTR_EQUAL) return FALSE;
    }
    return TRUE;
}

typedef struct {
    wchar_t aumid[160], args[400], inputId[24], reply[512];
    BYTE type;
    BOOL hasReply;
    WinNote fallback;
} ActJob;

static DWORD WINAPI ActivateWorker(LPVOID p)
{
    ActJob *j = (ActJob *)p;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    BOOL done = FALSE;
    if (j->type == WA_PROTOCOL) {
        if (SafeUri(j->args)) done = App_ShellOpen(j->args);     /* sin comillas ni modificadores colados */
    } else {
        CLSID clsid;
        NotifCb *cb = NULL;
        if (FindActivator(j->aumid, &clsid) &&
            SUCCEEDED(CoCreateInstance(&clsid, NULL, CLSCTX_LOCAL_SERVER, &kIID_NotifCb, (void **)&cb)) && cb) {
            CoAllowSetForegroundWindow((IUnknown *)cb, NULL);      /* que la app pueda pasar al frente */
            NotifInput in = { j->inputId, j->reply };
            done = SUCCEEDED(cb->lpVtbl->Activate(cb, j->aumid, j->args, j->hasReply ? &in : NULL, j->hasReply ? 1 : 0));
            cb->lpVtbl->Release(cb);
        }
    }
    if (!done && !j->hasReply) Wn_Open(&j->fallback);     /* sin activador: al menos abrir la app */
    if (!done && j->hasReply && g_ctrl)
        PostMessageW(g_ctrl, WM_WNCHANGED, 1, 0);         /* la respuesta no se pudo entregar */
    CoUninitialize();
    SecureZeroMemory(j->reply, sizeof(j->reply));
    HeapFree(GetProcessHeap(), 0, j);
    return 0;
}

void Wn_Activate(const WinNote *w, int action, LPCWSTR reply)
{
    if (!w->aumid[0]) return;
    ActJob *j = (ActJob *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ActJob));
    if (!j) return;
    lstrcpynW(j->aumid, w->aumid, 160);
    if (action >= 0 && action < w->nact) {
        lstrcpynW(j->args, w->act[action].args, 400);
        j->type = w->act[action].type;
    } else {
        lstrcpynW(j->args, w->launch, 400);
        j->type = w->launchType;
    }
    if (reply && reply[0] && w->hasInput) {
        lstrcpynW(j->inputId, w->inputId, 24);
        lstrcpynW(j->reply, reply, 512);
        j->hasReply = TRUE;
        if (j->type == WA_PROTOCOL) j->type = WA_FOREGROUND;
    }
    j->fallback = *w;
    HANDLE t = CreateThread(NULL, 0, ActivateWorker, j, 0, NULL);
    if (t) CloseHandle(t); else HeapFree(GetProcessHeap(), 0, j);
}

/* ───────────────────────── Vigilancia ───────────────────────── */
static DWORD WINAPI Watcher(LPVOID unused)
{
    (void)unused;
    wchar_t dir[MAX_PATH];
    if (!NotifDir(dir)) return 0;
    HANDLE h = CreateFileW(dir, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    static DWORD buf[1024];
    DWORD got;
    while (ReadDirectoryChangesW(h, buf, sizeof(buf), FALSE,
                                 FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE, &got, NULL, NULL))
        PostMessageW(g_ctrl, WM_WNCHANGED, 0, 0);
    CloseHandle(h);
    return 0;
}

void Wn_Start(void)
{
    static BOOL started;
    if (started) return;
    started = TRUE;
    DWORD sz = sizeof(s_cleared);
    RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"ClearedId", RRF_RT_REG_QWORD, NULL, &s_cleared, &sz);
    Wn_Refresh(FALSE);
    HANDLE t = CreateThread(NULL, 0, Watcher, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

void Wn_Stop(void)
{
    if (s_db) { sqlite3_close(s_db); s_db = NULL; }
    for (int i = 0; i < s_napps; ++i)
        for (int k = 0; k < 2; ++k) if (s_apps[i].icon[k]) DeleteObject(s_apps[i].icon[k]);
    s_napps = 0;
    s_count = 0;
    s_ncur = s_nhist = 0;
}

/* ───────────────────────── Restaurar banners ───────────────────────── */
static void RestoreBanners(void)
{
    HKEY mine;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, HIDDEN_KEY, 0, KEY_QUERY_VALUE, &mine) != ERROR_SUCCESS) return;
    wchar_t name[320], path[400];
    for (DWORD i = 0;; ++i) {
        DWORD nlen = 320, prev = 0, sz = sizeof(prev), type = 0;
        if (RegEnumValueW(mine, i, name, &nlen, NULL, &type, (BYTE *)&prev, &sz) != ERROR_SUCCESS) break;
        if (type != REG_DWORD || nlen > 300) continue;
        wsprintfW(path, NOTIF_SETTINGS L"\\%s", name);
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            if (prev == 0xFFFFFFFF) RegDeleteValueW(k, L"ShowBanner");
            else RegSetValueExW(k, L"ShowBanner", 0, REG_DWORD, (const BYTE *)&prev, sizeof(prev));
            RegCloseKey(k);
        }
    }
    RegCloseKey(mine);
    RegDeleteTreeW(HKEY_CURRENT_USER, HIDDEN_KEY);
}

/* Deshace en segundo plano los ShowBanner=0 que escribieron versiones anteriores. */
static DWORD WINAPI RestoreWorker(LPVOID unused)
{
    (void)unused;
    RestoreBanners();
    return 0;
}

void Wn_RestoreBanners(void)
{
    HANDLE t = CreateThread(NULL, 0, RestoreWorker, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else RestoreBanners();
}

void Wn_RestoreBannersSync(void)
{
    RestoreBanners();
}
