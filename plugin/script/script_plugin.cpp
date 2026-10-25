// ============================================================================
// script_plugin.cpp - runs the click scripts written by script_editor.py.
//
// Install: plugins\script\ (plugin.json + script.dll). The framework inside
//          version.dll discovers it, loads it and owns every thread - no
//          injection, no host process support, no DllMain work.
//
// Engine:  script_engine.inc is included UNCHANGED, so the JSON contract, file
//          selection and the click/wheel/paste helpers are literally the same
//          code the engine has always run. Only the two things
//          a plugin must not own are replaced:
//            - its run thread          -> stepped through submitTask, one step
//                                         per call, so no tick ever blocks;
//            - its RegisterHotKey loop -> removed; the JSON hotkey is booked
//                                         through Host::addHotkey and dispatched
//                                         by the framework (Phase 1).
//
// Trigger: 1) the script's "hotkey" field, booked via Host::addHotkey and
//             dispatched by the framework (the key still reaches the app);
//          2) the panel: tick the scripts to run, press Run - they execute
//             back-to-back (double-click opens a JSON viewer; Stop aborts the
//             run and the queue);
//          3) an optional panel toggle hotkey (<pluginDir>\config.ini panel_hotkey);
//          4) %APPDATA%\TTVoice\scripts\run.txt appears -> run, file deleted.
//
// Files:   %APPDATA%\TTVoice\scripts\<name>.json  editor output
//          %APPDATA%\TTVoice\scripts\active.txt   name saved last
//          <pluginDir>\config.ini                  panel_hotkey
//
// Note: the proxy's built-in engine no longer registers its own hotkey, so the
// framework dispatcher is the single owner of registration and dispatch.
// UI strings are Chinese: keep this file UTF-8 and compile with /utf-8
// (build_plugin.bat already passes it).
// ============================================================================

#include "../sdk/pplugin.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

static const Host* g_host = nullptr;

// ============================================================================
// Shims: what overlay.inc normally provides to script_engine.inc.
// ============================================================================

static HWND  g_hwnd = nullptr;       // pplugin top level window
static HWND  g_viewHwnd = nullptr;   // FLUTTERVIEW child (input + capture)
static float g_scale = 1.0f;         // view dpi / 96
static int   g_panelVk = 0;         // config.ini panel_hotkey (toggle panel)
static volatile LONG g_scriptStatus = 1;   // 0 ok, 1 no file, 2 rejected

static void LogFromOverlay(const char* text) {
    if (g_host != nullptr && g_host->log != nullptr) {
        g_host->log(text);
    }
}

// The proxy swaps the pill label between "activity" and "no script". There is no
// pill in a plugin, so the code is only remembered and a rejected file explains
// itself in the log.
static void SetScriptStatus(int code, const char* reason) {
    if (code == 2 && reason != nullptr && reason[0] != 0) {
        char line[128] = {};
        sprintf_s(line, "script: unusable (%s)", reason);
        LogFromOverlay(line);
    }
    InterlockedExchange(&g_scriptStatus, code);
}

// ============================================================================
// The engine, unchanged: JSON parse, file selection, reload policy, clicks,
// wheel, paste, and the run thread we deliberately do not use.
// C4505 is expected here: the thread + RegisterHotKey halves of the engine are
// unreferenced by design (a plugin owns neither), the rest is shared verbatim.
// ============================================================================

#pragma warning(push)
#pragma warning(disable: 4505)   // unreferenced local function
#include "script_engine.inc"   // engine now lives in this plugin, not the framework
#pragma warning(pop)

// ============================================================================
// Window / config helpers
// ============================================================================

static BOOL CALLBACK FindViewProc(HWND hwnd, LPARAM lp) {
    wchar_t cls[128] = {};
    GetClassNameW(hwnd, cls, 128);
    if (wcscmp(cls, L"FLUTTERVIEW") == 0) {
        *reinterpret_cast<HWND*>(lp) = hwnd;
        return FALSE;
    }
    return TRUE;
}

static UINT QueryDpi(HWND hwnd) {
    using PFN = UINT(WINAPI*)(HWND);
    static PFN fn = reinterpret_cast<PFN>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    return (fn != nullptr && hwnd != nullptr) ? fn(hwnd) : 0;
}

// Re-resolves main window, FLUTTERVIEW and the scale. The framework's
// viewWindow() still returns the top level window, so the child is found here.
static bool RefreshView() {
    if (g_host == nullptr) {
        return false;
    }

    HWND main = g_host->mainWindow();
    if (main == nullptr || !IsWindow(main)) {
        if (g_hwnd != nullptr) {
            LogFromOverlay("script: main window gone");
        }
        g_hwnd = nullptr;
        g_viewHwnd = nullptr;
        return false;
    }
    g_hwnd = main;

    HWND view = g_viewHwnd;
    if (view == nullptr || !IsWindow(view)) {
        view = nullptr;
        EnumChildWindows(main, &FindViewProc, reinterpret_cast<LPARAM>(&view));
        if (view == nullptr) {
            return false;
        }
        g_viewHwnd = view;

        RECT rc = {};
        GetClientRect(view, &rc);
        char line[160] = {};
        sprintf_s(line, "script: view=%p %dx%d", reinterpret_cast<void*>(view),
                  static_cast<int>(rc.right - rc.left),
                  static_cast<int>(rc.bottom - rc.top));
        LogFromOverlay(line);
    }

    UINT dpi = QueryDpi(g_viewHwnd);
    if (dpi == 0) {
        const float hostScale = g_host->scale();
        dpi = static_cast<UINT>((hostScale > 0.0f ? hostScale : 1.0f) * 96.0f + 0.5f);
    }
    g_scale = (dpi > 0) ? (static_cast<float>(dpi) / 96.0f) : 1.0f;
    return true;
}

// Reads the plugin's own config from <pluginDir>\config.ini (not the framework's
// overlay.ini). panel_hotkey is the only key today. A template is written on
// first run so the key is discoverable.
static void LoadConfig() {
    const char* dir = (g_host != nullptr && g_host->pluginDir != nullptr)
                          ? g_host->pluginDir()
                          : nullptr;
    if (dir == nullptr || dir[0] == 0) {
        LogFromOverlay("script: pluginDir unavailable, defaults kept");
        return;
    }

    wchar_t wdir[MAX_PATH] = {};
    Utf8ToWideN(dir, wdir, MAX_PATH);

    wchar_t path[MAX_PATH] = {};
    swprintf_s(path, L"%s\\config.ini", wdir);

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path, L"rb") != 0 || fp == nullptr) {
        if (_wfopen_s(&fp, path, L"wb") == 0 && fp != nullptr) {
            const char* tmpl =
                "; script plugin config\r\n"
                "; panel_hotkey = VK code that toggles the panel (0 = disabled).\r\n"
                "; Examples: F1=0x70, F9=0x78, F10=0x79, F12=0x7B\r\n"
                "panel_hotkey=0\r\n";
            fwrite(tmpl, 1, strlen(tmpl), fp);
            fclose(fp);
            LogFromOverlay("script: wrote plugin config template");
        }
        return;
    }

    char buf[4096] = {};
    const size_t got = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[got] = 0;

    char* next = nullptr;
    for (char* line = strtok_s(buf, "\r\n", &next); line != nullptr;
         line = strtok_s(nullptr, "\r\n", &next)) {
        while (*line == ' ' || *line == '\t') ++line;
        if (*line == ';' || *line == '#' || *line == 0) continue;

        char* eq = strchr(line, '=');
        if (eq == nullptr) continue;
        *eq = 0;
        const char* key = line;
        const char* val = eq + 1;
        while (*val == ' ' || *val == '\t') ++val;

        if (_stricmp(key, "panel_hotkey") == 0) {
            g_panelVk = atoi(val);
        }
    }

    char line[96] = {};
    sprintf_s(line, "script: config panel_hotkey=%d", g_panelVk);
    LogFromOverlay(line);
}

// ============================================================================
// Stepped runner.
//
// script_engine.inc runs on its own thread; a plugin must not create one, so the
// same sequence is driven one step per submitTask call. Between steps the loader
// thread keeps ticking, watching files and pumping the console window, so even a
// long inter-row delay blocks nothing.
// ============================================================================

enum RunStage {
    kStageIdle = 0,
    kStageDelay,   // inter-row delay_ms
    kStageAct      // wheel / paste / click
};

