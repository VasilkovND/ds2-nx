// nfsmw - who really draws the race shadow map
//
// ===========================================================================
//  The complete chain, read from the binary. Three levers failed because they all pointed at the same
//  place, and that place is the smallest of the three that draw.
//
//  sub_82443B18 is the shadow pass. It receives the one/two maps boolean in r4 and loops once per map
//  (r18 = 1 or 2) over views 13 and 14 (r26 starts at 0x82A38070 + 1456 = view 13 and advances 112
//  bytes). Within each iteration it draws with four different emitters, in this exact order
//  (nfsmw_recomp.55.cpp:16149, asm 299-438):
//
//    1. sub_824FA010(vista, 0x200000)   RenderWorldModels  -> WorldModels with bones
//    2. sub_824FA010(vista, 0x202000)   RenderWorldModels  -> WorldModels without bones
//    3. sub_824C3610(cull, vista, 512)  StuffScenery       -> scenery marked as caster
//    4. a loop over a global list (0x82C84D38: pointer at +4, count at +12) that calls virtual
//       method +12 of each element with (vista, 0). These are the entities: the cars.
//
//  All four end in the same function, sub_8243E358 = eViewPlatInterface::Render(eModel*, matrix,
//  light, flags, blend) (EcstasyData.hpp:232). It is the bottleneck all geometry goes through, which is
//  why it is measured there.
//
// ---------------------------------------------------------------------------------------------
//  Why none of the three moved a single triangle
//
//  All three only touched emitter 3 (the scenery), and only the part of it that was already filtered.
//
//  a) nfsmw_sombras_lod (bit 0x1000 in cull_info+132) and nfsmw_sombras_lod_h (H in cull_info+192)
//     write into the SceneryCullInfo. The only reader is DrawAScenery, which only decides which
//     scenery enters the scenery draw list. Neither RenderWorldModels nor the car loop look at that
//     record: it does not exist for them.
//
//  b) On top of that, StuffScenery is called for shadows with stuff_flags = 512 (0x200), which
//     according to Scenery.cpp:1065-1095 sets exclusive_flags = 0x10000. With that, of the whole
//     scenery list the culling has just built, only what carries that bit is drawn, and only these
//     set it (Scenery.cpp:1131-1147):
//         (SceneryInst->ExcludeFlags & 0x200000) && !(ExcludeFlags & 0x4000000)   -> normal caster
//         (SceneryInst->ExcludeFlags & 0x40000000)                                -> forced caster
//     So the scenery that reaches the shadow map is a subset hand-marked by the artists. Lowering the
//     LOD of the whole list does not touch the few that pass the filter.
//
//  c) nfsmw_sombras_corte (PixelMinSize, vista+36) does target the right emitter (WorldModel::Render
//     uses it, WorldModel.cpp:313), but the value falls six times short. The cutoff is
//         distancia = 35 * (1 + H / PixelMinSize)          (eView.cpp:44-64, fixed radius 35)
//     and with the numbers measured on the console (H of view 13 = 9,146.3; H of the scene = 790.6;
//     PixelMinSize = ancho_destino * 0.009375, i.e. 15 in the map and 12 on screen):
//         scene     (H 790.6  P 12)  -> culls at   2,341 units
//         map       (H 9146.3 P 15)  -> culls at  21,376 units     9.1 times farther
//         map with nfsmw_sombras_corte = 150 (P 22) -> culls at 14,583 units
//     All of Rockport fits within 14,583 units: that is why that cvar comes out inert. The lever
//     exists; the number is just absurd for the H of a shadow view.
//
//  d) nfsmw_sombras_mapas (1 map instead of 2) is something else and lives in nfsmw_recorte_sombras.cpp.
//
// ---------------------------------------------------------------------------------------------
//  C7 read: the shadow map has no fat to trim by LOD. Closed.
//
//  Measured breakdown of view 13 (view 14 always shows 0 draws: nfsmw_sombras_mapas leaves a single
//  map). Per-frame averages over 17 race reports (and 15 more from a build that already had C7 but
//  not this lever, so there is a clean A/B):
//
//      emitter                  draws   k triangles   % of triangles
//      world with bones           0.0          0.00         0.0 %
//      static world               5.0          0.21         0.6 %   <- what the lever targeted
//      scenery                  186.4         12.52        35.6 %
//      cars and the rest         77.7         22.41        63.8 %
//      TOTAL                    269.1         35.15
//
//  Fourth failed lever, and for the same reason as the previous three: the wrong emitter. This time
//  the mechanism was right (WorldModel::Render does read PixelMinSize, and the log confirms the write
//  22 -> 72), but that whole emitter is worth 0.6 % of the map. The A/B:
//      without -> with:  WorldModel drawn 3.73 -> 5.00 | mundo-resto 0.16 -> 0.21 k triangles
//                        whole map (C7) 31.89 -> 35.15 k | shadows (C6) 63.3 -> 67.5 k
//  Everything goes up instead of down; they are two different laps, so the delta is noise, but the
//  conclusion does not depend on the noise: there are not even 0.2 k triangles to save there.
//
//  And the top ten solids by name say the other two emitters have nothing either:
//    - The player's car alone is 45 % of the top ten: "RX8_KIT00_BODY_A" 4,622 tris in ONE draw and
//      "RX8_BASE_A" 1,354, i.e. 5.98 k = 17 % of the whole map in TWO draws. The "_A" is the LOD:
//      the hero enters the shadow map with the highest-detail mesh. And it cannot be lowered:
//      CarRenderInfo::Render does car_body_lod = bMin(mMaxLodLevel, bodyLodIx + mMinLodLevel) and
//      for CarRenderUsage_Player (RideInfo, CarInfo.cpp:814-818) mMin == mMax, so neither the pixel
//      size nor ForceCarLOD (which also goes through bClamp(min,max)) moves it. Getting "_A" rather
//      than "_B" with mMin==mMax==B in the PS2 decomp is precisely the proof that the 360 binary
//      pins it even higher.
//    - The other cars already come in the low mesh: without the hero there are ~76 draws with
//      16.4 k, i.e. 215 triangles per draw. With the H of view 13 (9,146) and the light ~2,000 units
//      away, car_pixel_size comes out ~11 and the CarBodyLodSwapSize ladder {120,25,20,10,0} already
//      puts them on the last step. There is no lower step.
//    - The scenery is "XT_" (vegetation: TREELINE, REDWOOD, POPLAR, JUNIPER, HEDGE, BUSH_FOREST) and
//      "XB_" (buildings: HAPARTMENT, CP_APARTMENT, COLONIALHSE, BEACHHOTEL), almost all "_DEINST",
//      which are already merged batches. Their 186 draws average 67 triangles: hundreds of tiny
//      instances (XT_REDWOODL 26 tris/draw over 54 draws, XO_OVERPASS 16, XO_CRASHBARREL 48). The
//      big ones left are the merged batches, and those are the subset the artists hand-marked as
//      casters: exactly what was already found inert twice.
//
//  In short: of the map's 35 k triangles, 6 k are a car pinned by design, 16 k are cars already at
//  their worst mesh and 12.5 k are hundreds of 67-triangle instances. The ceiling of everything that
//  could be trimmed by LOD is ~6 k guest triangles (~11.5 k real, given the 1.92 factor between C7
//  and C6) = 0.64 ms, and getting it would mean ruining the player car's shadow. Not touched. Any
//  reduction of the pass's 3.5 ms has to come from something other than geometry: the 5.12 Mtexels
//  it opens or the vegetation's alpha material.
//
//  ---------------------------------------------------------------------------------------------
//  How the breakdown is measured
//
//  The right lever differs per emitter:
//      world      -> PixelMinSize of view 13/14 (what nfsmw_sombras_mundo_corte does)
//      scenery    -> cull_info (already tried, moves nothing)
//      cars       -> not possible with PixelMinSize: the game exempts views 13 and 14 on purpose.
//                    CarRender.cpp:2814 says literally
//                        if (car_pixel_size < view->GetPixelMinSize())
//                            if ((unsigned)(view->ID - 13) > 1) return false;
//                    so in views 13 and 14 the car is always drawn, however small. For cars the
//                    lever is the LOD passed to CarRenderInfo::Render (tireLOD and carLOD
//                    parameters), not the size.
//
//  The breakdown is measured in sub_8243E358 because all four pass through it, and it is attributed
//  with a thread mark set by the RenderWorldModels and StuffScenery hooks. The triangles come from the
//  model itself: eModel+0x0C = eSolid, eSolid+0x14 = NumPolys (int16), eSolid+0xA0 = name
//  (Ecstasy.hpp:15 and 95). Only fields the game dereferences in that same call are read, and after
//  it has done so, so no memory the game does not touch can be touched.
//
//  What the log says (the "C7" line), every 10 s and per frame:
//    - per view (13, 14 and view 1 as reference) and per emitter: draws and k triangles
//    - how many WorldModels were examined and how many were drawn (the rest were culled by the game
//      or by this lever): this shows at a glance whether nfsmw_sombras_mundo_corte bites
//    - how many times CullView runs with a cull_info of views 13/14 and how many SceneryDrawInfo it
//      leaves in its list. If it is 0, the scenery is not even culled for the shadow map; if it is
//      large but the "escenario" emitter draws little, the caster filter of StuffScenery is what
//      trims, not the LOD.
//    - the 10 solids that put the most triangles into the map, by name. That tells whether it is a
//      car, a building or vegetation, with no interpretation.
//
// ---------------------------------------------------------------------------------------------
//  The lever that was tried: nfsmw_sombras_mundo_corte -> off (0), measured to have no effect
//
//  It is the PixelMinSize of views 13 and 14, but expressed in world units, which is the only thing
//  that can be reasoned about. It is computed by inverting the formula above:
//
//      PixelMinSize = 35 * H / (corte - 35)          H read from the view in that same frame
//
//  It is written right before RenderWorldModels and the original value is restored right after, so
//  it does not overwrite nfsmw_sombras_corte (which writes the same field from the pass hook) and it
//  does not touch any other view or pass. And it only tightens: if the computed value is lower than
//  the current one, nothing is written.
//
//  Why it was set to 4500 and why it is now 0.
//  The safety reasoning was correct (and so was the write: the log confirms it), but the emitter it
//  targets contributes 0.6 % of the map's triangles. Culling something that costs nothing gains
//  nothing, and background shadows are lost in exchange. It stays at 0.
//
//  The scene stops drawing an object beyond 2,341 units from the scene camera. The light camera is
//  2,000 units away from it (measured on the console: the scene looks from (539, 4549, 230) and the
//  map from (1029, 3694, 1971)). By the triangle inequality, an object the scene draws is at most
//  2,341 + 2,000 = 4,341 units from the light. With the cutoff at 4,500 no object visible on screen
//  loses its shadow: only shadows of things the scene no longer draws are lost. With 15 the map
//  reached 21,376 units, 4.8 times farther than needed.
// ===========================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>

