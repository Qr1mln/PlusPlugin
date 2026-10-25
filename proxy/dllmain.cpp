// version.dll proxy payload: forwarding shell + plugin loader.
// Target process: whatever target.ini (next to this DLL) names in target=;
// the shipped app is a Flutter + ANGLE(D3D11) host, e.g. TTVoice.exe (x64).
//
// Forwarding model:
//   app -> our stub (stubs.asm) -> jmp qword ptr [g_pfn_X] -> real version.dll!X
//   The real DLL is loaded from %SystemRoot%\System32 by absolute path,
//   so there is no recursion and no version coupling.
//
// Design note: the real DLL is resolved inside DllMain because the app may
// call these APIs before the main window exists. Keep the dependency chain of
// the real DLL trivial (kernel32 only) so early loading cannot disturb the
// graphics stack.
//
// Framework duties (this module is the framework, see pplugin.h):
//   1. Forward every version.dll export to the real one (stubs.asm).
//   2. Provide the plugin framework only. The proxy does NOT render: no Present
//      hook, no D3D/DXGI device, no overlay window. Each plugin owns its own
//      window/UI and accesses the host through the Host API. Plugins never
//      install hooks or create devices themselves, so they stay unloadable.
//   3. Load and unload plugins from %APPDATA%\TTVoice\plugins. A plugin only
//      books callbacks through Host, so the loader can revoke the
//      whole set and then FreeLibrary it.
//
// NOTE: keep this file pure ASCII.

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cwchar>

#include "proxy_exports.h"

#pragma comment(lib, "user32.lib")

extern "C" {
#define DEF_PTR(name) void* g_pfn_##name = nullptr;
PROXY_EXPORT_LIST(DEF_PTR)
#undef DEF_PTR
}

static int g_resolveMiss = -1;
static const wchar_t* kTargetDll = L"version.dll";
static const wchar_t* kLogFile = L"proxy.log";

// Installed by the framework package once plugins may be loaded. Declared
// before Log() so the logger can mirror every line to the plugin log sinks.
static void (*g_logFanout)(const char* line) = nullptr;
static const int kLogLineChars = 1024;

// Directory our own DLL lives in (trailing backslash included). Both proxy.log
// and target.ini are looked up there, so they travel with the artifact.
static bool DllDirPath(wchar_t* out, size_t cap) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&DllDirPath), &self);
    if (self == nullptr || GetModuleFileNameW(self, out, (DWORD)cap) == 0) {
        return false;
    }
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash == nullptr) {
        return false;
    }
    *(slash + 1) = L'\0';
    return true;
}

// Resolve the log file path: next to our own DLL (version.dll) by default, so
// it sits beside the deployed artifact (e.g. %APPDATA%\TTVoice\). Falls back to
// %TEMP% when the module path cannot be resolved.
static void LogFilePath(wchar_t* out, size_t cap) {
    if (DllDirPath(out, cap) && wcscat_s(out, cap, kLogFile) == 0) {
        return;
    }
    GetTempPathW((DWORD)cap, out);
    wcscat_s(out, cap, kLogFile);
}

static void Log(const char* fmt, ...) {
    char line[kLogLineChars] = {};

    va_list ap;
    va_start(ap, fmt);
    vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);

    wchar_t path[MAX_PATH + 32] = {};
    LogFilePath(path, sizeof(path) / sizeof(path[0]));

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path, L"a") != 0 || fp == nullptr) return;

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    fprintf(fp, "[%02u:%02u:%02u.%03u][pid=%lu][tid=%lu] ",
            static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute),
            static_cast<unsigned>(st.wSecond), static_cast<unsigned>(st.wMilliseconds),
            GetCurrentProcessId(), GetCurrentThreadId());
    fprintf(fp, "%s", line);
    fputc('\n', fp);
    fclose(fp);

    // Mirror to plugins. The fan-out guards against re-entry, so a log sink
    // can never recurse into Log().
    if (g_logFanout != nullptr) {
        g_logFanout(line);
    }
}

// ---------------------------------------------------------------------------
// Host selection: target.ini next to our DLL.
//
// The host exe name is configuration, not code. Deploy target.ini beside
// version.dll with one key (a section header is optional, so both layouts
// below work):
//     target=SomeApp.exe
//     [host]
//     target=SomeApp.exe
//
// When the file is missing it is created with the placeholder below
// (target=test.exe), so the deployment is discoverable and editable instead of
// silently dead. test.exe never matches a real host, so a wrong deployment
// still hooks nothing.
//
// Optional second key, on by default:
//     debug=true      pop a MessageBox once the proxy finished attaching, so a
//                     successful load is visible without reading proxy.log
//                     (0 / false / no / off disables it)
// ---------------------------------------------------------------------------
static const wchar_t* kTargetIniName  = L"target.ini";
static const char*    kTargetKey      = "target";
static const char*    kDebugKey       = "debug";
static const char*    kDefaultTarget  = "test.exe";