struct RunState {
    Script    script;
    int       round = 0;
    int       index = 0;
    int       stage = kStageIdle;
    ULONGLONG start = 0;
    ULONGLONG deadline = 0;
};

static RunState g_run;
static ULONGLONG g_lastReload = 0;
static ULONGLONG g_lastTrigger = 0;

// File base name of the running script (what the panel rows are keyed on).
// Written by the loader thread when a run starts, read by the panel timer.
static wchar_t g_runningName[64] = {};

static void __cdecl RunStep(void* user);

static void ScheduleStep() {
    if (g_host == nullptr || g_host->submitTask == nullptr) {
        return;
    }
    if (g_host->submitTask(&RunStep, nullptr) != P_OK) {
        // Cannot happen with a single outstanding step, but never spin and
        // never leave g_scriptActive latched on a run that will not continue.
        LogFromOverlay("script: task queue full, run aborted");
        g_run.stage = kStageIdle;
        InterlockedExchange(&g_scriptActive, 0);
    }
}

static void BeginEvent() {
    g_run.stage = kStageAct;
}

static void FinishRun() {
    char line[160] = {};
    sprintf_s(line, "script: done in %llums",
              static_cast<unsigned long long>(GetTickCount64() - g_run.start));
    LogFromOverlay(line);
    g_run.stage = kStageIdle;
    InterlockedExchange(&g_scriptActive, 0);
}

// Applies one event's actions. The engine's RunEventActions owns the whole
// order (wheel -> paste -> click) and the selector/CDP branch, so the plugin
// and the engine's own thread run literally the same sequence.
static void ActEvent() {
    const ScriptEvent& ev = g_run.script.events[g_run.index];
    RunEventActions(ev, g_run.script.targetUrl);

    const bool lastRow = (g_run.round == g_run.script.loop) &&
                         (g_run.index == g_run.script.count - 1);
    if (lastRow) {
        FinishRun();
        return;
    }

    ++g_run.index;
    if (g_run.index >= g_run.script.count) {
        ++g_run.round;
        g_run.index = 0;
    }

    const int delay = ev.delayMs;
    if (delay > 0) {
        g_run.stage = kStageDelay;
        g_run.deadline = GetTickCount64() + static_cast<ULONGLONG>(delay);
    } else {
        BeginEvent();
    }
    ScheduleStep();
}

static void __cdecl RunStep(void* user) {
    (void)user;
    if (g_run.stage == kStageIdle) {
        return;
    }

    switch (g_run.stage) {
    case kStageDelay:
        if (GetTickCount64() < g_run.deadline) {
            ScheduleStep();
            return;
        }
        BeginEvent();
        ScheduleStep();
        return;
    case kStageAct:
        ActEvent();
        return;
    default:
        g_run.stage = kStageIdle;
        return;
    }
}

static void RunNow(bool verbose) {
    if (g_run.stage != kStageIdle) {
        LogFromOverlay("script: run ignored, previous run still active");
        return;
    }
    if (!ScriptReload(verbose)) {
        return;   // ScriptReload already logged why
    }
    if (g_script.count <= 0) {
        return;
    }
    if (!RefreshView()) {
        LogFromOverlay("script: view window not found, run skipped");
        return;
    }

    // The script's coordinates were picked at a specific view size; centered
    // H5 popups lay out differently per window size, so a mismatch means the
    // click will land elsewhere. Say so loudly.
    if (g_script.viewW > 0 && g_viewHwnd != nullptr) {
        RECT rc = {};
        GetClientRect(g_viewHwnd, &rc);
        const int cw = static_cast<int>(rc.right - rc.left);
        const int ch = static_cast<int>(rc.bottom - rc.top);
        if (cw != g_script.viewW || ch != g_script.viewH) {
            char line[256] = {};
            sprintf_s(line, "script: WARNING view size changed since pick "
                            "(picked at %dx%d, now %dx%d) - clicks may miss; "
                            "re-pick in the editor with the same window state",
                      g_script.viewW, g_script.viewH, cw, ch);
            LogFromOverlay(line);
        }
    }

    g_run.script = g_script;   // detached copy, a reload cannot tear it
    g_run.round = 1;
    g_run.index = 0;
    g_run.start = GetTickCount64();
    InterlockedExchange(&g_scriptActive, 1);
    CdpRunBegin();   // selector rows get a fresh CDP connect attempt per run

    // Panel rows are keyed by file base name (what EnumerateScripts shows).
    {
        wchar_t base[64] = {};
        const wchar_t* slash = wcsrchr(g_scriptPath, L'\\');
        wcsncpy_s(base, (slash != nullptr ? slash + 1 : g_scriptPath), _TRUNCATE);
        wchar_t* dot = wcsrchr(base, L'.');
        if (dot != nullptr) *dot = 0;
        wcscpy_s(g_runningName, base);
    }

    char nm[256] = {};
    char line[320] = {};
    sprintf_s(line, "script: run \"%s\" events=%d loop=%d",
              Nz(g_run.script.name, nm, sizeof(nm)), g_run.script.count,
              g_run.script.loop);
    LogFromOverlay(line);

    BeginEvent();
    ScheduleStep();
}

// ============================================================================
// Triggers
//
// Hotkeys are booked through Host::addHotkey in init and dispatched by the
// framework (WM_HOTKEY on its own message window), so the plugin no longer reads
// the keyboard itself. run.txt stays as the keyless trigger a script/test
// harness can use.
// ============================================================================

static void __cdecl OnHotkey(int vk, void*) {
    (void)vk;
    LogFromOverlay("script: hotkey pressed (framework dispatch)");
    RunNow(true);
}

static void PollTriggerFile() {
    wchar_t appdata[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0) {
        return;
    }
    wchar_t path[MAX_PATH] = {};
    swprintf_s(path, L"%s\\TTVoice\\scripts\\run.txt", appdata);

    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    // Delete before running: a crash mid-run must not replay the request.
    if (!DeleteFileW(path)) {
        LogFromOverlay("script: run.txt delete failed, trigger ignored");
        return;
    }
    LogFromOverlay("script: run requested (run.txt)");
    RunNow(true);
}

// ============================================================================
// Self-owned D2D panel, console-plugin look (dark rounded panel, per-pixel
// alpha) with a deliberately light GUI: one WS_EX_LAYERED popup, zero Win32
// controls. The toolbar (run/stop/reload/hide), the checkbox rows and the
// title-bar close zone are painted rects hit-tested in the WndProc. Drag by
// the title bar only; WS_EX_NOACTIVATE keeps the keyboard with the app.
//
// Mirrors the device-farm UI: tick the rows to run, press 运行 - checked
// scripts execute back-to-back; 停止 aborts the current run and drops the
// queue. Runs on its own thread so message pumping never blocks the framework
// loader thread. The panel does not call Host::submitTask directly: that needs
// the framework's slot context, which is only set during framework callbacks.
// Run/stop requests are flagged here and serviced from OnPluginTick, which
// runs with the context set. Host::log works from any thread.
// ============================================================================

static HWND   g_panel = nullptr;
static HWND   g_owner = nullptr;          // app top level window (owner)
static POINT  g_ownerOrigin = {0, 0};     // owner client origin, screen px
static bool   g_ownerIconic = false;
static HANDLE g_panelThread = nullptr;
static DWORD  g_panelTid = 0;
static HINSTANCE g_panelInst = nullptr;
static volatile LONG g_panelStop = 0;
static volatile LONG g_runRequested = 0;
static volatile LONG g_stopRequested = 0;
static volatile LONG g_stopAck = 0;
static wchar_t g_requestedName[64] = {};   // script base name to run next

// Run queue: checked scripts minus the first (it goes through
// g_requestedName). Filled by the panel thread, drained by OnPluginTick
// between runs; the engine runs one script at a time.
static CRITICAL_SECTION g_queueLock;
static wchar_t g_queue[16][64] = {};
static int     g_queueCount = 0;

// ---- row model (panel thread only) ----------------------------------------

enum RowStatus {
    kRowReady = 0,
    kRowRunning,
    kRowQueued,
    kRowDone,
    kRowStopped,
    kRowCancelled
};

struct ScriptRow {
    wchar_t name[260];
    int     events;
    int     loop;
    int     status;
    bool    checked;
};

