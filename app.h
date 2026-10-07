/* Declaraciones compartidas entre los módulos de OpenDock. */
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define APP_NAME        L"OpenDock"
#ifndef APP_VERSION             /* (las pruebas del actualizador lo cambian al compilar) */
#define APP_VERSION     L"2.3.0"
#endif
#define APP_PUBLISHER   L"OpenDock"
#define REG_KEY         L"Software\\OpenDock"

#define WM_TRAY         (WM_APP + 1)
#define WM_SETRADIUS    (WM_APP + 2)
#define WM_SHOWPANEL    (WM_APP + 3)
#define WM_SHOTFILE     (WM_APP + 4)   /* hay una captura guardada pendiente (sin parámetros) */
#define WM_WNCHANGED    (WM_APP + 5)   /* cambió la base de notificaciones de Windows */
#define WM_BARCHANGED   (WM_APP + 6)   /* la barra superior apareció/desapareció: recolocar esquinas */
#define WM_LAUNCHER     (WM_APP + 7)   /* tecla Windows sola: abrir o cerrar el buscador (w = 1: Win+N, el centro) */
#define WM_UPDATE       (WM_APP + 8)   /* actualizador: w = UPD_* (l = datos del hilo, si los hay) */
enum { UPD_CHECKED = 1, UPD_APPLY, UPD_READY, UPD_FAILED };
#define WM_POPFRAME     (WM_APP + 80)  /* fotograma de la animación de una ventana emergente */
#define WM_TRAYCHANGED  (WM_APP + 81)  /* cambió algún icono de la bandeja de Windows */

#define RADIUS_MIN      2
#define RADIUS_MAX      120

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

typedef struct {
    int  radius;
    BOOL enabled;
    BOOL hideCapture;
    BOOL notch;         /* avisos tipo notch en lugar de notificaciones de Windows */
    BOOL captures;      /* mostrar las capturas de pantalla en el notch */
    BOOL mirror;        /* mostrar las notificaciones reales de Windows en el notch */
    BOOL hideBanners;   /* notificaciones ofuscadas: campanita en lugar del contenido */
    BOOL edgeHover;     /* mini notch al llevar el cursor al borde superior */
    int  material;      /* MAT_OLED · MAT_GLASS · MAT_SYSTEM */
    int  size;          /* 0 compacto · 1 normal · 2 grande */
    int  accent;        /* 0 = acento de Windows, 1..N = kAccentPresets */
    int  bounce;        /* 0 suave · 1 normal · 2 muy bouncy */
    int  notchX;        /* centro del notch, 0..10000 del ancho del monitor */
    int  notchY;        /* 0 = pegado al borde superior; >0 = isla flotante (px lógicos) */
    BOOL floating;      /* estilo propio: la isla flota separada del borde en vez de ser un notch */
    int  sound;         /* sonido al llegar una notificación: 0 silencio · 1 Windows · 2.. Material (3 = Eco suave) */
    BOOL menubar;       /* barra superior estilo macOS */
    BOOL hideClock;     /* con la barra activa, ocultar el reloj de la barra de tareas */
    BOOL dock;          /* dock inferior estilo macOS */
    BOOL dockHideTaskbar; /* desactivar la barra de tareas de Windows mientras el dock está activo */
    int  dockBlur;      /* fondo del dock: 0 sin desenfoque · 1 suave · 2 intenso */
    int  dockOpacity;   /* 0..3 → 30 % · 55 % · 75 % · 92 % */
    int  dockIcon;      /* tamaño de icono: 0 pequeño · 1 mediano · 2 grande · 3 enorme */
    BOOL battPct;       /* barra superior: porcentaje dentro de la batería */
    int  dockAutoHide;  /* ocultar dock sin el cursor: 0 no · 1 a la mitad · 2 del todo */
    BOOL dockWinFull;   /* con el dock oculto, las ventanas maximizadas llegan hasta abajo */
    BOOL siteIcons;     /* avisos del navegador: el icono del sitio (si lo trae) en vez de su inicial */
    int  soundVol;      /* volumen del sonido de notificación, 0..100 */
    BOOL launcher;      /* la tecla Windows abre el buscador propio en vez del Inicio */
    int  autoUpdate;    /* buscar actualizaciones a diario: 1 sí · 0 no · -1 sin elegir aún (no se busca) */
} Config;

