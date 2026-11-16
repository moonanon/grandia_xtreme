/* Texture replacements for the GPU renderer: a folder of PCSX2-style
   replacement files (port/src/gs_gl/gsgl_replace.c). Off unless GRX_TEXTURES
   names folders or ./textures has some. */
#ifndef GSGL_REPLACE_H
#define GSGL_REPLACE_H

#include "grx_gl.h"
#include "../gs/gs_draw.h"

/* Index the folders (recursively, earlier ones win) listed in GRX_TEXTURES,
   else each folder in ./textures in name order; once, from the GL
   thread, after the GL functions are loaded. Returns the number of files. */
int gsgl_rep_init(void);

/* The replacement's GL texture (DXT, with its mip levels, loaded on first
   use) for the texture a draw samples, or 0: not enabled, not in the
   folder, or it didn't load. Local memory must be current. */
GLuint gsgl_rep_lookup(const GsTex *t);

#endif
