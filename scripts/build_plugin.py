#!/usr/bin/env python3
r"""
PlusPlugin 模块：交叉编译 + 部署到运行时目录

模块 = 导出 `NotifyPlatform` 的 dll，契约全是 JSON（见 proxy/module_loader.h）。
部署位置是 `%APPDATA%\TTVoice\<dll>`（与 framework.dll 同目录），由 version.ini
的 `[modules]` 列出后由 proxy 加载；换文件即触发 proxy 的热加载。

用法（在仓库根目录执行）：
  python scripts/build_plugin.py                      # 默认编 console
  python scripts/build_plugin.py console
  python scripts/build_plugin.py console --no-build   # 跳过编译，直接用已有 dll

流程（每个模块）：
  1) SSH 到 Linux 编译机，mingw 交叉编译 -> 产物落回 `plugin\<名称>\<名称>.dll`；
  2) 部署到本机 `%APPDATA%\TTVoice\<dll>`：应用在跑时走「改名换位」
     （dll -> dll.old，再写新 dll），proxy 的 watcher 会在 ~1s 内热加载；
     应用没跑就直接覆盖。

说明：version.dll 由 scripts/proxy_build.py 编译、deploy.py 部署；framework.dll 由
scripts/framework_build.py 编译、deploy_plugin.py 部署。
依赖：pip install paramiko。
"""
import glob
import os
import shutil
import subprocess
import sys

import paramiko

# ===================== 配置区（按需修改） =====================
HOST = "192.168.167.128"                        # Linux 交叉编译机
USER = "n111"
PASSWORD = os.getenv("VM_PASSWORD", "n123456")   # 密钥认证失败时兜底
KEY_FILE = os.path.join(os.path.expanduser("~"), ".ssh", "id_ed25519")
LINUX_REMOTE_ROOT = "/home/n111/plusplugin-build"

# 脚本放在 scripts/ 下，仓库根目录是它的上一级
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 每个模块：源码相对目录、产物名（部署名与产物名一致）
PLUGINS = {
    "console": {
        "dir": "plugin/console",
        "dll": "console.dll",
    },
}
DEFAULT_PLUGINS = ("console",)
APP_EXE = "TTVoice.exe"
# =============================================================


def log(msg):
    print(f"[*] {msg}")


def decode_console(raw):
    """远端 g++ / cmd 在中文 Windows 输出 CP936；优先按 CP936 解码，诊断不乱码。"""
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


def ssh_connect():
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    key = KEY_FILE if os.path.isfile(KEY_FILE) else None
    log(f"连接编译机 {USER}@{HOST} ...（{'密钥' if key else '密码'}认证）")
    client.connect(HOST, username=USER, password=PASSWORD, key_filename=key, timeout=30)
    return client


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
        log(f"[{spec['dll']}] 源码目录没有 .cpp")
        return False

    remote_dir = f"{LINUX_REMOTE_ROOT}/plugin/{spec['dll'].split('.')[0]}"
    # 远端先清掉旧源文件：本地删掉的文件否则会继续参与编译。
    run_status(client, f"mkdir -p {remote_dir} && rm -f {remote_dir}/*.cpp {remote_dir}/*.h")

    sftp = client.open_sftp()
    try:
        for src in sources:
            sftp.put(src, f"{remote_dir}/{os.path.basename(src)}")
        for hdr in sorted(glob.glob(os.path.join(src_dir, "*.h"))):
            sftp.put(hdr, f"{remote_dir}/{os.path.basename(hdr)}")
        log(f"[{spec['dll']}] 已上传源文件/头文件 -> {remote_dir}")
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
        log(f"[{spec['dll']}] Linux 编译失败（exit {status}）")
        return False

    local_dll = os.path.join(src_dir, spec["dll"])
    sftp = client.open_sftp()
    try:
        sftp.get(f"{remote_dir}/{spec['dll']}", local_dll)
    finally:
        sftp.close()
    log(f"[{spec['dll']}] 已下载 {local_dll}")
    return True


def app_exe_path():
    return os.path.join(os.environ.get("APPDATA", ""), "TTVoice", APP_EXE)


