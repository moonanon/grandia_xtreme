#!/usr/bin/env python3
"""Field map files (FIELD/xxxx.MDT): unpack, inspect, disassemble scripts.

  mdt.py unpack FILE.MDT [OUT]        LZSS-decoded map data (FileDecompress m:42EA20)
  mdt.py info FILE                    header sections, entrances
  mdt.py areas FILE                   event areas (EvtAreaLoad f:5B3A18, 0x48 bytes)
  mdt.py occ FILE [KIND]              OCC records (OccLoad f:5B5C38, 0x80 bytes)
  mdt.py occs FILE [GROUP|AREA]       OCCS sequences (OccsLoad f:5B66D0, 0x94 bytes)
  mdt.py scripts FILE [ID]            scenario scripts, disassembled (EvtScnrExec f:662BB8)

FILE is an .MDT (unpacked on the fly) or already unpacked data. Offsets are
into the unpacked data. Reads game data only from the paths given."""
import struct
import sys

# ---------------------------------------------------------------- unpacking


def unpack(raw):
    key, = struct.unpack_from("<I", raw, 0)
    magic, size, clen = (struct.unpack_from("<I", raw, 4 * i)[0] ^ key for i in (1, 2, 3))
    if magic != 0x15246468:
        raise ValueError("not a compressed MDT (magic %#x)" % magic)
    src = raw[16:16 + clen]
    text = bytearray(0x800)
    r, i, flags, n = 0x7DE, 0, 0, len(src)
    out = bytearray()
    while True:
        flags >>= 1
        if not flags & 0x100:
            if i == n:
                break
            flags = src[i] | 0xFF00
            i += 1
        if flags & 1:
            if i == n:
                break
            c = src[i]
            i += 1
            out.append(c)
            text[r] = c
            r = (r + 1) & 0x7FF
        else:
            if i + 1 >= n:
                break
            a, b = src[i], src[i + 1]
            i += 2
            a |= (b & 0xE0) << 3
            for k in range((b & 0x1F) + 3):
                c = text[(a + k) & 0x7FF]
                out.append(c)
                text[r] = c
                r = (r + 1) & 0x7FF
    return bytes(out)


def load(path):
    raw = open(path, "rb").read()
    if len(raw) >= 8 and struct.unpack_from("<I", raw, 4)[0] ^ struct.unpack_from("<I", raw, 0)[0] == 0x15246468:
        return unpack(raw)
    return raw


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


NONE = 0xFFFFFFFF

# Header: count field -> data offset field, and what reads it (MapDataLoad f:5D1828)
SECTIONS = {
    0x0C: ("entrances", 0x08, 0x20),
    0x1C: ("models", 0x18, None),
    0x24: ("event areas", 0x20, 0x48),
    0x2C: ("OCC", 0x28, 0x80),
    0x34: ("OCCS", 0x30, None),
    0x3C: ("tenant areas", 0x38, None),
    0x44: ("tenant tracks", 0x40, None),
    0x4C: ("tenants", 0x48, None),
    0x54: ("sound emitters", 0x50, None),
    0x74: ("point lights", 0x70, None),
    0x7C: ("scripts", None, None),
    0x9C: ("treasure", 0x98, None),
    0xA4: ("camera scripts", None, None),
}


def section(d, ofs_field):
    o = u32(d, ofs_field)
    return None if o == NONE else o


# ---------------------------------------------------------------- scripts


def script_block(d):
    """(index list of (id, start), body offset) of the map's script block"""
    blk = section(d, 0x7C)
    if blk is None:
        return [], None
    body = blk + u32(d, blk)
    ids = []
    p = blk + 4
    while p + 4 <= body:
        sid, val = struct.unpack_from("<HH", d, p)
        p += 4
        if val == 0xFFFF:
            break
        ids.append((sid, body + val * 8))
    return ids, body


