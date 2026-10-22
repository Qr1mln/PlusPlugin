"""诊断 d3d9 序数 137 问题：
1) 本机 System32\d3d9.dll 是否存在序数 137（有无名字）
2) System32\dwmapi.dll 的导入（是否引用 d3d9）
3) 安装目录内各 PE 对 d3d9 的导入方式（名字/序号）
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_imports import PE, collect_pe_files, INSTALL_DIR  # noqa: E402

SYSROOT = os.environ.get("SystemRoot", r"C:\Windows")


def export_table(path):
    with open(path, "rb") as fh:
        data = fh.read()
    pe = PE(data).parse()
    rva, _ = pe.data_dir(0)
    if not rva:
        return None
    off = pe.rva2off(rva)
    (_c, _t, _maj, _min, _n, base, num_funcs, num_names, addr_funcs, addr_names,
     addr_ord) = struct.unpack_from("<IIHHIIIIIII", data, off)
    funcs_off = pe.rva2off(addr_funcs)
    names_off = pe.rva2off(addr_names)
    ords_off = pe.rva2off(addr_ord)

    addr_by_ord = {}
    for i in range(num_funcs):
        addr_by_ord[base + i] = struct.unpack_from("<I", data, funcs_off + i * 4)[0]

    names_by_ord = {}
    for i in range(num_names):
        name_rva = struct.unpack_from("<I", data, names_off + i * 4)[0]
        no = pe.rva2off(name_rva)
        end = data.find(b"\0", no)
        name = data[no:end].decode("latin1", "replace")
        idx = struct.unpack_from("<H", data, ords_off + i * 2)[0]
        names_by_ord[base + idx] = name
    return {"base": base, "funcs": addr_by_ord, "names": names_by_ord}


def imports_detail(path):
    with open(path, "rb") as fh:
        pe = PE(fh.read()).parse()
    out = {}
    for idx, kind in ((1, "import"), (13, "delay")):
        rva, _ = pe.data_dir(idx)
        if not rva:
            continue
        off = pe.rva2off(rva)
        guard = 0
        while off and guard < 512:
            guard += 1
            if idx == 1:
                oft, _ts, _fc, name_rva, ft = struct.unpack_from("<IIIII", pe.data, off)
                next_off = off + 20
            else:
                fields = struct.unpack_from("<IIIIIIII", pe.data, off)
                oft, name_rva = fields[3], fields[1]
                next_off = off + 32
            if name_rva == 0:
                break
            no = pe.rva2off(name_rva)
            end = pe.data.find(b"\0", no)
            dll = pe.data[no:end].decode("latin1", "replace").lower()
            thunk_rva = oft
            toff = pe.rva2off(thunk_rva) if thunk_rva else None
            size = 8 if pe.pe32plus else 4
            funcs = []
            i = 0
            while toff and i < 4096:
                raw = pe.data[toff + i * size:toff + (i + 1) * size]
                if len(raw) < size:
                    break
                val = struct.unpack("<Q" if size == 8 else "<I", raw)[0]
                if val == 0:
                    break
                high = 1 << 63 if size == 8 else 1 << 31
                if val & high:
                    funcs.append("#%d" % (val & 0xFFFF))
                else:
                    n_off = pe.rva2off(val & 0x7FFFFFFF)
                    if n_off:
                        e2 = pe.data.find(b"\0", n_off + 2)
                        funcs.append(pe.data[n_off + 2:e2].decode("latin1", "replace"))
                i += 1
            out.setdefault(dll, []).extend(funcs)
            off = next_off
    return out


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    d3d9 = os.path.join(SYSROOT, "System32", "d3d9.dll")
    print(f"--- local {d3d9} ---")
    if os.path.isfile(d3d9):
        info = export_table(d3d9)
        if info:
            has137 = 137 in info["funcs"]
            print(f"ordinal 137 exists: {has137}  name: {info['names'].get(137, '<none>')}")
            print(f"total funcs: {len(info['funcs'])}  named: {len(info['names'])}")
            sample = [f"{o}:{n}" for o, n in list(info["names"].items())[:6]]
            print("sample:", ", ".join(sample))
    else:
        print("not found")

    print("\n--- System32\\dwmapi.dll imports ---")
    d = os.path.join(SYSROOT, "System32", "dwmapi.dll")
    if os.path.isfile(d):
        for dll, funcs in imports_detail(d).items():
            print(f"  {dll}: {', '.join(funcs[:10])}{' ...' if len(funcs) > 10 else ''}")
    else:
        print("  not found")

    print("\n--- install dir modules importing d3d9 ---")
    for path in collect_pe_files(INSTALL_DIR):
        try:
            info = imports_detail(path)
        except Exception:
            continue
        if "d3d9.dll" in info:
            funcs = info["d3d9.dll"]
            ordinals = [f for f in funcs if f.startswith("#")]
            print(f"  {os.path.basename(path)}: {len(funcs)} refs, ordinals={ordinals}")
            print(f"      names: {', '.join([f for f in funcs if not f.startswith('#')][:10])}")


if __name__ == "__main__":
    main()
