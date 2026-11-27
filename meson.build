project('FnQAA', 'c', 'cpp',
  license: 'GPL-2.0-or-later',
  version: run_command(
    find_program('python3', 'python', required: true),
    '-c',
    'import re, pathlib; text = pathlib.Path("version/fnqaa_version.h").read_text(); print(re.search(r"#define FNQAA_VERSION_STRING \\"([^\\"]+)\\"", text).group(1))',
    check: true,
  ).stdout().strip(),
  meson_version: '>= 0.60.0',
  default_options: [
    meson.version().version_compare('>=1.3.0') ? 'c_std=gnu11,c11' : 'c_std=gnu11',
    'cpp_std=c++17',
    'buildtype=debugoptimized',
    'b_vscrt=from_buildtype',
    'warning_level=3',
  ],
)

python = find_program('python3', 'python', required: true)
fs = import('fs')
cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')

fallback_defaults = ['default_library=static']
quiet_fallback_defaults = fallback_defaults + ['warning_level=0']
curl_fallback_defaults = quiet_fallback_defaults + [
  'tests=disabled',
  'tool=disabled',
  'unittests=disabled',
  # Windows releases use Schannel and the built-in resolver. Keep optional
  # transfer encodings and protocols from silently linking runner-installed
  # DLLs into an otherwise self-contained package.
  'brotli=disabled',
  'gsasl=disabled',
  'gss-api=disabled',
  'http2=disabled',
  'idn=disabled',
  'libuv=disabled',
  'openssl=disabled',
  'psl=disabled',
  'rtmp=disabled',
  'ssh=disabled',
  'zstd=disabled',
]
openal_fallback_defaults = quiet_fallback_defaults
vorbis_fallback_defaults = quiet_fallback_defaults + [
  'tests=false',
  'ogg:default_library=static',
  'ogg:warning_level=0',
]
jpeg_fallback_defaults = quiet_fallback_defaults + ['tests=disabled']

win32 = host_machine.system() == 'windows'
darwin = host_machine.system() == 'darwin'
linux = host_machine.system() == 'linux'
if darwin
  add_languages('objc')
endif
cpu_family = host_machine.cpu_family()
# Targets are enabled one at a time (AGENTS.md Project Plan step 7). 32-bit
# x86 is the retail-compatible target on every platform; x86_64 is enabled on
# Windows and Linux, and aarch64, armv7l (hard-float), ppc64le and ppc64 on
# Linux, where FnQL-game supplies the game modules. The compiler probe catches
# a wrapper whose real target disagrees with the configured CPU family (a
# dropped -m32, an x32 ABI, a soft-float or big-endian ARM toolchain, or a
# ppc64 compiler whose byte order differs from the configured family).
fnql_target_probe_template = '''
#if !(@0@)
#error FnQAA requires a @1@ target
#endif
typedef char fnql_pointer_width_must_match[(sizeof(void *) == @2@) ? 1 : -1];
int main(void) { return 0; }
'''
# cpu_family: [systems (empty: any), predicate, pointer bytes]
fnql_enabled_targets = {
  'x86': [[],
    'defined(_M_IX86) || defined(__i386__)', '4'],
  'x86_64': [['windows', 'linux'],
    '(defined(_M_AMD64) || defined(__x86_64__)) && !defined(_M_ARM64EC)', '8'],
  'aarch64': [['linux'],
    'defined(__aarch64__) && !defined(__AARCH64EB__)', '8'],
  'arm': [['linux'],
    'defined(__arm__) && defined(__ARM_ARCH) && __ARM_ARCH >= 7 && defined(__ARM_PCS_VFP) && !defined(__ARMEB__)', '4'],
  'ppc64le': [['linux'],
    'defined(__PPC64__) && defined(__LITTLE_ENDIAN__)', '8'],
  'ppc64': [['linux'],
    'defined(__PPC64__) && defined(__BIG_ENDIAN__)', '8'],
}
fnql_target_supported = false
if fnql_enabled_targets.has_key(cpu_family)
  fnql_target = fnql_enabled_targets[cpu_family]
  if fnql_target[0].length() == 0 or fnql_target[0].contains(host_machine.system())
    fnql_target_supported = cc.compiles(
      fnql_target_probe_template.format(fnql_target[1], cpu_family, fnql_target[2]),
      name: cpu_family + ' target')
  endif
endif
if not fnql_target_supported
  error('FnQAA supports only 32-bit x86 targets and x86_64 on Windows and Linux, plus aarch64, armv7l (hard-float), ppc64le and ppc64 on Linux; Meson configured host CPU family "' + cpu_family + '" on "' + host_machine.system() + '" or the compiler target probe failed. Select Win32 or x64 on Windows, or use a native compiler or a matching cross file (misc/meson/linux-<arch>.ini) on Linux.')
endif

renderer_prefix = 'fnqaa'
build_client = get_option('build-client')
build_server = get_option('build-server')
# The client is enabled on x86 and x86_64 only. On the Linux aarch64, armv7l,
# ppc64le and ppc64 targets the gated artifacts are the dedicated server and
# FnQL-game's modules (FnQL-game milestone M11); a client there has no gate
# yet, so it is not built (AGENTS.md constraint 9).
if build_client and not ['x86', 'x86_64'].contains(cpu_family)
  error('The FnQAA client is not enabled on ' + cpu_family + ': only the dedicated server and FnQL-game modules are gated on this target. Configure with -Dbuild-client=false.')
endif
renderer_default = get_option('renderer-default')
renderer_dlopen = get_option('renderer-dlopen')
enabled_renderers = get_option('renderers')

# A modular client must always include its default GLx module, even when a
# developer requests only a Vulkan-family module in an explicit list.
if build_client and renderer_default == 'glx' and 'glx' not in enabled_renderers
  enabled_renderers += 'glx'
endif

if not build_client
  enabled_renderers = []
elif not renderer_dlopen
  enabled_renderers = [renderer_default]
endif

# Output names follow the FnQ3 baseline: x86 keeps the retail-shaped unsuffixed
# names, and every other target carries FnQ3's suffix so builds can share a
# directory.
if cpu_family == 'x86'
  renderer_arch = 'x86'
  binary_suffix = ''
elif cpu_family == 'x86_64'
  renderer_arch = 'x86_64'
  binary_suffix = win32 ? '.x64' : '.x86_64'
else
  # aarch64, arm, ppc64le and ppc64 (Linux only): FnQ3's names, which match
  # the engine's ARCH_STRING.
  renderer_arch = cpu_family
  binary_suffix = '.' + cpu_family
