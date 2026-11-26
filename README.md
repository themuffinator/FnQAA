# openQAA

Run **Quake Arena Arcade** (Xbox Live Arcade, 2010) natively on the PC, from
your own unmodified copy of the game.

openQAA uses the [FnQL](https://github.com/themuffinator/FnQL) engine. QAA's
own game code, its `qagameXe.dll` and `cgameXe.dll` modules, is statically
recompiled from your package into native code on your machine. No game data
or game code is included in this repository.

> **Status: M2, the engine.** The FnQL engine is imported and builds as
> `openqaa` and `openqaa-ded`. It mounts the game's data straight from your
> XBLA package, without extracting anything. The game modules (M3 to M5) and
> the menus (M6) are not there yet, and pak0.bpk's file names wait on the
> name hash (M1), so the game does not play yet. See [the plan](docs/PLAN.md).

## Build

Meson and Ninja, with Visual Studio 2022 on Windows or GCC on Linux. On
Windows, from the repository root:

```powershell
pwsh scripts/msvc-env.ps1 meson setup build/msvc --buildtype=release
pwsh scripts/msvc-env.ps1 meson compile -C build/msvc
pwsh scripts/msvc-env.ps1 meson test -C build/msvc
```

On Linux:

```sh
meson setup build/linux --buildtype=release
meson compile -C build/linux
meson test -C build/linux
```

[BUILD.md](BUILD.md) has the options, dependencies and tests.

## Run

You need your own copy of the game: the STFS package from the XBLA download
(the file `584108C8/000D0000/0ED9DD87…` inside the zip), or a folder holding
the package's files (`default.xex`, `baseq3/pak0.bpk`, ...). Point the engine
at it with `fs_qaaPackage`; it is read in place and never written:

```powershell
build/msvc/openqaa.exe +set fs_qaaPackage "D:\Games\QAA\0ED9DD87783BD72BA56ED67ACD52BBFF1F47F08458"
```

- `+set fs_qaaPackage <path>`: the package file, a folder holding its files,
  or a `.bpk` file.
- `+set fs_qaaNameList <file>`: a text file naming pak0.bpk records, one
  `<16 hex digit hash> <path>` per line (a path alone is hashed by the
  engine once the hash is known).
- `qaa_info`: what is mounted, and how many records have names.
- `qaa_bpkread <hash>`: read a pak0.bpk record by its 64-bit name hash and
  print its size and CRC-32 (reads only).

Until the name hash is known no record can be found by name, so the engine
needs a `default.cfg` from somewhere (a loose `baseq3/default.cfg`, or a name
list entry) to start. For automated runs use `+set com_headless 2`, which
starts the client without a window.

## Tools

The Python tools read the package for investigation. Build the LZX library
first:

```sh
clang -O2 -shared -DQAA_LZX_SHARED -o .tmp/qaa_lzx.dll code/qaa/qaa_lzx.c
python tools/stfs.py <package> list
python tools/stfs.py <package> extract -o ref/extract
python tools/xex.py ref/extract/qagameXe.dll info
python tools/xex.py ref/extract/qagameXe.dll image -o ref/pe/qagameXe.dll.pe
python tools/bpk.py ref/extract/baseq3/pak0.bpk verify
```

`tools/xex.py` needs the Python `cryptography` package.

## Documentation

- [docs/PLAN.md](docs/PLAN.md): architecture and milestones
- [docs/FORMATS.md](docs/FORMATS.md): package and file format facts
- [BUILD.md](BUILD.md): building and testing
- [CREDITS.md](CREDITS.md): the engine's lineage and the imported FnQL commit
- [AGENTS.md](AGENTS.md): rules for contributors and AI agents

## Licence

GPL-2.0, as FnQL and the Quake III Arena source it descends from.
