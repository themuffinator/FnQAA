"""The user's side of the recompile: finding the package, checking the tools,
building the LZX helper and installing the built libraries next to FnQAA.

Used by build.py. Two layouts are recognised:

- a FnQAA release: the engine (fnqaa, fnqaa-ded) in the release folder and
  this kit in its setup/ subfolder (ROOT is setup/). The package is looked
  for in the release folder and the libraries are installed there, where the
  engine finds them.
- a source checkout: ROOT is the repository. The package is looked for in
  ref/ (AGENTS.md) and the libraries stay in build/recomp/native/bin, where
  every build directory finds them.

The package is found the way the engine finds it (code/qaa/qaa_fs.inc,
FS_QAA_FindPackage), and also as the XBLA zip itself.
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
WINDOWS = os.name == "nt"
TITLE_ID = 0x584108C8
ARCADE_TITLE = 0x000D0000           # STFS content type of an XBLA game (saves are 0x00000001)
ENGINE_NAMES = ("fnqaa.exe", "fnqaa-ded.exe") if WINDOWS else ("fnqaa", "fnqaa-ded")
LIBRARIES = re.compile(r"^(lib)?qaa_(guest_runtime|qagame|cgame|ui)\.(dll|so|dylib)$")
EXE_HEADER = "qaa_default_xex_header.bin"
ZIP_MEMBER = re.compile(r"(^|/)584108C8/000D0000/[^/]+$", re.IGNORECASE)


def log(msg):
    print("[recomp] " + msg, flush=True)


# ---- layout -----------------------------------------------------------------

def engine_dir():
    """The FnQAA release folder this kit is part of, or None in a source checkout."""
    for d in (os.path.dirname(ROOT), ROOT):
        if any(os.path.isfile(os.path.join(d, n)) for n in ENGINE_NAMES):
            return d
    return None


# ---- finding the package ----------------------------------------------------

def is_package(path):
    """The STFS package (title 584108C8, an Arcade title) or a folder of its files."""
    if os.path.isdir(path):
        return os.path.isfile(os.path.join(path, "baseq3", "pak0.bpk")) or \
            os.path.isfile(os.path.join(path, "default.xex"))
    try:
        with open(path, "rb") as f:
            head = f.read(0x364)
    except OSError:
        return False
    if len(head) < 0x364 or head[:4] not in (b"LIVE", b"CON ", b"PIRS"):
        return False
    content_type, = struct.unpack_from(">I", head, 0x344)
    title, = struct.unpack_from(">I", head, 0x360)
    return title == TITLE_ID and content_type == ARCADE_TITLE


def zip_member(path):
    """The package's member in a zip (the XBLA zip holds 584108C8/000D0000/<package>), or None."""
    try:
        with zipfile.ZipFile(path) as z:
            members = [m for m in z.infolist() if not m.is_dir()]
    except (OSError, zipfile.BadZipFile):
        return None
    named = [m for m in members if ZIP_MEMBER.search(m.filename)]
    if len(named) == 1:
        return named[0]
    return members[0] if len(members) == 1 and not named else None


def _entries(d):
    try:
        names = sorted(os.listdir(d))
    except OSError:
        return []
    return [os.path.join(d, n) for n in names]


def search_roots():
    eng = engine_dir()
    if eng:
        return [eng]
    return [os.path.join(ROOT, "ref", "package"), os.path.join(ROOT, "ref"), ROOT]


def find_package(roots=None):
    """(path, kind) of the first package found, kind 'package' or 'zip'; (None, None) if none."""
    roots = roots or search_roots()
    for root in roots:
        candidates = _entries(os.path.join(root, "584108C8", "000D0000"))
        for content in ("Content", "content"):
            for profile in _entries(os.path.join(root, content)):
                candidates += _entries(os.path.join(profile, "584108C8", "000D0000"))
        candidates += _entries(root)
        for c in candidates:
            if is_package(c):
                return c, "package"
    for root in roots:
        for c in _entries(root):
            if c.lower().endswith(".zip") and zip_member(c):
                return c, "zip"
    return None, None


