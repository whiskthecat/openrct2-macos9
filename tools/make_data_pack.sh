#!/usr/bin/env bash
# Build openrct2.pak (OpenRCT2's own open data: sprites, fonts, languages,
# object definitions) and title.pak. Run inside the Linux development
# container, e.g.  ./build.sh data   (which calls this script).
#
#   tools/make_data_pack.sh OUTPUT-FOLDER
set -euo pipefail
cd "$(dirname "$0")/.."
out="$(mkdir -p "${1:?usage: tools/make_data_pack.sh OUTPUT-FOLDER}" && cd "$1" && pwd)"
build=build/data
stage=build/data-stage

# OpenRCT2's command-line tool compiles the sprite sheets from upstream/resources.
cmake -S . -B "$build" -G Ninja -DBUILD_UI=OFF >/dev/null
python3 tools/build_identity.py prepare "$build"  # before compiling: a fresh tree discards unknown objects
cmake --build "$build" --target openrct2-cli
rm -rf "$stage" && mkdir -p "$stage"
for sheet in g2 fonts palettes tracks; do
  (cd "$build" && ./openrct2-cli sprite build "$OLDPWD/$stage/$sheet.dat" "$OLDPWD/upstream/resources/$sheet/sprites.json" silent)
done
cp -r upstream/data/language upstream/data/scenario_patches upstream/data/shaders "$stage/"

# The changelog the game shows on its first launch: this port's version, then OpenRCT2's.
version=$(sed -n 's/^#define RCT_VERSION_\(MAJOR\|MINOR\|PATCH\) //p' compat/classic/ClassicVersion.h | paste -sd.)
{
  echo "OpenRCT2 for Mac OS 9, version $version"
  echo "------------------------------------------------------------------------"
  echo "- A native build for PowerPC Macs running Mac OS 9, based on OpenRCT2"
  echo "  $(sed -n 's/^#define kOpenRCT2Version "\(.*\)"/\1/p' upstream/src/openrct2/Version.h) (revision $(python3 -c 'import json; print(json.load(open("upstream.lock.json"))["revision"][:9])')). OpenRCT2's own changes follow."
  echo "- Project page: https://github.com/whiskthecat/openrct2-macos9"
  echo
  cat upstream/distribution/changelog.txt
} > "$stage/changelog.txt"

# Object definitions, at the version the pinned OpenRCT2 revision expects.
url=$(python3 -c 'import json; print(json.load(open("upstream/assets.json"))["objects"]["url"])')
sum=$(python3 -c 'import json; print(json.load(open("upstream/assets.json"))["objects"]["sha256"])')
mkdir -p build/downloads
zip=build/downloads/objects-$sum.zip
[ -f "$zip" ] || curl -fsSL "$url" -o "$zip"
echo "$sum  $zip" | sha256sum -c - >/dev/null
mkdir -p "$stage/object" && unzip -q -o "$zip" -d "$stage/object"

python3 tools/mkpak.py "$out/openrct2.pak" "$stage"
python3 tools/mkpak.py "$out/title.pak" packsrc/title
