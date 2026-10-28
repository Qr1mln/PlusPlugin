// ============================================================================
// window_target.cpp - 见 window_target.h。
// ============================================================================
#include "window_target.h"

#include "pplugin.h"

HWND WindowTarget::target() const {
  HWND h = nullptr;
  if (host_ != nullptr && host_->viewWindow != nullptr) {
    h = host_->viewWindow();
  }
  if (h == nullptr && host_ != nullptr && host_->mainWindow != nullptr) {
    h = host_->mainWindow();
  }
  return h;
}

bool WindowTarget::view_rect(RECT& out) const {
  HWND wnd = target();
  if (wnd == nullptr) {
    return false;
  }
  RECT rc;  // 客户区大小（客户坐标）
  if (!GetClientRect(wnd, &rc)) {
    return false;
  }
  POINT tl = {0, 0};  // 客户区左上角 -> 屏幕坐标
  if (!ClientToScreen(wnd, &tl)) {
    return false;
  }
  out.left = tl.x;
  out.top = tl.y;
  out.right = tl.x + (rc.right - rc.left);
  out.bottom = tl.y + (rc.bottom - rc.top);
  if (out.right <= out.left || out.bottom <= out.top) {
    return false;
  }
  return true;
}

bool WindowTarget::resolve_client(int x, int y, const std::string& space, int& cx,
                                  int& cy) const {
  HWND w = target();
  if (w == nullptr) {
    return false;
  }
  if (space == "screen") {
    POINT p = {x, y};
    if (!ScreenToClient(w, &p)) {
      return false;
    }
    cx = p.x;
    cy = p.y;
  } else {
    cx = x;
    cy = y;
  }
  return true;
}

bool WindowTarget::geometry(nlohmann::json& out) const {
  RECT rc;
  if (!view_rect(rc)) {
    return false;
  }
  out["x"] = rc.left;
  out["y"] = rc.top;
  out["w"] = rc.right - rc.left;
  out["h"] = rc.bottom - rc.top;
  out["scale"] = (host_ != nullptr && host_->scale != nullptr) ? host_->scale() : 1.0f;
  return true;
}
