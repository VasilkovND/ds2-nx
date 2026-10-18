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

// Diagnostics while the node-loop crash of boots 8-13 is chased (see overrides.toml and PORTING.md). sub_8231D8D0
// builds each node's matrix with sub_82107DD8(r3 = parent matrix, r4 = orientation, r5 = output), where
// r4 = r22 + 32 * node and r5 = r17 + 52 * node. r17 is set once, after the object lookup, as the object (r29) plus
// a 16-bit offset; boot 13 crashed with r17 = 0x1_4B1E_xxxx, which that add cannot produce. So r17 is kept per
// thread right after the add and compared after every call in the function: the first call site after which it
// differs is the one that replaced it.
namespace {

struct R17State {
  uint64_t kept = 0;
  uint32_t object = 0;
  uint32_t handle = 0;
  bool valid = false;
};
thread_local R17State g_r17;
std::atomic<int> g_r17_reported{0};

bool GuestReadable(uint32_t address) {
  if (address >= 0x82000000u && address < 0x84000000u) {
    return true;  // the executable's image: QueryProtect does not report it readable, and it is
  }
  auto* memory = rex::system::kernel_memory();
  uint32_t protect = 0;
  auto* heap = memory->LookupHeap(address);
  return heap && heap->QueryProtect(address, &protect) && (protect & rex::memory::kMemoryProtectRead);
}

void CheckR17(uint32_t site, PPCRegister& r17) {
  if (!g_r17.valid || r17.u64 == g_r17.kept) {
    return;
  }
  if (g_r17_reported.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_WARN("[r17] changed by the call returning to {:08X}: {:016X} -> {:016X} (object {:08X}, handle {:08X})",
                site, g_r17.kept, r17.u64, g_r17.object, g_r17.handle);
  }
  g_r17.kept = r17.u64;  // one line per change
}

}  // namespace

// 0x8231D940, after add r17,r10,r29.
void Halo3R17Keep(PPCRegister& r17, PPCRegister& r29, PPCRegister& r14) {
  g_r17 = {r17.u64, r29.u32, r14.u32, true};
  if (r17.u64 >> 32 && g_r17_reported.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_WARN("[r17] already wrong at the add: {:016X} (object {:016X}, handle {:08X})", r17.u64, r29.u64,
                r14.u32);
  }
}

#define HALO3_R17_AFTER(addr) \
  void Halo3R17After_##addr(PPCRegister& r17) { CheckR17(0x##addr, r17); }
HALO3_R17_AFTER(8231DA04)
HALO3_R17_AFTER(8231DA18)
HALO3_R17_AFTER(8231DA9C)
HALO3_R17_AFTER(8231DB30)
HALO3_R17_AFTER(8231DB7C)
HALO3_R17_AFTER(8231DBE8)
HALO3_R17_AFTER(8231DC54)
HALO3_R17_AFTER(8231DC78)
HALO3_R17_AFTER(8231DC9C)
HALO3_R17_AFTER(8231DCD4)
HALO3_R17_AFTER(8231DD3C)
HALO3_R17_AFTER(8231DDBC)
HALO3_R17_AFTER(8231DDF0)
HALO3_R17_AFTER(8231DE5C)
HALO3_R17_AFTER(8231DE6C)
HALO3_R17_AFTER(8231DEC8)
#undef HALO3_R17_AFTER

// 0x8231DF8C and 0x8231DFF8, the two `bl sub_82107DD8` of the node loop: compares r17 once more, and logs the
// first 16 nodes whose orientation (r4) or output (r5) is not readable, with what the loop was built from.
void Halo3NodeLoopCheck(PPCRegister& r4, PPCRegister& r5, PPCRegister& r17, PPCRegister& r22, PPCRegister& r29,
                        PPCRegister& r30, PPCRegister& r21, PPCRegister& r14) {
  CheckR17(0x8231DF8C, r17);
  if (GuestReadable(r4.u32) && GuestReadable(r4.u32 + 31) && GuestReadable(r5.u32) && GuestReadable(r5.u32 + 51)) {
    return;
  }
  static std::atomic<int> reported{0};
  if (reported.fetch_add(1, std::memory_order_relaxed) >= 16) {
    return;
  }
  auto* memory = rex::system::kernel_memory();
  auto load32 = [memory](uint32_t address) {
    return GuestReadable(address)
               ? rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<const uint8_t*>(address))
               : 0xDEADDEADu;
  };
  REXLOG_WARN("[nodos] handle {:08X} object {:016X} [{:08X} {:08X} {:08X} {:08X}] +312/+316 {:08X}; node {} of {} "
              "(r21 {:08X}): r4 {:016X} (r22 {:016X}), r5 {:016X} (r17 {:016X})",
              r14.u32, r29.u64, load32(r29.u32), load32(r29.u32 + 4), load32(r29.u32 + 8), load32(r29.u32 + 12),
              load32(r29.u32 + 312), r30.u32, load32(r21.u32 + 48), r21.u32, r4.u64, r22.u64, r5.u64, r17.u64);
}
