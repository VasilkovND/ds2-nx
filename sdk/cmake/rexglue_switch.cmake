#==========================================================
# rexglue_switch.cmake - Nintendo Switch (devkitA64 + libnx)
#
#   rexglue_switch_configure_target(<target> [GPU_PLUGINS ...])
#       what rexglue_configure_target does on Switch
#   rexglue_switch_add_nro(<target> [NAME ..] [AUTHOR ..] [VERSION ..] [ICON ..])
#       packages <target>.nro next to the executable
#
# Requires the switch-devkitA64.cmake toolchain (DEVKITPRO, compiler, specs) and
# REXGLUE_SWITCH_NVK_SDK pointing at the Mesa Horizon SDK. With
# REXGLUE_SWITCH_DEKO3D the title brings a deko3d renderer instead: no Mesa SDK,
# no GPU plugin, and deko3d (devkitPro's package) is linked.
#==========================================================
include_guard(GLOBAL)

set(REXGLUE_SWITCH_NVK_SDK "" CACHE PATH
    "portlibs/switch directory of the Mesa Horizon SDK (static NVK driver)")
option(REXGLUE_SWITCH_DEKO3D_DEBUG
    "deko3d build: link deko3d's debug library (libdeko3dd: every call checked, readable errors)" OFF)
if(REXGLUE_SWITCH_DEKO3D)
    # The GPU library of the title: deko3d. rex::switch_gpu is what the title links.
    if(NOT TARGET rex::switch_gpu)
        add_library(rexglue_switch_gpu INTERFACE)
        add_library(rex::switch_gpu ALIAS rexglue_switch_gpu)
        if(REXGLUE_SWITCH_DEKO3D_DEBUG)
            set(_deko3d deko3dd)
        else()
            set(_deko3d deko3d)
        endif()
        target_link_directories(rexglue_switch_gpu INTERFACE "${DEVKITPRO}/libnx/lib")
        target_link_libraries(rexglue_switch_gpu INTERFACE ${_deko3d} nx stdc++ m)
        target_link_options(rexglue_switch_gpu INTERFACE -pthread)
    endif()
elseif(NOT EXISTS "${REXGLUE_SWITCH_NVK_SDK}/lib/libvulkan.a")
    message(FATAL_ERROR
        "REXGLUE_SWITCH_NVK_SDK must point at <mesa-...-horizon-sdk>/opt/devkitpro/portlibs/switch "
        "(no lib/libvulkan.a under '${REXGLUE_SWITCH_NVK_SDK}')")
endif()

# Static NVK. Not find_package(Vulkan): the Mesa SDK's config requires libelf,
# which devkitPro does not package. switch_elf_stubs.c satisfies the linker
# instead; the NVIDIA CUBIN parser that uses libelf never runs on Switch.
# Listed twice for archive rescanning, as the Mesa config does without
# LINK_GROUP support.
if(NOT REXGLUE_SWITCH_DEKO3D AND NOT TARGET rex::nvk)
    add_library(rexglue_switch_nvk INTERFACE)
    add_library(rex::nvk ALIAS rexglue_switch_nvk)
    add_library(rex::switch_gpu ALIAS rexglue_switch_nvk)
    target_link_directories(rexglue_switch_nvk INTERFACE
        "${REXGLUE_SWITCH_NVK_SDK}/lib"
        "${DEVKITPRO}/portlibs/switch/lib"
        "${DEVKITPRO}/libnx/lib")
    target_link_libraries(rexglue_switch_nvk INTERFACE
        vulkan expat zstd z nx
        vulkan expat zstd z nx
        stdc++ m)
    target_link_options(rexglue_switch_nvk INTERFACE -pthread)
endif()