REXCVAR_DEFINE_INT32(nfsmw_sombras_mundo_corte, 0, "NFSMW",
                     "PROBADA INERTE (compilacion 117, con la C7 delante). La palanca SI escribe -el log dice "
                     "'PixelMinSize 22 -> 72, los WorldModel dejan de proyectar sombra a 4481 unidades en vez "
                     "de a 14586'- pero el emisor al que apunta pone 0,21 k triangulos de los 35,1 k del mapa, "
                     "o sea el 0,6 %. A/B de la vista 13 (116 sin ella / 117 con ella): WorldModel dibujados "
                     "3,73 -> 5,00 y mundo-resto 0,16 -> 0,21 k triangulos; el mapa entero 31,9 -> 35,1 k y la "
                     "C6 63,3 -> 67,5 k. No baja: sube, dentro del ruido de dos vueltas distintas. Aunque "
                     "cortara el 100 % del emisor ahorraria 0,2 k triangulos = 0,01 ms. Se deja en 0 para no "
                     "perder sombras de fondo a cambio de nada. Distancia en unidades del mundo desde la camara "
                     "de la luz; 0 = dejar el valor del juego. No toca coches ni escenario")
    .range(0, 60000);

REXCVAR_DEFINE_BOOL(nfsmw_sombras_reparto, true, "NFSMW",
                    "Anotar cada pocos segundos el reparto REAL del pase de sombras: cuantos dibujos y "
                    "cuantos triangulos pone cada emisor (mundo con huesos, mundo estatico, escenario y "
                    "coches) en las vistas 13 y 14, y los diez solidos que mas triangulos meten, por "
                    "nombre. Es la medida que dice cual de las palancas puede servir. Apagado no cuesta "
                    "nada: el gancho caliente lee un booleano y llama al original");

