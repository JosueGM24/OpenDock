/*
 * dock.c — dock estilo macOS: panel flotante abajo con las apps ancladas y abiertas,
 * magnificación al pasar el cursor (como el Dock de Apple) y clic para abrir/enfocar.
 *
 * - Apps ancladas: se leen los .lnk de la barra de tareas del usuario.
 * - Apps abiertas: se enumeran las ventanas; una barrita blanca marca las que corren
 *   (más larga y brillante la que está al frente).
 * - Iconos a alta resolución (IShellItemImageFactory), en caché y con alpha propio.
 * - Fondo configurable: desenfoque propio de lo que hay detrás (como el notch) y opacidad.
 * - Ventana layered; fuera del panel los clics pasan al escritorio.
 * - Se lanza todo vía explorer.exe (ninguna extensión de shell entra en el proceso).
 * - Opcional: desactiva la barra de tareas de Windows (autohide + oculta) y la devuelve
 *   tal cual al salir.
 */
#define COBJMACROS
#include "app.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <math.h>

#define DOCK_CLASS    L"CornerRadius.Dock"
#define WM_DFRAME     (WM_APP + 70)
#define WM_DOCK_APPBAR (WM_APP + 71)
#define TIMER_DSTATUS 1
#define TIMER_DBLUR   2
#define TIMER_DTASK   3      /* vigilancia corta de la barra tras cerrar Inicio */
#define TIMER_DRESCAN 4      /* repaso de apps tras un aviso de Windows (agrupa ráfagas) */
#define TIMER_DBLURQ  5      /* recaptura del fondo tras mover una ventana */
#define TIMER_DPEEK   6      /* fin de la visita a la bandeja */
#define TIMER_DSINK   7      /* ocultar a medias: un momento después de salir el cursor */
#define SINK_DELAY    450
#define STATUS_MS     10000  /* repaso de seguridad: lo normal llega por avisos */
#define MAX_ITEMS     48
#define ICON_GAP      12
#define DOCK_PADX     16
#define DOCK_PADY     11
#define BOTTOM_MARGIN 12
#define MAXMAG        1.5f
#define SPREAD        1.55f
#define BACK_SCALE    4      /* el fondo se captura a 1/4 de resolución */
#define MAX_ICONS     128

static const int   kIconSizes[4] = { 30, 38, 46, 56 };
static const float kOpacity[4]   = { 0.30f, 0.55f, 0.75f, 0.92f };

typedef struct {
    wchar_t launch[MAX_PATH];   /* .lnk o ruta a abrir con explorer */
    wchar_t exe[MAX_PATH];      /* ejecutable (para casar con ventanas) */
    wchar_t name[80];
    const DWORD *px;            /* RGBA del icono (straight alpha), de la caché */
    int     iw, ih;
    BOOL    pinned, running, minimized, active;
    HWND    hwnd;               /* una ventana representativa, si corre */
    HWND    wins[8];            /* todas sus ventanas, en orden z (la [0] arriba) */
    int     nwin;
    int     fgSeg;              /* segmento de la ventana al frente (-1 = ninguna) */
    float   scale, vel;         /* magnificación animada */
    float   hopT;               /* segundos desde que empezó el salto (<0: sin salto) */
    int     hops;               /* nº de saltos de la animación */
    float   hopH;               /* altura del primer salto (px) */
    float   barW;               /* ancho animado de la barrita de "abierta" */
} DockItem;

static struct {
    HWND     hwnd;
    UINT     dpi;
    RECT     mon;
    Canvas   frame, back;
    BOOL     appbar;            /* registrado como AppBar: reserva su franja abajo */
    DockItem items[MAX_ITEMS];
    int      count;
    int      hot, pressed;      /* índice bajo el cursor / pulsado */
    float    mouseX;            /* x del cursor relativo al centro del panel */
    BOOL     inside, fullscreen;
    int      panelX, panelY, panelW, panelH;   /* panel dibujado (coords de ventana) */
    int      iconTop;           /* borde superior del icono más alto (magnificado/saltando) */
    int      backY, backX;      /* origen del fondo capturado (en la ventana) */
    Canvas   raw;               /* captura sin desenfocar: si no cambió, no se recalcula */
    RECT     dirtyPrev;         /* zona presentada en el fotograma anterior */
    Canvas   gstrip;            /* vidrio teñido precalculado (franja del panel) */
    DWORD    gstripKey;
    BOOL     gstripDirty;
    DWORD    backTick, backHash;
    LARGE_INTEGER last, freq;
    DWORD    lastScan;
    HWINEVENTHOOK fgHook, winHook, moveHook;
    BOOL     dirty;             /* hay que redibujar aunque nada se anime */
    BOOL     shellOpen;         /* Inicio / Buscar de Windows abierto */
    BOOL     peek;              /* la bandeja de Windows a la vista un momento */
    DWORD    peekAt;
    float    slide;             /* 0 = visible · 1 = escondido bajo el borde */
    float    sink, sinkV;       /* ocultar dock: 0 arriba · 1 con media altura bajo la pantalla */
    int      sinkPx;
    DWORD    leaveAt;
    BOOL     menuOpen;
    int      taskWatch;         /* ticks restantes de TIMER_DTASK */
} D;

static int DS(int v) { return MulDiv(v, (int)D.dpi, 96); }
static int Base(void) { return DS(kIconSizes[max(0, min(3, g_cfg.dockIcon))]); }
/* alto de la ventana: icono magnificado + salto */
static int WinH(void) { return (int)(Base() * 2.3f) + DS(36); }

/* ───────────────────────── Pacer de alta resolución ───────────────────────── */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
static HANDLE        s_dpOn;
static volatile LONG s_dframe;

static DWORD WINAPI DockPacer(LPVOID u)
{
    (void)u;
    HANDLE t = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!t) t = CreateWaitableTimerW(NULL, FALSE, NULL);
    if (!t) return 0;
    for (;;) {
        WaitForSingleObject(s_dpOn, INFINITE);
        LARGE_INTEGER due;
        due.QuadPart = -10000000LL / 120;
        SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE);
        WaitForSingleObject(t, 50);
        if (D.hwnd && !InterlockedExchange(&s_dframe, 1)) PostMessageW(D.hwnd, WM_DFRAME, 0, 0);
    }
}

static void PacerOn(BOOL on)
{
    if (!s_dpOn) {
        s_dpOn = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE th = s_dpOn ? CreateThread(NULL, 0, DockPacer, NULL, 0, NULL) : NULL;
        if (th) CloseHandle(th);
    }
    if (!s_dpOn) return;
    if (on) SetEvent(s_dpOn); else ResetEvent(s_dpOn);
}

/* ───────────────────────── Iconos (con caché) ───────────────────────── */
static BOOL LoadIconPixels(LPCWSTR path, int px, DWORD **out, int *ow, int *oh)
{
    *out = NULL;
    IShellItem *si = NULL;
    if (FAILED(SHCreateItemFromParsingName(path, NULL, &IID_IShellItem, (void **)&si))) return FALSE;
    IShellItemImageFactory *f = NULL;
    HBITMAP bmp = NULL;
    if (SUCCEEDED(IShellItem_QueryInterface(si, &IID_IShellItemImageFactory, (void **)&f))) {
        SIZE sz = { px, px };
        IShellItemImageFactory_GetImage(f, sz, SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK, &bmp);
        IShellItemImageFactory_Release(f);
    }
    IShellItem_Release(si);
    if (!bmp) return FALSE;

    BITMAP bm;
    if (!GetObjectW(bmp, sizeof(bm), &bm) || bm.bmWidth < 1 || bm.bmWidth > 512) { DeleteObject(bmp); return FALSE; }
    const int w = bm.bmWidth, h = abs(bm.bmHeight);
    DWORD *buf = (DWORD *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)w * h * 4);
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    HDC dc = GetDC(NULL);
    const BOOL ok = buf && GetDIBits(dc, bmp, 0, h, buf, &bi, DIB_RGB_COLORS) == h;
    ReleaseDC(NULL, dc);
    DeleteObject(bmp);
    if (!ok) { if (buf) HeapFree(GetProcessHeap(), 0, buf); return FALSE; }

    /* ¿alpha premultiplicado? deshacerlo para quedarnos con color straight + alpha */
    BOOL premult = TRUE, anyAlpha = FALSE;
    for (int i = 0; i < w * h; ++i) {
        const DWORD v = buf[i], a = v >> 24;
        if (a) anyAlpha = TRUE;
        if (((v >> 16) & 255) > a || ((v >> 8) & 255) > a || (v & 255) > a) premult = FALSE;
    }
    for (int i = 0; i < w * h; ++i) {
        DWORD v = buf[i];
        int a = anyAlpha ? (int)(v >> 24) : 255;
        int r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
        if (premult && anyAlpha && a > 0 && a < 255) {
            r = min(255, r * 255 / a); g = min(255, g * 255 / a); b = min(255, b * 255 / a);
        }
        buf[i] = (DWORD)a << 24 | (DWORD)r << 16 | (DWORD)g << 8 | b;
    }
    *out = buf; *ow = w; *oh = h;
    return TRUE;
}

