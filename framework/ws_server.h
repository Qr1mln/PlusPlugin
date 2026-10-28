// ============================================================================
// ws_server.h - CivetWeb 封装：WS 端点 /ws + HTTP REST 端点。
//
// 只负责网络层：把 CivetWeb 的 C 回调转成成员调用，再转发给 CommandHandler /
// ScriptStore。日志统一走公共 log 模块。无 STL：参数/缓冲用 const char* / char[]。
// ============================================================================
#ifndef FRAMEWORK_WS_SERVER_H
#define FRAMEWORK_WS_SERVER_H

#include <cstddef>

#include "../common/json.h"

struct mg_context;
struct mg_connection;

class CommandHandler;
class ScriptStore;

class WsServer {
 public:
  // 启动 CivetWeb：监听 127.0.0.1:<port>，doc_root 用于托管静态文件。
  bool start(CommandHandler& handler, ScriptStore& scripts, const char* doc_root,
             int port);

  void stop() noexcept;

 private:
  static int on_ws_connect(const mg_connection*, void* cbdata);
  static void on_ws_ready(mg_connection* conn, void* cbdata);
  static int on_ws_data(mg_connection* conn, int bits, char* data, size_t len,
                        void* cbdata);
  static void on_ws_close(const mg_connection*, void* cbdata);
  static int on_log(const mg_connection* conn, const char* message);

  static int on_http_scripts(mg_connection* conn, void* cbdata);
  static int on_http_app(mg_connection* conn, void* cbdata);
  static int on_http_cursor(mg_connection* conn, void* cbdata);

  // 读请求体（限 4MB），写入 out，返回字节数。
  static int read_body(mg_connection* conn, const struct mg_request_info* ri,
                       char* out, int cap);

  // 发送 JSON 响应（HTTP 200 + application/json）。
  static void send_json(mg_connection* conn, ppjson::Value resp);

  mg_context* ctx_ = nullptr;
  CommandHandler* handler_ = nullptr;
  ScriptStore* scripts_ = nullptr;
  char port_opt_[32] = {};  // "127.0.0.1:<port>"，mg_start 期间保持有效
};

#endif  // FRAMEWORK_WS_SERVER_H