"""PE 导入表静态分析（只读）。

用途：判断 DLL 搜索顺序劫持（sideloading）是否有可用目标。
规则：导入的名字 = 系统目录解析 + 安装目录无同名文件 + 非 KnownDLL + 非 API-Set。
"""
import os
import struct
import sys
import winreg

INSTALL_DIR = r"C:\Users\111\AppData\Roaming\TTVoice"
API_SET_PREFIXES = ("api-ms-win-", "ext-ms-")


def load_known_dlls():
    known = set()
    try:
        key = winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE,
            r"SYSTEM\CurrentControlSet\Control\Session Manager\KnownDLLs",
        )
        i = 0
        while True:
            try:
                name, value, _ = winreg.EnumValue(key, i)
            except OSError:
                break
            known.add(str(value).lower())
            i += 1
    except OSError as exc:
        print(f"[warn] 读取 KnownDLLs 失败: {exc}")
    return known


class PE:
    def __init__(self, data):
        self.data = data

    def u16(self, off):
        return struct.unpack_from("<H", self.data, off)[0]

    def u32(self, off):
        return struct.unpack_from("<I", self.data, off)[0]

    def parse(self):
        d = self.data
        if len(d) < 0x40 or d[:2] != b"MZ":
            raise ValueError("not MZ")
        e_lfanew = self.u32(0x3C)
        if d[e_lfanew:e_lfanew + 4] != b"PE\0\0":
            raise ValueError("not PE")
        coff = e_lfanew + 4
        machine, nsect = struct.unpack_from("<HH", d, coff)
        size_opt = struct.unpack_from("<H", d, coff + 16)[0]
        opt = coff + 20
        magic = self.u16(opt)
        pe32plus = magic == 0x20B
        if not pe32plus and magic != 0x10B:
            raise ValueError("bad optional header")
        self.pe32plus = pe32plus
        dd_off = opt + (112 if pe32plus else 96)
        num_dirs = self.u32(opt + (108 if pe32plus else 92))
        self.dd_off = dd_off
        self.num_dirs = num_dirs
        sect_off = opt + size_opt
        self.sections = []
        for i in range(nsect):
            o = sect_off + i * 40
            name = d[o:o + 8].rstrip(b"\0").decode("latin1", "replace")
            vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", d, o + 8)
            self.sections.append((name, vaddr, vsize, raddr, rsize))

        def dd(idx):
            if idx >= num_dirs:
                return 0, 0
            return struct.unpack_from("<II", d, dd_off + idx * 8)

        self.imports = []   # (dll, kind)
        self._parse_imports(dd(1)[0], "import")
        self._parse_delay(dd(13)[0])
        return self

    def rva2off(self, rva):
        for _name, va, vs, ra, rs in self.sections:
            if va <= rva < va + max(vs, rs):
                return ra + (rva - va)
        return None

    def data_dir(self, idx):
        if idx >= self.num_dirs:
            return 0, 0
        return struct.unpack_from("<II", self.data, self.dd_off + idx * 8)

    def _dll_name(self, name_rva):
        off = self.rva2off(name_rva)
        if off is None or off >= len(self.data):
            return None
        end = self.data.find(b"\0", off)
        if end < 0:
            return None
        return self.data[off:end].decode("latin1", "replace")

    def _parse_imports(self, irva, kind):
        if not irva:
            return
        off = self.rva2off(irva)
        guard = 0
        while off and off + 20 <= len(self.data) and guard < 512:
            guard += 1
            # IMAGE_IMPORT_DESCRIPTOR: OriginalFirstThunk, TimeDateStamp,
            # ForwarderChain, Name, FirstThunk
            _oft, _ts, _fc, name_rva, _ft = struct.unpack_from("<IIIII", self.data, off)
            if name_rva == 0:
                break
            dll = self._dll_name(name_rva)
            if dll:
                self.imports.append((dll, kind))
            off += 20

    def _parse_delay(self, drva):
        if not drva:
            return
        off = self.rva2off(drva)
        guard = 0
        while off and off + 32 <= len(self.data) and guard < 512:
            guard += 1
            fields = struct.unpack_from("<IIIIIIII", self.data, off)
            name_rva = fields[1]
            if name_rva == 0:
                break
            dll = self._dll_name(name_rva)
            if dll:
                self.imports.append((dll, "delay"))
            off += 32


