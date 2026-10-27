// Single-line log shared by the whole proxy TU set.
//
// Every line goes to proxy.log (next to version.dll, %TEMP% as fallback) and,
// once the framework is up, is mirrored to the plugin log sinks. The fan-out
// variable stays a global because plugin_api.inc writes it directly.
//
// NOTE: keep the proxy sources pure ASCII.
#pragma once

// Installed by the framework after plugin scan; nullptr before that.
extern void (*g_logFanout)(const char* line);

// printf-style; appends "[HH:MM:SS.mmm]-[proxy] " then mirrors to g_logFanout.
// LogTagged uses the given component (e.g. "framework") instead of "proxy".
void Log(const char* fmt, ...);
void LogTagged(const char* component, const char* fmt, ...);
