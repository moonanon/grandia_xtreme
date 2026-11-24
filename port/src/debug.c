/* The freeze tool (grx_debug.h, port/docs/debug.md).

   History, kept while the game runs:
   - pictures: the GPU renderer's, blitted on the GPU into a ring of
     640x448 textures (no read-back until a dump); the software GS's,
     halved into a ring in memory;
   - each field's pad input, and the last log lines.
   A dump writes them, the picture shown now at full size, the kernel's
   threads and the game's memory (the exe's writable sections, the heaps,
   the scratchpad; memory.txt indexes memory.bin) to a new directory.

   The server takes one client at a time on 127.0.0.1, one line per
   request, one line of JSON per answer. Its thread only moves the lines:
   the requests run on the game thread (grx_dbg_poll), between fields. */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
typedef struct { int unused; } WSADATA;
#define INVALID_SOCKET (-1)
#define closesocket close
#define MAKEWORD(a, b) 0
#define WSAStartup(v, d) 0
#define WSAGetLastError() errno
#include <sched.h>
#include <sys/stat.h>
#include <sys/uio.h>
typedef int LONG;
#define InterlockedExchange(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define SwitchToThread() sched_yield()
#define InterlockedIncrement(p) (__atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST))
#define InterlockedDecrement(p) (__atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST))
#define _strdup strdup
extern const char __executable_start[];
#define GetModuleHandleA(x) ((void *)__executable_start)
#define CreateDirectoryA(d, a) mkdir((d), 0777)
#endif
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL3/SDL.h>
#include "grx_debug.h"
#include "grx_video.h"
#include "grx_gl.h"
#include "grx_pad.h"
#include "grx_audio.h"
#include "grx_kernel.h"
#include "grx_memory.h"
#include "grx_hwmem.h"
#include "grx_platform.h"
#include "grx_widescreen.h"
#include "grx_draw_distance.h"
#include "gs/gs_internal.h"
#include "grx_state.h"

#define DEFAULT_PORT 7790
#define NPIC_GL 120   /* GPU pictures (game frames: 2 s at 60, 4 s at 30) */
#define NPIC_CPU 240  /* software pictures, every field (4 s), halved */
#define PIC_W 640
#define PIC_H 448
#define NINPUT 1200   /* fields of pad input (20 s) */
#define LOG_BYTES (128 * 1024)
#define MAX_READ (4 << 20)

extern int grx_gs_display(const uint32_t **pixels, int *width, int *height, int *pitch) __attribute__((weak));

static volatile LONG frozen;    /* holding the fields */
static volatile LONG steps;     /* fields to run before holding again */
static uint64_t field_now;
static uint64_t freeze_at = ~0ull;
static int auto_dump = 1;
static char dump_root[512] = "build/freeze";
static char last_dump[600];

/* ---------------------------------------------------------------------- */
/* Log */

static char logbuf[LOG_BYTES];
static uint64_t log_head; /* bytes written ever; the ring holds the last LOG_BYTES */
static volatile LONG log_lock;

void grx_dbg_log_line(const char *line) {
    size_t n = strlen(line), i;
    while (InterlockedExchange(&log_lock, 1)) {
        SwitchToThread();
    }
    for (i = 0; i < n; i++) {
        logbuf[log_head++ % LOG_BYTES] = line[i];
    }
    logbuf[log_head++ % LOG_BYTES] = '\n';
    InterlockedExchange(&log_lock, 0);
}

/* The log from byte `from` (clamped to what the ring holds) to now, as a
   new string */
static char *log_since(uint64_t from) {
    uint64_t start, i;
    char *s;
    while (InterlockedExchange(&log_lock, 1)) {
        SwitchToThread();
    }
    start = log_head > LOG_BYTES ? log_head - LOG_BYTES : 0;
    if (from < start) {
        from = start;
    }
    s = malloc((size_t)(log_head - from) + 1);
    if (s != NULL) {
        for (i = from; i < log_head; i++) {
            s[i - from] = logbuf[i % LOG_BYTES];
        }
        s[log_head - from] = 0;
    }
    InterlockedExchange(&log_lock, 0);
    return s;
}

/* ---------------------------------------------------------------------- */
/* Input */

static struct {
    uint64_t field;
    unsigned pad[2];
} inputs[NINPUT];
static uint64_t ninputs;

