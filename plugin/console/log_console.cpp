// ============================================================================
// log_console.cpp - overlay log panel, implemented as a plugin.
//
// The panel is its OWN window now, not a draw callback on the app's swapchain.
// Why: drawing inside the app's Present() ties the panel to Flutter's frame
// cadence. Flutter only renders when it schedules a frame, so a drag (whose
// input the plugin swallows) produced zero repaints and looked badly laggy.
// With a real child window the panel repaints from tick(), independent of the
// app, and mouse input arrives through its own WndProc.
//
// Threading rule that matters here: a window's message queue belongs to the
// thread that created it. This module owns one pump thread; the window is created
// lazily inside that loop and the loop is what pumps it. "init" only validates
// and starts the thread.
//
// Module contract (JSON only, same as framework.dll): export NotifyPlatform,
// receive {"cmd":"init","dispatch":"<NotifyGhost ptr>","ctx":"0"}, query the host
// with {"cmd":"mainWindow"|"viewWindow"|"scale"}, publish a line with
// {"cmd":"log","text":...}, register the log sink with {"cmd":"logSink","ptr":...},
// and stop on {"cmd":"shutdown"}.
//
// Rendering: WS_EX_LAYERED child of the app's view window + a D2D DC render
// target bound to a top-down 32bpp DIB, pushed with UpdateLayeredWindow. That
// keeps the same look as before (82% dark rounded panel) with per-pixel alpha.
//
// Keep this file pure ASCII.
// ============================================================================

#include <windows.h>

#include <cstdlib>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <cwchar>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using Microsoft::WRL::ComPtr;

namespace {

const int kRingLines     = 256;      // buffered lines between sink and panel
const int kRingLineChars = 256;      // per line, including terminator

const int   kVisibleLines = 16;      // lines shown in the panel
const int   kLineH        = 18;      // line pitch, logical px
const float kFontSize     = 13.0f;   // DWrite size in DIPs == logical px at 96 dpi
const float kPanelW       = 540.0f;  // panel width, logical px
const float kPad          = 8.0f;    // inner padding, logical px
const float kMargin       = 24.0f;   // gap from the window edge, logical px
const float kCorner       = 6.0f;    // rounded corner radius, logical px
const float kTitleH       = 26.0f;   // title bar height, logical px, drag handle
const float kPanelH       = kVisibleLines * kLineH + 2.0f * kPad;  // log area
const float kTotalH       = kTitleH + kPanelH;                     // whole window
const UINT  kIdleRepaintMs = 250;    // refresh floor when no new line arrives
const wchar_t* kTitleText = L"console";

const wchar_t* kPanelClass = L"ppluginConsolePanel";

struct RingEntry {
    char text[kRingLineChars];
};

RingEntry g_ring[kRingLines];
int       g_head = 0;                // next write position
int       g_tail = 0;                // oldest unread position
SRWLOCK   g_ringLock = SRWLOCK_INIT;
volatile LONG g_dirty = 1;           // ring changed, needs a repaint

// -- JSON glue ---------------------------------------------------------------
//
// The only contract with the host is NotifyPlatform / NotifyGhost + JSON. The
// payloads here are tiny and fixed, so a couple of substring helpers are enough;
// no JSON library (and no shared ABI header) is needed.

typedef int (__cdecl* GhostFn)(const char* req, char* resp, int cap, void* ctx);

GhostFn   g_ghost = nullptr;
void*     g_ghostCtx = nullptr;
bool      g_ready = false;           // dispatch handed over by "init"
HANDLE    g_thread = nullptr;        // pump thread
volatile bool g_stop = false;

// {"key":"value"} -> value (no escape handling: the payloads are cmd/hwnd/ptr).
bool JsonStr(const char* json, const char* key, char* out, int cap) {
    if (json == nullptr || key == nullptr || out == nullptr || cap <= 0) return false;
    char pat[32] = {};
    _snprintf_s(pat, sizeof(pat), _TRUNCATE, "\"%s\"", key);
    const char* p = strstr(json, pat);
    if (p == nullptr) return false;
    p = strchr(p + strlen(pat), ':');
    if (p == nullptr) return false;
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '"') ++p;
    int n = 0;
    while (*p != '\0' && *p != '"' && *p != ',' && *p != '}' && n + 1 < cap) {
        out[n++] = *p++;
    }
    out[n] = '\0';
    return n > 0;
}

// {"key":123.5} -> number.
bool JsonNum(const char* json, const char* key, double* out) {
    if (json == nullptr || key == nullptr || out == nullptr) return false;
    char pat[32] = {};
    _snprintf_s(pat, sizeof(pat), _TRUNCATE, "\"%s\"", key);
    const char* p = strstr(json, pat);
    if (p == nullptr) return false;
    p = strchr(p + strlen(pat), ':');
    if (p == nullptr) return false;
    *out = strtod(p + 1, nullptr);
    return true;
}

bool GhostCall(const char* req, char* resp, int cap) {
    if (g_ghost == nullptr) return false;
    resp[0] = '\0';
    return g_ghost(req, resp, cap, g_ghostCtx) >= 0;
}