def unpack_zip(path, dest_root):
    """Unpacks the package from the XBLA zip into dest_root/584108C8/000D0000/<package>,
    where the engine finds it; returns that path. The zip is only read."""
    with zipfile.ZipFile(path) as z:
        m = zip_member(path)
        name = m.filename.replace("\\", "/").rstrip("/").split("/")[-1]
        dest = os.path.join(dest_root, "584108C8", "000D0000", name)
        if os.path.isfile(dest) and os.path.getsize(dest) == m.file_size:
            return dest
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        log("unpacking %s from %s (%d MB, once)" % (name, os.path.basename(path), m.file_size >> 20))
        part = dest + ".part"
        with z.open(m) as src, open(part, "wb") as dst:
            shutil.copyfileobj(src, dst, 1 << 20)
        os.replace(part, dest)
    return dest


# ---- tools ------------------------------------------------------------------

def check_tools():
    """Fails with one message naming every missing tool and how to install it."""
    missing = []
    try:
        import cryptography  # noqa: F401
    except ImportError:
        missing.append(("Python package cryptography", "python -m pip install cryptography",
                        "python3 -m pip install cryptography (or: sudo apt install python3-cryptography)"))
    tools = [("git", "Git.Git", "git"), ("cmake", "Kitware.CMake", "cmake"), ("ninja", "Ninja-build.Ninja", "ninja-build")]
    tools += [("clang-cl", "LLVM.LLVM", None), ("lld-link", "LLVM.LLVM", None)] if WINDOWS else \
        [("clang", None, "clang"), ("clang++", None, "clang")]
    for exe, winget, apt in tools:
        if not shutil.which(exe):
            missing.append((exe, "winget install %s" % winget if winget else None,
                            "sudo apt install %s" % apt if apt else None))
    if WINDOWS and not _have_msvc():
        missing.append(("Visual Studio 2022 C++ build tools (the Windows SDK and C++ libraries clang-cl links with)",
                        'winget install Microsoft.VisualStudio.2022.BuildTools --override '
                        '"--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive"', None))
    if not missing:
        return
    lines = ["the recompile needs these, which were not found:"]
    for what, win, lin in missing:
        how = win if WINDOWS else lin
        lines.append("  - %s%s" % (what, (": " + how) if how else ""))
    lines.append("Install them, open a new terminal so PATH picks them up, and run this again.")
    raise SystemExit("\n".join(lines))


def _have_msvc():
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not os.path.isfile(vswhere):
        return bool(os.environ.get("VCToolsInstallDir"))
    r = subprocess.run([vswhere, "-latest", "-products", "*", "-requires",
                        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                       capture_output=True, text=True)
    return bool(r.stdout.strip())


def ensure_lzx(out):
    """Builds the LZX helper tools/xex.py decodes modules with (code/qaa/qaa_lzx.c)
    unless QAA_LZX_DLL names one, and points QAA_LZX_DLL at it."""
    given = os.environ.get("QAA_LZX_DLL")
    if given and os.path.isfile(given):
        return given
    srcs = [os.path.join(ROOT, "code", "qaa", n) for n in ("qaa_lzx.c", "qaa_bpk.c")]
    lib = os.path.join(out, "tools", "qaa_lzx" + (".dll" if WINDOWS else ".so"))
    newest = max(os.path.getmtime(s) for s in srcs)
    if not (os.path.isfile(lib) and os.path.getmtime(lib) >= newest):
        os.makedirs(os.path.dirname(lib), exist_ok=True)
        cmd = ["clang", "-O2", "-shared", "-DQAA_LZX_SHARED", "-o", lib] + srcs
        cmd[1:1] = ["-fuse-ld=lld"] if WINDOWS else ["-fPIC"]
        log("building the LZX helper")
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            sys.stdout.write(r.stdout + r.stderr)
            raise SystemExit("building the LZX helper failed: " + " ".join(cmd))
    os.environ["QAA_LZX_DLL"] = lib
    return lib


# ---- installing -------------------------------------------------------------

def install(bin_dir, dest):
    """Copies the runtime, the recompiled modules and the XEX header next to the engine."""
    names = [n for n in sorted(os.listdir(bin_dir)) if LIBRARIES.match(n) or n == EXE_HEADER]
    if EXE_HEADER not in names or not any(n for n in names if "guest_runtime" in n):
        raise SystemExit("%s does not hold the runtime and its header" % bin_dir)
    for n in names:
        shutil.copy2(os.path.join(bin_dir, n), os.path.join(dest, n))
    log("installed %s into %s" % (", ".join(names), dest))
