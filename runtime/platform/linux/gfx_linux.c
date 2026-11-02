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
#include "input_map.h"
#include "platform.h"
#include "vk.h"


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
