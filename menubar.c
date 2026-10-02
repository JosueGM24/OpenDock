/*
 * menubar.c — barra superior estilo macOS y centro de control.
 *
 * Barra: una franja fina en el borde superior del monitor principal, registrada como
 * AppBar (SHAppBarMessage): Windows le reserva el espacio y las ventanas maximizadas
 * quedan debajo, así nunca tapa sus botones. Izquierda: icono y nombre de la app activa. Derecha:
 * volumen, Wi-Fi, batería, fecha/hora y el botón del centro de control. Se oculta sola
 * con apps a pantalla completa.
 *
 * Centro de control: fichas (Wi-Fi, batería, No molestar, notificaciones) y
 * deslizadores reales de brillo (WMI) y volumen (Core Audio).
 *
 * "Ocultar reloj de Windows" usa el ajuste oficial ShowSystrayDateTimeValueName y se
 * restaura al quitar la barra, al salir y al desinstalar.
 */
#define COBJMACROS
#include "app.h"
#include <shellapi.h>
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <wlanapi.h>
#include <wbemcli.h>
#include <bluetoothapis.h>
#include <math.h>
#include <shobjidl.h>
#include "resource.h"

#define BAR_CLASS       L"OpenDock.MenuBar"
#define CC_CLASS        L"OpenDock.ControlCenter"
#define WM_BAR_APPBAR   (WM_APP + 60)
#define TIMER_CLOCK     1
#define TIMER_STATUS    2
#define TIMER_BANIM     3       /* muelles de los iconos de la barra */
#define TIMER_TRAY      4       /* abrir la bandeja de Windows: Win+B y luego Entrar */
#define TIMER_WIFIQ     5       /* releer el Wi-Fi tras un aviso (agrupa ráfagas) */
#define WM_BAR_STATUS   (WM_APP + 61)   /* aviso de Windows: 1 = volumen, 2 = Wi-Fi */
#define BAR_H           28      /* alto lógico de la barra */
#define CC_W            340     /* el mismo ancho que los ajustes, que viven dentro */

#define ADVANCED_KEY    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced"
#define CLOCK_VALUE     L"ShowSystrayDateTimeValueName"

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

/* GUID propios: no todos están en libuuid de mingw */
static const GUID kCLSID_MMDeviceEnumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID kIID_IMMDeviceEnumerator  = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID kIID_IAudioEndpointVolume = { 0x5CDF2C82, 0x841E, 0x4546, { 0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A } };
static const GUID kIID_IAudioEndpointVolumeCallback = { 0x657804FA, 0xD6AD, 0x4496, { 0x8A, 0x60, 0x35, 0x27, 0x52, 0xAF, 0x4F, 0x89 } };
static const GUID kIID_IUnknown = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID kGUID_BatteryPercent = { 0xA7AD8041, 0xB45A, 0x4CAE, { 0x87, 0xA3, 0xEE, 0xCB, 0xB4, 0x68, 0xA9, 0xE1 } };

enum { BH_NONE, BH_LOGO, BH_APP, BH_VOL, BH_WIFI, BH_BATT, BH_CLOCK, BH_CC, BH_TRAY, BH_COUNT };

typedef struct {
    DWORD bg, fg, fg2, tile, tileOn, track, accent;
    BOOL  light;
} BarLook;

static struct {
    HWND    hwnd;
    UINT    dpi;
    RECT    mon;
    int     h;
    Canvas  cv;
    HFONT   fBold, fText, fIcon;
    BarLook look;
    BOOL    registered, fullscreen;
    RECT    hit[BH_COUNT];
    int     pressed;
    int     bhot;               /* icono bajo el cursor */
    int     trayStep;
    float   bs[BH_COUNT], bsv[BH_COUNT];    /* su escala animada (vectorial: crece nítido) */
    LARGE_INTEGER blast;
    /* gestos de estado: silencio (am), nivel del altavoz (vl), ondas (rip), Wi-Fi (wl),
     * carga (bc) y nivel de la batería (bf), cada uno con su muelle */
    float   am, amv, vl, vlv, rip, ripv, wl, wlv, bc, bcv, bf, bfv;
    BOOL    synced, sMute, sWifi, sCharge;
    float   sVol;
    int     sBatt;
    HPOWERNOTIFY battNotify;
    wchar_t app[64];
    HBITMAP appIcon;            /* icono de la app activa (NULL: el logo) */
    int     appIconPx;
    wchar_t appKey[MAX_PATH];   /* de qué es ese icono: AppUserModelID o ruta del ejecutable */
    int     lastMinute;

    /* estado del sistema */
    int     battery;            /* 0..100, -1 sin batería */
    BOOL    charging;
    int     wifi;               /* -1 sin conexión inalámbrica, 0..100 calidad */
    wchar_t ssid[64];
    float   volume;
    BOOL    muted;
    IAudioEndpointVolume *ep;
    HANDLE  wlan;
} B;

static int BS(int v) { return MulDiv(v, (int)B.dpi, 96); }

/* ───────────────────────── Aspecto ───────────────────────── */
static void LoadBarLook(void)
{
    Theme th;
    Theme_Load(&th);
    BarLook *l = &B.look;
    l->light = g_cfg.material == MAT_SYSTEM && !th.dark;
    if (l->light) {
        l->bg = 0xF2F2F7; l->fg = 0x000000; l->fg2 = 0x3C3C43; l->tile = 0xFFFFFF; l->track = 0xD1D1D6;
    } else {
        l->bg = g_cfg.material == MAT_OLED ? 0x000000 : 0x1C1C1E;
        l->fg = 0xFFFFFF; l->fg2 = 0xC7C7CC; l->tile = 0x2C2C2E; l->track = 0x48484A;
    }
    const int a = max(0, min(ACCENT_COUNT - 1, g_cfg.accent));
    l->accent = a ? kAccentPresets[a] : (l->light ? th.accent : th.accentOnBlack);
    l->tileOn = l->accent;
    if (B.hwnd) App_BarSurfaceChanged();   /* las esquinas de arriba llevan el color de la barra */
}

/* ───────────────────────── Vidrio ─────────────────────────
 * Con el material "Vidrio" la barra se pinta sobre lo que hay detrás (fondo de escritorio
 * o ventanas), capturado a 1/4, desenfocado y teñido, igual que el notch y el dock. La
 * barra queda fuera de captura para no verse a sí misma. */
#define BACK_SCALE 4
static Canvas s_back;
static DWORD  s_backHash;

static BOOL BarGlass(void) { return g_cfg.material == MAT_GLASS && !B.look.light; }

static BOOL CaptureBarBack(void)
{
    if (!B.hwnd || !BarGlass()) return FALSE;
    /* algo más que la barra: el desenfoque de su borde de abajo cuenta con lo que hay
     * debajo, igual que el del notch pegado cuenta con lo que hay detrás de la barra */
    const int below = max(BS(24), MulDiv(g_cfg.radius, (int)B.dpi, 96));      /* + las esquinas de arriba */
    const int sw = max(1, (B.mon.right - B.mon.left) / BACK_SCALE), sh = max(1, (B.h + below + BACK_SCALE - 1) / BACK_SCALE);
    if (s_back.w != sw || s_back.h != sh) {
        Canvas_Free(&s_back);
        if (!Canvas_Init(&s_back, sw, sh)) return FALSE;
    }
    HDC screen = GetDC(NULL);
    SetStretchBltMode(s_back.dc, HALFTONE);
    SetBrushOrgEx(s_back.dc, 0, 0, NULL);
    StretchBlt(s_back.dc, 0, 0, sw, sh, screen, B.mon.left, B.mon.top, sw * BACK_SCALE, sh * BACK_SCALE, SRCCOPY);
    ReleaseDC(NULL, screen);
    GdiFlush();
    DWORD hash = 2166136261u;
    for (int i = 0; i < sw * sh; ++i) hash = (hash ^ (s_back.px[i] & 0xFFFFFF)) * 16777619u;
    const BOOL changed = hash != s_backHash;
    s_backHash = hash;
    for (int i = 0; i < 2; ++i) Gfx_BoxBlur(s_back.px, sw, sh, max(1, BS(16) / BACK_SCALE));
    for (int i = 0; i < sw * sh; ++i) {           /* vibrancia */
        const DWORD v = s_back.px[i];
        const int R = (v >> 16) & 255, G = (v >> 8) & 255, Bl = v & 255, Y = (R * 77 + G * 151 + Bl * 28) >> 8;
        s_back.px[i] = (DWORD)max(0, min(255, Y + (R - Y) * 3 / 2)) << 16 |
                       (DWORD)max(0, min(255, Y + (G - Y) * 3 / 2)) << 8 |
                       (DWORD)max(0, min(255, Y + (Bl - Y) * 3 / 2));
    }
    return changed;
}

/* El vidrio en (x, y) relativo a la esquina superior izquierda de la barra. */
static DWORD GlassAt(int x, int y, DWORD tint)
{
    const float fy = (y + 0.5f) / BACK_SCALE - 0.5f, fx = (x + 0.5f) / BACK_SCALE - 0.5f;
    const int y0 = max(0, min(s_back.h - 1, (int)floorf(fy))), y1 = min(s_back.h - 1, y0 + 1);
    const int x0 = max(0, min(s_back.w - 1, (int)floorf(fx))), x1 = min(s_back.w - 1, x0 + 1);
    const float ty = max(0.0f, min(1.0f, fy - y0)), tx = max(0.0f, min(1.0f, fx - x0));
    const DWORD *p = s_back.px;
    const DWORD b = Gfx_Mix(Gfx_Mix(p[y0 * s_back.w + x0], p[y0 * s_back.w + x1], tx),
                            Gfx_Mix(p[y1 * s_back.w + x0], p[y1 * s_back.w + x1], tx), ty);
    return Gfx_Mix(b, tint, 0.52f) & 0xFFFFFF;
}

static void PaintGlass(Canvas *c, DWORD tint)
{
    GdiFlush();
    for (int y = 0; y < c->h; ++y)
        for (int x = 0; x < c->w; ++x) c->px[y * c->w + x] = GlassAt(x, y, tint);
}

/* Las esquinas redondeadas de arriba empiezan bajo la barra. En vez de negro se pintan con
 * la superficie de la barra (su vidrio, o su color), así la barra parece curvarse hacia
 * abajo en las esquinas. FALSE si en ese monitor no hay barra. glass: si es vidrio (cambia
 * con lo de detrás; esas esquinas quedan fuera de captura, como la barra). */
BOOL Bar_Surface(const RECT *mon, int sx, int sy, DWORD *rgb, BOOL *glass)
{
    if (!Bar_HeightOn(mon)) return FALSE;
    *glass = BarGlass() && (s_back.px || CaptureBarBack());
    *rgb = *glass ? GlassAt(sx - B.mon.left, sy - B.mon.top, B.look.bg) : B.look.bg;
    return TRUE;
}

static void BarApplyCapture(void)
{
    if (B.hwnd)
        SetWindowDisplayAffinity(B.hwnd, g_cfg.hideCapture || BarGlass() ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
}

/* ───────────────────────── Estado del sistema ───────────────────────── */
static void ReadBattery(void)
{
    SYSTEM_POWER_STATUS ps;
    B.battery = -1;
    B.charging = FALSE;
    if (!GetSystemPowerStatus(&ps) || (ps.BatteryFlag & 128) || ps.BatteryLifePercent > 100) return;
    B.battery = ps.BatteryLifePercent;
    B.charging = ps.ACLineStatus == 1;
}

/* Avisos del Wi-Fi (conectado, desconectado, cambio de señal): llegan en otro hilo y solo
 * dejan un mensaje; el escaneo de la lista no cuenta. */
static void WINAPI WlanNotify(PWLAN_NOTIFICATION_DATA d, PVOID ctx)
{
    (void)ctx;
    if (!d || !B.hwnd) return;
    const DWORD k = d->NotificationCode;
    if ((d->NotificationSource == WLAN_NOTIFICATION_SOURCE_ACM &&
         (k == wlan_notification_acm_connection_complete || k == wlan_notification_acm_disconnected ||
          k == wlan_notification_acm_interface_arrival || k == wlan_notification_acm_interface_removal)) ||
        (d->NotificationSource == WLAN_NOTIFICATION_SOURCE_MSM &&
         (k == wlan_notification_msm_signal_quality_change || k == wlan_notification_msm_connected ||
          k == wlan_notification_msm_disconnected)))
        PostMessageW(B.hwnd, WM_BAR_STATUS, 2, 0);
}

static void ReadWifi(void)
{
    B.wifi = -1;
    B.ssid[0] = 0;
    DWORD ver = 0;
    if (!B.wlan) {
        if (WlanOpenHandle(2, NULL, &ver, &B.wlan) != ERROR_SUCCESS) { B.wlan = NULL; return; }
        WlanRegisterNotification(B.wlan, WLAN_NOTIFICATION_SOURCE_ACM | WLAN_NOTIFICATION_SOURCE_MSM, TRUE,
                                 WlanNotify, NULL, NULL, NULL);
    }
    PWLAN_INTERFACE_INFO_LIST list = NULL;
    if (WlanEnumInterfaces(B.wlan, NULL, &list) != ERROR_SUCCESS || !list) return;
    for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
        if (list->InterfaceInfo[i].isState != wlan_interface_state_connected) continue;
        PWLAN_CONNECTION_ATTRIBUTES ca = NULL;
        DWORD sz = 0;
        WLAN_OPCODE_VALUE_TYPE vt;
        if (WlanQueryInterface(B.wlan, &list->InterfaceInfo[i].InterfaceGuid, wlan_intf_opcode_current_connection,
                               NULL, &sz, (PVOID *)&ca, &vt) == ERROR_SUCCESS && ca) {
            B.wifi = (int)ca->wlanAssociationAttributes.wlanSignalQuality;
            const DOT11_SSID *s = &ca->wlanAssociationAttributes.dot11Ssid;
            const int n = MultiByteToWideChar(CP_UTF8, 0, (const char *)s->ucSSID, (int)min(s->uSSIDLength, 32), B.ssid, 63);
            B.ssid[max(0, n)] = 0;
            WlanFreeMemory(ca);
            break;
        }
    }
    WlanFreeMemory(list);
}

/* Avisos de Core Audio: el volumen o el silencio cambiaron (teclas del teclado, otra app). */
static HRESULT STDMETHODCALLTYPE VcQuery(IAudioEndpointVolumeCallback *self, REFIID r, void **out)
{
    if (IsEqualIID(r, &kIID_IUnknown) || IsEqualIID(r, &kIID_IAudioEndpointVolumeCallback)) { *out = self; return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE VcRef(IAudioEndpointVolumeCallback *self) { (void)self; return 1; }
static HRESULT STDMETHODCALLTYPE VcNotify(IAudioEndpointVolumeCallback *self, PAUDIO_VOLUME_NOTIFICATION_DATA d)
{
    (void)self; (void)d;
    if (B.hwnd) PostMessageW(B.hwnd, WM_BAR_STATUS, 1, 0);
    return S_OK;
}
static IAudioEndpointVolumeCallbackVtbl s_vcVtbl = { VcQuery, VcRef, VcRef, VcNotify };
static IAudioEndpointVolumeCallback s_volCb = { &s_vcVtbl };

static void CloseVolume(void)
{
    if (!B.ep) return;
    IAudioEndpointVolume_UnregisterControlChangeNotify(B.ep, &s_volCb);
    IAudioEndpointVolume_Release(B.ep);
    B.ep = NULL;
}

static BOOL OpenVolume(void)
{
    if (B.ep) return TRUE;
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    if (FAILED(CoCreateInstance(&kCLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER, &kIID_IMMDeviceEnumerator, (void **)&en)))
        return FALSE;
    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev))) {
        IMMDevice_Activate(dev, &kIID_IAudioEndpointVolume, CLSCTX_INPROC_SERVER, NULL, (void **)&B.ep);
        IMMDevice_Release(dev);
    }
    IMMDeviceEnumerator_Release(en);
    if (B.ep) IAudioEndpointVolume_RegisterControlChangeNotify(B.ep, &s_volCb);
    return B.ep != NULL;
}

static void ReadVolume(void)
{
    if (!OpenVolume()) { B.volume = -1; return; }
    float v = 0;
    BOOL m = FALSE;
    if (FAILED(IAudioEndpointVolume_GetMasterVolumeLevelScalar(B.ep, &v))) {
        CloseVolume();                          /* el dispositivo cambió: reabrir la próxima vez */
        B.volume = -1;
        return;
    }
    IAudioEndpointVolume_GetMute(B.ep, &m);
    B.volume = v;
    B.muted = m;
}

static void SetVolume(float v)
{
    if (!OpenVolume()) return;
    v = max(0.0f, min(1.0f, v));
    IAudioEndpointVolume_SetMasterVolumeLevelScalar(B.ep, v, NULL);
    if (B.muted && v > 0) IAudioEndpointVolume_SetMute(B.ep, FALSE, NULL);
    B.volume = v;
    if (v > 0) B.muted = FALSE;
}

static void ToggleMute(void)
{
    if (!OpenVolume()) return;
    B.muted = !B.muted;
    IAudioEndpointVolume_SetMute(B.ep, B.muted, NULL);
}

