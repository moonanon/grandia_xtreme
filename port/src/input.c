/* Controller and keyboard mapping (grx_input.h, port/docs/controls.md).

   Each of the game's 16 buttons is on one gamepad input and one key. The
   defaults are the port's layout: the keyboard's arrows, Z, X, A, S, Q, W,
   E, R, Enter and Backspace, and the gamepad's buttons by position, with
   the Xbox layout's A on circle (the game's confirm) and B on cross. A map
   set on the Configurations page is kept whole in grandia.ini, next to
   grandia.exe; a kind without its section there follows the defaults, and
   so the layout. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "grx_input.h"
#include "grx_buttons.h"
#include "grx_video_opts.h"
#include "grx_platform.h"

#define CAPTURE_MS 5000

/* The DualShock bits, as pad.c's B_* */
static const unsigned bits[GRX_IN_COUNT] = {
    0x0020, 0x0040, 0x0080, 0x0010, 0x0004, 0x0008, 0x0001, 0x0002,
    0x0800, 0x0100, 0x0200, 0x0400, 0x1000, 0x4000, 0x8000, 0x2000,
};

/* grandia.ini's names for the buttons */
static const char *const ini_names[GRX_IN_COUNT] = {
    "circle", "cross", "square", "triangle", "l1", "r1", "l2", "r2",
    "start", "select", "l3", "r3", "up", "down", "left", "right",
};

static const int pad_defaults[GRX_IN_COUNT] = {
    SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, GRX_IN_LT, GRX_IN_RT,
    SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
};

static const int key_defaults[GRX_IN_COUNT] = {
    SDL_SCANCODE_X, SDL_SCANCODE_Z, SDL_SCANCODE_A, SDL_SCANCODE_S,
    SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R,
    SDL_SCANCODE_RETURN, SDL_SCANCODE_BACKSPACE, GRX_IN_NONE, GRX_IN_NONE,
    SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
};

static int custom[2];
static int map[2][GRX_IN_COUNT];

/* The capture: 0 none, 1 waiting for everything to be let go, 2 for a
   press, 3 done (until everything is let go) */
static int cap_state, cap_kind, cap_result = -1;
static Uint64 cap_start;

static int get_default(int kind, int b) {
    if (kind == GRX_IN_KEY) {
        return key_defaults[b];
    }
    /* The Xbox layout: A, the south button, confirms (circle) */
    if (grx_buttons_xbox() && (b == GRX_IN_CIRCLE || b == GRX_IN_CROSS)) {
        return b == GRX_IN_CIRCLE ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST;
    }
    return pad_defaults[b];
}

int grx_input_get(int kind, int b) {
    return custom[kind] ? map[kind][b] : get_default(kind, b);
}

unsigned grx_input_bit(int b) {
    return bits[b];
}

static const char *pad_name(int code) {
    int xbox = grx_buttons_xbox();
    switch (code) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return xbox ? "A" : "Cross";
    case SDL_GAMEPAD_BUTTON_EAST: return xbox ? "B" : "Circle";
    case SDL_GAMEPAD_BUTTON_WEST: return xbox ? "X" : "Square";
    case SDL_GAMEPAD_BUTTON_NORTH: return xbox ? "Y" : "Triangle";
    case SDL_GAMEPAD_BUTTON_BACK: return xbox ? "Back" : "Select";
    case SDL_GAMEPAD_BUTTON_GUIDE: return "Guide";
    case SDL_GAMEPAD_BUTTON_START: return "Start";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return xbox ? "LS" : "L3";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return xbox ? "RS" : "R3";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return xbox ? "LB" : "L1";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return xbox ? "RB" : "R1";
    case GRX_IN_LT: return xbox ? "LT" : "L2";
    case GRX_IN_RT: return xbox ? "RT" : "R2";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-pad Up";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-pad Down";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-pad Left";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-pad Right";
    case SDL_GAMEPAD_BUTTON_MISC1: return xbox ? "Share" : "Mute";
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: return "Paddle R1";
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: return "Paddle L1";
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: return "Paddle R2";
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: return "Paddle L2";
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: return "Touchpad";
    }
    return code == GRX_IN_NONE ? "-" : "Button";
}

