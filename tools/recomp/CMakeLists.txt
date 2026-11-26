# Native build of the recompiled modules, driven by tools/recomp/build.py:
#   QAA_XENONRECOMP_SRC  patched XenonRecomp source (build/recomp/xenonrecomp/src)
#   QAA_GEN_DIR          generated output root (build/recomp): <module>/ppc, <module>/glue
#   QAA_MODULES          module names (qagame;cgame)
cmake_minimum_required(VERSION 3.20)
project(qaa_recomp CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	message(FATAL_ERROR "XenonRecomp output needs clang (clang-cl on Windows)")
endif()

set(QAA_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_subdirectory(${QAA_REPO_ROOT}/code/guest guest)

foreach(mod IN LISTS QAA_MODULES)
	set(gen ${QAA_GEN_DIR}/${mod})
	file(GLOB recomp_sources CONFIGURE_DEPENDS ${gen}/ppc/ppc_recomp.*.cpp)
	add_library(qaa_${mod} SHARED
		${recomp_sources}
		${gen}/ppc/ppc_func_mapping.cpp
		${gen}/glue/qaa_module_glue.cpp
	)
	target_include_directories(qaa_${mod} PRIVATE
		${gen}/ppc
		${gen}/glue
		${QAA_REPO_ROOT}/code/guest/module
		${QAA_REPO_ROOT}/code/guest/include
		${QAA_XENONRECOMP_SRC}/thirdparty/simde
	)
	target_compile_definitions(qaa_${mod} PRIVATE PPC_INCLUDE_DETAIL)
	qaa_guest_code_options(qaa_${mod})
	set_source_files_properties(${gen}/glue/qaa_module_glue.cpp PROPERTIES OBJECT_DEPENDS "${gen}/glue/image.bin;${gen}/glue/xex_header.bin")
endforeach()
