/*
 * gfx.c — lienzo de 32 bits y primitivas antialiasadas.
 * Formas por distancia con signo (SDF) → bordes suaves sin GDI+ ni Direct2D.
 * El texto usa GDI sobre el mismo DIB.
 */
#include "app.h"
#include <math.h>

/* 0 se sustituye por el acento de Windows; el resto, paleta tipo Apple. */
const DWORD kAccentPresets[ACCENT_COUNT] = {
    0x0A84FF, 0x0A84FF, 0xBF5AF2, 0xFF375F, 0xFF9F0A, 0x30D158, 0x98989D,
};

BOOL Canvas_Init(Canvas *c, int w, int h)
{
    ZeroMemory(c, sizeof(*c));
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;            /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(NULL);
    c->dc  = CreateCompatibleDC(screen);
    c->bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&c->px, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!c->dc || !c->bmp || !c->px) { Canvas_Free(c); return FALSE; }

    c->old = SelectObject(c->dc, c->bmp);
    c->w = w;
    c->h = h;
    SetBkMode(c->dc, TRANSPARENT);
    return TRUE;
}

void Canvas_Free(Canvas *c)
{
    if (c->dc) {
        if (c->old) SelectObject(c->dc, c->old);
        DeleteDC(c->dc);
    }
    if (c->bmp) DeleteObject(c->bmp);
    ZeroMemory(c, sizeof(*c));
}

void Canvas_Clear(Canvas *c, DWORD rgb)
{
    GdiFlush();
    for (int i = 0, n = c->w * c->h; i < n; ++i) c->px[i] = rgb;
}

/* Distancia con signo a un rectángulo redondeado (negativa = dentro). */
float Gfx_SdRRect(float px, float py, float x, float y, float w, float h, float r)
{
    const float hw = w * 0.5f, hh = h * 0.5f;
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    if (r < 0) r = 0;
    const float qx = fabsf(px - (x + hw)) - (hw - r);
    const float qy = fabsf(py - (y + hh)) - (hh - r);
    const float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy;
    if (in > 0) in = 0;
    return sqrtf(ox * ox + oy * oy) + in - r;
}

/* Cobertura de un píxel a partir de la distancia (≈1 px de antialias). */
float Gfx_Cov(float d)
{
    const float a = 0.5f - d;
    return a < 0 ? 0 : a > 1 ? 1 : a;
}

void Gfx_Blend(Canvas *c, int x, int y, DWORD rgb, float a)
{
    if (a <= 0 || x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    if (a > 1) a = 1;
    DWORD *p = &c->px[y * c->w + x];
    const DWORD d = *p;
    const int ia = (int)(a * 256.0f + 0.5f);
    int ch[3];
    for (int i = 0; i < 3; ++i) {
        const int s = (int)((rgb >> (i * 8)) & 255), t = (int)((d >> (i * 8)) & 255);
        ch[i] = t + (s - t) * ia / 256;
    }
    *p = (DWORD)(ch[2] << 16 | ch[1] << 8 | ch[0]);
}

static void Span(const Canvas *c, float x, float y, float w, float h, float pad,
                 int *x0, int *y0, int *x1, int *y1)
{
    *x0 = max(0, (int)floorf(x - pad) - 1);
    *y0 = max(0, (int)floorf(y - pad) - 1);
    *x1 = min(c->w, (int)ceilf(x + w + pad) + 1);
    *y1 = min(c->h, (int)ceilf(y + h + pad) + 1);
}

void Gfx_FillRRect(Canvas *c, float x, float y, float w, float h, float r, DWORD rgb, float a)
{
    if (w <= 0 || h <= 0 || a <= 0) return;
    int x0, y0, x1, y1;
    GdiFlush();
    Span(c, x, y, w, h, 0, &x0, &y0, &x1, &y1);
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px)
            Gfx_Blend(c, px, py, rgb, a * Gfx_Cov(Gfx_SdRRect(px + 0.5f, py + 0.5f, x, y, w, h, r)));
}

/* Trazo centrado en el borde del rectángulo redondeado. */
void Gfx_StrokeRRect(Canvas *c, float x, float y, float w, float h, float r, float th, DWORD rgb, float a)
{
    if (w <= 0 || h <= 0 || a <= 0) return;
    int x0, y0, x1, y1;
    GdiFlush();
    Span(c, x, y, w, h, th, &x0, &y0, &x1, &y1);
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px) {
            const float d = fabsf(Gfx_SdRRect(px + 0.5f, py + 0.5f, x, y, w, h, r)) - th * 0.5f;
            Gfx_Blend(c, px, py, rgb, a * Gfx_Cov(d));
        }
}

void Gfx_FillCircle(Canvas *c, float cx, float cy, float rad, DWORD rgb, float a)
{
    Gfx_FillRRect(c, cx - rad, cy - rad, rad * 2, rad * 2, rad, rgb, a);
}

DWORD Gfx_Mix(DWORD a, DWORD b, float t)
{
    if (t <= 0) return a;
    if (t >= 1) return b;
    DWORD out = 0;
    for (int i = 0; i < 3; ++i) {
        const float s = (float)((a >> (i * 8)) & 255), e = (float)((b >> (i * 8)) & 255);
        out |= (DWORD)(s + (e - s) * t + 0.5f) << (i * 8);
    }
    return out;
}