def collect_pe_files(root):
    found = []
    for dirpath, _dirnames, filenames in os.walk(root):
        # 跳过 WebView2 用户数据与驱动目录（非 PE 或无关）
        for fn in filenames:
            if not fn.lower().endswith((".exe", ".dll")):
                continue
            found.append(os.path.join(dirpath, fn))
    return sorted(found)


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    if not os.path.isdir(INSTALL_DIR):
        print(f"安装目录不存在: {INSTALL_DIR}")
        return 1

    known = load_known_dlls()
    local_files = set()
    for dirpath, _d, filenames in os.walk(INSTALL_DIR):
        for fn in filenames:
            local_files.add(fn.lower())

    files = collect_pe_files(INSTALL_DIR)
    parsed = {}
    failed = []
    for path in files:
        try:
            with open(path, "rb") as fh:
                data = fh.read()
            pe = PE(data).parse()
            parsed[path] = pe
        except Exception as exc:  # noqa: BLE001 - 逐个文件容错
            failed.append((path, str(exc)))

    print(f"=== 解析 PE: 成功 {len(parsed)} / 总计 {len(files)} ===")
    if failed:
        print(f"=== 无法解析 {len(failed)} 个（可忽略）===")
        for p, e in failed[:8]:
            print(f"  {os.path.basename(p)}: {e}")

    # 统计候选
    candidates = {}  # dll -> {"importers": set(), "kinds": set()}
    for path, pe in parsed.items():
        for dll, kind in pe.imports:
            base = dll.lower().strip()
            # 过滤解析噪声：必须是合法 DLL 名
            if not base.endswith(".dll") or not base.replace(".", "").replace("-", "").replace("_", "").isalnum():
                continue
            if base.startswith(API_SET_PREFIXES):
                continue
            if base in known:
                continue
            if base in local_files:
                continue
            rec = candidates.setdefault(base, {"importers": set(), "kinds": set()})
            rec["importers"].add(os.path.basename(path))
            rec["kinds"].add(kind)

    print(f"\n=== 可劫持候选（安装目录无同名文件 / 非 KnownDLL / 非 API-Set）: {len(candidates)} 个 ===")
    main_exe = "TTVoice.exe"
    # 按“是否被主程序直接导入”排序
    def score(item):
        dll, rec = item
        direct = 1 if main_exe in rec["importers"] else 0
        delay = 1 if "delay" in rec["kinds"] else 0
        return (-direct, -len(rec["importers"]), len(dll))

    for dll, rec in sorted(candidates.items(), key=score)[:40]:
        tag = "主程序直接导入" if main_exe in rec["importers"] else "依赖链导入"
        kinds = "+".join(sorted(rec["kinds"]))
        sample = ", ".join(sorted(rec["importers"])[:4])
        more = "" if len(rec["importers"]) <= 4 else f" 等 {len(rec['importers'])} 个"
        print(f"  {dll:<28} [{tag}][{kinds}] <- {sample}{more}")

    # 主程序完整导入清单
    main_path = os.path.join(INSTALL_DIR, main_exe)
    if main_path in parsed:
        print(f"\n=== {main_exe} 导入的 DLL ===")
        for dll, kind in parsed[main_path].imports:
            mark = ""
            base = dll.lower()
            if base.startswith(API_SET_PREFIXES):
                mark = " (API-Set)"
            elif base in known:
                mark = " (KnownDLL)"
            elif base in local_files:
                mark = " (安装目录自带)"
            else:
                mark = " <-- 可劫持"
            print(f"  {dll:<32} [{kind}]{mark}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