/* Antes se recargaban todos los iconos cada 1,5 s; ahora se piden una vez por ruta. */
typedef struct { wchar_t path[MAX_PATH]; int size; DWORD *px; int w, h; } IconEntry;
static IconEntry s_icons[MAX_ICONS];
static int       s_nicons;

static void ClearIconCache(void)
{
    for (int i = 0; i < s_nicons; ++i)
        if (s_icons[i].px) HeapFree(GetProcessHeap(), 0, s_icons[i].px);
    s_nicons = 0;
}

static BOOL GetIcon(LPCWSTR path, DockItem *it)
{
    const int size = Base() * 2;
    for (int i = 0; i < s_nicons; ++i)
        if (s_icons[i].size == size && !lstrcmpiW(s_icons[i].path, path)) {
            it->px = s_icons[i].px; it->iw = s_icons[i].w; it->ih = s_icons[i].h;
            return it->px != NULL;
        }
    DWORD *px = NULL;
    int w = 0, h = 0;
    LoadIconPixels(path, size, &px, &w, &h);
    for (int i = 0; px && i < w * h; ++i) {           /* a premultiplicado, una sola vez */
        const DWORD v = px[i], a = v >> 24;
        px[i] = a << 24 | (((v >> 16) & 255) * a / 255) << 16 | (((v >> 8) & 255) * a / 255) << 8 | ((v & 255) * a / 255);
    }
    if (s_nicons < MAX_ICONS) {      /* también se guardan los fallos, para no reintentar */
        IconEntry *e = &s_icons[s_nicons++];
        lstrcpynW(e->path, path, MAX_PATH);
        e->size = size; e->px = px; e->w = w; e->h = h;
    } else if (px) {
        HeapFree(GetProcessHeap(), 0, px);    /* no debería pasar: Rescan vacía antes */
        px = NULL;
    }
    it->px = px; it->iw = w; it->ih = h;
    return px != NULL;
}

/* ───────────────────────── Apps ancladas / abiertas ───────────────────────── */
static const wchar_t *BaseName(const wchar_t *p)
{
    const wchar_t *b = p;
    for (const wchar_t *q = p; *q; ++q) if (*q == L'\\') b = q + 1;
    return b;
}

static DockItem *FindByExe(const wchar_t *exe)
{
    for (int i = 0; i < D.count; ++i)
        if (D.items[i].exe[0] && !lstrcmpiW(BaseName(D.items[i].exe), BaseName(exe))) return &D.items[i];
    return NULL;
}

static void ResolveLnk(LPCWSTR lnk, wchar_t *exeOut)
{
    exeOut[0] = 0;
    IShellLinkW *sl = NULL;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) return;
    IPersistFile *pf = NULL;
    if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
        if (SUCCEEDED(IPersistFile_Load(pf, lnk, STGM_READ)))
            IShellLinkW_GetPath(sl, exeOut, MAX_PATH, NULL, 0);
        IPersistFile_Release(pf);
    }
    IShellLinkW_Release(sl);
}

static void NewItem(DockItem *it)
{
    ZeroMemory(it, sizeof(*it));
    it->scale = 1.0f;
    it->hopT = -1.0f;
}

static void AddPinned(void)
{
    PWSTR appdata = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &appdata))) return;
    wchar_t dir[MAX_PATH], pat[MAX_PATH];
    wsprintfW(dir, L"%s\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar", appdata);
    CoTaskMemFree(appdata);
    wsprintfW(pat, L"%s\\*.lnk", dir);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (D.count >= MAX_ITEMS) break;
        DockItem *it = &D.items[D.count];
        NewItem(it);
        it->pinned = TRUE;
        wsprintfW(it->launch, L"%s\\%s", dir, fd.cFileName);
        ResolveLnk(it->launch, it->exe);
        lstrcpynW(it->name, fd.cFileName, 80);
        wchar_t *dot = wcsrchr(it->name, L'.');
        if (dot) *dot = 0;
        if (GetIcon(it->launch, it)) D.count++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static BOOL IsAppWindow(HWND w)
{
    if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return FALSE;
    const LONG ex = (LONG)GetWindowLongPtrW(w, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return FALSE;
    int cloak = 0;
    DwmGetWindowAttribute(w, 14 /*DWMWA_CLOAKED*/, &cloak, sizeof(cloak));
    if (cloak) return FALSE;
    return GetWindowTextLengthW(w) > 0;
}

static BOOL WindowExe(HWND w, wchar_t *path)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (!pid || pid == GetCurrentProcessId()) return FALSE;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return FALSE;
    DWORD n = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(p, 0, path, &n);
    CloseHandle(p);
    return ok;
}

/* Nombre corto de la app (FileDescription del ejecutable), o el nombre del .exe. */
static void AppNameRead(const wchar_t *exe, wchar_t *out, int cch);
static void AppName(const wchar_t *exe, wchar_t *out, int cch)
{
    static struct { wchar_t exe[MAX_PATH], name[80]; } cache[32];
    static int n, next;
    for (int i = 0; i < n; ++i) if (!lstrcmpiW(cache[i].exe, exe)) { lstrcpynW(out, cache[i].name, cch); return; }
    AppNameRead(exe, out, cch);
    const int slot = n < 32 ? n++ : (next++ % 32);
    lstrcpynW(cache[slot].exe, exe, MAX_PATH);
    lstrcpynW(cache[slot].name, out, 80);
}

static void AppNameRead(const wchar_t *exe, wchar_t *out, int cch)
{
    out[0] = 0;
    DWORD dummy = 0, sz = GetFileVersionInfoSizeW(exe, &dummy);
    if (sz && sz < 4 * 1024 * 1024) {
        void *vi = HeapAlloc(GetProcessHeap(), 0, sz);
        if (vi && GetFileVersionInfoW(exe, 0, sz, vi)) {
            struct { WORD lang, cp; } *tr = NULL;
            UINT len = 0;
            if (VerQueryValueW(vi, L"\\VarFileInfo\\Translation", (void **)&tr, &len) && len >= 4) {
                wchar_t key[64];
                wsprintfW(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr->lang, tr->cp);
                wchar_t *d = NULL;
                if (VerQueryValueW(vi, key, (void **)&d, &len) && d && len > 1) lstrcpynW(out, d, cch);
            }
        }
        if (vi) HeapFree(GetProcessHeap(), 0, vi);
    }
    if (!out[0]) {
        lstrcpynW(out, BaseName(exe), cch);
        wchar_t *dot = wcsrchr(out, L'.');
        if (dot) *dot = 0;
    }
}

static BOOL CALLBACK EnumProc(HWND w, LPARAM lp)
{
    (void)lp;
    if (!IsAppWindow(w)) return TRUE;
    wchar_t path[MAX_PATH];
    if (!WindowExe(w, path)) return TRUE;
    if (!lstrcmpiW(BaseName(path), L"explorer.exe")) return TRUE;

    DockItem *it = FindByExe(path);
    if (!it && D.count < MAX_ITEMS) {       /* app abierta no anclada: añadir al final */
        it = &D.items[D.count];
        NewItem(it);
        lstrcpynW(it->exe, path, MAX_PATH);
        lstrcpynW(it->launch, path, MAX_PATH);
        AppName(path, it->name, 80);
        if (GetIcon(path, it)) D.count++;
        else return TRUE;
    }
    if (it) {
        if (it->nwin < 8) it->wins[it->nwin++] = w;
        if (!it->running) { it->running = TRUE; it->hwnd = w; it->minimized = TRUE; }
        if (!IsIconic(w)) { it->hwnd = w; it->minimized = FALSE; }   /* preferir una no minimizada */
    }
    return TRUE;
}

