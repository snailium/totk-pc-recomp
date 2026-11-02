#!/bin/sh
# Syntax-check every runtime source against the Linux headers and report the ones that still fail.
#
# The Linux port touches ~60 runtime files; this script is the scoreboard: run it after each change and watch the
# failure count go down. It compiles nothing to completion and needs no game files.
#
#   tools/linux_compile_matrix.sh [--all]     # --all includes the Windows-only files the Linux build replaces
#
# Requires clang, and the Vulkan, SDL3, XCB and VMA headers (see docs/linux.md, "Build").
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
cc=${CC:-clang}
inc="-I $root/runtime -I $root/runtime/platform -I $root/runtime/platform/linux"
inc="$inc -I $root/runtime/third_party/imgui -I $root/runtime/third_party/vma"
ok=0
fail=0
for f in "$root"/runtime/*.c; do
    case $(basename "$f") in
    # Windows-only implementations the Linux build replaces with platform/linux.c and the SDL3 window
    gfx_window.c | gfx_window_probe.c | input_win.c | swkbd_win.c | prof_win.c | rdoc_win.c) [ "${1:-}" = "--all" ] || continue ;;
    esac
    err=$($cc -fsyntax-only -std=gnu11 $inc "$f" 2>&1 | grep -m1 "error:")
    if [ -z "$err" ]; then
        ok=$((ok + 1))
    else
        fail=$((fail + 1))
        echo "$(basename "$f"): $(echo "$err" | cut -c1-160)"
    fi
done
echo "OK=$ok FAIL=$fail"
[ "$fail" -eq 0 ]
