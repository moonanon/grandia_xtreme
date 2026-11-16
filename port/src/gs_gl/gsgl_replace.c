/* A PCSX2 replacement pack in the GPU renderer: files named
   <texture hash>-<CLUT hash>-<bits>.dds (or <texture hash>-<bits>.dds
   without a palette), anywhere under the folder. The hashes are PCSX2's
   (gs/gs_texhash.c). Bit 14 of <bits> was PCSX2's TCC, since dropped: it
   is ignored, as PCSX2 does when it reads a name. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "grx_platform.h"
#include "gsgl_replace.h"
#include "../gs/gs_texhash.h"

#define BITS_MASK (~0x4000u)

typedef struct {
    uint64_t tex, clut;
    uint32_t bits;
    char *path;
    GLuint gl;
    int state; /* 0 not loaded, 1 loaded, 2 failed */
} Rep;

static Rep *reps;
static int nreps, capreps;
static int *table; /* indices into reps, -1 empty */
static uint32_t tcap;
static uint64_t loaded_bytes;
static int nloaded;
static int enabled;

static uint32_t slot_of(uint64_t tex, uint64_t clut, uint32_t bits) {
    uint64_t h = tex * 0x9E3779B97F4A7C15ull ^ (clut + 0x7F4A7C15ull) * 0xC2B2AE3D27D4EB4Full ^ bits * 0x165667B19E3779F9ull;
    return (uint32_t)(h >> 32) & (tcap - 1);
}

static int find(uint64_t tex, uint64_t clut, uint32_t bits) {
    uint32_t i = slot_of(tex, clut, bits);
    while (table[i] >= 0) {
        const Rep *r = &reps[table[i]];
        if (r->tex == tex && r->clut == clut && r->bits == bits) {
            return table[i];
        }
        i = (i + 1) & (tcap - 1);
    }
    return -1;
}

static void add(uint64_t tex, uint64_t clut, uint32_t bits, const char *path) {
    uint32_t i;
    if (nreps == capreps) {
        capreps = capreps ? capreps * 2 : 2048;
        reps = realloc(reps, (size_t)capreps * sizeof(*reps));
        if (reps == NULL) {
            grx_fatal("textures: out of memory");
        }
    }
    if (tcap == 0 || (uint32_t)(nreps + 1) * 2 > tcap) {
        uint32_t k;
        tcap = tcap ? tcap * 2 : 8192;
        free(table);
        table = malloc(tcap * sizeof(*table));
        if (table == NULL) {
            grx_fatal("textures: out of memory");
        }
        memset(table, 0xFF, tcap * sizeof(*table));
        for (k = 0; k < (uint32_t)nreps; k++) {
            for (i = slot_of(reps[k].tex, reps[k].clut, reps[k].bits); table[i] >= 0; i = (i + 1) & (tcap - 1)) {
            }
            table[i] = (int)k;
        }
    }
    bits &= BITS_MASK;
    if (find(tex, clut, bits) >= 0) {
        return; /* the same texture twice: the first wins */
    }
    reps[nreps].tex = tex;
    reps[nreps].clut = clut;
    reps[nreps].bits = bits;
    reps[nreps].path = strdup(path);
    reps[nreps].gl = 0;
    reps[nreps].state = 0;
    for (i = slot_of(tex, clut, bits); table[i] >= 0; i = (i + 1) & (tcap - 1)) {
    }
    table[i] = nreps++;
}

/* "<hex>-<hex>-<hex>.dds" or "<hex>-<hex>.dds" */
static int parse_name(const char *name, uint64_t *tex, uint64_t *clut, uint32_t *bits) {
    unsigned long long a, b;
    unsigned c;
    size_t n = strlen(name);
    int k;
    if (n < 5 || strcasecmp(name + n - 4, ".dds") != 0) {
        return 0;
    }
    k = sscanf(name, "%llx-%llx-%x.", &a, &b, &c);
    if (k == 3) {
        *tex = a; *clut = b; *bits = c;
        return 1;
    }
    k = sscanf(name, "%llx-%x.", &a, &c);
    if (k == 2) {
        *tex = a; *clut = 0; *bits = c;
        return 1;
    }
    return 0;
}

typedef struct {
    int n;
} ScanCtx;

static SDL_EnumerationResult SDLCALL scan_cb(void *user, const char *dirname, const char *fname) {
    char path[2048];
    uint64_t tex, clut;
    uint32_t bits;
    SDL_PathInfo info;
    ScanCtx *ctx = user;
    size_t dn = strlen(dirname);
    snprintf(path, sizeof(path), "%s%s%s", dirname, dn && dirname[dn - 1] != '/' && dirname[dn - 1] != '\\' ? "/" : "", fname);
    if (SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_DIRECTORY) {
        char sub[2048];
        snprintf(sub, sizeof(sub), "%s/", path);
        SDL_EnumerateDirectory(sub, scan_cb, user);
        return SDL_ENUM_CONTINUE;
    }
    if (parse_name(fname, &tex, &clut, &bits)) {
        add(tex, clut, bits, path);
        ctx->n++;
    }
    return SDL_ENUM_CONTINUE;
}

