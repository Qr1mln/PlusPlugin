// plugin.h - the only pplugin-framework entry points the rest of the proxy uses.
//
// The implementation lives in plugin.cpp (merged from the former plugin_api.inc
// + plugin_loader.inc). Everything else in that file stays internal.
//
// Keep this file pure ASCII.
#pragma once

#include <windows.h>

namespace plugins {

// Hand the framework the host main window once it exists; also derives the DPI
// scale. Call from the payload worker thread, never from DllMain.
void SetHostContext(HWND mainHwnd);

// Scan version.ini [loaders], load the plugins and start the loader thread.
void Start();

}  // namespace plugins