REXCVAR_DEFINE_INT32(nfsmw_sombras_reparto_cada_s, 10, "NFSMW",
                     "Cada cuantos segundos se anota el reparto del pase de sombras")
    .range(2, 120);

REXCVAR_DEFINE_INT32(nfsmw_sombras_lod_h, 0, "NFSMW",
                     "PROBADA INERTE (compilacion 114): escribe la H en el SceneryCullInfo, que solo lee "
                     "DrawAScenery, o sea solo el escenario, y encima el que llega al mapa de sombras ya "
                     "esta filtrado por el bit de caster. El log confirmo la escritura (9.146 -> 2.372) y "
                     "los triangulos pasaron de 64,7 k a 65,5 k. Se deja en 0 para no perder sombras a "
                     "cambio de nada. En porcentaje de la H de la escena; 0 = no tocar")
    .range(0, 1200);

REXCVAR_DEFINE_BOOL(nfsmw_sombras_lod, false, "NFSMW",
                    "PROBADA INERTE (compilacion 113): manda el escenario del mapa de sombras a la rama del "
                    "cubo con el bit 0x1000 del SceneryCullInfo. El log confirmo la mascara "
                    "(0x00004114 -> 0x00005114) y los triangulos pasaron de 59,1 k a 60,0 k, porque en esa "
                    "rama el escenario marcado 0x1000100 se queda con la malla buena igual");

REXCVAR_DEFINE_BOOL(nfsmw_sombras_lod_diag, false, "NFSMW",
                    "Anota una vez por sesion las mascaras de todos los registros de vista, para comprobar "
                    "que las vistas 13 y 14 reciben el bit y ninguna otra");

