// ============================================================================
// schedule_store.cpp - 见 schedule_store.h。
// ============================================================================
#include "schedule_store.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <ctime>

#include <windows.h>

namespace {

const int kJobCap = 64;
const int kScriptsPerJob = 20;

// 堆上 JSON 池（解析/构造用）。
struct HeapNodes {
  ppjson::Node* nodes;
  ppjson::Pool pool;
  explicit HeapNodes(int n)
      : nodes(static_cast<ppjson::Node*>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                      sizeof(ppjson::Node) * static_cast<size_t>(n)))),
        pool(nodes, n) {}
  ~HeapNodes() {
    if (nodes) HeapFree(GetProcessHeap(), 0, nodes);
  }
  bool ok() const { return nodes != nullptr; }
  HeapNodes(const HeapNodes&) = delete;
  HeapNodes& operator=(const HeapNodes&) = delete;
};

// 大文本缓冲同样不能放栈：load() 会在 CivetWeb 工作线程里被 reload() 调用。
struct HeapChars {
  char* p;
  int cap;
  explicit HeapChars(int n)
      : p(static_cast<char*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                       static_cast<size_t>(n)))),
        cap(n) {}
  ~HeapChars() {
    if (p != nullptr) HeapFree(GetProcessHeap(), 0, p);
  }
  bool ok() const { return p != nullptr; }
  HeapChars(const HeapChars&) = delete;
  HeapChars& operator=(const HeapChars&) = delete;
};

// 读整个 UTF-8 文本文件（参考 script_store 的实现，独立一份避免跨 TU 依赖）。
int ReadTextFile(const wchar_t* path, char* out, int cap) {
  out[0] = '\0';
  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return -1;
  DWORD got = 0;
  const BOOL ok = ReadFile(h, out, static_cast<DWORD>(cap - 1), &got, nullptr);
  CloseHandle(h);
  if (!ok) return -1;
  if (static_cast<int>(got) >= cap) got = static_cast<DWORD>(cap - 1);
  out[got] = '\0';
  return static_cast<int>(got);
}

bool ParseHHMM(const char* s, int* h, int* m) {
  int H = 0, M = 0;
  if (s == nullptr || std::sscanf(s, "%d:%d", &H, &M) != 2) return false;
  if (H < 0 || H > 23 || M < 0 || M > 59) return false;
  *h = H;
  *m = M;
  return true;
}

bool ParseDateTime(const char* s, struct tm* t) {
  int Y = 0, Mo = 0, D = 0, H = 0, Mi = 0;
  if (s == nullptr ||
      std::sscanf(s, "%d-%d-%d %d:%d", &Y, &Mo, &D, &H, &Mi) != 5) {
    return false;
  }
  std::memset(t, 0, sizeof(*t));
  t->tm_year = Y - 1900;
  t->tm_mon = Mo - 1;
  t->tm_mday = D;
  t->tm_hour = H;
  t->tm_min = Mi;
  t->tm_sec = 0;
  t->tm_isdst = -1;
  return true;
}

// 周一..周日 标签
const char* DayLabel(int d) {
  switch (d) {
    case 1: return "一";
    case 2: return "二";
    case 3: return "三";
    case 4: return "四";
    case 5: return "五";
    case 6: return "六";
    case 7: return "日";
    default: return "?";
  }
}

}  // namespace

bool ScheduleStore::get_path(wchar_t* out, int cap) {
  wchar_t app[512] = {};
  const DWORD n = GetEnvironmentVariableW(L"APPDATA", app, 512);
  if (n == 0 || n >= 512) return false;
  return swprintf_s(out, static_cast<size_t>(cap),
                   L"%s\\TTVoice\\schedule.json", app) > 0;
}

