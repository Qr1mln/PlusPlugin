"""从系统 DLL 导出表生成 .def 转发文件（路线 1）。

用法: python tools/gen_def.py dwmapi.dll dwmapi_real native/dwmapi_proxy/dwmapi.def
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_imports import PE  # noqa: E402


def export_names(path):
    with open(path, "rb") as fh:
        data = fh.read()
    pe = PE(data).parse()
    rva, _ = pe.data_dir(0)
    if not rva:
        return []
    off = pe.rva2off(rva)
    if off is None:
        return []
    fields = struct.unpack_from("<IIHHIIIIIII", data, off)
    base = fields[5]
    num_names = fields[7]
    addr_names = fields[9]
    names_off = pe.rva2off(addr_names)
    if not names_off:
        return []
    out = []
    for i in range(num_names):
        name_rva = struct.unpack_from("<I", data, names_off + i * 4)[0]
        no = pe.rva2off(name_rva)
        if no is None:
            continue
        end = data.find(b"\0", no)
        out.append((data[no:end].decode("latin1", "replace"), base + i))
    return out


def main():
    if len(sys.argv) < 2:
        print("usage: gen_def.py <dll-name> [real-module-name] [output-def]")
        return 1
    dll_name = sys.argv[1]
    real_module = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(dll_name)[0] + "_real"
    out_path = sys.argv[3] if len(sys.argv) > 3 else os.path.join("native", "dwmapi_proxy", "dwmapi.def")

    sysroot = os.environ.get("SystemRoot", r"C:\Windows")
    src = os.path.join(sysroot, "System32", dll_name)
    if not os.path.isfile(src):
        print(f"source not found: {src}")
        return 1

    names = export_names(src)
    if not names:
        print("no exports parsed")
        return 1

    internal = os.path.splitext(dll_name)[0]
    lines = [
        f"; auto-generated from {src}",
        f"; forwarders -> {real_module}.<name>  (real DLL must sit next to this one)",
        f"LIBRARY {internal}",
        "EXPORTS",
    ]
    for name, _ordinal in names:
        lines.append(f"    {name}={real_module}.{name}")

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\r\n") as fh:
        fh.write("\n".join(lines) + "\n")

    print(f"wrote {out_path}: {len(names)} forwarders")
    print("note: ordinal-only exports (no name) are not forwarded; target app imports by name only")
    return 0


if __name__ == "__main__":
    sys.exit(main())
