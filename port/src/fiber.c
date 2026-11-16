#ifdef _WIN32
/* Windows fibers behind grx_fiber.h, the EE kernel's thread contexts.
   This file may include <windows.h>; kernel.c (which defines the game's
   CreateThread, the EE kernel call) must not. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include "grx_fiber.h"
#include "grx_platform.h"

#define DEFAULT_STACK (1024 * 1024)
#define STACK_COMMIT (64 * 1024)

struct GrxFiber {
    void *handle;
    GrxFiberFn fn;
    void *arg;
    int is_main;
};

static GrxFiber main_fiber;

/* The game keeps addresses in 32-bit ints, its locals' too, so on x86-64
   every stack it runs on must be below 2 GB (port/docs/win64.md) */
static void check_stack_low(const char *what) {
#ifdef _WIN64
    volatile char here;
    if ((uintptr_t)&here >= 0x80000000u) {
        grx_fatal("fiber: %s's stack is at 0x%llX, above 2 GB, where the game's 32-bit addresses can't reach",
                  what, (unsigned long long)(uintptr_t)&here);
    }
#else
    (void)what;
#endif
}

static void WINAPI fiber_proc(void *param) {
    GrxFiber *f = param;
    check_stack_low("a thread");
    f->fn(f->arg);
    grx_fatal("fiber: a context's function returned");
}

GrxFiber *grx_fiber_main(void) {
    if (main_fiber.handle == NULL) {
        if (IsThreadAFiber()) {
            main_fiber.handle = GetCurrentFiber();
        } else {
            /* FIBER_FLAG_FLOAT_SWITCH: each context keeps its own x87/SSE
               control state */
            main_fiber.handle = ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
        }
        if (main_fiber.handle == NULL) {
            grx_fatal("fiber: ConvertThreadToFiberEx failed (%lu)", GetLastError());
        }
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
    f->handle = CreateFiberEx(STACK_COMMIT < stack_size ? STACK_COMMIT : stack_size, stack_size,
                              FIBER_FLAG_FLOAT_SWITCH, fiber_proc, f);
    if (f->handle == NULL) {
        grx_fatal("fiber: CreateFiberEx failed (%lu)", GetLastError());
    }
    return f;
}

void grx_fiber_switch(GrxFiber *to) {
    SwitchToFiber(to->handle);
}

void grx_fiber_delete(GrxFiber *f) {
    if (f == NULL || f->is_main) {
        return;
    }
    DeleteFiber(f->handle);
    free(f);
}

#endif /* _WIN32 */
