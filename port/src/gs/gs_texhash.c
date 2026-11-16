/* The hashes PCSX2 gives a texture, for its replacement packs
   (pcsx2/GS/Renderers/HW/GSTextureCache.cpp: HashTextureLevel and
   HashCacheKey::Create; GSTextureReplacements.cpp for the name).

   - Texture: XXH3-64 of the texture's 256-byte blocks of local memory, row
     of blocks by row of blocks, in GS memory layout, as the GS stores them
     (PSMCT32, PSMT8 and PSMT4 whose size is at least a block: the others
     PCSX2 unswizzles first, which the port doesn't need yet).
   - CLUT: XXH3-64 of the palette as 32-bit entries, 256 or 16 of them.
   - Bits: PSM (6), TW (4), TH (4), unused (1), TEXA TA0 (8), AEM (1),
     TA1 (8); the TEXA ones only for 16 and 24-bit textures, so zero here. */
#define XXH_INLINE_ALL
#include "xxhash.h"
#include "gs_internal.h"
#include "gs_texhash.h"

int gs_texhash(const GsTex *t, uint64_t *tex, uint64_t *clut, uint32_t *bits) {
    uint32_t bx, by, w = 1u << t->tw, h = 1u << t->th, bsx, bsy, nx, ny;
    XXH3_state_t st;

    switch (t->kind) {
    case GS_K32: bsx = 8; bsy = 8; break;
    case GS_K8: bsx = 16; bsy = 16; break;
    case GS_K4: bsx = 32; bsy = 16; break;
    default: return 0;
    }
    if (w < bsx || h < bsy) {
        return 0;
    }
    nx = w / bsx;
    ny = h / bsy;
    XXH3_64bits_reset(&st);
    for (by = 0; by < ny; by++) {
        for (bx = 0; bx < nx; bx++) {
            uint32_t a, x = bx * bsx, y = by * bsy;
            switch (t->kind) {
            case GS_K32: a = gs_addr32(0, t->tbp, t->tbw, x, y) * 4; break;
            case GS_K8: a = gs_addr8(t->tbp, t->tbw, x, y); break;
            default: a = gs_addr4(t->tbp, t->tbw, x, y) >> 1; break;
            }
            XXH3_64bits_update(&st, gs_vram + a, 256);
        }
    }
    *tex = XXH3_64bits_digest(&st);
    *clut = t->kind == GS_K32 ? 0 : XXH3_64bits(t->pal, sizeof(t->pal[0]) * (t->kind == GS_K8 ? 256 : 16));
    *bits = t->psm | t->tw << 6 | t->th << 10;
    return 1;
}

#include <stdio.h>
#include <stdlib.h>

void gs_texhash_log(const GsTex *t) {
    static FILE *f;
    static int init;
    static uint64_t seen[1 << 14];
    uint64_t tex, clut, key;
    uint32_t bits, i;

    if (!init) {
        const char *p = getenv("GRX_TEXHASH_LOG");
        init = 1;
        f = p != NULL && *p ? fopen(p, "a") : NULL;
    }
    if (f == NULL || !gs_texhash(t, &tex, &clut, &bits)) {
        return;
    }
    key = (tex ^ (clut * 0x9E3779B97F4A7C15ull) ^ bits) | 1;
    for (i = (uint32_t)key & ((1 << 14) - 1);; i = (i + 1) & ((1 << 14) - 1)) {
        if (seen[i] == key) {
            return;
        }
        if (seen[i] == 0) {
            seen[i] = key;
            break;
        }
    }
    if (t->kind == GS_K32) {
        fprintf(f, "%llx-%08x\n", (unsigned long long)tex, bits);
    } else {
        fprintf(f, "%llx-%llx-%08x\n", (unsigned long long)tex, (unsigned long long)clut, bits);
    }
    fflush(f);
}