// Copy a JSON body into the caller's buffer (mirrors framework's NotifyPlatform).
int WriteResp(const char* body, char* resp, int cap) {
    if (body == nullptr || resp == nullptr || cap <= 0) return -1;
    const int need = static_cast<int>(strlen(body));
    if (need + 1 > cap) return -(need + 1);
    memcpy(resp, body, static_cast<size_t>(need));
    resp[need] = '\0';
    return need;
}

// {"cmd":"mainWindow"|"viewWindow"} -> hwnd (nullptr while the host has no UI).
HWND QueryWindow(const char* cmd) {
    char req[96] = {};
    _snprintf_s(req, sizeof(req), _TRUNCATE, "{\"cmd\":\"%s\"}", cmd);
    char resp[256] = {};
    if (!GhostCall(req, resp, sizeof(resp))) return nullptr;
    char value[32] = {};
    if (!JsonStr(resp, "hwnd", value, sizeof(value))) return nullptr;
    return reinterpret_cast<HWND>(
        static_cast<uintptr_t>(_strtoui64(value, nullptr, 10)));
}

float QueryScale() {
    char resp[256] = {};
    double value = 1.0;
    if (GhostCall("{\"cmd\":\"scale\"}", resp, sizeof(resp)) &&
        JsonNum(resp, "scale", &value) && value > 0.0) {
        return static_cast<float>(value);
    }
    return 1.0f;
}

HINSTANCE                 g_inst = nullptr;
ComPtr<IDWriteFactory>    g_dw;
ComPtr<IDWriteTextFormat> g_fmt;
ComPtr<IDWriteTextFormat> g_titleFmt;
ComPtr<ID2D1Factory>      g_d2d;
ComPtr<ID2D1DCRenderTarget> g_rt;

// Panel window and its surfaces. All physical pixels; the window is a WS_POPUP
// owned by the app's top level window, so positions are screen coordinates.
HWND    g_panel = nullptr;
HWND    g_owner = nullptr;
POINT   g_ownerOrigin = {0, 0};      // owner client origin, screen px
bool    g_ownerIconic = false;
HDC     g_memDC = nullptr;
HBITMAP g_bmp = nullptr;
HGDIOBJ g_oldBmp = nullptr;
void*   g_bits = nullptr;
UINT    g_bmpW = 0;
UINT    g_bmpH = 0;
int     g_winW = 0;
int     g_winH = 0;
float   g_scale = 1.0f;
bool    g_dragging = false;
bool    g_titleHover = false;        // cursor over the title bar
POINT   g_grab = {0, 0};             // cursor offset inside the window at DOWN
ULONGLONG g_lastPaint = 0;

// -- collapsed chip ---------------------------------------------------------
//
// Same idea as the script panel: folding the console leaves a small chip in the
// same place, and clicking the chip brings the panel back. There is no toolbar
// here, so the fold button lives at the right end of the title bar.

const float kFoldW = 26.0f;          // fold button width inside the title bar
const float kMiniW = 96.0f;
const float kMiniH = 26.0f;
const wchar_t* kMiniText = L"console";

HWND    g_mini = nullptr;
bool    g_miniHover = false;
bool    g_miniDrag = false;
bool    g_miniVisible = false;       // survives app minimize/restore
bool    g_foldHover = false;
POINT   g_miniGrab = {0, 0};
POINT   g_miniDown = {0, 0};
HDC     g_mmemDC = nullptr;
HBITMAP g_mbmp = nullptr;
HGDIOBJ g_moldBmp = nullptr;
ComPtr<ID2D1DCRenderTarget> g_mrt;
UINT    g_mbmpW = 0;
UINT    g_mbmpH = 0;

// Amber-highlight the same keywords the built-in overlay panel uses, so a
// failure line stands out in the plugin panel too. Case sensitive wcsstr.
bool IsWarn(const wchar_t* s) {
    static const wchar_t* kTokens[] = {
        L"fail", L"timeout", L"busy", L"ignored", L"skipped",
        L"WARN", L"not seen", L"no json", L"error", L"rejected", L"quarantined"
    };
    for (const wchar_t* t : kTokens) {
        if (wcsstr(s, t) != nullptr) return true;
    }
    return false;
}

