// nfsmw - graphics work trimming during a race
//
// ===========================================================================
//  Why
//  Under Xenos emulation, a race ran at ~3 FPS with ~3,200 draws per frame,
//  at ~10,000 per second: every draw cost the same regardless of its pass.
//
//  How the game renders a race (recompiled code)
//    sub_824411B8  per-frame render: calls sub_82441100 and then
//                  sub_82445660.
//    sub_82441100  sub_8243FF30 enables views 1-5: byte +8 of each view
//                  in the table 0x82A38070 (112 bytes per view). In single
//                  player mode views 1 (scene) and 4 (reflection) remain.
//                  Then sub_824E4A20 -> sub_8243C6E0 enables the 6 faces of
//                  the car cubemap (pointers at 0x82A47E70) according to the
//                  row (counter 0x82A2CF24 % N of 0x828FB964) of the table
//                  0x828FBA88. It only prepares the views 1-17 that are active.
//    sub_82445660  with state 6 (0x82A39AD8) and no video, in this order:
//                  1600x1600 shadows, 640x360 road reflection (views
//                  4 and 5), 256x256 cubemap (views 18-23) and main view.
//    sub_824442E0  reflection pass: returns without doing anything if
//                  byte +8 of view 4 is 0.
//    sub_82444688  cubemap pass: skips each face whose byte +8 is 0.
//
//  What this file does, only in a race (state 6)
//  1. After sub_8243FF30, with nfsmw_reflejo_carretera = false, it disables
//     views 4 and 5 before they are prepared: they are not prepared, drawn or
//     resolved. The reflection texture keeps whatever it last held.
//     Not used on the Switch: the water samples that reflection, and the
//     sea in the coastal area came out black.
//  2. After sub_8243C6E0, with nfsmw_cubemap_caras_max = k (0..6), it leaves
//     at most k faces active per frame and rotates which ones, so all of
//     them get updated. With -1 it keeps the game's choice. The faces in
//     nfsmw_cubemap_caras_siempre do not count toward that limit: they are
//     updated every frame.
//  Outside a race it touches nothing: menus and garage stay as in retail.
//
//  The rear-view mirror
//  In a console recording the mirror refreshed ~6 times per second with the
//  scene at ~30: the game enables the 6 faces every frame (a single row,
//  "1 1 1 1 1 1") and with a 1-face limit each one is refreshed 1 of every
//  6 frames. The mirror comes from one of those faces.
//  - Pinning face 1 (view 19, mask 2), based on a misread diagnostic, left
//    the mirror just as slow on the console.
//  - Measured frame by frame on the PC (window recording and draws per copy
//    from C2): it comes from face 2, view 20, which C2 resolves into surface
//    07517000, and it is the most detailed one (380-530 draws; the others
//    7-90). With view 19 pinned it is refreshed 1 of every 5 frames; with
//    view 20 pinned (mask 4), every frame.
//  - Mask 4 costs ~+270 draws per frame in a race. With it the mirror was
//    confirmed fixed on the console, with the race at 26-31 FPS. It is the
//    default on the Switch.
//
//  How to disable it without a new build
//  In nfsmw.toml:   nfsmw_reflejo_carretera = true (already the default)
//                   nfsmw_cubemap_caras_max = -1
//                   nfsmw_cubemap_detalle_minimo = 0
//
//  ===========================================================================
//  What is really behind the two profile lines (race)
//
//  The C2 profile says "reflejo 0.95, 320 (cubo y desenfoque) 2.53" raw,
//  that is 1.55 and 4.12 ms real (x1.627). Both labels are misleading:
//
//  1) "reflejo" is not the road reflection. The category is decided by the
//     render target width (CategoriaDeDestino in nfsmw_nativo_dibujos.cpp):
//     everything with a pitch between 640 and 1279 lands there. What really
//     lands there is the 1024x576 output buffer (pitch 1040, height 576 =
//     599,040 texels), two passes per frame: 2 x 599,040 = 1.20 Mtexels,
//     which is exactly what C6 logs ("reflejo 1.20 Mtexels en 2.0 pases")...
//     and it logs the same in the menus, where there is no road and no
//     reflection. With 68 draws per frame in a race and 4 in the menu: it is
//     the final composite plus the HUD.
//     The road reflection (640x360, views 4 and 5) was off on the Switch in
//     this measurement and does not appear in any profile line: it was not
//     drawn, not resolved and cost nothing. Lowering its resolution or
//     alternating frames saves nothing because there is nothing to lower.
//     Turning it on costs ms; leaving it off is a visual difference from the
//     Xbox 360 (it is on now, see nfsmw_reflejo_carretera).
//
//  2) "320 (cubo y desenfoque)" is two different things mixed by pitch:
//     - The cubemap faces: 256x256 with pitch 320 (aligned to 80) = 81,920
//       texels per pass. Two faces are drawn per frame (C2 "caras
//       resueltas": each of the five that rotate shows up 41-52 times per
//       ~10 s, that is one per frame among the five, plus the pinned one).
//     - The bloom chain: one 512x288 pass (pitch 560, also lands here) and
//       three or four at 256x144 (pitch 320). Its 128x72 siblings (pitch
//       160) land in "menores".
//     Measured breakdown of the race frame (0.51 Mtexels, 6.2 passes, 311
//     draws, 4.12 ms real), with the known cost of 0.874 ms of GPU per 100
//     draws:
//       * the 311 cubemap face draws: ~2.7 ms  (66 %)
//       * the 6.2 open passes and their tile load: ~0.9 ms
//       * filling the 0.51 Mtexels: ~0.4 ms
//     So the cubemap does not cost pixels, it costs draws. Lowering the face
//     resolution barely touches those 2.7 ms; removing objects does.
//
//  3) The radial blur is no longer paid for. The real one (acceleration and
//     NOS) is 7 of the 12 samples of the composite quad (p_000139), and
//     nfsmw_nativo_sin_desenfoque removes it by default (1.5-2.0 ms real).
//     The log shows it applied: the specializations of those pipelines carry
//     bit 19 (0x80000). The seven samples read the same texture as the
//     center one (DIFFUSEMAP, the scene), not a separate chain, so removing
//     it leaves no orphan pass to cut. What remains in the "320" bucket is
//     not that blur: it is the bloom, and that one is visible.
// ===========================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_informe_diferido.h"  // deferred reports
#include "nfsmw_reflejo_demanda.h"
#include <rex/platform.h>

/*
 * On by default on the Switch as well: without it the sea in the coastal area came out black.
 *
 * The water samples this reflection's texture (views 4 and 5). With the trim, that texture is never
 * repainted during a race: on the console the sea in Heritage & Omega came out black, and on the PC, with
 * the trim forced on, flat and dark, without the sky or the cliff reflected. The trim dates from the Xenos
 * emulation era, when races ran below 10 FPS; today it changes the look, and graphics are not traded for
 * FPS.
 * It costs ~109 draws per frame (14 k triangles at 640x360).
 */
