// Simple Crosshair - a minimal crosshair overlay for Windows.
//
// It only draws a small, click-through, always-on-top image at the centre of
// the primary monitor. By design it does NOT read or write other processes'
// memory, inject DLLs, hook DirectX/input, capture the screen, look for game
// windows, or need admin rights. Settings live in SimpleCrosshair.ini next to the exe.

#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00  // Windows 10+ (per-monitor DPI APIs)
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <math.h>
#include <stdlib.h>
#include <wchar.h>

enum { ST_CROSS, ST_CROSS_DOT, ST_T, ST_DOT, ST_CIRCLE, ST_CIRCLE_DOT, ST_COUNT };
static const wchar_t* kStyles[ST_COUNT] = {
    L"Cross", L"Cross + dot", L"T-shape", L"Dot", L"Circle", L"Circle + dot",
};

static struct {
    int style, x, y, size, thickness, gap, dot, outline, opacity, visible;
    COLORREF color;
} g = { ST_CROSS, 0, 0, 6, 2, 3, 2, 1, 100, 1, RGB(0, 255, 0) };

struct Field { int id; const wchar_t* label; const wchar_t* key; int* v; int lo, hi; };
static Field kFields[] = {
    { 110, L"Size",      L"Size",      &g.size,      0,     200 },
    { 111, L"Thickness", L"Thickness", &g.thickness, 1,     30 },
    { 112, L"Gap",       L"Gap",       &g.gap,       0,     100 },
    { 113, L"Dot size",  L"Dot",       &g.dot,       1,     30 },
    { 114, L"Outline",   L"Outline",   &g.outline,   0,     5 },
    { 115, L"X offset",  L"X",         &g.x,         -4000, 4000 },
    { 116, L"Y offset",  L"Y",         &g.y,         -4000, 4000 },
};
enum { IDC_STYLE = 100, IDC_SWATCH, IDC_COLOR, IDC_OPACITY, IDC_SHOW, IDC_CENTER, IDC_SCREEN, IDC_STATUS,
       UPDOWN_OFFSET = 50 };
enum { HOTKEY_TOGGLE = 1, TIMER_TOPMOST = 1 };
enum { WM_TRAY = WM_APP + 1, IDM_SETTINGS = 200, IDM_TOGGLE, IDM_EXIT };

static HWND gMain, gOverlay;
static HICON gIconSm;
static UINT gTaskbarCreated;
static bool gExclusive;  // a game is in exclusive fullscreen, where Windows won't draw overlays
static HFONT gFont;
static HBRUSH gSwatchBrush;
static UINT gDpi = 96;
static bool gLoading;
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

static void FillScreenCombo();

// Rasterises the crosshair into a premultiplied BGRA bitmap and pushes it to
// the layered window. Only runs when a setting changes, so idle cost is zero.
static void ApplyOverlay() {
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

    BITMAPINFO bi = {};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -W, 1, 32, BI_RGB };
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);

    BYTE* p = (BYTE*)bits;
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

    POINT c = ScreenCenter(), src = { 0, 0 };
    POINT dst = { c.x - R + g.x, c.y - R + g.y };
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
        FillScreenCombo();
        ApplyOverlay();
        return 0;
    case WM_TIMER:  // safety net for anything the foreground event misses
        RaiseOverlay();
        CheckExclusive();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- settings window

static HWND Ctl(DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style,
                int x, int y, int w, int h, int id) {
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                           gMain, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
}

static BOOL CALLBACK SetFontProc(HWND child, LPARAM font) {
    SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
    return TRUE;
}

// Lists screens left-to-right as "N: WxH", N being Windows' own display number.
static void FillScreenCombo() {
    HWND combo = gMain ? GetDlgItem(gMain, IDC_SCREEN) : nullptr;
    if (!combo) return;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < gMonCount; ++i) {
        const Monitor& m = gMons[i];
        const wchar_t* num = m.dev + wcslen(m.dev);
        while (num > m.dev && iswdigit(num[-1])) --num;
        wchar_t label[96];
        wsprintfW(label, L"%s: %dx%d%s", *num ? num : L"?", m.rc.right - m.rc.left,
                  m.rc.bottom - m.rc.top, m.primary ? L" (main)" : L"");
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)label);
    }
    SendMessageW(combo, CB_SETCURSEL, SelectedMonitor(), 0);
    EnableWindow(combo, gMonCount > 1);
}

