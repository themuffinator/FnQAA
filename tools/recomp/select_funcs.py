#!/usr/bin/env python3
"""Choose which functions of a large image to recompile.

QAA's front end lives inside default.xex next to the engine, which FnQAA
replaces with FnQL. Only the front end's code is recompiled: the closure of
a set of root ranges through direct calls, code addresses formed in code
(function pointers) and code pointers stored in tables the code refers to
(vtables, callback tables). A function the closure reaches is

  - recompiled when it lies in a root or library range (or is listed in
    "recompile"), and its own references are followed;
  - otherwise a hook: the engine function the front end calls. The module
    gets a stub that hands the call to the host (docs/RECOMP.md), and the
    closure stops there.

The selection config is a JSON file of address facts (data/recomp/*.json);
this tool reads the user's image at build time and writes the lists.
"""
import bisect
import json
import struct


def _load(cfg_path):
    with open(cfg_path) as f:
        cfg = json.load(f)

    def ranges(key):
        return [(int(a, 16), int(b, 16)) for a, b in cfg.get(key, [])]

    def addrs(key):
        return {int(a, 16) for a in cfg.get(key, {})}

    return cfg, ranges("roots"), ranges("libraries"), addrs("recompile"), addrs("hooks"), ranges("root_tables")


class Selection:
    def __init__(self, image, base, functions, code_ranges, data_ranges, cfg_path):
        """image: the decoded image bytes at base; functions: [(start, size)];
        code_ranges / data_ranges: [(va, end)] of code and data sections."""
        self.image, self.base = image, base
        self.funcs = sorted(functions)
        self.starts = [s for s, _ in self.funcs]
        self.size = dict(self.funcs)
        self.code_ranges, self.data_ranges = code_ranges, data_ranges
        self.cfg, self.roots, self.libs, self.force_recompile, self.force_hook, self.root_tables = _load(cfg_path)

    def u32(self, va):
        return struct.unpack_from(">I", self.image, va - self.base)[0]

    def owner(self, va):
        i = bisect.bisect_right(self.starts, va) - 1
        if i >= 0 and va < self.starts[i] + self.funcs[i][1]:
            return self.starts[i]
        return None

    def in_ranges(self, va, rs):
        return any(a <= va < b for a, b in rs)

    def in_data(self, va):
        return self.in_ranges(va, self.data_ranges)

    def recompiled(self, f):
        if f in self.force_hook:
            return False
        return f in self.force_recompile or self.in_ranges(f, self.roots) or self.in_ranges(f, self.libs)

    # ---- references out of one function -------------------------------------
    def references(self, f):
        """(function starts, data addresses) referenced by f."""
        fns, data = set(), set()
        lis = {}
        for va in range(f, f + self.size[f], 4):
            w = self.u32(va)
            op = w >> 26
            if op == 18 and not (w & 2):    # b / bl: calls and tail calls
                off = w & 0x3FFFFFC
                if off & 0x2000000:
                    off -= 0x4000000
                t = (va + off) & 0xFFFFFFFF
                o = self.owner(t)
                if o is not None and o != f:
                    fns.add(o)
                continue
            if op == 16 and not (w & 2):    # bc leaving the function
                off = w & 0xFFFC
                if off & 0x8000:
                    off -= 0x10000
                t = (va + off) & 0xFFFFFFFF
                o = self.owner(t)
                if o is not None and o != f:
                    fns.add(o)
                continue
            rt, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            if op == 15 and ra == 0:        # lis
                lis[rt] = (imm, va)
                continue
            if ra in lis and op in (14, 24, 32, 34, 36, 38, 40, 42, 44, 48, 50, 52, 54, 58, 62):
                hi, lva = lis[ra]
                if va - lva <= 64:
                    if op == 24:
                        t = (hi << 16) | imm
                    else:
                        t = ((hi << 16) + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF
                    if t in self.size:
                        fns.add(t)
                    elif self.in_data(t):
                        data.add(t)
        return fns, data

    def table_functions(self, a, labels):
        """Code pointers in the table at a: up to the next address code
        refers to, or 1024 words."""
        out = set()
        for i in range(1024):
            va = a + 4 * i
            if i and va in labels:
                break
            if not self.in_data(va) or va - self.base + 4 > len(self.image):
                break
            v = self.u32(va)
            if v in self.size:
                out.add(v)
        return out

    # ---- the closure --------------------------------------------------------
    def run(self):
        refs = {}
        for f in self.starts:
            refs[f] = self.references(f)
        labels = set()
        for _, data in refs.values():
            labels |= data
        tables = {}
        include, hooks = set(), set()
        work = [f for f in self.starts if self.recompiled(f) and self.in_ranges(f, self.roots)]
        work += [f for f in self.force_recompile if f in self.size]
        # code pointers in the root tables (the CRT's initializer lists)
        for a, b in self.root_tables:
            for va in range(a, b, 4):
                v = self.u32(va)
                if v in self.size and self.recompiled(v):
                    work.append(v)
        seen = set(work)
        while work:
            f = work.pop()
            include.add(f)
            fns, data = refs[f]
            for d in data:
                if d not in tables:
                    tables[d] = self.table_functions(d, labels)
                fns = fns | tables[d]
            for g in fns:
                if g in seen:
                    continue
                seen.add(g)
                if self.recompiled(g):
                    work.append(g)
                else:
                    hooks.add(g)
        self.include, self.hooks = include, hooks
        return self

    def report(self):
        return {
            "include": sorted(self.include),
            "hooks": sorted(self.hooks),
            "include_bytes": sum(self.size[f] for f in self.include),
        }
