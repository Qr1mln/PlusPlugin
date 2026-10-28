#include "bootstrap.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>

#include "config.h"
#include "log.h"

#include "protocol.h"

namespace {

// Base name of the host EXE, compared against target= so we only act inside
// the configured process and never in a child or an unrelated one.
const wchar_t* ExeBaseName() {
    static const wchar_t* name = [] {
        static wchar_t buf[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        const wchar_t* slash = wcsrchr(buf, L'\\');
        return (slash != nullptr) ? slash + 1 : buf;
    }();
    return name;
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

// ---- 新链路辅助：宿主能力 + framework 直连 ----
float ComputeScale(HWND hwnd) {
    using PFN = UINT(WINAPI*)(HWND);
    static PFN getDpi = reinterpret_cast<PFN>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    const UINT dpi = (getDpi && hwnd) ? getDpi(hwnd) : 96;
    return (dpi > 0) ? (static_cast<float>(dpi) / 96.0f) : 1.0f;
}

struct ViewCtx {
    HWND first;
    HWND visible;
};

BOOL CALLBACK EnumViewProc(HWND child, LPARAM lp) {
    wchar_t cls[64] = {};
    if (GetClassNameW(child, cls, 63) <= 0 || lstrcmpiW(cls, L"FLUTTERVIEW") != 0) {
        return TRUE;
    }
    ViewCtx* ctx = reinterpret_cast<ViewCtx*>(lp);
    if (ctx->first == nullptr) {
        ctx->first = child;
    }
    if (IsWindowVisible(child) != FALSE) {
        ctx->visible = child;
        return FALSE;
    }
    return TRUE;
}

HWND ResolveViewWindow(HWND main) {
    if (main == nullptr) {
        return nullptr;
    }
    ViewCtx ctx{nullptr, nullptr};
    EnumChildWindows(main, &EnumViewProc, reinterpret_cast<LPARAM>(&ctx));
    return (ctx.visible != nullptr) ? ctx.visible : ctx.first;
}

// 加载同目录 framework.dll 并走 JSON 协议（PutPlatform）。
void LoadFrameworkViaProtocol(HWND hwnd) {
    const wchar_t* dir = ModuleDir();
    if (dir[0] == L'\0') {
        Log("WARN: module dir unknown, cannot load framework");
        return;
    }
    wchar_t path[MAX_PATH] = {};
    swprintf_s(path, MAX_PATH, L"%s\\framework.dll", dir);
    HMODULE fw = LoadLibraryW(path);
    if (fw == nullptr) {
        Log("WARN: LoadLibrary %ls failed err=%lu", path, GetLastError());
        return;
    }
    protocol::HostContext host = {};
    host.mainWindow = hwnd;
    host.viewWindow = ResolveViewWindow(hwnd);
    host.scale = ComputeScale(hwnd);
    if (!protocol::ProtocolStart(fw, host)) {
        Log("WARN: ProtocolStart failed");
        FreeLibrary(fw);
        return;
    }
    Log("framework via protocol: main=%p view=%p scale=%.2f",
        reinterpret_cast<void*>(host.mainWindow), reinterpret_cast<void*>(host.viewWindow),
        host.scale);
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
    // 新链路：直接加载同目录 framework.dll，经 JSON 协议（PutPlatform）启动。
    LoadFrameworkViaProtocol(hwnd);

    // Shown last so the dialog never delays plugin startup, and it parks only
    // this worker thread, not the host.
    if (DebugEnabled()) {
        ShowAttachDialog(target);
    }
    return 0;
}
