#!/usr/bin/env python3
"""Print a vtable's slots: slot index, OPD address, and the entry it resolves to.

    python scripts/vtab.py 0x00F758B0 [nslots]

PS3 PPC64 vtable entries are 4-byte pointers to OPD function descriptors,
so a slot needs two hops to name a function.
"""
import struct
import sys

from dis import read

def main():
    va = int(sys.argv[1], 0)
    n = int(sys.argv[2], 0) if len(sys.argv) > 2 else 32
    print('vtable 0x%08X' % va)
    for i in range(n):
        off = i * 4
        opd = struct.unpack('>I', read(va + off, 4))[0]
        if not opd:
            print('  +0x%02X slot %-3d 0' % (off, i)); continue
        try:
            entry = struct.unpack('>I', read(opd, 4))[0]
        except KeyError:
            print('  +0x%02X slot %-3d opd=0x%08X (unmapped)' % (off, i, opd)); continue
        print('  +0x%02X slot %-3d opd=0x%08X -> func_%08X' % (off, i, opd, entry))

if __name__ == '__main__':
    main()