REXCVAR_DEFINE_BOOL(nfsmw_reflejo_carretera, true, "NFSMW",
                    "Dibujar el reflejo de la carretera (pasada 640x360) durante la carrera; el agua tambien "
                    "lo muestrea, sin el el mar sale negro")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

/*
 * The reflection, only when something reads it.
 *
 * Once enabled for the sea, the reflection was drawn in every race frame: ~210 draws on average, up to 280
 * in Heritage & Omega. In one run, 42 % of those draws were for copies nobody read: halfway through
 * Heritage the C2 report said "0 lecturas", and in Ironwood it was only read in 37 % of frames. With the
 * ring as the bottleneck, that is ~2 ms of CPU per frame wasted in those stretches.
 *
 * The renderer records when the reflection texture is read (nfsmw_reflejo_demanda.h). If it was read in
 * the last 20 frames it is always drawn, as before. Otherwise only 1 of every nfsmw_reflejo_refresco: when
 * the water becomes visible again, its first frame reads a reflection at most 3 frames old (~100 ms) and
 * the next one is already up to date.
 * Self-checking guard: in each run, for the first 90 frames it is always drawn and the guard checks that
 * address 0x07C5A000 really is the reflection; if not, or if it keeps being resolved in skipped frames, it
 * turns itself off and says so in the log. false = always draw it.
 */
REXCVAR_DEFINE_BOOL(nfsmw_reflejo_bajo_demanda, true, "NFSMW",
                    "Dibujar el reflejo de la carretera solo cuando algo lo lee; sin lecturas recientes se renueva 1 de "
                    "cada nfsmw_reflejo_refresco fotogramas")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Measured on the PC (standing at the Heritage start): with 16 instead of the game's 6 the reflection
 * stays at 295-331 draws per copy (317-327 without it): its objects are already large. It gains nothing,
 * so it is off.
 */
REXCVAR_DEFINE_INT32(nfsmw_reflejo_detalle_minimo, 0, "NFSMW",
                     "Objetos de menos de N pixeles que no se dibujan en el reflejo de la carretera (vistas 4 y 5), como "
                     "nfsmw_cubemap_detalle_minimo en el cubo; 0 = el valor del juego (medido sin efecto con 16)")
    .range(0, 64)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_reflejo_refresco, 4, "NFSMW",
                     "Con nfsmw_reflejo_bajo_demanda: cada cuantos fotogramas se renueva el reflejo mientras nadie lo "
                     "lee (el agua que aparece lo ve como mucho con ese retraso)")
    .range(1, 30)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

/*
 * The reflection, only if the water is really visible.
 *
 * Deciding by reads alone, the reflection stopped being refreshed in stretches without reads (up to 75 %),
 * but at the Heritage & Omega start it was read in 100 % of frames, with 175-338 draws per copy, and that
 * is exactly the area that drops to 21 FPS. In PC captures of that start (at 100, 110 and 120 s) the sea
 * does not appear: the game submits the water because it falls inside the frustum, and the terrain covers
 * it completely.
 *
 * Now the ring wraps every draw that samples the reflection in an occlusion query and, when the GPU
 * finishes that work (1-2 frames later), records whether it left any sample. If none of those draws has
 * left a single sample in the last 20 frames, the reflection is refreshed 1 of every
 * nfsmw_reflejo_refresco, as when nobody reads it. A draw with no samples wrote nothing to any render
 * target, so that reflection never reaches the image. The only change is when the water reappears: its
 * first 3-4 frames show a reflection up to ~7 frames old.
 * Whatever cannot be measured (no room for the query, a game query already open, the deferred sky) is
 * treated as visible.
 * Self-checking guard: the final race composite (p_000139, full screen) is measured the same way every so
 * often and must leave samples; until 8 good witnesses, the decision is still made by reads, and if any
 * gives 0 it falls back to reads for the whole run and says so in the log. false = by reads.
 */
