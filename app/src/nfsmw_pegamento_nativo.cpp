// nfsmw - the draw glue in native code: sub_82452730 (with 8245_2690 and 8244_ED58 inside, as LTO used to
// inline them) and the draw-list loop that calls it (sub_82454B50).
//
// What it is (PowerPC read instruction by instruction: nfsmw_recomp.80.cpp, .36, .16 and .41)
//   The Main XThread walks each sorted draw list with sub_82454B50 (r3 = the view, r4 = the list: the count at
//   [lista+0] and 8-byte entries from lista+8 with the index at +4). For each entry it calls sub_82452730 with
//   r3 = the object (52 bytes at 829A_FA64 + 52 x index), r4 = the view, r5/r6 = two words of the loop's frame
//   (the last effect state). sub_82452730 (frame of 112):
//   1. E = [obj+12]; if [E+24] and the flags [obj+8] (0x400/0x800) or [r5]/[r6] change -> 8244_EA48 (effect
//      state change, which ends in the native pass start). Stores both in [r6]/[r5].
//   2. Depending on [vista+5]: 8245_3E20 or 8245_3D60 (material, matrices...), with 9 arguments (the 9th at
//      r1+84).
//   3. 8245_2690 (frame of 128): the vertex streams of A = [obj+0] (SetStreamSource, up to 6, 32-byte entries
//      from A+68; flag 0x8000 of [A+94+32i] marks the last one) and the index buffer M+32 with M = [[obj+4]]
//      (SetIndices, only if the global index buffer 82A2_D15C changes). Returns ([A+24] - [M+24]) / 2.
//   4. 8244_ED58 (frame of 112): depending on [E+6], effect parameters (sub_826992F0) and DrawIndexedVertices
//      (sub_82593C50) with the second draw 8244_EDF8 if 0x10, or the virtual call [[E]+20] if 0x20.
//
// Why native
//   In a stack sampling run (race), sub_82452730 plus what LTO inlined into it took 6.73 points of a core in
//   self time (1.8 ms per frame at 36.9 FPS): 5.6 belong to sub_82452730, 8245_2690 and 8244_ED58, and 4.3 are
//   cache-missing loads on the first touch of the object, A, M and E. The Xbox 360 game already prefetched
//   them with dcbt: two in the loop (the next object and the list 128 bytes ahead) and three here (A, [obj+24]
//   and [obj+4]), but the recompiler leaves dcbt as a comment (238 in the whole game). Here they come back as
//   PRFM (cache hints: they neither load nor fault), for the Xbox's 128-byte block (two 64-byte A57 lines) or
//   for the lines about to be read, plus two more hints: E on entry and M as soon as it is known. It also
//   works in registers: no context shuffling and no reloads of r1 after every guest store.
//
// Why it is identical
//   - No floating point: integers, copies and one mulhw. It does not touch the FPCR.
//   - Memory: the same stores, with the same bytes and in the same order relative to loads and calls: the lr
//     and the two zeros of r30/r31 that the generated code puts on the stack, the three stwu, r1+84, [r6] and
//     [r5], the global index buffer, and in the loop the two zeros of its frame, the global and [lista+0].
//     Everything the original reads again after a call ([obj+N], [[obj+4]], [A+94+32i], [E+6], the list, the
//     saved lr...) is reread here at the same point. There are no extra loads: the hints come from loads the
//     original makes at that same point.
//   - Registers: before each call and on exit, r3-r12, lr and r1 are exactly as in the original, with all 64
//     bits (the loop's r3 carries the high part of 829A_FA64 as a negative number; r1 is written through its
//     low half like the generated code), and after each call only what the original writes is written.
//     Non-volatile r13-r31, cr, xer and ctr are local variables of the generated code; r0, r2, r13, f and v
//     are not touched.
//   - The virtual call uses the same macro as the generated code (nfsmw_pch.h): same dispatch and same last
//     indirect target.
//   Note: the native version contains 8245_2690 and 8244_ED58 (it does not call them). If either of them is
//   ever hooked, that hook would not be used on this path: the native version must be redone or turned off
//   (the patch refuses to apply if they are already hooked). And if a codegen changes any of the four
//   functions, run montar.py and review.
//
// Self-checking guard (cvar nfsmw_pegamento_nativo; the project's standard guard)
//   Both call functions whose effects cannot be repeated (draws, SetIndices, effects...), so the guard compares
//   the native version with a literal copy of the original (the current generated code as is, without
//   comments, with its calls going through a recorder; copia_literal.py, and the patch checks that it equals
//   the generated code). Both run dry: each call records r0-r13, lr, f0-f13, v0-v13, FPCR, the last indirect
//   target and a sum of the memory written, and clobbers the volatile registers as a real function would. The
//   regions they write are snapshotted first, each dry run is undone, the calls, the exit state and the memory
//   are compared byte by byte, and then the original runs for real (its state is always the one kept). Glue:
//   the first kComprobaciones calls, the first kMinimoRaro of each rare path (view 8245_3E20, 2 or more
//   streams, flags, virtual, second draw) and then 1 in every kPeriodo. Loop: the first kComprobacionesBucle
//   and then 1 in every 4096 (lists with more than kMaxBucle draws are not compared; the loop picks them at
//   random, not with a mask, because it is called almost the same number of times every frame). A mismatch
//   writes "[pegamento] DIFERENCIA" (REXLOG_ERROR) and turns that native version off for the run.
//   Measured in the same run: the glue times 1 in 16 native calls and 1 in 64 through the original, and the
//   loop 1 in 8 and 1 in 32 at random, per draw; the two "[pegamento]" lines every 10 s give the savings in
//   ms/s (the loop one is approximate: each draw includes everything it calls, about 8 us).

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_informe_diferido.h"
#include <rex/platform.h>
// The generated code's macros (REX_RAW_ADDR, REX_LOAD_*, REX_STORE_*, REX_CALL_INDIRECT_FUNC...): the native
// version dispatches the virtual call exactly like the original, and the guard's literal copy compiles as is.
#include "generated/default/nfsmw_pch.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

REXCVAR_DEFINE_BOOL(nfsmw_pegamento_nativo, true, "NFSMW",
                    "El pegamento de dibujo (sub_82452730 con los flujos, los indices y el dibujo de cada objeto) y el "
                    "bucle de la lista que lo llama (sub_82454B50) en nativo (build 186), identicos y con las pistas de "
                    "cache (dcbt) del juego de Xbox 360. Se comprueban en seco contra una copia literal de la original y "
                    "se apagan solos si difieren; false = las originales")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82452730);  // the original glue
REX_EXTERN(__imp__sub_82454B50);  // the original loop
REX_EXTERN(sub_82452730);         // this file's hook: the loop calls through it, like the original after the patch
// The hooks the original calls, by name (nfsmw_d3d_trace.cpp and nfsmw_d3d_registros_nativo.cpp).
REX_EXTERN(sub_8258D968);  // SetStreamSource
REX_EXTERN(sub_8258DA60);  // SetIndices
REX_EXTERN(sub_826992F0);  // parametros de efecto
REX_EXTERN(sub_82593C50);  // DrawIndexedVertices
// The rest, as the generated code calls them (8244_EA48 through its sub_ alias, the others directly). The
// name is built in pieces on purpose: tools/llamadas_directas.py treats any address that appears whole in
// app/src as hooked, and their calls from the rest of the game must stay direct (__imp__, inlinable).
#define NFSMW_PEGAMENTO_UNIR_(a, b) a##b
#define NFSMW_PEGAMENTO_CAMBIO_ESTADO NFSMW_PEGAMENTO_UNIR_(sub_8244, EA48)
#define NFSMW_PEGAMENTO_VISTA_E20 NFSMW_PEGAMENTO_UNIR_(__imp__sub_8245, 3E20)
#define NFSMW_PEGAMENTO_VISTA_D60 NFSMW_PEGAMENTO_UNIR_(__imp__sub_8245, 3D60)
#define NFSMW_PEGAMENTO_SEGUNDO_DIBUJO NFSMW_PEGAMENTO_UNIR_(__imp__sub_8244, EDF8)
REX_EXTERN(NFSMW_PEGAMENTO_CAMBIO_ESTADO);
REX_EXTERN(NFSMW_PEGAMENTO_VISTA_E20);
REX_EXTERN(NFSMW_PEGAMENTO_VISTA_D60);
REX_EXTERN(NFSMW_PEGAMENTO_SEGUNDO_DIBUJO);

