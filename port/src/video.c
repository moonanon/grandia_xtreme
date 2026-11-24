/*
 * The frame loop and video output (grx_video.h).
 *
 * Fields. On the PS2 the vblank interrupt comes 59.94 times a second
 * whatever the game is doing. Here the kernel is cooperative, so a field
 * is run when every game thread waits (the kernel's idle hook): publish
 * the picture the GS displays, wait in real time for the field's deadline,
 * then flip the GS field (grx_gs_vsync), raise the kernel's vblank
 * (grx_kernel_field: the game's GsVblankHandler, alarms), count the EE
 * timers on, sample the pads and wake the threads blocked in
 * grx_frame_wait_vblank(). A game frame that took longer than a field gets
 * the vblanks it missed at once (up to MAX_CATCHUP), as the hardware would
 * have raised them during the frame, so the game's frame skipping
 * (GsGetVblanksSinceFlip) still sees them.
 *
 * The game waits for vblank by spinning: GsVsyncFlip loops until its
 * handler counts gsFlipWaitCount down, _sceWaitVSync polls INTC_STAT.
 * Those loops call grx_frame_wait_vblank() instead (FIXUPS and REPLACES in
 * port/tools/fixups/frame.py), which blocks the thread on a kernel
 * semaphore until the next field.
 *
 * Presents. The game's threads and the kernel are fibers on one OS thread,
 * which only gets to the idle hook when the game has finished its frame;
 * at 120 Hz a present is due every 8.3 ms, game work or not. So
 * presenting has its own OS thread, which owns the GL context: it swaps
 * with vsync (the display paces it), and each present shows the newest
 * picture published. The game thread only copies the GS's picture into a
 * triple buffer once per field (where it presented before), and its field
 * timing is the same as without a window. A GPU renderer draws in a
 * context shared with the presenter's and submits textures, fenced.
 * GRX_VIDEO_THREAD=0 presents on the game thread instead, once per field
 * and without vsync, as before (for debugging, and the shape a single-
 * threaded platform like the web would take).
 *
 * Locked to the display. The field clock runs on its own at 59.94 Hz and
 * the display on its; a 120 Hz display shows each picture for two
 * refreshes, but as the two clocks drift, the moment a picture is
 * published passes a refresh every few seconds, and for a second or so
 * pictures stay up for one refresh or three: motion stutters (a turning
 * camera seems to slow). So when the display refreshes a whole number of
 * times a field, within half a percent (60, 120, 240 Hz), a field lasts
 * that many refreshes, and each field the clock moves its phase a little
 * towards publishing half way between two of the presenter's swaps, the
 * moment furthest from both. The game runs up to 0.5% fast or slow then
 * (0.1% at 60 or 120 Hz). Not during a movie, whose audio keeps time.
 *
 * Defines the game-named _sceWaitVSync and _sceWaitVSyncCsr; must not
 * include <windows.h>.
 */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "grx_video.h"
#include "grx_gl.h"
#include "grx_pad.h"
#include "grx_gs.h"
#include "grx_kernel.h"
#include "grx_movie.h"
#include "grx_hwmem.h"
#include "grx_platform.h"
#include "grx_widescreen.h"
#include "grx_debug.h"
#include "grx_buttons.h"
#include "grx_skip.h"
#include "grx_menu.h"
#include "grx_input.h"
#include "grx_state_parts.h"
#include "grx_video_opts.h"

/* The GS (port/src/gs/) may not be linked yet: the frame loop runs
   without it (no picture, no CSR field flips) */
extern void grx_gs_vsync(void) __attribute__((weak));
extern int grx_gs_display(const uint32_t **pixels, int *width, int *height, int *pitch) __attribute__((weak));

/* The kernel's calls (port/src/kernel.c), with a layout-equal SemaParam */
typedef struct {
    int32_t currentCount, maxCount, initCount, numWaitThreads;
    uint32_t attr, option;
} VSemaParam;
int32_t CreateSema(VSemaParam *param);
int32_t WaitSema(int32_t sid);
int32_t iSignalSema(int32_t sid);

/* NTSC: 60000/1001 fields a second */
#define FIELD_NUM 1001000000000ull /* ns per field, times 60000 */
#define FIELD_DEN 60000ull
#define FIELD_NS (FIELD_NUM / FIELD_DEN)
#define MAX_CATCHUP 4

/* The EE timers (T0-T3 at 0x10000000 + 0x800 n: COUNT, MODE, COMP, HOLD).
   Counted on once per field, in MODE.CUE, at MODE.CLKS's rate */
#define BUSCLK 147456000ull
#define HSYNC_PER_SEC 15734ull /* NTSC: 262.5 lines a field */

static GrxVideoConfig cfg;
static SDL_Window *window;
static int sdl_up;

static uint64_t base_ns;      /* when field base_field was due */
static uint64_t base_field;
static uint64_t fields;       /* fields run */
static uint64_t start_ns;
static int started;

static int32_t vsync_sema = -1;
static int vsync_waiters;

static uint64_t log_ns, log_fields, log_late, log_flips;
static uint32_t log_presents;
static uint64_t flips;      /* game frames: GsVsyncFlip calls */
static uint64_t flips_want; /* the frames those would be at the rate the game asks for */
static uint64_t log_flips_want;

/* GsVsyncFlip(n): a game frame, which waits for n + 1 fields */
void grx_frame_flip(int32_t n) {
    flips++;
    flips_want += n == 0 ? 2 : 1; /* in 30 fps units: 60 fps counts twice */
}

static uint32_t *pattern;

/* ---------------------------------------------------------------------- */
/* Presenter state. `lock` guards what both threads touch: the software
   pictures' indices, the GL submission, the texture table, the hook and
   the time stamps. */

#define NSHARED 4
#define NGLTEX 16

static SDL_GLContext gl_ctx;          /* the presenter's */
static SDL_GLContext shared_ctx[NSHARED];
/* Each shared context has a window of its own off Windows: EGL lets a
   window's surface be current on one thread only, and the presenter's
   thread has the main window's. (WGL allows several contexts on one
   window.) */
static SDL_Window *shared_win[NSHARED];
static int nshared;
static int gl_up;                     /* presenting through GL */
static int threaded;                  /* on the presenter thread (else on the game thread) */
static int hidden;                    /* a hidden window, for a GL context only: no presents */
static SDL_Thread *present_thread;
static SDL_ThreadID present_tid;
static SDL_AtomicInt present_quit, present_ready;
static SDL_AtomicU32 present_count;
static SDL_AtomicInt win_w, win_h, minimized;
static uint64_t last_swap_ns;         /* (lock) when the presenter's last swap returned; 0 none */
static SDL_AtomicInt lock_k;          /* refreshes a field while the fields are locked to them, else 0 */
static SDL_AtomicInt refresh_mhz;     /* the display's refresh rate, mHz (0 unknown) */
static SDL_AtomicInt gl_source_on;
static SDL_Mutex *lock;
static int vsync_mode;                /* -1 adaptive, 1 on, 0 off (presenter thread) */

