/*
 * dock.c — dock estilo macOS: panel flotante abajo con las apps ancladas y abiertas,
 * magnificación al pasar el cursor (como el Dock de Apple) y clic para abrir/enfocar.
 *
 * - Apps ancladas: las de la barra de tareas, en su orden (la lista de Explorer): los .lnk
 *   y también las apps empaquetadas (Store, PWA), que no tienen .lnk.
 * - Apps abiertas: se enumeran las ventanas; una barrita blanca marca las que corren
 *   (más larga y brillante la que está al frente).
 * - Iconos a alta resolución (IShellItemImageFactory), en caché y con alpha propio.
 * - Fondo configurable: desenfoque propio de lo que hay detrás (como el notch) y opacidad.
 * - Ventana layered; fuera del panel los clics pasan al escritorio.
 * - Se lanza todo vía explorer.exe (ninguna extensión de shell entra en el proceso).
 * - Opcional: desactiva la barra de tareas de Windows (autohide + oculta) y la devuelve
 *   tal cual al salir.
 * - Un dock por monitor, todos con las mismas apps (como en macOS). Cada uno lleva su
 *   geometría, su lupa y sus animaciones; el del monitor principal (s_docks[0]) es además
 *   el que gobierna la barra de tareas de Windows. Los avisos de Windows (ganchos) son
 *   únicos y se reparten a todos los docks.
 */
#define COBJMACROS
#include "app.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <propsys.h>
#include <knownfolders.h>
#include <math.h>

#define DOCK_CLASS    L"OpenDock.Dock"
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
#define TIMER_DPOLL   8      /* ocultar dock: dónde está el cursor (barato: 20 veces por segundo) */
#define TIMER_DPREV   9      /* vista previa: el cursor lleva un momento sobre una app abierta */
#define PREV_DELAY    250
#define PREV_CLASS    L"OpenDock.DockPreview"
#define PREV_MAX      8
#define PEEK_CLASS    L"OpenDock.DockPeek"
#define PEEK_DELAY    150    /* cursor quieto en una tarjeta: esa ventana, en su sitio */
#define PEEK_FRESH    2500   /* la foto preparada al abrir la vista previa vale este rato */
#define STATUS_MS     10000  /* repaso de seguridad: lo normal llega por avisos */
#define MAX_ITEMS     48
#define ICON_GAP      12
#define DOCK_PADX     16
#define DOCK_PADY     11
#define BOTTOM_MARGIN 12
#define MAXMAG        1.5f
#define SPREAD        1.55f
#define BACK_SCALE    4      /* el fondo se captura a 1/4 de resolución */
#define MAX_ICONS     192    /* caché por tamaño: monitores con escalas distintas piden dos */
#define MAX_DOCKS     6      /* un dock por monitor */

static const int   kIconSizes[4] = { 30, 38, 46, 56 };
static const float kOpacity[4]   = { 0.30f, 0.55f, 0.75f, 0.92f };

typedef struct {
    wchar_t launch[MAX_PATH];   /* .lnk o ruta a abrir con explorer */
    wchar_t exe[MAX_PATH];      /* ejecutable (para casar con ventanas) */
    wchar_t name[80];
    wchar_t aumid[128];         /* app empaquetada (Store, PWA): su AppUserModelID */
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

typedef struct {
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
    BOOL     dirty;             /* hay que redibujar aunque nada se anime */
    BOOL     shellOpen;         /* Inicio / Buscar de Windows abierto */
    BOOL     peek;              /* la bandeja de Windows a la vista un momento */
    DWORD    peekAt;
    float    slide;             /* 0 = visible · 1 = escondido bajo el borde */
    float    sink, sinkV;       /* ocultar dock: 0 arriba · 1 con media altura bajo la pantalla */
    int      sinkPx;
    DWORD    leaveAt;
    BOOL     menuOpen;
    BOOL     atDock;            /* cursor en la zona del dock (incluido el hueco hasta el borde) */
    int      taskWatch;         /* ticks restantes de TIMER_DTASK */
    BOOL     fsSys, fsFg;       /* pantalla completa: según Windows · según la ventana de delante */
    volatile LONG wantFrames;   /* este dock se anima: el marcapasos le manda fotogramas */
    volatile LONG framePending; /* ya tiene un WM_DFRAME en la cola */
    DWORD    iconGen;           /* generación de la caché de iconos con la que se rehízo */
    UINT     pollEvery;         /* periodo actual de TIMER_DPOLL */
    int      prevArm;           /* icono con la vista previa en espera (TIMER_DPREV), -1 ninguno */
    DockItem pinCache[MAX_ITEMS];   /* anclados ya resueltos (a su tamaño de icono) */
    int      npinCache;
    DWORD    pinSig;
} Dock;

/* s_docks[0] es siempre el del monitor principal. Fuera de DockProc y de los recorridos
 * por todos los docks, D es el principal. */
static Dock  s_docks[MAX_DOCKS];
static Dock *s_d = &s_docks[0];
#define D (*s_d)
#define PRIMARY (&s_docks[0])

/* Vista previa de las ventanas de una app (una sola a la vez, del dock que la abrió). */
static struct {
    HWND    hwnd;
    Dock   *dock;
    wchar_t key[MAX_PATH];      /* ItemKey de la app: los índices cambian al reescanear */
    wchar_t suppress[MAX_PATH]; /* tras un clic en su icono no se reabre hasta salir de él */
    HWND    wins[PREV_MAX];
    HANDLE  thumb[PREV_MAX];    /* miniatura DWM (NULL: minimizada o sin imagen) */
    RECT    card[PREV_MAX], area[PREV_MAX], close[PREV_MAX];
    int     n, hot, pressed;
    BOOL    hotClose, tracking;
    int     cx, bottom;         /* centro x y borde de abajo, en pantalla */
    DWORD   outSince;           /* desde cuándo el cursor no está ni en ella ni en su icono */
    Canvas  cv;                 /* lo que se presenta (tamaño actual de la ventana) */
    Canvas  full;               /* el contenido a tamaño completo: se compone escalado */
    BOOL    fullDirty;
    HFONT   font, glyphs;
    int     fontPx;
    /* muelles, como el notch: nace de una pastilla sobre el icono, crece con el rebote del
     * tema y el contenido aparece cuando ya hay sitio; al cerrar vuelve a la pastilla */
    RECT    tdst[PREV_MAX];     /* destino de cada miniatura en el contenido completo */
    int     W, H;               /* tamaño completo */
    float   w, h, vw, vh, x, vx, fade;
    float   cs[PREV_MAX], csv[PREV_MAX], ca[PREV_MAX];     /* escala de cada tarjeta y su ✕ */
    float   xs[PREV_MAX], xsv[PREV_MAX];    /* tamaño de la ✕: crece con rebote al acercarse */
    int     mx, my;                         /* cursor en el contenido (para la cercanía a la ✕) */
    int     ox, oy;             /* origen del contenido en la ventana (fotograma actual) */
    BOOL    closing, animating;
    LARGE_INTEGER last;
} PV;
static void PreviewClose(void);
static BOOL PreviewOpenHere(void) { return PV.hwnd && PV.dock == s_d; }
#define CLOSE_NEAR  1.28f       /* ✕ con el cursor cerca */
#define CLOSE_HOT   1.7f        /* ✕ con el cursor encima */
static void CloseCircle(int i, float *cx, float *cy, float *r)
{
    const RECT *x = &PV.close[i];
    const float r0 = (x->right - x->left) * 0.5f, rs = r0 * PV.xs[i];
    *r = rs;
    *cx = x->right - rs;
    *cy = (x->top + x->bottom) * 0.5f + r0 - rs;     /* el borde de abajo no se mueve */
}
static HWINEVENTHOOK s_fgHook, s_winHook, s_moveHook;   /* únicos: se reparten a todos */

static BOOL AnyShellOpen(void)
{
    for (int k = 0; k < MAX_DOCKS; ++k) if (s_docks[k].hwnd && s_docks[k].shellOpen) return TRUE;
    return FALSE;
}

static int DS(int v) { return MulDiv(v, (int)D.dpi, 96); }
/* desenfoque del fondo: el del ajuste, salvo en escritorio remoto (ahí se vería a sí mismo) */
static int Blur(void) { return App_RemoteView() ? 0 : g_cfg.dockBlur; }
static int Base(void) { return DS(kIconSizes[max(0, min(3, g_cfg.dockIcon))]); }
/* alto de la ventana: icono magnificado + salto */
static int WinH(void) { return (int)(Base() * 2.3f) + DS(36); }

/* ───────────────────────── Pacer de alta resolución ───────────────────────── */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
static HANDLE s_dpOn;     /* evento manual: algún dock se está animando */

/* Hilo aparte: no mira D (que cambia en el hilo de la interfaz), sino cada dock. */
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
        for (int k = 0; k < MAX_DOCKS; ++k) {
            Dock *d = &s_docks[k];
            const HWND h = *(HWND volatile *)&d->hwnd;
            if (h && d->wantFrames && !InterlockedExchange(&d->framePending, 1)) PostMessageW(h, WM_DFRAME, 0, 0);
        }
    }
}

/* Fotogramas para el dock actual; el marcapasos sigue mientras alguno los quiera. */
static void PacerOn(BOOL on)
{
    if (!s_dpOn) {
        s_dpOn = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE th = s_dpOn ? CreateThread(NULL, 0, DockPacer, NULL, 0, NULL) : NULL;
        if (th) CloseHandle(th);
    }
    if (!s_dpOn) return;
    InterlockedExchange(&D.wantFrames, on);
    BOOL any = FALSE;
    for (int k = 0; k < MAX_DOCKS && !any; ++k) any = s_docks[k].hwnd && s_docks[k].wantFrames;
    if (any) SetEvent(s_dpOn); else ResetEvent(s_dpOn);
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

static DWORD s_iconGen;     /* sube al vaciar la caché: los items que apuntan a ella caducan */
static void ClearIconCache(void)
{
    for (int i = 0; i < s_nicons; ++i)
        if (s_icons[i].px) HeapFree(GetProcessHeap(), 0, s_icons[i].px);
    s_nicons = 0;
    ++s_iconGen;
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

static const wchar_t *ItemKey(const DockItem *it) { return it->aumid[0] ? it->aumid : it->exe[0] ? it->exe : it->launch; }

static void NewItem(DockItem *it)
{
    ZeroMemory(it, sizeof(*it));
    it->scale = 1.0f;
    it->hopT = -1.0f;
}

static void AddLnk(const wchar_t *dir, const wchar_t *file)
{
    if (D.count >= MAX_ITEMS) return;
    DockItem *it = &D.items[D.count];
    NewItem(it);
    it->pinned = TRUE;
    if (lstrlenW(dir) + lstrlenW(file) + 2 > MAX_PATH) return;      /* nombre de .lnk demasiado largo */
    wsprintfW(it->launch, L"%s\\%s", dir, file);
    ResolveLnk(it->launch, it->exe);
    lstrcpynW(it->name, file, 80);
    wchar_t *dot = wcsrchr(it->name, L'.');
    if (dot) *dot = 0;
    if (GetIcon(it->launch, it)) D.count++;
}

/* Una app empaquetada: se abre y se dibuja por shell:AppsFolder\<AUMID>. */
static BOOL AddPackagedAs(const wchar_t *aumid, BOOL pinned)
{
    if (D.count >= MAX_ITEMS) return FALSE;
    wchar_t path[MAX_PATH];
    wsprintfW(path, L"shell:AppsFolder\\%s", aumid);
    IShellItem *si = NULL;
    if (FAILED(SHCreateItemFromParsingName(path, NULL, &IID_IShellItem, (void **)&si))) return FALSE;
    DockItem *it = &D.items[D.count];
    NewItem(it);
    it->pinned = pinned;
    lstrcpynW(it->launch, path, MAX_PATH);
    lstrcpynW(it->aumid, aumid, 128);
    PWSTR dn = NULL;
    if (SUCCEEDED(IShellItem_GetDisplayName(si, SIGDN_NORMALDISPLAY, &dn)) && dn) { lstrcpynW(it->name, dn, 80); CoTaskMemFree(dn); }
    IShellItem_Release(si);
    for (int i = 0; i < D.count; ++i)               /* ya está como .lnk */
        if (D.items[i].pinned && !lstrcmpiW(D.items[i].name, it->name)) return TRUE;
    if (GetIcon(path, it)) { D.count++; return TRUE; }
    return FALSE;
}

static BOOL AddPackaged(const wchar_t *aumid) { return AddPackagedAs(aumid, TRUE); }

/* ¿Parece un AppUserModelID de paquete? Familia (nombre_hash de 13) + "!" + aplicación. */
static BOOL LooksAumid(const wchar_t *s)
{
    const wchar_t *bang = wcschr(s, L'!'), *us = NULL;
    if (!bang || bang == s || !bang[1]) return FALSE;
    for (const wchar_t *p = s; *p; ++p) if (*p <= L' ' || *p == L'\\' || *p == L'/' || *p == L':') return FALSE;
    for (const wchar_t *p = s; p < bang; ++p) if (*p == L'_') us = p;
    return us && bang - us - 1 == 13;
}

/* La lista de anclados de Explorer (Taskband\Favorites) guarda cada ancla como una lista de
 * elementos del shell. Dentro van, en UTF-16, el nombre del .lnk o el AppUserModelID de la
 * app empaquetada: se buscan a las dos alineaciones posibles y se ordenan por posición. */
typedef struct { DWORD at; int lnk; wchar_t aumid[128]; } PinRef;

static void PinNote(PinRef *refs, int *n, int max, DWORD at, int lnk, const wchar_t *aumid)
{
    for (int i = 0; i < *n; ++i)
        if ((lnk >= 0 && refs[i].lnk == lnk) || (lnk < 0 && refs[i].lnk < 0 && !lstrcmpiW(refs[i].aumid, aumid))) {
            if (at < refs[i].at) refs[i].at = at;
            return;
        }
    if (*n >= max) return;
    refs[*n].at = at; refs[*n].lnk = lnk;
    lstrcpynW(refs[*n].aumid, aumid ? aumid : L"", 128);
    ++*n;
}

static int ReadPinOrder(wchar_t (*lnks)[MAX_PATH], int nlnk, PinRef *refs, int max)
{
    static const wchar_t *kKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Taskband";
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, L"Favorites", RRF_RT_REG_BINARY, NULL, NULL, &size) != ERROR_SUCCESS ||
        !size || size > 1024 * 1024) return -1;
    BYTE *b = (BYTE *)HeapAlloc(GetProcessHeap(), 0, size);
    if (!b) return -1;
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, L"Favorites", RRF_RT_REG_BINARY, NULL, b, &size) != ERROR_SUCCESS) {
        HeapFree(GetProcessHeap(), 0, b);
        return -1;
    }
    int n = 0;
    wchar_t tok[260];
    for (DWORD par = 0; par < 2; ++par) {
        int len = 0;
        DWORD start = 0;
        for (DWORD i = par; i + 1 <= size; i += 2) {
            const wchar_t ch = i + 1 < size ? (wchar_t)(b[i] | b[i + 1] << 8) : 0;
            if (ch >= 0x20 && ch != 0x7F && (ch < 0xD800 || ch > 0xDFFF) && len < 259) {
                if (!len) start = i;
                tok[len++] = ch;
                continue;
            }
            if (len >= 5) {
                tok[len] = 0;
                if (len > 4 && !lstrcmpiW(tok + len - 4, L".lnk")) {
                    for (int k = 0; k < nlnk; ++k) {          /* puede llevar basura delante */
                        const int ln = lstrlenW(lnks[k]);
                        if (len >= ln && !lstrcmpiW(tok + len - ln, lnks[k])) { PinNote(refs, &n, max, start, k, NULL); break; }
                    }
                } else {
                    for (int skip = 0; skip < 3 && skip < len; ++skip)
                        if (LooksAumid(tok + skip)) { PinNote(refs, &n, max, start, -1, tok + skip); break; }
                }
            }
            len = 0;
        }
    }
    HeapFree(GetProcessHeap(), 0, b);
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (refs[j].at < refs[i].at) { PinRef t = refs[i]; refs[i] = refs[j]; refs[j] = t; }
    return n;
}

