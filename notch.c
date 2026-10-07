/*
 * notch.c — la "isla" de OpenDock, estilo Dynamic Island.
 *
 * Una sola ventana layered que cambia de forma con resortes entre estados:
 *   PEEK    aviso breve (cambios de la app, capturas, notificaciones de Windows)
 *   MINI    pastilla diminuta que sigue al cursor por el borde superior, con imán
 *           al centro del monitor y a la posición guardada del notch
 *   QUICK   si dejas el cursor quieto arriba: contador + "Ocultar banners de Windows"
 *   CENTER  al hacer clic: baja un panel con las notificaciones de Windows
 *
 * El aspecto (material, tamaño, acento, rebote) sale de g_cfg y se cambia en el panel.
 */
#include "app.h"
#include "resource.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <math.h>

#define NOTCH_CLASS L"OpenDock.Notch"
#define WM_NOTCH_FRAME (WM_APP + 40)   /* fotograma del marcapasos */
#define BACK_SCALE  4      /* el fondo del vidrio se captura a 1/4 y se desenfoca */
#define TIMER_FRAME 1
#define HOLD_MS     2200
#define HOLD_NOTE   4500
#define HOLD_SHOT   3500
#define MAX_HITS    96
#define BODY_LINE   18     /* alto de línea del cuerpo de un aviso (fBody, 13 px) */
#define PEEK_H      54     /* aviso de Windows recién llegado, cerrado (lógico) */
#define PEEK_LINES  7      /* líneas como mucho al expandirlo: cabe el cuerpo entero (256) */

enum { M_HIDDEN, M_PEEK, M_MINI, M_QUICK, M_CENTER };
enum { A_BOTTOM, A_TOP, A_MIDDLE };
enum { HT_NONE, HT_Q_OPEN, HT_Q_MUTE, HT_C_CLEAR, HT_C_GEAR, HT_C_MUTE, HT_CARD = 100, HT_CARDX = 200, HT_ACT = 300 };
#define ACT_REPLY 3                          /* HT_ACT + tarjeta*4 + (0..2 botón · 3 responder) */
static int CardOf(int hit)
{
    if (hit >= HT_ACT)   return (hit - HT_ACT) / 4;
    if (hit >= HT_CARDX) return hit - HT_CARDX;
    if (hit >= HT_CARD)  return hit - HT_CARD;
    return -1;
}

typedef struct {
    DWORD bg, fg, fg2, fg3, card, cardHot, line, accent, switchOff;
    float bgA, scale, zeta;
    BOOL  light, hairline;
} Look;

typedef struct { RECT r; int id; } Hit;

typedef struct {
    int      icon, level;
    wchar_t  title[128], detail[256];
    BOOL     isNote;
    BOOL     isBell;            /* notificación ofuscada: solo campanita + contador */
    wchar_t  aumid[160];
    wchar_t  app[64];
    wchar_t  logo[MAX_PATH];
    LONGLONG arrival;
} Peek;

static struct {
    HWND    hwnd;
    Canvas  content, frame, thumb;
    HFONT   fTitle, fBody, fSmall, fHeader, fIcon, fIconSm, fTrash;
    int     fontKey;
    UINT    dpi;
    RECT    mon;
    Look    look;

    int     mode;
    BOOL    closing, dirty;
    int     W, H, anchor;       /* tamaño objetivo del cuerpo y anclaje del contenido */
    float   ax, vax;            /* centro X animado (pantalla) */
    int     tx;                 /* centro X objetivo */
    int     ay;                 /* borde superior de la isla flotante */
    BOOL    attached;
    float   w, h, vw, vh, fade, op;

    DWORD   hideAt, hold;
    BOOL    hover, down, dragging, quitOnHide, isCapture, clickThrough, active;
    POINT   downPt;
    int     downTx, downTop, downHit;
    wchar_t openPath[MAX_PATH];
    UINT    actMsg;             /* aviso con acción: al pulsarlo, este mensaje al controlador */
    WPARAM  actW;
    LARGE_INTEGER last, freq;

    int     winX, winY, winW, winH, cox, coy;
    Hit     hits[MAX_HITS];
    int     nhits, hot;
    int     scroll, maxScroll;  /* scroll: destino (rueda) */
    float   scrollF, vScroll;   /* scroll animado */
    DWORD   scrollTick;         /* último movimiento de la rueda */
    int     listH, cardStep, cardH;
    float   cs[WN_MAX], cv[WN_MAX];     /* escala animada de cada tarjeta */
    float   tw[WN_MAX], tv[WN_MAX];     /* ancho animado de la franja de papelera */
    float   ex[WN_MAX], exv[WN_MAX];    /* alto extra animado: texto completo y botones */
    RECT    cardRc[WN_MAX];             /* rectángulo dibujado de cada tarjeta (contenido) */
    HWND    reply, replyEdit;           /* caja de respuesta rápida */
    int     hoverCard;                  /* tarjeta señalada y desde cuándo */
    int     mx;                         /* x del cursor en el contenido (para la papelera) */
    DWORD   hoverSince;
    LONGLONG replyId;
    HFONT   fReply;
    HBRUSH  replyBrush;
    Canvas  card;               /* tarjeta suelta (durante su animación de salida) */
    POINT   dwellPt;
    DWORD   dwellSince;
    Peek    peek;
    float   pex, pexv;          /* alto extra animado del aviso recién llegado (hover) */
    DWORD   peekSince;          /* desde cuándo está el cursor encima (0 = fuera) */

    /* borrado animado: la tarjeta sale hacia la derecha y las de abajo suben */
    LONGLONG outId;             /* 0 = ninguna · -1 = todas ("Borrar") */
    float   outT;
    float   shift, vshift;      /* desplazamiento extra de las tarjetas bajo la borrada */
    int     shiftFrom;

    Canvas  back;               /* vidrio: lo que hay detrás, reducido y desenfocado */
    DWORD   ringStart;          /* cuándo empieza a sonar la campanita */
    int     hz;                 /* frecuencia del monitor: ritmo de animación */
    Canvas  bell;               /* glifo de campana a escala 1, para rotarlo con filtrado */
    int     bellPx;
    int     unread;             /* notificaciones ofuscadas sin ver */
    DWORD   backTick;
    int     backOX, backOY;     /* origen de la captura respecto a la ventana */
} N;

/* Tarjetas ya dibujadas a escala 1: el scroll y el hover solo las escalan. */
typedef struct { Canvas cv; LONGLONG id; BOOL hot; int stamp; } CardCache;
static CardCache s_cards[WN_MAX];
static int       s_cardStamp = 1;     /* se incrementa para invalidarlas todas */

static int NS(int v) { return (int)(v * (float)N.dpi / 96.0f * N.look.scale + 0.5f); }
static float PillW(void);
static int TopEdge(void);
static float PillH(void);

/* ───────────────────────── Aspecto ───────────────────────── */
static void LoadLook(void)
{
    static const float kScale[3] = { 0.88f, 1.0f, 1.14f };
    static const float kZeta[3]  = { 0.85f, 0.62f, 0.40f };
    Theme th;
    Theme_Load(&th);
    Look *l = &N.look;
    ZeroMemory(l, sizeof(*l));
    l->scale = kScale[max(0, min(2, g_cfg.size))];
    l->zeta  = kZeta[max(0, min(2, g_cfg.bounce))];
    l->bgA   = 1.0f;

    if (g_cfg.material == MAT_SYSTEM && !th.dark) {
        l->light = l->hairline = TRUE;
        l->bg = 0xF2F2F7; l->fg = 0x000000; l->fg2 = 0x6C6C70; l->fg3 = 0xA1A1A6;
        l->card = 0xFFFFFF; l->cardHot = 0xE5E5EA; l->line = 0xD1D1D6; l->switchOff = 0xE3E3E8;
    } else {
        const BOOL oled = g_cfg.material == MAT_OLED;
        l->hairline = !oled;
        l->bg = oled ? 0x000000 : 0x1C1C1E;
        l->bgA = 1.0f;
        l->fg = 0xFFFFFF; l->fg2 = 0xAEAEB2; l->fg3 = 0x6E6E73;
        l->card = oled ? 0x1C1C1E : 0x2C2C2E; l->cardHot = oled ? 0x2C2C2E : 0x3A3A3C;
        l->line = 0x38383A; l->switchOff = 0x39393D;
    }
    const int a = max(0, min(ACCENT_COUNT - 1, g_cfg.accent));
    l->accent = a ? kAccentPresets[a] : (l->light ? th.accent : th.accentOnBlack);
}

static void MakeFonts(void)
{
    const int key = (int)N.dpi * 10 + max(0, min(2, g_cfg.size));
    if (N.fTitle && N.fontKey == key) return;
    HFONT *all[] = { &N.fTitle, &N.fBody, &N.fSmall, &N.fHeader, &N.fIcon, &N.fIconSm, &N.fTrash };
    for (int i = 0; i < 7; ++i) if (*all[i]) DeleteObject(*all[i]);
    /* gris (no ClearType): el texto se compone sobre alpha por píxel */
    LPCWSTR ui = Gfx_UiFace(), ic = Gfx_IconFace();
    N.fTitle  = Gfx_Font(ui, NS(14), FW_BOLD,     ANTIALIASED_QUALITY);
    N.fBody   = Gfx_Font(ui, NS(13), FW_SEMIBOLD, ANTIALIASED_QUALITY);   /* fino se leía mal */
    N.fSmall  = Gfx_Font(ui, NS(12), FW_NORMAL,   ANTIALIASED_QUALITY);
    N.fHeader = Gfx_Font(ui, NS(17), FW_SEMIBOLD, ANTIALIASED_QUALITY);
    N.fIcon   = Gfx_Font(ic, NS(16), FW_NORMAL,   ANTIALIASED_QUALITY);
    N.fIconSm = Gfx_Font(ic, NS(11), FW_NORMAL,   ANTIALIASED_QUALITY);
    N.fTrash  = Gfx_Font(ic, NS(14), FW_NORMAL,   ANTIALIASED_QUALITY);
    N.fontKey = key;
}

/* ───────────────────────── Piezas de dibujo ───────────────────────── */
static void AddHit(int l, int t, int r, int b, int id)
{
    if (N.nhits >= MAX_HITS) return;
    SetRect(&N.hits[N.nhits].r, l, t, r, b);
    N.hits[N.nhits++].id = id;
}

static int HitAt(int x, int y)
{
    POINT p = { x - N.cox, y - N.coy };
    for (int i = N.nhits - 1; i >= 0; --i)
        if (PtInRect(&N.hits[i].r, p)) return N.hits[i].id;
    return HT_NONE;
}

