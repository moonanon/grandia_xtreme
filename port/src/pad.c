/*
 * The controllers (grx_pad.h): Sony's libpad (src/main/479818.c to
 * 47AB58.c, and scePadPortOpen, still in assembly) over SDL3 gamepads and
 * the keyboard, with padman's side (the IOP module that talks to the pads
 * and DMAs their state to the EE every frame) simulated here.
 *
 * Each open port has a ScePadData record (sdk/libpad.h), what padman
 * would DMA: the state, the mode tables, the pending request's state and
 * the 32 data bytes scePadRead copies. The libpad calls read and write it
 * as the originals read and write the DMA area (their logic is kept), and
 * it is also copied into both halves of the caller's 0x100-byte area at
 * every update. Requests (scePadSetMainMode, scePadSetActAlign,
 * scePadSetButtonInfo, scePadSetVrefParam) are accepted at once and
 * complete a few fields later, as padman's do; meanwhile reqState is BUSY
 * and scePadGetState says EXECCMD.
 *
 * The data bytes are a DualShock 2's:
 *   [0] 0x00 (valid), [1] mode ID: 0x41 digital, 0x73 analog, 0x79 analog
 *   with pressure; the low nibble is the number of halfwords that follow
 *   [2] select, L3, R3, start, up, right, down, left (bits 0-7, 0 = down)
 *   [3] L2, R2, L1, R1, triangle, circle, cross, square (bits 0-7, 0 = down)
 *   [4] right stick X, [5] right Y, [6] left X, [7] left Y (0x80 centre,
 *       0 left/up, 0xFF right/down); not in digital mode
 *   [8..19] pressure (0 to 0xFF) of right, left, up, down, triangle,
 *       circle, cross, square, L1, R1, L2, R2; only in 0x79
 * and length (what scePadRead returns) is 2 + 2 * (ID & 0xF): 4, 8 or 20.
 *
 * Defines game names (scePad*); must not include <windows.h>.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#undef NULL
#include "sdk/libpad.h"
#include "grx_pad.h"
#include "grx_input.h"
#include "grx_skip.h"
#include "grx_platform.h"
#include "grx_video.h"
#include "grx_state_parts.h"

#define NPORTS 2

/* scePadGetState */
#define STATE_DISCONN 0
#define STATE_FINDPAD 1
#define STATE_FINDCTP1 2
#define STATE_EXECCMD 5
#define STATE_STABLE 6
/* reqState */
#define REQ_COMPLETE 0
#define REQ_FAILED 1
#define REQ_BUSY 2

/* Fields a request takes, and a newly found pad until it is stable */
#define REQ_FIELDS 3
#define FIND_FIELDS 4

enum { REQ_NONE, REQ_MAINMODE, REQ_ACTALIGN, REQ_BUTTONINFO, REQ_VREF };

/* DualShock 2 data bits (bytes 2 and 3 as one big-endian word, 1 = down) */
#define B_SELECT 0x0100
#define B_L3 0x0200
#define B_R3 0x0400
#define B_START 0x0800
#define B_UP 0x1000
#define B_RIGHT 0x2000
#define B_DOWN 0x4000
#define B_LEFT 0x8000
#define B_L2 0x0001
#define B_R2 0x0002
#define B_L1 0x0004
#define B_R1 0x0008
#define B_TRIANGLE 0x0010
#define B_CIRCLE 0x0020
#define B_CROSS 0x0040
#define B_SQUARE 0x0080

typedef struct {
    int open;
    ScePadData *area;   /* the caller's DMA area (two frames) */
    ScePadData d;       /* what padman would write there */
    int connected;
    int find_timer;
    int req, req_timer, req_arg, req_arg2;
    int press;          /* pressure data enabled (scePadEnterPressMode) */
    u8 align[6];
    u8 act[6];
    SDL_Gamepad *gp;
    SDL_JoystickID gp_id;
    u8 rumble_small, rumble_big;
} Port;

static Port ports[NPORTS];
static int pad_inited;

s32 _scePadInitDone;

/* ---------------------------------------------------------------------- */
/* Gamepads */

