#!/usr/bin/env python3
"""Package and verify FnQAA release archives (.github/workflows/release.yml).

A release archive holds only FnQAA's own files: the client, the dedicated
server, the renderer modules, README.md, LICENSE, CREDITS.md, a generated
INSTALL.txt, and the setup kit: setup.cmd or setup.sh and, under setup/,
FnQAA's recompile tooling (tools/recomp/build.py and what it uses, the
guest runtime's source, the LZX decoder's source). setup.cmd / setup.sh
recompile the game modules on the user's machine from their own package
and install them beside the engine. An archive never holds retail data or
code derived from it (AGENTS.md): no ref/, nothing built by the recompile
and nothing extracted from the package. Its file list is fixed: an
unexpected file fails.

  package_release.py version
      print the version from version/fnqaa_version.h
  package_release.py package --platform windows --stage <meson install dir> --output <dir> [--archive-prefix P]
      write <P>-<platform>-x86_64.zip (Windows) or .tar.gz (Linux); P defaults
      to fnqaa-<version>, and the release workflow passes FnQL-style
      fnqaa-<version>-<YYYYMMDD>-<commit> from scripts/manual_release.py
  package_release.py verify <archive>...
      check an archive's file list and that its binaries import only system
      libraries
  package_release.py checksums <archive>... --output SHA256SUMS.txt [--manifest release-manifest.json --meta K=V ...]
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
import zipfile
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
ARCH = "x86_64"
RENDERERS = ("glx", "vk", "rtx")
DOCS = ("README.md", "LICENSE", "CREDITS.md")
# The setup kit, shipped under setup/ with the repository's layout (git-tracked
# and new, not ignored, files under these paths).
SETUP_KIT = (
    "tools/recomp", "tools/stfs.py", "tools/xex.py", "tools/bpk.py", "code/guest", "data/abi", "data/recomp",
    "code/qaa/qaa_lzx.c", "code/qaa/qaa_lzx.h", "code/qaa/qaa_bpk.c", "code/qaa/qaa_bpk.h",
)
SETUP_KIT_EXCLUDE = re.compile(r"^tools/recomp/tests/|__pycache__|\.pyc$")

# Names that must never reach an archive, whatever the allow-list says: the
# recompiled modules, retail files and the package itself.
FORBIDDEN = re.compile(
    r"(^|/)(ref|build|\.tmp)/|(^|/)native/bin/|(^|/)(lib)?qaa_(qagame|cgame|ui|guest_runtime|synthetic_module)\.(dll|so)$"
    r"|qagameXe|cgameXe|(^|/)default\.xex$|\.(xex|bpk|pe|xma|stfs|bin)$|0ED9DD87|(^|/)584108C8(/|$)",
    re.IGNORECASE,
)

# Windows: every import must be a DLL that ships with Windows. The CRT is
# linked statically (b_vscrt=static_from_buildtype), so the Visual C++
# redistributable's DLLs are not allowed even though a runner has them.
WINDOWS_NONSYSTEM = re.compile(r"^(vcruntime|msvcp|concrt|vccorlib|vcomp|ucrtbased)", re.IGNORECASE)
# Linux: the libraries every desktop distribution has. OpenAL, libcurl and the
# GL/Vulkan loaders are opened at run time, not linked.
LINUX_SYSTEM = {
    "libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1",
    "libstdc++.so.6", "libgcc_s.so.1", "libX11.so.6", "ld-linux-x86-64.so.2",
}


def version() -> str:
    text = (ROOT / "version" / "fnqaa_version.h").read_text()
    match = re.search(r'#define FNQAA_VERSION_STRING "([^"]+)"', text)
    if not match:
        raise SystemExit("FNQAA_VERSION_STRING not found in version/fnqaa_version.h")
    return match.group(1)


def binaries(platform: str) -> list[str]:
    exe, lib = (".exe", ".dll") if platform == "windows" else ("", ".so")
    return ["fnqaa" + exe, "fnqaa-ded" + exe] + [f"fnqaa_{r}_{ARCH}{lib}" for r in RENDERERS]


def launcher(platform: str) -> str:
    return "setup.cmd" if platform == "windows" else "setup.sh"


def setup_kit() -> list[str]:
    """The kit's files, as repository paths."""
    out = subprocess.run(["git", "-C", str(ROOT), "ls-files", "--cached", "--others", "--exclude-standard",
                          "--", *SETUP_KIT], check=True, capture_output=True, text=True).stdout
    files = sorted({f for f in out.splitlines() if f and not SETUP_KIT_EXCLUDE.search(f) and (ROOT / f).is_file()})
    if "tools/recomp/build.py" not in files:
        raise SystemExit("the setup kit is missing tools/recomp/build.py")
    return files


