#include "bootstrap.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <filesystem>

#include "config.h"
#include "log.h"

// The framework is header-only and lives at the repo root. It uses ::Log and
// ::g_logFanout from log.h, so those declarations must be seen first.
#include "plugin_api.inc"
#include "plugin_loader.inc"

namespace {

// Base name of the host EXE, compared against target= so we only act inside
// the configured process and never in a child or an unrelated one.
const wchar_t* ExeBaseName() {
    static const std::wstring name = [] {
        wchar_t buf[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return std::filesystem::path(buf).filename().wstring();
    }();
    return name.c_str();
}

struct WindowSearch {
    DWORD pid;
    HWND found;
};

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<WindowSearch*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid || !IsWindowVisible(hwnd)) {
        return TRUE;
    }
    wchar_t cls[128] = {};
    GetClassNameW(hwnd, cls, 128);
    if (wcscmp(cls, L"FLUTTER_RUNNER_WIN32_WINDOW") == 0) {
        ctx->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

// TTVoice's UI is a Flutter desktop app whose top-level window uses the
// well-known FLUTTER_RUNNER_WIN32_WINDOW class. The framework derives the DPI
// scale from it and the plugins look their FLUTTERVIEW child up themselves.
HWND FindFlutterWindow() {
    WindowSearch ctx{GetCurrentProcessId(), nullptr};
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

bool WaitForDxgi() {
    for (int i = 0; i < 3000; ++i) {   // 30s
        HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
        if (dxgi != nullptr) {
            Log("dxgi.dll found at %p", reinterpret_cast<void*>(dxgi));
            return true;
        }
        Sleep(10);
    }
    return false;
}

HWND WaitForFlutterWindow() {
    for (int i = 0; i < 600; ++i) {   // 30s
        HWND hwnd = FindFlutterWindow();
        if (hwnd != nullptr) {
            return hwnd;
        }
        Sleep(50);
    }
    return nullptr;
}

void ShowAttachDialog(const wchar_t* target) {
    wchar_t msg[512] = {};
    swprintf_s(msg, L"version.dll loaded by %ls\ntarget=%ls", ExeBaseName(), target);
    Log("debug: attach dialog shown");
    MessageBoxW(nullptr, msg, L"pplugin", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
}

}  // namespace

DWORD WINAPI PayloadThread(LPVOID param) {
    (void)param;
    Log("attached exe=%ls resolve_miss=%d", ExeBaseName(), ResolveMissCount());

    const wchar_t* target = TargetExeName();
    if (target[0] == L'\0') {
        Log("skip: no target= in version.ini next to version.dll");
        return 0;
    }
    if (lstrcmpiW(ExeBaseName(), target) != 0) {
        Log("skip: exe=%ls is not the configured target=%ls", ExeBaseName(), target);
        return 0;
    }
    Log("target matched: %ls", target);

    if (!WaitForDxgi()) {
        Log("WARN: dxgi.dll not found in 30s");
        return 0;
    }

    HWND hwnd = WaitForFlutterWindow();
    if (hwnd == nullptr) {
        Log("WARN: flutter main window not found in 30s");
        return 0;
    }
    RECT rc = {};
    GetWindowRect(hwnd, &rc);
    Log("flutter window hwnd=%p rect=%ld,%ld,%ld,%ld", reinterpret_cast<void*>(hwnd), rc.left,
        rc.top, rc.right, rc.bottom);

    // Plugins load only after the UI exists and never under the loader lock:
    // a plugin may touch user32 or create a thread in its own DllMain.
    plugins::SetHostContext(hwnd);
    plugins::Start();

    // framework.dll 现在是 pplugin 插件，由上面的 plugins::Start() / plugin_loader
    // 扫描 plugins\ 自动加载（含热加载），不再在这里直接 LoadLibrary。

    // Shown last so the dialog never delays plugin startup, and it parks only
    // this worker thread, not the host.
    if (DebugEnabled()) {
        ShowAttachDialog(target);
    }
    return 0;
}
