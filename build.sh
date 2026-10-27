#!/usr/bin/env bash
# Build OpenRCT2 for Mac OS 9 in containers (Podman or Docker).
#
#   ./build.sh          the application: build/release/RCT.bin (MacBinary)
#   ./build.sh data     OpenRCT2's data packs: build/release/openrct2.pak, title.pak
#   ./build.sh all      both
#
# Environment: CONTAINER=podman|docker (default: whichever is installed),
# JOBS=N (default: all cores), MPW_GM=path/to/MPW-GM.img.bin (otherwise downloaded;
# see docs/building.md).
set -euo pipefail
cd "$(dirname "$0")"
root="$PWD"
what="${1:-app}"
engine="${CONTAINER:-$(command -v podman >/dev/null && echo podman || echo docker)}"
jobs="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}"
mac_image="${MAC_IMAGE:-openrct2-classic-mac}"
dev_image="${DEV_IMAGE:-openrct2-classic-dev}"

# 1. OpenRCT2 at the pinned revision, with this port's changes applied.
if [ ! -d upstream/.git ]; then
  url=$(python3 -c 'import json; print(json.load(open("upstream.lock.json"))["url"])')
  rev=$(python3 -c 'import json; print(json.load(open("upstream.lock.json"))["revision"])')
  echo "Fetching OpenRCT2 $rev"
  git init -q upstream
  git -C upstream fetch -q --depth 1 "$url" "$rev"
  git -C upstream checkout -q -b classic-mac FETCH_HEAD
  git -C upstream apply --whitespace=nowarn "$root/patches/openrct2-classic-mac.patch"
fi
# 2. zlib, libpng, zstd, SDL2 headers, nlohmann/json (checksummed downloads).
third_party/fetch.sh

# Apple's Universal Interfaces 3.4, from MPW 3.5 Golden Master (a free download from
# Apple in its day, still served by the Internet Archive's copy of Apple's FTP site).
mpw_url=https://web.archive.org/web/20250121125357id_/https://staticky.com/mirrors/ftp.apple.com/developer/Tool_Chest/Core_Mac_OS_Tools/MPW_etc./MPW-GM_Images/MPW-GM.img.bin
mpw_sha256=99bbfa95bb9800c8ffc572fce6d72e561f012331c5c623fa45f732502b6fa872
fetch_mpw() {
  [ -n "${MAC_UNIVERSAL_INTERFACES:-}" ] || [ -n "${MPW_GM:-}" ] && return
  MPW_GM="$PWD/build/downloads/MPW-GM.img.bin"
  if [ ! -f "$MPW_GM" ]; then
    echo "Downloading Apple's MPW 3.5 Golden Master (Universal Interfaces, 25 MB)"
    mkdir -p build/downloads && curl -fsSL "$mpw_url" -o "$MPW_GM.part"
    echo "$mpw_sha256  $MPW_GM.part" | { command -v sha256sum >/dev/null && sha256sum -c - || shasum -a 256 -c -; } >/dev/null
    mv "$MPW_GM.part" "$MPW_GM"
  fi
}

run() { # image, command...
  local image="$1"; shift
  local args=(--rm -i -v "$root:/work" -w /work -e CMAKE_BUILD_PARALLEL_LEVEL="$jobs")
  [ -n "${RUN_NAME:-}" ] && args+=(--name "$RUN_NAME")
  if [ "$image" = "$mac_image" ]; then
    if [ -n "${MAC_UNIVERSAL_INTERFACES:-}" ]; then
      args+=(-v "$(cd "$MAC_UNIVERSAL_INTERFACES" && pwd):/Retro68-build/toolchain/universal:ro" -e INTERFACES=universal)
    else
      args+=(-v "$(cd "$(dirname "$MPW_GM")" && pwd)/$(basename "$MPW_GM"):/mpw/MPW-GM.img.bin:ro"
             -e INTERFACES=universal -e INTERFACESFILE=/mpw/MPW-GM.img.bin)
    fi
  fi
  # Retro68's set-up step runs as root; hand the build output back afterwards.
  "$engine" run "${args[@]}" "$image" sh -c '"$@"; status=$?; chown -R '"$(id -u):$(id -g)"' build; exit $status' sh "$@"
}

mkdir -p build/release
if [ "$what" = app ] || [ "$what" = all ]; then
  fetch_mpw
  "$engine" build -q -t "$mac_image" -f container/Containerfile.mac . >/dev/null
  run "$mac_image" sh -c 'cmake -S . -B build/mac -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake >/dev/null &&
    python3 tools/build_identity.py prepare build/mac && cmake --build build/mac --target RCT_APPL'
  cp build/mac/RCT.bin build/release/RCT.bin
  echo "Built build/release/RCT.bin"
fi
if [ "$what" = data ] || [ "$what" = all ]; then
  "$engine" build -q -t "$dev_image" -f container/Containerfile container >/dev/null
  run "$dev_image" tools/make_data_pack.sh build/release
  echo "Built build/release/openrct2.pak and title.pak"
fi
