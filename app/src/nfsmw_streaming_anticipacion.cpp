// nfsmw - the streamer requests the zone too late
//
// What it fixes
//   Facade popping and the stutter at the two corners and the alley are the same event: the detailed
//   scenery section arrives late. Until it is there, the game draws the distant stand-ins of the 'Z'
//   sections (Scenery.cpp:927) and the detailed one pops in when
//   GetScenerySectionHeader(section) stops being nullptr. It is not mesh LOD: when an object has no
//   reduced model of its own, pModel[0] and pModel[2] are the same pointer (Scenery.cpp:401-421).
//
//   The lever is when the section is requested, and TrackStreamer::GetPredictedZone decides that: it
//   projects the player's position forward and the zone at that point is the one queued for loading
//   (TrackStreamer::DetermineCurrentZones -> DetermineStreamingSections).
//   The game looks 1.5 s ahead and never more than 100 m. At 25 m/s (the speed of a corner or an
//   alley) that is 37 meters. That is why it arrives late exactly where it is noticeable.
//
// Where it is in the recompiled binary   (see "The proof" below)
//   sub_824BE0A8 = TrackStreamer::GetPredictedZone(StreamingPositionEntry* r4)
//                  app/generated/default/nfsmw_recomp.99.cpp:18500
//   sub_824BEF30 = TrackStreamer::DetermineCurrentZones   nfsmw_recomp.129.cpp:17543  (only caller)
//   sub_824BE4E8 = TrackStreamer::DetermineStreamingSections  nfsmw_recomp.88.cpp:18083
//   sub_824BA290 = TrackPathManager::FindZone             nfsmw_recomp.37.cpp:18450
//   sub_824C5DA0 = VisibleSectionManager::FindDrivableSection  nfsmw_recomp.77.cpp:18030
//   sub_824B9690 = TrackStreamingBarrier::Intersects
//   Globals: TheTrackPathManager = 0x82C59DA0, TheVisibleSectionManager = 0x82C52C48
//   Game constants (floats, big endian, in guest memory):
//     0x82060E74 = 1.5f      seconds of lookahead          (lfs f29,-5724(r28), r28 = 0x820624D0)
//     0x82040200 = 100.0f    cap in meters                 (lfs f31,512(r10),  r10 = 0x82040000)
//     0x820B069C = 178.816f  MPH2MPS(400), no prediction above it  (lfs f27,1692(r11))
//     0x82061CE8 = 0.0f      (the same base nfsmw_escenario_lod.cpp already used)
//
// The proof that sub_824BE0A8 is GetPredictedZone
//   1. Anchor: DetermineStreamingSections calls GetScenerySectionNumber('Y'/'X'/'Z', 0), which is
//      inline and equals (letter-'A'+1)*100 -> 2500 / 2400 / 2600. Those three immediates in a row
//      appear in only one place in the 141 MB of generated code: sub_824BE4E8. And its first call
//      is to sub_824BE340, which is RemoveCurrentStreamingSections, as in the source.
//   2. From there, in address order, sub_824BE0A8 falls where the source puts GetPredictedZone
//      (TrackStreamer.cpp:1457) and its body matches instruction by instruction:
//        lfs f13,12(r31) / fmuls / lfs f0,16(r31) / fmadds / fsqrts f30   -> speed = bLength(Velocity)
//        bl 0x824ba290 with r5=6 and r6=0                                 -> FindZone(&Position, 6, zone)
//        lfs f0,20(r30) ; fcmpu vs 0.0 ; fabs                             -> zone->GetElevation()
//        fcmpu f30 vs f27 ; ble                                           -> speed > MPH2MPS(400)
//        fmuls f7,f30,f29 ; fcmpu f7,f31 ; ble                            -> (speed*1.5f) > 100.0f
//        fdivs f0,f31,f30 ; fmadds f2,f4,f0,f6                            -> pos + vel*(100.0f/speed)
//        fmadds f10,f12,f29,f0                                            -> pos + vel*1.5f
//        bl 0x824c5da0 ; loop of 4 over zone+48 ; loop over 16-byte barriers ; extsh r3
//   3. The caller does `addi r4,r31,-36` and first checks `lbz r11,-8(r31)`: r31-8 is the same
//      object +0x1C, which is exactly StreamingPositionEntry::PositionSet (TrackStreamer.hpp:88).
//
// Why the constants are not patched, which would be the obvious way
//   Because the 1.5f and the 100.0f live in the executable's shared constant tables. The base of the
//   100.0f (0x82040000) is loaded from 126 different places and offset 512 appears 108 times; the
//   1.5f's base is the same table nfsmw_escenario_lod uses. Changing those four bytes would touch
//   half the game. Ruled out with data, not out of caution.
//
// How it is done instead: the velocity is faked, at exactly the right moment
//   The key is that GetPredictedZone reads the velocity twice:
//     a) on entry, to compute `speed` (it stays in f30, which is callee-saved and is not recomputed);
//     b) further down, again from memory, to build the offset (lfs f4,12(r31)).
//   And between (a) and (b) there is always a call to FindZone (sub_824BA290), because the offset is
//   computed inside the loop and the loop starts there.
//
//   So: the real velocity is left in place on entry (so `speed` is the real one and both comparisons,
//   the 178.8 m/s one and the 100 m one, are decided as always) and the fake velocity is written
//   right after the first FindZone. Then:
//       if the game took the 1.5 s branch    ->  predict = pos + v_falsa * 1.5f
//       if it took the 100 m cap branch      ->  predict = pos + v_falsa * (100.0f / speed_real)
//   In both cases |v_falsa| can be chosen so the projected distance is exactly the desired one, with
//   no cap and without touching any shared constant. And since `speed` is still the real one, the
//   178.8 m/s (644 km/h) guard cannot trip by accident.
//
// Safety
//   - The velocity is always restored on exit, to the exact previous value. GetPredictedZone does
//     not write to the entry: it only reads. The window is microseconds long and on the same thread.
//   - If sub_824BA290 were not FindZone, or if the map had no prediction zones, the change is not
//     applied and the game behaves as always. The "aplicadas" counter says so.
//   - Safety net: with the diagnostic on, it is also called without the lookahead. If the lookahead
//     overshoots and the prediction collapses (it returns 0, or stays in the current zone, which is
//     what the function returns when it cannot predict), the earlier zone is used and it is counted
//     as a rescue. It can never end up worse than the original game.
//   - Calling twice is safe: verified that both functions it uses only write to their own caches.
//     FindZone stores at TheTrackPathManager+12+76*type a box around the queried position, the hit
//     count and a query counter (and since the position is the same in both calls, the second one
//     comes from the cache and is almost free); on first reading it seemed to write to the caller's
//     r4, but r4 is reassigned first (addi r4,r31,20) and what it touches is the box, not the entry.
//     FindDrivableSection only moves a node to the front of its MRU list. Neither touches simulation
//     state.
//
// What is not touched
//   GetLoadingPriority (the queue priority) has not been located with certainty and is not touched:
//   it only orders what has already been requested. This function decides what gets requested.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>