namespace nfsmw::pegamento_nativo {
namespace {

// ---------------------------------------------------------------------------------------------------------------
// Guest memory: REX_RAW_ADDR from nfsmw_pch.h, without volatile (C++ itself gives the ordering: accesses are
// not reordered across an opaque call or a store that may alias).
// ---------------------------------------------------------------------------------------------------------------
[[gnu::always_inline]] inline uint32_t L8(uint8_t* base, uint32_t d) {
  return *REX_RAW_ADDR(d);
}
[[gnu::always_inline]] inline uint32_t L16(uint8_t* base, uint32_t d) {
  uint16_t v;
  std::memcpy(&v, REX_RAW_ADDR(d), 2);
  return __builtin_bswap16(v);
}
[[gnu::always_inline]] inline uint32_t L32(uint8_t* base, uint32_t d) {
  uint32_t v;
  std::memcpy(&v, REX_RAW_ADDR(d), 4);
  return __builtin_bswap32(v);
}
[[gnu::always_inline]] inline uint64_t L64(uint8_t* base, uint32_t d) {
  uint64_t v;
  std::memcpy(&v, REX_RAW_ADDR(d), 8);
  return __builtin_bswap64(v);
}
[[gnu::always_inline]] inline void E32(uint8_t* base, uint32_t d, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(REX_RAW_ADDR(d), &v, 4);
}
[[gnu::always_inline]] inline void E64(uint8_t* base, uint32_t d, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(REX_RAW_ADDR(d), &v, 8);
}
// Cache hint (PRFM PLDL1KEEP on the Switch) for the 64-byte line of d: it neither loads nor faults, even if
// d is invalid.
[[gnu::always_inline]] inline void Anticipar(uint8_t* base, uint32_t d) {
  __builtin_prefetch(REX_RAW_ADDR(d));
}
// The Xbox 360 dcbt: the 128-byte block that contains d (two A57 lines).
[[gnu::always_inline]] inline void AnticiparBloque(uint8_t* base, uint32_t d) {
  const uint32_t b = d & ~127u;
  __builtin_prefetch(REX_RAW_ADDR(b));
  __builtin_prefetch(REX_RAW_ADDR(b + 64u));
}
// A zero the compiler cannot remove and the CPU only has once v has arrived (an instruction with a real data
// dependency): the hints it is added to issue after that load and do not compete with its cache miss.
[[gnu::always_inline]] inline uint32_t CeroTrasLeer(uint32_t v) {
#if defined(__aarch64__)
  uint32_t c;
  asm("and %w0, %w1, wzr" : "=r"(c) : "r"(v));
  return c;
#elif defined(__x86_64__)
  uint32_t c = v;
  asm("andl $0, %0" : "+r"(c));  // (tested on the PC; "and" is not a zeroing idiom: it waits for the data)
  return c;
#else
  return v & 0u;
#endif
}
// A list object (52 bytes; it may cross a line).
[[gnu::always_inline]] inline void AnticiparObjeto(uint8_t* base, uint32_t d) {
  __builtin_prefetch(REX_RAW_ADDR(d));
  __builtin_prefetch(REX_RAW_ADDR(d + 51u));
}

// ---------------------------------------------------------------------------------------------------------------
// Fixed addresses (as the PowerPC builds them: lis/addi) and the return address of each call (ctx.lr in the
// generated code).
// ---------------------------------------------------------------------------------------------------------------
constexpr uint64_t kLis32093 = uint64_t(int64_t(-32093) * 65536);       // lis rX,-32093 (sign-extended to 64 bits)
constexpr uint32_t kDispositivo = uint32_t(kLis32093) - 12448u;          // lwz r3,-12448(rX): the D3D device
constexpr uint32_t kIndicesActuales = uint32_t(kLis32093) - 11940u;      // lwz/stw rY,-11940(rX): the current index buffer
constexpr uint64_t kObjetosMenos4 = uint64_t(int64_t(-32101) * 65536 - 1440);  // r29 of the loop: lis -32101; addi -1440

constexpr uint32_t kMarco = 112;        // sub_82452730: stwu r1,-112(r1)
constexpr uint32_t kMarcoFlujos = 128;  // 8245_2690
constexpr uint32_t kMarcoDibujo = 112;  // 8244_ED58
constexpr uint32_t kMarcoBucle = 144;   // sub_82454B50
constexpr uint32_t kPilaVigilada = kMarco + kMarcoFlujos;  // [r1-240, r1): what the three write on the stack

// sub_82452730
constexpr uint64_t kVueltaEstado = 0x824527C8;
constexpr uint64_t kVueltaE20 = 0x82452808;
constexpr uint64_t kVueltaD60 = 0x82452814;
constexpr uint64_t kVueltaFlujos = 0x82452824;  // the call to 8245_2690 (its mflr r12)
constexpr uint64_t kVueltaDibujo = 0x8245284C;  // the call to 8244_ED58 (its mflr r12)
// 8245_2690
constexpr uint64_t kVueltaFlujo = 0x824526D0;
constexpr uint64_t kVueltaIndices = 0x82452714;
// 8244_ED58
constexpr uint64_t kVueltaEfecto = 0x8244ED88;
constexpr uint64_t kVueltaDibujar = 0x8244EDA8;
constexpr uint64_t kVueltaSegundo = 0x8244EDC8;
constexpr uint64_t kVueltaVirtual = 0x8244EDEC;
// sub_82454B50
constexpr uint64_t kVueltaBuclePrologo = 0x82454B58;  // bl __savegprlr_26 (the generated code does not run it)
constexpr uint64_t kVueltaPegamento = 0x82454BE4;

enum Que : uint32_t {
  kCambioEstado = 0,  // 8244_EA48
  kVistaE20 = 1,      // 8245_3E20
  kVistaD60 = 2,      // 8245_3D60
  kFlujo = 3,         // SetStreamSource
  kIndices = 4,       // SetIndices
  kEfecto = 5,        // parametros de efecto
  kDibujar = 6,       // DrawIndexedVertices
  kSegundo = 7,       // 8244_EDF8
  kIndirecta = 8,     // [[E]+20]
  kPegamento = 9,     // the loop: sub_82452730
};

enum Camino : uint32_t {
  kCaminoEstado = 1u << 0,     // effect state change (8244_EA48)
  kCaminoBanderas = 1u << 1,   // [E+24] != 0: the [obj+8] flags are checked
  kCaminoVistaE20 = 1u << 2,   // [vista+5] != 0: 8245_3E20 instead of 8245_3D60
  kCaminoFlujos = 1u << 3,     // two or more vertex streams
  kCaminoIndices = 1u << 4,    // changes the index buffer: SetIndices
  kCaminoVirtual = 1u << 5,    // [E+6] & 0x20: the virtual call instead of the effect and the draw
  kCaminoSegundo = 1u << 6,    // [E+6] & 0x10: the second draw
  kCaminos = 7,
  kCombinaciones = 1u << kCaminos
};
constexpr const char* kNombresCamino[kCaminos] = {"cambio de estado", "banderas", "vista E20", "2+ flujos",
                                                  "SetIndices",       "virtual",  "segundo dibujo"};
constexpr uint32_t kCaminosRaros = kCaminoBanderas | kCaminoVistaE20 | kCaminoFlujos | kCaminoVirtual | kCaminoSegundo;

// ---------------------------------------------------------------------------------------------------------------
// The real calls: the same functions the original calls, through the same path.
// ---------------------------------------------------------------------------------------------------------------
struct Reales {
  [[gnu::always_inline]] static inline void Llamar(PPCContext& ctx, uint8_t* base, uint32_t que) {
    switch (que) {
      case kCambioEstado: NFSMW_PEGAMENTO_CAMBIO_ESTADO(ctx, base); break;
      case kVistaE20: NFSMW_PEGAMENTO_VISTA_E20(ctx, base); break;
      case kVistaD60: NFSMW_PEGAMENTO_VISTA_D60(ctx, base); break;
      case kFlujo: sub_8258D968(ctx, base); break;
      case kIndices: sub_8258DA60(ctx, base); break;
      case kEfecto: sub_826992F0(ctx, base); break;
      case kDibujar: sub_82593C50(ctx, base); break;
      default: NFSMW_PEGAMENTO_SEGUNDO_DIBUJO(ctx, base); break;
    }
  }
  [[gnu::always_inline]] static inline void Indirecta(PPCContext& ctx, uint8_t* base, uint32_t destino) {
    REX_CALL_INDIRECT_FUNC(destino);
  }
  [[gnu::always_inline]] static inline void Pegamento(PPCContext& ctx, uint8_t* base) {
    sub_82452730(ctx, base);
  }
};

// ---------------------------------------------------------------------------------------------------------------
// The whole sub_82452730, with 8245_2690 and 8244_ED58 inlined. L = Reales (real calls) or Grabador (the
// guard, dry run). Returns the path (kCamino*).
// ---------------------------------------------------------------------------------------------------------------
template <class L>
uint32_t Nativa(PPCContext& ctx, uint8_t* base) {
  uint32_t camino = 0;
  // --- Prologo: mflr r12; stw r12,-8(r1); std r30,-24(r1); std r31,-16(r1); stwu r1,-112(r1) ---
  const uint64_t lr_entrada = ctx.lr;
  ctx.r12.u64 = lr_entrada;
  uint64_t r1 = ctx.r1.u64;
  E32(base, uint32_t(r1) - 8u, uint32_t(lr_entrada));
  E64(base, uint32_t(r1) - 24u, 0);  // r30 and r31 of the generated code are zeroed local variables
  E64(base, uint32_t(r1) - 16u, 0);
  {
    const uint32_t ea = uint32_t(r1) - kMarco;
    E32(base, ea, uint32_t(r1));
    r1 = (r1 & 0xFFFFFFFF00000000ull) | ea;  // ctx.r1.u32 = ea: the high half is kept
  }
  const uint64_t r31 = ctx.r3.u64;  // mr r31,r3: the object
  const uint64_t r30 = ctx.r4.u64;  // mr r30,r4: la vista
  const uint32_t obj = uint32_t(r31);
  const uint64_t a0 = L32(base, obj + 0);   // lwz r11,0(r31): A
  const uint64_t x0 = L32(base, obj + 24);  // lwz r10,24(r31)
  const uint64_t e0 = L32(base, obj + 12);  // lwz r3,12(r31): E
  const uint64_t r9a = L32(base, uint32_t(e0) + 24);  // lwz r9,24(r3)
  {
    // The hints issue when [E+24] arrives, since it is needed right away: if they issued earlier (or at the
    // same time), their misses would compete with its miss (the PC benchmark measured it: 40-70 extra cycles
    // on entry). What they request is not read until after 8245_3E20/3D60, so waiting for [E+24] costs them
    // nothing. CeroTrasLeer: a zero with a real dependency on the data.
    const uint32_t cero = CeroTrasLeer(uint32_t(r9a));
    const uint32_t da = uint32_t(a0) + cero, dx = uint32_t(x0) + cero, de = uint32_t(e0) + cero;
    // The game's dcbt r0,r11. Here, the lines from A+0 to A+127: A+24, A+36, A+40 and the first two streams
    // (A+68..A+127).
    Anticipar(base, da);
    Anticipar(base, da + 64u);
    Anticipar(base, da + 127u);
    Anticipar(base, dx);        // the game's dcbt r0,r10: matrix A of the matrices (64 bytes)
    Anticipar(base, dx + 63u);
    Anticipar(base, de);        // (new hint) E+0 and E+6, read by 8244_ED58 and by what 8245_3D60 calls
  }
  uint64_t r7 = ctx.r7.u64;
  uint64_t r8 = ctx.r8.u64;
  uint64_t r11;
  if (uint32_t(r9a) == 0) {  // cmplwi cr6,r9,0; beq loc_82452790
    r11 = 0;                 // li r11,0
  } else {
    camino |= kCaminoBanderas;
    const uint64_t banderas = L32(base, obj + 8);  // lwz r11,8(r31)
    r8 = uint32_t(banderas) & 0x400u;              // rlwinm r8,r11,0,21,21
    if (r8 != 0) {                                 // bne loc_82452788
      r11 = 1;
    } else {
      r7 = uint32_t(banderas) & 0x800u;  // rlwinm r7,r11,0,20,20
      r11 = r7 != 0 ? 1 : 0;             // beq loc_82452790 / li r11,1
    }
  }
  // --- loc_82452794: the loop's last effect state ---
  const uint32_t p5 = ctx.r5.u32;
  const uint32_t p6 = ctx.r6.u32;
  const uint64_t r4a = L32(base, p5);   // lwz r4,0(r5)
  uint64_t r10 = L32(base, p6);         // lwz r10,0(r6)
  uint64_t r9 = r4a - e0;               // subf r9,r3,r4
  E32(base, p6, uint32_t(r11));         // stw r11,0(r6)
  r10 = r10 - r11;                      // subf r10,r11,r10
  E32(base, p5, uint32_t(e0));          // stw r3,0(r5)
  r9 = r10 | r9;                        // or r9,r10,r9
  if (int32_t(uint32_t(r9)) != 0) {     // cmpwi cr6,r9,0; beq loc_824527C8
    camino |= kCaminoEstado;
    ctx.r3.u64 = e0;
    ctx.r4.u64 = 0;  // li r4,0
    ctx.r5.u64 = 0;  // li r5,0
    ctx.r6.u64 = r11;  // mr r6,r11
    ctx.r7.u64 = r7;
    ctx.r8.u64 = r8;
    ctx.r9.u64 = r9;
    ctx.r10.u64 = r10;
    ctx.r11.u64 = r11;
    ctx.r1.u64 = r1;
    ctx.lr = kVueltaEstado;
    L::Llamar(ctx, base, kCambioEstado);  // bl 8244_EA48
    r1 = ctx.r1.u64;
  }
  // --- loc_824527C8: the nine arguments of 8245_3E20 / 8245_3D60 ---
  const uint64_t p = L32(base, obj + 4);          // lwz r8,4(r31): P
  Anticipar(base, uint32_t(p));                   // the game's dcbt r0,r8 (only [P+0] is read)
  const uint64_t vista5 = L8(base, uint32_t(r30) + 5);  // lbz r7,5(r30)
  const uint64_t r11b = L32(base, obj + 28);      // lwz r11,28(r31)
  ctx.r8.u64 = r30;                               // mr r8,r30
  ctx.r5.u64 = r31 + 32;                          // addi r5,r31,32
  ctx.r11.u64 = r11b;
  ctx.r10.u64 = L32(base, obj + 20);              // lwz r10,20(r31)
  ctx.r9.u64 = L32(base, obj + 16);               // lwz r9,16(r31)
  ctx.r7.u64 = L32(base, obj + 24);               // lwz r7,24(r31)
  ctx.r6.u64 = L32(base, obj + 8);                // lwz r6,8(r31)
  ctx.r4.u64 = L32(base, obj + 0);                // lwz r4,0(r31)
  ctx.r3.u64 = L32(base, obj + 12);               // lwz r3,12(r31)
  E32(base, uint32_t(r1) + 84u, uint32_t(r11b));  // stw r11,84(r1)
  ctx.r1.u64 = r1;
  if (uint32_t(vista5) != 0) {  // cmplwi cr6,r7,0; beq loc_8245280C
    camino |= kCaminoVistaE20;
    ctx.lr = kVueltaE20;
    L::Llamar(ctx, base, kVistaE20);  // bl 8245_3E20
  } else {
    ctx.lr = kVueltaD60;
    L::Llamar(ctx, base, kVistaD60);  // bl 8245_3D60
  }
  r1 = ctx.r1.u64;

  // --- loc_82452814 ---
  const uint64_t p2 = L32(base, obj + 4);          // lwz r10,4(r31)
  const uint64_t a = L32(base, obj + 0);           // lwz r3,0(r31)
  const uint64_t m = L32(base, uint32_t(p2));      // lwz r4,0(r10): M
  // (new hints) [M+24] and the IB that starts at M+32, read by SetIndices and by the return of 8245_2690.
  Anticipar(base, uint32_t(m) + 24u);
  Anticipar(base, uint32_t(m) + 35u);
  ctx.r10.u64 = p2;
  // ===== 8245_2690 (bl with lr = kVueltaFlujos): mflr r12; bl __savegprlr_27 (lr only); stwu r1,-128(r1) =====
  ctx.r12.u64 = kVueltaFlujos;
  uint64_t r1f;
  {
    const uint32_t ea = uint32_t(r1) - kMarcoFlujos;
    E32(base, ea, uint32_t(r1));
    r1f = (r1 & 0xFFFFFFFF00000000ull) | ea;
  }
  const uint64_t r27 = a;  // mr r27,r3
  const uint64_t r28 = m;  // mr r28,r4
  uint64_t r30f = 0;       // li r30,0
  uint64_t r31f = r27 + 94;  // addi r31,r27,94
  for (;;) {  // loc_824526B0: one vertex stream
    const uint64_t r11f = r31f - 2;                          // addi r11,r31,-2
    const uint64_t r10f = L16(base, uint32_t(r31f));        // lhz r10,0(r31)
    ctx.r6.u64 = 0;                                          // li r6,0
    ctx.r3.u64 = L32(base, kDispositivo);                    // lwz r3,-12448(r29)
    ctx.r7.u64 = uint32_t(r10f) & 0x7FFFu;                   // clrlwi r7,r10,17
    ctx.r5.u64 = r11f - 24;                                  // addi r5,r11,-24
    ctx.r4.u64 = L8(base, uint32_t(r11f));                   // lbz r4,0(r11)
    ctx.r10.u64 = r10f;
    ctx.r11.u64 = r11f;
    ctx.r1.u64 = r1f;
    ctx.lr = kVueltaFlujo;
    L::Llamar(ctx, base, kFlujo);  // SetStreamSource, through its hook
    r1f = ctx.r1.u64;
    const uint64_t r9f = L16(base, uint32_t(r31f));  // lhz r9,0(r31)
    const uint64_t r8f = (uint64_t(uint32_t(r9f)) | (r9f << 32)) & 0xFFFF8000u;  // rlwinm r8,r9,0,0,16
    ctx.r9.u64 = r9f;
    ctx.r8.u64 = r8f;
    if (uint32_t(r8f) != 0) {  // cmplwi cr6,r8,0; bne loc_824526F0: it was the last one
      break;
    }
    camino |= kCaminoFlujos;
    r30f += 1;  // addi r30,r30,1
    r31f += 32;  // addi r31,r31,32
    if (!(int32_t(uint32_t(r30f)) < 6)) {  // cmpwi cr6,r30,6; blt loc_824526B0
      break;
    }
  }
  // --- loc_824526F0: the index buffer, if it changes ---
  const uint64_t r11g = kLis32093;                         // lis r11,-32093
  const uint64_t r4g = r28 + 32;                           // addi r4,r28,32
  uint64_t r10g = L32(base, kIndicesActuales);             // lwz r10,-11940(r11)
  E32(base, kIndicesActuales, uint32_t(r4g));              // stw r4,-11940(r11)
  r10g = r10g - r4g;                                       // subf r10,r4,r10
  ctx.r11.u64 = r11g;
  ctx.r4.u64 = r4g;
  ctx.r10.u64 = r10g;
  if (int32_t(uint32_t(r10g)) != 0) {  // cmpwi cr6,r10,0; beq loc_82452714
    camino |= kCaminoIndices;
    ctx.r3.u64 = L32(base, kDispositivo);  // lwz r3,-12448(r29)
    ctx.r1.u64 = r1f;
    ctx.lr = kVueltaIndices;
    L::Llamar(ctx, base, kIndices);  // SetIndices, through its hook
    r1f = ctx.r1.u64;
  }
  // --- loc_82452714: la vuelta ---
  {
    const uint64_t r7f = L32(base, uint32_t(r27) + 24);  // lwz r7,24(r27)
    const uint64_t r6f = L32(base, uint32_t(r28) + 24);  // lwz r6,24(r28)
    const uint64_t r5f = r7f - r6f;                      // subf r5,r6,r7
    ctx.r7.u64 = r7f;
    ctx.r6.u64 = r6f;
    ctx.r5.u64 = r5f;
    ctx.r3.s64 = int32_t(uint32_t(r5f)) >> 1;            // srawi r3,r5,1
  }
  r1 = r1f + kMarcoFlujos;  // addi r1,r1,128 (and b __restgprlr_27: returns without touching lr)

  // ===== Back in sub_82452730 =====
  {
    const uint64_t r9b = L32(base, obj + 0);         // lwz r9,0(r31)
    const uint64_t r8b = 1431633920;                 // lis r8,21845
    const uint64_t r4b = ctx.r3.u64;                 // mr r4,r3
    const uint64_t r3b = L32(base, obj + 12);        // lwz r3,12(r31)
    const uint64_t r7b = r8b | 21846;                // ori r7,r8,21846
    uint64_t r11c = L16(base, uint32_t(r9b) + 40);   // lhz r11,40(r9)
    r11c = uint64_t((int64_t(int32_t(uint32_t(r11c))) * int64_t(int32_t(uint32_t(r7b)))) >> 32);  // mulhw r11,r11,r7
    const uint64_t r10b = __builtin_rotateleft64(uint64_t(uint32_t(r11c)) | (r11c << 32), 1) & 0x1;  // rlwinm r10,r11,1,31,31
    const uint64_t r5b = r11c + r10b;                // add r5,r11,r10
    ctx.r9.u64 = r9b;
    ctx.r8.u64 = r8b;
    ctx.r4.u64 = r4b;
    ctx.r3.u64 = r3b;
    ctx.r7.u64 = r7b;
    ctx.r11.u64 = r11c;
    ctx.r10.u64 = r10b;
    ctx.r5.u64 = r5b;
  }
  // ===== 8244_ED58 (bl with lr = kVueltaDibujo): mflr r12; bl __savegprlr_29 (lr only); stwu r1,-112(r1) =====
  ctx.r12.u64 = kVueltaDibujo;
  uint64_t r1d;
  {
    const uint32_t ea = uint32_t(r1) - kMarcoDibujo;
    E32(base, ea, uint32_t(r1));
    r1d = (r1 & 0xFFFFFFFF00000000ull) | ea;
  }
  const uint64_t r31d = ctx.r3.u64;  // mr r31,r3: E
  const uint64_t r29d = ctx.r4.u64;  // mr r29,r4
  const uint64_t r30d = ctx.r5.u64;  // mr r30,r5
  {
    const uint64_t r11d = L16(base, uint32_t(r31d) + 6);  // lhz r11,6(r31)
    const uint64_t r10d = (uint64_t(uint32_t(r11d)) | (r11d << 32)) & 0x20;  // rlwinm r10,r11,0,26,26
    ctx.r11.u64 = r11d;
    ctx.r10.u64 = r10d;
    if (uint32_t(r10d) == 0) {  // cmplwi cr6,r10,0; bne loc_8244EDD0
      ctx.r3.u64 = L32(base, uint32_t(r31d) + 28);  // lwz r3,28(r31)
      ctx.r1.u64 = r1d;
      ctx.lr = kVueltaEfecto;
      L::Llamar(ctx, base, kEfecto);  // effect parameters, through its hook
      r1d = ctx.r1.u64;
      ctx.r6.u64 = r29d;                             // mr r6,r29
      ctx.r5.u64 = 0;                                // li r5,0
      ctx.r4.u64 = 4;                                // li r4,4
      ctx.r3.u64 = L32(base, kDispositivo);          // lis r11,-32093; lwz r3,-12448(r11)
      const uint64_t r11e = __builtin_rotateleft64(uint64_t(uint32_t(r30d)) | (r30d << 32), 1) & 0xFFFFFFFE;  // rlwinm r11,r30,1,0,30
      ctx.r11.u64 = r11e;
      ctx.r7.u64 = r30d + r11e;                      // add r7,r30,r11
      ctx.r1.u64 = r1d;
      ctx.lr = kVueltaDibujar;
      L::Llamar(ctx, base, kDibujar);  // DrawIndexedVertices, through its hook
      r1d = ctx.r1.u64;
      const uint64_t r9e = L16(base, uint32_t(r31d) + 6);  // lhz r9,6(r31)
      const uint64_t r8e = (uint64_t(uint32_t(r9e)) | (r9e << 32)) & 0x10;  // rlwinm r8,r9,0,27,27
      ctx.r9.u64 = r9e;
      ctx.r8.u64 = r8e;
      if (uint32_t(r8e) != 0) {  // cmplwi cr6,r8,0; beq loc_8244EDEC
        camino |= kCaminoSegundo;
        ctx.r5.u64 = r30d;  // mr r5,r30
        ctx.r4.u64 = r29d;  // mr r4,r29
        ctx.r3.u64 = r31d;  // mr r3,r31
        ctx.r1.u64 = r1d;
        ctx.lr = kVueltaSegundo;
        L::Llamar(ctx, base, kSegundo);  // bl 8244_EDF8
        r1d = ctx.r1.u64;
      }
    } else {  // loc_8244EDD0: the virtual call [[E]+20]
      camino |= kCaminoVirtual;
      const uint64_t r7d = L32(base, uint32_t(r31d) + 0);  // lwz r7,0(r31)
      ctx.r7.u64 = r7d;
      ctx.r5.u64 = r30d;  // mr r5,r30
      ctx.r4.u64 = r29d;  // mr r4,r29
      ctx.r3.u64 = r31d;  // mr r3,r31
      const uint64_t r6d = L32(base, uint32_t(r7d) + 20);  // lwz r6,20(r7)
      ctx.r6.u64 = r6d;
      ctx.r1.u64 = r1d;
      ctx.lr = kVueltaVirtual;
      L::Indirecta(ctx, base, uint32_t(r6d));  // mtctr r6; bctrl
      r1d = ctx.r1.u64;
    }
  }
  r1 = r1d + kMarcoDibujo;  // addi r1,r1,112 (and b __restgprlr_29: returns without touching lr)
  // --- Epilogue of sub_82452730: addi r1,r1,112; lwz r12,-8(r1); mtlr r12 (ld r30/r31: unused loads) ---
  r1 = r1 + kMarco;
  ctx.r1.u64 = r1;
  ctx.r12.u64 = L32(base, uint32_t(r1) - 8u);
  ctx.lr = ctx.r12.u64;
  return camino;
}

// ---------------------------------------------------------------------------------------------------------------
// The whole sub_82454B50: the list loop. Returns the number of draws it made.
// ---------------------------------------------------------------------------------------------------------------
template <class L>
uint32_t NativaBucle(PPCContext& ctx, uint8_t* base) {
  // --- Prologue: mflr r12; bl __savegprlr_26 (lr only); stwu r1,-144(r1) ---
  ctx.r12.u64 = ctx.lr;
  ctx.lr = kVueltaBuclePrologo;
  uint64_t r1 = ctx.r1.u64;
  {
    const uint32_t ea = uint32_t(r1) - kMarcoBucle;
    E32(base, ea, uint32_t(r1));
    r1 = (r1 & 0xFFFFFFFF00000000ull) | ea;
  }
  const uint64_t r27 = ctx.r4.u64;  // mr r27,r4: la lista
  const uint64_t r28 = ctx.r3.u64;  // mr r28,r3: la vista
  const uint64_t r11a = r27 + 8;    // addi r11,r27,8
  AnticiparBloque(base, uint32_t(r11a));  // the game's dcbt r0,r11: the entries
  const uint64_t r11b = L32(base, uint32_t(r11a) + 4);  // lwz r11,4(r11): the index of the first one
  const uint64_t r29 = kObjetosMenos4;                  // lis r10,-32101; addi r29,r10,-1440
  const uint64_t r11c = r11b * 52;                      // mulli r11,r11,52
  const uint64_t r10a = r11c + (r29 + 4);               // addi r10,r29,4; add r10,r11,r10
  AnticiparObjeto(base, uint32_t(r10a));                // the game's dcbt r0,r10: the first object
  const uint64_t r30a = L64(base, uint32_t(r27));       // ld r30,0(r27): the count
  E32(base, uint32_t(r1) + 84u, 0);                     // stw r26,84(r1) (r26 = 0)
  E32(base, uint32_t(r1) + 80u, 0);                     // stw r26,80(r1)
  ctx.r11.u64 = r11c;
  ctx.r10.u64 = r10a;
  uint32_t dibujos = 0;
  if (int32_t(uint32_t(r30a)) > 0) {  // cmpwi cr6,r30,0; ble loc_82454BF4
    uint64_t r30 = r30a;
    uint64_t r31 = r27 + 12;  // addi r31,r27,12
    do {  // loc_82454BA4
      const uint64_t r8 = r31 + 4;                         // addi r8,r31,4
      AnticiparBloque(base, uint32_t(r8) + 128u);          // the game's li r7,128; dcbt r7,r8: the list ahead
      const uint64_t r6 = L32(base, uint32_t(r31) + 8);    // lwz r6,8(r31): the index of the next one
      AnticiparObjeto(base, uint32_t(r6 * 52 + (r29 + 4)));  // the game's mulli r11,r6,52; add r5,r11,r10; dcbt r0,r5
      const uint64_t r4 = L32(base, uint32_t(r31));        // lwz r4,0(r31): the index of this one
      const uint64_t r11 = r4 * 52;                        // mulli r11,r4,52
      ctx.r3.u64 = r11 + (r29 + 4);                        // addi r10,r29,4; add r3,r11,r10
      ctx.r4.u64 = r28;                                    // mr r4,r28
      ctx.r5.u64 = r1 + 84;                                // addi r5,r1,84
      ctx.r6.u64 = r1 + 80;                                // addi r6,r1,80
      ctx.r7.u64 = 128;
      ctx.r8.u64 = r8;
      ctx.r10.u64 = r29 + 4;
      ctx.r11.u64 = r11;
      ctx.r1.u64 = r1;
      ctx.lr = kVueltaPegamento;
      L::Pegamento(ctx, base);  // sub_82452730, through its hook
      r1 = ctx.r1.u64;
      ++dibujos;
      r30 -= 1;  // addi r30,r30,-1
      r31 += 8;  // addi r31,r31,8
    } while (uint32_t(r30) != 0);  // cmplwi cr6,r30,0; bne loc_82454BA4
  }
  // --- loc_82454BF4 ---
  ctx.r10.u64 = kLis32093;  // lis r10,-32093
  ctx.r11.u64 = 0;          // mr r11,r26
  E32(base, kIndicesActuales, 0);  // stw r11,-11940(r10)
  E64(base, uint32_t(r27), 0);     // std r26,0(r27)
  r1 += kMarcoBucle;               // addi r1,r1,144 (and b __restgprlr_26: returns without touching lr)
  ctx.r1.u64 = r1;
  return dibujos;
}

// ---------------------------------------------------------------------------------------------------------------
// Guard: call recorder, memory regions and the literal copies of the originals.
// ---------------------------------------------------------------------------------------------------------------
constexpr uint32_t kMaxLlamadas = 16;    // glue: at most 12 (state, view, 6 streams, indices, 2 for the draw)
constexpr uint32_t kFotosBucle = 32;     // loop: full snapshot of the first ones
constexpr uint32_t kMaxBucle = 4096;     // and a summary of all of them so far (longer lists: not compared)
constexpr uint32_t kMaxZonas = 6;
constexpr uint32_t kMaxBytesZonas = 512;

static_assert(offsetof(PPCContext, r13) == offsetof(PPCContext, r3) + 13 * sizeof(PPCRegister),
              "r3, r0, r1, r2, r4..r13 seguidos");
static_assert(offsetof(PPCContext, f13) == offsetof(PPCContext, f0) + 13 * sizeof(PPCRegister), "f0..f13 seguidos");
static_assert(offsetof(PPCContext, v13) == offsetof(PPCContext, v0) + 13 * sizeof(PPCVRegister), "v0..v13 seguidos");

// What a call (or the exit) sees: all volatile registers, the FPCR, the last indirect target and the
// memory written.
struct Foto {
  uint32_t que;
  uint32_t destino;
  uint64_t memoria;    // sum of the regions at that moment: the order of stores relative to calls
  uint64_t r[14];      // r3, r0, r1, r2, r4 ... r13 (PPCContext order)
  uint64_t lr;
  uint64_t f[14];      // f0 ... f13
  uint8_t v[14 * 16];  // v0 ... v13
  uint32_t csr;
  uint32_t ultimo_indirecto;
};
static_assert(sizeof(Foto) == 8 + 8 + 14 * 8 + 8 + 14 * 8 + 14 * 16 + 8, "Foto sin relleno");
constexpr const char* kNombresR[14] = {"r3", "r0", "r1", "r2", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12",
                                       "r13"};

void Fotografiar(const PPCContext& c, Foto& f) {
  std::memcpy(f.r, &c.r3, sizeof(f.r));
  f.lr = c.lr;
  std::memcpy(f.f, &c.f0, sizeof(f.f));
  std::memcpy(f.v, &c.v0, sizeof(f.v));
  f.csr = c.fpscr.csr;
  f.ultimo_indirecto = c.last_indirect_target;
}

uint64_t Resumen(const Foto& f) {  // of the full snapshot (no padding), 8 bytes at a time
  static_assert(sizeof(Foto) % 8 == 0, "Foto en palabras de 8 bytes");
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&f);
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < sizeof(Foto); i += 8) {
    uint64_t w;
    std::memcpy(&w, p + i, 8);
    h = (h ^ w) * 0x100000001B3ull;
    h ^= h >> 29;
  }
  return h;
}

