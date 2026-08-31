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
static volatile long g_in_pump;   /* set while func_00013060 runs */
static volatile long g_in_fsinit; /* set while func_00014B58 runs */

extern "C" uint32_t gt5p_alloc_pre(const char* who, uint32_t a3_,
                                   uint32_t a4, uint32_t /*a5*/)
{
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
    if (strstr(who, "0091B8A8"))
        fprintf(stderr, "[dload] ctor obj=0x%08X\n", a3_);

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
        if (n++ < 24) {
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

    static int pad = -1;
    if (pad < 0) { const char* e = getenv("GT5P_HEAPPAD"); pad = e ? atoi(e) : 0; }
    if (pad > 0 && strstr(who, "0094FF30") && a4 && a4 < 0x01000000u)
        return a4 + (uint32_t)pad;
    return a4;
}

extern "C" void gt5p_alloc_note(const char* who, uint32_t a3, uint32_t a4,
                                uint32_t a5, uint32_t ret)
{
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