int grx_input_icon(int code) {
    switch (code) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return grx_buttons_xbox() ? 0 : 1;
    case SDL_GAMEPAD_BUTTON_EAST: return grx_buttons_xbox() ? 1 : 0;
    case SDL_GAMEPAD_BUTTON_WEST: return 2;
    case SDL_GAMEPAD_BUTTON_NORTH: return 3;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return 4;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return 5;
    case GRX_IN_LT: return 6;
    case GRX_IN_RT: return 7;
    }
    return -1;
}

const char *grx_input_name(int kind, int code) {
    static char buf[15];
    const char *s;
    if (kind == GRX_IN_PAD) {
        return pad_name(code);
    }
    if (code == GRX_IN_NONE) {
        return "-";
    }
    s = SDL_GetScancodeName((SDL_Scancode)code);
    if (s == NULL || !*s) {
        snprintf(buf, sizeof(buf), "Key %d", code);
        return buf;
    }
    snprintf(buf, sizeof(buf), "%s", s);
    return buf;
}

/* ---------------------------------------------------------------------- */
/* grandia.ini */

static char *ini_path(void) {
    static char path[1024];
    const char *base = SDL_GetBasePath();
    snprintf(path, sizeof(path), "%sgrandia.ini", base != NULL ? base : "");
    return path;
}

static const char *code_to_ini(int kind, int code) {
    const char *s = NULL;
    if (code == GRX_IN_NONE) {
        return "none";
    }
    if (kind == GRX_IN_KEY) {
        s = SDL_GetScancodeName((SDL_Scancode)code);
    } else if (code == GRX_IN_LT) {
        s = "lefttrigger";
    } else if (code == GRX_IN_RT) {
        s = "righttrigger";
    } else {
        s = SDL_GetGamepadStringForButton((SDL_GamepadButton)code);
    }
    return s != NULL && *s ? s : "none";
}

static int ini_to_code(int kind, const char *s) {
    if (SDL_strcasecmp(s, "none") == 0) {
        return GRX_IN_NONE;
    }
    if (kind == GRX_IN_KEY) {
        SDL_Scancode sc = SDL_GetScancodeFromName(s);
        return sc == SDL_SCANCODE_UNKNOWN ? -2 : (int)sc;
    }
    if (SDL_strcasecmp(s, "lefttrigger") == 0) {
        return GRX_IN_LT;
    }
    if (SDL_strcasecmp(s, "righttrigger") == 0) {
        return GRX_IN_RT;
    }
    {
        SDL_GamepadButton gb = SDL_GetGamepadButtonFromString(s);
        return gb == SDL_GAMEPAD_BUTTON_INVALID ? -2 : (int)gb;
    }
}

static void save(void) {
    const char *path = ini_path();
    FILE *f = fopen(path, "w");
    int kind, b;
    if (f == NULL) {
        grx_log("controls: can't write %s", path);
        return;
    }
    fputs("; Grandia Xtreme port: the controls and the video settings, as set on the\n"
          "; Configurations page (port/docs/controls.md). A section left out is the\n"
          "; defaults.\n",
          f);
    for (kind = 0; kind < 2; kind++) {
        if (!custom[kind]) {
            continue;
        }
        fprintf(f, "\n[%s]\n", kind == GRX_IN_PAD ? "controller" : "keyboard");
        for (b = 0; b < GRX_IN_COUNT; b++) {
            fprintf(f, "%s = %s\n", ini_names[b], code_to_ini(kind, map[kind][b]));
        }
    }
    grx_opts_ini_write(f);
    fclose(f);
}

void grx_settings_save(void) {
    save();
}

static char *trim(char *s) {
    char *e;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
        *--e = 0;
    }
    return s;
}

