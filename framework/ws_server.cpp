// ============================================================================
// ws_server.cpp - 见 ws_server.h。
// ============================================================================
#include "ws_server.h"

#include <cstdio>
#include <cstring>

#include "civetweb.h"

#include <windows.h>  // HeapAlloc：HTTP 请求缓冲不能放栈（CivetWeb 线程栈很小）

#include "command_handler.h"
#include "picker.h"
#include "script_store.h"

#include "../common/log.h"

namespace {
const char* kOk = "0000";
const char* kErr = "9999";

const int kHttpPoolNodes = 256;
const int kReplyCap = 16384;          // WS 响应缓冲（脚本列表等可能偏大）
const int kMsgCap = 16384;            // 一帧命令文本上限
const int kBodyCap = 4 * 1024 * 1024; // 请求体上限（与原 4MB 一致）
const int kHttpReplyCap = 65536;      // HTTP 响应缓冲（脚本正文最大 64KB）

// --- 堆上的请求缓冲 ---------------------------------------------------------
// CivetWeb 的请求处理跑在它自己的工作线程里，栈很小。曾经把 4MB 的请求体
// （POST /api/scripts 保存脚本）和 256 个 ppjson::Node（约 150KB）放在栈上，
// 一点保存就直接把宿主进程栈打爆 -> TTVoice 无任何日志闪退。
// 现在统一走进程堆，RAII 归还。
struct HeapNodes {
  ppjson::Node* nodes;
  ppjson::Pool pool;

  explicit HeapNodes(int n)
      : nodes(static_cast<ppjson::Node*>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                      sizeof(ppjson::Node) * static_cast<size_t>(n)))),
        pool(nodes, n) {}

  ~HeapNodes() {
    if (nodes != nullptr) {
      HeapFree(GetProcessHeap(), 0, nodes);
    }
  }

  bool ok() const { return nodes != nullptr; }

  HeapNodes(const HeapNodes&) = delete;
  HeapNodes& operator=(const HeapNodes&) = delete;
};

struct HeapText {
  char* p;

  explicit HeapText(int cap)
      : p(static_cast<char*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                       static_cast<size_t>(cap)))) {}

  ~HeapText() {
    if (p != nullptr) {
      HeapFree(GetProcessHeap(), 0, p);
    }
  }

  bool ok() const { return p != nullptr; }

  HeapText(const HeapText&) = delete;
  HeapText& operator=(const HeapText&) = delete;
};

int RequestBodyCap(const struct mg_request_info* ri) {
  if (ri == nullptr || ri->content_length <= 0) {
    return 1;
  }
  const long long need = static_cast<long long>(ri->content_length) + 1;
  return (need > kBodyCap) ? kBodyCap : static_cast<int>(need);
}

// 内存分配失败时的兜底响应（不再继续用空指针去构建 JSON）。
int SendOom(mg_connection* conn) {
  static const char kOom[] = "{\"success\":false,\"data\":\"out of memory\",\"code\":\"9999\"}";
  mg_send_http_ok(conn, "application/json; charset=utf-8", sizeof(kOom) - 1);
  mg_write(conn, kOom, sizeof(kOom) - 1);
  return 500;
}
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

int WsServer::on_ws_data(mg_connection* conn, int bits, char* data,
                         size_t len, void* cbdata) {
  auto* self = static_cast<WsServer*>(cbdata);
  if (conn == nullptr) {
    return 1;
  }

  // CivetWeb 把 close/ping/pong 帧也交给本回调（opcode 在 bits 低 4 位），
  // 它们不是命令文本。以前一律当文本处理，close 帧的状态码（1000 => 0x03 0xE8）
  // 被 %s 打成一行乱码 "recv: ?"，还会往正在关闭的连接回一条 bad json。
  const int opcode = bits & 0x0F;
  if (opcode == MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE) {
    int code = 1005;  // 帧内没带状态码
    if (data != nullptr && len >= 2) {
      code = (static_cast<unsigned char>(data[0]) << 8) |
             static_cast<unsigned char>(data[1]);
    }
    LogTagged("framework", "websocket close frame from client, code=%d", code);
    return 1;
  }
  if (opcode != MG_WEBSOCKET_OPCODE_TEXT &&
      opcode != MG_WEBSOCKET_OPCODE_CONTINUATION &&
      opcode != MG_WEBSOCKET_OPCODE_BINARY) {
    return 1;  // ping/pong 由 CivetWeb 自动处理，其余忽略
  }
  if (data == nullptr || len >= static_cast<size_t>(kMsgCap)) {
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
  HeapText body(kHttpReplyCap);
  if (!body.ok()) {
    SendOom(conn);
    return;
  }
  const int n = resp.Dump(body.p, kHttpReplyCap);
  const long long len = (n >= 0) ? static_cast<long long>(n) : 0;
  mg_send_http_ok(conn, "application/json; charset=utf-8", len);
  mg_write(conn, body.p, static_cast<size_t>(len));
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

  HeapNodes hp(kHttpPoolNodes);
  if (!hp.ok()) {
    return SendOom(conn);
  }
  ppjson::Pool& pool = hp.pool;
  ppjson::Value resp = ppjson::Value::Obj(pool);

  if (hasName && !ScriptStore::safe_name(nameBuf)) {
    resp.SetBool("success", false);
    resp.SetStr("data", "bad name");
    resp.SetStr("code", kErr);
  } else if (hasName && std::strcmp(method, "DELETE") == 0) {
    self->scripts_->remove(pool, nameBuf, resp);
  } else if (hasName &&
             (std::strcmp(method, "POST") == 0 || std::strcmp(method, "PUT") == 0)) {
    const int bodyCap = RequestBodyCap(ri);
    HeapText body(bodyCap);
    if (!body.ok()) {
      return SendOom(conn);
    }
    read_body(conn, ri, body.p, bodyCap);
    self->scripts_->write(pool, nameBuf, body.p, resp);
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
  HeapNodes hp(kHttpPoolNodes);
  if (!hp.ok()) {
    return SendOom(conn);
  }
  ppjson::Pool& pool = hp.pool;
  ppjson::Value g = ppjson::Value::Obj(pool);
  ppjson::Value resp = ppjson::Value::Obj(pool);
  // 每次读几何都让宿主重新解析一次窗口：应用重建窗口（换主题/多窗口/改分辨率）
  // 后旧 HWND 失效，这一步就是"刷新应用"真正起作用的地方。
  const bool refreshed = self->handler_->refresh_window();
  if (self->handler_->target_minimized()) {
    resp.SetBool("success", false);
    resp.SetStr("data", "view window minimized");
    resp.SetStr("code", kErr);
  } else if (!self->handler_->geometry(g)) {
    resp.SetBool("success", false);
    resp.SetStr("data", "view window unavailable");
    resp.SetStr("code", kErr);
  } else {
    g.SetBool("refreshed", refreshed);
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
  HeapNodes hp(kHttpPoolNodes);
  if (!hp.ok()) {
    return SendOom(conn);
  }
  ppjson::Pool& pool = hp.pool;
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