/**
 * @file        filesystem/disc_image_extract.cpp
 * @brief       Copies the files of an Xbox 360 disc image into a host folder, reading it in pieces
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * The disc format is the one DiscImageDevice reads (devices/disc_image_device.cpp, from Xenia): the game
 * partition starts at one of a few known offsets, sector 32 of it holds "MICROSOFT*XBOX*MEDIA" and the root
 * directory's sector and size, and each directory is a binary tree of entries (left, right, sector, length,
 * attributes, name). The difference is how it is read: with seeks and reads of what is needed, instead of a
 * mapping of the whole image. The first-launch flow is haloce-nx's (CC0), which unpacks its Xbox disc the same
 * way on the console.
 */

#include <rex/filesystem/disc_image_extract.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <system_error>
#include <vector>

namespace rex::filesystem {
namespace {

constexpr uint64_t kSectorSize = 2048;
constexpr uint64_t kLikelyGameOffsets[] = {0x00000000, 0x0000FB20, 0x00020600, 0x02080000, 0x0FD90000};
constexpr uint8_t kAttributeDirectory = 0x10;
constexpr uint32_t kMaxDirectoryBytes = 32 * 1024 * 1024;  // as DiscImageDevice::Verify
constexpr size_t kMaxEntries = 1 << 20;
constexpr int kMaxDepth = 32;
constexpr size_t kCopyChunk = 4 * 1024 * 1024;
constexpr uint64_t kProgressEvery = 8 * 1024 * 1024;
constexpr const char* kMarker = ".extracting";

struct FileCloser {
  void operator()(std::FILE* f) const {
    if (f) {
      std::fclose(f);
    }
  }
};
using File = std::unique_ptr<std::FILE, FileCloser>;

struct DiscFile {
  std::string path;  // with '/' separators
  uint64_t offset = 0;
  uint64_t size = 0;
};

bool ReadAt(std::FILE* f, uint64_t offset, void* buffer, size_t size) {
  if (fseeko(f, static_cast<off_t>(offset), SEEK_SET) != 0) {
    return false;
  }
  return std::fread(buffer, 1, size, f) == size;
}

uint16_t Load16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t Load32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool EqualsNoCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

class Lister {
 public:
  Lister(std::FILE* f, uint64_t game_offset, const DiscExtractOptions& options)
      : f_(f), game_offset_(game_offset), options_(options) {}

  bool Directory(uint32_t sector, uint32_t size, const std::string& prefix, int depth, std::string* error) {
    if (depth > kMaxDepth || size > kMaxDirectoryBytes) {
      *error = "a directory is nested too deep or too large: the image looks damaged";
      return false;
    }
    std::vector<uint8_t> buffer(size);
    if (!ReadAt(f_, game_offset_ + uint64_t(sector) * kSectorSize, buffer.data(), size)) {
      *error = "could not read a directory of the image (is it complete?)";
      return false;
    }
    // The tree is walked with a stack of entry ordinals (an entry is at ordinal * 4).
    std::vector<uint16_t> pending{0};
    while (!pending.empty()) {
      const size_t at = size_t(pending.back()) * 4;
      pending.pop_back();
      if (at + 14 > buffer.size() || ++entries_ > kMaxEntries) {
        *error = "a directory entry points outside its directory: the image looks damaged";
        return false;
      }
      const uint8_t* p = buffer.data() + at;
      const uint16_t left = Load16(p + 0);
      const uint16_t right = Load16(p + 2);
      const uint32_t entry_sector = Load32(p + 4);
      const uint32_t length = Load32(p + 8);
      const uint8_t attributes = p[12];
      const uint8_t name_length = p[13];
      if (at + 14 + name_length > buffer.size()) {
        *error = "a file name runs past its directory: the image looks damaged";
        return false;
      }
      const std::string name(reinterpret_cast<const char*>(p + 14), name_length);
      if (right) {
        pending.push_back(right);
      }
      if (left) {
        pending.push_back(left);
      }
      if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos ||
          name.find('\\') != std::string::npos) {
        continue;  // never write outside the destination
      }
      if (depth == 0 && std::any_of(options_.skip.begin(), options_.skip.end(),
                                    [&](std::string_view s) { return EqualsNoCase(s, name); })) {
        continue;
      }
      const std::string path = prefix.empty() ? name : prefix + "/" + name;
      if (attributes & kAttributeDirectory) {
        if (length && !Directory(entry_sector, length, path, depth + 1, error)) {
          return false;
        }
      } else {
        files.push_back({path, game_offset_ + uint64_t(entry_sector) * kSectorSize, length});
      }
    }
    return true;
  }