endif

# QAA: FnQAA's primary targets are Windows and Linux x86_64, so the
# executables carry no architecture suffix. Renderer modules keep the arch in
# their name because the client loads RENDERER_PREFIX_<name>_<arch>.
client_binary_name = 'fnqaa'
server_binary_name = 'fnqaa-ded'

common_c_args = []
common_cpp_args = []
link_args = []
dll_link_args = []

if cc.get_argument_syntax() == 'gcc'
  common_c_args += cc.get_supported_arguments([
    '-Wall',
    '-Wextra',
    '-Wimplicit',
    '-Wstrict-prototypes',
    '-Wno-unused-result',
    '-fvisibility=hidden',
    '-fno-strict-aliasing',
    '-fsigned-char',
  ])
  common_cpp_args += cpp.get_supported_arguments([
    '-Wall',
    '-Wextra',
    '-Wno-unused-result',
    '-fvisibility=hidden',
    '-fno-strict-aliasing',
    '-fsigned-char',
  ])
  if win32
    common_c_args += cc.get_supported_arguments(['-ffunction-sections'])
    common_cpp_args += cpp.get_supported_arguments(['-ffunction-sections'])
    if cc.get_linker_id() == 'link' or cc.get_linker_id() == 'lld-link'
      link_args += ['-Wl,/DYNAMICBASE', '-Wl,/NXCOMPAT']
    else
      link_args += ['-Wl,--dynamicbase', '-Wl,--nxcompat']
    endif
  else
    link_args += cc.get_supported_link_arguments(['-Wl,--gc-sections'])
  endif
elif cc.get_id() == 'msvc'
  common_c_args += ['/wd4201']
  common_cpp_args += ['/wd4201', '/EHsc']
endif

if get_option('strict-warnings')
  if cc.get_argument_syntax() == 'gcc'
    common_c_args += cc.get_supported_arguments(['-Werror'])
    common_cpp_args += cpp.get_supported_arguments(['-Werror'])
  elif cc.get_id() == 'msvc'
    common_c_args += ['/WX']
    common_cpp_args += ['/WX']
  endif
endif

if win32
  # MSVC marks portable C and Winsock interfaces as deprecated. Apply the
  # compatibility policy globally so bundled fallback dependencies receive the
  # same warning-clean Windows build as project-owned targets.
  win32_compat_args = ['-D_CRT_SECURE_NO_WARNINGS', '-D_CRT_NONSTDC_NO_WARNINGS', '-D_WINSOCK_DEPRECATED_NO_WARNINGS']
  add_global_arguments(win32_compat_args, language: ['c', 'cpp'])
  common_c_args += ['-DWIN32', '-D_WINDOWS', '-DNOMINMAX']
  common_cpp_args += ['-DWIN32', '-D_WINDOWS', '-DNOMINMAX']
elif darwin
  common_c_args += ['-DMACOS_X']
  common_cpp_args += ['-DMACOS_X']
endif

if get_option('local-headers')
  common_c_args += ['-DUSE_LOCAL_HEADERS=1']
  common_cpp_args += ['-DUSE_LOCAL_HEADERS=1']
endif

default_basedir = get_option('default-basedir')
if default_basedir != ''
  common_c_args += ['-DDEFAULT_BASEDIR="@0@"'.format(default_basedir)]
  common_cpp_args += ['-DDEFAULT_BASEDIR="@0@"'.format(default_basedir)]
endif

use_opengl_api = build_client and (renderer_default == 'glx' or 'glx' in enabled_renderers)
use_vulkan_api = build_client and (renderer_default in ['vk', 'rtx'] or 'vk' in enabled_renderers or 'rtx' in enabled_renderers)

if use_opengl_api
  common_c_args += ['-DUSE_OPENGL_API']
  common_cpp_args += ['-DUSE_OPENGL_API']
endif

if use_vulkan_api
  common_c_args += ['-DUSE_VULKAN_API']
  common_cpp_args += ['-DUSE_VULKAN_API']
endif

if renderer_dlopen
  common_c_args += [
    '-DUSE_RENDERER_DLOPEN',
    '-DRENDERER_PREFIX="@0@"'.format(renderer_prefix),
    '-DRENDERER_DEFAULT=@0@'.format(renderer_default),
  ]
  common_cpp_args += [
    '-DUSE_RENDERER_DLOPEN',
    '-DRENDERER_PREFIX="@0@"'.format(renderer_prefix),
    '-DRENDERER_DEFAULT=@0@'.format(renderer_default),
  ]
endif

include_dirs = include_directories(
  'code',
)

# QAA: FnQL's Steam integration and Workshop are removed (QAA has no Steam);
# the QAA package readers live in code/qaa.
qaa_src = [
  'code/qaa/qaa_bpk.c',
  'code/qaa/qaa_bpk_archive.c',
  'code/qaa/qaa_io.c',
  'code/qaa/qaa_lzx.c',
  'code/qaa/qaa_stfs.c',
]

qcommon_src = qaa_src + [
  'code/qaa/qaa_guest_host.cpp',
  'code/qcommon/cm_load.c',
  'code/qcommon/cm_patch.c',
  'code/qcommon/cm_polylib.c',
  'code/qcommon/cm_test.c',
  'code/qcommon/cm_trace.c',
  'code/qcommon/cmd.c',
  'code/qcommon/common.c',
  'code/qcommon/cvar.c',
  'code/qcommon/files.c',
  'code/qcommon/history.c',
  'code/qcommon/huffman_static.c',
  'code/qcommon/huffman.c',
  'code/qcommon/keys.c',
  'code/qcommon/qaa_binds.c',
  'code/qcommon/md4.c',
  'code/qcommon/md5.c',
  'code/qcommon/msg.cpp',
  'code/qcommon/net_chan.cpp',
  'code/qcommon/net_ip.c',
  'code/qcommon/puff.c',
  'code/qcommon/q_math.c',
  'code/qcommon/q_shared.c',
  'code/qcommon/unzip.c',
  'code/qcommon/vm.c',
]

platform_asm_src = []
client_asm_src = []
if win32 and cc.get_id() == 'msvc' and cpu_family == 'x86_64'
  message('Meson MSVC x64 builds use portable C fallbacks instead of MASM helpers; behavior stays the same, but hot-path setjmp/audio mixing optimizations are disabled for this build path')
  add_project_arguments('-DQ3_NO_CUSTOM_SETJMP', language: ['c', 'cpp'])
elif cpu_family == 'x86_64'
  client_asm_src += 'code/asm/snd_mix_x86_64_preprocessed.S'