static ScriptRow g_rows[64];
static int  g_rowCount = 0;
static int  g_visRows = 6;        // rows the window is sized for (4..12)
static int  g_scroll = 0;
static int  g_hoverRow = -1;
static int  g_hoverBtn = -1;
static bool g_titleHover = false;
static bool g_closeHover = false;
static bool g_dirty = true;
static bool g_dragging = false;
static POINT g_grab = {0, 0};

// ---- layout, logical px (multiplied by g_scale when drawn) -----------------

static const float kPanelW = 430.0f;
static const float kTitleH = 26.0f;
static const float kToolH  = 44.0f;
static const float kRowH   = 22.0f;
static const float kPad    = 8.0f;
static const float kCorner = 6.0f;
static const float kFontSize = 13.0f;
static const float kMargin = 24.0f;      // gap from the app window edge

struct BtnDef { const wchar_t* text; float w; };
static const BtnDef kBtns[] = {
    { L"▶ 运行", 86 },
    { L"■ 停止", 86 },
    { L"↻ 刷新", 86 },
    { L"隐藏",   64 },
};

// D2D / DWrite, panel thread only.
static ComPtr<IDWriteFactory>      g_dw;
static ComPtr<IDWriteTextFormat>   g_fmt;        // row text (YaHei UI)
static ComPtr<IDWriteTextFormat>   g_centerFmt;  // button labels
static ComPtr<IDWriteTextFormat>   g_titleFmt;   // title bar (Consolas, console)
static ComPtr<ID2D1Factory>        g_d2d;
static ComPtr<ID2D1DCRenderTarget> g_rt;
static HDC     g_memDC = nullptr;
static HBITMAP g_bmp = nullptr;
static HGDIOBJ g_oldBmp = nullptr;
static int     g_winW = 0;
static int     g_winH = 0;

static void QueueClear() {
    EnterCriticalSection(&g_queueLock);
    g_queueCount = 0;
    LeaveCriticalSection(&g_queueLock);
}

static void QueuePush(const wchar_t* name) {
    EnterCriticalSection(&g_queueLock);
    if (g_queueCount < 16) {
        wcscpy_s(g_queue[g_queueCount++], name);
    }
    LeaveCriticalSection(&g_queueLock);
}

static bool QueuePop(wchar_t* out, size_t cap) {
    EnterCriticalSection(&g_queueLock);
    const bool have = (g_queueCount > 0);
    if (have) {
        wcscpy_s(out, cap, g_queue[0]);
        --g_queueCount;
        for (int i = 0; i < g_queueCount; ++i) {
            wcscpy_s(g_queue[i], g_queue[i + 1]);
        }
    }
    LeaveCriticalSection(&g_queueLock);
    return have;
}

static bool QueueContains(const wchar_t* name) {
    EnterCriticalSection(&g_queueLock);
    bool found = false;
    for (int i = 0; i < g_queueCount && !found; ++i) {
        found = (wcscmp(g_queue[i], name) == 0);
    }
    LeaveCriticalSection(&g_queueLock);
    return found;
}

// Pulls the int that follows "key": in the file bytes. The editor's output is
// machine-written and flat, so a strstr scan is enough (no JSON parser here;
// the panel thread must not touch the engine's g_script).
static void JsonQuickInt(const char* buf, const char* key, int* out) {
    const char* p = strstr(buf, key);
    if (p == nullptr) return;
    p = strchr(p + strlen(key), ':');
    if (p == nullptr) return;
    *out = atoi(p + 1);
}

// ---- surfaces (same scheme as the console plugin) --------------------------

static void ReleaseSurfaces() {
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
}

// Top-down 32bpp DIB + a D2D DC render target bound to it. Rebuilt only when
// the pixel size changes.
static bool EnsureSurfaces(UINT w, UINT h) {
    if (g_rt != nullptr && w == static_cast<UINT>(g_winW) &&
        h == static_cast<UINT>(g_winH)) {
        return true;
    }
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

    g_winW = static_cast<int>(w);
    g_winH = static_cast<int>(h);
    return true;
}

// ---- panel state ops -------------------------------------------------------

