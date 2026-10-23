#!/usr/bin/env python3
r"""
TTVoice 编译 + 回填项目脚本（在虚拟机上编译，产物拉回项目 version_proxy/）
流程：
  1) 清空并重建虚拟机 C:\native
  2) 上传源码子目录（version_proxy/ framework/ sdk/）到虚拟机，保持同级
     （build.bat 用 /I %~dp0.. 找 framework/ 与 sdk/，故三者必须同级）
  3) 远程编译 build.bat 生成 version.dll
  4) 把 version.dll 回填到项目 D:\AIProjects\TTVoice\version_proxy\
说明：只负责「编译 + 回填项目」；要把 dll 装到运行时请另跑 deploy.py。
依赖：pip install paramiko
"""
import os
import sys
import paramiko

# ===================== 配置区（按需修改） =====================
VM_HOST = "172.28.11.230"
VM_USER = "111"
VM_PASSWORD = os.getenv("VM_PASSWORD", "111")  # 优先读环境变量 VM_PASSWORD；未设置时默认 111

# 宿主仓库根：源码平铺在仓库根下（version_proxy/ framework/ sdk/）
REPO_ROOT = r"D:\AIProjects\TTVoice"
# 编译 version.dll 需要的子目录，按原名上传到 VM_ROOT 下保持同级
SYNC_SUBDIRS = ("version_proxy", "framework", "sdk")

VM_ROOT = "C:/native"                           # 虚拟机目标根（sftp 用正斜杠）
VM_BUILD_CMD = r"cmd /c C:\native\version_proxy\build.bat"
VM_DLL = "C:/native/version_proxy/version.dll"  # 编译产物

# 回填目标：项目内 version_proxy 目录
PROJECT_DIR = os.path.join(REPO_ROOT, "version_proxy")
PROJECT_DLL = os.path.join(PROJECT_DIR, "version.dll")
# =============================================================


def log(msg):
    print(f"[*] {msg}")


def ssh_connect():
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    log(f"连接虚拟机 {VM_USER}@{VM_HOST} ...")
    client.connect(VM_HOST, username=VM_USER,
                   password=VM_PASSWORD, timeout=30)
    return client


def decode_console(raw):
    """cl.exe / cmd write CP936 on a Chinese Windows VM; decode that first so
    compiler diagnostics stay readable instead of turning into mojibake."""
    for enc in ("cp936", "utf-8"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("cp936", "replace")


def safe_print(text):
    """Never crash the build loop just because the local console codepage
    cannot encode a byte that came back from the VM."""
    enc = sys.stdout.encoding or "utf-8"
    try:
        print(text)
    except UnicodeEncodeError:
        print(text.encode(enc, "replace").decode(enc, "replace"))


def run_remote(client, cmd):
    log(f"远程执行: {cmd}")
    stdin, stdout, stderr = client.exec_command(cmd)
    out = decode_console(stdout.read())
    err = decode_console(stderr.read())
    rc = stdout.channel.recv_exit_status()
    if out.strip():
        safe_print(out)
    if err.strip():
        safe_print("[stderr] " + err)
    return rc, out, err


def sftp_makedirs(sftp, path):
    """Create path (and any missing parents) over SFTP, tolerating existing
    dirs. paramiko's mkdir does not create parents, and the VM's cmd mkdir
    output is locale-mangled, so build the tree over SFTP instead."""
    parts = [p for p in path.replace("\\", "/").split("/") if p]
    if not parts:
        return
    cur = parts[0]  # drive root like "C:"; do not mkdir it
    for p in parts[1:]:
        cur = f"{cur}/{p}"
        try:
            sftp.mkdir(cur)
        except IOError:
            pass  # already exists


def sftp_upload(sftp, local_dir, remote_dir):
    sftp_makedirs(sftp, remote_dir)   # ensure the target dir exists first
    for name in os.listdir(local_dir):
        lpath = os.path.join(local_dir, name)
        rpath = f"{remote_dir}/{name}"
        if os.path.isdir(lpath):
            sftp_makedirs(sftp, rpath)
            sftp_upload(sftp, lpath, rpath)
        else:
            sftp.put(lpath, rpath)
            log(f"上传 {lpath} -> {rpath}")


def pull_to_project(sftp):
    """把 VM 上的 version.dll 下载回填到项目 version_proxy/ 目录。"""
    os.makedirs(PROJECT_DIR, exist_ok=True)
    tmp = PROJECT_DLL + ".new"
    log(f"下载 version.dll 到项目 {PROJECT_DLL} ...")
    sftp.get(VM_DLL, tmp)
    os.replace(tmp, PROJECT_DLL)   # 原子覆盖
    log(f"已放到项目: {PROJECT_DLL}")


def main():
    client = ssh_connect()

    # 1) 清空并重建虚拟机根（rmdir/mkdir 用反斜杠，避免 cmd 把正斜杠当开关）
    run_remote(client, r"cmd /c rmdir /s /q C:\native")
    run_remote(client, r"cmd /c mkdir C:\native")

    # 2) 上传源码子目录，保持 version_proxy/framework/sdk 同级
    log("上传源码到虚拟机 ...")
    sftp = client.open_sftp()
    for sub in SYNC_SUBDIRS:
        local = os.path.join(REPO_ROOT, sub)
        if not os.path.isdir(local):
            log(f"[WARN] 本地缺少 {local}，跳过该目录")
            continue
        sftp_upload(sftp, local, f"{VM_ROOT}/{sub}")

    # 3) 远程编译
    rc, _, _ = run_remote(client, VM_BUILD_CMD)
    if rc != 0:
        log("编译失败，已停止。请检查上面的输出。")
        sftp.close()
        client.close()
        sys.exit(1)

    # 4) 校验产物
    try:
        sftp.stat(VM_DLL)
        log("编译产物 version.dll 已生成。")
    except IOError:
        log("未找到 version.dll，编译可能失败。")
        sftp.close()
        client.close()
        sys.exit(1)

    # 5) 回填到项目 version_proxy/ 目录
    pull_to_project(sftp)

    sftp.close()
    client.close()
    log("完成：已回填到项目。运行时部署请跑 deploy.py。")


if __name__ == "__main__":
    main()
