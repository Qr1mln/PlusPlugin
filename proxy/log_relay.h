// log_relay.h - 把 proxy 每行日志镜像给模块注册的 sink。
//
// 与模块约定：模块在 init 之后调 `{"cmd":"logSink","ptr":"<回调指针>"}` 注册，
// 回调签名 `void (__cdecl*)(const char* line, void* user)`，可能在任意线程被调。
// 卸载模块前必须 ClearSink()，否则会调到已释放的代码。
//
// 通道本体是 common/log 里预留的 g_logFanout（旧 pplugin 的订阅入口），这里只是
// 把它接到当前注册的模块 sink 上，并做防递归。
// ============================================================================
#pragma once

namespace log_relay {

typedef void (__cdecl* SinkFn)(const char* line, void* user);

// 挂上 common/log 的 fan-out 出口（进程内只需一次）。
void Start();

// 模块注册/取消；fn 为空表示取消。
bool SetSink(void* fn);

// 卸载模块前调用。
void ClearSink();

}  // namespace log_relay
