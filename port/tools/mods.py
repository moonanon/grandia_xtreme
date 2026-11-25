#!/usr/bin/env python3
"""The port's mods (README.md, "Included mods"): builds them into mods/,
which the port reads as it runs.

  mods.py ISO [--out mods] [--without NAME ...] [--list]

Reads the maps from your own disc image (nothing of the game's is in the
repo) and writes:
  mods/mods.txt       the mods built, one name a line (the port reads it)
  mods/mdt/XXXX.mdp   patches for each Geo Gate's map and for the town (0x2000)
  mods/flaglist.txt   the town gate's extra destinations (flag, value, name)

--without leaves a mod out (repeat it for more); --list lists them.

Geo Gates (GATES):

A gate is a copy of map 0x5020's (the template): its arrival entrance (id
0x2000), its two event areas, its OCC group 0x3D (glow particles, the
scripts' OCCs, sounds, the jump to town), its OCCS 0x3D01-05, the effect
pack (header +0x180), and scripts 0x3001 (activate: sets the gate's global
flag), 0x3002 (use: jump to town), plus hooks on 0x1000 (the glow by the
flag) and 0x1001 (the arrival effect). The town gets a jump OCC and its
OCCS, and script 0x3002 a branch for the new list value."""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mdt  # noqa: E402
import grxsys  # noqa: E402

# map, position (where the player stands at the gate), facing, name in the list,
# global flags (free: 0x3D8, 0x3F4, 0x3FC): `flag` activated (the list shows
# it), `open_flag` opened (the first touch's "I'll open the gate!"), list value (>= 0x26).
# The name is the map's own location name (script 0xE000), as the game's list entries are.
GATES = [
    dict(mod="land-ruins-2-geo-gate", desc="a new Geo Gate on Land Ruins 2, which has none",
         map=0x4040, pos=(-25.0, 700.0, -4300.0), facing=0.0, name="Land Ruins 2", flag=0x3F0, open_flag=0x3F1,
         value=0x26),
    dict(mod="gale-ruins-3-geo-gate", desc="a new Geo Gate deeper in Gale Ruins (map 0x5830)",
         map=0x5830, pos=(-3741.7, 700.0, -10802.9), facing=83.0, name="Gale Ruins 3", flag=0x3EF, open_flag=0x3F3,
         value=0x29),
    # An exit-only gate the game already has (exit_gate_patch): `flag` is the
    # one its own activation sets, `exit_group` its OCC/OCCS group; the exit
    # gates share one first-touch flag, so each gets an `open_flag` of its own
    dict(mod="land-ruins-3-geo-gate", desc="Land Ruins 3's exit-only Geo Gate made two-way",
         map=0x4050, pos=(-2.0, 405.0, 2.0), facing=0.0, name="Land Ruins 3", flag=0x3C8,
         open_flag=0x3ED, value=0x27, exit=True, exit_group=0x3C),
    dict(mod="aqua-ruins-geo-gate", desc="Aqua Ruins' Energy Conversion exit-only Geo Gate made two-way",
         map=0x50F0, pos=(-692.0, 220.0, 7082.0), facing=0.0, name="Aqua Ruins - Energy Conversion",
         flag=0x3CA, open_flag=0x3EE, value=0x28, exit=True, exit_group=0x3C),
]

# Mods with no files of their own, which the port's code applies when
# mods/mods.txt names them (grx_mod_enabled)
CODE_MODS = [
    dict(mod="map-overlay",
         desc="M (or R3) spreads the minimap flat over the whole field, as Final Fantasy XII's map overlay"),
    dict(mod="minimap-entrances",
         desc="the minimap shows every map's entrances and Geo Gates (the game shows them on the "
              "auto-built corridor floors only)"),
]

# Mods that patch the system data (grxsys.py): one file, mods/grxsys.grxp
GRXSYS_MODS = [
    dict(mod="thief-gear",
         desc="thief gear so Titto isn't the only thief: weapons and rings for a better item drop rate, "
              "armor and rings for a better chance of large gold payouts"),
    dict(mod="guaranteed-drops",
         desc="Pirate's Ring and Thief Trick give +20% a level: at level 5 enemies always drop an item, "
              "and from the best items they have"),
    # after guaranteed-drops: its level line reads the skill's amount
    dict(mod="pirates-ring-text",
         desc="Pirate's Ring's description says it drops more items, as it does, not more gold"),
]

