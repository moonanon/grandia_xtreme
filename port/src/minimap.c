/* The minimap overlay (grx_minimap.h, port/tools/fixups/minimap.py): the
   field's minimap spread flat over the whole picture. Toggled by its port
   action (M, R3; grx_input.h), with the map-overlay mod (mods/mods.txt). */
#include <SDL3/SDL.h>
#include "grx_minimap.h"
#include "grx_mdt.h"
#include "grx_platform.h"

static SDL_AtomicInt overlay;

int grx_map_overlay(void) {
    return SDL_GetAtomicInt(&overlay);
}

void grx_map_overlay_toggle(void) {
    int on;
    if (!grx_mod_enabled("map-overlay")) {
        grx_log("minimap: the overlay needs the map-overlay mod (port/tools/mods.py)");
        return;
    }
    on = !SDL_GetAtomicInt(&overlay);
    SDL_SetAtomicInt(&overlay, on);
    grx_log("minimap: overlay %s", on ? "on" : "off");
}
