/* Skipping cut scenes (grx_skip.h, port/docs/scene-skip.md).

   What is a cut scene. The field runs its events as scenario scripts
   (EvtScnr*): a running scenario holds the player still and drives the
   camera, the people and the messages. Talking to someone runs one too;
   those aren't cut scenes (ChrTalkUpdate starts them and keeps the person
   in chrTalkWork). Movies play through MovPlay, with sndMovieStreaming
   -1 meanwhile. The attract demo, which START already leaves, is left
   alone.

   The prompt. In a cut scene START, which a scene doesn't use, goes no
   further: it shows the prompt, held it fills the prompt's bar, and
   after 5 seconds the skip starts. Let go, the prompt stays a few
   seconds.

   The skip. Nothing of the scene's is left out: it runs as it would,
   only faster, behind a black screen:
   - the frame loop doesn't wait for the fields' times (grx_skip_fast);
   - the field runs several of its frames for each one drawn, as the
     debug mode's speed-up does (grx_skip_updates), and the GPU renderer
     draws nothing;
   - the sound driver runs by fields, with the device silent
     (grx_audio_by_fields): the play times scenes wait for count fields
     (SndUpdate's handle timers, on the vblank), so voices and music keep
     pace with them.
   Each field frame the scenario's buttons get confirm and skip
   (EVT_SCNR_KEY_*), which pass the messages as the player would, and the
   player's own buttons go nowhere.

   It ends when no scenario has run for 60 drawn frames (a scene that
   moves to another area and goes on there is skipped on: loading runs
   none), and where the player is wanted: a choice, the event's character
   select or a mini-game, the field's menus (a shop: the field draws
   frames without running FldUpdatePlay), and, as a last resort, a script
   that hasn't moved for a long time. A battle ends it too. A movie on
   the way is stopped as the game's own movie skip does (movSkipRequest);
   a skip started in a movie stops that movie only. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "grx_skip.h"
#include "grx_audio.h"
#include "grx_buttons.h"
#include "grx_gl.h"
#include "grx_pad.h"
#include "grx_platform.h"
#include "grx_video.h"

#ifndef GL_ONE_MINUS_SRC_ALPHA
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#endif

/* The game's (sys.h, the movie player) */
extern unsigned char sysState[];      /* +0x0B program: 2 FIELD */
extern unsigned char sysDemo[];       /* +0x01 mode: the attract demo */
extern signed char sndMovieStreaming; /* -1 while a movie plays */
extern unsigned int movSkipRequest;

#define PROGRAM_FIELD 2

/* DualShock bits (grx_skip_input) */
#define B_START 0x0800
/* With START, the game's soft reset (its main loop: L1 L2 R1 R2 START
   SELECT held) */
#define B_RESET 0x010F

/* EvtMsgCtl.flags: a choice is up; and skip.py's bit for the event's
   character select or a mini-game */
#define MSG_CHOICE 0x20
#define WANTS_PLAYER 0x10000
/* EVT_SCNR_KEY_CONFIRM | EVT_SCNR_KEY_SKIP */
#define KEYS_PASS 0x6000

#define HOLD_FIELDS 300      /* 5 seconds */
#define PROMPT_FIELDS 240    /* the prompt stays 4 seconds after START */
#define IDLE_FRAMES 60       /* drawn field frames without a scenario that end a skip */
#define MENU_FRAMES 150      /* drawn field frames in a menu that end it */
#define STILL_FRAMES 9000    /* drawn field frames the script may stand still (5 minutes at 30) */
#define SKIP_UPDATES 8       /* field frames run for each one drawn while skipping (GRX_SKIP_UPDATES) */

static int on = 1;
static int updates = SKIP_UPDATES;
static int trace;                      /* GRX_SKIP_TRACE: log the scenes */