bool ScheduleStore::compute_next(const SchedJob& job, long long now,
                                long long& out) {
  const SchedWhen& w = job.when;
  if (w.type == 1) {  // interval（秒级）
    out = now + static_cast<long long>(w.seconds > 0 ? w.seconds : 60);
    return true;
  }
  if (w.type == 2) {  // once
    struct tm t;
    if (!ParseDateTime(w.at_raw, &t)) {
      out = 0;
      return false;
    }
    const time_t cand = mktime(&t);
    if (cand <= static_cast<time_t>(now)) {
      out = 0;  // 已过期 -> 不补跑
      return false;
    }
    out = static_cast<long long>(cand);
    return true;
  }
  // daily：从 now 起找首个匹配的 at 时刻（按 weekday 过滤）
  time_t cur = static_cast<time_t>(now);
  for (int k = 0; k < 400; ++k) {
    struct tm* base = std::localtime(&cur);
    if (base == nullptr) {
      out = 0;
      return false;
    }
    struct tm cand = *base;
    cand.tm_hour = w.at_hour;
    cand.tm_min = w.at_min;
    cand.tm_sec = 0;
    cand.tm_isdst = -1;
    const time_t c = mktime(&cand);
    if (c < 0) {
      out = 0;
      return false;
    }
    bool ok_day = (w.day_count == 0);
    if (!ok_day) {
      const int wd = (cand.tm_wday == 0) ? 7 : cand.tm_wday;  // 1=Mon..7=Sun
      for (int d = 0; d < w.day_count; ++d) {
        if (w.days[d] == wd) {
          ok_day = true;
          break;
        }
      }
    }
    if (c >= static_cast<time_t>(now) && ok_day) {
      out = static_cast<long long>(c);
      return true;
    }
    cur = c + 86400;  // 推进一天
  }
  out = 0;
  return false;
}

void ScheduleStore::describe_when(const SchedJob& job, char* out, int cap) {
  const SchedWhen& w = job.when;
  if (w.type == 1) {
    const int secs = w.seconds > 0 ? w.seconds : 60;
    if (secs % 60 == 0) {
      std::snprintf(out, static_cast<size_t>(cap), "每 %d 分钟", secs / 60);
    } else {
      std::snprintf(out, static_cast<size_t>(cap), "每 %d 秒", secs);
    }
    return;
  }
  if (w.type == 2) {
    std::snprintf(out, static_cast<size_t>(cap), "一次性 %s",
                  w.at_raw[0] ? w.at_raw : "-");
    return;
  }
  char prefix[32] = {};
  if (w.day_count == 0) {
    std::snprintf(prefix, sizeof(prefix), "每天");
  } else {
    int off = 0;
    off += std::snprintf(prefix + off, sizeof(prefix) - off, "周");
    for (int i = 0; i < w.day_count; ++i) {
      off += std::snprintf(prefix + off, sizeof(prefix) - off, "%s",
                           DayLabel(w.days[i]));
    }
  }
  std::snprintf(out, static_cast<size_t>(cap), "%s %02d:%02d", prefix, w.at_hour,
                w.at_min);
}

void ScheduleStore::format_time(long long t, char* out, int cap) {
  if (t <= 0) {
    std::snprintf(out, static_cast<size_t>(cap), "-");
    return;
  }
  const time_t tt = static_cast<time_t>(t);
  struct tm* p = std::localtime(&tt);
  if (p == nullptr) {
    std::snprintf(out, static_cast<size_t>(cap), "-");
    return;
  }
  std::snprintf(out, static_cast<size_t>(cap), "%04d-%02d-%02d %02d:%02d",
                p->tm_year + 1900, p->tm_mon + 1, p->tm_mday, p->tm_hour,
                p->tm_min);
}

void ScheduleStore::format_time_secs(long long t, char* out, int cap) {
  if (t <= 0) {
    std::snprintf(out, static_cast<size_t>(cap), "-");
    return;
  }
  const time_t tt = static_cast<time_t>(t);
  struct tm* p = std::localtime(&tt);
  if (p == nullptr) {
    std::snprintf(out, static_cast<size_t>(cap), "-");
    return;
  }
  std::snprintf(out, static_cast<size_t>(cap), "%04d-%02d-%02d %02d:%02d:%02d",
                p->tm_year + 1900, p->tm_mon + 1, p->tm_mday, p->tm_hour,
                p->tm_min, p->tm_sec);
}

