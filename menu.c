/*
 * menu.c — menú contextual propio (en lugar del de Win32), con el material del notch, la
 * barra y el dock y el muelle de las ventanas emergentes.
 *
 * Menu_Track funciona como TrackPopupMenu con TPM_RETURNCMD: abre el menú encima del
 * punto dado, lleva su propio bucle de mensajes (el resto de OpenDock sigue animándose)
 * y devuelve el id elegido, o 0 si se cerró sin elegir. Cada opción lleva un icono de
 * Segoe Fluent; lo señalado no cambia de fondo: su icono crece y su texto se aparta, como
 * en el buscador. Flechas, Intro y Esc también valen; un clic fuera lo cierra.
 */
#include "app.h"
#include <math.h>

#define MN_CLASS    L"OpenDock.Menu"
#define MN_MAX      48
#define ROW_H       34
#define SEP_H       11
#define HEAD_H      28
#define PAD_Y       6

static struct {
    HWND     hwnd;
    Pop      pop;
    Canvas   cv, bgc;               /* bgc: el fondo (vidrio teñido o liso), hecho una vez al abrir */
    PopGlass glass;
    struct { int px; HFONT f; } gf[12];     /* iconos a cada tamaño del muelle */
    UINT     dpi;
    HFONT    fText, fHead;
    MenuItem it[MN_MAX];
    int      n, w, h, ox, oy;
    int      y[MN_MAX], ih[MN_MAX];
    int      hot, press;
    float    s[MN_MAX], sv[MN_MAX];
    BOOL     light, done, anim, tracking;
    int      result;
    DWORD    bg, fg, fg2, fg3, line, danger;
    LARGE_INTEGER last;
} M;

static int MS(int v) { return MulDiv(v, (int)M.dpi, 96); }

static BOOL Selectable(int i)
{
    return i >= 0 && i < M.n && !(M.it[i].flags & (MI_SEPARATOR | MI_HEADER | MI_DISABLED));
}

static int HitRow(int x, int y)
{
    if (x < 0 || x >= M.w) return -1;
    for (int i = 0; i < M.n; ++i) if (y >= M.y[i] && y < M.y[i] + M.ih[i]) return Selectable(i) ? i : -1;
    return -1;
}

/* Fuente de iconos a un tamaño: el muelle pasa por pocos tamaños, se guardan los últimos. */
static HFONT GlyphFont(int px)
{
    static int next;
    for (int i = 0; i < 12; ++i) if (M.gf[i].f && M.gf[i].px == px) return M.gf[i].f;
    const int k = next++ % 12;
    if (M.gf[k].f) DeleteObject(M.gf[k].f);
    M.gf[k].px = px;
    M.gf[k].f = Gfx_Font(Gfx_IconFace(), px, FW_NORMAL, ANTIALIASED_QUALITY);
    return M.gf[k].f;
}

