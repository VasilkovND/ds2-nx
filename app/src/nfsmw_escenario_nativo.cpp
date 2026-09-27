// nfsmw - ScenerySectionHeader::DrawAScenery (sub_824C2850) in native code.
//
// WHAT IT IS (PowerPC read instruction by instruction in nfsmw_recomp.70.cpp:18450-19374)
//   Called by the thread that prepares the frames (XThreadA6918080 in one console log; not the Main
//   XThread, which runs them) from TreeCull (sub_824C2F48), sub_824C3078 and sub_824C2EE8: once per
//   candidate scenery object of each view. In a race, 87,500 calls per second on average and 124,000 in the
//   alley. r3 = ScenerySectionHeader, r4 = instance number, r5 = SceneryCullInfo, r6 = visibility state
//   (1 = partial). 304-byte frame.
//   1. The instance: 64 bytes at [r3+32] + 64 * r4. If the view has a preculler section ([r5+196] >= 0)
//      and the instance's bit in the table [r3+48] says "not visible", out.
//   2. The instance's ExcludeFlags (+24) against the view's (+132), mask 0x080000FF (with 0x08000040 if
//      the instance carries 0x08000000 or 0x40): if any match, out.
//   3. Partial visibility: the instance's box at r1+80 and r1+112 and GetVisibleState (sub_8243E7D8,
//      through its hook).
//   4. InlinedViewGetPixelSize (sub_824C2720, a single caller: inlined here): the distance to the camera,
//      written to r1+80, and the size in pixels (fctiwz, which goes through r1-16).
//   5. The mesh: threshold of 32 px (23 in view 20 with 0x1000) in views with 0x800 or 0x1000, 32 with
//      0x20, 23 with view mode >= 3 and 18 in the normal camera, where LOD decides: distance < K
//      ([0x82062AC8]) or pixels / max(Density, 6) >= 8.7 -> the full one ([info+40]); otherwise the
//      reduced one ([info+48]).
//   6. With bit 0x200 of the instance, a SceneryDrawInfo without a matrix. Otherwise eFrameMallocMatrix
//      (820E_A568), the instance's rotation (8243_F8D8: 9 16-bit integers times 1/8192, inlined), the
//      position and, depending on the view's bits, the extra height for shadows (0x100), the mirror
//      (0x800) and the vegetation wind (0x4000: CreateWindRotMatrix 824F_D7C0 and bMulMatrix 8263_D020,
//      which are called as they are). In views 1 and 2 it looks for position markers in the eSolid for
//      the light flares (8221_9EA8).
//
// WHY NATIVE
//   It is the costliest game function left on the thread that prepares the frames: 5.5 % of a core
//   including its frameless leaves in stack sampling, and 72.6 ms/s on average in a race (0.83 us per
//   call, inclusive) with peaks of 109 ms/s. That thread is on the critical path: in the alley measurement
//   window the Main XThread spends 1,606 ms out of every 10 s waiting for commands the preparer has not
//   written yet, and in 3 of the 7 race stutters the one running late is the preparer. Recompiled, every
//   instruction goes through the context and through volatile loads and stores; the rotation, the
//   allocation and the pixel size are three more calls. Here everything stays in registers and only
//   GetVisibleState and, with wind, the two wind functions are called.
//
// WHY IT IS BIT-IDENTICAL
//   - Floating point: the same computation, operation by operation and with the same expression as the
//     generated code (double(float(a op b)) for fadds/fsubs/fmuls/fdivs, std::fma for fmadds, sqrt for
//     fsqrts and the same fctiwz expression). Between product and sum there is always an explicit float()
//     or std::fma, so GCC's FMA contraction cannot change anything. With any NaN in a floating-point
//     input (box, position, camera, radius, Density and the constants) the original runs: with two NaNs,
//     which one propagates depends on the operand order the compiler picks. Without NaN in the input,
//     lfs + stfs is a copy of the word.
//   - Denormal mode: the original's, without flush (disableFlushMode) throughout the scalar part, with a
//     compiler barrier after the change so that no conversion is placed before it; it exits without
//     flush, like the original.
//   - Memory: the same writes with the same bytes: the prologue (three doubles set to zero, which are
//     local variables in the generated code, and the frame back link), the box, the distance, the fctiwz
//     at r1-16, the std of the size, the std of the rotation at r1-32, -24 and -16, the matrix allocator
//     (0x82A2C3B4 and, when out of space, 0x82A2C3C4/C8), the matrix, the SceneryDrawInfo and [cull+140].
//   - Registers: the three callers (TreeCull, sub_824C3078 and sub_824C2EE8) only read local variables
//     after the call, and through their return r3 and f1 stay live (the liveness analysis over all the
//     generated code gives the same after GetVisibleState in those functions). r3, f1, r1, r12 and lr are
//     left as the original leaves them; GetVisibleState and the wind functions touch them themselves.
//     cr6, xer, r22-r31 and f29-f31 are local variables of the generated code.
//   - The flares with position markers (views 1 and 2) and a nonzero nfsmw_escenario_detalle go to the
//     whole original, which calls its own hooks. This is decided before writing anything outside the
//     stack.
//
// SELF-CHECKING GUARD (cvar nfsmw_escenario_nativo; project rule)
//   The first kComprobaciones calls, the first kMinimoViento of the wind path and then 1 of every
//   kPeriodo: the stack (kPila bytes below r1), [cull+140], the free SceneryDrawInfo, the matrix allocator
//   and the block that would be allocated are snapshotted; the native version runs, what it leaves is
//   saved and undone (whole memory and context), the original runs and they are compared byte by byte,
//   along with r3, f1, r1, r12, lr and the FPCR. The original's state is always kept. A single difference
//   turns the native version off for the session and writes "[escenario] DIFERENCIA" (REXLOG_ERROR).
//   "[escenario]" line every 10 s with the counts.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_informe_diferido.h"
#include <rex/platform.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

REXCVAR_DEFINE_BOOL(nfsmw_escenario_nativo, true, "NFSMW",
                    "ScenerySectionHeader::DrawAScenery (sub_824C2850: culling, tamano en pixeles, LOD y SceneryDrawInfo "
                    "de cada objeto de escenario) en nativo (build 184), identico bit a bit. Se comprueba contra la "
                    "original (las primeras 100.000 llamadas, las primeras 5.000 con viento y despues 1 de cada 4096) "
                    "y se apaga sola si difiere; false = la original")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The LOD setting of nfsmw_escenario_lod.cpp (its hook of sub_824C2720). Nonzero: the original runs, since
