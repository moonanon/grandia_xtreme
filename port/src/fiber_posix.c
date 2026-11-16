/* ucontext behind grx_fiber.h, the EE kernel's thread contexts on POSIX.
   The game keeps addresses in 32-bit ints, its locals' too, so every stack
   it runs on is mapped below 2 GB , the main thread's included:
   grx_run_on_low_stack() moves main() onto one before anything else. */
#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <ucontext.h>
#include "grx_fiber.h"
#include "grx_platform.h"

#define DEFAULT_STACK (1024 * 1024)
#define MAIN_STACK (16 * 1024 * 1024)

struct GrxFiber {
    ucontext_t ctx;
    void *stack;
    size_t stack_size;
    GrxFiberFn fn;
    void *arg;
    int is_main;
};

static GrxFiber main_fiber;
static int main_ready;

/* Stacks go at fixed addresses, in the order they are made: a save state
   (port/src/state.c) holds a run of the stack, with absolute addresses in it,
   and loads in a later run only if the stacks are where they were. The range
   is free of the PS2's windows (0x10000000-0x12000000, 0x20000000 and
   0x30000000 aliases of RAM, 0x70000000 scratchpad) and of the exe at
   0x04000000. */
#define STACK_REGION 0x50000000u
#define STACK_REGION_END 0x6F000000u
static uintptr_t next_stack = STACK_REGION;

static void *low_stack(size_t size) {
    void *p;
    size = (size + 0xFFFF) & ~(size_t)0xFFFF;
    while (next_stack + size <= STACK_REGION_END) {
        uintptr_t at = next_stack;
        next_stack += size + 0x10000; /* a gap after each: an overflow faults */
        p = mmap((void *)at, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_STACK, -1, 0);
        if (p == (void *)at) {
            return p;
        }
        if (p != MAP_FAILED) {
            munmap(p, size);
        }
    }
    return NULL;
}

static void check_stack_low(const char *what) {
    volatile char here;
    if ((uintptr_t)&here >= 0x80000000u) {
        grx_fatal("fiber: %s's stack is at 0x%llX, above 2 GB, where the game's 32-bit addresses can't reach", what,
                  (unsigned long long)(uintptr_t)&here);
    }
}

static void fiber_proc(unsigned lo, unsigned hi) {
    GrxFiber *f = (GrxFiber *)(((uintptr_t)hi << 32) | lo);
    check_stack_low("a thread");
    f->fn(f->arg);
    grx_fatal("fiber: a context's function returned");
}

GrxFiber *grx_fiber_main(void) {
    if (!main_ready) {
        main_ready = 1;
        main_fiber.is_main = 1;
        check_stack_low("the main thread");
    }
    return &main_fiber;
}

GrxFiber *grx_fiber_create(unsigned int stack_size, GrxFiberFn fn, void *arg) {
    GrxFiber *f = calloc(1, sizeof(*f));
    if (f == NULL) {
        grx_fatal("fiber: out of memory");
    }
    if (stack_size == 0) {
        stack_size = DEFAULT_STACK;
    }
    f->fn = fn;
    f->arg = arg;
    f->stack_size = stack_size;
    f->stack = low_stack(stack_size);
    if (f->stack == NULL) {
        grx_fatal("fiber: no room below 2 GB for a %u-byte stack", stack_size);
    }
    getcontext(&f->ctx);
    f->ctx.uc_stack.ss_sp = f->stack;
    f->ctx.uc_stack.ss_size = stack_size;
    f->ctx.uc_link = NULL;
    makecontext(&f->ctx, (void (*)(void))fiber_proc, 2, (unsigned)(uintptr_t)f, (unsigned)((uintptr_t)f >> 32));
    return f;
}

static GrxFiber *running;

void grx_fiber_switch(GrxFiber *to) {
    GrxFiber *from = running != NULL ? running : &main_fiber;
    running = to;
    swapcontext(&from->ctx, &to->ctx);
}

void grx_fiber_delete(GrxFiber *f) {
    if (f == NULL || f->is_main) {
        return;
    }
    munmap(f->stack, f->stack_size);
    free(f);
}

/* main() on a stack below 2 GB */
static struct {
    int (*fn)(int, char **);
    int argc;
    char **argv;
    int ret;
    ucontext_t from, to;
} trampoline;

static void run_trampoline(void) {
    trampoline.ret = trampoline.fn(trampoline.argc, trampoline.argv);
    swapcontext(&trampoline.to, &trampoline.from);
}

int grx_run_on_low_stack(int (*fn)(int, char **), int argc, char **argv) {
    void *stack = low_stack(MAIN_STACK);
    if (stack == NULL) {
        return fn(argc, argv);
    }
    trampoline.fn = fn;
    trampoline.argc = argc;
    trampoline.argv = argv;
    getcontext(&trampoline.to);
    trampoline.to.uc_stack.ss_sp = stack;
    trampoline.to.uc_stack.ss_size = MAIN_STACK;
    trampoline.to.uc_link = &trampoline.from;
    makecontext(&trampoline.to, run_trampoline, 0);
    swapcontext(&trampoline.from, &trampoline.to);
    return trampoline.ret;
}
#endif /* !_WIN32 */
