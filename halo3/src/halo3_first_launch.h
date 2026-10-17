// halo3 - first launch from a disc image: copies the game out of an .iso beside the NRO (see
// halo3_first_launch.cpp).

#pragma once

#include <filesystem>
#include <string>

namespace halo3::first_launch {

// When `folder` has no complete game in `folder/game` but holds an .iso, copies the disc into `folder/game`
// with its progress on screen, and returns that folder. Returns an empty path when there was nothing to do
// or the copy failed (the reason is then in Summary()).
std::filesystem::path ExtractIfNeeded(const std::filesystem::path& folder);

// What happened, for the log (logging starts after the paths are set).
const std::string& Summary();

}  // namespace halo3::first_launch
