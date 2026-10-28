// ============================================================================
// pick_overlay.cpp - 见 pick_overlay.h。
//
// 分层窗口：WS_EX_LAYERED + SetLayeredWindowAttributes(black, 235, COLORKEY|ALPHA)
// 等价于 Python 的 attributes("-transparentcolor", "black") + attributes("-alpha", 0.92)：
// 纯黑像素完全不挡视线，画上去的线条/文字按 92% 不透明渲染。
// WS_EX_TRANSPARENT / WM_NCHITTEST=HTTRANSPARENT 让鼠标消息照常落到下面的应用。
// ============================================================================
#include "pick_overlay.h"

#include <windows.h>

#include <cwchar>

#include "host_caps.h"

#include "../common/log.h"

namespace {

const wchar_t* kClassName = L"PlusPluginPickOverlay";

// 配色与 Python 版一致：窗口内青色、窗口外珊瑚红、提示黄色。
const COLORREF kKeyColor = RGB(0x00, 0x00, 0x00);
const BYTE kAlpha = 235;
const COLORREF kColorIn = RGB(0x00, 0xE5, 0xFF);
const COLORREF kColorOut = RGB(0xFF, 0x8A, 0x80);
const COLORREF kTagBg = RGB(0x10, 0x18, 0x20);
const COLORREF kTagText = RGB(0xFF, 0xFF, 0xFF);
const COLORREF kTipText = RGB(0xFF, 0xD5, 0x4F);

const HostCaps* g_host = nullptr;
HWND g_hwnd = nullptr;
bool g_classRegistered = false;
HFONT g_fontCoord = nullptr;
HFONT g_fontTip = nullptr;

int g_w = 0;      // 遮罩尺寸（= view 客户区物理像素）
int g_h = 0;
int g_x = 0;      // 遮罩左上角屏幕坐标
int g_y = 0;
POINT g_cur = {0, 0};   // 光标在 view 客户区里的物理像素
bool g_inside = false;  // 光标是否落在窗口内（决定配色与提示文案）
bool g_want = false;    // 拾取态：遮罩应该显示（窗口最小化时先挂起）
double g_scale = 1.0;

HWND TargetWindow() {
  HWND h = nullptr;
  if (g_host != nullptr && g_host->viewWindow != nullptr) {
    h = g_host->viewWindow();
  }
  if (h == nullptr && g_host != nullptr && g_host->mainWindow != nullptr) {
    h = g_host->mainWindow();
  }
  return h;
}

double HostScale() {
  const double s =
      (g_host != nullptr && g_host->scale != nullptr) ? g_host->scale() : 1.0;
  return (s > 0.0) ? s : 1.0;
}

double Round1(double v) {
  return (v >= 0.0) ? static_cast<double>(static_cast<long long>(v * 10.0 + 0.5)) / 10.0
                    : static_cast<double>(static_cast<long long>(v * 10.0 - 0.5)) / 10.0;
}

void EnsureFonts() {
  if (g_fontCoord != nullptr && g_fontTip != nullptr) {
    return;
  }
  g_scale = HostScale();
  const int coordH = static_cast<int>(13.0 * g_scale + 0.5);
  const int tipH = static_cast<int>(12.0 * g_scale + 0.5);
  if (g_fontCoord == nullptr) {
    g_fontCoord = CreateFontW(-coordH, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE,
                              L"Microsoft YaHei UI");
  }
  if (g_fontTip == nullptr) {
    g_fontTip = CreateFontW(-tipH, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE,
                            L"Microsoft YaHei UI");
  }
}

// 把遮罩贴到 view 客户区上（只在位置/尺寸变化时搬，避免每帧重排闪烁）。
// 目标不可见/最小化时返回 false：最小化窗口的 ClientToScreen 会给出 -32000，
// 照着走遮罩会被甩到屏幕外（表现就是"点了拾取但什么都没有"）。
bool SyncGeometry() {
  if (g_hwnd == nullptr) {
    return false;
  }
  const HWND view = TargetWindow();
  if (view == nullptr || !IsWindowVisible(view)) {
    return false;
  }
  const HWND root = GetAncestor(view, GA_ROOT);
  if (root != nullptr && IsIconic(root) != FALSE) {
    return false;
  }
  RECT rc = {};
  if (!GetClientRect(view, &rc)) {
    return false;
  }
  POINT tl = {0, 0};
  if (!ClientToScreen(view, &tl)) {
    return false;
  }
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) {
    return false;
  }
  if (w != g_w || h != g_h || tl.x != g_x || tl.y != g_y) {
    g_w = w;
    g_h = h;
    g_x = tl.x;
    g_y = tl.y;
    SetWindowPos(g_hwnd, HWND_TOPMOST, g_x, g_y, w, h,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
  }
  return true;
}

void PaintOverlay(HWND hwnd) {
  PAINTSTRUCT ps = {};
  HDC dc = BeginPaint(hwnd, &ps);
  if (dc == nullptr) {
    return;
  }

  RECT rc = {};
  GetClientRect(hwnd, &rc);
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) {
    EndPaint(hwnd, &ps);
    return;
  }

