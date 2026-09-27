// nfsmw - eViewPlatInterface::Render (sub_8243E358) in native code.
//
// WHAT IT IS
//   Render(this = view, r4 = eModel, r5 = matrix, r6 = light, r7 = flags, r8 = bone palette) walks the
//   model's submeshes and, for each visible one, prepares the draw packet. It is the bottleneck all geometry
//   goes through (scene, shadows, reflections). Read instruction by instruction in nfsmw_recomp.105.cpp:16383:
//     r29 = [model+12] (eSolid); if it is 0, out. If [r29+14] & 0x800 and the byte 0x82A2CFB7 != 0, out.
//     r28 = [r29] (the mesh); calls 8250_0010(model, r29, matrix) and, if [model+16] != 0, 8221_8B18(model,
//     r1+128) (puts the replacement textures into the solid's table).
//     For each submesh i < [r28+16] (256 bytes each from [r28+20]):
//       r30 = [e+28] (material); if [r28+64] != 0, it is remapped through the table 0x8293CC68 and
//       [r28+64] = 0.
//       If !(r7 & 4) and !([e+36] & 1): GetVisibleState(view, e, e+12, matrix); if it returns 0, next.
//       The 5 textures: [[r29+44] + 8 * byte[e+43..47] + 4] -> r1+96..112 (whenever it is visible).
//       If [texture0+36] != [0x82A45274]: arg9 = byte[e+42] == 255 ? 0 : [[r29+60] + 8 * byte + 4],
//       r1+92 = r8, r1+84 = arg9, and the packet 8245_2868(view, e, r29, r7, r30, r1+96, matrix, r6).
//     At the end, 8221_8AB8(model, r1+128) (returns the replacement textures).
//
// WHY NATIVE
//   Stack sampling on the console: 2.8-4.4 % self time of a core of the draw thread, 6.5 % with its leaves.
//   Per instruction: two thirds of the samples land on 8 loads that miss in cache: the first read of each
//   submesh (+0x1D8, the hottest: the PowerPC requested it with dcbt, which the recompiled code drops),
//   [texture0+36] (+0x2F4), the texture table and the model entry. The remaining third is the recompiled
//   code itself: every PowerPC register ends up written to the context and, since writes to guest memory
//   may overlap the context, r1 is reread after each one (5 times per submesh just for r1+96..112).
//   Here: the registers live in variables, the context is only written to make calls, and the submesh i+2
//   and the first texture and material of i+1 are requested in advance (no side effects: PRFM).
//   Measured on PC (x86, indicative only), with the original packet and the replacements inside:
//     - with a cold cache (4096 models in 128 MB, as on the console): 270-430 ns less per call (20-32 ns
//       per submesh, 10-13 % of the whole call). Without the prefetches, almost nothing: the saving comes
//       from hiding misses.
//     - with a warm cache: 25-40 ns less per call. Prefetching all five textures cost 40-80 ns when warm
//       without gaining when cold: only the first one is prefetched.
//   On aarch64 (devkitA64 with the Switch options), the path of a visible, drawn submesh: about 120
//   instructions plus about 26 for the prefetch, against about 170 in the original (counted by hand).
//
// THE PACKETS (8245_2868 and 8245_4428): NOT MOVED TO NATIVE CODE
//   Their matrix copy loop (the "almost all copies" part of the code) has not a single sample in the
//   profile: it only runs with bones. Their cost is recording the draw (52-byte record, sort key and bucket)
//   and the misses on [texture0+81/84], [material+4/6] and the texture keys, which the prefetch here already
//   requests earlier. What would be left to save is ~50 instructions per draw (~0.2-0.4 % of a core) in
//   exchange for the most delicate part: the copy passes 3 of every 4 words through double and the fourth
//   as is (seen in the disassembly), which depends on how GCC compiles each build, and with the heap
//   exhausted it writes 1 KB to address 0.
//
// WHY IT IS BIT-IDENTICAL
//   - Only integers are involved. The same reads and writes of guest memory, in the same relative order:
//     the frame back link (stwu), [r28+64] = 0 and r1+84/92/96..112 (stack nobody reads any more, but
//     memory ends up identical). The addresses, with the same arithmetic (64 bits where the original adds
//     in 64 and the address is the low part).
//   - The same calls, to the same names (sub_X: they go through their hooks, as in the original, whose
//     calls tools/llamadas_directas.py leaves as sub_X), in the same order, with the same 64-bit arguments,
//     the same lr and r1 = the 784-byte frame.
//   - What is prefetched is read-only and PRFM (which never faults): +28 and +43 of submesh i+1, which
//     exists and whose +28 the original always reads, and the solid's texture table only when the original
//     has already read it in that call (a solid that is not drawn could have it at 0).
//   - Registers: interprocedural liveness analysis of the 56,338 functions: after its 30 direct call sites
//     only r3 and f1 are live. r3: the input one if it exits early (paths 1 and 2) and the model (the r3 of
//     8221_8AB8, which does not touch it) if it walks the mesh. f1 and the denormal mode are only touched
//     by the functions it calls, which are the same and in the same order. It also leaves r1, r12 and lr
//     as the original does on all three paths, and r9-r11 on path 2; nobody reads the other volatiles.
//     Render never appears as a pointer in the code (no lis/addi forms its address): only its 30 bl call it.
//
// SELF-CHECKING GUARD (cvar nfsmw_eview_nativo; project rule)
//   Render calls functions with side effects (8221_8B18 changes the solid's texture table, the packet
//   records draws in global lists), so it cannot be run twice. The first kComprobacionesRender calls and
//   then 1 of every 4096:
//     1. The 9 words Render can write are snapshotted (back link, [r28+64] and r1+84..112).
//     2. The original runs for real (it is in charge). Its calls to 8250_0010, 8221_8B18, the packet and
//        8221_8AB8 go through the hooks in this file, which in "trace mode" record the arguments and, for
//        the packet, the 7 stack words it reads.
//     3. On entry to 8221_8AB8 (the last call, with memory as the loop saw it: replacement textures in
//        place, [r28+64] already 0), the native version is replayed in full as a dry run: it reads through a
//        shadow (its own writes and the snapshot of [r28+64]), writes nothing, calls nobody except
//        GetVisibleState (pure: it only writes its frame below r1, which is saved and restored afterwards),
//        and every call it would make is compared with the recorded one.
//     4. The call list, the arguments, the 9 words and, on return, r3/r1/lr (and r12, r9-r11 on the short
//        paths) must match. A difference turns the native version off for the session and writes
//        "[eview] DIFERENCIA".
//   The final state is always the original's. One thread at a time (if another one is checking, the
//   original runs at first and the native version afterwards). If the trace fills up (more than 1021 drawn
//   submeshes), that call does not count. "[eview]" line every 10 s: native calls, original calls, checked
//   calls and the average time of 1 in 16 native calls (with GetVisibleState and the packet included).

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_informe_diferido.h"  // deferred reports
#include <rex/platform.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

