// version.ini handling: where the proxy looks for its configuration, and the
// keys it understands.
//
// Current format (written when the file is missing):
//     [host]
//     target=MyApp.exe
//     debug=true
//
//     [loaders]
//     framework.dll
//     other.dll
//
// [host] keys keep the old semantics: target= says which host process this
// version.dll attaches to, debug= pops a dialog once attached.
// [loaders] entries are plain DLL file names loaded from ModuleDir(), one per
// line; an entry that is not listed is not loaded. Names are validated on read
// (no path separators, no "..", must end in .dll), de-duplicated and capped.
//
// Bare "key=value" files written by older builds (no [host] header) are still
// accepted for [host] keys: the Win32 profile API is tried first, then a line
// scanner.
//
// NOTE: keep the proxy sources pure ASCII.
#pragma once

#include <windows.h>

#include <filesystem>
#include <vector>

// Directory holding our version.dll; empty when it cannot be resolved.
const std::filesystem::path& ModuleDir();

// <ModuleDir>\version.ini; empty when ModuleDir is empty.
const std::filesystem::path& VersionIniPath();

// Host exe base name from [host] target= (wide, cached). L"" when nothing
// usable is configured, i.e. the caller must not attach.
const wchar_t* TargetExeName();

// [host] debug= (default true): pop a dialog once the proxy has attached.
bool DebugEnabled();

// [loaders] dlls= entries, already validated. Plain names only: resolve each
// against ModuleDir() before loading.
const std::vector<std::wstring>& LoadDllNames();