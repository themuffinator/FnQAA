#!/usr/bin/env python3
"""Light static analysis of decoded Xbox 360 PE images (see tools/xex.py).

  funcs    list functions from .pdata
  xrefs    find code that builds an address with lis + addi/ori/load/store
  dis      disassemble the function containing an address
  strings  find strings matching a regex, with their addresses

Images are in loaded layout: file offset = VA - image base. Needs capstone.
"""
import argparse
import re
import struct
import sys

import capstone


class Image:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        optsz = struct.unpack_from("<H", d, pe + 20)[0]
        self.base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
        self.sections = {}
        sh = pe + 24 + optsz
        for i in range(nsec):
            name, vs, va = struct.unpack_from("<8sII", d, sh + i * 40)
            self.sections[name.rstrip(b"\0").decode()] = (self.base + va, vs)
        self.md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
        self._funcs = None

    def u32(self, va):
        return struct.unpack_from(">I", self.d, va - self.base)[0]

    def funcs(self):
        """[(start, end)] from .pdata: begin VA, then length in words in bits 8..29."""
        if self._funcs is None:
            va, size = self.sections[".pdata"]
            out = []
            for o in range(va - self.base, va - self.base + size, 8):
                begin, info = struct.unpack_from(">II", self.d, o)
                if begin == 0:
                    continue
                n = (info >> 8) & 0x3FFFFF
                out.append((begin, begin + n * 4))
            out.sort()
            self._funcs = out
        return self._funcs

    def func_at(self, va):
        import bisect
        f = self.funcs()
        i = bisect.bisect_right(f, (va, 0xFFFFFFFF)) - 1
        if i >= 0 and f[i][0] <= va < f[i][1]:
            return f[i]
        return None

    def text(self):
        va, size = self.sections[".text"]
        return va, va + size

    def xrefs(self, target, window=12):
        """Code addresses that form `target` from a lis high half and a low-16 use."""
        hi = (target + 0x8000) >> 16 & 0xFFFF
        lo = target & 0xFFFF
        start, end = self.text()
        hits = []
        d, b = self.d, self.base
        lis_at = {}
        for va in range(start, end, 4):
            w = struct.unpack_from(">I", d, va - b)[0]
            op = w >> 26
            rt = (w >> 21) & 31
            ra = (w >> 16) & 31
            imm = w & 0xFFFF
            if op == 15 and ra == 0:          # lis rt, imm
                if imm == hi:
                    lis_at[rt] = va
                elif rt in lis_at:
                    del lis_at[rt]
                continue
            # addi, ori(lo unsigned), lwz, lbz, stw, lfs, ld...
            if op in (14, 24, 32, 34, 36, 38, 40, 42, 44, 48, 50, 52, 54, 58, 62) and ra in lis_at:
                use_lo = imm if op != 24 else imm
                full = ((hi << 16) + (use_lo - 0x10000 if use_lo & 0x8000 and op != 24 else use_lo)) & 0xFFFFFFFF
                if op == 24:
                    full = (((target >> 16) & 0xFFFF) << 16) | imm
                if full == target and va - lis_at[ra] <= window * 4:
                    hits.append(va)
        return hits

    def disasm(self, start, end):
        code = self.d[start - self.base:end - self.base]
        for i in self.md.disasm(code, start):
            yield i

    def strings(self, pattern, minlen=4):
        for m in re.finditer(rb"[\x20-\x7e]{%d,}" % minlen, self.d):
            if re.search(pattern.encode(), m.group()):
                yield self.base + m.start(), m.group().decode()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image")
    ap.add_argument("command", choices=["funcs", "xrefs", "dis", "strings"])
    ap.add_argument("arg", nargs="?")
    a = ap.parse_args()
    img = Image(a.image)
    if a.command == "funcs":
        for s, e in img.funcs():
            print("%08x %08x" % (s, e))
    elif a.command == "xrefs":
        t = int(a.arg, 16)
        for va in img.xrefs(t):
            f = img.func_at(va)
            print("%08x in %s" % (va, "%08x" % f[0] if f else "?"))
    elif a.command == "dis":
        va = int(a.arg, 16)
        f = img.func_at(va) or (va, va + 0x200)
        for i in img.disasm(*f):
            w = struct.unpack_from(">I", img.d, i.address - img.base)[0]
            print("%08x  %08x  %-8s %s" % (i.address, w, i.mnemonic, i.op_str))
    elif a.command == "strings":
        for va, s in img.strings(a.arg):
            print("%08x %s" % (va, s))
    return 0


if __name__ == "__main__":
    sys.exit(main())
