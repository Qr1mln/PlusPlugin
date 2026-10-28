// ============================================================================
// command_handler.cpp - 见 command_handler.h。
// ============================================================================
#include "command_handler.h"

#include "input_dispatcher.h"
#include "logger.h"
#include "picker.h"
#include "window_target.h"

namespace {
const std::string kOk = "0000";
const std::string kErr = "9999";
}  // namespace

std::string CommandHandler::make_reply(const std::string& id, bool success,
                                       const nlohmann::json& data,
                                       const std::string& code) {
  nlohmann::json r;
  r["id"] = id;
  r["success"] = success;
  r["data"] = data;
  r["code"] = code;
  return r.dump();
}

bool CommandHandler::cursor(nlohmann::json& out) const {
  return PickerCursorInfo(out);
}

std::string CommandHandler::handle(const std::string& msg, void* conn) {
  nlohmann::json cmd;
  try {
    cmd = nlohmann::json::parse(msg);
  } catch (...) {
    return make_reply("", false, "bad json", kErr);
  }
  if (!cmd.is_object()) {
    return make_reply("", false, "not an object", kErr);
  }

  const std::string id = cmd.value("id", std::string());
  const std::string type = cmd.value("type", std::string());
  std::string category = type;
  std::string sub;
  if (!type.empty()) {
    category = type;
    sub = cmd.value("action", std::string());
  } else {
    category = cmd.value("action", std::string());
    sub = cmd.value("op", std::string());
  }

  const std::string space = cmd.value("space", std::string("view"));
  const std::string button = cmd.value("button", std::string("left"));

  if (category == "mouse") {
    if (sub.empty()) {
      sub = "move";
    }
    if (sub == "move") {
      const int dx = cmd.value("dx", 0);
      const int dy = cmd.value("dy", 0);
      input_.move(dx, dy);
      log_.publish("mouse move dx=%d dy=%d", dx, dy);
    } else if (sub == "moveTo") {
      const int x = cmd.value("x", -1);
      const int y = cmd.value("y", -1);
      if (x < 0 || y < 0) {
        return make_reply(id, false, "moveTo needs x,y", kErr);
      }
      input_.move_to(x, y, space);
      log_.publish("mouse moveTo %s=%d,%d", space.c_str(), x, y);
    } else if (sub == "click") {
      const int x = cmd.value("x", -1);
      const int y = cmd.value("y", -1);
      input_.click(x, y, space, button);
      log_.publish("mouse click %s=%d,%d button=%s", space.c_str(), x, y,
                   button.c_str());
    } else if (sub == "down" || sub == "up") {
      const int x = cmd.value("x", -1);
      const int y = cmd.value("y", -1);
      input_.button(x, y, space, button, sub == "down");
      log_.publish("mouse %s %s=%d,%d button=%s", sub.c_str(), space.c_str(), x,
                   y, button.c_str());
    } else if (sub == "scroll") {
      const int delta = cmd.value("delta", 0);
      input_.scroll(delta);
      log_.publish("mouse scroll delta=%d", delta);
    } else {
      return make_reply(id, false, "unknown mouse action", kErr);
    }
    return make_reply(id, true, "", kOk);
  }

  if (category == "key") {
    const int vk = cmd.value("vk", 0);
    if (vk <= 0 || vk > 255) {
      return make_reply(id, false, "invalid vk", kErr);
    }
    input_.key(static_cast<WORD>(vk));
    log_.publish("key vk=%d", vk);
    return make_reply(id, true, "", kOk);
  }

  if (category == "text") {
    const std::string text = cmd.value("text", std::string());
    if (text.empty()) {
      return make_reply(id, false, "text is empty", kErr);
    }
    input_.text(text);
    log_.publish("text bytes=%d", static_cast<int>(text.size()));
    return make_reply(id, true, "", kOk);
  }

  if (category == "delay") {
    const int ms = cmd.value("ms", 0);
    input_.delay(ms);
    log_.publish("delay ms=%d", ms);
    return make_reply(id, true, "", kOk);
  }

  if (category == "geometry") {
    nlohmann::json g;
    if (!win_.geometry(g)) {
      return make_reply(id, false, "view window unavailable", kErr);
    }
    return make_reply(id, true, g, kOk);
  }

  if (category == "cursor") {
    if (sub == "watch" || sub == "unwatch") {
      // 订阅后光标位置由推送线程持续下发（{"t":"cursor",...}），前端不用再轮询。
      const bool on = (sub == "watch");
      const bool state = PickerSetWatch(conn, on);
      log_.publish("cursor %s -> %s", sub.c_str(), state ? "on" : "off");
      nlohmann::json d;
      d["watch"] = state;
      return make_reply(id, true, d, kOk);
    }
    // 只读：回报真实光标在 view 内的位置（单帧，保留给 HTTP/旧前端）。
    nlohmann::json c;
    if (!PickerCursorInfo(c)) {
      return make_reply(id, false, "view window unavailable", kErr);
    }
    return make_reply(id, true, c, kOk);
  }

  if (category == "pick") {
    // 拾取模式做在 DLL 里：装 WH_MOUSE_LL 钩子，吞掉 view 内的一次左键，
    // 通过 {"t":"pick",...} 主动推给前端（右键取消）。
    if (sub == "start" || sub.empty()) {
      if (!PickerBeginPick()) {
        return make_reply(id, false, "push thread not running", kErr);
      }
      log_.publish("pick: start");
      return make_reply(id, true, "", kOk);
    }
    if (sub == "cancel" || sub == "stop") {
      PickerCancelPick();
      log_.publish("pick: cancel");
      return make_reply(id, true, "", kOk);
    }
    return make_reply(id, false, "unknown pick action", kErr);
  }

  return make_reply(id, false, "unknown type", kErr);
}
