#!/usr/bin/env python3
"""Headless start/quit gates for the openQAA client and dedicated server.

usage: headless_tests.py <openqaa client> <openqaa-ded server> [test names]

Every run is unattended: the client starts with com_headless 2 (no window),
sound off; the dedicated server is headless by default. No retail data is
needed: the test writes a synthetic XBLA package (STFS) whose baseq3/pak0.bpk
holds default.cfg, names it through fs_qaaNameList and checks that the engine
mounts the package in place, executes that default.cfg and reads a record by
raw hash. When QAA_PACKAGE names the user's real package, the server and the
client also mount it and read its first BPK record by hash (a loose synthetic
default.cfg stands in until the BPK name hash is known); otherwise those
checks are skipped.

Windows also checks that no process window is ever created.
"""
from __future__ import annotations

import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import zlib
from pathlib import Path

CLIENT: Path
SERVER: Path

MARKER = "QAA_SYNTHETIC_DEFAULT_CFG"
CFG_HASH = 0x5EED0000DEFA0C46
EXTRA_HASH = 0x5EED00000000E47A
EXTRA = b"IBSP\x2f\x00\x00\x00 synthetic record read by raw hash\n"


def make_bpk(records):
    """records: list of (hash, bytes), stored raw (flags 0)."""
    table = 8 + 24 * len(records)
    head = struct.pack(">II", 1, len(records))
    body, offset, rows = b"", table, b""
    for h, data in records:
        rows += struct.pack(">QIIII", h, offset, len(data), 0, len(data))
        body += data
        offset += len(data)
    return head + rows + body