/*
 * Why 250 % and 160 m, and not x4 and 300 m
 *
 * At 25 m/s (corner, alley) the game looks 37.5 m ahead. With 250 % that is 94 m: a bit more than
 * twice the margin exactly where the popping is visible, and at that distance the facade is already
 * outside the detail the eye tracks. At 60 m/s (highway) the game was already capped at 100 m and
 * goes to 160, which is the ceiling.
 *
 * Going higher has a real, measured cost: each extra zone is megabytes read from the SD, and a
 * measured run shows the game rereads 79 % to 95 % of the bytes every lap. Requesting 10 seconds
 * ahead (300 m at 30 m/s) can trade one stutter for four. With 250/160 the number of live sections
 * rises only a little and the 360 retail pool (~195 MB, `lis r10,3125` in nfsmw_recomp.44.cpp:13766)
 * has plenty of room.
 *
 * Both values can be changed from the toml without rebuilding; 100 % is exactly the original game.
 */
REXCVAR_DEFINE_INT32(nfsmw_streaming_anticipacion, 250, "NFSMW",
                     "Porcentaje sobre la anticipacion del streamer. 100 = el juego original "
                     "(1,5 s por delante). 250 = mira 2,5 veces mas lejos. El popping de fachadas "
                     "y el tiron al entrar en zona nueva salen de aqui")
    .range(100, 600);

REXCVAR_DEFINE_INT32(nfsmw_streaming_techo_m, 160, "NFSMW",
                     "Metros maximos que se le deja mirar por delante al streamer. 0 = el tope del "
                     "juego (100 m). Ojo: cada metro de mas son secciones de mas que leer de la SD")
    .range(0, 400);

REXCVAR_DEFINE_BOOL(nfsmw_streaming_diag, true, "NFSMW",
                    "Llama a la prediccion tambien SIN adelanto para poder comparar zona a zona y "
                    "para rescatar el resultado si el adelanto se pasa de largo. Cuesta dos "
                    "llamadas por servicio del streamer (2 por fotograma como mucho)");

