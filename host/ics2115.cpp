// ICS2115 (see ics2115.h).
#include "ics2115.h"
#include <cstring>

ICS2115::ICS2115() {
    // volume: 4.8 exponential (exponent bits 11-8, mantissa 7-0), as measured on hardware
    for (int i = 0; i < 4096; i++) {
        int ex = i >> 8, mant = i & 0xff;
        volume[i] = ex == 0 ? (uint16_t)(mant >> 7) : (uint16_t)((((0x100 | mant) << (ex - 1)) + 0xff) >> 8);
    }
    // u-law (MIL-STD-188-113), scaled to 16 bits
    uint16_t lut[8];
    for (int i = 0; i < 8; i++) lut[i] = (uint16_t)(((33 << 2) << i) - (33 << 2));
    for (int i = 0; i < 256; i++) {
        int ex = (~i >> 4) & 7, mant = ~i & 15;
        int16_t val = (int16_t)(lut[ex] + (mant << (ex + 3)));
        ulaw[i] = (i & 0x80) ? (int16_t)-val : val;
        int bw = 0;
        for (int x = i; x; x >>= 1) bw++;
        panlaw[i] = (uint16_t)(16 - (bw - 1));   // pan attenuation: 16 - log2(i)
    }
    panlaw[0] = 0xfff;
    reset();
}

void ICS2115::reset() {
    std::memset(v, 0, sizeof v);
    std::memset(timer, 0, sizeof timer);
    irq_enabled = irq_pending = 0;
    irq_on = false;
    active_osc = 31;
    osc_select = reg_select = 0;
    for (Voice& vc : v) {
        vc.osc_conf = 2; vc.osc_ctrl = 2;
        vc.pan = 0x7f; vc.vol_ctrl = 1;
    }
}

void ICS2115::recalc_irq() {
    bool irq = irq_pending & irq_enabled;
    for (int i = 0; !irq && i < 32; i++)
        irq = ((v[i].osc_conf & 0x20) && (v[i].osc_conf & 0x80)) || ((v[i].vol_ctrl & 0x20) && (v[i].vol_ctrl & 0x80));
    irq_on = irq;
}

void ICS2115::recalc_timer(int t) {
    uint64_t period = (uint64_t)((timer[t].scale & 0x1f) + 1) * (timer[t].preset + 1);
    period <<= 4 + (timer[t].scale >> 5);
    if (timer[t].period != period) { timer[t].period = period; timer[t].count = 0; }
}

void ICS2115::advance(uint32_t clocks) {
    for (int t = 0; t < 2; t++) {
        if (!timer[t].period) continue;
        timer[t].count += clocks;
        if (timer[t].count >= timer[t].period) {
            timer[t].count %= timer[t].period;
            if (!(irq_pending & (1 << t))) { irq_pending |= (uint8_t)(1 << t); recalc_irq(); }
        }
    }
}

// ---- voices ----
bool ICS2115::step_osc(Voice& vc) {
    bool irq = false;
    if (vc.osc_conf & 0x40) { vc.osc_acc -= (uint32_t)vc.fc << 2; vc.osc_left = (int32_t)(vc.osc_acc - vc.osc_start); }
    else { vc.osc_acc += (uint32_t)vc.fc << 2; vc.osc_left = (int32_t)(vc.osc_end - vc.osc_acc); }
    if (vc.osc_left > 0) return false;
    if (vc.osc_conf & 0x20) { vc.osc_conf |= 0x80; irq = true; }
    if (vc.osc_conf & 0x08) {   // loop
        if (vc.osc_conf & 0x10) vc.osc_conf ^= 0x40;   // bidirectional: turn around
        if (vc.osc_conf & 0x40) { vc.osc_acc = vc.osc_end + vc.osc_left; vc.osc_left = (int32_t)(vc.osc_acc - vc.osc_start); }
        else { vc.osc_acc = vc.osc_start - vc.osc_left; vc.osc_left = (int32_t)(vc.osc_end - vc.osc_acc); }
    } else {
        vc.osc_ctrl |= 1;   // done
        vc.osc_acc = (vc.osc_conf & 0x40) ? vc.osc_start : vc.osc_end;
    }
    return irq;
}

