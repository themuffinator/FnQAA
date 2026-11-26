# Building FnQAA

FnQAA builds with Meson and Ninja. The primary targets are **Windows
x86_64** and **Linux x86_64**. Meson accepts the other targets FnQL enables
(32-bit x86, and the dedicated server on Linux aarch64, armv7l, ppc64le and
ppc64), but FnQAA does not test them.

A build produces:

| Output | What it is |
|---|---|
| `fnqaa` (`fnqaa.exe`) | the client |
| `fnqaa-ded` (`fnqaa-ded.exe`) | the dedicated server |
| `fnqaa_glx_<arch>`, `fnqaa_vk_<arch>`, `fnqaa_rtx_<arch>` | renderer modules loaded by the client (`cl_renderer glx/vk/rtx`) |
| `fnqaa_*_tests` | unit tests, run by `meson test` |

## Dependencies

Meson looks for system libraries first and otherwise builds the pinned
fallbacks in `subprojects/*.wrap` (downloaded on first configure): SDL3,
OpenAL Soft headers, libcurl, libjpeg-turbo, Ogg/Vorbis, FreeType, FontStash
and zlib. `--wrap-mode=nofallback` forces system libraries only.

## Windows

Install Visual Studio 2022 with the C++ x64 tools, Python 3, Meson and Ninja.
`scripts/msvc-env.ps1` runs a command inside the x64 developer environment
(found with vswhere), so no developer prompt is needed:

```powershell
pwsh scripts/msvc-env.ps1 meson setup build/msvc --buildtype=release
pwsh scripts/msvc-env.ps1 meson compile -C build/msvc
pwsh scripts/msvc-env.ps1 meson test -C build/msvc
```

From an "x64 Native Tools" prompt plain `meson` works the same way. The MSVC
x64 build uses FnQL's portable C fallbacks instead of its MASM helpers.
Tested with MSVC 19.44 (Visual Studio 2022).

### VS Code