// Full path of target.ini (next to our DLL); false when that cannot be built.
static bool TargetIniPath(wchar_t* out, size_t cap) {
    if (!DllDirPath(out, cap)) {
        return false;
    }
    return wcscat_s(out, cap, kTargetIniName) == 0;
}

// Seeds target.ini with target=test.exe + debug=true. Only called when the file
// is absent, never to overwrite an existing one.
static bool WriteDefaultTargetIni(const wchar_t* path) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path, L"wb") != 0 || fp == nullptr) {
        return false;
    }
    const char* text = "; host process for version.dll, e.g. target=MyApp.exe\r\n"
                       "target=test.exe\r\n"
                       "; pop a dialog when the proxy is attached (default true)\r\n"
                       "debug=true\r\n";
    fwrite(text, 1, strlen(text), fp);
    fclose(fp);
    return true;
}

// Reads one "key=value" line out of an ASCII/UTF-8 ini. Section headers, blank
// lines and ';' / '#' comments are skipped; quotes and a trailing comment are
// trimmed. Returns false when the key is absent or its value is empty.
static bool ReadIniValue(const wchar_t* path, const char* key, char* out, size_t cap) {
    if (path == nullptr || key == nullptr || out == nullptr || cap == 0) {
        return false;
    }
    out[0] = 0;

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path, L"rb") != 0 || fp == nullptr) {
        return false;
    }

    const size_t keyLen = strlen(key);
    bool found = false;
    while (!found) {
        char line[512] = {};
        if (fgets(line, sizeof(line), fp) == nullptr) {
            break;
        }
        char* p = line;
        if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
            (unsigned char)p[2] == 0xBF) {
            p += 3;   // UTF-8 BOM
        }
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        // Blank line, comment, or section header: the key may still come later.
        if (*p == 0 || *p == '\r' || *p == '\n' || *p == ';' || *p == '#' || *p == '[') {
            continue;
        }
        char* eq = strchr(p, '=');
        if (eq == nullptr) {
            continue;
        }
        char* keyEnd = eq;
        while (keyEnd > p && (keyEnd[-1] == ' ' || keyEnd[-1] == '\t')) {
            --keyEnd;
        }
        if (static_cast<size_t>(keyEnd - p) != keyLen ||
            _strnicmp(p, key, keyLen) != 0) {
            continue;
        }

        char* val = eq + 1;
        while (*val == ' ' || *val == '\t' || *val == '"' || *val == '\'') {
            ++val;
        }
        char* valEnd = val;
        while (*valEnd != 0 && *valEnd != ';' && *valEnd != '\r' && *valEnd != '\n') {
            ++valEnd;
        }
        while (valEnd > val && (valEnd[-1] == ' ' || valEnd[-1] == '\t' ||
                                valEnd[-1] == '"' || valEnd[-1] == '\'')) {
            --valEnd;
        }
        const size_t len = static_cast<size_t>(valEnd - val);
        if (len == 0 || len >= cap) {
            break;   // empty value: treat as "not configured"
        }
        memcpy(out, val, len);
        out[len] = 0;
        found = true;
    }
    fclose(fp);
    return found;
}

// Boolean ini value: 1/true/yes/on vs 0/false/no/off, case-insensitive. A
// missing key, an empty value or an unknown token yields the fallback.
static bool ReadIniBool(const wchar_t* path, const char* key, bool fallback) {
    char raw[32] = {};
    if (!ReadIniValue(path, key, raw, sizeof(raw))) {
        return fallback;
    }
    if (_stricmp(raw, "1") == 0 || _stricmp(raw, "true") == 0 ||
        _stricmp(raw, "yes") == 0 || _stricmp(raw, "on") == 0) {
        return true;
    }
    if (_stricmp(raw, "0") == 0 || _stricmp(raw, "false") == 0 ||
        _stricmp(raw, "no") == 0 || _stricmp(raw, "off") == 0) {
        return false;
    }
    return fallback;
}

// debug= from target.ini; default true. Read once: a MessageBox per attach.
static bool DebugEnabled() {
    static int cached = -1;
    if (cached < 0) {
        wchar_t ini[MAX_PATH] = {};
        const bool off = TargetIniPath(ini, MAX_PATH) &&
                         !ReadIniBool(ini, kDebugKey, true);
        cached = off ? 0 : 1;
    }
    return cached != 0;
}

