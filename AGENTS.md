# AGENTS.md

Instructions for AI agents working in this repository. Read
[docs/PLAN.md](docs/PLAN.md) first: it holds the architecture and milestones.

## Mission

openQAA runs **Quake Arena Arcade** (Xbox Live Arcade, 2010) natively on the
PC from the user's own, unmodified XBLA package. The engine is
[FnQL](https://github.com/themuffinator/FnQL) (engine only). The retail game
modules, `qagameXe.dll` and `cgameXe.dll`, are statically recompiled from the
user's package into native code and hosted by the engine. Nothing is
reimplemented that can be recompiled.

1. **Retail behaviour is authoritative.** Where QAA differs from Quake Live
   or Quake III, QAA wins. PC deviations (resolution, mouse and keyboard,
   networking without Xbox Live) are allowed only where the Xbox behaviour
   cannot apply, and each one is documented.
2. **Never modify the retail files.** The package, `default.xex`, the modules
   and `pak0.bpk` are read-only inputs, read in place.
3. **Never commit or ship retail data or code derived from it.** That
   includes extracted assets, decoded PE images, decompiler output and
   recompiled C/C++. Recompilation runs on the user's machine from their copy.

## Never commit game data or decompiler output

- `ref/` holds the user's package, everything extracted or decoded from it,
  and analysis databases. It is ignored by git and must stay that way.
- Tests use synthetic data. Checks against the real package take its path
  from the environment and are skipped when it is absent.

## Where things are

| Need | Where |
|---|---|
| The user's package (zip) | `ref/Quake Arena Arcade (World) (XBLA).zip` |
| The STFS package | `ref/package/584108C8/000D0000/0ED9DD87783BD72BA56ED67ACD52BBFF1F47F08458` |
| Files from the package | `ref/extract/` (`python tools/stfs.py <pkg> extract -o ref/extract`) |
| Decoded PE images | `ref/pe/<name>.pe` (`python tools/xex.py <xex> image -o ...`) |
| pak0.bpk entries, by name hash | `ref/bpk/<hash>` (`python tools/bpk.py <bpk> extract -o ref/bpk`) |
| Format facts | `docs/FORMATS.md` |
| Engine baseline | `E:\Repositories\FnQL` (engine only; see its `AGENTS.md`) |

The tools need the LZX library built first:
`clang -O2 -shared -DQAA_LZX_SHARED -o .tmp/qaa_lzx.dll code/qaa/qaa_lzx.c`
(or set `QAA_LZX_DLL`). `tools/xex.py` needs the `cryptography` package.

## Formats

Every claim in `docs/FORMATS.md` is marked **verified** (a tool exercises it
over the whole package), **code** (read from named retail code) or
**observed** (seen in some samples). Keep observed facts separate from
inferences in docs, comments and commits.

## Code

- Engine code follows FnQL's style and AGENTS.md rules. Keep id's and FnQL's
  license headers; mark each QAA change in imported files with a `// QAA:`
  comment saying whether it is retail behaviour (with the retail function's
  address) or a PC deviation (with the reason).
- Generated (recompiled) code is build output, never source.
- Run fully headless for every automated test: no window, audio device or
  input capture. Shut the game down when the test ends.
