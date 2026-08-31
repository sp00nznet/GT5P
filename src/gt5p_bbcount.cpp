/* Basic-block hit counter for one lifted function -- see scripts/bbcount.py.
 *
 * func_00956D20, the LZ decoder, spins with no calls in its loop once the
 * abort protocol hands it a zero-length input buffer. Entry/return brackets
 * cannot see inside a call that never returns, so count blocks instead and
 * dump the hottest ones once the total says we are clearly looping. */
#include <stdio.h>
#include <stdint.h>

#define GT5P_BB_MAX 512

static unsigned long long g_bb[GT5P_BB_MAX];
static unsigned           g_bb_addr[GT5P_BB_MAX];
static unsigned long long g_bb_total;
static int                g_bb_dumped;

extern "C" void gt5p_bb_hit(unsigned idx, unsigned addr)
{
    if (idx >= GT5P_BB_MAX) return;
    g_bb[idx]++;
    g_bb_addr[idx] = addr;

    /* 200M block entries is far past any legitimate decode of a 1 MB stream. */
    if (++g_bb_total == 200000000ull && !g_bb_dumped) {
        g_bb_dumped = 1;
        fprintf(stderr, "[bb] 200M block entries -- hottest blocks:\n");
        for (int round = 0; round < 12; round++) {
            int best = -1;
            unsigned long long bestn = 0;
            for (int i = 0; i < GT5P_BB_MAX; i++)
                if (g_bb[i] > bestn) { bestn = g_bb[i]; best = i; }
            if (best < 0 || bestn == 0) break;
            fprintf(stderr, "[bb]   0x%08X  %llu\n", g_bb_addr[best], bestn);
            g_bb[best] = 0;
        }
        fflush(stderr);
    }
}

/* One-shot register dump from inside a hot loop. func_00956D20's window copy
 * should finish in ~26k iterations from the field values seen at entry, and it
 * runs 50M -- so the values the loop actually uses are re-read later in the
 * function from a different pointer. Print them once, deep into the spin. */
extern "C" void gt5p_bb_regs(unsigned long long r25, unsigned long long r26,
                             unsigned long long r28, unsigned long long r31,
                             unsigned long long r29)
{
    static unsigned long long n;
    ++n;
    /* Sample early and late: if the bound and wrap are already wrong on the
     * first iteration the loop was ENTERED with garbage; if they start sane
     * and go bad, something inside the loop clobbers them. */
    if (n != 1 && n != 1000 && n != 1000000ull && n != 5000000ull) return;
    fprintf(stderr,
            "[loop] iteration %llu:\n"
            "[loop]   r25 window base = 0x%llX\n"
            "[loop]   r28 wrap  (base+0x8000) = 0x%llX\n"
            "[loop]   r26 bound = 0x%llX  (offset %lld)\n"
            "[loop]   r31 dest  = 0x%llX  (offset %lld)\n"
            "[loop]   r29 src   = 0x%llX  (offset %lld)\n",
            n, r25, r28, r26, (long long)(r26 - r25), r31, (long long)(r31 - r25),
            r29, (long long)(r29 - r25));
    fflush(stderr);
}

/* Sampled at the window-copy setup, immediately before the loop is entered.
 * The loop samples show a sane bound for a million iterations and then a zero
 * one, which leaves two possibilities: either the setup itself ever runs with
 * a zero bound (bad input), or it never does and something inside the loop
 * clobbers the callee-saved registers (bad call). This tells them apart. */
extern "C" void gt5p_bb_setup(unsigned long long r25, unsigned long long r26,
                              unsigned long long r28, unsigned long long r31,
                              unsigned long long r24)
{
    static unsigned n;
    if (n++ >= 12) return;
    fprintf(stderr, "[setup] #%u base=0x%llX bound=0x%llX(+%lld) wrap=0x%llX(+%lld) "
                    "cursor=0x%llX(+%lld) r24=%llu\n",
            n, r25, r26, (long long)(r26 - r25), r28, (long long)(r28 - r25),
            r31, (long long)(r31 - r25), r24);
    fflush(stderr);
}

/* func_006679A0 lays out ~28 aligned buffers and then allocates the remainder
 * as `capacity - accumulated`. When the accumulated total exceeds the
 * capacity the subtraction goes negative, gets zero-extended to 32 bits, and
 * becomes a 2.6 GB allocation request that the allocator grants -- which is
 * what wrecks the arena and leaves main retrying 264 bytes forever. Print both
 * operands so it is clear which side is wrong. */
/* C++ linkage: the call site is declared inside lifted C++ code, where an
 * extern "C" declaration is not permitted at block scope. */
void gt5p_layout(unsigned long long capacity, unsigned long long used)
{
    static int n = 0;
    if (n++ >= 6) return;
    long long rem = (long long)(int)(unsigned)capacity - (long long)(int)(unsigned)used;
    fprintf(stderr, "[layout] capacity=0x%08X (%llu)  used=0x%08X (%llu)  remainder=%lld%s\n",
            (unsigned)capacity, capacity, (unsigned)used, used, rem,
            rem < 0 ? "   <-- NEGATIVE" : "");
    fflush(stderr);
}

/* func_00950650 walks the heap free list looking for the largest free block,
 * reading each node's size from +4 and its successor from +12. One node comes
 * back carrying a size larger than the whole 182 MB arena, and the title then
 * asks for that much. Report the first such node: its ADDRESS is what a write
 * watch needs to catch whoever trampled it. */
