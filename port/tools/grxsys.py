#!/usr/bin/env python3
"""Patches to SYS/GRXSYS.PAC, the game's system data (items, skills, shops,
text), for the mods in mods.py (port/src/grxsys_patch.c reads them).

  grxsys.py ISO [--out mods] [NAME ...]    builds mods/grxsys.grxp

The patch isn't a repacked file: it is the byte ranges of the unpacked file
that the mods change, and the rows and text they add after its end, so
nothing of the game's goes in the repo and no compressor is needed. Table
offsets are those of the unpacked file's header (the game turns them into
pointers after the patch is applied); text pointers in a table are relative
to that table's start."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mdt  # noqa: E402

SKILL_TABLE = 0xA4   # header word: the skill table (0x97 x 0x2C)
SKILL_SIZE = 0x2C
SKILL_AMOUNT = 0x0C  # s16, per level
SKILL_HELPDESC = 0x28  # text pointer, relative to the skill table

PIRATES_RING = 26
THIEF_TRICK = 62

# The game's glyph codes: a byte, then 0xF9
LETTERS = {c: 0x60 + i for i, c in enumerate("ABCDEFGHIJKLMNOPQRSTUVWXYZ")}
LETTERS.update({c: 0x80 + i for i, c in enumerate("abcdefghijklmnopqrstuvwxyz")})
LETTERS.update({" ": 0x9F, "'": 0x46, "%": 0x44, "-": 0x4C})
LETTERS.update({str(n): 0x4F + n for n in range(10)})


def text(s, end=b"\x00"):
    """A string in glyph codes; `end` follows it (a line is 0x4D 0xF9 0x00 in the descriptions)"""
    return b"".join(bytes((LETTERS[c], 0xF9)) for c in s) + end


LINE = b"\x4d\xf9\x00"


class Pack:
    """The unpacked file, edited in place; what changes becomes a patch"""

    def __init__(self, data):
        self.orig = bytes(data)
        self.data = bytearray(data)

    def word(self, off):
        return struct.unpack_from("<I", self.data, off)[0]

    def s16(self, off):
        return struct.unpack_from("<h", self.data, off)[0]

    def put16(self, off, v):
        struct.pack_into("<h", self.data, off, v)

    def append(self, blob, align=4):
        while len(self.data) % align:
            self.data.append(0)
        off = len(self.data)
        self.data += blob
        return off

    def skill(self, n):
        return self.word(SKILL_TABLE) + n * SKILL_SIZE

    def set_skill_text(self, n, lines):
        """The skill's description (its 4 lines) as new text after the file's end"""
        table = self.word(SKILL_TABLE)
        blob = b"".join(text(s, LINE if i else b"\x00") for i, s in enumerate(lines)) + b"\0\0"
        struct.pack_into("<I", self.data, self.skill(n) + SKILL_HELPDESC, self.append(blob) - table)

    def patch(self):
        """The GRXS file: unpacked size, extra bytes, ops (offset, data)"""
        old, new = self.orig, self.data
        ops, i = [], 0
        while i < len(old):
            if old[i] == new[i]:
                i += 1
                continue
            j = i
            while j < len(old) and (old[j] != new[j] or any(old[k] != new[k] for k in range(j, min(j + 16, len(old))))):
                j += 1
            ops.append((i, bytes(new[i:j])))
            i = j
        if len(new) > len(old):
            ops.append((len(old), bytes(new[len(old):])))
        out = bytearray(b"GRXS" + struct.pack("<III", len(old), len(new) - len(old), len(ops)))
        for off, blob in ops:
            out += struct.pack("<II", off, len(blob)) + blob
            out += b"\0" * (-len(blob) % 4)
        return bytes(out), ops


# ---------------------------------------------------------------- the mods


def pirate_ring_100(p):
    """Pirate's Ring and Thief Trick: +20% a level, so 100% at level 5. Pirate's
    Ring's bonus is the item's chance (the drop check is `random + bonus > the
    enemy's rate`, so 100 never misses); Thief Trick's is the item's tier (added
    to the enemy's rank lookup in percent, so 100 takes the top row)."""
    for n, effect in ((PIRATES_RING, 0x75), (THIEF_TRICK, 0x76)):
        s = p.skill(n)
        assert p.data[s + 8] == effect and p.s16(s + SKILL_AMOUNT) == 2 and p.data[s + 3] == 5, "skill %d isn't as read" % n
        p.put16(s + SKILL_AMOUNT, 20)
    p.set_skill_text(PIRATES_RING, ["Enemies drop ", "more items", "Only if enemy's defeated", "Level X 20%"])
    p.set_skill_text(THIEF_TRICK, ["Enemies drop ", "better items", "Only if enemy's defeated", "Level X 20%"])


MODS = {"guaranteed-drops": pirate_ring_100}


def build(iso, names, out):
    """mods/grxsys.grxp for the named mods (removed when there are none)"""
    path = os.path.join(out, "grxsys.grxp")
    names = [n for n in names if n in MODS]
    if not names:
        if os.path.exists(path):
            os.remove(path)
        return
    p = Pack(mdt.unpack(iso.file("SYS/GRXSYS.PAC")))
    for n in names:
        MODS[n](p)
    blob, ops = p.patch()
    with open(path, "wb") as f:
        f.write(blob)
    print("grxsys.grxp: %s (%d changed ranges, %d bytes added)" % (", ".join(names), len(ops), len(p.data) - len(p.orig)))


if __name__ == "__main__":
    import argparse
    import mods
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("iso")
    ap.add_argument("names", nargs="*", default=list(MODS))
    ap.add_argument("--out", default="mods")
    a = ap.parse_args()
    build(mods.Iso(a.iso), a.names, a.out)
