// Simple Crosshair - a minimal crosshair overlay for Windows.
//
// It only draws a small, click-through, always-on-top image at the centre of
// the chosen monitor. By design it does NOT read or write other processes'
// memory, inject DLLs, hook DirectX/input, capture the screen, look for game
// windows, or need admin rights. Settings live in SimpleCrosshair.ini next to the exe.

#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00  // Windows 10+ (per-monitor DPI APIs)
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <math.h>
#include <stdlib.h>
#include <wchar.h>

enum { ST_CROSS, ST_CROSS_DOT, ST_T, ST_DOT, ST_CIRCLE, ST_CIRCLE_DOT, ST_COUNT };
static const wchar_t* kStyleNames[ST_COUNT] = {
    L"CROSS", L"CROSS + DOT", L"T-SHAPE", L"DOT", L"CIRCLE", L"CIRCLE + DOT",
};

static struct {
    int style, x, y, size, thickness, gap, dot, outline, opacity, visible;
    COLORREF color;
} g = { ST_CROSS, 0, 0, 6, 2, 3, 2, 1, 100, 1, RGB(0, 255, 0) };

struct Field { const wchar_t* label; const wchar_t* key; int* v; int lo, hi; };
static Field kFields[] = {
    { L"SIZE",      L"Size",      &g.size,      0,     80 },
    { L"THICKNESS", L"Thickness", &g.thickness, 1,     12 },
    { L"GAP",       L"Gap",       &g.gap,       0,     40 },
    { L"DOT SIZE",  L"Dot",       &g.dot,       1,     12 },
    { L"OUTLINE",   L"Outline",   &g.outline,   0,     4 },
    { L"X",         L"X",         &g.x,         -4000, 4000 },
    { L"Y",         L"Y",         &g.y,         -4000, 4000 },
};
static const COLORREF kPresets[] = {
    RGB(0, 255, 0), RGB(0, 255, 255), RGB(255, 0, 255), RGB(255, 255, 0),
    RGB(255, 149, 0), RGB(255, 59, 59), RGB(255, 255, 255),
};
enum { PRESET_COUNT = sizeof(kPresets) / sizeof(kPresets[0]) };
enum { HOTKEY_TOGGLE = 1, TIMER_TOPMOST = 1, TIMER_REPEAT = 2 };
enum { WM_TRAY = WM_APP + 1, IDM_SETTINGS = 200, IDM_TOGGLE, IDM_EXIT };

static HWND gMain, gOverlay;
static HICON gIconSm;
static UINT gTaskbarCreated;
static bool gExclusive;  // a game is in exclusive fullscreen, where Windows won't draw overlays
static UINT gDpi = 96;
static wchar_t gIni[MAX_PATH];
static wchar_t gScreen[CCHDEVICENAME];  // chosen monitor's device name; empty = primary

struct Monitor { RECT rc; wchar_t dev[CCHDEVICENAME]; bool primary; };
static Monitor gMons[16];
static int gMonCount;

static int S(int v) { return MulDiv(v, gDpi, 96); }
static int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// ---------------------------------------------------------------- settings

static int ReadInt(const wchar_t* key, int def) {
    wchar_t buf[32];
    GetPrivateProfileStringW(L"Crosshair", key, L"", buf, 32, gIni);
    return buf[0] ? _wtoi(buf) : def;
}

static void WriteInt(const wchar_t* key, int v) {
    wchar_t buf[32];
    _itow(v, buf, 10);
    WritePrivateProfileStringW(L"Crosshair", key, buf, gIni);
}

static void LoadSettings() {
    GetModuleFileNameW(nullptr, gIni, MAX_PATH);
    wchar_t* ext = wcsrchr(gIni, L'.');
    if (ext) wcscpy(ext, L".ini");

    g.style = Clamp(ReadInt(L"Style", g.style), 0, ST_COUNT - 1);
    g.opacity = Clamp(ReadInt(L"Opacity", g.opacity), 10, 100);
    g.visible = ReadInt(L"Visible", g.visible) != 0;
    GetPrivateProfileStringW(L"Crosshair", L"Screen", L"", gScreen, CCHDEVICENAME, gIni);
    for (Field& f : kFields) *f.v = Clamp(ReadInt(f.key, *f.v), f.lo, f.hi);

    wchar_t hex[16];
    GetPrivateProfileStringW(L"Crosshair", L"Color", L"", hex, 16, gIni);
    if (hex[0]) {
        unsigned long c = wcstoul(hex, nullptr, 16);
        g.color = RGB((c >> 16) & 255, (c >> 8) & 255, c & 255);
    }
}

static void SaveSettings() {
    WriteInt(L"Style", g.style);
    WriteInt(L"Opacity", g.opacity);
    WriteInt(L"Visible", g.visible);
    WritePrivateProfileStringW(L"Crosshair", L"Screen", gScreen, gIni);
    for (Field& f : kFields) WriteInt(f.key, *f.v);
    wchar_t hex[16];
    wsprintfW(hex, L"%02X%02X%02X", GetRValue(g.color), GetGValue(g.color), GetBValue(g.color));
    WritePrivateProfileStringW(L"Crosshair", L"Color", hex, gIni);
}

// ---------------------------------------------------------------- overlay

static BOOL CALLBACK AddMonitor(HMONITOR h, HDC, LPRECT, LPARAM) {
    MONITORINFOEXW mi;
    mi.cbSize = sizeof(mi);
    if (gMonCount < 16 && GetMonitorInfoW(h, &mi)) {
        Monitor& m = gMons[gMonCount++];
        m.rc = mi.rcMonitor;
        wcscpy(m.dev, mi.szDevice);
        m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    }
    return TRUE;
}

// Rebuilds the monitor list, ordered left-to-right then top-to-bottom.
static void RefreshMonitors() {
    gMonCount = 0;
    EnumDisplayMonitors(nullptr, nullptr, AddMonitor, 0);
    for (int i = 1; i < gMonCount; ++i)
        for (int j = i; j > 0; --j) {
            const RECT &a = gMons[j - 1].rc, &b = gMons[j].rc;
            if (a.left < b.left || (a.left == b.left && a.top <= b.top)) break;
            Monitor t = gMons[j]; gMons[j] = gMons[j - 1]; gMons[j - 1] = t;
        }
}