static void EnumerateScripts() {
    g_rowCount = 0;
    g_scroll = 0;
    wchar_t appdata[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0) return;
    wchar_t pattern[MAX_PATH] = {};
    swprintf_s(pattern, L"%s\\TTVoice\\scripts\\*.json", appdata);
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (g_rowCount >= 64) break;

            ScriptRow& r = g_rows[g_rowCount];
            ZeroMemory(&r, sizeof(r));
            wcscpy_s(r.name, fd.cFileName);
            wchar_t* dot = wcsrchr(r.name, L'.');
            if (dot != nullptr) *dot = 0;

            wchar_t path[MAX_PATH] = {};
            swprintf_s(path, L"%s\\TTVoice\\scripts\\%s", appdata, fd.cFileName);
            FILE* fp = nullptr;
            if (_wfopen_s(&fp, path, L"rb") == 0 && fp != nullptr) {
                char buf[16384] = {};
                const size_t got = fread(buf, 1, sizeof(buf) - 1, fp);
                fclose(fp);
                buf[got] = 0;
                JsonQuickInt(buf, "\"events\"", &r.events);
                JsonQuickInt(buf, "\"loop\"", &r.loop);
            }
            ++g_rowCount;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // Size the window for the row count (4..12 rows).
    int vis = g_rowCount;
    if (vis < 4) vis = 4;
    if (vis > 12) vis = 12;
    if (vis != g_visRows) {
        g_visRows = vis;
        if (g_panel != nullptr) {
            const int w = static_cast<int>(kPanelW * g_scale + 0.5f);
            const int hgt = static_cast<int>(
                (kTitleH + kToolH + 2.0f * kPad + vis * kRowH) * g_scale + 0.5f);
            SetWindowPos(g_panel, nullptr, 0, 0, w, hgt,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    g_dirty = true;
}

// Status column upkeep: rows flip through 运行中/排队中/已结束/已停止/已取消
// driven by the engine state (g_scriptActive / g_runningName).
static void UpdateStatuses() {
    const bool running = InterlockedCompareExchange(&g_scriptActive, 0, 0) != 0;
    const bool stopped = InterlockedExchange(&g_stopAck, 0) != 0;
    bool changed = false;
    for (int i = 0; i < g_rowCount; ++i) {
        ScriptRow& r = g_rows[i];
        const bool isRun = (r.status == kRowRunning);
        const bool isQueue = (r.status == kRowQueued);
        if (!isRun && !isQueue) continue;
        if (running && isRun && wcscmp(r.name, g_runningName) == 0) {
            continue;   // still the active script
        }
        int ns = r.status;
        if (stopped) {
            ns = isQueue ? kRowCancelled : kRowStopped;
        } else if (running) {
            if (isQueue && wcscmp(r.name, g_runningName) == 0) {
                ns = kRowRunning;      // its turn came
            } else if (isRun) {
                ns = kRowDone;
            }
        } else if (QueueContains(r.name) ||
                   (g_runRequested != 0 &&
                    wcscmp(r.name, g_requestedName) == 0)) {
            continue;   // queued or about to start
        } else {
            ns = isQueue ? kRowCancelled : kRowDone;
        }
        if (ns != r.status) {
            r.status = ns;
            changed = true;
        }
    }
    if (changed) g_dirty = true;
}

// Run button: every checked row joins the queue (first one starts now);
// nothing checked falls back to the hovered row, else the default script.
static void RequestRunChecked() {
    wchar_t names[16][260] = {};
    int idx[16] = {};
    int n = 0;
    for (int i = 0; i < g_rowCount && n < 16; ++i) {
        if (!g_rows[i].checked) continue;
        wcscpy_s(names[n], g_rows[i].name);
        idx[n] = i;
        ++n;
    }

    if (n == 0) {
        const int r = g_hoverRow;
        wchar_t name[260] = {};
        if (r >= 0) wcscpy_s(name, g_rows[r].name);
        wcsncpy_s(g_requestedName, name, _TRUNCATE);
        if (r >= 0) g_rows[r].status = kRowRunning;
        InterlockedExchange(&g_runRequested, 1);
        g_dirty = true;
        return;
    }

    for (int i = 1; i < n; ++i) {
        QueuePush(names[i]);
        g_rows[idx[i]].status = kRowQueued;
    }
    wcsncpy_s(g_requestedName, names[0], _TRUNCATE);
    g_rows[idx[0]].status = kRowRunning;
    InterlockedExchange(&g_runRequested, 1);
    g_dirty = true;
}

static void OnButton(HWND hwnd, int i) {
    switch (i) {
    case 0: RequestRunChecked(); break;
    case 1:
        QueueClear();
        InterlockedExchange(&g_stopRequested, 1);
        break;
    case 2: EnumerateScripts(); break;
    case 3: ShowWindow(hwnd, SW_HIDE); break;
    default: break;
    }
}

// ---- painting --------------------------------------------------------------

struct StatusStyle { const wchar_t* text; float r, g, b; };

static StatusStyle StatusOf(int st) {
    switch (st) {
    case kRowRunning:   return { L"运行中", 0.35f, 0.80f, 0.50f };
    case kRowQueued:    return { L"排队中", 0.38f, 0.65f, 0.98f };
    case kRowDone:      return { L"已结束", 0.55f, 0.60f, 0.68f };
    case kRowStopped:   return { L"已停止", 1.00f, 0.72f, 0.30f };
    case kRowCancelled: return { L"已取消", 1.00f, 0.72f, 0.30f };
    default:            return { L"就绪",   0.62f, 0.67f, 0.74f };
    }
}

// One full repaint: draw into the DIB, hand it to the layered window.
static void Paint() {
    if (g_panel == nullptr || g_fmt == nullptr || g_centerFmt == nullptr ||
        g_titleFmt == nullptr) {
        return;
    }
    if (!EnsureSurfaces(
            static_cast<UINT>(kPanelW * g_scale + 0.5f),
            static_cast<UINT>((kTitleH + kToolH + 2.0f * kPad + g_visRows * kRowH) *
                              g_scale + 0.5f))) {
        return;
    }
    const RECT bounds = {0, 0, g_winW, g_winH};
    if (FAILED(g_rt->BindDC(g_memDC, &bounds))) return;

    const float W = static_cast<float>(g_winW);
    const float H = static_cast<float>(g_winH);
    const float s = g_scale;

    ComPtr<ID2D1SolidColorBrush> body, band, btn, btnHover, rowHover, fg, dim,
                                 green, red, blue, white, line, sep;
    const bool ok =
        SUCCEEDED(g_rt->CreateSolidColorBrush(D2D1::ColorF(0.04f, 0.05f, 0.07f, 0.82f),
                                              body.GetAddressOf())) &&
        SUCCEEDED(g_rt->CreateSolidColorBrush(
            g_titleHover ? D2D1::ColorF(0.16f, 0.19f, 0.25f, 0.92f)
                         : D2D1::ColorF(0.10f, 0.12f, 0.16f, 0.90f),
            band.GetAddressOf())) &&
        SUCCEEDED(g_rt->CreateSolidColorBrush(D2D1::ColorF(0.14f, 0.17f, 0.22f, 0.95f),
                                              btn.GetAddressOf())) &&
        SUCCEEDED(g_rt->CreateSolidColorBrush(D2D1::ColorF(0.24f, 0.28f, 0.36f, 0.98f),
                                              btnHover.GetAddressOf())) &&
        SUCCEEDED(g_rt->CreateSolidColorBrush(D2D1::ColorF(0.16f, 0.20f, 0.26f, 0.60f),
                                              rowHover.GetAddressOf())) &&
        SUCCEEDED(g_rt->CreateSolidColorBrush(D2D1::ColorF(0.86f, 0.89f, 0.93f, 1.00f),
                                              fg.GetAddressOf())) &&
        SUCCEEDED(g_rt->CreateSolidColorBrush(D2D1::ColorF(0.55f, 0.60f, 0.68f, 1.00f),
                                              dim.GetAddressOf())) &&
        SUCCEEDED(g_rt->CreateSolidColorBrush(D2D1::ColorF(0.35f, 0.80f, 0.50f, 1.00f),
                                              green.GetAddressOf())) &&
        SUCCEEDED(g_rt->CreateSolidColorBrush(D2D1::ColorF(0.92f, 0.45f, 0.38f, 1.00f),
                                              red.GetAddressOf())) &&
        SUCCEEDED(g_rt->CreateSolidColorBrush(D2D1::ColorF(0.38f, 0.65f, 0.98f, 1.00f),
                                              blue.GetAddressOf())) &&
        SUCCEEDED(g_rt->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.96f, 0.98f, 1.00f),
                                              white.GetAddressOf())) &&
        SUCCEEDED(g_rt->CreateSolidColorBrush(D2D1::ColorF(0.72f, 0.77f, 0.85f, 1.00f),
                                              line.GetAddressOf())) &&
        SUCCEEDED(g_rt->CreateSolidColorBrush(D2D1::ColorF(0.00f, 0.00f, 0.00f, 0.35f),
                                              sep.GetAddressOf()));
    if (!ok) return;

    g_rt->BeginDraw();
    g_rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    const D2D1_ROUNDED_RECT outline = D2D1::RoundedRect(
        D2D1::RectF(0.0f, 0.0f, W, H), kCorner * s, kCorner * s);
    g_rt->FillRoundedRectangle(outline, band.Get());
    g_rt->PushAxisAlignedClip(outline.rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    const float titleH = kTitleH * s;
    const float toolH = kToolH * s;
    const float pad = kPad * s;

    // body under the toolbar, separators under title and toolbar
    g_rt->FillRectangle(D2D1::RectF(0.0f, titleH + toolH, W, H), body.Get());
    g_rt->FillRectangle(D2D1::RectF(0.0f, titleH - 1.0f, W, titleH), line.Get());
    g_rt->FillRectangle(D2D1::RectF(0.0f, titleH + toolH - 1.0f, W, titleH + toolH),
                        sep.Get());

    // title text + close zone (two crossed lines)
    const D2D1_RECT_F titleR = D2D1::RectF(pad, 0.0f, W - 30.0f * s, titleH);
    g_rt->DrawText(L"script", 6, g_titleFmt.Get(), &titleR, line.Get());
    const float cx = W - 15.0f * s;
    const float cy = titleH * 0.5f;
    const float d = 4.0f * s;
    const float swc = 1.3f * s;
    g_rt->DrawLine(D2D1::Point2F(cx - d, cy - d), D2D1::Point2F(cx + d, cy + d),
                   g_closeHover ? white.Get() : line.Get(), swc);
    g_rt->DrawLine(D2D1::Point2F(cx - d, cy + d), D2D1::Point2F(cx + d, cy - d),
                   g_closeHover ? white.Get() : line.Get(), swc);

    // toolbar buttons
    float bx = pad;
    const float by = titleH + 8.0f * s;
    const float bh = 28.0f * s;
    for (int i = 0; i < 4; ++i) {
        const float bw = kBtns[i].w * s;
        const D2D1_ROUNDED_RECT br = D2D1::RoundedRect(
            D2D1::RectF(bx, by, bx + bw, by + bh), 4.0f * s, 4.0f * s);
        g_rt->FillRoundedRectangle(br, (g_hoverBtn == i) ? btnHover.Get()
                                                         : btn.Get());
        const D2D1_RECT_F tr = D2D1::RectF(bx, by, bx + bw, by + bh);
        ID2D1SolidColorBrush* label =
            (i == 0) ? green.Get() : ((i == 1) ? red.Get() : fg.Get());
        g_rt->DrawText(kBtns[i].text,
                       static_cast<UINT32>(wcslen(kBtns[i].text)),
                       g_centerFmt.Get(), &tr, label);
        bx += bw + 10.0f * s;
    }

    // rows: checkbox | name | events | loop | status
    const float rowTop = titleH + toolH + pad * 0.5f;
    const float rowH = kRowH * s;
    if (g_rowCount == 0) {
        const D2D1_RECT_F er = D2D1::RectF(pad, rowTop, W - pad,
                                           rowTop + g_visRows * rowH);
        g_rt->DrawText(L"无脚本（scripts\\*.json）",
                       static_cast<UINT32>(wcslen(L"无脚本（scripts\\*.json）")),
                       g_fmt.Get(), &er, dim.Get());
    }
    for (int vi = 0; vi < g_visRows; ++vi) {
        const int ri = vi + g_scroll;
        if (ri >= g_rowCount) break;
        const ScriptRow& r = g_rows[ri];
        const float y = rowTop + vi * rowH;

        if (ri == g_hoverRow) {
            g_rt->FillRectangle(D2D1::RectF(pad, y, W - pad, y + rowH),
                                rowHover.Get());
        }

        const float cbx = pad + 2.0f * s;
        const float cb = 13.0f * s;
        const float cby = y + (rowH - cb) * 0.5f;
        const D2D1_ROUNDED_RECT cbr = D2D1::RoundedRect(
            D2D1::RectF(cbx, cby, cbx + cb, cby + cb), 2.5f * s, 2.5f * s);
        if (r.checked) {
            g_rt->FillRoundedRectangle(cbr, blue.Get());
            g_rt->DrawLine(D2D1::Point2F(cbx + 3.0f * s, cby + 6.5f * s),
                           D2D1::Point2F(cbx + 5.5f * s, cby + 9.0f * s),
                           white.Get(), 1.6f * s);
            g_rt->DrawLine(D2D1::Point2F(cbx + 5.5f * s, cby + 9.0f * s),
                           D2D1::Point2F(cbx + 10.0f * s, cby + 3.5f * s),
                           white.Get(), 1.6f * s);
        } else {
            g_rt->DrawRoundedRectangle(cbr, dim.Get(), 1.0f * s);
        }

        const D2D1_RECT_F nameR = D2D1::RectF(30.0f * s, y, 212.0f * s, y + rowH);
        g_rt->DrawText(r.name, static_cast<UINT32>(wcslen(r.name)),
                       g_fmt.Get(), &nameR, fg.Get());

        wchar_t num[32] = {};
        swprintf_s(num, L"%d", r.events);
        const D2D1_RECT_F evR = D2D1::RectF(216.0f * s, y, 258.0f * s, y + rowH);
        g_rt->DrawText(num, static_cast<UINT32>(wcslen(num)),
                       g_fmt.Get(), &evR, dim.Get());

        swprintf_s(num, L"%d", r.loop);
        const D2D1_RECT_F lpR = D2D1::RectF(262.0f * s, y, 302.0f * s, y + rowH);
        g_rt->DrawText(num, static_cast<UINT32>(wcslen(num)),
                       g_fmt.Get(), &lpR, dim.Get());

        const StatusStyle ss = StatusOf(r.status);
        ComPtr<ID2D1SolidColorBrush> st;
        if (SUCCEEDED(g_rt->CreateSolidColorBrush(
                D2D1::ColorF(ss.r, ss.g, ss.b, 1.0f), st.GetAddressOf()))) {
            const D2D1_RECT_F stR = D2D1::RectF(306.0f * s, y, W - pad, y + rowH);
            g_rt->DrawText(ss.text, static_cast<UINT32>(wcslen(ss.text)),
                           g_fmt.Get(), &stR, st.Get());
        }
    }

    g_rt->PopAxisAlignedClip();   // rounded outline
    if (FAILED(g_rt->EndDraw())) {
        LogFromOverlay("script: EndDraw failed, surfaces rebuilt");
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
        LogFromOverlay("script: UpdateLayeredWindow failed");
    }
    g_dirty = false;
}

// ---- hit testing / messages ------------------------------------------------

static bool InTitle(float ly) { return ly >= 0.0f && ly < kTitleH; }

static bool InClose(float lx, float ly) {
    return lx >= kPanelW - 26.0f && InTitle(ly);
}

static int HitButton(float lx, float ly) {
    if (ly < kTitleH || ly >= kTitleH + kToolH) return -1;
    float x = kPad;
    for (int i = 0; i < 4; ++i) {
        if (lx >= x && lx < x + kBtns[i].w) return i;
        x += kBtns[i].w + 10.0f;
    }
    return -1;
}

static int HitRow(float lx, float ly) {
    (void)lx;   // rows span the full width; only the y matters
    const float top = kTitleH + kToolH + kPad * 0.5f;
    if (ly < top) return -1;
    const int vi = static_cast<int>((ly - top) / kRowH);
    if (vi < 0 || vi >= g_visRows) return -1;
    const int ri = vi + g_scroll;
    return (ri < g_rowCount) ? ri : -1;
}

// Cursor position in the window, logical px.
static void ClientLogical(HWND hwnd, POINT* out) {
    POINT p = {};
    GetCursorPos(&p);
    ScreenToClient(hwnd, &p);
    out->x = static_cast<int>(p.x / g_scale);
    out->y = static_cast<int>(p.y / g_scale);
}

// Keep the panel attached to the app window and hidden while it is minimized
// (same deal the console panel strikes).
static void OwnerFollow(HWND hwnd) {
    if (g_owner == nullptr || !IsWindow(g_owner)) return;
    const bool iconic = IsIconic(g_owner) != 0;
    if (iconic && !g_ownerIconic) {
        ShowWindow(hwnd, SW_HIDE);
        g_ownerIconic = true;
        return;
    }
    if (!iconic && g_ownerIconic) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        g_ownerIconic = false;
        g_dirty = true;
    }
    if (iconic) return;

    POINT origin = {0, 0};
    RECT orc = {};
    if (GetClientRect(g_owner, &orc)) {
        ClientToScreen(g_owner, &origin);
        const int dx = origin.x - g_ownerOrigin.x;
        const int dy = origin.y - g_ownerOrigin.y;
        if ((dx != 0 || dy != 0) && !g_dragging) {
            RECT wr = {};
            GetWindowRect(hwnd, &wr);
            SetWindowPos(hwnd, nullptr, wr.left + dx, wr.top + dy, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        g_ownerOrigin = origin;
    }
}

// ---- content viewer (double-click a row) -----------------------------------
//
// A second layered popup in the same dark style, sharing the panel's thread,
// D2D factory and fonts. Shows the script's JSON pretty-printed (newline +
// two-space indent after '{' '[' and before '}' ']', strings kept intact).
// Wheel scrolls; title bar drags; the close zone hides it - the panel itself
// keeps running.

static const float kViewW     = 560.0f;
static const float kViewRows  = 24.0f;
static const float kViewLineH = 17.0f;

static HWND   g_viewer = nullptr;
static HDC     g_vmemDC = nullptr;
static HBITMAP g_vbmp = nullptr;
static HGDIOBJ g_voldBmp = nullptr;
static ComPtr<ID2D1DCRenderTarget> g_vrt;
static ComPtr<IDWriteTextFormat>   g_monoFmt;
static int   g_vwinW = 0;
static int   g_vwinH = 0;
static wchar_t g_viewTitle[260] = {};
static wchar_t g_viewLines[400][256] = {};
static int   g_viewLineCount = 0;
static int   g_viewScroll = 0;
static bool  g_viewDrag = false;
static bool  g_viewCloseHover = false;
static bool  g_viewDirty = true;
static POINT g_viewGrab = {0, 0};

static void ViewPushLine(const wchar_t* buf, int n) {
    if (g_viewLineCount >= 400) return;
    if (n > 254) n = 254;
    wchar_t* dst = g_viewLines[g_viewLineCount];
    memcpy(dst, buf, static_cast<size_t>(n) * sizeof(wchar_t));
    dst[n] = 0;
    ++g_viewLineCount;
}

// Display-only pretty printer: strings are copied verbatim (a string longer
// than a row just wraps raw); whitespace outside strings is re-flowed.
static void PrettyJson(const wchar_t* src) {
    g_viewLineCount = 0;
    wchar_t cur[256] = {};
    int n = 0;
    int indent = 0;
    bool inStr = false;
    bool esc = false;

    auto flushLine = [&]() { ViewPushLine(cur, n); n = 0; };
    auto addIndent = [&]() {
        for (int t = 0; t < indent && n < 254; ++t) cur[n++] = L' ';
    };

    for (const wchar_t* p = src; *p != 0; ++p) {
        const wchar_t ch = *p;
        if (inStr) {
            if (n >= 254) flushLine();
            cur[n++] = ch;
            if (esc) esc = false;
            else if (ch == L'\\') esc = true;
            else if (ch == L'"') inStr = false;
            continue;
        }
        switch (ch) {
        case L'"':
            if (n >= 254) flushLine();
            cur[n++] = ch;
            inStr = true;
            break;
        case L'{': case L'[':
            if (n >= 254) flushLine();
            cur[n++] = ch;
            flushLine();
            ++indent;
            addIndent();
            break;
        case L'}': case L']':
            flushLine();
            if (indent > 0) --indent;
            addIndent();
            cur[n++] = ch;
            break;
        case L',':
            if (n >= 254) flushLine();
            cur[n++] = ch;
            flushLine();
            addIndent();
            break;
        case L'\r': case L'\n':
            break;   // the original line breaks are replaced by the pretty ones
        default:
            if (n == 0 && (ch == L' ' || ch == L'\t')) break;  // no leading ws
            if (n >= 254) flushLine();
            cur[n++] = ch;
            break;
        }
    }
    if (n > 0) flushLine();
}

static void LoadViewContent(const wchar_t* name) {
    g_viewTitle[0] = 0;
    g_viewLineCount = 0;
    g_viewScroll = 0;
    wcscpy_s(g_viewTitle, name);

    wchar_t appdata[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0) return;
    wchar_t path[MAX_PATH] = {};
    swprintf_s(path, L"%s\\TTVoice\\scripts\\%s.json", appdata, name);
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path, L"rb") != 0 || fp == nullptr) {
        const wchar_t* err = L"（无法打开脚本文件）";
        ViewPushLine(err, static_cast<int>(wcslen(err)));
        g_viewDirty = true;
        return;
    }
    char buf[32768] = {};
    const size_t got = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[got] = 0;

    wchar_t wide[16384] = {};
    MultiByteToWideChar(CP_UTF8, 0, buf, -1, wide, 16384);
    PrettyJson(wide);
    if (g_viewLineCount == 0) {
        const wchar_t* empty = L"（空脚本）";
        ViewPushLine(empty, static_cast<int>(wcslen(empty)));
    }
    g_viewDirty = true;
}

static void ViewReleaseSurfaces() {
    g_vrt.Reset();
    if (g_vmemDC != nullptr && g_voldBmp != nullptr) {
        SelectObject(g_vmemDC, g_voldBmp);
        g_voldBmp = nullptr;
    }
    if (g_vbmp != nullptr) {
        DeleteObject(g_vbmp);
        g_vbmp = nullptr;
    }
    if (g_vmemDC != nullptr) {
        DeleteDC(g_vmemDC);
        g_vmemDC = nullptr;
    }
}

static bool ViewEnsureSurfaces(UINT w, UINT h) {
    if (g_vrt != nullptr && w == static_cast<UINT>(g_vwinW) &&
        h == static_cast<UINT>(g_vwinH)) {
        return true;
    }
    ViewReleaseSurfaces();
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
    g_vmemDC = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (g_vmemDC == nullptr) return false;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(w);
    bi.bmiHeader.biHeight = -static_cast<LONG>(h);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    g_vbmp = CreateDIBSection(g_vmemDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (g_vbmp == nullptr || bits == nullptr) {
        ViewReleaseSurfaces();
        return false;
    }
    g_voldBmp = SelectObject(g_vmemDC, g_vbmp);

    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        0.0f, 0.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
    if (FAILED(g_d2d->CreateDCRenderTarget(&props, g_vrt.GetAddressOf()))) {
        ViewReleaseSurfaces();
        return false;
    }
    g_vrt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    g_vrt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    g_vwinW = static_cast<int>(w);
    g_vwinH = static_cast<int>(h);
    return true;
}

static void PaintView() {
    if (g_viewer == nullptr || g_fmt == nullptr || g_monoFmt == nullptr) return;
    if (!ViewEnsureSurfaces(
            static_cast<UINT>(kViewW * g_scale + 0.5f),
            static_cast<UINT>((kTitleH + kViewRows * kViewLineH + 2.0f * kPad) *
                              g_scale + 0.5f))) {
        return;
    }
    const RECT bounds = {0, 0, g_vwinW, g_vwinH};
    if (FAILED(g_vrt->BindDC(g_vmemDC, &bounds))) return;

    const float W = static_cast<float>(g_vwinW);
    const float H = static_cast<float>(g_vwinH);
    const float s = g_scale;

    ComPtr<ID2D1SolidColorBrush> body, band, fg, dim, line, white;
    const bool ok =
        SUCCEEDED(g_vrt->CreateSolidColorBrush(D2D1::ColorF(0.04f, 0.05f, 0.07f, 0.85f),
                                               body.GetAddressOf())) &&
        SUCCEEDED(g_vrt->CreateSolidColorBrush(
            g_viewCloseHover ? D2D1::ColorF(0.16f, 0.19f, 0.25f, 0.92f)
                             : D2D1::ColorF(0.10f, 0.12f, 0.16f, 0.90f),
            band.GetAddressOf())) &&
        SUCCEEDED(g_vrt->CreateSolidColorBrush(D2D1::ColorF(0.86f, 0.89f, 0.93f, 1.00f),
                                               fg.GetAddressOf())) &&
        SUCCEEDED(g_vrt->CreateSolidColorBrush(D2D1::ColorF(0.55f, 0.60f, 0.68f, 1.00f),
                                               dim.GetAddressOf())) &&
        SUCCEEDED(g_vrt->CreateSolidColorBrush(D2D1::ColorF(0.72f, 0.77f, 0.85f, 1.00f),
                                               line.GetAddressOf())) &&
        SUCCEEDED(g_vrt->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.96f, 0.98f, 1.00f),
                                               white.GetAddressOf()));
    if (!ok) return;

    g_vrt->BeginDraw();
    g_vrt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    const D2D1_ROUNDED_RECT outline = D2D1::RoundedRect(
        D2D1::RectF(0.0f, 0.0f, W, H), kCorner * s, kCorner * s);
    g_vrt->FillRoundedRectangle(outline, band.Get());
    g_vrt->PushAxisAlignedClip(outline.rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    const float titleH = kTitleH * s;
    const float pad = kPad * s;
    g_vrt->FillRectangle(D2D1::RectF(0.0f, titleH, W, H), body.Get());
    g_vrt->FillRectangle(D2D1::RectF(0.0f, titleH - 1.0f, W, titleH), line.Get());

    const D2D1_RECT_F titleR = D2D1::RectF(pad, 0.0f, W - 30.0f * s, titleH);
    g_vrt->DrawText(g_viewTitle, static_cast<UINT32>(wcslen(g_viewTitle)),
                    g_fmt.Get(), &titleR, line.Get());
    const float cx = W - 15.0f * s;
    const float cy = titleH * 0.5f;
    const float d = 4.0f * s;
    g_vrt->DrawLine(D2D1::Point2F(cx - d, cy - d), D2D1::Point2F(cx + d, cy + d),
                    g_viewCloseHover ? white.Get() : line.Get(), 1.3f * s);
    g_vrt->DrawLine(D2D1::Point2F(cx - d, cy + d), D2D1::Point2F(cx + d, cy - d),
                    g_viewCloseHover ? white.Get() : line.Get(), 1.3f * s);

    const float lh = kViewLineH * s;
    const int vis = static_cast<int>((H - titleH - pad) / lh);
    const int mx = (g_viewLineCount > vis) ? (g_viewLineCount - vis) : 0;
    if (g_viewScroll > mx) g_viewScroll = mx;
    if (g_viewScroll < 0) g_viewScroll = 0;
    for (int i = 0; i < vis; ++i) {
        const int li = i + g_viewScroll;
        if (li >= g_viewLineCount) break;
        const float y = titleH + pad * 0.5f + i * lh;
        const D2D1_RECT_F lr = D2D1::RectF(pad, y, W - pad, y + lh);
        g_vrt->DrawText(g_viewLines[li],
                        static_cast<UINT32>(wcslen(g_viewLines[li])),
                        g_monoFmt.Get(), &lr, fg.Get());
    }

    g_vrt->PopAxisAlignedClip();
    if (FAILED(g_vrt->EndDraw())) {
        ViewReleaseSurfaces();
        return;
    }

    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    POINT src = {0, 0};
    SIZE size = {g_vwinW, g_vwinH};
    UpdateLayeredWindow(g_viewer, nullptr, nullptr, &size, g_vmemDC, &src, 0,
                        &blend, ULW_ALPHA);
    g_viewDirty = false;
}

// Rows that fit under the viewer's title bar, physical px -> logical.
static int ViewVisibleRows() {
    const float bodyH = static_cast<float>(g_vwinH) / g_scale - kTitleH - kPad;
    return static_cast<int>(bodyH / kViewLineH);
}

static LRESULT CALLBACK ViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN: {
        POINT pt = {};
        ClientLogical(hwnd, &pt);
        const float lx = static_cast<float>(pt.x);
        const float ly = static_cast<float>(pt.y);
        if (lx >= kViewW - 26.0f && InTitle(ly)) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        if (InTitle(ly)) {
            POINT cur = {};
            GetCursorPos(&cur);
            RECT wr = {};
            GetWindowRect(hwnd, &wr);
            g_viewGrab.x = cur.x - wr.left;
            g_viewGrab.y = cur.y - wr.top;
            g_viewDrag = true;
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_viewDrag) {
            g_viewDrag = false;
            ReleaseCapture();
        }
        return 0;
    case WM_MOUSEMOVE: {
        if (g_viewDrag) {
            POINT cur = {};
            GetCursorPos(&cur);
            SetWindowPos(hwnd, nullptr, cur.x - g_viewGrab.x, cur.y - g_viewGrab.y,
                         0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        POINT pt = {};
        ClientLogical(hwnd, &pt);
        const bool close = static_cast<float>(pt.x) >= kViewW - 26.0f &&
                           InTitle(static_cast<float>(pt.y));
        TRACKMOUSEEVENT tme = {};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        if (close != g_viewCloseHover) {
            g_viewCloseHover = close;
            PaintView();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_viewCloseHover) {
            g_viewCloseHover = false;
            PaintView();
        }
        return 0;
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int ns = g_viewScroll + ((delta < 0) ? 3 : -3);
        const int mx = (g_viewLineCount > ViewVisibleRows())
                           ? (g_viewLineCount - ViewVisibleRows()) : 0;
        if (ns > mx) ns = mx;
        if (ns < 0) ns = 0;
        if (ns != g_viewScroll) {
            g_viewScroll = ns;
            PaintView();
        }
        return 0;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt = {};
            ClientLogical(hwnd, &pt);
            if (InTitle(static_cast<float>(pt.y))) {
                SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32646)));
                return TRUE;
            }
        }
        break;
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        g_viewer = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void OpenViewer(int ri) {
    if (ri < 0 || ri >= g_rowCount) return;
    LoadViewContent(g_rows[ri].name);

    if (g_viewer == nullptr || !IsWindow(g_viewer)) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &ViewProc;
        wc.hInstance = g_panelInst;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.lpszClassName = L"ppluginScriptViewer";
        RegisterClassExW(&wc);   // ERROR_CLASS_ALREADY_EXISTS tolerated

        const int wpx = static_cast<int>(kViewW * g_scale + 0.5f);
        const int hpx = static_cast<int>(
            (kTitleH + kViewRows * kViewLineH + 2.0f * kPad) * g_scale + 0.5f);
        int x = CW_USEDEFAULT;
        int y = CW_USEDEFAULT;
        if (g_panel != nullptr && IsWindow(g_panel)) {
            RECT pr = {};
            GetWindowRect(g_panel, &pr);
            x = pr.left - wpx - static_cast<int>(8.0f * g_scale + 0.5f);
            y = pr.top;
            if (x < 0) x = pr.right + static_cast<int>(8.0f * g_scale + 0.5f);
        }
        g_viewer = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            L"ppluginScriptViewer", L"", WS_POPUP, x, y, wpx, hpx,
            g_owner, nullptr, g_panelInst, nullptr);
        if (g_viewer == nullptr) {
            LogFromOverlay("script: viewer window create failed");
            return;
        }
    } else {
        // follow the panel's current spot on every open
        RECT pr = {};
        if (g_panel != nullptr && IsWindow(g_panel) && GetWindowRect(g_panel, &pr)) {
            const int wpx = static_cast<int>(kViewW * g_scale + 0.5f);
            int x = pr.left - wpx - static_cast<int>(8.0f * g_scale + 0.5f);
            if (x < 0) x = pr.right + static_cast<int>(8.0f * g_scale + 0.5f);
            SetWindowPos(g_viewer, nullptr, x, pr.top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    ShowWindow(g_viewer, SW_SHOWNOACTIVATE);
    PaintView();
}

static LRESULT CALLBACK ScriptPanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN: {
        POINT pt = {};
        ClientLogical(hwnd, &pt);
        const float lx = static_cast<float>(pt.x);
        const float ly = static_cast<float>(pt.y);
        if (InClose(lx, ly)) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        if (InTitle(ly)) {
            POINT cur = {};
            GetCursorPos(&cur);
            RECT wr = {};
            GetWindowRect(hwnd, &wr);
            g_grab.x = cur.x - wr.left;
            g_grab.y = cur.y - wr.top;
            g_dragging = true;
            SetCapture(hwnd);
            return 0;
        }
        const int b = HitButton(lx, ly);
        if (b >= 0) {
            OnButton(hwnd, b);
            Paint();
            return 0;
        }
        const int r = HitRow(lx, ly);
        if (r >= 0) {
            g_rows[r].checked = !g_rows[r].checked;
            g_dirty = true;
            Paint();
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_dragging) {
            g_dragging = false;
            ReleaseCapture();
        }
        return 0;
    case WM_LBUTTONDBLCLK: {
        POINT pt = {};
        ClientLogical(hwnd, &pt);
        OpenViewer(HitRow(static_cast<float>(pt.x), static_cast<float>(pt.y)));
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_dragging) {
            POINT cur = {};
            GetCursorPos(&cur);
            SetWindowPos(hwnd, nullptr, cur.x - g_grab.x, cur.y - g_grab.y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        POINT pt = {};
        ClientLogical(hwnd, &pt);
        const float lx = static_cast<float>(pt.x);
        const float ly = static_cast<float>(pt.y);
        const bool title = InTitle(ly) && !InClose(lx, ly);
        const bool close = InClose(lx, ly);
        const int btn = HitButton(lx, ly);
        const int row = HitRow(lx, ly);
        TRACKMOUSEEVENT tme = {};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        if (title != g_titleHover || close != g_closeHover ||
            btn != g_hoverBtn || row != g_hoverRow) {
            g_titleHover = title;
            g_closeHover = close;
            g_hoverBtn = btn;
            g_hoverRow = row;
            g_dirty = true;
            Paint();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_titleHover || g_closeHover || g_hoverBtn >= 0 || g_hoverRow >= 0) {
            g_titleHover = false;
            g_closeHover = false;
            g_hoverBtn = -1;
            g_hoverRow = -1;
            g_dirty = true;
            Paint();
        }
        return 0;
    case WM_MOUSEWHEEL: {
        if (g_rowCount <= g_visRows) return 0;
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int ns = g_scroll + ((delta < 0) ? 3 : -3);
        const int mx = g_rowCount - g_visRows;
        if (ns > mx) ns = mx;
        if (ns < 0) ns = 0;
        if (ns != g_scroll) {
            g_scroll = ns;
            g_dirty = true;
            Paint();
        }
        return 0;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt = {};
            ClientLogical(hwnd, &pt);
            if (InTitle(static_cast<float>(pt.y))) {
                SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32646)));  // SIZEALL
                return TRUE;
            }
        }
        break;
    }
    case WM_TIMER:
        if (wp == 1) {
            OwnerFollow(hwnd);
            if (!IsWindow(hwnd)) return 0;
            UpdateStatuses();
            if (g_dirty) Paint();
        }
        return 0;
    case WM_RBUTTONDOWN:
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_ERASEBKGND:
        return 1;   // layered window paints itself
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI ScriptPanelThread(LPVOID) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &ScriptPanelProc;
    wc.hInstance = g_panelInst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.lpszClassName = L"ppluginScriptPanel";
    RegisterClassExW(&wc);

    // Fonts on the panel thread: rows use the YaHei UI face for Chinese, the
    // title bar keeps the console panel's Consolas.
    if (SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(g_dw.GetAddressOf())))) {
        g_dw->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, kFontSize * g_scale, L"zh-cn",
            g_fmt.GetAddressOf());
        g_dw->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, kFontSize * g_scale, L"zh-cn",
            g_centerFmt.GetAddressOf());
        g_dw->CreateTextFormat(L"Consolas", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, kFontSize * g_scale, L"en-us",
            g_titleFmt.GetAddressOf());
        if (g_fmt != nullptr) {
            g_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            g_fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            g_fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        if (g_centerFmt != nullptr) {
            g_centerFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            g_centerFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            g_centerFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        if (g_titleFmt != nullptr) {
            g_titleFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            g_titleFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            g_titleFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        // viewer body: monospace JSON
        g_dw->CreateTextFormat(L"Consolas", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 12.0f * g_scale, L"en-us",
            g_monoFmt.GetAddressOf());
        if (g_monoFmt != nullptr) {
            g_monoFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            g_monoFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            g_monoFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }

    // Anchor to the app's top-right, one console-panel height down so the two
    // panels do not overlap on start; draggable anywhere afterwards.
    const int wpx = static_cast<int>(kPanelW * g_scale + 0.5f);
    const int hpx = static_cast<int>(
        (kTitleH + kToolH + 2.0f * kPad + g_visRows * kRowH) * g_scale + 0.5f);
    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    HWND main = (g_host != nullptr) ? g_host->mainWindow() : nullptr;
    if (main != nullptr && !IsIconic(main)) {
        RECT prc = {};
        POINT origin = {0, 0};
        GetClientRect(main, &prc);
        ClientToScreen(main, &origin);
        x = origin.x + prc.right - wpx - static_cast<int>(kMargin * g_scale + 0.5f);
        y = origin.y + static_cast<int>((kMargin + 340.0f) * g_scale + 0.5f);
        g_owner = main;
        g_ownerOrigin = origin;
    }

    g_panel = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"ppluginScriptPanel", L"", WS_POPUP, x, y, wpx, hpx,
        main, nullptr, g_panelInst, nullptr);   // main is the owner, not parent
    if (g_panel == nullptr) {
        LogFromOverlay("script: panel window create failed");
        g_monoFmt.Reset();
        g_titleFmt.Reset();
        g_centerFmt.Reset();
        g_fmt.Reset();
        g_dw.Reset();
        UnregisterClassW(L"ppluginScriptPanel", g_panelInst);
        return 0;
    }

    EnumerateScripts();
    ShowWindow(g_panel, SW_SHOW);
    SetTimer(g_panel, 1, 150, nullptr);
    LogFromOverlay("script: panel started");

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_panel != nullptr) {
        DestroyWindow(g_panel);   // own thread, as required
        g_panel = nullptr;
    }
    if (g_viewer != nullptr) {
        DestroyWindow(g_viewer);
        g_viewer = nullptr;
    }
    ViewReleaseSurfaces();
    ReleaseSurfaces();
    g_monoFmt.Reset();
    g_titleFmt.Reset();
    g_centerFmt.Reset();
    g_fmt.Reset();
    g_dw.Reset();
    UnregisterClassW(L"ppluginScriptPanel", g_panelInst);
    UnregisterClassW(L"ppluginScriptViewer", g_panelInst);
    return 0;
}