// Cached host exe base name taken from target.ini; empty when the DLL directory
// itself cannot be resolved. A missing/unusable config falls back to
// target=test.exe (and the file is seeded when absent).
static const wchar_t* TargetExeName() {
    static wchar_t buf[MAX_PATH] = {};
    if (buf[0] != L'\0') {
        return buf;
    }

    wchar_t ini[MAX_PATH] = {};
    if (!TargetIniPath(ini, MAX_PATH)) {
        return buf;
    }

    char raw[128] = {};
    if (ReadIniValue(ini, kTargetKey, raw, sizeof(raw))) {
        if (MultiByteToWideChar(CP_ACP, 0, raw, -1, buf, MAX_PATH) > 0) {
            return buf;
        }
        buf[0] = L'\0';
    }

    // No usable target=: seed the file when it is missing, then use the
    // placeholder so both sides agree on what was read.
    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) {
        if (WriteDefaultTargetIni(ini)) {
            Log("%ls not found, created with target=%s", kTargetIniName, kDefaultTarget);
        } else {
            Log("WARN: %ls not found and could not be created", kTargetIniName);
        }
    } else {
        Log("WARN: %ls has no usable target=, using %s", kTargetIniName, kDefaultTarget);
    }
    MultiByteToWideChar(CP_ACP, 0, kDefaultTarget, -1, buf, MAX_PATH);
    return buf;
}