namespace {

// 解析 {"jobs":[...]}（也兼容裸数组），填充 SchedJob 并计算 next_run。
// 返回 false 表示文本非法（PUT 保存应拒绝并保持原文件）；解析成功但为空 -> *count=0。
bool ParseJobs(const char* text, SchedJob* out, int cap, int* count) {
  *count = 0;
  if (text == nullptr || text[0] == '\0' || out == nullptr || cap <= 0) return false;
  // 跳过 UTF-8 BOM：外部编辑器（记事本等）保存会带上，不然整份配置会被当成非法 JSON，
  // 表现为“所有定时任务莫名其妙不见了/不触发”。
  const unsigned char* bom = reinterpret_cast<const unsigned char*>(text);
  if (bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF) {
    text += 3;
    if (text[0] == '\0') return false;
  }
  HeapNodes hp(4096);
  if (!hp.ok()) return false;
  ppjson::Value root;
  char err[64] = {};
  if (!ppjson::Value::Parse(text, hp.pool, root, err, sizeof(err))) return false;
  ppjson::Value jobs = root.IsObj() ? root.Find("jobs") : root;
  if (!jobs.IsArr()) return false;

  int cnt = 0;
  for (int i = 0; i < jobs.Size() && cnt < cap; ++i) {
    ppjson::Value j = jobs.At(i);
    if (!j.IsObj()) continue;
    SchedJob& job = out[cnt];
    std::memset(&job, 0, sizeof(job));

    const char* id = j.Find("id").AsStr();
    if (id) std::strncpy(job.id, id, 15);
    const char* nm = j.Find("name").AsStr();
    if (nm) std::strncpy(job.name, nm, 127);

    ppjson::Value sc = j.Find("scripts");
    if (sc.IsArr()) {
      for (int k = 0; k < sc.Size() && job.script_count < kScriptsPerJob; ++k) {
        const char* s = sc.At(k).AsStr();
        if (s && *s) std::strncpy(job.scripts[job.script_count++], s, 127);
      }
    }
    if (job.script_count == 0) {
      const char* single = j.Find("script").AsStr();
      if (single && *single) {
        std::strncpy(job.scripts[0], single, 127);
        job.script_count = 1;
      }
    }

    job.enabled = j.Find("enabled").AsBool(true);
    job.loop = static_cast<int>(j.Find("loop").AsInt(1));
    if (job.loop <= 0) job.loop = 1;
    const char* dr = j.Find("dir").AsStr();
    if (dr) std::strncpy(job.dir, dr, 255);
    job.gap_ms = static_cast<int>(j.Find("gap_ms").AsInt(0));
    if (job.gap_ms < 0) job.gap_ms = 0;

    ppjson::Value w = j.Find("when");
    if (!w.IsObj()) w = ppjson::Value::Obj(hp.pool);
    const char* wt = w.Find("type").AsStr();
    if (wt && std::strcmp(wt, "interval") == 0)
      job.when.type = 1;
    else if (wt && std::strcmp(wt, "once") == 0)
      job.when.type = 2;
    else
      job.when.type = 0;

    const char* at = w.Find("at").AsStr();
    if (job.when.type == 2) {
      if (at) std::strncpy(job.when.at_raw, at, 31);
    } else {
      int h = 21, m = 30;
      if (at && ParseHHMM(at, &h, &m)) {
        job.when.at_hour = h;
        job.when.at_min = m;
      }
    }
    // 间隔：优先新格式 seconds，其次旧格式 minutes（*60），都没有则 30 分钟。
    {
      const int secs = static_cast<int>(w.Find("seconds").AsInt(0));
      const int mins = static_cast<int>(w.Find("minutes").AsInt(0));
      int total = (secs > 0) ? secs : ((mins > 0) ? mins * 60 : 1800);
      if (total < 1) total = 1;
      job.when.seconds = total;
      job.when.minutes = (total + 59) / 60;  // 兼容旧字段的展示
    }
    ppjson::Value dys = w.Find("days");
    if (dys.IsArr()) {
      for (int k = 0; k < dys.Size() && job.when.day_count < 7; ++k) {
        const int d = static_cast<int>(dys.At(k).AsInt(0));
        if (d >= 1 && d <= 7) job.when.days[job.when.day_count++] = d;
      }
    }

    const long long now = static_cast<long long>(std::time(nullptr));
    ScheduleStore::compute_next(job, now, job.next_run);
    ++cnt;
  }
  *count = cnt;
  return true;
}

}  // namespace

