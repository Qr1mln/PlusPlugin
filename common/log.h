// common/log.h - 公共单行日志模块。
//
// proxy(version.dll) 与 framework(framework.dll) 都编译本文件，各自得到一份静态
// 副本；用进程内命名互斥量跨 DLL 串行化写，保证同一进程内两个模块并发写同一
// proxy.log 不会交错。
//
// 日志文件固定为「本模块所在目录 \ proxy.log」（两 DLL 都在 %APPDATA%\TTVoice，
// 故指向同一文件）。行格式：[HH:MM:SS.mmm]-[component] text\n
//
// 代理侧保留 g_logFanout：把每行镜像给已订阅的插件日志 sink（旧 pplugin 路径）。
// 新直连链路下通常为 null，可安全忽略。
#pragma once

// 代理侧日志 fan-out（镜像给插件订阅者）。新链路通常 null。声明在此以兼容 plugin.cpp。
extern void (*g_logFanout)(const char* line);

// printf 风格；组件默认 "proxy"。
void Log(const char* fmt, ...);

// 显式指定 component（如 "framework" / "platform"）。
void LogTagged(const char* component, const char* fmt, ...);
