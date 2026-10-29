// ICS2115 WaveFront wavetable synthesizer (our own implementation; behaviour as documented by MAME's ics2115.cpp
// and the ICS patents it cites: oscillators with forward/bidirectional loops, volume ramps, pan law, u-law and
// 8/16-bit samples, two timers, IRQ to the Z80).
#pragma once
#include <cstdint>

struct ICS2115 {
    static constexpr int CLOCK = 33868800;

    struct Voice {
        // oscillator: addresses are 20.12 fixed point (sample address << 12), fc = 6.9 frequency step
        int32_t osc_left;
        uint32_t osc_acc, osc_start, osc_end;
        uint16_t fc;
        uint8_t saddr;                     // address bits 27-20
        uint8_t osc_conf;                  // 0 ulaw, 1 16-bit, 3 loop, 4 bidir, 5 irq, 6 invert, 7 irq pending
        uint8_t osc_ctrl;                  // 0 done, 1 stop
        // volume ramp
        int32_t vol_left, vol_add, vol_start, vol_end, vol_acc;
        uint16_t vol_regacc;
        uint8_t vol_incr, pan, vol_mode;
        uint8_t vol_ctrl;                  // 0 done, 1 stop, 3 loop, 4 bidir, 5 irq, 6 invert, 7 irq pending
        bool playing() const { return !(osc_ctrl & 3); }
    };

    const uint8_t* rom = nullptr;          // 16 MB sample space (BIOS samples at 0, game samples at 0x400000)
    Voice v[32];
    struct Timer { uint8_t scale, preset; uint64_t period, count; } timer[2];
    uint8_t active_osc, osc_select, reg_select, irq_enabled, irq_pending;
    bool irq_on;
    int16_t ulaw[256];
    uint16_t volume[4096], panlaw[256];

    ICS2115();
    void reset();
    uint8_t read(int offset);              // Z80 ports 0x8000-0x8003
    void write(int offset, uint8_t data);
    void advance(uint32_t clocks);         // run the timers
    void render(int32_t* left, int32_t* right, int n);   // mix n samples at rate() (adds to the buffers)
    int rate() const { return CLOCK / ((active_osc + 1) * 32); }

private:
    uint16_t reg_read();
    void reg_write(uint16_t data, uint16_t mask);
    void recalc_irq();
    void recalc_timer(int t);
    int32_t sample(Voice& vc);
    uint8_t rom_byte(const Voice& vc, uint32_t addr) const { return rom[((uint32_t)vc.saddr << 20 | (addr & 0xfffff)) & 0xffffff]; }
    bool step_osc(Voice& vc);
    bool step_vol(Voice& vc);
};
