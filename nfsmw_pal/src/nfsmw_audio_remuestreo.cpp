// nfsmw - linear resamplers of the sound engine in native code
//
// On the console, the remaining robotic audio shows up in hard crashes, with the audio server thread at
// 70-94 % of a core. On PC the sound engine's two linear resamplers take 16.6 % of that thread:
// sub_82619820 (9.9 %) and sub_826031C0 (6.7 %). The recompiled code handles each sample with volatile reads
// and writes of guest memory (with byte swapping) and converts the integers to floating point through the
// stack; sub_82619820 also stores the position and the fraction to memory on every sample.
//
// Here they do the same floating-point operations in the same order as the recompiled code (subtraction,
// product and fma in double precision, rounded to single), so the result is bit-identical, without the extra
// accesses. Each sample i, with the position in 16.16 (integer part in pos, fraction in frac < 65536), source
// origen and step paso:
//   s0 = origen[pos], s1 = origen[pos + 1]
//   salida[i] = s0 + (s1 - s0) * (frac * K)       K = the float at 0x820AFD18
//   acc = frac + paso; pos += acc >> 16; frac = acc & 0xFFFF
//
// nfsmw_audio_remuestreo_nativo: 0 = recompiled code; 1 = native; 2 = validate: computes the native result
// separately, runs the recompiled code and compares each sample and the final state, bit by bit (the game uses
// the recompiled result). Every 10 s it logs how many calls and samples it has done and how many differences it
// saw.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_informe_diferido.h"  // deferred reports

#include "nfsmw_audio_nativo.h"

// Native by default. On PC, in the alley test run, it gave 0 differences against the recompiled code over
// 130 million samples and runs 1.7 and 3.7 times faster.
REXCVAR_DEFINE_INT32(nfsmw_audio_remuestreo_nativo, 1, "NFSMW",
                     "Remuestreadores lineales del motor de sonido (sub_826031C0 y sub_82619820): 0 = codigo "
                     "recompilado, 1 = nativo (mismo resultado bit a bit; por defecto), 2 = validar el nativo contra "
                     "el recompilado");

REX_EXTERN(__imp__sub_826031C0);
REX_EXTERN(__imp__sub_82619820);

