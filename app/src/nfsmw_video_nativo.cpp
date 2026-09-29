// nfsmw - cutscenes: the game's WMV3 decoding replaced by FFmpeg's
//
// On the Switch the XDK's recompiled WMV3 decoder runs at ~98 % of a core and produces ~22 frames per
// second: cutscenes drop from ~60 to ~20 FPS and the audio ends before the picture. The game's decoder
// interface (recompiled code):
//  - sub_8272FD30 prepares a movie's context; [ctx+3300] points to {application data, function}.
//  - sub_827312C0 (DecodeData, from sub_82734040) requests the compressed frame in chunks through
//    sub_82749C10, which jumps to that function with r4 = offset, r5 = &pointer to the data, r6 = bytes
//    requested, r7 = &bytes returned and r8 = &data remaining. It reads the picture header, swaps the
//    buffers and decodes according to the type ([ctx+280]): I with [ctx+15708] = sub_828C35D8, P with
//    [ctx+15712] = sub_8278A518 and B with [ctx+3016] = sub_828C58C8. Those, with what they call, take
//    almost all of the video thread's time.
//  - The new picture goes into planes [ctx+3672] (Y), [ctx+3676] (U) and [ctx+3680] (V), with the
//    origin at +[ctx+216] and +[ctx+220] (32- and 16-pixel borders: strides of 1344 and 672 for
//    1280x720). DecodeData rotates three buffers; [ctx+3684/3688/3692] is the reference.
//  - [ctx+3844] is the loop filter of the sequence (LOOPFILTER). With 1, the three functions also run a
//    filter over the picture (FFmpeg's WMV3 decoder applies it too). When they finish, I sets
//    [ctx+15516] = 0; P also sets [ctx+15488] = 1 and [ctx+15512] = ([ctx+3844], [ctx+14776] != 0 or
//    [ctx+15148] != -1); B sets [ctx+15516] = 0 and [ctx+15512] = [ctx+15488] = [ctx+436] = 1.
//  - The game's videos are I and P frames without the loop filter. The dubbed videos of a Brazilian
//    Portuguese fan translation use the loop filter, extended motion vectors and one B frame between
//    anchors. With them the game's recompiled decoder leaves the picture at zero (it shows green) and takes
//    up to 330 ms per frame even on the PC; FFmpeg decodes them correctly.
//
// Cvars:
//  - nfsmw_video_wmv3_nativo: sub_828C35D8, sub_8278A518 and sub_828C58C8 do not decode. FFmpeg decodes
//    the same bytes the game requested, without reordering (each frame comes out of its own call), and its
//    planes are copied into the game's buffers. If a frame cannot be replaced: in the game's videos the game
//    decodes it, and the next native one waits for an I frame; in the others the last good picture (or
//    black) is repeated until the next I frame, because the game's decoder does not work with them.
//  - nfsmw_video_wmv3_sombra (diagnostic): the game decodes and its planes are compared with FFmpeg's;
//    the luma of frame 30 of each movie is saved as PGM in the working folder.
//  - nfsmw_video_wmv3_datos_diag (diagnostic): logs the calls to the data function, the arguments of
//    sub_8272FD30 and the context fields of each movie.
// FFmpeg needs the size and the 4 sequence bytes: they come from the ASF header of the last movie the
// game read (xboxkrnl_io.cpp in the SDK).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "nfsmw_video_nativo.h"
#include "nfsmw_video_wmv3.h"

namespace rex::kernel::xboxkrnl {
std::string NfsmwUltimoWmvLeido();  // SDK xboxkrnl_io.cpp: last .wmv movie the game read
}  // namespace rex::kernel::xboxkrnl

// On by default: on the PC the four intro movies play every frame through FFmpeg with no rejections,
// and the shadow comparison gave bit-identical planes.
REXCVAR_DEFINE_BOOL(nfsmw_video_wmv3_nativo, true, "NFSMW",
                    "Cinematicas: descodifica los fotogramas WMV3 con FFmpeg en lugar del descodificador "
                    "recompilado del juego (mismos bytes y mismos buferes de imagen)");
REXCVAR_DEFINE_BOOL(nfsmw_video_wmv3_sombra, false, "NFSMW",
                    "Diagnostico: el juego descodifica y se comparan sus planos con los de FFmpeg para los "
                    "mismos bytes; guarda la luma del fotograma 30 de cada pelicula en PGM");
REXCVAR_DEFINE_BOOL(nfsmw_video_wmv3_datos_diag, false, "NFSMW",
                    "Diagnostico: anota las llamadas a la funcion de datos del descodificador WMV3 y los campos "
                    "del contexto de cada pelicula");
// The dubbed videos of a fan translation have B frames (the game's do not). The game decodes them with
// [ctx+3016] = sub_828C58C8. This diagnostic logs, for the first frames of each movie, the type ([ctx+280]:
// 0 I, 1 P, 2 B), which planes each decode changes and which context fields it writes, to know where the game
// leaves the picture of a B frame and what has to be imitated.
REXCVAR_DEFINE_BOOL(nfsmw_video_wmv3_b_diag, false, "NFSMW",
                    "Diagnostico: en los primeros fotogramas de cada pelicula anota el tipo, los planos que cambia "
                    "cada descodificacion (I, P y B) y los campos del contexto que escribe");

