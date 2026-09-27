// nfsmw - cutscene diagnostics (WMV)
//
// With nfsmw_video_diag = true, sub_826DB4F8 is logged (presentation of a movie frame, main thread,
// docs/audio-and-video.md): its caller, the video object, its size, the widths, strides and heights of
// the planes, the pending frames and the texture group, and every 5 s the rate and the time spent
// inside. The decoding (sub_827312C0, sub_828C35D8 and sub_8278A518) is in nfsmw_video_nativo.cpp.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(nfsmw_video_diag, false, "NFSMW",
                    "Diagnostico de las cinematicas: anota la presentacion de fotogramas de las peliculas (quien "
                    "llama, hilo, ritmo y buferes)");

namespace nfsmw::video_diag {
namespace {

uint32_t Leer32(const uint8_t* base, uint32_t direccion) {
  uint32_t v = 0;
  std::memcpy(&v, base + direccion, sizeof(v));
  return __builtin_bswap32(v);
}

int64_t AhoraUs() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

uint64_t IdHilo() {
  return std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFFFF;
}

struct Contador {
  std::atomic<uint64_t> llamadas{0};
  std::atomic<int64_t> desde_us{0};
  std::atomic<uint64_t> llamadas_desde{0};
  std::atomic<int64_t> us_dentro{0};
};
Contador g_presentar;

// A summary every 5 s: calls per second and average time inside.
void Resumen(const char* que, Contador& c, int64_t ahora) {
  int64_t desde = c.desde_us.load(std::memory_order_relaxed);
  if (desde == 0) {
    c.desde_us.store(ahora, std::memory_order_relaxed);
    c.llamadas_desde.store(c.llamadas.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return;
  }
  if (ahora - desde < 5000000) {
    return;
  }
  if (!c.desde_us.compare_exchange_strong(desde, ahora)) {
    return;
  }
  const uint64_t total = c.llamadas.load(std::memory_order_relaxed);
  const uint64_t n = total - c.llamadas_desde.exchange(total);
  const int64_t dentro = c.us_dentro.exchange(0);
  REXLOG_INFO("[video] {}: {:.1f} llamadas/s, {:.2f} ms dentro de media ({} en total)", que,
              double(n) * 1e6 / double(ahora - desde), n ? double(dentro) / double(n) / 1000.0 : 0.0, total);
}

}  // namespace
}  // namespace nfsmw::video_diag

REX_EXTERN(__imp__sub_826DB4F8);

REX_HOOK_RAW(sub_826DB4F8) {
  using namespace nfsmw::video_diag;
  if (!REXCVAR_GET(nfsmw_video_diag)) {
    __imp__sub_826DB4F8(ctx, base);
    return;
  }
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t obj = ctx.r3.u32;
  const uint64_t n = g_presentar.llamadas.fetch_add(1, std::memory_order_relaxed);
  if (n < 6 || n % 300 == 0) {
    REXLOG_INFO("[video] presentar #{} lr={:08X} obj={:08X} hilo={:06X} r4={:08X} r5={:08X} | {}x{} +44={:08X} "
                "Y {}x{} paso {} U {}x{} paso {} V {}x{} paso {} pendientes={} grupo={}",
                n, lr, obj, IdHilo(), ctx.r4.u32, ctx.r5.u32, Leer32(base, obj + 124), Leer32(base, obj + 128),
                Leer32(base, obj + 44), Leer32(base, obj + 332), Leer32(base, obj + 356), Leer32(base, obj + 344),
                Leer32(base, obj + 336), Leer32(base, obj + 360), Leer32(base, obj + 348),
                Leer32(base, obj + 340), Leer32(base, obj + 364), Leer32(base, obj + 352),
                Leer32(base, obj + 368), Leer32(base, obj + 372));
  }
  const int64_t antes = AhoraUs();
  __imp__sub_826DB4F8(ctx, base);
  const int64_t despues = AhoraUs();
  g_presentar.us_dentro.fetch_add(despues - antes, std::memory_order_relaxed);
  Resumen("presentar", g_presentar, despues);
}
