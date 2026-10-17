/**
 * @file        filesystem/disc_image_extract.h
 * @brief       Copies the files of an Xbox 360 disc image into a host folder
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace rex::filesystem {

// Where an extraction is, for a progress display.
struct DiscExtractProgress {
  std::string_view file;  // disc path of the file being copied ("maps/mainmenu.map")
  uint64_t done = 0;      // bytes copied so far, all files
  uint64_t total = 0;     // bytes to copy, all files
};

struct DiscExtractOptions {
  // Top-level disc entries to leave out (compared without case), e.g. "$SystemUpdate".
  std::span<const std::string_view> skip;
  // Called every few megabytes and at each new file. Returning false stops the extraction.
  std::function<bool(const DiscExtractProgress&)> progress;
  // Only list: progress is called once per file with done = 0, nothing is written.
  bool list_only = false;
};

// Copies every file of an Xbox 360 disc image (GDFX, the same format DiscImageDevice reads) into `destination`,
// keeping its folders. The image is read in pieces, never mapped whole: on the Switch a 7 GB image does not fit
// in memory, which is why DiscImageDevice cannot be used there.
//
// Resumable: `destination/.extracting` exists while it runs and is removed at the end, and a file that is
// already there with the right size is not copied again. A folder without the marker is complete.
//
// Returns false with `error` set on failure or when `progress` asked to stop.
bool ExtractDiscImage(const std::filesystem::path& image, const std::filesystem::path& destination,
                      const DiscExtractOptions& options, std::string* error);

// True while an extraction into `destination` has not finished (its marker is there).
bool DiscExtractionUnfinished(const std::filesystem::path& destination);

}  // namespace rex::filesystem
