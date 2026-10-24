// ============================================================================
// script_plugin.cpp - runs the click scripts written by script_editor.py.
//
// Install: plugins\script\ (plugin.json + script.dll). The framework inside
//          version.dll discovers it, loads it and owns every thread - no
//          injection, no host process support, no DllMain work.
//
// Engine:  ../../proxy/script_engine.inc is included UNCHANGED, so the
//          JSON contract, file selection, click/wheel/paste helpers and the gate
//          math are literally the same code the proxy runs. Only the two things
//          a plugin must not own are replaced:
//            - its run thread          -> stepped through submitTask, one step
//                                         per call, so no tick ever blocks;
//            - its RegisterHotKey loop -> removed; the JSON hotkey is booked
//                                         through Host::addHotkey and dispatched
//                                         by the framework (Phase 1).
//
// Trigger: 1) the script's "hotkey" field, booked via Host::addHotkey and
//             dispatched by the framework (the key still reaches the app);
//          2) the panel's Run button / list-box double-click (serviced from
//             tick) or an optional panel toggle hotkey (<pluginDir>\config.ini panel_hotkey);
//          3) %APPDATA%\TTVoice\scripts\run.txt appears -> run, file deleted.
//
// Files:   %APPDATA%\TTVoice\scripts\<name>.json  editor output
//          %APPDATA%\TTVoice\scripts\active.txt   name saved last
//          <pluginDir>\config.ini                  panel_hotkey (gate_poll_ms fixed at 120ms)
//
// Note: the proxy's built-in engine no longer registers its own hotkey, so the
// framework dispatcher is the single owner of registration and dispatch.
// Keep this file pure ASCII.
// ============================================================================

#include "../sdk/pplugin.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static const Host* g_host = nullptr;

// ============================================================================
// Shims: what overlay.inc normally provides to script_engine.inc.
// ============================================================================

static HWND  g_hwnd = nullptr;       // pplugin top level window
static HWND  g_viewHwnd = nullptr;   // FLUTTERVIEW child (input + capture)
static float g_scale = 1.0f;         // view dpi / 96
static const int g_gatePollMs = 120; // fixed visual-gate capture interval (ms)
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
// Frame capture for wait_px (same DIB + PrintWindow path the coordinates were
// measured with, so the gate sees what the editor sees).
// ============================================================================

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

struct Shot {
    HDC     dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ oldBmp = nullptr;
    BYTE*   bits = nullptr;
    int     w = 0;
    int     h = 0;
};

static Shot g_shot;   // framework thread only (run steps + gate polls)

static void ShotFree() {
    if (g_shot.bmp != nullptr) {
        SelectObject(g_shot.dc, g_shot.oldBmp);
        DeleteObject(g_shot.bmp);
    }
    if (g_shot.dc != nullptr) {
        DeleteDC(g_shot.dc);
    }
    g_shot = Shot();
}

