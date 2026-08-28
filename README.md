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

## Status: Phase 9 — Job Chains Run, 12 SPU Images Live

> The title's SPURS job chains now run. Twelve SPU images are lifted and
> registered — the WWS job manager policy module plus eleven job binaries — and
> **every dispatch hits**, around 500,000 of them in 90 seconds. Downstream of
> that the boot woke up: fifteen guest threads, `cellAudio` initialised with its
> mixing thread running, `cellPad` up, and the first file the title has ever
> asked for opened and read. It still has not drawn a pixel or loaded an asset.

| Milestone | Status |
|-----------|--------|
| PKG extraction | **Done** — 3,019 files, 1.8 GB |
| SELF decryption (`EBOOT.BIN`, `EMAIN.SELF`) | **Done** — RAP-derived, validated against a known-good control |
| ELF analysis & import resolution | **Done** — 439 imports, 291 NIDs resolved (66%) |
| Function boundary detection | **Done** — 40,476 found, 38,598 in executable sections |
| PPU code lifting | **Done** — 39,657 functions, 3.4M lines, 72 instructions unlifted |
| Import resolution (NID) | **Done** — all 439 thunks lifted as `ps3_hle_call`, nothing patched at load |
| Project scaffold & build system | **Done** — clang-cl + Ninja, 70 MB executable |
| ELF loading & VM setup | **Done** — segments, PT_TLS, OPD entry, fault-commit |
| CRT initialisation | **Done** — TLS block, `r13`, argv/envp |
| LV2 syscall dispatch | **Done** — full table, 5 guest threads run |
| SPU lifting | **Done** — 12 images, 4,328 functions, zero dispatch misses |
| SPURS job chains | **Done** — created, kicked, walked, completion signalled |
| Audio (`cellAudio` → WASAPI) | **Partial** — pipeline up, mixing thread runs, no output device |
| Input (`cellPad` → XInput) | **Partial** — `cellPadInit` reached |
| Filesystem | **Partial** — `PARAM.SFO` opens and reads; no asset loads yet |
| Graphics (RSX → D3D12) | **Partial** — GCM init, 1280×720, tiles/zcull/buffers; nothing drawn |
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

### What the First Boot Does

```
$ ./build/gt5p.exe
=== Gran Turismo 5 Prologue Recompiled ===
Built with ps3recomp | 39657 lifted functions
[ELF]  segments 0x00010000 (15.9 MB, R-X) and 0x00F50000 (RW), PT_TLS 0x01034570
[ELF]  entry OPD 0x00F9D690 -> code 0x00010230, TOC 0x010382D8
[crt]  sys_initialize_tls: block 0x0E000000, r13=0x0E007000
[SPU]  initialize(nspu=6, nrawspu=0)
[SYS]  5 guest threads created -- Job Manager Event Handler, spurs_printf_handler, 3 PDI workers
[cellSpurs]     Initialize nSpus=5, 4 workloads added (Wws_Job, pm=0x00F4CB80, 11,648 bytes)
[cellGcmSys]    Init(cmdSize=0x10000, ioSize=0x100000, ioAddr=0x40000000)
[cellVideoOut]  Configure resId=2 -> 1280x720, pitch 8192
[cellGcmSys]    3 tiles, zcull 1920x1088, display buffers 0 and 1, MapMainMemory(0x20000000, 174 MB)
```

…and then the SPU actually runs:

```
[GT5P]      SPU: Wws_Job registered (fp=0x6FFFB30E41EE17BC, 11648 bytes at 0x00F4CB80)
[cellSpurs] JobGuardInitialize(guard=0x20039900 chain=0x20039780 notify=1 autoReset=1)
[cellSpurs] wid=0 PM resolved (fp=0x6FFFB30E41EE17BC image=1)
[spurs-pm]  DMA cmd=0x40 lsa=0x00B00 ea=0x020017C00 size=0x100 tag=8
[spurs-pm]  DMA cmd=0x20 lsa=0x00B00 ea=0x020017C40 size=0x10 tag=8
[spurs-pm]  DMA cmd=0x40 lsa=0x00D00 ea=0x020017C80 size=0x80 tag=8
```

