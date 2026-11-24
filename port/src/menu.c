/* The port's own menu (grx_menu.h): Esc opens it anywhere and the game
   waits meanwhile (video.c holds the fields as the debug server's freeze
   does). Two tabs, Tab (L1/R1) between them: Display, the Display page's
   settings (grx_video_opts.h), and Controls, the port's actions and the
   game's buttons, each "what it does   key   PS2 button" (grx_input.h:
   Enter on one, then a key or a gamepad button; the list scrolls). Up/Down
   pick a row, Left/Right change it, Enter confirms, Esc closes; with a
   gamepad the D-pad, the south button and the east button or START.
   All saved to grandia.ini.

   The picture: the scene skip prompt's stroke font and panel
   (skip_overlay.c's grx_menu_art), rendered again when the text or the
   size changes, faded in over the game's picture by the presenter. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "grx_gl.h"
#include "grx_input.h"
#include "grx_menu.h"
#include "grx_platform.h"
#include "grx_skip.h"
#include "grx_video_opts.h"

#include <stdint.h>
#ifndef GL_ONE_MINUS_SRC_ALPHA
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#endif

/* A row: a Display setting, a port action (grx_input.h), one of the
   game's buttons, or Reset / Close */
enum { R_SETTING, R_ACTION, R_BUTTON, R_RESET, R_CLOSE, R_TABS };
typedef struct {
    int kind, id;
} Row;
enum { SET_RES, SET_FPS, SET_ASPECT };
enum { TAB_DISPLAY, TAB_CONTROLS, TABS };
#define MAX_ROWS 24
#define VISIBLE_ROWS 9
static const char *const tab_names[TABS] = {"Display", "Controls"};

/* The game's buttons by what they do (as its Configurations page) */
static const char *const button_roles[GRX_IN_COUNT] = {
    "Confirm", "Cancel and ready", "Map zoom", "Menu", "Turn camera left", "Turn camera right", "Ready",
    "Reset camera", "Skip scene", "First person", NULL, NULL, "Up", "Down", "Left", "Right",
};

static int tab_rows(int t, Row *r) {
    int n = 0, i;
    if (t == TAB_DISPLAY) {
        for (i = SET_RES; i <= SET_ASPECT; i++) {
            r[n++] = (Row){R_SETTING, i};
        }
    } else {
        for (i = 0; i < GRX_ACT_COUNT; i++) {
            r[n++] = (Row){R_ACTION, i};
        }
        for (i = 0; i < GRX_IN_COUNT; i++) {
            if (button_roles[i] != NULL) {
                r[n++] = (Row){R_BUTTON, i};
            }
        }
        r[n++] = (Row){R_RESET, 0};
    }
    r[n++] = (Row){R_CLOSE, 0};
    return n;
}

/* cursor: the row in the tab, -1 the tabs themselves; top: the first row shown; waiting: 1 while
   an action or a button waits for its new key or gamepad button */
static SDL_AtomicInt open_, tab, cursor, top, version, waiting;

static Row cur_row(void) {
    Row r[MAX_ROWS];
    if (SDL_GetAtomicInt(&cursor) < 0) {
        return (Row){R_TABS, 0};
    }
    tab_rows(SDL_GetAtomicInt(&tab), r);
    return r[SDL_GetAtomicInt(&cursor)];
}

int grx_menu_open(void) {
    return SDL_GetAtomicInt(&open_);
}

void grx_menu_toggle(void) {
    int o = !SDL_GetAtomicInt(&open_);
    SDL_SetAtomicInt(&cursor, 0);
    SDL_SetAtomicInt(&top, 0);
    SDL_SetAtomicInt(&waiting, 0);
    SDL_SetAtomicInt(&open_, o);
    SDL_AddAtomicInt(&version, 1);
    grx_log("menu: %s", o ? "open (the game waits)" : "closed");
}