namespace nfsmw::audio_remuestreo {
namespace {

using namespace nfsmw::audio_nativo;

constexpr uint32_t kDirConstante = 0x820AFD18;  // lis r11,-32245; lfs f0,-744(r11)

// One sample, with the operations of the recompiled code (fsubs, fcfid, frsp, fmuls and fmadds from XenonRecomp).
inline float Interpolar(float s0, float s1, uint32_t frac, float k) {
  const double resta = double(float(double(s1) - double(s0)));
  const double t = double(float(double(float(double(int64_t(frac)))) * double(k)));
  return float(double(float(std::fma(resta, t, double(s0)))));
}

struct Estado {
  uint32_t pos;
  uint32_t frac;
};

// The common loop. escribir(i, muestra) stores each sample as soon as it is computed, in the same order as the
// recompiled code (this matters if the destination overlaps the source).
template <typename Escribir>
Estado Bucle(uint8_t* base, int32_t n, uint32_t origen, uint32_t pos, uint32_t frac, uint32_t paso, float k,
             Escribir escribir) {
  for (int32_t i = 0; i < n; ++i) {
    const uint32_t dir = (pos << 2) + origen;
    const float s0 = LeerFloat(base, dir);
    const float s1 = LeerFloat(base, dir + 4);
    escribir(i, Interpolar(s0, s1, frac, k));
    const uint32_t acc = frac + paso;
    pos += (acc >> 16) & 0xFFFF;
    frac = acc & 0xFFFF;
  }
  return {pos, frac};
}

struct Contador {
  std::atomic<uint64_t> llamadas{0};
  std::atomic<uint64_t> muestras{0};
  std::atomic<uint64_t> diferencias{0};
  std::atomic<uint64_t> pisados{0};  // sub_82619820 with the state inside the destination: use the recompiled code
};
Contador g_826031C0;
Contador g_82619820;
std::atomic<int64_t> g_ultimo_informe_ms{0};
std::atomic<bool> g_diferencia_anotada{false};
thread_local std::vector<float> t_local;

int64_t AhoraMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void Contar(Contador& c, int32_t n) {
  c.llamadas.fetch_add(1, std::memory_order_relaxed);
  c.muestras.fetch_add(uint64_t(std::max(n, 0)), std::memory_order_relaxed);
  const int64_t ahora = AhoraMs();
  int64_t ultimo = g_ultimo_informe_ms.load(std::memory_order_relaxed);
  if (ultimo == 0) {
    g_ultimo_informe_ms.compare_exchange_strong(ultimo, ahora, std::memory_order_relaxed);
    return;
  }
  if (ahora - ultimo < 10000 || !g_ultimo_informe_ms.compare_exchange_strong(ultimo, ahora, std::memory_order_relaxed)) {
    return;
  }
  NFSMW_INFORME_DIFERIDO("[audio] remuestreo lineal (modo {}): sub_826031C0 {} llamadas y {} muestras, sub_82619820 {} llamadas y {} "
              "muestras ({} con el estado dentro del destino), diferencias con el recompilado {} y {}",
              REXCVAR_GET(nfsmw_audio_remuestreo_nativo), g_826031C0.llamadas.exchange(0),
              g_826031C0.muestras.exchange(0), g_82619820.llamadas.exchange(0), g_82619820.muestras.exchange(0),
              g_82619820.pisados.exchange(0), g_826031C0.diferencias.exchange(0), g_82619820.diferencias.exchange(0));
}

void AnotarDiferencia(const char* funcion, uint32_t destino, int32_t indice, uint32_t nativo, uint32_t recompilado) {
  if (!g_diferencia_anotada.exchange(true, std::memory_order_relaxed)) {
    REXLOG_WARN("[audio] remuestreo lineal: primera diferencia en {}: destino 0x{:08X}, muestra {}, nativo 0x{:08X}, "
                "recompilado 0x{:08X}",
                funcion, destino, indice, nativo, recompilado);
  }
}

// Compares the destination and state left by the recompiled code with the separately computed result.
uint64_t Comparar(const char* funcion, uint8_t* base, uint32_t destino, int32_t n, const std::vector<float>& local,
                  uint32_t dir_pos, uint32_t dir_frac, Estado estado) {
  uint64_t diferencias = 0;
  for (int32_t i = 0; i < n; ++i) {
    const uint32_t recompilado = Leer32(base, destino + uint32_t(i) * 4);
    const uint32_t nativo = Bits(local[size_t(i)]);
    if (recompilado != nativo) {
      AnotarDiferencia(funcion, destino, i, nativo, recompilado);
      ++diferencias;
    }
  }
  if (Leer32(base, dir_pos) != estado.pos || Leer32(base, dir_frac) != (estado.frac << 16)) {
    AnotarDiferencia(funcion, destino, -1, estado.pos, Leer32(base, dir_pos));
    ++diferencias;
  }
  return diferencias;
}

}  // namespace

// sub_826031C0: r4 = samples, r5 = source, r6 = destination, r7 = &pos, r8 = &frac (in the upper half), r9 =
// integer part of the step, r10 = fraction of the step (in its upper half). Reads the state on entry and
// writes it on exit.
void Remuestreo826031C0(PPCContext& ctx, uint8_t* base) {
  const int32_t modo = REXCVAR_GET(nfsmw_audio_remuestreo_nativo);
  if (modo != 1 && modo != 2) {
    __imp__sub_826031C0(ctx, base);
    return;
  }
  const int32_t n = ctx.r4.s32;
  const uint32_t origen = ctx.r5.u32;
  const uint32_t destino = ctx.r6.u32;
  const uint32_t dir_pos = ctx.r7.u32;
  const uint32_t dir_frac = ctx.r8.u32;
  const uint32_t paso = (ctx.r9.u32 << 16) | ((ctx.r10.u32 >> 16) & 0xFFFF);
  ctx.fpscr.disableFlushMode();
  const float k = LeerFloat(base, kDirConstante);
  const uint32_t pos = Leer32(base, dir_pos);
  const uint32_t frac = Leer16(base, dir_frac);
  Contar(g_826031C0, n);
  if (modo == 2) {
    std::vector<float>& local = t_local;
    local.resize(size_t(std::max(n, 0)));
    const Estado estado =
        Bucle(base, n, origen, pos, frac, paso, k, [&](int32_t i, float v) { local[size_t(i)] = v; });
    __imp__sub_826031C0(ctx, base);
    g_826031C0.diferencias.fetch_add(Comparar("sub_826031C0", base, destino, n, local, dir_pos, dir_frac, estado),
                                     std::memory_order_relaxed);
    return;
  }
  const Estado estado = Bucle(base, n, origen, pos, frac, paso, k, [&](int32_t i, float v) {
    EscribirFloat(base, destino + uint32_t(i) * 4, v);
  });
  Escribir32(base, dir_pos, estado.pos);
  Escribir32(base, dir_frac, estado.frac << 16);
  // Volatile registers as the recompiled code leaves them (just in case): r3 = pos, r9 = frac << 16,
  // r10 = paso, f0 = K.
  ctx.r3.u64 = estado.pos;
  ctx.r9.u64 = uint64_t(estado.frac) << 16;
  ctx.r10.u64 = paso;
  ctx.f0.f64 = double(k);
}

// sub_82619820: r3 = samples, r4 = source, r5 = destination, r6 = &pos, r7 = &frac (in the upper half), r8 =
// integer part of the step, r9 = fraction of the step (in its upper half). The recompiled code reads and writes
// the state in memory on every sample; if the state lies inside the destination, that changes the result, so
// that case is left to the recompiled code.
void Remuestreo82619820(PPCContext& ctx, uint8_t* base) {
  const int32_t modo = REXCVAR_GET(nfsmw_audio_remuestreo_nativo);
  if (modo != 1 && modo != 2) {
    __imp__sub_82619820(ctx, base);
    return;
  }
  const int32_t n = ctx.r3.s32;
  const uint32_t origen = ctx.r4.u32;
  const uint32_t destino = ctx.r5.u32;
  const uint32_t dir_pos = ctx.r6.u32;
  const uint32_t dir_frac = ctx.r7.u32;
  const uint32_t paso = (ctx.r8.u32 << 16) | ((ctx.r9.u32 >> 16) & 0xFFFF);
  const uint64_t bytes = uint64_t(std::max(n, 0)) * 4;
  if (Solapan(dir_pos, 4, destino, bytes) || Solapan(dir_frac, 4, destino, bytes) || Solapan(dir_pos, 4, dir_frac, 4)) {
    g_82619820.pisados.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_82619820(ctx, base);
    return;
  }
  ctx.fpscr.disableFlushMode();
  const float k = LeerFloat(base, kDirConstante);
  const uint32_t frac = Leer16(base, dir_frac);
  const uint32_t pos = Leer32(base, dir_pos);
  Contar(g_82619820, n);
  if (modo == 2) {
    std::vector<float>& local = t_local;
    local.resize(size_t(std::max(n, 0)));
    const Estado estado =
        Bucle(base, n, origen, pos, frac, paso, k, [&](int32_t i, float v) { local[size_t(i)] = v; });
    __imp__sub_82619820(ctx, base);
    g_82619820.diferencias.fetch_add(Comparar("sub_82619820", base, destino, n, local, dir_pos, dir_frac, estado),
                                     std::memory_order_relaxed);
    return;
  }
  const Estado estado = Bucle(base, n, origen, pos, frac, paso, k, [&](int32_t i, float v) {
    EscribirFloat(base, destino + uint32_t(i) * 4, v);
  });
  Escribir32(base, dir_pos, estado.pos);
  Escribir32(base, dir_frac, estado.frac << 16);
  // Volatile registers as the recompiled code leaves them: r3 = frac << 16, r4 = frac, r9 = 0, r10 = paso, f0 = K.
  ctx.r3.u64 = uint64_t(estado.frac) << 16;
  ctx.r4.u64 = estado.frac;
  ctx.r9.u64 = 0;
  ctx.r10.u64 = paso;
  ctx.f0.f64 = double(k);
}

}  // namespace nfsmw::audio_remuestreo
