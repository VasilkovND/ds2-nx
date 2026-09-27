// nfsmw - the dump of the changed registers of the game's D3D, in native code.
//
// WHAT sub_825A2AA0 IS
//   An Xbox 360 D3D function that the game calls from FlushState on every draw (~2,400 per
//   frame): it walks a 64-bit mask of registers marked as changed and, for each run of
//   consecutive bits, writes a type-0 PM4 packet into the command ring (header + the values
//   of those registers). It produces a good share of the ~83,000 packets per frame measured
//   on the console.
//
// WHY NATIVE
//   Recompiled, each copied word is a byte-reversed load, a byte-reversed store (both
//   through volatile pointers) and the PowerPC registers reread and rewritten in the
//   context: ~15-20 instructions per word. A reversed load followed by a reversed store is
//   a plain copy, so here it is a memcpy.
//
// WHY IT IS BIT-IDENTICAL
//   Only integers are involved. The original PowerPC (read instruction by instruction):
//
//     r29 = r6 - 4; r4 = [r3+0]
//     loop:   z = cntlzd(mask); r9 = [r3+4]; r28 = z + r5; r29 += z*4; mask <<= z
//             n = cntlzd(~mask)                                 // consecutive 1 bits, 1..64
//             if (r4 + n*4 < r9, unsigned):                     // fits in the ring
//                 [r4+4] = ((n-1) << 16) | r28; r4 += 4          // header
//                 n times: r29 += 4; r4 += 4; [r4] = [r29]       // word copy
//                 mask <<= n (bitwise: 64 shifts leave 0)
//             else: r4 = sub_825A29E8(r3, r4, r28, r29, n, 1); r29 += n*4; mask <<= n
//             r5 = r28 + n
//             repeat while mask != 0
//     [r3+0] = r4
//
//   Note: the loop runs once even if the mask arrives as 0, and in that case it would copy
//   ~2^32 words. The callers never do that, but to be exact in every case, with mask 0 the
//   original is called. The ring-full path is also left to the game (sub_825A29E8), with the
//   same registers and the same stack frame.
//
// HOW TO CHECK IT
//   The "[d3d_registros]" log line (every 10 s): calls, copied words and how many times the
//   game's path was taken. If something looked wrong, nfsmw_d3d_registros_nativo = false
//   restores the original.
//
// At the end of the file: FlushState with a single marker packet (phase 2 of the Direct3D-level renderer,
// cvar nfsmw_d3d_marcador) and the counter for the ring-full path (sub_825A29E8).

#include "nfsmw_nativo_ganchos.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_informe_diferido.h"  // deferred reports
#include "nfsmw_esperas_tiron.h"     // TreeCull in the [tiron] juego log line
#include <rex/platform.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <type_traits>  // std::conditional_t in SetTextureNativo

REXCVAR_DEFINE_BOOL(nfsmw_d3d_registros_nativo, true, "NFSMW",
                    "Volcado de registros cambiados del D3D del juego (sub_825A2AA0) en nativo: una copia "
                    "en vez de ~15-20 instrucciones por palabra. Resultado identico bit a bit")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

// Same as REX_PHYS_HOST_OFFSET in nfsmw_pch.h: on Win32 physical addresses are 0x1000 higher.
inline uint32_t Desplazamiento(uint32_t direccion) {
#if REX_PLATFORM_WIN32
  return direccion >= 0xE0000000u ? 0x1000u : 0u;
#else
  (void)direccion;
  return 0u;
#endif
}
inline uint8_t* Puntero(uint8_t* base, uint32_t direccion) {
  return base + direccion + Desplazamiento(direccion);
}
inline uint32_t Leer32(uint8_t* base, uint32_t direccion) {
  uint32_t v;
  std::memcpy(&v, Puntero(base, direccion), 4);
  return __builtin_bswap32(v);
}
inline void Escribir32(uint8_t* base, uint32_t direccion, uint32_t valor) {
  valor = __builtin_bswap32(valor);
  std::memcpy(Puntero(base, direccion), &valor, 4);
}

// Copies n 32-bit words as they are (no swapping: swapping on read and on write cancels out). If the
// run crossed the 0xE0000000 boundary (where the offset changes on Win32), word by word.
inline void CopiarPalabras(uint8_t* base, uint32_t destino, uint32_t origen, uint32_t n) {
  const uint32_t bytes = n * 4;
  const bool cruza = Desplazamiento(destino) != Desplazamiento(destino + bytes - 1) ||
                     Desplazamiento(origen) != Desplazamiento(origen + bytes - 1);
  if (!cruza) {
    std::memmove(Puntero(base, destino), Puntero(base, origen), bytes);
    return;
  }
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t v;
    std::memcpy(&v, Puntero(base, origen + i * 4), 4);
    std::memcpy(Puntero(base, destino + i * 4), &v, 4);
  }
}

// Report counters. Written by the game thread that draws; no read-modify-write atomics (on the A57
// each fetch_add is an ldxr/stxr loop).
std::atomic<uint64_t> g_llamadas{0};
std::atomic<uint64_t> g_palabras{0};
std::atomic<uint64_t> g_lentos{0};
std::atomic<int64_t> g_siguiente_ms{0};

template <typename T>
inline void Sumar(std::atomic<T>& c, T n) {
  c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

void Informe() {
  const int64_t ahora = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  const int64_t siguiente = g_siguiente_ms.load(std::memory_order_relaxed);
  if (ahora < siguiente) {
    return;
  }
  g_siguiente_ms.store(ahora + 10000, std::memory_order_relaxed);
  if (siguiente == 0) {
    REXLOG_INFO("[d3d_registros] volcado de registros del D3D en nativo (sub_825A2AA0)");
    return;
  }
  const uint64_t llamadas = g_llamadas.exchange(0, std::memory_order_relaxed);
  const uint64_t palabras = g_palabras.exchange(0, std::memory_order_relaxed);
  NFSMW_INFORME_DIFERIDO("[d3d_registros] ultimos 10 s: {} llamadas, {} palabras copiadas ({:.1f} por llamada), {} por el "
              "camino del juego (anillo lleno o mascara 0)",
              llamadas, palabras, llamadas ? double(palabras) / double(llamadas) : 0.0,
              g_lentos.exchange(0, std::memory_order_relaxed));
}

}  // namespace

REX_EXTERN(__imp__sub_825A2AA0);
REX_EXTERN(sub_825A29E8);

REX_HOOK_RAW(sub_825A2AA0) {
  static const bool activo = REXCVAR_GET(nfsmw_d3d_registros_nativo);
  uint64_t mascara = ctx.r4.u64;
  // Phase 1 of the Direct3D-level renderer. Register base_registro + i comes from r6 + 4 * i.
  nfsmw::nativo::AprenderGrupoEspejo(ctx.r5.u32, mascara, ctx.r6.u32 - ctx.r3.u32);
  if (!activo || mascara == 0) {
    if (activo) {
      Sumar(g_lentos, uint64_t(1));
    }
    __imp__sub_825A2AA0(ctx, base);
    return;
  }

  const uint32_t dispositivo = ctx.r3.u32;
  uint32_t registro = ctx.r5.u32;      // r5
  uint32_t origen = ctx.r6.u32 - 4;    // r29
  uint32_t escritura = Leer32(base, dispositivo + 0);  // r4
  uint64_t copiadas = 0;
  uint32_t r3_final = dispositivo;  // the original leaves in r3 what the last slow call returned

  do {
    const uint32_t z = uint32_t(__builtin_clzll(mascara));  // mascara != 0 here
    const uint32_t fin = Leer32(base, dispositivo + 4);      // r9, reread on every iteration
    const uint32_t r = z + registro;                          // r28
    origen += z * 4;
    mascara <<= z;                                            // z <= 63
    const uint64_t invertida = ~mascara;
    const uint32_t n = invertida ? uint32_t(__builtin_clzll(invertida)) : 64u;  // r30, 1..64
    if (uint32_t(escritura + n * 4) < fin) {
      Escribir32(base, escritura + 4, ((n - 1) << 16) | r);
      CopiarPalabras(base, escritura + 8, origen + 4, n);
      escritura += 4 + n * 4;
      copiadas += n;
    } else {
      // Ring full: the game handles it, like the original (r3..r8 and its 144-byte frame).
      Sumar(g_lentos, uint64_t(1));
      const uint32_t pila = ctx.r1.u32;
      ctx.r1.u64 = pila - 144;
      Escribir32(base, pila - 144, pila);  // stwu r1,-144(r1): cadena de marcos
      ctx.r3.u64 = dispositivo;
      ctx.r4.u64 = escritura;
      ctx.r5.u64 = r;
      ctx.r6.u64 = origen;
      ctx.r7.u64 = n;
      ctx.r8.u64 = 1;
      ctx.lr = 0x825A2B0C;  // the one of "bl 0x825a29e8" at 0x825A2B08
      sub_825A29E8(ctx, base);
      ctx.r1.u64 = pila;
      escritura = ctx.r3.u32;
      r3_final = ctx.r3.u32;
    }
    origen += n * 4;
    mascara = n >= 64 ? 0 : (mascara << n);
    registro = r + n;
  } while (mascara != 0);

  Escribir32(base, dispositivo + 0, escritura);
  ctx.r3.u64 = r3_final;
  ctx.r4.u64 = escritura;
  ctx.r5.u64 = registro;

  Sumar(g_llamadas, uint64_t(1));
  Sumar(g_palabras, copiadas);
  if ((g_llamadas.load(std::memory_order_relaxed) & 1023) == 0) {
    Informe();
  }
}

// ---------------------------------------------------------------------------------------------------
// Measurement only.
//
// Other candidates for a native rewrite involve floating point (GetVisibleState: the box against the
// 6 view planes), many branches (TreeCull and DrawAScenery of the scenery) or a lot of code (the effect
// parameter upload, sub_826992F0). Rewriting them without knowing their cost would be a gamble, and a
// rounding error in culling shows up as popping. These hooks change nothing (they call the original):
// they count the calls and time 1 in every 16 (inclusive time: TreeCull includes DrawAScenery).
// "[medida]" log line every 10 s: calls/s and estimated ms per second.
// The counters are not read-modify-write atomics (A57 without LSE); if two threads count at the same
// time a count can be lost, which does not matter for this purpose.
// ---------------------------------------------------------------------------------------------------
namespace {
struct Medida {
  const char* nombre;
  std::atomic<uint64_t> llamadas{0};
  std::atomic<uint64_t> muestras{0};
  std::atomic<uint64_t> ns{0};
};
Medida g_m_visible{"GetVisibleState (8243E7D8)"};
Medida g_m_draw{"DrawAScenery (824C2850)"};
Medida g_m_tree{"TreeCull (824C2F48)"};
Medida g_m_efecto{"parametros de efecto (826992F0)"};
Medida* const kMedidas[] = {&g_m_visible, &g_m_draw, &g_m_tree, &g_m_efecto};
std::atomic<int64_t> g_siguiente_medida_ms{0};

inline int64_t AhoraNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void InformeMedida() {
  const int64_t ahora = AhoraNs() / 1000000;
  const int64_t siguiente = g_siguiente_medida_ms.load(std::memory_order_relaxed);
  if (ahora < siguiente) {
    return;
  }
  g_siguiente_medida_ms.store(ahora + 10000, std::memory_order_relaxed);
  if (siguiente == 0) {
    return;
  }
  std::string linea;
  for (Medida* m : kMedidas) {
    const uint64_t n = m->llamadas.exchange(0, std::memory_order_relaxed);
    const uint64_t k = m->muestras.exchange(0, std::memory_order_relaxed);
    const uint64_t ns = m->ns.exchange(0, std::memory_order_relaxed);
    const double ms_s = k ? double(ns) / double(k) * double(n) / 1e6 / 10.0 : 0.0;
    linea += fmt::format(" | {}: {:.0f} llamadas/s, {:.2f} ms/s", m->nombre, double(n) / 10.0, ms_s);
  }
  NFSMW_INFORME_DIFERIDO("[medida] ultimos 10 s{}", linea);
}

template <typename F>
inline void Medir(Medida& m, F&& llamar) {
  const uint64_t n = m.llamadas.load(std::memory_order_relaxed) + 1;
  m.llamadas.store(n, std::memory_order_relaxed);
  if ((n & 15) != 0) {
    llamar();
    return;
  }
  const int64_t t0 = AhoraNs();
  llamar();
  Sumar(m.ns, uint64_t(AhoraNs() - t0));
  Sumar(m.muestras, uint64_t(1));
}
}  // namespace

// ---------------------------------------------------------------------------------------------------
// eViewPlatInterface::GetVisibleState (sub_8243E7D8) in native code, bit-identical.
//
// WHAT IT DOES (PowerPC read instruction by instruction: nfsmw_recomp.5.cpp and, for the helper, .87.cpp)
//   Input: r3 = view (6 planes of 16 bytes at [r3] + 192), r4 and r5 = minimum and maximum corners of the
//   box (3 floats each), r6 = 4x4 matrix or 0. With a matrix, its helper (8243_E740) transforms the box: for
//   each axis, row * min and row * max, the min() and the max() of the two, added to row 3. Then: center
//   c = (min + max) * 0.5 with w = 1, half-extent e = max - c and, for each plane P, d = c . P (4 components)
//   and r = e . |P| (3). "fuera" (outside) = min(0.5, d0 + r0, ..., d5 + r5) and "dentro" (inside) =
//   min(0.5, d0 - r0, ..., d5 - r5), lane by lane. r3 = 0 if some lane of "fuera" is not >= 0 (NaN included),
//   2 if all lanes of "dentro" are, and 1 otherwise.
//
// WHY NATIVE
//   Measured ([medida] log line): ~82,000 calls/s at 0.53 us, about 44 ms/s of the game thread. Recompiled,
//   each vector instruction reads and writes its register in the context, the box goes through the stack
//   and through f0-f13 in double precision, and each call writes the FPCR twice (disableFlushMode for the lfs
//   and enableFlushMode for the vector part). Here everything stays in registers and the FPCR is written at
//   most once.
//
// WHY IT IS BIT-IDENTICAL
//   - The same simde and rex::ppc functions as the generated code, with the operands in the same order (with
//     a NaN, simde_mm_min_ps returns the second one). The constants (0.5, the xyz mask, the 1 of w and the
//     sign bit) come from the same expressions. The box (and the matrix), operation by operation like the
//     original.
//   - The 12 dot products of the planes (vmsum4fp128 / vmsum3fp128 = simde_mm_dp_ps): on the Switch and on
//     PC, dp_ps multiplies and adds in pairs, ((x0 + x1) + (x2 + x3)), with faddp or DPPS; here they are done
//     four at a time with simde_mm_hadd_ps, which is that same sum (faddp / haddps): same products, same
//     sums, same order. The two chains min(0.5, x0, ..., x5) only serve to see whether they are >= 0: without
//     NaN they are the true minimum (>= 0 if and only if all six are); with any NaN the original's chain is
//     done, value by value. None of those values leaves the function (v0-v13, see registers): only r3. Where
//     dp_ps is not native, everything is done one by one, like the original.
//   - The same denormal mode: enableFlushMode() before the vector part, and the thread leaves in that mode,
//     as with the original. Its disableFlushMode only matters for the lfs, which are not needed here (see
//     registers).
//   - Same compiler and same options as the generated code: on the Switch this file and nfsmw_recomp get the
//     same FLAGS, without -ffp-contract=off (only the SDK has that). The center's "vmaddfp" comes out as
//     fmla in the original (disassembled) and here (devkitA64, same expression). Even if one side did not
//     fuse it, the only possible difference would be the sign of a zero in c, which changes no comparison.
//   - Memory: the same reads and the same writes in its 128-byte frame below r1 (lr at r1 - 8, the frame
//     back link, the box at +80 and +96 or, with a matrix, the two transformed 16-byte vectors): the stack
//     ends up identical byte for byte. If r1 is not 16-byte aligned, or some read lands inside that frame
//     (the original would do it after writing the frame), the original runs.
//   - Registers: r3, and r12 and lr as its epilogue leaves them; r1 untouched. The other volatiles the
//     original changes (r4-r11, f0, f9-f13 and v0-v13) are read by nobody: liveness analysis of all the
//     generated code (56,338 functions). At its 17 direct call sites, only r3 and f1 (which it does not
//     touch) are live afterwards. The two branches to it (8243_E7D0 and 8221_E7A0) are only reached through
//     indirect calls, and there the caller cannot rely on another function's volatiles: out of the game's
//     12,943, the analysis flags 47 (r4-r10 read by variadic functions or by paths it cannot tell apart, and
//     v1 as a returned vector); 40 do not set up r5 and r6, which this one reads on entry, and the other 7,
//     reviewed one by one, call other things. cr6, xer, v30 and v31 are local variables in the generated code.
//
// SELF-CHECKING GUARD (project rule)
//   The first kComprobaciones calls and then 1 of every kPeriodo: the native version computes and writes its
//   frame, what it left is saved, the frame is undone, the original runs and r3, r12, lr, r1, the denormal
//   mode and the 128 bytes of the frame are compared (the 32 of the box only if there is no NaN: the original
//   passes it through lfs/stfs in double precision, and a signaling NaN becomes quiet). In those calls the
//   original's state is kept. A single difference turns the native version off for the rest of the session
//   ("[visible] DIFERENCIA" in the log, with the data). "[visible]" line every 10 s with the counts. If
//   something looked wrong: nfsmw_visible_nativo = false.
// ---------------------------------------------------------------------------------------------------
#include <rex/ppc/intrinsics.h>

