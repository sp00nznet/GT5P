/* Allocation overlap detector.
 *
 * Two allocator structures end up on the same guest memory during this title's
 * boot: a size-class pool built during static init at 0x200063D0/0x20006458,
 * and a second one of identical shape built inside main from the 1 MB heap
 * func_009BFEF8 creates. Both are carved by the CRT heap's block allocator,
 * func_0094FF30. If it hands the same range out twice, this says so.
 *
 * scripts/instrument_alloc.py wraps the lifted function and calls
 * gt5p_alloc_note() with (r3, r4, r5) in and r3 out. Enable with
 * GT5P_ALLOCWATCH=1; GT5P_ALLOCWATCH=2 also logs every call, not just the
 * overlaps.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define MAX_BLOCKS 4096

namespace {

struct Block { uint32_t addr, size, heap, seq; };

Block    g_blocks[MAX_BLOCKS];
unsigned g_count;
unsigned g_seq;
unsigned g_reported;
#ifdef _WIN32
SRWLOCK  g_lock = SRWLOCK_INIT;
#endif

int watch_level()
{
    static int lvl = -1;
    if (lvl < 0) {
        const char* e = getenv("GT5P_ALLOCWATCH");
        lvl = e ? atoi(e) : 0;
    }
    return lvl;
}

}  // namespace

/* GT5P_CANARY=<guest addr>:<expected word>: check that word on every wrapped
 * call and report the first call after which it changed. Every other probe so
 * far has been cross-thread, where a buffered stderr makes ordering a guess;
 * this one runs on the allocating thread, so "it was still right at call #N and
 * wrong at #N+1" is exact. */
extern "C" uint32_t vm_read32(uint64_t addr);
extern "C" void vm_write32(uint64_t addr, uint32_t val);
extern "C" void ppu_guest_callstack(const char* tag);
extern "C" void ppu_guard_page(uint32_t guest_ea);

namespace {
void canary_check(unsigned seq, const char* who)
{
    static int      armed = -1;
    static uint32_t addr, want;
    static int      broken;
    if (armed < 0) {
        const char* e = getenv("GT5P_CANARY");
        armed = (e && sscanf(e, "%x:%x", &addr, &want) == 2) ? 1 : 0;
    }
    if (armed <= 0 || broken) return;

    /* GT5P_CANARY_ARM=<call#>: write-protect the canary's page at that call.
     * The canary itself only notices damage at the *next* wrapped call, on
     * whichever thread happens to make it — which is the wrong thread to ask
     * for a backtrace. The page guard catches the actual store, wherever and
     * whenever it happens. Set this to the last call at which the word was
     * still correct. */
    {
        static long     arm_at = -1;
        static uint32_t arm_addr;
        if (arm_at < 0) {
            const char* e = getenv("GT5P_CANARY_ARM");
            unsigned long a = 0; unsigned g = 0;
            int n = e ? sscanf(e, "%lu:%x", &a, &g) : 0;
            arm_at   = n >= 1 ? (long)a : 0;
            arm_addr = n >= 2 ? g : addr;   /* default: guard the canary itself */
        }
        if (arm_at > 0 && (long)seq == arm_at) {
            fprintf(stderr, "[canary] arming page guard on 0x%08X at call #%u\n",
                    arm_addr, seq);
            fflush(stderr);
            ppu_guard_page(arm_addr);
        }
    }

    uint32_t now = vm_read32(addr);
    /* Arm only once the word has actually been set: before that it is simply
     * not written yet, and reporting the pre-init state says nothing. */
    static int seen;
    if (!seen) { if (now == want) { seen = 1;
                     fprintf(stderr, "[canary] 0x%08X reached 0x%08X at call #%u (%s)\n",
                             addr, want, seq, who); fflush(stderr); }
                 return; }
    if (now == want) return;
    broken = 1;
    fprintf(stderr, "[canary] 0x%08X changed 0x%08X -> 0x%08X, first seen at "
                    "call #%u (%s)\n", addr, want, now, seq, who);
#ifdef _WIN32
    void* fr[32];
    USHORT n = RtlCaptureStackBackTrace(0, 32, fr, nullptr);
    char* mb = (char*)GetModuleHandleA(nullptr);
    fprintf(stderr, "[canary] host rva:");
    for (USHORT i = 0; i < n; i++)
        fprintf(stderr, " %llX", (unsigned long long)((char*)fr[i] - mb));
    fputc('\n', stderr);
    ppu_guest_callstack("canary");
#endif
    fflush(stderr);
}
}  // namespace

/* GT5P_HEAPPAD=<bytes>: add slack to every CRT block allocation.
 *
 * Not a fix -- a diagnosis. func_006A4400 is an unbounded bump arena run
 * twice: once with arena->base == 0 to total the sizes, then over a CRT block
 * that big to fill. Those two passes disagree here (one call measures 0 bytes
 * and then writes 0x26C), so the fill walks ~0x428 bytes off the end of a 0x58
 * block and stamps its data over the CRT's free-list nodes, which is what
 * finally wedges the boot in an operator new retry loop.
 *
 * If padding every block by more than the overrun lets the boot continue, the
 * chain is confirmed end to end and the remaining work is the count mismatch
 * alone. If it does not, something else is also wrong and this rules it in.
 */
/* The five calls func_00917208 makes, in order: the guard, device slot 26,
 * the state reset, an unnamed step, a release, and the request's slot 9 --
 * which is func_0091BAD8, the only writer of state 3 in the module. */
static uint32_t g_expand_obj;      /* PDISTD::FileExpandPSX, from [handler+12] */
static volatile long g_in_drain;   /* set once func_00917208 is entered */

