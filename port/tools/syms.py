"""Names the port has to define itself (port/tools/build.py).

1. Names the matching build's linker defines (build/c_syms*.ld, from
   tools/c_syms.py): a label plus an offset (a field inside another
   object), or a fixed address. lld's COFF mode has no --defsym, so an
   offset alias goes into the generated copy of the file that defines its
   base, as `.set` (COFF can't express "undefined symbol + offset"). A
   string inside another C file's .rodata (`__rodata_<file> + off`) has no
   counterpart in clang's output; its bytes are copied from the matching
   build's object into linker_data.c. Fixed hardware addresses get buffers
   there too, until the platform layer gives them a meaning.
   Without the matching build (or with GRX_PORT_INPUTS=1) the names come
   from port/inputs/ and the bytes from the game's own files in iso/
   (port/tools/port_inputs.py).
2. What is still undefined when linking: functions still in assembly,
   kernel calls, data only asm defines. stubs.c gets a function that stops
   the game with its name, or a zeroed buffer of the asm object's size.
"""

import glob
import json
import os
import re
import struct

# i686 Windows C names carry a leading underscore in assembly; x86-64's
# don't (build.py's set_target sets it)
PREFIX = "_"

# On x86-64: linker-defined names that move there (fixups/bss64.json's
# "aliases", from gen_bss64.py; build.py's set_target sets it):
# {name: ["own", size]} an object of its own, {name: ["set", base, offset]}
# an alias at another place
MOVED = {}


def from_inputs():
    """GRX_PORT_INPUTS=1: take what the matching build gives from
    port/inputs/ even where build/ has it (to check them)"""
    return os.environ.get("GRX_PORT_INPUTS", "") not in ("", "0")


def inputs_dir(root):
    return os.path.join(root, "port", "inputs")


class LinkerNames:
    def __init__(self, root, inputs=None):
        """The matching build's c_syms*.ld from build/, or else (or with
        inputs=True) their copies in port/inputs/ (port_inputs.py)"""
        self.root = root
        files = []
        if not inputs and not (inputs is None and from_inputs()):
            files = sorted(glob.glob(os.path.join(root, "build", "c_syms*.ld")))
        if not files and inputs is not False:
            files = sorted(glob.glob(os.path.join(inputs_dir(root), "c_syms*.ld")))
        if not files:
            raise SystemExit("c_syms*.ld not found in build/ or port/inputs/: run the matching build "
                             "first (tools/docker.sh make)")
        self.rel = {}    # name -> (base, offset)
        self.fixed = {}  # name -> address
        for path in files:
            binary = re.match(r"c_syms\.?(\w*)\.ld", os.path.basename(path)).group(1) or "main"
            with open(path) as f:
                for m in re.finditer(r"PROVIDE\(([\w.]+) = ([\w.]+)(?: \+ (0x[0-9A-Fa-f]+))?\);", f.read()):
                    name, base, off = m.group(1), m.group(2), m.group(3)
                    if name.startswith(".L"):
                        continue
                    if base.startswith("0x"):
                        # The overlays' files give every main-executable
                        # name its address too; those the port defines
                        # (or stubs) like any other. Only addresses outside
                        # the game's memory are fixed by the hardware.
                        addr = int(base, 16)
                        if not 0x00400000 <= addr < 0x02000000:
                            self.fixed.setdefault(name, addr)
                    else:
                        self.rel.setdefault(name, (base, int(off, 16) if off else 0, binary))
        # The hand-written ones: names computed from a label
        # (config/symbol_exprs.ld) and the fixed memory map (load areas of
        # the overlays and of the disc's programs, config/memory_map.ld)
        with open(os.path.join(root, "config", "symbol_exprs.ld")) as f:
            for m in re.finditer(r"PROVIDE\((\w+) = (\w+)\s*([-+][-+*0-9A-Fa-fx\s]*)\);", f.read()):
                off = eval(m.group(3).replace(" ", "")) if m.group(3).strip() else 0
                self.rel.setdefault(m.group(1), (m.group(2), off, "any"))
        # Plain aliases (placeholder names for the soft-float helpers, ...)
        with open(os.path.join(root, "config", "linker_script_extra.ld")) as f:
            for m in re.finditer(r"^(\w+) = (\w+);", f.read(), re.M):
                self.rel.setdefault(m.group(1), (m.group(2), 0, "any"))
        with open(os.path.join(root, "config", "memory_map.ld")) as f:
            for m in re.finditer(r"^(\w+)\s*=\s*(0x[0-9A-Fa-f]+)\s*;", f.read(), re.M):
                self.fixed.setdefault(m.group(1), int(m.group(2), 16))

    def _resolve(self, name, defined, depth=0):
        """(base defined by a port object, offset) for an alias, or None"""
        if name in defined:
            return name, 0
        if name not in self.rel or depth > 8:
            return None
        base, off, _ = self.rel[name]
        ro = _rodata(self.root, base)
        if ro:
            # Inside another file's .rodata: relative to the named object
            # there, if there is one (a string literal has no name)
            for sym, value, size in ro[1]:
                if value <= off < value + max(size, 1) and sym in defined:
                    return sym, off - value
            return None
        r = self._resolve(base, defined, depth + 1)
        return (r[0], r[1] + off) if r else None

    def per_file_asm(self, defined):
        """{source: top-level asm defining the aliases whose base it defines}"""
        out = {}
        for name in sorted(self.rel):
            if name in defined:
                continue
            r = self._resolve(name, defined)
            if not r:
                continue
            base, off = r
            moved = MOVED.get(name)
            if moved and moved[0] == "own":
                # In the base's file, as before, but an object of its own
                # (the comment marks it as placed here: placed_names)
                out.setdefault(defined[base], []).append(
                    f"unsigned char {name}[{moved[1]}] __attribute__((aligned(16))); /* placed {name} */")
                continue
            if moved:
                base, off = moved[1], moved[2]
                if base not in defined:
                    raise SystemExit(f"{name}: its x86-64 base {base} isn't defined (bss64.json)")
            out.setdefault(defined[base], []).append(
                f'__asm__(".globl {PREFIX}{name}\\n.set {PREFIX}{name}, {PREFIX}{base}{off:+d}");')
        return {k: "/* Names the matching build's linker defines (port/tools/syms.py) */\n" + "\n".join(v) + "\n"
                for k, v in out.items()}

    @staticmethod
    def placed_names(text):
        """{name: True} of the names per_file_asm's text defines: its
        aliases (.globl) and its objects of their own (/* placed */)"""
        return set(re.findall(r"\.globl " + PREFIX + r"(\w+)\\n", text)) | set(re.findall(r"/\* placed (\w+) \*/", text))

    def write_data_c(self, path, defined):
        lines = ["/* Generated by port/tools/syms.py: names the matching build's linker",
                 "   defines that aren't relative to anything the port compiles. */", ""]
        for name in sorted(self.rel):
            if name in defined or self._resolve(name, defined):
                continue
            base, off, binary = self.rel[name]
            if not base.startswith("__rodata_"):
                continue  # left to the stubs
            ro = _rodata(self.root, base)
            if ro is None:
                raise SystemExit(f"{name}: no matching-build object for {base}")
            data = ro[0]
            # From the offset to the end of the section: the string (or
            # constant) the name points at, and whatever follows it
            chunk = data[off:] or b"\0"
            body = ",".join(str(b) for b in chunk)
            lines.append(f"/* {base} + 0x{off:X} */")
            lines.append(f"const unsigned char {name}[{len(chunk)}] __attribute__((aligned(16))) = {{{body}}};")
        lines.append("")
        lines.append("/* Fixed addresses (hardware, scratchpad, uncached aliases): buffers for now */")
        for name, addr in sorted(self.fixed.items()):
            if name in defined:
                continue
            lines.append(f"unsigned char {name}[0x10000] __attribute__((aligned(16))); /* 0x{addr:08X} */")
        with open(path, "w", newline="\n") as f:
            f.write("\n".join(lines) + "\n")