bool ICS2115::step_vol(Voice& vc) {
    if (vc.vol_ctrl & 3) return false;   // done / stopped
    if (vc.vol_ctrl & 0x40) { vc.vol_acc -= vc.vol_add; vc.vol_left = vc.vol_acc - vc.vol_start; }
    else { vc.vol_acc += vc.vol_add; vc.vol_left = vc.vol_end - vc.vol_acc; }
    if (vc.vol_left > 0) return false;
    bool irq = false;
    if (vc.vol_ctrl & 0x20) { vc.vol_ctrl |= 0x80; irq = true; }
    if (vc.vol_ctrl & 0x08) {   // loop (the boundary is always crossed here)
        int32_t inc = vc.vol_incr;
        if (!(vc.vol_ctrl & 0x10)) {
            if (!(vc.vol_ctrl & 0x40)) vc.vol_acc = vc.vol_start - (vc.vol_end - (vc.vol_acc + inc));
            else vc.vol_acc = vc.vol_end + ((vc.vol_acc - inc) - vc.vol_start);
        } else {
            if (!(vc.vol_ctrl & 0x40)) vc.vol_acc = vc.vol_end + (vc.vol_end - (vc.vol_acc + inc));
            else vc.vol_acc = vc.vol_start - ((vc.vol_acc - inc) - vc.vol_start);
        }
    } else {
        vc.vol_ctrl |= 1;
    }
    return irq;
}

int32_t ICS2115::sample(Voice& vc) {
    uint32_t cur = vc.osc_acc >> 12, next;
    if (vc.osc_ctrl == 0 && (vc.osc_conf & 0x08) && !(vc.osc_conf & 0x10) && vc.osc_left < ((int32_t)vc.fc << 2))
        next = vc.osc_start >> 12;   // about to loop: interpolate towards the loop start
    else
        next = cur + 2;
    int32_t s1, s2;
    if (vc.osc_conf & 0x01) {
        s1 = ulaw[rom_byte(vc, cur)];
        s2 = ulaw[rom_byte(vc, cur + 1)];
    } else if (!(vc.osc_conf & 0x02)) {
        s1 = (int8_t)rom_byte(vc, cur) * 256;
        s2 = (int8_t)rom_byte(vc, cur + 1) * 256;
    } else {
        s1 = rom_byte(vc, cur) | ((int8_t)rom_byte(vc, cur + 1) * 256);
        s2 = rom_byte(vc, next) | ((int8_t)rom_byte(vc, next + 1) * 256);
    }
    int32_t fract = (vc.osc_acc & 0xff8) >> 3;
    return ((s1 << 9) + (s2 - s1) * fract) >> 9;
}

void ICS2115::render(int32_t* left, int32_t* right, int n) {
    bool irq = false;
    for (int o = 0; o <= active_osc; o++) {
        Voice& vc = v[o];
        if (!vc.playing()) continue;
        // volume increment: what the hardware's 'fine' encoding gives on the reference implementation
        int fine = 1 << (3 * (vc.vol_incr >> 6));
        vc.vol_add = (vc.vol_incr & 0x3f) << ((10 - fine) & 31);
        for (int i = 0; i < n && vc.playing(); i++) {
            int32_t va = (vc.vol_acc >> 14) & 0xfff;
            int32_t li = va - panlaw[255 - vc.pan], ri = va - panlaw[vc.pan];
            int32_t vl = li > 0 ? volume[li] : 0, vr = ri > 0 ? volume[ri] : 0;
            int32_t s = sample(vc);
            left[i] += (s * vl) >> 15;
            right[i] += (s * vr) >> 15;
            if (step_osc(vc)) irq = true;
            if (step_vol(vc)) irq = true;
        }
    }
    if (irq) recalc_irq();
}