namespace nfsmw::sombras_lod {
namespace {

// --- SceneryCullInfo (nfsmwdecomp, Scenery.hpp:123), as laid out on the 360 -------------------
constexpr uint32_t kRegistroBytes = 208;      // sub_82440890: registro[i] = objeto + i*208
constexpr uint32_t kOffContador = 2496;       // objeto+2496 = NumCullInfos (208 * 12)
constexpr uint32_t kOffVista = 128;           // +128 = pView
constexpr uint32_t kOffMascara = 132;         // +132 = ExcludeFlags
constexpr uint32_t kOffPrimerDibujo = 136;    // +136 = pFirstDrawInfo
constexpr uint32_t kOffUltimoDibujo = 140;    // +140 = pCurrentDrawInfo
constexpr uint32_t kOffPosicion = 160;        // +160 = Position (bVector3)
constexpr uint32_t kOffHRegistro = 192;       // +192 = H
constexpr uint32_t kDrawInfoBytes = 12;       // sizeof(SceneryDrawInfo) on the 360
constexpr uint32_t kBitMallaReducida = 0x1000u;
constexpr uint32_t kRegistrosMax = 64;        // sanity: the game builds at most 12

// --- eView (nfsmwdecomp, Ecstasy.hpp:152) -----------------------------------------------------
constexpr uint32_t kBaseVistas = 0x82A38070;
constexpr uint32_t kBytesPorVista = 112;
constexpr uint32_t kVistas = 24;
constexpr uint32_t kOffIdVista = 4;           // eView+0x04 = ID
constexpr uint32_t kOffHVista = 12;           // eView+0x0C = H
constexpr uint32_t kOffPixelMinSize = 36;     // eView+0x24 = PixelMinSize
constexpr uint32_t kVistaEscena = 1;
constexpr uint32_t kVistaSombras1 = 13;
constexpr uint32_t kVistaSombras2 = 14;

// --- eModel / eSolid (nfsmwdecomp, Ecstasy.hpp:15 y 95) ---------------------------------------
constexpr uint32_t kOffSolidEnModelo = 12;    // eModel+0x0C = eSolid*
constexpr uint32_t kOffNumPolys = 0x14;       // eSolid+0x14 = NumPolys (int16)
constexpr uint32_t kOffNombreSolido = 0xA0;   // eSolid+0xA0 = Name[64]
constexpr int32_t kNumPolysMax = 32767;
// On Win32 and Mac ARM64, addresses from here up carry the +0x1000 of REX_PHYS_HOST_OFFSET, and
// this reads with a bare memcpy: nothing counted here lives there, so it is rejected instead of
// reading a shifted address.
constexpr uint32_t kDireccionMaxima = 0xE0000000u;
constexpr uint32_t kPagina = 0x1000u;

// --- Render constants -------------------------------------------------------------------------
// WorldModel.cpp:253 WorldObjectMaximumRadius; the radius every WorldModel is measured with.
constexpr double kRadioMundo = 35.0;
// exc_flag that sub_82443B18 passes to RenderWorldModels (WorldModel.cpp:281-292).
constexpr uint32_t kFlagSombra = 0x200000u;   // "only those that cast shadows"
constexpr uint32_t kFlagSinHuesos = 0x2000u;  // with the bit: static ones; without it: animated ones
// The three numbers of ScenerySectionHeader::DrawAScenery, in pixels and absolute.
constexpr double kUmbralDibujar = 17.0;
constexpr double kUmbralMallaBuena = 52.2;    // 8.7 x the Density minimum (6.0)
constexpr double kRadioDeReferencia = 35.0;
// Range in which an H makes sense: below it the map would empty, above it it is not a scale.
constexpr double kHMinima = 1.0;
constexpr double kHMaxima = 1.0e6;
constexpr uint32_t kPixelMinSizeMax = 4096;   // the same sanity cap nfsmw_sombras_corte uses

// --- Emitters ---------------------------------------------------------------------------------
// In the shadow pass RenderWorldModels is called twice and the split is exact: 0x200000 carries the
// WorldModels with bones and 0x202000 those without. In the other passes it is called once and all
// of the world lands in "mundo-resto", which there simply means "mundo".
enum Emisor : uint8_t {
  kEmisorMundoAnimado = 0,   // RenderWorldModels(vista, 0x200000): WorldModel con huesos
  kEmisorMundoResto = 1,     // RenderWorldModels in any other case
  kEmisorEscenario = 2,      // StuffScenery
  kEmisorOtros = 3,          // the entity loop (cars) and everything else
  kEmisores = 4,
};
const char* const kNombreEmisor[kEmisores] = {"mundo-conhuesos", "mundo-resto", "escenario",
                                              "otros (coches y demas)"};

struct Celda {
  std::atomic<uint32_t> dibujos{0};
  std::atomic<uint64_t> triangulos{0};
};
Celda g_celda[kVistas][kEmisores];
std::atomic<uint32_t> g_mundo_mirados[kVistas];   // calls to WorldModel::Render per view
std::atomic<uint32_t> g_cull_pasadas[kVistas];    // times CullView receives a cull_info of that view
std::atomic<uint32_t> g_cull_objetos[kVistas];    // SceneryDrawInfo entries culling leaves in its list
std::atomic<uint32_t> g_fotogramas{0};

// Thread mark: who gets credited with the draw going through eView::Render right now.
thread_local uint8_t t_emisor = kEmisorOtros;

// The hot hook does not read the cvar: it reads this, refreshed once per frame.
std::atomic<bool> g_reparto_activo{false};

// --- Solids table, only for views 13 and 14 (110 draws per frame) -----------------------------
constexpr uint32_t kSolidos = 128;
constexpr uint32_t kSondeos = 4;
constexpr uint32_t kNombreLargo = 28;
struct Solido {
  std::atomic<uint32_t> clave{0};   // puntero al eSolid; 0 = libre
  std::atomic<uint32_t> dibujos{0};
  std::atomic<uint64_t> triangulos{0};
  char nombre[kNombreLargo + 1] = {0};
};
Solido g_solidos[kSolidos];
std::atomic<uint32_t> g_solidos_perdidos{0};

std::atomic<bool> g_anotado{false};
std::atomic<bool> g_anotado_diag{false};
std::atomic<bool> g_anotado_camaras{false};
std::atomic<int32_t> g_pct_anotado{-1};
std::atomic<int32_t> g_corte_anotado{-1};
std::atomic<int64_t> g_informe_ms{0};

// Position of the current frame's scene camera (report only).
std::atomic<bool> g_escena_vista{false};
float g_escena_pos[3] = {0.0f, 0.0f, 0.0f};

uint32_t Leer32(const uint8_t* base, uint32_t direccion) {
  uint32_t v = 0;
  std::memcpy(&v, base + direccion, sizeof(v));
  return __builtin_bswap32(v);
}

int32_t Leer16Con(const uint8_t* base, uint32_t direccion) {
  uint16_t v = 0;
  std::memcpy(&v, base + direccion, sizeof(v));
  return int16_t(__builtin_bswap16(v));
}

void Escribir32(uint8_t* base, uint32_t direccion, uint32_t valor) {
  const uint32_t v = __builtin_bswap32(valor);
  std::memcpy(base + direccion, &v, sizeof(v));
}

float ComoFloat(uint32_t v) {
  float f = 0.0f;
  std::memcpy(&f, &v, sizeof(f));
  return f;
}

uint32_t ComoU32(float f) {
  uint32_t v = 0;
  std::memcpy(&v, &f, sizeof(v));
  return v;
}

// Guest floats are big-endian: they must be byte-swapped, not read raw.
float LeerFlotante(const uint8_t* base, uint32_t direccion) { return ComoFloat(Leer32(base, direccion)); }

void EscribirFlotante(uint8_t* base, uint32_t direccion, float valor) {
  Escribir32(base, direccion, ComoU32(valor));
}

// Only a pointer to the start of a view in the table whose ID matches is accepted.
bool VistaValida(const uint8_t* base, uint32_t vista, uint32_t* id_salida) {
  if (vista < kBaseVistas) {
    return false;
  }
  const uint32_t desplazamiento = vista - kBaseVistas;
  if (desplazamiento % kBytesPorVista != 0) {
    return false;
  }
  const uint32_t indice = desplazamiento / kBytesPorVista;
  if (indice >= kVistas) {
    return false;
  }
  if (Leer32(base, vista + kOffIdVista) != indice) {
    return false;
  }
  *id_salida = indice;
  return true;
}

bool HRazonable(double h) { return std::isfinite(h) && h >= kHMinima && h <= kHMaxima; }

bool EsVistaDeSombras(uint32_t id) { return id == kVistaSombras1 || id == kVistaSombras2; }

// Distance at which an object of radius R stops covering `px` pixels: d = R * (1 + H / px).
double Distancia(double h, double px) { return kRadioDeReferencia * (1.0 + h / px); }

// PixelMinSize that makes an object of radius 35 stop being drawn beyond `corte` units.
// It is the exact inverse of eView::GetPixelSize (eView.cpp:44-64): px = radio * H / (d - radio).
double PixelMinSizeParaCorte(double h, double corte) {
  return (kRadioMundo * h) / (corte - kRadioMundo);
}

int64_t AhoraMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// --- Contabilidad ------------------------------------------------------------------------------

void ApuntarSolido(const uint8_t* base, uint32_t solido, uint32_t triangulos) {
  const uint32_t inicio = (solido >> 4) % kSolidos;
  for (uint32_t s = 0; s < kSondeos; ++s) {
    Solido& fila = g_solidos[(inicio + s) % kSolidos];
    uint32_t clave = fila.clave.load(std::memory_order_relaxed);
    if (clave == 0) {
      // First time it is seen: the name is stored. It is the only time it is read.
      uint32_t esperado = 0;
      if (!fila.clave.compare_exchange_strong(esperado, solido, std::memory_order_relaxed)) {
        clave = esperado;
      } else {
        // The name is only read if it lies in the same 4 KB page as the NumPolys the game has just read:
        // that way no page the game has not touched can be touched. eSolid is 0xE0 bytes, so this holds
        // in 95 % of cases and the rest stay unnamed.
        const uint32_t dentro = solido & (kPagina - 1);
        if (dentro + kOffNombreSolido + kNombreLargo <= kPagina) {
          std::memcpy(fila.nombre, base + solido + kOffNombreSolido, kNombreLargo);
          fila.nombre[kNombreLargo] = 0;
          for (uint32_t i = 0; i < kNombreLargo; ++i) {
            const unsigned char c = static_cast<unsigned char>(fila.nombre[i]);
            if (c != 0 && (c < 32 || c > 126)) {
              fila.nombre[i] = '?';  // not a name: make it visible in the log without breaking the format
            }
          }
        } else {
          std::memcpy(fila.nombre, "(cruza de pagina)", 18);
        }
        clave = solido;
      }
    }
    if (clave == solido) {
      fila.dibujos.fetch_add(1, std::memory_order_relaxed);
      fila.triangulos.fetch_add(triangulos, std::memory_order_relaxed);
      return;
    }
  }
  g_solidos_perdidos.fetch_add(1, std::memory_order_relaxed);
}

void ReiniciarCuentas() {
  for (uint32_t v = 0; v < kVistas; ++v) {
    for (uint32_t e = 0; e < kEmisores; ++e) {
      g_celda[v][e].dibujos.store(0, std::memory_order_relaxed);
      g_celda[v][e].triangulos.store(0, std::memory_order_relaxed);
    }
    g_mundo_mirados[v].store(0, std::memory_order_relaxed);
    g_cull_pasadas[v].store(0, std::memory_order_relaxed);
    g_cull_objetos[v].store(0, std::memory_order_relaxed);
  }
  for (uint32_t i = 0; i < kSolidos; ++i) {
    g_solidos[i].clave.store(0, std::memory_order_relaxed);
    g_solidos[i].dibujos.store(0, std::memory_order_relaxed);
    g_solidos[i].triangulos.store(0, std::memory_order_relaxed);
    g_solidos[i].nombre[0] = 0;
  }
  g_solidos_perdidos.store(0, std::memory_order_relaxed);
  g_fotogramas.store(0, std::memory_order_relaxed);
}

void AnotarVista(uint32_t vista, double fotogramas) {
  uint32_t dibujos_total = 0;
  uint64_t triangulos_total = 0;
  for (uint32_t e = 0; e < kEmisores; ++e) {
    dibujos_total += g_celda[vista][e].dibujos.load(std::memory_order_relaxed);
    triangulos_total += g_celda[vista][e].triangulos.load(std::memory_order_relaxed);
  }
  const uint32_t cull_pasadas = g_cull_pasadas[vista].load(std::memory_order_relaxed);
  if (dibujos_total == 0 && cull_pasadas == 0) {
    return;
  }
  const uint32_t mirados = g_mundo_mirados[vista].load(std::memory_order_relaxed);
  REXLOG_INFO(
      "[sombras lod] C7 vista {}: TOTAL {:.0f} dibujos {:.1f} k triangulos ({:.0f} tri/dibujo) = "
      "{} {:.0f}/{:.1f}k | {} {:.0f}/{:.1f}k | {} {:.0f}/{:.1f}k | {} {:.0f}/{:.1f}k; "
      "WorldModel mirados {:.0f}, dibujados {:.0f}; culling del escenario: {:.2f} pasadas con "
      "{:.0f} objetos en la lista",
      vista, double(dibujos_total) / fotogramas, double(triangulos_total) / fotogramas / 1000.0,
      dibujos_total != 0 ? double(triangulos_total) / double(dibujos_total) : 0.0,
      kNombreEmisor[0], double(g_celda[vista][0].dibujos.load(std::memory_order_relaxed)) / fotogramas,
      double(g_celda[vista][0].triangulos.load(std::memory_order_relaxed)) / fotogramas / 1000.0,
      kNombreEmisor[1], double(g_celda[vista][1].dibujos.load(std::memory_order_relaxed)) / fotogramas,
      double(g_celda[vista][1].triangulos.load(std::memory_order_relaxed)) / fotogramas / 1000.0,
      kNombreEmisor[2], double(g_celda[vista][2].dibujos.load(std::memory_order_relaxed)) / fotogramas,
      double(g_celda[vista][2].triangulos.load(std::memory_order_relaxed)) / fotogramas / 1000.0,
      kNombreEmisor[3], double(g_celda[vista][3].dibujos.load(std::memory_order_relaxed)) / fotogramas,
      double(g_celda[vista][3].triangulos.load(std::memory_order_relaxed)) / fotogramas / 1000.0,
      double(mirados) / fotogramas,
      double(g_celda[vista][0].dibujos.load(std::memory_order_relaxed) +
             g_celda[vista][1].dibujos.load(std::memory_order_relaxed)) /
          fotogramas,
      double(cull_pasadas) / fotogramas,
      double(g_cull_objetos[vista].load(std::memory_order_relaxed)) / fotogramas);
}

void EmitirInforme(const uint8_t* base, double segundos) {
  (void)base;
  const uint32_t fotogramas = g_fotogramas.load(std::memory_order_relaxed);
  if (fotogramas == 0) {
    ReiniciarCuentas();
    return;
  }
  const double f = double(fotogramas);
  REXLOG_INFO(
      "[sombras lod] C7 reparto del pase de sombras, ultimos {:.1f} s ({} fotogramas), medias POR "
      "FOTOGRAMA. Los triangulos son la suma de eSolid::NumPolys de cada eView::Render, o sea el "
      "recuento del invitado; compara con los \"k triangulos\" de la linea C6",
      segundos, fotogramas);
  AnotarVista(kVistaSombras1, f);
  AnotarVista(kVistaSombras2, f);
  AnotarVista(kVistaEscena, f);

  // The ten solids with the most triangles in the shadow views.
  uint32_t orden[kSolidos];
  uint32_t n = 0;
  for (uint32_t i = 0; i < kSolidos; ++i) {
    if (g_solidos[i].clave.load(std::memory_order_relaxed) != 0 &&
        g_solidos[i].triangulos.load(std::memory_order_relaxed) != 0) {
      orden[n++] = i;
    }
  }
  std::sort(orden, orden + n, [](uint32_t a, uint32_t b) {
    return g_solidos[a].triangulos.load(std::memory_order_relaxed) >
           g_solidos[b].triangulos.load(std::memory_order_relaxed);
  });
  // All on one line: the whole report comes from the guest thread, and writing fourteen lines in a
  // row in the middle of a frame is noticeable.
  const uint32_t tope = n < 10 ? n : 10;
  std::string lista;
  for (uint32_t i = 0; i < tope; ++i) {
    const Solido& s = g_solidos[orden[i]];
    const uint32_t dib = s.dibujos.load(std::memory_order_relaxed);
    const uint64_t tri = s.triangulos.load(std::memory_order_relaxed);
    lista += fmt::format("{}\"{}\" {:.1f}k en {:.0f} dib ({} tri/dib)", i == 0 ? "" : " | ", s.nombre,
                         double(tri) / f / 1000.0, double(dib) / f, dib != 0 ? uint32_t(tri / dib) : 0u);
  }
  const uint32_t perdidos = g_solidos_perdidos.load(std::memory_order_relaxed);
  REXLOG_INFO(
      "[sombras lod] C7 los {} solidos que mas triangulos meten en el mapa de sombras (por fotograma; "
      "{:.0f} dibujos no cupieron en la tabla): {}",
      tope, double(perdidos) / f, lista);
  ReiniciarCuentas();
}

// Once per frame: refreshes the hot hook's switch and writes the report when due.
void LatirFotograma(const uint8_t* base) {
  const bool activo = REXCVAR_GET(nfsmw_sombras_reparto);
  const bool antes = g_reparto_activo.exchange(activo, std::memory_order_relaxed);
  if (!activo) {
    if (antes) {
      ReiniciarCuentas();
      g_informe_ms.store(0, std::memory_order_relaxed);
    }
    return;
  }
  g_fotogramas.fetch_add(1, std::memory_order_relaxed);
  const int64_t ahora = AhoraMs();
  int64_t desde = g_informe_ms.load(std::memory_order_relaxed);
  if (desde == 0) {
    g_informe_ms.store(ahora, std::memory_order_relaxed);
    ReiniciarCuentas();
    return;
  }
  const int64_t periodo = int64_t(REXCVAR_GET(nfsmw_sombras_reparto_cada_s)) * 1000;
  if (ahora - desde >= periodo) {
    g_informe_ms.store(ahora, std::memory_order_relaxed);
    EmitirInforme(base, double(ahora - desde) / 1000.0);
  }
}

}  // namespace
}  // namespace nfsmw::sombras_lod

