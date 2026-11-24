/* Patches to SYS/GRXSYS.PAC (port/src/grxsys_patch.c, port/tools/grxsys.py) */
#ifndef GRX_GRXSYS_H
#define GRX_GRXSYS_H

#include <stdint.h>

/* Bytes mods/grxsys.grxp adds to the unpacked file (0 without one) */
uint32_t grx_grxsys_extra(void);

/* Applies the patch to the unpacked file of `size` bytes, in a buffer
   grx_grxsys_extra() bytes larger */
void grx_grxsys_patch(unsigned char *data, uint32_t size);

#endif
