#include <stdio.h>
#include <stdint.h>
/* SPU workload registry for Gran Turismo 5 Prologue.
 *
 * The runtime matches a guest SPU image by content fingerprint and runs the
 * lifted entry registered here. Without a match cellSpurs logs
 *
 *     [cellSpurs] wid=N PM NOT LIFTED (fp=... size=...) -- workload will not run
 *
 * and the workload never executes, so whatever the title was waiting on never
 * happens.
 *
 * Unlike the SPURS *job* binaries other titles load from their data files, this
 * one is embedded in EMAIN.ELF and can be taken statically: all four of this
 * title's workloads carry the same policy module, Sony's WWS job manager,
 * 11,648 bytes at guest 0x00F4CB80.
 *
 * The fingerprint is computed here, at registration time, by calling the
 * runtime's own spu_workload_fingerprint over the loaded image rather than
 * hardcoding a constant. That is not fussiness: the runtime's hash is seeded
 * with 1469598103934665603, which is the FNV-1a-64 offset basis with a digit
 * dropped (the real one is 14695981039346656037 = 0xCBF29CE484222325). It is
 * self-consistent, so it works fine as a hash -- but a constant baked in from
 * any offline FNV-1a-64 implementation will not match, and the workload then
 * silently never runs. Computing it in-process is immune either way.
 *
 * To regenerate the lifted image:
 *
 *   python - <<'EOF'
 *   d = open('input/EMAIN.ELF','rb').read()
 *   open('analysis/spu/wws_job_pm.bin','wb').write(d[0xF4CB80-0x10000:][:11648])
 *   EOF
 *   python <ps3recomp>/tools/find_spu_functions.py --raw --base 0 \
 *       --out analysis/spu/wwsjob_funcs.json analysis/spu/wws_job_pm.bin
 *   python <ps3recomp>/tools/spu_lifter.py analysis/spu/wws_job_pm.bin \
 *       --base 0 --functions analysis/spu/wwsjob_funcs.json \
 *       --symbol-prefix gt5p_wwsjob --output generated/spu_lifted/wwsjob \
 *       --source-name spu_recomp.c
 */
#include "spu_workload.h"

extern unsigned char* vm_base;
extern void spu_begin_image(int image_id);

extern void gt5p_wwsjob_spu_func_00000000(spu_context*);
extern void gt5p_wwsjob_spu_recomp_register(void);

#define WWS_JOB_PM_EA   0x00F4CB80u
#define WWS_JOB_PM_SIZE 11648u

/* Call after the ELF is in memory and before the game creates any SPURS
 * workload -- main.cpp does both. */
void gt5p_spu_register_all(void)
{
    uint64_t fp = spu_workload_fingerprint(vm_base + WWS_JOB_PM_EA, WWS_JOB_PM_SIZE);

    spu_begin_image(1);
    gt5p_wwsjob_spu_recomp_register();
    spu_workload_register_img(fp, gt5p_wwsjob_spu_func_00000000, 1, "Wws_Job");
    spu_begin_image(0);

    printf("[GT5P] SPU: Wws_Job registered (fp=0x%016llX, %u bytes at 0x%08X)\n",
           (unsigned long long)fp, WWS_JOB_PM_SIZE, WWS_JOB_PM_EA);
}
