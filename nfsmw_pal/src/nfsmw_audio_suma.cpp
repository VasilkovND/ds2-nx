// nfsmw - gain-scaled sum of the sound engine in native code (sub_825FDFB0)
//
// sub_825DCED8 mixes each source into its channels by calling through the pointer at 0x82A2B1C8, which in the
// measured race always points to sub_825FDFB0 (46,725 out of 46,725 calls). It is a leaf:
//   dst[i] = dst[i] + src[i] * g        for i < n
// with n in r3, the source in r5, the destination in r6 and the gain g in f1, in blocks of 4 samples plus a
// remainder. Each sample reads the source, reads the destination, does an fmadds
// (double(float(std::fma(src, g, dst)))) and stores the destination, in that order. The native code does the
// same sample by sample, so the result is bit-identical even if the source and the destination overlap. Only
// volatile registers change (r4, r7-r11, f0, f2-f13, cr6); r3 and f1 stay the same.
//
// nfsmw_audio_suma_nativa: 0 = recompiled; 1 = native (default); 2 = validate: runs the recompiled code, saves
// the destination, undoes it, runs the native code, compares and keeps the recompiled result. A summary is
// logged every 10 s.

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_informe_diferido.h"  // deferred reports

#include "nfsmw_audio_nativo.h"

REXCVAR_DEFINE_INT32(nfsmw_audio_suma_nativa, 1, "NFSMW",
                     "Suma con ganancia del motor de sonido (sub_825FDFB0): 0 = codigo recompilado, 1 = nativo (mismo "
                     "resultado bit a bit; por defecto), 2 = validar el nativo contra el recompilado");

REX_EXTERN(__imp__sub_825FDFB0);

namespace nfsmw::audio_suma {
namespace {

using namespace nfsmw::audio_nativo;

constexpr int32_t kMaxValidar = 1 << 20;

std::atomic<uint64_t> g_llamadas{0};
std::atomic<uint64_t> g_muestras{0};
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
  NFSMW_INFORME_DIFERIDO("[audio] suma con ganancia (modo {}): sub_825FDFB0 {} llamadas y {} muestras, diferencias con el "
              "recompilado {}",
              modo, g_llamadas.exchange(0), g_muestras.exchange(0), g_diferencias.exchange(0));
}

// The sum in native code, sample by sample and in the same order as the recompiled code.
void Nativo(uint8_t* base, int32_t n, uint32_t origen, uint32_t destino, double g) {
  if (n <= 0) {
    return;
  }
  const uint64_t bytes = uint64_t(n) * 4;
  // Below 0xE0000000 the host address is base + address on both platforms: it advances 4 bytes at a time.
  if (uint64_t(origen) + bytes <= 0xE0000000ull && uint64_t(destino) + bytes <= 0xE0000000ull) {
    const uint8_t* po = Dir(base, origen);
    uint8_t* pd = Dir(base, destino);
    for (int32_t i = 0; i < n; ++i) {
      uint32_t o;
      uint32_t d;
      std::memcpy(&o, po + size_t(i) * 4, 4);
      std::memcpy(&d, pd + size_t(i) * 4, 4);
      const float fo = std::bit_cast<float>(__builtin_bswap32(o));
      const float fd = std::bit_cast<float>(__builtin_bswap32(d));
      const uint32_t r = __builtin_bswap32(std::bit_cast<uint32_t>(float(std::fma(double(fo), g, double(fd)))));
      std::memcpy(pd + size_t(i) * 4, &r, 4);
    }
    return;
  }
  for (int32_t i = 0; i < n; ++i) {
    const float fo = LeerFloat(base, origen + uint32_t(i) * 4);
    const uint32_t dir_d = destino + uint32_t(i) * 4;
    const float fd = LeerFloat(base, dir_d);
    EscribirFloat(base, dir_d, float(std::fma(double(fo), g, double(fd))));
  }
}

// Mode 2: compares the destination left by the recompiled and the native code, as 4-byte words.
void Validar(PPCContext& ctx, uint8_t* base) {
  const int32_t n = ctx.r3.s32;
  const uint32_t origen = ctx.r5.u32;
  const uint32_t destino = ctx.r6.u32;
  const double g = ctx.f1.f64;
  if (n <= 0 || n > kMaxValidar || uint64_t(destino) + uint64_t(n) * 4 > 0xE0000000ull) {
    __imp__sub_825FDFB0(ctx, base);
    return;
  }
  const size_t bytes = size_t(n) * 4;
  thread_local std::vector<uint8_t> antes;
  thread_local std::vector<uint8_t> recompilado;
  uint8_t* p = Dir(base, destino);
  antes.assign(p, p + bytes);
  __imp__sub_825FDFB0(ctx, base);
  recompilado.assign(p, p + bytes);
  std::memcpy(p, antes.data(), bytes);
  Nativo(base, n, origen, destino, g);
  if (std::memcmp(p, recompilado.data(), bytes) != 0) {
    g_diferencias.fetch_add(1, std::memory_order_relaxed);
    if (!g_diferencia_anotada.exchange(true, std::memory_order_relaxed)) {
      size_t i = 0;
      while (i + 4 <= bytes && std::memcmp(p + i, recompilado.data() + i, 4) == 0) {
        i += 4;
      }
      REXLOG_WARN("[audio] suma con ganancia: primera diferencia en la muestra {} de {} (destino 0x{:08X}, origen "
                  "0x{:08X}, g {})",
                  i / 4, n, destino, origen, g);
    }
  }
  std::memcpy(p, recompilado.data(), bytes);  // the game continues with the recompiled result
}

}  // namespace

void Suma825FDFB0(PPCContext& ctx, uint8_t* base) {
  const int32_t modo = REXCVAR_GET(nfsmw_audio_suma_nativa);
  if (modo != 1 && modo != 2) {
    __imp__sub_825FDFB0(ctx, base);
    return;
  }
  const int32_t n = ctx.r3.s32;
  g_llamadas.fetch_add(1, std::memory_order_relaxed);
  g_muestras.fetch_add(uint64_t(std::max(n, 0)), std::memory_order_relaxed);
  Informar(modo);
  if (modo == 2) {
    Validar(ctx, base);
    return;
  }
  ctx.fpscr.disableFlushMode();
  Nativo(base, n, ctx.r5.u32, ctx.r6.u32, ctx.f1.f64);
}

}  // namespace nfsmw::audio_suma