// =============================================================================================
// eViewPlatInterface::Render(eModel*, bMatrix4*, eLightContext*, uint32, bMatrix4*)
//
// The bottleneck all geometry goes through: the four emitters of the shadow pass end here, and so do
// the ~920 draws of the scene. It only counts; it changes nothing.
//
// The original is called first and the fields are read afterwards on purpose: the game does
// `Solid = *(model+0x0C)` without checking the model (nfsmw_recomp.105.cpp:15817) and then
// dereferences the Solid, so reading those two fields cannot touch any page the game has not
// already touched.
// =============================================================================================
REX_EXTERN(__imp__sub_8243E358);
// Render runs natively (nfsmw_eview_nativo.cpp, cvar nfsmw_eview_nativo). This is still the only hook
// of sub_8243E358: instead of the original it calls nfsmw::eview::Render, which chooses between the
// native version (checked against the original) and the original.
namespace nfsmw::eview {
void Render(PPCContext& ctx, uint8_t* base);
}
REX_HOOK_RAW(sub_8243E358) {
  using namespace nfsmw::sombras_lod;
  if (!g_reparto_activo.load(std::memory_order_relaxed)) {
    nfsmw::eview::Render(ctx, base);
    return;
  }
  const uint32_t vista = ctx.r3.u32;
  const uint32_t modelo = ctx.r4.u32;
  const uint8_t emisor = t_emisor;
  nfsmw::eview::Render(ctx, base);

  uint32_t id = 0;
  if (modelo == 0 || modelo >= kDireccionMaxima || !VistaValida(base, vista, &id)) {
    return;
  }
  const uint32_t solido = Leer32(base, modelo + kOffSolidEnModelo);
  uint32_t triangulos = 0;
  if (solido != 0 && solido < kDireccionMaxima) {
    const int32_t polys = Leer16Con(base, solido + kOffNumPolys);
    if (polys > 0 && polys <= kNumPolysMax) {
      triangulos = uint32_t(polys);
    }
  }
  g_celda[id][emisor].dibujos.fetch_add(1, std::memory_order_relaxed);
  if (triangulos != 0) {
    g_celda[id][emisor].triangulos.fetch_add(triangulos, std::memory_order_relaxed);
    if (EsVistaDeSombras(id)) {
      ApuntarSolido(base, solido, triangulos);
    }
  }
}

