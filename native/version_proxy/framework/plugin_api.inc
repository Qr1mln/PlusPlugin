// ============================================================================
// plugin_api.inc - host side of the TTPluginHost capability table.
//
// Ownership rule: the framework owns every hook, every system registration
// point and every thread. A plugin only books callbacks here, so an unload can
// revoke the whole set from one place. Plugins never call these functions
// directly; they only see the TTPluginHost table.
//
// Included by dllmain.cpp after overlay.inc, so it shares the translation unit
// (and therefore the static Log() helper) with the rest of the payload.
//
// Package: framework. Depends on the public contract in ../sdk/ttplugin.h.
// Keep this file pure ASCII.
// ============================================================================

#ifndef TTVOICE_PLUGIN_API_INC
#define TTVOICE_PLUGIN_API_INC

#include "../sdk/ttplugin.h"

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
    kLoading,       // inside TTPluginDesc::init
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
struct DrawEntry {
    TTPluginDrawFn fn = nullptr;
    void* user = nullptr;
    int priority = 0;
};

struct RegionEntry {
    RECT rect = {};
    TTPluginClickFn fn = nullptr;
    void* user = nullptr;
    int priority = 0;
};

struct HotkeyEntry {
    int vk = 0;
    TTPluginHotkeyFn fn = nullptr;
    void* user = nullptr;
};

struct TaskEntry {
    TTPluginTaskFn fn = nullptr;
    void* user = nullptr;
};

struct LogSinkEntry {
    TTPluginLogFn fn = nullptr;
    void* user = nullptr;
};

// ---------------------------------------------------------------------------
// One resident plugin.
// ---------------------------------------------------------------------------
struct PluginSlot {
    PluginState state = PluginState::kEmpty;
    HMODULE module = nullptr;
    const TTPluginDesc* desc = nullptr;

    bool autoLoad = true;
    int priority = 0;

    wchar_t dir[kPathChars] = {};
    wchar_t dllPath[kPathChars] = {};
    char dirUtf8[kPathUtf8] = {};
    char dllName[kPathChars] = {};
    char name[kTextShort] = {};
    char version[kTextVersion] = {};

    DrawEntry draws[TTPLUGIN_MAX_DRAWS] = {};
    int drawCount = 0;
    RegionEntry regions[TTPLUGIN_MAX_REGIONS] = {};
    int regionCount = 0;
    HotkeyEntry hotkeys[TTPLUGIN_MAX_HOTKEYS] = {};
    int hotkeyCount = 0;

    TaskEntry tasks[kMaxTasksPerPlugin] = {};
    int taskHead = 0;
    int taskTail = 0;

    // One log subscription per plugin; revoked by ClearRegistrations().
    LogSinkEntry logSink;

    // Bumped around every callback that may run off the loader thread, so an
    // unload can prove that no plugin code is on a stack any more.
    LONG inflight = 0;
};

// ---------------------------------------------------------------------------
// Shared state. All of it lives in the loader thread's world; only inflight is
// touched from other threads and only through Interlocked*.
// ---------------------------------------------------------------------------
static PluginSlot g_slots[kMaxPlugins];
static int g_slotCount = 0;
static int g_activeSlot = -1;      // slot whose init() is running
static HWND g_mainHwnd = nullptr;
static float g_uiScale = 1.0f;
static bool g_started = false;

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
// The TTPluginHost entry points.
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

static int __cdecl HostAddDraw(TTPluginDrawFn fn, int priority, void* user) {
    if (g_activeSlot < 0 || fn == nullptr) {
        return TTPLUGIN_ERR_PARAM;
    }
    PluginSlot& slot = g_slots[g_activeSlot];
    if (slot.drawCount >= TTPLUGIN_MAX_DRAWS) {
        return TTPLUGIN_ERR_FULL;
    }
    DrawEntry& entry = slot.draws[slot.drawCount++];
    entry.fn = fn;
    entry.priority = priority;
    entry.user = user;
    return TTPLUGIN_OK;
}

