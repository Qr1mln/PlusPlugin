// ============================================================================
// logger.h - 日志桥：把插件的日志行同时（按需）写到 proxy.log。
//
// 原来的 EmitLog 同时干两件事：1) 收集进 logs 向量；2) 写进 host->publishLog。
// 实测 logs 向量在调用方从未被读取，因此这里只保留“发布到 host 日志”这一职责，
// 接口按用途拆成 publish（自由格式）与 publish_line（已格式化行）。
// ============================================================================
#ifndef FRAMEWORK_LOGGER_H
#define FRAMEWORK_LOGGER_H

#include <string>

struct Host;  // 前向声明，避免在这里引入 pplugin.h 的全部定义

class Logger {
 public:
  // 绑定宿主提供的日志 sink（PluginInit 时调用一次）。
  void set_host(const Host* host) noexcept { host_ = host; }

  // 格式化并发布一行日志，参数是 printf 风格。
  void publish(const char* fmt, ...) noexcept;

  // 发布一条已经格式化好的日志行（不附加任何格式）。
  void publish_line(const std::string& line) noexcept;

 private:
  const Host* host_ = nullptr;
};

#endif  // FRAMEWORK_LOGGER_H