static void UpdateSwatch() {
    if (gSwatchBrush) DeleteObject(gSwatchBrush);
    gSwatchBrush = CreateSolidBrush(g.color);
    InvalidateRect(GetDlgItem(gMain, IDC_SWATCH), nullptr, TRUE);
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
        UpdateSwatch();
        ApplyOverlay();
    }
}

static void UpdateStatus(bool notify);

static void ToggleVisible() {
    g.visible = !g.visible;
    CheckDlgButton(gMain, IDC_SHOW, g.visible ? BST_CHECKED : BST_UNCHECKED);
    ApplyOverlay();
    UpdateStatus(false);
}

static void BuildUi(HWND hwnd) {
    gMain = hwnd;
    gLoading = true;
    const int L = 14, C = 100, CW = 156, ROW = 32;
    int y = 14;

    Ctl(0, L"STATIC", L"Style", 0, L, y + 4, 80, 20, -1);
    HWND combo = Ctl(0, WC_COMBOBOXW, nullptr, CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                     C, y, CW, 200, IDC_STYLE);
    for (const wchar_t* s : kStyles) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)s);
    SendMessageW(combo, CB_SETCURSEL, g.style, 0);
    y += ROW;

    Ctl(0, L"STATIC", L"Screen", 0, L, y + 4, 80, 20, -1);
    Ctl(0, WC_COMBOBOXW, nullptr, CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, C, y, CW, 200, IDC_SCREEN);
    FillScreenCombo();
    y += ROW;

    Ctl(0, L"STATIC", L"Color", 0, L, y + 4, 80, 20, -1);
    Ctl(WS_EX_CLIENTEDGE, L"STATIC", nullptr, 0, C, y + 1, 26, 22, IDC_SWATCH);
    Ctl(0, L"BUTTON", L"Choose...", BS_PUSHBUTTON | WS_TABSTOP, C + 34, y, CW - 34, 25, IDC_COLOR);
    y += ROW;

    Ctl(0, L"STATIC", L"Opacity", 0, L, y + 4, 80, 20, -1);
    HWND track = Ctl(0, TRACKBAR_CLASSW, nullptr, TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
                     C - 6, y, CW + 12, 26, IDC_OPACITY);
    SendMessageW(track, TBM_SETRANGE, TRUE, MAKELPARAM(10, 100));
    SendMessageW(track, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(track, TBM_SETPOS, TRUE, g.opacity);
    y += ROW;

    for (Field& f : kFields) {
        Ctl(0, L"STATIC", f.label, 0, L, y + 4, 80, 20, -1);
        HWND edit = Ctl(WS_EX_CLIENTEDGE, L"EDIT", nullptr, ES_RIGHT | ES_AUTOHSCROLL | WS_TABSTOP,
                        C, y, 90, 24, f.id);
        HWND spin = Ctl(0, UPDOWN_CLASSW, nullptr,
                        UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS | UDS_HOTTRACK,
                        0, 0, 0, 0, f.id + UPDOWN_OFFSET);
        SendMessageW(spin, UDM_SETBUDDY, (WPARAM)edit, 0);
        SendMessageW(spin, UDM_SETRANGE32, f.lo, f.hi);
        SendMessageW(spin, UDM_SETPOS32, 0, *f.v);
        y += ROW;
    }

    Ctl(0, L"BUTTON", L"Show", BS_AUTOCHECKBOX | WS_TABSTOP, L, y + 2, 80, 22, IDC_SHOW);
    CheckDlgButton(hwnd, IDC_SHOW, g.visible ? BST_CHECKED : BST_UNCHECKED);
    Ctl(0, L"BUTTON", L"Re-center", BS_PUSHBUTTON | WS_TABSTOP, C, y, 90, 25, IDC_CENTER);
    y += ROW + 4;

    Ctl(0, L"STATIC", L"Ctrl+Alt+H shows/hides the crosshair.\n"
                      L"Run games in borderless or windowed mode.\n"
                      L"Closing keeps it running in the tray.",
        0, L, y, C + CW - L, 54, -1);
    y += 54 + 8;

    Ctl(0, L"STATIC", nullptr, 0, L, y, C + CW - L, 54, IDC_STATUS);
    y += 54 + 8;

    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, gDpi);
    gFont = CreateFontIndirectW(&ncm.lfMessageFont);
    EnumChildWindows(hwnd, SetFontProc, (LPARAM)gFont);

    RECT rc = { 0, 0, S(C + CW + L), S(y) };
    AdjustWindowRectExForDpi(&rc, GetWindowLongW(hwnd, GWL_STYLE), FALSE, 0, gDpi);
    SetWindowPos(hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER);

    UpdateSwatch();
    gLoading = false;
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

