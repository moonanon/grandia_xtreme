"""The minimap overlay (port/src/minimap.c), as Final Fantasy XII's map
overlay: the minimap spread over the whole field, flat and without its
panel, while the game goes on and the player walks.

With the overlay on, the minimap's view (6) is the whole picture (the
widescreen sides too) both where the map model is projected and where
it is drawn, MapMiniDraw leaves out the dark quad it puts under the map,
and MapMiniUpdate looks straight down from far enough to show the whole
map (the projection's scale is in pixels, GphViewSetFov). The minimap's
zoom button still picks near or far."""

FIXUPS = {
    # MapMiniDraw: no dark quad under the overlay
    "src/field/601138.c": [
        (r'(#include "common\.h"\n)', r'\1int grx_map_overlay(void);\n#line 2\n'),
        (r"(\n)(    GphBatchDrawQuads\(&mapMiniWork->quads\[0\]\);)",
         r"\1    if (!grx_map_overlay())\n    \2"),
    ],
    # FldBuildDrawLists: the map model is projected through view 6, so the
    # overlay's whole-picture view is set before it, as well as before
    # MapMiniDraw (fixups/widescreen.py)
    "src/field/5B1200.c": [
        (r"\n(    FldSetDrawContext\(arg0, 1, 0\);\n    MapMiniModelDraw\(\);)",
         r"\n    { void GphViewSetArea(s32, s32, s32, s32, s32); int grx_ws_hud_side(void); int grx_map_overlay(void);"
         r" if (grx_map_overlay()) GphViewSetArea(6, 0, 0, 640, 448);"
         r" else GphViewSetArea(6, 0x20 - grx_ws_hud_side(), 0xF4, 0xB4, 0xA0); }\n\1"),
    ],
    # MapMiniUpdate: straight down while the overlay is on
    "src/field/600670.c": [
        (r'(#include "common\.h"\n)', r'\1int grx_map_overlay(void);\n#line 2\n'),
        (r"(    if \(sysSaveData\.optMiniMap2B == 0\) \{\n"
         r"        mapMiniWork->zoomTo\[0\] = 0\.6981317f;\n"
         r"    \} else \{\n"
         r"        mapMiniWork->zoomTo\[0\] = 1\.5707964f;\n"
         r"    \}\n)",
         r"\1    if (grx_map_overlay()) {\n        mapMiniWork->zoomTo[0] = 1.5707964f;\n"
         r"        mapMiniWork->zoomTo[1] = sysSaveData.optMiniMapZoom == 0 ? 100.0f : 140.0f;\n"
         r"        mapMiniWork->zoomTo[2] = sysSaveData.optMiniMapZoom == 0 ? 1.5f : 2.0f;\n    }\n"),
    ],
}
