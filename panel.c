/*
 * panel.c — mini ventana de configuración (estilo flyout de Windows 11).
 *
 * Se abre con clic en el icono de la bandeja, al lanzar el .exe otra vez, desde el
 * menú Inicio o con el engranaje del centro de notificaciones del notch.
 * Dos pestañas: General (esquinas) y Notch (avisos, notificaciones de Windows y
 * diseño de la isla). Todo se dibuja a mano sobre un DIB, respeta tema claro/oscuro
 * y color de acento, y se cierra al perder el foco.
 */
#include "app.h"
#include <dwmapi.h>
#include <shellapi.h>
#include "resource.h"

#define PANEL_CLASS   L"CornerRadius.Panel"
#define PW            340       /* ancho lógico (96 DPI) */
#define BODY_Y        116       /* inicio del contenido de la pestaña */
#define FOOT_H        96
#define TIMER_ANIM    0x5A1     /* ids propios: también llegan al centro de control que lo aloja */
#define TIMER_CONFIRM 0x5A2

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

/* Opciones (claves de los elementos) */
enum {
    K_RADIUS, K_HIDECAP, K_STARTUP, K_BAR, K_CLOCK, K_BATTPCT, K_DOCK, K_DOCKTB, K_DHIDE, K_DWINFULL, K_DBLUR, K_DOPAC, K_DICON,
    K_NOTCH, K_MIRROR, K_BANNERS, K_SITEICON, K_EDGE, K_SHOTS,
    K_SOUND, K_SOUNDVOL, K_STYLE, K_MATERIAL, K_SIZE, K_BOUNCE, K_ACCENT, K_TEST,
    K_COUNT
};
enum { IT_RADIUS, IT_DIVIDER, IT_TOGGLE, IT_SEGMENT, IT_SWATCH, IT_BUTTON, IT_PICKER, IT_VOLUME };

typedef struct {
    int     type, key;
    LPCWSTR title, sub;
    LPCWSTR opts[4];
} ItemDef;

static const ItemDef kGeneral[] = {
    { IT_RADIUS,  K_RADIUS },
    { IT_DIVIDER, -1 },
    { IT_TOGGLE,  K_HIDECAP, L"Ocultar en capturas", L"No salen en screenshots ni grabaciones" },
    { IT_TOGGLE,  K_STARTUP, L"Iniciar con Windows", L"Arranca sola al encender el equipo" },
    { IT_DIVIDER, -1 },
    { IT_SEGMENT, K_MATERIAL, L"Material \x00B7 notch, barra y dock", NULL, { L"OLED", L"Vidrio", L"Sistema" } },
    { IT_DIVIDER, -1 },
    { IT_TOGGLE,  K_BAR,     L"Barra superior", L"Sin ella vuelve la barra de tareas y se quita el dock \x00B7 Ctrl+Alt+B" },
    { IT_TOGGLE,  K_CLOCK,   L"Ocultar reloj de Windows", L"Mientras la barra superior está activa" },
    { IT_TOGGLE,  K_BATTPCT, L"Porcentaje en la batería", L"El número dentro del icono de la barra" },
};

static const ItemDef kDock[] = {
    { IT_TOGGLE,  K_DOCK,    L"Dock", L"Apps ancladas y abiertas, con magnificación \x00B7 Ctrl+Alt+D" },
    { IT_TOGGLE,  K_DOCKTB,  L"Desactivar barra de tareas", L"La de Windows desaparece mientras el dock está activo" },
    { IT_SEGMENT, K_DHIDE,   L"Ocultar sin el cursor", NULL, { L"No", L"A la mitad", L"Del todo" } },
    { IT_TOGGLE,  K_DWINFULL, L"Ventanas hasta abajo", L"Con el dock oculto, las maximizadas usan toda la altura" },
    { IT_DIVIDER, -1 },
    { IT_SEGMENT, K_DBLUR,   L"Desenfoque del fondo", NULL, { L"No", L"Suave", L"Intenso" } },
    { IT_SEGMENT, K_DOPAC,   L"Opacidad", NULL, { L"30 %", L"55 %", L"75 %", L"92 %" } },
    { IT_SEGMENT, K_DICON,   L"Tamaño de iconos", NULL, { L"Pequeño", L"Mediano", L"Grande", L"Enorme" } },
};

static const ItemDef kNotch[] = {
    { IT_TOGGLE,  K_NOTCH,   L"Avisos tipo notch" },
    { IT_TOGGLE,  K_MIRROR,  L"Notificaciones de Windows en el notch" },
    { IT_TOGGLE,  K_BANNERS, L"Notificaciones ofuscadas" },
    { IT_TOGGLE,  K_SITEICON, L"Icono de las apps web", L"En avisos del navegador, el del sitio si lo trae; si no, su inicial" },
    { IT_TOGGLE,  K_EDGE,    L"Mini notch al pasar por arriba" },
    { IT_TOGGLE,  K_SHOTS,   L"Capturas en el notch" },
    { IT_DIVIDER, -1 },
    { IT_PICKER,  K_SOUND,    L"Sonido de notificación" },
    { IT_VOLUME,  K_SOUNDVOL, L"Volumen" },
    { IT_SEGMENT, K_STYLE,    L"Estilo",   NULL, { L"Notch", L"Flotante" } },
    { IT_SEGMENT, K_SIZE,     L"Tamaño",   NULL, { L"Compacto", L"Normal", L"Grande" } },
    { IT_SEGMENT, K_BOUNCE,   L"Rebote",   NULL, { L"Suave", L"Normal", L"Bouncy" } },
    { IT_SWATCH,  K_ACCENT,   L"Acento" },
    { IT_BUTTON,  K_TEST,     L"Probar notch" },
};

#define MAX_ITEMS 16
typedef struct { const ItemDef *def; int y, h; } Item;

/* Hits: elemento * 16 + subparte; fijos a partir de 1000 */
#define HIT(i, sub)   ((i) * 16 + (sub) + 1)
#define HIT_ITEM(h)   (((h) - 1) / 16)
#define HIT_SUB(h)    (((h) - 1) % 16)
enum { H_NONE = 0, H_MASTER = 1000, H_TAB0, H_TAB1, H_TAB2, H_INSTALL, H_EXIT, H_BACK };
#define NTABS 3

static const int kChips[5] = { 8, 12, 16, 24, 32 };

