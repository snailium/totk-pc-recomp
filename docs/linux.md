# Linux port

Goal: build `totk` as a native Linux ELF that runs without Wine or Proton, on Bazzite (BC-250, APU/gfx1013) and on
SteamOS (self-built Steam Machine, RX 9070). Windows stays the reference build; the Linux path must not regress it.

## Status

- Windows is the only supported host today (`docs/building.md`). Linux and the Steam Deck are in the README's
  *Planned* list.
- The platform split exists but is unfinished: `runtime/platform/platform.h` declares the OS surface the runtime is
  allowed to use, `runtime/platform/win32.c` is its only implementation, and `tools/check_platform.py` keeps an
  allowlist of the files that still call Win32 directly. Those entries (tagged `2.7a` memory/flt/page tracking,
  `2.7b` threads/time/locks/FP state) are the worklist.
- `docs/architecture.md` says the Vulkan back end already "runs on Linux"; only windowing, input, the platform layer
  and the build are Windows-bound today.

## What the Proton experiment already proved (2026-10-31, `.101`)

The Windows build was produced and run under Wine 11, which answers the questions a port would otherwise have to
answer from scratch:

| Question | Result |
|---|---|
| Do the fixed guest addresses work off Windows? | Yes — `--selftest` passed (3 modules, 301,848 functions, 22.5 M dispatch slots) |
| Does `VK_EXT_external_memory_host` accept host pointers through a translation layer? | Yes — import, bind and map of an anonymous 1 MiB host buffer returned the written bytes (`RESULT: PASS`) |
| Does the write-watch design survive a non-Windows page tracker? | Yes — Wine 11's `userfaultfd` path reports every written page; 256 MiB written + queried in **50 ms** (vs 280 ms through mprotect traps) |
| Does the game boot off Windows? | Yes — 360 frames rendered on llvmpipe (no DRI3 in the headless container, so no GPU), ASTC on the compute path, GPL fast-link active |
| What breaks a translation layer? | `ucrtbase.crealf` (Wine 10 stub) kills the lifter's numpy; fixed in Wine 11 |

The last row is the argument for a native port: every one of those interactions is an emulation detail a native build
does not have.

## Where the seams are

- `runtime/platform/platform.h` — 45 functions: threads, affinity, CPU count, monotonic time, sleep, mutex/cond,
  memory commit/read, ~15 file calls, exe path, environment, UI language, thread id, font path, OS version, stderr
  path, clipboard, open folder, `dlopen`, machine load (CPU/GPU/thermal/memory), child processes.
- `runtime/platform/win32.c` — 684 lines, the reference implementation of all of it.
- `runtime/platform/fp_x86.h` — MXCSR access, already arch-only, not OS-specific.
- `tools/check_platform.py` — the allowlist of remaining direct Win32 users (41 files, ~1,575 call sites, probes
  included). Excluding probes, the real runtime is ~30 files.

## Porting inventory

Counted from the runtime (probe files excluded where they only test):

| Area | Representative calls | Replacement |
|---|---|---|
| Threads | `CreateThread`×34, `GetCurrentThreadId`×12, `SwitchToThread`×5 | `pthread_create`, `gettid`, `sched_yield` behind `plat_*` |
| Time | `QueryPerformanceCounter`×17, `timeBeginPeriod` | `clock_gettime(CLOCK_MONOTONIC)` behind `plat_now_ns` |
| Files | `CreateFileW`×15, `ReadFile`×26, `WriteFile`×9 | POSIX `open/read/write/pread` behind `plat_file_*` |
| Memory | `VirtualAlloc`×7, `VirtualProtect`×8 | `mmap(MAP_FIXED_NOREPLACE)`, `mprotect` |
| Write watch | `GetWriteWatch`×3, `MEM_WRITE_WATCH` | userfaultfd `WP_ASYNC` → soft-dirty → mprotect traps |
| Preemption | `SuspendThread`×7, `SetThreadContext`×2, `GetThreadContext` | `SIGUSR1` + `ucontext` (RIP/RSP rewrite) |
| Child processes | `CreateProcessW`×6 | `fork` + pipes + `execv` |
| Window/input | `CreateWindowExW`, `GetMessageW`, `GetKeyboardState`, `SetCursor` | SDL3 (`SDL_Vulkan_CreateSurface`, SDL events); `input_map.c` is already platform-neutral |
| Shell extras | clipboard, open folder, profiler, RenderDoc loader | `wl-copy`/`xclip`, `xdg-open`, optional |

