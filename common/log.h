// common/log.h - 公共单行日志模块。
//
// proxy(version.dll) 与 framework(framework.dll) 都编译本文件，各自得到一份静态
// 副本；用进程内命名互斥量跨 DLL 串行化写，保证同一进程内两个模块并发写同一
// proxy.log 不会交错。
//
// 日志文件固定为「本模块所在目录 \ proxy.log」（两 DLL 都在 %APPDATA%\TTVoice，
// 故指向同一文件）。行格式：[HH:MM:SS.mmm]-[component] text\n
//
// 代理侧保留 g_logFanout：把每行镜像给已注册的模块日志 sink（proxy/log_relay
// 用它把日志推给 console 之类的 JSON 模块）。没人订阅时为 null。
#pragma once

// 日志 fan-out 出口（只传正文，不带时间戳/组件前缀）。由 log_relay 挂接。
extern void (*g_logFanout)(const char* line);

// printf 风格；组件默认 "proxy"。
void Log(const char* fmt, ...);

// 显式指定 component（如 "framework" / "platform"）。
void LogTagged(const char* component, const char* fmt, ...);
