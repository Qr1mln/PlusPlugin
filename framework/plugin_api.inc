// ============================================================================
// plugin_api.inc - host side of the Host capability table.
//
// Ownership rule: the framework owns every hook, every system registration
// point and every thread. A plugin only books callbacks here, so an unload can
// revoke the whole set from one place. Plugins never call these functions
// directly; they only see the Host table.
//
// Included by dllmain.cpp after overlay.inc, so it shares the translation unit
// (and therefore the static Log() helper) with the rest of the payload.
//
// Package: framework. Depends on the public contract in ../sdk/pplugin.h.
// Keep this file pure ASCII.
// ============================================================================

#ifndef PPLUGIN_PLUGIN_API_INC
#define PPLUGIN_PLUGIN_API_INC

#include "../sdk/pplugin.h"

namespace plugins {

// ---------------------------------------------------------------------------
// Limits and timings. No magic values are allowed past this block.
// ---------------------------------------------------------------------------
const int kMaxPlugins        = 16;   // resident plugins
const int kMaxTasksPerPlugin = 16;   // queued tasks per plugin
const int kTextShort         = 64;   // name buffer, including terminator
const int kTextVersion       = 32;   // version buffer, including terminator
const int kPathChars         = MAX_PATH;
const int kPathUtf8          = MAX_PATH * 3;

const int kDrainTimeoutMs = 2000; // wait for in flight calls before unload
const int kControlPollMs  = 500;  // control.txt poll interval
const int kTickIntervalMs = 16;   // plugin tick, also the UI message pump rate
const int kTasksPerPoll   = 8;    // task budget per poll, keeps the loop fair
const int kManifestMaxBytes = 64 * 1024;

// ---------------------------------------------------------------------------
// Plugin lifecycle. See docs for the state diagram.
// ---------------------------------------------------------------------------
enum class PluginState {
    kEmpty = 0,     // slot unused
    kDisabled,      // discovered but enabled=false in plugin.json
    kDiscovered,    // manifest parsed, DLL not mapped yet
    kLoading,       // inside Desc::init
    kActive,        // callable
    kDraining,      // no new calls, waiting for inflight to reach zero
    kQuarantined,   // stuck, stays mapped but is never called again
    kRejected,      // validation failed
    kUnloaded       // mapped out, slot may be reused
};

// Human readable state, for logs only.
static const char* StateName(PluginState state) {
    switch (state) {
    case PluginState::kEmpty:       return "empty";
    case PluginState::kDisabled:    return "disabled";
    case PluginState::kDiscovered:  return "discovered";
    case PluginState::kLoading:     return "loading";
    case PluginState::kActive:      return "active";
    case PluginState::kDraining:    return "draining";
    case PluginState::kQuarantined: return "quarantined";
    case PluginState::kRejected:    return "rejected";
    case PluginState::kUnloaded:    return "unloaded";
    default:                        return "unknown";
    }
}

// ---------------------------------------------------------------------------
// Bookkeeping records. One array per capability, owned by the framework.
// M0 stores them and revokes them; dispatch happens in M1.
// ---------------------------------------------------------------------------
struct HotkeyEntry {
    int vk = 0;
    int id = 0;          // RegisterHotKey id, unique within the dispatcher window
    HotkeyFn fn = nullptr;
    void* user = nullptr;
};

struct TaskEntry {
    TaskFn fn = nullptr;
    void* user = nullptr;
};

struct LogSinkEntry {
    LogFn fn = nullptr;
    void* user = nullptr;
};

// ---------------------------------------------------------------------------
// One resident plugin.
// ---------------------------------------------------------------------------
struct PluginSlot {
    PluginState state = PluginState::kEmpty;
    HMODULE module = nullptr;
    const Desc* desc = nullptr;

    bool autoLoad = true;
    int priority = 0;

    wchar_t dir[kPathChars] = {};
    wchar_t dllPath[kPathChars] = {};
    char dirUtf8[kPathUtf8] = {};
    char dllName[kPathChars] = {};
    char name[kTextShort] = {};
    char version[kTextVersion] = {};

    HotkeyEntry hotkeys[MAX_HOTKEYS] = {};
    int hotkeyCount = 0;

