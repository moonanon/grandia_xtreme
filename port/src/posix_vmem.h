/* The few Win32 virtual-memory calls hwmem.c uses, on POSIX (Linux), so its
   address-space bookkeeping (reserve a hole, commit runs of it, aliased
   views of one section) is the same code on both systems.

   The semantics hwmem.c relies on, not Windows' in general:
   - VirtualAlloc(addr, ...) always names an address, and fails (NULL) when
     anything is already mapped in [addr, addr + size).
   - MEM_RESERVE makes address space nothing else can take (PROT_NONE,
     MAP_NORESERVE); MEM_COMMIT makes it readable and writable, zero-filled.
   - MEM_DECOMMIT returns committed pages to the reserved state, zeroed again.
   - VirtualQuery reports the run of one state around an address, built from
     /proc/self/maps.
   - A "section" is a memfd; MapViewOfFileEx maps it at exactly the address
     asked for. */
#ifndef GRX_POSIX_VMEM_H
#define GRX_POSIX_VMEM_H

#ifndef _WIN32

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef void *HANDLE;
typedef void *HMODULE;
typedef unsigned long DWORD;

#define MAX_PATH 260
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_DECOMMIT 0x4000
#define MEM_RELEASE 0x8000
#define MEM_FREE 0x10000
#define MEM_PRIVATE 0x20000
#define MEM_MAPPED 0x40000
#define MEM_IMAGE 0x1000000
#define PAGE_NOACCESS 0x01
#define PAGE_READWRITE 0x04
#define FILE_MAP_ALL_ACCESS 0xF001F

typedef struct {
    void *BaseAddress;
    void *AllocationBase;
    size_t RegionSize;
    DWORD State;
    DWORD Type;
} MEMORY_BASIC_INFORMATION;

#define PV_MAX_RANGES 256

/* What this process mapped through VirtualAlloc / MapViewOfFileEx, to know
   a size where Windows would remember it */
static struct {
    uintptr_t base;
    size_t size;
    int is_view;
} pv_alloc[PV_MAX_RANGES];

static void pv_note(uintptr_t base, size_t size, int is_view) {
    int i;
    for (i = 0; i < PV_MAX_RANGES; i++) {
        if (pv_alloc[i].size == 0) {
            pv_alloc[i].base = base;
            pv_alloc[i].size = size;
            pv_alloc[i].is_view = is_view;
            return;
        }
    }
}

static size_t pv_take(uintptr_t base) {
    int i;
    for (i = 0; i < PV_MAX_RANGES; i++) {
        if (pv_alloc[i].size != 0 && pv_alloc[i].base == base) {
            size_t n = pv_alloc[i].size;
            pv_alloc[i].size = 0;
            return n;
        }
    }
    return 0;
}

static DWORD GetLastError(void) {
    return (DWORD)errno;
}

static char pv_last_path[MAX_PATH];

/* One mapping of /proc/self/maps */
typedef struct {
    uintptr_t lo, hi;
    char perms[5];
    char path[MAX_PATH];
    int shared;
} PvMap;

/* The mapping at or after addr (the first with hi > addr); 0 when none */
static int pv_next_map(uintptr_t addr, PvMap *m) {
    FILE *f = fopen("/proc/self/maps", "r");
    char line[1024];
    int found = 0;

    if (f == NULL) {
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        unsigned long long lo, hi, off;
        char perms[8], dev[16], path[MAX_PATH] = "";
        unsigned long inode;
        int n = sscanf(line, "%llx-%llx %7s %llx %15s %lu %259[^\n]", &lo, &hi, perms, &off, dev, &inode, path);
        if (n < 6 || hi <= addr) {
            continue;
        }
        m->lo = (uintptr_t)lo;
        m->hi = (uintptr_t)hi;
        memcpy(m->perms, perms, 4);
        m->perms[4] = 0;
        m->shared = perms[3] == 's';
        {
            char *p = path;
            while (*p == ' ') {
                p++;
            }
            snprintf(m->path, sizeof(m->path), "%s", p);
        }
        found = 1;
        break; /* /proc/self/maps is sorted by address */
    }
    fclose(f);
    return found;
}

