#!/usr/bin/env python3
r"""
PlusPlugin 插件：本地编译 + 安装到运行时主机

用法：
  python build_plugin.py                      # 默认编 script-engine 插件
  python build_plugin.py script-engine
  python build_plugin.py console
  python build_plugin.py script-engine console  # 依次编多个
  python build_plugin.py script-engine --no-build  # 跳过本地编译，直接用已有 dll

流程（每个插件）：
  1) 本地跑 plugin\<名称>\build_plugin.bat（bat 自带 vswhere→vcvars64），
     产物 plugin\<名称>\<名称>.dll 直接落在源码目录
  2) 上传 dll + plugin.json 到运行时主机 %APPDATA%\TTVoice\plugins\<名称>\
     应用在跑时走框架热重载（enabled=false 卸载 → 覆盖 → enabled=true 加载）；
     全新插件目录不会被运行中的框架发现，装完需重启一次 pplugin。

说明：只负责「编译 + 装插件」；version.dll 框架本体由 build.py 编译、deploy.py 部署。
前置：script-engine 在远端 Linux 上用 mingw 交叉编译（本机无需 MSVC）。
依赖：pip install paramiko（连接编译/运行时主机用）
"""
import glob
import json
import os
import shutil
import subprocess
import sys
import time

import paramiko

# ===================== 配置区（按需修改） =====================
HOST = "192.168.167.128"                    # 运行时主机（跑 TTVoice.exe 的机器）
USER = "n111"
PASSWORD = os.getenv("VM_PASSWORD", "n123456")  # 密钥认证失败时兜底；优先走下面的 KEY_FILE
KEY_FILE = os.path.join(os.path.expanduser("~"), ".ssh", "id_ed25519")

REPO_ROOT = os.path.dirname(os.path.abspath(__file__))
REMOTE_TTVOICE = "C:/Users/111/AppData/Roaming/TTVoice"  # 远端 %APPDATA%\TTVoice 兜底值（正常会自动探测）

# 每个插件：源码相对目录、产物名、安装目录名、要清理的旧产物
PLUGINS = {
    "script-engine": {
        "dir": "plugin/script-engine",
        "dll": "script-engine.dll",
        "install": "script-engine",
        "library": True,
        "remote_build": "linux",
        "deploy": "local",
        "stale": (),
    },
    "console": {
        "dir": "plugin/console",
        "dll": "console.dll",
        "install": "console",
        "remote_build": "linux",
        "deploy": "local",
        "stale": ("log_console.dll",),
    },
}
DEFAULT_PLUGINS = ("script-engine",)
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
    """本地控制台编码不了某些字符时降级打印，别让流程崩掉。"""
    enc = sys.stdout.encoding or "utf-8"
    try:
        print(text)
    except UnicodeEncodeError:
        print(text.encode(enc, "replace").decode(enc, "replace"))


def run_bat(bat):
    """本地跑 bat（vswhere→vcvars64 自包含），转发输出并返回退出码。"""
    log(f"本地编译: {bat}")
    proc = subprocess.run([os.environ.get("ComSpec", "cmd.exe"), "/c", bat],
                          cwd=os.path.dirname(bat), capture_output=True)
    out = decode_console(proc.stdout)
    err = decode_console(proc.stderr)
    if out.strip():
        safe_print(out)
    if err.strip():
        safe_print("[stderr] " + err)
    return proc.returncode


def ssh_connect():
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    key = KEY_FILE if os.path.isfile(KEY_FILE) else None
    log(f"连接运行时主机 {USER}@{HOST} ...（{'密钥' if key else '密码'}认证）")
    client.connect(HOST, username=USER, password=PASSWORD, key_filename=key, timeout=30)
    return client


def exec_out(client, cmd):
    _, stdout, _ = client.exec_command(cmd)
    return decode_console(stdout.read())


def remote_ttvoice_dir(client):
    r"""探测远端 %APPDATA%\TTVoice（sftp 用正斜杠）；探测失败退回配置值。"""
    appdata = exec_out(client, "cmd /c echo %APPDATA%").strip().strip('"')
    if not appdata or "%" in appdata:
        log(f"[WARN] 探测远端 APPDATA 失败，使用配置值 {REMOTE_TTVOICE}")
        return REMOTE_TTVOICE
    return appdata.replace("\\", "/") + "/TTVoice"


