#!/usr/bin/env python3
"""Assemble the portable Linux folder: build/linux-<ver>/totk plus what it needs to run.

    tools/make_linux_release.py --version v121 [--out dist/totk-linux] [--with-libs] [--exefs DIR] [--romfs DIR]

The folder holds the executable, the shader translator (totk_xlate and its .id), a run.sh launcher that sets the
environment the port wants, totk.ini.example and a README. --with-libs copies the shared libraries the binary links
against that a SteamOS or Bazzite image may not carry (SDL3 above all) into lib/, and run.sh puts that on
LD_LIBRARY_PATH; libc, libstdc++ and the Vulkan loader are left to the system.

Game data is never copied: --exefs/--romfs (or TOTK_EXEFS/TOTK_ROMFS at run time) point at the user's own dump.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]

ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
ap.add_argument('--version', default='v121')
ap.add_argument('--build', type=Path, help='default build/linux-<version>')
ap.add_argument('--out', type=Path, help='default dist/totk-linux')
ap.add_argument('--with-libs', action='store_true', help='copy the non-system shared libraries into lib/')
ap.add_argument('--exefs', help='the ExeFS folder to bake into run.sh (optional)')
ap.add_argument('--romfs', help='the RomFS folder to bake into run.sh (optional)')
args = ap.parse_args()

build = args.build or root / 'build' / f'linux-{args.version}'
out = args.out or root / 'dist' / 'totk-linux'
exe = build / 'totk'
if not exe.is_file():
    sys.exit(f'{exe}: build it first with tools/build_linux.py')

out.mkdir(parents=True, exist_ok=True)
shutil.copy2(exe, out / 'totk')
os.chmod(out / 'totk', 0o755)

xlate = build / 'totk_xlate'
if not xlate.is_file():
    sys.exit(f'{xlate}: build it with tools/build_xlate.py --rid linux-x64')
for name in ('totk_xlate', 'totk_xlate.id'):
    shutil.copy2(build / name, out / name)

# The runtime reads the RomFS index and any overlay patches from beside the executable, so a bundle that is meant to run
# the game needs them; --prepare-shaders writes the shader cache there too, and it is rebuilt on demand.
for name in ('romfs-index.bin',):
    src = build / name
    if src.is_file():
        shutil.copy2(src, out / name)
if (build / 'romfs-patches').is_dir():
    shutil.copytree(build / 'romfs-patches', out / 'romfs-patches', dirs_exist_ok=True)

for name in ('totk.ini.example',):
    src = root / name
    if src.is_file():
        shutil.copy2(src, out / name)

if args.with_libs:
    lib = out / 'lib'
    lib.mkdir(exist_ok=True)
    want = ('libSDL3.so', 'libopus.so', 'libvpx.so', 'libzstd.so')
    seen = set()
    for line in subprocess.run(['ldd', str(exe)], capture_output=True, text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) < 3 or '=>' not in line:
            continue
        soname, path = parts[0], parts[2]
        if not soname.startswith(want) or not os.path.isfile(path) or soname in seen:
            continue
        seen.add(soname)
        shutil.copy2(path, lib / os.path.basename(path))
    print(f'lib/: {len(seen)} libraries')

run = out / 'run.sh'
env_lines = []
if args.exefs:
    env_lines.append(f'export TOTK_EXEFS="{args.exefs}"')
if args.romfs:
    env_lines.append(f'export TOTK_ROMFS="{args.romfs}"')
run.write_text('\n'.join([
    '#!/bin/sh',
    '# Launcher for the Linux port: keeps the runtime next to this script and passes everything on.',
    'set -e',
    'here=$(cd "$(dirname "$0")" && pwd)',
    'cd "$here"',
    'if [ -d "$here/lib" ]; then LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"; export LD_LIBRARY_PATH; fi',
    '# userfaultfd may be blocked by the distribution (vm.unprivileged_userfaultfd=0) or a container: the write watch',
    '# then falls back to the process-wide soft-dirty bits, which is slower but correct.',
    ': "${TOTK_WRITE_WATCH:=auto}"; export TOTK_WRITE_WATCH',
    *env_lines,
    'exec "$here/totk" "$@"',
    '']) )
os.chmod(run, 0o755)

# A desktop entry next to the launcher: %k is this file, so the entry keeps working wherever the folder is unpacked.
(out / 'totk.desktop').write_text('''[Desktop Entry]
Type=Application
Name=Tears of the Kingdom (native)
Comment=Native Linux port of the recompiled game
Exec=sh -c 'cd "$(dirname "$1")" && exec ./run.sh' sh %k
Terminal=false
Categories=Game;
''')

(out / 'README.txt').write_text(f"""The Legend of Zelda: Tears of the Kingdom, native Linux port ({args.version})

    ./run.sh --help             options
    ./run.sh --selftest         check the runtime without game files
    ./run.sh                    play

Requirements
  - x86-64 Linux with a Vulkan 1.3 driver (RADV on AMD, ANV on Intel, NVIDIA 550+)
  - SDL3 (bundled in lib/ when this folder was built with --with-libs)
  - your own copy of the game's ExeFS/RomFS, either in ./exefs and ./romfs next to this file, or named by
    TOTK_EXEFS / TOTK_ROMFS

Notes
  - the first start translates shaders on demand; after that ./shader-cache makes it quick
  - userfaultfd is used for the write watch when the kernel allows it; otherwise the port falls back to soft-dirty
    bits automatically (log line: "watch: ...")
  - logs: ./logs/totk.log
""")

print(f'{out}: totk, totk_xlate{" , lib/" if args.with_libs else ""}, run.sh, README.txt')
