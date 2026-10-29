// ============================================================================
// task_scheduler.cpp - 见 task_scheduler.h。
// ============================================================================
#include "task_scheduler.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "../common/log.h"
#include "input_dispatcher.h"
#include "script_store.h"
#include "window_target.h"

namespace {
const char* kOk = "0000";
const char* kErr = "9999";

// --- 堆上的请求缓冲（解析脚本用，复用 ws_server 的思路）----------------------
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

// 构造统一封套 {success,data,code}。
void TaskReply(ppjson::Pool& pool, ppjson::Value& out, bool ok, ppjson::Value data,
               const char* code) {
  out = ppjson::Value::Obj(pool);
  out.SetBool("success", ok);
  if (data.Valid()) {
    out.SetValue("data", data);
  } else {
    out.SetValue("data", ppjson::Value::Str(pool, ""));
  }
  out.SetStr("code", code);
}

// status 枚举 -> 前端友好的字符串。
const char* StatusStr(int s) {
  switch (s) {
    case TaskScheduler::kRunning: return "running";
    case TaskScheduler::kPaused: return "paused";
    case TaskScheduler::kDone: return "done";
    case TaskScheduler::kError: return "error";
    default: return "stopped";
  }
}

}  // namespace

TaskScheduler::TaskScheduler(WindowTarget& win, InputDispatcher& input,
                             ScriptStore& scripts)
    : win_(win), input_(input), scripts_(scripts) {
  InitializeCriticalSection(&cs_);
}

TaskScheduler::~TaskScheduler() {
  signal_stop();
  if (thread_ != nullptr) {
    WaitForSingleObject(thread_, 2000);
    CloseHandle(thread_);
    thread_ = nullptr;
  }
  if (stop_ev_ != nullptr) {
    CloseHandle(stop_ev_);
    stop_ev_ = nullptr;
  }
  free_events();
  DeleteCriticalSection(&cs_);
}

void TaskScheduler::free_events() {
  if (events_ != nullptr) {
    HeapFree(GetProcessHeap(), 0, events_);
    events_ = nullptr;
  }
  event_count_ = 0;
}

int TaskScheduler::key_name_to_vk(const char* name) const {
  if (name == nullptr || name[0] == '\0') return 0;
  // 组合键只取最后一个 '+' 后的主键（与前端一致）。
  const char* plus = std::strrchr(name, '+');
  const char* key = (plus && plus[1] != '\0') ? plus + 1 : name;
  char buf[32] = {};
  int bi = 0;
  for (const char* s = key; *s != '\0' && bi < 31; ++s) {
    buf[bi++] = static_cast<char>(std::tolower(static_cast<unsigned char>(*s)));
  }
  buf[bi] = '\0';
  if (std::strcmp(buf, "enter") == 0 || std::strcmp(buf, "return") == 0) return 13;
  if (std::strcmp(buf, "esc") == 0 || std::strcmp(buf, "escape") == 0) return 27;
  if (std::strcmp(buf, "space") == 0) return 32;
  if (std::strcmp(buf, "tab") == 0) return 9;
  if (std::strcmp(buf, "backspace") == 0) return 8;
  if (std::strcmp(buf, "capslock") == 0) return 20;
  if (std::strcmp(buf, "shift") == 0) return 16;
  if (std::strcmp(buf, "ctrl") == 0 || std::strcmp(buf, "control") == 0) return 17;
  if (std::strcmp(buf, "alt") == 0) return 18;
  if (std::strcmp(buf, "left") == 0) return 37;
  if (std::strcmp(buf, "up") == 0) return 38;
  if (std::strcmp(buf, "right") == 0) return 39;
  if (std::strcmp(buf, "down") == 0) return 40;
  if (std::strcmp(buf, "home") == 0) return 36;
  if (std::strcmp(buf, "end") == 0) return 35;
  if (std::strcmp(buf, "pageup") == 0) return 33;
  if (std::strcmp(buf, "pagedown") == 0) return 34;
  if (std::strcmp(buf, "insert") == 0) return 45;
  if (std::strcmp(buf, "delete") == 0 || std::strcmp(buf, "del") == 0) return 46;
  if (std::strcmp(buf, "printscreen") == 0) return 44;
  if (std::strcmp(buf, "numlock") == 0) return 144;
  if (std::strcmp(buf, "scrolllock") == 0) return 145;
  if (std::strcmp(buf, "pause") == 0) return 19;
  if (buf[0] == 'f' && buf[1] >= '1' && buf[1] <= '9') {
    int n = buf[1] - '0';
    if (buf[2] >= '0' && buf[2] <= '9') n = n * 10 + (buf[2] - '0');
    if (n >= 1 && n <= 24) return 0x70 + n;
  }
  if (buf[0] >= 'a' && buf[0] <= 'z') return std::toupper(buf[0]);
  if (buf[0] >= '0' && buf[0] <= '9') return buf[0];
  return 0;
}