// As a called function would: the volatile registers change (to a value that depends on the call).
void Ensuciar(PPCContext& c, uint32_t k) {
  const uint64_t marca = 0xC0DE000000000000ull | (uint64_t(k) << 24);
  PPCRegister* const r = &c.r3;  // r3, r0, r1, r2, r4 ... r13
  for (uint32_t i = 0; i < 14; ++i) {
    if (i != 2 && i != 3 && i != 13) {  // nobody touches r1, r2 and r13
      r[i].u64 = marca | (0x10u + i);
    }
  }
  c.lr = marca | 0xFF;
  PPCRegister* const f = &c.f0;
  for (uint32_t i = 0; i < 14; ++i) {
    f[i].u64 = marca | (0x100u + i);
  }
  uint8_t* const v = c.v0.u8;
  for (uint32_t i = 0; i < 14 * 16; ++i) {
    v[i] = uint8_t(0xA5u ^ (k * 29u) ^ i);
  }
  c.last_indirect_target = 0xC0DE0000u | k;
}

struct Zonas {
  uint32_t n = 0;
  uint32_t total = 0;
  uint32_t direccion[kMaxZonas];
  uint32_t bytes[kMaxZonas];
  bool Agregar(uint32_t d, uint32_t b) {
    if (n == kMaxZonas || total + b > kMaxBytesZonas || uint64_t(d) + b > 0xE0000000ull) {
      return false;
    }
    direccion[n] = d;
    bytes[n] = b;
    ++n;
    total += b;
    return true;
  }
  bool Toca(uint32_t d, uint32_t b) const {
    for (uint32_t i = 0; i < n; ++i) {
      if (uint64_t(d) < uint64_t(direccion[i]) + bytes[i] && uint64_t(direccion[i]) < uint64_t(d) + b) {
        return true;
      }
    }
    return false;
  }
  void Copiar(uint8_t* base, uint8_t* destino) const {
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; ++i) {
      std::memcpy(destino + o, REX_RAW_ADDR(direccion[i]), bytes[i]);
      o += bytes[i];
    }
  }
  void Restaurar(uint8_t* base, const uint8_t* origen) const {  // in reverse: if two regions overlap, the earlier one wins
    uint32_t o = total;
    for (uint32_t i = n; i-- > 0;) {
      o -= bytes[i];
      std::memcpy(REX_RAW_ADDR(direccion[i]), origen + o, bytes[i]);
    }
  }
  uint64_t Suma(uint8_t* base) const {  // 8 bytes at a time and the rest one byte at a time
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < n; ++i) {
      const uint8_t* p = REX_RAW_ADDR(direccion[i]);
      uint32_t k = 0;
      for (; k + 8 <= bytes[i]; k += 8) {
        uint64_t w;
        std::memcpy(&w, p + k, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
      }
      for (; k < bytes[i]; ++k) {
        h = (h ^ p[k]) * 0x100000001B3ull;
      }
      h = (h ^ direccion[i]) * 0x100000001B3ull;
    }
    return h;
  }
};

