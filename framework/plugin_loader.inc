// ============================================================================
// plugin_loader.inc - plugin lifecycle for the pplugin framework.
//
// Duties, in order of execution:
//   1. Scan      %APPDATA%\TTVoice\plugins\<NNN-name>\ for plugin.json
//   2. Validate  relative dll name, x64 PE, abi, struct size, mandatory export
//   3. Load      LoadLibraryExW by absolute path, then Desc::init
//   4. Dispatch  queued tasks (draw / region / hotkey dispatch lands in M1)
//   5. Unload    mark draining, revoke registrations, wait inflight, shutdown,
//                FreeLibrary
//   6. Control   poll plugins\control.txt for "load <name>" / "unload <name>"
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
static const wchar_t* const kManifestNameW = L"plugin.json";
static const wchar_t* const kControlNameW  = L"control.txt";
static const char* const    kManifestName  = "plugin.json";
static const wchar_t       kDisabledPrefix = L'_';
static const int            kControlLineChars = 128;

// Hot reload cadence. The quiet period is what keeps a half written dll (a
// deploy copies it in chunks) from being mapped: a change only counts once the
// file has stopped moving for this long.
static const int kWatchPollMs     = 500;
static const int kReloadSettleMs  = 400;
static const int kReloadRetryMs   = 2000;
static const int kReloadMaxRetries = 3;

using GetDescFn = const Desc* (__cdecl *)(void);

// Manifest text reuse. Loader thread only, so a single buffer is enough.
static char s_manifestBuf[kManifestMaxBytes];

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
// Minimal flat JSON reader. plugin.json is a flat object written by us, so a
// full parser is unnecessary and would duplicate the plugin side of the
// codebase.
// ---------------------------------------------------------------------------
static const char* JsonFindValue(const char* json, const char* key) {
    if (json == nullptr || key == nullptr) {
        return nullptr;
    }
    const size_t keyLen = strlen(key);
    const char* cursor = json;
    while ((cursor = strchr(cursor, '"')) != nullptr) {
        const char* name = cursor + 1;
        const char* nameEnd = strchr(name, '"');
        if (nameEnd == nullptr) {
            return nullptr;
        }
        cursor = nameEnd + 1;

        const bool sameKey = (static_cast<size_t>(nameEnd - name) == keyLen) &&
                             (strncmp(name, key, keyLen) == 0);
        if (!sameKey) {
            continue;
        }

        const char* colon = strchr(nameEnd, ':');
        if (colon == nullptr) {
            return nullptr;
        }
        // A quote between this key and the colon means nameEnd closed a value,
        // not a key: keep scanning instead of returning the wrong field.
        const char* nextQuote = strchr(nameEnd + 1, '"');
        if (nextQuote != nullptr && nextQuote < colon) {
            continue;
        }
        ++colon;
        while (*colon == ' ' || *colon == '\t' || *colon == '\r' || *colon == '\n') {
            ++colon;
        }
        return colon;
    }
    return nullptr;
}

static bool JsonReadString(const char* json, const char* key, char* out, int cap) {
    if (out == nullptr || cap <= 0) {
        return false;
    }
    out[0] = 0;
    const char* value = JsonFindValue(json, key);
    if (value == nullptr || *value != '"') {
        return false;
    }
    ++value;
    int len = 0;
    while (*value != 0 && *value != '"' && len < cap - 1) {
        if (*value == '\\' && value[1] != 0) {
            ++value;  // good enough for our own manifests
        }
        out[len++] = *value++;
    }
    out[len] = 0;
    return len > 0;
}

static int JsonReadInt(const char* json, const char* key, int fallback) {
    const char* value = JsonFindValue(json, key);
    if (value == nullptr) {
        return fallback;
    }
    return atoi(value);
}

