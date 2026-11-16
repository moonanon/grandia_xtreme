/* Memory at the PS2 addresses the game's C uses literally: see
   port/include/grx_hwmem.h and port/docs/hardware.md.

   In a 32-bit Windows process the hardware windows (0x10000000,
   0x11000000, 0x12000000, 0x70000000) are normally free: the exe loads low
   (0x00400000, or wherever ASLR puts it within the first few tens of MB),
   so do the loader's heaps, stacks and mapped NLS files, and system DLLs
   go near the top of the 2 GB. Nothing guarantees it, so every failure
   is reported with what is in the way. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include "posix_vmem.h"
#endif
#include "grx_hwmem.h"

static GrxHwRange ranges[GRX_HW_NRANGES] = {
    {"EE registers", GRX_HW_EE_REGS_BASE, GRX_HW_EE_REGS_SIZE, 0, 0, 0, 0},
    {"VU memory", GRX_HW_VU_MEM_BASE, GRX_HW_VU_MEM_SIZE, 0, 0, 0, 0},
    {"GS privileged registers", GRX_HW_GS_PRIV_BASE, GRX_HW_GS_PRIV_SIZE, 0, 0, 0, 0},
    {"scratchpad", GRX_HW_SPR_BASE, GRX_HW_SPR_SIZE, 0, 0, 0, 0},
};

static HANDLE sections[GRX_HW_NRANGES];

/* The addresses of an aliased range's views: cached, uncached, accelerated */
static const uint32_t alias_bits[3] = {0, GRX_PS2_UNCACHED, GRX_PS2_UNCACHED_ACC};

static void hw_log(const char *fmt, ...) {
    va_list ap;
    fputs("hwmem: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

const char *grx_hwmem_probe(uint32_t base, uint32_t size, char *buf, size_t n) {
    uintptr_t p = base;
    uintptr_t end = (uintptr_t)base + size;

    while (p < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (VirtualQuery((void *)p, &mi, sizeof(mi)) == 0) {
            snprintf(buf, n, "unqueryable (error %lu)", GetLastError());
            return buf;
        }
        if (mi.State != MEM_FREE) {
            const char *type = mi.Type == MEM_IMAGE ? "image" : mi.Type == MEM_MAPPED ? "mapped view" : "private";
            char file[MAX_PATH] = "";
            if (mi.Type == MEM_IMAGE) {
                GetModuleFileNameA((HMODULE)mi.AllocationBase, file, sizeof(file));
            }
            snprintf(buf, n, "%s%s%s allocated at 0x%08lX (%s, region 0x%08lX+0x%lX)", type, file[0] ? " " : "",
                     file, (unsigned long)(uintptr_t)mi.AllocationBase,
                     mi.State == MEM_COMMIT ? "committed" : "reserved", (unsigned long)(uintptr_t)mi.BaseAddress,
                     (unsigned long)mi.RegionSize);
            return buf;
        }
        p = (uintptr_t)mi.BaseAddress + mi.RegionSize;
    }
    snprintf(buf, n, "free");
    return buf;
}

/* Maps one range; 0 or the address of the view that failed */
static uint32_t map_range(GrxHwRange *r, int i) {
    void *p;
    int v;

    if (!r->aliased) {
        p = VirtualAlloc((void *)(uintptr_t)r->base, r->size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p == (void *)(uintptr_t)r->base) {
            return 0;
        }
        r->error = GetLastError();
        if (p != NULL) {
            /* Can't happen with an aligned base, but never keep the wrong place */
            VirtualFree(p, 0, MEM_RELEASE);
        }
        return r->base;
    }
    /* One pagefile-backed section seen at base, base | 0x20000000 and
       base | 0x30000000, as the PS2's main RAM is */
    sections[i] = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, r->size, NULL);
    if (sections[i] == NULL) {
        r->error = GetLastError();
        return r->base;
    }
    for (v = 0; v < 3; v++) {
        void *want = (void *)(uintptr_t)(r->base | alias_bits[v]);
        if (MapViewOfFileEx(sections[i], FILE_MAP_ALL_ACCESS, 0, 0, r->size, want) != want) {
            r->error = GetLastError();
            while (v-- > 0) {
                UnmapViewOfFile((void *)(uintptr_t)(r->base | alias_bits[v]));
            }
            CloseHandle(sections[i]);
            sections[i] = NULL;
            return (uint32_t)(uintptr_t)want;
        }
    }
    return 0;
}

/* The EE addresses of the main executable, its overlays and the disc
   programs' load areas (0x00400000-0x00760000). The MIPS interpreter
   (port/src/mips/) tells EE addresses from host ones by range, so nothing
   of the host may land there. Reserved, never committed. */
#define EE_HOLE_BASE 0x00400000u
#define EE_HOLE_END  0x00760000u
#define GRANULE      0x10000u /* VirtualAlloc reserves in 64 KB steps */

/* Windows maps a few views low in the address space before main (one at
   0x00400000 always in a 32-bit process, at 0x00420000 in a 64-bit one,
   sometimes more), so the free parts are reserved one by one. The views
   already there are noted: nothing the game is handed a pointer to. */
static struct { uint32_t base, end; } ee_res[32];
static int ee_nres;
static uint32_t ee_views[16];
static int ee_nviews;

static void reserve_ee_hole(void) {
    uint32_t a = EE_HOLE_BASE;
    while (a < EE_HOLE_END && ee_nres < (int)(sizeof(ee_res) / sizeof(ee_res[0]))) {
        MEMORY_BASIC_INFORMATION mi;
        uint32_t end;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof(mi))) {
            break;
        }
        end = (uint32_t)(uintptr_t)mi.BaseAddress + (uint32_t)mi.RegionSize;
        if (end > EE_HOLE_END) {
            end = EE_HOLE_END;
        }
        if (mi.State != MEM_FREE && mi.Type == MEM_MAPPED &&
            ee_nviews < (int)(sizeof(ee_views) / sizeof(ee_views[0]))) {
            ee_views[ee_nviews++] = (uint32_t)(uintptr_t)mi.AllocationBase;
        }
        if (mi.State == MEM_FREE) {
            uint32_t lo = (a + GRANULE - 1) & ~(GRANULE - 1), hi = end & ~(GRANULE - 1);
            if (hi > lo && VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_RESERVE, PAGE_NOACCESS)) {
                ee_res[ee_nres].base = lo;
                ee_res[ee_nres].end = hi;
                ee_nres++;
            }
        }
        a = end;
    }
}

