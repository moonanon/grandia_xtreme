/* PCSX2-compatible texture hashes (port/src/gs/gs_texhash.c): the names of
   a PCSX2 replacement pack's files are <texture hash>-<CLUT hash>-<bits>. */
#ifndef GS_TEXHASH_H
#define GS_TEXHASH_H

#include <stdint.h>
#include "gs_draw.h"

/* The three parts of a replacement's name for the texture a draw samples,
   read from local memory (so the memory must be current). Returns 0 when
   the texture isn't of a kind that PCSX2 hashes block by block
   (PSMCT32, PSMT8, PSMT4, at least one block in size): those are the
   only ones the port looks up. */
int gs_texhash(const GsTex *t, uint64_t *tex, uint64_t *clut, uint32_t *bits);

/* GRX_TEXHASH_LOG=file: appends the name of each texture the GPU renderer
   decodes (once per name) as <texture>-<CLUT>-<bits> in PCSX2's format,
   to check them against a pack's file names */
void gs_texhash_log(const GsTex *t);

#endif
