/* The GS's GPU renderer: OpenGL 3.3 core, at an internal scale of the
   GS's resolution (port/docs/gpu.md has the design and its reasons).

   It sits behind the software GS's front end (port/src/gs/: the GIF, the
   registers, transfers, CLUT loads; gs_internal.h's GsBackend) and takes
   its primitives instead of the software queue. Local memory lives in
   three places, tracked per 8 KB page:
   - gs_vram, the CPU's copy, where host-to-local transfers write and
     from where CLUTs and read-backs are taken;
   - the mirror, a 1024 x 1024 R32UI texture with the same words, from
     where the shaders decode textures of any format, and where render
     targets are resolved to (swizzled back into memory order);
   - render targets: colour (RGBA8) and depth (32-bit float) textures at
     the internal scale, one per buffer (base, width, layout). A page is
     owned by at most one target, which then has its newest contents.
   Each page has an owner (a target or none) and two flags: gs_vram is up
   to date, the mirror is up to date. Moving a page's contents between
   them is done when something needs it there: a draw into a target
   loads the pages it doesn't own yet from the mirror (after resolving
   their owner, or uploading gs_vram); a texture from memory makes its
   pages current in the mirror and decodes them into a cached RGBA8
   texture; a texture that is a target's buffer in the same format is
   sampled from the target itself, at the internal scale; the CPU reading
   or partly overwriting a target's pages reads them back first (the only
   place the CPU waits for the GPU).

   Everything runs on the thread that feeds the GS (the game's), in a
   context shared with the presenter (grx_video.h); primitives are batched
   until the draw state changes. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "grx_gl.h"
#include "grx_gs.h"
#include "grx_video.h"
#include "grx_platform.h"
#include "grx_skip.h"
#include "grx_video_opts.h"
#include "../gs/gs_internal.h"
#include "../gs/gs_draw.h"
#include "../gs/gs_texhash.h"
#include "gsgl_replace.h"

extern const char gsgl_vram_glsl[], gsgl_quad_vs[], gsgl_decode_fs[], gsgl_load_fs[], gsgl_loadz_fs[],
    gsgl_resolve_fs[], gsgl_draw_vs[], gsgl_draw_fs[], gsgl_compose_vs[], gsgl_compose_fs[], gsgl_copy_fs[];

/* The internal scale across and down (sx > sy at a wide aspect, port/docs/widescreen.md) */
static int sx = 1, sy = 1;
static int trace; /* GRX_GSGL_TRACE=1: log what moves between memory and targets */
static int no_rt;  /* GRX_GSGL_NORT=1: textures always from memory (to compare) */
static int clip01; /* glClipControl: depth in [0, 1] */

/* ---------------------------------------------------------------------- */
/* Statistics (GRX_GS_STATS=1: at exit) */

enum { S_FLUSH, S_DRAWS, S_PRIMS, S_LOADS, S_SKIPPED, S_RESOLVES, S_UPLOADS, S_READBACKS, S_DECODES, S_COMPOSES,
       S_SELFCOPY, S_N };
static const char *const stat_names[S_N] = {"batches",  "draw calls", "primitives", "page loads", "pages overwritten unloaded", "page resolves",
                                            "page uploads", "page read-backs", "texture decodes", "pictures",
                                            "self copies"};
static uint64_t nstat[S_N];
static int stats_on;

static void print_stats(void) {
    int i;
    fprintf(stderr, "gs-gl:");
    for (i = 0; i < S_N; i++) {
        fprintf(stderr, " %s %llu%s", stat_names[i], (unsigned long long)nstat[i], i + 1 < S_N ? "," : "\n");
    }
}

/* ---------------------------------------------------------------------- */
/* GL objects */

typedef struct Prog {
    GLuint p;
    GLint vp, vps, zmax, tf, tw, reg, tsz, toff, at, fog, bk, bm, fix, misc, dimx, scale;
    GLint fmt, texa, items, geo, lay, r1, r2, o1, o2, pm, off;
} Prog;

static Prog p_draw, p_decode, p_load, p_loadz, p_resolve, p_compose, p_copy;
static GLuint vram_tex, vram_fbo, sw_tex, inv_tex, clut_tex, vao, vbo, empty_vao;
static GLuint samp[6]; /* decoded: [wrap s repeat?][wrap t repeat?] as 0..3; 4: a target (clamped); 5: a replacement (mipmapped, wrapped in the shader) */
static GLuint draw_fbo;
static GLuint copy_tex, copy_fbo;
static int copy_w, copy_h;

#define VBO_SIZE (8u << 20)
static uint32_t vbo_pos;

static GLuint program(const char *vs, const char *fs, int vram, const char *what, Prog *p) {
    static char vbuf[32768], fbuf[32768];
    const char *hdr = "#version 330 core\n";
    if (vram) {
        snprintf(vbuf, sizeof(vbuf), "%s", vs);
        snprintf(fbuf, sizeof(fbuf), "%s%s%s", hdr, gsgl_vram_glsl, fs);
        fs = fbuf;
    }
    memset(p, 0, sizeof(*p));
    p->p = grx_gl_program(vs, fs, what);
    if (p->p == 0) {
        return 0;
    }
#define LOC(f, n) p->f = glGetUniformLocation(p->p, n)
    LOC(vp, "u_vp"); LOC(vps, "u_vps"); LOC(zmax, "u_zmax"); LOC(tf, "u_tf"); LOC(tw, "u_tw"); LOC(reg, "u_reg");
    LOC(tsz, "u_tsz"); LOC(toff, "u_toff"); LOC(at, "u_at"); LOC(fog, "u_fog"); LOC(bk, "u_bk");
    LOC(bm, "u_bm"); LOC(fix, "u_fix"); LOC(misc, "u_misc"); LOC(dimx, "u_dimx"); LOC(scale, "u_scale");
    LOC(fmt, "u_fmt"); LOC(texa, "u_texa"); LOC(items, "u_items"); LOC(geo, "u_geo"); LOC(lay, "u_lay");
    LOC(r1, "u_r1"); LOC(r2, "u_r2"); LOC(o1, "u_o1"); LOC(o2, "u_o2"); LOC(pm, "u_pm"); LOC(off, "u_off");
#undef LOC
    /* Texture units: 0 the texture / source, 1 the destination or CLUT or
       inverse tables or circuit 2, 2 the mirror, 3 the swizzle tables */
    glUseProgram(p->p);
    {
        static const char *const u0[] = {"u_tex", "u_src", "u_c1"};
        static const char *const u1[] = {"u_dst", "u_clut", "u_inv", "u_c2"};
        int i;
        for (i = 0; i < 3; i++) {
            GLint l = glGetUniformLocation(p->p, u0[i]);
            if (l >= 0) {
                glUniform1i(l, 0);
            }
        }
        for (i = 0; i < 4; i++) {
            GLint l = glGetUniformLocation(p->p, u1[i]);
            if (l >= 0) {
                glUniform1i(l, 1);
            }
        }
        if (glGetUniformLocation(p->p, "u_vram") >= 0) {
            glUniform1i(glGetUniformLocation(p->p, "u_vram"), 2);
        }
        if (glGetUniformLocation(p->p, "u_sw") >= 0) {
            glUniform1i(glGetUniformLocation(p->p, "u_sw"), 3);
        }
    }
    (void)vbuf;
    return p->p;
}

static void bind_tex(int unit, GLuint t) {
    glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    glBindTexture(GL_TEXTURE_2D, t);
}

static GLuint new_tex(GLint ifmt, int w, int h, GLenum fmt, GLenum type) {
    GLuint t;
    glGenTextures(1, &t);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, NULL);
    return t;
}

static GLuint new_fbo(GLuint color, GLuint depth) {
    GLuint f;
    glGenFramebuffers(1, &f);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
    return f;
}

/* The plain state the helpers below draw with */
static void plain_state(void) {
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

/* ---------------------------------------------------------------------- */
/* Pages and targets */

#define NT 32
typedef struct Tgt {
    int used, depth, lay;
    uint32_t bp, bw;  /* blocks (page-aligned), width in 64 pixels */
    int w, h;         /* native pixels */
    GLuint tex, fbo;
    uint32_t last;    /* vsync it was last used */
} Tgt;
static Tgt tg[NT];

static uint8_t owner[512]; /* target + 1, or 0 */
static uint8_t cpu_ok[512], gpu_ok[512], ver_read[512];
static uint32_t ver[512], gen = 1;
static uint32_t vsyncs;
static int need_setup;      /* memory changed: set up again */
static int tgt_pin = -1;    /* a target find_target mustn't evict (one still in use) */

static int lay_cell_h(int lay) {
    return lay <= GS_L32Z ? 32 : 64;
}

static void flush(void);
static int rescale(void);
static int disp_dirty = 1;

/* A page's contents changed (a draw, a CPU write) */
static inline void page_written(int p) {
    gpu_ok[p] = 0;
    if (ver_read[p]) {
        ver[p] = ++gen;
        ver_read[p] = 0;
    }
}

/* Upload pages from gs_vram to the mirror (runs of consecutive pages) */
static void upload_pages(const uint8_t *want) {
    int p = 0;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, vram_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    while (p < 512) {
        int q;
        if (!want[p]) {
            p++;
            continue;
        }
        for (q = p; q < 512 && want[q]; q++) {
            gpu_ok[q] = 1;
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, p * 2, 1024, (q - p) * 2, GL_RED_INTEGER, GL_UNSIGNED_INT,
                        gs_vram + (size_t)p * 8192);
        nstat[S_UPLOADS] += (uint64_t)(q - p);
        p = q;
    }
}

static void set_items(const Prog *pr, const int *items, int n) {
    static GLint buf[512];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, items, (size_t)n * sizeof(int));
    glUniform4iv(pr->items, (n + 3) / 4, buf);
}

