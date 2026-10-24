#!/usr/bin/env python3
r"""
PlusPlugin 插件：编译 + 回填项目 + 安装到运行时（在虚拟机上编译）

用法：
  python build_plugin.py                 # 默认编 script 插件
  python build_plugin.py script
  python build_plugin.py console
  python build_plugin.py script console  # 依次编多个

流程（每个插件）：
  1) 清空并重建虚拟机 C:\native（只做一次，逐个上传并集源码）
  2) 上传插件源码目录 + 依赖（sdk/；script 的引擎已并入自身目录，不再需要 proxy/）
  3) 远程跑插件目录下的 build_plugin.bat
  4) 把产物回填到项目 plugin\<名称>\
  5) 安装到 %APPDATA%\TTVoice\plugins\<名称>\
     应用在跑时走框架热重载（enabled=false 卸载 → 覆盖 → enabled=true 加载）；
     全新插件目录不会被运行中的框架发现，装完需重启一次 pplugin。

说明：只负责「编译 + 回填 + 装插件」；version.dll 框架本身由 deploy.py 负责。
依赖：pip install paramiko
"""
import json
import os
import shutil
import subprocess
import sys
import time

import paramiko

# ===================== 配置区（按需修改） =====================
VM_HOST = "172.20.106.42"
VM_USER = "111"
VM_PASSWORD = os.getenv("VM_PASSWORD", "111")

REPO_ROOT = r"D:\PyProjects\PlusPlugin"
VM_ROOT = "C:/native"
RUNTIME_PLUGINS = os.path.join(os.environ.get("APPDATA", ""), "TTVoice", "plugins")

# 每个插件：源码相对目录、产物名、安装目录名、额外的依赖子目录
PLUGINS = {
    "script": {
        "dir": "plugin/script",
        "dll": "script.dll",
        "install": "script",
        "deps": ("sdk",),           # 引擎 script_engine.inc 已移入 plugin/script/
        "stale": (),
    },
    "console": {
        "dir": "plugin/console",
        "dll": "console.dll",
        "install": "console",
        "deps": ("sdk",),
        "stale": ("log_console.dll",),
    },
}
DEFAULT_PLUGINS = ("script",)
# =============================================================


def log(msg):
    print(f"[*] {msg}")


def ssh_connect():
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    log(f"连接虚拟机 {VM_USER}@{VM_HOST} ...")
    client.connect(VM_HOST, username=VM_USER, password=VM_PASSWORD, timeout=30)
    return client


