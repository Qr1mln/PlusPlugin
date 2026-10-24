#!/usr/bin/env python3
r"""
PlusPlugin console 插件 编译 + 回填项目脚本（在虚拟机上编译，产物拉回项目）
流程：
  1) 清空并重建虚拟机 C:\native
  2) 上传 console 插件源码（plugin/console/）+ 其头文件依赖 sdk/，保持同级
     （build_plugin.bat 用 /I %~dp0.. 让 "../sdk/pplugin.h" 解析到仓库根 sdk/）
  3) 远程编译 plugin/console/build_plugin.bat 生成 console.dll
  4) 把 console.dll 回填到项目 d:\PyProjects\PlusPlugin\plugin\console\
  5) 安装到宿主运行时 %APPDATA%\TTVoice\plugins\console
     pplugin 正在运行时走「热重载」：先把 plugin.json 的 enabled 改成 false
     让框架卸载旧 dll（释放文件锁），覆盖 dll 后再改回 true，框架会自动加载
     新版本，不需要重启应用。
说明：只负责「编译 + 回填 + 装插件」；version.dll 框架本身由 deploy.py 负责。
依赖：pip install paramiko
"""
import json
import os
import subprocess
import sys
import time
import shutil
import paramiko

# ===================== 配置区（按需修改） =====================
VM_HOST = "172.28.11.230"
VM_USER = "111"
VM_PASSWORD = os.getenv("VM_PASSWORD", "111")  # 优先读环境变量 VM_PASSWORD；未设置时默认 111

# 宿主仓库根
REPO_ROOT = r"D:\PyProjects\PlusPlugin"
# 上传到 VM_ROOT 下、保持同级的子目录：插件源码 + 头文件依赖 sdk/
SYNC_SUBDIRS = ("plugin/console", "sdk")

VM_ROOT = "C:/native"                                       # 虚拟机目标根（sftp 用正斜杠）
VM_BUILD_CMD = r"cmd /c C:\native\plugin\console\build_plugin.bat"
VM_DLL = "C:/native/plugin/console/console.dll"             # 编译产物

# 回填目标：项目内 plugin/console 目录
PROJECT_DIR = os.path.join(REPO_ROOT, "plugin", "console")
PROJECT_DLL = os.path.join(PROJECT_DIR, "console.dll")
STALE_NAMES = ("log_console.dll",)   # 旧产物名，安装时顺手清掉

# 宿主运行时安装目录：框架只扫描 plugins\ 下的子目录（一插件一目录）。
# 名字要与运行时的实际目录一致，否则每次编译会多建一个副本被重复加载。
INSTALL_DIR = os.path.join(os.environ.get("APPDATA", ""), "TTVoice", "plugins",
                           "console")
DLL_NAME = "console.dll"
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
    """把 VM 上的 console.dll 下载回填到项目 plugin/console/ 目录。"""
    os.makedirs(PROJECT_DIR, exist_ok=True)
    tmp = PROJECT_DLL + ".new"
    log(f"下载 console.dll 到项目 {PROJECT_DLL} ...")
    sftp.get(VM_DLL, tmp)
    os.replace(tmp, PROJECT_DLL)   # 原子覆盖
    log(f"已放到项目: {PROJECT_DLL}")


def app_running():
    out = subprocess.run(["tasklist", "/fi", "imagename eq TTVoice.exe", "/fo", "csv", "/nh"],
                         capture_output=True, text=True).stdout
    return "TTVoice.exe" in out


def set_enabled(path, enabled):
    """Rewrite only the enabled flag, keeping everything else in the manifest."""
    data = {}
    try:
        with open(path, "r", encoding="utf-8-sig") as f:
            data = json.load(f)
    except (OSError, ValueError):
        data = {"schema": 1, "name": "console", "version": "0.3.0",
                "priority": 5, "dll": "console.dll"}
    data["enabled"] = bool(enabled)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
        f.write("\n")


def wait_unlocked(path, timeout=8.0):
    """A mapped dll cannot be opened for writing. Poll until the framework has
    unloaded the plugin and released it."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(path, "r+b"):
                return True
        except OSError:
            time.sleep(0.2)
    return False


def install_to_runtime():
    """Install the built files. When the app is running this uses the framework's
    hot reload: enabled=false (framework unloads, releasing the dll lock), copy,
    enabled=true (framework loads the new build). The framework only scans
    subdirectories of plugins\\, so the files go into plugins\\console."""
    os.makedirs(INSTALL_DIR, exist_ok=True)
    dst_dll = os.path.join(INSTALL_DIR, DLL_NAME)
    dst_json = os.path.join(INSTALL_DIR, "plugin.json")

    hot = app_running()
    if hot:
        log("pplugin 正在运行：先写 enabled=false 让框架卸载旧版本")
        if not os.path.exists(dst_json):
            shutil.copy2(os.path.join(PROJECT_DIR, "plugin.json"), dst_json)
        set_enabled(dst_json, False)
        # 旧版本可能仍以旧文件名存在，一并等它释放
        for stale in STALE_NAMES:
            wait_unlocked(os.path.join(INSTALL_DIR, stale), timeout=3.0)
        if not wait_unlocked(dst_dll, timeout=6.0) and os.path.exists(dst_dll):
            log(f"[WARN] {dst_dll} 仍被占用，请关闭 pplugin 后重跑本脚本")
            raise SystemExit(1)
        log("旧 dll 已释放，开始覆盖")

    try:
        shutil.copy2(os.path.join(PROJECT_DIR, DLL_NAME), dst_dll)
        shutil.copy2(os.path.join(PROJECT_DIR, "plugin.json"), dst_json)
        for stale in STALE_NAMES:
            p = os.path.join(INSTALL_DIR, stale)
            if not os.path.exists(p):
                continue
            try:
                os.remove(p)
                log(f"删除旧产物 {p}")
            except OSError:
                log(f"[WARN] 旧产物 {p} 仍被占用，重启 pplugin 后可手动删除")
    except PermissionError:
        log(f"[WARN] {dst_dll} 被占用，请先关闭 TTVoice.exe 再重跑本脚本")
        raise SystemExit(1)

    log(f"安装 {dst_dll}")
    if hot:
        log("已覆盖，框架会在约 1 秒内自动加载新版本（日志里看 plugin: ... hot reloading）")
    else:
        log("pplugin 未运行，下次启动时加载")


def main():
    client = ssh_connect()

    # 1) 清空并重建虚拟机根（rmdir/mkdir 用反斜杠，避免 cmd 把正斜杠当开关）
    run_remote(client, r"cmd /c rmdir /s /q C:\native")
    run_remote(client, r"cmd /c mkdir C:\native")

    # 2) 上传源码子目录，保持 plugin/console 与 sdk 同级
    log("上传源码到虚拟机 ...")
    sftp = client.open_sftp()
    for sub in SYNC_SUBDIRS:
        local = os.path.join(REPO_ROOT, *sub.split("/"))
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
        log(f"编译产物 {DLL_NAME} 已生成。")
    except IOError:
        log(f"未找到 {DLL_NAME}，编译可能失败。")
        sftp.close()
        client.close()
        sys.exit(1)

    # 5) 回填到项目 plugin/console/ 目录
    pull_to_project(sftp)

    sftp.close()
    client.close()

    # 6) 装到宿主运行时（应用在跑就自动热重载）
    install_to_runtime()
    log("完成。")


if __name__ == "__main__":
    main()