static void change(Row row, int dir) {
    int n;
    if (row.kind != R_SETTING) {
        return;
    }
    switch (row.id) {
    case SET_RES:
        n = (grx_opts_resolution() + dir + GRX_RES_COUNT) % GRX_RES_COUNT;
        grx_opts_set_resolution(n);
        break;
    case SET_FPS:
        grx_opts_set_frame_rate(grx_opts_frame_rate() == 60 ? 30 : 60);
        break;
    case SET_ASPECT:
        n = (grx_opts_aspect() + dir + GRX_ASPECT_COUNT) % GRX_ASPECT_COUNT;
        grx_opts_set_aspect(n);
        break;
    }
}

/* A new key (kind GRX_IN_KEY) or gamepad input for the row waiting: 1 if
   it took it */
static int assign(int kind, int code) {
    Row row = cur_row();
    if (row.kind == R_ACTION) {
        if (!(kind == GRX_IN_KEY ? grx_action_key_ok(code) : grx_action_pad_ok(code))) {
            return 0;
        }
        grx_action_set(kind, row.id, code);
    } else if (row.kind == R_BUTTON) {
        if (!(kind == GRX_IN_KEY ? grx_input_key_ok(code) : grx_input_pad_ok(code))) {
            return 0;
        }
        grx_input_set(kind, row.id, code);
    }
    SDL_SetAtomicInt(&waiting, 0);
    return 1;
}

/* From the keyboard or a gamepad */
enum { NAV_UP, NAV_DOWN, NAV_LEFT, NAV_RIGHT, NAV_OK, NAV_BACK, NAV_TAB_PREV, NAV_TAB_NEXT };

static void nav(int n) {
    Row rows[MAX_ROWS], row;
    int c = SDL_GetAtomicInt(&cursor), nrows = tab_rows(SDL_GetAtomicInt(&tab), rows), t;
    row = c >= 0 ? rows[c] : (Row){R_TABS, 0};
    /* On the tabs, Left/Right go between them */
    if (c < 0 && (n == NAV_LEFT || n == NAV_RIGHT)) {
        n = n == NAV_LEFT ? NAV_TAB_PREV : NAV_TAB_NEXT;
    }
    switch (n) {
    case NAV_BACK:
        grx_menu_toggle();
        return;
    case NAV_TAB_PREV:
    case NAV_TAB_NEXT:
        SDL_SetAtomicInt(&tab, (SDL_GetAtomicInt(&tab) + (n == NAV_TAB_NEXT ? 1 : TABS - 1)) % TABS);
        c = c < 0 ? -1 : 0;
        break;
    case NAV_UP:
        c = c < 0 ? nrows - 1 : c - 1;
        break;
    case NAV_DOWN:
        c = c + 1 >= nrows ? -1 : c + 1;
        break;
    case NAV_LEFT:
        change(row, -1);
        break;
    case NAV_RIGHT:
        change(row, 1);
        break;
    case NAV_OK:
        if (row.kind == R_TABS) {
            c = 0;
            break;
        }
        if (row.kind == R_CLOSE) {
            grx_menu_toggle();
            return;
        }
        if (row.kind == R_RESET) {
            grx_action_reset();
            grx_input_reset(GRX_IN_KEY);
            grx_input_reset(GRX_IN_PAD);
        } else if (row.kind == R_ACTION || row.kind == R_BUTTON) {
            SDL_SetAtomicInt(&waiting, 1);
        } else {
            change(row, 1);
        }
        break;
    }
    /* The rows shown follow the cursor */
    t = SDL_GetAtomicInt(&top);
    t = c < 0 ? 0 : c < t ? c : c >= t + VISIBLE_ROWS ? c - VISIBLE_ROWS + 1 : t;
    t = n == NAV_TAB_PREV || n == NAV_TAB_NEXT ? 0 : t;
    SDL_SetAtomicInt(&top, t);
    SDL_SetAtomicInt(&cursor, c);
    SDL_AddAtomicInt(&version, 1);
}

