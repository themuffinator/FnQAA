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

Each module exports two ordinals (presumably `dllEntry` and `vmMain`; the
cgame has the string `vmMain: unknown command %i`). The modules import only
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
  including about 2,000 VMX/VMX128 vector instructions, so the lifter must
  cover VMX128. Planned: XenonRecomp (MIT), driven by our decoded images and
  `.pdata` function tables, run as a build step. To be confirmed in M3.

## Milestones

**M0 Foundation** (done): STFS, XEX2 and BPK/LZX readers; the package
decodes fully; formats documented.

**M1 pak0 names.** Recover the BPK name hash from `default.xex`'s
filesystem, then build a name list from every path string in the binaries
and the decoded text files, so that every entry has its path. Gate: every
entry named, or listed as unnamed.

**M2 Engine.** Import FnQL (engine only) and rebrand it. Mount the STFS
package and pak0.bpk natively, so no extraction step is needed. Gate: a
headless `devmap` loads a QAA map in FnQL's renderer and takes a screenshot.
Status: in progress. FnQL 57f081f is imported engine-only as `openqaa` and
`openqaa-ded`, without Steam or the WebUI; the package and pak0.bpk mount in
place (`fs_qaaPackage`) and records read by raw hash; the client and server
start and quit headless. The `devmap` gate waits on the name hash (M1), which
`QAA_BpkNameHash` (`code/qaa/qaa_bpk_hash.c`) holds as a placeholder.

**M3 Recompiler pipeline.** XEX decode in the build; recompile qagame and
cgame; the guest runtime and kernel HLE; call `dllEntry` and `vmMain`.
Gate: `GAME_INIT` runs and returns.

**M4 Server game.** Recover QAA's game trap table from `default.xex`
(`SV_GameSystemCalls`) and from the module's trap calls; write the bridge
and its structure translation. Gate: a headless server runs a map with bots
for N frames, deterministically.

**M5 Client game.** The cgame trap table and bridge. Gate: a headless
client renders a match with QAA's HUD.

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
