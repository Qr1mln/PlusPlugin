#!/usr/bin/env python3
r"""
PlusPlugin 本地编译脚本：编译 version.dll，产物留在项目 proxy\ 目录
流程：
  1) 运行 proxy\build.bat（bat 自带 vswhere→vcvars64，产物落在 proxy\）
  2) 校验 proxy\version.dll 已生成
说明：只编译、不部署。把 dll 更新到运行时主机请跑 deploy.py；
      插件（plugin\ 下）用 build_plugin.py 本地编译并安装。
前置：本机装有 Visual Studio Build Tools（MSVC x64 + MASM 组件）。
"""
import os
import subprocess
import sys

# ===================== 配置区（按需修改） =====================
REPO_ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD_BAT = os.path.join(REPO_ROOT, "proxy", "build.bat")
PRODUCT = os.path.join(REPO_ROOT, "proxy", "version.dll")
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
    """本地控制台编码不了某些字符时降级打印，别让编译流程崩掉。"""
    enc = sys.stdout.encoding or "utf-8"
    try:
        print(text)
    except UnicodeEncodeError:
        print(text.encode(enc, "replace").decode(enc, "replace"))


def run_bat(bat):
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


def main():
    if not os.path.isfile(BUILD_BAT):
        log(f"未找到 {BUILD_BAT}")
        raise SystemExit(1)
    if run_bat(BUILD_BAT) != 0:
        log("编译失败，请检查上面的输出。")
        raise SystemExit(1)
    if not os.path.isfile(PRODUCT):
        log(f"未找到产物 {PRODUCT}，编译可能失败。")
        raise SystemExit(1)
    log(f"完成：{PRODUCT}")
    log("更新到运行时主机请跑 deploy.py。")


if __name__ == "__main__":
    main()
