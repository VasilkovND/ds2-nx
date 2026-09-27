// nfsmw - scenery switched meshes while still large on screen
//
// WHAT IT FIXES
//   The mesh switch of the scenery (ScenerySectionHeader::DrawAScenery). Each SceneryInfo
//   has pModel[4] (Scenery.hpp:173, offset 0x28); the game chooses between pModel[0] (full) and
//   pModel[2] (reduced). Each eModel carries its own eSolid and its own pTextureTable, so
//   the switch changes geometry and texture, and it is a pointer swap without a fade: pop.
//
// WHERE IT IS IN THE RECOMPILED BINARY
//   sub_824C2850 = ScenerySectionHeader::DrawAScenery   app/generated/default/nfsmw_recomp.70.cpp:18095
//     :18292  call to sub_824C2720 (InlinedViewGetPixelSize) -> returns pixel_size in r3
//             and writes the distance to the camera into the float r6 points to
//     :18391  lfs f7,80(r1)      <- that distance
//     :18395  lfs f13,3552(r27)  <- constant at 0x82062AC8 (r27 = 0x82061CE8)
//     :18410  blt -> 0x824C2A88  <- if distance < K: full mesh, nothing else is checked
//     :18412  lfs f13,-4396(r27) <- the 8.7f, at 0x82060BBC
//     :18417  blt -> 0x824C2AC8  <- if pixel_size/max(Density,6) < 8.7: reduced mesh
//   sub_824C2720 = InlinedViewGetPixelSize              app/generated/default/nfsmw_recomp.4.cpp:18706
//   A single caller in the whole binary: nfsmw_recomp.70.cpp:18292, inside DrawAScenery.
//
// WHY THE HOOK IS HERE AND NOT IN DrawAScenery
//   The distance sub_824C2720 writes is only read in that comparison. The return value
//   (pixel_size) is not touched, so neither the 18 px cutoff nor the branches for the
//   rear-view mirror / cubemap / shadows (which go by ExcludeFlags 0x1800 and 0x20 and do not
//   look at the distance) move.
//   Possible side effects: none outside the full/reduced choice for the filtered view.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>

/*
 * Why -1 and not 150.
 *
 * Measured on screenshots and on video frames: the switch happens when the object covers a radius of
 * ~159 px, i.e. a third of the screen. The 52.2 px that comes out of the formula is only the floor (8.7 x
 * the minimum of 6.0); the real threshold is 8.7 x Density, and that garage door has Density ~18-19. That
 * is why it is so visible.
 *
 * And the geometry is worse than it seemed: an object is drawn from r*H/18 and is only on the full mesh
 * from r*H/(8.7*D), so the fraction of area on the full mesh is (18/(8.7*D))^2 = 11.9 % with the minimum
 * Density and 1.1 % with Density 19. Between 88 % and 99 % of what is visible is on the reduced mesh.
 *
 * With 150 units the switch is pushed away from where the eye is, but there is still a switch. The aim
 * was to make the effect disappear, not to disguise it, so the value was -1.
 *
 * The cost, which is the risk: the extra triangles on the same objects cost almost nothing (same
 * silhouette, same fragments, and the scene is ALU-bound: TMU 51 %, ROP 8 %). The estimate is 0.2-0.6 ms.
 * What could cost something is draws: if the full mesh brings more materials, that means more draw calls
 * at ~12 us of CPU each, and that gets expensive. It shows in "C7" and "C6 dibujos" in the log.
 *
 * If it turns out expensive: lower it to 150 in the toml (it covers everything that looks large, for
 * tenths of a ms) or to 0 to go back exactly to the game's behavior.
 */
