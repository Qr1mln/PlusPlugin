// ============================================================================
// command_handler.h - 命令解析与分发（WebSocket 文本帧 / 供 HTTP 复用）。
//
// 解析 {id,type,action,space,button,...}（兼容旧 action/op），分发到输入派发、
// 取点（picker）等子模块，把统一封套写进调用方 buffer：
//   {id, success, data, code}，code "0000"=成功 "9999"=失败。
//
// 无 STL：命令与响应都走 const char* / char buffer。
// ============================================================================
#ifndef FRAMEWORK_COMMAND_HANDLER_H
#define FRAMEWORK_COMMAND_HANDLER_H

#include <windows.h>

#include "../common/json.h"

#include "window_target.h"

class InputDispatcher;

class CommandHandler {
 public:
  CommandHandler(WindowTarget& win, InputDispatcher& input)
      : win_(win), input_(input) {}

  // 解析并分发一条命令（msg 为 NUL 结尾 UTF-8）。响应写入 out，返回字节数
  // （>=0），或 -(need+1)（out 不够）。conn 为发起连接（cursor watch/pick 需要）。
  int handle(const char* msg, char* out, int cap, void* conn);

  bool geometry(ppjson::Value& out) const { return win_.geometry(out); }
  bool cursor(ppjson::Value& out) const;

 private:
  static int make_reply(ppjson::Pool& pool, char* out, int cap, const char* id,
                        bool success, ppjson::Value data, const char* code);

  WindowTarget& win_;
  InputDispatcher& input_;
};

#endif  // FRAMEWORK_COMMAND_HANDLER_H