enum { MAT_OLED, MAT_GLASS, MAT_SYSTEM };
#define ACCENT_COUNT 7
extern const DWORD kAccentPresets[ACCENT_COUNT];

extern Config    g_cfg;
extern HINSTANCE g_inst;
extern HWND      g_ctrl;

/* ── corner_radius.c ── */
#define APPF_NOTIFY 1   /* mostrar aviso en el notch */
#define APPF_NOSAVE 2   /* no escribir el registro (p.ej. mientras se arrastra el slider) */
void  Cfg_Save(void);
UINT  MonitorDpi(HMONITOR m);
float RadiusToT(int r);
int   TToRadius(float t);
void  App_SetRadius(int r, UINT flags);
void  App_SetEnabled(BOOL on, UINT flags);
void  App_SetHideCapture(BOOL on);
void  App_SetNotch(BOOL on);
void  App_SetCaptures(BOOL on);
void  App_NotchConfigChanged(void);
void  App_SetMenuBar(BOOL on, BOOL notify);
void  App_SetHideClock(BOOL on);
void  App_SetDock(BOOL on, BOOL notify);
void  App_Install(void);
void  App_Uninstall(void);
void  App_Quit(void);
void  App_Relaunch(LPCWSTR exe, LPCWSTR args);   /* salir limpio y arrancar exe con args */
void  App_SetAutoUpdate(BOOL on);
BOOL  App_AutoUpdateShown(void);                 /* lo que enseña el interruptor (sin elegir: sí antes de instalar) */
DWORD RegReadDword(LPCWSTR name, DWORD def);
void  RegWriteDword(LPCWSTR name, DWORD v);

/* ── update.c: actualizaciones desde GitHub Releases (solo si el usuario las activó o las pide) ── */
void    Upd_Check(BOOL interactive);
void    Upd_Apply(void);
LPCWSTR Upd_Available(void);
void    Upd_OnMessage(WPARAM w, LPARAM l);
void    Upd_Schedule(void);
BOOL    Upd_Timer(UINT_PTR id);
void    Upd_Startup(DWORD afterPid);
void    Upd_WaitFor(DWORD pid);
int     Upd_SelfTest(LPCWSTR logPath);

/* ── gfx.c: lienzo BGRA + primitivas antialiasadas (SDF) ── */
typedef struct {
    HDC     dc;
    HBITMAP bmp;
    HGDIOBJ old;
    DWORD  *px;         /* 0xAARRGGBB, top-down */
    int     w, h;
} Canvas;

typedef struct {
    BOOL  dark;
    DWORD bg, fg, fg2, fg3, line, ctrl, ctrlHover, track, thumb, accent, onAccent, danger;
    DWORD accentOnBlack; /* acento legible sobre negro (notch) */
} Theme;

BOOL    Canvas_Init(Canvas *c, int w, int h);
void    Canvas_Free(Canvas *c);
void    Canvas_Clear(Canvas *c, DWORD rgb);
float   Gfx_SdRRect(float px, float py, float x, float y, float w, float h, float r);
float   Gfx_Cov(float d);
void    Gfx_Blend(Canvas *c, int x, int y, DWORD rgb, float a);
void    Gfx_FillRRect(Canvas *c, float x, float y, float w, float h, float r, DWORD rgb, float a);
void    Gfx_StrokeRRect(Canvas *c, float x, float y, float w, float h, float r, float th, DWORD rgb, float a);
void    Gfx_FillCircle(Canvas *c, float cx, float cy, float rad, DWORD rgb, float a);
DWORD   Gfx_Mix(DWORD a, DWORD b, float t);
void    Gfx_BoxBlur(DWORD *px, int w, int h, int r);
void    Gfx_BlitScaled(Canvas *dst, const Canvas *src, float cx, float cy, float k);

