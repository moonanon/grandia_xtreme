/* FldMenuOptionsUpdate (0x0062B3F8), the Configurations page, with three
   rows of the port's after the game's seven: Controller and Keyboard,
   which open a list of the game's buttons and what each is on, to change
   (grx_input.h, port/docs/controls.md), and Display, which opens a page
   of the video settings, Resolution, Frame rate and Aspect ratio, whose
   values left and right (or confirm) change at once (grx_video_opts.h).
   The game's rows are as in src/field/62B3F8.c; FldMenuOptionsInit's
   window is taller for the three (port/tools/fixups/controls.py).

   The new rows' text is the port's own: FldMenuWinTextAdd with message
   index -1 takes a string (TxtMsgGetCodes), each character one byte (the
   font's glyph + 0x20) or two (0xF0 | page, low byte; glyph (page + 1) *
   0xD0 - 0x20 + low). The font's ASCII is glyph c + 0x81F (the game's
   own strings: A 0x860, a 0x880, 0 0x84F, '(' 0x847, '.' 0x84D), and
   space 0x89F; so page 9.

   All the page's text shares text set 1: 28 lines of up to 14 characters,
   slot s on line s (s < 14) or the right half of line s - 14. The game
   puts the title in 0, its rows' names in 1-7 and their values in 8-14;
   the names in 1 and 2 run into the right half (slots 15 and 16), and the
   value popup uses 17 and 18. The port's three names go in 19 to 21.
   While a list or the Display page is open, the page behind it draws no
   text and they have them all. */
#include "common.h"
#include "protos/field.h"
#include "grxsys.h"
#include "pad.h"
#include "save.h"
#include "field/n23.h"
#include "field/menu.h"
#include "grx_buttons.h"
#include "grx_input.h"
#include "grx_video_opts.h"

s32 FldMenuWinTextAdd(s32, s32, s32, s32, s32, s32, s32, s32, s32, s32);

#define ROWS 10         /* the game's 7 and the port's 3 */
#define ACT_ROW GRX_IN_COUNT /* the port's actions' first row (grx_input.h) */
#define DEFAULTS_ROW (GRX_IN_COUNT + GRX_ACT_COUNT)
#define LIST_ROWS (DEFAULTS_ROW + 1) /* the buttons, the port's actions and "Use defaults" */
#define LIST_SHOWN 9    /* rows the list shows at once, three texts a row */
#define LIST_Y 0x26     /* the list's first row in its window */
#define LIST_H 0x16

#define GROW 0x48       /* the page's window is taller, downwards (controls.py) */
#define PORT_SLOT 19    /* the port's row names' first text slot */

#define TXT(win, s, x, y, size, slot, color) \
    FldMenuWinTextAdd(win, 1, Str(s), x, y, size, size, slot, -1, color)

/* An ASCII string in the font's codes (above), for FldMenuWinTextAdd,
   which reads it at once */
static s32 Str(const char *s) {
    static u8 buf[2 * 15 + 1];
    s32 i = 0;
    for (; *s != 0 && i < 2 * 14; s++) {
        u8 c = (u8)*s;
        buf[i++] = c > 0x20 && c < 0x7F ? (u8)(c + 0x1F) : 0x9F;
        buf[i++] = 0xF9;
    }
    buf[i] = 0;
    return (s32)buf;
}

static const char *const port_rows[3] = {"Controller", "Keyboard", "Display"};

/* The lists name each button by what it does (in the field and the menus,
   with the port's controls: port/docs/controls.md) and by the PlayStation
   button it is */
static const char *const does[GRX_IN_COUNT] = {
    "Confirm", "Cancel/Ready", "Map zoom", "Menu", "Turn camera", "Turn camera", "Ready", "Reset camera",
    "Skip scene", "First person", "", "", "Up", "Down", "Left", "Right",
};
static const char *const ps_buttons[GRX_IN_COUNT] = {
    "Circle", "Cross", "Square", "Triangle", "L1", "R1", "L2", "R2",
    "Start", "Select", "L3", "R3", "D-pad", "D-pad", "D-pad", "D-pad",
};

/* The list: which kind (grx_input.h), the cursor and the first row shown */
static s32 list_kind, list_cursor, list_top;