static void UpdateActive(void)
{
    HWND fg = GetForegroundWindow();
    if (fg) fg = GetAncestor(fg, GA_ROOTOWNER);
    wchar_t path[MAX_PATH];
    const BOOL have = fg && WindowExe(fg, path);
    for (int i = 0; i < D.count; ++i) {
        DockItem *it = &D.items[i];
        it->active = have && it->running && it->exe[0] && !lstrcmpiW(BaseName(it->exe), BaseName(path));
        if (it->active && fg && !IsIconic(fg)) it->hwnd = fg;
        /* cada ventana conserva su segmento aunque cambie el orden z: se numeran por
         * identificador (estable mientras viva la ventana) */
        it->fgSeg = -1;
        for (int k = 0; k < it->nwin && it->active; ++k) {
            if (it->wins[k] != fg) continue;
            int rank = 0;
            for (int j = 0; j < it->nwin; ++j) if ((ULONG_PTR)it->wins[j] < (ULONG_PTR)fg) ++rank;
            it->fgSeg = rank;
        }
    }
}

static const wchar_t *ItemKey(const DockItem *it) { return it->exe[0] ? it->exe : it->launch; }

static void Rescan(void)
{
    /* conservar el estado animado por app para no cortar la animación */
    static DockItem keep[MAX_ITEMS];
    const int oldN = D.count;
    CopyMemory(keep, D.items, sizeof(DockItem) * oldN);

    if (s_nicons > MAX_ICONS - MAX_ITEMS) ClearIconCache();   /* los items se rehacen ahora */
    D.count = 0;
    AddPinned();
    const int pinned = D.count;
    EnumWindows(EnumProc, 0);
    /* EnumWindows las da en orden z (cambia al pasar de una app a otra): las abiertas sin
     * anclar conservan su sitio según el orden en que aparecieron */
    static wchar_t order[MAX_ITEMS][MAX_PATH];
    static int norder;
    int rank[MAX_ITEMS];
    for (int i = pinned; i < D.count; ++i) {
        rank[i] = norder + i;
        for (int j = 0; j < norder; ++j) if (!lstrcmpiW(order[j], ItemKey(&D.items[i]))) { rank[i] = j; break; }
    }
    for (int i = pinned; i < D.count; ++i)
        for (int j = i + 1; j < D.count; ++j)
            if (rank[j] < rank[i]) {
                DockItem t = D.items[i]; D.items[i] = D.items[j]; D.items[j] = t;
                const int r = rank[i]; rank[i] = rank[j]; rank[j] = r;
            }
    norder = 0;
    for (int i = pinned; i < D.count && norder < MAX_ITEMS; ++i) lstrcpynW(order[norder++], ItemKey(&D.items[i]), MAX_PATH);
    for (int i = 0; i < D.count; ++i) {
        DockItem *it = &D.items[i];
        for (int j = 0; j < oldN; ++j)
            if (!lstrcmpiW(ItemKey(&keep[j]), ItemKey(it))) {
                it->scale = keep[j].scale; it->vel = keep[j].vel;
                it->hopT = keep[j].hopT; it->hops = keep[j].hops; it->hopH = keep[j].hopH;
                it->barW = keep[j].barW;
                break;
            }
    }
    UpdateActive();
    D.lastScan = GetTickCount();
}

/* ───────────────────────── Aspecto ───────────────────────── */
typedef struct { DWORD panel, bar; float opacity; BOOL light; } DockLook;

static void LoadDockLook(DockLook *l)
{
    Theme th;
    Theme_Load(&th);
    l->light = g_cfg.material == MAT_SYSTEM && !th.dark;
    /* mismo material que el notch y la barra: OLED negro puro, vidrio o el tema del sistema */
    if (l->light) { l->panel = 0xF2F2F7; l->bar = 0x1C1C1E; }
    else          { l->panel = g_cfg.material == MAT_OLED ? 0x000000 : 0x1C1C1E; l->bar = 0xFFFFFF; }
    l->opacity = kOpacity[max(0, min(3, g_cfg.dockOpacity))];
}

/* ───────────────────────── Disposición ─────────────────────────
 * La escala de cada icono sale de la distancia (en índices) al cursor, medida sobre la
 * retícula en reposo → estable. Las posiciones se recomponen con los anchos escalados. */
static float MagAt(int i, float fidx)
{
    const float di = (i - fidx) / SPREAD;
    return 1.0f + (MAXMAG - 1.0f) * expf(-di * di);
}

/* x (ventana) de la posición u (en "huecos": el icono i ocupa [i, i+1) con medio espacio a
 * cada lado) en el dock centrado, con las escalas que corresponden a magnificar en u. */
static float SlotX(float u)
{
    const float base = (float)Base(), gap = (float)DS(ICON_GAP), W = (float)(D.mon.right - D.mon.left);
    float total = (D.count - 1) * gap, off = -gap * 0.5f;
    const int k = max(0, min(D.count - 1, (int)u));
    for (int i = 0; i < D.count; ++i) {
        const float w = base * MagAt(i, u - 0.5f);
        total += w;
        if (i < k) off += w + gap;
        else if (i == k) off += (u - k) * (w + gap);
    }
    return (W - total) * 0.5f + off;
}

/* El dock se queda centrado y crece simétrico. Para que el icono que se agranda sea el
 * que está bajo el cursor, se busca la posición u cuya x, ya magnificada, cae en el cursor
 * (bisección: SlotX crece con u). */
static float SolveSlot(void)
{
    const float mx = D.mouseX + (D.mon.right - D.mon.left) * 0.5f;
    float lo = 0.0f, hi = (float)D.count;
    if (mx <= SlotX(lo)) return lo;
    if (mx >= SlotX(hi)) return hi;
    for (int it = 0; it < 24; ++it) {
        const float mid = (lo + hi) * 0.5f;
        if (SlotX(mid) < mx) lo = mid; else hi = mid;
    }
    return (lo + hi) * 0.5f;
}

static void TargetScales(float *out)
{
    const float fidx = D.inside ? SolveSlot() - 0.5f : 0.0f;
    for (int i = 0; i < D.count; ++i) out[i] = D.inside ? MagAt(i, fidx) : 1.0f;
}

/* x (ventana) del borde izquierdo del primer icono, con el dock centrado. */
static float FirstIconX(void)
{
    const float base = (float)Base(), gap = (float)DS(ICON_GAP);
    float total = (D.count - 1) * gap;
    for (int i = 0; i < D.count; ++i) total += base * D.items[i].scale;
    return ((D.mon.right - D.mon.left) - total) * 0.5f;
}

static float PanelH(void) { return (float)(Base() + 2 * DS(DOCK_PADY)); }
static float PanelBottom(void) { return (float)(WinH() - DS(BOTTOM_MARGIN)); }

/* Altura del salto: parábolas sucesivas que van perdiendo altura, como el Dock al abrir. */
#define HOP_PERIOD 0.40f
static float HopOffset(const DockItem *it)
{
    if (it->hopT < 0) return 0;
    const int k = (int)(it->hopT / HOP_PERIOD);
    if (k >= it->hops) return 0;
    const float p = it->hopT / HOP_PERIOD - k;
    return it->hopH * powf(0.62f, (float)k) * 4.0f * p * (1.0f - p);
}

/* ───────────────────────── Fondo desenfocado ───────────────────────── */
static BOOL CaptureBackdrop(void)
{
    const int y0 = max(0, (int)(PanelBottom() - PanelH()) - DS(16));
    /* solo la franja que hay detrás del panel (más el sitio para crecer al magnificar) */
    const int span = D.panelW > 0 ? D.panelW + DS(160) : D.frame.w;
    const int x0 = max(0, min(D.frame.w - 1, (D.frame.w - span) / 2));
    const int sw = max(1, min(D.frame.w - x0, span) / BACK_SCALE), sh = max(1, (WinH() - y0) / BACK_SCALE);
    if (D.raw.w != sw || D.raw.h != sh) {
        Canvas_Free(&D.raw);
        if (!Canvas_Init(&D.raw, sw, sh)) return FALSE;
        D.backHash = 0;
    }
    HDC screen = GetDC(NULL);
    SetStretchBltMode(D.raw.dc, COLORONCOLOR);       /* el desenfoque ya suaviza: sin HALFTONE */
    StretchBlt(D.raw.dc, 0, 0, sw, sh, screen, D.mon.left + x0, D.mon.bottom - WinH() + D.sinkPx + y0,
               sw * BACK_SCALE, sh * BACK_SCALE, SRCCOPY);
    ReleaseDC(NULL, screen);
    GdiFlush();
    DWORD hash = 2166136261u;
    for (int i = 0; i < sw * sh; ++i) hash = (hash ^ (D.raw.px[i] & 0xFFFFFF)) * 16777619u;
    D.backTick = GetTickCount();
    if (hash == D.backHash && D.back.px && D.back.w == sw && D.back.h == sh && D.backX == x0 && D.backY == y0)
        return FALSE;                                  /* lo de detrás no cambió */
    D.backHash = hash;
    if (D.back.w != sw || D.back.h != sh) {
        Canvas_Free(&D.back);
        if (!Canvas_Init(&D.back, sw, sh)) return FALSE;
    }
    D.backX = x0; D.backY = y0;
    D.gstripDirty = TRUE;                              /* el vidrio se vuelve a teñir */
    CopyMemory(D.back.px, D.raw.px, (SIZE_T)sw * sh * 4);
    const int r = max(1, DS(g_cfg.dockBlur == 2 ? 24 : 11) / BACK_SCALE);
    for (int i = 0; i < 2; ++i) Gfx_BoxBlur(D.back.px, sw, sh, r);
    /* vibrancia: algo más de saturación, como el vidrio de Apple */
    for (int i = 0; i < sw * sh; ++i) {
        const DWORD v = D.back.px[i];
        const int R = (v >> 16) & 255, G = (v >> 8) & 255, B = v & 255, Y = (R * 77 + G * 151 + B * 28) >> 8;
        D.back.px[i] = (DWORD)max(0, min(255, Y + (R - Y) * 3 / 2)) << 16 |
                       (DWORD)max(0, min(255, Y + (G - Y) * 3 / 2)) << 8 |
                       (DWORD)max(0, min(255, Y + (B - Y) * 3 / 2));
    }
    return TRUE;
}

