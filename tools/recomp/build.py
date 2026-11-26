#!/usr/bin/env python3
"""Statically recompile Quake Arena Arcade's game modules from the user's package.

    python tools/recomp/build.py --package <STFS package, its zip, or an extracted folder>

Steps, all into the output directory (default build/recomp, ignored by git):

  1. find qagameXe.dll and cgameXe.dll in the package (read only)
  2. export XenonRecomp at the pinned commit, apply tools/recomp/patches,
     build XenonRecomp and XenonAnalyse with CMake and clang
  3. analyse each module (tools/recomp/analyse.py) into TOML configs
  4. run XenonAnalyse (cross-check) and XenonRecomp; collect every warning
  5. generate each module's glue (kernel thunks, descriptor, image)
  6. build the guest runtime, one native library per module and the
     headless gate harness with CMake (tools/recomp/CMakeLists.txt)

The pinned XenonRecomp commit is in tools/recomp/xenonrecomp.json. The
upstream checkout is only read (git archive); patches apply to the copy.
"""
import argparse
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))

MODULES = {"qagame": "qagameXe.dll", "cgame": "cgameXe.dll", "ui": "default.xex"}
# Modules recompiled from part of an image: the front end inside default.xex
# (docs/UI.md). The config picks the functions (tools/recomp/select_funcs.py).
SUBSETS = {"ui": os.path.join(ROOT, "data", "recomp", "ui.json")}
# Files read from the package: the modules, and the title executable for its XEX header.
INPUTS = dict(MODULES, executable="default.xex")
EXE_HEADER = "qaa_default_xex_header.bin"
PIN = json.load(open(os.path.join(HERE, "xenonrecomp.json")))


def log(msg):
    print("[recomp] " + msg, flush=True)


def run(cmd, cwd=None, capture=False, check=True, env=None):
    if not capture:
        log("$ " + " ".join(cmd))
    r = subprocess.run(cmd, cwd=cwd, env=env, text=True,
                       stdout=subprocess.PIPE if capture else None,
                       stderr=subprocess.STDOUT if capture else None)
    if check and r.returncode != 0:
        if capture:
            sys.stdout.write(r.stdout or "")
        raise SystemExit("command failed (%d): %s" % (r.returncode, " ".join(cmd)))
    return r


# ---- inputs ---------------------------------------------------------------

def find_modules(package, out):
    """Return {name: path of the module's XEX}, extracting from an STFS
    package (or a zip holding one) into out/input when needed."""
    found = {}
    if os.path.isdir(package):
        names = {f.lower(): os.path.join(package, f) for f in os.listdir(package)}
        for mod, fn in INPUTS.items():
            if fn.lower() in names:
                found[mod] = names[fn.lower()]
        return found
    inp = os.path.join(out, "input")
    os.makedirs(inp, exist_ok=True)
    stfs_path = package
    with open(package, "rb") as f:
        magic = f.read(4)
    if magic[:2] == b"PK":
        with zipfile.ZipFile(package) as z:
            members = [m for m in z.infolist() if not m.is_dir()]
            if len(members) != 1:
                raise SystemExit("%s: expected one STFS package in the zip, found %d files" % (package, len(members)))
            stfs_path = os.path.join(inp, "package.stfs")
            if not os.path.exists(stfs_path) or os.path.getsize(stfs_path) != members[0].file_size:
                log("unpacking %s from the zip" % members[0].filename)
                with z.open(members[0]) as src, open(stfs_path, "wb") as dst:
                    shutil.copyfileobj(src, dst, 1 << 20)
    from stfs import Stfs
    pkg = Stfs(stfs_path)
    for e in pkg.entries:
        for mod, fn in INPUTS.items():
            if not e["dir"] and e["path"].lower() == fn.lower():
                dst = os.path.join(inp, fn)
                data = pkg.read_file(e)
                if not os.path.exists(dst) or open(dst, "rb").read() != data:
                    with open(dst, "wb") as o:
                        o.write(data)
                found[mod] = dst
    return found


