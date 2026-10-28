#!/usr/bin/env python3
r"""
framework_build.py - 编译 framework.dll（CivetWeb + 裸 TCP/WS 服务端）。

与 version.dll 同机（远端 Linux mingw）交叉编译；framework.dll 由 version.dll 的
bootstrap 在进程启动时直接 LoadLibrary 加载（入口 NotifyPlatform，JSON 握手），
所以单独编一遍。

流程：
  1) 连远端 Linux 编译机；
  2) 上传 framework\ 下全部 *.cpp/*.h、common\（json/log）、libs\civetweb；
  3) CivetWeb 用 gcc 单独编 .o，再与 framework 源文件链接成 framework.dll；
  4) 产物 framework.dll 落回 framework\。

前置：pip install paramiko。
"""
import os
import sys

import paramiko

# ===================== 配置区（按需修改） =====================
HOST = "192.168.167.128"                          # 远端 mingw 交叉编译机
USER = "n111"
PASSWORD = os.getenv("VM_PASSWORD", "n123456")     # 密钥认证失败时兜底；优先走下面的 KEY_FILE
KEY_FILE = os.path.join(os.path.expanduser("~"), ".ssh", "id_ed25519")

REPO_ROOT = os.path.dirname(os.path.abspath(__file__))
REMOTE_ROOT = "/home/n111/plusplugin-build"
FRAMEWORK_DLL = os.path.join(REPO_ROOT, "framework", "framework.dll")
# =============================================================


def log(msg):
    print(f"[*] {msg}")


def decode_console(raw):
    for enc in ("cp936", "utf-8"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("cp936", "replace")


def ssh_connect():
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    key = KEY_FILE if os.path.isfile(KEY_FILE) else None
    log(f"连接编译机 {USER}@{HOST} ...（{'密钥' if key else '密码'}认证）")
    client.connect(HOST, username=USER, password=PASSWORD, key_filename=key, timeout=30)
    return client


def run_status(client, cmd):
    _in, out, err = client.exec_command(cmd)
    o = decode_console(out.read())
    e = decode_console(err.read())
    status = out.channel.recv_exit_status()
    return status, (o + e)


def framework_build(client):
    """编译 framework.dll：CivetWeb 用 gcc 单独编 .c，再与 framework.cpp 链接。"""
    log("=== framework.dll（CivetWeb + 裸 TCP/WS 服务端）===")
    remote_fw = f"{REMOTE_ROOT}/framework"
    client.exec_command(
        f"mkdir -p {remote_fw} {REMOTE_ROOT}/common "
        f"{REMOTE_ROOT}/libs/civetweb && rm -f {REMOTE_ROOT}/common/* "
        f"{remote_fw}/*.cpp {remote_fw}/*.h")
    sftp = client.open_sftp()
    try:
        # framework 目录下的源文件全部上传（framework.cpp + picker.cpp 等）。
        for name in os.listdir(os.path.join(REPO_ROOT, "framework")):
            p = os.path.join(REPO_ROOT, "framework", name)
            if os.path.isfile(p) and (name.endswith(".cpp") or name.endswith(".h")):
                sftp.put(p, f"{remote_fw}/{name}")
        # 共享模块 common/：framework 里 #include "../common/json.h" / log.h。
        for name in os.listdir(os.path.join(REPO_ROOT, "common")):
            p = os.path.join(REPO_ROOT, "common", name)
            if os.path.isfile(p) and (name.endswith(".cpp") or name.endswith(".h")):
                sftp.put(p, f"{REMOTE_ROOT}/common/{name}")
        # CivetWeb 是单文件 C，但 civetweb.c 会 #include 多个 .inl（md5/sha1/sort/
        # match/response/timer/handle_form 等），整目录上传最稳妥。
        civet_dir = os.path.join(REPO_ROOT, "libs", "civetweb")
        remote_civet = f"{REMOTE_ROOT}/libs/civetweb"
        for name in os.listdir(civet_dir):
            p = os.path.join(civet_dir, name)
            if os.path.isfile(p):
                sftp.put(p, f"{remote_civet}/{name}")
    finally:
        sftp.close()

    # CivetWeb 是 C 文件，用 gcc 单独编；-DNO_SSL 避开 OpenSSL，-DUSE_WEBSOCKET
    # 把 mg_set_websocket_handler / mg_websocket_write 编进库（否则链接报未定义）。
    cmd = (
        f"cd {remote_fw} && "
        f"x86_64-w64-mingw32-gcc -c -Os -ffunction-sections -fdata-sections "
        f"-DNO_SSL -DUSE_WEBSOCKET "
        f"../libs/civetweb/civetweb.c -o civetweb.o && "
        f"x86_64-w64-mingw32-g++ -std=c++17 -Os -shared -static "
        f"-ffunction-sections -fdata-sections "
        f"-static-libgcc -static-libstdc++ "
        f"-Wl,--gc-sections -s "
        f"-I../common -I../libs -I../libs/civetweb "
        f"-o framework.dll *.cpp ../common/*.cpp civetweb.o "
        f"-lws2_32 -luser32 -lgdi32 -ladvapi32 -lwinmm -lpthread"
    )
    status, out = run_status(client, cmd)
    if out.strip():
        log(out.strip())
    if status != 0:
        log("framework.dll 远端编译失败（exit %d）" % status)
        return False

    sftp = client.open_sftp()
    try:
        sftp.get(f"{remote_fw}/framework.dll", FRAMEWORK_DLL)
    finally:
        sftp.close()
    log(f"已下载 {FRAMEWORK_DLL}")
    return True


def main():
    client = ssh_connect()
    try:
        if not framework_build(client):
            raise SystemExit(1)
    finally:
        client.close()
    log("完成。更新到运行时主机请跑 deploy.py。")


if __name__ == "__main__":
    main()