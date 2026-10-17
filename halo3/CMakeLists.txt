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
