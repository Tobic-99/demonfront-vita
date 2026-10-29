// Z80 interpreter (our own implementation) for the PGM's sound CPU. Opcodes are decoded by their x/y/z/p/q
// fields (the standard documented structure), with the DD/FD (IX/IY), CB, DDCB/FDCB and ED groups.
// The board supplies memory and I/O through the callbacks.
#pragma once
#include <cstdint>

struct Z80 {
    uint8_t a, f, b, c, d, e, h, l;
    uint16_t af_, bc_, de_, hl_;      // the alternate set (packed hi << 8 | lo)
    uint16_t ix, iy, sp, pc;
    uint8_t i, r;
    bool iff1, iff2, halted;
    int im;
    int ei_delay;                     // EI: interrupts are taken only after the next instruction
    bool nmi_pending, irq_line;

    uint8_t (*read)(uint16_t addr);
    void (*write)(uint16_t addr, uint8_t v);
    uint8_t (*in)(uint16_t port);
    void (*out)(uint16_t port, uint8_t v);

    void reset();
    int step();                       // one instruction (or interrupt acceptance); returns T-states
    int run(int cycles);              // run at least 'cycles' T-states; returns the number used
    void nmi() { nmi_pending = true; }
};
