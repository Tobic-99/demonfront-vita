# How the port runs on the PS Vita

The PGM board runs at 59.19 Hz (264 lines of 64 µs). Measured on a PS Vita at 444 MHz: 59.2 fps; the
translated 68000 and ARM code take about 1.1 ms per frame, drawing a frame 7–11 ms (split over two cores), the
sound about 30% of a core.

## CPU: the translated code

- **ARM (IGS027A)**: the game's ARM program is Thumb code in the first 96 KB of the external ROM.
  `tools/arm_discover.py` finds the functions (calls, code pointers, switch tables in the compiler's `add pc`
  forms, `bl` used as a long branch), `tools/thumbrec.py` writes one C++ function per guest function: registers
  and flags in C++ locals (dead flag work removed by the compiler), `bl` as a direct call, switches as C++
  `switch`, literal loads folded from the ROM.
- **68000**: `tools/m68k_isa.py` (decoder), `tools/m68k_discover.py` (BIOS and game: vectors, service tables,
  GCC switch tables, code pointers), `tools/m68krec.py` (one C++ function per guest function; registers and
  X/N/Z/V/C in locals; memory as native 16-bit words so the shared RAM matches the ARM's layout; 4 KB page tables).
- **Idle loops**: the 68000's vblank wait and the ARM's command wait end the CPU's time slice instead of spinning.
- **Scheduler**: both CPUs on fibers, one scanline slice at a time; a latch write or an ARM bank switch ends the
  writer's slice at once, so the other CPU sees it in time.

## Video

- Tiles are decoded to one byte per pixel when the ROMs are loaded (background 32x32 5 bpp, text 8x8 4 bpp) and
  drawn as spans; sprites (zoomed) are read from the A/B ROMs.
- The video state is copied at vblank (`video::Frame`), so drawing runs on other cores while the CPUs go on.
- **Two bands**: `video::render` draws lines y0..y1-1; the render thread draws the top half, a band thread on
  core 0 (next to the CPUs, which need ~1.1 ms per frame) the bottom half. The banded output is identical to a
  full-frame draw.
- **Drawing and presenting overlap**: two pixel buffers; while frame N is drawn, a present thread uploads frame
  N-1 as a texture and waits for the display's vblank (vitaGL).
- vitaGL's two fixed-function shaders ship in the VPK and are copied to its shader cache at start, so
  libshacccg is not needed.

## Sound

The Z80 and ICS2115 (wavetable, 33075 Hz) run on the audio thread as fast as the output needs them, resampled to
48 kHz; the 68000 posts latch values, NMIs and resets through atomics.

## Threads

| Core | Threads (priority) |
|---|---|
| 0 | CPUs: 68000 + ARM (0x60), band (bottom half of each frame, 0x70) |
| 1 | render (top half, 0x48) |
| 2 | present (0x48), audio (0x40), battery RAM writer (0xA0) |
