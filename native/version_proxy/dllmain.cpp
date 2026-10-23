// version.dll proxy payload: forwarding shell + early hooks + plugin loader.
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
// Framework duties (this module is the framework, see ttplugin.h):
//   1. Forward every version.dll export to the real one (stubs.asm).
//   2. Own every hook installation: the D3D11 present patch and the early
//      GetProcAddress interception. Plugins never install hooks themselves,
//      which is exactly what makes a plugin unloadable at runtime.
//   3. Load and unload plugins from %APPDATA%\TTVoice\plugins. A plugin only
//      books callbacks through TTPluginHost, so the loader can revoke the
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
static const wchar_t* kLogFile = L"tt_version_proxy.log";

// Installed by the framework package once plugins may be loaded. Declared
// before Log() so the logger can mirror every line to the plugin log sinks.
static void (*g_logFanout)(const char* line) = nullptr;
static const int kLogLineChars = 1024;

static void Log(const char* fmt, ...) {
    char line[kLogLineChars] = {};

    va_list ap;
    va_start(ap, fmt);
    vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);

    wchar_t temp[MAX_PATH] = {};
    DWORD n = GetTempPathW(MAX_PATH, temp);
    if (n == 0 || n >= MAX_PATH) return;

    wchar_t path[MAX_PATH + 32] = {};
    swprintf_s(path, L"%s%s", temp, kLogFile);

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

#include "overlay.inc"
#include "framework/plugin_loader.inc"  // pulls plugin_api.inc and sdk/ttplugin.h

namespace ov {
static void LogFromOverlay(const char* text) {
    Log("%s", text);
    PushScreenLog(text);  // mirror onto the overlay when log_on_screen=1
}
}  // namespace ov

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

static UINT QueryDpi(HWND hwnd) {
    using PFN = UINT(WINAPI*)(HWND);
    static PFN fn = reinterpret_cast<PFN>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    return fn ? fn(hwnd) : 0;
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

static HWND FindFlutterWindow() {
    WindowSearch ctx{GetCurrentProcessId(), nullptr};
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

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
    Log("flutter window hwnd=%p rect=%ld,%ld,%ld,%ld dpi=%u",
        reinterpret_cast<void*>(hwnd), rc.left, rc.top, rc.right, rc.bottom, QueryDpi(hwnd));

    const bool overlayReady = ov::Init(hwnd);
    Log("overlay init=%d scale=%.2f", overlayReady ? 1 : 0, ov::g_scale);

    // Plugins load only after the UI exists and never under the loader lock:
    // a plugin may touch user32 or create a thread in its own DllMain.
    plugins::SetHostContext(hwnd, ov::g_scale);
    plugins::Start();

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_resolveMiss = ResolveRealDll();

        // Start the D3D11/GetProcAddress patch as early as possible: ANGLE
        // creates its device during startup, before the main window exists.
        if (lstrcmpiW(ExeBaseName(), L"TTVoice.exe") == 0) {
            ov::StartD3D11Hook();
        }

        HANDLE th = CreateThread(nullptr, 0, PayloadThread, nullptr, 0, nullptr);
        if (th) CloseHandle(th);
    }
    return TRUE;
}
