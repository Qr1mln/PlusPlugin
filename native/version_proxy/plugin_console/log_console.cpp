// ============================================================================
// log_console.cpp - CMD style log window, implemented as a plugin.
//
// What it proves about the ABI:
//   - the framework pushes every log line to subscribed plugins (addLogSink),
//     so a viewer is just another plugin, not a framework feature;
//   - the plugin owns no thread: its window messages are pumped from
//     TTPluginDesc::tick, which the framework calls on its loader thread;
//   - unloading is clean because the only resources are a window, a font and
//     a static ring buffer. No heap, no thread, no hook.
//
// Threading: the log sink may run on any framework thread, so it only writes
// into the ring under a lock and posts a wake-up message. The window is only
// touched from tick, which runs on the thread that created it.
//
// Keep this file pure ASCII.
// ============================================================================

#include "../sdk/ttplugin.h"
#include <cstdio>
#include <cstring>

namespace {

const int kRingLines      = 256;   // buffered lines between sink and window
const int kRingLineChars  = 256;   // per line, including terminator
const int kWindowWidth    = 860;
const int kWindowHeight   = 480;
const int kEditCharLimit  = 400000;  // trim the edit control past this size
const int kFontHeight     = -14;     // logical units, negative = char height
const UINT kWmPushLine    = WM_APP + 1;

const wchar_t* const kClassName   = L"TTVoiceLogConsole";
const wchar_t* const kWindowTitle = L"TTVoice log";

struct RingEntry {
    char text[kRingLineChars];
};

RingEntry g_ring[kRingLines];
int g_head = 0;                 // next write position
int g_tail = 0;                 // next unread position
SRWLOCK g_ringLock = SRWLOCK_INIT;

HWND g_frame = nullptr;
HWND g_edit = nullptr;
HFONT g_font = nullptr;
const TTPluginHost* g_host = nullptr;
bool g_classRegistered = false;

LRESULT CALLBACK ConsoleProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// @brief Module handle of this plugin, without bumping its reference count.
// @return Own HMODULE, or null.
// @note UNCHANGED_REFCOUNT is mandatory: a plain GetModuleHandleEx would add a
//       reference and the framework's FreeLibrary could never unload us.
HMODULE OwnModule() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ConsoleProc), &module);
    return module;
}

// @brief Drains buffered lines into the edit control. Window thread only.
void AppendPending() {
    if (g_edit == nullptr) {
        return;
    }
    for (;;) {
        RingEntry copy = {};
        AcquireSRWLockExclusive(&g_ringLock);
        if (g_tail >= g_head) {
            ReleaseSRWLockExclusive(&g_ringLock);
            return;
        }
        copy = g_ring[g_tail % kRingLines];
        ++g_tail;
        ReleaseSRWLockExclusive(&g_ringLock);

        wchar_t wide[kRingLineChars] = {};
        MultiByteToWideChar(CP_UTF8, 0, copy.text, -1, wide, kRingLineChars);

        if (GetWindowTextLengthW(g_edit) > kEditCharLimit) {
            SetWindowTextW(g_edit, L"");  // keep memory bounded, lose history
        }

        wchar_t line[kRingLineChars + 2] = {};
        swprintf_s(line, L"%s\r\n", wide);

        const int length = GetWindowTextLengthW(g_edit);
        SendMessageW(g_edit, EM_SETSEL, static_cast<WPARAM>(length),
                     static_cast<LPARAM>(length));
        SendMessageW(g_edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line));
        SendMessageW(g_edit, EM_SCROLLCARET, 0, 0);
    }
}

// @brief Framework log sink. Runs on an arbitrary thread, so it must not touch
//        any window state directly.
void __cdecl OnLogLine(const char* line, void*) {
    if (line == nullptr) {
        return;
    }
    HWND frame = g_frame;
    if (frame == nullptr) {
        return;
    }

    AcquireSRWLockExclusive(&g_ringLock);
    strncpy_s(g_ring[g_head % kRingLines].text, kRingLineChars, line, _TRUNCATE);
    ++g_head;
    if (g_head - g_tail > kRingLines) {
        g_tail = g_head - kRingLines;  // drop the oldest line
    }
    ReleaseSRWLockExclusive(&g_ringLock);

    PostMessageW(frame, kWmPushLine, 0, 0);
}