constexpr uint32_t kMinimoPuntero = 0x10000u;

// The regions the glue writes in a dry run (no calls): the stack [r1-240, r1), [r5], [r6] and the global
// index buffer. They depend only on registers. It also checks that whatever the dry run will read as a
// pointer looks like a pointer and that no region overlaps anything that decides an address: otherwise it
// does not compare (only the original runs).
bool CalcularZonas(const PPCContext& ctx, uint8_t* base, Zonas& z) {
  const uint32_t pila = ctx.r1.u32;
  const uint32_t obj = ctx.r3.u32;
  const uint32_t vista = ctx.r4.u32;
  const uint32_t p5 = ctx.r5.u32;
  const uint32_t p6 = ctx.r6.u32;
  if (pila < kMinimoPuntero + kPilaVigilada || obj < kMinimoPuntero || vista < kMinimoPuntero ||
      p5 < kMinimoPuntero || p6 < kMinimoPuntero) {
    return false;
  }
  if (!z.Agregar(pila - kPilaVigilada, kPilaVigilada) || !z.Agregar(p6, 4) || !z.Agregar(p5, 4) ||
      !z.Agregar(kIndicesActuales, 4)) {
    return false;
  }
  if (z.Toca(obj, 32) || z.Toca(vista + 5, 1)) {
    return false;
  }
  const uint32_t e = L32(base, obj + 12);
  const uint32_t a = L32(base, obj + 0);
  const uint32_t p = L32(base, obj + 4);
  if (e < kMinimoPuntero || a < kMinimoPuntero || p < kMinimoPuntero || z.Toca(e, 32) || z.Toca(a, 256) ||
      z.Toca(p, 4)) {
    return false;
  }
  const uint32_t m = L32(base, p);
  if (m < kMinimoPuntero || z.Toca(m + 24, 4)) {
    return false;
  }
  if ((L16(base, e + 6) & 0x20u) != 0) {
    const uint32_t tabla = L32(base, e);
    if (tabla < kMinimoPuntero || z.Toca(tabla + 20, 4)) {
      return false;
    }
  }
  return true;
}

