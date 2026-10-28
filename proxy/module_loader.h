// module_loader.h - 按 JSON 契约加载/热加载模块 dll。
//
// "模块"就是：导出 `int NotifyPlatform(const char* req, char* resp, int cap)`，
// init 请求里收 `{"cmd":"init","dispatch":"<NotifyGhost 指针>","ctx":"0"}`，
// 之后用 `{"cmd":"mainWindow"|"viewWindow"|"scale"}` 反查宿主能力，退出时收
// `{"cmd":"shutdown"}`。**没有共享结构体、没有 GetDesc/ABI 校验**。
//
// 加载顺序：framework.dll 是核心模块（总是加载、失败要告警），
// version.ini `[modules]` 里列的是额外模块（可选，缺文件就跳过并继续监视）。
// 每个模块都独立监视文件标识，被替换就地热加载。
// ============================================================================
#pragma once

#include <windows.h>

#include "protocol.h"

namespace module_loader {

// 加载核心模块 + [modules] 额外模块，并起监视线程。返回值只表示核心模块是否
// 加载成功；热加载失败只写日志。
bool Start(const protocol::HostContext& host);

// 停止监视并卸载全部模块（进程退出不必调用）。
void Stop();

}  // namespace module_loader