# ---- XenonRecomp -------------------------------------------------------------

def locate_xenonrecomp(arg):
    cands = [arg, os.environ.get("QAA_XENONRECOMP")]
    p = ROOT
    for _ in range(5):  # a sibling checkout of this repository (or of the main checkout)
        p = os.path.dirname(p)
        cands.append(os.path.join(p, "XenonRecomp"))
    for c in cands:
        if c and os.path.isdir(os.path.join(c, ".git")) or (c and os.path.isfile(os.path.join(c, ".git"))):
            return os.path.abspath(c)
    return None


def git(repo, *args, capture=True):
    return run(["git", "-c", "core.autocrlf=false", "-C", repo] + list(args), capture=capture)


def export_tree(repo, commit, dest):
    """git archive repo@commit (and its submodules, recursively) into dest."""
    data = subprocess.run(["git", "-c", "core.autocrlf=false", "-C", repo, "archive", "--format=tar", commit],
                          stdout=subprocess.PIPE, check=True).stdout
    with tarfile.open(fileobj=io.BytesIO(data)) as t:
        t.extractall(dest, filter="data") if sys.version_info >= (3, 12) else t.extractall(dest)
    tree = git(repo, "ls-tree", "-r", commit).stdout
    for line in tree.splitlines():
        meta, path = line.split("\t", 1)
        mode, kind, sha = meta.split()
        if kind == "commit":
            sub = os.path.join(repo, path)
            if not os.path.exists(os.path.join(sub, ".git")):
                raise SystemExit("submodule %s is not checked out in %s (run git submodule update --init)" % (path, repo))
            export_tree(sub, sha, os.path.join(dest, path))


def patch_files():
    d = os.path.join(HERE, "patches")
    return sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith(".patch"))


def recompiler_id():
    h = hashlib.sha256()
    for p in patch_files():
        h.update(open(p, "rb").read())
    return "XenonRecomp %s + FnQAA patches %s" % (PIN["commit"][:7], h.hexdigest()[:12])


def prepare_xenonrecomp(args, out):
    stamp_id = recompiler_id()
    xr = os.path.join(out, "xenonrecomp")
    src = os.path.join(xr, "src")
    stamp = os.path.join(xr, "src.stamp")
    if not (os.path.exists(stamp) and open(stamp).read() == stamp_id):
        repo = locate_xenonrecomp(args.xenonrecomp)
        if repo is None:
            repo = os.path.join(xr, "upstream")
            if not os.path.isdir(repo):
                log("cloning %s" % PIN["url"])
                run(["git", "clone", "--recurse-submodules", PIN["url"], repo])
            run(["git", "-C", repo, "checkout", "--recurse-submodules", PIN["commit"]])
        log("exporting XenonRecomp %s from %s" % (PIN["commit"][:7], repo))
        have = run(["git", "-C", repo, "cat-file", "-t", PIN["commit"]], capture=True, check=False)
        if have.returncode != 0 or have.stdout.strip() != "commit":
            raise SystemExit("%s does not have the pinned commit %s" % (repo, PIN["commit"]))
        if os.path.isdir(src):
            shutil.rmtree(src)
        os.makedirs(src)
        export_tree(repo, PIN["commit"], src)
        # The export may sit inside this repository's (ignored) build
        # directory; stop git from finding that repository, or git apply
        # silently skips paths outside the current directory.
        env = dict(os.environ, GIT_CEILING_DIRECTORIES=os.path.dirname(os.path.abspath(src)))
        for p in patch_files():
            log("applying %s" % os.path.basename(p))
            run(["git", "-c", "core.filemode=false", "apply", "--ignore-whitespace", "--whitespace=nowarn", "-v", p], cwd=src, env=env)
        for f in ("XenonRecomp/recompiler.cpp", "XenonUtils/xex.cpp"):
            if "FnQAA" not in open(os.path.join(src, f), encoding="utf-8", errors="replace").read():
                raise SystemExit("patches did not apply to %s" % f)
        with open(stamp, "w") as f:
            f.write(stamp_id)
    bld = os.path.join(xr, "build")
    exe = ".exe" if os.name == "nt" else ""
    recomp = os.path.join(bld, "XenonRecomp", "XenonRecomp" + exe)
    analyse = os.path.join(bld, "XenonAnalyse", "XenonAnalyse" + exe)
    cmake_configure(src, bld, [])
    run(["cmake", "--build", bld, "--target", "XenonRecomp", "XenonAnalyse", "--parallel", str(args.jobs)], capture=True)
    return src, recomp, analyse


