/*
 * pop.c — ventanas emergentes propias (centro de control, ajustes) que se abren y se
 * cierran con el mismo muelle que el notch: el "Rebote" configurado (suave · normal ·
 * bouncy) decide cuánto rebotan al aparecer.
 *
 * La ventana es layered y algo mayor que su contenido (margen para la sombra y para que
 * el rebote no se recorte). El contenido se pinta como siempre en un Canvas opaco; aquí
 * se compone escalado alrededor de su ancla (la esquina de la que "sale"), con esquinas
 * redondeadas por distancia con signo y una sombra suave, y se presenta con
 * UpdateLayeredWindow. En reposo se compone sin escalar (solo copia y esquinas).
 */
#include "app.h"
#include <math.h>

#define MAX_POPS 4

/* ───────────────────────── Marcapasos ───────────────────────── */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
static Pop *volatile s_live[MAX_POPS];
static HANDLE s_on;

static DWORD WINAPI PopPacer(LPVOID u)
{
    (void)u;
    HANDLE t = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!t) t = CreateWaitableTimerW(NULL, FALSE, NULL);
    if (!t) return 0;
    for (;;) {
        WaitForSingleObject(s_on, INFINITE);
        LARGE_INTEGER due;
        due.QuadPart = -10000000LL / 120;
        SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE);
        WaitForSingleObject(t, 50);
        BOOL any = FALSE;
        for (int i = 0; i < MAX_POPS; ++i) {
            Pop *p = s_live[i];
            if (!p) continue;
            any = TRUE;
            if (p->hwnd && !InterlockedExchange(&p->queued, 1)) PostMessageW(p->hwnd, WM_POPFRAME, 0, 0);
        }
        if (!any) ResetEvent(s_on);
    }
}

static void Live(Pop *p, BOOL on)
{
    if (!s_on) {
        s_on = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE th = s_on ? CreateThread(NULL, 0, PopPacer, NULL, 0, NULL) : NULL;
        if (th) CloseHandle(th);
    }
    int slot = -1;
    for (int i = 0; i < MAX_POPS; ++i) {
        if (s_live[i] == p) { if (!on) s_live[i] = NULL; return; }
        if (!s_live[i] && slot < 0) slot = i;
    }
    if (on && slot >= 0) {
        s_live[slot] = p;
        if (s_on) SetEvent(s_on);
    }
}

/* ───────────────────────── Muelle ───────────────────────── */
float Pop_Zeta(void)
{
    static const float kZeta[3] = { 0.85f, 0.62f, 0.40f };     /* los del notch */
    return kZeta[max(0, min(2, g_cfg.bounce))];
}

static void Spring(float *x, float *v, float target, float dt, float k, float z)
{
    const float a = (target - *x) * k - *v * 2.0f * sqrtf(k) * z;
    *v += a * dt; *x += *v * dt;
}

/* ───────────────────────── Composición ───────────────────────── */
static void FreeStatic(Pop *p)
{
    if (p->base) HeapFree(GetProcessHeap(), 0, p->base);
    if (p->mask) HeapFree(GetProcessHeap(), 0, p->mask);
    p->base = NULL; p->mask = NULL;
}

static DWORD Over(DWORD src, int cov, DWORD under);
static float Smooth(float t) { t = t < 0 ? 0 : t > 1 ? 1 : t; return t * t * (3 - 2 * t); }

/* Sombra de una tarjeta (x, y, w, h, r) en el punto (px, py): negra, suave, algo caída. */
static float ShadowA(const Pop *p, float px, float py, float x, float y, float w, float h, float r)
{
    const float M = (float)p->margin, drop = M * 0.28f;
    const float sd = Gfx_SdRRect(px, py, x, y + drop, w, h, r);
    if (sd <= -M * 0.5f) return p->shadow;
    return p->shadow * (1.0f - Smooth((sd + M * 0.5f) / (M * 1.2f)));
}

