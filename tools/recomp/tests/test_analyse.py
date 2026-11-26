#!/usr/bin/env python3
"""Unit tests for tools/recomp/analyse.py on synthetic PowerPC code.

    python tools/recomp/tests/test_analyse.py

Every image here is assembled by the test; no retail data is used.
"""
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import ppc  # noqa: E402
from analyse import Analysis  # noqa: E402
from module_image import ModuleImage, Section  # noqa: E402

BASE = 0x82000000
RDATA = BASE + 0x1000
TEXT = BASE + 0x2000


# ---- a tiny assembler ----------------------------------------------------------

def lis(rd, imm): return (15 << 26) | (rd << 21) | (imm & 0xFFFF)
def addi(rd, ra, imm): return (14 << 26) | (rd << 21) | (ra << 16) | (imm & 0xFFFF)
def li(rd, imm): return addi(rd, 0, imm)
def rlwinm(ra, rs, sh, mb, me): return (21 << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1)
def slwi(ra, rs, n): return rlwinm(ra, rs, n, 0, 31 - n)
def x31(rd, ra, rb, xo): return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (xo << 1)
def lwzx(rd, ra, rb): return x31(rd, ra, rb, 23)
def lhzx(rd, ra, rb): return x31(rd, ra, rb, 279)
def lbzx(rd, ra, rb): return x31(rd, ra, rb, 87)
def add(rd, ra, rb): return x31(rd, ra, rb, 266)
def lwz(rd, ra, d): return (32 << 26) | (rd << 21) | (ra << 16) | (d & 0xFFFF)
def mtctr(rs): return 0x7C0903A6 | (rs << 21)
def cmplwi(crf, ra, imm): return (10 << 26) | (crf << 23) | (ra << 16) | imm


NOP, BLR, BCTR = 0x60000000, 0x4E800020, 0x4E800420


class Asm:
    def __init__(self, at):
        self.at = at
        self.words = []
        self.fix = []

    @property
    def pc(self):
        return self.at + 4 * len(self.words)

    def emit(self, *ws):
        self.words += ws
        return self

    def bgt(self, crf, target):
        self.words.append((16 << 26) | (12 << 21) | ((4 * crf + 1) << 16) | ((target - self.pc) & 0xFFFC))
        return self

    def b(self, target, link=False):
        self.words.append((18 << 26) | ((target - self.pc) & 0x03FFFFFC) | int(link))
        return self


def make_image(text_words, rdata=b"", pdata=()):
    data = bytearray(0x4000)
    data[0:2] = b"MZ"
    data[RDATA - BASE:RDATA - BASE + len(rdata)] = rdata
    for i, w in enumerate(text_words):
        struct.pack_into(">I", data, TEXT - BASE + 4 * i, w)
    img = ModuleImage.__new__(ModuleImage)
    img.path = "synthetic"
    img.original_name = "synthetic.dll"
    img.data = bytes(data)
    img.base = BASE
    img.size = 0x8000
    img.entry = None
    img.sections = [Section(".rdata", RDATA, 0x1000, 0x1000, 0x40000040),
                    Section(".text", TEXT, 4 * len(text_words), 4 * len(text_words), 0x60000020)]
    img.code_sections = [img.sections[1]]
    img.pdata = sorted(pdata)
    img.imports = []
    img.exports = {}
    img.export_table = None
    return img


def be32(*vs):
    return b"".join(struct.pack(">I", v) for v in vs)


