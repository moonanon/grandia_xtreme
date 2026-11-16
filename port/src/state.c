/*
 * Save states: port/include/grx_state.h, port/docs/savestates.md.
 *
 * When. Only between fields, from the kernel's idle hook (video.c), with
 * every game thread waiting and the game's main thread the only one there
 * is besides the idle loop: then the main thread is stopped at the same
 * place each field (waiting for the vblank), its stack holds the same
 * calls, and the idle hook runs on that stack, below them. A request made
 * while that isn't so (a load screen's extra threads, a movie) waits for
 * a field where it is, for a while.
 *
 * What. The game's memory: its two RAM blocks and the disc programs'
 * memory (at fixed addresses, grx_hwmem.h, so the absolute addresses in it
 * hold in another run), the hardware windows (EE registers, VU memory, GS
 * registers, scratchpad), and its data in the exe (between the
 * grx_state_begin and grx_state_end markers, build.py). The main thread's
 * stack, from grx_state_point up to grx_state_run_game: grx_state_point
 * pushes every callee-saved register first, so what the callers keep in
 * registers is in it too. Then the platform layer's parts that stand in
 * for the PS2's hardware, each through its own function (modules[]).
 *
 * Loading checks everything before it changes anything: the same exe (a
 * hash of its code), the same addresses, the same calls on the stack (the
 * call the main thread waits in, from the kernel, and the stack's depth),
 * each part's size. Then it copies memory, the parts,
 * and last the stack, over the frames of whoever called the idle hook;
 * grx_state_point pops the saved run's registers and returns into it.
 *
 * Files: savestates/slotN.grxstate next to grandia.exe, the data in runs
 * of zeros and literals (the heaps are mostly zeros).
 */
#define WIN32_LEAN_AND_MEAN
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "grx_hwmem.h"
#include "grx_memory.h"
#include "grx_mips.h"
#include "grx_movie.h"
#include "grx_platform.h"
#include "grx_state.h"
#include "grx_state_parts.h"

#define MAGIC "GRXSTATE"
#define VERSION 1
#define MAX_STACK (512 * 1024)
#define WAIT_FIELDS 600 /* a request that can't be carried out by then is dropped (10 s) */

extern char grx_state_data_begin[16], grx_state_data_end[16];
extern char grx_state_bss_begin[16], grx_state_bss_end[16];

/* ---------------------------------------------------------------------- */
/* The stream */

enum { MODE_SAVE, MODE_LOAD, MODE_MEASURE };

struct GrxState {
    int mode;
    uint8_t *buf;     /* saving: grows; loading: the file's data */
    size_t len, cap;  /* saving: used, allocated; loading: size */
    size_t pos;       /* loading, measuring */
    int bad;          /* loading: read past the end */
};

void grx_state_io(GrxState *s, void *p, size_t n) {
    switch (s->mode) {
    case MODE_SAVE:
        if (s->len + n > s->cap) {
            size_t cap = s->cap ? s->cap : (64u << 20);
            uint8_t *b;
            while (s->len + n > cap) {
                cap *= 2;
            }
            b = realloc(s->buf, cap);
            if (b == NULL) {
                grx_fatal("state: out of memory (%zu bytes)", cap);
            }
            s->buf = b;
            s->cap = cap;
        }
        memcpy(s->buf + s->len, p, n);
        s->len += n;
        break;
    case MODE_LOAD:
        if (s->pos + n > s->len) {
            s->bad = 1;
            return;
        }
        memcpy(p, s->buf + s->pos, n);
        s->pos += n;
        break;
    default:
        s->pos += n;
        break;
    }
}

int grx_state_loading(const GrxState *s) {
    return s->mode == MODE_LOAD;
}

static void put32(GrxState *s, uint32_t v) {
    grx_state_io(s, &v, 4);
}

static void put64(GrxState *s, uint64_t v) {
    grx_state_io(s, &v, 8);
}

static const uint8_t *take(GrxState *s, size_t n) {
    const uint8_t *p;
    if (s->pos + n > s->len) {
        s->bad = 1;
        return NULL;
    }
    p = s->buf + s->pos;
    s->pos += n;
    return p;
}

static uint32_t get32(GrxState *s) {
    const uint8_t *p = take(s, 4);
    uint32_t v = 0;
    if (p) {
        memcpy(&v, p, 4);
    }
    return v;
}