# Startup objects every title needs, built here, in the SDK's scope, where C is
# enabled: a title project may declare CXX only, and CMake cannot compile a C
# source added to a target of that directory. Linked as an OBJECT library, the
# objects reach the executable directly, never through an archive:
#   runtime_switch.cpp  libnx startup constants (applet type, heap, exception
#                       stack); they override weak libnx symbols
#   switch_elf_stubs.c  libelf stubs for Mesa's NVK
#   switch_crash_hooks.c  abort()/exit() reports and stderr to the SD; abort
#                       must be defined before libc.a is searched
if(NOT TARGET rexglue_switch_startup)
    if(REXGLUE_SWITCH_DEKO3D)
        set(_switch_elf_stubs)  # only Mesa needs them
    else()
        set(_switch_elf_stubs "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_elf_stubs.c")
    endif()
    add_library(rexglue_switch_startup OBJECT
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/runtime_switch.cpp"
        ${_switch_elf_stubs}
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_crash_hooks.c"
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_perf.cpp"
        # FPS and resolution for the console overlays (SaltyNX) and Reverse-NX reading.
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_saltynx.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_sysclk.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_apm.cpp")
    # switch_perf.cpp registers every thread (the profiler needs their handles)
    # by wrapping libnx's threadCreate/threadClose, which pthread goes through.
    # It also counts the libnx calls that cost IPC or walk memory (fences,
    # submissions, NvMap, GPU addresses and mappings, armDCacheClean, the window
    # queue and svcSleepThread) and the time the calling threads spend inside.
    target_link_options(rexglue_switch_startup INTERFACE
        "LINKER:--wrap=threadCreate,--wrap=threadClose"
        "LINKER:--wrap=nvFenceWait,--wrap=nvGpuChannelKickoff,--wrap=nvMapCreate"
        "LINKER:--wrap=nvAddressSpaceAllocFixed,--wrap=nvioctlNvhostAsGpu_MapBufferEx"
        "LINKER:--wrap=armDCacheClean,--wrap=nwindowQueueBuffer,--wrap=bqDequeueBuffer"
        "LINKER:--wrap=svcSleepThread")
endif()

function(rexglue_switch_configure_target target_name)
    cmake_parse_arguments(ARG "" "" "GPU_PLUGINS" ${ARGN})

    target_sources(${target_name} PRIVATE
        ${REXGLUE_SHARE_DIR}/windowed_app_main_switch.cpp
        ${REXGLUE_SHARE_DIR}/rex_app.cpp)
    target_link_libraries(${target_name} PRIVATE rexglue_switch_startup)

    target_compile_definitions(${target_name} PRIVATE
        REXGLUE_BUILD_CONFIG="$<CONFIG>")
    # The SDK headers the title includes use C++23 (std::byteswap in rex/types.h).
    target_compile_features(${target_name} PRIVATE cxx_std_23)

    # Every object of the runtime and of the GPU plugin goes into the title, as
    # the object files of the SDL build would. From an archive the linker only
    # pulls members that resolve an undefined symbol, and several do nothing
    # but override a weak libnx symbol (__libnx_exception_handler, which the
    # guest memory needs), register themselves from a static constructor
    # (cvars, kernel exports) or serve the plugin entry point.
    set(_whole "$<TARGET_FILE:rexruntime>")
    set(_libs rexruntime)
    foreach(_plugin IN LISTS ARG_GPU_PLUGINS)
        if(NOT TARGET rexgpu-${_plugin})
            message(FATAL_ERROR
                "rexglue_switch_configure_target: unknown GPU plugin '${_plugin}' "
                "(no target rexgpu-${_plugin})")
        endif()
        list(APPEND _whole "$<TARGET_FILE:rexgpu-${_plugin}>")
        list(APPEND _libs rexgpu-${_plugin})
    endforeach()
    string(JOIN "," _whole_csv ${_whole})
    target_link_options(${target_name} PRIVATE
        "LINKER:--whole-archive,${_whole_csv},--no-whole-archive")
    set_property(TARGET ${target_name} APPEND PROPERTY LINK_DEPENDS ${_whole})
    # Still linked normally, for the build order and their own dependencies
    # (glslang, FFmpeg, ...), which stay regular archives.
    target_link_libraries(${target_name} PRIVATE ${_libs} rex::switch_gpu)

    # rex_app.cpp includes imgui.h. rexruntime links imgui privately, so its
    # include path does not reach the title; take the path alone, since imgui's
    # objects are already inside rexruntime.
    target_include_directories(${target_name} PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endfunction()

#----------------------------------------------------------
# rexglue_switch_link_module(<module target> HOST <title target>)
#
# A guest DLL module is a shared library elsewhere: kernel_state.cpp loads it by
# name with DynamicLibrary and enters it through ReXModule_Register and
# ReXModule_GetImageInfo. An NRO has no dlopen, so the module (a static archive
# here, REXGLUE_MODULE_LIBRARY_TYPE) is linked into the title, the way the GPU
# plugin is (gpu_plugin_loader.cpp), except that every module exports the same
# two names:
#   1. ld -r merges the module's objects into one relocatable object;
#   2. objcopy makes its hidden symbols local (modules build with hidden
#      visibility, as on the PC), so modules and title cannot clash on sub_* or
#      helper names, and renames the two entry points after the module;
#   3. a generated source registers them with rex::platform::RegisterStaticLibrary
#      under the module's name, which is the name kernel_state.cpp loads.
#----------------------------------------------------------
function(rexglue_switch_link_module target_name)
    cmake_parse_arguments(ARG "" "HOST" "" ${ARGN})
    if(NOT ARG_HOST)
        message(FATAL_ERROR "rexglue_switch_link_module: ${target_name} needs HOST (the title target)")
    endif()
    find_program(REXGLUE_SWITCH_LD aarch64-none-elf-ld
        HINTS "${DEVKITPRO}/devkitA64/bin" NO_DEFAULT_PATH REQUIRED)
    find_program(REXGLUE_SWITCH_OBJCOPY aarch64-none-elf-objcopy
        HINTS "${DEVKITPRO}/devkitA64/bin" NO_DEFAULT_PATH REQUIRED)

    string(MAKE_C_IDENTIFIER "${target_name}" _id)
    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/rexglue_modules")
    set(_merged "${_dir}/${target_name}.merged.o")
    set(_object "${_dir}/${target_name}.o")
    set(_register "${_dir}/${target_name}_register.cpp")
    file(MAKE_DIRECTORY "${_dir}")

    add_custom_command(OUTPUT "${_object}"
        COMMAND "${REXGLUE_SWITCH_LD}" -r --whole-archive "$<TARGET_FILE:${target_name}>" -o "${_merged}"
        COMMAND "${REXGLUE_SWITCH_OBJCOPY}" --localize-hidden
            --redefine-sym "ReXModule_Register=ReXModule_Register_${_id}"
            --redefine-sym "ReXModule_GetImageInfo=ReXModule_GetImageInfo_${_id}"
            "${_merged}" "${_object}"
        DEPENDS ${target_name}
        COMMENT "Linking guest module ${target_name} into the title"
        VERBATIM)

    file(CONFIGURE OUTPUT "${_register}" @ONLY CONTENT [=[
// Generated by rexglue_switch_link_module: guest module @target_name@, linked into the NRO.
#include <rex/platform/dynlib.h>

// Renamed by objcopy from ReXModule_Register and ReXModule_GetImageInfo. Only
// their addresses are taken here; kernel_state.cpp casts them to the real types.
extern "C" void ReXModule_Register_@_id@();
extern "C" void ReXModule_GetImageInfo_@_id@();

namespace {
const rex::platform::StaticLibrarySymbol kSymbols[] = {
    {"ReXModule_Register", reinterpret_cast<void*>(&ReXModule_Register_@_id@)},
    {"ReXModule_GetImageInfo", reinterpret_cast<void*>(&ReXModule_GetImageInfo_@_id@)},
};
const bool kRegistered =
    (rex::platform::RegisterStaticLibrary("@target_name@", kSymbols, 2), true);
}  // namespace
]=])

    # The title may be declared after its modules (dll_targets.cmake comes first).
    set_source_files_properties("${_object}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    cmake_language(EVAL CODE
        "cmake_language(DEFER CALL target_sources ${ARG_HOST} PRIVATE \"${_object}\" \"${_register}\")")
endfunction()

function(rexglue_switch_add_nro target_name)
    cmake_parse_arguments(ARG "" "NAME;AUTHOR;VERSION;ICON" "" ${ARGN})
    if(NOT ARG_NAME)
        set(ARG_NAME "${target_name}")
    endif()
    if(NOT ARG_AUTHOR)
        set(ARG_AUTHOR "ReXGlue")
    endif()
    if(NOT ARG_VERSION)
        set(ARG_VERSION "${PROJECT_VERSION}")
        if(NOT ARG_VERSION)
            set(ARG_VERSION "1.0.0")
        endif()
    endif()
    if(NOT ARG_ICON)
        set(ARG_ICON "${DEVKITPRO}/libnx/default_icon.jpg")
    endif()

    find_program(REXGLUE_NACPTOOL nacptool HINTS "${DEVKITPRO}/tools/bin" NO_DEFAULT_PATH REQUIRED)
    find_program(REXGLUE_ELF2NRO elf2nro HINTS "${DEVKITPRO}/tools/bin" NO_DEFAULT_PATH REQUIRED)

    set(_dir "$<TARGET_FILE_DIR:${target_name}>")
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND "${REXGLUE_NACPTOOL}" --create "${ARG_NAME}" "${ARG_AUTHOR}" "${ARG_VERSION}"
            "${_dir}/${target_name}.nacp"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:${target_name}>"
            "${_dir}/${target_name}.stripped.elf"
        COMMAND "${CMAKE_STRIP}" --strip-all "${_dir}/${target_name}.stripped.elf"
        COMMAND "${REXGLUE_ELF2NRO}" "${_dir}/${target_name}.stripped.elf"
            "${_dir}/${target_name}.nro" "--nacp=${_dir}/${target_name}.nacp" "--icon=${ARG_ICON}"
        COMMENT "Packaging ${target_name}.nro"
        VERBATIM)
endfunction()
