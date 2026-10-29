// PGM sound: the Z80 (8.47 MHz, program uploaded by the 68000 into its 64 KB RAM) driving the ICS2115.
//   Z80 I/O: 0x8000-0x8003 ICS2115, 0x8100 latch 3, 0x8200 latch 1 (+ NMI from the 68000), 0x8400 latch 2
//   68000: 0xc00002/4/c the three latches (writing the first one NMIs the Z80), 0xc00008 Z80 reset (0x5050) / halt
// Runs inside the scheduler (one scanline at a time); the mixed samples (33075 Hz stereo) go to a ring buffer
// that the platform's audio output reads.
#pragma once
#include <cstdint>
#include <string>

namespace sound {
bool load();                              // sample ROMs (romset): BIOS pgm_m01s.rom + game w04501.u5
void reset();
void run_line();                          // one scanline (64 us) of Z80, ICS2115 timers and mixing
void latch_w(int n, uint8_t v);           // from the 68000 (n = 0..2)
uint8_t latch_r(int n);
void z80_control_w(uint16_t v);           // 0xc00008
int rate();                               // output sample rate (Hz)
int pull(int16_t* stereo, int max_frames);   // synchronous mode: take mixed frames (L, R interleaved)
void set_threaded(bool on);               // threaded mode: run_line() does nothing, the audio thread calls generate()
void generate(int16_t* stereo, int frames);  // threaded mode: run the chips for 'frames' samples and mix them
extern bool enabled;                      // false: skip the Z80 and the mixing entirely
}
