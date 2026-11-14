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
#include "platform/platform.h"
#include "ryza2_offsets.h"

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

static void ryza2_hook_exnowloading_thread(Cpu *c);
static GuestFn g_orig_exnowloading_thread;
static uint64_t g_exnowloading_ptr;
static void ryza2_hook_app_main_loop(Cpu *c);
static GuestFn g_orig_app_main_loop;
static void ryza2_hook_ktgl_set_present_interval(Cpu *c);
static GuestFn g_orig_ktgl_set_present_interval;
static uint64_t g_ktgl_dev;

// Unlock 60 FPS across all of Ryza 2's internal frame-rate judgment points:
//   1. App present interval controller (f_70009cfea0 at 0x70009d00ec & f_7000aef860 at 0x7000aef8dc):
//      In shipping code, App (at [0x7001a7a658], GOT [0x70019a2e00]) initializes app+0x1d1 = 1 (dynamic 30/60 mode)
//      and app+0x1d4 = -1 (fixed interval override). Every frame a 3D scene or title menu is drawn, f_7000aef860
//      sets *(uint8_t *)(app + 0x1d0) = 1, which makes f_70009cfea0 set KTGL present interval (ktgl_dev + 0x2a9c)
//      to 2 (30 FPS). Setting app+0x1d1 = 0 and app+0x1d4 = 1 makes f_70009cfea0 ignore app+0x1d0 and lock the
//      present interval to 1 (60 FPS).
//   2. Dynamic resolution / frame-time threshold mode (0x700198f254 via GOT [0x70019a6578]):
//      Shipping .data initializes this to 3 (forced 30 FPS mode in f_7000a84540/f_7000a847a0/f_7000a84820/f_7000a84fc0).
//      Setting it to 0 switches the frame-time controller to 60 FPS mode.
//   3. Background loading / save-screen thread loop (EXNowLoading, f_70009ce970 at 0x70009ce970):
//      Waits for 2.0 VSyncs (30 FPS) and calls SleepThread(20 ms) every iteration (~38 FPS cap), while main-thread
//      wait loops (0x70009cec40, 0x70009ced58, 0x70009cee64) poll at 1 ms. Handled by hooking f_70009ce970 and
//      observing _ZN2nn2os11SleepThreadENS_8TimeSpanE.
//   4. KTGL Graphics Device Present Interval (ktgl_dev + 0x2a9c):
//      Directly sets and holds ktgl_dev+0x2a9c = 1 so all swapchain presents, sync waits, and pacing loops target 60 Hz.
static void ryza2_apply_60fps_state(void) {
    const uint64_t main_base = 0x7000000000ull;
    uint64_t mode_ptr = LD64(main_base + RYZA2_OFF_DYN_RES_MODE_GOT);
    if (mode_ptr > 0x10000 && LD32(mode_ptr) == 3u) {
        ST32(mode_ptr, 0u);
        rt_log("ryza2: dynamic resolution mode 3 -> 0 (60 FPS)");
    }
    uint64_t app_slot = LD64(main_base + RYZA2_OFF_APP_GOT);
    if (app_slot > 0x10000) {
        uint64_t app = LD64(app_slot);
        if (app > 0x10000) {
            static bool app_logged = false;
            if (!app_logged) {
                rt_log("ryza2: app=%#llx, locking interval override app+0x1d0=0, app+0x1d1=0, app+0x1d4=1",
                       (unsigned long long)app);
                app_logged = true;
            }
            ST8(app + 0x1d0ull, 0u);
            ST8(app + 0x1d1ull, 0u);
            ST32(app + 0x1d4ull, 1u);
        }
    }
    uint64_t ktgl_dev = g_ktgl_dev;
    if (ktgl_dev > 0x10000) {
        static bool ktgl_logged = false;
        if (!ktgl_logged) {
            rt_log("ryza2: ktgl_dev=%#llx, locking ktgl+0x2a9c=1, ktgl+0x2a29=0", (unsigned long long)ktgl_dev);
            ktgl_logged = true;
        }
        ST32(ktgl_dev + 0x2a9cull, 1u);
        ST8(ktgl_dev + 0x2a29ull, 0u);
    }
}

static void ryza2_hook_exnowloading_thread(Cpu *c) {
    g_exnowloading_ptr = c->x[0];
    ryza2_apply_60fps_state();
    if (g_orig_exnowloading_thread) g_orig_exnowloading_thread(c);
}

