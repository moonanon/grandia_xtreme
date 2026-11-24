/* Map patches and extra "Return to" destinations (grx_mdt.h,
   port/tools/mods.py, port/docs/geo-gates.md).

   mods/mdt/XXXX.mdp, read when map file XXXX is loaded: after the map
   decompresses into a buffer made that much larger, each "append" puts its
   data after the map (16-aligned) and points a header word at it; each
   "set" writes a header word. That replaces whole sections (entrances,
   event areas, OCCs, scripts...) with longer copies, offsets in the header
   being from the map's start.

   mods/mods.txt: the mods built, one name a line; the port's code applies
   the ones it names that have no files (grx_mod_enabled).

   mods/flaglist.txt: "flag value name" lines, destinations after the
   game's 0x26 in the town gate's list (FldFlagListOpen): shown when the
   global flag is set, picking one runs the list's script with the value.

   Both come from the player's own disc through mods.py; nothing of the
   game's is in the repo. Paths are from the working directory. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "grx_mdt.h"
#include "grx_platform.h"

#define MAX_PATCH (4u << 20)

static int cached_file = -1;
static unsigned char *patch;
static uint32_t patch_len;

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void load(int file) {
    char path[64];
    FILE *f;
    long n;

    if (file == cached_file) {
        return;
    }
    cached_file = file;
    free(patch);
    patch = NULL;
    patch_len = 0;
    snprintf(path, sizeof(path), "mods/mdt/%04X.mdp", file & 0xFFFF);
    f = fopen(path, "rb");
    if (f == NULL) {
        return;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n >= 8 && (unsigned long)n <= MAX_PATCH) {
        patch = malloc((size_t)n);
        if (patch != NULL && fread(patch, 1, (size_t)n, f) == (size_t)n && memcmp(patch, "GRXP", 4) == 0) {
            patch_len = (uint32_t)n;
        } else {
            free(patch);
            patch = NULL;
        }
    }
    fclose(f);
    if (patch == NULL) {
        grx_log("mdt: %s isn't a map patch, left out", path);
    }
}

/* Calls fn(type, field, data, size) for each op; 0 if the file is bad */
static int walk(void (*fn)(void *, uint32_t, uint32_t, const unsigned char *, uint32_t), void *ctx) {
    uint32_t n, i, p = 8;

    if (patch == NULL) {
        return 0;
    }
    n = rd32(patch + 4);
    for (i = 0; i < n; i++) {
        uint32_t type, field, size;
        if (p + 12 > patch_len) {
            return 0;
        }
        type = rd32(patch + p);
        field = rd32(patch + p + 4);
        size = rd32(patch + p + 8);
        p += 12;
        if (size > patch_len - p) {
            return 0;
        }
        fn(ctx, type, field, patch + p, size);
        p = (p + size + 3) & ~3u;
    }
    return 1;
}

static void add_size(void *ctx, uint32_t type, uint32_t field, const unsigned char *data, uint32_t size) {
    (void)field;
    (void)data;
    if (type == 1) {
        *(uint32_t *)ctx += size + 16;
    }
}

uint32_t grx_mdt_extra(int file) {
    uint32_t extra = 0;
    load(file);
    if (!walk(add_size, &extra)) {
        return 0;
    }
    return extra + 16;
}

typedef struct {
    unsigned char *data;
    uint32_t end;
} Apply;

static void apply(void *ctx, uint32_t type, uint32_t field, const unsigned char *src, uint32_t size) {
    Apply *a = ctx;
    if (field + 4 > 0x200) {
        return; /* header words only */
    }
    if (type == 0 && size == 4) {
        memcpy(a->data + field, src, 4);
    } else if (type == 1) {
        a->end = (a->end + 15) & ~15u;
        memcpy(a->data + a->end, src, size);
        memcpy(a->data + field, &a->end, 4);
        a->end += size;
    }
}

