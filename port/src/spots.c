/* Spot recorder (grx_skip.h): F8 appends where the party leader stands
   to spots.txt in the working directory, one line a spot - the map
   number, the leader's position and facing - and shows "Location recorded".
   For picking places in the field by playing (save points to add). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <math.h>
#include <SDL3/SDL.h>
#include "grx_skip.h"
#include "grx_platform.h"

/* The game's */
extern unsigned char sysState[];    /* +0x0B program: 2 FIELD */
extern unsigned char sysSaveData[]; /* +0x1E4E map number, +0x1E4C the one before */
extern unsigned char chrList[];     /* +4: 9 ChrObj pointers, 1 the leader (ChrGetByNo) */

#define PROGRAM_FIELD 2
#define CHR_HEADING 0x54  /* s32, 16-bit angle units */
#define CHR_MODEL 0x60    /* GphModel pointer */
#define MODEL_POS 0x10    /* 3 floats */

static SDL_AtomicInt want;

static uint32_t rd32(const unsigned char *p);

#ifdef GRX_DEBUG_KIT
/* Debug kit (build.py --debug): the game's debug mode, and noclip */
extern int sysDebugMode;
static SDL_AtomicInt noclip;
static float noclip_y;
static int noclip_set;

/* Map warp: F6 the next map of the game's MAP JUMP list, Shift+F6 the one
   before, as its debug page jumps (the map file must exist) */
extern unsigned short dbgFldMapJumpTable[]; /* 0xBA map numbers */
extern void MapJumpRequest(int, int);
extern void MapMakeMdtPath(unsigned short, char *);
extern int FileGetSizeAligned(int);
extern int MapLoadGetState(void);
#define MAP_JUMP_COUNT 0xBA
static SDL_AtomicInt warp_step;
static char warp_path[64];

void grx_warp_request(int step) {
    SDL_SetAtomicInt(&warp_step, step);
}

static void warp_field(void) {
    int step = SDL_GetAtomicInt(&warp_step), i, n, cur = -1;
    uint16_t map;

    if (step == 0 || sysState[0x0B] != PROGRAM_FIELD || MapLoadGetState() != -1) {
        return;
    }
    SDL_SetAtomicInt(&warp_step, 0);
    memcpy(&map, sysSaveData + 0x1E4E, 2);
    for (i = 0; i < MAP_JUMP_COUNT; i++) {
        if (dbgFldMapJumpTable[i] == map) {
            cur = i;
            break;
        }
    }
    for (n = 0, i = cur; n < MAP_JUMP_COUNT; n++) {
        i = (i + step + MAP_JUMP_COUNT) % MAP_JUMP_COUNT;
        MapMakeMdtPath(dbgFldMapJumpTable[i], warp_path);
        if (FileGetSizeAligned((int)(uintptr_t)warp_path) > 0) {
            grx_log("debug: warp to map 0x%04X (%d of %d, %s)", dbgFldMapJumpTable[i], i + 1, MAP_JUMP_COUNT, warp_path);
            noclip_set = 0;
            MapJumpRequest(dbgFldMapJumpTable[i], 0);
            return;
        }
    }
    grx_log("debug: no map to warp to");
}

/* Gate warp: Ctrl+F6 the next Geo Gate, Ctrl+Shift+F6 the one before,
   arriving at its arrival entrance (0x2000). The game's gates are the
   town list's destinations (town OCCs 0x3D1F-0x3D44); the mods' are
   there when their map patch is (port/tools/mods.py) */
static const uint16_t gate_maps[] = {
    0x4040, 0x4050, 0x4800, 0x4820, 0x5010, 0x5020, 0x50F0, 0x5810, 0x5830, 0x6080, 0x6082, 0x6084, 0x6086,
    0x6088, 0x608A, 0x7024, 0x8010, 0x9014, 0x9034, 0xA080, 0xA082, 0xD880, 0xD882, 0xD884, 0xD886,
    0xD888, 0xD88A, 0xD88C, 0xD88E, 0xD890, 0xD892, 0xD894, 0xD896, 0xD898, 0xD89A, 0xD89C, 0xD89E,
    0xD8A0, 0xD8A2, 0xD8A4, 0xD8A6, 0x3838,
};
static const uint16_t mod_gate_maps[] = {0x4040, 0x4050, 0x50F0, 0x5830};
#define GATE_COUNT ((int)(sizeof(gate_maps) / sizeof(gate_maps[0])))
static SDL_AtomicInt gate_step;
static int gate_cur = -1;

