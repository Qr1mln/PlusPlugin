// ============================================================================
// ws_server.cpp - 见 ws_server.h。
// ============================================================================
#include "ws_server.h"

#include <cstdio>
#include <cstring>

#include "civetweb.h"
#include "command_handler.h"
#include "picker.h"
#include "script_store.h"

#include "../common/log.h"

namespace {
const char* kOk = "0000";
const char* kErr = "9999";

const int kHttpPoolNodes = 256;
const int kReplyCap = 16384;   // WS 响应缓冲（脚本列表等可能偏大）
const int kMsgCap = 16384;     // 一帧命令文本上限
const int kBodyCap = 4 * 1024 * 1024;  // 请求体上限（与原 4MB 一致）
}  // namespace

bool WsServer::start(CommandHandler& handler, ScriptStore& scripts,
                     const char* doc_root, int port) {
  handler_ = &handler;
  scripts_ = &scripts;

  std::snprintf(port_opt_, sizeof(port_opt_), "127.0.0.1:%d", port);
  const char* options[] = {
      "listening_ports",          port_opt_,
      "document_root",            (doc_root != nullptr) ? doc_root : "",
      "enable_directory_listing", "no",
      "request_timeout_ms",       "30000",
      nullptr,
  };

  struct mg_callbacks cb = {};
  cb.log_message = &on_log;
  ctx_ = mg_start(&cb, this, options);
  if (ctx_ == nullptr) {
    LogTagged("framework", "mg_start failed");
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
  return 0;
}

void WsServer::on_ws_ready(mg_connection* conn, void* cbdata) {
  auto* self = static_cast<WsServer*>(cbdata);
  PickerAddClient(static_cast<void*>(conn));
  LogTagged("framework", "websocket client connected");
}

void WsServer::on_ws_close(const mg_connection* conn, void* cbdata) {
  auto* self = static_cast<WsServer*>(cbdata);
  PickerRemoveClient(const_cast<mg_connection*>(conn));
  LogTagged("framework", "websocket client disconnected");
}

int WsServer::on_ws_data(mg_connection* conn, int /*bits*/, char* data,
                         size_t len, void* cbdata) {
  auto* self = static_cast<WsServer*>(cbdata);
  if (conn == nullptr || data == nullptr || len >= static_cast<size_t>(kMsgCap)) {
    return 1;
  }
  // 复制到 NUL 结尾的缓冲，供解析与日志使用。
  char msg[kMsgCap] = {};
  std::memcpy(msg, data, len);
  msg[len] = '\0';

  // 取点轮询频繁，跳过回显。
  if (std::strstr(msg, "\"cursor\"") == nullptr) {
    LogTagged("framework", "recv: %s", msg);
  }
  char reply[kReplyCap] = {};
  const int n = self->handler_->handle(msg, reply, kReplyCap, conn);
  if (n > 0) {
    mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, reply,
                       static_cast<size_t>(n));
  }
  return 1;
}

int WsServer::on_log(const mg_connection*, const char* message) {
  if (message != nullptr) {
    LogTagged("framework", "civetweb: %s", message);
  }
  return 1;
}

int WsServer::read_body(mg_connection* conn, const struct mg_request_info* ri,
                        char* out, int cap) {
  if (out == nullptr || cap <= 0) {
    return 0;
  }
  out[0] = '\0';
  if (ri == nullptr || ri->content_length <= 0 ||
      ri->content_length >= kBodyCap || ri->content_length >= cap) {
    return 0;
  }
  int got = 0;
  while (got < static_cast<int>(ri->content_length)) {
    const int n = mg_read(conn, out + got, static_cast<size_t>(cap - 1 - got));
    if (n <= 0) {
      break;
    }
    got += n;
  }
  out[got] = '\0';
  return got;
}

void WsServer::send_json(mg_connection* conn, ppjson::Value resp) {
  char body[16384] = {};
  const int n = resp.Dump(body, sizeof(body));
  const long long len = (n >= 0) ? static_cast<long long>(n) : 0;
  mg_send_http_ok(conn, "application/json; charset=utf-8", len);
  mg_write(conn, body, static_cast<size_t>(len));
}

int WsServer::on_http_scripts(mg_connection* conn, void* cbdata) {
  auto* self = static_cast<WsServer*>(cbdata);
  const struct mg_request_info* ri = mg_get_request_info(conn);
  const char* method =
      (ri != nullptr && ri->request_method != nullptr) ? ri->request_method : "GET";

  char nameBuf[256] = {};
  bool hasName = false;
  if (ri != nullptr && ri->query_string != nullptr &&
      mg_get_var(ri->query_string, strlen(ri->query_string), "name", nameBuf,
                 sizeof(nameBuf)) > 0) {
    hasName = true;
  }

  ppjson::Node nodes[kHttpPoolNodes] = {};
  ppjson::Pool pool(nodes, kHttpPoolNodes);
  ppjson::Value resp = ppjson::Value::Obj(pool);

  if (hasName && !ScriptStore::safe_name(nameBuf)) {
    resp.SetBool("success", false);
    resp.SetStr("data", "bad name");
    resp.SetStr("code", kErr);
  } else if (hasName && std::strcmp(method, "DELETE") == 0) {
    self->scripts_->remove(pool, nameBuf, resp);
  } else if (hasName &&
             (std::strcmp(method, "POST") == 0 || std::strcmp(method, "PUT") == 0)) {
    char body[kBodyCap] = {};
    read_body(conn, ri, body, kBodyCap);
    self->scripts_->write(pool, nameBuf, body, resp);
  } else if (hasName) {
    self->scripts_->read(pool, nameBuf, resp);
  } else {
    self->scripts_->list(pool, resp);
  }
  send_json(conn, resp);
  return 200;
}

int WsServer::on_http_app(mg_connection* conn, void* cbdata) {
  auto* self = static_cast<WsServer*>(cbdata);
  ppjson::Node nodes[kHttpPoolNodes] = {};
  ppjson::Pool pool(nodes, kHttpPoolNodes);
  ppjson::Value g = ppjson::Value::Obj(pool);
  ppjson::Value resp = ppjson::Value::Obj(pool);
  if (!self->handler_->geometry(g)) {
    resp.SetBool("success", false);
    resp.SetStr("data", "view window unavailable");
    resp.SetStr("code", kErr);
  } else {
    g.SetStr("app", "TTVoice.exe");
    resp.SetBool("success", true);
    resp.SetValue("data", g);
    resp.SetStr("code", kOk);
  }
  send_json(conn, resp);
  return 200;
}

int WsServer::on_http_cursor(mg_connection* conn, void* cbdata) {
  auto* self = static_cast<WsServer*>(cbdata);
  ppjson::Node nodes[kHttpPoolNodes] = {};
  ppjson::Pool pool(nodes, kHttpPoolNodes);
  ppjson::Value c = ppjson::Value::Obj(pool);
  ppjson::Value resp = ppjson::Value::Obj(pool);
  if (!self->handler_->cursor(c)) {
    resp.SetBool("success", false);
    resp.SetStr("data", "view window unavailable");
    resp.SetStr("code", kErr);
  } else {
    resp.SetBool("success", true);
    resp.SetValue("data", c);
    resp.SetStr("code", kOk);
  }
  send_json(conn, resp);
  return 200;
}