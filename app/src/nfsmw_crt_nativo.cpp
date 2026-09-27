// nfsmw - native memset of the game's CRT (sub_826BE610)
//
// sub_826BE610 is the memset of the game's CRT, as seen in the generated code:
//  - writes byte by byte until the destination is 4-byte aligned;
//  - replicates the byte into a word with two rlwimi;
//  - writes 16-byte blocks with four stw, then the remaining words and the leftover bytes;
//  - leaves r3 untouched.
// It is the sibling of the memcpy at 0x826BE1B0, which already goes to the host's through [rexcrt] in
// overrides.toml. Putting it there too would require regenerating and rebuilding all the code; a hook in the
// app has the same effect. The recompiled code writes with volatile stores and byte swapping and std::memset
// writes the same thing in one go: memory ends up identical. Only volatile registers change (r0, r4-r6, ctr
// and cr0), which the caller does not read after the call.
// The audio calls it for every source and packet (sub_825DCED8), but the whole game uses it.
//
// On the Switch, if the write lands on a watched page, the exception handler unwatches it and replays the
// instruction without decoding it (exception_handler_switch.cpp), just like with the host memcpy.
//
// nfsmw_crt_memset_nativo: 0 = recompiled; 1 = native (default); 2 = validate: runs the recompiled code, saves
// what it wrote, undoes it, runs the native code, compares and keeps the recompiled result (up to 1 MB per
// call). With mode 2 or with nfsmw_crt_diag, calls and bytes are counted and a summary is logged every 10 s.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(nfsmw_crt_memset_nativo, 1, "NFSMW",
                     "memset del CRT del juego (sub_826BE610): 0 = codigo recompilado, 1 = memset del host (la memoria "
                     "queda igual; por defecto), 2 = validar el nativo contra el recompilado");
REXCVAR_DEFINE_BOOL(nfsmw_crt_diag, false, "NFSMW",
                    "Diagnostico: cuenta las llamadas y los bytes del memset del juego (sub_826BE610) y anota un "
                    "resumen cada 10 s");

REX_EXTERN(__imp__sub_826BE610);

namespace nfsmw::crt {
namespace {

constexpr uint32_t kMaxValidar = uint32_t(1) << 20;

std::atomic<uint64_t> g_llamadas{0};
std::atomic<uint64_t> g_bytes{0};
std::atomic<uint64_t> g_validadas{0};
std::atomic<uint64_t> g_sin_validar{0};
std::atomic<uint64_t> g_diferencias{0};
std::atomic<int64_t> g_ultimo_informe_ms{0};
std::atomic<bool> g_diferencia_anotada{false};

int64_t AhoraMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void Informar(int32_t modo) {
  const int64_t ahora = AhoraMs();
  int64_t ultimo = g_ultimo_informe_ms.load(std::memory_order_relaxed);
  if (ultimo == 0) {
    g_ultimo_informe_ms.compare_exchange_strong(ultimo, ahora, std::memory_order_relaxed);
    return;
  }
  if (ahora - ultimo < 10000 || !g_ultimo_informe_ms.compare_exchange_strong(ultimo, ahora, std::memory_order_relaxed)) {
    return;
  }
  REXLOG_INFO("[crt] memset del juego (modo {}) en {:.1f} s: {} llamadas y {} KB; validadas {}, sin validar (mas de "
              "1 MB) {}, diferencias con el recompilado {}",
              modo, double(ahora - ultimo) / 1000.0, g_llamadas.exchange(0), g_bytes.exchange(0) >> 10,
              g_validadas.exchange(0), g_sin_validar.exchange(0), g_diferencias.exchange(0));
}

// Same arguments as the PPC: 32-bit destination and count, 8-bit value. r3 stays the same, as in the
// recompiled code.
inline void Nativo(PPCContext& ctx, uint8_t* base) {
  const uint32_t n = ctx.r5.u32;
  if (n != 0) {
    std::memset(rex::memory::GuestPtr<uint8_t*>(base, ctx.r3.u32), ctx.r4.u8, n);
  }
}

void Validar(PPCContext& ctx, uint8_t* base) {
  const uint32_t destino = ctx.r3.u32;
  const uint32_t n = ctx.r5.u32;
  const uint8_t valor = ctx.r4.u8;
  if (n == 0 || n > kMaxValidar) {
    if (n != 0) {
      g_sin_validar.fetch_add(1, std::memory_order_relaxed);
    }
    __imp__sub_826BE610(ctx, base);
    return;
  }
  thread_local std::vector<uint8_t> antes;
  thread_local std::vector<uint8_t> recompilado;
  uint8_t* p = rex::memory::GuestPtr<uint8_t*>(base, destino);
  antes.assign(p, p + n);
  __imp__sub_826BE610(ctx, base);
  const uint32_t r3_recompilado = ctx.r3.u32;
  recompilado.assign(p, p + n);
  std::memcpy(p, antes.data(), n);
  std::memset(p, valor, n);
  if (std::memcmp(p, recompilado.data(), n) != 0 || r3_recompilado != destino) {
    g_diferencias.fetch_add(1, std::memory_order_relaxed);
    if (!g_diferencia_anotada.exchange(true, std::memory_order_relaxed)) {
      REXLOG_WARN("[crt] memset del juego: primera diferencia en 0x{:08X}, {} bytes, valor 0x{:02X}, r3 del "
                  "recompilado 0x{:08X}",
                  destino, n, valor, r3_recompilado);
    }
  }
  std::memcpy(p, recompilado.data(), n);  // the game continues with the recompiled result
  g_validadas.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

void Memset826BE610(PPCContext& ctx, uint8_t* base) {
  const int32_t modo = REXCVAR_GET(nfsmw_crt_memset_nativo);
  if (modo == 1 && !REXCVAR_GET(nfsmw_crt_diag)) {
    Nativo(ctx, base);
    return;
  }
  if (modo != 1 && modo != 2) {
    __imp__sub_826BE610(ctx, base);
    return;
  }
  const uint64_t llamadas = g_llamadas.fetch_add(1, std::memory_order_relaxed) + 1;
  g_bytes.fetch_add(ctx.r5.u32, std::memory_order_relaxed);
  if ((llamadas & 0x3FF) == 0) {
    Informar(modo);
  }
  if (modo == 2) {
    Validar(ctx, base);
  } else {
    Nativo(ctx, base);
  }
}

}  // namespace nfsmw::crt

// A single hook per function: on the Switch the linker accepts duplicate definitions and would silently keep one.
REX_HOOK_RAW(sub_826BE610) {
  nfsmw::crt::Memset826BE610(ctx, base);
}