/* ───────────────────────── Anclados propios del dock ─────────────────────────
 * Windows no deja anclar a su barra con una API pública, así que "Anclar al dock" y
 * "Quitar del dock" guardan dos listas propias (REG_MULTI_SZ en la clave de OpenDock):
 *   DockAnclas   apps añadidas (ruta del .exe, o "aumid:" + AppUserModelID)
 *   DockOcultas  anclados de la barra de Windows que el dock no enseña (su ItemKey)
 * La barra de tareas de Windows no se toca. */
#define PIN_CAP 32
static wchar_t s_pinAdd[PIN_CAP][MAX_PATH], s_pinHide[PIN_CAP][MAX_PATH];
static int     s_npinAdd, s_npinHide;
static DWORD   s_pinVer;               /* cambia al editar las listas: invalida la caché */
static BOOL    s_pinLoaded;

static int ReadMulti(LPCWSTR name, wchar_t list[][MAX_PATH])
{
    static wchar_t buf[PIN_CAP * MAX_PATH + 2];
    DWORD size = sizeof(buf) - 2 * sizeof(wchar_t);
    ZeroMemory(buf, sizeof(buf));
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, name, RRF_RT_REG_MULTI_SZ, NULL, buf, &size) != ERROR_SUCCESS) return 0;
    int n = 0;
    for (const wchar_t *p = buf; *p && n < PIN_CAP; p += lstrlenW(p) + 1) lstrcpynW(list[n++], p, MAX_PATH);
    return n;
}

static void WriteMulti(LPCWSTR name, wchar_t list[][MAX_PATH], int n)
{
    static wchar_t buf[PIN_CAP * MAX_PATH + 2];
    int o = 0;
    for (int i = 0; i < n; ++i) { lstrcpyW(buf + o, list[i]); o += lstrlenW(list[i]) + 1; }
    buf[o++] = 0;
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_MULTI_SZ, (const BYTE *)buf, (DWORD)(o * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}

static void LoadPinLists(void)
{
    if (s_pinLoaded) return;
    s_pinLoaded = TRUE;
    s_npinAdd = ReadMulti(L"DockAnclas", s_pinAdd);
    s_npinHide = ReadMulti(L"DockOcultas", s_pinHide);
}

static int FindIn(wchar_t list[][MAX_PATH], int n, const wchar_t *key)
{
    for (int i = 0; i < n; ++i) if (!lstrcmpiW(list[i], key)) return i;
    return -1;
}

static void RemoveAt(wchar_t list[][MAX_PATH], int *n, int i)
{
    MoveMemory(list[i], list[i + 1], (SIZE_T)(*n - i - 1) * sizeof(list[0]));
    --*n;
}

static void AppendTo(wchar_t list[][MAX_PATH], int *n, const wchar_t *key)
{
    if (*n < PIN_CAP && FindIn(list, *n, key) < 0) lstrcpynW(list[(*n)++], key, MAX_PATH);
}

/* Una app añadida con "Anclar al dock" a partir de su .exe. */
static void AppName(const wchar_t *exe, wchar_t *out, int cch);
static void AddExePin(const wchar_t *exe)
{
    if (D.count >= MAX_ITEMS || GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) return;
    DockItem *it = &D.items[D.count];
    NewItem(it);
    it->pinned = TRUE;
    lstrcpynW(it->exe, exe, MAX_PATH);
    lstrcpynW(it->launch, exe, MAX_PATH);
    if (!lstrcmpiW(BaseName(exe), L"explorer.exe")) lstrcpynW(it->name, L"Explorador de archivos", 80);
    else AppName(exe, it->name, 80);
    if (GetIcon(exe, it)) D.count++;
}

/* Firma de lo anclado: la fecha de la carpeta de accesos, la lista de Explorer en el
 * registro y la generación de la caché de iconos. Si no cambió, los anclados son los mismos
 * y no hace falta volver a resolver cada .lnk (lo más caro del repaso del dock). */
static DWORD PinSignature(const wchar_t *dir)
{
    DWORD h = 2166136261u ^ s_iconGen;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(dir, GetFileExInfoStandard, &fa))
        h = (h ^ fa.ftLastWriteTime.dwLowDateTime) * 16777619u, h = (h ^ fa.ftLastWriteTime.dwHighDateTime) * 16777619u;
    BYTE buf[8192];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Taskband", L"Favorites",
                     RRF_RT_REG_BINARY, NULL, buf, &size) == ERROR_SUCCESS)
        for (DWORD i = 0; i < size; ++i) h = (h ^ buf[i]) * 16777619u;
    else h = (h ^ size) * 16777619u;     /* lista más grande que el búfer: cuenta su tamaño */
    return h | 1;
}

static void AddPinned(void)
{
    PWSTR appdata = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &appdata))) return;
    wchar_t dir[MAX_PATH], pat[MAX_PATH];
    wsprintfW(dir, L"%s\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar", appdata);
    CoTaskMemFree(appdata);
    wsprintfW(pat, L"%s\\*.lnk", dir);

    /* caché por dock: cada monitor puede pedir otro tamaño de icono */
    LoadPinLists();
    const DWORD now = PinSignature(dir) ^ (s_pinVer * 2654435761u) ^ ((DWORD)Base() << 20);
    if (now == D.pinSig) {
        const int n = min(D.npinCache, MAX_ITEMS - D.count);
        CopyMemory(&D.items[D.count], D.pinCache, sizeof(DockItem) * n);
        D.count += n;
        return;
    }
    const int first = D.count;

    static wchar_t lnks[MAX_ITEMS][MAX_PATH];
    static PinRef refs[MAX_ITEMS];
    BOOL used[MAX_ITEMS] = { 0 };
    int nlnk = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (nlnk < MAX_ITEMS) lstrcpynW(lnks[nlnk++], fd.cFileName, MAX_PATH); } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    /* en el orden de la barra de tareas; lo que no esté en su lista, al final */
    const int n = ReadPinOrder(lnks, nlnk, refs, MAX_ITEMS);
    for (int i = 0; i < n; ++i) {
        if (refs[i].lnk >= 0) { AddLnk(dir, lnks[refs[i].lnk]); used[refs[i].lnk] = TRUE; }
        else AddPackaged(refs[i].aumid);
    }
    for (int k = 0; k < nlnk; ++k) if (!used[k]) AddLnk(dir, lnks[k]);
    /* fuera las que se quitaron del dock; luego las añadidas con "Anclar al dock" */
    for (int i = D.count - 1; i >= first; --i)
        if (FindIn(s_pinHide, s_npinHide, ItemKey(&D.items[i])) >= 0) {
            MoveMemory(&D.items[i], &D.items[i + 1], (SIZE_T)(D.count - i - 1) * sizeof(DockItem));
            --D.count;
        }
    for (int i = 0; i < s_npinAdd; ++i) {
        BOOL dup = FALSE;
        const wchar_t *key = s_pinAdd[i], *aumid = !wcsncmp(key, L"aumid:", 6) ? key + 6 : NULL;
        for (int j = first; j < D.count && !dup; ++j) dup = !lstrcmpiW(ItemKey(&D.items[j]), aumid ? aumid : key);
        if (dup) continue;
        if (aumid) AddPackaged(aumid);
        else AddExePin(key);
    }
    D.npinCache = D.count - first;
    CopyMemory(D.pinCache, &D.items[first], sizeof(DockItem) * D.npinCache);
    D.pinSig = now;
}

/* El AppUserModelID de una ventana (las de apps empaquetadas y PWA lo llevan). */
static const PROPERTYKEY kPKEY_AumId = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 5 };
static BOOL WindowAumid(HWND w, wchar_t *out, int cch)
{
    out[0] = 0;
    IPropertyStore *ps = NULL;
    if (FAILED(SHGetPropertyStoreForWindow(w, &IID_IPropertyStore, (void **)&ps)) || !ps) return FALSE;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(IPropertyStore_GetValue(ps, &kPKEY_AumId, &v)) && v.vt == VT_LPWSTR && v.pwszVal) lstrcpynW(out, v.pwszVal, cch);
    PropVariantClear(&v);
    IPropertyStore_Release(ps);
    return out[0] != 0;
}

BOOL Dock_WindowAumid(HWND w, wchar_t *out, int cch) { return WindowAumid(w, out, cch); }

static DockItem *FindByAumid(HWND w)
{
    BOOL any = FALSE;
    for (int i = 0; i < D.count && !any; ++i) any = D.items[i].aumid[0] != 0;
    if (!any) return NULL;
    wchar_t id[128];
    if (!WindowAumid(w, id, 128)) return NULL;
    for (int i = 0; i < D.count; ++i)
        if (D.items[i].aumid[0] && !lstrcmpiW(D.items[i].aumid, id)) return &D.items[i];
    return NULL;
}

/* Las mismas reglas que la barra de tareas: WS_EX_APPWINDOW la pone siempre; si no, ni las
 * de herramientas ni las que tienen dueño (diálogos, paletas). */
