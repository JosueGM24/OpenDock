/*
 * OpenDock (antes OpenDock) — esquinas redondeadas, notch, barra superior y dock para Windows.
 *
 * Cómo funciona:
 *   Por cada monitor crea 4 ventanas diminutas (una por esquina) que son:
 *     - layered + per-pixel alpha (UpdateLayeredWindow): máscara negra antialiasada
 *     - click-through (WS_EX_TRANSPARENT): nunca bloquean el ratón
 *     - topmost + tool window: encima de todo y fuera de Alt+Tab / barra de tareas
 *     - excluidas de capturas (WDA_EXCLUDEFROMCAPTURE, Win10 2004+)
 *   El radio se escala según el DPI de cada monitor (Per-Monitor V2).
 *   En pantallas OLED el negro = píxel apagado de verdad.
 *
 * Control: icono en la bandeja (clic = configuración, clic derecho = menú) + atajos
 *   Ctrl+Alt+R            activar / desactivar
 *   Ctrl+Alt+RePág/AvPág  radio +2 / -2 px
 *
 * CLI:
 *   OpenDock.exe --radius 16   (si ya está abierto, actualiza la instancia viva)
 *   OpenDock.exe --settings    (abre la configuración)
 *   OpenDock.exe --exit        (cierra la instancia en ejecución)
 *   OpenDock.exe --uninstall   (lo usa Configuración → Aplicaciones)
 */
#include "app.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <math.h>
#include <tlhelp32.h>
#include "resource.h"

#define CTRL_CLASS      L"OpenDock.Controller"
#define CORNER_CLASS    L"OpenDock.Corner"

#define IDM_TOGGLE      100
#define IDM_CAPTURE     101
#define IDM_STARTUP     102
#define IDM_EXIT        103
#define IDM_SETTINGS    104
#define IDM_MENUBAR     105
#define IDM_DOCK        106
#define IDM_UPDATE      107
#define IDM_CHECKUPD    108
#define IDM_RADIUS_BASE 200

#define TIMER_TOPMOST   1
#define TIMER_REBUILD   2
#define TIMER_CLIP      3
#define TIMER_SHOTFILE  4
#define TIMER_EDGE      5
#define TIMER_WN        6
#define TIMER_WNPOLL    7
#define TIMER_FSCHECK   20      /* la ventana de delante cambió de tamaño: ¿pantalla completa? */
#define TIMER_DNDHINT   21      /* poco después de arrancar: ¿hace falta sugerir "No molestar"? */

#define HK_TOGGLE       1
#define HK_UP           2
#define HK_DOWN         3
#define HK_BAR          4
#define HK_DOCK         5

#define MAX_CORNERS     64          /* 16 monitores x 4 esquinas */

static const int kPresets[] = { 6, 8, 10, 12, 14, 16, 20, 24, 32, 40, 56 };

HINSTANCE g_inst;
HWND      g_ctrl;
Config    g_cfg = { 12, TRUE, TRUE, TRUE, TRUE, TRUE, FALSE, TRUE, MAT_OLED, 1, 0, 1, 5000, 0, FALSE, 0, FALSE, TRUE, FALSE, TRUE, 1, 2, 1, FALSE, 0, TRUE, TRUE, 55 };

static HWND      g_corners[MAX_CORNERS];
static struct { int x, y, s, which; RECT mon; int affinity; } g_cinfo[MAX_CORNERS];   /* para repintar solo las de arriba */
static int       g_count;
static NOTIFYICONDATAW g_nid;
static UINT      g_wmTaskbarCreated;
static HWINEVENTHOOK g_fgHook, g_locHook;
static BOOL      g_noSave;                  /* tras desinstalar: no volver a crear la clave */
static wchar_t   g_relaunch[MAX_PATH];
static wchar_t   g_relaunchArgs[48] = L"--installed";   /* con qué se relanza (instalar o actualizar) */
static CRITICAL_SECTION g_shotLock;         /* g_shotPending: la deja ShotWatcher, la recoge WM_SHOTFILE */
static wchar_t   g_shotPending[MAX_PATH];      /* tras instalar: arrancar la copia instalada */
static DWORD     g_clipSeq;                 /* último cambio de portapapeles revisado */
static wchar_t   g_shotPath[MAX_PATH];      /* captura guardada pendiente de asociar al aviso */
static DWORD     g_shotTick;
static BOOL      g_wnStarted;               /* lector de notificaciones de Windows activo */
static int       g_edgeTicks;               /* cuánto lleva el cursor pegado arriba */

/* ───────────────────────── Seguridad ─────────────────────────
 * Endurece el proceso antes de cargar nada:
 *   - DLLs solo desde System32 (evita DLL hijacking desde Descargas)
 *   - sin DLLs de red/baja integridad, ni inyección vía AppInit/hooks heredados
 *   - sin código generado en tiempo de ejecución, solo fuentes del sistema
 *   - solo binarios firmados por Microsoft pueden cargarse a partir de aquí
 *   - el heap termina el proceso si detecta corrupción                        */
static void Harden(void)
{
    HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, NULL, 0);
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE | BASE_SEARCH_PATH_PERMANENT);

    PROCESS_MITIGATION_IMAGE_LOAD_POLICY img = { 0 };
    img.NoRemoteImages = 1;
    img.NoLowMandatoryLabelImages = 1;
    img.PreferSystem32Images = 1;
    SetProcessMitigationPolicy(ProcessImageLoadPolicy, &img, sizeof(img));

    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY ext = { 0 };
    ext.DisableExtensionPoints = 1;
    SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &ext, sizeof(ext));

    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dyn = { 0 };
    dyn.ProhibitDynamicCode = 1;
    SetProcessMitigationPolicy(ProcessDynamicCodePolicy, &dyn, sizeof(dyn));

    PROCESS_MITIGATION_FONT_DISABLE_POLICY font = { 0 };
    font.DisableNonSystemFonts = 1;
    SetProcessMitigationPolicy(ProcessFontDisablePolicy, &font, sizeof(font));

    PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY sig = { 0 };
    sig.MicrosoftSignedOnly = 1;
    SetProcessMitigationPolicy(ProcessSignaturePolicy, &sig, sizeof(sig));
}

/* ───────────────────────── DPI ───────────────────────── */
typedef HRESULT (WINAPI *PFN_GetDpiForMonitor)(HMONITOR, int, UINT *, UINT *);
static PFN_GetDpiForMonitor pGetDpiForMonitor;

static void InitDpi(void)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); /* respaldo del manifest */
    HMODULE sh = LoadLibraryExW(L"shcore.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (sh) pGetDpiForMonitor = (PFN_GetDpiForMonitor)(void *)GetProcAddress(sh, "GetDpiForMonitor");
}

UINT MonitorDpi(HMONITOR m)
{
    UINT x = 96, y = 96;
    if (pGetDpiForMonitor && SUCCEEDED(pGetDpiForMonitor(m, 0 /*MDT_EFFECTIVE_DPI*/, &x, &y)))
        return x;
    return 96;
}

/* Escala del slider: raíz cuadrada → más precisión en los radios pequeños, que son los habituales. */
float RadiusToT(int r)
{
    const float lo = sqrtf((float)RADIUS_MIN), hi = sqrtf((float)RADIUS_MAX);
    return (sqrtf((float)r) - lo) / (hi - lo);
}

int TToRadius(float t)
{
    const float lo = sqrtf((float)RADIUS_MIN), hi = sqrtf((float)RADIUS_MAX);
    const float s = lo + (hi - lo) * t;
    return (int)(s * s + 0.5f);
}

/* ───────────────────────── Registro ───────────────────────── */
DWORD RegReadDword(LPCWSTR name, DWORD def)
{
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, name, RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS)
        return v;
    return def;
}

void RegWriteDword(LPCWSTR name, DWORD v)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
        RegCloseKey(k);
    }
}