REXCVAR_DEFINE_BOOL(nfsmw_visible_nativo, true, "NFSMW",
                    "eViewPlatInterface::GetVisibleState (sub_8243E7D8: la caja contra los 6 planos de la vista) en "
                    "nativo (build 174), identico bit a bit. Se comprueba contra la original (las primeras 200.000 "
                    "llamadas y despues 1 de cada 4096) y se apaga sola si difiere; false = la original")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_8243E7D8);

namespace {
namespace visible {

using V = simde__m128;

constexpr uint64_t kComprobaciones = 200000;  // first calls checked (~2.5 s of racing)
constexpr uint64_t kPeriodo = 4096;           // then 1 of every kPeriodo (a power of 2)

// Like the other counters in the file: no atomic read-modify-write (A57 without LSE).
std::atomic<bool> g_apagado{false};
std::atomic<uint64_t> g_llamadas{0};
std::atomic<uint64_t> g_nativas{0};      // ultimos 10 s
std::atomic<uint64_t> g_originales{0};   // last 10 s: misaligned stack or a read inside the frame
std::atomic<uint64_t> g_comprobadas{0};  // since startup, all without differences
std::atomic<int64_t> g_siguiente_ms{0};

// lvx128 exactly as the generated code translates it: the aligned 16-byte block, reversed with VectorMaskL
// (lane 3 is PowerPC word 0).
inline V Lvx(uint8_t* base, uint32_t direccion) {
  const uint32_t ea = direccion & ~0xFu;
  return simde_mm_castsi128_ps(
      simde_mm_shuffle_epi8(simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(Puntero(base, ea))),
                            simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(VectorMaskL))));
}

// stvx as is.
inline void Stvx(uint8_t* base, uint32_t direccion, V v) {
  const uint32_t ea = direccion & ~0xFu;
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(Puntero(base, ea)),
                       simde_mm_shuffle_epi8(simde_mm_castps_si128(v),
                                             simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(VectorMaskL))));
}

inline V Y(V a, simde__m128i mascara) {  // vand vD,a,mascara
  return simde_mm_castsi128_ps(simde_mm_and_si128(simde_mm_castps_si128(a), mascara));
}

inline V Absoluto(V plano, simde__m128i signo) {  // vandc vD,plano,v13 = simde_mm_andnot_si128(v13, plano)
  return simde_mm_castsi128_ps(simde_mm_andnot_si128(signo, simde_mm_castps_si128(plano)));
}

// Does a read of n bytes at "direccion" land inside the 128-byte frame that the original writes below r1?
inline bool EnElMarco(uint32_t direccion, uint32_t n, uint32_t marco) {
  return uint32_t(direccion - marco + n - 1) < 127u + n;
}

inline uint32_t Grande32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

inline bool EsNaN(uint32_t bits) {
  return (bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu) != 0;
}

// Do the 32 bytes of the box in the frame (+80 to +111) contain a NaN?
inline bool CajaConNaN(const uint8_t* marco) {
  for (uint32_t i = 80; i < 112; i += 4) {
    if (EsNaN(Grande32(marco + i))) {
      return true;
    }
  }
  return false;
}

struct Calculo {
  uint32_t r3;
  bool matriz;
  uint32_t caja[6];  // bits of min.x, min.y, min.z, max.x, max.y and max.z
  V mn;              // the box the original leaves in its frame (transformed, if there is a matrix)
  V mx;
};

// Where simde_mm_dp_ps is native, it adds in pairs, ((x0 + x1) + (x2 + x3)): on the Switch vmulq_f32 +
// vaddvq_f32 (two faddp) and on PC (SSE4.1) DPPS. simde_mm_hadd_ps does those same sums in one instruction
// (faddp / haddps, which is not fused with the product), so the 12 dot products come out bit-identical four
// at a time. Where it is not native (portable simde adds in a different order), simde_mm_dp_ps one by one,
// like the original. The PC test forces the second path with NFSMW_VISIBLE_POR_PAREJAS=0.
#ifndef NFSMW_VISIBLE_POR_PAREJAS
#if defined(SIMDE_ARM_NEON_A64V8_NATIVE) || (defined(SIMDE_X86_SSE4_1_NATIVE) && defined(SIMDE_X86_SSE3_NATIVE))
#define NFSMW_VISIBLE_POR_PAREJAS 1
#else
#define NFSMW_VISIBLE_POR_PAREJAS 0
#endif
#endif
constexpr bool kPorParejas = NFSMW_VISIBLE_POR_PAREJAS != 0;

// The product of a vmsum3fp128 (e . |P|): lane 0 (w) at +0.0, like the 0xEF mask of simde_mm_dp_ps.
inline V SinW(V e, V plano, simde__m128i signo, simde__m128i xyz) {
  return Y(simde_mm_mul_ps(e, Absoluto(plano, signo)), xyz);
}

inline V Pareja(V x) {  // [a b c d] -> [a+b c+d a+b c+d]
  return simde_mm_hadd_ps(x, x);
}

// The original's chain with a NaN: min(0.5, x0, ..., x5) with simde_mm_min_ps, each value repeated in the 4
// lanes as in the original (lanes [x0 x1 x2 x3] and [x4 x5 x4 x5]). Is it >= 0?
[[gnu::noinline]] bool CadenaConNaN(V medio, V x0123, V x45) {
  alignas(16) float a[4];
  alignas(16) float b[4];
  simde_mm_store_ps(a, x0123);
  simde_mm_store_ps(b, x45);
  V m = simde_mm_min_ps(medio, simde_mm_set1_ps(a[0]));
  m = simde_mm_min_ps(m, simde_mm_set1_ps(a[1]));
  m = simde_mm_min_ps(m, simde_mm_set1_ps(a[2]));
  m = simde_mm_min_ps(m, simde_mm_set1_ps(a[3]));
  m = simde_mm_min_ps(m, simde_mm_set1_ps(b[0]));
  m = simde_mm_min_ps(m, simde_mm_set1_ps(b[1]));
  return simde_mm_movemask_ps(simde_mm_cmpge_ps(m, simde_mm_setzero_ps())) == 0xF;
}

// The same, fast: without NaN that chain is the true minimum, and it is >= 0 if and only if all six are.
[[gnu::always_inline]] inline bool NoNegativos(V medio, V x0123, V x45) {
  const V cero = simde_mm_setzero_ps();
  if (simde_mm_movemask_ps(simde_mm_and_ps(simde_mm_cmpge_ps(x0123, cero), simde_mm_cmpge_ps(x45, cero))) == 0xF) {
    return true;  // all six >= 0 (with a NaN the comparison is already false)
  }
  if (simde_mm_movemask_ps(simde_mm_or_ps(simde_mm_cmpunord_ps(x0123, x0123), simde_mm_cmpunord_ps(x45, x45))) == 0) {
    return false;  // some < 0 and none is NaN
  }
  return CadenaConNaN(medio, x0123, x45);
}

// The helper 8243_E740 (nfsmw_recomp.87.cpp), in its order: the box times the matrix (rows at m, +16, +32, +48).
[[gnu::always_inline]] inline void CajaPorMatriz(uint8_t* base, uint32_t m, V& mn, V& mx) {
  const simde__m128i v0 = simde_mm_castps_si128(mn);
  const simde__m128i v13 = simde_mm_castps_si128(mx);
  const V v8 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(v0, 0xFF));   // vspltw v8,v0,0
  const V v7 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(v13, 0xFF));  // vspltw v7,v13,0
  const V fila0 = Lvx(base, m);                                           // lvx128 v12,r0,r3
  const V v6 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(v0, 0xAA));   // vspltw v6,v0,1
  const V v4 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(v0, 0x55));   // vspltw v4,v0,2
  const V v5 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(v13, 0xAA));  // vspltw v5,v13,1
  const V a0 = simde_mm_mul_ps(fila0, v8);                                // vmulfp128 v0,v12,v8
  const V v3 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(v13, 0x55));  // vspltw v3,v13,2
  const V b0 = simde_mm_mul_ps(fila0, v7);                                // vmulfp128 v13,v12,v7
  const V fila1 = Lvx(base, m + 16);                                      // lvx128 v11,r0,r11
  const V a1 = simde_mm_mul_ps(fila1, v6);                                // vmulfp128 v12,v11,v6
  const V fila2 = Lvx(base, m + 32);                                      // lvx128 v9,r0,r9
  const V b1 = simde_mm_mul_ps(fila1, v5);                                // vmulfp128 v11,v11,v5
  const V fila3 = Lvx(base, m + 48);                                      // lvx128 v10,r0,r10
  const V a2 = simde_mm_mul_ps(fila2, v4);                                // vmulfp128 v8,v9,v4
  const V b2 = simde_mm_mul_ps(fila2, v3);                                // vmulfp128 v9,v9,v3
  const V lo0 = simde_mm_min_ps(a0, b0);                                  // vminfp v7,v0,v13
  const V hi0 = simde_mm_max_ps(a0, b0);                                  // vmaxfp v0,v0,v13
  const V lo1 = simde_mm_min_ps(a1, b1);                                  // vminfp v13,v12,v11
  const V hi1 = simde_mm_max_ps(a1, b1);                                  // vmaxfp v12,v12,v11
  const V lo2 = simde_mm_min_ps(a2, b2);                                  // vminfp v11,v8,v9
  const V lo = simde_mm_add_ps(fila3, lo0);                               // vaddfp v7,v10,v7
  const V hi = simde_mm_add_ps(fila3, hi0);                               // vaddfp v0,v10,v0
  const V hi2 = simde_mm_max_ps(a2, b2);                                  // vmaxfp v10,v8,v9
  mn = simde_mm_add_ps(simde_mm_add_ps(lo, lo1), lo2);                    // vaddfp v13,v7,v13; vaddfp v13,v13,v11
  mx = simde_mm_add_ps(simde_mm_add_ps(hi, hi1), hi2);                    // vaddfp v0,v0,v12; vaddfp v0,v0,v10
}

// The whole original, without touching memory or the context (except the denormal mode, like the original).
// false = the original has to run, and in that case nothing has been touched.
[[gnu::always_inline]] inline bool Calcular(PPCContext& ctx, uint8_t* base, Calculo& k) {
  const uint32_t pila = ctx.r1.u32;
  const uint32_t marco = pila - 128;  // stwu r1,-128(r1)
  const uint32_t vista = ctx.r3.u32;
  const uint32_t pmin = ctx.r4.u32;
  const uint32_t pmax = ctx.r5.u32;
  const uint32_t mat = ctx.r6.u32;
  if ((pila & 0xFu) != 0 || EnElMarco(pmin, 12, marco) || EnElMarco(pmax, 12, marco) || EnElMarco(vista, 4, marco) ||
      (mat != 0 && EnElMarco(mat & ~0xFu, 64, marco))) {
    return false;
  }
  const uint32_t planos = Leer32(base, vista) + 192u;  // lwz r11,0(r8); addi r11,r11,192
  if (EnElMarco(planos & ~0xFu, 96, marco)) {
    return false;
  }
  // lfs of the box, stfs to r1+80 and r1+96 and lvx128 back: lanes 3, 2 and 1 = x, y, z. Lane 0 (w) would be
  // whatever was on the stack: it is always masked before use, and the matrix helper does not read it.
  for (uint32_t i = 0; i < 3; ++i) {
    k.caja[i] = Leer32(base, pmin + 4 * i);
    k.caja[3 + i] = Leer32(base, pmax + 4 * i);
  }
  V mn = simde_mm_castsi128_ps(simde_mm_set_epi32(int32_t(k.caja[0]), int32_t(k.caja[1]), int32_t(k.caja[2]), 0));
  V mx = simde_mm_castsi128_ps(simde_mm_set_epi32(int32_t(k.caja[3]), int32_t(k.caja[4]), int32_t(k.caja[5]), 0));
  ctx.fpscr.enableFlushMode();  // the one of "vcfux v10,v13,1" (and the helper's): the original leaves in this mode
  k.matriz = mat != 0;
  if (k.matriz) {  // cmplwi cr6,r6,0; beq; bl to the helper with r4 = r1+80 and r5 = r1+96
    CajaPorMatriz(base, mat, mn, mx);
  }
  k.mn = mn;
  k.mx = mx;

  const simde__m128i v11 = simde_mm_set1_epi32(int(0x0));                // vspltisw v11,0
  const simde__m128i unos = simde_mm_set1_epi32(int(0xFFFFFFFF));        // vspltisw v0,-1
  const simde__m128i uno = simde_mm_set1_epi32(int(0x1));                // vspltisw v13,1
  const simde__m128i xyz = simde_mm_alignr_epi8(unos, v11, 12);          // vsldoi v12,v0,v11,4
  const V medio = simde_mm_mul_ps(rex::ppc::simde_mm_cvtepu32_ps_(uno),  // vcfux v10,v13,1
                                  simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x3F000000))));
  const V w1 = simde_mm_castsi128_ps(                                    // vcfux v3,v13,0; vsldoi v3,v11,v3,4
      simde_mm_alignr_epi8(v11, simde_mm_castps_si128(rex::ppc::simde_mm_cvtepu32_ps_(uno)), 12));
  const simde__m128i signo = simde_mm_sllv_epi32(unos, simde_mm_and_si128(unos, simde_mm_set1_epi32(0x1F)));  // vslw
  const V cero = simde_mm_castsi128_ps(v11);

  const V a = Y(mn, xyz);                                  // vand v0,v2,v12
  const V b = Y(mx, xyz);                                  // vand v12,v1,v12
  const V suma = simde_mm_add_ps(a, b);                    // vaddfp v0,v0,v12
  const V p0 = Lvx(base, planos);                          // lvx128 v9,r0,r11
  const V p1 = Lvx(base, planos + 16);                     // lvx128 v8,r0,r9
  const V p2 = Lvx(base, planos + 32);                     // lvx128 v7,r0,r8
  const V p3 = Lvx(base, planos + 48);                     // lvx128 v6,r0,r7
  const V p4 = Lvx(base, planos + 64);                     // lvx128 v5,r0,r6
  const V p5 = Lvx(base, planos + 80);                     // lvx128 v4,r0,r5
  const V c = simde_mm_add_ps(simde_mm_mul_ps(suma, medio), w1);  // vmaddfp v0,v0,v10,v3
  const V e = simde_mm_sub_ps(b, c);                       // vsubfp v12,v12,v0
  if constexpr (kPorParejas) {
    // d = c . P and r = e . |P| of the six planes (vmsum4fp128 / vmsum3fp128), in lanes [0 1 2 3] and [4 5 4 5];
    // "fuera" with d + r and "dentro" with d - r, with the same operand order as the original.
    const V d0123 = simde_mm_hadd_ps(simde_mm_hadd_ps(simde_mm_mul_ps(c, p0), simde_mm_mul_ps(c, p1)),
                                     simde_mm_hadd_ps(simde_mm_mul_ps(c, p2), simde_mm_mul_ps(c, p3)));
    const V d45 = Pareja(simde_mm_hadd_ps(simde_mm_mul_ps(c, p4), simde_mm_mul_ps(c, p5)));
    const V r0123 = simde_mm_hadd_ps(simde_mm_hadd_ps(SinW(e, p0, signo, xyz), SinW(e, p1, signo, xyz)),
                                     simde_mm_hadd_ps(SinW(e, p2, signo, xyz), SinW(e, p3, signo, xyz)));
    const V r45 = Pareja(simde_mm_hadd_ps(SinW(e, p4, signo, xyz), SinW(e, p5, signo, xyz)));
    if (!NoNegativos(medio, simde_mm_add_ps(d0123, r0123), simde_mm_add_ps(d45, r45))) {
      k.r3 = 0;
    } else {
      k.r3 = NoNegativos(medio, simde_mm_sub_ps(d0123, r0123), simde_mm_sub_ps(d45, r45)) ? 2u : 1u;
    }
    return true;
  }
  // One by one, like the original.
  const V d0 = simde_mm_dp_ps(c, p0, 0xFF);                // vmsum4fp128 v9,v0,v9
  const V d1 = simde_mm_dp_ps(c, p1, 0xFF);                // vmsum4fp128 v8,v0,v8
  const V r0 = simde_mm_dp_ps(e, Absoluto(p0, signo), 0xEF);  // vandc v2,v9,v13; vmsum3fp128 v3,v12,v2
  const V d2 = simde_mm_dp_ps(c, p2, 0xFF);                // vmsum4fp128 v9,v0,v7
  const V d3 = simde_mm_dp_ps(c, p3, 0xFF);                // vmsum4fp128 v7,v0,v6
  const V r2 = simde_mm_dp_ps(e, Absoluto(p2, signo), 0xEF);  // vandc v3,v7,v13; vmsum3fp128 v3,v12,v3
  const V r1 = simde_mm_dp_ps(e, Absoluto(p1, signo), 0xEF);  // vandc v1,v8,v13; vmsum3fp128 v6,v12,v1
  const V r3 = simde_mm_dp_ps(e, Absoluto(p3, signo), 0xEF);  // vandc v2,v6,v13; vmsum3fp128 v10,v12,v2
  const V d4 = simde_mm_dp_ps(c, p4, 0xFF);                // vmsum4fp128 v9,v0,v5
  const V d5 = simde_mm_dp_ps(c, p5, 0xFF);                // vmsum4fp128 v0,v0,v4
  const V r4 = simde_mm_dp_ps(e, Absoluto(p4, signo), 0xEF);  // vandc v3,v5,v13; vmsum3fp128 v13,v12,v3
  const V r5 = simde_mm_dp_ps(e, Absoluto(p5, signo), 0xEF);  // vandc v5,v4,v13; vmsum3fp128 v12,v12,v5
  // "fuera": min(0.5, d + r) plane by plane, always with the accumulator as the first operand (vminfp
  // v30,v10,v2; v3,v30,v2; v6,v3,v6; v7,v6,v2; v12,v7,v8; v12,v12,v9).
  V fuera = simde_mm_min_ps(medio, simde_mm_add_ps(d0, r0));  // vaddfp v2,v9,v3
  fuera = simde_mm_min_ps(fuera, simde_mm_add_ps(d1, r1));    // vaddfp v2,v8,v6
  fuera = simde_mm_min_ps(fuera, simde_mm_add_ps(d2, r2));    // vaddfp v6,v9,v3
  fuera = simde_mm_min_ps(fuera, simde_mm_add_ps(d3, r3));    // vaddfp v2,v7,v10
  fuera = simde_mm_min_ps(fuera, simde_mm_add_ps(d4, r4));    // vaddfp v8,v9,v13
  fuera = simde_mm_min_ps(fuera, simde_mm_add_ps(d5, r5));    // vaddfp v9,v0,v12
  // "dentro": min(0,5, d - r) (vminfp v1,v10,v31; v8,v1,v8; v8,v8,v9; v10,v8,v10; v13,v10,v13; v0,v13,v0).
  V dentro = simde_mm_min_ps(medio, simde_mm_sub_ps(d0, r0));  // vsubfp v31,v9,v3
  dentro = simde_mm_min_ps(dentro, simde_mm_sub_ps(d1, r1));   // vsubfp v8,v8,v6
  dentro = simde_mm_min_ps(dentro, simde_mm_sub_ps(d2, r2));   // vsubfp v9,v9,v3
  dentro = simde_mm_min_ps(dentro, simde_mm_sub_ps(d3, r3));   // vsubfp v10,v7,v10
  dentro = simde_mm_min_ps(dentro, simde_mm_sub_ps(d4, r4));   // vsubfp v13,v9,v13
  dentro = simde_mm_min_ps(dentro, simde_mm_sub_ps(d5, r5));   // vsubfp v0,v0,v12
  // vcmpgefp. + mfocrf + not + rlwinm: 0 if not all are >= 0; if they are, vcmpgefp. of "dentro" + rlwimi + rlwinm.
  if (simde_mm_movemask_ps(simde_mm_cmpge_ps(fuera, cero)) != 0xF) {
    k.r3 = 0;
  } else {
    k.r3 = simde_mm_movemask_ps(simde_mm_cmpge_ps(dentro, cero)) == 0xF ? 2u : 1u;
  }
  return true;
}