endif

server_src = [
  'code/server/sv_bot.cpp',
  'code/server/sv_ccmds.cpp',
  'code/server/sv_client.cpp',
  'code/server/sv_filter.cpp',
  'code/server/sv_game.cpp',
  'code/server/sv_init.cpp',
  'code/server/sv_lockstep.cpp',
  'code/server/sv_main.cpp',
  'code/server/sv_net_chan.cpp',
  'code/server/sv_snapshot.cpp',
  'code/server/sv_world.cpp',
]

client_src = [
  'code/client/cl_avi.cpp',
  'code/client/cl_cgame.cpp',
  'code/client/cl_cin.cpp',
  'code/client/cl_console.cpp',
  'code/client/cl_input.cpp',
  'code/client/cl_jpeg.cpp',
  'code/client/cl_keys.cpp',
  'code/client/cl_main.cpp',
  'code/client/cl_net_chan.cpp',
  'code/client/cl_parse.cpp',
  'code/client/cl_scrn.cpp',
  # QAA: QAA's Scaleform front end, recompiled from default.xex (M6).
  'code/client/cl_qaa_ui.cpp',
  'code/client/cl_qaa_ui_raster.cpp',
  'code/client/audio/snd_main.cpp',
  'code/client/audio/legacy/snd_adpcm.cpp',
  'code/client/audio/legacy/snd_dma.cpp',
  'code/client/audio/legacy/snd_mem.cpp',
  'code/client/audio/legacy/snd_mix.cpp',
  'code/client/audio/legacy/snd_wavelet.cpp',
  'code/client/audio/codecs/snd_codec_wav.cpp',
  # QAA: QAA's sounds are XMA2, decoded by code/qaa/qaa_xma2.c.
  'code/client/audio/codecs/snd_codec_xma.cpp',
  'code/qaa/qaa_xma2.c',
  'code/client/audio/codecs/snd_codec.cpp',
  'code/client/audio/openal/AudioSystem.cpp',
]

botlib_src = [
  'code/botlib/be_aas_bspq3.cpp',
  'code/botlib/be_aas_cluster.cpp',
  'code/botlib/be_aas_debug.cpp',
  'code/botlib/be_aas_entity.cpp',
  'code/botlib/be_aas_file.cpp',
  'code/botlib/be_aas_main.cpp',
  'code/botlib/be_aas_move.cpp',
  'code/botlib/be_aas_optimize.cpp',
  'code/botlib/be_aas_reach.cpp',
  'code/botlib/be_aas_route.cpp',
  'code/botlib/be_aas_routealt.cpp',
  'code/botlib/be_aas_sample.cpp',
  'code/botlib/be_ai_char.cpp',
  'code/botlib/be_ai_chat.cpp',
  'code/botlib/be_ai_gen.cpp',
  'code/botlib/be_ai_goal.cpp',
  'code/botlib/be_ai_move.cpp',
  'code/botlib/be_ai_weap.cpp',
  'code/botlib/be_ai_weight.cpp',
  'code/botlib/be_ea.cpp',
  'code/botlib/be_interface.cpp',
  'code/botlib/l_crc.cpp',
  'code/botlib/l_libvar.cpp',
  'code/botlib/l_log.cpp',
  'code/botlib/l_memory.cpp',
  'code/botlib/l_precomp.cpp',
  'code/botlib/l_script.cpp',
  'code/botlib/l_struct.cpp',
]

renderer_common_src = [
  'code/renderercommon/tr_font.c',
  'code/renderercommon/tr_font_stash.c',
  'code/renderercommon/tr_image_bmp.c',
  'code/renderercommon/tr_image_dds.c',
  'code/renderercommon/tr_image_jpg.c',
  'code/renderercommon/tr_image_pcx.c',
  'code/renderercommon/tr_image_png.c',
  'code/renderercommon/tr_image_tga.c',
  'code/renderercommon/tr_image_wal.c',
  'code/renderercommon/tr_noise.c',
]

renderer_gl_src = [
  'code/renderer/tr_animation.c',
  'code/renderer/tr_arb.c',
  'code/renderer/tr_backend.c',
  'code/renderer/tr_bsp.c',
  'code/renderer/tr_cmds.c',
  'code/renderer/tr_curve.c',
  'code/renderer/tr_flares.c',
  'code/renderer/tr_image.c',
  'code/renderer/tr_init.c',
  'code/renderer/tr_light.c',
  'code/renderer/tr_main.c',
  'code/renderer/tr_marks.c',
  'code/renderer/tr_mesh.c',
  'code/renderer/tr_model_iqm.c',
  'code/renderer/tr_model.c',
  'code/renderer/tr_scene.c',
  'code/renderer/tr_shade_calc.c',
  'code/renderer/tr_shade.c',
  'code/renderer/tr_shader.c',
  'code/renderer/tr_shadows.c',
  'code/renderer/tr_sky.c',
  'code/renderer/tr_surface.c',
  'code/renderer/tr_vbo.c',
  'code/renderer/tr_world.c',
]

renderer_glx_src = [
  'code/rendererglx/glx_caps.cpp',
  'code/rendererglx/glx_debug.cpp',
  'code/rendererglx/glx_draw.cpp',
  'code/rendererglx/glx_executor.cpp',
  'code/rendererglx/glx_material.cpp',
  'code/rendererglx/glx_module.cpp',
  'code/rendererglx/glx_post_shader.cpp',
  'code/rendererglx/glx_postprocess.cpp',
  'code/rendererglx/glx_profiler.cpp',
  'code/rendererglx/glx_static_world.cpp',
  'code/rendererglx/glx_stream.cpp',
]

renderer_vk_src = [
  'code/renderervk/tr_animation.c',
  'code/renderervk/tr_backend.c',
  'code/renderervk/tr_bsp.c',
  'code/renderervk/tr_cmds.c',
  'code/renderervk/tr_curve.c',
  'code/renderervk/tr_image.c',
  'code/renderervk/tr_init.c',
  'code/renderervk/tr_light.c',
  'code/renderervk/tr_main.c',
  'code/renderervk/tr_marks.c',
  'code/renderervk/tr_mesh.c',
  'code/renderervk/tr_model_iqm.c',
  'code/renderervk/tr_model.c',
  'code/renderervk/tr_scene.c',
  'code/renderervk/tr_shade_calc.c',
  'code/renderervk/tr_shade.c',
  'code/renderervk/tr_shader.c',
  'code/renderervk/tr_shadows.c',
  'code/renderervk/tr_sky.c',
  'code/renderervk/tr_surface.c',
  'code/renderervk/tr_world.c',
  'code/renderervk/vk_flares.c',
  'code/renderervk/vk_vbo.c',
  'code/renderervk/vk.c',
]

