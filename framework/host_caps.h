// ============================================================================
// host_caps.h - framework 侧唯一需要的宿主机能力：三根函数指针。
//
// framework 与 proxy 之间已经不走 C 结构共享，只有 JSON（NotifyPlatform /
// dispatch {"cmd":"mainWindow"}）。这里的三个指针由 framework 自己填充，函数体
// 就是那几条 JSON 调用，属于内部中转表，与插件机制无关。
// ============================================================================
#ifndef FRAMEWORK_HOST_CAPS_H
#define FRAMEWORK_HOST_CAPS_H

#include <windows.h>

struct HostCaps {
  HWND (__cdecl* mainWindow)();   // 宿主顶层窗口（FLUTTER_RUNNER_WIN32_WINDOW）
  HWND (__cdecl* viewWindow)();   // 真正接收输入的子窗口（FLUTTERVIEW）
  float (__cdecl* scale)();       // DPI 缩放（96dpi = 1.0）
  // 让宿重新解析窗口（应用重建窗口后清缓存）。旧宿主可能没有这一根 -> nullptr。
  bool (__cdecl* refreshWindow)();
};

#endif  // FRAMEWORK_HOST_CAPS_H
