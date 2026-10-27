#!/usr/bin/env python3
"""Pack a directory tree into one file for the Mac OS 9 build (see compat/classic/ClassicVfs.h).

  mkpak.py OUT.pak ROOT [--skip-ext .ogg,.wav] [--prefix Assets]

Format: "RCTPAK1\\0", u32 count, then per file u32 offset, u32 size, u32 name length, name;
all integers little-endian, offsets absolute, names are '/'-separated paths.
"""
import argparse, os, struct

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('out'); parser.add_argument('root')
parser.add_argument('--skip-ext', default='')
parser.add_argument('--prefix', default='', help='path prefix inside the pack')
parser.add_argument('--split-mb', type=int, default=0, help='start a new part (OUT2.pak, OUT3.pak, ...) before exceeding this size')
parser.add_argument('--only', default='', help='comma-separated substrings; when given, .sea/.sv6 files must match one')
parser.add_argument('--only-ext', default='', help='comma-separated extensions; when given, only these files are packed')
parser.add_argument('--audio-cache', default='', help='convert .ogg sounds and WAV music (.dat) for the Mac Sound Manager, keeping results in this folder')
args = parser.parse_args()
skip = {e.lower() for e in args.skip_ext.split(',') if e}
only_ext = {e.lower() for e in args.only_ext.split(',') if e}
files = []
for directory, _, names in os.walk(args.root):
    for name in names:
        if os.path.splitext(name)[1].lower() in skip:
            continue
        if only_ext and os.path.splitext(name)[1].lower() not in only_ext:
            continue
        full = os.path.join(directory, name)
        relative = os.path.relpath(full, args.root).replace(os.sep, '/')
        files.append(((args.prefix + '/' if args.prefix else '') + relative, full))
files.sort()

def transcode(full):
    """Ogg Vorbis is too expensive to decode on a G3 while the game runs. Sounds are stored instead
    in a form the Mac's Sound Manager plays directly: 16-bit big-endian samples for short sounds,
    IMA 4:1 (mono) for long ones such as ride music. Header, big-endian: 'RCTA', format
    ('twos' or 'ima4'), sample rate, channels, bits, sample frames; then the sound data."""
    import hashlib, subprocess
    stat = os.stat(full)
    key = hashlib.sha1(f'{full}:{stat.st_size}:{stat.st_mtime_ns}:v2'.encode()).hexdigest()
    out = os.path.join(args.audio_cache, key + '.rcta')
    if os.path.exists(out):
        return out
    probe = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'a:0', '-show_entries',
                            'stream=channels,sample_rate:format=duration', '-of', 'csv=p=0', full],
                           capture_output=True, text=True, check=True).stdout.split()
    rate, channels = [int(v) for v in probe[0].split(',')[:2]]
    duration = float(probe[1].strip(','))
    rate = min(rate, 22050)
    if duration * rate * channels * 2 > 1.5 * 1024 * 1024:
        os.makedirs(args.audio_cache, exist_ok=True)
        temporary = os.path.join(args.audio_cache, key + '.aifc')  # a file: the muxer has to seek back to fill in sizes
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', full, '-ac', '1', '-ar', str(rate), '-c:a', 'adpcm_ima_qt',
                        '-f', 'aiff', temporary], check=True)
        with open(temporary, 'rb') as source:
            aiff = source.read()
        os.remove(temporary)
        at = aiff.index(b'SSND')
        size = struct.unpack('>I', aiff[at + 4:at + 8])[0]
        data = aiff[at + 16:at + 8 + size]
        data = data[:len(data) // 34 * 34]
        header = struct.pack('>4s4sIHHI', b'RCTA', b'ima4', rate, 1, 16, len(data) // 34 * 64)
    else:
        data = subprocess.run(['ffmpeg', '-v', 'error', '-i', full, '-ar', str(rate), '-f', 's16be', '-acodec', 'pcm_s16be', '-'],
                              capture_output=True, check=True).stdout
        header = struct.pack('>4s4sIHHI', b'RCTA', b'twos', rate, channels, 16, len(data) // (2 * channels))
    os.makedirs(args.audio_cache, exist_ok=True)
    with open(out, 'wb') as target:
        target.write(header + data)
    return out

def is_audio(name, full):
    # RCT Classic's sounds are Ogg Vorbis; the original games keep music as WAV files named .dat.
    if name.lower().endswith('.ogg'):
        return True
    if name.lower().endswith('.dat') and os.path.getsize(full) > 1024 * 1024:
        with open(full, 'rb') as source:
            head = source.read(12)
        return head[:4] == b'RIFF' and head[8:12] == b'WAVE'
    return False

if args.audio_cache:
    files = [(name, transcode(full) if is_audio(name, full) else full) for name, full in files]
only = [o.lower() for o in args.only.split(',') if o]
if only:
    files = [f for f in files if not f[0].lower().endswith(('.sea', '.sv6', '.sc6')) or any(o in f[0].lower() for o in only)]

def write(path, group):
    offset = 12 + sum(12 + len(name.encode()) for name, _ in group)
    with open(path, 'wb') as out:
        out.write(b'RCTPAK1\0' + struct.pack('<I', len(group)))
        for name, full in group:
            size = os.path.getsize(full)
            encoded = name.encode()
            out.write(struct.pack('<III', offset, size, len(encoded)) + encoded)
            offset += size
        for _, full in group:
            with open(full, 'rb') as source:
                out.write(source.read())
    print(f'{path}: {len(group)} files, {offset} bytes')

limit = args.split_mb * 1024 * 1024
groups, current, used = [], [], 0
for item in files:
    size = os.path.getsize(item[1]) + 12 + len(item[0].encode())
    if limit and current and used + size > limit:
        groups.append(current); current, used = [], 0
    current.append(item); used += size
groups.append(current)
stem, extension = os.path.splitext(args.out)
for number, group in enumerate(groups, 1):
    write(args.out if number == 1 else f'{stem}{number}{extension}', group)