static void attach(SDL_JoystickID id) {
    int i;
    SDL_Gamepad *gp;
    for (i = 0; i < NPORTS; i++) {
        if (ports[i].gp != NULL && ports[i].gp_id == id) {
            return;
        }
    }
    for (i = 0; i < NPORTS; i++) {
        if (ports[i].gp == NULL) {
            break;
        }
    }
    if (i == NPORTS) {
        return;
    }
    gp = SDL_OpenGamepad(id);
    if (gp == NULL) {
        grx_log("pad: can't open gamepad %u: %s", (unsigned)id, SDL_GetError());
        return;
    }
    ports[i].gp = gp;
    ports[i].gp_id = id;
    grx_log("pad: port %d: %s", i + 1, SDL_GetGamepadName(gp));
}

static void detach(SDL_JoystickID id) {
    int i;
    for (i = 0; i < NPORTS; i++) {
        if (ports[i].gp != NULL && ports[i].gp_id == id) {
            grx_log("pad: port %d: gamepad removed", i + 1);
            SDL_CloseGamepad(ports[i].gp);
            ports[i].gp = NULL;
            ports[i].gp_id = 0;
        }
    }
}

void grx_pad_init(void) {
    SDL_JoystickID *ids;
    int n = 0, i;
    if (pad_inited) {
        return;
    }
    pad_inited = 1;
    grx_input_init();
    ids = SDL_GetGamepads(&n);
    for (i = 0; ids != NULL && i < n; i++) {
        attach(ids[i]);
    }
    SDL_free(ids);
}

void grx_pad_shutdown(void) {
    int i;
    for (i = 0; i < NPORTS; i++) {
        if (ports[i].gp != NULL) {
            SDL_CloseGamepad(ports[i].gp);
            ports[i].gp = NULL;
        }
    }
    pad_inited = 0;
}

void grx_pad_event(const union SDL_Event *ev) {
    if (ev->type == SDL_EVENT_GAMEPAD_ADDED) {
        attach(ev->gdevice.which);
    } else if (ev->type == SDL_EVENT_GAMEPAD_REMOVED) {
        detach(ev->gdevice.which);
    }
}

/* ---------------------------------------------------------------------- */
/* Input */

/* Which key and gamepad input each button is on: grx_input.h */

/* A trigger counts as pressed past this (of 32767) */
#define TRIGGER_DOWN 8000