static int __cdecl HostAddRegion(const RECT* logicalRect, int priority,
                                 TTPluginClickFn fn, void* user) {
    if (g_activeSlot < 0 || fn == nullptr || logicalRect == nullptr) {
        return TTPLUGIN_ERR_PARAM;
    }
    PluginSlot& slot = g_slots[g_activeSlot];
    if (slot.regionCount >= TTPLUGIN_MAX_REGIONS) {
        return TTPLUGIN_ERR_FULL;
    }
    RegionEntry& entry = slot.regions[slot.regionCount++];
    entry.rect = *logicalRect;
    entry.priority = priority;
    entry.fn = fn;
    entry.user = user;
    return TTPLUGIN_OK;
}

static int __cdecl HostAddHotkey(int vk, TTPluginHotkeyFn fn, void* user) {
    if (g_activeSlot < 0 || fn == nullptr || vk <= 0) {
        return TTPLUGIN_ERR_PARAM;
    }
    PluginSlot& slot = g_slots[g_activeSlot];
    if (slot.hotkeyCount >= TTPLUGIN_MAX_HOTKEYS) {
        return TTPLUGIN_ERR_FULL;
    }
    HotkeyEntry& entry = slot.hotkeys[slot.hotkeyCount++];
    entry.vk = vk;
    entry.fn = fn;
    entry.user = user;
    return TTPLUGIN_OK;
}

static int __cdecl HostSubmitTask(TTPluginTaskFn fn, void* user) {
    if (g_activeSlot < 0 || fn == nullptr) {
        return TTPLUGIN_ERR_PARAM;
    }
    PluginSlot& slot = g_slots[g_activeSlot];
    const int used = slot.taskTail - slot.taskHead;
    if (used >= kMaxTasksPerPlugin) {
        return TTPLUGIN_ERR_FULL;
    }
    TaskEntry& entry = slot.tasks[slot.taskTail % kMaxTasksPerPlugin];
    entry.fn = fn;
    entry.user = user;
    ++slot.taskTail;
    return TTPLUGIN_OK;
}

static void* __cdecl HostQuery(const char* capability) {
    // Reserved. Each future capability becomes a new atomic entry in
    // ttplugin.h, never an opaque hook installer.
    if (capability != nullptr) {
        Log("[plugin:%s] query '%s' unsupported", ActiveName(), capability);
    }
    return nullptr;
}

static int __cdecl HostAddLogSink(TTPluginLogFn fn, void* user) {
    if (g_activeSlot < 0 || fn == nullptr) {
        return TTPLUGIN_ERR_PARAM;
    }
    PluginSlot& slot = g_slots[g_activeSlot];
    if (slot.logSink.fn != nullptr) {
        return TTPLUGIN_ERR_FULL;
    }
    slot.logSink.fn = fn;
    slot.logSink.user = user;
    return TTPLUGIN_OK;
}

// The table handed to plugins. Filled once, never mutated afterwards.
static const TTPluginHost g_host = {
    TTPLUGIN_ABI,
    static_cast<uint32_t>(sizeof(TTPluginHost)),
    &HostLog,
    &HostPluginDir,
    &HostMainWindow,
    &HostViewWindow,
    &HostScale,
    &HostAddDraw,
    &HostAddRegion,
    &HostAddHotkey,
    &HostSubmitTask,
    &HostQuery,
    &HostAddLogSink
};

// ---------------------------------------------------------------------------
// Log fan-out.
//
// Mirrors every framework log line to the subscribed plugins. Three rules keep
// it safe:
//   - re-entry is blocked per thread, so a sink calling TTPluginHost::log can
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
    slot->drawCount = 0;
    slot->regionCount = 0;
    slot->hotkeyCount = 0;
    slot->taskHead = 0;
    slot->taskTail = 0;
    slot->logSink.fn = nullptr;
    slot->logSink.user = nullptr;
}

static void SetHostContext(HWND mainHwnd, float uiScale) {
    g_mainHwnd = mainHwnd;
    if (uiScale > 0.0f) {
        g_uiScale = uiScale;
    }
}

}  // namespace plugins

#endif  // TTVOICE_PLUGIN_API_INC
