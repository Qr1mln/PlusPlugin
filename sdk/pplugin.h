/* ============================================================================
 * pplugin.h - pplugin 插件 P_ABI（P_ABI v4）
 *
 * 框架（version.dll）与插件（独立 DLL）之间的唯一契约头文件。
 * 双方各自编译、运行时由框架 LoadLibraryEx 加载插件，仅通过本文件对接。
 *
 * 契约规则（详见 docs/plugin-framework.md）：
 *   1. 仅 POD，C 兼容：禁止 C++ 类型 / STL / 异常。
 *   2. 字段默认只能追加，不能重排，不能改语义；移除能力属破坏性变更，须 bump P_ABI 并允许重排。
 *   3. 插件不得自行装 hook、建线程、碰系统注册点；一切走 Host。
 *   4. 只有破坏性变更才 bump P_ABI；bump 之后旧插件一律拒绝加载，必须重编。
 *   5. 注释用中文（非纯 ASCII），由编译单元 /utf-8 支持；正文仍为 C/POD。
 * ==========================================================================*/

#ifndef PPLUGIN_H
#define PPLUGIN_H

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 最低兼容 P_ABI 版本：插件填所需版本，框架要求与此值完全相等。 */
#define P_ABI 4u

/* 返回码：0=成功，负数=失败。 */
#define P_OK          0
#define ERR_FULL   -1   /* 容量已满 */
#define ERR_STATE  -2   /* 生命周期外调用 */
#define ERR_PARAM  -3   /* 空指针 / 非法值 */

/* 插件 DLL 必须导出的唯一符号（C 链接，见 GetDesc）。 */
#define ENTRY_NAME "GetDesc"

/* 延迟任务回调：经 Host::submitTask 由框架线程执行（卸载时可等排空）。 */
typedef void (__cdecl *TaskFn)(void* user);

/* 日志 sink：每行 UTF-8 单行无换行；任意框架线程，内部不得再调 Host::publishLog。 */
typedef void (__cdecl *LogFn)(const char* line, void* user);

/* ---------------------------------------------------------------------------
 * Host - init() 时交给插件的能力表（框架实现，插件只读，shutdown() 后不得缓存）。
 * 所有函数指针均为 __cdecl。同一 P_ABI 版本内字段偏移稳定，新字段只追加。
 * P_ABI v2 起移除 overlay 相关字段（addDraw / addRegion / addPointerSink / query），
 * 属破坏性变更，旧插件须用新版 pplugin.h 重编。
 * P_ABI v3 起移除 addHotkey（热键派发已下线），其后字段整体前移，同样属破坏性变更：
 * 旧插件（abi != 3）会被框架直接拒绝，必须重编。
 * P_ABI v4 起移除 pluginDir（插件可用 GetModuleFileNameW 自行定位），字段再次前移。
 * -------------------------------------------------------------------------*/
typedef struct Host {
    uint32_t abi;        /* 先比对 abi == P_ABI，再比对 structSize >= sizeof(Host) 才用追加字段 */
    uint32_t structSize;

    void        (__cdecl *publishLog)(const char* text);       /* 中心日志（UTF-8 单行） */
    HWND        (__cdecl *mainWindow)(void);                   /* 宿主顶层窗口，UI 就绪前可能 null */
    HWND        (__cdecl *viewWindow)(void);                   /* 接收输入的子窗口，可能 null */
    float       (__cdecl *scale)(void);                        /* DPI 缩放（96dpi=1.0） */

    int         (__cdecl *submitTask)(TaskFn fn, void* user);                  /* 队列满返回 ERR_FULL */

    int         (__cdecl *subscribeLog)(LogFn fn, void* user);  /* 订阅日志流；每插件一个 */
} Host;

/* ---------------------------------------------------------------------------
 * Desc - 插件经 GetDesc() 返回给框架的清单（插件侧填充）。
 * -------------------------------------------------------------------------*/
typedef struct Desc {
    uint32_t abi;          /* 所需 P_ABI，必须填 P_ABI；与框架不等即被拒 */
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
