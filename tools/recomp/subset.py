"""Keep a subset of XenonRecomp's output.

XenonRecomp recompiles every function of an image. For the front end only
the functions select_funcs.py chose are wanted: this rewrites the generated
sources with just those (and the register save/restore helpers), rewrites
the function mapping to match, and lists the functions the kept code calls
that were not kept, which the glue turns into host-call stubs.
"""
import os
import re

FUNC_START = re.compile(r'^__attribute__\(\(alias\("__imp__(\w+)"\)\)\) PPC_WEAK_FUNC\((\w+)\);$')
SUB = re.compile(r"\bsub_([0-9A-F]{8})\b")


def _functions(path):
    """Yield (name, text) for each function in a generated source file."""
    name, buf = None, []
    with open(path) as f:
        for line in f:
            m = FUNC_START.match(line.rstrip("\n"))
            if m:
                if name:
                    yield name, "".join(buf)
                name, buf = m.group(2), [line]
            elif name:
                buf.append(line)
    if name:
        yield name, "".join(buf)


def subset(src_dir, out_dir, keep, chunk_bytes=2 << 20):
    """keep: set of guest addresses. Returns (kept addresses, called but not
    kept addresses, generated lines that flag errors in kept code)."""
    os.makedirs(out_dir, exist_ok=True)
    for f in os.listdir(out_dir):
        if f.startswith("ppc_recomp.") and f.endswith(".cpp"):
            os.remove(os.path.join(out_dir, f))
    kept, called, errors = set(), set(), []
    files = sorted((f for f in os.listdir(src_dir) if f.startswith("ppc_recomp.") and f.endswith(".cpp")),
                   key=lambda f: int(f.split(".")[1]))
    out, size, n = [], 0, 0

    def flush():
        nonlocal out, size, n
        if out:
            with open(os.path.join(out_dir, "ppc_recomp.%d.cpp" % n), "w") as o:
                o.write('#include "ppc_recomp_shared.h"\n\n')
                o.write("".join(out))
            n += 1
        out, size = [], 0

    for fn in files:
        for name, text in _functions(os.path.join(src_dir, fn)):
            if name.startswith("sub_"):
                a = int(name[4:], 16)
                if a not in keep:
                    continue
                kept.add(a)
            for m in SUB.finditer(text):
                called.add(int(m.group(1), 16))
            for line in text.splitlines():
                if "// ERROR" in line:
                    errors.append("%s: %s" % (name, line.strip()))
            out.append(text)
            size += len(text)
            if size >= chunk_bytes:
                flush()
    flush()

    # the function mapping: kept functions and the helpers
    mapping = os.path.join(src_dir, "ppc_func_mapping.cpp")
    lines = []
    with open(mapping) as f:
        for line in f:
            m = re.match(r"\s*\{ 0x([0-9A-F]+), (\w+) \},", line)
            if m and m.group(2).startswith("sub_") and int(m.group(1), 16) not in kept:
                continue
            lines.append(line)
    with open(os.path.join(out_dir, "ppc_func_mapping.cpp"), "w") as o:
        o.write("".join(lines))
    for h in ("ppc_recomp_shared.h", "ppc_config.h", "ppc_context.h"):
        p = os.path.join(src_dir, h)
        if os.path.exists(p):
            data = open(p).read()
            dst = os.path.join(out_dir, h)
            if not os.path.exists(dst) or open(dst).read() != data:
                with open(dst, "w") as o:
                    o.write(data)
    return kept, called - kept, errors