/* A gamepad input (grx_input.h) is down: its analog level (0-255), else 0 */
static int pad_level(SDL_Gamepad *gp, int code) {
    if (code == GRX_IN_LT || code == GRX_IN_RT) {
        Sint16 v = SDL_GetGamepadAxis(gp, code == GRX_IN_LT ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                                                            : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        return v > TRIGGER_DOWN ? (u8)(v >> 7) : 0;
    }
    return code >= 0 && code < SDL_GAMEPAD_BUTTON_COUNT && SDL_GetGamepadButton(gp, (SDL_GamepadButton)code) ? 0xFF
                                                                                                          : 0;
}

/* Port 1's last input: 1 a key, 0 the gamepad (grx_pad_keyboard) */
static int keyboard_last;

/* A stick pushed past this (of 32767) counts as the gamepad in use */
#define STICK_USED 16000

/* Port 1's keys and gamepad for the Configurations page's capture
   (grx_input_poll), and which was used last: 1 while the game should see
   nothing pressed */
static int capture_poll(SDL_Gamepad *gp, const bool *keys) {
    int key = GRX_IN_NONE, pad = GRX_IN_NONE, any = 0, i;
    for (i = 0; keys != NULL && i < SDL_SCANCODE_COUNT; i++) {
        if (keys[i]) {
            any = 1;
            if (key == GRX_IN_NONE) {
                key = i;
            }
        }
    }
    for (i = 0; gp != NULL && i < SDL_GAMEPAD_BUTTON_COUNT + 2; i++) {
        int code = i < SDL_GAMEPAD_BUTTON_COUNT ? i : i == SDL_GAMEPAD_BUTTON_COUNT ? GRX_IN_LT : GRX_IN_RT;
        if (pad_level(gp, code)) {
            any = 1;
            if (pad == GRX_IN_NONE) {
                pad = code;
            }
        }
    }
    if (pad != GRX_IN_NONE) {
        keyboard_last = 0;
    } else if (key != GRX_IN_NONE) {
        keyboard_last = 1;
    }
    for (i = 0; gp != NULL && i < 4; i++) {
        Sint16 v = SDL_GetGamepadAxis(gp, i == 0 ? SDL_GAMEPAD_AXIS_LEFTX : i == 1 ? SDL_GAMEPAD_AXIS_LEFTY
                                          : i == 2 ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_RIGHTY);
        if (v > STICK_USED || v < -STICK_USED) {
            keyboard_last = 0;
        }
    }
    return grx_input_poll(key, pad, any, keys != NULL && keys[SDL_SCANCODE_ESCAPE]);
}

int grx_pad_keyboard(void) {
    return ports[0].gp == NULL || keyboard_last;
}

/* Pressure bytes 8..19: which button each is */
static const u16 pressure_order[12] = {B_RIGHT, B_LEFT, B_UP, B_DOWN, B_TRIANGLE, B_CIRCLE,
                                       B_CROSS, B_SQUARE, B_L1, B_R1, B_L2, B_R2};

/* An SDL axis (-32768..32767) as a DualShock stick byte, with a small dead
   zone around the centre (a DualShock's own rest jitter is about that) */
static u8 stick(Sint16 v) {
    if (v > -3000 && v < 3000) {
        return 0x80;
    }
    return (u8)((v + 32768) >> 8);
}

/* GRX_PAD_SCRIPT: presses on port 1 at given fields, for runs without a
   window ("600:start 700:down 760:cross"), each held for 8 fields, or N
   with "*N" ("900:rright*60"). rleft, rright, rup and rdown push the right
   stick fully over, lleft, lright, lup and ldown the left one; a number
   after a stick's name pushes it that many percent over ("lright75"). */
#define SCRIPT_HOLD 8
enum { S_RLEFT = 1, S_RRIGHT, S_RUP, S_RDOWN, S_LLEFT, S_LRIGHT, S_LUP, S_LDOWN };
static struct {
    uint64_t field;
    u16 bit;
    u8 stick;
    u8 pct;
    unsigned hold;
} script[256];
static int nscript = -1;
static u8 script_st[4] = {0x80, 0x80, 0x80, 0x80}; /* the sticks now (right X, Y, left X, Y), as SDL's bytes */

int grx_pad_script_add(const char *name, unsigned long long field, unsigned hold) {
    static const struct {
        const char *name;
        u16 bit;
        u8 stick;
    } names[] = {
        {"up", B_UP, 0}, {"down", B_DOWN, 0}, {"left", B_LEFT, 0}, {"right", B_RIGHT, 0}, {"cross", B_CROSS, 0},
        {"circle", B_CIRCLE, 0}, {"square", B_SQUARE, 0}, {"triangle", B_TRIANGLE, 0}, {"l1", B_L1, 0},
        {"r1", B_R1, 0}, {"l2", B_L2, 0}, {"r2", B_R2, 0}, {"start", B_START, 0}, {"select", B_SELECT, 0},
        {"rleft", 0, S_RLEFT}, {"rright", 0, S_RRIGHT}, {"rup", 0, S_RUP}, {"rdown", 0, S_RDOWN},
        {"lleft", 0, S_LLEFT}, {"lright", 0, S_LRIGHT}, {"lup", 0, S_LUP}, {"ldown", 0, S_LDOWN},
    };
    int k, i, pct = 100;
    size_t len = strlen(name);
    if (nscript < 0) {
        nscript = 0;
    }
    for (k = 0; k < (int)(sizeof(names) / sizeof(names[0])); k++) {
        if (strcmp(name, names[k].name) == 0) {
            break;
        }
    }
    if (k == (int)(sizeof(names) / sizeof(names[0]))) {
        /* A stick's name and its push in percent ("lright75") */
        while (len > 0 && name[len - 1] >= '0' && name[len - 1] <= '9') {
            len--;
        }
        pct = atoi(name + len);
        if (pct > 100) {
            pct = 100;
        }
        for (k = 0; k < (int)(sizeof(names) / sizeof(names[0])); k++) {
            if (names[k].stick != 0 && strncmp(name, names[k].name, len) == 0 && names[k].name[len] == '\0'
                && name[len] != '\0') {
                break;
            }
        }
    }
    if (k == (int)(sizeof(names) / sizeof(names[0]))) {
        return -1;
    }
    /* A free entry, else one that is over */
    i = nscript;
    if (i == (int)(sizeof(script) / sizeof(script[0]))) {
        for (i = 0; i < nscript && script[i].field + script[i].hold > grx_frame_count(); i++)
            ;
        if (i == nscript) {
            return -1;
        }
    } else {
        nscript++;
    }
    script[i].field = field;
    script[i].bit = names[k].bit;
    script[i].stick = names[k].stick;
    script[i].pct = (u8)pct;
    script[i].hold = hold;
    return 0;
}

static u16 scripted(void) {
    uint64_t now = grx_frame_count();
    u16 down = 0;
    int i;

    if (nscript < 0) {
        const char *s = getenv("GRX_PAD_SCRIPT");
        nscript = 0;
        while (s != NULL && *s) {
            char name[16];
            unsigned long long f;
            unsigned hold = SCRIPT_HOLD;
            int n = 0, m = 0;
            if (sscanf(s, " %llu:%15[a-z0-9]%n", &f, name, &n) != 2 || n == 0) {
                break;
            }
            if (sscanf(s + n, "*%u%n", &hold, &m) == 1) {
                n += m;
            }
            grx_pad_script_add(name, f, hold);
            s += n;
        }
    }
    memset(script_st, 0x80, sizeof(script_st));
    for (i = 0; i < nscript; i++) {
        if (now >= script[i].field && now < script[i].field + script[i].hold) {
            /* SDL's bytes: 0x00 left and up, 0xFF right and down, at 100 */
            u8 lo = (u8)(0x80 - script[i].pct * 0x80 / 100), hi = (u8)(0x80 + script[i].pct * 0x7F / 100);
            down |= script[i].bit;
            switch (script[i].stick) {
            case S_RLEFT: script_st[0] = lo; break;
            case S_RRIGHT: script_st[0] = hi; break;
            case S_RUP: script_st[1] = lo; break;
            case S_RDOWN: script_st[1] = hi; break;
            case S_LLEFT: script_st[2] = lo; break;
            case S_LRIGHT: script_st[2] = hi; break;
            case S_LUP: script_st[3] = lo; break;
            case S_LDOWN: script_st[3] = hi; break;
            }
        }
    }
    return down;
}

/* Fill the 32 data bytes and the length for the port's current mode */
/* The port actions' gamepad inputs held (port 0), for their press */
static unsigned act_held;

static void sample(Port *p, int port) {
    u16 down = 0, keys_down;
    u8 press[12];
    u8 sticks[4] = {0x80, 0x80, 0x80, 0x80};
    u8 l2 = 0, r2 = 0;
    u8 id = p->d.modeCurId;
    int i;

    if (port == 0) {
        const bool *keys = SDL_GetKeyboardState(NULL);
        for (i = 0; keys != NULL && i < GRX_IN_COUNT; i++) {
            int k = grx_input_get(GRX_IN_KEY, i);
            if (k > 0 && k < SDL_SCANCODE_COUNT && keys[k]) {
                down |= (u16)grx_input_bit(i);
            }
        }
        /* The keypad's Enter is START too, with START on Enter */
        if (keys != NULL && keys[SDL_SCANCODE_KP_ENTER] && grx_input_get(GRX_IN_KEY, GRX_IN_START) == SDL_SCANCODE_RETURN) {
            down |= B_START;
        }
        down |= scripted();
        if (down & B_L2) {
            l2 = 0xFF;
        }
        if (down & B_R2) {
            r2 = 0xFF;
        }
    }
    keys_down = down;
    if (p->gp != NULL) {
        int a;
        /* The port's actions (grx_input.h) on their press; their inputs
           aren't the game's */
        for (a = 0; a < GRX_ACT_COUNT; a++) {
            int v = pad_level(p->gp, grx_action_get(GRX_IN_PAD, a)) != 0;
            if (port == 0 && v && !(act_held & (1u << a))) {
                grx_action_run(a);
            }
            if (port == 0) {
                act_held = v ? act_held | (1u << a) : act_held & ~(1u << a);
            }
        }
        for (i = 0; i < GRX_IN_COUNT; i++) {
            int code = grx_input_get(GRX_IN_PAD, i), v;
            for (a = 0; a < GRX_ACT_COUNT && grx_action_get(GRX_IN_PAD, a) != code; a++)
                ;
            v = a == GRX_ACT_COUNT ? pad_level(p->gp, code) : 0;
            if (v) {
                down |= (u16)grx_input_bit(i);
                /* L2 and R2's pressure: a trigger's level */
                if (i == GRX_IN_L2 && v > l2) {
                    l2 = (u8)v;
                } else if (i == GRX_IN_R2 && v > r2) {
                    r2 = (u8)v;
                }
            }
        }
        sticks[0] = stick(SDL_GetGamepadAxis(p->gp, SDL_GAMEPAD_AXIS_RIGHTX));
        sticks[1] = stick(SDL_GetGamepadAxis(p->gp, SDL_GAMEPAD_AXIS_RIGHTY));
        sticks[2] = stick(SDL_GetGamepadAxis(p->gp, SDL_GAMEPAD_AXIS_LEFTX));
        sticks[3] = stick(SDL_GetGamepadAxis(p->gp, SDL_GAMEPAD_AXIS_LEFTY));
    }
    if (port == 0) {
        /* The keyboard's directions also push the left stick: the field
           walks by the stick in analog mode */
        if (keys_down & (B_UP | B_DOWN | B_LEFT | B_RIGHT)) {
            if (sticks[2] == 0x80 && sticks[3] == 0x80) {
                sticks[2] = (keys_down & B_LEFT) ? 0x00 : (keys_down & B_RIGHT) ? 0xFF : 0x80;
                sticks[3] = (keys_down & B_UP) ? 0x00 : (keys_down & B_DOWN) ? 0xFF : 0x80;
            }
        }
        for (i = 0; i < 4; i++) {
            if (script_st[i] != 0x80) {
                sticks[i] = script_st[i];
            }
        }
        /* The scene skip (grx_skip.h) takes START in a scene, and the
           buttons while it skips (a scene centres the sticks itself) */
        down = (u16)grx_skip_input(keys_down, (u16)(down & ~keys_down));
        /* The Configurations page waiting for a press to put on a button */
        if (capture_poll(p->gp, SDL_GetKeyboardState(NULL))) {
            down = 0;
            memset(sticks, 0x80, sizeof(sticks));
        }
    }
    if (id == 0x41) {
        /* Digital mode: no stick buttons */
        down &= (u16)~(B_L3 | B_R3);
    }
    for (i = 0; i < 12; i++) {
        press[i] = (down & pressure_order[i]) ? 0xFF : 0;
    }
    press[10] = (down & B_L2) ? (l2 ? l2 : 0xFF) : 0;
    press[11] = (down & B_R2) ? (r2 ? r2 : 0xFF) : 0;

    memset(p->d.data, 0, sizeof(p->d.data));
    p->d.data[0] = 0;
    p->d.data[1] = id;
    p->d.data[2] = (u8)~(down >> 8);
    p->d.data[3] = (u8)~down;
    if (id != 0x41) {
        memcpy(&p->d.data[4], sticks, 4);
    }
    if (id == 0x79) {
        memcpy(&p->d.data[8], press, 12);
    }
    p->d.length = 2 + 2 * (id & 0xF);
}

/* ---------------------------------------------------------------------- */
/* padman */

/* A DualShock 2 as padman describes it once found */
static void found(Port *p) {
    ScePadData *d = &p->d;
    u8 *raw = (u8 *)d;
    d->state = STATE_STABLE;
    d->currentTask = 1;
    d->reqState = REQ_COMPLETE;
    d->modeConfig = 2;
    d->model = 3;
    d->nrOfModes = 2;
    d->modeTable[0] = 4;
    d->modeTable[1] = 7;
    d->modeTable[2] = d->modeTable[3] = 0;
    /* It powers up digital */
    d->modeCurOffs = 0;
    d->modeCurId = 0x41;
    d->lock = 0;
    p->press = 0;
    /* Two actuators (the small motor: on/off; the big one: 0-255) and one
       combination; scePadInfoAct's function, sub, size, current */
    d->nrOfActuators = 2;
    d->numActComb = 1;
    memset(d->actData, 0, sizeof(d->actData));
    d->actData[0] = 1, d->actData[1] = 0, d->actData[2] = 0, d->actData[3] = 0x31;
    d->actData[4] = 1, d->actData[5] = 1, d->actData[6] = 8, d->actData[7] = 0x6E;
    d->actData[16] = 2, d->actData[17] = 0, d->actData[18] = 1, d->actData[19] = 0;
    /* scePadGetButtonMask: all 18 pressure-sensitive bits */
    raw[0x79] = 0xFF;
    raw[0x7A] = 0xFF;
    raw[0x7B] = 0x03;
    raw[0x7C] = 0x00;
    memset(p->align, 0xFF, sizeof(p->align));
    memset(p->act, 0, sizeof(p->act));
}

static void lost(Port *p) {
    p->d.state = STATE_DISCONN;
    p->d.currentTask = 0;
    p->d.length = 0;
    memset(p->d.data, 0xFF, sizeof(p->d.data));
    p->req = REQ_NONE;
    p->req_timer = 0;
}

static void rumble(Port *p) {
    u8 small = 0, big = 0;
    int i;
    for (i = 0; i < 6; i++) {
        if (p->align[i] == 0) {
            small = p->act[i];
        } else if (p->align[i] == 1) {
            big = p->act[i];
        }
    }
    if (p->gp == NULL) {
        return;
    }
    if (small == 0 && big == 0) {
        if (p->rumble_small || p->rumble_big) {
            SDL_RumbleGamepad(p->gp, 0, 0, 0);
        }
    } else {
        /* Refreshed each frame while the game drives it */
        SDL_RumbleGamepad(p->gp, (Uint16)(big * 257), small ? 0xFFFF : 0, 250);
    }
    p->rumble_small = small;
    p->rumble_big = big;
}

static void complete(Port *p) {
    ScePadData *d = &p->d;
    int ok = 1;
    switch (p->req) {
    case REQ_MAINMODE:
        d->modeCurOffs = (u8)(p->req_arg != 0);
        d->lock = (u8)p->req_arg2;
        if (d->modeCurOffs == 0) {
            p->press = 0;
        }
        break;
    case REQ_BUTTONINFO:
        if (d->modeCurOffs == 0) {
            ok = 0; /* no pressure data in digital mode */
        } else {
            p->press = p->req_arg != 0;
        }
        break;
    case REQ_ACTALIGN:
    case REQ_VREF:
        break;
    }
    d->modeCurId = d->modeCurOffs == 0 ? 0x41 : p->press ? 0x79 : 0x73;
    d->reqState = ok ? REQ_COMPLETE : REQ_FAILED;
    p->req = REQ_NONE;
}

static void publish(Port *p) {
    if (p->area != NULL) {
        p->area[0] = p->d;
        p->area[1] = p->d;
    }
}

void grx_pad_update(void) {
    int i;
    for (i = 0; i < NPORTS; i++) {
        Port *p = &ports[i];
        int connected = i == 0 || p->gp != NULL;
        if (!p->open) {
            continue;
        }
        if (!connected) {
            if (p->d.state != STATE_DISCONN) {
                lost(p);
            }
        } else if (p->d.state == STATE_DISCONN) {
            p->d.state = STATE_FINDPAD;
            p->d.currentTask = 0;
            p->find_timer = FIND_FIELDS;
        } else if (p->d.state == STATE_FINDPAD) {
            if (--p->find_timer <= 0) {
                found(p);
            }
        }
        if (p->d.state == STATE_STABLE) {
            if (p->req != REQ_NONE && --p->req_timer <= 0) {
                complete(p);
            }
            sample(p, i);
        }
        p->d.frame++;
        publish(p);
    }
}

/* A request padman accepts if the pad is there and not busy */
static s32 request(s32 port, s32 slot, int kind, int arg, int arg2) {
    Port *p;
    if (port < 0 || port >= NPORTS || slot != 0 || !ports[port].open) {
        return 0;
    }
    p = &ports[port];
    if (p->d.state != STATE_STABLE || p->req != REQ_NONE) {
        return 0;
    }
    p->req = kind;
    p->req_arg = arg;
    p->req_arg2 = arg2;
    p->req_timer = REQ_FIELDS;
    return 1;
}

void grx_pad_describe(int port, char *buf, int n) {
    Port *p;
    int i, k;
    if (port < 0 || port >= NPORTS) {
        snprintf(buf, n, "no port %d", port);
        return;
    }
    p = &ports[port];
    k = snprintf(buf, n, "port %d: %s state %d req %d id %02X len %d%s |", port + 1, p->open ? "open" : "closed",
                 p->d.state, p->d.reqState, p->d.modeCurId, (int)p->d.length, p->gp ? " (gamepad)" : "");
    for (i = 0; i < 20 && k > 0 && k < n; i++) {
        k += snprintf(buf + k, n - k, " %02X", p->d.data[i]);
    }
}

int grx_pad_xbox_like(int port) {
    if (port < 0 || port >= NPORTS || ports[port].gp == NULL) {
        return -1;
    }
    switch (SDL_GetGamepadType(ports[port].gp)) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE:
    case SDL_GAMEPAD_TYPE_STANDARD: /* a generic XInput pad, laid out as an Xbox one */
        return 1;
    default:
        return 0;
    }
}