static DWORD SampleBack(int x, int y)
{
    const float fx = (x - D.backX + 0.5f) / BACK_SCALE - 0.5f, fy = (y - D.backY + 0.5f) / BACK_SCALE - 0.5f;
    const int x0 = max(0, min(D.back.w - 1, (int)floorf(fx))), y0 = max(0, min(D.back.h - 1, (int)floorf(fy)));
    const int x1 = min(D.back.w - 1, x0 + 1), y1 = min(D.back.h - 1, y0 + 1);
    const float tx = max(0.0f, min(1.0f, fx - x0)), ty = max(0.0f, min(1.0f, fy - y0));
    const DWORD *p = D.back.px;
    return Gfx_Mix(Gfx_Mix(p[y0 * D.back.w + x0], p[y0 * D.back.w + x1], tx),
                   Gfx_Mix(p[y1 * D.back.w + x0], p[y1 * D.back.w + x1], tx), ty);
}

/* ───────────────────────── Dibujo ─────────────────────────
 * Se compone en color straight + alpha y se premultiplica al final, antes de presentar. */
/* El fotograma se compone directamente en BGRA premultiplicado (lo que pide
 * UpdateLayeredWindow): "encima" es src + dst·(1 − α), todo en enteros. */
static void PutPx(DWORD *dst, DWORD rgb, float a)
{
    const int A = (int)(a * 255.0f + 0.5f);
    if (A <= 0) return;
    const DWORD d = *dst;
    const DWORD ia = 255 - A;
    const DWORD rb = ((((rgb & 0xFF00FF) * A) + ((d & 0xFF00FF) * ia) + 0x800080) >> 8) & 0xFF00FF;
    const DWORD g  = ((((rgb & 0x00FF00) * A) + ((d & 0x00FF00) * ia) + 0x008000) >> 8) & 0x00FF00;
    const DWORD al = (DWORD)A + (((d >> 24) * ia + 127) / 255);
    *dst = min(255u, al) << 24 | rb | g;
}

/* Icono escalado: bilineal en punto fijo sobre píxeles ya premultiplicados (de la caché). */
static void BlitIcon(Canvas *f, const DockItem *it, float cx, float bottom, float size)
{
    if (!it->px || size < 1) return;
    const float hw = size * 0.5f, left = cx - hw, topf = bottom - size;
    const int x0 = max(0, (int)floorf(left)), x1 = min(f->w, (int)ceilf(cx + hw));
    const int y0 = max(0, (int)floorf(topf)), y1 = min(f->h, (int)ceilf(bottom));
    const float inv = (float)it->iw / size;
    const int step = (int)(inv * 65536.0f), W = it->iw, H = it->ih;
    for (int y = y0; y < y1; ++y) {
        const int sy = (int)(((y + 0.5f - topf) * inv - 0.5f) * 65536.0f);
        const int iy = sy >> 16, fy = (sy >> 8) & 255;
        if (iy < -1 || iy >= H) continue;
        const DWORD *ra = &it->px[max(0, iy) * W], *rbw = &it->px[min(H - 1, iy + 1) * W];
        DWORD *out = &f->px[y * f->w];
        int sx = (int)(((x0 + 0.5f - left) * inv - 0.5f) * 65536.0f);
        for (int x = x0; x < x1; ++x, sx += step) {
            const int ix = sx >> 16, fx = (sx >> 8) & 255;
            if (ix < -1 || ix >= W) continue;
            const int xa = max(0, ix), xb = min(W - 1, ix + 1);
            const DWORD p0 = ra[xa], p1 = ra[xb], p2 = rbw[xa], p3 = rbw[xb];
            if (!(p0 | p1 | p2 | p3)) continue;
            /* dos canales a la vez: (a, g) y (r, b) */
            const DWORD t0 = (((p0 >> 8) & 0xFF00FF) * (256 - fx) + ((p1 >> 8) & 0xFF00FF) * fx) >> 8 & 0xFF00FF;
            const DWORD t1 = (((p2 >> 8) & 0xFF00FF) * (256 - fx) + ((p3 >> 8) & 0xFF00FF) * fx) >> 8 & 0xFF00FF;
            const DWORD u0 = ((p0 & 0xFF00FF) * (256 - fx) + (p1 & 0xFF00FF) * fx) >> 8 & 0xFF00FF;
            const DWORD u1 = ((p2 & 0xFF00FF) * (256 - fx) + (p3 & 0xFF00FF) * fx) >> 8 & 0xFF00FF;
            const DWORD ag = ((t0 * (256 - fy) + t1 * fy) >> 8) & 0xFF00FF;
            const DWORD rb = ((u0 * (256 - fy) + u1 * fy) >> 8) & 0xFF00FF;
            const DWORD c = ag << 8 | rb, ca = c >> 24;
            if (!ca) continue;
            const DWORD d = out[x], ia = 255 - ca;
            if (!ia) { out[x] = c; continue; }
            const DWORD drb = (((d & 0xFF00FF) * ia + 0x800080) >> 8) & 0xFF00FF;
            const DWORD dag = ((((d >> 8) & 0xFF00FF) * ia + 0x800080) >> 8) & 0xFF00FF;
            out[x] = c + (dag << 8 | drb);
        }
    }
}

static void FrameRRect(Canvas *f, float x, float y, float w, float h, float r, DWORD rgb, float alpha)
{
    const int x0 = max(0, (int)floorf(x) - 1), x1 = min(f->w, (int)ceilf(x + w) + 1);
    const int y0 = max(0, (int)floorf(y) - 1), y1 = min(f->h, (int)ceilf(y + h) + 1);
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px) {
            const float cov = Gfx_Cov(Gfx_SdRRect(px + 0.5f, py + 0.5f, x, y, w, h, r)) * alpha;
            if (cov > 0.002f) PutPx(&f->px[py * f->w + px], rgb, cov);
        }
}

/* Panel con vidrio: el fondo desenfocado teñido con el color del panel según la opacidad. */
/* Panel de vidrio. El color (fondo desenfocado teñido) se precalcula una vez por captura en
 * una franja; en cada fotograma solo se recorta con las esquinas (SDF solo en los bordes). */
static void GlassRRect(Canvas *f, float x, float y, float w, float h, float r, DWORD tint, float op)
{
    const int gy0 = max(0, (int)floorf(y) - 1), gh = min(f->h - gy0, (int)ceilf(h) + 3);
    if (D.gstrip.w != f->w || D.gstrip.h != gh || D.gstripKey != (tint ^ (DWORD)(op * 1000) ^ (DWORD)gy0 << 20) || D.gstripDirty) {
        if (D.gstrip.w != f->w || D.gstrip.h != gh) { Canvas_Free(&D.gstrip); if (!Canvas_Init(&D.gstrip, f->w, gh)) return; }
        GdiFlush();
        for (int yy = 0; yy < gh; ++yy)
            for (int xx = 0; xx < f->w; ++xx)
                D.gstrip.px[yy * f->w + xx] = Gfx_Mix(SampleBack(xx, gy0 + yy), tint, op) & 0xFFFFFF;
        D.gstripKey = tint ^ (DWORD)(op * 1000) ^ (DWORD)gy0 << 20;
        D.gstripDirty = FALSE;
    }
    const int x0 = max(0, (int)floorf(x) - 1), x1 = min(f->w, (int)ceilf(x + w) + 1);
    const int y0 = max(gy0, (int)floorf(y) - 1), y1 = min(gy0 + gh, (int)ceilf(y + h) + 1);
    for (int py = y0; py < y1; ++py) {
        const DWORD *g = &D.gstrip.px[(py - gy0) * f->w];
        DWORD *out = &f->px[py * f->w];
        const float cy = py + 0.5f;
        const BOOL midRow = cy > y + r && cy < y + h - r;
        const BOOL inRow = cy > y + 1.0f && cy < y + h - 1.0f;
        for (int px = x0; px < x1; ++px) {
            const float cx = px + 0.5f;
            if (inRow && ((cx > x + r && cx < x + w - r) || (midRow && cx > x + 1.0f && cx < x + w - 1.0f))) {
                out[px] = 0xFF000000 | g[px];               /* interior: copia directa */
                continue;
            }
            const float cov = Gfx_Cov(Gfx_SdRRect(cx, cy, x, y, w, h, r));
            if (cov > 0.002f) PutPx(&out[px], g[px], cov);
        }
    }
}