/* Target t's pages (all owned by it) back into the mirror */
static void resolve(int t, const int *pages, int n) {
    const Tgt *g = &tg[t];
    int i;
    if (n == 0) {
        return;
    }
    flush();
    if (trace)
        grx_log("trace: resolve target %d (bp %x bw %u lay %d depth %d) %d pages from %d", t, g->bp, g->bw, g->lay,
                g->depth, n, pages[0]);
    plain_state();
    glBindFramebuffer(GL_FRAMEBUFFER, vram_fbo);
    glViewport(0, 0, 1024, 1024);
    glUseProgram(p_resolve.p);
    glBindVertexArray(empty_vao);
    bind_tex(0, g->tex);
    glBindSampler(0, 0);
    bind_tex(1, inv_tex);
    glBindSampler(1, 0);
    glUniform4i(p_resolve.fmt, g->lay, g->depth, (GLint)g->bp, (GLint)g->bw);
    glUniform2i(p_resolve.scale, sx, sy);
    glUniform4f(p_resolve.geo, 1024, 2, 2.0f / 1024, 2.0f / 1024);
    glUniform4i(p_resolve.lay, 0, 1, 1, 1);
    for (i = 0; i < n; i += 512) {
        int k = n - i < 512 ? n - i : 512;
        set_items(&p_resolve, pages + i, k);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 6, k);
    }
    for (i = 0; i < n; i++) {
        gpu_ok[pages[i]] = 1;
    }
    nstat[S_RESOLVES] += (uint64_t)n;
}

/* Make the mirror current for these pages */
static void mirror_ready(const uint8_t *want) {
    static uint8_t up[512];
    static int list[NT][512];
    int n[NT], p, t, any_up = 0;
    memset(n, 0, sizeof(n));
    memset(up, 0, sizeof(up));
    for (p = 0; p < 512; p++) {
        if (!want[p] || gpu_ok[p]) {
            continue;
        }
        if (owner[p]) {
            t = owner[p] - 1;
            list[t][n[t]++] = p;
        } else {
            up[p] = 1;
            any_up = 1;
        }
    }
    for (t = 0; t < NT; t++) {
        if (n[t]) {
            resolve(t, list[t], n[t]);
        }
    }
    if (any_up) {
        upload_pages(up);
    }
}

static void pages_to_want(const GsPages *s, uint8_t *want) {
    int p;
    for (p = 0; p < 512; p++) {
        want[p] = (uint8_t)((s->w[p >> 5] >> (p & 31)) & 1);
    }
}

/* gs_vram current for these pages (the CPU waits for the GPU) */
static void read_back(const uint8_t *want) {
    static uint8_t need[512];
    int p = 0, any = 0;
    for (p = 0; p < 512; p++) {
        need[p] = want[p] && !cpu_ok[p];
        any |= need[p];
    }
    if (!any) {
        return;
    }
    flush();
    mirror_ready(need);
    glBindFramebuffer(GL_FRAMEBUFFER, vram_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    p = 0;
    while (p < 512) {
        int q;
        if (!need[p]) {
            p++;
            continue;
        }
        for (q = p; q < 512 && need[q]; q++) {
            cpu_ok[q] = 1;
        }
        glReadPixels(0, p * 2, 1024, (q - p) * 2, GL_RED_INTEGER, GL_UNSIGNED_INT, gs_vram + (size_t)p * 8192);
        nstat[S_READBACKS] += (uint64_t)(q - p);
        p = q;
    }
}

static int tgt_page(const Tgt *g, int cell) {
    return (int)(((g->bp >> 5) + (uint32_t)cell) & 511);
}

/* A target's texture, (re)allocated to h native rows (contents kept) */
static void tgt_alloc(Tgt *g, int h) {
    GLuint ot = g->tex, of = g->fbo;
    int oh = g->h;
    if (g->depth) {
        g->tex = new_tex(GL_DEPTH_COMPONENT32F, g->w * sx, h * sy, GL_DEPTH_COMPONENT, GL_FLOAT);
        g->fbo = new_fbo(0, g->tex);
        glDrawBuffer(GL_NONE);
    } else {
        g->tex = new_tex(GL_RGBA8, g->w * sx, h * sy, GL_RGBA, GL_UNSIGNED_BYTE);
        g->fbo = new_fbo(g->tex, 0);
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        grx_log("gs-gl: a %dx%d target is incomplete", g->w * sx, h * sy);
    }
    if (ot != 0) {
        /* the rows it had */
        plain_state();
        glUseProgram(p_copy.p);
        glBindVertexArray(empty_vao);
        bind_tex(0, ot);
        glBindSampler(0, 0);
        glViewport(0, 0, g->w * sx, oh * sy);
        glUniform4i(p_copy.off, 0, 0, g->depth, 0);
        if (g->depth) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_ALWAYS);
            glDepthMask(GL_TRUE);
        }
        glDrawArrays(GL_TRIANGLES, 0, 3);
        plain_state();
        glDeleteFramebuffers(1, &of);
        glDeleteTextures(1, &ot);
    }
    g->h = h;
}

static int find_target(uint32_t bp, uint32_t bw, int depth, int lay, int h) {
    int i, free_i = -1, old = -1;
    Tgt *g;
    if (h > 2048) {
        h = 2048;
    }
    for (i = 0; i < NT; i++) {
        g = &tg[i];
        if (!g->used) {
            if (free_i < 0) {
                free_i = i;
            }
            continue;
        }
        if (g->bp == bp && g->bw == bw && g->depth == depth && g->lay == lay) {
            g->last = vsyncs;
            if (h > g->h) {
                flush();
                tgt_alloc(g, (h + 63) & ~63);
            }
            return i;
        }
        if (i != tgt_pin && (old < 0 || g->last < tg[old].last)) {
            old = i;
        }
    }
    if (free_i < 0) {
        /* Evict the least recently used: its pages go back to memory */
        static uint8_t want[512];
        int p;
        flush();
        free_i = old;
        g = &tg[old];
        memset(want, 0, sizeof(want));
        for (p = 0; p < 512; p++) {
            want[p] = owner[p] == old + 1;
        }
        mirror_ready(want);
        for (p = 0; p < 512; p++) {
            if (owner[p] == old + 1) {
                owner[p] = 0;
                cpu_ok[p] = 0;
            }
        }
        /* (gs_vram for them comes from the mirror if the CPU reads them) */
        glDeleteFramebuffers(1, &g->fbo);
        glDeleteTextures(1, &g->tex);
        /* the batch's state may name this slot (its targets, its texture) */
        need_setup = 1;
    }
    g = &tg[free_i];
    memset(g, 0, sizeof(*g));
    g->used = 1;
    g->bp = bp;
    g->bw = bw;
    g->depth = depth;
    g->lay = lay;
    g->w = (int)bw * 64;
    g->last = vsyncs;
    flush();
    tgt_alloc(g, h < 64 ? 64 : (h + 63) & ~63);
    return free_i;
}

/* Target t takes the cells (row * bw + column) it doesn't own yet: their
   contents come from the mirror */
static void acquire(int t, const int *cells, int n) {
    static int need[1024], pages[1024];
    static uint8_t want[512];
    Tgt *g = &tg[t];
    int i, k = 0, ch = lay_cell_h(g->lay);
    for (i = 0; i < n; i++) {
        int p = tgt_page(g, cells[i]);
        if (owner[p] != t + 1) {
            need[k] = cells[i];
            pages[k++] = p;
        }
    }
    if (k == 0) {
        return;
    }
    flush();
    if (trace)
        grx_log("trace: target %d (bp %x bw %u lay %d depth %d) loads %d pages from %d (owner %d)", t, g->bp, g->bw,
                g->lay, g->depth, k, pages[0], owner[pages[0]] - 1);
    memset(want, 0, sizeof(want));
    for (i = 0; i < k; i++) {
        want[pages[i]] = 1;
    }
    mirror_ready(want);
    plain_state();
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glViewport(0, 0, g->w * sx, g->h * sy);
    glBindVertexArray(empty_vao);
    if (g->depth) {
        glUseProgram(p_loadz.p);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
        glDepthMask(GL_TRUE);
    } else {
        glUseProgram(p_load.p);
    }
    {
        const Prog *pr = g->depth ? &p_loadz : &p_load;
        int mode = g->lay >= GS_L16 ? 1 : 0;
        bind_tex(2, vram_tex);
        bind_tex(3, sw_tex);
        glBindSampler(2, 0);
        glBindSampler(3, 0);
        glUniform4i(pr->fmt, g->lay, mode, (GLint)g->bp, (GLint)g->bw);
        glUniform2i(pr->scale, sx, sy);
        glUniform4f(pr->geo, 0, 0, 2.0f / (float)(g->w * sx), 2.0f / (float)(g->h * sy));
        glUniform4i(pr->lay, 1, (GLint)g->bw, 64, ch);
        for (i = 0; i < k; i += 512) {
            int m = k - i < 512 ? k - i : 512;
            set_items(pr, need + i, m);
            glDrawArraysInstanced(GL_TRIANGLES, 0, 6, m);
        }
    }
    plain_state();
    for (i = 0; i < k; i++) {
        owner[pages[i]] = (uint8_t)(t + 1);
    }
    nstat[S_LOADS] += (uint64_t)k;
}

/* The cells of a target that a rectangle of native pixels (x1, y1
   exclusive) covers, owned by it from here on; with `write`, their memory
   copies are out of date */
static void take_rect_cov(int t, int x0, int y0, int x1, int y1, int write, const int *cov);
static void take_rect(int t, int x0, int y0, int x1, int y1, int write) {
    take_rect_cov(t, x0, y0, x1, y1, write, NULL);
}

/* ... where the cells wholly inside cov (x0, y0, x1, y1) are overwritten
   anyway: they needn't be loaded */
static void take_rect_cov(int t, int x0, int y0, int x1, int y1, int write, const int *cov) {
    static int cells[1024];
    Tgt *g = &tg[t];
    int ch = lay_cell_h(g->lay), r, c, n = 0, bw = (int)g->bw;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g->w) x1 = g->w;
    if (y1 > 2048) y1 = 2048;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    if (y1 > g->h) {
        flush();
        tgt_alloc(g, (y1 + 63) & ~63);
    }
    for (r = y0 / ch; r <= (y1 - 1) / ch; r++) {
        for (c = x0 / 64; c <= (x1 - 1) / 64 && n < 1024; c++) {
            int cell = r * bw + c, p = tgt_page(g, cell);
            if (owner[p] == t + 1) {
                continue;
            }
            if (cov != NULL && c * 64 >= cov[0] && (c + 1) * 64 <= cov[2] && r * ch >= cov[1] &&
                (r + 1) * ch <= cov[3]) {
                owner[p] = (uint8_t)(t + 1);
                nstat[S_SKIPPED]++;
                continue;
            }
            cells[n++] = cell;
        }
    }
    if (n) {
        acquire(t, cells, n);
    }
    if (write) {
        for (r = y0 / ch; r <= (y1 - 1) / ch; r++) {
            for (c = x0 / 64; c <= (x1 - 1) / 64; c++) {
                int p = tgt_page(g, r * bw + c);
                cpu_ok[p] = 0;
                page_written(p);
            }
        }
        disp_dirty = 1;
    }
    g->last = vsyncs;
}

