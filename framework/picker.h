// ============================================================================
// picker.h - 取点模块：光标位置实时推送 + 左键拾取（原 Python 版 picker.py 的 C++ 实现）。
//
// 为什么放 DLL：取点是只读查询（GetCursorPos/ScreenToClient）加一次低级鼠标钩子，
// 必须在宿主进程里做；做成 DLL 后，web 端不再轮询，由服务端主动下发。
//
// 协议（服务端 -> 客户端的主动帧，不带 id，不属于任何请求的应答）：
//   {"t":"cursor", sx,sy,x,y,lx,ly,w,h,scale,inside,f8}   ~30Hz 光标读数
//   {"t":"pick","action":"ready"}                        已进入拾取态
//   {"t":"pick","sx","sy","x","y","lx","ly","scale"}     拾取到的点（点击被吞掉）
//   {"t":"pick","action":"cancel"}                       右键或命令取消
//   {"t":"pick","action":"failed","error":"..."}         装钩子失败
//
// 客户端命令（走 /ws）：
//   {"type":"cursor","action":"watch"|"unwatch"}   订阅/退订光标流（按连接记）
//   {"type":"pick","action":"start"|"cancel"}      开始/取消拾取
//
// 并发模型：模块内起一条消息循环线程，它同时承担
//   1) SetTimer 驱动的光标采样（约 30Hz）；
//   2) WH_MOUSE_LL 钩子的宿主线程（钩子回调只在该线程泵消息时被派发）。
// 因此钩子回调里只做「算坐标 + 入队」，真正的广播在线程的消息循环里完成。
// ============================================================================
#ifndef PICKER_H
#define PICKER_H

#include <cstddef>

#include "../common/json.h"

#include "pplugin.h"

// 把一帧文本写给某个 WS 连接（由 framework.cpp 用 mg_websocket_write 实现）。
using PickerBroadcast = void (*)(void* conn, const char* data, size_t len);

// 启动/停止推送线程。日志走公共 log 模块（组件 framework），broadcast 用于下发。
bool PickerStart(const Host* host, PickerBroadcast broadcast);
void PickerStop();          // 停线程并等待退出（PluginShutdown 用）
void PickerRequestStop();   // 只发退出信号、不等待（DllMain 里不能 join）

// 连接生命周期：framework.cpp 在 WS ready / close 回调里通知本模块。
void PickerAddClient(void* conn);
void PickerRemoveClient(void* conn);

// 命令入口：订阅/退订光标流、开始/取消拾取。
bool PickerSetWatch(void* conn, bool on);
bool PickerBeginPick();
void PickerCancelPick();

// 一次性读数（与推送帧同 payload），供 /api/cursor 与 {"type":"cursor"} 复用。
bool PickerCursorInfo(ppjson::Value& out);

#endif  // PICKER_H
