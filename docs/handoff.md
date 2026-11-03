# Handoff: native Linux port of TotK, and the road to 50 fps

Written at the end of a long session. Everything below is measured in this session unless it says otherwise; anything that
was not measured is marked as such. The branch is local (never pushed), the working tree is clean, and the last commit is
`d60a8aa` on `linux-native`.

## 1. Where everything is

| Thing | Location |
|---|---|
| Repository | `/home/gwang/deepseek-workspace/IT-Service/totk/totk-pc-recomp`, branch `linux-native` |
| Build host | `gwang@192.168.111.101`, container **`totk-dev-p`** (has `/dev/dri` and `SYS_PTRACE`, but see the rule below) |
| Build command | inside the container: `python3 tools/build_linux.py --version v121 --generated /totk/generated/v121 --romfs /totk/install/romfs --exefs /totk/install/exefs --jobs 8`, output `/totk/linux-src/build/linux-v121/totk` |
| Target machine | `bc250@192.168.111.144` (Bazzite 44, key `~/.dsh/keystore/bc250_144`, passwordless sudo) |
| Game data on the target | `~/totk/game/{exefs,romfs}` (16 GB), profile `~/totk/profile`, run folder `~/totk/dist-linux` (binary, `totk_xlate`, `lib/`, `romfs-index.bin`, `romfs-patches/`) |
| Launch wrapper | `~/totk/play.sh` sets `TOTK_EXEFS`/`TOTK_ROMFS`/`TOTK_PROFILE_DIR`/`TOTK_LANG`/Wayland vars and execs `dist-linux/run.sh` |

**Rule: do not use the 7900 XTX on `.101`.** It serves the production model endpoint. The container is for building only
(CPU); all game runs belong on `.144`. Game runs on `.101` were done earlier in the session and stopped when the owner
said so.

## 2. The goal and where it stands

**Goal:** 50+ fps sustained from this port on the BC-250.

**Today:** 12.6-12.7 fps with the default configuration (79 ms per frame). The game itself has already dropped to
`interval 3` (a 20 fps target) and runs at `speed ≈ 0.47` of real time — it is waiting for the port, not the reverse.

Hardware, verified: **8 cores / 16 threads** (DXEv3 firmware), **40/40 CU** (`bc250-cu-live-manager.sh status`; `dmesg`'s
`active_cu_number 24` is the boot topology and is *not* evidence either way), 386 GB/s measured memory bandwidth. The
hardware is not the limit.

## 3. What works

- Boots to the **title screen** (art, logo, menu), animated, presented through a real Wayland window
  (`gfx: window 1920x1080, swapchain 1920x1006`), captured with `spectacle` at 12.5-12.6 fps.
- `--selftest` passes in the build container and from the release folder; `readelf -V` reports `GLIBC_2.38` at most.
- Linux platform layer, Win32 shim, build path, SDL3 window + Vulkan WSI, SDL3 input layer, userfaultfd/soft-dirty write
  watch, signal-based preemption (priority inheritance and preemption disable; the rip redirect is still off by default),
  portable release folder, CI.

## 4. The frame budget (measured, `TOTK_WAIT_LOG=1`)

| Cost | Per frame | Instrumented at |
|---|---|---|
| replay recording | 31 ms | `waitstat_add("replay record", ...)` in `runtime/vk_sync.c` `vk_sync_submit` |
| GPU submit (blocked) | 20 ms | `waitstat_add("submit2 ioctl", ...)`, same function |
| present blit (second submit) | 23 ms | `waitstat_add("blit submit", ...)` in `runtime/platform/linux/gfx_window_linux.c` `vk_present_blit` |

Everything else measured is small: present lock 0.0 ms, acquire 0.1 ms, pacing 0.0 ms, queue present 0.1 ms, upload flush
0.0 ms, write-watch epoch 0.0 ms (it is off by default now), guest waits a few ms each.

Frame content (`TOTK_TRACE=gfx`): about **130 render passes, 280-330 draws, 55 clears, 19 dispatches** and seven 1080p
targets per frame, all in one submission. The pass machinery itself is only 2.6 ms per 60 frames, so the recording time is
in the draw/bind handlers (≈100 µs per draw, which is far too much).

## 5. What is ruled out, each with the experiment that ruled it out

