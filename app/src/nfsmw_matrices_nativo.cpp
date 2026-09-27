// nfsmw - per-draw matrices (sub_824538D0) in native code.
//
// WHAT IT IS (PowerPC read instruction by instruction in nfsmw_recomp.78.cpp:16080)
//   Called by the game thread on every draw with r3 = material object ([r3+12] = handle table, [r3+28] =
//   effect), r4 = matrix A (4 rows of 16 bytes) and r5 = view block (rows P at +240, rows Q at +304 and the
//   position E at +48). 384-byte frame.
//   1. Cache: 0x82A2D190 keeps (r5, r4, r3) of the previous call. If they are the same, it does nothing else.
//   2. M1 = A x P, M2 = A x Q and M3 = A x C, with C the pass matrix (0x82C40CC0 + 384 * [0x82A2D1A0]): each
//      row is a vmulfp128 and three vmaddfp. They go to the frame (r1+128, +192 and +256).
//   3. The rigid inverse of A (xyz rows of A0..A2 with -A3.Ai in w) and, with it, E' (E = [r5+48..56] with
//      w = [0x82063038]) and G' (G = [[0x82A2C4F8] + 288]): vmsum3fp128, vmsum4fp128 and vmrghw. They go to
//      r1+80 and r1+112.
//   4. Depending on the handles in the table, the effect's writers: 82449C00 (matrix) with M1 (+572), A
//      (+568), M3 (+468) and M2 (+576); 82449988 (vector) with E' (+284), G' (+292) and, if there is a
//      handle at +564, with the vector at +16 of the pass record normalized by sub_8215_A588 into r1+320.
//
// WHY NATIVE
//   Stack sampling: 7.2 % of a core of the game thread including its frameless leaves. Recompiled (13,696
//   bytes in the ELF, with the vector body duplicated by the conditional enableFlushMode) every vector
//   instruction goes through the context, every lvx/stvx rereads VectorMaskL from memory, and every call
//   writes the FPCR (msr fpcr) four or five times, plus once more in sub_8215_A588. Here everything stays in
//   registers and the FPCR is written at most twice (the vector part in flush mode and the normalization in
//   scalar mode). Vector part, one path (devkitA64 -O3): 595 instructions, 265 memory accesses and 5 msr in
//   the recompiled ELF; here 394, 78 and 1. The normalization goes from 251 instructions with its own msr
//   to about 60 inlined ones. The writers are called just as in the original, through their hooks
//   (nfsmw_material_nativo.cpp): their work does not change.
//
// WHY IT IS BIT-IDENTICAL
//   - Floating point: the same simde functions as the generated code, with the same operands and in the
//     same order. The nfsmw and nfsmw_recomp targets are compiled with FMA contraction (without
//     -ffp-contract=off) and GCC fuses "a*b + c" depending on who else uses each product; this was read
//     in the ELF and is reproduced the same way here:
//       * The 12 rows of M1, M2 and M3: the first product (vmulfp128, a single-use local variable) is fused
//         with the rounded second one: fma(X0, a0, r(X1*a1)), then fma(X2, a2, .) and fma(X3, a3, .). Here
//         the same: the first product in its local variable, first, and the rest in the same form.
//       * The three vmaddfp of the inverse (v10, v0, v12): the original stores the first product in the
//         context (a second use) and the second one is fused: fma((1,1,1,0), Ai, r(dp * (0,0,0,1))). Here
//         that first product goes through Opaco() (an empty asm) so that GCC has to do the same.
//       * The constants -1, (0,0,0,1) and (1,1,1,0) also go through Opaco(): the original computes them at
//         run time (scvtf, ext) and GCC must not turn "x * -1" into "-x" (with a NaN it is not the same).
//       * simde_mm_dp_ps is the same on both sides (on the Switch vmulq_f32 + faddp + faddp).
//     Checked in devkitA64's GIMPLE with the Switch options (12 of 12 rows and 3 of 3 of the inverse, the
//     same as the original compiled separately) and on PC with FMA (0 differences).
//     The denormal flush mode is the original's: the whole vector part with FZ set (the original clears it
//     only for its lfs/stfs, which are word copies here) and the normalization without it. It exits in the
//     mode the original exits in (with FZ, or without it if it normalized).
//   - NaN: with several NaNs in one operation, which one propagates depends on the order of the operands
//     inside the instruction, which the compiler chooses (the ELF has products with the operands reversed
//     relative to simde). That is why, if any floating-point input is NaN (A, P, Q, C, E, its w, G, or the
//     vector and the two constants of the normalization), the original does the work (the whole function,
//     or only sub_8215_A588). Without NaN in the input, any NaN that appears is the default NaN and the order
//     can no longer change a single bit; infinities, denormals and signed zeros depend only on the
//     operations, which are the same (PC test with and without FMA).
//   - Memory: the same final state byte for byte. Prologue (frame back link, r3 at r1+404 and r4 at
//     r1+412), the cache, r1+96, the 12 rows, E', G', the three floats of the normalization and what the
//     writers write. The stfs to r1+80..92 are not written because the stvx of E' overwrites that same whole
//     block. The original reads some inputs after writing to its frame and here they are read before: if
//     any lands inside the frame, or r1 is not 16-byte aligned, the original runs. If it is left to the
//     original after the frame has been written (NaN), it does not matter: it writes it again in full before
//     reading it, and the cache is left as it was. The reads of the handle table and of the effect are
//     repeated after each writer, as in the original.
//   - Registers: interprocedural liveness analysis of all the generated code (56,338 functions): after the
//     16 direct call sites only r3 is live (in 2) and f1 (which is not touched). r3 is left as the original
//     leaves it, r1 as its stwu + addi, and r12/lr like the original; the writers get the same
//     r3/r4/r5/lr/r1. cr6, xer, r28-r31 and v19-v31 are local variables in the generated code.
//
// SELF-CHECKING GUARD (cvar nfsmw_matrices_nativo; project rule)
//   The first kComprobaciones calls of each path (cache and computation) and then 1 of every 4096: the
//   native version records each write (with copies of the two writers, 82449C00 and 82449988, identical to
//   the ones in nfsmw_material_nativo.cpp), it is undone, the original runs (with the real hooks) and the
//   recorded bytes, the whole frame (416 bytes), r3, r1 and the FPCR are compared. The original's result is
//   always kept. A single difference turns the native version off for the session and writes
//   "[matrices] DIFERENCIA". "[matrices]" line every 10 s.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_informe_diferido.h"  // deferred reports
#include <rex/platform.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

REXCVAR_DEFINE_BOOL(nfsmw_matrices_nativo, true, "NFSMW",
                    "Matrices por dibujo (sub_824538D0: A x P, A x Q, A x C y la inversa rigida de A) en nativo "
                    "(build 176), identico bit a bit. Se comprueba contra la original (las primeras 100.000 llamadas "
                    "de cada camino y despues 1 de cada 4096) y se apaga sola si difiere; false = la original")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_824538D0);
// The writers by their usual name: the hook in nfsmw_material_nativo.cpp (or the original, if there is none).
REX_EXTERN(sub_82449C00);
REX_EXTERN(sub_82449988);
// The original normalization. The name is assembled from parts on purpose: tools/llamadas_directas.py treats
// any address that appears whole in app/src as hooked, and its 70 calls from the generated code must stay
// direct.
#define NFSMW_MATRICES_UNIR_(a, b) a##b
#define NFSMW_MATRICES_NORMALIZAR NFSMW_MATRICES_UNIR_(__imp__sub_8215, A588)
REX_EXTERN(NFSMW_MATRICES_NORMALIZAR);