/* The game thread's */
static int scene_now, scene_choice;    /* from the last grx_skip_scene */
static uint64_t scene_field;           /* the field it came in */
static int in_scene;
static unsigned raw, keys_prev, pad_prev;
static int from_keys;                  /* the last press was the keyboard's */
static int held, prompt;
static int skipping;
static int start_lock;                 /* START held back until released */
static int idle, still, menu;
static unsigned last_where;
/* Since the last field: FldUpdate drew a frame, FldUpdatePlay ran, with a
   scene, and the script moved */
static int drew, frames_ran, scene_ran, moved;

/* For the presenter */
static SDL_AtomicInt ov_show, ov_kind, ov_button, ov_progress, ov_black;

/* Fast forward (grx_turbo_cycle): the speed picked, 1 2 or 4 */
static SDL_AtomicInt turbo_pick;

void grx_skip_config(const char *arg) {
    if (arg == NULL || !*arg) {
        arg = getenv("GRX_SCENE_SKIP");
    }
    if (arg == NULL || !*arg || strcmp(arg, "on") == 0) {
        on = 1;
    } else if (strcmp(arg, "off") == 0) {
        on = 0;
    } else {
        grx_fatal("--scene-skip / GRX_SCENE_SKIP: on or off, not \"%s\"", arg);
    }
    if (getenv("GRX_SKIP_UPDATES") != NULL && atoi(getenv("GRX_SKIP_UPDATES")) >= 1) {
        updates = atoi(getenv("GRX_SKIP_UPDATES"));
    }
    trace = getenv("GRX_SKIP_TRACE") != NULL;
    if (getenv("GRX_TURBO") != NULL) {
        int t = atoi(getenv("GRX_TURBO"));
        SDL_SetAtomicInt(&turbo_pick, t >= 4 ? 4 : t >= 2 ? 2 : 1);
    }
}

static int movie_playing(void) {
    return sndMovieStreaming == -1;
}

static void stop(const char *why) {
    if (!skipping) {
        return;
    }
    grx_log("skip: done at field %llu (%s)", (unsigned long long)grx_frame_count(), why);
    skipping = 0;
    start_lock = 1;
    held = 0;
    prompt = 0;
    grx_audio_by_fields(0);
}

static void start(void) {
    held = 0;
    start_lock = 1;
    if (movie_playing()) {
        grx_log("skip: the movie, at field %llu", (unsigned long long)grx_frame_count());
        movSkipRequest = 1;
        prompt = 0;
        return;
    }
    grx_log("skip: the scene, from field %llu", (unsigned long long)grx_frame_count());
    skipping = 1;
    idle = 0;
    still = 0;
    menu = 0;
    grx_audio_by_fields(1);
}

void grx_skip_scene(int scene, unsigned msg_flags, unsigned where, uint16_t *keys) {
    scene_now = scene;
    scene_choice = scene && (msg_flags & (MSG_CHOICE | WANTS_PLAYER));
    scene_field = grx_frame_count();
    if (!skipping) {
        return;
    }
    frames_ran = 1;
    if (!scene) {
        return;
    }
    scene_ran = 1;
    if (msg_flags & MSG_CHOICE) {
        stop("a choice");
        return;
    }
    if (msg_flags & WANTS_PLAYER) {
        stop("the player's turn");
        return;
    }
    if (where != last_where) {
        moved = 1;
        last_where = where;
    }
    *keys |= KEYS_PASS;
}

unsigned grx_skip_input(unsigned keys, unsigned pad) {
    unsigned down = keys | pad;
    if (keys & ~keys_prev) {
        from_keys = 1;
    } else if (pad & ~pad_prev) {
        from_keys = 0;
    }
    keys_prev = keys;
    pad_prev = pad;
    raw = down;
    if (!on) {
        return down;
    }
    if ((down & B_RESET) == B_RESET) {
        /* The soft reset on its way: everything goes to the game */
        stop("a reset");
        held = 0;
        return down;
    }
    if (skipping) {
        return 0;
    }
    if (start_lock) {
        if (!(down & B_START)) {
            start_lock = 0;
        }
        down &= ~(unsigned)B_START;
    }
    if (in_scene) {
        down &= ~(unsigned)B_START;
    }
    return down;
}

