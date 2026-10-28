#!/usr/bin/env python3
r"""
PlusPlugin 框架编译脚本：编译 version.dll（proxy payload / 插件框架本体）。

旧版靠本机 MSVC + MASM（proxy\build.bat）。本版改为与 build_plugin.py 共用同一台
远端 mingw 编译机交叉编译，所以本机无需装 Visual Studio。

流程：
  1) 连远端 Linux 编译机；
  2) 上传 proxy\ 源码与头（dllmain.cpp / proxy_exports.h / exports.def）、
     framework\（*.inc）、sdk\（pplugin.h）到远端；
  3) 由 proxy_exports.h 的 PROXY_EXPORT_LIST 生成 GAS 版转发桩 stubs.S
     （mingw 能汇编，替代 MSVC 的 MASM stubs.asm，二者等价）；
  4) x86_64-w64-mingw32 交叉编译 + 链接 exports.def，产物 version.dll 落回 proxy\；
  5) 本机只编译不部署；更新到运行时主机请跑 deploy.py。

说明：框架依赖 MASM 转发桩，这里改用 GAS 生成等价桩，因此不再需要本机 MASM。
前置：pip install paramiko。
"""
import os
import re
import sys

import paramiko

# ===================== 配置区（按需修改） =====================
HOST = "192.168.167.128"                          # 远端 mingw 交叉编译机
USER = "n111"
PASSWORD = os.getenv("VM_PASSWORD", "n123456")     # 密钥认证失败时兜底；优先走下面的 KEY_FILE
KEY_FILE = os.path.join(os.path.expanduser("~"), ".ssh", "id_ed25519")

REPO_ROOT = os.path.dirname(os.path.abspath(__file__))
PROXY_DIR = os.path.join(REPO_ROOT, "proxy")
REMOTE_ROOT = "/home/n111/plusplugin-build"
REMOTE_PROXY = f"{REMOTE_ROOT}/proxy"
PRODUCT = os.path.join(PROXY_DIR, "version.dll")
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


def parse_export_names():
    """从 proxy_exports.h 的 PROXY_EXPORT_LIST(X) 解析导出名。"""
    path = os.path.join(PROXY_DIR, "proxy_exports.h")
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    names = re.findall(r'X\(\s*([A-Za-z_]\w*)\s*\)', text)
    if not names:
        log("无法从 proxy_exports.h 解析导出名")
        raise SystemExit(1)
    return names


def gen_stubs_s(names):
    """生成 GAS 版转发桩（intel 语法），等价于 stubs.asm：jmp [rip+g_pfn_X]。"""
    lines = [
        "# auto-generated from proxy_exports.h by build.py (GAS / mingw, 替代 MASM stubs.asm)",
        ".intel_syntax noprefix",
        ".text",
        "",
    ]
    for n in names:
        lines.append(f".globl g_pfn_{n}")
    lines.append("")
    for n in names:
        lines.append(f".globl {n}")
    lines.append("")
    for n in names:
        lines.append(f"{n}:")
        lines.append(f"    jmp [rip + g_pfn_{n}]")
        lines.append("")
    return "\n".join(lines)


def build(client):
    names = parse_export_names()
    log(f"导出名 {len(names)} 个，生成 GAS 桩")
    stubs = gen_stubs_s(names)

    client.exec_command(f"mkdir -p {REMOTE_PROXY} {REMOTE_ROOT}/framework {REMOTE_ROOT}/sdk")
    sftp = client.open_sftp()
    try:
        for f in ("dllmain.cpp", "proxy_exports.h", "exports.def", "log.h", "log.cpp",
                  "config.h", "config.cpp", "bootstrap.h", "bootstrap.cpp",
                  "plugin_api.inc", "plugin_loader.inc"):
            sftp.put(os.path.join(PROXY_DIR, f), f"{REMOTE_PROXY}/{f}")
        for base, remote in (("framework", f"{REMOTE_ROOT}/framework"),
                             ("sdk", f"{REMOTE_ROOT}/sdk")):
            local = os.path.join(REPO_ROOT, base)
            for name in os.listdir(local):
                p = os.path.join(local, name)
                if os.path.isfile(p):
                    sftp.put(p, f"{remote}/{name}")
        with sftp.open(f"{REMOTE_PROXY}/stubs.S", "w") as fh:
            fh.write(stubs.encode("utf-8"))
        log(f"已上传源码 / framework / sdk / stubs.S -> {REMOTE_PROXY}")
    finally:
        sftp.close()

    # 去掉 CRLF（Linux 下 GAS 不喜欢），汇编桩，再用 mingw 链接 exports.def。
    cmd = (
        f"cd {REMOTE_PROXY} && for f in *.cpp *.S; do [ -e \"$f\" ] && sed -i 's/\\r$//' \"$f\"; done && "
        f"x86_64-w64-mingw32-as -o stubs.o stubs.S && "
        f"x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared -static "
        f"-static-libgcc -static-libstdc++ "
        f"-I{REMOTE_ROOT} -o version.dll *.cpp stubs.o exports.def -luser32"
    )
    status, out = run_status(client, cmd)
    if out.strip():
        log(out.strip())
    if status != 0:
        log("version.dll 远端编译失败（exit %d）" % status)
        return False

    sftp = client.open_sftp()
    try:
        sftp.get(f"{REMOTE_PROXY}/version.dll", PRODUCT)
    finally:
        sftp.close()
    log(f"已下载 {PRODUCT}")
    return True


def build_framework(client):
    """编译 framework.dll：CivetWeb 用 gcc 单独编 .c，再与 framework.cpp 链接。

    与 version.dll 同机交叉编译；framework.dll 是 version.dll 经 [loaders]
    直接 LoadLibrary 的独立 DLL（不是 pplugin 插件），所以这里单独编一遍。
    """
    log("=== framework.dll（CivetWeb + 裸 TCP/WS 服务端）===")
    remote_fw = f"{REMOTE_ROOT}/framework"
    client.exec_command(
        f"mkdir -p {remote_fw} {REMOTE_ROOT}/sdk "
        f"{REMOTE_ROOT}/libs/civetweb {REMOTE_ROOT}/libs/nlohmann")
    sftp = client.open_sftp()
    try:
        # framework 目录下的源文件全部上传（framework.cpp + picker.cpp 等）。
        for name in os.listdir(os.path.join(REPO_ROOT, "framework")):
            p = os.path.join(REPO_ROOT, "framework", name)
            if os.path.isfile(p) and (name.endswith(".cpp") or name.endswith(".h")):
                sftp.put(p, f"{remote_fw}/{name}")
        sftp.put(os.path.join(REPO_ROOT, "sdk", "pplugin.h"),
                 f"{REMOTE_ROOT}/sdk/pplugin.h")
        sftp.put(os.path.join(REPO_ROOT, "libs", "nlohmann", "json.hpp"),
                 f"{REMOTE_ROOT}/libs/nlohmann/json.hpp")
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
        f"x86_64-w64-mingw32-gcc -c -O2 -DNO_SSL -DUSE_WEBSOCKET "
        f"../libs/civetweb/civetweb.c -o civetweb.o && "
        f"x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared -static "
        f"-static-libgcc -static-libstdc++ "
        f"-I../sdk -I../libs -I../libs/civetweb "
        f"-o framework.dll framework.cpp picker.cpp civetweb.o "
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
        if not build(client):
            raise SystemExit(1)
        if not build_framework(client):
            raise SystemExit(1)
    finally:
        client.close()
    log("完成。更新到运行时主机请跑 deploy.py。")


if __name__ == "__main__":
    main()
