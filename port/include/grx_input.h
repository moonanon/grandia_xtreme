/* Controller and keyboard mapping (port/src/input.c, port/docs/controls.md):
   which gamepad input and which key press each of the game's buttons. Set
   on the Configurations page (port/game/field/62B3F8.c) and kept in
   grandia.ini next to grandia.exe; without it, the defaults are the port's
   layout (grx_pad.h, grx_buttons.h). */
#ifndef GRX_INPUT_H
#define GRX_INPUT_H

/* The game's buttons, in the order the pages list them */
enum {
    GRX_IN_CIRCLE, GRX_IN_CROSS, GRX_IN_SQUARE, GRX_IN_TRIANGLE,
    GRX_IN_L1, GRX_IN_R1, GRX_IN_L2, GRX_IN_R2,
    GRX_IN_START, GRX_IN_SELECT, GRX_IN_L3, GRX_IN_R3,
    GRX_IN_UP, GRX_IN_DOWN, GRX_IN_LEFT, GRX_IN_RIGHT,
    GRX_IN_COUNT
};

/* What a game button is on: a gamepad input (an SDL_GamepadButton, or one
   of the triggers below) or a key (an SDL_Scancode). GRX_IN_NONE: nothing */
enum { GRX_IN_PAD, GRX_IN_KEY };
#define GRX_IN_LT 100
#define GRX_IN_RT 101
#define GRX_IN_NONE (-1)

/* The port's own actions, each on one gamepad input and one key (the
   defaults: fast forward on L3 and Tab, the minimap overlay on R3 and M),
   set from the Esc menu (port/src/menu.c), kept in grandia.ini's [port] */
enum { GRX_ACT_FAST_FORWARD, GRX_ACT_MAP_OVERLAY, GRX_ACT_COUNT };
int grx_action_get(int kind, int a);
void grx_action_set(int kind, int a, int code);
void grx_action_reset(void);
const char *grx_action_label(int a);
/* Whether an action may go on a key (not one of the game's buttons', nor
   Esc or F1-F12) or a gamepad input (not one of the game's buttons', but
   for L3 and R3) */
int grx_action_key_ok(int key);
int grx_action_pad_ok(int code);
/* Whether a game button may go on a key (not the port's own) or a gamepad
   input (not a port action's) */
int grx_input_key_ok(int key);
int grx_input_pad_ok(int code);
/* The action on key (an SDL_Scancode), or -1 */
int grx_action_for_key(int key);
/* Runs action a (the presenter's thread or the pads') */
void grx_action_run(int a);
/* A gamepad input's PS2 name (Cross, L3...), whatever the layout */
const char *grx_input_name_ps2(int code);

/* Once, before the pads: reads grandia.ini */
void grx_input_init(void);

/* What game button b is on */
int grx_input_get(int kind, int b);

/* Game button b's bit in the DualShock data (bytes 2 and 3 as one
   big-endian word, as pad.c's B_*) */
unsigned grx_input_bit(int b);

/* An input's name for the pages, at most 14 characters: a gamepad's as
   the layout names it (A or cross for the south button), a key's SDL's */
const char *grx_input_name(int kind, int code);

/* A gamepad input's icon (grx_buttons_icon's i) as the layout shows it,
   or -1: the face buttons, the shoulder buttons and the triggers (the
   south button is A, the circle icon, with the Xbox layout, and cross
   with the PlayStation one) */
int grx_input_icon(int code);

/* Putting a button on an input: grx_input_capture starts, and
   grx_input_captured says, each frame, -2 while waiting, -1 when cancelled
   (Escape, or 5 seconds), else the input pressed. From the start until
   everything is let go after, the game sees nothing pressed. */
void grx_input_capture(int kind);
int grx_input_captured(void);
/* 1 while the Configurations page waits for a key (Esc cancels it) */
int grx_input_capturing(void);

/* Puts game button b on `code`; a button that was on it gets b's old
   input, so every button keeps one. Saves grandia.ini. */
void grx_input_set(int kind, int b, int code);

/* Back to the defaults; saves grandia.ini */
void grx_input_reset(int kind);

/* Writes grandia.ini: the controls and the video settings (grx_video_opts.h) */
void grx_settings_save(void);

/* pad.c, port 1, each field: the first key and the first gamepad input
   down (GRX_IN_NONE if none), whether anything at all is, and Escape.
   1 while the game should see nothing pressed. */
int grx_input_poll(int key, int pad, int any, int escape);

#endif
