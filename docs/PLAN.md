# openQAA plan

Goal: run Quake Arena Arcade natively on the PC, from the user's unmodified
XBLA package, as close to the retail game as the PC allows.

## What the package holds (verified, see FORMATS.md)

| File | What it is |
|---|---|
| `default.xex` | The engine (`quake3xe.exe`): PowerPC, 16.3 MiB image at 0x82000000. Imports 313 xboxkrnl and 182 XAM functions. Statically links D3D9, D3DX's shader compiler, Scaleform GFx, libjpeg, libpng, zlib and botlib. |
| `qagameXe.dll` | Server game module (`qagame.dll`), image at 0x88000000. |
| `cgameXe.dll` | Client game module (`cgame.dll`), image at 0x86000000. |
| `baseq3/pak0.bpk` | All game data: 4,916 files, LZX-compressed, with hashed names. |
| `*.png`, `ArcadeInfo.xml` | Dashboard tiles and achievement metadata. |

The PDB paths read `C:\QuakeLiveArcade\code\...`. QAA was built from the
Quake Live code base, which is why FnQL is the right engine base.

Each module exports two ordinals: **ordinal 1 is `vmMain` and ordinal 2 is
`dllEntry`** (`Sys_LoadDll` 0x82184770, see [ABI.md](ABI.md)). Besides the
numbered syscalls, the engine hands each module a table of engine functions
through `vmMain` before `INIT`. The modules import only
CRT-level kernel functions: files, virtual memory, TLS, critical sections,
events, SEH, and one helper thread in cgame. There is no graphics, audio,
XAM or network import, so everything else goes through engine syscalls.

There is no `ui` module. Menus are Scaleform GFx movies (54 `GFX` files in
pak0) driven by `default.xex`'s SFUI layer. The HUD still uses
Quake Live-style `.menu` files parsed by the cgame.

pak0's data is in PC formats: BSP 47 (Quake Live's), MD3 15, little-endian
lumps, DXT1/3/5 DDS, WAV, TGA, JPEG, AAS 5. FnQL already reads all of these.

## Architecture

```
user's XBLA package (read-only)
  ├─ STFS reader ──► baseq3/pak0.bpk ──► BPK + LZX mount in the FnQL filesystem
  └─ default.xex, qagameXe.dll, cgameXe.dll
        │  build time, on the user's machine
        ▼
     XEX2 decode (AES, LZX) ──► PE image ──► PPC static recompiler ──► C++
                                                                      │
FnQL engine (native renderer, audio, input, net) ◄── syscall bridge ──┤
                                                     guest runtime ───┘
                                       (guest memory, xboxkrnl HLE)
```

- **Engine:** FnQL, imported engine-only and rebranded. It keeps its own
  renderers, audio, input and platform code. Engine behaviour that QAA
  changed is read from `default.xex` and applied, marked `// QAA:`.
- **Game modules:** recompiled, not rewritten. The generated code runs on a
  guest address space that mirrors the Xbox layout (modules at their image
  bases) and stays big-endian, as the recompiled loads and stores expect.
- **Syscall bridge:** the engine implements QAA's game and cgame trap tables.
  Arguments are guest addresses. Structures shared by pointer
  (`sharedEntity_t`, `playerState_t`, `refEntity_t`, `snapshot_t`,
  `usercmd_t`, `glconfig_t`, ...) are translated between big-endian guest
  memory and the engine's host structures at the boundary.
- **Kernel HLE:** about 55 xboxkrnl functions, mapped to the host C runtime
  and OS: files to the engine filesystem, memory, TLS, locks, events, the
  cgame helper thread.
- **Recompiler:** the modules hold about 88K PowerPC instructions each,
  including about 2,000 VMX/VMX128 vector instructions. XenonRecomp (MIT),
  with patches kept in `tools/recomp/patches/`, recompiles them at build time
  from configs our analysis generates ([RECOMP.md](RECOMP.md)).

## Milestones

**M0 Foundation** (done): STFS, XEX2 and BPK/LZX readers; the package
decodes fully; formats documented.

**M1 pak0 names** (done): the BPK name hash is recovered from
`default.xex` and implemented in `code/qaa/qaa_bpk.c`; `tools/bpk_names.py`
names 95% of the entries for tools. Retail cannot list a BPK, so the engine
needs no names: it opens files by exact name, as retail does.

