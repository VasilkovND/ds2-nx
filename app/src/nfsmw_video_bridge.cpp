#include "nfsmw_video_bridge.h"
#include "nfsmw_shader_hooks.h"
#include <atomic>
#include <bit>
#include <cmath>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

// Testing on the Switch showed a regression: planes were copied and then rejected because of
// the vertices. Only enable explicitly for development.
REXCVAR_DEFINE_BOOL(nfsmw_native_video, false, "NFSMW",
                    "Presentar cinematica por Vulkan nativo con respaldo Xenos")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace nfsmw::native {
namespace {
std::atomic<bool> g_desactivado{false};
std::mutex g_mutex;
std::shared_ptr<FotogramaVideo> g_capturado;
std::shared_ptr<const FotogramaVideo> g_listo;
ColaVideo g_cola;
std::atomic<unsigned> g_rechazos{0};
void Rechazo(unsigned bit,const char* motivo) {
  if (!(g_rechazos.fetch_or(bit)&bit)) REXLOG_WARN("[video nativo] captura rechazada: {}",motivo);
}
bool Activo() { return !g_desactivado.load(std::memory_order_relaxed) && REXCVAR_GET(nfsmw_native_video); }
uint32_t BE(const uint8_t* base, uint32_t p) {
  const volatile uint8_t* d = base + p;
  return uint32_t(d[0]) << 24 | uint32_t(d[1]) << 16 | uint32_t(d[2]) << 8 | d[3];
}
bool Rango(uint32_t p, uint64_t n) { return p && uint64_t(p) + n <= (uint64_t(1) << 32); }
}
void DesactivarVideo(const char* motivo) {
  if (!g_desactivado.exchange(true)) REXLOG_WARN("[video nativo] Xenos sigue activo: {}", motivo);
  { std::lock_guard lock(g_mutex); g_capturado.reset(); g_listo.reset(); }
  g_cola.Vaciar();
}
void InvalidarVideo() {
  if (!Activo()) return;
  std::lock_guard lock(g_mutex); g_listo.reset();
}
void CapturarPlanosVideo(const uint8_t* base, uint32_t objeto, uint32_t datos) {
  if (!Activo() || !Rango(objeto, 376)) return;
  try {
    { std::lock_guard lock(g_mutex); g_capturado.reset(); g_listo.reset(); }
    const auto* vs = ShaderOriginal({base + 0x8200FCF8u, 260});
    const auto* ps0 = ShaderOriginal({base + 0x8200FE00u, 444});
    const auto* ps1 = ShaderOriginal({base + 0x8200FFC0u, 444});
    // The original shader's identity already includes its stage. Do not repeat it with
    // a boolean: swapping VS/PS here rejected every frame.
    if (!vs || !ps0 || !ps1 || ShaderDeObjeto(BE(base,objeto+72)) != vs) {
      Rechazo(1,"VS del objeto o contenedores originales no reconocidos"); return;
    }
    const uint32_t ancho = BE(base,objeto+124), alto = BE(base,objeto+128);
    if (!ancho || !alto || ancho > 1920 || alto > 1080 || ((ancho|alto)&1)) {
      Rechazo(2,"dimensiones YUV no admitidas"); return;
    }
    const auto* pixel = ShaderDeObjeto(BE(base,objeto+76));
    if (pixel != ps0 && pixel != ps1) { Rechazo(4,"PS del objeto no reconocido"); return; }
    auto f = std::make_shared<FotogramaVideo>();
    f->objeto = objeto; f->ancho = ancho; f->alto = alto;
    f->vs = vs; f->ps[0] = ps0; f->ps[1] = ps1; f->variante = pixel == ps1;
    uint64_t offset = 0;
    for (unsigned plano = 0; plano < 3; ++plano) {
      const uint32_t w = plano ? ancho/2 : ancho, h = plano ? alto/2 : alto;
      const uint32_t pitch = BE(base,objeto+344+plano*4);
      if (BE(base,objeto+332+plano*4) != w || BE(base,objeto+356+plano*4) != h ||
          pitch < w || pitch > 4096 || !Rango(datos, offset+uint64_t(pitch)*h)) {
        Rechazo(8,"disposicion de planos o pitch no admitido"); return;
      }
      f->planos[plano].resize(size_t(w)*h);
      for (uint32_t y = 0; y < h; ++y) {
        // Plain loads: the Horizon handler is not required to emulate a SIMD memcpy
        // when a plane crosses a watched page of guest memory.
        const volatile uint8_t* origen = base + datos + offset + uint64_t(y)*pitch;
        auto* destino = f->planos[plano].data() + size_t(y)*w;
        for (uint32_t x = 0; x < w; ++x) destino[x] = origen[x];
      }
      offset += uint64_t(pitch)*h;
    }
    static std::atomic<bool> primera{true};
    if (primera.exchange(false)) REXLOG_INFO("[video nativo] primera captura YUV {}x{}; shaders verificados",ancho,alto);
    std::lock_guard lock(g_mutex); g_capturado = std::move(f);
  } catch (const std::exception& e) { DesactivarVideo(e.what()); }
}
void AnotarDibujoVideo(const uint8_t* base, bool esVideo, uint32_t objeto) {
  if (!Activo()) return;
  try {
    std::lock_guard lock(g_mutex);
    g_listo.reset();
    if (!esVideo || !g_capturado || g_capturado->objeto != objeto || !Rango(objeto,264)) return;
    // sub_826D8408 copies these same 120 bytes to the vertex buffer.
    for (size_t i = 0; i < 6; ++i) {
      auto leer = [&](unsigned j) { return std::bit_cast<float>(BE(base,objeto+144+uint32_t(i)*20+j*4)); };
      auto& v = g_capturado->vertices[i]; v = {leer(0),leer(1),leer(2),leer(3),leer(4)};
      if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) || !std::isfinite(v.u) || !std::isfinite(v.v) ||
          std::abs(v.x)>1.1f || std::abs(v.y)>1.1f || v.z<0 || v.z>1 || v.u<0 || v.u>1 || v.v<0 || v.v>1) {
        Rechazo(16,"vertices fuera del rango esperado"); return;
      }
    }
    g_listo = std::move(g_capturado);
  } catch (const std::exception& e) { DesactivarVideo(e.what()); }
}
void AnotarSwapVideo() {
  if (!Activo()) return;
  try {
    std::shared_ptr<const FotogramaVideo> f;
    { std::lock_guard lock(g_mutex); f = std::move(g_listo); g_capturado.reset(); }
    if (!g_cola.Encolar(std::move(f))) DesactivarVideo("cola de Swap llena; se evita desincronizar fotogramas");
  } catch (const std::exception& e) { DesactivarVideo(e.what()); }
}
std::shared_ptr<const FotogramaVideo> ConsumirVideo() {
  if (!Activo()) return {};
  return g_cola.Consumir();
}
}

REX_EXTERN(__imp__sub_82589DF0);
REX_HOOK_RAW(sub_82589DF0) {
  const bool video = ctx.lr == 0x826DB5C4 && ctx.r27.u32 == 0;
  const uint32_t objeto = ctx.r31.u32, datos = ctx.r25.u32;
  __imp__sub_82589DF0(ctx, base);
  if (video) nfsmw::native::CapturarPlanosVideo(base,objeto,datos);
}
