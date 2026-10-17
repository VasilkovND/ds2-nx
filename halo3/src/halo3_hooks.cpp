// halo3 - small hooks inside the game's own functions ([[midasm_hook]] in overrides.toml).
//
// Each one is called from the generated code at the address given in overrides.toml, before the instruction
// there. They are declared by the generated code itself (extern, global namespace), so the names must match.

#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>

// 0x821A85C4: the repeat of a worker thread's wait for work (sub_821A8500, started from sub_821B61B0). It polls
// a slot of its queue and the kernel's millisecond counter, for up to 66 ms, without resting. On the Xbox 360
// that spin had one of six hardware threads to itself; on the Switch it held one of the game's three cores at
// 98 % (stack samples, boot 7 in PORTING.md) and starved the rest, the audio included. A short sleep on each
// repeat keeps the worker answering within a fraction of a millisecond.
void Halo3WorkerPollYield() {
  rex::thread::Sleep(std::chrono::microseconds(100));
}

// 0x8231D910, after lwz r29,12(r10) in sub_8231D8D0 (diagnostic, while it is being chased): the game looks an
// object up by handle (r14; its slot is r10, 16 bytes per object) and takes its address (r29). Boots 8-11 crashed
// a little later in this function with a garbage pointer built from it (0xE0, 0x3F800020, 0x20, 0x4B1E6BA0, see
// PORTING.md): r22 = r29 + the 16-bit offset at r29 + 312, then r22 + 32 is read. This logs the first 16 lookups
// where either the object or r29 + that offset is not readable guest memory, with the slot's 16 bytes.
void Halo3ObjectLookupCheck(PPCRegister& r14, PPCRegister& r10, PPCRegister& r29) {
  static std::atomic<int> reported{0};
  auto* memory = rex::system::kernel_memory();
  auto readable = [memory](uint32_t address) {
    uint32_t protect = 0;
    auto* heap = memory->LookupHeap(address);
    return heap && heap->QueryProtect(address, &protect) && (protect & rex::memory::kMemoryProtectRead);
  };
  const uint32_t object = r29.u32;
  const bool object_ok = readable(object) && readable(object + 316);
  int32_t offset = 0;
  bool target_ok = true;
  if (object_ok) {
    offset = int16_t(rex::memory::load_and_swap<uint16_t>(memory->TranslateVirtual<const uint8_t*>(object + 312)));
    target_ok = offset == -1 || readable(object + uint32_t(offset) + 32);  // 0xFFFF takes another path
  }
  if (object_ok && target_ok) {
    return;
  }
  if (reported.fetch_add(1, std::memory_order_relaxed) >= 16) {
    return;
  }
  uint32_t slot[4] = {};
  if (readable(r10.u32)) {
    const auto* p = memory->TranslateVirtual<const uint8_t*>(r10.u32);
    for (int i = 0; i < 4; ++i) {
      slot[i] = rex::memory::load_and_swap<uint32_t>(p + i * 4);
    }
  }
  REXLOG_WARN("[objeto] handle {:08X}: slot {:08X} = {:08X} {:08X} {:08X} {:08X}, object {:08X} ({}), offset "
              "at +312 {} ({})",
              r14.u32, r10.u32, slot[0], slot[1], slot[2], slot[3], object, object_ok ? "readable" : "NOT readable",
              offset, target_ok ? "ok" : "points at unreadable memory");
}
