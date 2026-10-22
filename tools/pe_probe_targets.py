"""针对候选 DLL 的精确核对：导入方（加载时机）+ 系统 DLL 导出数量（转发成本）。"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_imports import PE, collect_pe_files, INSTALL_DIR  # noqa: E402

TARGETS = [
    "sensapi.dll",
    "version.dll",
    "dwmapi.dll",
    "comctl32.dll",
    "avrt.dll",
    "dsound.dll",
    "mpr.dll",
    "tdh.dll",
    "opengl32.dll",
    "dxgi.dll",
]


def parse_exports(path):
    with open(path, "rb") as fh:
        data = fh.read()
    pe = PE(data).parse()
    rva, size = pe.data_dir(0)
    if not rva:
        return []
    off = pe.rva2off(rva)
    if off is None:
        return []
    (
        _chars,
        _ts,
        _maj,
        _min,
        _name,
        _base,
        num_funcs,
        num_names,
        addr_funcs,
        addr_names,
        _addr_ord,
    ) = struct.unpack_from("<IIHHIIIIIII", data, off)
    if not num_names:
        return []
    names_off = pe.rva2off(addr_names)
    if names_off is None:
        return []
    out = []
    for i in range(num_names):
        name_rva = struct.unpack_from("<I", data, names_off + i * 4)[0]
        no = pe.rva2off(name_rva)
        if no is None:
            continue
        end = data.find(b"\0", no)
        out.append(data[no:end].decode("latin1", "replace"))
    return out


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    parsed = {}
    for path in collect_pe_files(INSTALL_DIR):
        try:
            with open(path, "rb") as fh:
                parsed[path] = PE(fh.read()).parse()
        except Exception:
            continue

    print(f"安装目录 PE 文件: {len(parsed)}\n")
    print(f"{'候选DLL':<20}{'导出数':>6}  {'被导入':>5}  导入方")
    print("-" * 100)
    for target in TARGETS:
        importers = []
        kinds = set()
        for path, pe in parsed.items():
            for dll, kind in pe.imports:
                if dll.lower().strip() == target:
                    importers.append(os.path.basename(path))
                    kinds.add(kind)
        sys_path = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", target)
        n_exports = len(parse_exports(sys_path)) if os.path.isfile(sys_path) else -1
        kind_tag = "+".join(sorted(kinds)) if kinds else "-"
        names = ", ".join(sorted(importers)[:6])
        more = "" if len(importers) <= 6 else f" 等{len(importers)}个"
        print(f"{target:<20}{n_exports:>6}  {len(importers):>5}  [{kind_tag}] {names}{more}")

    # 主程序导入链深度分析：哪些 DLL 在进程启动时被直接加载
    print("\n=== 主程序直接导入（进程启动即加载）===")
    main_exe = os.path.join(INSTALL_DIR, "TTVoice.exe")
    if main_exe in parsed:
        for dll, kind in parsed[main_exe].imports:
            base = dll.lower()
            if base.startswith(("api-ms-", "ext-ms-")):
                continue
            local = os.path.isfile(os.path.join(INSTALL_DIR, dll))
            print(f"  {dll:<40} {'本地' if local else '系统'} [{kind}]")


if __name__ == "__main__":
    main()