def make_stfs(files):
    """A read-only LIVE package; files: list of (path, bytes), at most one
    directory level, every file contiguous, all below 0xAA data blocks."""
    block = 0x1000
    entries, data_blocks, dirs = [], [], {}
    next_block = 1                      # data block 0 holds the file table
    for path, data in files:
        parts = path.split("/")
        parent = -1
        if len(parts) == 2:
            if parts[0] not in dirs:
                dirs[parts[0]] = len(entries)
                entries.append((parts[0], True, 0, 0, -1, 0))
            parent = dirs[parts[0]]
        count = max(1, (len(data) + block - 1) // block)
        entries.append((parts[-1], False, next_block, count, parent, len(data)))
        data_blocks.append((next_block, data))
        next_block += count
    assert next_block < 0xAA
    table = bytearray(block)
    for i, (name, is_dir, start, count, parent, size) in enumerate(entries):
        e = table[i * 0x40:(i + 1) * 0x40]
        e[:len(name)] = name.encode()
        e[0x28] = len(name) | 0x40 | (0x80 if is_dir else 0)
        e[0x29:0x2C] = count.to_bytes(3, "little")
        e[0x2C:0x2F] = count.to_bytes(3, "little")
        e[0x2F:0x32] = start.to_bytes(3, "little")
        e[0x32:0x34] = struct.pack(">h", parent)
        e[0x34:0x38] = struct.pack(">I", size)
        table[i * 0x40:(i + 1) * 0x40] = e
    header = bytearray(0x1000)
    header[0:4] = b"LIVE"
    header[0x340:0x344] = struct.pack(">I", 0x1000)
    header[0x344:0x348] = struct.pack(">I", 0x000D0000)
    header[0x360:0x364] = struct.pack(">I", 0x584108C8)
    header[0x37B] = 1
    header[0x37C:0x37E] = (1).to_bytes(2, "little")
    header[0x37E:0x381] = (0).to_bytes(3, "little")
    header[0x395:0x399] = struct.pack(">I", next_block)
    out = bytearray(header) + bytearray(block)          # backing block 0: hash table
    out += table                                        # data block 0 = backing 1
    for start, data in data_blocks:
        assert len(out) == 0x1000 + (start + 1) * block
        out += data + bytes((-len(data)) % block or (block if not data else 0))
    return bytes(out)


class Engine:
    def __init__(self, work: Path):
        self.work = work

    def run(self, exe: Path, args, timeout=90, expect_code=0):
        base = self.work / "base"
        home = self.work / "home"
        base.mkdir(exist_ok=True)
        home.mkdir(exist_ok=True)
        cmd = [str(exe), "+set", "fs_basepath", str(base), "+set", "fs_homepath", str(home),
               "+set", "net_port", "0", "+set", "net_port6", "0", "+set", "developer", "1", *args]
        if exe == CLIENT:
            cmd[1:1] = ["+set", "com_headless", "2", "+set", "s_initsound", "0", "+set", "in_nograb", "1"]
        env = dict(os.environ, OPENQAA_HEADLESS="2", SDL_AUDIODRIVER="dummy")
        log = self.work / "out.log"
        windows = set()
        with log.open("wb") as out:
            flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
            proc = subprocess.Popen(cmd, cwd=str(exe.parent), stdin=subprocess.DEVNULL, stdout=out,
                                    stderr=subprocess.STDOUT, env=env, creationflags=flags)
            try:
                deadline = time.monotonic() + timeout
                while proc.poll() is None and time.monotonic() < deadline:
                    windows |= process_windows(proc.pid)
                    time.sleep(0.02)
                if proc.poll() is None:
                    raise AssertionError("engine hung: " + " ".join(cmd))
            finally:
                if proc.poll() is None:
                    proc.kill()
                proc.wait(timeout=10)
        text = log.read_text(errors="replace")
        if proc.returncode != expect_code:
            raise AssertionError("exit %d (expected %d)\n%s" % (proc.returncode, expect_code, text[-4000:]))
        if windows:
            raise AssertionError("the process created windows: %r\n%s" % (windows, text[-2000:]))
        return text


def process_windows(pid):
    if os.name != "nt":
        return set()
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    found = set()
    proto = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def collect(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            found.add(hwnd)
        return True
    user32.EnumWindows(proto(collect), 0)
    return found


def first_real_record(package: Path):
    """(hash, decoded size) of pak0.bpk's first record, read with tools/stfs.py."""
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
    import stfs  # noqa: E402
    pkg = stfs.Stfs(str(package))
    entry = next(e for e in pkg.entries if e["path"] == "baseq3/pak0.bpk")
    head = pkg.read_block(entry["start"])
    pkg.f.close()
    h, _off, size, _flags, _stored = struct.unpack_from(">QIIII", head, 8)
    return h, size


class HeadlessTests(unittest.TestCase):
    def setUp(self):
        root = Path(os.environ.get("MESON_BUILD_ROOT", CLIENT.parent))
        self.work = Path(tempfile.mkdtemp(prefix="openqaa-headless-", dir=str(root)))
        self.engine = Engine(self.work)
        self.cfg = ("echo " + MARKER + "\n").encode()
        bpk = make_bpk([(CFG_HASH, self.cfg), (EXTRA_HASH, EXTRA)])
        self.package = self.work / "package"
        self.package.write_bytes(make_stfs([("default.xex", b"synthetic"), ("baseq3/pak0.bpk", bpk)]))
        self.folder = self.work / "folder"
        (self.folder / "baseq3").mkdir(parents=True)
        (self.folder / "default.xex").write_bytes(b"synthetic")
        (self.folder / "baseq3" / "pak0.bpk").write_bytes(bpk)
        self.names = self.work / "names.txt"
        self.names.write_text("# synthetic name list\n%016x default.cfg\n" % CFG_HASH)

    def tearDown(self):
        # only this test's own folder, inside the build tree, never through a link
        for path in [self.work, *self.work.rglob("*")]:
            self.assertFalse(path.is_symlink(), path)
        shutil.rmtree(self.work)

    def synthetic_run(self, exe, source):
        out = self.engine.run(exe, ["+set", "fs_qaaPackage", str(source), "+set", "fs_qaaNameList", str(self.names),
                                    "+fs_restart", "+qaa_info", "+qaa_bpkread", "%016x" % EXTRA_HASH,
                                    "+wait", "3", "+quit"])
        self.assertIn(MARKER, out, "default.cfg was not executed from the BPK")
        self.assertEqual(out.count("Quake Arena Arcade "), 2, "fs_restart did not remount the package")
        self.assertIn("fs_qaaNameList: 1 of 2 records named", out)
        self.assertIn("qaa_bpkread %016x: %d bytes" % (EXTRA_HASH, len(EXTRA)), out)
        self.assertIn("crc32 %08x, begins 'IBSP'" % zlib.crc32(EXTRA), out)
        self.assertNotIn("SHORT READ", out)
        return out

    def test_server_mounts_synthetic_package(self):
        out = self.synthetic_run(SERVER, self.package)
        self.assertIn("Quake Arena Arcade STFS package", out)
        self.assertIn("title ID: 584108C8", out)

    def test_server_mounts_folder(self):
        out = self.synthetic_run(SERVER, self.folder)
        self.assertIn("Quake Arena Arcade folder", out)

    def test_client_starts_headless_and_quits(self):
        out = self.synthetic_run(CLIENT, self.package)
        self.assertIn("Client Shutdown (Client quit)", out)

    def test_missing_default_cfg_is_fatal(self):
        out = self.engine.run(SERVER, ["+quit"], expect_code=1)
        self.assertIn("Couldn't load default.cfg", out)

    @unittest.skipUnless(os.environ.get("QAA_PACKAGE"), "set QAA_PACKAGE to the user's STFS package")
    def test_real_package_record_by_hash(self):
        package = Path(os.environ["QAA_PACKAGE"])
        h, size = first_real_record(package)
        base = self.work / "base" / "baseq3"
        base.mkdir(parents=True)
        (base / "default.cfg").write_bytes(self.cfg)    # until the BPK name hash is known
        for exe in (SERVER, CLIENT):
            with self.subTest(exe=exe.name):
                out = self.engine.run(exe, ["+set", "fs_qaaPackage", str(package), "+qaa_info",
                                            "+qaa_bpkread", "%016x" % h, "+wait", "3", "+quit"])
                self.assertIn("title ID: 584108C8", out)
                self.assertIn("4916 records", out)
                self.assertIn("qaa_bpkread %016x: %d bytes" % (h, size), out)
                self.assertNotIn("SHORT READ", out)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    CLIENT, SERVER = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    unittest.main(argv=[sys.argv[0], "-v", *sys.argv[3:]])  # optional test names
