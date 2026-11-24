"""Widescreen (port/docs/widescreen.md, port/src/widescreen.c).

At an aspect wider than 4:3 the game keeps its 640-pixel screen space and
the GS front end squeezes the frame into the 640-pixel frame buffer
(port/src/gs/gs_widescreen.c). What has to change in the game is that it
draws the scene past x = 0 and x = 640: its culling.

All the 3D culling (GphMeshClipTest, GphSphereClipTest, the clip codes of
GphVertXformClip, the CPU clipper's edges, the model and particle culls)
reads the per-view clip block that VuViewClipSetup builds from the view's
area, magnification and projection distance. For a full-width view
(x = 0, w = 640) the block is built as if the view were wider by
grx_ws_view_extra() pixels on each side, with the centre where it was: the
side planes and the clip scale (magH / w) widen, and the projection (the
centre offset x + 2048 + centerX, magH, projDist) is unchanged. The
view's own area, its scissor and what GphViewGetArea returns (the
frame-buffer effects use it) stay 640 wide.

The 2D sprite routines drop a sprite whose corners are all left of 0 or
right of 640; markers anchored to 3D positions (UtilWorldToScreen) go
through them, so their bounds widen too.

The HUD stays at the screen's edges, moved out by grx_ws_hud_side()
pixels (up to the --hud aspect): in the field the mini-map (view 6, bottom
left) and the threat gauge (bottom right, port/game/field/6020A8.c); in
battle the unit info window (bottom left, while giving commands), the IP
dial (bottom right) and the hit counts of chains (left, on enemies; right,
on the party). The party's HP/SP panels and the tactics under them stay
where they are at 4:3, in a row across the middle.
The mini-map's view, and so its scissor, starts left of x = 0 then; the
scissor's x fields hold it as an 11-bit two's complement, which the GS
front end reads that way when it squeezes.

The side follows the window. Positions worked out when drawn follow it
at once; those the game keeps (the unit info window, the hit
counts) are moved each frame by the side's change since they were placed
(grx_ws_hud_moved: 1 the unit info window, 2 and 3 the enemy and party
hit counts; 0 is unused).
"""

_DECL = ("const float *grx_ws_view(const float *c, float *tmp); int grx_ws_view_extra(void); "
         "int grx_ws_hud_side(void); int grx_ws_hud_moved(int i); ")



def _decl(next_line):
    """The declarations after `#include "common.h"`, line numbers kept"""
    return (r'(#include "common\.h"\n)', r"\1" + _DECL + "\n#line %d\n" % next_line)


# x < 0 and x > 640 tests in the sprite routines (common.h on line 1)
_SPR_CULL = [
    _decl(2),
    (r"(\.x) < 0\b", r"\1 < -grx_ws_view_extra()"),
    (r"(\.x) > 640\b", r"\1 > 640 + grx_ws_view_extra()"),
]

FIXUPS = {
    # VuViewClipSetup(Out *o, In1 *a, In2 *b, In3 *c, f32 z): c is the view's
    # area (x, y, w, h, centerX, centerY, magH, magV, projDist, ...)
    "src/main/45E0A8.c": [
        _decl(3),
        (r"(    u64 k;)\n", r"\1 In3 grx_wv; c = (In3 *)grx_ws_view((const float *)c, (float *)&grx_wv);\n"),
    ],
    # SprDrawRect, SprDrawRectFast, SprDrawColorQuad
    "src/main/40F0B0.c": _SPR_CULL,
    "src/main/40F768.c": _SPR_CULL,
    "src/main/40F9E8.c": _SPR_CULL,
    # GsDrawEnvPacketSet: a negative x stays in its field
    "src/main/456720.c": [
        (r"p->scissor = \(u64\)x0 \+ \(\(u64\)x1 << 16\)",
         r"p->scissor = (u64)(x0 & 0x7FF) + ((u64)(x1 & 0x7FF) << 16)"),
    ],
    # FldDraw: the mini-map's view (GphViewSetup(6, 32, 244, 180, 160, ...)
    # at the field's start) at the left edge, each frame (the aspect follows
    # the window); the whole picture while the map overlay is on
    # (fixups/minimap.py)
    "src/field/5B06A0.c": [
        (r"\n(        FldSetDrawContext\(arg0, 1, 0\);\n        MapMiniDraw\(\);)",
         r"\n        { void GphViewSetArea(s32, s32, s32, s32, s32); int grx_ws_hud_side(void); int grx_map_overlay(void);"
         r" if (grx_map_overlay()) GphViewSetArea(6, 0, 0, 640, 448);"
         r" else GphViewSetArea(6, 0x20 - grx_ws_hud_side(), 0xF4, 0xB4, 0xA0); }\1"),
    ],
    # BtlUnitInfoUpdate: the unit info window's place when it opens, and
    # while open
    "src/battle/695180.c": [
        _decl(3),
        (r"o->x = 24\.0f;", r"o->x = 24.0f - grx_ws_hud_side();"),
        (r"\n(    ent = btlUnitInfo\.unit;\n)", r"\n    p->frame.x -= grx_ws_hud_moved(1); \1"),
    ],
    # BtlHudComboPopupUpdate(list): the hit counts on screen
    "src/battle/6D1768.c": [
        _decl(3),
        (r"\n(    for \(i = 0; i < 12; i\+\+, p\+\+\) \{\n)",
         r"\n    { s32 d = grx_ws_hud_moved(p == btlHudEnemyComboPopups ? 2 : 3); if (d != 0)"
         r" for (q = p, i = 0; i < 12; i++, q++) { f32 s = q->unit >= 4 ? -d : d;"
         r" q->x += s; q->destX += s; q->startX += s; } } \1"),
    ],
    # BtlHudComboPopupAdd: the hit count of a chain slides to its row at
    # x 44 (on an enemy, at the left edge) or ending at x 596 (on the party,
    # at the right edge)
    "src/battle/6D3C68.c": [
        _decl(3),
        (r"p->destX = 44\.0f;", r"p->destX = 44.0f - grx_ws_hud_side();"),
        (r"p->destX = 596\.0f - ", r"p->destX = 596.0f + grx_ws_hud_side() - "),
    ],
    # SysMain: the screen capture the menus draw behind them, taken at the
    # aspect of its time; widescreen.c resamples it when the aspect changes
    # (the Display page's Aspect ratio, with a menu open)
    "src/main/4000C0.c": [
        (r"(GphFbStoreImage\(0, 0, 0x280, 0x1C0, sysState\.screenCaptureAddr\);)",
         r"\1 { void grx_ws_capture_taken(unsigned addr); grx_ws_capture_taken((u32)sysState.screenCaptureAddr); }"),
    ],
    # IpGaugeDraw: the IP dial's centre, x 542 + ipDialX
    "src/battle/6A7CF8.c": [
        _decl(3),
        (r"542\.0f \+ ipDialX\b", r"542.0f + ipDialX + grx_ws_hud_side()"),
        (r"ipDialX \+ 542\.0f", r"ipDialX + 542.0f + grx_ws_hud_side()"),
    ],
}