    TaskEntry tasks[kMaxTasksPerPlugin] = {};
    int taskHead = 0;
    int taskTail = 0;

    // One log subscription per plugin; revoked by ClearRegistrations().
    LogSinkEntry logSink;

    // Bumped around every callback that may run off the loader thread, so an
    // unload can prove that no plugin code is on a stack any more.
    LONG inflight = 0;

    // Hot reload bookkeeping: loader thread only. The stamps are what the files
    // looked like when we last acted on them; pending* is the newer state we
    // keep seeing while a deploy is still writing, and pendAt is when we first
    // saw it (used for the quiet period before acting).
    bool stampsValid = false;
    FILETIME jsonTime = {};
    DWORD    jsonSize = 0;
    FILETIME dllTime = {};
    DWORD    dllSize = 0;
    FILETIME pendJsonTime = {};
    DWORD    pendJsonSize = 0;
    FILETIME pendDllTime = {};
    DWORD    pendDllSize = 0;
    ULONGLONG pendAt = 0;
    bool      wantLoad = false;     // set when a reload failed and should retry
    ULONGLONG retryAt = 0;
    int       retries = 0;
};

// ---------------------------------------------------------------------------
// Shared state. All of it lives in the loader thread's world; only inflight is
// touched from other threads and only through Interlocked*.
// ---------------------------------------------------------------------------
static PluginSlot g_slots[kMaxPlugins];
static int g_slotCount = 0;
static thread_local int g_activeSlot = -1;   // slot whose callback runs on this thread
static HWND g_mainHwnd = nullptr;
static float g_uiScale = 1.0f;

// Derive the host's DPI scale from its main window. Resolved dynamically so we
// stay loadable on pre-Win8.1 (GetDpiForWindow is missing there -> fall back to
// 96 dpi = scale 1.0). This is the data source for Host::scale().
static float ComputeHostScale(HWND hwnd) {
    UINT dpi = 96;
    using PFN = UINT(WINAPI*)(HWND);
    static PFN getDpi = reinterpret_cast<PFN>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (getDpi && hwnd) {
        dpi = getDpi(hwnd);
    }
    return (dpi > 0) ? (static_cast<float>(dpi) / 96.0f) : 1.0f;
}

static bool g_started = false;

// ---------------------------------------------------------------------------
// Hotkey dispatcher.
//
// RegisterHotKey(hwnd, ...) posts WM_HOTKEY to the thread that created hwnd, so
// a message-only window on a dedicated thread owns every registration and pumps
// the queue. Plugins never see RegisterHotKey: they book via Host::addHotkey and
// get fn(vk) back here. UnregisterHotKey happens in ClearRegistrations on unload.
// ---------------------------------------------------------------------------
static HWND g_hotkeyHwnd = nullptr;
static HANDLE g_hotkeyReady = nullptr;
static const int kHotkeyIdBase = 0x2000;   // stays clear of engine ids (0x5454)

static void DispatchHotkey(int id) {
    for (int s = 0; s < g_slotCount; ++s) {
        PluginSlot& slot = g_slots[s];
        if (slot.state != PluginState::kActive) {
            continue;
        }
        for (int h = 0; h < slot.hotkeyCount; ++h) {
            if (slot.hotkeys[h].id != id) {
                continue;
            }
            const HotkeyEntry entry = slot.hotkeys[h];   // copy before guarding
            if (entry.fn == nullptr) {
                return;
            }
            InterlockedIncrement(&slot.inflight);
            if (slot.state == PluginState::kActive) {
                const int prev = g_activeSlot;
                g_activeSlot = s;   // Host::log / submitTask see the owner
                entry.fn(entry.vk, entry.user);
                g_activeSlot = prev;
            }
            InterlockedDecrement(&slot.inflight);
            return;
        }
    }
}

static LRESULT CALLBACK HotkeyWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_HOTKEY) {
        DispatchHotkey(static_cast<int>(wp));
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI HotkeyThreadProc(LPVOID) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &HotkeyWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PPluginHotkeyWnd";
    RegisterClassExW(&wc);
    g_hotkeyHwnd = CreateWindowExW(0, L"PPluginHotkeyWnd", L"", 0, 0, 0, 0, 0,
                                   HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (g_hotkeyReady != nullptr) {
        SetEvent(g_hotkeyReady);
    }
    if (g_hotkeyHwnd == nullptr) {
        return 0;
    }
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

static void StartHotkeyThread() {
    if (g_hotkeyHwnd != nullptr || g_hotkeyReady != nullptr) {
        return;
    }
    g_hotkeyReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread = CreateThread(nullptr, 0, &HotkeyThreadProc, nullptr, 0, nullptr);
    if (thread == nullptr) {
        Log("plugin: hotkey CreateThread failed err=%lu", GetLastError());
        if (g_hotkeyReady != nullptr) {
            CloseHandle(g_hotkeyReady);
            g_hotkeyReady = nullptr;
        }
        return;
    }
    CloseHandle(thread);
    if (g_hotkeyReady != nullptr) {
        WaitForSingleObject(g_hotkeyReady, 3000);
        CloseHandle(g_hotkeyReady);
        g_hotkeyReady = nullptr;
    }
    Log(g_hotkeyHwnd != nullptr ? "plugin: hotkey dispatcher ready"
                                : "plugin: hotkey dispatcher unavailable");
}

// ---------------------------------------------------------------------------
// Small helpers.
// ---------------------------------------------------------------------------
static void WideToUtf8(const wchar_t* src, char* dst, int cap) {
    if (dst == nullptr || cap <= 0) {
        return;
    }
    dst[0] = 0;
    if (src != nullptr) {
        WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, cap - 1, nullptr, nullptr);
    }
    dst[cap - 1] = 0;
}

static void Utf8ToWide(const char* src, wchar_t* dst, int cap) {
    if (dst == nullptr || cap <= 0) {
        return;
    }
    dst[0] = 0;
    if (src != nullptr && src[0] != 0) {
        MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, cap);
    }
    dst[cap - 1] = 0;
}

