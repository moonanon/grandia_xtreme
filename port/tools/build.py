#!/usr/bin/env python3
"""Build the Windows port (port/README.md): the game's C compiled with clang
for i686 Windows (or x86-64, port/docs/win64.md), linked with the platform
layer in port/.

    python port/tools/build.py                    # build build/port/grandia.exe
    python port/tools/build.py --target x86_64    # build build/port64/grandia.exe
    python port/tools/build.py --list             # which sources are in, which are replaced

The matching build is untouched. Each game source is copied to
build/port/gen/ with its inline asm translated to C (asm2c.py) and the few
constructs clang rejects fixed (FIXUPS), then compiled; `#line` keeps
errors pointing at the original file.

Needs: clang from llvm-mingw (on PATH, in GRX_CLANG, or the winget
install), asm/ from tools/split.sh (for the names of the functions still
in assembly) and build/c_syms*.ld from the matching build (for the names
the linker defines).
"""

import argparse
import concurrent.futures
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import threading

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.dirname(__file__))
import asm2c  # noqa: E402
import syms  # noqa: E402

BINARIES = ["main", "field", "battle", "viewer"]

# What differs between the targets: the triple, the C names' prefix in
# assembly (COFF has the leading underscore only on i686), llvm-dlltool's
# machine and the default output directory. --target picks one.
TARGETS = {
    "i686": {"triple": "i686-w64-mingw32", "prefix": "_", "machine": "i386", "out": "port"},
    "x86_64": {"triple": "x86_64-w64-mingw32", "prefix": "", "machine": "i386:x86-64", "out": "port64"},
    # Native Linux, x86-64: the same 32-bit-EE-address build as x86_64, ELF and glibc
    "linux64": {"triple": "x86_64-unknown-linux-gnu", "prefix": "", "machine": "i386:x86-64", "out": "portlinux"},
}
LINUX = False
EE64_TARGETS = ("x86_64", "linux64")
TARGET = TARGETS["i686"]["triple"]
OUT = os.path.join(ROOT, "build", TARGETS["i686"]["out"])


def set_target(name):
    global TARGET, OUT, LINUX
    t = TARGETS[name]
    LINUX = name == "linux64"
    TARGET = t["triple"]
    OUT = os.path.join(ROOT, "build", t["out"])
    syms.PREFIX = t["prefix"]
    # The linker-defined names that move on x86-64 (port/tools/gen_bss64.py)
    syms.MOVED = {}
    if name in EE64_TARGETS:
        with open(os.path.join(ROOT, "port", "tools", "fixups", "bss64.json")) as f:
            syms.MOVED = json.load(f)["aliases"]

# Library code the port takes from the host instead: libgcc (soft float,
# 64-bit division) and newlib's libc and libm. The game calls them by their
# standard names, which the Windows C runtime provides. Sony's SDK code
# stays (the platform layer replaces it piece by piece), and so does
# libmpeg's decoder, which follows MSSG's mpeg2decode.
HOST_LIB = re.compile(r"Third-party library code")
KEEP_LIB = re.compile(r"Sony|MSSG|libmpeg|mpeg2decode")
# Library code the game depends on exactly, so it stays although the host
# has the same names: newlib's rand returns 31 bits, the Windows C
# runtime's only 15, and the game takes rand() >> 15 and >> 16 (with the
# host's, VuRandInt is always 0 and FldSpotSetup's slot search never ends)
HOST_LIB_KEEP = {"src/main/482970.c"}

# Game sources the platform layer replaces (port/src/), left out like the
# library code above: {first, last} address ranges of src/<binary>/ files
PORT_REPLACES = [
    # libcdvd, over the disc image (port/src/cdvd.c, port/docs/disc.md)
    ("main", 0x46EB30, 0x46FD58),
    # sceSifGetOtherData: IOP memory reads, done at once (port/src/cdvd.c)
    ("main", 0x48EBD8, 0x48EBD8),
]


def port_replaced(rel):
    m = re.fullmatch(r"src/(\w+)/([0-9A-F]{6})\.c", rel)
    return bool(m) and any(b == m.group(1) and lo <= int(m.group(2), 16) <= hi for b, lo, hi in PORT_REPLACES)

GAME_FLAGS = [
    "-std=gnu89", "-fno-builtin", "-fcommon", "-fno-strict-aliasing", "-fwrapv",
    # Single-precision maths rounded at every step, as on the EE (the x87
    # default keeps extended-precision intermediates)
    "-msse2", "-mfpmath=sse",
    # Don't assume a pointer's target is aligned as its type says: FVECTOR
    # is aligned(16), but the game casts pointers into unaligned records to
    # such types (FIELD's point lights into GphLightInit), which was fine
    # with the EE's word stores and faults with SSE's movaps. Struct layout
    # is unchanged.
    "-fmax-type-align=4",
    "-fno-delete-null-pointer-checks",
    # Floats converted to integers they don't fit (a negative angle to u16,
    # say) are defined on the EE (cvt.w.s, then the low bits); in C they
    # are undefined, so clang mustn't assume they can't happen. This makes
    # them saturate; compile_one then gives the narrow and unsigned ones the
    # EE's low bits (ee_float_casts)
    "-fno-strict-float-cast-overflow",
    # The game's C mixes ints and pointers freely; on a 32-bit target that
    # is harmless, so these stay warnings
    "-Wno-error=int-conversion", "-Wno-error=incompatible-pointer-types",
    "-Wno-error=incompatible-function-pointer-types", "-Wno-error=return-type",
    "-w",
]
PORT_FLAGS = ["-std=gnu11", "-Wall", "-Wno-unused-function", "-msse2", "-mfpmath=sse"]