/* GL objects (the presenter's context) */
static GLuint prog, vao, samplers[2];
static GLint u_uvmap;

/* The latest field's stamps for published pictures (main writes) */
static uint64_t stamp_field, stamp_ns;
static int dropped(void);

/* Software pictures: a triple buffer, the game thread writes `back` and
   swaps it with `ready`, the presenter swaps `ready` with `front` */
typedef struct SoftPic {
    uint32_t *px;
    size_t cap;
    int w, h;
    float aspect;
    uint64_t field, time_ns;
} SoftPic;
static SoftPic soft[3];
static int soft_back = 0, soft_ready = 1, soft_front = 2, soft_new;

/* The presenter's textures for them: three, so the two held pictures and
   the upload never share one */
typedef struct SoftTex {
    GLuint tex;
    int w, h;
} SoftTex;
static SoftTex soft_tex[3];

/* The two newest pictures: [0] previous, [1] current. soft >= 0: one of
   soft_tex; -1: a submitted GL texture (or none) */
typedef struct Held {
    GrxVideoFrame f;
    int soft;
} Held;
static Held held[2] = {{{0}, -1}, {{0}, -1}};

/* GL submissions not yet presented (the newest only) */
static GrxVideoFrame pending;
static GLsync pending_fence;
static int have_pending;

/* Submitted textures: refs = held or pending; at 0 the presenter is done
   with it once `release` (a fence after its last draw) has passed */
typedef struct GlTex {
    GLuint tex;
    int refs;
    GLsync release;
} GlTex;
static GlTex gltex[NGLTEX];

static GrxVideoPresentHook hook;
static void *hook_user;

/* ---------------------------------------------------------------------- */
/* Window */

static const char vs_src[] = "#version 330 core\n"
                             "uniform vec4 uv_map;\n"
                             "out vec2 uv;\n"
                             "void main() {\n"
                             "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
                             "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
                             "    uv = p * uv_map.xy + uv_map.zw;\n"
                             "}\n";
static const char fs_src[] = "#version 330 core\n"
                             "uniform sampler2D picture;\n"
                             "in vec2 uv;\n"
                             "out vec4 color;\n"
                             "void main() {\n"
                             "    color = vec4(texture(picture, uv).rgb, 1.0);\n"
                             "}\n";

static void query_refresh(void) {
    const SDL_DisplayMode *m;
    if (window == NULL) {
        return;
    }
    m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    SDL_SetAtomicInt(&refresh_mhz, m != NULL ? (int)(m->refresh_rate * 1000.0f + 0.5f) : 0);
}

static void query_size(void) {
    int w = 0, h = 0;
    if (window != NULL && SDL_GetWindowSizeInPixels(window, &w, &h)) {
        SDL_SetAtomicInt(&win_w, w);
        SDL_SetAtomicInt(&win_h, h);
    }
}