void Cfg_Save(void)
{
    if (g_noSave) return;
    RegWriteDword(L"Radius", (DWORD)g_cfg.radius);
    RegWriteDword(L"Enabled", (DWORD)g_cfg.enabled);
    RegWriteDword(L"HideFromCapture", (DWORD)g_cfg.hideCapture);
    RegWriteDword(L"NotchAlerts", (DWORD)g_cfg.notch);
    RegWriteDword(L"CaptureAlerts", (DWORD)g_cfg.captures);
    RegWriteDword(L"MirrorWindows", (DWORD)g_cfg.mirror);
    RegWriteDword(L"HideBanners", (DWORD)g_cfg.hideBanners);
    RegWriteDword(L"EdgeHover", (DWORD)g_cfg.edgeHover);
    RegWriteDword(L"NotchMaterial", (DWORD)g_cfg.material);
    RegWriteDword(L"NotchSize", (DWORD)g_cfg.size);
    RegWriteDword(L"NotchAccent", (DWORD)g_cfg.accent);
    RegWriteDword(L"NotchBounce", (DWORD)g_cfg.bounce);
    RegWriteDword(L"NotchFloating", (DWORD)g_cfg.floating);
    RegWriteDword(L"NotchSoundV4", (DWORD)g_cfg.sound);
    RegWriteDword(L"MenuBar", (DWORD)g_cfg.menubar);
    RegWriteDword(L"HideWindowsClock", (DWORD)g_cfg.hideClock);
    RegWriteDword(L"Dock", (DWORD)g_cfg.dock);
    RegWriteDword(L"DockHideTaskbar", (DWORD)g_cfg.dockHideTaskbar);
    RegWriteDword(L"DockBlur", (DWORD)g_cfg.dockBlur);
    RegWriteDword(L"DockOpacity", (DWORD)g_cfg.dockOpacity);
    RegWriteDword(L"DockIconSize", (DWORD)g_cfg.dockIcon);
    RegWriteDword(L"BarBatteryPercent", (DWORD)g_cfg.battPct);
    RegWriteDword(L"DockAutoHide", (DWORD)g_cfg.dockAutoHide);
    RegWriteDword(L"DockWindowsFull", (DWORD)g_cfg.dockWinFull);
    RegWriteDword(L"NotchSiteIcons", (DWORD)g_cfg.siteIcons);
    RegWriteDword(L"NotchSoundVol", (DWORD)g_cfg.soundVol);
    RegWriteDword(L"Launcher", (DWORD)g_cfg.launcher);
    if (g_cfg.autoUpdate >= 0) RegWriteDword(L"AutoUpdate", (DWORD)g_cfg.autoUpdate);   /* sin elegir: no se escribe */
    RegWriteDword(L"NotchX", (DWORD)g_cfg.notchX);
    RegWriteDword(L"NotchY", (DWORD)g_cfg.notchY);
}

/* Todo lo leído se valida: el registro es entrada externa. */
static void LoadConfig(void)
{
    g_cfg.radius      = (int)RegReadDword(L"Radius", 12);
    g_cfg.enabled     = RegReadDword(L"Enabled", 1) != 0;
    g_cfg.hideCapture = RegReadDword(L"HideFromCapture", 1) != 0;
    g_cfg.notch       = RegReadDword(L"NotchAlerts", 1) != 0;
    g_cfg.captures    = RegReadDword(L"CaptureAlerts", 1) != 0;
    g_cfg.mirror      = RegReadDword(L"MirrorWindows", 1) != 0;
    g_cfg.hideBanners = RegReadDword(L"HideBanners", 0) != 0;
    g_cfg.edgeHover   = RegReadDword(L"EdgeHover", 1) != 0;
    g_cfg.material    = (int)min(RegReadDword(L"NotchMaterial", MAT_OLED), 2);
    g_cfg.size        = (int)min(RegReadDword(L"NotchSize", 1), 2);
    g_cfg.accent      = (int)min(RegReadDword(L"NotchAccent", 0), ACCENT_COUNT - 1);
    g_cfg.bounce      = (int)min(RegReadDword(L"NotchBounce", 1), 2);
    g_cfg.floating    = RegReadDword(L"NotchFloating", 0) != 0;
    {   /* catálogos anteriores: silencio y Windows se conservan; Campanita (ya no está) y los
         * sintetizados pasan a Eco suave, que es también el de una instalación nueva */
        enum { ECO_SUAVE = 3 };
        const DWORD v4 = RegReadDword(L"NotchSoundV4", 0xFFFF), v3 = RegReadDword(L"NotchSoundV3", 0xFFFF);
        const DWORD v2 = RegReadDword(L"NotchSoundV2", 0xFFFF), old = RegReadDword(L"NotchSound", 0xFFFF);
        g_cfg.sound = v4 != 0xFFFF ? (int)min(v4, SOUND_COUNT - 1)
                    : v3 != 0xFFFF ? (v3 <= 1 ? (int)v3 : v3 == 2 ? ECO_SUAVE : (int)min(v3 - 1, SOUND_COUNT - 1))
                    : v2 != 0xFFFF ? (v2 <= 1 ? (int)v2 : ECO_SUAVE)
                    : old != 0xFFFF && old <= 1 ? (int)old : ECO_SUAVE;
    }
    g_cfg.menubar     = RegReadDword(L"MenuBar", 0) != 0;
    g_cfg.hideClock   = RegReadDword(L"HideWindowsClock", 1) != 0;
    g_cfg.dock        = RegReadDword(L"Dock", 0) != 0;
    g_cfg.dockHideTaskbar = RegReadDword(L"DockHideTaskbar", 1) != 0;
    g_cfg.dockBlur    = (int)min(RegReadDword(L"DockBlur", 1), 2);
    g_cfg.dockOpacity = (int)min(RegReadDword(L"DockOpacity", 2), 3);
    g_cfg.dockIcon    = (int)min(RegReadDword(L"DockIconSize", 1), 3);
    g_cfg.battPct     = RegReadDword(L"BarBatteryPercent", 0) != 0;
    g_cfg.dockAutoHide = (int)min(RegReadDword(L"DockAutoHide", 0), 2);
    g_cfg.dockWinFull  = RegReadDword(L"DockWindowsFull", 1) != 0;
    g_cfg.siteIcons    = RegReadDword(L"NotchSiteIcons", 1) != 0;
    g_cfg.launcher     = RegReadDword(L"Launcher", 1) != 0;
    {   /* solo se activa con un gesto del usuario: sin valor, no se busca */
        const DWORD au = RegReadDword(L"AutoUpdate", 0xFFFFFFFF);
        g_cfg.autoUpdate = au == 0xFFFFFFFF ? -1 : au != 0;
    }
    {   /* antes eran cuatro niveles; ahora 0..100 */
        static const int kOld[4] = { 25, 55, 80, 100 };
        const DWORD v = RegReadDword(L"NotchSoundVol", 0xFFFF);
        g_cfg.soundVol = v != 0xFFFF ? (int)min(v, 100) : kOld[min(RegReadDword(L"NotchSoundVolume", 1), 3)];
    }
    g_cfg.notchX      = (int)RegReadDword(L"NotchX", 5000);
    g_cfg.notchY      = (int)RegReadDword(L"NotchY", 0);
    g_cfg.radius = max(RADIUS_MIN, min(RADIUS_MAX, g_cfg.radius));
    g_cfg.notchX = max(0, min(10000, g_cfg.notchX));
    g_cfg.notchY = max(0, min(4000, g_cfg.notchY));
}

/* ───────────────────────── Render de la máscara ─────────────────────────
 * which: bit0 = derecha, bit1 = abajo  →  0 TL, 1 TR, 2 BL, 3 BR
 * Alpha = cobertura analítica del exterior del círculo (≈1 px de antialias).
 * Formato: BGRA premultiplicado; negro puro → solo importa el alpha.
 * UpdateLayeredWindow también mueve/redimensiona la ventana.               */