/* rowX has room for 10 (padB follows it, zeroed with it by
   FldMenuOptionsInit and not used) */
#define ROWX(i) (((s8 *)fldMenuOptions.rowX)[i])

/* The list's window, from the page's top (the field menu's icons are
   above it) down; the page's closes meanwhile (it would be drawn over the
   list) and opens again after, as FldMenuOptionsInit opens it */
static void ListOpen(s32 kind) {
    s32 h = LIST_Y + LIST_SHOWN * LIST_H + 0x10;

    list_kind = kind;
    list_cursor = 0;
    list_top = 0;
    FldMenuWinClose(8, 0, 3);
    FldMenuWinOpen(9, 0x140, 0x80 + h / 2, 0x200, h, 0, 0, 0x8008);
    FldMenuWinSetActive(9);
}

static void ListClose(void) {
    FldMenuWinClose(9, 0, 3);
    FldMenuWinOpen(8, 0x140, 0xE0 + GROW / 2, 0x188, 0xC0 + GROW, 0, 0, 0x88010);
    FldMenuWinSetActive(8);
}

/* The icons on the list this frame, drawn after the menu's windows: rects
   of the icons texture (0xEC) and where they go in window 9, 20 high */
#define ICON_X 0xE0     /* the PlayStation button's column */
#define VALUE_X 0x158   /* what it is on */
static struct {
    s16 u, v, w, h, x, y;
} icons[2 * LIST_SHOWN];
static s32 n_icons;

static void AddIcon(s32 x, s32 y, s32 u, s32 v, s32 w, s32 h) {
    icons[n_icons].u = (s16)u;
    icons[n_icons].v = (s16)v;
    icons[n_icons].w = (s16)w;
    icons[n_icons].h = (s16)h;
    icons[n_icons].x = (s16)x;
    icons[n_icons].y = (s16)y;
    n_icons++;
}

/* From FldMenuDraw (port/tools/fixups/controls.py), after the windows: the
   icons ListDraw put down this frame, at window 9's place (its text is
   from its centre less half its size, as FldMenuWinDraw places it) */
void FldMenuOptionsIconsDraw(void) {
    static SprRect r[2 * LIST_SHOWN];
    SprRectListEntry parts[2 * LIST_SHOWN + 1];
    FldMenuWin *e = &fldMenuWins[9];
    s32 i, x0, y0;

    if (n_icons == 0 || !e->active) {
        n_icons = 0;
        return;
    }
    x0 = (s32)(e->pos[0] + 0.5f) - (e->w2 >> 1);
    y0 = (s32)(e->pos[1] + 0.5f) - (e->h2 >> 1);
    for (i = 0; i < n_icons; i++) {
        r[i].w = (s16)(icons[i].w * 20 / icons[i].h);
        r[i].h = 20;
        r[i].u0 = (u16)icons[i].u;
        r[i].v0 = (u16)icons[i].v;
        r[i].u1 = (u16)(icons[i].u + icons[i].w);
        r[i].v1 = (u16)(icons[i].v + icons[i].h);
        parts[i].rect = &r[i];
        parts[i].x = (s16)(x0 + icons[i].x);
        parts[i].y = (s16)(y0 + icons[i].y);
    }
    parts[i].rect = NULL;
    n_icons = 0;
    GphTexBind(0xEC);
    SprBegin(0.0f, 0.0f);
    SprDrawRectList(parts);
    SprFlush();
}

/* The list's text and cursor; slots from n. Each row: what the button
   does, the PlayStation button (its icon, where the texture has one) and
   the gamepad input or key it is on (a gamepad's as the layout's icon,
   where it has one: A, B, X, Y, LB ... for the Xbox layout, the
   PlayStation buttons for the other). */