def op_len(d, p):
    """Length of the op at p and a short text for it (EvtScnrExec's decoding)"""
    c = d[p]
    a = d[p + 1] if p + 1 < len(d) else 0
    s16 = lambda o: struct.unpack_from("<H", d, p + o)[0]
    if c in (0, 1, 2):
        return 1, "end"
    if c == 3:
        sub, v = a >> 4, s16(2)
        tgt = ((a & 0xF) << 16) + v
        return 4, {0: "return", 1: "jump-if-false body+%#x" % tgt, 2: "jump body+%#x" % tgt,
                   3: "call body+%#x" % tgt, 4: "wait-flag %s%#x" % ("set " if v >> 13 else "clear ", v & 0x1FFF),
                   5: "rand %d -> result" % v}.get(sub, "op3/%d" % sub)
    if c == 4:
        return 2, "id-base %#x" % (a << 8)
    if c == 5:
        return 3, ["occ-wake", "occs-wake", "occ-run-wait", "occs-run-wait"][a] + " +%#x" % d[p + 2] if a < 4 else "op5/%d" % a
    if c == 6:
        return 4, "tenant-point %#x %d" % (s16(1), d[p + 3])
    if c == 7:
        n = 6 if a == 11 else 4
        return 2 + n, "syscmd 7/%d %s" % (a, d[p + 2:p + 2 + n].hex(" "))
    if c in (8, 9, 10):
        mode, v = a, s16(2)
        op, idx = v >> 13, v & 0x1FFF
        size = c - 8
        if mode == 0:
            n = (1, 2, 4)[size]
            val = int.from_bytes(d[p + 4:p + 4 + n], "little")
            src = "%#x" % val
        else:
            n = 2
            src = "var[%#x]" % s16(4)
        name = ["set", "add", "==", "!=", ">", ">=", "<", "<="][op]
        return 4 + n, "var%s[%#x] %s %s" % ("bhw"[size], idx, name, src)
    if c == 11:
        v = s16(1)
        return 3, "flag %#x %s" % (v & 0x1FFF, ["clear", "set", "test-clear", "test-set", "get"][min(v >> 13, 4)])
    if c == 12:
        return 2, "cmd12 %d" % a
    if c == 13:
        n = {0: 8, 2: 1, 3: 1}.get(a, 0)
        return 2 + n, "cmd13/%d %s" % (a, d[p + 2:p + 2 + n].hex(" "))
    if c == 14:
        n = 2 if a <= 5 else 3 if a == 6 else 0
        return 2 + n, "cmd14/%d %s" % (a, d[p + 2:p + 2 + n].hex(" "))
    if c == 15:
        return 4, "cmd15 %s" % d[p + 1:p + 4].hex(" ")
    if c == 16:
        return 8, "map-jump/%d occ+%#x map %#x entrance %#x" % (a, s16(2), s16(4), s16(6))
    if c == 17:
        return 4, "item%d %#x" % (a, s16(2))
    if c == 18:
        return 2, "op18 %d" % a
    if c == 19:
        return 6, "money%d %d" % (a, struct.unpack_from("<i", d, p + 2)[0])
    if c == 20:
        return 6, "cmd20 %s" % d[p + 1:p + 6].hex(" ")
    if c == 21:
        return 5, "cmd21 %s" % d[p + 1:p + 5].hex(" ")
    if c == 24:
        return 3, "picture %d %d" % (a, d[p + 2])
    if c == 29:
        return (3, "wait %d" % d[p + 2]) if a == 0xFF else (4, "wait-stream %d %d" % (a, s16(2)))
    # 23 and the rest: a message, up to its 0x17 0x00 (EvtScnrMsgStart f:663C90)
    q = p + 1
    while q + 1 < len(d) and not (d[q] == 0x17 and d[q + 1] == 0):
        q += 1
    kind = "choice" if c == 23 and a == 0x40 else "msg"
    return q + 2 - p, "%s (%d bytes)" % (kind, q + 2 - p)


def disasm(d, start, body, limit=4000):
    """Ops from start until an end (following no jumps), as (offset, len, text)"""
    p, out = start, []
    while p < len(d) and len(out) < limit:
        n, t = op_len(d, p)
        out.append((p, n, t))
        p += n
        if t == "end":
            break
    return out


# ---------------------------------------------------------------- commands


def section_end(d, start):
    """The next header section's start after `start`, else the data's end"""
    offs = [u32(d, f) for f in range(0x0C, 0x188, 4)]
    return min([o for o in offs if o != NONE and start < o <= len(d)] + [len(d)])