static void StartPanel() {
    if (g_panelThread != nullptr) return;
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&StartPanel), &mod)) {
        LogFromOverlay("script: panel module resolve failed");
        return;
    }
    g_panelInst = reinterpret_cast<HINSTANCE>(mod);
    InterlockedExchange(&g_panelStop, 0);
    g_panelThread = CreateThread(nullptr, 0, &ScriptPanelThread, nullptr, 0, &g_panelTid);
    if (g_panelThread == nullptr) {
        LogFromOverlay("script: panel thread create failed");
    }
}

static void StopPanel() {
    if (g_panelThread == nullptr) return;
    InterlockedExchange(&g_panelStop, 1);
    PostThreadMessageW(g_panelTid, WM_QUIT, 0, 0);
    WaitForSingleObject(g_panelThread, 2000);
    CloseHandle(g_panelThread);
    g_panelThread = nullptr;
    g_panel = nullptr;
    g_owner = nullptr;
}

// Toggle panel visibility from the optional panel hotkey.
static void __cdecl OnPanelKey(int vk, void*) {
    (void)vk;
    if (g_panel == nullptr) return;
    ShowWindow(g_panel, IsWindowVisible(g_panel) ? SW_HIDE : SW_SHOW);
}

// ============================================================================
// Plugin entry points
// ============================================================================