int grx_hwmem_ee_hole_reserved(void) {
    return ee_nres > 0;
}

/* The PS2's IOP block (0x01FF0000, grx_sif.h) and its uncached aliases:
   an EE address there that still reaches cdvd.c or sif.c is taken for
   the block (grx_iop_addr), so no host memory may be there, or a real
   buffer that lands on it is read into the port's 64 KB block instead
   (it happened on x86-64: the heap at 0x01FF1DC0, a 128 KB stream read).
   Reserved before the pool, never committed. */
static void reserve_iop_aliases(void) {
    static const uint32_t at[3] = {0x01FF0000u, 0x21FF0000u, 0x31FF0000u};
    int i;
    for (i = 0; i < 3; i++) {
        if (!VirtualAlloc((void *)(uintptr_t)at[i], GRANULE, MEM_RESERVE, PAGE_NOACCESS)) {
            char what[160];
            hw_log("the IOP block's address 0x%08X is taken (%s): a buffer there would be mistaken for it", at[i],
                   grx_hwmem_probe(at[i], GRANULE, what, sizeof(what)));
        }
    }
}

/* The game's memory below GRX_HW_LOW_LIMIT (heaps, the MIPS programs'
   area, ...) comes from a pool: at init every free 64 KB block there is
   reserved, and grx_hwmem_alloc_low commits runs of it. Otherwise
   whatever allocates first (DLLs, the GL driver, thread stacks: Windows
   hands out the lowest free addresses) takes the space the game needs. */
#define POOL_GRANULES (GRX_HW_LOW_LIMIT / GRANULE)
static uint8_t pool_state[POOL_GRANULES]; /* 0 not ours, 1 reserved, 2 committed */
static uint16_t pool_res[POOL_GRANULES];  /* which reservation: a run can't span two */
static uint16_t pool_len[POOL_GRANULES];  /* at a committed run's first granule: its length */
static int pool_up;

