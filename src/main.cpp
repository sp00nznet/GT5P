/**
 * Gran Turismo 5 Prologue Recompiled — entry point
 *
 * Brings up the ps3recomp virtual machine, loads EMAIN.ELF at its own
 * addresses, registers the lifted function table, and calls the guest's
 * entry point.
 *
 * There is deliberately no import resolver here. Every one of this title's
 * 439 import thunks was lifted as `ps3_hle_call(<nid>, ctx)` (the lifter's
 * --hle-stubs path, fed by scripts/gen_hle_stubs.py reading the ELF's own
 * module descriptors), so imports resolve by NID inside the runtime with
 * nothing to patch at load time.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "ps3emu/module.h"

#include "ppu_context.h"
#include "vm.h"
#include "lv2_syscall_table.h"

#include "elf_loader.h"

/* The lifter's function table, defined in the generated sources. Declared here
 * rather than including generated/ppu_recomp.h: that header defines its own
 * ppu_context, which collides with the runtime's. The two layouts are kept
 * identical by the lifter, and this is the only symbol we need from it. */
extern "C" {
typedef struct { uint64_t addr; void (*func)(ppu_context*); const char* name; } func_entry;
extern const func_entry function_table[];
extern const uint64_t   function_table_count;   /* entries, excluding sentinel */
}

/* Definitions the runtime and the HLE libraries declare extern. */
uint8_t* vm_base = nullptr;
ps3_module_registry g_ps3_module_registry = {};

extern "C" {
void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
void ppu_recomp_register(void);           /* installs function_table[] */
void ppu_install_thread_trampoline(void);
void ppu_hle_register_all(void);          /* generated: src/gen/ppu_hle_nids.cpp */
void ppu_sysprx_register(void);
void ps3_install_guest_ptr_trap(void);
unsigned int ps3_hle_count(void);
void cellfs_set_root_path(const char* root);
void cellfs_add_path_mapping(const char* ps3_prefix, const char* host_path);
void cellGame_init_from_paramsfo(const char* sfo_path);
void vm_write32(uint64_t addr, uint32_t val);   /* big-endian guest store */
uint32_t vm_read32(uint64_t addr);
extern uint32_t ppu_hle_inject_base;      /* HLE-visible GCM window base */
uint32_t ppu_prof_resolve_host(void* ra);  /* host RIP -> guest function addr */
int  ppu_stwcx32(uint64_t ea, uint32_t expected, uint32_t val);
void ps3_indirect_call(ppu_context* ctx);
void ps3_hle_call(unsigned nid, ppu_context* ctx);
uint8_t vm_read8(uint64_t addr);
void ppu_guard_page(uint32_t guest_ea);
void vm_write64(uint64_t a, uint64_t v);
void vm_write8(uint64_t a, uint8_t v);
void vm_write16(uint64_t a, uint16_t v);
uint64_t vm_read64(uint64_t a);
extern const char* g_hle_inflight[64];    /* per-thread: HLE currently executing */
void gt5p_spu_register_all(void);         /* generated: src/gen/spu_workloads.c */
void gt5p_start_present_thread(void);     /* src/gt5p_present.cpp */
}

/* RSX local memory, as cellGcmGetConfiguration reports it (cellGcmSys.c). */
#define GT5P_LOCAL_MEM_BASE 0xC0000000u
#define GT5P_LOCAL_MEM_SIZE 0x10000000u

/* GCM control/label page. cellGcmSys keeps put/get/ref and the label slots
 * here, and the RSX pump reads them every frame. */
#define GT5P_GCM_CTRL_BASE  0x03000000u
#define GT5P_GCM_CTRL_SIZE  0x00010000u

/* argv/envp block handed to the guest _start. */
#define GT5P_ARGV_BASE      0x00F00000u

static ppu_context g_main_ctx;

/* function_table[] is emitted in ascending address order and
 * sentinel-terminated; function_table_count excludes the sentinel. */
