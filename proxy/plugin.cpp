// ============================================================================
// plugin.cpp - host side of the pplugin framework: the Host capability table
// plus the plugin lifecycle (discovery, validation, load, hot reload, unload).
//
// Merged from the former plugin_api.inc + plugin_loader.inc so the payload is a
// normal translation unit instead of "include the .inc implementation".
//
// Ownership rule: the framework owns every hook, every system registration
// point and every thread. A plugin only books callbacks here, so an unload can
// revoke the whole set from one place. Plugins never call these functions
// directly; they only see the Host table.
//
// Only SetHostContext() and Start() are visible outside this file (plugin.h).
//
// Package: framework. Depends on the public contract in ../sdk/pplugin.h.
// Keep this file pure ASCII.
// ============================================================================

#include "plugin.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

#include "../sdk/pplugin.h"

#include "config.h"
#include "log.h"

// ===========================================================================
// Former plugin_api.inc: host side of the Host capability table.
// ===========================================================================
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

// Exported to the rest of the proxy (declared in plugin.h).
void SetHostContext(HWND mainHwnd) {
    g_mainHwnd = mainHwnd;
    g_uiScale = ComputeHostScale(mainHwnd);
    Log("host init=%d scale=%.2f", mainHwnd ? 1 : 0, g_uiScale);
}

}  // namespace plugins