static void Ulw(Pop *p)
{
    RECT wr;
    GetWindowRect(p->hwnd, &wr);
    POINT dst = { wr.left, wr.top }, src = { 0, 0 };
    SIZE sz = { p->frame.w, p->frame.h };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC screen = GetDC(NULL);
    UpdateLayeredWindow(p->hwnd, screen, &dst, &sz, p->frame.dc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
}

/* En reposo: la sombra y la máscara de esquinas se calculan una vez; luego cada
 * repintado solo copia el contenido. */
static BOOL BuildStatic(Pop *p, int cw, int ch)
{
    FreeStatic(p);
    const int W = p->frame.w, H = p->frame.h, M = p->margin, T = p->mtop;
    const float lift = p->attached ? p->radius : 0.0f;      /* pegada: esquinas de arriba rectas */
    p->base = (DWORD *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)W * H * 4);
    p->mask = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)cw * ch);
    if (!p->base || !p->mask) { FreeStatic(p); return FALSE; }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float a = ShadowA(p, x + 0.5f, y + 0.5f, (float)M, T - lift, (float)cw, ch + lift, p->radius);
            p->base[y * W + x] = (DWORD)(a * 255.0f + 0.5f) << 24;          /* negro premultiplicado */
        }
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x)
            p->mask[y * cw + x] = (BYTE)(Gfx_Cov(Gfx_SdRRect(x + 0.5f, y + 0.5f, 0, -lift, (float)cw, ch + lift, p->radius)) * 255.0f + 0.5f);
    p->sw = cw; p->sh = ch;
    return TRUE;
}

static DWORD Over(DWORD src, int cov, DWORD under)
{
    /* contenido opaco con cobertura cov (0..255) sobre "under" premultiplicado */
    const int ia = 255 - cov;
    const DWORD r = (((src >> 16) & 255) * cov + ((under >> 16) & 255) * ia) / 255;
    const DWORD g = (((src >> 8) & 255) * cov + ((under >> 8) & 255) * ia) / 255;
    const DWORD b = ((src & 255) * cov + (under & 255) * ia) / 255;
    const DWORD a = (255 * cov + (under >> 24) * ia) / 255;
    return a << 24 | r << 16 | g << 8 | b;
}

/* Hombros cóncavos (estilo notch): el color de la fila de arriba se curva hacia fuera. */
static void Ears(const Pop *p, DWORD *out, int W, const Canvas *content, int x0, int x1, int top, float alpha)
{
    if (!p->attached || p->ear < 1) return;
    const int e = (int)ceilf(p->ear);
    for (int side = 0; side < 2; ++side) {
        const DWORD col = content->px[side ? content->w - 1 : 0] & 0xFFFFFF;
        const float cx = side ? x1 + p->ear : x0 - p->ear, cy = top + p->ear;
        for (int y = top; y < top + e; ++y)
            for (int x = side ? x1 : x0 - e; x < (side ? x1 + e : x0); ++x) {
                if (x < 0 || x >= W) continue;
                const float d = hypotf(x + 0.5f - cx, y + 0.5f - cy);
                const float cov = max(0.0f, min(1.0f, d - p->ear + 0.5f)) * alpha;
                if (cov > 0.002f) out[y * W + x] = Over(col, (int)(cov * 255.0f + 0.5f), out[y * W + x]);
            }
    }
}