// The loop: its frame [r1-144, r1-56) (the stwu and the two zeros at r1+80 and r1+84), [lista+0] (8) and
// the global.
bool CalcularZonasBucle(const PPCContext& ctx, uint8_t* base, Zonas& z, uint32_t& cuenta) {
  const uint32_t pila = ctx.r1.u32;
  const uint32_t lista = ctx.r4.u32;
  if (pila < kMinimoPuntero + kMarcoBucle || lista < kMinimoPuntero) {
    return false;
  }
  const uint32_t n = uint32_t(L64(base, lista));
  cuenta = int32_t(n) > 0 ? n : 0;
  if (cuenta > kMaxBucle) {
    return false;
  }
  if (!z.Agregar(pila - kMarcoBucle, 88) || !z.Agregar(lista, 8) || !z.Agregar(kIndicesActuales, 4)) {
    return false;
  }
  return !z.Toca(lista + 8, 8u * (cuenta + 1u));  // the entries must not lie in a region
}

struct Grabacion {
  uint32_t n;  // calls made (beyond kMaxLlamadas only the first are kept and nothing is compared)
  Foto llamadas[kMaxLlamadas];
  Foto salida;
};
struct GrabacionBucle {
  uint32_t n;
  Foto llamadas[kFotosBucle];
  uint64_t resumen[kMaxBucle];
  Foto salida;
};
// Guard state. Global rather than per thread (on the Switch a large TLS is paid by every thread of the
// process): one thread uses it at a time (g_comprobando); if another thread arrived meanwhile, that call
// only runs the original.
std::atomic<bool> g_comprobando{false};
std::atomic<bool> g_comprobando_bucle{false};
Grabacion* t_grabacion = nullptr;
GrabacionBucle* t_grabacion_bucle = nullptr;
const Zonas* t_zonas = nullptr;
const Zonas* t_zonas_bucle = nullptr;
uint8_t* t_base = nullptr;
Grabacion g_copia;
Grabacion g_nativa;
GrabacionBucle g_copia_bucle;
GrabacionBucle g_nativa_bucle;
uint8_t g_antes[kMaxBytesZonas];
uint8_t g_mem_copia[kMaxBytesZonas];
uint8_t g_mem_nativa[kMaxBytesZonas];

[[gnu::noinline]] void Grabar(PPCContext& ctx, uint32_t que, uint32_t destino) {
  Grabacion& g = *t_grabacion;
  const uint32_t k = g.n++;
  if (k < kMaxLlamadas) {
    Foto& f = g.llamadas[k];
    Fotografiar(ctx, f);
    f.que = que;
    f.destino = destino;
    f.memoria = t_zonas->Suma(t_base);
  }
  Ensuciar(ctx, k);
}

