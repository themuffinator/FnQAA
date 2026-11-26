# openQAA guest runtime: hosts the statically recompiled game modules.
#
# Needs QAA_XENONRECOMP_SRC: a (patched) XenonRecomp source tree, for
# XenonUtils/ppc_context.h and the simde headers it includes. The engine
# links qaa_guest and loads the module libraries tools/recomp/build.py
# produces.

if(NOT QAA_XENONRECOMP_SRC)
	message(FATAL_ERROR "set QAA_XENONRECOMP_SRC to the XenonRecomp source tree")
endif()

set(QAA_GUEST_DIR ${CMAKE_CURRENT_SOURCE_DIR})

add_library(qaa_guest STATIC
	src/guest.cpp
	src/kernel.cpp
)
target_include_directories(qaa_guest
	PUBLIC
		${QAA_GUEST_DIR}/include
	PRIVATE
		${QAA_GUEST_DIR}/src
		${QAA_XENONRECOMP_SRC}/XenonUtils
		${QAA_XENONRECOMP_SRC}/thirdparty/simde
)
target_compile_features(qaa_guest PUBLIC cxx_std_20)
target_compile_definitions(qaa_guest PRIVATE _CRT_SECURE_NO_WARNINGS)
if(NOT WIN32)
	target_link_libraries(qaa_guest PUBLIC ${CMAKE_DL_LIBS} pthread)
endif()

# Compile options every guest-code translation unit needs.
function(qaa_guest_code_options target)
	if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		if(MSVC)
			target_compile_options(${target} PRIVATE /clang:-march=x86-64-v2 /clang:-fno-strict-aliasing
				-Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -Wno-null-arithmetic
				-Wno-c23-extensions -Wno-unused-function)
		else()
			target_compile_options(${target} PRIVATE -march=x86-64-v2 -fno-strict-aliasing
				-Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -Wno-null-arithmetic
				-Wno-c23-extensions -Wno-unused-function)
		endif()
	endif()
endfunction()
qaa_guest_code_options(qaa_guest)

# A hand-written module in the recompiler's output style: no retail data.
add_library(qaa_synthetic_module SHARED tests/synthetic/synthetic_module.cpp)
target_include_directories(qaa_synthetic_module PRIVATE
	${QAA_GUEST_DIR}/include
	${QAA_GUEST_DIR}/module
	${QAA_GUEST_DIR}/tests/synthetic
	${QAA_XENONRECOMP_SRC}/XenonUtils
	${QAA_XENONRECOMP_SRC}/thirdparty/simde
)
target_compile_definitions(qaa_synthetic_module PRIVATE PPC_INCLUDE_DETAIL)
target_compile_features(qaa_synthetic_module PRIVATE cxx_std_20)
qaa_guest_code_options(qaa_synthetic_module)

add_executable(qaa_guest_tests tests/test_guest.cpp)
target_link_libraries(qaa_guest_tests PRIVATE qaa_guest)
target_compile_definitions(qaa_guest_tests PRIVATE
	QAA_SYNTHETIC_MODULE="$<TARGET_FILE:qaa_synthetic_module>")
add_dependencies(qaa_guest_tests qaa_synthetic_module)

# The M3 gate: load recompiled modules, call dllEntry and vmMain(INIT).
add_executable(qaa_gate tests/gate.cpp)
target_link_libraries(qaa_gate PRIVATE qaa_guest)
target_compile_definitions(qaa_gate PRIVATE _CRT_SECURE_NO_WARNINGS
	QAA_ABI_DIR="${QAA_GUEST_DIR}/../../data/abi")