/* C++ linkage to match the block-scope declaration at the call site. */
extern "C" int gt5p_alloc_owner(unsigned addr, unsigned* out_ptr, unsigned* out_len);
void gt5p_freenode(unsigned node, unsigned size)
{
    enum { ARENA_LO = 0x20000000u, ARENA_HI = 0x2ADFFF80u };
    /* Remember the node the walk came from. A bad node is reached by following
     * a bad `next`, so the PREDECESSOR is where the damage actually is -- the
     * bad node itself is just whatever live memory that pointer landed in. */
    static unsigned prev_node, prev_size;
    unsigned pn = prev_node, ps = prev_size;
    prev_node = node; prev_size = size;
    if (size <= (ARENA_HI - ARENA_LO)) return;
    static int n = 0;
    if (n++ >= 4) return;
    unsigned op = 0, ol = 0;
    int owned = gt5p_alloc_owner(node, &op, &ol);
    fprintf(stderr, "[freelist] node=0x%08X size=0x%08X -- larger than the arena\n",
            node, size);
    {
        unsigned pp = 0, pl = 0;
        int pown = gt5p_alloc_owner(pn, &pp, &pl);
        fprintf(stderr, "[freelist]   reached from node=0x%08X size=0x%08X%s\n",

                pn, ps, pown ? "  (that one is ALSO inside a live block)" : "");
        if (pown)
            fprintf(stderr, "[freelist]     predecessor block 0x%08X..0x%08X (%u bytes)\n",

                    pp, pp + pl, pl);
    }
    if (owned)
        fprintf(stderr, "[freelist]   INSIDE live allocation 0x%08X..0x%08X (%u bytes) -- freed while in use\n",
                op, op + ol, ol);
    else
        fprintf(stderr, "[freelist]   not inside any recorded allocation\n");
    fflush(stderr);
}

/* Cross-reference: is a corrupt free-list node sitting inside a block the
 * allocator has handed out and not taken back?
 *
 * The write watch already showed that a "free" node is live memory, but not
 * WHICH allocation owns it. Recording every block the allocator returns and
 * then asking, at the moment the walk trips over a bad node, which live block
 * contains it turns "a block is free and allocated at once" into a specific
 * (address, size) pair -- which is what an instrumented free path needs to
 * match against. */
enum { GT5P_ALLOC_MAX = 4096 };
static unsigned g_alloc_ptr[GT5P_ALLOC_MAX];
static unsigned g_alloc_len[GT5P_ALLOC_MAX];
static unsigned g_alloc_n;

extern "C" void gt5p_alloc_record(unsigned ptr, unsigned len)
{
    if (g_alloc_n < GT5P_ALLOC_MAX) {
        g_alloc_ptr[g_alloc_n] = ptr;
        g_alloc_len[g_alloc_n] = len;
        g_alloc_n++;
    }
}

extern "C" int gt5p_alloc_owner(unsigned addr, unsigned* out_ptr, unsigned* out_len)
{
    for (unsigned i = 0; i < g_alloc_n; i++)
        if (addr >= g_alloc_ptr[i] && addr < g_alloc_ptr[i] + g_alloc_len[i]) {
            *out_ptr = g_alloc_ptr[i];
            *out_len = g_alloc_len[i];
            return 1;
        }
    return 0;
}

/* Free-list validator, used to bracket a suspect function.
 *
 * The write watch shows func_006A4400 and func_006C2D5C writing float data
 * into bytes the allocator reads as node linkage, but not whether the block
 * was on the list at the time -- which is the difference between a
 * use-after-free and a coincidence. Walking the list before and after a call
 * settles it: valid going in, broken coming out, and the caller owns it.
 *
 * Bucket heads live at heap+16..heap+76 (func_00950650 indexes them with r10
 * running 76 down to 16 in steps of 4); nodes carry an end pointer at +4 and a
 * successor at +12. */
extern "C" unsigned vm_read32(unsigned long long);

static int node_ok(unsigned n, unsigned end, unsigned next)
{
    enum { LO = 0x20000000u, HI = 0x2ADFFF80u };
    if (n & 3) return 0;                         /* nodes are aligned */
    if (n < LO || n >= HI) return 0;
    if (end < LO || end > HI || end <= n) return 0;
    if (next && (next < LO || next >= HI || (next & 3))) return 0;
    return 1;
}

extern "C" int gt5p_freelist_bad(unsigned heap)
{
    int bad = 0;
    for (unsigned off = 16; off <= 76; off += 4) {
        unsigned n = vm_read32(heap + off);
        for (int hops = 0; n && hops < 4096; hops++) {
            unsigned end  = vm_read32(n + 4);
            unsigned next = vm_read32(n + 12);
            if (!node_ok(n, end, next)) { bad++; break; }
            n = next;
        }
    }
    return bad;
}

/* The virtual immediately before the token count in func_006C2D5C is what
 * fills the scratch buffer with an object's parameter descriptor. The first
 * thirteen counts come back empty, so either a different method is being
 * dispatched for those objects or the same one is doing nothing. Print the
 * resolved target and the object so the two groups can be told apart. */
/* `obj` carries the dispatch index r5: func_006C5CF8 switches on it and
 * returns without filling when it exceeds 8. */
void gt5p_descr(unsigned target, unsigned obj)
{
    static unsigned n;
    if (n++ >= 26) return;
    fprintf(stderr, "[descr] #%u obj=0x%08X -> func_%08X\n", n, obj, target);
    fflush(stderr);
}