static struct {
    HWND   hwnd;
    UINT   dpi;
    Canvas cv;
    Theme  th;
    HFONT  fTitle, fBody, fStrong, fSmall, fIcon;
    HICON  icon;
    int    tab;
    Item   items[MAX_ITEMS];
    int    nitems, height;
    int    hover, pressed;
    BOOL   dragging, tracking, confirmUninstall;
    float  sw[K_COUNT], master;
    DWORD  closedAt;
    Pop    pop;                 /* abre/cierra con el rebote del notch */
    PopGlass glass;
    /* alojado como sección del centro de control (P.hwnd es entonces su ventana) */
    BOOL   embedded;
    const PopGlass *extGlass;
    int    extX, extY;
} P;

static int S(int v) { return MulDiv(v, (int)P.dpi, 96); }

/* ───────────────────────── Estado de las opciones ───────────────────────── */
static BOOL ToggleValue(int key)
{
    switch (key) {
    case K_HIDECAP: return g_cfg.hideCapture;
    case K_STARTUP: return Inst_IsStartup();
    case K_BAR:     return g_cfg.menubar;
    case K_CLOCK:   return g_cfg.hideClock;
    case K_BATTPCT: return g_cfg.battPct;
    case K_DOCK:    return g_cfg.dock;
    case K_DOCKTB:  return g_cfg.dockHideTaskbar;
    case K_DWINFULL: return g_cfg.dockWinFull;
    case K_NOTCH:   return g_cfg.notch;
    case K_MIRROR:  return g_cfg.mirror;
    case K_BANNERS: return g_cfg.hideBanners;
    case K_SITEICON: return g_cfg.siteIcons;
    case K_EDGE:    return g_cfg.edgeHover;
    case K_SHOTS:   return g_cfg.captures;
    }
    return FALSE;
}

/* Las opciones del notch dependen del interruptor general de avisos. */
static BOOL ItemEnabled(int key)
{
    if (key == K_CLOCK || key == K_BATTPCT) return g_cfg.menubar;
    if (key == K_MATERIAL) return TRUE;
    if (key == K_DWINFULL) return g_cfg.dock && g_cfg.dockAutoHide;
    if (key == K_DOCKTB || key == K_DHIDE || key == K_DBLUR || key == K_DOPAC || key == K_DICON) return g_cfg.dock;
    if (key == K_SOUNDVOL) return g_cfg.notch && g_cfg.sound != 0;
    return key < K_MIRROR || key == K_BANNERS || g_cfg.notch;
}

static int SegmentValue(int key)
{
    switch (key) {
    case K_SOUND:    return g_cfg.sound;
    case K_STYLE:    return g_cfg.floating;
    case K_MATERIAL: return g_cfg.material;
    case K_SIZE:     return g_cfg.size;
    case K_DBLUR:    return g_cfg.dockBlur;
    case K_DOPAC:    return g_cfg.dockOpacity;
    case K_DICON:    return g_cfg.dockIcon;
    case K_DHIDE:    return g_cfg.dockAutoHide;
    }
    return g_cfg.bounce;
}

static int SegmentCount(const ItemDef *d) { return d->opts[3] ? 4 : d->opts[2] ? 3 : 2; }

static void FreeFonts(void)
{
    HFONT *fonts[] = { &P.fTitle, &P.fBody, &P.fStrong, &P.fSmall, &P.fIcon };
    for (int i = 0; i < 5; ++i)
        if (*fonts[i]) { DeleteObject(*fonts[i]); *fonts[i] = NULL; }
}

static void MakeResources(void)
{
    FreeFonts();
    LPCWSTR face = Gfx_UiFace();
    P.fTitle  = Gfx_Font(face, S(15), FW_SEMIBOLD, CLEARTYPE_QUALITY);
    P.fBody   = Gfx_Font(face, S(13), FW_NORMAL,   CLEARTYPE_QUALITY);
    P.fStrong = Gfx_Font(face, S(13), FW_SEMIBOLD, CLEARTYPE_QUALITY);
    P.fSmall  = Gfx_Font(face, S(12), FW_NORMAL,   CLEARTYPE_QUALITY);
    P.fIcon   = Gfx_Font(Gfx_IconFace(), S(13), FW_NORMAL, CLEARTYPE_QUALITY);
    if (P.icon) DestroyIcon(P.icon);
    P.icon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, S(32), S(32), 0);
}

/* ───────────────────────── Maquetación ───────────────────────── */
static int ItemHeight(const ItemDef *d)
{
    switch (d->type) {
    case IT_RADIUS:  return 100;
    case IT_DIVIDER: return 13;
    case IT_TOGGLE:  return d->sub ? 52 : 42;
    case IT_SEGMENT: return 60;
    case IT_SWATCH:  return 50;
    case IT_BUTTON:  return 48;
    case IT_PICKER:  return 60;
    case IT_VOLUME:  return 58;
    }
    return 0;
}

static void Layout(void)
{
    const ItemDef *defs = P.tab == 2 ? kDock : P.tab ? kNotch : kGeneral;
    const int n = P.tab == 2 ? (int)(sizeof(kDock) / sizeof(kDock[0]))
                : P.tab ? (int)(sizeof(kNotch) / sizeof(kNotch[0])) : (int)(sizeof(kGeneral) / sizeof(kGeneral[0]));
    int y = BODY_Y;
    P.nitems = 0;
    for (int i = 0; i < n && i < MAX_ITEMS; ++i) {
        Item *it = &P.items[P.nitems++];
        it->def = &defs[i];
        it->y = y;
        it->h = ItemHeight(&defs[i]);
        y += it->h;
    }
    P.height = y + 8 + FOOT_H;
}

static int FootY(void) { return P.height - FOOT_H; }

/* Rectángulos (lógicos) de las subpartes de cada elemento */
static RECT ChipRect(const Item *it, int i) { RECT r = { 16 + i * 63, it->y + 60, 16 + i * 63 + 55, it->y + 88 }; return r; }
static RECT SliderRect(const Item *it) { RECT r = { 8, it->y + 26, PW - 8, it->y + 52 }; return r; }
static RECT SegRect(const Item *it, int i)
{
    const int w = (PW - 32) / SegmentCount(it->def);
    RECT r = { 16 + i * w, it->y + 24, 16 + (i + 1) * w, it->y + 54 };
    return r;
}
static RECT SwatchRect(const Item *it, int i)
{
    const int cx = PW - 16 - 11 - (ACCENT_COUNT - 1 - i) * 30, cy = it->y + 25;
    RECT r = { cx - 13, cy - 13, cx + 13, cy + 13 };
    return r;
}
static RECT ButtonRect(const Item *it) { RECT r = { 16, it->y + 8, PW - 16, it->y + 40 }; return r; }
/* selector ‹ nombre ›: 0 anterior · 1 el nombre (vuelve a sonar) · 2 siguiente */
static RECT PickRect(const Item *it, int i)
{
    const int l = i == 0 ? 16 : i == 1 ? 58 : PW - 58, r = i == 0 ? 58 : i == 1 ? PW - 58 : PW - 16;
    RECT x = { l, it->y + 24, r, it->y + 54 };
    return x;
}
static RECT Rc(int l, int t, int r, int b) { RECT x = { l, t, r, b }; return x; }
static RECT TabRect(int i) { const int w = (PW - 32) / NTABS; return Rc(16 + i * w, 70, i == NTABS - 1 ? PW - 16 : 16 + (i + 1) * w, 102); }

