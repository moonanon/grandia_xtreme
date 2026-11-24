"""The debug kit, only in build.py --debug builds: super mode (F5, port/src/spots.c's
grx_super): the party can't die (NO_DEATH, party only - the game's flag
spares enemies too), magic and moves cost nothing (NO_MPSP), and the
battle command menu's debug kill-all (hold L2, press L1) works without
the game's debug mode (which crashes the port).

Map reveal (Shift+F7 or GRX_MAP_REVEAL=1, grx_map_reveal): the minimap
counts every cell as explored, without writing the save's explored bits,
so turning it off gives the normal map back."""

DEBUG_ONLY = True

PROTO = r'\1int grx_super(void);\n#line 2\n'
HEAD = r'(#include "common\.h"\n)'
REVEAL_PROTO = r'\1int grx_map_reveal(void);\nint grx_map_reveal_changed(void);\n#line 2\n'

FIXUPS = {
    # MapMiniExploredGet: everything explored while revealing
    "src/field/5FFC30.c": [
        (HEAD, REVEAL_PROTO),
        (r"(s32 MapMiniExploredGet\(s32 arg0, u16 arg1\) \{\n)", r"\1    if (grx_map_reveal()) {\n        return 1;\n    }\n"),
    ],
    # MapMiniDraw: on a toggle, the minimap's picture again from the
    # explored cells (MapMiniLoad does it once per map)
    "src/field/601138.c": [
        (HEAD, REVEAL_PROTO),
        (r"(    if \(MapMiniIsHidden\(1\) != 0\) \{\n        return;\n    \}\n)",
         r"\1    if (grx_map_reveal_changed() && mapMiniWork->hdr != NULL) {\n        MapMiniTexRevealExplored();\n    }\n"),
    ],
    # BtlUnitApplyDamage: a killing hit refills instead
    "src/battle/6D90A8.c": [
        (HEAD, PROTO),
        (r"if \(btlDebugCfg\.flags & 2\) \{", r"if ((btlDebugCfg.flags & 2) || (grx_super() && u->id < 4)) {"),
    ],
    # Each turn: MP/SP refilled
    "src/battle/6BB170.c": [
        (HEAD, PROTO),
        (r"if \(btlDebugCfg\.flags & BTL_DBG_NO_MPSP\) \{", r"if ((btlDebugCfg.flags & BTL_DBG_NO_MPSP) || grx_super()) {"),
    ],
    # BtlCmdMenuInput: L1 with L2 held kills every enemy
    "src/battle/69E008.c": [
        (HEAD, PROTO),
        (r"if \(SysIsDebugMode\(\) == 0\) \{\n(\s+)return;\n(\s+)\}\n(\s+)if \(pad->held & 1\) \{",
         r"if (SysIsDebugMode() == 0 && !(grx_super() && (pad->held & 1))) {\n\1return;\n\2}\n\3if (pad->held & 1) {"),
    ],
}