static BOOL PaintCorner(HWND hwnd, int px, int py, int s, int which, const RECT *mon)
{
    Canvas c;
    if (!Canvas_Init(&c, s, s)) return FALSE;
    /* arriba, con la barra superior: la superficie de la barra en vez de negro */
    DWORD rgb = 0;
    BOOL glass = FALSE;
    const BOOL bar = !(which & 2) && Bar_Surface(mon, px, py, &rgb, &glass);

    const double cx = (which & 1) ? 0.0 : (double)s;
    const double cy = (which & 2) ? 0.0 : (double)s;
    const double r  = (double)s;
    for (int y = 0; y < s; ++y) {
        const double dy = (y + 0.5) - cy;
        for (int x = 0; x < s; ++x) {
            const double dx = (x + 0.5) - cx;
            double a = sqrt(dx * dx + dy * dy) - r + 0.5;
            if (a < 0.0) a = 0.0; else if (a > 1.0) a = 1.0;
            const DWORD A = (DWORD)(BYTE)(a * 255.0 + 0.5);
            if (!bar) { c.px[y * s + x] = A << 24; continue; }
            if (glass) Bar_Surface(mon, px + x, py + y, &rgb, &glass);
            c.px[y * s + x] = A << 24 | (((rgb >> 16) & 255) * A / 255) << 16 | (((rgb >> 8) & 255) * A / 255) << 8 | (rgb & 255) * A / 255;
        }
    }

    POINT dst = { px, py }, src = { 0, 0 };
    SIZE  sz  = { s, s };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC screen = GetDC(NULL);
    UpdateLayeredWindow(hwnd, screen, &dst, &sz, c.dc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
    Canvas_Free(&c);
    return glass;
}

/* Pinta la esquina i; el vidrio no debe verse a sí mismo al capturar lo de detrás, así que
 * esas esquinas quedan fuera de captura (solo se llama al sistema si cambia). */
static void PaintCornerAt(int i, HWND h)
{
    const BOOL glass = PaintCorner(h, g_cinfo[i].x, g_cinfo[i].y, g_cinfo[i].s, g_cinfo[i].which, &g_cinfo[i].mon);
    const int aff = App_HideFromCapture(glass) ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
    if (g_cinfo[i].affinity != aff) { SetWindowDisplayAffinity(h, (DWORD)aff); g_cinfo[i].affinity = aff; }
}

/* ───────────────────────── Gestión de esquinas ───────────────────────── */
static int g_used;

static void DestroyCornersFrom(int first)
{
    for (int i = first; i < g_count; ++i)
        if (g_corners[i]) DestroyWindow(g_corners[i]);
    g_count = first;
}

/* Reutiliza las ventanas existentes: cambiar el radio no parpadea. */
static BOOL CALLBACK MonitorProc(HMONITOR mon, HDC hdc, LPRECT lprc, LPARAM lp)
{
    (void)hdc; (void)lprc; (void)lp;
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;
    const RECT r = mi.rcMonitor;

    int s = MulDiv(g_cfg.radius, (int)MonitorDpi(mon), 96);
    const int maxS = min(r.right - r.left, r.bottom - r.top) / 2;
    if (s > maxS) s = maxS;
    if (s < 1) s = 1;

    /* con la barra superior visible, el redondeo de arriba empieza bajo ella */
    const int top = r.top + Bar_HeightOn(&r);
    for (int i = 0; i < 4 && g_used < MAX_CORNERS; ++i) {
        const int x = (i & 1) ? r.right - s : r.left;
        const int y = (i & 2) ? r.bottom - s : top;

        HWND h = g_used < g_count ? g_corners[g_used] : NULL;
        const BOOL fresh = !h;
        if (fresh) {
            h = CreateWindowExW(
                WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                CORNER_CLASS, L"", WS_POPUP, x, y, s, s, NULL, NULL, g_inst, NULL);
            if (!h) continue;
        }
        if (fresh) g_cinfo[g_used].affinity = -1;
        g_cinfo[g_used].x = x; g_cinfo[g_used].y = y; g_cinfo[g_used].s = s; g_cinfo[g_used].which = i; g_cinfo[g_used].mon = r;
        PaintCornerAt(g_used, h);
        if (fresh) ShowWindow(h, SW_SHOWNOACTIVATE);
        g_corners[g_used++] = h;
    }
    return TRUE;
}

static void RebuildCorners(void);
/* El fondo detrás de la barra cambió: solo cambian las esquinas de arriba de su monitor. */
void App_BarSurfaceChanged(void)
{
    if (!g_cfg.enabled) return;
    for (int i = 0; i < g_count; ++i)
        if (g_corners[i] && !(g_cinfo[i].which & 2) && Bar_HeightOn(&g_cinfo[i].mon)) PaintCornerAt(i, g_corners[i]);
}

static void RebuildCorners(void)
{
    if (!g_cfg.enabled) { DestroyCornersFrom(0); return; }
    g_used = 0;
    EnumDisplayMonitors(NULL, NULL, MonitorProc, 0);
    const int used = g_used;
    if (used > g_count) g_count = used;
    DestroyCornersFrom(used);
}

/* ¿Hay una ventana a pantalla completa (Escritorio remoto maximizado, vídeo, juego) arriba
 * del todo en este monitor? Entonces su barra y su dock se apartan en lugar de subirse
 * encima. Se mira cada monitor por su orden Z, no solo la ventana con el foco: con dos
 * monitores, un vídeo a pantalla completa en uno sigue tapándolo aunque se use el otro. */
static BOOL Skippable(HWND w)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    BOOL cloaked = FALSE;     /* otro escritorio virtual, apps de la Store suspendidas */
    if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
    wchar_t cls[64];
    return GetClassNameW(w, cls, 64) && (!lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"WorkerW") ||
           !lstrcmpW(cls, L"Shell_TrayWnd") || !lstrcmpW(cls, L"Shell_SecondaryTrayWnd"));
}

BOOL App_FullscreenOn(const RECT *mon)
{
    for (HWND w = GetTopWindow(NULL); w; w = GetWindow(w, GW_HWNDNEXT)) {
        RECT r, x;      /* primero lo barato; lo que pregunta a DWM, solo si está en este monitor */
        if (!IsWindowVisible(w) || IsIconic(w) || !GetWindowRect(w, &r) || !IntersectRect(&x, &r, mon) || Skippable(w))
            continue;
        const BOOL covers = r.left <= mon->left && r.top <= mon->top && r.right >= mon->right && r.bottom >= mon->bottom;
        if (covers && (GetWindowLongW(w, GWL_STYLE) & WS_CAPTION) != WS_CAPTION) return TRUE;
        /* avisos, menús y paneles flotantes por encima no cuentan; una ventana normal sí */
        const LONG ex = GetWindowLongW(w, GWL_EXSTYLE);
        if (!(ex & (WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE))) return FALSE;
    }
    return FALSE;
}

static void CheckFullscreen(void)
{
    Bar_FullscreenFg();
    Dock_FullscreenFg();
}

/* ───────────────────────── Escritorio remoto ─────────────────────────
 * Chrome Remote Desktop transmite la pantalla capturándola: lo que está fuera de captura
 * (WDA_EXCLUDEFROMCAPTURE: "Ocultar en capturas" y el vidrio) no le llega. Mientras hay una
 * sesión remota, OpenDock se deja capturar y el vidrio pasa a color sólido (si no, al
 * capturar lo de detrás se vería a sí mismo); al acabar vuelve todo como estaba. */
static BOOL s_remote;

