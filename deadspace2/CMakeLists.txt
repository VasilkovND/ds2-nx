# deadspace2 - Dead Space 2 (Xbox 360, Title Update 2) on the Nintendo Switch.
#
# The recompiled game code is not made here: it is the checked-in tree of
# Dead-Space-2-Recomp (https://github.com/bifftannen1/Dead-Space-2-Recomp, src_tu2/:
# 47,739 functions, PDB-named). Point DS2_RECOMP_DIR at a checkout of it. At configure
# time tools/prepare_tree.py mirrors src_tu2/ as symlinks with a Switch PCH
# (tools/make_nx_pch.py); see PORTING.md for why.
#
# Configure (Switch):
#   cmake -S deadspace2 -B deadspace2/out/sw -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=tools/switch/cmake/switch-devkitA64.cmake \
#     -DREXSDK_DIR=$PWD/sdk -DDS2_RECOMP_DIR=/path/to/Dead-Space-2-Recomp \
#     -DREXGLUE_SWITCH_NVK_SDK=/path/to/mesa-sdk/opt/devkitpro/portlibs/switch
#   cmake --build deadspace2/out/sw -j 4
# or with -DREXGLUE_SWITCH_DEKO3D=ON instead of the NVK SDK (the minimal deko3d renderer, see PORTING.md).

cmake_minimum_required(VERSION 3.25)
project(deadspace2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(cmake/ds2_switch.cmake)

set(DS2_SOURCES
    src/main.cpp
    src/ds2_disc_swap.cpp
    src/ds2_shader_dump.cpp
    src/ds2_audio.cpp
    src/ds2_cpu.cpp
    src/ds2_resolution.cpp
    src/ds2_menu.cpp
    src/ds2_shaders.cpp
    # The shader library reader, shared with the PC tools (shaders/).
    ${CMAKE_CURRENT_SOURCE_DIR}/../shaders/ds_shader_library.cpp
)
if(REXGLUE_SWITCH_DEKO3D)
    list(APPEND DS2_SOURCES src/ds2_gpu.cpp)
endif()

if(WIN32)
    add_executable(deadspace2 WIN32 ${DS2_SOURCES})
else()
    add_executable(deadspace2 ${DS2_SOURCES})
endif()

# NVK build: ReXGlue's Xenos emulation (rexgpu-xenos) draws the game. Slow on the Switch, but it is the way to
# see the first frames. deko3d build: src/ds2_gpu.cpp stands in for the GPU.
if(REXGLUE_SWITCH_DEKO3D)
    ds2_setup_target(deadspace2)
    rexglue_switch_add_deko3d(deadspace2)
    target_compile_definitions(deadspace2 PRIVATE DS2_DEKO3D=1)
else()
    ds2_setup_target(deadspace2 GPU_PLUGINS xenos)
endif()

target_include_directories(deadspace2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shaders
                           ${REXSDK_DIR}/thirdparty/xxHash)

# The version (Homebrew Menu and the first lines of the log) and the commit it was built from, so a log a player sends
# says which build made it. The commit is read when CMake configures (build_mac.sh RECONFIGURE=1 refreshes it).
set(DS2_VERSION "1.2.5 (TU2)")
execute_process(COMMAND git rev-parse --short=10 HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                OUTPUT_VARIABLE DS2_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git status --porcelain --untracked-files=no --ignore-submodules=all WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                OUTPUT_VARIABLE DS2_SUCIO OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT DS2_COMMIT)
    set(DS2_COMMIT "unknown")
elseif(DS2_SUCIO)
    string(APPEND DS2_COMMIT "+changes")
endif()
target_compile_definitions(deadspace2 PRIVATE DS2_VERSION="${DS2_VERSION}" DS2_COMMIT="${DS2_COMMIT}")

# Disc swap, title update mount and XamSwapDisc (src/ds2_disc_swap.cpp) are TU2 code.
target_compile_definitions(deadspace2 PRIVATE DS2_TARGET_TU2=1)

# Which tools/patch_local_regs.py build this is (cmake/ds2_switch.cmake), for the log's [cpu] line.
target_compile_definitions(deadspace2 PRIVATE DS2_LOCAL_REGS="${DS2_LOCAL_REGS}")

# Release builds (the default) have no debug modes: only ZL + ZR + Up (the FPS counter) answers, whatever ds2.toml
# says, and telemetria.flag / sin_resplandor.flag next to the NRO are ignored. -DDS2_DEPURACION=ON brings them back
# (switch_atajos_depuracion=true in ds2.toml for the ZL + ZR shortcuts), for working on the port.
option(DS2_DEPURACION "Debug modes: ZL + ZR shortcuts, telemetria.flag, sin_resplandor.flag" OFF)
target_compile_definitions(deadspace2 PRIVATE DS2_DEPURACION=$<BOOL:${DS2_DEPURACION}>)

# Nintendo Switch: deadspace2.nro. On the SD card: sdmc:/switch/ds2-nx/ds2-nx.nro, with disc1/, disc2/ and tu/ beside it
# (src/ds2_app.h finds them there).
if(REXGLUE_PLATFORM_SWITCH)
    # Homebrew menu icon: ours (res/icon.jpg, tools/make_release_icon.py), with no art from the game. Your own build can
    # use the game's (Isaac's helmet from your disc's dashboard art; assets/ is not committed):
    #   python3 tools/make_icon.py <disc1/nxeart>   and configure with -DDS2_ICON_DEL_DISCO=ON
    option(DS2_ICON_DEL_DISCO "Homebrew menu icon from your own disc (assets/icon.jpg)" OFF)
    set(DS2_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/icon.jpg")
    if(DS2_ICON_DEL_DISCO AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.jpg")
        set(DS2_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.jpg")
    endif()
    set(DS2_ICON_ARG ICON "${DS2_ICON}")
    rexglue_switch_add_nro(deadspace2
        NAME "Dead Space 2"
        AUTHOR "VasilkovND"
        VERSION "${DS2_VERSION}"
        ${DS2_ICON_ARG})
endif()