def expected_files(platform: str) -> list[str]:
    kit = ["setup/" + f for f in setup_kit()]
    return binaries(platform) + list(DOCS) + ["INSTALL.txt", launcher(platform)] + kit


def default_prefix() -> str:
    """fnqaa-<version>, as FnQL's release channel names archives; the release
    workflow passes the manual channel's fnqaa-<version>-<date>-<commit>
    (scripts/manual_release.py, archive_prefix)."""
    return f"fnqaa-{version()}"


# fnqaa-<version>[-<YYYYMMDD>-<commit>]-windows-x86_64.zip or -linux-x86_64.tar.gz
ARCHIVE_NAME = re.compile(r"^(?P<prefix>fnqaa-[0-9][A-Za-z0-9._+-]*)-"
                          r"(?P<platform>windows(?=-x86_64\.zip$)|linux(?=-x86_64\.tar\.gz$))-x86_64"
                          r"\.(?P<ext>zip|tar\.gz)$")


def archive_name(platform: str, prefix: str) -> str:
    return f"{prefix}-{platform}-{ARCH}" + (".zip" if platform == "windows" else ".tar.gz")


def install_text(platform: str, ver: str) -> str:
    win = platform == "windows"
    exe = "fnqaa.exe" if win else "./fnqaa"
    setup = "setup.cmd" if win else "./setup.sh"
    tools = ("""       winget install Python.Python.3.13 Git.Git Kitware.CMake Ninja-build.Ninja LLVM.LLVM
       python -m pip install cryptography
   and Visual Studio 2022, or its Build Tools, with the C++ workload (clang
   links with its Windows SDK and C++ libraries)."""
             if win else """       sudo apt install python3 python3-cryptography git cmake ninja-build clang""")
    openal = ("OpenAL Soft (soft_oal.dll or OpenAL32.dll beside fnqaa.exe,\nor installed)"
              if win else "OpenAL (libopenal.so.1, the libopenal1 package)")
    return f"""FnQAA {ver} ({platform} {ARCH})
==========================

FnQAA runs Quake Arena Arcade from your own copy of the game. This archive
holds the engine and a setup kit; it contains no game data and no game code.

1. Put your copy of the game in this folder, as you have it:
   - the XBLA zip ("Quake Arena Arcade (World) (XBLA).zip"), or that zip
     extracted (it makes 584108C8/000D0000/0ED9DD87...),
   - the Content folder copied from your Xbox 360 or its storage device
     (Content/0000000000000000/584108C8/000D0000/...), or
   - the package file itself (0ED9DD87...), or a folder of its files.
   Nothing needs extracting beyond that: the engine reads the game's data
   inside the package, and never changes it.

2. Run {setup} once. It finds the game, recompiles its game code on this
   machine (a few minutes) and installs it in this folder. It needs, and
   tells you if any is missing:
{tools}

3. Start {exe}.

Audio needs {openal}.

To keep the game elsewhere, run {setup} --package <path> and start
{exe} +set fs_qaaPackage <path>.

FnQAA is GPL-2.0 (LICENSE); CREDITS.md lists the engine's lineage.
"""


def check_name(name: str) -> None:
    if FORBIDDEN.search(name):
        raise SystemExit(f"refusing to package {name}: retail data or recompiled code never ships")