def app_running():
    """本机 TTVoice 是否在跑：在跑就得走「改名换位」，否则可以被覆盖。"""
    try:
        proc = subprocess.run(
            ["tasklist", "/fi", f"imagename eq {APP_EXE}", "/fo", "csv", "/nh"],
            capture_output=True, text=True)
    except OSError:
        return False
    return APP_EXE.lower() in (proc.stdout or "").lower()


def hot_replace(src_dll, dst_dll):
    """应用在跑时不能覆盖已映射的 dll：改成「改名换位」，proxy 的 watcher 会热加载。"""
    tmp = dst_dll + ".new"
    old = dst_dll + ".old"
    shutil.copy2(src_dll, tmp)
    try:
        if os.path.isfile(old):
            os.remove(old)
    except OSError:
        pass
    try:
        os.replace(dst_dll, old)
    except OSError as exc:
        log(f"[WARN] 无法改名旧 dll（{exc}），改为直接覆盖")
        try:
            os.remove(tmp)
        except OSError:
            pass
        return False
    try:
        os.replace(tmp, dst_dll)
        return True
    except OSError as exc:
        log(f"[WARN] 替换失败，回滚：{exc}")
        try:
            os.replace(old, dst_dll)
        except OSError:
            pass
        return False


def deploy_local(spec):
    """把 dll 放到 %APPDATA%\\TTVoice\\<dll>，应用在跑时走热加载。"""
    appdata = os.environ.get("APPDATA", "")
    if not appdata:
        log("[WARN] 本机没有 APPDATA，跳过本地部署")
        return False
    runtime_dir = os.path.join(appdata, "TTVoice")
    if not os.path.isdir(runtime_dir):
        log(f"[WARN] 运行时目录不存在：{runtime_dir}")
        return False

    src_dll = os.path.join(REPO_ROOT, *spec["dir"].split("/"), spec["dll"])
    dst_dll = os.path.join(runtime_dir, spec["dll"])
    if not os.path.isfile(src_dll):
        log(f"[WARN] 未找到产物 {src_dll}")
        return False

    running = app_running()
    if running and os.path.isfile(dst_dll):
        if hot_replace(src_dll, dst_dll):
            log(f"已热替换 {dst_dll}（proxy 约 1s 内热加载，日志看 module: ... hot reload done）")
            return True
        log("[!] 热替换失败：请关闭 TTVoice 后重跑")
        return False

    try:
        shutil.copy2(src_dll, dst_dll)
        log(f"已部署 {dst_dll}")
        if not os.path.isfile(os.path.join(runtime_dir, spec["dll"])):
            log("[!] 复制后仍找不到文件，检查权限")
            return False
        if not running:
            log(f"应用未运行，下次启动时加载；记得在 version.ini [modules] 里加上 {spec['dll']}")
        else:
            log(f"首次部署该模块：需要重启一次宿主才会加载；之后换文件即热加载")
        return True
    except OSError as exc:
        log(f"[WARN] 部署失败：{exc}")
        return False


def build_one(client, name, spec, no_build):
    src_dir = os.path.join(REPO_ROOT, *spec["dir"].split("/"))
    dll = os.path.join(src_dir, spec["dll"])
    log(f"=== 模块 {name}（{spec['dir']}）===")

    if not no_build:
        if not build_on_linux(client, spec) and not os.path.isfile(dll):
            return False
    else:
        log(f"[{name}] --no-build：跳过编译")

    if not os.path.isfile(dll):
        log(f"[{name}] 未找到产物 {dll}")
        return False
    return deploy_local(spec)


def main():
    args = sys.argv[1:]
    no_build = "--no-build" in args
    args = [a for a in args if not a.startswith("--")]
    names = [a.lower() for a in args] or list(DEFAULT_PLUGINS)
    for name in names:
        if name not in PLUGINS:
            log(f"未知模块 {name}，可选：{', '.join(PLUGINS)}")
            raise SystemExit(2)

    client = ssh_connect()
    try:
        failed = [n for n in names if not build_one(client, n, PLUGINS[n], no_build)]
    finally:
        client.close()
    if failed:
        log(f"失败模块：{', '.join(failed)}")
        raise SystemExit(1)
    log("完成。")


if __name__ == "__main__":
    main()
