// overlay.inc - included by dllmain.cpp
// Present hook + overlay rendering inside the app's own swapchain.
//
// Rendering paths:
//   1) Direct2D (preferred): rounded rect + text
//   2) D3D11 ClearView fallback: solid rects, works on any D3D11 device
//
// Flip-model note: ANGLE uses a flip-model swapchain, so GetBuffer(0) rotates
// between several textures. RTV / D2D bitmap objects are therefore cached per
// backbuffer texture (up to 4 entries) and cleared on ResizeBuffers.
//
// Keep this file pure ASCII.

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <d2d1helper.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite_3.h>
#include <wrl/client.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <windowsx.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")

namespace ov {

using Microsoft::WRL::ComPtr;

static void LogFromOverlay(const char* text);
static void GetButtonRectPx(LONG* l, LONG* t, LONG* r, LONG* b);
static bool HitTestButton(int x, int y);

static HWND  g_hwnd = nullptr;
static float g_scale = 1.0f;
static bool  g_ready = false;
static bool  g_patched = false;
static bool  g_hwndMatched = false;
static bool  g_loggedOther = false;
static bool  g_d2dTried = false;
static bool  g_loggedD2dFail = false;
static bool  g_loggedFallback = false;
static int   g_hits = 0;

// Interaction state (touched from the window thread and the render thread).
static HWND   g_viewHwnd = nullptr;
static WNDPROC g_origViewProc = nullptr;
static volatile LONG g_hover = 0;
static volatile LONG g_pressed = 0;
static volatile LONG g_clicks = 0;

// Plugin pointer capture: set while a plugin owns the drag, plus the last
// logical position it saw, so CAPTURECHANGED can close the drag in place.
// Window thread only.
static bool  g_viewCapture = false;
static float g_lastLx = 0.0f;
static float g_lastLy = 0.0f;

// Present counter. The overlay only paints inside the app's Present, so this is
// the real "how often could a drag update be seen" number. One line per drag
// reports it, which is how we tell a paint-starvation stall from input lag.
static volatile LONG g_presents = 0;
static ULONGLONG g_dragStartMs = 0;
static LONG      g_dragStartFrames = 0;

// Button geometry in logical (96 dpi) units; overridden by overlay.ini.
// Defaults match the app's own top-bar pills as measured on a 150% display.
static float g_btnX = 1200.0f;
static float g_btnY = 9.0f;
static float g_btnW = 80.0f;
static float g_btnH = 30.0f;
static float g_fontSize = 15.0f;
static wchar_t g_btnText[64] = L"\x6D3B\x52A8";  // "activity" in Chinese
// Shown instead of g_btnText while there is no script file at all: without a
// script the button has nothing to run, and the pill should say so.
static wchar_t g_btnTextNoScript[64] = L"\x65E0\x811A\x672C";  // "no script"
static volatile LONG g_noScriptFile = 0;

// Poll interval of the script engine's visual gate (see "Visual gating").
// The old hard-coded two-step click chain (click_x / click2_* in overlay.ini)
// is gone: the overlay button only runs JSON scripts, so a click with no
// usable script does nothing.
static int   g_gatePollMs = 120;

// Look-and-feel sampled from the app's "code redeem" pill (top bar, right side):
// yellow #FFDC3B text and 1px outline on the transparent top-bar background,
// fully rounded corners. All overridable from overlay.ini.
static D2D1_COLOR_F g_textColor   = {1.000f, 0.863f, 0.231f, 1.0f};
static D2D1_COLOR_F g_borderColor = {1.000f, 0.863f, 0.231f, 1.0f};
static D2D1_COLOR_F g_bgColor     = {0.090f, 0.098f, 0.122f, 1.0f};
static float g_bgAlpha     = 0.0f;   // rest state; hover/press add on top
static float g_borderWidth = 1.0f;   // logical px, 0 disables the outline
static float g_radius      = 0.0f;   // logical px, 0 = pill (h/2)

// Font: the app ships NotoSansSC.ttf in its web assets, so the overlay can draw
// with the exact same face. Path is relative to the exe directory unless it is
// absolute or contains %ENV% references. Empty = system collection only.
static wchar_t g_fontFile[MAX_PATH] =
    L"data\\flutter_assets\\web\\fonts\\NotoSansSC.ttf";
static wchar_t g_fontFamily[128] = L"Noto Sans SC";
static int     g_fontWeight = 600;

using PresentFn  = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
using ResizeFn   = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT,
                                               DXGI_FORMAT, UINT);

static PresentFn  g_origPresent  = nullptr;
static Present1Fn g_origPresent1 = nullptr;
static ResizeFn   g_origResize   = nullptr;

// Shared device / context (owned by ANGLE).
static ComPtr<ID3D11Device>         g_dev;
static ComPtr<ID3D11DeviceContext>  g_ctx;
static ComPtr<ID3D11DeviceContext1> g_ctx1;

// D2D objects.
static ComPtr<ID2D1Factory1>        g_d2dFactory;
static ComPtr<ID2D1Device>          g_d2dDevice;
static ComPtr<ID2D1DeviceContext>   g_d2dCtx;
static ComPtr<ID2D1SolidColorBrush> g_brush;
static ComPtr<IDWriteFactory>       g_dw;
static ComPtr<IDWriteTextFormat>    g_textFmt;
static ComPtr<IDWriteFontCollection1> g_fontColl;  // keeps the private font set alive

// Per-backbuffer cache (flip model).
struct BufEntry {
    ComPtr<ID3D11Texture2D>        tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID2D1Bitmap1>           bmp;
    UINT w = 0;
    UINT h = 0;
};

static const int kMaxBufEntries = 4;
static BufEntry g_buffers[kMaxBufEntries];
static int g_bufferCount = 0;

static void ResetTargets() {
    for (int i = 0; i < kMaxBufEntries; ++i) {
        g_buffers[i].tex.Reset();
        g_buffers[i].rtv.Reset();
        g_buffers[i].bmp.Reset();
        g_buffers[i].w = 0;
        g_buffers[i].h = 0;
    }
    g_bufferCount = 0;
}

static bool EnsureDevice(IDXGISwapChain* sc) {
    if (g_dev) return true;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device),
                             reinterpret_cast<void**>(g_dev.GetAddressOf())))) {
        return false;
    }
    g_dev->GetImmediateContext(g_ctx.GetAddressOf());
    if (g_ctx) g_ctx.As(&g_ctx1);
    return true;
}

