#!/usr/bin/env python3
"""Check every conditional branch in a lifted function against its encoding.

A `bc` carries three things the lifted `if` has to reproduce exactly: which CR
field, which bit of it, and whether the branch is taken when that bit is SET or
CLEAR. Get the sense backwards and the code still runs -- it just takes the
other arm, which in list surgery means a node is left linked.

    python scripts/audit_branch.py 0094F818 ...
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from dis import read, bounds

GEN = Path(__file__).resolve().parent.parent / "generated"
BITS = {0: (8, 'LT'), 1: (4, 'GT'), 2: (2, 'EQ'), 3: (1, 'SO')}


def lifted_body(name):
    for chunk in sorted(GEN.glob("ppu_recomp_*.cpp")):
        t = chunk.read_text(encoding="latin-1")
        for pref in ("void %s(ppu_context* ctx) {" % name,
                     "void gt5p_orig_%s(ppu_context* ctx) {" % name):
            i = t.find(pref)
            if i >= 0:
                return t[i:t.find("\n}\n", i)]
    return None


def check(addr):
    name = "func_%08X" % addr
    lo, hi = bounds(addr)
    body = lifted_body(name)
    if body is None:
        print("  %s: not in generated/" % name)
        return 0
    lifted = [l for l in body.splitlines() if 'ctx->cr >>' in l and 'goto' in l]
    ws = struct.unpack('>%dI' % ((hi - lo) // 4), read(lo, hi - lo))
    k = bad = 0
    for i, w in enumerate(ws):
        if (w >> 26) != 16:
            continue
        BO, BI = (w >> 21) & 31, (w >> 16) & 31
        if not (BO & 4):          # decrements CTR -- a loop form, not a plain test
            continue
        mask, nm = BITS[BI & 3]
        crf = BI >> 2
        shift = 4 * (7 - crf)
        taken_when_set = bool(BO & 8)
        if k >= len(lifted):
            print("  %s +0x%03X: no lifted branch left to match" % (name, i * 4))
            bad += 1
            continue
        L = lifted[k]; k += 1
        neg = '(!(' in L
        want = ("((ctx->cr >> %d) & %d)" % (shift, mask))
        ok = (want in L) and (neg != taken_when_set)
        if not ok:
            bad += 1
            print("  %s +0x%03X  bc BO=%d BI=%d -> cr%d.%s, taken when %s" %
                  (name, i * 4, BO, BI, crf, nm, "SET" if taken_when_set else "CLEAR"))
            print("      expect %s%s" % ("" if taken_when_set else "negated ", want))
            print("      lifted: %s" % L.strip()[:150])
    return bad


if __name__ == "__main__":
    total = 0
    for a in sys.argv[1:]:
        total += check(int(a, 16))
    print("\n%s" % ("mismatches: %d" % total if total else "all branches agree"))