def compilers():
    if os.name == "nt":
        return ["-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl", "-DCMAKE_LINKER=lld-link"]
    return ["-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++"]


def cmake_configure(src, bld, extra, build_type="Release"):
    cache = os.path.join(bld, "CMakeCache.txt")
    if os.path.exists(cache):
        run(["cmake", "-S", src, "-B", bld] + extra)
        return
    run(["cmake", "-S", src, "-B", bld, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=" + build_type] + compilers() + extra)


# ---- per module ------------------------------------------------------------------

WARNING_PATTERNS = [
    re.compile(r"^Unrecognized instruction"),
    re.compile(r"^Unable to decode"),
    re.compile(r"^ERROR"),
    re.compile(r"^WARNING"),
    re.compile(r"^Found a switch jump table"),
]


def recompile_module(mod, xex, out, xr_src, recomp_exe, analyse_exe, args):
    from analyse import Analysis
    from module_image import ModuleImage
    from glue import write_glue

    mdir = os.path.join(out, mod)
    ppc_dir = os.path.join(mdir, "ppc")
    os.makedirs(ppc_dir, exist_ok=True)
    kernel_table = os.path.join(xr_src, "XenonUtils", "xbox", "xboxkrnl_table.inc")
    log("%s: analysing %s" % (mod, xex))
    img = ModuleImage(xex, kernel_table)
    subset_cfg = None
    overrides = {}
    if mod in SUBSETS:
        subset_cfg = SUBSETS[mod]
        for k, v in json.load(open(subset_cfg)).get("switches", {}).items():
            overrides[int(k, 16)] = {"index": int(v["index"]), "count": int(v["count"]), "start": int(v["start"], 16)}
    an = Analysis(img, switch_overrides=overrides).run()
    rep = an.report()
    xex_rel = os.path.relpath(os.path.abspath(xex), mdir)
    gen_dir = "ppc_full" if subset_cfg else "ppc"
    if subset_cfg:
        ppc_dir = os.path.join(mdir, gen_dir)
        os.makedirs(ppc_dir, exist_ok=True)
    with open(os.path.join(mdir, mod + ".toml"), "w") as f:
        f.write(an.config_toml(xex_rel, gen_dir, mod + "_switch.toml", {}))
    with open(os.path.join(mdir, mod + "_switch.toml"), "w") as f:
        f.write(an.switch_toml())

    # XenonAnalyse's own jump-table scan, as a cross-check of ours
    xa_toml = os.path.join(mdir, mod + "_xenonanalyse.toml")
    run([analyse_exe, xex, xa_toml], capture=True)
    xa_bases = set(int(m, 16) for m in re.findall(r"^base = 0x([0-9A-Fa-f]+)", open(xa_toml).read(), re.M))
    ours = {sw["cmp"] for sw in an.switches.values()} | set(an.switches)
    xa_missing = sorted(b for b in xa_bases if not any(abs(b - o) <= 0x40 for o in ours))

    log("%s: recompiling" % mod)
    r = run([recomp_exe, os.path.join(mdir, mod + ".toml"), os.path.join(xr_src, "XenonUtils", "ppc_context.h")],
            capture=True, check=False)
    with open(os.path.join(mdir, "xenonrecomp.log"), "w") as f:
        f.write(r.stdout)
    if r.returncode != 0:
        sys.stdout.write(r.stdout)
        raise SystemExit("XenonRecomp failed on %s" % mod)
    warnings = [l for l in r.stdout.splitlines() if any(p.match(l) for p in WARNING_PATTERNS)]
    # The VMX helpers are optional: a module without them prints ERROR lines.
    absent = {n.split()[1] for n in an.notes if n.startswith("helper ")}
    expected = [w for w in warnings if any("__%s address is unspecified" % h in w for h in absent)]
    warnings = [w for w in warnings if w not in expected]

    # what the recompiler made of our function list
    funcs = set()
    for line in open(os.path.join(ppc_dir, "ppc_func_mapping.cpp")):
        m = re.match(r"\s*\{ 0x([0-9A-F]+), (\w+) \},", line)
        if m and m.group(2).startswith("sub_"):
            funcs.add(int(m.group(1), 16))
    expected_funcs = {b for b, _, _ in an.all_functions()}
    extra = sorted(funcs - expected_funcs)
    gen_errors = []
    for fn in sorted(os.listdir(ppc_dir)):
        if fn.startswith("ppc_recomp.") and fn.endswith(".cpp"):
            for line in open(os.path.join(ppc_dir, fn)):
                if "// ERROR" in line or "// WARNING" in line:
                    gen_errors.append(line.strip())

    hooks = ()
    selection = None
    if subset_cfg:
        from select_funcs import Selection
        from subset import subset
        code = [(sc.va, sc.va + sc.vsize) for sc in img.sections if sc.code]
        data = [(sc.va, sc.va + min(sc.vsize, sc.rsize)) for sc in img.sections if not sc.code and sc.name != ".pdata"]
        sel = Selection(img.data, img.base, [(b, n) for b, n, _ in an.all_functions()], code, data, subset_cfg).run()
        kept, called, sub_errors = subset(ppc_dir, os.path.join(mdir, "ppc"), sel.include)
        hooks = sorted(called)
        unexpected = sorted(set(hooks) - sel.hooks)
        selection = {"kept": len(kept), "kept_bytes": sum(sel.size[a] for a in kept), "hooks": ["%08X" % a for a in hooks],
                     "hooks_not_selected": ["%08X" % a for a in unexpected]}
        owner = sel.owner
        warnings = [w for w in warnings if not re.search(r"0x([0-9A-F]{8})", w)
                    or owner(int(re.search(r"0x([0-9A-F]{8})", w).group(1), 16)) in kept]
        gen_errors = sub_errors
        extra = [a for a in extra if a in kept]
        xa_missing = [b for b in xa_missing if owner(b) in kept]
        def problem_in_kept(p):
            m = re.search(r"[0-9A-F]{8}", p)
            return not m or owner(int(m.group(0), 16)) in kept
        # (problem messages name the bctr or function address first)
        rep["problems"] = [p for p in rep["problems"] if problem_in_kept(p)]
        log("%s: kept %d of %d functions (%d bytes), %d hooks" % (mod, len(kept), len(funcs), selection["kept_bytes"], len(hooks)))
    write_glue(os.path.join(mdir, "glue"), mod, rep, img.data, recompiler_id(),
               img.xex.raw[:img.xex.pe_offset], hooks=hooks, run_entry=not subset_cfg)

    summary = {
        "module": mod,
        "xex": xex,
        "functions": rep["functions"],
        "switches": rep["switches"],
        "indirect_jumps": len(rep["indirect_jumps"]),
        "seh_functions": len(rep["seh"]),
        "analysis_problems": rep["problems"],
        "analysis_notes": rep["notes"],
        "recompiler_warnings": warnings,
        "recompiler_expected": expected,
        "recompiler_extra_functions": ["%08X" % a for a in extra],
        "generated_errors": gen_errors,
        "xenonanalyse_tables": len(xa_bases),
        "xenonanalyse_tables_we_lack": ["%08X" % b for b in xa_missing],
        "subset": selection,
    }
    rep["build"] = summary
    with open(os.path.join(mdir, mod + "_analysis.json"), "w") as f:
        json.dump(rep, f, indent=1)
    fnc = rep["functions"]
    log("%s: %d functions (%d .pdata, %d outside .pdata, %d chunks), %d jump tables, %d SEH functions" % (
        mod, fnc["total"], fnc["pdata"], fnc["gap"], fnc["chunk"], rep["switches"], len(rep["seh"])))
    for w in rep["problems"]:
        log("%s: ANALYSIS PROBLEM: %s" % (mod, w))
    for w in warnings:
        log("%s: RECOMPILER: %s" % (mod, w))
    for w in gen_errors[:20]:
        log("%s: GENERATED: %s" % (mod, w))
    if extra:
        log("%s: XenonRecomp added %d functions the analysis did not list: %s" % (
            mod, len(extra), " ".join("%08X" % a for a in extra[:16])))
    if xa_missing:
        log("%s: XenonAnalyse found jump tables we did not: %s" % (mod, " ".join("%08X" % b for b in xa_missing)))
    return summary


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--package", required=True, help="STFS package, a zip of it, or a folder with the extracted files")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "recomp"), help="output directory")
    ap.add_argument("--xenonrecomp", help="XenonRecomp checkout (default: $QAA_XENONRECOMP or a sibling checkout)")
    ap.add_argument("--modules", default="qagame,cgame,ui")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--build-type", default="Release")
    ap.add_argument("--skip-native", action="store_true", help="stop after generating C++")
    ap.add_argument("--strict", action="store_true", help="fail on any analysis problem or recompiler warning")
    a = ap.parse_args()

    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    mods = [m.strip() for m in a.modules.split(",") if m.strip()]
    for m in mods:
        if m not in MODULES:
            raise SystemExit("unknown module %s (have %s)" % (m, ", ".join(MODULES)))
    found = find_modules(os.path.abspath(a.package), out)
    missing = [m for m in mods if m not in found]
    if missing:
        raise SystemExit("not in the package: %s" % ", ".join(MODULES[m] for m in missing))

    # default.xex's XEX header, for the runtime's RtlImageXexHeaderField
    exe_header = None
    if "executable" in found:
        from xex import Xex
        x = Xex(found["executable"])
        exe_header = os.path.join(out, EXE_HEADER)
        with open(exe_header, "wb") as f:
            f.write(x.raw[:x.pe_offset])
    else:
        log("default.xex is not in the package: modules will see no executable XEX header")

    xr_src, recomp_exe, analyse_exe = prepare_xenonrecomp(a, out)
    summaries = [recompile_module(m, found[m], out, xr_src, recomp_exe, analyse_exe, a) for m in mods]
    with open(os.path.join(out, "summary.json"), "w") as f:
        json.dump({"recompiler": recompiler_id(), "modules": summaries}, f, indent=1)

    bad = [s for s in summaries if s["analysis_problems"] or s["recompiler_warnings"] or s["generated_errors"]
           or s["recompiler_extra_functions"]]
    if bad and a.strict:
        raise SystemExit("analysis or recompilation problems (see %s/summary.json)" % out)

    if a.skip_native:
        return 0
    bld = os.path.join(out, "native")
    cmake_configure(HERE, bld, [
        "-DQAA_XENONRECOMP_SRC=" + xr_src.replace("\\", "/"),
        "-DQAA_GEN_DIR=" + out.replace("\\", "/"),
        "-DQAA_MODULES=" + ";".join(mods),
    ], a.build_type)
    run(["cmake", "--build", bld, "--parallel", str(a.jobs)])
    if exe_header:
        shutil.copyfile(exe_header, os.path.join(bld, "bin", EXE_HEADER))
    log("libraries in %s" % os.path.join(bld, "bin"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