static const char* ActiveName() {
    if (g_activeSlot < 0 || g_activeSlot >= g_slotCount) {
        return "framework";
    }
    return g_slots[g_activeSlot].name;
}

static int FindPluginByName(const char* name) {
    if (name == nullptr || name[0] == 0) {
        return -1;
    }
    for (int i = 0; i < g_slotCount; ++i) {
        if (_stricmp(g_slots[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// The Host entry points.
// ---------------------------------------------------------------------------
static void __cdecl HostLog(const char* text) {
    Log("[plugin:%s] %s", ActiveName(), (text != nullptr) ? text : "(null)");
}

static const char* __cdecl HostPluginDir() {
    if (g_activeSlot < 0 || g_activeSlot >= g_slotCount) {
        return "";
    }
    return g_slots[g_activeSlot].dirUtf8;
}

static HWND __cdecl HostMainWindow() {
    return g_mainHwnd;
}

static HWND __cdecl HostViewWindow() {
    // M1 records the real child window from the present hook. Until then the
    // top level window is the only handle the framework can vouch for.
    return g_mainHwnd;
}

static float __cdecl HostScale() {
    return g_uiScale;
}

static int __cdecl HostAddHotkey(int vk, HotkeyFn fn, void* user) {
    if (g_activeSlot < 0 || fn == nullptr || vk <= 0) {
        return ERR_PARAM;
    }
    PluginSlot& slot = g_slots[g_activeSlot];
    if (slot.hotkeyCount >= MAX_HOTKEYS) {
        return ERR_FULL;
    }
    if (g_hotkeyHwnd == nullptr) {
        Log("plugin: %s addHotkey vk=0x%02X dropped (no dispatcher)", slot.name,
            static_cast<unsigned>(vk));
        return ERR_STATE;
    }
    const int id = kHotkeyIdBase + g_activeSlot * MAX_HOTKEYS + slot.hotkeyCount;
    if (!RegisterHotKey(g_hotkeyHwnd, id, 0, static_cast<UINT>(vk))) {
        Log("plugin: %s RegisterHotKey vk=0x%02X failed err=%lu", slot.name,
            static_cast<unsigned>(vk), GetLastError());
        return ERR_PARAM;
    }
    HotkeyEntry& entry = slot.hotkeys[slot.hotkeyCount++];
    entry.vk = vk;
    entry.id = id;
    entry.fn = fn;
    entry.user = user;
    Log("plugin: %s hotkey vk=0x%02X -> id=%d", slot.name,
        static_cast<unsigned>(vk), id);
    return P_OK;
}

static int __cdecl HostSubmitTask(TaskFn fn, void* user) {
    if (g_activeSlot < 0 || fn == nullptr) {
        return ERR_PARAM;
    }
    PluginSlot& slot = g_slots[g_activeSlot];
    const int used = slot.taskTail - slot.taskHead;
    if (used >= kMaxTasksPerPlugin) {
        return ERR_FULL;
    }
    TaskEntry& entry = slot.tasks[slot.taskTail % kMaxTasksPerPlugin];
    entry.fn = fn;
    entry.user = user;
    ++slot.taskTail;
    return P_OK;
}

static int __cdecl HostAddLogSink(LogFn fn, void* user) {
    if (g_activeSlot < 0 || fn == nullptr) {
        return ERR_PARAM;
    }
    PluginSlot& slot = g_slots[g_activeSlot];
    if (slot.logSink.fn != nullptr) {
        return ERR_FULL;
    }
    slot.logSink.fn = fn;
    slot.logSink.user = user;
    return P_OK;
}

// The table handed to plugins. Filled once, never mutated afterwards.
static const Host g_host = {
    P_ABI,
    static_cast<uint32_t>(sizeof(Host)),
    &HostLog,
    &HostPluginDir,
    &HostMainWindow,
    &HostViewWindow,
    &HostScale,
    &HostAddHotkey,
    &HostSubmitTask,
    &HostAddLogSink
};

// ---------------------------------------------------------------------------
// Log fan-out.
//
// Mirrors every framework log line to the subscribed plugins. Three rules keep
// it safe:
//   - re-entry is blocked per thread, so a sink calling Host::log can
//     never recurse;
//   - the slot table is only appended during Start(), before any line can
//     reach a sink, so the walk needs no lock;
//   - a slot that turns inactive mid-walk is skipped by the re-check after
//     inflight is bumped, which is also what makes an unload wait for it.
// ---------------------------------------------------------------------------
static thread_local bool t_inLogFanout = false;

static void FanoutLog(const char* line) {
    if (line == nullptr || t_inLogFanout) {
        return;
    }
    t_inLogFanout = true;
    for (int i = 0; i < g_slotCount; ++i) {
        PluginSlot& slot = g_slots[i];
        if (slot.state != PluginState::kActive || slot.logSink.fn == nullptr) {
            continue;
        }
        InterlockedIncrement(&slot.inflight);
        if (slot.state == PluginState::kActive) {
            slot.logSink.fn(line, slot.logSink.user);
        }
        InterlockedDecrement(&slot.inflight);
    }
    t_inLogFanout = false;
}

// ---------------------------------------------------------------------------
// Revocation. Called from the unload path only, on the loader thread.
// ---------------------------------------------------------------------------
static void ClearRegistrations(PluginSlot* slot) {
    if (slot == nullptr) {
        return;
    }
    // Release OS hotkeys first so a reloaded plugin can register the same vk.
    for (int h = 0; h < slot->hotkeyCount; ++h) {
        if (slot->hotkeys[h].id != 0 && g_hotkeyHwnd != nullptr) {
            UnregisterHotKey(g_hotkeyHwnd, slot->hotkeys[h].id);
        }
        slot->hotkeys[h].id = 0;
        slot->hotkeys[h].vk = 0;
        slot->hotkeys[h].fn = nullptr;
        slot->hotkeys[h].user = nullptr;
    }
    slot->hotkeyCount = 0;
    slot->taskHead = 0;
    slot->taskTail = 0;
    slot->logSink.fn = nullptr;
    slot->logSink.user = nullptr;
}

static void SetHostContext(HWND mainHwnd) {
    g_mainHwnd = mainHwnd;
    g_uiScale = ComputeHostScale(mainHwnd);
    Log("host init=%d scale=%.2f", mainHwnd ? 1 : 0, g_uiScale);
}

}  // namespace plugins

#endif  // PPLUGIN_PLUGIN_API_INC
