/* The R5900 interpreter for the disc's MIPS programs (port/include/grx_mips.h).

   What it implements is what the 486 programs use (scanned from the disc,
   see port/tests/mips_test.c) plus their close relatives: the integer ISA
   with the 64-bit operations, the R5900's three-operand mult and the MMI
   pipeline-1 ops (madd, mflo1, div1, ...), the FPU's single precision
   with the EE's edge cases (no infinities, NaNs or denormals: results
   clamp to +-FLT_MAX or flush to zero, x/0 is +-FLT_MAX, sqrt ignores the
   sign, cvt.w.s saturates), and VU0 macro mode on the port's VU0 state
   (grx_vf/grx_vi, port/include/grx_ee.h), since the game's own C uses the
   same registers. Anything else stops with the instruction. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "grx_ee.h"
#include "grx_mips.h"
#include "grx_platform.h"
#include "grx_state_parts.h"

uint64_t grx_mips_step_limit;
int grx_mips_log_calls;
int grx_mips_check_code;
uint8_t *grx_mips_null_page;
int (*grx_mips_host_check)(uint32_t addr);

static GrxMipsCpu cpu;
static int depth;

/* ------------------------------------------------------------------ memory */

static uint8_t *win;
static uint32_t win_host; /* (uint32_t) of win */
static uint32_t stack_lo, stack_hi;
static int strict;
static int ready;

static struct {
    uint32_t size;     /* of the program loaded there (0: unknown) */
    uint32_t code_end; /* offset where its data starts, once entered */
    uint32_t gen;      /* bumped on each load */
} areas[GRX_MIPS_NAREAS];

/* The bridge */
static const GrxMipsHandler *handlers;
static void *const *func_hosts;
static void *const *data_hosts;
/* ABI globals sorted by host address: indexes into grx_mips_abi_data */
static int *data_by_host;
static int data_by_host_n;
static uint32_t data_ee_lo, data_ee_hi, data_host_lo, data_host_hi;
/* ABI functions with a native address, sorted by it */
static int *func_by_host;
static int func_by_host_n;
static uint32_t func_host_lo = 0xFFFFFFFFu, func_host_hi; /* empty */

/* thunks.c */
void grx_mips_thunks_init(void);
uint32_t grx_mips_thunks_lo(void);
uint32_t grx_mips_thunks_hi(void);
void grx_mips_thunks_area_reloaded(void);

static inline int in_window(uint32_t a) { return a - GRX_MIPS_WIN_EE < GRX_MIPS_WIN_SIZE; }
static inline int in_host_window(uint32_t a) { return a - win_host < GRX_MIPS_WIN_SIZE; }
static inline int in_stack(uint32_t a) { return a - stack_lo < stack_hi - stack_lo; }

/* The ABI global holding an EE address (an end pointer counts, unless
   another object starts there), or -1 */
static int data_at_ee(uint32_t a) {
    int lo = 0, hi = grx_mips_abi_ndata - 1, best = -1;
    if (a - data_ee_lo > data_ee_hi - data_ee_lo) {
        return -1;
    }
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (grx_mips_abi_data[mid].ee <= a) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (best >= 0 && a - grx_mips_abi_data[best].ee <= grx_mips_abi_data[best].size) {
        return best;
    }
    return -1;
}

static int data_at_host(uint32_t a) {
    int lo = 0, hi = data_by_host_n - 1, best = -1;
    if (!data_by_host || a - data_host_lo > data_host_hi - data_host_lo) {
        return -1;
    }
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if ((uint32_t)(uintptr_t)data_hosts[data_by_host[mid]] <= a) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (best >= 0) {
        int k = data_by_host[best];
        if (a - (uint32_t)(uintptr_t)data_hosts[k] <= grx_mips_abi_data[k].size) {
            return k;
        }
    }
    return -1;
}

static int func_at_ee(uint32_t a) {
    int lo = 0, hi = grx_mips_abi_nfuncs - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (grx_mips_abi_funcs[mid].addr == a) {
            return mid;
        }
        if (grx_mips_abi_funcs[mid].addr < a) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -1;
}

static int func_at_host(uint32_t a) {
    int lo = 0, hi = func_by_host_n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t h = (uint32_t)(uintptr_t)func_hosts[func_by_host[mid]];
        if (h == a) {
            return func_by_host[mid];
        }
        if (h < a) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -1;
}

const char *grx_mips_describe(uint32_t a, char *buf, size_t n) {
    int k = data_at_ee(a), i;
    uint32_t best = 0;
    const char *bn = 0;
    if (k >= 0) {
        snprintf(buf, n, "%s+0x%X", grx_mips_abi_data[k].name, a - grx_mips_abi_data[k].ee);
        return buf;
    }
    if (in_window(a)) {
        uint32_t off = a - GRX_MIPS_WIN_EE;
        snprintf(buf, n, "%s program area %u + 0x%X", off < 0x20000 ? "party" : off < 0x40000 ? "enemy" : "magic",
                 off < 0x40000 ? (off >> 15) & 3 : 0, off & 0x7FFF);
        return buf;
    }
    for (i = 0; i < grx_mips_abi_nfuncs; i++) {
        if (grx_mips_abi_funcs[i].addr <= a && grx_mips_abi_funcs[i].addr >= best) {
            best = grx_mips_abi_funcs[i].addr;
            bn = grx_mips_abi_funcs[i].name;
        }
    }
    for (i = 0; i < grx_mips_abi_ndata; i++) {
        if (grx_mips_abi_data[i].ee <= a && grx_mips_abi_data[i].ee >= best) {
            best = grx_mips_abi_data[i].ee;
            bn = grx_mips_abi_data[i].name;
        }
    }
    if (bn && a >= GRX_MIPS_EE_LOW && a < GRX_MIPS_EE_HIGH) {
        snprintf(buf, n, "not an ABI address; the nearest ABI name below is %s (0x%08X)", bn, best);
    } else {
        snprintf(buf, n, "a host address");
    }
    return buf;
}

static void fault(const char *what, uint32_t a) __attribute__((noreturn));
static void fault(const char *what, uint32_t a) {
    char d[128];
    grx_fatal("mips: %s 0x%08X (%s) at pc 0x%08X", what, a, grx_mips_describe(a, d, sizeof(d)), cpu.pc - 4);
}

static uint8_t *ee_slow(uint32_t a) {
    int k = data_at_ee(a);
    if (k >= 0) {
        return (uint8_t *)data_hosts[k] + (a - grx_mips_abi_data[k].ee);
    }
    if (strict) {
        fault("access to the EE address", a);
    }
    return (uint8_t *)(uintptr_t)a;
}

/* Host pointer for an access at EE-form address a */
static inline uint8_t *mem(uint32_t a) {
    uint32_t off = a - GRX_MIPS_WIN_EE;
    if (off < GRX_MIPS_WIN_SIZE) {
        return win + off;
    }
    if (a - GRX_MIPS_EE_LOW < GRX_MIPS_EE_HIGH - GRX_MIPS_EE_LOW) {
        return ee_slow(a);
    }
    /* A null pointer (plus or minus a field offset), or the EE's kernel
       segments: nothing the port has */
    if (a - 0xFFFF0000u < 0x20000u) {
        if (grx_mips_null_page) {
            return grx_mips_null_page + (a & 0xFFFF); /* tests */
        }
        fault("access to", a);
    }
    if (a >= 0x80000000u || (grx_mips_host_check && !grx_mips_host_check(a))) {
        fault("access to", a);
    }
    return (uint8_t *)(uintptr_t)a;
}

void *grx_mips_mem(uint32_t addr, uint32_t size) {
    (void)size;
    return mem(addr);
}

void *grx_mips_area(uint32_t ee) {
    if (!in_window(ee)) {
        grx_fatal("mips: 0x%08X isn't in the programs' load areas", ee);
    }
    return win + (ee - GRX_MIPS_WIN_EE);
}

/* ------------------------------------------------------------------ forms */