// The chosen screen, or the primary one if it is unset or no longer connected.
static int SelectedMonitor() {
    int primary = 0;
    for (int i = 0; i < gMonCount; ++i) {
        if (gScreen[0] && wcscmp(gMons[i].dev, gScreen) == 0) return i;
        if (gMons[i].primary) primary = i;
    }
    return primary;
}

static POINT ScreenCenter() {
    if (gMonCount == 0) RefreshMonitors();
    const RECT& r = gMons[SelectedMonitor()].rc;
    return { r.left + (r.right - r.left) / 2, r.top + (r.bottom - r.top) / 2 };
}

static void Redraw() {
    if (gMain) InvalidateRect(gMain, nullptr, FALSE);
}

// A W x W premultiplied BGRA image of the crosshair; its centre is pixel R (or the corner at R
// for even line widths). The caller frees `px`.
struct Raster { int R, W; BYTE* px; };

static Raster RasterizeCrosshair() {
    const int t = g.thickness, o = g.outline;
    const bool cross = g.style == ST_CROSS || g.style == ST_CROSS_DOT || g.style == ST_T;
    const bool circle = g.style == ST_CIRCLE || g.style == ST_CIRCLE_DOT;
    const bool dot = g.style == ST_CROSS_DOT || g.style == ST_DOT || g.style == ST_CIRCLE_DOT;

    int ext = dot ? g.dot : 0;
    if (cross && g.gap + g.size + t > ext) ext = g.gap + g.size + t;
    if (circle && g.size + t > ext) ext = g.size + t;
    const int R = ext + o + 2, W = 2 * R + 1, N = W * W;

    float* a = (float*)calloc(N * 3, sizeof(float));  // shape, scratch, outline
    float *tmp = a + N, *out = a + 2 * N;

    // Odd widths centre on a pixel, even widths on a pixel corner, so lines stay crisp.
    const float ct = R + ((t & 1) ? 0.5f : 0.f), ht = t * 0.5f;
    const float cd = R + ((g.dot & 1) ? 0.5f : 0.f), hd = g.dot * 0.5f;

    for (int y = 0; y < W; ++y) {
        for (int x = 0; x < W; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            const float dx = px - ct, dy = py - ct, ax = fabsf(dx), ay = fabsf(dy);
            float v = 0;
            if (cross) {
                bool h = ay < ht && ax <= g.gap + g.size && (ax > g.gap || g.gap == 0);
                bool vert = ax < ht && ay <= g.gap + g.size && (ay > g.gap || g.gap == 0) &&
                            !(g.style == ST_T && dy < 0);
                if (h || vert) v = 1;
            }
            if (circle) v = fmaxf(v, Clamp01(ht + 0.5f - fabsf(hypotf(dx, dy) - g.size)));
            if (dot) {
                const float ex = px - cd, ey = py - cd;
                float d = g.dot <= 3 ? (fabsf(ex) < hd && fabsf(ey) < hd ? 1.f : 0.f)
                                     : Clamp01(hd + 0.5f - hypotf(ex, ey));
                v = fmaxf(v, d);
            }
            a[y * W + x] = v;
        }
    }

    // Outline = shape dilated by `o` pixels (separable max filter).
    if (o > 0) {
        for (int y = 0; y < W; ++y)
            for (int x = 0; x < W; ++x) {
                float m = 0;
                for (int k = x - o; k <= x + o; ++k)
                    if (k >= 0 && k < W) m = fmaxf(m, a[y * W + k]);
                tmp[y * W + x] = m;
            }
        for (int y = 0; y < W; ++y)
            for (int x = 0; x < W; ++x) {
                float m = 0;
                for (int k = y - o; k <= y + o; ++k)
                    if (k >= 0 && k < W) m = fmaxf(m, tmp[k * W + x]);
                out[y * W + x] = m;
            }
    }

    BYTE* px = (BYTE*)malloc(N * 4);
    BYTE* p = px;
    const float op = g.opacity / 100.f;
    const float r = GetRValue(g.color), gr = GetGValue(g.color), b = GetBValue(g.color);
    for (int i = 0; i < N; ++i, p += 4) {
        const float A = a[i] * op, alpha = (a[i] + out[i] * (1 - a[i])) * op;  // black outline
        p[0] = (BYTE)(b * A + 0.5f);
        p[1] = (BYTE)(gr * A + 0.5f);
        p[2] = (BYTE)(r * A + 0.5f);
        p[3] = (BYTE)(alpha * 255 + 0.5f);
    }
    free(a);
    return { R, W, px };
}