void grx_gate_warp_request(int step) {
    SDL_SetAtomicInt(&gate_step, step);
}

static int gate_available(uint16_t map) {
    char path[32];
    size_t i;
    FILE *f;
    for (i = 0; i < sizeof(mod_gate_maps) / sizeof(mod_gate_maps[0]); i++) {
        if (mod_gate_maps[i] == map) {
            snprintf(path, sizeof(path), "mods/mdt/%04X.mdp", map);
            if ((f = fopen(path, "rb")) == NULL) {
                return 0;
            }
            fclose(f);
        }
    }
    MapMakeMdtPath(map, warp_path);
    return FileGetSizeAligned((int)(uintptr_t)warp_path) > 0;
}

static void warp_gate(void) {
    int step = SDL_GetAtomicInt(&gate_step), n, i;
    uint16_t map;

    if (step == 0 || sysState[0x0B] != PROGRAM_FIELD || MapLoadGetState() != -1) {
        return;
    }
    SDL_SetAtomicInt(&gate_step, 0);
    memcpy(&map, sysSaveData + 0x1E4E, 2);
    if (gate_cur < 0 || gate_maps[gate_cur] != map) {
        /* From elsewhere: the first gate onwards, or the last one back */
        gate_cur = step > 0 ? GATE_COUNT - 1 : 0;
        for (i = 0; i < GATE_COUNT; i++) {
            if (gate_maps[i] == map) {
                gate_cur = i;
            }
        }
    }
    for (n = 0, i = gate_cur; n < GATE_COUNT; n++) {
        i = (i + step + GATE_COUNT) % GATE_COUNT;
        if (gate_available(gate_maps[i])) {
            grx_log("debug: warp to the Geo Gate on map 0x%04X (%d of %d)", gate_maps[i], i + 1, GATE_COUNT);
            gate_cur = i;
            noclip_set = 0;
            MapJumpRequest(gate_maps[i], 0x2000);
            return;
        }
    }
    grx_log("debug: no Geo Gate to warp to");
}

/* Super mode (F5, port/tools/fixups/devkit.py) */
static SDL_AtomicInt super_on;

int grx_super(void) {
    return SDL_GetAtomicInt(&super_on);
}

void grx_super_toggle(void) {
    int on = !SDL_GetAtomicInt(&super_on);
    SDL_SetAtomicInt(&super_on, on);
    grx_log("debug: super mode %s (party can't die, free MP/SP, L2+L1 in the command menu kills all)",
            on ? "on" : "off");
}

/* Map reveal (Shift+F7 or GRX_MAP_REVEAL=1, port/tools/fixups/devkit.py) */
static SDL_AtomicInt reveal = {-1}, reveal_changed;

int grx_map_reveal(void) {
    int on = SDL_GetAtomicInt(&reveal);
    if (on < 0) {
        const char *e = getenv("GRX_MAP_REVEAL");
        on = e != NULL && e[0] != '\0' && e[0] != '0';
        SDL_SetAtomicInt(&reveal, on);
    }
    return on;
}

int grx_map_reveal_changed(void) {
    return SDL_CompareAndSwapAtomicInt(&reveal_changed, 1, 0);
}

void grx_map_reveal_toggle(void) {
    int on = !grx_map_reveal();
    SDL_SetAtomicInt(&reveal, on);
    SDL_SetAtomicInt(&reveal_changed, 1);
    grx_log("debug: map reveal %s", on ? "on (the whole minimap)" : "off (from the next map)");
}

int grx_noclip(void) {
    return SDL_GetAtomicInt(&noclip);
}

void grx_noclip_toggle(void) {
    int on = !SDL_GetAtomicInt(&noclip);
    SDL_SetAtomicInt(&noclip, on);
    noclip_set = 0;
    grx_log("debug: noclip %s", on ? "on" : "off");
}