void TaskScheduler::extract_event(ppjson::Value ev, TaskEvent& out) const {
  const char* type = ev.Find("type").AsStr();
  if (std::strcmp(type, "mouse") == 0) {
    out.type = 0;
    const char* a = ev.Find("action").AsStr();
    const char* b = ev.Find("button").AsStr();
    if (a[0] != '\0') std::strncpy(out.action, a, sizeof(out.action) - 1);
    if (b[0] != '\0') std::strncpy(out.button, b, sizeof(out.button) - 1);
    out.x = static_cast<int>(ev.Find("x").AsInt(0));
    out.y = static_cast<int>(ev.Find("y").AsInt(0));
    out.wheel = static_cast<int>(ev.Find("wheel").AsInt(0));
  } else if (std::strcmp(type, "delay") == 0) {
    out.type = 1;
    out.ms = static_cast<int>(ev.Find("ms").AsInt(0));
  } else if (std::strcmp(type, "key") == 0) {
    out.type = 2;
    out.vk = key_name_to_vk(ev.Find("key").AsStr());
  } else if (std::strcmp(type, "input") == 0) {
    out.type = 3;
    const char* mode = ev.Find("mode").AsStr();
    out.text_mode = (std::strcmp(mode, "random") == 0) ? 1 : 0;
    if (out.text_mode == 1) {
      ppjson::Value texts = ev.Find("texts");
      if (texts.IsArr()) {
        int c = texts.Size();
        if (c > 16) c = 16;
        for (int i = 0; i < c; ++i) {
          const char* t = texts.At(i).AsStr();
          if (t[0] != '\0') std::strncpy(out.texts[i], t, sizeof(out.texts[i]) - 1);
        }
        out.text_count = c;
      }
    } else {
      const char* t = ev.Find("text").AsStr();
      if (t[0] != '\0') std::strncpy(out.text, t, sizeof(out.text) - 1);
    }
  } else {
    out.type = 4;  // findImage 等暂不支持，执行时跳过
  }
}

void TaskScheduler::exec_event(const TaskEvent& ev, float scale) {
  switch (ev.type) {
    case 0: {  // mouse
      const int px = static_cast<int>(ev.x * scale + 0.5f);
      const int py = static_cast<int>(ev.y * scale + 0.5f);
      const char* a = ev.action;
      if (std::strcmp(a, "click") == 0) {
        input_.click(px, py, "view", ev.button);
      } else if (std::strcmp(a, "down") == 0) {
        input_.button(px, py, "view", ev.button, true);
      } else if (std::strcmp(a, "up") == 0) {
        input_.button(px, py, "view", ev.button, false);
      } else if (std::strcmp(a, "scroll") == 0) {
        input_.scroll(ev.wheel);
      } else {  // move / moveTo 都当绝对移动到 (x,y)
        input_.move_to(px, py, "view");
      }
      LogTagged("framework", "task: mouse %s view=%d,%d", a, px, py);
      break;
    }
    case 1: {  // delay（可中断：每 50ms 检查 stop）
      int remaining = ev.ms;
      while (remaining > 0) {
        const int step = remaining > 50 ? 50 : remaining;
        if (WaitForSingleObject(stop_ev_, static_cast<DWORD>(step)) == WAIT_OBJECT_0) {
          return;
        }
        remaining -= step;
      }
      break;
    }
    case 2:  // key
      if (ev.vk > 0) input_.key(static_cast<WORD>(ev.vk));
      break;
    case 3: {  // text
      const char* t = (ev.text_mode == 1)
                          ? (ev.text_count > 0 ? ev.texts[std::rand() % ev.text_count] : "")
                          : ev.text;
      if (t != nullptr && t[0] != '\0') input_.text(t);
      break;
    }
    case 4:  // findImage 暂不支持
      LogTagged("framework", "task: findImage skipped (unsupported)");
      break;
  }
}