static BOOL IsAppWindow(HWND w)
{
    if (!IsWindowVisible(w)) return FALSE;
    const LONG ex = (LONG)GetWindowLongPtrW(w, GWL_EXSTYLE);
    if (!(ex & WS_EX_APPWINDOW) && ((ex & WS_EX_TOOLWINDOW) || GetWindow(w, GW_OWNER))) return FALSE;
    int cloak = 0;
    DwmGetWindowAttribute(w, 14 /*DWMWA_CLOAKED*/, &cloak, sizeof(cloak));
    if (cloak) return FALSE;
    return (ex & WS_EX_APPWINDOW) || GetWindowTextLengthW(w) > 0;
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
    /* De explorer.exe sólo las ventanas del Explorador de archivos: el escritorio, la barra
     * y demás piezas del shell viven en el mismo proceso y no son apps. */
    const BOOL explorer = !lstrcmpiW(BaseName(path), L"explorer.exe");
    if (explorer) {
        wchar_t cls[32];
        if (!GetClassNameW(w, cls, 32) || (lstrcmpW(cls, L"CabinetWClass") && lstrcmpW(cls, L"ExploreWClass")))
            return TRUE;
    }

    DockItem *it = FindByAumid(w);           /* app empaquetada o PWA: antes que por el .exe */
    /* Apps de la Tienda (UWP): la ventana es de ApplicationFrameHost.exe, que las aloja a
     * todas; cada una va por su AppUserModelID, no por ese .exe. */
    if (!it && !lstrcmpiW(BaseName(path), L"ApplicationFrameHost.exe")) {
        wchar_t id[128];
        if (!WindowAumid(w, id, 128)) return TRUE;
        for (int i = 0; i < D.count && !it; ++i) if (!lstrcmpiW(D.items[i].aumid, id)) it = &D.items[i];
        if (!it) {
            const int before = D.count;
            if (!AddPackagedAs(id, FALSE)) return TRUE;
            if (D.count > before) it = &D.items[before];
            else                                 /* ya anclada como acceso directo: esa */
                for (int i = 0; i < D.count && !it; ++i)
                    if (D.items[i].pinned && !lstrcmpiW(D.items[i].name, D.items[D.count].name)) it = &D.items[i];
        }
        if (!it) return TRUE;
    }
    if (!it) it = FindByExe(path);
    if (!it && D.count < MAX_ITEMS) {       /* app abierta no anclada: añadir al final */
        it = &D.items[D.count];
        NewItem(it);
        lstrcpynW(it->exe, path, MAX_PATH);
        lstrcpynW(it->launch, path, MAX_PATH);
        if (explorer) lstrcpynW(it->name, L"Explorador de archivos", 80);
        else AppName(path, it->name, 80);
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
    int owner = -1;                         /* la app que tiene esa ventana, si alguna */
    for (int i = 0; i < D.count && owner < 0; ++i)
        for (int k = 0; k < D.items[i].nwin; ++k) if (D.items[i].wins[k] == fg) { owner = i; break; }
    for (int i = 0; i < D.count; ++i) {
        DockItem *it = &D.items[i];
        it->active = owner >= 0 ? i == owner
                   : have && it->running && it->exe[0] && !it->aumid[0] && !lstrcmpiW(BaseName(it->exe), BaseName(path));
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
    D.iconGen = s_iconGen;
}

/* Todos los docks a la vez (p. ej. al anclar o quitar una app). */
static void Kick(void);
static void RescanAll(void)
{
    Dock *o = s_d;
    for (int k = 0; k < MAX_DOCKS; ++k) {
        s_d = &s_docks[k];
        if (D.hwnd && !D.fullscreen) { Rescan(); Kick(); }
    }
    s_d = o;
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
static BOOL HiddenAway(void) { return g_cfg.dockAutoHide == 2 && D.sink > 0.98f && !D.atDock; }

static BOOL CaptureBackdrop(void)
{
    if (HiddenAway()) return FALSE;             /* nadie lo ve: ya se capturará al subir */
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
    const int r = max(1, DS(Blur() == 2 ? 24 : 11) / BACK_SCALE);
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
    /* otro dock vació la caché de iconos: los de este apuntan a memoria liberada */
    if (D.count && D.iconGen != s_iconGen) Rescan();
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

    if (Blur()) {
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

/* Cuánto baja al ocultarse: hasta la mitad del panel o del todo (con su sombra). */
static float SinkDepth(void)
{
    if (g_cfg.dockAutoHide == 2) return (float)(WinH() - (D.panelH > 0 ? D.panelY : (int)(PanelBottom() - PanelH())) + DS(8));
    return DS(BOTTOM_MARGIN) + PanelH() * 0.5f;
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
    const BOOL sunk = g_cfg.dockAutoHide && !D.inside && !D.atDock && !D.menuOpen && !PreviewOpenHere() && GetTickCount() - D.leaveAt >= SINK_DELAY;
    const float kt = sunk ? 1.0f : 0.0f, z = sunk ? 0.95f : min(1.0f, Pop_Zeta() + 0.12f);
    if (fabsf(kt - D.sink) > 0.002f || fabsf(D.sinkV) > 0.02f) {
        for (int k = 0; k < 2; ++k) Spring2(&D.sink, &D.sinkV, kt, dt * 0.5f, sunk ? 140.0f : 260.0f, z);
        busy = TRUE;
    } else if (D.sink != kt) {
        D.sink = kt; D.sinkV = 0;
        if (Blur()) SetTimer(D.hwnd, TIMER_DBLURQ, 30, NULL);    /* el vidrio, desde su sitio nuevo */
    }
    D.sinkPx = (int)lroundf(D.sink * SinkDepth());
    /* con el cursor quieto encima ya no se redibuja 120 veces por segundo: solo si algo
     * se movió (o lo pidió un clic), y el marcapasos se para hasta el próximo movimiento */
    if (busy || D.dirty) { Render(); D.dirty = FALSE; }
    if (!busy) PacerOn(FALSE);
}

static void Kick(void)
{
    /* el reloj sólo se pone a cero si la animación estaba parada: con ella en marcha, cada
     * movimiento del ratón (hasta 1000 por segundo) lo reiniciaba y los muelles avanzaban
     * una fracción del tiempo real: la lupa iba a cámara lenta */
    if (!D.wantFrames) QueryPerformanceCounter(&D.last);
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
    App_ShellOpen(it->launch);      /* vía explorer: ninguna extensión de shell entra en nuestro proceso */
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
 * - varias: el clic abre su vista previa (ver más abajo) y se elige ahí; Activate solo
 *   recorre las ventanas si se llama sin ella (p. ej. desde el teclado). */
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

/* ───────────────────────── Vista previa de ventanas ─────────────────────────
 * Con el cursor un momento sobre una app abierta (o con un clic si tiene varias ventanas)
 * sale encima de su icono una tarjeta por ventana con su miniatura en vivo (DWM), su título
 * y una ✕ para cerrarla; clic en la tarjeta = esa ventana al frente. La ventana de la vista
 * previa es normal (no layered): DWM solo compone miniaturas en ventanas así. Mientras está
 * abierta, un temporizador de 50 ms mira el cursor; cerrada no queda nada en marcha. */
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#define PREV_TIMER  1
#define PREV_ANIM   2      /* fotogramas de sus muelles (solo mientras se mueve) */

static int LiveWins(const DockItem *it)
{
    int n = 0;
    for (int k = 0; k < it->nwin; ++k) if (IsWindow(it->wins[k])) ++n;
    return n;
}

static DockItem *ItemByKey(const wchar_t *key)
{
    for (int i = 0; i < D.count; ++i) if (!lstrcmpiW(ItemKey(&D.items[i]), key)) return &D.items[i];
    return NULL;
}

static void PreviewDropThumbs(void)
{
    for (int i = 0; i < PREV_MAX; ++i)
        if (PV.thumb[i]) { DwmUnregisterThumbnail(PV.thumb[i]); PV.thumb[i] = NULL; }
}

/* ── Muelles de la vista previa (los mismos que el notch) ── */
static float PvZeta(void)
{
    static const float z[3] = { 0.85f, 0.62f, 0.40f };     /* suave · normal · bouncy */
    return z[max(0, min(2, g_cfg.bounce))];
}

/* ── Vistazo: la ventana de la tarjeta señalada, en su sitio ──
 * Como el vistazo de la barra de tareas, sin su API no documentada: una ventana que cubre
 * el monitor de esa ventana, por debajo del dock y de la vista previa, con una foto de la
 * pantalla tomada justo antes que se oscurece y se desenfoca poco a poco, y encima la
 * miniatura DWM de la ventana a su tamaño real y en su sitio (entra con el muelle del tema).
 * La foto se prepara en cuanto la vista previa termina de abrirse, para que el vistazo
 * salga al momento (con monitores grandes tomarla y desenfocarla lleva su rato).
 * Solo existe mientras se mira: al soltar la tarjeta se libera todo. */
static struct {
    HWND    hwnd, target, arm;
    HANDLE  thumb;
    RECT    mon;                /* monitor que cubre (pantalla) */
    Canvas  cur, dim;           /* lo presentado · la foto oscurecida y desenfocada */
    RECT    snapMon;            /* foto preparada de antemano: de qué monitor y cuándo */
    DWORD   snapAt;
    float   t, ta, s, sv;       /* fondo 0→1 · miniatura 0→1 · escala de la miniatura */
    DWORD   armAt, outAt;
    BOOL    animating;
    LARGE_INTEGER last;
} PK;

/* La foto, fuera (al cerrar la vista previa, o porque ya se usó). */
static void PeekDrop(void)
{
    PK.snapAt = 0;
    Canvas_Free(&PK.cur);
    Canvas_Free(&PK.dim);
}

static void PeekHide(void)
{
    PK.arm = NULL;
    PK.outAt = 0;
    if (PK.thumb) { DwmUnregisterThumbnail(PK.thumb); PK.thumb = NULL; }
    const BOOL shown = PK.hwnd != NULL;
    if (PK.hwnd) { const HWND h = PK.hwnd; PK.hwnd = NULL; KillTimer(h, 1); DestroyWindow(h); }
    PK.target = NULL;
    PK.animating = FALSE;
    if (shown) PeekDrop();      /* se fue oscureciendo: ya no es la pantalla tal cual */
}

static void PeekThumb(void)
{
    RECT wr;
    if (!PK.thumb || !PK.target || !GetWindowRect(PK.target, &wr)) return;
    const float cx = (wr.left + wr.right) * 0.5f - PK.mon.left, cy = (wr.top + wr.bottom) * 0.5f - PK.mon.top;
    const float hw = (wr.right - wr.left) * 0.5f * PK.s, hh = (wr.bottom - wr.top) * 0.5f * PK.s;
    DWM_THUMBNAIL_PROPERTIES tp = { 0 };
    tp.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
    SetRect(&tp.rcDestination, (int)lroundf(cx - hw), (int)lroundf(cy - hh), (int)lroundf(cx + hw), (int)lroundf(cy + hh));
    tp.opacity = (BYTE)(255.0f * max(0.0f, min(1.0f, PK.ta)) + 0.5f);
    tp.fVisible = TRUE;
    tp.fSourceClientAreaOnly = FALSE;
    DwmUpdateThumbnailProperties(PK.thumb, &tp);
}

static void PeekKick(void)
{
    if (!PK.hwnd || PK.animating) return;
    PK.animating = TRUE;
    QueryPerformanceCounter(&PK.last);
    SetTimer(PK.hwnd, 1, 10, NULL);
}

/* La foto del monitor: tal cual en cur (así entra sin saltos) y, aparte, a 1/4, desenfocada,
 * desaturada y oscura, vuelta a su tamaño en dim. */
static BOOL PeekSnapshot(const RECT *mon)
{
    const int w = mon->right - mon->left, h = mon->bottom - mon->top;
    if (w <= 0 || h <= 0 || !Canvas_Init(&PK.cur, w, h) || !Canvas_Init(&PK.dim, w, h)) return FALSE;
    HDC screen = GetDC(NULL);
    BitBlt(PK.cur.dc, 0, 0, w, h, screen, mon->left, mon->top, SRCCOPY);
    ReleaseDC(NULL, screen);
    Canvas sm = { 0 };
    const int sw = max(1, w / 4), sh = max(1, h / 4);
    if (!Canvas_Init(&sm, sw, sh)) return FALSE;
    SetStretchBltMode(sm.dc, COLORONCOLOR);     /* rápida: el desenfoque de después la suaviza */
    SetBrushOrgEx(sm.dc, 0, 0, NULL);
    StretchBlt(sm.dc, 0, 0, sw, sh, PK.cur.dc, 0, 0, w, h, SRCCOPY);
    GdiFlush();
    for (int i = 0; i < 2; ++i) Gfx_BoxBlur(sm.px, sw, sh, max(1, DS(14) / 4));
    for (int i = 0; i < sw * sh; ++i) {
        const DWORD v = sm.px[i];
        const float r = (float)((v >> 16) & 255), g = (float)((v >> 8) & 255), b = (float)(v & 255);
        const float y = r * 0.30f + g * 0.59f + b * 0.11f;
        const int R = (int)((y + (r - y) * 0.45f) * 0.42f), G = (int)((y + (g - y) * 0.45f) * 0.42f), B = (int)((y + (b - y) * 0.45f) * 0.42f);
        sm.px[i] = (DWORD)R << 16 | (DWORD)G << 8 | (DWORD)B;
    }
    SetStretchBltMode(PK.dim.dc, HALFTONE);
    SetBrushOrgEx(PK.dim.dc, 0, 0, NULL);
    StretchBlt(PK.dim.dc, 0, 0, w, h, sm.dc, 0, 0, sw, sh, SRCCOPY);
    GdiFlush();
    Canvas_Free(&sm);
    PK.snapMon = *mon;
    PK.snapAt = GetTickCount() | 1;
    return TRUE;
}

/* La vista previa acaba de abrirse en el monitor mon: foto lista para el vistazo. */
static void PeekPrepare(const RECT *mon)
{
    if (PK.hwnd || (PK.snapAt && EqualRect(&PK.snapMon, mon) && GetTickCount() - PK.snapAt < PEEK_FRESH)) return;
    Canvas_Free(&PK.cur);
    Canvas_Free(&PK.dim);
    PK.snapAt = 0;
    if (!PeekSnapshot(mon)) { Canvas_Free(&PK.cur); Canvas_Free(&PK.dim); PK.snapAt = 0; }
}

/* Vistazo a target (con la vista previa y su dock en D). */
static void PeekShow(HWND target)
{
    RECT wr;
    MONITORINFO mi = { sizeof(mi) };
    if (!IsWindow(target) || IsIconic(target) || !GetWindowRect(target, &wr) ||
        !GetMonitorInfoW(MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST), &mi)) { PeekHide(); return; }
    if (PK.hwnd && !EqualRect(&PK.mon, &mi.rcMonitor)) PeekHide();     /* en otro monitor: otra foto */
    if (!PK.hwnd) {
        PK.mon = mi.rcMonitor;
        /* la foto preparada al abrir la vista previa, si es de este monitor y reciente */
        const BOOL ready = PK.snapAt && PK.cur.px && PK.dim.px && EqualRect(&PK.snapMon, &PK.mon)
                           && GetTickCount() - PK.snapAt < PEEK_FRESH;
        if (!ready) {
            Canvas_Free(&PK.cur);
            Canvas_Free(&PK.dim);
            if (!PeekSnapshot(&PK.mon)) { PeekHide(); return; }
        }
        /* 1 px menos de alto (abajo, bajo el dock): una ventana sin marco que cubre el monitor
         * entero es "pantalla completa" para Windows, que entonces aparta la barra y el dock, y
         * con ellos la vista previa y el propio vistazo */
        PK.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, PEEK_CLASS, L"OpenDock Vistazo", WS_POPUP,
                                  PK.mon.left, PK.mon.top, PK.mon.right - PK.mon.left, PK.mon.bottom - PK.mon.top - 1,
                                  NULL, NULL, g_inst, NULL);
        if (!PK.hwnd) { PeekHide(); return; }
        const DWORD square = 1;     /* DWMWCP_DONOTROUND: cubre el monitor entero */
        DwmSetWindowAttribute(PK.hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &square, sizeof(square));
        if (App_HideFromCapture(FALSE)) SetWindowDisplayAffinity(PK.hwnd, WDA_EXCLUDEFROMCAPTURE);
        PK.t = 0;
        /* justo por debajo del dock (y así de la vista previa, que está encima de él) */
        SetWindowPos(PK.hwnd, D.hwnd ? D.hwnd : HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    if (PK.target != target) {
        if (PK.thumb) { DwmUnregisterThumbnail(PK.thumb); PK.thumb = NULL; }
        PK.target = target;
        if (FAILED(DwmRegisterThumbnail(PK.hwnd, target, &PK.thumb))) PK.thumb = NULL;
        PK.ta = 0;
        PK.s = 0.97f;
        PK.sv = 0;
        PeekThumb();
    }
    PK.arm = NULL;
    PK.outAt = 0;
    PeekKick();
}

static void PeekFrame(void)
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    float dt = (float)(t.QuadPart - PK.last.QuadPart) / (float)f.QuadPart;
    PK.last = t;
    if (dt > 0.05f) dt = 0.05f;
    const float t0 = PK.t;
    PK.t += (1.0f - PK.t) * min(1.0f, dt * 12.0f);
    if (PK.t > 0.995f) PK.t = 1.0f;
    PK.ta += (1.0f - PK.ta) * min(1.0f, dt * 14.0f);
    if (PK.ta > 0.995f) PK.ta = 1.0f;
    for (int k = 0; k < 2; ++k) Spring2(&PK.s, &PK.sv, 1.0f, dt * 0.5f, 380.0f, PvZeta());
    if (fabsf(PK.s - 1.0f) < 0.0008f && fabsf(PK.sv) < 0.01f) { PK.s = 1.0f; PK.sv = 0; }

    /* el fondo avanza de la foto tal cual a la oscurecida: cur se acerca a dim lo que falta */
    if (PK.t != t0 && PK.dim.px && PK.cur.px) {
        const DWORD k = (DWORD)(256.0f * (PK.t - t0) / max(0.001f, 1.0f - t0) + 0.5f), ik = 256 - min(k, 256u);
        DWORD *a = PK.cur.px;
        const DWORD *b = PK.dim.px;
        const int n = PK.cur.w * PK.cur.h;
        for (int i = 0; i < n; ++i) {
            const DWORD x = a[i], y = b[i];
            a[i] = ((((x & 0xFF00FF) * ik + (y & 0xFF00FF) * k) >> 8) & 0xFF00FF) |
                   ((((x & 0x00FF00) * ik + (y & 0x00FF00) * k) >> 8) & 0x00FF00);
        }
        if (PK.t >= 1.0f) Canvas_Free(&PK.dim);     /* ya son iguales */
        InvalidateRect(PK.hwnd, NULL, FALSE);
        UpdateWindow(PK.hwnd);
    }
    PeekThumb();
    if (PK.t >= 1.0f && PK.ta >= 1.0f && PK.s == 1.0f) {
        KillTimer(PK.hwnd, 1);
        PK.animating = FALSE;
    }
}

/* Cada 50 ms con la vista previa abierta: ¿hay que mirar alguna ventana? Un hueco breve
 * entre tarjetas no lo quita (si no, al pasar de una a otra parpadearía). */
static void PeekUpdate(void)
{
    const int i = PV.closing ? -1 : PV.hot;
    const HWND want = i >= 0 && i < PV.n && PV.thumb[i] && !IsIconic(PV.wins[i]) ? PV.wins[i] : NULL;
    const DWORD now = GetTickCount();
    if (!want) {
        PK.arm = NULL;
        if (!PK.hwnd) return;
        if (!PK.outAt) PK.outAt = now | 1;
        else if (now - PK.outAt >= 160) PeekHide();
        return;
    }
    PK.outAt = 0;
    if (PK.hwnd) { if (PK.target != want) PeekShow(want); return; }
    if (PK.arm != want) { PK.arm = want; PK.armAt = now; return; }
    if (now - PK.armAt >= PEEK_DELAY) PeekShow(want);
}

static LRESULT CALLBACK PeekProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND:    return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (PK.cur.dc) BitBlt(dc, 0, 0, PK.cur.w, PK.cur.h, PK.cur.dc, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        if (h == PK.hwnd && w == 1) PeekFrame();
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        if (h == PK.hwnd) PeekHide();       /* clic fuera de la vista previa: se acabó el vistazo */
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static float PvPillW(void) { return min(PV.W * 0.5f, (float)DS(90)); }
static float PvPillH(void) { return min(PV.H * 0.4f, (float)DS(16)); }

/* Centro x al que va: sobre su icono, sin salirse del monitor (cerrando, el propio icono). */
static float PvTargetX(void)
{
    if (PV.closing) return (float)PV.cx;
    const int margin = DS(12);
    const int l = max((int)D.mon.left + margin, min((int)D.mon.right - margin - PV.W, PV.cx - PV.W / 2));
    return l + PV.W * 0.5f;
}

/* Arranca los fotogramas (solo mientras algo se mueve; en reposo no corre nada). */
static void PreviewKick(void)
{
    if (!PV.hwnd || PV.animating) return;
    PV.animating = TRUE;
    QueryPerformanceCounter(&PV.last);
    SetTimer(PV.hwnd, PREV_ANIM, 10, NULL);
}

static void PreviewDestroy(void);
static void PreviewApply(void);

/* Coloca las tarjetas de las ventanas vivas de it (con el dock de PV en D). La primera vez
 * nace como una pastilla sobre el icono; si ya estaba abierta, los muelles la llevan al
 * nuevo tamaño y al nuevo icono. */
static void PreviewBuild(const DockItem *it)
{
    PreviewDropThumbs();
    PV.n = 0;
    for (int k = 0; k < it->nwin && PV.n < PREV_MAX; ++k)
        if (IsWindow(it->wins[k])) PV.wins[PV.n++] = it->wins[k];
    if (!PV.n) { PreviewClose(); return; }

    if (PV.fontPx != DS(12) || !PV.font) {
        if (PV.font) DeleteObject(PV.font);
        if (PV.glyphs) DeleteObject(PV.glyphs);
        PV.fontPx = DS(12);
        PV.font = Gfx_Font(Gfx_UiFace(), DS(12), FW_SEMIBOLD, CLEARTYPE_QUALITY);
        PV.glyphs = Gfx_Font(Gfx_IconFace(), DS(9), FW_NORMAL, CLEARTYPE_QUALITY);
    }
    const int O = DS(8), C = DS(8), T = DS(24), G = DS(6), margin = DS(12);
    const int monW = D.mon.right - D.mon.left - 2 * margin;
    int tw = DS(200);
    if (2 * O + PV.n * (tw + 2 * C) + (PV.n - 1) * G > monW)        /* muchas: más pequeñas */
        tw = max(DS(96), (monW - 2 * O - (PV.n - 1) * G) / PV.n - 2 * C);
    const int th = tw * 5 / 8, cardW = tw + 2 * C, cardH = C + T + th + C;
    PV.W = 2 * O + PV.n * cardW + (PV.n - 1) * G;
    PV.H = 2 * O + cardH;

    for (int i = 0; i < PV.n; ++i) {
        const int x = O + i * (cardW + G), y = O, cs = DS(20);
        SetRect(&PV.card[i], x, y, x + cardW, y + cardH);
        SetRect(&PV.area[i], x + C, y + C + T, x + C + tw, y + C + T + th);
        SetRect(&PV.close[i], x + C + tw - cs, y + C + (T - cs) / 2, x + C + tw, y + C + (T - cs) / 2 + cs);
        PV.cs[i] = 1.0f; PV.csv[i] = 0; PV.ca[i] = 0; PV.xs[i] = 1.0f; PV.xsv[i] = 0;
        if (IsIconic(PV.wins[i]) || FAILED(DwmRegisterThumbnail(PV.hwnd, PV.wins[i], &PV.thumb[i]))) { PV.thumb[i] = NULL; continue; }
        SIZE src = { 0, 0 };
        DwmQueryThumbnailSourceSize(PV.thumb[i], &src);
        if (src.cx <= 0 || src.cy <= 0) { DwmUnregisterThumbnail(PV.thumb[i]); PV.thumb[i] = NULL; continue; }
        const float k = min((float)tw / src.cx, (float)th / src.cy);
        const int dw = max(1, (int)(src.cx * k)), dh = max(1, (int)(src.cy * k));
        SetRect(&PV.tdst[i], PV.area[i].left + (tw - dw) / 2, PV.area[i].top + (th - dh) / 2,
                PV.area[i].left + (tw - dw) / 2 + dw, PV.area[i].top + (th - dh) / 2 + dh);
    }
    if (PV.hot >= PV.n) PV.hot = -1;
    PV.fullDirty = TRUE;
    PV.closing = FALSE;
    if (!IsWindowVisible(PV.hwnd)) {        /* nace de la pastilla, encima de su icono */
        PV.w = PvPillW(); PV.h = PvPillH();
        PV.x = (float)PV.cx;
        PV.vw = PV.vh = PV.vx = 0;
        PV.fade = 0;
        PreviewApply();
        ShowWindow(PV.hwnd, SW_SHOWNOACTIVATE);
    }
    PreviewKick();
}

/* Lleva el estado animado a la ventana: tamaño y sitio, miniaturas (que siguen la escala
 * de su tarjeta y aparecen con el contenido) y repintado. */
static void PreviewApply(void)
{
    const int wi = max(1, (int)lroundf(PV.w)), hi = max(1, (int)lroundf(PV.h));
    PV.ox = (wi - PV.W) / 2;
    PV.oy = (hi - PV.H) / 2;
    SetWindowPos(PV.hwnd, HWND_TOPMOST, (int)lroundf(PV.x - PV.w * 0.5f), PV.bottom - hi, wi, hi,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    const BYTE op = (BYTE)(255.0f * max(0.0f, min(1.0f, PV.fade)) + 0.5f);
    for (int i = 0; i < PV.n; ++i) {
        if (!PV.thumb[i]) continue;
        const RECT *c = &PV.card[i], *t = &PV.tdst[i];
        const float k = PV.cs[i], cx = (c->left + c->right) * 0.5f, cy = (c->top + c->bottom) * 0.5f;
        DWM_THUMBNAIL_PROPERTIES tp = { 0 };
        tp.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        tp.rcDestination.left   = PV.ox + (int)lroundf(cx + (t->left - cx) * k);
        tp.rcDestination.top    = PV.oy + (int)lroundf(cy + (t->top - cy) * k);
        tp.rcDestination.right  = PV.ox + (int)lroundf(cx + (t->right - cx) * k);
        tp.rcDestination.bottom = PV.oy + (int)lroundf(cy + (t->bottom - cy) * k);
        tp.opacity = op;
        tp.fVisible = op > 2;
        tp.fSourceClientAreaOnly = FALSE;
        DwmUpdateThumbnailProperties(PV.thumb[i], &tp);
    }
    RedrawWindow(PV.hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
}

/* Un fotograma (con el dock de PV en D). */
static void PreviewFrame(void)
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    float dt = (float)(t.QuadPart - PV.last.QuadPart) / (float)f.QuadPart;
    PV.last = t;
    if (dt > 0.05f) dt = 0.05f;

    const float z = PvZeta(), tx = PvTargetX();
    const float tw = PV.closing ? PvPillW() : (float)PV.W, th = PV.closing ? PvPillH() : (float)PV.H;
    const float fw = PV.w, fh = PV.h, fx = PV.x, ff = PV.fade;
    for (int k = 0; k < 2; ++k) {
        if (PV.closing) {       /* sin rebote al cerrar: todo llega junto */
            Spring2(&PV.w, &PV.vw, tw, dt * 0.5f, 300.0f, 1.0f);
            Spring2(&PV.h, &PV.vh, th, dt * 0.5f, 300.0f, 1.0f);
            Spring2(&PV.x, &PV.vx, tx, dt * 0.5f, 300.0f, 1.0f);
        } else {
            Spring2(&PV.w, &PV.vw, tw, dt * 0.5f, 380.0f, z);
            Spring2(&PV.h, &PV.vh, th, dt * 0.5f, 420.0f, min(1.0f, z + 0.1f));
            Spring2(&PV.x, &PV.vx, tx, dt * 0.5f, 320.0f, z);
        }
    }
    if (PV.w < 1.0f) PV.w = 1.0f;
    if (PV.h < 1.0f) PV.h = 1.0f;
    if (!PV.closing) {
        const float tf = PV.h > PV.H * 0.55f ? 1.0f : 0.0f;    /* el contenido, cuando ya hay sitio */
        PV.fade += (tf - PV.fade) * min(1.0f, dt * (tf > PV.fade ? 16.0f : 26.0f));
        if (PV.fade > 0.999f) PV.fade = 1.0f;
    } else {
        /* contenido y forma van ligados a la altura: nada se apaga de golpe */
        const float span = max(1.0f, PV.H - th);
        const float prog = max(0.0f, min(1.0f, (PV.h - th) / span));
        const float g = max(0.0f, min(1.0f, (prog - 0.25f) / 0.5f));
        PV.fade = min(PV.fade, g * g * (3 - 2 * g));
        if (prog < 0.04f) { PreviewDestroy(); return; }
    }

    BOOL cards = FALSE;
    for (int i = 0; i < PV.n; ++i) {
        /* la señalada crece un poco y las demás se apartan, como las tarjetas del notch */
        const float target = PV.closing || PV.hot < 0 ? 1.0f : i == PV.hot ? 1.035f : 0.965f;
        const float b = PV.cs[i], cb = PV.ca[i];
        for (int k = 0; k < 2; ++k) Spring2(&PV.cs[i], &PV.csv[i], target, dt * 0.5f, 420.0f, z);
        if (fabsf(PV.cs[i] - target) < 0.0008f && fabsf(PV.csv[i]) < 0.01f) { PV.cs[i] = target; PV.csv[i] = 0; }
        const float ct = !PV.closing && i == PV.hot ? 1.0f : 0.0f;          /* su ✕ aparece */
        PV.ca[i] += (ct - PV.ca[i]) * min(1.0f, dt * 18.0f);
        if (fabsf(PV.ca[i] - ct) < 0.01f) PV.ca[i] = ct;
        /* la ✕ crece al acercarse y más al ponerse encima, con más rebote que el resto */
        float xt = 1.0f;
        if (!PV.closing && i == PV.hot) {
            if (PV.hotClose) xt = CLOSE_HOT;
            else {
                const RECT *x = &PV.close[i];
                const float dx = PV.mx - (x->left + x->right) * 0.5f, dy = PV.my - (x->top + x->bottom) * 0.5f;
                const float prox = max(0.0f, min(1.0f, 1.0f - (sqrtf(dx * dx + dy * dy) - DS(10)) / DS(44)));
                xt = 1.0f + (CLOSE_NEAR - 1.0f) * prox;
            }
        }
        const float xb = PV.xs[i];
        for (int k = 0; k < 2; ++k) Spring2(&PV.xs[i], &PV.xsv[i], xt, dt * 0.5f, 520.0f, min(PvZeta(), 0.42f));
        if (fabsf(PV.xs[i] - xt) < 0.002f && fabsf(PV.xsv[i]) < 0.02f) { PV.xs[i] = xt; PV.xsv[i] = 0; }
        if (PV.ca[i] != cb || PV.xs[i] != xb) PV.fullDirty = TRUE;
        if (PV.cs[i] != b || PV.ca[i] != cb || PV.xs[i] != xb) cards = TRUE;
    }

    const float moved = fabsf(PV.w - fw) + fabsf(PV.h - fh) + fabsf(PV.x - fx) + fabsf(PV.fade - ff) * 50;
    const BOOL settled = !PV.closing && !cards && moved < 0.02f
        && fabsf(PV.vw) < 1.0f && fabsf(PV.vh) < 1.0f && fabsf(PV.vx) < 1.0f
        && fabsf(PV.w - tw) < 0.5f && fabsf(PV.h - th) < 0.5f && fabsf(PV.x - tx) < 0.5f && PV.fade >= 1.0f;
    if (settled) {
        PV.w = tw; PV.h = th; PV.x = tx;
        PV.vw = PV.vh = PV.vx = 0;
        KillTimer(PV.hwnd, PREV_ANIM);
        PV.animating = FALSE;
    }
    if (settled || moved > 0.02f || cards || PV.fullDirty) PreviewApply();
    if (settled) PeekPrepare(&D.mon);       /* ya quieta: la foto para un vistazo inmediato */
}

/* Cierra con su animación de vuelta a la pastilla (lo inmediato es PreviewDestroy). */
static void PreviewClose(void)
{
    if (!PV.hwnd || PV.closing) return;
    PeekHide();
    if (!IsWindowVisible(PV.hwnd)) { PreviewDestroy(); return; }
    PV.closing = TRUE;
    PV.hot = PV.pressed = -1;
    PV.hotClose = FALSE;
    PreviewKick();
}

static void PreviewDestroy(void)
{
    if (!PV.hwnd) return;
    PeekHide();
    PeekDrop();
    const HWND h = PV.hwnd;
    PV.hwnd = NULL;             /* antes de destruir: WM_DESTROY ya no vuelve a entrar aquí */
    PreviewDropThumbs();
    KillTimer(h, PREV_TIMER);
    KillTimer(h, PREV_ANIM);
    DestroyWindow(h);
    Canvas_Free(&PV.cv);
    Canvas_Free(&PV.full);
    PV.dock = NULL;
    PV.n = 0;
    PV.closing = PV.animating = FALSE;
}

/* Abre (o cambia a) la vista previa del icono i del dock actual. */
static void PreviewOpen(int i)
{
    if (i < 0 || i >= D.count || !LiveWins(&D.items[i]) || D.menuOpen) return;
    DockItem *it = &D.items[i];
    if (PV.hwnd && PV.dock != s_d) PreviewDestroy();   /* la tenía el dock de otro monitor */
    if (!PV.hwnd) {
        PV.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, PREV_CLASS, L"OpenDock Vista previa",
                                  WS_POPUP, 0, 0, 1, 1, NULL, NULL, g_inst, NULL);
        if (!PV.hwnd) return;
        const DWORD round = 2;      /* DWMWCP_ROUND */
        DwmSetWindowAttribute(PV.hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &round, sizeof(round));
        DockLook L;
        LoadDockLook(&L);
        const DWORD edge = Gfx_Mix(L.panel, L.bar, L.light ? 0.12f : 0.16f);
        const COLORREF border = RGB((edge >> 16) & 255, (edge >> 8) & 255, edge & 255);
        DwmSetWindowAttribute(PV.hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
        if (App_HideFromCapture(FALSE)) SetWindowDisplayAffinity(PV.hwnd, WDA_EXCLUDEFROMCAPTURE);
        SetTimer(PV.hwnd, PREV_TIMER, 50, NULL);
    } else if (!PV.closing && lstrcmpiW(PV.key, ItemKey(it))) {
        PV.fade = min(PV.fade, 0.3f);       /* otra app: su contenido entra con un fundido */
    }
    PV.dock = s_d;
    lstrcpynW(PV.key, ItemKey(it), MAX_PATH);
    PV.hot = PV.pressed = -1;
    PV.hotClose = FALSE;
    PV.outSince = 0;
    /* encima del icono, a la altura que alcanza con la lupa (no salta con los botes) */
    const float base = (float)Base(), gap = (float)DS(ICON_GAP);
    float x = FirstIconX();
    for (int k = 0; k < i; ++k) x += base * D.items[k].scale + gap;
    PV.cx = D.mon.left + (int)(x + base * it->scale * 0.5f);
    const int winTop = D.mon.bottom - WinH() + D.sinkPx;
    PV.bottom = winTop + (int)(PanelBottom() - DS(DOCK_PADY) - base * MAXMAG) - DS(8);
    PreviewBuild(it);
}

/* El cursor se movió sobre el dock actual: abrir la vista previa tras un momento, o cambiarla
 * al momento si ya hay una abierta (o volver a abrirla si se estaba cerrando). */
static void PreviewHover(void)
{
    const DockItem *it = D.hot >= 0 && D.hot < D.count ? &D.items[D.hot] : NULL;
    const wchar_t *key = it ? ItemKey(it) : NULL;
    if (PV.suppress[0] && (!key || lstrcmpiW(key, PV.suppress))) PV.suppress[0] = 0;
    if (!it || !LiveWins(it) || D.menuOpen || PV.suppress[0]) {
        if (D.prevArm >= 0) { KillTimer(D.hwnd, TIMER_DPREV); D.prevArm = -1; }
        return;
    }
    if (PV.hwnd) {
        if (PV.dock != s_d || PV.closing || lstrcmpiW(PV.key, key)) PreviewOpen(D.hot);
        return;
    }
    if (D.prevArm != D.hot) { D.prevArm = D.hot; SetTimer(D.hwnd, TIMER_DPREV, PREV_DELAY, NULL); }
}

/* El contenido a tamaño completo (cambia con el ratón, no con los muelles). */
static void CloseStroke(Canvas *c, float ax, float ay, float bx, float by, float th, DWORD rgb)
{
    const float vx = bx - ax, vy = by - ay, len2 = vx * vx + vy * vy;
    GdiFlush();
    for (int y = max(0, (int)(min(ay, by) - th)); y < min(c->h, (int)(max(ay, by) + th) + 1); ++y)
        for (int x = max(0, (int)(min(ax, bx) - th)); x < min(c->w, (int)(max(ax, bx) + th) + 1); ++x) {
            const float px = x + 0.5f - ax, py = y + 0.5f - ay;
            float t = len2 > 0 ? (px * vx + py * vy) / len2 : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            const float dx = px - vx * t, dy = py - vy * t;
            Gfx_Blend(c, x, y, rgb, Gfx_Cov(sqrtf(dx * dx + dy * dy) - th * 0.5f));
        }
}

static void PreviewRenderFull(const DockLook *L)
{
    if (PV.full.w != PV.W || PV.full.h != PV.H) {
        Canvas_Free(&PV.full);
        if (!Canvas_Init(&PV.full, PV.W, PV.H)) return;
    }
    Canvas *c = &PV.full;
    Canvas_Clear(c, L->panel);
    const DockItem *it = ItemByKey(PV.key);
    if (it && D.iconGen != s_iconGen) it = NULL;        /* iconos de una caché ya vaciada */
    const int T = DS(24), is = DS(16);
    for (int i = 0; i < PV.n; ++i) {
        const RECT *r = &PV.card[i], *a = &PV.area[i];
        const float ca = PV.ca[i];
        if (ca > 0.01f) Gfx_FillRRect(c, (float)r->left, (float)r->top, (float)(r->right - r->left), (float)(r->bottom - r->top),
                                      (float)DS(10), Gfx_Mix(L->panel, L->bar, L->light ? 0.08f : 0.10f), ca);
        /* fondo del hueco de la miniatura: se ve si la ventana no llena su proporción */
        Gfx_FillRRect(c, (float)a->left, (float)a->top, (float)(a->right - a->left), (float)(a->bottom - a->top),
                      (float)DS(6), Gfx_Mix(L->panel, L->bar, 0.05f), 1.0f);
        const int ty = a->top - T;
        if (it) BlitIcon(c, it, (float)(a->left + is / 2), (float)(ty + (T + is) / 2), (float)is);
        wchar_t title[128];
        if (GetWindowTextW(PV.wins[i], title, 128) <= 0) lstrcpynW(title, it ? it->name : L"", 128);
        const int tx = a->left + (it ? is + DS(6) : 0), tr = ca > 0.01f ? PV.close[i].left - DS(4) : a->right;
        Gfx_Text(c, PV.font, title, tx, ty, max(0, tr - tx), T, L->bar, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        if (!PV.thumb[i] && it) {           /* minimizada: su icono en grande */
            const float s = min(a->right - a->left, a->bottom - a->top) * 0.55f;
            BlitIcon(c, it, (a->left + a->right) * 0.5f, (a->top + a->bottom) * 0.5f + s * 0.5f, s);
        }
        if (ca > 0.01f) {
            float cx, cy, r;
            CloseCircle(i, &cx, &cy, &r);
            const BOOL on = i == PV.hot && PV.hotClose;
            /* se enciende en rojo a medida que crece hacia "encima" */
            const float heat = max(0.0f, min(1.0f, (PV.xs[i] - 1.0f) / (CLOSE_HOT - 1.0f)));
            const DWORD idle = Gfx_Mix(L->panel, L->bar, 0.18f);
            const DWORD fill = on ? 0xE5443C : Gfx_Mix(idle, 0xE5443C, heat * 0.55f);
            const DWORD bg = Gfx_Mix(L->panel, L->bar, L->light ? 0.08f : 0.10f);
            Gfx_FillCircle(c, cx, cy, r, fill, ca);
            const float a = r * 0.36f, th = max(1.4f, r * 0.17f);
            const DWORD ink = Gfx_Mix(bg, on || heat > 0.6f ? 0xFFFFFF : L->bar, ca);
            CloseStroke(c, cx - a, cy - a, cx + a, cy + a, th, ink);
            CloseStroke(c, cx - a, cy + a, cx + a, cy - a, th, ink);
        }
    }
    GdiFlush();
    PV.fullDirty = FALSE;
}

/* Copia la tarjeta r del contenido completo, escalada k alrededor de su centro y mezclada con
 * el fondo según fade, al lienzo presentado (de wi×hi, con el contenido en ox, oy). */
static void PreviewBlitCard(const RECT *r, float k, float fade, DWORD bg, int wi, int hi)
{
    const Canvas *s = &PV.full;
    Canvas *d = &PV.cv;
    const float cx = (r->left + r->right) * 0.5f, cy = (r->top + r->bottom) * 0.5f;
    const float hw = (r->right - r->left) * 0.5f * k, hh = (r->bottom - r->top) * 0.5f * k;
    const int x0 = max(0, (int)floorf(PV.ox + cx - hw)), x1 = min(wi, (int)ceilf(PV.ox + cx + hw));
    const int y0 = max(0, (int)floorf(PV.oy + cy - hh)), y1 = min(hi, (int)ceilf(PV.oy + cy + hh));
    const float inv = 1.0f / k, maxX = (float)(r->right - 1), maxY = (float)(r->bottom - 1);
    for (int y = y0; y < y1; ++y) {
        float sy = cy + (y + 0.5f - PV.oy - cy) * inv - 0.5f;
        sy = max((float)r->top, min(maxY, sy));
        const int iy = (int)sy, iy1 = min(iy + 1, r->bottom - 1);
        const float fy = sy - iy;
        for (int x = x0; x < x1; ++x) {
            float sx = cx + (x + 0.5f - PV.ox - cx) * inv - 0.5f;
            sx = max((float)r->left, min(maxX, sx));
            const int ix = (int)sx, ix1 = min(ix + 1, r->right - 1);
            const float fx = sx - ix;
            const DWORD p = Gfx_Mix(Gfx_Mix(s->px[iy * s->w + ix], s->px[iy * s->w + ix1], fx),
                                    Gfx_Mix(s->px[iy1 * s->w + ix], s->px[iy1 * s->w + ix1], fx), fy);
            d->px[y * d->w + x] = fade >= 1.0f ? p & 0xFFFFFF : Gfx_Mix(bg, p & 0xFFFFFF, fade);
        }
    }
}

static void PreviewPaint(HDC dc)
{
    RECT rc;
    GetClientRect(PV.hwnd, &rc);
    const int wi = rc.right, hi = rc.bottom;
    if (wi <= 0 || hi <= 0) return;
    if (PV.cv.w < wi || PV.cv.h < hi) {         /* con margen: el rebote la pasa de su tamaño */
        Canvas_Free(&PV.cv);
        if (!Canvas_Init(&PV.cv, wi + wi / 5 + 8, hi + hi / 5 + 8)) return;
    }
    DockLook L;
    LoadDockLook(&L);
    if (PV.fullDirty || PV.full.w != PV.W || PV.full.h != PV.H) PreviewRenderFull(&L);
    GdiFlush();
    Canvas *c = &PV.cv;
    for (int y = 0; y < hi; ++y) {
        DWORD *row = &c->px[y * c->w];
        for (int x = 0; x < wi; ++x) row[x] = L.panel;
    }
    if (PV.full.px && PV.fade > 0.003f)
        for (int i = 0; i < PV.n; ++i) PreviewBlitCard(&PV.card[i], PV.cs[i], PV.fade, L.panel, wi, hi);
    BitBlt(dc, 0, 0, wi, hi, c->dc, 0, 0, SRCCOPY);
}

static int PreviewHit(POINT p, BOOL *onClose)
{
    *onClose = FALSE;
    if (PV.closing || PV.fade < 0.5f) return -1;
    p.x -= PV.ox;
    p.y -= PV.oy;
    for (int i = 0; i < PV.n; ++i)
        if (PtInRect(&PV.card[i], p)) {
            float cx, cy, r;
            CloseCircle(i, &cx, &cy, &r);
            const float dx = p.x - cx, dy = p.y - cy;
            *onClose = dx * dx + dy * dy <= (r + 2.0f) * (r + 2.0f);
            return i;
        }
    return -1;
}

/* Cada 50 ms con la vista previa abierta (con su dock en D). */
static void PreviewCheck(void)
{
    if (PV.closing) return;
    const DockItem *it = D.hwnd ? ItemByKey(PV.key) : NULL;
    if (!it || D.fullscreen || D.shellOpen || D.menuOpen || D.sink > 0.05f || (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) {
        PreviewClose();
        return;
    }
    /* cambiaron sus ventanas (se abrió o cerró alguna): se rehace */
    int n = 0;
    BOOL same = TRUE;
    for (int k = 0; k < it->nwin; ++k) {
        if (!IsWindow(it->wins[k])) continue;
        if (n >= PREV_MAX || PV.wins[n] != it->wins[k]) same = FALSE;
        ++n;
    }
    if (!same || min(n, PREV_MAX) != PV.n) { PreviewBuild(it); if (!PV.hwnd || PV.closing) return; }

    POINT pt;
    GetCursorPos(&pt);
    RECT wr;
    GetWindowRect(PV.hwnd, &wr);
    const BOOL inside = PtInRect(&wr, pt);
    const BOOL onIcon = D.inside && D.hot >= 0 && D.hot < D.count && &D.items[D.hot] == it;
    if (!inside && ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000)
        && WindowFromPoint(pt) != D.hwnd) { PreviewClose(); return; }      /* clic en otra parte */
    if (inside || onIcon) PV.outSince = 0;
    else if (!PV.outSince) PV.outSince = GetTickCount() | 1;
    else if (GetTickCount() - PV.outSince >= 300) { PreviewClose(); return; }
    PeekUpdate();
}

static LRESULT PreviewProcFor(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND:    return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        PreviewPaint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        if (w == PREV_TIMER) PreviewCheck();
        else if (w == PREV_ANIM) PreviewFrame();
        return 0;
    case WM_MOUSEMOVE: {
        if (!PV.tracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            PV.tracking = TrackMouseEvent(&tme);
        }
        POINT p = { (short)LOWORD(l), (short)HIWORD(l) };
        BOOL onClose;
        const int hot = PreviewHit(p, &onClose);
        PV.mx = p.x - PV.ox;
        PV.my = p.y - PV.oy;
        if (hot != PV.hot || onClose != PV.hotClose) {
            PV.hot = hot; PV.hotClose = onClose;
            PV.fullDirty = TRUE;
            PreviewKick();
        } else if (hot >= 0) PreviewKick();     /* acercándose a la ✕: que crezca */
        return 0;
    }
    case WM_MOUSELEAVE:
        PV.tracking = FALSE;
        if (PV.hot >= 0) { PV.hot = -1; PV.hotClose = FALSE; PV.fullDirty = TRUE; PreviewKick(); }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN: {
        POINT p = { (short)LOWORD(l), (short)HIWORD(l) };
        BOOL onClose;
        PV.pressed = PreviewHit(p, &onClose);
        return 0;
    }
    case WM_LBUTTONUP:
    case WM_MBUTTONUP: {
        POINT p = { (short)LOWORD(l), (short)HIWORD(l) };
        BOOL onClose;
        const int i = PreviewHit(p, &onClose);
        if (i < 0 || i != PV.pressed) return 0;
        PV.pressed = -1;
        const HWND win = PV.wins[i];
        if (onClose || m == WM_MBUTTONUP) {
            PostMessageW(win, WM_CLOSE, 0, 0);      /* la tarjeta se va cuando la ventana se cierre */
        } else {
            DockItem *it = ItemByKey(PV.key);
            if (it) StartHop(it, FALSE);
            PreviewClose();
            FocusWindow(win);
        }
        return 0;
    }
    case WM_DESTROY:
        if (PV.hwnd == h) {
            PeekHide();
            PV.hwnd = NULL; PreviewDropThumbs(); Canvas_Free(&PV.cv); Canvas_Free(&PV.full);
            PV.dock = NULL; PV.n = 0; PV.closing = PV.animating = FALSE;
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* Mientras se atiende un mensaje de la vista previa, D es el dock que la abrió. */
static LRESULT CALLBACK PreviewProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (!PV.dock || PV.hwnd != h) return m == WM_MOUSEACTIVATE ? MA_NOACTIVATE : DefWindowProcW(h, m, w, l);
    Dock *o = s_d;
    s_d = PV.dock;
    const LRESULT r = PreviewProcFor(h, m, w, l);
    s_d = o;
    return r;
}

/* ───────────────────────── Clic derecho ─────────────────────────
 * Como la barra de tareas de Windows: los archivos recientes de la app (los mismos que
 * enseña su lista de saltos), sus ventanas, abrir otra, anclar o quitar del dock,
 * ejecutar como administrador, abrir la ubicación del archivo y cerrar. Las tareas propias
 * de cada app (p. ej. "Nueva ventana InPrivate") no tienen API pública de lectura. */
#define IDM_WIN0    100
#define IDM_RECENT0 150
#define IDM_NEW     200
#define IDM_CLOSE   201
#define IDM_PREFS   202
#define IDM_PIN     203
#define IDM_ADMIN   204
#define IDM_FOLDER  205
#define IDM_ENDTASK 206
#define MAX_RECENT  8

static BOOL LnkAumid(const wchar_t *lnk, wchar_t *out, int cch)
{
    out[0] = 0;
    IShellLinkW *sl = NULL;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) return FALSE;
    IPersistFile *pf = NULL;
    if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
        if (SUCCEEDED(IPersistFile_Load(pf, lnk, STGM_READ))) {
            IPropertyStore *ps = NULL;
            if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPropertyStore, (void **)&ps))) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(IPropertyStore_GetValue(ps, &kPKEY_AumId, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
                    lstrcpynW(out, v.pwszVal, cch);
                PropVariantClear(&v);
                IPropertyStore_Release(ps);
            }
        }
        IPersistFile_Release(pf);
    }
    IShellLinkW_Release(sl);
    return out[0] != 0;
}

/* AppUserModelID implícito de un .exe sin uno propio: su ruta con la carpeta conocida
 * cambiada por su GUID ("{1AC14E77-…}\notepad.exe"), como lo calcula Windows. */
static void ImplicitAumid(const wchar_t *exe, wchar_t *out, int cch)
{
    static const KNOWNFOLDERID *kf[] = { &FOLDERID_System, &FOLDERID_SystemX86, &FOLDERID_ProgramFilesX86,
                                         &FOLDERID_ProgramFilesX64, &FOLDERID_UserProgramFiles, &FOLDERID_Windows };
    lstrcpynW(out, exe, cch);
    for (int i = 0; i < (int)(sizeof(kf) / sizeof(kf[0])); ++i) {
        PWSTR dir = NULL;
        if (FAILED(SHGetKnownFolderPath(kf[i], 0, NULL, &dir)) || !dir) continue;
        const int n = lstrlenW(dir);
        const BOOL hit = n && CompareStringOrdinal(exe, n, dir, n, TRUE) == CSTR_EQUAL && exe[n] == L'\\';
        CoTaskMemFree(dir);
        if (hit) {
            wchar_t g[40];
            if (StringFromGUID2(kf[i], g, 40) && lstrlenW(g) + lstrlenW(exe + n) < cch) wsprintfW(out, L"%s%s", g, exe + n);
            return;
        }
    }
}

/* El id con el que Windows guarda la lista de saltos de la app. */
static BOOL AppIdFor(const DockItem *it, wchar_t *out, int cch)
{
    if (it->aumid[0]) { lstrcpynW(out, it->aumid, cch); return TRUE; }
    for (int k = 0; k < it->nwin; ++k) if (IsWindow(it->wins[k]) && WindowAumid(it->wins[k], out, cch)) return TRUE;
    const int ll = lstrlenW(it->launch);
    if (ll > 4 && !lstrcmpiW(it->launch + ll - 4, L".lnk") && LnkAumid(it->launch, out, cch)) return TRUE;
    if (it->exe[0]) { ImplicitAumid(it->exe, out, cch); return TRUE; }
    return FALSE;
}

/* Archivos recientes de la app (sólo archivos del disco, que es lo que se puede abrir). */
static int RecentFiles(const DockItem *it, wchar_t paths[][MAX_PATH], wchar_t names[][64])
{
    wchar_t id[MAX_PATH];
    if (!AppIdFor(it, id, MAX_PATH)) return 0;
    IApplicationDocumentLists *dl = NULL;
    if (FAILED(CoCreateInstance(&CLSID_ApplicationDocumentLists, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IApplicationDocumentLists, (void **)&dl))) return 0;
    int n = 0;
    IObjectArray *oa = NULL;
    if (SUCCEEDED(IApplicationDocumentLists_SetAppID(dl, id)) &&
        SUCCEEDED(IApplicationDocumentLists_GetList(dl, ADLT_RECENT, 20, &IID_IObjectArray, (void **)&oa))) {
        UINT c = 0;
        IObjectArray_GetCount(oa, &c);
        for (UINT i = 0; i < c && n < MAX_RECENT; ++i) {
            IShellItem *si = NULL;
            PWSTR path = NULL, dn = NULL;
            if (FAILED(IObjectArray_GetAt(oa, i, &IID_IShellItem, (void **)&si)) || !si) continue;
            if (SUCCEEDED(IShellItem_GetDisplayName(si, SIGDN_FILESYSPATH, &path)) && path &&
                GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
                lstrcpynW(paths[n], path, MAX_PATH);
                if (SUCCEEDED(IShellItem_GetDisplayName(si, SIGDN_NORMALDISPLAY, &dn)) && dn) lstrcpynW(names[n], dn, 64);
                else lstrcpynW(names[n], BaseName(path), 64);
                ++n;
            }
            if (path) CoTaskMemFree(path);
            if (dn) CoTaskMemFree(dn);
            IShellItem_Release(si);
        }
        IObjectArray_Release(oa);
    }
    IApplicationDocumentLists_Release(dl);
    return n;
}

static void TogglePin(const DockItem *it)
{
    LoadPinLists();
    const wchar_t *key = ItemKey(it);
    wchar_t add[MAX_PATH];
    if (it->aumid[0]) wsprintfW(add, L"aumid:%.250s", it->aumid);
    else lstrcpynW(add, it->exe[0] ? it->exe : it->launch, MAX_PATH);
    if (it->pinned) {
        const int a = FindIn(s_pinAdd, s_npinAdd, add);
        if (a >= 0) RemoveAt(s_pinAdd, &s_npinAdd, a);     /* lo añadió el dock: se quita */
        else AppendTo(s_pinHide, &s_npinHide, key);         /* anclado en la barra: se oculta */
    } else {
        const int h = FindIn(s_pinHide, s_npinHide, key);
        if (h >= 0) RemoveAt(s_pinHide, &s_npinHide, h);
        else AppendTo(s_pinAdd, &s_npinAdd, add);
    }
    WriteMulti(L"DockAnclas", s_pinAdd, s_npinAdd);
    WriteMulti(L"DockOcultas", s_pinHide, s_npinHide);
    ++s_pinVer;
}

/* Anclar desde fuera del dock (el buscador): target es un AppUserModelID (con o sin
 * "aumid:"), un .exe o un .lnk. Busca la app en el dock principal (anclada o abierta);
 * si no está, tmp la describe para añadirla. */
static BOOL TargetItem(LPCWSTR target, DockItem *tmp, DockItem **found)
{
    *found = NULL;
    NewItem(tmp);
    const int n = lstrlenW(target);
    if (!wcsncmp(target, L"aumid:", 6)) lstrcpynW(tmp->aumid, target + 6, 128);
    else if (LooksAumid(target)) lstrcpynW(tmp->aumid, target, 128);
    else if (n > 4 && !lstrcmpiW(target + n - 4, L".lnk")) {
        if (!LnkAumid(target, tmp->aumid, 128)) ResolveLnk(target, tmp->exe);
        lstrcpynW(tmp->launch, target, MAX_PATH);
    } else {
        lstrcpynW(tmp->exe, target, MAX_PATH);
        lstrcpynW(tmp->launch, target, MAX_PATH);
    }
    if (!tmp->aumid[0] && !tmp->exe[0]) return FALSE;
    const Dock *p = &s_docks[0];
    for (int i = 0; i < p->count && !*found; ++i) {
        const DockItem *it = &p->items[i];
        if (tmp->aumid[0] ? !lstrcmpiW(it->aumid, tmp->aumid) : (it->exe[0] && !lstrcmpiW(it->exe, tmp->exe)))
            *found = (DockItem *)it;
    }
    return TRUE;
}

BOOL Dock_IsPinned(LPCWSTR target)
{
    DockItem tmp, *it;
    return TargetItem(target, &tmp, &it) && it && it->pinned;
}

void Dock_TogglePin(LPCWSTR target)
{
    DockItem tmp, *it;
    if (!TargetItem(target, &tmp, &it)) return;
    TogglePin(it ? it : &tmp);
    RescanAll();
}

static void RunAsAdmin(const DockItem *it)
{
    const wchar_t *target = it->exe[0] ? it->exe : it->launch;
    for (const wchar_t *q = target; *q; ++q) if (*q == L'"' || *q < 0x20) return;
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = target;
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
}

/* ¿Está "Finalizar tarea" de la barra de tareas? (Configuración → Para programadores) */
static BOOL EndTaskEnabled(void)
{
    DWORD v = 0, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\\TaskbarDeveloperSettings",
                 L"TaskbarEndTask", RRF_RT_REG_DWORD, NULL, &v, &sz);
    return v != 0;
}

/* El proceso que de verdad pinta la ventana: en las apps de la Tienda la ventana es de
 * ApplicationFrameHost y la app vive en su CoreWindow hija. */
static DWORD WindowPid(HWND w)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    wchar_t path[MAX_PATH];
    if (WindowExe(w, path) && !lstrcmpiW(BaseName(path), L"ApplicationFrameHost.exe")) {
        for (HWND c = FindWindowExW(w, NULL, NULL, NULL); c; c = FindWindowExW(w, c, NULL, NULL)) {
            DWORD cp = 0;
            GetWindowThreadProcessId(c, &cp);
            if (cp && cp != pid) return cp;
        }
    }
    return pid;
}

/* Finalizar tarea, como la barra de tareas: termina los procesos de sus ventanas. Al
 * Explorador no (es también el escritorio y la barra): a sus ventanas se les pide cerrar. */
static void DockEndTask(const DockItem *it)
{
    DWORD done[8];
    int nd = 0;
    for (int k = 0; k < it->nwin; ++k) {
        const HWND w = it->wins[k];
        if (!IsWindow(w)) continue;
        wchar_t path[MAX_PATH];
        if (WindowExe(w, path) && !lstrcmpiW(BaseName(path), L"explorer.exe")) { PostMessageW(w, WM_CLOSE, 0, 0); continue; }
        const DWORD pid = WindowPid(w);
        BOOL seen = !pid || pid == GetCurrentProcessId();
        for (int j = 0; j < nd && !seen; ++j) seen = done[j] == pid;
        if (seen) continue;
        if (nd < 8) done[nd++] = pid;
        HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (p) { TerminateProcess(p, 1); CloseHandle(p); }
    }
}

static void AppMenu(int i, POINT at)
{
    DockItem *it = &D.items[i];
    HWND fg = GetForegroundWindow();
    if (fg) fg = GetAncestor(fg, GA_ROOTOWNER);
    MenuItem m[48];
    int n = 0;
    #define ADD(id_, text_, glyph_, flags_) do { if (n < 48) { m[n].id = (id_); m[n].text = (text_); m[n].glyph = (glyph_); m[n].flags = (flags_); ++n; } } while (0)

    static wchar_t rpath[MAX_RECENT][MAX_PATH], rname[MAX_RECENT][64];
    const int nrec = RecentFiles(it, rpath, rname);
    if (nrec) {
        ADD(0, L"Recientes", 0, MI_HEADER);
        for (int r = 0; r < nrec; ++r) ADD(IDM_RECENT0 + r, rname[r], 0xE8A5, 0);
        ADD(0, NULL, 0, MI_SEPARATOR);
    }

    static wchar_t wt[8][96];
    int shown = 0;
    for (int k = 0; k < it->nwin && k < 8; ++k) {
        if (!IsWindow(it->wins[k])) continue;
        if (GetWindowTextW(it->wins[k], wt[k], 96) <= 0) lstrcpynW(wt[k], it->name, 96);
        ADD(IDM_WIN0 + k, wt[k], 0xE737, it->wins[k] == fg ? MI_CHECKED : 0);
        ++shown;
    }
    if (shown) ADD(0, NULL, 0, MI_SEPARATOR);

    ADD(IDM_NEW, shown ? L"Nueva ventana" : it->name, shown ? 0xE710 : 0xE768, 0);
    ADD(IDM_PIN, it->pinned ? L"Quitar del dock" : L"Anclar al dock", it->pinned ? 0xE77A : 0xE718, 0);
    const BOOL classic = !it->aumid[0] && (it->exe[0] || it->launch[0]);
    if (classic) {
        ADD(IDM_ADMIN, L"Ejecutar como administrador", 0xE7EF, 0);
        if (it->exe[0]) ADD(IDM_FOLDER, L"Abrir ubicaci\x00F3n del archivo", 0xE838, 0);
    }
    if (shown) {
        ADD(0, NULL, 0, MI_SEPARATOR);
        ADD(IDM_CLOSE, shown > 1 ? L"Cerrar todas las ventanas" : L"Cerrar ventana", 0xE711, 0);
        if (EndTaskEnabled()) ADD(IDM_ENDTASK, L"Finalizar tarea", 0xE7BA, MI_DANGER);
    }
    ADD(0, NULL, 0, MI_SEPARATOR);
    ADD(IDM_PREFS, L"Ajustes del dock\x2026", 0xE713, 0);
    #undef ADD

    PreviewClose();
    D.menuOpen = TRUE;                           /* con el menú abierto el dock no se esconde */
    const int cmd = Menu_Track(m, n, at);
    D.menuOpen = FALSE;
    D.leaveAt = GetTickCount();
    if (!D.hwnd) return;                         /* su monitor se quitó con el menú abierto */
    if (g_cfg.dockAutoHide) SetTimer(D.hwnd, TIMER_DSINK, SINK_DELAY, NULL);
    if (i >= D.count || &D.items[i] != it) return;     /* el dock se rehízo mientras tanto */
    if (cmd >= IDM_WIN0 && cmd < IDM_WIN0 + it->nwin) { StartHop(it, FALSE); FocusWindow(it->wins[cmd - IDM_WIN0]); }
    else if (cmd >= IDM_RECENT0 && cmd < IDM_RECENT0 + nrec) App_ShellOpen(rpath[cmd - IDM_RECENT0]);
    else if (cmd == IDM_NEW)     { StartHop(it, TRUE); Launch(it); }
    else if (cmd == IDM_PIN)     { TogglePin(it); RescanAll(); }
    else if (cmd == IDM_ADMIN)   RunAsAdmin(it);
    else if (cmd == IDM_FOLDER)  App_ShellSelect(it->exe);
    else if (cmd == IDM_CLOSE)   { for (int k = 0; k < it->nwin; ++k) if (IsWindow(it->wins[k])) PostMessageW(it->wins[k], WM_CLOSE, 0, 0); }
    else if (cmd == IDM_ENDTASK) DockEndTask(it);
    else if (cmd == IDM_PREFS)   Panel_ShowTab(2);
}

/* ───────────────────────── Barra de tareas de Windows ─────────────────────────
 * Se pone en autoocultar (para que el área de trabajo ocupe toda la pantalla) y además
 * se ocultan sus ventanas, así ni asoma al llevar el cursor abajo. Al salir se restaura
 * el estado previo, que queda guardado en el registro por si el proceso muere. */
static BOOL CALLBACK ShowTrayProc(HWND w, LPARAM show)
{
    wchar_t cls[40];
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;          /* la de la bandeja propia no se toca */
    if (GetClassNameW(w, cls, 40) && (!lstrcmpW(cls, L"Shell_TrayWnd") || !lstrcmpW(cls, L"Shell_SecondaryTrayWnd"))) {
        if (show && !IsWindowVisible(w)) ShowWindow(w, SW_SHOWNA);
        else if (!show && IsWindowVisible(w)) ShowWindow(w, SW_HIDE);
    }
    return TRUE;
}

static void SetTaskbarAutohide(BOOL hide)
{
    APPBARDATA abd = { sizeof(abd) };
    abd.hWnd = Tray_Explorer();
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

/* Solo las barras de Explorer (la principal y las de otros monitores), sin recorrer todas
 * las ventanas del sistema. */
static void ShowTaskbars(BOOL show)
{
    static const wchar_t *kCls[] = { L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd" };
    for (int c = 0; c < 2; ++c)
        for (HWND w = FindWindowExW(NULL, NULL, kCls[c], NULL); w; w = FindWindowExW(NULL, w, kCls[c], NULL))
            ShowTrayProc(w, show);
}

static void SetTaskbarOff(BOOL off)
{
    SetTaskbarAutohide(off);
    ShowTaskbars(!off);
}

void Dock_RestoreTaskbar(void) { SetTaskbarOff(FALSE); }

/* La bandeja de Windows (iconos de Tailscale, OneDrive…) vive en la barra de tareas: para
 * usarla se deja ver la barra un momento; al irse el foco de ella, el dock la vuelve a ocultar. */
void Dock_TrayPeek(BOOL on)
{
    Dock *d = PRIMARY;           /* la barra de tareas la gobierna el dock principal */
    if (!d->hwnd || !g_cfg.dockHideTaskbar) return;
    d->peek = on;
    d->peekAt = GetTickCount();
    if (on) {
        ShowTaskbars(TRUE);
        SetTimer(d->hwnd, TIMER_DPEEK, 700, NULL);
    } else {                     /* el panel ya está arriba: la barra de tareas sobra */
        KillTimer(d->hwnd, TIMER_DPEEK);
        if (!AnyShellOpen()) SetTaskbarOff(TRUE);
    }
}

/* ───────────────────────── Ventana ───────────────────────── */
/* Monitor del dock actual: el principal para s_docks[0]; los demás conservan el suyo. */
static void DockMeasure(void)
{
    HMONITOR m = s_d == PRIMARY ? MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY)
                                : MonitorFromRect(&D.mon, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(m, &mi);
    D.mon = mi.rcMonitor;
    D.dpi = MonitorDpi(m);      /* la caché de iconos va por tamaño: no hace falta vaciarla */
}

/* Reserva media altura del dock: las ventanas maximizadas llegan hasta la mitad del panel
 * (no quedan tapadas detrás de todo el dock ni dejan una franja vacía debajo). */
static void AppBarPos(void)
{
    if (!D.appbar) return;
    /* oculto: las ventanas usan toda la altura y el dock asoma por encima */
    const int reserve = g_cfg.dockAutoHide && g_cfg.dockWinFull ? 0 : DS(BOTTOM_MARGIN) + (int)(PanelH() * 0.5f);
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
    const BOOL exclude = App_HideFromCapture(Blur() != 0);
    if (!D.hwnd) return;
    SetWindowDisplayAffinity(D.hwnd, exclude ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
    if (Blur()) {
        SetTimer(D.hwnd, TIMER_DBLUR, 3000, NULL);
        if (!s_moveHook)
            s_moveHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, NULL, WinHook, 0, 0,
                                         WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    } else {
        KillTimer(D.hwnd, TIMER_DBLUR);
        if (s_moveHook) { UnhookWinEvent(s_moveHook); s_moveHook = NULL; }
    }
}

static void CALLBACK FgHook(HWINEVENTHOOK hk, DWORD ev, HWND w, LONG obj, LONG child, DWORD th, DWORD t)
{
    (void)hk; (void)ev; (void)obj; (void)child; (void)th; (void)t;
    if (!PRIMARY->hwnd) return;
    /* ¿se abrió Inicio o Buscar? Windows lo dibuja pegado a su barra y no se puede mover;
     * solo se aparta el dock del monitor donde sale */
    wchar_t path[MAX_PATH];
    const BOOL shell = w && WindowExe(w, path) &&
                       (!lstrcmpiW(BaseName(path), L"SearchHost.exe") || !lstrcmpiW(BaseName(path), L"StartMenuExperienceHost.exe"));
    RECT smon = { 0 };
    MONITORINFO mi = { sizeof(mi) };
    if (shell && GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi)) smon = mi.rcMonitor;
    if (AnyShellOpen() && !shell && g_cfg.dockHideTaskbar && !PRIMARY->fullscreen) {
        /* la barra asomó con Inicio y Explorer la vuelve a mostrar un instante después
         * de cerrarlo: se vigila unos segundos para ocultarla en cuanto aparezca */
        SetTaskbarOff(TRUE);
        PRIMARY->taskWatch = 16;
        SetTimer(PRIMARY->hwnd, TIMER_DTASK, 120, NULL);
    }
    Dock *o = s_d;
    for (int k = 0; k < MAX_DOCKS; ++k) {
        s_d = &s_docks[k];
        if (!D.hwnd || D.fullscreen) continue;
        D.shellOpen = shell && (EqualRect(&smon, &D.mon) || IsRectEmpty(&smon));
        if (D.shellOpen) { D.inside = FALSE; D.hot = -1; }
        else Rescan();      /* barato: los iconos vienen de la caché */
        Kick();
    }
    s_d = o;
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
    if (!PRIMARY->hwnd || obj != OBJID_WINDOW || child != CHILDID_SELF || !w) return;
    if (ev == EVENT_OBJECT_SHOW) {
        wchar_t cls[32];
        if (GetClassNameW(w, cls, 32) && (!lstrcmpW(cls, L"Shell_TrayWnd") || !lstrcmpW(cls, L"Shell_SecondaryTrayWnd"))) {
            /* Explorer volvió a mostrar su barra: fuera, salvo con Inicio o la bandeja abiertos */
            if (g_cfg.dockHideTaskbar && !AnyShellOpen() && !PRIMARY->peek)
                SetTimer(PRIMARY->hwnd, TIMER_DTASK, 60, NULL), PRIMARY->taskWatch = 3;
            return;
        }
    }
    const BOOL moved = ev == EVENT_OBJECT_LOCATIONCHANGE;
    if (moved && !(GetAncestor(w, GA_ROOT) == w && IsWindowVisible(w))) return;
    const BOOL app = ev == EVENT_OBJECT_SHOW && IsAppWindow(w);
    Dock *o = s_d;
    for (int k = 0; k < MAX_DOCKS; ++k) {
        s_d = &s_docks[k];
        if (!D.hwnd) continue;
        if (moved) {                                       /* algo se movió: quizá detrás del dock */
            if (!HiddenAway()) SetTimer(D.hwnd, TIMER_DBLURQ, 160, NULL);
        } else if (ev == EVENT_OBJECT_SHOW) {
            if (app) SetTimer(D.hwnd, TIMER_DRESCAN, 150, NULL);
            if (Blur()) SetTimer(D.hwnd, TIMER_DBLURQ, 200, NULL);
        } else {
            /* se ocultó o se cerró: solo importa si era de una app del dock */
            if (KnownWindow(w)) SetTimer(D.hwnd, TIMER_DRESCAN, 150, NULL);
            if (Blur() && ev == EVENT_OBJECT_HIDE) SetTimer(D.hwnd, TIMER_DBLURQ, 200, NULL);
        }
    }
    s_d = o;
}

static void StartHooks(void)
{
    if (!s_fgHook)
        s_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, FgHook, 0, 0,
                                   WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    /* ventanas que se abren, se cierran o se ocultan: los docks se enteran al momento */
    if (!s_winHook)
        s_winHook = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE, NULL, WinHook, 0, 0,
                                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

static void StopHooks(void)
{
    if (s_winHook) { UnhookWinEvent(s_winHook); s_winHook = NULL; }
    if (s_moveHook) { UnhookWinEvent(s_moveHook); s_moveHook = NULL; }
    if (s_fgHook) { UnhookWinEvent(s_fgHook); s_fgHook = NULL; }
}

/* Ocultar dock: la zona que lo mantiene arriba es el panel (con sus iconos aumentados) y
 * todo lo que queda debajo hasta el borde de la pantalla, aunque ahí no haya nada que
 * dibujar. Se mira la posición del cursor, no los mensajes de ratón: así ni el hueco ni el
 * rebote al subir lo "sueltan". Ya arriba, la zona se mide desde su sitio final. */
static BOOL InDockZone(POINT pt)
{
    if (pt.x < D.mon.left || pt.x >= D.mon.right || pt.y < D.mon.top || pt.y >= D.mon.bottom || D.panelW <= 0) return FALSE;
    const int x = pt.x - D.mon.left;
    if (x < D.panelX - DS(40) || x > D.panelX + D.panelW + DS(40)) return FALSE;
    const int winTop = D.mon.bottom - WinH() + (D.atDock ? 0 : D.sinkPx);
    int top = winTop + (D.atDock ? min(D.iconTop - DS(6), D.panelY - DS(10)) : D.panelY - DS(10));
    top = min(top, D.mon.bottom - DS(2));          /* del todo oculto: basta tocar el borde */
    return pt.y >= top;
}

static void PollCursor(void)
{
    POINT pt;
    if (!GetCursorPos(&pt) || D.fullscreen || D.shellOpen) return;
    {   /* lejos del borde de abajo (o en otro monitor) no llega en 150 ms: cada 150 ms basta */
        const BOOL nearEdge = pt.x >= D.mon.left && pt.x < D.mon.right && D.mon.bottom - pt.y <= 260;
        const UINT want = !D.atDock && !nearEdge ? 150 : 50;
        if (want != D.pollEvery) { D.pollEvery = want; SetTimer(D.hwnd, TIMER_DPOLL, want, NULL); }
    }
    const BOOL at = InDockZone(pt);
    if (at == D.atDock) return;
    D.atDock = at;
    if (!at) { D.leaveAt = GetTickCount(); SetTimer(D.hwnd, TIMER_DSINK, SINK_DELAY, NULL); }
    else if (Blur() && g_cfg.dockAutoHide == 2) {   /* el vidrio, desde donde va a quedar */
        const int keep = D.sinkPx;
        D.sinkPx = 0;
        CaptureBackdrop();
        D.sinkPx = keep;
    }
    Kick();
}

static void ApplyAutoHide(void)
{
    if (!D.hwnd) return;
    if (g_cfg.dockAutoHide) { D.pollEvery = 50; SetTimer(D.hwnd, TIMER_DPOLL, 50, NULL); }
    else { KillTimer(D.hwnd, TIMER_DPOLL); D.atDock = FALSE; }
}

static void DockApplyFullscreen(void)
{
    const BOOL fs = D.fsSys || D.fsFg;
    if (!D.hwnd || fs == D.fullscreen) return;
    D.fullscreen = fs;
    if (fs && PreviewOpenHere()) PreviewDestroy();
    ShowWindow(D.hwnd, fs ? SW_HIDE : SW_SHOWNOACTIVATE);
    if (!fs) { Rescan(); Kick(); }
}

/* La pantalla completa se mira por monitor: un juego en uno no esconde el dock del otro. */
void Dock_FullscreenFg(void)
{
    Dock *o = s_d;
    for (int k = 0; k < MAX_DOCKS; ++k) {
        s_d = &s_docks[k];
        if (!D.hwnd) continue;
        D.fsFg = App_FullscreenOn(&D.mon);
        DockApplyFullscreen();
    }
    s_d = o;
}

/* ¿La ventana de delante está en el monitor de este dock? */
static BOOL ForegroundHere(void)
{
    HWND fg = GetForegroundWindow();
    MONITORINFO mi = { sizeof(mi) };
    return fg && GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi) && EqualRect(&mi.rcMonitor, &D.mon);
}

static LRESULT DockProcFor(HWND h, UINT m, WPARAM w, LPARAM l)
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
        else if (w == ABN_FULLSCREENAPP) { D.fsSys = (BOOL)l && ForegroundHere(); DockApplyFullscreen(); }   /* solo en su monitor */
        return 0;
    case WM_WINDOWPOSCHANGED: {
        APPBARDATA abd = { sizeof(abd) };
        abd.hWnd = h;
        if (D.appbar) SHAppBarMessage(ABM_WINDOWPOSCHANGED, &abd);
        break;
    }
    case WM_DFRAME:
        InterlockedExchange(&D.framePending, 0);
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
        PreviewHover();
        Kick();
        return 0;
    }
    case WM_MOUSELEAVE:
        D.inside = FALSE;
        D.hot = -1;
        if (D.prevArm >= 0) { KillTimer(h, TIMER_DPREV); D.prevArm = -1; }
        PV.suppress[0] = 0;
        D.leaveAt = GetTickCount();
        if (g_cfg.dockAutoHide) SetTimer(h, TIMER_DSINK, SINK_DELAY, NULL);
        Kick();             /* deja que las escalas vuelvan a 1 y luego se detiene */
        return 0;
    case WM_LBUTTONDOWN:
        D.pressed = HitIndex((short)LOWORD(l));
        return 0;
    case WM_LBUTTONUP: {
        const int hit = HitIndex((short)LOWORD(l));
        if (hit >= 0 && hit == D.pressed) {
            const DockItem *it = &D.items[hit];
            const BOOL open = PreviewOpenHere() && !PV.closing && !lstrcmpiW(PV.key, ItemKey(it));
            if (D.prevArm >= 0) { KillTimer(h, TIMER_DPREV); D.prevArm = -1; }
            if (LiveWins(it) >= 2 && !open) {           /* varias ventanas: se elige en la vista previa */
                PV.suppress[0] = 0;
                PreviewOpen(hit);
            } else {
                lstrcpynW(PV.suppress, ItemKey(it), MAX_PATH);  /* sin reabrirse bajo el cursor */
                PreviewClose();
                if (LiveWins(it) < 2) Activate(hit);
            }
        }
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
            /* Windows avisa de pantalla completa para todo el sistema: cuenta en el
             * monitor de la ventana de delante */
            QUERY_USER_NOTIFICATION_STATE qs;
            BOOL fs = SUCCEEDED(SHQueryUserNotificationState(&qs)) &&
                      (qs == QUNS_BUSY || qs == QUNS_RUNNING_D3D_FULL_SCREEN || qs == QUNS_PRESENTATION_MODE);
            D.fsSys = fs && ForegroundHere();
            DockApplyFullscreen();
            if (!D.fullscreen && !D.inside) { Rescan(); Kick(); }   /* repaso de seguridad */
            if (s_d == PRIMARY && g_cfg.dockHideTaskbar && !AnyShellOpen() && !D.peek) SetTaskbarOff(TRUE);
        } else if (w == TIMER_DRESCAN) {
            KillTimer(h, TIMER_DRESCAN);
            if (!D.fullscreen) { Rescan(); Kick(); }
        } else if (w == TIMER_DBLURQ) {
            KillTimer(h, TIMER_DBLURQ);
            if (!D.fullscreen && !D.wantFrames && CaptureBackdrop()) Render();
        } else if (w == TIMER_DPOLL) {
            PollCursor();
        } else if (w == TIMER_DPREV) {
            KillTimer(h, TIMER_DPREV);
            if (D.inside && D.hot >= 0 && D.hot == D.prevArm && !PV.suppress[0]) PreviewOpen(D.hot);
            D.prevArm = -1;
        } else if (w == TIMER_DSINK) {
            KillTimer(h, TIMER_DSINK);
            Kick();
        } else if (w == TIMER_DPEEK) {          /* solo en el principal */
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
                if (g_cfg.dockHideTaskbar && !AnyShellOpen()) SetTaskbarOff(TRUE);
            }
        } else if (w == TIMER_DTASK) {          /* solo en el principal */
            const BOOL shell = AnyShellOpen();
            if (!shell && !D.peek && g_cfg.dockHideTaskbar) SetTaskbarOff(TRUE);
            if (--D.taskWatch <= 0 || shell) KillTimer(h, TIMER_DTASK);
        } else if (w == TIMER_DBLUR) {
            /* el fondo cambió (ventana movida, vídeo…): recomponer el vidrio */
            if (!D.fullscreen && !D.wantFrames && CaptureBackdrop()) Render();
        }
        return 0;
    case WM_DESTROY: {
        KillTimer(h, TIMER_DSTATUS);
        KillTimer(h, TIMER_DBLUR);
        KillTimer(h, TIMER_DTASK);
        KillTimer(h, TIMER_DRESCAN);
        KillTimer(h, TIMER_DBLURQ);
        KillTimer(h, TIMER_DPEEK);
        KillTimer(h, TIMER_DSINK);
        KillTimer(h, TIMER_DPOLL);
        KillTimer(h, TIMER_DPREV);
        if (PreviewOpenHere()) PreviewDestroy();
        if (D.appbar) {
            APPBARDATA abd = { sizeof(abd) };
            abd.hWnd = h;
            SHAppBarMessage(ABM_REMOVE, &abd);      /* las ventanas recuperan el espacio */
            D.appbar = FALSE;
        }
        D.count = 0;
        D.npinCache = 0;
        D.pinSig = 0;
        Canvas_Free(&D.frame);
        Canvas_Free(&D.back);
        Canvas_Free(&D.raw);
        Canvas_Free(&D.gstrip);
        ZeroMemory(&D.dirtyPrev, sizeof(D.dirtyPrev));
        D.hwnd = NULL;
        PacerOn(FALSE);
        /* los ganchos y la caché de iconos son de todos: se quitan con el último dock */
        BOOL any = FALSE;
        for (int k = 0; k < MAX_DOCKS && !any; ++k) any = s_docks[k].hwnd != NULL;
        if (!any) { StopHooks(); ClearIconCache(); }
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

/* Cada ventana lleva su dock; mientras se atiende un mensaje, D es ese dock. */
static LRESULT CALLBACK DockProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)l)->lpCreateParams);
    Dock *d = (Dock *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!d) return DefWindowProcW(h, m, w, l);
    Dock *o = s_d;
    s_d = d;
    const LRESULT r = DockProcFor(h, m, w, l);
    s_d = o;
    return r;
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

    WNDCLASSEXW pc = { sizeof(pc) };
    pc.style         = CS_DROPSHADOW;
    pc.lpfnWndProc   = PreviewProc;
    pc.hInstance     = g_inst;
    pc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    pc.lpszClassName = PREV_CLASS;
    RegisterClassExW(&pc);

    WNDCLASSEXW kc = { sizeof(kc) };
    kc.lpfnWndProc   = PeekProc;
    kc.hInstance     = g_inst;
    kc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    kc.lpszClassName = PEEK_CLASS;
    RegisterClassExW(&kc);
}