# RTX remains a separate renderer while sharing Vulkan's file contract. Keep
# this derived list so Vulkan-family source drift is visible at configure time.
renderer_rtx_src = []
foreach renderer_vk_file : renderer_vk_src
  renderer_rtx_src += renderer_vk_file.replace('code/renderervk/', 'code/rendererrtx/')
endforeach

client_deps = []
server_deps = []
renderer_deps = []
copy_runtime_files = []

thread_dep = dependency('threads')
client_deps += thread_dep
server_deps += thread_dep

fontstash_dep = dependency('', required: false)
if build_client
  fontstash_dep = dependency('fontstash',
    required: true,
    fallback: 'fontstash',
  )
endif
if fontstash_dep.found()
  # Retail Quake Live's retained host-text lane uses the 2014 FontStash/STB
  # implementation. Keep it separate from the classic FreeType fontInfo_t lane.
  common_c_args += ['-DBUILD_FONTSTASH']
  renderer_deps += fontstash_dep
  # Static-renderer builds extract renderer objects into the client target.
  client_deps += fontstash_dep
endif

freetype_dep = dependency('', required: false)
if build_client
  freetype_dep = dependency('freetype2',
    required: true,
    fallback: 'freetype2',
    default_options: quiet_fallback_defaults + [
      'brotli=disabled',
      'bzip2=disabled',
      'harfbuzz=disabled',
      'png=disabled',
      'zlib=disabled',
    ],
  )
endif
if freetype_dep.found()
  common_c_args += ['-DBUILD_FREETYPE']
  renderer_deps += freetype_dep
  # Static-renderer builds extract renderer objects into the client target.
  client_deps += freetype_dep
endif

libm = cc.find_library('m', required: false)
if libm.found()
  client_deps += libm
  server_deps += libm
  renderer_deps += libm
endif

if not win32
  libdl = cc.find_library('dl', required: false)
  if libdl.found()
    client_deps += libdl
    server_deps += libdl
    renderer_deps += libdl
  endif
endif

if win32
  foreach lib : ['winmm', 'comctl32', 'ws2_32', 'gdi32', 'ole32', 'psapi', 'shell32', 'user32', 'advapi32']
    found_lib = cc.find_library(lib, required: true)
    client_deps += found_lib
    server_deps += found_lib
  endforeach
endif

openal_dep = dependency('', required: false)
if build_client or get_option('audio-tests')
  openal_dep = dependency('openal',
    method: 'pkg-config',
    required: true,
    fallback: 'openal-soft',
    default_options: openal_fallback_defaults,
  )
endif
openal_header_dep = dependency('', required: false)
openal_source = 'disabled'
if openal_dep.found()
  openal_header_dep = openal_dep.partial_dependency(compile_args: true, includes: true)
  openal_source = openal_dep.type_name() == 'internal' ? 'subproject headers' : 'system headers'
  client_deps += openal_header_dep
endif

sdl_opt = get_option('sdl')
sdl_dep = dependency('', required: false)
sdl_source = 'disabled'
if build_client and not sdl_opt.disabled()
  sdl_dep = dependency('sdl3',
    version: '>=3.2.0',
    required: false,
    fallback: 'sdl3',
    default_options: quiet_fallback_defaults,
  )
  if sdl_dep.found()
    sdl_source = sdl_dep.type_name() == 'internal' ? 'subproject' : 'system'
  endif
endif

if build_client and sdl_opt.enabled() and not sdl_dep.found()
  error('SDL3 was enabled but no SDL3 dependency was found')
endif

# Cocoa window, input, audio, and Vulkan-surface integration are provided by
# SDL3.  Falling through to the X11 backend on Darwin produces a configuration
# that can never build, so reject it at configure time with a useful diagnosis.
if darwin and build_client and not sdl_dep.found()
  error('macOS client builds require SDL3; configure with -Dsdl=enabled')
endif
use_sdl = build_client and not sdl_opt.disabled() and sdl_dep.found()

if use_sdl
  client_deps += sdl_dep
  common_c_args += ['-DSDL_FUNCTION_POINTER_IS_VOID_POINTER=1', '-DUSE_SDL_SYSCON=1']
  common_cpp_args += ['-DSDL_FUNCTION_POINTER_IS_VOID_POINTER=1', '-DUSE_SDL_SYSCON=1']
endif

if darwin and build_client
  client_deps += dependency('appleframeworks', modules: 'AppKit')
endif

curl_opt = get_option('curl')
use_curl = false
# Windows has no system libcurl to resolve at runtime (the engine would look for
# libcurl-3.dll and nothing ships it), so 'auto' links libcurl there and keeps
# runtime resolution on platforms with a system libcurl.
curl_dlopen_opt = get_option('curl-dlopen')
use_curl_dlopen = curl_dlopen_opt == 'true' or (curl_dlopen_opt == 'auto' and not win32)
curl_dep = dependency('', required: false)
curl_header_dep = dependency('', required: false)
curl_c_args = []
curl_source = 'disabled'

if build_client and not curl_opt.disabled()
  curl_dep = dependency('libcurl',
    required: curl_opt.enabled(),
    fallback: 'curl',
    default_options: curl_fallback_defaults,
  )

  if curl_dep.found()
    use_curl = true
    curl_source = curl_dep.type_name() == 'internal' ? 'subproject' : 'system'
    curl_header_dep = curl_dep.partial_dependency(compile_args: true, includes: true)
    if use_curl_dlopen
      client_deps += curl_header_dep
    else
      client_deps += curl_dep
    endif
    server_deps += curl_header_dep
  endif
endif

if use_curl
  client_src += 'code/client/cl_curl.cpp'
  common_c_args += ['-DUSE_CURL'] + curl_c_args
  common_cpp_args += ['-DUSE_CURL'] + curl_c_args
  if use_curl_dlopen
    common_c_args += ['-DUSE_CURL_DLOPEN']
    common_cpp_args += ['-DUSE_CURL_DLOPEN']
  endif
endif