namespace nfsmw::streaming_anticipacion {
namespace {

// Guest addresses. See the header comment for how they were found.
constexpr uint32_t kDirSegundos = 0x82060E74;  // float 1.5f
constexpr uint32_t kDirTecho = 0x82040200;     // float 100.0f
constexpr uint32_t kDirVelMax = 0x820B069C;    // float 178.816f = MPH2MPS(400)

// StreamingPositionEntry (TrackStreamer.hpp:82). Confirmed in the disassembly.
constexpr uint32_t kOffVelX = 12;        // bVector2 Velocity
constexpr uint32_t kOffVelY = 16;
constexpr uint32_t kOffZonaActual = 36;  // int16 CurrentZone (0x24)

constexpr uint32_t kDireccionMaxima = 0xE0000000u;  // same as in nfsmw_sombras_lod.cpp
constexpr uint32_t kCadaCuantasLineas = 1024;       // ~ one line every 8-17 s

// Sanity check on what is read from game memory: if it does not look like 1.5 s / 100 m, the
// addresses are not what we think and nothing at all must be done.
constexpr float kSegundosMin = 0.1f, kSegundosMax = 10.0f;
constexpr float kTechoMin = 10.0f, kTechoMax = 2000.0f;
constexpr float kLongitudFalsaMax = 1000.0f;  // m/s; only a vector, not a real speed

uint32_t Leer32(const uint8_t* base, uint32_t dir) {
  uint32_t v = 0;
  std::memcpy(&v, base + dir, sizeof(v));
  return __builtin_bswap32(v);
}

int32_t Leer16Con(const uint8_t* base, uint32_t dir) {
  uint16_t v = 0;
  std::memcpy(&v, base + dir, sizeof(v));
  return int16_t(__builtin_bswap16(v));
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

// Handoff state between the two hooks. Per thread on purpose: the streamer runs on the game thread,
// but half the world calls FindZone and there the hook must be a no-op.
thread_local uint32_t t_entrada = 0;   // the StreamingPositionEntry being predicted
thread_local float t_vel_x = 0.0f;     // fake velocity to leave in place
thread_local float t_vel_y = 0.0f;
thread_local bool t_armado = false;    // true only inside the lookahead call
thread_local bool t_aplicado = false;  // set by the FindZone hook when it actually writes

std::atomic<bool> g_presentado{false};    // the "this is what the game does" line
std::atomic<bool> g_constantes_mal{false};

// Counters. All relaxed: they are not compared with each other, only read when printing.
std::atomic<uint64_t> g_llamadas{0};   // times the hook was entered
std::atomic<uint64_t> g_actuadas{0};   // of those, the ones where a farther lookahead was requested
std::atomic<uint64_t> g_aplicadas{0};  // of those, the ones where the handoff actually wrote
std::atomic<uint64_t> g_distintas{0};  // times the requested zone changes due to the lookahead
std::atomic<uint64_t> g_rescates{0};   // times the lookahead overshot and the earlier zone was returned
std::atomic<uint64_t> g_sin_relevo{0};  // armed but FindZone was never called with the entry
// Sums for the "before" and "after" averages, in meters.
std::atomic<uint64_t> g_suma_antes_mm{0};
std::atomic<uint64_t> g_suma_despues_mm{0};

void Anotar(const uint8_t* base, int32_t mult_pct, int32_t techo_cvar) {
  if (g_presentado.exchange(true)) return;
  REXLOG_INFO(
      "[stream] el juego mira {:.2f} s por delante y nunca mas de {:.1f} m (y deja de predecir por "
      "encima de {:.1f} m/s); con anticipacion={} % y techo={} m se le pide mirar mas lejos",
      double(LeerFlotante(base, kDirSegundos)), double(LeerFlotante(base, kDirTecho)),
      double(LeerFlotante(base, kDirVelMax)), mult_pct,
      techo_cvar > 0 ? techo_cvar : int32_t(LeerFlotante(base, kDirTecho)));
}

void QuizaImprimir() {
  const uint64_t n = g_llamadas.load(std::memory_order_relaxed);
  if (n == 0 || (n % kCadaCuantasLineas) != 0) return;
  const uint64_t act = g_actuadas.load(std::memory_order_relaxed);
  const uint64_t apl = g_aplicadas.load(std::memory_order_relaxed);
  const double antes = act ? double(g_suma_antes_mm.load(std::memory_order_relaxed)) / act / 1000.0 : 0.0;
  const double desp = act ? double(g_suma_despues_mm.load(std::memory_order_relaxed)) / act / 1000.0 : 0.0;
  REXLOG_INFO(
      "[stream] C8 anticipacion: {} llamadas, {} con adelanto, {} aplicadas ({} sin relevo); "
      "mira {:.1f} m -> {:.1f} m de media; {} veces pide una zona distinta; {} rescates",
      n, act, apl, g_sin_relevo.load(std::memory_order_relaxed), antes, desp,
      g_distintas.load(std::memory_order_relaxed), g_rescates.load(std::memory_order_relaxed));
}

}  // namespace
}  // namespace nfsmw::streaming_anticipacion

// =================================================================================================
// TrackPathManager::FindZone(bVector2* pos, int type, TrackPathZone* prev)
//
// The handoff. It changes nothing by itself: it only writes the fake velocity when inside
// GetPredictedZone (t_armado) and the call is on the same entry being predicted (GetPredictedZone
// calls it with r4 = &entry->Position, which is the entry + 0). Otherwise it is two comparisons and
// nothing else.
//
// r4 is read before calling the original on purpose: r4 is volatile and the original clobbers it.
// =================================================================================================
REX_EXTERN(__imp__sub_824BA290);
REX_HOOK_RAW(sub_824BA290) {
  using namespace nfsmw::streaming_anticipacion;
  const uint32_t arg = ctx.r4.u32;
  __imp__sub_824BA290(ctx, base);
  if (!t_armado || arg == 0 || arg != t_entrada) {
    return;
  }
  EscribirFlotante(base, t_entrada + kOffVelX, t_vel_x);
  EscribirFlotante(base, t_entrada + kOffVelY, t_vel_y);
  t_aplicado = true;
}

// =================================================================================================
// TrackStreamer::GetPredictedZone(StreamingPositionEntry* r4) -> short
// Returns the section number of the zone the streamer is going to queue for loading.
// =================================================================================================
REX_EXTERN(__imp__sub_824BE0A8);
REX_HOOK_RAW(sub_824BE0A8) {
  using namespace nfsmw::streaming_anticipacion;

  g_llamadas.fetch_add(1, std::memory_order_relaxed);
  const int32_t mult_pct = REXCVAR_GET(nfsmw_streaming_anticipacion);
  const int32_t techo_cvar = REXCVAR_GET(nfsmw_streaming_techo_m);
  const uint32_t entrada = ctx.r4.u32;

  if (mult_pct <= 100 || g_constantes_mal.load(std::memory_order_relaxed) || entrada == 0 ||
      entrada + kOffZonaActual + 2u >= kDireccionMaxima) {
    __imp__sub_824BE0A8(ctx, base);
    QuizaImprimir();
    return;
  }

  // The game's own constants. If they are not what we expect, the addresses are wrong and this turns
  // off for good: better to do nothing than to move something we do not understand.
  const float k_seg = LeerFlotante(base, kDirSegundos);
  const float k_techo = LeerFlotante(base, kDirTecho);
  if (!(k_seg >= kSegundosMin && k_seg <= kSegundosMax) ||
      !(k_techo >= kTechoMin && k_techo <= kTechoMax)) {
    if (!g_constantes_mal.exchange(true)) {
      REXLOG_WARN("[stream] las constantes del streamer no cuadran (segundos={} tope={}): "
                  "anticipacion DESACTIVADA, el juego se queda como estaba",
                  double(k_seg), double(k_techo));
    }
    __imp__sub_824BE0A8(ctx, base);
    QuizaImprimir();
    return;
  }
  Anotar(base, mult_pct, techo_cvar);

  const float vx = LeerFlotante(base, entrada + kOffVelX);
  const float vy = LeerFlotante(base, entrada + kOffVelY);
  const float s = std::sqrt(vx * vx + vy * vy);
  // Stopped or nearly so: there is no direction to look ahead in and the game does not predict either.
  if (!std::isfinite(s) || s <= 1.0f) {
    __imp__sub_824BE0A8(ctx, base);
    QuizaImprimir();
    return;
  }

  const float techo = techo_cvar > 0 ? float(techo_cvar) : k_techo;
  const float antes = std::min(k_seg * s, k_techo);                          // what the game looks at
  const float despues = std::min(k_seg * s * (float(mult_pct) / 100.0f), techo);  // what we want
  if (!(despues > antes + 0.5f)) {
    __imp__sub_824BE0A8(ctx, base);
    QuizaImprimir();
    return;
  }

  /*
   * The length of the fake vector. The game picks the branch with the real speed (f30), which is
   * already computed before anything is written:
   *     cap branch     (s*k_seg  > k_techo):  predict = pos + v_falsa * (k_techo / s)
   *     normal branch  (s*k_seg <= k_techo):  predict = pos + v_falsa * k_seg
   * Solved so that the projected distance is exactly "despues".
   */
  float largo = (k_seg * s > k_techo) ? (despues * s / k_techo) : (despues / k_seg);
  if (!std::isfinite(largo) || largo <= 0.0f) {
    __imp__sub_824BE0A8(ctx, base);
    QuizaImprimir();
    return;
  }
  largo = std::min(largo, kLongitudFalsaMax);

  t_entrada = entrada;
  t_vel_x = vx / s * largo;
  t_vel_y = vy / s * largo;
  t_aplicado = false;

  // Reference without lookahead: it measures the real "before" value and serves the rescue. The
  // function only reads the entry, so calling it twice has no side effects.
  const bool diag = REXCVAR_GET(nfsmw_streaming_diag);
  int32_t zona_base = 0;
  if (diag) {
    // All 64 bits are saved, not just the low half: in the recompiled code `mr` copies the whole
    // register, and leaving the high half inconsistent is the kind of bug nobody sees.
    const uint64_t r3_orig = ctx.r3.u64;
    const uint64_t r4_orig = ctx.r4.u64;
    const uint64_t lr_orig = ctx.lr;
    t_armado = false;  // keep the handoff from touching anything in the reference call
    __imp__sub_824BE0A8(ctx, base);
    zona_base = int32_t(int16_t(uint16_t(ctx.r3.u32)));
    ctx.r3.u64 = r3_orig;
    ctx.r4.u64 = r4_orig;
    ctx.lr = lr_orig;
  }

  t_armado = true;
  __imp__sub_824BE0A8(ctx, base);
  t_armado = false;

  // Always restore, whether it was applied or not: writing back the same value costs nothing.
  EscribirFlotante(base, entrada + kOffVelX, vx);
  EscribirFlotante(base, entrada + kOffVelY, vy);
  t_entrada = 0;

  g_actuadas.fetch_add(1, std::memory_order_relaxed);
  g_suma_antes_mm.fetch_add(uint64_t(antes * 1000.0f), std::memory_order_relaxed);
  g_suma_despues_mm.fetch_add(uint64_t(despues * 1000.0f), std::memory_order_relaxed);
  if (t_aplicado) {
    g_aplicadas.fetch_add(1, std::memory_order_relaxed);
  } else {
    // The handoff did not engage: FindZone was never called with this entry, so the velocity was
    // never changed and the result is the game's own. If this number resembles the "con adelanto"
    // one, the hook is not doing anything and sub_824BA290 needs checking.
    g_sin_relevo.fetch_add(1, std::memory_order_relaxed);
  }

  if (diag) {
    const int32_t zona_ade = int32_t(int16_t(uint16_t(ctx.r3.u32)));
    if (zona_ade != zona_base) {
      g_distintas.fetch_add(1, std::memory_order_relaxed);
      /*
       * Rescue. If the projected point falls outside the zones the map has linked,
       * predict_position_used stays false and the function returns FindDrivableSection of the
       * current position, that is: zero lookahead, worse than the game. It is detected because the
       * lookahead result is 0 or the zone we are already in, while the game's was a different one.
       * In that case the game's result is returned, so this cannot make anything worse.
       */
      const int32_t zona_actual = Leer16Con(base, entrada + kOffZonaActual);
      if (zona_base != 0 && (zona_ade == 0 || zona_ade == zona_actual)) {
        ctx.r3.s64 = int64_t(int16_t(zona_base));
        g_rescates.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  QuizaImprimir();
}

namespace nfsmw::streaming_anticipacion {
// Available for the native renderer's periodic report, next to nfsmw::escenario_lod::Resumen()
// (nfsmw_nativo_sistema.cpp:2793). It is not needed there: the hook already prints its own
// "[stream] C8" line every 1,024 calls.
std::string Resumen() {
  const uint64_t n = g_llamadas.load(std::memory_order_relaxed);
  const uint64_t act = g_actuadas.load(std::memory_order_relaxed);
  return "anticipacion: " + std::to_string(n) + " predicciones, " + std::to_string(act) +
         " con adelanto, " + std::to_string(g_aplicadas.load(std::memory_order_relaxed)) +
         " aplicadas, " + std::to_string(g_distintas.load(std::memory_order_relaxed)) +
         " zonas distintas";
}
}  // namespace nfsmw::streaming_anticipacion
