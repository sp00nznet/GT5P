#!/usr/bin/env python3
"""Make guest setjmp/longjmp transfer control instead of returning.

ppu_lifter emits longjmp's final `blr` as `return;`, so the lifted longjmp
restores the guest register file and then returns to its own caller rather than
to the setjmp site. See src/gt5p_longjmp.cpp for what that does to GT5P's LZ
decoder.

Two edits, both in generated/:

  * every call site of the guest setjmp gets a real host setjmp wrapped around
    it, so the landing point is a live frame on the host stack;
  * the guest longjmp keeps its original body -- which is what restores the
    guest registers -- and then fires the host longjmp.

Run after every re-lift; generated/ is rebuilt wholesale and is not in the repo.

    python scripts/setjmp_patch.py
"""
import re
import sys
from pathlib import Path

GEN = Path(__file__).resolve().parent.parent / "generated"

SETJMP = "00A0A458"
LONGJMP = "00A0A57C"

DECLS = """
/* ---- setjmp_patch.py ------------------------------------------------- */
#include <setjmp.h>
#include <stdint.h>
struct gt5p_jmp;
extern "C" gt5p_jmp* gt5p_jmp_push(uint32_t buf_ea);
extern "C" jmp_buf*  gt5p_jmp_host(gt5p_jmp* e);
extern "C" int       gt5p_jmp_depth(void);
extern "C" void      gt5p_jmp_unwind(int depth);
extern "C" void      gt5p_jmp_fire(uint32_t buf_ea, int value);
"""

LONGJMP_WRAPPER = """
/* ---- setjmp_patch.py: guest longjmp ---------------------------------- */
void func_%(lj)s(ppu_context* ctx)
{
    /* Capture the arguments before the original body overwrites the register
     * file with the saved state. */
    uint32_t buf = (uint32_t)ctx->gpr[3];
    int      val = (int)(int32_t)ctx->gpr[4];

    /* The original restores r1, r2, r13-r31, LR, CR and the FPRs from the
     * guest buffer -- exactly the state the guest expects to resume with. */
    gt5p_orig_func_%(lj)s(ctx);
    DRAIN_TRAMPOLINE(ctx);

    /* Then actually go there. Returns only if nothing is armed, in which case
     * this degrades to the old (wrong, but no worse) behaviour. */
    gt5p_jmp_fire(buf, val);
}
"""

# The lifted call site looks like:
#     ctx->lr = 0x009190C4; func_00A0A458(ctx); DRAIN_TRAMPOLINE(ctx);
CALL_RE = re.compile(
    r"( *)ctx->lr = (0x[0-9A-Fa-f]+); func_" + SETJMP + r"\(ctx\); DRAIN_TRAMPOLINE\(ctx\);")


def patch_call_sites(text):
    """Put a host setjmp in the frame that calls the guest setjmp.

    setjmp has to appear lexically in the frame that will be jumped back into,
    so this cannot live behind a helper -- it is expanded at each site.
    """
    def repl(m):
        ind, lr = m.group(1), m.group(2)
        return (
            f"{ind}{{ gt5p_jmp* _j = gt5p_jmp_push((uint32_t)ctx->gpr[3]);\n"
            f"{ind}  if (setjmp(*gt5p_jmp_host(_j)) == 0) {{\n"
            f"{ind}      ctx->lr = {lr}; func_{SETJMP}(ctx); DRAIN_TRAMPOLINE(ctx);\n"
            f"{ind}  }} }}")
    return CALL_RE.subn(repl, text)


def guard_callers(text, names):
    """Restore the buffer stack depth when a setjmp-calling function returns.

    Without this a buffer stays armed after its frame is gone, and a later
    longjmp would jump into a dead frame.
    """
    n = 0
    for name in names:
        define = f"void {name}(ppu_context* ctx) {{"
        if define not in text or f"gt5p_orig_{name}" in text:
            continue
        text = text.replace(define, f"void gt5p_orig_{name}(ppu_context* ctx) {{", 1)
        text += (
            f"\n/* ---- setjmp_patch.py: {name} arms a setjmp buffer ---- */\n"
            f"void {name}(ppu_context* ctx)\n{{\n"
            f"    int _d = gt5p_jmp_depth();\n"
            f"    gt5p_orig_{name}(ctx);\n"
            f"    gt5p_jmp_unwind(_d);\n"
            f"}}\n")
        n += 1
    return text, n


def main():
    total_sites, total_guards, did_longjmp = 0, 0, False
    for chunk in sorted(GEN.glob("ppu_recomp_*.cpp")):
        text = chunk.read_text(encoding="utf-8", errors="replace")
        original = text

        if "setjmp_patch.py" not in text:
            # find which functions contain a setjmp call before rewriting them
            owners = []
            for m in re.finditer(r"^void (?:gt5p_orig_)?(func_[0-9A-F]{8})\(ppu_context\* ctx\) \{",
                                 text, re.M):
                end = text.find("\nvoid ", m.end())
                body = text[m.start():end if end > 0 else len(text)]
                if f"func_{SETJMP}(ctx);" in body:
                    owners.append(m.group(1))

            text, sites = patch_call_sites(text)
            total_sites += sites
            if sites:
                text = DECLS + text
                text, guards = guard_callers(text, owners)
                total_guards += guards

            define = f"void func_{LONGJMP}(ppu_context* ctx) {{"
            if define in text:
                text = text.replace(define, f"void gt5p_orig_func_{LONGJMP}(ppu_context* ctx) {{", 1)
                if DECLS not in text:
                    text = DECLS + text
                text += LONGJMP_WRAPPER % {"lj": LONGJMP}
                did_longjmp = True

        if text != original:
            chunk.write_text(text, encoding="utf-8")
            print(f"  patched {chunk.name}")

    print(f"setjmp call sites wrapped: {total_sites}")
    print(f"arming functions guarded:  {total_guards}")
    print(f"longjmp rewritten:         {did_longjmp}")
    return 0 if (total_sites and did_longjmp) else 1


if __name__ == "__main__":
    sys.exit(main())