TEMPLATE_MAP = 0x5020
TOWN_MAPS = (0x2000, 0xE400)   # the town, and its file once system flag 0x88 is set (MapJumpRequest)
VARIANTS = 4                   # map files n..n+3: one map's story variants (MapHasVariant), same layout
GROUP = 0x3D                 # OCC/OCCS id group of a gate (free in the target maps: checked)
SCRIPT_FLAG_BASE = 0xAC8     # script flag n = global flag n - 0xAC8 (variable area = save +0x1E38)
ARRIVED_FLAG = 0xEBA         # global 0x3F2: "came through a gate", shared by every gate
TOWN_JUMP_TEMPLATE = 0x3D22  # town OCC: the jump to 0x5020's gate
TOWN_OCCS_TEMPLATE = 0x3D0A  # town OCCS for list value 3 (0x5020)

# ---------------------------------------------------------------- disc


class Iso:
    """Just enough ISO 9660 to read a file by path"""

    def __init__(self, path):
        self.f = open(path, "rb")
        pvd = self.read(16 * 2048, 2048)
        if pvd[1:6] != b"CD001":
            raise SystemExit("%s: not an ISO 9660 image" % path)
        self.root = pvd[156:156 + 34]

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def _entries(self, rec):
        lba, size = struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
        data, p = self.read(lba * 2048, size), 0
        while p < len(data):
            n = data[p]
            if n == 0:
                p = (p // 2048 + 1) * 2048
                continue
            r = data[p:p + n]
            name = r[33:33 + r[32]].decode("ascii", "replace").split(";")[0]
            yield name, r
            p += n

    def exists(self, path):
        try:
            self.file(path)
            return True
        except SystemExit:
            return False

    def file(self, path):
        rec = self.root
        for part in path.split("/"):
            for name, r in self._entries(rec):
                if name.upper() == part.upper():
                    rec = r
                    break
            else:
                raise SystemExit("not on the disc: " + path)
        lba, size = struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
        return self.read(lba * 2048, size)


def load_map(iso, n):
    return mdt.unpack(iso.file("FIELD/%04X.MDT" % n))


# ---------------------------------------------------------------- records

u32 = mdt.u32


def recs(d, ofs_field, cnt_field, size):
    o, n = u32(d, ofs_field), u32(d, cnt_field)
    return [bytearray(d[o + i * size:o + (i + 1) * size]) for i in range(n)] if o != mdt.NONE else []


def put_f(r, o, *v):
    struct.pack_into("<%df" % len(v), r, o, *v)


def get_f(r, o, n=3):
    return list(struct.unpack_from("<%df" % n, r, o))


def put_h(r, o, v):
    struct.pack_into("<H", r, o, v & 0xFFFF)


def get_h(r, o):
    return struct.unpack_from("<H", r, o)[0]


# ---------------------------------------------------------------- scripts


class Asm:
    """Scenario code (EvtScnrExec's encoding, port/tools/mdt.py), body-relative"""

    def __init__(self, base):
        self.base, self.b, self.fix = base, bytearray(), []

    def here(self):
        return self.base + len(self.b)

    def raw(self, *bs):
        self.b += bytes(bs)

    def id_base(self, g):
        self.raw(4, g)

    def flag(self, f, op):  # 0 clear, 1 set, 2 test-clear, 3 test-set
        self.raw(0x0B, *struct.pack("<H", (op << 13) | f))

    def occs_wake(self, n, wait=False):
        self.raw(5, 3 if wait else 1, n)

    def occ_wake(self, n):
        self.raw(5, 0, n)

    def varw_eq(self, idx, v):
        self.raw(0x0A, 0, *struct.pack("<H", (2 << 13) | idx), *struct.pack("<I", v))

    def jump(self, sub, label_or_ofs):  # 1 if false, 2 always, 3 call
        self.fix.append((len(self.b), sub, label_or_ofs))
        self.raw(3, 0, 0, 0)

    def end(self):
        self.raw(2)

    def resolve(self, labels):
        for at, sub, t in self.fix:
            ofs = labels[t] if isinstance(t, str) else t
            self.b[at + 1] = (sub << 4) | (ofs >> 16)
            struct.pack_into("<H", self.b, at + 2, ofs & 0xFFFF)
        return bytes(self.b)


def script_block_parts(d):
    blk = u32(d, 0x7C)
    size = u32(d, blk)
    ids, body = mdt.script_block(d)
    body_bytes = d[body:mdt.section_end(d, body)]
    index = [(sid, (st - body) // 8) for sid, st in ids]
    return index, bytearray(body_bytes)


def build_block(index, body):
    """size-prefixed index {id, offset/8} ... {0, 0xFFFF}, then the body"""
    idx = b"".join(struct.pack("<HH", i, v) for i, v in index) + struct.pack("<HH", 0, 0xFFFF)
    size = 4 + len(idx)
    size = (size + 15) & ~15
    return struct.pack("<I", size) + idx.ljust(size - 4, b"\0") + bytes(body)


def add_code(body, emit):
    """Appends 8-aligned code built by emit(asm) -> labels; returns its offset/8"""
    while len(body) % 8:
        body.append(0)
    a = Asm(len(body))
    labels = emit(a)
    body += a.resolve(labels)
    return a.base // 8


# ---------------------------------------------------------------- patches


class Patch:
    """mods/mdt/XXXX.mdp: 'GRXP', count, then {type, field, size, data}:
    type 0 sets the header word at `field`, type 1 appends data (16-aligned)
    and points the header word at it (port/src/mdt_patch.c)"""

    def __init__(self):
        self.ops = []

    def set(self, field, value):
        self.ops.append((0, field, struct.pack("<I", value)))

    def section(self, field, data):
        self.ops.append((1, field, bytes(data)))

    def write(self, path):
        out = bytearray(b"GRXP" + struct.pack("<I", len(self.ops)))
        for t, f, data in self.ops:
            out += struct.pack("<III", t, f, len(data)) + data
            while len(out) % 4:
                out.append(0)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, "wb").write(out)


def open_msg(tpl):
    """The template's first touch: "Maybe I can use this Geo Gate here. OK,
    I'll open the gate!" - a message routine its 0x3001 calls (a message and
    a return, no jumps), to copy as it is"""
    t_ids, t_body = mdt.script_block(tpl)
    at = dict(t_ids)[0x3001]
    msg = None
    while msg is None:
        n, t = mdt.op_len(tpl, at)
        if t.startswith("call body+"):
            msg = t_body + int(t.split("+")[1], 16)
        at += n
    q = msg
    while True:
        n, t = mdt.op_len(tpl, q)
        q += n
        if t == "return":
            break
    return tpl[msg:q]


def gate_patch(tpl, d, g):
    p = Patch()
    sflag = g["flag"] + SCRIPT_FLAG_BASE

    # The template gate: entrance 0x2000, its areas (those its OCCs fire from)
    t_ents = recs(tpl, 0x0C, 0x08, 0x20)
    t_ent = next(e for e in t_ents if get_h(e, 0) == 0x2000)
    t_occ = recs(tpl, 0x2C, 0x28, 0x80)
    t_occs = recs(tpl, 0x34, 0x30, 0x94)
    occ_idx = [i for i, r in enumerate(t_occ) if r[5] == GROUP]
    occs_idx = [i for i, r in enumerate(t_occs) if r[1] == GROUP]
    t_areas = recs(tpl, 0x24, 0x20, 0x48)
    area_idx = sorted({get_h(t_occ[i], 6) for i in occ_idx if get_h(t_occ[i], 6) != 0xFFFF})

    # Where it goes: the player's spot, the gate's centre at the template's offset
    ex, ey, ez = get_f(t_ent, 4)
    cx, cy, cz = get_f(t_areas[area_idx[0]], 0x10)
    nx, ny, nz = g["pos"]
    dx, dy, dz = nx - ex, ny - ey, nz - ez

    ents = recs(d, 0x0C, 0x08, 0x20)
    if any(get_h(e, 0) == 0x2000 for e in ents):
        raise SystemExit("map %#x already has a gate arrival (entrance 0x2000)" % g["map"])
    e = bytearray(t_ent)
    put_f(e, 4, nx, ny, nz)
    put_f(e, 0x10, g["facing"], g["facing"])
    ents.append(e)

    areas = recs(d, 0x24, 0x20, 0x48)
    area_map = {}
    for i in area_idx:
        r = bytearray(t_areas[i])
        x, y, z = get_f(r, 0x10)
        put_f(r, 0x10, x + dx, y + dy, z + dz)
        area_map[i] = len(areas)
        areas.append(r)

    occ = recs(d, 0x2C, 0x28, 0x80)
    if any(r[5] == GROUP for r in occ):
        raise SystemExit("map %#x already uses OCC group %#x" % (g["map"], GROUP))
    occ_map = {i: len(occ) + k for k, i in enumerate(occ_idx)}
    for i in occ_idx:
        r = bytearray(t_occ[i])
        a = get_h(r, 6)
        if a != 0xFFFF:
            put_h(r, 6, area_map[a])
        kind = r[0x18]
        if kind == 48:     # particle emitter at a position
            x, y, z = get_f(r, 0x30)
            put_f(r, 0x30, x + dx, y + dy, z + dz)
        elif kind == 0:    # script, turning the player toward a point (z stored negated)
            x, y, z = get_f(r, 0x24)
            put_f(r, 0x24, x + dx, y + dy, z - dz)
        elif kind == 5:    # switch on event areas
            a = get_h(r, 0x24)
            if a in area_map:
                put_h(r, 0x24, area_map[a])
        occ.append(r)

    occs = recs(d, 0x34, 0x30, 0x94)
    if any(r[1] == GROUP for r in occs):
        raise SystemExit("map %#x already uses OCCS group %#x" % (g["map"], GROUP))
    for i in occs_idx:
        r = bytearray(t_occs[i])
        for k in range(32):
            op, arg = get_h(r, 0x14 + 2 * k), get_h(r, 0x54 + 2 * k)
            if op == 0x101:
                put_h(r, 0x54 + 2 * k, occ_map[arg])
            elif op == 0x102:
                raise SystemExit("template OCCS %d wakes an OCCS: not handled" % i)
        occs.append(r)

    # Scripts: activate, use, and hooks ahead of the map's own 0x1000/0x1001
    index, body = script_block_parts(d)
    have = dict(index)
    for sid in (0x3001, 0x3002):
        if sid in have:
            raise SystemExit("map %#x already has script %#x" % (g["map"], sid))

    msg_code = open_msg(tpl)
    oflag = g["open_flag"] + SCRIPT_FLAG_BASE

    def opened_msg(a):
        a.b += msg_code
        return {}

    msg_ofs = add_code(body, opened_msg) * 8

    def activate(a):
        # 1st touch: the message; 2nd: activate (the 3rd uses it, 0x3002)
        a.flag(oflag, 2)
        a.jump(1, "act")
        a.flag(oflag, 1)
        a.jump(3, msg_ofs)
        a.end()
        L = {"act": a.here()}
        a.id_base(GROUP)
        a.flag(sflag, 1)
        a.occs_wake(1)
        a.end()
        return L

    def use(a):
        a.id_base(GROUP)
        a.flag(ARRIVED_FLAG, 1)
        a.occs_wake(4)
        a.end()
        return {}

    def on_load(a):
        L = {}
        a.id_base(GROUP)
        a.flag(sflag, 2)
        a.jump(1, "l1")
        a.occs_wake(2)            # dormant glow, the activate area on
        L["l1"] = a.here()
        a.flag(sflag, 3)
        a.jump(1, "l2")
        a.occs_wake(3)            # active glow, the use area on
        L["l2"] = a.here()
        if 0x1000 in have:
            a.jump(2, have[0x1000] * 8)
        else:
            a.end()
        return L

    def on_arrive(a):
        L = {}
        a.flag(ARRIVED_FLAG, 3)
        a.jump(1, "out")
        a.id_base(GROUP)
        a.occs_wake(5)
        a.occs_wake(5, wait=True)
        a.flag(ARRIVED_FLAG, 0)
        L["out"] = a.here()
        if 0x1001 in have:
            a.jump(2, have[0x1001] * 8)
        else:
            a.end()
        return L

    new = {0x3001: add_code(body, activate), 0x3002: add_code(body, use),
           0x1000: add_code(body, on_load), 0x1001: add_code(body, on_arrive)}
    index = [(sid, new.pop(sid, v)) for sid, v in index] + sorted(new.items())

    p.set(0x08, len(ents))
    p.section(0x0C, b"".join(ents))
    p.set(0x20, len(areas))
    p.section(0x24, b"".join(areas))
    p.set(0x28, len(occ))
    p.section(0x2C, b"".join(occ))
    p.set(0x30, len(occs))
    p.section(0x34, b"".join(occs))
    p.section(0x7C, build_block(index, body))
    if u32(d, 0x180) == mdt.NONE:
        o = u32(tpl, 0x180)
        p.section(0x180, tpl[o:mdt.section_end(tpl, o)])
    return p


def exit_gate_patch(tpl, d, g):
    """An existing exit-only gate made two-way. It already has a normal
    gate's scripts and OCC/OCCS layout under its own group (0x3C): touch
    once for a message, again to activate (its own flag, which the town
    list then uses), then use. Added: the arrival entrance; the arrival
    OCCs the template has and it lacks, and its empty arrival OCCS 5 (the
    white of the jump stays on screen without it) filled as the template's;
    the template's effect pack (the exit gates' own draws them in another
    colour); and the template's first-touch message ("I'll open the gate!")
    instead of the exit gate's ("only good for returning to Locca")"""
    p = Patch()
    t_ent = next(e for e in recs(tpl, 0x0C, 0x08, 0x20) if get_h(e, 0) == 0x2000)
    ents = recs(d, 0x0C, 0x08, 0x20)
    if any(get_h(e, 0) == 0x2000 for e in ents):
        raise SystemExit("map %#x already has a gate arrival (entrance 0x2000)" % g["map"])
    e = bytearray(t_ent)
    put_f(e, 4, *g["pos"])
    put_f(e, 0x10, g["facing"], g["facing"])
    ents.append(e)

    def ops_at(at):
        out = []
        while not out or out[-1] != "end":
            n, t = mdt.op_len(d, at)
            out.append(t)
            at += n
        return out

    # 0x3001: flag test-clear, jump-if-false (activate), flag set, call (message), end
    ids, b0 = mdt.script_block(d)
    ops = ops_at(dict(ids)[0x3001])
    if len(ops) != 5 or not ops[3].startswith("call body+"):
        raise SystemExit("map %#x: unexpected gate script 0x3001: %s" % (g["map"], ops))
    oflag = g["open_flag"] + SCRIPT_FLAG_BASE
    act = int(ops[1].split("+")[1], 16)
    aops = ops_at(b0 + act)    # id-base, flag set (activated), occs-wake 1, end
    if aops[1] != "flag %#x set" % (g["flag"] + SCRIPT_FLAG_BASE):
        raise SystemExit("map %#x: the gate activates with %r, not global flag %#x" % (g["map"], aops[1], g["flag"]))

    # Arrival: the template's OCCs by id (group byte swapped), OCCS 5 as the template's
    grp = g["exit_group"]
    occ = recs(d, 0x2C, 0x28, 0x80)
    t_occ = recs(tpl, 0x2C, 0x28, 0x80)
    by_id = {get_h(r, 4): i for i, r in enumerate(occ) if r[5] == grp}
    t_occs5 = next(r for r in recs(tpl, 0x34, 0x30, 0x94) if get_h(r, 0) == GROUP << 8 | 5)
    s5 = bytearray(t_occs5)
    put_h(s5, 0, grp << 8 | 5)
    for k in range(32):
        if get_h(s5, 0x14 + 2 * k) == 0x101:
            r = t_occ[get_h(s5, 0x54 + 2 * k)]
            oid = grp << 8 | (get_h(r, 4) & 0xFF)
            if oid not in by_id:
                if r[0x18] not in (16, 21):
                    raise SystemExit("template arrival OCC kind %d: not handled" % r[0x18])
                r = bytearray(r)
                put_h(r, 4, oid)
                by_id[oid] = len(occ)
                occ.append(r)
            put_h(s5, 0x54 + 2 * k, by_id[oid])
        elif get_h(s5, 0x14 + 2 * k) == 0x102:
            raise SystemExit("template OCCS 5 wakes an OCCS: not handled")
    occs = [s5 if get_h(r, 0) == grp << 8 | 5 else r for r in recs(d, 0x34, 0x30, 0x94)]

    index, body = script_block_parts(d)
    msg_code = open_msg(tpl)
    msg_ofs = add_code(body, lambda a: (a.raw(*msg_code), {})[1]) * 8

    def first_touch(a):
        a.flag(oflag, 2)
        a.jump(1, act)
        a.flag(oflag, 1)
        a.jump(3, msg_ofs)
        a.end()
        return {}

    new = add_code(body, first_touch)
    index = [(sid, new if sid == 0x3001 else v) for sid, v in index]
    p.set(0x08, len(ents))
    p.section(0x0C, b"".join(ents))
    p.set(0x28, len(occ))
    p.section(0x2C, b"".join(occ))
    p.section(0x34, b"".join(occs))
    p.section(0x7C, build_block(index, body))
    o = u32(tpl, 0x180)
    p.section(0x180, tpl[o:mdt.section_end(tpl, o)])
    return p


def town_patch(town, gates):
    p = Patch()
    occ = recs(town, 0x2C, 0x28, 0x80)
    occs = recs(town, 0x34, 0x30, 0x94)
    jt = next(r for r in occ if get_h(r, 4) == TOWN_JUMP_TEMPLATE)
    st = next(r for r in occs if get_h(r, 0) == TOWN_OCCS_TEMPLATE)
    used_occ = {get_h(r, 4) for r in occ}
    used_occs = {get_h(r, 0) for r in occs}
    index, body = script_block_parts(town)
    have = dict(index)
    branches = []
    for g in gates:
        oid = next(i for i in range(GROUP << 8 | 0x45, GROUP << 8 | 0xFF) if i not in used_occ)
        used_occ.add(oid)
        r = bytearray(jt)
        put_h(r, 4, oid)
        put_h(r, 0x20, g["map"])   # map; entrance 0x2000 kept
        occ.append(r)
        sid = next(i for i in range(GROUP << 8 | 0x2D, GROUP << 8 | 0xFF) if i not in used_occs)
        used_occs.add(sid)
        s = bytearray(st)
        put_h(s, 0, sid)
        for k in range(32):
            if get_h(s, 0x14 + 2 * k) == 0x101:
                put_h(s, 0x54 + 2 * k, len(occ) - 1)
        occs.append(s)
        branches.append((g["value"] + 1, sid & 0xFF))

    # Script 0x3002 (the list's event): the new values first, then the game's
    def dispatch(a):
        L = {}
        for k, (v, occs_n) in enumerate(branches):
            a.varw_eq(8, v)
            a.jump(1, "n%d" % k)
            a.flag(ARRIVED_FLAG, 1)
            a.id_base(GROUP)
            a.occ_wake(0x0E)
            a.occs_wake(occs_n)
            a.end()
            L["n%d" % k] = a.here()
        a.jump(2, have[0x3002] * 8)
        return L

    new = add_code(body, dispatch)
    index = [(sid, new if sid == 0x3002 else v) for sid, v in index]
    p.set(0x28, len(occ))
    p.section(0x2C, b"".join(occ))
    p.set(0x30, len(occs))
    p.section(0x34, b"".join(occs))
    p.section(0x7C, build_block(index, body))
    return p


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("iso", nargs="?")
    ap.add_argument("--out", default="mods")
    ap.add_argument("--without", action="append", default=[], metavar="NAME", help="leave this mod out")
    ap.add_argument("--list", action="store_true", help="list the mods and stop")
    a = ap.parse_args(argv)
    every = GATES + CODE_MODS + GRXSYS_MODS
    if a.list:
        for m in every:
            print("%-24s %s" % (m["mod"], m["desc"]))
        return 0
    if a.iso is None:
        ap.error("the disc image is needed")
    unknown = set(a.without) - {m["mod"] for m in every}
    if unknown:
        ap.error("no such mod: %s (--list lists them)" % ", ".join(sorted(unknown)))
    gates = [g for g in GATES if g["mod"] not in a.without]
    iso = Iso(a.iso)
    tpl = load_map(iso, TEMPLATE_MAP)

    # Files of mods left out (or of an earlier run) go first
    mdt_dir = os.path.join(a.out, "mdt")
    if os.path.isdir(mdt_dir):
        for n in os.listdir(mdt_dir):
            if n.endswith(".mdp"):
                os.remove(os.path.join(mdt_dir, n))
    for g in gates:
        files = [m for m in range(g["map"], g["map"] + VARIANTS) if iso.exists("FIELD/%04X.MDT" % m)]
        for m in files:
            (exit_gate_patch if g.get("exit") else gate_patch)(tpl, load_map(iso, m), g).write(
                os.path.join(mdt_dir, "%04X.mdp" % m))
        print("%-24s map %#06x (files %s), flag %#x, list value %#x" % (
            g["mod"], g["map"], " ".join("%04X" % m for m in files), g["flag"], g["value"]))
    if gates:
        for t in TOWN_MAPS:
            town_patch(load_map(iso, t), gates).write(os.path.join(mdt_dir, "%04X.mdp" % t))
    with open(os.path.join(a.out, "flaglist.txt"), "w") as f:
        f.write("# flag value name (port/src/mdt_patch.c: the town gate's extra destinations)\n")
        for g in gates:
            f.write("%#x %#x %s\n" % (g["flag"], g["value"], g["name"]))
    grxsys.build(iso, [m["mod"] for m in GRXSYS_MODS if m["mod"] not in a.without], a.out)
    with open(os.path.join(a.out, "mods.txt"), "w") as f:
        f.write("# The mods built (port/tools/mods.py), one a line\n")
        for m in every:
            if m["mod"] not in a.without:
                f.write(m["mod"] + "\n")
                if m in CODE_MODS:
                    print(m["mod"])
    print("wrote", a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