/* Walls and floors off for the leader, its height held (PgUp/PgDn move
   it); event areas (exits, doors) still trigger */
static void noclip_field(void) {
    unsigned char *chr, *model;
    const bool *keys = SDL_GetKeyboardState(NULL);
    float zero = 0, y;
    uint16_t mask;

    if (getenv("GRX_GAME_DEBUG") != NULL) {
        sysDebugMode = 1;
    }
    if (!SDL_GetAtomicInt(&noclip) || sysState[0x0B] != PROGRAM_FIELD) {
        return;
    }
    chr = (unsigned char *)(uintptr_t)rd32(chrList + 4);
    model = chr != NULL ? (unsigned char *)(uintptr_t)rd32(chr + CHR_MODEL) : NULL;
    if (model == NULL) {
        return;
    }
    if (!noclip_set) {
        memcpy(&noclip_y, model + MODEL_POS + 4, 4);
        noclip_set = 1;
    }
    if (keys != NULL && keys[SDL_SCANCODE_PAGEUP]) {
        noclip_y += 15.0f;
    }
    if (keys != NULL && keys[SDL_SCANCODE_PAGEDOWN]) {
        noclip_y -= 15.0f;
    }
    /* Home/End: forward/back along the leader's heading (16-bit angle;
       FldDirToRot: facing (-sin, cos) on x/z), through walls */
    if (keys != NULL && (keys[SDL_SCANCODE_HOME] || keys[SDL_SCANCODE_END])) {
        int32_t heading;
        float pos[3], a, step = keys[SDL_SCANCODE_HOME] ? 30.0f : -30.0f;
        memcpy(&heading, chr + CHR_HEADING, 4);
        memcpy(pos, model + MODEL_POS, sizeof(pos));
        a = (float)(heading & 0xFFFF) * (6.2831853f / 65536.0f);
        pos[0] += -sinf(a) * step;
        pos[2] += cosf(a) * step;
        memcpy(model + MODEL_POS, pos, sizeof(pos));
    }
    memcpy(&mask, chr + 6, 2);
    mask &= (uint16_t)~(2 | 8);
    memcpy(chr + 6, &mask, 2);
    y = noclip_y;
    memcpy(model + MODEL_POS + 4, &y, 4);
    memcpy(chr + 0x184, &zero, 4);
}

#endif /* GRX_DEBUG_KIT */

void grx_spot_request(void) {
    SDL_SetAtomicInt(&want, 1);
}

static uint32_t rd32(const unsigned char *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

void grx_spot_field(void) {
    const unsigned char *chr, *model;
    uint16_t map, old;
    float pos[3];
    int32_t heading;
    FILE *f;
    time_t now;
    char when[32];

#ifdef GRX_DEBUG_KIT
    noclip_field();
    warp_field();
    warp_gate();
#endif
    if (!SDL_CompareAndSwapAtomicInt(&want, 1, 0)) {
        return;
    }
    if (sysState[0x0B] != PROGRAM_FIELD) {
        grx_log("spot: not in the field");
        return;
    }
    chr = (const unsigned char *)(uintptr_t)rd32(chrList + 4);
    model = chr != NULL ? (const unsigned char *)(uintptr_t)rd32(chr + CHR_MODEL) : NULL;
    if (model == NULL) {
        grx_log("spot: no party leader here");
        return;
    }
    memcpy(&map, sysSaveData + 0x1E4E, 2);
    memcpy(&old, sysSaveData + 0x1E4C, 2);
    memcpy(pos, model + MODEL_POS, sizeof(pos));
    memcpy(&heading, chr + CHR_HEADING, 4);
    now = time(NULL);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&now));
    f = fopen("spots.txt", "a");
    if (f == NULL) {
        grx_log("spot: can't open spots.txt");
        return;
    }
    fprintf(f, "map 0x%04X (from 0x%04X)  pos %.1f %.1f %.1f  facing %.0f  # %s\n", map, old, pos[0], pos[1],
            pos[2], (double)(heading & 0xFFFF) * 360.0 / 65536.0, when);
    fclose(f);
    grx_log("spot: map 0x%04X pos %.1f %.1f %.1f saved to spots.txt", map, pos[0], pos[1], pos[2]);
    grx_skip_notice();
}
