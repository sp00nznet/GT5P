#!/usr/bin/env python3
"""Who reaches this address?

    python scripts/xref.py 0x0068E3E0 [more addrs...]

Prints, for each target:

  * every `bl`/`b` in an executable segment whose branch target is the address,
    named by the function that contains the branch;
  * every word anywhere in the image that equals the address -- an OPD entry,
    so a vtable slot or a function-pointer table, which is how most of this
    title's calls actually happen.

The second half is the point. A C++ engine reaches almost nothing by direct
branch: the wait/signal pair that gates GT5P's boot has exactly one `bl` caller
and one data reference, and the data reference is the one that matters.
"""
import bisect
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ELF = ROOT / "input" / "EMAIN.ELF"
FUNCS = ROOT / "analysis" / "functions_code.json"


def load():
    d = ELF.read_bytes()
    ph_off = struct.unpack(">Q", d[0x20:0x28])[0]
    ph_esz = struct.unpack(">H", d[0x36:0x38])[0]
    ph_n = struct.unpack(">H", d[0x38:0x3A])[0]
    segs = []
    for i in range(ph_n):
        o = ph_off + i * ph_esz
        t, fl = struct.unpack(">II", d[o:o + 8])
        off, va = struct.unpack(">QQ", d[o + 8:o + 24])
        fsz = struct.unpack(">Q", d[o + 32:o + 40])[0]
        if t == 1:
            segs.append((va, off, fsz, fl))
    fns = sorted((int(e["start"], 16), int(e["end"], 16))
                 for e in json.loads(FUNCS.read_text()))
    return d, segs, fns


def main(argv):
    if not argv:
        print(__doc__)
        return 1
    d, segs, fns = load()
    starts = [s for s, _ in fns]

    def owner(a):
        i = bisect.bisect_right(starts, a) - 1
        return fns[i][0] if i >= 0 and fns[i][0] <= a < fns[i][1] else None

    for spec in argv:
        target = int(spec, 16) if spec.lower().startswith("0x") else int(spec, 16)
        print(f"=== 0x{target:08X} (in func_{owner(target) or 0:08X}) ===")
        hits = 0
        for base, off, fsz, fl in segs:
            n = fsz // 4
            words = struct.unpack(f">{n}I", d[off:off + n * 4])
            for i, w in enumerate(words):
                a = base + i * 4
                if (fl & 1) and (w >> 26) == 18:          # b / bl
                    li = w & 0x03FFFFFC
                    if li & 0x02000000:
                        li -= 0x04000000
                    if ((a + li) & 0xFFFFFFFF) == target:
                        kind = "bl" if (w & 1) else "b "
                        print(f"  {kind} 0x{a:08X}  in func_{owner(a) or 0:08X}")
                        hits += 1
                elif w == target:
                    # An OPD is {code, toc}; report the pointer to the OPD too,
                    # since that is what a vtable slot actually holds.
                    print(f"  data 0x{a:08X} holds it (OPD -> referenced as 0x{a:08X})")
                    hits += 1
        if not hits:
            print("  no references -- reached indirectly, or dead")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