jpeg_dep = dependency('', required: false)
jpeg_source = 'disabled'
if build_client
  jpeg_force_system = get_option('system-jpeg')
  if jpeg_force_system
    jpeg_dep = dependency('libjpeg', required: false)
    if not jpeg_dep.found()
      jpeg_dep = dependency('jpeg', required: true)
    endif
    jpeg_source = 'system'
  else
    jpeg_dep = dependency('libjpeg',
      required: true,
      fallback: 'libjpeg-turbo',
      default_options: jpeg_fallback_defaults,
    )
    jpeg_source = jpeg_dep.type_name() == 'internal' ? 'subproject' : 'system'
  endif
  client_deps += jpeg_dep
  common_c_args += ['-DUSE_SYSTEM_JPEG']
  common_cpp_args += ['-DUSE_SYSTEM_JPEG']
endif

if build_client and get_option('ogg-vorbis')
  vorbisfile_dep = dependency('vorbisfile',
    required: false,
    fallback: 'vorbis',
    default_options: vorbis_fallback_defaults,
  )
  if not vorbisfile_dep.found()
    error('Ogg Vorbis support was enabled but vorbisfile was not found')
  endif
  client_src += 'code/client/audio/codecs/snd_codec_ogg.cpp'
  client_deps += vorbisfile_dep
  common_c_args += ['-DUSE_OGG_VORBIS']
  common_cpp_args += ['-DUSE_OGG_VORBIS']
  ogg_vorbis_source = vorbisfile_dep.type_name() == 'internal' ? 'subproject' : 'system'
else
  ogg_vorbis_source = 'disabled'
endif

if win32
  win_resource = []
  if cc.get_argument_syntax() == 'gcc'
    rc_prog = find_program('windres', 'rc', 'llvm-rc', required: false)
  else
    rc_prog = find_program('rc', 'llvm-rc', 'windres', required: false)
  endif
  # Neither resource compiler emits a depfile, so the files the script pulls in
  # have to be listed explicitly. Without this an edit to the DPI manifest, the
  # icon, or the version header leaves the previous resource blob linked in and
  # the change appears to have no effect until the build tree is wiped.
  win_resource_deps = files(
    'code/win32/q3.manifest',
    'code/win32/resource.h',
    'code/win32/fnqaa.ico',
    'version/fnqaa_version.h',
  )
  if rc_prog.found()
    if rc_prog.full_path().to_lower().contains('windres')
      win_resource = custom_target('win_resource',
        input: 'code/win32/win_resource.rc',
        output: 'win_resource.o',
        depend_files: win_resource_deps,
        command: [rc_prog, '-i', '@INPUT@', '-o', '@OUTPUT@', '--include-dir', meson.project_source_root()],
      )
    else
      win_resource = custom_target('win_resource',
        input: 'code/win32/win_resource.rc',
        output: 'win_resource.res',
        depend_files: win_resource_deps,
        command: [rc_prog, '/fo', '@OUTPUT@', '/I', meson.project_source_root(), '@INPUT@'],
      )
    endif
  else
    warning('No Windows resource compiler found; Meson targets will build without win_resource.rc')
  endif
  q3_platform_src = [
    'code/win32/win_main.cpp',
    'code/win32/win_shared.cpp',
    'code/win32/win_syscon.cpp',
  ] + win_resource
  if build_client and use_sdl
    q3_ui_src = [
      'code/sdl/sdl_gamma.cpp',
      'code/sdl/sdl_glimp.cpp',
      'code/sdl/sdl_input.cpp',
      'code/sdl/sdl_snd.cpp',
    ]
  elif build_client
    q3_ui_src = [
      'code/win32/win_gamma.cpp',
      'code/win32/win_glimp.cpp',
      'code/win32/win_input.cpp',
      'code/win32/win_minimize.cpp',
      'code/win32/win_snd.cpp',
      'code/win32/win_wndproc.cpp',
    ]
    if use_opengl_api
      q3_ui_src += 'code/win32/win_qgl.cpp'
    endif
    if use_vulkan_api
      q3_ui_src += 'code/win32/win_qvk.cpp'
    endif
  endif

else
  q3_platform_src = [
    'code/unix/unix_main.cpp',
    'code/unix/unix_shared.cpp',
    'code/unix/unix_syscon.cpp',
    'code/unix/linux_signals.cpp',
  ]
  if build_client and use_sdl
    q3_ui_src = [
      'code/sdl/sdl_gamma.cpp',
      'code/sdl/sdl_glimp.cpp',
      'code/sdl/sdl_input.cpp',
      'code/sdl/sdl_snd.cpp',
    ]
    if darwin
      q3_ui_src += 'code/sdl/sdl_macos_window.m'
    endif
  elif build_client
    x11_dep = dependency('x11', required: true)
    client_deps += x11_dep
    q3_ui_src = [
      'code/unix/linux_glimp.cpp',
      'code/unix/linux_snd.cpp',
      'code/unix/x11_dga.cpp',
      'code/unix/x11_randr.cpp',
      'code/unix/x11_vidmode.cpp',
    ]
    if linux
      q3_ui_src += 'code/unix/linux_joystick.cpp'
    endif
    if use_opengl_api
      q3_ui_src += 'code/unix/linux_qgl.cpp'
    endif
    if use_vulkan_api
      q3_ui_src += 'code/unix/linux_qvk.cpp'
    endif
  endif
endif

foreach runtime_file : copy_runtime_files
  configure_file(
    input: runtime_file,
    output: runtime_file.split('/')[-1],
    copy: true,
    install: true,
    install_dir: '.',
  )
endforeach

# A release build of the engine defines NDEBUG, as the Makefile's release
# builds do.  Without it every shipped Meson binary named itself "-debug" in
# its version string (PLATFORM_STRING in q_platform.h), kept assert() live and
# ran the collision trace validator on every trace.  It is given to the engine
# targets only: the test executables below use assert() as their check and
# must keep it in a release build tree, which is what CI builds and tests.
engine_ndebug_args = get_option('buildtype') == 'release' ? ['-DNDEBUG'] : []
engine_c_args = common_c_args + engine_ndebug_args
engine_cpp_args = common_cpp_args + engine_ndebug_args

botlib = static_library('botlib', botlib_src,
  include_directories: include_dirs,
  cpp_args: engine_cpp_args + ['-DBOTLIB'],
  install: false,
)
botlib_objects = [botlib.extract_all_objects(recursive: false)]

renderer_base_src = renderer_common_src + ['code/qcommon/q_shared.c', 'code/qcommon/puff.c', 'code/qcommon/q_math.c'] + platform_asm_src
renderer_targets = []
renderer_module_type = darwin ? 'shared_module' : 'shared_library'

