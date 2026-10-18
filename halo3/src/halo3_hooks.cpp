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

// 0x8231DF8C and 0x8231DFF8, the two `bl sub_82107DD8` of the node loop in sub_8231D8D0 (diagnostic, while it is
// being chased). The loop builds each node's matrix from its orientation (r4 = r22 + 32 * node, r22 = the object
// r29 plus the 16-bit offset at r29 + 312) for node = 1 .. [r21 + 48]. Boots 8-12 crashed on the first load of
// sub_82107DD8 with r4 unmapped (0x20, 0xE0, 0x3F800020, 0x4B1E6BA0, 0x4B1DA500, see PORTING.md); the object
// lookup itself was fine (boot 12). This logs the first 16 such nodes, with what the loop was built from, and
// points r4 at the game's default-orientation buffer (0x82804480, 32 bytes per node, filled by sub_8210ECF8) so
// the run goes on and shows whether more follows.
void Halo3NodeLoopCheck(PPCRegister& r4, PPCRegister& r22, PPCRegister& r29, PPCRegister& r30, PPCRegister& r21,
                        PPCRegister& r14) {
  static std::atomic<int> reported{0};
  static std::atomic<uint32_t> redirected{0};
  auto* memory = rex::system::kernel_memory();
  auto readable = [memory](uint32_t address) {
    uint32_t protect = 0;
    auto* heap = memory->LookupHeap(address);
    return heap && heap->QueryProtect(address, &protect) && (protect & rex::memory::kMemoryProtectRead);
  };
  if (readable(r4.u32) && readable(r4.u32 + 31)) {
    return;
  }
  const uint32_t node = r30.u32;
  const uint32_t total = redirected.fetch_add(1, std::memory_order_relaxed) + 1;
  if (reported.fetch_add(1, std::memory_order_relaxed) < 16) {
    auto load32 = [&](uint32_t address) {
      return readable(address) ? rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<const uint8_t*>(address))
                               : 0xDEADDEADu;
    };
    const uint32_t object = r29.u32;
    const int32_t offset = readable(object + 312) ? int16_t(load32(object + 312) >> 16) : 0x7FFFFFFF;
    REXLOG_WARN("[nodos] handle {:08X} object {:08X} [{:08X} {:08X} {:08X} {:08X}] offset at +312 {} -> r22 {:08X}; "
                "node {} of {} (r21 {:08X}): r4 {:08X} not readable (redirected so far: {})",
                r14.u32, object, load32(object), load32(object + 4), load32(object + 8), load32(object + 12), offset,
                r22.u32, node, load32(r21.u32 + 48), r21.u32, r4.u32, total);
  }
  r4.u64 = 0x82804480u + 32u * (node < 255 ? node : 0u);
}
