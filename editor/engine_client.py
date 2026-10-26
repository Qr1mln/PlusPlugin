r"""与宿主进程内 script-engine.dll 的 TCP 帧协议客户端。

帧 = 4 字节小端长度 + UTF-8 JSON，双向：
  编辑器 -> DLL：{"cmd":"load","loop":N,"events":[...]} / {"cmd":"play"} / {"cmd":"stop"}
  DLL -> 编辑器：{"t":"log","msg":"..."} / {"t":"done"} / {"t":"stopped"}
"""
from __future__ import annotations

import json
import queue
import socket
import struct
import threading
import time

ENGINE_HOST = "127.0.0.1"
ENGINE_PORT = 47800
RUN_TIMEOUT = 60  # 单次执行看门狗：超过这么秒仍无 done，视为应用无响应，强制收尾断连

# 会话消息类型（EngineSession.messages 队列元素的第一项）
MSG_LOG = "log"      # 执行日志（第二项为文本）
MSG_DONE = "done"    # 一轮结束（done/stopped 或连接断开）
MSG_ERROR = "error"  # 执行错误（第二项为文本）


class EngineClient:
    """连引擎 TCP 端口，发送命令帧、接收回包帧（线程安全）。"""

    def __init__(self, host: str = ENGINE_HOST, port: int = ENGINE_PORT):
        self.host = host
        self.port = port
        self.sock = None
        self.wlock = threading.Lock()
        self.rlock = threading.Lock()

    @property
    def connected(self) -> bool:
        return self.sock is not None

    def connect(self, timeout: float = 2.0) -> bool:
        if self.sock is not None:
            return True
        try:
            self.sock = socket.create_connection((self.host, self.port), timeout=timeout)
        except OSError:
            self.sock = None
            return False
        return True

    def send(self, obj: dict) -> bool:
        data = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        packed = struct.pack("<I", len(data)) + data
        with self.wlock:
            s = self.sock
            if s is None:
                return False
            try:
                s.sendall(packed)
            except OSError:
                return False
            return True

    def recv_frame(self):
        with self.rlock:
            s = self.sock
            if s is None:
                return None
            head = self._recv_exact(s, 4)
            if head is None:
                return None
            n = struct.unpack("<I", head)[0]
            if n == 0 or n > (1 << 20):
                return None
            return self._recv_exact(s, n)

    @staticmethod
    def _recv_exact(s, n: int):
        buf = bytearray()
        while len(buf) < n:
            try:
                chunk = s.recv(n - len(buf))
            except OSError:
                return None
            if not chunk:
                return None
            buf += chunk
        return bytes(buf)

    def close(self) -> None:
        # 必须先 shutdown 再 close：否则读取线程正持着 rlock 阻塞在 recv，
        # 这里再去抢 rlock 就会死锁。shutdown 会让阻塞的 recv 立即返回。
        s = self.sock
        if s is None:
            return
        try:
            s.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        with self.wlock, self.rlock:
            self.sock = None
            try:
                s.close()
            except OSError:
                pass


class EngineSession:
    """一次「推送脚本 + 收执行回包」的会话。

    职责：连接、后台发送 load+play、后台读取回包、看门狗计时。
    回包以 (kind, text) 放入 messages 队列，由 UI 线程 drain() 消费：
        (MSG_LOG, 文本)    执行日志
        (MSG_DONE, None)   一轮结束（done/stopped 或连接断开）
        (MSG_ERROR, 文本)  执行错误

    典型用法（UI 线程）：
        if not session.connect(): 报错
        session.start(events, loop)
        # 之后在 after() 轮询里：
        while (m := session.drain()) is not None: 处理
        if session.timed_out(): 收尾
    """

    def __init__(self, client: EngineClient, timeout: float = RUN_TIMEOUT):
        self.client = client
        self.timeout = timeout
        self.messages: queue.Queue = queue.Queue()
        self._running = False
        self._started_at = 0.0

    @property
    def running(self) -> bool:
        return self._running

    def connect(self, timeout: float = 2.0) -> bool:
        return self.client.connect(timeout)

    def start(self, events: list[dict], loop: int) -> None:
        """连接成功后调用：起读回包线程 + 后台发送 load/play。"""
        self._drain_stale()
        self._running = True
        self._started_at = time.time()
        threading.Thread(target=self._read_loop, daemon=True).start()
        threading.Thread(target=self._send_script, args=(events, loop), daemon=True).start()

    def stop(self) -> None:
        """请求停止（后台发送，避免应用卡死时阻塞 UI 线程）。"""
        threading.Thread(target=lambda: self.client.send({"cmd": "stop"}), daemon=True).start()

    def timed_out(self) -> bool:
        return self._running and (time.time() - self._started_at) > self.timeout

    def finish(self) -> None:
        self._running = False

    def close(self) -> None:
        self.client.close()

    def drain(self):
        """取一条待处理消息，返回 (kind, text) 或 None。"""
        try:
            return self.messages.get_nowait()
        except queue.Empty:
            return None

    def _drain_stale(self) -> None:
        """丢弃上一轮残留的消息，避免旧 __DONE__ 让新一轮刚开始就收尾。"""
        while True:
            try:
                self.messages.get_nowait()
            except queue.Empty:
                return

    def _send_script(self, events: list[dict], loop: int) -> None:
        ok1 = self.client.send({"cmd": "load", "loop": loop, "events": events})
        ok2 = self.client.send({"cmd": "play"})
        self.messages.put((MSG_LOG, "sock: → load 发送=%s, play 发送=%s" % (ok1, ok2)))

    def _read_loop(self) -> None:
        while True:
            frame = self.client.recv_frame()
            if frame is None:
                self.messages.put((MSG_LOG, "sock: 连接已断开（应用可能已关闭）"))
                self.messages.put((MSG_DONE, None))
                self.client.close()
                break
            try:
                obj = json.loads(frame.decode("utf-8"))
            except Exception:
                continue
            t = obj.get("t")
            if t == "log":
                self.messages.put((MSG_LOG, str(obj.get("msg", ""))))
            elif t in ("done", "stopped"):
                self.messages.put((MSG_LOG, "script: %s" % t))
                self.messages.put((MSG_DONE, None))
                # 一轮结束即关闭连接，下次运行重新连，避免复用已卡死的连接
                self.client.close()
                break
            else:
                self.messages.put((MSG_LOG, str(obj)))