int grx_menu_key(int key, int scancode) {
    if (!grx_menu_open()) {
        return 0;
    }
    if (SDL_GetAtomicInt(&waiting)) {
        /* Esc gives up; a key a game button or the port uses is refused */
        if (key == SDLK_ESCAPE) {
            SDL_SetAtomicInt(&waiting, 0);
        } else {
            assign(GRX_IN_KEY, scancode);
        }
        SDL_AddAtomicInt(&version, 1);
        return 1;
    }
    switch (key) {
    case SDLK_ESCAPE:
        nav(NAV_BACK);
        break;
    case SDLK_TAB:
        nav((SDL_GetModState() & SDL_KMOD_SHIFT) ? NAV_TAB_PREV : NAV_TAB_NEXT);
        break;
    case SDLK_UP:
        nav(NAV_UP);
        break;
    case SDLK_DOWN:
        nav(NAV_DOWN);
        break;
    case SDLK_LEFT:
        nav(NAV_LEFT);
        break;
    case SDLK_RIGHT:
        nav(NAV_RIGHT);
        break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_X:
        nav(NAV_OK);
        break;
    }
    return 1; /* the game sees no key while open */
}

int grx_menu_pad(int code) {
    if (!grx_menu_open()) {
        return 0;
    }
    if (SDL_GetAtomicInt(&waiting)) {
        if (assign(GRX_IN_PAD, code)) {
            SDL_AddAtomicInt(&version, 1);
        }
        return 1;
    }
    switch (code) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        nav(NAV_UP);
        break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        nav(NAV_DOWN);
        break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        nav(NAV_LEFT);
        break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        nav(NAV_RIGHT);
        break;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        nav(NAV_TAB_PREV);
        break;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        nav(NAV_TAB_NEXT);
        break;
    case SDL_GAMEPAD_BUTTON_SOUTH:
        nav(NAV_OK);
        break;
    case SDL_GAMEPAD_BUTTON_EAST:
    case SDL_GAMEPAD_BUTTON_START:
        nav(NAV_BACK);
        break;
    }
    return 1;
}

/* ---------------------------------------------------------------------- */
/* The presenter's side */

static const char vs_src[] = "#version 330 core\n"
                             "uniform vec4 rect;\n"
                             "out vec2 uv;\n"
                             "void main() {\n"
                             "    int i = gl_VertexID;\n"
                             "    vec2 p = vec2(i == 1 || i == 2 || i == 4 ? 1.0 : 0.0, i == 2 || i == 4 || i == 5 ? 1.0 : 0.0);\n"
                             "    gl_Position = vec4(mix(rect.xy, rect.zw, p), 0.0, 1.0);\n"
                             "    uv = vec2(p.x, 1.0 - p.y);\n"
                             "}\n";
/* The prompt's colours (skip.c): G the panel, B gold, R white; premultiplied */
static const char fs_src[] = "#version 330 core\n"
                             "uniform sampler2D art;\n"
                             "uniform float alpha;\n"
                             "in vec2 uv;\n"
                             "out vec4 color;\n"
                             "vec4 over(vec4 top, vec4 under) { return top + under * (1.0 - top.a); }\n"
                             "void main() {\n"
                             "    vec4 t = texture(art, uv);\n"
                             "    vec4 c = vec4(0.035, 0.04, 0.055, 1.0) * (0.86 * t.g);\n"
                             "    c = over(vec4(vec3(1.0, 0.8, 0.36) * t.b, t.b), c);\n"
                             "    c = over(vec4(vec3(0.96 * t.r), 0.96 * t.r), c);\n"
                             "    color = c * alpha;\n"
                             "}\n";

static int gl_state;
static GLuint prog, vao, tex;
static GLint u_rect, u_alpha;
static int drawn_version = -1, drawn_px = -1, tex_w, tex_h;
static float a_open;
static uint64_t last_ns;

static void upload(GLuint t, unsigned char *art, int w, int h);

/* "key\tPS2 button", as the menu's two columns */
static void binding(char *out, size_t n, int key, int pad) {
    char k[24];
    snprintf(k, sizeof(k), "%s", grx_input_name(GRX_IN_KEY, key));
    snprintf(out, n, "%s\t%s", k, grx_input_name_ps2(pad));
}

