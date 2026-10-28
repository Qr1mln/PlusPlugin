// ============================================================================
// picker.cpp - 取点模块实现（协议见 picker.h）。
//
// 两条数据流：
//   1) 光标流：有订阅者时，每 33ms 采样一次 GetCursorPos + ScreenToClient 后广播；
//   2) 拾取：装 WH_MOUSE_LL 钩子，落在 view 内的左键被吞掉并广播坐标，右键取消。
//
// 关键点：
//   - 低级鼠标钩子由安装它的线程派发，所以本模块的推送线程保持消息循环即可，
//     钩子回调天然与采样同线程，不需要额外同步采样状态。
//   - 广播时持有 g_mutex；连接 close 回调也拿这把锁摘连接，避免写到已回收的 conn。
// ============================================================================

#include "picker.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "pick_overlay.h"

#include "../common/log.h"

namespace {

const HostCaps* g_host = nullptr;
PickerBroadcast g_broadcast = nullptr;

// 推送线程的消息 id
const UINT kMsgPickStart = WM_APP + 1;  // 装钩子，进入拾取态
const UINT kMsgPickFlush = WM_APP + 2;  // 发掉待发拾取结果（wParam=1 时发完卸钩子）
const UINT_PTR kTimerId = 1;
const UINT kTickMs = 33;  // 约 30Hz

// WS 连接表：conn 由 framework.cpp 传入（mg_connection*），watch=是否订阅光标流
struct Client {
    void* conn;
    bool watch;
};
const int kMaxClients = 64;

CRITICAL_SECTION g_cs;          // 保护 g_clients / watch 状态 / 待发拾取结果
bool g_csInit = false;          // g_cs 仅首次建线程前初始化一次
Client g_clients[kMaxClients];
int g_clientCount = 0;
std::atomic<int> g_watchCount(0);
std::atomic<bool> g_pickActive(false);

char g_pendingPick[256];
bool g_pendingReady = false;

// RAII 锁：构造即 EnterCriticalSection，析构即 LeaveCriticalSection。
struct CsGuard {
  explicit CsGuard(CRITICAL_SECTION& cs) : cs_(cs) { EnterCriticalSection(&cs_); }
  ~CsGuard() { LeaveCriticalSection(&cs_); }
  CsGuard(const CsGuard&) = delete;
  CsGuard& operator=(const CsGuard&) = delete;
  CRITICAL_SECTION& cs_;
};

// 钩子状态（只由推送线程访问）
HHOOK g_hook = nullptr;
bool g_swallowLeftUp = false;
bool g_swallowRightUp = false;

HANDLE g_thread = nullptr;
volatile DWORD g_tid = 0;

void LogLine(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    LogTagged("framework", "%s", buf);
}

// 目标窗口：优先 viewWindow（真正收输入的 Flutter 子窗口），回退 mainWindow。
HWND TargetWindow() {
    HWND h = nullptr;
    if (g_host != nullptr && g_host->viewWindow != nullptr) {
        h = g_host->viewWindow();
    }
    if (h == nullptr && g_host != nullptr && g_host->mainWindow != nullptr) {
        h = g_host->mainWindow();
    }
    return h;
}

double HostScale() {
    const double s = (g_host != nullptr && g_host->scale != nullptr) ? g_host->scale() : 1.0;
    return (s > 0.0) ? s : 1.0;
}

double Round1(double v) {
    return (v >= 0.0) ? std::floor(v * 10.0 + 0.5) / 10.0 : std::ceil(v * 10.0 - 0.5) / 10.0;
}

// 把一帧文本发给全部（或仅订阅的）连接。
// 写在持锁区间内，保证用到的 conn 不会被并发回收；wait=false 时拿不到锁就跳过这一帧
// （连接正在关闭），避免与 CivetWeb 的关闭路径互相等待。
void Broadcast(const char* msg, int msgLen, bool onlyWatch, bool wait) {
    if (g_broadcast == nullptr || msg == nullptr || msgLen <= 0) {
        return;
    }
    if (wait) {
        EnterCriticalSection(&g_cs);
    } else if (!TryEnterCriticalSection(&g_cs)) {
        return;  // 连接正在关闭，跳过这一帧
    }
    for (int i = 0; i < g_clientCount; ++i) {
        const Client& c = g_clients[i];
        if (onlyWatch && !c.watch) {
            continue;
        }
        g_broadcast(c.conn, msg, static_cast<size_t>(msgLen));
    }
    LeaveCriticalSection(&g_cs);
}

// 排一条拾取结果并唤醒推送线程立刻发出（stopAfter=true 时发完卸钩子）。
void QueuePick(const char* json, bool stopAfter) {
    {
        CsGuard lk(g_cs);
        std::strncpy(g_pendingPick, (json != nullptr) ? json : "",
                     sizeof(g_pendingPick) - 1);
        g_pendingPick[sizeof(g_pendingPick) - 1] = '\0';
        g_pendingReady = true;
    }
    const DWORD tid = g_tid;
    if (tid != 0) {
        PostThreadMessageW(tid, kMsgPickFlush, stopAfter ? 1 : 0, 0);
    }
}

// 低级鼠标钩子：拾取态下吞掉 view 内的左键并上报坐标；右键取消（同样吞掉）。
LRESULT CALLBACK MouseHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0 || !g_pickActive.load()) {
        return CallNextHookEx(g_hook, code, wParam, lParam);
    }
    if (wParam == WM_LBUTTONDOWN) {
        const MSLLHOOKSTRUCT* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        const HWND wnd = TargetWindow();
        if (ms != nullptr && wnd != nullptr) {
            POINT cp = ms->pt;
            RECT rc;
            if (ScreenToClient(wnd, &cp) && GetClientRect(wnd, &rc) && cp.x >= 0 && cp.y >= 0 &&
                cp.x < rc.right && cp.y < rc.bottom) {
                const double scale = HostScale();
                ppjson::Node nodes[16] = {};
                ppjson::Pool pool(nodes, 16);
                ppjson::Value p = ppjson::Value::Obj(pool);
                p.SetStr("t", "pick");
                p.SetNum("sx", ms->pt.x);
                p.SetNum("sy", ms->pt.y);
                p.SetNum("x", cp.x);
                p.SetNum("y", cp.y);
                p.SetNum("lx", Round1(cp.x / scale));
                p.SetNum("ly", Round1(cp.y / scale));
                p.SetNum("scale", scale);
                char jbuf[256] = {};
                p.Dump(jbuf, sizeof(jbuf));
                g_swallowLeftUp = true;
                QueuePick(jbuf, true);
                return 1;  // 吞掉这次点击，应用不响应
            }
        }
    } else if (wParam == WM_LBUTTONUP && g_swallowLeftUp) {
        g_swallowLeftUp = false;
        return 1;
    } else if (wParam == WM_RBUTTONDOWN) {
        g_swallowRightUp = true;
        ppjson::Node nodes[8] = {};
        ppjson::Pool pool(nodes, 8);
        ppjson::Value p = ppjson::Value::Obj(pool);
        p.SetStr("t", "pick");
        p.SetStr("action", "cancel");
        char jbuf[128] = {};
        p.Dump(jbuf, sizeof(jbuf));
        QueuePick(jbuf, true);
        return 1;
    } else if (wParam == WM_RBUTTONUP && g_swallowRightUp) {
        g_swallowRightUp = false;
        return 1;
    }
    return CallNextHookEx(g_hook, code, wParam, lParam);
}