// it uses it.
REXCVAR_DECLARE(int32_t, nfsmw_escenario_detalle);

REX_EXTERN(__imp__sub_824C2850);  // la original
REX_EXTERN(sub_8243E7D8);         // GetVisibleState through its hook, like the original (itself native)
// CreateWindRotMatrix and bMulMatrix, the originals. The name is assembled from parts on purpose:
// tools/llamadas_directas.py treats any address that appears whole in app/src as hooked, and their calls
// from the generated code must remain direct (__imp__, inlinable).
#define NFSMW_ESCENARIO_UNIR_(a, b) a##b
#define NFSMW_ESCENARIO_VIENTO NFSMW_ESCENARIO_UNIR_(__imp__sub_824F, D7C0)
#define NFSMW_ESCENARIO_MULTIPLICAR NFSMW_ESCENARIO_UNIR_(__imp__sub_8263, D020)
REX_EXTERN(NFSMW_ESCENARIO_VIENTO);
REX_EXTERN(NFSMW_ESCENARIO_MULTIPLICAR);

namespace nfsmw::escenario_nativo {
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
[[gnu::always_inline]] inline uint16_t Leer16(uint8_t* base, uint32_t direccion) {
  uint16_t v;
  std::memcpy(&v, Puntero(base, direccion), 2);
  return __builtin_bswap16(v);
}
[[gnu::always_inline]] inline uint32_t Leer32(uint8_t* base, uint32_t direccion) {
  uint32_t v;
  std::memcpy(&v, Puntero(base, direccion), 4);
  return __builtin_bswap32(v);
}
[[gnu::always_inline]] inline void Escribir32(uint8_t* base, uint32_t direccion, uint32_t valor) {
  valor = __builtin_bswap32(valor);
  std::memcpy(Puntero(base, direccion), &valor, 4);
}
[[gnu::always_inline]] inline void Escribir64(uint8_t* base, uint32_t direccion, uint64_t valor) {
  valor = __builtin_bswap64(valor);
  std::memcpy(Puntero(base, direccion), &valor, 8);
}

// lfs: the guest word as a float, widened to double, like "double(temp.f32)" in the generated code.
[[gnu::always_inline]] inline double Lfs(uint32_t palabra) {
  float f;
  std::memcpy(&f, &palabra, 4);
  return double(f);
}
// stfs: the double to float, like "temp.f32 = float(ctx.fN.f64)".
[[gnu::always_inline]] inline uint32_t Stfs(double d) {
  const float f = float(d);
  uint32_t palabra;
  std::memcpy(&palabra, &f, 4);
  return palabra;
}
[[gnu::always_inline]] inline bool EsNaN(uint32_t palabra) {
  return (palabra & 0x7FFFFFFFu) > 0x7F800000u;
}
// fctiwz, with the same expression as the generated code.
[[gnu::always_inline]] inline int64_t Fctiwz(double v) {
  return std::isnan(v) ? int64_t(0x80000000U) : (v >= double(INT_MAX)) ? INT_MAX : simde_mm_cvttsd_si32(simde_mm_load_sd(&v));
}
// Compiler barrier: no memory reads or writes (nor anything that depends on them) cross an FPCR change.
[[gnu::always_inline]] inline void Barrera() {
  __asm__ __volatile__("" ::: "memory");
}

// ---------------------------------------------------------------------------------------------------------------
// Fixed addresses (PowerPC lis + offset) and return addresses of each bl (ctx.lr in the generated code).
// ---------------------------------------------------------------------------------------------------------------
constexpr uint32_t kMarco = 304;                   // stwu r1,-304(r1)
constexpr uint32_t kPila = 1536;                   // what is watched below r1: this frame, GetVisibleState and the wind
constexpr uint32_t kSeis = 0x82060E70;             // lfs f31,-3704(r27): 6.0, the Density minimum
constexpr uint32_t kDistanciaK = 0x82062AC8;       // lfs f13,3552(r27): K, 25.0 as measured on the console
constexpr uint32_t kUmbralLod = 0x82060BBC;        // lfs f13,-4396(r27): 8,7
constexpr uint32_t kUno = 0x82063038;              // lfs f29,4944(r27): the w of the position
constexpr uint32_t kAlturaExtra = 0x82063970;      // lfs f0,7304(r27): EnvMapShadowExtraHeight
constexpr uint32_t kSesenta = 0x82057114;          // lfs f0,28948(0x82050000): the 60 of the wind
constexpr uint32_t kCero = 0x82061CE8;             // lfs f13,7400(0x82060000) of the rotation: the w column
constexpr uint32_t kEscalaRotacion = 0x820AFF50;   // lfs f0,-176(0x820B0000) of the rotation: 1/8192
constexpr uint32_t kModoVista = 0x82A2CEE4;        // lwz r11,-12572(0x82A30000): eGetCurrentViewMode()
constexpr uint32_t kReservaActual = 0x82A2C3B4;    // eFrameMalloc: the free pointer
constexpr uint32_t kReservaFin = 0x82A2C3B8;       //   y su final
constexpr uint32_t kReservaAgotada = 0x82A2C3C4;   // out of space: the flag
constexpr uint32_t kReservaPerdida = 0x82A2C3C8;   // and the requested bytes
constexpr uint32_t kVistas = 0x82A38070;           // addi r26,r11,-32656 con r11 = 0x82A40000
constexpr uint32_t kVista1 = kVistas + 112;        // eGetView(1)
constexpr uint32_t kVista2 = kVistas + 224;        // eGetView(2)
constexpr uint32_t kMagia360 = 0xB60B60B7u;        // lis r4,-18933; ori r10,r4,24759: divide by 360
constexpr uint32_t kVueltaPrologo = 0x824C2858;    // bl __savegprlr_22 (the generated code does not execute it)
constexpr uint32_t kVueltaVisible = 0x824C2968;
constexpr uint32_t kVueltaPixeles = 0x824C299C;
constexpr uint32_t kVueltaReserva = 0x824C2B58;
constexpr uint32_t kVueltaRotacion = 0x824C2B70;
constexpr uint32_t kVueltaViento = 0x824C2C74;
constexpr uint32_t kVueltaMultiplicar = 0x824C2C84;
constexpr uint32_t kVueltaMarcador = 0x824C2CE8;

// [inicio, inicio + n) toca la pila vigilada [pila - kPila, pila)?
[[gnu::always_inline]] inline bool EnPila(uint32_t inicio, uint32_t n, uint32_t pila) {
  return uint64_t(inicio) + n > uint64_t(pila) - kPila && uint64_t(inicio) < uint64_t(pila);
}

enum Camino : uint32_t { kDescarte = 0, kSinMatriz = 1, kConMatriz = 2, kConViento = 3, kCaminos = 4 };
constexpr const char* kNombresCamino[kCaminos] = {"descarte", "sin matriz", "con matriz", "con viento"};
enum Motivo : uint32_t {
  kPorApagada = 0,
  kPorDetalle = 1,
  kPorPila = 2,
  kPorNaN = 3,
  kPorDestellos = 4,
  kPorComprobar = 5,  // the wind, until its first kMinimoViento are checked: left to the guard
  kMotivos = 6
};

struct Salida {
  bool hecha;       // true: the state is the original's; false: neither memory outside the stack nor context touched
  uint32_t motivo;  // if not done
  uint32_t camino;  // if done
};

constexpr uint64_t kComprobaciones = 100000;  // first calls checked (a little over 1 s of racing)
constexpr uint64_t kMinimoViento = 5000;      // and the first ones of the wind, which calls two originals
constexpr uint64_t kPeriodo = 4096;           // then 1 of every kPeriodo (a power of 2)

// Counters (no read-modify-write atomics: A57 without LSE). Written by the thread that prepares the
// frames; if another thread counted at the same time some count would be lost, which does not matter for
// the report.
std::atomic<uint64_t> g_llamadas{0};
std::atomic<uint64_t> g_nativas[kCaminos];      // for the report period
std::atomic<uint64_t> g_originales[kMotivos];   // for the report period
std::atomic<uint64_t> g_comprobadas[kCaminos];  // since startup, all without differences
std::atomic<uint64_t> g_comprobadas_total{0};
std::atomic<bool> g_apagado{false};
std::atomic<int64_t> g_siguiente_ms{0};
std::atomic<int8_t> g_activo{-1};

template <typename T>
inline void Sumar(std::atomic<T>& c, T n) {
  c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

inline bool Activo() {
  int8_t a = g_activo.load(std::memory_order_relaxed);
  if (a < 0) [[unlikely]] {
    a = REXCVAR_GET(nfsmw_escenario_nativo) ? 1 : 0;
    g_activo.store(a, std::memory_order_relaxed);
  }
  return a != 0;
}

// ---------------------------------------------------------------------------------------------------------------
// The whole of sub_824C2850. kGuardia: called by Comprobar (nothing is left to the guard because of the wind).
// ---------------------------------------------------------------------------------------------------------------
template <bool kGuardia>
Salida Nativa(PPCContext& ctx, uint8_t* base) {
  const uint32_t yo = ctx.r3.u32;      // ScenerySectionHeader
  const uint32_t numero = ctx.r4.u32;  // numero de instancia
  const uint32_t cull = ctx.r5.u32;    // SceneryCullInfo (r25)
  const uint64_t estado = ctx.r6.u64;  // visibility_state (r26)
  const uint32_t pila = ctx.r1.u32;
  const uint32_t marco = pila - kMarco;

  // Nothing that is read can be on the stack that this function and the ones it calls write (the guard
  // watches that).
  if (pila < kPila || EnPila(cull, 200, pila)) {
    return {false, kPorPila, 0};
  }
  const uint32_t inst = Leer32(base, yo + 32) + (numero << 6);  // lwz r9,32(r3); rlwinm r10,r4,6,0,25; add r30,r10,r9
  if (EnPila(inst, 64, pila)) {
    return {false, kPorPila, 0};
  }

  const PPCRegister r1e = ctx.r1, r3e = ctx.r3, r4e = ctx.r4, r5e = ctx.r5, r6e = ctx.r6, r12e = ctx.r12, f1e = ctx.f1;
  const uint64_t lre = ctx.lr;
  // Leaving it to the original halfway: up to that point only the stack has been written (which the original
  // writes again the same way) and the context, which is left as it was.
  const auto a_la_original = [&](uint32_t motivo) -> Salida {
    ctx.r1 = r1e;
    ctx.r3 = r3e;
    ctx.r4 = r4e;
    ctx.r5 = r5e;
    ctx.r6 = r6e;
    ctx.r12 = r12e;
    ctx.f1 = f1e;
    ctx.lr = lre;
    return {false, motivo, 0};
  };
  // loc_824C2B3C: addi r1,r1,304; lfd f29-f31 (disableFlushMode); b __restgprlr_22.
  const auto salir = [&](uint32_t camino) -> Salida {
    ctx.r1.s64 = ctx.r1.s64 + kMarco;
    ctx.fpscr.disableFlushMode();
    return {true, 0, camino};
  };

  // --- Prologo ---
  ctx.r12.u64 = ctx.lr;             // mflr r12
  ctx.lr = kVueltaPrologo;          // bl __savegprlr_22
  ctx.fpscr.disableFlushMode();     // the one of "stfd f29,-112(r1)"
  Escribir64(base, pila - 112, 0);  // stfd f29, f30 and f31: in the generated code, local variables set to zero
  Escribir64(base, pila - 104, 0);
  Escribir64(base, pila - 96, 0);
  Escribir32(base, marco, pila);    // stwu r1,-304(r1)
  ctx.r1.u32 = marco;

  // --- 1. The preculler: the instance's bit in the view's section ---
  const int32_t seccion = int32_t(Leer32(base, cull + 196));  // lwz r11,196(r25); cmpwi; blt
  if (seccion >= 0) {
    const uint32_t fila = uint32_t(int32_t(int16_t(Leer16(base, inst + 28)))) << 7;  // lhz; extsh; rlwinm 7,0,24
    const uint32_t byte = Leer8(base, fila + uint32_t(seccion >> 3) + Leer32(base, yo + 48));  // srawi; add; lbzx
    if ((byte & (1u << (uint32_t(seccion) & 7u))) != 0) {  // slw r11,r4,r7; and; cmpwi; bne
      return salir(kDescarte);                              // r3 = this, sin tocar
    }
  }

  // --- 2. ExcludeFlags ---
  const uint32_t banderas = Leer32(base, inst + 24);  // lwz r11,24(r30) (and the later lwz r9,24(r30))
  uint32_t marcadas = banderas;
  if ((marcadas & 0x08000000u) != 0 || (marcadas & 0x40u) != 0) {
    marcadas |= 0x08000040u;  // oris r11,r11,2048; ori r11,r11,64
  }
  const uint32_t banderas_vista = Leer32(base, cull + 132);  // lwz r5,132(r25) (and the later ones)
  if (((marcadas ^ 0xFFFFFF60u) & banderas_vista & 0x080000FFu) != 0) {  // xor; and; clrlwi 4; rlwinm 0,24,4
    return salir(kDescarte);
  }
  // lhz r9,62(r30); extsh; x 72 (rlwinm, add, rlwinm); add r31,r11,r10 con r10 = [r3+24]
  const uint32_t info = uint32_t(int32_t(int16_t(Leer16(base, inst + 62)))) * 72u + Leer32(base, yo + 24);
  if (EnPila(info, 72, pila)) {
    return a_la_original(kPorPila);
  }

  // --- 3. Visibilidad parcial ---
  uint64_t visibilidad = estado;               // r26
  if (int32_t(uint32_t(estado)) == 1) {        // cmpwi cr6,r26,1
    uint32_t caja[6];
    for (uint32_t i = 0; i < 6; ++i) {
      caja[i] = Leer32(base, inst + 4 * i);
    }
    if (EsNaN(caja[0]) | EsNaN(caja[1]) | EsNaN(caja[2]) | EsNaN(caja[3]) | EsNaN(caja[4]) | EsNaN(caja[5])) {
      return a_la_original(kPorNaN);           // lfs + stfs through double would quiet a signaling NaN
    }
    Escribir32(base, marco + 80, caja[0]);     // stfs f0,80 / f13,84 / f12,88: the minimum
    Escribir32(base, marco + 84, caja[1]);
    Escribir32(base, marco + 88, caja[2]);
    Escribir32(base, marco + 112, caja[3]);    // stfs f11,112 / f10,116 / f9,120: the maximum
    Escribir32(base, marco + 116, caja[4]);
    Escribir32(base, marco + 120, caja[5]);
    ctx.r6.s64 = 0;
    ctx.r5.s64 = ctx.r1.s64 + 112;
    ctx.r4.s64 = ctx.r1.s64 + 80;
    ctx.r3.u64 = Leer32(base, cull + 128);     // lwz r3,128(r25): la vista
    ctx.lr = kVueltaVisible;
    sub_8243E7D8(ctx, base);                   // GetVisibleState
    visibilidad = ctx.r3.u64;                  // mr r26,r3
    if (int32_t(uint32_t(visibilidad)) == 0) {
      return salir(kDescarte);
    }
  }

  // --- 4. InlinedViewGetPixelSize (sub_824C2720), inlined and operation by operation ---
  ctx.fpscr.disableFlushMode();  // the one of "lfs f8,56(r31)" (GetVisibleState exits with flush on)
  Barrera();                     // no computation before this point
  const uint32_t w_radio = Leer32(base, info + 56);
  const uint32_t w_seis = Leer32(base, kSeis);
  const uint32_t w_x = Leer32(base, inst + 32);
  const uint32_t w_y = Leer32(base, inst + 36);
  const uint32_t w_z = Leer32(base, inst + 40);
  const uint32_t w_cx = Leer32(base, cull + 160);
  const uint32_t w_cy = Leer32(base, cull + 164);
  const uint32_t w_cz = Leer32(base, cull + 168);
  const uint32_t w_dx = Leer32(base, cull + 176);
  const uint32_t w_dy = Leer32(base, cull + 180);
  const uint32_t w_dz = Leer32(base, cull + 184);
  const uint32_t w_h = Leer32(base, cull + 192);
  if (EsNaN(w_radio) | EsNaN(w_seis) | EsNaN(w_x) | EsNaN(w_y) | EsNaN(w_z) | EsNaN(w_cx) | EsNaN(w_cy) |
      EsNaN(w_cz) | EsNaN(w_dx) | EsNaN(w_dy) | EsNaN(w_dz) | EsNaN(w_h)) {
    return a_la_original(kPorNaN);
  }
  const double seis = Lfs(w_seis);                            // f31
  const double radio = double(float(Lfs(w_radio) + seis));    // fadds f1,f8,f31
  ctx.f1.f64 = radio;
  ctx.lr = kVueltaPixeles;                                    // bl 0x824c2720
  PPCRegister menos;
  menos.f64 = radio;
  menos.u64 ^= 0x8000000000000000ull;                         // fneg f11,f1
  const double ey = double(float(Lfs(w_y) - Lfs(w_cy)));      // fsubs f0,f0,f10
  double ez = double(float(Lfs(w_z) - Lfs(w_cz)));            // fsubs f13,f13,f9
  double ex = double(float(Lfs(w_x) - Lfs(w_cx)));            // fsubs f12,f12,f7
  const double t6 = double(float(Lfs(w_dy) * ey));            // fmuls f6,f8,f0
  const double t3 = double(float(std::fma(Lfs(w_dz), ez, t6)));  // fmadds f3,f5,f13,f6
  const double t2 = double(float(std::fma(Lfs(w_dx), ex, t3)));  // fmadds f2,f4,f12,f3
  uint64_t r3;
  uint32_t w_distancia = 0;
  if (t2 < menos.f64) {                                       // fcmpu cr6,f2,f11; bge
    r3 = 0;                                                   // li r3,0: behind the camera
  } else {
    ez = double(float(ez * ez));                              // fmuls f13,f13,f13
    const double h = Lfs(w_h);                                // lfs f11,192(r3)
    ex = double(float(std::fma(ex, ex, ez)));                 // fmadds f12,f12,f12,f13
    const double d2 = double(float(std::fma(ey, ey, ex)));    // fmadds f10,f0,f0,f12
    double tam = h;                                           // fmr f12,f11
    const double distancia = double(float(std::sqrt(d2)));   // fsqrts f0,f10
    w_distancia = Stfs(distancia);
    Escribir32(base, marco + 80, w_distancia);                // stfs f0,0(r6), r6 = r1+80
    const double resto = double(float(distancia - radio));    // fsubs f13,f0,f1
    if (resto > radio) {                                      // fcmpu cr6,f13,f1; ble
      const double t9 = double(float(h / resto));             // fdivs f9,f11,f13
      tam = double(float(t9 * radio));                        // fmuls f12,f9,f1
    }
    PPCRegister entero;
    entero.s64 = Fctiwz(tam);                                 // fctiwz f8,f12
    Escribir32(base, marco - 16, entero.u32);                 // stfiwx f8,0,r11 con r11 = r1-16
    r3 = entero.u32;                                          // lwz r3,-16(r1)
  }
  if (int32_t(uint32_t(r3)) <= 1) {                           // cmpwi cr6,r3,1; ble
    ctx.r3.u64 = r3;
    return salir(kDescarte);
  }
  if ((banderas & 0x2000000u) != 0) {                         // rlwinm r7,r9,0,6,6; addi r3,r3,10
    r3 = uint64_t(int64_t(r3) + 10);
  }
  const int32_t pixeles = int32_t(uint32_t(r3));
  ctx.r3.u64 = r3;  // what remains in r3 if it is discarded from here

  // --- 5. La malla (r28) ---
  uint32_t modelo;
  if ((banderas_vista & 0x800u) != 0 || (banderas_vista & 0x1000u) != 0) {  // loc_824C2A90
    const uint32_t vista = Leer32(base, cull + 128);
    int32_t umbral = 32;
    if (int32_t(Leer32(base, vista + 4)) == 20 && (banderas_vista & 0x1000u) != 0) {
      umbral = 23;
    }
    if (pixeles < umbral) {
      return salir(kDescarte);
    }
    if ((banderas & 0x80u) != 0) {
      modelo = Leer32(base, info + 48);                                    // loc_824C2AC8
    } else if ((banderas & 0x100u) != 0 || (banderas & 0x1000000u) != 0) {
      modelo = Leer32(base, info + 40);                                    // loc_824C2AF8
    } else {
      modelo = Leer32(base, info + 52);
    }
  } else if ((banderas_vista & 0x20u) != 0) {
    if (pixeles < 32) {
      return salir(kDescarte);
    }
    modelo = Leer32(base, info + 48);
  } else if (int32_t(Leer32(base, kModoVista)) >= 3) {
    if (pixeles < 23) {
      return salir(kDescarte);
    }
    modelo = Leer32(base, info + 48);
  } else {                                                                  // loc_824C2A10
    if (pixeles < 18) {
      return salir(kDescarte);
    }
    const uint32_t buena = Leer32(base, info + 40);                        // r10 = pModel[0]
    modelo = buena;                                                         // loc_824C2A88 if there are no more
    if (buena != 0) {
      const uint32_t solido = Leer32(base, buena + 12);
      if (solido != 0 && int32_t(int16_t(Leer16(base, solido + 20))) >= 40) {
        const uint32_t w_densidad = Leer32(base, solido + 156);
        const uint32_t w_k = Leer32(base, kDistanciaK);
        const uint32_t w_umbral = Leer32(base, kUmbralLod);
        if (EsNaN(w_densidad) | EsNaN(w_k) | EsNaN(w_umbral)) {
          return a_la_original(kPorNaN);
        }
        double densidad = Lfs(w_densidad);                                 // lfs f0,156(r11)
        if (densidad < seis) {                                             // fcmpu cr6,f0,f31; bge
          densidad = seis;                                                 // fmr f0,f31
        }
        const int64_t px64 = int64_t(pixeles);                             // extsw r6,r3
        const bool cerca = Lfs(w_distancia) < Lfs(w_k);                    // lfs f7,80(r1); lfs f13,3552(r27); fcmpu
        Escribir64(base, marco + 80, uint64_t(px64));                      // std r6,80(r1)
        const double f5 = double(px64);                                    // lfd f6,80(r1); fcfid f5,f6
        const double f4 = double(float(f5));                               // frsp f4,f5
        const double cociente = double(float(f4 / densidad));              // fdivs f0,f4,f0
        if (!cerca) {                                                      // blt cr6 -> la buena
          const double umbral_lod = Lfs(w_umbral);                         // lfs f13,-4396(r27)
          if (cociente < umbral_lod || std::isnan(cociente)) {             // blt / bso -> loc_824C2AC8
            modelo = Leer32(base, info + 48);                              // la reducida
          }
        }
      }
    }
  }
  if (modelo == 0) {                                                        // loc_824C2AFC: cmplwi cr6,r28,0; beq
    return salir(kDescarte);
  }

  // --- 6. SceneryDrawInfo sin matriz ---
  if ((banderas & 0x200u) != 0) {
    const uint32_t tope = Leer32(base, cull + 144);
    const uint32_t actual = Leer32(base, cull + 140);
    if (actual >= tope) {                                                   // cmplw; bge: lleno
      return salir(kDescarte);
    }
    const uint64_t modelo_y_estado = uint64_t(modelo) + visibilidad;        // add r3,r28,r26
    Escribir32(base, cull + 140, actual + 12u);                             // stw r4,140(r25)
    Escribir32(base, actual + 0, uint32_t(modelo_y_estado));                // stw r3,0(r11)
    Escribir32(base, actual + 4, 0u);                                       // stw r10,4(r11)
    Escribir32(base, actual + 8, inst);                                     // stw r30,8(r11)
    ctx.r3.u64 = modelo_y_estado;
    return salir(kSinMatriz);
  }

  // --- 7. With a matrix (loc_824C2B50). Before writing anything outside the stack: flares, wind, constants ---
  const uint32_t vista = Leer32(base, cull + 128);
  const bool vista_1_o_2 = vista == kVista1 || vista == kVista2;
  const uint32_t solido = Leer32(base, modelo + 12);
  if (vista_1_o_2 && solido != 0 && Leer32(base, solido + 128) != 0 && Leer8(base, solido + 27) != 0) {
    return a_la_original(kPorDestellos);  // with position markers: the flare loop belongs to the original
  }
  const bool viento = (banderas_vista & 0x4000u) != 0 && solido != 0 && (Leer16(base, solido + 14) & 0x80u) != 0;
  if constexpr (!kGuardia) {
    if (viento && g_comprobadas[kConViento].load(std::memory_order_relaxed) < kMinimoViento) [[unlikely]] {
      return a_la_original(kPorComprobar);
    }
  }
  const bool altura = ((banderas_vista & banderas) & 0x100u) != 0;  // lwz r9,132(r25); lwz r8,24(r30); and; 0x100
  const bool espejo = (banderas_vista & 0x800u) != 0;               // lwz r5,132(r25); 0x800
  const uint32_t w_uno = Leer32(base, kUno);
  const uint32_t w_cero = Leer32(base, kCero);
  const uint32_t w_escala = Leer32(base, kEscalaRotacion);
  const uint32_t w_altura = altura ? Leer32(base, kAlturaExtra) : 0u;
  const uint32_t w_sesenta = viento ? Leer32(base, kSesenta) : 0u;
  if (EsNaN(w_uno) | EsNaN(w_cero) | EsNaN(w_escala) | EsNaN(w_altura) | EsNaN(w_sesenta)) {
    return a_la_original(kPorNaN);
  }

  // eFrameMallocMatrix(1) (820E_A568), inlined: 64 bytes from the frame allocator.
  ctx.lr = kVueltaReserva;
  const uint32_t reserva = Leer32(base, kReservaActual);
  const uint32_t fin = Leer32(base, kReservaFin);
  uint32_t matriz = 0;
  if (uint32_t(reserva + 64u) < fin) {                    // cmplw cr6,r10,r8; blt
    Escribir32(base, kReservaActual, reserva + 64u);
    matriz = reserva;
  } else {                                                // out of space: the flag and the requested bytes
    Escribir32(base, kReservaAgotada, 1u);
    Escribir32(base, kReservaPerdida, Leer32(base, kReservaPerdida) + 64u);
  }
  if (matriz == 0) {                                      // mr r31,r3; cmplwi cr6,r31,0; beq
    ctx.r3.u64 = 0;
    return salir(kConMatriz);
  }

  // The rotation (8243_F8D8), inlined: each 16-bit integer (std + lfd + fcfid + frsp) times the scale (fmuls).
  ctx.lr = kVueltaRotacion;
  const double escala = Lfs(w_escala);
  const auto rotacion = [&](uint32_t desplazamiento) -> uint32_t {
    const double a = double(int64_t(int16_t(Leer16(base, inst + desplazamiento))));  // lhz; extsh; std; lfd; fcfid
    const double b = double(float(a));                                              // frsp
    return Stfs(double(float(b * escala)));                                          // fmuls; stfs
  };
  Escribir32(base, matriz + 12, w_cero);                  // stfs f13,12(r4): without NaN, the word of kCero
  Escribir32(base, matriz + 4, rotacion(46));
  Escribir32(base, matriz + 8, rotacion(48));
  Escribir32(base, matriz + 0, rotacion(44));
  Escribir32(base, matriz + 28, w_cero);
  Escribir32(base, matriz + 24, rotacion(54));
  Escribir32(base, matriz + 20, rotacion(52));
  Escribir32(base, matriz + 16, rotacion(50));
  Escribir32(base, matriz + 44, w_cero);
  Escribir32(base, matriz + 36, rotacion(58));
  Escribir32(base, matriz + 40, rotacion(60));
  Escribir32(base, matriz + 32, rotacion(56));
  // What its std leave in its work area (r1-32, -24 and -16 of the leaf, which has no frame): the last ones.
  Escribir64(base, marco - 32, uint64_t(int64_t(int16_t(Leer16(base, inst + 50)))));
  Escribir64(base, marco - 24, uint64_t(int64_t(int16_t(Leer16(base, inst + 60)))));
  Escribir64(base, marco - 16, uint64_t(int64_t(int16_t(Leer16(base, inst + 56)))));
  double f1 = double(float(double(int64_t(int16_t(Leer16(base, inst + 58))))));  // its last f1: frsp f1,f3

  // The position and the w (lfs + stfs without NaN: the same words).
  Escribir32(base, matriz + 48, w_x);
  Escribir32(base, matriz + 52, w_y);
  Escribir32(base, matriz + 56, w_z);
  Escribir32(base, matriz + 60, w_uno);
  if (altura) {                                           // fmr f2,f13; lfs f0,7304(r27); fadds f1,f2,f0; stfs
    f1 = double(float(Lfs(w_z) + Lfs(w_altura)));
    Escribir32(base, matriz + 56, Stfs(f1));
  }
  if (espejo) {                                           // lfs f0,40(r31); fneg; stfs: without NaN, the sign bit
    Escribir32(base, matriz + 40, Leer32(base, matriz + 40) ^ 0x80000000u);
  }
  ctx.f1.f64 = f1;

  // The SceneryDrawInfo with a matrix.
  const uint32_t tope = Leer32(base, cull + 144);         // lwz r3,144(r25)
  const uint32_t actual = Leer32(base, cull + 140);       // lwz r29,140(r25)
  ctx.r3.u64 = tope;
  if (actual >= tope) {                                   // full: the allocated matrix stays, as in the original
    return salir(kConMatriz);
  }
  Escribir32(base, cull + 140, actual + 12u);             // stw r11,140(r25)
  Escribir32(base, actual + 0, uint32_t(uint64_t(modelo) + visibilidad));  // add r10,r28,r26; stw r10,0(r29)
  if (viento) {
    // lfs f12,48(r31); fmuls by 60; fctiwz; stfiwx to r1+80; lwz; and the remainder of dividing by 360 (mulhw,
    // srawi...).
    const double f11 = double(float(Lfs(w_x) * Lfs(w_sesenta)));
    PPCRegister f10;
    f10.s64 = Fctiwz(f11);
    Escribir32(base, marco + 80, f10.u32);
    const uint64_t r11 = f10.u32;                                                         // lwz r11,80(r1)
    const int64_t r10 = (int64_t(int32_t(uint32_t(r11))) * int64_t(int32_t(kMagia360))) >> 32;  // mulhw r10,r11,r10
    const uint64_t r9 = uint64_t(r10) + r11;                                              // add r9,r10,r11
    const int64_t r10b = int64_t(int32_t(uint32_t(r9)) >> 8);                             // srawi r10,r9,8
    const uint64_t r8 = uint64_t(r10b) + ((uint32_t(r10b) >> 31) & 1u);                   // rlwinm r9,r10,1,31,31; add
    const int64_t r7 = static_cast<int64_t>(r8 * uint64_t(360));                          // mulli r7,r8,360
    ctx.r5.u64 = r11 - uint64_t(r7);                                                      // subf r5,r7,r11
    ctx.r3.u64 = vista;                                   // lwz r3,128(r25)
    ctx.r4.s64 = ctx.r1.s64 + 128;
    ctx.r6.u64 = matriz;
    ctx.lr = kVueltaViento;
    NFSMW_ESCENARIO_VIENTO(ctx, base);                    // CreateWindRotMatrix(vista, r1+128, desfase, matriz)
    ctx.r5.u64 = matriz;
    ctx.r4.s64 = ctx.r1.s64 + 128;
    ctx.r3.u64 = matriz;
    ctx.lr = kVueltaMultiplicar;
    NFSMW_ESCENARIO_MULTIPLICAR(ctx, base);               // bMulMatrix(matriz, matriz, r1+128)
  }
  Escribir32(base, actual + 8, inst);                     // stw r30,8(r29)
  Escribir32(base, actual + 4, matriz);                   // stw r31,4(r29)
  if (vista_1_o_2 && solido != 0) {                       // 8221_9EA8(solido, 0): without markers it returns 0
    ctx.lr = kVueltaMarcador;
    ctx.r3.u64 = 0;
  }
  return salir(viento ? kConViento : kConMatriz);
}

// ---------------------------------------------------------------------------------------------------------------
// Report, guard and call.
// ---------------------------------------------------------------------------------------------------------------
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
    REXLOG_INFO("[escenario] DrawAScenery (sub_824C2850) {}",
                Activo() ? "en nativo (build 184): empieza comprobando contra la original"
                         : "por la original (nfsmw_escenario_nativo = false)");
    return;
  }
  uint64_t nativas[kCaminos];
  uint64_t total = 0;
  for (uint32_t c = 0; c < kCaminos; ++c) {
    nativas[c] = g_nativas[c].exchange(0, std::memory_order_relaxed);
    total += nativas[c];
  }
  uint64_t originales[kMotivos];
  uint64_t total_originales = 0;
  for (uint32_t m = 0; m < kMotivos; ++m) {
    originales[m] = g_originales[m].exchange(0, std::memory_order_relaxed);
    total_originales += originales[m];
  }
  NFSMW_INFORME_DIFERIDO(
      "[escenario] ultimos 10 s: {} en nativo ({} descartes, {} sin matriz, {} con matriz, {} con viento), {} por la "
      "original (apagada {}, nfsmw_escenario_detalle {}, pila {}, NaN {}, destellos {}, viento a comprobar {}); "
      "comprobadas contra la original desde el arranque: {} ({} descartes, {} sin matriz, {} con matriz, {} con "
      "viento; las primeras {}, las primeras {} con viento y despues 1 de cada {}){}",
      total, nativas[kDescarte], nativas[kSinMatriz], nativas[kConMatriz], nativas[kConViento], total_originales,
      originales[kPorApagada], originales[kPorDetalle], originales[kPorPila], originales[kPorNaN],
      originales[kPorDestellos], originales[kPorComprobar], g_comprobadas_total.load(std::memory_order_relaxed),
      g_comprobadas[kDescarte].load(std::memory_order_relaxed), g_comprobadas[kSinMatriz].load(std::memory_order_relaxed),
      g_comprobadas[kConMatriz].load(std::memory_order_relaxed), g_comprobadas[kConViento].load(std::memory_order_relaxed),
      kComprobaciones, kMinimoViento, kPeriodo,
      g_apagado.load(std::memory_order_relaxed) ? " | APAGADA por diferencia" : "");
}

