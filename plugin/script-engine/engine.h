// ============================================================================
// engine.h - script-engine 插件内部共享声明（不进 ABI；对外只有 sdk/pplugin.h）
//
// 模块划分：
//   script_plugin.cpp  插件入口（GetDesc / init / tick / shutdown / 崩溃捕获）
//   engine.cpp         命令队列 + 执行线程 + 日志队列/日志线程 + 事件执行
//   tcp_server.cpp     TCP 服务（127.0.0.1:47800）+ 协议解析（nlohmann/json）
//   input_win32.cpp    PP_* 输入原语（PostMessage 合成鼠标/键盘）+ 键名解析
//
// 协议（双向，帧 = 4 字节小端长度 + UTF-8 JSON）：
//   编辑器 -> DLL：{"cmd":"load","loop":N,"events":[...]}
//                  {"cmd":"play"}   {"cmd":"stop"}
//   DLL -> 编辑器：{"t":"log","msg":"..."} / {"t":"done"} / {"t":"stopped"}
// ============================================================================
#pragma once

#include <winsock2.h>   // 必须排在 windows.h 之前，否则会拉进 winsock.h 造成冲突
#include <windows.h>

#include <string>
#include <vector>

#include "../../sdk/pplugin.h"

// 输入原语返回码
#define PP_OK                   0
#define PP_ERR_PARAM           -1
#define PP_ERR_STATE           -2
#define PP_ERR_NO_INJECT       -3   // 后台拿不到目标线程输入状态，只发了裸消息
#define PP_ERR_NOT_IMPLEMENTED -4

// PP_* 输入原语（input_win32.cpp 实现；历史导出，仍保留供外部 ctypes 调用）
extern "C" {
__declspec(dllexport) int __cdecl PP_Mouse(HWND view, int x, int y, int button, int action);
__declspec(dllexport) int __cdecl PP_Delay(int ms);
__declspec(dllexport) int __cdecl PP_FindImage(HWND view, const char* image,
                                               double threshold, int timeout_ms);
__declspec(dllexport) int __cdecl PP_InputText(HWND view, const char* text);
__declspec(dllexport) int __cdecl PP_Key(HWND view, int vk, int ctrl, int alt, int shift,
                                         int win, int action, int real);
__declspec(dllexport) int __cdecl PP_Version(void);
}

namespace se {

constexpr int kPort = 47800;              // 只监听回环，编辑器连这个端口
constexpr uint32_t kMaxFrame = 1u << 20;  // 单帧上限 1MB

struct Event {
    std::string type;      // mouse | delay | findImage | input | key
    std::string action;    // mouse: move/down/up/click；key: press/down/up
    std::string button;    // left | right | middle
    std::string text;      // input
    std::string key;       // key 表达式，如 Ctrl+Shift+A
    std::string image;     // findImage
    int x = 0, y = 0, ms = 0, timeout_ms = 0;
    double threshold = 0.9;
};

enum class CmdType { Load, Play, Stop, ReloadSchedule, GetSchedule, RunScripts, Delay };

struct Command {
    CmdType type = CmdType::Load;
    std::vector<Event> events;
    std::vector<std::string> scripts;   // RunScripts：按名字读脚本文件后顺序执行
    std::string dir;                    // RunScripts：脚本目录（空 = scripts 根目录）
    int loop = 1;
    int ms = 0;                         // Delay：等待毫秒数（脚本之间的间隔）
};

// -- 生命周期（script_plugin.cpp 调）----------------------------------------
bool Start(const Host* host);   // 存 host 并起执行线程 + 日志线程
void Shutdown();                // 停线程；调用前先 tcp::Stop()
const Host* GetHost();          // init 之前 / shutdown 之后为 null
bool Quitting();                // 全局退出标志

// -- 日志：任意线程可调，只入队，由日志线程转发给 host->publishLog 与客户端 ----
void Log(const char* fmt, ...);
void SendRaw(const std::string& json_frame);   // 原样推一帧（done/stopped）

// -- 命令队列（tcp_server.cpp 调）------------------------------------------
void Push(Command cmd);
void RequestStop();

// -- 键名 / 组合键解析（input_win32.cpp 实现）------------------------------
int NameToVk(const std::string& name);
bool ParseKeyCombo(const std::string& expr, int& vk, bool& ctrl, bool& alt,
                   bool& shift, bool& win);

}  // namespace se

namespace tcp {
void Start();                               // 起服务线程（单客户端）
void Stop();                                // 关监听 / 断开并 join
bool Send(const std::string& json_frame);   // 给当前客户端推一帧；无客户端返回 false
}  // namespace tcp