// ---------------------------------------------------------------------------
// Font setup
//
// The app bundles NotoSansSC.ttf under data\flutter_assets\web\fonts, so the
// overlay can render with the very same face as the surrounding UI. A private
// font set (DirectWrite 3) is built straight from the file: no system-wide
// install, no AddFontResourceEx ordering issues, and no dependency on the font
// being visible to the system collection.
// ---------------------------------------------------------------------------

static bool Utf8ToWide(const char* s, wchar_t* dst, int cap) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, nullptr, 0);
    UINT cp = CP_UTF8;
    if (n <= 0) {
        cp = CP_ACP;
        n = MultiByteToWideChar(cp, 0, s, -1, nullptr, 0);
    }
    if (n <= 0 || n > cap) return false;
    MultiByteToWideChar(cp, (cp == CP_UTF8) ? MB_ERR_INVALID_CHARS : 0, s, -1, dst, cap);
    return true;
}

// "#RRGGBB" or "#AARRGGBB"; the leading '#' is optional.
static bool ParseColor(const char* s, D2D1_COLOR_F* out) {
    while (*s == ' ' || *s == '\t') ++s;
    if (*s == '#') ++s;

    unsigned v = 0;
    int digits = 0;
    for (; *s && digits < 8; ++s) {
        const char c = *s;
        unsigned d;
        if (c >= '0' && c <= '9')      d = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
        else break;
        v = (v << 4) | d;
        ++digits;
    }

    if (digits == 6) {
        out->r = static_cast<float>((v >> 16) & 0xFF) / 255.0f;
        out->g = static_cast<float>((v >> 8) & 0xFF) / 255.0f;
        out->b = static_cast<float>(v & 0xFF) / 255.0f;
        out->a = 1.0f;
        return true;
    }
    if (digits == 8) {
        out->a = static_cast<float>((v >> 24) & 0xFF) / 255.0f;
        out->r = static_cast<float>((v >> 16) & 0xFF) / 255.0f;
        out->g = static_cast<float>((v >> 8) & 0xFF) / 255.0f;
        out->b = static_cast<float>(v & 0xFF) / 255.0f;
        return true;
    }
    return false;
}

// Makes g_fontFile absolute: expands %ENV% and resolves relative paths against
// the host exe directory (%APPDATA%\TTVoice), where the assets live.
static void ResolveFontPath() {
    if (!g_fontFile[0]) return;

    if (wcsstr(g_fontFile, L"%")) {
        wchar_t expanded[MAX_PATH] = {};
        if (ExpandEnvironmentStringsW(g_fontFile, expanded, MAX_PATH) > 0 && expanded[0]) {
            wcscpy_s(g_fontFile, expanded);
        }
    }

    const bool absolute = (g_fontFile[0] == L'\\') ||
                          (g_fontFile[1] == L':' && g_fontFile[2] == L'\\');
    if (absolute) return;

    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) return;
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) return;
    *slash = 0;

    wchar_t full[MAX_PATH] = {};
    swprintf_s(full, L"%s\\%s", exe, g_fontFile);
    wcscpy_s(g_fontFile, full);
}

static bool TryCreateTextFormat(IDWriteFontCollection* coll, const wchar_t* family) {
    if (!g_dw) return false;

    const int weight = (g_fontWeight >= 100 && g_fontWeight <= 950) ? g_fontWeight : 400;
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(g_dw->CreateTextFormat(family, coll,
                                      static_cast<DWRITE_FONT_WEIGHT>(weight),
                                      DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL,
                                      g_fontSize * g_scale, L"zh-cn",
                                      fmt.GetAddressOf()))) {
        return false;
    }
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    g_textFmt = fmt;
    return true;
}

// Builds the private font set from the app's own ttf. Returns false when the
// file is missing or DirectWrite 3 is unavailable, so the caller can fall back.
static bool InitFontFromFile() {
    if (!g_dw || !g_fontFile[0]) return false;

    if (GetFileAttributesW(g_fontFile) == INVALID_FILE_ATTRIBUTES) {
        char buf[360] = {};
        sprintf_s(buf, "font: file missing %ls (system fallback)", g_fontFile);
        LogFromOverlay(buf);
        return false;
    }

    ComPtr<IDWriteFactory5>            f5;
    ComPtr<IDWriteFontFile>            file;
    ComPtr<IDWriteFontSetBuilder1>     builder;
    ComPtr<IDWriteFontSet>             set;
    ComPtr<IDWriteFontCollection1>     coll;

    if (FAILED(g_dw.As(&f5)) ||
        FAILED(g_dw->CreateFontFileReference(g_fontFile, nullptr, file.GetAddressOf())) ||
        FAILED(f5->CreateFontSetBuilder(builder.GetAddressOf())) ||
        FAILED(builder->AddFontFile(file.Get())) ||
        FAILED(builder->CreateFontSet(set.GetAddressOf())) ||
        FAILED(f5->CreateFontCollectionFromFontSet(set.Get(), coll.GetAddressOf()))) {
        LogFromOverlay("font: private font set failed (dwrite 3 unavailable?)");
        return false;
    }

    bool ok = TryCreateTextFormat(coll.Get(), g_fontFamily);

    // The configured family name may not match the file; use its real name.
    if (!ok) {
        const UINT32 families = coll->GetFontFamilyCount();
        for (UINT32 i = 0; i < families && !ok; ++i) {
            ComPtr<IDWriteFontFamily> fam;
            if (FAILED(coll->GetFontFamily(i, fam.GetAddressOf()))) continue;
            ComPtr<IDWriteLocalizedStrings> names;
            if (FAILED(fam->GetFamilyNames(names.GetAddressOf()))) continue;
            wchar_t name[128] = {};
            if (names->GetCount() > 0 &&
                SUCCEEDED(names->GetString(0, name, 128)) && name[0]) {
                ok = TryCreateTextFormat(coll.Get(), name);
                if (ok) wcscpy_s(g_fontFamily, name);
            }
        }
    }

    if (!ok) {
        char buf[220] = {};
        sprintf_s(buf, "font: file loaded but no usable family (wanted %ls)", g_fontFamily);
        LogFromOverlay(buf);
        return false;
    }

    g_fontColl = coll;
    char buf[420] = {};
    sprintf_s(buf, "font: file=%ls family=%ls size=%.1fpx weight=%d",
              g_fontFile, g_fontFamily, g_fontSize * g_scale, g_fontWeight);
    LogFromOverlay(buf);
    return true;
}

