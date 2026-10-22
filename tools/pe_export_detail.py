"""核对系统 DLL 导出细节：函数总数 / 命名导出数 / 序号空洞 / 转发导出。"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_imports import PE  # noqa: E402


def detail(path):
    with open(path, "rb") as fh:
        data = fh.read()
    pe = PE(data).parse()
    rva, _size = pe.data_dir(0)
    if not rva:
        return None
    off = pe.rva2off(rva)
    fields = struct.unpack_from("<IIHHIIIIIII", data, off)
    (_chars, _ts, _maj, _min, _name_rva, base, num_funcs, num_names,
     addr_funcs, addr_names, addr_ord) = fields

    funcs_off = pe.rva2off(addr_funcs)
    names_off = pe.rva2off(addr_names)
    ords_off = pe.rva2off(addr_ord)

    names = []
    for i in range(num_names):
        name_rva = struct.unpack_from("<I", data, names_off + i * 4)[0]
        no = pe.rva2off(name_rva)
        end = data.find(b"\0", no)
        names.append(data[no:end].decode("latin1", "replace"))

    # 每个序号对应的函数地址，识别转发（地址落在导出目录范围内）
    exp_lo, exp_hi = rva, rva + (off and 0) + 4096
    forwarders = []
    for idx in range(num_funcs):
        func_rva = struct.unpack_from("<I", data, funcs_off + idx * 4)[0]
        if exp_lo <= func_rva < exp_hi:
            s_off = pe.rva2off(func_rva)
            if s_off:
                end = data.find(b"\0", s_off)
                forwarders.append((base + idx, data[s_off:end].decode("latin1", "replace")))

    named_ordinals = set()
    for i in range(num_names):
        named_ordinals.add(struct.unpack_from("<H", data, ords_off + i * 2)[0])
    ordinal_only = [base + i for i in range(num_funcs) if i not in named_ordinals]

    return {
        "num_funcs": num_funcs,
        "num_names": num_names,
        "base": base,
        "ordinal_only": ordinal_only,
        "forwarders": forwarders,
        "names": names,
    }


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    sysroot = os.environ.get("SystemRoot", r"C:\Windows")
    for dll in ("dwmapi.dll", "version.dll", "winmm.dll"):
        path = os.path.join(sysroot, "System32", dll)
        if not os.path.isfile(path):
            print(f"{dll}: not found")
            continue
        info = detail(path)
        if not info:
            print(f"{dll}: no export dir")
            continue
        print(f"=== {dll} ===")
        print(f"  NumberOfFunctions={info['num_funcs']}  NumberOfNames={info['num_names']}  Base={info['base']}")
        print(f"  仅序号导出(无名字): {len(info['ordinal_only'])} 个 -> {info['ordinal_only'][:20]}")
        print(f"  转发型导出: {len(info['forwarders'])} 个")
        for o, fwd in info["forwarders"][:6]:
            print(f"    #{o} -> {fwd}")
        print(f"  命名导出前 8 个: {info['names'][:8]}")
        print()


if __name__ == "__main__":
    main()