`.vscode/` has build tasks and debugger launches in FnQL's style.
`.vscode/build.ps1` imports the x64 developer environment itself, then sets
up or reconfigures the build directory and builds the client, its renderer
modules and the dedicated server. The recompile task and every launch use
the STFS package at `ref/package/584108C8/000D0000/0ED9DD87783BD72BA56ED67ACD52BBFF1F47F08458`
(the package's own path inside the XBLA zip, under the ignored `ref/`). Keep
your package there, or set `QAA_PACKAGE` (as for the tests) for the tasks
and edit `fs_qaaPackage` in `launch.json` for the debugger.

| Task | What it does |
|---|---|
| `meson: build` (default, Ctrl+Shift+B) | Release build in `build/msvc` |
| `meson: build debug` | Debug build in `build/msvc-debug` |
| `meson: setup` | Configure `build/msvc` only |
| `recomp: game modules and front end` | `tools/recomp/build.py --package <package>` into `build/recomp` |
| `Build FnQAA Full` | The recompile, then the Release build |
| `meson: test` | Build everything and run every test, headless |
| `run: client (GLx)`, `run: dedicated server (q3dm6)` | Build, then run without the debugger (`.vscode/launch.ps1`) |

The launch configurations (`cppvsdbg`) start the windowed client with GLx,
Vulkan or RTX, the client straight into `q3dm6`, the Debug build (built
first), and the dedicated server on `q3dm6`. Both build directories use the recompiled modules in
`build/recomp/native/bin`.

## Linux

Install GCC or Clang, Python 3, Meson, Ninja, pkg-config, and the X11 and
OpenGL development headers (or SDL3's dependencies for the SDL backend):

```sh
meson setup build/linux --buildtype=release
meson compile -C build/linux
meson test -C build/linux
```

`-Dsdl=disabled` builds the X11 backend without SDL3. Tested with GCC 13.3
on Ubuntu 24.04 (WSL), `-Dsdl=disabled`, with system OpenAL, libjpeg and
Vorbis.

## Options

| Option | Default | Meaning |
|---|---|---|
| `build-client` | `true` | build `fnqaa` |
| `build-server` | `true` | build `fnqaa-ded` |
| `renderer-dlopen` | `true` | one client with external renderer modules; `false` links `renderer-default` in |
| `renderers` | `glx,vk,rtx` | renderer modules to build |
| `renderer-default` | `glx` | default `cl_renderer` |
| `sdl` | `auto` | SDL3 video, input and audio backend |
| `curl`, `curl-dlopen` | `auto` | HTTP downloads through libcurl |
| `system-jpeg` | `false` | require the system libjpeg |
| `ogg-vorbis` | `true` | Ogg Vorbis audio |
| `default-basedir` | empty | compiled-in `DEFAULT_BASEDIR` |
| `audio-tests`, `glx-tests` | `true` | build those unit tests |
| `strict-warnings` | `false` | warnings are errors in FnQAA's targets |

FnQL's Steam integration (Steamworks, the Workshop, Steam install detection)
is removed from FnQAA, and its Awesomium WebUI is not part of it; there is
no option for either. `code/qaa/qaa_null_webui.cpp` implements the WebUI's
engine interface with every service unavailable.

## Tests

`meson test` runs every test headless: the default test setup sets
`FNQAA_HEADLESS=2`, so a client never opens a window, and dedicated servers
are headless anyway.

- `fnqaa_qaa_package`: the STFS and BPK readers on a synthetic package,
  and the BPK name hash.
- `fnqaa_headless`: the client (`com_headless 2`, sound off) and the
  dedicated server start, mount a synthetic package (STFS file and folder),
  find the `default.cfg` in its pak0.bpk by name hash and execute it (it
  execs a second config by a mixed-case, backslash path), read a record by
  raw hash and quit with exit code 0. On Windows it also checks that the client
  never creates a visible window.
- With `QAA_PACKAGE` set to your STFS package, `fnqaa_headless` also
  boots both binaries from it (QAA's own `default.cfg`) and reads the first
  pak0.bpk record by hash.
  Without it that check is skipped. No retail data is ever written.
- `fnqaa_qaa_ui_raster`: the Scaleform front end's rasteriser (coverage,
  colour transforms, blending, masks, texture and glyph fills) and its DXT,
  DDS and TGA decoders, on synthetic data ([docs/UI.md](docs/UI.md)).
- The rest are FnQL's engine unit tests. `fnqaa_audio_loopback` skips
  when the OpenAL driver lacks `ALC_SOFT_loopback`.

```powershell
$env:QAA_PACKAGE = 'E:\path\to\584108C8\000D0000\0ED9DD87783BD72BA56ED67ACD52BBFF1F47F08458'
pwsh scripts/msvc-env.ps1 meson test -C build/msvc --print-errorlogs
```

## Releases

`.github/workflows/release.yml` builds a release on demand. It only runs
when started by hand: on GitHub open **Actions → release → Run workflow**
and pick the branch, or use the GitHub CLI:

```sh
gh workflow run release.yml --ref main
gh workflow run release.yml --ref main -f draft=false -f prerelease=true
```

| Input | Default | Meaning |
|---|---|---|
| `draft` | `true` | create the release as a draft (the changelog queue is kept) |
| `prerelease` | `true` | mark it as a pre-release |

### Versions

Versioning is FnQL's manual release channel (`scripts/manual_release.py`,
`scripts/fnqaa_meta.py`, `scripts/version.py`):

- `version/fnqaa_version.h` holds `<major>.<minor>.<patch>`; bump it by hand
  for a new line of releases, and tag a stable release `v<major>.<minor>.<patch>`.
- A release's build number is the number of commits since the latest stable
  tag (every commit while there is none). Its version is
  `<major>.<minor>.<patch>.<build>`, its tag `<version>-<YYYYMMDD>-<commit>`,
  its title `FnQAA <date> (<version>)`, and its archives
  `fnqaa-<tag>-windows-x86_64.zip` and `fnqaa-<tag>-linux-x86_64.tar.gz`.
- Each build job stamps the build number into `version/fnqaa_version.h`
  (`stamp-version`) before configuring, so the binaries and the Windows
  version resources report the release version.

`python scripts/manual_release.py summary` prints what the next release
would be.

### Release notes and the changelog

Release notes come from FnQL's changelog management (`scripts/changelog.py`):

- Add a player-facing entry to the `Unreleased` queue in
  [docs/CHANGELOG.md](docs/CHANGELOG.md) when a change lands, under the
  category that fits. The notes publish the queue as written: `Highlights`
  first (or the first entry of each category when it is empty), then every
  other entry by category.
- A curated `docs/releases/<tag>.md` or `<version>.md` replaces the queue for
  that release ([docs/releases/README.md](docs/releases/README.md)).
- With neither, the highlights come from the commit subjects.
- The notes go on with the getting-started steps, the build details and the
  commits since the previous release.
- After a published (non-draft) release the workflow resets the queue and
  pushes that as `Reset pending changelog after release`. After publishing
  a draft by hand, run `python scripts/changelog.py clear-unreleased` and
  commit.

`python scripts/manual_release.py release-notes --build-number <n>` renders
the notes locally, and `python scripts/changelog.py section --clean` shows
the queue.

### What the workflow does

1. tests the release scripts (`tests/release_scripts_tests.py`), resolves
   the version and tag, and refuses a tag that already exists;
2. builds Windows x86_64 with MSVC (Visual Studio 2022, static CRT, SDL3) and
   Linux x86_64 with GCC on Ubuntu 24.04 (X11 backend), each with
   `--wrap-mode=forcefallback` so the bundled dependencies are linked in
   statically;
3. runs every test headless (`FNQAA_HEADLESS=2`; on Linux the windowless
   client gets a virtual X display from Xvfb with Mesa's software OpenGL, and
   never maps a window);
4. packages both archives with `scripts/package_release.py`;
5. creates the GitHub release on the built commit with both archives,
   `SHA256SUMS.txt` and `release-manifest.json` (version, tag, commit, and
   each archive's size and SHA-256). A draft release creates its tag only
   when it is published.

FnQL's GitHub Models highlights, FnQL-game pin and Discord announcement are
not part of FnQAA's release.

### What an archive holds

An archive holds `fnqaa`, `fnqaa-ded`, the three renderer modules,
README.md, LICENSE, CREDITS.md, an INSTALL.txt, and the setup kit:
`setup.cmd` or `setup.sh` (from `scripts/release/`) and, under `setup/`, the
recompile tooling with the repository's layout (`tools/recomp/`, the
package and XEX readers, the guest runtime's source in `code/guest/`, the
LZX decoder's source and `data/abi`, `data/recomp`). That is all:
`package_release.py verify` fails on any other file, on anything named like
retail data or a recompiled module, and on binaries that link libraries a
plain Windows or Linux install lacks. Releases never contain the game's data
or code: users put their package in the release folder and run `setup`,
which recompiles the game modules from it on their machine (README.md,
Install). The Linux build needs glibc 2.38 and GCC 12's libstdc++ or newer
(Ubuntu 24.04, Debian 13, Fedora 39), and links nothing else but libX11.

To check an archive locally:

```sh
python scripts/package_release.py verify fnqaa-0.0.2.30-20261126-3f80a8a1-linux-x86_64.tar.gz
```