static void reserve_low_pool(void) {
    uint32_t a = GRANULE;
    uint16_t res = 0;
    pool_up = 1;
    while (a < GRX_HW_LOW_LIMIT) {
        MEMORY_BASIC_INFORMATION mi;
        uint32_t end, lo, hi, g;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof(mi))) {
            break;
        }
        end = (uint32_t)(uintptr_t)mi.BaseAddress + (uint32_t)mi.RegionSize;
        if (end > GRX_HW_LOW_LIMIT || end <= a) {
            end = GRX_HW_LOW_LIMIT;
        }
        lo = (a + GRANULE - 1) & ~(GRANULE - 1);
        hi = end & ~(GRANULE - 1);
        if (mi.State == MEM_FREE && hi > lo &&
            VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_RESERVE, PAGE_NOACCESS)) {
            res++;
            for (g = lo / GRANULE; g < hi / GRANULE; g++) {
                pool_state[g] = 1;
                pool_res[g] = res;
            }
        }
        a = end;
    }
}

/* First fit: a run of n reserved granules in one reservation, committed */
static void *pool_alloc(uint32_t n) {
    uint32_t g = 1, k;
    while (g + n <= POOL_GRANULES) {
        for (k = 0; k < n; k++) {
            if (pool_state[g + k] != 1 || pool_res[g + k] != pool_res[g]) {
                break;
            }
        }
        if (k == n) {
            void *p = VirtualAlloc((void *)(uintptr_t)(g * GRANULE), n * GRANULE, MEM_COMMIT, PAGE_READWRITE);
            if (p == NULL) {
                return NULL;
            }
            for (k = 0; k < n; k++) {
                pool_state[g + k] = 2;
            }
            pool_len[g] = (uint16_t)n;
            return p;
        }
        g += k + 1;
    }
    return NULL;
}

/* The run of n granules at granule g, if the pool reserved all of it (in
   one reservation) and none is in use: committed */
static void *pool_alloc_at(uint32_t g, uint32_t n) {
    uint32_t k;
    void *p;
    if (g == 0 || g + n > POOL_GRANULES) {
        return NULL;
    }
    for (k = 0; k < n; k++) {
        if (pool_state[g + k] != 1 || pool_res[g + k] != pool_res[g]) {
            return NULL;
        }
    }
    p = VirtualAlloc((void *)(uintptr_t)(g * GRANULE), n * GRANULE, MEM_COMMIT, PAGE_READWRITE);
    if (p == NULL) {
        return NULL;
    }
    for (k = 0; k < n; k++) {
        pool_state[g + k] = 2;
    }
    pool_len[g] = (uint16_t)n;
    return p;
}

static int pool_free(void *p) {
    uint32_t g = (uint32_t)(uintptr_t)p / GRANULE, k;
    if ((uintptr_t)p % GRANULE != 0 || g >= POOL_GRANULES || pool_state[g] != 2 || pool_len[g] == 0) {
        return 0;
    }
    VirtualFree(p, pool_len[g] * GRANULE, MEM_DECOMMIT);
    for (k = 0; k < pool_len[g]; k++) {
        pool_state[g + k] = 1;
    }
    pool_len[g] = 0;
    return 1;
}

/* 1 if [lo, hi) holds nothing but free space and the reservations above:
   the MIPS interpreter can then tell EE addresses from host ones there */
int grx_hwmem_ee_hole_clean(uint32_t lo, uint32_t hi) {
    uint32_t a = lo;
    while (a < hi) {
        MEMORY_BASIC_INFORMATION mi;
        int ours = 0, k;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof(mi))) {
            return 0;
        }
        for (k = 0; k < ee_nres; k++) {
            if ((uint32_t)(uintptr_t)mi.AllocationBase == ee_res[k].base) {
                ours = 1;
            }
        }
        for (k = 0; k < ee_nviews; k++) {
            if ((uint32_t)(uintptr_t)mi.AllocationBase == ee_views[k]) {
                ours = 1;   /* the system's, from before main */
            }
        }
        if (mi.State != MEM_FREE && !ours) {
            return 0;
        }
        a = (uint32_t)(uintptr_t)mi.BaseAddress + (uint32_t)mi.RegionSize;
    }
    return 1;
}

