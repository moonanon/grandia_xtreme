#ifdef _WIN32
/* Crash reports: on an access violation (most often the game touching a
   PS2 hardware address the port doesn't handle yet), print where it
   happened and the code addresses on the stack, for llvm-symbolizer. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "grx_platform.h"
#include "grx_mips.h"
#include "grx_debug.h"

extern char __ImageBase;
uint64_t grx_frame_count(void);

/* The registers by the target's names (i686 or x86-64,
   port/docs/win64.md); addresses are printed as 0x%08llX */
#ifdef _WIN64
#define CTX_PC(c) ((c)->Rip)
#define CTX_SP(c) ((c)->Rsp)
#define CTX_FP_FLAGS CONTEXT_FLOATING_POINT
#else
#define CTX_PC(c) ((c)->Eip)
#define CTX_SP(c) ((c)->Esp)
#define CTX_FP_FLAGS CONTEXT_EXTENDED_REGISTERS
#endif
#define ADDR(a) ((unsigned long long)(a))

static int in_image_text(uintptr_t addr) {
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)&__ImageBase;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)dos + dos->e_lfanew);
    const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    int i;
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        uintptr_t lo = (uintptr_t)&__ImageBase + s->VirtualAddress;
        if ((s->Characteristics & IMAGE_SCN_MEM_EXECUTE) && addr >= lo && addr < lo + s->Misc.VirtualSize) {
            return 1;
        }
    }
    return 0;
}

static volatile LONG reporting;

/* The interpreter's PC if a disc program was running, then the code
   addresses on the stack from the stack pointer up to the end of its region (a thread's
   or a fiber's stack): without frame pointers, the return addresses */
static void report_where(uintptr_t stack) {
    MEMORY_BASIC_INFORMATION mi;
    const uintptr_t *sp = (const uintptr_t *)stack, *top = sp + 4096;
    int n;

    /* A disc program's wild pointer faults inside the interpreter: say
       where in the program it was */
    if (grx_mips_cpu()->steps != 0) {
        fprintf(stderr, "MIPS interpreter: last pc 0x%08X after %llu instructions\n", grx_mips_cpu()->pc,
                (unsigned long long)grx_mips_cpu()->steps);
    }
    if (VirtualQuery(sp, &mi, sizeof(mi)) && (const uintptr_t *)((const char *)mi.BaseAddress + mi.RegionSize) < top) {
        top = (const uintptr_t *)((const char *)mi.BaseAddress + mi.RegionSize);
    }
    {
        char exe[MAX_PATH];
        if (!GetModuleFileNameA(NULL, exe, sizeof(exe))) {
            strcpy(exe, "grandia.exe");
        }
        fprintf(stderr, "code addresses on the stack (llvm-symbolizer --obj=\"%s\" ...):\n", exe);
    }
    for (n = 0; sp < top; sp++) {
        if (in_image_text(*sp)) {
            fprintf(stderr, " 0x%08llX", ADDR(*sp));
            if (++n % 8 == 0) {
                fputc('\n', stderr);
            }
            if (n >= 32) {
                break;
            }
        }
    }
    fputc('\n', stderr);
    fflush(stderr);
}

static int is_float_trap(DWORD code) {
    return (code >= 0xC000008D && code <= 0xC0000093) || code == 0xC00002B4 || code == 0xC00002B5;
}

/* GRX_FP_TRAP (grx_fp_trap_field): each place that makes a NaN or divides
   by zero is reported once, with its stack, and the game carries on: the
   instruction runs again with the exceptions masked, single-stepped, and
   they are unmasked again after it. Float-to-int conversions are skipped:
   the compiler uses them on purpose (1 << i as a float's exponent). */
#define FP_TRAP_MASKS 0x280u /* invalid operation, division by zero */
#define FP_TRAP_SEEN 64
static uintptr_t fp_seen[FP_TRAP_SEEN];
static int fp_nseen, fp_stepping;

static DWORD *ctx_mxcsr(CONTEXT *c) {
#ifdef _WIN64
    return &c->MxCsr;
#else
    return (DWORD *)&c->ExtendedRegisters[24]; /* FXSAVE's MXCSR */
#endif
}