That is lifted SPU code executing on a host thread, issuing MFC transfers to pull
its workload descriptor out of guest main memory. All four of this title's SPURS
workloads carry the same program — `Wws_Job`, Sony's WWS job manager, 11,648
bytes embedded in the ELF at `0x00F4CB80`. Unlike the job binaries most titles
load from their data files, this one could be taken statically: extract, 193
functions from `find_spu_functions.py`, lift, register.

It then settles: the main thread polls at guest `0x00941EE0` on a 20 ms
`sys_timer_usleep`. The job manager reads its descriptor, finds no job to claim
and returns, so nothing advances past that.

Two things worth noting about how quiet the run is. **No NID went
unimplemented** — 487 handlers were registered from ps3recomp's libraries and the
boot path did not reach past them. And **no file was opened**: the title has not
asked for a single byte of its 1.8 GB of assets yet, which places the stall
before any content loading, not inside it.

### What Unblocked It

Two missing pieces in ps3recomp's `cellSpurs`, both of which made the title hang
with nothing in the log to say why:

- **`cellSpursCreateJobChain` had no implementation.** Only the `WithAttribute`
  form existed, so the plain form fell through to the unresolved-NID path, faked
  `CELL_OK`, and registered nothing. The chain did not exist as far as the
  runtime was concerned.
- **`cellSpursKickJobChain` was a no-op** — and declared `(spurs, jobChain)` when
  it is `(jobChain, numReadyCount)`. It is the *other* way a title starts a
  chain: `Run` for one created ready to go, `Kick` to hand the SPUs more of an
  existing one. This title uses Create + Kick, so its chains were created and
  then never walked.