// =============================================================================================
// RenderWorldModels(eView* vista, int exc_flag)   (WorldModel.cpp:331)
//
// r3 = the view, r4 = the pass flags. In the shadow pass it is called twice per map: 0x200000 (the
// WorldModels with bones) and 0x202000 (the static ones). Two things happen here:
//   1. mark the emitter so eView::Render knows whom to credit the draws to
//   2. apply nfsmw_sombras_mundo_corte: raise PixelMinSize only during this call and restore the
//      original value on exit, so nobody else is overwritten (nfsmw_sombras_corte writes the same
//      field from the pass hook and keeps its own bookkeeping).
// =============================================================================================
REX_EXTERN(__imp__sub_824FA010);
REX_HOOK_RAW(sub_824FA010) {
  using namespace nfsmw::sombras_lod;
  const uint32_t vista = ctx.r3.u32;
  const uint32_t flags = ctx.r4.u32;
  uint32_t id = 0;
  const bool vista_ok = VistaValida(base, vista, &id);
  const bool en_sombras = vista_ok && EsVistaDeSombras(id) && (flags & kFlagSombra) != 0;

  const uint8_t emisor_previo = t_emisor;
  if (vista_ok) {
    // Outside the shadow pass bit 0x2000 means something else, so everything goes to "mundo-resto".
    t_emisor = ((flags & kFlagSombra) != 0 && (flags & kFlagSinHuesos) == 0) ? kEmisorMundoAnimado
                                                                            : kEmisorMundoResto;
  }

  // --- la palanca ------------------------------------------------------------------------------
  bool restaurar = false;
  uint32_t pmin_original = 0;
  const int32_t corte = REXCVAR_GET(nfsmw_sombras_mundo_corte);
  if (en_sombras && corte > int32_t(kRadioMundo) + 1) {
    const double h = double(LeerFlotante(base, vista + kOffHVista));
    pmin_original = Leer32(base, vista + kOffPixelMinSize);
    if (HRazonable(h) && pmin_original != 0 && pmin_original <= kPixelMinSizeMax) {
      const double px = PixelMinSizeParaCorte(h, double(corte));
      if (std::isfinite(px) && px > 0.0 && px <= double(kPixelMinSizeMax)) {
        const uint32_t pmin_nuevo = uint32_t(std::ceil(px));
        // Only tightens: if the game (or nfsmw_sombras_corte) already asks for more, nothing is touched.
        if (pmin_nuevo > pmin_original) {
          Escribir32(base, vista + kOffPixelMinSize, pmin_nuevo);
          restaurar = true;
          if (g_corte_anotado.exchange(corte) != corte) {
            const uint32_t vista_escena = kBaseVistas + kVistaEscena * kBytesPorVista;
            const double h_escena = double(LeerFlotante(base, vista_escena + kOffHVista));
            const uint32_t pmin_escena = Leer32(base, vista_escena + kOffPixelMinSize);
            const double corte_escena =
                (HRazonable(h_escena) && pmin_escena != 0 && pmin_escena <= kPixelMinSizeMax)
                    ? Distancia(h_escena, double(pmin_escena))
                    : 0.0;
            REXLOG_INFO(
                "[sombras lod] corte del mundo en la vista {}: H {:.1f}, PixelMinSize {} -> {} "
                "(los WorldModel dejan de proyectar sombra a {:.0f} unidades en vez de a {:.0f}). "
                "La escena deja de dibujarlos a {:.0f}. No toca coches (el juego exime a las vistas "
                "13 y 14 en CarRender) ni escenario",
                id, h, pmin_original, pmin_nuevo, Distancia(h, double(pmin_nuevo)),
                Distancia(h, double(pmin_original)), corte_escena);
          }
        }
      }
    }
  }

  __imp__sub_824FA010(ctx, base);

  if (restaurar) {
    Escribir32(base, vista + kOffPixelMinSize, pmin_original);
  }
  t_emisor = emisor_previo;
}

