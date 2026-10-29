// ============================================================================
// schedule_engine.cpp - 见 schedule_engine.h。
// ============================================================================
#include "schedule_engine.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "../common/log.h"
#include "input_dispatcher.h"
#include "picker.h"
#include "script_store.h"
#include "task_scheduler.h"
#include "window_target.h"

namespace {
const char* kOk = "0000";
const char* kErr = "9999";

// 定时任务执行日志：同时写 proxy.log 并广播给编辑器（{t:"log"}，日志面板/弹窗可见）。
void SchedLog(const char* level, const char* text) {
  if (text == nullptr) return;
  LogTagged("framework", "sched: %s", text);
  ppjson::Node nodes[8] = {};
  ppjson::Pool pool(nodes, 8);
  ppjson::Value v = ppjson::Value::Obj(pool);
  v.SetStr("t", "log");
  v.SetStr("level", level != nullptr ? level : "info");
  v.SetStr("text", text);
  char buf[768] = {};
  const int n = v.Dump(buf, sizeof(buf));
  if (n > 0) PickerBroadcastText(buf, n);
}

// 构造统一封套 {success,data,code}。
void EngineReply(ppjson::Pool& pool, ppjson::Value& out, bool ok, ppjson::Value data,
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
}  // namespace

ScheduleEngine::ScheduleEngine(WindowTarget& win, InputDispatcher& input,
                               ScriptStore& scripts, TaskScheduler& task)
    : win_(win), input_(input), scripts_(scripts), task_(task) {
  InitializeCriticalSection(&cs_);
}

ScheduleEngine::~ScheduleEngine() {
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
  DeleteCriticalSection(&cs_);
}

void ScheduleEngine::load_from_file() {
  wchar_t path[640] = {};
  if (!ScheduleStore::get_path(path, 640)) return;
  EnterCriticalSection(&cs_);
  ScheduleStore::load(path, jobs_, 64, &job_count_);
  LeaveCriticalSection(&cs_);
  char buf[96] = {};
  std::snprintf(buf, sizeof(buf), "已加载 %d 条定时任务", job_count_);
  SchedLog("info", buf);
}

void ScheduleEngine::reload() {
  load_from_file();
}

int ScheduleEngine::find_job(const char* id) const {
  if (id == nullptr) return -1;
  for (int i = 0; i < job_count_; ++i) {
    if (std::strcmp(jobs_[i].id, id) == 0) return i;
  }
  return -1;
}

bool ScheduleEngine::run_job(SchedJob& job) {
  const char* sc[20] = {};
  for (int k = 0; k < job.script_count && k < 20; ++k) sc[k] = job.scripts[k];

  // 复用 task_scheduler 的堆池模式（HTTP 同款）。
  struct HN {
    ppjson::Node* n;
    ppjson::Pool pool;
    explicit HN(int c)
        : n(static_cast<ppjson::Node*>(
              HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                        sizeof(ppjson::Node) * static_cast<size_t>(c)))),
          pool(n, c) {}
    ~HN() {
      if (n) HeapFree(GetProcessHeap(), 0, n);
    }
    bool ok() const { return n != nullptr; }
  } hn(2048);
  if (!hn.ok()) return false;
  ppjson::Value out = ppjson::Value::Obj(hn.pool);
  task_.start_job(hn.pool, job.name, job.dir, sc, job.script_count, job.loop,
                  job.gap_ms, out);
  return out.Find("success").AsBool(false);
}

void ScheduleEngine::advance_job(SchedJob& job, long long completed) {
  if (job.when.type == 1) {  // interval（秒级）
    job.next_run = completed + static_cast<long long>(job.when.seconds > 0 ? job.when.seconds : 60);
  } else if (job.when.type == 2) {  // once
    job.next_run = 0;                // 跑过一次，不再触发
  } else {                          // daily
    ScheduleStore::compute_next(job, completed, job.next_run);
  }
}

bool ScheduleEngine::run_now(const char* job_id) {
  EnterCriticalSection(&cs_);
  const int idx = find_job(job_id);
  if (idx < 0) {
    LeaveCriticalSection(&cs_);
    SchedLog("warn", "立即执行失败：找不到该任务");
    return false;
  }
  SchedJob& j = jobs_[idx];
  j.pending = false;
  if (task_.is_busy()) {
    j.pending = true;  // 排队，空闲后由 loop 拾起
    char qbuf[192] = {};
    std::snprintf(qbuf, sizeof(qbuf), "「%s」排队中（有任务在跑，空闲后执行）", j.name);
    LeaveCriticalSection(&cs_);
    SchedLog("warn", qbuf);
    return true;
  }
  LeaveCriticalSection(&cs_);
  const bool ok = run_job(j);
  EnterCriticalSection(&cs_);
  if (ok) {
    running_idx_ = idx;
  } else {
    advance_job(j, static_cast<long long>(std::time(nullptr)));
  }
  LeaveCriticalSection(&cs_);
  char buf[192] = {};
  std::snprintf(buf, sizeof(buf), "立即执行「%s」%s", j.name, ok ? "已启动" : "启动失败");
  SchedLog(ok ? "info" : "error", buf);
  return ok;
}

