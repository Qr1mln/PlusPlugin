// Bootstrapping hooks between DllMain and the worker thread.
//
// NOTE: keep the proxy sources pure ASCII.
#pragma once

#include <windows.h>

// Arms the WebView2 DevTools port through its environment variable. Must run
// in DllMain: the switch is read when the host creates its FIRST WebView2
// environment, so nothing later is early enough.
void EnableWebView2Cdp();

// Worker thread entry spawned from DllMain. Confirms the host EXE, waits for
// the graphics stack and the Flutter window, then hands the HWND to the
// framework which loads plugins. Never runs under the loader lock.
DWORD WINAPI PayloadThread(LPVOID param);

// number of version.dll exports that failed to resolve (set in DllMain).
int ResolveMissCount();