void grx_mdt_patch(int file, unsigned char *data, uint32_t size) {
    Apply a;
    load(file);
    if (patch == NULL) {
        return;
    }
    a.data = data;
    a.end = size;
    if (walk(apply, &a)) {
        grx_log("mdt: map %04X patched (mods/mdt), %u bytes added", file & 0xFFFF, a.end - size);
    }
}

/* ---------------------------------------------------------------------- */
/* Extra "Return to" destinations */

/* FldFlagListWork.entries holds 0x40 lines, the game's 0x26 first */
#define MAX_DEST (0x40 - 0x26)

typedef struct {
    uint16_t flag, value;
    /* The name in the scripts' 2-byte codes (char + 0x1F, 0xF9), as
       TxtGlyphQueueStr reads them; in the exe's image, below 4 GB */
    unsigned char name[96];
    /* And as glyph codes (char + 0x81F), as the list draws them */
    uint16_t glyphs[48];
    int nglyphs;
} Dest;

static Dest dests[MAX_DEST];
static int ndests = -1;

static void dests_load(void) {
    FILE *f;
    char line[160];

    if (ndests >= 0) {
        return;
    }
    ndests = 0;
    f = fopen("mods/flaglist.txt", "r");
    if (f == NULL) {
        return;
    }
    while (ndests < MAX_DEST && fgets(line, sizeof(line), f) != NULL) {
        unsigned flag, value;
        int at = 0, i, k = 0;
        Dest *d = &dests[ndests];
        d->nglyphs = 0;
        if (line[0] == '#' || sscanf(line, "%x %x %n", &flag, &value, &at) < 2) {
            continue;
        }
        for (i = at; line[i] && line[i] != '\n' && line[i] != '\r' && k + 3 < (int)sizeof(d->name); i++) {
            unsigned int c = (unsigned char)line[i];
            if (c < 0x20 || c > 0x7E) {
                continue;
            }
            /* The font's space is code 0x80 (the game's names have it) */
            if (c == ' ') {
                c = 0x80;
            }
            d->name[k++] = (unsigned char)(c + 0x1F);
            d->name[k++] = 0xF9;
            d->glyphs[d->nglyphs++] = (uint16_t)(c + 0x81F);
        }
        d->name[k] = 0;
        d->flag = (uint16_t)flag;
        d->value = (uint16_t)value;
        ndests++;
    }
    fclose(f);
    grx_log("mdt: %d extra Geo Gate destinations (mods/flaglist.txt)", ndests);
}

int grx_flaglist_count(void) {
    dests_load();
    return ndests;
}

int grx_flaglist_flag(int i) {
    dests_load();
    return i >= 0 && i < ndests ? dests[i].flag : 0;
}

int grx_flaglist_value(int i) {
    dests_load();
    return i >= 0 && i < ndests ? dests[i].value : 0;
}

unsigned char *grx_flaglist_name(int i) {
    dests_load();
    return i >= 0 && i < ndests ? dests[i].name : NULL;
}

uint16_t *grx_flaglist_glyphs(int i, int *len) {
    dests_load();
    if (i < 0 || i >= ndests) {
        *len = 0;
        return NULL;
    }
    *len = dests[i].nglyphs;
    return dests[i].glyphs;
}

/* mods/mods.txt, read once */
#define MAX_MODS 32
static char mod_names[MAX_MODS][48];
static int nmods = -1;

int grx_mod_enabled(const char *name) {
    int i;
    if (nmods < 0) {
        FILE *f = fopen("mods/mods.txt", "r");
        char line[64];
        nmods = 0;
        while (f != NULL && nmods < MAX_MODS && fgets(line, sizeof(line), f) != NULL) {
            if (sscanf(line, " %47[a-z0-9-]", mod_names[nmods]) == 1) {
                nmods++;
            }
        }
        if (f != NULL) {
            fclose(f);
        }
        grx_log("mods: %d enabled (mods/mods.txt)", nmods);
    }
    for (i = 0; i < nmods; i++) {
        if (strcmp(mod_names[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}
