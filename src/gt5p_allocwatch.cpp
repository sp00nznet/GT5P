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

extern "C" void gt5p_alloc_note(const char* who, uint32_t a3, uint32_t a4,
                                uint32_t a5, uint32_t ret)
{
    int lvl = watch_level();
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