void grx_input_init(void) {
    const char *path = ini_path();
    FILE *f = fopen(path, "r");
    char line[256];
    int kind = -1, b, video = 0;
    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char *s = trim(line), *eq;
        int code;
        if (*s == ';' || *s == '#' || *s == 0) {
            continue;
        }
        if (*s == '[') {
            video = SDL_strncasecmp(s, "[video]", 7) == 0;
            kind = SDL_strncasecmp(s, "[controller]", 12) == 0 ? GRX_IN_PAD
                 : SDL_strncasecmp(s, "[keyboard]", 10) == 0 ? GRX_IN_KEY : -1;
            if (kind >= 0 && !custom[kind]) {
                /* A section starts from the defaults: a button it leaves
                   out keeps its own */
                for (b = 0; b < GRX_IN_COUNT; b++) {
                    map[kind][b] = get_default(kind, b);
                }
                custom[kind] = 1;
            }
            continue;
        }
        eq = strchr(s, '=');
        if (video && eq != NULL) {
            *eq = 0;
            s = trim(s);
            if (!grx_opts_ini_read(s, trim(eq + 1))) {
                grx_log("video: %s: \"%s = %s\" not understood", path, s, trim(eq + 1));
            }
            continue;
        }
        if (kind < 0 || eq == NULL) {
            continue;
        }
        *eq = 0;
        s = trim(s);
        for (b = 0; b < GRX_IN_COUNT && SDL_strcasecmp(s, ini_names[b]) != 0; b++)
            ;
        code = ini_to_code(kind, trim(eq + 1));
        if (b == GRX_IN_COUNT || code == -2) {
            grx_log("controls: %s: \"%s = %s\" not understood", path, s, trim(eq + 1));
            continue;
        }
        map[kind][b] = code;
    }
    fclose(f);
    grx_log("controls: %s%s%s", path, custom[GRX_IN_PAD] ? " (controller)" : "",
            custom[GRX_IN_KEY] ? " (keyboard)" : "");
}

void grx_input_set(int kind, int b, int code) {
    int o;
    if (!custom[kind]) {
        for (o = 0; o < GRX_IN_COUNT; o++) {
            map[kind][o] = get_default(kind, o);
        }
        custom[kind] = 1;
    }
    for (o = 0; o < GRX_IN_COUNT; o++) {
        if (o != b && code != GRX_IN_NONE && map[kind][o] == code) {
            map[kind][o] = map[kind][b];
        }
    }
    map[kind][b] = code;
    save();
}

void grx_input_reset(int kind) {
    custom[kind] = 0;
    save();
}

/* ---------------------------------------------------------------------- */
/* Capture */

void grx_input_capture(int kind) {
    cap_kind = kind;
    cap_state = 1;
    cap_result = -2;
    cap_start = SDL_GetTicks();
}

int grx_input_captured(void) {
    return cap_result;
}

/* The port's own keys are not for the game */
static int reserved_key(int key) {
    return key == SDL_SCANCODE_ESCAPE || key == SDL_SCANCODE_TAB || key == SDL_SCANCODE_F9 || key == SDL_SCANCODE_F10 ||
           key == SDL_SCANCODE_F11 || (key >= SDL_SCANCODE_F1 && key <= SDL_SCANCODE_F4);
}

int grx_input_poll(int key, int pad, int any, int escape) {
    switch (cap_state) {
    case 1:
        if (!any) {
            cap_state = 2;
        }
        break;
    case 2:
        if (escape || SDL_GetTicks() - cap_start > CAPTURE_MS) {
            cap_result = -1;
            cap_state = 3;
        } else if (cap_kind == GRX_IN_KEY && key != GRX_IN_NONE && !reserved_key(key)) {
            cap_result = key;
            cap_state = 3;
        } else if (cap_kind == GRX_IN_PAD && pad != GRX_IN_NONE) {
            cap_result = pad;
            cap_state = 3;
        }
        break;
    case 3:
        if (!any) {
            cap_state = 0;
        }
        break;
    }
    return cap_state != 0;
}