static void (*dispatch_lookup(uint32_t guest_addr))(ppu_context*)
{
    uint64_t lo = 0, hi = function_table_count;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        uint32_t a = (uint32_t)function_table[mid].addr;
        if (a == guest_addr) return function_table[mid].func;
        if (a < guest_addr) lo = mid + 1; else hi = mid;
    }
    return nullptr;
}

/* Nearest lifted function at or below `addr`, for turning a saved return
 * address back into func_XXXXXXXX+offset. */
static uint32_t enclosing_func(uint32_t addr)
{
    uint64_t lo = 0, hi = function_table_count, best = 0;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        uint32_t a = (uint32_t)function_table[mid].addr;
        if (a <= addr) { best = a; lo = mid + 1; } else hi = mid;
    }
    return (uint32_t)best;
}

#ifdef _WIN32
/* GT5P_MAINSTACK=<seconds>: after that long, print the main thread's guest call
 * chain. The lifter's fragment model leaves the ABI back-chain LR slots zero,
 * so walking is useless; scanning the stack for words that land inside a lifted
 * function is not, and a lifted function's name IS its guest address.
 *
 * The runtime has ppu_dump_guest_stack, but it filters to guest < 0x600000 --
 * this title's code runs to 0xBE0AF4, so every address worth seeing here is
 * outside that window. */
#define GT5P_CODE_LO 0x00010000u
#define GT5P_CODE_HI 0x00BE0AF4u

static HANDLE g_main_thread;
static uintptr_t g_last_rip;
static void dump_hostmap(void);

/* GT5P_GUARD=<watch>:<arm>: spin until the guest word at <arm> becomes
 * non-zero, then write-protect <watch>'s page so the runtime's guard handler
 * reports every writer into it.
 *
 * The store watch (LBP_WW) only sees lifted vm_write8/16/32. A 64-bit `std`,
 * an atomic, or a host-side memset lands invisibly -- and this title has a
 * free-list entry whose fields are zeroed at init and read back as -1 with no
 * store in between, which is exactly that shape. */
static DWORD WINAPI guard_arm(LPVOID spec)
{
    uint32_t watch = 0, arm = 0, want = 0;
    int n = sscanf((const char*)spec, "%x:%x:%x", &watch, &arm, &want);
    if (n < 2 || !watch) return 0;
    for (int i = 0; i < 60000 && arm; i++) {
        uint32_t v = vm_read32(arm);
        if (n >= 3 ? (v == want) : (v != 0)) break;
        Sleep(1);
    }
    fprintf(stderr, "[GT5P] arming page guard on 0x%08X ([0x%08X]=0x%08X)\n",
            watch, arm, vm_read32(arm));
    dump_hostmap();
    fflush(stderr);
    ppu_guard_page(watch);
    return 0;
}

/* The page guard reports a writer as a module RVA. Print the RVAs of the
 * runtime entry points a guest store can plausibly come through, so the nearest
 * one below it names the writer without a symbol server. RVAs move between
 * builds, so this has to come from the same run as the guard output. */
static void dump_hostmap(void)
{
    {
        char* mb = (char*)GetModuleHandleA(nullptr);
        struct { const char* n; void* p; } k[] = {
            { "vm_write32",     (void*)&vm_write32     },
            { "vm_read32",      (void*)&vm_read32      },
            { "ppu_stwcx32",    (void*)&ppu_stwcx32    },
            { "ps3_hle_call",   (void*)&ps3_hle_call   },
            { "ps3_indirect",   (void*)&ps3_indirect_call },
            { "ppu_guard_page", (void*)&ppu_guard_page },
            { "vm_write8",      (void*)&vm_write8      },
            { "vm_write16",     (void*)&vm_write16     },
            { "vm_write64",     (void*)&vm_write64     },
            { "vm_read64",      (void*)&vm_read64      },
            { "ppu_register_fn",(void*)&ppu_register_function },
        };
        for (auto& e : k)
            fprintf(stderr, "[hostmap] %-14s rva=0x%llX\n", e.n,
                    (unsigned long long)((char*)e.p - mb));
    }
    fflush(stderr);
}

