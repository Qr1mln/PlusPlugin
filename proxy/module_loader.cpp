// module_loader.cpp - 见 module_loader.h。
//
// 热加载的事实依据：部署脚本在 dll 被映射时无法覆盖原文件，只能「改名换位」——
// 把正在用的 xxx.dll 改名为 xxx.dll.old，再把新文件放到 xxx.dll。因此监视文件
// **标识**（卷序列号 + 文件索引 + 大小）就能发现替换；旧文件占着 .old 这个名字，
// 等我们 FreeLibrary 之后才能删掉。
#include "module_loader.h"

#include <cstdio>
#include <cwchar>

#include "config.h"
#include "log.h"
#include "log_relay.h"

namespace {

const wchar_t* kCoreModule = L"framework.dll";
const wchar_t* kOldSuffix = L".old";

const DWORD kPollMs = 700;       // 轮询文件标识的间隔
const DWORD kSettleMs = 250;     // 连续两次标识一致才认为“写完了”
const DWORD kStopSliceMs = 100;  // 停止标志的切片检查粒度
const int kLoadRetries = 10;     // 换文件后重试加载的次数（文件可能仍在写）
const DWORD kLoadRetryMs = 300;

const int kMaxModules = 1 + kMaxModuleNames;  // 核心 + [modules]

// 文件标识：卷序列号 + 文件索引 + 大小，三个一起才能识别“同名不同文件”。
struct FileSig {
  DWORD volume;
  DWORD indexHigh;
  DWORD indexLow;
  DWORD sizeHigh;
  DWORD sizeLow;
  bool valid;
};

struct Module {
  wchar_t name[kMaxModuleNameChars];  // 显示名 / [modules] 里的裸名
  wchar_t path[MAX_PATH];             // 绝对路径
  HMODULE handle;                     // 已加载时非空
  bool core;                          // framework.dll：必需 + 走转发通道
  FileSig sig;                        // 磁盘上那份文件的标识（未加载时也记）
};

Module g_modules[kMaxModules] = {};
int g_moduleCount = 0;
protocol::HostContext g_host = {};
HANDLE g_thread = nullptr;
volatile bool g_stop = false;

bool SameSig(const FileSig& a, const FileSig& b) {
  return a.valid && b.valid && a.volume == b.volume &&
         a.indexHigh == b.indexHigh && a.indexLow == b.indexLow &&
         a.sizeHigh == b.sizeHigh && a.sizeLow == b.sizeLow;
}

bool ReadSig(const wchar_t* path, FileSig& out) {
  out = FileSig{0, 0, 0, 0, 0, false};
  HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return false;
  }
  BY_HANDLE_FILE_INFORMATION info = {};
  const BOOL ok = GetFileInformationByHandle(h, &info);
  CloseHandle(h);
  if (!ok) {
    return false;
  }
  out.volume = info.dwVolumeSerialNumber;
  out.indexHigh = info.nFileIndexHigh;
  out.indexLow = info.nFileIndexLow;
  out.sizeHigh = info.nFileSizeHigh;
  out.sizeLow = info.nFileSizeLow;
  out.valid = true;
  return true;
}

// 把 <ModuleDir>\<name> 填进 slot；目录不可用返回 false。
bool AddModule(const wchar_t* name, bool core) {
  if (g_moduleCount >= kMaxModules) {
    return false;
  }
  const wchar_t* dir = ModuleDir();
  if (dir[0] == L'\0') {
    return false;
  }
  Module& m = g_modules[g_moduleCount];
  wcsncpy_s(m.name, kMaxModuleNameChars, name, _TRUNCATE);
  swprintf_s(m.path, MAX_PATH, L"%s\\%s", dir, name);
  m.handle = nullptr;
  m.core = core;
  m.sig = FileSig{0, 0, 0, 0, 0, false};
  ++g_moduleCount;
  return true;
}

// 清掉部署脚本留下的 <dll>.old；仍被映射时删不掉，忽略即可。
void DeleteStaleOld(const Module& m) {
  wchar_t old[MAX_PATH] = {};
  swprintf_s(old, MAX_PATH, L"%s%s", m.path, kOldSuffix);
  if (DeleteFileW(old) == FALSE) {
    const DWORD err = GetLastError();
    if (err != ERROR_FILE_NOT_FOUND) {
      Log("module: delete %ls failed err=%lu (restart to clean up)", old, err);
    }
  }
}