static void InitTextFormat() {
    if (!g_dw) return;

    if (InitFontFromFile()) return;

    if (TryCreateTextFormat(nullptr, g_fontFamily)) {
        char buf[220] = {};
        sprintf_s(buf, "font: system collection family=%ls size=%.1fpx",
                  g_fontFamily, g_fontSize * g_scale);
        LogFromOverlay(buf);
        return;
    }
    if (TryCreateTextFormat(nullptr, L"Microsoft YaHei")) {
        LogFromOverlay("font: fallback Microsoft YaHei");
        return;
    }
    LogFromOverlay("font: no text format available");
}

// ---------------------------------------------------------------------------
// Script status -> pill label
//
// The overlay no longer draws a log panel: that lives in the console plugin's
// own window now (plugin/console). All the framework keeps is the one bit the
// button itself needs - "there is no usable script" - which switches the pill
// text to no_script_text. Details go to the log file only.
// ---------------------------------------------------------------------------

// Script state -> pill label, written by ScriptReload in script_engine.inc.
//   0 = a script loaded fine and owns the button
//   1 = no script file at all -> the pill reads no_script_text
//   2 = a script file was picked but rejected (bad json / empty events)
// Only code 1 touches the pill label; the reason for a rejected script goes to
// the log file, which the console plugin renders in its own window.
static void SetScriptStatus(int code, const char* reason) {
    if (code == 2 && reason != nullptr && reason[0] != 0) {
        char line[128] = {};
        sprintf_s(line, "script: unusable (%s)", reason);
        LogFromOverlay(line);
    }
    InterlockedExchange(&g_noScriptFile, (code == 1) ? 1 : 0);
}

static void InitD2D() {
    if (g_d2dTried) return;
    g_d2dTried = true;
    if (!g_dev) return;

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 __uuidof(ID2D1Factory1), nullptr,
                                 reinterpret_cast<void**>(g_d2dFactory.GetAddressOf())))) {
        if (!g_loggedD2dFail) {
            g_loggedD2dFail = true;
            LogFromOverlay("d2d: D2D1CreateFactory failed");
        }
        return;
    }

    ComPtr<IDXGIDevice> dxgiDev;
    if (FAILED(g_dev.As(&dxgiDev))) return;

    const HRESULT hr = g_d2dFactory->CreateDevice(dxgiDev.Get(), g_d2dDevice.GetAddressOf());
    if (FAILED(hr)) {
        if (!g_loggedD2dFail) {
            g_loggedD2dFail = true;
            char buf[160] = {};
            sprintf_s(buf, "d2d: CreateDevice failed hr=0x%08X fl=0x%X (d3d11 fallback)",
                      static_cast<unsigned>(hr),
                      static_cast<unsigned>(g_dev->GetFeatureLevel()));
            LogFromOverlay(buf);
        }
        g_d2dDevice.Reset();
        return;
    }

    if (FAILED(g_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                                g_d2dCtx.GetAddressOf()))) {
        g_d2dCtx.Reset();
        return;
    }

    if (FAILED(g_d2dCtx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1),
                                               g_brush.GetAddressOf()))) {
        g_d2dCtx.Reset();
        return;
    }

    if (SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                      reinterpret_cast<IUnknown**>(g_dw.GetAddressOf())))) {
        InitTextFormat();
    }
}

// Find (or create) the cache entry for the current backbuffer.
static BufEntry* ObtainBuffer(IDXGISwapChain* sc) {
    if (!g_dev) return nullptr;

    ComPtr<ID3D11Texture2D> back;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D),
                             reinterpret_cast<void**>(back.GetAddressOf())))) {
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC td = {};
    back->GetDesc(&td);

    for (int i = 0; i < g_bufferCount; ++i) {
        if (g_buffers[i].tex.Get() == back.Get() &&
            g_buffers[i].w == td.Width && g_buffers[i].h == td.Height) {
            return &g_buffers[i];
        }
    }

    BufEntry* e = nullptr;
    if (g_bufferCount < kMaxBufEntries) {
        e = &g_buffers[g_bufferCount++];
    } else {
        e = &g_buffers[0];
        e->rtv.Reset();
        e->bmp.Reset();
    }

    e->tex = back;
    e->w = td.Width;
    e->h = td.Height;

    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = td.Format;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(g_dev->CreateRenderTargetView(back.Get(), &rd, e->rtv.GetAddressOf()))) {
        e->tex.Reset();
        return nullptr;
    }

    if (g_d2dCtx && td.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        ComPtr<IDXGISurface> surf;
        if (SUCCEEDED(back.As(&surf))) {
            const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            g_d2dCtx->CreateBitmapFromDxgiSurface(surf.Get(), &props, e->bmp.GetAddressOf());
        }
    }
    return e;
}

static bool DrawD2D(IDXGISwapChain* sc) {
    if (!g_d2dCtx) return false;

    BufEntry* e = ObtainBuffer(sc);
    if (!e || !e->bmp) return false;

    LONG bl = 0, bt = 0, br = 0, bb = 0;
    GetButtonRectPx(&bl, &bt, &br, &bb);

    const bool hover = g_hover != 0;
    const bool press = g_pressed != 0;

    g_d2dCtx->SetTarget(e->bmp.Get());
    g_d2dCtx->BeginDraw();
    g_d2dCtx->SetTransform(D2D1::Matrix3x2F::Identity());

    const D2D1_RECT_F r = D2D1::RectF(static_cast<float>(bl), static_cast<float>(bt),
                                       static_cast<float>(br), static_cast<float>(bb));
    const float radius = (g_radius > 0.0f ? g_radius : g_btnH * 0.5f) * g_scale;
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, radius, radius);

    // Rest state keeps the pill transparent so the app's own top bar shows
    // through; hover/press add a faint white wash as feedback.
    if (g_bgAlpha > 0.0f) {
        D2D1_COLOR_F bg = g_bgColor;
        bg.a = (g_bgAlpha > 1.0f) ? 1.0f : g_bgAlpha;
        g_brush->SetColor(bg);
        g_d2dCtx->FillRoundedRectangle(rr, g_brush.Get());
    }

    const float highlight = press ? 0.14f : (hover ? 0.07f : 0.0f);
    if (highlight > 0.0f) {
        g_brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, highlight));
        g_d2dCtx->FillRoundedRectangle(rr, g_brush.Get());
    }

    if (g_borderWidth > 0.0f) {
        D2D1_COLOR_F border = g_borderColor;
        if (press) {
            border.r *= 0.82f;
            border.g *= 0.82f;
            border.b *= 0.82f;
        }
        g_brush->SetColor(border);
        g_d2dCtx->DrawRoundedRectangle(rr, g_brush.Get(), g_borderWidth * g_scale);
    }

    if (g_textFmt) {
        g_brush->SetColor(g_textColor);
        const wchar_t* label = g_noScriptFile ? g_btnTextNoScript : g_btnText;
        g_d2dCtx->DrawText(label, static_cast<UINT32>(wcslen(label)),
                           g_textFmt.Get(), &r, g_brush.Get());
    }

    // M1 seam: dispatch plugin draws on the shared D2D context. Plugins draw in
    // logical pixels, so scale the transform and pass logical width/height; the
    // overlay itself keeps only the pill now - the log panel is the console
    // plugin's own window (gray release).
    if (g_scale > 0.0f) {
        g_d2dCtx->SetTransform(D2D1::Matrix3x2F::Scale(g_scale, g_scale));
        plugins::RenderFrame(g_d2dCtx.Get(), static_cast<float>(e->w) / g_scale,
                             static_cast<float>(e->h) / g_scale);
        g_d2dCtx->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    if (FAILED(g_d2dCtx->EndDraw())) {
        ResetTargets();
        return false;
    }
    return true;
}