static void Text(Canvas *c, HFONT f, LPCWSTR s, int x, int y, int w, int h, DWORD rgb, UINT fmt)
{
    if (w > 0) Gfx_Text(c, f, s, x, y, w, h, rgb, fmt | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

/* El motivo de la app: una esquina redondeada trazada. */
static void DrawCornerGlyph(Canvas *c, float x, float y, float g, DWORD rgb)
{
    const float th = (float)NS(2), o = th;
    for (int py = (int)y; py < (int)(y + g); ++py)
        for (int px = (int)x; px < (int)(x + g); ++px) {
            const float d = fabsf(Gfx_SdRRect(px + 0.5f, py + 0.5f, x + o, y + o, g * 2, g * 2, g * 0.75f));
            Gfx_Blend(c, px, py, rgb, Gfx_Cov(d - th * 0.5f));
        }
}

/* Icono pequeño y nítido, centrado en una caja de box×box: se pide al sistema a su
 * tamaño exacto y se compone píxel a píxel respetando la transparencia. */
static int IconPx(void) { return NS(18); }

/* Compone un mapa de bits de 32 bpp (alfa premultiplicado o no, según venga) en (x, y),
 * centrado en s×s. shape: 0 tal cual · 1 esquinas de app · 2 círculo (fotos de perfil). */
static BOOL BlitIcon(Canvas *c, HBITMAP icon, int x, int y, int s, int shape, float alpha)
{
    BITMAP bm;
    if (!icon || !GetObjectW(icon, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmWidth > 512 || abs(bm.bmHeight) > 512) return FALSE;
    const int w = bm.bmWidth, h = abs(bm.bmHeight);
    DWORD *px = (DWORD *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)w * h * 4);
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;     /* top-down, sea como sea el original */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    if (!px || GetDIBits(c->dc, icon, 0, h, px, &bi, DIB_RGB_COLORS) != h) { if (px) HeapFree(GetProcessHeap(), 0, px); return FALSE; }
    /* ¿alpha premultiplicado? (algunas apps lo dan así y otras no) */
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
            if (premult && anyAlpha && a < 255) {
                rgb = (DWORD)min(255, (int)((v >> 16) & 255) * 255 / a) << 16
                    | (DWORD)min(255, (int)((v >> 8) & 255) * 255 / a) << 8
                    | (DWORD)min(255, (int)(v & 255) * 255 / a);
            }
            float k = a / 255.0f * alpha;
            if (shape == 1) k *= Gfx_Cov(Gfx_SdRRect(xx + 0.5f, yy + 0.5f, 0, 0, (float)w, (float)h, min(w, h) * 0.24f));
            else if (shape == 2) k *= Gfx_Cov(hypotf(xx + 0.5f - w * 0.5f, yy + 0.5f - h * 0.5f) - min(w, h) * 0.5f);
            Gfx_Blend(c, ox + xx, oy + yy, rgb, k);
        }
    HeapFree(GetProcessHeap(), 0, px);
    return TRUE;
}

/* Icono del aviso. Los de sitios web, como en Windows, traen dos imágenes: la del sitio y,
 * a veces, la del remitente (foto de perfil). Con las dos, como en iOS: la foto redonda y el
 * icono del sitio de insignia en su esquina; con una, esa; sin ninguna, la inicial. */
static void DrawAppIcon(Canvas *c, int bx, int by, int box, LPCWSTR aumid, LPCWSTR app, LPCWSTR logo, float alpha)
{
    const int s = IconPx(), x = bx + (box - s) / 2, y = by + (box - s) / 2;
    const BOOL site = Wn_IsSite(aumid);
    HBITMAP photo = g_cfg.siteIcons && logo && logo[0] ? Wn_LogoIcon(logo, s) : NULL;
    if (photo && BlitIcon(c, photo, x, y, s, 2, alpha)) {
        const int bs = max(8, s * 9 / 20), bxx = x + s - bs + NS(3), byy = y + s - bs + NS(3);
        HBITMAP badge = aumid && aumid[0] ? Wn_AppIcon(aumid, bs) : NULL;
        if (badge) {
            Gfx_FillCircle(c, bxx + bs * 0.5f, byy + bs * 0.5f, bs * 0.5f + (float)NS(2) * 0.75f, N.look.bg, alpha);   /* aro */
            BlitIcon(c, badge, bxx, byy, bs, site ? 1 : 0, alpha);
        }
        return;
    }
    HBITMAP icon = aumid && aumid[0] ? Wn_AppIcon(aumid, s) : NULL;
    if (BlitIcon(c, icon, x, y, s, site ? 1 : 0, alpha)) return;
    /* sin icono: cuadrado redondeado con la inicial */
    Gfx_FillRRect(c, (float)x, (float)y, (float)s, (float)s, s * 0.28f, N.look.accent, alpha);
    wchar_t ini[2] = { app && app[0] ? app[0] : L'?', 0 };
    Gfx_Text(c, N.fSmall, ini, x, y, s, s, Gfx_Mix(N.look.bg, 0xFFFFFF, alpha), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void Ago(LONGLONG ft, wchar_t *out)
{
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    const LONGLONG now = (LONGLONG)nowFt.dwHighDateTime << 32 | nowFt.dwLowDateTime;
    const LONGLONG s = (now - ft) / 10000000;
    if (s < 60)          lstrcpyW(out, L"ahora");
    else if (s < 3600)   wsprintfW(out, L"%d min", (int)(s / 60));
    else if (s < 86400)  wsprintfW(out, L"%d h", (int)(s / 3600));
    else if (s < 172800) lstrcpyW(out, L"ayer");
    else {
        FILETIME f = { (DWORD)ft, (DWORD)(ft >> 32) }, lf;
        SYSTEMTIME st;
        FileTimeToLocalFileTime(&f, &lf);
        FileTimeToSystemTime(&lf, &st);
        wsprintfW(out, L"%d/%d", st.wDay, st.wMonth);
    }
}

static BOOL BeginContent(int w, int h)
{
    N.W = w;
    N.H = h;
    N.nhits = 0;
    Canvas_Free(&N.content);
    if (!Canvas_Init(&N.content, w, h)) return FALSE;
    Canvas_Clear(&N.content, N.look.bg);
    return TRUE;
}

/* ───────────────────────── Contenido por estado ───────────────────────── */
static void RenderPeekAlert(void)
{
    const Peek *p = &N.peek;
    DWORD iconColor = N.look.accent, detailColor = N.look.fg2;
    switch (p->icon) {
    case NI_RADIUS: detailColor = N.look.fg; break;
    case NI_ON:     iconColor = detailColor = 0x30D158; break;
    case NI_OFF:    iconColor = detailColor = 0x8E8E93; break;
    case NI_CHECK:  iconColor = 0x30D158; break;
    case NI_WARN:   iconColor = detailColor = 0xFF9F0A; break;
    }

    const int padL = NS(14), padR = NS(16), gap = NS(10), gap2 = NS(18), barW = NS(72);
    const int g = N.thumb.dc ? N.thumb.w : NS(18);
    const int tw = Gfx_TextWidth(N.fTitle, p->title);
    const int dw = p->detail[0] ? Gfx_TextWidth(N.fBody, p->detail) : 0;
    const int rowW = g + gap + tw + (p->level >= 0 ? gap2 + barW : 0) + (dw ? gap2 + dw : 0);
    const int monW = N.mon.right - N.mon.left;

    if (!BeginContent(min(max(padL + rowW + padR, NS(190)), monW - NS(40)), NS(38))) return;
    Canvas *c = &N.content;
    int x = max(padL, (N.W - rowW) / 2);
    const int cy = N.H / 2;

    if (N.thumb.dc) {
        /* miniatura de la captura con esquinas redondeadas sobre el fondo */
        const int ty = cy - N.thumb.h / 2;
        GdiFlush();
        for (int yy = 0; yy < N.thumb.h; ++yy)
            for (int xx = 0; xx < N.thumb.w; ++xx) {
                const float cov = Gfx_Cov(Gfx_SdRRect(xx + 0.5f, yy + 0.5f, 0, 0,
                                                      (float)N.thumb.w, (float)N.thumb.h, (float)NS(5)));
                Gfx_Blend(c, x + xx, ty + yy, N.thumb.px[yy * N.thumb.w + xx] & 0xFFFFFF, cov);
            }
    } else if (p->icon == NI_RADIUS || p->icon == NI_ON || p->icon == NI_OFF) {
        DrawCornerGlyph(c, (float)x, (float)(cy - g / 2), (float)g, iconColor);
    } else {
        const wchar_t *glyph = p->icon == NI_CHECK ? L"\xE73E" : p->icon == NI_WARN ? L"\xE7BA" : L"\xE946";
        Gfx_Text(c, N.fIcon, glyph, x, 0, g, N.H, iconColor, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    x += g + gap;
    Text(c, N.fTitle, p->title, x, 0, tw + 2, N.H, N.look.fg, 0);
    x += tw;
    if (p->level >= 0) {
        x += gap2;
        const float fill = (float)min(p->level, 1000) / 1000.0f;
        Gfx_FillRRect(c, (float)x, (float)(cy - NS(2)), (float)barW, (float)NS(4), (float)NS(2), N.look.line, 1.0f);
        Gfx_FillRRect(c, (float)x, (float)(cy - NS(2)), barW * fill, (float)NS(4), (float)NS(2), iconColor, 1.0f);
        x += barW;
    }
    if (dw) Text(c, N.fBody, p->detail, x + gap2, 0, min(dw + 2, N.W - x - gap2 - padR / 2), N.H, detailColor, 0);
}

/* Alto extra que necesita el aviso recién llegado para enseñar su texto entero. */
static float PeekNeed(void)
{
    const int tx = NS(12) + NS(30) + NS(10), right = NS(360) - NS(16);
    if (!N.peek.detail[0] || !N.content.dc) return 0.0f;
    RECT r = { 0, 0, right - tx, 0 };
    HGDIOBJ o = SelectObject(N.content.dc, N.fBody);
    DrawTextW(N.content.dc, N.peek.detail, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
    SelectObject(N.content.dc, o);
    return (float)max(0, min((int)(r.bottom - r.top), NS(BODY_LINE) * PEEK_LINES) - NS(BODY_LINE));
}

static void RenderPeekNote(void)
{
    const Peek *p = &N.peek;
    const int extra = (int)(N.pex + 0.5f);
    if (!BeginContent(NS(360), NS(PEEK_H) + extra)) return;
    Canvas *c = &N.content;
    const int box = NS(30), bx = NS(12), tx = bx + box + NS(10), right = N.W - NS(16);
    wchar_t ago[32];
    Ago(p->arrival, ago);
    const int aw = Gfx_TextWidth(N.fSmall, ago);

    DrawAppIcon(c, bx, (NS(PEEK_H) - box) / 2, box, p->aumid, p->app, p->logo, 1.0f);
    Text(c, N.fTitle, p->title[0] ? p->title : p->app, tx, NS(8), right - aw - NS(8) - tx, NS(20), N.look.fg, 0);
    Text(c, N.fSmall, ago, right - aw, NS(8), aw + 1, NS(20), N.look.fg3, 0);
    if (extra <= 1)
        Text(c, N.fBody, p->detail[0] ? p->detail : p->app, tx, NS(28), right - tx, NS(BODY_LINE), N.look.fg2, 0);
    else    /* con el cursor encima: el texto completo se descubre según crece la isla */
        Gfx_Text(c, N.fBody, p->detail, tx, NS(28), right - tx, NS(BODY_LINE) + extra, N.look.fg2,
                 DT_WORDBREAK | DT_END_ELLIPSIS | DT_EDITCONTROL | DT_NOPREFIX);
}


static void DrawBell(Canvas *c, float px, float py, float g, float angle, DWORD rgb)
{
    /* el glifo se dibuja una vez por tamaño (blanco sobre negro = cobertura) */
    const int S = (int)(g * 1.6f) + 2;
    if (N.bellPx != S || !N.bell.dc) {
        Canvas_Free(&N.bell);
        if (!Canvas_Init(&N.bell, S, S)) return;
        Canvas_Clear(&N.bell, 0);
        HFONT f = Gfx_Font(Gfx_IconFace(), (int)(g * 1.05f), FW_NORMAL, ANTIALIASED_QUALITY);
        Gfx_Text(&N.bell, f, L"\xEA8F", 0, 0, S, S, 0xFFFFFF, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DeleteObject(f);
        GdiFlush();
        N.bellPx = S;
    }
    /* pivote: arriba al centro del glifo, como una campana colgada */
    const float pvx = S * 0.5f, pvy = S * 0.5f - g * 0.45f;
    const float cs = cosf(angle), sn = sinf(angle);
    const float ox = px, oy = py + g * 0.05f;       /* pivote en el lienzo destino */
    GdiFlush();
    const int R = S;
    for (int y = max(0, (int)(oy - R * 0.3f)); y < min(c->h, (int)(oy + R)); ++y)
        for (int x = max(0, (int)(ox - R)); x < min(c->w, (int)(ox + R)); ++x) {
            const float dx = x + 0.5f - ox, dy = y + 0.5f - oy;
            const float sx = dx * cs + dy * sn + pvx - 0.5f, sy = -dx * sn + dy * cs + pvy - 0.5f;
            const int ix = (int)floorf(sx), iy = (int)floorf(sy);
            if (ix < 0 || iy < 0 || ix >= S - 1 || iy >= S - 1) continue;
            const float tx = sx - ix, ty = sy - iy;
            const DWORD *p = N.bell.px;
            const float a = ((p[iy * S + ix] & 255) * (1 - tx) + (p[iy * S + ix + 1] & 255) * tx) * (1 - ty)
                          + ((p[(iy + 1) * S + ix] & 255) * (1 - tx) + (p[(iy + 1) * S + ix + 1] & 255) * tx) * ty;
            if (a > 1.0f) Gfx_Blend(c, x, y, rgb, a / 255.0f);
        }
}

static float RingAngle(void)
{
    const LONG ms = (LONG)(GetTickCount() - N.ringStart);
    if (ms < 0 || ms > 2300) return 0;
    float t = ms / 1000.0f;
    if (t > 1.1f) t -= 1.1f;                    /* segundo toque */
    return 0.55f * sinf(t * 21.0f) * expf(-t * 3.2f);
}

static void DrawLine(Canvas *c, float ax, float ay, float bx, float by, float th, DWORD rgb)
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

/* Interruptor de "notificaciones ofuscadas": campanita; tachada y en acento si está activo. */
static void DrawMuteButton(Canvas *c, int x, int y, int d, int id)
{
    const BOOL on = g_cfg.hideBanners;
    const float cx = x + d * 0.5f, cy = y + d * 0.5f, g = d * 0.4f;
    Gfx_FillCircle(c, cx, cy, d * 0.5f, on ? N.look.accent : N.look.card, 1.0f);
    const DWORD fg = on ? 0xFFFFFF : N.look.fg2;
    DrawBell(c, cx, cy - g * 0.5f, g, RingAngle(), fg);
    if (on) {
        const float o = d * 0.27f;
        DrawLine(c, cx - o, cy - o, cx + o, cy + o, (float)NS(4), N.look.accent);  /* hueco */
        DrawLine(c, cx - o, cy - o, cx + o, cy + o, (float)NS(2), fg);
    }
    AddHit(x, y, x + d, y + d, id);
}

static void RenderPeekBell(void)
{
    if (!BeginContent(NS(112), NS(34))) return;
    Canvas *c = &N.content;
    const float g = (float)NS(14);
    DrawBell(c, (float)NS(30), (N.H - g) * 0.5f, g, RingAngle(), N.look.fg);

    wchar_t buf[16];
    wsprintfW(buf, L"%d", max(1, N.unread));
    const int pw = max(NS(24), Gfx_TextWidth(N.fSmall, buf) + NS(14)), ph = NS(20);
    const int px = N.W - NS(14) - pw, py = (N.H - ph) / 2;
    Gfx_FillRRect(c, (float)px, (float)py, (float)pw, (float)ph, ph * 0.5f, N.look.accent, 1.0f);
    Gfx_Text(c, N.fSmall, buf, px, py, pw, ph, 0xFFFFFF, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void RenderQuick(void)
{
    if (!BeginContent(NS(272), NS(44))) return;
    Canvas *c = &N.content;
    const int n = g_cfg.mirror ? Wn_Count() : 0;
    Text(c, N.fTitle, L"Notificaciones", NS(18), 0, N.W - NS(120), N.H, N.look.fg, 0);

    wchar_t buf[16];
    wsprintfW(buf, L"%d", n);
    const int pw = max(NS(24), Gfx_TextWidth(N.fSmall, buf) + NS(14)), ph = NS(22);
    const int px = N.W - NS(14) - pw, py = (N.H - ph) / 2;
    const int d = NS(28), mx = px - NS(8) - d;
    AddHit(0, 0, mx - NS(4), N.H, HT_Q_OPEN);
    AddHit(px - NS(4), 0, N.W, N.H, HT_Q_OPEN);
    DrawMuteButton(c, mx, (N.H - d) / 2, d, HT_Q_MUTE);

    /* contador en pastilla de acento (o "0" discreto) */
    Gfx_FillRRect(c, (float)px, (float)py, (float)pw, (float)ph, ph * 0.5f, n ? N.look.accent : N.look.card, 1.0f);
    Gfx_Text(c, N.fSmall, buf, px, py, pw, ph, n ? 0xFFFFFF : N.look.fg2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

#define ACT_ROW  40         /* fila de botones de la tarjeta expandida (lógico) */
#define BODY_MAX 4          /* líneas de texto como mucho al expandir */

/* Pastillas de la tarjeta: "Responder" (si admite texto) y los botones del aviso. */
static int ActionPills(const WinNote *w, int tx, int right, int py, RECT *r, int *k)
{
    int n = 0, x = tx;
    const int h = NS(26);
    for (int pass = 0; pass < 2; ++pass)
        for (int a = pass ? 0 : -1; pass ? a < w->nact : a < 0; ++a) {
            LPCWSTR label;
            int id;
            if (!pass) { if (!w->hasInput) break; label = L"Responder"; id = ACT_REPLY; }
            else {
                if (w->act[a].input[0]) continue;         /* el "enviar" de la caja: lo hace Responder */
                label = w->act[a].label; id = a;
            }
            const int pw = Gfx_TextWidth(N.fSmall, label) + NS(24);
            if (x + pw > right) return n;
            SetRect(&r[n], x, py, x + pw, py + h);
            k[n++] = id;
            x += pw + NS(8);
        }
    return n;
}

static BOOL HasPills(const WinNote *w)
{
    if (w->hasInput) return TRUE;
    for (int a = 0; a < w->nact; ++a) if (!w->act[a].input[0]) return TRUE;
    return FALSE;
}

/* Alto extra que necesita la tarjeta i para enseñarlo todo. */
static float NeedExtra(int i, int cw)
{
    const WinNote *w = Wn_Get(i);
    const int tx = NS(14) + NS(30) + NS(10), right = cw - NS(14);
    int extra = 0;
    if (w->body[0]) {
        RECT r = { 0, 0, right - tx, 0 };
        HGDIOBJ o = SelectObject(N.content.dc, N.fBody);
        DrawTextW(N.content.dc, w->body, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
        SelectObject(N.content.dc, o);
        extra = max(0, min((int)(r.bottom - r.top), NS(BODY_LINE) * BODY_MAX) - NS(BODY_LINE));
    }
    if (HasPills(w)) extra += NS(ACT_ROW);
    return (float)extra;
}

/* Dibuja la tarjeta i en (x, y) del lienzo c (a escala 1); h = alto actual (con extra). */
static void DrawCard(Canvas *c, int x, int y, int cw, int h, int i, float alpha, BOOL hot)
{
    const Look *L = &N.look;
    const WinNote *w = Wn_Get(i);
    const int cardH = N.cardH, box = NS(30), tx = x + NS(14) + box + NS(10), right = x + cw - NS(14);
    wchar_t ago[32];
    Gfx_FillRRect(c, (float)x, (float)y, (float)cw, (float)h, (float)NS(16), L->card, alpha);
    DrawAppIcon(c, x + NS(14), y + (cardH - box) / 2, box, w->aumid, w->app, w->logo, alpha);
    Ago(w->arrival, ago);
    const int aw = Gfx_TextWidth(N.fSmall, ago);
    const DWORD cardBg = Gfx_Mix(L->bg, L->card, alpha);
    Text(c, N.fSmall, w->app, tx, y + NS(9), right - aw - NS(8) - tx, NS(16), Gfx_Mix(cardBg, L->fg2, alpha), 0);
    (void)hot;
    Text(c, N.fSmall, ago, right - aw, y + NS(9), aw + 1, NS(16), Gfx_Mix(cardBg, L->fg3, alpha), 0);
    Text(c, N.fTitle, w->title[0] ? w->title : w->app, tx, y + NS(26), right - tx, NS(20), Gfx_Mix(cardBg, L->fg, alpha), 0);

    const int extra = h - cardH, pills = HasPills(w) ? NS(ACT_ROW) : 0;
    if (extra <= 1) {
        Text(c, N.fBody, w->body, tx, y + NS(46), right - tx, NS(BODY_LINE), Gfx_Mix(cardBg, L->fg2, alpha), 0);
        return;
    }
    /* expandida: el texto completo se va descubriendo según crece la tarjeta */
    const int bodyH = NS(BODY_LINE) + max(0, extra - pills);
    Gfx_Text(c, N.fBody, w->body, tx, y + NS(46), right - tx, bodyH, Gfx_Mix(cardBg, L->fg2, alpha),
             DT_WORDBREAK | DT_END_ELLIPSIS | DT_EDITCONTROL);
    if (!pills || extra < pills * 0.5f) return;
    const float show = min(1.0f, (extra - pills * 0.5f) / (pills * 0.5f)) * alpha;
    RECT pr[WN_ACTIONS + 1];
    int pk[WN_ACTIONS + 1];
    const int n = ActionPills(w, tx, right, y + h - NS(ACT_ROW) + NS(4), pr, pk);
    for (int p = 0; p < n; ++p) {
        const BOOL on = N.hot == HT_ACT + i * 4 + pk[p];
        const DWORD bg = Gfx_Mix(L->card, L->fg, on ? 0.20f : 0.10f);
        Gfx_FillRRect(c, (float)pr[p].left, (float)pr[p].top, (float)(pr[p].right - pr[p].left), (float)(pr[p].bottom - pr[p].top),
                      (pr[p].bottom - pr[p].top) * 0.5f, bg, show);
        Gfx_Text(c, N.fSmall, pk[p] == ACT_REPLY ? L"Responder" : w->act[pk[p]].label, pr[p].left, pr[p].top,
                 pr[p].right - pr[p].left, pr[p].bottom - pr[p].top, Gfx_Mix(Gfx_Mix(cardBg, bg, show), L->fg, show),
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

/* Franja de papelera: entra desde el borde derecho de la tarjeta, a todo su alto, con las
 * esquinas de la propia tarjeta; sobre ella se ensancha y se enciende. Se dibuja encima
 * de la tarjeta ya escalada (k), así la caché de tarjetas no cambia. */
#define TRASH_W     40
#define TRASH_W_HOT 58
static void DrawTrash(Canvas *c, float ccx, float ccy, int cw, int cardH, float k, float tw, float alpha, BOOL zoneHot)
{
    const float R = ccx + cw * k * 0.5f, T = ccy - cardH * k * 0.5f, h = cardH * k, w = tw * k;
    const DWORD red = zoneHot ? 0xFF453A : 0xE5443C;
    Gfx_FillRRect(c, R - w, T, w, h, NS(16) * k, red, alpha * min(1.0f, tw / NS(14)));
    /* icono: aparece cuando la franja ya tiene sitio y crece un poco al encenderse */
    const float show = max(0.0f, min(1.0f, (tw - NS(20)) / NS(12)));
    if (show <= 0) return;
    const int ib = NS(22), ix = (int)(R - w * 0.5f - ib * 0.5f), iy = (int)(ccy - ib * 0.5f);
    Gfx_Text(c, zoneHot ? N.fIcon : N.fTrash, L"\xE74D", ix, iy, ib, ib,
             Gfx_Mix(red, 0xFFFFFF, show * alpha), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* Copia src escalado por k alrededor de (cx, cy): bilineal en punto fijo (8 bits),
 * o copia directa de filas cuando k ≈ 1. */
static void BlitScaled(Canvas *dst, const Canvas *src, float cx, float cy, float k)
{
    GdiFlush();
    if (fabsf(k - 1.0f) < 0.002f) {
        const int ox = (int)lroundf(cx - src->w * 0.5f), oy = (int)lroundf(cy - src->h * 0.5f);
        const int x0 = max(0, ox), x1 = min(dst->w, ox + src->w);
        if (x1 <= x0) return;
        for (int y = max(0, oy); y < min(dst->h, oy + src->h); ++y)
            CopyMemory(&dst->px[y * dst->w + x0], &src->px[(y - oy) * src->w + (x0 - ox)], (x1 - x0) * 4);
        return;
    }
    const float hw = src->w * k * 0.5f, hh = src->h * k * 0.5f, inv = 1.0f / k;
    const int x0 = max(0, (int)floorf(cx - hw)), x1 = min(dst->w, (int)ceilf(cx + hw));
    const int y0 = max(0, (int)floorf(cy - hh)), y1 = min(dst->h, (int)ceilf(cy + hh));
    const int W = src->w, H = src->h;
    for (int y = y0; y < y1; ++y) {
        const int sy = (int)(((y + 0.5f - (cy - hh)) * inv - 0.5f) * 256.0f);
        const int iy = sy >> 8, fy = sy & 255;
        if (iy < -1 || iy >= H) continue;
        const DWORD *ra = &src->px[max(0, iy) * W], *rb = &src->px[min(H - 1, iy + 1) * W];
        DWORD *out = &dst->px[y * dst->w];
        int sx = (int)(((x0 + 0.5f - (cx - hw)) * inv - 0.5f) * 256.0f);
        const int step = (int)(inv * 256.0f);
        for (int x = x0; x < x1; ++x, sx += step) {
            const int ix = sx >> 8, fx = sx & 255;
            if (ix < -1 || ix >= W) continue;
            const int xa = max(0, ix), xb = min(W - 1, ix + 1);
            const DWORD a = ra[xa], b = ra[xb], c = rb[xa], d = rb[xb];
            /* rojo y azul juntos, verde aparte */
            const DWORD rbTop = ((a & 0xFF00FF) * (256 - fx) + (b & 0xFF00FF) * fx) >> 8 & 0xFF00FF;
            const DWORD rbBot = ((c & 0xFF00FF) * (256 - fx) + (d & 0xFF00FF) * fx) >> 8 & 0xFF00FF;
            const DWORD gTop  = ((a & 0x00FF00) * (256 - fx) + (b & 0x00FF00) * fx) >> 8 & 0x00FF00;
            const DWORD gBot  = ((c & 0x00FF00) * (256 - fx) + (d & 0x00FF00) * fx) >> 8 & 0x00FF00;
            out[x] = ((rbTop * (256 - fy) + rbBot * fy) >> 8 & 0xFF00FF) | ((gTop * (256 - fy) + gBot * fy) >> 8 & 0x00FF00);
        }
    }
}

static void RenderCenter(void)
{
    const int pad = NS(18), headerH = NS(56), cardH = NS(70), gap = NS(12), footH = NS(14);
    const int count = g_cfg.mirror ? Wn_Count() : 0;
    float extraAll = 0;
    for (int i = 0; i < min(count, WN_MAX); ++i) extraAll += N.ex[i];
    const int fullH = count ? count * (cardH + gap) - gap + pad + (int)extraAll : NS(120);
    /* como mucho seis tarjetas a la vista; la que se expande hace crecer el panel */
    const int listH = count ? min(fullH, 6 * (cardH + gap) - gap + pad) : NS(120);
    N.maxScroll = max(0, fullH - listH);
    N.scroll = max(0, min(N.scroll, N.maxScroll));
    N.scrollF = max(0.0f, min(N.scrollF, (float)N.maxScroll));

    if (!BeginContent(NS(384), headerH + listH + footH)) return;
    Canvas *c = &N.content;
    const Look *L = &N.look;

    /* lista (la cabecera y el pie se pintan encima: hacen de recorte) */
    if (!count) {
        Text(c, N.fBody, g_cfg.mirror ? L"Sin notificaciones" : L"Activa \x201CNotificaciones de Windows\x201D en ajustes",
             pad, headerH, N.W - 2 * pad, listH - pad, L->fg2, DT_CENTER);
    }
    const int cw = N.W - 2 * pad;
    N.listH = listH;
    N.cardStep = cardH + gap;
    N.cardH = cardH;
    float acc = 0;                  /* alto extra de las tarjetas de arriba */
    for (int i = 0; i < count; ++i) {
        const int ch = cardH + (i < WN_MAX ? (int)(N.ex[i] + 0.5f) : 0);
        const WinNote *w = Wn_Get(i);
        /* salida: la tarjeta borrada (o todas, escalonadas) se desliza y se desvanece */
        float t = 0;
        if (N.outId == -1)         t = N.outT * 1.6f - i * 0.12f;
        else if (N.outId == w->id) t = N.outT;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        const float e = t * t * (3 - 2 * t), alpha = 1.0f - e;
        const int dx = (int)(e * (N.W - pad));
        const int y = headerH + i * (cardH + gap) + (int)acc - (int)lroundf(N.scrollF) + (i >= N.shiftFrom ? (int)N.shift : 0);
        if (i < WN_MAX) acc += N.ex[i];
        if (y + ch < headerH || y > headerH + listH || alpha <= 0.01f) continue;
        const BOOL hot = !N.outId && (CardOf(N.hot) == i || (N.reply && w->id == N.replyId));
        const int x = pad + dx;
        const float k = i < WN_MAX ? N.cs[i] : 1.0f;

        if (alpha < 0.999f || i >= WN_MAX || ch != cardH) {   /* saliendo o expandida: al vuelo */
            if (N.card.w != cw || N.card.h != ch) { Canvas_Free(&N.card); Canvas_Init(&N.card, cw, ch); }
            if (fabsf(k - 1.0f) < 0.002f || !N.card.dc) DrawCard(c, x, y, cw, ch, i, alpha, hot);
            else {
                Canvas_Clear(&N.card, L->bg);
                DrawCard(&N.card, 0, 0, cw, ch, i, alpha, hot);
                BlitScaled(c, &N.card, x + cw * 0.5f, y + ch * 0.5f, k);
            }
        } else {
            /* tarjeta en caché (dibujada una vez); el scroll y el hover solo la escalan */
            CardCache *cc = &s_cards[i];
            if (cc->cv.w != cw || cc->cv.h != cardH || cc->id != w->id || cc->hot != hot || cc->stamp != s_cardStamp) {
                if (cc->cv.w != cw || cc->cv.h != cardH) { Canvas_Free(&cc->cv); Canvas_Init(&cc->cv, cw, cardH); }
                if (cc->cv.dc) {
                    Canvas_Clear(&cc->cv, L->bg);
                    DrawCard(&cc->cv, 0, 0, cw, cardH, i, 1.0f, hot);
                    GdiFlush();
                }
                cc->id = w->id; cc->hot = hot; cc->stamp = s_cardStamp;
            }
            if (cc->cv.dc) BlitScaled(c, &cc->cv, x + cw * 0.5f, y + cardH * 0.5f, k);
            else DrawCard(c, x, y, cw, cardH, i, alpha, hot);
        }
        const float ccx = x + cw * 0.5f, ccy = y + ch * 0.5f;
        if (i < WN_MAX && N.tw[i] > 0.5f) DrawTrash(c, ccx, ccy, cw, ch, k, N.tw[i], alpha, N.hot == HT_CARDX + i);
        if (i < WN_MAX) SetRect(&N.cardRc[i], (int)(ccx - cw * k * 0.5f), (int)(ccy - ch * k * 0.5f),
                                (int)(ccx + cw * k * 0.5f), (int)(ccy + ch * k * 0.5f));
        if (N.outId) continue;
        /* zonas de clic con la misma escala que la tarjeta dibujada */
        AddHit((int)(ccx - cw * k * 0.5f), max((int)(ccy - ch * k * 0.5f), headerH),
               (int)(ccx + cw * k * 0.5f), min((int)(ccy + ch * k * 0.5f), headerH + listH), HT_CARD + i);
        if (hot && i < WN_MAX && ch > cardH + NS(ACT_ROW) / 2) {     /* botones de la tarjeta expandida */
            RECT pr[WN_ACTIONS + 1];
            int pk[WN_ACTIONS + 1];
            const int tx = x + NS(14) + NS(30) + NS(10), n = ActionPills(w, tx, x + cw - NS(14), y + ch - NS(ACT_ROW) + NS(4), pr, pk);
            for (int p = 0; p < n; ++p) {
                const float l0 = ccx + (pr[p].left - ccx) * k, r0 = ccx + (pr[p].right - ccx) * k;
                const float t0 = ccy + (pr[p].top - ccy) * k, b0 = ccy + (pr[p].bottom - ccy) * k;
                if (t0 >= headerH && b0 <= headerH + listH) AddHit((int)l0, (int)t0, (int)r0, (int)b0, HT_ACT + i * 4 + pk[p]);
            }
        }
        if (hot && i < WN_MAX && N.tw[i] > NS(8)) {      /* la franja entera es el botón de borrar */
            const float R = ccx + cw * k * 0.5f, zw = max(N.tw[i], (float)NS(TRASH_W)) * k;
            AddHit((int)(R - zw), max((int)(ccy - ch * k * 0.5f), headerH),
                   (int)R, min((int)(ccy + ch * k * 0.5f), headerH + listH), HT_CARDX + i);
        }
    }

    /* cabecera */
    Gfx_FillRRect(c, 0, 0, (float)N.W, (float)headerH, 0, L->bg, 1.0f);
    Text(c, N.fHeader, L"Notificaciones", pad + NS(6), 0, NS(200), headerH, L->fg, 0);
    const int gb = NS(32), gx = N.W - pad - gb, gy = (headerH - gb) / 2;
    Gfx_FillCircle(c, gx + gb * 0.5f, gy + gb * 0.5f, gb * 0.5f, N.hot == HT_C_GEAR ? L->cardHot : L->card, 1.0f);
    Gfx_Text(c, N.fIconSm, L"\xE713", gx, gy, gb, gb, L->fg2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    AddHit(gx, gy, gx + gb, gy + gb, HT_C_GEAR);
    const int mx = gx - NS(8) - gb;
    DrawMuteButton(c, mx, gy, gb, HT_C_MUTE);
    if (count) {
        const int bw = Gfx_TextWidth(N.fSmall, L"Borrar") + NS(26), bx = mx - NS(8) - bw;
        Gfx_FillRRect(c, (float)bx, (float)gy, (float)bw, (float)gb, gb * 0.5f, N.hot == HT_C_CLEAR ? L->cardHot : L->card, 1.0f);
        Gfx_Text(c, N.fSmall, L"Borrar", bx, gy, bw, gb, L->fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        AddHit(bx, gy, bx + bw, gy + gb, HT_C_CLEAR);
    }

    /* pie: margen inferior limpio (las tarjetas que se desplazan quedan debajo) */
    Gfx_FillRRect(c, 0, (float)(N.H - footH), (float)N.W, (float)footH, 0, L->bg, 1.0f);
}

static void RenderContent(void)
{
    switch (N.mode) {
    case M_PEEK:   if (N.peek.isBell) RenderPeekBell(); else if (N.peek.isNote) RenderPeekNote(); else RenderPeekAlert(); N.anchor = A_BOTTOM; break;
    case M_QUICK:  RenderQuick();  N.anchor = A_BOTTOM; break;
    case M_CENTER: RenderCenter(); N.anchor = A_TOP;    break;
    case M_MINI:   BeginContent(NS(110), NS(9)); N.anchor = A_BOTTOM; break;
    }
    GdiFlush();
    N.dirty = TRUE;
}

/* ───────────────────────── Fotograma ───────────────────────── */
static void CaptureBackdrop(void);
static DWORD SampleBack(int x, int y);
static DWORD OverGlass(DWORD glass, DWORD c, DWORD bg);
static void ClampTarget(void)
{
    const int half = max(N.W, (int)N.w) / 2 + NS(8);
    if (N.tx < N.mon.left + half)  N.tx = N.mon.left + half;
    if (N.tx > N.mon.right - half) N.tx = N.mon.right - half;
    if (!N.attached && N.ay > N.mon.bottom - N.H - NS(8)) N.ay = N.mon.bottom - N.H - NS(8);
}

static void RenderFrame(void)
{
    const Look *L = &N.look;
    const float w = N.w > 0 ? N.w : 0, h = N.h > 0 ? N.h : 0;
    const float fl = N.attached ? min((float)NS(7), h * 0.5f) : 0.0f;    /* curva cóncava junto al borde */
    const float rmode = N.mode == M_CENTER ? (float)NS(26) : N.attached ? (float)NS(16) : 1e6f;
    const float rb = min(h * 0.5f, rmode);

    /* ventana en pantalla */
    const int topPad = N.attached ? 0 : NS(14);
    N.winW = (int)(max((float)N.W, w) * 1.15f + 2 * fl) + NS(24);
    N.winH = topPad + (int)(max((float)N.H, h) * 1.12f) + NS(16);
    N.winX = (int)lroundf(N.ax) - N.winW / 2;
    N.winY = N.attached ? TopEdge() : N.ay - topPad;

    if (N.frame.w < N.winW || N.frame.h < N.winH) {
        Canvas_Free(&N.frame);
        if (!Canvas_Init(&N.frame, N.winW + NS(40), N.winH + NS(60))) return;
    }
    Canvas *f = &N.frame;
    GdiFlush();
    for (int y = 0; y < N.winH; ++y) ZeroMemory(&f->px[y * f->w], N.winW * sizeof(DWORD));

    /* cuerpo en coordenadas de ventana */
    const float cx = N.winW * 0.5f + (N.ax - lroundf(N.ax));
    const float top = N.attached ? 0.0f : (float)topPad;   /* la isla crece hacia abajo */
    const float left = cx - w * 0.5f, right = cx + w * 0.5f, bottom = top + h;
    N.cox = (int)lroundf(cx - N.W * 0.5f);
    N.coy = (int)lroundf(N.anchor == A_TOP ? top
                       : N.attached ? h - N.H
                       : top + (h - N.H) * 0.5f);

    const int x0 = max(0, (int)floorf(left - fl) - 1), x1 = min(N.winW, (int)ceilf(right + fl) + 1);
    const int y0 = max(0, (int)floorf(top) - 1),       y1 = min(N.winH, (int)ceilf(bottom) + 1);
    const DWORD bg = L->bg, hairColor = L->light ? 0xC7C7CC : 0x5A5A5E;
    const BOOL glass = g_cfg.material == MAT_GLASS && !App_RemoteView();
    const BOOL hair = L->hairline && !(N.attached && Bar_HeightOn(&N.mon) > 0);
    if (glass) CaptureBackdrop();
    const BOOL simple = !glass && L->bgA >= 1.0f && N.fade >= 0.999f && N.op >= 0.999f && N.content.px;

    for (int y = y0; y < y1; ++y) {
        const float py = y + 0.5f;
        for (int x = x0; x < x1; ++x) {
            const float px = x + 0.5f;
            float d;
            /* atajo: dentro de la zona recta no hace falta la SDF completa */
            if (px > left + rb && px < right - rb && py < bottom - rb && (N.attached || py > top + rb)) {
                d = -min(min(px - left, right - px), min(bottom - py, N.attached ? 1e6f : py - top));
                if (simple && d < -2.5f) {      /* interior opaco: copia directa del contenido */
                    const int qx = x - N.cox, qy = y - N.coy;
                    const DWORD c = qx >= 0 && qy >= 0 && qx < N.content.w && qy < N.content.h
                                  ? N.content.px[qy * N.content.w + qx] & 0xFFFFFF : bg;
                    f->px[y * f->w + x] = 0xFF000000 | c;
                    continue;
                }
            }
            else if (N.attached)    /* cuerpo extendido por encima del borde → solo esquinas de abajo */
                d = Gfx_SdRRect(px, py, left, -rb, w, h + rb, rb);
            else
                d = Gfx_SdRRect(px, py, left, top, w, h, rb);
            float cov = Gfx_Cov(d);
            if (fl > 0.5f && py < fl) {
                float ccx = -1;
                if (px < left && px > left - fl)        ccx = left - fl;
                else if (px > right && px < right + fl) ccx = right + fl;
                if (ccx >= 0) {
                    const float dx = px - ccx, dy = py - fl;
                    float a = sqrtf(dx * dx + dy * dy) - fl + 0.5f;
                    if (a > 1) a = 1;
                    if (a > cov) cov = a;
                }
            }
            const float a = cov * N.op;
            if (a <= 0.002f) continue;

            DWORD c = bg;
            const int qx = x - N.cox, qy = y - N.coy;
            if (N.fade > 0 && N.content.px && qx >= 0 && qy >= 0 && qx < N.content.w && qy < N.content.h)
                c = Gfx_Mix(bg, N.content.px[qy * N.content.w + qx] & 0xFFFFFF, N.fade);
            if (glass) c = OverGlass(Gfx_Mix(SampleBack(x, y), bg, 0.52f), c, bg);
            if (hair && d > -2.0f && d < 0.5f)
                c = Gfx_Mix(c, hairColor, 0.55f * Gfx_Cov(fabsf(d + 0.8f) - 0.5f));

            float k = a;
            if (L->bgA < 1.0f) {    /* vidrio: fondo translúcido, texto y tarjetas opacos */
                int ink = 0;
                for (int s = 0; s < 24; s += 8)
                    ink = max(ink, abs((int)((c >> s) & 255) - (int)((bg >> s) & 255)));
                k = a * (L->bgA + (1.0f - L->bgA) * min(1.0f, ink / 40.0f));
            }
            f->px[y * f->w + x] = (DWORD)(k * 255.0f + 0.5f) << 24
                                | (DWORD)(((c >> 16) & 255) * k + 0.5f) << 16
                                | (DWORD)(((c >> 8) & 255) * k + 0.5f) << 8
                                | (DWORD)((c & 255) * k + 0.5f);
        }
    }

    POINT dst = { N.winX, N.winY }, src = { 0, 0 };
    SIZE  sz  = { N.winW, N.winH };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC screen = GetDC(NULL);
    UpdateLayeredWindow(N.hwnd, screen, &dst, &sz, f->dc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
    N.dirty = FALSE;
}

/* ───────────────────────── Vidrio ─────────────────────────
 * El desenfoque de DWM no está disponible para ventanas layered, así que lo hacemos
 * nosotros: copiamos lo que hay detrás de la isla (la isla está excluida de captura,
 * no se ve a sí misma), a 1/4 de resolución, lo desenfocamos y le subimos la
 * saturación. Solo vive en memoria y solo mientras la isla está en pantalla.     */
static void CaptureBackdrop(void)
{
    /* pegada a la barra, la isla y la barra son una sola pieza de vidrio: se captura desde
     * el borde del monitor (lo que hay detrás de la barra cuenta para el desenfoque) y en la
     * misma rejilla y con el mismo radio que la barra, así no se nota la unión */
    const int ext = N.attached ? max(0, N.winY - N.mon.top) : 0;
    const int ox = ((N.winX - N.mon.left) % BACK_SCALE + BACK_SCALE) % BACK_SCALE;
    const int sw = max(1, (N.winW + ox + BACK_SCALE - 1) / BACK_SCALE), sh = max(1, (N.winH + ext + BACK_SCALE - 1) / BACK_SCALE);
    if (N.back.w != sw || N.back.h != sh) {
        Canvas_Free(&N.back);
        if (!Canvas_Init(&N.back, sw, sh)) return;
    }
    HDC screen = GetDC(NULL);
    SetStretchBltMode(N.back.dc, HALFTONE);
    SetBrushOrgEx(N.back.dc, 0, 0, NULL);
    StretchBlt(N.back.dc, 0, 0, sw, sh, screen, N.winX - ox, N.winY - ext, sw * BACK_SCALE, sh * BACK_SCALE, SRCCOPY);
    ReleaseDC(NULL, screen);
    GdiFlush();
    N.backOX = ox; N.backOY = ext;
    for (int i = 0; i < 2; ++i) Gfx_BoxBlur(N.back.px, sw, sh, max(1, MulDiv(16, (int)N.dpi, 96) / BACK_SCALE));
    /* vibrancia: un poco más de saturación, como el vidrio de Apple */
    for (int i = 0; i < sw * sh; ++i) {
        const DWORD v = N.back.px[i];
        const int r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255, y = (r * 77 + g * 151 + b * 28) >> 8;
        const int R = max(0, min(255, y + (r - y) * 3 / 2)), G = max(0, min(255, y + (g - y) * 3 / 2)), B = max(0, min(255, y + (b - y) * 3 / 2));
        N.back.px[i] = (DWORD)R << 16 | (DWORD)G << 8 | (DWORD)B;
    }
    N.backTick = GetTickCount();
}

/* Muestreo bilineal del fondo reducido. */
static DWORD SampleBack(int x, int y)
{
    if (!N.back.px) return N.look.bg;
    const float fx = (x + N.backOX + 0.5f) / BACK_SCALE - 0.5f, fy = (y + N.backOY + 0.5f) / BACK_SCALE - 0.5f;
    const int x0 = max(0, min(N.back.w - 1, (int)floorf(fx))), y0 = max(0, min(N.back.h - 1, (int)floorf(fy)));
    const int x1 = min(N.back.w - 1, x0 + 1), y1 = min(N.back.h - 1, y0 + 1);
    const float tx = max(0.0f, min(1.0f, fx - x0)), ty = max(0.0f, min(1.0f, fy - y0));
    const DWORD *p = N.back.px;
    return Gfx_Mix(Gfx_Mix(p[y0 * N.back.w + x0], p[y0 * N.back.w + x1], tx),
                   Gfx_Mix(p[y1 * N.back.w + x0], p[y1 * N.back.w + x1], tx), ty);
}

/* Pinta el contenido sobre el vidrio: lo que apenas difiere del fondo plano (tarjetas)
 * se suma al vidrio y deja verlo; texto e iconos conservan su color exacto. */
static DWORD OverGlass(DWORD glass, DWORD c, DWORD bg)
{
    DWORD out = 0;
    int ink = 0;
    for (int sft = 0; sft < 24; sft += 8) {
        const int d = (int)((c >> sft) & 255) - (int)((bg >> sft) & 255);
        ink = max(ink, abs(d));
        out |= (DWORD)max(0, min(255, (int)((glass >> sft) & 255) + d)) << sft;
    }
    return Gfx_Mix(out, c, min(1.0f, ink / 110.0f));
}

/* ───────────────────────── Marcapasos ─────────────────────────
 * SetTimer no baja de ~15,6 ms (≈64 fps, con tirones). Un hilo con un temporizador
 * de alta resolución marca el ritmo a la frecuencia real del monitor (60/120/144 Hz)
 * y solo pide un fotograma nuevo cuando el anterior ya se pintó.               */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
static HANDLE        s_pacerOn;     /* evento manual: la isla está visible */
static volatile LONG s_framePending;

static DWORD WINAPI Pacer(LPVOID unused)
{
    (void)unused;
    HANDLE t = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!t) t = CreateWaitableTimerW(NULL, FALSE, NULL);
    if (!t) return 0;
    for (;;) {
        WaitForSingleObject(s_pacerOn, INFINITE);
        LARGE_INTEGER due;
        due.QuadPart = -10000000LL / max(30, min(240, N.hz));
        SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE);
        WaitForSingleObject(t, 50);
        if (N.hwnd && !InterlockedExchange(&s_framePending, 1))
            PostMessageW(N.hwnd, WM_NOTCH_FRAME, 0, 0);
    }
}

static void StartFrames(void)
{
    if (!s_pacerOn) {
        s_pacerOn = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE th = s_pacerOn ? CreateThread(NULL, 0, Pacer, NULL, 0, NULL) : NULL;
        if (th) { SetThreadPriority(th, THREAD_PRIORITY_ABOVE_NORMAL); CloseHandle(th); }
        else if (s_pacerOn) { CloseHandle(s_pacerOn); s_pacerOn = NULL; }
    }
    if (s_pacerOn) SetEvent(s_pacerOn);
    else SetTimer(N.hwnd, TIMER_FRAME, 10, NULL);     /* respaldo */
}

static void StopFrames(void)
{
    if (s_pacerOn) ResetEvent(s_pacerOn);
    KillTimer(N.hwnd, TIMER_FRAME);
}

/* ───────────────────────── Sonido ─────────────────────────
 * Silencio, el de Windows y los avisos y celebraciones de Material Design Sound Resources (Google, CC-BY 4.0), incrustados como
 * recursos (mono, 16 bit, 48 kHz, recortados y a la misma altura). El volumen se aplica al
 * reproducir. */
#define MATERIAL_COUNT 13
static const struct { LPCWSTR name; BOOL celebration; } kMaterial[MATERIAL_COUNT] = {
    { L"Nota" }, { L"Eco suave" }, { L"Alerta" }, { L"Destello" }, { L"Cascada" }, { L"Aviso" }, { L"Ambiente" },
    { L"Confirmar" }, { L"Logro", TRUE }, { L"Brindis", TRUE }, { L"Fanfarria", TRUE }, { L"Fiesta", TRUE },
    { L"Completado", TRUE },
};

LPCWSTR Notch_SoundName(int i)
{
    if (i == 0) return L"Silencio";
    if (i == 1) return L"Windows";
    return i > 1 && i < SOUND_COUNT ? kMaterial[i - 2].name : L"";
}

LPCWSTR Notch_SoundFamily(int i)
{
    if (i <= 1) return L"Sistema";
    return i < SOUND_COUNT && kMaterial[i - 2].celebration ? L"Celebración \x00B7 Material" : L"Avisos \x00B7 Material";
}

/* Un sonido de Material: el recurso tal cual (vive con el ejecutable, no se libera). */
static BYTE *MaterialSound(int k, DWORD *size)
{
    HRSRC r = FindResourceW(g_inst, MAKEINTRESOURCEW(IDR_SOUND0 + k), (LPCWSTR)RT_RCDATA);
    HGLOBAL g = r ? LoadResource(g_inst, r) : NULL;
    BYTE *p = g ? (BYTE *)LockResource(g) : NULL;
    *size = r ? SizeofResource(g_inst, r) : 0;
    return p && *size > 44 ? p : NULL;
}

/* El sonido de notificación de Windows, leído de su .wav para poder bajarle el volumen. */
static BYTE *LoadWindowsSound(DWORD *size)
{
    wchar_t raw[MAX_PATH], path[MAX_PATH];
    DWORD sz = sizeof(raw);
    if (RegGetValueW(HKEY_CURRENT_USER, L"AppEvents\\Schemes\\Apps\\.Default\\Notification.Default\\.Current", NULL,
                     RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, NULL, raw, &sz) != ERROR_SUCCESS || !raw[0])
        return NULL;
    if (!ExpandEnvironmentStringsW(raw, path, MAX_PATH)) return NULL;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    const DWORD len = GetFileSize(f, NULL);
    BYTE *b = len > 44 && len < 8 * 1024 * 1024 ? (BYTE *)HeapAlloc(GetProcessHeap(), 0, len) : NULL;
    DWORD got = 0;
    if (b && (!ReadFile(f, b, len, &got, NULL) || got != len || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4))) {
        HeapFree(GetProcessHeap(), 0, b);
        b = NULL;
    }
    CloseHandle(f);
    *size = len;
    return b;
}

/* Copia del sonido con el volumen aplicado (solo PCM de 16 bits; otro formato, tal cual). */
static BYTE *s_play;
static DWORD s_playCap;

static const BYTE *WithVolume(const BYTE *wav, DWORD size, float gain)
{
    if (s_playCap < size) {
        if (s_play) HeapFree(GetProcessHeap(), 0, s_play);
        s_play = (BYTE *)HeapAlloc(GetProcessHeap(), 0, size);
        s_playCap = s_play ? size : 0;
        if (!s_play) return wav;
    }
    CopyMemory(s_play, wav, size);
    WORD fmt = 0, bits = 0;
    for (DWORD p = 12; p + 8 <= size; ) {
        DWORD clen;
        CopyMemory(&clen, s_play + p + 4, 4);
        if (clen > size - p - 8) break;             /* trozo que se sale del archivo */
        if (!memcmp(s_play + p, "fmt ", 4) && p + 24 <= size) { CopyMemory(&fmt, s_play + p + 8, 2); CopyMemory(&bits, s_play + p + 22, 2); }
        if (!memcmp(s_play + p, "data", 4)) {
            if (fmt == 1 && bits == 16) {
                short *d = (short *)(s_play + p + 8);
                const DWORD count = min(clen, size - p - 8) / 2;
                for (DWORD i = 0; i < count; ++i) d[i] = (short)(d[i] * gain);
            }
            break;
        }
        p += 8 + clen + (clen & 1);
    }
    return s_play;
}

void Notch_PlaySound(void)
{
    static BYTE *s_built[SOUND_COUNT];
    static DWORD s_size[SOUND_COUNT];
    const float gain = powf(max(0, min(100, g_cfg.soundVol)) / 100.0f, 1.2f);   /* 0..100, curva de oído */
    const int i = g_cfg.sound;
    if (i <= 0 || i >= SOUND_COUNT) return;
    if (Bar_SystemSilent()) return;                  /* el sistema en silencio manda sobre el volumen propio */
    if (!s_built[i]) s_built[i] = i == 1 ? LoadWindowsSound(&s_size[i]) : MaterialSound(i - 2, &s_size[i]);
    PlaySoundW(NULL, NULL, 0);                       /* el anterior se corta antes de reutilizar el búfer */
    if (!s_built[i]) {                               /* Windows sin .wav legible: su alias, a su volumen */
        if (i == 1) PlaySoundW(L"Notification.Default", NULL, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
        return;
    }
    if (gain <= 0.001f) return;
    const BYTE *w = WithVolume(s_built[i], s_size[i], gain);
    PlaySoundW((LPCWSTR)w, NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

/* ───────────────────────── Estados ───────────────────────── */
static void SetClickThrough(BOOL on)
{
    if (on == N.clickThrough) return;
    LONG_PTR ex = GetWindowLongPtrW(N.hwnd, GWL_EXSTYLE);
    ex = on ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT);
    SetWindowLongPtrW(N.hwnd, GWL_EXSTYLE, ex);
    N.clickThrough = on;
}

static void SetActivatable(BOOL on)
{
    LONG_PTR ex = GetWindowLongPtrW(N.hwnd, GWL_EXSTYLE);
    ex = on ? (ex & ~WS_EX_NOACTIVATE) : (ex | WS_EX_NOACTIVATE);
    SetWindowLongPtrW(N.hwnd, GWL_EXSTYLE, ex);
}

static void CloseReply(BOOL refocus);
static void Close(void)
{
    CloseReply(FALSE);
    N.closing = TRUE;
    N.dirty = TRUE;
}

static void Hide(void)
{
    StopFrames();
    ShowWindow(N.hwnd, SW_HIDE);
    if (N.mode == M_CENTER) SetActivatable(FALSE);
    N.outId = 0;
    N.outT = N.shift = N.vshift = 0;
    N.mode = M_HIDDEN;
    N.closing = N.hover = N.down = N.dragging = N.active = FALSE;
    N.hot = HT_NONE;
    Canvas_Free(&N.thumb);
    if (N.quitOnHide) {
        if (g_ctrl) PostMessageW(g_ctrl, WM_CLOSE, 0, 0);
        else PostQuitMessage(0);
    }
}

static BOOL EnsureWindow(void)
{
    if (N.hwnd) return TRUE;
    N.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             NOTCH_CLASS, L"OpenDock", WS_POPUP, 0, 0, 0, 0, NULL, NULL, g_inst, NULL);
    if (!N.hwnd) return FALSE;
    QueryPerformanceFrequency(&N.freq);
    return TRUE;
}

static void UseMonitorAt(POINT pt)
{
    HMONITOR m = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(m, (MONITORINFO *)&mi);
    N.mon = mi.rcMonitor;
    DEVMODEW dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    N.hz = EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1
         ? (int)dm.dmDisplayFrequency : 60;
    N.dpi = MonitorDpi(m);
    LoadLook();
    MakeFonts();
}

/* Notch: pegado al borde (salvo que se haya arrastrado abajo). Flotante: separado del borde. */
static int FloatGap(void) { return NS(10); }

/* Borde del que nace la isla: el de la pantalla o, con la barra superior, el de la barra. */
static int TopEdge(void) { return N.mon.top + Bar_HeightOn(&N.mon); }

static void AnchorY(void)
{
    N.attached = !g_cfg.floating && g_cfg.notchY <= 0;
    if (N.attached)          N.ay = TopEdge();
    else if (g_cfg.notchY)   N.ay = max(TopEdge(), N.mon.top + MulDiv(g_cfg.notchY, (int)N.dpi, 96));
    else                     N.ay = TopEdge() + FloatGap();
}

/* Monitor de la isla: los avisos salen en el principal, como los de Windows; el centro de
 * notificaciones y lo que se pide con el ratón, en el monitor del cursor. */
static void PlaceFromConfig(BOOL atCursor)
{
    POINT pt = { 0, 0 };            /* (0,0) siempre es del monitor principal */
    if (atCursor) GetCursorPos(&pt);
    UseMonitorAt(pt);
    const int nx = max(0, min(10000, g_cfg.notchX));
    N.tx = N.mon.left + MulDiv(N.mon.right - N.mon.left, nx, 10000);
    N.ax = (float)N.tx;
    N.vax = 0;
    AnchorY();
}

/* Entra en un estado; si estaba oculta, arranca la animación desde la pastilla cerrada. */
static void Enter(int mode)
{
    const BOOL fresh = N.mode == M_HIDDEN;
    const BOOL changed = N.mode != mode;
    if (N.mode == M_CENTER && mode != M_CENTER) { SetActivatable(FALSE); N.active = FALSE; }
    N.mode = mode;
    N.closing = FALSE;
    N.hot = HT_NONE;
    if (mode == M_CENTER) {
        ++s_cardStamp;              /* la hora relativa ("5 min") pudo cambiar */
        N.scroll = 0;
        N.scrollF = N.vScroll = 0;
        for (int i = 0; i < WN_MAX; ++i) { N.cs[i] = 1.0f; N.cv[i] = 0; N.tw[i] = N.tv[i] = 0; N.ex[i] = N.exv[i] = 0; }
    }
    RenderContent();
    ClampTarget();
    SetClickThrough(mode == M_MINI);
    Notch_ApplyCapture();
    if (changed) N.fade = 0;

    if (fresh) {
        N.w  = PillW();
        N.h  = PillH();
        N.vw = N.vh = 0;
        N.op = N.attached ? 1.0f : 0.0f;
        QueryPerformanceCounter(&N.last);
        RenderFrame();
        ShowWindow(N.hwnd, SW_SHOWNOACTIVATE);
        StartFrames();
    } else if (!changed) {
        N.vw += N.W * 1.2f;     /* pequeño "rebote" al actualizar con la isla ya abierta */
    }
    if (mode == M_CENTER) {
        SetActivatable(TRUE);
        SetForegroundWindow(N.hwnd);
        SetFocus(N.hwnd);
    }
    Notch_Raise();
}

static int MagnetX(int x)
{
    const int anchors[2] = {
        (N.mon.left + N.mon.right) / 2,
        N.mon.left + MulDiv(N.mon.right - N.mon.left, max(0, min(10000, g_cfg.notchX)), 10000),
    };
    for (int i = 0; i < 2; ++i)
        if (abs(x - anchors[i]) < NS(64)) return anchors[i];
    return x;
}

/* ───────────────────────── Animación ───────────────────────── */
/* Pastilla de la que nace la isla y a la que vuelve al cerrarse. */
static float PillW(void) { return N.attached ? N.W * 0.45f : min(N.W * 0.5f, (float)NS(90)); }
static float PillH(void) { return N.attached ? 0.0f : min(N.H * 0.4f, (float)NS(16)); }

static void Spring(float *x, float *v, float target, float dt, float k, float zeta)
{
    const float acc = (target - *x) * k - *v * 2.0f * sqrtf(k) * zeta;
    *v += acc * dt;
    *x += *v * dt;
}

static void ModeLogic(void)
{
    POINT pt;
    GetCursorPos(&pt);
    const DWORD now = GetTickCount();
    RECT body = { N.winX + N.cox, N.winY + N.coy, N.winX + N.cox + N.W, N.winY + N.coy + N.H };

    switch (N.mode) {
    case M_PEEK:
        if (!N.hover && !N.down && (LONG)(now - N.hideAt) >= 0) Close();
        break;
    case M_MINI:
        if (pt.y > TopEdge() + NS(26) || !Notch_InEdgeZone(pt, &N.mon, N.dpi)) { Close(); break; }
        N.tx = MagnetX(pt.x);
        ClampTarget();
        if (abs(pt.x - N.dwellPt.x) + abs(pt.y - N.dwellPt.y) > NS(4)) { N.dwellPt = pt; N.dwellSince = now; }
        else if (now - N.dwellSince > 450 && pt.y <= TopEdge() + NS(10)) Enter(M_QUICK);   /* la barra es el borde */
        break;
    case M_QUICK:
        InflateRect(&body, NS(28), NS(28));
        if (!PtInRect(&body, pt)) Close();
        break;
    case M_CENTER:
        if (!N.active && ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000) && !PtInRect(&body, pt))
            Close();
        break;
    }
}

static void Tick(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    float dt = (float)(t.QuadPart - N.last.QuadPart) / (float)N.freq.QuadPart;
    N.last = t;
    if (dt > 0.05f) dt = 0.05f;

    if (!N.closing) ModeLogic();
    if (N.mode == M_HIDDEN) return;

    float tw, th;
    if (!N.closing) {
        tw = (float)N.W; th = (float)N.H;
    } else if (N.attached) {
        tw = PillW(); th = 0;                       /* se recoge hacia el borde, como una persiana */
    } else {
        tw = PillW(); th = PillH();                 /* vuelve a la pastilla de la que nació */
    }
    const float fw = N.w, fh = N.h, fx = N.ax, ff = N.fade, fo = N.op;
    for (int i = 0; i < 2; ++i) {
        if (N.closing) {    /* sin rebote al cerrar: ancho y alto llegan juntos */
            Spring(&N.w, &N.vw, tw, dt * 0.5f, 300.0f, 1.0f);
            Spring(&N.h, &N.vh, th, dt * 0.5f, 300.0f, 1.0f);
        } else {
            Spring(&N.w, &N.vw, tw, dt * 0.5f, 380.0f, N.look.zeta);
            Spring(&N.h, &N.vh, th, dt * 0.5f, 420.0f, min(1.0f, N.look.zeta + 0.1f));
        }
        Spring(&N.ax, &N.vax, (float)N.tx, dt * 0.5f, N.mode == M_MINI ? 240.0f : 320.0f, N.look.zeta);
    }
    if (!N.closing) {
        const float tf = N.h > N.H * 0.55f ? 1.0f : 0.0f;  /* el texto aparece cuando ya hay sitio */
        N.fade += (tf - N.fade) * min(1.0f, dt * (tf > N.fade ? 16.0f : 26.0f));
        N.op   += (1.0f - N.op) * min(1.0f, dt * 14.0f);
    } else {
        /* al cerrar, contenido y forma van ligados a la altura: nada se apaga de golpe */
        const float span = max(1.0f, N.H - th);
        const float prog = max(0.0f, min(1.0f, (N.h - th) / span));        /* 1 abierto → 0 cerrado */
        const float f = max(0.0f, min(1.0f, (prog - 0.25f) / 0.5f));
        N.fade = min(N.fade, f * f * (3 - 2 * f));
        if (!N.attached) N.op = min(N.op, max(0.0f, min(1.0f, prog / 0.5f)));   /* se desvanece en la segunda mitad */
        const BOOL done = N.attached ? (N.h < 0.6f && fabsf(N.vh) < 40.0f) : (N.op < 0.03f || prog < 0.01f);
        if (done) { Hide(); return; }
    }
    if (N.mode == M_CENTER && (N.outId || N.shift != 0.0f)) {
        if (N.outId) {
            N.outT += dt / (N.outId == -1 ? 0.55f : 0.26f);
            if (N.outT >= 1.0f) {
                const LONGLONG id = N.outId;
                N.outId = 0;
                N.outT = 0;
                if (id == -1) {
                    Wn_DismissAll();
                } else {
                    for (int i = 0; i < Wn_Count(); ++i)
                        if (Wn_Get(i)->id == id) {
                            /* las de abajo arrancan donde estaban y suben con rebote */
                            N.shiftFrom = i;
                            N.shift = (float)N.cardStep;
                            N.vshift = 0;
                            break;
                        }
                    Wn_Dismiss(id);
                }
            }
        } else {
            for (int i = 0; i < 2; ++i) Spring(&N.shift, &N.vshift, 0, dt * 0.5f, 300.0f, N.look.zeta);
            if (fabsf(N.shift) < 0.3f && fabsf(N.vshift) < 6.0f) N.shift = N.vshift = 0;
        }
        RenderContent();
    }

    if (N.mode == M_CENTER && !N.closing && N.cardStep > 0) {
        BOOL busy = FALSE;
        /* scroll suave con el rebote del tema */
        const float s0 = N.scrollF;
        for (int i = 0; i < 2; ++i) Spring(&N.scrollF, &N.vScroll, (float)N.scroll, dt * 0.5f, 260.0f, max(0.75f, N.look.zeta));
        if (fabsf(N.scrollF - N.scroll) < 0.3f && fabsf(N.vScroll) < 4.0f) { N.scrollF = (float)N.scroll; N.vScroll = 0; }
        if (fabsf(N.scrollF - s0) > 0.01f) busy = TRUE;

        /* escalas: al hacer scroll crecen las del centro; con el ratón, la señalada */
        const int count = min(Wn_Count(), WN_MAX);
        const BOOL scrolling = GetTickCount() - N.scrollTick < 450 || fabsf(N.vScroll) > 30.0f;
        int hov = N.outId ? -1 : CardOf(N.hot);
        if (N.reply) for (int i = 0; i < count; ++i) if (Wn_Get(i)->id == N.replyId) hov = i;   /* respondiendo: sigue abierta */
        if (hov != N.hoverCard) { N.hoverCard = hov; N.hoverSince = GetTickCount(); }
        for (int i = 0; i < count; ++i) {
            float target = 1.0f;
            if (scrolling) {
                const float yc = i * (float)N.cardStep - N.scrollF + N.cardH * 0.5f;
                const float dist = fabsf(yc - N.listH * 0.5f) / max(1.0f, N.listH * 0.5f);
                target = 1.035f - 0.075f * min(1.0f, dist);
            } else if (hov >= 0) {
                target = i == hov ? 1.035f : 0.965f;
            }
            const float before = N.cs[i];
            for (int k = 0; k < 2; ++k) Spring(&N.cs[i], &N.cv[i], target, dt * 0.5f, 420.0f, N.look.zeta);
            if (fabsf(N.cs[i] - target) < 0.0008f && fabsf(N.cv[i]) < 0.01f) { N.cs[i] = target; N.cv[i] = 0; }
            if (fabsf(N.cs[i] - before) > 0.00005f) busy = TRUE;
            /* papelera: asoma en la tarjeta señalada y se ensancha sobre sí misma */
            /* solo asoma si el cursor se acerca al borde derecho: así no tapa el texto al leer */
            const BOOL nearTrash = i == hov && N.mx > N.cardRc[i].right - NS(TRASH_W + 30);
            const float tt = nearTrash ? (float)NS(N.hot == HT_CARDX + i ? TRASH_W_HOT : TRASH_W) : 0.0f;
            const float tb = N.tw[i];
            for (int k = 0; k < 2; ++k) Spring(&N.tw[i], &N.tv[i], tt, dt * 0.5f, 520.0f, 0.78f);
            if (fabsf(N.tw[i] - tt) < 0.3f && fabsf(N.tv[i]) < 2.0f) { N.tw[i] = tt; N.tv[i] = 0; }
            if (N.tw[i] < 0) N.tw[i] = 0;
            if (fabsf(N.tw[i] - tb) > 0.01f) busy = TRUE;
            /* la señalada crece si su texto está cortado o trae botones (tras un instante,
             * para que pasar el cursor por encima no haga saltar la lista) */
            const BOOL dwell = i == hov && (N.reply || GetTickCount() - N.hoverSince > 260);
            const float et = dwell ? NeedExtra(i, N.W - 2 * NS(18)) : 0.0f, eb = N.ex[i];
            for (int k = 0; k < 2; ++k) Spring(&N.ex[i], &N.exv[i], et, dt * 0.5f, 340.0f, max(0.7f, N.look.zeta));
            if (fabsf(N.ex[i] - et) < 0.3f && fabsf(N.exv[i]) < 2.0f) { N.ex[i] = et; N.exv[i] = 0; }
            if (N.ex[i] < 0) N.ex[i] = 0;
            if (fabsf(N.ex[i] - eb) > 0.01f || (i == hov && !dwell)) busy = TRUE;
        }
        if (busy) RenderContent();
    }

    /* aviso recién llegado: con el cursor encima un instante, crece hasta enseñar todo el texto */
    if (N.mode == M_PEEK && N.peek.isNote && !N.peek.isBell && !N.closing) {
        const BOOL dwell = N.peekSince && !N.down && GetTickCount() - N.peekSince > 260;
        const float et = dwell ? PeekNeed() : 0.0f, eb = N.pex;
        for (int k = 0; k < 2; ++k) Spring(&N.pex, &N.pexv, et, dt * 0.5f, 340.0f, max(0.7f, N.look.zeta));
        if (fabsf(N.pex - et) < 0.3f && fabsf(N.pexv) < 2.0f) { N.pex = et; N.pexv = 0; }
        if (N.pex < 0) N.pex = 0;
        if (fabsf(N.pex - eb) > 0.01f) RenderContent();
    }

    if ((N.mode == M_QUICK || N.mode == M_CENTER || (N.mode == M_PEEK && N.peek.isBell))
        && !N.closing && (LONG)(GetTickCount() - N.ringStart) < 2400) RenderContent();

    /* en reposo no se redibuja nada: el panel abierto no gasta CPU */
    const float moved = fabsf(N.w - fw) + fabsf(N.h - fh) + fabsf(N.ax - fx) + fabsf(N.fade - ff) * 50 + fabsf(N.op - fo) * 50;
    if (g_cfg.material == MAT_GLASS && !App_RemoteView() && GetTickCount() - N.backTick > 80) N.dirty = TRUE;
    if (moved > 0.02f || N.dirty) RenderFrame();
}

/* ───────────────────────── Interacción ───────────────────────── */
static void SavePosition(void)
{
    const int mw = max(1, N.mon.right - N.mon.left);
    g_cfg.notchX = MulDiv(N.tx - N.mon.left, 10000, mw);
    g_cfg.notchY = N.attached || (g_cfg.floating && N.ay - TopEdge() <= FloatGap()) ? 0
                 : max(1, MulDiv(N.ay - N.mon.top, 96, (int)N.dpi));
    Cfg_Save();
}

static void OpenPath(void)
{
    /* lo abre explorer con la app predeterminada: ninguna extensión de shell entra en nuestro proceso */
    if (App_ShellOpen(N.openPath)) {
    }
}

static void ToggleMute(void)
{
    g_cfg.hideBanners = !g_cfg.hideBanners;
    N.ringStart = GetTickCount();       /* la campanita suena al cambiar */
    App_NotchConfigChanged();
}

/* ───────────────────────── Respuesta rápida ─────────────────────────
 * Una caja de texto real (EDIT) que aparece sobre la tarjeta: Enter envía la respuesta a la
 * app por su activador, Esc la cierra. Al enviar, el aviso se retira como en Windows. */
#define REPLY_CLASS L"OpenDock.Reply"
static WNDPROC s_editProc;

static void CloseReply(BOOL refocus)
{
    if (!N.reply) return;
    HWND r = N.reply;
    N.reply = N.replyEdit = NULL;
    DestroyWindow(r);
    if (refocus && N.hwnd && N.mode == M_CENTER) SetForegroundWindow(N.hwnd);
}

static void SendReply(void)
{
    wchar_t text[512];
    if (!N.replyEdit || GetWindowTextW(N.replyEdit, text, 512) <= 0) return;
    for (int i = 0; i < Wn_Count(); ++i) {
        const WinNote *w = Wn_Get(i);
        if (w->id != N.replyId) continue;
        int send = -1;                                   /* el botón "enviar" de esa caja */
        for (int a = 0; a < w->nact; ++a) if (!lstrcmpiW(w->act[a].input, w->inputId)) send = a;
        Wn_Activate(w, send, text);
        N.outId = w->id; N.outT = 0;
        break;
    }
    SecureZeroMemory(text, sizeof(text));
    CloseReply(TRUE);
}

static LRESULT CALLBACK ReplyEditProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_KEYDOWN && w == VK_RETURN) { SendReply(); return 0; }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { CloseReply(TRUE); return 0; }
    if (m == WM_CHAR && (w == L'\r' || w == 27)) return 0;      /* sin pitido */
    return CallWindowProcW(s_editProc, h, m, w, l);
}

static LRESULT CALLBACK ReplyProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)w, RGB((N.look.fg >> 16) & 255, (N.look.fg >> 8) & 255, N.look.fg & 255));
        SetBkColor((HDC)w, RGB((N.look.cardHot >> 16) & 255, (N.look.cardHot >> 8) & 255, N.look.cardHot & 255));
        return (LRESULT)N.replyBrush;
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(h, &r);
        FillRect((HDC)w, &r, N.replyBrush);
        return 1;
    }
    case WM_SETFOCUS:
        if (N.replyEdit) SetFocus(N.replyEdit);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && N.reply == h) {
            const BOOL toNotch = (HWND)l == N.hwnd;
            CloseReply(FALSE);
            if (!toNotch && N.mode == M_CENTER) Close();     /* el foco se fue a otra parte */
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void OpenReply(int i)
{
    if (i < 0 || i >= WN_MAX || N.mode != M_CENTER) return;
    CloseReply(FALSE);
    static BOOL reg;
    if (!reg) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = ReplyProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(NULL, IDC_IBEAM);
        wc.lpszClassName = REPLY_CLASS;
        reg = RegisterClassExW(&wc) != 0;
    }
    const WinNote *w = Wn_Get(i);
    N.replyId = w->id;
    if (N.replyBrush) DeleteObject(N.replyBrush);
    N.replyBrush = CreateSolidBrush(RGB((N.look.cardHot >> 16) & 255, (N.look.cardHot >> 8) & 255, N.look.cardHot & 255));
    if (N.fReply) DeleteObject(N.fReply);
    N.fReply = Gfx_Font(Gfx_UiFace(), NS(13), FW_NORMAL, CLEARTYPE_QUALITY);

    /* sobre la fila de botones de la tarjeta, en pantalla */
    RECT wr;
    GetWindowRect(N.hwnd, &wr);
    const RECT *cr = &N.cardRc[i];
    const int x = wr.left + N.cox + cr->left + NS(10), wdt = cr->right - cr->left - NS(20);
    const int hgt = NS(34), y = wr.top + N.coy + cr->bottom - NS(ACT_ROW) + NS(2);
    N.reply = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, REPLY_CLASS, L"Responder", WS_POPUP | WS_CLIPCHILDREN,
                              x, y, wdt, hgt, N.hwnd, NULL, g_inst, NULL);
    if (!N.reply) return;
    int round = 2;                                       /* DWMWCP_ROUND */
    BOOL dark = !N.look.light;
    DwmSetWindowAttribute(N.reply, 33, &round, sizeof(round));
    DwmSetWindowAttribute(N.reply, 20, &dark, sizeof(dark));
    if (App_HideFromCapture(FALSE)) SetWindowDisplayAffinity(N.reply, WDA_EXCLUDEFROMCAPTURE);
    const int pad = NS(12), eh = NS(18);
    N.replyEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                  pad, (hgt - eh) / 2, wdt - 2 * pad, eh, N.reply, NULL, g_inst, NULL);
    if (N.replyEdit) {
        SendMessageW(N.replyEdit, WM_SETFONT, (WPARAM)N.fReply, FALSE);
        SendMessageW(N.replyEdit, EM_LIMITTEXT, 500, 0);
        SendMessageW(N.replyEdit, 0x1501 /* EM_SETCUEBANNER */, TRUE,
                     (LPARAM)(w->inputHint[0] ? w->inputHint : L"Escribe una respuesta\x2026  \x21B5 envía \x00B7 Esc cancela"));
        s_editProc = (WNDPROC)SetWindowLongPtrW(N.replyEdit, GWLP_WNDPROC, (LONG_PTR)ReplyEditProc);
    }
    ShowWindow(N.reply, SW_SHOW);
    SetForegroundWindow(N.reply);
    if (N.replyEdit) SetFocus(N.replyEdit);
}

static void Click(int hit)
{
    switch (N.mode) {
    case M_PEEK:
        if (N.actMsg)                            { if (g_ctrl) PostMessageW(g_ctrl, N.actMsg, N.actW, 0); N.actMsg = 0; Close(); }
        else if (N.openPath[0])                  { OpenPath(); Close(); }
        else if (N.isCapture)                    Close();
        else if (N.peek.isNote || g_cfg.mirror)  Notch_OpenCenter();
        else                                     { Close(); if (g_ctrl) Panel_Show(); }
        break;
    case M_QUICK:
        if (hit == HT_Q_MUTE)      ToggleMute();
        else if (hit == HT_Q_OPEN) Notch_OpenCenter();
        break;
    case M_CENTER:
        if (N.outId)                 break;     /* ya hay una salida en curso */
        if (hit == HT_C_CLEAR)       { N.outId = -1; N.outT = 0; }
        else if (hit == HT_C_GEAR)   { Close(); Panel_ShowTab(1); }
        else if (hit == HT_C_MUTE)   ToggleMute();
        else if (hit >= HT_ACT) {
            const int i = (hit - HT_ACT) / 4, a = (hit - HT_ACT) % 4;
            const WinNote *w = Wn_Get(i);
            if (a == ACT_REPLY) { OpenReply(i); break; }
            Wn_Activate(w, a, NULL);                       /* como pulsar el botón en Windows */
            const BOOL fg = w->act[a].type != WA_BACKGROUND;
            N.outId = w->id; N.outT = 0; N.hot = HT_NONE;  /* y el aviso se retira */
            if (fg) N.hideAt = GetTickCount();
        }
        else if (hit >= HT_CARDX)    { N.outId = Wn_Get(hit - HT_CARDX)->id; N.outT = 0; N.hot = HT_NONE; }
        else if (hit >= HT_CARD)     { Wn_Activate(Wn_Get(hit - HT_CARD), -1, NULL); Close(); }
        break;
    }
}

static LRESULT CALLBACK NotchProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_MOUSEACTIVATE:
        return N.mode == M_CENTER ? MA_ACTIVATE : MA_NOACTIVATE;

    case WM_ACTIVATE:
        N.active = LOWORD(w) != WA_INACTIVE;
        if (!N.active && N.reply && (HWND)l == N.reply) return 0;     /* escribiendo la respuesta */
        if (!N.active && N.mode == M_CENTER && !N.closing) Close();
        return 0;

    case WM_KEYDOWN:
        if (w == VK_ESCAPE && N.mode == M_CENTER) Close();
        return 0;

    case WM_TIMER:
        if (w == TIMER_FRAME) Tick();
        return 0;

    case WM_NOTCH_FRAME:
        InterlockedExchange(&s_framePending, 0);
        if (N.mode != M_HIDDEN) Tick();
        return 0;

    case WM_MOUSEWHEEL:
        if (N.mode == M_CENTER && N.maxScroll > 0) {
            N.scroll -= GET_WHEEL_DELTA_WPARAM(w) * NS(60) / WHEEL_DELTA;
            N.scroll = max(0, min(N.scroll, N.maxScroll));
            N.scrollTick = GetTickCount();   /* el Tick anima el scroll y las escalas */
        }
        return 0;

    case WM_LBUTTONDOWN:
        N.down = TRUE;
        N.dragging = FALSE;
        N.downHit = HitAt((short)LOWORD(l), (short)HIWORD(l));
        GetCursorPos(&N.downPt);
        N.downTx  = N.tx;
        N.downTop = N.attached ? TopEdge() : N.ay;
        SetCapture(h);
        return 0;

    case WM_MOUSEMOVE: {
        if (!N.hover) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            N.hover = TrackMouseEvent(&tme);
            N.peekSince = GetTickCount();
        }
        if (N.down && N.mode == M_PEEK) {
            POINT p;
            GetCursorPos(&p);
            const int dx = p.x - N.downPt.x, dy = p.y - N.downPt.y;
            if (!N.dragging && abs(dx) + abs(dy) > NS(5)) N.dragging = TRUE;
            if (N.dragging) {
                MONITORINFO mi = { sizeof(mi) };
                if (GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi)) N.mon = mi.rcMonitor;
                const int topY = N.downTop + dy;
                N.tx = MagnetX(N.downTx + dx);              /* el resorte la sigue con rebote */
                /* imán al borde superior: notch pegado, o flotante a su separación */
                const BOOL snap = topY - TopEdge() < NS(28);
                N.attached = snap && !g_cfg.floating;
                N.ay = N.attached ? TopEdge() : snap ? TopEdge() + FloatGap() : topY;
                ClampTarget();
                N.dirty = TRUE;
            }
            return 0;
        }
        const int hit = HitAt((short)LOWORD(l), (short)HIWORD(l));
        N.mx = (short)LOWORD(l) - N.cox;
        if (hit != N.hot) {
            N.hot = hit;
            if (N.mode == M_CENTER || N.mode == M_QUICK) RenderContent();
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        N.hover = FALSE;
        N.peekSince = 0;
        N.mx = -100000;
        if (N.hot != HT_NONE) { N.hot = HT_NONE; if (N.mode == M_CENTER || N.mode == M_QUICK) RenderContent(); }
        if ((LONG)(N.hideAt - (GetTickCount() + 1200)) < 0) N.hideAt = GetTickCount() + 1200;
        return 0;

    case WM_LBUTTONUP: {
        if (!N.down) return 0;
        const BOOL wasDrag = N.dragging;
        const int hit = HitAt((short)LOWORD(l), (short)HIWORD(l));
        N.down = N.dragging = FALSE;
        ReleaseCapture();
        if (wasDrag) {
            SavePosition();
            N.hideAt = GetTickCount() + 1500;
        } else if (hit == N.downHit) {
            Click(hit);
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        N.down = N.dragging = FALSE;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ───────────────────────── API ───────────────────────── */
void Notch_Register(void)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = NotchProc;
    wc.hInstance     = g_inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_HAND);
    wc.lpszClassName = NOTCH_CLASS;
    RegisterClassExW(&wc);
}

static BOOL CanPeek(BOOL force)
{
    if (!force && !g_cfg.notch) return FALSE;
    if (N.mode == M_CENTER && !N.closing) return FALSE;     /* el panel abierto ya muestra todo */
    QUERY_USER_NOTIFICATION_STATE qs;
    if (!force && SUCCEEDED(SHQueryUserNotificationState(&qs)) &&
        (qs == QUNS_BUSY || qs == QUNS_RUNNING_D3D_FULL_SCREEN || qs == QUNS_PRESENTATION_MODE))
        return FALSE;
    return EnsureWindow();
}

static void StartPeek(DWORD hold)
{
    if (N.mode == M_HIDDEN || N.mode == M_MINI || N.mode == M_QUICK) {
        if (N.mode == M_HIDDEN) PlaceFromConfig(FALSE);
        else { AnchorY(); if (!N.attached) PlaceFromConfig(FALSE); }
    }
    N.hold = hold;
    N.hideAt = GetTickCount() + hold;
    N.pex = N.pexv = 0;             /* el aviso nuevo nace cerrado */
    Enter(M_PEEK);
}

BOOL Notch_Show(int icon, LPCWSTR title, LPCWSTR detail, int level, BOOL force)
{
    if (!CanPeek(force)) return FALSE;
    Canvas_Free(&N.thumb);
    ZeroMemory(&N.peek, sizeof(N.peek));
    N.peek.icon = icon;
    N.peek.level = level;
    lstrcpynW(N.peek.title, title, 128);
    lstrcpynW(N.peek.detail, detail ? detail : L"", 256);
    N.isCapture = FALSE;
    N.openPath[0] = 0;
    N.actMsg = 0;
    StartPeek(HOLD_MS);
    return TRUE;
}

BOOL Notch_ShowCapture(const BITMAPINFO *bi, const void *bits, LPCWSTR title, LPCWSTR detail)
{
    if (!CanPeek(FALSE)) return FALSE;
    if (N.mode == M_HIDDEN) PlaceFromConfig(FALSE);
    ZeroMemory(&N.peek, sizeof(N.peek));
    N.peek.icon = NI_CHECK;
    N.peek.level = -1;
    lstrcpynW(N.peek.title, title, 128);
    lstrcpynW(N.peek.detail, detail ? detail : L"", 256);

    /* copia escalada de la captura: el portapapeles solo es válido durante esta llamada */
    Canvas_Free(&N.thumb);
    if (bi) {
        const int sw = bi->bmiHeader.biWidth, sh = abs(bi->bmiHeader.biHeight);
        const int th = NS(28), tw = max(NS(20), min(sh > 0 ? MulDiv(th, sw, sh) : th, NS(52)));
        if (Canvas_Init(&N.thumb, tw, th)) {
            SetStretchBltMode(N.thumb.dc, HALFTONE);
            SetBrushOrgEx(N.thumb.dc, 0, 0, NULL);
            StretchDIBits(N.thumb.dc, 0, 0, tw, th, 0, 0, sw, sh, bits, bi, DIB_RGB_COLORS, SRCCOPY);
            GdiFlush();
        }
    }
    N.isCapture = TRUE;
    N.openPath[0] = 0;
    N.actMsg = 0;
    StartPeek(HOLD_SHOT);
    return TRUE;
}

/* La herramienta de recortes copia al portapapeles y además guarda el archivo:
 * si el aviso de esa captura sigue en pantalla, solo le asociamos el archivo. */
BOOL Notch_AttachFile(LPCWSTR path)
{
    if (!N.hwnd || N.mode != M_PEEK || N.closing || !N.isCapture) return FALSE;
    lstrcpynW(N.openPath, path, MAX_PATH);
    N.hideAt = GetTickCount() + N.hold;
    return TRUE;
}

BOOL Notch_ShowWin(const WinNote *n)
{
    if (!g_cfg.mirror || !CanPeek(FALSE)) return FALSE;
    Canvas_Free(&N.thumb);
    ZeroMemory(&N.peek, sizeof(N.peek));
    N.peek.isNote = TRUE;
    N.peek.level = -1;
    lstrcpynW(N.peek.aumid, n->aumid, 160);
    N.peek.arrival = n->arrival;
    lstrcpynW(N.peek.app, n->app, 64);
    lstrcpynW(N.peek.logo, n->logo, MAX_PATH);
    lstrcpynW(N.peek.title, n->title, 128);
    lstrcpynW(N.peek.detail, n->body, 256);
    N.isCapture = FALSE;
    N.openPath[0] = 0;
    N.actMsg = 0;
    if (g_cfg.hideBanners) {        /* ofuscada: nada del contenido, solo la campanita */
        N.peek.isBell = TRUE;
        N.peek.title[0] = N.peek.detail[0] = 0;
        ++N.unread;
        N.ringStart = GetTickCount() + 260;
    }
    Notch_PlaySound();
    StartPeek(g_cfg.hideBanners ? HOLD_SHOT : HOLD_NOTE);
    return TRUE;
}

/* Zona del borde superior donde puede nacer el mini notch: alrededor de la posición
 * del notch y lejos de las esquinas, donde están cerrar pestaña/ventana y los
 * controles de las apps. */
BOOL Notch_InEdgeZone(POINT pt, const RECT *mon, UINT dpi)
{
    const int width = mon->right - mon->left;
    const int anchor = mon->left + MulDiv(width, max(0, min(10000, g_cfg.notchX)), 10000);
    const int half = max(MulDiv(220, (int)dpi, 96), width * 18 / 100);
    const int margin = MulDiv(260, (int)dpi, 96);
    return abs(pt.x - anchor) <= half && pt.x >= mon->left + margin && pt.x <= mon->right - margin;
}

/* Cursor en el borde superior (lo detecta el controlador): nace la mini pastilla. */
void Notch_EdgeHover(POINT pt)
{
    if (!g_cfg.notch || !g_cfg.edgeHover) return;
    if (N.mode != M_HIDDEN && !(N.closing && (N.mode == M_MINI || N.mode == M_QUICK))) return;
    if (!EnsureWindow()) return;
    if (N.mode == M_HIDDEN) {
        UseMonitorAt(pt);
        N.ax = (float)pt.x;
        N.vax = 0;
    }
    N.attached = !g_cfg.floating;
    N.ay = TopEdge() + (g_cfg.floating ? FloatGap() : 0);
    N.tx = MagnetX(pt.x);
    N.dwellPt = pt;
    N.dwellSince = GetTickCount();
    Enter(M_MINI);
}

/* Aviso que hace algo al pulsarlo (p. ej. "Pulsa para actualizar"): dura más que uno normal. */
BOOL Notch_ShowAction(int icon, LPCWSTR title, LPCWSTR detail, UINT msg, WPARAM w)
{
    if (!Notch_Show(icon, title, detail, -1, TRUE)) return FALSE;
    N.actMsg = msg;
    N.actW = w;
    N.hold = HOLD_NOTE * 2;
    N.hideAt = GetTickCount() + N.hold;
    return TRUE;
}

void Notch_ToggleCenter(void)
{
    if (N.hwnd && N.mode == M_CENTER && !N.closing) Close();
    else Notch_OpenCenter();
}

void Notch_OpenCenter(void)
{
    if (!EnsureWindow()) return;
    if (N.mode == M_HIDDEN) PlaceFromConfig(TRUE);
    Canvas_Free(&N.thumb);
    N.isCapture = FALSE;
    N.openPath[0] = 0;
    N.actMsg = 0;
    Wn_Refresh(FALSE);
    N.unread = 0;
    Enter(M_CENTER);
}

void Notch_NotesChanged(void)
{
    ++s_cardStamp;
    if (N.hwnd && !N.closing && (N.mode == M_CENTER || N.mode == M_QUICK)) RenderContent();
}

void Notch_StyleChanged(void)
{
    ++s_cardStamp;
    if (!N.hwnd || N.mode == M_HIDDEN) return;
    LoadLook();
    MakeFonts();
    Notch_ApplyCapture();
    if (N.mode == M_MINI || N.mode == M_QUICK) { N.attached = !g_cfg.floating; N.ay = TopEdge() + (g_cfg.floating ? FloatGap() : 0); }
    else AnchorY();
    RenderContent();
}

void Notch_Raise(void)
{
    if (N.hwnd && N.mode != M_HIDDEN && App_Covered(N.hwnd))
        SetWindowPos(N.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
}

void Notch_ApplyCapture(void)
{
    const BOOL exclude = App_HideFromCapture(g_cfg.material == MAT_GLASS);
    if (N.hwnd) SetWindowDisplayAffinity(N.hwnd, exclude ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
}

void Notch_QuitOnHide(void)
{
    N.quitOnHide = TRUE;
}

void Notch_Destroy(void)
{
    if (N.hwnd) DestroyWindow(N.hwnd);
    Canvas_Free(&N.content);
    Canvas_Free(&N.frame);
    Canvas_Free(&N.thumb);
    Canvas_Free(&N.back);
    Canvas_Free(&N.card);
    Canvas_Free(&N.bell);
    for (int i = 0; i < WN_MAX; ++i) Canvas_Free(&s_cards[i].cv);
    HFONT *all[] = { &N.fTitle, &N.fBody, &N.fSmall, &N.fHeader, &N.fIcon, &N.fIconSm, &N.fTrash };
    for (int i = 0; i < 7; ++i) if (*all[i]) DeleteObject(*all[i]);
    ZeroMemory(&N, sizeof(N));
}