// A memory area that the native version or the original can write: the state before, what the native version
// left, and the comparison.
struct Zona {
  const char* nombre;
  uint32_t direccion;
  uint32_t bytes;
  uint8_t* antes;
  uint8_t* nativa;
};

std::string Hex(const uint8_t* bytes, uint32_t n) {
  static const char kDigitos[] = "0123456789ABCDEF";
  std::string s;
  for (uint32_t i = 0; i < n; ++i) {
    s += kDigitos[bytes[i] >> 4];
    s += kDigitos[bytes[i] & 15];
  }
  return s;
}

// Guard: the native version, undo, the original, and compare. Always leaves the original's state.
[[gnu::noinline]] void Comprobar(PPCContext& ctx, uint8_t* base) {
  const uint32_t pila = ctx.r1.u32;
  const uint32_t cull = ctx.r5.u32;
  if (pila < kPila || EnPila(cull, 200, pila)) {
    Sumar(g_originales[kPorPila], uint64_t(1));
    __imp__sub_824C2850(ctx, base);
    return;
  }
  // Snapshots from before. The stack in a per-thread buffer (1.5 KB x 2); the rest is small.
  static thread_local uint8_t pila_antes[kPila];
  static thread_local uint8_t pila_nativa[kPila];
  uint8_t chicas_antes[4 + 12 + 4 + 8 + 64];
  uint8_t chicas_nativa[4 + 12 + 4 + 8 + 64];
  Zona zonas[6];
  uint32_t nz = 0;
  uint32_t usado = 0;
  const auto agregar = [&](const char* nombre, uint32_t direccion, uint32_t bytes) {
    zonas[nz++] = {nombre, direccion, bytes, chicas_antes + usado, chicas_nativa + usado};
    usado += bytes;
  };
  zonas[nz++] = {"pila", pila - kPila, kPila, pila_antes, pila_nativa};
  agregar("cull+140", cull + 140, 4);
  const uint32_t actual = Leer32(base, cull + 140);
  if (actual < Leer32(base, cull + 144)) {
    agregar("SceneryDrawInfo", actual, 12);
  }
  agregar("reserva", kReservaActual, 4);
  agregar("reserva agotada", kReservaAgotada, 8);
  // The block that would be allocated. With the pointer at 0 the original does not write to it (it exits
  // with r3 = 0): it is not checked.
  const uint32_t reserva = Leer32(base, kReservaActual);
  if (reserva != 0 && uint32_t(reserva + 64u) < Leer32(base, kReservaFin)) {
    agregar("matriz", reserva, 64);
  }
  for (uint32_t i = 0; i < nz; ++i) {
    std::memcpy(zonas[i].antes, Puntero(base, zonas[i].direccion), zonas[i].bytes);
  }
  const auto deshacer = [&]() {
    for (uint32_t i = nz; i-- > 0;) {
      std::memcpy(Puntero(base, zonas[i].direccion), zonas[i].antes, zonas[i].bytes);
    }
  };
  const PPCContext entrada = ctx;
  const auto volver_a_la_entrada = [&]() {
    ctx = entrada;
    ctx.fpscr.setcsr(ctx.fpscr.csr);  // the real FPCR, like the copy's
  };

  const Salida s = Nativa<true>(ctx, base);
  if (!s.hecha) {
    deshacer();
    volver_a_la_entrada();
    Sumar(g_originales[s.motivo], uint64_t(1));
    __imp__sub_824C2850(ctx, base);
    return;
  }
  for (uint32_t i = 0; i < nz; ++i) {
    std::memcpy(zonas[i].nativa, Puntero(base, zonas[i].direccion), zonas[i].bytes);
  }
  const uint64_t r3n = ctx.r3.u64, f1n = ctx.f1.u64, r1n = ctx.r1.u64, r12n = ctx.r12.u64, lrn = ctx.lr;
  const uint32_t csrn = ctx.fpscr.csr;
  deshacer();
  volver_a_la_entrada();
  __imp__sub_824C2850(ctx, base);

  const char* que = nullptr;
  const Zona* mala = nullptr;
  uint32_t byte = 0;
  for (uint32_t i = 0; i < nz && !mala; ++i) {
    const uint8_t* ahora = Puntero(base, zonas[i].direccion);
    for (uint32_t k = 0; k < zonas[i].bytes; ++k) {
      if (ahora[k] != zonas[i].nativa[k]) {
        mala = &zonas[i];
        byte = k;
        break;
      }
    }
  }
  if (mala) {
    que = "memoria";
  } else if (ctx.r3.u64 != r3n) {
    que = "r3";
  } else if (ctx.f1.u64 != f1n) {
    que = "f1";
  } else if (ctx.r1.u64 != r1n) {
    que = "r1";
  } else if (ctx.r12.u64 != r12n) {
    que = "r12";
  } else if (ctx.lr != lrn) {
    que = "lr";
  } else if (ctx.fpscr.csr != csrn) {
    que = "FPCR";
  }
  if (!que) {
    Sumar(g_comprobadas[s.camino], uint64_t(1));
    const uint64_t total = g_comprobadas_total.load(std::memory_order_relaxed) + 1;
    g_comprobadas_total.store(total, std::memory_order_relaxed);
    if (total == kComprobaciones) {
      REXLOG_INFO("[escenario] {} llamadas comprobadas contra la original (r3, f1, r1, r12, lr, FPCR, la pila, "
                  "[cull+140], el SceneryDrawInfo, la reserva y la matriz), 0 diferencias: DrawAScenery en nativo, y "
                  "sigue comprobando 1 de cada {}",
                  total, kPeriodo);
    }
    if (s.camino == kConViento && g_comprobadas[kConViento].load(std::memory_order_relaxed) == kMinimoViento) {
      REXLOG_INFO("[escenario] viento: {} llamadas comprobadas contra la original, 0 diferencias: tambien en nativo",
                  kMinimoViento);
    }
    return;
  }
  g_apagado.store(true, std::memory_order_relaxed);  // the state is already the original's
  const uint32_t n = mala ? (mala->bytes - byte < 8u ? mala->bytes - byte : 8u) : 0u;
  REXLOG_ERROR("[escenario] DIFERENCIA con la original ({}{}{}; camino {}): nativa {} original {}; r3 nativa 0x{:X} "
               "original 0x{:X}, f1 0x{:016X} / 0x{:016X}, r12 0x{:X} / 0x{:X}, lr 0x{:X} / 0x{:X}, FPCR 0x{:X} / "
               "0x{:X}; entrada: this 0x{:08X}, instancia {}, cull 0x{:08X}, estado {}, pila 0x{:08X}. Camino nativo "
               "APAGADO para el resto de la sesion: se queda la original",
               que, mala ? " en " : "", mala ? fmt::format("{} +{}", mala->nombre, byte) : std::string(),
               kNombresCamino[s.camino], mala ? Hex(mala->nativa + byte, n) : std::string("-"),
               mala ? Hex(Puntero(base, mala->direccion) + byte, n) : std::string("-"), r3n, ctx.r3.u64, f1n,
               ctx.f1.u64, r12n, ctx.r12.u64, lrn, ctx.lr, csrn, ctx.fpscr.csr, entrada.r3.u32, entrada.r4.u32,
               entrada.r5.u32, entrada.r6.u64, entrada.r1.u32);
}

}  // namespace

