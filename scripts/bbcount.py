#!/usr/bin/env python3
"""Count basic-block hits inside one lifted function.

For a function that spins with no calls in its loop, nothing in the tracing
harness can say *where* -- gt5p_alloc_pre only sees entries and returns. This
drops a counter on every label in the lifted body and dumps the hottest blocks
once, which names the loop directly.

    python scripts/bbcount.py 00956D20

Idempotent, and re-run after every re-lift: generated/ is rebuilt wholesale.
"""
import re
import sys
from pathlib import Path

GEN = Path(__file__).resolve().parent.parent / "generated"

PROLOGUE = '''
/* ---- bbcount.py: {name} --------------------------------------------- */
#include <stdio.h>
extern "C" void gt5p_bb_hit(unsigned idx, unsigned addr);
'''


def instrument(addr):
    name = f"func_{addr.upper()}"
    for chunk in sorted(GEN.glob("ppu_recomp_*.cpp")):
        text = chunk.read_text(encoding="utf-8", errors="replace")
        start = text.find(f"void {name}(ppu_context* ctx) {{")
        if start < 0:
            start = text.find(f"void gt5p_orig_{name}(ppu_context* ctx) {{")
        if start < 0:
            continue
        if f"gt5p_bb_hit" in text[start:start + 200000]:
            print(f"  {name}: already counted in {chunk.name}")
            return True
        end = text.find("\n}\n", start)
        body = text[start:end]
        labels = re.findall(r"^loc_([0-9A-Fa-f]{8}):", body, re.M)
        seen = {}

        def add(m):
            la = m.group(1)
            seen.setdefault(la, len(seen))
            return f"loc_{la}: gt5p_bb_hit({seen[la]}, 0x{la});"

        body = re.sub(r"^loc_([0-9A-Fa-f]{8}):", add, body, flags=re.M)
        text = text[:start] + PROLOGUE.format(name=name) + body + text[end:]
        chunk.write_text(text, encoding="utf-8")
        print(f"  {name}: {len(seen)} blocks counted in {chunk.name}")
        return True
    print(f"  {name}: NOT FOUND")
    return False


if __name__ == "__main__":
    for a in sys.argv[1:]:
        instrument(a.lower().replace("0x", ""))
