/* Skipping cut scenes (port/docs/scene-skip.md): --scene-skip on|off, or
   GRX_SCENE_SKIP. In a cut scene (a field event the player didn't start
   by talking to someone, or a movie) START shows "Hold START to skip",
   and held for 5 seconds skips the rest. A skip runs the scene
   to its end, as fast as the game goes, behind a black screen and with
   the sound off: messages are passed, and everything the scene does
   (flags, items, party, map changes) still happens. It stops early where
   the player is wanted (a choice, a menu, a mini-game) and at a battle.
   Movies stop at once, as the game's own movie skip does. */
#ifndef GRX_SKIP_H
#define GRX_SKIP_H

#include <stdint.h>

/* The --scene-skip / GRX_SCENE_SKIP setting (NULL: GRX_SCENE_SKIP, else
   on). Fatal on a bad value. */
void grx_skip_config(const char *arg);

/* For the game (port/tools/fixups/skip.py), each field frame after the
   scenario's buttons are read: whether a cut scene is running, the
   message state (EvtMsgCtl.flags, with 0x10000 while the event's
   character select or a mini-game waits for the player), where the
   script is (anything that changes as it runs), and the scenario's
   buttons (EVT_SCNR_KEY_*), which a skip sets to pass messages */
void grx_skip_scene(int scene, unsigned msg_flags, unsigned where, uint16_t *keys);

/* port/src/pad.c, port 1's sampling: the buttons down now (DualShock bits
   as the pad data's, 1 = down) from the keyboard and from the gamepad.
   Returns the buttons the game gets: START held back in a scene, nothing
   during a skip. */
unsigned grx_skip_input(unsigned keys, unsigned pad);

/* port/src/video.c, once per field after the pads */
void grx_skip_field(void);

/* While a skip runs, the fields to run at each wait for a vblank (a drawn
   frame's worth of field frames), else 0. Meanwhile fields come as fast
   as the game goes, the GPU renderer draws nothing, and the sound driver
   runs by fields rather than the audio device. */
int grx_skip_fast(void);

/* For FldUpdate (port/tools/fixups/skip.py): how many field frames to run
   before drawing one, `n` (the game's choice) but during a skip */
int grx_skip_updates(int n);

/* Fast forward (Tab): cycles the speed 1 -> 2 -> 4 -> 1. The field clock
   runs that many times faster while the sound device keeps real time, so
   music and voices play at their own speed. Shown as ">> n". */
void grx_turbo_cycle(void);

/* The speed now: the one picked, but 1 during a scene skip or a movie */
int grx_turbo_speed(void);

/* The presenter (GL context current, default framebuffer bound): the
   prompt and the skip's black screen over the picture, which is at
   (x, y, w, h) in a window of ww x wh pixels */
void grx_skip_draw_gl(int x, int y, int w, int h, int ww, int wh);

/* The prompt's picture (skip_overlay.c), also for port/tests/skip_art.c:
   text_px pixels to the capital height; kind 0 the prompt, 1 "Skipping";
   button 0 START, 1 Enter (keyboard), 2 Xbox Menu. RGBA: R text, G panel,
   B progress bar track. Sets *w, *h and the bar's x range in pixels;
   returns a malloc'd picture or NULL. */
unsigned char *grx_skip_art(int text_px, int kind, int button, int *w, int *h, int *bar_x0, int *bar_x1);

#endif