// The original's writes to its stack frame, with what would remain at the end.
[[gnu::always_inline]] inline void EscribirMarco(uint8_t* base, uint32_t pila, uint64_t lr, const Calculo& k) {
  const uint32_t marco = pila - 128;
  Escribir32(base, pila - 8, uint32_t(lr));  // mflr r12; stw r12,-8(r1)
  Escribir32(base, marco, pila);             // stwu r1,-128(r1)
  if (k.matriz) {
    Stvx(base, marco + 80, k.mn);  // stvx v13,r0,r4 of the helper (overwrites the stfs of the box)
    Stvx(base, marco + 96, k.mx);  // stvx v0,r0,r5
  } else {
    for (uint32_t i = 0; i < 3; ++i) {
      Escribir32(base, marco + 80 + 4 * i, k.caja[i]);      // stfs f0,80 / f13,84 / f12,88
      Escribir32(base, marco + 96 + 4 * i, k.caja[3 + i]);  // stfs f11,96 / f10,100 / f9,104
    }
  }
}

// Normal path. false = nothing touched: let the original run.
inline bool Nativa(PPCContext& ctx, uint8_t* base) {
  Calculo k;
  if (!Calcular(ctx, base, k)) {
    return false;
  }
  EscribirMarco(base, ctx.r1.u32, ctx.lr, k);
  ctx.r3.u64 = k.r3;
  ctx.r12.u64 = uint32_t(ctx.lr);  // epilogo: lwz r12,-8(r1); mtlr r12
  ctx.lr = ctx.r12.u64;
  return true;
}

// Guard: the native version and then the original over the same frame; the original's state is kept.
void Comprobar(PPCContext& ctx, uint8_t* base, uint64_t n) {
  const uint32_t pila = ctx.r1.u32;
  const uint32_t marco = pila - 128;
  const uint32_t vista = ctx.r3.u32;
  const uint32_t pmin = ctx.r4.u32;
  const uint32_t pmax = ctx.r5.u32;
  const uint32_t mat = ctx.r6.u32;
  const uint64_t lr = ctx.lr;
  const uint64_t r1 = ctx.r1.u64;
  Calculo k;
  if (Desplazamiento(marco) != Desplazamiento(pila - 1) || !Calcular(ctx, base, k)) {
    Sumar(g_originales, uint64_t(1));
    __imp__sub_8243E7D8(ctx, base);
    return;
  }
  uint8_t* const p = Puntero(base, marco);
  uint8_t antes[128];
  uint8_t nativa[128];
  std::memcpy(antes, p, 128);
  EscribirMarco(base, pila, lr, k);
  std::memcpy(nativa, p, 128);
  const uint32_t csr_nativa = ctx.fpscr.csr;
  std::memcpy(p, antes, 128);  // the original starts from the same frame
  __imp__sub_8243E7D8(ctx, base);

  const char* que = nullptr;
  uint32_t byte = 0;
  if (ctx.r3.u64 != k.r3) {
    que = "r3";
  } else if (ctx.r12.u64 != uint32_t(lr)) {
    que = "r12";
  } else if (ctx.lr != uint32_t(lr)) {
    que = "lr";
  } else if (ctx.r1.u64 != r1) {
    que = "r1";
  } else if (ctx.fpscr.csr != csr_nativa) {
    que = "modo de denormales";
  } else {
    const bool con_nan = CajaConNaN(nativa) || CajaConNaN(p);
    for (uint32_t i = 0; i < 128 && !que; ++i) {
      if (con_nan && i >= 80 && i < 112) {
        continue;  // NaN in the box: the original passes it through double (lfs/stfs) and can change its payload
      }
      if (p[i] != nativa[i]) {
        que = "marco de pila";
        byte = i;
      }
    }
  }
  if (que) {
    g_apagado.store(true, std::memory_order_relaxed);
    REXLOG_INFO("[visible] DIFERENCIA con la original ({}, byte +{} del marco) en la llamada {}: r3 nativa {} y original "
                "{}; vista 0x{:08X}, min 0x{:08X}, max 0x{:08X}, matriz 0x{:08X}, pila 0x{:08X}, caja {:08X} {:08X} "
                "{:08X} / {:08X} {:08X} {:08X}. Camino nativo APAGADO para el resto de la sesion: se queda la original",
                que, byte, n, k.r3, ctx.r3.u64, vista, pmin, pmax, mat, pila, k.caja[0], k.caja[1], k.caja[2],
                k.caja[3], k.caja[4], k.caja[5]);
    return;
  }
  Sumar(g_comprobadas, uint64_t(1));
  if (n == kComprobaciones) {
    REXLOG_INFO("[visible] {} llamadas comprobadas contra la original (r3, r12, lr, r1, modo de denormales y marco de "
                "pila), 0 diferencias: GetVisibleState en nativo, y sigue comprobando 1 de cada {}",
                g_comprobadas.load(std::memory_order_relaxed), kPeriodo);
  }
}

void Llamada(PPCContext& ctx, uint8_t* base) {
  static const bool activo = REXCVAR_GET(nfsmw_visible_nativo);
  if (!activo || g_apagado.load(std::memory_order_relaxed)) {
    __imp__sub_8243E7D8(ctx, base);
    return;
  }
  const uint64_t n = g_llamadas.load(std::memory_order_relaxed) + 1;
  g_llamadas.store(n, std::memory_order_relaxed);
  if (n <= kComprobaciones || (n & (kPeriodo - 1)) == 0) {
    Comprobar(ctx, base, n);
    return;
  }
  if (Nativa(ctx, base)) {
    Sumar(g_nativas, uint64_t(1));
    return;
  }
  Sumar(g_originales, uint64_t(1));
  __imp__sub_8243E7D8(ctx, base);
}

void Informe() {
  const int64_t ahora = AhoraNs() / 1000000;
  const int64_t siguiente = g_siguiente_ms.load(std::memory_order_relaxed);
  if (ahora < siguiente) {
    return;
  }
  g_siguiente_ms.store(ahora + 10000, std::memory_order_relaxed);
  const bool activo = REXCVAR_GET(nfsmw_visible_nativo);
  if (siguiente == 0) {
    REXLOG_INFO("[visible] GetVisibleState (8243E7D8) {}",
                activo ? "en nativo (build 174): empieza comprobando contra la original"
                       : "por la original (nfsmw_visible_nativo = false)");
    return;
  }
  if (!activo) {
    return;
  }
  NFSMW_INFORME_DIFERIDO("[visible] ultimos 10 s: {} en nativo, {} por la original (pila desalineada o lectura en su marco){}; "
              "comprobadas contra la original desde el arranque: {} (las primeras {} llamadas y despues 1 de cada {})",
              g_nativas.exchange(0, std::memory_order_relaxed), g_originales.exchange(0, std::memory_order_relaxed),
              g_apagado.load(std::memory_order_relaxed) ? " | APAGADO por diferencia" : "",
              g_comprobadas.load(std::memory_order_relaxed), kComprobaciones, kPeriodo);
}

}  // namespace visible
}  // namespace

REX_HOOK_RAW(sub_8243E7D8) {  // eViewPlatInterface::GetVisibleState: native (see above)
  Medir(g_m_visible, [&] { visible::Llamada(ctx, base); });
  if ((g_m_visible.llamadas.load(std::memory_order_relaxed) & 4095) == 0) {
    InformeMedida();
    visible::Informe();
  }
}

REX_EXTERN(__imp__sub_824C2850);
// DrawAScenery is native (nfsmw_escenario_nativo.cpp, cvar nfsmw_escenario_nativo, with its guard).
// The measurement stays here for comparison: the recompiled version took 0.83 us per call in a race.
namespace nfsmw::escenario_nativo {
void DrawAScenery(PPCContext& ctx, uint8_t* base);
}
REX_HOOK_RAW(sub_824C2850) {  // ScenerySectionHeader::DrawAScenery
  Medir(g_m_draw, [&] { nfsmw::escenario_nativo::DrawAScenery(ctx, base); });
}

REX_EXTERN(__imp__sub_824C2F48);
REX_HOOK_RAW(sub_824C2F48) {  // ScenerySectionHeader::TreeCull
  // Also its exact time, for the [tiron] juego log line (7,400 calls/s: two clock reads each). The 1-in-16
  // measurement of the [medida] line is unchanged.
  const int64_t inicio_ns = AhoraNs();
  Medir(g_m_tree, [&] { __imp__sub_824C2F48(ctx, base); });
  nfsmw::esperas::Sumar(nfsmw::esperas::kPreparadorEscenario, uint64_t(AhoraNs() - inicio_ns));
}

// ---------------------------------------------------------------------------------------------------
// sub_826992F0, the upload of effect parameters to the device, in native code.
//
// WHAT IT DOES (read instruction by instruction)
//   Two groups (the effect's and the shared one), each with 8 lists. For each 64-bit word of the
//   group's "dirty" mask, AND with the list's mask; each run of consecutive bits is a set of
//   consecutive 16-byte entries of a table. Lists 0 and 1: VS and PS floating-point constants
//   -> copy of 1-4 16-byte vectors to the device (register + 120 or + 376) and set their bits in
//   the device's dirty mask (+16 or +24). Lists 2-7: integers, booleans and the rest, through
//   calls to D3D functions. At the end, dcbzl of two 128-byte lines (clears the dirty bits).
//
//   Measured: ~150 ms/s of the game thread (~20 %), and in recompiled PowerPC each copied vector
//   is two loads/stores with byte permutation plus 8 reloads from the stack.
//
// WHAT IT DOES HERE
//   Lists 0 and 1 of both groups, in native code: a plain 16-byte copy (lvx128 + stvx128 with the
//   same permutation cancel out) and the same integers as the original. If any of lists 2-7 of
//   either group has work to do, the whole original is called: that way the D3D calls, their
//   registers and their stack are exactly the game's.
//
// SELF-CHECKING GUARD
//   The first kComprobaciones calls through the native path run twice: the native version
//   records each write (address and previous bytes), its results are saved, it is undone, the
//   original runs and they are compared byte by byte. A single difference turns the native path
//   off for good ("[efectos] DIFERENCIA" line in the log). "[efectos]" line every 10 s with
//   the counts.
// ---------------------------------------------------------------------------------------------------
REXCVAR_DEFINE_BOOL(nfsmw_d3d_efectos_nativo, true, "NFSMW",
                    "Envio de parametros de efecto (sub_826992F0) en nativo: entero con "
                    "nfsmw_d3d_efectos_nativo_todo (build 171); sin el, solo las constantes de coma flotante "
                    "y, con enteros, booleanos o texturas, la original. Se comprueba contra la original y se "
                    "apaga sola si difiere")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
inline uint64_t Leer64(uint8_t* base, uint32_t direccion) {
  uint64_t v;
  std::memcpy(&v, Puntero(base, direccion), 8);
  return __builtin_bswap64(v);
}

// Write log for the guard: address and previous bytes, to undo.
struct Deshacer {
  uint32_t direccion;
  uint32_t bytes;
  uint8_t antes[128];
};

struct Escritor {
  uint8_t* base;
  std::vector<Deshacer>* registro;  // nullptr outside the check

  void Anotar(uint32_t direccion, uint32_t bytes) {
    if (!registro) {
      return;
    }
    Deshacer d;
    d.direccion = direccion;
    d.bytes = bytes;
    std::memcpy(d.antes, Puntero(base, direccion), bytes);
    registro->push_back(d);
  }
  void Copiar16(uint32_t destino, uint32_t origen) {
    Anotar(destino, 16);
    std::memmove(Puntero(base, destino), Puntero(base, origen), 16);
  }
  void Escribir64(uint32_t direccion, uint64_t valor) {
    Anotar(direccion, 8);
    valor = __builtin_bswap64(valor);
    std::memcpy(Puntero(base, direccion), &valor, 8);
  }
  void Cero128(uint32_t direccion) {
    Anotar(direccion, 128);
    std::memset(Puntero(base, direccion), 0, 128);
  }
};

struct GrupoEfecto {
  uint32_t cuenta;    // 64-bit words of the mask ([this+288] or [this+292])
  uint32_t sucios;    // the group's dirty mask (this or [this+256])
  uint32_t tabla;     // r28 o r27
  uint32_t listas;    // offset of list 0 within the table (0 or 32)
  uint32_t entradas;  // [tabla+64] o [tabla+68]
  uint32_t r23;
  uint32_t r24;
};

// Some of lists 2-7 have work: those call D3D functions -> the original.
bool HayLlamadas(uint8_t* base, const GrupoEfecto& g) {
  for (uint32_t k = 2; k < 8; ++k) {
    const uint32_t lista = Leer32(base, g.tabla + g.listas + 4 * k);
    for (uint32_t i = 0; i < g.cuenta; ++i) {
      if (Leer64(base, g.sucios + i * 8) & Leer64(base, lista + i * 8)) {
        return true;
      }
    }
  }
  return false;
}