static bool JsonReadBool(const char* json, const char* key, bool fallback) {
    const char* value = JsonFindValue(json, key);
    if (value == nullptr) {
        return fallback;
    }
    if (_strnicmp(value, "true", 4) == 0) {
        return true;
    }
    if (_strnicmp(value, "false", 5) == 0) {
        return false;
    }
    return fallback;
}

// ---------------------------------------------------------------------------
// Manifest.
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

// Derives "name.dll" from the directory name when plugin.json is absent.
static void DefaultDllName(PluginSlot* slot) {
    if (slot == nullptr) {
        return;
    }
    const wchar_t* base = wcsrchr(slot->dir, L'\\');
    if (base == nullptr) {
        return;
    }
    ++base;
    wchar_t leaf[kPathChars] = {};
    swprintf_s(leaf, L"%s.dll", base);
    WideToUtf8(leaf, slot->dllName, kPathChars);

    char narrow[kPathUtf8] = {};
    WideToUtf8(base, narrow, kPathUtf8);
    strcpy_s(slot->name, static_cast<size_t>(sizeof(slot->name)), narrow);
}

static void ReadPluginManifest(int index) {
    if (index < 0 || index >= g_slotCount) {
        return;
    }
    PluginSlot& slot = g_slots[index];
    DefaultDllName(&slot);

    wchar_t manifest[kPathChars] = {};
    swprintf_s(manifest, L"%s\\%s", slot.dir, kManifestNameW);
    if (!ReadTextFile(manifest, s_manifestBuf, kManifestMaxBytes, nullptr)) {
        Log("plugin: %s has no %s, will use %s", slot.dirUtf8, kManifestName,
            slot.dllName);
        return;
    }

    char text[kTextShort] = {};
    if (JsonReadString(s_manifestBuf, "dll", text, static_cast<int>(sizeof(text)))) {
        strcpy_s(slot.dllName, static_cast<size_t>(sizeof(slot.dllName)), text);
    }
    if (JsonReadString(s_manifestBuf, "name", text, static_cast<int>(sizeof(text)))) {
        strcpy_s(slot.name, static_cast<size_t>(sizeof(slot.name)), text);
    }
    if (JsonReadString(s_manifestBuf, "version", text, static_cast<int>(sizeof(text)))) {
        strcpy_s(slot.version, static_cast<size_t>(sizeof(slot.version)), text);
    }

    slot.priority = JsonReadInt(s_manifestBuf, "priority", 0);
    slot.autoLoad = JsonReadBool(s_manifestBuf, "enabled", true);
    if (!slot.autoLoad) {
        slot.state = PluginState::kDisabled;
    }

    // sha256 is parsed from M3 on; the field is tolerated now so manifests do
    // not have to change when the trust chain lands.
    char digest[kTextShort] = {};
    if (!JsonReadString(s_manifestBuf, "sha256", digest, static_cast<int>(sizeof(digest)))) {
        Log("plugin: %s has no sha256 (trust check arrives in M3)", slot.dirUtf8);
    }
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

    // Absolute path plus DLL_LOAD_DIR: the plugin's own dependencies resolve
    // next to it, never through the process working directory.
    HMODULE module = LoadLibraryExW(slot.dllPath, nullptr,
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
    if (desc == nullptr || desc->name == nullptr || desc->abi == 0 ||
        desc->abi > P_ABI) {
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
    Log("plugin: loaded %s v%s from %s hotkeys=%d",
        slot.name, slot.version, slot.dirUtf8, slot.hotkeyCount);
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
static int CollectDirectories(const wchar_t* root, wchar_t names[][kPathChars],
                              int maxCount) {
    wchar_t pattern[kPathChars] = {};
    swprintf_s(pattern, L"%s\\*", root);

    WIN32_FIND_DATAW data = {};
    HANDLE find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) {
        return 0;
    }

    int count = 0;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            continue;
        }
        if (data.cFileName[0] == L'.' || data.cFileName[0] == kDisabledPrefix) {
            continue;
        }
        if (count >= maxCount) {
            Log("plugin: too many directories, rest ignored");
            break;
        }
        wcscpy_s(names[count], static_cast<size_t>(kPathChars), data.cFileName);
        ++count;
    } while (FindNextFileW(find, &data));

    FindClose(find);

    // Directory order is not guaranteed by FindFirstFile; sort so "NNN-" keeps
    // meaning something and runs stay reproducible.
    for (int i = 1; i < count; ++i) {
        wchar_t key[kPathChars] = {};
        wcscpy_s(key, static_cast<size_t>(kPathChars), names[i]);
        int j = i - 1;
        while (j >= 0 && wcscmp(names[j], key) > 0) {
            wcscpy_s(names[j + 1], static_cast<size_t>(kPathChars), names[j]);
            --j;
        }
        wcscpy_s(names[j + 1], static_cast<size_t>(kPathChars), key);
    }
    return count;
}

