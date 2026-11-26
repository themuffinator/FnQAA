#!/usr/bin/env python3
"""Generate XenonRecomp configs for an Xbox 360 module.

Reads the user's XEX2 module and writes, into a build directory:

  <name>.toml         XenonRecomp main config: register save/restore helper
                      addresses, explicit function boundaries for code that
                      .pdata does not cover, and words the recompiler must
                      skip (padding, SEH handler records, the export table).
  <name>_switch.toml  jump tables, found by emulating each bctr's dispatch
                      sequence for every index the bounds check admits.
  <name>_analysis.json
                      the facts the runtime and the build need (imports,
                      exports, sections, SEH scopes) plus every analysis
                      problem found.

The analysis does not trust XenonRecomp's own boundary scan: every byte of
every code section ends up in exactly one of .pdata functions, explicit
functions, import thunks, helpers or skipped data, and every branch that
leaves a function must land on a function start.
"""
import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ppc  # noqa: E402
from module_image import ModuleImage  # noqa: E402

# Byte patterns of the compiler's register save/restore helpers (from
# XenonRecomp's README) and the size of the 14 (or 64) entry point's body.
HELPERS = [
    ("restgprlr_14", bytes.fromhex("e9c1ff68"), 18 * 4 + 12),
    ("savegprlr_14", bytes.fromhex("f9c1ff68"), 18 * 4 + 8),
    ("restfpr_14", bytes.fromhex("c9ccff70"), 18 * 4 + 4),
    ("savefpr_14", bytes.fromhex("d9ccff70"), 18 * 4 + 4),
    ("restvmx_14", bytes.fromhex("3960fee07dcb60ce"), 18 * 8 + 4),
    ("savevmx_14", bytes.fromhex("3960fee07dcb61ce"), 18 * 8 + 4),
    ("restvmx_64", bytes.fromhex("3960fc00100b60cb"), 64 * 8 + 4),
    ("savevmx_64", bytes.fromhex("3960fc00100b61cb"), 64 * 8 + 4),
]

C_SPECIFIC_HANDLER = "__C_specific_handler"


class Problem(Exception):
    pass