/* Crea el dock d en el monitor mon (el principal, si d es s_docks[0]). */
static void CreateDock(Dock *d, const RECT *mon)
{
    Dock *o = s_d;
    s_d = d;
    ZeroMemory(d, sizeof(*d));
    D.mon = *mon;
    DockMeasure();
    D.hot = D.pressed = D.prevArm = -1;
    QueryPerformanceFrequency(&D.freq);
    QueryPerformanceCounter(&D.last);
    const HWND h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                   DOCK_CLASS, L"OpenDock Dock", WS_POPUP,
                                   D.mon.left, D.mon.bottom - WinH(), D.mon.right - D.mon.left, WinH(),
                                   NULL, NULL, g_inst, d);
    if (h) {
        D.hwnd = h;
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
        ApplyAutoHide();
    }
    s_d = o;
}

/* Vuelve a colocar el dock actual en su monitor (resolución, escala, tamaño de icono). */
static void PlaceDock(void)
{
    DockMeasure();
    AppBarPos();
    SetWindowPos(D.hwnd, HWND_TOPMOST, D.mon.left, D.mon.bottom - WinH(), D.mon.right - D.mon.left, WinH(), SWP_NOACTIVATE);
    Canvas_Free(&D.back);
    Rescan();
    Render();
}

typedef struct { RECT r[MAX_DOCKS]; int n; } MonList;