/* Nombre "humano" de la app en primer plano (descripción del ejecutable). */
static void ForegroundAppName(wchar_t *out, int cap)
{
    out[0] = 0;
    HWND w = GetForegroundWindow();
    if (!w) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return;
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(p, 0, path, &n);
    CloseHandle(p);
    if (!ok) return;

    const wchar_t *base = path;
    for (const wchar_t *q = path; *q; ++q) if (*q == L'\\') base = q + 1;
    if (!lstrcmpiW(base, L"explorer.exe")) {
        wchar_t cls[64];
        GetClassNameW(w, cls, 64);
        lstrcpynW(out, !lstrcmpW(cls, L"CabinetWClass") ? L"Explorador" : L"Escritorio", cap);
        return;
    }
    if (!lstrcmpiW(base, L"ApplicationFrameHost.exe")) {   /* apps de la Store: el título es el nombre */
        GetWindowTextW(w, out, cap);
        return;
    }
    DWORD h = 0;
    const DWORD sz = GetFileVersionInfoSizeW(path, &h);
    if (sz && sz < 4 * 1024 * 1024) {
        BYTE *buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, sz);
        if (buf && GetFileVersionInfoW(path, 0, sz, buf)) {
            struct { WORD lang, cp; } *tr = NULL;
            UINT len = 0;
            if (VerQueryValueW(buf, L"\\VarFileInfo\\Translation", (void **)&tr, &len) && len >= 4) {
                wchar_t q[64];
                wchar_t *desc = NULL;
                wsprintfW(q, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr->lang, tr->cp);
                if (VerQueryValueW(buf, q, (void **)&desc, &len) && len > 1 && desc[0]) lstrcpynW(out, desc, cap);
            }
        }
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
    }
    if (!out[0]) {
        lstrcpynW(out, base, cap);
        wchar_t *dot = wcsrchr(out, L'.');
        if (dot) *dot = 0;
    }
}

/* Icono de la app en primer plano: el de su AppUserModelID si lo tiene (apps de la Store,
 * PWA, y el de las ventanas que lo declaran), si no el de su ejecutable. Solo se pide al
 * sistema cuando cambia la app. TRUE si cambió. */
static BOOL ForegroundAppIcon(int px)
{
    HWND w = GetForegroundWindow();
    if (!w) return FALSE;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return FALSE;
    wchar_t key[MAX_PATH], parse[MAX_PATH + 24];
    if (Dock_WindowAumid(w, key, 128)) {
        wsprintfW(parse, L"shell:AppsFolder\\%s", key);
    } else {
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!p) return FALSE;
        DWORD n = MAX_PATH;
        const BOOL ok = QueryFullProcessImageNameW(p, 0, key, &n);
        CloseHandle(p);
        if (!ok) return FALSE;
        lstrcpynW(parse, key, MAX_PATH);
    }
    if (B.appIconPx == px && !lstrcmpiW(key, B.appKey)) return FALSE;
    lstrcpynW(B.appKey, key, MAX_PATH);
    B.appIconPx = px;
    if (B.appIcon) { DeleteObject(B.appIcon); B.appIcon = NULL; }
    IShellItem *si = NULL;
    if (SUCCEEDED(SHCreateItemFromParsingName(parse, NULL, &IID_IShellItem, (void **)&si))) {
        IShellItemImageFactory *f = NULL;
        if (SUCCEEDED(IShellItem_QueryInterface(si, &IID_IShellItemImageFactory, (void **)&f))) {
            SIZE sz = { px, px };
            IShellItemImageFactory_GetImage(f, sz, SIIGBF_RESIZETOFIT | SIIGBF_ICONONLY, &B.appIcon);
            IShellItemImageFactory_Release(f);
        }
        IShellItem_Release(si);
    }
    return TRUE;
}

/* Compone un mapa de bits de 32 bpp (alfa premultiplicado o no) centrado en s×s. */
static BOOL BlitAppIcon(Canvas *c, HBITMAP icon, int x, int y, int s)
{
    BITMAP bm;
    if (!icon || !GetObjectW(icon, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmWidth > 256 || abs(bm.bmHeight) > 256) return FALSE;
    const int w = bm.bmWidth, h = abs(bm.bmHeight);
    DWORD *px = (DWORD *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)w * h * 4);
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    if (!px || GetDIBits(c->dc, icon, 0, h, px, &bi, DIB_RGB_COLORS) != h) { if (px) HeapFree(GetProcessHeap(), 0, px); return FALSE; }
    BOOL premult = TRUE, anyAlpha = FALSE;
    for (int i = 0; i < w * h; ++i) {
        const DWORD v = px[i], a = v >> 24;
        if (a) anyAlpha = TRUE;
        if (((v >> 16) & 255) > a || ((v >> 8) & 255) > a || (v & 255) > a) premult = FALSE;
    }
    GdiFlush();
    const int ox = x + (s - w) / 2, oy = y + (s - h) / 2;
    for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
            const DWORD v = px[yy * w + xx];
            const int a = anyAlpha ? (int)(v >> 24) : 255;
            if (!a) continue;
            DWORD rgb = v & 0xFFFFFF;
            if (premult && anyAlpha && a < 255)
                rgb = (DWORD)min(255, (int)((v >> 16) & 255) * 255 / a) << 16 | (DWORD)min(255, (int)((v >> 8) & 255) * 255 / a) << 8
                    | (DWORD)min(255, (int)(v & 255) * 255 / a);
            Gfx_Blend(c, ox + xx, oy + yy, rgb, a / 255.0f);
        }
    HeapFree(GetProcessHeap(), 0, px);
    return TRUE;
}

/* ───────────────────────── Iconos vectoriales ─────────────────────────
 * Diseñados en el lienzo "Iconos de la barra" sobre una caja de 24×24 unidades y
 * pintados aquí con el mismo trazado: cada píxel se lleva al espacio del icono y se
 * mide su distancia con signo a cada forma → antialiasado a cualquier tamaño. */
typedef struct { float x, y; } V2;

/* Distancia con signo a un polígono (negativa dentro). */
static float SdPoly(V2 p, const V2 *v, int n)
{
    float d = 1e9f;
    BOOL in = FALSE;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        const float ex = v[i].x - v[j].x, ey = v[i].y - v[j].y, wx = p.x - v[j].x, wy = p.y - v[j].y;
        float t = (wx * ex + wy * ey) / (ex * ex + ey * ey);
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        const float dx = wx - ex * t, dy = wy - ey * t;
        d = min(d, dx * dx + dy * dy);
        if ((v[i].y > p.y) != (v[j].y > p.y) && p.x < (v[j].x - v[i].x) * (p.y - v[i].y) / (v[j].y - v[i].y) + v[i].x) in = !in;
    }
    return in ? -sqrtf(d) : sqrtf(d);
}

/* Gira p alrededor de (cx, cy); deg > 0 = horario en pantalla (como CSS rotate). */
static V2 Rot(V2 p, float cx, float cy, float deg)
{
    const float a = deg * 0.01745329f, cs = cosf(a), sn = sinf(a), x = p.x - cx, y = p.y - cy;
    V2 r = { cx + x * cs - y * sn, cy + x * sn + y * cs };
    return r;
}

static float SdSeg(V2 p, float ax, float ay, float bx, float by)
{
    const float ex = bx - ax, ey = by - ay, wx = p.x - ax, wy = p.y - ay;
    float t = (wx * ex + wy * ey) / (ex * ex + ey * ey);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return hypotf(wx - ex * t, wy - ey * t);
}

/* Arco de radio r con centro (cx, cy) entre los ángulos a0..a1 (radianes, pantalla). */
static float SdArc(V2 p, float cx, float cy, float r, float a0, float a1)
{
    const float dx = p.x - cx, dy = p.y - cy, ang = atan2f(dy, dx);
    if (ang >= a0 && ang <= a1) return fabsf(hypotf(dx, dy) - r);
    return min(hypotf(p.x - (cx + r * cosf(a0)), p.y - (cy + r * sinf(a0))),
               hypotf(p.x - (cx + r * cosf(a1)), p.y - (cy + r * sinf(a1))));
}

/* Recorre la caja del icono: (cx, cy) centro en px, size = 24 unidades en px. */
#define ICON_LOOP(cx, cy, size) \
    const float u_ = (size) / 24.0f; GdiFlush(); \
    for (int py = (int)((cy) - (size) * 0.5f) - 1; py <= (int)((cy) + (size) * 0.5f) + 1; ++py) \
        for (int px = (int)((cx) - (size) * 0.5f) - 1; px <= (int)((cx) + (size) * 0.5f) + 1; ++px) { \
            if (px < 0 || py < 0 || px >= c->w || py >= c->h) continue; \
            const V2 q = { (px + 0.5f - (cx)) / u_ + 12.0f, (py + 0.5f - (cy)) / u_ + 12.0f };
#define ICON_END }
#define COV(d) Gfx_Cov((d) * u_)

/* Ajustes (D1): engrane de 6 dientes de esquinas suaves, girado −30° (dientes arriba y abajo). */
static const V2 kGear[] = {
    { 18.578f, 9.786f }, { 21.226f, 9.734f }, { 21.226f, 14.266f }, { 18.578f, 14.214f },
    { 17.206f, 16.590f }, { 18.575f, 18.857f }, { 14.650f, 21.123f }, { 13.372f, 18.803f },
    { 10.628f, 18.803f }, { 9.350f, 21.123f }, { 5.425f, 18.857f }, { 6.794f, 16.590f },
    { 5.422f, 14.214f }, { 2.774f, 14.266f }, { 2.774f, 9.734f }, { 5.422f, 9.786f },
    { 6.794f, 7.410f }, { 5.425f, 5.143f }, { 9.350f, 2.877f }, { 10.628f, 5.197f },
    { 13.372f, 5.197f }, { 14.650f, 2.877f }, { 18.575f, 5.143f }, { 17.206f, 7.410f }
};
static void DrawGear(Canvas *c, float cx, float cy, float size, DWORD rgb)
{
    ICON_LOOP(cx, cy, size)
        const V2 g = Rot(q, 12, 12, 30.0f);          /* inverso del giro de −30° */
        const float sd = max(SdPoly(g, kGear, (int)(sizeof(kGear) / sizeof(kGear[0]))) - 1.1f,
                             3.0f - hypotf(g.x - 12, g.y - 12));
        const float a = COV(sd);
        if (a > 0) Gfx_Blend(c, px, py, rgb, a);
    ICON_END
}

/* Wi-Fi (W1): punto y dos arcos gruesos, inclinado 50° (15° del diseño + 35° del ajuste).
 * lv = cuánto está encendido, continuo: 0 sin conexión (todo tenue), 1 el punto, 2 el primer
 * arco, 3 los dos. Animado, al conectar se encienden en cascada desde el punto. */
static float Lit(float lv, int i) { return 0.28f + 0.72f * max(0.0f, min(1.0f, lv - i)); }
static void DrawWifi(Canvas *c, float cx, float cy, float size, float lv, DWORD fg)
{
    const float l0 = Lit(lv, 0), l1 = Lit(lv, 1), l2 = Lit(lv, 2);
    /* cada pieza crece un poco justo mientras se enciende */
    const float g1 = 0.5f * sinf(3.14159f * max(0.0f, min(1.0f, lv - 1))), g2 = 0.5f * sinf(3.14159f * max(0.0f, min(1.0f, lv - 2)));
    ICON_LOOP(cx, cy, size)
        const V2 w = Rot(Rot(q, 12, 12, -35.0f), 12, 15, -15.0f);
        const float a0 = -2.35619f, a1 = -0.78540f;  /* −135° … −45°: el abanico hacia arriba */
        const float dDot = hypotf(w.x - 12, w.y - 19.4f) - 1.7f;
        const float dA1 = SdArc(w, 12, 20, 5.2f + g1, a0, a1) - 1.3f, dA2 = SdArc(w, 12, 20, 9.6f + g2, a0, a1) - 1.3f;
        float a = COV(dDot) * l0;
        a = max(a, COV(dA1) * l1);
        a = max(a, COV(dA2) * l2);
        if (a > 0) Gfx_Blend(c, px, py, fg, a);
    ICON_END
}

/* Batería (B1): contorno fino, relleno dentro y el número calado (color del fondo sobre el
 * relleno, del texto sobre lo vacío). Cargando: rayo en lugar del número, relleno verde. */
static const V2 kBolt[] = { { 12, 7.6f }, { 8.6f, 12.6f }, { 11.6f, 12.6f }, { 10.6f, 16.4f }, { 14.4f, 11.2f }, { 11.4f, 11.2f }, { 12.2f, 7.6f } };
/* fill = nivel dibujado (animado, 0..100), pct = el número; chg = 0..1 cuánto se ve la carga
 * (animado: el rayo entra con rebote, el relleno pasa a verde y el número se desvanece). */
static void DrawBattery(Canvas *c, float cx, float cy, float size, float fill, int pct, float chg, BOOL number, DWORD fg, DWORD knock)
{
    pct = max(0, min(100, pct));
    fill = max(0.0f, min(100.0f, fill));
    const float ch = max(0.0f, min(1.0f, chg)), bs = max(0.01f, chg);    /* bs: escala del rayo */
    const float fillR = 3.35f + 1.4f + 13.9f * fill / 100.0f;
    const DWORD col = Gfx_Mix(pct <= 20 ? 0xFF453A : fg, 0x30D158, ch);

    /* el número se rasteriza una vez como máscara de cobertura */
    Canvas m = { 0 };
    const float uu = size / 24.0f;
    if (number && ch < 0.99f && Canvas_Init(&m, (int)ceilf(size) + 2, (int)ceilf(size) + 2)) {
        static HFONT f; static int fpx;
        const int want = max(6, (int)(7.8f * uu + 0.5f));
        if (!f || fpx != want) { if (f) DeleteObject(f); f = Gfx_Font(Gfx_UiFace(), want, FW_BOLD, ANTIALIASED_QUALITY); fpx = want; }
        wchar_t t[8];
        wsprintfW(t, L"%d", pct);
        Canvas_Clear(&m, 0);
        Gfx_Text(&m, f, t, 0, 0, m.w, m.h, 0xFFFFFF, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        GdiFlush();
    }
    int mx0 = (int)(cx - size * 0.5f) - 1, my0 = (int)(cy - size * 0.5f) - 1;
    if (m.px) {
        /* la caja de la fuente deja aire distinto arriba y abajo: se centra la tinta de los
         * dígitos en la cavidad (centro en x = 11 unidades, y = 12) */
        int bx0 = m.w, by0 = m.h, bx1 = -1, by1 = -1;
        for (int yy = 0; yy < m.h; ++yy)
            for (int xx = 0; xx < m.w; ++xx)
                if (((m.px[yy * m.w + xx] >> 8) & 255) > 60) {
                    bx0 = min(bx0, xx); bx1 = max(bx1, xx); by0 = min(by0, yy); by1 = max(by1, yy);
                }
        if (bx1 >= 0) {
            const float inkx = (bx0 + bx1 + 1) * 0.5f, inky = (by0 + by1 + 1) * 0.5f;
            mx0 = (int)lroundf(cx - uu - inkx);
            my0 = (int)lroundf(cy - inky);
        }
    }

    ICON_LOOP(cx, cy, size)
        /* contorno y botón (tenues) */
        /* contorno con el mismo peso que los arcos del Wi-Fi y el altavoz */
        const float dBody = fabsf(Gfx_SdRRect(q.x, q.y, 1.5f, 6.5f, 19, 11, 3.8f)) - 0.85f;
        const float dNub = SdSeg(q, 22.6f, 10.3f, 22.6f, 13.7f) - 1.0f;
        const float ao = max(COV(dBody), COV(dNub));
        if (ao > 0) Gfx_Blend(c, px, py, fg, ao);
        /* relleno, recortado a la cavidad */
        const float dIn = Gfx_SdRRect(q.x, q.y, 3.35f, 8.35f, 15.3f, 7.3f, 2.0f);
        const float af = COV(max(dIn, q.x - fillR));
        if (af > 0) Gfx_Blend(c, px, py, col, af);
        /* número y rayo, calados sobre el relleno; se cruzan al enchufar o desenchufar */
        float at = 0, ab = 0;
        if (chg > 0.01f) {
            const V2 qb = { (q.x - 11.5f) / bs + 11.5f, (q.y - 12.0f) / bs + 12.0f };
            ab = COV(SdPoly(qb, kBolt, 7) * bs);
        }
        if (m.px && ch < 0.99f) {
            const int ix = px - mx0, iy = py - my0;
            if (ix >= 0 && iy >= 0 && ix < m.w && iy < m.h) at = ((m.px[iy * m.w + ix] >> 8) & 255) / 255.0f * (1.0f - ch);
        }
        if (at > 0 || ab > 0) {
            const float inFill = COV(max(dIn, q.x - fillR));
            if (at > 0) Gfx_Blend(c, px, py, Gfx_Mix(fg, knock, inFill), at);
            if (ab > 0) Gfx_Blend(c, px, py, Gfx_Mix(fg, 0xFFFFFF, inFill), ab);
        }
    ICON_END
    Canvas_Free(&m);
}

/* Sonido (S4b suave): bloque de altavoz que se abre en cono con una curva continua, y dos
 * arcos gruesos como los del Wi-Fi. lit = arcos encendidos; muted = aspa en lugar de arcos.
 * El contorno es el trazado del lienzo, muestreado. */
static const V2 kSpeaker[] = {
    { 1.200f, 10.600f }, { 1.246f, 10.132f }, { 1.383f, 9.682f }, { 1.604f, 9.267f },
    { 1.903f, 8.903f }, { 2.267f, 8.604f }, { 2.682f, 8.383f }, { 3.132f, 8.246f },
    { 3.600f, 8.200f }, { 4.600f, 8.200f }, { 5.051f, 8.156f }, { 5.464f, 8.034f },
    { 5.845f, 7.846f }, { 6.199f, 7.605f }, { 6.531f, 7.325f }, { 6.846f, 7.019f },
    { 7.150f, 6.700f }, { 7.448f, 6.381f }, { 7.745f, 6.075f }, { 8.046f, 5.795f },
    { 8.357f, 5.554f }, { 8.683f, 5.366f }, { 9.029f, 5.244f }, { 9.400f, 5.200f },
    { 9.400f, 5.200f }, { 9.630f, 5.246f }, { 9.824f, 5.376f }, { 9.954f, 5.570f },
    { 10.000f, 5.800f }, { 10.000f, 18.200f }, { 9.954f, 18.430f }, { 9.824f, 18.624f },
    { 9.630f, 18.754f }, { 9.400f, 18.800f }, { 9.029f, 18.756f }, { 8.683f, 18.634f },
    { 8.357f, 18.446f }, { 8.046f, 18.205f }, { 7.745f, 17.925f }, { 7.448f, 17.619f },
    { 7.150f, 17.300f }, { 6.846f, 16.981f }, { 6.531f, 16.675f }, { 6.199f, 16.395f },
    { 5.845f, 16.154f }, { 5.464f, 15.966f }, { 5.051f, 15.844f }, { 4.600f, 15.800f },
    { 3.600f, 15.800f }, { 3.600f, 15.800f }, { 3.132f, 15.754f }, { 2.682f, 15.617f },
    { 2.267f, 15.396f }, { 1.903f, 15.097f }, { 1.604f, 14.733f }, { 1.383f, 14.318f },
    { 1.246f, 13.868f }, { 1.200f, 13.400f }
};

/* lv = arcos encendidos (continuo, 0..2); mute = 0..1 cuánto se ve el silencio (animado: las
 * ondas se recogen hacia el altavoz y el aspa entra girando); rip = onda al cambiar el
 * volumen (las ondas se abren al subir y se encogen al bajar). */
static void DrawSound(Canvas *c, float cx, float cy, float size, float lv, float mute, float rip, DWORD fg)
{
    const float mc = max(0.0f, min(1.0f, mute)), xs = max(0.01f, mute), keep = 1.0f - mc;
    const float shrink = 1.0f - 0.45f * mc, rp = max(-1.6f, min(1.8f, rip));
    const float r1 = 4.6f * shrink + rp * 0.55f, r2 = 9.0f * shrink + rp;
    const float l1 = Lit(lv, 0) * keep, l2 = Lit(lv, 1) * keep;
    ICON_LOOP(cx, cy, size)
        float a = COV(SdPoly(q, kSpeaker, (int)(sizeof(kSpeaker) / sizeof(kSpeaker[0]))));
        if (mute > 0.01f) {
            const V2 x = Rot((V2){ (q.x - 17.2f) / xs + 17.2f, (q.y - 12.0f) / xs + 12.0f }, 17.2f, 12.0f, -90.0f * keep);
            a = max(a, COV((min(SdSeg(x, 14.8f, 9.6f, 19.6f, 14.4f), SdSeg(x, 19.6f, 9.6f, 14.8f, 14.4f)) - 1.1f) * xs) * mc);
        }
        if (keep > 0.01f) {
            a = max(a, COV(SdArc(q, 9.6f, 12, r1, -0.78540f, 0.78540f) - 1.3f) * l1);
            a = max(a, COV(SdArc(q, 9.6f, 12, r2, -0.78540f, 0.78540f) - 1.3f) * l2);
        }
        if (a > 0) Gfx_Blend(c, px, py, fg, a);
    ICON_END
}

/* Tamaño unificado: cada icono se recorta a su tinta real (medida en el lienzo) y se escala
 * a la misma altura óptica; la batería, más ancha, algo más baja, como en macOS. */
typedef struct { float x0, y0, x1, y1, opt; } Ink;
static const Ink kInkGear  = { 1.78f, 1.67f, 22.22f, 22.33f, 1.00f };
static const Ink kInkWifi  = { 5.21f, 6.81f, 17.35f, 19.80f, 0.98f };
static const Ink kInkBatt  = { 0.65f, 5.65f, 23.60f, 18.35f, 0.86f };
static const Ink kInkSound = { 1.20f, 4.34f, 19.90f, 19.66f, 1.00f };

/* Coloca el icono con su tinta terminando en `right`: devuelve el ancho de tinta y el
 * centro y tamaño de caja (24 unidades) con los que hay que dibujarlo. */
static float PlaceIcon(const Ink *k, float right, float midY, float h, float *cx, float *cy, float *size)
{
    const float u = h * k->opt / (k->y1 - k->y0), w = (k->x1 - k->x0) * u;
    *size = 24.0f * u;
    *cx = right - w * 0.5f - ((k->x0 + k->x1) * 0.5f - 12.0f) * u;
    *cy = midY - ((k->y0 + k->y1) * 0.5f - 12.0f) * u;
    return w;
}

/* Bandeja: un chevrón hacia arriba, con el mismo trazo que los demás iconos. */
static void DrawChevron(Canvas *c, float cx, float cy, float size, DWORD fg)
{
    ICON_LOOP(cx, cy, size)
        const float d = min(SdSeg(q, 6.5f, 14.5f, 12.0f, 9.0f), SdSeg(q, 12.0f, 9.0f, 17.5f, 14.5f)) - 1.35f;
        const float a = COV(d);
        if (a > 0) Gfx_Blend(c, px, py, fg, a);
    ICON_END
}
static const Ink kInkTray = { 5.15f, 7.65f, 18.85f, 15.85f, 0.62f };

static int WifiArcs(void)  { return B.wifi >= 60 ? 2 : B.wifi >= 30 ? 1 : 0; }
static float WifiLit(void) { return B.wifi < 0 ? 0.0f : 1.0f + WifiArcs(); }
static BOOL  VolMuted(void) { return B.volume >= 0 && (B.muted || B.volume <= 0.001f); }
static float VolLit(void)  { return VolMuted() ? 0.0f : B.volume < 0.5f ? 1.0f : 2.0f; }

/* El logo de OpenDock a una tinta: el contorno del cuadrado y el notch colgando de arriba. */
static void DrawLogo(Canvas *c, float x, float y, float g, DWORD rgb)
{
    const float th = max(1.3f, BS(2) * 0.75f), in = th * 0.5f;
    const float nw = g * 0.46f, nh = g * 0.36f;
    GdiFlush();
    for (int py = (int)y; py < (int)(y + g + 1); ++py)
        for (int px = (int)x; px < (int)(x + g + 1); ++px) {
            const float fx = px + 0.5f, fy = py + 0.5f;
            float k = Gfx_Cov(fabsf(Gfx_SdRRect(fx, fy, x + in, y + in, g - th, g - th, g * 0.2f)) - th * 0.5f);
            k = max(k, Gfx_Cov(Gfx_SdRRect(fx, fy, x + (g - nw) * 0.5f, y - nh, nw, nh * 2, nh * 0.5f)));
            if (k > 0) Gfx_Blend(c, px, py, rgb, k);
        }
}

/* ───────────────────────── Barra: dibujo ───────────────────────── */
static void FormatClock(wchar_t *out, int cap)
{
    SYSTEMTIME st;
    wchar_t date[48], time[32];
    GetLocalTime(&st);
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"ddd d MMM", date, 48, NULL);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, time, 32);
    for (wchar_t *p = date; *p; ++p) if (*p == L'.') { MoveMemory(p, p + 1, (lstrlenW(p + 1) + 1) * sizeof(wchar_t)); --p; }
    CharUpperBuffW(date, 1);
    wsprintfW(out, L"%s  %s", date, time);
    out[cap - 1] = 0;
}