// ---- registers ----
uint16_t ICS2115::reg_read() {
    Voice& vc = v[osc_select];
    uint16_t r = 0;
    switch (reg_select) {
    case 0x00: r = vc.osc_conf; if (vc.osc_ctrl == 0) r |= 8; else r &= ~8; r <<= 8; break;
    case 0x01: r = vc.fc; break;
    case 0x02: r = (uint16_t)(vc.osc_start >> 16); break;
    case 0x03: r = (uint16_t)(vc.osc_start & 0xff00); break;
    case 0x04: r = (uint16_t)(vc.osc_end >> 16); break;
    case 0x05: r = (uint16_t)(vc.osc_end & 0xff00); break;
    case 0x06: r = vc.vol_incr; break;
    case 0x07: r = (uint16_t)(vc.vol_start >> 18); break;
    case 0x08: r = (uint16_t)(vc.vol_end >> 18); break;
    case 0x09: r = (uint16_t)(vc.vol_acc >> 10); break;
    case 0x0a: r = (uint16_t)(vc.osc_acc >> 16); break;
    case 0x0b: r = (uint16_t)(vc.osc_acc & 0xfff8); break;
    case 0x0c: r = (uint16_t)(vc.pan << 8); break;
    case 0x0d: r = 0x01; if (vc.vol_ctrl & 0x20) r |= 0x80; r <<= 8; break;
    case 0x0e: r = active_osc; break;
    case 0x0f:   // interrupt source: the first voice with a pending IRQ, acknowledged by this read
        r = 0xff;
        for (int i = 0; i <= active_osc; i++) {
            Voice& w = v[i];
            if ((w.osc_conf & 0x80) || (w.vol_ctrl & 0x80)) {
                r = (uint16_t)(i | 0xe0);
                if (w.osc_conf & 0x80) { w.osc_conf &= 0x7f; r &= ~0x80; }
                if (w.vol_ctrl & 0x80) { w.vol_ctrl &= 0x7f; r &= ~0x40; }
                recalc_irq();
                break;
            }
        }
        r <<= 8;
        break;
    case 0x10: r = (uint16_t)(vc.osc_ctrl << 8); break;
    case 0x11: r = (uint16_t)(vc.saddr << 8); break;
    case 0x40: case 0x41:   // timer: reading clears its IRQ
        r = timer[reg_select & 1].preset;
        irq_pending &= (uint8_t)~(1 << (reg_select & 1));
        recalc_irq();
        break;
    case 0x43: r = irq_pending & 3; break;
    case 0x4a: r = irq_pending; break;
    case 0x4b: r = 0x80; break;
    case 0x4c: r = 0x01; break;   // revision
    default: r = 0; break;
    }
    return r;
}

