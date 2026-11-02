#!/bin/sh
# Build the Linux port and run its self-test: the whole check a CI machine needs, no game files involved.
#
#     tools/ci_linux.sh [generated-dir]
#
# Needs clang, SDL3, Vulkan, opus, libvpx, zstd and the generated C (recomp/gen.py). The self-test covers the dispatch
# table, host functions, memory and IPC, and both write-watch trackers.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
gen=${1:-$root/generated/v121}
cd "$root"
python3 tools/build_linux.py --version v121 --generated "$gen" --jobs "${JOBS:-$(nproc)}"
./build/linux-v121/totk --selftest