class Analysis:
    def __init__(self, img, extra_functions=(), switch_overrides=None):
        self.img = img
        self.switch_overrides = switch_overrides or {}
        self.problems = []      # unresolved issues (fail the build when strict)
        self.notes = []         # resolved oddities worth reporting
        self.extra_functions = list(extra_functions)

    # ------------------------------------------------------------------
    def word(self, va):
        return self.img.u32(va)

    def code_words(self):
        for s in self.img.code_sections:
            for a in range(s.va, s.va + s.vsize - 3, 4):
                yield s, a, self.word(a)

    def run(self):
        self.find_helpers()
        self.find_fixed_regions()
        self.find_switches()
        self.find_references()
        self.partition()
        self.fix_branches()
        self.check()
        self.seh_dispatch()
        return self

    # ---- guest exception dispatch ---------------------------------------
    def seh_dispatch(self):
        """Where a guest exception can travel from RtlRaiseException to an
        __except handler, and how to unwind the frames in between.

        sites:  every call through which an exception may unwind in a
                function between a raise and a handler (or a handler's own
                function), with the innermost __except label around it
                (0 when there is none). The patched recompiler emits a
                PPC_SEH_RESUME check after each.
        unwind: for the functions whose frames may be popped, where their
                prologue saved non-volatile registers (offsets from the
                caller's stack pointer)."""
        self.seh_sites = {}
        self.unwind = {}
        raise_thunk = self.img.thunk_of("RtlRaiseException")
        except_fns = {s["function"]: s for s in self.seh if any(sc["target"] for sc in s["scopes"])}
        if not raise_thunk or not except_fns:
            return
        fns = [(b, n) for b, n, _ in self.img.pdata] + list(self.gap_functions)
        fns.sort()
        starts = [b for b, _ in fns]

        def owner(a):
            import bisect
            i = bisect.bisect_right(starts, a) - 1
            if i >= 0 and a < fns[i][0] + fns[i][1]:
                return fns[i][0]
            return None

        calls = {}		# function -> [(site, target)]
        for b, n in fns:
            for x in range(b, b + n, 4):
                w = self.word(x)
                if ppc.is_b(w) and ppc.lk(w):
                    calls.setdefault(b, []).append((x, ppc.b_target(w, x)))
        # functions that can raise, directly or through calls
        can_raise = {f for f, cs in calls.items() if any(t == raise_thunk for _, t in cs)}
        changed = True
        while changed:
            changed = False
            for f, cs in calls.items():
                if f not in can_raise and any(t in can_raise for _, t in cs):
                    can_raise.add(f)
                    changed = True

        def raising(t):
            return t == raise_thunk or t in can_raise

        def innermost_except(f, site):
            s = except_fns.get(f)
            if not s:
                return 0
            for sc in s["scopes"]:		# scope tables list inner scopes first
                if sc["target"] and sc["begin"] <= site < sc["end"]:
                    return sc["target"]
            return 0

        # frames an exception may unwind on its way to a handler: functions
        # reached (through raising calls) from calls inside __except scopes
        between = set()
        work = []
        for f, s in except_fns.items():
            for site, t in calls.get(f, []):
                if raising(t) and innermost_except(f, site) and t != raise_thunk:
                    work.append(t)
        while work:
            f = work.pop()
            if f in between:
                continue
            between.add(f)
            work += [t for _, t in calls.get(f, []) if t in can_raise]
        for f in between | set(except_fns):
            for site, t in calls.get(f, []):
                if raising(t):
                    self.seh_sites[site] = innermost_except(f, site)
        for f in between:
            info = self.decode_prologue(f)
            if info is None:
                self.problems.append("SEH: cannot decode the prologue of %08X, which an exception may unwind" % f)
            else:
                end = next(b + n for b, n in fns if b == f)
                self.unwind[f] = (end, info)

    def decode_prologue(self, f):
        """[(kind, reg, offset from the caller's SP)] for the registers the
        prologue of f saves, or None if it uses a form we do not know."""
        saves = []
        n = self.img.prolog.get(f)
        if not n:
            # no .pdata prologue length: scan to the stack update
            n = 16
        helpers = {}
        for name, (a, _) in self.helpers.items():
            count = 18 if name.endswith("_14") else 64
            step = 8 if "vmx" in name else 4
            for i in range(count):
                helpers[a + i * step] = (name, i)
        frame = 0		# bytes the prologue has moved r1 down so far
        r12 = None		# r12 = r1 + r12 (for __savefpr), relative to the caller's SP
        for k in range(n):
            x = f + 4 * k
            w = self.word(x)
            op = ppc.opcd(w)
            if ppc.is_b(w) and ppc.lk(w):
                t = ppc.b_target(w, x)
                h = helpers.get(t)
                if not h:
                    return None
                name, i = h
                if name == "savegprlr_14":
                    first = 14 + i
                    saves += [("gpr", r, -frame - 8 * (32 - r) - 8) for r in range(first, 32)]
                elif name == "savefpr_14":
                    if r12 is None:
                        return None
                    first = 14 + i
                    saves += [("fpr", r, r12 - 8 * (32 - r)) for r in range(first, 32)]
                else:
                    return None		# vector saves: not supported
            elif op == 62 and (w & 3) == 0 and ppc.ra(w) == 1:		# std rS, d(r1)
                saves.append(("gpr", ppc.rt(w), -frame + ppc.sext(w & 0xFFFC, 16)))
            elif op == 54 and ppc.ra(w) == 1:						# stfd fS, d(r1)
                saves.append(("fpr", ppc.rt(w), -frame + ppc.simm(w)))
            elif op == 37 and ppc.rt(w) == 1 and ppc.ra(w) == 1:	# stwu r1, -N(r1)
                frame -= ppc.simm(w)
            elif op == 14 and ppc.rt(w) == 12 and ppc.ra(w) == 1:	# addi r12, r1, K
                r12 = -frame + ppc.simm(w)
            elif op == 31 and ppc.xo10(w) == 183 and ppc.rt(w) == 1:	# stwux r1, r1, rX: dynamic frame
                frame = None
                break
        if frame is None:
            # A dynamic frame: saves made before the update are still
            # relative to the caller's SP; later ones are not supported.
            return [s for s in saves] if saves else []
        return saves

    # ---- helpers -------------------------------------------------------
    def find_helpers(self):
        self.helpers = {}
        for name, pat, size in HELPERS:
            hits = []
            for s in self.img.code_sections:
                lo = s.va - self.img.base
                hi = lo + s.vsize
                i = self.img.data.find(pat, lo, hi)
                while i >= 0:
                    if i % 4 == 0:
                        hits.append(self.img.base + i)
                    i = self.img.data.find(pat, i + 1, hi)
            if len(hits) == 1:
                self.helpers[name] = (hits[0], size)
            elif len(hits) > 1:
                self.problems.append("helper %s matches %d times: %s" % (
                    name, len(hits), ", ".join("%08X" % h for h in hits)))
            else:
                self.notes.append("helper %s not present" % name)

    # ---- import thunks, SEH records, export table, padding -------------
    def find_fixed_regions(self):
        img = self.img
        self.thunks = {}  # address -> import
        for imp in img.imports:
            if imp.thunk:
                self.thunks[imp.thunk] = imp
        self.seh_handler = img.thunk_of(C_SPECIFIC_HANDLER)
        self.fixed = []  # (start, size, kind)
        for a, imp in self.thunks.items():
            self.fixed.append((a, 16, "thunk"))
        for name, (a, size) in self.helpers.items():
            self.fixed.append((a, size, "helper"))
        self.seh = []
        for begin, length, exc in img.pdata:
            if self.seh_handler and self.word(begin - 8) == self.seh_handler:
                scope = self.word(begin - 4)
                n = self.word(scope)
                recs = []
                for i in range(min(n, 64)):
                    b, e, h, t = (self.word(scope + 4 + 16 * i + 4 * k) for k in range(4))
                    recs.append({"begin": b, "end": e, "handler": h, "target": t})
                self.seh.append({"function": begin, "scope_table": scope, "scopes": recs})
                self.fixed.append((begin - 8, 8, "seh"))
        if img.export_table and img.in_code(img.export_table[0]):
            a, size = img.export_table
            self.fixed.append((a, size, "exports"))
            self.export_word = self.word(a)
        else:
            self.export_word = None
        self.fixed.sort()

    # ---- jump tables ---------------------------------------------------
    def find_switches(self):
        self.switches = {}
        self.indirect_jumps = []
        self.dispatch = set()  # addresses inside switch dispatch sequences
        for s, a, w in self.code_words():
            if w != ppc.BCTR:
                continue
            if any(t <= a < t + 16 for t in self.thunks):
                continue
            try:
                sw = self.decode_switch(a)
            except Problem as e:
                if self.looks_like_table_jump(a):
                    self.problems.append("bctr at %08X looks like a jump table but %s" % (a, e))
                else:
                    self.indirect_jumps.append(a)
                continue
            self.switches[a] = sw
            for x in range(sw["cmp"], a + 4, 4):
                self.dispatch.add(x)

    def looks_like_table_jump(self, a):
        """True when the register moved to CTR is computed from an indexed
        load (lwzx/lhzx/lbzx) rather than loaded as a plain pointer."""
        src = None
        k = 1
        for k in range(1, 5):
            w = self.word(a - 4 * k)
            if ppc.is_mtctr(w):
                src = ppc.rt(w)
                break
        if src is None:
            return False
        want = {src}
        for j in range(k + 1, k + 12):
            w = self.word(a - 4 * j)
            op = ppc.opcd(w)
            if op == 31 and ppc.xo10(w) in (23, 279, 87, 343) and ppc.rt(w) in want:
                return True
            if op == 31 and ppc.xo10(w) == 266 and ppc.rt(w) in want:
                want |= {ppc.ra(w), ppc.rb(w)}
            elif op in (32, 34, 40, 42) and ppc.rt(w) in want:
                return False
            if ppc.is_terminator(w):
                return False
        return False

    def decode_switch(self, bctr):
        ov = self.switch_overrides.get(bctr)
        if ov:
            return self.decode_switch_override(bctr, ov)
        # mtctr rX within the 4 preceding instructions
        mt = None
        for k in range(1, 5):
            if ppc.is_mtctr(self.word(bctr - 4 * k)):
                mt = bctr - 4 * k
                break
        if mt is None:
            raise Problem("no mtctr before it")
        # the bounds check: cmplwi crN, rI, K; ... bgt/bge crN, default
        br = None
        for k in range(1, 24):
            a = bctr - 4 * k
            w = self.word(a)
            if ppc.is_b(w) or ppc.is_bclr(w) or ppc.is_bcctr(w):
                raise Problem("a branch at %08X precedes the bounds check" % a)
            if ppc.is_bc(w) and not ppc.lk(w):
                br = a
                break
        if br is None:
            raise Problem("no bounds check")
        w = self.word(br)
        bo, bi = ppc.bc_bo(w), ppc.bc_bi(w)
        crf, bit = bi >> 2, bi & 3
        if (bo & 0x1C) == 0x0C and bit == 1:       # bgt: index <= K valid
            inclusive = True
        elif (bo & 0x1C) == 0x04 and bit == 0:     # bge: index < K valid
            inclusive = False
        else:
            raise Problem("bounds branch at %08X is not bgt/bge" % br)
        default = ppc.bc_target(w, br)
        cmp = None
        for k in range(1, 9):
            a = br - 4 * k
            cw = self.word(a)
            if ppc.is_cmplwi(cw) and ppc.cmp_crf(cw) == crf:
                cmp = a
                break
            if ppc.is_b(cw) or ppc.is_bc(cw) or ppc.is_bclr(cw) or ppc.is_bcctr(cw):
                break
        if cmp is None:
            raise Problem("no cmplwi for the bounds branch at %08X" % br)
        cw = self.word(cmp)
        index_reg, bound = ppc.ra(cw), ppc.uimm(cw)
        count = bound + 1 if inclusive else bound
        if count <= 0 or count > 4096:
            raise Problem("implausible case count %d" % count)
        # The compiler sometimes recomputes the index from its source
        # (`addi rI, rX, k` before the check and `addi rY, rX, k` after it),
        # so seed rX as well when rI was formed that way.
        seed = None
        for k in range(1, 9):
            a = cmp - 4 * k
            pw = self.word(a)
            if ppc.is_b(pw) or ppc.is_bc(pw) or ppc.opcd(pw) == 19:
                break
            if ppc.opcd(pw) == 14 and ppc.rt(pw) == index_reg and ppc.ra(pw) not in (0, index_reg):
                seed = (ppc.ra(pw), ppc.simm(pw))
                break
            if index_reg in (ppc.rt(pw), ppc.ra(pw)) and ppc.opcd(pw) not in (36, 37, 38, 39, 44, 45, 10, 11):
                break
        labels = []
        for i in range(count):
            regs = {index_reg: i}
            if seed:
                regs[seed[0]] = (i - seed[1]) & 0xFFFFFFFF
            ctr = self.emulate(cmp + 4, bctr, regs, skip=br)
            if ctr is None:
                raise Problem("dispatch target is not computable")
            if regs.get(index_reg) != i:
                raise Problem("index register r%d is overwritten" % index_reg)
            if not self.img.in_code(ctr) or ctr & 3:
                raise Problem("case %d lands outside code at %08X" % (i, ctr))
            labels.append(ctr)
        return {"base": bctr, "cmp": cmp, "r": index_reg, "default": default, "labels": labels}

    def decode_switch_override(self, bctr, ov):
        """A table whose bounds check is not next to it (a state machine
        that checks its state at the bottom of the loop): the config names
        the index register, the case count and where the dispatch starts."""
        index_reg, count, start = ov["index"], ov["count"], ov["start"]
        labels = []
        for i in range(count):
            regs = {index_reg: i}
            ctr = self.emulate(start, bctr, regs)
            if ctr is None or not self.img.in_code(ctr) or ctr & 3:
                raise Problem("override: case %d is not computable" % i)
            labels.append(ctr)
        return {"base": bctr, "cmp": start, "r": index_reg, "default": bctr + 4, "labels": labels}

    def emulate(self, start, end, regs, skip=None):
        """Run the straight-line code [start, end) with partially known
        registers; return the value moved to CTR, or None."""
        img = self.img
        ctr = None

        def get(r):
            return regs.get(r)

        def put(r, v):
            regs[r] = None if v is None else v & 0xFFFFFFFF

        for a in range(start, end, 4):
            if a == skip:
                continue
            w = self.word(a)
            op = ppc.opcd(w)
            d, s_, b = ppc.rt(w), ppc.ra(w), ppc.rb(w)
            if op in (14, 15):  # addi / addis
                imm = ppc.simm(w) << (16 if op == 15 else 0)
                base = 0 if s_ == 0 else get(s_)
                put(d, None if base is None else base + imm)
            elif op in (24, 25):  # ori / oris (rA = rS | imm)
                v = get(d)
                put(s_, None if v is None else v | (ppc.uimm(w) << (16 if op == 25 else 0)))
            elif op == 21:  # rlwinm
                v = get(d)
                sh, mb, me = ppc.rlwinm_fields(w)
                put(s_, None if v is None else ppc.rotl32(v, sh) & ppc.mask32(mb, me))
            elif op in (32, 34, 40, 42):  # lwz lbz lhz lha
                base = 0 if s_ == 0 else get(s_)
                if base is None:
                    put(d, None)
                else:
                    ea = (base + ppc.simm(w)) & 0xFFFFFFFF
                    put(d, self.load(op, ea))
            elif op == 31:
                xo = ppc.xo10(w)
                if xo == 467:  # mtspr
                    if ppc.is_mtctr(w):
                        ctr = get(d)
                elif xo == 266:  # add
                    x, y = get(s_), get(b)
                    put(d, None if x is None or y is None else x + y)
                elif xo == 40:  # subf
                    x, y = get(s_), get(b)
                    put(d, None if x is None or y is None else y - x)
                elif xo == 444:  # or / mr
                    x, y = get(d), get(b)
                    put(s_, None if x is None or y is None else x | y)
                elif xo in (23, 87, 279, 343):  # lwzx lbzx lhzx lhax
                    x = 0 if s_ == 0 else get(s_)
                    y = get(b)
                    if x is None or y is None:
                        put(d, None)
                    else:
                        put(d, self.load({23: 32, 87: 34, 279: 40, 343: 42}[xo], (x + y) & 0xFFFFFFFF))
                elif xo in (954, 922):  # extsb / extsh
                    v = get(d)
                    put(s_, None if v is None else ppc.sext(v, 8 if xo == 954 else 16))
                else:
                    put(d, None)
                    put(s_, None)
            elif ppc.is_b(w) or ppc.is_bc(w) or op == 19:
                return None
            elif op in (48, 50, 59, 63, 4):
                pass  # lfs, lfd, FP arithmetic and VMX write no GPR
            elif op in (49, 51):
                put(s_, None)  # lfsu, lfdu update rA
            else:
                # Anything else: assume it may write either register field.
                if op not in (36, 37, 38, 39, 44, 45, 52, 53, 54, 55, 62, 10, 11):  # stores, compares
                    put(d, None)
                    put(s_, None)
        return ctr

    def load(self, op, ea):
        img = self.img
        if not img.stored(ea, 4 if op == 32 else 2 if op in (40, 42) else 1):
            return None
        if op == 32:
            return img.u32(ea)
        if op == 34:
            return img.u8(ea)
        if op == 40:
            return img.u16(ea)
        return ppc.sext(img.u16(ea), 16)

    # ---- references into code --------------------------------------------
    def find_references(self):
        img = self.img
        self.bl_targets = set()
        self.branch_out = {}   # filled after partition
        self.code_refs = set()
        self.data_refs = set()
        self.labels = set()
        for sw in self.switches.values():
            self.labels.update(sw["labels"])
        lis = {}
        for s, a, w in self.code_words():
            if ppc.is_b(w) and ppc.lk(w):
                self.bl_targets.add(ppc.b_target(w, a))
            op = ppc.opcd(w)
            if op == 15 and ppc.ra(w) == 0:
                lis[ppc.rt(w)] = (a, (ppc.simm(w) << 16) & 0xFFFFFFFF)
            elif op == 14 and ppc.ra(w) in lis:
                la, hi = lis[ppc.ra(w)]
                if a - la <= 32 and a not in self.dispatch:
                    t = (hi + ppc.simm(w)) & 0xFFFFFFFF
                    if img.in_code(t) and not t & 3:
                        self.code_refs.add(t)
            if ppc.is_terminator(w):
                lis.clear()
        for s in img.sections:
            if s.code or s.name in (".pdata",):
                continue
            end = s.va + min(s.vsize, s.rsize)
            for a in range(s.va, end - 3, 4):
                if not img.stored(a):
                    break
                v = img.u32(a)
                if img.in_code(v) and not v & 3:
                    self.data_refs.add(v)
        self.refs = (self.bl_targets | self.code_refs | self.data_refs | set(img.exports.values())
                     | ({img.entry} if img.entry else set())) - self.labels

    # ---- function boundaries ---------------------------------------------
    def occupied(self):
        """Sorted (start, end, kind) of everything that is not gap code."""
        occ = [(b, b + n, "pdata") for b, n, _ in self.img.pdata]
        occ += [(a, a + n, k) for a, n, k in self.fixed]
        return sorted(occ)

    def gap_segments(self):
        occ = self.occupied()
        segs = []
        for s in self.img.code_sections:
            cur, end = s.va, s.va + s.vsize
            for b, e, _ in occ:
                if e <= cur or b >= end:
                    continue
                if b > cur:
                    segs.append((cur, b))
                cur = max(cur, e)
            if cur < end:
                segs.append((cur, end))
        out = []
        for a, e in segs:
            # zero padding at either end is skipped by the recompiler
            while a < e and self.word(a) == 0:
                a += 4
            while e > a and self.word(e - 4) == 0:
                e -= 4
            if a < e:
                out.append((a, e))
        return out

    def partition(self, forced=()):
        self.forced = set(forced) | set(self.extra_functions)
        self.gap_functions = []
        for a, e in self.gap_segments():
            starts = {x for x in self.refs | self.forced if a <= x < e}
            starts.add(a)
            self.gap_functions += self.split_segment(a, e, starts)

    def split_segment(self, a, e, starts):
        funcs = []
        cur = a
        reach = a
        x = a
        while x < e:
            w = self.word(x)
            if w == 0:
                # padding inside a segment ends the function before it
                if x > cur:
                    funcs.append((cur, x - cur))
                while x < e and self.word(x) == 0:
                    x += 4
                cur = reach = x
                continue
            if ppc.is_b(w) and not ppc.lk(w):
                t = ppc.b_target(w, x)
                if cur <= t < e and t > x:
                    reach = max(reach, t)
            elif ppc.is_bc(w) and not ppc.lk(w):
                t = ppc.bc_target(w, x)
                if cur <= t < e and t > x:
                    reach = max(reach, t)
            sw = self.switches.get(x)
            if sw:
                for t in sw["labels"]:
                    if t > x:
                        reach = max(reach, t)
            n = x + 4
            if n < e and (n in starts or (self.ends_flow(x, w) and n > reach)):
                funcs.append((cur, n - cur))
                cur = reach = n
            x = n
        if x > cur:
            funcs.append((cur, x - cur))
        return funcs

    def ends_flow(self, x, w):
        return ppc.is_terminator(w)

    def all_functions(self):
        fns = [(b, n, "pdata") for b, n, _ in self.img.pdata]
        fns += [(b, n, "gap") for b, n in self.gap_functions]
        fns += [(b, n, "chunk") for b, n in getattr(self, "chunks", [])]
        return sorted(fns)

    def symbol_starts(self):
        s = {b for b, _, _ in self.all_functions()}
        s |= set(self.thunks)
        for name, (a, size) in self.helpers.items():
            n = 18 if name.endswith("_14") else 64
            step = 8 if "vmx" in name else 4
            s |= {a + i * step for i in range(n)}
        return s

    def fix_branches(self):
        """Every branch leaving its function must reach a function start.
        Targets inside gap code become new starts; targets inside a .pdata
        function become overlapping 'chunk' functions running to its end.
        SEH filter and termination-handler funclets start chunks too: the
        runtime calls them when it dispatches a guest exception."""
        self.chunks = []
        pdata = [(b, n) for b, n, _ in self.img.pdata]
        for s in self.seh:
            for sc in s["scopes"]:
                h = sc["handler"]
                if h < 0x10000:		# a constant filter (EXCEPTION_EXECUTE_HANDLER)
                    continue
                owner = [(b, n) for b, n in pdata if b <= h < b + n]
                if owner and h != owner[0][0]:
                    self.chunks.append((h, owner[0][0] + owner[0][1] - h))
        self.chunks = sorted(set(self.chunks))
        for _ in range(16):
            starts = self.symbol_starts()
            fns = self.all_functions()
            new_gap, new_chunks = set(), set()
            for b, n, kind in fns:
                for x in range(b, b + n, 4):
                    w = self.word(x)
                    t = None
                    if ppc.is_b(w):
                        t = ppc.b_target(w, x)
                        if not ppc.lk(w) and b <= t < b + n:
                            continue
                    elif ppc.is_bc(w) and not ppc.lk(w):
                        t = ppc.bc_target(w, x)
                        if b <= t < b + n:
                            continue
                    if t is None or t in starts:
                        continue
                    if not self.img.in_code(t):
                        self.problems.append("branch at %08X leaves the code sections (%08X)" % (x, t))
                        continue
                    owner = [f for f in fns if f[0] <= t < f[0] + f[1]]
                    if owner and owner[0][2] in ("pdata", "chunk"):
                        new_chunks.add((t, owner[0][0] + owner[0][1] - t))
                    else:
                        new_gap.add(t)
            if not new_gap and not new_chunks:
                return
            self.chunks = sorted(set(self.chunks) | new_chunks)
            if new_gap:
                self.partition(self.forced | new_gap)
        self.problems.append("branch fix-up did not converge")

    # ---- final checks ----------------------------------------------------
    def check(self):
        fns = self.all_functions()
        skip_words = {0}
        if self.seh_handler:
            skip_words.add(self.seh_handler)
        if self.export_word is not None:
            skip_words.add(self.export_word)
        for b, n, kind in fns:
            if self.word(b) in skip_words:
                self.problems.append("function %08X starts with a skipped word %08X" % (b, self.word(b)))
        for a, sw in self.switches.items():
            owner = [f for f in fns if f[0] <= a < f[0] + f[1]]
            if not owner:
                self.problems.append("switch at %08X is in no function" % a)
                continue
            for f in owner:
                bad = [t for t in sw["labels"] if not f[0] <= t < f[0] + f[1]]
                if bad and f[2] != "chunk":
                    self.problems.append("switch at %08X: %d labels outside function %08X" % (a, len(bad), f[0]))
        # Words that decode to nothing the recompiler can skip must not sit
        # where its sweep would try to analyse them.
        covered = sorted([(b, b + n) for b, n, _ in fns] + [(a, a + n) for a, n, _ in self.fixed])
        for s in self.img.code_sections:
            cur = s.va
            for b, e in covered:
                if e <= cur:
                    continue
                if b > cur:
                    for x in range(cur, b, 4):
                        if self.word(x) != 0:
                            self.problems.append("uncovered code word at %08X" % x)
                            break
                cur = max(cur, e)

    # ---- output ------------------------------------------------------------
    def config_toml(self, xex_rel, out_rel, switch_rel, options):
        lines = ["# Generated by tools/recomp/analyse.py from %s. Build output." % os.path.basename(self.img.path),
                 "[main]",
                 'file_path = "%s"' % xex_rel.replace("\\", "/"),
                 'out_directory_path = "%s"' % out_rel.replace("\\", "/"),
                 'switch_table_file_path = "%s"' % switch_rel.replace("\\", "/")]
        for k, v in options.items():
            lines.append("%s = %s" % (k, "true" if v else "false"))
        for name, _, _ in HELPERS:
            if name in self.helpers:
                lines.append("%s_address = 0x%08X" % (name, self.helpers[name][0]))
        lines.append("functions = [")
        for b, n, kind in self.all_functions():
            if kind != "pdata":
                lines.append("    { address = 0x%08X, size = 0x%X }, # %s" % (b, n, kind))
        lines.append("]")
        lines.append("invalid_instructions = [")
        lines.append("    { data = 0x00000000, size = 4 }, # padding")
        if self.seh and self.seh_handler:
            lines.append("    { data = 0x%08X, size = 8 }, # __C_specific_handler, scope table" % self.seh_handler)
        if self.export_word is not None:
            lines.append("    { data = 0x%08X, size = %d }, # XEX export table" % (self.export_word, self.img.export_table[1]))
        lines.append("]")
        for site in sorted(getattr(self, "seh_sites", {})):
            lines.append("")
            lines.append("[[seh_site]]")
            lines.append("address = 0x%08X" % site)
            lines.append("target = 0x%08X" % self.seh_sites[site])
        return "\n".join(lines) + "\n"

    def switch_toml(self):
        out = ["# Generated by tools/recomp/analyse.py. Build output."]
        for a in sorted(self.switches):
            sw = self.switches[a]
            out.append("[[switch]]")
            out.append("base = 0x%08X" % a)
            out.append("r = %d" % sw["r"])
            out.append("default = 0x%08X" % sw["default"])
            out.append("labels = [")
            out += ["    0x%08X," % t for t in sw["labels"]]
            out.append("]")
            out.append("")
        return "\n".join(out) + "\n"

    def report(self):
        img = self.img
        fns = self.all_functions()
        return {
            "module": img.original_name,
            "base": img.base, "image_size": img.size, "entry": img.entry,
            "stored_size": len(img.data),
            "sections": [{"name": s.name, "va": s.va, "vsize": s.vsize, "rsize": s.rsize,
                          "flags": s.flags} for s in img.sections],
            "exports": {str(k): v for k, v in sorted(img.exports.items())},
            "imports": [{"library": i.library, "ordinal": i.ordinal, "name": i.name,
                         "kind": i.kind, "slot": i.slot, "thunk": i.thunk} for i in img.imports],
            "helpers": {k: v[0] for k, v in self.helpers.items()},
            "functions": {"pdata": len(img.pdata),
                          "gap": len(self.gap_functions),
                          "chunk": len(self.chunks),
                          "total": len(fns)},
            "function_list": [[b, n, k] for b, n, k in fns],
            "switches": len(self.switches),
            "indirect_jumps": self.indirect_jumps,
            "seh": self.seh,
            "seh_sites": [[s, t] for s, t in sorted(getattr(self, "seh_sites", {}).items())],
            "unwind": [{"function": f, "end": e, "saves": [list(s) for s in saves]}
                       for f, (e, saves) in sorted(getattr(self, "unwind", {}).items())],
            "notes": self.notes,
            "problems": self.problems,
        }


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("xex", help="the module's XEX2 file (read only)")
    ap.add_argument("-o", "--out-dir", required=True)
    ap.add_argument("--name", help="config base name (default: module name)")
    ap.add_argument("--kernel-table", help="xenia-style xboxkrnl_table.inc for import names")
    ap.add_argument("--ppc-dir", default=None, help="recompiler output directory (default <name>_ppc)")
    a = ap.parse_args()
    img = ModuleImage(a.xex, a.kernel_table)
    name = a.name or os.path.splitext(img.original_name)[0]
    an = Analysis(img).run()
    os.makedirs(a.out_dir, exist_ok=True)
    ppc_dir = a.ppc_dir or name + "_ppc"
    os.makedirs(os.path.join(a.out_dir, ppc_dir), exist_ok=True)
    xex_rel = os.path.relpath(os.path.abspath(a.xex), os.path.abspath(a.out_dir))
    with open(os.path.join(a.out_dir, name + ".toml"), "w") as f:
        f.write(an.config_toml(xex_rel, ppc_dir, name + "_switch.toml", {}))
    with open(os.path.join(a.out_dir, name + "_switch.toml"), "w") as f:
        f.write(an.switch_toml())
    rep = an.report()
    with open(os.path.join(a.out_dir, name + "_analysis.json"), "w") as f:
        json.dump(rep, f, indent=1)
    fn = rep["functions"]
    print("%s: %d functions (%d .pdata, %d gap, %d chunk), %d switches, %d indirect jumps, %d SEH functions" % (
        name, fn["total"], fn["pdata"], fn["gap"], fn["chunk"], rep["switches"],
        len(rep["indirect_jumps"]), len(rep["seh"])))
    for n in rep["notes"]:
        print("  note:", n)
    for p in rep["problems"]:
        print("  PROBLEM:", p)
    return 1 if rep["problems"] else 0


if __name__ == "__main__":
    sys.exit(main())
