// version.dll proxy payload: forwarding shell + plugin loader.
// Target process: TTVoice.exe (x64), Flutter + ANGLE(D3D11)
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

// Resolve the log file path: next to our own DLL (version.dll) by default, so
// it sits beside the deployed artifact (e.g. %APPDATA%\TTVoice\). Falls back to
// %TEMP% when the module path cannot be resolved.
static void LogFilePath(wchar_t* out, size_t cap) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LogFilePath), &self);
    if (self != nullptr && GetModuleFileNameW(self, out, (DWORD)cap) > 0) {
        wchar_t* slash = wcsrchr(out, L'\\');
        if (slash != nullptr) {
            *(slash + 1) = L'\0';
            if (wcscat_s(out, cap, kLogFile) == 0) return;
        }
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

// Cached base name of the host EXE, used to guard payload so we only act inside
// TTVoice.exe and never a child process or the wrong target.
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

    if (lstrcmpiW(ExeBaseName(), L"TTVoice.exe") != 0) {
        Log("skip: not TTVoice.exe");
        return 0;
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

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_resolveMiss = ResolveRealDll();

        // No early hook: the proxy does not render. The framework owns the Host
        // API (incl. scale, derived from the host window); plugins own their
        // own windows/UI.

        // Defer all plugin/host work to a worker thread, never under the loader lock.
        HANDLE th = CreateThread(nullptr, 0, PayloadThread, nullptr, 0, nullptr);
        if (th) CloseHandle(th);
    }
    return TRUE;
}
