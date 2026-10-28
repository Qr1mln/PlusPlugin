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
// No C++ standard library: paths are fixed wide buffers, the loader name list
// is a fixed 2-D array.
//
// Keep this file pure ASCII.
#pragma once

#include <windows.h>

// Fixed sizes. Paths cover <ModuleDir>\version.ini and the module dir itself.
const int kConfigDirChars   = 512;   // ModuleDir / VersionIniPath buffer
const int kMaxLoadDlls      = 8;     // [loaders] entries kept
const int kMaxDllNameChars  = 64;    // one [loaders] dll name

// Directory holding our version.dll (no trailing backslash); L"" when it
// cannot be resolved. Points at a cached static buffer.
const wchar_t* ModuleDir();

// <ModuleDir>\version.ini; L"" when ModuleDir is empty. Cached static buffer.
const wchar_t* VersionIniPath();

// Host exe base name from [host] target= (wide, cached). L"" when nothing
// usable is configured, i.e. the caller must not attach.
const wchar_t* TargetExeName();

// [host] debug= (default true): pop a dialog once the proxy has attached.
bool DebugEnabled();

// [loaders] plain dll names, already validated and de-duplicated. Writes up to
// cap entries (each a fixed kMaxDllNameChars buffer) and returns the count.
// Resolve each name against ModuleDir() before loading.
int LoadDllNames(wchar_t out[][kMaxDllNameChars], int cap);