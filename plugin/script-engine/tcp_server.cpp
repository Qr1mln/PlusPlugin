// ============================================================================
// tcp_server.cpp - TCP 服务（127.0.0.1:47800，单客户端）+ 协议解析
//
// 只监听回环地址，不做鉴权：能连 127.0.0.1 的进程本来就能在本机做任何事。
// 帧 = 4 字节小端长度 + UTF-8 JSON；解析失败只记日志、不断连接。
// ============================================================================
#include "engine.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "../../libs/nlohmann/json.hpp"

namespace tcp {
namespace {

std::mutex g_clientMx;                    // 保护 g_client 的读写与发送/关闭互斥
SOCKET g_client = INVALID_SOCKET;
std::thread g_thread;

// 读满 want 字节：200ms 轮询既能容忍客户端连上后暂时不发数据，
// 也能在引擎退出时及时收尾。返回 false 表示对端断开 / 出错 / 要退出。
bool RecvAll(SOCKET s, void* dst, int want) {
    char* p = static_cast<char*>(dst);
    int left = want;
    while (left > 0) {
        if (se::Quitting()) return false;
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s, &rf);
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        const int sel = select(0, &rf, nullptr, nullptr, &tv);
        if (sel < 0) return false;
        if (sel == 0) continue;
        const int got = recv(s, p, left, 0);
        if (got <= 0) return false;   // 0=对端断开，<0=出错
        p += got;
        left -= got;
    }
    return true;
}

bool SendAll(SOCKET s, const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        const int n = send(s, data + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

const char* CmdName(se::CmdType t) {
    switch (t) {
    case se::CmdType::Load: return "load";
    case se::CmdType::Play: return "play";
    case se::CmdType::Stop: return "stop";
    case se::CmdType::ReloadSchedule: return "reload_schedule";
    case se::CmdType::GetSchedule: return "get_schedule";
    case se::CmdType::RunScripts: return "run_scripts";
    case se::CmdType::Delay: return "delay";
    }
    return "?";
}

// 解析一帧命令。JSON 里的浮点坐标（如 504.7）由 nlohmann 转成 int，不会解析失败。
bool ParseCommand(const std::string& text, se::Command& out) {
    try {
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return false;
        const std::string cmd = j.value("cmd", std::string());
        if (cmd == "play") {
            out.type = se::CmdType::Play;
            return true;
        }
        if (cmd == "stop") {
            out.type = se::CmdType::Stop;
            return true;
        }
        if (cmd == "reload_schedule") {
            out.type = se::CmdType::ReloadSchedule;
            return true;
        }
        if (cmd == "get_schedule") {
            out.type = se::CmdType::GetSchedule;
            return true;
        }
        if (cmd == "run_scripts") {
            // 「立即执行」：编辑器直接给脚本名列表，不依赖已保存的任务配置。
            out.type = se::CmdType::RunScripts;
            out.loop = j.value("loop", 1);
            out.dir = j.value("dir", std::string());
            out.ms = std::max(0, j.value("gap_ms", 0));   // 脚本之间的间隔
            out.scripts.clear();
            const auto it = j.find("scripts");
            if (it != j.end() && it->is_array()) {
                for (const auto& s : *it) {
                    if (s.is_string()) {
                        const std::string name = s.get<std::string>();
                        if (!name.empty()) out.scripts.push_back(name);
                    }
                }
            }
            return true;
        }
        if (cmd != "load") return false;
        out.type = se::CmdType::Load;
        out.loop = j.value("loop", 1);
        out.events.clear();
        const auto it = j.find("events");
        if (it == j.end() || !it->is_array()) return true;   // 空事件表也接受
        for (const auto& e : *it) {
            if (!e.is_object()) continue;
            se::Event ev;
            ev.type = e.value("type", std::string());
            ev.action = e.value("action", std::string());
            ev.button = e.value("button", std::string());
            ev.text = e.value("text", std::string());
            ev.key = e.value("key", std::string());
            ev.image = e.value("image", std::string());
            ev.x = e.value("x", 0);
            ev.y = e.value("y", 0);
            ev.ms = e.value("ms", 0);
            ev.timeout_ms = e.value("timeout_ms", 0);
            ev.threshold = e.value("threshold", 0.9);
            out.events.push_back(std::move(ev));
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void ServeClient(SOCKET c) {
    for (;;) {
        uint32_t n = 0;
        if (!RecvAll(c, &n, 4)) break;
        if (n == 0 || n > se::kMaxFrame) {
            se::Log("sock: bad frame length %u", n);
            break;
        }
        std::string frame(n, '\0');
        if (!RecvAll(c, &frame[0], static_cast<int>(n))) break;
        se::Log("sock: frame %u bytes", n);

        se::Command cmd;
        if (!ParseCommand(frame, cmd)) {
            se::Log("sock: bad command");
            continue;
        }
        se::Log("sock: recv cmd=%s", CmdName(cmd.type));
        const se::CmdType type = cmd.type;
        se::Push(std::move(cmd));
        if (type == se::CmdType::RunScripts) {
            // 立即回执：命令已入队，不等执行完（脚本可能要跑很久）。
            se::SendRaw("{\"t\":\"ok\",\"what\":\"run_scripts\"}");
        }
    }
}

void Loop() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        se::Log("script-engine: WSAStartup 失败");
        return;
    }
    SOCKET listen_s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_s == INVALID_SOCKET) {
        se::Log("script-engine: socket 创建失败");
        WSACleanup();
        return;
    }
    BOOL yes = TRUE;
    setsockopt(listen_s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes),
               sizeof yes);

    sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 127.0.0.1
    addr.sin_port = htons(static_cast<unsigned short>(se::kPort));
    if (bind(listen_s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == SOCKET_ERROR) {
        se::Log("script-engine: bind 127.0.0.1:%d 失败（端口被占用？）", se::kPort);
        closesocket(listen_s);
        WSACleanup();
        return;
    }
    listen(listen_s, 2);
    se::Log("script-engine: TCP 引擎监听 127.0.0.1:%d", se::kPort);

    while (!se::Quitting()) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(listen_s, &rf);
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;   // 200ms 轮询退出标志
        if (select(0, &rf, nullptr, nullptr, &tv) <= 0) continue;

        SOCKET c = accept(listen_s, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;

        // 发送超时 + 关 Nagle（小帧即时发出）；单客户端，新连接覆盖旧的。
        int sndto = 1000;
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sndto),
                   sizeof sndto);
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes),
                   sizeof yes);
        {
            std::lock_guard<std::mutex> lk(g_clientMx);
            g_client = c;
        }
        se::Log("sock: client connected");

        ServeClient(c);

        {
            std::lock_guard<std::mutex> lk(g_clientMx);
            if (g_client == c) g_client = INVALID_SOCKET;
            closesocket(c);
        }
        se::Log("sock: client disconnected");
    }

    {
        std::lock_guard<std::mutex> lk(g_clientMx);
        if (g_client != INVALID_SOCKET) {
            closesocket(g_client);
            g_client = INVALID_SOCKET;
        }
    }
    closesocket(listen_s);
    WSACleanup();
}

}  // namespace

void Start() {
    try {
        g_thread = std::thread(Loop);
    } catch (...) {
        g_thread = std::thread();
    }
}

void Stop() {
    if (g_thread.joinable()) g_thread.join();
}

bool Send(const std::string& json_frame) {
    std::string buf;
    const uint32_t n = static_cast<uint32_t>(json_frame.size());
    buf.append(reinterpret_cast<const char*>(&n), 4);
    buf += json_frame;

    std::lock_guard<std::mutex> lk(g_clientMx);
    if (g_client == INVALID_SOCKET) return false;
    return SendAll(g_client, buf.data(), static_cast<int>(buf.size()));
}

}  // namespace tcp
