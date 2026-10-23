# ds2_switch.cmake - what the codegen-emitted rexglue.cmake gives a title, for a title whose
# recompiled code is checked in elsewhere (Dead-Space-2-Recomp's src_tu2/) instead of generated.
#
#   DS2_RECOMP_DIR   checkout of https://github.com/bifftannen1/Dead-Space-2-Recomp
#   REXSDK_DIR       this repository's sdk/ (rexglue-sdk-nx)
#
# The tree is used as it is, with one exception: its PCH. src_tu2's files include
# "ds2_rex_diagnostic_pch.h" with quotes, which finds the copy next to them first, so the
# build compiles a mirror of symlinks (tools/prepare_tree.py) whose PCH is the Switch one
# (tools/make_nx_pch.py: GCC's DEFINE_REX_FUNC and REX_RAW_ADDR_W).

set(DS2_RECOMP_DIR "" CACHE PATH "Checkout of Dead-Space-2-Recomp (holds src_tu2/)")
if(NOT EXISTS "${DS2_RECOMP_DIR}/src_tu2/sources.cmake")
    message(FATAL_ERROR
        "DS2_RECOMP_DIR must point at a checkout of Dead-Space-2-Recomp "
        "(no src_tu2/sources.cmake under '${DS2_RECOMP_DIR}')")
endif()

set(REXSDK_DIR "" CACHE PATH "Path to the rexglue-sdk(-nx) source tree")
if(NOT REXSDK_DIR)
    get_filename_component(REXSDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../sdk" ABSOLUTE)
endif()
add_subdirectory("${REXSDK_DIR}" rexglue-sdk)
message(STATUS "ds2: ReXGlue SDK from ${REXSDK_DIR}")

# The mirror of src_tu2 with the Switch PCH, and the guest registers in host locals (tools/patch_local_regs.py: ON,
# OFF, or CHECK for the build that verifies its analysis at run time, see the script). The pass reads the hooks and
# the native callers of guest functions from src/ and the SDK, so a change there re-runs CMake (the pass itself only
# when the hooks or the guest names they mention changed).
set(DS2_LOCAL_REGS "ON" CACHE STRING "Guest registers in host locals: ON, OFF or CHECK (tools/patch_local_regs.py)")
string(TOLOWER "${DS2_LOCAL_REGS}" _ds2_local_regs)
if(NOT _ds2_local_regs MATCHES "^(on|off|check)$")
    message(FATAL_ERROR "DS2_LOCAL_REGS must be ON, OFF or CHECK")
endif()
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
set(DS2_TREE "${CMAKE_BINARY_DIR}/src_tu2_nx")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../tools/prepare_tree.py"
            "${DS2_RECOMP_DIR}/src_tu2" "${DS2_TREE}" --local-regs ${_ds2_local_regs}
            "${CMAKE_CURRENT_LIST_DIR}/../src" "${REXSDK_DIR}/src" "${REXSDK_DIR}/include"
    RESULT_VARIABLE _ds2_tree_rc)
if(NOT _ds2_tree_rc EQUAL 0)
    message(FATAL_ERROR "ds2: tools/prepare_tree.py failed (${_ds2_tree_rc})")
endif()
message(STATUS "ds2: guest registers in host locals: ${DS2_LOCAL_REGS}")
file(GLOB _ds2_hook_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../src/*.cpp" "${CMAKE_CURRENT_LIST_DIR}/../src/*.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${DS2_RECOMP_DIR}/src_tu2/ds2_rex_diagnostic_pch.h"
    "${DS2_RECOMP_DIR}/src_tu2/sources.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/../tools/prepare_tree.py"
    "${CMAKE_CURRENT_LIST_DIR}/../tools/patch_local_regs.py"
    "${CMAKE_CURRENT_LIST_DIR}/../tools/make_nx_pch.py"
    ${_ds2_hook_sources})

set(REXGLUE_RECOMP_DEBUG_INFO "none" CACHE STRING
    "Debug info level for generated code: line-tables-only, full, or none")
set(REXGLUE_RECOMP_OPTIONS "")
if(REXGLUE_RECOMP_DEBUG_INFO STREQUAL "none")
    list(APPEND REXGLUE_RECOMP_OPTIONS $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-g0>)
elseif(REXGLUE_RECOMP_DEBUG_INFO STREQUAL "line-tables-only")
    list(APPEND REXGLUE_RECOMP_OPTIONS $<$<CXX_COMPILER_ID:Clang,AppleClang>:-gline-tables-only>)
endif()

# src_tu2/sources.cmake: 2 support TUs + 157 units. CMAKE_CURRENT_LIST_DIR there is the mirror.
include("${DS2_TREE}/sources.cmake")
set(DS2_GUEST_SOURCES ${GENERATED_SOURCES})
set_source_files_properties(${DS2_GUEST_SOURCES} PROPERTIES COMPILE_OPTIONS "${REXGLUE_RECOMP_OPTIONS}")
list(LENGTH DS2_GUEST_SOURCES _ds2_n)
message(STATUS "ds2: ${_ds2_n} recompiled sources from ${DS2_RECOMP_DIR}/src_tu2")

# Usage: ds2_setup_target(<target> [GPU_PLUGINS xenos])
macro(ds2_setup_target target_name)
    add_library(${target_name}_recomp OBJECT ${DS2_GUEST_SOURCES})
    target_include_directories(${target_name}_recomp PRIVATE "${DS2_TREE}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(${target_name}_recomp PRIVATE rex::runtime)
    rexglue_apply_target_settings(${target_name}_recomp)
    target_precompile_headers(${target_name}_recomp PRIVATE "${DS2_TREE}/ds2_rex_diagnostic_pch.h")
    target_compile_options(${target_name}_recomp PRIVATE ${REXGLUE_RECOMP_OPTIONS})
    target_link_libraries(${target_name} PRIVATE ${target_name}_recomp)
    target_include_directories(${target_name} PRIVATE "${DS2_TREE}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(${target_name} PRIVATE rex::runtime)
    rexglue_configure_target(${target_name} ${ARGN})
endmacro()
