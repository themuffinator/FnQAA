#!/usr/bin/env python3
"""List and extract Quake Arena Arcade .bpk archives.

Layout (observed in baseq3/pak0.bpk, all big-endian):
  u32 version (1), u32 count
  count x { u64 name hash, u32 offset, u32 uncompressed size,
            u32 flags (1 = XMemCompress), u32 stored size }
  each stored file is a native XMemCompress stream (0x0FF512EE header).
Names are stored only as hashes.
"""
import argparse
import ctypes
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def load_lzx(path=None):
    path = path or os.environ.get("QAA_LZX_DLL") or os.path.join(HERE, "..", ".tmp", "qaa_lzx.dll")
    lib = ctypes.CDLL(os.path.abspath(path))
    lib.qaa_lzx_decode_native.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
    lib.qaa_lzx_decode_frames.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint]
    lib.qaa_lzx_error.restype = ctypes.c_char_p
    return lib


class Bpk:
    def __init__(self, path, lzx=None):
        self.data = open(path, "rb").read()
        self.version, n = struct.unpack_from(">II", self.data, 0)
        self.entries = [struct.unpack_from(">QIIII", self.data, 8 + i * 24) for i in range(n)]
        self.lzx = lzx or load_lzx()

    def read(self, e):
        h, off, usize, flags, csize = e
        src = self.data[off:off + csize]
        if flags == 0:
            return src
        out = ctypes.create_string_buffer(usize)
        err = self.lzx.qaa_lzx_decode_native(src, len(src), out, usize)
        if err:
            raise ValueError("%016x: %s" % (h, self.lzx.qaa_lzx_error(err).decode()))
        return out.raw


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("bpk")
    ap.add_argument("command", choices=["list", "extract", "verify"])
    ap.add_argument("-o", "--out", default="bpk")
    ap.add_argument("-n", "--names", help="name list from tools/bpk_names.py; extract to real paths")
    a = ap.parse_args()
    b = Bpk(a.bpk)
    names = {}
    if a.names:
        for line in open(a.names, encoding="latin-1"):
            if len(line) > 17:
                names[int(line[:16], 16)] = line[17:].rstrip("\r\n")
    bad = 0
    for e in b.entries:
        if a.command == "list":
            print("%016x %10d %10d %d %s" % (e[0], e[2], e[4], e[3], names.get(e[0], "")))
            continue
        try:
            d = b.read(e)
        except ValueError as ex:
            bad += 1
            print(ex, file=sys.stderr)
            continue
        if a.command == "extract":
            rel = names.get(e[0])
            if rel:
                dst = os.path.join(a.out, *rel.replace(chr(92), "/").split("/"))
            else:
                dst = os.path.join(a.out, "_unnamed", "%016x" % e[0])
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as o:
                o.write(d)
    if a.command != "list":
        print("%d entries, %d failed" % (len(b.entries), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