static void input_line(char *buf, size_t n, int i) {
    static const char *const names[16] = {"L2", "R2", "L1", "R1", "tri", "circle", "cross", "square",
                                          "select", "L3", "R3", "start", "up", "right", "down", "left"};
    unsigned p = inputs[i].pad[0];
    size_t k = (size_t)snprintf(buf, n, "%llu stick %02X,%02X", (unsigned long long)inputs[i].field,
                                (p >> 16) & 0xFF, (p >> 24) & 0xFF);
    int b;
    for (b = 0; b < 16 && k < n; b++) {
        if (p & (1u << b)) {
            k += (size_t)snprintf(buf + k, n - k, " %s", names[b]);
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Pictures */

typedef struct {
    uint64_t field;
    float aspect;
    int w, h; /* the part of the slot used */
} PicInfo;

/* GPU: a ring of textures, each with its FBO */
static GLuint gl_tex[NPIC_GL], gl_fbo[NPIC_GL], gl_src_fbo;
static PicInfo gl_info[NPIC_GL];
static uint64_t gl_count;
static SDL_GLContext gl_owner;
static int gl_failed;

static int gl_ring_init(void) {
    int i;
    glGenTextures(NPIC_GL, gl_tex);
    glGenFramebuffers(NPIC_GL, gl_fbo);
    glGenFramebuffers(1, &gl_src_fbo);
    for (i = 0; i < NPIC_GL; i++) {
        glBindTexture(GL_TEXTURE_2D, gl_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, PIC_W, PIC_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gl_fbo[i]);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl_tex[i], 0);
        if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            grx_log("debug: no GL picture history (FBO incomplete)");
            return -1;
        }
    }
    gl_owner = SDL_GL_GetCurrentContext();
    return 0;
}

void grx_dbg_picture_gl(const GrxVideoFrame *f) {
    GLint rd = 0, dr = 0, tex = 0, sc = 0;
    int slot;
    if (gl_failed || f->texture == 0 || f->width <= 0 || f->height <= 0) {
        return;
    }
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rd);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dr);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetIntegerv(GL_SCISSOR_TEST, &sc);
    if (gl_count == 0 && gl_ring_init() != 0) {
        gl_failed = 1;
    } else {
        slot = (int)(gl_count % NPIC_GL);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, gl_src_fbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->texture, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gl_fbo[slot]);
        if (sc) {
            glDisable(GL_SCISSOR_TEST);
        }
        /* Kept bottom-up, as GL reads it back */
        if (f->top_down) {
            glBlitFramebuffer(0, 0, f->width, f->height, 0, PIC_H, PIC_W, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        } else {
            glBlitFramebuffer(0, 0, f->width, f->height, 0, 0, PIC_W, PIC_H, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        }
        if (sc) {
            glEnable(GL_SCISSOR_TEST);
        }
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        gl_info[slot].field = field_now;
        gl_info[slot].aspect = f->aspect;
        gl_info[slot].w = PIC_W;
        gl_info[slot].h = PIC_H;
        gl_count++;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rd);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)dr);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
}

/* CPU: the software GS's picture, halved, every field */
static uint32_t *cpu_pix; /* NPIC_CPU slots of (PIC_W / 2) x (PIC_H / 2) */
static PicInfo cpu_info[NPIC_CPU];
static uint64_t cpu_count;

static void picture_cpu(void) {
    const uint32_t *px;
    int w, h, pitch, x, y, slot, ow, oh;
    uint32_t *d;
    if (gs_backend != NULL || grx_gs_display == NULL) {
        return; /* the GPU renderer's come through grx_dbg_picture_gl */
    }
    if (cpu_pix == NULL) {
        cpu_pix = malloc((size_t)NPIC_CPU * (PIC_W / 2) * (PIC_H / 2) * 4);
        if (cpu_pix == NULL) {
            return;
        }
    }
    if (!grx_gs_display(&px, &w, &h, &pitch) || px == NULL || w <= 1 || h <= 1) {
        return;
    }
    slot = (int)(cpu_count % NPIC_CPU);
    ow = w / 2 < PIC_W / 2 ? w / 2 : PIC_W / 2;
    oh = h / 2 < PIC_H / 2 ? h / 2 : PIC_H / 2;
    d = cpu_pix + (size_t)slot * (PIC_W / 2) * (PIC_H / 2);
    for (y = 0; y < oh; y++) {
        for (x = 0; x < ow; x++) {
            d[y * (PIC_W / 2) + x] = px[(y * 2) * pitch + x * 2];
        }
    }
    cpu_info[slot].field = field_now;
    cpu_info[slot].aspect = grx_ws_aspect();
    cpu_info[slot].w = ow;
    cpu_info[slot].h = oh;
    cpu_count++;
}

