#!/bin/sh
# Build the Linux port and run its self-test: the whole check a CI machine needs, no game files involved.
#
#     tools/ci_linux.sh [generated-dir]
#
# Needs clang, SDL3, Vulkan, opus, libvpx, zstd and the generated C (recomp/gen.py). The self-test covers the dispatch
# table, host functions, memory and IPC, and both write-watch trackers.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
# The generated C usually lives outside the checkout (it needs the game's own files): pass its directory, or point
# TOTK_GENERATED at it.
gen=${1:-${TOTK_GENERATED:-$root/generated/v121}}
if [ ! -f "$gen/layout.json" ]; then
    echo "no generated C at $gen (pass the directory: tools/ci_linux.sh /path/to/generated/v121)" >&2
    exit 2
fi
cd "$root"
python3 tools/build_linux.py --version v121 --generated "$gen" --jobs "${JOBS:-$(nproc)}"
./build/linux-v121/totk --selftest
