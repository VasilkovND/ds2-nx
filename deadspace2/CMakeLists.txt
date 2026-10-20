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
# or with -DREXGLUE_SWITCH_DEKO3D=ON instead of the NVK SDK (no draws yet, see PORTING.md).

cmake_minimum_required(VERSION 3.25)
project(deadspace2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(cmake/ds2_switch.cmake)

set(DS2_SOURCES
    src/main.cpp
    src/ds2_disc_swap.cpp
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
# see the first frames (docs/porting-another-game.md, step 3). deko3d build: src/ds2_gpu.cpp stands in for the GPU.
if(REXGLUE_SWITCH_DEKO3D)
    ds2_setup_target(deadspace2)
    rexglue_switch_add_deko3d(deadspace2)
    target_compile_definitions(deadspace2 PRIVATE DS2_DEKO3D=1)
else()
    ds2_setup_target(deadspace2 GPU_PLUGINS xenos)
endif()

# Disc swap, title update mount and XamSwapDisc (src/ds2_disc_swap.cpp) are TU2 code.
target_compile_definitions(deadspace2 PRIVATE DS2_TARGET_TU2=1)

# Nintendo Switch: deadspace2.nro. On the SD card: sdmc:/ds2-nx/ds2-nx.nro, with disc1/, disc2/ and tu/ beside it
# (src/ds2_app.h finds them there).
if(REXGLUE_PLATFORM_SWITCH)
    # Homebrew menu icon: a 256x256 JPEG made from your own disc (assets/ is ignored).
    set(DS2_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.jpg")
    set(DS2_ICON_ARG)
    if(EXISTS "${DS2_ICON}")
        set(DS2_ICON_ARG ICON "${DS2_ICON}")
    endif()
    rexglue_switch_add_nro(deadspace2
        NAME "Dead Space 2"
        AUTHOR "nx-mod"
        VERSION "0.1.0"
        ${DS2_ICON_ARG})
endif()