// The hook in nfsmw_d3d_registros_nativo.cpp calls it inside its measurement ("[medida] DrawAScenery").
void DrawAScenery(PPCContext& ctx, uint8_t* base) {
  const uint64_t n = g_llamadas.load(std::memory_order_relaxed) + 1;
  g_llamadas.store(n, std::memory_order_relaxed);
  if ((n & (kPeriodo - 1)) == 0) [[unlikely]] {
    Informe();
  }
  if (!Activo() || g_apagado.load(std::memory_order_relaxed)) [[unlikely]] {
    Sumar(g_originales[kPorApagada], uint64_t(1));
    __imp__sub_824C2850(ctx, base);
    return;
  }
  if (REXCVAR_GET(nfsmw_escenario_detalle) != 0) [[unlikely]] {
    Sumar(g_originales[kPorDetalle], uint64_t(1));
    __imp__sub_824C2850(ctx, base);
    return;
  }
  if (n <= kComprobaciones || (n & (kPeriodo - 1)) == 0) [[unlikely]] {
    Comprobar(ctx, base);
    return;
  }
  const Salida s = Nativa<false>(ctx, base);
  if (s.hecha) [[likely]] {
    Sumar(g_nativas[s.camino], uint64_t(1));
    return;
  }
  if (s.motivo == kPorComprobar) {
    Comprobar(ctx, base);
    return;
  }
  Sumar(g_originales[s.motivo], uint64_t(1));
  __imp__sub_824C2850(ctx, base);
}

}  // namespace nfsmw::escenario_nativo
