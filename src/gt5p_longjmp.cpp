/* setjmp/longjmp for the recompiled build.
 *
 * func_00A0A458 and func_00A0A57C are the PS3 libc setjmp and longjmp: the
 * first saves r1, r2, r13-r31, LR, CR and the callee-saved FPRs into a
 * 16-byte-aligned buffer, the second loads them all back. On hardware longjmp
 * ends in `blr` *after* restoring LR from the buffer, so the branch goes to
 * the setjmp call site, not to longjmp's own caller.
 *
 * A static recompiler cannot express that. ppu_lifter emits longjmp's `blr` as
 * a plain `return;`, so the lifted longjmp restores every guest register and
 * then returns to whoever called it -- which is somewhere deep inside the
 * decoder, now running with the register values that were live at the setjmp.
 *
 * That is not theoretical. GT5P's LZ decoder aborts a decompression by
 * longjmp-ing out of it: func_00919060 arms a buffer at obj+0x186C, and when
 * the input runs dry func_009200F0 fires it. With the jump modelled as a
 * return, control resumed inside a 32 KB window-copy loop carrying
 * func_00919060's registers -- the loop bound became 0 and the wrap pointer
 * the object's own address, so the copy could never terminate and walked
 * megabytes past its window writing bytes.
 *
 * The fix is to put a real host setjmp in the lifted frame that calls the
 * guest setjmp, and to have the guest longjmp reach it with a host longjmp.
 * The guest register file is still restored by the original lifted longjmp
 * body, so the guest sees exactly the state it expects; only the transfer of
 * control is host-side. Frames jumped over hold no C++ objects with
 * destructors -- lifted code is plain C in C++ clothing -- so the unwind is
 * safe.
 *
 * scripts/setjmp_patch.py installs the call-site half; re-run it after every
 * re-lift, since generated/ is rebuilt wholesale.
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>

#define GT5P_JMP_MAX 16

struct gt5p_jmp {
    jmp_buf  host;
    uint32_t buf_ea;   /* the guest jmp_buf, aligned the way the guest aligns it */
};

/* A stack, not a map: setjmp buffers nest, and a buffer is only a valid target
 * while the frame that armed it is still on the host stack. The wrapper around
 * each setjmp-calling function restores the depth on the way out, so a buffer
 * whose frame has returned can never be fired into. */
static thread_local gt5p_jmp g_jmp[GT5P_JMP_MAX];
static thread_local int      g_depth;

/* The guest rounds the buffer pointer up to 16 in both setjmp and longjmp
 * (addi r3,r3,15 / rldicr r3,r3,0,59), so key on the same value. */
static uint32_t gt5p_jmp_key(uint32_t ea) { return (ea + 15u) & ~15u; }

extern "C" gt5p_jmp* gt5p_jmp_push(uint32_t buf_ea)
{
    if (g_depth >= GT5P_JMP_MAX) {
        fprintf(stderr, "[setjmp] depth overflow, buffer 0x%08X not armed\n", buf_ea);
        return &g_jmp[GT5P_JMP_MAX - 1];
    }
    gt5p_jmp* e = &g_jmp[g_depth++];
    e->buf_ea = gt5p_jmp_key(buf_ea);
    return e;
}

extern "C" int  gt5p_jmp_depth(void)       { return g_depth; }
extern "C" void gt5p_jmp_unwind(int depth) { if (depth < g_depth) g_depth = depth; }

/* Returns only if no live buffer matches, in which case the caller falls back
 * to the old behaviour and returns normally. */
extern "C" void gt5p_jmp_fire(uint32_t buf_ea, int value)
{
    uint32_t key = gt5p_jmp_key(buf_ea);
    for (int i = g_depth - 1; i >= 0; i--) {
        if (g_jmp[i].buf_ea != key) continue;
        g_depth = i;                       /* the target frame is the new top */
        longjmp(g_jmp[i].host, value ? value : 1);
    }
    static int warned = 0;
    if (warned++ < 4)
        fprintf(stderr, "[setjmp] longjmp to 0x%08X with no live buffer\n", buf_ea);
}

/* The call-site patch cannot see the layout of gt5p_jmp, so hand it the
 * jmp_buf. setjmp still expands lexically at the call site, which is the part
 * that matters -- this only locates the storage. */
extern "C" jmp_buf* gt5p_jmp_host(gt5p_jmp* e) { return &e->host; }
