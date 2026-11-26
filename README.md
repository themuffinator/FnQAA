# openQAA

Run **Quake Arena Arcade** (Xbox Live Arcade, 2010) natively on the PC, from
your own unmodified copy of the game.

openQAA uses the [FnQL](https://github.com/themuffinator/FnQL) engine. QAA's
own game code, its `qagameXe.dll` and `cgameXe.dll` modules, is statically
recompiled from your package into native code on your machine. No game data
or game code is included in this repository.

> **Status: M1.** The tools read the XBLA package, decrypt and decompress
> its XEX2 images, decode every file in `pak0.bpk`, and recover 95% of the
> archive's file names from QAA's own name hash. The engine is not imported
> yet. See [the plan](docs/PLAN.md).

## Tools

Build the LZX library, then point the tools at your package:

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
- [AGENTS.md](AGENTS.md): rules for contributors and AI agents

## Licence

GPL-2.0, as FnQL and the Quake III Arena source it descends from.
