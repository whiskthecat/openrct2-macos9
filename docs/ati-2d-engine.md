# Can the Rage 128's 2D engine draw the game's sprites? (measured 2026-10-05)

All numbers are from the physical iMac G3 (450 MHz, Rage 128 Pro, 800x600, 256 colours),
taken inside the running game (`{"op":"sprites"}` and `{"op":"blitprobe","engine":4}` through
`tools/rctl.py`; code in `compat/sdl/ShimHostMac.cpp` and `RemoteControl.cpp`).

## What the painter actually draws (Fiasco Forest, idle, 22 frames/s)

| Kind | per frame | pixels each | µs each | ms per frame |
| --- | ---: | ---: | ---: | ---: |
| recoloured (guests, ride and scenery colours) | 1,086 | 289 | 6.0 | 6.5 |
| masked (terrain edge blending) | 457 | 363 | 5.0 | 2.3 |
| plain | 351 | 373 | 5.3 | 1.9 |
| translucent (water, glass) | 66 | 336 | 6.1 | 0.4 |
| clipped away entirely | 483 | 0 | 0.8 | 0.4 |
| **all** | **2,443** | | | **11.4** |

So sprite drawing is about 11 ms of a 45 ms frame, at 5-6 µs per sprite of ~300 pixels.

## What one sprite-sized blit costs by each route

| Route | 16x24 | 64x32 |
| --- | ---: | ---: |
| QuickDraw `CopyBits`, video memory to screen (accelerated by the driver) | 18.7 µs | 21.8 µs |
| the same with a transparent colour | 19.5 µs | 22.5 µs |
| QuickDraw from main memory | 21.8 µs | 39.2 µs |
| **2D engine driven directly** (packets into the card's command FIFO), opaque or colour-keyed | **3.9 µs** | **6.9 µs** |
| Processor writing straight into video memory, opaque / skipping transparent pixels | 14 / 18 µs | 46 / 93 µs |
| Processor drawing a ~300 pixel game sprite into main memory (today's painter) | ~5.5 µs | |

- An off-screen world in video memory is granted (`useDistantHdwrMem`), and the driver's
  transparent copy uses the card's colour compare against index 0: the primitives exist.
- QuickDraw has ~18 µs of overhead per call, three times what the processor needs for the
  whole sprite. Through QuickDraw the card can never win at this granularity.
- Apple's driver keeps the card in command-processor mode (`PM4_BUFFER_CNTL` = 0x78000000);
  writes to the drawing registers are ignored. Type-0 packets written in pairs to
  `PM4_FIFO_DATA_EVEN/ODD` (0x1000/0x1004) work and coexist with the driver: surfaces via
  `SRC/DST_PITCH_OFFSET`, then source, destination, size per blit. Pixels verified.
- Directly driven, a blit is about 3.2 µs fixed plus 1.8 ns per pixel (about 560 million
  pixels/s). The fixed part is the card, not us: the loop spends its time waiting for FIFO room.
- The processor is slow at writing video memory (27-45 million pixels/s), about 20 times
  slower than the engine at moving pixels and 3 times slower than writing main memory.

## What that means

Best case, with every sprite kind on the card: 1,960 sprites x ~3.8 µs = 7.4 ms instead of
11 ms, about 3.6 ms saved per 45 ms frame (22 -> 24 frames/s). To get there the card needs a
sprite cache in video memory keyed by image and colours (55% of sprites are recoloured), the
translucent 3% still have to be done by the processor by reading video memory back, and
windows, text and filters either stay in main memory and are uploaded (as now) or get three
times slower. It is a second drawing backend that only exists on this card with this driver,
and no emulator implements the engine well enough to test it off the machine.

The engine is overwhelmingly better only at large areas, which is how it is already used
(view scrolling). Composing the frame in video memory would also remove the copy to the
screen (1.5 ms idle, ~9 ms while panning).

## Reference for the packet format

The command-processor registers and packet layout used by the probe match the open-source
Linux driver for this chip (`drivers/gpu/drm/r128/r128_drv.h` and `r128_state.c`, Linux v5.15):
`PM4_BUFFER_CNTL` 0x0704 (mode 7 = 64-entry PIO command FIFO), `PM4_STAT` 0x07B8 (FIFO count
in the low 12 bits, busy bit 16, GUI active bit 31), `PM4_FIFO_DATA_EVEN/ODD` 0x1000/0x1004,
type-0 packet header `(count-1) << 16 | register >> 2`, filler packet 0x80000000, and the
type-3 blit `0xC0009B00 | 5 << 16` followed by the master control word, source and destination
pitch/offset, `(x << 16) | y` source, `(x << 16) | y` destination, `(w << 16) | h`.
That driver sends one rectangle per "multi" packet, so it does not show whether several
rectangles in one packet are accepted.

Parked until someone is near the G3 (a bad packet could freeze its display until restart):
whether a type-3 blit packet, or several rectangles per packet, lowers the 3.2 µs fixed cost.
