#!/usr/bin/env python3
"""Content identity for the port, both lanes, and consumers. No Git on NAS needed."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parent
SCHEMA = 1
# Include ignored vendor sources and untracked source additions too. Reports,
# instructions, assets and build products cannot affect the compiled engine.
INPUTS = ('CMakeLists.txt', 'compat', 'tools', 'container', 'upstream/src',
          'upstream/CMakeLists.txt', 'upstream/cmake', 'third_party')


def digest_file(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def snapshot(port=PORT):
    files = {}
    for name in INPUTS:
        top = port / name
        paths = [top] if top.is_file() else top.rglob('*')
        for path in paths:
            relative = path.relative_to(port)
            if any(p in ('.git', '__pycache__', 'build') or p.startswith('build-') for p in relative.parent.parts):
                continue
            if path.is_file():
                files[relative.as_posix()] = digest_file(path)
    encoded = json.dumps(files, sort_keys=True, separators=(',', ':')).encode()
    return dict(schema=SCHEMA, source=hashlib.sha256(encoded).hexdigest(), files=files)


def repositories(root=ROOT):
    result = {}
    for name, path in (('port', root), ('upstream', root / 'port/upstream')):
        def git(*args):
            return subprocess.check_output(['git', '-C', str(path), *args], stderr=subprocess.DEVNULL).decode().strip()
        try:
            if Path(git('rev-parse', '--show-toplevel')).resolve() != path.resolve():
                continue
            result[name] = dict(revision=git('rev-parse', 'HEAD'), branch=git('branch', '--show-current'))
        except subprocess.CalledProcessError:
            pass  # disposable fixtures need no Git metadata
    return result


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.tmp-' + str(os.getpid()))
    tmp.write_text(json.dumps(value, sort_keys=True, indent=2) + '\n')
    tmp.replace(path)


def preparation_inputs():
    # execute.py owns this temporary inventory for one stable-source command.
    # Receipt emission and consumers always take a fresh content snapshot.
    context = os.environ.get('RCT_PREPARATION_INPUTS')
    if context:
        saved = json.loads(Path(context).read_text())
        if saved.get('port') == str(PORT) and saved['inputs']['source'] == os.environ.get('RCT_SOURCE_STATE'):
            return saved['inputs']
    return snapshot()


def prepare(build, generate=True, current=None):
    """Run BEFORE the build system reads its graph (the cmake/make/ninja shims).

    Content deltas get a timestamp newer than every retained object. An unstamped
    directory loses its objects once, since their source ancestry is unknowable.
    This keeps ordinary changed-file dependency rebuilds fast on either lane.
    """
    build = Path(build).resolve()
    if not (build / 'CMakeCache.txt').exists():
        return
    cache = (build / 'CMakeCache.txt').read_text()
    if f'CMAKE_HOME_DIRECTORY:INTERNAL={PORT}' not in cache.splitlines():
        return  # unrelated projects, e.g. an upstream asset build
    current = dict(current if current is not None else preparation_inputs())
    state_path = build / 'rct-inputs.json'
    ancestry_path = build / 'rct-ancestry.json'
    basis = ancestry_path if ancestry_path.exists() else state_path
    previous = json.loads(basis.read_text()) if basis.exists() else None
    # app/data/home/install can contain read-only worker inputs. Never change
    # their dates or follow their links when normalizing compiler products.
    def output(path):
        return (path.is_file() and not path.is_symlink()
                and path.relative_to(build).parts[0] not in ('app', 'data', 'home', 'install', 'User'))
    changed = {p for p, h in current['files'].items()
               if previous is None or previous.get('files', {}).get(p) != h}
    unknown = previous is None or previous.get('schema') != SCHEMA
    removed = previous is not None and set(previous.get('files', {})) - set(current['files'])
    # Known, unchanged content needs only its recorded dependency dates. Walking
    # every compiler product (and every screenshot) adds no ancestry evidence.
    outputs = [p for p in build.rglob('*') if output(p)] if unknown or changed or removed else []
    objects = [p for p in outputs if p.suffix in ('.o', '.obj', '.a')]
    now = time.time_ns()
    # Moving changed sources into the future can cause endless Make rebuilds.
    # Bring future output dates back first, then touch content deltas at now.
    for path in outputs:
        stat = path.stat()
        if stat.st_mtime_ns > now:
            os.utime(path, ns=(stat.st_atime_ns, now - 1_000_000))
    newest = max((p.stat().st_mtime_ns for p in outputs), default=0)
    stamp_time = max(time.time_ns(), newest + 1)
    if unknown:
        for obj in objects:
            obj.unlink()
        print(f'[rct] {build.name}: discarded objects without content ancestry', file=sys.stderr)
    dates = {}
    linked_change = False
    for relative in current['files']:
        path = PORT / relative
        stat = path.stat()
        if path.is_symlink():
            linked_change |= relative in changed
            continue
        # An unchanged upload may restore a different date from the laptop.
        # Restore this lane's known date so no-op builds remain no-ops.
        date = stamp_time if relative in changed else (previous or {}).get('mtimes', {}).get(relative, stat.st_mtime_ns)
        date = min(date, stamp_time)
        if stat.st_mtime_ns != date:
            os.utime(path, ns=(stat.st_atime_ns, date))
        dates[relative] = date
    if previous is not None:
        if changed:
            print(f'[rct] {build.name}: {len(changed)} content changes', file=sys.stderr)
        # Removing a header can otherwise leave an old object linked if its
        # dependency file was lost. Additions are handled by CONFIGURE_DEPENDS.
        if linked_change or removed:
            for obj in objects:
                obj.unlink(missing_ok=True)
            print(f'[rct] {build.name}: removed/linked inputs invalidate objects', file=sys.stderr)
    ancestry = {k: current[k] for k in ('schema', 'source', 'files')}
    ancestry['mtimes'] = dates
    if previous is None or any(previous.get(k) != v for k, v in ancestry.items()):
        atomic_json(ancestry_path, ancestry)
    if not generate:
        return
    # The exact config and lane distinguish builds with the same source snapshot.
    config = digest_file(build / 'CMakeCache.txt')
    lane = os.environ.get('RCT_EXECUTION_HOST', 'unwrapped')
    image = os.environ.get('RCT_BUILD_IMAGE', 'unknown')
    build_id = hashlib.sha256((current['source'] + config + lane + image).encode()).hexdigest()
    current.update(build=build_id, config=config, lane=lane, image=image)
    current['repositories'] = json.loads(os.environ.get('RCT_SOURCE_REPOSITORIES', '{}'))
    generated = build / 'rct-build.cpp'
    cpp = ('#include "BuildIdentity.h"\n'
           f'const char* RctBuild::Source() {{ return "rct-source-v1:{current["source"]}"; }}\n'
           f'const char* RctBuild::Id() {{ return "rct-build-v1:{build_id}"; }}\n'
           f'const char* RctBuild::Lane() {{ return "{lane}"; }}\n')
    if not generated.exists() or generated.read_text() != cpp:
        generated.write_text(cpp)
        os.utime(generated, ns=(stamp_time, stamp_time))
    if not state_path.exists() or json.loads(state_path.read_text()) != current:
        atomic_json(state_path, current)


def embedded(binary, data=None):
    data = Path(binary).read_bytes() if data is None else data
    result = {}
    for key, prefix in (('source', b'rct-source-v1:'), ('build', b'rct-build-v1:')):
        matches = set(re.findall(prefix + rb'([0-9a-f]{64})', data))
        if len(matches) != 1:
            raise RuntimeError(f'{binary}: missing or ambiguous embedded {key} identity; rebuild through wrappers')
        result[key] = matches.pop().decode()
    return result


def stamp(build, binaries):
    state = json.loads((Path(build) / 'rct-inputs.json').read_text())
    if snapshot()['source'] != state['source']:
        raise RuntimeError('sources changed during build; rebuild before using its outputs')
    for name in binaries:
        binary = Path(build) / name
        if not binary.is_file():
            continue
        identity = embedded(binary)
        if any(identity[key] != state[key] for key in ('source', 'build')):
            raise RuntimeError(f'{binary}: built code does not match the prepared source/config state')
        atomic_json(str(binary) + '.build.json', dict(state, binary_sha256=digest_file(binary), binary=name))


def verify(binary, source=None, data=None):
    binary = Path(binary)
    sidecar = Path(str(binary) + '.build.json')
    if not binary.exists() or not sidecar.exists():
        raise RuntimeError(f'{binary}: binary or build receipt missing; build through container wrappers')
    receipt = json.loads(sidecar.read_text())
    source = source or snapshot()['source']
    if receipt.get('source') != source:
        raise RuntimeError(f'{binary}: STALE SOURCE (built {receipt.get("source")}, current {source}); rebuild')
    data = binary.read_bytes() if data is None else data
    if receipt.get('binary_sha256') != hashlib.sha256(data).hexdigest():
        raise RuntimeError(f'{binary}: binary bytes do not match its build receipt; rebuild')
    actual = embedded(binary, data)
    if any(actual[key] != receipt.get(key) for key in ('source', 'build')):
        raise RuntimeError(f'{binary}: embedded build identity disagrees with receipt')
    prepared = binary.parent / 'rct-inputs.json'
    if prepared.exists():
        state = json.loads(prepared.read_text())
        if state.get('build') != receipt['build']:
            raise RuntimeError(f'{binary}: a different build was prepared here; finish rebuilding this target')
    cache = binary.parent / 'CMakeCache.txt'
    if cache.exists() and digest_file(cache) != receipt['config']:
        raise RuntimeError(f'{binary}: build configuration changed; rebuild this target')
    return receipt


def resolve(binary):
    """Logical legacy path -> verified output on the most recently selected lane.

    Explicit build-nas paths remain explicit. Never silently fall back to another
    lane's binary when the requested lane is absent or stale.
    """
    binary = Path(binary).absolute()
    host_path = ROOT / '.rct-nas/build-host'
    host = host_path.read_text().strip() if host_path.exists() else 'nas'
    if host not in ('local', 'nas'):
        raise RuntimeError(f'{host_path}: invalid selected lane')
    if not os.environ.get('RCT_EXECUTION_HOST') and host == 'nas':
        relative = binary.relative_to(PORT) if binary.is_relative_to(PORT) else None
        if relative is not None and relative.parts[0] != 'build-nas':
            binary = PORT / 'build-nas' / relative
    receipt = verify(binary)
    print(f'[rct] consume={binary} source={receipt["source"]} build={receipt["build"]} lane={receipt["lane"]}', file=sys.stderr)
    return binary, receipt


def require_running(status, receipt):
    if status.get('build', {}) != {k: receipt[k] for k in ('source', 'build', 'lane')}:
        raise RuntimeError('running game build does not match the verified binary; launch it with g3run.sh')
    if snapshot()['source'] != receipt['source']:
        raise RuntimeError('sources changed during measurement; discard this run and rebuild')


def evidence(receipt):
    return {k: v for k, v in receipt.items() if k != 'files'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['source', 'context', 'prepare', 'stamp', 'verify', 'resolve'])
    parser.add_argument('paths', nargs='*')
    args = parser.parse_args()
    if args.action == 'source':
        print(snapshot()['source'])
    elif args.action == 'context':
        print(json.dumps(repositories()))
    elif args.action == 'prepare':
        prepare(args.paths[0] if args.paths else '.')
    elif args.action == 'stamp':
        stamp(args.paths[0], args.paths[1:])
    elif args.action == 'verify':
        for path in args.paths:
            verify(path)
    elif args.action == 'resolve':
        print(resolve(args.paths[0])[0])


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, ValueError, KeyError) as error:
        print('[rct] build identity refused: ' + str(error), file=sys.stderr)
        sys.exit(125)