static void ryza2_hook_app_main_loop(Cpu *c) {
    ryza2_apply_60fps_state();
    if (g_orig_app_main_loop) g_orig_app_main_loop(c);
    ryza2_apply_60fps_state();
}

static void ryza2_hook_ktgl_set_present_interval(Cpu *c) {
    g_ktgl_dev = c->x[0];
    ryza2_apply_60fps_state();
    c->x[1] = 1;
    if (g_orig_ktgl_set_present_interval) g_orig_ktgl_set_present_interval(c);
}

static void obs_ryza2_yield_thread(Cpu *c) {
    ryza2_apply_60fps_state();
    rt_original(c);
}
HOST_OBSERVER("_ZN2nn2os11YieldThreadEv", obs_ryza2_yield_thread);

static void obs_ryza2_sleep_thread(Cpu *c) {
    ryza2_apply_60fps_state();
    const uint64_t main_base = 0x7000000000ull;
    uint64_t lr = c->x[30];
    if (lr == main_base + RYZA2_OFF_EXNOWLOADING_SLEEP_LR) {
        // EXNowLoading thread loop (f_70009ce970):
        // Note: in lifted C, `x19` is cached in a local C variable in `f_70009ce970` and `c->x[19]` is clobbered
        // by callees inside the loop, so we use `g_exnowloading_ptr` captured on entry to `f_70009ce970`.
        // Pre-seed accumulated VSync counter *(float *)(ex + 0x14) = 1.0f so a single 60 Hz tick reaches the
        // 2.0f threshold, and shorten the 20 ms end-of-iteration sleep (x0 = 20,000,000 ns) to 8 ms so the
        // background spinner runs at 60 FPS without starving the main thread of mutex `ex + 8`.
        uint64_t ex = g_exnowloading_ptr;
        if (ex > 0x10000 && LD8(ex + 0x10ull) != 0u && LD8(ex + 0x12ull) == 0u && LD32(ex + 0x14ull) == 0u) {
            ST32(ex + 0x14ull, 0x3f800000u);   // 1.0f
        }
        if (c->x[0] == 20000000ull) {
            c->x[0] = 8000000ull;              // 8 ms
        }
    } else if (lr == main_base + RYZA2_OFF_EXNOWLOADING_WAIT_LR) {
        // EXNowLoading::waitFirstDraw (f_70009cedc0): main thread waits for the loading thread to enter its draw
        // body (`*(ex + 0x11) != 0`). With the loading thread drawing at 60 Hz (~2 ms draw window per 16.6 ms),
        // polling at 1 ms can miss the window for many frames; poll at 100 us instead so the main thread catches
        // the draw window immediately.
        if (c->x[0] == 1000000ull) {
            c->x[0] = 100000ull;               // 100 us
        }
    } else if (lr == main_base + RYZA2_OFF_ANIM_WORKER_SLEEP_LR) {
        // Animation/physics worker wait loop (f_7000aa5700): main thread polls worker slots in user space.
        plat_pause();
        return;
    } else if (c->x[0] == 1ull) {
        // f_70009e2ef0 tail-calls SleepThread(1 ns) after every worker ConditionVariable::Signal.
        // Skip the redundant 1 ns sleep after waking a worker thread.
        return;
    }
    rt_original(c);
}
HOST_OBSERVER("_ZN2nn2os11SleepThreadENS_8TimeSpanE", obs_ryza2_sleep_thread);

static const RTFuncHook g_ryza2_hooks[] = {
    {"main", RYZA2_OFF_EXNOWLOADING, ryza2_hook_exnowloading_thread, &g_orig_exnowloading_thread},
    {"main", RYZA2_OFF_APP_MAIN_LOOP, ryza2_hook_app_main_loop, &g_orig_app_main_loop},
    {"main", RYZA2_OFF_KTGL_SET_PRESENT, ryza2_hook_ktgl_set_present_interval, &g_orig_ktgl_set_present_interval},
};

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
    ryza2_apply_60fps_state();
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
    .on_present = ryza2_apply_60fps_state,
    .func_hooks = g_ryza2_hooks,
    .func_hook_n = (int)(sizeof g_ryza2_hooks / sizeof *g_ryza2_hooks),
    .split_draws_off = 1,
    .feedback_off = 1,
    .shadow_wb_off = 1,
    .free_cores = 1,
    .frame_queue = 1,
    .replay_async_off = 1,
    .vk_present = "mailbox",
};

const RTTarget *rt_target(void) { return &g_ryza2; }