static void Paint(void)
{
    if (!M.hwnd) return;
    if (M.cv.w != M.w || M.cv.h != M.h) {
        Canvas_Free(&M.cv);
        if (!Canvas_Init(&M.cv, M.w, M.h)) return;
    }
    Canvas *c = &M.cv;
    GdiFlush();
    if (M.bgc.px) CopyMemory(c->px, M.bgc.px, (SIZE_T)M.w * M.h * 4);
    else Canvas_Clear(c, M.bg);
    const int pad = MS(8);
    for (int i = 0; i < M.n; ++i) {
        const MenuItem *m = &M.it[i];
        const int y = M.y[i], h = M.ih[i];
        if (m->flags & MI_SEPARATOR) {
            Gfx_FillRRect(c, (float)MS(14), (float)(y + h / 2), (float)(M.w - MS(28)), 1.0f, 0, M.line, 1.0f);
            continue;
        }
        if (m->flags & MI_HEADER) {
            Gfx_Text(c, M.fHead, m->text, MS(16), y, M.w - MS(32), h, M.fg3, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            continue;
        }
        const BOOL hot = i == M.hot, off = (m->flags & MI_DISABLED) != 0;
        const DWORD base = m->flags & MI_DANGER ? M.danger : M.fg;
        const DWORD col = off ? M.fg3 : hot ? base : Gfx_Mix(M.bg, base, 0.86f);
        const float sc = M.s[i];
        const int gx = pad + MS(8), gw = MS(22), nudge = (int)lroundf((sc - 1.0f) * MS(18));
        if (m->glyph || (m->flags & MI_CHECKED)) {
            /* el icono crece con el muelle: se dibuja a su tamaño escalado */
            const HFONT gf = GlyphFont(max(8, (int)lroundf(MS(15) * sc)));
            const WCHAR g[2] = { m->flags & MI_CHECKED ? 0xE73E : m->glyph, 0 };
            if (gf) Gfx_Text(c, gf, g, gx - MS(4), y, gw + MS(8), h, m->flags & MI_CHECKED ? M.fg : col, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        const int tx = gx + gw + MS(10) + nudge;
        Gfx_Text(c, M.fText, m->text, tx, y, M.w - tx - MS(14), h, col, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    Pop_Present(&M.pop, c);
}

static void Spring(float *x, float *v, float t, float dt, float k, float z)
{
    const float a = (t - *x) * k - *v * 2.0f * sqrtf(k) * z;
    *v += a * dt; *x += *v * dt;
}

static void Kick(void)
{
    if (!M.hwnd) return;
    if (!M.anim) { QueryPerformanceCounter(&M.last); M.anim = TRUE; }     /* sólo si estaba parado */
    Pop_Hold(&M.pop, TRUE);
}

static BOOL Advance(void)
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    float dt = (float)(t.QuadPart - M.last.QuadPart) / (float)f.QuadPart;
    M.last = t;
    if (dt > 0.05f) dt = 0.05f;
    const float z = Pop_Zeta();
    BOOL moving = FALSE;
    for (int i = 0; i < M.n; ++i) {
        const float tg = i == M.press ? 0.9f : i == M.hot ? 1.22f : 1.0f;
        for (int k = 0; k < 2; ++k) Spring(&M.s[i], &M.sv[i], tg, dt * 0.5f, 420.0f, z);
        if (fabsf(M.s[i] - tg) < 0.0008f && fabsf(M.sv[i]) < 0.01f) { M.s[i] = tg; M.sv[i] = 0; }
        else moving = TRUE;
    }
    return moving;
}

static void Finish(int id)
{
    if (M.done) return;
    M.result = id;
    M.done = TRUE;
    if (M.hwnd) Pop_Close(&M.pop);
}

static void SetHot(int i)
{
    if (i == M.hot) return;
    M.hot = i;
    Kick();
}

static void Step(int dir)
{
    int i = M.hot;
    for (int k = 0; k < M.n; ++k) {
        i = (i < 0 ? (dir > 0 ? -1 : M.n) : i) + dir;
        if (i < 0) i = M.n - 1;
        if (i >= M.n) i = 0;
        if (Selectable(i)) { SetHot(i); return; }
    }
}

static LRESULT CALLBACK MenuProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEWHEEL && m != WM_MOUSEHWHEEL) l = Pop_Mouse(&M.pop, l);
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        Paint();
        return 0;
    }
    case WM_NCHITTEST: return Pop_NcHit(&M.pop, l);
    case WM_MOUSEACTIVATE: return MA_ACTIVATE;
    case WM_POPFRAME:
        Pop_Step(&M.pop);
        if (!M.hwnd) return 0;
        if (M.anim) {
            const BOOL moving = Advance();
            Paint();
            if (!moving) { M.anim = FALSE; Pop_Hold(&M.pop, FALSE); }
        } else if (M.cv.px) Pop_Present(&M.pop, &M.cv);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) Finish(0);
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) Finish(0);
        else if (w == VK_DOWN || w == VK_TAB) Step(1);
        else if (w == VK_UP) Step(-1);
        else if (w == VK_RETURN || w == VK_SPACE) { if (Selectable(M.hot)) Finish(M.it[M.hot].id); }
        return 0;
    case WM_MOUSEMOVE:
        if (!M.tracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            M.tracking = TrackMouseEvent(&tme);
        }
        SetHot(HitRow((short)LOWORD(l), (short)HIWORD(l)));
        return 0;
    case WM_MOUSELEAVE:
        M.tracking = FALSE;
        M.press = -1;
        SetHot(-1);
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN:
        M.press = HitRow((short)LOWORD(l), (short)HIWORD(l));
        Kick();
        return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: {
        const int hit = HitRow((short)LOWORD(l), (short)HIWORD(l)), was = M.press;
        M.press = -1;
        if (hit >= 0 && hit == was) Finish(M.it[hit].id);
        else Kick();
        return 0;
    }
    case WM_DESTROY:
        Pop_Destroyed(&M.pop);
        Pop_FreeGlass(&M.glass);
        Canvas_Free(&M.cv);
        Canvas_Free(&M.bgc);
        for (int i = 0; i < 12; ++i) if (M.gf[i].f) { DeleteObject(M.gf[i].f); M.gf[i].f = NULL; }
        if (M.fText) { DeleteObject(M.fText); M.fText = NULL; }
        if (M.fHead) { DeleteObject(M.fHead); M.fHead = NULL; }
        M.hwnd = NULL;
        if (!M.done) { M.done = TRUE; M.result = 0; }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void Menu_Register(void)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = MenuProc;
    wc.hInstance     = g_inst;
    wc.lpszClassName = MN_CLASS;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassExW(&wc);
}