static void DrawFallback(IDXGISwapChain* sc) {
    if (!g_ctx1) return;

    BufEntry* e = ObtainBuffer(sc);
    if (!e || !e->rtv) return;

    if (!g_loggedFallback) {
        g_loggedFallback = true;
        LogFromOverlay("overlay: using d3d11 fallback (no text)");
    }

    const bool press = g_pressed != 0;
    const float bodyLight[4]   = {g_bgColor.r + 0.08f, g_bgColor.g + 0.08f,
                                  g_bgColor.b + 0.08f, 1.0f};
    const float bodyDark[4]    = {g_bgColor.r, g_bgColor.g, g_bgColor.b, 1.0f};
    const float accent[4]      = {g_borderColor.r, g_borderColor.g,
                                  g_borderColor.b, 1.0f};

    D3D11_RECT bodyRect = {};
    LONG bl = 0, bt = 0, br = 0, bb = 0;
    GetButtonRectPx(&bl, &bt, &br, &bb);
    bodyRect.left = bl;
    bodyRect.top = bt;
    bodyRect.right = br;
    bodyRect.bottom = bb;
    g_ctx1->ClearView(e->rtv.Get(), press ? bodyLight : bodyDark, &bodyRect, 1);

    D3D11_RECT barRect = bodyRect;
    barRect.bottom = barRect.top + static_cast<LONG>(6.0f * g_scale);
    g_ctx1->ClearView(e->rtv.Get(), accent, &barRect, 1);
}

static void Draw(IDXGISwapChain* sc) {
    if (!g_ready || !EnsureDevice(sc)) return;

    if (!g_d2dTried) InitD2D();

    if (DrawD2D(sc)) return;
    DrawFallback(sc);
}

static bool IsRelatedWindow(HWND w) {
    if (!w || !g_hwnd) return false;
    if (w == g_hwnd) return true;
    if (GetAncestor(w, GA_ROOT) == g_hwnd) return true;   // child view window
    if (GetWindow(w, GW_OWNER) == g_hwnd) return true;    // owned popup
    return false;
}

static void GetButtonRectPx(LONG* l, LONG* t, LONG* r, LONG* b) {
    *l = static_cast<LONG>(g_btnX * g_scale);
    *t = static_cast<LONG>(g_btnY * g_scale);
    *r = static_cast<LONG>((g_btnX + g_btnW) * g_scale);
    *b = static_cast<LONG>((g_btnY + g_btnH) * g_scale);
}

static bool HitTestButton(int x, int y) {
    LONG l = 0, t = 0, r = 0, b = 0;
    GetButtonRectPx(&l, &t, &r, &b);
    return x >= l && x < r && y >= t && y < b;
}

// ---------------------------------------------------------------------------
// Visual gating (used by the script engine's wait_px)
//
// UI elements that only appear after the previous click has navigated cannot be
// hit on a fixed delay: they show up anywhere from a few hundred ms to several
// seconds later. So the script engine captures the view with
// PrintWindow(PW_RENDERFULLCONTENT) - the same path the coordinates were
// measured with - and proceeds only after the target's cream fill shows up in
// three polls in a row (~240 ms of stability), so a fade-in / slide-in frame
// cannot trigger it and the element has stopped moving before it is hit.
// ---------------------------------------------------------------------------

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

struct ViewCapture {
    HDC     dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ oldBmp = nullptr;
    BYTE*   bits = nullptr;
    int     w = 0;
    int     h = 0;
};

static ViewCapture     g_capture;      // only touched by the script run thread

// Grabs the current view content into a top-down 32bpp DIB, reusing the
// bitmap until the client size changes.
static bool CaptureView() {
    HWND view = g_viewHwnd;
    if (!view) return false;

    RECT rc = {};
    if (!GetClientRect(view, &rc)) return false;
    const int w = static_cast<int>(rc.right - rc.left);
    const int h = static_cast<int>(rc.bottom - rc.top);
    if (w <= 0 || h <= 0) return false;

    if (g_capture.w != w || g_capture.h != h) {
        if (g_capture.bmp) {
            SelectObject(g_capture.dc, g_capture.oldBmp);
            DeleteObject(g_capture.bmp);
            g_capture.bmp = nullptr;
            g_capture.bits = nullptr;
        }
        if (!g_capture.dc) g_capture.dc = CreateCompatibleDC(nullptr);
        if (!g_capture.dc) return false;

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;   // top-down: row 0 is the top row
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        g_capture.bmp = CreateDIBSection(g_capture.dc, &bi, DIB_RGB_COLORS, &bits,
                                         nullptr, 0);
        if (!g_capture.bmp || !bits) {
            if (g_capture.bmp) {
                DeleteObject(g_capture.bmp);
                g_capture.bmp = nullptr;
            }
            g_capture.bits = nullptr;
            return false;
        }
        g_capture.bits = static_cast<BYTE*>(bits);
        g_capture.oldBmp = SelectObject(g_capture.dc, g_capture.bmp);
        g_capture.w = w;
        g_capture.h = h;
    }

    return PrintWindow(view, g_capture.dc, PW_RENDERFULLCONTENT) != FALSE;
}