static void ScanPlugins() {
    wchar_t root[kPathChars] = {};
    if (!GetPluginsRoot(root, kPathChars)) {
        return;
    }
    if (CreateDirectoryW(root, nullptr) == FALSE &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        Log("plugin: cannot create %ls err=%lu", root, GetLastError());
        return;
    }

    wchar_t names[kMaxPlugins][kPathChars] = {};
    const int found = CollectDirectories(root, names, kMaxPlugins);
    Log("plugin: scanning %ls, %d candidate(s)", root, found);

    for (int i = 0; i < found; ++i) {
        PluginSlot& slot = g_slots[g_slotCount];
        slot = PluginSlot();
        slot.state = PluginState::kDiscovered;
        swprintf_s(slot.dir, L"%s\\%s", root, names[i]);
        WideToUtf8(slot.dir, slot.dirUtf8, kPathUtf8);
        ++g_slotCount;

        ReadPluginManifest(g_slotCount - 1);
        Log("plugin: found %s name=%s dll=%s auto=%d priority=%d", slot.dirUtf8,
            slot.name, slot.dllName, slot.autoLoad ? 1 : 0, slot.priority);

        if (slot.autoLoad) {
            LoadPlugin(g_slotCount - 1);
        }
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
// Hot reload: watch plugin.json (enabled flag + write time) and the plugin dll.
//
// A rebuild or a manifest edit should not need a restart, but a deploy copies
// files in chunks and mapping a half written dll is fatal. Every observed change
// therefore has to stay quiet for kReloadSettleMs before it is acted on; until
// then the newest stamps replace the pending ones and the timer restarts.
//
// Rules:
//   json: enabled=false -> unload and stay kDisabled
//   json: enabled=true  -> (re)load, so saving the json is the reload trigger
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

static void SlotManifestPath(const PluginSlot& slot, wchar_t* out, size_t cap) {
    swprintf_s(out, cap, L"%s\\%s", slot.dir, kManifestNameW);
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
    wchar_t jsonPath[kPathChars] = {};
    wchar_t dllPath[kPathChars] = {};
    SlotManifestPath(slot, jsonPath, kPathChars);
    SlotDllPath(slot, dllPath, kPathChars);
    FileStamp(jsonPath, &slot.jsonTime, &slot.jsonSize);
    FileStamp(dllPath, &slot.dllTime, &slot.dllSize);
    slot.pendAt = 0;
}

static void ReadManifestFlags(const PluginSlot& slot, bool* enabled, int* priority,
                              char* dllName, int dllCap) {
    wchar_t manifest[kPathChars] = {};
    SlotManifestPath(slot, manifest, kPathChars);
    if (!ReadTextFile(manifest, s_manifestBuf, kManifestMaxBytes, nullptr)) {
        return;   // no manifest any more: keep the caller's current defaults
    }
    *enabled = JsonReadBool(s_manifestBuf, "enabled", true);
    *priority = JsonReadInt(s_manifestBuf, "priority", *priority);
    char text[kPathChars] = {};
    if (JsonReadString(s_manifestBuf, "dll", text, static_cast<int>(sizeof(text)))) {
        strncpy_s(dllName, static_cast<size_t>(dllCap), text, _TRUNCATE);
    }
}

static void ReloadSlot(int index, bool jsonChanged) {
    PluginSlot& slot = g_slots[index];
    const bool wasActive = (slot.state == PluginState::kActive);

    if (jsonChanged) {
        bool enabled = true;
        int priority = slot.priority;
        char dllName[kPathChars] = {};
        strcpy_s(dllName, slot.dllName);
        ReadManifestFlags(slot, &enabled, &priority, dllName,
                          static_cast<int>(sizeof(dllName)));
        slot.priority = priority;

        if (!enabled) {
            if (wasActive) UnloadPlugin(index);
            if (slot.state != PluginState::kQuarantined) {
                slot.state = PluginState::kDisabled;
                Log("plugin: %s disabled by plugin.json", slot.dirUtf8);
            }
            return;
        }
        if (_stricmp(dllName, slot.dllName) != 0) {
            strcpy_s(slot.dllName, dllName);
            Log("plugin: %s manifest points at %s", slot.dirUtf8, slot.dllName);
        }
        if (wasActive) {
            UnloadPlugin(index);
        } else if (slot.state == PluginState::kRejected) {
            slot.state = PluginState::kDisabled;   // let the load below retry it
        }
    } else if (!wasActive) {
        return;   // dll touched while it was not loaded: nothing to restart
    } else {
        UnloadPlugin(index);
    }

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

        wchar_t jsonPath[kPathChars] = {};
        wchar_t dllPath[kPathChars] = {};
        SlotManifestPath(slot, jsonPath, kPathChars);
        SlotDllPath(slot, dllPath, kPathChars);

        FILETIME jt = {};
        DWORD js = 0;
        FILETIME dt = {};
        DWORD ds = 0;
        const bool hasJson = FileStamp(jsonPath, &jt, &js);
        const bool hasDll = FileStamp(dllPath, &dt, &ds);

        if (!slot.stampsValid) {
            RecordStamps(slot);
            slot.stampsValid = true;
            continue;   // baseline only, never a reload on the first look
        }

        const bool jsonChanged = hasJson && !SameStamp(jt, js, slot.jsonTime, slot.jsonSize);
        const bool dllChanged = hasDll && !SameStamp(dt, ds, slot.dllTime, slot.dllSize);
        if (!jsonChanged && !dllChanged) {
            slot.pendAt = 0;
            continue;
        }

        const bool stillMoving = slot.pendAt == 0 ||
            !SameStamp(jt, js, slot.pendJsonTime, slot.pendJsonSize) ||
            !SameStamp(dt, ds, slot.pendDllTime, slot.pendDllSize);
        if (stillMoving) {
            slot.pendJsonTime = jt;
            slot.pendJsonSize = js;
            slot.pendDllTime = dt;
            slot.pendDllSize = ds;
            slot.pendAt = now;
            continue;
        }
        if (now - slot.pendAt < static_cast<ULONGLONG>(kReloadSettleMs)) {
            continue;   // quiet period not over yet
        }

        slot.pendAt = 0;
        slot.pendJsonTime = jt;
        slot.pendJsonSize = js;
        slot.pendDllTime = dt;
        slot.pendDllSize = ds;
        Log("plugin: %s changed on disk (%s%s), hot reloading", slot.dirUtf8,
            jsonChanged ? "json" : "",
            dllChanged ? (jsonChanged ? "+dll" : "dll") : "");
        ReloadSlot(i, jsonChanged);
        // The reload may have changed the dll name, so baseline against the
        // paths the slot uses now instead of the ones we just compared.
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
    // The hotkey dispatcher must be live before ScanPlugins(): a plugin books
    // its hotkeys inside init(), which runs during the scan.
    StartHotkeyThread();
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
