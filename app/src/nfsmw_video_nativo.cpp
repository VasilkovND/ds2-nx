// nfsmw - cutscenes: the game's WMV3 decoding replaced by FFmpeg's
//
// On the Switch the XDK's recompiled WMV3 decoder runs at ~98 % of a core and produces ~22 frames per
// second: cutscenes drop from ~60 to ~20 FPS and the audio ends before the picture. The game's decoder
// interface (recompiled code):
//  - sub_8272FD30 prepares a movie's context; [ctx+3300] points to {application data, function}.
//  - sub_827312C0 (DecodeData, from sub_82734040) requests the compressed frame in chunks through
//    sub_82749C10, which jumps to that function with r4 = offset, r5 = &pointer to the data, r6 = bytes
//    requested, r7 = &bytes returned and r8 = &data remaining. It reads the picture header, swaps the
//    buffers and decodes according to the type ([ctx+280]): I with [ctx+15708] = sub_828C35D8 and P
//    with [ctx+15712] = sub_8278A518. Those two, with what they call, take almost all of the video
//    thread's time.
//  - The new picture goes into planes [ctx+3672] (Y), [ctx+3676] (U) and [ctx+3680] (V), with the
//    origin at +[ctx+216] and +[ctx+220] (32- and 16-pixel borders: strides of 1344 and 672 for
//    1280x720).
//  - Without postprocessing ([ctx+3844] = 0) both only end up writing context fields: I sets
//    [ctx+15516] = 0; P also sets [ctx+15488] = 1 and [ctx+15512] = ([ctx+14776] != 0 or
//    [ctx+15148] != -1).
//
// Cvars:
//  - nfsmw_video_wmv3_nativo: sub_828C35D8 and sub_8278A518 do not decode. FFmpeg decodes the same
//    bytes the game requested and its planes are copied into the game's buffers. If a frame cannot be
//    replaced the game decodes it, and the next native one waits for an I frame.
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

REX_EXTERN(__imp__sub_82749C10);
REX_EXTERN(__imp__sub_827312C0);
REX_EXTERN(__imp__sub_8278A518);
REX_EXTERN(__imp__sub_828C35D8);
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
         REXCVAR_GET(nfsmw_video_wmv3_datos_diag);
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

bool PlanosDelJuego(uint8_t* base, uint32_t obj, int ancho, Planos& p) {
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
  p.y = base + Leer32(base, obj + 3672) + off_y;
  p.u = base + Leer32(base, obj + 3676) + off_c;
  p.v = base + Leer32(base, obj + 3680) + off_c;
  return true;
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
  uint64_t fotogramas = 0;     // I and P frames that went through the hooks
  uint64_t nativos = 0;
  uint64_t rechazados = 0;
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
  REXLOG_INFO("[video] WMV3 fin de '{}' (contexto {:08X}): {} fotogramas, {} con FFmpeg, {} rechazados; sombra: "
              "peor Y max {} media {:.3f}",
              p.ruta, p.obj, p.fotogramas, p.nativos, p.rechazados, p.peor_max_y, p.peor_media_y);
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
  const bool config_ok = Leer32(base, obj + 3844) == 0 && Leer32(base, obj + 15424) == 6 &&
                         Leer32(base, obj + 15708) == kDescodificarI && Leer32(base, obj + 15712) == kDescodificarP;
  p.preparada = info_ok && config_ok && p.dec.Abrir(info);
  const auto& s = info.secuencia;
  REXLOG_INFO("[video] WMV3: contexto {:08X} -> '{}' {}x{} secuencia {:02X}{:02X}{:02X}{:02X}; configuracion {}; "
              "FFmpeg {}",
              obj, p.ruta, info.ancho, info.alto, s.size() > 0 ? s[0] : 0, s.size() > 1 ? s[1] : 0,
              s.size() > 2 ? s[2] : 0, s.size() > 3 ? s[3] : 0, config_ok ? "conocida" : "distinta",
              p.preparada ? "listo" : "no disponible");
  if (REXCVAR_GET(nfsmw_video_wmv3_datos_diag)) {
    REXLOG_INFO("[video] contexto {:08X}: +3300={:08X} +15708={:08X} +15712={:08X} +15424={} +3844={} +132={} "
                "+136={} +200={} +204={} +216={:X} +220={:X}",
                obj, Leer32(base, obj + 3300), Leer32(base, obj + 15708), Leer32(base, obj + 15712),
                Leer32(base, obj + 15424), Leer32(base, obj + 3844), Leer32(base, obj + 132),
                Leer32(base, obj + 136), Leer32(base, obj + 200), Leer32(base, obj + 204),
                Leer32(base, obj + 216), Leer32(base, obj + 220));
  }
  return p;
}

bool Rechazar(Pelicula& p, bool intra, const char* motivo) {
  ++p.rechazados;
  p.desincronizado = true;
  if (p.avisos++ < 10) {
    REXLOG_WARN("[video] WMV3 nativo: el fotograma {} ({}) de '{}' lo descodifica el juego: {}", p.fotogramas,
                intra ? "I" : "P", p.ruta, motivo);
  }
  return false;
}

