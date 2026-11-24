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

ITEM_TABLE = 0x84    # header word: items (0x800 x 0x20), their equip records and text
SORT_KEYS = 0x88     # header word: u16 per item
SHOP_TABLE = 0xA0    # header word: 3 shop lists of 150 x 10 bytes (id, 3 required items, scenario progress)
SHOP_LIST = 1500
EFFECT_DROP_RATE = 0x75
CHAR = dict(evan=0x01, brandol=0x02, carmyne=0x04, jaid=0x08, titto=0x10, ulk=0x20, myamm=0x40, lutina=0x80)

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


def help_text(kind, desc):
    """An item's help: its kind, a break, the description ("Weapon", "Item")"""
    return text(kind, b"") + b"\x4e\xf9" + text(desc, b"") + b"\x4d\xf9\x00"


class Pack:
    """The unpacked file, edited in place; what changes becomes a patch"""

    def __init__(self, data):
        self.orig = bytes(data)
        self.data = bytearray(data)
        self.taken = set()

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

    # --- items

    def item(self, n):
        return self.word(ITEM_TABLE) + n * 32

    def item_name(self, n):
        o, out = self.word(ITEM_TABLE) + struct.unpack_from("<I", self.data, self.item(n) + 0xC)[0], ""
        inv = {v: k for k, v in LETTERS.items()}
        while self.data[o]:
            out += inv.get(self.data[o], "?")
            o += 2
        return out

    def equip(self, n):
        e = struct.unpack_from("<I", self.data, self.item(n) + 0x18)[0]
        return self.word(ITEM_TABLE) + e if e else None

    def attack(self, n):
        return self.s16(self.equip(n) + 2)

    def free_id(self, lo, hi):
        for n in range(lo, hi + 1):
            if self.data[self.item(n)] == 0 and n not in self.taken:
                self.taken.add(n)
                return n
        raise SystemExit("no free item id in %d-%d" % (lo, hi))

    def add_item(self, like, lo, hi, name, kind, desc, price, mask, attack, bonus, help_desc_from):
        """A new equipment item: a copy of item `like`'s row (icon, model, rank)
        with its own name, help, equip record (attack, character mask, drop
        rate bonus) and the drop-rate help of item `help_desc_from`"""
        table = self.word(ITEM_TABLE)
        n = self.free_id(lo, hi)
        row = bytearray(self.data[self.item(like):self.item(like) + 32])
        struct.pack_into("<i", row, 4, price)
        struct.pack_into("<I", row, 0xC, self.append(text(name)) - table)
        struct.pack_into("<I", row, 0x10, self.append(help_text(kind, desc)) - table)
        struct.pack_into("<I", row, 0x14, struct.unpack_from("<I", self.data, self.item(help_desc_from) + 0x14)[0])
        rec = bytearray(0x2C)
        struct.pack_into("<H", rec, 0, mask)
        struct.pack_into("<h", rec, 2, attack)
        rec[0x15] = EFFECT_DROP_RATE
        struct.pack_into("<h", rec, 0x16, bonus)
        struct.pack_into("<I", row, 0x18, self.append(bytes(rec)) - table)
        struct.pack_into("<I", row, 0x1C, 0)
        self.data[self.item(n):self.item(n) + 32] = row
        keys = self.word(SORT_KEYS)
        struct.pack_into("<H", self.data, keys + 2 * n, struct.unpack_from("<H", self.data, keys + 2 * like)[0])
        return n

    def shop_row(self, group, slot):
        return self.word(SHOP_TABLE) + group * SHOP_LIST + slot * 10

    def set_shop(self, group, slot, item, reqs, progress):
        struct.pack_into("<4hH", self.data, self.shop_row(group, slot), item, *reqs, progress)

    def find_shop(self, group, item):
        for i in range(150):
            r = struct.unpack_from("<4hH", self.data, self.shop_row(group, i))
            if r[0] == item:
                return i, r[1:4], r[4]
        raise SystemExit("item %d isn't in shop list %d" % (item, group))

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



THIEF_SWORD = 840           # Thief's Sword (Brandol's, +10), widened to Evan too
EVAN_BRANDOL = CHAR["evan"] | CHAR["brandol"]
EVERYONE_BUT_TITTO = 0xFF & ~CHAR["titto"]
SHOP_WEAPONS, SHOP_ACCESSORIES = 0, 2
ACCESSORY_SLOTS = 80        # slots 80-95 of the accessory list: the shop that reads 16-31 reads these too

