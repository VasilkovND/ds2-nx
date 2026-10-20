// deadspace2 - discs and title update on the Switch.
//
// Adapted from Dead-Space-2-Recomp's src/app/ds2_disc_swap.cpp (the PC build). What changed:
// the PC build mounts the disc ISOs with DiscImageDevice, which maps the whole image in memory;
// the Switch cannot (see sdk/include/rex/filesystem/disc_image_extract.h), so each disc is an
// extracted folder mounted with HostPathDevice. Both bigfiles are under 4 GB, so FAT32 SD cards work.
//
// Layout the game sees, as on the PC build:
//   game:                 the title update folder: disc 1's default.xex + TU2's default.xexp
//   d:, \Device\Cdrom0    the disc in the drive (disc 1 or 2), switched by XamSwapDisc
//
// The game asks for disc 2 at chapter 4 (disc1_info.txt / disc2_info.txt list the chapters). With TU2,
// disc 2's executable patches to the same image as disc 1's, so one recompiled build runs both.

#include "ds2_rex_diagnostic_pch.h"
#include "ds2_disc_swap.h"

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xex2_info.h>
#include <rex/system/xevent.h>
#include <rex/thread.h>

#include <array>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

using rex::X_STATUS;  // SDK status macros use this unqualified typedef.

namespace ds2::disc_swap {
namespace {
using namespace rex::filesystem;

constexpr std::array<const char*, 2> kDevices{"\\Device\\Ds2Disc1", "\\Device\\Ds2Disc2"};
constexpr std::array<const char*, 2> kAliases{"d:", "\\Device\\Cdrom0"};
constexpr const char* kUpdate = "\\Device\\Ds2TitleUpdate";
constexpr uint32_t kTitleId = 0x454108DF;

VirtualFileSystem* fs = nullptr;
std::array<Entry*, 2> roots{};
Entry* update_root = nullptr;
unsigned current_disc = 0;

struct CloseFile {
  void operator()(File* f) const {
    if (f) f->Destroy();
  }
};

template <typename T>
T Read(File& file, size_t offset) {
  T value{};
  const size_t size = file.entry()->size();
  if (offset > size || sizeof(T) > size - offset) throw std::runtime_error("Truncated disc XEX header");
  size_t count = 0;
  if (file.ReadSync({reinterpret_cast<uint8_t*>(&value), sizeof(T)}, offset, &count) != X_STATUS_SUCCESS ||
      count != sizeof(T))
    throw std::runtime_error("Cannot read disc XEX header");
  return value;
}

// Which disc a mounted folder is: the XEX execution info (title, disc number) and the disc's marker file.
unsigned Identify(Device& device) {
  auto* entry = device.ResolvePath("default.xex");
  File* raw = nullptr;
  if (!entry || entry->Open(FileAccess::kGenericRead, &raw) != X_STATUS_SUCCESS)
    throw std::runtime_error("Disc folder has no readable default.xex");
  std::unique_ptr<File, CloseFile> file(raw);
  auto header = Read<rex::xex2_header>(*file, 0);
  if (header.magic != 0x58455832 ||
      header.header_count > (entry->size() - offsetof(rex::xex2_header, headers)) / sizeof(rex::xex2_opt_header))
    throw std::runtime_error("Invalid disc XEX header");
  for (uint32_t i = 0; i < header.header_count; ++i) {
    auto opt = Read<rex::xex2_opt_header>(*file, offsetof(rex::xex2_header, headers) +
                                                     i * sizeof(rex::xex2_opt_header));
    if (opt.key != rex::XEX_HEADER_EXECUTION_INFO) continue;
    auto info = Read<rex::xex2_opt_execution_info>(*file, opt.offset);
    if (info.title_id != kTitleId || info.disc_count != 2 || info.disc_number < 1 || info.disc_number > 2)
      throw std::runtime_error("Folder is not a Dead Space 2 two-disc release");
    const auto marker = "disc" + std::to_string(info.disc_number) + "_info.txt";
    if (!device.ResolvePath(marker)) throw std::runtime_error("DS2 disc identity marker is missing");
    REXLOG_INFO("DS2_DISC_ID disc={} title={:08X} media={:08X}", info.disc_number, uint32_t(info.title_id),
                uint32_t(info.media_id));
    return info.disc_number;
  }
  throw std::runtime_error("Disc XEX execution info missing");
}

unsigned Mount(const std::filesystem::path& folder, unsigned expected) {
  auto disc = std::make_unique<HostPathDevice>(kDevices[expected - 1], folder, true);
  if (!disc->Initialize()) throw std::runtime_error("Cannot open disc folder: " + folder.string());
  const auto number = Identify(*disc);
  if (number != expected)
    throw std::runtime_error("Folder " + folder.string() + " holds disc " + std::to_string(number) +
                             ", expected disc " + std::to_string(expected));
  auto* root = disc->ResolvePath("");
  if (!root || !fs->RegisterDevice(std::move(disc))) throw std::runtime_error("Cannot register disc mount");
  roots[number - 1] = root;
  return number;
}

bool Select(unsigned number) {
  if (number < 1 || number > roots.size() || !roots[number - 1]) return false;
  if (fs->ResolvePath("game:") != update_root) return false;
  struct Previous {
    bool present;
    std::string target;
  };
  std::array<Previous, kAliases.size()> previous;
  for (size_t i = 0; i < kAliases.size(); ++i)
    previous[i].present = fs->FindSymbolicLink(kAliases[i], previous[i].target);
  try {
    for (auto alias : kAliases) {
      fs->UnregisterSymbolicLink(alias);
      if (!fs->RegisterSymbolicLink(alias, kDevices[number - 1]) || fs->ResolvePath(alias) != roots[number - 1])
        throw std::runtime_error("Alias verification failed");
    }
    if (fs->ResolvePath("game:") != update_root) throw std::runtime_error("TU2 mapping changed");
  } catch (...) {
    for (size_t i = 0; i < kAliases.size(); ++i) {
      fs->UnregisterSymbolicLink(kAliases[i]);
      if (previous[i].present && !fs->RegisterSymbolicLink(kAliases[i], previous[i].target))
        throw std::runtime_error("Disc alias rollback failed");
    }
    return false;
  }
  current_disc = number;
  REXLOG_INFO("DS2_DISC_SELECT disc={} device={} tu2_preserved=true", number, kDevices[number - 1]);
  return true;
}
}  // namespace

void Prepare(rex::Runtime& runtime, const std::filesystem::path& update_directory,
             const std::filesystem::path& disc1, const std::filesystem::path& disc2) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(update_directory / "default.xexp", ec))
    throw std::runtime_error("TU2 is required: put default.xexp in " + update_directory.string());
  fs = runtime.file_system();
  Mount(disc1, 1);
  if (!disc2.empty() && std::filesystem::is_directory(disc2, ec)) {
    Mount(disc2, 2);
  } else {
    REXLOG_WARN("ds2: no disc 2 folder; the game will stop at the disc swap in chapter 4");
  }
  auto update = std::make_unique<HostPathDevice>(kUpdate, update_directory, true);
  if (!update->Initialize()) throw std::runtime_error("Cannot initialize TU2 mount");
  update_root = update->ResolvePath("");
  if (!update_root || !fs->RegisterDevice(std::move(update))) throw std::runtime_error("Cannot register TU2 mount");
  auto lock = rex::thread::global_critical_region::AcquireDirect();
  fs->UnregisterSymbolicLink("game:");
  if (!fs->RegisterSymbolicLink("game:", kUpdate) || !Select(1))
    throw std::runtime_error("Cannot select disc 1 and the TU2 mapping");
}

}  // namespace ds2::disc_swap

