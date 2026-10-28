// ============================================================================
// input_dispatcher.cpp - 见 input_dispatcher.h。
//
// 注意：PostMessage 不进系统输入队列，个别程序（Raw Input / 特殊自绘）可能不响应。
// ============================================================================
#include "input_dispatcher.h"

#include <cstring>

#include "window_target.h"

void InputDispatcher::post_mouse(UINT msg, WPARAM wParam, int cx, int cy) const {
  HWND w = win_.target();
  if (w == nullptr) {
    return;
  }
  PostMessageW(w, msg, wParam,
               MAKELPARAM(static_cast<short>(cx), static_cast<short>(cy)));
}

void InputDispatcher::move(int dx, int dy) {
  win_.set_cursor(win_.cursor_x() + dx, win_.cursor_y() + dy);
  post_mouse(WM_MOUSEMOVE, 0, win_.cursor_x(), win_.cursor_y());
}

void InputDispatcher::move_to(int x, int y, const char* space) {
  int cx = x;
  int cy = y;
  if (!win_.resolve_client(x, y, space, cx, cy)) {
    return;
  }
  win_.set_cursor(cx, cy);
  post_mouse(WM_MOUSEMOVE, 0, cx, cy);
}

UINT InputDispatcher::button_down_msg(const char* button) {
  if (button != nullptr && std::strcmp(button, "right") == 0) {
    return WM_RBUTTONDOWN;
  }
  if (button != nullptr && std::strcmp(button, "middle") == 0) {
    return WM_MBUTTONDOWN;
  }
  return WM_LBUTTONDOWN;
}

UINT InputDispatcher::button_up_msg(const char* button) {
  if (button != nullptr && std::strcmp(button, "right") == 0) {
    return WM_RBUTTONUP;
  }
  if (button != nullptr && std::strcmp(button, "middle") == 0) {
    return WM_MBUTTONUP;
  }
  return WM_LBUTTONUP;
}

WPARAM InputDispatcher::button_down_key(const char* button) {
  if (button != nullptr && std::strcmp(button, "right") == 0) {
    return MK_RBUTTON;
  }
  if (button != nullptr && std::strcmp(button, "middle") == 0) {
    return MK_MBUTTON;
  }
  return MK_LBUTTON;
}

void InputDispatcher::click(int x, int y, const char* space, const char* button) {
  if (x >= 0 && y >= 0) {
    int cx = x;
    int cy = y;
    if (win_.resolve_client(x, y, space, cx, cy)) {
      win_.set_cursor(cx, cy);
    }
  }
  post_mouse(WM_MOUSEMOVE, 0, win_.cursor_x(), win_.cursor_y());
  post_mouse(button_down_msg(button), button_down_key(button), win_.cursor_x(),
             win_.cursor_y());
  post_mouse(button_up_msg(button), 0, win_.cursor_x(), win_.cursor_y());
}

void InputDispatcher::button(int x, int y, const char* space, const char* button,
                             bool down) {
  if (x >= 0 && y >= 0) {
    int cx = x;
    int cy = y;
    if (win_.resolve_client(x, y, space, cx, cy)) {
      win_.set_cursor(cx, cy);
    }
  }
  if (down) {
    post_mouse(button_down_msg(button), button_down_key(button), win_.cursor_x(),
               win_.cursor_y());
  } else {
    post_mouse(button_up_msg(button), 0, win_.cursor_x(), win_.cursor_y());
  }
}

void InputDispatcher::key(WORD vk) {
  HWND w = win_.target();
  if (w == nullptr) {
    return;
  }
  PostMessageW(w, WM_KEYDOWN, vk, 0);
  PostMessageW(w, WM_KEYUP, vk, 0);
}

void InputDispatcher::delay(int ms) {
  if (ms > 0 && ms <= 60000) {
    Sleep(static_cast<DWORD>(ms));
  }
}

// 滚轮：delta 以 WHEEL_DELTA(120) 的倍数，正=上滚，负=下滚。
void InputDispatcher::scroll(int delta) {
  if (delta == 0) {
    return;
  }
  HWND w = win_.target();
  if (w == nullptr) {
    return;
  }
  POINT p = {win_.cursor_x(), win_.cursor_y()};
  ClientToScreen(w, &p);
  PostMessageW(w, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<short>(delta)),
               MAKELPARAM(p.x, p.y));
}

// 文本输入：UTF-8 -> UTF-16，逐字符向目标窗口投递 WM_CHAR。
// 用 HeapAlloc 的临时宽缓冲，避免 std::wstring。
void InputDispatcher::text(const char* utf8) {
  if (utf8 == nullptr || utf8[0] == '\0') {
    return;
  }
  HWND w = win_.target();
  if (w == nullptr) {
    return;
  }
  const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
  if (wlen <= 1) {
    return;
  }
  wchar_t* ws = static_cast<wchar_t*>(
      HeapAlloc(GetProcessHeap(), 0, sizeof(wchar_t) * static_cast<size_t>(wlen)));
  if (ws == nullptr) {
    return;
  }
  MultiByteToWideChar(CP_UTF8, 0, utf8, -1, ws, wlen);
  for (int i = 0; i < wlen - 1; ++i) {
    PostMessageW(w, WM_CHAR, static_cast<WPARAM>(ws[i]), 0);
  }
  HeapFree(GetProcessHeap(), 0, ws);
}