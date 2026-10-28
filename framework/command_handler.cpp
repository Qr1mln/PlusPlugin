// ============================================================================
// command_handler.cpp - 见 command_handler.h。
// ============================================================================
#include "command_handler.h"

#include <cstring>

#include "input_dispatcher.h"
#include "picker.h"
#include "window_target.h"

#include "../common/log.h"

namespace {
const char* kOk = "0000";
const char* kErr = "9999";

// 每条命令用一块堆池。栈放不下（Node 约 600B），用 HeapAlloc；RAII 归还。
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

}  // namespace

int CommandHandler::make_reply(ppjson::Pool& pool, char* out, int cap,
                               const char* id, bool success, ppjson::Value data,
                               const char* code) {
  ppjson::Value r = ppjson::Value::Obj(pool);
  r.SetStr("id", (id != nullptr) ? id : "");
  r.SetBool("success", success);
  if (data.Valid()) {
    r.SetValue("data", data);
  } else {
    r.SetValue("data", ppjson::Value::Str(pool, ""));
  }
  r.SetStr("code", code);
  return r.Dump(out, cap);
}

bool CommandHandler::cursor(ppjson::Value& out) const {
  return PickerCursorInfo(out);
}

int CommandHandler::handle(const char* msg, char* out, int cap, void* conn) {
  PoolHeap hp(512);
  ppjson::Pool& pool = hp.pool;

  ppjson::Value cmd;
  char err[64] = {};
  if (msg == nullptr || !ppjson::Value::Parse(msg, pool, cmd, err, sizeof(err))) {
    return make_reply(pool, out, cap, "", false, ppjson::Value::Str(pool, "bad json"), kErr);
  }
  if (!cmd.IsObj()) {
    return make_reply(pool, out, cap, "", false,
                      ppjson::Value::Str(pool, "not an object"), kErr);
  }

  const char* id = cmd.Find("id").AsStr();
  const char* type = cmd.Find("type").AsStr();
  const char* action = cmd.Find("action").AsStr();
  const char* op = cmd.Find("op").AsStr();

  const char* category = (type[0] != '\0') ? type : action;
  const char* sub = (type[0] != '\0') ? action : op;

  const char* space = cmd.Find("space").IsStr() ? cmd.Find("space").AsStr() : "view";
  const char* button = cmd.Find("button").IsStr() ? cmd.Find("button").AsStr() : "left";

  if (std::strcmp(category, "mouse") == 0) {
    if (sub[0] == '\0') {
      sub = "move";
    }
    if (std::strcmp(sub, "move") == 0) {
      const int dx = static_cast<int>(cmd.Find("dx").AsInt(0));
      const int dy = static_cast<int>(cmd.Find("dy").AsInt(0));
      input_.move(dx, dy);
      LogTagged("framework", "mouse move dx=%d dy=%d", dx, dy);
    } else if (std::strcmp(sub, "moveTo") == 0) {
      const int x = static_cast<int>(cmd.Find("x").AsInt(-1));
      const int y = static_cast<int>(cmd.Find("y").AsInt(-1));
      if (x < 0 || y < 0) {
        return make_reply(pool, out, cap, id, false,
                          ppjson::Value::Str(pool, "moveTo needs x,y"), kErr);
      }
      input_.move_to(x, y, space);
      LogTagged("framework", "mouse moveTo %s=%d,%d", space, x, y);
    } else if (std::strcmp(sub, "click") == 0) {
      const int x = static_cast<int>(cmd.Find("x").AsInt(-1));
      const int y = static_cast<int>(cmd.Find("y").AsInt(-1));
      input_.click(x, y, space, button);
      LogTagged("framework", "mouse click %s=%d,%d button=%s", space, x, y, button);
    } else if (std::strcmp(sub, "down") == 0 || std::strcmp(sub, "up") == 0) {
      const int x = static_cast<int>(cmd.Find("x").AsInt(-1));
      const int y = static_cast<int>(cmd.Find("y").AsInt(-1));
      input_.button(x, y, space, button, std::strcmp(sub, "down") == 0);
      LogTagged("framework", "mouse %s %s=%d,%d button=%s", sub, space, x, y, button);
    } else if (std::strcmp(sub, "scroll") == 0) {
      const int delta = static_cast<int>(cmd.Find("delta").AsInt(0));
      input_.scroll(delta);
      LogTagged("framework", "mouse scroll delta=%d", delta);
    } else {
      return make_reply(pool, out, cap, id, false,
                        ppjson::Value::Str(pool, "unknown mouse action"), kErr);
    }
    return make_reply(pool, out, cap, id, true, ppjson::Value(), kOk);
  }

  if (std::strcmp(category, "key") == 0) {
    const int vk = static_cast<int>(cmd.Find("vk").AsInt(0));
    if (vk <= 0 || vk > 255) {
      return make_reply(pool, out, cap, id, false, ppjson::Value::Str(pool, "invalid vk"), kErr);
    }
    input_.key(static_cast<WORD>(vk));
    LogTagged("framework","key vk=%d", vk);
    return make_reply(pool, out, cap, id, true, ppjson::Value(), kOk);
  }

  if (std::strcmp(category, "text") == 0) {
    const char* text = cmd.Find("text").AsStr();
    if (text[0] == '\0') {
      return make_reply(pool, out, cap, id, false,
                        ppjson::Value::Str(pool, "text is empty"), kErr);
    }
    input_.text(text);
    LogTagged("framework","text len=%d", static_cast<int>(std::strlen(text)));
    return make_reply(pool, out, cap, id, true, ppjson::Value(), kOk);
  }

  if (std::strcmp(category, "delay") == 0) {
    const int ms = static_cast<int>(cmd.Find("ms").AsInt(0));
    input_.delay(ms);
    LogTagged("framework","delay ms=%d", ms);
    return make_reply(pool, out, cap, id, true, ppjson::Value(), kOk);
  }

  if (std::strcmp(category, "geometry") == 0) {
    ppjson::Value g = ppjson::Value::Obj(pool);
    if (!win_.geometry(g)) {
      return make_reply(pool, out, cap, id, false,
                        ppjson::Value::Str(pool, "view window unavailable"), kErr);
    }
    return make_reply(pool, out, cap, id, true, g, kOk);
  }

  if (std::strcmp(category, "cursor") == 0) {
    if (std::strcmp(sub, "watch") == 0 || std::strcmp(sub, "unwatch") == 0) {
      const bool on = (std::strcmp(sub, "watch") == 0);
      const bool state = PickerSetWatch(conn, on);
      LogTagged("framework", "cursor %s -> %s", sub, state ? "on" : "off");
      ppjson::Value d = ppjson::Value::Obj(pool);
      d.SetBool("watch", state);
      return make_reply(pool, out, cap, id, true, d, kOk);
    }
    ppjson::Value c = ppjson::Value::Obj(pool);
    if (!PickerCursorInfo(c)) {
      return make_reply(pool, out, cap, id, false,
                        ppjson::Value::Str(pool, "view window unavailable"), kErr);
    }
    return make_reply(pool, out, cap, id, true, c, kOk);
  }

  if (std::strcmp(category, "pick") == 0) {
    if (std::strcmp(sub, "start") == 0 || sub[0] == '\0') {
      if (!PickerBeginPick()) {
        return make_reply(pool, out, cap, id, false,
                          ppjson::Value::Str(pool, "push thread not running"), kErr);
      }
      LogTagged("framework", "pick: start");
      return make_reply(pool, out, cap, id, true, ppjson::Value(), kOk);
    }
    if (std::strcmp(sub, "cancel") == 0 || std::strcmp(sub, "stop") == 0) {
      PickerCancelPick();
      LogTagged("framework", "pick: cancel");
      return make_reply(pool, out, cap, id, true, ppjson::Value(), kOk);
    }
    return make_reply(pool, out, cap, id, false,
                      ppjson::Value::Str(pool, "unknown pick action"), kErr);
  }

  return make_reply(pool, out, cap, id, false,
                    ppjson::Value::Str(pool, "unknown type"), kErr);
}