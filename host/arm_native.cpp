// Native stand-ins for ARM-mode C-library routines in the ARM ROM (reached from Thumb through 'bx pc').
#include "arm_funcs.h"
#include <cstdio>

// 0x08017ca0: semihosting call (ARM SWI 0x123456, the debugger's "Angel" channel): r0 = operation, r1 = argument.
// On the real board nothing answers it; we print what the game would have sent to a debugger.
void a_08017ca0(arm::Ctx* c) {
    uint32_t op = c->r[0], arg = c->r[1];
    switch (op) {
    case 0x03: std::putchar((int)arm::rd8(arg)); break;                                   // SYS_WRITEC
    case 0x04: for (uint32_t a = arg; uint32_t ch = arm::rd8(a); ++a) std::putchar((int)ch); break;   // SYS_WRITE0
    default: std::printf("[arm] semihosting op %02x arg %08x (lr %08x)\n", op, arg, c->r[14]); break;
    }
    c->r[0] = 0;
}
