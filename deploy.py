#!/usr/bin/env python3
r"""
PlusPlugin 运行时部署脚本（把本地编译的 version.dll 更新到本机运行时）

流程：
  1) 校验本地 proxy\version.dll（build.py 产出）
  2) 关闭本机宿主 TTVoice.exe（顺带关掉 script_editor.py，避免它持有管道会话）
  3) 备份旧 version.dll 为 .bak（仅首次）后覆盖
  4) 重新启动 TTVoice.exe

说明：
  - 不编译。编译请先跑 build.py；插件（plugin\ 下）用 build_plugin.py。
  - 与 build_plugin.py 的 deploy_local 保持一致：运行时（TTVoice）就跑在本机，
    因此直接在本机停/启 + 复制，不再 SSH 到远程 Windows（旧版是这么做的，已废弃）。
前置：本机需能 taskkill / 启动 TTVoice.exe，无需联网。
"""
import os
import shutil
import subprocess
import sys
import time

REPO_ROOT = os.path.dirname(os.path.abspath(__file__))
PROJECT_DLL = os.path.join(REPO_ROOT, "proxy", "version.dll")  # 源（build.py 产出）
START_AFTER_DEPLOY = True

APP_EXE = "TTVoice.exe"


def log(msg):
    print(f"[*] {msg}")


def app_exe_path():
    return os.path.join(os.environ.get("APPDATA", ""), "TTVoice", APP_EXE)


def stop_app():
    """部署前关闭本机应用，返回它之前是否在运行。"""
    try:
        result = subprocess.run(["taskkill", "/F", "/IM", APP_EXE, "/T"],
                                capture_output=True, text=True)
        if result.returncode == 0:
            log(f"已关闭 {APP_EXE}")
            time.sleep(1.5)
            return True
        log(f"{APP_EXE} 未在运行")
        return False
    except OSError as exc:
        log(f"[WARN] 关闭 {APP_EXE} 失败：{exc}")
        return False


def stop_editor():
    """关闭正在运行的 script_editor.py：部署期间避免它持有一个管道会话。"""
    script = (
        "$procs = Get-CimInstance Win32_Process | "
        "Where-Object { $_.CommandLine -like '*script_editor.py*' }; "
        "foreach ($p in $procs) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }; "
        "if ($procs) { Write-Output 'stopped' }"
    )
    try:
        result = subprocess.run(["powershell", "-NoProfile", "-Command", script],
                                capture_output=True, text=True, timeout=20)
        if "stopped" in result.stdout:
            log("已关闭 script_editor.py")
            time.sleep(1.0)
            return True
    except (OSError, subprocess.TimeoutExpired):
        pass
    return False


def start_app():
    """部署完成后重新启动本机应用。"""
    exe = app_exe_path()
    if not os.path.isfile(exe):
        log(f"[WARN] 找不到 {exe}，未重启应用")
        return False
    try:
        subprocess.Popen([exe], cwd=os.path.dirname(exe), close_fds=True)
        log(f"已重启应用: {exe}")
        return True
    except OSError as exc:
        log(f"[WARN] 重启应用失败：{exc}")
        return False


def main():
    if not os.path.isfile(PROJECT_DLL):
        log(f"未找到 {PROJECT_DLL}，请先跑 build.py 编译。")
        raise SystemExit(1)

    appdata = os.environ.get("APPDATA", "")
    if not appdata:
        log("[WARN] 本机没有 APPDATA，无法部署")
        raise SystemExit(1)

    runtime_dir = os.path.join(appdata, "TTVoice")
    dst = os.path.join(runtime_dir, "version.dll")
    bak = dst + ".bak"

    stop_app()
    stop_editor()

    os.makedirs(runtime_dir, exist_ok=True)

    # 备份旧 dll（仅首次）
    if os.path.isfile(dst) and not os.path.isfile(bak):
        try:
            shutil.copy2(dst, bak)
            log(f"已备份原 dll -> {bak}")
        except OSError as exc:
            log(f"[WARN] 备份失败：{exc}")

    # 覆盖（带重试应对占用）
    for attempt in range(5):
        try:
            shutil.copy2(PROJECT_DLL, dst)
            log(f"覆盖成功: {dst}")
            break
        except PermissionError:
            log(f"version.dll 被占用，重试 ({attempt + 1}/5)...")
            stop_app()
            time.sleep(2)
    else:
        log("仍无法覆盖，请手动关闭占用 version.dll 的进程后重试。")
        log("诊断（本机执行）： tasklist /m version.dll")
        raise SystemExit(1)

    if START_AFTER_DEPLOY:
        start_app()

    log("完成。")


if __name__ == "__main__":
    main()
