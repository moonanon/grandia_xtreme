# Grandia Xtreme: decompilation and Windows port

A decompilation of **Grandia Xtreme (USA)** for the PlayStation 2
(`SLUS_204.17`, disc version 1.01), and a native Windows port built from
it.

- **The decompilation** (`src/`, `include/`, `config/`, `tools/`): the
  game's code as C and assembly that build back to its four code files
  byte for byte. About 92% of the code is in C.
- **The port** (`port/`): that C compiled for Windows with clang, and a
  platform layer standing in for the PS2 (kernel, GS graphics in OpenGL,
  VU, sound, movies, memory cards, controllers). 32-bit and 64-bit builds.

![The attract demo's battle in the 64-bit port, at 16:9](screenshots/attract-demo.jpg)

*The title screen's attract demo in the port, widescreen.*

**No game data is included**, and none may be added. You need your own
copy of the game: its disc image to play, and four code files from it to
build.

## What the port adds

- Widescreen: the 3D fills any window from 4:3 to 32:9, the HUD moves to
  the sides.
- An OpenGL renderer at a multiple of the PS2's resolution: it fills the
  window, or set it on the Display page (640x448, 720p, 1080p, 1440p, 4K),
  with the frame rate (60 or 30, locked to your display's refresh) and the
  aspect ratio.
- A longer draw distance (fog pushed back, up to 8 times the original).
- Xbox controllers with Xbox button icons in the game, or a keyboard; any
  button can be remapped from the game's Configurations page.
- The field camera on the right stick: turn it, and tilt it up and down.
- Skip cut scenes: hold START for 5 seconds.
- Save states: Shift+F1 to F4 save, F1 to F4 load.
- Fast forward: Tab cycles 1x, 2x and 4x (shown as ">> 2" / ">> 4");
  music and voices keep their normal speed. Off during movies and scene
  skips. `GRX_TURBO=2` or `4` starts at that speed.

## Building

The port builds on Windows. The decompilation's build, which the port
needs once, runs in Docker from a Linux or macOS shell: on Windows, WSL.

### What you need

- Windows 10 or 11, with [Python 3](https://www.python.org/downloads/).
- [WSL](https://learn.microsoft.com/windows/wsl/install) (Ubuntu) with
  `python3`, `python3-venv` and `curl`, and
  [Docker Desktop](https://www.docker.com/products/docker-desktop/) with its
  WSL integration on.
- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw), the clang toolchain
  for Windows:

  ```sh
  winget install MartinStorsjo.LLVM-MinGW.UCRT
  ```

- [SDL3](https://github.com/libsdl-org/SDL/releases): the
  `SDL3-devel-<version>-mingw.zip` release, unpacked into `port/deps/`, so
  that `port/deps/SDL3-<version>/i686-w64-mingw32/` and
  `x86_64-w64-mingw32/` exist. (Or set `GRX_SDL3` to where it is.)
- Your copy of the game, as a disc image (`.iso`, or `.mds` with its
  `.mdf`).

### 1. The game's code files

Copy these four files from the root of your disc (next to `SYSTEM.CNF`)
into `iso/`. Any tool that reads ISO 9660 images can take them out of an
image, or mount the image in Windows.

| File | Size | SHA-1 |
| --- | ---: | --- |
| `iso/SLUS_204.17` | 712,448 | `3fd574c7a1e0431c6f437851f0a8ea5fcad61b71` |
| `iso/FIELD.BIN` | 849,478 | `672830cc6c285b666d88387548be51487701cf5a` |
| `iso/BATTLE.BIN` | 496,464 | `bff65a26ebbe7adab74340a4fcb08f3cb196c65a` |
| `iso/VIEWER.BIN` | 57,156 | `ac36fdc130674229a2a27665cbcfc46e40713e98` |

`iso/`, `disc/`, disc images and everything the build makes are in
`.gitignore`.

### 2. The decompilation's build

The port needs it once: it reads the assembly for the functions not yet in
C and the addresses the matching build's linker gives. In WSL, in the
repository's folder (`/mnt/c/...`):

```sh
python3 -m venv .venv
.venv/bin/pip install "splat64[mips]==0.50.1"
tools/split.sh
tools/get_compilers.sh
tools/docker.sh make -j8
```

- `tools/split.sh` splits the four files into assembly (`asm/`).
- `tools/get_compilers.sh` downloads Sony's EE GCC builds from decomp.me's
  compiler archive into `tools/compilers/`.
- `tools/docker.sh` builds a Linux image with the MIPS tools on first use,
  then runs the build in it. `make` rebuilds the four files and checks
  them against yours:

  ```
  OK       build/SLUS_204.17.rom
  OK       build/field.bin
  OK       build/battle.bin
  OK       build/viewer.bin
  ```

### 3. The port

In a Windows terminal (PowerShell or Command Prompt), in the repository's
folder:

```sh
python port/tools/build.py --target x86_64   # 64-bit: build/port64/grandia.exe
python port/tools/build.py                   # 32-bit: build/port/grandia.exe
```

A first build compiles about 4,600 files and takes a while; later builds
compile only what changed. Each copies `SDL3.dll` next to its
`grandia.exe`.

## Running

The port reads everything but the code (pictures, sound, movies, maps)
from your disc image while it runs. Put the image somewhere (`disc/` is a
good place: it's ignored), then, in PowerShell or Command Prompt:

```powershell
.\build\port64\grandia.exe --boot --disc "disc\Grandia Xtreme.ISO"
```

`--boot` is needed for now. Instead of `--disc`, you can set the
`GRX_DISC` environment variable to the image's path.

**Memory cards** are folders: `%APPDATA%\grandia_extreme\memcard\card1\`
(slot 1) and `card2\`. Set `GRX_MC_DIR` to keep them somewhere else.

**Settings** for the controls and the display (resolution, frame rate,
aspect ratio) are saved in `grandia.ini` next to `grandia.exe`. Change them
on the game's **Configurations** page (on the title screen or in the field
menu): its Controller, Keyboard and Display rows.

### Options

| Option | |
| --- | --- |
| `--fullscreen` | start fullscreen (Alt+Enter, F11 or a double click switches) |
| `--scale N` | window size: 480 lines times N (default 2) |
| `--aspect A` | `auto` (the default: follows the window), `4:3` (the original), `16:9`, or `W:H` |
| `--hud H` | how wide the HUD spreads to the sides: `21:9` (default), `16:9`, `4:3`, or `none` |
| `--draw-distance N` | 1 (the original) to 8; default 4 |
| `--renderer R` | `gl` (the GPU, default) or `software` |
| `--buttons B` | `auto` (by the controller), `xbox` or `playstation` |
| `--camera C` | `stick` (default: the right stick turns and tilts the camera) or `buttons` (L1 and R1 only, as the original) |
| `--scene-skip S` | `on` (default: hold START 5 seconds to skip a cut scene) or `off` |
| `--integer`, `--sharp`, `--smooth` | scale by whole multiples; nearest or linear filtering |

`grandia.exe` with no options lists them all.

### Controls

A gamepad works as the PS2's controller, with an Xbox controller's A on
confirm (circle). On the keyboard:

| Does | PS2 button | Key |
| --- | --- | --- |
| Confirm | Circle | X |
| Cancel, ready the party | Cross | Z |
| Map zoom | Square | A |
| Menu | Triangle | S |
| Turn the camera | L1, R1 | Q, W |
| Ready the party | L2 | E |
| Reset the camera | R2 | R |
| Start, skip a scene | Start | Enter |
| First-person view | Select | Backspace |
| Move | D-pad | Arrows |

To change them, open **Configurations** (on the title screen or in the
field menu): the **Controller** and **Keyboard** rows list every button.

Keys of the port's own, which can't be remapped:

| Key | |
| --- | --- |
| F1 to F4 | load save state 1 to 4 |
| Shift+F1 to Shift+F4 | save state 1 to 4 (saved in `savestates\` next to `grandia.exe`; a state only loads in the build that made it) |
| F11, Alt+Enter | window or fullscreen |
| F9 | freeze the game and write a dump to `build/freeze/` (F9 again resumes) |
| F10 | while frozen, run one field |

## Linux (native)

```sh
python3 port/tools/build.py --target linux64 -j$(nproc)   # build/portlinux/grandia
./grandia-xtreme      # boots the first .iso in iso/ or disc/; or pass --disc PATH
```

`./grandia-xtreme` runs `build/portlinux/grandia` from the repository root, where it
finds `textures/`.

Needs clang, lld and SDL3 (for example Homebrew's `llvm`, `lld` and `sdl3`, or your
distribution's), after the decompilation's build above has run once. Run it from a
Wayland or X11 session with OpenGL 3.3.

## HD texture packs

PCSX2 texture replacement packs (DDS files named `<hash>-<hash>-<bits>.dds`)
are drawn in place of the game's textures, e.g. the HD pack and the widescreen
UI pack by Panda_Venom and 1vierock. Put each pack's `replacements/` folder (or
a symlink to it) in `textures/`; the folders are read in name order and, for a
texture in several, the first folder's file wins:

    textures/01-widescreen/   # Grnd_X_Widescreen_replacements.7z
    textures/02-hd/           # .../PCSX2/textures/SLUS-20417/replacements

`GRX_TEXTURES=dir1:dir2` (`;` on Windows) lists folders instead. The packs'
files are not included (`textures/` is git-ignored).

## Layout

| Path | What |
| --- | --- |
| `config/` | splat configs, symbol names, the memory map |
| `src/`, `include/` | the decompiled C and its headers |
| `tools/` | the decompilation's build and checking scripts |
| `port/tools/build.py` | the port's build |
| `port/tools/fixups/` | changes the port makes to the game's C as it builds it (the game's sources aren't edited) |
| `port/game/` | C for functions the decompilation still has only in assembly |
| `port/src/` | the platform layer: kernel, GS and its OpenGL renderer, VU, sound, movies, memory cards, input |
| `port/tests/` | tests for the platform layer's pieces |
| `port/tools/grxdbg.py` | reads and writes a frozen game's memory by C name (F9) |

## Third-party library code

The game links library code next to its own: libgcc, newlib (with its
fdlibm maths) and Sony's PS2 SDK libraries. Each C file holding library
code says which library at its top. Where a library is open source, the
matching C follows the upstream source and falls under that library's
license. [THIRD_PARTY.md](THIRD_PARTY.md) lists the files and quotes the
notices. SDL3, which the port uses, is under the zlib license.
