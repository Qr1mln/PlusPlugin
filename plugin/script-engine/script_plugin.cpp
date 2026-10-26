// ============================================================================
// script_plugin.cpp - 插件入口（pplugin 契约的唯一对外部分）
//
// 其余实现见同目录：
//   engine.h / engine.cpp   命令队列、执行线程、日志线程、事件执行
//   tcp_server.cpp          TCP 服务（127.0.0.1:47800）与协议解析
//   input_win32.cpp         PP_* 输入原语与键名解析
//
// 契约要点：init 里起线程（被明确放行的例外）、shutdown 里 join；
// 执行权在宿主进程内，鼠标/键盘消息直接投给宿主自己的 FLUTTERVIEW 窗口。
// ============================================================================
#include "engine.h"

#include <cstdio>

#include "schedule.h"

namespace {

const char* kCrashLogPath = "C:\\Users\\111\\AppData\\Roaming\\TTVoice\\script_crash.log";

// 执行期崩溃捕获：把异常码/地址落盘，避免宿主进程直接崩，便于定位。
void DbgCrash(const char* where, unsigned code, void* addr) {
    char buf[256];
    const int n = snprintf(buf, sizeof buf, "[script-engine] %s 捕获异常: code=0x%08X addr=%p\n",
                           where, code, addr);
    HANDLE f = CreateFileA(kCrashLogPath, GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        SetFilePointer(f, 0, nullptr, FILE_END);
        DWORD written = 0;
        WriteFile(f, buf, static_cast<DWORD>(n), &written, nullptr);
        CloseHandle(f);
    }
}

// 全局未处理异常过滤器：记录崩溃地址后只结束当前（出错的）线程，宿主进程得以存活。
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    const unsigned code = (ep != nullptr) ? ep->ExceptionRecord->ExceptionCode : 0;
    void* addr = (ep != nullptr) ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    DbgCrash("Unhandled", code, addr);
    ExitThread(static_cast<DWORD>(-1));
    return EXCEPTION_EXECUTE_HANDLER;
}

int __cdecl OnPluginInit(const Host* host) {
    if (host == nullptr || host->abi != P_ABI) return ERR_PARAM;
    SetUnhandledExceptionFilter(CrashFilter);
    if (!se::Start(host)) {
        if (host->log != nullptr) host->log("script-engine: 引擎线程启动失败");
        return ERR_STATE;
    }
    tcp::Start();
    sched::Start();
    if (host->log != nullptr) {
        host->log("script-engine: 进程内引擎已启动（TCP 127.0.0.1:47800）");
    }
    return P_OK;
}

int __cdecl OnPluginTick(void) {
    return 0;
}

int __cdecl OnPluginShutdown(void) {
    sched::Stop();   // 先停调度，避免它在引擎收尾时再压命令
    tcp::Stop();
    se::Shutdown();
    return P_OK;
}

const Desc kDesc = {
    P_ABI,
    sizeof(Desc),
    "script-engine",
    "0.3.0",
    &OnPluginInit,
    &OnPluginTick,
    &OnPluginShutdown
};

}  // namespace

extern "C" __declspec(dllexport) const Desc* __cdecl GetDesc(void) {
    return &kDesc;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    (void)module;
    (void)reason;
    (void)reserved;
    return TRUE;
}
