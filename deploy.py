#!/usr/bin/env python3
r"""
TTVoice 运行时部署脚本（把项目里的 version.dll 装到运行时目录）
流程：
  1) 关闭宿主 TTVoice.exe（杀进程树，释放 dll 句柄）
  2) 备份运行时旧 version.dll 为 .bak（若尚无备份）
  3) 用项目 version_proxy\version.dll 覆盖运行时 version.dll（占用重试）
  4) 启动 TTVoice（可在配置区关闭）
说明：不编译、不连 VM。编译 + 回填项目请先跑 build.py。
"""
import os
import time
import shutil
import subprocess

# ===================== 配置区（按需修改） =====================
REPO_ROOT = r"D:\AIProjects\TTVoice"
PROJECT_DLL = os.path.join(REPO_ROOT, "version_proxy", "version.dll")  # 源（build.py 产出）
DST_DIR = r"C:\Users\111\AppData\Roaming\TTVoice"                      # 宿主 TTVoice 运行时目录
TTVOICE_EXE = os.path.join(DST_DIR, "TTVoice.exe")
START_AFTER_DEPLOY = True                               # 部署后是否自动启动 TTVoice
# =============================================================


def log(msg):
    print(f"[*] {msg}")


def main():
    if not os.path.exists(PROJECT_DLL):
        log(f"未找到 {PROJECT_DLL}，请先跑 build.py 编译并回填项目。")
        raise SystemExit(1)

    # 1) 关闭宿主 TTVoice（杀整个进程树，确保释放 dll 句柄）
    log("关闭宿主 TTVoice.exe ...")
    subprocess.run("taskkill /im TTVoice.exe /f /t", shell=True)
    time.sleep(2)

    os.makedirs(DST_DIR, exist_ok=True)
    dst_dll = os.path.join(DST_DIR, "version.dll")
    tmp_dll = dst_dll + ".new"

    # 2) 先落到同目录临时文件（避免中途失败破坏原文件）
    log(f"复制 {PROJECT_DLL} -> {tmp_dll} ...")
    shutil.copy2(PROJECT_DLL, tmp_dll)

    # 3) 备份旧 dll 并原子覆盖；占用则重试
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
        raise SystemExit(1)

    # 4) 启动
    if START_AFTER_DEPLOY:
        log(f"启动 {TTVOICE_EXE} ...")
        os.chdir(DST_DIR)
        subprocess.Popen([TTVOICE_EXE], shell=True)

    log("完成。")


if __name__ == "__main__":
    main()