void Pop_Present(Pop *p, const Canvas *content)
{
    if (!p->hwnd || !content || !content->px) return;
    p->content = content;
    const int M = p->margin, T = p->mtop, cw = content->w, ch = content->h, W = cw + 2 * M, H = ch + M + T;
    p->cw = cw; p->ch = ch;
    if (p->frame.w != W || p->frame.h != H) {
        Canvas_Free(&p->frame);
        FreeStatic(p);
        if (!Canvas_Init(&p->frame, W, H)) return;
    }
    GdiFlush();
    DWORD *out = p->frame.px;
    const BOOL rest = !p->closing && p->a >= 0.999f && (p->attached
                      ? p->fade >= 0.999f && fabsf(p->aw - cw) < 0.5f && fabsf(p->ah - ch) < 0.5f
                      : fabsf(p->s - 1.0f) < 0.0008f);

    if (rest) {
        if ((p->sw != cw || p->sh != ch || !p->base) && !BuildStatic(p, cw, ch)) return;
        CopyMemory(out, p->base, (SIZE_T)W * H * 4);
        for (int y = 0; y < ch; ++y) {
            const DWORD *srow = &content->px[y * cw];
            const BYTE *mrow = &p->mask[y * cw];
            DWORD *drow = &out[(y + T) * W + M];
            for (int x = 0; x < cw; ++x) {
                const int m = mrow[x];
                if (m == 255) drow[x] = 0xFF000000 | (srow[x] & 0xFFFFFF);
                else if (m) drow[x] = Over(srow[x], m, drow[x]);
            }
        }
        Ears(p, out, W, content, M, M + cw, T, 1.0f);
        Ulw(p);
        return;
    }

    if (p->attached) {
        /* estilo notch, igual que el panel de notificaciones: la forma nace de una pastilla
         * bajo el engrane y crece en ancho y alto con el muelle configurado; el contenido va
         * pegado a su borde de abajo y aparece cuando ya hay sitio */
        ZeroMemory(out, (SIZE_T)W * H * 4);
        const float A = max(0.0f, min(1.0f, p->a)), F = max(0.0f, min(1.0f, p->fade));
        const float aw = max(2.0f, min((float)(W - 4), p->aw)), ah = max(0.0f, min((float)(H - 2), p->ah));
        const float pivot = M + p->ax * cw, left = pivot - aw * p->ax, right = left + aw;
        const float rb = max(0.5f, min(ah * 0.5f, p->radius)), fl = min(p->ear, ah * 0.5f);
        const int cox = M, coy = (int)lroundf(ah - ch);
        const DWORD bg = content->px[0] & 0xFFFFFF;
        const int f8 = (int)(F * 256.0f + 0.5f), a8 = (int)(A * 255.0f + 0.5f);
        /* solo se recorre la forma y su sombra; el interior se copia sin calcular nada */
        const float ext = M * 1.3f;
        const int sx0 = max(0, (int)floorf(left - fl - ext)), sx1 = min(W, (int)ceilf(right + fl + ext));
        const int sy1 = min(H, (int)ceilf(ah + M * 0.28f + ext));
        const int ix0 = (int)ceilf(left + 1.0f), ix1 = (int)floorf(right - 1.0f);
        const DWORD shadowFull = (DWORD)(p->shadow * A * 255.0f + 0.5f) << 24;
        for (int y = 0; y < sy1; ++y) {
            DWORD *drow = &out[y * W];
            const float py = y + 0.5f;
            const int qy = y - coy;
            const BOOL midRow = py >= fl && py < ah - rb;          /* sin esquinas ni hombros */
            const DWORD *crow = qy >= 0 && qy < ch ? &content->px[qy * cw] : NULL;
            for (int x = sx0; x < sx1; ++x) {
                if (midRow && x >= ix0 && x < ix1) {
                    const int qx = x - cox;
                    DWORD c = crow && qx >= 0 && qx < cw ? crow[qx] & 0xFFFFFF : bg;
                    if (f8 < 256) {                                /* aún apareciendo */
                        const DWORD rb2 = (((bg & 0xFF00FF) * (256 - f8) + (c & 0xFF00FF) * f8) >> 8) & 0xFF00FF;
                        const DWORD g2 = (((bg & 0x00FF00) * (256 - f8) + (c & 0x00FF00) * f8) >> 8) & 0x00FF00;
                        c = rb2 | g2;
                    }
                    drow[x] = a8 >= 255 ? 0xFF000000 | c : Over(c, a8, shadowFull);
                    continue;
                }
                const float px = x + 0.5f;
                const float sa = ShadowA(p, px, py, left, -rb, aw, ah + rb, rb) * A;
                float cov = 0;
                if (py < ah + 1.5f && px > left - fl - 1 && px < right + fl + 1) {
                    cov = Gfx_Cov(Gfx_SdRRect(px, py, left, -rb, aw, ah + rb, rb));
                    if (fl > 0.5f && py < fl) {           /* hombros cóncavos junto a la barra */
                        float ccx = -1;
                        if (px < left && px > left - fl)        ccx = left - fl;
                        else if (px > right && px < right + fl) ccx = right + fl;
                        if (ccx >= 0) cov = max(cov, min(1.0f, hypotf(px - ccx, py - fl) - fl + 0.5f));
                    }
                }
                cov *= A;
                const DWORD under = (DWORD)(sa * 255.0f + 0.5f) << 24;
                if (cov <= 0.002f) { if (sa > 0.002f) drow[x] = under; continue; }
                const int qx = x - cox;
                DWORD c = bg;
                if (F > 0 && crow && qx >= 0 && qx < cw) c = Gfx_Mix(bg, crow[qx] & 0xFFFFFF, F);
                drow[x] = Over(c, (int)(cov * 255.0f + 0.5f), under);
            }
        }
        Ulw(p);
        return;
    }

    /* animando: contenido escalado alrededor del ancla y desvanecido */
    ZeroMemory(out, (SIZE_T)W * H * 4);
    const float s = max(0.05f, p->s), A = max(0.0f, min(1.0f, p->a));
    const float axp = M + p->ax * cw, ayp = T + p->ay * ch;
    const float x0 = axp + (M - axp) * s, y0 = ayp + (T - ayp) * s, w = cw * s, h = ch * s, r = p->radius * s;
    const float inv = 1.0f / s, ext = M * 1.3f;
    const int bx0 = max(0, (int)floorf(x0 - ext)), bx1 = min(W, (int)ceilf(x0 + w + ext));
    const int by0 = max(0, (int)floorf(y0 - ext)), by1 = min(H, (int)ceilf(y0 + h + M * 0.28f + ext));
    for (int y = by0; y < by1; ++y) {
        DWORD *drow = &out[y * W];
        for (int x = bx0; x < bx1; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            const float sa = ShadowA(p, px, py, x0, y0, w, h, r) * A;
            const float cov = Gfx_Cov(Gfx_SdRRect(px, py, x0, y0, w, h, r)) * A;
            if (cov <= 0.002f) {
                if (sa > 0.002f) drow[x] = (DWORD)(sa * 255.0f + 0.5f) << 24;
                continue;
            }
            /* muestreo bilineal del contenido */
            const float sx = (px - axp) * inv + axp - M - 0.5f, sy = (py - ayp) * inv + ayp - T - 0.5f;
            const int ix = (int)floorf(sx), iy = (int)floorf(sy);
            const float fx = sx - ix, fy = sy - iy;
            const int xa = max(0, min(cw - 1, ix)), xb = max(0, min(cw - 1, ix + 1));
            const int ya = max(0, min(ch - 1, iy)), yb = max(0, min(ch - 1, iy + 1));
            const DWORD c = Gfx_Mix(Gfx_Mix(content->px[ya * cw + xa], content->px[ya * cw + xb], fx),
                                    Gfx_Mix(content->px[yb * cw + xa], content->px[yb * cw + xb], fx), fy);
            const DWORD under = (DWORD)(sa * 255.0f + 0.5f) << 24;
            drow[x] = Over(c, (int)(cov * 255.0f + 0.5f), under);
        }
    }
    Ulw(p);
}

