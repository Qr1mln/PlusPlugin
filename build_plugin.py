#!/usr/bin/env python3
r"""
PlusPlugin 插件：本地编译 + 安装到运行时主机

用法：
  python build_plugin.py                 # 默认编 script 插件
  python build_plugin.py script
  python build_plugin.py console
  python build_plugin.py script console  # 依次编多个

流程（每个插件）：
  1) 本地跑 plugin\<名称>\build_plugin.bat（bat 自带 vswhere→vcvars64），
     产物 plugin\<名称>\<名称>.dll 直接落在源码目录
  2) 上传 dll + plugin.json 到运行时主机 %APPDATA%\TTVoice\plugins\<名称>\
     应用在跑时走框架热重载（enabled=false 卸载 → 覆盖 → enabled=true 加载）；
     全新插件目录不会被运行中的框架发现，装完需重启一次 pplugin。

说明：只负责「编译 + 装插件」；version.dll 框架本体由 build.py 编译、deploy.py 部署。
前置：本机装有 Visual Studio Build Tools（MSVC x64 组件）。
依赖：pip install paramiko（连接运行时主机用）
"""
import json
import os
import subprocess
import sys
import time

import paramiko

# ===================== 配置区（按需修改） =====================
HOST = "192.168.167.1"                      # 运行时主机（跑 TTVoice.exe 的机器）
USER = "111"
PASSWORD = os.getenv("VM_PASSWORD", "111")  # 密钥认证失败时兜底；优先走下面的 KEY_FILE
KEY_FILE = os.path.join(os.path.expanduser("~"), ".ssh", "id_ed25519")

REPO_ROOT = os.path.dirname(os.path.abspath(__file__))
REMOTE_TTVOICE = "C:/Users/111/AppData/Roaming/TTVoice"  # 远端 %APPDATA%\TTVoice 兜底值（正常会自动探测）

# 每个插件：源码相对目录、产物名、安装目录名、要清理的旧产物
PLUGINS = {
    "script": {
        "dir": "plugin/script",
        "dll": "script.dll",
        "install": "script",
        "stale": (),
    },
    "console": {
        "dir": "plugin/console",
        "dll": "console.dll",
        "install": "console",
        "stale": ("log_console.dll",),
    },
}
DEFAULT_PLUGINS = ("script",)
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


def install(client, sftp, runtime_dir, spec):
    """上传到运行时主机 plugins\\<name>\\，应用在跑时用 enabled 开关热重载。"""
    src_dir = os.path.join(REPO_ROOT, *spec["dir"].split("/"))
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


def build_one(client, sftp, runtime_dir, name, spec):
    log(f"=== 插件 {name}（{spec['dir']}）===")
    src_dir = os.path.join(REPO_ROOT, *spec["dir"].split("/"))
    bat = os.path.join(src_dir, "build_plugin.bat")
    if not os.path.isfile(bat):
        log(f"[{name}] 未找到 {bat}")
        return False
    if run_bat(bat) != 0:
        log(f"[{name}] 编译失败。")
        return False
    dll = os.path.join(src_dir, spec["dll"])
    if not os.path.isfile(dll):
        log(f"[{name}] 未找到产物 {dll}")
        return False
    install(client, sftp, runtime_dir, spec)
    return True


def main():
    names = [a.lower() for a in sys.argv[1:]] or list(DEFAULT_PLUGINS)
    for name in names:
        if name not in PLUGINS:
            log(f"未知插件 {name}，可选：{', '.join(PLUGINS)}")
            raise SystemExit(2)

    client = ssh_connect()
    sftp = client.open_sftp()
    runtime_dir = remote_ttvoice_dir(client)
    log(f"运行时目录: {runtime_dir}")

    failed = [n for n in names if not build_one(client, sftp, runtime_dir, n, PLUGINS[n])]

    sftp.close()
    client.close()
    if failed:
        log(f"失败插件：{', '.join(failed)}")
        raise SystemExit(1)
    log("完成。")


if __name__ == "__main__":
    main()