REXCVAR_DEFINE_BOOL(nfsmw_eview_nativo, true, "NFSMW",
                    "eViewPlatInterface::Render (sub_8243E358, el bucle de submallas de cada modelo) en nativo, "
                    "identico bit a bit (build 176). Se comprueba contra la original al empezar y 1 de cada 4096 "
                    "llamadas despues, y se apaga sola si difiere; false = la original")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// This file only names by their 8 digits the functions it hooks or that were already hooked
// (tools/llamadas_directas.py treats any 82xxxxxx address in app/src as hooked); the rest are split (8245_4428).
REX_EXTERN(__imp__sub_8243E358);  // original Render (the hook is in nfsmw_sombras_lod.cpp and calls Render() from here)
REX_EXTERN(__imp__sub_82452868);  // draw packet
REX_EXTERN(__imp__sub_82500010);
REX_EXTERN(__imp__sub_82218B18);
REX_EXTERN(__imp__sub_82218AB8);
REX_EXTERN(sub_82452868);  // hooks in this file (called by name, like the original)
REX_EXTERN(sub_82500010);
REX_EXTERN(sub_82218B18);
REX_EXTERN(sub_82218AB8);
REX_EXTERN(sub_8243E7D8);  // GetVisibleState: its hook is in nfsmw_d3d_registros_nativo.cpp