_RODATA_CACHE = {}


def _rodata(root, base):
    """(bytes, [(symbol, offset, size)]) of the .rodata that a
    __rodata_<binary>_<file> marker starts: from the matching build's
    object, or else from port/inputs/rodata.json and the game's own file in
    iso/; None if base isn't such a marker"""
    m = re.fullmatch(r"__rodata_(main|field|battle|viewer)_(\w+)", base)
    if not m:
        return None
    if base not in _RODATA_CACHE:
        found = None if from_inputs() else rodata_from_object(root, base, missing_ok=True)
        _RODATA_CACHE[base] = found or rodata_from_inputs(root, base)
    return _RODATA_CACHE[base]


_INPUT_RODATA = {}


def rodata_from_inputs(root, base):
    if not _INPUT_RODATA:
        path = os.path.join(inputs_dir(root), "rodata.json")
        if not os.path.exists(path):
            raise SystemExit(f"no matching-build object for {base}, and no port/inputs/rodata.json")
        with open(path, encoding="utf-8") as f:
            _INPUT_RODATA.update(json.load(f))
    e = _INPUT_RODATA.get(base)
    if e is None:
        raise SystemExit(f"{base} isn't in port/inputs/rodata.json (port/tools/port_inputs.py)")
    game_file = os.path.join(root, "iso", e["file"])
    try:
        with open(game_file, "rb") as f:
            f.seek(e["offset"])
            data = f.read(e["size"])
    except OSError:
        raise SystemExit(f"iso/{e['file']} not found: the port takes some of the game's constants "
                         "from it (README.md, Building)")
    if len(data) != e["size"]:
        raise SystemExit(f"iso/{e['file']} is too short: is it the US disc's?")
    # The words the linker relocates hold their addends, as in the object
    data = bytearray(data)
    for off, word in e.get("addends", {}).items():
        data[int(off):int(off) + 4] = bytes.fromhex(word)
    return bytes(data), [tuple(o) for o in e["objects"]]