static int scan_root(const char *dir, size_t n) {
    char root[2048];
    ScanCtx ctx = {0};
    int before = nreps;
    if (n == 0 || n >= sizeof(root) - 1) {
        return 0;
    }
    memcpy(root, dir, n);
    root[n] = 0;
    if (root[n - 1] != '/' && root[n - 1] != '\\') {
        root[n] = '/';
        root[n + 1] = 0;
    }
    SDL_EnumerateDirectory(root, scan_cb, &ctx);
    grx_log("textures: %d files in %s, %d used (others already given by an earlier folder)", ctx.n, root, nreps - before);
    return ctx.n;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int gsgl_rep_init(void) {
    const char *dir = getenv("GRX_TEXTURES");
    int total = 0;

    if (dir == NULL || *dir == 0) {
        /* Default: each folder in ./textures, in name order (earlier wins) */
        int i, count = 0;
        char **names = SDL_GlobDirectory("textures", NULL, 0, &count);
        if (names == NULL || count == 0) {
            SDL_free(names);
            return 0;
        }
        qsort(names, (size_t)count, sizeof(*names), cmp_str);
        for (i = 0; i < count; i++) {
            char path[2048];
            SDL_PathInfo info;
            snprintf(path, sizeof(path), "textures/%s", names[i]);
            /* (the glob lists subfolders too: only the top level here) */
            if (strpbrk(names[i], "/\\") == NULL && SDL_GetPathInfo(path, &info) &&
                info.type == SDL_PATHTYPE_DIRECTORY) {
                total += scan_root(path, strlen(path));
            }
        }
        SDL_free(names);
        enabled = nreps > 0;
        grx_log("textures: %d replacements indexed", nreps);
        return total;
    }
    /* A list of folders (';'-separated on Windows, ':' elsewhere); for a
       texture in several, the first folder's file is used */
    while (*dir) {
#ifdef _WIN32
        size_t n = strcspn(dir, ";");
#else
        size_t n = strcspn(dir, ":");
#endif
        total += scan_root(dir, n);
        dir += n;
        if (*dir) {
            dir++;
        }
    }
    enabled = nreps > 0;
    grx_log("textures: %d replacements indexed%s", nreps, enabled ? "" : " (none: replacements off)");
    return total;
}

/* Upload a DDS (DXT1/3/5, with its mip levels) as a GL texture; 0 on failure */
static GLuint load_dds(const char *path, uint64_t *bytes) {
    FILE *f = fopen(path, "rb");
    uint8_t *buf = NULL;
    long len;
    uint32_t w, h, mips, fcc, i, bs;
    GLenum fmt;
    GLuint t = 0;
    size_t off = 128;

    if (f == NULL) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 128 || (buf = malloc((size_t)len)) == NULL || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(buf);
        return 0;
    }
    fclose(f);
#define U32(o) ((uint32_t)buf[o] | (uint32_t)buf[(o) + 1] << 8 | (uint32_t)buf[(o) + 2] << 16 | (uint32_t)buf[(o) + 3] << 24)
    h = U32(12);
    w = U32(16);
    mips = U32(28);
    fcc = U32(84);
    if (memcmp(buf, "DDS ", 4) != 0 || w == 0 || h == 0 || w > 16384 || h > 16384) {
        free(buf);
        return 0;
    }
    if (mips == 0) {
        mips = 1;
    }
    if (fcc == 0x31545844u) { /* DXT1 */
        fmt = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
        bs = 8;
    } else if (fcc == 0x33545844u) { /* DXT3 */
        fmt = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
        bs = 16;
    } else if (fcc == 0x35545844u) { /* DXT5 */
        fmt = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        bs = 16;
    } else {
        free(buf);
        return 0;
    }
#undef U32
    glGenTextures(1, &t);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t);
    for (i = 0; i < mips; i++) {
        uint32_t lw = w >> i ? w >> i : 1, lh = h >> i ? h >> i : 1;
        size_t sz = (size_t)((lw + 3) / 4) * ((lh + 3) / 4) * bs;
        if (off + sz > (size_t)len) {
            mips = i; /* truncated file: the levels it has */
            break;
        }
        glCompressedTexImage2D(GL_TEXTURE_2D, (GLint)i, fmt, (GLsizei)lw, (GLsizei)lh, 0, (GLsizei)sz, buf + off);
        off += sz;
        *bytes += sz;
    }
    free(buf);
    if (mips == 0) {
        glDeleteTextures(1, &t);
        return 0;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)mips - 1);
    return t;
}

GLuint gsgl_rep_lookup(const GsTex *t) {
    uint64_t tex, clut;
    uint32_t bits;
    int i;
    Rep *r;

    if (!enabled || !gs_texhash(t, &tex, &clut, &bits)) {
        return 0;
    }
    i = find(tex, clut, bits & BITS_MASK);
    if (i < 0) {
        return 0;
    }
    r = &reps[i];
    if (r->state == 0) {
        r->gl = load_dds(r->path, &loaded_bytes);
        r->state = r->gl ? 1 : 2;
        if (r->gl == 0) {
            grx_log("textures: can't use %s", r->path);
        } else {
            nloaded++;
            if (nloaded == 1 || nloaded % 25 == 0) {
                grx_log("textures: %d replacements in use, %llu MB", nloaded, (unsigned long long)(loaded_bytes >> 20));
            }
        }
    }
    return r->gl;
}
