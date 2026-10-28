// version.ini handling: where the proxy looks for its configuration, and the
// keys it understands.
//
// Current format (written when the file is missing):
//     [host]
//     target=MyApp.exe
//     debug=true
//
//     [modules]
//     console.dll
//
// [host] keys keep the old semantics: target= says which host process this
// version.dll attaches to, debug= pops a dialog once attached.
// [modules] lists *extra* JSON modules (plain dll names in ModuleDir()). The
// same contract as framework.dll: export NotifyPlatform, answer {"cmd":"init"}
// / {"cmd":"shutdown"}. framework.dll itself is NOT configured here: it is
// always loaded first and watched for hot reload (see module_loader.h).
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
const int kConfigDirChars     = 512;  // ModuleDir / VersionIniPath buffer
const int kMaxModuleNames     = 8;    // [modules] entries kept
const int kMaxModuleNameChars = 64;   // one [modules] dll name

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

// [modules] plain dll names, already validated and de-duplicated. Writes up to
// cap entries (each a fixed kMaxModuleNameChars buffer) and returns the count.
// Resolve each name against ModuleDir() before loading.
int ExtraModuleNames(wchar_t out[][kMaxModuleNameChars], int cap);