static LONG fp_trap(CONTEXT *c) {
    const unsigned char *op = (const unsigned char *)CTX_PC(c);
    int i, seen = 0;
    if (!(op[1] == 0x0F && op[2] == 0x5B && (op[0] == 0xF3 || op[0] == 0x66))) {
        for (i = 0; i < fp_nseen; i++) {
            seen |= fp_seen[i] == CTX_PC(c);
        }
        if (!seen && fp_nseen < FP_TRAP_SEEN) {
            fp_seen[fp_nseen++] = CTX_PC(c);
            fprintf(stderr, "fp-trap: field %llu, MXCSR flags %02lX at pc 0x%08llX\n",
                    (unsigned long long)grx_frame_count(), *ctx_mxcsr(c) & 0x3F, ADDR(CTX_PC(c)));
            report_where(CTX_SP(c));
        }
    }
    *ctx_mxcsr(c) = (*ctx_mxcsr(c) | FP_TRAP_MASKS) & ~0x3Fu;
    c->EFlags |= 0x100; /* trap after the instruction */
    fp_stepping = 1;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD main_thread;

/* A crash: say where, and end the process */
static LONG report(EXCEPTION_POINTERS *e) {
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    CONTEXT *c = e->ContextRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION && r->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION &&
        !is_float_trap(r->ExceptionCode)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (InterlockedExchange(&reporting, 1)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    fprintf(stderr, "grandia: exception 0x%08lX at pc 0x%08llX", r->ExceptionCode, ADDR(CTX_PC(c)));
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        fprintf(stderr, " (%s 0x%08llX)", r->ExceptionInformation[0] == 0 ? "reading" : r->ExceptionInformation[0] == 1 ? "writing" : "executing",
                ADDR(r->ExceptionInformation[1]));
    }
    fprintf(stderr, "%s\n", in_image_text(CTX_PC(c)) ? "" : " - pc is outside the code: a call through a bad pointer");
    report_where(CTX_SP(c));
    ExitProcess(3);
}

/* First, before any __try: the FP traps, and a crash on the game's thread
   (its code has no handlers). Another thread's exception may be a driver's
   or a system DLL's that it handles itself: those are left to their
   handlers, and reported by on_unhandled if none takes them. */
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *e) {
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    CONTEXT *c = e->ContextRecord;
    if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && fp_stepping) {
        fp_stepping = 0;
        *ctx_mxcsr(c) &= ~FP_TRAP_MASKS;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (is_float_trap(r->ExceptionCode) && (c->ContextFlags & CTX_FP_FLAGS) == CTX_FP_FLAGS) {
        return fp_trap(c);
    }
    if (GetCurrentThreadId() != main_thread) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    return report(e);
}

static LONG WINAPI on_unhandled(EXCEPTION_POINTERS *e) {
    return report(e);
}

/* A debugging build with GRX_PORT_CFLAGS=-fstack-protector-all (build.py):
   a function whose stack was overrun calls this as it returns. The
   toolchain's own ends the process without a word; this says which
   function it was (the return address is in it). */
void __stack_chk_fail(void) __attribute__((noreturn));
void __stack_chk_fail(void) {
    fprintf(stderr, "grandia: stack overrun found on return from the function at 0x%08llX\n",
            ADDR(__builtin_return_address(0)));
    report_where((uintptr_t)__builtin_frame_address(0));
    fflush(stderr);
    ExitProcess(3);
}

void grx_crash_init(void) {
    main_thread = GetCurrentThreadId();
    AddVectoredExceptionHandler(1, on_exception);
    SetUnhandledExceptionFilter(on_unhandled);
}

/* GRX_FP_TRAP=field: from that field on, report each place on the game's
   thread that makes a NaN or divides by zero (fp_trap). The EE's FPU has neither (it saturates), so the game's C code
   can make NaNs on the host that it never made on a PS2. Called from the
   frame loop, which runs on the game's thread. */
void grx_fp_trap_field(unsigned long long field) {
    static int64_t from = -2;
    if (from == -2) {
        const char *s = getenv("GRX_FP_TRAP");
        from = s != NULL ? atoll(s) : -1;
    }
    if (from >= 0 && field == (uint64_t)from && GetCurrentThreadId() == main_thread) {
        grx_log("fp-trap: trapping invalid operations and divisions by zero from field %llu",
                (unsigned long long)field);
        __builtin_ia32_ldmxcsr(__builtin_ia32_stmxcsr() & ~FP_TRAP_MASKS);
    }
}