static void ListDraw(s32 n, s32 capturing) {
    s32 i, y, b;
    s32 keys = list_kind == GRX_IN_KEY;
    s32 u, v, w, icon;
    const char *name;
    const char *value;

    if (list_cursor < list_top) {
        list_top = list_cursor;
    } else if (list_cursor >= list_top + LIST_SHOWN) {
        list_top = list_cursor - LIST_SHOWN + 1;
    }
    n_icons = 0;
    TXT(9, port_rows[list_kind], 0x20, 6, 0x16, n++, 2);
    for (i = 0; i < LIST_SHOWN && list_top + i < LIST_ROWS; i++) {
        b = list_top + i;
        y = LIST_Y + i * LIST_H;
        if (b == DEFAULTS_ROW) {
            name = "Use defaults";
            value = "";
        } else if (b >= ACT_ROW) {
            name = grx_action_label(b - ACT_ROW);
            if (capturing && b == list_cursor) {
                value = keys ? "Press a key" : "Press a button";
            } else {
                value = grx_input_name(list_kind, grx_action_get(list_kind, b - ACT_ROW));
            }
        } else {
            name = does[b];
            if (capturing && b == list_cursor) {
                value = keys ? "Press a key" : "Press a button";
            } else {
                value = grx_input_name(list_kind, grx_input_get(list_kind, b));
            }
        }
        if (*name) {
            TXT(9, name, 0x24, y, b == list_cursor ? 0x16 : 0x14, n++, b == list_cursor ? 8 : 0);
        }
        /* Icons instead of names where there are (FldMenuOptionsIconsDraw) */
        if (b < 8 && grx_buttons_ps_icon(b, &u, &v, &w)) {
            AddIcon(ICON_X, y, u, v, w, 16);
        } else if (b < ACT_ROW) {
            TXT(9, ps_buttons[b], ICON_X, y, 0x14, n++, b == list_cursor ? 8 : 0);
        }
        icon = -1;
        if (!keys && b < DEFAULTS_ROW && !(capturing && b == list_cursor)) {
            icon = grx_input_icon(b < ACT_ROW ? grx_input_get(GRX_IN_PAD, b) : grx_action_get(GRX_IN_PAD, b - ACT_ROW));
        }
        if (icon >= 0 && grx_buttons_icon(icon, &u, &v, &w)) {
            AddIcon(VALUE_X, y, u, v, w, 32);
        } else if (*value) {
            TXT(9, value, VALUE_X, y, 0x14, n++, 2);
        }
    }
    FldMenuCursorSetAt(9, 20.0f, (f32)((list_cursor - list_top) * LIST_H + LIST_Y) + 12.0f);
}

/* Up and down on a list of n rows, as the item list moves (repeatFast: a
   press, then every frame once held), and a press past an end wraps
   round, as the game's page does, where held it stops there: the cursor
   after */
static s32 CursorStep(s32 cur, s32 n) {
    u16 fast = padPort0[0].repeatFast;
    u16 trig = padPort0[0].trigger;

    if (fast & 0x1000) {
        return cur != 0 ? cur - 1 : (trig & 0x1000) ? n - 1 : cur;
    }
    if (fast & 0x4000) {
        return cur < n - 1 ? cur + 1 : (trig & 0x4000) ? 0 : cur;
    }
    return cur;
}

/* State 2, the list: 3 to stay */
static s32 ListUpdate(s32 n) {
    u16 trig = padPort0[0].trigger;
    s32 c = CursorStep(list_cursor, LIST_ROWS);

    ListDraw(n, 0);
    if (trig & 0x20) {
        SndSePlay(0x7D2, 0, 0, 0x1E, 0);
        if (list_cursor == DEFAULTS_ROW) {
            grx_input_reset(list_kind);
            grx_action_reset();
        } else {
            grx_input_capture(list_kind);
            fldMenuOptions.state = 3;
        }
    } else if (trig & 0x40) {
        SndSePlay(0x7D3, 0, 0, 0x1E, 0);
        ListClose();
        fldMenuOptions.state = 0;
    } else if (c != list_cursor) {
        list_cursor = c;
        SndSePlay(0x7D1, 0, 0, 0x1E, 0);
    }
    return 3;
}

