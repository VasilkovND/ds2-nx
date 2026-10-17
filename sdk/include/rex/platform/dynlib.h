/**
 * @file        platform/dynlib.h
 * @brief       Platform-agnostic dynamic library loading
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

#include <rex/platform.h>

namespace rex::platform {

enum class SymbolResolution {
  // Resolve symbols on first use. Maps to RTLD_LAZY on POSIX; the only mode on
  // Windows.
  kLazy,
  // Resolve all symbols at load time. Load fails if any unresolved symbol
  // exists. Maps to RTLD_NOW on POSIX; the only mode on Windows.
  kImmediate,
};

class DynamicLibrary {
 public:
  DynamicLibrary() = default;
  ~DynamicLibrary();

  DynamicLibrary(const DynamicLibrary&) = delete;
  DynamicLibrary& operator=(const DynamicLibrary&) = delete;
  DynamicLibrary(DynamicLibrary&& other) noexcept;
  DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

  bool Load(const std::filesystem::path& path, SymbolResolution mode = SymbolResolution::kLazy);
  void Close();
  explicit operator bool() const { return handle_ != nullptr; }

  void* GetRawSymbol(const char* name) const;

  template <typename T>
  T GetSymbol(const char* name) const {
    return reinterpret_cast<T>(GetRawSymbol(name));
  }

 private:
  void* handle_ = nullptr;
};

#if REX_PLATFORM_SWITCH
// A library linked into the NRO in place of a shared library (a recompiled guest DLL, see
// rexglue_switch_link_module in rexglue_switch.cmake). Once registered, DynamicLibrary::Load
// finds it by name and GetRawSymbol returns the addresses given here. Registration runs from
// static constructors, before main.
struct StaticLibrarySymbol {
  const char* name;
  void* address;
};
void RegisterStaticLibrary(const char* name, const StaticLibrarySymbol* symbols, size_t count);
#endif

namespace lib_names {

#if REX_PLATFORM_WIN32

inline constexpr const char* kVulkanLoader = "vulkan-1.dll";
inline constexpr const char* kRenderDoc = "renderdoc.dll";
inline constexpr const char* kSpirvToolsSdkPath = "Bin/SPIRV-Tools-shared.dll";

#elif REX_PLATFORM_SWITCH

// There is no dynamic loading on Switch: everything is linked statically.
inline constexpr const char* kVulkanLoader = "";
inline constexpr const char* kRenderDoc = "";
inline constexpr const char* kSpirvToolsSdkPath = "";

#elif REX_PLATFORM_ANDROID

inline constexpr const char* kVulkanLoader = "libvulkan.so";
inline constexpr const char* kRenderDoc = "libVkLayer_GLES_RenderDoc.so";
inline constexpr const char* kSpirvToolsSdkPath = "bin/libSPIRV-Tools-shared.so";

#elif REX_PLATFORM_LINUX

inline constexpr const char* kVulkanLoader = "libvulkan.so.1";
inline constexpr const char* kRenderDoc = "librenderdoc.so";
inline constexpr const char* kSpirvToolsSdkPath = "bin/libSPIRV-Tools-shared.so";

#elif REX_PLATFORM_MAC

inline constexpr const char* kVulkanLoader = "libvulkan.1.dylib";
inline constexpr const char* kRenderDoc = "librenderdoc.dylib";
inline constexpr const char* kSpirvToolsSdkPath = "lib/libSPIRV-Tools-shared.dylib";

#else
#error No library names provided for the target platform.
#endif

}  // namespace lib_names

}  // namespace rex::platform