def remote_is_windows(client):
    """远端能跑 cmd 才是 Windows；Linux 主机只编译、不部署。"""
    return "Windows" in exec_out(client, "cmd /c ver").strip()


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


def app_running(client):
    return "TTVoice.exe" in exec_out(
        client, 'tasklist /fi "imagename eq TTVoice.exe" /fo csv /nh')


def scanned_this_run(sftp, runtime_dir, name):
    """当前进程认不认识这个插件目录。

    框架只在 Start() 里 ScanPlugins 一次，之后只热重载已扫描到的槽位；日志里
    最后一次 "attached exe=" 之后有没有该目录的 "plugin: found ..." 就是答案。
    """
    try:
        st = sftp.stat(f"{runtime_dir}/proxy.log")
        with sftp.open(f"{runtime_dir}/proxy.log", "rb") as fh:
            if st.st_size > 400000:
                fh.seek(-400000, 2)
            text = fh.read().decode("utf-8", errors="replace")
    except IOError:
        return False
    start = text.rfind("attached exe=")
    if start < 0:
        return False
    run = text[start:]
    return "plugin: found" in run and rf"\plugins\{name}" in run


def set_enabled(sftp, path, enabled):
    data = {}
    try:
        with sftp.open(path, "rb") as fh:
            data = json.loads(fh.read().decode("utf-8-sig", "replace"))
    except (IOError, ValueError):
        data = {"schema": 1, "enabled": bool(enabled)}
    data["enabled"] = bool(enabled)
    payload = (json.dumps(data, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    with sftp.open(path, "wb") as fh:
        fh.write(payload)


def wait_unlocked(sftp, path, timeout=6.0):
    """已映射的 dll 不能写打开；框架卸载后会释放（远端走 sftp 同样成立）。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with sftp.open(path, "r+b"):
                return True
        except IOError:
            time.sleep(0.2)
    try:
        sftp.stat(path)
        return False
    except IOError:
        return True  # 文件已不存在，视为可用


def install_library(client, sftp, runtime_dir, spec, src_dir):
    """纯执行库：只传 dll，不写清单、不走热重载。"""
    dst_dir = f"{runtime_dir}/plugins/{spec['install']}"
    dst_dll = f"{dst_dir}/{spec['dll']}"
    sftp_makedirs(sftp, dst_dir)
    if app_running(client) and not wait_unlocked(sftp, dst_dll):
        log(f"[WARN] 远端 {dst_dll} 仍被占用，请关闭 pplugin 后重跑")
        raise SystemExit(1)
    try:
        sftp.put(os.path.join(src_dir, spec["dll"]), dst_dll)
    except IOError as e:
        log(f"[WARN] 上传失败：{e}")
        raise SystemExit(1)
    log(f"安装 {dst_dll}")


def install(client, sftp, runtime_dir, spec):
    """上传到运行时主机 plugins\\<name>\\，应用在跑时用 enabled 开关热重载。"""
    src_dir = os.path.join(REPO_ROOT, *spec["dir"].split("/"))
    if spec.get("library"):
        install_library(client, sftp, runtime_dir, spec, src_dir)
        return
    dst_dir = f"{runtime_dir}/plugins/{spec['install']}"
    dst_dll = f"{dst_dir}/{spec['dll']}"
    dst_json = f"{dst_dir}/plugin.json"

    sftp_makedirs(sftp, dst_dir)
    hot = app_running(client)
    # The framework scans plugins\ only once, in Start(). A directory that
    # appeared after that is unknown to the running process, so there is nothing
    # to hot reload: the app has to be restarted before the plugin exists at all.
    scanned = hot and scanned_this_run(sftp, runtime_dir, spec["install"])

    if scanned:
        log("pplugin 正在运行且已扫描到该插件：先写 enabled=false 让框架卸载旧版本")
        set_enabled(sftp, dst_json, False)
        for stale in spec["stale"]:
            wait_unlocked(sftp, f"{dst_dir}/{stale}", timeout=3.0)
        if not wait_unlocked(sftp, dst_dll):
            log(f"[WARN] 远端 {dst_dll} 仍被占用，请关闭 pplugin 后重跑")
            raise SystemExit(1)

    try:
        sftp.put(os.path.join(src_dir, spec["dll"]), dst_dll)
        # 源 plugin.json 里 enabled=true，覆盖回去即触发框架重新加载
        sftp.put(os.path.join(src_dir, "plugin.json"), dst_json)
        for stale in spec["stale"]:
            p = f"{dst_dir}/{stale}"
            try:
                sftp.remove(p)
                log(f"删除旧产物 {p}")
            except IOError:
                pass  # 不存在
    except IOError as e:
        log(f"[WARN] 上传失败（文件可能被占用）：{e}")
        log("请先关闭远端 TTVoice.exe 后重跑")
        raise SystemExit(1)

    log(f"安装 {dst_dll}")
    if not hot:
        log("pplugin 未运行，下次启动时加载")
    elif scanned:
        log("已覆盖，框架约 1 秒内自动加载新版本（日志看 plugin: ... hot reloading）")
    else:
        log("[!] 该插件目录不在当前进程的扫描结果里：重启一次 pplugin 才会加载")


LINUX_REMOTE_ROOT = "/home/n111/plusplugin-build"


def run_status(client, cmd):
    """执行远端命令，返回 (exit_status, 合并输出)。"""
    _stdin, stdout, stderr = client.exec_command(cmd)
    out = decode_console(stdout.read())
    err = decode_console(stderr.read())
    status = stdout.channel.recv_exit_status()
    return status, (out + err)


def build_on_linux(client, spec):
    """把源码上传到 Linux，用 mingw 交叉编译，再把产物下载回源码目录。"""
    src_dir = os.path.join(REPO_ROOT, *spec["dir"].split("/"))
    sources = sorted(glob.glob(os.path.join(src_dir, "*.cpp")))
    if not sources:
        log(f"[{spec['install']}] 源码目录没有 .cpp")
        return False

    remote_dir = f"{LINUX_REMOTE_ROOT}/plugin/{spec['install']}"
    run_status(client, f"mkdir -p {remote_dir} {LINUX_REMOTE_ROOT}/sdk")

    sftp = client.open_sftp()
    try:
        for src in sources:
            sftp.put(src, f"{remote_dir}/{os.path.basename(src)}")
        headers = sorted(glob.glob(os.path.join(src_dir, "*.h")))
        for hdr in headers:
            sftp.put(hdr, f"{remote_dir}/{os.path.basename(hdr)}")
        sdk_header = os.path.join(REPO_ROOT, "sdk", "pplugin.h")
        if os.path.isfile(sdk_header):
            sftp.put(sdk_header, f"{LINUX_REMOTE_ROOT}/sdk/pplugin.h")
        # 第三方单头文件依赖（如 nlohmann/json）：源码里按 ../../libs/... 相对包含，
        # 远端必须保持同样的目录层级，否则编译找不到头文件。
        libs_dir = os.path.join(REPO_ROOT, "libs")
        if os.path.isdir(libs_dir):
            for base, _dirs, files in os.walk(libs_dir):
                rel = os.path.relpath(base, REPO_ROOT).replace("\\", "/")
                run_status(client, f"mkdir -p {LINUX_REMOTE_ROOT}/{rel}")
                for name in files:
                    sftp.put(os.path.join(base, name), f"{LINUX_REMOTE_ROOT}/{rel}/{name}")
            log(f"[{spec['install']}] 已上传 libs -> {LINUX_REMOTE_ROOT}/libs")
        log(f"[{spec['install']}] 已上传源文件/头文件和 sdk/pplugin.h -> {remote_dir}")
    finally:
        sftp.close()

    names = " ".join(os.path.basename(s) for s in sources)
    cmd = (
        f"cd {remote_dir} && for f in *.cpp *.h; do [ -e \"$f\" ] && sed -i 's/\\r$//' \"$f\"; done; "
        f"x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -shared -static -static-libgcc -static-libstdc++ "
        f"-o {spec['dll']} {names} -luser32 -lgdi32 -lws2_32 -ld2d1 -ldwrite -lole32 -loleaut32 -luuid"
    )
    status, out = run_status(client, cmd)
    if out.strip():
        log(out.strip())
    if status != 0:
        log(f"[{spec['install']}] Linux 编译失败（exit {status}）")
        return False

    local_dll = os.path.join(src_dir, spec["dll"])
    sftp = client.open_sftp()
    try:
        sftp.get(f"{remote_dir}/{spec['dll']}", local_dll)
    finally:
        sftp.close()
    log(f"[{spec['install']}] 已下载 {local_dll}")
    return True


APP_EXE = "TTVoice.exe"


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


def stop_editor():
    """关闭正在运行的 script_editor.py：它 ctypes 加载了 dll，会锁住文件。"""
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
            log("已关闭 script_editor.py（它加载了 dll，会锁住文件）")
            time.sleep(1.0)
            return True
    except (OSError, subprocess.TimeoutExpired):
        pass
    return False


def deploy_local(spec):
    """把产物和 plugin.json 复制到本机应用的 %APPDATA%\\TTVoice\\plugins\\<install>\\。"""
    src_dir = os.path.join(REPO_ROOT, *spec["dir"].split("/"))
    src = os.path.join(src_dir, spec["dll"])
    appdata = os.environ.get("APPDATA", "")
    if not appdata:
        log("[WARN] 本机没有 APPDATA，跳过本地部署")
        return False
    dst_dir = os.path.join(appdata, "TTVoice", "plugins", spec["install"])
    dst = os.path.join(dst_dir, spec["dll"])
    stop_app()
    stop_editor()
    try:
        os.makedirs(dst_dir, exist_ok=True)
        for attempt in range(3):
            try:
                shutil.copy2(src, dst)
                break
            except PermissionError:
                if attempt == 2:
                    raise
                time.sleep(1.0)
        manifest = os.path.join(src_dir, "plugin.json")
        if os.path.isfile(manifest):
            shutil.copy2(manifest, os.path.join(dst_dir, "plugin.json"))
            log(f"部署清单: {os.path.join(dst_dir, 'plugin.json')}")
    except OSError as exc:
        log(f"[WARN] 部署失败：{dst} 可能被占用（先关闭 script_editor.py / 应用）：{exc}")
        start_app()
        return False
    log(f"部署到应用: {dst}")
    start_app()
    return True


def build_one(client, sftp, runtime_dir, name, spec, no_build=False, deploy=True):
    log(f"=== 插件 {name}（{spec['dir']}）===")
    src_dir = os.path.join(REPO_ROOT, *spec["dir"].split("/"))
    dll = os.path.join(src_dir, spec["dll"])

    if not no_build:
        if spec.get("remote_build") == "linux":
            if not build_on_linux(client, spec) and not os.path.isfile(dll):
                return False
        else:
            bat = os.path.join(src_dir, "build_plugin.bat")
            if not os.path.isfile(bat):
                log(f"[{name}] 未找到 {bat}")
            elif run_bat(bat) != 0:
                if os.path.isfile(dll):
                    log(f"[{name}] 本地编译失败（本机可能没有 MSVC），改用已有 {spec['dll']}")
                else:
                    log(f"[{name}] 编译失败，且源码目录没有现成 {spec['dll']}")
                    return False
    else:
        log(f"[{name}] --no-build：跳过编译")

    if not os.path.isfile(dll):
        log(f"[{name}] 未找到产物 {dll}（先在 Linux 编译，或加 --no-build 使用已有产物）")
        return False

    if spec.get("deploy") == "local":
        return deploy_local(spec)
    if deploy:
        install(client, sftp, runtime_dir, spec)
    else:
        log(f"[{name}] 远端不是 Windows，跳过部署：{dll}")
    return True


def main():
    args = sys.argv[1:]
    no_build = "--no-build" in args
    args = [a for a in args if not a.startswith("--")]
    names = [a.lower() for a in args] or list(DEFAULT_PLUGINS)
    for name in names:
        if name not in PLUGINS:
            log(f"未知插件 {name}，可选：{', '.join(PLUGINS)}")
            raise SystemExit(2)

    client = ssh_connect()
    sftp = client.open_sftp()
    deploy = remote_is_windows(client)
    runtime_dir = remote_ttvoice_dir(client) if deploy else ""
    if deploy:
        log(f"运行时目录: {runtime_dir}")
    else:
        log(f"[INFO] 远端 {HOST} 不是 Windows：script-engine 部署到本机应用，其它插件跳过远端部署")

    failed = [n for n in names if not build_one(client, sftp, runtime_dir, n, PLUGINS[n],
                                                no_build, deploy)]

    sftp.close()
    client.close()
    if failed:
        log(f"失败插件：{', '.join(failed)}")
        raise SystemExit(1)
    log("完成。")


if __name__ == "__main__":
    main()