void Gfx_Text(Canvas *c, HFONT f, LPCWSTR s, int x, int y, int w, int h, DWORD rgb, UINT fmt)
{
    RECT r = { x, y, x + w, y + h };
    HGDIOBJ o = SelectObject(c->dc, f);
    SetTextColor(c->dc, RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255));
    DrawTextW(c->dc, s, -1, &r, fmt | DT_NOPREFIX);
    SelectObject(c->dc, o);
}

int Gfx_TextWidth(HFONT f, LPCWSTR s)
{
    SIZE sz = { 0, 0 };
    HDC dc = GetDC(NULL);
    HGDIOBJ o = SelectObject(dc, f);
    GetTextExtentPoint32W(dc, s, lstrlenW(s), &sz);
    SelectObject(dc, o);
    ReleaseDC(NULL, dc);
    return sz.cx;
}

HFONT Gfx_Font(LPCWSTR face, int px, int weight, BYTE quality)
{
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, quality, DEFAULT_PITCH, face);
}

static int CALLBACK FontFound(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM lp)
{
    (void)lf; (void)tm; (void)type;
    *(BOOL *)lp = TRUE;
    return 0;
}

static BOOL HasFont(LPCWSTR face)
{
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    BOOL found = FALSE;
    HDC dc = GetDC(NULL);
    EnumFontFamiliesExW(dc, &lf, FontFound, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found;
}

/* Win11 trae Segoe UI Variable y Segoe Fluent Icons; Win10 cae a las clásicas. */
LPCWSTR Gfx_UiFace(void)
{
    static LPCWSTR face;
    if (!face) face = HasFont(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    return face;
}

LPCWSTR Gfx_IconFace(void)
{
    static LPCWSTR face;
    if (!face) face = HasFont(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    return face;
}

/* AccentPalette: 8 entradas RGBA, de la más clara (0) a la más oscura (6). */
static DWORD PaletteColor(const BYTE *pal, int i)
{
    return (DWORD)pal[i * 4] << 16 | (DWORD)pal[i * 4 + 1] << 8 | pal[i * 4 + 2];
}

void Theme_Load(Theme *t)
{
    DWORD light = 1, sz = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &sz);

    BYTE pal[32];
    DWORD psz = sizeof(pal);
    const BOOL havePal = RegGetValueW(HKEY_CURRENT_USER,
                                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                                      L"AccentPalette", RRF_RT_REG_BINARY, NULL, pal, &psz) == ERROR_SUCCESS
                         && psz >= 32;

    t->dark = !light;
    t->accentOnBlack = havePal ? PaletteColor(pal, 1) : 0x60CDFF;
    if (t->dark) {
        t->bg = 0x202020;  t->fg = 0xFFFFFF;  t->fg2 = 0xC8C8C8;  t->fg3 = 0x8B8B8B;
        t->line = 0x333333; t->ctrl = 0x2D2D2D; t->ctrlHover = 0x383838;
        t->track = 0x5A5A5A; t->thumb = 0x454545;
        t->accent = havePal ? PaletteColor(pal, 1) : 0x60CDFF;
        t->onAccent = 0x000000; t->danger = 0xFF99A4;
    } else {
        t->bg = 0xF9F9F9;  t->fg = 0x1B1B1B;  t->fg2 = 0x5D5D5D;  t->fg3 = 0x8A8A8A;
        t->line = 0xE5E5E5; t->ctrl = 0xEDEDED; t->ctrlHover = 0xE3E3E3;
        t->track = 0xC4C4C4; t->thumb = 0xFFFFFF;
        t->accent = havePal ? PaletteColor(pal, 4) : 0x005FB8;
        t->onAccent = 0xFFFFFF; t->danger = 0xC42B1C;
    }
}

/* Desenfoque de caja separable (horizontal + vertical), solo RGB. */
void Gfx_BoxBlur(DWORD *px, int w, int h, int r)
{
    DWORD *tmp = (DWORD *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)w * h * 4);
    if (!tmp || r < 1) { if (tmp) HeapFree(GetProcessHeap(), 0, tmp); return; }
    for (int pass = 0; pass < 2; ++pass) {           /* horizontal → tmp, vertical → px */
        const int n = pass ? h : w, lines = pass ? w : h;
        for (int L = 0; L < lines; ++L) {
            int sr = 0, sg = 0, sb = 0;
            for (int k = -r; k <= r; ++k) {
                const int i = max(0, min(n - 1, k));
                const DWORD v = pass ? tmp[i * w + L] : px[L * w + i];
                sr += (v >> 16) & 255; sg += (v >> 8) & 255; sb += v & 255;
            }
            for (int i = 0; i < n; ++i) {
                const int d = 2 * r + 1;
                const DWORD out = (DWORD)(sr / d) << 16 | (DWORD)(sg / d) << 8 | (DWORD)(sb / d);
                if (pass) px[i * w + L] = out; else tmp[L * w + i] = out;
                const int a = max(0, i - r), b = min(n - 1, i + r + 1);
                const DWORD va = pass ? tmp[a * w + L] : px[L * w + a];
                const DWORD vb = pass ? tmp[b * w + L] : px[L * w + b];
                sr += (int)((vb >> 16) & 255) - (int)((va >> 16) & 255);
                sg += (int)((vb >> 8) & 255) - (int)((va >> 8) & 255);
                sb += (int)(vb & 255) - (int)(va & 255);
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, tmp);
}

/* Copia src escalado por k alrededor de (cx, cy) de dst: bilineal en punto fijo. */
void Gfx_BlitScaled(Canvas *dst, const Canvas *src, float cx, float cy, float k)
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