static BOOL DetectRemote(void)
{
    if (GetSystemMetrics(SM_REMOTESESSION)) return TRUE;          /* Escritorio remoto de Windows */
    /* Chrome Remote Desktop transmite la sesión de consola: si no es la nuestra, no nos ve.
     * (Su proceso corre como SYSTEM: no se puede preguntar su sesión sin permisos.) */
    DWORD me = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &me) || me != WTSGetActiveConsoleSessionId()) return FALSE;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return FALSE;
    PROCESSENTRY32W pe = { sizeof(pe) };
    BOOL found = FALSE;
    /* el proceso de escritorio de Chrome Remote Desktop solo existe con alguien conectado
     * (remoting_host.exe, el servicio, corre siempre y no cuenta) */
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        if (!lstrcmpiW(pe.szExeFile, L"remoting_desktop.exe") || !lstrcmpiW(pe.szExeFile, L"remote_assistance_host.exe"))
            found = TRUE;
    CloseHandle(snap);
    return found;
}

BOOL App_RemoteView(void) { return s_remote; }

BOOL App_HideFromCapture(BOOL glass) { return !s_remote && (g_cfg.hideCapture || glass); }

static void CheckRemote(void)
{
    const BOOL now = DetectRemote();
    if (now == s_remote) return;
    s_remote = now;
    RebuildCorners();
    Notch_StyleChanged();
    Notch_ApplyCapture();
    Bar_StyleChanged();
    Dock_ConfigChanged();
}

/* La ventana de delante se movió o cambió de tamaño (maximizar el Escritorio remoto lo
 * pasa a pantalla completa sin cambiar de ventana): se comprueba un instante después. */
static void CALLBACK LocationHook(HWINEVENTHOOK h, DWORD ev, HWND w, LONG o, LONG c, DWORD t, DWORD ms)
{
    (void)h; (void)ev; (void)t; (void)ms;
    if (o == OBJID_WINDOW && c == CHILDID_SELF && w && w == GetForegroundWindow())
        SetTimer(g_ctrl, TIMER_FSCHECK, 120, NULL);
}

/* Recorre el orden Z hacia arriba desde h: si hay una ventana visible de otro proceso por
 * encima, hay que volver a subirla. Casi siempre no la hay y se ahorra el SetWindowPos
 * (que hace trabajar a DWM aunque la ventana ya esté arriba). */
BOOL App_Covered(HWND h)
{
    RECT mine;
    if (!h || !GetWindowRect(h, &mine)) return FALSE;
    const DWORD me = GetCurrentProcessId();
    for (HWND p = GetWindow(h, GW_HWNDPREV); p; p = GetWindow(p, GW_HWNDPREV)) {
        if (!IsWindowVisible(p)) continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(p, &pid);
        if (pid == me) continue;
        RECT r, x;
        if (!GetWindowRect(p, &r) || !IntersectRect(&x, &r, &mine)) continue;     /* no la pisa */
        BOOL cloaked = FALSE;     /* apps de la Store suspendidas, otros escritorios: "visibles" pero ocultas */
        if (SUCCEEDED(DwmGetWindowAttribute(p, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) continue;
        return TRUE;
    }
    return FALSE;
}

/* Huella de los monitores (posición, tamaño y cuál es el principal). */
static BOOL CALLBACK MonSigProc(HMONITOR mon, HDC hdc, LPRECT lprc, LPARAM lp)
{
    (void)hdc; (void)lprc;
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(mon, &mi)) {
        DWORD *h = (DWORD *)lp;
        const LONG v[5] = { mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom,
                            (LONG)(mi.dwFlags & MONITORINFOF_PRIMARY) };
        for (int i = 0; i < 5; ++i) *h = (*h ^ (DWORD)v[i]) * 16777619u;
    }
    return TRUE;
}

/* Al desconectar un monitor, Windows no siempre avisa cuando ya terminó de recolocarlo
 * todo: si la barra y el dock se rehicieron a medias, quedaban dos en el mismo monitor.
 * Si la huella cambió desde la última vez, se rehace todo. */
static void CheckMonitors(void)
{
    static DWORD last;
    DWORD sig = 2166136261u;
    EnumDisplayMonitors(NULL, NULL, MonSigProc, (LPARAM)&sig);
    if (last && sig != last && !g_noSave) SetTimer(g_ctrl, TIMER_REBUILD, 400, NULL);
    last = sig;
}

static void RaiseCorners(void)
{
    static int tick;
    if (!(++tick & 1)) CheckRemote();
    CheckMonitors();
    CheckFullscreen();
    Bar_Raise();
    Dock_Raise();
    for (int i = 0; i < g_count; ++i)
        if (App_Covered(g_corners[i]))
            SetWindowPos(g_corners[i], HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
    Notch_Raise();
}

/* Cuando cambia la ventana en primer plano (p.ej. clic en la barra de tareas,
 * que también es topmost) volvemos a subir las esquinas al instante. */
static void CALLBACK ForegroundHook(HWINEVENTHOOK h, DWORD ev, HWND w, LONG o, LONG c, DWORD t, DWORD ms)
{
    (void)h; (void)ev; (void)w; (void)o; (void)c; (void)t; (void)ms;
    RaiseCorners();
    Bar_ForegroundChanged();
}

/* ───────────────────────── Bandeja ───────────────────────── */
static void UpdateTrayTip(void)
{
    if (!g_nid.hWnd) return;
    wsprintfW(g_nid.szTip, L"OpenDock \x2014 %d px%s", g_cfg.radius, g_cfg.enabled ? L"" : L" (pausado)");
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void AddTrayIcon(void)
{
    if (g_nid.hIcon) DestroyIcon(g_nid.hIcon);
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_ctrl;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wsprintfW(g_nid.szTip, L"OpenDock \x2014 %d px", g_cfg.radius);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    UpdateTrayTip();
}

static void RemoveTrayIcon(void)
{
    if (!g_nid.hWnd) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_nid.hIcon) DestroyIcon(g_nid.hIcon);
    ZeroMemory(&g_nid, sizeof(g_nid));
}

static void ShowTrayMenu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    HMENU sub  = CreatePopupMenu();
    wchar_t buf[96];

    for (int i = 0; i < (int)(sizeof(kPresets) / sizeof(kPresets[0])); ++i) {
        wsprintfW(buf, L"%d px", kPresets[i]);
        AppendMenuW(sub, MF_STRING | (kPresets[i] == g_cfg.radius ? MF_CHECKED : 0), IDM_RADIUS_BASE + i, buf);
    }

    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Configuración\x2026");
    SetMenuDefaultItem(menu, IDM_SETTINGS, FALSE);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (g_cfg.enabled ? MF_CHECKED : 0), IDM_TOGGLE, L"Esquinas redondeadas\tCtrl+Alt+R");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, L"Radio\tCtrl+Alt+RePág/AvPág");
    AppendMenuW(menu, MF_STRING | (g_cfg.menubar ? MF_CHECKED : 0), IDM_MENUBAR, L"Barra superior\tCtrl+Alt+B");
    AppendMenuW(menu, MF_STRING | (g_cfg.dock ? MF_CHECKED : 0), IDM_DOCK, L"Dock inferior\tCtrl+Alt+D");
    AppendMenuW(menu, MF_STRING | (g_cfg.hideCapture ? MF_CHECKED : 0), IDM_CAPTURE, L"Ocultar en capturas de pantalla");
    AppendMenuW(menu, MF_STRING | (Inst_IsStartup() ? MF_CHECKED : 0), IDM_STARTUP, L"Iniciar con Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    if (Upd_Available()) {
        wsprintfW(buf, L"Actualizar a %s", Upd_Available());
        AppendMenuW(menu, MF_STRING, IDM_UPDATE, buf);
    } else AppendMenuW(menu, MF_STRING, IDM_CHECKUPD, L"Buscar actualizaciones");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Salir");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu); /* destruye también el submenú */
}

