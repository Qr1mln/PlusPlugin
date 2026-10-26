// version.dll proxy payload: exporting shell + loader bootstrap.
// Target process: whatever target.ini (next to this DLL) names in target=;
// the shipped app is a Flutter + ANGLE(D3D11) host, e.g. TTVoice.exe (x64).
//
// Forwarding model:
//   app -> our stub (stubs.asm) -> jmp qword ptr [g_pfn_X] -> real version.dll!X
//   The real DLL is loaded from %SystemRoot%\System32 by absolute path, so
//   there is no recursion and no version coupling.
//
// Module layout (this file is only the entry point):
//   log.h/.cpp         proxy.log + plugin log fan-out
//   config.h/.cpp      target.ini (Win32 profile API + legacy fallback)
//   bootstrap.h/.cpp   WebView2 CDP switch, window discovery, plugin start
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
//      books callbacks through Host, so the loader can revoke the whole set and
//      then FreeLibrary it.
//
// NOTE: keep the proxy sources pure ASCII.

#include <windows.h>

#include <filesystem>

#include "bootstrap.h"
#include "log.h"
#include "proxy_exports.h"

#pragma comment(lib, "user32.lib")

extern "C" {
#define DEF_PTR(name) void* g_pfn_##name = nullptr;
PROXY_EXPORT_LIST(DEF_PTR)
#undef DEF_PTR
}

namespace {

int g_resolveMiss = -1;

// Populate g_pfn_* with the real System32 version.dll exports (forwarding model).
int ResolveRealDll() {
    wchar_t sysdir[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(sysdir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return -1;
    }
    const std::filesystem::path realPath = std::filesystem::path(sysdir) / L"version.dll";
    HMODULE real = LoadLibraryW(realPath.c_str());
    if (real == nullptr) {
        return -1;
    }

    int missing = 0;
#define RESOLVE(name)                                                     \
    g_pfn_##name = reinterpret_cast<void*>(GetProcAddress(real, #name));  \
    if (g_pfn_##name == nullptr) ++missing;
    PROXY_EXPORT_LIST(RESOLVE)
#undef RESOLVE
    return missing;
}

}  // namespace

int ResolveMissCount() {
    return g_resolveMiss;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_resolveMiss = ResolveRealDll();

        // Must run before the host can create its first WebView2 environment
        // (see EnableWebView2Cdp). kernel32 only, no CRT, no loader-lock
        // hazards.
        EnableWebView2Cdp();

        // No early hook: the proxy does not render. The framework owns the Host
        // API (incl. scale, derived from the host window); plugins own their own
        // windows/UI.

        // Defer all plugin/host work to a worker thread, never under the loader
        // lock. Deliberately CreateThread and not std::thread: starting a C++
        // thread (CRT init, allocation) under the loader lock can deadlock.
        HANDLE th = CreateThread(nullptr, 0, PayloadThread, nullptr, 0, nullptr);
        if (th != nullptr) {
            CloseHandle(th);
        }
    }
    return TRUE;
}