// Lists 0 (field 4, register+120, device dirty bits +16) and 1 (field 8, +376, +24).
uint64_t ListaFlotante(Escritor& w, const GrupoEfecto& g, uint32_t dispositivo, uint32_t k) {
  uint8_t* const base = w.base;
  const uint32_t campo = k == 0 ? 4 : 8;
  const uint32_t sesgo = k == 0 ? 120 : 376;
  const uint32_t sucios_dispositivo = dispositivo + (k == 0 ? 16 : 24);
  const uint64_t r18 = uint64_t(1) << 63;
  uint64_t vectores = 0;
  const uint32_t lista = Leer32(base, g.tabla + g.listas + 4 * k);
  for (uint32_t i = 0; i < g.cuenta; ++i) {
    uint64_t m = Leer64(base, g.sucios + i * 8) & Leer64(base, lista + i * 8);
    uint32_t e = g.entradas + i * 1024;
    while (m != 0) {
      const uint32_t z = uint32_t(__builtin_clzll(m));
      e += z * 16;
      m <<= z;
      const uint64_t inv = ~m;
      const uint32_t n = inv ? uint32_t(__builtin_clzll(inv)) : 64u;
      const uint32_t fin = e + n * 16;
      m = n >= 64 ? 0 : (m << n);
      do {
        const uint32_t w0 = Leer32(base, e);
        uint32_t wc = Leer32(base, e + campo);
        const uint32_t t = g.r23 + (((w0 << 17) | (w0 >> 15)) & 0x1FFF8u);
        const uint32_t idx = Leer32(base, t + 4);
        const uint32_t origen = g.r24 + (((idx << 4) | (idx >> 28)) & 0xFFFF0u);
        const uint32_t destino = dispositivo + ((((wc & 0x3FFu) + sesgo) << 4) & 0xFFFFFFF0u);
        uint32_t j = 0;
        do {
          w.Copiar16((destino + j * 16) & ~0xFu, (origen + j * 16) & ~0xFu);
          ++vectores;
          ++j;
          wc = Leer32(base, e + campo);
        } while (j <= ((wc >> 10) & 3u));
        wc = Leer32(base, e + campo);
        e += 16;
        const uint64_t lo = wc & 0x3FFu;
        const uint64_t a = (lo >> 2) & 0x3FFFFFFFu;                          // r10
        const uint64_t b = ((((wc >> 10) & 3u) + lo) >> 2) & 0x3FFFFFFFu;     // r9
        uint64_t d = (b - a) & 0xFFFFFFFFu;
        uint64_t s = d & 0x7F;
        if (s > 0x3F) {
          s = 0x3F;
        }
        const uint64_t bits = uint64_t(int64_t(r18) >> s);
        const uint8_t a8 = uint8_t(a);
        const uint64_t v = (a8 & 0x40) ? 0 : (bits >> (a8 & 0x7F));
        w.Escribir64(sucios_dispositivo, v | Leer64(base, sucios_dispositivo));
      } while (e < fin);
    }
  }
  return vectores;
}

constexpr uint32_t kComprobaciones = 256;
std::atomic<bool> g_efectos_apagado{false};
std::atomic<uint32_t> g_efectos_comprobadas{0};
std::atomic<uint64_t> g_efectos_nativas{0};
std::atomic<uint64_t> g_efectos_originales{0};
std::atomic<uint64_t> g_efectos_vectores{0};
std::atomic<int64_t> g_efectos_siguiente_ms{0};

// The whole native path. Returns false (without having written anything) if the original has to run.
bool EfectosNativo(Escritor& w, uint32_t self, uint64_t& vectores) {
  uint8_t* const base = w.base;
  const uint32_t tabla_a = Leer32(base, self + 536);
  GrupoEfecto a;
  a.cuenta = Leer32(base, self + 288);
  a.sucios = self;
  a.tabla = tabla_a;
  a.listas = 0;
  a.entradas = Leer32(base, tabla_a + 64);
  a.r23 = Leer32(base, self + 264);
  a.r24 = Leer32(base, self + 296);
  // r27: the table of the shared group (cntlzw/or/rlwinm/and/andc of the original).
  const uint32_t tabla_b = (Leer32(base, self + 696) != 0 && Leer32(base, 0x828E0B40u) != 0)
                               ? Leer32(base, 0x8290E564u)
                               : tabla_a;
  GrupoEfecto b;
  b.cuenta = Leer32(base, self + 292);
  b.sucios = Leer32(base, self + 256);
  b.tabla = tabla_b;
  b.listas = 32;
  b.entradas = Leer32(base, tabla_b + 68);
  b.r23 = Leer32(base, Leer32(base, self + 268));
  b.r24 = Leer32(base, Leer32(base, self + 300));
  if (HayLlamadas(base, a) || HayLlamadas(base, b)) {
    return false;
  }
  const uint32_t dispositivo = Leer32(base, self + 700);
  vectores += ListaFlotante(w, a, dispositivo, 0);
  vectores += ListaFlotante(w, a, dispositivo, 1);
  vectores += ListaFlotante(w, b, dispositivo, 0);
  vectores += ListaFlotante(w, b, dispositivo, 1);
  w.Cero128(self & ~127u);                      // dcbzl r0,r20
  w.Cero128(Leer32(base, self + 256) & ~127u);  // lwz r11,256(r20); dcbzl r0,r11
  return true;
}

// State and counts of the full path (nfsmw_d3d_efectos_nativo_todo, further down).
// Like the other counters in this file, written by the drawing thread, without atomic
// read-modify-write.
struct RecuentoTodo {
  std::atomic<uint64_t> nativas{0};      // calls done entirely in native code (checked ones included)
  std::atomic<uint64_t> con_listas{0};   // of those, with work in lists 2-7 (formerly left to the original)
  std::atomic<uint64_t> vectores{0};     // constantes de coma flotante copiadas
  std::atomic<uint64_t> enteros{0};      // entradas de constantes enteras
  std::atomic<uint64_t> booleanos{0};    // entradas de constantes booleanas
  std::atomic<uint64_t> texturas{0};     // SetTexture calls done in native code
  std::atomic<uint64_t> raras{0};        // texture releases (done by the original)
  std::atomic<uint64_t> varios{0};       // integers of 2-4 registers (vectors 2-4 from the original's stack)
  std::atomic<uint64_t> abandonadas{0};  // checks abandoned because of a texture release
  std::atomic<uint64_t> llenas{0};       // checks abandoned because the layer is full (more than 8 KB written)
};
RecuentoTodo g_todo;
constexpr uint32_t kComprobacionesTodo = 512;        // first calls of the full path that are checked
constexpr uint64_t kPeriodoTodo = 4096;              // then 1 of every kPeriodoTodo
std::atomic<bool> g_todo_apagado{false};             // a difference turns off only the full path
std::atomic<bool> g_todo_pendiente{false};           // the last check was abandoned: check the next one
std::atomic<uint32_t> g_todo_comprobadas{0};         // comprobaciones completas, sin diferencias
std::atomic<uint32_t> g_todo_comprobadas_listas{0};  // of those, with integers, booleans or textures
std::atomic<uint64_t> g_todo_llamadas{0};

void InformeEfectos() {
  const int64_t ahora = AhoraNs() / 1000000;
  const int64_t siguiente = g_efectos_siguiente_ms.load(std::memory_order_relaxed);
  if (ahora < siguiente) {
    return;
  }
  g_efectos_siguiente_ms.store(ahora + 10000, std::memory_order_relaxed);
  if (siguiente == 0) {
    return;
  }
  const uint64_t nativas = g_efectos_nativas.exchange(0, std::memory_order_relaxed);
  const uint64_t originales = g_efectos_originales.exchange(0, std::memory_order_relaxed);
  const uint64_t vectores = g_efectos_vectores.exchange(0, std::memory_order_relaxed);
  const uint64_t todo = g_todo.nativas.exchange(0, std::memory_order_relaxed);
  if (nativas != 0 || originales != 0 || todo == 0) {  // the lists 0-1 path (floating-point constants only)
    REXLOG_INFO("[efectos] ultimos 10 s: {} en nativo ({:.1f} vectores cada una), {} por la original "
                "(enteros/booleanos){}; comprobadas contra la original: {} de {}",
                nativas, nativas ? double(vectores) / double(nativas) : 0.0, originales,
                g_efectos_apagado.load(std::memory_order_relaxed) ? " | APAGADO por diferencia" : "",
                g_efectos_comprobadas.load(std::memory_order_relaxed), kComprobaciones);
  }
  if (todo != 0 || g_todo_apagado.load(std::memory_order_relaxed)) {  // The full path
    const uint64_t vectores_todo = g_todo.vectores.exchange(0, std::memory_order_relaxed);
    const uint64_t varios = g_todo.varios.exchange(0, std::memory_order_relaxed);
    NFSMW_INFORME_DIFERIDO("[efectos] camino completo, ultimos 10 s: {} en nativo ({:.1f} vectores cada una; {} con listas "
                "2-7: {} enteros, {} booleanos, {} texturas, {} liberaciones por la original{}){}; comprobadas "
                "contra la original desde el arranque: {} ({} con listas 2-7; las primeras {} y luego 1 de cada "
                "{}); abandonadas en estos 10 s: {} por una liberacion y {} por capa llena",
                todo, todo ? double(vectores_todo) / double(todo) : 0.0,
                g_todo.con_listas.exchange(0, std::memory_order_relaxed),
                g_todo.enteros.exchange(0, std::memory_order_relaxed),
                g_todo.booleanos.exchange(0, std::memory_order_relaxed),
                g_todo.texturas.exchange(0, std::memory_order_relaxed),
                g_todo.raras.exchange(0, std::memory_order_relaxed),
                varios ? fmt::format(", {} enteros de 2-4 registros", varios) : std::string(),
                g_todo_apagado.load(std::memory_order_relaxed) ? " | APAGADO por diferencia" : "",
                g_todo_comprobadas.load(std::memory_order_relaxed),
                g_todo_comprobadas_listas.load(std::memory_order_relaxed), kComprobacionesTodo, kPeriodoTodo,
                g_todo.abandonadas.exchange(0, std::memory_order_relaxed),
                g_todo.llenas.exchange(0, std::memory_order_relaxed));
  }
}
}  // namespace

REX_EXTERN(__imp__sub_826992F0);

// ---------------------------------------------------------------------------------------------------
// The whole of sub_826992F0 in native code (cvar nfsmw_d3d_efectos_nativo_todo).
//
// WHY
//   53 % of its ~72,000 calls/s had something in lists 2-7 (integers, booleans or textures) and went
//   entirely through the original: ~7 % of a core of the game thread in stack sampling. Lists 2-7
//   and the five small D3D functions they call are now handled here.
//
// WHAT EACH LIST DOES (PowerPC read instruction by instruction)
//   0 and 1  VS and PS floating-point constants: the same as the lists 0-1 path.
//   2 and 3  integer constants. vctsxs (float -> int32, truncating, saturated) of the parameter's
//            vector; its word 0 and words 1-3 of the default value table ([table+80] +
//            16 * byte 13 or 12 of the entry; vrlimi128 with mask 7) form a vector on the stack, from
//            which the D3D function (8259_B6B8 VS, 8259_B708 PS) stores (byte 11 << 16) | (byte 7 << 8) |
//            byte 3 into word 2536 (or 2552) + device register, and sets bit 0x80000000 of its dirty
//            mask (+32).
//   4 and 5  boolean constants. vctuxs (float -> uint32, saturated) of the vector; the D3D function
//            (8259_B578 VS, 8259_B5D0 PS) moves bit 0 of each word to bit (register & 31) of word
//            2528 (or 2532) + register / 32, and sets the same bit 0x80000000 of +32.
//   6 and 7  textures: the D3D SetTexture (8258_A648) with the pointer from the parameter's table. Its
//            rare call, releasing the texture that leaves the slot when it runs out of uses
//            (sub_82594C70), is left to the original, with the stack and registers it would have.
//   At the end, dcbzl of the effect's two dirty lines, as in the lists 0-1 path.
//
// WHY IT IS BIT-IDENTICAL
//   - Same order of reads and writes: group A list by list and then group B. Whatever the original
//     rereads on each word (the word count, the list and entry pointers, the dirty mask) and on each
//     entry is reread; what it keeps in registers on entry (device, table A, group B dirty mask,
//     r23/r24 of both groups) is read once; the group B table, after group A.
//   - The conversions are the same SDK functions the recompiled code uses
//     (rex::ppc::simde_mm_vctsxs and simde_mm_vctuxs, with the same lvx128 load and the same
//     vrlimi128), and enableFlushMode() is called before each one, as the original does, to leave
//     the thread in the same mode.
//   - The rest are 32-bit integers with the same rlwinm/rlwimi masks. The comparison in SetTexture's
//     rlwinm. is 32-bit, as the recompiled code does it.
//   - Integers of 2-4 registers: the D3D function reads vectors 2-4 from the original's stack
//     (sp - 448 + 176 ... + 223), where it does not write; those same bytes are read from the guest stack.
//   - r3 on return is what the original would leave (this, the device or whatever SetTexture leaves):
//     no caller reads it (all 24 call sites checked), but two tail jumps to this function
//     (8244_8730 and 8244_ED38) pass it on to whoever called them.
//   - No new hook: the D3D functions are done in here and the game's other calls to them do not
//     change. Their addresses are split with "_" on purpose: tools/llamadas_directas.py treats any
//     82xxxxxx address that appears in the sources as hooked and would stop making them direct calls.
//   - Not used with nfsmw_d3d_trace on: SetTexture has its trace hook there, which would not see
//     calls from here (its NotificarVideo does nothing with SetTexture).
//
// SELF-CHECKING GUARD (extended)
//   The first kComprobacionesTodo calls and then 1 of every kPeriodoTodo: the native version runs with
//   its writes in a separate layer (it does not touch game memory or the floating-point mode, and calls
//   nothing), then the original runs over the untouched memory, and the results are compared: every
//   byte written by the native version, the device area the original can write (+16..+10272 and the
//   texture pointers +12704..+12832: that way anything the original writes and the native version does
//   not also shows up), r3, r1 and the floating-point mode. If the SetTexture release is needed (or the
//   layer fills up: more than 8 KB written), that check is abandoned (the original does it) and the next
//   one is checked. A difference turns off only the full path and the lists 0-1 path comes back, with
//   its own guard ("[efectos] DIFERENCIA del camino completo" line in the log).
// ---------------------------------------------------------------------------------------------------
#include <rex/ppc/intrinsics.h>

REXCVAR_DEFINE_BOOL(nfsmw_d3d_efectos_nativo_todo, true, "NFSMW",
                    "sub_826992F0 ENTERA en nativo (build 171): tambien las constantes enteras y booleanas y "
                    "las texturas (SetTexture). Se comprueba contra la original (las primeras 512 llamadas y "
                    "despues 1 de cada 4096) y se apaga sola si difiere; false = como la build 154 (solo las "
                    "constantes de coma flotante). Necesita nfsmw_d3d_efectos_nativo")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, nfsmw_d3d_trace);

REX_EXTERN(__imp__sub_82594C70);

namespace {

// rlwinm/rlwimi: 32-bit rotation (N from 1 to 31).
template <unsigned N>
inline uint32_t Rotl32(uint32_t x) {
  static_assert(N > 0 && N < 32, "rotacion de 1 a 31");
  return (x << N) | (x >> (32 - N));
}

inline uint32_t LeerGrande32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// Memory for the full path, direct: the game's.
struct MemoriaDirecta {
  static constexpr bool kComprobando = false;
  uint8_t* base;
  PPCContext* ctx;

  uint8_t L8(uint32_t d) const { return *Puntero(base, d); }
  uint32_t L32(uint32_t d) const { return Leer32(base, d); }
  uint64_t L64(uint32_t d) const { return Leer64(base, d); }
  void Leer16(uint32_t d, uint8_t* destino) const { std::memcpy(destino, Puntero(base, d), 16); }
  void E32(uint32_t d, uint32_t v) { Escribir32(base, d, v); }
  void E64(uint32_t d, uint64_t v) {
    v = __builtin_bswap64(v);
    std::memcpy(Puntero(base, d), &v, 8);
  }
  void Copiar16(uint32_t destino, uint32_t origen) {
    std::memmove(Puntero(base, destino), Puntero(base, origen), 16);
  }
  void Cero128(uint32_t d) { std::memset(Puntero(base, d), 0, 128); }
  void ModoVectorial() { ctx->fpscr.enableFlushMode(); }
  bool Llena() const { return false; }
};

// Memory for the check: reads the game's and writes to a separate layer (a byte table keyed by address);
// that way the native version touches nothing and the original then runs over the untouched memory.
struct MemoriaCapa {
  static constexpr bool kComprobando = true;
  static constexpr uint32_t kHuecos = 16384;        // power of 2 (one game call writes ~0.5 KB)
  static constexpr uint32_t kLimite = kHuecos / 2;  // bytes written; beyond this the check is abandoned
  uint8_t* base;
  PPCContext* ctx;
  std::vector<uint32_t> direcciones = std::vector<uint32_t>(kHuecos, 0u);  // direccion + 1; 0 = libre
  std::vector<uint8_t> valores = std::vector<uint8_t>(kHuecos, 0);
  uint32_t usados = 0;
  bool modo_vectorial = false;

  uint32_t Hueco(uint32_t d) const {
    uint32_t h = (d * 2654435761u) >> 18;  // 14 bits
    while (direcciones[h] != 0 && direcciones[h] != d + 1) {
      h = (h + 1) & (kHuecos - 1);
    }
    return h;
  }
  bool Escrito(uint32_t d) const { return direcciones[Hueco(d)] != 0; }
  void Leer(uint32_t d, uint8_t* destino, uint32_t n) const {
    const uint8_t* p = Puntero(base, d);
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t h = Hueco(d + i);
      destino[i] = direcciones[h] != 0 ? valores[h] : p[i];
    }
  }
  void Escribir(uint32_t d, const uint8_t* origen, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t h = Hueco(d + i);
      if (direcciones[h] == 0) {
        direcciones[h] = d + i + 1;
        ++usados;
      }
      valores[h] = origen[i];
    }
  }
  uint8_t L8(uint32_t d) const {
    uint8_t b;
    Leer(d, &b, 1);
    return b;
  }
  uint32_t L32(uint32_t d) const {
    uint8_t b[4];
    Leer(d, b, 4);
    return LeerGrande32(b);
  }
  uint64_t L64(uint32_t d) const {
    uint8_t b[8];
    Leer(d, b, 8);
    return (uint64_t(LeerGrande32(b)) << 32) | LeerGrande32(b + 4);
  }
  void Leer16(uint32_t d, uint8_t* destino) const { Leer(d, destino, 16); }
  void E32(uint32_t d, uint32_t v) {
    const uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    Escribir(d, b, 4);
  }
  void E64(uint32_t d, uint64_t v) {
    E32(d, uint32_t(v >> 32));
    E32(d + 4, uint32_t(v));
  }
  void Copiar16(uint32_t destino, uint32_t origen) {
    uint8_t b[16];
    Leer(origen, b, 16);
    Escribir(destino, b, 16);
  }
  void Cero128(uint32_t d) {
    const uint8_t cero[128] = {};
    Escribir(d, cero, 128);
  }
  void ModoVectorial() { modo_vectorial = true; }  // the original will set it; the thread is not touched here
  bool Llena() const { return usados > kLimite; }
};