void ScheduleEngine::get_status(ppjson::Pool& pool, ppjson::Value& out) {
  EnterCriticalSection(&cs_);
  ppjson::Value arr = ppjson::Value::Arr(pool);
  const bool busy = task_.is_busy();
  for (int i = 0; i < job_count_; ++i) {
    const SchedJob& j = jobs_[i];
    ppjson::Value o = ppjson::Value::Obj(pool);
    o.SetStr("id", j.id[0] ? j.id : "");
    o.SetStr("name", j.name);
    o.SetBool("enabled", j.enabled);
    o.SetNum("loop", j.loop);
    o.SetNum("gap_ms", j.gap_ms);
    o.SetStr("dir", j.dir);
    ppjson::Value sc = ppjson::Value::Arr(pool);
    for (int k = 0; k < j.script_count; ++k) sc.AddStr(j.scripts[k]);
    o.SetValue("scripts", sc);
    ppjson::Value w = ppjson::Value::Obj(pool);
    const char* wt = j.when.type == 1 ? "interval" : (j.when.type == 2 ? "once" : "daily");
    w.SetStr("type", wt);
    if (j.when.type == 1) {
      w.SetNum("seconds", j.when.seconds > 0 ? j.when.seconds : 60);
      w.SetNum("minutes", j.when.minutes > 0 ? j.when.minutes : 1);  // 旧前端兼容
    }
    else if (j.when.type == 2) w.SetStr("at", j.when.at_raw[0] ? j.when.at_raw : "");
    else {
      char buf[16] = {};
      std::snprintf(buf, sizeof(buf), "%02d:%02d", j.when.at_hour, j.when.at_min);
      w.SetStr("at", buf);
      ppjson::Value dys = ppjson::Value::Arr(pool);
      for (int k = 0; k < j.when.day_count; ++k) dys.AddNum(j.when.days[k]);
      w.SetValue("days", dys);
    }
    o.SetValue("when", w);
    char next[32] = {};
    if (j.when.type == 1 && (j.when.seconds % 60) != 0) {
      ScheduleStore::format_time_secs(j.next_run, next, sizeof(next));
    } else {
      ScheduleStore::format_time(j.next_run, next, sizeof(next));
    }
    o.SetStr("next", next);
    o.SetNum("next_run", static_cast<double>(j.next_run));
    o.SetBool("running", busy && running_idx_ == i);
    o.SetBool("pending", j.pending);
    arr.AddValue(o);
  }
  LeaveCriticalSection(&cs_);
  ppjson::Value data = ppjson::Value::Obj(pool);
  data.SetValue("jobs", arr);
  EngineReply(pool, out, true, data, kOk);
}

DWORD WINAPI ScheduleEngine::LoopThunk(LPVOID self) {
  static_cast<ScheduleEngine*>(self)->loop();
  return 0;
}

void ScheduleEngine::loop() {
  while (WaitForSingleObject(stop_ev_, 1000) != WAIT_OBJECT_0) {
    const long long now = static_cast<long long>(std::time(nullptr));
    EnterCriticalSection(&cs_);
    if (task_.is_busy()) {
      LeaveCriticalSection(&cs_);
      continue;  // 正在跑任务：排队中的 job 保持 pending
    }
    // 任务空闲：若刚跑完一个，推进它的下一次时间
    if (running_idx_ >= 0) {
      advance_job(jobs_[running_idx_], now);
      running_idx_ = -1;
    }
    // 选一个要跑的：优先 pending（曾到点但当时在忙），否则现在到点的
    int pick = -1;
    for (int i = 0; i < job_count_; ++i) {
      if (jobs_[i].pending && jobs_[i].enabled) {  // 停用的任务连排队也不跑
        pick = i;
        break;
      }
    }
    if (pick < 0) {
      for (int i = 0; i < job_count_; ++i) {
        const SchedJob& j = jobs_[i];
        if (j.enabled && j.next_run > 0 && now >= j.next_run) {
          pick = i;
          break;
        }
      }
    }
    if (pick >= 0) {
      const bool wasPending = jobs_[pick].pending;
      jobs_[pick].pending = false;
      SchedJob& j = jobs_[pick];
      char jobName[128] = {};
      std::strncpy(jobName, j.name, sizeof(jobName) - 1);
      LeaveCriticalSection(&cs_);
      const bool ok = run_job(j);
      EnterCriticalSection(&cs_);
      if (ok) {
        running_idx_ = pick;
      } else {
        advance_job(j, now);  // 启动失败：推进时间，避免每秒重试
      }
      LeaveCriticalSection(&cs_);
      char buf[192] = {};
      std::snprintf(buf, sizeof(buf), "%s「%s」%s", wasPending ? "补跑排队任务" : "到点触发",
                    jobName, ok ? "已启动" : "启动失败");
      SchedLog(ok ? "info" : "error", buf);
    } else {
      LeaveCriticalSection(&cs_);
    }
  }
}

void ScheduleEngine::start() {
  stop_ev_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  load_from_file();
  thread_ = CreateThread(nullptr, 0, LoopThunk, this, 0, nullptr);
  LogTagged("framework", "sched: engine started");
}

void ScheduleEngine::signal_stop() {
  if (stop_ev_ != nullptr) SetEvent(stop_ev_);
}

void ScheduleEngine::stop_all() {
  signal_stop();
  if (thread_ != nullptr) {
    WaitForSingleObject(thread_, 2000);
    CloseHandle(thread_);
    thread_ = nullptr;
  }
}