  // 双缓冲：整幅重画，避免十字线拖动时的闪烁。
  HDC mem = CreateCompatibleDC(dc);
  HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
  HGDIOBJ oldBmp = SelectObject(mem, bmp);
  SetBkMode(mem, TRANSPARENT);

  // 底色必须是色键纯黑 -> 全透明，只留下画上去的元素。
  HBRUSH bg = CreateSolidBrush(kKeyColor);
  FillRect(mem, &rc, bg);
  DeleteObject(bg);

  const COLORREF accent = g_inside ? kColorIn : kColorOut;

  // 边框 + 十字线 + 光标方框
  HPEN pen = CreatePen(PS_SOLID, 2, accent);
  HGDIOBJ oldPen = SelectObject(mem, pen);
  HGDIOBJ oldBrush = SelectObject(mem, GetStockObject(NULL_BRUSH));
  Rectangle(mem, 1, 1, w - 1, h - 1);
  MoveToEx(mem, g_cur.x, 0, nullptr);
  LineTo(mem, g_cur.x, h);
  MoveToEx(mem, 0, g_cur.y, nullptr);
  LineTo(mem, w, g_cur.y);
  Rectangle(mem, g_cur.x - 5, g_cur.y - 5, g_cur.x + 6, g_cur.y + 6);
  SelectObject(mem, oldBrush);
  SelectObject(mem, oldPen);
  DeleteObject(pen);

  // 光标旁的坐标标签：逻辑坐标（脚本用）+ 物理客户区像素
  const double lx = Round1(static_cast<double>(g_cur.x) / g_scale);
  const double ly = Round1(static_cast<double>(g_cur.y) / g_scale);
  wchar_t label[160] = {};
  const int labelCap = static_cast<int>(sizeof(label) / sizeof(label[0]));
  if (g_inside) {
    swprintf_s(label, labelCap, L"逻辑 (%.1f, %.1f)   物理 (%d, %d)", lx, ly,
               g_cur.x, g_cur.y);
  } else {
    swprintf_s(label, labelCap,
               L"窗口外（点了不算）  逻辑 (%.1f, %.1f)   物理 (%d, %d)", lx, ly,
               g_cur.x, g_cur.y);
  }

  HGDIOBJ oldFont = SelectObject(mem, g_fontCoord);
  SIZE sz = {};
  GetTextExtentPoint32W(mem, label, static_cast<int>(wcslen(label)), &sz);

  int tx = g_cur.x + 14;
  int ty = g_cur.y + 14;
  if (tx + sz.cx + 10 > w) {
    tx = w - sz.cx - 10;
  }
  if (ty + sz.cy + 8 > h) {
    ty = h - sz.cy - 8;
  }
  if (tx < 4) {
    tx = 4;
  }
  if (ty < 4) {
    ty = 4;
  }

  HBRUSH tagBg = CreateSolidBrush(kTagBg);
  HPEN tagPen = CreatePen(PS_SOLID, 1, accent);
  HGDIOBJ oldTagBrush = SelectObject(mem, tagBg);
  HGDIOBJ oldTagPen = SelectObject(mem, tagPen);
  Rectangle(mem, tx - 5, ty - 3, tx + sz.cx + 5, ty + sz.cy + 3);
  SelectObject(mem, oldTagPen);
  SelectObject(mem, oldTagBrush);
  DeleteObject(tagPen);
  DeleteObject(tagBg);