/* ───────────────────────── API ───────────────────────── */
void Pop_Open(Pop *p, HWND h, int margin, float radius, float ax, float ay)
{
    Canvas_Free(&p->frame);
    FreeStatic(p);
    p->hwnd = h;
    p->margin = margin;
    p->mtop = margin;
    p->attached = FALSE;
    p->ear = 0;
    p->hold = FALSE;
    p->radius = radius;
    p->ax = ax; p->ay = ay;
    p->s = 0.86f; p->vs = 0; p->a = 0;
    p->shadow = 0.34f;
    p->closing = FALSE;
    p->queued = 0;
    p->content = NULL;
    p->sw = p->sh = 0;
    QueryPerformanceCounter(&p->last);
    Live(p, TRUE);
}

void Pop_Close(Pop *p)
{
    if (!p->hwnd || p->closing) return;
    p->closing = TRUE;
    p->vs = 0;
    QueryPerformanceCounter(&p->last);
    Live(p, TRUE);
}

void Pop_Destroyed(Pop *p)
{
    Live(p, FALSE);
    Canvas_Free(&p->frame);
    FreeStatic(p);
    p->hwnd = NULL;
    p->content = NULL;
    p->closing = FALSE;
}

/* Estilo notch: pegada arriba (sin margen superior); nace de una pastilla de ancho pillW. */
void Pop_SetAttached(Pop *p, float ear, float pillW)
{
    p->attached = TRUE;
    p->ear = ear;
    p->mtop = 0;
    p->pillW = pillW;
    p->aw = pillW; p->ah = 0; p->vaw = p->vah = 0;
    p->fade = 0;
    p->a = 0.4f;
}

void Pop_Hold(Pop *p, BOOL on)
{
    p->hold = on;
    if (on && p->hwnd) Live(p, TRUE);
}

