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
        static long arm_at = -1;
        if (arm_at < 0) {
            const char* e = getenv("GT5P_CANARY_ARM");
            arm_at = e ? atol(e) : 0;
        }
        if (arm_at > 0 && (long)seq == arm_at) {
            fprintf(stderr, "[canary] arming page guard on 0x%08X at call #%u\n",
                    addr, seq);
            fflush(stderr);
            ppu_guard_page(addr);
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

extern "C" void gt5p_alloc_note(const char* who, uint32_t a3, uint32_t a4,
                                uint32_t a5, uint32_t ret)
{
    int lvl = watch_level();
    canary_check(g_seq + 1, who);
    if (!lvl) return;

    /* func_0094FF30(heap, size, align) -> block. A zero return is a failed
     * allocation and owns no range. */
    uint32_t size = a4;
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
