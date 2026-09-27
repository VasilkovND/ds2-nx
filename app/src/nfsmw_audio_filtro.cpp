// nfsmw - recursive filter of the sound engine in native code (sub_825CD088)
//
// After the resamplers, it is the heaviest leaf function of the audio server thread on PC: 7.9 % of its
// CPU, 7.4 us per call, about 4 calls per packet. It is a two-stage filter over a delay line of the
// object at r3 (+4 a, +8 b, +12 c, +16 buffer, +24 write position), with the input in r8, the output in
// r9, n = r4 and three delay taps in r5, r6 and r7. For each sample j:
//   t = x[j] - buf[r5 + j] * a
//   y = t - buf[r6 + j] * b                buf[pos + j] = y
//   z = buf[r7 + j] * c + buf[r6 + j]       (read after storing y)
//   salida[j] = z * g        if r10 == 1   (g = the float at 0x820AFD68)
//   salida[j] += z * g       otherwise
// Returns r3 = 1.
//
// The floating-point operations are those of the recompiled code (fnmsubs, fmadds and fmuls in double
// precision, rounded to single) in the same order, and so are the reads and writes of each sample, so the
// result is bit-identical even if the taps land on the write position. The object's fields are read once
// if nothing that is written lands on them; if something does, they are reread on every sample, as the
// recompiled code does.
//
// nfsmw_audio_filtro_nativo: 0 = recompiled; 1 = native; 2 = validate: runs the recompiled code, saves
// what it wrote, undoes it, runs the native code, compares byte by byte and keeps the recompiled result.
// A summary is logged every 10 s.

#include <algorithm>
#include <atomic>
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

// Native by default. On PC, in the alley test run, it gave 0 differences against the recompiled code
// over 29 million samples and runs 1.6 times faster.
REXCVAR_DEFINE_INT32(nfsmw_audio_filtro_nativo, 1, "NFSMW",
                     "Filtro recursivo del motor de sonido (sub_825CD088): 0 = codigo recompilado, 1 = nativo (mismo "
                     "resultado bit a bit; por defecto), 2 = validar el nativo contra el recompilado");

REX_EXTERN(__imp__sub_825CD088);

