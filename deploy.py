#!/usr/bin/env python3
r"""
PlusPlugin 运行时部署脚本（把本地编译的 version.dll 更新到运行时主机）
流程：
  1) 校验本地 proxy\version.dll（build.py 产出）
  2) SSH 连接运行时主机，关闭宿主 TTVoice.exe（杀进程树，释放 dll 句柄）
  3) 上传到远端同目录临时文件，备份旧 version.dll 为 .bak（仅首次）后原子覆盖
  4) 远程启动 pplugin（可在配置区关闭）
说明：不编译。编译请先跑 build.py；插件（plugin\ 下）用 build_plugin.py。
依赖：pip install paramiko
"""
import os
import sys
import time

import paramiko

# ===================== 配置区（按需修改） =====================
HOST = "192.168.167.1"                      # 运行时主机（跑 TTVoice.exe 的机器）
USER = "111"
PASSWORD = os.getenv("VM_PASSWORD", "111")  # 密钥认证失败时兜底；优先走下面的 KEY_FILE
KEY_FILE = os.path.join(os.path.expanduser("~"), ".ssh", "id_ed25519")

REPO_ROOT = os.path.dirname(os.path.abspath(__file__))
PROJECT_DLL = os.path.join(REPO_ROOT, "proxy", "version.dll")  # 源（build.py 产出）
REMOTE_TTVOICE = "C:/Users/111/AppData/Roaming/TTVoice"        # 远端 %APPDATA%\TTVoice 兜底值（正常会自动探测）
START_AFTER_DEPLOY = True                 # 部署后是否远程启动 pplugin
TASK_NAME = "PlusPlugin_TTVoice"          # 交互启动用的计划任务名（SSH 会话非交互，GUI 起不来）
# =============================================================


def log(msg):
    print(f"[*] {msg}")


def decode_console(raw):
    """cl.exe / cmd 在中文 Windows 输出 CP936；优先按 CP936 解码，诊断不乱码。"""
    for enc in ("cp936", "utf-8"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("cp936", "replace")


def safe_print(text):
    """本地控制台编码不了某些字符时降级打印，别让部署流程崩掉。"""
    enc = sys.stdout.encoding or "utf-8"
    try:
        print(text)
    except UnicodeEncodeError:
        print(text.encode(enc, "replace").decode(enc, "replace"))


def ssh_connect():
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    key = KEY_FILE if os.path.isfile(KEY_FILE) else None
    log(f"连接运行时主机 {USER}@{HOST} ...（{'密钥' if key else '密码'}认证）")
    client.connect(HOST, username=USER, password=PASSWORD, key_filename=key, timeout=30)
    return client


def run_remote(client, cmd):
    log(f"远程执行: {cmd}")
    _, stdout, stderr = client.exec_command(cmd)
    out = decode_console(stdout.read())
    err = decode_console(stderr.read())
    rc = stdout.channel.recv_exit_status()
    if out.strip():
        safe_print(out)
    if err.strip():
        safe_print("[stderr] " + err)
    return rc


def remote_ttvoice_dir(client):
    r"""探测远端 %APPDATA%\TTVoice（sftp 用正斜杠）；探测失败退回配置值。"""
    _, stdout, _ = client.exec_command("cmd /c echo %APPDATA%")
    appdata = decode_console(stdout.read()).strip().strip('"')
    if not appdata or "%" in appdata:
        log(f"[WARN] 探测远端 APPDATA 失败，使用配置值 {REMOTE_TTVOICE}")
        return REMOTE_TTVOICE
    return appdata.replace("\\", "/") + "/TTVoice"


def sftp_makedirs(sftp, path):
    parts = [p for p in path.replace("\\", "/").split("/") if p]
    if not parts:
        return
    cur = parts[0]
    for p in parts[1:]:
        cur = f"{cur}/{p}"
        try:
            sftp.mkdir(cur)
        except IOError:
            pass  # already exists


def main():
    if not os.path.exists(PROJECT_DLL):
        log(f"未找到 {PROJECT_DLL}，请先跑 build.py 编译。")
        raise SystemExit(1)

    client = ssh_connect()
    sftp = client.open_sftp()

    runtime_dir = remote_ttvoice_dir(client)
    log(f"运行时目录: {runtime_dir}")
    win_dir = runtime_dir.replace("/", "\\")
    dst_dll = f"{runtime_dir}/version.dll"
    dst_bak = dst_dll + ".bak"
    tmp_dll = dst_dll + ".new"

    # 1) 关闭宿主 pplugin（杀整个进程树，确保释放 dll 句柄；没在跑时 taskkill 非零，忽略）
    log("关闭宿主 TTVoice.exe ...")
    run_remote(client, "taskkill /im TTVoice.exe /f /t")
    time.sleep(2)

    sftp_makedirs(sftp, runtime_dir)

    # 2) 先落到同目录临时文件（避免中途失败破坏原文件）
    log(f"上传 {PROJECT_DLL} -> {tmp_dll} ...")
    sftp.put(PROJECT_DLL, tmp_dll)

    # 3) 备份旧 dll（仅首次，远端还没有旧 dll 时跳过）
    try:
        sftp.stat(dst_dll)
        try:
            sftp.stat(dst_bak)
        except IOError:
            run_remote(client, f'cmd /c copy /y "{win_dir}\\version.dll" "{win_dir}\\version.dll.bak"')
            log(f"已备份原 dll -> {dst_bak}")
    except IOError:
        pass

    # 4) 原子覆盖；占用则杀进程重试
    for attempt in range(1, 6):
        rc = run_remote(client, f'cmd /c move /y "{win_dir}\\version.dll.new" "{win_dir}\\version.dll"')
        if rc == 0:
            log("覆盖成功。")
            break
        log(f"version.dll 被占用，重试 ({attempt}/5)...")
        run_remote(client, "taskkill /im TTVoice.exe /f /t")
        time.sleep(2)
    else:
        log("仍无法覆盖，请手动关闭占用 version.dll 的进程后重试。")
        log("诊断（在运行时主机执行）： tasklist /m version.dll")
        sftp.close()
        client.close()
        raise SystemExit(1)

    # 5) 远程启动：SSH 会话是非交互的，GUI 应用在里面秒退；
    #    借助计划任务（交互式令牌）把 TTVoice 拉到登录用户的控制台会话
    if START_AFTER_DEPLOY:
        log("远程启动 TTVoice.exe ...")
        run_remote(client, f'schtasks /create /tn {TASK_NAME} /tr "{win_dir}\\TTVoice.exe" /sc once /sd 2099/01/01 /st 00:00 /f')
        run_remote(client, f'schtasks /run /tn {TASK_NAME}')

    sftp.close()
    client.close()
    log("完成。")


if __name__ == "__main__":
    main()
