// ============================================================================
// framework.cpp - MVP 的 framework.dll 实现（组合根）。
//
// 职责（MVP 只验证一条闭环）：
//   浏览器 --WebSocket--> framework.dll --> 宿主窗口（动鼠标/按键/等待）--> 回日志
//
// 设计要点：
//   - 作为 pplugin 插件，经 GetDesc/init(const Host*) 拿到 proxy 交来的能力
//     （Host: publishLog / mainWindow / viewWindow / scale）。
//   - 用 CivetWeb（libs/civetweb，C，裸 TCP/WS，零协议风险）起一个
//     127.0.0.1 的 WebSocket 服务端，URI 为 /ws；顺带把 DLL 所在目录当 document_root，
//     可直接 http 托管 index.html。
//   - 事件执行：鼠标/键盘/滚轮/文本走 PostMessage 后台投递到目标窗口（不动物理鼠标）。
//   - 每条命令执行后，把日志写回 proxy.log（host->log）并通过 WS 回一个 JSON 包。
//
// 模块化拆分（见 framework/ 各 .h/.cpp）：
//   logger          日志桥（host->publishLog）
//   window_target   目标窗口 + 坐标换算 + 虚拟光标
//   input_dispatcher 后台输入派发（PostMessage）
//   script_store    脚本文件持久化（%APPDATA%\TTVoice\scripts）
//   command_handler WS 命令解析与分发 + 几何/光标读数
//   ws_server       CivetWeb 封装（/ws + /api/*）
//   本文件只负责：DLL 生命周期、组装上述模块、对接 picker 广播、端口自测。
//
// 明确不做（见 MVP 规划）：截图取点、OCR、任务调度、热重载、Web 编辑器、JS 脚本层。
// ============================================================================

#include <winsock2.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "../common/json_util.h"

#include "civetweb.h"
#include "command_handler.h"
#include "input_dispatcher.h"
#include "logger.h"
#include "picker.h"
#include "pplugin.h"
#include "script_store.h"
#include "ws_server.h"
#include "window_target.h"

namespace {

HMODULE g_hModule = nullptr;
wchar_t g_moduleDir[MAX_PATH] = {};

// picker 模块的广播出口：给某个 WS 连接写一帧文本。
void PickerSend(void* conn, const char* data, size_t len) {
  if (conn != nullptr) {
    mg_websocket_write(static_cast<struct mg_connection*>(conn),
                       MG_WEBSOCKET_OPCODE_TEXT, data, len);
  }
}

const char* ModuleDirUtf8() {
  static char buf[MAX_PATH * 3] = {};
  if (g_moduleDir[0] == L'\0') {
    buf[0] = '.';
    buf[1] = '\0';
    return buf;
  }
  WideCharToMultiByte(CP_UTF8, 0, g_moduleDir, -1, buf, sizeof(buf), nullptr,
                      nullptr);
  return buf;
}

// 在 mg_start 之前自己先绑一次端口，把 WSA 错误号打进 proxy.log。
// 这样即便 CivetWeb 没把 bind 失败原因通过 log_message 回调吐出来，也能定位
// （例如 WSA=10048 表示端口已被占用）。
static void ProbePort(const Host* host, int port) {
  if (host == nullptr || host->publishLog == nullptr) {
    return;
  }
  WSADATA wd;
  if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
    host->publishLog("ProbePort: WSAStartup failed");
    return;
  }
  SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
  if (s == INVALID_SOCKET) {
    host->publishLog("ProbePort: socket() failed");
    WSACleanup();
    return;
  }
  int yes = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
  sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<u_short>(port));
  a.sin_addr.s_addr = inet_addr("127.0.0.1");
  int r = bind(s, (sockaddr*)&a, sizeof(a));
  if (r != 0) {
    const int e = WSAGetLastError();
    char buf[192];
    snprintf(buf, sizeof(buf),
             "ProbePort: bind 127.0.0.1:%d failed, WSA=%d", port, e);
    host->publishLog(buf);
  } else {
    char buf[192];
    snprintf(buf, sizeof(buf), "ProbePort: bind 127.0.0.1:%d ok (端口可用)",
             port);
    host->publishLog(buf);
  }
  closesocket(s);
  WSACleanup();
}

// 组合根：持有全部子模块。成员按依赖顺序声明，使引用成员的构造先于使用。
//   Logger / WindowTarget / ScriptStore 无依赖；InputDispatcher 依赖 WindowTarget；
//   CommandHandler 依赖 Logger/WindowTarget/InputDispatcher；WsServer 依赖
//   Logger/CommandHandler/ScriptStore。
class Framework {
 public:
  Framework() : input_(win_), handler_(log_, win_, input_) {}