Both are fixed in [ps3recomp#98](https://github.com/sp00nznet/ps3recomp/pull/98),
which this port currently requires.

With the chain walking, eleven distinct SPU job binaries showed up as
`[spurs-job] dispatch MISS` — captured with `SPU_DUMP_MISS`, lifted, and
registered by `scripts/lift_spu_jobs.py`:

```
spujob_1F4DFFB8347F469B_9440    198 functions
spujob_2700D2E254DC9B26_19216   332
spujob_50B204D2E7F341C3_69152   546
spujob_95895166008B201A_25344   419
spujob_96888A5FD8A35332_53648     1   <-- almost certainly wrong
spujob_BE66D8D2210CDCD4_36448   691
spujob_CDC79000AF23EFEA_24816   414
spujob_CF6687DDC3CEB944_17200   298
spujob_F13517B6BAAB5638_20448   355
spujob_FE904C090B0D0DFE_13840   251
spujob_FF5E29441A480DBC_53296   630
```

4,135 SPU functions across the eleven, plus 193 in the policy module. Zero
dispatch misses on the next run.

### Where It Is Now

The boot went from 293 log lines to 3.8 million. It runs a steady job loop —
two jobs dispatched over and over — with fifteen guest threads alive, audio
initialised, and `cellSysutil`'s disc-game registration done. What it does *not*
do is load anything: the only file it has ever opened is `PARAM.SFO`.

Two of the eleven lifted job images are visibly not right, and are the obvious
next thing to look at:

- **`j96888A5F`** lifted **1 function from 53,648 bytes**. `find_spu_functions`
  found no seeds it trusted, so almost all of that image is unlifted.
- **`jBE66D8D2`** issues MFC transfers to garbage effective addresses
  (`0x7801C102`, `0x2502C081`), which the runtime rejects as malformed. Its
  lifted code is computing addresses wrong somewhere.

Also outstanding, but not what is blocking: `cellSpursShutdownJobChain`,
`cellKbInit`/`SetReadMode`/`SetCodeType` and `cellMouseInit` are unresolved
NIDs, and WASAPI refuses the 8-channel format the title asks for
(`AUDCLNT_E_UNSUPPORTED_FORMAT`), so audio initialises but has no output device.

### The Fingerprint Is Not FNV-1a-64

Registering a lifted SPU image means matching it by content hash, and the obvious
move is to compute that hash offline and bake in the constant. That silently does
not work here.

`spu_workload_fingerprint` is documented as FNV-1a-64 and seeds with
`1469598103934665603` — the real offset basis, `14695981039346656037`
(`0xCBF29CE484222325`), with a digit dropped. It is self-consistent, so it is a
perfectly good hash and nothing inside the runtime notices. But every offline
FNV-1a-64 implementation disagrees, and a mismatched fingerprint is not an error:
the workload is simply logged as `PM NOT LIFTED` and never runs.

This port sidesteps the whole question by computing the fingerprint at
registration time with the runtime's own function, over the image already in
guest memory. Correct whichever value the basis holds.

### Imports Resolve Without an Import Resolver

The usual approach is to patch the guest's import table at load time — walk the
PLT, write function descriptors, hope the addresses were read correctly. This
port does none of that.

Every imported function has a `li r12,0` thunk in `.text` that the PS3 loader
would have patched at boot. `scripts/gen_hle_stubs.py` reads the ELF's own module
descriptors for the `(thunk address, NID)` pairs — 439 of them across 26 modules,
all in `0x00BDD414..0x00BE0AD4` — and hands them to the lifter's `--hle-stubs`,
which emits each thunk body as:

```c
void func_00BDD414(ppu_context* ctx) {
        ps3_hle_call(0x0B168F92u, ctx); return;  /* import stub */
}
```

Resolution is then the runtime's NID registry, decided at compile time. There is
no load-time patching to get wrong, no hardcoded slot addresses to re-derive when
the binary changes, and the mechanism is title-agnostic.

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

### Prerequisites

- **Python** 3.10+ and `pip install -r requirements.txt`
- **ps3recomp** — clone from [sp00nznet/ps3recomp](https://github.com/sp00nznet/ps3recomp).
  Currently needs [#98](https://github.com/sp00nznet/ps3recomp/pull/98) (SPURS
  job chains) and [#97](https://github.com/sp00nznet/ps3recomp/pull/97)
  (`pkg_extract` directory tree).
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

```bash
# 5. Build (clang-cl + Ninja inside a VS x64 environment)
scripts/build.cmd

# 6. Run
./build/gt5p.exe

# 7. Capture and lift any SPU job image the runtime does not recognise,
#    then rebuild. Repeat until no `dispatch MISS` remains.
SPU_DUMP_MISS=analysis/spu/dump ./build/gt5p.exe
python scripts/lift_spu_jobs.py
scripts/build.cmd
```

`scripts/build.cmd` sources `vcvars64.bat`, then configures and builds. The
lifted chunks are ~35 MB of C++ each; the first build takes a while.

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
│   ├── decrypt_self.py      # pure-Python SELF decryptor (key path validated)
│   ├── gen_hle_stubs.py     # ELF import descriptors -> lifter --hle-stubs map
│   ├── lift_spu_jobs.py     # captured SPU job images -> lifted + registered
│   └── build.cmd            # vcvars64 + cmake + ninja
├── src/
│   ├── main.cpp             # VM bring-up, ELF load, entry
│   ├── elf_loader.h         # PT_LOAD / PT_TLS / OPD entry
│   ├── compat/              # <dirent.h>/<unistd.h> shims for ppu_fs.cpp on Win32
│   └── gen/
│       ├── ppu_hle_nids.cpp # generated: 488 NID -> ps3recomp handler registrations
│       └── spu_workloads.c  # generated: 12 SPU images -> fingerprint registrations
├── CMakeLists.txt
├── input/                   # Your game files go here (gitignored)
├── analysis/                # Derived from the binary — regenerate, don't commit (gitignored)
├── generated/               # Lifted C++ output (gitignored)
├── build/                   # Build output (gitignored)
└── data/                    # Keys — never committed
```

## Contributing

Early days, and the biggest jobs have not started:

- **The two bad SPU images** — `j96888A5F` (1 function lifted from 53 KB) and
  `jBE66D8D2` (malformed MFC addresses). Described above; the most likely reason
  the title runs a job loop without ever loading anything.
- **Why nothing is loaded** — 1.8 GB of assets in `USRDIR/PDIPFS` and the title
  has opened exactly one file.
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
