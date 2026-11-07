// The older SDK's target: Atelier Ryza 2 and the titles of its generation.
//
// Their entry does not hand nnMain to the runtime the way nn::init::Start does; it expects rtld to have brought nn::os
// up already, and when it returns the port has to run the module constructors and call nnMain itself. The order below is
// what the older SDK needs, established one fatal at a time (docs from the fork's porting notes):
//
//   1. nnosInitialize (exported by sdk): rtld's own step, which builds OsResourceManager -> ThreadManager -> TlsManager.
//      Without it the entry faults on a null `this` in every manager it touches.
//   2. the module constructors (main, subsdk0, sdk), which the Start path would have run for it.
//   3. nnMain.
#include "target.h"

#include "host.h"

void rt_boot_module_inits(Cpu *c);        // boot.c
void rt_boot_entry_callbacks(uint64_t out[3]);

// The older SDK's entry never receives the main thread handle rtld hands the newer one, so the main thread's ThreadType
// (the object at TLR+0x1F8) keeps a zero at +0x1B0. The SDK's user-mode critical sections and mutexes tag their lock word
// with that field, and a zero tag makes the main thread's locks look free to everyone else.
static void main_thread_tag(Cpu *c, uint32_t handle) {
    uint64_t self = LD64(c->tpidrro + 0x1F8);
    if (self > 0x10000 && LD32(self + 0x1B0) != handle) {
        rt_log("boot: main ThreadType %#llx handle field %#x -> %#x", (unsigned long long)self, LD32(self + 0x1B0), handle);
        ST32(self + 0x1B0, handle);
    }
}

// On hardware rtld gives the process its heap (svcSetHeapSize) before the entry runs, and the older SDK's allocator
// assumes it is there: without it the first large allocation answers NULL and the title zeroes it. Take the largest
// size the kernel accepts.
static void establish_heap(void) {
    static const uint64_t sizes[] = {4ull << 30, 2ull << 30, 1ull << 30, 512ull << 20, 256ull << 20};
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        uint64_t at = 0;
        if (mem_set_heap_size(sizes[i], &at) == 0) {
            rt_log("boot: heap %#llx established at %#llx, as rtld would", (unsigned long long)sizes[i],
                   (unsigned long long)at);
            return;
        }
    }
    rt_fatal("boot: no heap size the kernel accepts");
}

static void ryza2_guest_main(Cpu *c) {
    uint32_t handle = kern_main_thread(&c);
    establish_heap();
    char d[128];
    uint64_t init = ldr_symbol("nnosInitialize");
    if (!init) rt_fatal("boot: nnosInitialize is not exported");
    rt_log("boot: older SDK entry: nnosInitialize at %s (main thread %#x)", ldr_describe(init, d, sizeof d), handle);
    rt_call(c, init, 0);
    rt_log("boot: nnosInitialize returned");
    main_thread_tag(c, handle);
    // The game's own entry, called with the arguments nn::init::Start would take: it brings up libc (malloc's function
    // table is one of the things it fills) and runs the module constructors through the callbacks.
    uint64_t entry = ldr_symbol("nninitStartup");
    if (entry) {
        uint64_t cb[3];
        rt_boot_entry_callbacks(cb);
        rt_log("boot: older SDK entry: nninitStartup at %s", ldr_describe(entry, d, sizeof d));
        rt_call(c, entry, 5, (uint64_t)handle, (uint64_t)0, cb[0], cb[1], cb[2]);
        rt_log("boot: nninitStartup returned; running the module constructors and nnMain");
    }
    rt_boot_module_inits(c);
    uint64_t nnmain = ldr_symbol("nnMain");
    if (!nnmain) rt_fatal("boot: the entry returned and nnMain is not exported");
    rt_log("boot: ENTER nnMain at %s", ldr_describe(nnmain, d, sizeof d));
    rt_call(c, nnmain, 0);
    rt_fatal("boot: nnMain returned");
}

// The module constructor counts of the base title (main 85, subsdk0 44, sdk 109): the ExeFS sanity check.
static const RTInitCount g_ryza2_inits[] = {{"main", 85}, {"subsdk0", 44}, {"sdk", 109}};

// The mount table the title prints at start-up: ROOT: is the RomFS itself, DISC: and RES: are rom:/Data/NX/.
static const RTMountAlias g_ryza2_aliases[] = {
    {"ROOT:", ""}, {"DISC:", "Data/NX"}, {"RES:", "Data/NX"}, {NULL, NULL},
};

static const RTTarget g_ryza2 = {
    .name = "ryza2",
    .guest_main = ryza2_guest_main,
    .sdk_flags = RT_SDK_USER_CRITICAL_SECTIONS,
    .romfs_image_name = "romfs.bin",
    .mount_aliases = g_ryza2_aliases,
    .pool_min = 8ull << 30,   // 1.85 GB of graphics work memory does not fit in the retail pool
    .init_counts = g_ryza2_inits,
    .init_count_n = 3,
    .present_interval_override = 1,
};

const RTTarget *rt_target(void) { return &g_ryza2; }
