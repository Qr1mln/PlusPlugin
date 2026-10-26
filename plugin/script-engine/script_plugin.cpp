// ============================================================================
// script_plugin.cpp - script_editor.py 使用的脚本事件执行库
//
// 同时作为 pplugin 插件存在，但只保留最小插件入口：GetDesc + 空的
// init/tick/shutdown（只校验 abi，不注册任何 Host 能力）。这样框架能按
// plugin.json 的 enabled 正常加载/卸载它，不再报 "has no GetDesc"。
//
// 真正的事件执行仍由 script_editor.py 用 ctypes 直接调用 PP_* 导出完成。
//
// 导出（C 链接，__cdecl）：
//   int  PP_Mouse(HWND view, int x, int y, int button, int action)
//   int  PP_Delay(int ms)
//   int  PP_FindImage(HWND view, const char* image, double threshold, int timeout_ms)
//   int  PP_Version(void)
//   const Desc* GetDesc(void)          // 插件入口（框架用）
//
//   button : 0=左键 1=右键 2=中键
//   action : 0=移动 1=按下 2=抬起 3=单击（按下+抬起）
//   x, y   : 相对 view 客户区的物理像素
//
// 找图暂时是占位实现：返回 PP_ERR_NOT_IMPLEMENTED (-4)，编辑器记日志后跳过。
// ============================================================================

#include <windows.h>

#include "../../sdk/pplugin.h"

#define PP_OK                  0
#define PP_ERR_PARAM          -1
#define PP_ERR_STATE          -2
#define PP_ERR_NOT_IMPLEMENTED -4

namespace {

// 按键编号 -> 鼠标按下消息
UINT DownMessage(int button) {
    if (button == 1) return WM_RBUTTONDOWN;
    if (button == 2) return WM_MBUTTONDOWN;
    return WM_LBUTTONDOWN;
}

// 按键编号 -> 鼠标抬起消息
UINT UpMessage(int button) {
    if (button == 1) return WM_RBUTTONUP;
    if (button == 2) return WM_MBUTTONUP;
    return WM_LBUTTONUP;
}

// 按键编号 -> 鼠标移动时携带的按键标志
WPARAM DownFlag(int button) {
    if (button == 1) return MK_RBUTTON;
    if (button == 2) return MK_MBUTTON;
    return MK_LBUTTON;
}

}  // namespace

// 鼠标事件：往目标窗口投后台鼠标消息（不置前、不动光标）。
extern "C" __declspec(dllexport) int __cdecl PP_Mouse(HWND view, int x, int y,
                                                       int button, int action) {
    if (view == nullptr || !IsWindow(view)) return PP_ERR_PARAM;
    if (button < 0 || button > 2) return PP_ERR_PARAM;
    if (action < 0 || action > 3) return PP_ERR_PARAM;

    const LPARAM point = MAKELPARAM(x, y);
    const WPARAM flag = DownFlag(button);

    switch (action) {
    case 0:  // 移动
        PostMessageW(view, WM_MOUSEMOVE, 0, point);
        break;
    case 1:  // 按下：先移到位再按下
        PostMessageW(view, WM_MOUSEMOVE, flag, point);
        PostMessageW(view, DownMessage(button), flag, point);
        break;
    case 2:  // 抬起
        PostMessageW(view, UpMessage(button), 0, point);
        break;
    case 3:  // 单击：移动 -> 按下 -> 抬起
        PostMessageW(view, WM_MOUSEMOVE, flag, point);
        PostMessageW(view, DownMessage(button), flag, point);
        PostMessageW(view, UpMessage(button), 0, point);
        break;
    default:
        return PP_ERR_PARAM;
    }
    return PP_OK;
}

// 延迟事件：阻塞等待指定毫秒。
extern "C" __declspec(dllexport) int __cdecl PP_Delay(int ms) {
    if (ms < 0) return PP_ERR_PARAM;
    if (ms > 0) Sleep(static_cast<DWORD>(ms));
    return PP_OK;
}

// 找图事件：占位，匹配算法尚未实现。
extern "C" __declspec(dllexport) int __cdecl PP_FindImage(HWND view, const char* image,
                                                          double threshold, int timeout_ms) {
    (void)view;
    (void)image;
    (void)threshold;
    (void)timeout_ms;
    return PP_ERR_NOT_IMPLEMENTED;  // 占位：匹配算法实现前一律返回未实现
}

// 库版本：供编辑器确认导出集合匹配。
extern "C" __declspec(dllexport) int __cdecl PP_Version(void) {
    return 2;
}

// ---------------------------------------------------------------------------
// 最小插件入口：只让框架认可本 DLL，不预订任何能力。
// 卸载时框架先 drain inflight 再调 shutdown，这里没有任何资源要放。
// ---------------------------------------------------------------------------
static int __cdecl OnPluginInit(const Host* host) {
    if (host == nullptr || host->abi != P_ABI) return ERR_PARAM;
    return P_OK;
}

static int __cdecl OnPluginTick(void) {
    return 0;
}

static int __cdecl OnPluginShutdown(void) {
    return P_OK;
}

static const Desc kDesc = {
    P_ABI,
    sizeof(Desc),
    "script-engine",
    "0.1.0",
    &OnPluginInit,
    &OnPluginTick,
    &OnPluginShutdown
};

extern "C" __declspec(dllexport) const Desc* __cdecl GetDesc(void) {
    return &kDesc;
}

// 纯执行库：DllMain 保持空实现，不在这里做任何工作。
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    (void)module;
    (void)reason;
    (void)reserved;
    return TRUE;
}
