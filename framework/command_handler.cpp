// ============================================================================
// command_handler.cpp - 见 command_handler.h。
// ============================================================================
#include "command_handler.h"

#include "input_dispatcher.h"
#include "logger.h"
#include "picker.h"
#include "window_target.h"

namespace {
const char* kOk = "0000";
const char* kErr = "9999";

// 每条命令用一块堆池（节点数按命令规模给足）。栈放不下（Node 约 600B），
// 用 HeapAlloc；RAII 保证异常/提前 return 也会归还。
struct PoolHeap {
  ppjson::Node* nodes;
  ppjson::Pool pool;
  explicit PoolHeap(int n)
      : nodes(static_cast<ppjson::Node*>(HeapAlloc(GetProcessHeap(), 0,
                                                   sizeof(ppjson::Node) * n))),
        pool(nodes, n) {}
  ~PoolHeap() {
    if (nodes != nullptr) {
      HeapFree(GetProcessHeap(), 0, nodes);
    }
  }
  PoolHeap(const PoolHeap&) = delete;
  PoolHeap& operator=(const PoolHeap&) = delete;
};

// data 缺省（invalid）时写成空串，保证响应始终有 data 字段。
ppjson::Value EmptyData(ppjson::Pool& pool) {
  return ppjson::Value::Str(pool, "");
}
}  // namespace

std::string CommandHandler::make_reply(ppjson::Pool& pool, const char* id,
                                       bool success, ppjson::Value data,
                                       const char* code) {
  ppjson::Value r = ppjson::Value::Obj(pool);
  r.SetStr("id", (id != nullptr) ? id : "");
  r.SetBool("success", success);
  if (data.Valid()) {
    r.SetValue("data", data);
  } else {
    r.SetValue("data", EmptyData(pool));
  }
  r.SetStr("code", code);
  char buf[8192] = {};
  r.Dump(buf, sizeof(buf));
  return std::string(buf);
}

bool CommandHandler::cursor(ppjson::Value& out) const {
  return PickerCursorInfo(out);
}