static int gt5p_drain_step(const char* who)
{
    static const char* const names[] = {
        "0091B480", "00916E40", "00917158", "009169F8", "0091B4F0", "0091BAD8",
        /* the two scope helpers func_00916E40 calls before the delegate --
         * hot enough that they are only worth printing inside the window */
        "00947A68", "0095A298", "0095A0F0", "0091D468", "0091E348",
        /* the calls func_0091E37C -- the delegate's tail-called
         * continuation -- makes, one of which does not come back */
        "0091D530", "0091D5A0", "0091D620", "0091D690", "0091D720",
        "0091D7A0", "0091D828", "0091E37C", "0091D510",
        /* func_00925F60 is PFSFSHdd slot 29 -- the packed-file read. It
         * pushes the operation onto a global queue under a lock and then
         * hands off; these are every call it makes. */
        "00925F60", "00916DD0", "00938DA0", "00951CA8", "00938BE8",
        "0093BF70", "00918C48", "009200F0",
        /* the decompressor loop and its work call -- entered before
         * the window opens, so only their returns print, which is
         * exactly the question: does the worker ever come back out */
        "00920198", "00919060", "00920220",
        /* the layer between the loop and the wait */
        "00957C88", "00956D20", "00956C58", "00A0A458",
        /* func_0091FBA8 is the producer: it waits on 6896 for the
         * decoder to be ready, writes the buffer range into
         * 6932/6936 and signals 6876. It was waiting and got woken,
         * and then never fed anything. */
        "0091FBA8", "0091FF30",
        /* the decode loop's two callbacks: slot 3 is the refill
         * (func_009200F0), slot 2 the output writer */
        "00918B88", "0091FD78", "0091FCC8",
    };
    if (!g_in_drain) return 0;
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strstr(who, names[i])) return 1;
    return 0;
}

/* func_00917EB0's switch table at 0x00918004, as offsets from the table base.
 * Recovered by scripts/jumptables.py; find_functions cannot see through a
 * bctr, which is why 0x009180AC had to be seeded by hand before the boot
 * would resolve its indirect calls. */
static uint32_t gt5p_jt_00918004(uint32_t sel)
{
    static const uint32_t t[12] = {
        0x009180AC, 0x009180AC, 0x0091820C, 0x0091820C, 0x009181F0, 0x009181D4,
        0x009181B8, 0x0091819C, 0x00918180, 0x00918164, 0x00917F18, 0x00918120,
    };
    return sel < 12 ? t[sel] : 0x00917F18u;
}

static volatile long g_in_pump;   /* set while func_00013060 runs */
static volatile long g_in_fsinit; /* set while func_00014B58 runs */

static unsigned g_probe_tick;   /* orders [arena] and [enum] lines */
static int g_fl_before;   /* free-list state entering a bracketed call */

