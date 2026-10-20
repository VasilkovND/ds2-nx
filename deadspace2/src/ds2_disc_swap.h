// deadspace2 - the two discs and the title update, mounted from extracted folders (ds2_disc_swap.cpp).
#pragma once

#include <filesystem>

namespace rex { class Runtime; }

namespace ds2::disc_swap {
// Mounts disc 1 and (when present) disc 2 as folders, and the folder holding the title update
// (default.xex of disc 1 + default.xexp) as game:. d: and \Device\Cdrom0 point at the disc in the drive.
void Prepare(rex::Runtime& runtime, const std::filesystem::path& update_directory,
             const std::filesystem::path& disc1, const std::filesystem::path& disc2);
// Binds the XEX's XamSwapDisc import to the swap handler. Call before the module launches.
void RegisterImport(rex::Runtime& runtime);
}  // namespace ds2::disc_swap