static void Render(void)
{
    if (D.frame.w != D.mon.right - D.mon.left || D.frame.h != WinH()) {
        Canvas_Free(&D.frame);
        if (!Canvas_Init(&D.frame, D.mon.right - D.mon.left, WinH())) return;
    }
    Canvas *f = &D.frame;
    GdiFlush();
    ZeroMemory(f->px, (SIZE_T)f->w * f->h * 4);
    int top = f->h;
    if (D.count < 1) goto present;

    DockLook L;
    LoadDockLook(&L);
    const float base = (float)Base(), gap = (float)DS(ICON_GAP);
    float total = 0;
    for (int i = 0; i < D.count; ++i) total += base * D.items[i].scale + (i ? gap : 0);
    const float padx = (float)DS(DOCK_PADX), pady = (float)DS(DOCK_PADY);
    const float panelW = total + 2 * padx, panelH = PanelH();
    const float px = FirstIconX() - padx;
    const float panelBottom = PanelBottom(), panelTop = panelBottom - panelH;
    const float rad = panelH * 0.30f;

    if (g_cfg.dockBlur) {
        /* durante la animación no se recaptura (daría tirones); el temporizador lo
         * refresca en cuanto el dock queda quieto */
        if (!D.back.px) CaptureBackdrop();
        if (D.back.px) GlassRRect(f, px, panelTop, panelW, panelH, rad, L.panel, L.opacity);
    } else {
        FrameRRect(f, px, panelTop, panelW, panelH, rad, L.panel, L.opacity);
    }
    top = (int)panelTop;

    /* iconos sobre la línea base inferior del panel */
    const float baseline = panelBottom - pady;
    float x = px + padx;
    for (int i = 0; i < D.count; ++i) {
        DockItem *it = &D.items[i];
        const float s = base * it->scale;
        const float cx = x + s * 0.5f, b = baseline - HopOffset(it);
        BlitIcon(f, it, cx, b, s);
        top = min(top, (int)(b - s));
        /* barrita de "abierta": corta y tenue; larga y brillante si está al frente */
        if (it->barW > 0.5f) {
            const float bh = max(2.0f, (float)DS(3)), by = panelBottom - pady * 0.5f - bh * 0.5f;
            const float al = it->active ? 0.95f : 0.55f;
            if (it->nwin >= 2) {
                /* varias ventanas: un segmento por ventana (hasta 3); solo el de la que
                 * está al frente va en blanco, el resto en gris */
                const int ns = min(it->nwin, 3), on = it->fgSeg < 0 ? -1 : min(it->fgSeg, ns - 1);
                const float g2 = (float)DS(3), sw = max(bh, (it->barW - g2 * (ns - 1)) / ns);
                float sx = cx - (sw * ns + g2 * (ns - 1)) * 0.5f;
                for (int k = 0; k < ns; ++k, sx += sw + g2)
                    FrameRRect(f, sx, by, sw, bh, bh * 0.5f, L.bar, k == on ? 0.95f : 0.38f);
            } else {
                FrameRRect(f, cx - it->barW * 0.5f, by, it->barW, bh, bh * 0.5f, L.bar, al);
            }
        }
        x += s + gap;
    }

    D.iconTop = (int)panelTop;
    for (int i = 0; i < D.count; ++i) {
        const float s = base * D.items[i].scale;
        D.iconTop = min(D.iconTop, (int)(baseline - HopOffset(&D.items[i]) - s));
    }
    D.panelX = (int)px; D.panelW = (int)panelW;
    D.panelY = (int)panelTop; D.panelH = (int)panelH;

present:;
    /* zona con contenido este fotograma: el panel con sitio para la magnificación */
    RECT dirty = { 0, max(0, top - 2), f->w, f->h };
    if (D.count > 0 && D.panelW > 0) { dirty.left = max(0, D.panelX - DS(12)); dirty.right = min(f->w, D.panelX + D.panelW + DS(12)); }

    /* con Inicio abierto el dock baja y se desvanece para no chocar con el menú */
    const float e = D.slide * D.slide * (3 - 2 * D.slide);
    POINT dst = { D.mon.left, D.mon.bottom - WinH() + D.sinkPx + (int)(e * (WinH() - D.panelY + DS(6))) }, src = { 0, 0 };
    SIZE sz = { f->w, f->h };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)(255 * (1 - e) + 0.5f), AC_SRC_ALPHA };
    /* a DWM solo se le manda lo que cambió (lo de ahora y lo del fotograma anterior) */
    RECT upd = dirty;
    if (D.dirtyPrev.right > D.dirtyPrev.left) UnionRect(&upd, &upd, &D.dirtyPrev);
    D.dirtyPrev = dirty;
    HDC screen = GetDC(NULL);
    UPDATELAYEREDWINDOWINFO ui = { sizeof(ui) };
    ui.hdcDst = screen; ui.pptDst = &dst; ui.psize = &sz; ui.hdcSrc = f->dc; ui.pptSrc = &src;
    ui.pblend = &bf; ui.dwFlags = ULW_ALPHA; ui.prcDirty = &upd;
    if (!UpdateLayeredWindowIndirect(D.hwnd, &ui)) {
        ui.prcDirty = NULL;                            /* primera vez o cambio de tamaño */
        UpdateLayeredWindowIndirect(D.hwnd, &ui);
    }
    ReleaseDC(NULL, screen);
}

/* ───────────────────────── Animación ───────────────────────── */
static void Spring2(float *x, float *v, float target, float dt, float k, float z)
{
    const float a = (target - *x) * k - *v * 2.0f * sqrtf(k) * z;
    *v += a * dt; *x += *v * dt;
}

static int HitIndex(int mx)
{
    /* mx en coords de ventana; se busca el icono cuyo rango horizontal lo contiene */
    const float base = (float)Base(), gap = (float)DS(ICON_GAP);
    float x = FirstIconX();
    for (int i = 0; i < D.count; ++i) {
        const float s = base * D.items[i].scale;
        if (mx >= x - gap * 0.5f && mx < x + s + gap * 0.5f) return i;
        x += s + gap;
    }
    return -1;
}

static float BarTarget(const DockItem *it)
{
    const BOOL multi = it->nwin >= 2;      /* varias ventanas: sitio para un segmento por ventana */
    return it->active ? (float)DS(multi ? 24 : 16) : it->running ? (float)DS(multi ? 14 : 6) : 0.0f;
}