def decode_console(raw):
    for enc in ("cp936", "utf-8"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("cp936", "replace")


def safe_print(text):
    enc = sys.stdout.encoding or "utf-8"
    try:
        print(text)
    except UnicodeEncodeError:
        print(text.encode(enc, "replace").decode(enc, "replace"))


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
            pass


def sftp_upload(sftp, local_dir, remote_dir):
    sftp_makedirs(sftp, remote_dir)
    for name in os.listdir(local_dir):
        lpath = os.path.join(local_dir, name)
        rpath = f"{remote_dir}/{name}"
        if os.path.isdir(lpath):
            sftp_makedirs(sftp, rpath)
            sftp_upload(sftp, lpath, rpath)
        else:
            sftp.put(lpath, rpath)
            log(f"上传 {lpath} -> {rpath}")


def app_running():
    out = subprocess.run(["tasklist", "/fi", "imagename eq TTVoice.exe", "/fo", "csv", "/nh"],
                         capture_output=True, text=True).stdout
    return "TTVoice.exe" in out


def scanned_this_run(name):
    """当前进程认不认识这个插件目录。

    框架只在 Start() 里 ScanPlugins 一次，之后只热重载已扫描到的槽位；日志里
    最后一次 "attached exe=" 之后有没有该目录的 "plugin: found ..." 就是答案。
    """
    path = os.path.join(os.environ.get("APPDATA", ""), "TTVoice",
                        "proxy.log")
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()[-400000:]
    except OSError:
        return False
    start = text.rfind("attached exe=")
    if start < 0:
        return False
    run = text[start:]
    return "plugin: found" in run and f"\\plugins\\{name}" in run


def set_enabled(path, enabled):
    data = {}
    try:
        with open(path, "r", encoding="utf-8-sig") as fh:
            data = json.load(fh)
    except (OSError, ValueError):
        data = {"schema": 1, "enabled": bool(enabled)}
    data["enabled"] = bool(enabled)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(data, fh, ensure_ascii=False, indent=2)
        fh.write("\n")


def wait_unlocked(path, timeout=6.0):
    """已映射的 dll 不能写打开；框架卸载后会释放。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(path, "r+b"):
                return True
        except OSError:
            time.sleep(0.2)
    return not os.path.exists(path)


def install(spec):
    """装到 %APPDATA%\\TTVoice\\plugins\\<name>\\，应用在跑时用 enabled 开关热重载。"""
    src_dir = os.path.join(REPO_ROOT, *spec["dir"].split("/"))
    dst_dir = os.path.join(RUNTIME_PLUGINS, spec["install"])
    dst_dll = os.path.join(dst_dir, spec["dll"])
    dst_json = os.path.join(dst_dir, "plugin.json")

    os.makedirs(dst_dir, exist_ok=True)
    hot = app_running()
    # The framework scans plugins\ only once, in Start(). A directory that
    # appeared after that is unknown to the running process, so there is nothing
    # to hot reload: the app has to be restarted before the plugin exists at all.
    scanned = hot and scanned_this_run(spec["install"])

    if scanned:
        log("pplugin 正在运行且已扫描到该插件：先写 enabled=false 让框架卸载旧版本")
        set_enabled(dst_json, False)
        for stale in spec["stale"]:
            wait_unlocked(os.path.join(dst_dir, stale), timeout=3.0)
        if not wait_unlocked(dst_dll):
            log(f"[WARN] {dst_dll} 仍被占用，请关闭 pplugin 后重跑")
            raise SystemExit(1)

    try:
        shutil.copy2(os.path.join(src_dir, spec["dll"]), dst_dll)
        shutil.copy2(os.path.join(src_dir, "plugin.json"), dst_json)
        for stale in spec["stale"]:
            p = os.path.join(dst_dir, stale)
            if os.path.exists(p):
                try:
                    os.remove(p)
                    log(f"删除旧产物 {p}")
                except OSError:
                    log(f"[WARN] 旧产物 {p} 占用中")
    except PermissionError:
        log(f"[WARN] {dst_dll} 被占用，请先关闭 TTVoice.exe 再重跑")
        raise SystemExit(1)

    log(f"安装 {dst_dll}")
    if not hot:
        log("pplugin 未运行，下次启动时加载")
    elif scanned:
        log("已覆盖，框架约 1 秒内自动加载新版本（日志看 plugin: ... hot reloading）")
    else:
        log("[!] 该插件目录不在当前进程的扫描结果里：重启一次 pplugin 才会加载")


def build_one(client, sftp, name, spec):
    log(f"=== 插件 {name}（{spec['dir']}）===")
    rc = run_remote(client, rf"cmd /c C:\native\{spec['dir'].replace('/', chr(92))}\build_plugin.bat")
    if rc != 0:
        log(f"[{name}] 编译失败。")
        return False

    vm_dll = f"{VM_ROOT}/{spec['dir']}/{spec['dll']}"
    try:
        sftp.stat(vm_dll)
    except IOError:
        log(f"[{name}] 未找到产物 {vm_dll}")
        return False

    dst_dir = os.path.join(REPO_ROOT, *spec["dir"].split("/"))
    os.makedirs(dst_dir, exist_ok=True)
    dst = os.path.join(dst_dir, spec["dll"])
    tmp = dst + ".new"
    log(f"下载 {spec['dll']} 到项目 {dst}")
    sftp.get(vm_dll, tmp)
    os.replace(tmp, dst)
    install(spec)
    return True


def main():
    names = [a.lower() for a in sys.argv[1:]] or list(DEFAULT_PLUGINS)
    for name in names:
        if name not in PLUGINS:
            log(f"未知插件 {name}，可选：{', '.join(PLUGINS)}")
            raise SystemExit(2)

    client = ssh_connect()
    run_remote(client, r"cmd /c rmdir /s /q C:\native")
    run_remote(client, r"cmd /c mkdir C:\native")

    log("上传源码到虚拟机 ...")
    sftp = client.open_sftp()
    synced = set()
    for name in names:
        spec = PLUGINS[name]
        dirs = (spec["dir"],) + tuple(spec["deps"])
        for sub in dirs:
            if sub in synced:
                continue
            local = os.path.join(REPO_ROOT, *sub.split("/"))
            if not os.path.isdir(local):
                log(f"[WARN] 本地缺少 {local}，跳过")
                continue
            sftp_upload(sftp, local, f"{VM_ROOT}/{sub}")
            synced.add(sub)

    failed = []
    for name in names:
        if not build_one(client, sftp, name, PLUGINS[name]):
            failed.append(name)

    sftp.close()
    client.close()

    if failed:
        log(f"失败插件：{', '.join(failed)}")
        raise SystemExit(1)
    log("完成。")


if __name__ == "__main__":
    main()