REX_EXTERN(__imp__sub_82749C10);
REX_EXTERN(__imp__sub_827312C0);
REX_EXTERN(__imp__sub_8278A518);
REX_EXTERN(__imp__sub_828C35D8);
REX_EXTERN(__imp__sub_828C58C8);
REX_EXTERN(__imp__sub_8272FD30);

namespace nfsmw::video_nativo {

namespace {
std::atomic<uint64_t> g_fotogramas_nativos{0};
}  // namespace

uint64_t FotogramasNativos() {
  return g_fotogramas_nativos.load(std::memory_order_relaxed);
}

namespace {

using video_wmv3::DescodificadorWmv3;
using video_wmv3::Fotograma;
using video_wmv3::InfoWmv;

constexpr size_t kMaxFotograma = 8 * 1024 * 1024;
constexpr uint32_t kDescodificarI = 0x828C35D8;
constexpr uint32_t kDescodificarP = 0x8278A518;
constexpr uint32_t kDescodificarB = 0x828C58C8;  // [ctx+3016], in the B branch of DecodeData
// Type of frame that goes through the hooks
enum Tipo : int { kI = 0, kP = 1, kB = 2 };
const char* NombreTipo(int tipo) {
  return tipo == kI ? "I" : tipo == kP ? "P" : "B";
}

uint32_t Leer32(const uint8_t* base, uint32_t direccion) {
  uint32_t v = 0;
  std::memcpy(&v, base + direccion, sizeof(v));
  return __builtin_bswap32(v);
}

void Escribir32(uint8_t* base, uint32_t direccion, uint32_t valor) {
  const uint32_t v = __builtin_bswap32(valor);
  std::memcpy(base + direccion, &v, sizeof(v));
}

int64_t AhoraUs() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

bool Activo() {
  return REXCVAR_GET(nfsmw_video_wmv3_nativo) || REXCVAR_GET(nfsmw_video_wmv3_sombra) ||
         REXCVAR_GET(nfsmw_video_wmv3_datos_diag) || REXCVAR_GET(nfsmw_video_wmv3_b_diag);
}

// --- Compressed frame the game requests ------------------------------------------------------------------

struct Captura {
  uint32_t ctx = 0;
  uint32_t estructura = 0;  // r3 of sub_82749C10: {application data, function}
  uint32_t secciones = 0;
  bool quedan = false;      // the last section says frame bytes remain
  bool error = false;
  std::vector<uint8_t> datos;
};
Captura g_captura;
std::mutex g_captura_m;
thread_local Captura* t_captura = nullptr;  // non-null inside sub_827312C0 on this thread
std::atomic<uint32_t> g_datos_anotados{0};

void AnotarSeccion(const uint8_t* base, uint32_t lr, uint32_t estructura, uint32_t desplazamiento, uint32_t pedidos,
                   uint32_t p_datos, uint32_t p_bytes, uint32_t p_quedan, uint32_t resultado) {
  Captura& c = *t_captura;
  const uint32_t datos = p_datos ? Leer32(base, p_datos) : 0;
  const uint32_t bytes = p_bytes ? Leer32(base, p_bytes) : 0;
  const uint32_t quedan = p_quedan ? Leer32(base, p_quedan) : 0;
  c.estructura = estructura;
  c.quedan = quedan != 0;
  ++c.secciones;
  if (bytes) {
    if (datos && uint64_t(datos) + bytes <= 0x100000000ull && c.datos.size() + bytes <= kMaxFotograma) {
      c.datos.insert(c.datos.end(), base + datos, base + datos + bytes);
    } else {
      c.error = true;
    }
  }
  if (REXCVAR_GET(nfsmw_video_wmv3_datos_diag) && g_datos_anotados.fetch_add(1, std::memory_order_relaxed) < 80) {
    REXLOG_INFO("[video] datos: lr={:08X} ctx={:08X} estructura={:08X} ({:08X} {:08X}) desplazamiento={} "
                "pedidos={} -> resultado={:08X} datos={:08X} bytes={} quedan={} | seccion {}, {} bytes",
                lr, c.ctx, estructura, Leer32(base, estructura), Leer32(base, estructura + 4), desplazamiento,
                pedidos, resultado, datos, bytes, quedan, c.secciones, c.datos.size());
  }
}

// Asks the game's data function for the rest of the frame, like the game's bit reader (sub_82734DC8,
// object [ctx+76]): structure [lector+44], offset 0, 4 bytes requested and "remaining" in [lector+24].
//  - With a non-zero offset the application function (sub_827204A0) takes another path: the game
//    crashed.
//  - "remaining" must end up in [lector+24]: at the end of DecodeData, sub_8272E128 keeps requesting
//    chunks while it is 1. With "remaining" in another variable the next frame was swallowed
//    (eahd_bumper frame 81 of 120) and at the end of the movie it kept waiting for data.
void CompletarFotograma(PPCContext& ctx, uint8_t* base, uint32_t obj) {
  Captura& c = *t_captura;
  const uint32_t lector = Leer32(base, obj + 76);
  const uint32_t estructura = Leer32(base, lector + 44);
  const uint64_t r1 = ctx.r1.u64;
  const uint64_t lr = ctx.lr;
  const uint32_t sp = ctx.r1.u32 - 128;
  Escribir32(base, sp, ctx.r1.u32);  // stack back chain
  for (int i = 0; Leer32(base, lector + 24) != 0 && !c.error && i < 4096; ++i) {
    const size_t antes = c.datos.size();
    Escribir32(base, sp + 80, 0);
    Escribir32(base, sp + 88, 0);
    ctx.r1.u64 = sp;
    ctx.r3.u64 = estructura;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = sp + 88;
    ctx.r6.u64 = 4;
    ctx.r7.u64 = sp + 80;
    ctx.r8.u64 = lector + 24;
    __imp__sub_82749C10(ctx, base);
    AnotarSeccion(base, 0, estructura, 0, 4, sp + 88, sp + 80, lector + 24, ctx.r3.u32);
    if (c.datos.size() == antes) {
      break;
    }
  }
  ctx.r1.u64 = r1;
  ctx.lr = lr;
}

// --- The game's picture planes -------------------------------------------------------------------------

struct Planos {
  uint8_t* y = nullptr;
  uint8_t* u = nullptr;
  uint8_t* v = nullptr;
  int paso_y = 0;
  int paso_c = 0;
};

// campo: 3672 is the set of planes of the new picture ([ctx+3672/3676/3680]); 3684, the other set of planes
// ([ctx+3684/3688/3692]), which the B branch of DecodeData copies over the first one in some cases.
bool PlanosDelJuego(uint8_t* base, uint32_t obj, int ancho, Planos& p, uint32_t campo = 3672) {
  const uint32_t off_y = Leer32(base, obj + 216);
  const uint32_t off_c = Leer32(base, obj + 220);
  if (off_y < 32 || off_c < 16 || (off_y - 32) % 32 != 0 || (off_c - 16) % 16 != 0) {
    return false;
  }
  p.paso_y = int((off_y - 32) / 32);
  p.paso_c = int((off_c - 16) / 16);
  if (p.paso_y < ancho || p.paso_c < (ancho + 1) / 2) {
    return false;
  }
  p.y = base + Leer32(base, obj + campo) + off_y;
  p.u = base + Leer32(base, obj + campo + 4) + off_c;
  p.v = base + Leer32(base, obj + campo + 8) + off_c;
  return true;
}

// --- B frame diagnostic -------------------------------------------------------------------------------------

constexpr uint32_t kBytesContexto = 20480;  // the context fields in use go up to +19972
constexpr uint64_t kFotogramasDiagB = 48;

// FNV-1a of the visible luma, one row in four: enough to see which plane a decode changes.
uint64_t HuellaLuma(uint8_t* base, uint32_t obj, uint32_t campo, int ancho, int alto) {
  Planos p;
  if (!PlanosDelJuego(base, obj, ancho, p, campo)) {
    return 0;
  }
  uint64_t h = 1469598103934665603ull;
  for (int y = 0; y < alto; y += 4) {
    const uint8_t* fila = p.y + int64_t(y) * p.paso_y;
    for (int x = 0; x < ancho; ++x) {
      h = (h ^ fila[x]) * 1099511628211ull;
    }
  }
  return h;
}

struct FotoContexto {
  std::vector<uint8_t> ctx;
  uint32_t planos[6] = {};   // [ctx+3672 .. 3692]
  uint64_t luma[2] = {0, 0};  // plane sets 3672 and 3684
};

void Fotografiar(uint8_t* base, uint32_t obj, int ancho, int alto, FotoContexto& f) {
  f.ctx.assign(base + obj, base + obj + kBytesContexto);
  for (int i = 0; i < 6; ++i) {
    f.planos[i] = Leer32(base, obj + 3672 + 4 * uint32_t(i));
  }
  f.luma[0] = HuellaLuma(base, obj, 3672, ancho, alto);
  f.luma[1] = HuellaLuma(base, obj, 3684, ancho, alto);
}

void AnotarCambios(uint8_t* base, uint32_t obj, int ancho, int alto, uint64_t n, int tipo, uint32_t resultado,
                   const FotoContexto& antes) {
  FotoContexto despues;
  Fotografiar(base, obj, ancho, alto, despues);
  std::string campos;
  int anotados = 0;
  int cambiados = 0;
  for (uint32_t off = 0; off + 4 <= kBytesContexto; off += 4) {
    uint32_t a = 0;
    uint32_t d = 0;
    std::memcpy(&a, antes.ctx.data() + off, 4);
    std::memcpy(&d, despues.ctx.data() + off, 4);
    if (a == d) {
      continue;
    }
    ++cambiados;
    if (anotados++ < 40) {
      campos += fmt::format(" +{}:{:X}>{:X}", off, __builtin_bswap32(a), __builtin_bswap32(d));
    }
  }
  REXLOG_INFO("[video] B diag #{} {} (tipo {} en +280) -> {:08X} | luma 3672 {} | luma 3684 {} | planos "
              "{:08X}/{:08X} -> {:08X}/{:08X} | {} campos cambiados:{}",
              n, NombreTipo(tipo), Leer32(base, obj + 280), resultado,
              antes.luma[0] == despues.luma[0] ? "igual" : "CAMBIA",
              antes.luma[1] == despues.luma[1] ? "igual" : "CAMBIA",
              antes.planos[0], antes.planos[3], despues.planos[0], despues.planos[3], cambiados, campos);
}

void CopiarPlanos(const Fotograma& f, const Planos& g) {
  const int ancho_c = (f.ancho + 1) / 2;
  const int alto_c = (f.alto + 1) / 2;
  for (int y = 0; y < f.alto; ++y) {
    std::memcpy(g.y + int64_t(y) * g.paso_y, f.planos[0] + int64_t(y) * f.pasos[0], size_t(f.ancho));
  }
  for (int y = 0; y < alto_c; ++y) {
    std::memcpy(g.u + int64_t(y) * g.paso_c, f.planos[1] + int64_t(y) * f.pasos[1], size_t(ancho_c));
    std::memcpy(g.v + int64_t(y) * g.paso_c, f.planos[2] + int64_t(y) * f.pasos[2], size_t(ancho_c));
  }
}

struct Diferencia {
  int max = 0;
  double media = 0.0;
  uint64_t malos = 0;  // pixels differing by more than 3
};

Diferencia CompararPlano(const uint8_t* g, int paso_g, const uint8_t* n, int paso_n, int ancho, int alto) {
  Diferencia d;
  uint64_t suma = 0;
  for (int y = 0; y < alto; ++y) {
    const uint8_t* fg = g + int64_t(y) * paso_g;
    const uint8_t* fn = n + int64_t(y) * paso_n;
    for (int x = 0; x < ancho; ++x) {
      const int v = std::abs(int(fg[x]) - int(fn[x]));
      suma += uint64_t(v);
      d.max = std::max(d.max, v);
      d.malos += v > 3;
    }
  }
  d.media = double(suma) / double(std::max<int64_t>(int64_t(ancho) * alto, 1));
  return d;
}

void GuardarPgm(const std::string& nombre, const uint8_t* plano, int paso, int ancho, int alto, int escala,
                const uint8_t* resta = nullptr, int paso_resta = 0) {
  FILE* f = std::fopen(nombre.c_str(), "wb");
  if (!f) {
    return;
  }
  std::fprintf(f, "P5\n%d %d\n255\n", ancho, alto);
  std::vector<uint8_t> fila(static_cast<size_t>(ancho));
  for (int y = 0; y < alto; ++y) {
    const uint8_t* p = plano + int64_t(y) * paso;
    if (resta) {
      const uint8_t* r = resta + int64_t(y) * paso_resta;
      for (int x = 0; x < ancho; ++x) {
        fila[size_t(x)] = uint8_t(std::min(255, std::abs(int(p[x]) - int(r[x])) * escala));
      }
    } else {
      std::memcpy(fila.data(), p, size_t(ancho));
    }
    std::fwrite(fila.data(), 1, fila.size(), f);
  }
  std::fclose(f);
}

// --- Pelicula en curso ---------------------------------------------------------------------------------

struct Pelicula {
  uint32_t obj = 0;
  std::string ruta;
  DescodificadorWmv3 dec;
  bool preparada = false;      // FFmpeg open and the game's configuration known
  bool desincronizado = true;  // FFmpeg is waiting for an I frame
  // The game's videos are I and P frames without the loop filter, and its decoder works as a fallback. In the
  // others (B frames or the loop filter, like the dubbed videos of a translation) the game's recompiled decoder
  // leaves the picture at zero (it shows green): if FFmpeg fails, the last good picture is repeated.
  bool con_b = false;
  bool juego_fiable = true;
  std::vector<uint8_t> ultima;  // last good picture: Y, U and V one after another
  int ultima_ancho = 0;
  int ultima_alto = 0;
  uint64_t fotogramas = 0;     // I, P and B frames that went through the hooks
  uint64_t nativos = 0;
  uint64_t rechazados = 0;
  uint64_t repetidos = 0;
  uint64_t avisos = 0;
  int peor_max_y = 0;
  double peor_media_y = 0.0;
  // Summary every 5 s
  int64_t desde_us = 0;
  uint64_t n_juego = 0;
  uint64_t n_nativo = 0;
  uint64_t n_sombra = 0;
  int64_t us_juego = 0;
  int64_t us_nativo = 0;
};
std::mutex g_peli_m;
std::unique_ptr<Pelicula> g_peli;

void Resumen(Pelicula& p, int64_t ahora) {
  if (p.desde_us == 0) {
    p.desde_us = ahora;
    return;
  }
  if (ahora - p.desde_us < 5000000) {
    return;
  }
  const double s = double(ahora - p.desde_us) / 1e6;
  REXLOG_INFO("[video] WMV3 '{}': {:.1f} fotogramas/s | juego {} a {:.2f} ms | FFmpeg {} a {:.2f} ms | sombra {}",
              p.ruta, double(p.n_juego + p.n_nativo) / s, p.n_juego,
              p.n_juego ? double(p.us_juego) / double(p.n_juego) / 1000.0 : 0.0, p.n_nativo,
              p.n_nativo ? double(p.us_nativo) / double(p.n_nativo) / 1000.0 : 0.0, p.n_sombra);
  p.desde_us = ahora;
  p.n_juego = p.n_nativo = p.n_sombra = 0;
  p.us_juego = p.us_nativo = 0;
}

void ResumenFinal(const Pelicula& p) {
  REXLOG_INFO("[video] WMV3 fin de '{}' (contexto {:08X}): {} fotogramas, {} con FFmpeg, {} rechazados ({} con la "
              "ultima imagen repetida); sombra: peor Y max {} media {:.3f}",
              p.ruta, p.obj, p.fotogramas, p.nativos, p.rechazados, p.repetidos, p.peor_max_y, p.peor_media_y);
}

// Con g_peli_m tomado.
Pelicula& PeliculaDe(const uint8_t* base, uint32_t obj) {
  if (g_peli && g_peli->obj == obj) {
    return *g_peli;
  }
  if (g_peli) {
    ResumenFinal(*g_peli);
  }
  g_peli = std::make_unique<Pelicula>();
  Pelicula& p = *g_peli;
  p.obj = obj;
  p.ruta = rex::kernel::xboxkrnl::NfsmwUltimoWmvLeido();
  InfoWmv info;
  const bool info_ok = !p.ruta.empty() && video_wmv3::LeerInfoWmv(p.ruta, info);
  // [ctx+3844] is the loop filter of the sequence (LOOPFILTER): with 1, sub_828C35D8, sub_8278A518 and
  // sub_828C58C8 also run a filter over the picture, which FFmpeg's WMV3 decoder applies too. B frames: the 3
  // MAXBFRAMES bits of the sequence (STRUCT_C), which the game decodes with [ctx+3016].
  const uint32_t filtro = Leer32(base, obj + 3844);
  p.con_b = info_ok && info.secuencia.size() >= 4 && ((info.secuencia[3] >> 4) & 7) != 0;
  p.juego_fiable = filtro == 0 && !p.con_b;
  const bool config_ok = filtro <= 1 && Leer32(base, obj + 15424) == 6 &&
                         Leer32(base, obj + 15708) == kDescodificarI && Leer32(base, obj + 15712) == kDescodificarP &&
                         (!p.con_b || Leer32(base, obj + 3016) == kDescodificarB);
  // With the diagnostics, FFmpeg is opened even if the configuration is different: the shadow compares anyway.
  const bool comparar = REXCVAR_GET(nfsmw_video_wmv3_sombra) || REXCVAR_GET(nfsmw_video_wmv3_b_diag);
  p.preparada = info_ok && (config_ok || comparar) && p.dec.Abrir(info) && config_ok;
  const auto& s = info.secuencia;
  REXLOG_INFO("[video] WMV3: contexto {:08X} -> '{}' {}x{} secuencia {:02X}{:02X}{:02X}{:02X}; filtro de bloques {}, "
              "fotogramas B {}; configuracion {}; FFmpeg {}",
              obj, p.ruta, info.ancho, info.alto, s.size() > 0 ? s[0] : 0, s.size() > 1 ? s[1] : 0,
              s.size() > 2 ? s[2] : 0, s.size() > 3 ? s[3] : 0, filtro, p.con_b ? "si" : "no",
              config_ok ? "conocida" : "distinta", p.preparada ? "listo" : "no disponible");
  if (REXCVAR_GET(nfsmw_video_wmv3_datos_diag) || REXCVAR_GET(nfsmw_video_wmv3_b_diag)) {
    REXLOG_INFO("[video] contexto {:08X}: +3300={:08X} +15708={:08X} +15712={:08X} +3016={:08X} +15424={} "
                "+3844={} +132={} +136={} +200={} +204={} +208={} +216={:X} +220={:X} +14740={} +18384={} +3872={}",
                obj, Leer32(base, obj + 3300), Leer32(base, obj + 15708), Leer32(base, obj + 15712),
                Leer32(base, obj + 3016), Leer32(base, obj + 15424), Leer32(base, obj + 3844),
                Leer32(base, obj + 132), Leer32(base, obj + 136), Leer32(base, obj + 200), Leer32(base, obj + 204),
                Leer32(base, obj + 208), Leer32(base, obj + 216), Leer32(base, obj + 220), Leer32(base, obj + 14740),
                Leer32(base, obj + 18384), Leer32(base, obj + 3872));
  }
  return p;
}

// The context as the game's functions leave it when they finish: sub_828C35D8 (I), sub_8278A518 (P) and
// sub_828C58C8 (B). The B branch of DecodeData then sets [ctx+15512] and [ctx+15488] to its own value.
void DejarContexto(uint8_t* base, uint32_t obj, int tipo) {
  if (tipo == kP) {
    const bool marca = Leer32(base, obj + 3844) != 0 || Leer32(base, obj + 14776) != 0 ||
                       Leer32(base, obj + 15148) != 0xFFFFFFFFu;
    Escribir32(base, obj + 15512, marca ? 1 : 0);
    Escribir32(base, obj + 15488, 1);
  } else if (tipo == kB) {
    Escribir32(base, obj + 15512, 1);
    Escribir32(base, obj + 15488, 1);
    Escribir32(base, obj + 436, 1);
  }
  Escribir32(base, obj + 15516, 0);
}

void GuardarUltima(Pelicula& p, const Fotograma& f) {
  const int ancho_c = (f.ancho + 1) / 2;
  const int alto_c = (f.alto + 1) / 2;
  p.ultima.resize(size_t(f.ancho) * size_t(f.alto) + 2 * size_t(ancho_c) * size_t(alto_c));
  uint8_t* d = p.ultima.data();
  for (int y = 0; y < f.alto; ++y, d += f.ancho) {
    std::memcpy(d, f.planos[0] + int64_t(y) * f.pasos[0], size_t(f.ancho));
  }
  for (int k = 1; k < 3; ++k) {
    for (int y = 0; y < alto_c; ++y, d += ancho_c) {
      std::memcpy(d, f.planos[k] + int64_t(y) * f.pasos[k], size_t(ancho_c));
    }
  }
  p.ultima_ancho = f.ancho;
  p.ultima_alto = f.alto;
}

// Without a good picture yet, black: the game's planes at zero show green.
void RepetirUltima(uint8_t* base, const Pelicula& p) {
  Planos g;
  if (p.ultima.empty()) {
    const int ancho = p.dec.ancho();
    const int alto = p.dec.alto();
    if (ancho <= 0 || alto <= 0 || !PlanosDelJuego(base, p.obj, ancho, g)) {
      return;
    }
    for (int y = 0; y < alto; ++y) {
      std::memset(g.y + int64_t(y) * g.paso_y, 16, size_t(ancho));
    }
    for (int y = 0; y < (alto + 1) / 2; ++y) {
      std::memset(g.u + int64_t(y) * g.paso_c, 128, size_t((ancho + 1) / 2));
      std::memset(g.v + int64_t(y) * g.paso_c, 128, size_t((ancho + 1) / 2));
    }
    return;
  }
  if (!PlanosDelJuego(base, p.obj, p.ultima_ancho, g)) {
    return;
  }
  const int ancho_c = (p.ultima_ancho + 1) / 2;
  const int alto_c = (p.ultima_alto + 1) / 2;
  Fotograma f;
  f.ancho = p.ultima_ancho;
  f.alto = p.ultima_alto;
  f.planos[0] = p.ultima.data();
  f.pasos[0] = f.ancho;
  f.planos[1] = f.planos[0] + size_t(f.ancho) * size_t(f.alto);
  f.pasos[1] = ancho_c;
  f.planos[2] = f.planos[1] + size_t(ancho_c) * size_t(alto_c);
  f.pasos[2] = ancho_c;
  CopiarPlanos(f, g);
}

// A frame that FFmpeg does not replace. In the game's videos the game decodes it (false). In the others its
// decoder does not work: the last good picture is repeated and the frame is done (true, r3 = 0).
bool Rechazar(PPCContext& ctx, uint8_t* base, Pelicula& p, int tipo, const char* motivo) {
  ++p.rechazados;
  p.desincronizado = true;
  const bool repetir = !p.juego_fiable;
  if (p.avisos++ < 10) {
    REXLOG_WARN("[video] WMV3 nativo: el fotograma {} ({}) de '{}' {}: {}", p.fotogramas, NombreTipo(tipo), p.ruta,
                repetir ? "repite la ultima imagen" : "lo descodifica el juego", motivo);
  }
  if (!repetir) {
    return false;
  }
  RepetirUltima(base, p);
  DejarContexto(base, p.obj, tipo);
  ctx.r3.u64 = 0;
  ++p.fotogramas;
  ++p.repetidos;
  return true;
}

// Replaces the decoding of one frame. true if r3 already holds the result.
bool SustituirFotograma(PPCContext& ctx, uint8_t* base, Pelicula& p, Captura& c, int tipo) {
  const bool intra = tipo == kI;
  const uint32_t obj = p.obj;
  if (p.desincronizado && !intra) {
    // Until the next I frame: in the game's videos the game decodes, and in the others the last picture is
    // repeated (FFmpeg does not have the references of this frame).
    if (p.juego_fiable) {
      ++p.rechazados;
      return false;
    }
    return Rechazar(ctx, base, p, tipo, "esperando al siguiente fotograma I");
  }
  if (Leer32(base, obj + 15424) != 6) {
    p.preparada = false;
    return Rechazar(ctx, base, p, tipo, "perfil distinto");
  }
  if (c.quedan || Leer32(base, Leer32(base, obj + 76) + 24) != 0) {
    CompletarFotograma(ctx, base, obj);
  }
  if (c.quedan || c.error || c.datos.empty()) {
    return Rechazar(ctx, base, p, tipo, "fotograma comprimido incompleto");
  }
  const int64_t antes = AhoraUs();
  Fotograma f;
  if (!p.dec.Descodificar(c.datos.data(), c.datos.size(), intra, f)) {
    return Rechazar(ctx, base, p, tipo, "FFmpeg no lo descodifica");
  }
  Planos g;
  if (f.ancho != p.dec.ancho() || f.alto != p.dec.alto() || !PlanosDelJuego(base, obj, f.ancho, g)) {
    p.preparada = false;
    return Rechazar(ctx, base, p, tipo, "los planos del juego no cuadran con el video");
  }
  CopiarPlanos(f, g);
  if (!p.juego_fiable) {
    GuardarUltima(p, f);
  }
  DejarContexto(base, obj, tipo);
  ctx.r3.u64 = 0;
  const int64_t despues = AhoraUs();
  p.desincronizado = false;
  ++p.fotogramas;
  ++p.nativos;
  ++p.n_nativo;
  g_fotogramas_nativos.fetch_add(1, std::memory_order_relaxed);
  p.us_nativo += despues - antes;
  Resumen(p, despues);
  return true;
}

// After the game decodes: FFmpeg decodes the same bytes and the planes are compared.
void Sombra(uint8_t* base, Pelicula& p, const Captura& c, int tipo) {
  const bool intra = tipo == kI;
  const uint64_t n = p.fotogramas - 1;
  if (c.quedan || c.error || c.datos.empty()) {
    if (p.avisos++ < 10) {
      REXLOG_WARN("[video] sombra #{}: fotograma comprimido incompleto ({} bytes, quedan {}, error {})", n,
                  c.datos.size(), c.quedan, c.error);
    }
    p.desincronizado = true;
    return;
  }
  if (p.desincronizado && !intra) {
    return;
  }
  const int64_t antes = AhoraUs();
  Fotograma f;
  if (!p.dec.Descodificar(c.datos.data(), c.datos.size(), intra, f)) {
    p.desincronizado = true;
    return;
  }
  const int64_t despues = AhoraUs();
  p.desincronizado = false;
  ++p.n_sombra;
  Planos g;
  if (!PlanosDelJuego(base, p.obj, f.ancho, g)) {
    return;
  }
  const int ancho_c = (f.ancho + 1) / 2;
  const int alto_c = (f.alto + 1) / 2;
  const Diferencia dy = CompararPlano(g.y, g.paso_y, f.planos[0], f.pasos[0], f.ancho, f.alto);
  const Diferencia du = CompararPlano(g.u, g.paso_c, f.planos[1], f.pasos[1], ancho_c, alto_c);
  const Diferencia dv = CompararPlano(g.v, g.paso_c, f.planos[2], f.pasos[2], ancho_c, alto_c);
  p.peor_max_y = std::max(p.peor_max_y, dy.max);
  p.peor_media_y = std::max(p.peor_media_y, dy.media);
  const bool raro = dy.max > 8 || du.max > 8 || dv.max > 8;
  if (n < 4 || n % 90 == 0 || (raro && p.avisos++ < 20)) {
    REXLOG_INFO("[video] sombra #{} {} {} bytes en {} secciones: Y max {} media {:.3f} >3 {} | U max {} media "
                "{:.3f} | V max {} media {:.3f} | FFmpeg {:.2f} ms",
                n, NombreTipo(tipo), c.datos.size(), c.secciones, dy.max, dy.media, dy.malos, du.max, du.media,
                dv.max, dv.media, double(despues - antes) / 1000.0);
  }
  // B frames: FFmpeg also against the other set of planes, to know in which one the game leaves each type.
  if (REXCVAR_GET(nfsmw_video_wmv3_b_diag) && n < kFotogramasDiagB) {
    Planos g2;
    if (PlanosDelJuego(base, p.obj, f.ancho, g2, 3684)) {
      const Diferencia d2 = CompararPlano(g2.y, g2.paso_y, f.planos[0], f.pasos[0], f.ancho, f.alto);
      REXLOG_INFO("[video] B sombra #{} {}: FFmpeg contra planos 3672 Y max {} media {:.3f} | contra planos 3684 "
                  "Y max {} media {:.3f}",
                  n, NombreTipo(tipo), dy.max, dy.media, d2.max, d2.media);
    }
  }
  if (n == 30) {
    const std::string prefijo = fmt::format("wmv3_{:08X}_030", p.obj);
    GuardarPgm(prefijo + "_juego.pgm", g.y, g.paso_y, f.ancho, f.alto, 1);
    GuardarPgm(prefijo + "_nativo.pgm", f.planos[0], f.pasos[0], f.ancho, f.alto, 1);
    GuardarPgm(prefijo + "_diferencia.pgm", g.y, g.paso_y, f.ancho, f.alto, 16, f.planos[0], f.pasos[0]);
  }
  // B frames: the luma and the U chroma of the game and of FFmpeg in a few frames, to see which one is right.
  if (REXCVAR_GET(nfsmw_video_wmv3_b_diag) && (n == 140 || n == 141 || n == 200 || n == 361 || n == 362 || n == 420)) {
    const std::string prefijo = fmt::format("wmv3b_{:08X}_{:03}_{}", p.obj, n, NombreTipo(tipo));
    GuardarPgm(prefijo + "_juego_y.pgm", g.y, g.paso_y, f.ancho, f.alto, 1);
    GuardarPgm(prefijo + "_juego_u.pgm", g.u, g.paso_c, ancho_c, alto_c, 1);
    GuardarPgm(prefijo + "_ffmpeg_y.pgm", f.planos[0], f.pasos[0], f.ancho, f.alto, 1);
    GuardarPgm(prefijo + "_ffmpeg_u.pgm", f.planos[1], f.pasos[1], ancho_c, alto_c, 1);
  }
}

void LlamarAlJuego(PPCContext& ctx, uint8_t* base, int tipo) {
  if (tipo == kI) {
    __imp__sub_828C35D8(ctx, base);
  } else if (tipo == kP) {
    __imp__sub_8278A518(ctx, base);
  } else {
    __imp__sub_828C58C8(ctx, base);
  }
}

void Descodificar(PPCContext& ctx, uint8_t* base, int tipo) {
  const bool nativo = REXCVAR_GET(nfsmw_video_wmv3_nativo);
  const bool sombra = REXCVAR_GET(nfsmw_video_wmv3_sombra);
  const bool diag = REXCVAR_GET(nfsmw_video_wmv3_datos_diag);
  const bool b_diag = REXCVAR_GET(nfsmw_video_wmv3_b_diag);
  if (!nativo && !sombra && !diag && !b_diag) {
    LlamarAlJuego(ctx, base, tipo);
    return;
  }
  const uint32_t obj = ctx.r3.u32;
  std::lock_guard<std::mutex> lock(g_peli_m);
  Pelicula& p = PeliculaDe(base, obj);
  Captura* c = t_captura && t_captura->ctx == obj ? t_captura : nullptr;
  if (nativo && p.preparada) {
    if (c && SustituirFotograma(ctx, base, p, *c, tipo)) {
      return;
    }
    if (!c && Rechazar(ctx, base, p, tipo, "llamada fuera de DecodeData")) {
      return;
    }
  }
  FotoContexto foto;
  const bool anotar = b_diag && p.fotogramas < kFotogramasDiagB && p.dec.ancho() > 0;
  if (anotar) {
    Fotografiar(base, obj, p.dec.ancho(), p.dec.alto(), foto);
  }
  const int64_t antes = AhoraUs();
  LlamarAlJuego(ctx, base, tipo);
  const int64_t despues = AhoraUs();
  ++p.fotogramas;
  ++p.n_juego;
  p.us_juego += despues - antes;
  if (anotar) {
    AnotarCambios(base, obj, p.dec.ancho(), p.dec.alto(), p.fotogramas - 1, tipo, ctx.r3.u32, foto);
  }
  if (diag && p.fotogramas <= 12 && c) {
    REXLOG_INFO("[video] fotograma #{} {}: {} bytes en {} secciones, quedan {}, error {}, resultado {:08X}, "
                "juego {:.2f} ms",
                p.fotogramas - 1, NombreTipo(tipo), c->datos.size(), c->secciones, c->quedan, c->error,
                ctx.r3.u32, double(despues - antes) / 1000.0);
  }
  if (sombra && !nativo && p.dec.ancho() > 0 && c && ctx.r3.u32 == 0) {
    Sombra(base, p, *c, tipo);
  }
  Resumen(p, despues);
}

}  // namespace
}  // namespace nfsmw::video_nativo