/* ── tray.c: los iconos de la bandeja de Windows, para la barra ── */
typedef struct { HWND hwnd; UINT uid; HICON icon; WCHAR tip[128]; } TrayItem;
void    Tray_Start(HWND notify);
void    Tray_Stop(void);
int     Tray_Snapshot(TrayItem *out, int max);
void    Tray_Click(HWND hwnd, UINT uid, BOOL right);
HWND    Tray_Explorer(void);

/* ── pop.c: ventanas emergentes con el muelle del notch ── */
typedef struct {
    HWND   hwnd;
    int    margin;              /* px alrededor del contenido (sombra y rebote) */
    int    mtop;                /* margen de arriba (0 si va pegada a la barra) */
    BOOL   attached;            /* estilo notch: pegada a la barra, se despliega hacia abajo */
    float  ear;                 /* radio de los hombros cóncavos (pegada) */
    BOOL   hold;                /* el dueño anima algo propio: seguir mandando fotogramas */
    float  aw, ah, vaw, vah;    /* pegada: forma animada (como la isla del notch) */
    float  fade, pillW;         /* aparición del contenido; ancho de la pastilla de origen */
    int    cw, ch;              /* tamaño del contenido presentado */
    float  radius, ax, ay;      /* radio de esquinas; ancla 0..1 de la que "sale" */
    float  s, vs, a, shadow;    /* escala, velocidad, opacidad */
    BOOL   closing;
    volatile LONG queued;
    Canvas frame;
    const Canvas *content;
    DWORD *base;                /* sombra precalculada (reposo) */
    BYTE  *mask;                /* cobertura de esquinas (reposo) */
    int    sw, sh;
    LARGE_INTEGER last;
} Pop;
typedef struct { Canvas cv; int x, y; } PopGlass;
void    Pop_Open(Pop *p, HWND h, int margin, float radius, float ax, float ay);
void    Pop_Close(Pop *p);
void    Pop_Destroyed(Pop *p);
void    Pop_Tick(Pop *p);
void    Pop_Step(Pop *p);
void    Pop_SetAttached(Pop *p, float ear, float pillW);
void    Pop_Hold(Pop *p, BOOL on);
float   Pop_Zeta(void);
void    Pop_Present(Pop *p, const Canvas *content);
void    Pop_SetBounds(Pop *p, int x, int y, int w, int h);
void    Pop_ContentRect(const Pop *p, RECT *r);
LPARAM  Pop_Mouse(const Pop *p, LPARAM l);
LRESULT Pop_NcHit(const Pop *p, LPARAM l);
void    Pop_CaptureGlass(PopGlass *g, int x, int y, int w, int h, int blur);
void    Pop_PaintGlass(Canvas *c, const PopGlass *g, int ox, int oy, DWORD tint, float op);
void    Pop_FreeGlass(PopGlass *g);

/* ── panel.c dentro del centro de control (con la barra superior activa) ── */
int     Panel_Embed(HWND host, UINT dpi, int tab, const PopGlass *glass, int ox, int oy);
void    Panel_Unembed(void);
BOOL    Panel_IsEmbedded(void);
const Canvas *Panel_RenderEmbedded(void);
LRESULT Panel_HostInput(HWND h, UINT m, WPARAM w, LPARAM l);
BOOL    Panel_IsTimer(WPARAM id);
/* menubar.c: el centro de control aloja los ajustes como segunda sección */
BOOL    Bar_IsOn(void);
void    CC_OpenSettings(int tab);
void    CC_ShowControls(void);
void    CC_SettingsResized(int h);
void    CC_CloseAll(void);
void    Gfx_Text(Canvas *c, HFONT f, LPCWSTR s, int x, int y, int w, int h, DWORD rgb, UINT fmt);
int     Gfx_TextWidth(HFONT f, LPCWSTR s);
HFONT   Gfx_Font(LPCWSTR face, int px, int weight, BYTE quality);
LPCWSTR Gfx_UiFace(void);
LPCWSTR Gfx_IconFace(void);
void    Theme_Load(Theme *t);

