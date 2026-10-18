// Universal Xbox 360 deko renderer: the hooks a game plugs in for its own effects and image settings.
//
// The generic renderer (RenderDk) draws any Xbox 360 game. Everything a particular game adds on top - the extra
// shader specialization for its passes, alternate SPIR-V for an effect, a post-process colour grade, FXAA - comes
// through this interface. A game with none of that (for example a first bring-up) uses the default no-op hooks and
// the renderer behaves as the plain Xbox 360 GPU would.
#pragma once

#include <cstdint>
#include <span>
#include <string>

#include <rex/logging.h>

namespace rex::x360 {

struct EntradaShader;

// A colour grade applied in the output pass. All-neutral by default (no change).
struct PostProcess {
  bool activo = false;
  float brillo = 0.0f;
  float contraste = 1.0f;
  float saturacion = 1.0f;
  float vibracion = 0.0f;
  float temperatura = 0.0f;
  float gamma = 1.0f;
  float tinte_r = 1.0f;
  float tinte_g = 1.0f;
  float tinte_b = 1.0f;
  float tinte = 0.0f;
  float vineta = 0.0f;
  float lineas = 0.0f;
};

// How the renderer should treat one draw, beyond the plain Xbox 360 behaviour.
struct ClasePase {
  bool sin_desenfoque = false;            // a composite pass to draw without its radial blur
  bool cielo = false;                     // the sky pass
  bool pantalla_completa_sin_z = false;   // a full-screen pass with no depth: skip the early-depth optimisation
  uint32_t variante = 0;                  // non-zero: compile this pixel shader from SpirvDeVariante(variante)
};

class RenderHooks {
 public:
  virtual ~RenderHooks() = default;

  // Image settings read in the output pass.
  virtual bool Fxaa() { return false; }
  virtual PostProcess Posproceso() { return {}; }
  virtual uint64_t VersionPosproceso() { return 0; }  // changes when Posproceso() would differ

  // Called once per Swap with the interval in milliseconds, for a game's frame-pacing watchdog. Default: ignored.
  virtual void Latido(double ms) { (void)ms; }

  // A log line from the renderer. A game may write it off the hot thread; the default writes it now.
  virtual void Informe(const std::string& linea) { REXLOG_INFO("{}", linea); }

  // Per-pixel-shader classification. A game recognises its own passes by ps.huella_contenedor and returns how the
  // renderer should treat the draw. Default: a plain draw, no extra behaviour.
  virtual ClasePase Clasificar(const EntradaShader& ps) { (void)ps; return {}; }
  // Alternate SPIR-V for a ClasePase::variante (an effect's own shader), or an empty span to use the shader's own.
  virtual std::span<const uint32_t> SpirvDeVariante(uint32_t variante) { (void)variante; return {}; }
};

// The default hooks: no effects, neutral settings. Used by a game that plugs in nothing.
inline RenderHooks& HooksPorDefecto() {
  static RenderHooks hooks;
  return hooks;
}

}  // namespace rex::x360