namespace nfsmw::matrices {
namespace {

// ---------------------------------------------------------------------------------------------------------------
// Guest memory: the same translation as REX_RAW_ADDR / REX_LOAD / REX_STORE in nfsmw_pch.h.
// ---------------------------------------------------------------------------------------------------------------
[[gnu::always_inline]] inline uint32_t Desplazamiento(uint32_t direccion) {
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
  return direccion >= 0xE0000000u ? 0x1000u : 0u;
#else
  (void)direccion;
  return 0u;
#endif
}
[[gnu::always_inline]] inline uint8_t* Puntero(uint8_t* base, uint32_t direccion) {
  return base + direccion + Desplazamiento(direccion);
}
[[gnu::always_inline]] inline uint8_t Leer8(uint8_t* base, uint32_t direccion) {
  return *Puntero(base, direccion);
}
[[gnu::always_inline]] inline uint32_t Leer32(uint8_t* base, uint32_t direccion) {
  uint32_t v;
  std::memcpy(&v, Puntero(base, direccion), 4);
  return __builtin_bswap32(v);
}
// 16 bytes from an aligned address, in memory order.
[[gnu::always_inline]] inline void LeerBloque(uint8_t* base, uint32_t alineada, uint8_t* salida) {
  std::memcpy(salida, Puntero(base, alineada), 16);
}

// ---------------------------------------------------------------------------------------------------------------
// Write with or without recording (as in nfsmw_material_nativo.cpp). Without recording it costs nothing; when
// recording (guard) it saves address, size and previous bytes to undo.
// ---------------------------------------------------------------------------------------------------------------
enum Clase : uint8_t { kPura = 0, kOr = 1 };

struct Anotacion {
  uint32_t direccion;
  uint8_t bytes;
  uint8_t clase;
  uint8_t mascara;
  uint8_t antes[16];
  uint8_t despues[16];
};

struct Registro {
  Anotacion* a;
  uint32_t capacidad;
  uint32_t n = 0;
  bool lleno = false;
};

template <bool kAnotar>
struct Memoria {
  uint8_t* base;
  Registro* registro;