static BOOL CALLBACK MonProc(HMONITOR m, HDC dc, LPRECT rc, LPARAM lp)
{
    (void)dc; (void)rc;
    MonList *ml = (MonList *)lp;
    MONITORINFO mi = { sizeof(mi) };
    if (ml->n < MAX_DOCKS && GetMonitorInfoW(m, &mi) && !(mi.dwFlags & MONITORINFOF_PRIMARY)) ml->r[ml->n++] = mi.rcMonitor;
    return TRUE;
}

/* Un dock en cada monitor que no es el principal: se quitan los de monitores que ya no
 * están, se recolocan los que siguen y se crean los que faltan. */
static void SyncSecondaries(void)
{
    MonList ml = { 0 };
    EnumDisplayMonitors(NULL, NULL, MonProc, (LPARAM)&ml);
    BOOL have[MAX_DOCKS] = { 0 };
    Dock *o = s_d;
    for (int k = 1; k < MAX_DOCKS; ++k) {
        s_d = &s_docks[k];
        if (!D.hwnd) continue;
        int j = 0;
        while (j < ml.n && (have[j] || !EqualRect(&ml.r[j], &D.mon))) ++j;
        if (j < ml.n) { have[j] = TRUE; PlaceDock(); }
        else DestroyWindow(D.hwnd);
    }
    s_d = o;
    for (int j = 0; j < ml.n; ++j) {
        if (have[j]) continue;
        for (int k = 1; k < MAX_DOCKS; ++k)
            if (!s_docks[k].hwnd) { CreateDock(&s_docks[k], &ml.r[j]); break; }
    }
}

