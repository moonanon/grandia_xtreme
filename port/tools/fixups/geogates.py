"""Map patches and extra Geo Gate destinations (port/src/mdt_patch.c,
port/tools/mods.py).

MapLoadUpdate decompresses a map into a field-heap buffer of its exact size:
the buffer gets the patch's extra bytes, and the patch is applied right after.
The town gate's list (FldFlagListOpen, FldFlagListSetPage) takes the extra
destinations after the game's 0x26; FldFlagListUpdate's value lookup is in the
port's own port/game/field/657F20.c.
"""

PROTOS = ("#include <stdint.h>\n"
          "uint32_t grx_mdt_extra(int file);\n"
          "void grx_mdt_patch(int file, unsigned char *data, uint32_t size);\n"
          "int grx_flaglist_count(void);\n"
          "int grx_flaglist_flag(int i);\n"
          "unsigned char *grx_flaglist_name(int i);\n")

GATE_TINT = "".join("c->col[%d].r = 0x30;\n c->col[%d].g = 0xC0;\n c->col[%d].b = 0xFF;\n" % (k, k, k) for k in range(4))

FIXUPS = {
    # MapMiniDraw: entrances on every map with the minimap-entrances mod (the
    # game draws them only on auto-built floors, AmapIsLoaded); a Geo Gate
    # (its arrival entrance, MapEntrance.id +0x28 = 0x2000) is the entrance
    # icon in the gates' cyan; Ocean Bed's (map 0x5010) is left out while
    # the map's scripts keep it sealed, until main flag 0x2EA4 (a scene on
    # that map); a gate mods.py appended past the 16 entrances the minimap
    # shows is still drawn
    "src/field/601138.c": [
        (r'(#include "common\.h"\n)', r'\1int grx_mod_enabled(const char *);\n#line 2\n'),
        (r"if \(AmapIsLoaded\(\) == 1\) \{(\n\s+for \(i = 0; i < 0x10; i\+\+\) \{\n\s+p = MapEntranceGet)",
         r'if (AmapIsLoaded() == 1 || grx_mod_enabled("minimap-entrances")) {\1'),
        (r"for \(i = 0; i < 0x10; i\+\+\) \{\n(\s+)p = MapEntranceGet\(i\);\n(\s+)if \(p == NULL\) break;\n",
         r"for (i = 0; i < 0x40; i++) {\n\1p = MapEntranceGet(i);\n\2if (p == NULL) break;\n"
         r"\2if (i >= 0x10 && *(u16 *)((u8 *)p + 0x28) != 0x2000) continue;\n"
         r"\2if (*(u16 *)((u8 *)p + 0x28) == 0x2000 && EvtGetMapNo() == 0x5010 && sysSaveData.mainFlag < 0x2EA4) continue;\n"
         r"\2if (c >= &mapMiniWork->icons[62]) break;\n"),
        (r"(\s+)(c->type = 0xD;\n\s+c->uv\[0\]\[0\] = 0x8E;)",
         r"\1if (*(u16 *)((u8 *)p + 0x28) == 0x2000) {\1" + GATE_TINT.replace("\n", r"\1") + r"}\1\2"),
    ],
    # MapLoadUpdate: the compressed map's buffer and decompression
    "src/field/5D2848.c": [
        (r'(#include "field/n17\.h"\n)', r'\1' + PROTOS.replace("\\", "\\\\") + '#line 9\n'),
        (r"( +)mapLoadState\.data = FldMemAlloc\(mapLoadState\.size\);\n"
         r"(\s+if \(mapLoadState\.which == 0\) \{\n"
         r"\s+FileDecompress\(mapLoadState\.largeBuf, mapLoadState\.data, 1\);\n"
         r"\s+\} else \{\n"
         r"\s+FileDecompress\(mapLoadState\.smallBuf, mapLoadState\.data, 1\);\n"
         r"\s+\}\n)",
         r"\1mapLoadState.data = FldMemAlloc(mapLoadState.size + grx_mdt_extra(mapNo.file));\n"
         r"\2\1grx_mdt_patch(mapNo.file, (unsigned char *)mapLoadState.data, mapLoadState.size);\n"),
    ],
    # FldFlagListOpen: the extra destinations after the game's
    "src/field/657C70.c": [
        (r'(#include "field/n25\.h"\n)', r'\1' + PROTOS + '#line 4\n'),
        (r"for \(i = 0; i < 0x26; i\+\+\) \{\n(\s+)if \(EvtGlobalFlagGet\(fldFlagListEntries\[i\]\.id\) == 1\) \{",
         r"for (i = 0; i < 0x26 + grx_flaglist_count(); i++) {\n"
         r"\1if (EvtGlobalFlagGet(i < 0x26 ? fldFlagListEntries[i].id : grx_flaglist_flag(i - 0x26)) == 1) {"),
    ],
    # FldFlagListSetPage: their names
    "src/field/657E60.c": [
        (r'(#include "field/n25\.h"\n)', r'\1' + PROTOS + '#line 5\n'),
        (r"(\s+)TxtGlyphQueueMsg\(0, sysGrxsysPac->msg->tbl\[1\], i \* 2, fldFlagListWork->entries\[base \+ i\]\);",
         r"\1if (fldFlagListWork->entries[base + i] >= 0x26) {"
         r"\1    TxtGlyphQueueStr(0, grx_flaglist_name(fldFlagListWork->entries[base + i] - 0x26), i * 2);"
         r"\1} else {"
         r"\1    TxtGlyphQueueMsg(0, sysGrxsysPac->msg->tbl[1], i * 2, fldFlagListWork->entries[base + i]);"
         r"\1}"),
    ],
}
