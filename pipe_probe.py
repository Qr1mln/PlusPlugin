r"""独立探针：直接连 script-engine 的 TCP 端口，发 load+play，打印 DLL 回传的每一帧。
用于把「编辑器发送问题」和「DLL 处理问题」分开定位。
用法：先启动应用（TTVoice），再 python pipe_probe.py
（不再需要关掉 script_editor.py——TCP 不再像管道那样单实例独占）
"""
import json
import socket
import struct
import threading
import time

HOST = "127.0.0.1"
PORT = 47800


def connect(timeout: float = 3.0):
    try:
        return socket.create_connection((HOST, PORT), timeout=timeout)
    except OSError as e:
        print("[probe] 连接失败：", e)
        return None


s = connect()
if s is None:
    print("[probe] 连不上 %s:%d —— 应用没启动？script-engine 插件没加载？" % (HOST, PORT))
    raise SystemExit
print("[probe] connected %s:%d" % (HOST, PORT))


def send(obj):
    data = json.dumps(obj, ensure_ascii=False).encode("utf-8")
    packed = struct.pack("<I", len(data)) + data
    try:
        s.sendall(packed)
    except OSError as e:
        print("[probe] send failed:", e)
        return
    print("[probe] send", obj.get("cmd"), "bytes=%d" % len(packed))


def recv_exact(n: int):
    buf = bytearray()
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return bytes(buf)


def read_loop():
    while True:
        head = recv_exact(4)
        if head is None:
            print("[probe] read head failed -> 连接关闭/断开")
            break
        n = struct.unpack("<I", head)[0]
        if n == 0 or n > (1 << 20):
            print("[probe] bad length", n)
            break
        body = recv_exact(n)
        if body is None:
            print("[probe] read body failed")
            break
        print("[probe] RECV:", body.decode("utf-8", "replace"))


rt = threading.Thread(target=read_loop, daemon=True)
rt.start()

send({"cmd": "load", "loop": 1, "events": [
    {"type": "mouse", "action": "click", "button": "left", "x": 100, "y": 100, "remark": ""},
    {"type": "delay", "ms": 200, "remark": ""},
]})
send({"cmd": "play"})

time.sleep(5)
print("[probe] done waiting")