## Why a Win32 compatibility layer

The runtime calls the Windows API directly in ~60 files (`SRWLOCK`, `InterlockedIncrement`, `VirtualAlloc`,
`CreateFileW`, `SuspendThread`…). Rewriting all of them onto `platform.h` is the target design but it is weeks of work
before anything links. The Linux branch therefore adds `runtime/platform/linux/win32_compat.{h,c}` plus shim headers
named `windows.h`, `psapi.h`, `bcrypt.h`, `tlhelp32.h`, `timeapi.h` and `io.h`, all resolved only when
`-I runtime/platform/linux` is on the include path.

Rules for this layer:

- It is Linux-only and never linked into the Windows build.
- It forwards to POSIX and keeps Windows semantics where the runtime depends on them (lazy zero-initialised locks,
  `GetWriteWatch` with `RESET`, `SuspendThread`/`SetThreadContext` over `ucontext`).
- It is a bridge, not an API: files move to `platform.h` as they are touched, and the shim shrinks with them
  (`tools/linux_compile_matrix.sh` is the scoreboard).

## The three hard parts

1. **Write watch.** Three implementations, selected at startup and logged, because the fallback changes frame time by
   an order of magnitude:
   - `userfaultfd` with `UFFD_FEATURE_WP_ASYNC` + `PAGEMAP_SCAN`/`PAGE_IS_WRITTEN` (kernel 5.19+/6.8+, and
     `UFFD_USER_MODE_ONLY` so `vm.unprivileged_userfaultfd=0` does not matter);
   - soft-dirty via `/proc/self/clear_refs` + `/proc/self/pagemap` (slower, always available);
   - mprotect + `SIGSEGV` traps (correct, worst).
   Measured on `.101`: 50 ms vs 280 ms for 256 MiB of first writes, so the selection must be visible in the log.
2. **Core preemption.** `SuspendThread`/`SetThreadContext` becomes `tgkill(SIGUSR1)`; the handler inspects
   `ucontext`'s RIP, and when it is in lifted code at host depth 0 rewrites RIP/RSP to `kern_ap_stub` and returns.
   The existing "two loads when nobody waits" fast path and `TOTK_ASYNC_PREEMPT=0` stay.
3. **Page freeze/thaw.** `VirtualProtect(PAGE_READONLY)` + the vectored handler becomes `mprotect` + a `SIGSEGV`
   handler that must distinguish the game's own aliasing faults from real bugs (the Windows VEH already has that
   distinction; port the same rule).

## Distribution

- **glibc floor is the only real per-distribution constraint.** Build on the oldest target (Ubuntu 22.04 container,
  glibc 2.35) or ship the Steam Linux Runtime; a binary built on Ubuntu 26.04 (glibc 2.43) will not start on older
  SteamOS. Bazzite 44 and Ubuntu 26.04 both have the kernel and Mesa features the port needs.
- **Immutable systems** (Bazzite, SteamOS): ship a portable folder — ELF, `lib/` with SDL3/opus/libvpx/zstd, `exefs/`,
  `romfs/`, caches — the same shape as the Windows `package_build.py` output. No system package installs.
- Both targets run Wayland (KDE on Bazzite, gamescope on SteamOS); SDL3 covers X11/Wayland and gamepad input.
- Do not depend on distribution SDL3/Vulkan headers at run time; build against the understood versions and bundle.

## TODO

Acceptance for each item is a command that fails before and passes after.

