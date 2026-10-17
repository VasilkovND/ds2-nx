/**
 * @file        rex/core/dynlib_switch.cpp
 * @brief       DynamicLibrary backend for Horizon: there is no dynamic loading
 *
 * An NRO cannot load libraries at run time: dlopen does not exist and everything is
 * linked statically. Load() finds only the libraries registered with
 * RegisterStaticLibrary, which is how recompiled guest DLLs reach the kernel's
 * module loader (kernel_state.cpp): rexglue_switch_link_module links each one into
 * the NRO and registers its two entry points under the library's name.
 *
 * For any other path Load() returns "not loaded", which the SDK's other callers
 * already handle:
 *
 *   - gpu_plugin_loader.cpp, which on Switch must be resolved through static
 *     registration (see rex::system::LoadGpuPlugin)
 *   - the optional RenderDoc and SPIRV-Tools loaders, which make no sense on the
 *     console
 *
 * If another caller that depends on dlopen is ever added, it will show up here as a
 * Load() that returns false, not as a link error.
 */

#include <cstring>
#include <string>
#include <vector>

#include <rex/platform.h>
#include <rex/platform/dynlib.h>

static_assert(REX_PLATFORM_SWITCH, "This file is Nintendo Switch-only");

namespace rex::platform {

namespace {

struct StaticLibrary {
  const char* name;
  const StaticLibrarySymbol* symbols;
  size_t count;
};

// Function-local: registration runs from other files' static constructors.
std::vector<StaticLibrary>& StaticLibraries() {
  static std::vector<StaticLibrary> libraries;
  return libraries;
}

}  // namespace

void RegisterStaticLibrary(const char* name, const StaticLibrarySymbol* symbols, size_t count) {
  StaticLibraries().push_back({name, symbols, count});
}

DynamicLibrary::~DynamicLibrary() {
  Close();
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept : handle_(other.handle_) {
  other.handle_ = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
  if (this != &other) {
    Close();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

bool DynamicLibrary::Load(const std::filesystem::path& path, SymbolResolution mode) {
  (void)mode;
  Close();
  // Matched by stem, so "halo3_L360", "libhalo3_L360.so" and a full path all find "halo3_L360".
  std::string stem = path.stem().string();
  if (stem.rfind("lib", 0) == 0) {
    stem.erase(0, 3);
  }
  for (auto& library : StaticLibraries()) {
    if (stem == library.name) {
      handle_ = &library;
      return true;
    }
  }
  return false;
}

void DynamicLibrary::Close() {
  handle_ = nullptr;
}

void* DynamicLibrary::GetRawSymbol(const char* name) const {
  auto* library = static_cast<const StaticLibrary*>(handle_);
  if (!library) {
    return nullptr;
  }
  for (size_t i = 0; i < library->count; ++i) {
    if (std::strcmp(library->symbols[i].name, name) == 0) {
      return library->symbols[i].address;
    }
  }
  return nullptr;
}

}  // namespace rex::platform