static uint32_t code_end_of(uint32_t a) {
    return areas[(a - GRX_MIPS_WIN_EE) >> 15].code_end;
}

/* GRX_MIPS_TRACE=1: log each area's code end when it is prepared, and
   program addresses converted while their area's code end isn't known */
static int mips_trace(void) {
    static int on = -1;
    if (on < 0) {
        const char *t = getenv("GRX_MIPS_TRACE");
        on = t != NULL && *t != '0';
    }
    return on;
}

static inline uint32_t host_form(uint32_t v, int code) {
    if (in_window(v)) {
        if ((v & 0x7FFF) < code_end_of(v)) {
            return code ? (uint32_t)(uintptr_t)grx_mips_thunk(v) : v;
        }
        if (code && code_end_of(v) == 0 && mips_trace()) {
            grx_log("mips: 0x%08X converted as data: its area isn't prepared yet", v);
        }
        return v - GRX_MIPS_WIN_EE + win_host;
    }
    if (v - data_ee_lo <= data_ee_hi - data_ee_lo) {
        int k = data_at_ee(v);
        if (k >= 0) {
            return (uint32_t)(uintptr_t)data_hosts[k] + (v - grx_mips_abi_data[k].ee);
        }
    }
    /* An ABI function's address (BtlUnitStateMgcDie, MgcPtclGroupsAreDone
       are taken with lui/addiu): the port's function */
    if (v - GRX_MIPS_EE_LOW < GRX_MIPS_EE_HIGH - GRX_MIPS_EE_LOW && func_hosts) {
        int k = func_at_ee(v);
        if (k >= 0 && func_hosts[k]) {
            return (uint32_t)(uintptr_t)func_hosts[k];
        }
    }
    return v;
}

static inline uint32_t ee_form(uint32_t v) {
    if (in_host_window(v)) {
        return v - win_host + GRX_MIPS_WIN_EE;
    }
    if (v - grx_mips_thunks_lo() < grx_mips_thunks_hi() - grx_mips_thunks_lo()) {
        uint32_t t = grx_mips_thunk_target((const void *)(uintptr_t)v);
        if (t) {
            return t;
        }
    }
    if (v - data_host_lo <= data_host_hi - data_host_lo) {
        int k = data_at_host(v);
        if (k >= 0) {
            return grx_mips_abi_data[k].ee + (v - (uint32_t)(uintptr_t)data_hosts[k]);
        }
    }
    if (v - func_host_lo <= func_host_hi - func_host_lo) {
        int k = func_at_host(v);
        if (k >= 0) {
            return grx_mips_abi_funcs[k].addr;
        }
    }
    return v;
}

uint32_t grx_mips_to_host(uint32_t v) { return (v & 3) ? v : host_form(v, 1); }
uint32_t grx_mips_to_host_any(uint32_t v) { return host_form(v, (v & 3) == 0); }
uint32_t grx_mips_to_ee(uint32_t v) { return (v & 3) ? v : ee_form(v); }
uint32_t grx_mips_to_ee_any(uint32_t v) { return ee_form(v); }

/* A word stored at host address dst */
static inline uint32_t store_form(uint32_t v, uint32_t dst) {
    if (v & 3) {
        return v;
    }
    return host_form(v, !in_stack(dst));
}

static inline uint32_t load_form(uint32_t v) {
    return (v & 3) ? v : ee_form(v);
}

/* ------------------------------------------------------------------ areas */

uint32_t grx_mips_code_end(uint32_t ee) {
    return in_window(ee) ? code_end_of(ee) : 0;
}

void grx_mips_area_loaded(uint32_t ee, uint32_t size) {
    int i;
    if (!in_window(ee)) {
        grx_fatal("mips: 0x%08X isn't in the programs' load areas", ee);
    }
    i = (ee - GRX_MIPS_WIN_EE) >> 15;
    areas[i].size = size > GRX_MIPS_AREA_SIZE ? GRX_MIPS_AREA_SIZE : size;
    areas[i].code_end = 0;
    areas[i].gen++;
    grx_mips_thunks_area_reloaded();
}

uint32_t grx_mips_area_gen(uint32_t ee) {
    return areas[(ee - GRX_MIPS_WIN_EE) >> 15].gen;
}

/* Where an area's code ends: the last `jr $ra` (and its delay slot)
   before the first "save" string, which starts the program's .rodata
   (tools/abi_scan.py) */
static uint32_t find_code_end(const uint8_t *p, uint32_t size) {
    uint32_t limit = size, a, end = 0;
    for (a = 0; a + 5 <= size; a++) {
        if (memcmp(p + a, "save", 5) == 0) {
            limit = a;
            break;
        }
    }
    for (a = 0; a + 4 <= limit; a += 4) {
        uint32_t w;
        memcpy(&w, p + a, 4);
        if (w == 0x03E00008u) {
            end = a + 8;
        }
    }
    /* Functions after the last `jr $ra` that are only a tail call into
       the game (`j` to an address in 0x00400000-0x00760000, then its delay
       slot) are code too: the small GBX programs hand such a function's
       address to BATTLE as a callback (BtlCharDef.setState) */
    for (;;) {
        uint32_t w, target;
        if (end + 8 > limit) {
            break;
        }
        memcpy(&w, p + end, 4);
        target = (w & 0x03FFFFFFu) << 2;
        if ((w >> 26) != 2 || target < GRX_MIPS_EE_LOW || target >= GRX_MIPS_EE_HIGH) {
            break;
        }
        end += 8;
    }
    return end;
}

/* An area's program is entered: find its code, and turn the pointers in
   its data into host form (once: what it changes is no longer EE form) */
static void prepare(int i) {
    uint8_t *p = win + i * GRX_MIPS_AREA_SIZE;
    uint32_t base = GRX_MIPS_WIN_EE + i * GRX_MIPS_AREA_SIZE;
    uint32_t size = areas[i].size ? areas[i].size : GRX_MIPS_AREA_SIZE;
    uint32_t a;
    if (areas[i].code_end) {
        return;
    }
    areas[i].code_end = find_code_end(p, size);
    if (areas[i].code_end == 0) {
        areas[i].code_end = 4; /* nothing found: the entry is code */
    }
    if (mips_trace()) {
        grx_log("mips: area %d (0x%08X) prepared: size 0x%X%s, code ends at +0x%X", i, base, size,
                areas[i].size ? "" : " (unknown, whole area)", areas[i].code_end);
        /* Small programs: the instructions, to check the code end */
        if (size <= 0x200) {
            for (a = 0; a + 4 <= size && a < areas[i].code_end + 0x40; a += 4) {
                uint32_t w;
                char d[96];
                memcpy(&w, p + a, 4);
                grx_log("mips:   +%03X %08X %s", a, w, grx_mips_disasm(w, base + a, d, sizeof(d)));
            }
        }
    }
    for (a = (areas[i].code_end + 3) & ~3u; a + 4 <= size; a += 4) {
        uint32_t w, h;
        memcpy(&w, p + a, 4);
        if (w & 3) {
            continue;
        }
        if (in_window(w)) {
            if (w - base >= size || w - base < areas[i].code_end) {
                continue; /* another area, or code (jump tables): left in EE form */
            }
        }
        h = host_form(w, 0);
        if (h != w) {
            memcpy(p + a, &h, 4);
        }
    }
}

/* ------------------------------------------------------------------ setup */

static void sort_by(int *idx, int n, void *const *addrs) {
    int i, j;
    for (i = 1; i < n; i++) {
        int k = idx[i];
        uint32_t v = (uint32_t)(uintptr_t)addrs[k];
        for (j = i - 1; j >= 0 && (uint32_t)(uintptr_t)addrs[idx[j]] > v; j--) {
            idx[j + 1] = idx[j];
        }
        idx[j + 1] = k;
    }
}

static int overlaps_ee(uint32_t a, uint32_t size) {
    return a < GRX_MIPS_EE_HIGH && a + size > GRX_MIPS_EE_LOW;
}