void ICS2115::reg_write(uint16_t data, uint16_t mask) {
    Voice& vc = v[osc_select];
    const bool hi = mask & 0xff00, lo = mask & 0x00ff;
    switch (reg_select) {
    case 0x00: if (hi) { vc.osc_conf = (uint8_t)((vc.osc_conf & 0x80) | ((data >> 8) & 0x7f)); recalc_irq(); } break;
    case 0x01:
        if (hi) vc.fc = (uint16_t)((vc.fc & 0x00fe) | (data & 0xff00));
        if (lo) vc.fc = (uint16_t)((vc.fc & 0xff00) | (data & 0x00fe));
        break;
    case 0x02:
        if (hi) vc.osc_start = (vc.osc_start & 0x00ffffff) | ((uint32_t)(data & 0xff00) << 16);
        if (lo) vc.osc_start = (vc.osc_start & 0xff00ffff) | ((uint32_t)(data & 0x00ff) << 16);
        break;
    case 0x03: if (hi) vc.osc_start = (vc.osc_start & 0xffff00ff) | (data & 0xff00); break;
    case 0x04:
        if (hi) vc.osc_end = (vc.osc_end & 0x00ffffff) | ((uint32_t)(data & 0xff00) << 16);
        if (lo) vc.osc_end = (vc.osc_end & 0xff00ffff) | ((uint32_t)(data & 0x00ff) << 16);
        break;
    case 0x05: if (hi) vc.osc_end = (vc.osc_end & 0xffff00ff) | (data & 0xff00); break;
    case 0x06: if (hi) vc.vol_incr = (uint8_t)(data >> 8); break;
    case 0x07: if (lo) vc.vol_start = (data & 0xff) << 18; break;
    case 0x08: if (lo) vc.vol_end = (data & 0xff) << 18; break;
    case 0x09:
        if (hi) vc.vol_regacc = (uint16_t)((vc.vol_regacc & 0x00ff) | (data & 0xff00));
        if (lo) vc.vol_regacc = (uint16_t)((vc.vol_regacc & 0xff00) | (data & 0x00ff));
        vc.vol_acc = vc.vol_regacc << 10;
        break;
    case 0x0a:
        if (hi) vc.osc_acc = (vc.osc_acc & 0x00ffffff) | ((uint32_t)(data & 0xff00) << 16);
        if (lo) vc.osc_acc = (vc.osc_acc & 0xff00ffff) | ((uint32_t)(data & 0x00ff) << 16);
        break;
    case 0x0b:
        if (hi) vc.osc_acc = (vc.osc_acc & 0xffff00ff) | (data & 0xff00);
        if (lo) vc.osc_acc = (vc.osc_acc & 0xffffff00) | (data & 0x00f8);
        break;
    case 0x0c: if (hi) vc.pan = (uint8_t)(data >> 8); break;
    case 0x0d: if (hi) { vc.vol_ctrl = (uint8_t)((vc.vol_ctrl & 0x80) | ((data >> 8) & 0x7f)); recalc_irq(); } break;
    case 0x0e: if (hi) active_osc = (uint8_t)((data >> 8) & 0x1f); break;
    case 0x10:
        if (hi) {
            vc.osc_ctrl = (uint8_t)((data >> 8) & 3);
            if (vc.osc_ctrl & 2) vc.vol_ctrl |= 2;   // stop: also stop the volume ramp
        }
        break;
    case 0x11: if (hi) vc.saddr = (uint8_t)(data >> 8); break;
    case 0x12: if (hi) vc.vol_mode = (uint8_t)(data >> 8); break;
    case 0x40: case 0x41: if (lo) { timer[reg_select & 1].preset = (uint8_t)data; recalc_timer(reg_select & 1); } break;
    case 0x42: case 0x43: if (lo) { timer[reg_select & 1].scale = (uint8_t)data; recalc_timer(reg_select & 1); } break;
    case 0x4a: if (lo) { irq_enabled = (uint8_t)data; recalc_irq(); } break;
    case 0x4f: if (lo) osc_select = (uint8_t)((data & 0xff) % (active_osc + 1)); break;
    default: break;
    }
}

uint8_t ICS2115::read(int offset) {
    switch (offset & 3) {
    case 0: {
        uint8_t r = 0;
        if (irq_on) {
            r |= 0x80;
            if (irq_enabled && (irq_pending & 3)) r |= 1;
            for (int i = 0; i <= active_osc; i++)
                if (v[i].osc_conf & 0x80) { r |= 2; break; }
        }
        return r;
    }
    case 1: return reg_select;
    case 2: return (uint8_t)reg_read();
    default: return (uint8_t)(reg_read() >> 8);
    }
}

void ICS2115::write(int offset, uint8_t data) {
    switch (offset & 3) {
    case 1: reg_select = data; break;
    case 2: reg_write(data, 0x00ff); break;
    case 3: reg_write((uint16_t)(data << 8), 0xff00); break;
    default: break;
    }
}