# Constructs ee-gcc 2.9 accepts and clang doesn't, per file: (regex, replacement)
FIXUPS = {
    # Implicit declarations, then definitions returning void
    "src/field/5FD6D0.c": [(r'(#include "m2c_macros.h"\n)',
                            r"\1" + "".join(f"void EvtNullHook{i:02d}(void); " for i in range(1, 14)))],
    # A local that redeclares a parameter (it shadows it; the parameter is
    # never read, so dropping the local changes nothing)
    "src/field/622E30.c": [(r"(s32 y, u8 a\) \{\n)    u8 a;\n", r"\1\n")],

    # Program loading. FIELD, BATTLE and VIEWER are linked in, so each
    # sysProgramTable entry is the overlay's entry function instead of its
    # load area (TITLE, ENDING, MCCHECK and AUTOMAP aren't on the disc: a
    # stop), and SysProgLoad no longer reads the file over it
    "src/main/data/499B00.c": [
        (r"(extern u8 SUBPROG_VRAM\[\];\n)", r"\1int FldEntry(void); int BtlEntry(void); int VwrEntry(void);\n"),
        (r'"VIEWER\.BIN", SUBPROG_VRAM', '"VIEWER.BIN", (u8 *)VwrEntry'),
        (r'"FIELD\.BIN", FIELD_VRAM', '"FIELD.BIN", (u8 *)FldEntry'),
        (r'"BATTLE\.BIN", BATTLE_VRAM', '"BATTLE.BIN", (u8 *)BtlEntry'),
        (r"SUBPROG_VRAM \}", "(u8 *)grx_program_absent }"),
    ],
    "src/main/400988.c": [(r"    FileGetSizeAligned\(&sysProgramTable\[sysState\.program\]\);\n    FileRead\([^\n]*\n",
                           "    /* port: linked in, nothing to read */\n\n")],
    # The disc's MIPS programs and their loading: port/tools/fixups/mips.py
}
# The same for include/'s headers ({"include/<path>": [(regex,
# replacement)]}): the game's sources compile against a copy of include/
# (OUT/hdr, generate_headers) with these applied
HEADER_FIXUPS = {}


