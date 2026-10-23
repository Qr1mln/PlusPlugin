// ============================================================================
// empty_plugin.cpp - minimal plugin used to verify the M0 load/unload path.
//
// It registers nothing, draws nothing and only writes two log lines, so a
// failure here is a framework problem, never a plugin problem.
// Keep this file pure ASCII.
// ============================================================================

#include "../sdk/ttplugin.h"

static const TTPluginHost* g_host = nullptr;

// @brief Records the capability table and reports itself ready.
// @param host Framework table, valid until shutdown().
// @return TTPLUGIN_OK to accept the load.
static int __cdecl OnPluginInit(const TTPluginHost* host) {
    g_host = host;
    if (host == nullptr || host->abi != TTPLUGIN_ABI) {
        return TTPLUGIN_ERR_PARAM;
    }
    host->log("empty: init");
    return TTPLUGIN_OK;
}

// @brief Periodic callback. M0 does nothing here on purpose.
// @return 0, always.
static int __cdecl OnPluginTick(void) {
    return 0;
}

// @brief Single cleanup point. DllMain must stay empty.
// @return TTPLUGIN_OK so the framework may FreeLibrary us.
static int __cdecl OnPluginShutdown(void) {
    if (g_host != nullptr && g_host->log != nullptr) {
        g_host->log("empty: shutdown");
    }
    g_host = nullptr;
    return TTPLUGIN_OK;
}

static const TTPluginDesc kDesc = {
    TTPLUGIN_ABI,
    static_cast<uint32_t>(sizeof(TTPluginDesc)),
    "empty",
    "0.1.0",
    &OnPluginInit,
    &OnPluginTick,
    &OnPluginShutdown
};

// @brief The only export the framework looks up.
// @return Immutable descriptor owned by this module.
extern "C" __declspec(dllexport) const TTPluginDesc* __cdecl TTPlugin_GetDesc(void) {
    return &kDesc;
}

// The framework owns every hook and thread. An empty DllMain is the correct
// implementation, not an omission.
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    (void)module;
    (void)reason;
    return TRUE;
}