static void publish(void) {
    int button = from_keys || grx_pad_xbox_like(0) < 0 ? 1 : grx_buttons_xbox() ? 2 : 0;
    SDL_SetAtomicInt(&ov_show, skipping || prompt > 0);
    SDL_SetAtomicInt(&ov_kind, skipping);
    SDL_SetAtomicInt(&ov_button, button);
    SDL_SetAtomicInt(&ov_progress, skipping ? 10000 : held * 10000 / HOLD_FIELDS);
    SDL_SetAtomicInt(&ov_black, skipping && !movie_playing());
}

void grx_skip_field(void) {
    int movie = movie_playing();
    int field = sysState[0x0B] == PROGRAM_FIELD;

    if (!on) {
        return;
    }
    if (skipping) {
        if (movie) {
            movSkipRequest = 1;
        } else if (!field) {
            stop("the field is over");
        } else if (drew) {
            /* Counted in drawn frames: fades go by those, and the play
               times of voices by fields, however many field frames each
               drawn one runs */
            menu = frames_ran ? 0 : menu + 1;
            idle = !frames_ran ? idle : scene_ran ? 0 : idle + 1;
            still = !scene_ran || moved ? 0 : still + 1;
            if (idle >= IDLE_FRAMES) {
                stop("the scene is over");
            } else if (menu >= MENU_FRAMES) {
                stop("a menu");
            } else if (still >= STILL_FRAMES) {
                stop("the script waits");
            }
        }
        drew = 0;
        frames_ran = 0;
        scene_ran = 0;
        moved = 0;
        if (grx_skip_fast()) {
            /* One field's sound a field, as the vblank counts it: the
               play times scenes wait for (SndUpdate's handle timers)
               count fields, whatever the field logic's speed */
            grx_audio_field();
        }
        publish();
        return;
    }
    in_scene = sysDemo[1] == 0
               && (movie || (field && scene_now && !scene_choice && grx_frame_count() - scene_field <= 4));
    if (trace) {
        static int was;
        if (in_scene != was) {
            grx_log("skip-trace: scene %d at field %llu", in_scene, (unsigned long long)grx_frame_count());
        }
        was = in_scene;
    }
    if (!in_scene) {
        held = 0;
        prompt = 0;
    } else {
        if ((raw & B_START) && !start_lock && (raw & B_RESET) != B_RESET) {
            if (++held >= HOLD_FIELDS) {
                start();
            }
            prompt = PROMPT_FIELDS;
        } else {
            held = 0;
            if (prompt > 0) {
                prompt--;
            }
        }
    }
    publish();
}

/* A drawn frame is `updates` of the field's, and as many frames' fields
   go by, so everything the vblank counts keeps pace */
int grx_skip_fast(void) {
    int frame = sysState[0x09] > 0 ? sysState[0x09] : 1; /* frameMode: fields a frame */
    return skipping && !movie_playing() ? updates * frame : 0;
}

int grx_skip_updates(int n) {
    drew = 1;
    return grx_skip_fast() > 0 ? updates : n;
}

void grx_turbo_cycle(void) {
    int t = SDL_GetAtomicInt(&turbo_pick);
    t = t >= 4 ? 1 : t >= 2 ? 4 : 2;
    SDL_SetAtomicInt(&turbo_pick, t);
    grx_log("turbo: x%d", t);
}

int grx_turbo_speed(void) {
    int t = SDL_GetAtomicInt(&turbo_pick);
    return t > 1 && !skipping && !movie_playing() ? t : 1;
}

/* ---------------------------------------------------------------------- */
/* The presenter's side */

static const char vs_src[] = "#version 330 core\n"
                             "uniform vec4 rect;\n" /* x0, y0, x1, y1 in clip space */
                             "out vec2 uv;\n"
                             "void main() {\n"
                             "    int i = gl_VertexID;\n"
                             "    vec2 p = vec2(i == 1 || i == 2 || i == 4 ? 1.0 : 0.0, i == 2 || i == 4 || i == 5 ? 1.0 : 0.0);\n"
                             "    gl_Position = vec4(mix(rect.xy, rect.zw, p), 0.0, 1.0);\n"
                             "    uv = vec2(p.x, 1.0 - p.y);\n"
                             "}\n";
