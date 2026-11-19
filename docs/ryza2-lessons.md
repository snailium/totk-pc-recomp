# Atelier Ryza 2 Porting & 60 FPS Optimization Lessons

Summary of key engineering lessons from bringing *Atelier Ryza 2: Lost Legends & the Secret Fairy* (Switch v1.0.0) to 60 FPS on Linux / AMD BC-250 APU.

## 1. Older Switch SDK Boot & Threading Differences
- **Entry sequence**: Unlike newer SDKs that enter via `nn::init::Start`, Ryza 2 requires:
  1. Calling `nnosInitialize` first (builds `OsResourceManager`, `ThreadManager`, `TlsManager`).
  2. Establishing the guest heap early via `svcSetHeapSize` (`establish_heap()`), otherwise early allocations return `NULL`.
  3. Tagging the main thread's `ThreadType + 0x1B0` with its kernel handle (`main_thread_tag()`), otherwise user-mode critical sections treat the main thread's locks as free.
  4. Calling `nninitStartup`, running module `DT_INIT` constructors (`main`: 85, `subsdk0`: 44, `sdk`: 109), then entering `nnMain`.
- **User critical sections**: Enable `RT_SDK_USER_CRITICAL_SECTIONS` in `RTTarget` so condition variables can release and hand off kernel-owned locks properly.

## 2. 60 FPS Unlock Requires Multiple Engine Gates
A single Atmosphere cheat is not enough; Ryza 2 enforces 30 FPS across four independent layers:
1. **NVN presentation**: Set `present_interval_override = 1` and `vk_present = "mailbox"`.
2. **App present interval controller**: Clear `app+0x1d0 = 0`, `app+0x1d1 = 0`, and set `app+0x1d4 = 1`.
3. **Dynamic resolution / frame-time mode**: Switch the global mode flag from `3` to `0`.
4. **KTGL device & background loader**: Lock `ktgl_dev + 0x2a9c = 1` (`ktgl_dev + 0x2a29 = 0`), pre-seed `EXNowLoading` VSync accumulator (`1.0f`), and shorten its `20 ms` loop sleep to `8 ms` (`100 us` in `waitFirstDraw`).

## 3. Performance Bottlenecks & Pitfalls
- **Shadow memory write-back feedback loop (`~5 ms/frame`)**:
  - Skip shadow tracking/syncing for `NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS` pools.
  - Set `.shadow_wb_off = 1` so `note_written()` in `vk_exec_draw.c` does not mark `CPU_UNCACHED` SSBO/UBO ranges as GPU-written. Otherwise `vk_gpu_written()` copies `11.7 MiB/frame` back to guest memory, which triggers Linux `uffdio_writeprotect` dirty bits and `vk_guest_written()`, forcing `shadow_sync_pool()` to copy them right back into `r->smap` on the next submit and causing spurious texture re-uploads.
- **Worker wait loops & `SleepThread` overhead (`~2.5 ms/frame`)**:
  - **Animation/task worker wait (`f_7000aa5700`)**: Original code only branched to sleep on `state == 0`, causing the main thread to call `LockMutex` while the worker was still running (`state == 1`) and hit kernel futex waits. Patching to `state < 2` (`cmp w8, #2; b.lt`) and spinning in user space with `plat_pause()` eliminates both futex contention and `20 us` kernel sleeps.
  - **Render command-buffer worker wait (`f_7000aef9c0`) — CAUTION**: Do **not** force the main thread to spin until `state == 2`. When a worker's `+0x2c` skip flag is non-zero, it skips the job and never writes `state = 2`, causing a permanent hang/deadlock.
  - **1 ns tail-call sleep**: Skip `SleepThread(1 ns)` after `ConditionVariable::Signal`.
- **Profiling pitfall**: `TOTK_TRACE=perf` enables per-frame `dirty_frame` page scans (`~22 ms/frame` overhead). Always measure scheduling/blocking with `TOTK_TRACE=sched` only, and measure final FPS with tracing disabled.
- **Compiler optimization (`-O2` vs `-O1` on lifted C)**: Recompiling generated C at `-O2` yielded no FPS gain over `-O1` (`49` vs `50 FPS` in the heavy title 3D scene); remaining CPU cost is structural (imbalanced draw-recording workload on the last render worker).

## 4. Version-Independent Patching Architecture
- **Instruction patches**: Use wildcard AOB `pattern` rules in `patches/ryza2_60fps.txt` (`recomp/patches.py`) instead of editing generated C files.
- **Address/GOT offsets**: Use `tools/gen_ryza2_offsets.py` during `tools/build_game.py --target ryza2` to scan `main` NSO and emit `ryza2_offsets.h`, keeping `runtime/targets/ryza2.c` free of hardcoded v1.0.0 addresses.

## 5. SteamOS / Bazzite Game Mode
- Do not hardcode `export SDL_VIDEODRIVER=wayland` in `run.sh`; let SDL3 auto-select the backend so the same script works in both KDE Desktop Mode and Gamescope Game Mode.

## 6. UMA & AMD BC-250 APU Notes
- **Why Linux UMA still uses `shadow_on`**: On Linux `amdgpu`, re-protecting pages imported via userptr invalidates the GPU buffer (~1.3 ms per 16 MiB at the next submit). Keeping `shadow` buffers with `userfaultfd` + `PAGEMAP_SCAN` async write-watch avoids this, provided `CPU_NO_ACCESS` pools are skipped and `shadow_wb_off = 1` is set.
- **RAM / VRAM split (`bazzite-deck`)**: With a `6 GiB` VRAM / `9.4 GiB` RAM (`9.4 GiB` GTT) static split on BC-250, Ryza 2's working set (~`3.0 GB` RAM, `0.5–1.5 GB` VRAM) fits comfortably without triggering the Linux OOM killer.
- **Dual-CCX affinity**: Pinning guest threads to the first CCX (`0xff`) and host threads to the second CCX (`0xff00`) via `plat_cpu_groups` with `free_cores = 1` eliminated cross-CCX scheduling overhead (+5.7 FPS in `S04_T_CENTRAL`).

