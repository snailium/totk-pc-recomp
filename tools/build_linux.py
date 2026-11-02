#!/usr/bin/env python3
"""Build the Linux executable: generated C + runtime -> build/linux-<ver>/totk.

The Windows build (tools/build_game.py) writes a Ninja file for MSYS2. This script is its Linux counterpart: same
sources, same flags, system clang and the distribution's SDL3, Vulkan, opus, libvpx and zstd.

Usage:
    tools/build_linux.py --version v121 [--generated generated/v121] [--jobs N] [--exefs DIR] [--romfs DIR]

The first run needs generated C from recomp/gen.py (which also writes layout.h). --exefs/--romfs bake the default game
paths into the executable; they default to build/linux-<ver>/../.. and are only used when the command line or
TOTK_EXEFS does not name them.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CC = os.environ.get('CC', 'clang')
GEN_FLAGS = ['-O1', '-fno-strict-aliasing', '-Wno-return-type', '-Wno-unused-label', '-fvisibility=hidden',
             '-march=x86-64-v3']
RT_FLAGS = ['-O2', '-g', '-Wall', '-fno-strict-aliasing', '-march=x86-64-v3']
# The Linux build replaces these with platform/linux.c, platform/linux/win32_compat.c and the SDL3 window layer.
WINDOWS_ONLY = {'gfx_window.c', 'gfx_window_probe.c', 'input_win.c', 'swkbd_win.c', 'prof_win.c', 'rdoc_win.c'}
INCLUDES = ['-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'runtime' / 'platform'),
            '-I', str(ROOT / 'runtime' / 'platform' / 'linux'), '-I', str(ROOT / 'runtime' / 'third_party' / 'imgui'),
            '-I', str(ROOT / 'runtime' / 'third_party' / 'vma')]


def c_string(s):
    return '"' + str(s).replace('\\', '\\\\').replace('"', '\\"') + '"'


def module_glue(layout, hashes, exefs, romfs, profile, version):
    """The same glue tools/build_game.py writes: layout table, linked function tables, hashes, default paths."""
    out = ['// Module glue written by tools/build_linux.py; do not edit.',
           '#include "dispatch.h"', '#include "host.h"', '#include "layout.h"', '']
    for name, base, size in layout:
        u = name.upper()
        out.append(f'_Static_assert(MOD_{u}_BASE == {base:#x}ull && MOD_{u}_SIZE == {size:#x}ull, '
                   f'"layout.h and layout.json disagree on {name}");')
    out += ['', 'const RtLayoutEntry rt_layout[] = {']
    out += [f'    {{"{n}", MOD_{n.upper()}_BASE, MOD_{n.upper()}_SIZE}},' for n, _, _ in layout]
    out += ['};', f'const int rt_layout_count = {len(layout)};', '']
    for name, _, _ in layout:
        out += [f'extern const uint64_t {name}_fn_addrs[];', f'extern const GuestFn {name}_fn_ptrs[];',
                f'extern const uint64_t {name}_fn_count;']
    out += ['', 'int rt_linked_modules(RtModule *out, int max) {', '    int n = 0;']
    for name, _, _ in layout:
        u = name.upper()
        out.append(f'    if (n < max) out[n++] = (RtModule){{"{name}", MOD_{u}_BASE, MOD_{u}_SIZE, {name}_fn_addrs, '
                   f'{name}_fn_ptrs, {name}_fn_count}};')
    out += ['    return n;', '}', '']
    out += ['const RtFileHash rt_exefs_sha256[] = {']
    out += [f'    {{{c_string(f)}, {c_string(h)}}},' for f, h in sorted(hashes.items())]
    out += ['};', f'const int rt_exefs_sha256_count = {len(hashes)};', '']
    for name, value in (('exefs', exefs), ('romfs', romfs), ('profile', profile)):
        out.append(f'const char rt_default_{name}[] = {c_string(Path(value).as_posix())};')
    number = {'v0': 100, 'v112': 112, 'v121': 121, 'v143': 143}.get(version, 0)
    out += [f'const int rt_game_version = {number};', 'const char rt_port_version[] = "linux-native";', '']
    return '\n'.join(out)


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    return r.returncode, r.stdout + r.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', default='v121')
    ap.add_argument('--generated', type=Path, default=None)
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--exefs', type=Path)
    ap.add_argument('--romfs', type=Path)
    args = ap.parse_args()

    gen = (args.generated or ROOT / 'generated' / args.version).resolve()
    out = ROOT / 'build' / f'linux-{args.version}'
    obj = out / 'obj'
    obj.mkdir(parents=True, exist_ok=True)
    layout = [(m['name'], int(m['base'], 0), int(m['size'], 0))
              for m in json.loads((gen / 'layout.json').read_text())]
    # expected-v<display>.json: v121 -> 1.2.1
    display = {'v0': '1.0.0', 'v112': '1.1.2', 'v121': '1.2.1', 'v143': '1.4.3'}[args.version]
    expected = json.loads((ROOT / 'tools' / 'data' / f'expected-v{display}.json').read_text())
    hashes = expected['exefs_sha256']

    exefs = (args.exefs or out / 'exefs').resolve()
    romfs = (args.romfs or out / 'romfs').resolve()
    profile = (ROOT / 'build' / ('profile' if args.version == 'v121' else f'profile-{args.version}')).resolve()
    (out / 'modules.c').write_text(module_glue(layout, hashes, exefs, romfs, profile, args.version), encoding='utf-8')
    # romfs_config.h: the real RomFS identity when an extracted RomFS is next to the build, a named placeholder
    # otherwise (the executable builds either way; fs_init stops at start when the placeholder is in).
    sys.path.insert(0, str(ROOT / 'tools'))
    import romfs_index
    if (romfs / 'index.json').is_file():
        romfs_index.write(romfs, out)
    else:
        romfs_index.write_placeholder(out)

    sources = []
    for f in sorted((ROOT / 'runtime').glob('*.c')):
        if f.name in WINDOWS_ONLY:
            continue
        sources.append((f, RT_FLAGS))
    sources.append((ROOT / 'runtime' / 'platform' / 'linux.c', RT_FLAGS))
    sources.append((ROOT / 'runtime' / 'platform' / 'linux' / 'win32_compat.c', RT_FLAGS))
    sources.append((ROOT / 'runtime' / 'platform' / 'linux' / 'gfx_linux.c', RT_FLAGS))
    sources.append((ROOT / 'runtime' / 'platform' / 'linux' / 'gfx_window_linux.c', RT_FLAGS))
    sources.append((ROOT / 'runtime' / 'vk_vma.cpp', RT_FLAGS))   # VMA is C++: compiled with clang++ below
    sources.append((out / 'modules.c', RT_FLAGS))
    for sub in ('main', 'subsdk0', 'sdk'):
        for f in sorted((gen / sub).glob('*.c')):
            sources.append((f, GEN_FLAGS))
    sources = [(f, flags) for f, flags in sources if f.is_file()]

    def compile_one(item):
        f, flags = item
        name = (str(f.relative_to(ROOT)) if f.is_relative_to(ROOT) else f.name).replace(os.sep, '_')
        o = obj / (name + '.o')
        if o.exists() and o.stat().st_mtime > f.stat().st_mtime:
            return None
        if f.suffix == '.cpp':
            cmd = ['clang++', '-std=c++17', '-c', *flags, *INCLUDES, '-I', str(gen), str(f), '-o', str(o)]
        else:
            cmd = [CC, '-std=gnu11', '-c', *flags, *INCLUDES, '-I', str(gen), '-I', str(out), '-include',
                   str(ROOT / 'runtime' / 'platform' / 'linux' / 'win32_compat.h'), str(f), '-o', str(o)]
        code, log = run(cmd)
        return None if code == 0 else (f, log)

    t0 = time.time()
    failures = []
    done = 0
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for res in pool.map(compile_one, sources):
            done += 1
            if res:
                failures.append(res)
            if done % 50 == 0:
                print(f'  {done}/{len(sources)} objects, {time.time() - t0:.0f}s', flush=True)
    if failures:
        for f, log in failures[:5]:
            print(f'\n--- {f}\n{log[-3000:]}', file=sys.stderr)
        print(f'{len(failures)} file(s) failed', file=sys.stderr)
        return 1

    objs = sorted(str(p) for p in obj.glob('*.o'))
    exe = out / 'totk'
    cmd = [CC, '-fuse-ld=lld', '-o', str(exe), *objs,
           '-Wl,-T,' + str(ROOT / 'runtime' / 'platform' / 'linux' / 'sections.ld'),
           '-lSDL3', '-lvulkan', '-lopus', '-lvpx', '-lzstd', '-lpthread', '-ldl', '-lm', '-lstdc++']
    code, log = run(cmd)
    if code:
        print(log[-6000:], file=sys.stderr)
        return 1
    print(f'{exe}: {len(objs)} objects, {exe.stat().st_size / (1 << 20):.0f} MiB, {time.time() - t0:.0f}s')
    return 0


if __name__ == '__main__':
    sys.exit(main())