/* Premultiplied: the panel, then the bar (filled to fill_x, or a moving
   stretch while skipping), then the text; solid: black */
static const char fs_src[] = "#version 330 core\n"
                             "uniform sampler2D art;\n"
                             "uniform int solid;\n"
                             "uniform float alpha;\n"
                             "uniform vec4 bar;\n" /* fill_x, moving (0/1), phase, 0 in uv */
                             "in vec2 uv;\n"
                             "out vec4 color;\n"
                             "vec4 over(vec4 top, vec4 under) { return top + under * (1.0 - top.a); }\n"
                             "void main() {\n"
                             "    if (solid != 0) { color = vec4(0.0, 0.0, 0.0, alpha); return; }\n"
                             "    vec4 t = texture(art, uv);\n"
                             "    vec4 c = vec4(0.035, 0.04, 0.055, 1.0) * (0.8 * t.g);\n"
                             "    float fill = bar.y > 0.5 ? smoothstep(0.32, 0.0, abs(fract(uv.x * 0.6 - bar.z) - 0.5))\n"
                             "                             : step(uv.x, bar.x);\n"
                             "    float ba = t.b * mix(0.3, 1.0, fill);\n"
                             "    c = over(vec4(mix(vec3(1.0), vec3(1.0, 0.8, 0.36), fill) * ba, ba), c);\n"
                             "    c = over(vec4(vec3(0.96 * t.r), 0.96 * t.r), c);\n"
                             "    color = c * alpha;\n"
                             "}\n";

static int gl_state; /* 0 not set up, 1 ready, -1 failed */
static GLuint prog, vao, tex;
static GLint u_rect, u_solid, u_alpha, u_bar;
static int tex_px = -1, tex_kind = -1, tex_button = -1, tex_w, tex_h, tex_b0, tex_b1;
static GLuint ttex;
static int ttex_px = -1, ttex_kind = -1, ttex_w, ttex_h;
static float a_panel, a_black, a_turbo;
static uint64_t last_ns;