static int gl_setup(void) {
    int i;
    if (grx_gl_load() != 0) {
        return -1;
    }
    prog = grx_gl_program(vs_src, fs_src, "the presenter");
    if (prog == 0) {
        return -1;
    }
    u_uvmap = glGetUniformLocation(prog, "uv_map");
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "picture"), 0);
    glGenVertexArrays(1, &vao);
    glGenSamplers(2, samplers);
    for (i = 0; i < 2; i++) {
        glSamplerParameteri(samplers[i], GL_TEXTURE_MIN_FILTER, i ? GL_LINEAR : GL_NEAREST);
        glSamplerParameteri(samplers[i], GL_TEXTURE_MAG_FILTER, i ? GL_LINEAR : GL_NEAREST);
        glSamplerParameteri(samplers[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(samplers[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (glGetError() != GL_NO_ERROR) {
        grx_log("video: GL error setting up");
        return -1;
    }
    return 0;
}

static void no_window(const char *what) {
    grx_log("video: %s (%s), running headless", what, SDL_GetError());
    if (gl_ctx != NULL) {
        SDL_GL_DestroyContext(gl_ctx);
        gl_ctx = NULL;
    }
    if (window != NULL) {
        SDL_DestroyWindow(window);
        window = NULL;
    }
    cfg.headless = 1;
}

static int open_window(void);

int grx_video_init(const GrxVideoConfig *c) {
    cfg = *c;
    if (cfg.smooth < 0) {
        cfg.smooth = cfg.integer ? 0 : 1;
    }
    /* Pads keep working while the console has the focus (--pad-test) */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        grx_fatal("SDL_Init: %s", SDL_GetError());
    }
    sdl_up = 1;
    grx_pad_init();
    lock = SDL_CreateMutex();
    if (lock == NULL) {
        grx_fatal("SDL_CreateMutex: %s", SDL_GetError());
    }
    if (cfg.headless && !cfg.hidden_gl) {
        return 0;
    }
    return open_window();
}

static int open_window(void) {
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL;
    const char *th;
    int scale, ww, wh;

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        no_window("no video");
        return -1;
    }
    scale = cfg.scale > 0 ? cfg.scale : 2;
    if (cfg.headless) {
        /* (cfg.hidden_gl) */
        flags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN;
        hidden = 1;
    } else if (cfg.fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    ww = grx_ws_window_width(480 * scale);
    wh = 480 * scale;
    if (!cfg.headless && !cfg.fullscreen) {
        /* A window bigger than the screen's usable area would be cut to
           it, and its shape, which --aspect auto follows, with it. Shrink
           it to fit with its shape kept; the margin is for the title bar
           and borders. (Some systems open the window at the whole usable
           area anyway: a 1920x1080 handheld at 150% gives 1920x974.) */
        SDL_DisplayID d = SDL_GetPrimaryDisplay();
        SDL_Rect r;
        if (SDL_GetDisplayUsableBounds(d, &r) && r.w > 0 && r.h > 0) {
            /* The bounds are in pixels, the window's size in points scaled
               by the display's scale */
            float cs = SDL_GetDisplayContentScale(d);
            double shape = (double)ww / wh;
            int maxw, maxh;
            if (cs < 1.0f) {
                cs = 1.0f;
            }
            maxw = (int)(r.w / cs) - 16;
            maxh = (int)(r.h / cs) - 48;
            if (wh > maxh) {
                wh = maxh;
                ww = (int)(wh * shape + 0.5);
            }
            if (ww > maxw) {
                ww = maxw;
                wh = (int)(ww / shape + 0.5);
            }
        }
    }
    window = SDL_CreateWindow("Grandia Xtreme", ww, wh, flags);
    if (window == NULL) {
        no_window("no window");
        return -1;
    }
    gl_ctx = SDL_GL_CreateContext(window);
    if (gl_ctx == NULL) {
        no_window("no OpenGL 3.3 core context");
        return -1;
    }
    if (gl_setup() != 0) {
        no_window("OpenGL set-up failed");
        return -1;
    }
    /* Until the presenter thread takes over: paced by the field clock */
    SDL_GL_SetSwapInterval(0);
    SDL_GL_SwapWindow(window);
    gl_up = 1;
    query_size();
    query_refresh();
    th = getenv("GRX_VIDEO_THREAD");
    threaded = !hidden && (th == NULL || atoi(th) != 0);
    grx_log("video: %dx%d window (%dx%d pixels)%s, OpenGL %s (%s), display %.2f Hz, presenting %s", ww,
            wh, SDL_GetAtomicInt(&win_w), SDL_GetAtomicInt(&win_h), cfg.fullscreen ? ", fullscreen" : "",
            (const char *)glGetString(GL_VERSION),
            (const char *)glGetString(GL_RENDERER), SDL_GetAtomicInt(&refresh_mhz) / 1000.0,
            threaded ? "on its own thread at the display's rate" : "on the game thread once per field");
    return 0;
}

static void stop_presenter(void) {
    if (present_thread != NULL && SDL_GetCurrentThreadID() != present_tid) {
        SDL_SetAtomicInt(&present_quit, 1);
        SDL_WaitThread(present_thread, NULL);
        present_thread = NULL;
    }
}

void grx_video_shutdown(void) {
    int i;
    if (!sdl_up) {
        return;
    }
    stop_presenter();
    grx_pad_shutdown();
    for (i = 0; i < nshared; i++) {
        SDL_GL_DestroyContext(shared_ctx[i]);
        if (shared_win[i] != NULL) {
            SDL_DestroyWindow(shared_win[i]);
            shared_win[i] = NULL;
        }
    }
    nshared = 0;
    if (gl_ctx != NULL) {
        SDL_GL_DestroyContext(gl_ctx);
        gl_ctx = NULL;
    }
    gl_up = 0;
    if (window != NULL) {
        SDL_DestroyWindow(window);
        window = NULL;
    }
    SDL_Quit();
    sdl_up = 0;
    for (i = 0; i < 3; i++) {
        free(soft[i].px);
        soft[i].px = NULL;
        soft[i].cap = 0;
    }
    free(pattern);
    pattern = NULL;
}

/* A picture for --video-test while the GS shows nothing: bars, and a
   block that moves one step per field (tearing or dropped fields show) */
static const uint32_t *test_picture(int *w, int *h) {
    static const uint32_t bars[8] = {0xFFC0C0C0, 0xFFC0C000, 0xFF00C0C0, 0xFF00C000,
                                     0xFFC000C0, 0xFFC00000, 0xFF0000C0, 0xFF000000};
    int x, y, bx;
    *w = 640;
    *h = 448;
    if (pattern == NULL) {
        pattern = malloc(640 * 448 * sizeof(uint32_t));
        if (pattern == NULL) {
            return NULL;
        }
    }
    bx = (int)(fields % 600) * 640 / 600;
    for (y = 0; y < 448; y++) {
        uint32_t *row = pattern + y * 640;
        for (x = 0; x < 640; x++) {
            row[x] = y < 320 ? bars[x * 8 / 640] : ((x ^ y) & 16 ? 0xFF202020 : 0xFF404040);
        }
        if (y >= 340 && y < 420) {
            for (x = bx; x < bx + 40 && x < 640; x++) {
                row[x] = 0xFFFFFFFF;
            }
        }
    }
    return pattern;
}

/* The game thread: copy the GS's picture into the triple buffer */
void grx_video_present(void) {
    const uint32_t *px = NULL;
    int w = 0, h = 0, pitch = 0, have = 0, y, t;
    SoftPic *p;

    if (!gl_up || SDL_GetAtomicInt(&gl_source_on) || dropped()) {
        return;
    }
    if (grx_gs_display != NULL) {
        have = grx_gs_display(&px, &w, &h, &pitch) && px != NULL && w > 0 && h > 0;
    }
    if (!have && cfg.test_pattern) {
        px = test_picture(&w, &h);
        pitch = w;
        have = px != NULL;
    }
    p = &soft[soft_back];
    p->w = p->h = 0;
    if (have) {
        size_t n = (size_t)w * (size_t)h;
        if (n > p->cap) {
            uint32_t *nb = realloc(p->px, n * sizeof(uint32_t));
            if (nb == NULL) {
                return;
            }
            p->px = nb;
            p->cap = n;
        }
        for (y = 0; y < h; y++) {
            memcpy(p->px + (size_t)y * w, px + (size_t)y * pitch, (size_t)w * sizeof(uint32_t));
        }
        p->w = w;
        p->h = h;
    }
    p->aspect = grx_ws_aspect();
    p->field = stamp_field;
    p->time_ns = stamp_ns;
    SDL_LockMutex(lock);
    t = soft_back;
    soft_back = soft_ready;
    soft_ready = t;
    soft_new = 1;
    SDL_UnlockMutex(lock);
}

/* ---------------------------------------------------------------------- */
/* Presenting (with the presenter's context current) */

static GlTex *gltex_find(GLuint tex) {
    int i;
    for (i = 0; i < NGLTEX; i++) {
        if (gltex[i].tex == tex) {
            return &gltex[i];
        }
    }
    return NULL;
}

/* Under lock: the presenter or the pending slot lets go of `tex`; `fence`
   follows the presenter's last use of it (NULL: it never drew it) */
static GLsync gltex_unref(GLuint tex, GLsync fence) {
    GlTex *e = gltex_find(tex);
    GLsync old = NULL;
    if (e == NULL || e->refs <= 0) {
        return fence;
    }
    e->refs--;
    if (fence != NULL) {
        /* Fences signal in order: the newer one covers the older */
        old = e->release;
        e->release = fence;
    }
    return old;
}

static void drop(Held *h) {
    GLsync fence, old;
    if (h->f.texture == 0 || h->soft >= 0) {
        return;
    }
    fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    SDL_LockMutex(lock);
    old = gltex_unref(h->f.texture, fence);
    SDL_UnlockMutex(lock);
    if (old != NULL) {
        glDeleteSync(old);
    }
}

static void push(const GrxVideoFrame *f, int soft_idx) {
    drop(&held[0]);
    held[0] = held[1];
    held[1].f = *f;
    held[1].soft = soft_idx;
}

static void upload(const SoftPic *p) {
    GrxVideoFrame f;
    SoftTex *st = NULL;
    int k;

    memset(&f, 0, sizeof(f));
    f.field = p->field;
    f.time_ns = p->time_ns;
    if (p->w <= 0 || p->h <= 0) {
        push(&f, -1);
        return;
    }
    for (k = 0; k < 3; k++) {
        if (held[0].soft != k && held[1].soft != k) {
            st = &soft_tex[k];
            break;
        }
    }
    if (st->tex == 0) {
        glGenTextures(1, &st->tex);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, st->tex);
    if (st->w != p->w || st->h != p->h) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, p->w, p->h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        st->w = p->w;
        st->h = p->h;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, p->w);
    /* 0xAARRGGBB in memory on a little-endian host: B, G, R, A */
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, p->w, p->h, GL_BGRA, GL_UNSIGNED_BYTE, p->px);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    f.texture = st->tex;
    f.width = p->w;
    f.height = p->h;
    f.aspect = p->aspect;
    f.top_down = 1;
    push(&f, k);
}

/* Take the newest picture published or submitted, if there is a new one */
static void take_new(void) {
    GrxVideoFrame nf;
    GLsync fence = NULL, old = NULL;
    const SoftPic *sp = NULL;
    int got = 0, t;

    SDL_LockMutex(lock);
    if (have_pending) {
        nf = pending;
        fence = pending_fence;
        have_pending = 0;
        got = 2;
        if (!SDL_GetAtomicInt(&gl_source_on)) {
            /* Submitted, then the source switched back: never shown */
            old = gltex_unref(nf.texture, NULL);
            got = 0;
        }
    } else if (soft_new) {
        t = soft_front;
        soft_front = soft_ready;
        soft_ready = t;
        soft_new = 0;
        sp = &soft[soft_front];
        got = 1;
    }
    SDL_UnlockMutex(lock);
    if (old != NULL) {
        glDeleteSync(old);
    }
    if (fence != NULL) {
        /* The GPU waits for the renderer's commands; the CPU goes on */
        glWaitSync(fence, 0, GL_TIMEOUT_IGNORED);
        glDeleteSync(fence);
    }
    if (got == 2) {
        push(&nf, -1);
    } else if (got == 1) {
        upload(sp);
    }
}

/* Where draw() put the picture last (the window's pixels, from the bottom
   left) */
static int pic_x, pic_y, pic_w, pic_h;

static void draw(const GrxVideoFrame *f) {
    int ow = SDL_GetAtomicInt(&win_w), oh = SDL_GetAtomicInt(&win_h);
    float a, dw, dh;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glViewport(0, 0, ow, oh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (f->texture == 0 || f->width <= 0 || f->height <= 0 || ow <= 0 || oh <= 0) {
        return;
    }
    /* The picture at its display aspect, as large as the window allows */
    a = f->aspect > 0 ? f->aspect : 4.0f / 3.0f;
    if ((float)ow >= (float)oh * a) {
        dh = (float)oh;
        dw = dh * a;
    } else {
        dw = (float)ow;
        dh = dw / a;
    }
    if (cfg.integer) {
        /* Whole multiples of the picture's lines, the width to the aspect */
        int k = (int)(dh / (float)f->height);
        if (k >= 1) {
            dh = (float)(k * f->height);
            dw = dh * a;
        }
    }
    pic_x = (int)((ow - dw) / 2);
    pic_y = (int)((oh - dh) / 2);
    pic_w = (int)(dw + 0.5f);
    pic_h = (int)(dh + 0.5f);
    glViewport(pic_x, pic_y, pic_w, pic_h);
    glUseProgram(prog);
    glBindVertexArray(vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, f->texture);
    glBindSampler(0, samplers[cfg.smooth ? 1 : 0]);
    /* Viewport bottom (v 0) to the picture's bottom line */
    if (f->top_down) {
        glUniform4f(u_uvmap, 1.0f, -1.0f, 0.0f, 1.0f);
    } else {
        glUniform4f(u_uvmap, 1.0f, 1.0f, 0.0f, 0.0f);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* GRX_PRESENT_SNAPSHOT=FILE.ppm (at GRX_PRESENT_SNAPSHOT_AT presents,
   default 300): the window's picture as presented, scaling, filtering
   and present hook included, read back once (the GS's own picture is
   GRX_SNAPSHOT_DIR's) */
static void present_snapshot(void) {
    static int at = -1;
    static const char *path;
    int ow = SDL_GetAtomicInt(&win_w), oh = SDL_GetAtomicInt(&win_h), y;
    unsigned char *buf;
    FILE *f;

    if (at < 0) {
        const char *e = getenv("GRX_PRESENT_SNAPSHOT_AT");
        path = getenv("GRX_PRESENT_SNAPSHOT");
        at = e != NULL && atoi(e) > 0 ? atoi(e) : 300;
    }
    if (path == NULL || SDL_GetAtomicU32(&present_count) != (uint32_t)at || ow <= 0 || oh <= 0) {
        return;
    }
    buf = malloc((size_t)ow * (size_t)oh * 4);
    f = fopen(path, "wb");
    if (buf != NULL && f != NULL) {
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glReadPixels(0, 0, ow, oh, GL_RGBA, GL_UNSIGNED_BYTE, buf);
        fprintf(f, "P6\n%d %d\n255\n", ow, oh);
        for (y = oh - 1; y >= 0; y--) {
            const unsigned char *row = buf + (size_t)y * (size_t)ow * 4;
            int x;
            for (x = 0; x < ow; x++) {
                fwrite(row + x * 4, 1, 3, f);
            }
        }
        grx_log("video: present %d written to %s (%dx%d)", at, path, ow, oh);
    }
    if (f != NULL) {
        fclose(f);
    }
    free(buf);
}

/* How many presents each picture stayed up for: [n - 1] pictures shown n
   times, [3] 4 or more (the presenter counts, log_rate reads and clears) */
static SDL_AtomicInt shown_for[4];

static void count_shown(uint64_t field) {
    static uint64_t last_field = UINT64_MAX;
    static int times;
    if (field == last_field) {
        times++;
        return;
    }
    if (last_field != UINT64_MAX && times > 0) {
        SDL_AddAtomicInt(&shown_for[times > 4 ? 3 : times - 1], 1);
    }
    last_field = field;
    times = 1;
}

static void present_once(void) {
    GrxVideoPresentHook h;
    GrxVideoFrame show;
    void *hu;

    take_new();
    count_shown(held[1].f.field);
    SDL_LockMutex(lock);
    h = hook;
    hu = hook_user;
    SDL_UnlockMutex(lock);
    show = held[1].f;
    if (h != NULL) {
        uint64_t now = SDL_GetTicksNS();
        double t = 1.0;
        GrxVideoFrame out = show;
        const GrxVideoFrame *p = &held[0].f, *c = &held[1].f;
        if (p->texture != 0 && c->texture != 0 && c->time_ns > p->time_ns) {
            t = (double)(int64_t)(now - c->time_ns) / (double)(c->time_ns - p->time_ns);
            t = t < 0 ? 0 : t > 1 ? 1 : t;
        }
        if (h(hu, p, c, t, now, &out)) {
            show = out;
        }
    }
    draw(&show);
    grx_skip_draw_gl(pic_x, pic_y, pic_w, pic_h, SDL_GetAtomicInt(&win_w), SDL_GetAtomicInt(&win_h));
    grx_menu_draw_gl(pic_x, pic_y, pic_w, pic_h, SDL_GetAtomicInt(&win_w), SDL_GetAtomicInt(&win_h));
    present_snapshot();
    SDL_GL_SwapWindow(window);
    SDL_AddAtomicU32(&present_count, 1);
}

/* The Frame rate setting (grx_video_opts.h). While the fields are locked
   to the display, k refreshes a field: a swap every k refreshes for 60
   (one per picture; the swaps a refresh showed each picture k times) and
   every 2k for 30. Otherwise a swap a refresh, as before: 30 still shows
   every other picture (grx_video_gl_submit, grx_video_present drop the
   rest). On the presenter's thread, which has the context. */
static void frame_rate_interval(void) {
    static int interval = 1, failed;
    int k = SDL_GetAtomicInt(&lock_k), n = 1;
    if (vsync_mode == 0) {
        return;
    }
    if (k > 0) {
        n = k * (grx_opts_frame_rate() == 30 ? 2 : 1);
    }
    if (n == interval || n == failed) {
        return;
    }
    if (SDL_GL_SetSwapInterval(vsync_mode < 0 ? -n : n) || SDL_GL_SetSwapInterval(n)) {
        interval = n;
        failed = 0;
        grx_log("video: a swap every %d refresh%s", n, n == 1 ? "" : "es");
    } else {
        failed = n;
        grx_log("video: a swap every %d refreshes isn't available: %s", n, SDL_GetError());
    }
}

/* Frame rate 30: only the even fields' pictures go to the presenter */
static int dropped(void) {
    return grx_opts_frame_rate() == 30 && (stamp_field & 1) != 0;
}

static int SDLCALL present_main(void *arg) {
    uint64_t last = 0;
    (void)arg;
    if (!SDL_GL_MakeCurrent(window, gl_ctx)) {
        grx_log("video: presenter: SDL_GL_MakeCurrent: %s; no more presents", SDL_GetError());
        SDL_SetAtomicInt(&present_ready, 1);
        return 1;
    }
    /* Adaptive vsync (late frames tear instead of waiting a whole refresh)
       where the driver has it, else plain vsync */
    vsync_mode = SDL_GL_SetSwapInterval(-1) ? -1 : SDL_GL_SetSwapInterval(1) ? 1 : 0;
    grx_log("video: presenter thread: vsync %s", vsync_mode < 0 ? "adaptive" : vsync_mode ? "on" : "unavailable");
    present_once();
    SDL_SetAtomicInt(&present_ready, 1);
    while (!SDL_GetAtomicInt(&present_quit)) {
        uint64_t now = SDL_GetTicksNS(), period;
        int mhz = SDL_GetAtomicInt(&refresh_mhz);
        if (SDL_GetAtomicInt(&minimized)) {
            SDL_Delay(10);
            continue;
        }
        frame_rate_interval();
        /* The swap paces this loop. If it doesn't block (vsync off, or
           forced off in the driver), pace it to the refresh rate */
        period = 1000000000000ull / (uint64_t)(mhz > 0 ? mhz : 60000);
        if (vsync_mode != 0) {
            period /= 2;
        }
        if (last != 0 && now - last < period) {
            SDL_DelayPrecise(period - (now - last));
            now = SDL_GetTicksNS();
        }
        last = now;
        present_once();
        if (vsync_mode != 0) {
            uint64_t t = SDL_GetTicksNS();
            SDL_LockMutex(lock);
            last_swap_ns = t;
            SDL_UnlockMutex(lock);
        }
    }
    glFinish();
    SDL_GL_MakeCurrent(window, NULL);
    return 0;
}

/* GRX_VIDEO_THREAD=0: present on the game thread, in the presenter's
   context, putting back whichever context the thread had */
static void present_inline(void) {
    SDL_GLContext was;
    if (!gl_up || threaded || hidden || SDL_GetAtomicInt(&minimized)) {
        return;
    }
    was = SDL_GL_GetCurrentContext();
    if (was != gl_ctx && !SDL_GL_MakeCurrent(window, gl_ctx)) {
        return;
    }
    present_once();
    if (was != gl_ctx && was != NULL) {
        SDL_GL_MakeCurrent(window, was);
    }
}

static void start_presenter(void) {
    int i;
    if (!gl_up || !threaded) {
        return;
    }
    /* The context can be current on one thread only */
    if (SDL_GL_GetCurrentContext() == gl_ctx) {
        SDL_GL_MakeCurrent(window, NULL);
    }
    present_thread = SDL_CreateThread(present_main, "grx_present", NULL);
    if (present_thread == NULL) {
        grx_log("video: no presenter thread (%s), presenting on the game thread", SDL_GetError());
        threaded = 0;
        return;
    }
    present_tid = SDL_GetThreadID(present_thread);
    /* Until it has set up and presented once: what the driver allocates
       for that falls inside main()'s hold on the game's low memory */
    for (i = 0; i < 2000 && !SDL_GetAtomicInt(&present_ready); i++) {
        SDL_Delay(1);
    }
}

/* ---------------------------------------------------------------------- */
/* The GPU renderer's interface */

int grx_video_gl_available(void) {
    return gl_up;
}

void grx_video_window_size(int *w, int *h) {
    *w = gl_up ? SDL_GetAtomicInt(&win_w) : 0;
    *h = gl_up ? SDL_GetAtomicInt(&win_h) : 0;
}

void *grx_video_gl_create_context(void) {
    SDL_GLContext c;
    SDL_Window *w = NULL;
    if (!gl_up) {
        grx_log("video: no GL context to share (headless)");
        return NULL;
    }
    if (started) {
        grx_log("video: grx_video_gl_create_context after grx_frame_start");
        return NULL;
    }
    if (nshared >= NSHARED) {
        grx_log("video: too many shared GL contexts");
        return NULL;
    }
    if (!SDL_GL_MakeCurrent(window, gl_ctx)) {
        grx_log("video: SDL_GL_MakeCurrent: %s", SDL_GetError());
        return NULL;
    }
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
#ifdef _WIN32
    c = SDL_GL_CreateContext(window);
#else
    w = SDL_CreateWindow("grx-gl", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    c = w != NULL ? SDL_GL_CreateContext(w) : NULL;
#endif
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    if (c == NULL) {
        grx_log("video: no shared GL context: %s", SDL_GetError());
#ifndef _WIN32
        if (w != NULL) {
            SDL_DestroyWindow(w);
        }
#endif
        return NULL;
    }
    shared_win[nshared] = w;
    shared_ctx[nshared++] = c;
    return c;
}

int grx_video_gl_make_current(void *ctx) {
    SDL_Window *w = window;
    int i;
    for (i = 0; i < nshared; i++) {
        if (shared_ctx[i] == (SDL_GLContext)ctx && shared_win[i] != NULL) {
            w = shared_win[i];
        }
    }
    if (window == NULL || !SDL_GL_MakeCurrent(w, (SDL_GLContext)ctx)) {
        grx_log("video: SDL_GL_MakeCurrent: %s", SDL_GetError());
        return -1;
    }
    return 0;
}

void grx_video_gl_source(int on) {
    SDL_SetAtomicInt(&gl_source_on, on != 0);
}

void grx_video_gl_submit(const GrxVideoFrame *frame) {
    static int warned;
    GrxVideoFrame nf = *frame;
    GLsync fence, old = NULL, evicted = NULL;

    grx_dbg_picture_gl(frame);
    if (!gl_up || (frame->texture != 0 && dropped())) {
        return;
    }
    fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    SDL_LockMutex(lock);
    nf.field = stamp_field;
    nf.time_ns = stamp_ns;
    if (have_pending) {
        /* Replaced before it was presented: the presenter never drew it */
        old = gltex_unref(pending.texture, NULL);
        if (pending_fence != NULL) {
            glDeleteSync(pending_fence);
        }
    }
    if (nf.texture != 0) {
        GlTex *e = gltex_find(nf.texture);
        int i;
        if (e == NULL) {
            /* A free entry, else one whose texture the presenter is done with */
            e = gltex_find(0);
            for (i = 0; e == NULL && i < NGLTEX; i++) {
                if (gltex[i].refs == 0) {
                    e = &gltex[i];
                    evicted = e->release;
                }
            }
            if (e != NULL) {
                e->tex = nf.texture;
                e->refs = 0;
                e->release = NULL;
            }
        }
        if (e != NULL) {
            e->refs++;
        } else if (!warned) {
            warned = 1;
            grx_log("video: more than %d submitted GL textures in use; some aren't tracked", NGLTEX);
        }
    }
    pending = nf;
    pending_fence = fence;
    have_pending = 1;
    SDL_UnlockMutex(lock);
    if (old != NULL) {
        glDeleteSync(old);
    }
    if (evicted != NULL) {
        /* Another texture's entry reused: its last use still has to pass
           before this context draws again */
        glWaitSync(evicted, 0, GL_TIMEOUT_IGNORED);
        glDeleteSync(evicted);
    }
}

int grx_video_gl_texture_free(unsigned texture) {
    GlTex *e;
    GLsync fence = NULL;

    SDL_LockMutex(lock);
    e = gltex_find(texture);
    if (e != NULL) {
        if (e->refs > 0) {
            SDL_UnlockMutex(lock);
            return 0;
        }
        fence = e->release;
        e->tex = 0;
        e->release = NULL;
    }
    SDL_UnlockMutex(lock);
    if (fence != NULL) {
        glWaitSync(fence, 0, GL_TIMEOUT_IGNORED);
        glDeleteSync(fence);
    }
    return 1;
}

void grx_video_set_present_hook(GrxVideoPresentHook h, void *user) {
    if (lock == NULL) {
        return;
    }
    SDL_LockMutex(lock);
    hook = h;
    hook_user = user;
    SDL_UnlockMutex(lock);
}

/* ---------------------------------------------------------------------- */
/* Events */

/* exit() runs main.c's atexit handlers: audio and memory cards first,
   then grx_video_shutdown, whose SDL_Quit must come last */
static void quit(const char *why) {
    grx_log("video: %s, exiting after %llu fields", why, (unsigned long long)fields);
    exit(0);
}

/* F11, Alt+Enter or a double click: fullscreen, or back to the window */
static void toggle_fullscreen(void) {
    if (window != NULL) {
        cfg.fullscreen = !cfg.fullscreen;
        SDL_SetWindowFullscreen(window, cfg.fullscreen);
    }
}

void grx_video_pump(void) {
    SDL_Event ev;

    if (!sdl_up) {
        return;
    }
    while (SDL_PollEvent(&ev)) {
        /* Only the game's window: the other GL contexts' hidden windows
           send their own (a 64x64 size, shown, ...) */
        if (ev.type >= SDL_EVENT_WINDOW_FIRST && ev.type <= SDL_EVENT_WINDOW_LAST && window != NULL &&
            ev.window.windowID != SDL_GetWindowID(window)) {
            continue;
        }
        switch (ev.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            quit("window closed");
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            /* The open port menu takes the gamepad too (the game waits) */
            grx_menu_pad(ev.gbutton.button);
            break;
        case SDL_EVENT_KEY_DOWN:
            if (!ev.key.repeat && grx_dbg_key(ev.key.key)) {
                break;
            }
            /* The port menu: Esc opens it (not while the Configurations
               page waits for a key, where Esc cancels); open, it takes the keys */
            if (grx_menu_key(ev.key.key, ev.key.scancode)) {
                break;
            }
            if (!ev.key.repeat && ev.key.key == SDLK_ESCAPE && !grx_input_capturing()) {
                grx_menu_toggle();
                break;
            }
            if (!ev.key.repeat &&
                (ev.key.key == SDLK_F11 || (ev.key.key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT)))) {
                toggle_fullscreen();
            }
            if (!ev.key.repeat && grx_action_for_key(ev.key.scancode) >= 0) {
                grx_action_run(grx_action_for_key(ev.key.scancode));
            }
#ifdef GRX_DEBUG_KIT
            if (!ev.key.repeat && ev.key.key == SDLK_F5) {
                grx_super_toggle();
            }
            if (!ev.key.repeat && ev.key.key == SDLK_F6 && (ev.key.mod & SDL_KMOD_CTRL)) {
                grx_gate_warp_request((ev.key.mod & SDL_KMOD_SHIFT) ? -1 : 1);
            } else if (!ev.key.repeat && ev.key.key == SDLK_F6) {
                grx_warp_request((ev.key.mod & SDL_KMOD_SHIFT) ? -1 : 1);
            }
            if (!ev.key.repeat && ev.key.key == SDLK_F7) {
                if (ev.key.mod & SDL_KMOD_SHIFT) {
                    grx_map_reveal_toggle();
                } else {
                    grx_noclip_toggle();
                }
            }
            if (!ev.key.repeat && ev.key.key == SDLK_F12) {
                grx_debug_hud_toggle();
            }
#endif
            if (!ev.key.repeat && ev.key.key == SDLK_F8) {
                grx_spot_request();
            }
            /* Save states: F1-F4 load slot 1-4, Shift+F1-F4 save there */
            if (!ev.key.repeat && ev.key.key >= SDLK_F1 && ev.key.key <= SDLK_F4) {
                grx_state_request((int)(ev.key.key - SDLK_F1) + 1, !(ev.key.mod & SDL_KMOD_SHIFT));
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            /* A double click on the picture, as in video players (the
               title bar's is the system's: maximise) */
            if (ev.button.button == SDL_BUTTON_LEFT && ev.button.clicks == 2) {
                toggle_fullscreen();
            }
            break;
        /* The presenter reads the window's state from here: SDL's window
           calls belong on this thread */
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            SDL_SetAtomicInt(&win_w, ev.window.data1);
            SDL_SetAtomicInt(&win_h, ev.window.data2);
            break;
        case SDL_EVENT_WINDOW_MINIMIZED:
            SDL_SetAtomicInt(&minimized, 1);
            break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
        case SDL_EVENT_WINDOW_SHOWN:
            SDL_SetAtomicInt(&minimized, 0);
            query_size();
            break;
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
        case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
            query_refresh();
            query_size();
            break;
        default:
            grx_pad_event(&ev);
            break;
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Fields */

/* A field's length, in ns times FIELD_DEN: FIELD_NUM, or locked to the
   display some refreshes */
static uint64_t field_num = FIELD_NUM;

static uint64_t field_due(uint64_t n) {
    return base_ns + (n - base_field) * field_num / FIELD_DEN;
}

/* Each field: lock the field clock to the presenter's swaps, or unlock it
   (the header) */
static void lock_to_display(uint64_t now) {
    static int logged = -1;
    uint64_t want = FIELD_NUM, refresh_num = 0, swap, due, r;
    int mhz = SDL_GetAtomicInt(&refresh_mhz), k = 0, locked, turbo = grx_turbo_speed();
    int64_t ph, err, adj;

    SDL_LockMutex(lock);
    swap = last_swap_ns;
    SDL_UnlockMutex(lock);
    if (turbo == 1 && gl_up && threaded && !hidden && vsync_mode != 0 && mhz > 0 && swap != 0 &&
        !SDL_GetAtomicInt(&minimized) && !grx_movie_playing()) {
        refresh_num = 1000000000000ull * FIELD_DEN / (uint64_t)mhz;
        k = (int)((FIELD_NUM + refresh_num / 2) / refresh_num);
        if (k >= 1 && k <= 8) {
            uint64_t f = (uint64_t)k * refresh_num;
            uint64_t d = f > FIELD_NUM ? f - FIELD_NUM : FIELD_NUM - f;
            /* Swaps a while ago (a window being dragged, a GPU stall):
               free running until they come again */
            if (d * 200 < FIELD_NUM && (swap > now || now - swap < 4 * f / FIELD_DEN)) {
                want = f;
            }
        }
    }
    locked = want != FIELD_NUM;
    SDL_SetAtomicInt(&lock_k, locked ? k : 0);
    if (turbo > 1) {
        want = FIELD_NUM / (uint64_t)turbo;
    }
    if (want != field_num) {
        base_ns = field_due(fields);
        base_field = fields;
        field_num = want;
    }
    if (locked != logged) {
        if (locked) {
            grx_log("video: fields locked to the display, %d refresh%s a field (%.3f fields/s)", k,
                    k == 1 ? "" : "es", (double)mhz / 1000.0 / k);
        } else if (logged == 1) {
            grx_log("video: fields at 59.94/s on their own clock");
        }
        logged = locked;
    }
    if (!locked) {
        return;
    }
    /* The next field's due time against the last swap, in refreshes: aim
       for half way, a small step at a time (swaps jitter) */
    r = refresh_num / FIELD_DEN;
    due = field_due(fields);
    ph = (int64_t)(due - swap) % (int64_t)r;
    if (ph < 0) {
        ph += (int64_t)r;
    }
    err = ph - (int64_t)r / 2;
    adj = err / 8;
    if (adj > (int64_t)r / 32) adj = (int64_t)r / 32;
    if (adj < -(int64_t)r / 32) adj = -(int64_t)r / 32;
    base_ns -= (uint64_t)adj;
}

/* During a scene skip (grx_skip.h) the next field is always due now, and
   each wait for a vblank gets several: the fields to run at a wait */
static int skip_due(uint64_t now) {
    int n = grx_skip_fast();
    if (n > 0) {
        base_ns = now;
        base_field = fields;
        return n;
    }
    return 1;
}

/* The EE timers in count mode, on by one field */
static void timers_field(void) {
    static const uint64_t rates[4] = {BUSCLK, BUSCLK / 16, BUSCLK / 256, HSYNC_PER_SEC};
    int t;
    for (t = 0; t < 4; t++) {
        uintptr_t base = 0x10000000u + 0x800u * (uintptr_t)t;
        uint32_t mode = GRX_HW32(base + 0x10);
        if (mode & 0x80) {
            uint64_t r = rates[mode & 3];
            uint32_t ticks = (uint32_t)(r * 1001 * (fields + 1) / FIELD_DEN - r * 1001 * fields / FIELD_DEN);
            GRX_HW32(base) = (GRX_HW32(base) + ticks) & 0xFFFF;
        }
    }
}

static void wake_waiters(void) {
    while (vsync_waiters > 0) {
        vsync_waiters--;
        iSignalSema(vsync_sema);
    }
}

static void log_rate(uint64_t now) {
    uint32_t pc = SDL_GetAtomicU32(&present_count);
    if (log_ns == 0) {
        log_ns = now;
        log_fields = fields;
        log_presents = pc;
        log_flips = flips;
        log_flips_want = flips_want;
        return;
    }
    if (now - log_ns >= 1000000000ull) {
        double s = (double)(now - log_ns) / 1e9;
        uint64_t nf = flips - log_flips;
        char pres[160] = "no window";
        if (gl_up) {
            snprintf(pres, sizeof(pres), "%.2f presents/s (display %.2f Hz; pictures up 1/2/3/4+ times: %d/%d/%d/%d)",
                     (double)(uint32_t)(pc - log_presents) / s, SDL_GetAtomicInt(&refresh_mhz) / 1000.0,
                     SDL_SetAtomicInt(&shown_for[0], 0), SDL_SetAtomicInt(&shown_for[1], 0),
                     SDL_SetAtomicInt(&shown_for[2], 0), SDL_SetAtomicInt(&shown_for[3], 0));
        }
        grx_log("video: %.2f fields/s, %.2f game frames/s (asks for %s fps), %s, %llu late, field %llu",
                (double)(fields - log_fields) / s, (double)nf / s,
                nf == 0                                 ? "?"
                : flips_want - log_flips_want >= 2 * nf ? "60"
                : flips_want - log_flips_want == nf     ? "30"
                                                        : "30/60",
                pres, (unsigned long long)log_late, (unsigned long long)fields);
        log_ns = now;
        log_fields = fields;
        log_presents = pc;
        log_flips = flips;
        log_flips_want = flips_want;
        log_late = 0;
    }
}

/* GRX_SNAPSHOT_DIR (and GRX_SNAPSHOT_EVERY fields, default 60): write the
   displayed picture there as field_NNNNNN.ppm, also in headless runs, to
   see what the game shows without a window */
static void snapshot(void) {
    static int every = -1;
    static const char *dir;
    const uint32_t *px;
    int w, h, pitch, x, y;
    char path[512];
    FILE *f;

    if (every < 0) {
        const char *e = getenv("GRX_SNAPSHOT_EVERY");
        dir = getenv("GRX_SNAPSHOT_DIR");
        every = e != NULL && atoi(e) > 0 ? atoi(e) : 60;
    }
    if (dir == NULL || fields % (uint64_t)every != 0 || grx_gs_display == NULL) {
        return;
    }
    if (!grx_gs_display(&px, &w, &h, &pitch) || px == NULL || w <= 0 || h <= 0) {
        return;
    }
    snprintf(path, sizeof(path), "%s/field_%06llu.ppm", dir, (unsigned long long)fields);
    f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    /* Wider than 4:3 the frame holds the picture squeezed: the comment
       says the aspect to show it at (port/docs/widescreen.md) */
    fprintf(f, "P6\n# aspect %.4f\n%d %d\n255\n", grx_ws_aspect(), w, h);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t c = px[y * pitch + x];
            unsigned char rgb[3] = {(unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c};
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

/* One field's vblank */
static void run_field(void) {
    snapshot();
    if (grx_gs_vsync != NULL) {
        grx_gs_vsync();
    }
    grx_ws_update();
    grx_fp_trap_field(fields);
    timers_field();
    grx_pad_update();
    grx_buttons_field();
    grx_skip_field();
    grx_spot_field();
    fields++;
    grx_dbg_field(fields);
    grx_kernel_field();
    wake_waiters();
}

/* The freeze tool (grx_debug.h): while frozen, no field runs, but the
   window stays alive and the tool's requests are served. Then the field
   clock starts again from now. 1 if it held. */
static int hold(void) {
    if (!grx_dbg_holding() && !grx_menu_open()) {
        return 0;
    }
    if (window != NULL && grx_dbg_holding()) {
        char title[96];
        snprintf(title, sizeof(title), "Grandia Xtreme - frozen at field %llu (F9 resumes, F10 steps)",
                 (unsigned long long)fields);
        SDL_SetWindowTitle(window, title);
    }
    while (grx_dbg_holding() || grx_menu_open()) {
        grx_video_present();
        present_inline();
        grx_video_pump();
        grx_dbg_poll();
        SDL_Delay(5);
    }
    if (window != NULL) {
        SDL_SetWindowTitle(window, "Grandia Xtreme");
    }
    base_ns = SDL_GetTicksNS();
    base_field = fields;
    return 1;
}

/* Run the fields due by now (at least `min`), up to MAX_CATCHUP (or
   `min`); further behind than that, the clock starts again from now.
   During a movie all of them (grx_movie_playing): its audio keeps time. */
static void run_due(uint64_t now, int min) {
    int k = 0, most = grx_movie_playing() ? INT_MAX : min > MAX_CATCHUP ? min : MAX_CATCHUP;
    uint64_t due;
    while (k < most && (k < min || field_due(fields) <= now)) {
        if (hold()) {
            now = SDL_GetTicksNS();
        }
        run_field();
        k++;
    }
    if (k > 1) {
        log_late += (uint64_t)(k - 1);
    }
    if (field_due(fields) <= now) {
        base_ns = now;
        base_field = fields;
    }
    lock_to_display(now);
    /* Pictures from now on are the last field's: stamp them with when it
       was due (the clock, not the jitter of when it ran) */
    due = field_due(fields);
    SDL_LockMutex(lock);
    stamp_field = fields;
    stamp_ns = due > field_num / FIELD_DEN ? due - field_num / FIELD_DEN : 0;
    SDL_UnlockMutex(lock);
    if (cfg.log_fields) {
        log_rate(now);
    }
    if (cfg.seconds > 0 && (double)(now - start_ns) >= cfg.seconds * 1e9) {
        quit("time is up");
    }
}

/* A save state's message in the title for a few seconds */
static void state_title(void) {
    static char shown[160];
    const char *m = grx_state_message();
    char title[200];
    if (window == NULL) {
        return;
    }
    if (m != NULL && strcmp(m, shown) != 0) {
        snprintf(shown, sizeof(shown), "%s", m);
        snprintf(title, sizeof(title), "Grandia Xtreme - %s", m);
        SDL_SetWindowTitle(window, title);
    } else if (m == NULL && shown[0]) {
        shown[0] = 0;
        SDL_SetWindowTitle(window, "Grandia Xtreme");
    }
}

/* Save states: after a load the field clock starts again from now */
void grx_video_state_loaded(void) {
    base_ns = SDL_GetTicksNS();
    base_field = fields;
}

static void idle_hook(void) {
    uint64_t now, due;
    int n;

    /* A save state saved or loaded here, between fields (state.c). After a
       load these are the saved run's frames: back to the kernel at once. */
    if (grx_state_service()) {
        return;
    }
    state_title();
    grx_video_present();
    present_inline();
    grx_video_pump();
    now = SDL_GetTicksNS();
    n = skip_due(now);
    due = field_due(fields);
    if (now < due) {
        SDL_DelayPrecise(due - now);
        now = SDL_GetTicksNS();
    }
    run_due(now, n);
}

void grx_frame_start(void) {
    VSemaParam p;

    if (started) {
        return;
    }
    started = 1;
    if (!sdl_up) {
        /* Headless without grx_video_init: still need the clock */
        if (!SDL_Init(SDL_INIT_EVENTS)) {
            grx_fatal("SDL_Init: %s", SDL_GetError());
        }
        sdl_up = 1;
    }
    if (lock == NULL) {
        lock = SDL_CreateMutex();
        if (lock == NULL) {
            grx_fatal("SDL_CreateMutex: %s", SDL_GetError());
        }
    }
    memset(&p, 0, sizeof(p));
    p.initCount = 0;
    p.maxCount = 256;
    vsync_sema = CreateSema(&p);
    if (vsync_sema < 0) {
        grx_fatal("video: no semaphore for the vblank waits");
    }
    start_ns = base_ns = SDL_GetTicksNS();
    base_field = 0;
    stamp_ns = start_ns;
    grx_kernel_set_idle_hook(idle_hook);
    start_presenter();
}

void grx_frame_wait_vblank(void) {
    if (!started) {
        grx_frame_start();
    }
    if (grx_kernel_in_interrupt() || grx_kernel_current_thread() == 0) {
        /* Can't block here: the vblank comes now */
        uint64_t now = SDL_GetTicksNS();
        uint64_t due;
        int n = skip_due(now);
        due = field_due(fields);
        if (now < due) {
            SDL_DelayPrecise(due - now);
            now = SDL_GetTicksNS();
        }
        run_due(now, n);
        return;
    }
    vsync_waiters++;
    WaitSema(vsync_sema);
}

void grx_frame_poll(void) {
    uint64_t now;
    if (!started || grx_kernel_in_interrupt()) {
        return;
    }
    now = SDL_GetTicksNS();
    skip_due(now);
    if (field_due(fields) <= now) {
        grx_video_present();
        present_inline();
        grx_video_pump();
        run_due(now, 0);
    }
}

void grx_ee_spin(uint32_t loops) {
    /* 3 cycles a pass at 294.912 MHz */
    uint64_t end = SDL_GetTicksNS() + (uint64_t)loops * 3 * 1000000000u / 294912000u;

    do {
        grx_frame_poll();
    } while (SDL_GetTicksNS() < end);
}

uint64_t grx_frame_count(void) {
    return fields;
}

/* ---------------------------------------------------------------------- */
/* libgraph's vblank waits (src/main/48C4E0.c, 48C570.c), which poll
   INTC_STAT's VBLANK_START bit (and the CSR copy SetVSyncFlag's handler
   makes): the next field, through the kernel */

void _sceWaitVSync(void) {
    grx_frame_wait_vblank();
}

int64_t _sceWaitVSyncCsr(void) {
    grx_frame_wait_vblank();
    return (int64_t)GRX_HW64(GRX_HW_GS_PRIV_BASE + 0x1000);
}