namespace nfsmw::audio_filtro {
namespace {

using namespace nfsmw::audio_nativo;

constexpr uint32_t kDirGanancia = 0x820AFD68;  // lis r11,-32245; lfs f0,-664(r11)
constexpr uint32_t kCampos = 4;                 // bytes +4 to +27 of the object are read
constexpr uint32_t kTamCampos = 24;

struct Argumentos {
  uint32_t obj;
  int32_t n;
  uint32_t toma5;
  uint32_t toma6;
  uint32_t toma7;
  uint32_t entrada;
  uint32_t salida;
  int32_t modo;
};

Argumentos LeerArgumentos(const PPCContext& ctx) {
  return {ctx.r3.u32, ctx.r4.s32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.s32};
}

inline uint32_t DirToma(uint32_t buf, uint32_t toma, int32_t j) {
  return ((toma + uint32_t(j)) << 2) + buf;
}

// Output part of each sample, the same in both paths.
inline void Salida(uint8_t* base, const Argumentos& a, int32_t j, float z, float g) {
  const uint32_t dir = a.salida + uint32_t(j) * 4;
  if (a.modo == 1) {
    EscribirFloat(base, dir, float(double(float(double(z) * double(g)))));
  } else {
    const float anterior = LeerFloat(base, dir);
    EscribirFloat(base, dir, float(double(float(std::fma(double(z), double(g), double(anterior))))));
  }
}

// The filter in native code. Returns whether it could read the object's fields only once.
bool Nativo(uint8_t* base, const Argumentos& a) {
  const float g = LeerFloat(base, kDirGanancia);
  if (a.n <= 0) {
    return true;
  }
  const uint64_t bytes = uint64_t(a.n) * 4;
  const uint32_t buf0 = Leer32(base, a.obj + 16);
  const uint32_t pos0 = Leer32(base, a.obj + 24);
  const bool fijos = !Solapan((pos0 << 2) + buf0, bytes, a.obj + kCampos, kTamCampos) &&
                     !Solapan(a.salida, bytes, a.obj + kCampos, kTamCampos);
  if (fijos) {
    const float ca = LeerFloat(base, a.obj + 4);
    const float cb = LeerFloat(base, a.obj + 8);
    const float cc = LeerFloat(base, a.obj + 12);
    for (int32_t j = 0; j < a.n; ++j) {
      const float x = LeerFloat(base, a.entrada + uint32_t(j) * 4);
      const float p1 = LeerFloat(base, DirToma(buf0, a.toma5, j));
      const float t = float(-std::fma(double(p1), double(ca), -double(x)));
      const float p2 = LeerFloat(base, DirToma(buf0, a.toma6, j));
      const float y = float(-std::fma(double(p2), double(cb), -double(t)));
      EscribirFloat(base, DirToma(buf0, pos0, j), y);
      const float q = LeerFloat(base, DirToma(buf0, a.toma7, j));
      const float p = LeerFloat(base, DirToma(buf0, a.toma6, j));
      const float z = float(std::fma(double(q), double(cc), double(p)));
      Salida(base, a, j, z, g);
    }
    return true;
  }
  // Something that is written lands on the object's fields: they are reread in the same order as the
  // recompiled code.
  for (int32_t j = 0; j < a.n; ++j) {
    const uint32_t buf = Leer32(base, a.obj + 16);
    const float ca = LeerFloat(base, a.obj + 4);
    const float x = LeerFloat(base, a.entrada + uint32_t(j) * 4);
    const uint32_t pos = Leer32(base, a.obj + 24);
    const float cb = LeerFloat(base, a.obj + 8);
    const float p1 = LeerFloat(base, DirToma(buf, a.toma5, j));
    const float t = float(-std::fma(double(p1), double(ca), -double(x)));
    const float p2 = LeerFloat(base, DirToma(buf, a.toma6, j));
    const float y = float(-std::fma(double(p2), double(cb), -double(t)));
    EscribirFloat(base, DirToma(buf, pos, j), y);
    const uint32_t buf2 = Leer32(base, a.obj + 16);
    const float cc = LeerFloat(base, a.obj + 12);
    const float q = LeerFloat(base, DirToma(buf2, a.toma7, j));
    const float p = LeerFloat(base, DirToma(buf2, a.toma6, j));
    const float z = float(std::fma(double(q), double(cc), double(p)));
    Salida(base, a, j, z, g);
  }
  return false;
}

std::atomic<uint64_t> g_llamadas{0};
std::atomic<uint64_t> g_muestras{0};
std::atomic<uint64_t> g_releidos{0};
std::atomic<uint64_t> g_diferencias{0};
std::atomic<int64_t> g_ultimo_informe_ms{0};
std::atomic<bool> g_diferencia_anotada{false};

int64_t AhoraMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void Informar() {
  const int64_t ahora = AhoraMs();
  int64_t ultimo = g_ultimo_informe_ms.load(std::memory_order_relaxed);
  if (ultimo == 0) {
    g_ultimo_informe_ms.compare_exchange_strong(ultimo, ahora, std::memory_order_relaxed);
    return;
  }
  if (ahora - ultimo < 10000 || !g_ultimo_informe_ms.compare_exchange_strong(ultimo, ahora, std::memory_order_relaxed)) {
    return;
  }
  NFSMW_INFORME_DIFERIDO("[audio] filtro nativo (modo {}): sub_825CD088 {} llamadas y {} muestras, {} releyendo el objeto, "
              "diferencias con el recompilado {}",
              REXCVAR_GET(nfsmw_audio_filtro_nativo), g_llamadas.exchange(0), g_muestras.exchange(0),
              g_releidos.exchange(0), g_diferencias.exchange(0));
}

struct Rango {
  uint32_t dir;
  uint32_t bytes;
  std::vector<uint8_t> datos;
};

void Guardar(uint8_t* base, Rango& r) {
  r.datos.resize(r.bytes);
  std::memcpy(r.datos.data(), Dir(base, r.dir), r.bytes);
}

void Poner(uint8_t* base, const Rango& r) {
  std::memcpy(Dir(base, r.dir), r.datos.data(), r.bytes);
}

// Mode 2. What the filter writes: the delay line from the write position, the output and, in case they
// overlap, the object's fields. They are compared as 4-byte words.
void Validar(PPCContext& ctx, uint8_t* base) {
  const Argumentos a = LeerArgumentos(ctx);
  if (a.n <= 0) {
    __imp__sub_825CD088(ctx, base);
    return;
  }
  const uint32_t bytes = uint32_t(a.n) * 4;
  const uint32_t buf = Leer32(base, a.obj + 16);
  const uint32_t pos = Leer32(base, a.obj + 24);
  thread_local std::vector<Rango> antes(3);
  thread_local std::vector<Rango> recompilado(3);
  const uint32_t dirs[3] = {(pos << 2) + buf, a.salida, a.obj + kCampos};
  const uint32_t tams[3] = {bytes, bytes, kTamCampos};
  for (size_t i = 0; i < 3; ++i) {
    antes[i].dir = recompilado[i].dir = dirs[i];
    antes[i].bytes = recompilado[i].bytes = tams[i];
    Guardar(base, antes[i]);
  }
  __imp__sub_825CD088(ctx, base);
  for (size_t i = 0; i < 3; ++i) {
    Guardar(base, recompilado[i]);
  }
  for (size_t i = 0; i < 3; ++i) {
    Poner(base, antes[i]);
  }
  // The arguments were read before calling the recompiled code, which changes the volatile registers.
  Nativo(base, a);
  uint64_t diferencias = 0;
  for (size_t i = 0; i < 3; ++i) {
    const Rango& r = recompilado[i];
    for (uint32_t off = 0; off + 4 <= r.bytes; off += 4) {
      if (std::memcmp(Dir(base, r.dir + off), r.datos.data() + off, 4) != 0) {
        if (!g_diferencia_anotada.exchange(true, std::memory_order_relaxed)) {
          REXLOG_WARN("[audio] filtro nativo: primera diferencia en 0x{:08X} (rango {}), nativo 0x{:08X}, recompilado "
                      "0x{:08X}",
                      r.dir + off, i, Leer32(base, r.dir + off),
                      (uint32_t(r.datos[off]) << 24) | (uint32_t(r.datos[off + 1]) << 16) |
                          (uint32_t(r.datos[off + 2]) << 8) | uint32_t(r.datos[off + 3]));
        }
        ++diferencias;
      }
    }
  }
  // The game continues with the recompiled code's result.
  for (size_t i = 0; i < 3; ++i) {
    Poner(base, recompilado[i]);
  }
  g_diferencias.fetch_add(diferencias, std::memory_order_relaxed);
}

}  // namespace

void Filtro825CD088(PPCContext& ctx, uint8_t* base) {
  const int32_t modo = REXCVAR_GET(nfsmw_audio_filtro_nativo);
  if (modo != 1 && modo != 2) {
    __imp__sub_825CD088(ctx, base);
    return;
  }
  const int32_t n = ctx.r4.s32;
  g_llamadas.fetch_add(1, std::memory_order_relaxed);
  g_muestras.fetch_add(uint64_t(std::max(n, 0)), std::memory_order_relaxed);
  Informar();
  if (modo == 2) {
    Validar(ctx, base);
    return;
  }
  ctx.fpscr.disableFlushMode();
  if (!Nativo(base, LeerArgumentos(ctx))) {
    g_releidos.fetch_add(1, std::memory_order_relaxed);
  }
  ctx.r3.u64 = 1;
}

}  // namespace nfsmw::audio_filtro
