"""Extract a title's add-on-content packs into the dlc/ tree the runtime serves.

Each DLC file (NSZ or NSP) holds one aocNCA whose RomFS is the pack's files, at the same paths the base
RomFS uses (Data/NX/..., and a marker like ITE00 or BGM01 at the root). The tool unpacks every file in
<DLC folder> whose name carries the title's aoc IDs ([0100B020117D....]), in title-ID order, into
numbered directories under the output: dlc/01, dlc/02, ... plus dlc/<NN>/aoc.json (index, title ID, file
name, counts) and dlc/aoc-manifest.json. The numbering is the order nn::aoc::ListAddOnContent reports and
nn::fs::MountAddOnContent mounts (fs_host.c), so it only has to be deterministic, not meaningful.

NSZ files are unpacked to NSP first (ncz.py, run with this interpreter). The ticket yields the titlekey,
which goes to hactool through the MNG_TITLE_KEYS environment the vendored scripts define - key values are
never printed. The aocNCA is the largest NCA in the package; hactool extracts its RomFS as a plain tree.

usage: extract_dlc.py <DLC folder> <keys file> <out dir> [--hactool FILE]
Runs under .venv-py314 (compression.zstd through ncz.py).
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools' / 'vendor' / 'macnativegames'))
from extract_exefs import common_tickets, container_entries, load_keys, nca_inventory, record_offset  # noqa: E402
from extract_update_romfs import copy_range, hactool_prod_keys  # noqa: E402

LONG_HEX = re.compile(r'[0-9A-Fa-f]{32,}')   # key material and hashes stay out of the console
AOC_ID = re.compile(r'\[(0100B020117D[0-9A-Fa-f]{4})\]')   # this title's add-on-content IDs


def die(message):
    sys.exit(f'extract_dlc: {message}')


def packs(folder):
    """[(title ID, path)] of the folder's DLC files, in title-ID order; NSP and NSZ both accepted."""
    rows = []
    for p in sorted(folder.iterdir()):
        m = AOC_ID.search(p.name)
        if not m:
            if p.suffix.lower() in ('.nsz', '.nsp'):
                print(f'  skip (no add-on-content id in its name): {p.name}')
            continue
        rows.append((m.group(1), p))
    rows.sort()
    return rows


def hactool_fail(log):
    return f'hactool failed: {LONG_HEX.sub("<hex>", log)[-300:]}'


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('folder', type=Path, help='the folder holding the DLC NSZ/NSP files')
    ap.add_argument('keys', type=Path, help='prod.keys')
    ap.add_argument('out', type=Path, help='the dlc/ directory the runtime reads (TOTK_AOC_DIR)')
    ap.add_argument('--hactool', type=Path, default=ROOT / 'toolchain' / 'src' / 'hactool' / 'hactool')
    args = ap.parse_args()
    if not args.folder.is_dir():
        die(f'{args.folder} is not a folder')
    if not args.keys.is_file():
        die(f'{args.keys} is not a file')

    ncz = ROOT / 'tools' / 'ncz.py'
    to_unpack = packs(args.folder)
    if not to_unpack:
        die(f'no DLC files with an add-on-content id under {args.folder}')
    keys = load_keys(args.keys)
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='dlc-', dir=args.out.parent) as tmp:
        tmp = Path(tmp)
        manifest = []
        for n, (title_id, path) in enumerate(to_unpack, 1):
            dest = args.out / f'{n:02d}'
            if dest.exists():
                shutil.rmtree(dest)
            dest.mkdir()
            nsp = path
            if path.suffix.lower() == '.nsz':
                nsp = tmp / 'pack.nsp'
                r = subprocess.run([sys.executable, str(ncz), str(path), str(nsp)], capture_output=True, text=True)
                if r.returncode != 0:
                    die(f'{path.name}: ncz.py failed: {LONG_HEX.sub("<hex>", r.stderr)[-200:]}')
            with nsp.open('rb') as f:
                _, entries = container_entries(f, nsp.stat().st_size)
                tickets = common_tickets(f, entries)
                inv = nca_inventory(f, entries, keys)
                rec = max((x for x, _ in inv), key=lambda x: x['size'])
            ticket_keys = tmp / 'title.keys'
            fd = os.open(ticket_keys, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, 'w') as tf:
                for rights, key in tickets.items():
                    tf.write(f'{rights.hex()} = {key.hex()}\n')
            hactool_keys = tmp / 'hactool.keys'
            hactool_prod_keys(args.keys, hactool_keys)
            nca = tmp / 'aoc.nca'
            copy_range(nsp, record_offset(rec), rec['size'], nca)
            r = subprocess.run([str(args.hactool), '-x', '-y', '--disablekeywarns', '--suppresskeys',
                                '-k', str(hactool_keys), '--romfsdir', str(dest), str(nca)],
                               env=dict(os.environ, MNG_TITLE_KEYS=str(ticket_keys)), capture_output=True, text=True)
            if r.returncode != 0 or '(FAIL)' in (r.stdout + r.stderr):
                die(f'{path.name}: {hactool_fail(r.stdout + r.stderr)}')
            os.remove(ticket_keys)
            files = [q for q in dest.rglob('*') if q.is_file()]
            entry = {'index': n, 'title_id': title_id, 'file': path.name, 'files': len(files),
                     'bytes': sum(q.stat().st_size for q in files)}
            (dest / 'aoc.json').write_text(json.dumps(entry, indent=2) + '\n')
            manifest.append(entry)
            print(f'  [{n:02d}/{len(to_unpack)}] {title_id} {len(files)} files, '
                  f'{entry["bytes"] / 2**20:.1f} MB -> {dest}')
    (args.out / 'aoc-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'extract_dlc: {len(manifest)} pack(s), {sum(m["bytes"] for m in manifest) / 2**20:.0f} MB total')


if __name__ == '__main__':
    main()