/* ───────────────────────── Acciones ───────────────────────── */
void App_SetRadius(int r, UINT flags)
{
    r = max(RADIUS_MIN, min(RADIUS_MAX, r));
    if (r != g_cfg.radius) {
        g_cfg.radius = r;
        if (!(flags & APPF_NOSAVE)) Cfg_Save();
        RebuildCorners();
        UpdateTrayTip();
        Panel_Refresh();
    }
    if (flags & APPF_NOTIFY) {
        wchar_t d[16];
        wsprintfW(d, L"%d px", r);
        Notch_Show(NI_RADIUS, L"Radio", d, (int)(RadiusToT(r) * 1000.0f), FALSE);
    }
}

void App_SetEnabled(BOOL on, UINT flags)
{
    g_cfg.enabled = on;
    Cfg_Save();
    RebuildCorners();
    UpdateTrayTip();
    Panel_Refresh();
    if (flags & APPF_NOTIFY)
        Notch_Show(on ? NI_ON : NI_OFF, L"Esquinas redondeadas", on ? L"Activadas" : L"En pausa", -1, FALSE);
}

void App_SetHideCapture(BOOL on)
{
    g_cfg.hideCapture = on;
    Cfg_Save();
    RebuildCorners();
    Notch_ApplyCapture();
    Bar_StyleChanged();
    Dock_ConfigChanged();
    Panel_Refresh();
}

void App_SetNotch(BOOL on)
{
    g_cfg.notch = on;
    Cfg_Save();
    Panel_Refresh();
    if (on) Notch_Show(NI_INFO, L"Avisos tipo notch", L"Arrástrame donde quieras", -1, FALSE);
}

/* Aplica las opciones del notch: lector de Windows, banners, borde superior y diseño. */
void App_NotchConfigChanged(void)
{
    Cfg_Save();
    if (g_cfg.mirror && !g_wnStarted) { Wn_Start(); g_wnStarted = TRUE; }
    if (g_ctrl) {
        if (g_cfg.notch && g_cfg.edgeHover) SetTimer(g_ctrl, TIMER_EDGE, 50, NULL);
        else KillTimer(g_ctrl, TIMER_EDGE);
        /* la vigilancia de la carpeta llega tarde con algunos navegadores: sondeo ligero */
        if (g_cfg.mirror) SetTimer(g_ctrl, TIMER_WNPOLL, 600, NULL);
        else KillTimer(g_ctrl, TIMER_WNPOLL);
    }
    Notch_StyleChanged();
    Bar_StyleChanged();     /* material y acento son comunes a notch, barra y dock */
    Dock_ConfigChanged();
    Panel_Refresh();
}

/* Sondeo ligero del cursor: si se queda en la fila superior del monitor, nace el mini notch. */
static void CheckEdge(void)
{
    POINT pt;
    if (!GetCursorPos(&pt)) return;
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi)) return;
    {   /* lejos del borde de arriba no puede llegar en 150 ms: cada 150 ms basta */
        static UINT every = 50;
        const UINT want = pt.y - mi.rcMonitor.top > 260 ? 150 : 50;
        if (want != every) { every = want; SetTimer(g_ctrl, TIMER_EDGE, every, NULL); }
    }
    const BOOL buttons = (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000;
    /* con la barra superior, su franja hace de borde: no hace falta subir hasta arriba */
    const int edge = mi.rcMonitor.top + 1 + Bar_HeightOn(&mi.rcMonitor);
    if (pt.y > edge || buttons ||
        !Notch_InEdgeZone(pt, &mi.rcMonitor, MonitorDpi(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST)))) {
        g_edgeTicks = 0;        /* en las esquinas no: ahí se cierran pestañas y ventanas */
        return;
    }
    if (++g_edgeTicks == 3) {       /* ~150 ms: evita dispararlo al pasar de largo */
        QUERY_USER_NOTIFICATION_STATE qs;
        if (SUCCEEDED(SHQueryUserNotificationState(&qs)) &&
            (qs == QUNS_BUSY || qs == QUNS_RUNNING_D3D_FULL_SCREEN || qs == QUNS_PRESENTATION_MODE))
            return;
        Notch_EdgeHover(pt);
    }
}

/* La barra y el dock van juntos: sin barra superior se quita también el dock y vuelve la
 * barra de tareas de Windows; al volver a activar la barra, el dock vuelve si estaba. */
static BOOL s_dockWithBar;

void App_SetMenuBar(BOOL on, BOOL notify)
{
    g_cfg.menubar = on;
    if (!on) {
        s_dockWithBar = g_cfg.dock;
        g_cfg.dock = FALSE;
    } else if (s_dockWithBar) {
        g_cfg.dock = TRUE;
        s_dockWithBar = FALSE;
    }
    Cfg_Save();
    Bar_Apply();
    Dock_Apply();
    if (!on) Dock_RestoreTaskbar();     /* la barra de tareas de Windows, de vuelta */
    RebuildCorners();
    Notch_StyleChanged();       /* el notch flotante se recoloca bajo la barra */
    Panel_Refresh();
    if (notify)
        Notch_Show(NI_INFO, L"Barra superior", on ? L"Activada \x00B7 Ctrl+Alt+B" : L"Oculta \x00B7 Ctrl+Alt+B para volver", -1, TRUE);
}

void App_SetHideClock(BOOL on)
{
    g_cfg.hideClock = on;
    Cfg_Save();
    Bar_ApplyClock(g_cfg.menubar && on);
    Panel_Refresh();
}

void App_SetDock(BOOL on, BOOL notify)
{
    g_cfg.dock = on;
    Cfg_Save();
    Dock_Apply();
    Panel_Refresh();
    if (notify)
        Notch_Show(NI_INFO, L"Dock", on ? L"Activado \x00B7 Ctrl+Alt+D" : L"Oculto \x00B7 Ctrl+Alt+D para volver", -1, TRUE);
}

void App_SetCaptures(BOOL on)
{
    g_cfg.captures = on;
    Cfg_Save();
    Panel_Refresh();
}

/* ───────────────────────── Capturas de pantalla ─────────────────────────
 * Recortes (Win+Shift+S / ImpPant) deja la imagen en el portapapeles sin ventana
 * dueña; navegadores y editores sí tienen dueño. Así distinguimos una captura de
 * "copiar imagen" sin leer el contenido de nada más.                          */
static const wchar_t *BaseName(const wchar_t *path)
{
    const wchar_t *name = path;
    for (const wchar_t *q = path; *q; ++q) if (*q == L'\\') name = q + 1;
    return name;
}