unsigned grx_pad_raw(int port) {
    const Port *p;
    uint32_t b, lx = 0x80, ly = 0x80;
    if (port < 0 || port >= NPORTS || !ports[port].open || ports[port].d.state != STATE_STABLE) {
        return 0x80800000u;
    }
    p = &ports[port];
    b = (((uint32_t)p->d.data[2] << 8) | p->d.data[3]) ^ 0xFFFFu;
    if (p->d.modeCurId != 0x41) {
        lx = p->d.data[6];
        ly = p->d.data[7];
    }
    return b | lx << 16 | ly << 24;
}

/* ---------------------------------------------------------------------- */
/* libpad */

static ScePadData *dma(s32 port, s32 slot) {
    if (port < 0 || port >= NPORTS || slot != 0 || !ports[port].open) {
        return NULL;
    }
    return &ports[port].d;
}

s32 scePadInit(s32 mode) {
    int i;
    (void)mode;
    _scePadInitDone = 1;
    for (i = 0; i < NPORTS; i++) {
        ports[i].open = 0;
    }
    return 1;
}

s32 scePadEnd(void) {
    _scePadInitDone = 0;
    return 1;
}

s32 scePadPortOpen(s32 port, s32 slot, void *area) {
    Port *p;
    if (port < 0 || port >= NPORTS || slot != 0 || area == NULL) {
        return 0;
    }
    p = &ports[port];
    memset(&p->d, 0, sizeof(p->d));
    p->open = 1;
    p->area = (ScePadData *)area;
    p->req = REQ_NONE;
    lost(p);
    publish(p);
    return 1;
}