def package(platform: str, stage: Path, output: Path, prefix: str) -> Path:
    ver = version()
    base = archive_name(platform, prefix).removesuffix(".zip").removesuffix(".tar.gz")
    if not ARCHIVE_NAME.match(archive_name(platform, prefix)):
        raise SystemExit(f"archive prefix {prefix!r} does not give a valid archive name")
    sources: dict[str, Path | bytes] = {}
    for name in binaries(platform):
        path = stage / name
        if not path.is_file():
            raise SystemExit(f"{path} is missing: meson install did not stage it")
        sources[name] = path
    for name in DOCS:
        sources[name] = ROOT / name
    sources["INSTALL.txt"] = install_text(platform, ver).encode()
    # setup.cmd gets CRLF line endings (cmd.exe misreads labels in LF files)
    script = (ROOT / "scripts" / "release" / launcher(platform)).read_bytes().replace(b"\r\n", b"\n")
    sources[launcher(platform)] = script.replace(b"\n", b"\r\n") if platform == "windows" else script
    for name in setup_kit():
        sources["setup/" + name] = ROOT / name
    extra = sorted(p.relative_to(stage).as_posix() for p in stage.rglob("*")
                   if p.is_file() and p.relative_to(stage).as_posix() not in sources)
    if extra:
        # import libraries, debug files and the like stay out of the archive
        print("not packaged (not part of a release):", ", ".join(extra))
    for name in sources:
        check_name(name)

    output.mkdir(parents=True, exist_ok=True)
    archive = output / archive_name(platform, prefix)
    if platform == "windows":
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
            for name, src in sources.items():
                data = src if isinstance(src, bytes) else src.read_bytes()
                zf.writestr(f"{base}/{name}", data)
    else:
        mtime = int(os.environ.get("SOURCE_DATE_EPOCH") or time.time())
        with tarfile.open(archive, "w:gz") as tf:
            dirinfo = tarfile.TarInfo(base)
            dirinfo.type, dirinfo.mode, dirinfo.mtime = tarfile.DIRTYPE, 0o755, mtime
            tf.addfile(dirinfo)
            for name, src in sources.items():
                data = src if isinstance(src, bytes) else src.read_bytes()
                info = tarfile.TarInfo(f"{base}/{name}")
                info.size, info.mtime = len(data), mtime
                info.mode = 0o755 if name in binaries(platform) or name.endswith(".sh") else 0o644
                tf.addfile(info, io.BytesIO(data))
    print(f"wrote {archive} ({archive.stat().st_size} bytes)")
    return archive


def read_archive(archive: Path) -> tuple[str, dict[str, bytes]]:
    if archive.name.endswith(".zip"):
        with zipfile.ZipFile(archive) as zf:
            return "windows", {i.filename: zf.read(i) for i in zf.infolist() if not i.is_dir()}
    if archive.name.endswith(".tar.gz"):
        with tarfile.open(archive, "r:gz") as tf:
            files = {}
            for member in tf.getmembers():
                if member.isdir():
                    continue
                if not member.isfile():
                    raise SystemExit(f"{archive}: {member.name} is not a regular file")
                files[member.name] = tf.extractfile(member).read()
            return "linux", files
    raise SystemExit(f"{archive}: not a .zip or .tar.gz")


def pe_imports(data: bytes) -> list[str]:
    """The DLL names in a PE32+ image's import directory."""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE image")
    nsections = struct.unpack_from("<H", data, pe + 6)[0]
    optsize = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    if struct.unpack_from("<H", data, opt)[0] != 0x20B:
        raise ValueError("not a PE32+ (x86_64) image")
    import_rva = struct.unpack_from("<I", data, opt + 112 + 8)[0]
    sections = [struct.unpack_from("<IIII", data, opt + optsize + 40 * i + 8)
                for i in range(nsections)]  # VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData

    def offset(rva: int) -> int:
        for vsize, va, rawsize, raw in sections:
            if va <= rva < va + max(vsize, rawsize):
                return raw + rva - va
        raise ValueError(f"RVA {rva:#x} is outside every section")

    names, pos = [], offset(import_rva) if import_rva else None
    while pos is not None:
        name_rva = struct.unpack_from("<I", data, pos + 12)[0]
        if not name_rva:
            break
        start = offset(name_rva)
        names.append(data[start:data.index(b"\0", start)].decode("ascii"))
        pos += 20
    return names


def elf_needed(data: bytes) -> list[str]:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "elf"
        path.write_bytes(data)
        out = subprocess.run(["readelf", "-d", str(path)], check=True, capture_output=True, text=True).stdout
    return re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", out)