// Decoder data function (jumps to [[r3+4]] with r3 = [r3]).
REX_HOOK_RAW(sub_82749C10) {
  using namespace nfsmw::video_nativo;
  if (!t_captura || ctx.r3.u32 != Leer32(base, t_captura->ctx + 3300)) {
    __imp__sub_82749C10(ctx, base);
    return;
  }
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t estructura = ctx.r3.u32;
  const uint32_t desplazamiento = ctx.r4.u32;
  const uint32_t p_datos = ctx.r5.u32;
  const uint32_t pedidos = ctx.r6.u32;
  const uint32_t p_bytes = ctx.r7.u32;
  const uint32_t p_quedan = ctx.r8.u32;
  __imp__sub_82749C10(ctx, base);
  AnotarSeccion(base, lr, estructura, desplazamiento, pedidos, p_datos, p_bytes, p_quedan, ctx.r3.u32);
}

// DecodeData: collects the chunks of the compressed frame it requests.
REX_HOOK_RAW(sub_827312C0) {
  using namespace nfsmw::video_nativo;
  if (!Activo() || t_captura) {
    __imp__sub_827312C0(ctx, base);
    return;
  }
  std::unique_lock<std::mutex> lock(g_captura_m, std::try_to_lock);
  if (!lock.owns_lock()) {
    __imp__sub_827312C0(ctx, base);
    return;
  }
  g_captura.ctx = ctx.r3.u32;
  g_captura.estructura = 0;
  g_captura.secciones = 0;
  g_captura.quedan = false;
  g_captura.error = false;
  g_captura.datos.clear();
  t_captura = &g_captura;
  __imp__sub_827312C0(ctx, base);
  t_captura = nullptr;
}