/* 24-bit BMP of 0xAARRGGBB pixels, top-down rows `pitch` apart, stretched
   across to the display aspect (nearest) */
static int write_bmp(const char *path, const uint32_t *px, int w, int h, int pitch, float aspect, int bottom_up) {
    int ow = aspect > 0 ? (int)(h * aspect + 0.5f) : w, y, x, row;
    FILE *f;
    unsigned char hdr[54];
    unsigned char *line;
    if (ow < w / 2 || ow > w * 3) {
        ow = w;
    }
    row = (ow * 3 + 3) & ~3;
    f = fopen(path, "wb");
    if (f == NULL) {
        return -1;
    }
    line = calloc(1, (size_t)row);
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    *(uint32_t *)(hdr + 2) = (uint32_t)(54 + row * h);
    *(uint32_t *)(hdr + 10) = 54;
    *(uint32_t *)(hdr + 14) = 40;
    *(int32_t *)(hdr + 18) = ow;
    *(int32_t *)(hdr + 22) = h; /* bottom-up */
    *(uint16_t *)(hdr + 26) = 1;
    *(uint16_t *)(hdr + 28) = 24;
    *(uint32_t *)(hdr + 34) = (uint32_t)(row * h);
    fwrite(hdr, 1, 54, f);
    for (y = 0; y < h && line != NULL; y++) {
        const uint32_t *s = px + (size_t)(bottom_up ? y : h - 1 - y) * pitch;
        for (x = 0; x < ow; x++) {
            uint32_t c = s[(int)((long long)x * w / ow)];
            line[x * 3] = (unsigned char)c;
            line[x * 3 + 1] = (unsigned char)(c >> 8);
            line[x * 3 + 2] = (unsigned char)(c >> 16);
        }
        fwrite(line, 1, (size_t)row, f);
    }
    free(line);
    fclose(f);
    return 0;
}

static int dump_pictures(const char *dir) {
    char path[700];
    int n = 0;
    uint64_t i, first;
    CreateDirectoryA(dir, NULL);
    if (gl_count > 0 && !gl_failed) {
        SDL_GLContext was = SDL_GL_GetCurrentContext();
        uint32_t *buf = malloc((size_t)PIC_W * PIC_H * 4);
        GLint rd = 0;
        if (was != gl_owner) {
            grx_video_gl_make_current(gl_owner);
        }
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rd);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        first = gl_count > NPIC_GL ? gl_count - NPIC_GL : 0;
        for (i = first; i < gl_count && buf != NULL; i++) {
            const PicInfo *pi = &gl_info[i % NPIC_GL];
            int k;
            glBindFramebuffer(GL_READ_FRAMEBUFFER, gl_fbo[i % NPIC_GL]);
            glReadPixels(0, 0, PIC_W, PIC_H, GL_RGBA, GL_UNSIGNED_BYTE, buf);
            for (k = 0; k < PIC_W * PIC_H; k++) {
                uint32_t c = buf[k];
                buf[k] = ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
            }
            snprintf(path, sizeof(path), "%s/%06llu.bmp", dir, (unsigned long long)pi->field);
            if (write_bmp(path, buf, PIC_W, PIC_H, PIC_W, pi->aspect, 1) == 0) {
                n++;
            }
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rd);
        free(buf);
        if (was != gl_owner && was != NULL) {
            grx_video_gl_make_current(was);
        }
    }
    if (cpu_count > 0) {
        first = cpu_count > NPIC_CPU ? cpu_count - NPIC_CPU : 0;
        for (i = first; i < cpu_count; i++) {
            const PicInfo *pi = &cpu_info[i % NPIC_CPU];
            snprintf(path, sizeof(path), "%s/%06llu.bmp", dir, (unsigned long long)pi->field);
            if (write_bmp(path, cpu_pix + (size_t)(i % NPIC_CPU) * (PIC_W / 2) * (PIC_H / 2), pi->w, pi->h,
                          PIC_W / 2, pi->aspect, 0) == 0) {
                n++;
            }
        }
    }
    return n;
}

/* ---------------------------------------------------------------------- */
/* Memory */

static int mem_read(uintptr_t addr, void *buf, size_t n) {
#ifdef _WIN32
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, buf, n, &got) && got == n ? 0 : -1;
#else
    struct iovec l = {buf, n}, r = {(void *)addr, n};
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n ? 0 : -1;
#endif
}

