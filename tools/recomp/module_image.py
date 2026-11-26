"""A decoded Xbox 360 module: PE sections, .pdata, imports and exports.

Built on tools/xex.py. Everything here is read from the user's XEX2 file at
build time; nothing derived from it is stored in the repository.
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from xex import Xex  # noqa: E402

IMAGE_SCN_CNT_CODE = 0x20


class Section:
    def __init__(self, name, va, vsize, rsize, flags):
        self.name, self.va, self.vsize, self.rsize, self.flags = name, va, vsize, rsize, flags

    @property
    def code(self):
        return bool(self.flags & IMAGE_SCN_CNT_CODE)

    def __repr__(self):
        return "<%s %08X+%X>" % (self.name, self.va, self.vsize)


class Import:
    """One xboxkrnl/xam import. `slot` is the record the loader overwrites
    with the export's address (type 0); `thunk` the 16-byte call stub
    (type 1), absent for variable imports."""

    def __init__(self, library, ordinal):
        self.library, self.ordinal = library, ordinal
        self.slot = None
        self.thunk = None
        self.name = None
        self.kind = None  # 'function' or 'variable' from the export table


def load_export_names(table_path):
    """{ordinal: (name, kind)} from a xenia-style *_table.inc file."""
    names = {}
    pat = re.compile(r"XE_EXPORT\(\s*\w+\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\)")
    with open(table_path) as f:
        for m in pat.finditer(f.read()):
            names[int(m.group(1), 16)] = (m.group(2), "variable" if m.group(3) == "kVariable" else "function")
    return names


class ModuleImage:
    def __init__(self, xex_path, kernel_table=None):
        self.path = xex_path
        self.xex = Xex(xex_path)
        self.data = self.xex.image()
        self.base = self.xex.load_address
        image_base = self.xex.value(0x00010201)
        if image_base is not None:
            self.base = image_base
        self.size = self.xex.image_size
        self.entry = self.xex.value(0x00010100)
        self._parse_pe()
        self._parse_imports(kernel_table)
        self._parse_exports()
        pe = self.xex.blob(0x000183FF)
        self.original_name = pe[4:].split(b"\0")[0].decode() if pe else os.path.basename(xex_path)

    # ---- memory --------------------------------------------------------
    def stored(self, va, n=4):
        off = va - self.base
        return 0 <= off and off + n <= len(self.data)

    def u32(self, va):
        off = va - self.base
        if off < 0 or off + 4 > len(self.data):
            return 0  # beyond the stored data: zero-filled BSS
        return struct.unpack_from(">I", self.data, off)[0]

    def u16(self, va):
        off = va - self.base
        if off < 0 or off + 2 > len(self.data):
            return 0
        return struct.unpack_from(">H", self.data, off)[0]

    def u8(self, va):
        off = va - self.base
        if off < 0 or off >= len(self.data):
            return 0
        return self.data[off]

    # ---- PE --------------------------------------------------------------
    def _parse_pe(self):
        d = self.data
        if d[:2] != b"MZ":
            raise ValueError("decoded image has no MZ header")
        lf = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, lf + 6)[0]
        opt = struct.unpack_from("<H", d, lf + 20)[0]
        s = lf + 24 + opt
        self.sections = []
        for _ in range(nsec):
            name = d[s:s + 8].rstrip(b"\0").decode("ascii", "replace")
            vs, va, rs = struct.unpack_from("<III", d, s + 8)
            ch = struct.unpack_from("<I", d, s + 36)[0]
            self.sections.append(Section(name, self.base + va, vs, rs, ch))
            s += 40
        self.code_sections = [x for x in self.sections if x.code]
        pd = self.section(".pdata")
        self.pdata = []
        self.prolog = {}  # function begin -> prologue length in instructions
        if pd:
            for i in range(pd.vsize // 8):
                begin, info = struct.unpack_from(">II", d, pd.va - self.base + 8 * i)
                if begin == 0:
                    continue
                # Xbox 360 .pdata: PrologLength:8, FunctionLength:22,
                # ThirtyTwoBit:1, ExceptionFlag:1 (low to high bits).
                self.pdata.append((begin, ((info >> 8) & 0x3FFFFF) * 4, bool(info >> 31)))
                self.prolog[begin] = info & 0xFF
            self.pdata.sort()

    def section(self, name):
        for s in self.sections:
            if s.name == name:
                return s
        return None

    def section_at(self, va):
        for s in self.sections:
            if s.va <= va < s.va + max(s.vsize, s.rsize):
                return s
        return None

    def in_code(self, va):
        return any(s.va <= va < s.va + s.vsize for s in self.code_sections)

    # ---- imports / exports ------------------------------------------------
    def _parse_imports(self, kernel_table):
        names = load_export_names(kernel_table) if kernel_table else {}
        self.imports = []
        for lib, _ver, recs in self.xex.import_libraries():
            by_ord = {}
            for rec in recs:
                w = self.u32(rec)
                kind, ordinal = w >> 24, w & 0xFFFF
                imp = by_ord.get(ordinal)
                if imp is None:
                    imp = by_ord[ordinal] = Import(lib, ordinal)
                    self.imports.append(imp)
                    if lib == "xboxkrnl.exe" and ordinal in names:
                        imp.name, imp.kind = names[ordinal]
                if kind == 0:
                    imp.slot = rec
                else:
                    imp.thunk = rec
        for imp in self.imports:
            if imp.name is None:
                imp.name = "%s_%03X" % (imp.library.split(".")[0], imp.ordinal)
            if imp.kind is None:
                imp.kind = "function" if imp.thunk else "variable"

    def _parse_exports(self):
        """XEX export table (security info +0x160): magic[3], module
        number[2], version[3], image base >> 16, count, ordinal base, then
        count image offsets."""
        self.exports = {}
        self.export_table = None
        addr = self.xex.export_table
        if not addr:
            return
        words = [self.u32(addr + 4 * i) for i in range(11)]
        count, ord_base = words[9], words[10]
        if count > 0x1000:
            return
        img = words[8] << 16
        for i in range(count):
            off = self.u32(addr + 44 + 4 * i)
            if off:
                self.exports[ord_base + i] = img + off
        self.export_table = (addr, 44 + 4 * count)

    def thunk_of(self, name):
        for imp in self.imports:
            if imp.name == name:
                return imp.thunk
        return None
