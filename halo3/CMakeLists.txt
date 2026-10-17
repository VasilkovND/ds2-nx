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
    src/halo3_gpu.cpp
    src/halo3_hooks.cpp
    src/halo3_first_launch.cpp
)

if(WIN32)
    add_executable(halo3 WIN32 ${HALO3_SOURCES})
else()
    add_executable(halo3 ${HALO3_SOURCES})
endif()

rexglue_setup_target(halo3)

# deko3d (the Switch build): the SDK's shared presentation layer (sdk/src/ui/deko3d: device, swapchain, presenter,
# ImGui overlay, the uam compile worker). The game's frames come with the renderer.
if(REXGLUE_SWITCH_DEKO3D)
    rexglue_switch_add_deko3d(halo3)
    target_compile_definitions(halo3 PRIVATE HALO3_DEKO3D=1)
endif()

# Nintendo Switch: halo3.nro next to the ELF. On the SD card it goes in sdmc:/halo3-nx/ as halo3-nx.nro, beside
# the extracted disc (default.xex and its folders); src/halo3_app.h finds the game there.
# Built with -DREXGLUE_SWITCH_DEKO3D=ON: no Xenos emulation plugin. src/halo3_gpu.cpp stands in for the GPU;
# until the deko3d renderer is in, nothing is drawn and the game's progress is in the log.
if(REXGLUE_PLATFORM_SWITCH)
    # Homebrew menu icon: the game's own title image as a 256x256 JPEG, made from your disc (assets/ is ignored):
    #   rexglue init --project-name x --xex-path X --project-root <tmp> achievements X assets   (writes assets/icons/title.png)
    # then scale icons/title.png to assets/icon.jpg. Without it the NRO gets libnx's default icon.
    set(HALO3_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.jpg")
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
