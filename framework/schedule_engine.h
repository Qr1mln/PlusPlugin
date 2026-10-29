// ============================================================================
// schedule_engine.h - 定时触发引擎（进程内）。
//
// 持有 jobs（来自 schedule.json），后台线程每秒检查触发条件：到点且当前没有任务在跑
// 就启动（复用 TaskScheduler 单任务执行器）；正忙则排队（pending），空闲后跑一次
// （不补跑错过的多次）。每次跑完按 when 计算下一次时间（once 跑过即不再触发）。
// ============================================================================
#ifndef FRAMEWORK_SCHEDULE_ENGINE_H
#define FRAMEWORK_SCHEDULE_ENGINE_H

#include <windows.h>

#include "../common/json.h"
#include "schedule_store.h"

class WindowTarget;
class InputDispatcher;
class ScriptStore;
class TaskScheduler;

class ScheduleEngine {
 public:
  ScheduleEngine(WindowTarget& win, InputDispatcher& input, ScriptStore& scripts,
                 TaskScheduler& task);
  ~ScheduleEngine();

  // 读 schedule.json 并归一化 + 计算 next_run。
  void load_from_file();
  // 重新加载（保存后或外部改动文件后调用）。
  void reload();
  // 立即执行某任务（不等触发时刻，仍受单任务约束：忙则排队）。
  // 返回 false 表示找不到该任务或启动失败（脚本读取失败等）；排队成功返回 true。
  bool run_now(const char* job_id);
  // 列出任务及下次运行时间：out 为封套 {success,data:{jobs:[...]}}。
  void get_status(ppjson::Pool& pool, ppjson::Value& out);

  void start();        // 启动触发线程
  void signal_stop();  // 仅发停止信号（DLL 卸载用）
  void stop_all();     // 等线程退出

 private:
  static DWORD WINAPI LoopThunk(LPVOID self);
  void loop();
  bool run_job(SchedJob& job);  // 调 task_.start_job；返回是否成功启动
  void advance_job(SchedJob& job, long long completed);  // 算下一次/标记完成
  int find_job(const char* id) const;

  WindowTarget& win_;
  InputDispatcher& input_;
  ScriptStore& scripts_;
  TaskScheduler& task_;

  CRITICAL_SECTION cs_;
  HANDLE stop_ev_ = nullptr;
  HANDLE thread_ = nullptr;
  SchedJob jobs_[64];
  int job_count_ = 0;
  int running_idx_ = -1;  // 当前正在跑的任务下标（-1 表示无）
};

#endif  // FRAMEWORK_SCHEDULE_ENGINE_H
