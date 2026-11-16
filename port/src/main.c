/* The port's entry point and diagnostics.

   Start-up order (grandia.exe --boot):
     1. the crash reporter;
     2. grx_hwmem_init: the PS2's fixed addresses, then the game's memory
        below 0x08000000 (grx_memory_init, the SIF directory scratch with
        its alias views), before anything else (SDL's drivers, the OpenGL driver,
        their threads' stacks) takes that address space. SDL3.dll has to
        be delay-loaded (port/tools/build.py) for the same reason: loaded
        with the exe, its imports (user32, gdi32) map views over the IOP
        block's 0x01FF0000 before main runs;
     3. SDL (video, gamepads, events), the window and its OpenGL context,
        the pads (grx_video_init);
     4. the GS (grx_gs_init) and its renderer (grx_gs_gl_init, which makes
        its GL context here), then audio (grx_audio_init: SDL's audio
        stream, which the sound driver in port/src/snd/ renders into);
     5. the kernel, which adopts this OS thread as the game's main thread,
        and the frame loop's idle hook (grx_frame_start), which also hands
        the GL context to the presenter thread: GL contexts shared with it
        (grx_video_gl_create_context, for a GPU renderer) are made before
        this, in step 4;
     6. GameMain (crt0, then SysMain).
   grx_hwmem_init reserves all the free memory below 0x08000000 for the
   game (grx_hwmem_alloc_low hands it out), so what steps 3 to 5 load and
   allocate (the GL driver, thread stacks) goes above it. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "grx_fiber.h"
#include "grx_platform.h"
#include "grx_hwmem.h"
#include "grx_kernel.h"
#include "grx_video.h"
#include "grx_widescreen.h"
#include "grx_draw_distance.h"
#include "grx_debug.h"
#include "grx_buttons.h"
#include "grx_camera.h"
#include "grx_skip.h"
#include "grx_pad.h"
#include "grx_memory.h"
#include "grx_sif.h"
#include "grx_audio.h"
#include "grx_mc.h"
#include "grx_gl.h"
#include "grx_state.h"

int GameMain(int argc, char **argv);

/* The GS (port/src/gs/) may not be linked yet */
extern void grx_gs_init(void) __attribute__((weak));
extern int grx_gs_gl_init(void) __attribute__((weak));

/* The game's pad code, for --pad-test: PadInit (42D310), PadUpdate
   (42D388) and the two-entry Pad array (pad.h; 0x54 bytes each) */
int PadInit(void);
void PadUpdate(void *pads);
extern unsigned char padPort0[];
extern int padPhase[2];

void grx_log(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fputs(line, stderr);
    fputc('\n', stderr);
    /* The freeze tool keeps the last lines */
    grx_dbg_log_line(line);
}