if build_client and renderer_dlopen
  renderer_c_args = engine_c_args + ['-DUSE_RENDERER_DLOPEN']
  renderer_cpp_args = engine_cpp_args + ['-DUSE_RENDERER_DLOPEN']

  if 'glx' in enabled_renderers
    renderer_targets += build_target(renderer_prefix + '_glx_' + renderer_arch,
      renderer_gl_src + renderer_glx_src + renderer_base_src,
      target_type: renderer_module_type,
      include_directories: include_dirs,
      dependencies: renderer_deps,
      c_args: renderer_c_args + ['-DRENDERER_CLASSIC', '-DRENDERER_GLX'],
      cpp_args: renderer_cpp_args + ['-DRENDERER_GLX'],
      link_args: dll_link_args,
      name_prefix: '',
      gnu_symbol_visibility: 'hidden',
      install: true,
      install_dir: '.',
    )
  endif

  if 'vk' in enabled_renderers
    renderer_targets += build_target(renderer_prefix + '_vk_' + renderer_arch,
      renderer_vk_src + renderer_base_src,
      target_type: renderer_module_type,
      include_directories: include_dirs,
      dependencies: renderer_deps,
      c_args: renderer_c_args + ['-DRENDERER_VULKAN'],
      cpp_args: renderer_cpp_args,
      link_args: dll_link_args,
      name_prefix: '',
      gnu_symbol_visibility: 'hidden',
      install: true,
      install_dir: '.',
    )
  endif

  if 'rtx' in enabled_renderers
    renderer_targets += build_target(renderer_prefix + '_rtx_' + renderer_arch,
      renderer_rtx_src + renderer_base_src,
      target_type: renderer_module_type,
      include_directories: include_dirs,
      dependencies: renderer_deps,
      c_args: renderer_c_args + ['-DRENDERER_RTX'],
      cpp_args: renderer_cpp_args,
      link_args: dll_link_args,
      name_prefix: '',
      gnu_symbol_visibility: 'hidden',
      install: true,
      install_dir: '.',
    )
  endif

endif

static_renderer_objects = []
if build_client and not renderer_dlopen
  if renderer_default == 'glx'
    renderer_glx_static_c_args = engine_c_args + [
      '-DRENDERER_CLASSIC',
      '-DRENDERER_GLX',
      '-DRE_DrawScaledText=R_GLX_DrawScaledText',
      '-DRE_MeasureScaledText=R_GLX_MeasureScaledText',
    ]
    static_renderer = static_library('renderer_static_glx',
      renderer_gl_src + renderer_glx_src + renderer_common_src,
      include_directories: include_dirs,
      dependencies: renderer_deps,
      c_args: renderer_glx_static_c_args,
      cpp_args: engine_cpp_args + ['-DRENDERER_GLX'],
      install: false,
    )
    static_renderer_objects = [static_renderer.extract_all_objects(recursive: false)]
  elif renderer_default == 'vk'
    renderer_vk_static_c_args = engine_c_args + [
      '-DRENDERER_VULKAN',
      '-DRE_DrawScaledText=R_VK_DrawScaledText',
      '-DRE_MeasureScaledText=R_VK_MeasureScaledText',
    ]
    static_renderer = static_library('renderer_static_vk',
      renderer_vk_src + renderer_common_src,
      include_directories: include_dirs,
      dependencies: renderer_deps,
      c_args: renderer_vk_static_c_args,
      cpp_args: engine_cpp_args,
      install: false,
    )
    static_renderer_objects = [static_renderer.extract_all_objects(recursive: false)]
  elif renderer_default == 'rtx'
    renderer_rtx_static_c_args = engine_c_args + [
      '-DRENDERER_RTX',
      '-DRE_DrawScaledText=R_RTX_DrawScaledText',
      '-DRE_MeasureScaledText=R_RTX_MeasureScaledText',
    ]
    static_renderer = static_library('renderer_static_rtx',
      renderer_rtx_src + renderer_common_src,
      include_directories: include_dirs,
      dependencies: renderer_deps,
      c_args: renderer_rtx_static_c_args,
      cpp_args: engine_cpp_args,
      install: false,
    )
    static_renderer_objects = [static_renderer.extract_all_objects(recursive: false)]
  endif
endif

client_c_args = engine_c_args
client_cpp_args = engine_cpp_args
if linux and build_client and not use_sdl
  client_cpp_args += ['-DUSE_JOYSTICK']
endif
if client_asm_src.length() > 0 and cc.get_id() != 'msvc'
  client_c_args += ['-DELF']
endif

if build_client
  client_exe = executable(client_binary_name,
    q3_platform_src + q3_ui_src + qcommon_src + server_src + client_src + platform_asm_src + client_asm_src,
    include_directories: include_dirs,
    dependencies: client_deps,
    objects: botlib_objects + static_renderer_objects,
    c_args: client_c_args,
    cpp_args: client_cpp_args,
    link_args: link_args,
    win_subsystem: win32 ? 'windows' : 'console',
    install: true,
    install_dir: '.',
  )
endif

if build_server
  server_exe = executable(server_binary_name,
    q3_platform_src + qcommon_src + server_src + platform_asm_src,
    include_directories: include_dirs,
    dependencies: server_deps,
    objects: botlib_objects,
    c_args: engine_c_args + ['-DDEDICATED'],
    cpp_args: engine_cpp_args + ['-DDEDICATED'],
    link_args: link_args,
    win_subsystem: win32 ? 'windows' : 'console',
    install: true,
    install_dir: '.',
  )
endif

if get_option('audio-tests')
  audio_zone_tests = executable('fnqaa_audio_zone_tests',
    'tests/audio/audiozone_runtime_tests.cpp',
    include_directories: include_directories('code/client/audio/shared'),
    cpp_args: common_cpp_args,
  )
  test('fnqaa_audio_zones', audio_zone_tests)

  audio_recovery_tests = executable('fnqaa_audio_recovery_tests',
    'tests/audio/recovery_policy_tests.cpp',
    include_directories: include_directories('code/client/audio/shared'),
    cpp_args: common_cpp_args,
  )
  test('fnqaa_audio_recovery', audio_recovery_tests)

  audio_init_policy_tests = executable('fnqaa_audio_init_policy_tests',
    'tests/audio/init_policy_tests.cpp',
    include_directories: include_directories('code/client/audio/shared'),
    cpp_args: common_cpp_args,
  )
  test('fnqaa_audio_init_policy', audio_init_policy_tests)

  audio_occlusion_tests = executable('fnqaa_audio_occlusion_tests',
    'tests/audio/occlusion_policy_tests.cpp',
    include_directories: include_directories('code/client/audio/shared'),
    cpp_args: common_cpp_args,
  )
  test('fnqaa_audio_occlusion', audio_occlusion_tests)

  audio_tone_tests = executable('fnqaa_audio_tone_tests',
    'tests/audio/tone_policy_tests.cpp',
    include_directories: include_directories('code/client/audio/shared'),
    cpp_args: common_cpp_args,
  )
  test('fnqaa_audio_tone', audio_tone_tests)

  audio_loopback_tests = executable('fnqaa_audio_loopback_tests',
    'tests/audio/openal_loopback_tests.cpp',
    dependencies: client_deps + [openal_header_dep],
    cpp_args: common_cpp_args,
  )
  test('fnqaa_audio_loopback', audio_loopback_tests, should_fail: false)