### M0 — scaffolding (this branch)
- [x] `docs/linux.md` (this file) and the TODO list
- [x] branch `linux-native`, no changes to Windows behaviour
- [x] `tools/build_linux.py`: clang + lld, pkg-config libraries, ELF; writes the same `modules.c` glue as
      `build_game.py`, compiles 463 objects (runtime + 385 generated chunks) and links `build/linux-<ver>/totk`
- [x] `tools/linux_compile_matrix.sh` — the scoreboard for the runtime port (66 of 70 sources compile clean; the
      other four need the generated `layout.h`/`romfs_config.h`)
- [x] `runtime/third_party/vma/vk_mem_alloc.h` vendored (3.5.0, MIT) so the Vulkan files build without an MSYS2 package
- [x] `tools/build_xlate.py --rid linux-x64` publishes `totk_xlate` for Linux (NativeAOT); the translator path is
      built with the platform separator and file name, so the game finds it next to `totk`
- [ ] `tools/check_platform.py --root . --target linux` reports progress against the allowlist

### M1 — platform layer
- [x] `runtime/platform/linux.c`: threads, affinity, CPU count, monotonic time, sleep, mutex/cond (zero-init valid),
      memory commit/read, all `plat_file_*`, exe path, env, UI language, thread id, font path, OS version, stderr
      path, clipboard, open folder, `dlopen`, machine load, child processes
- [x] `tests/platform_test.c` exercising threads, cond, atomic write, append, child pipes, memory read
- [x] the platform layer compiles clean with `-Wall -Wextra` under gcc, ASan run of the test (clang still to check in CI)

### M2 — runtime off Win32
- [x] Win32 compatibility layer for the bring-up (`win32_compat.{h,c}` and the shim headers), documented above
- [x] `vk.h` picks XCB instead of Win32 for the presentation surface; `VkNativeWindow` replaces `HWND`;
      `vk_device.c` asks SDL3 (`SDL_Vulkan_GetPresentationSupport`) instead of `vkGetPhysicalDeviceWin32…`
- [x] every non-generated runtime source compiles against the Linux headers (66/70; the rest need the generated files)
- [x] fibres over `ucontext` (`CreateFiberEx`/`SwitchToFiber`), event objects, `SetFileInformationByHandle`,
      vectored exception handlers wired into the `SIGSEGV` path so the freeze/thaw handshake in `memory.c` works
- [ ] replace the direct Win32 calls of the ~30 runtime files with `plat_*` (files, threads first, they are mechanical)
- [x] memory: reservation with `MAP_FIXED_NOREPLACE`, commit with `mprotect`, decommit zeroes through
      `MADV_DONTNEED`, `VirtualQuery` reports reservations as `MEM_RESERVE`
- [ ] loader: ELF-free (modules are already NSO→C); only `MAP_FIXED` and the dispatch table remain
- [x] `totk --selftest` passes as a Linux ELF against the 1.2.1 modules (`build/linux-v121/totk`)

### M3 — window, input, audio, graphics
- [x] SDL3 window + `SDL_Vulkan_CreateSurface`, swapchain, blit and present (`platform/linux/gfx_window_linux.c`)
- [ ] input: SDL events → the existing `input_map` key ids; gamepad/rumble already SDL3 (still the no-op stubs)
- [ ] audio: SDL3 output already; `audren` host path unchanged
- [ ] shader translator child process through `plat_child_*`; shader cache paths
- [x] the renderer initialises on real hardware: with a headless Wayland compositor (`weston --backend=headless`)
      the port picks the RX 7900 XTX through RADV, creates the device, mirrors its pools and builds the ASTC decoder
- [x] async preemption is active: the linker now puts all generated code after the runtime, so the "lifted code"
      address range no longer swallows host functions (`lifted code exe+0x1e037f0-exe+0xee9f2e0`), and `OpenThread`
      answers for the calling thread even when it was not created through `CreateThread`
