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