static BOOL FromScreenshotTool(void)
{
    static const wchar_t *kTools[] = {
        L"SnippingTool.exe", L"ScreenClippingHost.exe", L"ScreenSketch.exe",
        L"ShareX.exe", L"Greenshot.exe", L"Lightshot.exe", L"flameshot.exe",
    };
    HWND owner = GetClipboardOwner();
    if (!owner) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(owner, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return FALSE;
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(p, 0, path, &n);
    CloseHandle(p);
    if (!ok) return FALSE;

    const wchar_t *name = BaseName(path);
    for (int i = 0; i < (int)(sizeof(kTools) / sizeof(kTools[0])); ++i)
        if (!lstrcmpiW(name, kTools[i])) return TRUE;
    return FALSE;
}

/* El portapapeles es entrada de otros procesos: se valida el DIB antes de tocarlo. */
static SIZE_T DibBitsOffset(const BITMAPINFOHEADER *h, SIZE_T size)
{
    if (size < sizeof(BITMAPINFOHEADER) || h->biSize < sizeof(BITMAPINFOHEADER) || h->biSize > size) return 0;
    if (h->biWidth <= 0 || h->biWidth > 32768 || h->biHeight == 0 || h->biHeight > 32768 || h->biHeight < -32768) return 0;
    if (h->biPlanes != 1 || (h->biBitCount != 24 && h->biBitCount != 32)) return 0;
    if (h->biCompression != BI_RGB && h->biCompression != BI_BITFIELDS) return 0;
    if (h->biClrUsed > 256) return 0;

    SIZE_T off = h->biSize + (SIZE_T)h->biClrUsed * sizeof(RGBQUAD);
    if (h->biCompression == BI_BITFIELDS && h->biSize == sizeof(BITMAPINFOHEADER)) off += 3 * sizeof(DWORD);
    const SIZE_T stride = (((SIZE_T)h->biWidth * h->biBitCount + 31) / 32) * 4;
    const SIZE_T rows = (SIZE_T)(h->biHeight < 0 ? -h->biHeight : h->biHeight);
    return off + stride * rows <= size ? off : 0;
}

static void AttachPendingShot(void)
{
    if (g_shotPath[0] && GetTickCount() - g_shotTick < 4000 && Notch_AttachFile(g_shotPath))
        g_shotPath[0] = 0;
}

static void CheckClipboardCapture(void)
{
    const DWORD seq = GetClipboardSequenceNumber();
    if (seq == g_clipSeq) return;
    g_clipSeq = seq;
    if (!g_cfg.captures || !g_cfg.notch) return;
    if (!IsClipboardFormatAvailable(CF_DIB) || !FromScreenshotTool()) return;
    if (!OpenClipboard(g_ctrl)) return;

    HANDLE hd = GetClipboardData(CF_DIB);
    const BITMAPINFO *bi = hd ? (const BITMAPINFO *)GlobalLock(hd) : NULL;
    if (bi) {
        const SIZE_T off = DibBitsOffset(&bi->bmiHeader, GlobalSize(hd));
        if (off) {
            wchar_t d[48];
            wsprintfW(d, L"%d \x00D7 %d", bi->bmiHeader.biWidth, abs(bi->bmiHeader.biHeight));
            if (Notch_ShowCapture(bi, (const BYTE *)bi + off, L"Captura copiada", d)) AttachPendingShot();
        }
        GlobalUnlock(hd);
    }
    CloseClipboard();
}

static BOOL IsImageName(const wchar_t *name, int len)
{
    static const wchar_t *kExt[] = { L".png", L".jpg", L".jpeg" };
    for (int i = 0; i < 3; ++i) {
        const int n = lstrlenW(kExt[i]);
        if (len > n && CompareStringOrdinal(name + len - n, n, kExt[i], n, TRUE) == CSTR_EQUAL) return TRUE;
    }
    return FALSE;
}

/* Hilo que vigila la carpeta "Capturas de pantalla" (Win+ImpPant y el autoguardado
 * de Recortes) para que el clic en el aviso abra el archivo. */
static DWORD WINAPI ShotWatcher(LPVOID unused)
{
    (void)unused;
    PWSTR dir = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_Screenshots, 0, NULL, &dir))) return 0;
    HANDLE h = CreateFileW(dir, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) { CoTaskMemFree(dir); return 0; }

    static DWORD buf[4096];
    DWORD got = 0;
    while (ReadDirectoryChangesW(h, buf, sizeof(buf), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME, &got, NULL, NULL)) {
        const BYTE *p = (const BYTE *)buf;
        while (got) {
            const FILE_NOTIFY_INFORMATION *fi = (const FILE_NOTIFY_INFORMATION *)p;
            const int len = (int)(fi->FileNameLength / sizeof(wchar_t));
            const BOOL added = fi->Action == FILE_ACTION_ADDED || fi->Action == FILE_ACTION_RENAMED_NEW_NAME;
            if (added && IsImageName(fi->FileName, len) && lstrlenW(dir) + len + 2 < MAX_PATH) {
                EnterCriticalSection(&g_shotLock);
                const int n = wsprintfW(g_shotPending, L"%s\\", dir);   /* wsprintf no admite %.*s */
                lstrcpynW(g_shotPending + n, fi->FileName, len + 1);
                LeaveCriticalSection(&g_shotLock);
                PostMessageW(g_ctrl, WM_SHOTFILE, 0, 0);
            }
            if (!fi->NextEntryOffset) break;
            p += fi->NextEntryOffset;
        }
    }
    CloseHandle(h);
    CoTaskMemFree(dir);
    return 0;
}

void App_SetAutoUpdate(BOOL on)
{
    g_cfg.autoUpdate = on ? 1 : 0;
    Cfg_Save();
    Upd_Schedule();
    Panel_Refresh();
}

BOOL App_AutoUpdateShown(void)
{
    return g_cfg.autoUpdate == 1 || (g_cfg.autoUpdate < 0 && !Inst_IsRunningInstalled());
}

/* Sale limpio (barra de tareas, reloj, esquinas…) y, al final de wWinMain, arranca exe. */
void App_Relaunch(LPCWSTR exe, LPCWSTR args)
{
    lstrcpynW(g_relaunch, exe, MAX_PATH);
    lstrcpynW(g_relaunchArgs, args, 48);
    App_Quit();
}

void App_Install(void)
{
    wchar_t exe[MAX_PATH];
    /* el panel enseña "Buscar actualizaciones" encendido junto a "Instalar": instalar con él
     * así es elegirlo (apagarlo antes de instalar lo deja apagado) */
    if (g_cfg.autoUpdate < 0) { g_cfg.autoUpdate = 1; Cfg_Save(); Upd_Schedule(); }
    if (!Inst_Install(exe)) {
        Notch_Show(NI_WARN, L"No se pudo instalar", L"Revisa permisos de la carpeta", -1, TRUE);
        return;
    }
    if (Inst_IsRunningInstalled()) {
        Panel_Refresh();
        Notch_Show(NI_CHECK, L"Instalada", L"Se inicia con Windows", -1, TRUE);
        return;
    }
    /* Pasamos el relevo a la copia instalada (ver el final de wWinMain). */
    lstrcpynW(g_relaunch, exe, MAX_PATH);
    Panel_Close();
    DestroyWindow(g_ctrl);
}

void App_Uninstall(void)
{
    Inst_Unregister();
    Inst_RemoveFiles();
    g_noSave = TRUE;
    Panel_Close();
    DestroyCornersFrom(0);
    RemoveTrayIcon();
    if (Notch_Show(NI_CHECK, L"OpenDock desinstalado", L"Hasta pronto", -1, TRUE)) Notch_QuitOnHide();
    else DestroyWindow(g_ctrl);
}

void App_Quit(void)
{
    Panel_Close();
    DestroyWindow(g_ctrl);
}

/* ───────────────────────── Ventanas ───────────────────────── */
static LRESULT CALLBACK CornerProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_NCHITTEST:     return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_DPICHANGED:    return 0;   /* el tamaño lo controlamos nosotros */
    }
    return DefWindowProcW(h, m, w, l);
}