static int mem_write(uintptr_t addr, const void *buf, size_t n) {
#ifdef _WIN32
    SIZE_T put = 0;
    return WriteProcessMemory(GetCurrentProcess(), (LPVOID)addr, buf, n, &put) && put == n ? 0 : -1;
#else
    struct iovec l = {(void *)buf, n}, r = {(void *)addr, n};
    return process_vm_writev(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n ? 0 : -1;
#endif
}

/* Copy [base, base + size) to f, skipping what can't be read (zeros) */
static void dump_range(FILE *f, FILE *idx, const char *name, uintptr_t base, size_t size, long long *off) {
    static unsigned char buf[65536];
    size_t done = 0;
    fprintf(idx, "%s 0x%08llx 0x%llx 0x%llx\n", name, (unsigned long long)base, (unsigned long long)size, *off);
    while (done < size) {
        size_t n = size - done < sizeof(buf) ? size - done : sizeof(buf);
        if (mem_read(base + done, buf, n) != 0) {
            memset(buf, 0, n);
        }
        fwrite(buf, 1, n, f);
        done += n;
    }
    *off += (long long)size;
}

#ifndef _WIN32
extern char grx_state_data_begin[16], grx_state_data_end[16];
extern char grx_state_bss_begin[16], grx_state_bss_end[16];
extern const char __executable_start[];
#endif

static void dump_memory(const char *dir) {
    char path[700];
    FILE *f, *idx;
    long long off = 0;
#ifdef _WIN32
    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
#else
    unsigned char *base = (unsigned char *)__executable_start;
#endif
    int i;

    snprintf(path, sizeof(path), "%s/memory.bin", dir);
    f = fopen(path, "wb");
    snprintf(path, sizeof(path), "%s/memory.txt", dir);
    idx = fopen(path, "w");
    if (f == NULL || idx == NULL) {
        if (f) fclose(f);
        if (idx) fclose(idx);
        return;
    }
    fprintf(idx, "# name host_address size offset_in_memory.bin; image base 0x%08llx\n", (unsigned long long)(uintptr_t)base);
#ifdef _WIN32
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        char name[16];
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) {
            continue;
        }
        snprintf(name, sizeof(name), "exe%.8s", (const char *)sec[i].Name);
        dump_range(f, idx, name, (uintptr_t)base + sec[i].VirtualAddress, sec[i].Misc.VirtualSize, &off);
    }
#else
    /* the game's data and bss: what the save states copy */
    dump_range(f, idx, "exedata", (uintptr_t)grx_state_data_begin, (size_t)(grx_state_data_end + 16 - grx_state_data_begin), &off);
    dump_range(f, idx, "exebss", (uintptr_t)grx_state_bss_begin, (size_t)(grx_state_bss_end + 16 - grx_state_bss_begin), &off);
#endif
    for (i = 0; i < GRX_AREA_COUNT; i++) {
        static const char *const names[GRX_AREA_COUNT] = {"ini_buf", "mfifo", "battle_heap", "face_heap", "heap7"};
        if (i == GRX_AREA_MFIFO_RING) {
            continue; /* the INI buffer's memory */
        }
        dump_range(f, idx, names[i], grx_ee_area(i), grx_ee_area_size(i), &off);
    }
    for (i = 0; i < GRX_HW_NRANGES; i++) {
        const GrxHwRange *r = grx_hwmem_range(i);
        if (r != NULL && r->mapped && strcmp(r->name, "scratchpad") == 0) {
            dump_range(f, idx, r->name, r->base, r->size, &off);
        }
    }
    fclose(f);
    fclose(idx);
}

/* ---------------------------------------------------------------------- */
/* Dumps */

static void write_text(const char *dir, const char *name, const char *text) {
    char path[700];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "w");
    if (f != NULL) {
        fputs(text, f);
        fclose(f);
    }
}

static void mkdirs(const char *path) {
    char p[600];
    size_t i;
    snprintf(p, sizeof(p), "%s", path);
    for (i = 1; p[i]; i++) {
        if (p[i] == '/' || p[i] == '\\') {
            char c = p[i];
            p[i] = 0;
            CreateDirectoryA(p, NULL);
            p[i] = c;
        }
    }
    CreateDirectoryA(p, NULL);
}