static uint64_t get64(GrxState *s) {
    const uint8_t *p = take(s, 8);
    uint64_t v = 0;
    if (p) {
        memcpy(&v, p, 8);
    }
    return v;
}

/* ---------------------------------------------------------------------- */
/* Messages (the window title shows the last one for a few seconds) */

static char message[160];
static uint64_t message_ms;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    message_ms = SDL_GetTicks();
    grx_log("state: %s", message);
}

const char *grx_state_message(void) {
    return message[0] && SDL_GetTicks() - message_ms < 3000 ? message : NULL;
}

/* ---------------------------------------------------------------------- */
/* This exe: a hash of its code */

static uint64_t build_id;

#ifdef _WIN32
static void exe_info(void) {
    const uint8_t *base;
    const IMAGE_NT_HEADERS *nt;
    const IMAGE_SECTION_HEADER *sec;
    int i;
    if (build_id) {
        return;
    }
    base = (const uint8_t *)GetModuleHandleA(NULL);
    nt = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    sec = IMAGE_FIRST_SECTION(nt);
    build_id = 0xCBF29CE484222325ull; /* FNV-1a over .text */
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (memcmp(sec[i].Name, ".text", 6) == 0) {
            const uint8_t *p = base + sec[i].VirtualAddress;
            size_t n = sec[i].Misc.VirtualSize, k;
            for (k = 0; k < n; k++) {
                build_id = (build_id ^ p[k]) * 0x100000001B3ull;
            }
        }
    }
    build_id ^= (uint64_t)nt->OptionalHeader.SizeOfImage << 1;
}
#else
extern const char __executable_start[], etext[];

static void exe_info(void) {
    const uint8_t *p = (const uint8_t *)__executable_start;
    size_t n = (size_t)(etext - __executable_start), k;
    if (build_id) {
        return;
    }
    build_id = 0xCBF29CE484222325ull; /* FNV-1a over the code */
    for (k = 0; k < n; k++) {
        build_id = (build_id ^ p[k]) * 0x100000001B3ull;
    }
    build_id ^= (uint64_t)n << 1;
}
#endif

/* ---------------------------------------------------------------------- */
/* The memory saved as it is */

typedef struct {
    const char *name;
    uintptr_t addr;
    size_t size;
} Region;

static int regions(Region *r, int max) {
    int n = 0, i;
    const GrxHwRange *hw;
#define ADD(nm, a, sz) do { if (n < max) { r[n].name = (nm); r[n].addr = (uintptr_t)(a); r[n].size = (sz); n++; } } while (0)
    ADD("exe data", grx_state_data_begin, (size_t)(grx_state_data_end + 16 - grx_state_data_begin));
    ADD("exe bss", grx_state_bss_begin, (size_t)(grx_state_bss_end + 16 - grx_state_bss_begin));
    ADD("RAM 0x00100000", grx_ee_area(GRX_AREA_INI_BUF), 0x00300000u);
    ADD("RAM 0x00760000", grx_ee_area(GRX_AREA_HEAP7), grx_ee_area_size(GRX_AREA_HEAP7));
    for (i = 0; i < GRX_HW_NRANGES; i++) {
        hw = grx_hwmem_range(i);
        if (hw->mapped) {
            ADD(hw->name, hw->base, hw->size);
        }
    }
    ADD("directory scratch", GRX_FIX_DIR_SCRATCH, 0x20000u);
    ADD("disc programs' window", GRX_FIX_MIPS_WINDOW, GRX_MIPS_WIN_SIZE);  /* grx_programs_init first */
    ADD("disc programs' stack", GRX_FIX_MIPS_STACK, 0x40000u);
#undef ADD
    return n;
}

/* ---------------------------------------------------------------------- */
/* Compression: runs of zero bytes (in 4-byte units) and of literals */

static void compress(const uint8_t *in, size_t n, GrxState *out) {
    size_t i = 0;
    while (i < n) {
        size_t z = i, l;
        while (z + 4 <= n && in[z] == 0 && in[z + 1] == 0 && in[z + 2] == 0 && in[z + 3] == 0) {
            z += 4;
        }
        l = z;
        /* literals up to the next run of 16 zero bytes */
        while (l < n) {
            if (l + 16 <= n && ((const uint32_t *)(in + l))[0] == 0 && memcmp(in + l, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) == 0) {
                break;
            }
            l++;
        }
        put32(out, (uint32_t)(z - i));
        put32(out, (uint32_t)(l - z));
        grx_state_io(out, (void *)(in + z), l - z);
        i = l;
    }
}