/* ── panel.c: mini ventana de configuración ── */
void Panel_Register(void);
void Panel_Show(void);
void Panel_ShowTab(int tab);
void Panel_Toggle(void);
void Panel_Close(void);
void Panel_Refresh(void);

/* ── notch.c: avisos tipo Dynamic Island ── */
enum { NI_RADIUS, NI_ON, NI_OFF, NI_CHECK, NI_INFO, NI_WARN };
void Notch_Register(void);
BOOL Notch_Show(int icon, LPCWSTR title, LPCWSTR detail, int level, BOOL force);
BOOL Notch_ShowCapture(const BITMAPINFO *bi, const void *bits, LPCWSTR title, LPCWSTR detail);
BOOL Notch_AttachFile(LPCWSTR path);
struct WinNote;
BOOL Notch_ShowWin(const struct WinNote *n);
void Notch_EdgeHover(POINT pt);
BOOL Notch_InEdgeZone(POINT pt, const RECT *mon, UINT dpi);
void Notch_OpenCenter(void);
BOOL Notch_ShowAction(int icon, LPCWSTR title, LPCWSTR detail, UINT msg, WPARAM w);   /* aviso que al pulsarlo manda msg al controlador */
void Notch_ToggleCenter(void);     /* Win+N: abre el centro de notificaciones, o lo cierra */
void Notch_NotesChanged(void);
void Notch_StyleChanged(void);
void Notch_Raise(void);
void Notch_PlaySound(void);
void Notch_ApplyCapture(void);
void Notch_QuitOnHide(void);
void Notch_Destroy(void);

/* ── winnotif.c: notificaciones reales de Windows ── */
#define WN_MAX 40
#define WN_ACTIONS 3
enum { WA_FOREGROUND, WA_BACKGROUND, WA_PROTOCOL };
typedef struct {
    wchar_t  label[32];
    wchar_t  args[400];
    wchar_t  input[24];         /* hint-inputId: el botón envía esa caja de texto */
    BYTE     type;              /* WA_* */
} WinAction;
typedef struct WinNote {
    LONGLONG id, arrival;       /* arrival = FILETIME */
    wchar_t  aumid[160];
    wchar_t  app[64];
    wchar_t  title[128];
    wchar_t  body[256];
    /* lo interactivo del aviso: abrir en su sitio, botones y respuesta rápida */
    wchar_t  launch[400];
    BYTE     launchType;
    WinAction act[WN_ACTIONS];
    int      nact;
    wchar_t  inputId[24], inputHint[48];
    BOOL     hasInput;
    wchar_t  logo[MAX_PATH];    /* icono propio del aviso (appLogoOverride), archivo local */
} WinNote;
void Wn_Start(void);
void Wn_Stop(void);
void Wn_Refresh(BOOL notify);
BOOL Wn_Changed(void);
int  Wn_Count(void);
int  Wn_QuietHours(void);   /* "No molestar" de Windows: -1 no se sabe · 0 apagado · 1 prioridad · 2 solo alarmas */
const WinNote *Wn_Get(int i);
void Wn_Dismiss(LONGLONG id);
void Wn_DismissAll(void);
void Wn_Open(const WinNote *n);
void Wn_Activate(const WinNote *w, int action, LPCWSTR reply);   /* action -1 = el aviso en sí */
void Wn_RestoreBannersSync(void);
void Wn_RestoreBanners(void);           /* deshace los ShowBanner=0 de versiones anteriores */
HBITMAP Wn_AppIcon(LPCWSTR aumid, int px);  /* icono 32 bpp al tamaño exacto; lo posee la caché */
HBITMAP Wn_LogoIcon(LPCWSTR path, int px);  /* el icono propio de un aviso (PNG, JPG, ICO…); ídem */
BOOL    Wn_IsSite(LPCWSTR aumid);           /* aviso de un sitio web (lo entrega el navegador) */

/* sonidos de notificación (notch.c): 0 silencio · 1 Windows · 2… los propios */
#define SOUND_COUNT 15
LPCWSTR Notch_SoundName(int i);
LPCWSTR Notch_SoundFamily(int i);  /* el icono propio de un aviso (PNG, JPG, ICO…); ídem */