// ===========================================================================
// Former plugin_loader.inc: plugin lifecycle for the pplugin framework.
//
// Duties, in order of execution:
//   1. Discover  version.ini [loaders] dlls= (bare names resolved against ModuleDir())
//   2. Validate  relative dll name, x64 PE, abi, struct size, mandatory export
//   3. Load      LoadLibraryExW by absolute path, then Desc::init
//   4. Dispatch  queued tasks (draw / region dispatch lands in M1)
//   5. Unload    mark draining, revoke registrations, wait inflight, shutdown,
//                FreeLibrary
//   6. Reload    watch each listed dll on disk and hot reload on change
//
// Everything here runs on one loader thread, so the slot table needs no lock.
// Only PluginSlot::inflight is shared with other threads.
// ===========================================================================
namespace plugins {

// ---------------------------------------------------------------------------
// File names and paths. Fixed on purpose: a configurable plugins root would
// let a tampered ini load code from any directory.
// ---------------------------------------------------------------------------
static const wchar_t* const kPluginsRootFormat = L"%s\\TTVoice\\plugins";
static const wchar_t* const kControlNameW  = L"control.txt";
static const int            kControlLineChars = 128;

// Hot reload cadence. The quiet period is what keeps a half written dll (a
// deploy copies it in chunks) from being mapped: a change only counts once the
// file has stopped moving for this long.
static const int kWatchPollMs     = 500;
static const int kReloadSettleMs  = 400;
static const int kReloadRetryMs   = 2000;
static const int kReloadMaxRetries = 3;

using GetDescFn = const Desc* (__cdecl *)(void);

// ---------------------------------------------------------------------------
// Path helpers.
// ---------------------------------------------------------------------------
static bool GetPluginsRoot(wchar_t* out, int cap) {
    if (out == nullptr || cap <= 0) {
        return false;
    }
    wchar_t appdata[kPathChars] = {};
    const DWORD len = GetEnvironmentVariableW(L"APPDATA", appdata, kPathChars);
    if (len == 0 || len >= static_cast<DWORD>(kPathChars)) {
        Log("plugin: APPDATA unavailable");
        return false;
    }
    swprintf_s(out, static_cast<size_t>(cap), kPluginsRootFormat, appdata);
    return true;
}

// Rejects absolute paths, drive letters, wildcards and any ".." segment, so a
// manifest cannot point outside its own directory.
static bool IsSafeRelativeName(const char* name) {
    if (name == nullptr || name[0] == 0) {
        return false;
    }
    if (name[0] == '/' || name[0] == '\\') {
        return false;
    }
    if (strstr(name, "..") != nullptr) {
        return false;
    }
    for (const char* p = name; *p != 0; ++p) {
        const char c = *p;
        if (c == ':' || c == '|' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>') {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// File read.
// ---------------------------------------------------------------------------
static bool ReadTextFile(const wchar_t* path, char* out, int cap, int* outLen) {
    if (path == nullptr || out == nullptr || cap <= 0) {
        return false;
    }
    out[0] = 0;
    if (outLen != nullptr) {
        *outLen = 0;
    }
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path, L"rb") != 0 || fp == nullptr) {
        return false;
    }
    const size_t got = fread(out, 1, static_cast<size_t>(cap) - 1, fp);
    fclose(fp);
    out[got] = 0;
    if (outLen != nullptr) {
        *outLen = static_cast<int>(got);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Validation.
// ---------------------------------------------------------------------------
static bool IsPeAmd64(const wchar_t* path) {
    if (path == nullptr) {
        return false;
    }
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path, L"rb") != 0 || fp == nullptr) {
        return false;
    }

    bool ok = false;
    IMAGE_DOS_HEADER dos = {};
    if (fread(&dos, sizeof(dos), 1, fp) == 1 && dos.e_magic == IMAGE_DOS_SIGNATURE &&
        dos.e_lfanew > 0) {
        if (_fseeki64(fp, dos.e_lfanew, SEEK_SET) == 0) {
            DWORD signature = 0;
            IMAGE_FILE_HEADER header = {};
            if (fread(&signature, sizeof(signature), 1, fp) == 1 &&
                signature == IMAGE_NT_SIGNATURE &&
                fread(&header, sizeof(header), 1, fp) == 1) {
                ok = (header.Machine == IMAGE_FILE_MACHINE_AMD64);
            }
        }
    }
    fclose(fp);
    return ok;
}

static void RejectPlugin(int index, const char* reason) {
    if (index < 0 || index >= g_slotCount) {
        return;
    }
    PluginSlot& slot = g_slots[index];
    if (slot.module != nullptr) {
        FreeLibrary(slot.module);
        slot.module = nullptr;
    }
    if (slot.loadedPath[0] != 0 && wcscmp(slot.loadedPath, slot.dllPath) != 0) {
        DeleteFileW(slot.loadedPath);
    }
    slot.loadedPath[0] = 0;
    slot.desc = nullptr;
    slot.state = PluginState::kRejected;
    Log("plugin: rejected %s (%s)", slot.dirUtf8,
        (reason != nullptr) ? reason : "unspecified");
}

// ---------------------------------------------------------------------------
// Load / unload.
// ---------------------------------------------------------------------------
static void LoadPlugin(int index) {
    if (index < 0 || index >= g_slotCount) {
        return;
    }
    PluginSlot& slot = g_slots[index];
    if (slot.state != PluginState::kDiscovered &&
        slot.state != PluginState::kDisabled &&
        slot.state != PluginState::kUnloaded) {
        return;
    }
    if (!IsSafeRelativeName(slot.dllName)) {
        RejectPlugin(index, "unsafe dll name");
        return;
    }

    wchar_t leaf[kPathChars] = {};
    Utf8ToWide(slot.dllName, leaf, kPathChars);
    swprintf_s(slot.dllPath, L"%s\\%s", slot.dir, leaf);
    if (GetFileAttributesW(slot.dllPath) == INVALID_FILE_ATTRIBUTES) {
        RejectPlugin(index, "dll missing");
        return;
    }
    if (!IsPeAmd64(slot.dllPath)) {
        RejectPlugin(index, "not an x64 PE");
        return;
    }

    // 复制成唯一名字再加载：原 DLL 不被映射锁定，部署可直接覆盖原文件，
    // watcher 察觉变化后自动热重载（否则 Windows 锁文件，只能重启）。
    static LONG s_loadSeq = 0;
    const LONG seq = InterlockedIncrement(&s_loadSeq);
    swprintf_s(slot.loadedPath, L"%s\\%s.%ld.loaded", slot.dir, leaf, seq);
    if (!CopyFileW(slot.dllPath, slot.loadedPath, FALSE)) {
        // 复制失败（如目录只读）就退回直接加载原文件。
        wcscpy_s(slot.loadedPath, static_cast<size_t>(kPathChars), slot.dllPath);
    }

    // Absolute path plus DLL_LOAD_DIR: the plugin's own dependencies resolve
    // next to it, never through the process working directory.
    HMODULE module = LoadLibraryExW(slot.loadedPath, nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                    LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module == nullptr) {
        Log("plugin: LoadLibraryEx %s failed err=%lu", slot.dllName, GetLastError());
        RejectPlugin(index, "LoadLibraryEx failed");
        return;
    }

    auto getDesc = reinterpret_cast<GetDescFn>(
        GetProcAddress(module, ENTRY_NAME));
    if (getDesc == nullptr) {
        Log("plugin: %s has no %s", slot.dllName, ENTRY_NAME);
        RejectPlugin(index, "missing entry export");
        return;
    }

    const Desc* desc = getDesc();
    if (desc == nullptr || desc->name == nullptr || desc->abi != P_ABI) {
        RejectPlugin(index, "bad desc or abi");
        return;
    }
    if (desc->structSize < static_cast<uint32_t>(sizeof(Desc))) {
        RejectPlugin(index, "desc too small");
        return;
    }

    slot.module = module;
    slot.desc = desc;
    strcpy_s(slot.name, static_cast<size_t>(sizeof(slot.name)), desc->name);
    if (desc->version != nullptr) {
        strcpy_s(slot.version, static_cast<size_t>(sizeof(slot.version)), desc->version);
    }
    slot.state = PluginState::kLoading;

    g_activeSlot = index;
    const int rc = (desc->init != nullptr) ? desc->init(&g_host) : P_OK;
    g_activeSlot = -1;

    if (rc != P_OK) {
        ClearRegistrations(&slot);
        Log("plugin: %s init returned %d", slot.name, rc);
        RejectPlugin(index, "init refused");
        return;
    }

    slot.state = PluginState::kActive;
    Log("plugin: loaded %s v%s from %s", slot.name, slot.version, slot.dirUtf8);
}

static void UnloadPlugin(int index) {
    if (index < 0 || index >= g_slotCount) {
        return;
    }
    PluginSlot& slot = g_slots[index];
    if (slot.state == PluginState::kQuarantined) {
        Log("plugin: %s stays quarantined, restart the app to drop it", slot.name);
        return;
    }
    if (slot.state != PluginState::kActive) {
        return;
    }

    slot.state = PluginState::kDraining;
    ClearRegistrations(&slot);

    const ULONGLONG deadline = GetTickCount64() +
                               static_cast<ULONGLONG>(kDrainTimeoutMs);
    while (InterlockedCompareExchange(&slot.inflight, 0, 0) > 0 &&
           GetTickCount64() < deadline) {
        Sleep(1);
    }

    if (InterlockedCompareExchange(&slot.inflight, 0, 0) > 0) {
        // Never unload code that may still be on a stack. Keeping the mapping
        // costs memory; calling into a freed image costs the process.
        slot.state = PluginState::kQuarantined;
        Log("plugin: %s drain timeout, quarantined", slot.name);
        return;
    }

    if (slot.desc != nullptr && slot.desc->shutdown != nullptr) {
        // Let the plugin log under its own name while it tears down.
        g_activeSlot = index;
        const int rc = slot.desc->shutdown();
        g_activeSlot = -1;
        if (rc != P_OK) {
            slot.state = PluginState::kQuarantined;
            Log("plugin: %s shutdown refused (%d), quarantined", slot.name, rc);
            return;
        }
    }

    HMODULE module = slot.module;
    slot.module = nullptr;
    slot.desc = nullptr;
    if (module != nullptr) {
        FreeLibrary(module);
    }
    if (slot.loadedPath[0] != 0 && wcscmp(slot.loadedPath, slot.dllPath) != 0) {
        DeleteFileW(slot.loadedPath);
    }
    slot.loadedPath[0] = 0;
    slot.state = PluginState::kUnloaded;
    Log("plugin: unloaded %s", slot.name);
}

// ---------------------------------------------------------------------------
// Task dispatch. M1 extends this with draw and region fan-out; both use the
// same inflight guard, so the unload protocol does not change.
// ---------------------------------------------------------------------------
// Plugin tick. Runs on the loader thread and is the only place a UI plugin
// pumps its own window messages: the framework keeps ownership of every
// thread, the plugin still gets a message loop it can unload from.
static void RunPendingTicks() {
    for (int i = 0; i < g_slotCount; ++i) {
        PluginSlot& slot = g_slots[i];
        if (slot.state != PluginState::kActive || slot.desc == nullptr ||
            slot.desc->tick == nullptr) {
            continue;
        }
        InterlockedIncrement(&slot.inflight);
        if (slot.state == PluginState::kActive) {
            const int prev = g_activeSlot;
            g_activeSlot = i;
            slot.desc->tick();
            g_activeSlot = prev;
        }
        InterlockedDecrement(&slot.inflight);
    }
}

static void RunPendingTasks() {
    for (int i = 0; i < g_slotCount; ++i) {
        PluginSlot& slot = g_slots[i];
        int budget = kTasksPerPoll;
        while (slot.state == PluginState::kActive &&
               slot.taskHead < slot.taskTail && budget > 0) {
            --budget;
            const TaskEntry entry = slot.tasks[slot.taskHead % kMaxTasksPerPlugin];
            ++slot.taskHead;
            InterlockedIncrement(&slot.inflight);
            const int prev = g_activeSlot;
            g_activeSlot = i;
            entry.fn(entry.user);
            g_activeSlot = prev;
            InterlockedDecrement(&slot.inflight);
        }
        if (slot.taskHead >= slot.taskTail) {
            slot.taskHead = 0;
            slot.taskTail = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Scan.
// ---------------------------------------------------------------------------
// Discovery is driven by version.ini [loaders] dlls= (bare file names, resolved
// against ModuleDir()). No plugins folder scan and no plugin.json; the hot
// reload watcher below still compares the dll file stamps on disk.
// 清理上次进程退出遗留的 *.loaded 副本（当前未加载时能删掉）。
static void CleanupLoadedCopies(const wchar_t* dir, const char* dllNameUtf8) {
    wchar_t leaf[kPathChars] = {};
    Utf8ToWide(dllNameUtf8, leaf, kPathChars);
    wchar_t pattern[kPathChars] = {};
    swprintf_s(pattern, L"%s\\%s.*.loaded", dir, leaf);
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            wchar_t full[kPathChars] = {};
            swprintf_s(full, L"%s\\%s", dir, fd.cFileName);
            DeleteFileW(full);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void ScanPlugins() {
    wchar_t names[kMaxLoadDlls][kMaxDllNameChars] = {};
    const int nameCount = LoadDllNames(names, kMaxLoadDlls);
    const wchar_t* hostDir = ModuleDir();
    Log("loaders.");

    for (int ni = 0; ni < nameCount; ++ni) {
        const wchar_t* name = names[ni];
        if (g_slotCount >= kMaxPlugins) {
            Log("plugin: too many dlls, rest ignored");
            break;
        }
        PluginSlot& slot = g_slots[g_slotCount];
        slot = PluginSlot();
        slot.state = PluginState::kDiscovered;
        wcscpy_s(slot.dir, static_cast<size_t>(kPathChars), hostDir);
        WideToUtf8(slot.dir, slot.dirUtf8, kPathUtf8);
        WideToUtf8(name, slot.dllName, kPathChars);

        wchar_t base[kMaxDllNameChars] = {};
        wcscpy_s(base, static_cast<size_t>(kMaxDllNameChars), name);
        wchar_t* dot = wcsrchr(base, L'.');
        if (dot != nullptr) {
            *dot = L'\0';
        }
        WideToUtf8(base, slot.name, static_cast<int>(sizeof(slot.name)));

        slot.autoLoad = true;
        slot.priority = g_slotCount;
        ++g_slotCount;

        CleanupLoadedCopies(slot.dir, slot.dllName);
        Log("loader:%s.", slot.dllName);
        LoadPlugin(g_slotCount - 1);
    }
}

// ---------------------------------------------------------------------------
// Control file: plugins\control.txt, commands "load <name>", "unload <name>",
// "list". The file is truncated before the commands run, so a crash mid-way
// cannot replay them on the next poll.
// ---------------------------------------------------------------------------
static void HandleControlLine(const char* line) {
    char command[kControlLineChars] = {};
    char name[kTextShort] = {};
    const int fields = sscanf_s(line, "%127s %63s", command,
                                static_cast<unsigned>(sizeof(command)), name,
                                static_cast<unsigned>(sizeof(name)));
    if (fields < 1) {
        return;
    }

    if (_stricmp(command, "list") == 0) {
        for (int i = 0; i < g_slotCount; ++i) {
            Log("plugin: [%d] %s state=%s priority=%d", i, g_slots[i].name,
                StateName(g_slots[i].state), g_slots[i].priority);
        }
        return;
    }
    if (fields < 2) {
        Log("plugin: control '%s' needs a plugin name", command);
        return;
    }

    const int index = FindPluginByName(name);
    if (index < 0) {
        Log("plugin: control '%s %s' -> unknown plugin", command, name);
        return;
    }

    if (_stricmp(command, "load") == 0) {
        LoadPlugin(index);
        return;
    }
    if (_stricmp(command, "unload") == 0) {
        UnloadPlugin(index);
        return;
    }
    Log("plugin: control command '%s' ignored", command);
}

// ---------------------------------------------------------------------------
// Hot reload: watch the listed dll on disk.
//
// A rebuild should not need a restart, but a deploy copies the file in chunks
// and mapping a half written dll is fatal. Every observed change therefore has
// to stay quiet for kReloadSettleMs before it is acted on; until then the
// newest stamp replaces the pending one and the timer restarts.
//
// Rules:
//   dll newer + loaded  -> unload + load
//   dll newer + unloaded-> ignored, nothing to restart
// A failed load retries a few times, because the previous instance can still
// hold the file for a moment after FreeLibrary.
// ---------------------------------------------------------------------------

static bool FileStamp(const wchar_t* path, FILETIME* time, DWORD* size) {
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (path == nullptr || !GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
        return false;
    }
    if (time != nullptr) *time = fad.ftLastWriteTime;
    if (size != nullptr) *size = fad.nFileSizeLow;
    return true;
}

static bool SameStamp(const FILETIME& a, DWORD as, const FILETIME& b, DWORD bs) {
    return a.dwLowDateTime == b.dwLowDateTime &&
           a.dwHighDateTime == b.dwHighDateTime && as == bs;
}

static void SlotDllPath(const PluginSlot& slot, wchar_t* out, size_t cap) {
    wchar_t leaf[kPathChars] = {};
    Utf8ToWide(slot.dllName, leaf, kPathChars);
    swprintf_s(out, cap, L"%s\\%s", slot.dir, leaf);
}

// Reads only the fields a reload decides on and never touches slot.state: the
// slot may still be kActive while we look at the new manifest.
// Snapshot the file stamps the slot should be compared against. Called once for
// the baseline and again after a reload, because the reload may have switched
// the manifest's dll name (that would otherwise trigger a second reload).
static void RecordStamps(PluginSlot& slot) {
    wchar_t dllPath[kPathChars] = {};
    SlotDllPath(slot, dllPath, kPathChars);
    FileStamp(dllPath, &slot.dllTime, &slot.dllSize);
    slot.pendAt = 0;
}

static void ReloadSlot(int index) {
    PluginSlot& slot = g_slots[index];
    if (slot.state != PluginState::kActive) {
        return;   // dll touched while it was not loaded: nothing to restart
    }
    UnloadPlugin(index);
    if (slot.state == PluginState::kQuarantined) {
        Log("plugin: %s stays quarantined, not reloading it", slot.dirUtf8);
        return;
    }
    LoadPlugin(index);
    if (slot.state == PluginState::kRejected) {
        slot.wantLoad = true;
        slot.retryAt = GetTickCount64() + static_cast<ULONGLONG>(kReloadRetryMs);
    }
}

// Polled from the loader thread.
static void WatchPlugins() {
    const ULONGLONG now = GetTickCount64();
    for (int i = 0; i < g_slotCount; ++i) {
        PluginSlot& slot = g_slots[i];
        if (slot.state == PluginState::kLoading ||
            slot.state == PluginState::kDraining ||
            slot.state == PluginState::kQuarantined) {
            continue;   // already moving, or never callable again
        }

        if (slot.wantLoad) {
            if (now < slot.retryAt) continue;
            slot.wantLoad = false;
            slot.state = PluginState::kDisabled;
            LoadPlugin(i);
            if (slot.state == PluginState::kRejected) {
                ++slot.retries;
                if (slot.retries < kReloadMaxRetries) {
                    slot.wantLoad = true;
                    slot.retryAt = now + static_cast<ULONGLONG>(kReloadRetryMs);
                    Log("plugin: %s reload retry %d/%d", slot.dirUtf8, slot.retries,
                        kReloadMaxRetries);
                } else {
                    Log("plugin: %s reload gave up after %d tries", slot.dirUtf8,
                        slot.retries);
                    slot.retries = 0;
                }
            } else if (slot.state == PluginState::kActive) {
                slot.retries = 0;
            }
            continue;
        }

        wchar_t dllPath[kPathChars] = {};
        SlotDllPath(slot, dllPath, kPathChars);
        FILETIME dt = {};
        DWORD ds = 0;
        const bool hasDll = FileStamp(dllPath, &dt, &ds);

        if (!slot.stampsValid) {
            RecordStamps(slot);
            slot.stampsValid = true;
            continue;   // baseline only, never a reload on the first look
        }

        const bool dllChanged = hasDll && !SameStamp(dt, ds, slot.dllTime, slot.dllSize);
        if (!dllChanged) {
            slot.pendAt = 0;
            continue;
        }

        const bool stillMoving = slot.pendAt == 0 ||
            !SameStamp(dt, ds, slot.pendDllTime, slot.pendDllSize);
        if (stillMoving) {
            slot.pendDllTime = dt;
            slot.pendDllSize = ds;
            slot.pendAt = now;
            continue;
        }
        if (now - slot.pendAt < static_cast<ULONGLONG>(kReloadSettleMs)) {
            continue;   // quiet period not over yet
        }

        slot.pendAt = 0;
        slot.pendDllTime = dt;
        slot.pendDllSize = ds;
        Log("plugin: %s changed on disk (dll), hot reloading", slot.dirUtf8);
        ReloadSlot(i);
        RecordStamps(slot);
    }
}

static void ProcessControlFile() {
    wchar_t root[kPathChars] = {};
    if (!GetPluginsRoot(root, kPathChars)) {
        return;
    }
    wchar_t path[kPathChars] = {};
    swprintf_s(path, L"%s\\%s", root, kControlNameW);

    char buffer[kControlLineChars * 16] = {};
    if (!ReadTextFile(path, buffer, static_cast<int>(sizeof(buffer)), nullptr)) {
        return;
    }

    FILE* truncate = nullptr;
    if (_wfopen_s(&truncate, path, L"w") == 0 && truncate != nullptr) {
        fclose(truncate);
    }

    char* next = nullptr;
    for (char* line = strtok_s(buffer, "\r\n", &next); line != nullptr;
         line = strtok_s(nullptr, "\r\n", &next)) {
        while (*line == ' ' || *line == '\t') {
            ++line;
        }
        if (*line == 0 || *line == ';' || *line == '#') {
            continue;
        }
        Log("plugin: control '%s'", line);
        HandleControlLine(line);
    }
}

// ---------------------------------------------------------------------------
// Loader thread.
// ---------------------------------------------------------------------------
static DWORD WINAPI PluginControlThread(LPVOID) {
    Log("plugin: loader thread up, tick=%dms control=%dms watch=%dms drain=%dms",
        kTickIntervalMs, kControlPollMs, kWatchPollMs, kDrainTimeoutMs);

    ULONGLONG lastControl = 0;
    ULONGLONG lastWatch = 0;
    for (;;) {
        RunPendingTicks();
        RunPendingTasks();

        const ULONGLONG now = GetTickCount64();
        if (now - lastWatch >= static_cast<ULONGLONG>(kWatchPollMs)) {
            lastWatch = now;
            WatchPlugins();
        }
        if (now - lastControl >= static_cast<ULONGLONG>(kControlPollMs)) {
            lastControl = now;
            ProcessControlFile();
        }
        Sleep(static_cast<DWORD>(kTickIntervalMs));
    }
    return 0;
}

// Called from PayloadThread once the main window exists. Never call this from
// DllMain: LoadLibrary under the loader lock deadlocks as soon as a plugin
// touches user32 or creates a thread.
// Exported to the rest of the proxy (declared in plugin.h).
void Start() {
    if (g_started) {
        return;
    }
    g_started = true;
    ScanPlugins();

    // From here on every Log() line is mirrored to plugin log sinks. Set after
    // the scan so the fan-out never walks a table that is still being filled.
    g_logFanout = &FanoutLog;

    HANDLE thread = CreateThread(nullptr, 0, &PluginControlThread, nullptr, 0, nullptr);
    if (thread == nullptr) {
        Log("plugin: CreateThread failed err=%lu", GetLastError());
        return;
    }
    CloseHandle(thread);
}

}  // namespace plugins