  std::vector<DiscFile> files;

 private:
  std::FILE* f_;
  uint64_t game_offset_;
  const DiscExtractOptions& options_;
  size_t entries_ = 0;
};

}  // namespace

bool DiscExtractionUnfinished(const std::filesystem::path& destination) {
  std::error_code ec;
  return std::filesystem::exists(destination / kMarker, ec);
}

bool ExtractDiscImage(const std::filesystem::path& image, const std::filesystem::path& destination,
                      const DiscExtractOptions& options, std::string* error) {
  std::string scratch;
  if (!error) {
    error = &scratch;
  }
  File f(std::fopen(image.string().c_str(), "rb"));
  if (!f) {
    *error = "could not open " + image.string();
    return false;
  }

  // The game partition: sector 32 holds the magic and the root directory.
  uint64_t game_offset = UINT64_MAX;
  uint8_t header[28];
  for (uint64_t offset : kLikelyGameOffsets) {
    if (ReadAt(f.get(), offset + 32 * kSectorSize, header, sizeof(header)) &&
        std::memcmp(header, "MICROSOFT*XBOX*MEDIA", 20) == 0) {
      game_offset = offset;
      break;
    }
  }
  if (game_offset == UINT64_MAX) {
    *error = image.filename().string() + " is not an Xbox 360 disc image (no MICROSOFT*XBOX*MEDIA header)";
    return false;
  }
  const uint32_t root_sector = Load32(header + 20);
  const uint32_t root_size = Load32(header + 24);
  if (root_size < 13 || root_size > kMaxDirectoryBytes) {
    *error = "the image's root directory looks damaged";
    return false;
  }

  Lister lister(f.get(), game_offset, options);
  if (!lister.Directory(root_sector, root_size, "", 0, error)) {
    return false;
  }
  auto& files = lister.files;
  // In disc order: one pass from the start of the image to its end.
  std::sort(files.begin(), files.end(),
            [](const DiscFile& a, const DiscFile& b) { return a.offset < b.offset; });
  DiscExtractProgress progress;
  for (const auto& file : files) {
    progress.total += file.size;
  }
  if (options.list_only) {
    for (const auto& file : files) {
      progress.file = file.path;
      if (options.progress && !options.progress(progress)) {
        break;
      }
    }
    return true;
  }

  std::error_code ec;
  std::filesystem::create_directories(destination, ec);
  if (ec) {
    *error = "could not create " + destination.string() + ": " + ec.message();
    return false;
  }
  { File marker(std::fopen((destination / kMarker).string().c_str(), "wb")); }

  std::vector<uint8_t> buffer(kCopyChunk);
  uint64_t next_report = 0;
  for (const auto& file : files) {
    progress.file = file.path;
    if (options.progress && !options.progress(progress)) {
      *error = "stopped";
      return false;
    }
    const std::filesystem::path target = destination / std::filesystem::path(file.path);
    if (std::filesystem::is_regular_file(target, ec) && std::filesystem::file_size(target, ec) == file.size) {
      progress.done += file.size;  // copied by an earlier, interrupted run
      continue;
    }
    std::filesystem::create_directories(target.parent_path(), ec);
    std::filesystem::path part = target;
    part += ".part";
    {
      File out(std::fopen(part.string().c_str(), "wb"));
      if (!out) {
        *error = "could not write " + part.string() + " (is the SD card full?)";
        return false;
      }
      if (fseeko(f.get(), static_cast<off_t>(file.offset), SEEK_SET) != 0) {
        *error = "could not seek to " + file.path + " in the image";
        return false;
      }
      uint64_t left = file.size;
      while (left) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(left, buffer.size()));
        if (std::fread(buffer.data(), 1, n, f.get()) != n) {
          *error = "could not read " + file.path + " from the image (is it complete?)";
          return false;
        }
        if (std::fwrite(buffer.data(), 1, n, out.get()) != n) {
          *error = "could not write " + file.path + " (is the SD card full?)";
          return false;
        }
        left -= n;
        progress.done += n;
        if (progress.done >= next_report) {
          next_report = progress.done + kProgressEvery;
          if (options.progress && !options.progress(progress)) {
            *error = "stopped";
            return false;
          }
        }
      }
    }
    std::filesystem::remove(target, ec);
    std::filesystem::rename(part, target, ec);
    if (ec) {
      *error = "could not rename " + part.string() + ": " + ec.message();
      return false;
    }
  }
  std::filesystem::remove(destination / kMarker, ec);
  return true;
}

}  // namespace rex::filesystem
