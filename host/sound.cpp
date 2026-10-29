// PGM sound (see sound.h).
//
// Two ways to run it:
//   synchronous (desktop, deterministic): the scheduler calls run_line() every scanline; samples go to a ring.
//   threaded (Vita): the audio thread calls generate() for the samples it needs and the chips run right there,
//     256 Z80 cycles per output sample (8.4672 MHz / 33075 Hz). The 68000 side only posts latch values, NMIs and
//     reset/halt requests (atomics), which the audio thread applies between chunks; the Z80 RAM is written by the
//     68000 only while the Z80 is halted (program upload).
#include "sound.h"
#include "romset.h"
#include "board.h"
#include "ics2115.h"
#include "z80.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace sound {
bool enabled = true;
uint64_t prof_z80_cycles;
static bool threaded;
static Z80 z80;
static ICS2115 ics;
static std::vector<uint8_t> samples;      // 16 MB ICS2115 address space
static bool z80_running;
static double z80_credit, ics_credit, mix_credit;

constexpr double LINE_HZ = 15625.0;        // 50 MHz / 5 / 640
constexpr double Z80_HZ = 33868800.0 / 4;

// requests from the 68000 (threaded mode)
static std::atomic<int> nmi_requests{0};
static std::atomic<int> control_request{-1};   // -1 none, else the value written to 0xc00008
static std::atomic<uint8_t> latch[3];

// ---- output ring (synchronous mode) ----
static constexpr uint32_t RING = 1u << 14;
static int16_t ring[RING * 2];
static std::atomic<uint32_t> ring_w{0}, ring_r{0};

static uint8_t z_read(uint16_t a) { return board::z80_ram[a]; }
static void z_write(uint16_t a, uint8_t v) { board::z80_ram[a] = v; }
static uint8_t z_in(uint16_t port) {
    switch (port >> 8) {
    case 0x80: return ics.read(port & 3);
    case 0x81: return latch[2].load(std::memory_order_relaxed);
    case 0x82: return latch[0].load(std::memory_order_relaxed);
    case 0x84: return latch[1].load(std::memory_order_relaxed);
    }
    return 0xff;
}
static void z_out(uint16_t port, uint8_t v) {
    switch (port >> 8) {
    case 0x80: ics.write(port & 3, v); break;
    case 0x81: latch[2].store(v, std::memory_order_relaxed); break;
    case 0x82: latch[0].store(v, std::memory_order_relaxed); break;
    case 0x84: latch[1].store(v, std::memory_order_relaxed); break;
    }
}

bool load() {
    samples.assign(0x1000000, 0);
    auto put = [&](const char* name, size_t at) {
        std::vector<uint8_t> b;
        if (!romset::read(name, b)) return false;
        std::memcpy(samples.data() + at, b.data(), std::min(b.size(), samples.size() - at));
        return true;
    };
    bool ok = put("pgm_m01s.rom", 0) && put("w04501.u5", 0x400000);
    ics.rom = samples.data();
    return ok;
}

void set_threaded(bool on) { threaded = on; }

void reset() {
    z80.read = z_read; z80.write = z_write; z80.in = z_in; z80.out = z_out;
    z80.reset();
    ics.reset();
    z80_running = false;   // halted until the 68000 releases it
}

static void apply_control(int v) {
    if (v == 0x5050) { ics.reset(); z80.reset(); z80_running = true; }
    else z80_running = false;   // the games halt the Z80 while they upload its program
}

void latch_w(int n, uint8_t v) {
    latch[n].store(v, std::memory_order_relaxed);
    if (n != 0) return;
    if (threaded) nmi_requests++;
    else if (z80_running) z80.nmi();
}
uint8_t latch_r(int n) { return latch[n].load(std::memory_order_relaxed); }

void z80_control_w(uint16_t v) {
    if (threaded) control_request.store(v);
    else apply_control(v);
}

int rate() { return ics.rate(); }

// Run the chips for n (<= 64) samples and mix them; out: interleaved stereo
static void chunk(int16_t* out, int n) {
    int32_t l[64] = {}, r[64] = {};
    for (int i = 0; i < n; i++) {
        ics.advance(1024);   // 32 voices x 32 clocks per sample
        if (z80_running) {
            z80.irq_line = ics.irq_on;
            z80_credit += 256;
            int used = z80.run((int)z80_credit);
            z80_credit -= used;
            prof_z80_cycles += used;
        }
    }
    ics.render(l, r, n);
    for (int i = 0; i < n; i++) {
        int32_t m = (l[i] + r[i]) / 10;   // mono board: both channels at 0.1 (MAME's routing), clipped
        if (m > 32767) m = 32767;
        if (m < -32768) m = -32768;
        out[2 * i] = out[2 * i + 1] = (int16_t)m;
    }
}

void generate(int16_t* out, int frames) {
    while (frames > 0) {
        int c = control_request.exchange(-1);
        if (c != -1) apply_control(c);
        for (int k = nmi_requests.exchange(0); k > 0; k--)
            if (z80_running) z80.nmi();
        int n = std::min(frames, 16);   // ~0.5 ms: the granularity of the 68000's requests
        chunk(out, n);
        out += 2 * n;
        frames -= n;
    }
}

void run_line() {
    if (!enabled || threaded) return;
    z80_credit += Z80_HZ / LINE_HZ;
    ics_credit += ICS2115::CLOCK / LINE_HZ;
    uint32_t clocks = (uint32_t)ics_credit;
    ics_credit -= clocks;
    ics.advance(clocks);
    if (z80_running) {
        z80.irq_line = ics.irq_on;
        int used = z80.run((int)z80_credit);
        z80_credit -= used;
        prof_z80_cycles += used;
    } else {
        z80_credit = 0;
    }
    mix_credit += ics.rate() / LINE_HZ;
    int n = (int)mix_credit;
    if (n <= 0) return;
    mix_credit -= n;
    n = std::min(n, 8);
    int32_t l[8] = {}, r[8] = {};
    ics.render(l, r, n);
    uint32_t w = ring_w.load(std::memory_order_relaxed);
    for (int i = 0; i < n; i++) {
        if (w - ring_r.load(std::memory_order_acquire) >= RING) break;   // full: drop
        int32_t m = (l[i] + r[i]) / 10;
        if (m > 32767) m = 32767;
        if (m < -32768) m = -32768;
        ring[(w & (RING - 1)) * 2] = ring[(w & (RING - 1)) * 2 + 1] = (int16_t)m;
        w++;
    }
    ring_w.store(w, std::memory_order_release);
}

int pull(int16_t* out, int max_frames) {
    uint32_t r = ring_r.load(std::memory_order_relaxed), w = ring_w.load(std::memory_order_acquire);
    int n = (int)(w - r);
    if (n > max_frames) n = max_frames;
    for (int i = 0; i < n; i++) {
        out[2 * i] = ring[((r + i) & (RING - 1)) * 2];
        out[2 * i + 1] = ring[((r + i) & (RING - 1)) * 2 + 1];
    }
    ring_r.store(r + n, std::memory_order_release);
    return n;
}
}