// Decoding of an I frame ([ctx+15708]).
REX_HOOK_RAW(sub_828C35D8) {
  nfsmw::video_nativo::Descodificar(ctx, base, nfsmw::video_nativo::kI);
}

// Decoding of a P frame ([ctx+15712]).
REX_HOOK_RAW(sub_8278A518) {
  nfsmw::video_nativo::Descodificar(ctx, base, nfsmw::video_nativo::kP);
}

// Decoding of a B frame ([ctx+3016]).
REX_HOOK_RAW(sub_828C58C8) {
  nfsmw::video_nativo::Descodificar(ctx, base, nfsmw::video_nativo::kB);
}

// Preparation of a movie's context: if the context is reused, FFmpeg starts from scratch.
REX_HOOK_RAW(sub_8272FD30) {
  using namespace nfsmw::video_nativo;
  if (Activo()) {
    std::lock_guard<std::mutex> lock(g_peli_m);
    if (g_peli && g_peli->obj == ctx.r3.u32) {
      ResumenFinal(*g_peli);
      g_peli.reset();
    }
    if (REXCVAR_GET(nfsmw_video_wmv3_datos_diag)) {
      REXLOG_INFO("[video] sub_8272FD30: ctx={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X} r8={:08X} r9={:08X} "
                  "r10={:08X} f1={} f2={} lr={:08X}",
                  ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32,
                  ctx.f1.f64, ctx.f2.f64, static_cast<uint32_t>(ctx.lr));
    }
  }
  __imp__sub_8272FD30(ctx, base);
}
