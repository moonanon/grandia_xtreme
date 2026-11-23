#!/usr/bin/env python3
"""Build the port in one step, from a fresh checkout and your disc image
(build.cmd at the repository's root runs this).

    python port/tools/build_all.py [--disc IMAGE] [--32 | --both] [--yes]

1. The game's four code files (SLUS_204.17, FIELD.BIN, BATTLE.BIN,
   VIEWER.BIN) are copied out of the disc image into iso/ and checked by
   SHA-1. The image is --disc, GRX_DISC, the only image in disc/, or asked
   for. (An .iso, or an .mds with its .mdf.)
2. llvm-mingw (clang for Windows) and SDL3 are found (GRX_CLANG, PATH, the
   winget install; GRX_SDL3, port/deps/), or else downloaded into
   port/deps/, a pinned release each, checked by SHA-256. Nothing is
   installed: delete port/deps/ to remove them.
3. port/tools/build.py builds the 64-bit grandia.exe (build/port64/), or
   with --32 the 32-bit one (build/port/), or --both. What it would take
   from the decompilation's own build (which needs Linux) comes from
   port/inputs/ and the game's files in iso/.
4. play.cmd at the root starts the game with the disc image.

Run it again after pulling changes: it only redoes what is out of date.
"""

import glob
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import urllib.request
import zipfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DEPS = os.path.join(ROOT, "port", "deps")

GAME_FILES = {
    "SLUS_204.17": (712448, "3fd574c7a1e0431c6f437851f0a8ea5fcad61b71"),
    "FIELD.BIN": (849478, "672830cc6c285b666d88387548be51487701cf5a"),
    "BATTLE.BIN": (496464, "bff65a26ebbe7adab74340a4fcb08f3cb196c65a"),
    "VIEWER.BIN": (57156, "ac36fdc130674229a2a27665cbcfc46e40713e98"),
}

LLVM_MINGW = {
    "name": "llvm-mingw-20261020-ucrt-x86_64",
    "url": "https://github.com/mstorsjo/llvm-mingw/releases/download/20261020/"
           "llvm-mingw-20261020-ucrt-x86_64.zip",
    "size": 190612575,
    "sha256": "fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd",
}
SDL3 = {
    "name": "SDL3-3.4.18",
    "url": "https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-devel-3.4.18-mingw.zip",
    "size": 5472877,
    "sha256": "cd98158c8d025a816f430a600b4e9526524e6219ee884ec72bd498b80fd3bf58",
}

TRIPLES = {"x86_64": "x86_64-w64-mingw32", "i686": "i686-w64-mingw32"}

ASSUME_YES = False


def say(text):
    print(text, flush=True)


def fail(text):
    say("")
    say("Stopped: " + text)
    sys.exit(1)


def ask(question):
    if ASSUME_YES:
        return True
    try:
        return input(question + " [Y/n] ").strip().lstrip("﻿").lower() in ("", "y", "yes")
    except EOFError:
        return False


# --- 1. The game's code files ---------------------------------------------------