// lvx128 exactly as the codegen translates it: 16 aligned bytes, reversed (lane 3 is word 0).
template <typename M>
inline simde__m128i CargarVector(const M& m, uint32_t direccion) {
  alignas(16) uint8_t crudo[16];
  m.Leer16(direccion & ~0xFu, crudo);
  return simde_mm_shuffle_epi8(simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(crudo)),
                               simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(VectorMaskL)));
}

// stvx as is: the 16 bytes as they would be on the guest stack (here, in an aligned buffer).
inline void GuardarVector(uint8_t* destino, simde__m128i v) {
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(destino),
                       simde_mm_shuffle_epi8(v, simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(VectorMaskL))));
}

struct GrupoTodo {
  uint32_t cuenta;    // address of the word count (this + 288 or + 292): reread on every word
  uint32_t sucios;    // dirty mask: this (group A) or [this+256] read on entry (group B)
  uint32_t tabla;     // r28 (grupo A) o r27 (grupo B)
  uint32_t listas;    // offset of list 0 in the table: 0 or 32
  uint32_t entradas;  // address of the entry pointer (table + 64 or + 68): reread on every word
  uint32_t r23;
  uint32_t r24;
};

template <typename M>
struct Todo {
  M m;
  uint32_t self = 0;         // r3 on entry (r20 and r22 of the original)
  uint32_t pila = 0;         // r1 al entrar
  uint32_t dispositivo = 0;  // r21 = [this+700]
  uint64_t r3 = 0;           // what the original would leave in r3
  bool toco_dispositivo = false;
  uint32_t enteros = 0;
  uint32_t booleanos = 0;
  uint32_t texturas = 0;
  uint32_t raras = 0;
  uint32_t varios = 0;
  uint64_t vectores = 0;
};

// Lists 0 and 1: VS floating-point constants (register + 120, dirty +16) and PS ones (+376, +24).
// The same as ListaFlotante, entry by entry.
template <typename M>
void EntradaFlotante(Todo<M>& c, const GrupoTodo& g, uint32_t e, bool ps) {
  M& m = c.m;
  const uint32_t campo = ps ? 8 : 4;
  const uint32_t sesgo = ps ? 376 : 120;
  const uint32_t sucios_dispositivo = c.dispositivo + (ps ? 24 : 16);
  const uint32_t w0 = m.L32(e);
  uint32_t wc = m.L32(e + campo);
  const uint32_t t = g.r23 + (Rotl32<17>(w0) & 0x1FFF8u);
  const uint32_t idx = m.L32(t + 4);
  const uint32_t origen = g.r24 + (Rotl32<4>(idx) & 0xFFFF0u);
  const uint32_t destino = c.dispositivo + ((((wc & 0x3FFu) + sesgo) << 4) & 0xFFFFFFF0u);
  uint32_t j = 0;
  do {
    m.Copiar16((destino + j * 16) & ~0xFu, (origen + j * 16) & ~0xFu);  // lvx128 + stvx128: plain copy
    ++c.vectores;
    ++j;
    wc = m.L32(e + campo);
  } while (j <= ((wc >> 10) & 3u));
  wc = m.L32(e + campo);
  const uint64_t lo = wc & 0x3FFu;
  const uint64_t a = (lo >> 2) & 0x3FFFFFFFu;                       // r10
  const uint64_t b = ((((wc >> 10) & 3u) + lo) >> 2) & 0x3FFFFFFFu;  // r9
  const uint64_t d = (b - a) & 0xFFFFFFFFu;
  uint64_t s = d & 0x7F;
  if (s > 0x3F) {
    s = 0x3F;
  }
  const uint64_t bits = uint64_t(int64_t(uint64_t(1) << 63) >> s);  // srad r9,r18,r9
  const uint8_t a8 = uint8_t(a);
  const uint64_t v = (a8 & 0x40) ? 0 : (bits >> (a8 & 0x7F));        // srd r10,r9,r10
  m.E64(sucios_dispositivo, v | m.L64(sucios_dispositivo));
}

// Lists 2 and 3: VS and PS integer constants, with the D3D function that stores them.
template <typename M>
void EntradaEntera(Todo<M>& c, const GrupoTodo& g, uint32_t e, bool ps) {
  M& m = c.m;
  const uint32_t w0 = m.L32(e + 0);                                // lwz r11,0(r31)
  const uint32_t defecto = m.L8(e + (ps ? 12 : 13));               // lbz r10,13(r31) o 12(r31)
  const uint32_t tabla_defecto = m.L32(g.tabla + 80);              // lwz r9,80(r28) o 80(r27)
  const uint32_t t = (Rotl32<17>(w0) & 0x1FFF8u) + g.r23;
  const uint32_t idx = m.L32(t + 4);
  const uint32_t origen = (Rotl32<4>(idx) & 0xFFFF0u) + g.r24;
  // lvx128 v0 / vctsxs v0,v0,0 / lvx128 v13 / vrlimi128 v0,v13,7,0 / stvx v0 (original's stack + 160):
  // word 0 converted and words 1-3 from the default value table, with the recompiled code's intrinsics.
  m.ModoVectorial();
  const simde__m128i v0 = rex::ppc::simde_mm_vctsxs(simde_mm_castsi128_ps(CargarVector(m, origen)));
  const simde__m128i v13 = CargarVector(m, (defecto << 4) + tabla_defecto);
  alignas(16) uint8_t vector[16];
  GuardarVector(vector, simde_mm_castps_si128(simde_mm_blend_ps(
                            simde_mm_castsi128_ps(v0), simde_mm_permute_ps(simde_mm_castsi128_ps(v13), 228), 7)));
  const uint32_t wc = m.L32(e + (ps ? 8 : 4));                     // lwz r11,4(r31) o 8(r31)
  const uint32_t registro = Rotl32<20>(wc) & 0xFFu;                // rlwinm r4,r11,20,24,31
  const uint32_t cuenta = (Rotl32<12>(wc) & 3u) + 1;               // rlwinm r10,r11,12,30,31; addi r6,r10,1
  // The D3D function: per register, (byte 11 << 16) | (byte 7 << 8) | byte 3 of a 16-byte vector.
  uint32_t destino = ((registro + (ps ? 2552u : 2536u)) << 2) + c.dispositivo;
  for (uint32_t i = 0; i < cuenta; ++i) {
    uint32_t b3, b7, b11;
    if (i == 0) {
      b3 = vector[3];
      b7 = vector[7];
      b11 = vector[11];
    } else {
      // Vectors 2-4: the original's stack (sp - 448 + 160 + 16 i), where it writes nothing.
      const uint32_t p = c.pila - 448 + 160 + 16 * i;
      b11 = m.L8(p + 11);
      b7 = m.L8(p + 7);
      b3 = m.L8(p + 3);
    }
    m.E32(destino, (b11 << 16) | (b7 << 8) | b3);
    destino += 4;
  }
  m.E64(c.dispositivo + 32, m.L64(c.dispositivo + 32) | 0x80000000ull);  // ld; oris r10,r10,32768; std
  c.r3 = c.dispositivo;                                                  // mr r3,r21
  ++c.enteros;
  if (cuenta > 1) {
    ++c.varios;
  }
}

// Lists 4 and 5: VS and PS boolean constants, with the D3D function that stores them.
template <typename M>
void EntradaBooleana(Todo<M>& c, const GrupoTodo& g, uint32_t e, bool ps) {
  M& m = c.m;
  const uint32_t w0 = m.L32(e + 0);
  const uint32_t t = (Rotl32<17>(w0) & 0x1FFF8u) + g.r23;
  const uint32_t idx = m.L32(t + 4);
  const uint32_t origen = (Rotl32<4>(idx) & 0xFFFF0u) + g.r24;
  // lvx128 v0 / vctuxs v0,v0,0 / stvx v0 (the original's stack + 256).
  m.ModoVectorial();
  alignas(16) uint8_t vector[16];
  GuardarVector(vector, rex::ppc::simde_mm_vctuxs(simde_mm_castsi128_ps(CargarVector(m, origen))));
  const uint32_t wc = m.L32(e + (ps ? 8 : 4));                     // lwz r11,4(r31) o 8(r31)
  uint32_t registro = m.L8(e + (ps ? 14 : 15));                    // lbz r4,15(r31) o 14(r31)
  const uint32_t cuenta = (Rotl32<2>(wc) & 3u) + 1;                // rlwinm r11,r11,2,30,31; addi r6,r11,1
  // The D3D function: bit 0 of each word of the vector goes to bit (register & 31) of word
  // 2528 (VS) or 2532 (PS) + register / 32 of the device.
  for (uint32_t i = 0; i < cuenta; ++i, ++registro) {
    const uint32_t valor = LeerGrande32(vector + 4 * i) & 1u;
    const uint32_t bit = registro & 31u;
    const uint32_t direccion = (((Rotl32<27>(registro) & 0x07FFFFFFu) + (ps ? 2532u : 2528u)) << 2) + c.dispositivo;
    const uint32_t antes = m.L32(direccion);
    m.E32(direccion, (antes & ~(1u << bit)) | (valor << bit));
  }
  m.E64(c.dispositivo + 32, m.L64(c.dispositivo + 32) | 0x80000000ull);
  c.r3 = c.dispositivo;
  ++c.booleanos;
}

// The D3D SetTexture (8258_A648) in native code, in the original's order. Returns false if the texture
// leaving the slot must be released while checking (a call cannot be undone).
template <typename M>
bool SetTextureNativo(Todo<M>& c, uint32_t hueco, uint32_t textura) {
  // With MemoriaDirecta, a local copy (two pointers nobody changes): that way the compiler knows that guest
  // writes do not touch it and keeps base in a register instead of rereading it from c.m after every write
  // (15 fewer loads per call). MemoriaCapa (the check) stays by reference: it holds state.
  std::conditional_t<M::kComprobando, M&, M> m = c.m;
  const uint32_t dispositivo = c.dispositivo;
  const uint32_t r30 = (hueco + 3176) << 2;                          // addi; rlwinm r30,r11,2,0,29
  const uint32_t anterior = m.L32(r30 + dispositivo);                // lwzx r3,r30,r7
  if constexpr (!M::kComprobando) {
    // Cache hints (PRFM: they neither read, write nor fault; they change neither memory, registers nor the
    // order of the real reads and writes). The texture leaving the slot is read and written at the end
    // ([anterior] and [anterior+4]), about 140 instructions after reading the new one: further than the A57
    // window (128), so if both missed they were paid in series. And the new object is read from +0 to +36: if
    // it crosses a cache line, the second line is requested right away. The check (MemoriaCapa) does not use
    // them.
    if (anterior != 0) {
      __builtin_prefetch(Puntero(m.base, anterior), 1);
    }
    if (textura != 0) {
      __builtin_prefetch(Puntero(m.base, textura + 36));
    }
  }
  if (textura != 0) {
    const uint32_t comun = m.L32(textura + 0);                       // lwz r10,0(r5)
    const uint32_t t16 = m.L32(textura + 16);                        // lwz r8,16(r5)
    const uint32_t fc = (hueco + 48) * 24 + dispositivo;             // texture fetch constant of the slot
    m.E32(textura + 0, comun + 0x80000u);                            // stw r10,0(r5)
    const uint32_t fc0 = m.L32(fc + 0);                              // lwz r10,0(r11)
    const uint32_t fc4 = m.L32(fc + 4);                              // lwz r29,4(r11)
    uint32_t r8 = (fc0 & 0x3FFC00u) | (t16 & 0xFFC003FFu);           // rlwimi r8,r10,0,10,21
    const uint32_t fc12 = m.L32(fc + 12);                            // lwz r10,12(r11)
    const uint32_t fc16 = m.L32(fc + 16);                            // lwz r28,16(r11)
    const uint32_t fc20 = m.L32(fc + 20);                            // lwz r27,20(r11)
    m.E32(fc + 0, r8);                                               // stw r8,0(r11)
    r8 = m.L32(textura + 20);                                        // lwz r8,20(r5)
    uint32_t r6 = ((Rotl32<12>(r8) & 0xFFFu) + 512) & 0x1000u;       // rlwinm; addi; rlwinm r6,r6,0,19,19
    r8 = r6 + r8;
    r6 = fc12;                                                       // mr r6,r10
    r8 = (fc4 & 0xE0000000u) | (r8 & 0x1FFFFFFFu);                   // rlwimi r8,r29,0,0,2
    r8 = (fc4 & 0x800u) | (r8 & 0xFFFFF7FFu);                        // rlwimi r8,r29,0,20,20
    const uint32_t r29 = fc16;                                       // rotlwi r29,r28,0
    m.E32(fc + 4, r8);                                               // stw r8,4(r11)
    m.E32(fc + 8, m.L32(textura + 24));                              // lwz r8,24(r5); stw r8,8(r11)
    r8 = m.L32(textura + 28);                                        // lwz r8,28(r5)
    m.E32(fc + 16, fc16);                                            // stw r28,16(r11)
    r8 = (fc12 & 0x7FF80000u) | (r8 & 0x8007FFFFu);                  // rlwimi r8,r10,0,1,12
    uint32_t r10 = fc12;
    r10 = (Rotl32<31>(r6) & 0x0007FFFFu) | (r10 & 0xFFF80000u);      // rlwimi r10,r6,31,13,31
    r10 = (Rotl32<31>(r6) & 0x7FF00000u) | (r10 & 0x800FFFFFu);      // rlwimi r10,r6,31,1,11
    m.E32(fc + 12, r8);                                              // stw r8,12(r11)
    r6 = Rotl32<13>(r10) & 0xFFFu;                                   // rlwinm r6,r10,13,20,31
    r8 = m.L32(textura + 36);                                        // lwz r8,36(r5)
    r10 = (((Rotl32<12>(r8) & 0xFFFu) + 512) & 0x1000u) + r8;
    r10 = (fc20 & 0xE00001FFu) | (r10 & 0x1FFFFE00u);                // rlwimi r10,r27,0,23,2
    m.E32(fc + 20, r10);                                             // stw r10,20(r11)
    const uint32_t r9 = dispositivo + hueco;                         // add r9,r7,r4
    r10 = m.L8(r9 + 12098);                                          // lbz r10,12098(r9)
    const uint32_t r8b = (r10 >> 2) - 1u;                            // rlwinm r8,r10,30,2,31; addi r8,r8,-1
    r6 = r6 & r8b;                                                   // and r6,r6,r8
    r10 = (r10 & ~r8b) + r6;                                         // andc r10,r10,r8; add r10,r6,r10
    r10 = (r29 & 0xFFFFFFFCu) | (r10 & 3u);                          // rlwimi r10,r29,0,0,29
    m.E32(fc + 16, r10);                                             // stw r10,16(r11)
    const uint32_t r6b = Rotl32<30>(m.L32(textura + 32)) & 0xFu;     // lwz r6,32(r5); rlwinm r6,r6,30,28,31
    uint32_t r8c = m.L8(r9 + 12046);                                 // lbz r8,12046(r9)
    if (r6b > r8c) {                                                 // cmplw cr6,r6,r8; ble
      r8c = r6b;
    }
    r10 = (Rotl32<2>(r8c) & 0x3Cu) | (r10 & 0xFFFFFFC3u);            // rlwimi r10,r8,2,26,29
    m.E32(fc + 16, r10);                                             // stw r10,16(r11)
    uint32_t r8d = Rotl32<26>(m.L32(textura + 32)) & 0xFu;           // lwz r8,32(r5); rlwinm r8,r8,26,28,31
    const uint32_t r9b = m.L8(r9 + 12072);                           // lbz r9,12072(r9)
    if (!(r8d < r9b)) {                                              // cmplw cr6,r8,r9; blt
      r8d = r9b;
    }
    r10 = (Rotl32<6>(r8d) & 0x3C0u) | (r10 & 0xFFFFFC3Fu);           // rlwimi r10,r8,6,22,25
    m.E32(fc + 16, r10);                                             // stw r10,16(r11)
    const uint32_t h8 = hueco & 0xFFu;                               // srd r11,r4,r6 con r6 = hueco
    const uint64_t bit = (h8 & 0x40u) ? 0 : ((uint64_t(1) << 63) >> (h8 & 0x7Fu));
    m.E64(dispositivo + 32, bit | m.L64(dispositivo + 32));          // ld r10,16(r9); or; std r11,16(r9)
  }
  m.E32(r30 + dispositivo, textura);                                 // stwx r5,r30,r7
  c.r3 = anterior;
  if (anterior == 0) {
    return true;
  }
  const uint32_t valla = m.L32(dispositivo + 10396);                 // lwz r10,10396(r7)
  const uint32_t comun = m.L32(anterior + 0) - 0x80000u;             // lwz r11,0(r3); subf r11,r31,r11
  m.E32(anterior + 4, valla);                                        // stw r10,4(r3)
  m.E32(anterior + 0, comun);                                        // stw r11,0(r3)
  // clrlwi r10,r11,8; rlwinm. r10,r10,0,24,12; bne: the recompiled code compares the low 32 bits.
  if ((comun & 0x00FFFFFFu & 0xFFF800FFu) != 0) {
    return true;
  }
  ++c.raras;
  if constexpr (M::kComprobando) {
    return false;
  } else {
    // The release is done by the game, with the stack and registers it would have: r1 in SetTexture's
    // frame (448 + 128 bytes below the entry one, with both frame back links), r3 the texture
    // and lr the return address inside SetTexture. It only reads r3 (per its PowerPC).
    PPCContext& ctx = *m.ctx;
    uint8_t* const base = m.base;
    const uint64_t r1 = ctx.r1.u64;
    Escribir32(base, c.pila - 448, c.pila);              // stwu r1,-448(r1) of the original
    Escribir32(base, c.pila - 448 - 128, c.pila - 448);  // stwu r1,-128(r1) de SetTexture
    ctx.r1.u32 = c.pila - 448 - 128;
    ctx.r3.u64 = anterior;
    ctx.lr = 0x8258A7C4;                                 // the instruction after its bl
    __imp__sub_82594C70(ctx, base);
    ctx.r1.u64 = r1;
    c.r3 = ctx.r3.u64;
    return true;
  }
}