// Counts the pill's cream fill (R>228, G>210, B<228, R-B>22 - the filter the
// points were measured with) in a band around (lx,ly), and reports the centroid
// of the matches so the log shows where the pill really is.
static int CountCreamBand(float lx, float ly, LONG* outX, LONG* outY) {
    if (!g_capture.bits) return 0;

    LONG x0 = static_cast<LONG>(lx * g_scale) - 200;  // physical px;
    LONG y0 = static_cast<LONG>(ly * g_scale) - 40;   // pill ~123x54
    LONG x1 = x0 + 400;
    LONG y1 = y0 + 80;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_capture.w - 1) x1 = g_capture.w - 1;
    if (y1 > g_capture.h - 1) y1 = g_capture.h - 1;
    if (x0 > x1 || y0 > y1) return 0;

    long long sumX = 0;
    long long sumY = 0;
    int hits = 0;

    for (LONG y = y0; y <= y1; ++y) {
        const BYTE* row = g_capture.bits +
                          static_cast<size_t>(y) * static_cast<size_t>(g_capture.w) * 4;
        for (LONG x = x0; x <= x1; ++x) {
            const BYTE* p = row + static_cast<size_t>(x) * 4;  // B,G,R,A
            const int b = p[0];
            const int g = p[1];
            const int r = p[2];
            if (r > 228 && g > 210 && b < 228 && (r - b) > 22) {
                sumX += x;
                sumY += y;
                ++hits;
            }
        }
    }

    if (hits > 0) {
        if (outX) *outX = static_cast<LONG>(sumX / hits);
        if (outY) *outY = static_cast<LONG>(sumY / hits);
    }
    return hits;
}

// Click scripts (tools/script_editor.py -> JSON -> this). Included here because
// it reuses CaptureView / CountCreamBand above.
#include "script_engine.inc"

// The overlay button has exactly one job: run the JSON script. ScriptTryRun
// reloads it when the file changed and runs it on its own thread, so the window
// thread never blocks on a capture.
//
// There is no hard-coded fallback any more. When nothing can be run, verbose=true
// makes ScriptReload log the reason on **every** click (no json found / parse
// failed ...), plus the pinned status line and the "no script" pill label - so
// no extra line is needed here.
static void OnButtonClick() {
    (void)ScriptTryRun(true);
}

// Runs on the window's own thread. Consumed here: the overlay pill, plugin
// regions (clicks) and plugin pointer captures (hover/drag). Everything else is
// forwarded to Flutter untouched.
static LRESULT CALLBACK ViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        const LONG inside = HitTestButton(x, y) ? 1 : 0;
        if (inside && !g_hover) {
            TRACKMOUSEEVENT tme = {};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
        }
        if (inside != g_hover) g_hover = inside;
        if (g_scale > 0.0f) {
            const float lx = static_cast<float>(x) / g_scale;
            const float ly = static_cast<float>(y) / g_scale;
            g_lastLx = lx;
            g_lastLy = ly;
            // Drag in progress: the plugin consumes the move, but the app still
            // gets it. Flutter only paints when it schedules a frame, and the
            // overlay can only repaint inside Present, so feeding motion back
            // is what keeps the panel tracking the cursor. DOWN/UP stay
            // swallowed, so the app never starts a gesture of its own.
            if (plugins::DispatchPointer(TTPLUGIN_POINTER_MOVE, lx, ly)) {
                break;
            }
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        if (HitTestButton(x, y)) {
            g_pressed = 1;
            return 0;
        }
        if (g_scale > 0.0f) {
            const float lx = static_cast<float>(x) / g_scale;
            const float ly = static_cast<float>(y) / g_scale;
            g_lastLx = lx;
            g_lastLy = ly;
            if (plugins::DispatchPointer(TTPLUGIN_POINTER_DOWN, lx, ly)) {
                // The plugin owns the pointer until UP; capture keeps MOVE
                // coming while the cursor leaves the window.
                SetCapture(hwnd);
                g_viewCapture = true;
                g_dragStartMs = GetTickCount64();
                g_dragStartFrames = g_presents;
                return 0;
            }
        }
        break;
    }
    case WM_LBUTTONUP: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        const LONG was = g_pressed;
        g_pressed = 0;
        if (was && HitTestButton(x, y)) {
            InterlockedIncrement(&g_clicks);
            LogFromOverlay("overlay: button clicked");
            OnButtonClick();
            return 0;
        }
        if (g_scale > 0.0f) {
            const float lx = static_cast<float>(x) / g_scale;
            const float ly = static_cast<float>(y) / g_scale;
            g_lastLx = lx;
            g_lastLy = ly;
            // Release order matters: dispatch UP first (that clears the plugin
            // capture), then hand the mouse back so CAPTURECHANGED sees no drag.
            if (plugins::DispatchPointer(TTPLUGIN_POINTER_UP, lx, ly)) {
                if (g_viewCapture) {
                    g_viewCapture = false;
                    ReleaseCapture();
                }
                // Diagnostics: how many overlay frames the drag could actually
                // be painted on. A long drag with 0-2 frames means the panel is
                // waiting for the app to present, not for the mouse.
                char line[160] = {};
                _snprintf_s(line, sizeof(line), _TRUNCATE,
                            "overlay: drag %llu ms, %ld overlay frame(s)",
                            static_cast<unsigned long long>(GetTickCount64() - g_dragStartMs),
                            g_presents - g_dragStartFrames);
                LogFromOverlay(line);
                return 0;
            }
        }
        // M1 seam: clicks the overlay pill did not take are offered to plugin
        // regions (logical px). A region that claims the point consumes it;
        // otherwise the event keeps flowing to Flutter unchanged.
        if (g_scale > 0.0f &&
            plugins::DispatchClick(static_cast<float>(x) / g_scale,
                                   static_cast<float>(y) / g_scale, 0)) {
            return 0;
        }
        if (was) return 0;  // pressed in the pill, released outside: swallow as before
        break;
    }
    case WM_CAPTURECHANGED:
        if (g_viewCapture) {
            g_viewCapture = false;
            // Alt-tab or another window grabbed the mouse mid drag: close the
            // plugin's drag at the last known point instead of leaving it latched.
            plugins::CancelPointerCapture(g_lastLx, g_lastLy);
        }
        break;
    case WM_MOUSELEAVE:
        g_hover = 0;
        g_pressed = 0;
        break;
    default:
        break;
    }
    return g_origViewProc ? CallWindowProcW(g_origViewProc, hwnd, msg, wp, lp)
                          : DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnsureViewSubclass(HWND view) {
    if (!view || view == g_viewHwnd) return;

    WNDPROC prev = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(view, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ViewProc)));
    if (!prev) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LogFromOverlay("overlay: SetWindowLongPtr failed, no click handling");
        }
        return;
    }
    g_origViewProc = prev;
    g_viewHwnd = view;
    LogFromOverlay("overlay: view subclassed");
}