/*
 * Off. The hypothesis was wrong, and this is measured.
 *
 * With the hook reaching 100 % of the objects (the log says "8.684.579 que el juego dibuja, 8.684.579
 * con la malla buena forzada"), the scenery triangles did not move (37.1 k without the hook against
 * 39.5 k with it, and that 6 % is driving noise). Forcing the full mesh changes nothing because there is
 * almost never a better mesh to choose.
 *
 * The reason is in the game itself: if an object has no detail model of its own, loading the section
 * copies the same pointer into all four levels (nfsmwdecomp, src/Speed/Indep/Src/World/Scenery.cpp:401-421:
 * "if pModel[2] is null and pModel[1] is not, pModel[2] = pModel[1]", and likewise for pModel[0]). So
 * pModel[0] and pModel[2] are the same eModel, with the same eSolid and the same texture table.
 *
 * What is visible on screen is not this. It is that the detailed scenery section arrives late: while it
 * is missing, the distant stand-ins of the 'Z' sections are drawn (Scenery.cpp:927), and when the section
 * comes in the full object appears all at once. The popping and the stutter are the same event. The lever
 * is the streamer's anticipation (see nfsmw_streaming_anticipacion.cpp).
 *
 * The hook, the cvar and the counters stay because they were hard to find and the per-caller breakdown
 * is still useful; but the default is 0 = the game decides, which is what retail does.
 */
REXCVAR_DEFINE_INT32(nfsmw_escenario_detalle, 0, "NFSMW",
                     "Unidades del mundo que se le adelanta al escenario el cambio a la malla buena. "
                     "0 = el juego decide (salta a media pantalla). 150 = la malla buena entra 150 "
                     "unidades antes. -1 = malla buena SIEMPRE mientras el objeto se dibuje")
    .range(-1, 20000);

REXCVAR_DEFINE_BOOL(nfsmw_escenario_detalle_diag, false, "NFSMW",
                    "Anota cada 10 s cuantos objetos de escenario entran en la lista, cuantos con "
                    "malla reducida, y los triangulos que costaria subirlos todos a la buena");

namespace nfsmw::escenario_lod {
namespace {

constexpr uint32_t kOffVistaEnCull = 128;        // SceneryCullInfo+128 = pView
constexpr uint32_t kOffIdVista = 4;              // eView+0x04 = ID
constexpr uint32_t kBaseVistas = 0x82A38070;     // same as in nfsmw_sombras_lod.cpp
constexpr uint32_t kBytesPorVista = 112;
constexpr uint32_t kVistaEscena = 1;
constexpr uint32_t kVistas = 21;
constexpr uint32_t kUmbralDistancia = 0x82062AC8;  // the game's float K (only to log it)
constexpr int32_t kPixelMinimoRamaNormal = 18;     // below this, DrawAScenery does not even decide

std::atomic<bool> g_anotado{false};
// Without these counters the first measurement was blind, and there was no way to see that the per-view
// filter was discarding the calls. They are three relaxed increments per object: their cost is negligible.
std::atomic<uint64_t> g_entradas{0};   // calls with the setting on
std::atomic<uint64_t> g_grandes{0};    // of those, the ones the game will draw (>= 18 px)
std::atomic<uint64_t> g_forzadas{0};   // of those, the ones whose distance we moved

uint32_t Leer32(const uint8_t* base, uint32_t dir) {
  uint32_t v = 0;
  std::memcpy(&v, base + dir, sizeof(v));
  return __builtin_bswap32(v);
}

float LeerFlotante(const uint8_t* base, uint32_t dir) {
  const uint32_t v = Leer32(base, dir);
  float f = 0.0f;
  std::memcpy(&f, &v, sizeof(f));
  return f;
}

void EscribirFlotante(uint8_t* base, uint32_t dir, float f) {
  uint32_t v = 0;
  std::memcpy(&v, &f, sizeof(v));
  v = __builtin_bswap32(v);
  std::memcpy(base + dir, &v, sizeof(v));
}

// No longer used: the per-view filter discarded almost all calls (see the hook's comment).
// Kept because the view mapping is useful and was hard to find.
[[maybe_unused]] bool EsVistaDeEscena(const uint8_t* base, uint32_t vista) {
  if (vista < kBaseVistas) return false;
  const uint32_t d = vista - kBaseVistas;
  if (d % kBytesPorVista != 0) return false;
  const uint32_t idx = d / kBytesPorVista;
  if (idx >= kVistas) return false;
  return Leer32(base, vista + kOffIdVista) == kVistaEscena && idx == kVistaEscena;
}

}  // namespace
}  // namespace nfsmw::escenario_lod