static void render(int px) {
    static char vals[VISIBLE_ROWS][64];
    const char *labels[VISIBLE_ROWS], *values[VISIBLE_ROWS];
    Row rows[MAX_ROWS];
    unsigned char *art;
    int t = SDL_GetAtomicInt(&tab), nrows = tab_rows(t, rows), first = SDL_GetAtomicInt(&top), i, n;

    n = nrows - first < VISIBLE_ROWS ? nrows - first : VISIBLE_ROWS;
    for (i = 0; i < n; i++) {
        Row row = rows[first + i];
        int wait = SDL_GetAtomicInt(&waiting) && SDL_GetAtomicInt(&cursor) == first + i;
        values[i] = vals[i];
        switch (row.kind) {
        case R_SETTING:
            labels[i] = row.id == SET_RES ? "Resolution" : row.id == SET_FPS ? "Frame rate" : "Aspect";
            if (row.id == SET_RES) {
                snprintf(vals[i], sizeof(vals[i]), "%s", grx_opts_resolution_name(grx_opts_resolution()));
            } else if (row.id == SET_FPS) {
                snprintf(vals[i], sizeof(vals[i]), "%d", grx_opts_frame_rate());
            } else {
                snprintf(vals[i], sizeof(vals[i]), "%s", grx_opts_aspect_name(grx_opts_aspect()));
            }
            break;
        case R_ACTION:
            labels[i] = grx_action_label(row.id);
            binding(vals[i], sizeof(vals[i]), grx_action_get(GRX_IN_KEY, row.id), grx_action_get(GRX_IN_PAD, row.id));
            break;
        case R_BUTTON:
            labels[i] = button_roles[row.id];
            binding(vals[i], sizeof(vals[i]), grx_input_get(GRX_IN_KEY, row.id), grx_input_get(GRX_IN_PAD, row.id));
            break;
        case R_RESET:
            labels[i] = "Reset controls";
            values[i] = NULL;
            break;
        default:
            labels[i] = "Close";
            values[i] = NULL;
            break;
        }
        if (wait) {
            snprintf(vals[i], sizeof(vals[i]), "Press a key or button");
            values[i] = vals[i];
        }
    }
    {
        /* On the tabs: the current one between arrows, and no row picked */
        int c = SDL_GetAtomicInt(&cursor);
        static char focused[32];
        const char *tabs[TABS];
        for (i = 0; i < TABS; i++) {
            tabs[i] = tab_names[i];
        }
        if (c < 0) {
            snprintf(focused, sizeof(focused), "< %s >", tab_names[t]);
            tabs[t] = focused;
        }
        art = grx_menu_art(px, tabs, TABS, t, labels, values, n, c < 0 ? -1 : c - first, &tex_w, &tex_h);
    }
    if (art == NULL) {
        return;
    }
    upload(tex, art, tex_w, tex_h);
}

static int gl_ready(void) {
    if (gl_state == 0) {
        prog = grx_gl_program(vs_src, fs_src, "the port menu");
        if (prog == 0) {
            gl_state = -1;
            return 0;
        }
        u_rect = glGetUniformLocation(prog, "rect");
        u_alpha = glGetUniformLocation(prog, "alpha");
        glUseProgram(prog);
        glUniform1i(glGetUniformLocation(prog, "art"), 0);
        glGenVertexArrays(1, &vao);
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl_state = 1;
    }
    return gl_state > 0;
}

static void upload(GLuint t, unsigned char *art, int w, int h) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, art);
    free(art);
}

/* A picture at (x0, y0) (GL's bottom-left origin) in a ww x wh window */
static void blit(GLuint t, float x0, float y0, float pw, float ph, int ww, int wh, float alpha) {
    glViewport(0, 0, ww, wh);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog);
    glBindVertexArray(vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t);
    glBindSampler(0, 0);
    glUniform1f(u_alpha, alpha);
    glUniform4f(u_rect, x0 / (float)ww * 2 - 1, y0 / (float)wh * 2 - 1, (x0 + pw) / (float)ww * 2 - 1,
                (y0 + ph) / (float)wh * 2 - 1);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