static RECT Scaled(RECT r) { RECT s = { S(r.left), S(r.top), S(r.right), S(r.bottom) }; return s; }

static BOOL InLogical(RECT r, int x, int y)
{
    const RECT s = Scaled(r);
    const POINT p = { x, y };
    return PtInRect(&s, p);
}

static int HitTest(int x, int y)
{
    if (InLogical(Rc(PW - 72, 16, PW - 8, 56), x, y))                  return H_MASTER;
    if (P.embedded && InLogical(Rc(10, 16, 54, 60), x, y))             return H_BACK;
    for (int i = 0; i < NTABS; ++i) if (InLogical(TabRect(i), x, y)) return H_TAB0 + i;
    if (InLogical(Rc(16, FootY() + 12, 196, FootY() + 44), x, y))      return H_INSTALL;
    if (InLogical(Rc(PW - 96, FootY() + 12, PW - 16, FootY() + 44), x, y)) return H_EXIT;

    for (int i = 0; i < P.nitems; ++i) {
        const Item *it = &P.items[i];
        if (!InLogical(Rc(8, it->y, PW - 8, it->y + it->h), x, y)) continue;
        switch (it->def->type) {
        case IT_RADIUS:
            if (InLogical(SliderRect(it), x, y)) return HIT(i, 0);
            for (int c = 0; c < 5; ++c) if (InLogical(ChipRect(it, c), x, y)) return HIT(i, 1 + c);
            return H_NONE;
        case IT_TOGGLE:
            return ItemEnabled(it->def->key) ? HIT(i, 0) : H_NONE;
        case IT_SEGMENT:
            for (int s = 0; s < SegmentCount(it->def); ++s) if (InLogical(SegRect(it, s), x, y)) return HIT(i, s);
            return H_NONE;
        case IT_SWATCH:
            for (int s = 0; s < ACCENT_COUNT; ++s) if (InLogical(SwatchRect(it, s), x, y)) return HIT(i, s);
            return H_NONE;
        case IT_BUTTON:
            return InLogical(ButtonRect(it), x, y) ? HIT(i, 0) : H_NONE;
        case IT_VOLUME:
            return ItemEnabled(it->def->key) && InLogical(Rc(16, it->y + 22, PW - 16, it->y + 56), x, y) ? HIT(i, 0) : H_NONE;
        case IT_PICKER:
            if (!ItemEnabled(it->def->key)) return H_NONE;
            for (int s = 0; s < 3; ++s) if (InLogical(PickRect(it, s), x, y)) return HIT(i, s);
            return H_NONE;
        }
    }
    return H_NONE;
}

/* El panel sigue el material común: OLED negro puro, vidrio sobre lo que hay detrás o
 * el tema del sistema. */
static void ApplyMaterial(void)
{
    if (!P.th.dark) return;
    if (g_cfg.material == MAT_OLED) {
        P.th.bg = 0x000000; P.th.ctrl = 0x1C1C1E; P.th.ctrlHover = 0x2C2C2E; P.th.line = 0x2C2C2E;
    } else if (g_cfg.material == MAT_GLASS) {
        P.th.ctrl = 0x3A3A3C; P.th.ctrlHover = 0x48484A;
    }
}

/* ───────────────────────── Dibujo ───────────────────────── */
static void FillR(Canvas *c, RECT r, int rad, DWORD rgb, float a)
{
    r = Scaled(r);
    Gfx_FillRRect(c, (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top), (float)S(rad), rgb, a);
}

