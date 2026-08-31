#!/usr/bin/env python3
"""Minimal PPC64 BE disassembler over the game ELF.

Only the forms that come up while chasing engine control flow: D-form
loads/stores, branches, and enough arithmetic to read a state machine.
Not a general disassembler -- objdump is, and it is not here.

    python scripts/dis.py 0x00917380            # one function (bounds from analysis)
    python scripts/dis.py 0x00917380 0x9174A0   # explicit range
    python scripts/dis.py 0x00917380 --refs 64  # only lines touching displacement 64
"""
import json
import os
import struct
import sys

ELF = os.path.join(os.path.dirname(__file__), '..', 'input', 'EMAIN.ELF')
FNS = os.path.join(os.path.dirname(__file__), '..', 'analysis', 'functions_code.json')

_d = open(ELF, 'rb').read()
_ph_off = struct.unpack('>Q', _d[0x20:0x28])[0]
_ph_esz = struct.unpack('>H', _d[0x36:0x38])[0]
_ph_n = struct.unpack('>H', _d[0x38:0x3A])[0]
SEGS = []
for _i in range(_ph_n):
    _o = _ph_off + _i * _ph_esz
    if struct.unpack('>I', _d[_o:_o + 4])[0] != 1:
        continue
    _off, _va = struct.unpack('>QQ', _d[_o + 8:_o + 24])
    _fsz = struct.unpack('>Q', _d[_o + 32:_o + 40])[0]
    SEGS.append((_va, _off, _fsz))


def read(va, n):
    for base, off, fsz in SEGS:
        if base <= va < base + fsz:
            return _d[off + (va - base):off + (va - base) + n]
    raise KeyError('0x%08X not mapped' % va)


def bounds(addr):
    """Function extent from the analysis pass, or a 512-byte window."""
    try:
        for f in json.load(open(FNS)):
            if int(f['start'], 16) == addr:
                return addr, int(f['end'], 16)
    except (OSError, ValueError):
        pass
    return addr, addr + 0x200


DFORM = {32: 'lwz', 33: 'lwzu', 34: 'lbz', 35: 'lbzu', 36: 'stw', 37: 'stwu',
         38: 'stb', 39: 'stbu', 40: 'lhz', 42: 'lha', 44: 'sth', 46: 'lmw',
         47: 'stmw', 14: 'addi', 15: 'addis', 24: 'ori', 28: 'andi.'}


def branch_target(addr, w):
    op = w >> 26
    if op == 18:
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        return li if w & 2 else (addr + li) & 0xFFFFFFFF
    if op == 16:
        bd = w & 0xFFFC
        if bd & 0x8000:
            bd -= 0x10000
        return bd if w & 2 else (addr + bd) & 0xFFFFFFFF
    return None


def lines(lo, hi):
    buf = read(lo, hi - lo)
    for i, w in enumerate(struct.unpack('>%dI' % ((hi - lo) // 4), buf)):
        a = lo + i * 4
        op = w >> 26
        tgt = branch_target(a, w)
        if tgt is not None:
            kind = 'bl' if (op == 18 and w & 1) else ('b' if op == 18 else 'bc')
            yield a, w, '%-5s 0x%08X' % (kind, tgt), None
        elif op == 58 or op == 62:               # ld/std family, DS-form
            rt, ra = (w >> 21) & 31, (w >> 16) & 31
            ds = w & 0xFFFC
            if ds & 0x8000:
                ds -= 0x10000
            yield a, w, '%-5s r%d, %d(r%d)' % ('std' if op == 62 else 'ld', rt, ds, ra), ds
        elif op in DFORM:
            rt, ra = (w >> 21) & 31, (w >> 16) & 31
            d = w & 0xFFFF
            if d & 0x8000:
                d -= 0x10000
            yield a, w, '%-5s r%d, %d(r%d)' % (DFORM[op], rt, d, ra), d
        elif op == 31 and ((w >> 1) & 0x3FF) == 467:      # mtspr
            yield a, w, 'mtspr', None
        else:
            yield a, w, '.long 0x%08X' % w, None


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    only = None
    if '--refs' in sys.argv:
        only = int(sys.argv[sys.argv.index('--refs') + 1], 0)
    lo = int(args[0], 0)
    hi = int(args[1], 0) if len(args) > 1 else bounds(lo)[1]
    print('=== 0x%08X..0x%08X ===' % (lo, hi))
    for a, w, text, disp in lines(lo, hi):
        if only is not None and disp != only:
            continue
        print('  %08X  %s' % (a, text))


if __name__ == '__main__':
    main()