static int setup(void) {
    prog = grx_gl_program(vs_src, fs_src, "the scene skip prompt");
    if (prog == 0) {
        return -1;
    }
    u_rect = glGetUniformLocation(prog, "rect");
    u_solid = glGetUniformLocation(prog, "solid");
    u_alpha = glGetUniformLocation(prog, "alpha");
    u_bar = glGetUniformLocation(prog, "bar");
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "art"), 0);
    glGenVertexArrays(1, &vao);
    for (int i = 0; i < 2; i++) {
        GLuint *t = i == 0 ? &tex : &ttex;
        glGenTextures(1, t);
        glBindTexture(GL_TEXTURE_2D, *t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    return 0;
}

/* Eases v towards target at `rate` a second */
static float ease(float v, float target, float rate, float dt) {
    float step = rate * dt;
    if (v < target) {
        return v + step < target ? v + step : target;
    }
    return v - step > target ? v - step : target;
}

static void quad(float x0, float y0, float x1, float y1, int ww, int wh) {
    glUniform4f(u_rect, x0 / (float)ww * 2 - 1, y0 / (float)wh * 2 - 1, x1 / (float)ww * 2 - 1,
                y1 / (float)wh * 2 - 1);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void grx_skip_draw_gl(int x, int y, int w, int h, int ww, int wh) {
    uint64_t now = SDL_GetTicksNS();
    float dt = last_ns != 0 ? (float)(now - last_ns) / 1e9f : 0.0f;
    int show = SDL_GetAtomicInt(&ov_show), kind = SDL_GetAtomicInt(&ov_kind);
    int speed = grx_turbo_speed();
    int px;

    last_ns = now;
    dt = dt > 0.1f ? 0.1f : dt;
    a_panel = ease(a_panel, show ? 1.0f : 0.0f, show ? 8.0f : 4.0f, dt);
    a_black = ease(a_black, SDL_GetAtomicInt(&ov_black) ? 1.0f : 0.0f, SDL_GetAtomicInt(&ov_black) ? 5.0f : 2.5f, dt);
    a_turbo = ease(a_turbo, speed > 1 ? 1.0f : 0.0f, speed > 1 ? 8.0f : 4.0f, dt);
    if ((a_panel <= 0.0f && a_black <= 0.0f && a_turbo <= 0.0f) || w <= 0 || h <= 0 || ww <= 0 || wh <= 0 ||
        gl_state < 0) {
        return;
    }
    if (gl_state == 0) {
        gl_state = setup() == 0 ? 1 : -1;
        if (gl_state < 0) {
            grx_log("skip: no prompt (its shader didn't build)");
            return;
        }
    }
    glViewport(0, 0, ww, wh);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog);
    glBindVertexArray(vao);
    if (a_black > 0.0f) {
        glUniform1i(u_solid, 1);
        glUniform1f(u_alpha, a_black);
        quad((float)x, (float)y, (float)(x + w), (float)(y + h), ww, wh);
    }
    /* The text's capitals about a fortieth of the picture's height */
    px = (int)((float)h * 0.024f + 0.5f);
    px = px < 12 ? 12 : px > 72 ? 72 : px;
    if (a_panel > 0.0f) {
        int button = SDL_GetAtomicInt(&ov_button);
        float m = (float)h * 0.04f, p = (float)SDL_GetAtomicInt(&ov_progress) / 10000.0f;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glBindSampler(0, 0);
        if (px != tex_px || kind != tex_kind || (kind == 0 && button != tex_button)) {
            unsigned char *art = grx_skip_art(px, kind, button, &tex_w, &tex_h, &tex_b0, &tex_b1);
            if (art != NULL) {
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tex_w, tex_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, art);
                free(art);
                tex_px = px;
                tex_kind = kind;
                tex_button = button;
            }
        }
        if (tex_w > 0) {
            float fill = ((float)tex_b0 + p * (float)(tex_b1 - tex_b0)) / (float)tex_w;
            float x1 = (float)(x + w) - m, y0 = (float)y + m;
            glUniform1i(u_solid, 0);
            glUniform1f(u_alpha, a_panel);
            glUniform4f(u_bar, fill, (float)kind, (float)(now % 1500000000u) / 1.5e9f, 0.0f);
            quad(x1 - (float)tex_w, y0, x1, y0 + (float)tex_h, ww, wh);
        }
    }
    /* Fast forward: ">> n", the "Skipping" panel's look and corner (above
       the prompt when that shows), the speed kept while it fades out */
    if (a_turbo > 0.0f) {
        float m = (float)h * 0.04f, x1 = (float)(x + w) - m;
        float y0 = (float)y + m + (a_panel > 0.0f && tex_w > 0 ? ((float)tex_h + m * 0.5f) * a_panel : 0.0f);
        int tk = speed > 1 ? speed : ttex_kind;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ttex);
        glBindSampler(0, 0);
        if (tk >= 2 && (px != ttex_px || tk != ttex_kind)) {
            int b0, b1;
            unsigned char *art = grx_skip_art(px, tk, 0, &ttex_w, &ttex_h, &b0, &b1);
            if (art != NULL) {
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ttex_w, ttex_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, art);
                free(art);
                ttex_px = px;
                ttex_kind = tk;
            }
        }
        if (ttex_kind >= 2) {
            glUniform1i(u_solid, 0);
            glUniform1f(u_alpha, a_turbo);
            glUniform4f(u_bar, 1.0f, 1.0f, (float)(now % 1500000000u) / 1.5e9f, 0.0f);
            quad(x1 - (float)ttex_w, y0, x1, y0 + (float)ttex_h, ww, wh);
        }
    }
    glBindVertexArray(0);
    glDisable(GL_BLEND);
}