// Pushes the crosshair to the layered window. Only runs when a setting changes, so idle cost is zero.
static void ApplyOverlay() {
    Raster ras = RasterizeCrosshair();
    const int W = ras.W;

    BITMAPINFO bi = {};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -W, 1, 32, BI_RGB };
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    memcpy(bits, ras.px, W * W * 4);
    free(ras.px);

    POINT c = ScreenCenter(), src = { 0, 0 };
    POINT dst = { c.x - ras.R + g.x, c.y - ras.R + g.y };
    SIZE size = { W, W };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(gOverlay, screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    ShowWindow(gOverlay, g.visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

static void CheckExclusive();

// Fullscreen games often make themselves topmost when they take focus; move back above them.
static void RaiseOverlay() {
    if (g.visible)
        SetWindowPos(gOverlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Out-of-context WinEvent: Windows just posts us a notification when the foreground window
// changes (the same mechanism screen readers use). Nothing is loaded into other processes.
static void CALLBACK OnForegroundChanged(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    RaiseOverlay();
}

static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_DISPLAYCHANGE:  // screen added/removed/rearranged, or a game changed resolution
        RefreshMonitors();
        Redraw();
        ApplyOverlay();
        return 0;
    case WM_TIMER:  // safety net for anything the foreground event misses
        RaiseOverlay();
        CheckExclusive();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- settings window (Direct2D)
//
// The settings window is drawn by hand: Layout() both paints every widget and records where the
// interactive ones are, so mouse handling hit-tests exactly what was last drawn.

static ID2D1Factory* gD2d;
static IDWriteFactory* gDw;
static ID2D1HwndRenderTarget* gRt;
static ID2D1SolidColorBrush* gBrush;
static ID2D1StrokeStyle* gRound;
static IDWriteTextFormat *fBrand, *fSection, *fLabel, *fValue, *fSmall, *fGlyph;
static bool gDraw;  // false = measure only (used to size the window before it exists)
static int gZoom = 2;

enum : UINT {
    BG = 0x0B0D12, PANEL = 0x141821, PANEL_HI = 0x1D2230, LINE = 0x272D3A,
    TEXT = 0xE8ECF4, MUTED = 0x7D8799, AMBER = 0xFFB020, SKY_TOP = 0x1A2232, SKY_LOW = 0x0C0F15,
};
static const float CW = 400, M = 22, IW = CW - 2 * M;  // client width, margin, inner width (DIPs)

enum Kind { K_PREVIEW, K_STYLE, K_SWATCH, K_SLIDER, K_STEP, K_CENTER, K_SCREEN, K_TOGGLE };
struct Widget { Kind kind; D2D1_RECT_F r; int arg; int* v; int lo, hi; };
static Widget gW[64];
static int gWN, gHot = -1, gActive = -1;
static bool gTracking;

static D2D1_COLOR_F C(UINT rgb, float a = 1) {
    return { ((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a };
}
static UINT FromRef(COLORREF c) { return (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c); }
static D2D1_RECT_F RC(float l, float t, float r, float b) { return { l, t, r, b }; }
static D2D1_RECT_F Inset(D2D1_RECT_F r, float d) { return { r.left + d, r.top + d, r.right - d, r.bottom - d }; }
static float Dip(int px) { return px * 96.f / gDpi; }

// UI accent = the crosshair colour, lightened if it is too dark to read on the dark theme.
static UINT Accent() {
    float r = GetRValue(g.color), gg = GetGValue(g.color), b = GetBValue(g.color);
    const float lum = (0.2126f * r + 0.7152f * gg + 0.0722f * b) / 255;
    if (lum < 0.4f) {
        const float t = (0.4f - lum) / (1 - lum);
        r += (255 - r) * t; gg += (255 - gg) * t; b += (255 - b) * t;
    }
    return ((UINT)r << 16) | ((UINT)gg << 8) | (UINT)b;
}

static int Add(Kind k, D2D1_RECT_F r, int arg = 0, int* v = nullptr, int lo = 0, int hi = 0) {
    if (gWN == 64) return -1;
    gW[gWN] = { k, r, arg, v, lo, hi };
    return gWN++;
}

static void Fill(D2D1_RECT_F r, float rad, D2D1_COLOR_F c) {
    if (!gDraw) return;
    gBrush->SetColor(&c);
    D2D1_ROUNDED_RECT rr = { r, rad, rad };
    gRt->FillRoundedRectangle(&rr, gBrush);
}

static void Stroke(D2D1_RECT_F r, float rad, D2D1_COLOR_F c, float w = 1) {
    if (!gDraw) return;
    gBrush->SetColor(&c);
    D2D1_ROUNDED_RECT rr = { r, rad, rad };
    gRt->DrawRoundedRectangle(&rr, gBrush, w, nullptr);
}

static void Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float w = 1) {
    if (!gDraw) return;
    gBrush->SetColor(&c);
    gRt->DrawLine({ x1, y1 }, { x2, y2 }, gBrush, w, gRound);
}

static void Circle(float cx, float cy, float rad, D2D1_COLOR_F c, bool fill, float w = 1) {
    if (!gDraw) return;
    gBrush->SetColor(&c);
    D2D1_ELLIPSE e = { { cx, cy }, rad, rad };
    if (fill) gRt->FillEllipse(&e, gBrush);
    else gRt->DrawEllipse(&e, gBrush, w, nullptr);
}

static void Text(const wchar_t* s, D2D1_RECT_F r, IDWriteTextFormat* f, D2D1_COLOR_F c,
                 DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    if (!gDraw) return;
    gBrush->SetColor(&c);
    f->SetTextAlignment(align);
    gRt->DrawText(s, (UINT32)wcslen(s), f, &r, gBrush, D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
}

static void Section(float y, const wchar_t* title, const wchar_t* value, UINT acc) {
    Fill(RC(M, y + 5, M + 6, y + 11), 1, C(acc));
    Text(title, RC(M + 13, y, M + 200, y + 16), fSection, C(TEXT));
    if (value) Text(value, RC(CW - M - 200, y, CW - M, y + 16), fSection, C(acc), DWRITE_TEXT_ALIGNMENT_TRAILING);
}

static void StyleIcon(int s, float cx, float cy, D2D1_COLOR_F c) {
    const float gap = 4, len = 9, w = 2.5f;
    const bool cross = s == ST_CROSS || s == ST_CROSS_DOT || s == ST_T;
    if (cross) {
        Line(cx - gap - len, cy, cx - gap, cy, c, w);
        Line(cx + gap, cy, cx + gap + len, cy, c, w);
        Line(cx, cy + gap, cx, cy + gap + len, c, w);
        if (s != ST_T) Line(cx, cy - gap - len, cx, cy - gap, c, w);
    }
    if (s == ST_CIRCLE || s == ST_CIRCLE_DOT) Circle(cx, cy, 11, c, false, 2);
    if (s == ST_CROSS_DOT || s == ST_CIRCLE_DOT) Circle(cx, cy, 2.2f, c, true);
    if (s == ST_DOT) Circle(cx, cy, 4.5f, c, true);
}

static void StatusPill(D2D1_RECT_F r, UINT acc) {
    const wchar_t* t = L"LIVE";
    UINT c = acc;
    if (!g.visible) { t = L"OFF"; c = MUTED; }
    else if (gExclusive) { t = L"BLOCKED"; c = AMBER; }
    Fill(r, 12, C(c, 0.12f));
    Stroke(Inset(r, 0.5f), 12, C(c, 0.55f));
    Circle(r.left + 15, (r.top + r.bottom) / 2, 3.5f, C(c), true);
    Text(t, RC(r.left + 24, r.top, r.right - 8, r.bottom), fSection, C(c), DWRITE_TEXT_ALIGNMENT_CENTER);
}

// A little "game scene" (skyline + perspective grid) with the real crosshair bitmap on top,
// drawn pixel-exact at 1x/2x/4x.
static void Preview(D2D1_RECT_F r, UINT acc) {
    if (!gDraw) return;
    D2D1_ROUNDED_RECT rr = { r, 10, 10 };

    D2D1_GRADIENT_STOP stops[2] = { { 0, C(SKY_TOP) }, { 1, C(SKY_LOW) } };
    ID2D1GradientStopCollection* coll = nullptr;
    gRt->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &coll);
    D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lp = { { 0, r.top }, { 0, r.bottom } };
    ID2D1LinearGradientBrush* sky = nullptr;
    gRt->CreateLinearGradientBrush(&lp, nullptr, coll, &sky);
    gRt->FillRoundedRectangle(&rr, sky);
    sky->Release();
    coll->Release();

    // Clip the scene to the rounded panel.
    ID2D1RoundedRectangleGeometry* clip = nullptr;
    gD2d->CreateRoundedRectangleGeometry(&rr, &clip);
    ID2D1Layer* layer = nullptr;
    gRt->CreateLayer(nullptr, &layer);
    D2D1_LAYER_PARAMETERS lpar = {};
    lpar.contentBounds = r;
    lpar.geometricMask = clip;
    lpar.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
    lpar.maskTransform._11 = 1;
    lpar.maskTransform._22 = 1;
    lpar.opacity = 1;
    gRt->PushLayer(&lpar, layer);

    const float w = r.right - r.left, h = r.bottom - r.top;
    const float hz = r.top + h * 0.56f, cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    static const float kBlocks[][3] = {  // x, width, height (fractions)
        { 0.00f, 0.09f, 0.34f }, { 0.08f, 0.06f, 0.58f }, { 0.13f, 0.08f, 0.40f }, { 0.22f, 0.05f, 0.72f },
        { 0.27f, 0.09f, 0.46f }, { 0.36f, 0.07f, 0.26f }, { 0.58f, 0.08f, 0.36f }, { 0.66f, 0.05f, 0.64f },
        { 0.71f, 0.09f, 0.44f }, { 0.80f, 0.06f, 0.78f }, { 0.86f, 0.08f, 0.38f }, { 0.94f, 0.07f, 0.52f },
    };
    for (auto& b : kBlocks)
        Fill(RC(r.left + b[0] * w, hz - b[2] * (hz - r.top) * 0.85f, r.left + (b[0] + b[1]) * w, hz), 0, C(0x10151E));
    for (int i = -9; i <= 9; ++i) Line(cx, hz, cx + i * w / 7, r.bottom, C(acc, 0.09f), 1);
    for (int i = 1; i <= 7; ++i) {
        const float t = i / 7.f, yy = hz + (r.bottom - hz) * t * t;
        Line(r.left, yy, r.right, yy, C(acc, 0.09f), 1);
    }
    Line(r.left, hz, r.right, hz, C(acc, 0.28f), 1);

    // The actual crosshair, aligned to physical pixels so it looks exactly like in game.
    Raster ras = RasterizeCrosshair();
    D2D1_BITMAP_PROPERTIES bp = { { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED }, 96, 96 };
    ID2D1Bitmap* bmp = nullptr;
    gRt->CreateBitmap({ (UINT32)ras.W, (UINT32)ras.W }, ras.px, ras.W * 4, &bp, &bmp);
    free(ras.px);
    const float sc = gDpi / 96.f, pcx = roundf(cx * sc), pcy = roundf(cy * sc);
    const float z = (float)gZoom, lo = ras.R * z, hi = (ras.W - ras.R) * z;
    D2D1_RECT_F dst = RC((pcx - lo) / sc, (pcy - lo) / sc, (pcx + hi) / sc, (pcy + hi) / sc);
    if (bmp) {
        gRt->DrawBitmap(bmp, &dst, g.visible ? 1.f : 0.3f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
        bmp->Release();
    }

    Text(L"PREVIEW", RC(r.left + 12, r.top + 8, r.left + 120, r.top + 22), fSmall, C(MUTED));
    float chipBottom = r.bottom - 8;
    if (gExclusive && g.visible) {
        Fill(RC(r.left, r.bottom - 28, r.right, r.bottom), 0, C(0x000000, 0.65f));
        Text(L"EXCLUSIVE FULLSCREEN — SET YOUR GAME TO BORDERLESS",
             RC(r.left, r.bottom - 28, r.right, r.bottom), fSmall, C(AMBER), DWRITE_TEXT_ALIGNMENT_CENTER);
        chipBottom = r.bottom - 34;
    }
    wchar_t zl[8];
    wsprintfW(zl, L"%d×", gZoom);
    D2D1_RECT_F chip = RC(r.right - 42, chipBottom - 20, r.right - 8, chipBottom);
    Fill(chip, 6, C(0x000000, 0.45f));
    Text(zl, chip, fSmall, C(gHot >= 0 && gW[gHot].kind == K_PREVIEW ? acc : TEXT), DWRITE_TEXT_ALIGNMENT_CENTER);

    gRt->PopLayer();
    layer->Release();
    clip->Release();
    Stroke(Inset(r, 0.5f), 10, C(LINE));
}

static void Slider(float y, const wchar_t* label, int* v, int lo, int hi, const wchar_t* unit, bool zeroOff, UINT acc) {
    Text(label, RC(M, y, M + 96, y + 28), fLabel, C(MUTED));
    const D2D1_RECT_F tr = RC(M + 100, y + 12, CW - M - 62, y + 16);
    const int id = Add(K_SLIDER, tr, 0, v, lo, hi);
    const bool hot = id >= 0 && (gHot == id || gActive == id);
    const float kx = tr.left + (float)(*v - lo) / (hi - lo) * (tr.right - tr.left), cy = y + 14;
    Fill(tr, 2, C(LINE));
    Fill(RC(tr.left, tr.top, kx, tr.bottom), 2, C(acc));
    if (hot) Circle(kx, cy, 12, C(acc, 0.2f), true);
    Circle(kx, cy, 7, C(TEXT), true);
    Circle(kx, cy, 7, C(acc), false, 2);
    wchar_t buf[32];
    if (zeroOff && *v == 0) wcscpy(buf, L"OFF");
    else wsprintfW(buf, L"%d%s", *v, unit);
    Text(buf, RC(CW - M - 56, y, CW - M, y + 28), fValue, C(TEXT), DWRITE_TEXT_ALIGNMENT_TRAILING);
}

static void StepButton(D2D1_RECT_F r, int dir, int* v, const wchar_t* glyph, UINT acc) {
    const int id = Add(K_STEP, r, dir, v, -4000, 4000);
    const bool hot = id >= 0 && gHot == id, down = id >= 0 && gActive == id;
    Fill(r, 6, down ? C(acc, 0.25f) : C(hot ? PANEL_HI : PANEL));
    Stroke(Inset(r, 0.5f), 6, hot ? C(acc, 0.7f) : C(LINE));
    Text(glyph, r, fGlyph, hot ? C(acc) : C(TEXT), DWRITE_TEXT_ALIGNMENT_CENTER);
}

static void Stepper(float x, float y, const wchar_t* label, int* v, UINT acc) {
    Text(label, RC(x, y, x + 16, y + 28), fValue, C(MUTED));
    StepButton(RC(x + 18, y, x + 46, y + 28), -1, v, L"−", acc);
    const D2D1_RECT_F box = RC(x + 49, y, x + 101, y + 28);
    Fill(box, 6, C(BG));
    Stroke(Inset(box, 0.5f), 6, C(LINE));
    wchar_t buf[16];
    _itow(*v, buf, 10);
    Text(buf, box, fValue, C(*v ? acc : TEXT), DWRITE_TEXT_ALIGNMENT_CENTER);
    StepButton(RC(x + 104, y, x + 132, y + 28), +1, v, L"+", acc);
}

// Paints the whole window (when gDraw) and rebuilds the widget list. Returns the content height.
static float Layout() {
    gWN = 0;
    const UINT acc = Accent();
    float y = 18;

    // Header: brand + live status.
    Fill(RC(M, y + 3, M + 4, y + 25), 2, C(acc));
    Text(L"SIMPLE CROSSHAIR", RC(M + 14, y, M + 260, y + 28), fBrand, C(TEXT));
    StatusPill(RC(CW - M - 104, y + 2, CW - M, y + 26), acc);
    y += 28 + 16;

    const D2D1_RECT_F pv = RC(M, y, CW - M, y + 150);
    Add(K_PREVIEW, pv);
    Preview(pv, acc);
    y = pv.bottom + 18;

    // Style tiles.
    Section(y, L"STYLE", kStyleNames[g.style], acc);
    y += 24;
    const float tw = (IW - 5 * 8) / 6, th = 50;
    for (int i = 0; i < ST_COUNT; ++i) {
        const D2D1_RECT_F r = RC(M + i * (tw + 8), y, M + i * (tw + 8) + tw, y + th);
        const int id = Add(K_STYLE, r, i);
        const bool sel = g.style == i, hot = id >= 0 && gHot == id;
        Fill(r, 8, sel ? C(acc, 0.12f) : C(hot ? PANEL_HI : PANEL));
        Stroke(Inset(r, 0.75f), 8, sel ? C(acc) : hot ? C(acc, 0.45f) : C(LINE), sel ? 1.5f : 1);
        StyleIcon(i, (r.left + r.right) / 2, (r.top + r.bottom) / 2, sel ? C(acc) : C(hot ? TEXT : MUTED));
    }
    y += th + 18;

    // Colour swatches + opacity.
    wchar_t hex[16];
    wsprintfW(hex, L"#%02X%02X%02X", GetRValue(g.color), GetGValue(g.color), GetBValue(g.color));
    Section(y, L"COLOR", hex, acc);
    y += 24;
    const float d = 28, sp = (IW - (PRESET_COUNT + 1) * d) / PRESET_COUNT;
    bool isPreset = false;
    for (int i = 0; i <= PRESET_COUNT; ++i) {
        const float cx = M + d / 2 + i * (d + sp), cy = y + d / 2;
        const int id = Add(K_SWATCH, RC(cx - d / 2, cy - d / 2, cx + d / 2, cy + d / 2), i);
        const bool hot = id >= 0 && gHot == id;
        if (i < PRESET_COUNT) {
            const bool sel = g.color == kPresets[i];
            isPreset |= sel;
            Circle(cx, cy, d / 2 - 4, C(FromRef(kPresets[i])), true);
            if (sel) Circle(cx, cy, d / 2 - 0.5f, C(TEXT), false, 2);
            else if (hot) Circle(cx, cy, d / 2 - 0.5f, C(MUTED), false, 1.5f);
        } else if (!isPreset) {  // custom colour in use
            Circle(cx, cy, d / 2 - 4, C(FromRef(g.color)), true);
            Circle(cx, cy, d / 2 - 0.5f, C(TEXT), false, 2);
        } else {  // "pick a custom colour" button
            Circle(cx, cy, d / 2 - 1, C(hot ? PANEL_HI : PANEL), true);
            Circle(cx, cy, d / 2 - 1, hot ? C(acc) : C(LINE), false, 1.2f);
            Line(cx - 5, cy, cx + 5, cy, hot ? C(acc) : C(MUTED), 1.8f);
            Line(cx, cy - 5, cx, cy + 5, hot ? C(acc) : C(MUTED), 1.8f);
        }
    }
    y += d + 8;
    Slider(y, L"OPACITY", &g.opacity, 10, 100, L"%", false, acc);
    y += 28 + 12;

    // Shape sliders (size, thickness, gap, dot, outline).
    Section(y, L"SHAPE", nullptr, acc);
    y += 22;
    for (int i = 0; i < 5; ++i, y += 28)
        Slider(y, kFields[i].label, kFields[i].v, kFields[i].lo, kFields[i].hi, L" px", i == 4, acc);
    y += 12;

    // Position: X/Y steppers + centre.
    Section(y, L"POSITION", nullptr, acc);
    y += 24;
    Stepper(M, y, L"X", &g.x, acc);
    Stepper(M + 142, y, L"Y", &g.y, acc);
    const D2D1_RECT_F cb = RC(CW - M - 74, y, CW - M, y + 28);
    const int cid = Add(K_CENTER, cb);
    Fill(cb, 6, cid >= 0 && gHot == cid ? C(acc, 0.18f) : C(acc, 0.06f));
    Stroke(Inset(cb, 0.5f), 6, C(acc, 0.8f), 1.2f);
    Text(L"CENTER", cb, fSection, C(acc), DWRITE_TEXT_ALIGNMENT_CENTER);
    y += 28 + 12;

    // Screen selector.
    Text(L"SCREEN", RC(M, y, M + 96, y + 28), fLabel, C(MUTED));
    const D2D1_RECT_F box = RC(M + 100, y, CW - M, y + 28);
    const bool multi = gMonCount > 1;
    Fill(box, 6, C(PANEL));
    Stroke(Inset(box, 0.5f), 6, C(LINE));
    const D2D1_RECT_F la = RC(box.left, box.top, box.left + 32, box.bottom);
    const D2D1_RECT_F ra = RC(box.right - 32, box.top, box.right, box.bottom);
    int lid = -1, rid = -1;
    if (multi) { lid = Add(K_SCREEN, la, -1); rid = Add(K_SCREEN, ra, +1); }
    Text(L"‹", la, fGlyph, !multi ? C(LINE) : gHot == lid ? C(acc) : C(TEXT), DWRITE_TEXT_ALIGNMENT_CENTER);
    Text(L"›", ra, fGlyph, !multi ? C(LINE) : gHot == rid ? C(acc) : C(TEXT), DWRITE_TEXT_ALIGNMENT_CENTER);
    if (gMonCount > 0) {
        const Monitor& m = gMons[SelectedMonitor()];
        const wchar_t* num = m.dev + wcslen(m.dev);
        while (num > m.dev && iswdigit(num[-1])) --num;
        wchar_t label[96];
        wsprintfW(label, L"%s: %d×%d%s", *num ? num : L"?", m.rc.right - m.rc.left, m.rc.bottom - m.rc.top,
                  m.primary ? L"  ·  MAIN" : L"");
        Text(label, RC(la.right, box.top, ra.left, box.bottom), fValue, C(multi ? TEXT : MUTED),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    y += 28 + 16;

    Line(M, y, CW - M, y, C(LINE));
    y += 14;

    // On/off switch + hotkey hint.
    const int tid = Add(K_TOGGLE, RC(M, y, M + 200, y + 26));
    const D2D1_RECT_F sw = RC(M, y, M + 48, y + 26);
    if (tid >= 0 && gHot == tid) Fill(Inset(sw, -3), 16, C(acc, 0.15f));
    Fill(sw, 13, g.visible ? C(acc) : C(LINE));
    Circle(g.visible ? sw.right - 13 : sw.left + 13, y + 13, 9, g.visible ? C(BG) : C(MUTED), true);
    Text(g.visible ? L"CROSSHAIR ON" : L"CROSSHAIR OFF", RC(M + 60, y, M + 220, y + 26), fValue,
         C(g.visible ? TEXT : MUTED));
    const D2D1_RECT_F hk = RC(CW - M - 116, y + 1, CW - M, y + 25);
    Stroke(Inset(hk, 0.5f), 6, C(LINE));
    Text(L"CTRL + ALT + H", hk, fSmall, C(MUTED), DWRITE_TEXT_ALIGNMENT_CENTER);
    y += 26 + 14;

    Text(L"Arrow keys nudge  ·  Shift = 10 px  ·  Scroll to adjust  ·  Esc to tray",
         RC(M, y, CW - M, y + 16), fSmall, C(MUTED));
    return y + 16 + 18;
}

static IDWriteTextFormat* MakeFont(float size, DWRITE_FONT_WEIGHT w,
                                   DWRITE_FONT_STRETCH st = DWRITE_FONT_STRETCH_NORMAL) {
    IDWriteTextFormat* f = nullptr;
    // Bahnschrift ships with Windows 10+; DirectWrite falls back to Segoe UI if it is missing.
    gDw->CreateTextFormat(L"Bahnschrift", nullptr, w, DWRITE_FONT_STYLE_NORMAL, st, size, L"en-us", &f);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return f;
}

static void InitGraphics() {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, (void**)&gD2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&gDw);
    D2D1_STROKE_STYLE_PROPERTIES sp = {};
    sp.startCap = sp.endCap = sp.dashCap = D2D1_CAP_STYLE_ROUND;
    gD2d->CreateStrokeStyle(&sp, nullptr, 0, &gRound);
    fBrand = MakeFont(22, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STRETCH_CONDENSED);
    fSection = MakeFont(12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    fLabel = MakeFont(12.5f, DWRITE_FONT_WEIGHT_NORMAL);
    fValue = MakeFont(13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    fSmall = MakeFont(11, DWRITE_FONT_WEIGHT_NORMAL);
    fGlyph = MakeFont(19, DWRITE_FONT_WEIGHT_SEMI_BOLD);
}

static void DiscardTarget() {
    if (gBrush) { gBrush->Release(); gBrush = nullptr; }
    if (gRt) { gRt->Release(); gRt = nullptr; }
}

static void Paint() {
    if (!gRt) {
        RECT rc;
        GetClientRect(gMain, &rc);
        D2D1_RENDER_TARGET_PROPERTIES rp = {};
        rp.dpiX = rp.dpiY = (float)gDpi;
        D2D1_HWND_RENDER_TARGET_PROPERTIES hp = { gMain, { (UINT32)rc.right, (UINT32)rc.bottom }, D2D1_PRESENT_OPTIONS_NONE };
        if (FAILED(gD2d->CreateHwndRenderTarget(&rp, &hp, &gRt))) return;
        D2D1_COLOR_F white = C(TEXT);
        gRt->CreateSolidColorBrush(&white, nullptr, &gBrush);
    }
    gRt->BeginDraw();
    D2D1_COLOR_F bg = C(BG);
    gRt->Clear(&bg);
    gDraw = true;
    Layout();
    gDraw = false;
    if (gRt->EndDraw(nullptr, nullptr) == D2DERR_RECREATE_TARGET) DiscardTarget();
}

// Window frame: dark title bar and a border in the accent colour (Windows 11; ignored on 10).
static void UpdateFrame() {
    if (!gMain) return;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(gMain, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF caption = RGB(BG >> 16, (BG >> 8) & 255, BG & 255), text = RGB(232, 236, 244);
    const UINT a = Accent();
    COLORREF border = RGB(a >> 16, (a >> 8) & 255, a & 255);
    DwmSetWindowAttribute(gMain, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    DwmSetWindowAttribute(gMain, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof(text));
    DwmSetWindowAttribute(gMain, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
}

static void Changed() {
    ApplyOverlay();
    UpdateFrame();
    Redraw();
}

static void PickColor() {
    static COLORREF custom[16];
    CHOOSECOLORW cc = { sizeof(cc) };
    cc.hwndOwner = gMain;
    cc.rgbResult = g.color;
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&cc)) {
        g.color = cc.rgbResult;
        Changed();
    }
}

static void UpdateStatus(bool notify);

static void ToggleVisible() {
    g.visible = !g.visible;
    ApplyOverlay();
    UpdateStatus(false);
}

static void CycleScreen(int dir) {
    if (gMonCount < 2) return;
    const int i = (SelectedMonitor() + dir + gMonCount) % gMonCount;
    wcscpy(gScreen, gMons[i].dev);
    Changed();
}

static void Nudge(int* v, int d, int lo, int hi) {
    const int n = Clamp(*v + d, lo, hi);
    if (n != *v) { *v = n; Changed(); }
}

static void SliderTo(const Widget& w, float x) {
    const float t = Clamp01((x - w.r.left) / (w.r.right - w.r.left));
    Nudge(w.v, w.lo + (int)lroundf(t * (w.hi - w.lo)) - *w.v, w.lo, w.hi);
}

static int HitTest(float x, float y) {
    for (int i = gWN - 1; i >= 0; --i) {
        D2D1_RECT_F r = gW[i].r;
        if (gW[i].kind == K_SLIDER) { r.left -= 8; r.right += 8; r.top -= 12; r.bottom += 12; }
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
    }
    return -1;
}

static void Press(int i, float x) {
    const Widget w = gW[i];
    switch (w.kind) {
    case K_PREVIEW: gZoom = gZoom == 4 ? 1 : gZoom * 2; Redraw(); break;
    case K_STYLE: g.style = w.arg; Changed(); break;
    case K_SWATCH:
        if (w.arg < PRESET_COUNT) { g.color = kPresets[w.arg]; Changed(); }
        else { gActive = -1; ReleaseCapture(); PickColor(); }
        break;
    case K_SLIDER: SliderTo(w, x); break;
    case K_STEP: Nudge(w.v, w.arg, w.lo, w.hi); SetTimer(gMain, TIMER_REPEAT, 380, nullptr); break;
    case K_CENTER: g.x = g.y = 0; Changed(); break;
    case K_SCREEN: CycleScreen(w.arg); break;
    case K_TOGGLE: ToggleVisible(); break;
    }
}

static void Wheel(int i, int d) {
    const Widget w = gW[i];
    switch (w.kind) {
    case K_SLIDER: case K_STEP: Nudge(w.v, d, w.lo, w.hi); break;
    case K_STYLE: g.style = (g.style - d % ST_COUNT + ST_COUNT) % ST_COUNT; Changed(); break;
    case K_SCREEN: CycleScreen(-d); break;
    case K_PREVIEW: gZoom = d > 0 ? (gZoom < 4 ? gZoom * 2 : 4) : (gZoom > 1 ? gZoom / 2 : 1); Redraw(); break;
    default: break;
    }
}

// ---------------------------------------------------------------- tray

static void AddTrayIcon() {
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = gMain;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = gIconSm;
    wcscpy(nid.szTip, L"Simple Crosshair");
    Shell_NotifyIconW(NIM_ADD, &nid);
    UpdateStatus(false);
}

// Status badge + tray tooltip; `notify` also pops a one-off tray notification.
static void UpdateStatus(bool notify) {
    Redraw();

    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = gMain;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    wcscpy(nid.szTip, !g.visible ? L"Simple Crosshair - hidden"
                    : gExclusive ? L"Simple Crosshair - blocked by exclusive fullscreen"
                                 : L"Simple Crosshair - showing");
    if (notify) {
        nid.uFlags |= NIF_INFO;
        nid.dwInfoFlags = NIIF_INFO;
        wcscpy(nid.szInfoTitle, L"Crosshair can't show over this game");
        wcscpy(nid.szInfo, L"It's in exclusive fullscreen, where Windows doesn't draw overlays. "
                           L"Set the game's display mode to Borderless or Windowed.");
    }
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// Windows reports exclusive (D3D) fullscreen through the shell's notification state; nothing
// about the game itself is inspected.
static void CheckExclusive() {
    QUERY_USER_NOTIFICATION_STATE st;
    const bool exclusive = SUCCEEDED(SHQueryUserNotificationState(&st)) && st == QUNS_RUNNING_D3D_FULL_SCREEN;
    if (exclusive == gExclusive) return;
    gExclusive = exclusive;
    UpdateStatus(exclusive && g.visible);
}

static void RemoveTrayIcon() {
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = gMain;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void ShowSettings() {
    ShowWindow(gMain, IsIconic(gMain) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(gMain);
}

static void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Settings");
    AppendMenuW(menu, MF_STRING | (g.visible ? MF_CHECKED : 0), IDM_TOGGLE, L"Show crosshair\tCtrl+Alt+H");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");
    SetMenuDefaultItem(menu, IDM_SETTINGS, FALSE);

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(gMain);  // required so the menu closes when clicking elsewhere
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, gMain, nullptr);
    PostMessageW(gMain, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == gTaskbarCreated && msg) {  // Explorer restarted; put the tray icon back
        AddTrayIcon();
        return 0;
    }
    switch (msg) {
    case WM_CREATE:
        gMain = hwnd;
        UpdateFrame();
        AddTrayIcon();
        RegisterHotKey(hwnd, HOTKEY_TOGGLE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'H');
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        Paint();
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        const float x = Dip(GET_X_LPARAM(lp)), y = Dip(GET_Y_LPARAM(lp));
        if (gActive >= 0 && gW[gActive].kind == K_SLIDER) SliderTo(gW[gActive], x);
        const int h = gActive >= 0 ? gActive : HitTest(x, y);
        if (h != gHot) { gHot = h; Redraw(); }
        if (!gTracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            gTracking = TrackMouseEvent(&tme) != FALSE;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        gTracking = false;
        if (gActive < 0 && gHot >= 0) { gHot = -1; Redraw(); }
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const float x = Dip(GET_X_LPARAM(lp)), y = Dip(GET_Y_LPARAM(lp));
        const int i = HitTest(x, y);
        if (i < 0) return 0;
        gActive = gHot = i;
        SetCapture(hwnd);
        Press(i, x);
        return 0;
    }

    case WM_LBUTTONUP:
        if (gActive >= 0) ReleaseCapture();
        return 0;

    case WM_CAPTURECHANGED:
        KillTimer(hwnd, TIMER_REPEAT);
        if (gActive >= 0) { gActive = -1; Redraw(); }
        return 0;

    case WM_MOUSEWHEEL: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &p);
        const int i = HitTest(Dip(p.x), Dip(p.y)), d = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        if (i >= 0 && d) Wheel(i, d);
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && gHot >= 0) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;

    case WM_KEYDOWN: {
        const int step = GetKeyState(VK_SHIFT) < 0 ? 10 : 1;
        switch (wp) {
        case VK_LEFT: Nudge(&g.x, -step, -4000, 4000); return 0;
        case VK_RIGHT: Nudge(&g.x, step, -4000, 4000); return 0;
        case VK_UP: Nudge(&g.y, -step, -4000, 4000); return 0;
        case VK_DOWN: Nudge(&g.y, step, -4000, 4000); return 0;
        case VK_ESCAPE: PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
        }
        break;
    }

    case WM_TIMER:
        if (wp == TIMER_REPEAT) {  // press-and-hold on a +/- button
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            if (gActive >= 0 && gW[gActive].kind == K_STEP && HitTest(Dip(p.x), Dip(p.y)) == gActive)
                Nudge(gW[gActive].v, gW[gActive].arg, gW[gActive].lo, gW[gActive].hi);
            SetTimer(hwnd, TIMER_REPEAT, 45, nullptr);
        }
        return 0;

    case WM_TRAY:
        if (lp == WM_LBUTTONUP) ShowSettings();
        else if (lp == WM_RBUTTONUP) ShowTrayMenu();
        return 0;

    case WM_CLOSE:  // closing the settings window only hides it; the crosshair stays up
        SaveSettings();
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) ShowWindow(hwnd, SW_HIDE);
        else if (gRt) {
            D2D1_SIZE_U size = { LOWORD(lp), HIWORD(lp) };
            gRt->Resize(&size);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_SETTINGS: ShowSettings(); break;
        case IDM_TOGGLE: ToggleVisible(); break;
        case IDM_EXIT: DestroyWindow(hwnd); break;
        }
        return 0;

    case WM_HOTKEY:
        if (wp == HOTKEY_TOGGLE) ToggleVisible();
        return 0;

    case WM_ENDSESSION:
        if (wp) SaveSettings();
        return 0;

    case WM_DESTROY:
        SaveSettings();
        RemoveTrayIcon();
        UnregisterHotKey(hwnd, HOTKEY_TOGGLE);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Small green crosshair for the taskbar/title bar, drawn at runtime.
static HICON MakeIcon(int n) {
    BITMAPINFO bi = {};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), n, -n, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    DWORD* px = (DWORD*)bits;
    const int c = n / 2, t = n >= 32 ? 4 : 2, gap = n / 8;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            bool h = y >= c - t / 2 && y < c + t / 2 && abs(x - c) >= gap;
            bool v = x >= c - t / 2 && x < c + t / 2 && abs(y - c) >= gap;
            px[y * n + x] = (h || v) ? 0xFF20E040 : 0;
        }
    BYTE* zero = (BYTE*)calloc(n * n / 8 + n, 1);
    HBITMAP mask = CreateBitmap(n, n, 1, 1, zero);
    free(zero);
    ICONINFO ii = { TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    // One instance only; a second launch just brings the settings window back.
    CreateMutexW(nullptr, TRUE, L"SimpleCrosshair.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"SimpleCrosshairMain", nullptr)) {
            ShowWindow(existing, IsIconic(existing) ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    gDpi = GetDpiForSystem();
    LoadSettings();
    InitGraphics();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"SimpleCrosshairOverlay";
    RegisterClassExW(&wc);

    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = MainProc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(BG >> 16, (BG >> 8) & 255, BG & 255));
    wc.hIcon = MakeIcon(S(32));
    wc.hIconSm = gIconSm = MakeIcon(GetSystemMetricsForDpi(SM_CXSMICON, gDpi));
    gTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    wc.lpszClassName = L"SimpleCrosshairMain";
    RegisterClassExW(&wc);

    // Layered + transparent = fully click-through; toolwindow keeps it out of Alt+Tab.
    gOverlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                               WS_EX_NOACTIVATE,
                               L"SimpleCrosshairOverlay", L"Simple Crosshair Overlay", WS_POPUP, 0, 0, 1, 1,
                               nullptr, nullptr, inst, nullptr);
    SetTimer(gOverlay, TIMER_TOPMOST, 500, nullptr);
    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnForegroundChanged, 0, 0,
                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    ApplyOverlay();

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = { 0, 0, S((int)CW), (int)ceilf(Layout() * gDpi / 96.f) };  // measure-only pass
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, gDpi);
    CreateWindowExW(0, L"SimpleCrosshairMain", L"Simple Crosshair", style, CW_USEDEFAULT, CW_USEDEFAULT,
                    rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    ShowWindow(gMain, show);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
