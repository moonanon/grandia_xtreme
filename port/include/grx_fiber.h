/* Execution contexts for the port's EE kernel (port/src/kernel.c): one per
   EE thread, switched cooperatively. On Windows they are fibers
   (port/src/fiber.c); a browser build would put something else behind the
   same interface. Kept apart from the kernel so that the file defining the
   game's CreateThread & co. never sees <windows.h>. */
#ifndef GRX_FIBER_H
#define GRX_FIBER_H

typedef struct GrxFiber GrxFiber;
typedef void (*GrxFiberFn)(void *arg);

/* The calling OS thread's own context (converted to a fiber on first use) */
GrxFiber *grx_fiber_main(void);

/* A new context that runs fn(arg) when first switched to; fn must never
   return. stack_size is the stack reserve in bytes (0: a default). */
GrxFiber *grx_fiber_create(unsigned int stack_size, GrxFiberFn fn, void *arg);

/* Suspend the running context and resume `to` */
void grx_fiber_switch(GrxFiber *to);

/* Free a context that isn't running. Deleting the main one is ignored. */
void grx_fiber_delete(GrxFiber *f);

#ifndef _WIN32
/* Run fn(argc, argv) on a stack below 2 GB (the game's addresses are 32-bit):
   what main() does first on POSIX. Returns fn's result. */
int grx_run_on_low_stack(int (*fn)(int, char **), int argc, char **argv);
#endif

#endif
