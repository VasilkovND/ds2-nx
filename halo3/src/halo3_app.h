// halo3 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <rex/filesystem.h>
#include <rex/rex_app.h>

class Halo3App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Halo3App>(new Halo3App(ctx, "halo3",
        PPCImageConfig));
  }

  // The game's files, when --game_data_root is not given. Searched next to the
  // executable, in this order:
  //   1. a game_root/ or game/ folder with the extracted disc (sdmc:/halo3-nx/game/
  //      on the Switch). Before the folder itself: the old port left a lone
  //      default.xex beside its NRO, without the maps.
  //   2. the folder itself, if it holds default.xex
  //   3. an .iso, the first in alphabetical order
  // Left empty if none is found: the SDK then says what is missing. Runs before
  // the config file is read, as in nfsmw (see nfsmw_pal/src/nfsmw_app.h).
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) {
      return;
    }
    std::error_code ec;
    const auto folder = rex::filesystem::GetExecutableFolder();
    if (folder.empty() || !std::filesystem::is_directory(folder, ec)) {
      return;
    }
    for (const auto& candidate : {folder / "game_root", folder / "game", folder}) {
      if (std::filesystem::is_regular_file(candidate / "default.xex", ec)) {
        paths.game_data_root = candidate;
        return;
      }
    }
    std::vector<std::filesystem::path> isos;
    for (const auto& e : std::filesystem::directory_iterator(folder, ec)) {
      std::string ext = e.path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(),
                     [](unsigned char c) { return char(std::tolower(c)); });
      if (ext == ".iso" && e.is_regular_file(ec)) {
        isos.push_back(e.path());
      }
    }
    if (!isos.empty()) {
      paths.game_data_root = *std::min_element(isos.begin(), isos.end());
    }
  }
};
