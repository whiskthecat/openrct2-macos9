#!/usr/bin/env python3
"""Turn your own RollerCoaster Tycoon files into the .pak files the Mac reads.

  python3 tools/make_packs.py SOURCE [SOURCE ...] OUTPUT-FOLDER [--no-sound]

Each SOURCE is a game folder or a .zip of one. Give exactly one of:
  - RollerCoaster Tycoon Classic: the Steam/Windows folder (with Assets/) or the
    macOS app ("RCT Classic.app")
  - RollerCoaster Tycoon 2: the CD, Steam or GOG installation (with Data/g1.dat)
plus, optionally, RollerCoaster Tycoon 1 (Steam, GOG or CD; with Data/csg1i.dat),
which adds the RCT1 scenarios, graphics and music. Several folders of the same game
are merged, for example an RCT1 installation and its CD.

Sounds and music are converted to formats the Sound Manager plays directly. That needs
ffmpeg and ffprobe on PATH; --no-sound skips them (the game is then silent).

The packs contain commercial game data: keep them for your own use.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PART_MB = 56
CLASSIC_DATA = {'.pob', '.td6', '.sea', '.sv6', '.dat'}  # objects, designs, scenarios, g1.dat
ORIGINAL_DIRS = {'data', 'objdata', 'scenarios', 'tracks'}


def children(folder):
    return {p.name.lower(): p for p in folder.iterdir()} if folder.is_dir() else {}


def classify(folder):
    """Return (kind, root) for one game folder, searching a few levels down."""
    candidates = [folder, *sorted((p for p in folder.rglob('*') if p.is_dir()), key=lambda p: len(p.parts))]
    for root in candidates[:2000]:
        names = children(root)
        data = children(names['data']) if 'data' in names else {}
        if 'g1.dat' in data:
            return 'rct2', root
        scenarios = children(names['scenarios']) if 'scenarios' in names else {}
        if {'csg1i.dat', 'csg1.dat', 'csg1.1'} & set(data) or any(n.endswith('.sc4') for n in scenarios):
            return 'rct1', root
        if 'g1.dat' in names and any(p.suffix.lower() == '.pob' for p in root.iterdir()):
            return 'classic', root
    return None, folder


def gather(kind, root, files):
    """Add (path inside the pack, file) pairs for one source."""
    if kind == 'classic':
        for p in root.iterdir():
            if p.is_file() and p.suffix.lower() in CLASSIC_DATA | {'.ogg'}:
                files.setdefault('Assets/' + p.name, p)
        return
    roots = [root]
    if kind == 'rct1':
        # Steam's RCT1 keeps the installed half of the game in a subfolder.
        roots += [p for p in root.iterdir() if p.is_dir() and p.name.lower() == 'rctdeluxe_install']
    for base in roots:
        for top in base.iterdir():
            if top.is_dir() and top.name.lower() in ORIGINAL_DIRS:
                for p in top.rglob('*'):
                    name = p.relative_to(base).as_posix()
                    if p.is_file() and (kind != 'rct1' or rct1_needed(name)):
                        files.setdefault(name, p)


def rct1_needed(name):
    """OpenRCT2 reads RCT1's scenarios, track designs, CSG1 graphics, music and Mega Park."""
    folder, _, file = name.lower().partition('/')
    if folder != 'data':
        return True
    return file.startswith(('csg1.', 'csg1i.', 'css')) or file == 'mp.dat'


