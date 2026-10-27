// ============================================================================
// plugin_loader.inc - plugin lifecycle for the pplugin framework.
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
//
// Package: framework. Pulls plugin_api.inc (and through it sdk/pplugin.h),
// so including this single file is enough for the payload.
// Keep this file pure ASCII.
// ============================================================================

#ifndef PPLUGIN_PLUGIN_LOADER_INC
#define PPLUGIN_PLUGIN_LOADER_INC

#include "plugin_api.inc"

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
    const std::vector<std::wstring>& names = LoadDllNames();
    const std::wstring hostDir = ModuleDir().wstring();
    Log("loaders.");

    for (const std::wstring& name : names) {
        if (g_slotCount >= kMaxPlugins) {
            Log("plugin: too many dlls, rest ignored");
            break;
        }
        PluginSlot& slot = g_slots[g_slotCount];
        slot = PluginSlot();
        slot.state = PluginState::kDiscovered;
        wcscpy_s(slot.dir, static_cast<size_t>(kPathChars), hostDir.c_str());
        WideToUtf8(slot.dir, slot.dirUtf8, kPathUtf8);
        WideToUtf8(name.c_str(), slot.dllName, kPathChars);

        std::wstring base = name;
        const size_t dot = base.rfind(L'.');
        if (dot != std::wstring::npos) {
            base.resize(dot);
        }
        WideToUtf8(base.c_str(), slot.name, static_cast<int>(sizeof(slot.name)));

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
static void Start() {
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

#endif  // PPLUGIN_PLUGIN_LOADER_INC
