#!/usr/bin/env python3
"""
TTVoice 部署脚本
流程：
  1) 清空并重建虚拟机 C:\native
  2) 上传宿主 native 源码到虚拟机
  3) 远程编译 build.bat 生成 version.dll
  4) 关闭宿主 TTVoice.exe
  5) 从虚拟机拉回 version.dll 覆盖到宿主 TTVoice 目录（先备份为 .bak）
  6) 启动 TTVoice（可在配置区关闭）
依赖：pip install paramiko
"""
import os
import sys
import time
import shutil
import subprocess
import paramiko

# ===================== 配置区（按需修改） =====================
VM_HOST = "172.28.11.230"
VM_USER = "111"
VM_PASSWORD = os.getenv("VM_PASSWORD", "111")  # 优先读环境变量 VM_PASSWORD；未设置时默认 111（可改成无默认值强制从环境变量读取）
SRC_DIR = r"D:\AIProjects\TTVoice\native"  # 宿主源码根
VM_NATIVE_DIR = "C:/native"               # 虚拟机目标根（sftp 用正斜杠）
VM_BUILD_CMD = r"cmd /c C:\native\version_proxy\build.bat"
VM_DLL = "C:/native/version_proxy/version.dll"   # 编译产物
DST_DIR = r"C:\Users\111\AppData\Roaming\TTVoice"  # 宿主 TTVoice 程序目录
TTVOICE_EXE = os.path.join(DST_DIR, "TTVoice.exe")
START_AFTER_DEPLOY = True                 # 部署后是否自动启动 TTVoice
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


def run_remote(client, cmd):
    log(f"远程执行: {cmd}")
    stdin, stdout, stderr = client.exec_command(cmd)
    out = stdout.read().decode("utf-8", "ignore")
    err = stderr.read().decode("utf-8", "ignore")
    rc = stdout.channel.recv_exit_status()
    if out.strip():
        print(out)
    if err.strip():
        print("[stderr]", err)
    return rc, out, err


def sftp_upload(sftp, local_dir, remote_dir):
    for name in os.listdir(local_dir):
        lpath = os.path.join(local_dir, name)
        rpath = f"{remote_dir}/{name}"
        if os.path.isdir(lpath):
            try:
                sftp.mkdir(rpath)
            except IOError:
                pass
            sftp_upload(sftp, lpath, rpath)
        else:
            sftp.put(lpath, rpath)
            log(f"上传 {lpath} -> {rpath}")


def main():
    # 1) 清空并重建虚拟机 C:\native
    client = ssh_connect()
    run_remote(client, f"cmd /c rmdir /s /q {VM_NATIVE_DIR}")
    run_remote(client, f"cmd /c mkdir {VM_NATIVE_DIR}")

    # 2) 上传源码
    log("上传源码到虚拟机 ...")
    sftp = client.open_sftp()
    sftp_upload(sftp, SRC_DIR, VM_NATIVE_DIR)

    # 3) 远程编译
    rc, _, _ = run_remote(client, VM_BUILD_CMD)
    if rc != 0:
        log("编译失败，已停止。请检查上面的输出。")
        client.close()
        sys.exit(1)

    # 4) 校验产物
    try:
        sftp.stat(VM_DLL)
        log("编译产物 version.dll 已生成。")
    except IOError:
        log("未找到 version.dll，编译可能失败。")
        client.close()
        sys.exit(1)

    # 5) 关闭宿主 TTVoice（杀整个进程树，确保释放 dll 句柄）
    log("关闭宿主 TTVoice.exe ...")
    subprocess.run("taskkill /im TTVoice.exe /f /t", shell=True)
    time.sleep(2)

    # 确保目标目录存在
    os.makedirs(DST_DIR, exist_ok=True)

    # 6) 先下载到临时文件，再尝试覆盖（避免中途失败破坏原文件）
    dst_dll = os.path.join(DST_DIR, "version.dll")
    tmp_dll = dst_dll + ".new"
    log(f"下载 version.dll 到临时文件 {tmp_dll} ...")
    sftp.get(VM_DLL, tmp_dll)

    # 重试覆盖：应对文件被占用（进程未完全释放 / 杀软扫描锁）
    for attempt in range(1, 6):
        try:
            if os.path.exists(dst_dll):
                bak = dst_dll + ".bak"
                if not os.path.exists(bak):
                    shutil.copy2(dst_dll, bak)
                    log(f"已备份原 dll -> {bak}")
                os.replace(tmp_dll, dst_dll)
            else:
                os.replace(tmp_dll, dst_dll)
            log("覆盖成功。")
            break
        except PermissionError:
            log(f"version.dll 被占用，重试 ({attempt}/5)...")
            subprocess.run("taskkill /im TTVoice.exe /f /t", shell=True)
            time.sleep(2)
    else:
        log("仍无法覆盖，请手动关闭占用 version.dll 的进程后重试。")
        log("诊断：在宿主执行  tasklist /m version.dll")
        sftp.close()
        client.close()
        sys.exit(1)

    sftp.close()
    client.close()

    # 7) 启动
    if START_AFTER_DEPLOY:
        log(f"启动 {TTVOICE_EXE} ...")
        os.chdir(DST_DIR)
        subprocess.Popen([TTVOICE_EXE], shell=True)

    log("完成。")


if __name__ == "__main__":
    main()