/* Un paso del muelle (sin presentar); al terminar de cerrarse destruye la ventana. */
void Pop_Step(Pop *p)
{
    InterlockedExchange(&p->queued, 0);
    if (!p->hwnd) return;
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    float dt = (float)(t.QuadPart - p->last.QuadPart) / (float)f.QuadPart;
    p->last = t;
    if (dt > 0.05f) dt = 0.05f;

    if (p->attached) {
        /* los mismos muelles que la isla del notch al abrir su panel */
        const float tw = p->closing ? p->pillW : (float)p->cw, th = p->closing ? 0.0f : (float)p->ch;
        for (int k = 0; k < 2; ++k) {
            if (p->closing) {       /* sin rebote al cerrar: ancho y alto llegan juntos */
                Spring(&p->aw, &p->vaw, tw, dt * 0.5f, 300.0f, 1.0f);
                Spring(&p->ah, &p->vah, th, dt * 0.5f, 300.0f, 1.0f);
            } else {
                /* algo más suave que la isla: es un panel grande y el mismo rebote se ve brusco */
                Spring(&p->aw, &p->vaw, tw, dt * 0.5f, 220.0f, min(1.0f, Pop_Zeta() + 0.22f));
                Spring(&p->ah, &p->vah, th, dt * 0.5f, 250.0f, min(1.0f, Pop_Zeta() + 0.28f));
            }
        }
        if (!p->closing) {
            const float tf = p->ah > p->ch * 0.42f ? 1.0f : 0.0f;     /* el contenido, cuando hay sitio */
            p->fade += (tf - p->fade) * min(1.0f, dt * (tf > p->fade ? 11.0f : 26.0f));
            p->a += (1.0f - p->a) * min(1.0f, dt * 10.0f);
            if (p->a > 0.999f) p->a = 1.0f;
            if (p->fade > 0.999f) p->fade = 1.0f;
            const BOOL settled = fabsf(p->aw - tw) < 0.3f && fabsf(p->vaw) < 3.0f && fabsf(p->ah - th) < 0.3f &&
                                 fabsf(p->vah) < 3.0f && p->fade >= 1.0f && p->a >= 1.0f;
            if (settled) { p->aw = tw; p->ah = th; p->vaw = p->vah = 0; if (!p->hold) Live(p, FALSE); }
        } else {
            /* contenido y forma ligados a la altura: nada se apaga de golpe */
            const float prog = max(0.0f, min(1.0f, p->ah / max(1.0f, (float)p->ch)));
            const float f = max(0.0f, min(1.0f, (prog - 0.25f) / 0.5f));
            p->fade = min(p->fade, f * f * (3 - 2 * f));
            if (p->ah < 0.6f && fabsf(p->vah) < 40.0f) {
                Live(p, FALSE);
                DestroyWindow(p->hwnd);
            }
        }
        return;
    }

    if (p->closing) {
        for (int k = 0; k < 2; ++k) Spring(&p->s, &p->vs, 0.92f, dt * 0.5f, 520.0f, 1.0f);
        p->a -= dt / 0.13f;
        if (p->a <= 0.0f) {
            p->a = 0;
            Live(p, FALSE);
            DestroyWindow(p->hwnd);         /* el dueño llama a Pop_Destroyed en WM_DESTROY */
            return;
        }
    } else {
        for (int k = 0; k < 2; ++k) Spring(&p->s, &p->vs, 1.0f, dt * 0.5f, 380.0f, Pop_Zeta());
        p->a = min(1.0f, p->a + dt / 0.12f);
        if (fabsf(p->s - 1.0f) < 0.0008f && fabsf(p->vs) < 0.01f && p->a >= 1.0f) {
            p->s = 1.0f; p->vs = 0;
            if (!p->hold) Live(p, FALSE);
        }
    }
}

void Pop_Tick(Pop *p)
{
    Pop_Step(p);
    if (p->hwnd && p->content) Pop_Present(p, p->content);
}

