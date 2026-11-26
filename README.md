# FnQAA

Run **Quake Arena Arcade** (Xbox Live Arcade, 2010) natively on the PC, from
your own unmodified copy of the game.

FnQAA uses the [FnQL](https://github.com/themuffinator/FnQL) engine. QAA's
own game code, its `qagameXe.dll` and `cgameXe.dll` modules, and its
Scaleform front end in `default.xex` are statically recompiled from your
package into native code on your machine. No game data
or game code is included in this repository.

> **Status: M4 to M6 under way.** The engine mounts your XBLA package in
> place and runs Quake Arena Arcade's own game code, recompiled from your
> package: a local game on `q3dm6` plays and renders, headless, and bots
> play in it, with QAA's own sounds and music (XMA2, decoded on the PC).
> QAA's own Scaleform menus, HUD and scoreboards run too (M6), from the
> title screen through the in-game menu, drawn by a software back end.
> Xbox Live features and profile saves are not there yet. See
> [the plan](docs/PLAN.md).

## Install

1. Download a [release](https://github.com/themuffinator/openQAA/releases)
   for Windows or Linux and extract it.
2. Put your copy of the game in that folder, as you have it. Any of these
   works:
   - the XBLA zip, `Quake Arena Arcade (World) (XBLA).zip`, or the zip
     extracted (it makes `584108C8/000D0000/0ED9DD87…`),
   - the `Content` folder copied from your Xbox 360 or its storage device
     (`Content/0000000000000000/584108C8/000D0000/0ED9DD87…`); the console
     keeps the game as the same file the zip holds,
   - the package file `0ED9DD87…` itself, or a folder of its files (from
     an Xbox 360 tool or an emulator).

   Nothing else needs extracting: the engine reads the game's data inside
   the package, in place, and never changes it.
3. Run `setup.cmd` (Windows) or `./setup.sh` (Linux) once. It finds the
   game, recompiles its game code on your machine (a few minutes) and
   installs it in the folder. It checks for the tools it needs first and
   says how to install any that are missing: Python 3 with `cryptography`,
   git, CMake, Ninja and clang (LLVM), plus Visual Studio 2022 or its Build
   Tools with the C++ workload on Windows.
4. Start `fnqaa`.

Run `setup` again after updating FnQAA. To keep the game elsewhere, run
`setup --package <path>` and start the engine with
`+set fs_qaaPackage <path>`.

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

The engine needs your own copy of the game: the STFS package from the XBLA
download (the file `584108C8/000D0000/0ED9DD87…` inside the zip, which is
the file the console keeps under `Content/0000000000000000/`), or a folder
holding the package's files (`default.xex`, `baseq3/pak0.bpk`, ...). It is
read in place and never written. With `fs_qaaPackage` unset the engine looks
for it in the install folder, then the home folder: under
`584108C8/000D0000/` (the zip extracted there), under
`Content/<profile>/584108C8/000D0000/` (a console's Content folder), or on
its own. Saves and title updates of the same title are skipped. From a
source build, name it:

```powershell
build/msvc/fnqaa.exe +set fs_qaaPackage "D:\Games\QAA\0ED9DD87783BD72BA56ED67ACD52BBFF1F47F08458"
```

- `+set fs_qaaPackage <path>`: the package file, a folder holding its files,
  or a `.bpk` file.
- `qaa_info`: what is mounted.
- `qaa_bpkread <hash>`: read a pak0.bpk record by its 64-bit name hash and
  print its size and CRC-32 (reads only).

pak0.bpk stores only a hash of each file's path, so, as on the Xbox, its
files are found by exact name and never appear in directory listings
(`dir`, the game's file lists). Loose files under `baseq3/` in the install
or home folder override the package. For automated runs use
`+set com_headless 2`, which starts the client without a window.

## Recompiling the game modules

```sh
python tools/recomp/build.py [--package <package>]
```

This decodes `qagameXe.dll`, `cgameXe.dll` and `default.xex` from your
package, recompiles the two game modules and the front end (SFUI and GFx in
`default.xex`) with a patched
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and builds native
libraries under `build/recomp/` (never committed). `<package>` is the STFS
package, the XBLA zip or a folder of the package's files; without it the
package is looked for in `ref/` (in a release, in the release folder). It
builds the LZX helper it decodes with itself. A release's `setup.cmd` and
`setup.sh` run this same script and copy the libraries next to the engine.
A source build's engine finds them in `build/recomp/` (or set
`qaa_modulePath`) and runs them in place of a game module and
the ui module; `+set qaa_game 0` and `+set qaa_ui 0` turn that off.
Quake Arena Arcade has no QVMs, so the engine loads none: FnQL would
otherwise fall back to `vm/*.qvm` whenever a native module is missing, and a
stray QVM from another game could replace QAA's own modules or menus.
`+set vm_allowQvm 1` on the command line (configs cannot set it) allows
them again.
[docs/RECOMP.md](docs/RECOMP.md) has the requirements, the tests and the
guest runtime's design.

## Tools

The Python tools read the package for investigation. Build the LZX library
first:

```sh
clang -O2 -shared -DQAA_LZX_SHARED -o .tmp/qaa_lzx.dll code/qaa/qaa_lzx.c code/qaa/qaa_bpk.c
python tools/stfs.py <package> list
python tools/stfs.py <package> extract -o ref/extract
python tools/xex.py ref/extract/qagameXe.dll info
python tools/xex.py ref/extract/qagameXe.dll image -o ref/pe/qagameXe.dll.pe
python tools/bpk.py ref/extract/baseq3/pak0.bpk verify
python tools/bpk_names.py ref/extract/baseq3/pak0.bpk --images ref/pe/*.pe -o ref/pak0.names
python tools/bpk.py ref/extract/baseq3/pak0.bpk extract -n ref/pak0.names -o ref/baseq3
```

`tools/xex.py` needs the Python `cryptography` package.

## Documentation

- [docs/PLAN.md](docs/PLAN.md): architecture and milestones
- [docs/FORMATS.md](docs/FORMATS.md): package and file format facts
- [docs/ABI.md](docs/ABI.md): the engine/game-module interface
- [docs/RECOMP.md](docs/RECOMP.md): the recompiler pipeline and guest runtime
- [docs/UI.md](docs/UI.md): the Scaleform front end
- [BUILD.md](BUILD.md): building, testing and releases
- [docs/CHANGELOG.md](docs/CHANGELOG.md): changes queued for the next release
- [CREDITS.md](CREDITS.md): the engine's lineage and the imported FnQL commit
- [AGENTS.md](AGENTS.md): rules for contributors and AI agents

## Licence

GPL-2.0, as FnQL and the Quake III Arena source it descends from.
