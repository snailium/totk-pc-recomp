"""Extract any Switch base game's ExeFS and RomFS, for the recompiler's bring-up of a game other than TotK.

tools/extract.py is TotK-specific on purpose: it verifies every module against known hashes and refuses anything else.
This tool reuses the same vendored extractors (tools/vendor/macnativegames) with a generic Program-NCA rule instead: the
base Program NCA of a combined package is the content_type 0 entry whose file systems are not BKTR (an update's are).

  python tools/extract_game.py --game base.xci --keys prod.keys --out build/ryza2 [--exefs-only]

Output matches what the build expects: <out>/exefs/{main,main.npdm,rtld,sdk,...} and <out>/romfs/{index.json,romfs.bin}.
Keys are read by the vendored code and never printed.
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VENDOR = ROOT / 'tools' / 'vendor' / 'macnativegames'
sys.path.insert(0, str(VENDOR))
import extract_exefs  # noqa: E402
import extract_romfs  # noqa: E402


def generic_program_nca(records, update=False):
    """(record, header) of the base Program NCA: content_type 0, and no BKTR file system (which marks an update)."""
    programs = []
    for record, hdr in records:
        if record['content_type'] != 0:
            continue
        bktr = any(hdr[0x400 + i * 512 + 4] == 4 for i in range(4))
        if bktr == bool(update):
            programs.append((record, hdr))
    if len(programs) != 1:
        names = ', '.join(f"{r['name']} {r['title_id']} size {r['size']}" for r, _ in records)
        raise SystemExit(f'cannot pick one {"update" if update else "base"} Program NCA; candidates: {len(programs)} of [{names}]')
    return programs[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', type=Path, required=True, help='base game .xci or .nsp (combined packages are fine)')
    ap.add_argument('--keys', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True, help='directory that gets exefs/ and romfs/')
    ap.add_argument('--exefs-only', action='store_true', help='stop after the code modules')
    args = ap.parse_args()

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    exefs, romfs = out / 'exefs', out / 'romfs'
    manifest = out / 'extraction-manifest.json'

    # The vendored scripts keep their TotK checks in program_nca; both call it, so one replacement covers them.
    extract_exefs.program_nca = generic_program_nca
    extract_romfs.program_nca = generic_program_nca

    if not exefs.is_dir():
        argv = ['extract_exefs.py', str(args.game), '--keys', str(args.keys), '--output', str(exefs)]
        old, sys.argv = sys.argv, argv
        try:
            extract_exefs.main()
        finally:
            sys.argv = old
        print(f'exefs: {", ".join(sorted(p.name for p in exefs.iterdir()))}')
    else:
        print(f'exefs: already at {exefs}')
    if args.exefs_only:
        return

    if not romfs.is_dir():
        argv = ['extract_romfs.py', '--keys', str(args.keys), '--manifest', str(manifest), '--output', str(romfs)]
        old, sys.argv = sys.argv, argv
        try:
            extract_romfs.main()
        finally:
            sys.argv = old
    print(f'romfs: {romfs}')


if __name__ == '__main__':
    main()
