#!/usr/bin/env python3
"""List and extract Xbox 360 STFS (CON/LIVE/PIRS) packages.

openQAA reads Quake Arena Arcade from the user's own XBLA package. This tool
is the reference reader used for investigation and tests; the engine has its
own reader.

Layout facts used here (Free60 STFS documentation):
  0x340  header size (BE u32); data starts at the next 4 KiB boundary
  0x379  volume descriptor: 0x37B flags (bit 0 -> one hash table per level),
         0x37C file table block count (LE u16), 0x37E first block (LE u24)
  file table entries are 0x40 bytes: name[0x28], flags (len | 0x40 contiguous
  | 0x80 directory), valid/alloc/start blocks (LE u24), parent (BE s16),
  size (BE u32), timestamps.
"""
import argparse
import fnmatch
import os
import struct
import sys

BLOCK = 0x1000
PER_TABLE = 0xAA


class Stfs:
    def __init__(self, path):
        self.f = open(path, "rb")
        hdr = self.f.read(0x400)
        self.magic = hdr[:4]
        if self.magic not in (b"CON ", b"LIVE", b"PIRS"):
            raise ValueError("not an STFS package: %r" % self.magic)
        self.header_size = struct.unpack_from(">I", hdr, 0x340)[0]
        self.content_type = struct.unpack_from(">I", hdr, 0x344)[0]
        self.title_id = struct.unpack_from(">I", hdr, 0x360)[0]
        flags = hdr[0x37B]
        # One table per level for read-only ("female") packages, two otherwise.
        self.shift = 0 if (flags & 1) else 1
        self.step0 = 0xAB if self.shift == 0 else 0xAC
        self.ft_count = struct.unpack_from("<H", hdr, 0x37C)[0]
        self.ft_block = int.from_bytes(hdr[0x37E:0x381], "little")
        self.alloc_blocks = struct.unpack_from(">I", hdr, 0x395)[0]
        self.base = (self.header_size + 0xFFF) & ~0xFFF
        self.entries = self._read_file_table()

    def _backing_data_block(self, n):
        s = self.shift
        r = (((n + 0xAA) // 0xAA) << s) + n
        if n < 0xAA:
            return r
        if n < 0x70E4:
            return r + (((n + 0x70E4) // 0x70E4) << s)
        return (1 << s) + r + (((n + 0x70E4) // 0x70E4) << s)

    def _level0_hash_block(self, n):
        s = self.shift
        if n < 0xAA:
            return 0
        r = (n // 0xAA) * self.step0
        r += ((n // 0x70E4) + 1) << s
        if n // 0x70E4 == 0:
            return r
        return r + (1 << s)

    def _addr(self, backing):
        return self.base + backing * BLOCK

    def read_block(self, n):
        self.f.seek(self._addr(self._backing_data_block(n)))
        return self.f.read(BLOCK)

    def next_block(self, n):
        self.f.seek(self._addr(self._level0_hash_block(n)) + (n % 0xAA) * 0x18 + 0x15)
        return int.from_bytes(self.f.read(3), "big")

    def _chain(self, start, count, contiguous):
        n = start
        for _ in range(count):
            yield n
            n = n + 1 if contiguous else self.next_block(n)

    def _read_file_table(self):
        raw = b"".join(self.read_block(b) for b in
                       self._chain(self.ft_block, self.ft_count, False))
        entries = []
        for i in range(0, len(raw), 0x40):
            e = raw[i:i + 0x40]
            fl = e[0x28]
            nlen = fl & 0x3F
            if nlen == 0:
                break
            entries.append({
                "index": i // 0x40,
                "name": e[:nlen].decode("ascii", "replace"),
                "dir": bool(fl & 0x80),
                "contiguous": bool(fl & 0x40),
                "valid": int.from_bytes(e[0x29:0x2C], "little"),
                "start": int.from_bytes(e[0x2F:0x32], "little"),
                "parent": struct.unpack_from(">h", e, 0x32)[0],
                "size": struct.unpack_from(">I", e, 0x34)[0],
            })
        by_index = {e["index"]: e for e in entries}
        for e in entries:
            parts, p = [e["name"]], e["parent"]
            while p != -1 and p in by_index:
                parts.append(by_index[p]["name"])
                p = by_index[p]["parent"]
            e["path"] = "/".join(reversed(parts))
        return entries

    def read_file(self, e):
        out = bytearray()
        for b in self._chain(e["start"], e["valid"], e["contiguous"]):
            out += self.read_block(b)
        return bytes(out[:e["size"]])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("package")
    ap.add_argument("command", choices=["info", "list", "extract"])
    ap.add_argument("glob", nargs="?", default="*")
    ap.add_argument("-o", "--out", default="extract")
    a = ap.parse_args()
    p = Stfs(a.package)
    if a.command == "info":
        print("magic %s title %08X content %08X header 0x%X shift %d blocks %d entries %d"
              % (p.magic.decode(), p.title_id, p.content_type, p.header_size,
                 p.shift, p.alloc_blocks, len(p.entries)))
        return
    for e in p.entries:
        if not fnmatch.fnmatch(e["path"], a.glob):
            continue
        if a.command == "list":
            print("%s %10d %s" % ("d" if e["dir"] else "-", e["size"], e["path"]))
        elif not e["dir"]:
            dst = os.path.join(a.out, *e["path"].split("/"))
            os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
            with open(dst, "wb") as o:
                o.write(p.read_file(e))
            print(e["path"])


if __name__ == "__main__":
    sys.exit(main())