// 拾取期间的临时置顶：先记下窗口原本是否已经置顶，拾取结束（取到点/取消）再还原，
// 免得把用户自己设的置顶也一起关掉。最小化时顺手还原窗口，否则窗口和遮罩都在屏幕外。
HWND g_topRoot = nullptr;
bool g_needRestoreTopmost = false;

void ApplyTopmost(bool on) {
    if (on) {
        const HWND view = TargetWindow();
        if (view == nullptr) {
            return;
        }
        const HWND root = GetAncestor(view, GA_ROOT);
        if (root == nullptr) {
            return;
        }
        if (IsIconic(root)) {
            ShowWindow(root, SW_RESTORE);
        }
        g_topRoot = root;
        g_needRestoreTopmost =
            (GetWindowLongPtrW(root, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0;
        SetWindowPos(root, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        LogLine("pick: window topmost on (restore=%d)", g_needRestoreTopmost ? 1 : 0);
        return;
    }
    if (g_topRoot != nullptr) {
        if (g_needRestoreTopmost && IsWindow(g_topRoot)) {
            SetWindowPos(g_topRoot, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        g_topRoot = nullptr;
        g_needRestoreTopmost = false;
        LogLine("pick: window topmost restored");
    }
}

void PickStopInThread() {
    if (g_hook != nullptr) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
        LogLine("pick: hook removed");
    }
    PickOverlayHide();
    ApplyTopmost(false);
    g_swallowLeftUp = false;
    g_swallowRightUp = false;
    g_pickActive.store(false);
}

void PickStartInThread(bool topmost) {
    if (g_hook != nullptr) {
        return;
    }
    g_swallowLeftUp = false;
    g_swallowRightUp = false;
    if (topmost) {
        ApplyTopmost(true);
    }
    // 低级钩子不需要跨进程注入，hMod 传 NULL 即可。
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, &MouseHookProc, nullptr, 0);

    ppjson::Node nodes[8] = {};
    ppjson::Pool pool(nodes, 8);
    ppjson::Value p = ppjson::Value::Obj(pool);
    p.SetStr("t", "pick");
    if (g_hook == nullptr) {
        g_pickActive.store(false);
        const DWORD err = GetLastError();
        LogLine("pick: SetWindowsHookEx failed err=%lu", err);
        p.SetStr("action", "failed");
        p.SetStr("error", "set hook failed");
    } else {
        g_pickActive.store(true);
        // 遮罩盖在 view 上：十字线 + 实时坐标，点完之后在 PickStopInThread 里收起。
        const bool overlay = PickOverlayShow(g_host);
        LogLine("pick: hook installed (左键拾取 / 右键取消) overlay=%d",
                overlay ? 1 : 0);
        p.SetStr("action", "ready");
    }
    char jbuf[256] = {};
    p.Dump(jbuf, sizeof(jbuf));
    Broadcast(jbuf, static_cast<int>(std::strlen(jbuf)), false, true);
}

void PickFlushInThread(bool stopAfter) {
    char msg[256] = {};
    bool has = false;
    {
        CsGuard lk(g_cs);
        if (g_pendingReady) {
            std::strncpy(msg, g_pendingPick, sizeof(msg) - 1);
            msg[sizeof(msg) - 1] = '\0';
            g_pendingReady = false;
            has = true;
        }
    }
    if (has) {
        Broadcast(msg, static_cast<int>(std::strlen(msg)), false, true);
    }
    if (stopAfter) {
        PickStopInThread();
    }
}

void PushCursorTick() {
    if (g_watchCount.load() <= 0) {
        return;
    }
    ppjson::Node nodes[16] = {};
    ppjson::Pool pool(nodes, 16);
    ppjson::Value c = ppjson::Value::Obj(pool);
    if (!PickerCursorInfo(c)) {
        return;
    }
    c.SetStr("t", "cursor");
    char jbuf[384] = {};
    c.Dump(jbuf, sizeof(jbuf));
    Broadcast(jbuf, static_cast<int>(std::strlen(jbuf)), true, false);
}

DWORD WINAPI PushThreadProc(LPVOID) {
    g_tid = GetCurrentThreadId();
    SetTimer(nullptr, kTimerId, kTickMs, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER) {
            PushCursorTick();
            PickOverlayTick();  // 拾取时跟随光标重绘遮罩
        } else if (msg.message == kMsgPickStart) {
            PickStartInThread(msg.wParam != 0);
        } else if (msg.message == kMsgPickFlush) {
            PickFlushInThread(msg.wParam != 0);
        } else {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    PickStopInThread();
    PickOverlayDestroy();  // 窗口回调在本 DLL 里，线程退出前必须销毁
    KillTimer(nullptr, kTimerId);
    g_tid = 0;
    return 0;
}

}  // namespace

bool PickerStart(const HostCaps* host, PickerBroadcast broadcast) {
    if (host == nullptr || broadcast == nullptr) {
        return false;
    }
    g_host = host;
    g_broadcast = broadcast;
    if (g_thread != nullptr) {
        return true;
    }
    if (!g_csInit) {
        InitializeCriticalSection(&g_cs);
        g_csInit = true;
    }
    g_thread = CreateThread(nullptr, 0, &PushThreadProc, nullptr, 0, nullptr);
    if (g_thread == nullptr) {
        LogLine("picker: CreateThread failed err=%lu", GetLastError());
        return false;
    }
    LogLine("picker: push thread started");
    return true;
}

void PickerRequestStop() {
    const DWORD tid = g_tid;
    if (tid != 0) {
        PostThreadMessageW(tid, WM_QUIT, 0, 0);
    }
    // 不等待线程退出：DllMain 里 join 会与加载器锁互等。
}

void PickerStop() {
    if (g_thread == nullptr) {
        return;
    }
    PickerRequestStop();
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = nullptr;
    g_tid = 0;
    {
        CsGuard lk(g_cs);
        g_clientCount = 0;
        g_watchCount.store(0);
    }
    LogLine("picker: push thread stopped");
}

void PickerAddClient(void* conn) {
    CsGuard lk(g_cs);
    if (g_clientCount < kMaxClients) {
        g_clients[g_clientCount].conn = conn;
        g_clients[g_clientCount].watch = false;
        ++g_clientCount;
    }
}

void PickerRemoveClient(void* conn) {
    CsGuard lk(g_cs);
    for (int i = 0; i < g_clientCount; ++i) {
        if (g_clients[i].conn == conn) {
            if (g_clients[i].watch) {
                g_watchCount.fetch_sub(1);
            }
            // 用尾部元素填补空洞，保持数组紧凑（O(1)）。
            g_clients[i] = g_clients[g_clientCount - 1];
            --g_clientCount;
            break;
        }
    }
}

bool PickerSetWatch(void* conn, bool on) {
    if (conn == nullptr) {
        return false;
    }
    CsGuard lk(g_cs);
    for (int i = 0; i < g_clientCount; ++i) {
        if (g_clients[i].conn == conn) {
            if (g_clients[i].watch != on) {
                g_clients[i].watch = on;
                if (on) {
                    g_watchCount.fetch_add(1);
                } else {
                    g_watchCount.fetch_sub(1);
                }
            }
            return on;
        }
    }
    return false;
}

bool PickerBeginPick(bool topmost) {
    const DWORD tid = g_tid;
    if (tid == 0) {
        return false;
    }
    PostThreadMessageW(tid, kMsgPickStart, topmost ? 1 : 0, 0);
    return true;
}

void PickerCancelPick() {
    ppjson::Node nodes[8] = {};
    ppjson::Pool pool(nodes, 8);
    ppjson::Value p = ppjson::Value::Obj(pool);
    p.SetStr("t", "pick");
    p.SetStr("action", "cancel");
    char jbuf[128] = {};
    p.Dump(jbuf, sizeof(jbuf));
    QueuePick(jbuf, true);
}

// 一次性光标读数：屏幕坐标、view 客户区物理像素、逻辑像素（x/scale）、窗口尺寸。
bool PickerCursorInfo(ppjson::Value& out) {
    const HWND wnd = TargetWindow();
    if (wnd == nullptr) {
        return false;
    }
    POINT sp = {0, 0};
    if (!GetCursorPos(&sp)) {
        return false;
    }
    RECT rc;
    if (!GetClientRect(wnd, &rc)) {
        return false;
    }
    POINT cp = sp;
    if (!ScreenToClient(wnd, &cp)) {
        return false;
    }
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const double scale = HostScale();
    out.SetNum("sx", sp.x);
    out.SetNum("sy", sp.y);
    out.SetNum("x", cp.x);
    out.SetNum("y", cp.y);
    out.SetNum("lx", Round1(cp.x / scale));
    out.SetNum("ly", Round1(cp.y / scale));
    out.SetNum("w", w);
    out.SetNum("h", h);
    out.SetNum("scale", scale);
    out.SetBool("inside", (cp.x >= 0 && cp.y >= 0 && cp.x < w && cp.y < h));
    // 页面此时没有焦点，浏览器收不到按键，只能由宿主进程采样全局键态。
    out.SetBool("f8", ((GetAsyncKeyState(VK_F8) & 0x8000) != 0));
    return true;
}
