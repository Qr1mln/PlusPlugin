// ============================================================================
// ws_server.cpp - 见 ws_server.h。
// ============================================================================
#include "ws_server.h"

#include <cstring>
#include <string>

#include "civetweb.h"
#include "command_handler.h"
#include "logger.h"
#include "picker.h"
#include "script_store.h"

WsServer* WsServer::g_log_self_ = nullptr;

bool WsServer::start(Logger& log, CommandHandler& handler, ScriptStore& scripts,
                     const std::string& doc_root, int port) {
  log_ = &log;
  handler_ = &handler;
  scripts_ = &scripts;
  g_log_self_ = this;

  port_opt_ = "127.0.0.1:" + std::to_string(port);
  const char* options[] = {
      "listening_ports",       port_opt_.c_str(),
      "document_root",         doc_root.c_str(),
      "enable_directory_listing", "no",
      "request_timeout_ms",    "30000",  // CivetWeb 只认 *_ms，且无 enable_websocket 选项
      nullptr,
  };

  struct mg_callbacks cb = {};
  cb.log_message = &on_log;  // 把 CivetWeb 启动/运行日志导进 proxy.log
  ctx_ = mg_start(&cb, this, options);
  if (ctx_ == nullptr) {
    log_->publish("mg_start failed");
    return false;
  }
  mg_set_websocket_handler(ctx_, "/ws", &on_ws_connect, &on_ws_ready,
                           &on_ws_data, &on_ws_close, this);
  mg_set_request_handler(ctx_, "/api/scripts", &on_http_scripts, this);
  mg_set_request_handler(ctx_, "/api/app", &on_http_app, this);
  mg_set_request_handler(ctx_, "/api/cursor", &on_http_cursor, this);
  return true;
}

void WsServer::stop() noexcept {
  if (ctx_ != nullptr) {
    mg_stop(ctx_);
    ctx_ = nullptr;
  }
}

int WsServer::on_ws_connect(const mg_connection*, void* /*cbdata*/) {
  return 0;  // 0 = 继续握手
}

void WsServer::on_ws_ready(mg_connection* conn, void* cbdata) {
  auto* self = static_cast<WsServer*>(cbdata);
  PickerAddClient(static_cast<void*>(conn));
  self->log_->publish("websocket client connected");
}

void WsServer::on_ws_close(const mg_connection* conn, void* cbdata) {
  auto* self = static_cast<WsServer*>(cbdata);
  PickerRemoveClient(const_cast<mg_connection*>(conn));
  self->log_->publish("websocket client disconnected");
}

int WsServer::on_ws_data(mg_connection* conn, int /*bits*/, char* data,
                         size_t len, void* cbdata) {
  auto* self = static_cast<WsServer*>(cbdata);
  if (conn == nullptr || data == nullptr) {
    return 1;
  }
  // 收一条命令（MVP 消息很小，单帧足够；多帧场景留待后续）。
  std::string msg(data, len);
  // 取点轮询约 14 次/秒，回显到 proxy.log 会把日志刷爆，直接跳过。
  if (msg.find("\"cursor\"") == std::string::npos) {
    self->log_->publish("recv: %s", msg.c_str());
  }
  const std::string reply = self->handler_->handle(msg, conn);
  mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, reply.c_str(),
                     reply.size());
  return 1;  // 1 = 保持连接
}

// CivetWeb 的日志回调：把它的启动/运行信息（含 mg_start 失败原因）导进 proxy.log。
// 返回 1 表示抑制 civetweb 自身的 stderr 输出（我们已转发）。
// 注意：该回调不带 cbdata，用静态 g_log_self_ 取实例（单例）。
int WsServer::on_log(const mg_connection*, const char* message) {
  if (g_log_self_ != nullptr && message != nullptr) {
    g_log_self_->log_->publish_line(std::string("civetweb: ") + message);
  }
  return 1;
}

std::string WsServer::read_body(mg_connection* conn,
                                const struct mg_request_info* ri) {
  std::string payload;
  if (ri != nullptr && ri->content_length > 0 &&
      ri->content_length < 4 * 1024 * 1024) {
    payload.resize(static_cast<size_t>(ri->content_length));
    size_t got = 0;
    while (got < payload.size()) {
      const int n = mg_read(conn, &payload[got], payload.size() - got);
      if (n <= 0) {
        break;
      }
      got += static_cast<size_t>(n);
    }
    payload.resize(got);
  }
  return payload;
}

void WsServer::send_json(mg_connection* conn, const nlohmann::json& resp) {
  const std::string body = resp.dump();
  mg_send_http_ok(conn, "application/json; charset=utf-8",
                  static_cast<long long>(body.size()));
  mg_write(conn, body.data(), body.size());
}

// GET    /api/scripts              列出
// GET    /api/scripts?name=xx      读取
// POST   /api/scripts?name=xx      新建/保存（body 为脚本 JSON）
// DELETE /api/scripts?name=xx      删除
int WsServer::on_http_scripts(mg_connection* conn, void* cbdata) {
  auto* self = static_cast<WsServer*>(cbdata);
  const struct mg_request_info* ri = mg_get_request_info(conn);
  const std::string method =
      (ri != nullptr && ri->request_method != nullptr) ? ri->request_method
                                                       : "GET";

  char nameBuf[256] = {};
  bool hasName = false;
  if (ri != nullptr && ri->query_string != nullptr &&
      mg_get_var(ri->query_string, strlen(ri->query_string), "name", nameBuf,
                 sizeof(nameBuf)) > 0) {
    hasName = true;
  }
  const std::string name(nameBuf);

  nlohmann::json resp;
  if (hasName && !ScriptStore::safe_name(name)) {
    resp["success"] = false;
    resp["data"] = "bad name";
    resp["code"] = "9999";
  } else if (hasName && method == "DELETE") {
    resp = self->scripts_->remove(name);
  } else if (hasName && (method == "POST" || method == "PUT")) {
    resp = self->scripts_->write(name, read_body(conn, ri));
  } else if (hasName) {
    resp = self->scripts_->read(name);
  } else {
    resp = self->scripts_->list();
  }
  send_json(conn, resp);
  return 200;
}

// GET /api/app: 目标窗口客户区几何 + DPI 缩放（供“刷新应用”展示）。
int WsServer::on_http_app(mg_connection* conn, void* cbdata) {
  auto* self = static_cast<WsServer*>(cbdata);
  nlohmann::json g;
  nlohmann::json resp;
  if (!self->handler_->geometry(g)) {
    resp["success"] = false;
    resp["data"] = "view window unavailable";
    resp["code"] = "9999";
  } else {
    g["app"] = "TTVoice.exe";
    resp["success"] = true;
    resp["data"] = g;
    resp["code"] = "0000";
  }
  send_json(conn, resp);
  return 200;
}

// GET /api/cursor: live cursor readout in view client coords (read-only).
int WsServer::on_http_cursor(mg_connection* conn, void* cbdata) {
  auto* self = static_cast<WsServer*>(cbdata);
  nlohmann::json c;
  nlohmann::json resp;
  if (!self->handler_->cursor(c)) {
    resp["success"] = false;
    resp["data"] = "view window unavailable";
    resp["code"] = "9999";
  } else {
    resp["success"] = true;
    resp["data"] = c;
    resp["code"] = "0000";
  }
  send_json(conn, resp);
  return 200;
}