LRESULT CALLBACK ConsoleProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case kWmPushLine:
        AppendPending();
        return 0;

    case WM_SIZE:
        if (g_edit != nullptr) {
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            SetWindowPos(g_edit, nullptr, 0, 0, rc.right, rc.bottom, SWP_NOZORDER);
        }
        return 0;

    case WM_CLOSE:
        // Closing the viewer must never end the host process. Hide instead.
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        g_edit = nullptr;
        g_frame = nullptr;
        return 0;

    default:
        break;
    }
    (void)wp;
    (void)lp;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// @brief Creates the console window on the calling thread (the loader thread).
// @return true when the window and edit control exist.
bool CreateConsole() {
    HMODULE module = OwnModule();
    if (module == nullptr) {
        return false;
    }

    if (!g_classRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &ConsoleProc;
        wc.hInstance = module;
        wc.lpszClassName = kClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        if (RegisterClassExW(&wc) == 0) {
            return false;
        }
        g_classRegistered = true;
    }

    g_frame = CreateWindowExW(0, kClassName, kWindowTitle, WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, kWindowWidth,
                              kWindowHeight, nullptr, nullptr, module, nullptr);
    if (g_frame == nullptr) {
        return false;
    }

    RECT rc = {};
    GetClientRect(g_frame, &rc);
    g_edit = CreateWindowExW(0, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                                 ES_READONLY | ES_AUTOVSCROLL,
                             0, 0, rc.right, rc.bottom, g_frame, nullptr, module,
                             nullptr);
    if (g_edit == nullptr) {
        DestroyWindow(g_frame);
        g_frame = nullptr;
        return false;
    }

    g_font = CreateFontW(kFontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (g_font != nullptr) {
        SendMessageW(g_edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    }
    SendMessageW(g_edit, EM_SETLIMITTEXT, static_cast<WPARAM>(kEditCharLimit), 0);
    ShowWindow(g_frame, SW_SHOW);
    return true;
}

void DestroyConsole() {
    if (g_frame != nullptr) {
        DestroyWindow(g_frame);  // WM_DESTROY clears g_frame and g_edit
    }
    g_frame = nullptr;
    g_edit = nullptr;
    if (g_font != nullptr) {
        DeleteObject(g_font);
        g_font = nullptr;
    }
    if (g_classRegistered) {
        UnregisterClassW(kClassName, OwnModule());
        g_classRegistered = false;
    }
}

// ---------------------------------------------------------------------------
// Plugin entry points.
// ---------------------------------------------------------------------------
int __cdecl OnPluginInit(const TTPluginHost* host) {
    g_host = host;
    if (host == nullptr) {
        return TTPLUGIN_ERR_PARAM;
    }

    // addLogSink sits after query in the table. Refuse instead of reading past
    // the end of an older framework build.
    if (host->structSize < sizeof(TTPluginHost)) {
        if (host->log != nullptr) {
            host->log("console: framework too old, no log sink");
        }
        g_host = nullptr;
        return TTPLUGIN_ERR_STATE;
    }

    if (!CreateConsole()) {
        if (host->log != nullptr) {
            host->log("console: window creation failed");
        }
        g_host = nullptr;
        return TTPLUGIN_ERR_STATE;
    }

    const int rc = host->addLogSink(&OnLogLine, nullptr);
    if (rc != TTPLUGIN_OK) {
        if (host->log != nullptr) {
            host->log("console: log sink rejected");
        }
        DestroyConsole();
        g_host = nullptr;
        return rc;
    }

    host->log("console: attached, window is live");
    return TTPLUGIN_OK;
}

// @brief Message pump for the console window. Runs on the loader thread.
int __cdecl OnPluginTick(void) {
    MSG msg = {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

int __cdecl OnPluginShutdown(void) {
    if (g_host != nullptr && g_host->log != nullptr) {
        g_host->log("console: detaching");
    }
    g_host = nullptr;

    DestroyConsole();

    // Drop buffered lines so a reload starts clean.
    AcquireSRWLockExclusive(&g_ringLock);
    g_head = 0;
    g_tail = 0;
    ReleaseSRWLockExclusive(&g_ringLock);
    return TTPLUGIN_OK;
}

const TTPluginDesc kDesc = {
    TTPLUGIN_ABI,
    static_cast<uint32_t>(sizeof(TTPluginDesc)),
    "console",
    "0.1.0",
    &OnPluginInit,
    &OnPluginTick,
    &OnPluginShutdown
};

}  // namespace

extern "C" __declspec(dllexport) const TTPluginDesc* __cdecl TTPlugin_GetDesc(void) {
    return &kDesc;
}

// Empty by design: every resource is created in init and released in shutdown.
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    (void)module;
    (void)reason;
    return TRUE;
}
