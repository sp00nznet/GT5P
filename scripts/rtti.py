#!/usr/bin/env python3
"""Recover class names from the Itanium C++ RTTI this title still ships.

    python scripts/rtti.py                 # write analysis/rtti.json, print a summary
    python scripts/rtti.py 0x006736A0 ...  # name these functions

GT5P was not stripped: every polymorphic class carries a `std::type_info` whose
name field points at a live mangled string, and every vtable is preceded by
`{ offset_to_top, typeinfo_ptr }`. That is enough to turn `func_006736A0` into
`PDIEXT::AdvertiseSimplePS3 vtable+0x10`, which is the difference between
reading a backtrace and guessing at one.

The demangler here is deliberately small -- it handles the nested-name form
(`N6PDIEXT18AdvertiseSimplePS3E`) and one level of template argument, which
covers essentially everything in this binary. Anything it does not recognise is
returned unchanged rather than dropped.
"""
import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ELF = ROOT / "input" / "EMAIN.ELF"
OUT = ROOT / "analysis" / "rtti.json"


def segments(d):
    ph_off = struct.unpack(">Q", d[0x20:0x28])[0]
    ph_esz = struct.unpack(">H", d[0x36:0x38])[0]
    ph_n = struct.unpack(">H", d[0x38:0x3A])[0]
    segs = []
    for i in range(ph_n):
        o = ph_off + i * ph_esz
        t = struct.unpack(">I", d[o:o + 4])[0]
        off, va = struct.unpack(">QQ", d[o + 8:o + 24])
        fsz = struct.unpack(">Q", d[o + 32:o + 40])[0]
        if t == 1:
            segs.append((va, off, fsz))
    return segs


class Image:
    def __init__(self):
        self.d = ELF.read_bytes()
        self.segs = segments(self.d)

    def read(self, va, n):
        for base, off, fsz in self.segs:
            if base <= va < base + fsz:
                avail = min(n, base + fsz - va)
                return self.d[off + (va - base):off + (va - base) + avail]
        return None

    def u32(self, va):
        r = self.read(va, 4)
        return struct.unpack(">I", r)[0] if r and len(r) == 4 else None

    def cstr(self, va, maxn=160):
        r = self.read(va, maxn)
        if not r:
            return None
        z = r.find(b"\0")
        if z < 0:
            return None
        try:
            return r[:z].decode("ascii")
        except UnicodeDecodeError:
            return None


def demangle(sym):
    """Just enough Itanium demangling for this binary's class names."""
    if not sym:
        return sym

    def parts(s, i):
        """Read <length><name> components until 'E'."""
        out = []
        while i < len(s):
            if s[i] == "E":
                return out, i + 1
            if s[i] == "I":                       # template args
                args, i = read_args(s, i + 1)
                if out:
                    out[-1] += "<" + ", ".join(args) + ">"
                continue
            m = re.match(r"(\d+)", s[i:])
            if not m:
                return out, i
            n = int(m.group(1))
            i += len(m.group(1))
            out.append(s[i:i + n])
            i += n
        return out, i

    def read_args(s, i):
        args = []
        while i < len(s):
            if s[i] == "E":
                return args, i + 1
            if s[i] == "N":
                p, i = parts(s, i + 1)
                args.append("::".join(p))
                continue
            m = re.match(r"(\d+)", s[i:])
            if m:
                n = int(m.group(1))
                i += len(m.group(1))
                args.append(s[i:i + n])
                i += n
                continue
            i += 1                                # builtin/unknown: skip
        return args, i

    if sym.startswith("N"):
        p, _ = parts(sym, 1)
        return "::".join(p) if p else sym
    m = re.match(r"^(\d+)(.*)$", sym)
    if m:
        n = int(m.group(1))
        return m.group(2)[:n]
    return sym


def build(img):
    """vtable address -> class name, and function address -> (class, slot)."""
    vtables, funcs = {}, {}
    for base, off, fsz in img.segs:
        for va in range(base, base + fsz - 8, 4):
            ti = img.u32(va - 4)                  # typeinfo pointer slot
            top = img.u32(va - 8)                 # offset_to_top
            if not ti or top is None or top > 0xFFFF:
                continue
            name_ptr = img.u32(ti + 4)
            if not name_ptr:
                continue
            raw = img.cstr(name_ptr)
            if not raw or len(raw) < 3 or not raw[0].isalnum():
                continue
            if not re.fullmatch(r"[A-Za-z0-9_.$]+", raw):
                continue
            cls = demangle(raw)
            if not cls or len(cls) > 120:
                continue
            vtables[va] = cls
            for slot in range(0, 0x200, 4):       # walk the slots
                opd = img.u32(va + slot)
                if not opd:
                    break
                code = img.u32(opd)
                if not code or not (0x00010000 < code < 0x01200000):
                    break
                funcs.setdefault(code, (cls, slot))
    return vtables, funcs


def main(argv):
    img = Image()
    vtables, funcs = build(img)
    OUT.write_text(json.dumps({
        "vtables": {f"0x{k:08X}": v for k, v in sorted(vtables.items())},
        "functions": {f"0x{k:08X}": {"class": c, "slot": s}
                      for k, (c, s) in sorted(funcs.items())},
    }, indent=1))
    if argv:
        for a in argv:
            v = int(a, 16)
            hit = funcs.get(v)
            if hit:
                print(f"func_{v:08X}  {hit[0]} vtable+0x{hit[1]:02X}")
            elif v in vtables:
                print(f"0x{v:08X}  vtable of {vtables[v]}")
            else:
                print(f"func_{v:08X}  (not virtual / unknown)")
        return 0
    print(f"{len(vtables)} vtables, {len(funcs)} virtual functions -> {OUT}")
    for k, v in list(sorted(vtables.items()))[:12]:
        print(f"  0x{k:08X}  {v}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
