#!/usr/bin/env python3
"""Check every compare in a lifted function against its encoding.

Signedness and CR-field placement are where a lifted compare goes wrong
quietly: a `cmpl` translated as signed still works for small values and fails
only once a pointer crosses 0x80000000, and a CR field off by one turns a
branch into its neighbour's. Both are invisible in a mnemonic-level diff, which
is why tools/disasm_audit_operands.py passes while the program misbehaves.

    python scripts/audit_cmp.py 0094F818 0094FF30 ...

For each cmp/cmpl/cmpi/cmpli it reports the expected signedness, width and CR
field, and whether the lifted line agrees.
"""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from dis import read, bounds

GEN = Path(__file__).resolve().parent.parent / "generated"


def lifted_body(name):
    for chunk in sorted(GEN.glob("ppu_recomp_*.cpp")):
        t = chunk.read_text(encoding="latin-1")
        for pref in ("void %s(ppu_context* ctx) {" % name,
                     "void gt5p_orig_%s(ppu_context* ctx) {" % name):
            i = t.find(pref)
            if i >= 0:
                j = t.find("\n}\n", i)
                return t[i:j]
    return None


def check(addr):
    name = "func_%08X" % addr
    lo, hi = bounds(addr)
    body = lifted_body(name)
    if body is None:
        print("  %s: not found in generated/" % name)
        return 0
    lines = body.splitlines()
    bad = 0
    ws = struct.unpack('>%dI' % ((hi - lo) // 4), read(lo, hi - lo))
    # lifted compares appear in source order; walk them in parallel
    lifted = [l for l in lines if 'cr_val' in l]
    k = 0
    for i, w in enumerate(ws):
        op = w >> 26
        signed = width = crf = None
        if op == 31 and ((w >> 1) & 0x3FF) in (0, 32):
            signed = ((w >> 1) & 0x3FF) == 0
            width = 64 if ((w >> 21) & 1) else 32
            crf = (w >> 23) & 7
        elif op in (10, 11):
            signed = (op == 11)
            width = 64 if ((w >> 21) & 1) else 32
            crf = (w >> 23) & 7
        if crf is None:
            continue
        if k >= len(lifted):
            print("  %s +0x%03X: no lifted compare left to match" % (name, i * 4))
            bad += 1
            continue
        L = lifted[k]; k += 1
        shift = 4 * (7 - crf)
        want_shift = "<< %d" % shift if shift else "<< 0"
        cast = "int32_t" if (signed and width == 32) else \
               "uint32_t" if (not signed and width == 32) else \
               "int64_t" if signed else "uint64_t"
        ok_sign = ("(%s)" % cast) in L
        ok_crf = (want_shift in L) or (shift == 0 and "<< 0" in L)
        if not (ok_sign and ok_crf):
            bad += 1
            print("  %s +0x%03X  %-6s w%d cr%d  expect (%s) and shift %d" %
                  (name, i * 4, "cmp" if signed else "cmpl", width, crf, cast, shift))
            print("      lifted: %s" % L.strip()[:150])
    return bad


if __name__ == "__main__":
    total = 0
    for a in sys.argv[1:]:
        total += check(int(a, 16))
    print("\n%s" % ("mismatches: %d" % total if total else "all compares agree"))