static bool CaptureView() {
    if (g_viewHwnd == nullptr) {
        return false;
    }

    RECT rc = {};
    if (!GetClientRect(g_viewHwnd, &rc)) {
        return false;
    }
    const int w = static_cast<int>(rc.right - rc.left);
    const int h = static_cast<int>(rc.bottom - rc.top);
    if (w <= 0 || h <= 0) {
        return false;
    }

    if (g_shot.w != w || g_shot.h != h) {
        if (g_shot.bmp != nullptr) {
            SelectObject(g_shot.dc, g_shot.oldBmp);
            DeleteObject(g_shot.bmp);
            g_shot.bmp = nullptr;
            g_shot.bits = nullptr;
        }
        if (g_shot.dc == nullptr) {
            g_shot.dc = CreateCompatibleDC(nullptr);
        }
        if (g_shot.dc == nullptr) {
            return false;
        }

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        g_shot.bmp = CreateDIBSection(g_shot.dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (g_shot.bmp == nullptr || bits == nullptr) {
            if (g_shot.bmp != nullptr) {
                DeleteObject(g_shot.bmp);
                g_shot.bmp = nullptr;
            }
            g_shot.bits = nullptr;
            return false;
        }
        g_shot.bits = static_cast<BYTE*>(bits);
        g_shot.oldBmp = SelectObject(g_shot.dc, g_shot.bmp);
        g_shot.w = w;
        g_shot.h = h;
    }

    return PrintWindow(g_viewHwnd, g_shot.dc, PW_RENDERFULLCONTENT) != FALSE;
}

// Cream pixel count in a +-200 x +-40 physical band around (lx,ly); centroid out.
static int CountCreamBand(float lx, float ly, LONG* outX, LONG* outY) {
    if (g_shot.bits == nullptr) {
        return 0;
    }

    LONG x0 = static_cast<LONG>(lx * g_scale) - 200;
    LONG y0 = static_cast<LONG>(ly * g_scale) - 40;
    LONG x1 = x0 + 400;
    LONG y1 = y0 + 80;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_shot.w - 1) x1 = g_shot.w - 1;
    if (y1 > g_shot.h - 1) y1 = g_shot.h - 1;
    if (x0 > x1 || y0 > y1) return 0;

    long long sumX = 0;
    long long sumY = 0;
    int hits = 0;

    for (LONG y = y0; y <= y1; ++y) {
        const BYTE* row = g_shot.bits +
                          static_cast<size_t>(y) * static_cast<size_t>(g_shot.w) * 4;
        for (LONG x = x0; x <= x1; ++x) {
            const BYTE* p = row + static_cast<size_t>(x) * 4;   // B,G,R,A
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
        if (outX != nullptr) *outX = static_cast<LONG>(sumX / hits);
        if (outY != nullptr) *outY = static_cast<LONG>(sumY / hits);
    }
    return hits;
}

// ============================================================================
// The engine, unchanged: JSON parse, file selection, reload policy, clicks,
// wheel, paste, gate helper, and the run thread we deliberately do not use.
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
// overlay.ini). panel_hotkey is the only key today; gate_poll_ms is a fixed
// constant. A template is written on first run so the key is discoverable.
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
// 4s gate wait blocks nothing.
// ============================================================================

enum RunStage {
    kStageIdle = 0,
    kStageDelay,   // inter-row delay_ms
    kStageGate,    // wait_px, one capture per gate_poll_ms
    kStageAct      // wheel / paste / click
};

struct RunState {
    Script    script;
    int       round = 0;
    int       index = 0;
    int       stage = kStageIdle;
    int       needPx = 0;
    int       stable = 0;
    int       maxHits = 0;
    ULONGLONG start = 0;
    ULONGLONG deadline = 0;
    ULONGLONG nextPoll = 0;
};

static RunState g_run;
static ULONGLONG g_lastReload = 0;
static ULONGLONG g_lastTrigger = 0;

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
    const ScriptEvent& ev = g_run.script.events[g_run.index];
    g_run.needPx = (ev.waitPx > 0) ? ev.waitPx : 0;
    if (g_run.needPx > 0) {
        g_run.stage = kStageGate;
        g_run.stable = 0;
        g_run.maxHits = 0;
        g_run.nextPoll = 0;
        g_run.start = GetTickCount64();
        g_run.deadline = g_run.start + static_cast<ULONGLONG>(
            (ev.waitMaxMs > 0) ? ev.waitMaxMs : 4000);
        return;
    }
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

// Applies one event's actions (wheel -> paste -> click, the engine's order).
static void ActEvent() {
    const ScriptEvent& ev = g_run.script.events[g_run.index];

    if (ev.wheel != 0) {
        SimulateWheelAt(ev.x, ev.y, ev.wheel);
    }
    if (ev.text[0]) {
        PasteTextToApp(ev.text);
    }
    SimulateClickKindAt(ev.x, ev.y, ev.click);

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

// One gate capture. Returns after re-arming itself until hit or timeout.
static void GateEvent() {
    const ULONGLONG now = GetTickCount64();
    const ScriptEvent& ev = g_run.script.events[g_run.index];

    if (now < g_run.nextPoll) {
        ScheduleStep();
        return;
    }
    g_run.nextPoll = now + static_cast<ULONGLONG>((g_gatePollMs < 30) ? 30 : g_gatePollMs);

    if (CaptureView()) {
        LONG bx = 0;
        LONG by = 0;
        const int hits = CountCreamBand(ev.x, ev.y, &bx, &by);
        if (hits > g_run.maxHits) {
            g_run.maxHits = hits;
        }
        if (hits >= g_run.needPx) {
            if (++g_run.stable >= 3) {
                char line[160] = {};
                sprintf_s(line, "script: gate hit %dpx at %ld,%ld after %llums", hits, bx, by,
                          static_cast<unsigned long long>(GetTickCount64() - g_run.start));
                LogFromOverlay(line);
                g_run.stage = kStageAct;
                ScheduleStep();
                return;
            }
        } else {
            g_run.stable = 0;
        }
    }

    if (GetTickCount64() >= g_run.deadline) {
        char line[160] = {};
        sprintf_s(line, "script: gate timeout %lums (max %dpx)",
                  static_cast<unsigned long>(g_run.deadline - g_run.start),
                  g_run.maxHits);
        LogFromOverlay(line);
        g_run.stage = kStageAct;   // a timeout still runs the row (engine parity)
        ScheduleStep();
        return;
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
    case kStageGate:
        GateEvent();
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

    g_run.script = g_script;   // detached copy, a reload cannot tear it
    g_run.round = 1;
    g_run.index = 0;
    g_run.start = GetTickCount64();
    InterlockedExchange(&g_scriptActive, 1);

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
// Self-owned Win32 panel (Phase 2): script list + Run / Reload / Hide.
//
// Runs on its own thread so message pumping never blocks the framework loader
// thread. The panel does not call Host::submitTask directly: that needs the
// framework's slot context, which is only set during framework callbacks. A Run
// request is flagged here and serviced from OnPluginTick, which runs with the
// context set. Host::log works from any thread (logs as "[plugin:?] ...").
// ============================================================================

static HWND   g_panel = nullptr;
static HWND   g_list = nullptr;
static HANDLE g_panelThread = nullptr;
static DWORD  g_panelTid = 0;
static volatile LONG g_panelStop = 0;
static volatile LONG g_runRequested = 0;
static wchar_t g_requestedName[64] = {};   // selected script base name

static const int IDC_LIST = 100;
static const int IDC_RUN = 101;
static const int IDC_RELOAD = 102;
static const int IDC_HIDE = 103;

static void EnumerateScripts(HWND list) {
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    wchar_t appdata[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0) return;
    wchar_t pattern[MAX_PATH] = {};
    swprintf_s(pattern, L"%s\\TTVoice\\scripts\\*.json", appdata);
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wchar_t base[260] = {};
        wcscpy_s(base, fd.cFileName);
        wchar_t* dot = wcsrchr(base, L'.');
        if (dot != nullptr) *dot = 0;
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(base));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Flags a run of the current list selection; serviced from OnPluginTick.
static void RequestRunBySelection() {
    wchar_t buf[260] = {};
    int sel = static_cast<int>(SendMessageW(g_list, LB_GETCURSEL, 0, 0));
    if (sel >= 0) {
        SendMessageW(g_list, LB_GETTEXT, static_cast<WPARAM>(sel),
                     reinterpret_cast<LPARAM>(buf));
    }
    wcscpy_s(g_requestedName, buf);   // empty when nothing selected -> default run
    InterlockedExchange(&g_runRequested, 1);
}

static LRESULT CALLBACK ScriptPanelProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_list = CreateWindowExW(0, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | WS_VSCROLL,
            8, 8, 264, 200, w, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)),
            GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"Run", WS_CHILD | WS_VISIBLE,
            8, 216, 84, 26, w, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RUN)),
            GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"Reload", WS_CHILD | WS_VISIBLE,
            100, 216, 84, 26, w, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RELOAD)),
            GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"Hide", WS_CHILD | WS_VISIBLE,
            192, 216, 84, 26, w, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HIDE)),
            GetModuleHandleW(nullptr), nullptr);
        EnumerateScripts(g_list);
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == IDC_LIST && HIWORD(wp) == LBN_DBLCLK) {
            RequestRunBySelection();
        } else if (id == IDC_RUN) {
            RequestRunBySelection();
        } else if (id == IDC_RELOAD) {
            EnumerateScripts(g_list);
            ScriptReload(false);
        } else if (id == IDC_HIDE) {
            ShowWindow(w, SW_HIDE);
        }
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(w, SW_HIDE);   // keep the thread alive; re-show via hotkey
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static DWORD WINAPI ScriptPanelThread(LPVOID) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &ScriptPanelProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ppluginScriptPanel";
    RegisterClassExW(&wc);

    if (g_panelStop) {
        UnregisterClassW(L"ppluginScriptPanel", wc.hInstance);
        return 0;
    }

    g_panel = CreateWindowExW(0, L"ppluginScriptPanel", L"pplugin Script Manager",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              300, 280, nullptr, nullptr, wc.hInstance, nullptr);
    if (g_panel == nullptr) {
        UnregisterClassW(L"ppluginScriptPanel", wc.hInstance);
        return 0;
    }
    ShowWindow(g_panel, SW_SHOW);
    LogFromOverlay("script: panel started");

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_panel = nullptr;
    UnregisterClassW(L"ppluginScriptPanel", wc.hInstance);
    return 0;
}