/* The hang watchdog: when no field has run for `seconds`, the game is in
   a loop that neither waits on the kernel nor polls the frame loop. Stop
   the game's thread and report where it is, as for a crash. */

static HANDLE game_thread;
static DWORD watch_seconds;

static DWORD WINAPI watchdog(LPVOID arg) {
    uint64_t last = grx_frame_count();
    DWORD still = 0;
    (void)arg;
    for (;;) {
        uint64_t now;
        Sleep(1000);
        now = grx_frame_count();
        /* Frozen by the freeze tool on purpose: not a hang */
        still = now == last && !grx_dbg_frozen() ? still + 1 : 0;
        last = now;
        if (still >= watch_seconds && !InterlockedExchange(&reporting, 1)) {
            CONTEXT c __attribute__((aligned(16))); /* x86-64's GetThreadContext needs it */
            SuspendThread(game_thread);
            memset(&c, 0, sizeof(c));
            c.ContextFlags = CONTEXT_CONTROL;
            fprintf(stderr, "grandia: hung: no field for %lu s (field %llu)", watch_seconds, (unsigned long long)now);
            if (GetThreadContext(game_thread, &c)) {
                fprintf(stderr, ", pc 0x%08llX\n", ADDR(CTX_PC(&c)));
                report_where(CTX_SP(&c));
            } else {
                fprintf(stderr, ", couldn't read the thread's context\n");
            }
            ExitProcess(4);
        }
    }
}

static void open_game_thread(void) {
    if (game_thread == NULL) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &game_thread,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0);
    }
}

void grx_watchdog_start(unsigned seconds) {
    if (seconds == 0 || watch_seconds != 0) {
        return;
    }
    watch_seconds = seconds;
    open_game_thread();
    CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
}

/* A sampling profiler: GRX_PROFILE=<file> records the game thread's pc
   about once a millisecond, between fields GRX_PROFILE_FROM and
   GRX_PROFILE_TO, and writes them (one hex address a line) at exit, for
   llvm-symbolizer */
#define PROF_MAX 400000
static uintptr_t *prof_eips;
static volatile LONG prof_n;
static uint64_t prof_from, prof_to = ~0ull;
static const char *prof_path;

static DWORD WINAPI profiler(LPVOID arg) {
    (void)arg;
    for (;;) {
        uint64_t f = grx_frame_count();
        Sleep(1);
        if (f < prof_from || f >= prof_to || prof_n >= PROF_MAX || reporting) {
            continue;
        }
        if (SuspendThread(game_thread) != (DWORD)-1) {
            CONTEXT c __attribute__((aligned(16))); /* x86-64's GetThreadContext needs it */
            memset(&c, 0, sizeof(c));
            c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(game_thread, &c)) {
                prof_eips[prof_n++] = CTX_PC(&c);
            }
            ResumeThread(game_thread);
        }
    }
}

static void profile_write(void) {
    FILE *f = fopen(prof_path, "w");
    LONG i;
    if (f == NULL) {
        return;
    }
    for (i = 0; i < prof_n; i++) {
        fprintf(f, "0x%08llX\n", ADDR(prof_eips[i]));
    }
    fclose(f);
    fprintf(stderr, "profile: %ld samples in %s\n", (long)prof_n, prof_path);
}

void grx_profile_start(void) {
    const char *from = getenv("GRX_PROFILE_FROM"), *to = getenv("GRX_PROFILE_TO");
    prof_path = getenv("GRX_PROFILE");
    if (prof_path == NULL || prof_eips != NULL) {
        return;
    }
    prof_eips = malloc(PROF_MAX * sizeof(*prof_eips));
    if (prof_eips == NULL) {
        return;
    }
    prof_from = from ? strtoull(from, NULL, 10) : 0;
    prof_to = to ? strtoull(to, NULL, 10) : ~0ull;
    open_game_thread();
    atexit(profile_write);
    CloseHandle(CreateThread(NULL, 0, profiler, NULL, 0, NULL));
}

#endif /* _WIN32 */
