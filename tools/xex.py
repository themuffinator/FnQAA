#!/usr/bin/env python3
"""Decode Xbox 360 XEX2 images to their loaded PE image.

Used to turn Quake Arena Arcade's default.xex, qagameXe.dll and cgameXe.dll
into flat big-endian PowerPC images for analysis and static recompilation.

XEX2 facts used (Free60 / xenia documentation):
  0x00 'XEX2', module flags, PE data offset, reserved, security info offset,
       optional header count; then (key, value) optional headers. A key whose
       low byte is 0x00/0x01 holds its value inline, 0xFF points to a blob
       that starts with its own size, any other low byte N points to N dwords.
  security info: +0x004 image size, +0x110 load address, +0x150 AES key
       (session key, itself AES-ECB encrypted with the retail key).
  file format info (0x3FF): encryption (0 none, 1 AES-CBC zero IV over the
       whole payload) and compression (1 basic: data/zero runs; 2 normal:
       hash-chained blocks of 16-bit-prefixed chunks forming one raw LZX
       stream).
"""
import argparse
import ctypes
import os
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bpk import load_lzx  # noqa: E402

RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")
DEVKIT_KEY = bytes(16)

HEADER_NAMES = {
    0x000002FF: "resource_info", 0x000003FF: "file_format_info",
    0x00000405: "base_reference", 0x000080FF: "bounding_path",
    0x00010001: "original_base_address", 0x00010100: "entry_point",
    0x00010201: "image_base_address", 0x000103FF: "import_libraries",
    0x00018002: "checksum_timestamp", 0x000183FF: "original_pe_name",
    0x000200FF: "static_libraries", 0x00020104: "tls_info",
    0x00020200: "default_stack_size", 0x00020301: "default_fs_cache_size",
    0x00020401: "default_heap_size", 0x00028002: "page_heap_size_and_flags",
    0x00030000: "system_flags", 0x00040006: "execution_info",
    0x00040201: "title_workspace_size", 0x00040310: "game_ratings",
    0x00040404: "lan_key", 0x000405FF: "xbox360_logo",
    0x000406FF: "multidisc_media_ids", 0x000407FF: "alternate_title_ids",
    0x00040801: "additional_title_memory", 0x00E10402: "exports_by_name",
}


def aes_ecb_dec(key, data):
    d = Cipher(algorithms.AES(key), modes.ECB()).decryptor()
    return d.update(data) + d.finalize()


def aes_cbc_dec(key, data):
    n = len(data) & ~15
    d = Cipher(algorithms.AES(key), modes.CBC(bytes(16))).decryptor()
    return d.update(data[:n]) + d.finalize() + data[n:]