int Menu_Track(const MenuItem *items, int n, POINT at)
{
    static BOOL busy;           /* no se anida: un clic fuera cierra el que hay */
    if (busy || n <= 0) return 0;
    if (M.hwnd) DestroyWindow(M.hwnd);      /* el anterior, aún cerrándose */
    M.n = min(n, MN_MAX);
    CopyMemory(M.it, items, M.n * sizeof(MenuItem));
    HMONITOR hm = MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hm, &mi);
    M.dpi = MonitorDpi(hm);
    LPCWSTR ui = Gfx_UiFace();
    M.fText = Gfx_Font(ui, MS(14), FW_NORMAL, CLEARTYPE_QUALITY);
    M.fHead = Gfx_Font(ui, MS(12), FW_SEMIBOLD, CLEARTYPE_QUALITY);

    Theme th;
    Theme_Load(&th);
    M.light = g_cfg.material == MAT_SYSTEM && !th.dark;
    if (M.light) { M.bg = 0xE5E5EA; M.fg = 0x1C1C1E; M.fg2 = 0x55555A; M.fg3 = 0x8E8E93; M.line = 0xC7C7CC; M.danger = 0xD70015; }
    else {
        M.bg = g_cfg.material == MAT_OLED ? 0x000000 : g_cfg.material == MAT_GLASS ? 0x262628 : 0x161618;
        M.fg = 0xFFFFFF; M.fg2 = 0xB8B8BD; M.fg3 = 0x8E8E93; M.line = 0x3A3A3C; M.danger = 0xFF6961;
    }

    /* medidas: ancho por el texto más largo */
    int wmax = 0, y = MS(PAD_Y);
    for (int i = 0; i < M.n; ++i) {
        const MenuItem *m = &M.it[i];
        M.y[i] = y;
        M.ih[i] = MS(m->flags & MI_SEPARATOR ? SEP_H : m->flags & MI_HEADER ? HEAD_H : ROW_H);
        y += M.ih[i];
        if (m->text) wmax = max(wmax, Gfx_TextWidth(m->flags & MI_HEADER ? M.fHead : M.fText, m->text));
        M.s[i] = 1.0f; M.sv[i] = 0;
    }
    M.w = max(MS(200), min(MS(360), wmax + MS(8 + 8 + 22 + 10 + 30)));
    M.h = y + MS(PAD_Y);

    /* encima del punto (el dock está abajo), centrado y dentro del monitor */
    const RECT *wk = &mi.rcWork;
    M.ox = max((int)wk->left + MS(8), min((int)wk->right - MS(8) - M.w, (int)at.x - M.w / 2));
    M.oy = at.y - M.h - MS(6);
    if (M.oy < wk->top + MS(8)) M.oy = at.y + MS(6);
    Canvas_Free(&M.bgc);
    if (Canvas_Init(&M.bgc, M.w, M.h)) {
        if (M.light) Canvas_Clear(&M.bgc, 0xE5E5EA);
        else if (g_cfg.material == MAT_GLASS) {
            Pop_CaptureGlass(&M.glass, M.ox, M.oy, M.w, M.h, MS(28));
            Pop_PaintGlass(&M.bgc, &M.glass, M.ox, M.oy, 0x1C1C1E, 0.58f);
        } else Canvas_Clear(&M.bgc, M.bg);
    }

    M.hot = M.press = -1;
    M.done = FALSE; M.result = 0; M.anim = FALSE; M.tracking = FALSE;
    M.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, MN_CLASS, L"Menú", WS_POPUP,
                             M.ox, M.oy, M.w, M.h, NULL, NULL, g_inst, NULL);
    if (!M.hwnd) return 0;
    if (g_cfg.hideCapture) SetWindowDisplayAffinity(M.hwnd, WDA_EXCLUDEFROMCAPTURE);
    const float ax = max(0.05f, min(0.95f, (at.x - M.ox) / (float)M.w));
    Pop_Open(&M.pop, M.hwnd, MS(24), (float)MS(14), ax, M.oy < at.y ? 1.0f : 0.0f);
    Pop_SetBounds(&M.pop, M.ox, M.oy, M.w, M.h);
    Paint();
    ShowWindow(M.hwnd, SW_SHOW);
    SetForegroundWindow(M.hwnd);

    /* bucle propio hasta elegir o cerrar (como TrackPopupMenu) */
    busy = TRUE;
    MSG msg;
    while (!M.done) {
        const BOOL r = GetMessageW(&msg, NULL, 0, 0);
        if (r <= 0) { if (r == 0) PostQuitMessage((int)msg.wParam); break; }
        /* un clic en otra ventana nuestra (el dock no toma el foco, así que el menú no se
         * entera por WM_ACTIVATE): cierra el menú y ese clic no hace nada más, como en Win32 */
        const UINT mm = msg.message;
        if (msg.hwnd != M.hwnd && (mm == WM_LBUTTONDOWN || mm == WM_RBUTTONDOWN || mm == WM_MBUTTONDOWN ||
                                   mm == WM_NCLBUTTONDOWN || mm == WM_NCRBUTTONDOWN || mm == WM_LBUTTONUP || mm == WM_RBUTTONUP)) {
            Finish(0);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    busy = FALSE;
    return M.result;
}