# The new weapons: name, the shop weapon each is cloned from (its price, its
# requirements and when it is sold), the bonus, the character mask, the id range
# (a character's weapons are 100 ids from 701 + 100 * character) and the cut
# in attack from the weapon it is cloned from, which is the price of the bonus
WEAPONS = [
    ("Brigand's Sword", 736, "Schiavona", +15, 0.12, EVAN_BRANDOL, (701, 800), "Bandit's keen blade"),
    ("Corsair's Sword", 733, "Scramasax", +25, 0.17, EVAN_BRANDOL, (701, 800), "Pirate captain's saber"),
    ("Brigand's Sword", 809, "Giant Broadsword", +15, 0.12, EVAN_BRANDOL, (801, 900), "Bandit's keen blade"),
    ("Corsair's Sword", 830, "Jet-Black Sword", +25, 0.17, EVAN_BRANDOL, (801, 900), "Pirate captain's saber"),
    ("Mercury Knife", 1405, "Double Moon", +15, 0.12, CHAR["lutina"], (1401, 1500), "Swift as the thief god"),
    ("Cacus Dagger", 1403, "Orihalcon Dagger", +25, 0.17, CHAR["lutina"], (1401, 1500), "Cunning fire thief's blade"),
]

# Rings (accessories): name, bonus, price, scenario progress when sold, character mask, text
RINGS = [
    ("Magpie Ring", +5, 1200, 0, EVERYONE_BUT_TITTO, "Loves shiny things"),
    ("Plunder Ring", +10, 6000, 13100, EVERYONE_BUT_TITTO, "Takes what it can"),
    ("Fortuna Ring", +15, 12000, 14750, EVERYONE_BUT_TITTO, "Luck favors thieves"),
    ("Mogay Ring", +15, 2000, 11300, CHAR["titto"], "Rabbits' thieving charm"),
    ("Mogay Ring DX", +30, 8000, 13100, CHAR["titto"], "Rabbits' finest swag"),
    ("Mogay Ring Premium", +45, 16000, 14750, CHAR["titto"], "Rabbit king's loot"),
]
RING_LIKE = 1938            # Heal Ring: the ring icon and rank


def thief_gear(p):
    """Drop-rate gear for the thieves, so Titto isn't the only one (his total
    is 75 with the skill at level 5, +20 Mogay Suit DX and a Mogay Ring; the
    others reach 50): swords and daggers for Evan, Brandol and Lutina, rings
    for everyone, sold in the shops at the late tiers"""
    names = {p.item_name(n) for n in range(0x800) if p.data[p.item(n)]}
    p.data[p.equip(THIEF_SWORD)] |= EVAN_BRANDOL
    assert p.s16(p.equip(THIEF_SWORD) + 0x16) == 10 and p.data[p.equip(THIEF_SWORD) + 0x15] == EFFECT_DROP_RATE
    slot = 128
    for name, ref, ref_name, bonus, cut, mask, (lo, hi), desc in WEAPONS:
        assert p.item_name(ref) == ref_name, "item %d isn't %s" % (ref, ref_name)
        assert name not in names or name == "Brigand's Sword" or name == "Corsair's Sword", name
        at, reqs, progress = p.find_shop(SHOP_WEAPONS, ref)
        price = struct.unpack_from("<i", p.data, p.item(ref) + 4)[0]
        n = p.add_item(ref, lo, hi, name, "Weapon", desc, price, mask, round(p.attack(ref) * (1 - cut)), bonus, THIEF_SWORD)
        p.set_shop(SHOP_WEAPONS, slot, n, reqs, progress)
        slot += 1
    slot = ACCESSORY_SLOTS
    for name, bonus, price, progress, mask, desc in RINGS:
        assert name not in names, name
        n = p.add_item(RING_LIKE, 2001, 2047, name, "Item", desc, price, mask, 0, bonus, THIEF_SWORD)
        p.set_shop(SHOP_ACCESSORIES, slot, n, (0, 0, 0), progress)
        slot += 1

MODS = {"guaranteed-drops": pirate_ring_100, "thief-gear": thief_gear}


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