static DWORD WINAPI main_stack_dump(LPVOID secs)
{
  for (;;) {
    Sleep((DWORD)(intptr_t)secs * 1000);

    /* Where is the main thread actually executing? ctx->lr only names the last
     * `bl` the lifted code took, and a function reached by a tail branch, or
     * looping without calling anything, never updates it. Suspending the thread
     * and mapping its RIP back through the function table names the guest
     * function it is *in*. */
    uint32_t live = 0;
    char hoststack[1400]; hoststack[0] = 0;
    if (g_main_thread) {
        CONTEXT hc; hc.ContextFlags = CONTEXT_FULL;
        if (SuspendThread(g_main_thread) != (DWORD)-1) {
            if (GetThreadContext(g_main_thread, &hc)) {
                live = ppu_prof_resolve_host((void*)hc.Rip);
                g_last_rip = (uintptr_t)hc.Rip;

                /* Walk the host stack with the Win64 unwinder and map each
                 * frame back to its guest function. A frame that resolves to
                 * func_00000000 is runtime/library code, and its position in
                 * the chain says which lifted function called into it. */
                int p = 0;
                for (int f = 0; f < 24 && hc.Rip && p < 1250; f++) {
                    uint32_t g = ppu_prof_resolve_host((void*)hc.Rip);
                    p += snprintf(hoststack + p, sizeof(hoststack) - p,
                                  g ? "  func_%08X" : "  <host:%08X>",
                                  g ? g : (unsigned)(hc.Rip & 0xFFFFFFFF));
                    DWORD64 imgBase = 0;
                    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(hc.Rip, &imgBase, nullptr);
                    if (!rf) break;                 /* leaf / no unwind data */
                    PVOID hd = nullptr; DWORD64 est = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imgBase, hc.Rip, rf,
                                     &hc, &hd, &est, nullptr);
                }
            }
            ResumeThread(g_main_thread);
        }
    }

    uint32_t sp = (uint32_t)g_main_ctx.gpr[1];
    fprintf(stderr, "[mainstack] in=func_%08X sp=0x%08X lr=0x%08X ctr=0x%08X r3=0x%08X r31=0x%08X\n",
            live, sp, (uint32_t)g_main_ctx.lr, (uint32_t)g_main_ctx.ctr,
            (uint32_t)g_main_ctx.gpr[3], (uint32_t)g_main_ctx.gpr[31]);
    fprintf(stderr, "[mainstack] in_hle=%s rip=%p\n",
            g_hle_inflight[0] ? g_hle_inflight[0] : "-", (void*)g_last_rip);
    {
        static int once = 0;
        if (!once++)
            fprintf(stderr, "[hostmap] stwcx32=%p indirect=%p hle_call=%p "
                            "read32=%p write32=%p\n",
                    (void*)&ppu_stwcx32, (void*)&ps3_indirect_call,
                    (void*)&ps3_hle_call, (void*)&vm_read32, (void*)&vm_write32);
    }
    fprintf(stderr, "[mainregs]");
    for (int r = 0; r < 32; r++)
        fprintf(stderr, " r%d=%08X", r, (uint32_t)g_main_ctx.gpr[r]);
    fprintf(stderr, "\n[mainregs] [r9]=%08X [r26]=%08X\n",
            vm_read32((uint32_t)g_main_ctx.gpr[9]),
            vm_read32((uint32_t)g_main_ctx.gpr[26]));

    /* GT5P_PEEK=addr[,addr...]: eight guest words at each address, then eight
     * more at whatever the first word points to. One level of indirection is
     * enough to read a pointer slot and the object behind it in one go. */
    if (const char* peek = getenv("GT5P_PEEK")) {
        char buf[512];
        strncpy(buf, peek, sizeof buf - 1); buf[sizeof buf - 1] = 0;
        for (char* tok = strtok(buf, ","); tok; tok = strtok(nullptr, ",")) {
            uint32_t a = (uint32_t)strtoul(tok, nullptr, 0);
            for (int level = 0; level < 2 && a; level++) {
                fprintf(stderr, "[peek] %08X:", a);
                for (int k = 0; k < 8; k++)
                    fprintf(stderr, " %08X", vm_read32(a + (uint32_t)k * 4));
                /* The same words straight out of the mapping, bypassing
                 * vm_read32 and its instrumentation. If these two lines
                 * disagree, the accessor is lying — and the guest reads the
                 * same lie. */
                fprintf(stderr, "\n[peek] %08X raw:", a);
                for (int k = 0; k < 8; k++) {
                    uint32_t w;
                    memcpy(&w, vm_base + a + (uint32_t)k * 4, 4);
                    fprintf(stderr, " %08X", _byteswap_ulong(w));
                }
                fputc('\n', stderr);
                a = vm_read32(a);
            }
        }
    }
    fprintf(stderr, "[hostchain]%s\n", hoststack);

    uint32_t last = 0;
    int shown = 0;
    for (int i = 0; i < 2048 && shown < 10; i++) {
        uint32_t w = vm_read32(sp + (uint32_t)i * 4);
        if (w < GT5P_CODE_LO || w >= GT5P_CODE_HI || w == last) continue;
        uint32_t f = enclosing_func(w);
        if (!f || w - f == 0 || w - f >= 0x4000) continue;   /* not a return site */
        fprintf(stderr, "   func_%08X+0x%-5X\n", f, w - f);
        last = w;
        shown++;
    }
    fflush(stderr);
  }
}
#endif

