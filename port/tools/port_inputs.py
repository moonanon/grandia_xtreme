#!/usr/bin/env python3
"""What the port build takes from the decompilation's own build, kept in
port/inputs/ so that the port builds without it (port/tools/build_all.py):

- c_syms*.ld: the names the matching build's linker defines (from
  tools/c_syms.py), copied as they are;
- asm_labels.json: the labels still in assembly (asm/, from
  tools/split.sh), {name: ["func", 0] or ["data", size]}, for the stubs;
- rodata.json: for each `__rodata_<binary>_<file>` marker, where that
  file's .rodata is in the game's own file on the disc (SLUS_204.17,
  FIELD.BIN, BATTLE.BIN, VIEWER.BIN), its size and the named objects in
  it. port/tools/syms.py reads the bytes from iso/ when it needs them, so
  nothing of the game's is kept here: only names, offsets and sizes.

    python port/tools/port_inputs.py           # write port/inputs/ from build/
    python port/tools/port_inputs.py --check   # does port/inputs/ match build/?

Run after the matching build (tools/docker.sh make) whenever the
decompilation's sources change. It needs build/ (the ELFs, c_syms*.ld and
the objects), asm/ and the game's files in iso/.
"""

import glob
import json
import os
import shutil
import struct
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.dirname(__file__))
import syms  # noqa: E402

INPUTS = os.path.join(ROOT, "port", "inputs")
# binary -> (the linked ELF in build/, the game's file in iso/)
BINARIES = {
    "main": ("SLUS_204.17.elf", "SLUS_204.17"),
    "field": ("field.elf", "FIELD.BIN"),
    "battle": ("battle.elf", "BATTLE.BIN"),
    "viewer": ("viewer.elf", "VIEWER.BIN"),
}


def elf_sections(elf):
    shoff, = struct.unpack_from("<I", elf, 0x20)
    shentsize, shnum, _ = struct.unpack_from("<HHH", elf, 0x2E)
    return [struct.unpack_from("<IIIIIIIIII", elf, shoff + i * shentsize) for i in range(shnum)]