endif

# QAA: the package readers on synthetic data (no retail data needed).
qaa_package_tests = executable('fnqaa_qaa_package_tests',
  ['tests/qaa_package_tests.c'] + qaa_src,
  include_directories: include_directories('code/qaa'),
  c_args: common_c_args,
)
test('fnqaa_qaa_package', qaa_package_tests)

# QAA: the XMA2 decoder on synthetic data (its transform, windowing and
# Huffman tables); the test includes code/qaa/qaa_xma2.c itself.
qaa_xma2_tests = executable('fnqaa_qaa_xma2_tests',
  'tests/qaa_xma2_tests.c',
  c_args: common_c_args,
  # the decoder's transform calls sin/cos/pow/lrintf, which GCC links from libm
  dependencies: libm.found() ? [libm] : [],
)
test('fnqaa_qaa_xma2', qaa_xma2_tests)

netchan_profile_tests = executable('fnqaa_netchan_profile_tests',
  'tests/netchan_profile_tests.cpp',
  include_directories: include_directories('code/qcommon'),
  cpp_args: common_cpp_args,
)
test('fnqaa_netchan_profiles', netchan_profile_tests)

netchan_codec_tests = executable('fnqaa_netchan_codec_tests',
  'tests/netchan_codec_tests.cpp',
  include_directories: include_directories('code/qcommon'),
  cpp_args: common_cpp_args,
)
test('fnqaa_netchan_codec', netchan_codec_tests)

# QAA: the Scaleform front end's rasteriser and image decoders (M6)
qaa_ui_raster_tests = executable('fnqaa_qaa_ui_raster_tests',
  ['tests/qaa_ui_raster_tests.cpp', 'code/client/cl_qaa_ui_raster.cpp'],
  include_directories: include_directories('code/client'),
  cpp_args: common_cpp_args,
  # the rasteriser splits large triangles across worker threads
  dependencies: thread_dep,
)
test('fnqaa_qaa_ui_raster', qaa_ui_raster_tests)

ql_font_text_tests = executable('fnqaa_ql_font_text_tests',
  'tests/ql_font_text_tests.cpp',
  cpp_args: common_cpp_args,
)
test('fnqaa_ql_font_text', ql_font_text_tests)

window_placement_tests = executable('fnqaa_window_placement_tests',
  'tests/window_placement_tests.cpp',
  cpp_args: common_cpp_args,
)
test('fnqaa_window_placement', window_placement_tests)

global_fog_tests = executable('fnqaa_global_fog_tests',
  'tests/global_fog_tests.cpp',
  'code/qcommon/q_shared.c',
  c_args: common_c_args,
  cpp_args: common_cpp_args,
)
test('fnqaa_global_fog', global_fog_tests)

menu_blur_tests = executable('fnqaa_menu_blur_tests',
  'tests/menu_blur_tests.cpp',
  'code/qcommon/q_shared.c',
  c_args: common_c_args,
  cpp_args: common_cpp_args,
)
test('fnqaa_menu_blur', menu_blur_tests)

levelshot_validation_tests = executable('fnqaa_levelshot_validation_tests',
  'tests/levelshot_validation_tests.cpp',
  'code/qcommon/q_shared.c',
  c_args: common_c_args,
  cpp_args: common_cpp_args,
)
test('fnqaa_levelshot_validation', levelshot_validation_tests)

bmp_loader_tests = executable('fnqaa_bmp_loader_tests',
  [
    'tests/bmp_loader_tests.c',
    'code/renderercommon/tr_image_bmp.c',
  ],
  include_directories: include_directories('code/qcommon', 'code/renderercommon'),
  c_args: common_c_args,
)
test('fnqaa_bmp_loader', bmp_loader_tests)

huffman_decoder_tests = executable('fnqaa_huffman_decoder_tests',
  [
    'tests/huffman_decoder_tests.c',
    'code/qcommon/huffman.c',
  ],
  include_directories: include_directories('code/qcommon'),
  c_args: common_c_args,
)
test('fnqaa_huffman_decoder', huffman_decoder_tests)

filesystem_write_bounds_tests = executable('fnqaa_filesystem_write_bounds_tests',
  'tests/filesystem_write_bounds_tests.c',
  include_directories: include_directories('code/qcommon'),
  c_args: common_c_args,
)
test('fnqaa_filesystem_write_bounds', filesystem_write_bounds_tests)

cvar_integer_tests = executable('fnqaa_cvar_integer_tests',
  'tests/cvar_integer_tests.c',
  include_directories: include_directories('code/qcommon'),
  c_args: common_c_args,
)
test('fnqaa_cvar_integer', cvar_integer_tests)

qaa_binds_tests = executable('fnqaa_qaa_binds_tests',
  ['tests/qaa_binds_tests.c', 'code/qcommon/qaa_binds.c'],
  include_directories: include_directories('code/qcommon'),
  c_args: common_c_args,
)
test('fnqaa_qaa_binds', qaa_binds_tests)

cvar_retail_bounds_tests = executable('fnqaa_cvar_retail_bounds_tests',
  'tests/cvar_retail_bounds_tests.c',
  include_directories: include_directories('code/qcommon'),
  c_args: common_c_args,
)
test('fnqaa_cvar_retail_bounds', cvar_retail_bounds_tests)

png_loader_tests = executable('fnqaa_png_loader_tests',
  [
    'tests/png_loader_tests.c',
    'code/renderercommon/tr_image_png.c',
    'code/qcommon/puff.c',
  ],
  include_directories: include_directories('code/qcommon', 'code/renderercommon'),
  c_args: common_c_args,
)
test('fnqaa_png_loader', png_loader_tests)

netchan_safety_tests = executable('fnqaa_netchan_safety_tests',
  'tests/netchan_safety_tests.cpp',
  include_directories: include_directories('code/qcommon'),
  cpp_args: common_cpp_args,
)
test('fnqaa_netchan_safety', netchan_safety_tests)