#ifdef _WIN32
/* Commit guest pages on first touch, the way ps3recomp's own boot harness
 * does. A title with its own main() does not get that for free, and any guest
 * access to a page nothing had explicitly committed kills the process instead
 * of just working. Commits are logged -- a page the guest touches that nothing
 * allocated is still worth knowing about; this makes it survivable, not
 * invisible. GT5P_NO_FAULT_COMMIT=1 lets the fault stand. */
static LONG CALLBACK fault_commit(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t at = (uintptr_t)er->ExceptionInformation[1];
    if (!vm_base || at < (uintptr_t)vm_base || at >= (uintptr_t)vm_base + 0x100000000ull)
        return EXCEPTION_CONTINUE_SEARCH;
    void* page = (void*)(at & ~(uintptr_t)0xFFFF);
    if (!VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
        return EXCEPTION_CONTINUE_SEARCH;
    static LONG n = 0;
    if (InterlockedIncrement(&n) <= 24)
        fprintf(stderr, "[GT5P] faulted in guest page 0x%08X (%s) -- committed\n",
                (uint32_t)((uintptr_t)page - (uintptr_t)vm_base),
                er->ExceptionInformation[0] == 1 ? "write" : "read");
    return EXCEPTION_CONTINUE_EXECUTION;
}

static LONG CALLBACK report_crash(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const char* how = er->ExceptionInformation[0] == 0 ? "read"
                        : er->ExceptionInformation[0] == 1 ? "write" : "execute";
        uintptr_t at = (uintptr_t)er->ExceptionInformation[1];
        fprintf(stderr, "\n[GT5P] ACCESS VIOLATION: %s at host 0x%llX", how,
                (unsigned long long)at);
        if (vm_base && at >= (uintptr_t)vm_base && at < (uintptr_t)vm_base + 0x100000000ull)
            fprintf(stderr, " = GUEST 0x%08X", (uint32_t)(at - (uintptr_t)vm_base));
        fprintf(stderr, " (thread %lu)\n", GetCurrentThreadId());
    }
    fprintf(stderr, "[GT5P]   r1(SP)=0x%08X r2(TOC)=0x%08X r3=0x%08X LR=0x%08X CTR=0x%08X\n",
            (uint32_t)g_main_ctx.gpr[1], (uint32_t)g_main_ctx.gpr[2],
            (uint32_t)g_main_ctx.gpr[3], (uint32_t)g_main_ctx.lr,
            (uint32_t)g_main_ctx.ctr);
    fflush(stderr);
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

static void setup_argv(const char* argv0)
{
    /* PS3 _start ABI: r3 = argc, r4 = argv, r5 = envp. A PS3 title derives its
     * content directory from argv[0], so passing none leaves it with no data
     * path at all. /app_home is the prefix ppu_fs.cpp strips before appending
     * the rest to the VFS root. */
    vm_commit(GT5P_ARGV_BASE, 0x10000u);
    memset(vm_base + GT5P_ARGV_BASE, 0, 0x10000u);

    uint32_t str_ea = GT5P_ARGV_BASE + 0x100;
    strcpy((char*)(vm_base + str_ea), argv0);
    vm_write32(GT5P_ARGV_BASE + 0, str_ea);   /* argv[0]        */
    vm_write32(GT5P_ARGV_BASE + 4, 0);        /* argv[1] = NULL */

    uint32_t envp_ea = GT5P_ARGV_BASE + 0x200;
    vm_write32(envp_ea, 0);                   /* envp[0] = NULL */

    g_main_ctx.gpr[3] = 1;
    g_main_ctx.gpr[4] = GT5P_ARGV_BASE;
    g_main_ctx.gpr[5] = envp_ea;
    printf("[GT5P] argv[0] = \"%s\"\n", argv0);
}

int main(int argc, char* argv[])
{
    /* Buffer both streams. Unbuffered stdio issues a write per conversion, so
     * with several guest threads logging at once lines interleave mid-token and
     * a message that IS in the log reads as absent to grep. The crash filter
     * flushes, so a fault still shows the tail. */
    static char out_buf[1 << 16], err_buf[1 << 16];
    setvbuf(stdout, out_buf, _IOFBF, sizeof out_buf);
    setvbuf(stderr, err_buf, _IOFBF, sizeof err_buf);

    printf("=== Gran Turismo 5 Prologue Recompiled ===\n");
    printf("Built with ps3recomp | %llu lifted functions\n\n",
           (unsigned long long)function_table_count);

#ifdef _WIN32
    ps3_install_guest_ptr_trap();
    if (!getenv("GT5P_NO_FAULT_COMMIT"))
        AddVectoredExceptionHandler(1, fault_commit);
    SetUnhandledExceptionFilter(report_crash);
#endif

    /* Move the HLE-visible GCM window (labels, control register, and the 16 KB
     * of offset tables) off this title's heap. NPUA80075 maps 174 MB at
     * 0x20000000 -- the runtime's default base -- and publishing the offset
     * tables there overwrote the CRT allocator's size-class pool, which wedged
     * the main thread in a livelock for the whole boot. 0x03000000 is free
     * here, and main() commits it below. Must be set before any guest code. */
    ppu_hle_inject_base = GT5P_GCM_CTRL_BASE;

    if (int32_t rc = vm_init()) {
        fprintf(stderr, "[GT5P] ERROR: VM init failed (0x%08X)\n", (unsigned)rc);
        return EXIT_FAILURE;
    }
    printf("[GT5P] VM initialised (base=%p)\n", (void*)vm_base);

    vm_commit(0, 0x10000);                                   /* null-page guard  */
    vm_commit(GT5P_GCM_CTRL_BASE, GT5P_GCM_CTRL_SIZE);
    memset(vm_base + GT5P_GCM_CTRL_BASE, 0, GT5P_GCM_CTRL_SIZE);
    vm_commit(GT5P_LOCAL_MEM_BASE, GT5P_LOCAL_MEM_SIZE);     /* RSX local memory */
    printf("[GT5P] GCM control page + %u MB RSX local memory committed\n",
           GT5P_LOCAL_MEM_SIZE / (1024 * 1024));

    /* NPUA80075 is an HDD title (PARAM.SFO CATEGORY=HG), so the disc, hdd-game
     * and app_home roots all have to land on the extracted package. */
    {
        const char* vfs = getenv("PS3_VFS_ROOT");
        if (!vfs || !*vfs) vfs = ".";
        cellfs_set_root_path(vfs);
        cellfs_add_path_mapping("/dev_bdvd/PS3_GAME/",          "input/pkg/");
        cellfs_add_path_mapping("/dev_hdd0/game/NPUA80075/",    "input/pkg/");
        cellfs_add_path_mapping("/app_home/",                   "input/pkg/");
        cellfs_add_path_mapping("/dev_hdd0/",                   "gamedata/dev_hdd0/");
        printf("[GT5P] cellFs root=\"%s\" (disc + hdd game -> input/pkg/)\n", vfs);

        /* Give cellGame the real ids. Without this the HLE answers every
         * cellGame/cellDiscGame query with its built-in fallback, and this
         * title's boot was being told its id is "BLES00000" when PARAM.SFO
         * says NPUA80075 -- which matters the moment anything builds a path
         * from it (/dev_hdd0/game/<id>/...). */
        {
            char sfo[512];
            snprintf(sfo, sizeof sfo, "%s/input/pkg/PARAM.SFO", vfs);
            cellGame_init_from_paramsfo(sfo);
        }
    }

    lv2_register_all_syscalls(&g_lv2_syscalls);

    const char* elf_path = argc > 1 ? argv[1] : "input/EMAIN.ELF";
    printf("[GT5P] Loading ELF: %s\n", elf_path);
    ElfLoadResult elf = load_elf_into_vm(elf_path);
    if (!elf.success) {
        fprintf(stderr, "[GT5P] ERROR: failed to load %s\n", elf_path);
        vm_shutdown();
        return EXIT_FAILURE;
    }

    ppu_hle_register_all();
    ppu_sysprx_register();
    printf("[HLE] ps3recomp NID registry: %u handlers\n", ps3_hle_count());

    ppu_recomp_register();
    ppu_install_thread_trampoline();
    gt5p_spu_register_all();   /* after the ELF load: fingerprints the loaded image */

    uint32_t entry_addr = (uint32_t)elf.func_addr;
    auto entry_func = dispatch_lookup(entry_addr);
    if (!entry_func) {
        fprintf(stderr, "[GT5P] ERROR: entry 0x%08X is not in the lifted function "
                        "table.\n", entry_addr);
        vm_shutdown();
        return EXIT_FAILURE;
    }

    g_main_ctx.gpr[2] = elf.toc;
    setup_argv(getenv("PS3_ARGV0") && *getenv("PS3_ARGV0")
                   ? getenv("PS3_ARGV0")
                   : "/app_home/USRDIR/EBOOT.BIN");

#ifdef _WIN32
    if (getenv("GT5P_HOSTMAP")) dump_hostmap();

    if (const char* secs = getenv("GT5P_MAINSTACK")) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                        GetCurrentProcess(), &g_main_thread,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0);
        CreateThread(nullptr, 0, main_stack_dump,
                     (LPVOID)(intptr_t)atoi(secs), 0, nullptr);
    }
#endif

#ifdef _WIN32
    if (char* g = getenv("GT5P_GUARD"))
        CreateThread(nullptr, 0, guard_arm, g, 0, nullptr);
#endif

    gt5p_start_present_thread();

    printf("[GT5P] Executing 0x%08X (TOC 0x%08X)...\n\n",
           entry_addr, (uint32_t)elf.toc);
    fflush(stdout);

    entry_func(&g_main_ctx);

    printf("\n[GT5P] Guest returned. r3=0x%llX\n",
           (unsigned long long)g_main_ctx.gpr[3]);

    vm_shutdown();
    return EXIT_SUCCESS;
}