- [x] positional reads: `ReadFile`/`WriteFile` now honour the `OVERLAPPED` offset. The runtime reads every RomFS file
      through it, so without the fix every read returned whatever the file position happened to be — the layout
      archives parsed into nothing, which is what produced the NULL object in `f_70011266c4`
- [x] the RomFS overlay applies: `RomFS ready: 304473 files, 1 overlays` after `tools/make_romfs_patches.py` writes
      `romfs-patches/` next to the binary (the installer's prepare step does this on Windows)
- [x] async preemption dispatches interrupts (`kernel: interrupt 0x1009 p47 ...`), as on Windows
- [x] the boot reaches the same stage as the Windows build: audio (`audren: renderer open`), the save mount, the first
      Vulkan replay and the game's first vibration value. Three portability bugs stood in the way: `GetFileAttributesW`
      failed without setting a Win32 error (so a missing directory looked like `ERROR_ALREADY_EXISTS`), paths built with
      backslashes did not resolve, and a fresh profile (no committed save) made the save working copy fail
- [ ] **async preemption: the interruption half corrupts guest state.** `TOTK_ASYNC_PREEMPT=2` (priority inheritance
      and the preemption disable, no redirects) boots like `=0`; `=1` redirects owners into `kern_ap_stub` and the guest
      dies soon after `hid: controllers initialized`. Two hazards are fixed (a signal frame written below the suspended
      thread's stack pointer destroyed its red zone — handlers now run on an alternate stack, and the stub reserves the
      red zone): the remaining one is in the redirect itself and needs the redirect log (`g_ap_log`) plus a reproduce.
      Until then the port defaults to `=2` (the default lives in `cfg.c`, so `totk.ini` and the environment still
      override it); `=1` turns the redirects on
- [x] the guest issues draw commands: after the vibration init the port runs the renderer (`nvnCommandBufferSetViewport`,
      `BindBlendState`, `nvnQueueFenceSync`, ... thousands of calls), so the boot reaches the drawing stage
- [x] the present path logs (`gfx: first present (swapchain WxH)` and a rate line every 120 presents), which is the
      cheapest way to tell "drawing" from "presenting": the port issues thousands of first-time nvn calls but never
      reaches `nvnQueuePresentTexture`, and `--prepare-shaders` populates the shader cache so the pipeline library
      prebuild that gated the Windows build is not paid on every run
- [ ] confirm that frames reach the compositor and then the title screen, on `.101` and on BC-250 Bazzite — the boot now loads content (`/RSDB/*.rstbl.byml.zs`,
      `/Pack/*.pack.zs`) and stops in the SDK's own `memcpy` (`f_70007bfa6c`/`f_70007bfa94`, deterministic, from
      `f_70007bd0b0`). The copy overruns its source: `si_addr` is `0x737265cc`, inside the heap but past the source
      object, so the length or the end pointer the caller passes is wrong. Next: read the argument setup in
      `f_70007bd0b0` (it takes its source end from `sp+0xa0`) and the caller `f_7000a68dd4`, which is where the size
      comes from — blocked on a display with DRI3: `.101` has
      no connected output and Xvfb has no DRI3, so the port boots with `
      vk: no usable Vulkan device` and stops at graphics init

### Notes from the first native boot

`totk` now boots as a Linux ELF through `nn::init::Start`, `nnosInitialize`, `nninitInitializeSdkModule`, `nninitStartup`,
`nnMain`, the account/HID setup and the RomFS mount. Two ELF-specific problems were found and fixed on the way:

- **Host-function and observer tables.** `host.h` brackets the tables with markers in `.ovr$a`/`.ovr$z` and puts the
  entries in `.ovr$m`. COFF keeps those adjacent; ELF keeps them as separate output sections in input order, so the
  registry came out empty and no override fired (the boot sequence diverged at `nnosInitialize`). Fixed with
  `runtime/platform/linux/sections.ld`, a linker script that coalesces `.ovr$a .ovr$m .ovr$z` and the `.obs` triple.