static LRESULT CALLBACK CtrlProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == g_wmTaskbarCreated && m != 0) {  /* explorer.exe se reinició */
        if (!g_noSave) AddTrayIcon();
        return 0;
    }

    switch (m) {
    case WM_TRAY:
        switch (LOWORD(l)) {
        case WM_LBUTTONUP:    Panel_Toggle(); break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:  ShowTrayMenu(h); break;
        }
        return 0;

    case WM_COMMAND: {
        const int id = LOWORD(w);
        if (id >= IDM_RADIUS_BASE && id < IDM_RADIUS_BASE + (int)(sizeof(kPresets) / sizeof(kPresets[0]))) {
            App_SetRadius(kPresets[id - IDM_RADIUS_BASE], 0);
        } else switch (id) {
        case IDM_SETTINGS: Panel_Show(); break;
        case IDM_MENUBAR:  App_SetMenuBar(!g_cfg.menubar, TRUE); break;
        case IDM_DOCK:     App_SetDock(!g_cfg.dock, TRUE); break;
        case IDM_TOGGLE:   App_SetEnabled(!g_cfg.enabled, 0); break;
        case IDM_CAPTURE:  App_SetHideCapture(!g_cfg.hideCapture); break;
        case IDM_STARTUP:  Inst_SetStartup(!Inst_IsStartup(), NULL); Panel_Refresh(); break;
        case IDM_EXIT:     App_Quit(); break;
        case IDM_UPDATE:   Upd_Apply(); break;
        case IDM_CHECKUPD: Upd_Check(TRUE); break;     /* pedido por el usuario: avisa también si está al día */
        }
        return 0;
    }

    case WM_UPDATE:
        Upd_OnMessage(w, l);
        return 0;

    case WM_HOTKEY:
        switch (w) {
        case HK_TOGGLE: App_SetEnabled(!g_cfg.enabled, APPF_NOTIFY); break;
        case HK_UP:     App_SetRadius(g_cfg.radius + 2, APPF_NOTIFY); break;
        case HK_DOWN:   App_SetRadius(g_cfg.radius - 2, APPF_NOTIFY); break;
        case HK_BAR:    App_SetMenuBar(!g_cfg.menubar, TRUE); break;
        case HK_DOCK:   App_SetDock(!g_cfg.dock, TRUE); break;
        }
        return 0;

    case WM_SETRADIUS:
        App_SetRadius((int)w, APPF_NOTIFY);
        return 0;

    case WM_SHOWPANEL:
        Panel_Show();
        return 0;

    case WM_BARCHANGED:
        RebuildCorners();
        return 0;

    case WM_LAUNCHER:
        if (w == 1) Notch_ToggleCenter();       /* Win+N, como en Windows 11 */
        else Launcher_Toggle();
        return 0;

    case WM_WNCHANGED:         /* la base se escribe en ráfagas: agrupar */
        if (w == 1) {          /* una respuesta rápida que la app no aceptó */
            Notch_Show(NI_WARN, L"No se pudo enviar", L"Esta app solo acepta respuestas desde Windows", -1, TRUE);
            return 0;
        }
        SetTimer(h, TIMER_WN, 300, NULL);
        return 0;

    case WM_CLIPBOARDUPDATE:   /* Recortes escribe varios formatos seguidos: esperamos a que termine */
        SetTimer(h, TIMER_CLIP, 150, NULL);
        return 0;

    case WM_SHOTFILE: {     /* la ruta la deja el hilo vigilante; el mensaje no trae punteros */
        EnterCriticalSection(&g_shotLock);
        lstrcpynW(g_shotPath, g_shotPending, MAX_PATH);
        g_shotPending[0] = 0;
        LeaveCriticalSection(&g_shotLock);
        if (!g_shotPath[0]) return 0;
        g_shotTick = GetTickCount();
        AttachPendingShot();
        if (g_shotPath[0]) SetTimer(h, TIMER_SHOTFILE, 800, NULL);  /* quizá el portapapeles llega después */
        return 0;
    }

    /* Cambios de resolución, monitores, escala o barra de tareas:
     * llegan en ráfaga, así que reconstruimos con debounce. */
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
    case WM_SETTINGCHANGE:
        if (!g_noSave) SetTimer(h, TIMER_REBUILD, 400, NULL);
        return 0;

    case WM_TIMER:
        if (Upd_Timer(w)) return 0;
        if (w == TIMER_REBUILD) { KillTimer(h, TIMER_REBUILD); RebuildCorners(); Bar_Reposition(); Dock_Reposition(); }
        else if (w == TIMER_TOPMOST) RaiseCorners();
        else if (w == TIMER_FSCHECK) { KillTimer(h, TIMER_FSCHECK); CheckFullscreen(); }
        else if (w == TIMER_DNDHINT) {
            /* las notificaciones llegan al notch, pero Windows también saca su banner: se
             * sugiere "No molestar" (los avisos siguen llegando al centro y al notch). Una vez al día. */
            KillTimer(h, TIMER_DNDHINT);
            SYSTEMTIME st;
            GetLocalTime(&st);
            const DWORD today = (DWORD)st.wYear * 10000 + st.wMonth * 100 + st.wDay;
            if (g_cfg.notch && g_cfg.mirror && Wn_QuietHours() == 0 && RegReadDword(L"DndHintDay", 0) != today) {
                RegWriteDword(L"DndHintDay", today);
                Notch_Show(NI_INFO, L"Activa \x201CNo molestar\x201D", L"Así los avisos solo salen en el notch", -1, FALSE);
            }
        }
        else if (w == TIMER_CLIP) { KillTimer(h, TIMER_CLIP); CheckClipboardCapture(); }
        else if (w == TIMER_EDGE) CheckEdge();
        else if (w == TIMER_WN) { KillTimer(h, TIMER_WN); if (g_cfg.mirror) Wn_Refresh(TRUE); }
        else if (w == TIMER_WNPOLL) { if (g_cfg.mirror && Wn_Changed()) Wn_Refresh(TRUE); }
        else if (w == TIMER_SHOTFILE) {
            KillTimer(h, TIMER_SHOTFILE);
            /* captura solo a archivo (sin portapapeles): aviso sin miniatura */
            if (g_shotPath[0] && g_cfg.captures &&
                Notch_ShowCapture(NULL, NULL, L"Captura guardada", BaseName(g_shotPath)))
                Notch_AttachFile(g_shotPath);
            g_shotPath[0] = 0;
        }
        return 0;

    case WM_DESTROY:
        if (g_fgHook) UnhookWinEvent(g_fgHook);
        if (g_locHook) UnhookWinEvent(g_locHook);
        RemoveClipboardFormatListener(h);
        Bar_Destroy();
        Bar_ApplyClock(FALSE);      /* sin OpenDock, el reloj de Windows vuelve */
        Dock_Destroy();
        Dock_RestoreTaskbar();
        Launcher_Destroy();
        Wn_Stop();
        UnregisterHotKey(h, HK_TOGGLE);
        UnregisterHotKey(h, HK_UP);
        UnregisterHotKey(h, HK_DOWN);
        UnregisterHotKey(h, HK_BAR);
        UnregisterHotKey(h, HK_DOCK);
        RemoveTrayIcon();
        Panel_Close();
        Notch_Destroy();
        DestroyCornersFrom(0);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void RegisterClasses(HINSTANCE hInst)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = CornerProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = CORNER_CLASS;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassExW(&wc);

    wc.lpfnWndProc   = CtrlProc;
    wc.lpszClassName = CTRL_CLASS;
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    RegisterClassExW(&wc);

    Panel_Register();
    Notch_Register();
    Bar_Register();
    Dock_Register();
    Launcher_Register();
    Menu_Register();
}

static void MessageLoop(void)
{
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

/* Abre una ruta, un AUMID (shell:AppsFolder\…) o un URI con explorer.exe: así ninguna
 * extensión de shell se carga en nuestro proceso. El destino va entre comillas como un solo
 * argumento: se rechaza todo lo que podría romper esas comillas o pasar como modificador
 * (comillas, caracteres de control, "/" o "-" al principio). */
BOOL App_ShellOpen(LPCWSTR target)
{
    if (!target || !target[0] || target[0] == L'/' || target[0] == L'-') return FALSE;
    const int n = lstrlenW(target);
    if (n > 2048) return FALSE;
    for (int i = 0; i < n; ++i) if (target[i] == L'"' || target[i] < 0x20 || target[i] == 0x7F) return FALSE;
    wchar_t exe[MAX_PATH];
    const UINT wl = GetSystemWindowsDirectoryW(exe, MAX_PATH);
    if (!wl || wl > MAX_PATH - 16) return FALSE;
    lstrcatW(exe, L"\\explorer.exe");
    const SIZE_T cap = (SIZE_T)(lstrlenW(exe) + n + 8);
    wchar_t *cmd = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, cap * sizeof(wchar_t));
    if (!cmd) return FALSE;
    wsprintfW(cmd, L"\"%s\" \"", exe);
    lstrcatW(cmd, target);
    lstrcatW(cmd, L"\"");
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    const BOOL ok = CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    HeapFree(GetProcessHeap(), 0, cmd);
    return ok;
}