  int start(const Host* host, const char* doc_root, int port) {
    log_.set_host(host);
    win_.set_host(host);
    ProbePort(host, port);  // 先自测端口是否可绑定（结果见 proxy.log）
    if (!server_.start(log_, handler_, scripts_, doc_root, port)) {
      host->publishLog("mg_start failed");
      return ERR_STATE;
    }
    if (!PickerStart(host, &PickerSend)) {
      host->publishLog("picker: start failed");
    }
    return P_OK;
  }

  // 正常卸载（PluginShutdown 调用）：等待推送线程退出。
  void stop() {
    PickerStop();
    server_.stop();
  }

  // 进程卸载（DllMain 调用）：只发退出信号、不等待，避免与加载器锁互等。
  void unload() {
    PickerRequestStop();
    server_.stop();
  }

 private:
  Logger log_;
  WindowTarget win_;
  InputDispatcher input_;  // 依赖 win_
  ScriptStore scripts_;
  CommandHandler handler_;  // 依赖 log_/win_/input_/scripts_
  WsServer server_;
};

Framework g_fw;

}  // namespace

// pplugin 插件入口：Desc::init(const Host*) 由 framework 的 plugin_loader 调用。
static int __cdecl PluginInit(const Host* host) {
  if (host == nullptr || host->abi != P_ABI ||
      host->structSize < sizeof(Host)) {
    return ERR_STATE;
  }
  if (host->publishLog == nullptr) {
    return ERR_PARAM;
  }

  const int port = 47801;
  const char* doc_root = ModuleDirUtf8();
  const int rc = g_fw.start(host, doc_root, port);
  if (rc == P_OK) {
    char ok[512] = {};
    std::snprintf(ok, sizeof(ok), "started, ws=ws://127.0.0.1:47801/ws doc_root=%s",
                  doc_root);
    host->publishLog(ok);
  }
  return rc;
}

static int __cdecl PluginTick(void) { return P_OK; }

static int __cdecl PluginShutdown(void) {
  g_fw.stop();
  return P_OK;
}

static const Desc g_desc = {
    P_ABI,
    static_cast<uint32_t>(sizeof(Desc)),
    "framework",
    "1.0",
    &PluginInit,
    &PluginTick,
    &PluginShutdown,
};

extern "C" __declspec(dllexport) const Desc* __cdecl GetDesc(void) {
  return &g_desc;
}

