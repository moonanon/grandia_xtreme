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
- Skip cut scenes: hold START for 1.5 seconds.
- Save states: Shift+F1 to F4 save, F1 to F4 load.
- Fast forward: Tab cycles 1x, 2x and 4x (shown as ">> 2" / ">> 4");
  music and voices keep their normal speed. Off during movies and scene
  skips. `GRX_TURBO=2` or `4` starts at that speed.
- Optional mods built from your own disc: more Geo Gates, a map overlay
  that spreads the minimap over the whole field, and entrances on every
  map's minimap (see [Included mods](#included-mods)).
- An Esc menu for the port's settings and its own buttons, and walking on
  the keyboard.
- F8 records where you stand (map, position, facing) to `spots.txt`, for
  picking places to add things.

## Building

The port builds on Windows 10 or 11. There are two ways: `build.cmd`,
which needs only Python, and the step-by-step build with the
decompilation's own build, for working on the decompilation.

### With build.cmd

You need [Python 3](https://www.python.org/downloads/) and your copy of
the game as a disc image. Double-click `build.cmd` in the repository's
folder, or run it in PowerShell or Command Prompt:

```powershell
.\build.cmd --disc "D:\Games\Grandia Xtreme.iso"
```

It does four things:

1. **The game's code files.** It copies `SLUS_204.17`, `FIELD.BIN`,
   `BATTLE.BIN` and `VIEWER.BIN` out of the image into `iso\` and checks
   each against the US release's SHA-1 (the table below). The image is
   the one given with `--disc`, else `GRX_DISC`, else the only image in
   `disc\`; otherwise it asks for it (you can drag the file onto the
   window). It reads `.iso` images and `.mds` ones with their `.mdf`,
   with plain or raw CD sectors. Nothing is written to `iso\` unless all
   four files check out.
2. **The tools.** It looks for [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)
   (the clang compiler: `GRX_CLANG`, `PATH`, a winget install, or
   `port\deps\`) and [SDL3](https://github.com/libsdl-org/SDL) (`GRX_SDL3`
   or `port\deps\`). Each one that is missing it offers to download from
   GitHub: llvm-mingw 20261020 (about 190 MB) and SDL3 3.4.18 (about
   5 MB), each checked against its SHA-256 and unpacked into
   `port\deps\`. Nothing is installed; deleting `port\deps\` removes them.
3. **The build.** It runs `port\tools\build.py`: the 64-bit
   `build\port64\grandia.exe` by default, `--32` for the 32-bit
   `build\port\grandia.exe`, `--both` for both. The first build compiles
   about 4,600 files and takes a while (about 10 to 20 minutes); later
   ones compile only what changed.
4. **play.cmd.** It writes `play.cmd` in the repository's folder, which
   starts the game with your image (see Running). A later run without a
   disc keeps the one `play.cmd` already has.

Run `build.cmd` again after pulling changes. `--yes` answers its
questions with yes, for scripts.

Nothing of the game's is in the repository: what the build needs from the
game, it takes from your image. What it would otherwise need from the
decompilation's build (names, addresses and sizes only) is kept in
`port/inputs/`.

If it stops, it says why. The usual reasons:

- *isn't the US release's*: the port is made from Grandia Xtreme (USA),
  `SLUS-20417`; other releases' files differ.
- *not an ISO 9660 image*, or *its .mdf isn't next to it*: give the
  `.iso`, or the `.mds` with its `.mdf` beside it.
- *couldn't download*: check the connection and run it again.

### Step by step, with the decompilation's build

This is the way to build when you work on the decompilation itself, and
what `build.cmd` does by hand. The decompilation's build runs in Docker
from a Linux or macOS shell: on Windows, WSL. When its output (`build/`,
`asm/`) is there, the port's build uses it instead of `port/inputs/`.

#### What you need

- [Python 3](https://www.python.org/downloads/).
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

#### 1. The game's code files

Copy these four files from the root of your disc (next to `SYSTEM.CNF`)
into `iso/`. Any tool that reads ISO 9660 images can take them out of an
image, or mount the image in Windows. (`build.cmd` does this step on its
own.)

| File | Size | SHA-1 |
| --- | ---: | --- |
| `iso/SLUS_204.17` | 712,448 | `3fd574c7a1e0431c6f437851f0a8ea5fcad61b71` |
| `iso/FIELD.BIN` | 849,478 | `672830cc6c285b666d88387548be51487701cf5a` |
| `iso/BATTLE.BIN` | 496,464 | `bff65a26ebbe7adab74340a4fcb08f3cb196c65a` |
| `iso/VIEWER.BIN` | 57,156 | `ac36fdc130674229a2a27665cbcfc46e40713e98` |

`iso/`, `disc/`, disc images, `port/deps/`, `play.cmd` and everything the
build makes are in `.gitignore`.

#### 2. The decompilation's build

In WSL, in the repository's folder (`/mnt/c/...`):

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

After a change to `src/` that renames or moves anything, run
`python port/tools/port_inputs.py` (in Windows) to bring `port/inputs/`
up to date for `build.cmd`; `--check` says whether it is.

#### 3. The port

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
from your disc image while it runs. After `build.cmd`, double-click
`play.cmd`, or run it with options:

```powershell
.\play.cmd --fullscreen
```

Or start `grandia.exe` yourself, in PowerShell or Command Prompt:

```powershell
.\build\port64\grandia.exe --boot --disc "disc\Grandia Xtreme.ISO"
```

`--boot` starts the game (without it, `grandia.exe` lists its options).
Instead of `--disc`, you can set the `GRX_DISC` environment variable to the
image's path. `disc\` is a good place for the image: it's ignored.

**Memory cards** are folders: `%APPDATA%\grandia_extreme\memcard\card1\`
(slot 1) and `card2\` (slot 2, empty unless `GRX_MC2` is `formatted`),
the same for both builds and wherever `grandia.exe` is. Set `GRX_MC_DIR`
to keep them somewhere else.

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
| `--scene-skip S` | `on` (default: hold START 1.5 seconds to skip a cut scene) or `off` |
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

The port's own actions, on the same pages:

| Does | Gamepad | Key |
| --- | --- | --- |
| Fast forward | L3 | Tab |
| Map overlay | R3 | M |

To change them, open **Configurations** (on the title screen or in the
field menu): the **Controller** and **Keyboard** rows list every button
and the port's actions. The Esc menu changes them too.

Keys of the port's own, which can't be remapped:

| Key | |
| --- | --- |
| Esc | the port's menu |
| F8 | record your location to `spots.txt` |
| F1 to F4 | load save state 1 to 4 |
| Shift+F1 to Shift+F4 | save state 1 to 4 (saved in `savestates\` next to `grandia.exe`; a state only loads in the build that made it) |
| F11, Alt+Enter | window or fullscreen |
| F9 | freeze the game and write a dump to `build/freeze/` (F9 again resumes) |
| F10 | while frozen, run one field |

## Debug builds

`build.py --debug` (with `--target`, as usual) builds the debug kit into
`build/portlinux-debug/` (or `port-debug`, `port64-debug`); release builds
leave it out. Its keys are listed in the top right corner (F12 hides them):

| Key | |
| --- | --- |
| F5 | super mode: the party can't die, free MP and SP; L2+L1 in the command menu wins the battle |
| F6, Shift+F6 | warp to the next or previous map |
| Ctrl+F6, Ctrl+Shift+F6 | warp to the next or previous Geo Gate |
| F7 | noclip (PgUp/PgDn rise and sink, Home/End move through walls) |
| Shift+F7 | reveal the whole minimap |

## Included mods

Mods change what is in the game or how it behaves, so they are optional:
the port applies them from `mods/` in the working directory, and without
it the game is as it was. They are built from your own disc, so nothing of
the game's is in the repository:

```sh
python3 port/tools/mods.py "iso/Grandia Xtreme (USA).iso"   # builds them all into mods/
python3 port/tools/mods.py --list                           # lists them
python3 port/tools/mods.py ISO --without minimap-entrances  # all but one
```

The extra Geo Gates go in the early dungeons, where gates are few and far
between: there, the walk back to Locca and in again is at its most tedious
while you are still getting to know your party. They make those dungeons
easier to come and go from, as the later ones already are.

| Mod | What it does | Files in `mods/` |
| --- | --- | --- |
| `land-ruins-2-geo-gate` | a new Geo Gate on Land Ruins 2, which has none | `mdt/4040.mdp`-`4043.mdp` |
| `land-ruins-3-geo-gate` | Land Ruins 3's exit-only Geo Gate made two-way | `mdt/4050.mdp`-`4053.mdp` |
| `aqua-ruins-geo-gate` | the Energy Conversion area's exit-only Geo Gate made two-way | `mdt/50F0.mdp`-`50F3.mdp` |
| `gale-ruins-3-geo-gate` | a new Geo Gate deeper in Gale Ruins (map 0x5830) | `mdt/5830.mdp`-`5833.mdp` |
| `map-overlay` | M (or R3) spreads the minimap flat over the whole field, as Final Fantasy XII's, while you keep walking | none: a line in `mods.txt` |
| `minimap-entrances` | the minimap shows every map's entrances, and Geo Gates in cyan (the game shows them on the corridor floors only) | none: a line in `mods.txt` |

With any Geo Gate mod, `mdt/2000.mdp` and `E400.mdp` (Locca's gate and its
later-story map) jump to the new gates, and `flaglist.txt` names them in
Locca's list. `mods.txt` lists the mods built.

Each `.mdp` is named after the map file it patches (four files for a map's
story variants). A new gate is activated as the game's are: touch it once,
then again; it then shows in Locca's list. Exit-only gates made two-way get
a normal gate's arrival, look and first message. `GATES` in
`port/tools/mods.py` is where to add more.

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
| `build.cmd`, `port/tools/build_all.py` | the port's build in one step, from your disc image |
| `port/tools/build.py` | the port's build |
| `port/inputs/` | what the port's build takes from the decompilation's build, so it builds without it |
| `port/tools/fixups/` | changes the port makes to the game's C as it builds it (the game's sources aren't edited) |
| `port/game/` | C for functions the decompilation still has only in assembly |
| `port/src/` | the platform layer: kernel, GS and its OpenGL renderer, VU, sound, movies, memory cards, input |
| `port/tests/` | tests for the platform layer's pieces |
| `port/tools/grxdbg.py` | reads and writes a frozen game's memory by C name (F9) |
| `port/tools/mods.py`, `port/tools/mdt.py` | the Geo Gate mods' map patches; reads and disassembles map files |

## Third-party library code

The game links library code next to its own: libgcc, newlib (with its
fdlibm maths) and Sony's PS2 SDK libraries. Each C file holding library
code says which library at its top. Where a library is open source, the
matching C follows the upstream source and falls under that library's
license. [THIRD_PARTY.md](THIRD_PARTY.md) lists the files and quotes the
notices. It also covers the port: its MPEG-2 decoder's IDCT, which comes
from MSSG `mpeg2decode`, and the libraries `build.cmd` downloads, SDL3
(zlib license) and llvm-mingw.
