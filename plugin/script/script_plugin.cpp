// ============================================================================
// script_plugin.cpp - runs the click scripts written by script_editor.py.
//
// Install: plugins\script\ (plugin.json + script.dll). The framework inside
//          version.dll discovers it, loads it and owns every thread - no
//          injection, no host process support, no DllMain work.
//
// Engine:  ../../version_proxy/script_engine.inc is included UNCHANGED, so the
//          JSON contract, file selection, click/wheel/paste helpers and the gate
//          math are literally the same code the proxy runs. Only the two things
//          a plugin must not own are replaced:
//            - its run thread          -> stepped through submitTask, one step
//                                         per call, so no tick ever blocks;
//            - its RegisterHotKey loop -> the JSON hotkey is polled in tick()
//                                         (the framework's addHotkey only
//                                         books entries today, nothing
//                                         dispatches them yet).
//
// Trigger: 1) the script's own "hotkey" field (polled; the key still reaches
//             the app because we cannot register it);
//          2) %APPDATA%\TTVoice\scripts\run.txt appears -> run, file deleted.
//
// Files:   %APPDATA%\TTVoice\scripts\<name>.json  editor output
//          %APPDATA%\TTVoice\scripts\active.txt   name saved last
//          %APPDATA%\TTVoice\overlay.ini          script= / gate_poll_ms only
//
// Note: with the proxy's built-in engine still compiled in, both engines see the
// same JSON. A JSON hotkey fires both (RegisterHotKey there, poll here), so
// leave "hotkey" empty while both are installed, or use run.txt.
// Keep this file pure ASCII.
// ============================================================================

#include "../sdk/ttplugin.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static const TTPluginHost* g_host = nullptr;

// ============================================================================
// Shims: what overlay.inc normally provides to script_engine.inc.
// ============================================================================

static HWND  g_hwnd = nullptr;       // TTVoice top level window
static HWND  g_viewHwnd = nullptr;   // FLUTTERVIEW child (input + capture)
static float g_scale = 1.0f;         // view dpi / 96
static int   g_gatePollMs = 120;     // overlay.ini gate_poll_ms
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
#include "../../version_proxy/script_engine.inc"
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

// Reads only the two keys a plugin without an overlay needs.
static void LoadConfig() {
    wchar_t appdata[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0) {
        LogFromOverlay("script: APPDATA missing, defaults kept");
        return;
    }

    wchar_t path[MAX_PATH] = {};
    swprintf_s(path, L"%s\\TTVoice\\overlay.ini", appdata);

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path, L"rb") != 0 || fp == nullptr) {
        LogFromOverlay("script: no overlay.ini, selection = active.txt / newest json");
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
        if (eq == nullptr) continue;
        *eq = 0;
        const char* key = line;
        const char* val = eq + 1;
        while (*val == ' ' || *val == '\t') ++val;

        if (_stricmp(key, "gate_poll_ms") == 0) {
            const int n = atoi(val);
            g_gatePollMs = (n < 30) ? 30 : n;
        } else if (_stricmp(key, "script") == 0 && val[0] != 0) {
            Utf8ToWideN(val, g_scriptName, 64);   // forced name wins over active.txt
            char narrow[128] = {};
            sprintf_s(narrow, "script: forced script=%s", val);
            LogFromOverlay(narrow);
        }
    }

    char line[96] = {};
    sprintf_s(line, "script: gate_poll_ms=%d", g_gatePollMs);
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
static int g_hotkeyDown = 0;
static int g_hotkeyLogged = 0;

static void __cdecl RunStep(void* user);

static void ScheduleStep() {
    if (g_host == nullptr || g_host->submitTask == nullptr) {
        return;
    }
    if (g_host->submitTask(&RunStep, nullptr) != TTPLUGIN_OK) {
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
// Triggers polled from tick()
//
// The framework owns RegisterHotKey, but addHotkey only books an entry today
// (dispatch lands with M1), so a key is observed instead: GetAsyncKeyState is a
// read, no registration and no hook. run.txt is the trigger that needs no key at
// all and is what a script/test harness should use.
// ============================================================================

static void PollHotkey() {
    const int vk = g_script.hotkeyVk;
    if (vk <= 0) {
        g_hotkeyDown = 0;
        g_hotkeyLogged = 0;
        return;
    }
    if (vk != g_hotkeyLogged) {
        g_hotkeyLogged = vk;
        char line[128] = {};
        sprintf_s(line, "script: hotkey vk=0x%02X polled (addHotkey not dispatched yet)",
                  static_cast<unsigned>(vk));
        LogFromOverlay(line);
    }

    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    if (down && !g_hotkeyDown) {
        LogFromOverlay("script: hotkey pressed");
        RunNow(true);
    }
    g_hotkeyDown = down ? 1 : 0;
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
// Plugin entry points
// ============================================================================

static int __cdecl OnPluginInit(const TTPluginHost* host) {
    if (host == nullptr || host->abi != TTPLUGIN_ABI ||
        host->structSize < static_cast<uint32_t>(sizeof(TTPluginHost)) ||
        host->log == nullptr || host->submitTask == nullptr) {
        return TTPLUGIN_ERR_PARAM;
    }
    g_host = host;

    LoadConfig();
    const bool viewOk = RefreshView();
    ScriptReload(true);

    char line[256] = {};
    sprintf_s(line, "script: init abi=%u scale=%.2f view=%s events=%d loop=%d",
              host->abi, g_scale, viewOk ? "found" : "missing", g_script.count,
              g_script.loop);
    LogFromOverlay(line);
    return TTPLUGIN_OK;
}

static int __cdecl OnPluginTick(void) {
    if (g_host == nullptr) {
        return 0;
    }
    const ULONGLONG now = GetTickCount64();

    // Cheap: one stat per second, same cadence as the proxy's 2s poll a bit
    // faster because a save in the editor should show up quickly.
    if (now - g_lastReload >= 1000) {
        g_lastReload = now;
        ScriptReload(false);
    }
    PollHotkey();

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
    ShotFree();

    if (g_host != nullptr && g_host->log != nullptr) {
        g_host->log("script: shutdown");
    }
    g_host = nullptr;
    g_hwnd = nullptr;
    g_viewHwnd = nullptr;
    return TTPLUGIN_OK;
}

static const TTPluginDesc kDesc = {
    TTPLUGIN_ABI,
    static_cast<uint32_t>(sizeof(TTPluginDesc)),
    "script",
    "0.1.0",
    &OnPluginInit,
    &OnPluginTick,
    &OnPluginShutdown
};

extern "C" __declspec(dllexport) const TTPluginDesc* __cdecl TTPlugin_GetDesc(void) {
    return &kDesc;
}

// The framework owns every hook and thread. An empty DllMain is correct.
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    (void)module;
    (void)reason;
    return TRUE;
}