static bool IsTargetSwapChain(IDXGISwapChain* sc, HWND* outWindow) {
    if (!g_hwnd) return false;

    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(sc->GetDesc(&desc))) return false;

    if (IsRelatedWindow(desc.OutputWindow)) {
        g_hwndMatched = true;
        if (outWindow) *outWindow = desc.OutputWindow;
        return true;
    }

    if (!g_loggedOther) {
        g_loggedOther = true;
        wchar_t cls[64] = {};
        if (desc.OutputWindow) GetClassNameW(desc.OutputWindow, cls, 64);
        char buf[200] = {};
        sprintf_s(buf, "first swapchain: out=%p cls=%ls w=%u h=%u fmt=%d",
                  reinterpret_cast<void*>(desc.OutputWindow), cls,
                  desc.BufferDesc.Width, desc.BufferDesc.Height,
                  static_cast<int>(desc.BufferDesc.Format));
        LogFromOverlay(buf);
    }

    if (desc.OutputWindow == nullptr && !g_hwndMatched) {
        RECT rc = {};
        if (GetClientRect(g_hwnd, &rc)) {
            const UINT w = static_cast<UINT>(rc.right - rc.left);
            const UINT h = static_cast<UINT>(rc.bottom - rc.top);
            if (desc.BufferDesc.Width == w && desc.BufferDesc.Height == h) {
                if (outWindow) *outWindow = nullptr;
                return true;
            }
        }
    }
    return false;
}

static void OnPresent(IDXGISwapChain* sc) {
    if (!g_ready) return;

    HWND view = nullptr;
    if (!IsTargetSwapChain(sc, &view)) return;
    InterlockedIncrement(&g_presents);
    if (g_hits++ == 0) LogFromOverlay("present hook hit");
    if (view) EnsureViewSubclass(view);
    Draw(sc);
}

static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    OnPresent(sc);
    return g_origPresent(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                              const DXGI_PRESENT_PARAMETERS* params) {
    OnPresent(sc);
    return g_origPresent1(sc, sync, flags, params);
}

static HRESULT STDMETHODCALLTYPE HookResize(IDXGISwapChain* sc, UINT count, UINT w, UINT h,
                                            DXGI_FORMAT fmt, UINT flags) {
    ResetTargets();
    return g_origResize(sc, count, w, h, fmt, flags);
}

static bool PatchVTable(IDXGISwapChain* sc) {
    void** vt = *reinterpret_cast<void***>(sc);
    if (!vt) return false;

    DWORD old = 0;
    if (!VirtualProtect(vt, 29 * sizeof(void*), PAGE_READWRITE, &old)) return false;

    g_origPresent = reinterpret_cast<PresentFn>(vt[8]);
    vt[8] = reinterpret_cast<void*>(&HookPresent);

    g_origResize = reinterpret_cast<ResizeFn>(vt[13]);
    vt[13] = reinterpret_cast<void*>(&HookResize);

    g_origPresent1 = reinterpret_cast<Present1Fn>(vt[22]);
    vt[22] = reinterpret_cast<void*>(&HookPresent1);

    VirtualProtect(vt, 29 * sizeof(void*), old, &old);
    return true;
}

// Create a throwaway swapchain just to obtain the shared vtable, then patch it.
static bool InstallHooks() {
    if (g_patched) return true;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TTProxyDummyWnd";
    RegisterClassExW(&wc);

    HWND dummy = CreateWindowExW(0, L"TTProxyDummyWnd", L"", WS_POPUP,
                                 0, 0, 4, 4, nullptr, nullptr, wc.hInstance, nullptr);
    if (!dummy) return false;

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 4;
    sd.BufferDesc.Height = 4;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = dummy;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;

    const D3D_DRIVER_TYPE driverTypes[] = {D3D_DRIVER_TYPE_HARDWARE,
                                           D3D_DRIVER_TYPE_WARP};
    HRESULT hr = E_FAIL;
    for (const D3D_DRIVER_TYPE type : driverTypes) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, nullptr, 0,
                                           D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
        if (SUCCEEDED(hr) && sc != nullptr) break;
        if (sc) { sc->Release(); sc = nullptr; }
        if (ctx) { ctx->Release(); ctx = nullptr; }
        if (dev) { dev->Release(); dev = nullptr; }
    }

    bool ok = false;
    if (SUCCEEDED(hr) && sc) ok = PatchVTable(sc);

    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(dummy);

    g_patched = ok;
    if (!ok) LogFromOverlay("hook: PatchVTable failed");
    return ok;
}

// ---------------------------------------------------------------------------
// D3D11 device creation hook
//
// ANGLE creates its D3D11 device without D3D11_CREATE_DEVICE_BGRA_SUPPORT,
// which makes ID2D1Factory1::CreateDevice fail with E_INVALIDARG. Patch the
// import tables of loaded modules so the flag is always added.
// ---------------------------------------------------------------------------

using PFN_CreateDevice = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                          const D3D_FEATURE_LEVEL*, UINT, UINT,
                                          ID3D11Device**, D3D_FEATURE_LEVEL*,
                                          ID3D11DeviceContext**);
using PFN_CreateDeviceSwap = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                              const D3D_FEATURE_LEVEL*, UINT, UINT,
                                              const DXGI_SWAP_CHAIN_DESC*,
                                              IDXGISwapChain**, ID3D11Device**,
                                              D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

static PFN_CreateDevice     g_realCreateDevice = nullptr;
static PFN_CreateDeviceSwap g_realCreateSwap   = nullptr;
static bool g_loggedCreateFlags = false;
static HMODULE g_d3d11Module = nullptr;

using PFN_GetProcAddress = FARPROC(WINAPI*)(HMODULE, LPCSTR);
static PFN_GetProcAddress g_realGetProcAddress = nullptr;
static bool g_loggedGpaHook = false;

static HRESULT WINAPI HookCreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type,
                                       HMODULE software, UINT flags,
                                       const D3D_FEATURE_LEVEL* levels, UINT levelCount,
                                       UINT sdk, ID3D11Device** device,
                                       D3D_FEATURE_LEVEL* outLevel,
                                       ID3D11DeviceContext** context) {
    if (!g_loggedCreateFlags) {
        g_loggedCreateFlags = true;
        char buf[120] = {};
        sprintf_s(buf, "d3d11: CreateDevice flags=0x%X -> add BGRA", flags);
        LogFromOverlay(buf);
    }
    return g_realCreateDevice(adapter, type, software,
                              flags | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                              levels, levelCount, sdk, device, outLevel, context);
}

