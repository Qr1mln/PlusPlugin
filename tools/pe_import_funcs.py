"""核对：安装目录内各 PE 对 dwmapi/version 的导入函数（按名 or 按序号）。"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_imports import PE, collect_pe_files, INSTALL_DIR  # noqa: E402

TARGETS = {"dwmapi.dll", "version.dll"}


def imported_funcs(pe, wanted):
    """返回 {dllname: [(name_or_ordinal)]}，仅收集 wanted 中的 DLL。"""
    result = {}
    irva, _ = pe.data_dir(1)
    if not irva:
        return result
    off = pe.rva2off(irva)
    guard = 0
    while off and off + 20 <= len(pe.data) and guard < 512:
        guard += 1
        oft, _ts, _fc, name_rva, ft = struct.unpack_from("<IIIII", pe.data, off)
        if name_rva == 0:
            break
        no = pe.rva2off(name_rva)
        end = pe.data.find(b"\0", no)
        dll = pe.data[no:end].decode("latin1", "replace").lower()
        if dll in wanted:
            thunk_rva = oft or ft
            toff = pe.rva2off(thunk_rva)
            size = 8 if pe.pe32plus else 4
            values = []
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
                    values.append("#%d" % (val & 0xFFFF))
                else:
                    name_off = pe.rva2off(val & 0x7FFFFFFF)
                    if name_off:
                        end2 = pe.data.find(b"\0", name_off + 2)
                        values.append(pe.data[name_off + 2:end2].decode("latin1", "replace"))
                i += 1
            result.setdefault(dll, []).extend(values)
        off += 20
    return result


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    for path in collect_pe_files(INSTALL_DIR):
        try:
            with open(path, "rb") as fh:
                pe = PE(fh.read()).parse()
        except Exception:
            continue
        funcs = imported_funcs(pe, TARGETS)
        for dll, names in funcs.items():
            ordinals = [n for n in names if n.startswith("#")]
            print(f"{os.path.basename(path)} -> {dll}")
            print(f"    名字导入 {len(names) - len(ordinals)} 个 | 序号导入 {len(ordinals)} 个")
            print(f"    函数: {', '.join(names[:12])}{' ...' if len(names) > 12 else ''}")
            if ordinals:
                print(f"    !! 序号导入: {ordinals}")


if __name__ == "__main__":
    main()