snapshot_history_tests = executable('fnqaa_snapshot_history_tests',
  'tests/snapshot_history_tests.cpp',
  'code/qcommon/msg.cpp',
  'code/qcommon/huffman_static.c',
  c_args: common_c_args + ['-DDEDICATED'],
  cpp_args: common_cpp_args + ['-DDEDICATED'],
)
test('fnqaa_snapshot_history', snapshot_history_tests)

input_compat_tests = executable('fnqaa_input_compat_tests',
  'tests/input_compat_tests.cpp',
  cpp_args: common_cpp_args,
)
test('fnqaa_input_compat', input_compat_tests)

client_cvar_compat_tests = executable('fnqaa_client_cvar_compat_tests',
  'tests/client_cvar_compat_tests.cpp',
  cpp_args: common_cpp_args,
)
test('fnqaa_client_cvar_compat', client_cvar_compat_tests)

server_cvar_compat_tests = executable('fnqaa_server_cvar_compat_tests',
  'tests/server_cvar_compat_tests.cpp',
  cpp_args: common_cpp_args,
)
test('fnqaa_server_cvar_compat', server_cvar_compat_tests)

message_codec_tests = executable('fnqaa_message_codec_tests',
  'tests/message_codec_tests.cpp',
  'code/qcommon/msg.cpp',
  'code/qcommon/huffman_static.c',
  include_directories: include_directories('code/qcommon'),
  c_args: common_c_args + ['-DDEDICATED'],
  cpp_args: common_cpp_args + ['-DDEDICATED'],
)
test('fnqaa_message_codec', message_codec_tests)

protocol_layout_tests = executable('fnqaa_protocol_layout_tests',
  'tests/protocol_layout_tests.cpp',
  include_directories: include_directories('code/qcommon'),
  cpp_args: common_cpp_args,
)
test('fnqaa_protocol_layout', protocol_layout_tests)

protocol_contract_tests = executable('fnqaa_protocol_contract_tests',
  'tests/protocol_contract_tests.cpp',
  include_directories: include_directories('code/qcommon'),
  cpp_args: common_cpp_args,
)
test('fnqaa_protocol_contract', protocol_contract_tests)

retail_auth_challenge_tests = executable('fnqaa_retail_auth_challenge_tests',
  'tests/retail_auth_challenge_tests.cpp',
  include_directories: include_directories('code/client'),
  cpp_args: common_cpp_args,
)
test('fnqaa_retail_auth_challenge', retail_auth_challenge_tests)

demo_stream_tests = executable('fnqaa_demo_stream_tests',
  'tests/demo_stream_tests.cpp',
  include_directories: include_directories('code/client'),
  cpp_args: common_cpp_args,
)
test('fnqaa_demo_stream', demo_stream_tests)

ql_bsp_contract_tests = executable('fnqaa_ql_bsp_contract_tests',
  'tests/ql_bsp_contract_tests.cpp',
  include_directories: include_directories('code/qcommon'),
  cpp_args: common_cpp_args,
)
test('fnqaa_ql_bsp_contract', ql_bsp_contract_tests)

collision_model_parity_tests = executable('fnqaa_collision_model_parity_tests',
  'tests/collision_model_parity_tests.cpp',
  cpp_args: common_cpp_args,
)
test('fnqaa_collision_model_parity', collision_model_parity_tests)

# Links the real trace path against a stub collision model so the capsule
# geometry and the debug trace-plane invariant are exercised, not just
# described.
# q_math.c calls atan2/sincos/logf/powf/acos, which i686 emits as real libm
# calls instead of inlining, so this target needs the math library explicitly.
collision_capsule_trace_deps = []
if libm.found()
  collision_capsule_trace_deps += libm
endif

collision_capsule_trace_tests = executable('fnqaa_collision_capsule_trace_tests',
  [
    'tests/collision_capsule_trace_tests.c',
    'code/qcommon/cm_trace.c',
    'code/qcommon/q_math.c',
  ],
  include_directories: include_directories('code/qcommon'),
  c_args: common_c_args,
  dependencies: collision_capsule_trace_deps,
)
test('fnqaa_collision_capsule_trace', collision_capsule_trace_tests)

if get_option('glx-tests')
  glx_logic_tests = executable('fnqaa_glx_logic_tests',
    'tests/glx/glx_logic_tests.cpp',
    'code/rendererglx/glx_executor.cpp',
    include_directories: include_directories('code/rendererglx', 'code/qcommon', 'code/renderercommon'),
    cpp_args: common_cpp_args,
  )
  test('fnqaa_glx_logic', glx_logic_tests)
endif

# QAA: the client and the dedicated server start headless, mount a synthetic
# package in place and quit cleanly. With QAA_PACKAGE set to the user's STFS
# package, both also read a record of the real pak0.bpk by hash.
if build_client and build_server
  test('fnqaa_headless', python,
    args: [files('tests/headless_tests.py'), client_exe, server_exe],
    depends: renderer_targets,
    env: {'MESON_BUILD_ROOT': meson.current_build_dir()},
    is_parallel: false,
    timeout: 300,
  )
endif

curl_summary = 'disabled'
if use_curl
  curl_summary = '@0@ (@1@)'.format(use_curl_dlopen ? 'dlopen' : 'linked', curl_source)
endif

summary({
  'client executable': build_client ? client_binary_name : 'disabled',
  'dedicated server': build_server ? server_binary_name : 'disabled',
  'renderer-dlopen': build_client ? renderer_dlopen : false,
  'renderer modules': build_client and renderer_dlopen ? ', '.join(enabled_renderers) : 'disabled',
  'static renderer': build_client and not renderer_dlopen ? renderer_default : 'disabled',
  'renderer default': renderer_default,
  'renderer arch': renderer_arch,
  'SDL3': use_sdl ? sdl_source : 'disabled',
  'OpenAL': openal_source,
  'cURL': curl_summary,
  'JPEG': jpeg_source,
  'Ogg Vorbis': ogg_vorbis_source,
}, section: 'FnQAA build')

# Every test runs unattended (AGENTS.md headless rule): clients start
# windowless (com_headless 2, which still renders and captures) and dedicated
# servers headless, unless a test passes +set com_headless itself.
# QAA: the variable is FNQAA_HEADLESS (FnQL's FNQL_HEADLESS).
add_test_setup('headless', env: {'FNQAA_HEADLESS': '2'}, is_default: true)
