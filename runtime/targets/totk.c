// The TotK target: the newest SDK, which the shared files were written against.
#include "target.h"

// boot.c: nn::init::Start(main thread handle, arguments region, loader_ready, module_inits, module_finis), which runs
// DT_INIT of main, subsdk0 and sdk and then nnMain.
void rt_boot_guest_main_init_start(Cpu *c);

static const RTTarget g_totk = {
    .name = "totk",
    .guest_main = rt_boot_guest_main_init_start,
    .sdk_flags = 0,
    .romfs_image_name = "romfs.bin",
    .pool_min = 0,         // the retail pool
    .init_counts = NULL,   // the shared TotK tables in boot.c
    .init_count_n = 0,
};

const RTTarget *rt_target(void) { return &g_totk; }