/* ---------------------------------------------------------------------- */
/* Texture cache: textures from memory, decoded into RGBA8 */

#define NTEX 384
typedef struct TexE {
    uint32_t key[6]; /* tbp, tbw, psm, tw | th << 8, texa (16/24-bit), CLUT hash (indexed) */
    uint32_t stamp, used;
    GLuint tex, fbo;
    GLuint rep; /* its replacement (gsgl_replace.c), or 0 */
    int w, h;
    GsPages pages;
} TexE;
static TexE texc[NTEX];
static int tex_is_rep; /* what texture_from_memory returned is a replacement */
static uint32_t tex_clock;

static uint32_t hash_words(const uint32_t *w, int n) {
    uint32_t h = 2166136261u;
    int i;
    for (i = 0; i < n; i++) {
        h = (h ^ w[i]) * 16777619u;
    }
    return h;
}

static int pages_current(const GsPages *s, uint32_t stamp) {
    int i;
    for (i = 0; i < 16; i++) {
        uint32_t m = s->w[i];
        while (m) {
            int p = i * 32 + __builtin_ctz(m);
            m &= m - 1;
            if (ver[p] > stamp) {
                return 0;
            }
        }
    }
    return 1;
}

static GLuint texture_from_memory(const GsDraw *D) {
    const GsTex *t = &D->t;
    uint32_t key[6];
    int i, w = 1 << t->tw, h = 1 << t->th, e = -1, lru = 0, indexed;
    TexE *x;
    static uint8_t want[512];

    indexed = t->kind == GS_K8 || t->kind == GS_K4 || t->kind == GS_K8H || t->kind == GS_K4HL || t->kind == GS_K4HH;
    key[0] = t->tbp;
    key[1] = t->tbw;
    key[2] = t->psm;
    key[3] = t->tw | t->th << 8;
    key[4] = (t->kind == GS_K16 || t->kind == GS_K24) ? (t->ta0 | t->ta1 << 8 | t->aem << 16) : 0;
    key[5] = indexed ? hash_words(t->pal, (t->kind == GS_K8 || t->kind == GS_K8H) ? 256 : 16) : 0;
    for (i = 0; i < NTEX; i++) {
        if (texc[i].tex != 0 && memcmp(texc[i].key, key, sizeof(key)) == 0) {
            e = i;
            break;
        }
        if (texc[i].used < texc[lru].used) {
            lru = i;
        }
    }
    x = e >= 0 ? &texc[e] : &texc[lru];
    x->used = ++tex_clock;
    if (e >= 0 && pages_current(&x->pages, x->stamp)) {
        tex_is_rep = x->rep != 0;
        return x->rep ? x->rep : x->tex;
    }
    /* Decode: the mirror must have the pages */
    pages_to_want(&D->rtex, want);
    mirror_ready(want);
    gs_texhash_log(t);
    x->rep = gsgl_rep_lookup(t);
    if (e < 0) {
        memcpy(x->key, key, sizeof(key));
        x->pages = D->rtex;
        if (x->tex == 0 || x->w != w || x->h != h) {
            if (x->tex != 0) {
                glDeleteFramebuffers(1, &x->fbo);
                glDeleteTextures(1, &x->tex);
            }
            x->tex = new_tex(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE);
            x->fbo = new_fbo(x->tex, 0);
            x->w = w;
            x->h = h;
        }
    }
    x->stamp = gen;
    for (i = 0; i < 512; i++) {
        if (want[i]) {
            ver_read[i] = 1;
        }
    }
    plain_state();
    glBindFramebuffer(GL_FRAMEBUFFER, x->fbo);
    glViewport(0, 0, w, h);
    glUseProgram(p_decode.p);
    glBindVertexArray(empty_vao);
    if (indexed) {
        bind_tex(1, clut_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, t->pal);
        glBindSampler(1, 0);
    }
    bind_tex(2, vram_tex);
    bind_tex(3, sw_tex);
    glBindSampler(2, 0);
    glBindSampler(3, 0);
    glUniform4i(p_decode.fmt, (GLint)t->layout, (GLint)t->kind, (GLint)t->tbp, (GLint)t->tbw);
    glUniform4i(p_decode.texa, (GLint)t->ta0, (GLint)t->ta1, (GLint)t->aem, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    nstat[S_DECODES]++;
    tex_is_rep = x->rep != 0;
    return x->rep ? x->rep : x->tex;
}

/* ---------------------------------------------------------------------- */
/* Drawing */

typedef struct V {
    float x, y;
    uint32_t z;
    uint8_t c[4];
    float s, t, q;
    float f;
    float cl[4]; /* texel coordinates clamped to this (sprites, upscaled) */
} V;

/* A batch's state: everything the draw calls depend on */
typedef struct St {
    int ct, dt;
    GLenum zfunc; /* 0: no depth test */
    int zwrite, cmask;
    int sc[4]; /* native x0, y0, x1, y1 (exclusive) */
    int blend;
    GLenum eq;
    GLuint tex;
    int samp, texw, texh;
    int pass2, cmask2, zwrite2;
    uint32_t zmax;
    int tf[4], tw[4], reg[4], at[4], bm[4], misc[4], dimx[16];
    float tsz[4], toff[4], fog[4], bk[4], fix;
} St;

static GsDraw gd;           /* the GS state of the batch */
static int gd_valid;
static St st;               /* its GL state, once set up */
static int st_ready;        /* set up (texture resolved) */
static int st_skip;         /* draws nothing (invalid frame) */
static V *vb;
static int nvb, vbcap;
static int warned_ad, warned_date;
static int rt_tex;          /* the batch's texture is target rt_tex - 1 */

static void flush(void) {
    const St *s = &st;
    const Tgt *c;
    uint32_t bytes;
    uint8_t *dst;
    int pass;

    if (nvb == 0) {
        return;
    }
    if (grx_skip_fast()) {
        /* A scene skip (grx_skip.h) hides the picture: the game runs
           faster without the draws, and the first frame after it draws
           everything again */
        nvb = 0;
        return;
    }
    c = &tg[s->ct];
    nstat[S_FLUSH]++;
    bytes = (uint32_t)nvb * sizeof(V);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    if (vbo_pos + bytes > VBO_SIZE) {
        vbo_pos = 0;
        dst = glMapBufferRange(GL_ARRAY_BUFFER, 0, bytes, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    } else {
        dst = glMapBufferRange(GL_ARRAY_BUFFER, vbo_pos, bytes, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
    }
    if (dst == NULL) {
        grx_log("gs-gl: can't map the vertex buffer");
        nvb = 0;
        return;
    }
    memcpy(dst, vb, bytes);
    glUnmapBuffer(GL_ARRAY_BUFFER);

    glBindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->tex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, s->dt >= 0 ? tg[s->dt].tex : 0, 0);
    glViewport(0, 0, c->w * sx, c->h * sy);
    glEnable(GL_SCISSOR_TEST);
    glScissor(s->sc[0] * sx, s->sc[1] * sy, (s->sc[2] - s->sc[0]) * sx, (s->sc[3] - s->sc[1]) * sy);
    glDisable(GL_CULL_FACE);
    glUseProgram(p_draw.p);
    glBindVertexArray(vao);
    glUniform2f(p_draw.vps, (float)sx, (float)sy);
    glUniform4f(p_draw.vp, 1.0f, 2.0f / (float)(c->w * sx), 2.0f / (float)(c->h * sy),
                clip01 ? 1.0f : 0.0f);
    glUniform1ui(p_draw.zmax, s->zmax);
    glUniform4iv(p_draw.tf, 1, s->tf);
    glUniform4iv(p_draw.tw, 1, s->tw);
    glUniform4iv(p_draw.reg, 1, s->reg);
    glUniform4fv(p_draw.tsz, 1, s->tsz);
    {
        float toff[4];
        memcpy(toff, s->toff, sizeof(toff));
        if (rt_tex && s->tex != copy_tex) {
            toff[2] = 1.0f / (float)(tg[rt_tex - 1].w * sx);
            toff[3] = 1.0f / (float)(tg[rt_tex - 1].h * sy);
        }
        glUniform4fv(p_draw.toff, 1, toff);
    }
    glUniform4fv(p_draw.fog, 1, s->fog);
    glUniform4fv(p_draw.bk, 1, s->bk);
    glUniform4iv(p_draw.bm, 1, s->bm);
    glUniform1f(p_draw.fix, s->fix);
    glUniform4iv(p_draw.misc, 1, s->misc);
    glUniform4iv(p_draw.dimx, 4, s->dimx);
    glUniform2i(p_draw.scale, sx, sy);
    if (s->tf[0]) {
        /* (a target's texture changes when it grows) */
        bind_tex(0, rt_tex && s->tex != copy_tex ? tg[rt_tex - 1].tex : s->tex);
        glBindSampler(0, samp[s->samp]);
    }
    if (s->misc[0]) {
        bind_tex(1, c->tex);
        glBindSampler(1, 0);
        glTextureBarrier();
    }
    if (s->dt >= 0) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(s->zfunc);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    if (s->blend) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE, GL_SRC1_COLOR, GL_ONE, GL_ZERO);
        glBlendEquationSeparate(s->eq, GL_FUNC_ADD);
    } else {
        glDisable(GL_BLEND);
    }
    for (pass = 0; pass < (s->pass2 ? 2 : 1); pass++) {
        int cm = pass ? s->cmask2 : s->cmask, zw = pass ? s->zwrite2 : s->zwrite, at[4];
        memcpy(at, s->at, sizeof(at));
        if (pass) {
            at[2] = 1;
        }
        glUniform4iv(p_draw.at, 1, at);
        glColorMask((cm & 1) != 0, (cm & 2) != 0, (cm & 4) != 0, (cm & 8) != 0);
        glDepthMask(s->dt >= 0 && zw ? GL_TRUE : GL_FALSE);
        glDrawArrays(GL_TRIANGLES, (GLint)(vbo_pos / sizeof(V)), nvb);
        nstat[S_DRAWS]++;
    }
    vbo_pos += bytes; /* (a whole number of vertices: the next draw starts at vbo_pos / sizeof(V)) */
    nvb = 0;
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

/* The GL state for the GS state in gd, but for the texture */
static void setup_state(void) {
    const GsDraw *D = &gd;
    St *s = &st;
    int lay, i, as, ds, ad, dd, ns, nd;

    memset(s, 0, sizeof(*s));
    st_skip = !D->fvalid || D->fbw == 0;
    if (st_skip) {
        return;
    }
    lay = D->f16 ? 2 + D->flay : D->flay;
    s->ct = find_target(D->fbp, D->fbw, 0, lay, D->scy1 + 1 < 448 ? D->scy1 + 1 : 448);
    s->dt = -1;
    if (D->zon) {
        int zl = D->z16 ? 2 + D->zlay : GS_L32Z;
        tgt_pin = s->ct; /* (finding the depth target mustn't evict the colour one) */
        s->dt = find_target(D->zbp, D->fbw, 1, zl, D->scy1 + 1 < 448 ? D->scy1 + 1 : 448);
        tgt_pin = -1;
        s->zfunc = D->ztst == 0 ? GL_NEVER : D->ztst == 1 ? GL_ALWAYS : D->ztst == 2 ? GL_GEQUAL : GL_GREATER;
        s->zwrite = D->zwrite;
        s->zmax = D->zmax;
    } else {
        s->zmax = 0xFFFFFFFFu;
    }
    /* colour channels written (a byte of FBMSK all set: that channel off) */
    if (D->f16) {
        s->cmask = ((D->fbmsk16 & 0x1F) != 0x1F) | (((D->fbmsk16 >> 5) & 0x1F) != 0x1F) << 1 |
                   (((D->fbmsk16 >> 10) & 0x1F) != 0x1F) << 2 | (((D->fbmsk16 >> 15) & 1) != 1) << 3;
    } else {
        for (i = 0; i < 4; i++) {
            if (((D->fbmsk >> (i * 8)) & 0xFF) != 0xFF) {
                s->cmask |= 1 << i;
            }
        }
    }
    s->sc[0] = D->scx0;
    s->sc[1] = D->scy0;
    s->sc[2] = D->scx1 + 1;
    s->sc[3] = D->scy1 + 1;
    if (s->sc[2] > tg[s->ct].w) {
        s->sc[2] = tg[s->ct].w;
    }
    if (s->sc[3] > 2048) {
        s->sc[3] = 2048;
    }

    /* blending: Cs * (C * as + ds) + Cd * (C * ad + dd) */
    if (D->abe) {
        as = (D->ba == 0) - (D->bb == 0);
        ds = D->bd == 0;
        ad = (D->ba == 1) - (D->bb == 1);
        dd = D->bd == 1;
        if (D->ba == D->bb) {
            as = ad = 0;
        }
        ns = as == -1 && ds == 0;
        nd = ad == -1 && dd == 0;
        if (!(as == 0 && ds == 1 && ad == 0 && dd == 0) || D->pabe) {
            s->blend = 1;
            s->eq = (ns && !nd) ? GL_FUNC_REVERSE_SUBTRACT : (!ns && nd) ? GL_FUNC_SUBTRACT : GL_FUNC_ADD;
            s->bm[0] = 1;
            s->bm[1] = D->bc == 2 ? 2 : 0;
            s->bm[2] = s->eq == GL_FUNC_REVERSE_SUBTRACT ? 2 : s->eq == GL_FUNC_SUBTRACT ? 1 : 0;
            s->bm[3] = D->pabe;
            s->bk[0] = (float)as;
            s->bk[1] = (float)ds;
            s->bk[2] = (float)ad;
            s->bk[3] = (float)dd;
            if (ns && nd) {
                s->bk[0] = s->bk[1] = s->bk[2] = s->bk[3] = 0;
            }
            s->fix = (float)D->fix / 128.0f;
            if (D->bc == 1 && !warned_ad) {
                warned_ad = 1;
                grx_log("gs-gl: blending by destination alpha isn't done yet (source alpha instead)");
            }
        }
    }
    s->tf[0] = D->tme;
    s->tf[1] = D->tfx;
    s->tf[2] = D->tcc;
    s->tf[3] = D->fst;
    s->tw[0] = D->t.wms;
    s->tw[1] = D->t.wmt;
    s->tw[2] = D->t.mag_linear;
    if (D->tme && D->needs_lod && D->t.lcm) {
        s->tw[2] = D->t.k > 0.0f ? D->t.min_linear : D->t.mag_linear;
    }
    s->tw[3] = D->iip;
    s->reg[0] = D->t.minu;
    s->reg[1] = D->t.maxu;
    s->reg[2] = D->t.minv;
    s->reg[3] = D->t.maxv;

    /* alpha test and what a failing pixel still writes */
    s->at[0] = D->atst;
    s->at[1] = D->aref;
    s->at[2] = 0;
    s->at[3] = D->fba;
    if (D->atst == 1) {
        s->at[2] = 2;
    } else {
        int zw = s->dt >= 0 && s->zwrite;
        switch (D->afail) {
        case 0:
            break;
        case 1: /* FB_ONLY */
            if (!zw) {
                s->at[2] = 2;
            } else if (D->atst == 0) {
                s->at[2] = 2;
                s->zwrite = 0;
            } else {
                s->pass2 = 1;
                s->cmask2 = s->cmask;
                s->zwrite2 = 0;
            }
            break;
        case 2: /* ZB_ONLY */
            if (D->atst == 0) {
                s->at[2] = 2;
                s->cmask = 0;
            } else {
                s->pass2 = 1;
                s->cmask2 = 0;
                s->zwrite2 = s->zwrite;
            }
            break;
        default: /* RGB_ONLY */
            if (D->atst == 0) {
                s->at[2] = 2;
                s->cmask &= 7;
                s->zwrite = 0;
            } else {
                s->pass2 = 1;
                s->cmask2 = s->cmask & 7;
                s->zwrite2 = 0;
            }
            break;
        }
    }
    s->fog[0] = (float)D->fge;
    s->fog[1] = (float)(D->fogcol & 255);
    s->fog[2] = (float)((D->fogcol >> 8) & 255);
    s->fog[3] = (float)((D->fogcol >> 16) & 255);
    s->misc[0] = D->date && glTextureBarrier != NULL;
    s->misc[1] = D->datm;
    s->misc[2] = D->f16;
    s->misc[3] = D->dthe;
    if (D->date && glTextureBarrier == NULL && !warned_date) {
        warned_date = 1;
        grx_log("gs-gl: no texture barrier in this GL: the destination alpha test is off");
    }
    for (i = 0; i < 16; i++) {
        s->dimx[i] = D->dimx[i >> 2][i & 3];
    }
}

/* Texel coordinates a primitive reads (bounding box, before wrapping);
   0 if unknown */
static int prim_texels(const GsDraw *D, const GsVertex *v, int nv, int *u0, int *v0, int *u1, int *v1) {
    double ul = 1e30, uh = -1e30, vl = 1e30, vh = -1e30;
    int i;
    for (i = 0; i < nv; i++) {
        double u, w;
        if (D->fst) {
            u = v[i].u / 16.0;
            w = v[i].v / 16.0;
        } else {
            double q = v[i].q;
            if (!(q > 0.0) && !(q < 0.0)) {
                return 0;
            }
            if ((q > 0.0) != (v[0].q > 0.0)) {
                return 0;
            }
            u = (double)v[i].s / q * (double)(1 << D->t.tw);
            w = (double)v[i].t / q * (double)(1 << D->t.th);
        }
        if (u < ul) ul = u;
        if (u > uh) uh = u;
        if (w < vl) vl = w;
        if (w > vh) vh = w;
    }
    if (!(ul > -4096 && uh < 4096 && vl > -4096 && vh < 4096)) {
        return 0;
    }
    *u0 = (int)floor(ul - 1);
    *u1 = (int)ceil(uh + 1);
    *v0 = (int)floor(vl - 1);
    *v1 = (int)ceil(vh + 1);
    return 1;
}

/* The texture for the batch: a target's buffer, or decoded from memory */
static void setup_texture(const GsVertex *v, int nv) {
    const GsDraw *D = &gd;
    St *s = &st;
    int i, u0, v0, u1, v1;

    rt_tex = 0;
    s->tsz[0] = (float)(1 << D->t.tw);
    s->tsz[1] = (float)(1 << D->t.th);
    if (D->t.kind == GS_K32 && !no_rt && prim_texels(D, v, nv, &u0, &v0, &u1, &v1)) {
        /* A colour target with this buffer, at whole pages from its start */
        for (i = 0; i < NT; i++) {
            const Tgt *g = &tg[i];
            int k, dx, dy, ch, x0, y0, x1, y1, r, c, owned = 0;
            if (!g->used || g->depth || g->lay != (int)D->t.layout || g->bw != D->t.tbw || D->t.tbp < g->bp ||
                ((D->t.tbp - g->bp) & 31)) {
                continue;
            }
            ch = lay_cell_h(g->lay);
            k = (int)((D->t.tbp - g->bp) >> 5);
            dx = (k % (int)g->bw) * 64;
            dy = (k / (int)g->bw) * ch;
            /* the texels, wrapped as the clamp modes allow */
            if (D->t.wms == 1 || D->t.wms == 2 || (D->t.wms == 0 && u0 >= 0 && u1 <= (1 << D->t.tw))) {
                x0 = u0 < 0 ? 0 : u0;
                x1 = u1 > (1 << D->t.tw) ? (1 << D->t.tw) : u1;
            } else {
                x0 = 0;
                x1 = 1 << D->t.tw;
            }
            if (D->t.wmt == 1 || D->t.wmt == 2 || (D->t.wmt == 0 && v0 >= 0 && v1 <= (1 << D->t.th))) {
                y0 = v0 < 0 ? 0 : v0;
                y1 = v1 > (1 << D->t.th) ? (1 << D->t.th) : v1;
            } else {
                y0 = 0;
                y1 = 1 << D->t.th;
            }
            x0 += dx;
            x1 += dx;
            y0 += dy;
            y1 += dy;
            if (x1 > g->w) x1 = g->w;
            if (y1 > g->h) y1 = g->h;
            if (x0 >= x1 || y0 >= y1) {
                continue;
            }
            for (r = y0 / ch; r <= (y1 - 1) / ch && !owned; r++) {
                for (c = x0 / 64; c <= (x1 - 1) / 64; c++) {
                    if (owner[tgt_page(g, r * (int)g->bw + c)] == i + 1) {
                        owned = 1;
                        break;
                    }
                }
            }
            if (!owned) {
                continue;
            }
            if (trace)
                grx_log("trace: texture from target %d (draw target %d) %d,%d-%d,%d", i, s->ct, x0, y0, x1, y1);
            take_rect(i, x0, y0, x1, y1, 0);
            s->tex = g->tex;
            s->texw = g->w * sx;
            s->texh = g->h * sy;
            if (i == s->ct) {
                /* The frame being drawn: sample a copy */
                if (copy_w < s->texw || copy_h < s->texh) {
                    if (copy_tex) {
                        glDeleteFramebuffers(1, &copy_fbo);
                        glDeleteTextures(1, &copy_tex);
                    }
                    copy_w = s->texw;
                    copy_h = s->texh;
                    copy_tex = new_tex(GL_RGBA8, copy_w, copy_h, GL_RGBA, GL_UNSIGNED_BYTE);
                    copy_fbo = new_fbo(copy_tex, 0);
                }
                glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
                bind_tex(0, copy_tex);
                glCopyTexSubImage2D(GL_TEXTURE_2D, 0, x0 * sx, y0 * sy, x0 * sx, y0 * sy,
                                    (x1 - x0) * sx, (y1 - y0) * sy);
                s->tex = copy_tex;
                s->texw = copy_w;
                s->texh = copy_h;
                nstat[S_SELFCOPY]++;
            }
            s->tsz[2] = 2.0f; /* the target's scale (u_scale) */
            s->tsz[3] = 1.0f;
            s->toff[0] = (float)dx;
            s->toff[1] = (float)dy;
            s->toff[2] = 1.0f / (float)s->texw;
            s->toff[3] = 1.0f / (float)s->texh;
            s->samp = 4;
            rt_tex = i + 1;
            return;
        }
    }
    s->tex = texture_from_memory(D);
    s->texw = 1 << D->t.tw;
    s->texh = 1 << D->t.th;
    s->tsz[2] = tex_is_rep ? -1.0f : 1.0f; /* -1: a replacement, sampled by normalised coordinates */
    s->tsz[3] = 0.0f;
    s->toff[0] = s->toff[1] = 0.0f;
    s->toff[2] = 1.0f / (float)s->texw;
    s->toff[3] = 1.0f / (float)s->texh;
    s->samp = tex_is_rep ? 5 : (D->t.wms == 0 ? 0 : 1) | (D->t.wmt == 0 ? 0 : 2);
}

static const float no_clamp[4] = {-1.0e7f, -1.0e7f, 1.0e7f, 1.0e7f};

static inline void vtx(V *o, const GsVertex *g, int32_t x, int32_t y, float s, float t, float q, const GsVertex *col) {
    memcpy(o->cl, no_clamp, sizeof(o->cl));
    o->x = (float)x / 16.0f;
    o->y = (float)y / 16.0f;
    o->z = col->z;
    memcpy(o->c, &col->rgba, 4);
    o->s = s;
    o->t = t;
    o->q = q;
    o->f = (float)col->f;
    (void)g;
}

static inline void stq(const GsVertex *g, float *s, float *t, float *q) {
    if (gd.fst) {
        *s = (float)g->u / 16.0f;
        *t = (float)g->v / 16.0f;
        *q = 1.0f;
    } else {
        *s = g->s;
        *t = g->t;
        *q = g->q;
    }
}

static V *vspace(int n) {
    if (nvb + n > vbcap) {
        if (nvb + n > (int)(VBO_SIZE / sizeof(V)) / 2) {
            flush();
        }
        if (nvb + n > vbcap) {
            vbcap = vbcap ? vbcap * 2 : 65536;
            vb = realloc(vb, (size_t)vbcap * sizeof(V));
            if (vb == NULL) {
                grx_fatal("gs-gl: out of memory");
            }
        }
    }
    nvb += n;
    return vb + nvb - n;
}

/* A rectangle of GS window coordinates (12.4) as two triangles */
static V *quad(int32_t x0, int32_t y0, int32_t x1, int32_t y1, float s0, float t0, float s1, float t1, float q,
              const GsVertex *col) {
    V *o = vspace(6);
    vtx(&o[0], col, x0, y0, s0, t0, q, col);
    vtx(&o[1], col, x1, y0, s1, t0, q, col);
    vtx(&o[2], col, x0, y1, s0, t1, q, col);
    vtx(&o[3], col, x1, y0, s1, t0, q, col);
    vtx(&o[4], col, x1, y1, s1, t1, q, col);
    vtx(&o[5], col, x0, y1, s0, t1, q, col);
    return o;
}

/* A sprite's texel coordinates (texels) at its first and last pixels
   along one axis, as the GS samples them: upscaled, the samples between
   the last pixel's and the edge would reach texels the GS never reads
   (the next strip of a texture, past its edge), which shows as seams */
static void sprite_range(int32_t p0, int32_t p1, float c0, float c1, float *lo, float *hi) {
    int32_t a = p0 < p1 ? p0 : p1, b = p0 < p1 ? p1 : p0;
    int first = (a + 15) >> 4, last = ((b + 15) >> 4) - 1;
    float u0, u1;
    if (p0 == p1 || last < first) {
        return;
    }
    u0 = c0 + (c1 - c0) * (float)(first * 16 - p0) / (float)(p1 - p0);
    u1 = c0 + (c1 - c0) * (float)(last * 16 - p0) / (float)(p1 - p0);
    *lo = u0 < u1 ? u0 : u1;
    *hi = u0 < u1 ? u1 : u0;
}

/* The same for a triangle that is half of a screen-aligned rectangle (two
   edges along the axes: 2D quads drawn as strips) with texel coordinates
   linear on the screen: their range over the native samples of its
   bounding box. Other triangles (3D) aren't clamped. */
static int tri_clamp(const GsVertex *v, float *cl) {
    float u[3], w[3], q;
    double ex1, ey1, ex2, ey2, det, gux, guy, gvx, gvy;
    int i, hor = 0, ver = 0, x0, x1, y0, y1;
    int32_t mnx = v[0].x, mxx = v[0].x, mny = v[0].y, mxy = v[0].y;
    for (i = 0; i < 3; i++) {
        const GsVertex *a = &v[i], *b = &v[(i + 1) % 3];
        hor |= a->y == b->y && a->x != b->x;
        ver |= a->x == b->x && a->y != b->y;
        if (v[i].x < mnx) mnx = v[i].x;
        if (v[i].x > mxx) mxx = v[i].x;
        if (v[i].y < mny) mny = v[i].y;
        if (v[i].y > mxy) mxy = v[i].y;
    }
    if (!hor || !ver) {
        return 0;
    }
    for (i = 0; i < 3; i++) {
        if (gd.fst) {
            u[i] = (float)v[i].u / 16.0f;
            w[i] = (float)v[i].v / 16.0f;
        } else {
            q = v[i].q;
            if (q != v[0].q || q == 0.0f) {
                return 0;
            }
            u[i] = v[i].s / q * (float)(1 << gd.t.tw);
            w[i] = v[i].t / q * (float)(1 << gd.t.th);
        }
    }
    ex1 = (v[1].x - v[0].x) / 16.0;
    ey1 = (v[1].y - v[0].y) / 16.0;
    ex2 = (v[2].x - v[0].x) / 16.0;
    ey2 = (v[2].y - v[0].y) / 16.0;
    det = ex1 * ey2 - ex2 * ey1;
    if (det == 0.0) {
        return 0;
    }
    gux = ((u[1] - u[0]) * ey2 - (u[2] - u[0]) * ey1) / det;
    guy = (ex1 * (u[2] - u[0]) - ex2 * (u[1] - u[0])) / det;
    gvx = ((w[1] - w[0]) * ey2 - (w[2] - w[0]) * ey1) / det;
    gvy = (ex1 * (w[2] - w[0]) - ex2 * (w[1] - w[0])) / det;
    x0 = (mnx + 15) >> 4;
    x1 = ((mxx + 15) >> 4) - 1;
    y0 = (mny + 15) >> 4;
    y1 = ((mxy + 15) >> 4) - 1;
    if (x1 < x0 || y1 < y0) {
        return 0;
    }
    cl[0] = cl[1] = 1.0e7f;
    cl[2] = cl[3] = -1.0e7f;
    for (i = 0; i < 4; i++) {
        double px = ((i & 1) ? x1 : x0) - v[0].x / 16.0, py = ((i & 2) ? y1 : y0) - v[0].y / 16.0;
        float cu = (float)(u[0] + gux * px + guy * py), cv = (float)(w[0] + gvx * px + gvy * py);
        if (cu < cl[0]) cl[0] = cu;
        if (cu > cl[2]) cl[2] = cu;
        if (cv < cl[1]) cl[1] = cv;
        if (cv > cl[3]) cl[3] = cv;
    }
    return 1;
}

static void gl_prim(const GsVertex *v, uint32_t type) {
    static const int nvs[8] = {1, 2, 2, 3, 3, 3, 2, 0};
    int nv = nvs[type & 7], i, x0, y0, x1, y1;
    int32_t mnx, mxx, mny, mxy;

    if (nv == 0) {
        return;
    }
    if (gs.dirty || !gd_valid) {
        static GsDraw nd;
        memset(&nd, 0, sizeof(nd));
        gs_setup(&nd);
        gs.dirty = 0;
        if (!gd_valid || memcmp(&nd, &gd, sizeof(nd)) != 0) {
            flush();
            gd = nd;
            gd_valid = 1;
            st_ready = 0;
        }
    }
    if (need_setup) {
        flush();
        st_ready = 0;
        need_setup = 0;
    }
    if (!st_ready) {
        setup_state();
        if (trace)
            grx_log("trace: state fbp %x zbp %x z %d ct %d dt %d tme %d tbp %x psm %x %dx%d", gd.fbp, gd.zbp, gd.zon,
                    st.ct, st.dt, gd.tme, gd.t.tbp, gd.t.psm, 1 << gd.t.tw, 1 << gd.t.th);
        if (!st_skip && gd.tme) {
            setup_texture(v, nv);
        }
        st_ready = 1;
    }
    if (st_skip) {
        return;
    }
    nstat[S_PRIMS]++;

    /* the pixels it can touch */
    mnx = mxx = v[0].x;
    mny = mxy = v[0].y;
    for (i = 1; i < nv; i++) {
        if (v[i].x < mnx) mnx = v[i].x;
        if (v[i].x > mxx) mxx = v[i].x;
        if (v[i].y < mny) mny = v[i].y;
        if (v[i].y > mxy) mxy = v[i].y;
    }
    /* (pixels are in when their sample point is left of and above the
       right and bottom edges; lines and points round to the nearest) */
    x0 = mnx >> 4;
    x1 = (mxx + 15) >> 4;
    y0 = mny >> 4;
    y1 = (mxy + 15) >> 4;
    if (type < 3) {
        x1++;
        y1++;
    }
    if (x0 < st.sc[0]) x0 = st.sc[0];
    if (y0 < st.sc[1]) y0 = st.sc[1];
    if (x1 > st.sc[2]) x1 = st.sc[2];
    if (y1 > st.sc[3]) y1 = st.sc[3];
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    /* A texture from a target: the pages this one reads are the target's too */
    if (rt_tex && rt_tex - 1 != st.ct) {
        int u0, v0, u1, v1;
        if (prim_texels(&gd, v, nv, &u0, &v0, &u1, &v1)) {
            const Tgt *g = &tg[rt_tex - 1];
            int dx = (int)st.toff[0], dy = (int)st.toff[1];
            if (gd.t.wms == 0 && (u0 < 0 || u1 > (1 << gd.t.tw))) {
                u0 = 0;
                u1 = 1 << gd.t.tw;
            }
            if (gd.t.wmt == 0 && (v0 < 0 || v1 > (1 << gd.t.th))) {
                v0 = 0;
                v1 = 1 << gd.t.th;
            }
            /* (only within the target: what is past it isn't drawn there) */
            take_rect(rt_tex - 1, u0 + dx, v0 + dy, u1 + dx > g->w ? g->w : u1 + dx,
                      v1 + dy > g->h ? g->h : v1 + dy, 0);
        }
    }
    {
        /* An opaque sprite overwrites what it covers unconditionally */
        int cov[4], *cc = NULL, *cz = NULL;
        if (type == 6) {
            cov[0] = (v[0].x < v[1].x ? v[0].x : v[1].x) + 15;
            cov[2] = (v[0].x < v[1].x ? v[1].x : v[0].x) + 15;
            cov[1] = (v[0].y < v[1].y ? v[0].y : v[1].y) + 15;
            cov[3] = (v[0].y < v[1].y ? v[1].y : v[0].y) + 15;
            cov[0] = cov[0] >> 4 > st.sc[0] ? cov[0] >> 4 : st.sc[0];
            cov[1] = cov[1] >> 4 > st.sc[1] ? cov[1] >> 4 : st.sc[1];
            cov[2] = cov[2] >> 4 < st.sc[2] ? cov[2] >> 4 : st.sc[2];
            cov[3] = cov[3] >> 4 < st.sc[3] ? cov[3] >> 4 : st.sc[3];
            if (st.at[2] == 2 && (st.dt < 0 || st.zfunc == GL_ALWAYS)) {
                if (!st.blend && !st.misc[0] && st.cmask == 15 && gd.fbmsk == 0 && rt_tex - 1 != st.ct) {
                    cc = cov;
                }
                if (st.dt >= 0 && st.zwrite) {
                    cz = cov;
                }
            }
        }
        take_rect_cov(st.ct, x0, y0, x1, y1, st.cmask != 0 || (st.pass2 && st.cmask2 != 0), cc);
        if (st.dt >= 0) {
            take_rect_cov(st.dt, x0, y0, x1, y1, st.zwrite || (st.pass2 && st.zwrite2), cz);
        }
    }

    switch (type) {
    case 6: { /* sprite: the second vertex's colour, Z, fog and Q */
        float s0, t0, q0, s1, t1, q1;
        V *o;
        int32_t px0 = v[0].x, py0 = v[0].y, px1 = v[1].x, py1 = v[1].y;
        float ss0, tt0, ss1, tt1;
        stq(&v[0], &s0, &t0, &q0);
        stq(&v[1], &s1, &t1, &q1);
        ss0 = s0;
        ss1 = s1;
        tt0 = t0;
        tt1 = t1;
        if (sx > 1 || sy > 1) {
            /* Upscaled, the edges go to the native pixel boundaries: the
               sprite covers the same GS pixels whole, so sprites that meet
               between two GS samples (one ending at 111.5, the next starting
               at 112) leave no gap; the texture keeps its place. Squeezed for
               widescreen, the boundaries are those of where the game drew
               it, squeezed in turn: at about a texel per pixel or fewer, the
               squeezed sprite's own would cut off its first texel column (a
               letter's left edge) */
            int32_t ox0 = gs_ws_squeezed ? gs.vq[0].x : px0, ox1 = gs_ws_squeezed ? gs.vq[1].x : px1;
            int32_t nx0 = ((ox0 + 15) >> 4) << 4, nx1 = ((ox1 + 15) >> 4) << 4;
            int32_t ny0 = ((py0 + 15) >> 4) << 4, ny1 = ((py1 + 15) >> 4) << 4;
            if (ox1 != ox0) {
                ss0 = s0 + (s1 - s0) * (float)(nx0 - ox0) / (float)(ox1 - ox0);
                ss1 = s0 + (s1 - s0) * (float)(nx1 - ox0) / (float)(ox1 - ox0);
            }
            if (gs_ws_squeezed) {
                /* as gs_widescreen.c squeezes */
                nx0 = 320 * 16 + (int32_t)lrintf((float)(nx0 - 320 * 16) * gs_ws_k);
                nx1 = 320 * 16 + (int32_t)lrintf((float)(nx1 - 320 * 16) * gs_ws_k);
            }
            if (py1 != py0) {
                tt0 = t0 + (t1 - t0) * (float)(ny0 - py0) / (float)(py1 - py0);
                tt1 = t0 + (t1 - t0) * (float)(ny1 - py0) / (float)(py1 - py0);
            }
            px0 = nx0;
            px1 = nx1;
            py0 = ny0;
            py1 = ny1;
        }
        o = quad(px0, py0, px1, py1, ss0, tt0, ss1, tt1, q1, &v[1]);
        if (gd.tme && (sx > 1 || sy > 1)) {
            float cl[4], ku = 1.0f, kv = 1.0f;
            memcpy(cl, no_clamp, sizeof(cl));
            if (!gd.fst) {
                ku = (float)(1 << gd.t.tw) / q1;
                kv = (float)(1 << gd.t.th) / q1;
            }
            /* Squeezed for widescreen, the GS would still have sampled it
               where the game drew it: about a texel per pixel or fewer
               squeezed, the first sample falls past the first texel column,
               and a letter would lose its left edge */
            sprite_range(gs_ws_squeezed ? gs.vq[0].x : v[0].x, gs_ws_squeezed ? gs.vq[1].x : v[1].x, s0 * ku, s1 * ku,
                         &cl[0], &cl[2]);
            sprite_range(v[0].y, v[1].y, t0 * kv, t1 * kv, &cl[1], &cl[3]);
            for (i = 0; i < 6; i++) {
                memcpy(o[i].cl, cl, sizeof(cl));
            }
            if (trace > 1)
                grx_log("trace: sprite %d,%d-%d,%d st %g,%g-%g,%g q %g fst %d clamp %g,%g-%g,%g", v[0].x, v[0].y, v[1].x,
                        v[1].y, s0, t0, s1, t1, q1, gd.fst, cl[0], cl[1], cl[2], cl[3]);
        }
        break;
    }
    case 3: case 4: case 5: {
        V *o = vspace(3);
        for (i = 0; i < 3; i++) {
            float s, t, q;
            stq(&v[i], &s, &t, &q);
            vtx(&o[i], &v[i], v[i].x, v[i].y, s, t, q, &v[i]);
            if (!gd.iip) {
                memcpy(o[i].c, &v[2].rgba, 4);
            }
        }
        if (gd.tme && (sx > 1 || sy > 1)) {
            float cl[4];
            if (tri_clamp(gs_ws_squeezed ? gs.vq : v, cl)) { /* as for sprites */
                for (i = 0; i < 3; i++) {
                    memcpy(o[i].cl, cl, sizeof(cl));
                }
            }
        }
        break;
    }
    case 0: { /* point: the pixel it rounds to */
        int32_t px = ((v[0].x + 8) >> 4) << 4, py = ((v[0].y + 8) >> 4) << 4;
        float s, t, q;
        stq(&v[0], &s, &t, &q);
        quad(px, py, px + 16, py + 16, s, t, s, t, q, &v[0]);
        break;
    }
    default: { /* line: a quad a pixel wide across its major axis */
        float s0, t0, q0, s1, t1, q1;
        int32_t dx = v[1].x - v[0].x, dy = v[1].y - v[0].y;
        V *o = vspace(6);
        const GsVertex *c1 = gd.iip ? &v[1] : &v[1];
        int32_t ox = 0, oy = 0;
        stq(&v[0], &s0, &t0, &q0);
        stq(&v[1], &s1, &t1, &q1);
        if ((dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy)) {
            oy = 8;
        } else {
            ox = 8;
        }
        vtx(&o[0], &v[0], v[0].x - ox, v[0].y - oy, s0, t0, q0, gd.iip ? &v[0] : c1);
        vtx(&o[1], &v[1], v[1].x - ox, v[1].y - oy, s1, t1, q1, c1);
        vtx(&o[2], &v[0], v[0].x + ox, v[0].y + oy, s0, t0, q0, gd.iip ? &v[0] : c1);
        vtx(&o[3], &v[1], v[1].x - ox, v[1].y - oy, s1, t1, q1, c1);
        vtx(&o[4], &v[1], v[1].x + ox, v[1].y + oy, s1, t1, q1, c1);
        vtx(&o[5], &v[0], v[0].x + ox, v[0].y + oy, s0, t0, q0, gd.iip ? &v[0] : c1);
        break;
    }
    }
}

/* ---------------------------------------------------------------------- */
/* Memory accesses from outside the draws */

static void gl_writing(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    static uint8_t partial[512];
    GsPages pg;
    const GsPsmInfo *pi = &gs_psm[psm & 63];
    int p, any = 0, full_words, pw, ph, bwp;

    flush();
    need_setup = 1;
    if (trace)
        grx_log("trace: write psm %x bp %x bw %u %u,%u %ux%u", psm, bp, bw, x, y, w, h);
    memset(&pg, 0, sizeof(pg));
    gs_pages_rect(&pg, psm, bp, bw, x, y, w, h);
    /* Pages the transfer writes all of need nothing kept; pages it writes
       part of must be current in gs_vram first */
    full_words = pi->kind == GS_K32 || pi->kind == GS_K16 || pi->kind == GS_K8 || pi->kind == GS_K4;
    switch (pi->layout) {
    case GS_L32: case GS_L32Z: pw = 64; ph = 32; bwp = (int)bw; break;
    case GS_L8: pw = 128; ph = 64; bwp = (int)gs_bw8(bw); break;
    case GS_L4: pw = 128; ph = 128; bwp = (int)gs_bw8(bw); break;
    default: pw = 64; ph = 64; bwp = (int)bw; break;
    }
    memset(partial, 0, sizeof(partial));
    for (p = 0; p < 512; p++) {
        int full = 0;
        if (!((pg.w[p >> 5] >> (p & 31)) & 1) || cpu_ok[p]) {
            continue;
        }
        if (full_words && (bp & 31) == 0 && bwp > 0 && x + w <= 2048 && y + h <= 2048) {
            int rel = (p - (int)(bp >> 5)) & 511, cx = rel % bwp, cy = rel / bwp;
            full = (int)x <= cx * pw && (int)(x + w) >= (cx + 1) * pw && (int)y <= cy * ph &&
                   (int)(y + h) >= (cy + 1) * ph && cx < bwp;
        }
        if (!full) {
            partial[p] = 1;
            any = 1;
        } else {
            /* gs_vram gets all of it: whatever held it before is out of date */
            owner[p] = 0;
            cpu_ok[p] = 1;
            page_written(p);
        }
    }
    if (any) {
        /* The rest of those pages' targets too, in the same read-back: a
           picture sent in many small transfers (the movie player's 16x16
           blocks into the frame the field drew) would otherwise wait for
           the GPU once per transfer, 120 times on its first frame */
        static uint8_t tgt[256];
        memset(tgt, 0, sizeof(tgt));
        for (p = 0; p < 512; p++) {
            if (partial[p] && owner[p]) {
                tgt[owner[p]] = 1;
            }
        }
        for (p = 0; p < 512; p++) {
            if (owner[p] && tgt[owner[p]] && !cpu_ok[p]) {
                partial[p] = 1;
            }
        }
        read_back(partial);
    }
}

static void gl_wrote(const GsPages *pg) {
    int i;
    need_setup = 1;
    disp_dirty = 1;
    for (i = 0; i < 16; i++) {
        uint32_t m = pg->w[i];
        while (m) {
            int p = i * 32 + __builtin_ctz(m);
            m &= m - 1;
            owner[p] = 0;
            cpu_ok[p] = 1;
            page_written(p);
        }
    }
}

static void gl_reading(const GsPages *pg) {
    static uint8_t want[512];
    pages_to_want(pg, want);
    read_back(want);
}

static void gl_loaded(void) {
    int p;
    flush();
    for (p = 0; p < 512; p++) {
        owner[p] = 0;
        cpu_ok[p] = 1;
        gpu_ok[p] = 0;
        ver[p] = ++gen;
        ver_read[p] = 0;
    }
    need_setup = 1;
    disp_dirty = 1;
}

/* ---------------------------------------------------------------------- */
/* The display */

#define NOUT 4
static GLuint out_tex[NOUT], out_fbo[NOUT];
static int out_w[NOUT], out_h[NOUT], out_cur = -1;
static uint64_t last_regs[5];
static int have_pic;
static int pic_new; /* composed, not submitted yet */

typedef struct Circ {
    int on;
    uint32_t bp, bw, psm, dbx, dby;
    int dx, dy, w, h;
} Circ;

static void circ(Circ *c, int on, uint64_t dispfb, uint64_t display) {
    uint32_t magh = GS_FIELD(display, 23, 4) + 1, magv = GS_FIELD(display, 27, 2) + 1;
    c->on = on;
    c->bp = GS_FIELD(dispfb, 0, 9) * 32;
    c->bw = GS_FIELD(dispfb, 9, 6);
    c->psm = GS_FIELD(dispfb, 15, 5);
    c->dbx = GS_FIELD(dispfb, 32, 11);
    c->dby = GS_FIELD(dispfb, 43, 11);
    c->dx = (int)(GS_FIELD(display, 0, 12) / magh);
    c->dy = (int)(GS_FIELD(display, 12, 11) / magv);
    c->w = (int)((GS_FIELD(display, 32, 12) + 1) / magh);
    c->h = (int)((GS_FIELD(display, 44, 11) + 1) / magv);
}

/* A circuit's buffer as a target, its pixels current there */
static int circ_target(const Circ *c) {
    int lay = c->psm == GS_PSMCT16 ? GS_L16 : c->psm == GS_PSMCT16S ? GS_L16S : GS_L32, t;
    if (c->bw == 0) {
        return -1;
    }
    t = find_target(c->bp, c->bw, 0, lay, (int)c->dby + c->h);
    take_rect(t, (int)c->dbx, (int)c->dby, (int)c->dbx + c->w, (int)c->dby + c->h, 0);
    return t;
}

/* Draw the picture the GS displays into the next output texture */
static int compose(void) {
    uint64_t pmode = gs_priv_read(GS_PRIV_PMODE);
    int en1 = GS_FIELD(pmode, 0, 1), en2 = GS_FIELD(pmode, 1, 1);
    int mmod = GS_FIELD(pmode, 5, 1), slbg = GS_FIELD(pmode, 7, 1), alp = GS_FIELD(pmode, 8, 8);
    uint32_t bgc = (uint32_t)gs_priv_read(GS_PRIV_BGCOLOR) & 0xFFFFFF;
    Circ c1, c2;
    int x0, y0, x1, y1, w, h, t1 = -1, t2 = -1, k, i;

    circ(&c1, en1, gs_priv_read(GS_PRIV_DISPFB1), gs_priv_read(GS_PRIV_DISPLAY1));
    circ(&c2, en2, gs_priv_read(GS_PRIV_DISPFB2), gs_priv_read(GS_PRIV_DISPLAY2));
    if (!en1 && !en2) {
        return 0;
    }
    if (en1 && en2) {
        x0 = c1.dx < c2.dx ? c1.dx : c2.dx;
        y0 = c1.dy < c2.dy ? c1.dy : c2.dy;
        x1 = c1.dx + c1.w > c2.dx + c2.w ? c1.dx + c1.w : c2.dx + c2.w;
        y1 = c1.dy + c1.h > c2.dy + c2.h ? c1.dy + c1.h : c2.dy + c2.h;
    } else {
        const Circ *c = en1 ? &c1 : &c2;
        x0 = c->dx;
        y0 = c->dy;
        x1 = c->dx + c->w;
        y1 = c->dy + c->h;
    }
    w = x1 - x0;
    h = y1 - y0;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        return 0;
    }
    flush();
    if (en1) {
        t1 = circ_target(&c1);
    }
    if (trace)
        grx_log("trace: display pmode %llx 1: bp %x bw %u psm %x at %u,%u %dx%d at %d,%d: target %d; 2: bp %x bw %u psm %x at %u,%u %dx%d at %d,%d", (unsigned long long)pmode, c1.bp, c1.bw, c1.psm, c1.dbx, c1.dby, c1.w, c1.h, c1.dx, c1.dy, t1, c2.bp, c2.bw, c2.psm, c2.dbx, c2.dby, c2.w, c2.h, c2.dx, c2.dy);
    if (en2 && !slbg) {
        tgt_pin = t1; /* (nor the second circuit's the first's) */
        t2 = circ_target(&c2);
        tgt_pin = -1;
    }
    /* the next output texture the presenter is done with */
    k = -1;
    for (i = 1; i <= NOUT && k < 0; i++) {
        int j = (out_cur + i + NOUT) % NOUT;
        if (out_tex[j] == 0 || grx_video_gl_texture_free(out_tex[j])) {
            k = j;
        }
    }
    if (k < 0) {
        k = (out_cur + 1) % NOUT;
    }
    if (out_tex[k] == 0 || out_w[k] != w * sx || out_h[k] != h * sy) {
        if (out_tex[k]) {
            glDeleteFramebuffers(1, &out_fbo[k]);
            glDeleteTextures(1, &out_tex[k]);
        }
        out_tex[k] = new_tex(GL_RGBA8, w * sx, h * sy, GL_RGBA, GL_UNSIGNED_BYTE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        out_fbo[k] = new_fbo(out_tex[k], 0);
        out_w[k] = w * sx;
        out_h[k] = h * sy;
    }
    plain_state();
    glBindFramebuffer(GL_FRAMEBUFFER, out_fbo[k]);
    glViewport(0, 0, w * sx, h * sy);
    glUseProgram(p_compose.p);
    glBindVertexArray(empty_vao);
    bind_tex(0, t1 >= 0 ? tg[t1].tex : 0);
    glBindSampler(0, 0);
    bind_tex(1, t2 >= 0 ? tg[t2].tex : 0);
    glBindSampler(1, 0);
    glUniform4i(p_compose.r1, (c1.dx - x0) * sx, (c1.dy - y0) * sy, (c1.dx - x0 + c1.w) * sx,
                (c1.dy - y0 + c1.h) * sy);
    glUniform4i(p_compose.r2, (c2.dx - x0) * sx, (c2.dy - y0) * sy, (c2.dx - x0 + c2.w) * sx,
                (c2.dy - y0 + c2.h) * sy);
    glUniform4i(p_compose.o1, (int)c1.dbx * sx, (int)c1.dby * sy, c1.psm == GS_PSMCT24, t1 >= 0);
    glUniform4i(p_compose.o2, (int)c2.dbx * sx, (int)c2.dby * sy, c2.psm == GS_PSMCT24, t2 >= 0);
    glUniform4i(p_compose.pm, mmod, alp, (GLint)bgc, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    out_cur = k;
    have_pic = 1;
    pic_new = 1;
    nstat[S_COMPOSES]++;
    return 1;
}

/* The display's registers and whether anything was drawn since */
static int display_changed(void) {
    uint64_t r[5];
    r[0] = gs_priv_read(GS_PRIV_PMODE);
    r[1] = gs_priv_read(GS_PRIV_DISPFB1);
    r[2] = gs_priv_read(GS_PRIV_DISPLAY1);
    r[3] = gs_priv_read(GS_PRIV_DISPFB2);
    r[4] = gs_priv_read(GS_PRIV_DISPLAY2) ^ gs_priv_read(GS_PRIV_BGCOLOR) * 31;
    if (disp_dirty || !have_pic || memcmp(r, last_regs, sizeof(r)) != 0) {
        memcpy(last_regs, r, sizeof(r));
        return 1;
    }
    return 0;
}

static void gl_vsync(void) {
    vsyncs++;
    if (trace)
        grx_log("trace: vsync %u", vsyncs);
    flush();
    if (rescale()) {
        disp_dirty = 1; /* compose the picture again, at the new scale */
    }
    if (display_changed()) {
        disp_dirty = 0;
        if (!compose()) {
            GrxVideoFrame f;
            have_pic = 0;
            pic_new = 0;
            memset(&f, 0, sizeof(f));
            grx_video_gl_submit(&f);
        }
    }
    /* (a picture composed for a snapshot since the last vsync counts) */
    if (pic_new) {
        GrxVideoFrame f;
        memset(&f, 0, sizeof(f));
        f.texture = out_tex[out_cur];
        f.width = out_w[out_cur];
        f.height = out_h[out_cur];
        f.aspect = gs_ws_aspect;
        f.top_down = 1;
        grx_video_gl_submit(&f);
        pic_new = 0;
    }
}

static int gl_display(const uint32_t **pixels, int *width, int *height, int *pitch) {
    static uint32_t *buf;
    static size_t cap;
    size_t n;
    int i, w, h;
    flush();
    if (display_changed()) {
        disp_dirty = 0;
        if (!compose()) {
            have_pic = 0;
        }
    }
    if (!have_pic || out_cur < 0) {
        return 0;
    }
    w = out_w[out_cur];
    h = out_h[out_cur];
    n = (size_t)w * (size_t)h;
    if (n > cap) {
        uint32_t *nb = realloc(buf, n * 4);
        if (nb == NULL) {
            return 0;
        }
        buf = nb;
        cap = n;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, out_fbo[out_cur]);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    /* R, G, B, A bytes to 0xAARRGGBB */
    for (i = 0; i < (int)n; i++) {
        uint32_t c = buf[i];
        buf[i] = 0xFF000000u | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
    }
    *pixels = buf;
    *width = w;
    *height = h;
    *pitch = w;
    return 1;
}

static const GsBackend gl_backend = {gl_prim, gl_writing, gl_wrote, gl_reading, gl_loaded, gl_vsync, gl_display};

/* ---------------------------------------------------------------------- */
/* Start-up */

/* The scale it should draw at. Down: GRX_GS_SCALE (N, or SXxSY for both),
   else the Resolution setting (grx_video_opts.h): the smallest whole
   multiple of the GS's 448 lines that reaches its lines, or for Flexible
   that fills the window's height. Across: the same, times how much wider
   than 4:3 the picture is shown (the frame holds the wide picture
   squeezed, port/docs/widescreen.md) */
static void want_scale(int *wx, int *wy) {
    const char *e = getenv("GRX_GS_SCALE");
    int w = 0, h = 0, a = 0, b = 0, x, y;
    if (e != NULL && sscanf(e, "%dx%d", &a, &b) == 2 && a >= 1 && b >= 1) {
        *wx = a > 16 ? 16 : a;
        *wy = b > 8 ? 8 : b;
        return;
    }
    if (e != NULL && atoi(e) >= 1) {
        y = atoi(e) > 8 ? 8 : atoi(e);
    } else {
        h = grx_opts_resolution_lines(grx_opts_resolution());
        if (h == 0) {
            grx_video_window_size(&w, &h);
        }
        y = h <= 0 ? 1 : (h + 447) / 448;
        y = y < 1 ? 1 : y > 8 ? 8 : y;
    }
    x = (int)lrintf((float)y * gs_ws_aspect * 0.75f);
    *wx = x < y ? y : x > 16 ? 16 : x;
    *wy = y;
}

static void choose_scale(void) {
    want_scale(&sx, &sy);
}

/* At a vsync: draw at another scale when the setting, the window (for
   Flexible: once it has kept its size for half a second, so a window
   being dragged doesn't redraw at each step) or the aspect asks for one.
   Every target goes, its pages back to memory (as find_target evicts
   one), and the targets come again at the new scale as they are drawn
   to. 1 if the scale changed */
static int rescale(void) {
    static int seen_x, seen_y, seen_for;
    static uint8_t want[512];
    int x, y, p, i;
    want_scale(&x, &y);
    if (x == sx && y == sy) {
        seen_for = 0;
        return 0;
    }
    if (x != seen_x || y != seen_y) {
        seen_x = x;
        seen_y = y;
        seen_for = 0;
    }
    if (grx_opts_resolution() == GRX_RES_FLEXIBLE && ++seen_for < 30) {
        return 0;
    }
    seen_for = 0;
    flush();
    for (p = 0; p < 512; p++) {
        want[p] = owner[p] != 0;
    }
    mirror_ready(want);
    for (p = 0; p < 512; p++) {
        if (owner[p] != 0) {
            owner[p] = 0;
            cpu_ok[p] = 0;
        }
    }
    for (i = 0; i < NT; i++) {
        if (tg[i].used) {
            glDeleteFramebuffers(1, &tg[i].fbo);
            glDeleteTextures(1, &tg[i].tex);
            memset(&tg[i], 0, sizeof(tg[i]));
        }
    }
    need_setup = 1;
    grx_log("gs-gl: internal scale %dx%d (%dx%d) from %dx%d", x, y, 640 * x, 448 * y, sx, sy);
    sx = x;
    sy = y;
    return 1;
}

int grx_gs_gl_init(void) {
    void *ctx;
    static uint16_t sw[512][128], inv[6][4096];
    int x, y, z, t, i;
    const char *e;

    ctx = grx_video_gl_create_context();
    if (ctx == NULL) {
        return -1;
    }
    if (grx_gl_load() != 0) {
        return -1;
    }
    gsgl_rep_init();
    if (glTextureBarrier == NULL) {
        grx_log("gs-gl: no GL_ARB_texture_barrier: the destination alpha test will be off");
    }
    clip01 = glClipControl != NULL;
    if (clip01) {
        glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    }
    choose_scale();

    if (!program(gsgl_draw_vs, gsgl_draw_fs, 0, "gs-gl draw", &p_draw) ||
        !program(gsgl_compose_vs, gsgl_decode_fs, 1, "gs-gl decode", &p_decode) ||
        !program(gsgl_quad_vs, gsgl_load_fs, 1, "gs-gl load", &p_load) ||
        !program(gsgl_quad_vs, gsgl_loadz_fs, 1, "gs-gl loadz", &p_loadz) ||
        !program(gsgl_quad_vs, gsgl_resolve_fs, 0, "gs-gl resolve", &p_resolve) ||
        !program(gsgl_compose_vs, gsgl_compose_fs, 0, "gs-gl compose", &p_compose) ||
        !program(gsgl_compose_vs, gsgl_copy_fs, 0, "gs-gl copy", &p_copy)) {
        return -1;
    }

    /* The mirror and the swizzle tables */
    gs_mem_init();
    vram_tex = new_tex(GL_R32UI, 1024, 1024, GL_RED_INTEGER, GL_UNSIGNED_INT);
    vram_fbo = new_fbo(vram_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        grx_log("gs-gl: can't render into an R32UI texture");
        return -1;
    }
    for (z = 0; z < 2; z++)
        for (y = 0; y < 32; y++)
            for (x = 0; x < 64; x++) {
                sw[z * 32 + y][x] = gs_pg32[z][y][x];
                inv[z][gs_pg32[z][y][x]] = (uint16_t)(x | y << 8);
            }
    for (t = 0; t < 4; t++)
        for (y = 0; y < 64; y++)
            for (x = 0; x < 64; x++) {
                sw[64 + t * 64 + y][x] = gs_pg16[t][y][x];
                inv[2 + t][gs_pg16[t][y][x]] = (uint16_t)(x | y << 8);
            }
    for (y = 0; y < 64; y++)
        for (x = 0; x < 128; x++)
            sw[320 + y][x] = gs_pg8[y][x];
    for (y = 0; y < 128; y++)
        for (x = 0; x < 128; x++)
            sw[384 + y][x] = gs_pg4[y][x];
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    sw_tex = new_tex(GL_R16UI, 128, 512, GL_RED_INTEGER, GL_UNSIGNED_SHORT);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 128, 512, GL_RED_INTEGER, GL_UNSIGNED_SHORT, sw);
    inv_tex = new_tex(GL_R16UI, 4096, 6, GL_RED_INTEGER, GL_UNSIGNED_SHORT);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4096, 6, GL_RED_INTEGER, GL_UNSIGNED_SHORT, inv);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    clut_tex = new_tex(GL_RGBA8, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE);

    glGenSamplers(6, samp);
    for (i = 0; i < 6; i++) {
        glSamplerParameteri(samp[i], GL_TEXTURE_MIN_FILTER, i == 5 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glSamplerParameteri(samp[i], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glSamplerParameteri(samp[i], GL_TEXTURE_WRAP_S, i < 4 && !(i & 1) ? GL_REPEAT : GL_CLAMP_TO_EDGE);
        glSamplerParameteri(samp[i], GL_TEXTURE_WRAP_T, i < 4 && !(i & 2) ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    }

    glGenVertexArrays(1, &empty_vao);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, VBO_SIZE, NULL, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(V), (void *)offsetof(V, x));
    glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, sizeof(V), (void *)offsetof(V, z));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(V), (void *)offsetof(V, c));
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(V), (void *)offsetof(V, s));
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(V), (void *)offsetof(V, f));
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(V), (void *)offsetof(V, cl));
    for (i = 0; i < 6; i++) {
        glEnableVertexAttribArray((GLuint)i);
    }
    glBindVertexArray(empty_vao);
    glGenFramebuffers(1, &draw_fbo);
    if (glGetError() != GL_NO_ERROR) {
        grx_log("gs-gl: GL error while setting up");
        return -1;
    }

    gs_flush();
    gl_loaded();
    gs_backend = &gl_backend;
    grx_video_gl_source(1);
    e = getenv("GRX_GSGL_TRACE");
    trace = e != NULL ? atoi(e) : 0;
    e = getenv("GRX_GSGL_NORT");
    no_rt = e != NULL && atoi(e) > 0;
    e = getenv("GRX_GS_STATS");
    if (e != NULL && atoi(e) > 0 && !stats_on) {
        stats_on = 1;
        atexit(print_stats);
    }
    grx_log("gs: GPU renderer, OpenGL %s on %s, internal scale %dx%d (%dx%d)%s", (const char *)glGetString(GL_VERSION),
            (const char *)glGetString(GL_RENDERER), sx, sy, 640 * sx, 448 * sy,
            clip01 ? "" : ", no clip control (depth less precise)");
    return 0;
}
