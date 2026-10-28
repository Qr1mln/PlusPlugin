#!/usr/bin/env python3
r"""
proxy_build.py - 编译 version.dll（proxy payload / 插件框架本体）。

流程：
  1) 连远端 Linux 编译机；
  2) 上传 proxy\ 下全部源码与头（*.cpp / *.h / *.inc / *.def），
     以及 framework\ 与 sdk\（bootstrap / plugin_*.inc 的 include 依赖）；
  3) 由 proxy_exports.h 的 PROXY_EXPORT_LIST 生成 GAS 版转发桩 stubs.S
     （mingw 能汇编，替代 MSVC 的 MASM stubs.asm，二者等价）；
  4) x86_64-w64-mingw32 交叉编译 + 链接 exports.def，产物 version.dll 落回 proxy\；
  5) 只编译不部署；更新到运行时主机请跑 deploy.py。

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
        "# auto-generated from proxy_exports.h by proxy_build.py (GAS / mingw)",
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


def proxy_build(client):
    names = parse_export_names()
    log(f"导出名 {len(names)} 个，生成 GAS 桩")
    stubs = gen_stubs_s(names)

    client.exec_command(f"mkdir -p {REMOTE_PROXY} {REMOTE_ROOT}/framework {REMOTE_ROOT}/sdk {REMOTE_ROOT}/common")
    sftp = client.open_sftp()
    try:
        # proxy 下全部源码/头/宏（遍历上传，自动包含 protocol/json_util 等新文件）。
        for name in sorted(os.listdir(PROXY_DIR)):
            if name.endswith((".cpp", ".h", ".inc", ".def")):
                sftp.put(os.path.join(PROXY_DIR, name), f"{REMOTE_PROXY}/{name}")
        # include 依赖：bootstrap/plugin_*.inc 需要 framework\ 与 sdk\。
        for base, remote in (("framework", f"{REMOTE_ROOT}/framework"),
                             ("sdk", f"{REMOTE_ROOT}/sdk"),
                             ("common", f"{REMOTE_ROOT}/common")):
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
    # 注意：远端 proxy/ 可能残留旧架构的 json_util.{cpp,h} 或旧版 log.{cpp,h}
    # （与 common/ 头不一致，会被 *.cpp 一并编入导致运行时访问违例 / 符号重定义）。
    # 先删掉它们，改用 common/ 的实现（json_util.cpp + log.cpp）。
    cmd = (
        f"cd {REMOTE_PROXY} && rm -f json_util.cpp json_util.h log.cpp log.h && "
        f"for f in *.cpp *.S; do [ -e \"$f\" ] && sed -i 's/\\r$//' \"$f\"; done && "
        f"x86_64-w64-mingw32-as -o stubs.o stubs.S && "
        f"x86_64-w64-mingw32-g++ -std=c++17 -Os -shared -static "
        f"-ffunction-sections -fdata-sections "
        f"-static-libgcc -static-libstdc++ "
        f"-Wl,--gc-sections -s "
        f"-I{REMOTE_ROOT} -I{REMOTE_ROOT}/common "
        f"-o version.dll *.cpp ../common/json_util.cpp ../common/log.cpp stubs.o exports.def -luser32"
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


def main():
    client = ssh_connect()
    try:
        if not proxy_build(client):
            raise SystemExit(1)
    finally:
        client.close()
    log("完成。更新到运行时主机请跑 deploy.py。")


if __name__ == "__main__":
    main()