static int decompress(GrxState *in, uint8_t *out, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint32_t z = get32(in), l = get32(in);
        const uint8_t *p;
        if (in->bad || z > n - i || l > n - i - z) {
            return 0;
        }
        memset(out + i, 0, z);
        i += z;
        p = take(in, l);
        if (p == NULL) {
            return 0;
        }
        memcpy(out + i, p, l);
        i += l;
    }
    return 1;
}

/* ---------------------------------------------------------------------- */
/* Where the stack is */

static uintptr_t game_top;

int grx_state_run_game(int (*entry)(int, char **), int argc, char **argv) {
    volatile char mark = 0;
    game_top = (uintptr_t)&mark;
    return entry(argc, argv);
}

/* grx_state_point(): pushes every callee-saved register, then calls
   grx_state_run(the stack pointer above them); pops them on the way back.
   After a load those are the saved run's. */
void grx_state_point(void);
void grx_state_run(uintptr_t boundary);
#if !defined(_WIN32) && defined(__x86_64__)
/* System V: only rbx, rbp and r12-r15 are callee-saved, no xmm */
__asm__(".text\n"
        ".globl grx_state_point\n"
        ".type grx_state_point, @function\n"
        "grx_state_point:\n"
        "  pushq %rbp\n  pushq %rbx\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
        "  subq $8, %rsp\n"
        "  movq %rsp, %rdi\n"
        "  call grx_state_run\n"
        "  addq $8, %rsp\n"
        "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rbx\n  popq %rbp\n"
        "  ret\n"
        ".size grx_state_point, .-grx_state_point\n"
        ".section .note.GNU-stack,\"\",@progbits\n"
        ".text\n");
#elif defined(_WIN64)
__asm__(".text\n"
        ".globl grx_state_point\n"
        ".def grx_state_point; .scl 2; .type 32; .endef\n"
        ".seh_proc grx_state_point\n"
        "grx_state_point:\n"
        "  pushq %rbp\n  .seh_pushreg %rbp\n"
        "  pushq %rbx\n  .seh_pushreg %rbx\n"
        "  pushq %rdi\n  .seh_pushreg %rdi\n"
        "  pushq %rsi\n  .seh_pushreg %rsi\n"
        "  pushq %r12\n  .seh_pushreg %r12\n"
        "  pushq %r13\n  .seh_pushreg %r13\n"
        "  pushq %r14\n  .seh_pushreg %r14\n"
        "  pushq %r15\n  .seh_pushreg %r15\n"
        "  subq $200, %rsp\n  .seh_stackalloc 200\n"
        "  movaps %xmm6, 32(%rsp)\n  .seh_savexmm %xmm6, 32\n"
        "  movaps %xmm7, 48(%rsp)\n  .seh_savexmm %xmm7, 48\n"
        "  movaps %xmm8, 64(%rsp)\n  .seh_savexmm %xmm8, 64\n"
        "  movaps %xmm9, 80(%rsp)\n  .seh_savexmm %xmm9, 80\n"
        "  movaps %xmm10, 96(%rsp)\n  .seh_savexmm %xmm10, 96\n"
        "  movaps %xmm11, 112(%rsp)\n  .seh_savexmm %xmm11, 112\n"
        "  movaps %xmm12, 128(%rsp)\n  .seh_savexmm %xmm12, 128\n"
        "  movaps %xmm13, 144(%rsp)\n  .seh_savexmm %xmm13, 144\n"
        "  movaps %xmm14, 160(%rsp)\n  .seh_savexmm %xmm14, 160\n"
        "  movaps %xmm15, 176(%rsp)\n  .seh_savexmm %xmm15, 176\n"
        "  .seh_endprologue\n"
        "  leaq 32(%rsp), %rcx\n"
        "  call grx_state_run\n"
        "  movaps 32(%rsp), %xmm6\n"
        "  movaps 48(%rsp), %xmm7\n"
        "  movaps 64(%rsp), %xmm8\n"
        "  movaps 80(%rsp), %xmm9\n"
        "  movaps 96(%rsp), %xmm10\n"
        "  movaps 112(%rsp), %xmm11\n"
        "  movaps 128(%rsp), %xmm12\n"
        "  movaps 144(%rsp), %xmm13\n"
        "  movaps 160(%rsp), %xmm14\n"
        "  movaps 176(%rsp), %xmm15\n"
        "  addq $200, %rsp\n"
        "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n"
        "  popq %rsi\n  popq %rdi\n  popq %rbx\n  popq %rbp\n"
        "  ret\n"
        ".seh_endproc\n");