static void Tick(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    float dt = (float)(t.QuadPart - D.last.QuadPart) / (float)D.freq.QuadPart;
    D.last = t;
    if (dt > 0.05f) dt = 0.05f;

    float tg[MAX_ITEMS];
    TargetScales(tg);
    BOOL busy = FALSE;
    for (int i = 0; i < D.count; ++i) {
        DockItem *it = &D.items[i];
        const float bs = it->scale;
        for (int k = 0; k < 2; ++k) Spring2(&it->scale, &it->vel, tg[i], dt * 0.5f, 520.0f, 0.66f);
        if (fabsf(it->scale - bs) > 0.0004f) busy = TRUE;
        if (it->hopT >= 0) {
            it->hopT += dt;
            if (it->hopT >= HOP_PERIOD * it->hops) it->hopT = -1.0f;
            else busy = TRUE;
        }
        const float bt = BarTarget(it), d = bt - it->barW;
        if (fabsf(d) > 0.3f) { it->barW += d * min(1.0f, dt * 14.0f); busy = TRUE; }
        else it->barW = bt;
    }
    const float st = D.shellOpen ? 1.0f : 0.0f;
    if (fabsf(st - D.slide) > 0.004f) { D.slide += (st - D.slide) * min(1.0f, dt * 11.0f); busy = TRUE; }
    else D.slide = st;
    /* ocultar dock: sin el cursor baja hasta que su mitad queda bajo la pantalla; al
     * acercarlo sube con el rebote configurado */
    const BOOL sunk = g_cfg.dockAutoHide && !D.inside && !D.menuOpen && GetTickCount() - D.leaveAt >= SINK_DELAY;
    const float kt = sunk ? 1.0f : 0.0f, z = sunk ? 0.95f : min(1.0f, Pop_Zeta() + 0.12f);
    if (fabsf(kt - D.sink) > 0.002f || fabsf(D.sinkV) > 0.02f) {
        for (int k = 0; k < 2; ++k) Spring2(&D.sink, &D.sinkV, kt, dt * 0.5f, sunk ? 140.0f : 260.0f, z);
        busy = TRUE;
    } else if (D.sink != kt) {
        D.sink = kt; D.sinkV = 0;
        if (g_cfg.dockBlur) SetTimer(D.hwnd, TIMER_DBLURQ, 30, NULL);    /* el vidrio, desde su sitio nuevo */
    }
    D.sinkPx = (int)lroundf(D.sink * (DS(BOTTOM_MARGIN) + PanelH() * 0.5f));
    /* con el cursor quieto encima ya no se redibuja 120 veces por segundo: solo si algo
     * se movió (o lo pidió un clic), y el marcapasos se para hasta el próximo movimiento */
    if (busy || D.dirty) { Render(); D.dirty = FALSE; }
    if (!busy) PacerOn(FALSE);
}

static void Kick(void)
{
    QueryPerformanceCounter(&D.last);
    D.dirty = TRUE;
    PacerOn(TRUE);
}

/* ───────────────────────── Acciones ───────────────────────── */
static void StartHop(DockItem *it, BOOL launching)
{
    /* al abrir: tres saltos altos que se van apagando; al enfocar: uno más corto */
    it->hops = launching ? 3 : 1;
    it->hopH = (float)Base() * (launching ? 0.75f : 0.38f);
    it->hopT = 0;
    Kick();
}

static void Launch(const DockItem *it)
{
    /* abrir vía explorer: ninguna extensión de shell entra en nuestro proceso */
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 16];
    GetWindowsDirectoryW(exe, MAX_PATH - 16);
    lstrcatW(exe, L"\\explorer.exe");
    wsprintfW(cmd, L"\"%s\" \"%s\"", exe, it->launch);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
}

static void FocusWindow(HWND w)
{
    if (!w || !IsWindow(w)) return;
    if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
    SetForegroundWindow(w);
}

/* Clic en una app:
 * - sin ventanas: se abre;
 * - una ventana: al frente, o se minimiza si ya lo estaba;
 * - varias: si la app no está al frente, trae la más reciente; si ya lo está, pasa a la
 *   siguiente (la que lleva más tiempo detrás), como recorrerlas con el Dock de macOS. */
static void Activate(int i)
{
    if (i < 0 || i >= D.count) return;
    DockItem *it = &D.items[i];
    int n = 0;
    for (int k = 0; k < it->nwin; ++k) if (IsWindow(it->wins[k])) it->wins[n++] = it->wins[k];
    it->nwin = n;

    if (!n) { StartHop(it, TRUE); Launch(it); return; }
    HWND fg = GetForegroundWindow();
    if (fg) fg = GetAncestor(fg, GA_ROOTOWNER);
    BOOL front = FALSE;
    for (int k = 0; k < n; ++k) if (it->wins[k] == fg) front = TRUE;

    if (n == 1) {
        if (front && !IsIconic(fg)) ShowWindow(fg, SW_MINIMIZE);
        else { StartHop(it, FALSE); FocusWindow(it->wins[0]); }
        return;
    }
    StartHop(it, FALSE);
    FocusWindow(front ? it->wins[n - 1] : it->wins[0]);    /* wins[] va en orden z: [0] arriba */
}

/* Clic derecho en una app: sus ventanas para elegir, y abrir otra o cerrarlas. */
#define IDM_WIN0   100
#define IDM_NEW    200
#define IDM_CLOSE  201
#define IDM_PREFS  202
static void AppMenu(int i, POINT at)
{
    DockItem *it = &D.items[i];
    HMENU m = CreatePopupMenu();
    if (!m) return;
    HWND fg = GetForegroundWindow();
    if (fg) fg = GetAncestor(fg, GA_ROOTOWNER);
    int shown = 0;
    for (int k = 0; k < it->nwin; ++k) {
        if (!IsWindow(it->wins[k])) continue;
        wchar_t t[64], esc[130];
        if (GetWindowTextW(it->wins[k], t, 64) <= 0) lstrcpyW(t, it->name);
        int o = 0;                               /* "&" es atajo en los menús: duplicarlo */
        for (const wchar_t *q = t; *q && o < 126; ++q) { if (*q == L'&') esc[o++] = L'&'; esc[o++] = *q; }
        esc[o] = 0;
        AppendMenuW(m, MF_STRING | (it->wins[k] == fg ? MF_CHECKED : 0), IDM_WIN0 + k, esc);
        ++shown;
    }
    if (shown) AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_NEW, shown ? L"Nueva ventana" : L"Abrir");
    if (shown) AppendMenuW(m, MF_STRING, IDM_CLOSE, shown > 1 ? L"Cerrar todas las ventanas" : L"Cerrar ventana");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_PREFS, L"Ajustes del dock\x2026");

    SetForegroundWindow(D.hwnd);                 /* para que el menú se cierre al hacer clic fuera */
    D.menuOpen = TRUE;                           /* con el menú abierto el dock no se esconde */
    const int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_BOTTOMALIGN | TPM_CENTERALIGN | TPM_RIGHTBUTTON,
                                        at.x, at.y, 0, D.hwnd, NULL);
    D.menuOpen = FALSE;
    D.leaveAt = GetTickCount();
    if (g_cfg.dockAutoHide) SetTimer(D.hwnd, TIMER_DSINK, SINK_DELAY, NULL);
    DestroyMenu(m);
    PostMessageW(D.hwnd, WM_NULL, 0, 0);
    if (cmd >= IDM_WIN0 && cmd < IDM_WIN0 + it->nwin) { StartHop(it, FALSE); FocusWindow(it->wins[cmd - IDM_WIN0]); }
    else if (cmd == IDM_NEW)   { StartHop(it, TRUE); Launch(it); }
    else if (cmd == IDM_CLOSE) { for (int k = 0; k < it->nwin; ++k) if (IsWindow(it->wins[k])) PostMessageW(it->wins[k], WM_CLOSE, 0, 0); }
    else if (cmd == IDM_PREFS) Panel_ShowTab(2);
}

/* ───────────────────────── Barra de tareas de Windows ─────────────────────────
 * Se pone en autoocultar (para que el área de trabajo ocupe toda la pantalla) y además
 * se ocultan sus ventanas, así ni asoma al llevar el cursor abajo. Al salir se restaura
 * el estado previo, que queda guardado en el registro por si el proceso muere. */
static BOOL CALLBACK ShowTrayProc(HWND w, LPARAM show)
{
    wchar_t cls[40];
    if (GetClassNameW(w, cls, 40) && (!lstrcmpW(cls, L"Shell_TrayWnd") || !lstrcmpW(cls, L"Shell_SecondaryTrayWnd"))) {
        if (show && !IsWindowVisible(w)) ShowWindow(w, SW_SHOWNA);
        else if (!show && IsWindowVisible(w)) ShowWindow(w, SW_HIDE);
    }
    return TRUE;
}

static void SetTaskbarAutohide(BOOL hide)
{
    APPBARDATA abd = { sizeof(abd) };
    abd.hWnd = FindWindowW(L"Shell_TrayWnd", NULL);
    if (!abd.hWnd) return;
    const BOOL already = RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"TaskbarWasAuto", RRF_RT_REG_DWORD, NULL, NULL, NULL) == ERROR_SUCCESS;
    if (hide == already) return;
    if (hide) {
        const UINT prev = (UINT)SHAppBarMessage(ABM_GETSTATE, &abd);
        HKEY k;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
            RegSetValueExW(k, L"TaskbarWasAuto", 0, REG_DWORD, (const BYTE *)&prev, sizeof(prev));
            RegCloseKey(k);
        }
        abd.lParam = ABS_AUTOHIDE | ABS_ALWAYSONTOP;
        SHAppBarMessage(ABM_SETSTATE, &abd);
    } else {
        DWORD prev = ABS_ALWAYSONTOP, sz = sizeof(prev);
        RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"TaskbarWasAuto", RRF_RT_REG_DWORD, NULL, &prev, &sz);
        abd.lParam = prev;
        SHAppBarMessage(ABM_SETSTATE, &abd);
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            RegDeleteValueW(k, L"TaskbarWasAuto");
            RegCloseKey(k);
        }
    }
}