void grx_mips_setup(void *window, void *stk, uint32_t stack_size, int strict_ee) {
    int i;
    if (ready) {
        grx_fatal("mips: set up twice");
    }
    win = window;
    win_host = (uint32_t)(uintptr_t)window;
    stack_lo = (uint32_t)(uintptr_t)stk;
    stack_hi = stack_lo + stack_size;
    if (overlaps_ee(win_host, GRX_MIPS_WIN_SIZE) || overlaps_ee(stack_lo, stack_size)) {
        grx_fatal("mips: the window (0x%08X) or stack (0x%08X) is inside the EE range 0x%08X-0x%08X", win_host,
                  stack_lo, GRX_MIPS_EE_LOW, GRX_MIPS_EE_HIGH);
    }
    strict = strict_ee;
    for (i = 1; i < grx_mips_abi_nfuncs; i++) {
        if (grx_mips_abi_funcs[i].addr <= grx_mips_abi_funcs[i - 1].addr) {
            grx_fatal("mips: grx_mips_abi_funcs isn't sorted");
        }
    }
    for (i = 1; i < grx_mips_abi_ndata; i++) {
        if (grx_mips_abi_data[i].ee < grx_mips_abi_data[i - 1].ee + grx_mips_abi_data[i - 1].size) {
            grx_fatal("mips: grx_mips_abi_data isn't sorted");
        }
    }
    if (grx_mips_abi_ndata) {
        data_ee_lo = grx_mips_abi_data[0].ee;
        data_ee_hi = grx_mips_abi_data[grx_mips_abi_ndata - 1].ee + grx_mips_abi_data[grx_mips_abi_ndata - 1].size;
    }
    grx_mips_thunks_init();
    ready = 1;
}

int grx_mips_ready(void) {
    return ready;
}

void grx_mips_set_bridge(const GrxMipsHandler *h, void *const *fh, void *const *dh) {
    static int dbh[4096], fbh[4096];
    int i;
    handlers = h;
    func_hosts = fh;
    data_hosts = dh;
    if (grx_mips_abi_ndata > 4096 || grx_mips_abi_nfuncs > 4096) {
        grx_fatal("mips: ABI tables too big");
    }
    data_by_host_n = 0;
    data_host_lo = 0xFFFFFFFFu;
    data_host_hi = 0;
    for (i = 0; dh && i < grx_mips_abi_ndata; i++) {
        uint32_t a = (uint32_t)(uintptr_t)dh[i];
        if (!a) {
            continue;
        }
        if (overlaps_ee(a, grx_mips_abi_data[i].size)) {
            grx_fatal("mips: the port's %s (0x%08X) is inside the EE range", grx_mips_abi_data[i].name, a);
        }
        dbh[data_by_host_n++] = i;
        if (a < data_host_lo) {
            data_host_lo = a;
        }
        if (a + grx_mips_abi_data[i].size > data_host_hi) {
            data_host_hi = a + grx_mips_abi_data[i].size;
        }
    }
    sort_by(dbh, data_by_host_n, dh);
    data_by_host = dbh;
    func_by_host_n = 0;
    for (i = 0; fh && i < grx_mips_abi_nfuncs; i++) {
        if (fh[i]) {
            fbh[func_by_host_n++] = i;
        }
    }
    sort_by(fbh, func_by_host_n, fh);
    func_by_host = fbh;
    func_host_lo = 0xFFFFFFFFu;
    func_host_hi = 0;
    if (func_by_host_n) {
        func_host_lo = (uint32_t)(uintptr_t)fh[fbh[0]];
        func_host_hi = (uint32_t)(uintptr_t)fh[fbh[func_by_host_n - 1]];
    }
}

GrxMipsCpu *grx_mips_cpu(void) {
    return &cpu;
}

/* ------------------------------------------------------------------ handlers */

static uint32_t int_arg(GrxMipsCpu *c, int n) {
    if (n < 0 || n > 7) {
        grx_fatal("mips: %s: integer argument %d", c->cur ? c->cur->name : "?", n);
    }
    return c->r[4 + n].w[0];
}

int32_t grx_mips_arg_i(GrxMipsCpu *c, int n) {
    uint32_t v = int_arg(c, n), h;
    /* An integer that is an address of the program or of an ABI global:
       the prototype says s32 for what is a pointer (gen_mips_bridge.py
       finds most of them; OVERRIDES has the rest). The native code would
       use the EE address, which is nothing in the port, so it gets the
       host form, as a pointer argument would (4-aligned values only, as
       for stores). Said once per function and argument. */
    if ((v & 3) == 0 && v && (h = host_form(v, 1)) != v) {
        static uint32_t said[64];
        static int nsaid;
        uint32_t key = (c->cur ? c->cur->addr : 0) ^ (uint32_t)n << 28;
        int k;
        for (k = 0; k < nsaid && said[k] != key; k++) {
        }
        if (k == nsaid && nsaid < 64) {
            char d[96];
            said[nsaid++] = key;
            grx_log("mips: %s: integer argument %d is 0x%08X (%s): passed as a pointer (add it to "
                    "gen_mips_bridge.py's OVERRIDES)", c->cur ? c->cur->name : "?", n + 1, v,
                    grx_mips_describe(v, d, sizeof(d)));
        }
        v = h;
    }
    return (int32_t)v;
}

void *grx_mips_arg_p(GrxMipsCpu *c, int n) {
    return (void *)(uintptr_t)grx_mips_to_host_any(int_arg(c, n));
}

int32_t grx_mips_arg_v(GrxMipsCpu *c, int n) {
    return (int32_t)grx_mips_to_host_any(int_arg(c, n));
}

int64_t grx_mips_arg_l(GrxMipsCpu *c, int n) {
    int_arg(c, n);
    return c->r[4 + n].sd[0];
}

double grx_mips_arg_d(GrxMipsCpu *c, int n) {
    double d;
    int_arg(c, n);
    memcpy(&d, &c->r[4 + n].d[0], 8);
    return d;
}

float grx_mips_arg_f(GrxMipsCpu *c, int n) {
    if (n < 0 || n > 7) {
        grx_fatal("mips: %s: float argument %d", c->cur ? c->cur->name : "?", n);
    }
    return c->f[12 + n].f;
}

void grx_mips_ret_i(GrxMipsCpu *c, int32_t v) {
    c->r[2].sd[0] = v;
}

void grx_mips_ret_p(GrxMipsCpu *c, const void *p) {
    c->r[2].sd[0] = (int32_t)grx_mips_to_ee_any((uint32_t)(uintptr_t)p);
}

void grx_mips_ret_l(GrxMipsCpu *c, int64_t v) {
    c->r[2].sd[0] = v;
}

void grx_mips_ret_d(GrxMipsCpu *c, double v) {
    memcpy(&c->r[2].d[0], &v, 8);
}

void grx_mips_ret_f(GrxMipsCpu *c, float v) {
    c->f[0].f = v;
}

/* ------------------------------------------------------------------ FPU */

static inline float fin(GrxMipsFpr x) {
    uint32_t e = x.u & 0x7F800000u;
    if (e == 0) {
        x.u &= 0x80000000u; /* denormals read as zero */
    } else if (e == 0x7F800000u) {
        x.u = (x.u & 0x80000000u) | 0x7F7FFFFFu; /* no infinities or NaNs: the biggest number */
    }
    return x.f;
}

static inline uint32_t fout(float r) {
    GrxMipsFpr x;
    uint32_t e;
    x.f = r;
    e = x.u & 0x7F800000u;
    if (e == 0x7F800000u) {
        x.u = (x.u & 0x80000000u) | 0x7F7FFFFFu; /* overflow clamps */
    } else if (e == 0) {
        x.u &= 0x80000000u; /* underflow flushes */
    }
    return x.u;
}

static inline uint32_t ee_fdiv(GrxMipsFpr a, GrxMipsFpr b) {
    float y = fin(b);
    if (y == 0.0f) {
        return ((a.u ^ b.u) & 0x80000000u) | 0x7F7FFFFFu;
    }
    return fout(fin(a) / y);
}

