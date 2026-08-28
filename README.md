# Gran Turismo 5 Prologue Recompiled

**The demo that was better than most finished games.**

In 2007 Polyphony Digital shipped a *prologue* — a paid, standalone slice of a
game that would not arrive for another three and a half years. Six tracks,
seventy-one cars, and a rendering pipeline that made a 2006 console look like it
had no business being that fast. It ran at 1080p. In 2008. People bought a demo
and did not feel short-changed, which is not a sentence anybody gets to write
twice.

Then it went away. GT5 Prologue was delisted, its online services were shut down
in 2013, and the PS3 store that sold it is on life support. What is left is a
1.9 GB package sitting on people's hard drives with no hardware that will
outlive them.

## Why This Exists

**Gran Turismo 5 Prologue Recompiled** is a native PC port built with
[ps3recomp](https://github.com/sp00nznet/ps3recomp) — a static recompilation
toolchain that translates PS3 PowerPC binaries into C, then compiles that into an
x86-64 executable. No emulator at runtime. No dynamic translation. The game's own
code, compiled for your machine.

This one is a step up from every ps3recomp target so far. GT5P is a full retail
engine: 39,653 lifted PPU functions, 439 imported system calls across 26 modules, an
RSX pipeline built by the studio that cared most about it, and Polyphony's own
packed filesystem holding 1.8 GB of assets. Tokyo Jungle, the previous most
complex target, is 7,924 functions.

## Status: Phase 5 — Whole Binary Lifted to C++, Nothing Runs Yet

> The package is unpacked, both SELFs are decrypted to plain ELF, the game binary
> is fully analysed, and all 39,653 PPU functions are lifted to C++ — 3.4 million
> lines of it. Only 72 instructions in the entire binary went untranslated. There
> is no build yet: no runtime glue, no dispatch table, no `main`. Nothing runs.

| Milestone | Status |
|-----------|--------|
| PKG extraction | **Done** — 3,019 files, 1.8 GB |
| SELF decryption (`EBOOT.BIN`, `EMAIN.SELF`) | **Done** — RAP-derived, validated against a known-good control |
| ELF analysis & import resolution | **Done** — 439 imports, 291 NIDs resolved (66%) |
| Function boundary detection | **Done** — 40,476 found, 38,598 in executable sections |
| PPU code lifting | **Done** — 39,653 functions, 3.4M lines, 72 instructions unlifted |
| Project scaffold & build system | Not started |
| ELF loading & VM setup | Not started |
| CRT initialisation | Not started |
| LV2 syscall dispatch | Not started |
| Graphics (RSX → D3D12) | Not started |
| SPU / SPURS job handling | Not started |
| Audio (`cellAudio` → WASAPI) | Not started |
| Input (`cellPad` → XInput) | Not started |
| PDIPFS asset loading | Not started |

### What the Binary Looks Like

The title ships two signed executables. `EBOOT.BIN` is a 420 KB launcher that
does little more than check the network and hand off; `EMAIN.SELF` is the game.

```
EMAIN.ELF        17,230,248 bytes, ELF64 big-endian PowerPC64
Entry point      0xF9D690
Segments         1 executable (15.9 MB of code), 8 program headers
Instructions     3,997,458 disassembled
Functions        40,476  (39,299 from .opd, the rest from prologue/leaf/branch passes)
                 1,878 of those sit above the last executable section (0xBE0AF4)
                 and disassemble as .rodata; they are dropped before lifting
Imports          439 functions across 26 modules
NIDs resolved    291 / 439
Assets           USRDIR/PDIPFS — 3,019 files, 1.8 GB, Polyphony's own packed VFS
```

Import counts, largest first:

| Module | Fns | Module | Fns | Module | Fns |
|---|---|---|---|---|---|
| `sceNp` | 81 | `sys_fs` | 22 | `cellRtc` | 7 |
| `cellSysutil` | 45 | `cellSpurs` | 22 | `cellHttpUtil` | 5 |
| `cellHttp` | 31 | `cellPamf` | 14 | `cellVpost` | 4 |
| `sysPrxForUser` | 29 | `cellNetCtl` | 13 | `cellMusicUtility` | 4 |
| `cellGcmSys` | 28 | `cellAudio` | 12 | `cellSysmodule` | 3 |
| `sys_io` | 27 | `cellDmux` | 12 | `sceNp2` | 2 |
| `cellImeJpUtility` | 24 | `cellUsbd` | 12 | `cellSsl` | 2 |
| `sys_net` | 22 | `libvdec` | 8 | `cellSysutilAvconfExt` | 1 |
| | | `cellAdec` | 8 | `sceNpBasicLimited` | 1 |

`sceNp` being the single largest dependency is the shape of this title: an
online game whose servers are gone. Most of those 81 calls will need to fail
gracefully rather than work.

The 148 unresolved NIDs cluster in `cellDmux`, `cellPamf`, `libvdec`,
`cellVpost`, `cellImeJpUtility` and `cellUsbd` — the media-decode and peripheral
paths. Those are stub-or-implement decisions for later, not blockers now.

### Lifting

All 38,598 in-range functions went through `ppu_lifter.py` in about a minute on
twelve workers:

```
Input            38,598 functions (+398 recovered by splitting merged ranges)
Emitted          39,653 functions, 15,858 unique call targets
Jump tables      194 dispatchers, 2,178 case targets, all resolved internally
Output           6 chunks + header, 3,404,026 lines, 194 MB of C++
Unlifted         72 instructions  (stvrx/stvlx x33 each, vsumsws x3, vsum4sbs x3)
Embedded data    5,010 .word literals inside function ranges
Clipped          21 functions whose range ends mid-flow; each emits a halt
```

72 unlifted instructions out of 3,997,458 is a good result and a short list: two
VMX unaligned-store forms and two VMX saturating-sum forms. They will need
handling in the lifter before anything that touches them can run, but they are a
bounded, nameable problem rather than an open-ended one.

### Getting to a Plain ELF Was the First Real Problem

GT5P's SELFs are NPDRM binaries with **key revision 0x0001** and NPD **license
type 1** (network). scetool and its clones refuse the RAP path for type 1 and ask
for an IDPS and `act.dat` instead, and the decryptor inherited from the previous
port produced garbage. Four separate things were wrong:

- **`pkg_extract.py` flattened the package tree to basenames.** GT5P's PDIPFS
  stores files as `A/YG`, `B/YG`, `C/YG`… so 1,716 of 3,019 files silently
  overwrote each other and the extraction looked like it had succeeded. Fixed in
  ps3recomp — this affects every title with a directory tree, not just this one.
- **The RAP initial key was wrong** (`8640B926…`, which is also what `ps3sce`'s
  bundled keyfile carries, so it is a widely copied bad value). The correct one
  is `869F7745C13FD890CCF29188E3CC3EDF`.
- **`NP_klic_key` and `NP_klic_free` were swapped.**
- **The NPDRM layer was applied as an XOR against the appldr key.** It is not.
  The 0x40-byte encryption root header gets an AES-128-CBC pass with the
  klicensee-derived key under a zero IV, *then* the appldr AES-256-CBC pass.

The fix is checked, not assumed: run against a *different* title whose decrypted
ELF was already known good, the corrected pipeline reproduces it **byte for
byte**. Only then was it pointed at GT5P.

Deriving the metadata key is the whole trick; segment decryption and ELF
reassembly are handed to `ps3sce` via its `--meta-info` option rather than
reimplemented. Hence two small scripts instead of one large one.

## How It Works

```
NPUA80075.pkg
    | pkg_extract.py           (finalized PKG, AES-128-CTR)
EBOOT.BIN + EMAIN.SELF + PDIPFS
    | self_metainfo.py         (RAP -> klicensee -> metadata key)
    | ps3sce --meta-info -d    (segment decrypt + ELF rebuild)
EMAIN.ELF - PowerPC64 big-endian
    | find_functions.py / ppu_disasm.py / ppu_lifter.py
Generated C++ (thousands of files)
    | clang-cl / MSVC
Native x86-64 executable
    + ps3recomp runtime (HLE OS services)
    + Graphics backend (RSX -> D3D12)
```

## Building

There is no build yet — the lifted C++ exists but has no runtime glue, no
dispatch table and no `main` to link against. What follows is the analysis and
lifting pipeline, which is reproducible today.

### Prerequisites

- **Python** 3.10+ and `pip install -r requirements.txt`
- **ps3recomp** — clone from [sp00nznet/ps3recomp](https://github.com/sp00nznet/ps3recomp)
- **ps3sce** (or scetool) for the SELF segment decrypt
- **A legitimate copy of Gran Turismo 5 Prologue** (NPUA80075) — the PSN package
  and its RAP. You must legally own the game; nothing here is included.

### Steps

```bash
# 1. Unpack the PSN package (yours, not ours)
python /path/to/ps3recomp/tools/pkg_extract.py your.pkg input/pkg

# 2. Derive the metadata key from your RAP, then decrypt
MI=$(python scripts/self_metainfo.py input/pkg/USRDIR/EMAIN.SELF your.rap)
ps3sce -m $MI -d input/pkg/USRDIR/EMAIN.SELF input/EMAIN.ELF

# 3. Analyse
python /path/to/ps3recomp/tools/elf_parser.py     input/EMAIN.ELF --imports > analysis/imports.json
python /path/to/ps3recomp/tools/find_functions.py input/EMAIN.ELF --json --output analysis/functions.json

# 4. Drop the .rodata that find_functions mistook for code, then lift
python -c "import json; f=json.load(open('analysis/functions.json'));   json.dump([x for x in f if int(x['start'],16) < 0xBE0AF4],             open('analysis/functions_code.json','w'))"
python /path/to/ps3recomp/tools/ppu_lifter.py input/EMAIN.ELF     --functions analysis/functions_code.json --output generated     --code-end 0xBE0AF4 --header-name ppu_recomp.h --source-name ppu_recomp.c
```

`0xBE0AF4` is the end of the last executable *section*. The R-X `PT_LOAD`
segment runs 3.6 MB past it into read-only data, and both `find_functions` and
the lifter will happily promote string tables into functions without that bound.

`scripts/decrypt_self.py` is a pure-Python alternative that does the key
derivation and segment decryption in one pass. Its key derivation is correct and
validated; its ELF reassembly is not yet right, which is why the two-step route
above is the documented one.

> **Note:** You must supply your own legally obtained game files. This project
> does not include, distribute, or link to any copyrighted game data.

## Game IDs

| Region | ID | Format |
|--------|----|--------|
| NA (Digital) | NPUA80075 | PSN |
| NA (Disc) | BCUS98158 | Blu-ray |
| EU (Disc) | BCES00071 | Blu-ray |
| JP (Disc) | BCJS30017 | Blu-ray |

This port targets the **US PSN build, NPUA80075 v02.00** (`PS3_SYSTEM_VER 02.1500`).

## Project Structure

```
GT5P/
├── config/
│   └── gt5p.toml            # ps3recomp configuration
├── scripts/
│   ├── self_metainfo.py     # RAP -> klicensee -> decrypted SELF metadata info
│   └── decrypt_self.py      # pure-Python SELF decryptor (key path validated)
├── input/                   # Your game files go here (gitignored)
├── analysis/                # Derived from the binary — regenerate, don't commit (gitignored)
├── generated/               # Recompiled C++ output (gitignored)
└── data/                    # Keys — never committed
```

## Contributing

Early days, and the biggest jobs have not started:

- **The runtime glue** — the next real milestone. Dispatch table, ELF loader,
  import resolution, CRT startup, `main`. Everything downstream is blocked on it.
- **Four VMX instructions** — `stvrx`, `stvlx`, `vsumsws`, `vsum4sbs`. 72 sites,
  and the only gap in an otherwise complete lift.
- **RSX graphics** — GT5P at 1080p on 2006 hardware means an aggressively tuned
  command stream. This is the deep end of the RSX → D3D12 work.
- **PDIPFS** — Polyphony's packed VFS. 3,019 opaque files with two-letter names.
  Nobody has to reverse it if `cellFs` is faithful enough, but knowing its shape
  will help when asset loads start failing.
- **`sceNp` without PSN** — 81 calls into services that no longer exist. Working
  out which ones the boot path insists on is early, tractable work.

If you have worked with RPCS3, N64Recomp, UnleashedRecomp or similar projects,
your expertise would be very welcome.

## License

The code in this repository is MIT licensed — see [LICENSE](LICENSE). That covers
*our* code only: the port scaffold, tooling and build scripts. It does not and
cannot cover Gran Turismo 5 Prologue itself, and no game data is included here.

## Legal

This project is static recompilation toolchain output. It contains no proprietary
Sony or Polyphony Digital code, no game assets, and no decryption keys. You must
provide your own legally obtained copy. It exists for game preservation.

**Gran Turismo** is a trademark of Sony Interactive Entertainment. This project is
not affiliated with, endorsed by, or connected to Sony or Polyphony Digital in any
way.

---

*Built with [ps3recomp](https://github.com/sp00nznet/ps3recomp)*
