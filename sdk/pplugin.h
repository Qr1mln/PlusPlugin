/* ============================================================================
 * pplugin.h - pplugin 插件 P_ABI（P_ABI v2）
 *
 * 框架（version.dll）与插件（独立 DLL）之间的唯一契约头文件。
 * 双方各自编译、运行时由框架 LoadLibraryEx 加载插件，仅通过本文件对接。
 *
 * 契约规则（详见 docs/plugin-framework.md）：
 *   1. 仅 POD，C 兼容：禁止 C++ 类型 / STL / 异常。
 *   2. 字段只能追加，不能重排，不能改语义。
 *   3. 插件不得自行装 hook、建线程、碰系统注册点；一切走 Host。
 *   4. 被删字段保留为 reserved 指针；只有破坏性变更才 bump P_ABI。
 *   5. 注释用中文（非纯 ASCII），由编译单元 /utf-8 支持；正文仍为 C/POD。
 * ==========================================================================*/

#ifndef PPLUGIN_H
#define PPLUGIN_H

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 最低兼容 P_ABI 版本：插件填所需最低版本，框架拒绝高于此值者。 */
#define P_ABI 2u

/* 每插件资源上限，超出后对应 add* 返回 ERR_FULL。 */
#define MAX_HOTKEYS 8

/* 返回码：0=成功，负数=失败。 */
#define P_OK          0
#define ERR_FULL   -1   /* 容量已满 */
#define ERR_STATE  -2   /* 生命周期外调用 */
#define ERR_PARAM  -3   /* 空指针 / 非法值 */

/* 插件 DLL 必须导出的唯一符号（C 链接，见 GetDesc）。 */
#define ENTRY_NAME "GetDesc"

/* 全局热键回调：vk 为触发虚拟码，注册/注销由框架拥有。 */
typedef void (__cdecl *HotkeyFn)(int vk, void* user);

/* 延迟任务回调：经 Host::submitTask 由框架线程执行（卸载时可等排空）。 */
typedef void (__cdecl *TaskFn)(void* user);

/* 日志 sink：每行 UTF-8 单行无换行；任意框架线程，内部不得再调 Host::log。 */
typedef void (__cdecl *LogFn)(const char* line, void* user);

/* ---------------------------------------------------------------------------
 * Host - init() 时交给插件的能力表（框架实现，插件只读，shutdown() 后不得缓存）。
 * 所有函数指针均为 __cdecl。同一 P_ABI 版本内字段偏移稳定，新字段只追加。
 * P_ABI v2 起移除 overlay 相关字段（addDraw / addRegion / addPointerSink / query），
 * 属破坏性变更，旧插件须用新版 pplugin.h 重编。
 * -------------------------------------------------------------------------*/
typedef struct Host {
    uint32_t abi;        /* 先比对 abi == P_ABI，再比对 structSize >= sizeof(Host) 才用追加字段 */
    uint32_t structSize;

    void        (__cdecl *log)(const char* text);              /* 中心日志（UTF-8 单行） */
    const char* (__cdecl *pluginDir)(void);                    /* 插件目录绝对路径（shutdown 前有效） */
    HWND        (__cdecl *mainWindow)(void);                   /* 宿主顶层窗口，UI 就绪前可能 null */
    HWND        (__cdecl *viewWindow)(void);                   /* 接收输入的子窗口，可能 null */
    float       (__cdecl *scale)(void);                        /* DPI 缩放（96dpi=1.0） */

    int         (__cdecl *addHotkey)(int vk, HotkeyFn fn, void* user);         /* vk 为 VK_* */
    int         (__cdecl *submitTask)(TaskFn fn, void* user);                  /* 队列满返回 ERR_FULL */

    int         (__cdecl *addLogSink)(LogFn fn, void* user);    /* 订阅日志流；每插件一个 */
} Host;

/* ---------------------------------------------------------------------------
 * Desc - 插件经 GetDesc() 返回给框架的清单（插件侧填充）。
 * -------------------------------------------------------------------------*/
typedef struct Desc {
    uint32_t abi;          /* 所需最低 P_ABI，通常填 P_ABI；高于框架值被拒 */
    uint32_t structSize;   /* 插件侧 sizeof(Desc) */

    const char* name;      /* ASCII 稳定 id，≤63 字符 */
    const char* version;   /* ASCII 版本文本，≤31 字符 */

    int  (__cdecl *init)(const Host* host);     /* 加载时调一次，返回 P_OK 才接受，否则立即卸载 */
    int  (__cdecl *tick)(void);                /* 可选；框架线程周期调用，须快返回 */
    int  (__cdecl *shutdown)(void);            /* FreeLibrary 前调一次：释放图形/COM、join 线程（勿在 DllMain 做） */
} Desc;

#ifdef __cplusplus
}
#endif

#endif /* PPLUGIN_H */