int grx_hwmem_init(void) {
    int failed = 0;
    int i;

    if (ee_nres == 0) {
        reserve_ee_hole();
    }
    if (!pool_up) {
        reserve_iop_aliases();
        reserve_low_pool();
    }

    for (i = 0; i < GRX_HW_NRANGES; i++) {
        GrxHwRange *r = &ranges[i];
        uint32_t bad;

        if (r->mapped) {
            continue;
        }
        bad = map_range(r, i);
        if (bad == 0) {
            r->mapped = 1;
            r->error = 0;
            r->blocker = 0;
            continue;
        }
        {
            MEMORY_BASIC_INFORMATION mi;
            char what[MAX_PATH + 96];
            r->blocker = VirtualQuery((void *)(uintptr_t)bad, &mi, sizeof(mi)) && mi.State != MEM_FREE
                             ? (uint32_t)(uintptr_t)mi.AllocationBase
                             : 0;
            hw_log("can't map the %s at 0x%08X-0x%08X (error %lu): %s", r->name, bad, bad + r->size - 1, r->error,
                   grx_hwmem_probe(bad, r->size, what, sizeof(what)));
        }
        failed++;
    }
    return failed;
}

void grx_hwmem_shutdown(void) {
    int i;
    int v;

    for (i = 0; i < GRX_HW_NRANGES; i++) {
        GrxHwRange *r = &ranges[i];
        if (!r->mapped) {
            continue;
        }
        if (r->aliased) {
            for (v = 0; v < 3; v++) {
                UnmapViewOfFile((void *)(uintptr_t)(r->base | alias_bits[v]));
            }
            CloseHandle(sections[i]);
            sections[i] = NULL;
        } else {
            VirtualFree((void *)(uintptr_t)r->base, 0, MEM_RELEASE);
        }
        r->mapped = 0;
    }
}

const GrxHwRange *grx_hwmem_range(int which) {
    return which >= 0 && which < GRX_HW_NRANGES ? &ranges[which] : NULL;
}

int grx_hwmem_mapped(uint32_t addr, uint32_t size) {
    int i;

    for (i = 0; i < GRX_HW_NRANGES; i++) {
        const GrxHwRange *r = &ranges[i];
        uint32_t a = r->aliased ? addr & ~GRX_PS2_UNCACHED_ACC : addr;
        if (r->mapped && a >= r->base && (uint64_t)a + size <= (uint64_t)r->base + r->size) {
            return 1;
        }
    }
    return 0;
}

const char *grx_hwmem_describe(uint32_t a, char *buf, size_t n) {
    if (a >= 0x10000000u && a < 0x10010000u) {
        static const struct { uint32_t base; const char *name; } dma[] = {
            {0x10008000u, "D0 VIF0"}, {0x10009000u, "D1 VIF1"}, {0x1000A000u, "D2 GIF"},
            {0x1000B000u, "D3 fromIPU"}, {0x1000B400u, "D4 toIPU"}, {0x1000C000u, "D5 SIF0"},
            {0x1000C400u, "D6 SIF1"}, {0x1000C800u, "D7 SIF2"}, {0x1000D000u, "D8 fromSPR"},
            {0x1000D400u, "D9 toSPR"},
        };
        size_t i;
        if (a >= 0x10008000u && a < 0x1000E000u) {
            for (i = sizeof(dma) / sizeof(dma[0]); i-- > 0;) {
                if (a >= dma[i].base) {
                    snprintf(buf, n, "EE register (DMA channel %s +0x%X)", dma[i].name, a - dma[i].base);
                    return buf;
                }
            }
        }
        snprintf(buf, n, "EE register (%s)",
                 a < 0x10002000u ? "timers" : a < 0x10003000u ? "IPU" : a < 0x10003800u ? "GIF"
                 : a < 0x10003C00u ? "VIF0" : a < 0x10004000u ? "VIF1" : a < 0x10008000u ? "FIFOs"
                 : a < 0x1000F000u ? "DMAC" : a < 0x1000F100u ? "INTC" : a < 0x1000F200u ? "SIO"
                 : a < 0x1000F500u ? "SIF" : "DMAC enable");
    } else if (a >= 0x10010000u && a < 0x11000000u) {
        snprintf(buf, n, "EE register space (unused)");
    } else if (a >= 0x11000000u && a < 0x11010000u) {
        snprintf(buf, n, "VU memory (%s +0x%X)",
                 a < 0x11004000u ? "VU0 micro" : a < 0x11008000u ? "VU0 data" : a < 0x1100C000u ? "VU1 micro" : "VU1 data",
                 a & 0x3FFFu);
    } else if (a >= 0x12000000u && a < 0x12002000u) {
        snprintf(buf, n, "GS privileged register (+0x%X)", a - 0x12000000u);
    } else if (a >= 0x70000000u && a < 0x70004000u) {
        snprintf(buf, n, "scratchpad (+0x%X)", a - 0x70000000u);
    } else if (a >= 0x20000000u && a < 0x22000000u) {
        snprintf(buf, n, "uncached alias of 0x%08X", a - 0x20000000u);
    } else if (a >= 0x30000000u && a < 0x32000000u) {
        snprintf(buf, n, "uncached-accelerated alias of 0x%08X", a - 0x30000000u);
    } else if (a >= 0x00700000u && a < 0x00750000u) {
        snprintf(buf, n, "load area of the disc's programs (0x%08X)", a);
    } else if (a >= 0x00750000u && a < 0x02000000u) {
        snprintf(buf, n, "fixed RAM (heap/stack/IOP block, 0x%08X)", a);
    } else if (a >= 0x80000000u) {
        snprintf(buf, n, "kernel segment (0x%08X)", a);
    } else {
        snprintf(buf, n, "not a PS2 hardware address (0x%08X)", a);
    }
    return buf;
}

