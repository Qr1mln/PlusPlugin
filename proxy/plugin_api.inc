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
    wchar_t loadedPath[kPathChars] = {};  // 实际加载的副本路径（原文件不被锁定）
    char dirUtf8[kPathUtf8] = {};
    char dllName[kPathChars] = {};
    char name[kTextShort] = {};
    char version[kTextVersion] = {};

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
static void __cdecl PublishLog(const char* text) {
    LogTagged(ActiveName(), "%s", (text != nullptr) ? text : "(null)");
}

static HWND __cdecl HostMainWindow() {
    return g_mainHwnd;
}

// Flutter 的真实输入目标是 runner 顶层窗口下的 FLUTTERVIEW 子窗口。把鼠标消息投给
// 顶层窗口不会有任何人处理（Flutter 的输入全在 view 里），所以 viewWindow() 必须
// 交出子窗口，否则插件 PostMessage 出去的点击「发送成功但毫无反应」。
struct ViewFindCtx {
    HWND first;     // 第一个类名匹配的（兜底）
    HWND visible;   // 其中可见的那个（优先：隐藏的 view 收不到输入）
};

static BOOL CALLBACK FindViewProc(HWND child, LPARAM lp) {
    wchar_t cls[64] = {};
    if (GetClassNameW(child, cls, 63) <= 0 || lstrcmpiW(cls, L"FLUTTERVIEW") != 0) {
        return TRUE;
    }
    ViewFindCtx* ctx = reinterpret_cast<ViewFindCtx*>(lp);
    if (ctx->first == nullptr) {
        ctx->first = child;
    }
    if (IsWindowVisible(child) != FALSE) {
        ctx->visible = child;
        return FALSE;   // 找到可见的，停止枚举
    }
    return TRUE;
}

// 子窗口在整个进程生命周期内稳定，找到后缓存；窗口重建时（IsWindow 失败）重新找。
static HWND ResolveViewWindow(HWND main) {
    static HWND cached = nullptr;
    if (cached != nullptr && IsWindow(cached) != FALSE) {
        return cached;
    }
    if (main == nullptr) {
        return nullptr;
    }
    ViewFindCtx ctx{nullptr, nullptr};
    EnumChildWindows(main, &FindViewProc, reinterpret_cast<LPARAM>(&ctx));
    cached = (ctx.visible != nullptr) ? ctx.visible : ctx.first;
    if (cached != nullptr) {
        Log("host view=0x%p (FLUTTERVIEW)", reinterpret_cast<void*>(cached));
    }
    return cached;
}

static HWND __cdecl HostViewWindow() {
    HWND view = ResolveViewWindow(g_mainHwnd);
    return (view != nullptr) ? view : g_mainHwnd;   // UI 未就绪时退回主窗口
}

static float __cdecl HostScale() {
    return g_uiScale;
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

static int __cdecl SubscribeLog(LogFn fn, void* user) {
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
    &PublishLog,
    &HostMainWindow,
    &HostViewWindow,
    &HostScale,
    &HostSubmitTask,
    &SubscribeLog
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