  [[gnu::always_inline]] inline void Anotar(uint32_t direccion, uint32_t bytes, uint8_t clase, uint8_t mascara) {
    if constexpr (kAnotar) {
      if (registro->n >= registro->capacidad) {
        registro->lleno = true;
        return;
      }
      Anotacion& e = registro->a[registro->n++];
      e.direccion = direccion;
      e.bytes = uint8_t(bytes);
      e.clase = clase;
      e.mascara = mascara;
      std::memcpy(e.antes, Puntero(base, direccion), bytes);
    } else {
      (void)direccion;
      (void)bytes;
      (void)clase;
      (void)mascara;
    }
  }
  [[gnu::always_inline]] inline void Escribir32(uint32_t direccion, uint32_t valor) {
    Anotar(direccion, 4, kPura, 0);
    valor = __builtin_bswap32(valor);
    std::memcpy(Puntero(base, direccion), &valor, 4);
  }
  [[gnu::always_inline]] inline void Escribir16B(uint32_t alineada, const uint8_t* bytes) {
    Anotar(alineada, 16, kPura, 0);
    std::memcpy(Puntero(base, alineada), bytes, 16);
  }
  [[gnu::always_inline]] inline void MarcarSucio(uint32_t direccion, uint8_t viejo, uint8_t bit) {
    Anotar(direccion, 1, kOr, bit);
    *Puntero(base, direccion) = uint8_t(viejo | bit);
  }
};

// ---------------------------------------------------------------------------------------------------------------
// Fixed addresses (PowerPC lis + offset; checked in the PC test).
// ---------------------------------------------------------------------------------------------------------------
constexpr uint32_t kUltima = 0x82A2D190;        // lis r11,-32093; addi r11,r11,-11888: r5 (+0), r4 (+4), r3 (+8)
constexpr uint32_t kIndicePase = 0x82A2D1A0;    // lwz r30,-11872(0x82A30000)
constexpr uint32_t kPases = 0x82C3F590;         // lis r11,-32060; addi r28,r11,-2672: registros de 384 bytes
constexpr uint32_t kMatrizPase = 5936;          // addi r8,r28,5936: C in the pass record
constexpr uint32_t kVectorPase = 5760 + 16;     // addi r10,r28,5760 ... addi r4,r11,16: vector a normalizar
constexpr uint32_t kPunteroG = 0x82A2C4F8;      // lwz r11,-15112(0x82A30000); addi r10,r11,288
constexpr uint32_t kUno = 0x82063038;           // lfs f0,12344(0x82060000): w of E; the 1 of the normalization
constexpr uint32_t kCero = 0x82061CE8;          // lfs f12,-4944(0x82063038): the 0 of the normalization
constexpr uint32_t kMarco = 384;                // stwu r1,-384(r1)
// Return addresses of each bl (ctx.lr in the generated code)
constexpr uint32_t kVueltaPrologo = 0x824538D8;  // bl __savegprlr (which the generated code does not execute)
constexpr uint32_t kVuelta572 = 0x82453C30;
constexpr uint32_t kVuelta568 = 0x82453C4C;
constexpr uint32_t kVueltaNormalizar = 0x82453C7C;
constexpr uint32_t kVuelta564 = 0x82453C8C;
constexpr uint32_t kVuelta468 = 0x82453CA8;
constexpr uint32_t kVuelta576 = 0x82453CC4;
constexpr uint32_t kVuelta284 = 0x82453D1C;
constexpr uint32_t kVuelta292 = 0x82453D3C;

// ---------------------------------------------------------------------------------------------------------------
// The two writers, copied unchanged from nfsmw_material_nativo.cpp. Only the guard uses them, to record what
// they are going to write; the normal path calls the hooks, like the original.
// ---------------------------------------------------------------------------------------------------------------
constexpr uint32_t kTablaBits = 0x8290DB68;   // lis r8,-32111; addi r8,r8,-9368: the mask of each bit
constexpr uint32_t kTablaFilas = 0x8208F7C0;  // lis r10,-32247; addi r7,r10,-2112: row masks of 82449C00

struct Grupo {
  uint32_t sucios;
  uint32_t tabla;
  uint32_t destino;
};
inline Grupo LeerGrupo(uint8_t* base, uint32_t efecto, uint32_t mango) {
  if ((mango & 1u) == 0) {
    return {efecto, Leer32(base, efecto + 264), Leer32(base, efecto + 296)};
  }
  return {Leer32(base, efecto + 256), Leer32(base, Leer32(base, efecto + 268)),
          Leer32(base, Leer32(base, efecto + 300))};
}
inline uint32_t Indice(uint32_t mango) {
  return (mango >> 1) & 0x1FFFFu;
}
inline uint32_t Entrada(uint32_t mango) {
  return (mango >> 15) & 0x1FFF8u;
}
inline uint32_t Registro16(uint32_t palabra) {
  return (palabra & 0xFFFFu) << 4;
}

// 82449988: aligned 16-byte vector. Leaves r3 = old dirty byte.
template <bool A>
inline uint32_t Escritora9988(Memoria<A>& m, uint32_t efecto, uint32_t mango, uint32_t p) {
  uint8_t* const base = m.base;
  uint8_t bloque[16];
  LeerBloque(base, p & ~0xFu, bloque);
  const Grupo g = LeerGrupo(base, efecto, mango);
  const uint32_t indice = Indice(mango);
  const uint8_t bit = Leer8(base, kTablaBits + (indice & 7u));
  const uint32_t sucio = g.sucios + (indice >> 3);
  const uint8_t viejo = Leer8(base, sucio);
  m.MarcarSucio(sucio, viejo, bit);
  const uint32_t palabra = Leer32(base, g.tabla + Entrada(mango) + 4);
  m.Escribir16B((Registro16(palabra) + g.destino) & ~0xFu, bloque);
  return viejo;
}

// 82449C00: 4x4 matrix (the rows given by the entry, transposed). Leaves r3 = the mask of the dirty bit.
template <bool A>
inline uint32_t EscritoraC00(Memoria<A>& m, uint32_t efecto, uint32_t mango, uint32_t p) {
  uint8_t* const base = m.base;
  uint32_t fila[4][4];
  for (uint32_t i = 0; i < 4; ++i) {
    LeerBloque(base, (p + 16 * i) & ~0xFu, reinterpret_cast<uint8_t*>(fila[i]));
  }
  const Grupo g = LeerGrupo(base, efecto, mango);
  const uint32_t entrada = g.tabla + Entrada(mango);
  const uint32_t palabra0 = Leer32(base, entrada);
  const uint32_t palabra1 = Leer32(base, entrada + 4);
  const uint32_t filas = ((palabra0 >> 4) & 7u) + 1;
  uint32_t mascara[4];
  LeerBloque(base, (((filas << 2) & 0xFFFFFFF0u) + kTablaFilas) & ~0xFu, reinterpret_cast<uint8_t*>(mascara));
  const uint32_t indice = Indice(mango);
  const uint8_t bit = Leer8(base, kTablaBits + (indice & 7u));
  const uint32_t sucio = g.sucios + (indice >> 3);
  const uint8_t viejo = Leer8(base, sucio);
  m.MarcarSucio(sucio, viejo, bit);
  const uint32_t destino = Registro16(palabra1) + g.destino;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t salida[4];
    for (uint32_t k = 0; k < 4; ++k) {
      salida[k] = (fila[i][k] & ~mascara[k]) | (fila[k][i] & mascara[k]);
    }
    m.Escribir16B((destino + 16 * i) & ~0xFu, reinterpret_cast<const uint8_t*>(salida));
  }
  return bit;
}

// ---------------------------------------------------------------------------------------------------------------
// Vectors as in the generated code: lvx128 = aligned block reversed byte by byte (lane 3 is PowerPC word 0).
// The reversal is VectorMaskL's (its first 16 bytes), but with a constant GCC knows.
// ---------------------------------------------------------------------------------------------------------------
using V = simde__m128i;
using F = simde__m128;

[[gnu::always_inline]] inline V Invertir(V v) {
  return simde_mm_shuffle_epi8(v, simde_mm_setr_epi8(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0));
}
[[gnu::always_inline]] inline V Lvx(uint8_t* base, uint32_t direccion) {
  V v;
  std::memcpy(&v, Puntero(base, direccion & ~0xFu), 16);
  return Invertir(v);
}
template <bool A>
[[gnu::always_inline]] inline void Stvx(Memoria<A>& m, uint32_t direccion, V v) {
  const V x = Invertir(v);
  uint8_t bytes[16];
  std::memcpy(bytes, &x, 16);
  m.Escribir16B(direccion & ~0xFu, bytes);
}
[[gnu::always_inline]] inline F Fl(V v) {
  return simde_mm_castsi128_ps(v);
}
[[gnu::always_inline]] inline V En(F f) {
  return simde_mm_castps_si128(f);
}
template <int kI>
[[gnu::always_inline]] inline V Splat(V v) {  // vspltw: 0xFF = palabra 0, 0xAA = 1, 0x55 = 2, 0x00 = 3
  return simde_mm_shuffle_epi32(v, kI);
}
// vmulfp128 vD,vA,vB and vmaddfp vD,vA,vB,vC in the exact form of the generated code.
[[gnu::always_inline]] inline F Mul(V a, V b) {
  return simde_mm_mul_ps(Fl(a), Fl(b));
}
[[gnu::always_inline]] inline F Madd(V a, V b, F c) {
  return simde_mm_add_ps(simde_mm_mul_ps(Fl(a), Fl(b)), c);
}

// A value GCC cannot see into (an empty asm): it can neither fuse it with an addition nor fold it as a constant.
template <class T>
[[gnu::always_inline]] inline T Opaco(T v) {
#if defined(SIMDE_ARM_NEON_A64V8_NATIVE)
  __asm__("" : "+w"(v));
#elif defined(SIMDE_X86_SSE2_NATIVE)
  __asm__("" : "+x"(v));
#else
  __asm__("" : "+m"(v));
#endif
  return v;
}

// Compiler barrier: no memory reads or writes cross the FPCR change.
[[gnu::always_inline]] inline void Barrera() {
  __asm__ __volatile__("" ::: "memory");
}

[[gnu::always_inline]] inline bool EsNaN(uint32_t bits) {
  return (bits & 0x7FFFFFFFu) > 0x7F800000u;
}
// Absolute value of each word (already in host order): a float is NaN if and only if it exceeds 0x7F800000.
[[gnu::always_inline]] inline V Abs(V bits) {
  return simde_mm_and_si128(bits, simde_mm_set1_epi32(0x7FFFFFFF));
}
[[gnu::always_inline]] inline V Mayor(V a, V b) {  // signed maximum; after Abs all are >= 0
  return simde_mm_max_epi32(a, b);
}
[[gnu::always_inline]] inline bool HayNaN(V mayor) {
  const V nan = simde_mm_cmpgt_epi32(mayor, simde_mm_set1_epi32(0x7F800000));
  uint64_t x[2];
  std::memcpy(x, &nan, 16);
  return (x[0] | x[1]) != 0;
}

// Does [inicio, inicio + n) touch the frame [marco, marco + 384)?
[[gnu::always_inline]] inline bool EnElMarco(uint32_t inicio, uint32_t n, uint32_t marco) {
  return uint64_t(inicio) + n > marco && uint64_t(inicio) < uint64_t(marco) + kMarco;
}

// ---------------------------------------------------------------------------------------------------------------
// The calls to the writers: on the normal path, through their hooks with the context as the original leaves
// it; in the guard, with the copies above, recording.
// ---------------------------------------------------------------------------------------------------------------
struct EscritorasPorGancho {
  PPCContext& ctx;
  uint8_t* base;
  // At the start (cache path or computation path, not when it is left to the original): mflr r12, the prologue
  // bl and the stwu.
  void Empezar(uint32_t marco) {
    ctx.r12.u64 = ctx.lr;
    ctx.lr = kVueltaPrologo;
    ctx.r1.u32 = marco;
  }
  uint32_t Matriz(uint32_t efecto, uint32_t mango, uint32_t p, uint32_t vuelta) {
    ctx.r5.u64 = p;
    ctx.r3.u64 = efecto;
    ctx.r4.u64 = mango;
    ctx.lr = vuelta;
    sub_82449C00(ctx, base);
    return ctx.r3.u32;
  }
  uint32_t Vector(uint32_t efecto, uint32_t mango, uint32_t p, uint32_t vuelta) {
    ctx.r5.u64 = p;
    ctx.r3.u64 = efecto;
    ctx.r4.u64 = mango;
    ctx.lr = vuelta;
    sub_82449988(ctx, base);
    return ctx.r3.u32;
  }
};

template <bool A>
struct EscritorasCopia {
  Memoria<A>& m;
  uint32_t llamadas = 0;
  void Empezar(uint32_t) {}
  uint32_t Matriz(uint32_t efecto, uint32_t mango, uint32_t p, uint32_t) {
    ++llamadas;
    return EscritoraC00(m, efecto, mango, p);
  }
  uint32_t Vector(uint32_t efecto, uint32_t mango, uint32_t p, uint32_t) {
    ++llamadas;
    return Escritora9988(m, efecto, mango, p);
  }
};

// ---------------------------------------------------------------------------------------------------------------
// sub_8215_A588 (nfsmw_recomp.24.cpp:2273): normalizes the 3 floats of s into d; with a length equal to
// [kCero], it leaves ([kUno], [kCero], [kCero]). In double precision and in scalar mode, operation by operation
// like the generated code (the fma in double and then to float, the same as the original). With a NaN in the
// input the original runs.
// ---------------------------------------------------------------------------------------------------------------
template <bool A>
inline void Normalizar(Memoria<A>& m, PPCContext& ctx, uint32_t d, uint32_t s) {
  uint8_t* const base = m.base;
  ctx.fpscr.disableFlushMode();  // its first instruction
  Barrera();
  const uint32_t wy = Leer32(base, s + 4);
  const uint32_t wx = Leer32(base, s + 0);
  const uint32_t wz = Leer32(base, s + 8);
  const uint32_t wcero = Leer32(base, kCero);
  const uint32_t wuno = Leer32(base, kUno);
  if (EsNaN(wx) || EsNaN(wy) || EsNaN(wz) || EsNaN(wcero) || EsNaN(wuno)) [[unlikely]] {
    if constexpr (A) {  // the original writes these three words (and the guard undoes them)
      m.Anotar(d, 4, kPura, 0);
      m.Anotar(d + 4, 4, kPura, 0);
      m.Anotar(d + 8, 4, kPura, 0);
    }
    ctx.r3.u64 = d;
    ctx.r4.u64 = s;
    ctx.lr = kVueltaNormalizar;
    NFSMW_MATRICES_NORMALIZAR(ctx, base);
    return;
  }
  PPCRegister t;
  t.u32 = wy;
  double f0 = double(t.f32);
  f0 = double(float(f0 * f0));
  t.u32 = wx;
  const double f13 = double(t.f32);
  t.u32 = wz;
  double f12 = double(t.f32);
  const double f11 = double(float(std::fma(f13, f13, f0)));
  const double f10 = double(float(std::fma(f12, f12, f11)));
  t.u32 = wcero;
  f12 = double(t.f32);
  f0 = double(float(sqrt(f10)));
  if (f0 == f12) {  // cr6.compare + beq: equal and no NaN
    // lfs/stfs of [kUno] and two stfs of f12: without NaN, copies of the word
    m.Escribir32(d + 0, wuno);
    m.Escribir32(d + 4, wcero);
    m.Escribir32(d + 8, wcero);
    return;
  }
  t.u32 = wuno;
  f0 = double(float(double(t.f32) / f0));
  t.u32 = wy;
  const double y = double(t.f32);
  t.u32 = wz;
  const double z = double(t.f32);
  t.f32 = float(double(float(f13 * f0)));
  m.Escribir32(d + 0, t.u32);
  t.f32 = float(double(float(y * f0)));
  m.Escribir32(d + 4, t.u32);
  t.f32 = float(double(float(z * f0)));
  m.Escribir32(d + 8, t.u32);
}

// ---------------------------------------------------------------------------------------------------------------
// sub_824538D0 entera.
// ---------------------------------------------------------------------------------------------------------------
enum class Camino : uint8_t { kCache, kCalculo, kOriginal };
enum Motivo : uint32_t { kPorPila = 0, kPorMarco = 1, kPorNaN = 2, kMotivos = 3 };

struct Salida {
  Camino camino;
  uint32_t r3;         // kCalculo: the r3 the original leaves
  bool normalizo;      // kCalculo: sub_8215_A588 was called (the original exits in scalar mode)
  uint32_t motivo;     // kOriginal: why
};

template <bool A, class E>
Salida Nativa(Memoria<A>& m, PPCContext& ctx, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t pila, E& esc) {
  uint8_t* const base = m.base;
  const uint32_t r1 = pila - kMarco;
  // Prologue: stwu r1,-384(r1); stw r3,404(r1); stw r4,412(r1). The cache, like the original: read and write
  // before checking whether it is the same input.
  m.Escribir32(r1, pila);
  m.Escribir32(r1 + 404, r3);
  m.Escribir32(r1 + 412, r4);
  const uint32_t ultima8 = Leer32(base, kUltima + 8);  // lwz r10,8(r11)
  const uint32_t ultima0 = Leer32(base, kUltima + 0);  // lwz r9,0(r11)
  const uint32_t ultima4 = Leer32(base, kUltima + 4);  // lwz r8,4(r11)
  m.Escribir32(kUltima + 4, r4);                  // stw r4,4(r11)
  m.Escribir32(kUltima + 8, r3);                  // stw r3,8(r11)
  m.Escribir32(kUltima + 0, r5);                  // stw r5,0(r11)
  if (((ultima0 - r5) | (ultima8 - r3) | (ultima4 - r4)) == 0) {  // or, or, cmpwi cr6,r10,0, beq
    esc.Empezar(r1);
    return {Camino::kCache, 0, false, 0};
  }

  // --- The addresses, without reading any float yet ---
  ctx.fpscr.enableFlushMode();  // the one of "vmulfp128 v22": the whole vector part runs with FZ set
  Barrera();
  auto a_la_original = [&](uint32_t motivo) -> Salida {
    m.Escribir32(kUltima + 4, ultima4);  // the cache as it was: the original must see the previous entry
    m.Escribir32(kUltima + 8, ultima8);
    m.Escribir32(kUltima + 0, ultima0);
    return {Camino::kOriginal, 0, false, motivo};
  };
  if ((pila & 0xFu) != 0) [[unlikely]] {
    return a_la_original(kPorPila);
  }
  const uint32_t indice = Leer32(base, kIndicePase);                    // lwz r30,-11872(r11)
  const uint32_t pase_c = kPases + kMatrizPase + ((3u * indice) << 7);  // rlwinm, add, rlwinm, add
  const uint32_t puntero_g = Leer32(base, kPunteroG);                   // lwz r11,-15112(r11)
  const uint32_t pos_g = puntero_g + 288;                               // addi r10,r11,288
  // The original reads some inputs after writing to its frame: none of them may land inside it.
  if (EnElMarco(r4 & ~0xFu, 64, r1) || EnElMarco((r5 + 240) & ~0xFu, 128, r1) || EnElMarco(r5 + 48, 12, r1) ||
      EnElMarco(pase_c & ~0xFu, 64, r1) || EnElMarco(pos_g & ~0xFu, 16, r1) || EnElMarco(kIndicePase, 4, r1) ||
      EnElMarco(kPunteroG, 4, r1) || EnElMarco(kUno, 4, r1)) [[unlikely]] {
    return a_la_original(kPorMarco);
  }

  // --- M1 = A x P, M2 = A x Q, M3 = A x C, in the order of the generated code (nfsmw_recomp.78.cpp:16147-16394)
  // and with the loads where the original does them. The absolute value of each input goes into the maximum: at
  // the end it says whether there was a NaN.
  const V a0 = Lvx(base, r4);        // lvx128 v12,r0,r4
  const V s00 = Splat<0xFF>(a0);     // vspltw v7,v12,0
  const V s01 = Splat<0xAA>(a0);     // vspltw v3,v12,1
  const V a1 = Lvx(base, r4 + 16);   // lvx128 v11,r0,r9
  const V a2 = Lvx(base, r4 + 32);   // lvx128 v10,r0,r8
  const V s10 = Splat<0xFF>(a1);     // vspltw v6,v11,0
  const V s20 = Splat<0xFF>(a2);     // vspltw v5,v10,0
  const V q0 = Lvx(base, r5 + 304);  // lvx128 v13,r0,r10
  const V a3 = Lvx(base, r4 + 48);   // lvx128 v9,r0,r7
  const V s30 = Splat<0xFF>(a3);     // vspltw v4,v9,0
  V mayor = Mayor(Mayor(Abs(a0), Abs(a1)), Mayor(Abs(a2), Abs(a3)));
  F v22 = Mul(q0, s00);              // vmulfp128 v22,v13,v7
  const V p0 = Lvx(base, r5 + 240);  // lvx128 v0,r0,r11
  F v30 = Mul(p0, s00);              // vmulfp128 v30,v0,v7
  F v28 = Mul(p0, s10);              // vmulfp128 v28,v0,v6
  F v27 = Mul(p0, s20);              // vmulfp128 v27,v0,v5
  const V s11 = Splat<0xAA>(a1);     // vspltw v2,v11,1
  F v26 = Mul(p0, s30);              // vmulfp128 v26,v0,v4
  const V s21 = Splat<0xAA>(a2);     // vspltw v1,v10,1
  const V c0 = Lvx(base, pase_c);    // lvx128 v8,r0,r9
  const V s31 = Splat<0xAA>(a3);     // vspltw v31,v9,1
  F v29 = Mul(c0, s00);              // vmulfp128 v29,v8,v7
  const V p1 = Lvx(base, r5 + 256);  // lvx128 v0,r0,r4
  F v25 = Mul(c0, s10);              // vmulfp128 v25,v8,v6
  const V q1 = Lvx(base, r5 + 320);  // lvx128 v7,r0,r8
  F v24 = Mul(c0, s20);              // vmulfp128 v24,v8,v5
  F v23 = Mul(c0, s30);              // vmulfp128 v23,v8,v4
  const V c1 = Lvx(base, pase_c + 16);  // lvx128 v8,r0,r3
  const F v21 = Mul(q0, s10);        // vmulfp128 v21,v13,v6
  const F v20 = Mul(q0, s20);        // vmulfp128 v20,v13,v5
  const F v19 = Mul(q0, s30);        // vmulfp128 v19,v13,v4
  mayor = Mayor(mayor, Mayor(Mayor(Abs(q0), Abs(p0)), Mayor(Abs(c0), Mayor(Abs(p1), Mayor(Abs(q1), Abs(c1))))));
  v30 = Madd(p1, s01, v30);          // vmaddfp v30,v0,v3,v30
  const V s02 = Splat<0x55>(a0);     // vspltw v6,v12,2
  v28 = Madd(p1, s11, v28);          // vmaddfp v28,v0,v2,v28
  const V s12 = Splat<0x55>(a1);     // vspltw v5,v11,2
  v27 = Madd(p1, s21, v27);          // vmaddfp v27,v0,v1,v27
  const V s22 = Splat<0x55>(a2);     // vspltw v4,v10,2
  v26 = Madd(p1, s31, v26);          // vmaddfp v26,v0,v31,v26
  const V c2 = Lvx(base, pase_c + 32);  // lvx128 v13,r0,r6
  v22 = Madd(q1, s01, v22);          // vmaddfp v22,v7,v3,v22
  const V p2 = Lvx(base, r5 + 272);  // lvx128 v0,r0,r7
  v29 = Madd(c1, s01, v29);          // vmaddfp v29,v8,v3,v29
  const V s32 = Splat<0x55>(a3);     // vspltw v3,v9,2
  v25 = Madd(c1, s11, v25);          // vmaddfp v25,v8,v2,v25
  v24 = Madd(c1, s21, v24);          // vmaddfp v24,v8,v1,v24
  v23 = Madd(c1, s31, v23);          // vmaddfp v23,v8,v31,v23
  const V q2 = Lvx(base, r5 + 336);  // lvx128 v8,r0,r5
  const F v2 = Madd(q1, s11, v21);   // vmaddfp v2,v7,v2,v21
  const F v1 = Madd(q1, s21, v20);   // vmaddfp v1,v7,v1,v20
  const F v31 = Madd(q1, s31, v19);  // vmaddfp v31,v7,v31,v19
  v30 = Madd(p2, s02, v30);          // vmaddfp v30,v0,v6,v30
  v28 = Madd(p2, s12, v28);          // vmaddfp v28,v0,v5,v28
  v27 = Madd(p2, s22, v27);          // vmaddfp v27,v0,v4,v27
  v26 = Madd(p2, s32, v26);          // vmaddfp v26,v0,v3,v26
  v29 = Madd(c2, s02, v29);          // vmaddfp v29,v13,v6,v29
  v25 = Madd(c2, s12, v25);          // vmaddfp v25,v13,v5,v25
  v24 = Madd(c2, s22, v24);          // vmaddfp v24,v13,v4,v24
  v23 = Madd(c2, s32, v23);          // vmaddfp v23,v13,v3,v23
  const F v6 = Madd(q2, s02, v22);   // vmaddfp v6,v8,v6,v22
  const V s03 = Splat<0x00>(a0);     // vspltw v7,v12,3
  const V p3 = Lvx(base, r5 + 288);  // lvx128 v0,r0,r4
  const F v5 = Madd(q2, s12, v2);    // vmaddfp v5,v8,v5,v2
  const V s13 = Splat<0x00>(a1);     // vspltw v11,v11,3
  const F v4 = Madd(q2, s22, v1);    // vmaddfp v4,v8,v4,v1
  const V s23 = Splat<0x00>(a2);     // vspltw v10,v10,3
  const F m1_0 = Madd(p3, s03, v30);  // vmaddfp v2,v0,v7,v30     M1 fila 0
  const F m1_1 = Madd(p3, s13, v28);  // vmaddfp v1,v0,v11,v28    M1 fila 1
  const V s33 = Splat<0x00>(a3);      // vspltw v9,v9,3
  const F v3 = Madd(q2, s32, v31);    // vmaddfp v3,v8,v3,v31
  const F m1_2 = Madd(p3, s23, v27);  // vmaddfp v31,v0,v10,v27   M1 fila 2
  const V c3 = Lvx(base, pase_c + 48);  // lvx128 v13,r0,r3
  const F m3_0 = Madd(c3, s03, v29);  // vmaddfp v30,v13,v7,v29   M3 fila 0
  const V q3 = Lvx(base, r5 + 352);   // lvx128 v12,r0,r11
  const F m1_3 = Madd(p3, s33, v26);  // vmaddfp v0,v0,v9,v26     M1 fila 3
  const F m2_0 = Madd(q3, s03, v6);   // vmaddfp v7,v12,v7,v6     M2 fila 0
  const F m3_1 = Madd(c3, s13, v25);  // vmaddfp v29,v13,v11,v25  M3 fila 1
  const F m2_1 = Madd(q3, s13, v5);   // vmaddfp v11,v12,v11,v5   M2 fila 1
  const F m3_2 = Madd(c3, s23, v24);  // vmaddfp v28,v13,v10,v24  M3 fila 2
  const F m2_2 = Madd(q3, s23, v4);   // vmaddfp v10,v12,v10,v4   M2 fila 2
  const F m3_3 = Madd(c3, s33, v23);  // vmaddfp v13,v13,v9,v23   M3 fila 3
  const F m2_3 = Madd(q3, s33, v3);   // vmaddfp v9,v12,v9,v3     M2 fila 3
  mayor = Mayor(mayor, Mayor(Mayor(Abs(c2), Abs(p2)), Mayor(Mayor(Abs(q2), Abs(p3)), Mayor(Abs(c3), Abs(q3)))));
  // To the frame. If it turns out at the end that there was a NaN and it is left to the original, nothing
  // happens: it writes all of this again before reading it.
  m.Escribir32(r1 + 96, r5 + 48);  // stw r6,96(r1)
  Stvx(m, r1 + 128, En(m1_0));     // stvx v2
  Stvx(m, r1 + 256, En(m3_0));     // stvx v30
  Stvx(m, r1 + 144, En(m1_1));     // stvx v1
  Stvx(m, r1 + 192, En(m2_0));     // stvx v7
  Stvx(m, r1 + 160, En(m1_2));     // stvx v31
  Stvx(m, r1 + 272, En(m3_1));     // stvx v29
  Stvx(m, r1 + 208, En(m2_1));     // stvx v11
  Stvx(m, r1 + 176, En(m1_3));     // stvx v0
  Stvx(m, r1 + 288, En(m3_2));     // stvx v28
  Stvx(m, r1 + 224, En(m2_2));     // stvx v10
  Stvx(m, r1 + 304, En(m3_3));     // stvx v13
  Stvx(m, r1 + 240, En(m2_3));     // stvx v9

  // --- The rigid inverse of A and E' = E x inv, G' = G x inv (16397-16577) ---
  const F uno = simde_mm_cvtepi32_ps(simde_mm_set1_epi32(1));                // vspltisw v27,1; vcfsx v12,v27,0
  const F menos_uno = Opaco(simde_mm_cvtepi32_ps(simde_mm_set1_epi32(-1)));  // vspltisw v6,-1; vcfsx v0,v6,0
  const V cero = simde_mm_set1_epi32(0);                                     // vspltisw v8,0
  const V b3 = Lvx(base, r4 + 48);   // lvx128 v11,r0,r5 (segunda lectura de A: r31 = [r1+412] = r4)
  const V b0 = Lvx(base, r4);        // lvx128 v10,r0,r31
  const V b2 = Lvx(base, r4 + 32);   // lvx128 v7,r0,r3
  const V b1 = Lvx(base, r4 + 16);   // lvx128 v9,r0,r4
  const V gv = Lvx(base, pos_g);     // lvx128 v13,r0,r10
  const F a3n = simde_mm_mul_ps(Fl(b3), menos_uno);                          // vmulfp128 v11,v11,v0
  const F w1 = Opaco(Fl(simde_mm_alignr_epi8(cero, En(uno), 12)));           // vsldoi v0,v8,v12,4: (0,0,0,1)
  const F xyz1 = Opaco(Fl(simde_mm_alignr_epi8(En(uno), cero, 12)));         // vsldoi v12,v12,v8,4: (1,1,1,0)
  // E: the lvx128 of r1+80 (16-byte aligned) reads the words of the stfs at +80, +84, +88 and +92. Those stfs
  // are not written: the stvx of E' overwrites that same whole block.
  const uint32_t ex = Leer32(base, r5 + 48);  // lfs f0,0(r11), f13,4(r11), f12,8(r11) con r11 = [r1+96] = r5+48
  const uint32_t ey = Leer32(base, r5 + 52);
  const uint32_t ez = Leer32(base, r5 + 56);
  const uint32_t ew = Leer32(base, kUno);     // lfs f0,12344(r11); stfs f0,92(r1)
  const V ev = simde_mm_set_epi32(int32_t(ex), int32_t(ey), int32_t(ez), int32_t(ew));
  const F e = Fl(ev);
  mayor = Mayor(mayor, Mayor(Abs(gv), Abs(ev)));
  const F d8 = simde_mm_dp_ps(a3n, Fl(b0), 0xEF);            // vmsum3fp128 v8,v11,v10
  const F d6 = simde_mm_dp_ps(a3n, Fl(b1), 0xEF);            // vmsum3fp128 v6,v11,v9
  const F d11 = simde_mm_dp_ps(a3n, Fl(b2), 0xEF);           // vmsum3fp128 v11,v11,v7
  const F m8 = Opaco(simde_mm_mul_ps(d8, w1));               // vmulfp128 v8,v8,v0 (the original stores it: 2 uses)
  const F m6 = Opaco(simde_mm_mul_ps(d6, w1));               // vmulfp128 v6,v6,v0
  const F g4 = simde_mm_dp_ps(Fl(gv), w1, 0xFF);             // vmsum4fp128 v4,v13,v0
  const F m5 = Opaco(simde_mm_mul_ps(d11, w1));              // vmulfp128 v5,v11,v0
  const F i0 = simde_mm_add_ps(simde_mm_mul_ps(xyz1, Fl(b0)), m8);  // vmaddfp v10,v12,v10,v8
  const F ew1 = simde_mm_dp_ps(e, w1, 0xFF);                 // vmsum4fp128 v8,v11,v0
  const F i1 = simde_mm_add_ps(simde_mm_mul_ps(xyz1, Fl(b1)), m6);  // vmaddfp v0,v12,v9,v6
  const F i2 = simde_mm_add_ps(simde_mm_mul_ps(xyz1, Fl(b2)), m5);  // vmaddfp v12,v12,v7,v5
  const F e0 = simde_mm_dp_ps(e, i0, 0xFF);                  // vmsum4fp128 v9,v11,v10
  const F e1 = simde_mm_dp_ps(e, i1, 0xFF);                  // vmsum4fp128 v7,v11,v0
  const F g0 = simde_mm_dp_ps(Fl(gv), i0, 0xFF);             // vmsum4fp128 v10,v13,v10
  const F g1 = simde_mm_dp_ps(Fl(gv), i1, 0xFF);             // vmsum4fp128 v0,v13,v0
  const F e2 = simde_mm_dp_ps(e, i2, 0xFF);                  // vmsum4fp128 v11,v11,v12
  const F g2 = simde_mm_dp_ps(Fl(gv), i2, 0xFF);             // vmsum4fp128 v13,v13,v12
  const V t12 = simde_mm_unpackhi_epi32(En(ew1), En(e1));    // vmrghw v12,v7,v8
  const V t0 = simde_mm_unpackhi_epi32(En(g4), En(g1));      // vmrghw v0,v0,v4
  const V t11 = simde_mm_unpackhi_epi32(En(e2), En(e0));     // vmrghw v11,v9,v11
  const V t13 = simde_mm_unpackhi_epi32(En(g2), En(g0));     // vmrghw v13,v10,v13
  const V ep = simde_mm_unpackhi_epi32(t12, t11);            // vmrghw v10,v11,v12: E'
  const V gp = simde_mm_unpackhi_epi32(t0, t13);             // vmrghw v9,v13,v0:   G'
  uint32_t tabla = Leer32(base, r3 + 12);                    // lwz r10,12(r29)
  const uint32_t mango572 = Leer32(base, tabla + 572);       // lwz r4,572(r10)
  Stvx(m, r1 + 80, ep);                                      // stvx v10 (overwrites the stfs at +80..+92)
  Stvx(m, r1 + 112, gp);                                     // stvx v9
  Barrera();  // all the vector work, before any mode change or call
  if (HayNaN(mayor)) [[unlikely]] {  // a NaN in the input: the original, from the start (see header)
    return a_la_original(kPorNaN);
  }
  esc.Empezar(r1);

  // --- The writers, rereading the table and the effect like the original (16578-16736) ---
  uint32_t r3_final = 0;
  bool normalizo = false;
  if (mango572 != 0) {
    r3_final = esc.Matriz(Leer32(base, r3 + 28), mango572, r1 + 128, kVuelta572);
  }
  tabla = Leer32(base, r3 + 12);  // loc_82453C30
  const uint32_t mango568 = Leer32(base, tabla + 568);
  if (mango568 != 0) {
    r3_final = esc.Matriz(Leer32(base, r3 + 28), mango568, r4, kVuelta568);  // mr r5,r31 (= [r1+412])
  }
  tabla = Leer32(base, r3 + 12);  // loc_82453C4C
  const uint32_t mango564 = Leer32(base, tabla + 564);
  if (mango564 != 0) {
    normalizo = true;
    Normalizar(m, ctx, r1 + 320, kPases + kVectorPase + ((3u * indice) << 7));
    r3_final = esc.Vector(Leer32(base, r3 + 28), mango564, r1 + 320, kVuelta564);  // mr r4,r9
  }
  tabla = Leer32(base, r3 + 12);  // loc_82453C8C
  const uint32_t mango468 = Leer32(base, tabla + 468);
  if (mango468 != 0) {
    r3_final = esc.Matriz(Leer32(base, r3 + 28), mango468, r1 + 256, kVuelta468);
  }
  tabla = Leer32(base, r3 + 12);  // loc_82453CA8: lwz r3,12(r29)
  r3_final = tabla;
  const uint32_t mango576 = Leer32(base, tabla + 576);
  if (mango576 != 0) {
    r3_final = esc.Matriz(Leer32(base, r3 + 28), mango576, r1 + 192, kVuelta576);
  }
  tabla = Leer32(base, r3 + 12);  // loc_82453CC4
  const uint32_t mango284 = Leer32(base, tabla + 284);
  const bool con292 = Leer32(base, tabla + 292) != 0;  // decided before the +284 writer
  if (mango284 != 0) {
    r3_final = esc.Vector(Leer32(base, r3 + 28), mango284, r1 + 80, kVuelta284);
  }
  if (con292) {
    tabla = Leer32(base, r3 + 12);  // loc_82453D1C: the handle is read again
    r3_final = esc.Vector(Leer32(base, r3 + 28), Leer32(base, tabla + 292), r1 + 112, kVuelta292);
  }
  return {Camino::kCalculo, r3_final, normalizo, 0};
}

// ---------------------------------------------------------------------------------------------------------------
// Counters, report and shutdown (no read-modify-write atomics: A57 without LSE).
// ---------------------------------------------------------------------------------------------------------------
constexpr uint64_t kComprobaciones = 100000;  // of each path; then 1 of every kPeriodo
constexpr uint64_t kPeriodo = 4096;
constexpr uint32_t kMaxAnotaciones = 96;      // 6 from the prologue and the cache, 1 + 12 + 2 from the frame, 3 from the
                                              // normalization and up to 4 x 5 + 3 x 2 from the writers
constexpr uint32_t kMarcoVigilado = kMarco + 32;  // the frame and the caller's r3/r4 slots (up to r1+416)

enum Tipo : uint32_t { kTipoCache = 0, kTipoCalculo = 1, kTipos = 2 };
constexpr const char* kNombres[kTipos] = {"cache", "calculo"};

struct Contadores {
  std::atomic<uint64_t> llamadas{0};
  std::atomic<uint64_t> nativas{0};       // for the report period
  std::atomic<uint64_t> originales{0};    // for the period: turned off or left to the original
  std::atomic<uint64_t> comprobadas{0};   // in the period
  std::atomic<uint64_t> comprobadas_total{0};
};
Contadores g_c[kTipos];
std::atomic<uint64_t> g_motivos[kMotivos];  // for the period: why it went to the original (stack, frame, NaN)
std::atomic<bool> g_apagado{false};
std::atomic<int64_t> g_siguiente_ms{0};

template <typename T>
inline void Sumar(std::atomic<T>& c, T n) {
  c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

std::atomic<int8_t> g_activo{-1};
inline bool Activo() {
  int8_t a = g_activo.load(std::memory_order_relaxed);
  if (a < 0) [[unlikely]] {
    a = REXCVAR_GET(nfsmw_matrices_nativo) ? 1 : 0;
    g_activo.store(a, std::memory_order_relaxed);
  }
  return a != 0;
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
    REXLOG_INFO("[matrices] sub_824538D0 en nativo (build 176); se comprueban contra la original las primeras {} "
                "llamadas de cada camino (cache y calculo) y despues 1 de cada {}",
                kComprobaciones, kPeriodo);
    return;
  }
  std::string linea;
  for (uint32_t t = 0; t < kTipos; ++t) {
    Contadores& c = g_c[t];
    linea += fmt::format(" | {}: {} nativas, {} originales, {} comprobadas", kNombres[t],
                         c.nativas.exchange(0, std::memory_order_relaxed),
                         c.originales.exchange(0, std::memory_order_relaxed),
                         c.comprobadas.exchange(0, std::memory_order_relaxed));
  }
  NFSMW_INFORME_DIFERIDO("[matrices] ultimos 10 s{} | a la original por pila desalineada {}, entrada en el marco {}, NaN {}{}",
              linea, g_motivos[kPorPila].exchange(0, std::memory_order_relaxed),
              g_motivos[kPorMarco].exchange(0, std::memory_order_relaxed),
              g_motivos[kPorNaN].exchange(0, std::memory_order_relaxed),
              g_apagado.load(std::memory_order_relaxed) ? " | APAGADA por diferencia" : "");
}

std::string Hex(const uint8_t* bytes, uint32_t n) {
  static const char kDigitos[] = "0123456789ABCDEF";
  std::string s;
  for (uint32_t i = 0; i < n; ++i) {
    s += kDigitos[bytes[i] >> 4];
    s += kDigitos[bytes[i] & 15];
  }
  return s;
}

void Fotografiar(Registro& r, uint8_t* base) {
  for (uint32_t i = 0; i < r.n; ++i) {
    std::memcpy(r.a[i].despues, Puntero(base, r.a[i].direccion), r.a[i].bytes);
  }
}
void Deshacer(const Registro& r, uint8_t* base) {
  for (uint32_t i = r.n; i-- > 0;) {
    std::memcpy(Puntero(base, r.a[i].direccion), r.a[i].antes, r.a[i].bytes);
  }
}
uint32_t PrimeraDistinta(const Registro& r, uint8_t* base) {
  for (uint32_t i = 0; i < r.n; ++i) {
    if (std::memcmp(Puntero(base, r.a[i].direccion), r.a[i].despues, r.a[i].bytes) != 0) {
      return i;
    }
  }
  return r.n;
}

// The bytes of the frame (and the caller's slots) one by one: what is in guest memory.
void CopiarMarco(uint8_t* base, uint32_t desde, uint8_t* salida) {
  for (uint32_t i = 0; i < kMarcoVigilado; ++i) {
    salida[i] = *Puntero(base, desde + i);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Normal path and guard.
// ---------------------------------------------------------------------------------------------------------------
inline void Rapido(PPCContext& ctx, uint8_t* base, Contadores& c) {
  Memoria<false> m{base, nullptr};
  EscritorasPorGancho esc{ctx, base};
  const uint32_t pila = ctx.r1.u32;
  const Salida s = Nativa(m, ctx, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, pila, esc);
  if (s.camino == Camino::kOriginal) [[unlikely]] {
    Sumar(c.originales, uint64_t(1));
    Sumar(g_motivos[s.motivo], uint64_t(1));
    __imp__sub_824538D0(ctx, base);  // from the entry state (the native version has not touched the context)
    return;
  }
  if (s.camino == Camino::kCalculo) {
    ctx.r3.u64 = s.r3;
  }
  ctx.r1.s64 = ctx.r1.s64 + kMarco;  // addi r1,r1,384 (Empezar did the stwu)
  Sumar(c.nativas, uint64_t(1));
}

void NotarComprobada(uint32_t tipo) {
  Contadores& c = g_c[tipo];
  Sumar(c.comprobadas, uint64_t(1));
  const uint64_t total = c.comprobadas_total.load(std::memory_order_relaxed) + 1;
  c.comprobadas_total.store(total, std::memory_order_relaxed);
  if (total == kComprobaciones && !g_apagado.load(std::memory_order_relaxed)) {
    REXLOG_INFO("[matrices] sub_824538D0 (camino {}): {} llamadas comprobadas contra la original byte a byte, 0 "
                "diferencias: camino nativo en marcha",
                kNombres[tipo], kComprobaciones);
  }
}

// Guard: the native version recording, undo, the original, compare. Always leaves the original's state.
[[gnu::noinline]] void Comprobar(PPCContext& ctx, uint8_t* base, uint32_t tipo) {
  const PPCRegister r3e = ctx.r3, r4e = ctx.r4, r5e = ctx.r5, r1e = ctx.r1, r12e = ctx.r12;
  const uint64_t lre = ctx.lr;
  const uint32_t csr_e = ctx.fpscr.csr;
  const uint32_t marco = r1e.u32 - kMarco;
  Anotacion anotaciones[kMaxAnotaciones];
  Registro reg{anotaciones, kMaxAnotaciones};
  Memoria<true> m{base, &reg};
  EscritorasCopia<true> esc{m};
  const Salida s = Nativa(m, ctx, r3e.u32, r4e.u32, r5e.u32, r1e.u32, esc);
  const uint32_t csr_nativa = ctx.fpscr.csr;
  Fotografiar(reg, base);
  Deshacer(reg, base);
  // The frame the original has to leave: the current one with what the native version wrote on top.
  uint8_t esperado[kMarcoVigilado];
  CopiarMarco(base, marco, esperado);
  for (uint32_t i = 0; i < reg.n; ++i) {
    for (uint32_t b = 0; b < reg.a[i].bytes; ++b) {
      const uint32_t k = reg.a[i].direccion + b - marco;
      if (k < kMarcoVigilado) {
        esperado[k] = reg.a[i].despues[b];
      }
    }
  }
  // The original from the entry state.
  ctx.r3 = r3e;
  ctx.r4 = r4e;
  ctx.r5 = r5e;
  ctx.r1 = r1e;
  ctx.r12 = r12e;
  ctx.lr = lre;
  if (ctx.fpscr.csr != csr_e) {
    ctx.fpscr.csr = csr_e;
    ctx.fpscr.setcsr(csr_e);
  }
  __imp__sub_824538D0(ctx, base);
  if (s.camino == Camino::kOriginal) {
    Sumar(g_c[tipo].originales, uint64_t(1));
    Sumar(g_motivos[s.motivo], uint64_t(1));
    return;  // the native version stepped aside without computing: nothing to compare
  }

  const uint32_t fm = uint32_t(PPCFPSCRRegister::FlushMask);
  const uint32_t csr_esperado =
      s.camino == Camino::kCache ? csr_e : (s.normalizo ? (csr_e & ~fm) : (csr_e | fm));
  const uint64_t r3_esperado = s.camino == Camino::kCache ? r3e.u64 : uint64_t(s.r3);
  PPCRegister r1x = r1e;
  r1x.u32 = marco;           // stwu r1,-384(r1)
  r1x.s64 = r1x.s64 + kMarco;  // addi r1,r1,384
  uint8_t ahora[kMarcoVigilado];
  CopiarMarco(base, marco, ahora);
  const uint32_t mala = PrimeraDistinta(reg, base);
  uint32_t byte_marco = kMarcoVigilado;
  for (uint32_t k = 0; k < kMarcoVigilado; ++k) {
    if (ahora[k] != esperado[k]) {
      byte_marco = k;
      break;
    }
  }
  const char* motivo = nullptr;
  if (reg.lleno) motivo = "demasiadas escrituras";
  else if (mala < reg.n) motivo = "bytes distintos";
  else if (byte_marco < kMarcoVigilado) motivo = "marco de pila distinto";
  else if (ctx.r3.u64 != r3_esperado) motivo = "r3 distinto";
  else if (ctx.r1.u64 != r1x.u64) motivo = "r1 distinto";
  else if (ctx.fpscr.csr != csr_esperado) motivo = "FPCR de la original distinto";
  else if (csr_nativa != csr_esperado) motivo = "FPCR de la nativa distinto";
  NotarComprobada(tipo);
  if (!motivo) {
    return;
  }
  g_apagado.store(true, std::memory_order_relaxed);  // the state is already the original's
  const Anotacion* a = mala < reg.n ? &reg.a[mala] : nullptr;
  REXLOG_INFO("[matrices] DIFERENCIA en sub_824538D0 ({}; camino {}, comprobacion {}): r3 0x{:08X} r4 0x{:08X} r5 "
              "0x{:08X} r1 0x{:08X}; escritoras {}; direccion 0x{:08X} nativa {} original {}; primer byte distinto "
              "del marco: {}; r3 nativa 0x{:X} original 0x{:X}; FPCR entrada 0x{:X} nativa 0x{:X} original 0x{:X}. "
              "Camino nativo APAGADO para siempre, se queda la original",
              motivo, kNombres[tipo], g_c[tipo].comprobadas_total.load(std::memory_order_relaxed), r3e.u32, r4e.u32,
              r5e.u32, r1e.u32, esc.llamadas, a ? a->direccion : 0u, a ? Hex(a->despues, a->bytes) : std::string("-"),
              a ? Hex(Puntero(base, a->direccion), a->bytes) : std::string("-"),
              byte_marco < kMarcoVigilado ? fmt::format("r1+{}", byte_marco) : std::string("ninguno"), r3_esperado,
              ctx.r3.u64, csr_e, csr_nativa, ctx.fpscr.csr);
}

// The same input as last time? (the cache path; the native version checks it again)
inline bool MismaEntrada(uint8_t* base, const PPCContext& ctx) {
  return Leer32(base, kUltima + 0) == ctx.r5.u32 && Leer32(base, kUltima + 4) == ctx.r4.u32 &&
         Leer32(base, kUltima + 8) == ctx.r3.u32;
}

inline void Matrices(PPCContext& ctx, uint8_t* base) {
  if (!Activo()) {
    __imp__sub_824538D0(ctx, base);
    return;
  }
  const uint32_t tipo = MismaEntrada(base, ctx) ? kTipoCache : kTipoCalculo;
  Contadores& c = g_c[tipo];
  const uint64_t n = c.llamadas.load(std::memory_order_relaxed) + 1;
  c.llamadas.store(n, std::memory_order_relaxed);
  if (g_apagado.load(std::memory_order_relaxed)) [[unlikely]] {
    Sumar(c.originales, uint64_t(1));
    __imp__sub_824538D0(ctx, base);
  } else if (n <= kComprobaciones || (n & (kPeriodo - 1)) == 0) [[unlikely]] {
    Comprobar(ctx, base, tipo);
  } else {
    Rapido(ctx, base, c);
  }
  if ((n & (kPeriodo - 1)) == 0) [[unlikely]] {
    Informe();
  }
}

}  // namespace
}  // namespace nfsmw::matrices

// The hook. The 16 calls in the generated code must go to sub_824538D0 and not to __imp__sub_824538D0:
// tools/llamadas_directas.py leaves them as sub_824538D0 because this file names the address.
REX_HOOK_RAW(sub_824538D0) {
  nfsmw::matrices::Matrices(ctx, base);
}