static float BarScale(int id) { return B.bs[id] > 0.1f ? max(0.8f, min(1.3f, B.bs[id])) : 1.0f; }

static void BarKick(void)
{
    if (!B.hwnd) return;
    QueryPerformanceCounter(&B.blast);
    SetTimer(B.hwnd, TIMER_BANIM, 10, NULL);
}

static void Bump(int id, float v)
{
    if (B.bs[id] < 0.1f) B.bs[id] = 1.0f;
    B.bsv[id] = v > 0 ? max(B.bsv[id], v) : min(B.bsv[id], v);
}

/* Cada cambio de estado tiene su gesto: el altavoz tacha o suelta las ondas y las abre o
 * encoge con el volumen; el Wi-Fi enciende sus arcos en cascada; la batería se llena o
 * vacía con suavidad y el rayo entra con rebote. Venga de donde venga el cambio (teclas,
 * rueda, centro de control, Windows). */
static void BarSync(void)
{
    if (!B.hwnd) return;
    const BOOL mute = VolMuted(), wifi = B.wifi >= 0;
    if (!B.synced) {                     /* al arrancar: el estado tal cual, sin animar */
        B.synced = TRUE;
        B.am = mute ? 1.0f : 0.0f; B.vl = VolLit(); B.wl = WifiLit();
        B.bc = B.charging ? 1.0f : 0.0f; B.bf = (float)max(0, B.battery);
        B.sMute = mute; B.sVol = B.volume; B.sWifi = wifi; B.sCharge = B.charging; B.sBatt = B.battery;
        return;
    }
    BOOL kick = FALSE;
    if (mute != B.sMute) { Bump(BH_VOL, 2.6f); kick = TRUE; }
    else if (!mute && B.volume >= 0 && fabsf(B.volume - B.sVol) > 0.004f) {
        const BOOL up = B.volume > B.sVol;
        Bump(BH_VOL, up ? 1.6f : -1.1f);
        B.ripv = up ? max(B.ripv, 20.0f) : min(B.ripv, -15.0f);
        kick = TRUE;
    }
    if (wifi != B.sWifi) { Bump(BH_WIFI, wifi ? 2.2f : -1.6f); kick = TRUE; }
    if (B.charging != B.sCharge) { Bump(BH_BATT, B.charging ? 2.6f : -1.4f); kick = TRUE; }
    else if (!B.charging && B.battery >= 0 && B.sBatt > 20 && B.battery <= 20) { Bump(BH_BATT, -2.0f); kick = TRUE; }
    B.sMute = mute; B.sVol = B.volume; B.sWifi = wifi; B.sCharge = B.charging; B.sBatt = B.battery;
    if (kick || fabsf(B.vl - VolLit()) > 0.001f || fabsf(B.wl - WifiLit()) > 0.001f ||
        fabsf(B.bf - (float)max(0, B.battery)) > 0.01f) BarKick();
}

/* Al cambiar el volumen (rueda, centro de control) el altavoz da un "pop" y sus ondas se
 * mueven; en el tope (0 o 100 %) también, para que se note el gesto. */
void Bar_PulseVolume(BOOL up)
{
    if (!B.hwnd) return;
    const float before = B.sVol;
    const BOOL wasMute = B.sMute;
    BarSync();
    if (fabsf(B.volume - before) <= 0.004f && wasMute == B.sMute) {
        Bump(BH_VOL, up ? 1.6f : -1.1f);
        BarKick();
    }
}

static BOOL Spring(float *x, float *v, float tg, float k, float z, float dt, float eps)
{
    for (int i = 0; i < 2; ++i) {
        const float a = (tg - *x) * k - *v * 2.0f * sqrtf(k) * z;
        *v += a * dt * 0.5f;
        *x += *v * dt * 0.5f;
    }
    if (fabsf(*x - tg) < eps && fabsf(*v) < eps * 10.0f) { *x = tg; *v = 0; return FALSE; }
    return TRUE;
}

static void BarAnimate(void)
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    float dt = (float)(t.QuadPart - B.blast.QuadPart) / (float)f.QuadPart;
    B.blast = t;
    if (dt > 0.05f) dt = 0.05f;
    const float z = Pop_Zeta();
    BOOL moving = FALSE;
    for (int id = BH_VOL; id <= BH_TRAY; ++id) {
        if (id == BH_CLOCK) continue;
        if (B.bs[id] < 0.1f) B.bs[id] = 1.0f;
        const float tg = B.pressed == id && B.bhot == id ? 0.88f : B.bhot == id ? 1.16f : 1.0f;
        for (int k = 0; k < 2; ++k) {
            const float acc = (tg - B.bs[id]) * 520.0f - B.bsv[id] * 2.0f * sqrtf(520.0f) * z;
            B.bsv[id] += acc * dt * 0.5f; B.bs[id] += B.bsv[id] * dt * 0.5f;
        }
        if (fabsf(B.bs[id] - tg) < 0.001f && fabsf(B.bsv[id]) < 0.01f) { B.bs[id] = tg; B.bsv[id] = 0; }
        else moving = TRUE;
    }
    /* gestos de estado: el silencio y la carga con el rebote configurado; los niveles suaves */
    moving |= Spring(&B.am, &B.amv, VolMuted() ? 1.0f : 0.0f, 380.0f, z, dt, 0.002f);
    moving |= Spring(&B.vl, &B.vlv, VolLit(), 200.0f, max(z, 0.8f), dt, 0.002f);
    moving |= Spring(&B.rip, &B.ripv, 0.0f, 330.0f, 0.3f, dt, 0.004f);
    moving |= Spring(&B.wl, &B.wlv, WifiLit(), 70.0f, max(z, 0.85f), dt, 0.002f);
    moving |= Spring(&B.bc, &B.bcv, B.charging ? 1.0f : 0.0f, 360.0f, z, dt, 0.002f);
    moving |= Spring(&B.bf, &B.bfv, (float)max(0, B.battery), 40.0f, 1.0f, dt, 0.05f);
    InvalidateRect(B.hwnd, NULL, FALSE);
    if (!moving) KillTimer(B.hwnd, TIMER_BANIM);
}

static void PaintBar(HDC target)
{
    RECT rc;
    GetClientRect(B.hwnd, &rc);
    if (!B.cv.dc || B.cv.w != rc.right || B.cv.h != rc.bottom) {
        Canvas_Free(&B.cv);
        if (!Canvas_Init(&B.cv, rc.right, rc.bottom)) return;
    }
    Canvas *c = &B.cv;
    const BarLook *L = &B.look;
    const int H = rc.bottom, cy = H / 2;
    if (BarGlass() && (s_back.px || CaptureBarBack())) PaintGlass(c, L->bg);
    else Canvas_Clear(c, L->bg);
    ZeroMemory(B.hit, sizeof(B.hit));

    /* izquierda: icono de la app activa (o el logo, si no tiene) + su nombre */
    int x = BS(16);
    const int is = BS(13);
    if (!B.appIcon || !BlitAppIcon(c, B.appIcon, x, cy - is / 2, is)) {
        const float g = (float)BS(14);
        DrawLogo(c, (float)x + (is - g) * 0.5f, cy - g * 0.5f, g, L->fg);
    }
    SetRect(&B.hit[BH_LOGO], 0, 0, x + is + BS(6), H);
    x += is + BS(8);
    if (B.app[0]) {
        const int w = min(Gfx_TextWidth(B.fBold, B.app), BS(320));
        Gfx_Text(c, B.fBold, B.app, x, 0, w + 2, H, L->fg, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }

    /* derecha (de derecha a izquierda): centro de control, hora, batería, Wi-Fi, volumen */
    int r = rc.right - BS(16);
    const float ih = (float)BS(14), gap = (float)BS(16);    /* altura óptica y separación */
    float icx, icy, isz, iw;
    iw = PlaceIcon(&kInkGear, (float)r, (float)cy, ih, &icx, &icy, &isz);
    DrawGear(c, icx, icy, isz * BarScale(BH_CC), L->fg);
    SetRect(&B.hit[BH_CC], r - (int)iw - BS(8), 0, r + BS(8), H);
    r -= (int)iw + BS(18);

    wchar_t clock[96];
    FormatClock(clock, 96);
    const int tw = Gfx_TextWidth(B.fBold, clock);        /* en negrita: se lee de un vistazo */
    Gfx_Text(c, B.fBold, clock, r - tw, 0, tw + 2, H, L->fg, DT_SINGLELINE | DT_VCENTER);
    SetRect(&B.hit[BH_CLOCK], r - tw - BS(6), 0, r + BS(6), H);
    r -= tw + BS(18);

    if (B.battery >= 0) {       /* el porcentaje va dentro de la batería */
        iw = PlaceIcon(&kInkBatt, (float)r, (float)cy, ih, &icx, &icy, &isz);
        DrawBattery(c, icx, icy, isz * BarScale(BH_BATT), B.bf, B.battery, B.bc, g_cfg.battPct, L->fg, L->bg);
        SetRect(&B.hit[BH_BATT], r - (int)iw - BS(4), 0, r + BS(6), H);
        r -= (int)(iw + gap);
    }

    iw = PlaceIcon(&kInkWifi, (float)r, (float)cy, ih, &icx, &icy, &isz);
    DrawWifi(c, icx, icy, isz * BarScale(BH_WIFI), B.wl, L->fg);
    SetRect(&B.hit[BH_WIFI], r - (int)iw - BS(4), 0, r + BS(6), H);
    r -= (int)(iw + gap);

    if (B.volume >= 0) {
        iw = PlaceIcon(&kInkSound, (float)r, (float)cy, ih, &icx, &icy, &isz);
        DrawSound(c, icx, icy, isz * BarScale(BH_VOL), B.vl, B.am, B.rip, L->fg);
        SetRect(&B.hit[BH_VOL], r - (int)iw - BS(4), 0, r + BS(6), H);
        r -= (int)(iw + gap);
    }
    {   /* la bandeja de Windows: Tailscale, OneDrive y demás iconos de la barra de tareas */
        iw = PlaceIcon(&kInkTray, (float)r, (float)cy, ih, &icx, &icy, &isz);
        DrawChevron(c, icx, icy, isz * BarScale(BH_TRAY), L->fg2);
        SetRect(&B.hit[BH_TRAY], r - (int)iw - BS(6), 0, r + BS(6), H);
    }

    BitBlt(target, 0, 0, c->w, c->h, c->dc, 0, 0, SRCCOPY);
}

/* ───────────────────────── Centro de control ───────────────────────── */
enum { CH_NONE, CH_WIFI, CH_BT, CH_BATT, CH_DND, CH_NOTIF, CH_BRIGHT, CH_VOL, CH_VOLICON, CH_HIDEBAR, CH_SETTINGS, CH_COUNT };

static struct {
    HWND   hwnd;
    Canvas cv;
    HFONT  fTitle, fSub, fIcon, fIconBig;
    int    bt;                  /* Bluetooth: -1 sin radio · 0 apagado · 1 encendido */
    /* movimiento: escala de cada elemento (hover / pulsado), "pop" de los iconos de los
     * deslizadores y su relleno, todos con el rebote configurado */
    int    hot, press;
    float  hs[CH_COUNT], hsv[CH_COUNT];
    float  is[2], isv[2];       /* 0 pantalla · 1 sonido */
    float  fill[2], fillv[2];
    Canvas el, ico;             /* lienzos de paso para componer escalado */
    /* subpágina abierta cuando section == 1: 1 ajustes · 2 Wi-Fi · 3 Bluetooth */
    int    page, listH, lhot;
    Canvas lcv;
    RECT   lhit[24];
    int    lid[24], nl;
    HFONT  fHead;
    GUID   wguid;
    wchar_t btDev[64];          /* primer dispositivo conectado */
    RECT   hit[CH_COUNT];
    int    dragging;            /* CH_BRIGHT / CH_VOL mientras se arrastra */
    int    brightness;          /* -1 = no disponible */
    DWORD  closedAt;
    Pop    pop;                 /* abre/cierra con el rebote del notch */
    PopGlass glass;
    int    ox, oy;              /* origen del contenido en pantalla */
    /* dos secciones: 0 controles · 1 ajustes de OpenDock (panel.c alojado) */
    int    section, setH;
    float  secT, secV;          /* transición entre secciones (0..1) */
    float  hcur, hv;            /* alto animado del contenido */
    Canvas ccv;                 /* la sección de controles */
    LARGE_INTEGER last;
} C;

/* ── brillo por WMI, en un hilo propio (cada llamada tarda decenas de ms) ── */
static volatile LONG s_brightTarget = -1;
static HANDLE        s_brightEvent;
static volatile LONG s_brightCurrent = -1;

static BOOL WmiConnect(IWbemServices **svc)
{
    IWbemLocator *loc = NULL;
    *svc = NULL;
    if (FAILED(CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (void **)&loc)))
        return FALSE;
    BSTR ns = SysAllocString(L"ROOT\\WMI");
    HRESULT hr = IWbemLocator_ConnectServer(loc, ns, NULL, NULL, NULL, 0, NULL, NULL, svc);
    SysFreeString(ns);
    IWbemLocator_Release(loc);
    if (FAILED(hr)) return FALSE;
    CoSetProxyBlanket((IUnknown *)*svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    return TRUE;
}

static int WmiReadBrightness(IWbemServices *svc)
{
    IEnumWbemClassObject *en = NULL;
    BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(L"SELECT CurrentBrightness FROM WmiMonitorBrightness");
    int value = -1;
    if (SUCCEEDED(IWbemServices_ExecQuery(svc, lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &en))) {
        IWbemClassObject *obj = NULL;
        ULONG got = 0;
        if (SUCCEEDED(IEnumWbemClassObject_Next(en, 2000, 1, &obj, &got)) && got) {
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(IWbemClassObject_Get(obj, L"CurrentBrightness", 0, &v, NULL, NULL))) {
                if (v.vt == VT_UI1) value = v.bVal;
                else if (v.vt == VT_I4) value = v.lVal;
            }
            VariantClear(&v);
            IWbemClassObject_Release(obj);
        }
        IEnumWbemClassObject_Release(en);
    }
    SysFreeString(lang);
    SysFreeString(q);
    return value;
}