/* ---- memory below GRX_HW_LOW_LIMIT ------------------------------------ */

/* The lowest free, 64 KB aligned block of the given size below GRX_HW_LOW_LIMIT
   whose aliases (if wanted) are free too */
static uintptr_t find_low(size_t size, int aliased) {
    uintptr_t p = 0x00010000u;

    while (p + size <= GRX_HW_LOW_LIMIT) {
        MEMORY_BASIC_INFORMATION mi;
        uintptr_t start, end;
        if (VirtualQuery((void *)p, &mi, sizeof(mi)) == 0) {
            break;
        }
        end = (uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (mi.State == MEM_FREE) {
            start = ((uintptr_t)mi.BaseAddress + 0xFFFF) & ~(uintptr_t)0xFFFF;
            while (start + size <= end && start + size <= GRX_HW_LOW_LIMIT) {
                char what[16];
                if (!aliased || (strcmp(grx_hwmem_probe((uint32_t)(start | GRX_PS2_UNCACHED), (uint32_t)size, what,
                                                        sizeof(what)), "free") == 0 &&
                                 strcmp(grx_hwmem_probe((uint32_t)(start | GRX_PS2_UNCACHED_ACC), (uint32_t)size, what,
                                                        sizeof(what)), "free") == 0)) {
                    return start;
                }
                start += 0x10000;
            }
        }
        p = end;
    }
    return 0;
}

static const char *unfixed;

void grx_hwmem_note_unfixed(const char *what) {
    if (unfixed == NULL) {
        unfixed = what;
        hw_log("%s isn't at its fixed address (something else is there): save states are off", what);
    }
}

const char *grx_hwmem_unfixed(void) {
    return unfixed;
}

void *grx_hwmem_alloc_low_at(uint32_t addr, size_t size) {
    if (addr % GRANULE != 0 || addr >= GRX_HW_LOW_LIMIT) {
        return NULL;
    }
    size = (size + 0xFFFF) & ~(size_t)0xFFFF;
    if (!pool_up) {
        if (ee_nres == 0) {
            reserve_ee_hole();
        }
        reserve_iop_aliases();
        reserve_low_pool();
    }
    return pool_alloc_at(addr / GRANULE, (uint32_t)(size / GRANULE));
}

void *grx_hwmem_alloc_low(size_t size) {
    int tries;
    void *p;

    size = (size + 0xFFFF) & ~(size_t)0xFFFF;
    if (!pool_up) {
        if (ee_nres == 0) {
            reserve_ee_hole();
        }
        reserve_iop_aliases();
        reserve_low_pool();
    }
    p = pool_alloc((uint32_t)(size / GRANULE));
    if (p != NULL) {
        return p;
    }
    /* The pool is full: whatever is still free down there */
    for (tries = 0; tries < 4; tries++) {
        uintptr_t at = find_low(size, 0);
        void *p;
        if (at == 0) {
            break;
        }
        p = VirtualAlloc((void *)at, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p == (void *)at) {
            return p;
        }
        if (p) {
            VirtualFree(p, 0, MEM_RELEASE);
        }
    }
    hw_log("no room for 0x%lX bytes below 0x%08X", (unsigned long)size, GRX_HW_LOW_LIMIT);
    return NULL;
}

void grx_hwmem_free_low(void *p) {
    if (p && !pool_free(p)) {
        VirtualFree(p, 0, MEM_RELEASE);
    }
}

#define MAX_ALIASED 8
static struct {
    void *base;
    size_t size;
    HANDLE section;
} aliased[MAX_ALIASED];

void *grx_hwmem_alloc_aliased(size_t size) {
    static const uint32_t views[3] = {0, GRX_PS2_UNCACHED, GRX_PS2_UNCACHED_ACC};
    HANDLE h;
    int slot;
    int tries;

    for (slot = 0; slot < MAX_ALIASED && aliased[slot].base; slot++) {
    }
    if (slot == MAX_ALIASED) {
        hw_log("too many aliased blocks");
        return NULL;
    }
    size = (size + 0xFFFF) & ~(size_t)0xFFFF;
    h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)size, NULL);
    if (h == NULL) {
        hw_log("CreateFileMapping(0x%lX) failed (error %lu)", (unsigned long)size, GetLastError());
        return NULL;
    }
    for (tries = 0; tries < 4; tries++) {
        uintptr_t at = find_low(size, 1);
        int v;
        if (at == 0) {
            break;
        }
        for (v = 0; v < 3; v++) {
            void *want = (void *)(at | views[v]);
            if (MapViewOfFileEx(h, FILE_MAP_ALL_ACCESS, 0, 0, size, want) != want) {
                break;
            }
        }
        if (v == 3) {
            aliased[slot].base = (void *)at;
            aliased[slot].size = size;
            aliased[slot].section = h;
            return (void *)at;
        }
        while (v-- > 0) {
            UnmapViewOfFile((void *)(at | views[v]));
        }
    }
    hw_log("no room for 0x%lX aliased bytes below 0x%08X", (unsigned long)size, GRX_HW_LOW_LIMIT);
    CloseHandle(h);
    return NULL;
}