// Publish one line into proxy.log through the ghost channel (tag=console).
void LogLine(const char* text) {
    if (text == nullptr || !g_ready) return;
    char escaped[1024] = {};
    int n = 0;
    for (const char* p = text; *p != '\0' && n + 2 < static_cast<int>(sizeof(escaped)); ++p) {
        if (*p == '"' || *p == '\\') {
            escaped[n++] = '\\';
        }
        escaped[n++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
    }
    char req[1200] = {};
    _snprintf_s(req, sizeof(req), _TRUNCATE,
                "{\"cmd\":\"log\",\"tag\":\"console\",\"text\":\"%s\"}", escaped);
    char resp[128] = {};
    GhostCall(req, resp, sizeof(resp));
}

// Framework log sink. Runs on an arbitrary framework thread: only touch the
// ring, and only under the lock.
void __cdecl OnLogLine(const char* line, void*) {
    if (line == nullptr) return;

    AcquireSRWLockExclusive(&g_ringLock);
    strncpy_s(g_ring[g_head % kRingLines].text, kRingLineChars, line, _TRUNCATE);
    ++g_head;
    if (g_head - g_tail > kRingLines) {
        g_tail = g_head - kRingLines;  // drop the oldest line
    }
    ReleaseSRWLockExclusive(&g_ringLock);
    InterlockedExchange(&g_dirty, 1);
}

// The panel is deliberately unclamped: it can be dragged anywhere on the
// desktop, including outside the app window. It is a WS_POPUP owned by the app
// window, so it is not clipped by it, and the owner delta is applied in tick()
// to keep it visually attached while the app window moves.

// Title bar hit test. y is the client coordinate of the cursor.
bool InTitleBar(int y) {
    return y >= 0 && y < static_cast<int>(kTitleH * g_scale + 0.5f);
}

// The fold button sits at the right end of the title bar.
bool InFoldZone(int x) {
    return x >= g_winW - static_cast<int>(kFoldW * g_scale + 0.5f);
}

// Implemented in the surfaces section below.
void HidePanel();
void ShowPanelAgain();
void MiniPaint();

LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN: {
        POINT cur = {};
        POINT client = {};
        GetCursorPos(&cur);
        client = cur;
        ScreenToClient(hwnd, &client);
        if (InTitleBar(client.y) && InFoldZone(client.x)) {
            HidePanel();   // fold to the chip instead of dragging
            return 0;
        }
        if (!InTitleBar(client.y)) {
            return 0;   // only the title bar drags; the log body stays inert
        }
        RECT wr = {};
        GetWindowRect(hwnd, &wr);
        g_grab.x = cur.x - wr.left;
        g_grab.y = cur.y - wr.top;
        g_dragging = true;
        SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT cur = {};
        GetCursorPos(&cur);
        if (g_dragging) {
            // Free placement: screen coordinates, no clamp at all.
            const int nx = cur.x - g_grab.x;
            const int ny = cur.y - g_grab.y;
            SetWindowPos(hwnd, nullptr, nx, ny, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        POINT client = cur;
        ScreenToClient(hwnd, &client);
        const bool hover = InTitleBar(client.y);
        const bool fold = hover && InFoldZone(client.x);
        if (hover && !g_titleHover) {
            TRACKMOUSEEVENT tme = {};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
        }
        if (hover != g_titleHover || fold != g_foldHover) {
            g_titleHover = hover;
            g_foldHover = fold;
            InterlockedExchange(&g_dirty, 1);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_titleHover || g_foldHover) {
            g_titleHover = false;
            g_foldHover = false;
            InterlockedExchange(&g_dirty, 1);
        }
        return 0;
    case WM_SETCURSOR: {
        POINT cur = {};
        GetCursorPos(&cur);
        ScreenToClient(hwnd, &cur);
        if (LOWORD(lp) == HTCLIENT && InTitleBar(cur.y)) {
            if (InFoldZone(cur.x)) {
                SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));  // hand
            } else {
                SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32646)));  // SIZEALL
            }
            return TRUE;
        }
        break;
    }
    case WM_LBUTTONUP: {
        if (!g_dragging) break;
        g_dragging = false;
        ReleaseCapture();
        // Report logical px, the same space the old anchored panel used.
        char line[96] = {};
        RECT wr = {};
        POINT org = {0, 0};
        GetWindowRect(hwnd, &wr);
        HWND parent = GetParent(hwnd);
        if (parent != nullptr) {
            org.x = wr.left;
            org.y = wr.top;
            ScreenToClient(parent, &org);
        }
        const float inv = (g_scale > 0.0f) ? (1.0f / g_scale) : 1.0f;
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "console: panel moved to (%.0f,%.0f)",
                    static_cast<float>(org.x) * inv,
                    static_cast<float>(org.y) * inv);
        LogLine(line);
        return 0;
    }
    case WM_RBUTTONDOWN:
        return 0;
    case WM_ERASEBKGND:
        return 1;   // layered window paints itself from tick()
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// -- surfaces ---------------------------------------------------------------

void ReleaseSurfaces() {
    g_rt.Reset();
    g_d2d.Reset();
    if (g_memDC != nullptr && g_oldBmp != nullptr) {
        SelectObject(g_memDC, g_oldBmp);   // never delete a selected bitmap
        g_oldBmp = nullptr;
    }
    if (g_bmp != nullptr) {
        DeleteObject(g_bmp);
        g_bmp = nullptr;
    }
    if (g_memDC != nullptr) {
        DeleteDC(g_memDC);
        g_memDC = nullptr;
    }
    g_bits = nullptr;
    g_bmpW = 0;
    g_bmpH = 0;
}

// Top-down 32bpp DIB + a D2D DC render target bound to it. Rebuilt only when
// the pixel size changes.
bool EnsureSurfaces(UINT w, UINT h) {
    if (g_rt != nullptr && w == g_bmpW && h == g_bmpH) return true;
    ReleaseSurfaces();
    if (w == 0 || h == 0) return false;

    if (g_d2d == nullptr) {
        D2D1_FACTORY_OPTIONS opts = {};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                     __uuidof(ID2D1Factory), &opts,
                                     reinterpret_cast<void**>(g_d2d.GetAddressOf())))) {
            return false;
        }
    }

    HDC screen = GetDC(nullptr);
    g_memDC = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (g_memDC == nullptr) return false;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(w);
    bi.bmiHeader.biHeight = -static_cast<LONG>(h);   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    g_bmp = CreateDIBSection(g_memDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (g_bmp == nullptr || bits == nullptr) {
        ReleaseSurfaces();
        return false;
    }
    g_oldBmp = SelectObject(g_memDC, g_bmp);
    g_bits = bits;

    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        0.0f, 0.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
    if (FAILED(g_d2d->CreateDCRenderTarget(&props, g_rt.GetAddressOf()))) {
        ReleaseSurfaces();
        return false;
    }
    g_rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    g_bmpW = w;
    g_bmpH = h;
    return true;
}