static void StartPanel() {
    if (g_panelThread != nullptr) return;
    InterlockedExchange(&g_panelStop, 0);
    g_panelThread = CreateThread(nullptr, 0, &ScriptPanelThread, nullptr, 0, &g_panelTid);
    if (g_panelThread == nullptr) {
        LogFromOverlay("script: panel thread create failed");
    }
}

static void StopPanel() {
    if (g_panelThread == nullptr) return;
    InterlockedExchange(&g_panelStop, 1);
    if (g_panel != nullptr) {
        PostMessageW(g_panel, WM_DESTROY, 0, 0);
    }
    PostThreadMessageW(g_panelTid, WM_QUIT, 0, 0);
    WaitForSingleObject(g_panelThread, 2000);
    CloseHandle(g_panelThread);
    g_panelThread = nullptr;
    g_panel = nullptr;
}

// Toggle panel visibility from the optional panel hotkey.
static void __cdecl OnPanelKey(int vk, void*) {
    (void)vk;
    if (g_panel == nullptr) return;
    if (IsWindowVisible(g_panel)) {
        ShowWindow(g_panel, SW_HIDE);
    } else {
        ShowWindow(g_panel, SW_SHOW);
        SetForegroundWindow(g_panel);
    }
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

    // A Run request from the panel (which runs on its own thread without the
    // framework's slot context) is serviced here, where g_activeSlot is set.
    if (InterlockedExchange(&g_runRequested, 0) != 0) {
        if (g_requestedName[0] != 0) {
            wcscpy_s(g_scriptName, g_requestedName);   // force a specific script
            RunNow(true);
            g_scriptName[0] = 0;                       // transient, restore default
        } else {
            RunNow(true);
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
    StopPanel();
    ShotFree();

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
    "0.1.0",
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
