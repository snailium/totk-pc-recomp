# Native Linux port: what works, what does not, and how to continue

This is the report for the `linux-native` branch. `docs/linux.md` holds the situation and the phased TODO list; this file
is the short version: what the branch delivers, the evidence for each claim, the one thing that still blocks the game,
and the order in which to attack it.

## What the branch delivers

A native ELF (`build/linux-v121/totk`, 260 MiB) that runs without Wine or Proton, built from the same generated C and
runtime sources as the Windows build.

| Piece | Where | Evidence |
|---|---|---|
| Platform layer: threads, affinity, time, sleep, mutex/cond, files, memory, child processes | `runtime/platform/linux.c`, `runtime/platform/platform.h` | `--selftest` passes; the game boots through the SDK |
| Win32 compatibility shim for the runtime's Windows-shaped code (SRW locks, events, fibers, file mappings, `VirtualAlloc`, own-thread suspend/context, SHA-256, toolhelp, wide strings) | `runtime/platform/linux/win32_compat.{h,c}` | covered by `--selftest` and by the game's boot |
| Build path | `tools/build_linux.py` | builds 464 objects, links in seconds |
| SDL3 window and Vulkan WSI | `runtime/platform/linux/gfx_window_linux.c` | the port selects an AMD RX 7900 XTX through RADV, creates the device and pools, builds the ASTC decoder |
| Shader translator for Linux (NativeAOT) | `tools/build_xlate.py --rid linux-x64` | `totk_xlate` 3.5 MB, identity `7cbfdf6e5c96f621` |
| Write watch: userfaultfd `WP_ASYNC` + `PAGEMAP_SCAN` | `win32_compat.c` | log line `watch: userfaultfd write watch active`; `--selftest` covers written-page reporting and reset |
| Write watch fallback: soft-dirty (`clear_refs` + pagemap bit 55) | `win32_compat.c` | `--selftest` with `TOTK_WRITE_WATCH=softdirty` |
| Signal-based core preemption: priority inheritance and the preemption disable | `runtime/kernel.c` | log line `asynchronous preemption of virtual cores on, priority inheritance and preemption disable on`; interrupt lines appear in a normal run |
| Portable release folder | `tools/make_linux_release.py` | `dist/totk-linux` runs `--selftest` from its own directory and after being copied elsewhere; bundled SDL3 resolves through `LD_LIBRARY_PATH` |
| CI | `tools/ci_linux.sh`, `.github/workflows/linux.yml` | build + `--selftest`, no game files needed |
| glibc floor | build flags + a local `log10f` | `readelf -V` reports `GLIBC_2.38` as the highest version, which Bazzite (2.39-2.41) and SteamOS (2.38+) have |

The boot reaches, in order: RomFS index and image, the SDK start, `nnMain`, account, HID, audio renderer open, the save
mount and commit, the first Vulkan replay, the first vibration value, and the loading screen being rendered.

## What blocks the game

The boot stops on the loading screen. With the applet event signalled by hand (`TOTK_POKE_EVENT=0x1003`, see below) the
stop moves one step further and names itself:

```
1001 prio 44  InternalCriticalSectionImplByHorizon::Enter      wait: thread 0x1004 (join)
1004 prio 30  host nvn:nvnQueueSubmitCommands                  wait: a guest object at 0x133312120
```

The main thread joins the NVN submit thread; the submit thread never returns. The key it waits on never appears in
`TOTK_SYNC_LOG`, so it is a wait without a timeout that nothing in the port signals.

## Ruled out, with the experiment that ruled it out

| Hypothesis | Experiment | Result |
|---|---|---|
| The port is just slow | 400 s run, three samples 45 s apart | identical line count, identical allocation count |
| Shader translation | the translator process list during the stall | no translator alive |
| Audio thread starving the rest | audio renderer timer fix, `TOTK_AUDIO=none` | same stall |
| Vulkan recorder thread | `TOTK_VK_RECORDER=0` | same stall |
| Pipeline library / prebuild | `TOTK_GPL=0`, `TOTK_PIPE_PREBUILD=0` | same stall |
| Window focus | `TOTK_ASSUME_FOCUS=1` | same stall |
| A loader thread died | `TOTK_THREAD_LOG=1` | 51 created, 50 alive, one normal exit |
| Core preemption and scheduling | `TOTK_ASYNC_PREEMPT=0/1/2` | same stall |
| Lost wake-up on the process-wide key path | `TOTK_SYNC_LOG=1` | only the SDK's timer condition variable is active |
| The NVN memory callback | `TOTK_NVN_MEMCB=0` | same stall |
| Replay thread placement | `TOTK_REPLAY_ASYNC=1` | same stall |
| Number of virtual cores | `TOTK_FREE_CORES=1` | same stall |
| A stuck present drain in the submit path | two-second watchdog in `vk_present_drain` | it never fires: `pending`/`busy` are clear |

## The applet system event

Before the deadlock, the guest waits (with no timeout) on a system event it created itself with
`nn::os::CreateSystemEvent` in inter-process mode: the channel a console's system uses to send applet messages. The port
has no system, so nothing signals it, and the boot parks there.

`TOTK_POKE_EVENT=0x1003` (a host thread signalling that handle every 5 ms) gets past it. Note that the first version of
that diagnostic looked the handle up as a *writable* event while the guest waits on the *readable* end, which is why an
earlier run wrongly "ruled out" this event.

## How to continue

1. Find what should signal the object at `0x133312120` inside the submit thread. It is a guest object in the heap; the
   signaller must be guest code, so the question is which thread on a console reaches it and why that thread does not
   here. `TOTK_SYNC_LOG=1` plus a `kill -QUIT` dump of the deadlocked process is the tool.
2. Check for host/guest lock inversion: the main thread wants a guest critical section while the submit thread sits
   inside a host function, and host functions that enter guest code (the memory callback is the only one left) must not
   hold host locks while doing so.
3. Then, in order: `gfx: first present` in the log, a screenshot from the compositor, the title screen; first on `.101`,
   then on the BC-250 and the Steam Machine, both of which have to be online (they have answered `No route to host`
   throughout this work).

## Useful switches

- `TOTK_LASTCALLS=1`: per-thread ring of the last 512 lifted calls, printed by both crash reporters.
- `TOTK_LOGHOSTRET=1`: the value a host call leaves in x0.
- `TOTK_EVENT_LOG=1`: event create/signal/reset/wait with the guest caller, plus the host-side signals.
- `TOTK_SYNC_LOG=1`: process-wide key waits that time out and signals that wake nobody.
- `TOTK_THREAD_LOG=1`: each guest thread's exit.
- `TOTK_POKE_EVENT=<handle>`: signal an event from a host thread every 5 ms.
- `TOTK_NVN_MEMCB=0`: never enter the guest's command buffer memory callback.
- `TOTK_WRITE_WATCH=softdirty`: force the soft-dirty write-watch fallback.
- `kill -QUIT <pid>`: every guest thread with its wait state, plus the interrupted thread's call ring.