/* Coloca la ventana para que el CONTENIDO ocupe (x, y, w, h) en pantalla. */
void Pop_SetBounds(Pop *p, int x, int y, int w, int h)
{
    SetWindowPos(p->hwnd, NULL, x - p->margin, y - p->mtop, w + 2 * p->margin, h + p->margin + p->mtop,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void Pop_ContentRect(const Pop *p, RECT *r)
{
    GetWindowRect(p->hwnd, r);
    r->left += p->margin; r->right -= p->margin;
    r->top += p->mtop; r->bottom -= p->margin;
}

/* Ratón en coordenadas del contenido. */
LPARAM Pop_Mouse(const Pop *p, LPARAM l)
{
    return MAKELPARAM((short)LOWORD(l) - p->margin, (short)HIWORD(l) - p->mtop);
}

/* La sombra no atrapa clics: fuera del contenido, transparente. */
LRESULT Pop_NcHit(const Pop *p, LPARAM l)
{
    RECT r;
    Pop_ContentRect(p, &r);
    POINT pt = { (short)LOWORD(l), (short)HIWORD(l) };
    return PtInRect(&r, pt) && !p->closing ? HTCLIENT : HTTRANSPARENT;
}

/* ───────────────────────── Vidrio ─────────────────────────
 * Se captura lo que hay detrás ANTES de mostrar la ventana (una columna de pantalla, para
 * que sirva aunque el contenido cambie de alto), a 1/4, desenfocado y con vibrancia. */
#define GLASS_SCALE 4
void Pop_CaptureGlass(PopGlass *g, int x, int y, int w, int h, int blur)
{
    const int sw = max(1, w / GLASS_SCALE), sh = max(1, h / GLASS_SCALE);
    if (g->cv.w != sw || g->cv.h != sh) {
        Canvas_Free(&g->cv);
        if (!Canvas_Init(&g->cv, sw, sh)) return;
    }
    g->x = x; g->y = y;
    HDC screen = GetDC(NULL);
    SetStretchBltMode(g->cv.dc, HALFTONE);
    SetBrushOrgEx(g->cv.dc, 0, 0, NULL);
    StretchBlt(g->cv.dc, 0, 0, sw, sh, screen, x, y, sw * GLASS_SCALE, sh * GLASS_SCALE, SRCCOPY);
    ReleaseDC(NULL, screen);
    GdiFlush();
    for (int i = 0; i < 2; ++i) Gfx_BoxBlur(g->cv.px, sw, sh, max(1, blur / GLASS_SCALE));
    for (int i = 0; i < sw * sh; ++i) {
        const DWORD v = g->cv.px[i];
        const int R = (v >> 16) & 255, G = (v >> 8) & 255, B = v & 255, Y = (R * 77 + G * 151 + B * 28) >> 8;
        g->cv.px[i] = (DWORD)max(0, min(255, Y + (R - Y) * 3 / 2)) << 16 |
                      (DWORD)max(0, min(255, Y + (G - Y) * 3 / 2)) << 8 |
                      (DWORD)max(0, min(255, Y + (B - Y) * 3 / 2));
    }
}

/* Pinta el vidrio como fondo de c, cuyo origen está en (ox, oy) de pantalla. */
void Pop_PaintGlass(Canvas *c, const PopGlass *g, int ox, int oy, DWORD tint, float op)
{
    if (!g->cv.px) { Canvas_Clear(c, tint); return; }
    GdiFlush();
    for (int y = 0; y < c->h; ++y) {
        const float fy = (oy + y - g->y + 0.5f) / GLASS_SCALE - 0.5f;
        const int y0 = max(0, min(g->cv.h - 1, (int)floorf(fy))), y1 = min(g->cv.h - 1, y0 + 1);
        const float ty = max(0.0f, min(1.0f, fy - y0));
        for (int x = 0; x < c->w; ++x) {
            const float fx = (ox + x - g->x + 0.5f) / GLASS_SCALE - 0.5f;
            const int x0 = max(0, min(g->cv.w - 1, (int)floorf(fx))), x1 = min(g->cv.w - 1, x0 + 1);
            const float tx = max(0.0f, min(1.0f, fx - x0));
            const DWORD *p = g->cv.px;
            const DWORD b = Gfx_Mix(Gfx_Mix(p[y0 * g->cv.w + x0], p[y0 * g->cv.w + x1], tx),
                                    Gfx_Mix(p[y1 * g->cv.w + x0], p[y1 * g->cv.w + x1], tx), ty);
            c->px[y * c->w + x] = Gfx_Mix(b, tint, op) & 0xFFFFFF;
        }
    }
}

void Pop_FreeGlass(PopGlass *g) { Canvas_Free(&g->cv); }