static void SetTaskbarOff(BOOL off)
{
    SetTaskbarAutohide(off);
    EnumWindows(ShowTrayProc, !off);
}

void Dock_RestoreTaskbar(void) { SetTaskbarOff(FALSE); }

/* La bandeja de Windows (iconos de Tailscale, OneDrive…) vive en la barra de tareas: para
 * usarla se deja ver la barra un momento; al irse el foco de ella, el dock la vuelve a ocultar. */
void Dock_TrayPeek(BOOL on)
{
    if (!D.hwnd || !g_cfg.dockHideTaskbar) return;
    D.peek = on;
    D.peekAt = GetTickCount();
    if (on) {
        EnumWindows(ShowTrayProc, TRUE);
        SetTimer(D.hwnd, TIMER_DPEEK, 700, NULL);
    }
}

/* ───────────────────────── Ventana ───────────────────────── */
static void DockMeasure(void)
{
    HMONITOR m = MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(m, &mi);
    D.mon = mi.rcMonitor;
    const UINT dpi = MonitorDpi(m);
    if (dpi != D.dpi) ClearIconCache();
    D.dpi = dpi;
}

/* Reserva media altura del dock: las ventanas maximizadas llegan hasta la mitad del panel
 * (no quedan tapadas detrás de todo el dock ni dejan una franja vacía debajo). */
static void AppBarPos(void)
{
    if (!D.appbar) return;
    /* oculto: las ventanas usan toda la altura y el dock asoma por encima */
    const int reserve = g_cfg.dockAutoHide ? 0 : DS(BOTTOM_MARGIN) + (int)(PanelH() * 0.5f);
    APPBARDATA abd = { sizeof(abd) };
    abd.hWnd = D.hwnd;
    abd.uEdge = ABE_BOTTOM;
    abd.rc = D.mon;
    abd.rc.top = D.mon.bottom - reserve;
    SHAppBarMessage(ABM_QUERYPOS, &abd);
    abd.rc.top = abd.rc.bottom - reserve;
    SHAppBarMessage(ABM_SETPOS, &abd);
}

static void CALLBACK WinHook(HWINEVENTHOOK hk, DWORD ev, HWND w, LONG obj, LONG child, DWORD th, DWORD t);

static void ApplyCapture(void)
{
    /* con desenfoque el dock debe quedar fuera de captura: si no, se vería a sí mismo */
    const BOOL exclude = g_cfg.hideCapture || g_cfg.dockBlur;
    if (D.hwnd) SetWindowDisplayAffinity(D.hwnd, exclude ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
    if (D.hwnd) {
        if (g_cfg.dockBlur) {
            SetTimer(D.hwnd, TIMER_DBLUR, 3000, NULL);
            if (!D.moveHook)
                D.moveHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, NULL, WinHook, 0, 0,
                                             WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        } else {
            KillTimer(D.hwnd, TIMER_DBLUR);
            if (D.moveHook) { UnhookWinEvent(D.moveHook); D.moveHook = NULL; }
        }
    }
}

static void CALLBACK FgHook(HWINEVENTHOOK hk, DWORD ev, HWND w, LONG obj, LONG child, DWORD th, DWORD t)
{
    (void)hk; (void)ev; (void)obj; (void)child; (void)th; (void)t;
    if (!D.hwnd || D.fullscreen) return;
    /* ¿se abrió Inicio o Buscar? Windows lo dibuja pegado a su barra y no se puede mover */
    wchar_t path[MAX_PATH];
    const BOOL shell = w && WindowExe(w, path) &&
                       (!lstrcmpiW(BaseName(path), L"SearchHost.exe") || !lstrcmpiW(BaseName(path), L"StartMenuExperienceHost.exe"));
    if (D.shellOpen && !shell && g_cfg.dockHideTaskbar) {
        /* la barra asomó con Inicio y Explorer la vuelve a mostrar un instante después
         * de cerrarlo: se vigila unos segundos para ocultarla en cuanto aparezca */
        SetTaskbarOff(TRUE);
        D.taskWatch = 16;
        SetTimer(D.hwnd, TIMER_DTASK, 120, NULL);
    }
    D.shellOpen = shell;
    if (shell) { D.inside = FALSE; D.hot = -1; }
    else Rescan();      /* barato: los iconos vienen de la caché */
    Kick();
}

/* Avisos de Windows sobre ventanas. Solo se mira lo que importa al dock, y se agrupa:
 * el repaso (o la recaptura del vidrio) va un instante después de la ráfaga. */
static BOOL KnownWindow(HWND w)
{
    for (int i = 0; i < D.count; ++i)
        for (int k = 0; k < D.items[i].nwin; ++k) if (D.items[i].wins[k] == w) return TRUE;
    return FALSE;
}

static void CALLBACK WinHook(HWINEVENTHOOK hk, DWORD ev, HWND w, LONG obj, LONG child, DWORD th, DWORD t)
{
    (void)hk; (void)th; (void)t;
    if (!D.hwnd || obj != OBJID_WINDOW || child != CHILDID_SELF || !w) return;
    if (ev == EVENT_OBJECT_LOCATIONCHANGE) {               /* algo se movió: quizá detrás del dock */
        if (GetAncestor(w, GA_ROOT) == w && IsWindowVisible(w)) SetTimer(D.hwnd, TIMER_DBLURQ, 160, NULL);
        return;
    }
    if (ev == EVENT_OBJECT_SHOW) {
        wchar_t cls[32];
        if (GetClassNameW(w, cls, 32) && (!lstrcmpW(cls, L"Shell_TrayWnd") || !lstrcmpW(cls, L"Shell_SecondaryTrayWnd"))) {
            /* Explorer volvió a mostrar su barra: fuera, salvo con Inicio o la bandeja abiertos */
            if (g_cfg.dockHideTaskbar && !D.shellOpen && !D.peek) SetTimer(D.hwnd, TIMER_DTASK, 60, NULL), D.taskWatch = 3;
            return;
        }
        if (IsAppWindow(w)) SetTimer(D.hwnd, TIMER_DRESCAN, 150, NULL);
        if (g_cfg.dockBlur) SetTimer(D.hwnd, TIMER_DBLURQ, 200, NULL);
        return;
    }
    /* se ocultó o se cerró: solo importa si era de una app del dock */
    if (KnownWindow(w)) SetTimer(D.hwnd, TIMER_DRESCAN, 150, NULL);
    if (g_cfg.dockBlur && ev == EVENT_OBJECT_HIDE) SetTimer(D.hwnd, TIMER_DBLURQ, 200, NULL);
}

static LRESULT CALLBACK DockProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_NCHITTEST: {
        if (D.slide > 0.3f) return HTTRANSPARENT;      /* escondido mientras Inicio está abierto */
        POINT p = { (short)LOWORD(l), (short)HIWORD(l) };
        ScreenToClient(h, &p);
        /* con el cursor dentro, la zona activa sube hasta los iconos magnificados: si no,
         * al llegar a la parte alta de un icono grande se "salía", los iconos encogían,
         * el cursor volvía a quedar dentro… y el hover parpadeaba */
        const int top = D.inside ? min(D.iconTop - DS(6), D.panelY - DS(10)) : D.panelY - DS(10);
        RECT band = { D.panelX - DS(40), top, D.panelX + D.panelW + DS(40), WinH() };
        return PtInRect(&band, p) ? HTCLIENT : HTTRANSPARENT;
    }
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_DOCK_APPBAR:
        if (w == ABN_POSCHANGED) AppBarPos();
        else if (w == ABN_FULLSCREENAPP && (BOOL)l != D.fullscreen) {
            D.fullscreen = (BOOL)l;
            ShowWindow(h, D.fullscreen ? SW_HIDE : SW_SHOWNOACTIVATE);
            if (!D.fullscreen) { Rescan(); Kick(); }
        }
        return 0;
    case WM_WINDOWPOSCHANGED: {
        APPBARDATA abd = { sizeof(abd) };
        abd.hWnd = h;
        if (D.appbar) SHAppBarMessage(ABM_WINDOWPOSCHANGED, &abd);
        break;
    }
    case WM_DFRAME:
        InterlockedExchange(&s_dframe, 0);
        if (D.hwnd) Tick();
        return 0;
    case WM_MOUSEMOVE: {
        if (!D.inside) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            D.inside = TRUE;
            if (GetTickCount() - D.lastScan > 500) Rescan();
        }
        const int mx = (short)LOWORD(l);
        D.mouseX = (float)(mx - D.frame.w / 2);
        D.hot = HitIndex(mx);
        Kick();
        return 0;
    }
    case WM_MOUSELEAVE:
        D.inside = FALSE;
        D.hot = -1;
        D.leaveAt = GetTickCount();
        if (g_cfg.dockAutoHide) SetTimer(h, TIMER_DSINK, SINK_DELAY, NULL);
        Kick();             /* deja que las escalas vuelvan a 1 y luego se detiene */
        return 0;
    case WM_LBUTTONDOWN:
        D.pressed = HitIndex((short)LOWORD(l));
        return 0;
    case WM_LBUTTONUP: {
        const int hit = HitIndex((short)LOWORD(l));
        if (hit >= 0 && hit == D.pressed) Activate(hit);
        D.pressed = -1;
        return 0;
    }
    case WM_RBUTTONUP: {    /* en un icono: sus ventanas; fuera: ajustes del dock */
        const int hit = HitIndex((short)LOWORD(l));
        if (hit < 0) { Panel_ShowTab(2); return 0; }
        RECT wr;
        GetWindowRect(h, &wr);
        POINT at = { wr.left + (short)LOWORD(l), wr.top + D.iconTop - DS(6) };
        AppMenu(hit, at);
        return 0;
    }
    case WM_TIMER:
        if (w == TIMER_DSTATUS) {
            QUERY_USER_NOTIFICATION_STATE qs;
            BOOL fs = SUCCEEDED(SHQueryUserNotificationState(&qs)) &&
                      (qs == QUNS_BUSY || qs == QUNS_RUNNING_D3D_FULL_SCREEN || qs == QUNS_PRESENTATION_MODE);
            if (fs != D.fullscreen) {
                D.fullscreen = fs;
                ShowWindow(h, fs ? SW_HIDE : SW_SHOWNOACTIVATE);
                if (!fs) { Rescan(); Render(); }
            }
            if (!fs && !D.inside) { Rescan(); Kick(); }   /* repaso de seguridad */
            if (g_cfg.dockHideTaskbar && !D.shellOpen && !D.peek) SetTaskbarOff(TRUE);
        } else if (w == TIMER_DRESCAN) {
            KillTimer(h, TIMER_DRESCAN);
            if (!D.fullscreen) { Rescan(); Kick(); }
        } else if (w == TIMER_DBLURQ) {
            KillTimer(h, TIMER_DBLURQ);
            if (!D.fullscreen && WaitForSingleObject(s_dpOn, 0) != WAIT_OBJECT_0 && CaptureBackdrop()) Render();
        } else if (w == TIMER_DSINK) {
            KillTimer(h, TIMER_DSINK);
            Kick();
        } else if (w == TIMER_DPEEK) {
            if (D.peek && GetTickCount() - D.peekAt > 1500) {
                /* fin de la visita a la bandeja: el foco ya no está en la barra ni en su panel */
                HWND fg = GetForegroundWindow();
                wchar_t cls[64] = L"";
                if (fg) GetClassNameW(fg, cls, 64);
                const BOOL tray = !lstrcmpW(cls, L"Shell_TrayWnd") || !lstrcmpW(cls, L"TopLevelWindowForOverflowXamlIsland") ||
                                  !lstrcmpW(cls, L"NotifyIconOverflowWindow") || !lstrcmpW(cls, L"Xaml_WindowedPopupClass");
                if (!tray || GetTickCount() - D.peekAt > 60000) D.peek = FALSE;
            }
            if (!D.peek) {
                KillTimer(h, TIMER_DPEEK);
                if (g_cfg.dockHideTaskbar && !D.shellOpen) SetTaskbarOff(TRUE);
            }
        } else if (w == TIMER_DTASK) {
            if (!D.shellOpen && !D.peek && g_cfg.dockHideTaskbar) SetTaskbarOff(TRUE);
            if (--D.taskWatch <= 0 || D.shellOpen) KillTimer(h, TIMER_DTASK);
        } else if (w == TIMER_DBLUR) {
            /* el fondo cambió (ventana movida, vídeo…): recomponer el vidrio */
            if (!D.fullscreen && WaitForSingleObject(s_dpOn, 0) != WAIT_OBJECT_0 && CaptureBackdrop()) Render();
        }
        return 0;
    case WM_DESTROY:
        KillTimer(h, TIMER_DSTATUS);
        KillTimer(h, TIMER_DBLUR);
        KillTimer(h, TIMER_DTASK);
        KillTimer(h, TIMER_DRESCAN);
        KillTimer(h, TIMER_DBLURQ);
        KillTimer(h, TIMER_DPEEK);
        KillTimer(h, TIMER_DSINK);
        if (D.winHook) { UnhookWinEvent(D.winHook); D.winHook = NULL; }
        if (D.moveHook) { UnhookWinEvent(D.moveHook); D.moveHook = NULL; }
        if (D.fgHook) { UnhookWinEvent(D.fgHook); D.fgHook = NULL; }
        if (D.appbar) {
            APPBARDATA abd = { sizeof(abd) };
            abd.hWnd = h;
            SHAppBarMessage(ABM_REMOVE, &abd);      /* las ventanas recuperan el espacio */
            D.appbar = FALSE;
        }
        ClearIconCache();
        D.count = 0;
        Canvas_Free(&D.frame);
        Canvas_Free(&D.back);
        Canvas_Free(&D.raw);
        Canvas_Free(&D.gstrip);
        ZeroMemory(&D.dirtyPrev, sizeof(D.dirtyPrev));
        D.hwnd = NULL;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ───────────────────────── API ───────────────────────── */
