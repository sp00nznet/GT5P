#!/usr/bin/env python3
"""Emit the lifter's --hle-stubs map by walking the ELF's own import table.

Every imported function has a `li r12,0` thunk in .text that the PS3 loader
would have patched at boot; unpatched, lifting it produces a body that branches
to whatever `li r12,0` decodes as. The lifter can replace each one with
`ps3_hle_call(nid, ctx)` instead -- it just needs to be told which address maps
to which NID, which the module's import descriptors already say.

    python scripts/gen_hle_stubs.py input/EMAIN.ELF analysis/hle_stubs.json

Writes a `{"stub": addr, "nid": nid}` list for --hle-stubs, plus a sibling
`*_named.json` carrying the module and resolved name for each entry (logging
and coverage reports; the lifter ignores it).
"""
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent / "ps3recomp" / "tools"))
from elf_parser import ELFFile, PRXImportEntry, vaddr_to_offset  # noqa: E402

try:
    from nid_database import NIDDatabase
except ImportError:
    NIDDatabase = None

IMPORT_ENTRY_SIZE = 0x2C


def collect(elf_path):
    elf = ELFFile(elf_path)
    elf.load()
    data, phdrs = elf.raw_data, elf.program_headers

    def u32(vaddr):
        off = vaddr_to_offset(phdrs, vaddr)
        return struct.unpack(">I", data[off:off + 4])[0] if off is not None else None

    def cstr(vaddr):
        off = vaddr_to_offset(phdrs, vaddr)
        if off is None:
            return ""
        return data[off:data.index(b"\0", off)].decode("ascii", "replace")

    mi = elf.module_info
    if not mi or not mi.imports_start:
        raise SystemExit("no module import table in this ELF")

    out = []
    for va in range(mi.imports_start, mi.imports_end, IMPORT_ENTRY_SIZE):
        off = vaddr_to_offset(phdrs, va)
        imp = PRXImportEntry.parse(data, off, True)
        module = cstr(imp.name_ptr)
        for k in range(imp.num_funcs):
            nid = u32(imp.nid_table_ptr + 4 * k)
            stub = u32(imp.stub_table_ptr + 4 * k)
            if nid is None or not stub:
                continue
            out.append({"stub": stub, "nid": nid, "module": module})
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    entries = collect(sys.argv[1])

    names = {}
    if NIDDatabase is not None:
        db = NIDDatabase()
        db.load_builtins()
        db.load_implemented()
        for e in entries:
            hit = db.lookup_nid(e["nid"])
            names[e["nid"]] = hit[1] if hit else None

    out = Path(sys.argv[2])
    out.write_text(json.dumps([{"stub": f"0x{e['stub']:08X}", "nid": f"0x{e['nid']:08X}"}
                               for e in entries], indent=1))
    named = out.with_name(out.stem + "_named.json")
    named.write_text(json.dumps([{**e,
                                  "stub": f"0x{e['stub']:08X}",
                                  "nid": f"0x{e['nid']:08X}",
                                  "name": names.get(e["nid"])} for e in entries], indent=1))

    resolved = sum(1 for e in entries if names.get(e["nid"]))
    lo = min(e["stub"] for e in entries)
    hi = max(e["stub"] for e in entries)
    print(f"{len(entries)} import stubs across "
          f"{len({e['module'] for e in entries})} modules, "
          f"0x{lo:08X}..0x{hi:08X}")
    print(f"{resolved} NIDs named, {len(entries) - resolved} unknown")
    print(f"wrote {out} and {named}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