static void TextR(Canvas *c, HFONT f, LPCWSTR s, RECT r, DWORD rgb, UINT fmt)
{
    r = Scaled(r);
    Gfx_Text(c, f, s, r.left, r.top, r.right - r.left, r.bottom - r.top, rgb,
             fmt | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

static void DrawSwitch(Canvas *c, int lx, int ly, float t, BOOL hot, BOOL enabled)
{
    const Theme *th = &P.th;
    const int x = S(lx), y = S(ly);
    const float w = (float)S(40), h = (float)S(20), r = h * 0.5f, line = (float)max(1, S(1));
    const float alpha = enabled ? 1.0f : 0.4f;
    if (t < 1.0f) {
        if (hot) Gfx_FillRRect(c, (float)x, (float)y, w, h, r, th->ctrlHover, (1.0f - t) * alpha);
        Gfx_StrokeRRect(c, x + line * 0.5f, y + line * 0.5f, w - line, h - line, r - line * 0.5f,
                        line, th->fg2, (1.0f - t) * alpha);
    }
    if (t > 0.0f) Gfx_FillRRect(c, (float)x, (float)y, w, h, r, th->accent, t * alpha);
    const float kr = (float)S(hot ? 6 : 5) * (1.0f - t) + (float)S(hot ? 7 : 6) * t;
    Gfx_FillCircle(c, x + r + (w - 2 * r) * t, y + r, kr, Gfx_Mix(th->fg2, th->onAccent, t), alpha);
}

static void DrawButton(Canvas *c, RECT r, int id, LPCWSTR label, BOOL primary, BOOL danger)
{
    const Theme *th = &P.th;
    const BOOL hot = P.hover == id, down = P.pressed == id && hot;
    DWORD bg, fg;
    if (primary) {
        bg = down ? Gfx_Mix(th->accent, th->bg, 0.25f) : hot ? Gfx_Mix(th->accent, th->bg, 0.12f) : th->accent;
        fg = th->onAccent;
    } else {
        bg = hot ? th->ctrlHover : th->ctrl;
        fg = danger ? th->danger : th->fg;
    }
    FillR(c, r, 6, bg, 1.0f);
    TextR(c, P.fStrong, label, r, fg, DT_CENTER);
}

static void DrawRadius(Canvas *c, const Item *it, int idx)
{
    const Theme *t = &P.th;
    wchar_t buf[32];
    TextR(c, P.fBody, L"Radio de las esquinas", Rc(16, it->y, 240, it->y + 20), t->fg, 0);
    wsprintfW(buf, L"%d px", g_cfg.radius);
    TextR(c, P.fStrong, buf, Rc(PW - 120, it->y, PW - 16, it->y + 20), t->fg, DT_RIGHT);

    const float sl = (float)S(26), sr = (float)S(PW - 26), cy = (float)S(it->y + 39);
    const float tx = sl + (sr - sl) * RadiusToT(g_cfg.radius);
    const BOOL hot = P.hover == HIT(idx, 0);
    Gfx_FillRRect(c, sl, cy - S(2), sr - sl, (float)S(4), (float)S(2), t->track, 1.0f);
    Gfx_FillRRect(c, sl, cy - S(2), tx - sl, (float)S(4), (float)S(2), t->accent, 1.0f);
    Gfx_FillCircle(c, tx, cy, (float)S(10), t->thumb, 1.0f);
    if (!t->dark) Gfx_StrokeRRect(c, tx - S(10), cy - S(10), (float)S(20), (float)S(20), (float)S(10), 1.0f, t->line, 1.0f);
    Gfx_FillCircle(c, tx, cy, (float)(P.dragging ? S(5) : hot ? S(7) : S(6)), t->accent, 1.0f);

    for (int i = 0; i < 5; ++i) {
        const BOOL sel = kChips[i] == g_cfg.radius;
        const RECT r = ChipRect(it, i);
        FillR(c, r, 6, sel ? t->accent : P.hover == HIT(idx, 1 + i) ? t->ctrlHover : t->ctrl, 1.0f);
        wsprintfW(buf, L"%d", kChips[i]);
        TextR(c, sel ? P.fStrong : P.fBody, buf, r, sel ? t->onAccent : t->fg, DT_CENTER);
    }
}

static void DrawItem(Canvas *c, const Item *it, int idx)
{
    const Theme *t = &P.th;
    const ItemDef *d = it->def;
    const BOOL enabled = d->key < 0 || ItemEnabled(d->key);
    const DWORD fg = enabled ? t->fg : t->fg3;

    switch (d->type) {
    case IT_RADIUS:
        DrawRadius(c, it, idx);
        break;

    case IT_DIVIDER:
        Gfx_FillRRect(c, (float)S(16), (float)S(it->y + 6), (float)S(PW - 32), (float)max(1, S(1)), 0, t->line, 1.0f);
        break;

    case IT_TOGGLE: {
        const BOOL hot = P.hover == HIT(idx, 0);
        if (hot) FillR(c, Rc(8, it->y + 2, PW - 8, it->y + it->h - 2), 6, t->ctrl, 1.0f);
        if (d->sub) {
            TextR(c, P.fBody, d->title, Rc(16, it->y + 8, PW - 72, it->y + 28), fg, 0);
            TextR(c, P.fSmall, d->sub, Rc(16, it->y + 27, PW - 72, it->y + 45), t->fg2, 0);
        } else {
            TextR(c, P.fBody, d->title, Rc(16, it->y, PW - 72, it->y + it->h), fg, 0);
        }
        DrawSwitch(c, PW - 56, it->y + (it->h - 20) / 2, P.sw[d->key], hot, enabled);
        break;
    }

    case IT_SEGMENT: {
        TextR(c, P.fBody, d->title, Rc(16, it->y + 2, PW - 16, it->y + 22), fg, 0);
        FillR(c, Rc(16, it->y + 24, PW - 16, it->y + 54), 8, t->ctrl, 1.0f);
        const int sel = SegmentValue(d->key);
        for (int s = 0; s < SegmentCount(d); ++s) {
            const RECT r = SegRect(it, s), in = Rc(r.left + 2, r.top + 2, r.right - 2, r.bottom - 2);
            if (s == sel)                      FillR(c, in, 6, t->dark ? 0x454545 : 0xFFFFFF, 1.0f);
            else if (P.hover == HIT(idx, s))   FillR(c, in, 6, t->ctrlHover, 1.0f);
            TextR(c, s == sel ? P.fStrong : P.fBody, d->opts[s], r, s == sel ? t->fg : t->fg2, DT_CENTER);
        }
        break;
    }

    case IT_SWATCH:
        TextR(c, P.fBody, d->title, Rc(16, it->y, 120, it->y + 50), fg, 0);
        for (int s = 0; s < ACCENT_COUNT; ++s) {
            const RECT r = Scaled(SwatchRect(it, s));
            const float cx = (r.left + r.right) * 0.5f, cy = (r.top + r.bottom) * 0.5f;
            const DWORD col = s ? kAccentPresets[s] : t->accent;
            if (s == g_cfg.accent)
                Gfx_StrokeRRect(c, cx - S(13), cy - S(13), (float)S(26), (float)S(26), (float)S(13), 2.0f, t->fg, 1.0f);
            Gfx_FillCircle(c, cx, cy, (float)S(P.hover == HIT(idx, s) ? 10 : 9), col, 1.0f);
            if (!s) Gfx_Text(c, P.fSmall, L"W", (int)(cx - S(9)), (int)(cy - S(9)), S(18), S(18),
                             t->onAccent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        break;

    case IT_BUTTON:
        DrawButton(c, ButtonRect(it), HIT(idx, 0), d->title, FALSE, FALSE);
        break;

    case IT_VOLUME: {           /* deslizador con altavoz que cambia y el porcentaje; suena al soltar */
        const BOOL on = ItemEnabled(d->key);
        const int v = g_cfg.soundVol;
        wchar_t pct[16];
        wsprintfW(pct, v ? L"%d %%" : L"Sin sonido", v);
        TextR(c, P.fBody, d->title, Rc(16, it->y + 2, PW - 16, it->y + 22), fg, 0);
        TextR(c, P.fStrong, pct, Rc(16, it->y + 2, PW - 16, it->y + 22), on ? t->fg : t->fg2, DT_RIGHT);
        const float sl = (float)S(46), sr = (float)S(PW - 46), cy = (float)S(it->y + 40);
        const float tx = sl + (sr - sl) * v / 100.0f, a = on ? 1.0f : 0.4f;
        const BOOL hot = P.hover == HIT(idx, 0);
        LPCWSTR lo = v == 0 ? L"\xE74F" : v < 34 ? L"\xE993" : v < 67 ? L"\xE994" : L"\xE995";
        Gfx_Text(c, P.fIcon, lo, S(16), (int)cy - S(10), S(24), S(20), on ? t->fg2 : t->fg3, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        Gfx_Text(c, P.fIcon, L"\xE995", S(PW - 40), (int)cy - S(10), S(24), S(20), on ? t->fg2 : t->fg3, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        const float th = (float)S(hot || P.dragging ? 8 : 6);
        Gfx_FillRRect(c, sl, cy - th * 0.5f, sr - sl, th, th * 0.5f, t->track, a);
        Gfx_FillRRect(c, sl, cy - th * 0.5f, max(th, tx - sl), th, th * 0.5f, t->accent, a);
        Gfx_FillCircle(c, tx, cy, (float)S(9), t->thumb, a);
        if (!t->dark) Gfx_StrokeRRect(c, tx - S(9), cy - S(9), (float)S(18), (float)S(18), (float)S(9), 1.0f, t->line, a);
        break;
    }

    case IT_PICKER: {           /* la librería de sonidos: se recorre con ‹ › y suena al elegir */
        TextR(c, P.fBody, d->title, Rc(16, it->y + 2, PW - 16, it->y + 22), fg, 0);
        TextR(c, P.fSmall, Notch_SoundFamily(g_cfg.sound), Rc(16, it->y + 2, PW - 16, it->y + 22), t->fg2, DT_RIGHT);
        FillR(c, Rc(16, it->y + 24, PW - 16, it->y + 54), 8, t->ctrl, 1.0f);
        for (int s = 0; s < 3; ++s) {
            const RECT r = PickRect(it, s), in = Rc(r.left + 2, r.top + 2, r.right - 2, r.bottom - 2);
            if (P.hover == HIT(idx, s)) FillR(c, in, 6, t->ctrlHover, 1.0f);
        }
        TextR(c, P.fStrong, L"\x2039", PickRect(it, 0), fg, DT_CENTER);
        TextR(c, P.fStrong, L"\x203A", PickRect(it, 2), fg, DT_CENTER);
        wchar_t label[64];
        wsprintfW(label, L"%s", Notch_SoundName(g_cfg.sound));
        TextR(c, P.fStrong, label, PickRect(it, 1), fg, DT_CENTER);
        break;
    }
    }
}

static void Render(void)
{
    RECT rc = { 0, 0, S(PW), S(P.height) };
    if (!P.cv.dc || P.cv.w != rc.right || P.cv.h != rc.bottom) {
        Canvas_Free(&P.cv);
        if (!Canvas_Init(&P.cv, rc.right, rc.bottom)) return;
    }
    Canvas *c = &P.cv;
    const Theme *t = &P.th;
    wchar_t buf[96];

    if (t->dark && g_cfg.material == MAT_GLASS) {
        if (P.embedded) Pop_PaintGlass(c, P.extGlass, P.extX, P.extY, 0x1C1C1E, 0.58f);
        else {
            RECT cr;
            Pop_ContentRect(&P.pop, &cr);
            Pop_PaintGlass(c, &P.glass, cr.left, cr.top, 0x1C1C1E, 0.6f);
        }
    } else {
        Canvas_Clear(c, t->bg);
    }

    /* Cabecera: icono (o volver, dentro del centro de control), nombre, estado e interruptor */
    if (P.embedded) {
        const float d = (float)S(32), cx = (float)S(32), cy = (float)S(36);
        Gfx_FillCircle(c, cx, cy, d * 0.5f, P.hover == H_BACK ? t->ctrlHover : t->ctrl, 1.0f);
        Gfx_Text(c, P.fIcon, L"\xE76B", (int)(cx - d * 0.5f), (int)(cy - d * 0.5f), (int)d, (int)d, t->fg,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        DrawIconEx(c->dc, S(16), S(20), P.icon, S(32), S(32), 0, NULL, DI_NORMAL);
    }
    TextR(c, P.fTitle, APP_NAME, Rc(60, 18, 240, 38), t->fg, 0);
    if (g_cfg.enabled) wsprintfW(buf, L"Activo \x00B7 %d px", g_cfg.radius);
    else lstrcpyW(buf, L"En pausa");
    TextR(c, P.fSmall, buf, Rc(60, 38, 240, 56), t->fg2, 0);
    DrawSwitch(c, PW - 56, 26, P.master, P.hover == H_MASTER, TRUE);

    /* Pestañas */
    FillR(c, Rc(16, 70, PW - 16, 102), 8, t->ctrl, 1.0f);
    static const LPCWSTR kTabs[NTABS] = { L"General", L"Notch", L"Dock" };
    for (int i = 0; i < NTABS; ++i) {
        const RECT r = TabRect(i);
        const RECT in = Rc(r.left + 2, r.top + 2, r.right - 2, r.bottom - 2);
        if (P.tab == i) FillR(c, in, 6, t->dark ? 0x454545 : 0xFFFFFF, 1.0f);
        else if (P.hover == H_TAB0 + i) FillR(c, in, 6, t->ctrlHover, 1.0f);
        TextR(c, P.tab == i ? P.fStrong : P.fBody, kTabs[i], r, P.tab == i ? t->fg : t->fg2, DT_CENTER);
    }

    for (int i = 0; i < P.nitems; ++i) DrawItem(c, &P.items[i], i);

    /* Pie: instalar / desinstalar, salir y estado */
    const int fy = FootY();
    Gfx_FillRRect(c, (float)S(16), (float)S(fy), (float)S(PW - 32), (float)max(1, S(1)), 0, t->line, 1.0f);
    const BOOL installed = Inst_IsRunningInstalled();
    const RECT inst = Rc(16, fy + 12, 196, fy + 44), exitb = Rc(PW - 96, fy + 12, PW - 16, fy + 44);
    if (installed)
        DrawButton(c, inst, H_INSTALL, P.confirmUninstall ? L"Confirmar" : L"Desinstalar", FALSE, P.confirmUninstall);
    else
        DrawButton(c, inst, H_INSTALL, Inst_Exists() ? L"Actualizar instalación" : L"Instalar en este equipo", TRUE, FALSE);
    DrawButton(c, exitb, H_EXIT, L"Salir", FALSE, FALSE);

    TextR(c, P.fSmall,
          installed ? (P.confirmUninstall ? L"Se quitará del equipo y del inicio de Windows"
                                          : L"Instalada \x00B7 sin permisos de administrador")
                    : L"Modo portátil \x00B7 instálala para dejarla fija",
          Rc(16, fy + 52, PW - 16, fy + 70), P.confirmUninstall ? t->danger : t->fg2, 0);
    TextR(c, P.fSmall, L"Ctrl+Alt+R activar \x00B7 Ctrl+Alt+RePág/AvPág radio",
          Rc(16, fy + 70, PW - 16, fy + 88), t->fg3, 0);

}

static void Paint(void)
{
    if (P.embedded) return;             /* lo pinta (y presenta) el centro de control */
    Render();
    Pop_Present(&P.pop, &P.cv);
}

/* ───────────────────────── Interacción ───────────────────────── */
static void Resize(void)
{
    if (P.embedded) { CC_SettingsResized(S(P.height)); InvalidateRect(P.hwnd, NULL, FALSE); return; }
    RECT wr;
    Pop_ContentRect(&P.pop, &wr);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(P.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    const int h = S(P.height);
    /* si el panel está abajo (junto a la barra de tareas), crece hacia arriba */
    const BOOL bottom = wr.bottom > (mi.rcWork.top + mi.rcWork.bottom) / 2;
    const int y = bottom ? wr.bottom - h : wr.top;
    Pop_SetBounds(&P.pop, wr.left, max((int)mi.rcWork.top, y), wr.right - wr.left, h);
    InvalidateRect(P.hwnd, NULL, FALSE);
}

static void SetTab(int tab)
{
    if (tab == P.tab) return;
    P.tab = tab;
    P.hover = P.pressed = H_NONE;
    Layout();
    Resize();
}

static BOOL s_dragVol;          /* el arrastre es el del volumen, no el del radio */

static void SliderTo(int x)
{
    if (s_dragVol) {
        const float sl = (float)S(46), sr = (float)S(PW - 46);
        const int v = (int)(max(0.0f, min(1.0f, ((float)x - sl) / (sr - sl))) * 100.0f + 0.5f);
        if (v != g_cfg.soundVol) { g_cfg.soundVol = v; Panel_Refresh(); }
        return;
    }
    const float sl = (float)S(26), sr = (float)S(PW - 26);
    float t = ((float)x - sl) / (sr - sl);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    const int r = TToRadius(t);
    if (r != g_cfg.radius) App_SetRadius(r, APPF_NOSAVE);
}

static void Activate(int hit)
{
    switch (hit) {
    case H_MASTER: App_SetEnabled(!g_cfg.enabled, 0); return;
    case H_TAB0:   SetTab(0); return;
    case H_TAB1:   SetTab(1); return;
    case H_TAB2:   SetTab(2); return;
    case H_BACK:   CC_ShowControls(); return;
    case H_EXIT:   App_Quit(); return;
    case H_INSTALL:
        if (!Inst_IsRunningInstalled()) { App_Install(); return; }
        if (!P.confirmUninstall) {
            P.confirmUninstall = TRUE;
            SetTimer(P.hwnd, TIMER_CONFIRM, 4000, NULL);
            InvalidateRect(P.hwnd, NULL, FALSE);
        } else {
            App_Uninstall();
        }
        return;
    }
    if (hit <= 0 || HIT_ITEM(hit) >= P.nitems) return;
    const ItemDef *d = P.items[HIT_ITEM(hit)].def;
    const int sub = HIT_SUB(hit);
    switch (d->key) {
    case K_RADIUS:   if (sub >= 1) App_SetRadius(kChips[sub - 1], 0); return;
    case K_HIDECAP:  App_SetHideCapture(!g_cfg.hideCapture); return;
    case K_STARTUP:  Inst_SetStartup(!Inst_IsStartup(), NULL); Panel_Refresh(); return;
    case K_BAR:      App_SetMenuBar(!g_cfg.menubar, FALSE); return;
    case K_CLOCK:    App_SetHideClock(!g_cfg.hideClock); return;
    case K_BATTPCT:  g_cfg.battPct = !g_cfg.battPct; Cfg_Save(); Bar_StyleChanged(); Panel_Refresh(); return;
    case K_DOCK:     App_SetDock(!g_cfg.dock, FALSE); return;
    case K_DOCKTB:   g_cfg.dockHideTaskbar = !g_cfg.dockHideTaskbar; Cfg_Save(); Dock_Apply(); Panel_Refresh(); return;
    case K_DHIDE:    g_cfg.dockAutoHide = sub; Cfg_Save(); Dock_ConfigChanged(); Panel_Refresh(); return;
    case K_DWINFULL: g_cfg.dockWinFull = !g_cfg.dockWinFull; Cfg_Save(); Dock_ConfigChanged(); Panel_Refresh(); return;
    case K_DBLUR:    g_cfg.dockBlur = sub;    Cfg_Save(); Dock_ConfigChanged(); Panel_Refresh(); return;
    case K_DOPAC:    g_cfg.dockOpacity = sub; Cfg_Save(); Dock_ConfigChanged(); Panel_Refresh(); return;
    case K_DICON:    g_cfg.dockIcon = sub;    Cfg_Save(); Dock_ConfigChanged(); Panel_Refresh(); return;
    case K_NOTCH:    App_SetNotch(!g_cfg.notch); return;
    case K_SHOTS:    App_SetCaptures(!g_cfg.captures); return;
    case K_MIRROR:   g_cfg.mirror = !g_cfg.mirror; break;
    case K_BANNERS:  g_cfg.hideBanners = !g_cfg.hideBanners; break;
    case K_SITEICON: g_cfg.siteIcons = !g_cfg.siteIcons; break;
    case K_EDGE:     g_cfg.edgeHover = !g_cfg.edgeHover; break;
    case K_SOUND:                       /* ‹ anterior · nombre: otra vez · siguiente › */
        if (sub == 0) g_cfg.sound = (g_cfg.sound + SOUND_COUNT - 1) % SOUND_COUNT;
        else if (sub == 2) g_cfg.sound = (g_cfg.sound + 1) % SOUND_COUNT;
        Cfg_Save(); Notch_PlaySound(); Panel_Refresh(); return;
    case K_STYLE:    g_cfg.floating = sub; break;
    case K_MATERIAL:
        g_cfg.material = sub;
        if (sub == MAT_GLASS && !g_cfg.dockBlur) g_cfg.dockBlur = 1;   /* vidrio también en el dock */
        break;
    case K_SIZE:     g_cfg.size = sub; break;
    case K_BOUNCE:   g_cfg.bounce = sub; break;
    case K_ACCENT:   g_cfg.accent = sub; break;
    case K_TEST:
        Notch_Show(NI_INFO, L"Así se ve tu notch", L"Arrástralo donde quieras", -1, TRUE);
        return;
    }
    App_NotchConfigChanged();
    /* previsualizar el cambio de diseño al momento */
    if (d->key >= K_STYLE && d->key <= K_ACCENT)
        Notch_Show(NI_INFO, L"Así se ve tu notch", d->key == K_BOUNCE ? L"Rebote actualizado" : L"Diseño actualizado", -1, TRUE);
}

static BOOL Animate(void)
{
    BOOL moving = FALSE;
    for (int k = 0; k < K_COUNT; ++k) {
        const float target = ToggleValue(k) ? 1.0f : 0.0f, d = target - P.sw[k];
        if (d > 0.01f || d < -0.01f) { P.sw[k] += d * 0.35f; moving = TRUE; }
        else P.sw[k] = target;
    }
    const float tm = g_cfg.enabled ? 1.0f : 0.0f, dm = tm - P.master;
    if (dm > 0.01f || dm < -0.01f) { P.master += dm * 0.35f; moving = TRUE; }
    else P.master = tm;
    return moving;
}

static void SnapSwitches(void)
{
    for (int k = 0; k < K_COUNT; ++k) P.sw[k] = ToggleValue(k) ? 1.0f : 0.0f;
    P.master = g_cfg.enabled ? 1.0f : 0.0f;
}

static BOOL IsSliderHit(int hit)
{
    if (hit <= 0 || hit >= H_MASTER || HIT_ITEM(hit) >= P.nitems || HIT_SUB(hit) != 0) return FALSE;
    const int type = P.items[HIT_ITEM(hit)].def->type;
    s_dragVol = type == IT_VOLUME;
    return type == IT_RADIUS || type == IT_VOLUME;
}

static LRESULT PanelInput(HWND h, UINT m, WPARAM w, LPARAM l);

static LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEWHEEL && m != WM_MOUSEHWHEEL) l = Pop_Mouse(&P.pop, l);
    switch (m) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        Paint();
        EndPaint(h, &ps);
        return 0;
    }

    case WM_NCHITTEST: return Pop_NcHit(&P.pop, l);
    case WM_POPFRAME:  Pop_Tick(&P.pop); return 0;

    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) Panel_Close();
        return 0;
    case WM_SETTINGCHANGE:      /* cambio de tema claro/oscuro o de acento */
        Theme_Load(&P.th);
        ApplyMaterial();
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)l;
        P.dpi = HIWORD(w);
        MakeResources();
        Pop_SetBounds(&P.pop, r->left + P.pop.margin, r->top + P.pop.margin, S(PW), S(P.height));
        return 0;
    }

    case WM_CLOSE:
        Panel_Close();
        return 0;

    case WM_DESTROY:
        Pop_Destroyed(&P.pop);
        Pop_FreeGlass(&P.glass);
        KillTimer(h, TIMER_ANIM);
        KillTimer(h, TIMER_CONFIRM);
        Canvas_Free(&P.cv);
        FreeFonts();
        if (P.icon) { DestroyIcon(P.icon); P.icon = NULL; }
        P.hwnd = NULL;
        P.closedAt = GetTickCount();
        return 0;
    }
    return PanelInput(h, m, w, l);
}

/* Ratón, teclado y temporizadores: los mismos en ventana propia o alojado. */
static LRESULT PanelInput(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_MOUSEMOVE: {
        const int x = (short)LOWORD(l), y = (short)HIWORD(l);
        if (!P.tracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            P.tracking = TrackMouseEvent(&tme);
        }
        if (P.dragging) { SliderTo(x); return 0; }
        const int hit = HitTest(x, y);
        if (hit != P.hover) { P.hover = hit; InvalidateRect(h, NULL, FALSE); }
        return 0;
    }

    case WM_MOUSELEAVE:
        P.tracking = FALSE;
        if (!P.dragging) { P.hover = H_NONE; InvalidateRect(h, NULL, FALSE); }
        return 0;

    case WM_LBUTTONDOWN:
        P.pressed = HitTest((short)LOWORD(l), (short)HIWORD(l));
        if (IsSliderHit(P.pressed)) {
            P.dragging = TRUE;
            SetCapture(h);
            SliderTo((short)LOWORD(l));
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_LBUTTONUP: {
        const int hit = HitTest((short)LOWORD(l), (short)HIWORD(l));
        const int pressed = P.pressed;
        P.pressed = H_NONE;
        if (P.dragging) {
            P.dragging = FALSE;
            ReleaseCapture();
            Cfg_Save();
            if (s_dragVol) Notch_PlaySound();       /* así suena con el volumen elegido */
        } else if (pressed != H_NONE && pressed == hit) {
            Activate(hit);
        }
        if (IsWindow(h)) InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_CAPTURECHANGED:
        if (P.dragging) { P.dragging = FALSE; Cfg_Save(); InvalidateRect(h, NULL, FALSE); }
        return 0;

    case WM_MOUSEWHEEL: {
        const BOOL up = GET_WHEEL_DELTA_WPARAM(w) > 0;
        if (P.hover > 0 && P.hover < H_MASTER && HIT_ITEM(P.hover) < P.nitems &&
            P.items[HIT_ITEM(P.hover)].def->type == IT_VOLUME) {      /* rueda sobre el volumen: de 5 en 5 */
            g_cfg.soundVol = max(0, min(100, g_cfg.soundVol + (up ? 5 : -5)));
            Cfg_Save(); Panel_Refresh(); Notch_PlaySound();
        } else if (P.tab == 0) App_SetRadius(g_cfg.radius + (up ? 1 : -1), 0);
        return 0;
    }

    case WM_KEYDOWN:
        switch (w) {
        case VK_ESCAPE: if (P.embedded) CC_ShowControls(); else PostMessageW(h, WM_CLOSE, 0, 0); break;
        case VK_TAB:    SetTab((P.tab + 1) % NTABS); break;
        case VK_LEFT: case VK_DOWN:  App_SetRadius(g_cfg.radius - 1, 0); break;
        case VK_RIGHT: case VK_UP:   App_SetRadius(g_cfg.radius + 1, 0); break;
        case VK_NEXT:  App_SetRadius(g_cfg.radius - 4, 0); break;
        case VK_PRIOR: App_SetRadius(g_cfg.radius + 4, 0); break;
        case VK_SPACE: App_SetEnabled(!g_cfg.enabled, 0); break;
        }
        return 0;

    case WM_TIMER:
        if (w == TIMER_ANIM) {
            if (!Animate()) KillTimer(h, TIMER_ANIM);
            InvalidateRect(h, NULL, FALSE);
        } else if (w == TIMER_CONFIRM) {
            KillTimer(h, TIMER_CONFIRM);
            P.confirmUninstall = FALSE;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;

    }
    return DefWindowProcW(h, m, w, l);
}

/* ───────────────────────── API ───────────────────────── */
void Panel_Register(void)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = PanelProc;
    wc.hInstance     = g_inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon         = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = PANEL_CLASS;
    RegisterClassExW(&wc);
}

/* Con la barra superior activa, los ajustes viven dentro del centro de control. */
int Panel_Embed(HWND host, UINT dpi, int tab, const PopGlass *glass, int ox, int oy)
{
    if (P.hwnd && !P.embedded) DestroyWindow(P.hwnd);     /* una ventana propia abierta: fuera */
    if (P.embedded && P.hwnd == host) {
        P.extGlass = glass; P.extX = ox; P.extY = oy;
        if (tab != P.tab) { P.tab = tab; P.hover = P.pressed = H_NONE; Layout(); }
        return S(P.height);
    }
    P.embedded = TRUE;
    P.hwnd = host;
    P.dpi = dpi;
    P.tab = tab;
    P.extGlass = glass; P.extX = ox; P.extY = oy;
    Theme_Load(&P.th);
    ApplyMaterial();
    MakeResources();
    Layout();
    SnapSwitches();
    P.hover = P.pressed = H_NONE;
    P.dragging = P.tracking = P.confirmUninstall = FALSE;
    return S(P.height);
}

void Panel_Unembed(void)
{
    if (!P.embedded) return;
    if (P.hwnd) { KillTimer(P.hwnd, TIMER_ANIM); KillTimer(P.hwnd, TIMER_CONFIRM); }
    if (P.dragging && GetCapture() == P.hwnd) ReleaseCapture();
    Canvas_Free(&P.cv);
    FreeFonts();
    if (P.icon) { DestroyIcon(P.icon); P.icon = NULL; }
    P.embedded = FALSE;
    P.hwnd = NULL;
    P.closedAt = GetTickCount();
}

BOOL Panel_IsEmbedded(void) { return P.embedded; }
BOOL Panel_IsTimer(WPARAM id) { return id == TIMER_ANIM || id == TIMER_CONFIRM; }

const Canvas *Panel_RenderEmbedded(void)
{
    if (!P.embedded) return NULL;
    Render();
    return P.cv.px ? &P.cv : NULL;
}

LRESULT Panel_HostInput(HWND h, UINT m, WPARAM w, LPARAM l) { return PanelInput(h, m, w, l); }

void Panel_ShowTab(int tab)
{
    if (Bar_IsOn()) { CC_OpenSettings(tab); return; }
    if (P.hwnd && !P.pop.closing) { SetTab(tab); SetForegroundWindow(P.hwnd); return; }
    if (P.hwnd) DestroyWindow(P.hwnd);      /* cerrándose: se reabre al momento */

    /* Anclar junto al icono de la bandeja (o al cursor si no se encuentra) */
    POINT anchor;
    RECT ir;
    NOTIFYICONIDENTIFIER nii = { sizeof(nii) };
    nii.hWnd = g_ctrl;
    nii.uID  = 1;
    if (g_ctrl && SUCCEEDED(Shell_NotifyIconGetRect(&nii, &ir))) {
        anchor.x = (ir.left + ir.right) / 2;
        anchor.y = (ir.top + ir.bottom) / 2;
    } else {
        GetCursorPos(&anchor);
    }
    HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);

    P.dpi = MonitorDpi(mon);
    P.tab = tab;
    Theme_Load(&P.th);
    ApplyMaterial();
    MakeResources();
    Layout();
    SnapSwitches();
    P.hover = P.pressed = H_NONE;
    P.dragging = P.tracking = P.confirmUninstall = FALSE;

    const RECT wa = mi.rcWork;
    const int w = S(PW), h = S(P.height), gap = S(12);
    const int x = anchor.x > (wa.left + wa.right) / 2 ? wa.right - w - gap : wa.left + gap;
    const int y = anchor.y > (wa.top + wa.bottom) / 2 ? wa.bottom - h - gap : wa.top + gap;

    const int top = max((int)wa.top, y);
    /* vidrio: lo que hay detrás, en toda la columna (el panel cambia de alto con las pestañas) */
    if (P.th.dark && g_cfg.material == MAT_GLASS) Pop_CaptureGlass(&P.glass, x, wa.top, w, wa.bottom - wa.top, S(28));
    P.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, PANEL_CLASS,
                             L"CornerRadius \x2014 Configuración", WS_POPUP,
                             x, top, w, h, NULL, NULL, g_inst, NULL);
    if (!P.hwnd) return;
    if (g_cfg.hideCapture) SetWindowDisplayAffinity(P.hwnd, WDA_EXCLUDEFROMCAPTURE);
    /* sale de la esquina más cercana a donde se pidió (bandeja, engrane, cursor) */
    const float ax = anchor.x > x + w / 2 ? 1.0f : 0.0f, ay = anchor.y > top + h / 2 ? 1.0f : 0.0f;
    Pop_Open(&P.pop, P.hwnd, S(18), (float)S(12), ax, ay);
    Pop_SetBounds(&P.pop, x, top, w, h);
    Paint();
    ShowWindow(P.hwnd, SW_SHOW);
    SetForegroundWindow(P.hwnd);
}

void Panel_Show(void)
{
    Panel_ShowTab(P.hwnd ? P.tab : 0);
}

void Panel_Toggle(void)
{
    if (Bar_IsOn()) { CC_OpenSettings(P.tab); return; }
    if (P.hwnd && !P.pop.closing) { Panel_Close(); return; }
    if (P.hwnd) return;                 /* el clic en la bandeja le quitó el foco: ya se está cerrando */
    /* el clic en la bandeja ya cerró el panel al quitarle el foco: no reabrir */
    if (GetTickCount() - P.closedAt < 300) return;
    Panel_Show();
}

void Panel_Close(void)
{
    if (P.embedded) { CC_CloseAll(); return; }
    if (P.hwnd) Pop_Close(&P.pop);
}

void Panel_Refresh(void)
{
    if (!P.hwnd) return;
    SetTimer(P.hwnd, TIMER_ANIM, 16, NULL);
    InvalidateRect(P.hwnd, NULL, FALSE);
}