/* ── menubar.c: barra superior estilo macOS + centro de control ── */
void Bar_Register(void);
void Bar_Apply(void);
void Bar_ApplyClock(BOOL hide);
void Bar_Reposition(void);
void Bar_ForegroundChanged(void);
int  Bar_HeightOn(const RECT *mon);
BOOL Bar_SystemSilent(void);         /* sistema en silencio o a cero: los avisos tampoco suenan */
BOOL App_Covered(HWND h);
BOOL App_ShellOpen(LPCWSTR target);         /* abre algo con %WINDIR%\\explorer.exe "target" (validado) */
BOOL App_ShellSelect(LPCWSTR path);        /* explorer /select,"path": abre su carpeta con él marcado */
void App_BarSurfaceChanged(void);           /* la barra cambió de color o de vidrio: repintar las esquinas */
BOOL App_FullscreenOn(const RECT *mon);     /* ventana a pantalla completa arriba del todo en ese monitor */
BOOL App_RemoteView(void);                  /* hay una sesión de escritorio remoto (Chrome Remote Desktop, RDP) */
BOOL App_HideFromCapture(BOOL glass);       /* ¿fuera de captura? (por la opción o por el vidrio; nunca en remoto) */
BOOL Bar_Surface(const RECT *mon, int sx, int sy, DWORD *rgb, BOOL *glass);                   /* ¿alguna ventana ajena (visible) está por encima? */
void Bar_Raise(void);
void Bar_FullscreenFg(void);                /* volver a mirar en cada monitor si hay pantalla completa */
void Bar_StyleChanged(void);
void Bar_PulseVolume(BOOL up);
void Bar_Destroy(void);

/* ── dock.c: dock inferior estilo macOS ── */
void Dock_Register(void);
void Dock_Apply(void);
void Dock_Reposition(void);
void Dock_Raise(void);
void Dock_FullscreenFg(void);
void Dock_RestoreTaskbar(void);
void Dock_ConfigChanged(void);
void Dock_TrayPeek(BOOL on);
void Dock_Destroy(void);
BOOL Dock_WindowAumid(HWND w, wchar_t *out, int cch);   /* AppUserModelID de una ventana (Store, PWA…) */
BOOL Dock_IsPinned(LPCWSTR target);     /* target: AppUserModelID ("aumid:" opcional), .exe o .lnk */
void Dock_TogglePin(LPCWSTR target);    /* anclar o quitar del dock (lo usa el buscador) */

/* ── menu.c: menú contextual propio (material y muelle de OpenDock) ── */
enum { MI_SEPARATOR = 1, MI_HEADER = 2, MI_DISABLED = 4, MI_CHECKED = 8, MI_DANGER = 16 };
typedef struct { int id; LPCWSTR text; WCHAR glyph; UINT flags; } MenuItem;   /* glyph: Segoe Fluent Icons */
void Menu_Register(void);
int  Menu_Track(const MenuItem *items, int n, POINT at);    /* encima de `at`; id elegido o 0 */

/* ── launcher.c: buscador de apps y archivos con la tecla Windows ── */
void Launcher_Register(void);
void Launcher_Apply(void);      /* arranca o para el gancho de la tecla y los índices según g_cfg.launcher */
void Launcher_Toggle(void);
void Launcher_Destroy(void);

/* ── install.c: instalación por usuario (sin admin) ── */
BOOL Inst_IsRunningInstalled(void);
BOOL Inst_MigrateLegacy(void);   /* de CornerRadius (nombre anterior): TRUE si estaba instalada */
BOOL Inst_Exists(void);
BOOL Inst_Install(wchar_t *outExe);
void Inst_Unregister(void);
void Inst_RemoveFiles(void);
int  Inst_Cleanup(DWORD waitPid);
BOOL Inst_IsStartup(void);
void Inst_SetStartup(BOOL on, LPCWSTR exe);
void Inst_UpdateInfo(void);      /* tras actualizar: la versión en Configuración → Aplicaciones */