static void WmiSetBrightness(IWbemServices *svc, int value)
{
    IEnumWbemClassObject *en = NULL;
    BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(L"SELECT * FROM WmiMonitorBrightnessMethods");
    BSTR cls = SysAllocString(L"WmiMonitorBrightnessMethods"), method = SysAllocString(L"WmiSetBrightness");
    if (SUCCEEDED(IWbemServices_ExecQuery(svc, lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &en))) {
        IWbemClassObject *inst = NULL, *klass = NULL, *sig = NULL, *args = NULL;
        ULONG got = 0;
        if (SUCCEEDED(IEnumWbemClassObject_Next(en, 2000, 1, &inst, &got)) && got &&
            SUCCEEDED(IWbemServices_GetObject(svc, cls, 0, NULL, &klass, NULL)) &&
            SUCCEEDED(IWbemClassObject_GetMethod(klass, method, 0, &sig, NULL)) &&
            SUCCEEDED(IWbemClassObject_SpawnInstance(sig, 0, &args))) {
            VARIANT path, t, b;
            VariantInit(&path);
            VariantInit(&t);
            VariantInit(&b);
            t.vt = VT_I4;  t.lVal = 0;
            b.vt = VT_UI1; b.bVal = (BYTE)max(0, min(100, value));
            IWbemClassObject_Put(args, L"Timeout", 0, &t, 0);
            IWbemClassObject_Put(args, L"Brightness", 0, &b, 0);
            if (SUCCEEDED(IWbemClassObject_Get(inst, L"__PATH", 0, &path, NULL, NULL)) && path.vt == VT_BSTR)
                IWbemServices_ExecMethod(svc, path.bstrVal, method, 0, NULL, args, NULL, NULL);
            VariantClear(&path);
        }
        if (args)  IWbemClassObject_Release(args);
        if (sig)   IWbemClassObject_Release(sig);
        if (klass) IWbemClassObject_Release(klass);
        if (inst)  IWbemClassObject_Release(inst);
        IEnumWbemClassObject_Release(en);
    }
    SysFreeString(lang); SysFreeString(q); SysFreeString(cls); SysFreeString(method);
}

static DWORD WINAPI BrightnessWorker(LPVOID unused)
{
    (void)unused;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IWbemServices *svc = NULL;
    if (WmiConnect(&svc)) s_brightCurrent = WmiReadBrightness(svc);
    for (;;) {
        WaitForSingleObject(s_brightEvent, INFINITE);
        const LONG target = InterlockedExchange(&s_brightTarget, -1);
        if (!svc && !WmiConnect(&svc)) continue;
        if (target >= 0) WmiSetBrightness(svc, target);
        else s_brightCurrent = WmiReadBrightness(svc);
    }
    return 0;
}

static void Brightness_Request(int value)     /* value < 0: solo releer */
{
    if (!s_brightEvent) {
        s_brightEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
        HANDLE t = s_brightEvent ? CreateThread(NULL, 0, BrightnessWorker, NULL, 0, NULL) : NULL;
        if (t) CloseHandle(t);
    }
    if (value >= 0) InterlockedExchange(&s_brightTarget, value);
    if (s_brightEvent) SetEvent(s_brightEvent);
}

static int CS(int v) { return BS(v); }

static void OpenUri(LPCWSTR uri)
{
    if (App_ShellOpen(uri)) {
    }
}

/* ───────────────────────── Centro de control: diseño ─────────────────────────
 * Como el de macOS: tarjeta de conectividad (Wi-Fi y Bluetooth), fichas de No molestar y
 * notificaciones, batería, deslizadores con su valor y dos botones al pie. */
#define CC_PAD   14
#define CC_GAP   10
#define CC_TILE  58
#define CC_BATT  60
#define CC_SROW  54
#define CC_FOOT  34

static void ReadBluetooth(void)
{
    C.bt = -1;
    C.btDev[0] = 0;
    BLUETOOTH_FIND_RADIO_PARAMS fp = { sizeof(fp) };
    HANDLE radio = NULL;
    HBLUETOOTH_RADIO_FIND f = BluetoothFindFirstRadio(&fp, &radio);
    if (!f) return;
    BluetoothFindRadioClose(f);
    C.bt = BluetoothIsConnectable(radio) ? 1 : 0;
    if (C.bt == 1) {            /* sin búsqueda: solo los ya conectados */
        BLUETOOTH_DEVICE_SEARCH_PARAMS sp = { sizeof(sp) };
        sp.fReturnConnected = TRUE;
        sp.hRadio = radio;
        BLUETOOTH_DEVICE_INFO di = { sizeof(di) };
        HBLUETOOTH_DEVICE_FIND df = BluetoothFindFirstDevice(&sp, &di);
        if (df) {
            do { if (di.fConnected) { lstrcpynW(C.btDev, di.szName, 64); break; } } while (BluetoothFindNextDevice(df, &di));
            BluetoothFindDeviceClose(df);
        }
    }
    CloseHandle(radio);
}

static int CCHeight(void)
{
    const int sliders = (C.brightness >= 0 ? 2 : 1) * CC_SROW + 10;
    return CC_PAD + (2 * CC_TILE + CC_GAP) + CC_GAP + CC_BATT + CC_GAP + sliders + CC_GAP + CC_FOOT + CC_PAD;
}

/* Fondo del material común (lo comparten las dos secciones). */
static void CCBackground(Canvas *c)
{
    if (B.look.light)                    Canvas_Clear(c, 0xE5E5EA);
    else if (g_cfg.material == MAT_GLASS) Pop_PaintGlass(c, &C.glass, C.ox, C.oy, 0x1C1C1E, 0.58f);
    else                                  Canvas_Clear(c, g_cfg.material == MAT_OLED ? 0x000000 : 0x161618);
}

static DWORD CardColor(void) { return B.look.light ? 0xFFFFFF : g_cfg.material == MAT_OLED ? 0x1C1C1E : 0x2C2C2E; }

static void Card(Canvas *c, RECT r)
{
    Gfx_FillRRect(c, (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top), (float)CS(16),
                  CardColor(), 1.0f);
}

