# Building openQAA

openQAA builds with Meson and Ninja. The primary targets are **Windows
x86_64** and **Linux x86_64**. Meson accepts the other targets FnQL enables
(32-bit x86, and the dedicated server on Linux aarch64, armv7l, ppc64le and
ppc64), but openQAA does not test them.

A build produces:

| Output | What it is |
|---|---|
| `openqaa` (`openqaa.exe`) | the client |
| `openqaa-ded` (`openqaa-ded.exe`) | the dedicated server |
| `openqaa_glx_<arch>`, `openqaa_vk_<arch>`, `openqaa_rtx_<arch>` | renderer modules loaded by the client (`cl_renderer glx/vk/rtx`) |
| `openqaa_*_tests` | unit tests, run by `meson test` |

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
| `build-client` | `true` | build `openqaa` |
| `build-server` | `true` | build `openqaa-ded` |
| `renderer-dlopen` | `true` | one client with external renderer modules; `false` links `renderer-default` in |
| `renderers` | `glx,vk,rtx` | renderer modules to build |
| `renderer-default` | `glx` | default `cl_renderer` |
| `sdl` | `auto` | SDL3 video, input and audio backend |
| `curl`, `curl-dlopen` | `auto` | HTTP downloads through libcurl |
| `system-jpeg` | `false` | require the system libjpeg |
| `ogg-vorbis` | `true` | Ogg Vorbis audio |
| `default-basedir` | empty | compiled-in `DEFAULT_BASEDIR` |
| `audio-tests`, `glx-tests` | `true` | build those unit tests |
| `strict-warnings` | `false` | warnings are errors in openQAA's targets |

FnQL's Steam integration (Steamworks, the Workshop, Steam install detection)
is removed from openQAA, and its Awesomium WebUI is not part of it; there is
no option for either. `code/qaa/qaa_null_webui.cpp` implements the WebUI's
engine interface with every service unavailable.

## Tests

`meson test` runs every test headless: the default test setup sets
`OPENQAA_HEADLESS=2`, so a client never opens a window, and dedicated servers
are headless anyway.

- `openqaa_qaa_package`: the STFS and BPK readers on a synthetic package,
  and the BPK name hash.
- `openqaa_headless`: the client (`com_headless 2`, sound off) and the
  dedicated server start, mount a synthetic package (STFS file and folder),
  find the `default.cfg` in its pak0.bpk by name hash and execute it (it
  execs a second config by a mixed-case, backslash path), read a record by
  raw hash and quit with exit code 0. On Windows it also checks that the client
  never creates a visible window.
- With `QAA_PACKAGE` set to your STFS package, `openqaa_headless` also
  boots both binaries from it (QAA's own `default.cfg`) and reads the first
  pak0.bpk record by hash.
  Without it that check is skipped. No retail data is ever written.
- `openqaa_qaa_ui_raster`: the Scaleform front end's rasteriser (coverage,
  colour transforms, blending, masks, texture and glyph fills) and its DXT,
  DDS and TGA decoders, on synthetic data ([docs/UI.md](docs/UI.md)).
- The rest are FnQL's engine unit tests. `openqaa_audio_loopback` skips
  when the OpenAL driver lacks `ALC_SOFT_loopback`.

```powershell
$env:QAA_PACKAGE = 'E:\path\to\584108C8\000D0000\0ED9DD87783BD72BA56ED67ACD52BBFF1F47F08458'
pwsh scripts/msvc-env.ps1 meson test -C build/msvc --print-errorlogs
```