static const char *dump(const char *where) {
    char dir[600], path[700], line[256], *s;
    time_t t = time(NULL);
    struct tm tm;
    uint64_t lh, i, first;
    FILE *f;
    int npic;

    if (where != NULL && *where) {
        snprintf(dir, sizeof(dir), "%s", where);
    } else {
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        snprintf(dir, sizeof(dir), "%s/%04d%02d%02d-%02d%02d%02d-f%llu", dump_root, tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (unsigned long long)field_now);
    }
    mkdirs(dir);
    snprintf(path, sizeof(path), "%s/pictures", dir);
    npic = dump_pictures(path);
    if (grx_gs_display != NULL) {
        const uint32_t *px;
        int w, h, pitch;
        if (grx_gs_display(&px, &w, &h, &pitch) && px != NULL) {
            snprintf(path, sizeof(path), "%s/now.bmp", dir);
            write_bmp(path, px, w, h, pitch, 0, 0);
        }
    }
    snprintf(path, sizeof(path), "%s/inputs.txt", dir);
    f = fopen(path, "w");
    if (f != NULL) {
        fprintf(f, "# field, port 1: left stick X,Y and the buttons down\n");
        first = ninputs > NINPUT ? ninputs - NINPUT : 0;
        for (i = first; i < ninputs; i++) {
            input_line(line, sizeof(line), (int)(i % NINPUT));
            fprintf(f, "%s\n", line);
        }
        fclose(f);
    }
    lh = log_head;
    grx_kernel_dump();
    s = log_since(lh);
    if (s != NULL) {
        write_text(dir, "threads.txt", s);
        free(s);
    }
    s = log_since(0);
    if (s != NULL) {
        write_text(dir, "log.txt", s);
        free(s);
    }
    dump_memory(dir);
    snprintf(path, sizeof(path), "%s/state.txt", dir);
    f = fopen(path, "w");
    if (f != NULL) {
        fprintf(f, "field %llu\nrenderer %s\naspect %.4f\ndraw_distance %g\nimage_base 0x%08llx\npictures %d\n",
                (unsigned long long)field_now, gs_backend != NULL ? "gl" : "software", grx_ws_aspect(),
                grx_draw_distance(), 
#ifdef _WIN32
                (unsigned long long)(uintptr_t)GetModuleHandleA(NULL),
#else
                (unsigned long long)(uintptr_t)__executable_start,
#endif
                npic);
        fclose(f);
    }
    snprintf(last_dump, sizeof(last_dump), "%s", dir);
    grx_log("debug: dump in %s (%d pictures)", dir, npic);
    return last_dump;
}

/* ---------------------------------------------------------------------- */
/* Server */

static int server_port = DEFAULT_PORT;
static int server_started;
static SDL_Mutex *req_lock;
static SDL_Condition *req_cond;
static char *req_line;   /* a request waiting for the game thread */
static char *req_answer; /* its answer */

/* Answers: JSON, built here */
typedef struct {
    char *s;
    size_t n, cap;
} Buf;

static void put(Buf *b, const char *fmt, ...) {
    va_list ap;
    int k;
    for (;;) {
        va_start(ap, fmt);
        k = vsnprintf(b->s + b->n, b->cap - b->n, fmt, ap);
        va_end(ap);
        if (k >= 0 && b->n + (size_t)k < b->cap) {
            b->n += (size_t)k;
            return;
        }
        b->cap = b->cap * 2 + (size_t)(k > 0 ? k : 256) + 256;
        b->s = realloc(b->s, b->cap);
        if (b->s == NULL) {
            grx_fatal("debug: out of memory");
        }
    }
}

static void put_str(Buf *b, const char *s) {
    put(b, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            put(b, "\\%c", c);
        } else if (c == '\n') {
            put(b, "\\n");
        } else if (c < 0x20) {
            put(b, "\\u%04x", c);
        } else {
            put(b, "%c", c);
        }
    }
    put(b, "\"");
}

static uintptr_t parse_addr(const char *s, int *ok) {
    char *end;
    unsigned long long v = strtoull(s, &end, 0);
    *ok = end != s && *end == 0;
    return (uintptr_t)v;
}

static void status(Buf *b) {
    put(b, "{\"ok\":true,\"field\":%llu,\"frozen\":%s,\"steps\":%ld,\"renderer\":\"%s\",\"aspect\":%.4f,"
           "\"draw_distance\":%g,\"image_base\":%llu,\"pictures\":%llu,\"last_dump\":",
        (unsigned long long)field_now, frozen ? "true" : "false", (long)steps,
        gs_backend != NULL ? "gl" : "software", grx_ws_aspect(), grx_draw_distance(),
        (unsigned long long)(uintptr_t)GetModuleHandleA(NULL),
        (unsigned long long)(gl_count > 0 ? gl_count : cpu_count));
    put_str(b, last_dump);
    put(b, "}");
}