void grx_fatal(const char *fmt, ...) {
    va_list ap;
    fputs("grandia: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void grx_unimplemented(const char *name) {
    grx_fatal("not in the port yet: %s", name);
}

static void usage(void) {
    printf("Grandia Xtreme port: it can't boot all the way yet (port/README.md).\n"
           "\n"
           "  grandia --boot [options]        boot the game anyway\n"
           "  grandia --video-test [options]  the frame loop alone: the game's vsync, a test picture\n"
           "  grandia --pad-test [options]    the game's pad code: prints what it reads\n"
           "\n"
           "  --disc PATH      the game's disc image (.iso/.mds; or set GRX_DISC)\n"
           "  --scale N        window size: 480 lines times N (default 2), the width to the aspect\n"
           "  --aspect A       4:3 (the original), 16:9, W:H, or auto (the default: follows the\n"
           "                   window's shape; or set GRX_ASPECT)\n"
           "  --hud H          how wide a screen the HUD follows to its sides: 4:3 (it stays\n"
           "                   where it is at 4:3), 16:9, 21:9 (the default), W:H or none (no\n"
           "                   limit; or set GRX_HUD)\n"
           "  --draw-distance N  how far the scene reaches before the fog: N times the\n"
           "                   original (default 4, 1 is the original, up to 8; or set\n"
           "                   GRX_DRAW_DISTANCE)\n"
           "  --buttons B      auto (the default: by the gamepad), xbox (A confirms, and A B X Y\n"
           "                   LB RB LT RT icons) or playstation (or set GRX_BUTTONS)\n"
           "  --camera C       stick (the default: the right stick turns and tilts the field\n"
           "                   camera; L1 and R1 still turn it) or buttons (L1 and R1 only, as\n"
           "                   the original; or set GRX_CAMERA)\n"
           "  --scene-skip S   on (the default: in a cut scene, hold START for 5 seconds to\n"
           "                   skip it) or off (or set GRX_SCENE_SKIP)\n"
           "  --fullscreen    start fullscreen (Alt+Enter, F11 or a double click toggles)\n"
           "  --integer        scale by whole multiples of the picture's height\n"
           "  --sharp, --smooth  nearest or linear filtering\n"
           "  --renderer R     gl (the GPU; default with a window) or software (the CPU, for\n"
           "                   weaker GPUs; or set GRX_GS_RENDERER)\n"
           "  --headless       no window (with --renderer gl, a hidden one for the GPU renderer)\n"
           "  --seconds S      exit after S seconds\n"
           "  --log-fields     log the field and present rates every second\n"
           "\n"
           "  F9 freezes the game and saves what led up to it (F9 again resumes), F10 steps\n"
           "  a field: port/docs/debug.md, port/tools/grxdbg.py\n"
           "\n"
           "  GRX_GS_SCALE=N      the GPU renderer's internal scale (default: fills the window)\n"
           "  GRX_VIDEO_THREAD=0  present on the game thread once per field, without vsync\n");
}

/* The GS's renderer: --renderer, else GRX_GS_RENDERER, else GL when the
   window has OpenGL 3.3; software when GL isn't there or doesn't start */
static void start_renderer(const char *want, int booting) {
    int gl;
    if (want == NULL || !*want) {
        want = getenv("GRX_GS_RENDERER");
    }
    if (want == NULL || !*want || strcmp(want, "auto") == 0) {
        /* (--video-test and --pad-test draw nothing through the GS) */
        gl = booting;
    } else if (strcmp(want, "gl") == 0 || strcmp(want, "opengl") == 0) {
        gl = 1;
    } else if (strcmp(want, "software") == 0 || strcmp(want, "sw") == 0) {
        gl = 0;
    } else {
        grx_fatal("--renderer / GRX_GS_RENDERER: gl or software, not \"%s\"", want);
    }
    if (!gl) {
        grx_log("gs: software renderer");
        return;
    }
    if (!grx_video_gl_available()) {
        grx_log("gs: no OpenGL 3.3 window (headless, or no GL): software renderer");
    } else if (grx_gs_gl_init == NULL || grx_gs_gl_init() != 0) {
        grx_log("gs: the GPU renderer didn't start: software renderer");
    }
}

static void set_disc(const char *path) {
#ifdef _WIN32
    if (_putenv_s("GRX_DISC", path) != 0) {
#else
    if (setenv("GRX_DISC", path, 1) != 0) {
#endif
        grx_fatal("can't set GRX_DISC");
    }
}

static void map_hardware(int booting) {
    /* The PS2 addresses the game uses literally, mapped before anything
       else takes the address space (port/docs/hardware.md) */
    (void)booting;
    if (grx_hwmem_init() != 0) {
        int i;
        for (i = 0; i < GRX_HW_NRANGES; i++) {
            const GrxHwRange *r = grx_hwmem_range(i);
            if (!r->mapped) {
                grx_log("couldn't map %s at 0x%08X (error %lu, taken by 0x%08X)", r->name, r->base, r->error,
                        r->blocker);
            }
        }
        grx_fatal("couldn't map the PS2 hardware addresses");
    }
    /* The game's low memory, while the address space is still free (the
       window's OpenGL driver would take much of it): both allocate once and
       hand out the same block later */
    grx_memory_init();
    grx_sif_dir_scratch();
}

/* --video-test: the game's own vsync code (GsVsyncInit, GsVsyncFlip and
   its vblank handler) on the frame loop, with a test picture */
void GsVsyncInit(void);
int GsVsyncFlip(int n);
int GsGetVblankCount(void);

static int video_test(int gl_test) {
    int frame = 0;
    GsVsyncInit();
    grx_log("video-test: GsVsyncInit done at field %llu, vblank count %d", (unsigned long long)grx_frame_count(),
            GsGetVblankCount());
    for (;;) {
        GsVsyncFlip(frame & 1);
        if (gl_test) {
            grx_gl_test_frame(grx_frame_count());
        }
        frame++;
        if (frame % 120 == 0) {
            grx_log("video-test: %d flips, field %llu, the game's vblank count %d", frame,
                    (unsigned long long)grx_frame_count(), GsGetVblankCount());
        }
    }
}

/* --pad-test: PadInit, then PadUpdate every field, printing what changes */
static int pad_test(void) {
    unsigned char last[2][0x54];
    char line[2][256], prev[2][256];
    int i;

    memset(last, 0, sizeof(last));
    memset(prev, 0, sizeof(prev));
    if (!PadInit()) {
        grx_fatal("pad-test: PadInit failed");
    }
    grx_log("pad-test: keyboard: arrows, Z cross, X circle, A square, S triangle, Q L1, W R1, E L2, R R2, "
            "Enter start, Backspace select");
    for (;;) {
        PadUpdate(padPort0);
        for (i = 0; i < 2; i++) {
            const unsigned char *p = padPort0 + i * 0x54;
            grx_pad_describe(i, line[i], sizeof(line[i]));
            if (strcmp(line[i], prev[i]) != 0 || memcmp(p, last[i], 0x24) != 0) {
                unsigned held = p[0] | (p[1] << 8), trig = p[4] | (p[5] << 8);
                printf("field %6llu  %s\n              game: phase %02X id %d press %d held %04X trig %04X "
                       "L %02X,%02X R %02X,%02X\n",
                       (unsigned long long)grx_frame_count(), line[i], padPhase[i], p[0x0C], p[0x0D], held, trig,
                       p[0x20], p[0x21], p[0x1E], p[0x1F]);
                fflush(stdout);
                strcpy(prev[i], line[i]);
                memcpy(last[i], p, 0x54);
            }
        }
        grx_frame_wait_vblank();
    }
}

#ifdef _WIN32
int main(int argc, char **argv)
#else
static int grx_main(int argc, char **argv);
int main(int argc, char **argv) {
    return grx_run_on_low_stack(grx_main, argc, argv);
}
static int grx_main(int argc, char **argv)
#endif
{
    GrxVideoConfig vc;
    const char *mode = NULL, *renderer = NULL, *aspect = NULL, *draw_dist = NULL, *buttons = NULL, *camera = NULL;
    const char *scene_skip = NULL, *hud = NULL;
    char **rest;
    int nrest = 0, i, gl_test = 0;

    grx_crash_init();
    memset(&vc, 0, sizeof(vc));
    vc.scale = 2;
    vc.smooth = -1;
    rest = calloc((size_t)argc + 1, sizeof(char *));
    if (rest == NULL) {
        grx_fatal("out of memory");
    }
    rest[nrest++] = argv[0];
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--boot") == 0 || strcmp(a, "--pad-test") == 0 || strcmp(a, "--video-test") == 0) {
            mode = a;
        } else if (strcmp(a, "--scale") == 0 && i + 1 < argc) {
            vc.scale = atoi(argv[++i]);
            if (vc.scale < 1 || vc.scale > 16) {
                grx_fatal("--scale: 1 to 16");
            }
        } else if (strcmp(a, "--fullscreen") == 0) {
            vc.fullscreen = 1;
        } else if (strcmp(a, "--integer") == 0) {
            vc.integer = 1;
        } else if (strcmp(a, "--sharp") == 0) {
            vc.smooth = 0;
        } else if (strcmp(a, "--smooth") == 0) {
            vc.smooth = 1;
        } else if (strcmp(a, "--headless") == 0) {
            vc.headless = 1;
        } else if (strcmp(a, "--log-fields") == 0) {
            vc.log_fields = 1;
        } else if (strcmp(a, "--seconds") == 0 && i + 1 < argc) {
            vc.seconds = atof(argv[++i]);
        } else if (strcmp(a, "--aspect") == 0 && i + 1 < argc) {
            aspect = argv[++i];
        } else if (strcmp(a, "--hud") == 0 && i + 1 < argc) {
            hud = argv[++i];
        } else if (strcmp(a, "--draw-distance") == 0 && i + 1 < argc) {
            draw_dist = argv[++i];
        } else if (strcmp(a, "--buttons") == 0 && i + 1 < argc) {
            buttons = argv[++i];
        } else if (strcmp(a, "--camera") == 0 && i + 1 < argc) {
            camera = argv[++i];
        } else if (strcmp(a, "--scene-skip") == 0 && i + 1 < argc) {
            scene_skip = argv[++i];
        } else if (strcmp(a, "--renderer") == 0 && i + 1 < argc) {
            renderer = argv[++i];
        } else if (strcmp(a, "--disc") == 0 && i + 1 < argc) {
            set_disc(argv[++i]);
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage();
            return 0;
        } else {
            rest[nrest++] = argv[i];
        }
    }
    if (mode == NULL) {
        /* The game can't boot all the way yet (port/README.md, "Status"),
           so booting it is opt-in */
        usage();
        return 0;
    }

    map_hardware(strcmp(mode, "--boot") == 0);
    /* A loop that never lets a field run is a hang: report where it is
       (GRX_WATCHDOG=seconds, 0 for off) */
    if (strcmp(mode, "--boot") == 0) {
        const char *w = getenv("GRX_WATCHDOG");
        grx_watchdog_start(w != NULL ? (unsigned)atoi(w) : 10);
        grx_profile_start();
    }
    vc.test_pattern = strcmp(mode, "--video-test") == 0;
    {
        /* --headless --renderer gl: the GPU renderer in a hidden window */
        const char *r = renderer != NULL ? renderer : getenv("GRX_GS_RENDERER");
        vc.hidden_gl = vc.headless && r != NULL && (strcmp(r, "gl") == 0 || strcmp(r, "opengl") == 0);
    }
    grx_ws_config(aspect);
    grx_ws_hud_config(hud);
    grx_dd_config(draw_dist);
    grx_buttons_config(buttons);
    grx_cam_config(camera);
    grx_skip_config(scene_skip);
    grx_video_init(&vc);
    /* Registered first, so it runs last: its SDL_Quit after everything
       else that uses SDL has shut down */
    atexit(grx_video_shutdown);
    if (grx_gs_init != NULL) {
        grx_gs_init();
        grx_ws_update();
        start_renderer(renderer, strcmp(mode, "--boot") == 0);
        grx_dbg_init();
    } else {
        grx_log("video: no GS in this build, no picture");
    }
    /* --video-test's checks of the presenter's GL interfaces (gl_test.c):
       a stand-in GPU renderer, whose shared context has to exist before
       grx_frame_start, and a stand-in interpolation hook */
    if (vc.test_pattern && grx_video_gl_available()) {
        const char *e = getenv("GRX_VIDEO_GL_TEST");
        gl_test = e != NULL && atoi(e) != 0 && grx_gl_test_init() == 0;
        e = getenv("GRX_VIDEO_HOOK_TEST");
        if (e != NULL && atoi(e) != 0) {
            grx_gl_test_hook_install();
        }
    }
    /* Without a device (or with GRX_AUDIO=0) the driver runs by wall clock,
       so the game's waits on sound state still finish */
    if (grx_audio_init() != 0) {
        grx_log("audio: no output");
    }
    atexit(grx_audio_quit);
    /* Memory cards are host directories (port/src/mc.c; GRX_MC_DIR,
       GRX_MC1, GRX_MC2); flush their metadata on the way out */
    atexit(grx_mc_shutdown);
    grx_kernel_init();
    grx_frame_start();

    if (strcmp(mode, "--video-test") == 0) {
        return video_test(gl_test);
    }
    if (strcmp(mode, "--pad-test") == 0) {
        return pad_test();
    }
    /* The save states' stack is the game's, from here down (state.c) */
    return grx_state_run_game(GameMain, nrest, rest);
}
