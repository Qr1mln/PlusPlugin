// ============================================================================
// task_scheduler.h - 进程内任务调度器（单任务串行）。
//
// 在 framework.dll 内跑脚本：读脚本 JSON 的 events，逐条经 InputDispatcher 执行，
// 支持循环次数、暂停/继续、停止（delay 可中断）。任务状态可被 WS / HTTP 查询，
// 因此浏览器关掉也继续跑。
//
// 无 STL：事件从脚本 JSON 提取为结构化副本（TaskEvent），执行线程直接用，不依赖
// JSON 池；字符串走固定缓冲（ppjson 字符串上限 512）。
// ============================================================================
#ifndef FRAMEWORK_TASK_SCHEDULER_H
#define FRAMEWORK_TASK_SCHEDULER_H

#include <windows.h>

#include "../common/json.h"

class WindowTarget;
class InputDispatcher;
class ScriptStore;

// 从脚本 JSON 提取出的单条事件结构化副本（执行线程直接用，不依赖 JSON 池）。
struct TaskEvent {
  int type;                 // 0 mouse, 1 delay, 2 key, 3 text, 4 findImage(跳过)
  char action[16];
  char button[16];
  int x, y;
  int ms;
  int wheel;
  int vk;
  int text_mode;            // 0 fixed, 1 random
  char text[512];
  char texts[16][512];
  int text_count;
};

class TaskScheduler {
 public:
  TaskScheduler(WindowTarget& win, InputDispatcher& input, ScriptStore& scripts);
  ~TaskScheduler();

  // 启动脚本任务（单任务：已有活跃任务则失败）。loop<=0 用脚本自带 loop，再缺省 1。
  // out 直接写封套 {success,data,code}。
  void start(ppjson::Pool& pool, const char* name, int loop, ppjson::Value& out);

  // 启动一个定时任务（多脚本顺序执行，脚本间等待 gap_ms；整套重复 loop 次）。
  // scripts 为脚本名数组（scripts 在相对 dir 子目录下，dir 为空则用默认 scripts 根）。
  // 单任务：已有活跃任务则失败。
  void start_job(ppjson::Pool& pool, const char* job_name, const char* dir,
                 const char* scripts[], int script_count, int loop, int gap_ms,
                 ppjson::Value& out);

  void stop(ppjson::Pool& pool, int id, ppjson::Value& out);
  void pause(ppjson::Pool& pool, int id, ppjson::Value& out);
  void resume(ppjson::Pool& pool, int id, ppjson::Value& out);
  // 返回任务状态数组（out 为封套 {success,data:{tasks:[...]}}）。
  void list(ppjson::Pool& pool, ppjson::Value& out);

  // 当前是否有任务在跑（运行或暂停中）。调度引擎据此判断是否到点排队。
  bool is_busy() const;

  // 正常关闭：发停止信号并等待线程退出。
  void stop_all();
  // 仅发停止信号（DLL 卸载时用，避免持锁等待）。
  void signal_stop();

  enum Status { kStopped = 0, kRunning, kPaused, kDone, kError };

  enum EventType {  // TaskEvent.type
    kMouse = 0,
    kDelay = 1,
    kKey = 2,
    kText = 3,
    kFindImage = 4,  // 暂不支持，执行时跳过
    kGap = 5,        // 脚本间等待（可中断）
  };

 private:
  static DWORD WINAPI RunThunk(LPVOID self);
  void run();
  void exec_event(const TaskEvent& ev, float scale);
  void extract_event(ppjson::Value ev, TaskEvent& out) const;
  int key_name_to_vk(const char* name) const;
  void free_events();

  WindowTarget& win_;
  InputDispatcher& input_;
  ScriptStore& scripts_;

  CRITICAL_SECTION cs_;
  HANDLE stop_ev_ = nullptr;
  HANDLE thread_ = nullptr;
  volatile LONG status_ = kStopped;
  volatile LONG idx_ = 0;       // 当前执行到第几条（0-based）
  volatile LONG loop_cur_ = 0;
  int loop_total_ = 1;
  int event_count_ = 0;
  int task_id_ = 0;
  TaskEvent* events_ = nullptr;  // HeapAlloc
  char name_[128] = {};
  bool active_ = false;          // 是否有任务（含已结束）
};

#endif  // FRAMEWORK_TASK_SCHEDULER_H