static inline int32_t cvt_w(float x) {
    if (x >= 2147483648.0f) {
        return 0x7FFFFFFF;
    }
    if (x <= -2147483648.0f) {
        return (int32_t)0x80000000u;
    }
    return (int32_t)x;
}

/* ------------------------------------------------------------------ VU0 macro mode */

static float vq; /* VU0 Q (the port's VU0 has no other user of it) */

static void vwrite(int d, int mask, const float *v) {
    int i;
    if (d == 0) {
        return;
    }
    for (i = 0; i < 4; i++) {
        if (mask & (1 << i)) {
            grx_vf[d].f[i] = v[i];
        }
    }
}

enum { V_ADD, V_SUB, V_MUL, V_MADD, V_MSUB, V_MAX, V_MIN };

/* d = op(s, t) per field; t broadcast from field bc, or the scalar *k */
static void varith(int op, int mask, int d, int s, int t, int bc, const float *k) {
    const float *a = grx_vf[s].f, *acc = grx_vf[GRX_ACC].f;
    float r[4];
    int i;
    for (i = 0; i < 4; i++) {
        float y = k ? *k : grx_vf[t].f[bc >= 0 ? bc : i];
        switch (op) {
        case V_ADD: r[i] = a[i] + y; break;
        case V_SUB: r[i] = a[i] - y; break;
        case V_MUL: r[i] = a[i] * y; break;
        case V_MADD: r[i] = acc[i] + a[i] * y; break;
        case V_MSUB: r[i] = acc[i] - a[i] * y; break;
        case V_MAX: r[i] = a[i] > y ? a[i] : y; break;
        default: r[i] = a[i] < y ? a[i] : y; break;
        }
    }
    vwrite(d, mask, r);
}

static int vu_mask(uint32_t w) {
    uint32_t dest = (w >> 21) & 15; /* x is bit 3 */
    return ((dest >> 3) & 1) | ((dest >> 1) & 2) | ((dest << 1) & 4) | ((dest << 3) & 8);
}

static void unknown(uint32_t w, uint32_t pc) __attribute__((noreturn));
static void unknown(uint32_t w, uint32_t pc) {
    char d[64];
    grx_fatal("mips: unknown instruction %08X (%s) at 0x%08X", w, grx_mips_disasm(w, pc, d, sizeof(d)), pc);
}

static void cop2(GrxMipsCpu *c, uint32_t w, uint32_t pc) {
    uint32_t rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31;
    if (rs < 0x10) {
        switch (rs) {
        case 0x01: /* qmfc2 */
            memcpy(&c->r[rt], &grx_vf[rd], 16);
            return;
        case 0x02: /* cfc2 */
            c->r[rt].sd[0] = (int32_t)grx_cfc2(rd);
            return;
        case 0x05: /* qmtc2 */
            if (rd) {
                memcpy(&grx_vf[rd], &c->r[rt], 16);
            }
            return;
        case 0x06: /* ctc2 */
            grx_ctc2(rd, c->r[rt].w[0]);
            return;
        }
        unknown(w, pc);
    }
    {
        int mask = vu_mask(w);
        int ft = (w >> 16) & 31, fs = (w >> 11) & 31, fd = (w >> 6) & 31;
        uint32_t fn = w & 63;
        float iv;
        memcpy(&iv, &grx_vi[21], 4); /* I */
        if (fn < 0x3C) {
            int bc = fn & 3;
            switch (fn) {
            case 0x00: case 0x01: case 0x02: case 0x03: varith(V_ADD, mask, fd, fs, ft, bc, 0); return;
            case 0x04: case 0x05: case 0x06: case 0x07: varith(V_SUB, mask, fd, fs, ft, bc, 0); return;
            case 0x08: case 0x09: case 0x0A: case 0x0B: varith(V_MADD, mask, fd, fs, ft, bc, 0); return;
            case 0x0C: case 0x0D: case 0x0E: case 0x0F: varith(V_MSUB, mask, fd, fs, ft, bc, 0); return;
            case 0x10: case 0x11: case 0x12: case 0x13: varith(V_MAX, mask, fd, fs, ft, bc, 0); return;
            case 0x14: case 0x15: case 0x16: case 0x17: varith(V_MIN, mask, fd, fs, ft, bc, 0); return;
            case 0x18: case 0x19: case 0x1A: case 0x1B: varith(V_MUL, mask, fd, fs, ft, bc, 0); return;
            case 0x1C: varith(V_MUL, mask, fd, fs, 0, -1, &vq); return;
            case 0x1D: varith(V_MAX, mask, fd, fs, 0, -1, &iv); return;
            case 0x1E: varith(V_MUL, mask, fd, fs, 0, -1, &iv); return;
            case 0x1F: varith(V_MIN, mask, fd, fs, 0, -1, &iv); return;
            case 0x20: varith(V_ADD, mask, fd, fs, 0, -1, &vq); return;
            case 0x21: varith(V_MADD, mask, fd, fs, 0, -1, &vq); return;
            case 0x22: varith(V_ADD, mask, fd, fs, 0, -1, &iv); return;
            case 0x23: varith(V_MADD, mask, fd, fs, 0, -1, &iv); return;
            case 0x24: varith(V_SUB, mask, fd, fs, 0, -1, &vq); return;
            case 0x25: varith(V_MSUB, mask, fd, fs, 0, -1, &vq); return;
            case 0x26: varith(V_SUB, mask, fd, fs, 0, -1, &iv); return;
            case 0x27: varith(V_MSUB, mask, fd, fs, 0, -1, &iv); return;
            case 0x28: varith(V_ADD, mask, fd, fs, ft, -1, 0); return;
            case 0x29: varith(V_MADD, mask, fd, fs, ft, -1, 0); return;
            case 0x2A: varith(V_MUL, mask, fd, fs, ft, -1, 0); return;
            case 0x2B: varith(V_MAX, mask, fd, fs, ft, -1, 0); return;
            case 0x2C: varith(V_SUB, mask, fd, fs, ft, -1, 0); return;
            case 0x2D: varith(V_MSUB, mask, fd, fs, ft, -1, 0); return;
            case 0x2E: grx_vu0_opmsub(fd, fs, ft); return;
            case 0x2F: varith(V_MIN, mask, fd, fs, ft, -1, 0); return;
            }
            unknown(w, pc);
        }
        switch (((w >> 6) & 31) << 2 | (fn & 3)) {
        case 0x00: case 0x01: case 0x02: case 0x03: varith(V_ADD, mask, GRX_ACC, fs, ft, fn & 3, 0); return;
        case 0x04: case 0x05: case 0x06: case 0x07: varith(V_SUB, mask, GRX_ACC, fs, ft, fn & 3, 0); return;
        case 0x08: case 0x09: case 0x0A: case 0x0B: varith(V_MADD, mask, GRX_ACC, fs, ft, fn & 3, 0); return;
        case 0x0C: case 0x0D: case 0x0E: case 0x0F: varith(V_MSUB, mask, GRX_ACC, fs, ft, fn & 3, 0); return;
        case 0x10: grx_vu0_itof(mask, ft, fs, 0); return;
        case 0x11: grx_vu0_itof(mask, ft, fs, 4); return;
        case 0x12: grx_vu0_itof(mask, ft, fs, 12); return;
        case 0x13: grx_vu0_itof(mask, ft, fs, 15); return;
        case 0x14: grx_vu0_ftoi(mask, ft, fs, 0); return;
        case 0x15: grx_vu0_ftoi(mask, ft, fs, 4); return;
        case 0x16: grx_vu0_ftoi(mask, ft, fs, 12); return;
        case 0x17: grx_vu0_ftoi(mask, ft, fs, 15); return;
        case 0x18: case 0x19: case 0x1A: case 0x1B: varith(V_MUL, mask, GRX_ACC, fs, ft, fn & 3, 0); return;
        case 0x1C: varith(V_MUL, mask, GRX_ACC, fs, 0, -1, &vq); return;
        case 0x1D: grx_vu0_abs(mask, ft, fs); return;
        case 0x1E: varith(V_MUL, mask, GRX_ACC, fs, 0, -1, &iv); return;
        case 0x20: varith(V_ADD, mask, GRX_ACC, fs, 0, -1, &vq); return;
        case 0x21: varith(V_MADD, mask, GRX_ACC, fs, 0, -1, &vq); return;
        case 0x22: varith(V_ADD, mask, GRX_ACC, fs, 0, -1, &iv); return;
        case 0x23: varith(V_MADD, mask, GRX_ACC, fs, 0, -1, &iv); return;
        case 0x24: varith(V_SUB, mask, GRX_ACC, fs, 0, -1, &vq); return;
        case 0x25: varith(V_MSUB, mask, GRX_ACC, fs, 0, -1, &vq); return;
        case 0x26: varith(V_SUB, mask, GRX_ACC, fs, 0, -1, &iv); return;
        case 0x27: varith(V_MSUB, mask, GRX_ACC, fs, 0, -1, &iv); return;
        case 0x28: varith(V_ADD, mask, GRX_ACC, fs, ft, -1, 0); return;
        case 0x29: varith(V_MADD, mask, GRX_ACC, fs, ft, -1, 0); return;
        case 0x2A: varith(V_MUL, mask, GRX_ACC, fs, ft, -1, 0); return;
        case 0x2C: varith(V_SUB, mask, GRX_ACC, fs, ft, -1, 0); return;
        case 0x2D: varith(V_MSUB, mask, GRX_ACC, fs, ft, -1, 0); return;
        case 0x2E: grx_vu0_opmula(fs, ft); return;
        case 0x2F: return; /* vnop */
        case 0x30: grx_vu0_move(mask, ft, fs); return;
        case 0x31: grx_vu0_mr32(mask, ft, fs); return;
        case 0x38: { /* vdiv Q, fs[fsf], ft[ftf] */
            float x = grx_vf[fs].f[(w >> 21) & 3], y = grx_vf[ft].f[(w >> 23) & 3];
            vq = y == 0.0f ? (x < 0.0f) != (y < 0.0f) ? -3.402823466e38f : 3.402823466e38f : x / y;
            return;
        }
        case 0x39: /* vsqrt Q, ft[ftf] */
            vq = sqrtf(fabsf(grx_vf[ft].f[(w >> 23) & 3]));
            return;
        case 0x3A: { /* vrsqrt Q, fs[fsf], ft[ftf] */
            float x = grx_vf[fs].f[(w >> 21) & 3], y = sqrtf(fabsf(grx_vf[ft].f[(w >> 23) & 3]));
            vq = y == 0.0f ? (x < 0.0f ? -3.402823466e38f : 3.402823466e38f) : x / y;
            return;
        }
        case 0x3B: return; /* vwaitq */
        }
        unknown(w, pc);
    }
}