def pack(files, stage, out, sound, cache, only_ext=None):
    """Write files (a dict) through mkpak.py; returns the part files in order."""
    if stage.exists():
        shutil.rmtree(stage)
    for name, source in files.items():
        if only_ext is not None and Path(name).suffix.lower() not in only_ext:
            continue
        target = stage / name
        target.parent.mkdir(parents=True, exist_ok=True)
        try:
            os.symlink(source.resolve(), target)
        except OSError:  # Windows without symlink rights
            shutil.copyfile(source, target)
    command = [sys.executable, str(HERE / 'mkpak.py'), str(out), str(stage), '--split-mb', str(PART_MB)]
    if sound:
        command += ['--audio-cache', str(cache)]
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
    parts, stem = [out], out.with_suffix('')
    while (part := Path(f'{stem}{len(parts) + 1}.pak')).exists():
        parts.append(part)
    return parts


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('sources', nargs='+', metavar='SOURCE')
    parser.add_argument('output')
    parser.add_argument('--no-sound', action='store_true')
    args = parser.parse_args()
    sound = not args.no_sound
    if sound and not (shutil.which('ffmpeg') and shutil.which('ffprobe')):
        sys.exit('Sound conversion needs ffmpeg and ffprobe on PATH (or pass --no-sound)')

    with tempfile.TemporaryDirectory() as scratch:
        scratch = Path(scratch)
        found = {'classic': {}, 'rct2': {}, 'rct1': {}}
        for number, source in enumerate(args.sources):
            source = Path(source).expanduser()
            if source.suffix.lower() == '.zip':
                unpacked = scratch / f'zip{number}'
                with zipfile.ZipFile(source) as archive:
                    archive.extractall(unpacked)
                source = unpacked
            if not source.is_dir():
                sys.exit(f'{source}: not a folder or .zip')
            kind, root = classify(source.resolve())
            if kind is None:
                sys.exit(f'{source}: no RollerCoaster Tycoon data found (looked for Assets/g1.dat, '
                         'Data/g1.dat or Data/csg1i.dat)')
            print(f'{source}: {dict(classic="RCT Classic", rct2="RCT2", rct1="RCT1")[kind]} data in {root}')
            gather(kind, root, found[kind])
        if bool(found['classic']) == bool(found['rct2']):
            sys.exit('Give exactly one of RollerCoaster Tycoon Classic or RollerCoaster Tycoon 2 '
                     '(OpenRCT2 needs one of them); RCT1 data is an optional addition.')
        lower = {n.lower(): n for n in found['rct1']}
        if 'data/csg1.dat' in lower and 'data/csg1.1' in lower:
            del found['rct1'][lower['data/csg1.1']]  # the CD's copy of the installed CSG1.DAT
        if found['rct1'] and 'data/csg1i.dat' not in lower:
            sys.exit('The RCT1 data is incomplete: Data/CSG1I.DAT is missing.')

        output = Path(args.output)
        output.mkdir(parents=True, exist_ok=True)
        for old in [*output.glob('game*.pak'), *output.glob('rct1*.pak')]:
            old.unlink()
        cache = scratch / 'audio'
        if found['classic']:
            # Data first, then sounds, so each part stays a manageable size.
            parts = pack(found['classic'], scratch / 'stage', scratch / 'data.pak', False, cache, CLASSIC_DATA)
            if sound:
                print('Converting sounds (a few minutes)')
                parts += pack(found['classic'], scratch / 'stage', scratch / 'sound.pak', True, cache, {'.ogg'})
            names = ['game.pak' if n == 1 else f'game{n}.pak' for n in range(1, len(parts) + 1)]
        else:
            if sound:
                print('Converting music (a few minutes)')
            parts = pack(found['rct2'], scratch / 'stage', scratch / 'game.pak', sound, cache)
            names = ['game.pak' if n == 1 else f'game{n}.pak' for n in range(1, len(parts) + 1)]
        if found['rct1']:
            rct1 = pack(found['rct1'], scratch / 'stage', scratch / 'rct1.pak', sound, cache)
            parts += rct1
            names += ['rct1.pak' if n == 1 else f'rct1-{n}.pak' for n in range(1, len(rct1) + 1)]
        for part, name in zip(parts, names):
            shutil.move(part, output / name)
            print(f'  {name}  {(output / name).stat().st_size / 1e6:.0f} MB')
    print(f'Done: copy the .pak files from {output} into the folder with the RCT application.')


if __name__ == '__main__':
    main()