// Lists 6 and 7: textures (list 7 reads the second word of the entry, like the PS lists).
template <typename M>
bool EntradaTextura(Todo<M>& c, const GrupoTodo& g, uint32_t e, bool ps) {
  M& m = c.m;
  const uint32_t w0 = m.L32(e + 0);                                  // lwz r11,0(r31)
  const uint32_t wc = m.L32(e + (ps ? 8 : 4));                       // lwz r10,4(r31) o 8(r31)
  const uint32_t t = (Rotl32<17>(w0) & 0x1FFF8u) + g.r23;
  const uint32_t hueco = Rotl32<10>(wc) & 0xFFu;                     // rlwinm r4,r10,10,24,31
  const uint32_t idx = m.L32(t + 4);
  const uint32_t textura = m.L32((Rotl32<4>(idx) & 0xFFFF0u) + g.r24);  // lwzx r5,r11,r24
  ++c.texturas;
  return SetTextureNativo(c, hueco, textura);
}

// One list of one group, walked like the original: for each word of the dirty mask AND the list's
// mask, each run of consecutive bits is a set of consecutive 16-byte entries.
template <uint32_t K, typename M>
bool Lista(Todo<M>& c, const GrupoTodo& g) {
  M& m = c.m;
  for (uint32_t i = 0; i < m.L32(g.cuenta); ++i) {
    const uint32_t lista = m.L32(g.tabla + g.listas + 4 * K);
    const uint32_t entradas = m.L32(g.entradas);
    uint64_t mascara = m.L64(g.sucios + 8 * i) & m.L64(lista + 8 * i);
    uint32_t e = entradas + 1024 * i;
    while (mascara != 0) {
      const uint32_t z = uint32_t(__builtin_clzll(mascara));  // cntlzd
      e += z * 16;
      mascara <<= z;
      const uint64_t invertida = ~mascara;
      const uint32_t n = invertida ? uint32_t(__builtin_clzll(invertida)) : 64u;  // bits seguidos, 1..64
      const uint32_t fin = e + n * 16;
      mascara = n >= 64 ? 0 : (mascara << n);
      do {
        c.toco_dispositivo = true;
        if constexpr (K <= 1) {
          EntradaFlotante(c, g, e, K == 1);
        } else if constexpr (K <= 3) {
          EntradaEntera(c, g, e, K == 3);
        } else if constexpr (K <= 5) {
          EntradaBooleana(c, g, e, K == 5);
        } else {
          if (!EntradaTextura(c, g, e, K == 7)) {
            return false;
          }
        }
        if constexpr (M::kComprobando) {
          if (m.Llena()) {
            return false;
          }
        }
        e += 16;
      } while (e < fin);
    }
  }
  return true;
}

template <typename M>
bool RecorrerGrupo(Todo<M>& c, const GrupoTodo& g) {
  return Lista<0>(c, g) && Lista<1>(c, g) && Lista<2>(c, g) && Lista<3>(c, g) && Lista<4>(c, g) &&
         Lista<5>(c, g) && Lista<6>(c, g) && Lista<7>(c, g);
}

// The whole of sub_826992F0. With MemoriaDirecta it never returns false.
template <typename M>
bool EfectosTodo(Todo<M>& c) {
  M& m = c.m;
  const uint32_t self = c.self;
  // What the original reads on entry and keeps in registers or on its stack.
  const uint32_t b_r24 = m.L32(m.L32(self + 300));
  const uint32_t b_r23 = m.L32(m.L32(self + 268));
  c.dispositivo = m.L32(self + 700);
  const uint32_t tabla_a = m.L32(self + 536);
  const uint32_t sucios_b = m.L32(self + 256);
  const GrupoTodo a{self + 288, self, tabla_a, 0, tabla_a + 64, m.L32(self + 264), m.L32(self + 296)};
  if (!RecorrerGrupo(c, a)) {
    return false;
  }
  // r27, the table of the shared group: after group A, as in the original.
  const uint32_t tabla_b =
      (m.L32(self + 696) != 0 && m.L32(0x828E0B40u) != 0) ? m.L32(0x8290E564u) : tabla_a;
  const GrupoTodo b{self + 292, sucios_b, tabla_b, 32, tabla_b + 68, b_r23, b_r24};
  if (!RecorrerGrupo(c, b)) {
    return false;
  }
  m.Cero128(self & ~127u);               // dcbzl r0,r20
  m.Cero128(m.L32(self + 256) & ~127u);  // lwz r11,256(r20); dcbzl r0,r11
  return !m.Llena();
}

template <typename M>
void ContarTodo(const Todo<M>& c) {
  Sumar(g_todo.nativas, uint64_t(1));
  Sumar(g_todo.vectores, c.vectores);
  if (c.enteros + c.booleanos + c.texturas == 0) {
    return;
  }
  Sumar(g_todo.con_listas, uint64_t(1));
  Sumar(g_todo.enteros, uint64_t(c.enteros));
  Sumar(g_todo.booleanos, uint64_t(c.booleanos));
  Sumar(g_todo.texturas, uint64_t(c.texturas));
  Sumar(g_todo.raras, uint64_t(c.raras));
  Sumar(g_todo.varios, uint64_t(c.varios));
}

// Device area the original can write (constant registers < 256 of each class, texture slots < 32):
// dirty masks, texture fetch constants, floating-point, boolean and integer constants (+16..+10272),
// and the texture pointers (+12704..+12832).
constexpr uint32_t kZona1Inicio = 16;
constexpr uint32_t kZona1Fin = 10272;
constexpr uint32_t kZona2Inicio = 12704;
constexpr uint32_t kZona2Fin = 12832;

bool TodoActivo() {
  const bool pedido = REXCVAR_GET(nfsmw_d3d_efectos_nativo_todo);
  const bool traza = REXCVAR_GET(nfsmw_d3d_trace);
  if (pedido && traza) {
    REXLOG_INFO("[efectos] nfsmw_d3d_trace activo: sin el camino completo, para que rex_d3d.log vea todas "
                "las llamadas a SetTexture; se usa el de la build 154");
  }
  return pedido && !traza;
}

void EfectosTodoLlamada(PPCContext& ctx, uint8_t* base) {
  const uint64_t n = g_todo_llamadas.load(std::memory_order_relaxed) + 1;
  g_todo_llamadas.store(n, std::memory_order_relaxed);
  const uint32_t comprobadas = g_todo_comprobadas.load(std::memory_order_relaxed);
  const bool comprobar = comprobadas < kComprobacionesTodo || n % kPeriodoTodo == 0 ||
                         g_todo_pendiente.load(std::memory_order_relaxed);
  if (!comprobar) {
    Todo<MemoriaDirecta> c{MemoriaDirecta{base, &ctx}};
    c.self = ctx.r3.u32;
    c.pila = ctx.r1.u32;
    c.r3 = ctx.r3.u64;
    EfectosTodo(c);  // with direct memory it is never abandoned
    ctx.r3.u64 = c.r3;
    ContarTodo(c);
    return;
  }

  // --- Guard: the native version with its writes in a separate layer, the original, and compare ---
  Todo<MemoriaCapa> c{MemoriaCapa{base, &ctx}};
  c.self = ctx.r3.u32;
  c.pila = ctx.r1.u32;
  c.r3 = ctx.r3.u64;
  if (!EfectosTodo(c)) {
    // The SetTexture release was needed (or the layer filled up): this one is done by the original and
    // the next one is checked.
    Sumar(c.m.Llena() ? g_todo.llenas : g_todo.abandonadas, uint64_t(1));
    g_todo_pendiente.store(true, std::memory_order_relaxed);
    __imp__sub_826992F0(ctx, base);
    return;
  }
  const uint32_t dispositivo = c.dispositivo;
  const bool zona = c.toco_dispositivo && Desplazamiento(dispositivo) == Desplazamiento(dispositivo + kZona2Fin);
  std::vector<uint8_t> antes;  // the device area before the original (the native version has not touched it)
  if (zona) {
    const uint8_t* p = Puntero(base, dispositivo);
    antes.assign(p + kZona1Inicio, p + kZona1Fin);
    antes.insert(antes.end(), p + kZona2Inicio, p + kZona2Fin);
  }
  const uint32_t csr_antes = ctx.fpscr.csr;
  const uint64_t r1_antes = ctx.r1.u64;
  __imp__sub_826992F0(ctx, base);
  ContarTodo(c);

  const MemoriaCapa& capa = c.m;
  const char* que = nullptr;
  uint32_t direccion = 0;
  for (uint32_t h = 0; h < MemoriaCapa::kHuecos && !que; ++h) {
    if (capa.direcciones[h] != 0 && *Puntero(base, capa.direcciones[h] - 1) != capa.valores[h]) {
      que = "byte escrito por la nativa";
      direccion = capa.direcciones[h] - 1;
    }
  }
  if (!que && zona) {
    const uint8_t* p = Puntero(base, dispositivo);
    auto mirar = [&](uint32_t desde, uint32_t hasta, const uint8_t* previo) {
      for (uint32_t i = desde; i < hasta && !que; ++i) {
        if (p[i] != previo[i - desde] && !capa.Escrito(dispositivo + i)) {
          que = "zona del dispositivo escrita por la original y no por la nativa";
          direccion = dispositivo + i;
        }
      }
    };
    mirar(kZona1Inicio, kZona1Fin, antes.data());
    mirar(kZona2Inicio, kZona2Fin, antes.data() + (kZona1Fin - kZona1Inicio));
  }
  if (!que && ctx.r3.u64 != c.r3) {
    que = "r3";
  }
  if (!que && ctx.r1.u64 != r1_antes) {
    que = "r1";
  }
  const uint32_t csr_esperado =
      capa.modo_vectorial ? (csr_antes | uint32_t(PPCFPSCRRegister::FlushMask)) : csr_antes;
  if (!que && ctx.fpscr.csr != csr_esperado) {
    que = "modo de coma flotante";
  }
  if (que) {
    g_todo_apagado.store(true, std::memory_order_relaxed);
    REXLOG_INFO("[efectos] DIFERENCIA del camino completo con la original ({}) en 0x{:08X} (llamada {}, this "
                "0x{:08X}, dispositivo 0x{:08X}; r3 0x{:X} nativa, 0x{:X} original): camino completo APAGADO "
                "para siempre; sigue el de la build 154",
                que, direccion, n, c.self, dispositivo, c.r3, ctx.r3.u64);
    return;
  }
  g_todo_pendiente.store(false, std::memory_order_relaxed);
  if (c.enteros + c.booleanos + c.texturas != 0) {
    Sumar(g_todo_comprobadas_listas, uint32_t(1));
  }
  g_todo_comprobadas.store(comprobadas + 1, std::memory_order_relaxed);
  if (comprobadas + 1 == kComprobacionesTodo) {
    REXLOG_INFO("[efectos] camino completo: {} llamadas comprobadas contra la original byte a byte ({} con "
                "enteros, booleanos o texturas), 0 diferencias; en marcha, y sigue comprobando 1 de cada {}",
                kComprobacionesTodo, g_todo_comprobadas_listas.load(std::memory_order_relaxed), kPeriodoTodo);
  }
}
}  // namespace

namespace {
void EfectosLlamada(PPCContext& ctx, uint8_t* base) {
  static const bool activo = REXCVAR_GET(nfsmw_d3d_efectos_nativo);
  static const bool todo = TodoActivo();
  if (!activo || g_efectos_apagado.load(std::memory_order_relaxed)) {
    __imp__sub_826992F0(ctx, base);
    return;
  }
  // The whole function in native code, with its guard. If that guard turns it off after a difference, the
  // code below takes over: the lists 0-1 path (floating-point constants only), with its own guard.
  if (todo && !g_todo_apagado.load(std::memory_order_relaxed)) {
    EfectosTodoLlamada(ctx, base);
    return;
  }
  const uint32_t self = ctx.r3.u32;
  uint64_t vectores = 0;
  const uint32_t comprobadas = g_efectos_comprobadas.load(std::memory_order_relaxed);
  if (comprobadas >= kComprobaciones) {
    Escritor w{base, nullptr};
    if (EfectosNativo(w, self, vectores)) {
      Sumar(g_efectos_nativas, uint64_t(1));
      Sumar(g_efectos_vectores, vectores);
      return;  // r3 is still this, as in the original without calls
    }
    Sumar(g_efectos_originales, uint64_t(1));
    __imp__sub_826992F0(ctx, base);
    return;
  }

  // --- Guard: native recording, undo, original, compare ---
  std::vector<Deshacer> registro;
  registro.reserve(256);
  Escritor w{base, &registro};
  if (!EfectosNativo(w, self, vectores)) {
    Sumar(g_efectos_originales, uint64_t(1));
    __imp__sub_826992F0(ctx, base);
    return;
  }
  std::vector<uint8_t> nativo;  // what the native version left at each recorded write
  for (const Deshacer& d : registro) {
    const uint8_t* p = Puntero(base, d.direccion);
    nativo.insert(nativo.end(), p, p + d.bytes);
  }
  for (auto it = registro.rbegin(); it != registro.rend(); ++it) {  // undo, from last to first
    std::memcpy(Puntero(base, it->direccion), it->antes, it->bytes);
  }
  __imp__sub_826992F0(ctx, base);
  size_t desplazamiento = 0;
  bool igual = true;
  uint32_t direccion_mala = 0;
  // nativo[] was read after the whole native run: it is its final state at every address it touched.
  for (size_t i = 0; i < registro.size() && igual; ++i) {
    const Deshacer& d = registro[i];
    if (std::memcmp(Puntero(base, d.direccion), nativo.data() + desplazamiento, d.bytes) != 0) {
      igual = false;
      direccion_mala = d.direccion;
    }
    desplazamiento += d.bytes;
  }
  if (!igual) {
    g_efectos_apagado.store(true, std::memory_order_relaxed);
    REXLOG_INFO("[efectos] DIFERENCIA con la original en 0x{:08X} (llamada {} de la comprobacion, this "
                "0x{:08X}): camino nativo APAGADO para siempre; se queda la original",
                direccion_mala, comprobadas + 1, self);
    return;
  }
  g_efectos_comprobadas.store(comprobadas + 1, std::memory_order_relaxed);
  if (comprobadas + 1 == kComprobaciones) {
    REXLOG_INFO("[efectos] {} llamadas comprobadas contra la original byte a byte, 0 diferencias: camino "
                "nativo en marcha",
                kComprobaciones);
  }
}
}  // namespace

REX_HOOK_RAW(sub_826992F0) {  // upload of effect parameters to the device
  Medir(g_m_efecto, [&] { EfectosLlamada(ctx, base); });
  if ((g_m_efecto.llamadas.load(std::memory_order_relaxed) & 4095) == 0) {
    InformeEfectos();
  }
}