// =================================================================================================
// InlinedViewGetPixelSize(SceneryCullInfo* r3, bVector3* position r4, float radius f1, float* out r6)
// Returns the size in pixels in r3 and writes the camera-object distance to *r6.
// =================================================================================================
REX_EXTERN(__imp__sub_824C2720);
REX_HOOK_RAW(sub_824C2720) {
  using namespace nfsmw::escenario_lod;

  const uint32_t cull = ctx.r3.u32;
  const uint32_t salida = ctx.r6.u32;

  __imp__sub_824C2720(ctx, base);

  const int32_t adelanto = REXCVAR_GET(nfsmw_escenario_detalle);
  if (adelanto == 0 || salida == 0 || cull == 0) {
    return;
  }
  ++g_entradas;
  // If the object does not reach the 18 px bar, DrawAScenery discards it (nfsmw_recomp.70.cpp:18355,
  // `cmpwi r3,18` + `blt loc_824C2B3C`): it does not draw it even with the reduced mesh. So there is
  // nothing to gain here and we save the work.
  if (ctx.r3.s32 < kPixelMinimoRamaNormal) {
    return;
  }
  ++g_grandes;
  /*
   * Per-view filter removed. It was what made this useless.
   *
   * The hook did run (the log has the line with K = 25.0) but the scenery triangles only went from
   * 37.1 k to 38.8 k (+4.6 %), when with 88-99 % on the reduced mesh they should have gone up far more.
   * So the filter was discarding almost all the calls: offset 128 of SceneryCullInfo is not the view,
   * or not always.
   *
   * And the filter was unnecessary anyway: writing this distance can only affect the branch that reads
   * it, and the only one that reads it is the full/reduced choice of the normal camera
   * (nfsmw_recomp.70.cpp:18391). The rear-view mirror, the cubemap, the reflection and the shadows go by
   * ExcludeFlags 0x1800 and 0x20, with fixed thresholds of 32 and 23 px, and do not look at the distance.
   * Removing it does not affect them.
   */

  if (!g_anotado.exchange(true)) {
    REXLOG_INFO("[escenario lod] el juego pasa a la malla buena por debajo de {:.1f} unidades; "
                "con el ajuste en {} se le adelanta esa decision",
                double(LeerFlotante(base, kUmbralDistancia)), adelanto);
  }

  if (adelanto < 0) {
    EscribirFlotante(base, salida, -1.0f);  // always below K, whatever K is
    ++g_forzadas;
    return;
  }
  const float d = LeerFlotante(base, salida);
  if (!(d > 0.0f) || d > 1.0e7f) {
    return;  // sanity: do not write over a value that is not a distance
  }
  EscribirFlotante(base, salida, d - float(adelanto));
  ++g_forzadas;
}

namespace nfsmw::escenario_lod {
// Called by the periodic report of the native renderer. If "forzadas" does not resemble "grandes",
// the hook is not reaching the objects and there is no need to look for the cause elsewhere.
std::string Resumen() {
  const uint64_t e = g_entradas.load(std::memory_order_relaxed);
  const uint64_t g = g_grandes.load(std::memory_order_relaxed);
  const uint64_t f = g_forzadas.load(std::memory_order_relaxed);
  return "escenario lod: " + std::to_string(e) + " objetos mirados, " + std::to_string(g) +
         " que el juego dibuja (>=18 px), " + std::to_string(f) + " con la malla buena forzada";
}
}  // namespace nfsmw::escenario_lod

// =================================================================================================
// DIAG (off by default, costs nothing when off): ScenerySectionHeader::DrawAScenery.
// r3 = this, r4 = instance number, r5 = SceneryCullInfo, r6 = visibility_state.
// After the call, if culling has added an object to the list (cull+140 advances 12 bytes), the pModel
// it wrote is read and compared with pModel[2] of the SceneryInfo to know whether it came out with the
// reduced mesh; the triangles of each mesh come from eSolid+0x14 (NumPolys).
// This gives the exact cost (triangles and draws) before raising the setting.
// =================================================================================================
// REX_EXTERN(__imp__sub_824C2850);
// REX_HOOK_RAW(sub_824C2850) { ... }
