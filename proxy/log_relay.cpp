// log_relay.cpp - 见 log_relay.h。
#include "log_relay.h"

#include <windows.h>

#include "log.h"

namespace {

// sink 在任意线程被读、在 proxy 线程被写，用原子指针换。
log_relay::SinkFn g_sink = nullptr;

// 防递归：sink 里再写日志（publishLog）时不再镜像，否则无限回环。
thread_local bool t_inRelay = false;

void __cdecl Bridge(const char* line) {
  if (line == nullptr || t_inRelay) {
    return;
  }
  log_relay::SinkFn sink = g_sink;
  if (sink == nullptr) {
    return;
  }
  t_inRelay = true;
  sink(line, nullptr);
  t_inRelay = false;
}

}  // namespace

namespace log_relay {

void Start() {
  g_logFanout = &Bridge;
}

bool SetSink(void* fn) {
  InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&g_sink), fn);
  return fn != nullptr;
}

void ClearSink() {
  SetSink(nullptr);
}

}  // namespace log_relay