DWORD WINAPI TaskScheduler::RunThunk(LPVOID self) {
  static_cast<TaskScheduler*>(self)->run();
  return 0;
}

void TaskScheduler::run() {
  const float scale = win_.scale();
  for (loop_cur_ = 0; loop_cur_ < loop_total_; ) {
    for (idx_ = 0; idx_ < event_count_; ) {
      if (WaitForSingleObject(stop_ev_, 0) == WAIT_OBJECT_0) goto done;
      while (status_ == kPaused) {
        if (WaitForSingleObject(stop_ev_, 100) == WAIT_OBJECT_0) goto done;
      }
      if (WaitForSingleObject(stop_ev_, 0) == WAIT_OBJECT_0) goto done;
      exec_event(events_[idx_], scale);
      ++idx_;
    }
    ++loop_cur_;
  }
done:
  EnterCriticalSection(&cs_);
  status_ = (WaitForSingleObject(stop_ev_, 0) == WAIT_OBJECT_0) ? kStopped : kDone;
  LeaveCriticalSection(&cs_);
  LogTagged("framework", "task: run finished (status=%s)", StatusStr(status_));
}

void TaskScheduler::start(ppjson::Pool& pool, const char* name, int loop,
                          ppjson::Value& out) {
  if (name == nullptr || name[0] == '\0') {
    ppjson::Value d = ppjson::Value::Obj(pool);
    d.SetStr("msg", "name required");
    TaskReply(pool, out, false, d, kErr);
    return;
  }
  EnterCriticalSection(&cs_);
  if (active_ && (status_ == kRunning || status_ == kPaused)) {
    ppjson::Value d = ppjson::Value::Obj(pool);
    d.SetStr("msg", "已有活跃任务，先停止");
    TaskReply(pool, out, false, d, kErr);
    LeaveCriticalSection(&cs_);
    return;
  }
  free_events();

  HeapNodes hn(2048);
  if (!hn.ok()) {
    TaskReply(pool, out, false, ppjson::Value(), kErr);
    LeaveCriticalSection(&cs_);
    return;
  }
  ppjson::Value resp = ppjson::Value::Obj(hn.pool);
  scripts_.read(hn.pool, name, resp);
  if (!resp.Find("success").AsBool(false)) {
    ppjson::Value d = ppjson::Value::Obj(hn.pool);
    d.SetStr("msg", "脚本不存在或解析失败");
    TaskReply(pool, out, false, d, kErr);
    LeaveCriticalSection(&cs_);
    return;
  }
  ppjson::Value data = resp.Find("data");
  ppjson::Value events = data.Find("events");
  if (!events.IsArr()) {
    ppjson::Value d = ppjson::Value::Obj(hn.pool);
    d.SetStr("msg", "脚本没有 events 数组");
    TaskReply(pool, out, false, d, kErr);
    LeaveCriticalSection(&cs_);
    return;
  }
  const int n = events.Size();
  events_ = static_cast<TaskEvent*>(HeapAlloc(
      GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(TaskEvent) * (n > 0 ? n : 1)));
  event_count_ = 0;
  for (int i = 0; i < n; ++i) {
    TaskEvent ev;
    std::memset(&ev, 0, sizeof(ev));
    extract_event(events.At(i), ev);
    events_[event_count_++] = ev;
  }
  loop_total_ = (loop > 0) ? loop : static_cast<int>(data.Find("loop").AsInt(1));
  if (loop_total_ <= 0) loop_total_ = 1;
  std::strncpy(name_, name, sizeof(name_) - 1);
  task_id_ = (task_id_ >= 0x7fffffff) ? 1 : task_id_ + 1;
  idx_ = 0;
  loop_cur_ = 0;
  status_ = kRunning;
  stop_ev_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  active_ = true;
  thread_ = CreateThread(nullptr, 0, RunThunk, this, 0, nullptr);
  if (thread_ == nullptr) {
    status_ = kError;
    active_ = false;
    if (stop_ev_ != nullptr) {
      CloseHandle(stop_ev_);
      stop_ev_ = nullptr;
    }
    free_events();
    TaskReply(pool, out, false, ppjson::Value(), kErr);
    LeaveCriticalSection(&cs_);
    return;
  }
  ppjson::Value d = ppjson::Value::Obj(pool);
  d.SetNum("id", task_id_);
  d.SetNum("count", event_count_);
  TaskReply(pool, out, true, d, kOk);
  LeaveCriticalSection(&cs_);
  LogTagged("framework", "task: start '%s' id=%d events=%d loop=%d", name, task_id_,
            event_count_, loop_total_);
}

