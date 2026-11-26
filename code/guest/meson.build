# FnQAA guest runtime: hosts the statically recompiled game modules.
#
# Include from the top-level meson.build:
#
#   qaa_xenonrecomp_src = '<patched XenonRecomp tree>'   # build/recomp/xenonrecomp/src
#   subdir('code/guest')
#
# and link qaa_guest_dep. The tree is the one tools/recomp/build.py exports
# and patches; the runtime needs its XenonUtils/ppc_context.h and the simde
# headers that includes. Leave qaa_xenonrecomp_src unset (or empty) to skip
# the runtime. The module libraries themselves come from build.py.
#
# Also defines qaa_guest_tests (unit tests on a synthetic module) and
# qaa_gate (the headless M3 gate), and registers the unit tests.

qaa_guest_enabled = is_variable('qaa_xenonrecomp_src') and qaa_xenonrecomp_src != ''
if qaa_guest_enabled
  qaa_guest_cpp = meson.get_compiler('cpp')
  if qaa_guest_cpp.get_id() != 'clang' and qaa_guest_cpp.get_id() != 'clang-cl'
    error('the guest runtime needs clang (clang-cl on Windows): XenonRecomp\'s headers use clang builtins')
  endif

  qaa_guest_inc = include_directories('include')
  # As arguments: the export usually sits inside the source tree (build/),
  # where Meson refuses absolute include_directories().
  qaa_guest_xr_args = [
    '-I' + (qaa_xenonrecomp_src / 'XenonUtils'),
    '-I' + (qaa_xenonrecomp_src / 'thirdparty' / 'simde'),
  ]

  # What every translation unit with guest code needs.
  qaa_guest_code_args = [
    '-Wno-unused-label', '-Wno-unused-variable', '-Wno-unused-but-set-variable',
    '-Wno-null-arithmetic', '-Wno-c23-extensions', '-Wno-unused-function',
    '-Wno-unused-parameter', '-Wno-missing-field-initializers',
  ]
  if qaa_guest_cpp.get_argument_syntax() == 'msvc'
    qaa_guest_code_args += ['/clang:-march=x86-64-v2', '/clang:-fno-strict-aliasing', '-D_CRT_SECURE_NO_WARNINGS']
  else
    qaa_guest_code_args += ['-march=x86-64-v2', '-fno-strict-aliasing']
  endif

  qaa_guest_deps = [dependency('threads')]
  if host_machine.system() != 'windows'
    qaa_guest_deps += qaa_guest_cpp.find_library('dl', required: false)
  endif

  qaa_guest_lib = static_library('qaa_guest',
    files('src/guest.cpp', 'src/kernel.cpp'),
    include_directories: [qaa_guest_inc, include_directories('src')],
    cpp_args: qaa_guest_code_args + qaa_guest_xr_args,
    dependencies: qaa_guest_deps,
    override_options: ['cpp_std=c++20', 'warning_level=1'],
  )
  qaa_guest_dep = declare_dependency(
    link_with: qaa_guest_lib,
    include_directories: qaa_guest_inc,
    dependencies: qaa_guest_deps,
  )

  # A hand-written module in the recompiler's output style: no retail data.
  qaa_synthetic_module = shared_module('qaa_synthetic_module',
    files('tests/synthetic/synthetic_module.cpp'),
    include_directories: [qaa_guest_inc, include_directories('module', 'tests/synthetic')],
    cpp_args: qaa_guest_code_args + qaa_guest_xr_args + ['-DPPC_INCLUDE_DETAIL'],
    override_options: ['cpp_std=c++20', 'warning_level=1'],
  )

  qaa_guest_tests = executable('qaa_guest_tests',
    files('tests/test_guest.cpp'),
    dependencies: qaa_guest_dep,
    override_options: ['cpp_std=c++20', 'warning_level=1'],
  )
  test('qaa_guest', qaa_guest_tests, args: [qaa_synthetic_module], timeout: 120)

  qaa_gate = executable('qaa_gate',
    files('tests/gate.cpp'),
    dependencies: qaa_guest_dep,
    cpp_args: ['-DQAA_ABI_DIR="' + (meson.project_source_root() / 'data' / 'abi') + '"']
      + (qaa_guest_cpp.get_argument_syntax() == 'msvc' ? ['-D_CRT_SECURE_NO_WARNINGS'] : []),
    override_options: ['cpp_std=c++20', 'warning_level=1'],
  )
endif