static void freeze_now(const char *why) {
    if (!frozen) {
        InterlockedExchange(&frozen, 1);
        InterlockedExchange(&steps, 0);
        grx_audio_pause(1);
        grx_log("debug: frozen at field %llu (%s): F9 resumes, F10 steps a field; grxdbg.py on port %d",
                (unsigned long long)field_now, why, server_port);
    }
}

static void resume(void) {
    if (frozen) {
        InterlockedExchange(&frozen, 0);
        grx_audio_pause(0);
        grx_log("debug: resumed at field %llu", (unsigned long long)field_now);
    }
}

static void start_server(void);

/* One request, on the game thread */
static char *run(char *line) {
    Buf b = {NULL, 0, 0};
    char *argv[4] = {0};
    int argc = 0, ok;
    char *p = line;
    while (argc < 4 && *p) {
        while (*p == ' ' || *p == '\t' || *p == '\r') {
            *p++ = 0;
        }
        if (!*p) {
            break;
        }
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r') {
            p++;
        }
    }
    if (argc && argc < 4 && *p) {
        *p = 0;
    }
    if (argc == 0 || strcmp(argv[0], "status") == 0) {
        status(&b);
    } else if (strcmp(argv[0], "freeze") == 0) {
        freeze_now("asked");
        status(&b);
    } else if (strcmp(argv[0], "resume") == 0) {
        resume();
        status(&b);
    } else if (strcmp(argv[0], "step") == 0) {
        long n = argc > 1 ? atol(argv[1]) : 1;
        freeze_now("asked");
        InterlockedExchange(&steps, n > 0 ? n : 1);
        status(&b);
    } else if (strcmp(argv[0], "read") == 0 && argc >= 3) {
        uintptr_t a = parse_addr(argv[1], &ok);
        long n = atol(argv[2]);
        unsigned char *buf;
        if (!ok || n <= 0 || n > MAX_READ) {
            put(&b, "{\"ok\":false,\"error\":\"read ADDR LEN (LEN up to %d)\"}", MAX_READ);
        } else if ((buf = malloc((size_t)n)) == NULL || mem_read(a, buf, (size_t)n) != 0) {
            put(&b, "{\"ok\":false,\"error\":\"can't read 0x%08llx+%ld\"}", (unsigned long long)a, n);
            free(buf);
        } else {
            long i;
            put(&b, "{\"ok\":true,\"addr\":%llu,\"hex\":\"", (unsigned long long)a);
            for (i = 0; i < n; i++) {
                put(&b, "%02x", buf[i]);
            }
            put(&b, "\"}");
            free(buf);
        }
    } else if (strcmp(argv[0], "write") == 0 && argc >= 3) {
        uintptr_t a = parse_addr(argv[1], &ok);
        size_t len = strlen(argv[2]), n = len / 2, i;
        unsigned char *buf = malloc(n + 1);
        /* Nothing is written unless all of it is hex, two digits a byte */
        int hex = len % 2 == 0;
        for (i = 0; i < len; i++) {
            hex &= isxdigit((unsigned char)argv[2][i]) != 0;
        }
        for (i = 0; hex && buf != NULL && i < n; i++) {
            unsigned v;
            sscanf(argv[2] + i * 2, "%2x", &v);
            buf[i] = (unsigned char)v;
        }
        if (!hex) {
            put(&b, "{\"ok\":false,\"error\":\"write ADDR HEX (two hex digits a byte)\"}");
        } else if (!ok || n == 0 || buf == NULL || mem_write(a, buf, n) != 0) {
            put(&b, "{\"ok\":false,\"error\":\"can't write\"}");
        } else {
            put(&b, "{\"ok\":true,\"addr\":%llu,\"bytes\":%llu}", (unsigned long long)a, (unsigned long long)n);
        }
        free(buf);
    } else if (strcmp(argv[0], "ee") == 0 && argc >= 2) {
        uintptr_t a = parse_addr(argv[1], &ok);
        put(&b, "{\"ok\":%s,\"host\":%llu}", ok ? "true" : "false", (unsigned long long)grx_ee_ram((uint32_t)a));
    } else if (strcmp(argv[0], "dump") == 0) {
        const char *d = dump(argc > 1 ? argv[1] : NULL);
        put(&b, "{\"ok\":true,\"dir\":");
        put_str(&b, d);
        put(&b, "}");
    } else if (strcmp(argv[0], "threads") == 0) {
        uint64_t lh = log_head;
        char *s;
        grx_kernel_dump();
        s = log_since(lh);
        put(&b, "{\"ok\":true,\"text\":");
        put_str(&b, s != NULL ? s : "");
        put(&b, "}");
        free(s);
    } else if (strcmp(argv[0], "log") == 0) {
        long n = argc > 1 ? atol(argv[1]) : 4096;
        uint64_t from = log_head > (uint64_t)n ? log_head - (uint64_t)n : 0;
        char *s = log_since(from);
        put(&b, "{\"ok\":true,\"text\":");
        put_str(&b, s != NULL ? s : "");
        put(&b, "}");
        free(s);
    } else if (strcmp(argv[0], "inputs") == 0) {
        long n = argc > 1 ? atol(argv[1]) : 60;
        uint64_t i, first = ninputs > (uint64_t)n ? ninputs - (uint64_t)n : 0;
        char l[256];
        if (ninputs - first > NINPUT) {
            first = ninputs - NINPUT;
        }
        put(&b, "{\"ok\":true,\"lines\":[");
        for (i = first; i < ninputs; i++) {
            input_line(l, sizeof(l), (int)(i % NINPUT));
            put(&b, i > first ? "," : "");
            put_str(&b, l);
        }
        put(&b, "]}");
    } else if (strcmp(argv[0], "pad") == 0 && argc >= 2) {
        /* A press from the next field (step to see it) */
        unsigned hold = argc > 2 ? (unsigned)atoi(argv[2]) : 8;
        if (grx_pad_script_add(argv[1], field_now, hold ? hold : 8) != 0) {
            put(&b, "{\"ok\":false,\"error\":\"pad NAME [FIELDS]: up, down, left, right, cross, circle, square, "
                    "triangle, l1, r1, l2, r2, start, select, rleft, rright, rup, rdown, lleft, lright, lup, ldown\"}");
        } else {
            status(&b);
        }
    } else if (strcmp(argv[0], "key") == 0 && argc >= 2) {
        /* A key press as the window gets it (SDL's scancode name: Escape,
           Tab, Down, Return, M...), for the port's own keys and menu */
        SDL_Scancode sc = SDL_GetScancodeFromName(argv[1]);
        if (sc == SDL_SCANCODE_UNKNOWN) {
            put(&b, "{\"ok\":false,\"error\":\"key NAME: an SDL scancode name (Escape, Tab, Down, Return, M)\"}");
        } else {
            SDL_Event ev;
            SDL_zero(ev);
            ev.type = SDL_EVENT_KEY_DOWN;
            ev.key.scancode = sc;
            ev.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
            ev.key.down = true;
            SDL_PushEvent(&ev);
            status(&b);
        }
    } else if (strcmp(argv[0], "state") == 0 && argc >= 3 && atoi(argv[2]) >= 1 && atoi(argv[2]) <= 4 &&
               (strcmp(argv[1], "save") == 0 || strcmp(argv[1], "load") == 0)) {
        /* A save state, as Shift+F1-F4 / F1-F4 (done at the next field
           boundary where it can be: step or resume to get there) */
        grx_state_request(atoi(argv[2]), strcmp(argv[1], "load") == 0);
        status(&b);
    } else if (strcmp(argv[0], "quit") == 0) {
        /* Ends the game, as closing its window does */
        grx_log("debug: quit asked");
        exit(0);
    } else {
        put(&b, "{\"ok\":false,\"error\":\"commands: status, freeze, resume, step [N], read ADDR LEN, "
                "write ADDR HEX, ee PS2ADDR, dump [DIR], threads, log [BYTES], inputs [FIELDS], pad NAME [FIELDS], state save|load SLOT, quit\"}");
    }
    put(&b, "\n");
    return b.s;
}

