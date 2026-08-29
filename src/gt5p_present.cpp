/* Present / vblank ticker.
 *
 * Nothing in a port's own main() advances the display. The game requests a flip
 * (`_cellGcmSetFlipCommand`), then polls `cellGcmGetFlipStatus` until it clears
 * -- and `cellGcmTickFlip` is what clears it. Without a thread calling it, a
 * title that flips waits forever, and nothing ever drains the RSX FIFO, so no
 * command the guest wrote is ever executed.
 *
 * ps3recomp's own harness (runtime/ppu/tests/boot_main.cpp) runs a thread like
 * this; a port with its own main() needs its own.
 *
 * Ticks are driven off real elapsed time rather than off how long present()
 * takes: on a hidden or occluded window DXGI throttles Present hard, and pacing
 * the guest's frame counter behind it drops the whole boot to a few frames a
 * second.
 *
 * ponytail: the harness cadence, unchanged (16 ms ticks, drain again at the
 * outer 4 ms cadence). Measure before retuning -- the drain interval is
 * load-bearing for titles that fence every render pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "ppu_context.h"
#include "sys_ppu_thread.h"

/* GT5P_THREADS=1: every 2 s, print each PPU thread's state and the guest
 * address of its last syscall/HLE call. "Runs its job loop but never loads
 * anything" is exactly the shape a wedged worker produces, and nothing else
 * shows which thread is where. */
extern "C" const char* g_hle_inflight[];

static void dump_threads(void)
{
    static int on = -1;
    if (on < 0) on = getenv("GT5P_THREADS") ? 1 : 0;
    if (!on) return;

    static ULONGLONG next = 0;
    ULONGLONG now = GetTickCount64();
    if (now < next) return;
    next = now + 2000;

    static const char* kState[] = { "FREE", "RUNNING", "FINISHED", "DETACHED" };
    fprintf(stderr, "[threads]\n");
    for (int i = 0; i < PPU_THREAD_MAX; i++) {
        const ppu_thread_info* t = &g_ppu_threads[i];
        if (t->state == PPU_THREAD_STATE_FREE) continue;
        fprintf(stderr, "   tid=%d %-9s entry=0x%08llX prof_pc=0x%08X hle=%-26s name=\"%s\"\n",
                i, kState[t->state & 3],
                (unsigned long long)t->entry_addr, t->prof_pc,
                (i < 64 && g_hle_inflight[i]) ? g_hle_inflight[i] : "-",
                t->name);
    }
}

extern "C" {
int  rsx_d3d12_backend_init(unsigned width, unsigned height, const char* title);
void rsx_d3d12_backend_present(void);
int  rsx_d3d12_backend_pump_messages(void);
void cellGcmTickVBlank(void);
void cellGcmTickFlip(void);
void cellGcm_rsx_process_fifo(void);
int  cellGcm_take_flip_pending(void);
}

static DWORD WINAPI present_thread(LPVOID)
{
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "Gran Turismo 5 Prologue (ps3recomp)";

    int rsx_ok = (rsx_d3d12_backend_init(1280, 720, title) == 0);
    fprintf(stderr, "[rsx] backend init %s\n",
            rsx_ok ? "OK -- window open" : "FAILED");

    ULONGLONG next_tick = GetTickCount64();
    for (;;) {
        Sleep(4);
        ULONGLONG now = GetTickCount64();

        int fired = 0;
        while ((long long)(now - next_tick) >= 0 && fired < 240) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            if (rsx_ok && cellGcm_take_flip_pending())
                rsx_d3d12_backend_present();
            if (rsx_ok)
                cellGcm_rsx_process_fifo();
            next_tick += 16;          /* ~60 Hz */
            fired++;
        }
        if (fired >= 240)
            next_tick = now;          /* fell far behind -- resync */

        dump_threads();

        /* Drain again at the outer cadence: titles fence every render pass on
         * an RSX label the drain writes, and waiting a full 16 ms per fence
         * paces the guest to single-digit fps. */
        if (rsx_ok) {
            if (cellGcm_take_flip_pending())
                rsx_d3d12_backend_present();
            cellGcm_rsx_process_fifo();
            if (rsx_d3d12_backend_pump_messages() != 0)
                rsx_ok = 0;           /* window closed */
        }
    }
}

extern "C" void gt5p_start_present_thread(void)
{
    if (getenv("GT5P_NO_PRESENT")) {
        printf("[GT5P] GT5P_NO_PRESENT: headless, no present thread\n");
        return;
    }
    CreateThread(NULL, 4u * 1024 * 1024, present_thread, NULL, 0, NULL);
    printf("[GT5P] present thread started (60 Hz vblank/flip ticker)\n");
}
