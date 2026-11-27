#!/usr/bin/env python3
"""Build FnQAA-pkg.fnz, the engine's own data archive, from pkg/.

The archive sits beside the engine and holds FnQAA's files only, never
retail data: pkg/baseq3/fnqaa.cfg, the PC defaults the engine runs right
after the package's default.cfg. The engine reads only the entries its
allow-list names (FS_RootArchiveAllowsFile in code/qcommon/files.c), so the
archive is not a general search path.

The bytes depend only on the files: entries are sorted, stamped 1980-01-01
and marked as Unix files, so every host builds the same archive.

  build_root_archive.py [--package-root pkg] [--output FnQAA-pkg.fnz]
  build_root_archive.py --check FnQAA-pkg.fnz
"""
from __future__ import annotations

import argparse
import os
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARCHIVE_NAME = "FnQAA-pkg.fnz"
PKG_ROOT = ROOT / "pkg"
# Entries the engine needs; a build without one fails.
REQUIRED = ("baseq3/fnqaa.cfg",)


def member_name(path: Path) -> str:
    name = path.as_posix()
    parts = name.split("/")
    if not name or name.startswith("/") or any(p in ("", ".", "..") or ":" in p for p in parts):
        raise ValueError(f"unsafe archive entry: {name!r}")
    if any(ord(c) < 32 or ord(c) == 127 for c in name):
        raise ValueError(f"control character in archive entry: {name!r}")
    return name


def assets(package_root: Path) -> list[tuple[Path, str]]:
    root = package_root.resolve()
    if not root.is_dir():
        raise FileNotFoundError(f"no package source folder {package_root}")
    out: list[tuple[Path, str]] = []
    seen: set[str] = set()
    for source in sorted(package_root.rglob("*"), key=lambda p: p.as_posix().lower()):
        if source.is_symlink():
            raise ValueError(f"package source must not be a link: {source}")
        if not source.is_file() or source.name == ".gitkeep":
            continue
        source.resolve().relative_to(root)  # raises if it escapes pkg/
        name = member_name(source.relative_to(package_root))
        if name.lower() in seen:
            raise ValueError(f"duplicate archive entry: {name}")
        seen.add(name.lower())
        out.append((source, name))
    return out


def check(names: list[str], label: str) -> None:
    for name in names:
        member_name(Path(name))
    missing = [n for n in REQUIRED if n not in names]
    if missing:
        raise ValueError(f"{label} is missing {', '.join(missing)}")


def write(output: Path, package_root: Path) -> None:
    entries = assets(package_root)
    check([name for _, name in entries], str(package_root))
    output.parent.mkdir(parents=True, exist_ok=True)
    temp = output.with_name(output.name + ".tmp")
    try:
        with zipfile.ZipFile(temp, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for source, name in entries:
                info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                info.create_system = 3  # the same bytes on Windows and Unix
                # LF line endings whatever the checkout's, for the same bytes everywhere
                archive.writestr(info, source.read_bytes().replace(b"\r\n", b"\n"))
        os.replace(temp, output)
    finally:
        if temp.exists():
            temp.unlink()


def check_archive(source, label: str | None = None) -> None:
    """Check an archive: a path, or a file object with its label."""
    label = label or Path(source).name
    with zipfile.ZipFile(source) as archive:
        if archive.testzip() is not None:
            raise ValueError(f"{label} has a corrupt entry")
        check([i.filename for i in archive.infolist() if not i.is_dir()], label)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--package-root", type=Path, default=PKG_ROOT)
    ap.add_argument("--output", type=Path, default=Path(ARCHIVE_NAME))
    ap.add_argument("--check", type=Path, help="check an existing archive instead of building one")
    a = ap.parse_args()
    try:
        if a.check:
            check_archive(a.check)
        else:
            write(a.output, a.package_root)
            check_archive(a.output)
    except (OSError, ValueError) as exc:
        print(f"build_root_archive.py: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
