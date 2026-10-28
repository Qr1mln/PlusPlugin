// ============================================================================
// command_handler.h - 命令解析与分发（WebSocket 文本帧 / 供 HTTP 复用）。
//
// 解析 {id,type,action,space,button,...}（兼容旧 action/op），分发到输入派发、
// 取点（picker）等子模块，返回统一封套字符串：
//   {id, success, data, code}，code "0000"=成功 "9999"=失败。
// 同时把若干几何/光标读数封装成 JSON，供 /api/app、/api/cursor 直接复用。
//
// JSON 用共享的 common/json_util（无第三方、无 STL 容器）。
// ============================================================================
#ifndef FRAMEWORK_COMMAND_HANDLER_H
#define FRAMEWORK_COMMAND_HANDLER_H

#include <string>

#include "../common/json_util.h"

#include "window_target.h"

class Logger;
class InputDispatcher;

class CommandHandler {
 public:
  CommandHandler(Logger& log, WindowTarget& win, InputDispatcher& input)
      : log_(log), win_(win), input_(input) {}

  // 解析并分发一条 WebSocket 命令。conn 为发起连接（cursor watch/pick 需要）。
  std::string handle(const std::string& msg, void* conn);

  // 几何信息 {x,y,w,h,scale}；窗口不可用时返回 false。
  bool geometry(ppjson::Value& out) const { return win_.geometry(out); }

  // 一次性光标读数（与推送帧同 payload）；窗口不可用时返回 false。
  bool cursor(ppjson::Value& out) const;

 private:
  static std::string make_reply(ppjson::Pool& pool, const char* id, bool success,
                                ppjson::Value data, const char* code);

  Logger& log_;
  WindowTarget& win_;
  InputDispatcher& input_;
};

#endif  // FRAMEWORK_COMMAND_HANDLER_H