static HRESULT WINAPI HookCreateDeviceSwap(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type,
                                           HMODULE software, UINT flags,
                                           const D3D_FEATURE_LEVEL* levels, UINT levelCount,
                                           UINT sdk, const DXGI_SWAP_CHAIN_DESC* desc,
                                           IDXGISwapChain** swapChain, ID3D11Device** device,
                                           D3D_FEATURE_LEVEL* outLevel,
                                           ID3D11DeviceContext** context) {
    if (!g_loggedCreateFlags) {
        g_loggedCreateFlags = true;
        char buf[120] = {};
        sprintf_s(buf, "d3d11: CreateDeviceAndSwapChain flags=0x%X -> add BGRA", flags);
        LogFromOverlay(buf);
    }
    return g_realCreateSwap(adapter, type, software,
                            flags | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                            levels, levelCount, sdk, desc, swapChain, device,
                            outLevel, context);
}

// ANGLE loads d3d11.dll dynamically and resolves the entry points through
// GetProcAddress, so the IAT of libGLESv2 never references them directly.
// Intercept GetProcAddress instead and hand out our wrappers.
static FARPROC WINAPI HookGetProcAddress(HMODULE mod, LPCSTR name) {
    FARPROC fn = g_realGetProcAddress ? g_realGetProcAddress(mod, name) : nullptr;
    if (fn && mod == g_d3d11Module && name != nullptr &&
        (reinterpret_cast<ULONG_PTR>(name) >> 16) != 0) {
        if (strcmp(name, "D3D11CreateDevice") == 0) {
            if (!g_loggedGpaHook) {
                g_loggedGpaHook = true;
                LogFromOverlay("gpa: D3D11CreateDevice intercepted");
            }
            return reinterpret_cast<FARPROC>(&HookCreateDevice);
        }
        if (strcmp(name, "D3D11CreateDeviceAndSwapChain") == 0) {
            if (!g_loggedGpaHook) {
                g_loggedGpaHook = true;
                LogFromOverlay("gpa: D3D11CreateDeviceAndSwapChain intercepted");
            }
            return reinterpret_cast<FARPROC>(&HookCreateDeviceSwap);
        }
    }
    return fn;
}


static void PatchIatForModule(HMODULE mod) {
    if (!mod) return;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<BYTE*>(mod) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) return;

    auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        reinterpret_cast<BYTE*>(mod) + dir.VirtualAddress);

    for (; imp->Name != 0; ++imp) {
        const char* dllName = reinterpret_cast<const char*>(
            reinterpret_cast<BYTE*>(mod) + imp->Name);
        const bool isD3D11 = _stricmp(dllName, "d3d11.dll") == 0;

        auto* oft = reinterpret_cast<IMAGE_THUNK_DATA*>(
            reinterpret_cast<BYTE*>(mod) +
            (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* ft = reinterpret_cast<IMAGE_THUNK_DATA*>(
            reinterpret_cast<BYTE*>(mod) + imp->FirstThunk);

        for (int i = 0; oft[i].u1.AddressOfData != 0; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL(oft[i].u1.Ordinal)) continue;

            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                reinterpret_cast<BYTE*>(mod) + oft[i].u1.AddressOfData);
            const char* fnName = reinterpret_cast<const char*>(byName->Name);

            void** slot = reinterpret_cast<void**>(&ft[i].u1.Function);
            void* replacement = nullptr;

            if (isD3D11 && _stricmp(fnName, "D3D11CreateDevice") == 0 &&
                *slot == reinterpret_cast<void*>(g_realCreateDevice)) {
                replacement = reinterpret_cast<void*>(&HookCreateDevice);
            } else if (isD3D11 && _stricmp(fnName, "D3D11CreateDeviceAndSwapChain") == 0 &&
                       *slot == reinterpret_cast<void*>(g_realCreateSwap)) {
                replacement = reinterpret_cast<void*>(&HookCreateDeviceSwap);
            } else if (_stricmp(fnName, "GetProcAddress") == 0 && g_realGetProcAddress &&
                       *slot == reinterpret_cast<void*>(g_realGetProcAddress)) {
                replacement = reinterpret_cast<void*>(&HookGetProcAddress);
            }

            if (!replacement) continue;
            if (*slot == replacement) continue;  // already patched


            DWORD old = 0;
            if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
                *slot = replacement;
                VirtualProtect(slot, sizeof(void*), old, &old);
            }
        }
    }
}

static void PatchAllD3D11Iat() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;

    MODULEENTRY32W me = {};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            PatchIatForModule(me.hModule);
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
}

// Patch the IAT of every module as it loads (runs under loader lock, keep it
// allocation-free and log-free).
struct TT_LDR_DLL_LOADED_NOTIFICATION_DATA {
    ULONG Flags;
    const UNICODE_STRING* FullDllName;
    const UNICODE_STRING* BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
};

using PLDR_DLL_NOTIFICATION_FUNCTION = VOID(CALLBACK*)(ULONG,
                                                       const TT_LDR_DLL_LOADED_NOTIFICATION_DATA*,
                                                       PVOID);

static VOID CALLBACK OnDllNotification(ULONG reason,
                                       const TT_LDR_DLL_LOADED_NOTIFICATION_DATA* data,
                                       PVOID) {
    if (reason == 1 && data && data->DllBase) {  // DLL notification: loaded
        PatchIatForModule(reinterpret_cast<HMODULE>(data->DllBase));
    }
}

static void RegisterDllNotifications() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;

    using PFN_Register = NTSTATUS(NTAPI*)(ULONG, PLDR_DLL_NOTIFICATION_FUNCTION, PVOID, PVOID*);
    auto reg = reinterpret_cast<PFN_Register>(
        GetProcAddress(ntdll, "LdrRegisterDllNotification"));
    if (!reg) return;

    PVOID cookie = nullptr;
    reg(0, OnDllNotification, nullptr, &cookie);
}

static DWORD WINAPI D3D11HookThread(LPVOID) {
    wchar_t sysdir[MAX_PATH] = {};
    if (GetSystemDirectoryW(sysdir, MAX_PATH) == 0) return 0;

    wchar_t path[MAX_PATH] = {};
    swprintf_s(path, L"%s\\d3d11.dll", sysdir);
    HMODULE lib = LoadLibraryW(path);
    if (!lib) {
        LogFromOverlay("d3d11: LoadLibrary failed");
        return 0;
    }
    g_d3d11Module = lib;

    g_realCreateDevice = reinterpret_cast<PFN_CreateDevice>(
        GetProcAddress(lib, "D3D11CreateDevice"));
    g_realCreateSwap = reinterpret_cast<PFN_CreateDeviceSwap>(
        GetProcAddress(lib, "D3D11CreateDeviceAndSwapChain"));
    if (!g_realCreateDevice && !g_realCreateSwap) {
        LogFromOverlay("d3d11: exports not found");
        return 0;
    }

    g_realGetProcAddress = reinterpret_cast<PFN_GetProcAddress>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcAddress"));

    RegisterDllNotifications();
    PatchAllD3D11Iat();

    // Safety net for modules whose imports are resolved later.
    for (int i = 0; i < 100; ++i) {  // ~10s
        Sleep(100);
        PatchAllD3D11Iat();
    }
    return 0;
}