/* ------------------------------------------------------------------ the interpreter */

#define RS(w) (((w) >> 21) & 31)
#define RT(w) (((w) >> 16) & 31)
#define RD(w) (((w) >> 11) & 31)
#define SA(w) (((w) >> 6) & 31)
#define SIMM(w) ((int32_t)(int16_t)(w))
#define UIMM(w) ((w) & 0xFFFFu)
#define SX32(x) ((uint64_t)(int64_t)(int32_t)(uint32_t)(x))

#define G(n) (c->r[n].d[0])
#define GS(n) (c->r[n].sd[0])
#define GW(n) (c->r[n].w[0])

static inline void branch(GrxMipsCpu *c, int taken, uint32_t pc, uint32_t w) {
    if (taken) {
        c->npc = pc + 4 + ((uint32_t)SIMM(w) << 2);
    }
}

/* Branch likely: the delay slot only runs when taken */
static inline void branch_l(GrxMipsCpu *c, int taken, uint32_t pc, uint32_t w) {
    if (taken) {
        c->npc = pc + 4 + ((uint32_t)SIMM(w) << 2);
    } else {
        c->pc = c->npc;
        c->npc += 4;
    }
}

static inline uint32_t ld32(uint32_t a) {
    uint32_t v;
    memcpy(&v, mem(a), 4);
    return v;
}

static inline void st32(uint32_t a, uint32_t v) {
    uint8_t *p = mem(a);
    v = store_form(v, (uint32_t)(uintptr_t)p);
    memcpy(p, &v, 4);
}

static inline uint64_t ld64(uint32_t a) {
    uint64_t v;
    memcpy(&v, mem(a), 8);
    return v;
}

/* lq/sq and lqc2/sqc2 ignore the low four address bits. The program's
   own memory and stack are 16-aligned as on the EE; a host object
   needn't be (port/include/grx_ee.h), so its address is taken as it is. */
static inline uint8_t *mem_q(uint32_t a) {
    uint8_t *p = mem(in_window(a) || in_stack(a) ? a & ~15u : a);
    return p;
}

static void st_words(uint8_t *p, const void *src, int n) {
    uint32_t w[4];
    int i;
    memcpy(w, src, (size_t)n * 4);
    for (i = 0; i < n; i++) {
        w[i] = store_form(w[i], (uint32_t)(uintptr_t)p);
    }
    memcpy(p, w, (size_t)n * 4);
}

static void ld_words(void *dst, const uint8_t *p, int n) {
    uint32_t w[4];
    int i;
    memcpy(w, p, (size_t)n * 4);
    for (i = 0; i < n; i++) {
        w[i] = load_form(w[i]);
    }
    memcpy(dst, w, (size_t)n * 4);
}

static const uint64_t LDL_MASK[8] = {0x00FFFFFFFFFFFFFFull, 0x0000FFFFFFFFFFFFull, 0x000000FFFFFFFFFFull,
                                     0x00000000FFFFFFFFull, 0x0000000000FFFFFFull, 0x000000000000FFFFull,
                                     0x00000000000000FFull, 0};
static const uint64_t LDR_MASK[8] = {0, 0xFF00000000000000ull, 0xFFFF000000000000ull, 0xFFFFFF0000000000ull,
                                     0xFFFFFFFF00000000ull, 0xFFFFFFFFFF000000ull, 0xFFFFFFFFFFFF0000ull,
                                     0xFFFFFFFFFFFFFF00ull};
static const uint64_t SDL_MASK[8] = {0xFFFFFFFFFFFFFF00ull, 0xFFFFFFFFFFFF0000ull, 0xFFFFFFFFFF000000ull,
                                     0xFFFFFFFF00000000ull, 0xFFFFFF0000000000ull, 0xFFFF000000000000ull,
                                     0xFF00000000000000ull, 0};
static const uint64_t SDR_MASK[8] = {0, 0x00000000000000FFull, 0x000000000000FFFFull, 0x0000000000FFFFFFull,
                                     0x00000000FFFFFFFFull, 0x000000FFFFFFFFFFull, 0x0000FFFFFFFFFFFFull,
                                     0x00FFFFFFFFFFFFFFull};
static const uint32_t LWL_MASK[4] = {0x00FFFFFFu, 0x0000FFFFu, 0x000000FFu, 0};
static const uint32_t LWR_MASK[4] = {0, 0xFF000000u, 0xFFFF0000u, 0xFFFFFF00u};
static const uint32_t SWL_MASK[4] = {0xFFFFFF00u, 0xFFFF0000u, 0xFF000000u, 0};
static const uint32_t SWR_MASK[4] = {0, 0x000000FFu, 0x0000FFFFu, 0x00FFFFFFu};

