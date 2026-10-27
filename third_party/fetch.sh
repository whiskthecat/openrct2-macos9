#!/usr/bin/env bash
# Fetch the small portable C libraries the engine needs. They are compiled
# from source for every target (host, big-endian Linux, Mac OS 9).
set -euo pipefail
cd "$(dirname "$0")"
command -v sha256sum >/dev/null || sha256sum() { shasum -a 256 "$@"; }  # macOS
get() { # name url sha256 strip-dir
  [ -d "$1" ] && return
  curl -fsSL "$2" -o "$1.tar.gz"
  echo "$3  $1.tar.gz" | sha256sum -c -
  mkdir "$1" && tar -xzf "$1.tar.gz" -C "$1" --strip-components=1 && rm "$1.tar.gz"
}
get zlib   https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz 9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
get libpng https://download.sourceforge.net/libpng/libpng-1.6.43.tar.gz e804e465d4b109b5ad285a8fb71f0dd3f74f0068f91ce3cdfde618180c174925
get zstd   https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz 8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1
# SDL2 headers only: the UI library is written against SDL's types and
# constants. compat/sdl supplies the few functions on top of our own platform code.
if [ ! -d sdl2 ]; then
  curl -fsSL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz -o sdl.tar.gz
  echo "24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4  sdl.tar.gz" | sha256sum -c -
  mkdir sdl2 && tar -xzf sdl.tar.gz -C sdl2 --strip-components=1 SDL2-2.30.9/include SDL2-2.30.9/LICENSE.txt && rm sdl.tar.gz
fi
if [ ! -d nlohmann ]; then
  mkdir -p nlohmann/nlohmann
  base=https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann
  curl -fsSL "$base/json.hpp" -o nlohmann/nlohmann/json.hpp
  curl -fsSL "$base/json_fwd.hpp" -o nlohmann/nlohmann/json_fwd.hpp
  ( cd nlohmann/nlohmann && sha256sum -c - ) <<'SUMS'
9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6  json.hpp
5cefbba751baf5243033fd894c70be9b37103d64b3d3a959e4173197d71137b9  json_fwd.hpp
SUMS
fi
cp -n libpng/scripts/pnglibconf.h.prebuilt libpng/pnglibconf.h