// Populate g_pfn_* with the real System32 version.dll exports (forwarding model).
static int ResolveRealDll() {
    wchar_t sysdir[MAX_PATH] = {};
    UINT n = GetSystemDirectoryW(sysdir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return -1;

    wchar_t path[MAX_PATH] = {};
    swprintf_s(path, L"%s\\%s", sysdir, kTargetDll);

    HMODULE real = LoadLibraryW(path);
    if (real == nullptr) return -1;

    int missing = 0;
#define RESOLVE(name)                                                     \
    g_pfn_##name = reinterpret_cast<void*>(GetProcAddress(real, #name));  \
    if (g_pfn_##name == nullptr) ++missing;
    PROXY_EXPORT_LIST(RESOLVE)
#undef RESOLVE
    return missing;
}

#include "framework/plugin_api.inc"     // g_slots / entries / PluginState (guarded; also pulled by the loader)
#include "framework/plugin_loader.inc"  // lifecycle / control.txt (re-includes plugin_api.inc, no-op)

// ---------------------------------------------------------------------------
// WebView2 CDP endpoint (consumed by the script plugin's selector clicks).
//
// The host's H5 layer (HeadlessInAppWebView composited into Flutter) ignores
// posted window messages and only obeys real input, so the engine drives it
// through the DevTools protocol instead: DOM query for the element rect, then
// Input.dispatchMouseEvent, which enters the browser's trusted input pipeline -
// the same road OS mouse messages take - without needing the window in the
// foreground.
//
// The port switch is a browser command line argument read from this
// environment variable when the FIRST WebView2 environment is created, so it
// must be set before that moment. DllMain of a statically imported proxy runs
// at process start, always early enough; PayloadThread and everything after it
// are not guarantees. Unconditional on purpose: the variable is inert for any
// process that never creates a WebView2 environment, while gating on
// target.ini would mean file I/O under the loader lock in DllMain.
//
// Append, never overwrite: the host may carry its own extra arguments.
// ---------------------------------------------------------------------------
static const wchar_t* kCdpEnvVar = L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS";
static const wchar_t* kCdpArg    = L"--remote-debugging-port=9223";

static void EnableWebView2Cdp() {
    wchar_t args[512] = {};
    const DWORD had = GetEnvironmentVariableW(kCdpEnvVar, args, 512);
    if (had == 0) {
        SetEnvironmentVariableW(kCdpEnvVar, kCdpArg);
        return;
    }
    if (had >= 500) return;   // suspiciously long, leave it alone
    if (wcsstr(args, L"--remote-debugging-port") != nullptr) return;
    wcscat_s(args, kCdpArg);
    SetEnvironmentVariableW(kCdpEnvVar, args);
}

// Cached base name of the host EXE, compared against target.ini so we only act
// inside the configured process and never a child or an unrelated one.
static const wchar_t* ExeBaseName() {
    static wchar_t buf[MAX_PATH] = {};
    if (buf[0] == L'\0') {
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        const wchar_t* slash = wcsrchr(buf, L'\\');
        if (slash) {
            const wchar_t* base = slash + 1;
            wmemmove(buf, base, wcslen(base) + 1);
        }
    }
    return buf;
}

struct WindowSearch {
    DWORD pid;
    HWND found;
};

static BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<WindowSearch*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid || !IsWindowVisible(hwnd)) return TRUE;

    wchar_t cls[128] = {};
    GetClassNameW(hwnd, cls, 128);
    if (wcscmp(cls, L"FLUTTER_RUNNER_WIN32_WINDOW") == 0) {
        ctx->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

// TTVoice's UI is a Flutter desktop app whose top-level window uses the well-known
// FLUTTER_RUNNER_WIN32_WINDOW class. We locate it to give its HWND to the framework
// as Host::mainWindow / viewWindow.
static HWND FindFlutterWindow() {
    WindowSearch ctx{GetCurrentProcessId(), nullptr};
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

// Runs off DllMain (spawned by CreateThread) so plugins are never loaded while the
// loader lock is held: a plugin's own DllMain may call user32 or start a thread.
// Flow: confirm host -> wait for the graphics stack (dxgi signals the UI is coming
// up) -> wait for the Flutter window -> hand HWND to framework (derives DPI scale,
// then starts plugins).
static DWORD WINAPI PayloadThread(LPVOID) {
    Log("attached exe=%ls resolve_miss=%d", ExeBaseName(), g_resolveMiss);

    const wchar_t* target = TargetExeName();
    if (target[0] == L'\0') {
        Log("skip: no target= in %ls next to %ls", kTargetIniName, kTargetDll);
        return 0;
    }
    if (lstrcmpiW(ExeBaseName(), target) != 0) {
        Log("skip: exe=%ls is not the configured target=%ls", ExeBaseName(), target);
        return 0;
    }
    Log("target matched: %ls", target);

    // DllMain armed the WebView2 debug port before any code of the host ran;
    // say so once here, where logging is safe, so a missing endpoint later is
    // easy to explain from the log alone.
    {
        wchar_t args[256] = {};
        GetEnvironmentVariableW(kCdpEnvVar, args, 256);
        Log("webview2 cdp args: %ls", args[0] ? args : L"(none)");
    }

    // Wait for dxgi.dll (ANGLE D3D11 backend).
    HMODULE dxgi = nullptr;
    for (int i = 0; i < 3000; ++i) {
        dxgi = GetModuleHandleW(L"dxgi.dll");
        if (dxgi != nullptr) break;
        Sleep(10);
    }
    if (dxgi == nullptr) {
        Log("WARN: dxgi.dll not found in 30s");
        return 0;
    }
    Log("dxgi.dll found at %p", reinterpret_cast<void*>(dxgi));

    HWND hwnd = nullptr;
    for (int i = 0; i < 600; ++i) {
        hwnd = FindFlutterWindow();
        if (hwnd != nullptr) break;
        Sleep(50);
    }
    if (hwnd == nullptr) {
        Log("WARN: flutter main window not found in 30s");
        return 0;
    }

    RECT rc = {};
    GetWindowRect(hwnd, &rc);
    Log("flutter window hwnd=%p rect=%ld,%ld,%ld,%ld",
        reinterpret_cast<void*>(hwnd), rc.left, rc.top, rc.right, rc.bottom);

    // Plugins load only after the UI exists and never under the loader lock:
    // a plugin may touch user32 or create a thread in its own DllMain.
    // The framework derives the DPI scale from hwnd itself (see Host::scale()).
    plugins::SetHostContext(hwnd);
    plugins::Start();

    // debug=true (default): make a successful attach visible without reading
    // proxy.log. Shown last so the dialog never delays plugin startup, and it
    // parks only this worker thread, not the host.
    if (DebugEnabled()) {
        wchar_t msg[512] = {};
        swprintf_s(msg, L"version.dll loaded by %ls\ntarget=%ls",
                   ExeBaseName(), target);
        Log("debug: attach dialog shown");
        MessageBoxW(nullptr, msg, L"pplugin", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
    }

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_resolveMiss = ResolveRealDll();

        // Must run before the host can create its first WebView2 environment
        // (see EnableWebView2Cdp above). kernel32 only, no CRT, no loader-lock
        // hazards.
        EnableWebView2Cdp();

        // No early hook: the proxy does not render. The framework owns the Host
        // API (incl. scale, derived from the host window); plugins own their
        // own windows/UI.

        // Defer all plugin/host work to a worker thread, never under the loader lock.
        HANDLE th = CreateThread(nullptr, 0, PayloadThread, nullptr, 0, nullptr);
        if (th) CloseHandle(th);
    }
    return TRUE;
}