static bool g_d3d11HookStarted = false;

static void StartD3D11Hook() {
    if (g_d3d11HookStarted) return;
    g_d3d11HookStarted = true;
    HANDLE th = CreateThread(nullptr, 0, D3D11HookThread, nullptr, 0, nullptr);
    if (th) CloseHandle(th);
}

// Reads %APPDATA%\TTVoice\overlay.ini (logical units). Creates a template on
// first run so the button can be repositioned without recompiling.
static void LoadConfig() {
    wchar_t appdata[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;

    wchar_t path[MAX_PATH] = {};
    swprintf_s(path, L"%s\\TTVoice\\overlay.ini", appdata);

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path, L"rb") != 0 || fp == nullptr) {
        if (_wfopen_s(&fp, path, L"wb") == 0 && fp != nullptr) {
            const char* tmpl =
                "; TTVoice overlay button config (logical px at 96 dpi)\r\n"
                "; x/y = top-left in client coords, w/h = size, text = UTF-8\r\n"
                "; font_size is logical px; it is multiplied by the window DPI scale.\r\n"
                "; font_file is relative to the exe dir unless absolute or %ENV% based.\r\n"
                "; colors are #RRGGBB or #AARRGGBB; bg_alpha 0 = see the app top bar.\r\n"
                "; radius 0 = pill (h/2); border_width 0 hides the outline.\r\n"
                "x=1200\r\n"
                "y=9\r\n"
                "w=80\r\n"
                "h=30\r\n"
                "font_size=15\r\n"
                "font_file=data\\flutter_assets\\web\\fonts\\NotoSansSC.ttf\r\n"
                "font_family=Noto Sans SC\r\n"
                "font_weight=600\r\n"
                "text_color=#FFDC3B\r\n"
                "border_color=#FFDC3B\r\n"
                "border_width=1\r\n"
                "bg_color=#17191F\r\n"
                "bg_alpha=0\r\n"
                "radius=0\r\n"
                "text=\xE6\xB4\xBB\xE5\x8A\xA8\r\n"
                "; no_script_text = pill label while no script file can be loaded\r\n"
                "no_script_text=\xE6\x97\xA0\xE8\x84\x9A\xE6\x9C\xAC\r\n"
                "; script = json name in %APPDATA%\\TTVoice\\scripts, written by\r\n"
                "; tools/script_editor.py. Empty = the file that editor saved last.\r\n"
                "; The pill only runs this script - with no usable script a click\r\n"
                "; does nothing and the label reads no_script_text.\r\n"
                "script=\r\n"
                "; gate_poll_ms = visual gate capture interval (ms, min 30)\r\n"
                "gate_poll_ms=120\r\n";
            fwrite(tmpl, 1, strlen(tmpl), fp);
            fclose(fp);
        }
        return;
    }

    char buf[8192] = {};
    const size_t got = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[got] = 0;

    char* next = nullptr;
    for (char* line = strtok_s(buf, "\r\n", &next); line != nullptr;
         line = strtok_s(nullptr, "\r\n", &next)) {
        while (*line == ' ' || *line == '\t') ++line;
        if (*line == ';' || *line == '#' || *line == 0) continue;

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char* key = line;
        const char* val = eq + 1;

        if (_stricmp(key, "x") == 0) {
            g_btnX = static_cast<float>(atof(val));
        } else if (_stricmp(key, "y") == 0) {
            g_btnY = static_cast<float>(atof(val));
        } else if (_stricmp(key, "w") == 0) {
            g_btnW = static_cast<float>(atof(val));
        } else if (_stricmp(key, "h") == 0) {
            g_btnH = static_cast<float>(atof(val));
        } else if (_stricmp(key, "font_size") == 0) {
            g_fontSize = static_cast<float>(atof(val));
        } else if (_stricmp(key, "font_file") == 0) {
            Utf8ToWide(val, g_fontFile, MAX_PATH);
        } else if (_stricmp(key, "font_family") == 0) {
            Utf8ToWide(val, g_fontFamily, 128);
        } else if (_stricmp(key, "font_weight") == 0) {
            g_fontWeight = atoi(val);
        } else if (_stricmp(key, "text_color") == 0) {
            ParseColor(val, &g_textColor);
        } else if (_stricmp(key, "border_color") == 0) {
            ParseColor(val, &g_borderColor);
        } else if (_stricmp(key, "border_width") == 0) {
            g_borderWidth = static_cast<float>(atof(val));
        } else if (_stricmp(key, "bg_color") == 0) {
            ParseColor(val, &g_bgColor);
        } else if (_stricmp(key, "bg_alpha") == 0) {
            g_bgAlpha = static_cast<float>(atof(val));
        } else if (_stricmp(key, "radius") == 0) {
            g_radius = static_cast<float>(atof(val));
        } else if (_stricmp(key, "text") == 0) {
            Utf8ToWide(val, g_btnText, 64);
        } else if (_stricmp(key, "no_script_text") == 0) {
            if (val[0] != 0) Utf8ToWide(val, g_btnTextNoScript, 64);  // empty = keep default
        } else if (_stricmp(key, "gate_poll_ms") == 0) {
            const int n = atoi(val);
            g_gatePollMs = (n < 30) ? 30 : n;   // below 30 the capture costs dominate
        } else if (_stricmp(key, "script") == 0) {
            Utf8ToWide(val, g_scriptName, 64);
        }
        // log_on_screen / log_lines / log_size / log_seconds are gone: the log
        // panel is the console plugin's own window now. Old keys are ignored.
    }
}

static bool Init(HWND hwnd) {
    g_hwnd = hwnd;

    UINT dpi = 96;
    using PFN = UINT(WINAPI*)(HWND);
    static PFN getDpi = reinterpret_cast<PFN>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (getDpi) dpi = getDpi(hwnd);
    g_scale = (dpi > 0) ? (static_cast<float>(dpi) / 96.0f) : 1.0f;

    LoadConfig();
    ResolveFontPath();

    if (!InstallHooks()) return false;

    // Picks up the hotkey from the script (no-op when it has none) and reloads
    // the script whenever the editor saves a new version.
    ScriptStartHotkey();

    g_ready = true;
    return true;
}

}  // namespace ov