// =============================================================================================
// WorldModel::Render(eView* vista, int exc_flag)   (WorldModel.cpp:256)
//
// r3 = the WorldModel, r4 = the view, r5 = the flags. It only counts candidates: how many world
// objects were examined. Subtracting the ones that end up drawn shows how many the set of filters
// removes (mCastsShadow, bones, PixelMinSize and frustum), which is how to check whether
// nfsmw_sombras_mundo_corte bites.
// =============================================================================================
REX_EXTERN(__imp__sub_824F9D78);
REX_HOOK_RAW(sub_824F9D78) {
  using namespace nfsmw::sombras_lod;
  if (g_reparto_activo.load(std::memory_order_relaxed)) {
    uint32_t id = 0;
    if (VistaValida(base, ctx.r4.u32, &id)) {
      g_mundo_mirados[id].fetch_add(1, std::memory_order_relaxed);
    }
  }
  __imp__sub_824F9D78(ctx, base);
}

// =============================================================================================
// GrandSceneryCullInfo::StuffScenery(eView* vista, int stuff_flags)   (Scenery.cpp:1065)
//
// r3 = the GrandSceneryCullInfo, r4 = the view, r5 = the stuff_flags (512 in the shadow pass).
// It only marks the emitter: within this call, everything that goes through eView::Render is scenery.
// =============================================================================================
REX_EXTERN(__imp__sub_824C3610);
REX_HOOK_RAW(sub_824C3610) {
  using namespace nfsmw::sombras_lod;
  uint32_t id = 0;
  const bool marcar = VistaValida(base, ctx.r4.u32, &id);
  const uint8_t emisor_previo = t_emisor;
  if (marcar) {
    t_emisor = kEmisorEscenario;
  }
  __imp__sub_824C3610(ctx, base);
  t_emisor = emisor_previo;
}

// =============================================================================================
// GrandSceneryCullInfo::DoCulling. Builds the frame's SceneryCullInfo entries and then walks them.
// sub_82440890 calls it and only sub_82445660 calls that one, so: once per race frame. That is why
// the report heartbeat goes here.
// Only ExcludeFlags (+132) is touched here. H (+192) cannot be touched here: the first loop of this
// same function copies it from the view (Scenery.cpp:1005).
// =============================================================================================
REX_EXTERN(__imp__sub_824C3468);
REX_HOOK_RAW(sub_824C3468) {
  using namespace nfsmw::sombras_lod;
  LatirFotograma(base);
  const uint32_t objeto = ctx.r3.u32;
  if (objeto != 0 && REXCVAR_GET(nfsmw_sombras_lod) && REXCVAR_GET(nfsmw_sombras_lod_h) == 0) {
    const uint32_t cuenta = Leer32(base, objeto + kOffContador);
    if (cuenta != 0 && cuenta <= kRegistrosMax) {
      const bool diag = REXCVAR_GET(nfsmw_sombras_lod_diag) && !g_anotado_diag.exchange(true);
      for (uint32_t i = 0; i < cuenta; ++i) {
        const uint32_t registro = objeto + i * kRegistroBytes;
        const uint32_t vista = Leer32(base, registro + kOffVista);
        uint32_t id = 0;
        if (!VistaValida(base, vista, &id)) {
          continue;
        }
        const uint32_t mascara = Leer32(base, registro + kOffMascara);
        if (diag) {
          REXLOG_INFO("[sombras lod] registro {}: vista {} mascara 0x{:08X}", i, id, mascara);
        }
        if (!EsVistaDeSombras(id) || (mascara & kBitMallaReducida) != 0) {
          continue;
        }
        Escribir32(base, registro + kOffMascara, mascara | kBitMallaReducida);
        if (!g_anotado.exchange(true)) {
          REXLOG_INFO(
              "[sombras lod] vista {}: mascara 0x{:08X} -> 0x{:08X}; el umbral de tamano sube de 17 a 32 px",
              id, mascara, mascara | kBitMallaReducida);
        }
      }
    }
  }
  __imp__sub_824C3468(ctx, base);
}