#ifdef GRX_DEBUG_KIT
/* ---------------------------------------------------------------------- */
/* The debug kit's keys, top right (build.py --debug); F12 hides it */

static SDL_AtomicInt hud_hidden;
static GLuint hud_tex;
static int hud_w, hud_h, hud_px = -1, hud_state = -1;

void grx_debug_hud_toggle(void) {
    SDL_SetAtomicInt(&hud_hidden, !SDL_GetAtomicInt(&hud_hidden));
}

static void debug_hud_draw(int x, int y, int w, int h, int ww, int wh) {
    static const char *const labels[] = {
        "F5  Super mode", "F6  Next map  Shift F6  Previous",
        "Ctrl F6  Next Geo Gate  Ctrl Shift F6  Previous", "F7  Noclip", "PgUp PgDn  Rise and sink",
        "Home End  Move through walls", "Shift F7  Map reveal", "F8  Record location", "F12  Hide this list",
    };
    enum { N = sizeof(labels) / sizeof(labels[0]) };
    const char *values[N] = {0};
    int px, st;

    if (SDL_GetAtomicInt(&hud_hidden) || w <= 0 || h <= 0 || !gl_ready()) {
        return;
    }
    st = grx_super() | grx_noclip() << 1 | grx_map_reveal() << 2;
    px = (int)((float)h * 0.02f + 0.5f);
    px = px < 10 ? 10 : px > 50 ? 50 : px;
    if (px != hud_px || st != hud_state) {
        unsigned char *art;
        values[0] = (st & 1) ? "on" : "off";
        values[3] = (st & 2) ? "on" : "off";
        values[6] = (st & 4) ? "on" : "off";
        {
            static const char *const title[] = {"Debug"};
            art = grx_menu_art(px, title, 1, 0, labels, values, N, -1, &hud_w, &hud_h);
        }
        if (art == NULL) {
            return;
        }
        if (hud_tex == 0) {
            glGenTextures(1, &hud_tex);
            glBindTexture(GL_TEXTURE_2D, hud_tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        upload(hud_tex, art, hud_w, hud_h);
        hud_px = px;
        hud_state = st;
    }
    blit(hud_tex, (float)(x + w - hud_w) - (float)h * 0.02f, (float)(y + h - hud_h) - (float)h * 0.02f,
         (float)hud_w, (float)hud_h, ww, wh, 0.85f);
}
#endif

void grx_menu_draw_gl(int x, int y, int w, int h, int ww, int wh) {
    int v = SDL_GetAtomicInt(&version), px;
    uint64_t now = SDL_GetTicksNS();
    float dt = last_ns != 0 ? (float)(now - last_ns) / 1e9f : 0.0f, x0, y0;

    last_ns = now;
    dt = dt > 0.1f ? 0.1f : dt;
#ifdef GRX_DEBUG_KIT
    debug_hud_draw(x, y, w, h, ww, wh);
#endif
    a_open = grx_menu_open() ? (a_open + dt * 10 > 1 ? 1 : a_open + dt * 10) : (a_open - dt * 8 < 0 ? 0 : a_open - dt * 8);
    if (a_open <= 0 || w <= 0 || h <= 0 || ww <= 0 || wh <= 0 || !gl_ready()) {
        return;
    }
    /* The text a little larger than the prompt's: capitals ~1/30 of the picture */
    px = (int)((float)h * 0.032f + 0.5f);
    px = px < 14 ? 14 : px > 90 ? 90 : px;
    if (v != drawn_version || px != drawn_px) {
        render(px);
        drawn_version = v;
        drawn_px = px;
    }
    if (tex_w <= 0) {
        return;
    }
    /* The panel in the middle, a slight rise as it opens */
    x0 = (float)x + ((float)w - (float)tex_w) / 2;
    y0 = (float)y + ((float)h - (float)tex_h) / 2 - (1 - a_open) * (float)h * 0.02f;
    blit(tex, x0, y0, (float)tex_w, (float)tex_h, ww, wh, a_open);
}
