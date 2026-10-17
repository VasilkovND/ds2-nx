// halo3 - first launch from a disc image.
//
// Put the NRO and the game's .iso in the same folder: the first start copies the disc into game/ beside them
// (about 7 GB; the disc's $SystemUpdate is left out), and every start after that runs from game/. On the
// Switch the image cannot be read in place: the SDK's disc reader maps the whole file, and 7 GB does not fit.
// The idea and the flow are haloce-nx's (CC0), which unpacks its Xbox disc the same way on first start.
//
// The copy can be interrupted: game/.extracting stays until it finishes, and the next start carries on,
// skipping the files already copied. The .iso is never deleted; once game/ is complete it can be removed.
//
// Progress is shown with libnx's text console. This runs before the game's window opens (OnConfigurePaths),
// so the console has the screen to itself and is closed before the window takes it. When the deko3d
// presenter is in, this belongs on the ImGui overlay from OnFinalizePaths instead.

#include "halo3_first_launch.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <string_view>
#include <system_error>
#include <vector>

#include <rex/filesystem/disc_image_extract.h>

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace halo3::first_launch {
namespace {

std::string g_summary;

std::filesystem::path FindImage(const std::filesystem::path& folder) {
  std::error_code ec;
  std::vector<std::filesystem::path> images;
  for (const auto& e : std::filesystem::directory_iterator(folder, ec)) {
    std::string ext = e.path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if ((ext == ".iso" || ext == ".xiso") && e.is_regular_file(ec)) {
      images.push_back(e.path());
    }
  }
  return images.empty() ? std::filesystem::path{} : *std::min_element(images.begin(), images.end());
}

// The screen while copying: libnx's console on the Switch, the terminal elsewhere.
class Screen {
 public:
  Screen() {
#if defined(__SWITCH__)
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad_);
#endif
  }
  ~Screen() {
#if defined(__SWITCH__)
    consoleExit(nullptr);
#endif
  }
  void Print(const char* text) {
    std::fputs(text, stdout);
    std::fflush(stdout);
#if defined(__SWITCH__)
    consoleUpdate(nullptr);
#endif
  }
  // Leaves a failure on screen until a button is pressed, instead of the program closing on a black screen.
  void WaitForButton() {
#if defined(__SWITCH__)
    Print("\n\nPress A to close.\n");
    while (appletMainLoop()) {
      padUpdate(&pad_);
      if (padGetButtonsDown(&pad_) & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_Plus)) {
        break;
      }
      svcSleepThread(16'666'667);
    }
#endif
  }

 private:
#if defined(__SWITCH__)
  PadState pad_{};
#endif
};

}  // namespace

const std::string& Summary() { return g_summary; }

std::filesystem::path ExtractIfNeeded(const std::filesystem::path& folder) {
  const auto game = folder / "game";
  std::error_code ec;
  if (std::filesystem::is_regular_file(game / "default.xex", ec) &&
      !rex::filesystem::DiscExtractionUnfinished(game)) {
    return {};  // already there
  }
  const auto image = FindImage(folder);
  if (image.empty()) {
    return {};
  }
  const bool resuming = rex::filesystem::DiscExtractionUnfinished(game);

  Screen screen;
  char line[512];
  std::snprintf(line, sizeof(line),
                "\x1b[2J\x1b[1;1HHALO 3: UNPACKING THE GAME\n\n"
                "%s the game out of %s into game/.\nThis only happens once (about 7 GB).\n\n",
                resuming ? "Carrying on copying" : "Copying", image.filename().string().c_str());
  screen.Print(line);

  static constexpr std::string_view kSkip[] = {"$SystemUpdate"};
  const auto started = std::chrono::steady_clock::now();
  rex::filesystem::DiscExtractOptions options;
  options.skip = kSkip;
  options.progress = [&](const rex::filesystem::DiscExtractProgress& p) {
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const double rate = seconds > 1 ? double(p.done) / seconds : 0;  // includes skipped files on a resume
    const double left = rate > 0 ? double(p.total - p.done) / rate : 0;
    std::snprintf(line, sizeof(line), "\x1b[7;1H%-60.60s\n%6.2f of %.2f GB  (%3d %%)   %s%-12s\n",
                  std::string(p.file).c_str(), double(p.done) / 1e9, double(p.total) / 1e9,
                  p.total ? int(p.done * 100 / p.total) : 0, rate > 0 ? "about " : "",
                  rate > 0 ? (std::to_string(int(left / 60) + 1) + " min left").c_str() : "");
    screen.Print(line);
    return true;
  };
  std::string error;
  if (!rex::filesystem::ExtractDiscImage(image, game, options, &error)) {
    g_summary = "could not unpack " + image.filename().string() + ": " + error;
    std::snprintf(line, sizeof(line), "\n\nThe game could not be copied:\n%s\n", error.c_str());
    screen.Print(line);
    screen.WaitForButton();
    return {};
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  g_summary = "unpacked " + image.filename().string() + " into " + game.string() + " in " +
              std::to_string(int(seconds)) + " s";
  return game;
}

}  // namespace halo3::first_launch
