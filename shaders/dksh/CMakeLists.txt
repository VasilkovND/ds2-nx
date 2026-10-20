# ds_dksh: the deko3d shader library to a DKSH cache, on the PC. Needs sdk/thirdparty with SPIRV-Cross, uam and
# xxHash (the SDK's submodules).
#   cmake -S shaders/dksh -B out/dksh -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build out/dksh
cmake_minimum_required(VERSION 3.20)
project(ds_dksh C CXX)

get_filename_component(DS_RAIZ "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(DS_TERCEROS "${DS_RAIZ}/sdk/thirdparty")
foreach(fuente SPIRV-Cross/spirv_glsl.cpp uam/uam_api.h xxHash/xxhash.h)
  if(NOT EXISTS "${DS_TERCEROS}/${fuente}")
    message(FATAL_ERROR "Missing sdk/thirdparty/${fuente}")
  endif()
endforeach()

# uam as the library uamlib (its own CMakeLists, from SwitchWakerHD)
add_subdirectory("${DS_TERCEROS}/uam" uam EXCLUDE_FROM_ALL)

# SPIRV-Cross: only the core and the GLSL backend
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_MSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${DS_TERCEROS}/SPIRV-Cross" spirv-cross EXCLUDE_FROM_ALL)

# The GLSL and DKSH cache code is the SDK's (rex::deko3d, sdk/src/ui/deko3d).
add_executable(ds_dksh
  ds_dksh.cpp
  "${DS_RAIZ}/sdk/src/ui/deko3d/glsl.cpp"
  "${DS_RAIZ}/sdk/src/ui/deko3d/cache_dksh.cpp"
  "${DS_RAIZ}/shaders/ds_shader_library.cpp")
target_include_directories(ds_dksh PRIVATE "${DS_RAIZ}/sdk/include"
                           "${DS_TERCEROS}/xxHash")
target_link_libraries(ds_dksh PRIVATE uamlib spirv-cross-glsl spirv-cross-core)
set_target_properties(ds_dksh PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
