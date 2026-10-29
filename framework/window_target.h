// ============================================================================
// window_target.h - 目标窗口与坐标换算。
//
// 把“找窗口 + 客户区矩形 + 屏幕/客户区坐标换算 + 虚拟光标”封装成一个对象，
// 其余模块（输入派发、取几何）只依赖它，不再各自持有 g_host / g_cx / g_cy。
// ============================================================================
#ifndef FRAMEWORK_WINDOW_TARGET_H
#define FRAMEWORK_WINDOW_TARGET_H

#include <windows.h>

#include "../common/json.h"
#include "host_caps.h"

class WindowTarget {
 public:
  void set_host(const HostCaps* host) noexcept { host_ = host; }

  // 目标窗口：优先 viewWindow（真正接收输入的 Flutter 子窗口），回退 mainWindow。
  HWND target() const;

  // 客户区在屏幕上的物理像素矩形；不可用时返回 false。
  bool view_rect(RECT& out) const;

  // 把 (x,y)（space 指定 "screen"/其他）解析成客户区坐标；窗口不可用返回 false。
  bool resolve_client(int x, int y, const char* space, int& cx, int& cy) const;

  // 几何信息对象 {x,y,w,h,scale}；窗口不可用时返回 false。
  bool geometry(ppjson::Value& out) const;

  // 让宿主重新解析目标窗口（编辑器"刷新应用"用）；宿主不支持则返回 false。
  bool refresh() const;

  // DPI 缩放（96dpi = 1.0）；宿主不支持返回 1.0。调度器换算逻辑坐标用。
  float scale() const;

  // 目标窗口的顶层窗口是否最小化（最小化时客户区屏幕坐标是 -32000，不能算几何）。
  bool target_minimized() const;

  // 插件内部维护的虚拟光标（客户区坐标），供相对移动/拖拽使用。
  int cursor_x() const noexcept { return cx_; }
  int cursor_y() const noexcept { return cy_; }
  void set_cursor(int x, int y) noexcept { cx_ = x; cy_ = y; }

 private:
  const HostCaps* host_ = nullptr;
  int cx_ = 0;
  int cy_ = 0;
};

#endif  // FRAMEWORK_WINDOW_TARGET_H
