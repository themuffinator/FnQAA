# Credits

## Engine lineage

FnQAA's engine is imported from FnQL, which descends from a long id Tech 3
lineage. Every license header and provenance comment from these projects is
kept in the imported code.

- [Quake III Arena](https://github.com/id-Software/Quake-III-Arena) by
  id Software: the original GPL engine.
- [ioquake3](https://github.com/ioquake/ioq3): the community continuation of
  the Quake III code base.
- [Quake3e](https://github.com/ec-/Quake3e): the modernized engine that FnQ3
  built on.
- [FnQ3](https://github.com/themuffinator/FnQ3): the modernized renderers,
  audio, platform and tooling that FnQL imported.
- [FnQL](https://github.com/themuffinator/FnQL): the Quake Live engine that
  FnQAA uses, engine only.

## Imported FnQL commit

The engine was imported from FnQL commit
`57f081f41915893ca3b6f521c4e285e3af917b3d` (2026-11-23, "Validate
retail-bounded cvars as retail does so com_maxfps 250x works"), taken with
`git archive` from the committed tree.

Imported: the client, server, qcommon, botlib, the GLx, Vulkan and RTX
renderers, audio, the SDL, Win32 and Unix platform layers, the game, cgame
and ui ABI headers, the Meson build with `subprojects/*.wrap`, the version
header (now `version/fnqaa_version.h`) and FnQL's compiled unit tests.

Left out: FnQL's Steam provider loader and Workshop adapter, the Awesomium
WebUI and its web.pak, the audio-zone authoring tool, the Visual Studio
projects, CMake and Make, release packaging, promo media, `pkg/` and the
Python source-regression tests. Steam and the WebUI are replaced by null
backends in `code/qaa/`. Changes to imported files are marked `// QAA:`.

FnQL's own credits, which carry into the imported code, include:

- [QL-SRP](https://github.com/themuffinator/QL-SRP) for reconstructed Quake
  Live behaviour and compatibility evidence.
- [CPMADevs CNQ3](https://bitbucket.org/CPMADevs/cnq3) for the rainbow text
  colour escape lineage.
- [Spearmint](https://github.com/zturtleman/spearmint) for the Quake 3 IHV
  (`IBSP v43`) map loading reference.
- Luigi Auriemma's [qldec](https://aluigi.altervista.org/papers.htm#qldec)
  for the Quake Live beta encrypted `.pk3` XOR table reference.
- [DarkMatter-Q2](https://github.com/Paril/DarkMatter-Q2) for cross-checking
  Quake II `.pak` and `.wal` support.
- Mikko Mononen's [FontStash](https://github.com/memononen/fontstash) and
  Sean Barrett's `stb_truetype` for the host text rasterizer.

The application icon (`code/win32/fnqaa.ico`, `code/sdl/sdl_icon.h`) is
still FnQL's.

## Formats

- The [Free60](https://free60.org/) STFS documentation, for the Xbox 360
  package layout read by `tools/stfs.py` and `code/qaa/qaa_stfs.c`.
- Microsoft's published LZX format description, for `code/qaa/qaa_lzx.c`.
- [FFmpeg](https://ffmpeg.org/)'s WMA Pro / XMA decoder (`libavcodec/
  wmaprodec.c`, `wma.c` and `wmaprodata.h`, FFmpeg 7.1, LGPL 2.1 or later,
  by Baptiste Coudurier, Benjamin Larsson, Ulion, Sascha Sommer and the
  FFmpeg project): ported into `code/qaa/qaa_xma2.c` and
  `code/qaa/qaa_xma2_tables.h`, which keep its notices and are used under
  the GPL as the LGPL allows.

## Trademark note

FnQAA is an independent project and is not affiliated with, endorsed by,
or sponsored by id Software, Bethesda, ZeniMax Media, Microsoft or Valve.
Quake, Quake III Arena, Quake Live and Quake Arena Arcade are trademarks of
their respective owners.