def sha1_of(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def game_files_ok():
    for name, (size, sha1) in GAME_FILES.items():
        path = os.path.join(ROOT, "iso", name)
        if not os.path.isfile(path) or os.path.getsize(path) != size or sha1_of(path) != sha1:
            return False
    return True


class Image:
    """User data of an image's 2048-byte sectors: an .iso, or an .mdf
    (named by its .mds) of plain or raw sectors"""

    LAYOUTS = [(2048, 0), (2352, 16), (2352, 24)]

    def __init__(self, path):
        if path.lower().endswith(".mds"):
            stem = path[:-4]
            mdf = next((stem + e for e in (".mdf", ".MDF", ".Mdf") if os.path.exists(stem + e)), None)
            if mdf is None:
                fail(f"{path}: its .mdf isn't next to it")
            path = mdf
        self.f = open(path, "rb")
        for self.sector, self.skip in self.LAYOUTS:
            if self.read(16, 1)[1:6] == b"CD001":
                return
        fail(f"{path}: not an ISO 9660 image (a PS2 DVD image, .iso or .mds/.mdf)")

    def read(self, lba, count):
        out = bytearray()
        for i in range(count):
            self.f.seek((lba + i) * self.sector + self.skip)
            out += self.f.read(2048)
        return bytes(out)

    def root_files(self):
        """{NAME: (lba, size)} of the root directory"""
        pvd = self.read(16, 1)
        lba, size = struct.unpack_from("<I", pvd, 158)[0], struct.unpack_from("<I", pvd, 166)[0]
        data = self.read(lba, (size + 2047) // 2048)
        out, pos = {}, 0
        while pos < len(data):
            n = data[pos]
            if n == 0:
                pos = (pos // 2048 + 1) * 2048  # records don't cross sectors
                continue
            rec = data[pos:pos + n]
            name = rec[33:33 + rec[32]].decode("latin-1").split(";")[0]
            if not rec[25] & 2:
                out[name.upper()] = struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
            pos += n
        return out


def known_disc(given):
    """--disc, GRX_DISC, or the only image in disc/"""
    if given:
        return given
    if os.environ.get("GRX_DISC"):
        return os.environ["GRX_DISC"]
    # An .mds without its .mdf is only a descriptor
    found = [p for p in glob.glob(os.path.join(ROOT, "disc", "*"))
             if p.lower().endswith(".iso")
             or p.lower().endswith(".mds") and glob.glob(glob.escape(p[:-4]) + ".[mM][dD][fF]")]
    return found[0] if len(found) == 1 else None


def ask_disc():
    if ASSUME_YES:
        return None
    say("Your Grandia Xtreme disc image (.iso, or .mds with its .mdf).")
    try:
        # (a byte order mark can come with pasted or piped text)
        path = input("Its path (you can drag the file onto this window): ").strip().lstrip("﻿").strip('"')
    except EOFError:
        return None
    return path or None


def extract_game_files(disc):
    if disc is None:
        fail("no disc image: give one with --disc, GRX_DISC, or put it in disc\\")
    if not os.path.exists(disc):
        fail(f"{disc} not found")
    say(f"Copying the game's code files out of {disc}")
    img = Image(disc)
    files = img.root_files()
    # All four checked before any is written
    got = {}
    for name, (size, sha1) in GAME_FILES.items():
        if name not in files:
            fail(f"{name} isn't on this disc: is it Grandia Xtreme (US, SLUS-20417)?")
        lba, n = files[name]
        data = img.read(lba, (n + 2047) // 2048)[:n]
        if n != size or hashlib.sha1(data).hexdigest() != sha1:
            fail(f"{name} on this disc isn't the US release's (SLUS-20417), which the port is made from")
        got[name] = data
    os.makedirs(os.path.join(ROOT, "iso"), exist_ok=True)
    for name, data in got.items():
        with open(os.path.join(ROOT, "iso", name), "wb") as f:
            f.write(data)
        say(f"  iso\\{name}")


# --- 2. Tools -------------------------------------------------------------------

def find_clang(arch):
    exe = TRIPLES[arch] + "-clang.exe"
    c = os.environ.get("GRX_CLANG")
    if c:
        return c
    c = shutil.which(TRIPLES[arch] + "-clang")
    if c:
        return c
    pkgs = os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\WinGet\Packages")
    found = sorted(glob.glob(os.path.join(pkgs, "MartinStorsjo.LLVM-MinGW*", "llvm-mingw*", "bin", exe)))
    found += sorted(glob.glob(os.path.join(DEPS, "llvm-mingw-*", "bin", exe)))
    return found[-1] if found else None


def find_sdl3():
    base = os.environ.get("GRX_SDL3")
    if base:
        return base
    found = sorted(p for p in glob.glob(os.path.join(DEPS, "SDL3-*")) if os.path.isdir(p))
    return found[-1] if found else None


def download(pkg, what):
    mb = pkg["size"] / 1e6
    if not ask(f"{what} isn't here. Download {pkg['name']} ({mb:.0f} MB) from GitHub into port\\deps?"):
        fail(f"{what} is needed (README.md, Building)")
    os.makedirs(DEPS, exist_ok=True)
    zpath = os.path.join(DEPS, os.path.basename(pkg["url"]))
    h = hashlib.sha256()
    say(f"Downloading {pkg['url']}")
    try:
        with urllib.request.urlopen(pkg["url"], timeout=60) as r, open(zpath + ".part", "wb") as f:
            got = 0
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
                h.update(chunk)
                got += len(chunk)
                print(f"\r  {got / 1e6:.0f} of {mb:.0f} MB", end="", flush=True)
    except (OSError, ValueError) as e:  # URLError and socket errors are OSErrors
        print()
        if os.path.exists(zpath + ".part"):
            os.remove(zpath + ".part")
        fail(f"couldn't download {pkg['url']} ({e}): check the connection and run build.cmd again")
    print()
    if h.hexdigest() != pkg["sha256"]:
        os.remove(zpath + ".part")
        fail(f"{os.path.basename(zpath)} didn't match its SHA-256 (a broken download?): run build.cmd again")
    os.replace(zpath + ".part", zpath)
    say(f"Unpacking {os.path.basename(zpath)}")
    with zipfile.ZipFile(zpath) as z:
        z.extractall(DEPS)
    os.remove(zpath)


def tools(arches):
    env = {}
    for arch in arches:
        if not find_clang(arch):
            download(LLVM_MINGW, "llvm-mingw (clang for Windows)")
        c = find_clang(arch)
        if not c:
            fail(f"clang for {TRIPLES[arch]} not found after unpacking llvm-mingw")
    # build.py finds the target's clang next to GRX_CLANG
    env["GRX_CLANG"] = find_clang(arches[0])
    if not find_sdl3():
        download(SDL3, "SDL3")
    sdl = find_sdl3()
    for arch in arches:
        if not os.path.exists(os.path.join(sdl, TRIPLES[arch], "include", "SDL3", "SDL.h")):
            fail(f"{sdl} has no {TRIPLES[arch]} tree: is it SDL3's mingw development release?")
    env["GRX_SDL3"] = sdl
    say(f"clang: {env['GRX_CLANG']}")
    say(f"SDL3:  {sdl}")
    return env


# --- 3. The build ---------------------------------------------------------------

def build(arches, env):
    for arch in arches:
        args = [sys.executable, os.path.join(ROOT, "port", "tools", "build.py")]
        if arch == "x86_64":
            args += ["--target", "x86_64"]
        say("")
        say(f"Building the {'64' if arch == 'x86_64' else '32'}-bit grandia.exe (the first build takes "
            "a while: it compiles about 4,600 files)")
        r = subprocess.run(args, cwd=ROOT, env={**os.environ, **env})
        if r.returncode != 0:
            fail("the build failed (above)")


def write_play(arch, disc):
    """play.cmd: the exe with the disc image (the last one's, when this run
    wasn't given one)"""
    path = os.path.join(ROOT, "play.cmd")
    if not disc and os.path.exists(path):
        with open(path, encoding="utf-8", errors="replace") as f:
            m = re.search(r'--disc "([^"]+)"', f.read())
        disc = m.group(1) if m else None
    exe = os.path.join("build", "port64" if arch == "x86_64" else "port", "grandia.exe")
    lines = ["@echo off", f'"%~dp0{exe}" --boot']
    if disc:
        lines[-1] += f' --disc "{os.path.abspath(disc)}"'
    lines[-1] += " %*"
    with open(path, "w", newline="\r\n") as f:
        f.write("\n".join(lines) + "\n")
    return exe, disc


def main():
    global ASSUME_YES
    args = sys.argv[1:]
    disc = None
    if "--disc" in args:
        i = args.index("--disc")
        if i + 1 >= len(args):
            fail("--disc needs the image's path")
        disc = args[i + 1]
        del args[i:i + 2]
    ASSUME_YES = "--yes" in args
    arches = ["x86_64", "i686"] if "--both" in args else ["i686"] if "--32" in args else ["x86_64"]
    unknown = [a for a in args if a not in ("--yes", "--both", "--32")]
    if unknown:
        fail("unknown option " + " ".join(unknown) + " (see port/tools/build_all.py)")
    if sys.platform != "win32":
        fail("the port builds on Windows for now")

    disc = known_disc(disc)
    if game_files_ok():
        say("The game's code files are in iso\\ already")
    else:
        disc = disc or ask_disc()
        extract_game_files(disc)
    env = tools(arches)
    build(arches, env)
    exe, disc = write_play(arches[0], disc)
    say("")
    say(f"Done: {exe}")
    if disc:
        say("Start the game with play.cmd (it passes on any options, such as --fullscreen).")
    else:
        say(f"Start it with: {exe} --boot --disc <your disc image>")


if __name__ == "__main__":
    main()