// ============================================================================
// 新链路：version.dll 直连的 JSON 协议入口（NotifyPlatform）。
//
// 不再需要 GetDesc/pplugin：version.dll 直接 LoadLibrary(framework.dll) 后调
// NotifyPlatform。这里把 ghost 交来的 dispatch 指针包成一张 Host 表，复用现有
// 子模块（logger/window_target 只认 Host*），从而零改动接入新链路。
// GetDesc 暂时保留，等新链路验证通过后再退役。
// ============================================================================
namespace {

typedef int (__cdecl* NotifyGhostFn)(const char*, char*, int, void*);
NotifyGhostFn g_dispatch = nullptr;
void* g_dispatchCtx = nullptr;

int WriteResp(const char* body, char* resp, int cap) {
  if (body == nullptr || resp == nullptr || cap <= 0) {
    return -1;
  }
  const int need = static_cast<int>(std::strlen(body));
  if (need + 1 > cap) {
    return -(need + 1);
  }
  std::memcpy(resp, body, static_cast<size_t>(need));
  resp[need] = '\0';
  return need;
}

// 调 ghost(request) 并把响应写进 out；返回是否成功。
bool CallBase(const char* req, char* out, int cap) {
  out[0] = '\0';
  if (g_dispatch == nullptr) {
    return false;
  }
  const int n = g_dispatch(req, out, cap, g_dispatchCtx);
  if (n < 0) {
    out[0] = '\0';
    return false;
  }
  return true;
}

void __cdecl DispatchPublishLog(const char* text) {
  ppjson::Node nodes[8] = {};
  ppjson::Pool pool(nodes, 8);
  ppjson::Value j = ppjson::Value::Obj(pool);
  j.SetStr("cmd", "publishLog");
  j.SetStr("text", (text != nullptr) ? text : "");
  char buf[1024] = {};
  j.Dump(buf, sizeof(buf));
  char resp[128] = {};
  CallBase(buf, resp, sizeof(resp));  // 响应忽略，仅转发日志
}

// 调 base 取一个 hwnd（"mainWindow"/"viewWindow"），解析 {"hwnd":"..."}。
HWND __cdecl DispatchWindowByCmd(const char* req) {
  char buf[512] = {};
  if (!CallBase(req, buf, sizeof(buf))) {
    return nullptr;
  }
  ppjson::Node nodes[8] = {};
  ppjson::Pool pool(nodes, 8);
  ppjson::Value j;
  char err[32] = {};
  if (!ppjson::Value::Parse(buf, pool, j, err, sizeof(err))) {
    return nullptr;
  }
  const char* hwnd = j.Find("hwnd").AsStr();
  return reinterpret_cast<HWND>(
      static_cast<uintptr_t>(std::strtoull(hwnd, nullptr, 10)));
}

HWND __cdecl DispatchMainWindow() { return DispatchWindowByCmd("{\"cmd\":\"mainWindow\"}"); }
HWND __cdecl DispatchViewWindow() { return DispatchWindowByCmd("{\"cmd\":\"viewWindow\"}"); }

float __cdecl DispatchScale() {
  char buf[512] = {};
  if (!CallBase("{\"cmd\":\"scale\"}", buf, sizeof(buf))) {
    return 1.0f;
  }
  ppjson::Node nodes[8] = {};
  ppjson::Pool pool(nodes, 8);
  ppjson::Value j;
  char err[32] = {};
  if (!ppjson::Value::Parse(buf, pool, j, err, sizeof(err))) {
    return 1.0f;
  }
  return static_cast<float>(j.Find("scale").AsNum(1.0));
}

// 用 ghost 的 dispatch 拼出 Host 表，交给现有子模块。
const Host g_hostViaDispatch = {
    P_ABI,
    static_cast<uint32_t>(sizeof(Host)),
    &DispatchPublishLog,
    &DispatchMainWindow,
    &DispatchViewWindow,
    &DispatchScale,
    nullptr,
    nullptr,
};

int StartViaProtocol() {
  const int port = 47801;
  const char* doc_root = ModuleDirUtf8();
  const int rc = g_fw.start(&g_hostViaDispatch, doc_root, port);
  if (rc == P_OK) {
    DispatchPublishLog("framework started via protocol (NotifyPlatform)");
  }
  return rc;
}

}  // namespace

extern "C" __declspec(dllexport)
int __cdecl NotifyPlatform(const char* reqJson, char* respJson, int cap) {
  if (reqJson == nullptr || respJson == nullptr || cap <= 0) {
    return -1;
  }
  ppjson::Node nodes[16] = {};
  ppjson::Pool pool(nodes, 16);
  ppjson::Value req;
  char err[64] = {};
  if (!ppjson::Value::Parse(reqJson, pool, req, err, sizeof(err))) {
    return WriteResp("{\"ok\":false,\"err\":\"invalid json\",\"code\":400}", respJson, cap);
  }
  const char* cmd = req.Find("cmd").AsStr();

  if (std::strcmp(cmd, "init") == 0) {
    g_dispatch = reinterpret_cast<NotifyGhostFn>(
        static_cast<uintptr_t>(std::strtoull(req.Find("dispatch").AsStr(), nullptr, 10)));
    g_dispatchCtx = reinterpret_cast<void*>(
        static_cast<uintptr_t>(std::strtoull(req.Find("ctx").AsStr(), nullptr, 10)));
    char buf[64] = {};
    std::snprintf(buf, sizeof(buf), "{\"ok\":%s,\"proto\":1}",
                  (StartViaProtocol() == P_OK) ? "true" : "false");
    return WriteResp(buf, respJson, cap);
  }
  if (std::strcmp(cmd, "shutdown") == 0) {
    g_fw.stop();
    return WriteResp("{\"ok\":true}", respJson, cap);
  }
  char buf[192] = {};
  std::snprintf(buf, sizeof(buf), "{\"ok\":true,\"echo\":\"%s\"}", cmd);
  return WriteResp(buf, respJson, cap);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_hModule = hModule;
    wchar_t buf[MAX_PATH] = {};
    if (GetModuleFileNameW(hModule, buf, MAX_PATH) > 0) {
      // 去掉文件名，保留目录：找最后一个反斜杠并截断（无 STL）。
      wchar_t* last = wcsrchr(buf, L'\\');
      if (last != nullptr) {
        *last = L'\0';
      }
      wcscpy_s(g_moduleDir, MAX_PATH, buf);
    }
    DisableThreadLibraryCalls(hModule);
  } else if (reason == DLL_PROCESS_DETACH) {
    g_fw.unload();
  }
  return TRUE;
}