static size_t VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *mi, size_t len) {
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)0xFFF;
    PvMap m;
    (void)len;

    if (!pv_next_map(a, &m)) {
        mi->BaseAddress = (void *)a;
        mi->AllocationBase = NULL;
        mi->RegionSize = ((uintptr_t)1 << 47) - a;
        mi->State = MEM_FREE;
        mi->Type = 0;
        return sizeof(*mi);
    }
    if (m.lo > a) {
        /* in the gap before the mapping */
        mi->BaseAddress = (void *)a;
        mi->AllocationBase = NULL;
        mi->RegionSize = m.lo - a;
        mi->State = MEM_FREE;
        mi->Type = 0;
        return sizeof(*mi);
    }
    mi->BaseAddress = (void *)m.lo;
    mi->AllocationBase = (void *)m.lo;
    mi->RegionSize = m.hi - m.lo;
    {
        /* Adjacent reservations of ours are one mapping here; report the
           one that holds addr, as Windows does (its allocation base matters
           to hwmem.c's checks) */
        int i;
        for (i = 0; i < PV_MAX_RANGES; i++) {
            if (pv_alloc[i].size != 0 && !pv_alloc[i].is_view && a >= pv_alloc[i].base &&
                a < pv_alloc[i].base + pv_alloc[i].size) {
                uintptr_t lo = pv_alloc[i].base > m.lo ? pv_alloc[i].base : m.lo;
                uintptr_t hi = pv_alloc[i].base + pv_alloc[i].size < m.hi ? pv_alloc[i].base + pv_alloc[i].size : m.hi;
                mi->BaseAddress = (void *)lo;
                mi->AllocationBase = (void *)pv_alloc[i].base;
                mi->RegionSize = hi - lo;
                break;
            }
        }
    }
    mi->State = m.perms[0] == '-' && m.perms[1] == '-' && m.perms[2] == '-' ? MEM_RESERVE : MEM_COMMIT;
    mi->Type = m.path[0] == '/' && !m.shared && m.perms[2] == 'x' ? MEM_IMAGE
               : m.shared || (m.path[0] == '/') ? MEM_MAPPED : MEM_PRIVATE;
    snprintf(pv_last_path, sizeof(pv_last_path), "%s", m.path);
    return sizeof(*mi);
}

static DWORD GetModuleFileNameA(HMODULE mod, char *buf, DWORD n) {
    (void)mod;
    snprintf(buf, n, "%s", pv_last_path);
    return (DWORD)strlen(buf);
}

static void *VirtualAlloc(void *addr, size_t size, DWORD type, DWORD protect) {
    int prot = protect == PAGE_NOACCESS ? PROT_NONE : PROT_READ | PROT_WRITE;
    void *p;

    if (type & MEM_RESERVE) {
        p = mmap(addr, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED) {
            return NULL;
        }
        if (p != addr) {
            munmap(p, size);
            errno = EEXIST;
            return NULL;
        }
        pv_note((uintptr_t)p, size, 0);
        return p;
    }
    /* MEM_COMMIT of reserved space */
    if (mprotect(addr, size, prot) != 0) {
        return NULL;
    }
    return addr;
}

static int VirtualFree(void *addr, size_t size, DWORD type) {
    if (type == MEM_DECOMMIT) {
        /* back to zeroed, reserved pages */
        return mmap(addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) !=
               MAP_FAILED;
    }
    if (size == 0) {
        size = pv_take((uintptr_t)addr);
    }
    return size != 0 && munmap(addr, size) == 0;
}

/* A section is a memfd; HANDLE is the descriptor + 1 so NULL means failure */
static HANDLE CreateFileMappingA(HANDLE file, void *attr, DWORD prot, DWORD hi, DWORD lo, const char *name) {
    int fd;
    (void)file; (void)attr; (void)prot; (void)hi; (void)name;
    fd = memfd_create("grx-hw", 0);
    if (fd < 0) {
        return NULL;
    }
    if (ftruncate(fd, (off_t)lo) != 0) {
        close(fd);
        return NULL;
    }
    return (HANDLE)(intptr_t)(fd + 1);
}

static void *MapViewOfFileEx(HANDLE h, DWORD access, DWORD ohi, DWORD olo, size_t size, void *want) {
    void *p = mmap(want, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, (int)(intptr_t)h - 1, 0);
    (void)access; (void)ohi; (void)olo;
    if (p == MAP_FAILED) {
        return NULL;
    }
    if (p != want) {
        munmap(p, size);
        errno = EEXIST;
        return NULL;
    }
    pv_note((uintptr_t)p, size, 1);
    return p;
}

static int UnmapViewOfFile(void *p) {
    size_t n = pv_take((uintptr_t)p);
    return n != 0 && munmap(p, n) == 0;
}

static int CloseHandle(HANDLE h) {
    return close((int)(intptr_t)h - 1) == 0;
}

#endif /* !_WIN32 */
#endif