static int __cdecl OnPluginInit(const Host* host) {
    if (host == nullptr || host->abi != P_ABI ||
        host->structSize < static_cast<uint32_t>(sizeof(Host)) ||
        host->log == nullptr || host->submitTask == nullptr) {
        return ERR_PARAM;
    }
    g_host = host;

    InitializeCriticalSection(&g_queueLock);
    LoadConfig();
    const bool viewOk = RefreshView();
    ScriptReload(true);
    StartPanel();

    if (g_script.hotkeyVk > 0) {
        if (host->addHotkey(g_script.hotkeyVk, &OnHotkey, nullptr) == P_OK) {
            char hk[96] = {};
            sprintf_s(hk, "script: hotkey vk=0x%02X registered",
                      static_cast<unsigned>(g_script.hotkeyVk));
            LogFromOverlay(hk);
        } else {
            LogFromOverlay("script: addHotkey rejected, hotkey disabled");
        }
    }
    if (g_panelVk > 0) {
        if (host->addHotkey(g_panelVk, &OnPanelKey, nullptr) == P_OK) {
            char hk[96] = {};
            sprintf_s(hk, "script: panel hotkey vk=0x%02X registered",
                      static_cast<unsigned>(g_panelVk));
            LogFromOverlay(hk);
        } else {
            LogFromOverlay("script: panel hotkey rejected");
        }
    }

    char line[256] = {};
    sprintf_s(line, "script: init abi=%u scale=%.2f view=%s events=%d loop=%d",
              host->abi, g_scale, viewOk ? "found" : "missing", g_script.count,
              g_script.loop);
    LogFromOverlay(line);
    return P_OK;
}

