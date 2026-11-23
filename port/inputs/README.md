# What the port takes from the decompilation's build

So that the port builds on Windows alone (`build.cmd`), without the
decompilation's own build (which needs Linux), these hold what
`port/tools/build.py` would otherwise read from it. They hold names,
addresses, offsets and sizes only, nothing of the game's:

| File | What |
| --- | --- |
| `c_syms*.ld` | the names the matching build's linker defines (`tools/c_syms.py`) |
| `asm_labels.json` | the labels still in assembly, with their kind and size, for the port's stubs |
| `rodata.json` | where each C file's constants (`.rodata`) are in the game's own files, for the few the port copies (`port/tools/syms.py` reads their bytes from `iso/`) |

`python port/tools/port_inputs.py` writes them from a matching build
(`build/`, `asm/`, `iso/`); `--check` says whether they still match it.
Run it after a change to `src/` that moves or renames anything.
`build.py` uses `build/` and `asm/` when they are there, and these
otherwise (`GRX_PORT_INPUTS=1`: these always).
