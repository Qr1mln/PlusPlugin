#!/usr/bin/env python3
r"""
插件部署：把本地编译的 framework.dll + 前端页面更新到运行时（%APPDATA%\TTVoice）。

framework.dll 是 version.dll 经 version.ini [loaders] 加载的 pplugin 插件；
plugin_loader 会监视 DLL 文件变化并自动热重载，所以正常情况下**不需要重启 TTVoice**。

framework.dll 在运行中被 TTVoice 映射占用，直接 copy2 覆盖会失败；本脚本用
「改名换位」（framework.dll -> framework.dll.old，再把新文件改名为 framework.dll）
让 plugin_loader 察觉变化并热重载。若改名也被占用阻止，才回退到停/启 TTVoice。

本脚本只部署插件，不动 version.dll（那是 deploy.py 的职责）。
前置：先跑 build.py 产出 framework/framework.dll；前端先 `cd editor2 && npm run build`。

用法：
  python deploy_plugin.py
"""
import os
import shutil

from deploy import log, start_app, stop_app  # 复用 deploy.py 的本机停/启（仅回退用）

REPO_ROOT = os.path.dirname(os.path.abspath(__file__))
FRAMEWORK_DLL = os.path.join(REPO_ROOT, "framework", "framework.dll")
DIST_DIR = os.path.join(REPO_ROOT, "editor2", "dist")  # 前端构建产物


def appdata_runtime_dir():
    appdata = os.environ.get("APPDATA", "")
    if not appdata:
        return ""
    return os.path.join(appdata, "TTVoice")


def hot_replace_framework(runtime_dir):
    """不停应用地替换 framework.dll（改名换位），成功返回 True。"""
    target = os.path.join(runtime_dir, "framework.dll")
    tmp = target + ".new"
    old = target + ".old"

    try:
        shutil.copy2(FRAMEWORK_DLL, tmp)
    except OSError as exc:
        log(f"[WARN] 写入临时文件失败：{exc}")
        return False

    # 清理上次遗留的 .old（若仍被映射则删不掉，忽略）
    try:
        if os.path.isfile(old):
            os.remove(old)
    except OSError:
        pass

    try:
        os.replace(target, old)  # 正在被 TTVoice 映射时可能抛 PermissionError
    except OSError:
        try:
            os.remove(tmp)
        except OSError:
            pass
        return False

    try:
        os.replace(tmp, target)
        return True
    except OSError as exc:
        log(f"[WARN] 替换失败，回滚：{exc}")
        try:
            os.replace(old, target)
        except OSError:
            pass
        return False


def deploy_frontend(runtime_dir):
    """优先部署 editor2/dist（index.html + assets/）；没有构建产物则回退源码页。"""
    if os.path.isdir(DIST_DIR):
        for name in os.listdir(DIST_DIR):
            src = os.path.join(DIST_DIR, name)
            dst = os.path.join(runtime_dir, name)
            try:
                if os.path.isdir(src):
                    shutil.copytree(src, dst, dirs_exist_ok=True)
                else:
                    shutil.copy2(src, dst)
                log(f"部署 {name} -> {runtime_dir}")
            except OSError as exc:
                log(f"[WARN] 部署 {name} 失败：{exc}")
        return True

    idx = os.path.join(REPO_ROOT, "editor2", "index.html")
    if os.path.isfile(idx):
        try:
            shutil.copy2(idx, os.path.join(runtime_dir, "index.html"))
            log("部署 index.html -> %s（未找到 editor2/dist，静态资源可能缺失）" % runtime_dir)
            return True
        except OSError as exc:
            log(f"[WARN] 部署 index.html 失败：{exc}")
    return False


def main():
    if not os.path.isfile(FRAMEWORK_DLL):
        log(f"未找到 {FRAMEWORK_DLL}，请先跑 build.py 编译。")
        raise SystemExit(1)

    runtime_dir = appdata_runtime_dir()
    if not runtime_dir:
        log("[WARN] 本机没有 APPDATA，无法部署")
        raise SystemExit(1)
    os.makedirs(runtime_dir, exist_ok=True)

    if hot_replace_framework(runtime_dir):
        log("framework.dll 已热替换，plugin_loader 会自动热重载（未重启应用）")
        restarted = False
    else:
        log("framework.dll 被占用，回退：停/启 TTVoice 部署")
        stop_app()
        try:
            shutil.copy2(FRAMEWORK_DLL, os.path.join(runtime_dir, "framework.dll"))
            log(f"部署 framework.dll -> {runtime_dir}")
        except OSError as exc:
            log(f"[WARN] 部署 framework.dll 失败：{exc}")
        restarted = True

    # 前端为静态文件，CivetWeb 每次请求读盘，无需重启。
    deploy_frontend(runtime_dir)

    if restarted:
        start_app()
    log("完成。")


if __name__ == "__main__":
    main()
