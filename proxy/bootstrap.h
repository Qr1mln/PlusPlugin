// Bootstrapping hooks between DllMain and the worker thread.
//
// NOTE: keep the proxy sources pure ASCII.
#pragma once

#include <windows.h>

// Worker thread entry spawned from DllMain. Confirms the host EXE, waits for
// the graphics stack and the Flutter window, then hands the HWND to the
// framework which loads plugins. Never runs under the loader lock.
DWORD WINAPI PayloadThread(LPVOID param);

// number of version.dll exports that failed to resolve (set in DllMain).
int ResolveMissCount();

// Host window lookups, shared with protocol.cpp's "refreshWindow" command so the
// cached HWNDs can be re-resolved when the app rebuilds its window.
HWND FindHostMainWindow();
HWND FindHostViewWindow(HWND main);
float HostDpiScale(HWND hwnd);