// Snapshot the tail of the ring into wide lines. Returns the count.
int SnapshotLines(wchar_t (*lines)[kRingLineChars]) {
    int n = 0;
    AcquireSRWLockExclusive(&g_ringLock);
    const int avail = g_head - g_tail;
    const int start = (avail > kVisibleLines) ? (g_head - kVisibleLines) : g_tail;
    n = g_head - start;
    for (int i = 0; i < n; ++i) {
        MultiByteToWideChar(CP_UTF8, 0, g_ring[(start + i) % kRingLines].text, -1,
                            lines[i], kRingLineChars);
    }
    ReleaseSRWLockExclusive(&g_ringLock);
    return n;
}

// One full repaint: draw into the DIB, hand it to the layered window.
void Paint() {
    if (g_panel == nullptr) return;
    if (!EnsureSurfaces(static_cast<UINT>(g_winW), static_cast<UINT>(g_winH))) return;
    if (g_fmt == nullptr) return;

    wchar_t lines[kVisibleLines][kRingLineChars] = {};
    const int n = SnapshotLines(lines);

    const RECT bounds = {0, 0, g_winW, g_winH};
    if (FAILED(g_rt->BindDC(g_memDC, &bounds))) return;

    ComPtr<ID2D1SolidColorBrush> bg;
    ComPtr<ID2D1SolidColorBrush> titleBg;
    ComPtr<ID2D1SolidColorBrush> titleFg;
    ComPtr<ID2D1SolidColorBrush> fg;
    ComPtr<ID2D1SolidColorBrush> warn;
    g_rt->BeginDraw();
    g_rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    const D2D1_COLOR_F titleColor = g_titleHover
        ? D2D1::ColorF(0.16f, 0.19f, 0.25f, 0.92f)
        : D2D1::ColorF(0.10f, 0.12f, 0.16f, 0.90f);
    const bool brushesOk =
        SUCCEEDED(g_rt->CreateSolidColorBrush(D2D1::ColorF(0.04f, 0.05f, 0.07f, 0.82f),
                                              bg.GetAddressOf())) &&
        SUCCEEDED(g_rt->CreateSolidColorBrush(titleColor, titleBg.GetAddressOf())) &&
        SUCCEEDED(g_rt->CreateSolidColorBrush(D2D1::ColorF(0.72f, 0.77f, 0.85f, 1.00f),
                                              titleFg.GetAddressOf())) &&
        SUCCEEDED(g_rt->CreateSolidColorBrush(D2D1::ColorF(0.86f, 0.89f, 0.93f, 1.00f),
                                              fg.GetAddressOf())) &&
        SUCCEEDED(g_rt->CreateSolidColorBrush(D2D1::ColorF(1.00f, 0.72f, 0.30f, 1.00f),
                                              warn.GetAddressOf()));
    if (brushesOk) {
        const float w = static_cast<float>(g_winW);
        const float h = static_cast<float>(g_winH);
        const float titleH = kTitleH * g_scale;
        const D2D1_ROUNDED_RECT panel = D2D1::RoundedRect(
            D2D1::RectF(0.0f, 0.0f, w, h), kCorner * g_scale, kCorner * g_scale);

        // Title band first, then the log body clipped to the same rounded
        // outline so the bottom corners stay transparent.
        g_rt->FillRoundedRectangle(panel, titleBg.Get());
        g_rt->PushAxisAlignedClip(panel.rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        g_rt->FillRectangle(D2D1::RectF(0.0f, titleH, w, h), bg.Get());
        g_rt->FillRectangle(D2D1::RectF(0.0f, titleH - 1.0f, w, titleH),
                            titleFg.Get());
        const float pad = kPad * g_scale;
        const D2D1_RECT_F titleRect = D2D1::RectF(pad, 0.0f, w - pad, titleH);
        if (g_titleFmt != nullptr) {
            g_rt->DrawText(kTitleText, static_cast<UINT32>(wcslen(kTitleText)),
                           g_titleFmt.Get(), &titleRect, titleFg.Get());
        }

        // Fold button: one short bar at the right end of the title bar.
        {
            const float cx = w - (kFoldW * g_scale) * 0.5f;
            const float cy = titleH * 0.5f;
            const float half = 5.0f * g_scale;
            ComPtr<ID2D1SolidColorBrush> fold;
            if (SUCCEEDED(g_rt->CreateSolidColorBrush(
                    g_foldHover ? D2D1::ColorF(0.86f, 0.89f, 0.93f, 1.0f)
                                : D2D1::ColorF(0.55f, 0.60f, 0.68f, 1.0f),
                    fold.GetAddressOf()))) {
                g_rt->DrawLine(D2D1::Point2F(cx - half, cy),
                               D2D1::Point2F(cx + half, cy), fold.Get(),
                               1.4f * g_scale);
            }
        }

        // Log body. Clip so a long line (NO_WRAP) cannot spill outside.
        g_rt->PushAxisAlignedClip(D2D1::RectF(0.0f, titleH, w, h),
                                  D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const float lh = kLineH * g_scale;
        for (int i = 0; i < n; ++i) {
            const float top = titleH + pad + i * lh;
            const D2D1_RECT_F rc = D2D1::RectF(pad, top, w - pad, top + lh);
            ID2D1SolidColorBrush* brush = IsWarn(lines[i]) ? warn.Get() : fg.Get();
            g_rt->DrawText(lines[i], static_cast<UINT32>(wcslen(lines[i])),
                           g_fmt.Get(), &rc, brush);
        }
        g_rt->PopAxisAlignedClip();   // log body
        g_rt->PopAxisAlignedClip();   // rounded outline
    }
    const HRESULT hr = g_rt->EndDraw();
    if (FAILED(hr)) {
        LogLine("console: EndDraw failed, surfaces rebuilt");
        ReleaseSurfaces();
        return;
    }

    // Premultiplied BGRA -> layered window.
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    POINT src = {0, 0};
    SIZE size = {g_winW, g_winH};
    if (!UpdateLayeredWindow(g_panel, nullptr, nullptr, &size, g_memDC, &src, 0,
                             &blend, ULW_ALPHA)) {
        LogLine("console: UpdateLayeredWindow failed");
    }
    g_lastPaint = GetTickCount64();
    InterlockedExchange(&g_dirty, 0);
}

// -- collapsed chip ---------------------------------------------------------

void MiniReleaseSurfaces() {
    g_mrt.Reset();
    if (g_mmemDC != nullptr && g_moldBmp != nullptr) {
        SelectObject(g_mmemDC, g_moldBmp);
        g_moldBmp = nullptr;
    }
    if (g_mbmp != nullptr) {
        DeleteObject(g_mbmp);
        g_mbmp = nullptr;
    }
    if (g_mmemDC != nullptr) {
        DeleteDC(g_mmemDC);
        g_mmemDC = nullptr;
    }
    g_mbmpW = 0;
    g_mbmpH = 0;
}

bool MiniEnsureSurfaces(UINT w, UINT h) {
    if (g_mrt != nullptr && w == g_mbmpW && h == g_mbmpH) return true;
    MiniReleaseSurfaces();
    if (w == 0 || h == 0) return false;

    if (g_d2d == nullptr) {
        D2D1_FACTORY_OPTIONS opts = {};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                     __uuidof(ID2D1Factory), &opts,
                                     reinterpret_cast<void**>(g_d2d.GetAddressOf())))) {
            return false;
        }
    }

    HDC screen = GetDC(nullptr);
    g_mmemDC = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (g_mmemDC == nullptr) return false;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(w);
    bi.bmiHeader.biHeight = -static_cast<LONG>(h);   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    g_mbmp = CreateDIBSection(g_mmemDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (g_mbmp == nullptr || bits == nullptr) {
        MiniReleaseSurfaces();
        return false;
    }
    g_moldBmp = SelectObject(g_mmemDC, g_mbmp);

    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        0.0f, 0.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
    if (FAILED(g_d2d->CreateDCRenderTarget(&props, g_mrt.GetAddressOf()))) {
        MiniReleaseSurfaces();
        return false;
    }
    g_mrt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    g_mrt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_mbmpW = w;
    g_mbmpH = h;
    return true;
}

