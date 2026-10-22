"""检查指定 PE 的导入（含延迟导入，正确处理 VA/RVA 与序号导入）。"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_imports import PE  # noqa: E402


def image_base(pe):
    opt = pe.rva2off(0)  # not used
    return 0


def read_thunks(pe, rva, size):
    off = pe.rva2off(rva)
    out = []
    if not off:
        return out
    i = 0
    while i < 4096:
        raw = pe.data[off + i * size:off + (i + 1) * size]
        if len(raw) < size:
            break
        val = struct.unpack("<Q" if size == 8 else "<I", raw)[0]
        if val == 0:
            break
        high = 1 << 63 if size == 8 else 1 << 31
        if val & high:
            out.append("#%d" % (val & 0xFFFF))
        else:
            n_off = pe.rva2off(val & 0x7FFFFFFF)
            if n_off:
                end = pe.data.find(b"\0", n_off + 2)
                out.append(pe.data[n_off + 2:end].decode("latin1", "replace"))
        i += 1
    return out


def dump(path):
    with open(path, "rb") as fh:
        pe = PE(fh.read()).parse()
    size = 8 if pe.pe32plus else 4
    print(f"=== {path} (pe32plus={pe.pe32plus}) ===")

    # normal imports
    rva, _ = pe.data_dir(1)
    if rva:
        off = pe.rva2off(rva)
        g = 0
        while off and g < 512:
            g += 1
            oft, _ts, _fc, name_rva, ft = struct.unpack_from("<IIIII", pe.data, off)
            if name_rva == 0:
                break
            no = pe.rva2off(name_rva)
            end = pe.data.find(b"\0", no)
            dll = pe.data[no:end].decode("latin1", "replace")
            funcs = read_thunks(pe, oft or ft, size)
            print(f"\n[import] {dll} ({len(funcs)})")
            print("   " + ", ".join(funcs[:20]) + (" ..." if len(funcs) > 20 else ""))
            off += 20

    # delay imports
    rva, _ = pe.data_dir(13)
    if rva:
        off = pe.rva2off(rva)
        g = 0
        while off and g < 512:
            g += 1
            fields = struct.unpack_from("<IIIIIIII", pe.data, off)
            attrs, name_rva, _hmod, iat, int_ = fields[0], fields[1], fields[2], fields[3], fields[4]
            if name_rva == 0:
                break
            no = pe.rva2off(name_rva)
            end = pe.data.find(b"\0", no)
            dll = pe.data[no:end].decode("latin1", "replace")
            thunk = int_ or iat
            funcs = read_thunks(pe, thunk, size)
            print(f"\n[delay] {dll} ({len(funcs)}) attrs={attrs:#x}")
            print("   " + ", ".join(funcs[:20]) + (" ..." if len(funcs) > 20 else ""))
            off += 32


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    for p in sys.argv[1:]:
        dump(p)
