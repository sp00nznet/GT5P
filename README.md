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

## Status: Phase 11 — The RSX Is Configured

> The main thread now reaches graphics setup. `cellGcmInit(cmdSize=0x10000,
> ioSize=0x100000, ioAddr=0x40000000)`, `cellGcmMapMainMemory(0x20000000, 174
> MB)`, six tile regions, zcull, two 1280×720 display buffers, and a first
> `cellGcmSetPrepareFlip(0)`. It got there because the SPURS completion event
> was throwing away the one field the game reads from it — see
> [The Audio Loop That Could Not Count](#the-audio-loop-that-could-not-count).
> `put` still sits at `0x10040`, no asset is loaded yet, and the boot ends in an
> `operator new` retry loop, so there are still no pixels.

| Milestone | Status |
|-----------|--------|
| PKG extraction | **Done** — 3,019 files, 1.8 GB |
| SELF decryption (`EBOOT.BIN`, `EMAIN.SELF`) | **Done** — RAP-derived, validated against a known-good control |
| ELF analysis & import resolution | **Done** — 439 imports, 291 NIDs resolved (66%) |
| Function boundary detection | **Done** — 40,476 found, 38,598 in executable sections |
| PPU code lifting | **Done** — 39,660 functions, 3.4M lines, 72 instructions unlifted |
| Import resolution (NID) | **Done** — all 439 thunks lifted as `ps3_hle_call`, nothing patched at load |
| Project scaffold & build system | **Done** — clang-cl + Ninja, 70 MB executable |
| ELF loading & VM setup | **Done** — segments, PT_TLS, OPD entry, fault-commit |
| CRT initialisation | **Done** — TLS block, `r13`, argv/envp |
| LV2 syscall dispatch | **Done** — full table, 15 guest threads run |
| SPU lifting | **Done** — 12 images, 4,328 functions, zero dispatch misses |
| SPURS job chains | **Done** — created, kicked, walked, per-job completion signalled |
| RSX configuration | **Done** — GCM init, main-memory map, tiles, zcull, display buffers |
| Audio (`cellAudio` → WASAPI) | **Partial** — SGX service loop runs to completion, no output device |
| Input (`cellPad` → XInput) | **Partial** — `cellPadInit` reached |
| Filesystem | **Partial** — `PARAM.SFO` opens and reads; no asset loads yet |
| Graphics (RSX → D3D12) | **Partial** — configured, but `put` never advances past `0x10040` |
| Present / vblank ticker | **Done** — `src/gt5p_present.cpp`, 60 Hz |
| PDIPFS asset loading | Not started |

### The Audio Loop That Could Not Count

The whole boot hung behind one thread. `sgx-audio-thr` held the job chain's
lightweight mutex at `jobchain + 0x200` and never released it, so the main
thread sat in `sys_lwmutex_lock` for the rest of the run — 10.4 seconds and
counting, with the guest-side owner field naming the audio thread the entire
time.

It was not stuck. It was looping, and its loop is this:

```
func_006CEC74(obj):
    lock is already held by the caller
    do {
        cellSpursJobGuardNotify(obj + 0x180)
        sys_event_queue_receive(obj->queue)      // r6 = event data2
        i = data2
        if (obj->callback[i]) obj->callback[i](obj->arg0[i], obj->arg1[i])
    } while (i + 1 < obj->queued)
    sys_lwmutex_unlock(obj + 0x200)
```

`data2` is *which job finished*. Our SPURS pushed the completion event with
`data2` hardcoded to zero:

```c
u64 d2 = 0, d3 = 0;
if (g_spurs_job_mbox_valid) { d2 = ...; d3 = ...; }   /* computed */
sys_event_queue_push_by_id(q, SPURS_EVENT_PORT, jc_ea, 0, probe());
                                                    /* ...and dropped */
```

`d2` and `d3` were computed from the SPU's outbound mailbox and then never
passed. With `data2` always 0, `i` is pinned at 0, `i + 1 < 2` is always true,
and the loop runs forever: **151,685 receives in 25 seconds, 214,702 SPU job
dispatches, no progress** — all with the mutex held.

The event now carries the mailbox value when the SPU produced one, and the job's
ordinal in the chain otherwise, which is exactly what that loop counts.
[ps3recomp#98](https://github.com/sp00nznet/ps3recomp/pull/98).

That one field was worth the whole graphics stack.

### A Detour That Was Not the Cause

`cellSpursShutdownJobChain` (NID `0x738E40E6`) was also unregistered and faked
`CELL_OK` thirteen times a boot, which made it the obvious suspect for a chain
that would not tear down. It is implemented now — chains carry a shutdown flag,
`jc_execute` honours it wherever the walk has reached, and Shutdown signals
completion so a parked thread wakes.

It did not move the boot at all. The NID was genuinely missing, which is why it
is in the PR, but the hang it was reached from had a different cause. Recorded
here because a negative result that cost a build cycle is worth the same line in
the log as a positive one.

### 363,577 Writes to Address Zero

With the audio chain finally running, its second job (`image 7`, fingerprint
`0xBE66D8D2210CDCD4`) turned out to be spilling its entire local store to guest
EA `0x0000` on every single pass — LS `0x0000`/`0x4000`/`0x8000` to EA
`0x0000`/`0x4000`/`0x8000`, 34 KB a lap. Of 364,600 SPU DMA PUTs in a 40 second
boot, 363,577 went to the null page:

| before | | after | |
|---|---|---|---|
| `ea 0x00000000` | 363,577 | `ea 0x01000000` | 197 |
| `ea 0x01000000` | 1,020 | `ea 0x20000000` | 3 |
| `ea 0x20000000` | 3 | | |

lv2 reserves the low 64 KB; an EA that small means the descriptor field holding
the real destination arrived as zero. ps3recomp already rejects malformed MFC
transfers for the same reason, so this joins them
([ps3recomp#107](https://github.com/sp00nznet/ps3recomp/pull/107)). The job
still reads its inputs from the wrong place — it also issues GETs from EAs like
`0x7801C102` — but the histogram is now readable, which it was not before.

### The Next Wall: a Heap That Grew Out of Its Own Region

The boot now ends in `operator new` retrying forever:

```
func_00951FA8(size):
    for (;;) {
        p = alloc(size);  if (p) return p;
        sys_timer_usleep(2000);          // func_00947F28
    }
```

A 264-byte (`0x108`) allocation fails 2,198 times in a run. The failure is not
the interesting part — the three allocations before it are:

```
func_0094FF30(heap=0x011806B0, size=0x37000, align=0x40) -> 0x42C49000
func_0094FF30(heap=0x011806B0, size=0x37000, align=0x40) -> 0x42C11FC0
func_0094FF30(heap=0x011806B0, size=0x29521, align=0x10) -> 0x42BE8A80
```

Those succeed, and they are a perfectly well-behaved downward bump allocator:
each return is the previous one minus the requested size plus the alignment.
They are simply bumping through `0x42Cxxxxx` — 1.1 GB in, where this title has
no memory at all. Read as floats they are 98.28, 96.56 and 95.27.

The heap descriptor says where it should be:

```
0x011806B0: 00F76D48 20000000 2ADFFF80 652D6E70
            vtable   base     limit    current
```

`base` and `limit` are right — the 174 MB region. `current` is `0x652D6E70`,
far outside `[base, limit)`.

Walking back from there, the free-list node the allocator carves from carries an
end pointer of **`0x42C80000`** — which is `100.0f`. `func_0094F6E8` splits a
block by taking `[node+4]` as the block's end, and every subsequent return is
that value minus the running total. The allocator itself knows the value is
wrong: the very next instruction is

```
lwz    r0, 0x8(r3)      # heap->limit = 0x2ADFFF80
cmplw  cr7, r0, r9      # r9 = 0x42C80000
blelr  cr7              # limit <= end -> bail out, skip the back-link
```

so it bails and leaves the list half-linked rather than rejecting the block.

That value propagates node to node, so an address watch only ever catches the
copy. Watching the *value* instead ([ps3recomp#108](https://github.com/sp00nznet/ps3recomp/pull/108))
found the origin in one run.

### An Arena With No End

`func_006A4400` is a bump arena:

```
r9 = arena->base;
if (r9 && out) { *out = r9 + arena->cursor; memset(*out, 0, size); }
arena->cursor += size;          /* no limit, no check, ever */
```

It is run twice. Pass one with `arena->base == 0` only accumulates sizes; the
caller then takes one CRT block that big, points the arena at it, and reruns the
identical code to fill. The two passes have to agree, and here they do not:

```
pass 1   out=0xFFFFF4FC  size=0x0      cursor 0x58    <- block sized 0x58
pass 2   out=0xFFFFF4FC  size=0x26C    cursor 0x2C4   <- and on to 0x480+
```

`0x26C` is `31 × 20`: a count that is 0 while measuring and 31 while filling.
So the arena gets a `0x58` block and writes more than a kilobyte into it,
marching 20-byte records — glyph metrics, floats defaulting to `100.0f` —
straight across the CRT's free-list nodes. That is where the heap's `next`
pointer becomes `100.0f`, and that is what eventually parks the main thread in
`operator new` forever.

The chain is confirmed end to end. `GT5P_HEAPPAD=<bytes>` adds slack to every
CRT block; with more slack than the overrun, the boot walks straight past the
retry loop:

| | `GT5P_HEAPPAD=0` | `GT5P_HEAPPAD=4096` |
|---|---|---|
| main ends at | `operator new` retry, `size=0x108` | `func_00938698`, the PDI path |
| heap reaches | `0x20017xxx` | `0x2043F510` |

That is a diagnosis, not a fix — it is a debug pad, and the real repair is
whatever makes the two passes disagree. But it moves the main thread into the
same function the six PDIPFS worker threads sit in, which is the first time this
port has had main anywhere near asset loading.

Still no file but `PARAM.SFO` opens, so that is the next wall.

### Older Findings

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

### The Current Blocker, Precisely

Main livelocks. `GT5P_MAINSTACK=<seconds>` suspends it, unwinds its **host**
stack with the Win64 unwinder and maps each frame back through the function
table; `GT5P_PEEK=<addr>[,...]` dumps guest words with one level of pointer
indirection. Together they walk the whole chain out.

```
[hostchain]  <host>  func_00B756D0  func_0083A210  func_0083AAA0  func_007FA158
             <host>  func_00838A60  func_00A274B8  func_00A27810  func_0002FB38
             <host>  func_007D2528  func_007D6C60  func_00011B28 ... func_00010230
[mainregs]   r4..r12 = CCCCCCD4   r27 = CCCCCCD4
```

`func_00B756D0+0x11C` allocates a 24-byte node and links it into a container,
about two million times over (the refcount it bumps climbs past `0x200990`).
Every pointer it touches is `0xCCCCCCD4`, and here is where that comes from:

1. `func_008B0A38` picks size class 5 for a 24-byte request and tail-branches to
   the class allocator `func_008B0920` with `entry = pool->base + 5*80`.
2. The pool at `0x010E3760` and its 20-entry array at `0x20006458` are built
   **correctly** — `func_008B02B8` allocates `n*80+8`, sets `pool->base` to
   `block+8`, `func_008AF3B8` zeroes each 0x20-byte entry and initialises its
   lock at `+32`, and `func_008AF2C8` fills in `{+0: owner, +4: size}`. All
   verified with the runtime's store watch (`LBP_WW`).
3. The class list is empty, so `func_008B0818` refills: `func_008B0798` carves a
   chunk, `func_008AF308` links it, and the head at `entry+8` is written —
   **`0xFFFFFFFF`**.
4. The next pop follows `-1` as a pointer, reads the top of the 4 GB VM window,
   and hands back garbage. `func_008B0920` stamps `0xCCCCCCCC` into the block's
   first three words (its uninitialised-memory poison) and returns `block+16`.
   `0xCCCCCCCC + 8 = 0xCCCCCCD4`.

The refill path itself reads clean: `func_008AF978` takes a 4080-byte page from
the pool's page allocator at `pool[+0x08]`, falls back to the general allocator
if that returns 0, and initialises the chunk. Nothing there can produce `-1`.

And the store watch shows the `0xFFFFFFFF` is **transient** — the very next
write to `entry+8` is a real pointer, `0x20097900`. So `-1` looks like an
"insertion in progress" sentinel, and the failure is a **reader following it**:
`func_008B0920` takes the entry's own lock at `entry+32` (`func_00938DA0`)
before touching the head, so on hardware nobody can observe the sentinel. That
makes the per-entry lock a suspect — but not the only one.

Narrowing further: the head store is not in `func_008B0818` at all. It calls
`func_008AF340` (unlink) and `func_008AF308` (link) to move a chunk to the front
of the list, and the store watch attributes their inlined bodies to the caller.
An unlink that empties the list should write `0` — the reader's only test is
`head == 0`. Writing `-1` means the last node's *next* field is `-1` rather than
`0`, which points at the chunk initialiser `func_008AF1B8` or at the splice
itself.

Those three were checked and are lifted faithfully — `func_008AF1B8` zeroes the
node's `prev`/`next` correctly, and the splice reads clean. The corruption is
elsewhere, and it is bigger than one word.

**The entry itself is wrong.** Read at steady state, and confirmed against raw
memory (`GT5P_PEEK` prints both `vm_read32` and the mapping directly — they
agree, so the accessor is not lying):

```
entry[5] @ 0x200065E8   owner=FFFFFFFF  size=FFFFFFFF  head=20097900  tail=FFFFFFFF
                        cache=00000000  count=00000001  max=FFFFFFFF  total=0243E79A
```

`owner` should be the pool (`0x010E3760`) and `size` should be `0x18`. Both are
written at init — the store watch sees `0x200065EC <- 0x18` and
`0x200065E8 <- 0x010E3760` — and both are `-1` a second later.

That is what drives the livelock: `func_008B0920`'s only test is
`[entry+4] == 0`, meaning "this size class is unused, fall back to the general
allocator". With `size` reading `-1` it *never* takes that branch, and instead
pops from a free list whose links are garbage.

Armed at the exact instant of the init store
(`LBP_WW=0x200065E8 PPU_WW_GUARD=1`), the page guard names two writers of that
word: one through `vm_write32` — a normal lifted 32-bit store, but the guard
reports the faulting RIP inside the accessor, so the *guest* caller is not
identified — and one from inside the lifted body of `func_00494F70`.

Ruled out this round, each cheaply and definitively:

- **Not SPU DMA.** `PS3_NO_JOBCHAIN=1` stops every job and reproduces exactly.
- **Not a faked-OK import.** `PS3_HLE_UNRESOLVED=fail` reproduces exactly.
- **Not dropped writes.** `ppu_vm_size` is 0, so the bounds check is disabled and
  nothing is being silently discarded.
- **Not the `ydkj_memmove` body override** — it is env-gated and off.

### Both Writers, Named

The guard already dumped a guest call stack on a watched-line hit; it just did
not print the writing routine's **live arguments** unless the watched word went
to zero. [ps3recomp#99](https://github.com/sp00nznet/ps3recomp/pull/99) removes
that restriction, and the answer falls out immediately. Two initialisers write
the same memory, both reached from the same CRT sequencer `func_00010368`:

```
1. the allocator pool                     r3=0x200065E8 r4=0x010E3760 r5=0x00000018
   _start -> func_00010368 -> func_00010200 -> func_000119B8
          -> func_008B0F10 -> func_008B02B8

2. a 1 MB, 64 KB-aligned arena setup      r3=0xFFFFF984 r4=0x00010000 r5=0x00100000
   _start -> func_00010368 -> func_000107F8 -> func_00011A28 -> func_00013D10
          -> func_00013EF0 -> func_00668390 -> func_009BFEF8 -> func_009BFCD0
          -> func_009C02A8 -> func_009DA12C
```

The first builds the size-class table. The second — the CRT heap initialiser,
`func_009BFEF8` passes it `size=0x100000, align=1920` — runs afterwards and
writes over it. That is what leaves `owner` and `size` reading `-1`.

### It Is Not the Ordering

Traced and eliminated. `func_00010368` calls `bl 0x9377C0` — the CRT heap
initialiser — at `0x0001043C`, **before** static init at `0x0001048C` and main
at `0x000104B8`. Verified in the disassembly *and* in the lifted C, including
the `bge` that guards the loop above it. `func_00937EF0` (malloc) also calls it
lazily when the heap slot is null, and that slot is set (`*(0x0117FEA0)` =
`0x011806B0`), so it initialises exactly once. The heap exists before anything
allocates from it, as it must.

The CRT heap is the whole guest arena: object at `0x011806B0`, base from
`*(0x01180730)`, size `*(0x01180734)` = `0x0AE00000` — 174 MB, which is exactly
what `cellGcmMapMainMemory(0x20000000, 0xAE00000)` maps.

### The Allocator Is Fine, and So Is the Table

`scripts/instrument_alloc.py` wraps a lifted function in place so its arguments
and return value can be logged. That tool was the missing piece all along: the
runtime's dispatch-table override only intercepts *indirect* calls, and a `bl`
in lifted code calls `func_XXXXXXXX` directly and never looks the address up.
The wrapper has to go in beside the definition, in the generated source.

With `func_0094FF30` — the CRT heap's block allocator — wrapped, and
`src/gt5p_allocwatch.cpp` checking every returned range against every previous
one:

```
[alloc] #42 func_0094FF30(heap=0x011806B0 size=0x400000 align=4096) -> 0x2A9BF000
[alloc] #43 func_0094FF30(heap=0x011806B0 size=0x70    align=16)   -> 0x200063D0
[alloc] #44 func_0094FF30(heap=0x011806B0 size=0x648   align=16)   -> 0x20006450
[alloc] #45 func_0094FF30(heap=0x011806B0 size=0x200000 align=4096) -> 0x2A7BE000
```

495 allocations, all sane. The pool's page-allocator object (#43) and its
size-class array (#44, `0x648` = 1608 bytes = 20×80+8) are **uniquely owned** —
nothing else is handed that memory. The overlaps the detector does flag are
sub-heaps nesting inside blocks they legitimately own (#48–#52 inside #42 and
#35) and repeated returns of a small block, which the detector calls an overlap
only because it does not track frees.

The table is right too. Wrapping `func_008AF2C8`, the per-entry initialiser:

```
[alloc] #45 func_008AF2C8(entry=0x20006458 owner=0x010E3760 size=4)
[alloc] #50 func_008AF2C8(entry=0x200065E8 owner=0x010E3760 size=24)
[alloc] #58 func_008AF2C8(entry=0x20006868 owner=0x010E3760 size=56)
```

Once per entry, `size = (i+1)*4`, entry[5] getting exactly the `0x18` it should.
So `{owner, size}` are correct at construction and something overwrites them
later.

**A correction to the previous round.** The page guard captures the writing
routine's registers *mid-function*, not at entry, so reading them as arguments
was wrong. `func_009DA12C`'s real arguments are `(stack_ptr, 1, 0x40000000)` and
it is called exactly once — it is not "a 1 MB, 64 KB-aligned arena setup". Those
were reused registers.

### A Canary, and an Exact Answer

`GT5P_CANARY=<addr>:<expected>` (in `src/gt5p_allocwatch.cpp`) checks a guest
word on every wrapped call, arms itself once the word first reaches the expected
value, and reports the first call after which it changed. It runs **on the
writing thread**, so unlike every earlier probe there is no buffered-stderr
ordering to guess at:

```
[canary] 0x200065E8 reached 0x010E3760 at call #138 (func_008AF2C8)
[canary] 0x200065E8 changed 0x010E3760 -> 0xFFFFFFFF, first seen at call #577
```

Correct at call #138. Wrong by call #577. The window is inside
**`func_009BFCD0`** — the GCM / video-out setup, reached
`main → func_00013EF0 → func_00668390 → func_009BFEF8 → func_009BFCD0` —
immediately before its single call to `func_009DA12C(stack, 1, 0x40000000)`,
where `0x40000000` is the GCM `ioAddr`.

Cleared inside that window: `cellVideoOutGetDeviceInfo` writes 268 bytes at
`0x011B140C` and `cellVideoOutGetState` 16 bytes beside it — both in the data
segment, nowhere near the pool.

**A caveat worth stating**, because it cost time: `ppu_prof_resolve_host` maps a
host address to the *nearest preceding* function-table entry. A frame inside
lifted code that has no entry of its own is silently attributed to whatever
comes before it. `func_009C0098` and `func_009C02A8` both appeared in these
chains repeatedly and are, on instrumenting them, **never called**.

### Found It: the Runtime Was Writing on the Game's Heap

The corruption is `ps3recomp` publishing its GCM bookkeeping into memory this
title owns.

`VM_HLE_INJECT_BASE` is where `cellGcmSys` puts the label block, the control
register, and the two 4096-entry offset tables the guest reads through
`cellGcmGetOffsetTable` — 0x8000 bytes the runtime writes without the title
knowing. It was `0x03000000` until flOw's heap grew over it, then `0x20000000`.
And GT5P maps its own 174 MB heap exactly there:

```
[cellGcmSys] MapMainMemory(ea=0x20000000, size=0xAE00000)
```

So every `gcm_publish_offset_tables()` wrote 16 KB of `0xFFFF` through the
game's allocations at `0x20003000..0x20007000` — which is precisely the range,
the value, and the halfword granularity the page guard measured.

It landed on the allocator's size-class table. Entry 5's `size` read `-1`
instead of `0x18`, `func_008B0920`'s only test is `size == 0` ("class unused,
use the general allocator"), so it never took that branch — it popped from a
free list whose links were garbage, returned poisoned pointers, and the main
thread livelocked two million iterations deep for the entire boot.

**Fixed in [ps3recomp#104](https://github.com/sp00nznet/ps3recomp/pull/104)**,
which makes the base a variable a port can set. `src/main.cpp` moves it to
`0x03000000`, a range this title never touches and which the port already
commits. With that in place the pool survives the whole boot — a canary on the
corrupted word never fires — and the main thread progresses past the livelock.

### The Next Wall

Main no longer livelocks; it blocks. From ~20 s on it sits at
`func_006CD570+0x2C`, which is `bl` to the `sys_lwmutex_lock` import with
`r3 = jobchain + 0x200`:

```
func_006CD570 -> func_006C5B60 -> func_006C5C2C -> func_006C5CF8
              -> func_006C2D5C -> ... -> func_006952A8 -> func_00693440 -> main
```

The mutex at `0x20039980` reads `owner=6, recursive_count=1`. Guest thread 6 is
**`sgx-audio-thr`**, sitting at `func_006CEC74` — the SGX audio service loop —
where it does `JobGuardNotify` then `sys_event_queue_receive`, over and over,
**holding the chain's lock the whole time**. It is not stuck: it takes 132 laps
through that loop in 40 s and 214,702 SPU jobs dispatch behind it. It simply
never lets go, and main can never get in to tear the chain down.

`cellSpursShutdownJobChain` (NID `0x738E40E6`) was **unresolved** — faked
`CELL_OK` 13 times a boot — so it looked like the obvious culprit. It is now
implemented ([ps3recomp#98](https://github.com/sp00nznet/ps3recomp/pull/98)):
the chain carries a shutdown flag, the walker honours it, and Shutdown signals
completion so a thread parked on the queue wakes. **It does not move this
title's boot.** The audio thread still holds the lock. Committed because the NID
was genuinely missing, not because it fixed the case that found it.

Also cleared: `cellGcm_fifo_recycle` is never entered (`GCM_RECDBG` prints
nothing), so `put` sitting at `0x10040` is a consequence of main being blocked,
not a separate graphics problem.

So the question is narrower than it looks: **what makes that audio loop exit?**
It waits on an event queue and re-arms; its exit condition is some guest state
that only the teardown path sets — and the teardown path is what is blocked
behind it.

### How It Was Found

Worth recording, because none of the ordinary tools could see this write.

The store is **host-side**, so `LBP_WW` and every other store watch are blind to
it. The page guard can see it, but only if it is armed at the right instant — so
`src/gt5p_allocwatch.cpp` grew a canary: `GT5P_CANARY=<addr>:<expected>` checks
a word on every wrapped call **on the writing thread**, arms once the word
first reaches its expected value, and reports the exact call after which it
changed. That gave "correct at call #138, wrong by #577" with no cross-thread
ambiguity. `GT5P_CANARY_ARM=<call#>[:<page>]` then write-protects any page at
that call, which caught the store itself and let it be bracketed page by page to
exactly `0x20003000..0x20007000`.

Two things cost real time and are worth knowing:

- `ppu_prof_resolve_host` maps a host address to the **nearest preceding**
  function-table entry, so a frame in code with no entry of its own is silently
  attributed to whatever comes before it. `func_009C0098` and `func_009C02A8`
  appeared in these chains repeatedly and are never called.
- The page guard captures the writing routine's registers **mid-function**, not
  at entry, so reading them as arguments produces confident nonsense.

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
  Currently needs [#104](https://github.com/sp00nznet/ps3recomp/pull/104)
  (movable HLE window — without it this title cannot boot),
  [#98](https://github.com/sp00nznet/ps3recomp/pull/98) (SPURS job chains),
  [#99](https://github.com/sp00nznet/ps3recomp/pull/99) (guard live args) and
  [#97](https://github.com/sp00nznet/ps3recomp/pull/97) (`pkg_extract` tree).
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
python /path/to/ps3recomp/tools/find_functions.py input/EMAIN.ELF --json     --seed-json config/extra_seeds.json --output analysis/functions.json

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
│   ├── gt5p.toml            # ps3recomp configuration
│   └── extra_seeds.json     # function starts find_functions missed, seen at runtime
├── scripts/
│   ├── self_metainfo.py     # RAP -> klicensee -> decrypted SELF metadata info
│   ├── decrypt_self.py      # pure-Python SELF decryptor (key path validated)
│   ├── gen_hle_stubs.py     # ELF import descriptors -> lifter --hle-stubs map
│   ├── lift_spu_jobs.py     # captured SPU job images -> lifted + registered
│   └── build.cmd            # vcvars64 + cmake + ninja
├── src/
│   ├── main.cpp             # VM bring-up, ELF load, entry, GT5P_MAINSTACK probe
│   ├── gt5p_present.cpp     # window + 60 Hz vblank/flip ticker, RSX FIFO drain
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

- **`func_008B0798` returns -1** — the current blocker, described above. A 1 KB
  chunk carve out of a 4 MB arena fails, and everything downstream is poison.
- **`jBE66D8D2`** issues MFC transfers to garbage effective addresses. (The other
  suspect image, `j96888A5F`, turned out to be 40 KB of zeros followed by float
  coefficients — a data blob, not code. "1 function from 53,648 bytes" was the
  tool being right.)
- **Why nothing is loaded** — 1.8 GB of assets in `USRDIR/PDIPFS` and the title
  has opened exactly one file. Downstream of the wedge, most likely.
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
