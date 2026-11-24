/* Map patches and extra Geo Gate destinations (port/src/mdt_patch.c) */
#ifndef GRX_MDT_H
#define GRX_MDT_H

#include <stdint.h>

/* Bytes map file `file`'s patch adds (0 without one), for the buffer it
   decompresses into */
uint32_t grx_mdt_extra(int file);

/* Applies it to the decompressed map of `size` bytes */
void grx_mdt_patch(int file, unsigned char *data, uint32_t size);

/* Whether mods/mods.txt names this mod (port/tools/mods.py) */
int grx_mod_enabled(const char *name);

/* Extra destinations of the town gate's list, after the game's 0x26 */
int grx_flaglist_count(void);
int grx_flaglist_flag(int i);
int grx_flaglist_value(int i);
unsigned char *grx_flaglist_name(int i);
uint16_t *grx_flaglist_glyphs(int i, int *len);

#endif