s32 scePadPortClose(s32 port, s32 slot) {
    if (dma(port, slot) == NULL) {
        return 0;
    }
    ports[port].open = 0;
    ports[port].area = NULL;
    return 1;
}

s32 scePadGetPortMax(void) {
    return NPORTS;
}

s32 scePadGetSlotMax(s32 port) {
    (void)port;
    return 1;
}

s32 scePadGetModVersion(void) {
    return 0x400; /* padman 4.0, what scePadInit checks for */
}

s32 scePadGetFrameCount(s32 port, s32 slot) {
    ScePadData *d = dma(port, slot);
    return d == NULL ? 0 : d->frame;
}

s32 scePadRead(s32 port, s32 slot, u8 *buf) {
    ScePadData *d = dma(port, slot);
    if (d == NULL) {
        return 0;
    }
    memcpy(buf, d->data, d->length);
    return d->length;
}

u8 scePadGetState(s32 port, s32 slot) {
    ScePadData *d = dma(port, slot);
    if (d == NULL) {
        return 0x63;
    }
    if (d->state == STATE_STABLE && d->reqState == REQ_BUSY) {
        return STATE_EXECCMD;
    }
    return d->state;
}

s32 scePadSetReqState(s32 port, s32 slot, s32 state) {
    ScePadData *d = dma(port, slot);
    if (d == NULL) {
        return 0;
    }
    d->reqState = (u8)state;
    return 1;
}