extern "C" uint32_t gt5p_alloc_pre(const char* who, uint32_t a3_,
                                   uint32_t a4, uint32_t a5_)
{
    /* Log every arena reservation, tagged by pass. func_006A4400 runs twice
     * over the same layout: a measuring pass with the arena base still 0 (so
     * it only accumulates the offset) and a filling pass with a real base. If
     * the two disagree by 0x468 bytes, the sequences differ somewhere, and the
     * only way to see where is to print both and line them up. */
    if (strstr(who, "006A4400")) {
        unsigned base = vm_read32(a3_);
        static int n = 0;
        if (1)
            fprintf(stderr, "[arena] ctx=0x%08X %s off=0x%08X size=%u\n",
                    a3_, base ? "fill   " : "measure", vm_read32(a3_ + 4), a5_);
        /* The 620-byte reservation is the one the measuring pass sizes at 0.
         * Name the caller that produces it -- that is where the two passes
         * part company. */
        if (a5_ == 620 && vm_read32(a3_)) {
            static int once = 0;
            if (!once++) ppu_guest_callstack("arena-620");
        }
    }

    /* GT5P_ARENA_OUTZERO=1 -- candidate fix.
     *
     * func_006A4400 skips writing *out when the arena base is still 0, which
     * is exactly the measuring pass. The caller then reads that slot anyway
     * and branches on it, so on this port it branches on whatever the stack
     * happened to hold while on hardware it saw something stable -- and the
     * measure and fill passes end up disagreeing by 0x468 bytes. The fill pass
     * then bumps past the region that was reserved for it and writes over
     * heap memory that is still on the free list.
     *
     * Writing 0 into the slot on that path makes the measuring pass see a
     * definite value instead of stack litter. If the two passes then agree,
     * the arena stops overrunning. */
    if (strstr(who, "006A4400")) {
        static int on = -1;
        if (on < 0) { const char* e = getenv("GT5P_ARENA_OUTZERO"); on = e ? atoi(e) : 0; }
        if (on && a4 && vm_read32(a3_) == 0)
            vm_write32(a4, 0);
    }

    /* Bracket the suspects: if the free list is intact on entry and broken on
     * exit, the function between the two checks is the one that broke it. */
    if (strstr(who, "006A4400") || strstr(who, "006C2D5C")) {
        extern int gt5p_freelist_bad(unsigned);
        g_fl_before = gt5p_freelist_bad(0x011806B0u);
    }

    /* Open the bracket-trace window as soon as the drain is entered. */
    /* func_00916DD0 calls slot 5 of the object at handler+12 and then
     * func_00918C48, and it clears that field on the way through -- so read it
     * here, in the pre-hook, while it is still set. */
    /* func_009200F0 is the other end of the FileExpandPSX handshake: it waits
     * on obj+6876, which func_0091FEB8 signals, and it runs on the
     * decompressor thread. If it is never entered, no such thread exists and
     * the signal has nobody to wake -- which is why the matching wait on
     * obj+6816 never returns. func_0091EFD0 is the class's setup. */
    if (strstr(who, "009200F0") || strstr(who, "0091EFD0")) {
        static int n = 0;
        if (strstr(who, "009200F0")) g_expand_obj = a3_;
        if (n++ < 8) {
            ppu_guest_callstack("expand-peer");
            fprintf(stderr, "[expand-peer] %s(0x%08X)\n", who, a3_);
            fflush(stderr);
        }
    }

    /* func_00956D20's window copy starts its cursor at r25 + [obj+42] but
     * bounds it at r25 + ([obj+40] & 0x7FFF), and wraps at r25 + 0x8000.
     * [obj+42] is a bare lhz, so a value >= 0x8000 puts the cursor outside
     * the 32 KB window: it never reaches the wrap and never equals the
     * bound, which is exactly the 50M-iteration spin. Print both. */
    if (strstr(who, "00956D20")) {
        uint32_t w40 = (vm_read32(a3_ + 40) >> 16) & 0xFFFF;
        uint32_t w42 = vm_read32(a3_ + 40) & 0xFFFF;
        fprintf(stderr, "[win] obj=0x%08X [+40]=0x%04X [+42]=0x%04X%s\n",
                a3_, w40, w42, w42 >= 0x8000 ? "   <-- OUTSIDE THE WINDOW" : "");
        fflush(stderr);
    }

    if (strstr(who, "00916DD0")) {
        uint32_t obj = vm_read32(a4 + 12);
        g_expand_obj = obj;
        uint32_t vt = obj ? vm_read32(obj) : 0;
        uint32_t opd = vt ? vm_read32(vt + 20) : 0;
        fprintf(stderr, "[slot5] obj=0x%08X vt=0x%08X -> func_%08X\n",
                obj, vt, opd ? vm_read32(opd) : 0);
        fflush(stderr);
    }

    /* PDISTD::FileExpandPSX handshake. func_0091FEB8 signals the event at
     * obj+6876 to wake the decompressor and then waits on obj+6816 for it to
     * answer -- and that wait never returns. Print every wait/signal that
     * lands inside this object so it is clear whether a decompressor thread
     * exists on the other side at all. */
    if (g_expand_obj && (strstr(who, "00954200") || strstr(who, "009542E0"))) {
        uint32_t off = a3_ - g_expand_obj;
        if (off < 8192) {
            static int n = 0;
            if (n++ < 300) {
                uint32_t st = vm_read32(a3_ + 16);
                fprintf(stderr, "[expand] %s obj+%u  waiter=%u signalled=%u\n",
                        strstr(who, "009542E0") ? "signal" : "wait  ", off,
                        (st >> 24) & 0xFF, (st >> 16) & 0xFF);
                fflush(stderr);
            }
        }
    }

    if (strstr(who, "00917208")) g_in_drain = 1;

    /* func_00687F90 is "task->start(); task->wait();" through vtable slots
     * +0x24 and +0x6C. Main reaches the wait and never leaves it -- nothing is
     * ever posted to the PDI worker queue and no condvar is signalled all boot
     * -- so name both methods; the start one is where the enqueue should be. */
    if (strstr(who, "00687F90")) {
        static int n = 0;
        if (n++ < 4) {
            uint32_t vt = vm_read32(a3_);
            fprintf(stderr, "[task] func_00687F90(obj=0x%08X) vtable=0x%08X "
                            "start=func_%08X wait=func_%08X\n",
                    a3_, vt, vm_read32(vm_read32(vt + 0x24)),
                    vm_read32(vm_read32(vt + 0x6C)));
            fflush(stderr);
        }
    }

    /* GameObjectBase::vtable+0x1C -- activate. It sets the busy byte that
     * MenuGameObject::wait() then blocks on for the rest of the boot, so the
     * question is what context calls it: if activate and wait are the same
     * caller, the object is meant to be ticked by a manager somebody else
     * drives; if they are different, main is waiting on work it started
     * earlier and the tick is its own frame loop. */
    if (strstr(who, "0068DA68")) {
        static int n = 0;
        if (n++ < 3) {
            fprintf(stderr, "[activate] GameObjectBase+0x1C obj=0x%08X busy=%u\n",
                    a3_, vm_read32(a3_ + 0x44) >> 24);
            ppu_guest_callstack("activate");
            fflush(stderr);
        }
    }

    /* func_00013060 is the attract pump: app init calls it between "activate
     * the AdvertiseSimplePS3 object" and "wait for it to finish", so it is what
     * has to drive the object to completion. It returns, and the object is
     * still busy -- so the question is whether it loops at all. */
    /* func_00014B58 is the filesystem setup: it reads settings through
     * func_00013C98, compares them with strcmp, and four branches decide which
     * file devices get created. strcmp is called constantly elsewhere, so only
     * look while this function is on the stack. */
    /* PDIEXT::FileDelayLoad -- vtable+0x10 waits for the load to reach state 3,
     * vtable+0x24 is its completion side. Main parks in the wait and the PDI
     * workers do signal its condvar, so it wakes, re-checks and sleeps again:
     * the load is not finishing. Print the state each time through. */
    /* func_0091B638 is the one state transition that actually runs (66 times a
     * boot). Print which object it advances and from what -- main waits on ONE
     * FileDelayLoad, and the question is whether that one is ever among them. */
    /* func_00917EB0 is the device worker step -- vtable slot 16, shared by
     * both file devices. It switches on [req+0x64] through a computed jump
     * table, and reaches the completion call at 0x00917F90 (virtual slot 9 ==
     * func_0091BAD8, the ONLY writer of state 3 in this module) only when r24
     * is non-zero -- which the disassembly sets solely on the switch's default
     * path. So log the fields the branches actually read. Dedup on the tuple:
     * a stalled request steps every tick and would otherwise flood. */
    /* func_00916F48 is device vtable slot 14 -- the worker "run" loop, shared
     * by both file devices. The async path (sel=0) hands the request to the
     * dev+64 priority list and returns; only this loop can pick it back up. If
     * it never runs for a given device, every async request on that device
     * parks forever, while synchronous ones (sel=2,3,4) still complete because
     * they never touch the queue. Count calls per device to tell those apart. */
    /* func_00917208 is device slot 25 -- the drain. The worker reaches it only
     * when dev+68 (the count of the dev+64 priority list) is non-zero, and it
     * is what moves a queued request into dev+56 so the step can run it. Both
     * devices share this implementation, so if it never runs for PFSFSHdd the
     * worker is not waking, not missing code. */
    /* The drain (func_00917208) is entered once for the stalled request and
     * never returns, so the hang is in one of the five calls it makes. Bracket
     * each: an unmatched ">" names the one that does not come back. */
    if (gt5p_drain_step(who)) {
        fprintf(stderr, "  > %s(0x%08X, 0x%08X)\n", who, a3_, a4);
        fflush(stderr);
    }

    /* func_0091D468 is where the drain finally hangs: it calls a delegate
     * whose function pointer lives at handler+24 with the context at
     * handler+28. Nothing static names that target -- it is stored at
     * registration time -- so read it at the call and resolve the OPD. */
    if (strstr(who, "0091D468")) {
        uint32_t opd = vm_read32(a3_ + 24);
        fprintf(stderr, "[cb] handler=0x%08X req=0x%08X fn_opd=0x%08X -> func_%08X ctx=0x%08X\n",
                a3_, a4, opd, opd ? vm_read32(opd) : 0, vm_read32(a3_ + 28));
        fflush(stderr);
    }

    if (strstr(who, "00917208")) {
        static unsigned n[8]; static uint32_t d[8]; static int nd;
        int i = 0;
        for (; i < nd; i++) if (d[i] == a3_) break;
        if (i == nd && nd < 8) { d[nd] = a3_; nd++; }
        if (i < 8 && ++n[i] <= 20)
            fprintf(stderr, "[drain] dev=0x%08X call#%u cur=0x%08X count=%u\n",
                    a3_, n[i], vm_read32(a3_ + 56), vm_read32(a3_ + 68));
    }

    /* func_009542E0(dev+76) is the wake the promoter sends; func_00954200 is
     * the worker's matching wait. Print both against the device so a lost
     * wakeup -- signalled before the worker reached the wait -- is visible as
     * a signal with no wake after it. */
    if (strstr(who, "009542E0") || strstr(who, "00954200")) {
        uint32_t dev = a3_ - 76;
        if (dev == 0x20094290 || dev == 0x20095E80) {
            static int n = 0;
            if (n++ < 400) {
                fprintf(stderr, "[sync] %s dev=0x%08X cur=0x%08X count=%u run=%u\n",
                        strstr(who, "009542E0") ? "signal" : "wait  ", dev,
                        vm_read32(dev + 56), vm_read32(dev + 68),
                        (vm_read32(dev + 108) >> 24) & 0xFF);
                fflush(stderr);
            }
        }
    }

    if (strstr(who, "00916F48")) {
        static uint32_t devs[8]; static unsigned hits[8]; static int nd;
        int i = 0;
        for (; i < nd; i++) if (devs[i] == a3_) break;
        if (i == nd && nd < 8) { devs[nd] = a3_; hits[nd] = 0; nd++; }
        if (i < 8 && ++hits[i] <= 3)
            fprintf(stderr, "[run] worker dev=0x%08X call#%u\n", a3_, hits[i]);
        if (i < 8 && hits[i] == 1000)
            fprintf(stderr, "[run] worker dev=0x%08X reached 1000 calls\n", a3_);
    }

    /* func_00926708 is PFSFSHdd's slot 17 -- the async open the stalled
     * request goes through. func_00920EA8 is GameData's, and that one's async
     * request does complete, so print both to see if the PFS side even
     * succeeds: a false return would take the error path and still finish. */
    if (strstr(who, "00926708") || strstr(who, "00920EA8")) {
        static int n = 0;
        if (n++ < 6) {
            fprintf(stderr, "[open17] %s dev=0x%08X req=0x%08X\n", who, a3_, a4);
            fflush(stderr);
        }
    }

    if (strstr(who, "00917EB0")) {
        uint32_t sw    = vm_read32(a4 + 0x64);   /* jump-table selector */
        uint32_t state = vm_read32(a4 + 0x8C);   /* 0->1->0->2, wants 3 */
        uint32_t c144  = vm_read32(a4 + 0x90);
        uint32_t d1    = (vm_read32(a4 + 0xD0) >> 16) & 0xFF;   /* byte +0xD1 */
        static uint32_t seen[64][2];
        static int nseen = 0;
        uint32_t key = (sw << 8) | (state & 0xFF);
        int dup = 0;
        for (int i = 0; i < nseen; i++)
            if (seen[i][0] == a4 && seen[i][1] == key) { dup = 1; break; }
        if (!dup) {
            if (nseen < 64) { seen[nseen][0] = a4; seen[nseen][1] = key; nseen++; }
            fprintf(stderr, "[step] dev=0x%08X req=0x%08X vt=0x%08X sel=%u state=%u n=%u b=%u -> case 0x%08X\n",
                    a3_, a4, vm_read32(a4), sw, state, c144, d1, gt5p_jt_00918004(sw));
            fflush(stderr);
        }
    }

    if (strstr(who, "0091B8A8"))
        fprintf(stderr, "[dload] ctor obj=0x%08X\n", a3_);

    /* FileDelayLoad vtable+0x08 is the SUBMIT, called by
     * func_0091B298 immediately before the wait. Five loads complete
     * and the sixth does not, so the question is whether the sixth is
     * even submitted. */
    if (strstr(who, "006A3D70")) {
        static int n = 0;
        if (n++ < 24)
            fprintf(stderr, "[count] func_006A3D70(0x%08X)\n", a3_);
    }

    /* func_00917380 is the worker step that ends in a completion.
     * Print which item it takes, to compare against the request that
     * never completes. */
    /* func_0091B480(request) is the predicate the completion
     * dispatcher branches on: zero sends it down a different path and
     * the request is not completed. Five requests complete and the
     * sixth does not, so print it per call. */
    if (strstr(who, "0091B480")) {
        static int n = 0;
        if (n++ < 30)
            /* Print more than the one field. If only +0x64 is wrong the write
             * was targeted; if the whole object reads zero the worker is
             * looking at different memory entirely. */
            fprintf(stderr, "[worker] pred req=0x%08X f64=%u f7C=%u f8C=%u "
                            "vt=0x%08X f20=0x%08X\n",
                    a3_, vm_read32(a3_ + 0x64), vm_read32(a3_ + 0x7C),
                    vm_read32(a3_ + 0x8C), vm_read32(a3_),
                    vm_read32(a3_ + 0x20));
    }

    if (strstr(who, "00917380")) {
        static int n = 0;
        if (n++ < 30)
            fprintf(stderr, "[worker] step obj=0x%08X arg=0x%08X\n", a3_, a4);
    }

    if (strstr(who, "0091B780"))
        /* The submit is: lock; if (state) bail; if (!obj->device) { obj->err =
         * 4; error path } else enqueue(device, obj). So the device pointer at
         * +0x20 decides whether the request is queued at all -- print it. */
        {
            /* The worker loop (func_00916F48) checks a stop byte at dev+0x6C
             * before waiting and exits if it is set. Some PDI threads read as
             * FINISHED by the time the sixth request lands, so the question is
             * whether the pool has already shut itself down. */
            uint32_t dev = vm_read32(a3_ + 0x20);
            /* The wakeup object is dev+0x4C. Its signal is guarded:
             *   if (!obj[0x10]) obj[0x11] = 1;   // no waiter: record pending
             *   else            cond_signal(obj);
             * so "waiter" and "pending" together say whether the wakeup was
             * delivered, banked, or dropped. */
            uint32_t sync = dev + 0x4C;
            /* Name the device class and its worker methods. The request is
             * promoted into a second list at dev+0x40 and parks there, so the
             * consumer of THAT list is what matters now. */
            {
                static int _dv = 0;
                if (_dv++ < 2 && dev) {
                    uint32_t dvt = vm_read32(dev);
                    fprintf(stderr, "[dev] dev=0x%08X vtable=0x%08X "
                                    "run=func_%08X step=func_%08X "
                                    "list28=0x%08X list40=0x%08X\n",
                            dev, dvt,
                            vm_read32(vm_read32(dvt + 0x38)),
                            vm_read32(vm_read32(dvt + 0x3C)),
                            vm_read32(dev + 0x28), vm_read32(dev + 0x40));
                }
            }
            fprintf(stderr, "[dload] submit obj=0x%08X state=%u device=0x%08X "
                            "stop=%u waiter=%u pending=%u\n",
                    a3_, vm_read32(a3_ + 0x8C), dev,
                    dev ? (vm_read32(dev + 0x6C) >> 24) : 0u,
                    dev ? ((vm_read32(sync + 0x10) >> 24) & 0xFF) : 0u,
                    dev ? ((vm_read32(sync + 0x10) >> 16) & 0xFF) : 0u);
        }

    if (strstr(who, "0091B638")) {
        static int n = 0;
        if (n++ < 12)
            fprintf(stderr, "[dload] step obj=0x%08X state=%u\n",
                    a3_, vm_read32(a3_ + 0x8C));
    }

    if (strstr(who, "0091BBA8") || strstr(who, "0091BAD8")) {
        static int n = 0;
        if (n++ < 20)
            fprintf(stderr, "[dload] %s obj=0x%08X state=%u err=0x%08X\n",
                    strstr(who, "0091BBA8") ? "wait" : "complete",
                    a3_, vm_read32(a3_ + 0x8C), vm_read32(a3_ + 0xD4));
            /* The ctor copies the request path into the object at +0x90
             * (stdu r0, 0x90(r27) then three more doublewords). Five loads
             * complete and the sixth hangs, so the useful question is which
             * file the sixth one is. */
            char pth[80]; unsigned k = 0;
            for (; k < sizeof pth - 1; k++) {
                uint32_t w = vm_read32((a3_ + 0x90 + k) & ~3u);
                char c = (char)((w >> (8 * (3 - ((a3_ + 0x90 + k) & 3)))) & 0xFF);
                if (!c) break;
                pth[k] = (c >= 32 && c < 127) ? c : '.';
            }
            pth[k] = 0;
            /* +0x90 is a std::string header, not inline characters -- +0x9C
             * holds the data pointer. Follow it, and dump a window of the
             * object either way so a wrong guess is visible rather than silent
             * (the first guess printed an empty string and looked like "no
             * path", which is a different and much more misleading answer). */
            uint32_t sp = vm_read32(a3_ + 0x9C);
            char ind[96]; unsigned m = 0;
            if (sp >= 0x10000u) {
                for (; m < sizeof ind - 1; m++) {
                    uint32_t w = vm_read32((sp + m) & ~3u);
                    char c = (char)((w >> (8 * (3 - ((sp + m) & 3)))) & 0xFF);
                    if (!c) break;
                    ind[m] = (c >= 32 && c < 127) ? c : '.';
                }
            }
            ind[m] = 0;
            fprintf(stderr, "[dload]   inline='%s' ptr=0x%08X -> '%s'\n",
                    pth, sp, ind);
            /* Two guesses at the request's path field were both wrong (+0x90
             * inline, +0x9C as a pointer). Stop guessing at the format and ask
             * who is making the request instead -- the caller names the
             * subsystem, which is what actually matters. */
            /* rtti.py's slot mapping put submit at vtable+0x08 = func_0091AE98,
             * but that function is never called -- so the mapping is wrong
             * there (thunks and multiple inheritance shift the walk). Read the
             * slot's target from the live vtable instead of trusting it. */
            if (strstr(who, "0091BBA8")) {
                uint32_t vt = vm_read32(a3_);
                fprintf(stderr, "[dload]   vtable=0x%08X submit=func_%08X "
                                "wait=func_%08X\n", vt,
                        vm_read32(vm_read32(vt + 0x08)),
                        vm_read32(vm_read32(vt + 0x10)));
            }
            ppu_guest_callstack(strstr(who, "0091BBA8") ? "dload-wait"
                                                       : "dload-complete");
            fflush(stderr);
    }

    if (strstr(who, "00014B58")) g_in_fsinit = 1;

    if (strstr(who, "00013060")) {
        g_in_pump = 1;
        fprintf(stderr, "[pump] ENTER func_00013060(r3=0x%08X) t=%llu ms\n",
                a3_, (unsigned long long)GetTickCount64());
        fflush(stderr);
    }

    /* func_008AF6C8(sink=0x010E3760, str) is the engine's log sink -- the
     * attract pump calls it eight times on the way out. Print what it is being
     * handed: the game's own account of why it gave up is worth more than any
     * amount of further disassembly. */
    /* func_009FF5B0(dst, src) is strcpy. The script-module resolve composes its
     * result through a chain of these into short-lived heap strings, which are
     * already freed by the time anything can peek at them -- so read the source
     * live, at the call. */
    if (strstr(who, "009FF5B0") && a4 && g_in_pump) {
        static int n = 0;
        if (n++ < 200) {
            char buf[128]; unsigned i = 0;
            for (; i < sizeof buf - 1; i++) {
                uint32_t w = vm_read32((a4 + i) & ~3u);
                char c = (char)((w >> (8 * (3 - ((a4 + i) & 3)))) & 0xFF);
                if (!c) break;
                buf[i] = (c >= 32 && c < 127) ? c : '.';
            }
            buf[i] = 0;
            if (i) fprintf(stderr, "[str] 0x%08X -> 0x%08X \"%s\"\n", a4, a3_, buf);
        }
    }

    /* func_007D5268(volume, path) is the resolve inside the script module
     * loader. Print the object's vtable so scripts/rtti.py can name the class:
     * knowing WHICH resource system is being asked, and what state it is in,
     * is the difference between "no volume mounted" and "mounted but empty". */
    /* The filesystem setup (func_00014B58) is configuration-driven:
     * func_00013C98 reads a setting, func_00A02BA8 compares it, and four
     * branches decide which file devices get created. Print the strings. */
    if (g_in_fsinit && (strstr(who, "00013C98") || strstr(who, "00A02BA8"))) {
        static int n = 0;
        if (n++ < 30) {
            char b3[96], b4[96];
            for (int k = 0; k < 2; k++) {
                uint32_t p = k ? a4 : a3_;
                char* o = k ? b4 : b3; unsigned i2 = 0;
                if (p >= 0x10000u) {
                    for (; i2 < 90; i2++) {
                        uint32_t w = vm_read32((p + i2) & ~3u);
                        char c = (char)((w >> (8 * (3 - ((p + i2) & 3)))) & 0xFF);
                        if (!c) break;
                        o[i2] = (c >= 32 && c < 127) ? c : (char)46;
                    }
                }
                o[i2] = 0;
            }
            fprintf(stderr, "[cfg] %s(0x%08X '%s', 0x%08X '%s')\n",
                    who, a3_, b3, a4, b4);
        }
    }

    if (strstr(who, "007D5268")) {
        static int n = 0;
        if (n++ < 3)
            fprintf(stderr, "[resolve] func_007D5268(obj=0x%08X vtable=0x%08X) "
                            "fields: %08X %08X %08X %08X\n",
                    a3_, vm_read32(a3_), vm_read32(a3_ + 4), vm_read32(a3_ + 8),
                    vm_read32(a3_ + 12), vm_read32(a3_ + 16));
    }

    if (strstr(who, "008AF6C8") && a4) {
        static int n = 0;
        if (n++ < 400) {
            char buf[160]; unsigned i = 0;
            for (; i < sizeof buf - 1; i++) {
                uint32_t w = vm_read32((a4 + i) & ~3u);
                char c = (char)((w >> (8 * (3 - ((a4 + i) & 3)))) & 0xFF);
                if (!c) break;
                buf[i] = (c >= 32 && c < 127) ? c : '.';
            }
            buf[i] = 0;
            if (i) fprintf(stderr, "[log] 0x%08X \"%s\"\n", a4, buf);
        }
    }

    /* GT5P_ALLOCWATCH=3: trace every wrapped call, from the PRE hook.
     *
     * Level 2 logs from gt5p_alloc_note, which returns early unless the call
     * both returned non-zero and had a non-zero r4 -- so a void function whose
     * leftover r4 happens to be 0 logs nothing, and reads as "never called".
     * That cost a wrong conclusion about func_00016130 being skipped when the
     * call site is plainly unconditional. The pre-hook has no such filter and
     * fires before the callee, so it also survives a call that never returns. */
    if (watch_level() >= 3)
        fprintf(stderr, "[call] %s(r3=0x%08X r4=0x%08X)\n", who, a3_, a4);

    /* GT5P_ARENA_ZERO=1 -- experiment.
     *
     * func_006C32A0 calls the arena for a 0x44 header, then reads the out
     * pointer back from that same stack slot and BRANCHES on it. In the
     * measuring pass (arena->base == 0) the arena deliberately does not write
     * *out, so the caller is branching on whatever the stack happened to hold.
     * On hardware that reads zero; here the guest stack is dirty, the two
     * passes take different branches, and they disagree by 0x468 bytes --
     * which the game itself notices (cmpw r3, r30 at 0x006C37CC) and treats as
     * an error. Writing 0 through on the measuring pass makes the slot
     * deterministic; if the two passes then agree, this is the cause. */
    if (strstr(who, "006A4400")) {
        static int z = -1;
        if (z < 0) { const char* e = getenv("GT5P_ARENA_ZERO"); z = e ? atoi(e) : 0; }
        /* Narrow it to the one call whose out-pointer is read back: the
         * 0x44 header at 0x006C3304. Zeroing every measuring-pass out
         * blanks slots the caller legitimately uses. z=2 = header only. */
        uint32_t sz = a5_;
        if (z && a4 >= 0x10000u && vm_read32(a3_) == 0
            && (z == 1 || sz == 0x44))
            vm_write32(a4, 0);
    }

    static int pad = -1;
    if (pad < 0) { const char* e = getenv("GT5P_HEAPPAD"); pad = e ? atoi(e) : 0; }
    if (pad > 0 && strstr(who, "0094FF30") && a4 && a4 < 0x01000000u)
        return a4 + (uint32_t)pad;
    return a4;
}

