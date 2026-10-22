"""生成 stub 型代理工程（AheadLib 风格）。

产物（native/dwmapi_proxy/）:
  stubs.asm        44 个 x64 stub: jmp qword ptr [g_pfn_X]
  exports.def      导出 stub 函数名
  proxy_exports.h  X-macro 列表，供 dllmain.cpp 定义/解析指针

用法: python tools/gen_proxy_stub.py [dll-name]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_def import export_names  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "native", "dwmapi_proxy")


def write(path, text, crlf=True):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="ascii", newline="\r\n" if crlf else "\n") as fh:
        fh.write(text)
    print(f"wrote {path}")


def main():
    dll = sys.argv[1] if len(sys.argv) > 1 else "dwmapi.dll"
    sysroot = os.environ.get("SystemRoot", r"C:\Windows")
    src = os.path.join(sysroot, "System32", dll)
    names = [n for n, _ in export_names(src)]
    if not names:
        print(f"no exports parsed from {src}")
        return 1

    # stubs.asm
    asm = ["; auto-generated, do not edit", "OPTION CASEMAP:NONE", ""]
    asm += [f"EXTERN g_pfn_{n}:QWORD" for n in names]
    asm += ["", ".code", ""]
    for n in names:
        asm += [f"{n} PROC", f"    jmp qword ptr [g_pfn_{n}]", f"{n} ENDP", ""]
    asm += ["END", ""]
    write(os.path.join(OUT_DIR, "stubs.asm"), "\n".join(asm))

    # exports.def
    internal = os.path.splitext(dll)[0]
    deff = [f"; auto-generated from {src}", f"LIBRARY {internal}", "EXPORTS"]
    deff += [f"    {n}" for n in names]
    deff += [""]
    write(os.path.join(OUT_DIR, "exports.def"), "\n".join(deff))

    # proxy_exports.h
    hdr = [
        "// auto-generated, do not edit",
        "#pragma once",
        "",
        "#define PROXY_EXPORT_LIST(X) \\",
    ]
    hdr += [f"    X({n}) \\" for n in names]
    hdr[-1] = hdr[-1].rstrip(" \\")  # last line without continuation
    hdr += ["", ""]
    write(os.path.join(OUT_DIR, "proxy_exports.h"), "\n".join(hdr))

    print(f"total exports: {len(names)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