/* Primero los secundarios: el principal se lleva al final los ganchos y la caché. */
static void DestroyDocks(void)
{
    for (int k = MAX_DOCKS - 1; k >= 0; --k)
        if (s_docks[k].hwnd) DestroyWindow(s_docks[k].hwnd);
}

void Dock_Apply(void)
{
    if (!g_cfg.dock) {
        DestroyDocks();
        Dock_RestoreTaskbar();
        return;
    }
    if (!PRIMARY->hwnd) {
        const RECT none = { 0 };
        DestroyDocks();             /* por si quedó alguno suelto */
        CreateDock(PRIMARY, &none);
        if (!PRIMARY->hwnd) return;
        StartHooks();
    } else {
        Dock *o = s_d;
        for (int k = 0; k < MAX_DOCKS; ++k) {
            s_d = &s_docks[k];
            if (D.hwnd) { Rescan(); Render(); }
        }
        s_d = o;
    }
    SyncSecondaries();
    SetTaskbarOff(g_cfg.dockHideTaskbar);
}

void Dock_ConfigChanged(void)
{
    Dock *o = s_d;
    for (int k = 0; k < MAX_DOCKS; ++k) {
        s_d = &s_docks[k];
        if (!D.hwnd) continue;
        ApplyCapture();
        PlaceDock();        /* con otro tamaño de icono la caché pide la nueva resolución */
        ApplyAutoHide();
        Kick();             /* ocultar dock activado o quitado: que baje o suba */
    }
    s_d = o;
}

/* Cambios de resolución, escala o monitores (conectar o quitar uno). */
void Dock_Reposition(void)
{
    if (!PRIMARY->hwnd) return;
    Dock *o = s_d;
    s_d = PRIMARY;
    PlaceDock();
    s_d = o;
    SyncSecondaries();
}

void Dock_Raise(void)
{
    for (int k = 0; k < MAX_DOCKS; ++k) {
        const Dock *d = &s_docks[k];
        if (d->hwnd && !d->fullscreen && App_Covered(d->hwnd))
            SetWindowPos(d->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
}

void Dock_Destroy(void)
{
    DestroyDocks();
    PreviewDestroy();
    if (PV.font) { DeleteObject(PV.font); PV.font = NULL; }
    if (PV.glyphs) { DeleteObject(PV.glyphs); PV.glyphs = NULL; }
    PV.fontPx = 0;
}