REXCVAR_DEFINE_BOOL(nfsmw_reflejo_visibilidad, true, "NFSMW",
                    "Con nfsmw_reflejo_bajo_demanda: el reflejo se renueva en todos los fotogramas solo si el agua deja "
                    "alguna muestra en pantalla (consulta de oclusion), y no solo porque se mande a dibujar")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace nfsmw::reflejo_demanda {
namespace {
std::atomic<uint64_t> g_swaps{0};
std::atomic<uint64_t> g_ultima_lectura{0};  // Swap number of the last read, plus 1 (0 = never)
std::atomic<uint64_t> g_lecturas{0};
std::atomic<uint64_t> g_copias{0};
// nfsmw_reflejo_visibilidad. The ring thread writes these and the game thread reads them.
constexpr uint64_t kTestigosParaAplicar = 8;
enum FaseVisibilidad : int { kVisMirando = 0, kVisAplicando = 1, kVisApagada = 2 };
std::atomic<int> g_fase_visibilidad{kVisMirando};
std::atomic<uint64_t> g_ultima_visible{0};  // Swap number when the last query with samples was read, plus 1
std::atomic<uint64_t> g_visibles{0};
std::atomic<uint64_t> g_ocultos{0};
std::atomic<uint64_t> g_sin_medida{0};
std::atomic<uint64_t> g_testigos_bien{0};
std::atomic<uint64_t> g_testigos_mal{0};
}  // namespace

void AnotarLectura() {
  g_lecturas.fetch_add(1, std::memory_order_relaxed);
  g_ultima_lectura.store(g_swaps.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

void AnotarCopia() { g_copias.fetch_add(1, std::memory_order_relaxed); }

void AnotarSwap() { g_swaps.fetch_add(1, std::memory_order_relaxed); }

bool MedirVisibilidad() {
  return REXCVAR_GET(nfsmw_reflejo_visibilidad) && REXCVAR_GET(nfsmw_reflejo_bajo_demanda) &&
         REXCVAR_GET(nfsmw_reflejo_carretera) &&
         g_fase_visibilidad.load(std::memory_order_relaxed) != kVisApagada;
}

bool VisibilidadComprobada() { return g_fase_visibilidad.load(std::memory_order_relaxed) == kVisAplicando; }

void AnotarVisible(bool medido) {
  (medido ? g_visibles : g_sin_medida).fetch_add(1, std::memory_order_relaxed);
  g_ultima_visible.store(g_swaps.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

void AnotarOculto() { g_ocultos.fetch_add(1, std::memory_order_relaxed); }

void AnotarTestigo(bool con_muestras) {
  if (!con_muestras) {
    g_testigos_mal.fetch_add(1, std::memory_order_relaxed);
    if (g_fase_visibilidad.exchange(kVisApagada, std::memory_order_relaxed) != kVisApagada) {
      REXLOG_ERROR("[recortes] reflejo por visibilidad: DIFERENCIA, la composicion final (pantalla entera) dio 0 "
                   "muestras en su consulta de oclusion tras {} testigos buenos; las consultas no son de fiar y el "
                   "reflejo vuelve a decidirse por lecturas (como la 191) el resto de la sesion",
                   g_testigos_bien.load(std::memory_order_relaxed));
    }
    return;
  }
  const uint64_t bien = g_testigos_bien.fetch_add(1, std::memory_order_relaxed) + 1;
  int esperada = kVisMirando;
  if (bien >= kTestigosParaAplicar &&
      g_fase_visibilidad.compare_exchange_strong(esperada, kVisAplicando, std::memory_order_relaxed)) {
    REXLOG_INFO("[recortes] reflejo por visibilidad: comprobado ({} testigos de la composicion final con muestras, 0 "
                "sin ninguna); desde aqui el reflejo se renueva en todos los fotogramas solo si el agua deja muestras en "
                "pantalla",
                bien);
  }
}

}  // namespace nfsmw::reflejo_demanda

REXCVAR_DEFINE_INT32(nfsmw_cubemap_caras_max, REX_PLATFORM_SWITCH != 0 ? 1 : -1, "NFSMW",
                     "Caras del mapa de entorno del coche que se actualizan por fotograma en "
                     "carrera (-1 = las que decide el juego)")
    .range(-1, 6)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// On the Switch it is 4: face 2 (view 20), the rear-view mirror's. Confirmed fixed on the console (mask 4,
// race at 26-31 FPS). It costs ~+270 draws per frame. It used to be 2: face 1 (view 19), which is not the
// mirror's.
/*
 * The rear-view mirror draws 415 objects into one 256x256 face.
 *
 * That is 86 % of the cubemap cost and 19.6 % of all the draws in the frame. And it is not using more
 * detail: it draws at 61.5 triangles per object, almost the same as the main scene (68.1), in 1/14 of the
 * area. Its density is 6,336 draws per Mpixel against 1,117 for the screen: 5.7 times.
 *
 * The reason is in the binary, not an assumption. sub_82216600 builds the draw-list filter mask of each
 * view and ends by writing it to vista+132. Inside it (nfsmw_recomp.17, loc_822166D4) there is this:
 *
 *     if (viewId == 20) { mascara &= ~0x40; mascara &= ~0x08000000; }
 *
 * So the game removes, from view 20 and only from it, two culling bits that the other five cubemap faces
 * do carry. That is why the mirror walks the whole world while its siblings draw between 6 and 95 objects.
 *
 * This cvar gives them back. It is reversible in one line and does not touch the mirror's refresh rate,
 * which still updates every frame.
 *
 * Verified on the console with this enabled; not to be re-checked.
 */
REXCVAR_DEFINE_BOOL(nfsmw_retrovisor_recorte, true, "NFSMW",
                    "Devolver al retrovisor (vista 20) los dos bits de recorte de la lista de dibujo que el "
                    "juego le quita a el solo. Dibuja 415 objetos donde sus hermanas dibujan 6-95, y son el "
                    "19,6 % de los dibujos del fotograma");

REXCVAR_DEFINE_INT32(nfsmw_cubemap_caras_siempre, REX_PLATFORM_SWITCH != 0 ? 4 : 0, "NFSMW",
                     "Caras del mapa de entorno que se actualizan todos los fotogramas aunque haya "
                     "limite (bit i = cara i de la tabla del juego; no cuentan para "
                     "nfsmw_cubemap_caras_max)")
    .range(0, 63)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

/*
 * The cubemap costs per draw, so it gets fewer draws.
 *
 * The face refreshed every frame draws between 10 and 100 objects (C2 "caras resueltas", "dibujos por
 * copia"), and at 0.874 ms of GPU per 100 draws that is most of the pass's 4.12 ms real. The game decides
 * which object enters a view with eView::PixelMinSize: in WorldModel::Render and in CarRender, if the
 * object's projected size is below that number, the call bails out before drawing (saving CPU and GPU at
 * once). The eView constructor sets it to 4 for every view, so a 256x256 environment map face applies the
 * same threshold as the whole screen.
 *
 * An object covering 8 pixels of a 256x256 face ends up, reflected on the curved body of a moving car,
 * well below one pixel on screen. Raising the threshold only on the rotating faces removes exactly those
 * objects.
 *
 * Since the field offset is deduced from the decompilation (nfsmwdecomp, eView +0x24 in a 112-byte
 * structure) and cannot be tested here, the code checks the layout before writing: the seven views it
 * cares about (the scene and the six faces) must read exactly the constructor's 4. If a single one does
 * not match, it writes nothing and logs it.
 *
 * How to measure it: in the log, "C2 caras resueltas desde el informe anterior" gives the draws per copy
 * of each face. On the PC it shows right away because all six are drawn every frame there.
 */
REXCVAR_DEFINE_INT32(nfsmw_cubemap_detalle_minimo, 8, "NFSMW",
                     "Tamano minimo en pixeles para que un objeto se dibuje en una cara del mapa de "
                     "entorno del coche (eView::PixelMinSize; el juego usa 4 en todas las vistas). Solo "
                     "se aplica a las caras que rotan, nunca a las fijas. 0 = dejar el valor del juego")
    .range(0, 64);

REXCVAR_DEFINE_INT32(nfsmw_cubemap_diag_ciclo_s, 0, "NFSMW",
                     "Diagnostico: con N > 0, en carrera solo se actualiza una cara del mapa de entorno "
                     "y cambia de cara cada N segundos (anota cada cambio), para ver cual usa el "
                     "retrovisor")
    .range(0, 60);

namespace nfsmw::recortes_carrera {
namespace {

constexpr uint32_t kBaseVistas = 0x82A38070;
constexpr uint32_t kBytesPorVista = 112;
constexpr uint32_t kVistas = 24;
constexpr uint32_t kOffActiva = 8;
constexpr uint32_t kVistaReflejo = 4;
constexpr uint32_t kVistaReflejoSegundo = 5;
constexpr uint32_t kEstadoJuego = 0x82A39AD8;
constexpr uint32_t kEstadoCarrera = 6;
constexpr uint32_t kCaras = 6;
constexpr uint32_t kNumFilasCaras = 0x828FB964;
constexpr uint32_t kFilasCaras = 0x828FBA88;
constexpr uint32_t kVistaEscena = 1;

/*
 * eView fields (nfsmwdecomp, src/Speed/Indep/Src/Ecstasy/Ecstasy.hpp). The class is 0x68 bytes and is
 * aligned to 16 because of the bVector3 at +0x28, hence the 112 bytes per view in this table. What this
 * file already used matches the decompilation: ID at +4 and Active at +8.
 */
constexpr uint32_t kOffH = 0x0C;               // float, the view's "pixel" scale
constexpr uint32_t kOffNearZ = 0x10;           // float
constexpr uint32_t kOffFarZ = 0x14;            // float
constexpr uint32_t kOffFovBias = 0x18;         // float
constexpr uint32_t kOffFovGrados = 0x1C;       // float
constexpr uint32_t kOffByN = 0x20;             // int BlackAndWhiteMode
constexpr uint32_t kOffDetalleMinimo = 0x24;   // int PixelMinSize
// bVector3 ViewDirection, eView+0x28 (Ecstasy.hpp:166). It is the direction the view points to, and it
// tells which cubemap face is which without guessing.
constexpr uint32_t kOffDireccion = 0x28;       // bVector3 ViewDirection (x,y,z floats)
constexpr int32_t kDetalleMinimoJuego = 4;     // eView::eView() sets it to 4 in every view

void Escribir32(uint8_t* base, uint32_t direccion, uint32_t valor) {
  const uint32_t v = __builtin_bswap32(valor);
  std::memcpy(base + direccion, &v, sizeof(v));
}

uint32_t Leer32(const uint8_t* base, uint32_t direccion) {
  uint32_t v = 0;
  std::memcpy(&v, base + direccion, sizeof(v));
  return __builtin_bswap32(v);
}

uint32_t DireccionActiva(uint32_t vista) {
  return kBaseVistas + vista * kBytesPorVista + kOffActiva;
}

float LeerFlotante(const uint8_t* base, uint32_t direccion) {
  const uint32_t v = Leer32(base, direccion);
  float f = 0.0f;
  std::memcpy(&f, &v, sizeof(f));
  return f;
}

uint32_t DireccionVista(uint32_t vista) {
  return kBaseVistas + vista * kBytesPorVista;
}

bool EnCarrera(const uint8_t* base) {
  return Leer32(base, kEstadoJuego) == kEstadoCarrera;
}

// Only pointers to the start of a view in the table are accepted.
bool EsVista(uint32_t direccion) {
  return direccion >= kBaseVistas && direccion < kBaseVistas + kVistas * kBytesPorVista &&
         (direccion - kBaseVistas) % kBytesPorVista == 0;
}

int NumeroDeVista(uint32_t direccion) {
  return EsVista(direccion) ? int((direccion - kBaseVistas) / kBytesPorVista) : -1;
}

std::atomic<bool> g_tabla_registrada{false};
std::atomic<bool> g_aviso_reflejo{false};
std::atomic<bool> g_aviso_caras{false};
std::atomic<bool> g_aviso_puntero{false};
std::atomic<bool> g_aviso_vistas_caras{false};
// Set only once all six faces have been named, not when trying.
std::atomic<bool> g_aviso_caras_nombradas{false};
std::atomic<uint32_t> g_rotacion{0};
// Faces already drawn at least once in this race. Until all six are, none is trimmed: a delayed
// reflection is acceptable, a black one is not.
constexpr uint32_t kTodasLasCaras = (1u << kCaras) - 1u;
uint32_t g_caras_estrenadas = 0;
std::atomic<bool> g_aviso_estreno{false};
std::atomic<bool> g_aviso_estreno_falta{false};
std::atomic<bool> g_aviso_retrovisor{false};
std::atomic<int> g_cara_diag{-1};
// Environment map minimum detail: 0 = nothing written to that face yet.
// Only touched from the game thread, inside the sub_8243C6E0 hook.
int32_t g_detalle_aplicado[kCaras] = {0, 0, 0, 0, 0, 0};
/*
 * The value the game really puts in each face, learned the first time it is seen. An earlier version
 * assumed it was 4 (what the constructor sets), and so the threshold was never applied: measured on the
 * console, the game uses 12 in the scene and 2 in the cubemap faces, so the "if it is not the game's
 * value, someone else is in charge" guard always rejected it. Result: the six faces cost +2.14 ms without
 * the saving meant to offset them. -1 = not seen yet.
 */
int32_t g_detalle_del_juego[kCaras] = {-1, -1, -1, -1, -1, -1};
int g_layout_vista = 0;  // 0 = unchecked, 1 = matches, -1 = mismatch (never written)
std::atomic<bool> g_aviso_detalle{false};

// In how many race frames the mirror face (view 20) is updated, logged every 10 s. Counted after
// sub_8243FF30, which only enables views 1-5: the table keeps the previous frame's final activation, with
// the face limit already applied. Only from the game's main thread. The face being active does not
// guarantee the mirror changes (with the wrong face pinned this read 100 %): the real rate is measured
// on the image.
constexpr uint32_t kVistaRetrovisor = 20;
struct ContadorRetrovisor {
  int64_t desde_ms = 0;
  uint32_t fotogramas = 0;
  uint32_t actualizada = 0;
};
ContadorRetrovisor g_retrovisor;

void ContarRetrovisor(const uint8_t* base) {
  if (!EnCarrera(base)) {
    g_retrovisor = {};
    return;
  }
  using namespace std::chrono;
  const int64_t ahora = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  if (g_retrovisor.desde_ms == 0) {
    g_retrovisor.desde_ms = ahora;  // the race's first frame does not have the limited table yet
    return;
  }
  ++g_retrovisor.fotogramas;
  if (base[DireccionActiva(kVistaRetrovisor)] != 0) {
    ++g_retrovisor.actualizada;
  }
  if (ahora - g_retrovisor.desde_ms >= 10000) {
    NFSMW_INFORME_DIFERIDO("[recortes] carrera, ultimos {:.1f} s: {} fotogramas; la cara del retrovisor (vista 20) se "
                "actualizo en {} ({:.0f} %)",
                double(ahora - g_retrovisor.desde_ms) / 1000.0, g_retrovisor.fotogramas,
                g_retrovisor.actualizada,
                100.0 * double(g_retrovisor.actualizada) / double(std::max<uint32_t>(g_retrovisor.fotogramas, 1)));
    g_retrovisor = {ahora, 0, 0};
  }
}

void RegistrarTablaCaras(const uint8_t* base) {
  if (g_tabla_registrada.exchange(true)) {
    return;
  }
  const uint32_t filas = Leer32(base, kNumFilasCaras);
  REXLOG_INFO("[recortes] cubemap del juego: {} fila(s) de activacion de caras", filas);
  if (filas == 0 || filas > 32) {
    return;
  }
  for (uint32_t f = 0; f < filas; ++f) {
    const uint32_t d = kFilasCaras + 24 * f;
    REXLOG_INFO("[recortes] fila {}: {} {} {} {} {} {}", f, Leer32(base, d), Leer32(base, d + 4),
                Leer32(base, d + 8), Leer32(base, d + 12), Leer32(base, d + 16),
                Leer32(base, d + 20));
  }
}

/*
 * Before writing a single byte into the view structure, the location of PixelMinSize must be certain.
 * The required signature is strong: the scene view and the six cubemap faces must read exactly the 4 the
 * eView constructor sets. A wrong offset would land on FovDegrees (a float such as 90), on ViewDirection
 * (floats) or on pCamera (a 0x8xxxxxxx pointer): all seven reading 4 right there is practically
 * impossible. The whole window is also logged once, so it can be confirmed in the console log.
 */
bool ComprobarLayoutDeVista(const uint8_t* base, uint32_t tabla) {
  if (g_layout_vista != 0) {
    return g_layout_vista > 0;
  }
  /*
   * The field is checked for a plausible value, not a specific number. Requiring exactly 4 (what
   * eView::eView sets) in the scene and the six faces never holds in a race: measured on the console, the
   * scene uses 12 and the faces 2. The check always returned false and nfsmw_cubemap_detalle_minimo
   * touched nothing, which cost 2.14 ms.
   */
  const uint32_t escena = DireccionVista(kVistaEscena);
  auto plausible = [&](uint32_t v) {
    const int32_t d = int32_t(Leer32(base, v + kOffDetalleMinimo));
    return d > 0 && d <= 64;
  };
  bool cuadra = plausible(escena);
  for (uint32_t i = 0; i < kCaras && cuadra; ++i) {
    const uint32_t vista = Leer32(base, tabla + 4 * i);
    if (!EsVista(vista)) {
      cuadra = false;
      break;
    }
    cuadra = plausible(vista);
  }
  const uint32_t cara0 = Leer32(base, tabla);
  if (EsVista(cara0)) {
    REXLOG_INFO("[recortes] cubo, cara 0 (vista {}): H {:.1f} cerca {:.2f} lejos {:.1f} fovbias {:.3f} "
                "fov {:.1f} byn {} detalle {}; escena detalle {}",
                NumeroDeVista(cara0), LeerFlotante(base, cara0 + kOffH),
                LeerFlotante(base, cara0 + kOffNearZ), LeerFlotante(base, cara0 + kOffFarZ),
                LeerFlotante(base, cara0 + kOffFovBias), LeerFlotante(base, cara0 + kOffFovGrados),
                Leer32(base, cara0 + kOffByN), int32_t(Leer32(base, cara0 + kOffDetalleMinimo)),
                int32_t(Leer32(base, escena + kOffDetalleMinimo)));
  }
  g_layout_vista = cuadra ? 1 : -1;
  if (!cuadra) {
    REXLOG_WARN("[recortes] cubo: la estructura de la vista no cuadra (PixelMinSize en +0x{:X} "
                "deberia ser un entero entre 1 y 64 en la escena y en las seis caras); "
                "nfsmw_cubemap_detalle_minimo no va a tocar nada",
                kOffDetalleMinimo);
  } else {
    REXLOG_INFO("[recortes] cubo: la estructura de la vista cuadra; la escena usa PixelMinSize {} y "
                "las caras {} {} {} {} {} {}",
                int32_t(Leer32(base, escena + kOffDetalleMinimo)),
                int32_t(Leer32(base, Leer32(base, tabla) + kOffDetalleMinimo)),
                int32_t(Leer32(base, Leer32(base, tabla + 4) + kOffDetalleMinimo)),
                int32_t(Leer32(base, Leer32(base, tabla + 8) + kOffDetalleMinimo)),
                int32_t(Leer32(base, Leer32(base, tabla + 12) + kOffDetalleMinimo)),
                int32_t(Leer32(base, Leer32(base, tabla + 16) + kOffDetalleMinimo)),
                int32_t(Leer32(base, Leer32(base, tabla + 20) + kOffDetalleMinimo)));
  }
  return cuadra;
}

/*
 * Raises the object size threshold only on the rotating faces. Never on the pinned ones
 * (nfsmw_cubemap_caras_siempre) and never on view 20, whatever the cvars say.
 */
void AjustarDetalleCubo(uint8_t* base, uint32_t tabla, uint32_t cara, uint32_t vista,
                        uint32_t siempre) {
  if (cara >= kCaras || (siempre & (1u << cara)) || NumeroDeVista(vista) == int(kVistaRetrovisor)) {
    return;
  }
  const int32_t actual = int32_t(Leer32(base, vista + kOffDetalleMinimo));
  /*
   * The game's value is learned, not assumed. The first time a face is seen, whatever it holds is the
   * game's value (nothing has been written to it yet). Assuming the constructor's 4 did not work: the
   * faces hold 2 in a race. It must be plausible, so garbage from a half-initialized view is not learned.
   */
  if (g_detalle_del_juego[cara] < 0) {
    if (actual <= 0 || actual > 64) {
      return;  // view not initialized: retry on the next frame
    }
    g_detalle_del_juego[cara] = actual;
  }
  const int32_t pedido = REXCVAR_GET(nfsmw_cubemap_detalle_minimo);
  const int32_t deseado = pedido > 0 ? pedido : g_detalle_del_juego[cara];
  if (actual == deseado) {
    return;
  }
  // Only overwrite what the game or we put there: any other value means someone else is in charge, so it
  // is left alone.
  if (actual != g_detalle_del_juego[cara] && actual != g_detalle_aplicado[cara]) {
    return;
  }
  if (!ComprobarLayoutDeVista(base, tabla)) {
    return;
  }
  Escribir32(base, vista + kOffDetalleMinimo, uint32_t(deseado));
  g_detalle_aplicado[cara] = deseado;
  if (!g_aviso_detalle.exchange(true)) {
    REXLOG_INFO("[recortes] cubo: los objetos de menos de {} pixeles dejan de dibujarse en las caras "
                "que rotan (el juego usa {}); las caras fijas no se tocan",
                deseado, kDetalleMinimoJuego);
  }
}

// Mirror diagnostic: a single active face, changing every 'ciclo_s' seconds.
void DiagnosticoUnaCara(uint8_t* base, uint32_t tabla, int32_t ciclo_s) {
  using namespace std::chrono;
  const int64_t ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  const int cara = int((ms / (int64_t(ciclo_s) * 1000)) % kCaras);
  for (uint32_t i = 0; i < kCaras; ++i) {
    const uint32_t vista = Leer32(base, tabla + 4 * i);
    if (EsVista(vista) && int(i) != cara) {
      base[vista + kOffActiva] = 0;
    }
  }
  if (g_cara_diag.exchange(cara) != cara) {
    REXLOG_INFO("[recortes] diagnostico: solo la cara {} del cubemap (vista {})", cara,
                NumeroDeVista(Leer32(base, tabla + 4 * uint32_t(cara))));
  }
}

/*
 * Which cubemap face is which.
 *
 * Only face 2 (view 20, the mirror) had been confirmed, by trial and error: pinning face 1 in the belief
 * that it was the mirror left the mirror just as slow. The other five were unknown, so there was no way
 * to decide which ones are worth refreshing more often for the car body reflection without paying for
 * all of them.
 *
 * `eView` has `bVector3 ViewDirection` at +0x28 (Ecstasy.hpp:166): the direction the view looks at. With
 * it each face is named without guessing. The game's Y axis is height (the cameras use Position.y for
 * elevation in GetPredictedZone), so +Y = sky and -Y = ground.
 *
 * Logged once per run, on entering a race. Cost: six 12-byte reads.
 */
const char* NombreDeDireccion(float x, float y, float z) {
  const float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, az = z < 0 ? -z : z;
  if (ay >= ax && ay >= az) return y > 0 ? "ARRIBA (cielo)" : "ABAJO (suelo)";
  if (az >= ax) return z > 0 ? "DELANTE" : "DETRAS";
  return x > 0 ? "DERECHA" : "IZQUIERDA";
}

void DiagnosticoCarasDelCubo(const uint8_t* base, uint32_t tabla) {
  /*
   * This cannot be read on the first call: there all six faces came out with direction 0,0,0, fov 0 and
   * far 0, because the cubemap views did not have their parameters yet. It waits until face 0 has a
   * believable fov and far distance; until then it retries on the next frame.
   */
  for (uint32_t i = 0; i < kCaras; ++i) {
    const uint32_t vista = Leer32(base, tabla + 4 * i);
    if (!EsVista(vista)) {
      return;  // no valid table yet: retry
    }
  }
  const uint32_t cara0 = Leer32(base, tabla);
  const float fov0 = LeerFlotante(base, cara0 + kOffFovGrados);
  const float lejos0 = LeerFlotante(base, cara0 + kOffFarZ);
  if (!(fov0 > 1.0f) || !(lejos0 > 1.0f)) {
    return;  // views half-initialized: retry on the next frame
  }
  const uint32_t siempre = uint32_t(REXCVAR_GET(nfsmw_cubemap_caras_siempre));
  const int32_t maximo = REXCVAR_GET(nfsmw_cubemap_caras_max);
  for (uint32_t i = 0; i < kCaras; ++i) {
    const uint32_t vista = Leer32(base, tabla + 4 * i);
    const float x = LeerFlotante(base, vista + kOffDireccion);
    const float y = LeerFlotante(base, vista + kOffDireccion + 4);
    const float z = LeerFlotante(base, vista + kOffDireccion + 8);
    const bool fija = (siempre & (1u << i)) != 0;
    REXLOG_INFO("[recortes] cubo cara {} = vista {} -> {} (dir {:+.2f} {:+.2f} {:+.2f}); "
                "fov {:.1f} lejos {:.0f} detalle {}; {}",
                i, NumeroDeVista(vista), NombreDeDireccion(x, y, z), double(x), double(y), double(z),
                double(LeerFlotante(base, vista + kOffFovGrados)),
                double(LeerFlotante(base, vista + kOffFarZ)),
                int32_t(Leer32(base, vista + kOffDetalleMinimo)),
                fija ? "SIEMPRE (no cuenta para el limite)" : "rota con las demas");
  }
  const int rotan = int(kCaras) - __builtin_popcount(siempre & 0x3Fu);
  REXLOG_INFO("[recortes] cubo: {} caras rotan a {} por fotograma, asi que cada una se renueva 1 de "
              "cada {:.1f} fotogramas; las fijas, en todos",
              rotan, maximo,
              maximo > 0 ? double(rotan) / double(maximo > rotan ? rotan : maximo) : double(rotan));
  g_aviso_caras_nombradas.store(true, std::memory_order_relaxed);
}

// nfsmw_reflejo_bajo_demanda. Only from the game's main thread (sub_8243FF30 hook).
constexpr uint32_t kReflejoGracia = 20;    // renderer frames without a read before spacing it out
constexpr uint32_t kReflejoMirando = 90;   // frames always drawing it, to check the address
enum class FaseReflejo { kMirando, kAplicando, kApagado };
struct EstadoReflejo {
  FaseReflejo fase = FaseReflejo::kMirando;
  uint32_t mirando = 0;
  uint64_t copias_inicio = 0;
  uint32_t desde_dibujo = 0;
  int64_t desde_ms = 0;
  uint32_t pedidos = 0;    // race frames in which the game requests it
  uint32_t dibujados = 0;  // of those, the ones allowed to draw
  uint64_t copias_informe = 0;
  uint64_t lecturas_informe = 0;
  uint64_t visibles_informe = 0;   // nfsmw_reflejo_visibilidad
  uint64_t ocultos_informe = 0;
  uint64_t sin_medida_informe = 0;
};
EstadoReflejo g_reflejo;

const char* NombreFase(FaseReflejo fase) {
  return fase == FaseReflejo::kAplicando ? "APLICANDO" : fase == FaseReflejo::kMirando ? "mirando" : "APAGADA";
}

void InformeReflejo(int64_t ahora) {
  namespace rd = nfsmw::reflejo_demanda;
  EstadoReflejo& e = g_reflejo;
  const uint64_t copias = rd::g_copias.load(std::memory_order_relaxed);
  const uint64_t lecturas = rd::g_lecturas.load(std::memory_order_relaxed);
  const uint64_t visibles = rd::g_visibles.load(std::memory_order_relaxed);
  const uint64_t ocultos = rd::g_ocultos.load(std::memory_order_relaxed);
  const uint64_t sin_medida = rd::g_sin_medida.load(std::memory_order_relaxed);
  if (e.desde_ms == 0) {
    e.desde_ms = ahora;
    e.copias_informe = copias;
    e.lecturas_informe = lecturas;
    e.visibles_informe = visibles;
    e.ocultos_informe = ocultos;
    e.sin_medida_informe = sin_medida;
    return;
  }
  if (ahora - e.desde_ms < 10000) {
    return;
  }
  const uint64_t dc = copias - e.copias_informe;
  const uint64_t dl = lecturas - e.lecturas_informe;
  // If copies to that address keep appearing while skipping, it is not the reflection: stop skipping.
  if (e.fase == FaseReflejo::kAplicando && e.dibujados + 10 + e.dibujados / 10 < dc) {
    e.fase = FaseReflejo::kApagado;
    REXLOG_ERROR("[recortes] reflejo bajo demanda: DIFERENCIA, {} copias a {:08X} con solo {} fotogramas de reflejo "
                 "dibujados; se apaga y el reflejo vuelve a dibujarse siempre",
                 dc, nfsmw::reflejo_demanda::kDireccion, e.dibujados);
  }
  NFSMW_INFORME_DIFERIDO("[recortes] reflejo bajo demanda (build 191), ultimos {:.1f} s: pedido en {} fotogramas, "
                         "dibujado en {} y saltado en {} ({:.0f} %); {} copias y {} lecturas de {:08X}; fase {}",
                         double(ahora - e.desde_ms) / 1000.0, e.pedidos, e.dibujados, e.pedidos - e.dibujados,
                         100.0 * double(e.pedidos - e.dibujados) / double(std::max<uint32_t>(e.pedidos, 1)), dc, dl,
                         nfsmw::reflejo_demanda::kDireccion, NombreFase(e.fase));
  // What the queries of the draws that sample the reflection reported in this interval.
  if (REXCVAR_GET(nfsmw_reflejo_visibilidad)) {
    const int fase = rd::g_fase_visibilidad.load(std::memory_order_relaxed);
    NFSMW_INFORME_DIFERIDO("[recortes] reflejo por visibilidad (build 192): {} dibujos del agua medidos, {} con muestras "
                           "en pantalla y {} tapados del todo; {} sin poder medir (se dan por visibles); testigos {} "
                           "bien y {} mal; decide por {}",
                           (visibles - e.visibles_informe) + (ocultos - e.ocultos_informe),
                           visibles - e.visibles_informe, ocultos - e.ocultos_informe,
                           sin_medida - e.sin_medida_informe,
                           rd::g_testigos_bien.load(std::memory_order_relaxed),
                           rd::g_testigos_mal.load(std::memory_order_relaxed),
                           fase == rd::kVisAplicando ? "VISIBILIDAD"
                           : fase == rd::kVisMirando ? "lecturas (comprobando los testigos)"
                                                     : "lecturas (guardia disparada)");
  }
  e.desde_ms = ahora;
  e.pedidos = 0;
  e.dibujados = 0;
  e.copias_informe = copias;
  e.lecturas_informe = lecturas;
  e.visibles_informe = visibles;
  e.ocultos_informe = ocultos;
  e.sin_medida_informe = sin_medida;
}

/*
 * nfsmw_reflejo_detalle_minimo: the same threshold as the cubemap faces (nfsmw_cubemap_detalle_minimo) but
 * on the reflection views. At the Heritage & Omega start the reflection is ~436 draws (PC dump) and the
 * game asks it for small objects just as for the scene, although on the water they come out scaled by
 * 0.2-0.6 and distorted by the waves. The game's value is learned the first time (plausible: 1-64) and
 * only its value or ours is overwritten; if the view's ID (+4) does not say 4 or 5, it is left alone.
 */
int32_t g_reflejo_detalle_juego[2] = {-1, -1};
int32_t g_reflejo_detalle_aplicado[2] = {0, 0};
std::atomic<bool> g_aviso_reflejo_detalle{false};

void AjustarDetalleReflejo(uint8_t* base) {
  const int32_t pedido = REXCVAR_GET(nfsmw_reflejo_detalle_minimo);
  for (uint32_t i = 0; i < 2; ++i) {
    const uint32_t numero = i == 0 ? kVistaReflejo : kVistaReflejoSegundo;
    const uint32_t vista = DireccionVista(numero);
    if (Leer32(base, vista + 4) != numero) {
      continue;  // not the view we think it is
    }
    const int32_t actual = int32_t(Leer32(base, vista + kOffDetalleMinimo));
    if (g_reflejo_detalle_juego[i] < 0) {
      if (actual <= 0 || actual > 64) {
        continue;  // not initialized: try another frame
      }
      g_reflejo_detalle_juego[i] = actual;
    }
    const int32_t deseado = pedido > 0 ? std::max(pedido, g_reflejo_detalle_juego[i]) : g_reflejo_detalle_juego[i];
    if (actual == deseado || (actual != g_reflejo_detalle_juego[i] && actual != g_reflejo_detalle_aplicado[i])) {
      continue;  // already set, or someone else is in charge
    }
    Escribir32(base, vista + kOffDetalleMinimo, uint32_t(deseado));
    g_reflejo_detalle_aplicado[i] = deseado;
    if (!g_aviso_reflejo_detalle.exchange(true)) {
      REXLOG_INFO("[recortes] reflejo: los objetos de menos de {} pixeles dejan de dibujarse en el reflejo de la "
                  "carretera (vista {}: el juego usa {})",
                  deseado, numero, g_reflejo_detalle_juego[i]);
    }
  }
}

// After the game enables the frame's views. Keeps or removes views 4 and 5 (the reflection).
void DecidirReflejo(uint8_t* base) {
  namespace rd = nfsmw::reflejo_demanda;
  EstadoReflejo& e = g_reflejo;
  using namespace std::chrono;
  InformeReflejo(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
  const uint32_t a4 = DireccionActiva(kVistaReflejo);
  const uint32_t a5 = DireccionActiva(kVistaReflejoSegundo);
  if (base[a4] == 0 && base[a5] == 0) {
    return;  // the game does not request it this frame
  }
  ++e.pedidos;
  if (e.fase == FaseReflejo::kMirando) {
    if (e.mirando == 0) {
      e.copias_inicio = rd::g_copias.load(std::memory_order_relaxed);
    }
    ++e.dibujados;
    if (++e.mirando < kReflejoMirando) {
      return;
    }
    // The ring runs one frame behind: it is enough that 80 % of the requested frames were resolved there.
    const uint64_t copias = rd::g_copias.load(std::memory_order_relaxed) - e.copias_inicio;
    if (copias * 10 >= uint64_t(kReflejoMirando) * 8) {
      e.fase = FaseReflejo::kAplicando;
      REXLOG_INFO("[recortes] reflejo bajo demanda: comprobado ({} copias a {:08X} en {} fotogramas con el reflejo "
                  "pedido); desde aqui solo se dibuja si se ha leido en los ultimos {} fotogramas, y si no, 1 de cada {}",
                  copias, rd::kDireccion, kReflejoMirando, kReflejoGracia,
                  std::max<int32_t>(1, REXCVAR_GET(nfsmw_reflejo_refresco)));
    } else {
      e.fase = FaseReflejo::kApagado;
      REXLOG_WARN("[recortes] reflejo bajo demanda: APAGADO; en {} fotogramas con el reflejo pedido solo hubo {} copias "
                  "a {:08X} (el reflejo no se resuelve ahi): se dibuja siempre",
                  kReflejoMirando, copias, rd::kDireccion);
    }
    return;
  }
  if (e.fase == FaseReflejo::kApagado) {
    ++e.dibujados;
    return;
  }
  const uint64_t swaps = rd::g_swaps.load(std::memory_order_relaxed);
  // Once the witnesses are verified, what counts is whether the water left samples on screen.
  const bool por_visibilidad = REXCVAR_GET(nfsmw_reflejo_visibilidad) &&
                               rd::g_fase_visibilidad.load(std::memory_order_relaxed) == rd::kVisAplicando;
  const uint64_t ultima = por_visibilidad ? rd::g_ultima_visible.load(std::memory_order_relaxed)
                                          : rd::g_ultima_lectura.load(std::memory_order_relaxed);
  const bool leido = ultima != 0 && swaps + 1 - ultima <= kReflejoGracia;
  const uint32_t refresco = uint32_t(std::max<int32_t>(1, REXCVAR_GET(nfsmw_reflejo_refresco)));
  if (leido || ++e.desde_dibujo >= refresco) {
    e.desde_dibujo = 0;
    ++e.dibujados;
    return;
  }
  base[a4] = 0;
  base[a5] = 0;
}

}  // namespace
}  // namespace nfsmw::recortes_carrera

REX_EXTERN(__imp__sub_8243FF30);
REX_HOOK_RAW(sub_8243FF30) {
  __imp__sub_8243FF30(ctx, base);
  using namespace nfsmw::recortes_carrera;
  ContarRetrovisor(base);
  if (!EnCarrera(base)) {
    return;
  }
  if (REXCVAR_GET(nfsmw_reflejo_carretera)) {
    AjustarDetalleReflejo(base);
    if (REXCVAR_GET(nfsmw_reflejo_bajo_demanda)) {
      DecidirReflejo(base);
    }
    return;
  }
  base[DireccionActiva(kVistaReflejo)] = 0;
  base[DireccionActiva(kVistaReflejoSegundo)] = 0;
  if (!g_aviso_reflejo.exchange(true)) {
    REXLOG_INFO("[recortes] carrera: reflejo de la carretera apagado (vistas 4 y 5)");
  }
}

/*
 * See nfsmw_retrovisor_recorte. The function writes the mask to r4+132 right before returning; here
 * the two bits are set again afterwards, which is the same as never having removed them.
 */
REX_EXTERN(__imp__sub_82216600);
REX_HOOK_RAW(sub_82216600) {
  const uint32_t puntero_vista = ctx.r3.u32;
  const uint32_t destino = ctx.r4.u32;
  __imp__sub_82216600(ctx, base);
  if (!REXCVAR_GET(nfsmw_retrovisor_recorte) || destino == 0) {
    return;
  }
  using namespace nfsmw::recortes_carrera;
  if (Leer32(base, puntero_vista + 4) != 20) {  // +4 = view ID
    return;
  }
  const uint32_t antes = Leer32(base, destino + 132);
  const uint32_t despues = antes | 0x40u | 0x08000000u;
  if (antes != despues) {
    Escribir32(base, destino + 132, despues);
    if (!g_aviso_retrovisor.exchange(true)) {
      REXLOG_INFO("[recortes] retrovisor: se le devuelven los dos bits de recorte (mascara 0x{:08X} -> 0x{:08X})",
                  antes, despues);
    }
  }
}

REX_EXTERN(__imp__sub_8243C6E0);
REX_HOOK_RAW(sub_8243C6E0) {
  const uint32_t tabla = ctx.r3.u32;
  __imp__sub_8243C6E0(ctx, base);
  using namespace nfsmw::recortes_carrera;
  RegistrarTablaCaras(base);
  if (!EnCarrera(base)) {
    // Outside a race the first-draw state is forgotten, so the next race draws all six faces once again.
    // The cubemap contents belong to the scenery and are no good for another one.
    g_caras_estrenadas = 0;
    g_aviso_estreno.store(false, std::memory_order_relaxed);
    g_aviso_estreno_falta.store(false, std::memory_order_relaxed);
    return;
  }
  if (!g_aviso_vistas_caras.exchange(true)) {
    REXLOG_INFO("[recortes] caras del cubemap (tabla 0x{:08X}): vistas {} {} {} {} {} {}", tabla,
                NumeroDeVista(Leer32(base, tabla)), NumeroDeVista(Leer32(base, tabla + 4)),
                NumeroDeVista(Leer32(base, tabla + 8)), NumeroDeVista(Leer32(base, tabla + 12)),
                NumeroDeVista(Leer32(base, tabla + 16)), NumeroDeVista(Leer32(base, tabla + 20)));
  }
  // Which face is which. Separate from the notice above and with its own flag, because trying on the
  // first call (when the cubemap views did not have their parameters yet) gave all six with direction
  // 0,0,0. It now retries until it matches.
  if (!g_aviso_caras_nombradas.load(std::memory_order_relaxed)) {
    DiagnosticoCarasDelCubo(base, tabla);
  }
  const uint32_t siempre = uint32_t(REXCVAR_GET(nfsmw_cubemap_caras_siempre));
  /*
   * The object size threshold goes up here on purpose: it also applies with nfsmw_cubemap_caras_max = -1
   * (the PC value), which is where the effect can be measured.
   */
  for (uint32_t i = 0; i < kCaras; ++i) {
    const uint32_t vista = Leer32(base, tabla + 4 * i);
    if (EsVista(vista)) {
      AjustarDetalleCubo(base, tabla, i, vista, siempre);
    }
  }
  const int32_t ciclo_s = REXCVAR_GET(nfsmw_cubemap_diag_ciclo_s);
  if (ciclo_s > 0) {
    DiagnosticoUnaCara(base, tabla, ciclo_s);
    return;
  }
  const int32_t maximo = REXCVAR_GET(nfsmw_cubemap_caras_max);
  if (maximo < 0) {
    return;
  }
  uint32_t activas[kCaras];
  uint32_t n = 0;
  uint32_t fijas = 0;
  for (uint32_t i = 0; i < kCaras; ++i) {
    const uint32_t vista = Leer32(base, tabla + 4 * i);
    if (vista == 0) {
      continue;
    }
    if (!EsVista(vista)) {
      if (!g_aviso_puntero.exchange(true)) {
        REXLOG_WARN("[recortes] cara {} apunta a 0x{:08X}, fuera de la tabla de vistas; no se toca",
                    i, vista);
      }
      continue;
    }
    if (base[vista + kOffActiva] == 0) {
      continue;
    }
    // The first draw is marked here, on the face about to be drawn, pinned faces included. The first
    // version marked it by walking activas[], and pinned faces are not in activas[] (they leave through
    // the continue below), so on the Switch (caras_siempre = 4, the mirror) bit 2 was never set, the
    // first-draw phase never ended and the limit was never applied: all six faces were drawn every frame
    // and the cubemap went from 6.0 to 10.0 ms real. It did not show on the PC because there caras_siempre
    // is 0 and no face is pinned.
    g_caras_estrenadas |= (1u << i);
    if (siempre & (1u << i)) {
      ++fijas;  // updated every frame
      continue;
    }
    activas[n++] = vista;
  }
  /*
   * The first full round is never trimmed.
   *
   * With nfsmw_cubemap_caras_max = 0 the five non-pinned faces were disabled in every frame, so they were
   * never drawn even once from the start of the race. The cubemap kept whatever it held before (nothing)
   * and the car body, which samples it as an environment map, reflected black: the cars looked dark.
   *
   * A delayed reflection is a defensible performance trade-off. A black reflection is a bug. So whatever
   * the limit, every face is drawn at least once; until then nothing is touched. That is six race frames,
   * paid once.
   */
  if (g_caras_estrenadas != kTodasLasCaras) {
    if (!g_aviso_estreno_falta.exchange(true)) {
      REXLOG_INFO("[recortes] carrera: estrenando el cubemap, faltan caras (mascara 0x{:X} de 0x{:X}); "
                  "hasta entonces no se recorta", g_caras_estrenadas, kTodasLasCaras);
    }
    return;  // no face is turned off until all of them have been drawn once
  }
  if (!g_aviso_estreno.exchange(true)) {
    REXLOG_INFO("[recortes] carrera: las seis caras del cubemap ya se han dibujado una vez; "
                "a partir de aqui manda el limite");
  }
  if (n <= uint32_t(maximo)) {
    return;
  }
  const uint32_t inicio = g_rotacion.fetch_add(1, std::memory_order_relaxed) % n;
  for (uint32_t k = 0; k < n; ++k) {
    if ((k + n - inicio) % n >= uint32_t(maximo)) {
      base[activas[k] + kOffActiva] = 0;
    }
  }
  if (!g_aviso_caras.exchange(true)) {
    REXLOG_INFO("[recortes] carrera: cubemap limitado a {} cara(s) por fotograma mas {} fija(s) "
                "(mascara 0x{:X}; el juego activaba {})",
                maximo, fijas, siempre, n + fijas);
  }
}