static int __cdecl OnPluginTick(void) {
    if (g_host == nullptr) {
        return 0;
    }
    const ULONGLONG now = GetTickCount64();

    // Stop request from the panel: drop the queue, kill the stepped run. The
    // runner holds no thread, so clearing the stage is the whole cancel.
    if (InterlockedExchange(&g_stopRequested, 0) != 0) {
        QueueClear();
        if (g_run.stage != kStageIdle) {
            g_run.stage = kStageIdle;
            InterlockedExchange(&g_scriptActive, 0);
            g_runningName[0] = 0;
            LogFromOverlay("script: stopped by user");
        } else {
            LogFromOverlay("script: stop ignored, nothing running");
        }
        InterlockedExchange(&g_stopAck, 1);
    }

    // A Run request from the panel (which runs on its own thread without the
    // framework's slot context) is serviced here, where g_activeSlot is set.
    // Serviced before the queue so the first checked script does not get
    // skipped in favor of its queued siblings.
    if (InterlockedExchange(&g_runRequested, 0) != 0) {
        if (g_requestedName[0] != 0) {
            wcscpy_s(g_scriptName, g_requestedName);   // force a specific script
            RunNow(true);
            g_scriptName[0] = 0;                       // transient, restore default
        } else {
            RunNow(true);
        }
    }

    // Idle -> start the next checked script, one per tick.
    if (g_run.stage == kStageIdle &&
        InterlockedCompareExchange(&g_scriptActive, 0, 0) == 0) {
        wchar_t next[64] = {};
        if (QueuePop(next, 64)) {
            wcscpy_s(g_scriptName, next);
            RunNow(true);
            g_scriptName[0] = 0;                       // transient, restore default
        }
    }

    // Cheap: one stat per second, same cadence as the proxy's 2s poll a bit
    // faster because a save in the editor should show up quickly.
    if (now - g_lastReload >= 1000) {
        g_lastReload = now;
        ScriptReload(false);
    }

    if (now - g_lastTrigger >= 300) {
        g_lastTrigger = now;
        PollTriggerFile();
    }
    return 0;
}

static int __cdecl OnPluginShutdown(void) {
    // The stepped runner holds no thread, so dropping the stage is the whole
    // cancel; queued steps were already revoked by the loader.
    g_run.stage = kStageIdle;
    InterlockedExchange(&g_scriptActive, 0);
    QueueClear();
    CdpShutdown();
    StopPanel();
    DeleteCriticalSection(&g_queueLock);

    if (g_host != nullptr && g_host->log != nullptr) {
        g_host->log("script: shutdown");
    }
    g_host = nullptr;
    g_hwnd = nullptr;
    g_viewHwnd = nullptr;
    return P_OK;
}

static const Desc kDesc = {
    P_ABI,
    static_cast<uint32_t>(sizeof(Desc)),
    "script",
    "0.4.0",
    &OnPluginInit,
    &OnPluginTick,
    &OnPluginShutdown
};

extern "C" __declspec(dllexport) const Desc* __cdecl GetDesc(void) {
    return &kDesc;
}

// The framework owns every hook and thread. An empty DllMain is correct.
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    (void)module;
    (void)reason;
    return TRUE;
}
