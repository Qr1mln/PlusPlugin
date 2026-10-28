#include "bootstrap.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>

#include "config.h"
#include "log.h"
#include "module_loader.h"
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
    HWND found;      // largest visible, non-minimized Flutter window
    LONG foundArea;
    HWND minimized;  // first minimized one, used only when nothing else exists
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
    if (wcscmp(cls, L"FLUTTER_RUNNER_WIN32_WINDOW") != 0) {
        return TRUE;
    }
    // The host may own several Flutter windows (main UI, image viewer, multi-window
    // dialogs). The main UI is the largest visible one; minimize creates a
    // throwaway 160x30 window, so never pick a minimized one unless it is all we have.
    if (IsIconic(hwnd)) {
        if (ctx->minimized == nullptr) {
            ctx->minimized = hwnd;
        }
        return TRUE;
    }
    RECT rc = {};
    if (!GetWindowRect(hwnd, &rc)) {
        return TRUE;
    }
    const LONG area = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (ctx->found == nullptr || area > ctx->foundArea) {
        ctx->found = hwnd;
        ctx->foundArea = area;
    }
    return TRUE;
}

// TTVoice's UI is a Flutter desktop app whose top-level window uses the
// well-known FLUTTER_RUNNER_WIN32_WINDOW class. The framework derives the DPI
// scale from it and the plugins look their FLUTTERVIEW child up themselves.
HWND FindFlutterWindow() {
    WindowSearch ctx{GetCurrentProcessId(), nullptr, 0, nullptr};
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    return (ctx.found != nullptr) ? ctx.found : ctx.minimized;
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

}  // namespace

// Shared with protocol.cpp: the "refreshWindow" command re-resolves the host
// window pair when the app rebuilds its window (theme switch / multi-window).
HWND FindHostMainWindow() { return FindFlutterWindow(); }

HWND FindHostViewWindow(HWND main) { return ResolveViewWindow(main); }

float HostDpiScale(HWND hwnd) { return ComputeScale(hwnd); }

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

    // 模块只在 UI 就绪后才加载，且绝不在 loader lock 下：framework 要起 WS 服务端
    // 和取点线程。module_loader 会持续监视各模块 dll 的替换，实现热加载。
    protocol::HostContext host = {};
    host.mainWindow = hwnd;
    host.viewWindow = ResolveViewWindow(hwnd);
    host.scale = ComputeScale(hwnd);
    module_loader::Start(host);

    // Shown last so the dialog never delays plugin startup, and it parks only
    // this worker thread, not the host.
    if (DebugEnabled()) {
        ShowAttachDialog(target);
    }
    return 0;
}