// Chip repaint: dark rounded body, "console", and a dot that turns amber while
// there are lines the user has not looked at yet.
void MiniPaint() {
    if (g_mini == nullptr || g_titleFmt == nullptr) return;
    if (!MiniEnsureSurfaces(static_cast<UINT>(kMiniW * g_scale + 0.5f),
                            static_cast<UINT>(kMiniH * g_scale + 0.5f))) {
        return;
    }
    const RECT bounds = {0, 0, static_cast<LONG>(g_mbmpW),
                         static_cast<LONG>(g_mbmpH)};
    if (FAILED(g_mrt->BindDC(g_mmemDC, &bounds))) return;

    const float w = static_cast<float>(g_mbmpW);
    const float h = static_cast<float>(g_mbmpH);
    const bool unread = (g_dirty != 0);

    ComPtr<ID2D1SolidColorBrush> body, fg, dot;
    if (FAILED(g_mrt->CreateSolidColorBrush(
            g_miniHover ? D2D1::ColorF(0.16f, 0.19f, 0.25f, 0.95f)
                        : D2D1::ColorF(0.06f, 0.07f, 0.10f, 0.88f),
            body.GetAddressOf())) ||
        FAILED(g_mrt->CreateSolidColorBrush(D2D1::ColorF(0.86f, 0.89f, 0.93f, 1.0f),
                                            fg.GetAddressOf())) ||
        FAILED(g_mrt->CreateSolidColorBrush(
            unread ? D2D1::ColorF(1.00f, 0.72f, 0.30f, 1.0f)
                   : D2D1::ColorF(0.45f, 0.50f, 0.58f, 1.0f),
            dot.GetAddressOf()))) {
        return;
    }

    g_mrt->BeginDraw();
    g_mrt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
        D2D1::RectF(0.0f, 0.0f, w, h), 5.0f * g_scale, 5.0f * g_scale);
    g_mrt->FillRoundedRectangle(rr, body.Get());
    g_mrt->DrawRoundedRectangle(rr, dot.Get(), 1.0f * g_scale);

    const D2D1_RECT_F tr = D2D1::RectF(10.0f * g_scale, 0.0f,
                                       w - 22.0f * g_scale, h);
    g_mrt->DrawText(kMiniText, static_cast<UINT32>(wcslen(kMiniText)),
                    g_titleFmt.Get(), &tr, fg.Get());
    g_mrt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(w - 13.0f * g_scale, h * 0.5f),
                                     3.0f * g_scale, 3.0f * g_scale),
                       dot.Get());

    if (FAILED(g_mrt->EndDraw())) {
        MiniReleaseSurfaces();
        return;
    }

    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    POINT src = {0, 0};
    SIZE size = {static_cast<LONG>(g_mbmpW), static_cast<LONG>(g_mbmpH)};
    UpdateLayeredWindow(g_mini, nullptr, nullptr, &size, g_mmemDC, &src, 0, &blend,
                        ULW_ALPHA);
}