static void divide(GrxMipsCpu *c, uint32_t w, uint64_t *lo, uint64_t *hi, int is_unsigned) {
    if (is_unsigned) {
        uint32_t n = GW(RS(w)), d = GW(RT(w));
        if (d == 0) {
            *lo = SX32(0xFFFFFFFFu);
            *hi = SX32(n);
        } else {
            *lo = SX32(n / d);
            *hi = SX32(n % d);
        }
    } else {
        int32_t n = (int32_t)GW(RS(w)), d = (int32_t)GW(RT(w));
        if (d == 0) {
            *lo = SX32(n < 0 ? 1 : -1);
            *hi = SX32(n);
        } else if (n == (int32_t)0x80000000u && d == -1) {
            *lo = SX32(0x80000000u);
            *hi = 0;
        } else {
            *lo = SX32(n / d);
            *hi = SX32(n % d);
        }
    }
}

static void exec(GrxMipsCpu *c, uint32_t w, uint32_t pc) {
    uint32_t op = w >> 26;
    uint32_t rs = RS(w), rt = RT(w), rd = RD(w);
    switch (op) {
    case 0x00: /* SPECIAL */
        switch (w & 63) {
        case 0x00: G(rd) = SX32(GW(rt) << SA(w)); return;
        case 0x02: G(rd) = SX32(GW(rt) >> SA(w)); return;
        case 0x03: G(rd) = SX32((int32_t)GW(rt) >> SA(w)); return;
        case 0x04: G(rd) = SX32(GW(rt) << (GW(rs) & 31)); return;
        case 0x06: G(rd) = SX32(GW(rt) >> (GW(rs) & 31)); return;
        case 0x07: G(rd) = SX32((int32_t)GW(rt) >> (GW(rs) & 31)); return;
        case 0x08: c->npc = GW(rs); return;
        case 0x09: {
            uint32_t t = GW(rs);
            G(rd) = SX32(pc + 8);
            c->npc = t;
            return;
        }
        case 0x0A: if (G(rt) == 0) { c->r[rd].d[0] = G(rs); } return;
        case 0x0B: if (G(rt) != 0) { c->r[rd].d[0] = G(rs); } return;
        case 0x0D: grx_fatal("mips: break %u at 0x%08X (a division by zero?)", (w >> 6) & 0xFFFFF, pc);
        case 0x0F: return; /* sync */
        case 0x10: G(rd) = c->hi; return;
        case 0x11: c->hi = G(rs); return;
        case 0x12: G(rd) = c->lo; return;
        case 0x13: c->lo = G(rs); return;
        case 0x14: G(rd) = G(rt) << (GW(rs) & 63); return;
        case 0x16: G(rd) = G(rt) >> (GW(rs) & 63); return;
        case 0x17: GS(rd) = GS(rt) >> (GW(rs) & 63); return;
        case 0x18: { /* mult rd, rs, rt */
            int64_t p = (int64_t)(int32_t)GW(rs) * (int32_t)GW(rt);
            c->lo = SX32((uint32_t)p);
            c->hi = SX32((uint32_t)((uint64_t)p >> 32));
            G(rd) = c->lo;
            return;
        }
        case 0x19: {
            uint64_t p = (uint64_t)GW(rs) * GW(rt);
            c->lo = SX32((uint32_t)p);
            c->hi = SX32((uint32_t)(p >> 32));
            G(rd) = c->lo;
            return;
        }
        case 0x1A: divide(c, w, &c->lo, &c->hi, 0); return;
        case 0x1B: divide(c, w, &c->lo, &c->hi, 1); return;
        case 0x20: case 0x21: G(rd) = SX32(GW(rs) + GW(rt)); return;
        case 0x22: case 0x23: G(rd) = SX32(GW(rs) - GW(rt)); return;
        case 0x24: G(rd) = G(rs) & G(rt); return;
        case 0x25: G(rd) = G(rs) | G(rt); return;
        case 0x26: G(rd) = G(rs) ^ G(rt); return;
        case 0x27: G(rd) = ~(G(rs) | G(rt)); return;
        case 0x2A: G(rd) = GS(rs) < GS(rt); return;
        case 0x2B: G(rd) = G(rs) < G(rt); return;
        case 0x2C: case 0x2D: G(rd) = G(rs) + G(rt); return;
        case 0x2E: case 0x2F: G(rd) = G(rs) - G(rt); return;
        case 0x38: G(rd) = G(rt) << SA(w); return;
        case 0x3A: G(rd) = G(rt) >> SA(w); return;
        case 0x3B: GS(rd) = GS(rt) >> SA(w); return;
        case 0x3C: G(rd) = G(rt) << (SA(w) + 32); return;
        case 0x3E: G(rd) = G(rt) >> (SA(w) + 32); return;
        case 0x3F: GS(rd) = GS(rt) >> (SA(w) + 32); return;
        }
        break;
    case 0x01: /* REGIMM */
        switch (rt) {
        case 0x00: branch(c, GS(rs) < 0, pc, w); return;
        case 0x01: branch(c, GS(rs) >= 0, pc, w); return;
        case 0x02: branch_l(c, GS(rs) < 0, pc, w); return;
        case 0x03: branch_l(c, GS(rs) >= 0, pc, w); return;
        case 0x10: case 0x11: {
            int taken = rt == 0x10 ? GS(rs) < 0 : GS(rs) >= 0;
            G(31) = SX32(pc + 8);
            branch(c, taken, pc, w);
            return;
        }
        }
        break;
    case 0x02: c->npc = ((pc + 4) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2); return;
    case 0x03:
        G(31) = SX32(pc + 8);
        c->npc = ((pc + 4) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
        return;
    case 0x04: branch(c, G(rs) == G(rt), pc, w); return;
    case 0x05: branch(c, G(rs) != G(rt), pc, w); return;
    case 0x06: branch(c, GS(rs) <= 0, pc, w); return;
    case 0x07: branch(c, GS(rs) > 0, pc, w); return;
    case 0x08: case 0x09: G(rt) = SX32(GW(rs) + (uint32_t)SIMM(w)); return;
    case 0x0A: G(rt) = GS(rs) < (int64_t)SIMM(w); return;
    case 0x0B: G(rt) = G(rs) < (uint64_t)(int64_t)SIMM(w); return;
    case 0x0C: G(rt) = G(rs) & UIMM(w); return;
    case 0x0D: G(rt) = G(rs) | UIMM(w); return;
    case 0x0E: G(rt) = G(rs) ^ UIMM(w); return;
    case 0x0F: G(rt) = SX32(UIMM(w) << 16); return;
    case 0x11: { /* COP1 */
        uint32_t fs = rd, fd = SA(w), ft = rt;
        switch (rs) {
        case 0x00: G(rt) = SX32(c->f[fs].u); return;
        case 0x02: G(rt) = SX32(fs == 31 ? (uint32_t)c->fcc << 23 | 0x01000001u : fs == 0 ? 0x2E30u : 0); return;
        case 0x04: c->f[fs].u = GW(rt); return;
        case 0x06: if (fs == 31) { c->fcc = (GW(rt) >> 23) & 1; } return;
        case 0x08:
            switch (rt) {
            case 0: branch(c, !c->fcc, pc, w); return;
            case 1: branch(c, c->fcc, pc, w); return;
            case 2: branch_l(c, !c->fcc, pc, w); return;
            case 3: branch_l(c, c->fcc, pc, w); return;
            }
            break;
        case 0x10: {
            float a = fin(c->f[fs]), b = fin(c->f[ft]);
            switch (w & 63) {
            case 0x00: c->f[fd].u = fout(a + b); return;
            case 0x01: c->f[fd].u = fout(a - b); return;
            case 0x02: c->f[fd].u = fout(a * b); return;
            case 0x03: c->f[fd].u = ee_fdiv(c->f[fs], c->f[ft]); return;
            case 0x04: c->f[fd].u = fout(sqrtf(fabsf(b))); return;
            case 0x05: c->f[fd].u = c->f[fs].u & 0x7FFFFFFFu; return;
            case 0x06: c->f[fd].u = c->f[fs].u; return;
            case 0x07: c->f[fd].u = c->f[fs].u ^ 0x80000000u; return;
            case 0x16: { /* rsqrt.s fd, fs, ft */
                GrxMipsFpr r;
                r.f = sqrtf(fabsf(b));
                c->f[fd].u = ee_fdiv(c->f[fs], r);
                return;
            }
            case 0x18: c->facc.u = fout(a + b); return;
            case 0x19: c->facc.u = fout(a - b); return;
            case 0x1A: c->facc.u = fout(a * b); return;
            case 0x1C: c->f[fd].u = fout(fin(c->facc) + a * b); return;
            case 0x1D: c->f[fd].u = fout(fin(c->facc) - a * b); return;
            case 0x1E: c->facc.u = fout(fin(c->facc) + a * b); return;
            case 0x1F: c->facc.u = fout(fin(c->facc) - a * b); return;
            case 0x24: c->f[fd].i = cvt_w(fin(c->f[fs])); return;
            case 0x28: c->f[fd].u = a > b ? c->f[fs].u : c->f[ft].u; return;
            case 0x29: c->f[fd].u = a < b ? c->f[fs].u : c->f[ft].u; return;
            case 0x30: c->fcc = 0; return;
            case 0x32: c->fcc = a == b; return;
            case 0x34: c->fcc = a < b; return;
            case 0x36: c->fcc = a <= b; return;
            }
            break;
        }
        case 0x14:
            if ((w & 63) == 0x20) {
                c->f[fd].u = fout((float)c->f[fs].i);
                return;
            }
            break;
        }
        break;
    }
    case 0x12: cop2(c, w, pc); return;
    case 0x14: branch_l(c, G(rs) == G(rt), pc, w); return;
    case 0x15: branch_l(c, G(rs) != G(rt), pc, w); return;
    case 0x16: branch_l(c, GS(rs) <= 0, pc, w); return;
    case 0x17: branch_l(c, GS(rs) > 0, pc, w); return;
    case 0x18: case 0x19: G(rt) = G(rs) + (uint64_t)(int64_t)SIMM(w); return;
    case 0x1A: { /* ldl */
        uint32_t a = GW(rs) + SIMM(w), s = a & 7;
        G(rt) = (G(rt) & LDL_MASK[s]) | (ld64(a & ~7u) << (56 - s * 8));
        return;
    }
    case 0x1B: { /* ldr */
        uint32_t a = GW(rs) + SIMM(w), s = a & 7;
        G(rt) = (G(rt) & LDR_MASK[s]) | (ld64(a & ~7u) >> (s * 8));
        return;
    }
    case 0x1C: /* MMI */
        switch (w & 63) {
        case 0x00: case 0x01: case 0x20: case 0x21: { /* madd, maddu, madd1, maddu1 */
            int pipe1 = (w & 63) >= 0x20;
            uint64_t *lo = pipe1 ? &c->lo1 : &c->lo, *hi = pipe1 ? &c->hi1 : &c->hi;
            uint64_t acc = (uint64_t)(uint32_t)*hi << 32 | (uint32_t)*lo;
            if (w & 1) {
                acc += (uint64_t)GW(rs) * GW(rt);
            } else {
                acc += (uint64_t)((int64_t)(int32_t)GW(rs) * (int32_t)GW(rt));
            }
            *lo = SX32((uint32_t)acc);
            *hi = SX32((uint32_t)(acc >> 32));
            G(rd) = *lo;
            return;
        }
        case 0x10: G(rd) = c->hi1; return;
        case 0x11: c->hi1 = G(rs); return;
        case 0x12: G(rd) = c->lo1; return;
        case 0x13: c->lo1 = G(rs); return;
        case 0x18: {
            int64_t p = (int64_t)(int32_t)GW(rs) * (int32_t)GW(rt);
            c->lo1 = SX32((uint32_t)p);
            c->hi1 = SX32((uint32_t)((uint64_t)p >> 32));
            G(rd) = c->lo1;
            return;
        }
        case 0x19: {
            uint64_t p = (uint64_t)GW(rs) * GW(rt);
            c->lo1 = SX32((uint32_t)p);
            c->hi1 = SX32((uint32_t)(p >> 32));
            G(rd) = c->lo1;
            return;
        }
        case 0x1A: divide(c, w, &c->lo1, &c->hi1, 0); return;
        case 0x1B: divide(c, w, &c->lo1, &c->hi1, 1); return;
        }
        break;
    case 0x1E: { /* lq */
        uint8_t *p = mem_q(GW(rs) + SIMM(w));
        ld_words(&c->r[rt], p, 4);
        return;
    }
    case 0x1F: {
        uint8_t *p = mem_q(GW(rs) + SIMM(w));
        st_words(p, &c->r[rt], 4);
        return;
    }
    case 0x20: G(rt) = (uint64_t)(int64_t)*(int8_t *)mem(GW(rs) + SIMM(w)); return;
    case 0x21: {
        int16_t v;
        memcpy(&v, mem(GW(rs) + SIMM(w)), 2);
        G(rt) = (uint64_t)(int64_t)v;
        return;
    }
    case 0x22: { /* lwl */
        uint32_t a = GW(rs) + SIMM(w), s = a & 3;
        G(rt) = SX32((GW(rt) & LWL_MASK[s]) | (ld32(a & ~3u) << (24 - s * 8)));
        return;
    }
    case 0x23: G(rt) = SX32(load_form(ld32(GW(rs) + SIMM(w)))); return;
    case 0x24: G(rt) = *(uint8_t *)mem(GW(rs) + SIMM(w)); return;
    case 0x25: {
        uint16_t v;
        memcpy(&v, mem(GW(rs) + SIMM(w)), 2);
        G(rt) = v;
        return;
    }
    case 0x26: { /* lwr */
        uint32_t a = GW(rs) + SIMM(w), s = a & 3, m = ld32(a & ~3u);
        if (s == 0) {
            G(rt) = SX32(m);
        } else {
            c->r[rt].w[0] = (GW(rt) & LWR_MASK[s]) | (m >> (s * 8));
        }
        return;
    }
    case 0x27: G(rt) = load_form(ld32(GW(rs) + SIMM(w))); return;
    case 0x28: *(uint8_t *)mem(GW(rs) + SIMM(w)) = (uint8_t)GW(rt); return;
    case 0x29: {
        uint16_t v = (uint16_t)GW(rt);
        memcpy(mem(GW(rs) + SIMM(w)), &v, 2);
        return;
    }
    case 0x2A: { /* swl */
        uint32_t a = GW(rs) + SIMM(w), s = a & 3;
        uint8_t *p = mem(a & ~3u);
        uint32_t m;
        memcpy(&m, p, 4);
        m = (GW(rt) >> (24 - s * 8)) | (m & SWL_MASK[s]);
        memcpy(p, &m, 4);
        return;
    }
    case 0x2B: st32(GW(rs) + SIMM(w), GW(rt)); return;
    case 0x2C: { /* sdl */
        uint32_t a = GW(rs) + SIMM(w), s = a & 7;
        uint8_t *p = mem(a & ~7u);
        uint64_t m;
        memcpy(&m, p, 8);
        m = (G(rt) >> (56 - s * 8)) | (m & SDL_MASK[s]);
        memcpy(p, &m, 8);
        return;
    }
    case 0x2D: { /* sdr */
        uint32_t a = GW(rs) + SIMM(w), s = a & 7;
        uint8_t *p = mem(a & ~7u);
        uint64_t m;
        memcpy(&m, p, 8);
        m = (G(rt) << (s * 8)) | (m & SDR_MASK[s]);
        memcpy(p, &m, 8);
        return;
    }
    case 0x2E: { /* swr */
        uint32_t a = GW(rs) + SIMM(w), s = a & 3;
        uint8_t *p = mem(a & ~3u);
        uint32_t m;
        memcpy(&m, p, 4);
        m = (GW(rt) << (s * 8)) | (m & SWR_MASK[s]);
        memcpy(p, &m, 4);
        return;
    }
    case 0x2F: return; /* cache */
    case 0x31: memcpy(&c->f[rt], mem(GW(rs) + SIMM(w)), 4); return;
    case 0x33: return; /* pref */
    case 0x36: /* lqc2 */
        if (rt) {
            memcpy(&grx_vf[rt], mem_q(GW(rs) + SIMM(w)), 16);
        }
        return;
    case 0x37: ld_words(&c->r[rt].d[0], mem(GW(rs) + SIMM(w)), 2); return;
    case 0x39: memcpy(mem(GW(rs) + SIMM(w)), &c->f[rt], 4); return;
    case 0x3E: memcpy(mem_q(GW(rs) + SIMM(w)), &grx_vf[rt], 16); return;
    case 0x3F: st_words(mem(GW(rs) + SIMM(w)), &c->r[rt].d[0], 2); return;
    }
    unknown(w, pc);
}

/* ------------------------------------------------------------------ native calls */

static void call_handler(GrxMipsCpu *c, int k) {
    const GrxMipsAbiFunc *f = &grx_mips_abi_funcs[k];
    GrxMipsHandler h = handlers ? handlers[k] : 0;
    const GrxMipsAbiFunc *outer = c->cur;
    if (!h) {
        h = grx_mips_builtin(f->name);
    }
    if (!h) {
        grx_fatal("mips: the program calls %s (0x%08X), which the bridge doesn't have", f->name, f->addr);
    }
    if (grx_mips_log_calls) {
        grx_log("mips: %08X -> %s(%08X, %08X, %08X, %08X)", GW(31) - 8, f->name, GW(4), GW(5), GW(6), GW(7));
    }
    c->cur = f;
    h(c, f);
    c->cur = outer;
}

/* A host function the bridge doesn't know: all arguments as ints */
static void call_native(GrxMipsCpu *c, uint32_t fn) {
    typedef int (*Fn8)(int, int, int, int, int, int, int, int);
    static uint32_t said[16];
    static int nsaid;
    uint32_t a[8];
    int i, k;
    for (k = 0; k < nsaid && said[k] != fn; k++) {
    }
    if (k == nsaid && nsaid < 16) {
        said[nsaid++] = fn;
        grx_log("mips: call to the native function at 0x%08X, which the bridge doesn't know (from 0x%08X); "
                "passing $a0-$t3 as ints", fn, GW(31) - 8);
    }
    for (i = 0; i < 8; i++) {
        a[i] = grx_mips_to_host(GW(4 + i));
    }
    i = ((Fn8)(uintptr_t)fn)((int)a[0], (int)a[1], (int)a[2], (int)a[3], (int)a[4], (int)a[5], (int)a[6], (int)a[7]);
    c->r[2].sd[0] = (int32_t)grx_mips_to_ee((uint32_t)i);
}

/* pc left the window: a call of native code, or the end of the outermost
   call. Returns 0 to stop. */
static int leave(GrxMipsCpu *c) {
    uint32_t pc = c->pc;
    int k;
    if (pc == GRX_MIPS_RETURN) {
        return 0;
    }
    if (in_host_window(pc)) {
        c->pc = pc - win_host + GRX_MIPS_WIN_EE;
        c->npc = c->pc + 4;
        return 1;
    }
    if (pc - grx_mips_thunks_lo() < grx_mips_thunks_hi() - grx_mips_thunks_lo()) {
        uint32_t t = grx_mips_thunk_target((const void *)(uintptr_t)pc);
        if (t) {
            c->pc = t;
            c->npc = t + 4;
            return 1;
        }
    }
    k = func_at_ee(pc);
    if (k < 0 && !(pc - GRX_MIPS_EE_LOW < GRX_MIPS_EE_HIGH - GRX_MIPS_EE_LOW)) {
        k = func_at_host(pc);
        if (k < 0) {
            if (pc < 0x10000) {
                grx_fatal("mips: jump to 0x%08X (from about 0x%08X, $ra 0x%08X)", pc, c->npc - 8, GW(31));
            }
            call_native(c, pc);
            c->pc = GW(31);
            c->npc = c->pc + 4;
            return 1;
        }
    }
    if (k < 0) {
        char d[128];
        grx_fatal("mips: jump to the EE address 0x%08X (%s), which isn't a function of the ABI ($ra 0x%08X)", pc,
                  grx_mips_describe(pc, d, sizeof(d)), GW(31));
    }
    call_handler(c, k);
    c->pc = GW(31);
    c->npc = c->pc + 4;
    return 1;
}

static void run(GrxMipsCpu *c) {
    for (;;) {
        uint32_t pc = c->pc, off = pc - GRX_MIPS_WIN_EE, w;
        if (off >= GRX_MIPS_WIN_SIZE) {
            if (!leave(c)) {
                return;
            }
            continue;
        }
        if (grx_mips_check_code && (off & 0x7FFF) >= areas[off >> 15].code_end) {
            grx_fatal("mips: execution at 0x%08X, past the code of its program (which ends at +0x%X)", pc,
                      areas[off >> 15].code_end);
        }
        memcpy(&w, win + off, 4);
        c->pc = c->npc;
        c->npc += 4;
        exec(c, w, pc);
        c->r[0].d[0] = 0;
        c->r[0].d[1] = 0;
        if (++c->steps == grx_mips_step_limit) {
            grx_fatal("mips: %llu instructions without returning (at 0x%08X)", (unsigned long long)c->steps, pc);
        }
    }
}

uint64_t grx_mips_callv_raw(uint32_t addr, const uint32_t *args, int nargs, float *f0) {
    GrxMipsCpu saved;
    uint32_t sp;
    uint64_t v0;
    int i;
    if (!ready) {
        grx_fatal("mips: called before grx_mips_setup");
    }
    if (nargs > 8) {
        grx_fatal("mips: %d arguments", nargs);
    }
    addr = grx_mips_to_ee_any(addr);
    if (in_window(addr) && (addr & 0x7FFF) == 0) {
        prepare((addr - GRX_MIPS_WIN_EE) >> 15); /* a program's entry */
    }
    if (depth > 0) {
        saved = cpu;
        sp = (cpu.r[29].w[0] - 0x40) & ~15u;
        if (!in_stack(sp) || sp - stack_lo < 0x1000) {
            grx_fatal("mips: out of stack (nested calls at depth %d)", depth);
        }
    } else {
        sp = stack_hi - 0x40;
        cpu.steps = 0;
    }
    for (i = 0; i < 8; i++) {
        cpu.r[4 + i].sd[0] = i < nargs ? (int32_t)grx_mips_to_ee(args[i]) : 0;
    }
    cpu.r[29].sd[0] = (int32_t)sp;
    cpu.r[31].sd[0] = (int32_t)GRX_MIPS_RETURN;
    cpu.pc = addr;
    cpu.npc = addr + 4;
    depth++;
    run(&cpu);
    depth--;
    v0 = cpu.r[2].d[0];
    if (f0) {
        *f0 = cpu.f[0].f;
    }
    if (depth > 0) {
        uint64_t steps = cpu.steps;
        cpu = saved;
        cpu.steps = steps;
    }
    return v0;
}

void grx_mips_abandon(void) {
    depth = 0;
    cpu.cur = 0;
}

uint32_t grx_mips_callv(uint32_t addr, const uint32_t *args, int nargs) {
    return grx_mips_to_host((uint32_t)grx_mips_callv_raw(addr, args, nargs, 0));
}

/* ------------------------------------------------------------------ save states */
/* (grx_state_parts.h) Between fields no program is running; the load
   areas' sizes and generations, VU0's Q and the thunks are kept. The
   window's contents are memory (state.c). */
void grx_mips_thunks_state_io(GrxState *s);

const char *grx_mips_state_blocker(void) {
    return depth > 0 ? "a disc program is running" : NULL;
}

void grx_mips_state_io(GrxState *s) {
    GRX_STATE_VAR(s, areas);
    GRX_STATE_VAR(s, vq);
    grx_mips_thunks_state_io(s);
}

void grx_mips_state_loaded(void) {
}