class Xex:
    def __init__(self, path):
        self.raw = open(path, "rb").read()
        r = self.raw
        if r[:4] != b"XEX2":
            raise ValueError("not an XEX2 image")
        self.flags, self.pe_offset, _, self.sec_offset, n = struct.unpack_from(">IIIII", r, 4)
        self.headers = {}
        for i in range(n):
            key, val = struct.unpack_from(">II", r, 0x18 + i * 8)
            self.headers[key] = val
        s = self.sec_offset
        self.image_size = struct.unpack_from(">I", r, s + 4)[0]
        self.image_flags = struct.unpack_from(">I", r, s + 0x10C)[0]
        self.load_address = struct.unpack_from(">I", r, s + 0x110)[0]
        self.export_table = struct.unpack_from(">I", r, s + 0x160)[0]
        self.enc_key = r[s + 0x150:s + 0x160]

    def blob(self, key):
        """The data an optional header points to, or None."""
        if key not in self.headers:
            return None
        val = self.headers[key]
        low = key & 0xFF
        if low in (0x00, 0x01):
            return struct.pack(">I", val)
        if low == 0xFF:
            size = struct.unpack_from(">I", self.raw, val)[0]
            return self.raw[val:val + size]
        return self.raw[val:val + low * 4]

    def value(self, key):
        return self.headers.get(key)

    def file_format(self):
        b = self.blob(0x000003FF)
        size, enc, comp = struct.unpack_from(">IHH", b, 0)
        return enc, comp, b

    def image(self):
        enc, comp, info = self.file_format()
        payload = self.raw[self.pe_offset:]
        if enc == 1:
            last = None
            for key in (RETAIL_KEY, DEVKIT_KEY):
                session = aes_ecb_dec(key, self.enc_key)
                img = self._decompress(aes_cbc_dec(session, payload), comp, info)
                last = img
                if img is not None and img[:2] == b"MZ":
                    return img
            raise ValueError("could not decrypt image (no MZ header)")
        img = self._decompress(payload, comp, info)
        if img is None:
            raise ValueError("decompression failed")
        return img

    def _decompress(self, data, comp, info):
        if comp == 0:
            return data[:self.image_size]
        if comp == 1:
            out = bytearray()
            p = 0
            for i in range(8, len(info), 8):
                dsize, zsize = struct.unpack_from(">II", info, i)
                out += data[p:p + dsize]
                out += bytes(zsize)
                p += dsize
            return bytes(out)
        if comp == 2:
            window = struct.unpack_from(">I", info, 8)[0]
            block_size = struct.unpack_from(">I", info, 12)[0]
            stream = bytearray()
            p = 0
            while block_size:
                nxt = p + block_size
                next_size = struct.unpack_from(">I", data, p)[0]
                q = p + 24
                while q + 2 <= nxt:
                    csize = (data[q] << 8) | data[q + 1]
                    q += 2
                    if csize == 0:
                        break
                    stream += data[q:q + csize]
                    q += csize
                p = nxt
                block_size = next_size
            bits = window.bit_length() - 1
            lzx = load_lzx()
            out = ctypes.create_string_buffer(self.image_size)
            err = lzx.qaa_lzx_decode_raw(bytes(stream), len(stream), out, self.image_size, bits)
            if err:
                return None
            return out.raw
        raise ValueError("unsupported compression type %d" % comp)

    def import_libraries(self):
        """[(library name, version, [import record addresses])]."""
        b = self.blob(0x000103FF)
        if not b:
            return []
        size, str_size, count = struct.unpack_from(">III", b, 0)
        names = b[12:12 + str_size].split(b"\0")
        names = [n.decode() for n in names if n]
        p = 12 + str_size
        libs = []
        for _ in range(count):
            lsize = struct.unpack_from(">I", b, p)[0]
            # size, digest[20], id, version, min version, name index (u16), count (u16)
            ver, minver = struct.unpack_from(">II", b, p + 0x1C)
            name_idx, n = struct.unpack_from(">HH", b, p + 0x24)
            recs = list(struct.unpack_from(">%dI" % n, b, p + 0x28))
            libs.append((names[name_idx], ver, recs))
            p += lsize
        return libs


def version_str(v):
    return "%d.%d.%d.%d" % (v >> 28, (v >> 24) & 15, (v >> 8) & 0xFFFF, v & 0xFF)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("xex")
    ap.add_argument("command", choices=["info", "image"])
    ap.add_argument("-o", "--out")
    a = ap.parse_args()
    x = Xex(a.xex)
    if a.command == "info":
        enc, comp, _ = x.file_format()
        print("flags %08X load %08X image size %08X enc %d comp %d" %
              (x.flags, x.load_address, x.image_size, enc, comp))
        for k, v in x.headers.items():
            print("  %08X %-24s %08X" % (k, HEADER_NAMES.get(k, "?"), v))
        pe = x.blob(0x000183FF)
        if pe:
            print("original PE name:", pe[4:].split(b"\0")[0].decode())
        for name, ver, recs in x.import_libraries():
            print("import %s %s: %d records" % (name, version_str(ver), len(recs)))
        return 0
    img = x.image()
    with open(a.out or os.path.splitext(a.xex)[0] + ".pe", "wb") as o:
        o.write(img)
    print("wrote %d bytes" % len(img))
    return 0


if __name__ == "__main__":
    sys.exit(main())