void grx_dbg_poll(void) {
    char *line, *answer;
    if (req_lock == NULL) {
        return;
    }
    SDL_LockMutex(req_lock);
    line = req_line;
    req_line = NULL;
    SDL_UnlockMutex(req_lock);
    if (line == NULL) {
        return;
    }
    answer = run(line);
    free(line);
    SDL_LockMutex(req_lock);
    req_answer = answer;
    SDL_BroadcastCondition(req_cond);
    SDL_UnlockMutex(req_lock);
}

static int send_all(SOCKET s, const char *p, size_t n) {
    while (n > 0) {
        int k = send(s, p, n > 65536 ? 65536 : (int)n, 0);
        if (k <= 0) {
            return -1;
        }
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

static void serve_client(SOCKET c) {
    char buf[8192];
    size_t have = 0;
    for (;;) {
        char *nl = memchr(buf, '\n', have);
        if (nl == NULL) {
            int k;
            if (have == sizeof(buf)) {
                return; /* a line too long */
            }
            k = recv(c, buf + have, (int)(sizeof(buf) - have), 0);
            if (k <= 0) {
                return;
            }
            have += (size_t)k;
            continue;
        }
        *nl = 0;
        {
            char *answer;
            SDL_LockMutex(req_lock);
            req_line = _strdup(buf);
            req_answer = NULL;
            while (req_answer == NULL) {
                SDL_WaitCondition(req_cond, req_lock);
            }
            answer = req_answer;
            req_answer = NULL;
            SDL_UnlockMutex(req_lock);
            if (send_all(c, answer, strlen(answer)) != 0) {
                free(answer);
                return;
            }
            free(answer);
        }
        have -= (size_t)(nl + 1 - buf);
        memmove(buf, nl + 1, have);
    }
}

static int SDLCALL server_main(void *arg) {
    SOCKET ls = (SOCKET)(uintptr_t)arg;
    for (;;) {
        SOCKET c = accept(ls, NULL, NULL);
        if (c == INVALID_SOCKET) {
            continue;
        }
        serve_client(c);
        closesocket(c);
    }
    return 0;
}

static void start_server(void) {
    WSADATA wsa;
    SOCKET ls;
    struct sockaddr_in a;
    SDL_Thread *t;
    if (server_started) {
        return;
    }
    server_started = 1;
    req_lock = SDL_CreateMutex();
    req_cond = SDL_CreateCondition();
    if (req_lock == NULL || req_cond == NULL || WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        grx_log("debug: no server");
        return;
    }
    ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)server_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (ls == INVALID_SOCKET || bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(ls, 1) != 0) {
        grx_log("debug: can't listen on 127.0.0.1:%d (error %d)", server_port, WSAGetLastError());
        if (ls != INVALID_SOCKET) {
            closesocket(ls);
        }
        return;
    }
    t = SDL_CreateThread(server_main, "grx-debug", (void *)(uintptr_t)ls);
    if (t == NULL) {
        grx_log("debug: no server thread");
        return;
    }
    SDL_DetachThread(t);
    grx_log("debug: server on 127.0.0.1:%d (port/tools/grxdbg.py)", server_port);
}

/* ---------------------------------------------------------------------- */
/* The frame loop */

void grx_dbg_init(void) {
    const char *e = getenv("GRX_DEBUG_PORT");
    if (e != NULL && atoi(e) > 0) {
        server_port = atoi(e);
        start_server();
    }
    e = getenv("GRX_FREEZE_AT");
    if (e != NULL && *e) {
        freeze_at = strtoull(e, NULL, 0);
    }
    e = getenv("GRX_FREEZE_DIR");
    if (e != NULL && *e) {
        snprintf(dump_root, sizeof(dump_root), "%s", e);
    }
    e = getenv("GRX_FREEZE_DUMP");
    if (e != NULL && *e == '0') {
        auto_dump = 0;
    }
}

static void freeze_and_dump(const char *why) {
    freeze_now(why);
    start_server();
    if (auto_dump) {
        dump(NULL);
    }
}

int grx_dbg_key(unsigned key) {
    if (key == SDLK_F9) {
        if (frozen) {
            resume();
        } else {
            freeze_and_dump("F9");
        }
        return 1;
    }
    if (key == SDLK_F10) {
        if (!frozen) {
            freeze_and_dump("F10");
        } else {
            InterlockedIncrement(&steps);
        }
        return 1;
    }
    return 0;
}

int grx_dbg_holding(void) {
    return frozen && steps <= 0;
}

int grx_dbg_frozen(void) {
    return frozen != 0;
}

void grx_dbg_field(uint64_t field) {
    int slot = (int)(ninputs % NINPUT);
    field_now = field;
    inputs[slot].field = field;
    inputs[slot].pad[0] = grx_pad_raw(0);
    inputs[slot].pad[1] = grx_pad_raw(1);
    ninputs++;
    picture_cpu();
    if (frozen && steps > 0 && InterlockedDecrement(&steps) == 0) {
        grx_log("debug: stepped to field %llu", (unsigned long long)field);
    }
    if (field == freeze_at) {
        freeze_and_dump("GRX_FREEZE_AT");
    }
    grx_dbg_poll();
}
