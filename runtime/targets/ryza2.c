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

void rt_boot_module_inits(Cpu *c);   // boot.c

static void ryza2_guest_main(Cpu *c) {
    uint32_t handle = kern_main_thread(&c);
    char d[128];
    uint64_t init = ldr_symbol("nnosInitialize");
    if (!init) rt_fatal("boot: nnosInitialize is not exported");
    rt_log("boot: older SDK entry: nnosInitialize at %s (main thread %#x)", ldr_describe(init, d, sizeof d), handle);
    rt_call(c, init, 0);
    rt_log("boot: nnosInitialize returned; running the module constructors and nnMain from here");
    rt_boot_module_inits(c);
    uint64_t nnmain = ldr_symbol("nnMain");
    if (!nnmain) rt_fatal("boot: the entry returned and nnMain is not exported");
    rt_log("boot: ENTER nnMain at %s", ldr_describe(nnmain, d, sizeof d));
    rt_call(c, nnmain, 0);
    rt_fatal("boot: nnMain returned");
}

// The module constructor counts of the base title (main 85, subsdk0 44, sdk 109): the ExeFS sanity check.
static const RTInitCount g_ryza2_inits[] = {{"main", 85}, {"subsdk0", 44}, {"sdk", 109}};

static const RTTarget g_ryza2 = {
    .name = "ryza2",
    .guest_main = ryza2_guest_main,
    .sdk_flags = RT_SDK_USER_CRITICAL_SECTIONS,
    .romfs_image_name = "romfs.bin",
    .init_counts = g_ryza2_inits,
    .init_count_n = 3,
};

const RTTarget *rt_target(void) { return &g_ryza2; }