bool ScheduleStore::load(const wchar_t* path, SchedJob* out, int cap,
                         int* count) {
  *count = 0;
  HeapChars text(256 * 1024);
  if (!text.ok()) return false;
  const int n = ReadTextFile(path, text.p, text.cap);
  if (n < 0 || text.p[0] == '\0') return true;  // 无文件 -> 空列表
  int cnt = 0;
  if (!ParseJobs(text.p, out, cap, &cnt)) return true;  // 非法 JSON -> 空列表
  *count = cnt;
  return true;
}

bool ScheduleStore::save_text(const wchar_t* path, const char* text) {
  SchedJob jobs[64];
  int count = 0;
  if (!ParseJobs(text, jobs, 64, &count)) return false;
  return save(path, jobs, count);
}

bool ScheduleStore::save(const wchar_t* path, const SchedJob* jobs, int count) {
  HeapNodes hp(8192);
  if (!hp.ok()) return false;
  ppjson::Pool& pool = hp.pool;
  ppjson::Value root = ppjson::Value::Obj(pool);
  root.SetNum("version", 1);
  ppjson::Value arr = ppjson::Value::Arr(pool);
  for (int i = 0; i < count; ++i) {
    const SchedJob& job = jobs[i];
    ppjson::Value o = ppjson::Value::Obj(pool);
    o.SetStr("id", job.id[0] ? job.id : "");
    o.SetStr("name", job.name);
    ppjson::Value sc = ppjson::Value::Arr(pool);
    for (int k = 0; k < job.script_count; ++k) sc.AddStr(job.scripts[k]);
    o.SetValue("scripts", sc);
    o.SetBool("enabled", job.enabled);
    o.SetNum("loop", job.loop);
    o.SetStr("dir", job.dir);
    o.SetNum("gap_ms", job.gap_ms);

    ppjson::Value w = ppjson::Value::Obj(pool);
    const char* wt = job.when.type == 1 ? "interval" : (job.when.type == 2 ? "once" : "daily");
    w.SetStr("type", wt);
    if (job.when.type == 1) {
      w.SetNum("seconds", job.when.seconds > 0 ? job.when.seconds : 60);
    } else if (job.when.type == 2) {
      w.SetStr("at", job.when.at_raw[0] ? job.when.at_raw : "2026-01-01 00:00");
    } else {
      char buf[16] = {};
      std::snprintf(buf, sizeof(buf), "%02d:%02d", job.when.at_hour, job.when.at_min);
      w.SetStr("at", buf);
      ppjson::Value dys = ppjson::Value::Arr(pool);
      for (int k = 0; k < job.when.day_count; ++k) dys.AddNum(job.when.days[k]);
      w.SetValue("days", dys);
    }
    o.SetValue("when", w);
    arr.AddValue(o);
  }
  root.SetValue("jobs", arr);

  HeapChars body(64 * 1024);
  if (!body.ok()) return false;
  const int n = root.Dump(body.p, body.cap);
  if (n < 0) return false;

  wchar_t tmp[640] = {};
  swprintf_s(tmp, L"%s.tmp", path);
  HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const BOOL ok = WriteFile(h, body.p, static_cast<DWORD>(n), &written, nullptr);
  CloseHandle(h);
  if (!ok || static_cast<int>(written) != n) {
    DeleteFileW(tmp);
    return false;
  }
  if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(path);
    if (!MoveFileExW(tmp, path, 0)) {
      DeleteFileW(tmp);
      return false;
    }
  }
  return true;
}