u8 scePadGetReqState(s32 port, s32 slot) {
    ScePadData *d = dma(port, slot);
    return d == NULL ? 0 : d->reqState;
}

void scePadStateIntToStr(u32 state, char *str) {
    strcpy(str, state < 8 ? _scePadStateStrTable[state] : "");
}

void scePadReqIntToStr(u32 state, u8 *str) {
    strcpy((char *)str, state < 3 ? _scePadReqStateStrTable[state] : "");
}

s32 scePadInfoAct(s32 port, s32 slot, s32 actno, s32 term) {
    ScePadData *d = dma(port, slot);
    if (d == NULL || d->currentTask != 1 || d->modeConfig < 2 || actno >= d->nrOfActuators) {
        return 0;
    }
    if (actno == -1) {
        return d->nrOfActuators;
    }
    if (actno < 0 || term < 1 || term > 4) {
        return 0;
    }
    return d->actData[actno * 4 + term - 1];
}

s32 scePadInfoComb(s32 port, s32 slot, s32 listno, s32 offs) {
    ScePadData *d = dma(port, slot);
    if (d == NULL || d->currentTask != 1 || d->modeConfig < 2) {
        return 0;
    }
    if (listno == -1) {
        return d->numActComb;
    }
    if (listno < 0 || listno >= d->numActComb || offs < -1 || offs > 2) {
        return 0;
    }
    return d->actData[16 + listno * 4 + offs + 1];
}