std::string CommandHandler::handle(const std::string& msg, void* conn) {
  PoolHeap hp(512);
  ppjson::Pool& pool = hp.pool;

  ppjson::Value cmd;
  char err[64] = {};
  if (!ppjson::Value::Parse(msg.c_str(), pool, cmd, err, sizeof(err))) {
    return make_reply(pool, "", false, ppjson::Value::Str(pool, "bad json"), kErr);
  }
  if (!cmd.IsObj()) {
    return make_reply(pool, "", false, ppjson::Value::Str(pool, "not an object"), kErr);
  }

  const char* id = cmd.Find("id").AsStr();
  const char* type = cmd.Find("type").AsStr();
  const char* action = cmd.Find("action").AsStr();
  const char* op = cmd.Find("op").AsStr();

  std::string category = type;
  std::string sub;
  if (type[0] != '\0') {
    sub = action;
  } else {
    category = action;
    sub = op;
  }

  const std::string space = (cmd.Find("space").IsStr()) ? cmd.Find("space").AsStr() : "view";
  const std::string button =
      (cmd.Find("button").IsStr()) ? cmd.Find("button").AsStr() : "left";

  if (category == "mouse") {
    if (sub.empty()) {
      sub = "move";
    }
    if (sub == "move") {
      const int dx = static_cast<int>(cmd.Find("dx").AsInt(0));
      const int dy = static_cast<int>(cmd.Find("dy").AsInt(0));
      input_.move(dx, dy);
      log_.publish("mouse move dx=%d dy=%d", dx, dy);
    } else if (sub == "moveTo") {
      const int x = static_cast<int>(cmd.Find("x").AsInt(-1));
      const int y = static_cast<int>(cmd.Find("y").AsInt(-1));
      if (x < 0 || y < 0) {
        return make_reply(pool, id, false, ppjson::Value::Str(pool, "moveTo needs x,y"), kErr);
      }
      input_.move_to(x, y, space);
      log_.publish("mouse moveTo %s=%d,%d", space.c_str(), x, y);
    } else if (sub == "click") {
      const int x = static_cast<int>(cmd.Find("x").AsInt(-1));
      const int y = static_cast<int>(cmd.Find("y").AsInt(-1));
      input_.click(x, y, space, button);
      log_.publish("mouse click %s=%d,%d button=%s", space.c_str(), x, y,
                   button.c_str());
    } else if (sub == "down" || sub == "up") {
      const int x = static_cast<int>(cmd.Find("x").AsInt(-1));
      const int y = static_cast<int>(cmd.Find("y").AsInt(-1));
      input_.button(x, y, space, button, sub == "down");
      log_.publish("mouse %s %s=%d,%d button=%s", sub.c_str(), space.c_str(), x,
                   y, button.c_str());
    } else if (sub == "scroll") {
      const int delta = static_cast<int>(cmd.Find("delta").AsInt(0));
      input_.scroll(delta);
      log_.publish("mouse scroll delta=%d", delta);
    } else {
      return make_reply(pool, id, false, ppjson::Value::Str(pool, "unknown mouse action"), kErr);
    }
    return make_reply(pool, id, true, EmptyData(pool), kOk);
  }

  if (category == "key") {
    const int vk = static_cast<int>(cmd.Find("vk").AsInt(0));
    if (vk <= 0 || vk > 255) {
      return make_reply(pool, id, false, ppjson::Value::Str(pool, "invalid vk"), kErr);
    }
    input_.key(static_cast<WORD>(vk));
    log_.publish("key vk=%d", vk);
    return make_reply(pool, id, true, EmptyData(pool), kOk);
  }

  if (category == "text") {
    const std::string text = cmd.Find("text").AsStr();
    if (text.empty()) {
      return make_reply(pool, id, false, ppjson::Value::Str(pool, "text is empty"), kErr);
    }
    input_.text(text);
    log_.publish("text bytes=%d", static_cast<int>(text.size()));
    return make_reply(pool, id, true, EmptyData(pool), kOk);
  }

  if (category == "delay") {
    const int ms = static_cast<int>(cmd.Find("ms").AsInt(0));
    input_.delay(ms);
    log_.publish("delay ms=%d", ms);
    return make_reply(pool, id, true, EmptyData(pool), kOk);
  }

  if (category == "geometry") {
    ppjson::Value g = ppjson::Value::Obj(pool);
    if (!win_.geometry(g)) {
      return make_reply(pool, id, false,
                        ppjson::Value::Str(pool, "view window unavailable"), kErr);
    }
    return make_reply(pool, id, true, g, kOk);
  }

  if (category == "cursor") {
    if (sub == "watch" || sub == "unwatch") {
      // 订阅后光标位置由推送线程持续下发（{"t":"cursor",...}），前端不用再轮询。
      const bool on = (sub == "watch");
      const bool state = PickerSetWatch(conn, on);
      log_.publish("cursor %s -> %s", sub.c_str(), state ? "on" : "off");
      ppjson::Value d = ppjson::Value::Obj(pool);
      d.SetBool("watch", state);
      return make_reply(pool, id, true, d, kOk);
    }
    // 只读：回报真实光标在 view 内的位置（单帧，保留给 HTTP/旧前端）。
    ppjson::Value c = ppjson::Value::Obj(pool);
    if (!PickerCursorInfo(c)) {
      return make_reply(pool, id, false,
                        ppjson::Value::Str(pool, "view window unavailable"), kErr);
    }
    return make_reply(pool, id, true, c, kOk);
  }

  if (category == "pick") {
    // 拾取模式做在 DLL 里：装 WH_MOUSE_LL 钩子，吞掉 view 内的一次左键，
    // 通过 {"t":"pick",...} 主动推给前端（右键取消）。
    if (sub == "start" || sub.empty()) {
      if (!PickerBeginPick()) {
        return make_reply(pool, id, false,
                          ppjson::Value::Str(pool, "push thread not running"), kErr);
      }
      log_.publish("pick: start");
      return make_reply(pool, id, true, EmptyData(pool), kOk);
    }
    if (sub == "cancel" || sub == "stop") {
      PickerCancelPick();
      log_.publish("pick: cancel");
      return make_reply(pool, id, true, EmptyData(pool), kOk);
    }
    return make_reply(pool, id, false, ppjson::Value::Str(pool, "unknown pick action"), kErr);
  }

  return make_reply(pool, id, false, ppjson::Value::Str(pool, "unknown type"), kErr);
}