void grx_hwmem_free_aliased(void *p) {
    int slot;

    for (slot = 0; slot < MAX_ALIASED; slot++) {
        if (aliased[slot].base == p && p) {
            uintptr_t at = (uintptr_t)p;
            UnmapViewOfFile((void *)at);
            UnmapViewOfFile((void *)(at | GRX_PS2_UNCACHED));
            UnmapViewOfFile((void *)(at | GRX_PS2_UNCACHED_ACC));
            CloseHandle(aliased[slot].section);
            aliased[slot].base = NULL;
            return;
        }
    }
}

/* ---- diagnostics ------------------------------------------------------- */

#ifdef _WIN32
static LONG CALLBACK fault_report(EXCEPTION_POINTERS *ep) {
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    char what[96];
    char module[MAX_PATH] = "?";
    uintptr_t pc = (uintptr_t)er->ExceptionAddress;
    uintptr_t mod = 0;
    MEMORY_BASIC_INFORMATION mi;
    uint32_t addr;

    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    addr = (uint32_t)er->ExceptionInformation[1];
    if (VirtualQuery((void *)pc, &mi, sizeof(mi)) && mi.Type == MEM_IMAGE) {
        mod = (uintptr_t)mi.AllocationBase;
        GetModuleFileNameA((HMODULE)mi.AllocationBase, module, sizeof(module));
    }
    hw_log("access violation: %s 0x%08X, %s, at 0x%08lX (%s+0x%lX)",
           er->ExceptionInformation[0] == 1 ? "write to" : er->ExceptionInformation[0] == 8 ? "execute at" : "read from",
           addr, grx_hwmem_describe(addr, what, sizeof(what)), (unsigned long)pc, module, (unsigned long)(pc - mod));
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

void grx_hwmem_install_fault_report(void) {
    static void *handle;

    if (handle == NULL) {
        handle = AddVectoredExceptionHandler(1, fault_report);
    }
}
#else
#include <signal.h>
#include <ucontext.h>

static void fault_report(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    char what[96];
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    uint32_t addr = (uint32_t)(uintptr_t)si->si_addr;
    struct sigaction dfl;

    hw_log("fault: signal %d at 0x%08X, %s, pc 0x%08lX", sig, addr, grx_hwmem_describe(addr, what, sizeof(what)),
           (unsigned long)pc);
    fflush(stderr);
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigaction(sig, &dfl, NULL);
}

void grx_hwmem_install_fault_report(void) {
    static int done;
    struct sigaction sa;

    if (!done) {
        done = 1;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = fault_report;
        sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
    }
}
#endif