s32 scePadInfoMode(s32 port, s32 slot, s32 term, s32 offs) {
    ScePadData *d = dma(port, slot);
    if (d == NULL || d->currentTask != 1 || d->reqState == REQ_BUSY) {
        return 0;
    }
    switch (term) {
    case 1: /* InfoModeCurID */
        return d->modeCurId == 0xF3 ? 0 : d->modeCurId >> 4;
    case 2: /* InfoModeCurExID */
        return d->modeConfig == 1 ? 0 : d->modeTable[d->modeCurOffs];
    case 3: /* InfoModeCurExOffs */
        return d->modeConfig == 1 ? 0 : d->modeCurOffs;
    case 4: /* InfoModeIdTable */
        if (d->modeConfig == 1) {
            return 0;
        }
        if (offs == -1) {
            return d->nrOfModes;
        }
        return offs >= 0 && offs < d->nrOfModes ? d->modeTable[offs] : 0;
    }
    return 0;
}

s32 scePadSetMainMode(s32 port, s32 slot, s32 offs, s32 lock) {
    s32 r = request(port, slot, REQ_MAINMODE, offs, lock);
    if (r == 1) {
        scePadSetReqState(port, slot, REQ_BUSY);
    }
    return r;
}

s32 scePadSetActDirect(s32 port, s32 slot, u8 *data) {
    ScePadData *d = dma(port, slot);
    if (d == NULL || d->currentTask != 1) {
        return 0;
    }
    memcpy(ports[port].act, data, 6);
    rumble(&ports[port]);
    return 1;
}