void ShowMini(bool show) {
    if (g_mini == nullptr || !IsWindow(g_mini)) return;
    if (!show) {
        ShowWindow(g_mini, SW_HIDE);
        g_miniVisible = false;
        return;
    }
    RECT pr = {};
    if (g_panel != nullptr && IsWindow(g_panel) && GetWindowRect(g_panel, &pr)) {
        SetWindowPos(g_mini, HWND_TOP, pr.left, pr.top, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE);
    }
    ShowWindow(g_mini, SW_SHOWNOACTIVATE);
    g_miniVisible = true;
    MiniPaint();
}

void HidePanel() {
    if (g_panel != nullptr && IsWindow(g_panel)) {
        ShowWindow(g_panel, SW_HIDE);
    }
    g_foldHover = false;
    ShowMini(true);
}

void ShowPanelAgain() {
    ShowMini(false);
    if (g_panel != nullptr && IsWindow(g_panel)) {
        ShowWindow(g_panel, SW_SHOWNOACTIVATE);
        InterlockedExchange(&g_dirty, 1);
        Paint();
    }
}

LRESULT CALLBACK MiniProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN: {
        POINT cur = {};
        GetCursorPos(&cur);
        RECT wr = {};
        GetWindowRect(hwnd, &wr);
        g_miniGrab.x = cur.x - wr.left;
        g_miniGrab.y = cur.y - wr.top;
        g_miniDown = cur;
        g_miniDrag = true;
        SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_miniDrag) {
            POINT cur = {};
            GetCursorPos(&cur);
            SetWindowPos(hwnd, HWND_TOP, cur.x - g_miniGrab.x,
                         cur.y - g_miniGrab.y, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE);
            return 0;
        }
        if (!g_miniHover) {
            g_miniHover = true;
            TRACKMOUSEEVENT tme = {};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            MiniPaint();
        }
        return 0;
    case WM_MOUSELEAVE:
        if (g_miniHover) {
            g_miniHover = false;
            MiniPaint();
        }
        return 0;
    case WM_LBUTTONUP: {
        if (!g_miniDrag) return 0;
        g_miniDrag = false;
        ReleaseCapture();
        POINT cur = {};
        GetCursorPos(&cur);
        const int dx = cur.x - g_miniDown.x;
        const int dy = cur.y - g_miniDown.y;
        if (dx * dx + dy * dy < 16) {   // a click, not a drag
            ShowPanelAgain();
        }
        return 0;
    }
    case WM_RBUTTONUP:
        ShowPanelAgain();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));   // hand
            return TRUE;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        g_mini = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool CreateMini() {
    if (g_mini != nullptr && IsWindow(g_mini)) return true;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &MiniProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"ppluginConsoleMini";
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LogLine("console: mini class registration failed");
        return false;
    }

    g_mini = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"ppluginConsoleMini", L"", WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT,
        static_cast<int>(kMiniW * g_scale + 0.5f),
        static_cast<int>(kMiniH * g_scale + 0.5f), g_owner, nullptr, g_inst,
        nullptr);
    if (g_mini == nullptr) {
        LogLine("console: mini window create failed");
        return false;
    }
    ShowWindow(g_mini, SW_HIDE);
    return true;
}

void DestroyMini() {
    if (g_mini != nullptr) {
        DestroyWindow(g_mini);
        g_mini = nullptr;
    }
    MiniReleaseSurfaces();
    g_miniHover = false;
    g_miniDrag = false;
    g_miniVisible = false;
}

// -- window -----------------------------------------------------------------

bool RegisterPanelClass() {
    if (g_inst != nullptr) return true;

    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&RegisterPanelClass), &mod)) {
        return false;
    }
    g_inst = reinterpret_cast<HINSTANCE>(mod);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &PanelProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.lpszClassName = kPanelClass;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    return true;
}