def rodata_object(root, base):
    """The matching build's object whose .rodata a marker starts, or None"""
    binary, file = re.fullmatch(r"__rodata_(main|field|battle|viewer)_(\w+)", base).groups()
    # The Makefile names the marker after the path with / as _
    cands = [file, re.sub(r"^data_", "data/", file)]
    return next((p for p in (os.path.join(root, "build", "src", binary, c + ".o") for c in cands)
                 if os.path.exists(p)), None)


def rodata_from_object(root, base, missing_ok=False):
    """(bytes, [(symbol, offset, size)]) of a marker's .rodata from the
    matching build's object (port_inputs.py records where they are). The
    bytes are the object's: words the linker relocates hold their addends"""
    path = rodata_object(root, base)
    if path is None:
        if missing_ok:
            return None
        raise SystemExit(f"no matching-build object for {base} (run the matching build)")
    with open(path, "rb") as f:
        elf = f.read()
    shoff, = struct.unpack_from("<I", elf, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x2E)
    sec = [struct.unpack_from("<IIIIIIIIII", elf, shoff + i * shentsize) for i in range(shnum)]

    def secname(s, strtab):
        return elf[sec[strtab][4] + s[0]:].split(b"\0", 1)[0].decode()

    names = [secname(s, shstrndx) for s in sec]
    if ".rodata" not in names:
        raise SystemExit(f"{path}: no .rodata")
    ro = names.index(".rodata")
    data = elf[sec[ro][4]:sec[ro][4] + sec[ro][5]]
    syms = []
    for i, s in enumerate(sec):
        if s[1] != 2:  # SHT_SYMTAB
            continue
        for k in range(s[5] // 16):
            st_name, st_value, st_size, st_info, _, st_shndx = struct.unpack_from("<IIIBBH", elf, s[4] + k * 16)
            if st_shndx == ro and st_info & 0xF in (0, 1) and st_name:
                n = elf[sec[s[6]][4] + st_name:].split(b"\0", 1)[0].decode()
                if not n.startswith((".", "__rodata_")):
                    syms.append((n, st_value, st_size))
    return data, syms


# --- stubs ---------------------------------------------------------------------

_SIZES = {".word": 4, ".half": 2, ".short": 2, ".byte": 1, ".float": 4, ".double": 8, ".dword": 8}


def asm_labels(root):
    """{name: ('func', 0) or ('data', size)} from asm/ (tools/split.sh)"""
    out = {}
    for path in glob.glob(os.path.join(root, "asm", "**", "*.s"), recursive=True):
        cur, size = None, 0
        with open(path, encoding="latin-1") as f:
            for line in f:
                # Data lines start with a /* address */ comment
                line = re.sub(r"/\*.*?\*/", "", line).strip()
                m = re.match(r"(glabel|dlabel)\s+(\w+)", line)
                if m:
                    if m.group(1) == "glabel":
                        out.setdefault(m.group(2), ("func", 0))
                    else:
                        cur, size = m.group(2), 0
                    continue
                if cur is None:
                    continue
                if line.startswith("enddlabel"):
                    out.setdefault(cur, ("data", size))
                    cur = None
                    continue
                m = re.match(r"(\.\w+)\s*(.*)", line)
                if not m:
                    continue
                d, rest = m.groups()
                if d in _SIZES:
                    size += _SIZES[d] * (rest.count(",") + 1)
                elif d == ".space":
                    size += int(rest.split(",")[0], 0)
                elif d in (".asciz", ".string", ".ascii"):
                    s = re.findall(r'"((?:[^"\\]|\\.)*)"', rest)
                    n = sum(len(re.sub(r"\\(x..|[0-7]{1,3}|.)", "x", x)) for x in s)
                    size += n + (len(s) if d != ".ascii" else 0)
    return out


def stub_labels(root):
    """asm_labels from asm/, or else port/inputs/asm_labels.json"""
    labels = {} if from_inputs() else asm_labels(root)
    path = os.path.join(inputs_dir(root), "asm_labels.json")
    if not labels and os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            labels = {k: tuple(v) for k, v in json.load(f).items()}
    return labels


def write_stubs(path, undefined, root):
    labels = stub_labels(root) if undefined else {}
    funcs, data = [], []
    for raw in sorted(undefined):
        name = raw
        kind = labels.get(name)
        if kind is None and PREFIX and name.startswith(PREFIX):
            kind = labels.get(name[len(PREFIX):])
        if kind and kind[0] == "data":
            data.append(f"unsigned char {name}[0x{max(kind[1], 16):X}] __attribute__((aligned(16)));")
        elif kind or not re.match(r"[DB]_[0-9A-F]{8}$", name):
            funcs.append(f'void {name}(void) {{ grx_unimplemented("{name}"); }}')
        else:
            # Unknown data: big enough for most tables
            data.append(f"unsigned char {name}[0x1000] __attribute__((aligned(16))); /* size unknown */")
    with open(path, "w", newline="\n") as f:
        f.write("/* Generated by port/tools/syms.py: what the port doesn't have yet. */\n")
        f.write('#include "grx_platform.h"\n\n')
        f.write("\n".join(data) + "\n\n" + "\n".join(funcs) + "\n")