def elf_symbols(elf):
    """{name: value} of an ELF's symbol table"""
    sec = elf_sections(elf)
    out = {}
    for s in sec:
        if s[1] != 2:  # SHT_SYMTAB
            continue
        strtab = sec[s[6]]
        for k in range(s[5] // 16):
            st_name, st_value = struct.unpack_from("<II", elf, s[4] + k * 16)
            if st_name:
                out.setdefault(elf[strtab[4] + st_name:].split(b"\0", 1)[0].decode(), st_value)
    return out


def elf_loads(elf):
    """[(vaddr, file offset, file size)] of the PT_LOAD segments"""
    phoff, = struct.unpack_from("<I", elf, 0x1C)
    phentsize, phnum = struct.unpack_from("<HH", elf, 0x2A)
    out = []
    for i in range(phnum):
        typ, off, vaddr, _, filesz = struct.unpack_from("<5I", elf, phoff + i * phentsize)
        if typ == 1 and filesz:
            out.append((vaddr, off, filesz))
    return out


def file_offset(binary, vma, built, original):
    """Where vma is in the game's own file: the main executable is an ELF
    (its segments say), an overlay a flat image of its ELF's first segment"""
    if binary == "main":
        for vaddr, off, size in elf_loads(original):
            if vaddr <= vma < vaddr + size:
                return off + vma - vaddr
        raise SystemExit(f"0x{vma:08X} isn't in SLUS_204.17's segments")
    return vma - min(v for v, _, _ in elf_loads(built))


def rodata_relocs(path):
    """Offsets of the words in an object's .rodata that the linker
    relocates (its .rel.rodata)"""
    with open(path, "rb") as f:
        elf = f.read()
    sec = elf_sections(elf)
    _, _, shstrndx = struct.unpack_from("<HHH", elf, 0x2E)
    names = [elf[sec[shstrndx][4] + s[0]:].split(b"\0", 1)[0].decode() for s in sec]
    ro = names.index(".rodata")
    out = set()
    for s in sec:
        if s[1] == 9 and s[7] == ro:  # SHT_REL for .rodata
            for k in range(s[5] // 8):
                r_offset, r_info = struct.unpack_from("<II", elf, s[4] + k * 8)
                if r_info & 0xFF != 2:  # R_MIPS_32
                    raise SystemExit(f"{path}: a .rodata relocation of type {r_info & 0xFF}")
                out.add(r_offset)
    return sorted(out)


def rodata_markers():
    """Every __rodata_ marker that a linker-defined name is relative to"""
    names = syms.LinkerNames(ROOT, inputs=False)
    return sorted({base for base, _, _ in names.rel.values() if base.startswith("__rodata_")})


def make():
    """{file name: text} of port/inputs/"""
    out = {}
    for path in sorted(glob.glob(os.path.join(ROOT, "build", "c_syms*.ld"))):
        with open(path, encoding="utf-8") as f:
            out[os.path.basename(path)] = f.read()
    if not out:
        raise SystemExit("build/c_syms*.ld not found: run the matching build first")
    labels = syms.asm_labels(ROOT)
    if not labels:
        raise SystemExit("asm/ not found: run tools/split.sh first")
    out["asm_labels.json"] = json.dumps({k: list(v) for k, v in sorted(labels.items())}, indent=0) + "\n"

    elves, originals, symtabs = {}, {}, {}
    rodata = {}
    for marker in rodata_markers():
        binary = marker.split("_")[3]
        if binary not in elves:
            elf_name, iso_name = BINARIES[binary]
            with open(os.path.join(ROOT, "build", elf_name), "rb") as f:
                elves[binary] = f.read()
            with open(os.path.join(ROOT, "iso", iso_name), "rb") as f:
                originals[binary] = f.read()
            symtabs[binary] = elf_symbols(elves[binary])
        data, objs = syms.rodata_from_object(ROOT, marker)
        vma = symtabs[binary].get(marker)
        if vma is None:
            raise SystemExit(f"{marker} isn't in build/{BINARIES[binary][0]}")
        off = file_offset(binary, vma, elves[binary], originals[binary])
        on_disc = bytearray(originals[binary][off:off + len(data)])
        # The object's relocated words hold addends (offsets into its own
        # sections), the disc's the linked addresses: keep the addends
        addends = {}
        for r in rodata_relocs(syms.rodata_object(ROOT, marker)):
            addends[str(r)] = data[r:r + 4].hex()
            on_disc[r:r + 4] = data[r:r + 4]
        if bytes(on_disc) != data:
            raise SystemExit(f"{marker}: the matching build's .rodata isn't at 0x{off:X} in "
                             f"iso/{BINARIES[binary][1]}")
        rodata[marker] = {"file": BINARIES[binary][1], "offset": off, "size": len(data),
                          "objects": [list(o) for o in objs]}
        if addends:
            rodata[marker]["addends"] = addends
    out["rodata.json"] = json.dumps(rodata, indent=1, sort_keys=True) + "\n"
    return out


def main():
    want = make()
    if "--check" in sys.argv[1:]:
        stale = []
        for name, text in want.items():
            path = os.path.join(INPUTS, name)
            try:
                with open(path, encoding="utf-8") as f:
                    if f.read() != text:
                        stale.append(name)
            except OSError:
                stale.append(name)
        extra = {os.path.basename(p) for p in glob.glob(os.path.join(INPUTS, "*"))} - set(want) - {"README.md"}
        if stale or extra:
            sys.exit("port/inputs/ is out of date: " + ", ".join(sorted(stale) + sorted(extra)))
        print("port/inputs/ matches build/")
        return
    if os.path.isdir(INPUTS):
        for p in glob.glob(os.path.join(INPUTS, "*")):
            if os.path.basename(p) not in want and os.path.basename(p) != "README.md":
                os.remove(p) if os.path.isfile(p) else shutil.rmtree(p)
    os.makedirs(INPUTS, exist_ok=True)
    for name, text in want.items():
        with open(os.path.join(INPUTS, name), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    print(f"wrote port/inputs/: {', '.join(sorted(want))}")


if __name__ == "__main__":
    main()