bool CreatePanel() {
    HWND main = QueryWindow("mainWindow");
    HWND view = QueryWindow("viewWindow");
    if (main == nullptr && view != nullptr) {
        main = GetAncestor(view, GA_ROOT);
    }
    if (main == nullptr) return false;               // UI not ready yet, retry
    // A minimized owner reports a bogus client origin (-32000ish) and a zero
    // client rect; anchoring there would park the panel off screen for good.
    if (IsIconic(main)) return false;
    if (!RegisterPanelClass()) {
        LogLine("console: window class registration failed");
        return false;
    }

    g_scale = QueryScale();
    if (g_scale <= 0.0f) g_scale = 1.0f;
    g_winW = static_cast<int>(kPanelW * g_scale + 0.5f);
    g_winH = static_cast<int>(kTotalH * g_scale + 0.5f);   // title bar + log body

    // Anchor to the owner's client origin (screen px), then the panel is free
    // to be dragged anywhere on the desktop. Default corner: bottom right.
    RECT prc = {};
    POINT origin = {0, 0};
    GetClientRect(main, &prc);
    ClientToScreen(main, &origin);
    const int margin = static_cast<int>(kMargin * g_scale + 0.5f);
    int x = origin.x + prc.right - g_winW - margin;
    int y = origin.y + prc.bottom - g_winH - margin;

    g_panel = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kPanelClass, L"",
        WS_POPUP | WS_VISIBLE, x, y, g_winW, g_winH,
        main, nullptr, g_inst, nullptr);   // main is the owner, not the parent
    if (g_panel == nullptr) {
        LogLine("console: CreateWindowEx failed");
        return false;
    }

    g_owner = main;
    g_ownerOrigin = origin;
    g_ownerIconic = IsIconic(main) != 0;
    if (g_ownerIconic) {
        ShowWindow(g_panel, SW_HIDE);
    }

    CreateMini();   // created hidden; the title bar fold button swaps to it

    char line[160] = {};
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "console: panel window up (%dx%d at screen %d,%d, unclamped)",
                g_winW, g_winH, x, y);
    LogLine(line);
    InterlockedExchange(&g_dirty, 1);
    return true;
}

void DestroyPanel() {
    if (g_panel != nullptr) {
        DestroyWindow(g_panel);   // must run on the thread that created it
        g_panel = nullptr;
    }
    DestroyMini();
    ReleaseSurfaces();
    g_dragging = false;
    g_winW = 0;
    g_winH = 0;
    g_owner = nullptr;
    g_ownerIconic = false;
}

// -- module entry points -----------------------------------------------------

