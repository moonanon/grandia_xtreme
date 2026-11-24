/* Patches to the system data pack (grx_grxsys.h, port/tools/grxsys.py).

   mods/grxsys.grxp, read when SysDataInit unpacks SYS/GRXSYS.PAC: the buffer
   is made `extra` bytes larger and each op writes its bytes at an offset of
   the unpacked file (offsets past its end are the added part: new rows,
   records and text, which the file's tables point at). The patch records the
   unpacked size it was made against; a different file is left alone.

   Layout (little-endian u32): "GRXS", unpacked size, extra, count, then per
   op: offset, size, data (padded to 4). Made from the player's own disc. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "grx_grxsys.h"
#include "grx_platform.h"

#define MAX_PATCH (4u << 20)

static unsigned char *patch;
static uint32_t patch_len;
static int loaded;

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void load(void) {
    FILE *f;
    long n;

    if (loaded) {
        return;
    }
    loaded = 1;
    f = fopen("mods/grxsys.grxp", "rb");
    if (f == NULL) {
        return;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n >= 16 && (unsigned long)n <= MAX_PATCH) {
        patch = malloc((size_t)n);
        if (patch != NULL && fread(patch, 1, (size_t)n, f) == (size_t)n && memcmp(patch, "GRXS", 4) == 0) {
            patch_len = (uint32_t)n;
        } else {
            free(patch);
            patch = NULL;
        }
    }
    fclose(f);
    if (patch == NULL) {
        grx_log("grxsys: mods/grxsys.grxp isn't a patch, left out");
    }
}

uint32_t grx_grxsys_extra(void) {
    load();
    return patch != NULL ? (rd32(patch + 8) + 15) & ~15u : 0;
}

void grx_grxsys_patch(unsigned char *data, uint32_t size) {
    uint32_t n, i, p = 16, limit;

    load();
    if (patch == NULL) {
        return;
    }
    if (rd32(patch + 4) != size) {
        grx_log("grxsys: the patch is for a %u byte file, this is %u: left out", rd32(patch + 4), size);
        return;
    }
    limit = size + rd32(patch + 8);
    n = rd32(patch + 12);
    for (i = 0; i < n; i++) {
        uint32_t off, len;
        if (p + 8 > patch_len) {
            break;
        }
        off = rd32(patch + p);
        len = rd32(patch + p + 4);
        p += 8;
        if (len > patch_len - p || off > limit || len > limit - off) {
            break;
        }
        memcpy(data + off, patch + p, len);
        p = (p + len + 3) & ~3u;
    }
    grx_log("grxsys: patched (mods/grxsys.grxp), %u ops", n);
}