/* Abre una ventana del Explorador con el archivo seleccionado (explorer /select,"ruta").
 * Mismas validaciones que App_ShellOpen: sólo rutas absolutas sin comillas ni controles. */
BOOL App_ShellSelect(LPCWSTR path)
{
    if (!path || lstrlenW(path) < 3 || path[1] != L':' || path[2] != L'\\') return FALSE;
    const int n = lstrlenW(path);
    if (n > 2048) return FALSE;
    for (int i = 0; i < n; ++i) if (path[i] == L'"' || path[i] < 0x20 || path[i] == 0x7F) return FALSE;
    wchar_t exe[MAX_PATH];
    const UINT wl = GetSystemWindowsDirectoryW(exe, MAX_PATH);
    if (!wl || wl > MAX_PATH - 16) return FALSE;
    lstrcatW(exe, L"\\explorer.exe");
    const SIZE_T cap = (SIZE_T)(lstrlenW(exe) + n + 20);
    wchar_t *cmd = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, cap * sizeof(wchar_t));
    if (!cmd) return FALSE;
    wsprintfW(cmd, L"\"%s\" /select,\"", exe);
    lstrcatW(cmd, path);
    lstrcatW(cmd, L"\"");
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    const BOOL ok = CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    HeapFree(GetProcessHeap(), 0, cmd);
    return ok;
}

/* Cierra la instancia en ejecución y espera a que termine. */
static void CloseRunningInstance(void)
{
    HWND other = FindWindowW(CTRL_CLASS, NULL);
    if (!other) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessageW(other, WM_CLOSE, 0, 0);
    if (p) {
        WaitForSingleObject(p, 5000);
        CloseHandle(p);
    }
}

/* ───────────────────────── Entrada ───────────────────────── */
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmdLine, int nShow)
{
    (void)hPrev; (void)cmdLine; (void)nShow;
    Harden();
    g_inst = hInst;

    BOOL wantExit = FALSE, wantSettings = FALSE, wantUninstall = FALSE, afterInstall = FALSE;
    int  cliRadius = 0;
    DWORD cleanupPid = 0, afterUpdate = 0;
    wchar_t updTest[MAX_PATH] = L"";
    int  argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        if      (!lstrcmpiW(argv[i], L"--exit"))      wantExit = TRUE;
        else if (!lstrcmpiW(argv[i], L"--settings"))  wantSettings = TRUE;
        else if (!lstrcmpiW(argv[i], L"--uninstall")) wantUninstall = TRUE;
        else if (!lstrcmpiW(argv[i], L"--installed")) afterInstall = TRUE;
        else if (!lstrcmpiW(argv[i], L"--radius") && i + 1 < argc)  cliRadius = _wtoi(argv[++i]);
        else if (!lstrcmpiW(argv[i], L"--cleanup") && i + 1 < argc) cleanupPid = (DWORD)_wtoi(argv[++i]);
        else if (!lstrcmpiW(argv[i], L"--after-update") && i + 1 < argc) afterUpdate = (DWORD)_wtoi(argv[++i]);
        else if (!lstrcmpiW(argv[i], L"--check-update-test") && i + 1 < argc) lstrcpynW(updTest, argv[++i], MAX_PATH);
    }
    if (argv) LocalFree(argv);

    if (cleanupPid) return Inst_Cleanup(cleanupPid);
    if (updTest[0]) return Upd_SelfTest(updTest);      /* prueba del actualizador, sin interfaz */
    if (afterUpdate) Upd_WaitFor(afterUpdate);          /* la versión anterior aún está saliendo */

    InitDpi();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    /* Desinstalación lanzada desde Configuración → Aplicaciones */
    if (wantUninstall) {
        LoadConfig();
        CloseRunningInstance();
        Inst_Unregister();
        Inst_RemoveFiles();
        g_noSave = TRUE;
        RegisterClasses(hInst);
        if (Notch_Show(NI_CHECK, L"OpenDock desinstalado", L"Hasta pronto", -1, TRUE)) {
            Notch_QuitOnHide();
            MessageLoop();
        }
        CoUninitialize();
        return 0;
    }

    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\OpenDock.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(CTRL_CLASS, NULL);
        if (other) {
            if (wantExit)       PostMessageW(other, WM_CLOSE, 0, 0);
            else if (cliRadius) PostMessageW(other, WM_SETRADIUS, (WPARAM)cliRadius, 0);
            else {
                /* volver a abrir el .exe = mostrar la configuración, sin diálogos */
                DWORD pid = 0;
                GetWindowThreadProcessId(other, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(other, WM_SHOWPANEL, 0, 0);
            }
        }
        if (mutex) CloseHandle(mutex);
        CoUninitialize();
        return 0;
    }
    if (wantExit) { CoUninitialize(); return 0; }

    /* Venimos de CornerRadius instalada: OpenDock ocupa su lugar y arranca la copia instalada. */
    if (Inst_MigrateLegacy() && !Inst_IsRunningInstalled() && Inst_Install(g_relaunch)) {
        if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
        wchar_t cmd[MAX_PATH + 16];
        wsprintfW(cmd, L"\"%s\" --installed", g_relaunch);
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        if (CreateProcessW(g_relaunch, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        CoUninitialize();
        return 0;
    }

    LoadConfig();
    if (cliRadius) {
        g_cfg.radius = max(RADIUS_MIN, min(RADIUS_MAX, cliRadius));
        Cfg_Save();
    }

    RegisterClasses(hInst);
    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_ctrl = CreateWindowExW(WS_EX_TOOLWINDOW, CTRL_CLASS, APP_NAME, WS_POPUP,
                             0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_ctrl) return 1;

    AddTrayIcon();
    const BOOL hotkeysOk =
        RegisterHotKey(g_ctrl, HK_TOGGLE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'R') &
        RegisterHotKey(g_ctrl, HK_UP,     MOD_CONTROL | MOD_ALT, VK_PRIOR) &
        RegisterHotKey(g_ctrl, HK_DOWN,   MOD_CONTROL | MOD_ALT, VK_NEXT);
    RegisterHotKey(g_ctrl, HK_BAR, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'B');
    RegisterHotKey(g_ctrl, HK_DOCK, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'D');

    g_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL,
                               ForegroundHook, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_locHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, NULL,
                                LocationHook, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetTimer(g_ctrl, TIMER_TOPMOST, 1500, NULL);
    SetTimer(g_ctrl, TIMER_DNDHINT, 15000, NULL);

    g_clipSeq = GetClipboardSequenceNumber();
    AddClipboardFormatListener(g_ctrl);
    InitializeCriticalSection(&g_shotLock);
    HANDLE watcher = CreateThread(NULL, 0, ShotWatcher, NULL, 0, NULL);
    if (watcher) CloseHandle(watcher);
    Wn_RestoreBanners();
    App_NotchConfigChanged();
    Bar_Apply();
    Dock_Apply();
    Launcher_Apply();
    RebuildCorners();

    RebuildCorners();

    const BOOL firstRun = RegReadDword(L"Welcomed", 0) == 0;
    if (firstRun) RegWriteDword(L"Welcomed", 1);

    Upd_Startup(afterUpdate);
    if (afterUpdate) { }    /* Upd_Startup ya avisó "Actualizado a …" */
    else if (afterInstall)
        Notch_Show(NI_CHECK, L"Instalada", L"Se inicia con Windows", -1, TRUE);
    else if (!hotkeysOk)
        Notch_Show(NI_WARN, L"Atajos ocupados", L"Otra app usa Ctrl+Alt+R/RePág", -1, FALSE);
    if (firstRun || wantSettings) Panel_Show();

    MessageLoop();

    CoUninitialize();
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }

    if (g_relaunch[0]) {
        wchar_t cmd[MAX_PATH + 64];
        wsprintfW(cmd, L"\"%s\" %s", g_relaunch, g_relaunchArgs);
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        if (CreateProcessW(g_relaunch, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    return 0;
}