namespace nfsmw::eview {
void Render(PPCContext& ctx, uint8_t* base);

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Guest memory: the same translation as REX_LOAD / REX_STORE in nfsmw_pch.h.
// ---------------------------------------------------------------------------------------------------------------
inline uint32_t Desplazamiento(uint32_t direccion) {
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
  return direccion >= 0xE0000000u ? 0x1000u : 0u;
#else
  (void)direccion;
  return 0u;
#endif
}
inline uint8_t* Puntero(uint8_t* base, uint32_t direccion) {
  return base + direccion + Desplazamiento(direccion);
}
inline uint32_t Leer8(uint8_t* base, uint32_t direccion) {
  return *Puntero(base, direccion);
}
inline uint32_t Leer16(uint8_t* base, uint32_t direccion) {
  uint16_t v;
  std::memcpy(&v, Puntero(base, direccion), 2);
  return __builtin_bswap16(v);
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
// PRFM: no side effects and it never faults, even if the address is invalid.
inline void Anticipar(uint8_t* base, uint32_t direccion) {
  __builtin_prefetch(Puntero(base, direccion), 0, 3);
}
// Only for the PC test (measuring with and without prefetches): always 1 in the game.
#ifndef NFSMW_EVIEW_ANTICIPAR
#define NFSMW_EVIEW_ANTICIPAR 1
#endif

// ---------------------------------------------------------------------------------------------------------------
// Fixed addresses (PowerPC lis + offset; checked in the PC test) and return addresses of Render's calls (the lr
// it sets before each bl; the trace hooks use it to recognize their own call).
// ---------------------------------------------------------------------------------------------------------------
constexpr uint32_t kBanderaSalida = 0x82A2CFB7;  // lis r11,-32093; lbz r9,-12361(r11)
constexpr uint32_t kTablaRemapeo = 0x8293CC68;   // lis r10,-32108; addi r19,r10,-13208
constexpr uint32_t kClaveOmitir = 0x82A45274;    // lis r10,-32092; lwz r25,21108(r10)
constexpr uint32_t kMarco = 784;                 // stwu r1,-784(r1)
constexpr uint64_t kLrPrologo = 0x8243E360;      // bl 826b_dce8 (saves r16-r31: in the recompiled code it writes nothing)
constexpr uint64_t kLr500010 = 0x8243E3C4;
constexpr uint64_t kLrB18 = 0x8243E3DC;
constexpr uint64_t kLrVisible = 0x8243E48C;
constexpr uint64_t kLrPaquete = 0x8243E554;
constexpr uint64_t kLrAB8 = 0x8243E574;
constexpr uint64_t kR11Bandera = 0xFFFFFFFF82A30000ull;  // lis r11,-32093 (signed, in 64 bits)

struct Entrada {
  uint64_t r1, lr, r3, r4, r5, r6, r7, r8;
};
inline Entrada LeerEntrada(const PPCContext& ctx) {
  return {ctx.r1.u64, ctx.lr, ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64, ctx.r7.u64, ctx.r8.u64};
}
// The frame: stwu only changes the low part of r1 (ctx.r1.u32 = ea).
inline uint64_t Marco64(uint64_t r1) {
  return (r1 & 0xFFFFFFFF00000000ull) | uint32_t(uint32_t(r1) - kMarco);
}

// ---------------------------------------------------------------------------------------------------------------
// The body, a single one for the normal path and for the guard's replay. P is the policy: where it reads
// from, where it writes to, and what "calling" means. Line numbers of nfsmw_recomp.105.cpp in parentheses.
// ---------------------------------------------------------------------------------------------------------------
template <class P>
[[gnu::always_inline]] inline void Cuerpo(P& p, const Entrada& e) {
  const uint64_t marco64 = Marco64(e.r1);
  const uint32_t R1 = uint32_t(marco64);
  p.E32(R1, uint32_t(e.r1));  // stwu r1,-784(r1) (16409)
  p.Prologo(e, marco64);      // mflr r12; bl (lr = 0x8243E360); r1 = marco
  const uint32_t modelo = uint32_t(e.r4);
  const uint32_t r29 = p.L32(modelo + 12);  // lwz r29,12(r17)
  if (r29 == 0) {
    p.Epilogo(marco64);
    return;
  }
  const uint32_t banderas = p.L16(r29 + 14);  // lhz r11,14(r29)
  if ((banderas & 0x800u) != 0) {
    const uint32_t salir = p.L8(kBanderaSalida);
    if (salir != 0) {
      p.SalidaBandera(banderas, salir);
      p.Epilogo(marco64);
      return;
    }
  }
  const uint32_t r28 = p.L32(r29);            // lwz r28,0(r29)
  const uint32_t lista = p.L32(r28 + 20);     // lwz r8,20(r28); dcbt r0,r8
  p.AnticipoInicial(lista);
  p.Llamar500010(e.r4, r29, e.r5);            // (16460)
  if (p.L32(modelo + 16) != 0) {              // lwz r7,16(r17)
    p.LlamarB18(e.r4, marco64 + 128);         // (16473)
  }
  if (int32_t(p.L32(r28 + 16)) > 0) {         // lwz r6,16(r28); ble
    const uint32_t r16 = uint32_t(e.r7) & 4u;             // rlwinm r16,r22,0,29,29
    const uint32_t r25 = p.L32(kClaveOmitir);
    // Only for prefetching: the solid's texture table, once the original has already read it in this call (a
    // drawn submesh). Not touched before that: a solid that is not drawn could have it at 0.
    uint32_t tabla_texturas = 0;
    uint64_t r27 = 0;     // offset of the submesh (addi r27,r27,256)
    uint64_t indice = 0;  // ctx.r11 at the loop header
    for (;;) {
      const uint64_t r23 = indice + 1;                        // addi r23,r11,1 (16501)
      const int32_t cuantas = int32_t(p.L32(r28 + 16));        // lwz r5,16(r28)
      if (int32_t(uint32_t(r23)) < cuantas) {                 // dcbt r0,r4 of the next one (16512)
        p.AnticipoBucle(uint32_t(r27 + p.L32(r28 + 20)) + 256u, int64_t(int32_t(uint32_t(r23))) + 1 < cuantas,
                        tabla_texturas);
      }
      const uint32_t base20 = p.L32(r28 + 20);                // lwz r10,20(r28)
      const uint32_t remapeo = p.L32(r28 + 64);               // lwz r11,64(r28)
      const uint64_t r31 = r27 + base20;                      // add r31,r27,r10
      const uint32_t sub = uint32_t(r31);
      uint32_t r30 = p.L32(sub + 28);                         // lwz r30,28(r31)
      if (remapeo != 0) {
        const uint32_t v = p.L32((p.L16(r30 + 4) << 2) + remapeo);  // lhz r3,4(r30); rotlwi; lwzx r11,r10,r11
        if (int32_t(v) != -1) {
          r30 = p.L32((v << 2) + kTablaRemapeo);             // rlwinm r9,r11,2,0,29; lwzx r30,r9,r19
        }
        p.E32(r28 + 64, 0);                                  // stw r18,64(r28)
      }
      bool dibujar = true;
      if (r16 == 0 && (p.L32(sub + 36) & 1u) == 0) {          // lwz r8,36(r31); clrlwi r7,r8,31
        dibujar = int32_t(p.Visible(e.r3, r31, r31 + 12, e.r5)) != 0;  // (16566); cmpwi r3,0
      }
      if (dibujar) {
        const uint32_t b43 = p.L8(sub + 43);
        const uint32_t b44 = p.L8(sub + 44);
        const uint32_t tabla = p.L32(r29 + 44);
        const uint32_t b45 = p.L8(sub + 45);
        const uint32_t b46 = p.L8(sub + 46);
        const uint32_t b47 = p.L8(sub + 47);
        const uint32_t t0 = p.L32((b43 << 3) + tabla + 4);   // lwz r11,4(r6)
        const uint32_t t1 = p.L32((b44 << 3) + tabla + 4);   // lwz r8,4(r5)
        const uint32_t t2 = p.L32((b45 << 3) + tabla + 4);   // lwz r7,4(r4)
        const uint32_t t3 = p.L32((b46 << 3) + tabla + 4);   // lwz r6,4(r3)
        const uint32_t t4 = p.L32((b47 << 3) + tabla + 4);   // lwz r5,4(r10)
        const uint32_t clave = p.L32(t0 + 36);               // lwz r9,36(r11)
        tabla_texturas = tabla;
        p.E32(R1 + 96, t0);                                  // stw r11,96(r1) ... stw r5,112(r1) (16617-16627)
        p.E32(R1 + 100, t1);
        p.E32(R1 + 104, t2);
        p.E32(R1 + 108, t3);
        p.E32(R1 + 112, t4);
        if (clave != r25) {                                  // cmplw r9,r25; beq
          const uint32_t b42 = p.L8(sub + 42);                // lbz r10,42(r31)
          uint32_t luz = 0;                                   // mr r11,r18
          if (b42 != 255u) {
            const uint32_t t60 = p.L32(r29 + 60);             // lwz r9,60(r29)
            luz = p.L32((b42 << 3) + t60 + 4);                // lwz r11,4(r4)
          }
          p.E32(R1 + 92, uint32_t(e.r8));                     // stw r20,92(r1)
          p.E32(R1 + 84, luz);                                // stw r11,84(r1)
          p.Paquete(e.r3, r31, r29, e.r7, r30, marco64 + 96, e.r5, e.r6);  // (16669)
        }
      }
      const int32_t n = int32_t(p.L32(r28 + 16));            // lwz r3,16(r28) (16672)
      indice = r23;
      r27 += 256;
      if (!(int32_t(uint32_t(indice)) < n)) {
        break;
      }
    }
  }
  p.LlamarAB8(e.r4, marco64 + 128);  // (16688)
  p.Epilogo(marco64);
}

// ---------------------------------------------------------------------------------------------------------------
// Policy of the normal path: the real memory and the real calls.
// ---------------------------------------------------------------------------------------------------------------
struct Real {
  PPCContext& ctx;
  uint8_t* base;

  uint32_t L8(uint32_t d) const { return Leer8(base, d); }
  uint32_t L16(uint32_t d) const { return Leer16(base, d); }
  uint32_t L32(uint32_t d) const { return Leer32(base, d); }
  void E32(uint32_t d, uint32_t v) const { Escribir32(base, d, v); }

  void Prologo(const Entrada& e, uint64_t marco64) const {
    ctx.r12.u64 = e.lr;  // mflr r12
    ctx.lr = kLrPrologo;
    ctx.r1.u64 = marco64;
  }
  void Epilogo(uint64_t marco64) const {
    ctx.r1.u64 = marco64 + kMarco;  // addi r1,r1,784 (in 64 bits, like the original)
  }
  void SalidaBandera(uint32_t banderas, uint32_t salir) const {
    ctx.r11.u64 = kR11Bandera;
    ctx.r10.u64 = banderas & 0x800u;
    ctx.r9.u64 = salir;
  }

  // --- prefetches (only here; in the replay they do nothing) ---
  void AnticipoInicial(uint32_t lista) const {
    if constexpr (!NFSMW_EVIEW_ANTICIPAR) return;
    Anticipar(base, lista);
    Anticipar(base, lista + 63);
    Anticipar(base, lista + 256);
    Anticipar(base, lista + 256 + 63);
  }
  // sig = submesh i+1 (requested one iteration ago). Submesh i+2 is requested and, from i+1, its material and
  // its first texture: the two pointers that miss the most in the profile (the key [texture0+36] here, and
  // +81/+84 of the texture and +4/+6 of the material in the packet). Not the other four textures: they are
  // usually shared and prefetching them cost more than it saved with a warm cache (measured on PC).
  // Reads: +28 and +43 of submesh i+1, which exists (i+1 < n) and whose +28 the original always reads; and the
  // texture table only if the original has already read it (table != 0). The rest are PRFM, which never fault.
  void AnticipoBucle(uint32_t sig, bool hay_otra, uint32_t tabla) const {
    if constexpr (!NFSMW_EVIEW_ANTICIPAR) return;
    if (hay_otra) {
      Anticipar(base, sig + 256);
      Anticipar(base, sig + 256 + 63);
    }
    Anticipar(base, Leer32(base, sig + 28) + 4);
    if (tabla != 0) {
      const uint32_t t0 = Leer32(base, (Leer8(base, sig + 43) << 3) + tabla + 4);
      Anticipar(base, t0 + 36);
      Anticipar(base, t0 + 84);
    }
  }

  // --- calls: same argument registers, same lr and r1 = frame (already set in the prologue) ---
  void Llamar500010(uint64_t r3, uint64_t r4, uint64_t r5) const {
    ctx.r5.u64 = r5;
    ctx.r4.u64 = r4;
    ctx.r3.u64 = r3;
    ctx.lr = kLr500010;
    sub_82500010(ctx, base);
  }
  void LlamarB18(uint64_t r3, uint64_t r4) const {
    ctx.r4.u64 = r4;
    ctx.r3.u64 = r3;
    ctx.lr = kLrB18;
    sub_82218B18(ctx, base);
  }
  uint32_t Visible(uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6) const {
    ctx.r6.u64 = r6;
    ctx.r5.u64 = r5;
    ctx.r4.u64 = r4;
    ctx.r3.u64 = r3;
    ctx.lr = kLrVisible;
    sub_8243E7D8(ctx, base);
    return ctx.r3.u32;
  }
  void Paquete(uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6, uint64_t r7, uint64_t r8, uint64_t r9,
               uint64_t r10) const {
    ctx.r10.u64 = r10;
    ctx.r9.u64 = r9;
    ctx.r8.u64 = r8;
    ctx.r7.u64 = r7;
    ctx.r6.u64 = r6;
    ctx.r5.u64 = r5;
    ctx.r4.u64 = r4;
    ctx.r3.u64 = r3;
    ctx.lr = kLrPaquete;
    sub_82452868(ctx, base);
  }
  void LlamarAB8(uint64_t r3, uint64_t r4) const {
    ctx.r4.u64 = r4;
    ctx.r3.u64 = r3;
    ctx.lr = kLrAB8;
    sub_82218AB8(ctx, base);
  }
};

// ---------------------------------------------------------------------------------------------------------------
// Guard: trace of the original's calls and dry replay of the native version.
// ---------------------------------------------------------------------------------------------------------------
enum Tipo : uint8_t { k500010 = 0, kB18 = 1, kPaquete = 2, kAB8 = 3 };
constexpr const char* kNombreLlamada[] = {"8250_0010", "8221_8B18", "paquete 8245_2868", "8221_8AB8"};

struct Evento {
  uint8_t tipo;
  uint64_t r[8];       // r3..r10 (the ones not passed, at 0)
  uint32_t pila[7];    // packet: the words at r1+84, +92 and +96..112 that it reads
};

constexpr uint32_t kMaxEventos = 1024;  // 3 + one per drawn submesh
constexpr uint32_t kPalabras = 9;       // enlace, [r28+64] y r1+84..112
constexpr uint32_t kPilaMuerta = 2048;  // below r1: GetVisibleState's frame (128) with a wide margin

struct Sombra {
  uint32_t dir[kPalabras];
  uint32_t val[kPalabras];
  uint32_t n = 0;
  bool inesperada = false;  // the native version wrote outside the 9
  uint32_t dir_inesperada = 0;

  void Anadir(uint8_t* base, uint32_t d) {
    dir[n] = d;
    val[n] = Leer32(base, d);
    ++n;
  }
  // Read of n bytes (big-endian) with whatever is in the shadow on top of memory.
  uint32_t Leer(uint8_t* base, uint32_t d, uint32_t bytes) const {
    uint32_t v = 0;
    for (uint32_t b = 0; b < bytes; ++b) {
      const uint32_t x = d + b;
      uint32_t byte = *Puntero(base, x);
      for (uint32_t i = 0; i < n; ++i) {
        if (uint32_t(x - dir[i]) < 4u) {
          byte = (val[i] >> (8 * (3 - (x - dir[i])))) & 0xFFu;
        }
      }
      v = (v << 8) | byte;
    }
    return v;
  }
  void Escribir(uint32_t d, uint32_t v) {
    for (uint32_t i = 0; i < n; ++i) {
      if (dir[i] == d) {
        val[i] = v;
        return;
      }
    }
    if (!inesperada) {
      inesperada = true;
      dir_inesperada = d;
    }
  }
};

struct Traza {
  Entrada e;
  uint64_t marco64 = 0;
  Evento ev[kMaxEventos];
  uint32_t n = 0;
  bool lleno = false;
  bool repetida = false;       // the replay has already been done (in 8221_8AB8 or on return)
  const char* fallo = nullptr;  // first difference of the replay
  uint32_t donde = 0;          // event index
  uint32_t camino = 0;         // 1, 2 or 3 depending on the native version
  uint32_t banderas = 0;       // path 2: what the native version leaves in r9-r11
  uint32_t salir = 0;
  uint32_t memoria_original = 0;  // "memoria distinta": what the original left in that word
  Sombra s;
};

// Thread with a Render check in progress (its context) and its trace. One thread at a time: the trace, the
// replay context and the stack copy are global (no allocations and no extra ~5 KB on the host stack).
std::atomic<PPCContext*> g_traza_ctx{nullptr};
Traza g_traza;
PPCContext g_cv;
alignas(16) uint8_t g_pila[kPilaMuerta];

// Replay policy: reads through the shadow, writes nothing, compares each call with the trace.
struct Repeticion {
  uint8_t* base;
  Traza& t;
  PPCContext& cv;  // separate context to call GetVisibleState
  uint32_t k = 0;  // next event of the trace
  bool ab8 = false;
  uint64_t ab8_r3 = 0, ab8_r4 = 0;
  uint32_t camino = 3;

  uint32_t L8(uint32_t d) const { return t.s.Leer(base, d, 1); }
  uint32_t L16(uint32_t d) const { return t.s.Leer(base, d, 2); }
  uint32_t L32(uint32_t d) const { return t.s.Leer(base, d, 4); }
  void E32(uint32_t d, uint32_t v) { t.s.Escribir(d, v); }
  void Prologo(const Entrada&, uint64_t) {}
  void Epilogo(uint64_t) {}
  void SalidaBandera(uint32_t banderas, uint32_t salir) {
    camino = 2;
    t.banderas = banderas;
    t.salir = salir;
  }
  void AnticipoInicial(uint32_t) {}
  void AnticipoBucle(uint32_t, bool, uint32_t) {}

  void Fallo(const char* que) {
    if (!t.fallo) {
      t.fallo = que;
      t.donde = k;
    }
  }
  void Comparar(const Evento& x) {
    if (k >= t.n) {
      if (!t.lleno) {  // with the trace full, whatever is beyond it cannot be compared (not a difference)
        Fallo("la nativa llama de mas");
      }
      ++k;
      return;
    }
    const Evento& y = t.ev[k];
    if (y.tipo != x.tipo) {
      Fallo("otra funcion");
    } else if (std::memcmp(y.r, x.r, sizeof(x.r)) != 0) {
      Fallo("otros registros");
    } else if (std::memcmp(y.pila, x.pila, sizeof(x.pila)) != 0) {
      Fallo("otra pila");
    }
    ++k;
  }
  void Llamar500010(uint64_t r3, uint64_t r4, uint64_t r5) {
    Comparar(Evento{k500010, {r3, r4, r5, 0, 0, 0, 0, 0}, {}});
  }
  void LlamarB18(uint64_t r3, uint64_t r4) { Comparar(Evento{kB18, {r3, r4, 0, 0, 0, 0, 0, 0}, {}}); }
  uint32_t Visible(uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6) {
    cv.r1.u64 = t.marco64;
    cv.r6.u64 = r6;
    cv.r5.u64 = r5;
    cv.r4.u64 = r4;
    cv.r3.u64 = r3;
    cv.lr = kLrVisible;
    sub_8243E7D8(cv, base);
    return cv.r3.u32;
  }
  void Paquete(uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6, uint64_t r7, uint64_t r8, uint64_t r9,
               uint64_t r10) {
    Evento x{kPaquete, {r3, r4, r5, r6, r7, r8, r9, r10}, {}};
    const uint32_t R1 = uint32_t(t.marco64);
    x.pila[0] = L32(R1 + 84);
    x.pila[1] = L32(R1 + 92);
    for (uint32_t i = 0; i < 5; ++i) {
      x.pila[2 + i] = L32(R1 + 96 + 4 * i);
    }
    Comparar(x);
  }
  void LlamarAB8(uint64_t r3, uint64_t r4) {
    ab8 = true;
    ab8_r3 = r3;
    ab8_r4 = r4;
  }
};

// ---------------------------------------------------------------------------------------------------------------
// Counters, report and shutdown (no atomic read-modify-write on the normal path: A57 without LSE).
// ---------------------------------------------------------------------------------------------------------------
constexpr uint64_t kComprobacionesRender = 30000;  // about 1-2 s of racing; then 1 of every kPeriodo
constexpr uint64_t kPeriodo = 4096;

std::atomic<int8_t> g_activo{-1};
std::atomic<bool> g_apagado{false};
std::atomic<uint64_t> g_llamadas{0};
std::atomic<uint64_t> g_nativas{0};           // ultimos 10 s
std::atomic<uint64_t> g_originales{0};        // ultimos 10 s
std::atomic<uint64_t> g_comprobadas{0};       // ultimos 10 s
std::atomic<uint64_t> g_comprobadas_total{0};
std::atomic<uint64_t> g_sin_comprobar{0};     // trace full or another thread checking (not differences)
std::atomic<uint64_t> g_medidas{0};           // last 10 s: timed native calls (1 in 16)
std::atomic<uint64_t> g_medidas_ns{0};
std::atomic<int64_t> g_siguiente_ms{0};
std::atomic<int64_t> g_desde_ms{0};           // start of the report period

template <typename T>
inline void Sumar(std::atomic<T>& c, T n) {
  c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

inline bool Activo() {
  int8_t a = g_activo.load(std::memory_order_relaxed);
  if (a < 0) [[unlikely]] {
    a = REXCVAR_GET(nfsmw_eview_nativo) ? 1 : 0;
    g_activo.store(a, std::memory_order_relaxed);
  }
  return a != 0;
}

inline int64_t AhoraNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void Informe() {
  const int64_t ahora = AhoraNs() / 1000000;
  const int64_t siguiente = g_siguiente_ms.load(std::memory_order_relaxed);
  if (ahora < siguiente) {
    return;
  }
  g_siguiente_ms.store(ahora + 10000, std::memory_order_relaxed);
  const int64_t desde = g_desde_ms.exchange(ahora, std::memory_order_relaxed);
  if (siguiente == 0) {
    REXLOG_INFO("[eview] Render (8243E358) en nativo (build 176): se comprueban contra la original las primeras {} "
                "llamadas y despues 1 de cada {}",
                kComprobacionesRender, kPeriodo);
    g_medidas.store(0, std::memory_order_relaxed);
    g_medidas_ns.store(0, std::memory_order_relaxed);
    return;
  }
  const double segundos = double(ahora - desde) / 1000.0;
  const uint64_t nativas = g_nativas.exchange(0, std::memory_order_relaxed);
  const uint64_t medidas = g_medidas.exchange(0, std::memory_order_relaxed);
  const uint64_t ns = g_medidas_ns.exchange(0, std::memory_order_relaxed);
  const double us = medidas ? double(ns) / double(medidas) / 1000.0 : 0.0;
  NFSMW_INFORME_DIFERIDO("[eview] ultimos {:.1f} s: Render {} nativas ({:.2f} us por llamada con GetVisibleState y el paquete "
              "dentro = {:.1f} ms/s), {} originales, {} comprobadas{}; comprobadas desde el arranque {} ({} sin poder "
              "comprobar)",
              segundos, nativas, us, segundos > 0 ? us * double(nativas) / 1000.0 / segundos : 0.0,
              g_originales.exchange(0, std::memory_order_relaxed), g_comprobadas.exchange(0, std::memory_order_relaxed),
              g_apagado.load(std::memory_order_relaxed) ? " | APAGADA por diferencia" : "",
              g_comprobadas_total.load(std::memory_order_relaxed), g_sin_comprobar.load(std::memory_order_relaxed));
}

// ---------------------------------------------------------------------------------------------------------------
// The normal path.
// ---------------------------------------------------------------------------------------------------------------
[[gnu::always_inline]] inline void RenderNativo(PPCContext& ctx, uint8_t* base) {
  Real p{ctx, base};
  const Entrada e = LeerEntrada(ctx);
  Cuerpo(p, e);
}

// ---------------------------------------------------------------------------------------------------------------
// The dry replay (in 8221_8AB8 or, if the original does not get there, on return). Leaves memory and context as
// they were (except the host FPCR, which is restored to the context's).
// ---------------------------------------------------------------------------------------------------------------
[[gnu::noinline]] void Repetir(PPCContext& ctx, uint8_t* base, bool en_ab8) {
  Traza& t = g_traza;
  t.repetida = true;
  const uint32_t R1 = uint32_t(t.marco64);
  // GetVisibleState writes its frame below r1: it is saved and restored (dead stack, but memory stays identical).
  const uint32_t bajo = R1 - kPilaMuerta;
  const bool pila_ok = Desplazamiento(bajo) == Desplazamiento(R1 - 1);
  if (pila_ok) {
    std::memcpy(g_pila, Puntero(base, bajo), kPilaMuerta);
  }
  g_cv = ctx;  // separate context for GetVisibleState: the real one is not touched
  Repeticion p{base, t, g_cv};
  Cuerpo(p, t.e);
  if (pila_ok) {
    std::memcpy(Puntero(base, bajo), g_pila, kPilaMuerta);
  }
  ctx.fpscr.setcsr(ctx.fpscr.csr);  // GetVisibleState (in g_cv) may have changed the denormal mode
  t.camino = p.ab8 ? 3u : p.camino == 2 ? 2u : 1u;

  if (!t.fallo && p.k != t.n && !t.lleno) {
    p.Fallo("la original llama a mas funciones");
  }
  if (!t.fallo) {
    if (en_ab8 != p.ab8) {
      t.fallo = en_ab8 ? "la nativa no llega a 8221_8AB8" : "la nativa llama a 8221_8AB8 y la original no";
    } else if (en_ab8 && (p.ab8_r3 != ctx.r3.u64 || p.ab8_r4 != ctx.r4.u64)) {
      t.fallo = "8221_8AB8 con otros registros";
    } else if (t.s.inesperada) {
      t.fallo = "escritura fuera de las 9 palabras";
    } else {
      for (uint32_t i = 0; i < t.s.n; ++i) {
        if (Leer32(base, t.s.dir[i]) != t.s.val[i]) {
          t.fallo = "memoria distinta";
          t.donde = i;
          t.memoria_original = Leer32(base, t.s.dir[i]);
          break;
        }
      }
    }
  }
}

void Apagar(const PPCContext& ctx, uint64_t n, const char* que, bool de_la_traza) {
  g_apagado.store(true, std::memory_order_relaxed);
  const Traza& t = g_traza;
  std::string detalle;
  if (!de_la_traza) {
    detalle = fmt::format("; al volver: r1 0x{:X} r3 0x{:X} lr 0x{:X} r12 0x{:X}", ctx.r1.u64, ctx.r3.u64, ctx.lr,
                          ctx.r12.u64);
  } else if (std::strcmp(que, "memoria distinta") == 0 && t.donde < t.s.n) {
    detalle = fmt::format("; palabra 0x{:08X}: nativa 0x{:08X}, original 0x{:08X}", t.s.dir[t.donde],
                          t.s.val[t.donde], t.memoria_original);
  } else if (t.donde < t.n) {
    const Evento& y = t.ev[t.donde];
    detalle = fmt::format("; llamada {} de la original: {} r3 0x{:X} r4 0x{:X} r5 0x{:X} r6 0x{:X} r7 0x{:X}",
                          t.donde, kNombreLlamada[y.tipo], y.r[0], y.r[1], y.r[2], y.r[3], y.r[4]);
  } else if (t.n == 0 && t.camino == 3) {
    detalle = "; la traza esta VACIA: las llamadas de la Render original no pasan por los ganchos de este fichero "
              "(falta revertir_llamadas_eview.py en app/generated/default?)";
  }
  REXLOG_INFO("[eview] DIFERENCIA en Render ({}; llamada {}, camino {}): vista 0x{:08X} modelo 0x{:08X} matriz "
              "0x{:08X} r1 0x{:08X}; la original hizo {} llamadas{}. Camino nativo APAGADO para el resto de la "
              "sesion, se queda la original",
              que, n, t.camino, uint32_t(t.e.r3), uint32_t(t.e.r4), uint32_t(t.e.r5), uint32_t(t.e.r1), t.n, detalle);
}

// Render guard. Always leaves the original's state.
[[gnu::noinline]] void ComprobarRender(PPCContext& ctx, uint8_t* base, uint64_t n) {
  PPCContext* libre = nullptr;
  if (!g_traza_ctx.compare_exchange_strong(libre, &ctx, std::memory_order_acquire, std::memory_order_relaxed)) {
    // Another thread is checking: the original at first; afterwards, the already checked native version.
    Sumar(g_sin_comprobar, uint64_t(1));
    if (n <= kComprobacionesRender) {
      Sumar(g_originales, uint64_t(1));
      __imp__sub_8243E358(ctx, base);
    } else {
      RenderNativo(ctx, base);
      Sumar(g_nativas, uint64_t(1));
    }
    return;
  }
  Traza& t = g_traza;
  t.e = LeerEntrada(ctx);
  t.marco64 = Marco64(t.e.r1);
  t.n = 0;
  t.lleno = false;
  t.repetida = false;
  t.fallo = nullptr;
  t.donde = 0;
  t.camino = 0;
  // 1. Snapshot of the words Render can write. [r28+64] only if the original is going to walk the mesh (the
  //    same reads it will do before its first call).
  const uint32_t R1 = uint32_t(t.marco64);
  t.s.n = 0;
  t.s.inesperada = false;
  t.s.Anadir(base, R1);
  const uint32_t r29 = Leer32(base, uint32_t(t.e.r4) + 12);
  if (r29 != 0 && ((Leer16(base, r29 + 14) & 0x800u) == 0 || Leer8(base, kBanderaSalida) == 0)) {
    t.s.Anadir(base, Leer32(base, r29) + 64);
  }
  t.s.Anadir(base, R1 + 84);
  t.s.Anadir(base, R1 + 92);
  for (uint32_t i = 0; i < 5; ++i) {
    t.s.Anadir(base, R1 + 96 + 4 * i);
  }
  // 2. The original, for real, with its calls in trace mode (3. the replay happens inside 8221_8AB8).
  __imp__sub_8243E358(ctx, base);
  if (!t.repetida) {
    Repetir(ctx, base, false);  // paths 1 and 2: the original did not reach 8221_8AB8
  }
  g_traza_ctx.store(nullptr, std::memory_order_release);
  // 4. Registros al volver.
  const char* que = t.fallo;
  if (!que) {
    const uint64_t marco64 = t.marco64;
    if (ctx.r1.u64 != marco64 + kMarco) {
      que = "r1 distinto";
    } else if (t.camino == 3) {
      if (ctx.r3.u64 != t.e.r4) que = "r3 distinto";
      else if (ctx.lr != kLrAB8) que = "lr distinto";
    } else {
      if (ctx.r3.u64 != t.e.r3) que = "r3 distinto";
      else if (ctx.lr != kLrPrologo || ctx.r12.u64 != t.e.lr) que = "r12/lr distintos";
      else if (t.camino == 2 && (ctx.r11.u64 != kR11Bandera || ctx.r10.u64 != (t.banderas & 0x800u) ||
                                 ctx.r9.u64 != t.salir)) que = "r9-r11 distintos";
    }
  }
  if (t.lleno && !que) {
    Sumar(g_sin_comprobar, uint64_t(1));  // too many submeshes for the trace: not counted as checked
    return;
  }
  if (que) {
    Apagar(ctx, n, que, que == t.fallo);
    return;
  }
  Sumar(g_comprobadas, uint64_t(1));
  const uint64_t total = g_comprobadas_total.load(std::memory_order_relaxed) + 1;
  g_comprobadas_total.store(total, std::memory_order_relaxed);
  if (total == kComprobacionesRender) {
    REXLOG_INFO("[eview] Render: {} llamadas comprobadas contra la original (llamadas, argumentos, pila y registros), "
                "0 diferencias: camino nativo en marcha, y sigue comprobando 1 de cada {}",
                total, kPeriodo);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Trace hooks: on the normal path, one relaxed atomic read and the real function.
// ---------------------------------------------------------------------------------------------------------------
inline bool EnTraza(const PPCContext& ctx, uint64_t lr) {
  return g_traza_ctx.load(std::memory_order_relaxed) == &ctx && ctx.lr == lr && ctx.r1.u64 == g_traza.marco64 &&
         !g_traza.repetida;
}
inline void Apuntar(const Evento& x) {
  Traza& t = g_traza;
  if (t.n >= kMaxEventos) {
    t.lleno = true;
    return;
  }
  t.ev[t.n++] = x;
}
[[gnu::noinline]] void TrazarPaquete(PPCContext& ctx, uint8_t* base) {
  Evento x{kPaquete,
           {ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64, ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64},
           {}};
  const uint32_t R1 = ctx.r1.u32;
  x.pila[0] = Leer32(base, R1 + 84);
  x.pila[1] = Leer32(base, R1 + 92);
  for (uint32_t i = 0; i < 5; ++i) {
    x.pila[2 + i] = Leer32(base, R1 + 96 + 4 * i);
  }
  Apuntar(x);
}

}  // namespace

// The entry point of Render: the sub_8243E358 hook in nfsmw_sombras_lod.cpp calls it instead of the original.
void Render(PPCContext& ctx, uint8_t* base) {
  if (!Activo() || g_apagado.load(std::memory_order_relaxed)) {
    __imp__sub_8243E358(ctx, base);
    return;
  }
  const uint64_t n = g_llamadas.load(std::memory_order_relaxed) + 1;
  g_llamadas.store(n, std::memory_order_relaxed);
  if (n <= kComprobacionesRender || (n & (kPeriodo - 1)) == 0) [[unlikely]] {
    ComprobarRender(ctx, base, n);
  } else if ((n & 15) == 0) [[unlikely]] {
    const int64_t t0 = AhoraNs();
    RenderNativo(ctx, base);
    Sumar(g_medidas_ns, uint64_t(AhoraNs() - t0));
    Sumar(g_medidas, uint64_t(1));
    Sumar(g_nativas, uint64_t(1));
  } else {
    RenderNativo(ctx, base);
    Sumar(g_nativas, uint64_t(1));
  }
  if ((n & (kPeriodo - 1)) == 0) [[unlikely]] {
    Informe();
  }
}

}  // namespace nfsmw::eview

// Render's calls to these four functions must go to sub_X and not to __imp__sub_X (tools/llamadas_directas.py
// leaves them as sub_X because they are hooked here). Only Render calls them: the hooks change nothing for anyone
// else.
REX_HOOK_RAW(sub_82452868) {  // the draw packet of each submesh
  using namespace nfsmw::eview;
  if (EnTraza(ctx, kLrPaquete)) [[unlikely]] {
    TrazarPaquete(ctx, base);
  }
  __imp__sub_82452868(ctx, base);
}
REX_HOOK_RAW(sub_82500010) {
  using namespace nfsmw::eview;
  if (EnTraza(ctx, kLr500010)) [[unlikely]] {
    Apuntar(Evento{k500010, {ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, 0, 0, 0, 0, 0}, {}});
  }
  __imp__sub_82500010(ctx, base);
}
REX_HOOK_RAW(sub_82218B18) {  // puts the model's replacement textures in place
  using namespace nfsmw::eview;
  if (EnTraza(ctx, kLrB18)) [[unlikely]] {
    Apuntar(Evento{kB18, {ctx.r3.u64, ctx.r4.u64, 0, 0, 0, 0, 0, 0}, {}});
  }
  __imp__sub_82218B18(ctx, base);
}
REX_HOOK_RAW(sub_82218AB8) {  // removes them: Render's last call, where the native version is replayed
  using namespace nfsmw::eview;
  if (EnTraza(ctx, kLrAB8)) [[unlikely]] {
    Repetir(ctx, base, true);
  }
  __imp__sub_82218AB8(ctx, base);
}