REX_EXTERN(__imp__XamSwapDisc);

namespace ds2::disc_swap {
// The XEX's import thunk for XamSwapDisc (0x82B0857C in TU2) is bound to the kernel's export when the module
// loads; bind it to the handler below instead, as the PC build does.
void RegisterImport(rex::Runtime& runtime) {
  if (!runtime.function_dispatcher()->SetFunction(0x82B0857C, __imp__XamSwapDisc))
    throw std::runtime_error("Cannot register DS2 TU2 swap import");
}
}  // namespace ds2::disc_swap

// XamSwapDisc(disc number, completion event, ...): the game's request for the other disc. Point d: at it
// and signal the event, as the console does once the new disc is in.
REX_EXTERN(__imp__XamSwapDisc) {
  using namespace ds2::disc_swap;
  const uint32_t requested = ctx.r3.u32;
  const uint32_t event_address = ctx.r4.u32;
  ctx.r3.u64 = X_STATUS_UNSUCCESSFUL;
  REXLOG_INFO("DS2_DISC_REQUEST requested={} current={} event={:08X}", requested, current_disc, event_address);
  try {
    if (!fs || !event_address) throw std::runtime_error("Swap has no mount state or completion event");
    auto* kernel = rex::system::kernel_state();
    auto event = rex::system::XObject::GetNativeObject(kernel, base + event_address);
    if (!event || event->type() != rex::system::XObject::Type::Event)
      throw std::runtime_error("Invalid swap completion event");
    auto lock = rex::thread::global_critical_region::AcquireDirect();
    if (!Select(requested)) throw std::runtime_error("Requested disc unavailable or alias transaction failed");
    static_cast<rex::system::XEvent*>(event.get())->Set(1, false);
    ctx.r3.u64 = X_STATUS_SUCCESS;
    REXLOG_INFO("DS2_DISC_COMPLETE disc={} event={:08X}", requested, event_address);
  } catch (const std::exception& error) {
    REXLOG_ERROR("DS2_DISC_FAILED requested={} reason={}", requested, error.what());
  }
}
