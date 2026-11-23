"""The field's controls (port/docs/controls.md).

- Running takes the left stick three quarters of the way over, not
  nearly all the way: ChrPlayerPadMove runs past 96 of 128 (the game's
  120).
- The first-person view is on SELECT, not L2: ChrPlayerUpdate switches it
  on SELECT, and EvtAreaUpdate keeps circle from talking or examining while
  SELECT is held, as it did for L2. Nothing in the field read SELECT; the
  game uses it only in its soft reset (L1 L2 R1 R2 START SELECT) and the
  debug menu on controller 2.
- L2 readies the party, as cross does: ChrPlayerUpdate's kamae (the party
  draws its weapons, enemies notice it) is held by either.

Pad bits (include/pad.h): 0x0001 L2, 0x0040 cross, 0x0100 select.

The Configurations page has three rows of the port's: Controller and
Keyboard, for the controls' mapping (grx_input.h), and Display, for the
video settings (grx_video_opts.h).
port/game/field/62B3F8.c is FldMenuOptionsUpdate with them, and
FldMenuOptionsInit's window is three rows (0x48) taller, downwards. Its
lists show PlayStation button icons, drawn after the menu's windows
(FldMenuDraw).
"""

# FldMenuOptionsUpdate: port/game/field/62B3F8.c
REPLACES = [
    ("field", 0x62B3F8, 0x62B3F8),
]

FIXUPS = {
    # FldMenuOptionsInit: the page's window, 0x188 x 0xC0 at the screen's
    # centre, for 7 rows of 0x18: three rows taller (Controller, Keyboard,
    # Display), downwards (the field menu's icons are above it)
    "src/field/62B340.c": [
        (r"FldMenuWinOpen\(8, 0x140, 0xE0, 0x188, 0xC0,", r"FldMenuWinOpen(8, 0x140, 0x104, 0x188, 0x108,"),
    ],
    # FldTitleDraw and the attract demo's SysDemoDraw: the PRESS START
    # BUTTON prompt from x 0xB0, moved right while it is PRESS ENTER (on the
    # keyboard, grx_buttons.h) to stay centred
    "src/field/656FF8.c": [
        (r"GphBatchSetOffset\(0xB0, 0x136\);",
         r"{ int grx_buttons_prompt_shift(void); GphBatchSetOffset(0xB0 + grx_buttons_prompt_shift(), 0x136); }"),
    ],
    "src/main/41EBA0.c": [
        (r"GphBatchSetOffset\(0xB0, 0x136\);",
         r"{ int grx_buttons_prompt_shift(void); GphBatchSetOffset(0xB0 + grx_buttons_prompt_shift(), 0x136); }"),
    ],
    # FldMenuDraw: after the menu's guide (over its windows), the keyboard
    # list's icons
    "src/field/61FBD8.c": [
        (r"(\(\(s32 \(\*\)\(void\)\)FldMenuGuideDraw\)\(\);)",
         r"\1 { void FldMenuOptionsIconsDraw(void); FldMenuOptionsIconsDraw(); }"),
    ],
    # ChrPlayerPadMove(o, pad): the left stick's distance from its centre
    # (each axis counted past 61 of 128); running past 120 of it
    "src/field/5F8D18.c": [
        (r"m > 120\.0f", r"m > 96.0f"),
    ],
    # ChrPlayerUpdate(pad): L2 toggles the first-person view, cross held
    # readies the party
    "src/field/5F8FB0.c": [
        (r"if \(\(pad->trigger & 1\) && ChrPlayerIsBusy\(\) == 0\)",
         r"if ((pad->trigger & 0x100) && ChrPlayerIsBusy() == 0)"),
        (r"if \(pad->held & 0x40\) \{", r"if (pad->held & 0x41) {"),
    ],
    # EvtAreaUpdate(pad): circle talks and examines unless the view button
    # is held
    "src/field/5B2418.c": [
        (r"!\(pad->held & 1\)\)", r"!(pad->held & 0x100))"),
    ],
}