// =============================================================================================
// GrandSceneryCullInfo::CullView(SceneryCullInfo*). r3 = GrandSceneryCullInfo, r4 = the cull_info.
// Called after DoCulling copies H and before TreeCull/DrawAScenery use it: the only window where
// writing +192 does anything. Measured inert (see above): nfsmw_sombras_lod_h defaults to 0 and this
// does nothing unless turned on by hand.
// =============================================================================================
REX_EXTERN(__imp__sub_824C33D0);
REX_HOOK_RAW(sub_824C33D0) {
  using namespace nfsmw::sombras_lod;
  const uint32_t registro = ctx.r4.u32;

  // The count that answers the hypothesis: how many times this hook runs with a cull_info of view 13
  // or 14, and how many objects the culling leaves in that view's list. With 0 runs, the scenery is not
  // culled for the shadow map and the three cull_info levers could do nothing. With N runs and M
  // objects, if the "escenario" emitter of the C7 report draws far fewer than M, the trimming comes
  // from the caster filter of StuffScenery.
  const bool registro_ok = registro != 0 && registro < kDireccionMaxima;
  uint32_t id_cuenta = 0;
  const bool contar = registro_ok && g_reparto_activo.load(std::memory_order_relaxed) &&
                      VistaValida(base, Leer32(base, registro + kOffVista), &id_cuenta);
  uint32_t antes_dibujo = 0;
  if (contar) {
    g_cull_pasadas[id_cuenta].fetch_add(1, std::memory_order_relaxed);
    antes_dibujo = Leer32(base, registro + kOffUltimoDibujo);
  }

  const int32_t pct = REXCVAR_GET(nfsmw_sombras_lod_h);
  if (registro_ok && pct != 0) {
    uint32_t id = 0;
    const uint32_t vista = Leer32(base, registro + kOffVista);
    if (VistaValida(base, vista, &id)) {
      if (id == kVistaEscena) {
        // Stored for the report below: with both cameras it is known how much each % trims.
        for (uint32_t i = 0; i < 3; ++i) {
          g_escena_pos[i] = LeerFlotante(base, registro + kOffPosicion + i * 4);
        }
        g_escena_vista.store(true, std::memory_order_relaxed);
      } else if (EsVistaDeSombras(id)) {
        const double h_escena = double(LeerFlotante(base, kBaseVistas + kVistaEscena * kBytesPorVista + kOffHVista));
        const double h_mapa = double(LeerFlotante(base, vista + kOffHVista));
        const double h_actual = double(LeerFlotante(base, registro + kOffHRegistro));
        if (HRazonable(h_escena) && HRazonable(h_mapa) && HRazonable(h_actual)) {
          const double h_pedida = h_escena * double(pct) / 100.0;
          // This lever only tightens: if the request is more permissive than the game, nothing is touched.
          if (h_pedida < h_actual) {
            EscribirFlotante(base, registro + kOffHRegistro, float(h_pedida));
            if (g_pct_anotado.exchange(pct) != pct) {
              REXLOG_INFO(
                  "[sombras lod] H: escena (vista {}) {:.1f}, mapa (vista {}) {:.1f} = {:.2f}x; con {} % "
                  "escribo {:.1f} en el cull_info, o sea x{:.3f} de lo que usa el juego. OJO: esto SOLO "
                  "afecta al escenario, y el que llega al mapa de sombras ya esta filtrado por el bit de "
                  "caster; medido inerte en la compilacion 114",
                  kVistaEscena, h_escena, id, h_mapa, h_mapa / h_escena, pct, h_pedida, h_pedida / h_mapa);
              REXLOG_INFO(
                  "[sombras lod] con esa H un objeto de radio {:.0f} deja de proyectar sombra pasadas "
                  "{:.0f} unidades (liston 17 px; el juego la quitaba a {:.0f}) y baja a la malla reducida "
                  "pasadas {:.0f} (liston {:.0f} px; el juego bajaba a {:.0f}). En la escena esos dos "
                  "limites estan en {:.0f} y {:.0f}",
                  kRadioDeReferencia, Distancia(h_pedida, kUmbralDibujar), Distancia(h_mapa, kUmbralDibujar),
                  Distancia(h_pedida, kUmbralMallaBuena), kUmbralMallaBuena,
                  Distancia(h_mapa, kUmbralMallaBuena), Distancia(h_escena, kUmbralDibujar),
                  Distancia(h_escena, kUmbralMallaBuena));
            }
            if (g_escena_vista.load(std::memory_order_relaxed) && !g_anotado_camaras.exchange(true)) {
              const double dx = double(LeerFlotante(base, registro + kOffPosicion + 0)) - double(g_escena_pos[0]);
              const double dy = double(LeerFlotante(base, registro + kOffPosicion + 4)) - double(g_escena_pos[1]);
              const double dz = double(LeerFlotante(base, registro + kOffPosicion + 8)) - double(g_escena_pos[2]);
              REXLOG_INFO(
                  "[sombras lod] camaras: la escena mira desde ({:.0f}, {:.0f}, {:.0f}) y el mapa desde "
                  "({:.0f}, {:.0f}, {:.0f}); separacion {:.0f} unidades. Cuanto mas lejos este la luz, mas "
                  "agresivo es el mismo porcentaje",
                  double(g_escena_pos[0]), double(g_escena_pos[1]), double(g_escena_pos[2]),
                  double(LeerFlotante(base, registro + kOffPosicion + 0)),
                  double(LeerFlotante(base, registro + kOffPosicion + 4)),
                  double(LeerFlotante(base, registro + kOffPosicion + 8)),
                  std::sqrt(dx * dx + dy * dy + dz * dz));
            }
          }
        }
      }
    }
  }
  __imp__sub_824C33D0(ctx, base);

  if (contar) {
    // pCurrentDrawInfo advances 12 bytes per object the culling adds to this view's list.
    const uint32_t despues = Leer32(base, registro + kOffUltimoDibujo);
    const uint32_t primero = Leer32(base, registro + kOffPrimerDibujo);
    if (despues > antes_dibujo && antes_dibujo >= primero && primero != 0) {
      const uint32_t bytes = despues - antes_dibujo;
      if (bytes % kDrawInfoBytes == 0 && bytes < 0x400000u) {
        g_cull_objetos[id_cuenta].fetch_add(bytes / kDrawInfoBytes, std::memory_order_relaxed);
      }
    }
  }
}
