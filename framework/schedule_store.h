// ============================================================================
// schedule_store.h - 定时任务配置（%APPDATA%\TTVoice\schedule.json）的读写与计算。
//
// 配置文件结构（编辑器负责编辑，DLL 只读它并据此触发）：
//   {"version":1,"jobs":[
//     {"id","name","scripts":[...],"enabled":true,"loop":1,"dir":"","gap_ms":0,
//      "when":{"type":"daily","at":"21:30","days":[1,2,3,4,5]}},
//     {"id","name","scripts":["x"],"enabled":true,"when":{"type":"interval","minutes":30}},
//     {"id","name","scripts":["x"],"enabled":true,"when":{"type":"once","at":"2026-10-27 21:30"}}]}
//
// 无 STL：纯 C 结构 + 固定缓冲；字符串走 UTF-8。时间用本地时间（time_t）。
// ============================================================================
#ifndef FRAMEWORK_SCHEDULE_STORE_H
#define FRAMEWORK_SCHEDULE_STORE_H

#include <windows.h>

#include "../common/json.h"

struct SchedWhen {
  int type;                 // 0 daily, 1 interval, 2 once
  int at_hour, at_min;      // daily: 触发时刻
  int minutes;              // interval: 间隔分钟（旧格式，读到时折算进 seconds）
  int seconds;              // interval: 间隔秒数（新格式，优先）
  char at_raw[32];          // once: "YYYY-MM-DD HH:MM"
  int days[7];              // daily: 1..7 = 周一..周日
  int day_count;
  SchedWhen()
      : type(0), at_hour(21), at_min(30), minutes(30), seconds(1800), day_count(0) {
    at_raw[0] = '\0';
    for (int i = 0; i < 7; ++i) days[i] = 0;
  }
};

struct SchedJob {
  char id[16];
  char name[128];
  char scripts[20][128];   // 最多 20 个脚本，按数组顺序执行
  int script_count;
  bool enabled;
  int loop;                 // 整套脚本重复次数
  char dir[256];           // 脚本子目录（相对 scripts，可为空）
  int gap_ms;              // 上一个脚本跑完等待多久再跑下一个
  SchedWhen when;
  // ---- 运行时（不持久化）----
  long long next_run;       // 下次运行（unix 秒，0=不再触发）
  bool pending;             // 到点时正忙 -> 排队，空闲后再跑（不补跑）
  SchedJob() : script_count(0), enabled(true), loop(1), gap_ms(0), next_run(0), pending(false) {
    id[0] = '\0';
    name[0] = '\0';
    for (int i = 0; i < 20; ++i) scripts[i][0] = '\0';
    dir[0] = '\0';
  }
};

class ScheduleStore {
 public:
  // 读 schedule.json 并归一化（script->scripts、默认值、计算 next_run）。
  // 文件不存在/非法 -> 返回空列表（count=0），不算失败。
  static bool load(const wchar_t* path, SchedJob* out, int cap, int* count);
  // 原子写回（tmp + MoveFileEx 覆盖）。
  static bool save(const wchar_t* path, const SchedJob* jobs, int count);
  // 解析一段 JSON（形如 {"jobs":[...]}，兼容裸数组）并规范化写盘：
  // 只落持久化字段（不含 next/next_run/running/pending 等运行时字段）。
  // 文本非法/结构不对 -> 返回 false，原文件保持不变。
  static bool save_text(const wchar_t* path, const char* text);

  // %APPDATA%\TTVoice\schedule.json 的完整路径。
  static bool get_path(wchar_t* out, int cap);

  // 计算下一次运行时间（>= now）。返回 false 表示不再触发（once 已过/无效）。
  static bool compute_next(const SchedJob& job, long long now, long long& out);

  // when 的人话描述（UI/日志共用）。
  static void describe_when(const SchedJob& job, char* out, int cap);
  // 把 unix 秒格式化为 "YYYY-MM-DD HH:MM"，0 写 "-"。
  static void format_time(long long t, char* out, int cap);
  // 精确到秒的版本（秒级间隔任务展示下次时间用）。
  static void format_time_secs(long long t, char* out, int cap);
};

#endif  // FRAMEWORK_SCHEDULE_STORE_H