#else
__asm__(".text\n"
        ".globl _grx_state_point\n"
        "_grx_state_point:\n"
        "  pushl %ebp\n  pushl %ebx\n  pushl %esi\n  pushl %edi\n"
        "  movl %esp, %eax\n"
        "  subl $12, %esp\n"
        "  pushl %eax\n"
        "  call _grx_state_run\n"
        "  addl $16, %esp\n"
        "  popl %edi\n  popl %esi\n  popl %ebx\n  popl %ebp\n"
        "  ret\n");
#endif

/* ---------------------------------------------------------------------- */
/* The parts of the platform layer that keep the machine's state */

typedef struct {
    const char *name;
    void (*io)(GrxState *s);
    void (*after_load)(void);
} Part;

static const Part parts[] = {
    GRX_STATE_PARTS
};

static const char *(*const blockers[])(void) = {
    GRX_STATE_BLOCKERS
};

/* ---------------------------------------------------------------------- */
/* Requests */

static int req_slot, req_load, req_fields;

void grx_state_request(int slot, int load) {
    req_slot = slot;
    req_load = load;
    req_fields = 0;
}

static void path_of(int slot, char *out, size_t n) {
    const char *exe = SDL_GetBasePath();
    snprintf(out, n, "%ssavestates/slot%d.grxstate", exe ? exe : "", slot);
}

