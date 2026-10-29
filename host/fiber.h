// Fibers: separate host stacks that the emulation thread switches between by hand (no OS threads). Each
// recompiled CPU (68000, ARM7) runs on its own fiber, so it can stop in the middle of a call chain when its time
// slice ends (host/scheduler.cpp) and continue there later.
#pragma once
#include <cstddef>

namespace fiber {
struct Fiber;
Fiber* current();                                              // the running fiber (the thread's own stack the first time)
Fiber* create(void (*fn)(void*), void* arg, size_t stack_bytes);   // starts suspended; fn must never return
size_t high_water(Fiber* f);                                   // bytes of its stack ever used (desktop only)
void destroy(Fiber* f);                                        // f must be suspended, not running
void switch_to(Fiber* to);                                     // suspend the current fiber, run 'to'
#ifdef DF_VITA
void set_pool(void* base, size_t bytes, size_t slot_bytes);  // fiber stacks: slots of this block (on the thread's own stack)
#endif
}
