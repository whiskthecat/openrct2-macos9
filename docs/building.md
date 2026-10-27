# Building

Everything builds in containers on Linux or macOS. You need Podman or Docker, Git,
Python 3, curl and about 10 GB of disk space.

```sh
./build.sh all
```

This produces `build/release/`:

| File | What it is |
| --- | --- |
| `RCT.bin` | The application, MacBinary-encoded (decode it on the Mac with StuffIt Expander) |
| `openrct2.pak` | OpenRCT2's own data: sprites, fonts, languages and object definitions |
| `title.pak` | The title-screen sequence |

The game data (`game*.pak`) comes from your copy of RCT Classic; see
[the README](../README.md#installing).

## What `build.sh` does

1. Fetches OpenRCT2 at the revision in `upstream.lock.json` into `upstream/`, then
   applies `patches/openrct2-classic-mac.patch`.
2. Downloads zlib, libpng, zstd, the SDL2 headers and nlohmann/json into
   `third_party/`, checking each one's SHA-256.
3. `./build.sh` (or `app`) builds a Retro68 image (`container/Containerfile.mac`). The
   image patches the toolchain's own libraries for the weak-symbol linker bug. Then
   it cross-compiles the engine and UI into `build/mac/RCT.bin`. A full build takes
   about 15 minutes on 16 cores.
4. `./build.sh data` builds a Debian image (`container/Containerfile`) and compiles
   OpenRCT2's command-line tool for Linux. That tool turns `upstream/resources` into
   the sprite files, and the pinned object pack is downloaded. All of it goes into
   `openrct2.pak`.

Rebuilds are incremental: run `./build.sh` again after changing anything in `compat/`
or `tools/`. To change the engine itself, edit `upstream/` and regenerate the patch:

```sh
git -C upstream diff $(python3 -c 'import json;print(json.load(open("upstream.lock.json"))["revision"])') \
  > patches/openrct2-classic-mac.patch
```

## Apple's Universal Interfaces

The game uses Mac OS headers and libraries that Retro68's open-source Multiversal
Interfaces don't cover, such as Code Fragment Manager, Display Manager and Name
Registry. Apple's Universal Interfaces 3.4 provide them. Apple gave them away as part
of MPW 3.5 Golden Master, but they can't be redistributed, so they aren't in this
repository. `build.sh` downloads the disk image (`MPW-GM.img.bin`, 25 MB) from the
[Internet Archive's copy of Apple's developer FTP site](https://web.archive.org/web/20250121125357/https://staticky.com/mirrors/ftp.apple.com/developer/Tool_Chest/Core_Mac_OS_Tools/MPW_etc./MPW-GM_Images/)
and checks its SHA-256. Retro68's container extracts the interfaces from it at the
start of each Mac build.

To use your own copy, set `MPW_GM` to the file. If you already have the interfaces
unpacked for Retro68 (a folder with `CIncludes`, `RIncludes` and `libppc`), set
`MAC_UNIVERSAL_INTERFACES` to that folder instead.

## Running on Linux

The same sources also build as a Linux program, for development and testing. It
draws to an off-screen display and is driven by scripts, not played. The CMake
targets are `rctgame` (the game) and `rctsim` (headless simulation that prints state
digests). Build them in the Debian image with an ordinary
`cmake -S . -B build/linux -G Ninja && cmake --build build/linux`. Configure with
`-DCMAKE_TOOLCHAIN_FILE=container/ppc-linux.cmake` for the big-endian PowerPC Linux
build, which runs under `qemu-ppc`.
