// Linux implementations of the pieces the Windows build keeps in gfx_window.c, input_win.c, prof_win.c, rdoc_win.c
// and swkbd_win.c.
//
// This is the first slice: the timing and bookkeeping the graphics path calls, and honest no-ops for the window,
// input and RenderDoc. It is enough for `totk --selftest` and for the headless probes; the SDL3 window, the Vulkan
// swapchain and the SDL3 input layer replace the no-ops in the next step (docs/linux.md, M3).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "flight.h"
#include "gfx.h"
#include "host.h"
#include "input.h"
#include "platform.h"
#include "vk.h"


// ---- input: nothing read yet
void input_poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    if (buttons) *buttons = 0;
    if (lx) *lx = 0;
    if (ly) *ly = 0;
    if (rx) *rx = 0;
    if (ry) *ry = 0;
}
void input_poll_ui(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    input_poll(buttons, lx, ly, rx, ry);
}
void input_block(int on) { (void)on; }
void input_menu(int open) { (void)open; }
int input_pad_read(uint64_t *buttons, int32_t *lx, int32_t *ly) {
    input_poll(buttons, lx, ly, NULL, NULL);
    return 1;
}
int input_pad_layout(void) { return -1; }
int input_pad_label(int pad) { (void)pad; return -1; }
int input_rumble(uint16_t low, uint16_t high, uint32_t ms) { (void)low; (void)high; (void)ms; return 0; }
uint16_t rumble_motor(float a, float curve, float strength) {
    (void)a; (void)curve; (void)strength;
    return 0;
}
int input_rumble_triggers_enabled(void) { return 0; }
void input_motion_start(void) {}
int input_motion_enabled(void) { return 0; }
void input_motion_axes(const float sdl_acc[3], const float sdl_gyro[3], double acc_g[3], double gyro[3]) {
    (void)sdl_acc; (void)sdl_gyro;
    memset(acc_g, 0, sizeof(double) * 3);
    memset(gyro, 0, sizeof(double) * 3);
}

// ---- RenderDoc: debug capture only
void rdoc_init(void) {}
int rdoc_active(void) { return 0; }
void rdoc_after_present(uint64_t frame) { (void)frame; }

// ---- profiler: the timing the graphics path records. The Windows file also owns the per-frame summary and the
// overlay hooks; here the counters are kept and nothing prints, so the overlay stays at "off".
static _Thread_local uint64_t t_begin;

uint64_t perf_begin(void) { return plat_now_ns(); }

void perf_end(int kind, uint64_t t0) { (void)kind; (void)t0; }
void perf_replay(uint64_t t0) { (void)t0; }
void perf_replay_thread(void) {}
void perf_main_thread(void) {}
void perf_count(int kind) { (void)kind; }
void perf_present(uint64_t frame) { (void)frame; }
void perf_summary_flush(void) {}
void perf_gpu_wait(uint64_t t0) { (void)t0; }
void perf_gpu_busy(uint64_t ns) { (void)ns; }
uint64_t perf_gpu_busy_total(void) { return 0; }
void perf_set_overlay(int mode) { (void)mode; }
int perf_gpu_timing(void) { return 0; }
void (*perf_gpu_stats)(PerfGpu *out);

void prof_start(void) {}
void prof_write(void) {}
int rt_ab(void) { return 0; }
int rt_knob(const char *name, const char *def) { (void)name; return def ? atoi(def) : 0; }