def verify(archive: Path, prefix: str | None = None) -> None:
    platform, files = read_archive(archive)
    match = ARCHIVE_NAME.match(archive.name)
    if not match or match["platform"] != platform:
        raise SystemExit(f"{archive.name}: not a FnQAA archive name (fnqaa-<version>...-<platform>-{ARCH})")
    if prefix and match["prefix"] != prefix:
        raise SystemExit(f"{archive.name}: expected {archive_name(platform, prefix)}")
    base = archive.name.removesuffix(".zip").removesuffix(".tar.gz")
    names = set()
    for name in files:
        check_name(name)
        path = PurePosixPath(name)
        if path.parts[0] != base or len(path.parts) < 2 or ".." in path.parts:
            raise SystemExit(f"{archive.name}: {name} is not under {base}/")
        names.add(PurePosixPath(*path.parts[1:]).as_posix())
    expected = set(expected_files(platform))
    if names != expected:
        raise SystemExit(f"{archive.name}: unexpected {sorted(names - expected)}, missing {sorted(expected - names)}")

    system32 = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32"
    for name in binaries(platform):
        data = files[f"{base}/{name}"]
        if platform == "windows":
            imports = pe_imports(data)
            bad = [dll for dll in imports if WINDOWS_NONSYSTEM.match(dll) or not (
                dll.lower().startswith(("api-ms-win-", "ext-ms-win-"))
                or (os.name != "nt" or (system32 / dll).is_file()))]
        else:
            if data[:4] != b"\x7fELF" or data[4] != 2:
                raise SystemExit(f"{archive.name}: {name} is not a 64-bit ELF file")
            imports = elf_needed(data)
            bad = [lib for lib in imports if lib not in LINUX_SYSTEM]
        if bad:
            raise SystemExit(f"{archive.name}: {name} links non-system libraries {bad} (all: {imports})")
        print(f"{name}: {', '.join(imports)}")
    print(f"{archive.name}: layout ok ({len(names)} files, no retail data)")


def checksums(archives: list[Path], output: Path, manifest: Path | None = None,
              release: dict[str, str] | None = None) -> None:
    """SHA256SUMS.txt and, as FnQL publishes, release-manifest.json (the release
    metadata and each archive's name, platform, size and SHA-256)."""
    archives = sorted(archives)
    sums = {a.name: hashlib.sha256(a.read_bytes()).hexdigest() for a in archives}
    lines = [f"{sums[a.name]}  {a.name}\n" for a in archives]
    output.write_text("".join(lines), newline="\n")
    print("".join(lines), end="")
    if manifest:
        data = dict(release or {})
        data["archives"] = [
            {"name": a.name, "platform": ARCHIVE_NAME.match(a.name)["platform"], "arch": ARCH,
             "size": a.stat().st_size, "sha256": sums[a.name]}
            for a in archives
        ]
        manifest.write_text(json.dumps(data, indent=2) + "\n", newline="\n")
        print(f"wrote {manifest}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="command", required=True)
    sub.add_parser("version")
    p = sub.add_parser("package")
    p.add_argument("--platform", choices=("windows", "linux"), required=True)
    p.add_argument("--stage", type=Path, required=True, help="the meson install prefix")
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--archive-prefix", help="default fnqaa-<version>; the workflow passes manual_release.py's")
    v = sub.add_parser("verify")
    v.add_argument("archives", type=Path, nargs="+")
    v.add_argument("--archive-prefix", help="require this prefix")
    c = sub.add_parser("checksums")
    c.add_argument("archives", type=Path, nargs="+")
    c.add_argument("--output", type=Path, required=True)
    c.add_argument("--manifest", type=Path, help="also write release-manifest.json here")
    c.add_argument("--meta", action="append", default=[], metavar="KEY=VALUE",
                   help="release metadata for the manifest (repeatable)")
    args = ap.parse_args(argv)

    if args.command == "version":
        print(version())
    elif args.command == "package":
        prefix = args.archive_prefix or default_prefix()
        verify(package(args.platform, args.stage.resolve(), args.output, prefix), prefix)
    elif args.command == "verify":
        for archive in args.archives:
            verify(archive, args.archive_prefix)
    else:
        meta = dict(item.split("=", 1) for item in args.meta)
        checksums(args.archives, args.output, args.manifest, meta)
    return 0


if __name__ == "__main__":
    sys.exit(main())