bool LoadModule(int index) {
  Module& m = g_modules[index];
  HMODULE handle = LoadLibraryW(m.path);
  if (handle == nullptr) {
    Log("module: LoadLibrary %ls failed err=%lu", m.path, GetLastError());
    return false;
  }
  // 核心模块走 protocol::ProtocolStart（同时绑好 proxy -> framework 的转发通道），
  // 额外模块只发 init / shutdown。
  const bool ok = m.core ? protocol::ProtocolStart(handle, g_host)
                         : protocol::ModuleStart(handle, g_host);
  if (!ok) {
    Log("module: %ls has no NotifyPlatform, ignored", m.name);
    if (m.core) {
      protocol::ProtocolStop();
    }
    FreeLibrary(handle);
    return false;
  }
  m.handle = handle;
  ReadSig(m.path, m.sig);
  Log("module: %ls loaded (%s)", m.name, m.core ? "core" : "extra");
  return true;
}

void UnloadModule(int index) {
  Module& m = g_modules[index];
  if (m.handle == nullptr) {
    return;
  }
  // 先摘掉它注册的日志 sink，再让模块自己收线程/窗口，最后解除映射；
  // 顺序反过来会把回调打到已释放的代码里。
  if (!m.core) {
    log_relay::ClearSink();
  }
  if (m.core) {
    protocol::ProtocolStop();
  } else {
    protocol::ModuleStop(m.handle);
  }
  FreeLibrary(m.handle);
  m.handle = nullptr;
  m.sig = FileSig{0, 0, 0, 0, 0, false};
}

// 文件变了：卸载旧模块 -> 重试加载新文件。
bool ReloadModule(int index) {
  Module& m = g_modules[index];
  Log("module: %ls changed, hot reloading", m.name);
  UnloadModule(index);

  for (int i = 0; i < kLoadRetries; ++i) {
    if (LoadModule(index)) {
      DeleteStaleOld(m);
      Log("module: %ls hot reload done", m.name);
      return true;
    }
    Sleep(kLoadRetryMs);
  }
  // 记下新标识，避免每次轮询都重来一遍；修好文件（再次改名换位）才会再触发。
  ReadSig(m.path, m.sig);
  Log("module: %ls hot reload failed after %d attempts, restart the host",
      m.name, kLoadRetries);
  return false;
}

// 监视线程：只认文件标识变化，写完（两次一致）才动手。
DWORD WINAPI WatchThread(LPVOID) {
  Log("module: watcher up (poll=%lums, modules=%d)",
      static_cast<unsigned long>(kPollMs), g_moduleCount);
  for (;;) {
    for (DWORD waited = 0; waited < kPollMs; waited += kStopSliceMs) {
      if (g_stop) {
        return 0;
      }
      Sleep(kStopSliceMs);
    }
    if (g_stop) {
      return 0;
    }

    for (int i = 0; i < g_moduleCount; ++i) {
      Module& m = g_modules[i];
      FileSig first = FileSig{0, 0, 0, 0, 0, false};
      if (!ReadSig(m.path, first) || SameSig(first, m.sig)) {
        continue;
      }
      Sleep(kSettleMs);
      FileSig second = FileSig{0, 0, 0, 0, 0, false};
      if (!ReadSig(m.path, second) || !SameSig(first, second)) {
        continue;  // 还在写，下一轮再看
      }
      ReloadModule(i);
    }
  }
}

}  // namespace

namespace module_loader {

bool Start(const protocol::HostContext& host) {
  g_host = host;
  g_moduleCount = 0;
  log_relay::Start();  // 模块可用 {"cmd":"logSink"} 订阅 proxy 日志

  // 核心模块总是第一个；[modules] 里的额外模块排在其后。
  AddModule(kCoreModule, true);
  wchar_t extras[kMaxModuleNames][kMaxModuleNameChars] = {};
  const int extraCount = ExtraModuleNames(extras, kMaxModuleNames);
  for (int i = 0; i < extraCount; ++i) {
    if (_wcsicmp(extras[i], kCoreModule) == 0) {
      continue;  // 重复列出 framework.dll 时忽略
    }
    AddModule(extras[i], false);
  }

  const bool coreOk = (g_moduleCount > 0) && LoadModule(0);
  if (!coreOk) {
    Log("WARN: framework.dll not loaded, ws server is down until it appears");
  }
  for (int i = 1; i < g_moduleCount; ++i) {
    LoadModule(i);  // 额外模块可选，失败只写日志
  }

  Log("modules: %d total, ws=ws://127.0.0.1:47801/ws", g_moduleCount);

  g_stop = false;
  g_thread = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
  if (g_thread == nullptr) {
    Log("WARN: module watcher thread failed err=%lu", GetLastError());
    return coreOk;  // 已加载成功，只是没有热加载能力
  }
  CloseHandle(g_thread);  // 不等它退出，停止信号走 g_stop
  return coreOk;
}

void Stop() {
  g_stop = true;
  for (int i = 0; i < g_moduleCount; ++i) {
    UnloadModule(i);
  }
}

}  // namespace module_loader
