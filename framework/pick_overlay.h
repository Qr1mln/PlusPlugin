// ============================================================================
// pick_overlay.h - 拾取遮罩：盖在目标 view 上的置顶透明窗口，画十字线 + 实时坐标。
// （原 Python 版 editor/picker.py 里 PickOverlay 的 C++ 实现）
//
// 为什么在 DLL：遮罩要盖在别的进程窗口上面，web 页面做不到；而取点的低级鼠标
// 钩子本来就在 framework 的推送线程里，窗口和钩子放同一条线程最省事。
//
// 外观对齐 Python 版：纯黑当 color key（全透明），所以底下的界面照常可见；
// 只剩窗口边框、十字线、光标方框、坐标标签、顶部提示，整窗 92% 不透明。
//
// 线程约束：下面 4 个函数都只能在 picker 推送线程（装钩子的那条）调用。
// ============================================================================
#ifndef FRAMEWORK_PICK_OVERLAY_H
#define FRAMEWORK_PICK_OVERLAY_H

struct HostCaps;

// 创建（首次）并显示遮罩，跟随 view 客户区。失败返回 false（不影响拾取本身）。
bool PickOverlayShow(const HostCaps* host);

// 跟随窗口位置/尺寸 + 采样光标位置并重绘（由 picker 的 33ms 定时器驱动）。
void PickOverlayTick();

// 隐藏遮罩（拾取结束/取消），窗口留着下次复用。
void PickOverlayHide();

// 销毁窗口、字体、窗口类。线程退出或 DLL 卸载前必须调用，否则回调指针悬空。
void PickOverlayDestroy();

#endif  // FRAMEWORK_PICK_OVERLAY_H