- **`CreateDirectoryW` must report `ERROR_ALREADY_EXISTS`**, not succeed silently: `ov_system.c`'s profile setup
  creates each path prefix and checks that code.

### Write watch: userfaultfd, with the trap flavour as a fallback

`win32_compat.c` now implements the kernel's dirty-bit tracking: `userfaultfd` with `UFFD_FEATURE_WP_ASYNC` registers
each watched range write-protected asynchronously (writes never fault) and `PAGEMAP_SCAN`/`PAGE_IS_WRITTEN` reads the
pages written since the last scan, `PM_SCAN_WP_MATCHING` re-arming them. That is the same mechanism Wine uses and it
coexists with `VK_EXT_external_memory_host`, the guest stack and the alias region.

The trap flavour (mprotect `PROT_READ` + `SIGSEGV`) stays as the fallback when userfaultfd cannot be opened. It must not
be armed for the whole watched block — the block holds the guest stack and every GPU-imported pool, and read-only pages
break both — so `g_watch_traps` stays 0 and `GetWriteWatch` reports the range as untracked, which callers treat as "every
page written": slower, never stale.

`userfaultfd` needs `--security-opt seccomp=unconfined` in Docker (the default profile blocks the syscall); the log line
`watch: userfaultfd write watch active (WP_ASYNC, kernel dirty bits)` confirms which path is in use.

### M4 — memory semantics
- [x] write watch: userfaultfd `WP_ASYNC` + `PAGEMAP_SCAN` path, logged at startup as
      `watch: userfaultfd write watch active`; trap fallback stays disarmed (see above), soft-dirty still to do
- [ ] preemption: `SIGUSR1` + `ucontext`; verify `TOTK_ASYNC_PREEMPT=0/1` both work
- [ ] freeze/thaw: `mprotect` + `SIGSEGV`; alias page table unchanged
- [ ] compare frame time with the Windows build on the same scene, same GPU

### M5 — packaging and targets
- [ ] portable Linux folder (ELF + `lib/` + `exefs/` + `romfs/`), `totk.ini` defaults, desktop entry
- [ ] glibc 2.35 build container; optional Steam Runtime manifest
- [ ] BC-250: Bazzite, RADV gfx1013, 16 GB APU — boots to field, log shows the write-watch mode and the GPU
- [ ] Steam Machine: SteamOS, RX 9070, gamepad — same, plus overlay/menu check
- [ ] CI: build the Linux ELF and run `--selftest` (no game files needed)

## Debugging notes

- `TOTK_LASTCALLS=1` keeps the last 512 lifted-code calls of each thread and both crash reporters print the newest 32
  with their arguments; a guest fault in a leaf routine has no usable frame chain and this ring still names the path.
- `TOTK_LOGHOSTRET=1` logs the x0 a host call leaves, which separates "a host function returned NULL" from "guest state
  was never initialised".
- The Windows build is a usable oracle: Wine 11's Wayland driver runs it on the real GPU in a container with a headless
  Weston (`--security-opt seccomp=unconfined --device /dev/dri`), boots in seconds rather than the minutes llvmpipe
  needs, and its host-call trace diffs cleanly against the port's. After `hid: controllers initialized` the two agree
  for 98 calls; Windows then calls `nn::os::AllocateAddressRegion`.
- gdb needs `--cap-add=SYS_PTRACE` to attach inside a container. The lifted code has no frame pointers, so its
  backtraces stop at the host frame; the runtime's own tracing is the useful tool there.
- Background runs started with `&`/nohup survive the shell and share the profile directory and the GPU — check
  `pgrep -x totk` before a run. `pkill -f <path>` also matches the shell running it (its command line holds the path),
  so use `pkill -x totk`.

## Non-goals

- No Wine/Proton shim in the native path.
- No new upstream features (HDR, VRR, ultrawide) before the two targets run.
- Windows behaviour must stay byte-for-byte buildable; `tools/check_content.py` rules still apply.