void Dock_Register(void)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = DockProc;
    wc.hInstance     = g_inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = DOCK_CLASS;
    RegisterClassExW(&wc);
}

void Dock_Apply(void)
{
    if (!g_cfg.dock) {
        if (D.hwnd) DestroyWindow(D.hwnd);
        Dock_RestoreTaskbar();
        return;
    }
    if (!D.hwnd) {
        DockMeasure();
        D.hot = D.pressed = -1;
        QueryPerformanceFrequency(&D.freq);
        QueryPerformanceCounter(&D.last);
        D.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                 DOCK_CLASS, L"CornerRadius Dock", WS_POPUP,
                                 D.mon.left, D.mon.bottom - WinH(), D.mon.right - D.mon.left, WinH(),
                                 NULL, NULL, g_inst, NULL);
        if (!D.hwnd) return;
        APPBARDATA abd = { sizeof(abd) };
        abd.hWnd = D.hwnd;
        abd.uCallbackMessage = WM_DOCK_APPBAR;
        D.appbar = SHAppBarMessage(ABM_NEW, &abd) != 0;
        AppBarPos();
        ApplyCapture();
        Rescan();
        for (int i = 0; i < D.count; ++i) D.items[i].barW = BarTarget(&D.items[i]);
        Render();
        ShowWindow(D.hwnd, SW_SHOWNOACTIVATE);
        SetTimer(D.hwnd, TIMER_DSTATUS, STATUS_MS, NULL);
        D.fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, FgHook, 0, 0,
                                   WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        /* ventanas que se abren, se cierran o se ocultan: el dock se entera al momento */
        D.winHook = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE, NULL, WinHook, 0, 0,
                                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        ApplyCapture();
    } else {
        Rescan();
        Render();
    }
    SetTaskbarOff(g_cfg.dockHideTaskbar);
}

void Dock_ConfigChanged(void)
{
    if (!D.hwnd) return;
    DockMeasure();
    ApplyCapture();
    AppBarPos();
    SetWindowPos(D.hwnd, HWND_TOPMOST, D.mon.left, D.mon.bottom - WinH(), D.mon.right - D.mon.left, WinH(), SWP_NOACTIVATE);
    Canvas_Free(&D.back);
    Rescan();           /* con otro tamaño de icono la caché pide la nueva resolución */
    Render();
    Kick();             /* ocultar dock activado o quitado: que baje o suba */
}

void Dock_Reposition(void)
{
    if (!D.hwnd) return;
    DockMeasure();
    AppBarPos();
    SetWindowPos(D.hwnd, HWND_TOPMOST, D.mon.left, D.mon.bottom - WinH(), D.mon.right - D.mon.left, WinH(), SWP_NOACTIVATE);
    Canvas_Free(&D.back);
    Rescan();
    Render();
}

void Dock_Raise(void)
{
    if (D.hwnd && !D.fullscreen)
        SetWindowPos(D.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void Dock_Destroy(void)
{
    if (D.hwnd) DestroyWindow(D.hwnd);
}
