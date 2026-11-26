"""Minimal 32-bit PowerPC instruction decoding for the recompiler analysis.

Only what tools/recomp/analyse.py needs: branch forms, the integer
instructions that compute jump-table targets, and a few loads. Words are
the big-endian instruction values as integers.
"""

NOP = 0x60000000
BLR = 0x4E800020
BCTR = 0x4E800420
BCTRL = 0x4E800421


def opcd(w):
    return w >> 26


def rt(w):
    return (w >> 21) & 31


def ra(w):
    return (w >> 16) & 31


def rb(w):
    return (w >> 11) & 31


def xo10(w):
    return (w >> 1) & 0x3FF


def simm(w):
    v = w & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def uimm(w):
    return w & 0xFFFF


def sext(v, bits):
    m = 1 << (bits - 1)
    v &= (1 << bits) - 1
    return (v ^ m) - m


# ---- branches -------------------------------------------------------------

def is_b(w):
    """b/bl/ba/bla (I-form)."""
    return opcd(w) == 18


def is_bc(w):
    return opcd(w) == 16


def lk(w):
    return w & 1


def aa(w):
    return (w >> 1) & 1


def b_target(w, addr):
    off = sext(w & 0x03FFFFFC, 26)
    return (off if aa(w) else addr + off) & 0xFFFFFFFF


def bc_target(w, addr):
    off = sext(w & 0xFFFC, 16)
    return (off if aa(w) else addr + off) & 0xFFFFFFFF


def bc_bo(w):
    return (w >> 21) & 31


def bc_bi(w):
    return (w >> 16) & 31


def is_bclr(w):
    return opcd(w) == 19 and xo10(w) == 16


def is_bcctr(w):
    return opcd(w) == 19 and xo10(w) == 528


def bo_always(bo):
    return (bo & 0x14) == 0x14


def is_terminator(w):
    """Control never falls through: b (no link), unconditional blr/bctr."""
    if is_b(w) and not lk(w):
        return True
    if (is_bclr(w) or is_bcctr(w)) and not lk(w) and bo_always(bc_bo(w)):
        return True
    return False


# ---- integer forms used by switch dispatch ---------------------------------

def is_mtctr(w):
    # mtspr 9, rS: opcode 31, xo 467, spr field (swapped halves) = 9 << 5
    return opcd(w) == 31 and xo10(w) == 467 and ((w >> 11) & 0x3FF) == (9 << 5)


def is_cmplwi(w):
    """cmpli crD, 0 (L=0), rA, UIMM."""
    return opcd(w) == 10 and ((w >> 21) & 1) == 0


def cmp_crf(w):
    return (w >> 23) & 7


def rlwinm_fields(w):
    return (w >> 11) & 31, (w >> 6) & 31, (w >> 1) & 31  # sh, mb, me


def rotl32(v, n):
    v &= 0xFFFFFFFF
    return ((v << n) | (v >> (32 - n))) & 0xFFFFFFFF if n else v


def mask32(mb, me):
    m = 0
    if mb <= me:
        for i in range(mb, me + 1):
            m |= 1 << (31 - i)
    else:
        for i in range(0, me + 1):
            m |= 1 << (31 - i)
        for i in range(mb, 32):
            m |= 1 << (31 - i)
    return m