/* Botón redondo con icono: de color de acento cuando está activo. */
static void Bubble(Canvas *c, float cx, float cy, float d, BOOL on, int kind, LPCWSTR glyph)
{
    const BarLook *L = &B.look;
    Gfx_FillCircle(c, cx, cy, d * 0.5f, on ? L->tileOn : (L->light ? 0xE5E5EA : 0x3A3A3C), 1.0f);
    const DWORD ic = on ? 0xFFFFFF : L->fg;
    if (kind == 1) DrawWifi(c, cx, cy, d * 0.62f, WifiLit(), ic);
    else Gfx_Text(c, C.fIcon, glyph, (int)(cx - d * 0.5f), (int)(cy - d * 0.5f), (int)d, (int)d, ic, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void TwoLines(Canvas *c, int x, int y, int w, int h, LPCWSTR title, LPCWSTR sub)
{
    const BarLook *L = &B.look;
    Gfx_Text(c, C.fTitle, title, x, y + h / 2 - CS(17), w, CS(18), L->fg, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    Gfx_Text(c, C.fSub, sub, x, y + h / 2 + CS(1), w, CS(16), L->fg2, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}


/* Deslizador: etiqueta y valor arriba; pista gruesa con el icono dentro (que hace "pop"). */
static RECT SliderTrack(RECT r)
{
    RECT t = { r.left, r.top + CS(22) - CS(6), r.right, r.top + CS(22) + CS(26) + CS(6) };
    return t;
}

static void DrawSliderRow(Canvas *c, RECT r, int k)
{
    const BarLook *L = &B.look;
    const float v = max(0.0f, min(1.0f, C.fill[k]));
    const float real = k ? (B.muted ? 0.0f : max(0.0f, B.volume)) : C.brightness / 100.0f;
    LPCWSTR glyph = k ? (B.muted ? L"\xE74F" : L"\xE767") : L"\xE706";
    wchar_t val[16];
    wsprintfW(val, L"%d %%", (int)(max(0.0f, min(1.0f, real)) * 100.0f + 0.5f));
    Gfx_Text(c, C.fTitle, k ? L"Sonido" : L"Pantalla", r.left, r.top, r.right - r.left, CS(18), L->fg, DT_SINGLELINE | DT_VCENTER);
    Gfx_Text(c, C.fSub, val, r.left, r.top, r.right - r.left, CS(18), L->fg2, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
    const float x = (float)r.left, y = (float)(r.top + CS(22)), w = (float)(r.right - r.left), h = (float)CS(26);
    const DWORD fillc = L->light ? 0x1C1C1E : 0xFFFFFF, inkc = L->light ? 0xFFFFFF : 0x1C1C1E;
    Gfx_FillRRect(c, x, y, w, h, h * 0.5f, L->light ? 0xE5E5EA : 0x3A3A3C, 1.0f);
    Gfx_FillRRect(c, x, y, max(h, w * v), h, h * 0.5f, fillc, 1.0f);
    /* icono con su escala animada: solo su trazo (máscara), sin cuadro de fondo */
    const int ib = (int)h;
    if (C.ico.w != ib || C.ico.h != ib) { Canvas_Free(&C.ico); Canvas_Init(&C.ico, ib, ib); }
    if (C.ico.px) {
        Canvas_Clear(&C.ico, 0);
        Gfx_Text(&C.ico, C.fIcon, glyph, 0, 0, ib, ib, 0xFFFFFF, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        GdiFlush();
        const float sc = max(0.85f, min(1.25f, C.is[k])), icx = x + CS(4) + ib * 0.5f, icy = y + ib * 0.5f, half = ib * sc * 0.5f;
        for (int py = (int)(icy - half) - 1; py <= (int)(icy + half) + 1; ++py)
            for (int px = (int)(icx - half) - 1; px <= (int)(icx + half) + 1; ++px) {
                if (px < 0 || py < 0 || px >= c->w || py >= c->h) continue;
                const float sx = (px + 0.5f - icx) / sc + ib * 0.5f - 0.5f, sy = (py + 0.5f - icy) / sc + ib * 0.5f - 0.5f;
                const int ix = (int)floorf(sx), iy = (int)floorf(sy);
                if (ix < -1 || iy < -1 || ix >= ib || iy >= ib) continue;
                const float fx = sx - ix, fy = sy - iy;
                float a = 0;
                for (int q = 0; q < 4; ++q) {
                    const int qx = ix + (q & 1), qy = iy + (q >> 1);
                    if (qx < 0 || qy < 0 || qx >= ib || qy >= ib) continue;
                    const float wq = ((q & 1) ? fx : 1 - fx) * ((q >> 1) ? fy : 1 - fy);
                    a += ((C.ico.px[qy * ib + qx] >> 8) & 255) / 255.0f * wq;
                }
                /* sobre el relleno, color de tinta; fuera de él (relleno corto), el del texto */
                if (a > 0.004f) Gfx_Blend(c, px, py, px + 0.5f < x + max(h, w * v) ? inkc : L->fg, min(1.0f, a));
            }
    }
}

static void DrawConnRow(Canvas *c, RECT rr, int k)
{
    const float b0 = (float)CS(30), bd = b0 * C.hs[k ? CH_BT : CH_WIFI];
    const float cy = (rr.top + rr.bottom) * 0.5f, cx = rr.left + CS(8) + b0 * 0.5f;
    const int tx = (int)(cx + b0 * 0.5f) + CS(10), tw = rr.right - tx - CS(6);
    if (!k) {
        Bubble(c, cx, cy, bd, B.wifi >= 0, 1, NULL);
        TwoLines(c, tx, rr.top, tw, rr.bottom - rr.top, L"Wi\x2011" L"Fi", B.wifi >= 0 ? B.ssid : L"Sin conexión");
    } else {
        Bubble(c, cx, cy, bd, C.bt == 1, 0, L"\xE702");
        TwoLines(c, tx, rr.top, tw, rr.bottom - rr.top, L"Bluetooth",
                 C.bt < 0 ? L"No disponible" : C.bt == 0 ? L"Desactivado" : C.btDev[0] ? C.btDev : L"Activado");
    }
}

static void DrawTileCard(Canvas *c, RECT r, int k)
{
    wchar_t sub[64];
    const float bd = (float)CS(30), cy = (r.top + r.bottom) * 0.5f, cx = r.left + CS(12) + bd * 0.5f;
    const int tx = (int)(cx + bd * 0.5f) + CS(10), tw = r.right - tx - CS(10);
    Card(c, r);
    if (!k) {
        Bubble(c, cx, cy, bd, FALSE, 0, L"\xE708");
        TwoLines(c, tx, r.top, tw, r.bottom - r.top, L"No molestar", L"Ajustes");
    } else {
        wsprintfW(sub, g_cfg.mirror ? L"%d en el notch" : L"Desactivadas", Wn_Count());
        Bubble(c, cx, cy, bd, g_cfg.mirror && Wn_Count() > 0, 0, L"\xEA8F");
        TwoLines(c, tx, r.top, tw, r.bottom - r.top, L"Notificaciones", sub);
    }
}

static void DrawBattCard(Canvas *c, RECT r, int k)
{
    (void)k;
    const BarLook *L = &B.look;
    const float bw = (float)CS(40), cy = (r.top + r.bottom) * 0.5f;
    Card(c, r);
    DrawBattery(c, r.left + CS(14) + bw * 0.5f, cy, bw, (float)max(0, B.battery), max(0, B.battery), B.charging ? 1.0f : 0.0f,
                FALSE, L->fg, CardColor());
    Gfx_Text(c, C.fTitle, L"Batería", r.left + CS(14) + (int)bw + CS(12), r.top, CS(180), r.bottom - r.top, L->fg,
             DT_SINGLELINE | DT_VCENTER);
    if (B.battery >= 0) {
        wchar_t pct[16];
        wsprintfW(pct, L"%d %%", B.battery);
        Gfx_Text(c, C.fIconBig, pct, r.left, r.top, r.right - r.left - CS(16), r.bottom - r.top, L->fg, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
    }
}

static void DrawFootBtn(Canvas *c, RECT r, int k)
{
    const BarLook *L = &B.look;
    Gfx_FillRRect(c, (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top), (r.bottom - r.top) * 0.5f,
                  k ? L->tileOn : CardColor(), 1.0f);
    Gfx_Text(c, C.fSub, k ? L"Ajustes  \x203A" : L"Ocultar barra", r.left, r.top, r.right - r.left, r.bottom - r.top,
             k ? 0xFFFFFF : L->fg, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
}

/* Un elemento con su escala animada: se pinta en un lienzo de paso (con lo que haya debajo)
 * y se compone escalado sobre su sitio; en reposo se pinta directo. El color no cambia. */
static void Element(Canvas *dst, int id, RECT r, void (*draw)(Canvas *, RECT, int), int k)
{
    const float s = C.hs[id];
    if (fabsf(s - 1.0f) < 0.002f) { draw(dst, r, k); return; }
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w < 2 || h < 2) return;
    if (C.el.w != w || C.el.h != h) { Canvas_Free(&C.el); if (!Canvas_Init(&C.el, w, h)) { draw(dst, r, k); return; } }
    GdiFlush();
    for (int y = 0; y < h; ++y) {
        const int sy = r.top + y;
        if (sy < 0 || sy >= dst->h) continue;
        CopyMemory(&C.el.px[y * w], &dst->px[sy * dst->w + max(0, (int)r.left)], (SIZE_T)min(w, dst->w - max(0, (int)r.left)) * 4);
    }
    RECT lr = { 0, 0, w, h };
    draw(&C.el, lr, k);
    GdiFlush();
    Gfx_BlitScaled(dst, &C.el, (r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f, s);
}

static void PaintControls(void)
{
    RECT rc = { 0, 0, CS(CC_W), CS(CCHeight()) };
    if (!C.ccv.dc || C.ccv.w != rc.right || C.ccv.h != rc.bottom) {
        Canvas_Free(&C.ccv);
        if (!Canvas_Init(&C.ccv, rc.right, rc.bottom)) return;
    }
    Canvas *c = &C.ccv;
    ZeroMemory(C.hit, sizeof(C.hit));
    CCBackground(c);

    const int pad = CS(CC_PAD), gap = CS(CC_GAP), colW = (rc.right - 2 * pad - gap) / 2, th = CS(CC_TILE);
    int y = pad;
    RECT r;

    /* conectividad: una tarjeta con dos filas (cada fila crece por su cuenta) */
    SetRect(&r, pad, y, pad + colW, y + 2 * th + gap);
    Card(c, r);
    const int rowH = (r.bottom - r.top) / 2;
    for (int k = 0; k < 2; ++k) {
        RECT rr = { r.left + CS(4), r.top + k * rowH + CS(2), r.right - CS(4), r.top + (k + 1) * rowH - CS(2) };
        const int id = k ? CH_BT : CH_WIFI;
        DrawConnRow(c, rr, k);
        C.hit[id] = rr;
    }
    /* fichas */
    for (int k = 0; k < 2; ++k) {
        SetRect(&r, pad + colW + gap, y + k * (th + gap), rc.right - pad, y + k * (th + gap) + th);
        const int id = k ? CH_NOTIF : CH_DND;
        Element(c, id, r, DrawTileCard, k);
        C.hit[id] = r;
    }
    y += 2 * th + gap + gap;

    SetRect(&r, pad, y, rc.right - pad, y + CS(CC_BATT));
    Element(c, CH_BATT, r, DrawBattCard, 0);
    C.hit[CH_BATT] = r;
    y += CS(CC_BATT) + gap;

    /* deslizadores en una tarjeta */
    const int ns = C.brightness >= 0 ? 2 : 1;
    SetRect(&r, pad, y, rc.right - pad, y + ns * CS(CC_SROW) + CS(10));
    Card(c, r);
    int sy = r.top + CS(10);
    for (int k = C.brightness >= 0 ? 0 : 1; k < 2; ++k) {
        RECT sr = { r.left + CS(14), sy, r.right - CS(14), sy + CS(CC_SROW) - CS(4) };
        const int id = k ? CH_VOL : CH_BRIGHT;
        Element(c, id, sr, DrawSliderRow, k);
        C.hit[id] = SliderTrack(sr);
        if (k) { C.hit[CH_VOLICON] = C.hit[CH_VOL]; C.hit[CH_VOLICON].right = C.hit[CH_VOLICON].left + CS(32); }
        sy += CS(CC_SROW);
    }
    y = r.bottom + gap;

    /* pie */
    for (int k = 0; k < 2; ++k) {
        SetRect(&r, k ? pad + colW + gap : pad, y, k ? rc.right - pad : pad + colW, y + CS(CC_FOOT));
        const int id = k ? CH_SETTINGS : CH_HIDEBAR;
        Element(c, id, r, DrawFootBtn, k);
        C.hit[id] = r;
    }
}

/* ───────────────────────── Listas: Wi-Fi y Bluetooth ───────────────────────── */
typedef struct { wchar_t ssid[33], prof[256]; int q; BOOL secure, connected; } WNet;
typedef struct { wchar_t name[64]; BOOL connected; ULONG cod; } BDev;
#define MAX_NETS 9
#define MAX_DEVS 9
static WNet s_nets[MAX_NETS];
static int  s_nnets;
static BDev s_devs[MAX_DEVS];
static int  s_ndevs;
#define TIMER_WSCAN 7

static BOOL WifiIface(GUID *g)
{
    DWORD ver = 0;
    if (!B.wlan && WlanOpenHandle(2, NULL, &ver, &B.wlan) != ERROR_SUCCESS) { B.wlan = NULL; return FALSE; }
    PWLAN_INTERFACE_INFO_LIST list = NULL;
    if (WlanEnumInterfaces(B.wlan, NULL, &list) != ERROR_SUCCESS || !list) return FALSE;
    const BOOL ok = list->dwNumberOfItems > 0;
    if (ok) *g = list->InterfaceInfo[0].InterfaceGuid;
    WlanFreeMemory(list);
    return ok;
}

static void ReadNetworks(void)
{
    s_nnets = 0;
    if (!WifiIface(&C.wguid)) return;
    PWLAN_AVAILABLE_NETWORK_LIST nl = NULL;
    if (WlanGetAvailableNetworkList(B.wlan, &C.wguid, 0, NULL, &nl) != ERROR_SUCCESS || !nl) return;
    for (DWORD i = 0; i < nl->dwNumberOfItems; ++i) {
        const WLAN_AVAILABLE_NETWORK *n = &nl->Network[i];
        if (!n->dot11Ssid.uSSIDLength) continue;                 /* redes ocultas */
        wchar_t ssid[33];
        const int len = MultiByteToWideChar(CP_UTF8, 0, (const char *)n->dot11Ssid.ucSSID, (int)min(n->dot11Ssid.uSSIDLength, 32), ssid, 32);
        ssid[max(0, len)] = 0;
        int j = 0;
        while (j < s_nnets && lstrcmpW(s_nets[j].ssid, ssid)) ++j;     /* una fila por nombre */
        if (j == s_nnets) {
            if (s_nnets >= MAX_NETS) continue;
            ZeroMemory(&s_nets[j], sizeof(WNet));
            lstrcpyW(s_nets[j].ssid, ssid);
            s_nnets++;
        }
        WNet *w = &s_nets[j];
        w->q = max(w->q, (int)n->wlanSignalQuality);
        w->secure |= n->bSecurityEnabled;
        if (n->dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) w->connected = TRUE;
        if (n->strProfileName[0] && !w->prof[0]) lstrcpynW(w->prof, n->strProfileName, 256);
    }
    WlanFreeMemory(nl);
    /* la conectada primero, luego por señal */
    for (int i = 0; i < s_nnets; ++i)
        for (int j = i + 1; j < s_nnets; ++j)
            if (s_nets[j].connected > s_nets[i].connected ||
                (s_nets[j].connected == s_nets[i].connected && s_nets[j].q > s_nets[i].q)) {
                WNet t = s_nets[i]; s_nets[i] = s_nets[j]; s_nets[j] = t;
            }
}

static void ReadDevices(void)
{
    s_ndevs = 0;
    BLUETOOTH_DEVICE_SEARCH_PARAMS sp = { sizeof(sp) };
    sp.fReturnAuthenticated = sp.fReturnRemembered = sp.fReturnConnected = TRUE;   /* sin búsqueda activa */
    BLUETOOTH_DEVICE_INFO di = { sizeof(di) };
    HBLUETOOTH_DEVICE_FIND f = BluetoothFindFirstDevice(&sp, &di);
    if (!f) return;
    do {
        if (s_ndevs >= MAX_DEVS || !di.szName[0]) continue;
        BDev *d = &s_devs[s_ndevs++];
        lstrcpynW(d->name, di.szName, 64);
        d->connected = di.fConnected;
        d->cod = di.ulClassofDevice;
    } while (BluetoothFindNextDevice(f, &di));
    BluetoothFindDeviceClose(f);
    for (int i = 0; i < s_ndevs; ++i)               /* los conectados primero */
        for (int j = i + 1; j < s_ndevs; ++j)
            if (s_devs[j].connected > s_devs[i].connected) { BDev t = s_devs[i]; s_devs[i] = s_devs[j]; s_devs[j] = t; }
}

static LPCWSTR DevGlyph(ULONG cod)
{
    switch ((cod >> 8) & 0x1F) {
    case 0x04: return L"\xE7F6";        /* audio: auriculares */
    case 0x02: return L"\xE8EA";        /* teléfono */
    case 0x01: return L"\xE7F8";        /* equipo */
    case 0x05: return (cod & 0x40) ? L"\xE765" : L"\xE962";    /* teclado · ratón */
    }
    return L"\xE702";
}

enum { LH_BACK = 1, LH_MORE = 2, LH_ROW = 10 };
#define LIST_ROW 48

static int ListHeight(void)
{
    const int n = C.page == 2 ? s_nnets : s_ndevs;
    return CC_PAD + 34 + CC_GAP + max(1, n) * LIST_ROW + 8 + CC_GAP + CC_FOOT + CC_PAD;
}

static void AddL(RECT r, int id) { if (C.nl < 24) { C.lhit[C.nl] = r; C.lid[C.nl++] = id; } }

/* Página de lista: volver, título, filas en una tarjeta y un enlace a los ajustes. */
static const Canvas *PaintList(void)
{
    const int W = CS(CC_W), H = CS(ListHeight());
    if (!C.lcv.dc || C.lcv.w != W || C.lcv.h != H) {
        Canvas_Free(&C.lcv);
        if (!Canvas_Init(&C.lcv, W, H)) return NULL;
    }
    Canvas *c = &C.lcv;
    const BarLook *L = &B.look;
    const BOOL wifi = C.page == 2;
    C.nl = 0;
    CCBackground(c);
    const int pad = CS(CC_PAD), gap = CS(CC_GAP);

    /* cabecera */
    const float d = (float)CS(32);
    Gfx_FillCircle(c, pad + d * 0.5f, pad + d * 0.5f, d * 0.5f, CardColor(), 1.0f);
    Gfx_Text(c, C.fIcon, L"\xE76B", pad, pad, (int)d, (int)d, L->fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT br = { pad, pad, pad + (int)d, pad + (int)d };
    AddL(br, LH_BACK);
    Gfx_Text(c, C.fHead, wifi ? L"Wi\x2011" L"Fi" : L"Bluetooth", pad + (int)d + CS(12), pad, CS(200), (int)d, L->fg,
             DT_SINGLELINE | DT_VCENTER);
    int y = pad + CS(34) + gap;

    /* filas */
    const int n = wifi ? s_nnets : s_ndevs, rh = CS(LIST_ROW);
    RECT card = { pad, y, W - pad, y + max(1, n) * rh + CS(8) };
    Card(c, card);
    if (!n) {
        Gfx_Text(c, C.fSub, wifi ? L"Buscando redes\x2026" : (C.bt < 0 ? L"Sin Bluetooth" : L"No hay dispositivos emparejados"),
                 card.left, card.top, card.right - card.left, card.bottom - card.top, L->fg2, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
    for (int i = 0; i < n; ++i) {
        RECT rr = { card.left + CS(4), card.top + CS(4) + i * rh, card.right - CS(4), card.top + CS(4) + (i + 1) * rh };
        if (i) Gfx_FillRRect(c, (float)rr.left + CS(52), (float)rr.top, (float)(rr.right - rr.left - CS(60)), (float)max(1, CS(1)), 0,
                             L->light ? 0xE5E5EA : 0x3A3A3C, 1.0f);
        const float bd = (float)CS(30), cy = (rr.top + rr.bottom) * 0.5f, cx = rr.left + CS(8) + bd * 0.5f;
        BOOL on;
        LPCWSTR name, sub;
        wchar_t sb[48];
        if (wifi) {
            const WNet *w = &s_nets[i];
            on = w->connected;
            Gfx_FillCircle(c, cx, cy, bd * 0.5f, on ? L->tileOn : (L->light ? 0xE5E5EA : 0x3A3A3C), 1.0f);
            DrawWifi(c, cx, cy, bd * 0.62f, w->q >= 60 ? 3.0f : w->q >= 30 ? 2.0f : 1.0f, on ? 0xFFFFFF : L->fg);
            name = w->ssid;
            lstrcpyW(sb, on ? L"Conectado" : w->prof[0] ? L"Red conocida" : w->secure ? L"Protegida" : L"Abierta");
            sub = sb;
        } else {
            const BDev *dv = &s_devs[i];
            on = dv->connected;
            Gfx_FillCircle(c, cx, cy, bd * 0.5f, on ? L->tileOn : (L->light ? 0xE5E5EA : 0x3A3A3C), 1.0f);
            Gfx_Text(c, C.fIcon, DevGlyph(dv->cod), (int)(cx - bd * 0.5f), (int)(cy - bd * 0.5f), (int)bd, (int)bd,
                     on ? 0xFFFFFF : L->fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            name = dv->name;
            sub = on ? L"Conectado" : L"No conectado";
        }
        const int tx = (int)(cx + bd * 0.5f) + CS(12);
        TwoLines(c, tx, rr.top, rr.right - tx - CS(36), rr.bottom - rr.top, name, sub);
        if (wifi && s_nets[i].secure)       /* candado de las redes protegidas */
            Gfx_Text(c, C.fIcon, L"\xE72E", rr.right - CS(32), rr.top, CS(24), rr.bottom - rr.top, L->fg2,
                     DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        AddL(rr, LH_ROW + i);
    }
    y = card.bottom + gap;

    RECT mr = { pad, y, W - pad, y + CS(CC_FOOT) };
    Gfx_FillRRect(c, (float)mr.left, (float)mr.top, (float)(mr.right - mr.left), (float)(mr.bottom - mr.top),
                  (mr.bottom - mr.top) * 0.5f, CardColor(), 1.0f);
    Gfx_Text(c, C.fSub, wifi ? L"Configuración de Wi\x2011" L"Fi  \x203A" : L"Configuración de Bluetooth  \x203A",
             mr.left, mr.top, mr.right - mr.left, mr.bottom - mr.top, L->fg, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    AddL(mr, LH_MORE);
    return c;
}

static int ListHit(int x, int y)
{
    const POINT p = { x, y };
    for (int i = C.nl - 1; i >= 0; --i) if (PtInRect(&C.lhit[i], p)) return C.lid[i];
    return 0;
}

static void CCKick(void);
static void CC_Close(void);

static void CC_GoList(int page)
{
    C.page = page;
    C.lhot = 0;
    if (page == 2) {
        ReadNetworks();
        if (B.wlan) WlanScan(B.wlan, &C.wguid, NULL, NULL, NULL);     /* refresca en segundo plano */
        SetTimer(C.hwnd, TIMER_WSCAN, 1800, NULL);
    } else {
        ReadBluetooth();
        ReadDevices();
    }
    C.listH = CS(ListHeight());
    C.section = 1;
    C.hot = C.press = 0;
    C.dragging = 0;
    CCKick();
}

static void ListClick(int id)
{
    if (id == LH_BACK) { C.section = 0; C.hot = C.press = 0; KillTimer(C.hwnd, TIMER_WSCAN); CCKick(); return; }
    if (id == LH_MORE) { CC_Close(); OpenUri(C.page == 2 ? L"ms-settings:network-wifi" : L"ms-settings:bluetooth"); return; }
    const int i = id - LH_ROW;
    if (C.page == 2 && i >= 0 && i < s_nnets) {
        const WNet *w = &s_nets[i];
        if (w->connected) return;
        if (w->prof[0] && B.wlan) {         /* red conocida: conectar aquí mismo */
            WLAN_CONNECTION_PARAMETERS cp = { 0 };
            cp.wlanConnectionMode = wlan_connection_mode_profile;
            cp.strProfile = w->prof;
            cp.dot11BssType = dot11_BSS_type_any;
            WlanConnect(B.wlan, &C.wguid, &cp, NULL);
            SetTimer(C.hwnd, TIMER_WSCAN, 1500, NULL);     /* y se refresca el estado */
        } else {                            /* nueva: la contraseña se pide en Windows */
            CC_Close();
            OpenUri(L"ms-settings:network-wifi");
        }
    } else if (C.page == 3) {
        CC_Close();
        OpenUri(L"ms-settings:bluetooth");
    }
}

/* Copia src sobre dst desplazada dx px, con opacidad a. */
static void BlitFade(Canvas *dst, const Canvas *src, int dx, float a)
{
    GdiFlush();
    const int rows = min(dst->h, src->h);
    for (int y = 0; y < rows; ++y) {
        DWORD *d = &dst->px[y * dst->w];
        const DWORD *sr = &src->px[y * src->w];
        for (int x = max(0, dx); x < min(dst->w, src->w + dx); ++x)
            d[x] = a >= 0.999f ? sr[x - dx] : Gfx_Mix(d[x], sr[x - dx], a);
    }
}

/* Como BlitFade, pero además escalada (k) alrededor de (cx, cy) del origen, bilineal. */
static void BlitDepth(Canvas *dst, const Canvas *src, float dx, float k, float cx, float cy, float a)
{
    if (fabsf(k - 1.0f) < 0.002f) { BlitFade(dst, src, (int)lroundf(dx), a); return; }
    GdiFlush();
    const float inv = 1.0f / k;
    const int a8 = (int)(max(0.0f, min(1.0f, a)) * 256.0f + 0.5f);
    const int y0 = max(0, (int)floorf(cy - cy * k)), y1 = min(dst->h, (int)ceilf(cy + (src->h - cy) * k));
    const int x0 = max(0, (int)floorf(dx + cx - cx * k)), x1 = min(dst->w, (int)ceilf(dx + cx + (src->w - cx) * k));
    for (int y = y0; y < y1; ++y) {
        const float sy = (y + 0.5f - cy) * inv + cy - 0.5f;
        const int iy = (int)floorf(sy);
        if (iy < 0 || iy >= src->h - 1) continue;
        const int fy = (int)((sy - iy) * 256.0f);
        DWORD *d = &dst->px[y * dst->w];
        const DWORD *r0 = &src->px[iy * src->w], *r1 = r0 + src->w;
        for (int x = x0; x < x1; ++x) {
            const float sx = (x + 0.5f - dx - cx) * inv + cx - 0.5f;
            const int ix = (int)floorf(sx);
            if (ix < 0 || ix >= src->w - 1) continue;
            const int fx = (int)((sx - ix) * 256.0f);
            DWORD out = 0;
            for (int sh = 0; sh < 24; sh += 8) {        /* por canal: B, G, R */
                const int c00 = (r0[ix] >> sh) & 255, c01 = (r0[ix + 1] >> sh) & 255;
                const int c10 = (r1[ix] >> sh) & 255, c11 = (r1[ix + 1] >> sh) & 255;
                const int top = c00 * 256 + (c01 - c00) * fx, bot = c10 * 256 + (c11 - c10) * fx;
                const int v = (top * 256 + (bot - top) * fy) >> 16;
                const int bg = (d[x] >> sh) & 255;
                out |= (DWORD)((bg * (256 - a8) + v * a8) >> 8) << sh;
            }
            d[x] = out;
        }
    }
}

static float Smooth(float a, float b, float x)
{
    const float t = max(0.0f, min(1.0f, (x - a) / (b - a)));
    return t * t * (3.0f - 2.0f * t);
}

/* Compone la sección visible. Al entrar en una sección (ajustes, Wi-Fi, Bluetooth) los
 * controles se quedan atrás, se encogen un poco y se apagan, y la sección llega desde la
 * derecha y se asienta; al volver, el gesto al revés: los controles vuelven al frente. Las
 * dos capas no se mezclan a medias mucho tiempo: una se va antes de que la otra llegue. */
static void PaintCC(void)
{
    const int W = CS(CC_W), H = max(1, (int)(C.hcur + 0.5f));
    if (!C.cv.dc || C.cv.w != W || C.cv.h != H) {
        Canvas_Free(&C.cv);
        if (!Canvas_Init(&C.cv, W, H)) return;
    }
    CCBackground(&C.cv);
    const float t = max(0.0f, min(1.0f, C.secT));
    const float e = max(-0.04f, min(1.04f, C.secT));          /* posición: deja un leve asentamiento */
    if (t < 0.999f) {
        PaintControls();
        const float out = 1.0f - Smooth(0.0f, 0.6f, t);
        if (C.ccv.px && out > 0.004f)
            BlitDepth(&C.cv, &C.ccv, -e * W * 0.12f, 1.0f - 0.07f * t, W * 0.5f, (float)CS(40), out);
    }
    if (t > 0.001f) {
        const Canvas *sc = C.page == 1 ? Panel_RenderEmbedded() : PaintList();
        const float in = Smooth(0.18f, 0.85f, t);
        if (sc && in > 0.004f) BlitFade(&C.cv, sc, (int)lroundf((1.0f - e) * W * 0.34f), in);
    }
    Pop_Present(&C.pop, &C.cv);
}

static void CCSpring(float *x, float *v, float target, float dt, float k, float z)
{
    const float acc = (target - *x) * k - *v * 2.0f * sqrtf(k) * z;
    *v += acc * dt; *x += *v * dt;
}

/* Un paso de la transición entre secciones y del alto (con el rebote configurado). */
static BOOL CCAdvance(void)
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    float dt = (float)(t.QuadPart - C.last.QuadPart) / (float)f.QuadPart;
    C.last = t;
    if (dt > 0.05f) dt = 0.05f;
    const float tT = (float)C.section, tH = (float)(C.section ? (C.page == 1 ? C.setH : C.listH) : CS(CCHeight()));
    for (int k = 0; k < 2; ++k) {
        /* contenido y alto con el mismo pulso, para que la ventana y la sección lleguen juntas */
        CCSpring(&C.secT, &C.secV, tT, dt * 0.5f, 210.0f, 0.84f);
        CCSpring(&C.hcur, &C.hv, tH, dt * 0.5f, 210.0f, min(1.0f, max(0.72f, Pop_Zeta() + 0.25f)));
    }
    /* escala de los elementos: crecen al señalarlos, se hunden al pulsarlos, con rebote */
    BOOL moving = FALSE;
    const float z = Pop_Zeta();
    for (int id = 1; id < CH_COUNT; ++id) {
        const BOOL btn = id == CH_HIDEBAR || id == CH_SETTINGS, row = id == CH_WIFI || id == CH_BT;
        const BOOL slider = id == CH_VOL || id == CH_BRIGHT;
        float tg = 1.0f;
        if (C.dragging == id)  tg = 1.02f;
        else if (C.press == id) tg = 0.97f;
        else if (C.hot == id)   tg = btn ? 1.03f : row ? 1.12f : slider ? 1.012f : 1.02f;     /* filas: su icono */
        if (row && C.press == id) tg = 0.9f;
        const float b0 = C.hs[id];
        for (int k = 0; k < 2; ++k) CCSpring(&C.hs[id], &C.hsv[id], tg, dt * 0.5f, 420.0f, z);
        if (fabsf(C.hs[id] - tg) < 0.0006f && fabsf(C.hsv[id]) < 0.01f) { C.hs[id] = tg; C.hsv[id] = 0; }
        if (fabsf(C.hs[id] - b0) > 0.00005f) moving = TRUE;
    }
    /* deslizadores: el relleno sigue al valor y el icono vuelve de su "pop" */
    for (int k = 0; k < 2; ++k) {
        const float real = k ? (B.muted ? 0.0f : max(0.0f, B.volume)) : max(0, C.brightness) / 100.0f;
        const float f0 = C.fill[k], i0 = C.is[k];
        for (int j = 0; j < 2; ++j) {
            CCSpring(&C.fill[k], &C.fillv[k], real, dt * 0.5f, 320.0f, max(0.55f, z));
            CCSpring(&C.is[k], &C.isv[k], 1.0f, dt * 0.5f, 380.0f, z);
        }
        if (fabsf(C.fill[k] - real) < 0.0005f && fabsf(C.fillv[k]) < 0.005f) { C.fill[k] = real; C.fillv[k] = 0; }
        if (fabsf(C.is[k] - 1.0f) < 0.0008f && fabsf(C.isv[k]) < 0.01f) { C.is[k] = 1.0f; C.isv[k] = 0; }
        if (fabsf(C.fill[k] - f0) > 0.00005f || fabsf(C.is[k] - i0) > 0.00005f) moving = TRUE;
    }
    const BOOL done = !moving && fabsf(C.secT - tT) < 0.002f && fabsf(C.secV) < 0.02f && fabsf(C.hcur - tH) < 0.4f && fabsf(C.hv) < 2.0f;
    if (done) {
        C.secT = tT; C.secV = 0; C.hcur = tH; C.hv = 0;
        if (!C.section && Panel_IsEmbedded()) Panel_Unembed();     /* de vuelta en controles */
    }
    Pop_Hold(&C.pop, !done);
    return moving || !done;      /* ¿cambió algo del contenido? */
}

static void CCKick(void)
{
    QueryPerformanceCounter(&C.last);
    Pop_Hold(&C.pop, TRUE);
}

static void CC_GoSettings(int tab)
{
    C.setH = Panel_Embed(C.hwnd, B.dpi, tab, &C.glass, C.ox, C.oy);
    C.page = 1;
    C.section = 1;
    C.dragging = 0;
    C.hot = C.press = 0;                 /* que nada se quede agrandado al volver */
    CCKick();
}

void CC_ShowControls(void)
{
    if (!C.hwnd) return;
    C.section = 0;
    C.hot = C.press = 0;
    CCKick();
}

void CC_SettingsResized(int h)
{
    C.setH = h;
    if (C.hwnd) CCKick();
}

BOOL Bar_IsOn(void) { return B.hwnd && !B.fullscreen; }


static int CCHitTest(int x, int y)
{
    POINT p = { x, y };
    for (int i = CH_COUNT - 1; i > CH_NONE; --i)
        if (PtInRect(&C.hit[i], p)) return i;
    return CH_NONE;
}

static void SliderValue(int which, int x)
{
    const RECT *r = &C.hit[which];
    const float v = max(0.0f, min(1.0f, (float)(x - r->left) / max(1, (int)(r->right - r->left))));
    const int k = which == CH_VOL ? 1 : 0;
    const float before = k ? (B.muted ? 0.0f : B.volume) : C.brightness / 100.0f;
    if (which == CH_VOL) SetVolume(v);
    else { C.brightness = (int)(v * 100.0f + 0.5f); Brightness_Request(C.brightness); }
    /* el icono responde al cambio: crece hacia donde va el valor y rebota */
    /* un empujón por cambio, no acumulativo: arrastrar no lo dispara */
    const float kick = v > before ? 2.2f : -1.4f;
    C.isv[k] = kick > 0 ? max(C.isv[k], kick) : min(C.isv[k], kick);
    if (k) Bar_PulseVolume(v > before);
    CCKick();
    InvalidateRect(C.hwnd, NULL, FALSE);
    if (B.hwnd) InvalidateRect(B.hwnd, NULL, FALSE);
}

static void CC_Close(void) { if (C.hwnd) Pop_Close(&C.pop); }
void CC_CloseAll(void) { CC_Close(); }

static LRESULT CALLBACK CCProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEWHEEL && m != WM_MOUSEHWHEEL) l = Pop_Mouse(&C.pop, l);
    /* en la sección de ajustes, ratón, teclado y temporizadores son del panel alojado */
    if (Panel_IsEmbedded() && ((m == WM_TIMER && Panel_IsTimer(w)) ||
        (C.section == 1 && C.page == 1 && ((m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || m == WM_MOUSELEAVE || m == WM_KEYDOWN || m == WM_CAPTURECHANGED))))
        return Panel_HostInput(h, m, w, l);
    if (C.section == 1 && C.page >= 2) {             /* páginas de Wi-Fi y Bluetooth */
        switch (m) {
        case WM_MOUSEMOVE: {
            const int hv = ListHit((short)LOWORD(l), (short)HIWORD(l));
            if (hv != C.lhot) {
                C.lhot = hv;
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
                TrackMouseEvent(&tme);
                InvalidateRect(h, NULL, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE: C.lhot = 0; InvalidateRect(h, NULL, FALSE); return 0;
        case WM_LBUTTONDOWN: return 0;
        case WM_LBUTTONUP: ListClick(ListHit((short)LOWORD(l), (short)HIWORD(l))); return 0;
        case WM_KEYDOWN: if (w == VK_ESCAPE) ListClick(LH_BACK); return 0;
        case WM_MOUSEWHEEL: return 0;
        }
    }
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        PaintCC();
        EndPaint(h, &ps);
        return 0;
    }
    case WM_NCHITTEST: return Pop_NcHit(&C.pop, l);
    case WM_TIMER:
        if (w == TIMER_WSCAN) {
            KillTimer(h, TIMER_WSCAN);
            if (C.section == 1 && C.page == 2) {
                ReadNetworks();
                ReadWifi();
                BarSync();
                C.listH = CS(ListHeight());
                CCKick();
            }
        }
        return 0;
    case WM_POPFRAME:
        Pop_Step(&C.pop);
        if (!C.hwnd) return 0;              /* se terminó de cerrar */
        /* si solo se anima la ventana (abrir, cerrar), el contenido es el mismo: no se repinta */
        if (CCAdvance() || !C.cv.px) PaintCC();
        else Pop_Present(&C.pop, &C.cv);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) CC_Close();
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) PostMessageW(h, WM_CLOSE, 0, 0);
        return 0;
    case WM_LBUTTONDOWN: {
        const int hit = CCHitTest((short)LOWORD(l), (short)HIWORD(l));
        C.press = hit == CH_VOLICON ? CH_VOL : hit;
        CCKick();
        if (hit == CH_VOL || hit == CH_BRIGHT) {
            C.dragging = hit;
            SetCapture(h);
            SliderValue(hit, (short)LOWORD(l));
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (C.dragging) { SliderValue(C.dragging, (short)LOWORD(l)); return 0; }
        int hv = CCHitTest((short)LOWORD(l), (short)HIWORD(l));
        if (hv == CH_VOLICON) hv = CH_VOL;
        if (hv != C.hot) {
            C.hot = hv;
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            CCKick();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        C.hot = C.press = 0;
        CCKick();
        return 0;
    case WM_LBUTTONUP: {
        C.press = 0;
        CCKick();
        if (C.dragging) { C.dragging = 0; ReleaseCapture(); return 0; }
        switch (CCHitTest((short)LOWORD(l), (short)HIWORD(l))) {
        case CH_WIFI:     CC_GoList(2); break;             /* la lista, en la misma ventana */
        case CH_BT:       CC_GoList(3); break;
        case CH_BATT:     CC_Close(); OpenUri(L"ms-settings:batterysaver"); break;
        case CH_DND:      CC_Close(); OpenUri(L"ms-settings:notifications"); break;
        case CH_NOTIF:    CC_Close(); Notch_OpenCenter(); break;
        case CH_VOLICON:  ToggleMute(); C.isv[1] = max(C.isv[1], 2.4f); CCKick(); Bar_PulseVolume(TRUE); break;
        case CH_HIDEBAR:  CC_Close(); App_SetMenuBar(FALSE, TRUE); break;
        case CH_SETTINGS: CC_GoSettings(0); break;
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        C.dragging = 0;
        return 0;
    case WM_MOUSEWHEEL: {
        const BOOL up = GET_WHEEL_DELTA_WPARAM(w) > 0;
        SetVolume((B.muted ? 0 : B.volume) + (up ? 0.05f : -0.05f));
        C.isv[1] = up ? max(C.isv[1], 2.2f) : min(C.isv[1], -1.4f);
        CCKick();
        Bar_PulseVolume(up);
        return 0;
    }
    case WM_CLOSE:
        CC_Close();
        return 0;
    case WM_DESTROY: {
        Panel_Unembed();
        Pop_Destroyed(&C.pop);
        Pop_FreeGlass(&C.glass);
        Canvas_Free(&C.cv);
        Canvas_Free(&C.ccv);
        Canvas_Free(&C.el);
        Canvas_Free(&C.ico);
        Canvas_Free(&C.lcv);
        KillTimer(h, TIMER_WSCAN);
        C.page = 0;
        if (C.fHead) { DeleteObject(C.fHead); C.fHead = NULL; }
        C.section = 0;
        HFONT *all[] = { &C.fTitle, &C.fSub, &C.fIcon, &C.fIconBig };
        for (int i = 0; i < 4; ++i) if (*all[i]) { DeleteObject(*all[i]); *all[i] = NULL; }
        C.hwnd = NULL;
        C.closedAt = GetTickCount();
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

/* Abre el centro de control. Estilo "Notch": pegado al borde de la barra, con hombros, se
 * despliega hacia abajo como el panel de notificaciones del notch. "Flotante": tarjeta
 * separada que crece desde el engrane. Las dos con el rebote configurado. */
static void CC_Open(int section, int tab)
{
    if (C.hwnd) DestroyWindow(C.hwnd);
    ReadVolume();
    ReadWifi();
    ReadBattery();
    ReadBluetooth();
    BarSync();
    C.brightness = s_brightCurrent;
    C.hot = C.press = 0;
    for (int i = 0; i < CH_COUNT; ++i) { C.hs[i] = 1.0f; C.hsv[i] = 0; }
    C.fill[0] = max(0, C.brightness) / 100.0f; C.fill[1] = B.muted ? 0.0f : max(0.0f, B.volume);
    C.fillv[0] = C.fillv[1] = 0; C.is[0] = C.is[1] = 1.0f; C.isv[0] = C.isv[1] = 0;
    Brightness_Request(-1);                             /* refresca para la próxima vez */

    LPCWSTR ui = Gfx_UiFace(), ic = Gfx_IconFace();
    C.fTitle = Gfx_Font(ui, CS(13), FW_SEMIBOLD, CLEARTYPE_QUALITY);
    C.fSub   = Gfx_Font(ui, CS(12), FW_NORMAL,   CLEARTYPE_QUALITY);
    C.fIcon  = Gfx_Font(ic, CS(14), FW_NORMAL,   CLEARTYPE_QUALITY);
    C.fIconBig = Gfx_Font(ui, CS(20), FW_BOLD, CLEARTYPE_QUALITY);     /* el porcentaje de batería */
    if (C.fHead) DeleteObject(C.fHead);
    C.fHead = Gfx_Font(ui, CS(17), FW_BOLD, CLEARTYPE_QUALITY);
    const BOOL attached = !g_cfg.floating;
    const int w = CS(CC_W), h = CS(CCHeight());
    C.ox = B.mon.right - w - (attached ? CS(14) : CS(8));
    C.oy = B.mon.top + B.h + (attached ? 0 : CS(8));
    /* vidrio: lo que hay detrás en toda la columna (los ajustes son más altos) */
    if (g_cfg.material == MAT_GLASS) Pop_CaptureGlass(&C.glass, C.ox, C.oy, w, B.mon.bottom - C.oy, CS(28));
    C.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, CC_CLASS, L"Centro de control", WS_POPUP,
                             C.ox, C.oy, w, h, NULL, NULL, g_inst, NULL);
    if (!C.hwnd) return;
    if (g_cfg.hideCapture) SetWindowDisplayAffinity(C.hwnd, WDA_EXCLUDEFROMCAPTURE);
    if (attached) {
        /* como el panel de notificaciones del notch: nace de una pastilla bajo el engrane
         * (margen amplio para que el rebote no se recorte) */
        Pop_Open(&C.pop, C.hwnd, CS(44), (float)CS(24), 0.95f, 0.0f);
        Pop_SetAttached(&C.pop, (float)CS(8), (float)CS(150));
    } else {
        Pop_Open(&C.pop, C.hwnd, CS(18), (float)CS(18), 0.94f, 0.0f);  /* flotante: sale del engrane */
    }
    C.section = section;
    C.page = section ? 1 : 0;
    C.secT = (float)section; C.secV = 0;
    if (section) C.setH = Panel_Embed(C.hwnd, B.dpi, tab, &C.glass, C.ox, C.oy);
    C.hcur = (float)(section ? C.setH : h); C.hv = 0;
    QueryPerformanceCounter(&C.last);
    Pop_SetBounds(&C.pop, C.ox, C.oy, w, (int)C.hcur);
    PaintCC();
    ShowWindow(C.hwnd, SW_SHOWNA);
    SetForegroundWindow(C.hwnd);
}

static void CC_Toggle(void)
{
    if (C.hwnd && !C.pop.closing) { CC_Close(); return; }
    if (!C.hwnd && GetTickCount() - C.closedAt < 300) return;     /* el clic en la barra ya lo cerró */
    CC_Open(0, 0);
}

/* Ajustes de OpenDock: una sección del centro de control, no otra ventana. */
void CC_OpenSettings(int tab)
{
    if (C.hwnd && !C.pop.closing) { CC_GoSettings(tab); SetForegroundWindow(C.hwnd); return; }
    CC_Open(1, tab);
}

/* ───────────────────────── Bandeja ─────────────────────────
 * Los iconos de la bandeja de Windows (Tailscale, OneDrive…) en un panel propio bajo el
 * chevrón, con el material de la barra y el muelle del centro de control. Los iconos los
 * recoge tray.c; un clic aquí es un clic en el icono (izquierdo o derecho, con su menú). */
#define TP_CLASS L"OpenDock.TrayMenu"
#define TP_MAX   32
#define TP_COLS  6

static struct {
    HWND     hwnd;
    Pop      pop;
    PopGlass glass;
    Canvas   cv;
    TrayItem it[TP_MAX];
    Canvas   ico[TP_MAX];           /* cada icono rasterizado: color premultiplicado y alfa */
    float    hs[TP_MAX], hsv[TP_MAX];
    int      n, hot, press, ox, oy, w, h;
    HFONT    fTip;
    LARGE_INTEGER last;
    DWORD    closedAt;
} TP;

static int TPCell(void) { return CS(42); }
static int TPCols(void) { return max(1, min(TP_COLS, TP.n)); }
static int TPWidth(void) { return max(CS(210), TPCols() * TPCell() + 2 * CS(12)); }
static int TPHeight(void) { return CS(10) + ((TP.n + TPCols() - 1) / TPCols()) * TPCell() + CS(30); }

static void TPCellCenter(int i, float *cx, float *cy)
{
    const int cols = TPCols(), cell = TPCell(), x0 = (TP.w - cols * cell) / 2;
    *cx = x0 + (i % cols) * cell + cell * 0.5f;
    *cy = CS(10) + (i / cols) * cell + cell * 0.5f;
}

static int TPHit(int x, int y)
{
    for (int i = 0; i < TP.n; ++i) {
        float cx, cy;
        TPCellCenter(i, &cx, &cy);
        if (fabsf(x - cx) <= TPCell() * 0.5f && fabsf(y - cy) <= TPCell() * 0.5f) return i;
    }
    return -1;
}

/* Un icono de Windows a píxeles con alfa: se dibuja sobre negro y sobre blanco, y la
 * diferencia da la transparencia (vale para iconos con alfa y para los de máscara). */
static void RasterIcon(Canvas *out, HICON ic, int px)
{
    Canvas b = { 0 }, w = { 0 };
    if (!ic || !Canvas_Init(&b, px, px) || !Canvas_Init(&w, px, px) || !Canvas_Init(out, px, px)) {
        Canvas_Free(&b); Canvas_Free(&w); return;
    }
    Canvas_Clear(&b, 0x000000);
    Canvas_Clear(&w, 0xFFFFFF);
    DrawIconEx(b.dc, 0, 0, ic, px, px, 0, NULL, DI_NORMAL);
    DrawIconEx(w.dc, 0, 0, ic, px, px, 0, NULL, DI_NORMAL);
    GdiFlush();
    for (int i = 0; i < px * px; ++i) {
        const DWORD bb = b.px[i], ww = w.px[i];
        int d = 0;
        for (int sh = 0; sh < 24; sh += 8) d = max(d, (int)((ww >> sh) & 255) - (int)((bb >> sh) & 255));
        const DWORD a = (DWORD)max(0, min(255, 255 - d));
        out->px[i] = (a << 24) | (bb & 0xFFFFFF);
    }
    Canvas_Free(&b);
    Canvas_Free(&w);
}

/* El icono escalado k alrededor de (cx, cy), bilineal, sobre lo que haya. */
static void TPBlitIcon(Canvas *dst, const Canvas *ic, float cx, float cy, float k)
{
    if (!ic->px) return;
    const float half = ic->w * k * 0.5f, inv = 1.0f / k;
    for (int y = (int)(cy - half) - 1; y <= (int)(cy + half) + 1; ++y)
        for (int x = (int)(cx - half) - 1; x <= (int)(cx + half) + 1; ++x) {
            if (x < 0 || y < 0 || x >= dst->w || y >= dst->h) continue;
            const float sx = (x + 0.5f - cx) * inv + ic->w * 0.5f - 0.5f, sy = (y + 0.5f - cy) * inv + ic->h * 0.5f - 0.5f;
            const int ix = (int)floorf(sx), iy = (int)floorf(sy);
            const float fx = sx - ix, fy = sy - iy;
            float acc[4] = { 0 };
            for (int q = 0; q < 4; ++q) {
                const int qx = ix + (q & 1), qy = iy + (q >> 1);
                if (qx < 0 || qy < 0 || qx >= ic->w || qy >= ic->h) continue;
                const float wq = ((q & 1) ? fx : 1 - fx) * ((q >> 1) ? fy : 1 - fy);
                const DWORD p = ic->px[qy * ic->w + qx];
                acc[0] += (p & 255) * wq; acc[1] += ((p >> 8) & 255) * wq; acc[2] += ((p >> 16) & 255) * wq; acc[3] += (p >> 24) * wq;
            }
            if (acc[3] < 0.5f) continue;
            DWORD *d = &dst->px[y * dst->w + x];
            const float ia = 1.0f - acc[3] / 255.0f;
            DWORD o = 0;
            for (int ch = 0; ch < 3; ++ch) {
                const float v = ((*d >> (ch * 8)) & 255) * ia + acc[ch];
                o |= (DWORD)max(0, min(255, (int)(v + 0.5f))) << (ch * 8);
            }
            *d = o;
        }
}

static void TPFree(void)
{
    for (int i = 0; i < TP.n; ++i) {
        if (TP.it[i].icon) DestroyIcon(TP.it[i].icon);
        Canvas_Free(&TP.ico[i]);
    }
    TP.n = 0;
}

static void TPLoad(void)
{
    TPFree();
    TP.n = Tray_Snapshot(TP.it, TP_MAX);
    for (int i = 0; i < TP.n; ++i) {
        RasterIcon(&TP.ico[i], TP.it[i].icon, CS(20));
        TP.hs[i] = 1.0f; TP.hsv[i] = 0;
    }
    if (TP.hot >= TP.n) TP.hot = -1;
}

static void TPPaint(void)
{
    if (!TP.cv.dc || TP.cv.w != TP.w || TP.cv.h != TP.h) {
        Canvas_Free(&TP.cv);
        if (!Canvas_Init(&TP.cv, TP.w, TP.h)) return;
    }
    Canvas *c = &TP.cv;
    const BarLook *L = &B.look;
    if (L->light)                         Canvas_Clear(c, 0xE5E5EA);
    else if (g_cfg.material == MAT_GLASS) Pop_PaintGlass(c, &TP.glass, TP.ox, TP.oy, 0x1C1C1E, 0.58f);
    else                                  Canvas_Clear(c, g_cfg.material == MAT_OLED ? 0x000000 : 0x161618);
    for (int i = 0; i < TP.n; ++i) {
        float cx, cy;
        TPCellCenter(i, &cx, &cy);
        TPBlitIcon(c, &TP.ico[i], cx, cy, max(0.8f, min(1.3f, TP.hs[i])));
    }
    /* abajo, el nombre del icono señalado (la primera línea de su descripción) */
    wchar_t tip[128];
    if (TP.hot >= 0 && TP.it[TP.hot].tip[0]) {
        lstrcpynW(tip, TP.it[TP.hot].tip, 128);
        for (wchar_t *p = tip; *p; ++p) if (*p == L'\r' || *p == L'\n') { *p = 0; break; }
    } else lstrcpynW(tip, L"Bandeja del sistema", 128);
    Gfx_Text(c, TP.fTip, tip, CS(12), TP.h - CS(30), TP.w - CS(24), CS(24), TP.hot >= 0 ? L->fg : L->fg2,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    Pop_Present(&TP.pop, c);
}

static void TPKick(void)
{
    QueryPerformanceCounter(&TP.last);
    Pop_Hold(&TP.pop, TRUE);
}

static BOOL TPAdvance(void)
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    float dt = (float)(t.QuadPart - TP.last.QuadPart) / (float)f.QuadPart;
    TP.last = t;
    if (dt > 0.05f) dt = 0.05f;
    const float z = Pop_Zeta();
    BOOL moving = FALSE;
    for (int i = 0; i < TP.n; ++i) {
        const float tg = TP.press == i ? 0.88f : TP.hot == i ? 1.18f : 1.0f, b0 = TP.hs[i];
        for (int k = 0; k < 2; ++k) CCSpring(&TP.hs[i], &TP.hsv[i], tg, dt * 0.5f, 420.0f, z);
        if (fabsf(TP.hs[i] - tg) < 0.0006f && fabsf(TP.hsv[i]) < 0.01f) { TP.hs[i] = tg; TP.hsv[i] = 0; }
        if (fabsf(TP.hs[i] - b0) > 0.00005f) moving = TRUE;
    }
    Pop_Hold(&TP.pop, moving);
    return moving;
}

static void TP_Close(void) { if (TP.hwnd) Pop_Close(&TP.pop); }

static LRESULT CALLBACK TPProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEWHEEL && m != WM_MOUSEHWHEEL) l = Pop_Mouse(&TP.pop, l);
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        TPPaint();
        EndPaint(h, &ps);
        return 0;
    }
    case WM_NCHITTEST: return Pop_NcHit(&TP.pop, l);
    case WM_POPFRAME:
        Pop_Step(&TP.pop);
        if (!TP.hwnd) return 0;
        if (TPAdvance() || !TP.cv.px) TPPaint();
        else Pop_Present(&TP.pop, &TP.cv);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) TP_Close();
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) TP_Close();
        return 0;
    case WM_MOUSEMOVE: {
        const int hv = TPHit((short)LOWORD(l), (short)HIWORD(l));
        if (hv != TP.hot) {
            TP.hot = hv;
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            TPKick();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        TP.hot = TP.press = -1;
        TPKick();
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN:
        TP.press = TPHit((short)LOWORD(l), (short)HIWORD(l));
        TPKick();
        return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: {
        const int hit = TPHit((short)LOWORD(l), (short)HIWORD(l)), was = TP.press;
        TP.press = -1;
        TPKick();
        if (hit < 0 || hit != was) return 0;
        const HWND app = TP.it[hit].hwnd;
        const UINT uid = TP.it[hit].uid;
        TP_Close();                         /* el panel se va y la app abre su menú o su ventana */
        Tray_Click(app, uid, m == WM_RBUTTONUP);
        return 0;
    }
    case WM_DESTROY:
        Pop_Destroyed(&TP.pop);
        Pop_FreeGlass(&TP.glass);
        Canvas_Free(&TP.cv);
        TPFree();
        if (TP.fTip) { DeleteObject(TP.fTip); TP.fTip = NULL; }
        TP.hwnd = NULL;
        TP.closedAt = GetTickCount();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* Abre el panel de la bandeja; FALSE si no hay iconos propios que enseñar. */
static BOOL TP_Open(void)
{
    if (TP.hwnd) DestroyWindow(TP.hwnd);
    TP.hot = TP.press = -1;
    TPLoad();
    if (!TP.n) return FALSE;
    TP.fTip = Gfx_Font(Gfx_UiFace(), CS(12), FW_SEMIBOLD, CLEARTYPE_QUALITY);
    const BOOL attached = !g_cfg.floating;
    TP.w = TPWidth(); TP.h = TPHeight();
    const RECT *hb = &B.hit[BH_TRAY];
    const int cx = B.mon.left + (hb->left + hb->right) / 2;
    TP.ox = max(B.mon.left + CS(8), min(B.mon.right - CS(8) - TP.w, cx - TP.w / 2));
    TP.oy = B.mon.top + B.h + (attached ? 0 : CS(8));
    if (g_cfg.material == MAT_GLASS) Pop_CaptureGlass(&TP.glass, TP.ox, TP.oy, TP.w, TP.h + CS(80), CS(28));
    TP.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, TP_CLASS, L"Bandeja", WS_POPUP,
                              TP.ox, TP.oy, TP.w, TP.h, NULL, NULL, g_inst, NULL);
    if (!TP.hwnd) { TPFree(); return FALSE; }
    if (g_cfg.hideCapture) SetWindowDisplayAffinity(TP.hwnd, WDA_EXCLUDEFROMCAPTURE);
    const float ax = max(0.05f, min(0.95f, (cx - TP.ox) / (float)TP.w));
    if (attached) {
        Pop_Open(&TP.pop, TP.hwnd, CS(44), (float)CS(20), ax, 0.0f);
        Pop_SetAttached(&TP.pop, (float)CS(8), (float)CS(70));
    } else {
        Pop_Open(&TP.pop, TP.hwnd, CS(18), (float)CS(16), ax, 0.0f);
    }
    QueryPerformanceCounter(&TP.last);
    Pop_SetBounds(&TP.pop, TP.ox, TP.oy, TP.w, TP.h);
    TPPaint();
    ShowWindow(TP.hwnd, SW_SHOWNA);
    SetForegroundWindow(TP.hwnd);
    return TRUE;
}

/* Un icono cambió (Tailscale se conectó, una app se cerró…): el panel abierto se pone al día. */
static void TP_Refresh(void)
{
    if (!TP.hwnd || TP.pop.closing) return;
    TPLoad();
    if (!TP.n) { TP_Close(); return; }
    TP.w = TPWidth(); TP.h = TPHeight();
    const RECT *hb = &B.hit[BH_TRAY];
    const int cx = B.mon.left + (hb->left + hb->right) / 2;
    TP.ox = max(B.mon.left + CS(8), min(B.mon.right - CS(8) - TP.w, cx - TP.w / 2));
    Pop_SetBounds(&TP.pop, TP.ox, TP.oy, TP.w, TP.h);
    TPPaint();
}

/* ───────────────────────── Barra: ventana ───────────────────────── */
static void MakeBarFonts(void)
{
    HFONT *all[] = { &B.fBold, &B.fText, &B.fIcon };
    for (int i = 0; i < 3; ++i) if (*all[i]) DeleteObject(*all[i]);
    LPCWSTR ui = Gfx_UiFace();
    B.fBold = Gfx_Font(ui, BS(13), FW_BOLD, CLEARTYPE_QUALITY);
    B.fText = Gfx_Font(ui, BS(13), FW_NORMAL, CLEARTYPE_QUALITY);
    B.fIcon = Gfx_Font(Gfx_IconFace(), BS(14), FW_NORMAL, CLEARTYPE_QUALITY);
}

static void BarPosition(void)
{
    APPBARDATA abd = { sizeof(abd) };
    abd.hWnd = B.hwnd;
    abd.uEdge = ABE_TOP;
    abd.rc = B.mon;
    abd.rc.bottom = B.mon.top + B.h;
    SHAppBarMessage(ABM_QUERYPOS, &abd);
    abd.rc.bottom = abd.rc.top + B.h;
    SHAppBarMessage(ABM_SETPOS, &abd);
    SetWindowPos(B.hwnd, HWND_TOPMOST, abd.rc.left, abd.rc.top, abd.rc.right - abd.rc.left, B.h,
                 SWP_NOACTIVATE | (B.fullscreen ? 0 : SWP_SHOWWINDOW));
    InvalidateRect(B.hwnd, NULL, FALSE);
}

static void BarMeasure(void)
{
    HMONITOR m = MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(m, &mi);
    B.mon = mi.rcMonitor;
    B.dpi = MonitorDpi(m);
    B.h = BS(BAR_H);
    LoadBarLook();
    MakeBarFonts();
}

static void RefreshStatus(void)
{
    ReadBattery();
    ReadWifi();
    ReadVolume();
    ForegroundAppName(B.app, 64);
    ForegroundAppIcon(BS(13));
}

/* El reloj solo cambia una vez por minuto: se despierta justo al cambiar (con vidrio,
 * cada 5 s como mucho, para recomponer el fondo si algo se movió detrás). */
static void ArmClock(HWND h)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    UINT ms = (60 - st.wSecond) * 1000 - st.wMilliseconds + 30;
    if (BarGlass()) ms = min(ms, 5000u);
    SetTimer(h, TIMER_CLOCK, max(200u, ms), NULL);
}

/* El panel de iconos ocultos de Windows (Tailscale, OneDrive…) es una ventana de Explorer.
 * Se abre con Win+B y Entrar y, en cuanto aparece, se coloca bajo el chevrón de la barra;
 * si Explorer intenta devolverlo abajo, se vuelve a subir mientras siga abierto. Ya arriba,
 * la barra de tareas se vuelve a esconder (si al hacerlo Windows cerrase el panel, en
 * adelante se deja a la vista mientras esté abierto). */
#define TRAY_FLYOUT L"TopLevelWindowForOverflowXamlIsland"
static HWND s_trayFly;
static HWINEVENTHOOK s_trayHook;
static BOOL s_trayKeepTaskbar;

static void PlaceTrayFlyout(HWND w)
{
    RECT r;
    if (!B.hwnd || !GetWindowRect(w, &r)) return;
    const int fw = r.right - r.left;
    const RECT *hb = &B.hit[BH_TRAY];
    int x = B.mon.left + (hb->left + hb->right) / 2 - fw / 2;
    x = max(B.mon.left + BS(8), min(B.mon.right - BS(8) - fw, x));
    const int y = B.mon.top + B.h + BS(6);
    if (r.left != x || r.top != y) SetWindowPos(w, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

static void StopTrayWatch(void)
{
    if (s_trayHook) { UnhookWinEvent(s_trayHook); s_trayHook = NULL; }
    s_trayFly = NULL;
}

static void CALLBACK TrayHook(HWINEVENTHOOK hk, DWORD ev, HWND w, LONG obj, LONG child, DWORD th, DWORD t)
{
    (void)hk; (void)th; (void)t;
    if (!s_trayFly || w != s_trayFly || obj != OBJID_WINDOW || child != CHILDID_SELF) return;
    if (ev == EVENT_OBJECT_HIDE) { StopTrayWatch(); return; }
    if (ev == EVENT_OBJECT_SHOW || ev == EVENT_OBJECT_LOCATIONCHANGE) {
        PlaceTrayFlyout(w);
        if (ev == EVENT_OBJECT_SHOW && B.hwnd && g_cfg.dock && g_cfg.dockHideTaskbar && !s_trayKeepTaskbar) {
            B.trayStep = 3;
            SetTimer(B.hwnd, TIMER_TRAY, 250, NULL);
        }
    }
}

static void StartTrayWatch(void)
{
    StopTrayWatch();
    HWND w = FindWindowW(TRAY_FLYOUT, NULL);           /* existe siempre, oculta hasta abrirse */
    DWORD pid = 0;
    if (!w || !GetWindowThreadProcessId(w, &pid)) return;
    s_trayFly = w;
    s_trayHook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE, NULL, TrayHook, pid, 0, WINEVENT_OUTOFCONTEXT);
    if (IsWindowVisible(w)) PlaceTrayFlyout(w);
}

/* Pantalla completa: lo avisa Windows (ABN_FULLSCREENAPP) o lo vemos nosotros en la ventana
 * de delante (el Escritorio remoto maximizado, por ejemplo, no siempre lo avisa). */
static BOOL s_fsAbn, s_fsFg;

static void BarApplyFullscreen(void)
{
    const BOOL fs = s_fsAbn || s_fsFg;
    if (!B.hwnd || fs == B.fullscreen) return;
    B.fullscreen = fs;
    if (fs && TP.hwnd) DestroyWindow(TP.hwnd);
    if (fs) CC_Close();
    ShowWindow(B.hwnd, fs ? SW_HIDE : SW_SHOWNOACTIVATE);
    if (g_ctrl) PostMessageW(g_ctrl, WM_BARCHANGED, 0, 0);
    if (!fs) SetWindowPos(B.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void Bar_FullscreenFg(const RECT *mon)
{
    s_fsFg = mon && EqualRect(mon, &B.mon);
    BarApplyFullscreen();
}

static LRESULT CALLBACK BarProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        PaintBar(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_BAR_APPBAR:
        if (w == ABN_POSCHANGED) BarPosition();
        else if (w == ABN_FULLSCREENAPP) {          /* juegos, vídeo, presentaciones: fuera */
            s_fsAbn = (BOOL)l;
            BarApplyFullscreen();
        }
        return 0;
    case WM_WINDOWPOSCHANGED: {
        APPBARDATA abd = { sizeof(abd) };
        abd.hWnd = h;
        SHAppBarMessage(ABM_WINDOWPOSCHANGED, &abd);
        break;
    }
    case WM_MOUSEMOVE: {
        POINT p = { (short)LOWORD(l), (short)HIWORD(l) };
        int hv = BH_NONE;
        for (int i = 1; i < BH_COUNT; ++i) if (PtInRect(&B.hit[i], p)) hv = i;
        if (hv != B.bhot) {
            B.bhot = hv;
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            BarKick();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        B.bhot = BH_NONE;
        B.pressed = BH_NONE;
        BarKick();
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p = { (short)LOWORD(l), (short)HIWORD(l) };
        B.pressed = BH_NONE;
        for (int i = 1; i < BH_COUNT; ++i) if (PtInRect(&B.hit[i], p)) B.pressed = i;
        BarKick();
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT p = { (short)LOWORD(l), (short)HIWORD(l) };
        int hit = BH_NONE;
        for (int i = 1; i < BH_COUNT; ++i) if (PtInRect(&B.hit[i], p)) hit = i;
        const int was = B.pressed;
        B.pressed = BH_NONE;
        BarKick();
        if (hit != was) return 0;
        switch (hit) {
        case BH_LOGO:  Panel_ShowTab(0); break;
        case BH_CLOCK: Notch_OpenCenter(); break;
        case BH_VOL: case BH_WIFI: case BH_BATT: case BH_CC: CC_Toggle(); break;
        case BH_TRAY:  /* el panel propio con los iconos de la bandeja */
            if (TP.hwnd && !TP.pop.closing) { TP_Close(); break; }
            if (!TP.hwnd && GetTickCount() - TP.closedAt < 300) break;     /* el clic ya lo cerró */
            if (TP_Open()) break;
            /* aún sin iconos propios: el panel de Windows, dejando ver la barra de tareas */
            Dock_TrayPeek(TRUE);
            B.trayStep = 0;
            SetTimer(h, TIMER_TRAY, 120, NULL);
            break;
        }
        return 0;
    }
    case WM_MOUSEWHEEL:     /* rueda sobre la barra = volumen, como en muchas barras de macOS */
        SetVolume((B.muted ? 0 : B.volume) + (GET_WHEEL_DELTA_WPARAM(w) > 0 ? 0.04f : -0.04f));
        Bar_PulseVolume(GET_WHEEL_DELTA_WPARAM(w) > 0);
        return 0;
    case WM_TIMER:
        if (w == TIMER_CLOCK) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            if (st.wMinute != B.lastMinute) { B.lastMinute = st.wMinute; InvalidateRect(h, NULL, FALSE); }
            /* vidrio: si cambió lo que hay detrás (ventana arrastrada, fondo nuevo), repintar */
            if (BarGlass() && CaptureBarBack()) { InvalidateRect(h, NULL, FALSE); App_BarSurfaceChanged(); }
            ArmClock(h);
        } else if (w == TIMER_BANIM) {
            BarAnimate();
        } else if (w == TIMER_TRAY) {
            /* Win+B lleva el foco al área de notificación (al botón de iconos ocultos) y
             * Entrar lo abre: el panel real de Windows, con todo funcionando */
            if (B.trayStep == 0) {
                StartTrayWatch();
                INPUT in[4] = { 0 };
                for (int i = 0; i < 4; ++i) in[i].type = INPUT_KEYBOARD;
                in[0].ki.wVk = VK_LWIN; in[1].ki.wVk = 'B';
                in[2].ki.wVk = 'B'; in[2].ki.dwFlags = KEYEVENTF_KEYUP;
                in[3].ki.wVk = VK_LWIN; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(4, in, sizeof(INPUT));
                B.trayStep = 1;
                SetTimer(h, TIMER_TRAY, 380, NULL);
            } else if (B.trayStep == 1) {
                INPUT in[2] = { 0 };
                in[0].type = in[1].type = INPUT_KEYBOARD;
                in[0].ki.wVk = in[1].ki.wVk = VK_RETURN;
                in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(2, in, sizeof(INPUT));
                B.trayStep = 2;
                SetTimer(h, TIMER_TRAY, 2000, NULL);     /* si no llega a abrirse, se deja de vigilar */
            } else if (B.trayStep == 2) {
                KillTimer(h, TIMER_TRAY);
                if (!s_trayFly || !IsWindowVisible(s_trayFly)) StopTrayWatch();
            } else if (B.trayStep == 3) {           /* el panel ya está arriba */
                Dock_TrayPeek(FALSE);
                B.trayStep = 4;
                SetTimer(h, TIMER_TRAY, 450, NULL);
            } else {
                KillTimer(h, TIMER_TRAY);
                /* ¿Windows lo cerró al esconder la barra? Entonces la próxima vez se queda */
                if (!s_trayFly || !IsWindowVisible(s_trayFly)) { s_trayKeepTaskbar = TRUE; StopTrayWatch(); }
            }
        } else if (w == TIMER_STATUS) {         /* repaso de seguridad: lo normal llega por avisos */
            ReadBattery();
            ReadWifi();
            ReadVolume();
            BarSync();
            InvalidateRect(h, NULL, FALSE);
        } else if (w == TIMER_WIFIQ) {
            KillTimer(h, TIMER_WIFIQ);
            ReadWifi();
            BarSync();
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_TRAYCHANGED:
        TP_Refresh();
        return 0;
    case WM_BAR_STATUS:
        if (w == 1) {                           /* volumen o silencio: al momento */
            ReadVolume();
            BarSync();
            InvalidateRect(h, NULL, FALSE);
        } else SetTimer(h, TIMER_WIFIQ, 250, NULL);
        return 0;
    case WM_POWERBROADCAST:                     /* enchufar, desenchufar, cambio de porcentaje */
        ReadBattery();
        BarSync();
        InvalidateRect(h, NULL, FALSE);
        return TRUE;
    case WM_SETTINGCHANGE:
        LoadBarLook();
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_DESTROY: {
        KillTimer(h, TIMER_CLOCK);
        KillTimer(h, TIMER_STATUS);
        KillTimer(h, TIMER_BANIM);
        KillTimer(h, TIMER_TRAY);
        KillTimer(h, TIMER_WIFIQ);
        StopTrayWatch();
        if (TP.hwnd) DestroyWindow(TP.hwnd);
        Tray_Stop();
        if (B.battNotify) { UnregisterPowerSettingNotification(B.battNotify); B.battNotify = NULL; }
        B.synced = FALSE;
        if (B.registered) {
            APPBARDATA abd = { sizeof(abd) };
            abd.hWnd = h;
            SHAppBarMessage(ABM_REMOVE, &abd);      /* las ventanas recuperan el espacio */
            B.registered = FALSE;
        }
        Canvas_Free(&B.cv);
        Canvas_Free(&s_back);
        s_backHash = 0;
        B.hwnd = NULL;
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

/* ───────────────────────── Reloj de Windows ───────────────────────── */
static void BroadcastTray(void)
{
    DWORD_PTR r;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"TraySettings", SMTO_ABORTIFHUNG, 1000, &r);
}

void Bar_ApplyClock(BOOL hide)
{
    HKEY k;
    const BOOL already = RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"ClockHidden", RRF_RT_REG_DWORD, NULL, NULL, NULL) == ERROR_SUCCESS;
    if (hide == already) return;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, ADVANCED_KEY, 0, NULL, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    if (hide) {
        DWORD prev = 0xFFFFFFFF, sz = sizeof(prev);
        if (RegQueryValueExW(k, CLOCK_VALUE, NULL, NULL, (BYTE *)&prev, &sz) != ERROR_SUCCESS) prev = 0xFFFFFFFF;
        const DWORD zero = 0;
        RegSetValueExW(k, CLOCK_VALUE, 0, REG_DWORD, (const BYTE *)&zero, sizeof(zero));
        HKEY mine;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &mine, NULL) == ERROR_SUCCESS) {
            RegSetValueExW(mine, L"ClockHidden", 0, REG_DWORD, (const BYTE *)&prev, sizeof(prev));
            RegCloseKey(mine);
        }
    } else {
        DWORD prev = 0xFFFFFFFF, sz = sizeof(prev);
        RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"ClockHidden", RRF_RT_REG_DWORD, NULL, &prev, &sz);
        if (prev == 0xFFFFFFFF) RegDeleteValueW(k, CLOCK_VALUE);
        else RegSetValueExW(k, CLOCK_VALUE, 0, REG_DWORD, (const BYTE *)&prev, sizeof(prev));
        HKEY mine;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_SET_VALUE, &mine) == ERROR_SUCCESS) {
            RegDeleteValueW(mine, L"ClockHidden");
            RegCloseKey(mine);
        }
    }
    RegCloseKey(k);
    BroadcastTray();
}

/* ───────────────────────── API ───────────────────────── */
void Bar_Register(void)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = BarProc;
    wc.hInstance     = g_inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = BAR_CLASS;
    RegisterClassExW(&wc);

    WNDCLASSEXW cc = { sizeof(cc) };
    cc.style         = CS_DROPSHADOW;
    cc.lpfnWndProc   = CCProc;
    cc.hInstance     = g_inst;
    cc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    cc.lpszClassName = CC_CLASS;
    RegisterClassExW(&cc);

    WNDCLASSEXW tp = { sizeof(tp) };
    tp.lpfnWndProc   = TPProc;
    tp.hInstance     = g_inst;
    tp.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    tp.lpszClassName = TP_CLASS;
    RegisterClassExW(&tp);
}

/* Muestra u oculta la barra según g_cfg (y el reloj de Windows con ella). */
void Bar_Apply(void)
{
    if (!g_cfg.menubar) {
        CC_Close();
        if (B.hwnd) DestroyWindow(B.hwnd);
        Bar_ApplyClock(FALSE);
        return;
    }
    if (!B.hwnd) {
        BarMeasure();
        RefreshStatus();
        B.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, BAR_CLASS, L"OpenDock", WS_POPUP,
                                 B.mon.left, B.mon.top, B.mon.right - B.mon.left, B.h, NULL, NULL, g_inst, NULL);
        if (!B.hwnd) return;
        APPBARDATA abd = { sizeof(abd) };
        abd.hWnd = B.hwnd;
        abd.uCallbackMessage = WM_BAR_APPBAR;
        B.registered = SHAppBarMessage(ABM_NEW, &abd) != 0;
        BarApplyCapture();
        BarPosition();
        ArmClock(B.hwnd);
        SetTimer(B.hwnd, TIMER_STATUS, 30000, NULL);
        Tray_Start(B.hwnd);             /* los iconos de la bandeja, para el chevrón */
        B.battNotify = RegisterPowerSettingNotification(B.hwnd, &kGUID_BatteryPercent, DEVICE_NOTIFY_WINDOW_HANDLE);
        BarSync();
        Brightness_Request(-1);
    } else {
        LoadBarLook();
        InvalidateRect(B.hwnd, NULL, FALSE);
    }
    Bar_ApplyClock(g_cfg.hideClock);
}

/* Cambió el material, el acento o "ocultar en capturas". */
void Bar_StyleChanged(void)
{
    if (!B.hwnd) return;
    LoadBarLook();
    BarApplyCapture();
    s_backHash = 0;
    Canvas_Free(&s_back);
    InvalidateRect(B.hwnd, NULL, FALSE);
    if (C.hwnd) InvalidateRect(C.hwnd, NULL, FALSE);
}

/* Cambios de resolución, escala o monitores. */
void Bar_Reposition(void)
{
    if (!B.hwnd) return;
    BarMeasure();
    BarPosition();
}

void Bar_ForegroundChanged(void)
{
    if (!B.hwnd) return;
    wchar_t name[64];
    ForegroundAppName(name, 64);
    const BOOL icon = ForegroundAppIcon(BS(13));
    if (name[0] && lstrcmpW(name, B.app)) lstrcpynW(B.app, name, 64);
    else if (!icon) return;
    InvalidateRect(B.hwnd, NULL, FALSE);
}

/* Alto que ocupa la barra en el monitor indicado (0 si no está en él). */
int Bar_HeightOn(const RECT *mon)
{
    if (!B.hwnd || B.fullscreen || !EqualRect(mon, &B.mon)) return 0;
    return B.h;
}

void Bar_Raise(void)
{
    if (B.hwnd && !B.fullscreen && App_Covered(B.hwnd))
        SetWindowPos(B.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void Bar_Destroy(void)
{
    if (C.hwnd) DestroyWindow(C.hwnd);      /* al salir, sin animación */
    if (B.hwnd) DestroyWindow(B.hwnd);
    if (B.appIcon) { DeleteObject(B.appIcon); B.appIcon = NULL; B.appKey[0] = 0; }
    CloseVolume();
    if (B.wlan) { WlanCloseHandle(B.wlan, NULL); B.wlan = NULL; }     /* también quita sus avisos */
    HFONT *all[] = { &B.fBold, &B.fText, &B.fIcon };
    for (int i = 0; i < 3; ++i) if (*all[i]) { DeleteObject(*all[i]); *all[i] = NULL; }
}