s32 scePadSetActAlign(s32 port, s32 slot, u8 *data) {
    s32 r = request(port, slot, REQ_ACTALIGN, 0, 0);
    if (r == 1) {
        memcpy(ports[port].align, data, 6);
        scePadSetReqState(port, slot, REQ_BUSY);
    }
    return r;
}

s32 scePadGetButtonMask(s32 port, s32 slot) {
    ScePadData *d = dma(port, slot);
    const u8 *raw = (const u8 *)d;
    if (d == NULL || d->currentTask != 1 || d->modeConfig < 2 || d->model < 2) {
        return 0;
    }
    return raw[0x79] | (raw[0x7A] << 8) | (raw[0x7B] << 16) | (raw[0x7C] << 24);
}

s32 scePadSetButtonInfo(s32 port, s32 slot, s32 mask) {
    s32 r = request(port, slot, REQ_BUTTONINFO, mask, 0);
    if (r == 1) {
        scePadSetReqState(port, slot, REQ_BUSY);
    }
    return r;
}

s32 scePadInfoPressMode(s32 port, s32 slot) {
    if (dma(port, slot) == NULL) {
        return 0;
    }
    return scePadGetButtonMask(port, slot) == 0x3FFFF;
}

s32 scePadEnterPressMode(s32 port, s32 slot) {
    if (dma(port, slot) == NULL) {
        return 0;
    }
    return scePadSetButtonInfo(port, slot, 0xFFF);
}

s32 scePadExitPressMode(s32 port, s32 slot) {
    if (dma(port, slot) == NULL) {
        return 0;
    }
    return scePadSetButtonInfo(port, slot, 0);
}

s32 scePadSetVrefParam(s32 port, s32 slot, ScePadVrefParam *param) {
    s32 r;
    (void)param;
    r = request(port, slot, REQ_VREF, 0, 0);
    if (r == 1) {
        scePadSetReqState(port, slot, REQ_BUSY);
    }
    return r;
}

/* ---------------------------------------------------------------------- */
/* Save states (grx_state_parts.h): libpad's ports as the game sees them;
   the gamepads behind them are this run's */
void grx_pad_state_io(GrxState *s) {
    int i;
    for (i = 0; i < NPORTS; i++) {
        SDL_Gamepad *gp = ports[i].gp;
        SDL_JoystickID id = ports[i].gp_id;
        GRX_STATE_VAR(s, ports[i]);
        ports[i].gp = gp;
        ports[i].gp_id = id;
    }
    GRX_STATE_VAR(s, _scePadInitDone);
}