extern "C" void gt5p_alloc_note(const char* who, uint32_t a3, uint32_t a4,
                                uint32_t a5, uint32_t ret)
{
    /* func_006C5B60 is the descriptor lookup: it calls five helpers in
     * sequence and the scratch buffer either ends up with text or does not.
     * Log each so an empty call and a populated one can be compared -- the
     * first helper a populated call reaches that an empty one does not is
     * where the two part company. Tagged with the shared tick so the [enum]
     * counts interleave. */
    if (strstr(who, "006CD570") || strstr(who, "006CF50C") ||
        strstr(who, "006CF6A4") || strstr(who, "006CF57C") ||
        strstr(who, "006CEC74") || strstr(who, "006C5B60")) {
        static int n = 0;
        if (n++ < 60)
            fprintf(stderr, "[look] #%u %s(0x%08X, 0x%08X) -> 0x%08X\n",
                    ++g_probe_tick, who, a3, a4, ret);
    }

    /* The enumerator behind the whole chain. func_006A3D70 walks a collection
     * and returns its length; the arena is sized at length*20. It answers 0
     * when the layout is measured and 31 when it is filled, so the question is
     * what it is walking and why that is empty the first time round. Log the
     * object it is handed and what it returns, plus the first few next-entry
     * calls, so the two passes can be compared directly. */
    if (strstr(who, "006A3D70")) {
        static int n = 0;
        if (n++ < 40)
            { char txt[80]; int k=0;
              for (; k < 72; k++) { uint32_t w = vm_read32((a3 + k) & ~3u);
                  unsigned char c = (unsigned char)((w >> (8*(3-((a3+k)&3)))) & 0xFF);
                  if (!c) break; txt[k] = (c>=32 && c<127) ? (char)c : '.'; }
              txt[k]=0;
              fprintf(stderr, "[enum] #%u func_006A3D70(obj=0x%08X) -> count=%u  str=\"%s\"\n",
                      ++g_probe_tick, a3, ret, txt); }
    }
    if (strstr(who, "006A3D14")) {
        static int n = 0;
        if (n++ < 14)
            fprintf(stderr, "[enum]   next(obj=0x%08X buf=0x%08X) -> 0x%08X  [obj+0]=0x%08X\n",
                    a3, a4, ret, vm_read32(a3));
    }

    if (strstr(who, "006A4400") || strstr(who, "006C2D5C")) {
        extern int gt5p_freelist_bad(unsigned);
        int after = gt5p_freelist_bad(0x011806B0u);
        static int n = 0;
        if (after != g_fl_before && n++ < 8)
            fprintf(stderr, "[flcheck] %s(ctx=0x%08X out=0x%08X size=%u) bad-chains %d -> %d  ctx:[base=0x%08X off=0x%08X]\n",
                    who, a3, a4, a5, g_fl_before, after,
                    vm_read32(a3), vm_read32(a3 + 4));
    }

    /* func_0094FF30 is the game's allocator, (heap, size, align) -> block.
     *
     * The boot does not stall on I/O. Main parks in func_009F3FF0 retrying a
     * 264-byte allocation forever, and it fails because ONE request for
     * 0x9A934380 bytes -- 2.6 GB, against a 182 MB arena -- is granted, which
     * hands back a pointer outside the arena and wrecks the bookkeeping.
     *
     * Print every request over a megabyte, flag any result outside the arena,
     * and dump the guest call stack for an obviously garbage size so the caller
     * is named rather than guessed at. */
    if (strstr(who, "0094FF30") && ret) {
        { extern void gt5p_alloc_record(unsigned, unsigned);
          gt5p_alloc_record(ret, a4); }
        static int big = 0;
        if (a4 > (1u << 20) && big++ < 14) {
            int outside = (ret < 0x20000000u || ret >= 0x2ADFFF80u);
            fprintf(stderr, "[big] heap=0x%08X size=%u (0x%08X) align=%u -> 0x%08X%s\n",
                    a3, a4, a4, a5, ret, outside ? "   <-- OUTSIDE ARENA" : "");
            fflush(stderr);
        }
        if (a4 > (256u << 20)) {
            static int once = 0;
            if (!once++) {
                fprintf(stderr, "[giant] %u bytes (0x%08X) granted at 0x%08X -- caller:\n",
                        a4, a4, ret);
                ppu_guest_callstack("giant-alloc");
                fflush(stderr);
            }
        }
    }

    /* func_0091D7A0 calls func_0091D510 and then slot 29 (vtable+116) of
     * whatever it returns. Nothing static names that target, so resolve it
     * from the returned object the moment the call comes back -- one rebuild
     * instead of another round of descend-and-guess. */
    if (strstr(who, "0091D510") && ret) {
        uint32_t vt = vm_read32(ret);
        uint32_t opd = vt ? vm_read32(vt + 116) : 0;
        fprintf(stderr, "[slot29] obj=0x%08X vt=0x%08X -> func_%08X\n",
                ret, vt, opd ? vm_read32(opd) : 0);
        fflush(stderr);
    }

    if (gt5p_drain_step(who)) {
        fprintf(stderr, "  < %s ret=0x%08X\n", who, ret);
        fflush(stderr);
    }

    /* Read [obj+0x64] the instant the constructor returns. It sets that
     * field to 4 unconditionally, and the worker later reads 0 with no
     * store in between that the watch can see -- so establish whether it
     * is ever actually 4. */
    if (strstr(who, "0091B8A8"))
        fprintf(stderr, "[dload] ctor-done obj=0x%08X f64=%u f7C=%u\n",
                a3, vm_read32(a3 + 0x64), vm_read32(a3 + 0x7C));

    /* The pump's first act is: if (func_0096DE30()) return; -- so this
     * one byte decides whether attract mode runs at all. */
    if (strstr(who, "0096DE30"))
        fprintf(stderr, "[pump] gate func_0096DE30 -> %u (non-zero SKIPS attract)\n",
                ret & 0xFF);

    if (strstr(who, "007D1B60")) {
        static int n = 0;
        if (n++ < 6)
            fprintf(stderr, "[pump] step func_007D1B60(0x%08X) -> 0x%08X  [ret+0]=0x%08X\n",
                    a3, ret, ret ? vm_read32(ret) : 0);
    }

    /* func_00013C98(name, buf, len) -> found? Print the result AND the buffer
     * it filled: "found" alone does not say whether the value that came back is
     * the one the filesystem setup then compares against. */
    if (strstr(who, "00013C98")) {
        static int n = 0;
        if (n++ < 8) {
            char nm[64], val[64]; unsigned i;
            for (i = 0; i < 60; i++) {
                uint32_t w = vm_read32((a3 + i) & ~3u);
                char c = (char)((w >> (8 * (3 - ((a3 + i) & 3)))) & 0xFF);
                if (!c) break;
                nm[i] = (c >= 32 && c < 127) ? c : '.';
            }
            nm[i] = 0;
            for (i = 0; i < 60; i++) {
                uint32_t w = vm_read32((a4 + i) & ~3u);
                char c = (char)((w >> (8 * (3 - ((a4 + i) & 3)))) & 0xFF);
                if (!c) break;
                val[i] = (c >= 32 && c < 127) ? c : '.';
            }
            val[i] = 0;
            fprintf(stderr, "[cfg] get '%s' -> found=%u value='%s'\n",
                    nm, ret & 0xFF, val);
            fflush(stderr);
        }
    }

    if (strstr(who, "00014B58")) g_in_fsinit = 0;

    if (strstr(who, "00013060")) {
        g_in_pump = 0;
        fprintf(stderr, "[pump] LEAVE func_00013060 -> 0x%08X t=%llu ms\n",
                ret, (unsigned long long)GetTickCount64());
        fflush(stderr);
    }

    int lvl = watch_level();
    canary_check(g_seq + 1, who);
    if (!lvl) return;

    /* func_0094FF30(heap, size, align) -> block. A zero return is a failed
     * allocation and owns no range -- so it is not tracked for overlap, but at
     * level 2 it is still the most interesting line in the log: this title's
     * operator new retries a failed allocation forever (alloc, usleep(2000),
     * alloc, ...), and dropping the zero returns hid the whole loop. */
    /* The failure that matters is upstream of the zero return: two 5 MB
     * requests come back as 0x426E8A70 / 0x421E8A60 -- 1.1 GB in, where this
     * title has no memory at all -- and the next request is for 0x9AD29180
     * bytes. Read as floats those are 59.6353, 39.6353 and a denormal: the
     * block headers are carrying somebody's vertex data, not link pointers.
     * Catch the first handful of impossible returns/sizes with a guest stack,
     * which names the caller walking the corrupted list. The bound is this
     * title's map: 174 MB at 0x20000000, plus the ELF low. */
    /* func_006A4400 is an unbounded bump arena:
     *     r9 = arena->base; if (r9 && out) { *out = r9 + arena->cursor;
     *                                        memset(*out, 0, size); }
     *     arena->cursor += size;
     * It is run twice -- pass 1 with arena->base == 0 to total the sizes, then
     * the caller takes one CRT block that big and reruns it to fill. There is
     * no limit check anywhere, so if the two passes disagree by even one entry
     * the second walks straight off the end of the block and into the CRT's
     * free-list nodes. Printing base and cursor at every call shows the exact
     * call where base + cursor passes the block it was given. */
    if (strstr(who, "006A4400")) {
        fprintf(stderr, "[arena] %s(arena=0x%08X out=0x%08X size=0x%X) "
                        "base=0x%08X cursor=0x%X -> 0x%08X\n",
                who, a3, a4, a5, vm_read32(a3), vm_read32(a3 + 4), ret);
        /* The overrun is one call: 0 bytes on the measuring pass, 0x26C on the
         * filling pass. Name its caller -- the size getters reached from
         * func_006C2D5C all return 0, so it is a different site. */
        if (a5 >= 0x100) {
            static int n = 0;
            if (n++ < 3) ppu_guest_callstack("arena-big");
        }
        fflush(stderr);
    }

    const bool returns_ptr = strstr(who, "0094FF30") || strstr(who, "00937EF0");
    if (returns_ptr && ret && (ret < 0x00010000u || ret >= 0x2AE00000u)) {
        static int n = 0;
        if (n++ < 4) {
            fprintf(stderr, "[alloc] IMPOSSIBLE return %s(a3=0x%08X a4=0x%08X "
                            "a5=0x%08X) -> 0x%08X (%.4f as float)\n",
                    who, a3, a4, a5, ret, (double)*(const float*)&ret);
            ppu_guest_callstack("alloc-impossible");
            fflush(stderr);
        }
    }

    uint32_t size = a4;
    if (lvl >= 2 && !ret)
        fprintf(stderr, "[alloc] FAIL %s(a3=0x%08X a4=0x%08X a5=0x%08X) -> 0\n",
                who, a3, a4, a5);
    if (!ret || !size) return;

#ifdef _WIN32
    AcquireSRWLockExclusive(&g_lock);
#endif
    unsigned seq = ++g_seq;
    if (lvl >= 2)
        fprintf(stderr, "[alloc] #%u %s(heap=0x%08X size=0x%X align=%u) -> 0x%08X\n",
                seq, who, a3, size, a5, ret);

    for (unsigned i = 0; i < g_count; i++) {
        const Block& b = g_blocks[i];
        if (ret < b.addr + b.size && b.addr < ret + size) {
            if (g_reported++ < 16) {
                fprintf(stderr,
                        "[alloc] OVERLAP: #%u 0x%08X..0x%08X (size 0x%X, heap 0x%08X)\n"
                        "[alloc]     over #%u 0x%08X..0x%08X (size 0x%X, heap 0x%08X)\n",
                        seq, ret, ret + size, size, a3,
                        b.seq, b.addr, b.addr + b.size, b.size, b.heap);
                fflush(stderr);
            }
        }
    }
    if (g_count < MAX_BLOCKS) {
        g_blocks[g_count].addr = ret;
        g_blocks[g_count].size = size;
        g_blocks[g_count].heap = a3;
        g_blocks[g_count].seq  = seq;
        g_count++;
    }
#ifdef _WIN32
    ReleaseSRWLockExclusive(&g_lock);
#endif
}
