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
void vm_write32(uint64_t addr, uint32_t val);   /* big-endian guest store */
void gt5p_spu_register_all(void);         /* generated: src/gen/spu_workloads.c */
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

    printf("[GT5P] Executing 0x%08X (TOC 0x%08X)...\n\n",
           entry_addr, (uint32_t)elf.toc);
    fflush(stdout);

    entry_func(&g_main_ctx);

    printf("\n[GT5P] Guest returned. r3=0x%llX\n",
           (unsigned long long)g_main_ctx.gpr[3]);

    vm_shutdown();
    return EXIT_SUCCESS;
}
