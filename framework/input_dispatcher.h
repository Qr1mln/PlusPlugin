// ============================================================================
// input_dispatcher.h - 后台输入派发（PostMessage，不动物理鼠标）。
//
// 把鼠标/键盘/滚轮/文本事件以 PostMessage 形式投递到目标窗口（见 WindowTarget）。
// 无状态，但持有 WindowTarget 的**可写**引用：输入会更新其虚拟光标。
//
// 无 STL：参数用 const char*（调用方保证生命周期）。
// ============================================================================
#ifndef FRAMEWORK_INPUT_DISPATCHER_H
#define FRAMEWORK_INPUT_DISPATCHER_H

#include <windows.h>

class WindowTarget;  // 前向声明

class InputDispatcher {
 public:
  explicit InputDispatcher(WindowTarget& win) : win_(win) {}

  void move(int dx, int dy);
  void move_to(int x, int y, const char* space);
  void click(int x, int y, const char* space, const char* button);
  void button(int x, int y, const char* space, const char* button, bool down);
  void key(WORD vk);
  void delay(int ms);
  void scroll(int delta);
  void text(const char* utf8);

 private:
  // 向目标窗口投递鼠标消息（客户区坐标）。
  void post_mouse(UINT msg, WPARAM wParam, int cx, int cy) const;

  static UINT button_down_msg(const char* button);
  static UINT button_up_msg(const char* button);
  static WPARAM button_down_key(const char* button);

  WindowTarget& win_;
};

#endif  // FRAMEWORK_INPUT_DISPATCHER_H