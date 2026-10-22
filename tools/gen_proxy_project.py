"""生成完整的 stub 型代理工程（指定目标 DLL）。

用法: python tools/gen_proxy_project.py version.dll
产物: native/<name>_proxy/{stubs.asm,exports.def,proxy_exports.h,dllmain.cpp,build.bat}
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_def import export_names  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CPP_TEMPLATE = r'''// @@TARGET@@ proxy payload (stub mode)
// Target process: TTVoice.exe (x64), Flutter + ANGLE(D3D11)
//
// Forwarding model:
//   app -> our stub (stubs.asm) -> jmp qword ptr [g_pfn_X] -> real @@TARGET@@!X
//   The real DLL is loaded from %SystemRoot%\System32 by absolute path,
//   so there is no recursion and no version coupling.
//
// Design note: the real DLL is resolved inside DllMain because the app may
// call these APIs before the main window exists. Keep the dependency chain of
// the real DLL trivial (kernel32 only) so early loading cannot disturb the
// graphics stack.
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
static const wchar_t* kTargetDll = L"@@TARGET@@";
static const wchar_t* kLogFile = L"@@LOG@@";

static void Log(const char* fmt, ...) {
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

    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);

    fputc('\n', fp);
    fclose(fp);
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

    Log("payload ready (hook placeholder)");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_resolveMiss = ResolveRealDll();
        HANDLE th = CreateThread(nullptr, 0, PayloadThread, nullptr, 0, nullptr);
        if (th) CloseHandle(th);
    }
    return TRUE;
}
'''

BAT_TEMPLATE = r'''@echo off
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere not found. Install Visual Studio Build Tools first.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo [ERROR] No Visual Studio with MSVC x64 toolset found.
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [ERROR] vcvars64.bat failed
    exit /b 1
)

where ml64 >nul 2>nul
if errorlevel 1 (
    echo [ERROR] ml64.exe MASM assembler not found in PATH
    exit /b 1
)

if exist @@BASE@@.dll del /q @@BASE@@.dll
if exist stubs.obj del /q stubs.obj

ml64 /nologo /c /Fo stubs.obj stubs.asm
if errorlevel 1 (
    echo [ERROR] ml64 failed
    exit /b 1
)

cl /nologo /LD /MT /O2 /EHsc /W4 /utf-8 dllmain.cpp stubs.obj /link user32.lib /DEF:exports.def /OUT:@@BASE@@.dll /MACHINE:X64
if errorlevel 1 (
    echo [ERROR] link failed
    exit /b 1
)

echo.
echo [OK] built: %CD%\@@BASE@@.dll
dir /b @@BASE@@.dll
'''


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="ascii", newline="\r\n") as fh:
        fh.write(text)
    print(f"wrote {path}")


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else "version.dll"
    base = os.path.splitext(target)[0]
    sysroot = os.environ.get("SystemRoot", r"C:\Windows")
    src = os.path.join(sysroot, "System32", target)
    names = [n for n, _ in export_names(src)]
    if not names:
        print(f"no exports parsed from {src}")
        return 1

    out_dir = os.path.join(ROOT, "native", f"{base}_proxy")

    asm = ["; auto-generated, do not edit", "OPTION CASEMAP:NONE", ""]
    asm += [f"EXTERN g_pfn_{n}:QWORD" for n in names]
    asm += ["", ".code", ""]
    for n in names:
        asm += [f"{n} PROC", f"    jmp qword ptr [g_pfn_{n}]", f"{n} ENDP", ""]
    asm += ["END", ""]
    write(os.path.join(out_dir, "stubs.asm"), "\n".join(asm))

    deff = [f"; auto-generated from {src}", f"LIBRARY {base}", "EXPORTS"]
    deff += [f"    {n}" for n in names]
    deff += [""]
    write(os.path.join(out_dir, "exports.def"), "\n".join(deff))

    hdr = ["// auto-generated, do not edit", "#pragma once", "",
           "#define PROXY_EXPORT_LIST(X) \\"]
    hdr += [f"    X({n}) \\" for n in names]
    hdr[-1] = hdr[-1].rstrip(" \\")
    hdr += ["", ""]
    write(os.path.join(out_dir, "proxy_exports.h"), "\n".join(hdr))

    cpp = (CPP_TEMPLATE.replace("@@TARGET@@", target)
                      .replace("@@LOG@@", f"tt_{base}_proxy.log"))
    write(os.path.join(out_dir, "dllmain.cpp"), cpp)

    bat = BAT_TEMPLATE.replace("@@BASE@@", base)
    write(os.path.join(out_dir, "build.bat"), bat)

    print(f"target={target} exports={len(names)} dir={out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
