#!/usr/bin/env python3
"""Recover switch-statement targets that only a computed branch reaches.

find_functions walks direct branches, so a `bctr` through a table is a dead
end for it: the table's targets look like unreferenced bytes and never get
lifted. func_00917EB0's 12-way switch on the async-request state is one --
its state-0 target (0x009180AC) had to be seeded by hand before the boot
would resolve, and there are ten more like it in that one function alone.

The layout GCC emits here puts the table immediately after the bctr, with
each entry a signed 32-bit offset from the table's own base:

    cmplwi rX, N            <- N is the highest case index
    ble    .Ldispatch
    ...default...
  .Ldispatch:
    lwz    r11, table@toc(rTOC)
    rldic  r9, r0, 2, 30
    lwzx   r0, r9, r11
    extsw  r0, r0
    add    r0, r0, r11
    mtctr  r0
    bctr
    .long  off0, off1, ...  <- table base == this address

Writes the targets as seeds. Emitting a target that is already a known
function start is harmless -- find_functions dedupes -- so the bar for
accepting one is deliberately low: mapped, 4-aligned, and decoding to a
plausible instruction.
"""
import json
import os
import struct
import sys

from dis import SEGS, read

BCTR = 0x4E800420
TEXT_LO, TEXT_HI = 0x00010000, 0x00BE0AF4


def mapped(va):
    return any(b <= va < b + f for b, _, f in SEGS)


def plausible(va):
    """A target must at least decode to something that is not padding."""
    if not (TEXT_LO <= va < TEXT_HI) or va & 3 or not mapped(va):
        return False
    w = struct.unpack('>I', read(va, 4))[0]
    return w not in (0, 0xFFFFFFFF)


def case_count(addr, limit=0x400):
    """Highest case index, from the cmplwi/cmpwi guarding the dispatch.

    Only a cross-check: the compare can sit far back, on the other side of
    the default case (240 bytes, in func_00917EB0), so its absence is not
    evidence that this is not a table.
    """
    for back in range(4, limit, 4):
        w = struct.unpack('>I', read(addr - back, 4))[0]
        op, d = w >> 26, w & 0xFFFF
        if op in (10, 11) and 0 < d < 512:      # cmpli / cmpi immediate
            return d
    return None


def read_table(tbase):
    """Entries until one stops making sense.

    Self-terminating, so it does not depend on finding the compare. Two
    bounds: an entry must decode to a plausible target, and the table
    cannot run past the nearest target above it -- the compiler puts the
    first case's code directly after the table.
    """
    targets, limit = [], None
    for k in range(512):
        ea = tbase + k * 4
        if limit is not None and ea >= limit:
            break
        if not mapped(ea):
            break
        tgt = (tbase + struct.unpack('>i', read(ea, 4))[0]) & 0xFFFFFFFF
        if not plausible(tgt):
            break
        targets.append(tgt)
        if tgt > tbase and (limit is None or tgt < limit):
            limit = tgt
    return targets


def tables():
    for base, off, fsz in SEGS:
        if not (base <= TEXT_LO < base + fsz or TEXT_LO <= base < TEXT_HI):
            continue
        lo = max(base, TEXT_LO)
        hi = min(base + fsz, TEXT_HI)
        buf = read(lo, hi - lo)
        for i, w in enumerate(struct.unpack('>%dI' % ((hi - lo) // 4), buf)):
            if w != BCTR:
                continue
            addr = lo + i * 4
            # Require the lwzx+add shape in the run-up, or this is an
            # ordinary virtual call through ctr, not a table dispatch.
            window = [struct.unpack('>I', read(addr - b, 4))[0] for b in range(4, 40, 4)]
            has_lwzx = any((x >> 26) == 31 and ((x >> 1) & 0x3FF) in (23, 341) for x in window)
            has_add = any((x >> 26) == 31 and ((x >> 1) & 0x3FF) == 266 for x in window)
            if not (has_lwzx and has_add):
                continue
            tbase = addr + 4
            targets = read_table(tbase)
            if len(targets) < 3:            # too short to tell from stray data
                continue
            n = case_count(addr)
            if n is not None and len(targets) > n + 1:
                targets = targets[:n + 1]   # trust the compare when we have it
            yield addr, tbase, targets


def main():
    found, all_t = 0, set()
    for addr, tbase, targets in tables():
        found += 1
        all_t.update(targets)
        if '-v' in sys.argv:
            print('bctr 0x%08X table 0x%08X  %d cases' % (addr, tbase, len(targets)))
            print('   ' + ' '.join('0x%08X' % t for t in sorted(set(targets))))
    print('%d jump tables, %d distinct targets' % (found, len(all_t)))

    if '--write' in sys.argv:
        path = os.path.join(os.path.dirname(__file__), '..', 'config', 'extra_seeds.json')
        existing = json.load(open(path)) if os.path.exists(path) else []
        merged = sorted(set(existing) | {'0x%08X' % t for t in all_t})
        json.dump(merged, open(path, 'w'), indent=2)
        print('seeds: %d -> %d in config/extra_seeds.json' % (len(existing), len(merged)))


if __name__ == '__main__':
    main()