class SwitchTests(unittest.TestCase):
    def test_absolute_table_in_rdata(self):
        # leaf function (no .pdata) switching on r3 over 3 cases
        a = Asm(TEXT)
        a.emit(cmplwi(6, 3, 2))
        a.bgt(6, TEXT + 0x40)
        a.emit(lis(12, (RDATA >> 16)), slwi(0, 3, 2), addi(12, 12, RDATA & 0xFFFF), lwzx(0, 12, 0), mtctr(0), BCTR)
        cases = []
        for k in range(3):
            cases.append(a.pc)
            a.emit(li(3, 10 + k), BLR)
        while a.pc < TEXT + 0x40:
            a.emit(NOP)
        a.emit(li(3, -1), BLR)  # default at +0x40
        img = make_image(a.words, be32(*cases))
        an = Analysis(img).run()
        self.assertEqual(an.problems, [])
        sw = an.switches[TEXT + 4 * 7]
        self.assertEqual(sw["labels"], cases)
        self.assertEqual(sw["r"], 3)
        self.assertEqual(sw["default"], TEXT + 0x40)
        # one function covering the dispatch and every case
        self.assertEqual([(b, n) for b, n, _ in an.all_functions()], [(TEXT, 4 * len(a.words))])

    def test_short_offsets_with_nop(self):
        a = Asm(TEXT)
        a.emit(addi(11, 19, -5), cmplwi(6, 11, 1))
        a.bgt(6, TEXT + 0x80)
        a.emit(lis(12, RDATA >> 16), slwi(0, 11, 1), addi(12, 12, RDATA & 0xFFFF), lhzx(0, 12, 0))
        base_pc = TEXT + 0x30
        a.emit(lis(12, base_pc >> 16), addi(12, 12, base_pc & 0xFFFF), NOP, add(12, 12, 0), mtctr(12), BCTR)
        while a.pc < base_pc:
            a.emit(NOP)
        a.emit(li(3, 1), BLR, li(3, 2), BLR)
        while a.pc < TEXT + 0x80:
            a.emit(NOP)
        a.emit(BLR)
        img = make_image(a.words, struct.pack(">HH", 0, 8))
        an = Analysis(img).run()
        self.assertEqual(an.problems, [])
        (sw,) = an.switches.values()
        self.assertEqual(sw["labels"], [base_pc, base_pc + 8])
        self.assertEqual(sw["r"], 11)

    def test_byte_offsets_shifted(self):
        a = Asm(TEXT)
        a.emit(cmplwi(6, 11, 2))
        a.bgt(6, TEXT + 0x80)
        base_pc = TEXT + 0x40
        a.emit(lis(12, RDATA >> 16), addi(12, 12, RDATA & 0xFFFF), lbzx(0, 12, 11), slwi(0, 0, 2),
               lis(12, base_pc >> 16), NOP, addi(12, 12, base_pc & 0xFFFF), add(12, 12, 0), mtctr(12), BCTR)
        while a.pc < base_pc:
            a.emit(NOP)
        a.emit(BLR, BLR, BLR)
        while a.pc < TEXT + 0x80:
            a.emit(NOP)
        a.emit(BLR)
        img = make_image(a.words, bytes([2, 0, 1]))
        an = Analysis(img).run()
        self.assertEqual(an.problems, [])
        (sw,) = an.switches.values()
        self.assertEqual(sw["labels"], [base_pc + 8, base_pc, base_pc + 4])

    def test_recomputed_index(self):
        # cmplwi checks r10 = r11 - 1, the dispatch recomputes r11 - 1
        a = Asm(TEXT)
        a.emit(addi(10, 11, -1), cmplwi(6, 10, 1))
        a.bgt(6, TEXT + 0x80)
        a.emit(addi(11, 11, -1), lis(12, RDATA >> 16), slwi(0, 11, 2), addi(12, 12, RDATA & 0xFFFF),
               lwzx(0, 12, 0), mtctr(0), BCTR)
        c0 = a.pc
        a.emit(BLR)
        c1 = a.pc
        a.emit(BLR)
        while a.pc < TEXT + 0x80:
            a.emit(NOP)
        a.emit(BLR)
        img = make_image(a.words, be32(c0, c1))
        an = Analysis(img).run()
        self.assertEqual(an.problems, [])
        (sw,) = an.switches.values()
        self.assertEqual(sw["labels"], [c0, c1])
        self.assertEqual(sw["r"], 10)

    def test_pointer_tail_call_is_not_a_table(self):
        a = Asm(TEXT)
        a.emit(lwz(11, 3, 0), lwzx(4, 5, 6), mtctr(11), BCTR)
        img = make_image(a.words)
        an = Analysis(img).run()
        self.assertEqual(an.problems, [])
        self.assertEqual(an.switches, {})
        self.assertEqual(an.indirect_jumps, [TEXT + 12])


class BoundaryTests(unittest.TestCase):
    def test_leaf_functions_split_at_references(self):
        # f0 calls f1 (bl); f1 and f2 are leaf functions outside .pdata
        a = Asm(TEXT)
        f1 = TEXT + 0x10
        a.b(f1, link=True).emit(BLR, 0, 0)    # f0 + padding
        a.emit(li(3, 1), BLR)                 # f1 at +0x10
        a.emit(li(3, 2), BLR)                 # f2: unreferenced, after a blr
        img = make_image(a.words)
        an = Analysis(img).run()
        self.assertEqual(an.problems, [])
        self.assertEqual([(b, n) for b, n, _ in an.all_functions()],
                         [(TEXT, 8), (f1, 8), (f1 + 8, 8)])

    def test_branch_into_pdata_function_makes_a_chunk(self):
        a = Asm(TEXT)
        a.emit(li(3, 0), li(4, 0), li(5, 0), BLR)   # .pdata function, 16 bytes
        a.b(TEXT + 8)                                # gap code jumps into its middle
        img = make_image(a.words, pdata=[(TEXT, 16, False)])
        an = Analysis(img).run()
        self.assertEqual(an.problems, [])
        kinds = {(b, n): k for b, n, k in an.all_functions()}
        self.assertEqual(kinds[(TEXT + 8, 8)], "chunk")
        self.assertEqual(kinds[(TEXT + 16, 4)], "gap")

    def test_config_lists_only_non_pdata_functions(self):
        a = Asm(TEXT)
        a.emit(li(3, 0), BLR, li(3, 1), BLR)
        img = make_image(a.words, pdata=[(TEXT, 8, False)])
        an = Analysis(img).run()
        toml = an.config_toml("x.xex", "ppc", "x_switch.toml", {})
        self.assertIn("address = 0x%08X, size = 0x8 }, # gap" % (TEXT + 8), toml)
        self.assertNotIn("address = 0x%08X," % TEXT, toml)
        self.assertIn("{ data = 0x00000000, size = 4 }", toml)


class DecoderTests(unittest.TestCase):
    def test_branch_targets(self):
        self.assertEqual(ppc.b_target(0x4BFFFFFC, 0x1000), 0xFFC)
        self.assertEqual(ppc.bc_target(0x4199008C, 0x88032BC4), 0x88032C50)
        self.assertTrue(ppc.is_terminator(BLR))
        self.assertTrue(ppc.is_terminator(BCTR))
        self.assertFalse(ppc.is_terminator(0x4C9A0020))  # bnelr cr6
        self.assertTrue(ppc.is_mtctr(0x7C0903A6))
        self.assertEqual(ppc.mask32(0, 29), 0xFFFFFFFC)
        self.assertEqual(ppc.rotl32(0x80000001, 1), 3)


if __name__ == "__main__":
    unittest.main()