[[gnu::noinline]] void GrabarBucle(PPCContext& ctx) {
  GrabacionBucle& g = *t_grabacion_bucle;
  const uint32_t k = g.n++;
  if (k < kMaxBucle) {
    Foto f;
    Fotografiar(ctx, f);
    f.que = kPegamento;
    f.destino = 0;
    f.memoria = t_zonas_bucle->Suma(t_base);
    g.resumen[k] = Resumen(f);
    if (k < kFotosBucle) {
      g.llamadas[k] = f;
    }
  }
  Ensuciar(ctx, k);
}

// Dry run: nothing is called; what the call would see is recorded and the volatile registers are clobbered.
struct Grabador {
  static void Llamar(PPCContext& ctx, uint8_t* base, uint32_t que) { Grabar(ctx, que, 0); }
  static void Indirecta(PPCContext& ctx, uint8_t* base, uint32_t destino) { Grabar(ctx, kIndirecta, destino); }
  static void Pegamento(PPCContext& ctx, uint8_t* base) { GrabarBucle(ctx); }
};

// ==== Literal copy of sub_82452730 and of the two functions it calls that the native version contains
// ==== (generated by copia_literal.py): the generated code without comments; the calls go through the policy.
// ==== Do not edit by hand: the patch checks that it equals the current generated code.
#define NFSMW_PEGAMENTO_LLAMAR(c, b, que) Llamadas::Llamar(c, b, que)
#define NFSMW_PEGAMENTO_INDIRECTA(c, b, destino) Llamadas::Indirecta(c, b, destino)
#define NFSMW_PEGAMENTO_FLUJOS(c, b) CopiaFlujos<Llamadas>(c, b)
#define NFSMW_PEGAMENTO_DIBUJO(c, b) CopiaDibujo<Llamadas>(c, b)
#include "copias_literales/CopiaFlujos.inc"

#include "copias_literales/CopiaDibujo.inc"

#include "copias_literales/CopiaPegamento.inc"
#undef NFSMW_PEGAMENTO_LLAMAR
#undef NFSMW_PEGAMENTO_INDIRECTA
#undef NFSMW_PEGAMENTO_FLUJOS
#undef NFSMW_PEGAMENTO_DIBUJO
// ==== End of the literal copy of the glue

// ==== Literal copy of sub_82454B50, generated by tools/copia_literal.py. Do not edit by hand.
#define NFSMW_BUCLE_LLAMAR(c, b) Llamadas::Pegamento(c, b)
#include "copias_literales/CopiaBucle.inc"
#undef NFSMW_BUCLE_LLAMAR
// ==== End of the literal copy of the loop

// ---------------------------------------------------------------------------------------------------------------
// Counters, report, guards and calls.
// ---------------------------------------------------------------------------------------------------------------
constexpr uint64_t kComprobaciones = 20000;   // glue: first calls checked
constexpr uint64_t kMinimoRaro = 2000;        // and the first ones of each rare path
constexpr uint64_t kVentanaRaros = 1000000;   // (while within these calls: afterwards, 1 in every kPeriodo)
constexpr uint64_t kPeriodo = 4096;           // afterwards, 1 in every kPeriodo (power of 2)
constexpr uint64_t kMedidaOriginal = 64;      // 1 in 64 runs the original, timed (power of 2)
// Loop: each check walks the whole list twice in a dry run (~0.5 us per draw: ~1 ms for the large list),
// so only a few at the start and then very rarely (1 in 4096: one every ~5 s, ~0.2 ms/s).
constexpr uint64_t kComprobacionesBucle = 500;
constexpr uint32_t kBitsPeriodoBucle = 12;        // afterwards, 1 in 4096 at random (Toca)
constexpr uint32_t kBitsMedidaOriginalBucle = 5;  // 1 in 32 at random through the original, timed

enum Motivo : uint32_t { kPorApagada = 0, kPorComprobacion = 1, kPorMedida = 2, kMotivos = 3 };

// Counters without atomic read-modify-write (the A57 has no LSE): the Main XThread writes them; if another
// thread counted at the same time some counts would be lost, which does not matter for the report.
std::atomic<uint64_t> g_llamadas{0};
std::atomic<uint64_t> g_por_combinacion[kCombinaciones];  // native calls in the period, per path combination
std::atomic<uint64_t> g_originales[kMotivos];             // in the period
std::atomic<uint64_t> g_ns_nativa{0}, g_muestras_nativa{0};
std::atomic<uint64_t> g_ns_original{0}, g_muestras_original{0};
std::atomic<uint64_t> g_comprobadas{0};                   // since startup, all without mismatches
std::atomic<uint64_t> g_comprobadas_camino[kCaminos];
std::atomic<uint64_t> g_saltadas{0};                      // guard runs not compared (unbounded regions, too many calls)
std::atomic<uint32_t> g_raros_listos{0};                  // caminos raros con kMinimoRaro comprobadas
std::atomic<bool> g_raros_abiertos{true};
std::atomic<bool> g_apagado{false};
// The loop
std::atomic<uint64_t> g_llamadas_bucle{0};
std::atomic<uint64_t> g_nativas_bucle{0}, g_dibujos_bucle{0};
std::atomic<uint64_t> g_originales_bucle[kMotivos];
std::atomic<uint64_t> g_ns_nativa_bucle{0}, g_dibujos_nativa_bucle{0};
std::atomic<uint64_t> g_ns_original_bucle{0}, g_dibujos_original_bucle{0};
std::atomic<uint64_t> g_comprobadas_bucle{0};
std::atomic<uint64_t> g_saltadas_bucle{0};
std::atomic<bool> g_apagado_bucle{false};
std::atomic<int64_t> g_siguiente_ms{0};
std::atomic<int8_t> g_activo{-1};