| Hypothesis | Experiment | Result |
|---|---|---|
| Non-recursive critical sections self-deadlock | native stack `vk_staging <- upload <- vk_texture_by_id_locked <- shader_for` | **the actual boot deadlock**; fixed with `PTHREAD_MUTEX_RECURSIVE` (SRW stays non-recursive) |
| Thread-handle poll never reports a finished thread | boot prewarm of 187 shaders hung | fixed: `WaitForSingleObject(..., 0)` probes once before the deadline |
| Window never created | `gfx_window_start` was only called from the unused `vk_present_image` | fixed: `vk_present_blit` starts it; the window now comes up on the first present |
| Write-watch epoch reset | `clear_refs(4)` per submission cost **483 ms per call** | off by default (`TOTK_WATCH_EPOCH=1` to re-enable); 8 fps -> the fix was worth 3-4x |
| OS write watch cannot reset on Linux | `TOTK_WATCH=self` (port's own stamps) | made the Linux default: 4.3 -> 12.7 fps; `TOTK_WATCH=watch` restores the old behaviour |
| Empty present submit | removed in the synchronous path | no fps change; submits are GPU-throttled (total submit time constant) |
| Mirror full re-upload | `TOTK_MEMORY=0` (import mode) | no change (12.4 vs 12.7 fps) |
| Asynchronous replay | `TOTK_REPLAY_ASYNC=1` | no change (12.6 fps) |
| Command buffer splitting | `TOTK_SPLIT_DRAWS=0` / `1024` | no change |
| Preemption cost | `TOTK_ASYNC_PREEMPT=0` | 12.1 vs 12.6 fps, within noise |
| Guest spin on SVCs | `TOTK_SVC_LOG=1` histogram | dominated by SleepThread (~680/frame), all cheap; not the frame |
| GPU harvested to 24 CU | `bc250-cu-live-manager.sh status` | **wrong** — 40/40 active, WGP3/WGP4 routed via SPI (mask `0x1f`, CC `0xffe00000`) |

## 6. What to do next, in this order

1. **Fold the present blit into the replay's submission.** Today the present thread does its own `vkQueueSubmit2`
   (`present_main` in `runtime/vk_backend.c` -> `vk_present_blit` in `runtime/platform/linux/gfx_window_linux.c`), plus a
   timeline value for ordering. Recording the blit at the end of the replay's command buffer removes one submission and one
   queue wait per frame; expected up to 20 ms. Note `TOTK_PRESENT_TEST=1` (clear instead of blit) was added to price the
   blit, but the run aborted at startup with a one-line log and that path was never fixed — fix it first if you want the
   measurement.
2. **Counter-based profile of the draw handler.** A tail-to-head timestamp inside the op loop reads the wrong clock here
   (it produced absurd per-op times), so accumulate *counters* per phase instead: pipeline resolve, descriptor write,
   binding, `vkCmdDraw*`. Start at `vk_exec_op_draw` / `op_draw_idx` in `runtime/vk_exec_draw.c`. 100 µs per draw is the
   single most suspicious number in the whole budget.
3. **Cut GPU work.** 23 ms of blit wait means the GPU has a full frame of work at 1080p. Look for repeated full-screen
   passes and attachments that are loaded and stored without being read.
4. **Only then** the mirror: the dirty census says about 106 MiB per submission is uploaded because the mirror's dirty test
   (`page_dirty` in `runtime/vk_memory.c`) still falls back to `mem_written_peek` on an OS write watch whose bits never
   clear on Linux.

Target: 20 ms per frame.

## 7. Switches and tools that already exist

| Switch | What it does |
|---|---|
| `TOTK_WAIT_LOG=1` | per-site blocking table every 60 presents (the frame budget above) |
| `TOTK_TRACE=gfx` | per-frame passes/draws/clears/copies/targets |
| `TOTK_TRACE=perf` | texture re-uploads, dirty-page census, write-watch cost |
| `TOTK_SVC_LOG=1` | SVC histogram |
| `TOTK_PRESENT_LOG=1` | present statistics, presented texture state, SDL events |
| `TOTK_PRESENT_TEST=1` | clear the swapchain instead of blitting (see step 1: currently aborts) |
| `TOTK_BLIT_DUMP=<present #>` | write the image the blit is handed to `/tmp/blit_src.ppm` |
| `TOTK_WATCH=self` / `watch` | freshness from the port's stamps (Linux default) or the OS write watch |
| `TOTK_WATCH_EPOCH=1` | re-enable the `clear_refs` epoch (slow, 483 ms/call) |
| `TOTK_MEMORY=0` | import pools instead of mirroring |
| `TOTK_REPLAY_ASYNC=1`, `TOTK_SPLIT_DRAWS=n`, `TOTK_ASYNC_PREEMPT=0` | replay thread, split threshold, preemption |
| `kill -QUIT <pid>` | every guest thread with its wait state and its last lifted calls |
| `sudo eu-stack -p <pid>` | symbolised native stacks (this is how the `vk_staging` deadlock was found) |

## 8. Pitfalls learned the hard way

- `pkill -f <path>` kills the invoking shell; use `pkill -9 -x totk`.
- The build container skips a file whose `.o` is newer than the source: after copying sources in, `touch` them or delete
  the matching `.o`, otherwise you will test a stale binary (this happened and produced a wrong conclusion once).
- Run the game only through `~/totk/dist-linux/run.sh` (it sets `LD_LIBRARY_PATH`); the bare `./totk` fails on `libvpx`.
- Screenshots: `spectacle -b -n -o /tmp/shot.png` with `XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0
  DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus`.
- Key injection works: `ydotoold` (root, `--socket-path=/run/user/1000/.ydotool_socket --socket-perm=0666`) plus
  `YDOTOOL_SOCKET=... ydotool key <scan>:1 <scan>:0`. The default bindings are `E` = A, `Space` = X, `LShift` = B, so
  Enter does nothing on the title screen.
- The game is on a KDE session; it sometimes receives `SDL_EVENT_QUIT` from the compositor when a frame misses the ping.
  The port logs it once and keeps the surface.
- `TOTK_PRESENT_TEST=1` currently aborts during startup — fix before relying on it.

## 9. Repo hygiene

- Game data never enters the repo; `python tools/check_content.py --staged` is the gate CI uses and it passes.
- Commits are plain sentences without type prefixes; code, comments and docs are in English, conversation is Chinese.
- `docs/linux.md` is the long-running situation/TODO document; `docs/linux-port-report.md` is the shorter report; this file
  is the entry point for whoever continues.
