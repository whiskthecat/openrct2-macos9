# How it works

OpenRCT2 is a modern C++20 program. It expects threads, a few hundred megabytes of
RAM, a POSIX file system, SDL and a fast CPU. The iMac G3 has one 450 MHz PowerPC 750,
Mac OS 9's cooperative multitasking, a fixed application memory partition and a
Rage 128 video card. This page covers what it took to close that gap.

All engine changes are in `patches/openrct2-classic-mac.patch`. The Mac side is in
`compat/classic` (system services) and `compat/sdl` (display and input).

## Compiling it at all

- **Toolchain.** Retro68's GCC targets Classic Mac OS and produces a PEF application
  with a resource fork. The engine builds as C++20 with `-mcpu=750`.
- **A linker bug.** GCC emits template statics, inline variables and their guard
  variables as *weak common* symbols. Retro68's XCOFF linker gave each of these a
  zero-length section, so two variables silently shared the same memory. The game
  failed at random. `tools/xcoff_weakfix.py` rewrites every object file, and the
  toolchain's own libraries, to use ordinary commons; a compiler launcher applies it
  after each compile.
- **`int32_t` is `long`** in this toolchain, which breaks template matching all over
  the engine. The build redefines the 32-bit typedefs as `int` (same size and representation).
- **No threads.** Mutexes, futures and the job pool become single-threaded stand-ins
  (`compat/classic/prefix.h`, `JobPoolSync.cpp`). The game yields to Mac OS between frames.
- **Turned off:** networking, plugins (scripting), TrueType, HTTP and the version checker.
  libzip, ICU and FriBidi are replaced with small stand-ins (`compat/`).

## Memory

Classic Mac OS gives an application one fixed partition. It cannot join scattered
free space, so the largest free block matters as much as the total.

- **Original-game limits.** OpenRCT2 sizes its tables for parks far bigger than the
  original games allowed. This build uses the original limits: a 256×256 map instead
  of 1001×1001, 255 rides, 4 stations per ride, 10,000 entities. All 139 scenarios
  still load and run unchanged.
- **A size-class allocator** (`FastAlloc.cpp`) replaces `malloc`. The Mac Memory
  Manager stalls under the game's hundreds of thousands of small blocks. Large blocks
  go back to the system when freed, so changing park or zoom level gives memory back.
- **Object images** used to be copied twice while loading, one block per image:
  about 85,000 blocks in one park. Each object's images now go into a single block.
- **Sounds** are preloaded only when physical RAM and free heap allow it. Otherwise
  a bounded working set is kept and the rest is read on demand.

## Files

The engine sees virtual directories: `/RCT/openrct2` (OpenRCT2's data), `/RCT/game`
(RCT2 or RCT Classic), the optional `/RCT/rct1` (RCT1) and `/RCT/user` (saves and
config). All but the last are read-only pack files beside the application (`ClassicVfs.cpp`; the format is in
`tools/mkpak.py`). Reading them in 256 KB unbuffered windows, instead of through the C
library's 1 KB buffer, cut start-up from 21 s to 5 s and loading a park from 64 s to 9 s.

Autosaves capture the park in one tick, then serialise, compress and write it over
the following frames in pieces of 16 KB or less. The game doesn't pause while it saves.

## Display

- **8-bit, with the game's palette as the hardware colour table.** RCT is a 256-colour
  game, so frames are written to the screen with no conversion. Palette animation
  (water, lights) goes straight into the card's colour table. The driver's call waits
  for the vertical blank, which cost about 5 ms per frame; the direct write is used
  only after reading the card's table back to confirm it matches.
- **Only changed 16×16 cells are copied to video memory**, eight bytes at a time.
- **The video card scrolls.** When the view pans, the picture already on screen is
  moved by a screen-to-screen `CopyBits`, which the ATI driver performs with its 2D
  engine. Only the newly exposed strip is painted. The copy of the frame in memory
  is not moved at all; pending changes are carried along with the scroll.
- **Rain and snow** are drawn straight to the screen and erased before the next frame,
  so they never dirty the frame in memory.

## Painting

Most frame time on a G3 is spent deciding what to draw and drawing sprites. OpenRCT2
rebuilds and sorts the whole visible scene every frame. This build keeps what it can:

- **Column cache.** The main view keeps, for each 32-pixel-wide column, the sorted
  list of map elements (terrain, paths, scenery, track). Each frame it adds only the
  moving things (guests, vehicles) and redraws only the rectangles that changed. A
  map change drops the columns it touches. A change confined to one tile, such as an
  animation or a newly placed item, replaces just that tile's entries.
- **Map pictures.** Each cached column also keeps a picture of itself with only the
  map drawn. A rectangle that is dirty because a guest moved is restored from that
  picture. Then the guest is drawn, plus only the map elements in front of it. Before
  this, 92% of the per-frame drawing was scenery redrawn behind something that moved.
- **Exact ordering.** By default the cached view sorts sprites the same way OpenRCT2
  does, so pictures match a full repaint. An option trades that for speed.
- **Smaller wins:** inline run-length blitting, skipping empty mask words, inlined
  bounds and state checks (there is no link-time inlining), entity interpolation
  prepared only for the last tick before each picture, and up to 12 catch-up ticks
  per frame, so the game keeps its speed when the frame rate drops.

Zooming out one step used to run at 10 fps standing still and 6–8 fps panning;
with the cache it runs at 38 and 36–37 fps (Fiasco Forest).

## Sound

RCT Classic's sounds are Ogg Vorbis, too expensive to decode on a G3 during play.
`tools/make_packs.py` converts them when the packs are built: effects to 16-bit
samples, and long pieces such as ride music to IMA 4:1 mono, which the Sound Manager
decodes itself. The original games' WAV music gets the same treatment; their sound
banks are left as they are, and each effect is read from the bank on its own. Each playing sound gets its own Sound Manager channel, 16 at most.
The Sound Manager mixes them and converts their rates; the game only sends volume,
pan and rate commands. Loops stay gapless because one repeat is always queued ahead.

## Testing

- **Byte order.** The engine was written for little-endian PCs. A big-endian PowerPC
  Linux build, run under `qemu-user`, plays the same parks as the x86 build. The two
  must produce identical state digests (random state, guests, cash, entities) after
  thousands of ticks. The real G3 produces the same digest.
- **Pictures.** Scripted sessions at three screen sizes compare the cached painter
  against OpenRCT2's own paint lists. Every differing pixel must be explained by a
  colour the reference would draw there.
- **Hardware.** Timings in this repository come from the iMac G3 itself. It runs
  about 38 times slower than the development PC and 3.5 times slower than QEMU's G3
  emulation, so emulator timings were never used to judge speed.

[ati-2d-engine.md](ati-2d-engine.md) records an experiment driving the Rage 128's
2D engine directly through its command FIFO, to draw sprites on the card. It works
and blits are fast, but the per-sprite overhead leaves too little to gain, so the
game draws sprites on the CPU.
