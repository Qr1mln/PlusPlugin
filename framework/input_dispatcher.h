// ============================================================================
// input_dispatcher.h - 后台输入派发（PostMessage，不动物理鼠标）。
//
// 把鼠标/键盘/滚轮/文本事件以 PostMessage 形式投递到目标窗口（见 WindowTarget）。
// 设计为无状态、对 WindowTarget 的只读引用，方便单测与复用。
// ============================================================================
#ifndef FRAMEWORK_INPUT_DISPATCHER_H
#define FRAMEWORK_INPUT_DISPATCHER_H

#include <string>

#include <windows.h>

class WindowTarget;  // 前向声明

class InputDispatcher {
 public:
  explicit InputDispatcher(const WindowTarget& win) : win_(win) {}

  void move(int dx, int dy);
  void move_to(int x, int y, const std::string& space);
  void click(int x, int y, const std::string& space, const std::string& button);
  void button(int x, int y, const std::string& space, const std::string& button,
              bool down);
  void key(WORD vk);
  void delay(int ms);
  void scroll(int delta);
  void text(const std::string& utf8);

 private:
  // 向目标窗口投递鼠标消息（客户区坐标）。
  void post_mouse(UINT msg, WPARAM wParam, int cx, int cy) const;

  static UINT button_down_msg(const std::string& button);
  static UINT button_up_msg(const std::string& button);
  static WPARAM button_down_key(const std::string& button);

  const WindowTarget& win_;
};

#endif  // FRAMEWORK_INPUT_DISPATCHER_H
