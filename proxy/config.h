// target.ini handling: where the proxy looks for its configuration, and the
// two keys it understands (target / debug).
//
// Current format (written when the file is missing):
//     [host]
//     target=MyApp.exe
//     debug=true
//
// Bare "key=value" files written by older builds (no [host] header) are still
// accepted: the Win32 profile API is tried first, then a line scanner.
//
// NOTE: keep the proxy sources pure ASCII.
#pragma once

#include <windows.h>

#include <filesystem>

// Directory holding our version.dll; empty when it cannot be resolved.
const std::filesystem::path& ModuleDir();

// <ModuleDir>\target.ini; empty when ModuleDir is empty.
const std::filesystem::path& TargetIniPath();

// Host exe base name from target= (wide, cached). L"" when nothing usable is
// configured, i.e. the caller must not attach.
const wchar_t* TargetExeName();

// debug= (default true): pop a dialog once the proxy has attached.
bool DebugEnabled();