void TaskScheduler::stop(ppjson::Pool& pool, int /*id*/, ppjson::Value& out) {
  EnterCriticalSection(&cs_);
  if (!active_) {
    TaskReply(pool, out, false, ppjson::Value(), kErr);
    LeaveCriticalSection(&cs_);
    return;
  }
  if (stop_ev_ != nullptr) SetEvent(stop_ev_);
  TaskReply(pool, out, true, ppjson::Value(), kOk);
  LeaveCriticalSection(&cs_);
}

void TaskScheduler::pause(ppjson::Pool& pool, int /*id*/, ppjson::Value& out) {
  EnterCriticalSection(&cs_);
  if (active_ && status_ == kRunning) status_ = kPaused;
  TaskReply(pool, out, true, ppjson::Value(), kOk);
  LeaveCriticalSection(&cs_);
}

void TaskScheduler::resume(ppjson::Pool& pool, int /*id*/, ppjson::Value& out) {
  EnterCriticalSection(&cs_);
  if (active_ && status_ == kPaused) status_ = kRunning;
  TaskReply(pool, out, true, ppjson::Value(), kOk);
  LeaveCriticalSection(&cs_);
}

void TaskScheduler::list(ppjson::Pool& pool, ppjson::Value& out) {
  ppjson::Value arr = ppjson::Value::Arr(pool);
  EnterCriticalSection(&cs_);
  if (active_) {
    ppjson::Value t = ppjson::Value::Obj(pool);
    t.SetNum("id", task_id_);
    t.SetStr("name", name_);
    t.SetStr("status", StatusStr(status_));
    t.SetNum("loop_cur", loop_cur_);
    t.SetNum("loop_total", loop_total_);
    t.SetNum("idx", idx_);
    t.SetNum("count", event_count_);
    arr.AddValue(t);
  }
  LeaveCriticalSection(&cs_);
  ppjson::Value data = ppjson::Value::Obj(pool);
  data.SetValue("tasks", arr);
  TaskReply(pool, out, true, data, kOk);
}

void TaskScheduler::stop_all() {
  signal_stop();
  if (thread_ != nullptr) {
    WaitForSingleObject(thread_, 2000);
    CloseHandle(thread_);
    thread_ = nullptr;
  }
}

void TaskScheduler::signal_stop() {
  if (stop_ev_ != nullptr) SetEvent(stop_ev_);
}
