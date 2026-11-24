"""The system data pack's patch (port/src/grxsys_patch.c, port/tools/grxsys.py).

SysDataInit decompresses SYS/GRXSYS.PAC into a buffer of its exact size: the
buffer gets the patch's extra bytes, and the patch is applied right after,
before the tables' offsets are turned into pointers.
"""

FIXUPS = {
    "src/main/41D1B8.c": [
        (r'(#include "common\.h"\n)', r'\1#include <stdint.h>\nuint32_t grx_grxsys_extra(void);\n'
                                      r'void grx_grxsys_patch(unsigned char *data, uint32_t size);\n#line 2\n'),
        (r"( +)hdr = MemAlloc32\(FileCmpGetSize\(buf\)\);\n( +)FileDecompress\(buf, hdr, 1\);\n",
         r"\1hdr = MemAlloc32(FileCmpGetSize(buf) + grx_grxsys_extra());\n"
         r"\2FileDecompress(buf, hdr, 1);\n"
         r"\2grx_grxsys_patch((unsigned char *)(uintptr_t)(u32)hdr, FileCmpGetSize(buf));\n"),
    ],
}
