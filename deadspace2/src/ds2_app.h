// deadspace2 - Dead Space 2 (Xbox 360, TU2) on the Nintendo Switch.
//
// Based on Dead-Space-2-Recomp's src/app/ds2_app.h (PC) and halo3/src/halo3_app.h (Switch).
//
// Where the game is, next to the NRO (sdmc:/switch/ds2-nx/ on the console):
//   tu/       default.xex of disc 1 and default.xexp of Title Update 2   (game:, and game_data_root)
//   disc1/    the extracted Disc 1 (default.xex, bigfile0.viv, bigfile1.viv, disc1_info.txt, ...)
//   disc2/    the extracted Disc 2 (optional until chapter 4)
// tools/stage_sd.py makes this layout from your ISOs and title update.
//
// The recompiled code is TU2: it only runs with the TU2-patched image, so tu/default.xexp is required.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include "ds2_disc_swap.h"
#if DS2_DEKO3D
#include "ds2_gpu.h"
#endif

REXCVAR_DEFINE_STRING(ds2_disc1, "", "DS2", "Extracted Disc 1 folder. Empty: disc1/ next to the executable.");
REXCVAR_DEFINE_STRING(ds2_disc2, "", "DS2", "Extracted Disc 2 folder. Empty: disc2/ next to the executable.");

class Ds2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Ds2App>(new Ds2App(ctx, "ds2", PPCImageConfig));
  }

 protected:
#if DS2_DEKO3D
  // The deko3d build has no Xenos emulation plugin: ds2_gpu.cpp answers the game's Direct3D (ring, interrupts,
  // GPU writes to memory) so it runs past its first frame. Nothing is drawn yet.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (!config.graphics) config.graphics = ds2::gpu::CreateGraphicsSystem();
  }
#endif

  // game_data_root is the folder with default.xex (ConstructRuntime checks it): tu/, where the title update is.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) return;
    std::error_code ec;
    const auto folder = rex::filesystem::GetExecutableFolder();
    if (folder.empty()) return;
    for (const auto& candidate : {folder / "tu", folder / "game"}) {
      if (std::filesystem::is_regular_file(candidate / "default.xex", ec)) {
        paths.game_data_root = candidate;
        return;
      }
    }
  }

  void OnPostInitLogging() override {
#if !DS2_DEKO3D
    // NVK build: ReXGlue's Xenos emulation draws the game.
    DefaultFlag("gpu_plugin", "xenos");
#endif
  }

  // After Runtime::Setup (the VFS exists), before LoadXexImage.
  void OnLoadXexImage(std::string& xex_image) override {
    const auto folder = rex::filesystem::GetExecutableFolder();
    auto pick = [&](const std::string& flag, const char* name) {
      return flag.empty() ? folder / name : std::filesystem::path(flag);
    };
    try {
      ds2::disc_swap::Prepare(*runtime(), game_data_root(), pick(REXCVAR_GET(ds2_disc1), "disc1"),
                              pick(REXCVAR_GET(ds2_disc2), "disc2"));
    } catch (const std::exception& error) {
      Fail(error.what());
    }
    REXLOG_INFO("ds2: TU2 at {}; loading {}", game_data_root().string(), xex_image);
  }

  void OnPreLaunchModule() override {
    try {
      ds2::disc_swap::RegisterImport(*runtime());
    } catch (const std::exception& error) {
      Fail(error.what());
    }
  }

 private:
  static void DefaultFlag(const char* name, const char* value) {
    if (!rex::cvar::GetFlagInfo(name) || rex::cvar::GetFlagSource(name) != rex::cvar::Source::kDefault) return;
    rex::cvar::SetFlagByName(name, value);
  }

  [[noreturn]] static void Fail(const std::string& msg) {
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    rex::FlushLogging();
    std::_Exit(1);
  }
};