template <typename T>
inline void Sumar(std::atomic<T>& c, T n) {
  c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

// 1 in 2^bits loop calls, spread at random: the loop is called almost the same number of times every
// frame, and with a mask (n & 255) the same list would always be checked or timed.
inline bool Toca(uint64_t n, uint32_t bits, uint64_t cual) {
  return ((n * 0x9E3779B97F4A7C15ull) >> (64 - bits)) == cual;
}

inline int64_t AhoraNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline bool Activo() {
  int8_t a = g_activo.load(std::memory_order_relaxed);
  if (a < 0) [[unlikely]] {
    a = REXCVAR_GET(nfsmw_pegamento_nativo) ? 1 : 0;
    g_activo.store(a, std::memory_order_relaxed);
  }
  return a != 0;
}

void Informe() {
  const int64_t ahora = AhoraNs() / 1000000;
  const int64_t siguiente = g_siguiente_ms.load(std::memory_order_relaxed);
  if (ahora < siguiente) {
    return;
  }
  g_siguiente_ms.store(ahora + 10000, std::memory_order_relaxed);
  if (siguiente == 0) {
    REXLOG_INFO("[pegamento] pegamento de dibujo (sub_82452730) y su bucle (sub_82454B50) {}",
                Activo() ? "en nativo (build 186): empiezan comprobando contra la copia literal de la original"
                         : "por la original (nfsmw_pegamento_nativo = false)");
    return;
  }
  // The glue
  uint64_t por_camino[kCaminos] = {};
  uint64_t nativas = 0;
  for (uint32_t c = 0; c < kCombinaciones; ++c) {
    const uint64_t k = g_por_combinacion[c].exchange(0, std::memory_order_relaxed);
    nativas += k;
    for (uint32_t b = 0; b < kCaminos; ++b) {
      if (c & (1u << b)) {
        por_camino[b] += k;
      }
    }
  }
  uint64_t originales[kMotivos];
  uint64_t total_originales = 0;
  for (uint32_t m = 0; m < kMotivos; ++m) {
    originales[m] = g_originales[m].exchange(0, std::memory_order_relaxed);
    total_originales += originales[m];
  }
  const uint64_t ns_n = g_ns_nativa.exchange(0, std::memory_order_relaxed);
  const uint64_t k_n = g_muestras_nativa.exchange(0, std::memory_order_relaxed);
  const uint64_t ns_o = g_ns_original.exchange(0, std::memory_order_relaxed);
  const uint64_t k_o = g_muestras_original.exchange(0, std::memory_order_relaxed);
  const double us_n = k_n ? double(ns_n) / double(k_n) / 1000.0 : 0.0;
  const double us_o = k_o ? double(ns_o) / double(k_o) / 1000.0 : 0.0;
  const double ahorro = (k_n && k_o) ? (us_o - us_n) * double(nativas) / 10.0 / 1000.0 : 0.0;
  NFSMW_INFORME_DIFERIDO(
      "[pegamento] ultimos 10 s: {:.0f} llamadas/s; {} en nativo ({} {}, {} {}, {} {}, {} {}, {} {}, {} {}, {} {}), {} "
      "por la original (apagada {}, comprobacion {}, medida {}); nativa {:.3f} us/llamada ({} cronometradas), original "
      "{:.3f} us/llamada ({}) con todo lo que llaman: ahorro {:.2f} ms/s; comprobadas contra la copia literal desde el "
      "arranque: {} (sin comparar {}){}",
      double(nativas + total_originales) / 10.0, nativas, kNombresCamino[0], por_camino[0], kNombresCamino[1],
      por_camino[1], kNombresCamino[2], por_camino[2], kNombresCamino[3], por_camino[3], kNombresCamino[4],
      por_camino[4], kNombresCamino[5], por_camino[5], kNombresCamino[6], por_camino[6], total_originales,
      originales[kPorApagada], originales[kPorComprobacion], originales[kPorMedida], us_n, k_n, us_o, k_o, ahorro,
      g_comprobadas.load(std::memory_order_relaxed), g_saltadas.load(std::memory_order_relaxed),
      g_apagado.load(std::memory_order_relaxed) ? " | APAGADA por diferencia" : "");
  // The loop
  const uint64_t nb = g_nativas_bucle.exchange(0, std::memory_order_relaxed);
  const uint64_t db = g_dibujos_bucle.exchange(0, std::memory_order_relaxed);
  uint64_t ob[kMotivos];
  uint64_t total_ob = 0;
  for (uint32_t m = 0; m < kMotivos; ++m) {
    ob[m] = g_originales_bucle[m].exchange(0, std::memory_order_relaxed);
    total_ob += ob[m];
  }
  const uint64_t ns_nb = g_ns_nativa_bucle.exchange(0, std::memory_order_relaxed);
  const uint64_t d_nb = g_dibujos_nativa_bucle.exchange(0, std::memory_order_relaxed);
  const uint64_t ns_ob = g_ns_original_bucle.exchange(0, std::memory_order_relaxed);
  const uint64_t d_ob = g_dibujos_original_bucle.exchange(0, std::memory_order_relaxed);
  const double us_nb = d_nb ? double(ns_nb) / double(d_nb) / 1000.0 : 0.0;
  const double us_ob = d_ob ? double(ns_ob) / double(d_ob) / 1000.0 : 0.0;
  const double ahorro_b = (d_nb && d_ob) ? (us_ob - us_nb) * double(db) / 10.0 / 1000.0 : 0.0;
  NFSMW_INFORME_DIFERIDO(
      "[pegamento] bucle de la lista, ultimos 10 s: {:.0f} llamadas/s y {:.0f} dibujos/s en nativo; {} por la original "
      "(apagada {}, comprobacion {}, medida {}); nativa {:.3f} us/dibujo ({} dibujos cronometrados), original {:.3f} "
      "us/dibujo ({}): ahorro {:.2f} ms/s; comprobadas desde el arranque: {} (sin comparar {}){}",
      double(nb) / 10.0, double(db) / 10.0, total_ob, ob[kPorApagada], ob[kPorComprobacion], ob[kPorMedida], us_nb,
      d_nb, us_ob, d_ob, ahorro_b, g_comprobadas_bucle.load(std::memory_order_relaxed),
      g_saltadas_bucle.load(std::memory_order_relaxed),
      g_apagado_bucle.load(std::memory_order_relaxed) ? " | APAGADO por diferencia" : "");
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

// The first difference between two snapshots ("" if they are equal).
std::string Diferencia(const Foto& a, const Foto& b) {
  if (a.que != b.que || a.destino != b.destino) {
    return fmt::format("llamada {}/0x{:08X} frente a {}/0x{:08X}", a.que, a.destino, b.que, b.destino);
  }
  for (uint32_t i = 0; i < 14; ++i) {
    if (a.r[i] != b.r[i]) {
      return fmt::format("{} 0x{:X} frente a 0x{:X}", kNombresR[i], a.r[i], b.r[i]);
    }
  }
  if (a.lr != b.lr) {
    return fmt::format("lr 0x{:X} frente a 0x{:X}", a.lr, b.lr);
  }
  for (uint32_t i = 0; i < 14; ++i) {
    if (a.f[i] != b.f[i]) {
      return fmt::format("f{} 0x{:016X} frente a 0x{:016X}", i, a.f[i], b.f[i]);
    }
  }
  for (uint32_t i = 0; i < 14; ++i) {
    if (std::memcmp(a.v + 16 * i, b.v + 16 * i, 16) != 0) {
      return fmt::format("v{} {} frente a {}", i, Hex(a.v + 16 * i, 16), Hex(b.v + 16 * i, 16));
    }
  }
  if (a.csr != b.csr) {
    return fmt::format("FPCR 0x{:X} frente a 0x{:X}", a.csr, b.csr);
  }
  if (a.ultimo_indirecto != b.ultimo_indirecto) {
    return fmt::format("ultimo indirecto 0x{:X} frente a 0x{:X}", a.ultimo_indirecto, b.ultimo_indirecto);
  }
  if (a.memoria != b.memoria) {
    return fmt::format("la memoria escrita hasta ahi (suma 0x{:016X} frente a 0x{:016X})", a.memoria, b.memoria);
  }
  return std::string();
}

// The first difference in the memory they leave behind ("" if equal).
std::string DiferenciaMemoria(const Zonas& z, const uint8_t* copia, const uint8_t* nativa) {
  uint32_t o = 0;
  for (uint32_t i = 0; i < z.n; ++i) {
    for (uint32_t b = 0; b < z.bytes[i]; ++b) {
      if (copia[o + b] != nativa[o + b]) {
        const uint32_t m = z.bytes[i] - b < 8u ? z.bytes[i] - b : 8u;
        return fmt::format("memoria en 0x{:08X} (zona {} +{}): original {} nativa {}", z.direccion[i] + b, i, b,
                           Hex(copia + o + b, m), Hex(nativa + o + b, m));
      }
    }
    o += z.bytes[i];
  }
  return std::string();
}

// Glue guard: dry runs of the literal copy and of the native version, each one undone, then compared, and
// then the original for real.
[[gnu::noinline]] void Comprobar(PPCContext& ctx, uint8_t* base) {
  Zonas z;
  if (g_comprobando.exchange(true, std::memory_order_acquire)) {  // another thread is in the guard
    Sumar(g_saltadas, uint64_t(1));
    Sumar(g_originales[kPorComprobacion], uint64_t(1));
    __imp__sub_82452730(ctx, base);
    return;
  }
  if (!CalcularZonas(ctx, base, z)) {
    g_comprobando.store(false, std::memory_order_release);
    Sumar(g_saltadas, uint64_t(1));
    Sumar(g_originales[kPorComprobacion], uint64_t(1));
    __imp__sub_82452730(ctx, base);
    return;
  }
  Grabacion& copia = g_copia;
  Grabacion& nativa = g_nativa;
  z.Copiar(base, g_antes);
  const PPCContext entrada = ctx;
  const auto volver = [&]() {
    z.Restaurar(base, g_antes);
    ctx = entrada;
    ctx.fpscr.setcsr(ctx.fpscr.csr);  // the real FPCR, same as on entry
  };
  t_zonas = &z;
  t_base = base;
  copia.n = 0;
  t_grabacion = &copia;
  CopiaPegamento<Grabador>(ctx, base);
  Fotografiar(ctx, copia.salida);
  z.Copiar(base, g_mem_copia);
  volver();
  nativa.n = 0;
  t_grabacion = &nativa;
  const uint32_t camino = Nativa<Grabador>(ctx, base);
  Fotografiar(ctx, nativa.salida);
  z.Copiar(base, g_mem_nativa);
  volver();
  t_grabacion = nullptr;

  std::string que;
  uint32_t donde = 0;
  if (copia.n != nativa.n) {
    que = fmt::format("{} llamadas frente a {}", copia.n, nativa.n);
  } else if (copia.n <= kMaxLlamadas) {
    for (uint32_t k = 0; k < copia.n && que.empty(); ++k) {
      que = Diferencia(copia.llamadas[k], nativa.llamadas[k]);
      donde = k + 1;
    }
    if (que.empty()) {
      que = Diferencia(copia.salida, nativa.salida);
      donde = 0;
    }
    if (que.empty()) {
      que = DiferenciaMemoria(z, g_mem_copia, g_mem_nativa);
    }
  }
  const bool demasiadas = copia.n > kMaxLlamadas;
  g_comprobando.store(false, std::memory_order_release);
  __imp__sub_82452730(ctx, base);  // the original for real, from the entry state: its state is kept
  Sumar(g_originales[kPorComprobacion], uint64_t(1));
  if (que.empty() && demasiadas) {
    Sumar(g_saltadas, uint64_t(1));
    return;
  }
  if (que.empty()) {
    const uint64_t total = g_comprobadas.load(std::memory_order_relaxed) + 1;
    g_comprobadas.store(total, std::memory_order_relaxed);
    for (uint32_t b = 0; b < kCaminos; ++b) {
      if (camino & (1u << b)) {
        const uint64_t k = g_comprobadas_camino[b].load(std::memory_order_relaxed) + 1;
        g_comprobadas_camino[b].store(k, std::memory_order_relaxed);
        if (k == kMinimoRaro && ((1u << b) & kCaminosRaros) != 0) {
          g_raros_listos.store(g_raros_listos.load(std::memory_order_relaxed) | (1u << b), std::memory_order_relaxed);
          REXLOG_INFO("[pegamento] camino {}: {} llamadas comprobadas contra la copia literal de la original, 0 "
                      "diferencias",
                      kNombresCamino[b], kMinimoRaro);
        }
      }
    }
    if (total == kComprobaciones) {
      REXLOG_INFO("[pegamento] {} llamadas comprobadas contra la copia literal de la original (en cada llamada que "
                  "hace: r0-r13, lr, f0-f13, v0-v13, FPCR, ultimo indirecto y la memoria escrita; y la salida, la pila, "
                  "[r5], [r6] y la global de indices), 0 diferencias: en nativo, y sigue comprobando 1 de cada {}",
                  total, kPeriodo);
    }
    return;
  }
  g_apagado.store(true, std::memory_order_relaxed);  // the state is already the original's
  REXLOG_ERROR("[pegamento] DIFERENCIA con la original ({}{}; camino 0x{:X}): entrada objeto 0x{:08X}, vista 0x{:08X}, "
               "r5 0x{:08X}, r6 0x{:08X}, pila 0x{:08X}. Pegamento nativo APAGADO para el resto de la sesion: se "
               "queda la original",
               que, donde ? fmt::format(", en la llamada {}", donde) : std::string(", a la salida"), camino,
               entrada.r3.u32, entrada.r4.u32, entrada.r5.u32, entrada.r6.u32, entrada.r1.u32);
}

// Loop guard: the same, with a summary of each call (the first kFotosBucle with a full snapshot).
[[gnu::noinline]] void ComprobarBucle(PPCContext& ctx, uint8_t* base) {
  Zonas z;
  uint32_t cuenta = 0;
  if (g_comprobando_bucle.exchange(true, std::memory_order_acquire)) {
    Sumar(g_saltadas_bucle, uint64_t(1));
    Sumar(g_originales_bucle[kPorComprobacion], uint64_t(1));
    __imp__sub_82454B50(ctx, base);
    return;
  }
  if (!CalcularZonasBucle(ctx, base, z, cuenta)) {
    g_comprobando_bucle.store(false, std::memory_order_release);
    Sumar(g_saltadas_bucle, uint64_t(1));
    Sumar(g_originales_bucle[kPorComprobacion], uint64_t(1));
    __imp__sub_82454B50(ctx, base);
    return;
  }
  GrabacionBucle& copia = g_copia_bucle;
  GrabacionBucle& nativa = g_nativa_bucle;
  z.Copiar(base, g_antes);
  const PPCContext entrada = ctx;
  const auto volver = [&]() {
    z.Restaurar(base, g_antes);
    ctx = entrada;
    ctx.fpscr.setcsr(ctx.fpscr.csr);
  };
  t_zonas_bucle = &z;
  t_base = base;
  copia.n = 0;
  t_grabacion_bucle = &copia;
  CopiaBucle<Grabador>(ctx, base);
  Fotografiar(ctx, copia.salida);
  z.Copiar(base, g_mem_copia);
  volver();
  nativa.n = 0;
  t_grabacion_bucle = &nativa;
  NativaBucle<Grabador>(ctx, base);
  Fotografiar(ctx, nativa.salida);
  z.Copiar(base, g_mem_nativa);
  volver();
  t_grabacion_bucle = nullptr;

  std::string que;
  uint32_t donde = 0;
  if (copia.n != nativa.n) {
    que = fmt::format("{} llamadas frente a {}", copia.n, nativa.n);
  } else if (copia.n <= kMaxBucle) {
    for (uint32_t k = 0; k < copia.n && que.empty(); ++k) {
      if (k < kFotosBucle) {
        que = Diferencia(copia.llamadas[k], nativa.llamadas[k]);
      } else if (copia.resumen[k] != nativa.resumen[k]) {
        que = "lo que ve la llamada (registros o memoria escrita; resumen distinto)";
      }
      donde = k + 1;
    }
    if (que.empty()) {
      que = Diferencia(copia.salida, nativa.salida);
      donde = 0;
    }
    if (que.empty()) {
      que = DiferenciaMemoria(z, g_mem_copia, g_mem_nativa);
    }
  }
  const bool demasiadas = copia.n > kMaxBucle;
  g_comprobando_bucle.store(false, std::memory_order_release);
  __imp__sub_82454B50(ctx, base);  // the original for real
  Sumar(g_originales_bucle[kPorComprobacion], uint64_t(1));
  if (que.empty() && demasiadas) {
    Sumar(g_saltadas_bucle, uint64_t(1));
    return;
  }
  if (que.empty()) {
    const uint64_t total = g_comprobadas_bucle.load(std::memory_order_relaxed) + 1;
    g_comprobadas_bucle.store(total, std::memory_order_relaxed);
    if (total == kComprobacionesBucle) {
      REXLOG_INFO("[pegamento] bucle de la lista: {} llamadas comprobadas contra la copia literal de la original (cada "
                  "dibujo que manda y la salida, su marco, [lista+0] y la global de indices), 0 diferencias: en nativo, "
                  "y sigue comprobando 1 de cada {} al azar",
                  total, 1u << kBitsPeriodoBucle);
    }
    return;
  }
  g_apagado_bucle.store(true, std::memory_order_relaxed);
  REXLOG_ERROR("[pegamento] DIFERENCIA en el bucle de la lista con la original ({}{}): entrada vista 0x{:08X}, lista "
               "0x{:08X} ({} dibujos), pila 0x{:08X}. Bucle nativo APAGADO para el resto de la sesion: se queda la "
               "original",
               que, donde ? fmt::format(", en la llamada {}", donde) : std::string(", a la salida"), entrada.r3.u32,
               entrada.r4.u32, cuenta, entrada.r1.u32);
}

// The rare paths this call will take, read from memory beforehand (it only decides whether the guard runs
// it). They are loads the original always makes ([vista+5], E, [E+24], [E+6], A and [A+94]), and only those
// of the paths still missing: reading A this early defeats the A hint while the window lasts.
uint32_t Predecir(const PPCContext& ctx, uint8_t* base, uint32_t pendientes) {
  const uint32_t obj = ctx.r3.u32;
  uint32_t c = 0;
  if ((pendientes & kCaminoVistaE20) != 0 && L8(base, ctx.r4.u32 + 5) != 0) {
    c |= kCaminoVistaE20;
  }
  if ((pendientes & (kCaminoBanderas | kCaminoVirtual | kCaminoSegundo)) != 0) {
    const uint32_t e = L32(base, obj + 12);
    if (L32(base, e + 24) != 0) {
      c |= kCaminoBanderas;
    }
    const uint32_t b = L16(base, e + 6);
    if ((b & 0x20u) != 0) {
      c |= kCaminoVirtual;
    } else if ((b & 0x10u) != 0) {
      c |= kCaminoSegundo;
    }
  }
  if ((pendientes & kCaminoFlujos) != 0 && (L16(base, L32(base, obj + 0) + 94) & 0x8000u) == 0) {
    c |= kCaminoFlujos;
  }
  return c & pendientes;
}

}  // namespace

void Pegamento(PPCContext& ctx, uint8_t* base) {
  const uint64_t n = g_llamadas.load(std::memory_order_relaxed) + 1;
  g_llamadas.store(n, std::memory_order_relaxed);
  if ((n & (kPeriodo - 1)) == 0) [[unlikely]] {
    Informe();
  }
  if (!Activo() || g_apagado.load(std::memory_order_relaxed)) [[unlikely]] {
    Sumar(g_originales[kPorApagada], uint64_t(1));
    __imp__sub_82452730(ctx, base);
    return;
  }
  if (n <= kComprobaciones || (n & (kPeriodo - 1)) == 0) [[unlikely]] {
    Comprobar(ctx, base);
    return;
  }
  if (g_raros_abiertos.load(std::memory_order_relaxed)) [[unlikely]] {
    const uint32_t pendientes = kCaminosRaros & ~g_raros_listos.load(std::memory_order_relaxed);
    if (pendientes == 0 || n > kVentanaRaros) {
      g_raros_abiertos.store(false, std::memory_order_relaxed);
    } else if (Predecir(ctx, base, pendientes) != 0) {
      Comprobar(ctx, base);
      return;
    }
  }
  if ((n & (kMedidaOriginal - 1)) == kMedidaOriginal / 2) [[unlikely]] {
    const int64_t t0 = AhoraNs();
    __imp__sub_82452730(ctx, base);
    Sumar(g_ns_original, uint64_t(AhoraNs() - t0));
    Sumar(g_muestras_original, uint64_t(1));
    Sumar(g_originales[kPorMedida], uint64_t(1));
    return;
  }
  if ((n & 15) == 8) [[unlikely]] {
    const int64_t t0 = AhoraNs();
    const uint32_t c = Nativa<Reales>(ctx, base);
    Sumar(g_ns_nativa, uint64_t(AhoraNs() - t0));
    Sumar(g_muestras_nativa, uint64_t(1));
    Sumar(g_por_combinacion[c], uint64_t(1));
    return;
  }
  Sumar(g_por_combinacion[Nativa<Reales>(ctx, base)], uint64_t(1));
}

void Bucle(PPCContext& ctx, uint8_t* base) {
  const uint64_t n = g_llamadas_bucle.load(std::memory_order_relaxed) + 1;
  g_llamadas_bucle.store(n, std::memory_order_relaxed);
  if (!Activo() || g_apagado_bucle.load(std::memory_order_relaxed)) [[unlikely]] {
    Sumar(g_originales_bucle[kPorApagada], uint64_t(1));
    __imp__sub_82454B50(ctx, base);
    return;
  }
  if (n <= kComprobacionesBucle || Toca(n, kBitsPeriodoBucle, 0)) [[unlikely]] {
    ComprobarBucle(ctx, base);
    return;
  }
  if (Toca(n, kBitsMedidaOriginalBucle, 16)) [[unlikely]] {
    const uint32_t cuenta = uint32_t(L64(base, ctx.r4.u32));  // the list count (the original reads it the same way)
    const int64_t t0 = AhoraNs();
    __imp__sub_82454B50(ctx, base);
    Sumar(g_ns_original_bucle, uint64_t(AhoraNs() - t0));
    Sumar(g_dibujos_original_bucle, uint64_t(int32_t(cuenta) > 0 ? cuenta : 0));
    Sumar(g_originales_bucle[kPorMedida], uint64_t(1));
    return;
  }
  if (Toca(n, 3, 5)) [[unlikely]] {
    const int64_t t0 = AhoraNs();
    const uint32_t d = NativaBucle<Reales>(ctx, base);
    Sumar(g_ns_nativa_bucle, uint64_t(AhoraNs() - t0));
    Sumar(g_dibujos_nativa_bucle, uint64_t(d));
    Sumar(g_nativas_bucle, uint64_t(1));
    Sumar(g_dibujos_bucle, uint64_t(d));
    return;
  }
  const uint32_t d = NativaBucle<Reales>(ctx, base);
  Sumar(g_nativas_bucle, uint64_t(1));
  Sumar(g_dibujos_bucle, uint64_t(d));
}

}  // namespace nfsmw::pegamento_nativo

// The generated code's calls go to sub_82452730 (2) and to sub_82454B50 (6): the patch turns them back from
// __imp__ to sub_. The indirect-call dispatch table already points to both hooks.
REX_HOOK_RAW(sub_82452730) {  // the draw glue for one list object
  nfsmw::pegamento_nativo::Pegamento(ctx, base);
}
REX_HOOK_RAW(sub_82454B50) {  // the draw-list loop
  nfsmw::pegamento_nativo::Bucle(ctx, base);
}