/* State 3, waiting for the press to put on the button */
static s32 ListCapture(s32 n) {
    s32 got = grx_input_captured();

    ListDraw(n, got == -2);
    if (got == -1) {
        SndSePlay(0x7D3, 0, 0, 0x1E, 0);
        fldMenuOptions.state = 2;
    } else if (got >= 0 && list_cursor >= ACT_ROW) {
        /* A port action: not on one of the game's buttons' inputs */
        if (list_kind == GRX_IN_KEY ? grx_action_key_ok(got) : grx_action_pad_ok(got)) {
            grx_action_set(list_kind, list_cursor - ACT_ROW, got);
            SndSePlay(0x7D2, 0, 0, 0x1E, 0);
        } else {
            SndSePlay(0x7D3, 0, 0, 0x1E, 0);
        }
        fldMenuOptions.state = 2;
    } else if (got >= 0) {
        grx_input_set(list_kind, list_cursor, got);
        SndSePlay(0x7D2, 0, 0, 0x1E, 0);
        fldMenuOptions.state = 2;
    }
    return 3;
}

/* The Display page (state 4): the video settings (grx_video_opts.h), one
   a row, in the list's window. Up and down choose, left and right (or
   confirm, onwards) change the value at once, cancel goes back */
#define DISP_ROWS 3
#define DISP_H 0x1C
static const char *const disp_rows[DISP_ROWS] = {"Resolution", "Frame rate", "Aspect ratio"};
static s32 disp_cursor;

static void DisplayOpen(void) {
    s32 h = LIST_Y + DISP_ROWS * DISP_H + 0x10;

    disp_cursor = 0;
    FldMenuWinClose(8, 0, 3);
    FldMenuWinOpen(9, 0x140, 0x80 + h / 2, 0x200, h, 0, 0, 0x8008);
    FldMenuWinSetActive(9);
}

static const char *DisplayValue(s32 row) {
    switch (row) {
    case 0: return grx_opts_resolution_name(grx_opts_resolution());
    case 1: return grx_opts_frame_rate() == 30 ? "30" : "60";
    default: return grx_opts_aspect_name(grx_opts_aspect());
    }
}

/* One step left (-1) or right (+1), round */
static void DisplayStep(s32 row, s32 d) {
    s32 v;
    switch (row) {
    case 0:
        v = grx_opts_resolution() + d;
        grx_opts_set_resolution(v < 0 ? GRX_RES_COUNT - 1 : v >= GRX_RES_COUNT ? 0 : v);
        break;
    case 1:
        grx_opts_set_frame_rate(grx_opts_frame_rate() == 30 ? 60 : 30);
        break;
    default:
        v = grx_opts_aspect() + d;
        grx_opts_set_aspect(v < 0 ? GRX_ASPECT_COUNT - 1 : v >= GRX_ASPECT_COUNT ? 0 : v);
        break;
    }
}

static s32 DisplayUpdate(s32 n) {
    u16 trig = padPort0[0].trigger;
    s32 i, y, c, sel;
    char v[16];

    TXT(9, "Display", 0x20, 6, 0x16, n++, 2);
    for (i = 0; i < DISP_ROWS; i++) {
        const char *t = DisplayValue(i);
        y = LIST_Y + i * DISP_H;
        sel = i == disp_cursor;
        TXT(9, disp_rows[i], 0x24, y, sel ? 0x16 : 0x14, n++, sel ? 8 : 0);
        c = 0;
        if (sel) {
            v[c++] = '<';
            v[c++] = ' ';
        }
        while (*t != 0 && c < 12) {
            v[c++] = *t++;
        }
        if (sel) {
            v[c++] = ' ';
            v[c++] = '>';
        }
        v[c] = 0;
        TXT(9, v, sel ? 0x10C : 0x118, y, 0x14, n++, 2);
    }
    FldMenuCursorSetAt(9, 20.0f, (f32)(disp_cursor * DISP_H + LIST_Y) + 12.0f);

    c = CursorStep(disp_cursor, DISP_ROWS);
    if (trig & 0x40) {
        SndSePlay(0x7D3, 0, 0, 0x1E, 0);
        ListClose();
        fldMenuOptions.state = 0;
    } else if (trig & (0x20 | 0x2000 | 0x8000)) {
        DisplayStep(disp_cursor, (trig & 0x8000) ? -1 : 1);
        SndSePlay(0x7D1, 0, 0, 0x1E, 0);
    } else if (c != disp_cursor) {
        disp_cursor = c;
        SndSePlay(0x7D1, 0, 0, 0x1E, 0);
    }
    return 3;
}