  SetTextColor(mem, kTagText);
  TextOutW(mem, tx, ty, label, static_cast<int>(wcslen(label)));
  SelectObject(mem, oldFont);

  // 顶部提示
  const wchar_t* tip = L"左键拾取　右键取消";
  HGDIOBJ oldTipFont = SelectObject(mem, g_fontTip);
  SIZE tsz = {};
  GetTextExtentPoint32W(mem, tip, static_cast<int>(wcslen(tip)), &tsz);
  SetTextColor(mem, kTipText);
  TextOutW(mem, (w - tsz.cx) / 2, 5, tip, static_cast<int>(wcslen(tip)));
  SelectObject(mem, oldTipFont);

  BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);

  SelectObject(mem, oldBmp);
  DeleteObject(bmp);
  DeleteDC(mem);
  EndPaint(hwnd, &ps);
}

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      PaintOverlay(hwnd);
      return 0;
    case WM_NCHITTEST:
      return HTTRANSPARENT;  // 鼠标一律穿透到下面的应用
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

bool RegisterOverlayClass() {
  if (g_classRegistered) {
    return true;
  }
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &OverlayProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kClassName;
  if (RegisterClassExW(&wc) == 0) {
    LogTagged("framework", "pick overlay: RegisterClassEx failed err=%lu",
              GetLastError());
    return false;
  }
  g_classRegistered = true;
  return true;
}

}  // namespace

bool PickOverlayShow(const HostCaps* host) {
  g_host = host;
  if (!RegisterOverlayClass()) {
    return false;
  }
  if (g_hwnd == nullptr) {
    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
            WS_EX_NOACTIVATE,
        kClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (g_hwnd == nullptr) {
      LogTagged("framework", "pick overlay: CreateWindowEx failed err=%lu",
                GetLastError());
      return false;
    }
    SetLayeredWindowAttributes(g_hwnd, kKeyColor, kAlpha,
                               LWA_COLORKEY | LWA_ALPHA);
    g_w = 0;
    g_h = 0;
  }
  EnsureFonts();
  g_want = true;
  if (!SyncGeometry()) {
    // 目标窗口此刻最小化/不可见：先挂起，等 Tick 发现窗口回来再显示。
    ShowWindow(g_hwnd, SW_HIDE);
    return true;
  }
  ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
  PickOverlayTick();  // 立刻出一帧，别等下一个 33ms
  return true;
}

void PickOverlayTick() {
  if (g_hwnd == nullptr || !g_want) {
    return;
  }
  if (!SyncGeometry()) {
    if (IsWindowVisible(g_hwnd)) {
      ShowWindow(g_hwnd, SW_HIDE);
    }
    return;
  }
  if (!IsWindowVisible(g_hwnd)) {
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
  }
  // 目标窗口可能被置顶（"拾取时置顶"）：它一被激活就会排到 topmost 组最前面，
  // 把遮罩压到下面。每帧把遮罩重新提回 topmost 最前，才能一直盖住目标窗口。
  SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
  const HWND view = TargetWindow();
  POINT sp = {0, 0};
  if (view != nullptr && GetCursorPos(&sp)) {
    POINT cp = sp;
    if (ScreenToClient(view, &cp)) {
      g_cur = cp;
      g_inside = (cp.x >= 0 && cp.y >= 0 && cp.x < g_w && cp.y < g_h);
    }
  }
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

void PickOverlayHide() {
  g_want = false;
  if (g_hwnd != nullptr) {
    ShowWindow(g_hwnd, SW_HIDE);
  }
}

void PickOverlayDestroy() {
  g_want = false;
  if (g_hwnd != nullptr) {
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
  }
  if (g_fontCoord != nullptr) {
    DeleteObject(g_fontCoord);
    g_fontCoord = nullptr;
  }
  if (g_fontTip != nullptr) {
    DeleteObject(g_fontTip);
    g_fontTip = nullptr;
  }
  if (g_classRegistered) {
    UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
    g_classRegistered = false;
  }
  g_w = 0;
  g_h = 0;
  g_x = 0;
  g_y = 0;
}