**M2 Engine** (done; the `devmap` gate moves to M4). Import FnQL (engine
only) and rebrand it. Mount the STFS package and pak0.bpk natively, so no
extraction step is needed. FnQL 57f081f is imported engine-only as `openqaa` and
`openqaa-ded`, without Steam or the WebUI; the package and pak0.bpk mount in
place (`fs_qaaPackage`), files are found by name hash (`qaa_bpk_name_hash`)
as retail finds them, and the client and server boot headless from QAA's own
`default.cfg`. The `devmap` gate waits on the game modules: a map cannot
load without qagame (M4) and cgame (M5).

**M3 Recompiler pipeline** (done). XEX decode in the build; recompile
qagame and cgame; the guest runtime and kernel HLE; call `dllEntry` and
`vmMain`. `tools/recomp/build.py` builds both modules on Windows and Linux.
In the headless gate, qagame's `GAME_INIT` returns after 95 engine calls,
and cgame's `CG_INIT` after 690, with its asset-loading thread started.
Guest exception dispatch is tested on synthetic stacks only, since no real
run has raised one.

**ABI** (done): the game and cgame trap tables, vmMain commands, the engine
function tables and the shared structure layouts are recovered from
`default.xex` and both modules ([ABI.md](ABI.md), `data/abi/`).

**M4 Server game** (in progress). The engine hosts the recompiled qagame
(`code/server/sv_qaa_game.inc`): QAA's vmMain numbering, its game traps
with structure translation (`code/qaa/qaa_structs.h`), its engine function
table, host copies of entities and player states that follow guest memory,
and the guest runtime loaded as a shared library
(`code/qaa/qaa_guest_host.cpp`). Engine errors raised inside guest code
unwind the guest first. `map` and `devmap` take Quake III's arguments when
there are no factories, as QAA's engine does. Done: a headless dedicated
server runs `q3dm6` with QAA's game code. Bots: the botlib traps 200-581
and the engine table's three botlib entries run FnQL's botlib with QAA's
structures (`code/server/sv_qaa_botlib.inc`; QAA's own entity layouts and
its stubbed chat calls are in [ABI.md](ABI.md)). Headless, bots added with
`addbot` route, fight, die and respawn on `q3dm6`, on the dedicated and the
listen server. Open: with only bots the match stays in warmup (`CS_WARMUP`
-1), so frags do not score unless `g_doWarmup 0`; how QAA starts the match
outside the Live lobby is to be read from the game. And the engine's Quake
Live cvar defaults (`fraglimit`, `timelimit`) that override QAA's.

**M5 Client game** (in progress). The engine hosts the recompiled cgame
(`code/client/cl_qaa_cgame.inc`): QAA's trap numbering converted into
FnQL's dispatcher, snapshot, game state, glconfig, refEntity and font
translation, and the cgame engine table. The renderer reads QAA's DDS
textures (`code/renderercommon/tr_image_dds.c`) and loads
`scripts/master.shader` by name, as QAA's renderer does; the client draws
without a ui module. Done: a headless client plays `q3dm6` and takes a
screenshot of the world, items and view weapon. Sound works: QAA's
sounds and music are XMA2 (FORMATS.md), decoded by `code/qaa/qaa_xma2.c`
(ported from FFmpeg) through `snd_codec_xma.cpp`, with retail's `.xma`
before `.wav` lookup; a headless run on q3dm6 (OpenAL Soft's wave-file
backend, no audio device) plays the map's music and the cgame's weapon,
item and player sounds, looping sounds included. Open: the HUD is
Scaleform (`ui/hud.gfx`), so it arrives with M6; the
DrawStretchPic y-flip QAA's engine applies is to be checked once 2D draws;
`S_AddRealLoopingSound` stands in for QAA's own looping-sound call. PC
deviation: the cgame's model-loading thread is off
(`cg_asyncModelLoading 0`), because FnQL's renderer is single-threaded.

**M6 Menus.** QAA's Scaleform front end. Preferred path: recompile the
SFUI and GFx player code from `default.xex` with the same pipeline and give
its renderer interface an FnQL back end, so the menus run their own code.

**M7 Platform.** Controller layout as on the Xbox plus mouse and keyboard,
audio, profile and settings saves, local achievements, display options.

**M8 Multiplayer.** LAN and internet play in place of Xbox Live sessions.

## Rules carried through every milestone

- Retail behaviour wins; PC deviations are documented with their reason.
- Retail files are never modified, committed or redistributed.
- Every automated run is headless.