# More fixups and replaced sources, one module per area of the platform
# layer (port/tools/fixups/*.py, each with optional FIXUPS, HEADER_FIXUPS
# and REPLACES like the ones above), so work on different areas doesn't
# touch this file. A file's fixups from several modules apply in
# module-name order.
def _load_fixup_modules():
    import importlib.util
    for path in sorted(glob.glob(os.path.join(os.path.dirname(__file__), "fixups", "*.py"))):
        spec = importlib.util.spec_from_file_location("fixups_" + os.path.basename(path)[:-3], path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        for rel, subs in getattr(mod, "FIXUPS", {}).items():
            FIXUPS.setdefault(rel, []).extend(subs)
        for rel, subs in getattr(mod, "HEADER_FIXUPS", {}).items():
            HEADER_FIXUPS.setdefault(rel, []).extend(subs)
        PORT_REPLACES.extend(getattr(mod, "REPLACES", []))


_load_fixup_modules()

# The EE varargs macros in a few files: clang's builtin instead
VA_START = re.compile(r"#define\s+va_start\(\s*(\w+)\s*,\s*(\w+)\s*\)(?:[^\n]*\\\n)*[^\n]*")


def brew_prefix():
    """Homebrew's prefix (for clang and SDL3 when they aren't on the system), or ''"""
    try:
        return subprocess.run(["brew", "--prefix"], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


BREW = os.environ.get("HOMEBREW_PREFIX") or brew_prefix()


def find_clang():
    """Linux: GRX_CLANG, clang on PATH or Homebrew's. Windows: llvm-mingw's <triple>-clang: GRX_CLANG (or its sibling for the other
    target), on PATH, or the winget install"""
    if LINUX:
        c = os.environ.get("GRX_CLANG") or shutil.which("clang") or os.path.join(BREW, "bin", "clang")
        if not os.path.exists(c):
            sys.exit("clang not found: install it (brew install llvm) or set GRX_CLANG")
        return c
    c = os.environ.get("GRX_CLANG")
    if c:
        sib = os.path.join(os.path.dirname(c), f"{TARGET}-clang" + (".exe" if c.lower().endswith(".exe") else ""))
        return sib if os.path.exists(sib) else c
    c = shutil.which(f"{TARGET}-clang")
    if c:
        return c
    pkgs = os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\WinGet\Packages")
    found = glob.glob(os.path.join(pkgs, "MartinStorsjo.LLVM-MinGW*", "llvm-mingw*", "bin", f"{TARGET}-clang.exe"))
    if found:
        return found[0]
    sys.exit(f"clang for {TARGET} not found: install llvm-mingw "
             "(winget install MartinStorsjo.LLVM-MinGW.UCRT) or set GRX_CLANG")


def find_sdl3():
    """Linux: SDL3's install prefix (pkg-config, GRX_SDL3). Windows: SDL3's mingw development tree for the target (port/README.md): GRX_SDL3,
    or the newest port/deps/SDL3-*"""
    if LINUX:
        base = os.environ.get("GRX_SDL3")
        if not base:
            env = dict(os.environ, PKG_CONFIG_PATH=os.pathsep.join(
                filter(None, [os.environ.get("PKG_CONFIG_PATH"), os.path.join(BREW, "lib", "pkgconfig")])))
            p = subprocess.run(["pkg-config", "--variable=prefix", "sdl3"], capture_output=True, text=True, env=env)
            base = p.stdout.strip() if p.returncode == 0 else None
        if not base or not os.path.exists(os.path.join(base, "include", "SDL3", "SDL.h")):
            sys.exit("SDL3 not found: install it (brew install sdl3, or your distro's SDL3 development package) "
                     "or set GRX_SDL3 to its prefix")
        return base
    base = os.environ.get("GRX_SDL3")
    if not base:
        found = sorted(glob.glob(os.path.join(ROOT, "port", "deps", "SDL3-*")))
        base = found[-1] if found else None
    tree = base and os.path.join(base, TARGET)
    if not tree or not os.path.exists(os.path.join(tree, "include", "SDL3", "SDL.h")):
        sys.exit("SDL3 not found: put the official SDL3-devel-<version>-mingw release in port/deps/ "
                 "(https://github.com/libsdl-org/SDL/releases) or set GRX_SDL3")
    return tree


def sdl3_delay_lib(clang, sdl):
    """An import library for SDL3.dll that can be delay-loaded: SDL's own
    can't, and a static import loads user32/gdi32 before main(), which
    maps ~20 MB of low address space the PS2 memory needs
    (port/docs/hardware.md). Built from the DLL's exports."""
    if LINUX:
        lib = os.path.join(sdl, "lib")
        return [f"-L{lib}", "-lSDL3", f"-Wl,-rpath,{lib}"]
    bindir = os.path.dirname(clang)
    dll = os.path.join(sdl, "bin", "SDL3.dll")
    lib = os.path.join(OUT, "libSDL3_delay.a")
    if not newer(lib, [dll]):
        return lib
    os.makedirs(OUT, exist_ok=True)
    p = subprocess.run([os.path.join(bindir, "gendef"), "-", dll], capture_output=True, text=True, check=True)
    deffile = os.path.join(OUT, "SDL3.def")
    with open(deffile, "w", newline="\n") as f:
        f.write(p.stdout)
    machine = next(t["machine"] for t in TARGETS.values() if t["triple"] == TARGET)
    subprocess.run([os.path.join(bindir, "llvm-dlltool"), "-m", machine, "-d", deffile, "-l", lib], check=True)
    return lib


def game_sources():
    """(path relative to ROOT, binary, replaced by the host) for every C file:
    the game's, then port/game/, the port's own C for functions the game
    still has in assembly (they needn't match; nothing but the port uses
    them)"""
    out = []
    for b in BINARIES:
        for p in sorted(glob.glob(os.path.join(ROOT, "src", b, "*.c")) + glob.glob(os.path.join(ROOT, "src", b, "data", "*.c"))):
            rel = os.path.relpath(p, ROOT).replace("\\", "/")
            with open(p, encoding="latin-1") as f:
                head = f.read(600)
            host = bool(HOST_LIB.search(head) and not KEEP_LIB.search(head)) and rel not in HOST_LIB_KEEP
            out.append((rel, b, host or port_replaced(rel)))
    for b in BINARIES:
        for p in sorted(glob.glob(os.path.join(ROOT, "port", "game", b, "*.c"))):
            out.append((os.path.relpath(p, ROOT).replace("\\", "/"), b, False))
    return out


def generate(rel, extra="", out_dir=None):
    """The port's copy of a game source; returns its path"""
    with open(os.path.join(ROOT, rel), encoding="latin-1") as f:
        src = f.read()
    src, _ = asm2c.translate(src)
    src = VA_START.sub(lambda m: f"#define va_start({m.group(1)}, {m.group(2)}) __builtin_va_start({m.group(1)}, {m.group(2)})"
                       + "\n" * m.group(0).count("\n"), src)
    for pat, rep in FIXUPS.get(rel, []):
        src, n = re.subn(pat, rep, src)
        if n == 0:
            raise RuntimeError(f"{rel}: fixup {pat!r} no longer applies")
    out = os.path.join(out_dir or os.path.join(OUT, "gen"), rel)
    text = f'#line 1 "{rel}"\n{src}'
    if extra:
        text += "\n" + extra
    os.makedirs(os.path.dirname(out), exist_ok=True)
    old = None
    if os.path.exists(out):
        with open(out, encoding="latin-1") as f:
            old = f.read()
    if old != text:
        with open(out, "w", encoding="latin-1", newline="\n") as f:
            f.write(text)
    return out


_headers_done = {}


def generate_headers():
    """The game's copy of include/ (OUT/hdr) with HEADER_FIXUPS applied,
    each starting with a #line naming the original; files are rewritten
    only when their text changes. Returns the copies' paths."""
    hdr = os.path.join(OUT, "hdr")
    if hdr in _headers_done:
        return _headers_done[hdr]
    src_root = os.path.join(ROOT, "include")
    out = []
    for p in sorted(glob.glob(os.path.join(src_root, "**", "*.h"), recursive=True)):
        rel = "include/" + os.path.relpath(p, src_root).replace("\\", "/")
        with open(p, encoding="latin-1") as f:
            text = f.read()
        for pat, rep in HEADER_FIXUPS.get(rel, []):
            text, n = re.subn(pat, rep, text)
            if n == 0:
                raise RuntimeError(f"{rel}: header fixup {pat!r} no longer applies")
        text = f'#line 1 "{rel}"\n{text}'
        dst = os.path.join(hdr, os.path.relpath(p, src_root))
        old = None
        if os.path.exists(dst):
            with open(dst, encoding="latin-1") as f:
                old = f.read()
        if old != text:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "w", encoding="latin-1", newline="\n") as f:
                f.write(text)
        out.append(dst)
    missing = set(HEADER_FIXUPS) - {"include/" + os.path.relpath(p, src_root).replace("\\", "/")
                                    for p in glob.glob(os.path.join(src_root, "**", "*.h"), recursive=True)}
    if missing:
        raise RuntimeError(f"header fixups for headers that don't exist: {sorted(missing)}")
    _headers_done[hdr] = out
    return out


def headers_signature():
    """A hash of the game's types as it sees them (include/ with the header
    fixups, and the port's headers before them that shape types):
    port/tools/gen_bss64.py records it in fixups/bss64.json, which
    describes them"""
    h = hashlib.sha1()
    port = [os.path.join(ROOT, "port", "include", n) for n in ("grx_port.h", "grx_ee.h", "grx_ee_names.h", "grx_ee64.h")]
    for p in generate_headers() + port:
        with open(p, "rb") as f:
            # line endings as in the repository: a Windows checkout
            # (core.autocrlf) has CRLF, which changes no type
            h.update(os.path.basename(p).encode() + f.read().replace(b"\r\n", b"\n"))
    return h.hexdigest()


def newer(target, deps):
    if not os.path.exists(target):
        return True
    t = os.path.getmtime(target)
    # A dependency that has gone (a header a source no longer finds there)
    # counts as changed
    return any(not os.path.exists(d) or os.path.getmtime(d) > t for d in deps)


# Float-to-integer conversions as the EE does them. The EE converts a float
# to s32 with cvt.w.s, which saturates; a narrower or unsigned target gets
# that s32's low bits (gcc stores or masks them). So a negative angle stored
# in a u16 wraps (-90 degrees: 0xC000), where C leaves it undefined.
# -fno-strict-float-cast-overflow (GAME_FLAGS) makes clang's conversions
# defined, but as saturation to the target type: right for s32, wrong for
# the rest (that angle became 0). So the game's sources go through LLVM IR,
# where those conversions are calls to llvm.fpto[su]i.sat.iN: the 8- and
# 16-bit ones become the s32 saturation and a truncation, and the unsigned
# 32-bit one a 64-bit conversion and a truncation (the low bits, as the EE
# gives for any value it gets in range).
_SAT_RE = re.compile(r"^(\s*)(%[-\w.$]+) = ((?:tail |notail )?)call (?:[\w ]* )?(i8|i16|i32) "
                     r"@llvm\.fpto(s|u)i\.sat\.i(?:8|16|32)\.(f32|f64)\(((?:float|double)[^()]*)\)(.*)$")


def ee_float_casts(ll):
    """The IR with the EE's narrow and unsigned float conversions; None if
    there were none"""
    if "fptosi.sat.v" in ll or "fptoui.sat.v" in ll:
        raise RuntimeError("vector float-to-int conversion: ee_float_casts doesn't handle it")
    out, n, need = [], 0, set()
    for line in ll.split("\n"):
        m = _SAT_RE.match(line)
        if m:
            ind, name, tail, ty, su, fty, arg, rest = m.groups()
            if ty != "i32" or su == "u":
                wide = "i64" if ty == "i32" else "i32"
                tmp = "%ee." + name[1:]
                dbg = re.search(r", !dbg ![0-9]+", rest)
                out.append(f"{ind}{tmp} = {tail}call {wide} @llvm.fptosi.sat.{wide}.{fty}({arg}){rest}")
                out.append(f"{ind}{name} = trunc {wide} {tmp} to {ty}{dbg.group(0) if dbg else ''}")
                need.add(f"declare {wide} @llvm.fptosi.sat.{wide}.{fty}({'float' if fty == 'f32' else 'double'})")
                n += 1
                continue
        out.append(line)
    if not n:
        return None
    text = "\n".join(out)
    for d in sorted(need):
        if d.split("(")[0] not in text.replace("declare noundef ", "declare "):
            text += "\n" + d + "\n"
    return text


# Going through IR costs a second clang run, so OUT/ee_casts.json keeps
# which sources need it (most don't): keyed by the source's text, the
# headers' and this scheme's version, so anything that could add a
# conversion sends the file through IR again
EE_CASTS_VERSION = 1
_ee_known = {}
_ee_lock = threading.Lock()
_ee_sig = ""


def ee_casts_load(headers):
    global _ee_known, _ee_sig
    h = hashlib.sha1(str(EE_CASTS_VERSION).encode())
    for p in sorted(headers):
        with open(p, "rb") as f:
            h.update(p.encode() + f.read())
    _ee_sig = h.hexdigest()
    try:
        with open(os.path.join(OUT, "ee_casts.json")) as f:
            _ee_known = json.load(f)
    except (OSError, ValueError):
        _ee_known = {}


def ee_casts_save():
    with open(os.path.join(OUT, "ee_casts.json"), "w") as f:
        json.dump(_ee_known, f)


_direct = {}


def direct_clang(clang):
    """The clang that llvm-mingw's <target>-clang wrapper runs (bin/clang-<N>),
    for compiling: on Windows the wrapper chain (<target>-clang, clang,
    clang-<N>) costs about 2 s a run, against 0.1 s for clang itself, and
    the commands here pass -target themselves. Linking keeps the wrapper
    (it adds the runtime and linker flags)."""
    if clang not in _direct:
        real = sorted(glob.glob(os.path.join(os.path.dirname(clang), "clang-[0-9]*.exe"))) if os.name == "nt" else []
        _direct[clang] = real[-1] if real else clang
    return _direct[clang]


def depfile_headers(dep):
    """The headers a compile read, from clang's -MD file; None without one"""
    try:
        with open(dep, encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return None
    body = text.split(": ", 1)[1] if ": " in text else ""
    return [t for t in body.replace("\\\n", " ").split() if t.endswith(".h")]


LINUX_FLAGS = ["-fno-pic", "-fno-pie", "-D_GNU_SOURCE", "-fno-stack-protector"]


def compile_one(clang, src, obj, flags, deps):
    """Compile src to obj when it is older than src, the tools or the
    headers: those the last compile read (its .d file, clang -MD), or every
    header in deps before there is one"""
    dep = obj[:-2] + ".d"
    read = depfile_headers(dep)
    if read is not None:
        deps = [d for d in deps if not d.endswith(".h")] + [os.path.join(ROOT, h) for h in read]
    if not newer(obj, [src] + deps):
        return obj, None
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    clang = direct_clang(clang)
    flags = flags + ["-MD", "-MF", dep] + (LINUX_FLAGS if LINUX else [])
    key = None
    if "-fno-strict-float-cast-overflow" in flags:
        with open(src, "rb") as f:
            key = hashlib.sha1(f.read() + _ee_sig.encode() + " ".join(flags).encode()).hexdigest()
        with _ee_lock:
            if _ee_known.get(src) == [key, False]:
                key = None  # known to have none: straight to the object
    if key is not None:
        # The game's C, through IR (ee_float_casts)
        ll = obj[:-2] + ".ll"
        p = subprocess.run([clang, "-target", TARGET, "-S", "-emit-llvm", src, "-o", ll] + flags,
                           capture_output=True, text=True, cwd=ROOT)
        if p.returncode == 0:
            with open(ll, encoding="utf-8") as f:
                text = ee_float_casts(f.read())
            if text is not None:
                with open(ll, "w", encoding="utf-8") as f:
                    f.write(text)
            opt = next((x for x in flags if re.fullmatch(r"-O\w", x)), "-O1")
            # The passes ran: only code generation is left (the .d file
            # is the first step's, which read the source)
            p = subprocess.run([clang, "-target", TARGET, "-c", ll, "-o", obj, opt, "-Xclang", "-disable-llvm-passes",
                                "-w"], capture_output=True, text=True, cwd=ROOT)
            if p.returncode == 0:
                with _ee_lock:
                    _ee_known[src] = [key, text is not None]
        if os.path.exists(ll):
            os.remove(ll)
    else:
        p = subprocess.run([clang, "-target", TARGET, "-c", src, "-o", obj] + flags,
                           capture_output=True, text=True, cwd=ROOT)
    if p.returncode:
        if os.path.exists(obj):
            os.remove(obj)
        return obj, p.stderr or "failed"
    return obj, None


def defined_names(clang, objs):
    """{name: source} for every global the game's objects define"""
    nm = os.path.join(os.path.dirname(clang), "llvm-nm.exe" if os.name == "nt" else "llvm-nm")
    out = {}
    items = sorted(objs.items())
    for i in range(0, len(items), 400):
        chunk = items[i:i + 400]
        # A response file: 400 absolute paths can exceed Windows' 32K
        # command line
        rsp = os.path.join(OUT, "nm.rsp")
        with open(rsp, "w") as f:
            f.write("\n".join('"' + o.replace("\\", "/") + '"' for _, o in chunk))
        p = subprocess.run([nm, "--defined-only", "-A", "-P", "@" + rsp],
                           capture_output=True, text=True, check=True)
        by_obj = {os.path.normcase(os.path.normpath(o)): rel for rel, o in chunk}
        for line in p.stdout.splitlines():
            # "<object>: <name> <type> <value> <size>"
            m = re.match(r"(.*\.o): (\S+) (\S) ", line)
            if not m or m.group(3) in "U":
                continue
            name = m.group(2)
            name = name[len(syms.PREFIX):] if name.startswith(syms.PREFIX) else name
            out.setdefault(name, by_obj.get(os.path.normcase(os.path.normpath(m.group(1)))))
    return out


GAME_INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)


def ee32_jobs(clang, jobs, nproc, libs=()):
    """On x86-64: the compile jobs for port/tools/ee32.py's copies, where
    every pointer field of a game struct is a 32-bit EE address. The game's
    sources (OUT/gen) and the platform files that include game headers are
    analysed against OUT/hdr and rewritten into OUT/gen64, the headers into
    OUT/hdr64. Stops with every problem the rewrite found."""
    import ee32
    hdr, hdr64 = os.path.join(OUT, "hdr"), os.path.join(OUT, "hdr64")
    gen, gen64 = os.path.join(OUT, "gen"), os.path.join(OUT, "gen64")
    ee64 = os.path.join(ROOT, "port", "include", "grx_ee64.h")
    game_headers = {os.path.relpath(p, os.path.join(ROOT, "include")).replace("\\", "/")
                    for p in glob.glob(os.path.join(ROOT, "include", "**", "*.h"), recursive=True)}
    sig = headers_signature()
    port_sig = hashlib.sha1(b"".join(open(p, "rb").read() for p in
                                     sorted(glob.glob(os.path.join(ROOT, "port", "include", "*.h"))))).hexdigest()
    plan = []  # (src, analysed text path, out path, analysis flags, compile job)
    for src, obj, flags in jobs:
        a = os.path.abspath(src)
        if os.path.normcase(a).startswith(os.path.normcase(gen + os.sep)):
            out = os.path.join(gen64, os.path.relpath(a, gen))
            cflags = [hdr64 if f == hdr else f for f in flags]
            plan.append((a, out, flags, (out, obj, cflags), sig))
            continue
        with open(a, encoding="latin-1") as f:
            incs = GAME_INCLUDE.findall(f.read())
        if not any(i in game_headers for i in incs):
            plan.append((None, None, None, (src, obj, flags), None))
            continue
        # A platform file that reads the game's structs
        rel = os.path.relpath(a, ROOT).replace("\\", "/")
        out = os.path.join(gen64, rel)
        aflags = [hdr if f == "include" else f for f in flags] + ["-include", ee64]
        cflags = [hdr64 if f == "include" else f for f in flags] + ["-include", ee64, "-I", os.path.dirname(a)]
        plan.append((a, out, aflags, (out, obj, cflags), sig + port_sig))
    # The names the game's objects have at PS2 offsets (ee32's extern_array)
    import syms as _syms
    from fixups import objects as _objects
    placed = set(_syms.LinkerNames(ROOT).rel)
    for rel in _objects.BSS_FILES:
        with open(os.path.join(ROOT, rel), encoding="latin-1") as f:
            placed |= {m.group(2) for m in re.finditer(_objects._BSS_DEF, f.read())}
    placed_file = os.path.join(OUT, "ee32_placed.txt")
    os.makedirs(OUT, exist_ok=True)
    with open(placed_file, "w") as f:
        f.write("\n".join(sorted(placed)) + "\n")
    psig = hashlib.sha1(" ".join(sorted(placed)).encode()).hexdigest()
    work = []
    for a, out, aflags, _, key_sig in plan:
        if a is None:
            continue
        with open(a, "rb") as f:
            text = f.read()
        key = ee32.key_of(text, key_sig + psig, aflags)
        cache = os.path.join(OUT, "ee32", hashlib.sha1(a.encode()).hexdigest()[:16] + ".json")
        work.append((direct_clang(clang), TARGET, a, aflags, [gen, hdr], cache, key, placed_file))
    with concurrent.futures.ProcessPoolExecutor(nproc) as ex:
        results = dict(zip((w[2] for w in work), ex.map(ee32.analyse, work, chunksize=8)))
    # A library source clang can't take (the host replaces it): left as it
    # is, its compile fails too and it is left out
    libs = {os.path.normcase(os.path.abspath(l)) for l in libs}
    skipped = {a for a, r in results.items() if r.get("failed") and os.path.normcase(a) in libs}
    results = {a: r for a, r in results.items() if a not in skipped}
    texts = {}

    def text_of(f):
        if f not in texts:
            with open(f, "rb") as fo:
                texts[f] = fo.read()
        return texts[f]
    edits, ctors, errors = ee32.merge(results, text_of)
    if errors:
        errors = sorted(set(errors))
        report = os.path.join(OUT, "ee32_problems.txt")
        with open(report, "w") as f:
            f.write("\n".join(errors) + "\n")
        for e in errors[:40]:
            sys.stderr.write(e + "\n")
        sys.exit(f"ee32: {len(errors)} uses of 32-bit pointer fields need a fixup (port/tools/ee32.py; all in {report})")

    def write(path, data):
        old = None
        if os.path.exists(path):
            with open(path, "rb") as f:
                old = f.read()
        if old != data:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
    # The headers, all of them (the ones without edits as they are)
    for p in generate_headers():
        n = ee32.norm(p)
        write(os.path.join(hdr64, os.path.relpath(p, hdr)), ee32.apply(text_of(n), edits.get(n, [])))
    out_jobs = []
    for a, out, aflags, job, _ in plan:
        if a is None:
            out_jobs.append(job)
            continue
        n = ee32.norm(a)
        data = ee32.apply(text_of(n), edits.get(n, []))
        tag = re.sub(r"\W", "_", os.path.relpath(a, ROOT))
        data += ee32.ctor_text(ctors.get(a, []), tag).encode("latin-1")
        if not os.path.normcase(a).startswith(os.path.normcase(gen + os.sep)):
            rel = os.path.relpath(a, ROOT).replace("\\", "/")
            data = f'#line 1 "{rel}"\n'.encode() + data
        write(out, data)
        out_jobs.append(job)
    return out_jobs


def game_flags(rel, opt):
    # GRX_PORT_CFLAGS adds flags for a debugging build (with --out), e.g.
    # -fstack-protector-all to find a function that overruns its stack
    hdr = os.path.join(OUT, "hdr")
    generate_headers()
    return (["-I", "port/include", "-I", hdr, "-I", "src", "-DGRX_PORT", "-g", f"-O{opt}",
             "-I", os.path.dirname(rel), "-include", "grx_port.h"] + GAME_FLAGS
            + os.environ.get("GRX_PORT_CFLAGS", "").split())


def check(files, opt):
    clang = find_clang()
    nm = os.path.join(os.path.dirname(clang), "llvm-nm.exe" if os.name == "nt" else "llvm-nm")
    tmp = os.path.join(OUT, "check", str(os.getpid()))
    failed = 0
    try:
        for f in files:
            rel = os.path.relpath(os.path.abspath(f), ROOT).replace("\\", "/")
            src = generate(rel, out_dir=os.path.join(tmp, "gen"))
            obj = os.path.join(tmp, rel[:-2] + ".o")
            _, err = compile_one(clang, src, obj, game_flags(rel, opt), [])
            if err:
                failed += 1
                print(f"FAIL {rel}\n{err}")
                continue
            p = subprocess.run([nm, "--defined-only", "-g", "-P", obj], capture_output=True, text=True)
            names = [l.split()[0][len(syms.PREFIX):] for l in p.stdout.splitlines() if l.split()[1] in "TDRB"]
            print(f"OK   {rel}: defines {' '.join(names)}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--target", choices=sorted(TARGETS), default="i686",
                    help="i686 (the default, into build/port) or x86_64 (into build/port64; port/docs/win64.md)")
    ap.add_argument("--list", action="store_true", help="list the sources and whether the host replaces them")
    ap.add_argument("-O", dest="opt", default="1", help="optimisation level (default 1)")
    ap.add_argument("--check", nargs="+", metavar="FILE",
                    help="only compile these game or port/game sources (into a private directory) and "
                         "list the globals each defines; safe to run several at once")
    ap.add_argument("--out", metavar="DIR",
                    help="build into DIR instead of build/port, so several full builds can run at once")
    args = ap.parse_args()
    set_target(args.target)
    if args.out:
        global OUT
        OUT = os.path.abspath(args.out)
    if args.check:
        sys.exit(check(args.check, args.opt))

    sources = game_sources()
    if args.list:
        for rel, b, host in sources:
            print(("host " if host else "port ") + rel)
        return
    game = [(rel, b) for rel, b, host in sources if not host]
    host_libs = [(rel, b) for rel, b, host in sources if host]
    print(f"{len(game)} game sources, {len(host_libs)} replaced by the host C runtime or the platform layer")

    clang = find_clang()
    sdl = find_sdl3()
    sdl_lib = sdl3_delay_lib(clang, sdl)
    tools_deps = [os.path.join(ROOT, "port", "tools", f) for f in ("build.py", "asm2c.py", "syms.py")]
    # The game's sources see the copy of include/ (generate_headers)
    headers = generate_headers() + glob.glob(os.path.join(ROOT, "port", "include", "*.h"))

    # Names the linker defines in the matching build go next to what they
    # are relative to, which takes knowing which object defines what: the
    # placement from the last build is reused, and fixed up after compiling
    names = syms.LinkerNames(ROOT)
    cache = os.path.join(OUT, "aliases.json")
    extras = {}
    if os.path.exists(cache):
        with open(cache) as f:
            extras = json.load(f)

    def game_job(rel):
        gen = generate(rel, extras.get(rel, ""))
        obj = os.path.join(OUT, "obj", rel[:-2] + ".o")
        return gen, obj, game_flags(rel, args.opt)

    # The library sources are compiled too: one is linked when the game
    # needs a name the host doesn't have (VIEWER's vwr_asinf, say)
    jobs = [game_job(rel) for rel, _ in game + host_libs]
    # The platform layer, with its subfolders (port/src/gs/, ...)
    for p in sorted(glob.glob(os.path.join(ROOT, "port", "src", "**", "*.c"), recursive=True)):
        rel = os.path.relpath(p, os.path.join(ROOT, "port", "src"))
        obj = os.path.join(OUT, "obj", "port", rel[:-2] + ".o")
        # The software GS is most of the frame's time: -O2 is ~10% faster
        # (the default -O1 only, so -O0 still debugs it)
        opt = "2" if args.opt == "1" and rel.startswith("gs" + os.sep) else args.opt
        # The game's headers are on the path for the platform files that
        # implement game-named calls (kernel.c includes sdk/eekernel.h)
        jobs.append((p, obj, ["-I", "port/include", "-I", "include", "-I", os.path.join(sdl, "include"),
                              "-I", "port/third_party/xxhash",
                              "-g", f"-O{opt}"] + PORT_FLAGS))

    lib_gen = {os.path.join(OUT, "gen", rel) for rel, _ in host_libs}
    port_jobs = jobs[len(game) + len(host_libs):]
    if args.target in EE64_TARGETS:
        jobs = ee32_jobs(clang, jobs, args.jobs, lib_gen)
    lib_gen |= {os.path.join(OUT, "gen64", rel) for rel, _ in host_libs}
    lib_norm = {os.path.normcase(os.path.normpath(p)) for p in lib_gen}

    def compile_all(jobs):
        """Compile; a library source that fails is only left out (the host
        replaces it), anything else stops the build"""
        errors = []
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
            futs = [(s, ex.submit(compile_one, clang, s, o, f, tools_deps + headers)) for s, o, f in jobs]
            for src, fu in futs:
                obj, err = fu.result()
                if err and os.path.normcase(os.path.normpath(src)) not in lib_norm:
                    errors.append((obj, err))
        if errors:
            for obj, err in errors[:20]:
                sys.stderr.write(err)
            sys.exit(f"{len(errors)} of {len(jobs)} files failed to compile")

    if args.target in EE64_TARGETS:
        with open(os.path.join(ROOT, "port", "tools", "fixups", "bss64.json")) as f:
            if json.load(f).get("headers") != headers_signature():
                sys.exit("port/tools/fixups/bss64.json was measured against other headers (a type, a header "
                         "fixup or grx_ee64.h changed): run python port/tools/gen_bss64.py")
    ee_casts_load(headers)
    try:
        compile_all(jobs)
    finally:
        ee_casts_save()
    print(f"compiled {len(jobs)} files")

    obj_of = {rel: os.path.join(OUT, "obj", rel[:-2] + ".o") for rel, _ in game + host_libs}
    obj_of = {rel: o for rel, o in obj_of.items() if os.path.exists(o)}
    # What the objects define, also counting the aliases they got from the
    # last build; placing aliases looks past those, or each build would
    # drop the aliases the one before placed
    all_defined = defined_names(clang, obj_of)
    placed = syms.LinkerNames.placed_names("".join(extras.values()))
    defined = {k: v for k, v in all_defined.items() if k not in placed}
    new_extras = names.per_file_asm(defined)
    changed = sorted(set(extras) ^ set(new_extras) | {k for k in new_extras if extras.get(k) != new_extras[k]})
    if changed:
        extras = new_extras
        if args.target in EE64_TARGETS:
            # The rewrite merges every source's edits (the headers'): all
            # again, cached but for these; only these are out of date
            again = [game_job(rel) for rel, _ in game + host_libs] + port_jobs
            compile_all(ee32_jobs(clang, again, args.jobs, lib_gen))
        else:
            compile_all([game_job(rel) for rel in changed])
        with open(cache, "w") as f:
            json.dump(extras, f, indent=1)
        print(f"placed linker-defined names in {len(changed)} files")
    defined.update({n: next(k for k, v in extras.items() if n in syms.LinkerNames.placed_names(v))
                    for n in syms.LinkerNames.placed_names("".join(extras.values()))})

    host_objs = {obj_of[rel] for rel, _ in host_libs if rel in obj_of}
    lib_obj_paths = {os.path.join(OUT, "obj", rel[:-2] + ".o") for rel, _ in host_libs}
    objs = [o for _, o, _ in jobs if o not in lib_obj_paths]
    exe = os.path.join(OUT, "grandia" if LINUX else "grandia.exe")
    stubs_c = os.path.join(OUT, "stubs.c")
    names.write_data_c(os.path.join(OUT, "linker_data.c"), defined)
    data_obj = os.path.join(OUT, "obj", "linker_data.o")
    _, err = compile_one(clang, os.path.join(OUT, "linker_data.c"), data_obj, ["-I", "port/include", "-w"], [])
    if err:
        sys.exit(err)
    objs.append(data_obj)

    # The game's own data between two markers (save states, port/src/state.c):
    # the platform layer's objects first, then grx_state_begin, everything
    # of the game's (its sources, the platform's C for its functions, the
    # linker-defined names, library sources, stubs), then grx_state_end.
    # The toolchain's own objects (the CRT, delay-load data) come after.
    # Input sections of one name keep this order in the output (checked
    # against the map after the link: check_state_order).
    port_dir = os.path.normcase(os.path.join(OUT, "obj", "port") + os.sep)
    game_port_dir = os.path.normcase(os.path.join(OUT, "obj", "port", "game") + os.sep)

    def is_platform(o):
        n = os.path.normcase(os.path.abspath(o))
        return n.startswith(port_dir) and not n.startswith(game_port_dir)
    marks = []
    for which in ("begin", "end"):
        src = os.path.join(OUT, f"state_{which}.c")
        with open(src, "w") as f:
            f.write(f"/* Generated by port/tools/build.py: bounds the game's data (port/src/state.c) */\n"
                    f"__attribute__((aligned(16))) char grx_state_data_{which}[16] = {{1}};\n"
                    f"__attribute__((aligned(16))) char grx_state_bss_{which}[16];\n")
        mobj = os.path.join(OUT, "obj", f"state_{which}.o")
        _, err = compile_one(clang, src, mobj, ["-w", "-fno-common"], [])
        if err:
            sys.exit(err)
        marks.append(mobj)
    platform_objs = [o for o in objs if is_platform(o)]
    objs = platform_objs + [marks[0]] + [o for o in objs if not is_platform(o)]

    # Link; whatever is still undefined (functions still in assembly, kernel
    # calls, data only asm defines) gets a stub, then link again
    stub_obj = os.path.join(OUT, "obj", "stubs.o")
    undefined, pulled = set(), set()
    for attempt in range(10):
        syms.write_stubs(stubs_c, undefined, ROOT)
        _, err = compile_one(clang, stubs_c, stub_obj, ["-I", "port/include", "-w"], [])
        if err:
            sys.exit(err)
        rsp = os.path.join(OUT, "link.rsp")
        with open(rsp, "w") as f:
            f.write("\n".join(o.replace("\\", "/") for o in objs + [stub_obj, marks[1]]))
        if LINUX:
            exports = os.path.join(OUT, "exports.map")
            with open(exports, "w") as f:
                f.write("{ local: *; };\n")
            link_cmd = [clang, "-target", TARGET, "-o", exe, "@" + rsp, "-g", "-fuse-ld=lld", "-no-pie",
                        "-Wl,--error-limit=0", "-Wl,-Map," + os.path.join(OUT, "grandia.map"),
                        # Above the PS2's fixed addresses in the low 32 MB,
                        # below 0x08000000, as on Windows; the stacks'
                        # region (fiber_posix.c) is above it
                        "-Wl,--image-base=0x04000000", "-Wl,-z,norelro", "-Wl,-z,noseparate-code",
                        "-Wl,--no-gc-sections",
                        # The game's C defines newlib's syscall stubs and errno
                        # (read, write, close, rand, errno...): none may
                        # preempt glibc's for the shared libraries
                        "-Wl,--version-script=" + exports] + sdl_lib + ["-lm", "-ldl", "-lpthread"]
        else:
            link_cmd = None
        p = subprocess.run(link_cmd or [clang, "-target", TARGET, "-o", exe, "@" + rsp, "-g",
                            "-Wl,--error-limit=0", "-Wl,-Map," + os.path.join(OUT, "grandia.map"),
                            # No ASLR: addresses in crash reports match the
                            # map, and the layout stays the same from run to run
                            "-Wl,--disable-dynamicbase",
                            # Above the PS2's fixed addresses in the low 32 MB
                            # (the IOP block at 0x01FF0000), below 0x08000000,
                            # which the game's address masks keep it under
                            # (port/docs/hardware.md)
                            "-Wl,--image-base=0x04000000",
                            # SDL3 delay-loaded: its DLLs come in after
                            # main() has mapped the PS2's memory
                            sdl_lib, "-Wl,--delayload=SDL3.dll",
                            # Winsock for the freeze tool's server
                            # (port/src/debug.c), delay-loaded the same way
                            "-lws2_32", "-Wl,--delayload=ws2_32.dll", "-ldelayimp"],
                           capture_output=True, text=True, cwd=ROOT)
        if p.returncode == 0:
            break
        missing = {n[len(syms.PREFIX):] if n.startswith(syms.PREFIX) else n
                   for n in re.findall(r"undefined symbol: (\S+)", p.stderr)}
        libs = {obj_of[defined[n]] for n in missing if n in defined} & host_objs
        if libs - pulled:
            pulled |= libs
            objs += sorted(libs)
            continue
        if not missing or missing <= undefined:
            sys.stderr.write(p.stderr[-6000:])
            sys.exit("link failed")
        undefined |= missing
    else:
        sys.exit("link failed")
    problem = check_state_order(os.path.join(OUT, "grandia.map"))
    if problem:
        sys.exit(f"save states: {problem}")
    try:
        if not LINUX:
            shutil.copy2(os.path.join(sdl, "bin", "SDL3.dll"), os.path.join(OUT, "SDL3.dll"))
    except PermissionError:
        # A running game has it open: it is the same file
        if not os.path.exists(os.path.join(OUT, "SDL3.dll")):
            raise
    print(f"linked {os.path.relpath(exe, ROOT)}: {len(pulled)} library sources linked after all, "
          f"{len(undefined)} names stubbed ({os.path.relpath(stubs_c, ROOT)})")


def check_state_order(map_path):
    """None if, in .data and .bss, the input sections between the
    grx_state_begin and grx_state_end markers are all the game's and none
    of the game's is outside them (save states copy what is between);
    else what is wrong"""
    obj_dir = os.path.normcase(os.path.join(OUT, "obj") + os.sep)
    platform = os.path.normcase(os.path.join(OUT, "obj", "port") + os.sep)
    game_port = os.path.normcase(os.path.join(OUT, "obj", "port", "game") + os.sep)
    secs = {".data": [], ".bss": []}
    # lld's COFF map: RVA, size, align; its ELF map: VMA, LMA, size, align
    line_re = re.compile(r"^\s*[0-9a-f]+\s+[0-9a-f]+\s+([0-9a-f]+)\s+\d+\s+(.*):\((\.data|\.bss)\)\s*$" if LINUX
                         else r"^\s*[0-9a-f]+\s+([0-9a-f]+)\s+\d+\s+(.*):\((\.data|\.bss)\)\s*$")
    with open(map_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = line_re.match(line)
            if m and int(m.group(1), 16) > 0:
                secs[m.group(3)].append(os.path.normcase(os.path.abspath(m.group(2))) if
                                        not m.group(2).startswith("<") else m.group(2))
    for sec, paths in secs.items():
        def kind(p):
            if p.endswith(("state_begin.o", "state_end.o")):
                return "mark"
            if not p.startswith(obj_dir):
                return "toolchain"
            if p.startswith(platform) and not p.startswith(game_port):
                return "platform"
            return "game"
        kinds = [kind(p) for p in paths]
        marks = [i for i, k in enumerate(kinds) if k == "mark"]
        if len(marks) != 2:
            return f"{sec}: {len(marks)} markers in the map, not 2"
        a, b = marks
        for i, k in enumerate(kinds):
            inside = a < i < b
            if inside and k != "game":
                return f"{sec}: {paths[i]} ({k}) is among the game's data"
            if not inside and k == "game":
                return f"{sec}: {paths[i]} (the game's) is outside the markers"
    return None


if __name__ == "__main__":
    main()
