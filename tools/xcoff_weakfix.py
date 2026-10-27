#!/usr/bin/env python3
"""Work around a Retro68 PowerPC linker bug with weak uninitialised data.

GCC emits template statics, inline variables and their guard variables as
*weak common* symbols (C_WEAKEXT + XTY_CM). This linker turns each of them
into a zero-length BSS section, so the next variable is laid on top of it and
the two silently share memory. Plain (non-weak) commons are merged correctly,
and are just as valid for objects that are identical in every translation
unit, so this tool rewrites weak commons to ordinary commons.

  xcoff_weakfix.py FILE...        patch XCOFF objects and ar archives in place
"""
import struct, subprocess, sys, tempfile, os, shutil

C_EXT, C_WEAKEXT, XTY_CM = 2, 111, 3


def patch_object(data: bytearray) -> int:
    if len(data) < 20 or struct.unpack_from('>H', data, 0)[0] != 0x01DF:
        return 0
    symptr, nsyms = struct.unpack_from('>II', data, 8)
    changed, i = 0, 0
    while i < nsyms:
        entry = symptr + i * 18
        sclass, numaux = data[entry + 16], data[entry + 17]
        if sclass == C_WEAKEXT and numaux >= 1:
            aux = symptr + (i + numaux) * 18
            if data[aux + 10] & 7 == XTY_CM:
                data[entry + 16] = C_EXT
                changed += 1
        i += 1 + numaux
    return changed


def patch_file(path: str, ar: str) -> int:
    with open(path, 'rb') as handle:
        head = handle.read(8)
    if head.startswith(b'!<arch>') or head.startswith(b'<bigaf>'):
        work = tempfile.mkdtemp()
        try:
            members = subprocess.run([ar, 't', path], capture_output=True, text=True, check=True).stdout.split('\n')
            members = [m for m in members if m]
            subprocess.run([ar, 'x', os.path.abspath(path)], cwd=work, check=True)
            total, touched = 0, []
            for name in dict.fromkeys(members):
                member = os.path.join(work, name)
                if not os.path.isfile(member):
                    continue
                blob = bytearray(open(member, 'rb').read())
                count = patch_object(blob)
                if count:
                    open(member, 'wb').write(blob)
                    touched.append(name)
                    total += count
            if touched:
                subprocess.run([ar, 'r', os.path.abspath(path)] + touched, cwd=work, check=True, capture_output=True)
            return total
        finally:
            shutil.rmtree(work)
    blob = bytearray(open(path, 'rb').read())
    count = patch_object(blob)
    if count:
        open(path, 'wb').write(blob)
    return count


if __name__ == '__main__':
    ar = os.environ.get('XCOFF_AR', 'powerpc-apple-macos-ar')
    quiet = os.environ.get('XCOFF_WEAKFIX_QUIET')
    for path in sys.argv[1:]:
        count = patch_file(path, ar)
        if not quiet:
            print(f'{path}: {count} weak commons made ordinary')