// Settings status line + tray tooltip; `notify` also pops a one-off tray notification.
static void UpdateStatus(bool notify) {
    const wchar_t* status = !g.visible ? L"Status: hidden (Ctrl+Alt+H to show)"
                          : gExclusive ? L"Status: blocked by an exclusive fullscreen game. "
                                         L"Set the game to Borderless or Windowed."
                                       : L"Status: showing";
    SetDlgItemTextW(gMain, IDC_STATUS, status);

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
        BuildUi(hwnd);
        AddTrayIcon();
        RegisterHotKey(hwnd, HOTKEY_TOGGLE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'H');
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
        return 0;

    case WM_COMMAND: {
        const int id = LOWORD(wp), code = HIWORD(wp);
        for (Field& f : kFields) {
            if (f.id != id) continue;
            if (code == EN_CHANGE && !gLoading) {
                BOOL ok = FALSE;
                int v = (int)GetDlgItemInt(hwnd, id, &ok, TRUE);
                if (ok) { *f.v = Clamp(v, f.lo, f.hi); ApplyOverlay(); }
            } else if (code == EN_KILLFOCUS) {
                SetDlgItemInt(hwnd, id, *f.v, TRUE);  // show the clamped value
            }
            return 0;
        }
        if (id == IDC_SCREEN && code == CBN_SELCHANGE) {
            int sel = (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < gMonCount) wcscpy(gScreen, gMons[sel].dev);
            ApplyOverlay();
        } else if (id == IDC_STYLE && code == CBN_SELCHANGE) {
            g.style = (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
            ApplyOverlay();
        } else if (id == IDC_COLOR) {
            PickColor();
        } else if (id == IDC_SHOW) {
            ToggleVisible();
        } else if (id == IDC_CENTER) {
            SetDlgItemInt(hwnd, 115, 0, TRUE);
            SetDlgItemInt(hwnd, 116, 0, TRUE);
        } else if (id == IDM_SETTINGS) {
            ShowSettings();
        } else if (id == IDM_TOGGLE) {
            ToggleVisible();
        } else if (id == IDM_EXIT) {
            DestroyWindow(hwnd);
        }
        return 0;
    }

    case WM_HSCROLL:
        if ((HWND)lp == GetDlgItem(hwnd, IDC_OPACITY)) {
            g.opacity = (int)SendMessageW((HWND)lp, TBM_GETPOS, 0, 0);
            ApplyOverlay();
        }
        return 0;

    case WM_HOTKEY:
        if (wp == HOTKEY_TOGGLE) ToggleVisible();
        return 0;

    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == GetDlgItem(hwnd, IDC_SWATCH)) return (LRESULT)gSwatchBrush;
        break;

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

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);
    gDpi = GetDpiForSystem();
    LoadSettings();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"SimpleCrosshairOverlay";
    RegisterClassExW(&wc);

    wc.lpfnWndProc = MainProc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
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

    CreateWindowExW(0, L"SimpleCrosshairMain", L"Simple Crosshair", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                    CW_USEDEFAULT, CW_USEDEFAULT, 300, 400, nullptr, nullptr, inst, nullptr);
    ShowWindow(gMain, show);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(gMain, &m)) continue;  // Tab / arrow-key navigation
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
