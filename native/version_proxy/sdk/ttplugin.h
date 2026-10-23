/* ============================================================================
 * ttplugin.h - TTVoice plugin ABI (TTPLUGIN_ABI v1)
 *
 * @file    ttplugin.h
 * @brief   Contract between the framework (version.dll) and plugins.
 * @author  TTVoice native team
 * @date    2026-10-23
 *
 * Contract rules (review enforced, see docs/plugin-framework.md):
 *   1. POD only, C compatible: no C++ types, no STL, no exceptions.
 *   2. Fields are append-only. Never reorder, never change semantics.
 *   3. A plugin must not install hooks, create threads or touch system wide
 *      registration points on its own. Everything goes through TTPluginHost
 *      so the framework can revoke it on unload.
 *   4. A removed field stays as a reserved pointer; abi is bumped only for a
 *      breaking change.
 *   5. Keep this file pure ASCII.
 * ==========================================================================*/

#ifndef TTVOICE_TTPLUGIN_H
#define TTVOICE_TTPLUGIN_H

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ABI version understood by this framework build. A plugin declares the
 * minimum it needs; the framework rejects anything above its own value. */
#define TTPLUGIN_ABI 1u

/* Capacity per plugin. Exceeding it makes the registration call fail. */
#define TTPLUGIN_MAX_DRAWS   8
#define TTPLUGIN_MAX_REGIONS 16
#define TTPLUGIN_MAX_HOTKEYS 8

/* Registration / init result codes (TTPluginHost::add* and TTPluginDesc::init). */
#define TTPLUGIN_OK          0
#define TTPLUGIN_ERR_FULL   -1   /* capacity exhausted */
#define TTPLUGIN_ERR_STATE  -2   /* plugin not in a callable state */
#define TTPLUGIN_ERR_PARAM  -3   /* null pointer or invalid value */

/* The single export every plugin DLL must provide. */
#define TTPLUGIN_ENTRY_NAME "TTPlugin_GetDesc"

/* Draw callback. d2dCtx is an ID2D1DeviceContext*, passed as void* so this
 * header stays free of Direct2D. Coordinates are logical pixels (96 dpi);
 * the framework applies the window scale. Runs on the render thread. */
typedef void (__cdecl *TTPluginDrawFn)(void* d2dCtx, float width, float height, void* user);

/* Click callback. Only the plugin owning the hit region receives it.
 * kind: 0 = left, 1 = double, 2 = right. Runs on the window thread. */
typedef void (__cdecl *TTPluginClickFn)(int kind, float x, float y, void* user);

/* Hotkey callback. The framework owns RegisterHotKey/UnregisterHotKey. */
typedef void (__cdecl *TTPluginHotkeyFn)(int vk, void* user);

/* Deferred task. The framework owns the thread, so an unload can wait for
 * the queue to drain instead of racing a live thread of the plugin. */
typedef void (__cdecl *TTPluginTaskFn)(void* user);

/* Log sink. line is UTF-8, one line, no trailing newline. Runs on an
 * arbitrary framework thread and must not call TTPluginHost::log. */
typedef void (__cdecl *TTPluginLogFn)(const char* line, void* user);

/* ---------------------------------------------------------------------------
 * TTPluginHost - the capability table handed to a plugin in init().
 *
 * Every entry is implemented by the framework. Plugins must treat the table
 * as read only and must not cache it past shutdown().
 * -------------------------------------------------------------------------*/
typedef struct TTPluginHost {
    /* Identity of this table. Check abi == TTPLUGIN_ABI and structSize against
     * your own sizeof: a field added later enlarges structSize but keeps
     * every older offset valid. */
    uint32_t abi;
    uint32_t structSize;

    /* Central log. Text is UTF-8, one line, no trailing newline needed.
     * Never null. */
    void        (__cdecl *log)(const char* text);

    /* Absolute UTF-8 path of the plugin directory. Valid until shutdown().
     * Never null. Use it to locate your own data files; the framework does
     * not know what is inside. */
    const char* (__cdecl *pluginDir)(void);

    /* Windows. mainWindow is the TTVoice top level window, viewWindow is the
     * child that receives input. Both may be null before the UI is ready. */
    HWND        (__cdecl *mainWindow)(void);
    HWND        (__cdecl *viewWindow)(void);

    /* DPI scale of the window (1.0 at 96 dpi). */
    float       (__cdecl *scale)(void);

    /* Register a draw callback. Lower priority draws first. */
    int         (__cdecl *addDraw)(TTPluginDrawFn fn, int priority, void* user);

    /* Register a clickable rectangle in logical pixels. Higher priority wins
     * the hit test. */
    int         (__cdecl *addRegion)(const RECT* logicalRect, int priority,
                                     TTPluginClickFn fn, void* user);

    /* Register a global hotkey. vk is a VK_* code. */
    int         (__cdecl *addHotkey)(int vk, TTPluginHotkeyFn fn, void* user);

    /* Queue work on the framework thread. Returns TTPLUGIN_ERR_FULL when the
     * per plugin queue is full. */
    int         (__cdecl *submitTask)(TTPluginTaskFn fn, void* user);

    /* Capability negotiation. Returns a pointer to a capability specific
     * table, or null when the framework does not provide it. Reserved: no
     * capability is defined yet. */
    void*       (__cdecl *query)(const char* capability);

    /* Subscribe to the framework log stream: every line the framework writes
     * is delivered to fn. Appended after query, so check
     * host->structSize >= sizeof(TTPluginHost) before using it against an
     * older framework build.
     *
     * One sink per plugin; a second call returns TTPLUGIN_ERR_FULL. The
     * framework revokes the sink before shutdown(), so fn never runs after
     * shutdown() starts.
     *
     * Threading: fn may run on any framework thread, including in the middle
     * of another plugin's call. A UI plugin must marshal the line to its own
     * window thread. fn must not call TTPluginHost::log. */
    int         (__cdecl *addLogSink)(TTPluginLogFn fn, void* user);
} TTPluginHost;

/* ---------------------------------------------------------------------------
 * TTPluginDesc - the plugin manifest returned by TTPlugin_GetDesc().
 * -------------------------------------------------------------------------*/
typedef struct TTPluginDesc {
    uint32_t abi;          /* minimum framework abi, usually TTPLUGIN_ABI */
    uint32_t structSize;   /* sizeof(TTPluginDesc) as compiled by the plugin */

    const char* name;      /* ASCII, stable id, max 63 chars */
    const char* version;   /* ASCII, free form, max 31 chars */

    /* Called once on load. Return TTPLUGIN_OK to accept the load; any other
     * value makes the framework unload the DLL again. */
    int  (__cdecl *init)(const TTPluginHost* host);

    /* Optional, may be null. Called periodically on the framework thread and
     * must return quickly: a blocking tick delays every other plugin. */
    int  (__cdecl *tick)(void);

    /* Called exactly once before FreeLibrary. Release every graphic / COM
     * resource and join every thread here, not in DllMain. Returning an
     * error makes the framework quarantine the plugin (kept mapped, never
     * called again) instead of unloading it. */
    int  (__cdecl *shutdown)(void);
} TTPluginDesc;

#ifdef __cplusplus
}
#endif

#endif /* TTVOICE_TTPLUGIN_H */
