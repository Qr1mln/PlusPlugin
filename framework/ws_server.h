// ============================================================================
// ws_server.h - CivetWeb 封装：WS 端点 /ws + HTTP REST 端点。
//
// 只负责网络层：把 CivetWeb 的 C 回调转成语法上的成员调用（通过 cbdata /
// user_data 拿到 WsServer* 实例），再转发给 Logger / CommandHandler /
// ScriptStore。具体的命令解析、脚本读写都不在这里。
//
// JSON 用共享的 common/json_util。
// ============================================================================
#ifndef FRAMEWORK_WS_SERVER_H
#define FRAMEWORK_WS_SERVER_H

#include <string>

#include "../common/json_util.h"

struct mg_context;
struct mg_connection;

class Logger;
class CommandHandler;
class ScriptStore;

class WsServer {
 public:
  // 启动 CivetWeb：监听 127.0.0.1:<port>，doc_root 用于托管静态文件。
  // 返回 false 表示 mg_start 失败（上下文未建立）。
  bool start(Logger& log, CommandHandler& handler, ScriptStore& scripts,
             const std::string& doc_root, int port);

  // 停止并释放 CivetWeb 上下文（幂等）。
  void stop() noexcept;

 private:
  // ---- CivetWeb 回调（静态跳板，cbdata == this）----
  static int on_ws_connect(const mg_connection*, void* cbdata);
  static void on_ws_ready(mg_connection* conn, void* cbdata);
  static int on_ws_data(mg_connection* conn, int bits, char* data, size_t len,
                        void* cbdata);
  static void on_ws_close(const mg_connection*, void* cbdata);
  static int on_log(const mg_connection* conn, const char* message);

  // ---- HTTP REST 端点 ----
  static int on_http_scripts(mg_connection* conn, void* cbdata);
  static int on_http_app(mg_connection* conn, void* cbdata);
  static int on_http_cursor(mg_connection* conn, void* cbdata);

  // 读请求体（限 4MB）。
  static std::string read_body(mg_connection* conn,
                               const struct mg_request_info* ri);

  // 发送 JSON 响应（HTTP 200 + application/json）。
  static void send_json(mg_connection* conn, ppjson::Value resp);

  mg_context* ctx_ = nullptr;
  Logger* log_ = nullptr;
  CommandHandler* handler_ = nullptr;
  ScriptStore* scripts_ = nullptr;
  std::string port_opt_;  // 让 options 数组里的端口字符串在 mg_start 期间保持有效

  // log_message 回调没有 cbdata，用这个静态指针拿实例（本 DLL 只有这一个服务）。
  static WsServer* g_log_self_;
};

#endif  // FRAMEWORK_WS_SERVER_H