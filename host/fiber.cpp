// Fibers: POSIX ucontext on the desktop; on the Vita a small ARM context switch (vitasdk has no ucontext).
#ifndef DF_VITA
#include "fiber.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ucontext.h>

namespace fiber {
struct Fiber {
    ucontext_t ctx;
    void* stack;
    size_t size;
    void (*fn)(void*);
    void* arg;
};
static Fiber main_fiber;
static Fiber* cur;

Fiber* current() { if (!cur) cur = &main_fiber; return cur; }

static void trampoline(unsigned lo, unsigned hi) {
    Fiber* f = (Fiber*)(((uintptr_t)hi << 32) | lo);
    f->fn(f->arg);
    std::abort();   // fn must not return
}

Fiber* create(void (*fn)(void*), void* arg, size_t stack_bytes) {
    Fiber* f = new Fiber{};
    f->stack = std::malloc(stack_bytes);
    f->size = stack_bytes;
    std::memset(f->stack, 0xA5, stack_bytes);   // painted: high_water() finds the deepest use
    f->fn = fn; f->arg = arg;
    getcontext(&f->ctx);
    f->ctx.uc_stack.ss_sp = f->stack;
    f->ctx.uc_stack.ss_size = stack_bytes;
    f->ctx.uc_link = nullptr;
    uintptr_t p = (uintptr_t)f;
    makecontext(&f->ctx, (void (*)())trampoline, 2, (unsigned)(p & 0xFFFFFFFFu), (unsigned)(p >> 32));
    return f;
}

size_t high_water(Fiber* f) {
    if (!f->stack) return 0;
    const uint8_t* p = (const uint8_t*)f->stack;
    size_t i = 0;
    while (i < f->size && p[i] == 0xA5) ++i;
    return f->size - i;
}
void destroy(Fiber* f) { std::free(f->stack); delete f; }

void switch_to(Fiber* to) {
    Fiber* from = current();
    if (to == from) return;
    cur = to;
    swapcontext(&from->ctx, &to->ctx);
}
}
#endif

#ifdef DF_VITA
#include "fiber.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

// fiber_swap(save, to): pushes the AAPCS callee-saved registers (r4-r12, lr: 40 bytes, keeps 8-byte alignment;
// d8-d15) on the current stack, stores sp in *save, switches to 'to' and pops the same frame from there.
// A new fiber's stack holds such a frame whose lr is fiber_entry, which calls r5(r4).
extern "C" void fiber_swap(void** save, void* to);
extern "C" void fiber_entry();
asm(R"(
    .text
    .align 2
    .arm
    .global fiber_swap
    .type fiber_swap, %function
fiber_swap:
    push {r4-r12, lr}
    vpush {d8-d15}
    str sp, [r0]
    mov sp, r1
    vpop {d8-d15}
    pop {r4-r12, lr}
    bx lr
    .size fiber_swap, .-fiber_swap
    .global fiber_entry
    .type fiber_entry, %function
fiber_entry:
    mov r0, r4
    blx r5
    b .
    .size fiber_entry, .-fiber_entry
)");

namespace fiber {
struct Fiber {
    void* sp;
    void* stack;
};
static Fiber main_fiber;
static Fiber* cur;

// The Vita kernel stops a thread (reason 0x10006) whose sp is outside the stack it was created with, so fiber
// stacks cannot come from the heap: they are slots of a block inside the owning thread's own stack (set_pool).
static uint8_t* pool;
static size_t pool_slot, pool_count;
static std::vector<int> pool_free;

void set_pool(void* base, size_t bytes, size_t slot_bytes) {
    pool = (uint8_t*)(((uintptr_t)base + 15) & ~(uintptr_t)15);
    pool_slot = slot_bytes;
    pool_count = (bytes - (pool - (uint8_t*)base)) / slot_bytes;
    pool_free.clear();
    for (size_t i = pool_count; i-- > 0;) pool_free.push_back((int)i);
}

Fiber* current() { if (!cur) cur = &main_fiber; return cur; }

Fiber* create(void (*fn)(void*), void* arg, size_t stack_bytes) {
    if (!pool) { std::printf("fiber: no stack pool (set_pool not called)\n"); std::abort(); }
    if (stack_bytes > pool_slot || pool_free.empty()) { std::printf("fiber: out of stack slots (%u of %u KB)\n", (unsigned)pool_count, (unsigned)(pool_slot >> 10)); std::abort(); }
    Fiber* f = new Fiber{};
    f->stack = pool + (size_t)pool_free.back() * pool_slot;
    pool_free.pop_back();
    stack_bytes = pool_slot;
    uintptr_t top = ((uintptr_t)f->stack + stack_bytes) & ~(uintptr_t)15;
    uint32_t* frame = (uint32_t*)(top - 104);   // d8-d15 (64 bytes), then r4-r12, lr
    for (int i = 0; i < 16; ++i) frame[i] = 0;
    uint32_t* g = frame + 16;
    for (int i = 0; i < 9; ++i) g[i] = 0;
    g[0] = (uint32_t)(uintptr_t)arg;   // r4
    g[1] = (uint32_t)(uintptr_t)fn;    // r5
    g[9] = (uint32_t)(uintptr_t)&fiber_entry;   // lr
    f->sp = frame;
    return f;
}

void destroy(Fiber* f) { pool_free.push_back((int)(((uint8_t*)f->stack - pool) / pool_slot)); delete f; }

void switch_to(Fiber* to) {
    Fiber* from = current();
    if (to == from) return;
    cur = to;
    fiber_swap(&from->sp, to->sp);
}
}
#endif