s32 FldMenuOptionsUpdate(void) {
    s32 i;
    s32 lim;
    s32 k;
    s32 h;
    s32 y;
    s32 n;
    u16 trig;

    lim = 0x2A;
    /* The rows step left by 6; the port's two line up with the last */
    for (i = 0; i < ROWS; i++, lim -= i < 7 ? 6 : 0) {
        if (i == fldMenuOptions.cursor) {
            ROWX(i) -= 8;
            if (ROWX(i) < lim) {
                ROWX(i) = lim;
            }
        } else {
            ROWX(i) += 8;
            if (ROWX(i) >= lim + 0x10) {
                ROWX(i) = lim + 0x10;
            }
        }
    }

    FldMenuWinTextClear(8);
    FldMenuWinTextClear(9);
    n = 0;
    /* The list covers the page: the text slots are the list's */
    if (fldMenuOptions.state == 2) {
        return ListUpdate(n);
    }
    if (fldMenuOptions.state == 3) {
        return ListCapture(n);
    }
    if (fldMenuOptions.state == 4) {
        return DisplayUpdate(n);
    }

    k = 6;
    h = FldMenuWinGetQuadBuf(8);
    y = 8;
    FldMenuWinTextAdd(8, 1, sysGrxsysPac->msg->tbl[4], 0x100, 0xC2 + GROW, 0x18, 0x14, n++, 0x15, k);
    for (i = 0; i < ROWS; i++) {
        h = FldMenuBarBuild(h, ROWX(i) + 8, y + 0x10, 0x140, 0xC, 1);
        if (i >= 7) {
            if (i == fldMenuOptions.cursor) {
                TXT(8, port_rows[i - 7], ROWX(i) + 0x10, y, 0x16, PORT_SLOT + i - 7, 8);
            } else {
                TXT(8, port_rows[i - 7], ROWX(i) + 0x10, y, 0x14, PORT_SLOT + i - 7, 0);
            }
        } else if (i == fldMenuOptions.cursor) {
            FldMenuWinTextAdd(8, 1, sysGrxsysPac->msg->tbl[4], ROWX(i) + 0x10, y, 0x16, 0x16, n++, i + 1, 8);
        } else {
            FldMenuWinTextAdd(8, 1, sysGrxsysPac->msg->tbl[4], ROWX(i) + 0x10, y, 0x14, 0x14, n++, i + 1, 0);
        }
        y += 0x18;
    }

    y = 8;
    if (fldMenuOptions.vibration == 0) {
        FldMenuWinTextAdd(8, 1, sysGrxsysPac->msg->tbl[4], ROWX(0) + 0x104, y, 0x14, 0x14, n++, 0xF, 2);
    } else {
        FldMenuWinTextAdd(8, 1, sysGrxsysPac->msg->tbl[4], ROWX(0) + 0x104, y, 0x14, 0x14, n++, 0x10, 2);
    }
    y += 0x18;
    if (fldMenuOptions.camReverse == 0) {
        FldMenuWinTextAdd(8, 1, sysGrxsysPac->msg->tbl[4], ROWX(1) + 0x100, y, 0x14, 0x14, n++, 0x11, 2);
    } else {
        FldMenuWinTextAdd(8, 1, sysGrxsysPac->msg->tbl[4], ROWX(1) + 0x100, y, 0x14, 0x14, n++, 0x12, 2);
    }
    y += 0x18;
    if (fldMenuOptions.soundMode == 0) {
        FldMenuWinTextAdd(8, 1, sysGrxsysPac->msg->tbl[4], ROWX(2) + 0xF8, y, 0x14, 0x14, n++, 0xA, 2);
    } else {
        FldMenuWinTextAdd(8, 1, sysGrxsysPac->msg->tbl[4], ROWX(2) + 0x100, y, 0x14, 0x14, n++, 0xB, 2);
    }
    y += 0x18;
    if (fldMenuOptions.ipStop == 0) {
        FldMenuWinTextAdd(8, 1, sysGrxsysPac->msg->tbl[4], ROWX(3) + 0x100, y, 0x14, 0x14, n++, 0xC, 2);
    } else {
        FldMenuWinTextAdd(8, 1, sysGrxsysPac->msg->tbl[4], ROWX(3) + 0x100, y, 0x14, 0x14, n++, 0xE, 2);
    }
    y += 0x18;
    if (fldMenuOptions.miniMapOff == 0) {
        FldMenuWinTextAdd(8, 1, sysGrxsysPac->msg->tbl[4], ROWX(4) + 0x104, y, 0x14, 0x14, n++, 0xF, 2);
    } else {
        FldMenuWinTextAdd(8, 1, sysGrxsysPac->msg->tbl[4], ROWX(4) + 0x104, y, 0x14, 0x14, n++, 0x10, 2);
    }
    y += 0x18;
    if (fldMenuOptions.miniMap2A == 0) {
        FldMenuWinTextAdd(8, 1, sysGrxsysPac->msg->tbl[4], ROWX(5) + 0x104, y, 0x14, 0x14, n++, 0xF, 2);
    } else {
        FldMenuWinTextAdd(8, 1, sysGrxsysPac->msg->tbl[4], ROWX(5) + 0x104, y, 0x14, 0x14, n++, 0x10, 2);
    }
    y += 0x18;
    if (fldMenuOptions.miniMap2B == 0) {
        FldMenuWinTextAdd(8, 1, sysGrxsysPac->msg->tbl[4], ROWX(6) + 0xF0, y, 0x14, 0x14, n++, 0x13, 2);
    } else {
        FldMenuWinTextAdd(8, 1, sysGrxsysPac->msg->tbl[4], ROWX(6) + 0xD8, y, 0x14, 0x14, n++, 0x14, 2);
    }

    switch (fldMenuOptions.state) {
    case 0:
        FldMenuCursorSetAt(8, (f32)((7 - (fldMenuOptions.cursor < 7 ? fldMenuOptions.cursor : 6)) * 6) + 16.0f, (f32)(fldMenuOptions.cursor * 0x18) + 20.0f);
        trig = padPort0[0].trigger;
        if (trig & 0x20) {
            SndSePlay(0x7D2, 0, 0, 0x1E, 0);
            if (fldMenuOptions.cursor == 9) {
                DisplayOpen();
                fldMenuOptions.state = 4;
                break;
            }
            if (fldMenuOptions.cursor >= 7) {
                ListOpen(fldMenuOptions.cursor == 7 ? GRX_IN_PAD : GRX_IN_KEY);
                fldMenuOptions.state = 2;
                break;
            }
            FldMenuWinOpen(9, 0x1E0, fldMenuOptions.cursor * 0x1C + 0xC4, 0x80, 0x40, 0, 0, 0x8008);
            FldMenuWinSetActive(9);
            fldMenuOptions.state = 1;
            switch (fldMenuOptions.cursor) {
            case 0: fldMenuOptions.value = fldMenuOptions.vibration; break;
            case 1: fldMenuOptions.value = fldMenuOptions.camReverse; break;
            case 2: fldMenuOptions.value = fldMenuOptions.soundMode; break;
            case 3: fldMenuOptions.value = fldMenuOptions.ipStop; break;
            case 4: fldMenuOptions.value = fldMenuOptions.miniMapOff; break;
            case 5: fldMenuOptions.value = fldMenuOptions.miniMap2A; break;
            case 6: fldMenuOptions.value = fldMenuOptions.miniMap2B; break;
            }
        } else if ((k = CursorStep(fldMenuOptions.cursor, ROWS)) != fldMenuOptions.cursor) {
            /* (the game's moves on presses only) */
            fldMenuOptions.cursor = k;
            SndSePlay(0x7D1, 0, 0, 0x1E, 0);
        } else if (trig & 0x40) {
            SndSePlay(0x7D3, 0, 0, 0x1E, 0);
            sysSaveData.optVibration = fldMenuOptions.vibration;
            sysSaveData.optCamReverse = fldMenuOptions.camReverse;
            sysSaveData.optSoundMode = fldMenuOptions.soundMode;
            sysSaveData.optIpStop = fldMenuOptions.ipStop;
            sysSaveData.optMiniMapOff = fldMenuOptions.miniMapOff;
            sysSaveData.optMiniMap2A = fldMenuOptions.miniMap2A;
            sysSaveData.optMiniMap2B = fldMenuOptions.miniMap2B;
            if (FldMenuIsDirectOpen() == 1) {
                FldMenuClose();
            } else {
                if (sysSaveData.optVibration == 0) {
                    PadSetVibEnabled(1);
                } else {
                    PadSetVibEnabled(0);
                }
                return 1;
            }
        }
        break;
    case 1:
        FldMenuCursorSetAt(9, 8.0f, (f32)(fldMenuOptions.value * 0x18) + 16.0f);
        n += 2;
        switch (fldMenuOptions.cursor) {
        case 1:
            FldMenuWinTextAdd(9, 1, sysGrxsysPac->msg->tbl[4], 0x20, 4, 0x18, 0x18, n++, 0x11, 2);
            FldMenuWinTextAdd(9, 1, sysGrxsysPac->msg->tbl[4], 0x20, 0x20, 0x18, 0x18, n++, 0x12, 2);
            break;
        case 2:
            FldMenuWinTextAdd(9, 1, sysGrxsysPac->msg->tbl[4], 0x18, 4, 0x18, 0x18, n++, 0xA, 2);
            FldMenuWinTextAdd(9, 1, sysGrxsysPac->msg->tbl[4], 0x18, 0x20, 0x18, 0x18, n++, 0xB, 2);
            break;
        case 3:
            FldMenuWinTextAdd(9, 1, sysGrxsysPac->msg->tbl[4], 0x20, 4, 0x18, 0x18, n++, 0xC, 2);
            FldMenuWinTextAdd(9, 1, sysGrxsysPac->msg->tbl[4], 0x20, 0x20, 0x18, 0x18, n++, 0xE, 2);
            break;
        case 0:
        case 4:
        case 5:
            FldMenuWinTextAdd(9, 1, sysGrxsysPac->msg->tbl[4], 0x28, 4, 0x18, 0x18, n++, 0xF, 2);
            FldMenuWinTextAdd(9, 1, sysGrxsysPac->msg->tbl[4], 0x28, 0x20, 0x18, 0x18, n++, 0x10, 2);
            break;
        case 6:
            FldMenuWinTextAdd(9, 1, sysGrxsysPac->msg->tbl[4], 8, 4, 0x14, 0x18, n++, 0x13, 2);
            FldMenuWinTextAdd(9, 1, sysGrxsysPac->msg->tbl[4], 8, 0x20, 0x14, 0x18, n++, 0x14, 2);
            break;
        }
        if (padPort0[0].trigger & 0x40) {
            FldMenuWinClose(9, 0, 3);
            FldMenuWinSetActive(8);
            SndSePlay(0x7D3, 0, 0, 0x1E, 0);
            fldMenuOptions.state = 0;
        } else if (padPort0[0].trigger & 0x4000) {
            if (fldMenuOptions.value == 0) {
                fldMenuOptions.value = 1;
                SndSePlay(0x7D1, 0, 0, 0x1E, 0);
            }
        } else if (padPort0[0].trigger & 0x1000) {
            if (fldMenuOptions.value != 0) {
                fldMenuOptions.value = 0;
                SndSePlay(0x7D1, 0, 0, 0x1E, 0);
                if (fldMenuOptions.cursor == 0) {
                    PadSetVibEnabled(1);
                    PadVibrate(padPort0, 1, 0x80, 4);
                }
            }
        } else if (padPort0[0].trigger & 0x20) {
            FldMenuWinClose(9, 0, 3);
            FldMenuWinSetActive(8);
            SndSePlay(0x7D2, 0, 0, 0x1E, 0);
            fldMenuOptions.state = 0;
            switch (fldMenuOptions.cursor) {
            case 0: fldMenuOptions.vibration = fldMenuOptions.value; break;
            case 1: fldMenuOptions.camReverse = fldMenuOptions.value; break;
            case 2: fldMenuOptions.soundMode = fldMenuOptions.value; break;
            case 3: fldMenuOptions.ipStop = fldMenuOptions.value; break;
            case 4: fldMenuOptions.miniMapOff = fldMenuOptions.value; break;
            case 5: fldMenuOptions.miniMap2A = fldMenuOptions.value; break;
            case 6: fldMenuOptions.miniMap2B = fldMenuOptions.value; break;
            }
            if (fldMenuOptions.cursor == 2) {
                SndSetOutputMode(fldMenuOptions.soundMode);
            }
        }
        break;
    }
    return 3;
}