/* Why the game can't be saved or loaded here, or NULL */
static const char *not_here(void) {
    static char why[160];
    const char *u = grx_hwmem_unfixed();
    size_t k;
    if (u != NULL) {
        snprintf(why, sizeof(why), "%s isn't at its fixed address in this run", u);
        return why;
    }
    if (game_top == 0) {
        return "the game hasn't started";
    }
    if (grx_movie_playing()) {
        return "a movie is playing";
    }
    for (k = 0; k < sizeof(blockers) / sizeof(blockers[0]); k++) {
        const char *b = blockers[k]();
        if (b != NULL) {
            return b;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Saving */

static GrxState *pending_save; /* filled by grx_state_run */

static int save_to(GrxState *s, uintptr_t boundary) {
    Region r[16];
    int n = regions(r, 16), i;
    size_t k;
    exe_info();
    grx_state_io(s, MAGIC, 8);
    put32(s, VERSION);
    put32(s, (uint32_t)sizeof(void *));
    put64(s, build_id);
    put64(s, (uint64_t)boundary);
    put64(s, (uint64_t)game_top);
    put64(s, (uint64_t)(uintptr_t)grx_kernel_main_wait_site());
    put32(s, (uint32_t)n);
    for (i = 0; i < n; i++) {
        put64(s, (uint64_t)r[i].addr);
        put64(s, (uint64_t)r[i].size);
        compress((const uint8_t *)r[i].addr, r[i].size, s);
    }
    put32(s, (uint32_t)(sizeof(parts) / sizeof(parts[0])));
    for (k = 0; k < sizeof(parts) / sizeof(parts[0]); k++) {
        size_t at;
        grx_state_io(s, (void *)parts[k].name, strlen(parts[k].name) + 1);
        at = s->len;
        put32(s, 0);
        if (parts[k].io != NULL) {
            parts[k].io(s);
        }
        {
            uint32_t sz = (uint32_t)(s->len - at - 4);
            memcpy(s->buf + at, &sz, 4);
        }
    }
    put64(s, (uint64_t)(game_top - boundary));
    grx_state_io(s, (void *)boundary, game_top - boundary);
    return 1;
}

static int write_file(int slot, const GrxState *s) {
    char path[1024], tmp[1040];
    const char *exe = SDL_GetBasePath();
    char dir[1024];
    FILE *f;
    snprintf(dir, sizeof(dir), "%ssavestates", exe ? exe : "");
    #ifdef _WIN32
    CreateDirectoryA(dir, NULL);
#else
    mkdir(dir, 0777);
#endif
    path_of(slot, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (f == NULL) {
        return 0;
    }
    /* All of it on the disk before it replaces the slot: on a full disk the
       CRT's buffered tail fails at fflush/fclose, and a short .tmp would
       replace a good state */
    if (fwrite(s->buf, 1, s->len, f) != s->len || fflush(f) != 0 ||
#ifdef _WIN32
        !FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(f)))
#else
        fsync(fileno(f)) != 0
#endif
        ) {
        fclose(f);
        remove(tmp);
        return 0;
    }
    if (fclose(f) != 0) {
        remove(tmp);
        return 0;
    }
#ifdef _WIN32
    return MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(tmp, path) == 0;
#endif
}

/* ---------------------------------------------------------------------- */
/* Loading */

static GrxState loaded;     /* the file, read before the point */
static int load_slot;
static int loaded_now;      /* set by grx_state_run once it has loaded */
static const char *load_problem;

static int read_file(int slot, GrxState *s) {
    char path[1024];
    FILE *f;
    long n;
    path_of(slot, path, sizeof(path));
    f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    s->buf = malloc(n > 0 ? (size_t)n : 1);
    if (s->buf == NULL || fread(s->buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(s->buf);
        s->buf = NULL;
        return 0;
    }
    fclose(f);
    s->mode = MODE_LOAD;
    s->len = (size_t)n;
    s->pos = 0;
    s->bad = 0;
    return 1;
}

/* The size of the part name at s->pos, its NUL included; 0 when there's
   no NUL before the end of the file (damaged) */
static size_t name_size(const GrxState *s) {
    const uint8_t *nul;
    if (s->pos >= s->len) {
        return 0;
    }
    nul = memchr(s->buf + s->pos, 0, s->len - s->pos);
    return nul != NULL ? (size_t)(nul - (s->buf + s->pos)) + 1 : 0;
}

/* Everything checked, nothing changed: NULL, or what doesn't match */
static const char *check(GrxState *s, uintptr_t boundary) {
    static char why[200];
    Region r[16];
    int n = regions(r, 16), i;
    uint32_t nparts;
    size_t k;
    const uint8_t *m = take(s, 8);
    uint64_t sz;
    exe_info();
    if (m == NULL || memcmp(m, MAGIC, 8) != 0 || get32(s) != VERSION) {
        return "not a save state of this version";
    }
    if (get32(s) != sizeof(void *)) {
        return "saved by the other (32/64-bit) build";
    }
    if (get64(s) != build_id) {
        return "saved by another build of grandia.exe";
    }
    if (get64(s) != boundary || get64(s) != game_top) {
        /* boundary to game_top: the stack's depth, the same at the same call */
        return "the game is waiting somewhere else (try again in a moment)";
    }
    if (get64(s) != (uint64_t)(uintptr_t)grx_kernel_main_wait_site()) {
        return "the game is waiting somewhere else (try again in a moment)";
    }
    if ((int)get32(s) != n) {
        return "the memory is laid out differently";
    }
    for (i = 0; i < n; i++) {
        uint64_t a = get64(s), size = get64(s);
        size_t done = 0;
        if (a != r[i].addr || size != r[i].size) {
            snprintf(why, sizeof(why), "%s is elsewhere in this run", r[i].name);
            return why;
        }
        /* skip the data */
        while (done < size) {
            uint32_t z = get32(s), l = get32(s);
            if (s->bad || z > size - done || l > size - done - z || take(s, l) == NULL) {
                return "the file is damaged";
            }
            done += z + l;
        }
    }
    nparts = get32(s);
    if (nparts != sizeof(parts) / sizeof(parts[0])) {
        return "saved with other parts";
    }
    for (k = 0; k < nparts; k++) {
        const char *name = (const char *)(s->buf + s->pos);
        size_t nl = name_size(s);
        GrxState measure = {MODE_MEASURE, NULL, 0, 0, 0, 0};
        uint32_t size;
        if (nl == 0) {
            return "the file is damaged";
        }
        if (strcmp(name, parts[k].name) != 0) {
            return "saved with other parts";
        }
        take(s, nl);
        size = get32(s);
        if (parts[k].io != NULL) {
            parts[k].io(&measure);
        }
        if (measure.pos != size || take(s, size) == NULL) {
            snprintf(why, sizeof(why), "the %s part's size differs", parts[k].name);
            return why;
        }
    }
    sz = get64(s);
    if (sz != game_top - boundary || s->pos + sz > s->len) {
        return "the file is damaged";
    }
    return s->bad ? "the file is damaged" : NULL;
}

static void apply(GrxState *s) {
    Region r[16];
    int n = regions(r, 16), i;
    size_t k;
    uint32_t nparts;
    s->pos = 8 + 4 + 4 + 8 + 8 + 8 + 8;
    get32(s);
    for (i = 0; i < n; i++) {
        get64(s);
        get64(s);
        if (!decompress(s, (uint8_t *)r[i].addr, r[i].size)) {
            grx_fatal("state: damaged while loading %s", r[i].name);
        }
    }
    nparts = get32(s);
    for (k = 0; k < nparts; k++) {
        size_t nl = name_size(s);
        if (nl == 0) {
            grx_fatal("state: damaged while loading the parts");
        }
        take(s, nl);
        get32(s);
        if (parts[k].io != NULL) {
            parts[k].io(s);
        }
    }
    {
        uint64_t sz = get64(s);
        /* last: the frames of the callers, below which this runs */
        memcpy((void *)(game_top - (uintptr_t)sz), s->buf + s->pos, (size_t)sz);
    }
}

/* Called by grx_state_point with the stack pointer above the registers it
   pushed */
void grx_state_run(uintptr_t boundary) {
    if (game_top <= boundary || game_top - boundary > MAX_STACK) {
        load_problem = "the game's stack isn't where it should be";
        return;
    }
    if (pending_save != NULL) {
        save_to(pending_save, boundary);
        return;
    }
    load_problem = check(&loaded, boundary);
    if (load_problem == NULL) {
        loaded_now = 1;
        apply(&loaded);
    }
}

/* After a load grx_state_point returns into the frames of the saved run,
   which was saving: its locals there are that run's. So what follows a
   load is done from here, on statics only (load_slot, loaded_now). */

static void finish_load(void) {
    size_t k;
    free(loaded.buf);
    memset(&loaded, 0, sizeof(loaded));
    for (k = 0; k < sizeof(parts) / sizeof(parts[0]); k++) {
        if (parts[k].after_load != NULL) {
            parts[k].after_load();
        }
    }
    say("loaded slot %d", load_slot);
}

int grx_state_service(void) {
    const char *why;
    int slot = req_slot, load = req_load;
    GrxState s = {MODE_SAVE, NULL, 0, 0, 0, 0};
    if (slot == 0) {
        return 0;
    }
    /* Both runs have the disc programs' memory (state regions); set up
       first, so not_here() sees whether it got its fixed addresses (once
       only: it does nothing when the programs are up) */
    grx_programs_init();
    why = not_here();
    if (why != NULL) {
        if (req_fields == 0) {
            grx_log("state: slot %d waits: %s", slot, why);
        }
        if (++req_fields >= WAIT_FIELDS) {
            say("slot %d: can't %s now (%s)", slot, load ? "load" : "save", why);
            req_slot = 0;
        }
        return 0;
    }
    req_slot = 0;
    if (load) {
        memset(&loaded, 0, sizeof(loaded));
        if (!read_file(slot, &loaded)) {
            say("slot %d is empty", slot);
            return 1;
        }
        load_slot = slot;
    }
    pending_save = load ? NULL : &s;
    load_problem = NULL;
    /* One call site for both: a load checks that the stack holds the same
       calls as the saved one, return addresses included */
    grx_state_point();
    if (loaded_now) {
        /* The saved run's frames, back from its save: statics only */
        loaded_now = 0;
        finish_load();
        return 1;
    }
    pending_save = NULL;
    if (load) {
        /* Nothing changed: load_problem says why */
        free(loaded.buf);
        memset(&loaded, 0, sizeof(loaded));
        say("slot %d: can't load (%s)", slot, load_problem ? load_problem : "?");
        return 1;
    }
    if (load_problem != NULL) {
        say("slot %d: not saved (%s)", slot, load_problem);
    } else if (!write_file(slot, &s)) {
        say("slot %d: can't write the file", slot);
    } else {
        say("saved to slot %d", slot);
    }
    free(s.buf);
    return 1;
}
