#!/usr/bin/env python3
"""Headless start/quit gates for the FnQAA client and dedicated server.

usage: headless_tests.py <fnqaa client> <fnqaa-ded server> [test names]

Every run is unattended: the client starts with com_headless 2 (no window),
sound off; the dedicated server is headless by default. No retail data is
needed: the test writes a synthetic XBLA package (STFS) whose baseq3/pak0.bpk
holds default.cfg and a second config under their name hashes, and checks that
the engine mounts the package in place, finds both by name, executes them and
reads a record by raw hash. When QAA_PACKAGE names the user's real package,
the server and the client also boot from it, executing QAA's own default.cfg,
and read its first BPK record by hash; otherwise those checks are skipped.

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
NESTED_MARKER = "QAA_SYNTHETIC_NESTED_CFG"
NESTED_NAME = "Cfg/Nested.CFG"          # looked up as exec cfg\nested.cfg
EXTRA_HASH = 0x5EED00000000E47A        # no name: reached only by raw hash
EXTRA = b"IBSP\x2f\x00\x00\x00 synthetic record read by raw hash\n"

_POLY, _SEED, _MASK = 0x0060034000F0D50B, 0xFAC432B10CD5E44A, (1 << 64) - 1


def _clmul_byte(i):
    c = 0
    for bit in range(7, -1, -1):
        c = (c << 1) & _MASK
        if (i >> bit) & 1:
            c ^= _POLY
    return c


_TABLE = [_clmul_byte(i) for i in range(256)]


def bpk_name_hash(path):
    """An independent copy of qaa_bpk_name_hash (code/qaa/qaa_bpk.c)."""
    h = _SEED
    for c in path.encode():
        if c == 0x2F:
            c = 0x5C
        elif 0x41 <= c <= 0x5A:
            c += 0x20
        v = c if c < 0x80 else (c - 0x100) & 0xFFFFFFFF   # sign extension, as qaa_bpk.c
        h = ((h << 8) & _MASK) ^ _TABLE[h >> 56] ^ v
    return h


assert bpk_name_hash("default.cfg") == 0xABF374818163C51F


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


def make_stfs(files, content_type=0x000D0000):
    """A read-only LIVE package; files: list of (path, bytes), at most one
    directory level, every file contiguous, all below 0xAA data blocks.
    content_type 0x000D0000 is an Arcade title, 0x00000001 a saved game."""
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
    header[0x344:0x348] = struct.pack(">I", content_type)
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
        env = dict(os.environ, FNQAA_HEADLESS="2", SDL_AUDIODRIVER="dummy")
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
        self.work = Path(tempfile.mkdtemp(prefix="fnqaa-headless-", dir=str(root)))
        self.engine = Engine(self.work)
        self.cfg = ("echo " + MARKER + "\nexec cfg\\nested.cfg\n").encode()
        nested = ("echo " + NESTED_MARKER + "\n").encode()
        bpk = make_bpk([(bpk_name_hash("default.cfg"), self.cfg), (bpk_name_hash(NESTED_NAME), nested),
                        (EXTRA_HASH, EXTRA)])
        self.bpk = bpk
        self.package = self.work / "package"
        self.package.write_bytes(make_stfs([("default.xex", b"synthetic"), ("baseq3/pak0.bpk", bpk)]))
        self.folder = self.work / "folder"
        (self.folder / "baseq3").mkdir(parents=True)
        (self.folder / "default.xex").write_bytes(b"synthetic")
        (self.folder / "baseq3" / "pak0.bpk").write_bytes(bpk)

    def tearDown(self):
        # only this test's own folder, inside the build tree, never through a link
        for path in [self.work, *self.work.rglob("*")]:
            self.assertFalse(path.is_symlink(), path)
        shutil.rmtree(self.work)

    def synthetic_run(self, exe, source):
        out = self.engine.run(exe, ["+set", "fs_qaaPackage", str(source),
                                    "+fs_restart", "+qaa_info", "+qaa_bpkread", "%016x" % EXTRA_HASH,
                                    "+wait", "3", "+quit"])
        self.assertIn(MARKER, out, "default.cfg was not executed from the BPK")
        self.assertIn(NESTED_MARKER, out, "cfg/nested.cfg was not found by name")
        self.assertEqual(out.count("Quake Arena Arcade "), 2, "fs_restart did not remount the package")
        self.assertIn("3 records", out)
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

    def test_client_binds_the_pad_as_qaa_and_the_keyboard_as_quake_live(self):
        # QAA's pad binds, under the Xbox 360's key names (default.cfg, then
        # its control scheme and the scheme's D-pad include)
        cfg = (b"unbindall\nbind UPARROW +forward\nbind MOUSE1 +attack\nbind MOUSE2 +strafe\n"
               b"exec controlschemes\\default.cfg\n")
        scheme = (b'bind JOY1 "lastweap;+button11;" // A button\n'
                  b'bind JOY8 "pause"           // start button\n'
                  b'bind MOUSE2 "+moveup;+button12;"       // left trigger\n'
                  b"exec controlschemes\\_dpadquick.cfg\n")
        dpad = b'bind UPARROW "vstr heavyweapons" \t// D-pad up arrow\n'
        bpk = make_bpk([(bpk_name_hash("default.cfg"), cfg),
                        (bpk_name_hash("controlschemes/default.cfg"), scheme),
                        (bpk_name_hash("controlschemes/_dpadquick.cfg"), dpad)])
        package = self.work / "binds"
        package.write_bytes(make_stfs([("default.xex", b"synthetic"), ("baseq3/pak0.bpk", bpk)]))
        out = self.engine.run(CLIENT, ["+set", "fs_qaaPackage", str(package), "+bindlist", "+wait", "3", "+quit"])
        binds = {}
        for line in out.splitlines():
            key, sep, rest = line.partition(' "')
            if sep and rest.endswith('"') and " " not in key:
                binds[key] = rest[:-1]
        self.assertEqual(binds.get("PAD0_SOUTH"), "lastweap;+button11;", out[-3000:])
        self.assertEqual(binds.get("PAD0_START"), "pause")
        self.assertEqual(binds.get("PAD0_RIGHTTRIGGER"), "+attack")
        self.assertEqual(binds.get("PAD0_LEFTTRIGGER"), "+moveup;+button12;")
        self.assertEqual(binds.get("PAD0_DPAD_UP"), "vstr heavyweapons")
        self.assertEqual(binds.get("PAD0_LEFTSTICK_UP"), "+forward")
        self.assertEqual(binds.get("PAD0_RIGHTSTICK_RIGHT"), "+right")
        # Quake Live's keyboard and mouse
        self.assertEqual(binds.get("MOUSE1"), "+attack")
        self.assertEqual(binds.get("MOUSE2"), "+zoom")
        self.assertEqual(binds.get("w"), "+forward")
        self.assertEqual(binds.get("SPACE"), "+moveup")
        self.assertEqual(binds.get("f"), "lastweap")
        self.assertEqual(binds.get("TAB"), "+scores")
        self.assertEqual(binds.get("KP_PGUP"), "follow 9")
        self.assertNotIn("UPARROW", binds)
        self.assertFalse([k for k in binds if k.startswith("JOY")], binds)

    # With fs_qaaPackage empty the engine finds the package where users put it.
    PACKAGE_NAME = "0ED9DD87783BD72BA56ED67ACD52BBFF1F47F08458"

    def place(self, root, *parts):
        dest = self.work / root / Path(*parts)
        dest.parent.mkdir(parents=True, exist_ok=True)
        if self.package.is_dir():
            shutil.copytree(self.package, dest)
        else:
            shutil.copyfile(self.package, dest)
        return dest

    def discovery_run(self, exe, where):
        out = self.engine.run(exe, ["+qaa_info", "+qaa_bpkread", "%016x" % EXTRA_HASH, "+wait", "3", "+quit"])
        self.assertIn("Found the Quake Arena Arcade package: " + str(where), out)
        self.assertIn(MARKER, out, "default.cfg was not executed from the found package")
        self.assertIn("qaa_bpkread %016x: %d bytes" % (EXTRA_HASH, len(EXTRA)), out)
        return out

    def test_server_finds_the_extracted_xbla_zip(self):
        where = self.place("base", "584108C8", "000D0000", self.PACKAGE_NAME)
        self.discovery_run(SERVER, where)

    def test_client_finds_an_xbox_content_folder(self):
        where = self.place("base", "Content", "0000000000000000", "584108C8", "000D0000", self.PACKAGE_NAME)
        out = self.discovery_run(CLIENT, where)
        self.assertIn("Client Shutdown (Client quit)", out)

    def test_server_finds_an_extracted_folder_in_home(self):
        self.package = self.folder
        where = self.place("home", "Quake Arena Arcade")
        out = self.discovery_run(SERVER, where)
        self.assertIn("Quake Arena Arcade folder", out)

    def test_server_finds_a_bare_package_file(self):
        where = self.place("base", self.PACKAGE_NAME)
        self.discovery_run(SERVER, where)

    def test_saves_and_other_titles_are_not_mounted(self):
        # a saved game of the same title, holding a valid pak0.bpk all the same
        save = make_stfs([("default.xex", b"synthetic"), ("baseq3/pak0.bpk", self.bpk)], content_type=0x00000001)
        (self.work / "base" / "584108C8" / "00000001").mkdir(parents=True)
        (self.work / "base" / "584108C8" / "00000001" / "save").write_bytes(save)
        (self.work / "base" / "savegame").write_bytes(save)
        out = self.engine.run(SERVER, ["+quit"], expect_code=1)
        self.assertNotIn("Found the Quake Arena Arcade package", out)
        self.assertIn("no Quake Arena Arcade package found", out)

    def test_missing_default_cfg_is_fatal(self):
        out = self.engine.run(SERVER, ["+quit"], expect_code=1)
        self.assertIn("Couldn't load default.cfg", out)

    @unittest.skipUnless(os.environ.get("QAA_PACKAGE"), "set QAA_PACKAGE to the user's STFS package")
    def test_real_package_record_by_hash(self):
        package = Path(os.environ["QAA_PACKAGE"])
        h, size = first_real_record(package)
        for exe in (SERVER, CLIENT):
            with self.subTest(exe=exe.name):
                out = self.engine.run(exe, ["+set", "fs_qaaPackage", str(package), "+qaa_info",
                                            "+qaa_bpkread", "%016x" % h, "+wait", "3", "+quit"])
                self.assertIn("title ID: 584108C8", out)
                self.assertIn("4916 records", out)
                self.assertNotIn(MARKER, out)       # QAA's own default.cfg, from the package
                self.assertIn("qaa_bpkread %016x: %d bytes" % (h, size), out)
                self.assertNotIn("SHORT READ", out)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    CLIENT, SERVER = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    unittest.main(argv=[sys.argv[0], "-v", *sys.argv[3:]])  # optional test names
