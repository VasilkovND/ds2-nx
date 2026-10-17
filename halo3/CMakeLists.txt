# halo3 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(halo3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(HALO3_SOURCES
    src/main.cpp
)

if(WIN32)
    add_executable(halo3 WIN32 ${HALO3_SOURCES})
else()
    add_executable(halo3 ${HALO3_SOURCES})
endif()

rexglue_setup_target(halo3)

# Nintendo Switch: halo3.nro next to the ELF. On the SD card it goes in sdmc:/halo3-nx/ as halo3-nx.nro, beside
# the extracted disc (default.xex and its folders); src/halo3_app.h finds the game there.
# Built with -DREXGLUE_SWITCH_DEKO3D=ON: no Xenos emulation plugin. Until the deko3d renderer is in, the game
# runs without graphics (GPU work is dropped) and its progress is in the log.
if(REXGLUE_PLATFORM_SWITCH)
    set(HALO3_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../tools/switch/icono/halo3_icono.jpg")
    set(HALO3_ICON_ARG)
    if(EXISTS "${HALO3_ICON}")
        set(HALO3_ICON_ARG ICON "${HALO3_ICON}")
    endif()
    rexglue_switch_add_nro(halo3
        NAME "Halo 3"
        AUTHOR "nx-mod"
        VERSION "0.1.0"
        ${HALO3_ICON_ARG})
endif()