// ---------------------------------------------------------------------------------------------------
// Phase 2 of the Direct3D-level renderer: the composite FlushState marker. See docs/native-renderer.md.
//
// WHAT CHANGES
//   On every draw, FlushState (825A40C0) dumps the dirty registers of the device mirror with sub_825A2AA0
//   (groups 0x2000-0x2380 and booleans), sub_825A2C58 (VS and PS constants) and sub_825A2B60 (fetch):
//   ~7 type-0 packets and ~6 padding words (type 2) per draw, measured. The PM4 ring thread, at 95 % load,
//   decodes each packet separately: ~2 us per draw on registers alone.
//   With this, FlushState writes a single type-3 NOP packet with all the runs inside (format: kMarcadorMagia
//   in nfsmw_nativo_ganchos.h), at the same place in the ring where the packets would go. The order relative
//   to the IM_LOADs, draws, resolves, fences and the Swap is the same. The data travels inside the ring and
//   not in a separate queue: that way, when the D3D replays a recorded buffer (BeginTiling/EndTiling), the
//   marker is applied again just as the type-0 packets would be, and the pace at which the game notifies
//   the ring does not change. 825A2D80 (streams) and 825A3AF0 (shaders, IM_LOAD) are still called as they
//   are, in the same order.
//
// SELF-CHECKING GUARD
//   Watching phase: the game's dumps run as always and, after their packets, a marker in check mode.
//     - Right here, on the game thread: the packets the dumps have just written must be exactly the
//       marker's runs (register, count, values and order; padding is skipped).
//     - On the PM4 ring thread, when reading the marker: the registers those packets left must hold what
//       the marker says (AnotarComprobacionMarcador). That also covers the recorded buffer replayed later.
//   With kComprobacionesJuego matches here and kComprobacionesAnillo in the ring, and no mismatch, it
//   switches to applying: only the marker. Even then, 1 of every kComprobarCada FlushState calls, and those
//   that carry a group not yet checked kMinimoPorGrupo times, go through the check path again. A single
//   difference on either side turns it off for the rest of the session ("[d3d_marcador] DIFERENCIA" in the
//   log). If the marker does not fit in the space the D3D has reserved ([dev+0] to [dev+4]), the game's
//   dumps run instead, since they know how to request space (sub_825A29E8).
// ---------------------------------------------------------------------------------------------------
REXCVAR_DEFINE_BOOL(nfsmw_d3d_marcador, true, "NFSMW",
                    "Renderizador nativo (25/09, build 170, fase 2 del renderizador a nivel de Direct3D): FlushState "
                    "escribe UN paquete con todos sus registros en vez de ~13 (tipo 0 y relleno). Empieza comprobando "
                    "contra el camino del juego y se apaga solo si algo difiere. false = como antes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, nfsmw_nativo_sombra_d3d);

REX_EXTERN(__imp__sub_825A2D80);  // streams: writes the vertex fetches to the mirror
REX_EXTERN(__imp__sub_825A3AF0);  // shaders: IM_LOAD to the ring
REX_EXTERN(__imp__sub_825A2C58);  // dump of the VS and PS constants
REX_EXTERN(__imp__sub_825A2B60);  // dump of the fetches
REX_EXTERN(__imp__sub_825A29E8);  // ring-full path of the three dumps

namespace {
namespace marcador {

using nfsmw::nativo::kMarcadorAplicar;
using nfsmw::nativo::kMarcadorComprobar;
using nfsmw::nativo::kMarcadorMagia;

// The FlushState groups, in the order it dumps them.
enum Grupo : uint32_t { kG2300, kG2380, kGVs, kGPs, kGFetch, kGBool, kG2000, kG2100, kG2180, kG2200, kG2280, kGrupos };
constexpr const char* kNombreGrupo[kGrupos] = {"0x2300", "0x2380", "VS",     "PS",     "fetch", "bool",
                                               "0x2000", "0x2100", "0x2180", "0x2200", "0x2280"};
// Bound on runs with alternating bits in every mask: 19+4+32+32+16+1+8+11+3+6+11 = 143.
constexpr uint32_t kMaxTramos = 160;
constexpr uint64_t kComprobacionesJuego = 20000;
constexpr uint64_t kComprobacionesAnillo = 20000;
constexpr uint32_t kMinimoPorGrupo = 64;
constexpr uint64_t kComprobarCada = 1024;  // potencia de 2
constexpr uint32_t kRelleno = 0x80000000u;  // type-2 packet 825A2C58 and 825A2B60 use to align their data

struct Tramo {
  uint32_t registro;  // primer registro
  uint32_t cuenta;    // palabras
  uint32_t origen;    // mirror in the device (guest address)
};

struct Volcado {
  Tramo tramos[kMaxTramos];
  uint32_t n;         // tramos
  uint32_t palabras;  // run headers plus values
  uint32_t grupos;    // bit g: group g has something
  bool desbordado;    // should never happen (kMaxTramos); if it does, the game's path
  uint64_t mascara[kGrupos];  // the one the game's dump would get (for the D3D shadow state)
  uint32_t registro_base[kGrupos];
  uint32_t origen_base[kGrupos];
};

// FlushState is called by one thread at a time (the one using the D3D); the ring counters are written only
// by the PM4 ring thread. No atomic read-modify-write (A57 without LSE).
std::atomic<bool> g_consumidor{false};
std::atomic<bool> g_apagado{false};
std::atomic<bool> g_aplicando{false};
std::atomic<uint64_t> g_iguales_juego{0};
std::atomic<uint64_t> g_iguales_anillo{0};
std::atomic<bool> g_distinto_anillo{false};
std::atomic<uint32_t> g_distinto_secuencia{0};
std::atomic<uint32_t> g_distinto_registro{0};
std::atomic<uint32_t> g_distinto_en_registros{0};
std::atomic<uint32_t> g_distinto_en_marcador{0};
std::atomic<uint32_t> g_comprobados_grupo[kGrupos];
std::atomic<uint32_t> g_grupos_listos{0};  // bit g: group g was already checked kMinimoPorGrupo times
std::atomic<uint32_t> g_secuencia{0};
std::atomic<uint64_t> g_turno{0};
std::atomic<uint64_t> g_anillo_lleno{0};  // calls to sub_825A29E8 (ring full during a dump)
// Report every 10 s.
std::atomic<uint64_t> g_i_llamadas{0};
std::atomic<uint64_t> g_i_con_registros{0};
std::atomic<uint64_t> g_i_aplicados{0};
std::atomic<uint64_t> g_i_comprobados{0};
std::atomic<uint64_t> g_i_sin_sitio{0};
std::atomic<uint64_t> g_i_no_comparables{0};
std::atomic<uint64_t> g_i_tramos{0};
std::atomic<uint64_t> g_i_palabras{0};
std::atomic<int64_t> g_i_siguiente_ms{0};

inline void Escribir64(uint8_t* base, uint32_t direccion, uint64_t valor) {
  valor = __builtin_bswap64(valor);
  std::memcpy(Puntero(base, direccion), &valor, 8);
}

// The runs of consecutive bits of a mask, walked like the game's dumps: bit b (counting from the top) is
// register registro_base + b * por_bit and its mirror starts at origen_base + 4 * b * por_bit.
inline void AnadirTramos(Volcado& v, uint32_t g, uint64_t mascara, uint32_t registro_base, uint32_t origen_base,
                         uint32_t por_bit) {
  v.grupos |= 1u << g;
  v.mascara[g] = mascara;
  v.registro_base[g] = registro_base;
  v.origen_base[g] = origen_base;
  uint32_t bit = 0;
  while (mascara != 0) {
    if (v.n == kMaxTramos) {
      v.desbordado = true;
      return;
    }
    const uint32_t z = uint32_t(__builtin_clzll(mascara));
    bit += z;
    mascara <<= z;  // z <= 63: the mask is not zero
    const uint64_t invertida = ~mascara;
    const uint32_t n = invertida ? uint32_t(__builtin_clzll(invertida)) : 64u;
    Tramo& t = v.tramos[v.n++];
    t.registro = registro_base + bit * por_bit;
    t.cuenta = n * por_bit;
    t.origen = origen_base + bit * por_bit * 4;
    v.palabras += 1 + t.cuenta;
    bit += n;
    mascara = n >= 64 ? 0 : (mascara << n);
  }
}

// The same groups, masks and mirrors as FlushState, in its order (read from the PowerPC). The +0x30 block
// only counts if +0x30 was nonzero on entry, as in the original.
inline void Colectar(uint8_t* base, uint32_t dev, bool con_30, Volcado& v) {
  v.n = 0;
  v.palabras = 0;
  v.grupos = 0;
  v.desbordado = false;
  if (con_30) {
    const uint64_t m = Leer64(base, dev + 0x30);
    if (const uint64_t r4 = m & 0xFFFFFFFFFC000000ull) {
      AnadirTramos(v, kG2300, r4, 0x2300, dev + 0x2DF8, 1);
    }
    if (uint32_t(m) & 0x03FC0000u) {
      AnadirTramos(v, kG2380, (m << 38) & 0xFF00000000000000ull, 0x2380, dev + 0x2E90, 1);
    }
  }
  if (const uint64_t m = Leer64(base, dev + 0x10)) {
    AnadirTramos(v, kGVs, m, 0x4000, dev + 0x780, 16);
  }
  if (const uint64_t m = Leer64(base, dev + 0x18)) {
    AnadirTramos(v, kGPs, m, 0x4400, dev + 0x1780, 16);
  }
  if (const uint64_t m = Leer64(base, dev + 0x20)) {
    if (const uint64_t r4 = m & 0xFFFFFFFF00000000ull) {
      AnadirTramos(v, kGFetch, r4, 0x4800, dev + 0x480, 6);
    }
    if (uint32_t(m) & 0x80000000u) {
      AnadirTramos(v, kGBool, 0xFFFFFFFFFF000000ull, 0x4900, dev + 0x2780, 1);
    }
    if (uint32_t(m) & 0x3FFFC000u) {
      AnadirTramos(v, kG2000, (m << 34) & 0xFFFF000000000000ull, 0x2000, dev + 0x2CC0, 1);
    }
  }
  if (const uint64_t m = Leer64(base, dev + 0x28)) {
    if (const uint64_t r4 = m & 0xFFFFF80000000000ull) {
      AnadirTramos(v, kG2100, r4, 0x2100, dev + 0x2D0C, 1);
    }
    if (m & 0x000007C000000000ull) {
      AnadirTramos(v, kG2180, (m << 21) & 0xF800000000000000ull, 0x2180, dev + 0x2D60, 1);
    }
    if (m & 0x0000003FFC000000ull) {
      AnadirTramos(v, kG2200, (m << 26) & 0xFFF0000000000000ull, 0x2200, dev + 0x2D74, 1);
    }
    if (uint32_t(m) & 0x03FFFFE0u) {
      AnadirTramos(v, kG2280, (m << 38) & 0xFFFFF80000000000ull, 0x2280, dev + 0x2DA4, 1);
    }
  }
}

// What FlushState leaves in the device besides the packets: the masks it looked at, set to zero, and, if
// it dumps the booleans, 0xFFFFFFFFFF000000 at +0x2CB0.
inline void LimpiarComoFlushState(uint8_t* base, uint32_t dev, bool con_30, const Volcado& v) {
  if (con_30) {
    Escribir64(base, dev + 0x30, 0);
  }
  for (const uint32_t campo : {0x10u, 0x18u, 0x20u, 0x28u}) {
    if (Leer64(base, dev + campo) != 0) {
      Escribir64(base, dev + campo, 0);
    }
  }
  if (v.grupos & (1u << kGBool)) {
    Escribir64(base, dev + 0x2CB0, 0xFFFFFFFFFF000000ull);
  }
}

// Phase 2b: the kPalabrasDibujo words of the Draw* record: function, VS, PS, the four arguments and the
// snapshot of the D3D shadow state (high and low). Read by LeerDibujoDelMarcador in nfsmw_nativo_sistema.cpp.
inline void EscribirDibujo(uint8_t* base, uint32_t p, const nfsmw::nativo::RegistroDibujo& r) {
  // Bits 8-23 carry the game's vegetation verdict (nfsmw_d3d_vegetacion_juego).
  Escribir32(base, p, uint32_t(r.funcion) | (uint32_t(r.vegetacion) << 8));
  Escribir32(base, p + 4, r.vs);
  Escribir32(base, p + 8, r.ps);
  for (uint32_t i = 0; i < 4; ++i) {
    Escribir32(base, p + 12 + 4 * i, r.args[i]);
  }
  Escribir32(base, p + 28, uint32_t(r.sombra >> 32));
  Escribir32(base, p + 32, uint32_t(r.sombra));
}

// The marker, if it fits entirely in the space the D3D has reserved: it is written from [dev+0]+4 and the
// last word can be [dev+4], the same criterion as the dumps (write + 4 * words < end). [dev+0] is left at
// the last one.
// Phase 2b: with modo_dibujo, the Draw* record goes before the runs; with no dump (v null, mode 0), a
// marker with only the record.
inline bool EscribirMarcador(uint8_t* base, uint32_t dev, const Volcado* v, uint32_t modo, uint32_t secuencia,
                             uint32_t modo_dibujo, const nfsmw::nativo::RegistroDibujo* dibujo) {
  const uint32_t palabras_dibujo = modo_dibujo ? nfsmw::nativo::kPalabrasDibujo : 0u;
  const uint32_t carga = 2 + palabras_dibujo + (v ? v->palabras : 0u);  // words after the PM4 header
  if (carga > 0x4000) {
    return false;
  }
  const uint32_t escritura = Leer32(base, dev + 0);
  const uint32_t fin = Leer32(base, dev + 4);
  if (uint64_t(escritura) + uint64_t(carga) * 4 >= uint64_t(fin)) {
    return false;
  }
  uint32_t p = escritura + 4;
  Escribir32(base, p, 0xC0001000u | ((carga - 1) << 16));
  Escribir32(base, p + 4, kMarcadorMagia | modo | (modo_dibujo << 4));
  Escribir32(base, p + 8, secuencia);
  p += 12;
  if (modo_dibujo) {
    EscribirDibujo(base, p, *dibujo);
    p += palabras_dibujo * 4;
  }
  for (uint32_t i = 0; v && i < v->n; ++i) {
    const Tramo& t = v->tramos[i];
    Escribir32(base, p, (t.cuenta << 16) | t.registro);
    CopiarPalabras(base, p + 4, t.origen, t.cuenta);
    p += 4 + t.cuenta * 4;
  }
  Escribir32(base, dev + 0, p - 4);
  return true;
}

struct Diferencia {
  const char* que = "";
  uint32_t tramo = 0;
  uint32_t registro = 0;
  uint32_t esperado = 0;
  uint32_t visto = 0;
};

// Watching phase: what the game's dumps have just written between desde+4 and hasta (inclusive) must be
// exactly the list of runs: a type-0 header for each run with its register and count, and its values,
// which are those of the mirror. The alignment padding (type 2) is skipped.
inline bool CoincidenPaquetes(uint8_t* base, uint32_t desde, uint32_t hasta, const Volcado& v, Diferencia& d) {
  uint32_t p = desde + 4;
  const uint32_t fin = hasta + 4;
  for (uint32_t i = 0; i < v.n; ++i) {
    const Tramo& t = v.tramos[i];
    for (uint32_t relleno = 0; relleno < 3 && p < fin && Leer32(base, p) == kRelleno; ++relleno) {
      p += 4;
    }
    const uint32_t cabecera = ((t.cuenta - 1) << 16) | t.registro;
    if (p >= fin || Leer32(base, p) != cabecera) {
      d = Diferencia{"cabecera", i, t.registro, cabecera, p < fin ? Leer32(base, p) : 0};
      return false;
    }
    p += 4;
    if ((fin - p) / 4 < t.cuenta) {
      d = Diferencia{"paquete corto", i, t.registro, t.cuenta, (fin - p) / 4};
      return false;
    }
    for (uint32_t k = 0; k < t.cuenta; ++k) {
      const uint32_t visto = Leer32(base, p + 4 * k);
      const uint32_t esperado = Leer32(base, t.origen + 4 * k);
      if (visto != esperado) {
        d = Diferencia{"valor", i, t.registro + k, esperado, visto};
        return false;
      }
    }
    p += 4 * t.cuenta;
  }
  if (p != fin) {
    d = Diferencia{"sobran palabras", v.n, 0, fin, p};
    return false;
  }
  return true;
}

// FlushState's 112-byte frame, opened only when game code has to be called, like the original:
// mflr r12; stw r12,-8(r1); std r30,-24(r1); std r31,-16(r1); stwu r1,-112(r1). With the non-volatile
// registers as locals of the generated code, r30 and r31 are saved as zero.
struct Marco {
  PPCContext& ctx;
  uint8_t* base;
  uint64_t lr;
  uint32_t pila = 0;
  bool abierto = false;
  Marco(PPCContext& c, uint8_t* b) : ctx(c), base(b), lr(c.lr) {}
  Marco(const Marco&) = delete;
  Marco& operator=(const Marco&) = delete;
  void Abrir() {
    if (abierto) {
      return;
    }
    abierto = true;
    pila = ctx.r1.u32;
    Escribir32(base, pila - 8, uint32_t(lr));
    Escribir64(base, pila - 24, 0);
    Escribir64(base, pila - 16, 0);
    Escribir32(base, pila - 112, pila);
    ctx.r1.u64 = pila - 112;
  }
  ~Marco() {
    if (abierto) {
      ctx.r1.u64 = pila;
    }
  }
};

inline void Argumentos(PPCContext& ctx, uint32_t dev, uint64_t mascara, uint32_t registro, uint32_t origen,
                       uint32_t vuelta) {
  ctx.r3.u64 = dev;
  ctx.r4.u64 = mascara;
  ctx.r5.u64 = registro;
  ctx.r6.u64 = origen;
  ctx.lr = vuelta;
}

// The end of FlushState as is (from loc_825A4110): the game's dumps, in their order, with their arguments,
// their return addresses and the masks zeroed after each group.
void VolcarComoElJuego(PPCContext& ctx, uint8_t* base, Marco& marco, uint32_t dev, bool con_30) {
  if (con_30) {
    uint64_t m = Leer64(base, dev + 0x30);
    if (const uint64_t r4 = m & 0xFFFFFFFFFC000000ull) {
      marco.Abrir();
      Argumentos(ctx, dev, r4, 0x2300, dev + 0x2DF8, 0x825A4130);
      sub_825A2AA0(ctx, base);
    }
    m = Leer64(base, dev + 0x30);
    if (uint32_t(m) & 0x03FC0000u) {
      marco.Abrir();
      Argumentos(ctx, dev, (m << 38) & 0xFF00000000000000ull, 0x2380, dev + 0x2E90, 0x825A4154);
      sub_825A2AA0(ctx, base);
    }
    Escribir64(base, dev + 0x30, 0);
  }
  if (const uint64_t m = Leer64(base, dev + 0x10)) {
    marco.Abrir();
    Argumentos(ctx, dev, m, 0x4000, dev + 0x780, 0x825A4178);
    __imp__sub_825A2C58(ctx, base);
    Escribir64(base, dev + 0x10, 0);
  }
  if (const uint64_t m = Leer64(base, dev + 0x18)) {
    marco.Abrir();
    Argumentos(ctx, dev, m, 0x4400, dev + 0x1780, 0x825A419C);
    __imp__sub_825A2C58(ctx, base);
    Escribir64(base, dev + 0x18, 0);
  }
  if (Leer64(base, dev + 0x20) != 0) {
    uint64_t m = Leer64(base, dev + 0x20);
    if (const uint64_t r4 = m & 0xFFFFFFFF00000000ull) {
      marco.Abrir();
      Argumentos(ctx, dev, r4, 0x4800, dev + 0x480, 0x825A41C8);
      __imp__sub_825A2B60(ctx, base);
    }
    m = Leer64(base, dev + 0x20);
    if (uint32_t(m) & 0x80000000u) {
      marco.Abrir();
      Escribir64(base, dev + 0x2CB0, 0xFFFFFFFFFF000000ull);
      Argumentos(ctx, dev, 0xFFFFFFFFFF000000ull, 0x4900, dev + 0x2780, 0x825A41F4);
      sub_825A2AA0(ctx, base);
    }
    m = Leer64(base, dev + 0x20);
    if (uint32_t(m) & 0x3FFFC000u) {
      marco.Abrir();
      Argumentos(ctx, dev, (m << 34) & 0xFFFF000000000000ull, 0x2000, dev + 0x2CC0, 0x825A4218);
      sub_825A2AA0(ctx, base);
    }
    Escribir64(base, dev + 0x20, 0);
  }
  if (Leer64(base, dev + 0x28) != 0) {
    uint64_t m = Leer64(base, dev + 0x28);
    if (const uint64_t r4 = m & 0xFFFFF80000000000ull) {
      marco.Abrir();
      Argumentos(ctx, dev, r4, 0x2100, dev + 0x2D0C, 0x825A4244);
      sub_825A2AA0(ctx, base);
    }
    m = Leer64(base, dev + 0x28);
    if (m & 0x000007C000000000ull) {
      marco.Abrir();
      Argumentos(ctx, dev, (m << 21) & 0xF800000000000000ull, 0x2180, dev + 0x2D60, 0x825A4270);
      sub_825A2AA0(ctx, base);
    }
    m = Leer64(base, dev + 0x28);
    if (m & 0x0000003FFC000000ull) {
      marco.Abrir();
      Argumentos(ctx, dev, (m << 26) & 0xFFF0000000000000ull, 0x2200, dev + 0x2D74, 0x825A429C);
      sub_825A2AA0(ctx, base);
    }
    m = Leer64(base, dev + 0x28);
    if (uint32_t(m) & 0x03FFFFE0u) {
      marco.Abrir();
      Argumentos(ctx, dev, (m << 38) & 0xFFFFF80000000000ull, 0x2280, dev + 0x2DA4, 0x825A42C0);
      sub_825A2AA0(ctx, base);
    }
    Escribir64(base, dev + 0x28, 0);
  }
}

void AnotarIgualJuego(const Volcado& v) {
  Sumar(g_iguales_juego, uint64_t(1));
  uint32_t listos = g_grupos_listos.load(std::memory_order_relaxed);
  for (uint32_t g = 0; g < kGrupos; ++g) {
    if ((v.grupos >> g) & 1) {
      const uint32_t veces = g_comprobados_grupo[g].load(std::memory_order_relaxed) + 1;
      g_comprobados_grupo[g].store(veces, std::memory_order_relaxed);
      if (veces >= kMinimoPorGrupo) {
        listos |= 1u << g;
      }
    }
  }
  g_grupos_listos.store(listos, std::memory_order_relaxed);
}

uint32_t DecidirModo(uint32_t grupos) {
  if (!g_aplicando.load(std::memory_order_relaxed)) {
    const uint64_t juego = g_iguales_juego.load(std::memory_order_relaxed);
    const uint64_t anillo = g_iguales_anillo.load(std::memory_order_relaxed);
    if (juego < kComprobacionesJuego || anillo < kComprobacionesAnillo) {
      return kMarcadorComprobar;
    }
    g_aplicando.store(true, std::memory_order_relaxed);
    REXLOG_INFO("[d3d_marcador] {} volcados comprobados en el hilo del juego y {} en el del anillo, 0 diferencias: "
                "FlushState escribe ya solo el marcador (1 de cada {} se sigue comprobando)",
                juego, anillo, kComprobarCada);
  }
  if ((grupos & ~g_grupos_listos.load(std::memory_order_relaxed)) != 0) {
    return kMarcadorComprobar;  // a group that has not been checked enough times yet
  }
  const uint64_t turno = g_turno.load(std::memory_order_relaxed) + 1;
  g_turno.store(turno, std::memory_order_relaxed);
  return (turno & (kComprobarCada - 1)) == 0 ? kMarcadorComprobar : kMarcadorAplicar;
}

// The group of a register, for the log.
const char* NombreGrupo(uint32_t registro) {
  if (registro >= 0x4000) {
    return kNombreGrupo[registro >= 0x4900 ? kGBool : registro >= 0x4800 ? kGFetch : registro >= 0x4400 ? kGPs : kGVs];
  }
  switch (registro & ~0x7Fu) {
    case 0x2000:
      return kNombreGrupo[kG2000];
    case 0x2100:
      return kNombreGrupo[kG2100];
    case 0x2180:
      return kNombreGrupo[kG2180];
    case 0x2200:
      return kNombreGrupo[kG2200];
    case 0x2280:
      return kNombreGrupo[kG2280];
    case 0x2300:
      return kNombreGrupo[kG2300];
    case 0x2380:
      return kNombreGrupo[kG2380];
    default:
      return "?";
  }
}

void Apagar(const char* lado, const char* que, uint32_t secuencia, uint32_t registro, uint32_t esperado,
            uint32_t visto) {
  if (g_apagado.load(std::memory_order_relaxed)) {
    return;
  }
  g_apagado.store(true, std::memory_order_relaxed);
  REXLOG_ERROR("[d3d_marcador] DIFERENCIA en el {} ({}): marcador {}, grupo {}, registro {:04X}, esperado {:08X}, "
               "visto {:08X}. Camino del marcador APAGADO para el resto de la sesion: FlushState vuelve al del juego",
               lado, que, secuencia, NombreGrupo(registro), registro, esperado, visto);
}

void InformeMarcador() {
  const int64_t ahora = AhoraNs() / 1000000;
  const int64_t siguiente = g_i_siguiente_ms.load(std::memory_order_relaxed);
  if (ahora < siguiente) {
    return;
  }
  g_i_siguiente_ms.store(ahora + 10000, std::memory_order_relaxed);
  if (siguiente == 0) {
    REXLOG_INFO("[d3d_marcador] marcador compuesto de FlushState activo (fase 2 del renderizador a nivel de "
                "Direct3D): empieza comprobando");
    return;
  }
  const uint64_t llamadas = g_i_llamadas.exchange(0, std::memory_order_relaxed);
  const uint64_t con_registros = g_i_con_registros.exchange(0, std::memory_order_relaxed);
  const uint64_t aplicados = g_i_aplicados.exchange(0, std::memory_order_relaxed);
  const uint64_t tramos = g_i_tramos.exchange(0, std::memory_order_relaxed);
  const uint64_t palabras = g_i_palabras.exchange(0, std::memory_order_relaxed);
  uint32_t listos = g_grupos_listos.load(std::memory_order_relaxed);
  uint32_t n_listos = 0;
  for (; listos; listos &= listos - 1) {
    ++n_listos;
  }
  NFSMW_INFORME_DIFERIDO("[d3d_marcador] ultimos 10 s: {} FlushState ({} con registros): {} solo con marcador ({:.1f} tramos y "
              "{:.1f} palabras cada uno), {} comprobados contra el juego, {} sin sitio en el anillo, {} no comparables "
              "| fase {} | comprobados: juego {} de {}, anillo {} de {}, grupos listos {} de {}",
              llamadas, con_registros, aplicados, aplicados ? double(tramos) / double(aplicados) : 0.0,
              aplicados ? double(palabras) / double(aplicados) : 0.0,
              g_i_comprobados.exchange(0, std::memory_order_relaxed),
              g_i_sin_sitio.exchange(0, std::memory_order_relaxed),
              g_i_no_comparables.exchange(0, std::memory_order_relaxed),
              g_apagado.load(std::memory_order_relaxed) ? "APAGADO"
              : g_aplicando.load(std::memory_order_relaxed) ? "aplicando"
                                                             : "mirando",
              g_iguales_juego.load(std::memory_order_relaxed), kComprobacionesJuego,
              g_iguales_anillo.load(std::memory_order_relaxed), kComprobacionesAnillo, n_listos, uint32_t(kGrupos));
}

}  // namespace marcador
}  // namespace

namespace nfsmw::nativo {

void ActivarConsumidorMarcadores(bool activo) {
  marcador::g_consumidor.store(activo, std::memory_order_release);
}

void AnotarComprobacionMarcador(bool igual, uint32_t secuencia, uint32_t registro, uint32_t en_registros,
                                uint32_t en_marcador) {
  if (igual) {
    Sumar(marcador::g_iguales_anillo, uint64_t(1));
    return;
  }
  if (marcador::g_distinto_anillo.load(std::memory_order_relaxed)) {
    return;
  }
  marcador::g_distinto_secuencia.store(secuencia, std::memory_order_relaxed);
  marcador::g_distinto_registro.store(registro, std::memory_order_relaxed);
  marcador::g_distinto_en_registros.store(en_registros, std::memory_order_relaxed);
  marcador::g_distinto_en_marcador.store(en_marcador, std::memory_order_relaxed);
  marcador::g_distinto_anillo.store(true, std::memory_order_release);
}

}  // namespace nfsmw::nativo

// Ring-full path of the dumps: nothing changes, it is only counted. With the ring full, a dump's packets
// end up split between two stretches of the ring and the watching phase cannot compare them.
REX_HOOK_RAW(sub_825A29E8) {
  Sumar(marcador::g_anillo_lleno, uint64_t(1));
  __imp__sub_825A29E8(ctx, base);
}

// From the FlushState hook (nfsmw_d3d_trace.cpp). false = nothing touched: let the original run.
bool NfsmwFlushStateMarcador(PPCContext& ctx, uint8_t* base) {
  using namespace marcador;
  // Phase 2b: the record of the Draw* that called this FlushState, if its hook left it pending. It leaves
  // from here in the marker or through the queue, before that Draw* writes its DRAW_INDX.
  nfsmw::nativo::RegistroDibujo dibujo;
  const bool con_dibujo = nfsmw::nativo::TomarDibujoEnCurso(dibujo);
  static const bool activo = REXCVAR_GET(nfsmw_d3d_marcador);
  if (!activo || !g_consumidor.load(std::memory_order_relaxed) || g_apagado.load(std::memory_order_relaxed)) {
    if (con_dibujo) {
      nfsmw::nativo::EntregarDibujo(dibujo, 0, false);
    }
    return false;
  }
  if (g_distinto_anillo.load(std::memory_order_acquire)) {
    Apagar("hilo del anillo", "registros tras los paquetes", g_distinto_secuencia.load(std::memory_order_relaxed),
           g_distinto_registro.load(std::memory_order_relaxed),
           g_distinto_en_marcador.load(std::memory_order_relaxed),
           g_distinto_en_registros.load(std::memory_order_relaxed));
    if (con_dibujo) {
      nfsmw::nativo::EntregarDibujo(dibujo, 0, false);
    }
    return false;
  }
  const uint32_t dev = ctx.r3.u32;
  const uint64_t lr = ctx.lr;
  Marco marco(ctx, base);
  // 1. Streams and shaders: the game's code, in the same order and with the same conditions as FlushState.
  const uint64_t m30 = Leer64(base, dev + 0x30);
  if (m30 != 0) {
    if (m30 & 0x400) {
      marco.Abrir();
      ctx.r3.u64 = dev;
      ctx.lr = 0x825A40F8;
      __imp__sub_825A2D80(ctx, base);
    }
    if (Leer64(base, dev + 0x30) & 0x1E0) {
      marco.Abrir();
      ctx.r3.u64 = dev;
      ctx.lr = 0x825A4110;
      __imp__sub_825A3AF0(ctx, base);
    }
  }
  // 2. The runs of all the dumps, with the masks left by streams and shaders.
  Volcado v;
  Colectar(base, dev, m30 != 0, v);
  const uint32_t secuencia = g_secuencia.load(std::memory_order_relaxed) + 1;
  g_secuencia.store(secuencia, std::memory_order_relaxed);
  const uint32_t modo = (v.n == 0 || v.desbordado) ? 0u : DecidirModo(v.grupos);
  const uint32_t modo_dibujo = con_dibujo ? nfsmw::nativo::DecidirModoDibujo() : 0u;  // phase 2b
  bool dibujo_en_marcador = false;
  const uint64_t llamadas = g_i_llamadas.load(std::memory_order_relaxed) + 1;
  g_i_llamadas.store(llamadas, std::memory_order_relaxed);
  Sumar(g_i_con_registros, uint64_t(v.n != 0));
  if (modo == kMarcadorAplicar && EscribirMarcador(base, dev, &v, kMarcadorAplicar, secuencia, modo_dibujo, &dibujo)) {
    // 3a. Only the marker.
    dibujo_en_marcador = modo_dibujo != 0;
    LimpiarComoFlushState(base, dev, m30 != 0, v);
    static const bool sombra = REXCVAR_GET(nfsmw_nativo_sombra_d3d);
    if (sombra) {  // what the 825A2AA0 hook would do per group (phase 1, as a shadow)
      for (uint32_t g = 0; g < kGrupos; ++g) {
        if ((v.grupos >> g) & 1) {
          nfsmw::nativo::AprenderGrupoEspejo(v.registro_base[g], v.mascara[g], v.origen_base[g] - dev);
        }
      }
    }
    ctx.r3.u64 = dev;
    Sumar(g_i_aplicados, uint64_t(1));
    Sumar(g_i_tramos, uint64_t(v.n));
    Sumar(g_i_palabras, uint64_t(v.palabras - v.n));
  } else {
    // 3b. The game's dumps and, in check mode, the comparison and the check marker after them.
    if (modo == kMarcadorAplicar) {
      Sumar(g_i_sin_sitio, uint64_t(1));
    }
    const uint32_t desde = Leer32(base, dev + 0);
    const uint64_t lentos = g_anillo_lleno.load(std::memory_order_relaxed);
    VolcarComoElJuego(ctx, base, marco, dev, m30 != 0);
    if (modo == kMarcadorComprobar) {
      const uint32_t hasta = Leer32(base, dev + 0);
      Diferencia d;
      if (g_anillo_lleno.load(std::memory_order_relaxed) != lentos || hasta < desde) {
        Sumar(g_i_no_comparables, uint64_t(1));
      } else if (!CoincidenPaquetes(base, desde, hasta, v, d)) {
        Apagar("hilo del juego", d.que, secuencia, d.registro, d.esperado, d.visto);
      } else {
        AnotarIgualJuego(v);
        if (EscribirMarcador(base, dev, &v, kMarcadorComprobar, secuencia, modo_dibujo, &dibujo)) {
          dibujo_en_marcador = modo_dibujo != 0;
          Sumar(g_i_comprobados, uint64_t(1));
        } else {
          Sumar(g_i_sin_sitio, uint64_t(1));
        }
      }
    }
  }
  // Phase 2b: if no register marker has taken the draw (nothing to dump, no space, ring full), one with
  // only the draw; and to the queue whatever has to go there (failure, or check mode).
  if (modo_dibujo != 0 && !dibujo_en_marcador) {
    dibujo_en_marcador = EscribirMarcador(base, dev, nullptr, 0, secuencia, modo_dibujo, &dibujo);
  }
  if (con_dibujo) {
    nfsmw::nativo::EntregarDibujo(dibujo, modo_dibujo, dibujo_en_marcador);
  }
  // Like the original's epilogue: lwz r12,-8(r1); mtlr r12.
  ctx.lr = lr;
  ctx.r12.u64 = uint32_t(lr);
  if ((llamadas & 4095) == 0) {
    InformeMarcador();
  }
  return true;
}