// One iteration of the panel loop. Must run on the pump thread: it creates the
// window, pumps the thread queue and repaints it.
void TickOnce() {
    if (g_panel == nullptr || g_owner == nullptr || !IsWindow(g_owner)) {
        DestroyPanel();
        CreatePanel();
        if (g_panel == nullptr) return;
    } else if (!IsWindow(g_panel)) {
        DestroyPanel();
        return;
    }

    // Pump the whole thread queue (panel + chip), not just the panel's messages.
    MSG msg = {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // A popup does not follow its owner by itself, and it must not float over
    // the desktop while the app is minimized (it is owned, so it would look
    // like the app is still up).
    const bool iconic = IsIconic(g_owner) != 0;
    if (iconic && !g_ownerIconic) {
        ShowWindow(g_panel, SW_HIDE);
        if (g_mini != nullptr) ShowWindow(g_mini, SW_HIDE);
        g_ownerIconic = true;
        return;
    }
    if (!iconic && g_ownerIconic) {
        // Restore whichever form was up: the chip means "still folded".
        if (g_miniVisible && g_mini != nullptr) {
            ShowWindow(g_mini, SW_SHOWNOACTIVATE);
        } else {
            ShowWindow(g_panel, SW_SHOWNOACTIVATE);
        }
        g_ownerIconic = false;
        InterlockedExchange(&g_dirty, 1);
    }
    if (iconic) return;

    // Keep the panel visually attached to the app window: apply the delta of
    // the owner's client origin. No clamping, so a panel parked outside the
    // app stays exactly where the user put it.
    POINT origin = {0, 0};
    RECT orc = {};
    if (GetClientRect(g_owner, &orc)) {
        ClientToScreen(g_owner, &origin);
        const int dx = origin.x - g_ownerOrigin.x;
        const int dy = origin.y - g_ownerOrigin.y;
        if ((dx != 0 || dy != 0) && !g_dragging && !g_miniDrag) {
            RECT wr = {};
            if (g_panel != nullptr && GetWindowRect(g_panel, &wr)) {
                SetWindowPos(g_panel, nullptr, wr.left + dx, wr.top + dy, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            if (g_mini != nullptr && GetWindowRect(g_mini, &wr)) {
                SetWindowPos(g_mini, nullptr, wr.left + dx, wr.top + dy, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        g_ownerOrigin = origin;
    }

    const bool due = (g_dirty != 0) ||
        (GetTickCount64() - g_lastPaint >= static_cast<ULONGLONG>(kIdleRepaintMs));
    if (due) {
        // While folded the chip repaints instead; g_dirty stays set so the dot
        // keeps flagging lines the user has not looked at yet.
        if (g_miniVisible) {
            MiniPaint();
        }
        if (IsWindowVisible(g_panel)) {
            Paint();
        }
    }
}

// 面板线程：窗口属于本线程，创建/泵消息/重绘全在这里做。
DWORD WINAPI PumpThread(LPVOID) {
    while (!g_stop) {
        TickOnce();
        Sleep(10);
    }
    DestroyPanel();   // DestroyWindow 必须在创建它的线程上跑
    return 0;
}

bool ModuleInit() {
    g_scale = QueryScale();
    if (g_scale <= 0.0f) g_scale = 1.0f;

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(g_dw.GetAddressOf())))) {
        LogLine("console: DWrite factory failed");
        return false;
    }
    if (FAILED(g_dw->CreateTextFormat(L"Consolas", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, kFontSize * g_scale, L"en-us",
            g_fmt.GetAddressOf()))) {
        LogLine("console: text format failed");
        g_dw.Reset();
        return false;
    }
    g_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g_fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    g_fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // Title bar uses its own, slightly smaller and bolder face.
    if (FAILED(g_dw->CreateTextFormat(L"Consolas", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, kFontSize * g_scale, L"en-us",
            g_titleFmt.GetAddressOf()))) {
        LogLine("console: title format failed");
        g_fmt.Reset();
        g_dw.Reset();
        return false;
    }
    g_titleFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g_titleFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g_titleFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // Register the log sink: proxy mirrors every line into our ring buffer.
    char sinkReq[128] = {};
    _snprintf_s(sinkReq, sizeof(sinkReq), _TRUNCATE,
                "{\"cmd\":\"logSink\",\"ptr\":\"%llu\"}",
                static_cast<unsigned long long>(
                    reinterpret_cast<uintptr_t>(&OnLogLine)));
    char sinkResp[256] = {};
    if (!GhostCall(sinkReq, sinkResp, sizeof(sinkResp))) {
        LogLine("console: log sink rejected");
        g_titleFmt.Reset();
        g_fmt.Reset();
        g_dw.Reset();
        return false;
    }

    // The panel window is created by this thread and so its queue is pumped
    // here; no other thread may touch it.
    g_stop = false;
    g_thread = CreateThread(nullptr, 0, PumpThread, nullptr, 0, nullptr);
    if (g_thread == nullptr) {
        LogLine("console: pump thread failed");
        g_titleFmt.Reset();
        g_fmt.Reset();
        g_dw.Reset();
        return false;
    }

    LogLine("console: attached, panel is its own window");
    return true;
}

void ModuleShutdown() {
    LogLine("console: detaching");

    g_stop = true;
    if (g_thread != nullptr) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }

    g_titleFmt.Reset();
    g_fmt.Reset();
    g_dw.Reset();
    g_titleHover = false;

    // Drop buffered lines so a reload starts clean.
    AcquireSRWLockExclusive(&g_ringLock);
    g_head = 0;
    g_tail = 0;
    ReleaseSRWLockExclusive(&g_ringLock);
    InterlockedExchange(&g_dirty, 1);
    g_lastPaint = 0;

    g_ready = false;
    g_ghost = nullptr;
    g_ghostCtx = nullptr;
}

}  // namespace

// The single export the host loader looks up. Contract is JSON only.
extern "C" __declspec(dllexport)
int __cdecl NotifyPlatform(const char* reqJson, char* respJson, int cap) {
    if (reqJson == nullptr || respJson == nullptr || cap <= 0) return -1;

    char cmd[32] = {};
    if (!JsonStr(reqJson, "cmd", cmd, sizeof(cmd))) {
        return WriteResp("{\"ok\":false,\"err\":\"bad json\"}", respJson, cap);
    }

    if (strcmp(cmd, "init") == 0) {
        char dispatch[32] = {};
        if (!JsonStr(reqJson, "dispatch", dispatch, sizeof(dispatch))) {
            return WriteResp("{\"ok\":false,\"err\":\"no dispatch\"}", respJson, cap);
        }
        g_ghost = reinterpret_cast<GhostFn>(
            static_cast<uintptr_t>(_strtoui64(dispatch, nullptr, 10)));
        char ctx[32] = {};
        g_ghostCtx = JsonStr(reqJson, "ctx", ctx, sizeof(ctx))
            ? reinterpret_cast<void*>(static_cast<uintptr_t>(_strtoui64(ctx, nullptr, 10)))
            : nullptr;
        g_ready = true;
        if (!ModuleInit()) {
            g_ready = false;
            g_ghost = nullptr;
            g_ghostCtx = nullptr;
            return WriteResp("{\"ok\":false,\"err\":\"init failed\"}", respJson, cap);
        }
        return WriteResp("{\"ok\":true,\"proto\":1}", respJson, cap);
    }

    if (strcmp(cmd, "shutdown") == 0) {
        ModuleShutdown();
        return WriteResp("{\"ok\":true}", respJson, cap);
    }

    return WriteResp("{\"ok\":false,\"err\":\"unknown cmd\"}", respJson, cap);
}

// Empty by design: every resource is created in init / the pump thread and
// released in shutdown.
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    (void)module;
    (void)reason;
    return TRUE;
}
