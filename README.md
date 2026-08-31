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

## Status: Phase 15 — longjmp Has To Actually Jump

> `func_00A0A57C` is the PS3 libc `longjmp`. It restores `r1`, `r2`, `r13`-`r31`,
> `LR` and `CR` from the buffer and ends in `blr`, so on hardware the branch goes
> to the address it just put in `LR` — the `setjmp` call site — and not to
> `longjmp`'s own caller. A static recompiler cannot express that, and
> `ppu_lifter` does not try: it emits that `blr` as a plain `return;`.
>
> So the lifted `longjmp` restored the whole guest register file and then
> returned to whoever called it. That was enough to hold this port at four
> files for the entire project.
>
> ```
>                        before          after
> PDIPFS files opened    4               108-118
> SPU job dispatches     159             113,765
> RSX SetTile            0               6
> RSX SetZcull           0               3
> SetDisplayBuffer       0               2
> SetPrepareFlip         0               1
> ```
>
> GT5P's LZ decoder aborts a decompression by `longjmp`-ing out of it.
> `func_00919060` arms a buffer at `obj+0x186C`; when the input runs dry,
> `func_009200F0` fires it. With the jump modelled as a return, control resumed
> **inside a 32 KB window-copy loop** carrying `func_00919060`'s registers —
> loop bound `0`, wrap pointer set to the object's own address — so the copy
> could never terminate and wrote bytes nearly 4 MB past its window. See
> [Finding It](#finding-it) for how that was cornered, and
> [Making longjmp Jump](#making-longjmp-jump) for the fix.
>
> The game now reads its own content in bulk, runs a continuous SPU job loop,
> and configures tiled render targets, Zcull, double-buffered display and a
> flip handler. It prepares one flip and does not go on to a second, so there
> is no frame loop yet and **still no attract mode**. Runs also vary — most
> stop at 7 files, roughly one in four reaches 108 — so a race sits behind
> this that the fix did not address.

### Making longjmp Jump

There are exactly two `setjmp` sites and three `longjmp` sites in the whole
binary, which makes a targeted fix practical. `scripts/setjmp_patch.py`:

- expands a **real host `setjmp`** at each guest-`setjmp` call site — it has to
  be lexically in the frame that will be jumped back into, so it cannot hide
  behind a helper;
- keeps the original lifted `longjmp` body, which is what restores the guest
  register file, and then fires a host `longjmp` to reach the site.

The guest therefore resumes with exactly the register state it expects; only
the transfer of control becomes host-side. Buffers are tracked on a stack and
each arming function's depth is restored on the way out, so a buffer whose
frame has returned can never be jumped into. Frames jumped over hold no C++
objects with destructors — lifted code is plain C in C++ clothing — so the
unwind is safe.

Re-run it after every re-lift, like `instrument_alloc.py`; `generated/` is
rebuilt wholesale.

### Finding It

Worth recording, because the symptom pointed nowhere near the cause. The chain
from the stalled file to the decoder took six layers of vtable dispatch, and at
the bottom `func_00956D20` was entered twice and returned once. It had no
blocking primitive in it, so it was spinning — but 2,280 bytes of dense bit
manipulation is not something to read hoping to spot the loop.

`scripts/bbcount.py` settles it mechanically: drop a counter on every label in
one lifted function and print the hottest. Four blocks at
`0x00957100`-`0x00957118` took 49.8 million hits each. Sampling that loop's
registers is what actually named the bug:

```
iteration          r28 wrap        r26 bound        r31 cursor
        1     base + 0x8000      base + 13        base + 9
    1,000     base + 0x8000      base + 1216      base + 1051
1,000,000     base + 0x8000      base + 25788     base + 25675
5,000,000     0x0112D640         0                base + 3,992,907
```

A million iterations of a perfectly healthy circular window copy, and then the
wrap and bound become values from a completely different function. Registers
that change without any instruction in the loop writing them means a call did
it — and the only call on that path was the one that fires the `longjmp`.

Instrumenting the *setup* block closed it: the loop was always entered with a
correct bound and wrap, every time. So it was never bad input, it was a bad
return.


### Where It Stops Now

Two outcomes, and which one a run gets is a coin toss weighted against us —
roughly three in four stop at 7 files, the rest reach 108.

The 7-file case is a real deadlock, not slowness: five minutes of wall clock
produces the same seven files and a 370 KB log, so nothing is even spinning.
The last thing that happens is a zero-length read into the decoder's input
buffer, then the SPURS job chain settles into a permanent cycle — the main
thread receiving job-chain completions on queue 1 with `d2` walking
0, 2, 4, 6, 8, 0xA, 0xC and never asking for another asset.

What both outcomes now reach, which nothing before this phase did:

- tiled render targets, Zcull, double-buffered display, flip and VBlank
  handlers all configured;
- the D3D12 backend fully initialised — adapter, pipeline states for every
  vertex class, vertex buffer;
- a continuous SPU job loop, 113,765 dispatches in a 75-second run, zero
  dispatch misses.

What neither reaches: a second flip. The title prepares exactly one and never
submits an RSX command buffer, so the FIFO drain has nothing to do and no frame
is ever presented.

It is worth being precise about *why*, because "the FIFO is broken" and "the
title never started rendering" look identical from a `put` pointer that does not
move. Under `GCM_DRAINDBG` the control register reads the same in every run,
including the ones that load 108 assets:

```
[DRAIN] getoff=00010040 put=00010040 ref=00000000
```

`put` never leaves the value `cellGcmInit` gave it. But the title *does* call
`cellGcmGetControlRegister`, exactly once, so it holds the pointer and the path
to the FIFO is wired end to end — it simply never kicks it. It also never polls
`cellGcmGetFlipStatus`. That is not a renderer that is failing; it is a renderer
that has not been asked to do anything yet. The frame loop sits behind whatever
makes the loader stop, and there is no evidence of a separate RSX defect waiting
underneath.

The outcomes are also a spectrum rather than a switch — 7, 39 and 108 assets
have all been observed from the same binary — which fits a race rather than a
branch.

#### The Stuck Runs Ask lv2 For More Memory

The two outcomes differ in one visible way before they diverge. Stuck runs
issue lv2 syscalls **341** and **342**; the run that reaches 108 files never
does. Both are unimplemented here, and `ppu_loader.cpp`'s catch-all answers
`CELL_OK`.

The wrappers are `func_00941808` (341) and `func_00941910` (342), and 341's is
a retry loop:

```
loop:  mr   r3, r29        ; a pointer the kernel is expected to write
       mr   r4, r31        ; 0x300000 -- three megabytes
       li   r11, 341
       sc
       cmpwi cr4, r3, 0
       bne  cr4, .retry    ; anything but 0 -> back-off call, then try again
       ...success...
```

So `CELL_OK` is taken as success and the title proceeds on three megabytes it
was never given. The obvious next thought — report failure instead, and let it
take a fallback path — is wrong, and measurably so: the branch retries on any
non-zero result, so an error answer spins that loop forever.

```
stub answers CELL_OK      7, 7, 0, 7   files
stub answers ENOMEM       1, 1, 1, 1   files
```

`GT5P_SC_FAIL=<numbers>` in the runtime makes named syscalls report failure, so
that experiment is repeatable rather than a one-off patch. The conclusion is
that these have to be *implemented*, not merely answered — but they are a
symptom rather than the cause, since the runs that get furthest never call them
at all. Something makes the stuck runs need memory the good ones do not.


A measurement note that cost real time here. The runtime turns verbose logging
on when stderr is redirected, on the reasoning that a redirected stream means
someone is capturing a log — and `runtime/ps3_log.h` warns in as many words
that the per-event lines are emitted from every guest thread through one
non-fair `FILE` lock, so the logging itself changes what the title does. Every
measurement through a pipe is therefore taken on a differently-scheduled
program. Pass `PS3_VERBOSE=0` when the numbers matter. It does not remove the
7-versus-108 split, so that race is real and not an artefact, but it is the
difference between a 16 MB log and a 370 KB one.

## Phase 14 — The Switch Statements Were Invisible

> `find_functions` walks direct branches. A `bctr` through a jump table is a
> dead end for it, so every case that *only* a computed branch reaches looks
> like unreferenced bytes and never gets lifted. There are 246 such tables in
> this binary and 1,710 targets sitting in that blind spot.
>
> ```
>                       before        after
> functions lifted      39,660        40,320
> spins on queue 0      4,398,000     0
> unresolved indirect   0             0
> SPU dispatch misses   0             0
> PDIPFS files opened   4             4
> ```
>
> The 4.4 million busy-spins on event queue 0 — a thread receiving on a queue
> id nothing ever set, blamed in this document on the Job Manager for weeks —
> were code the switch tables hid. They are gone. File progress is unchanged,
> so this buys headroom rather than a new asset. See
> [Recovering the Switch Statements](#recovering-the-switch-statements).
>
> The sixth async load is now traced the whole way down, from the file device
> to the exact frame that stops it. It ends in a decompressor that is handed a
> zero-length input buffer by an abort protocol and never comes back out. See
> [The Sixth Load, All the Way Down](#the-sixth-load-all-the-way-down).
> Still no attract mode.

### Recovering the Switch Statements

`0x009180AC` had to be seeded by hand before the boot would resolve its
indirect calls. That should have been the tell: it is not a function, it is
case 0 of a twelve-way switch in `func_00917EB0`, and eleven siblings were
sitting in the same blind spot with it.

The shape GCC emits here makes the table self-describing — it goes directly
after the `bctr`, and each entry is a signed 32-bit offset from the table's own
base:

```
cmplwi rX, 11
ble    .Ldispatch
...default arm...
.Ldispatch:
lwz    r11, table@toc(r2)
rldic  r9, r0, 2, 30
lwzx   r0, r9, r11
extsw  r0, r0
add    r0, r0, r11
mtctr  r0
bctr
.long  off0, off1, ...        <- table base == this address
```

`scripts/jumptables.py` reads entries until one stops decoding to a plausible
target, bounded above by the nearest target already seen — the compiler puts
the first case's code immediately after the table, so the table cannot run past
it. That bound is what makes it work without the guarding compare. Keying off
`cmplwi` alone misses this very function, whose compare sits **240 bytes back**,
on the far side of the default arm; a look-back window sized for the common
case finds nothing here.

Of 2,538 targets: 4 were already function starts, 824 land *inside* an
already-lifted function, and 1,710 sit in unlifted gaps. Only the 1,710 are
seeded. Seeding the 824 would split a working function in half, which is the
one way this change could do harm.

A caution earned the hard way, recorded so it is not re-chased: a 1,360-byte
run of real PPC code at `0x009159F8`, starting mid-body with no prologue, looks
exactly like a truncated function. It is not — every branch in the preceding
function was checked and none targets the gap. Unreferenced bytes that decode
cleanly are usually a jump table's cases or an exception landing pad, not
evidence of a lifting bug.

### The Sixth Load, All the Way Down

Five async loads complete; the sixth parks at state 2. The chain from there,
each step measured rather than inferred:

| Layer | What it is |
|---|---|
| `func_00917EB0` | device worker step, vtable slot 16 |
| `func_00917208` | the drain, slot 25 — moves a queued request into `dev+56` |
| `func_00916E40` | slot 26 |
| `func_00925F60` | `FileDevicePFSFSHdd` slot 29 — the packed-file read |
| `func_00916DD0` | calls slot 5 of the object at `handler+12` |
| `func_0091FEB8` | `PDISTD::FileExpandPSX` slot 5 — abort a decompression in flight |

`func_00917EB0` switches on `[req+0x64]` through the jump table above, and the
selector decides everything:

| sel | device slot | `r24` | behaviour |
|---|---|---|---|
| 0, 1 | 17 | 0 | async — queue on `dev+64`, complete on a later tick |
| 2, 3 | 18 | 1 | synchronous — completes this tick |
| 4 | 19 | 1 | synchronous — completes this tick |

That table corrects a reading this document carried for a while. The four
PFSFSHdd loads that "complete" are `sel=4`: they finish *synchronously* and
never touch the queue at all. The stalled load is the only genuinely
asynchronous request on that device, so "four succeed and one fails on the same
device" was never the paradox it looked like.

The read never gets as far as issuing I/O. Its first act is to stop any
decompression already running:

```
func_0091FEB8(obj):
    if (obj[6917] == 0) return       ; nothing in flight -- normal path
    obj[6932] = 0                    ; input buffer base
    obj[6936] = 0                    ; input buffer length
    signal(obj+6876)                 ; wake the decoder
    wait  (obj+6816)                 ; block until it unwinds   <-- never returns
    obj[6917] = 0
```

`FileExpandPSX` carries six auto-reset events at `obj+6796` stride 20, and they
pair up cleanly once you have all six:

```
6796  "start work"       waited by func_00920220, the decompressor thread loop
6816  "work done"        signalled by func_00920220, waited by the abort above
6876  "here is input"    waited by func_009200F0 (refill), signalled by the abort
6896  "buffer consumed"  signalled by func_009200F0, waited by func_0091FBA8
```

The synchronisation primitive itself is sound — a correct auto-reset event,
where a signal with no waiter parks a sticky flag at `+17` that the next wait
consumes, so wakeups are not lost. It was worth checking; it is not the bug.

Instrumenting both sides shows the handshake working right up to the end:

```
> func_0091FBA8(obj, handler)              producer starts
  > func_009200F0(obj, ...)                decoder asks for input
  signal obj+6896  waiter=1                "ready" -- producer was waiting
  wait   obj+6876  waiter=0                decoder waits for the buffer
< func_0091FBA8 ret=0x000FD303             producer returns
signal obj+6876  waiter=1                  the abort wakes the decoder
wait   obj+6816  waiter=0                  reader blocks -- forever
< func_009200F0 ret=1                      decoder wakes with base=0, len=0
```

Entry and return counts name the exact frame that does not come back:

```
func_009200F0  (refill)   1 call,  1 return
func_00956D20  (decode)   2 calls, 1 return     <--
func_00957C88  (stage)    1 call,  0 returns
```

The second `func_00956D20` gets its zero-length buffer and then neither returns,
nor calls the refill again, nor calls the output callback again. It is spinning
in pure computation — 2,280 bytes of dense bit manipulation with no blocking
primitive in it. `func_00920220` can only signal `6816` after that call returns,
so the reader waits on an event nobody will ever reach.

What has been ruled out, so the next attempt starts from here:

- **Not a lifting gap.** No `TODO`/unimplemented marker anywhere in the lifted
  `func_00956D20`; every instruction has a handler.
- **Not a missing jump table.** All nine `bctr`-class words in the decoder are
  `bctrl` — virtual calls through OPDs at vtable slots 2 and 3, not computed
  branches.
- **Not a disassembler bug.** `disasm_audit_operands.py` over the whole ELF:
  `addi`, `lwz`, `stw`, `ori`, `rlwinm` all pass with zero divergences; the four
  residual ones are float/vector *formatting* only.
- **Not a dead worker.** Every device's worker loop runs, and the decompressor
  thread is alive and reaches its refill.

Counting basic blocks inside the lifted function names the loop directly --
`scripts/bbcount.py` drops a counter on every label, and four blocks at
`0x00957100`-`0x00957118` take 49.8 million hits each. That loop is a 32 KB
circular output window: `r25` is the base, `r28 = r25 + 0x8000` the wrap, `r31`
the cursor, and the only exit is `r31 == r26`.

Sampling its registers shows it working perfectly and then falling off a cliff:

```
iteration          r28 wrap        r26 bound        r31 cursor
        1     base + 0x8000      base + 13        base + 9
    1,000     base + 0x8000      base + 1216      base + 1051
1,000,000     base + 0x8000      base + 25788     base + 25675
5,000,000     0x0112D640         0                base + 3,992,907
```

Up to a million iterations the wrap is exact and the bound tracks just ahead of
the cursor — this is a decoder doing its job. Then `r26` becomes **zero** — the
null input buffer the abort installed, propagated into the loop bound — and
`r28` becomes the object's own address. `r31 == r26` can now never hold, the
cursor never reaches the wrap either, and the loop writes bytes at steadily
climbing addresses: nearly 4 MB past the window and still going.

So this is not merely a hang. It is an unbounded write walking through guest
memory, and it is a strong candidate for the heap damage this document has
recorded for a long time without explaining — the allocator walking into
`0x42Cxxxxx`. The decoder is fully and correctly lifted; every instruction in
the loop was checked against its encoding, including the CR field mapping and
the `rldicl`/`rlwinm` masks. What it is fed is wrong, not how it was translated.

What stops this on hardware is `longjmp`. The abort does not expect the
decoder to notice the empty buffer and unwind on its own -- it jumps out of
it. That is the subject of [Phase 15](#status-phase-15--longjmp-has-to-actually-jump),
and fixing it took this port from four assets to a hundred and eight.

## Phase 13 — PDIPFS Mounts, and the Game Reads Its Own Data

> A bare run now does this:
>
> ```
> Open /dev_bdvd/PS3_GAME/PARAM.SFO   -> 1040 bytes
> Open PDIPFS/K/4D                    -> 160 bytes      (volume index)
> Open PDIPFS/5C/B2                   -> 54,842 bytes   (first real asset)
> ```
>
> **The packed filesystem is mounted.** Everything this document described as a
> wall — the missing application script, the attract object nothing would drive,
> the arena that measured an empty container — was downstream of a filesystem
> that did not exist, and it exists now. See
> [Three Bugs and a Command Line](#three-bugs-and-a-command-line).
>
> The async loader runs too. `PDIEXT::FileDelayLoad` cycles
> construct -> wait -> **complete** five times over before the sixth hangs, and
> `sys_cond_signal` — called **zero** times for the whole of this project's life
> until today — now fires 36 times a boot. Still no attract mode.
>
> `GT5P_HEAPPAD` is no longer needed and now hurts: with assets loading, zero
> allocations fail, and padding every block only shifts the layout into a worse
> one (4 files without it, 3 with). The heap still walks into `0x42Cxxxxx` —
> that corruption is real and unfixed — but it no longer wedges anything.

### Three Bugs and a Command Line

GT5P is configured entirely by its command line, and this port had never given
it a usable one.

**`argv` is an array of 64-bit pointers, not 32-bit.** The guest CRT's first act
(`func_00010368`) is

```
r10 = argv + 4;  r28 = argv;
do { r0 = [r10]; [r28] = r0; r10 += 8; r28 += 4; } while (++i < argc);
```

— read at stride 8, write at stride 4: compact an array of 64-bit pointers into
32-bit ones by keeping each low half. It does the same to `envp` straight after.
Writing 32-bit entries made it read every *other* slot, so `argv[0]` came out as
our `argv[1]` and the rest as NULL. **The title had never seen a single
argument, including its own path.**

**It needs `boot_from=<mode>`.** `func_00013C98(name, buf, len)` is a plain
`argv` scan for `name=value`, and it opens with `if (argc <= 1) return 0`. The
filesystem setup asks for `boot_from` first and returns immediately when it is
missing — which is why no file device was ever constructed. The accepted values
sit beside the key in `.rodata`:

```
0x00E98350 "boot_from"    0x00E98360 "bdvd"
                          0x00E98368 "gamedata"
                          0x00E98378 "hddgame"
```

They select different volume formats. `bdvd` looks for `USRDIR/GT.VOL`, the
retail disc layout — pick it and the game dutifully tries to open a file this
package does not contain. `gamedata` looks for PDIPFS, which is what the PSN
package ships.

**`argv[0]` must be the real path.** The setup `strcmp`s it against
`"/dev_bdvd/PS3_GAME/USRDIR/EMAIN.SELF"`. With `/app_home` it takes a different
branch and never opens the volume.

Plus a `cellFs` mapping for the relative `PDIPFS/` prefix: with
`boot_from=gamedata` the title opens `PDIPFS/K/4D` *relative*, because on
hardware its working directory is its own `USRDIR`.

`FileDeviceCellFS` and `FileDeviceGameData` are now constructed three times
each. Before this, nothing in that hierarchy was ever constructed at all.

## Phase 12 — Found the One Thing That Matters

> **GT5P's application flow is a script.** The boot's last act is to look up
> `"scripts/gt5p/Application"` — and that script lives in PDIPFS, Polyphony's
> packed filesystem, which this port has never mounted. Every other symptom in
> this document sits downstream of that one fact: no script, so the attract
> object is never driven; never driven, so `MenuGameObject::wait()` never
> returns; never returns, so the frame loop never starts and `put` stays at
> `0x10040`. The whole investigation now reduces to a single question — see
> [Root Cause](#root-cause-the-game-is-a-script-and-the-script-is-not-there).
>
> The boot is at least stable now: the intermittent crashes that dogged this
> session turned out to be the runtime writing a string over a function-pointer
> table, and six consecutive boots since the fix have not crashed once.

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
| Filesystem | **Working** — PDIPFS mounted; volume index and assets read |
| Graphics (RSX → D3D12) | **Partial** — configured; `put` idle at `0x10040` because no frame loop runs |
| Present / vblank ticker | **Done** — `src/gt5p_present.cpp`, 60 Hz |
| PDIPFS asset loading | **Started** — index + first 54 KB asset; loads stall at `FileDelayLoad` state 1 |

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

### The Write Watch Was Lying by Omission

`LBP_WW` prints its first 64 hits and then goes silent without saying so. I read
that silence as *"nothing writes this field"* four separate times in one
session — the heap bump pointer at `0x011806BC`, the Job Manager queue id at
`0x0118C500`, and both the `f64` and state fields of the stalled file request.
The state field had **2,340 writes** in the watched window. Sixty-four were
shown. Each time, the wrong conclusion sent the investigation somewhere else for
a while.

Fixed in [ps3recomp#111](https://github.com/sp00nznet/ps3recomp/pull/111): the
watch now announces the cap and `LBP_WW_MAX` raises it (`0` = unlimited).

Uncapped, the stalled request's state machine reads straight off:

```
<- 0   func_00916730
<- 1   func_00918310    enqueue: queued on the list at dev+40
<- 0   func_00917DE8
<- 2   func_009167E0    promoted to a second, priority-sorted list at dev+64,
                        worker signalled again
       (nothing further -- the wait needs 3)
```

So the request is neither ignored nor lost: it is promoted through two queues
and parks in the second. `func_009167E0` walks that list sorted on the
`[req+0xD8]`/`[req+0xDC]` key pair, inserts, and signals `dev+76` — the same
object the worker waits on. Whatever drains `dev+64` is the next thing to find.

### Why the Two Arena Passes Disagree

The caller is a textbook measure / allocate / fill, and it checks its own work:

```
r3 = 0;         bl func_006C32A0     ; measure -> r30
bl func_006A3988(r30)                ; allocate r30 bytes
r3 = buffer;    bl func_006C32A0     ; fill
cmpw r3, r30;   beq ok               ; the two totals must agree
```

Inside, the measuring pass reads back a pointer it never wrote:

```
bl func_006A4400(arena, r1+112, 0x44)   ; base == 0 -> does NOT write *out
r4 = [r1+0x70]                          ; reads that slot anyway
...
r11 = [r1+0x70];  cmpdi r9, 0           ; and BRANCHES on it
```

`func_006A4400` only writes `*out` when `arena->base` is non-zero, so on the
measuring pass that stack slot holds whatever was there before. Forcing it to
zero makes the two passes agree:

| | measure | fill | overrun |
|---|---|---|---|
| `GT5P_ARENA_ZERO=0` | `0x58` | `0x4C0` | **0x468 bytes** |
| `GT5P_ARENA_ZERO=1` | `0x11C` | `0x114` | none |

So that stale read is what drives the mismatch. **The experiment is not a fix**,
though: blanking every measuring-pass out-pointer also blanks slots the caller
legitimately uses, and it trades the corruption for crashes — four runs each,
`ARENA_ZERO=0` gives 0 crashes and 4 allocator spins, `ARENA_ZERO=1` gives 3
crashes and 1 spin. It is off by default. What it establishes is the mechanism,
which is worth more than the workaround would have been.

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

### The Next Wall: Everyone Is Waiting, Nobody Is Working

Past the heap, the boot is a textbook idle deadlock. Main's chain is

```
func_00687CD8 -> func_00687F90 -> vtable[+0x04] = func_0068DA68   (start)
                                     [obj+0x44] = 1               (busy)
                                     enqueue into a priority list
                              -> vtable[+0x24] = func_0068DB00    (wait)
                                     sys_cond_wait(cond 93)
```

and it never comes back. The object is a static at `0x0107B754`; `[obj+0x44]`
is set to 1 by the start method and **never cleared by anything, all boot**.

The wake path exists and is easy to name. The engine has three sibling helpers,
each an 11-instruction prologue falling through into a 30-instruction body:

| entry | body | syscall |
|---|---|---|
| `func_00938548` | `func_00938574` | 109 `sys_cond_signal_all` |
| `func_009385F0` | `func_0093861C` | 108 `sys_cond_signal` |
| `func_00938698` | `func_009386C4` | 107 `sys_cond_wait` |

40 call sites across the binary use them. Main's waiter is `func_0068DB00`; its
counterpart is `func_0068E3E0`, reached from `func_006736A0`, which is
`main-vtable+0x10` — the task's "finish" method. **`sys_cond_signal` and
`sys_cond_signal_all` are called zero times in the entire boot.** Every waiter
waits; nothing ever finishes.

The reason is one queue. The PDI worker loop is `func_00935690`:

```
while (running) {
    rc = sys_event_queue_receive(q, &evt, 1000000);   /* 1 s */
    if (rc != 0) return;                              /* give up */
    lock(); for (n = list_head; n; n = n->next) n->vtable[+8]();  unlock();
}
```

It only drains its work list *after* an event arrives. Queue 5 is the PDI
queue — created, never cancelled, and in a 30-second boot:

| queue | receive attempts | events delivered |
|---|---|---|
| 1 | 56,784 | 25 |
| 2 | 56,669 | 18 |
| 3 | 39,387 | 21 |
| **5** | **1,901** | **0** |

Nothing is ever posted to it. The enqueue path main takes
(`func_0068DF78`) is lock, priority-insert, unlock — `func_00938BE8` is plain
`sys_lwmutex_unlock`, not a wake — so the work lands on a list whose worker is
blocked waiting for a notification that has no sender.

The task itself is queued correctly. Both scheduler lists hold it:

```
0x011BB8E8: vtable=00F6F258  head=0107B76C  tail=0107B76C  state=2
0x011BB928: vtable=00F6F278  head=0107B780  tail=0107B780  state=2
```

So nothing is lost or mislinked — the work is sitting on the right list, and no
thread ever comes to take it.

**This probably subsumes the heap bug.** The arena's measuring pass counts
entries in a container that is empty, and its filling pass finds 31 — the same
shape you get when a table is populated between the two passes. Nothing in this
port has loaded any data yet, so "empty while measuring" is exactly what an
un-started asset system produces. If that is right, the arena overrun is a
*symptom* of this deadlock rather than a separate defect, and `GT5P_HEAPPAD`
stops being needed once work actually flows. Worth confirming before spending
effort on a direct fix for the count mismatch.

Queue 5 has no port connected to it at all — the boot wires ports only to
queues 1, 2 and 6 — and the guest never calls `sys_spu_thread_bind_queue`,
`sys_spu_thread_group_connect_event` or `sys_event_port_connect_ipc` anywhere.
SPURS attaches only queue 1. So the sender for queue 5 does not exist yet
because the code that would create it has not run.

### The Binary Still Has Its RTTI

This title was never stripped. Every polymorphic class carries a live
`std::type_info` with a mangled name, and every vtable is preceded by
`{ offset_to_top, typeinfo }`. `scripts/rtti.py` walks that and recovers **1,442
vtables and 8,314 virtual functions**, which turns the whole investigation from
address archaeology into reading:

```
func_0068DB00  MENU::MenuGameObject vtable+0x24
func_006736A0  MENU::MenuGameObject vtable+0x10
func_0068DA68  PDIEXT::GameObjectBase vtable+0x1C
func_00687F90  PDIEXT::AdvertiseSimplePS3 vtable+0x60
```

So the object the whole boot is waiting on is **`PDIEXT::AdvertiseSimplePS3`** —
the attract-mode object — registered into
`PDIEXT::UpdateManagerT<GameObjectPS3>` and
`PDIEXT::RenderManagerT<GameObjectPS3>`, sitting next to `PDIEXT::MPEGStream`
and `PDIEXT::PAMFStream`. Attract mode here is a streamed movie, and the boot
sequence is: activate the object (`GameObjectBase vtable+0x1C` sets busy and
registers it with both managers), then `MenuGameObject::wait()` blocks until it
deactivates.

Nothing ticks the managers. The only loop running is tid 1's sysutil pump
(`usleep(20000)` then `cellSysutilCheckCallback`, 888 calls in 30 s); the game's
own frame loop is main, and main is inside the wait.

`analysis/rtti.json` is derived from the game binary, so it is gitignored like
everything else in `analysis/` — run `scripts/rtti.py` against your own dump.

### Confirming the Gate

`FLOW_CONDKICK=1` caps infinite condition waits and returns `CELL_OK`, which is
a lie but a useful one: the guest's wait loop exits on a zero return. With it,
main leaves the attract wait and runs on into the SGX audio teardown, where it
contends for the job chain's lwmutex and — this time — **wins it, twelve times,
after ~4 ms each**. So that is no longer a deadlock, just contention.

It then dies writing through a null object (`write at host 0x24`,
`LR=0x0096C890`), which is what a lie of this shape earns: the object was never
actually made ready. The experiment is only worth its one conclusion — the
attract wait is the gate, and everything downstream is reachable once something
legitimately completes that object.

### The Intermittent Crash Was Us, Writing on a Function-Pointer Table

Half the runs in this session died somewhere different -- a null-object write,
an `unresolved indirect call -> 0x43F73A02`, a read through a garbage `CTR`.
They were all the same bug, and it was in the runtime, not the game.

`cellDiscGameGetBootDiscInfo` was declared with three parameters. It takes one.
The HLE adapter therefore handed it whatever `r4` held, and it wrote a title-id
string there. At GT5P's call site `r4` still holds a global loaded two
instructions earlier for the `RegisterDiscChangeCallback` call -- `0x01025510`,
which is the middle of an OPD table:

```
OPD 0x01025510 -> func_0093C3D0   <- "BLES" / "0000" written over code+toc
OPD 0x01025518 -> func_0093C480   <- code pointer became 0x3000C480
OPD 0x01025520 -> func_0093C778
```

`"BLES00000\0"` is ten bytes: it destroyed one entry outright and truncated the
next. Any indirect call through either landed in nowhere.

Fixed in [ps3recomp#109](https://github.com/sp00nznet/ps3recomp/pull/109), and
the port now also feeds `cellGame` the real ids from PARAM.SFO, which it had
never done -- the boot had been calling itself `BLES00000` when PARAM.SFO says
`NPUA80075`.

**Six consecutive 20-second boots, zero crashes, zero unresolved indirect
calls.** Before this, roughly half of them died.

### Root Cause: the Game Is a Script, and the Script Is Not There

App init is three calls:

```
bl func_00011A28    ; activate the AdvertiseSimplePS3 object (busy = 1)
bl func_00013060    ; the pump -- this is what must drive it to completion
bl func_00011AC8    ; wait for it to finish   <- main dies here
```

`func_00013060` takes one argument, and it is a string:

```
0x00E97618: "scripts/gt5p/Application"
```

**GT5P's application flow is a script**, loaded by name out of Polyphony's packed
filesystem. Instrumented, the pump's entire life is one millisecond:

```
[pump] ENTER func_00013060(r3=0x00E97618)
[pump] gate func_0096DE30 -> 0            (does not take the early-out)
[pump] step func_007D1B60(0x2027F6D0) -> 0x2027F70C
       two small allocations
[pump] LEAVE func_00013060                (same millisecond)
```

**Correction to a first reading of this.** That probe only logged calls which
returned non-zero *and* had a non-zero `r4`, so a void call with a stale `r4` of
zero vanished from the log — which made the pump look like it bailed on the spot.
Traced unconditionally from the pre-hook it makes **45 calls**: it registers
script symbols (`setSignalHandler`, `setRemainMaxTime`, `MRaceDisplayFace`),
builds the path as a `std::string`, and ends with

```
func_00838688(hModule=0x2AA3092C, &"scripts/gt5p/Application")
```

`hModule` is the script VM's module handle — RTTI puts `hAny` and `hCode` either
side of it. So the script system is up and is genuinely asked for the application
script. It comes back with nothing, the pump destructs its two RAII guards, and
returns. (`func_00A03150`, which the first pass took for the lookup, is `strlen`
— word-at-a-time zero detection, `cntlzd`, the lot.)

It comes back with nothing because there is nothing mounted: the two functions that build the mount path
`/dev_bdvd/PS3_GAME/USRDIR` + `/` + `PDIPFS` — `func_0002D938` and
`func_0002E810` — are **never called once** in a boot, and the only file this
port has ever opened is `PARAM.SFO`.

So the chain runs the whole way down:

| | |
|---|---|
| PDIPFS is never mounted | proven — mount functions never called, no file opened |
| so `scripts/gt5p/Application` cannot be found | the pump returns in 1 ms |
| so the attract object is never driven | its busy byte stays 1 all boot |
| so `MenuGameObject::wait()` never returns | main parks on cond 93 |
| so the frame loop never starts | `put` stays at `0x10040`, no pixels |

I also guessed this would explain the heap corruption — that the arena's
measuring pass counted a container which was empty only because no data had
loaded. **That was wrong.** With PDIPFS mounted and assets loading, the two
passes still disagree by exactly as much as before: the measuring pass ends at
cursor `0x58`, the filling pass at `0x4C0`. The overrun is unchanged and is now
the main source of instability — the same boot variously segfaults, spins in the
CRT allocator on a near-null free-list pointer, or reaches the clean stall.

**The next question is the right one to ask, and it is a single question:** what
should call the PDIPFS mount, and why has that not run? Everything else in this
README is downstream of it.

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
python /path/to/ps3recomp/tools/ppu_lifter.py input/EMAIN.ELF     --functions analysis/functions_code.json --output generated     --code-end 0xBE0AF4 --header-name ppu_recomp.h --source-name ppu_recomp.c     --hle-stubs analysis/hle_stubs.json
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