// Replaces the decoding of one frame. true if r3 already holds the result.
bool SustituirFotograma(PPCContext& ctx, uint8_t* base, Pelicula& p, Captura& c, bool intra) {
  const uint32_t obj = p.obj;
  if (p.desincronizado && !intra) {
    ++p.rechazados;  // the game decodes until the next I frame
    return false;
  }
  if (Leer32(base, obj + 3844) != 0 || Leer32(base, obj + 15424) != 6) {
    p.preparada = false;
    return Rechazar(p, intra, "posproceso o perfil distinto");
  }
  if (c.quedan || Leer32(base, Leer32(base, obj + 76) + 24) != 0) {
    CompletarFotograma(ctx, base, obj);
  }
  if (c.quedan || c.error || c.datos.empty()) {
    return Rechazar(p, intra, "fotograma comprimido incompleto");
  }
  const int64_t antes = AhoraUs();
  Fotograma f;
  if (!p.dec.Descodificar(c.datos.data(), c.datos.size(), intra, f)) {
    return Rechazar(p, intra, "FFmpeg no lo descodifica");
  }
  Planos g;
  if (f.ancho != p.dec.ancho() || f.alto != p.dec.alto() || !PlanosDelJuego(base, obj, f.ancho, g)) {
    p.preparada = false;
    return Rechazar(p, intra, "los planos del juego no cuadran con el video");
  }
  CopiarPlanos(f, g);
  // Context as sub_828C35D8 and sub_8278A518 leave it without postprocessing.
  if (!intra) {
    const bool marca = Leer32(base, obj + 14776) != 0 || Leer32(base, obj + 15148) != 0xFFFFFFFFu;
    Escribir32(base, obj + 15512, marca ? 1 : 0);
    Escribir32(base, obj + 15488, 1);
  }
  Escribir32(base, obj + 15516, 0);
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
void Sombra(uint8_t* base, Pelicula& p, const Captura& c, bool intra) {
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
                n, intra ? "I" : "P", c.datos.size(), c.secciones, dy.max, dy.media, dy.malos, du.max, du.media,
                dv.max, dv.media, double(despues - antes) / 1000.0);
  }
  if (n == 30) {
    const std::string prefijo = fmt::format("wmv3_{:08X}_030", p.obj);
    GuardarPgm(prefijo + "_juego.pgm", g.y, g.paso_y, f.ancho, f.alto, 1);
    GuardarPgm(prefijo + "_nativo.pgm", f.planos[0], f.pasos[0], f.ancho, f.alto, 1);
    GuardarPgm(prefijo + "_diferencia.pgm", g.y, g.paso_y, f.ancho, f.alto, 16, f.planos[0], f.pasos[0]);
  }
}

void Descodificar(PPCContext& ctx, uint8_t* base, bool intra) {
  const bool nativo = REXCVAR_GET(nfsmw_video_wmv3_nativo);
  const bool sombra = REXCVAR_GET(nfsmw_video_wmv3_sombra);
  const bool diag = REXCVAR_GET(nfsmw_video_wmv3_datos_diag);
  if (!nativo && !sombra && !diag) {
    if (intra) {
      __imp__sub_828C35D8(ctx, base);
    } else {
      __imp__sub_8278A518(ctx, base);
    }
    return;
  }
  const uint32_t obj = ctx.r3.u32;
  std::lock_guard<std::mutex> lock(g_peli_m);
  Pelicula& p = PeliculaDe(base, obj);
  Captura* c = t_captura && t_captura->ctx == obj ? t_captura : nullptr;
  if (nativo && p.preparada) {
    if (c && SustituirFotograma(ctx, base, p, *c, intra)) {
      return;
    }
    if (!c) {
      Rechazar(p, intra, "llamada fuera de DecodeData");
    }
  }
  const int64_t antes = AhoraUs();
  if (intra) {
    __imp__sub_828C35D8(ctx, base);
  } else {
    __imp__sub_8278A518(ctx, base);
  }
  const int64_t despues = AhoraUs();
  ++p.fotogramas;
  ++p.n_juego;
  p.us_juego += despues - antes;
  if (diag && p.fotogramas <= 12 && c) {
    REXLOG_INFO("[video] fotograma #{} {}: {} bytes en {} secciones, quedan {}, error {}, resultado {:08X}, "
                "juego {:.2f} ms",
                p.fotogramas - 1, intra ? "I" : "P", c->datos.size(), c->secciones, c->quedan, c->error,
                ctx.r3.u32, double(despues - antes) / 1000.0);
  }
  if (sombra && !nativo && p.preparada && c && ctx.r3.u32 == 0) {
    Sombra(base, p, *c, intra);
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
  nfsmw::video_nativo::Descodificar(ctx, base, true);
}

// Decoding of a P frame ([ctx+15712]).
REX_HOOK_RAW(sub_8278A518) {
  nfsmw::video_nativo::Descodificar(ctx, base, false);
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
