/* Crash reporting on POSIX (the Windows version, crash.c, also has a
   watchdog, a sampling profiler and a float-trap tool; those are not here yet). */
#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "grx_hwmem.h"
#include "grx_platform.h"

static void on_signal(int sig, siginfo_t *si, void *uc) {
    void *bt[48];
    int n;
    char what[96];
    uint32_t addr = (uint32_t)(uintptr_t)si->si_addr;
    (void)uc;

    fprintf(stderr, "grandia: signal %d (%s) at address 0x%llX, %s\n", sig, strsignal(sig),
            (unsigned long long)(uintptr_t)si->si_addr, grx_hwmem_describe(addr, what, sizeof(what)));
    n = backtrace(bt, 48);
    backtrace_symbols_fd(bt, n, STDERR_FILENO);
    fflush(stderr);
    _exit(128 + sig);
}

void grx_crash_init(void) {
    struct sigaction sa;
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};
    size_t i;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    /* The game's stacks are small mmaps; handle on an alternate stack so a
       stack overflow still reports */
    {
        static char altstack[64 * 1024];
        stack_t ss = {.ss_sp = altstack, .ss_size = sizeof(altstack), .ss_flags = 0};
        sigaltstack(&ss, NULL);
    }
    for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        sigaction(sigs[i], &sa, NULL);
    }
}

/* Before any other constructor (the game's data constructors run first
   otherwise), so a crash during start-up is reported too */
__attribute__((constructor(101))) static void crash_early(void) {
    grx_crash_init();
}

void grx_watchdog_start(unsigned seconds) {
    (void)seconds;
}

void grx_profile_start(void) {
}

void grx_fp_trap_field(unsigned long long field) {
    (void)field;
}
#endif /* !_WIN32 */