def cmd_info(d):
    print("size %#x" % len(d))
    for f, (name, cnt, sz) in sorted(SECTIONS.items()):
        o = section(d, f)
        if o is not None:
            print("  +%#05x %-15s @%#x%s" % (f, name, o, " count %d" % u32(d, cnt) if cnt is not None else ""))
    e = section(d, 0x0C)
    for i in range(u32(d, 0x08) if e is not None else 0):
        r = d[e + i * 0x20:e + i * 0x20 + 0x20]
        print("  entrance %d id %#06x pos %s heading %.0f" % (
            i, struct.unpack_from("<H", r, 0)[0], tuple(round(v, 1) for v in struct.unpack_from("<3f", r, 4)),
            struct.unpack_from("<f", r, 0x10)[0]))


def cmd_areas(d):
    a = section(d, 0x24)
    for i in range(u32(d, 0x20) if a is not None else 0):
        r = d[a + i * 0x48:a + i * 0x48 + 0x48]
        p0 = struct.unpack_from("<3f", r, 0x10)
        print("%3d type %d target %d fire %d mask %d flags %#x link %d/%d attr %d  p0 %s r %.0f h %.0f" % (
            i, r[0], r[1], r[2], r[3], r[8], r[0xD], struct.unpack_from("<h", r, 0xE)[0], r[0xC],
            tuple(round(v) for v in p0), *struct.unpack_from("<2f", r, 0x40)))


def cmd_occ(d, kind=None):
    o = section(d, 0x2C)
    for i in range(u32(d, 0x28) if o is not None else 0):
        r = d[o + i * 0x80:o + i * 0x80 + 0x80]
        if kind is not None and r[0x18] != kind:
            continue
        print("%3d id %#06x kind %2d  %s | %s" % (i, struct.unpack_from("<H", r, 4)[0], r[0x18],
                                                 r[0x17:0x30].hex(" "), r[0x30:0x40].hex(" ")))


def cmd_occs(d, want=None):
    """OCCS sequences (OccsLoad f:5B66D0, 0x94 bytes): id, trigger area, flags,
    condition, then up to 32 (op, arg) steps (OccsUpdate f:5B5E68)"""
    o = section(d, 0x34)
    for i in range(u32(d, 0x30) if o is not None else 0):
        r = d[o + i * 0x94:o + i * 0x94 + 0x94]
        sid, area, flags = struct.unpack_from("<3H", r, 0)
        if want is not None and (sid >> 8) != want and area != want:
            continue
        cond = struct.unpack_from("<7H", r, 6)
        ops = struct.unpack_from("<32H", r, 0x14)
        args = struct.unpack_from("<32H", r, 0x54)
        steps = " ".join("%x:%x" % (op, a) for op, a in zip(ops, args) if op or a)
        print("%3d id %#06x area %d flags %#x cond %s  %s" % (i, sid, area, flags,
                                                           " ".join("%x" % c for c in cond), steps))


def cmd_scripts(d, want=None):
    ids, body = script_block(d)
    for sid, start in ids:
        if want is not None and sid != want:
            continue
        print("script %#06x @%#x (body+%#x)" % (sid, start, start - body))
        if want is not None:
            # Linear sweep to the next script's start (bodies are contiguous;
            # jumps land inside the same stretch)
            starts = sorted(st for _, st in ids)
            end = min([st for st in starts if st > start] + [section_end(d, body)])
            p = start
            while p < end:
                n, t = op_len(d, p)
                print("  body+%06x  %-24s %s" % (p - body, d[p:p + min(n, 8)].hex(" "), t))
                p += n


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    cmd, path = argv[0], argv[1]
    if cmd == "unpack":
        out = argv[2] if len(argv) > 2 else path + ".bin"
        open(out, "wb").write(load(path))
        print(out)
        return 0
    d = load(path)
    arg = int(argv[2], 0) if len(argv) > 2 else None
    {"info": lambda: cmd_info(d), "areas": lambda: cmd_areas(d), "occ": lambda: cmd_occ(d, arg),
     "scripts": lambda: cmd_scripts(d, arg), "occs": lambda: cmd_occs(d, arg)}[cmd]()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
