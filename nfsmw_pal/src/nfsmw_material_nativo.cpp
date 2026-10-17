// nfsmw - per-draw material parameters, in native code.
//
// WHAT IT IS
//   sub_824511E8 prepares on the game thread, for every draw with a material, up to 11 effect parameters
//   (constant floats, two 16-bit integers converted to float, two integers converted to float, an integer and
//   a vector) by calling the parameter "writers" of the game's D3DX:
//     8244F2E0  32-bit integer                (this, &handle, value)           stwx
//     82449360  float                         (effect, handle, &value)         lvlx + stvewx
//     82449618  integer converted to float    (effect, handle, &value)         lvlx + vcfsx + stvewx
//     82449988  16-byte vector                (effect, handle, &vector)        lvx128 + stvx128
//     82449C00  4x4 matrix (the rows given by the entry, transposed)            4 x lvx128, vsel, 4 x stvx
//   They all do the same with the parameter's handle: bit 0 = group (0 the effect's, 1 the shared one),
//   bits 1-17 = "dirty" bit they set in the group's mask, bits 18-31 = 8-byte entry in the group's table;
//   the second word of the entry (& 0xFFFF) is the target constant register (16 bytes).
//     effect group (even handle):      dirty = effect,        table = [effect+264],   dest = [effect+296]
//     shared group (odd handle):       dirty = [effect+256],  table = [[effect+268]], dest = [[effect+300]]
//
// WHY NATIVE
//   Stack sampling on the console: 824511E8 with the writers inlined by LTO weighs 3-4 % of a core of the
//   game thread, and 82449C00 another 1.4-2.8 % from other callers. Recompiled, each writer loads the fields
//   of both groups and picks with masks, every PowerPC register ends up stored in the context, the value
//   goes through ctx.v0 in memory and 824511E8 writes the FPCR (msr fpcr) up to four times per call when
//   switching between scalar and vector mode and back. Here: only the chosen group, no context except what
//   someone reads afterwards and no FPCR change halfway through.
//
// WHY IT IS BIT-IDENTICAL (read instruction by instruction in nfsmw_recomp.95/120/12/128/43/96)
//   - Only integers and bit copies are involved. The "lfs + stfs" of constants in 824511E8 are a copy of
//     the word: GCC folds float->double->float (checked with devkitA64 16.1 -O3 and with the disassembly:
//     the recompiled code stores the word it read as is). The "extsh, std, lfd, fcfid, frsp, stfs" of the
//     16-bit integers is exact in any rounding mode. vcfsx uses the same simde conversion here as the
//     recompiled code, with the same FPCR (flush mode does not affect an integer to float conversion).
//   - The same memory reads and writes in the same relative order, including the stack writes the
//     original makes and nobody reads afterwards (824511E8's prologue, the value slot at r1+80, the scratch
//     area of 82449360 and 82449618 at r1-80..r1-68): guest memory ends up identical byte for byte, stack
//     included. This is not a whim: a later lvlx reads 16 bytes and an stvewx to a destination not aligned
//     to 16 would store those leftovers.
//   - lvlx/stvewx are done with the exact semantics of the recompiled code (VectorMaskL): word
//     j = (destination >> 2) & 3 of the source's 16-byte block, with zeros past the end of the block. With
//     destinations aligned to 16 (expected for constant registers) j = 0, but the other cases are exact too
//     (PC test).
//   - Registers: interprocedural liveness analysis of all the generated code: after the 179 calls to these
//     six functions nobody reads a volatile register that the original leaves different, except r3 when
//     the call is followed by the caller's return. r3 is left as the original does (the old dirty byte in
//     8244F2E0/82449988, the bit mask in 82449C00; in 824511E8 that of its last step), r12 and lr as
//     824511E8's epilogue leaves them, and the FPCR flush mode as the original (on after 82449618, off after
//     824511E8). cr, ctr and xer are local variables in the generated code.
//
// SELF-CHECKING GUARD (cvar nfsmw_material_nativo; project rule)
//   The first kComprobaciones calls of each function, and then 1 of every 4096, are checked against the
//   original and keep the original's result:
//   - Writers: the native version records each write (address and previous bytes), it is undone, the
//     original runs and what was written, r3 and the FPCR are compared byte by byte. Then what was written
//     is poisoned (bytes inverted; for the dirty byte, the bit cleared) and the original is run again (its
//     writes are idempotent): it must leave everything the same again. That proves it writes to exactly the
//     same addresses and, since each original writer does a fixed number of writes without branches, to no
//     other.
//   - 824511E8: the native version records writes and the list of calls to writers (type, r3, r4, r5 and
//     the 16 bytes of the value). It is undone and the original runs, whose calls go through the writers'
//     hooks in "trace mode": they record the call and do their full check against the original writer.
//     The list, the written bytes and r3/r12/lr/r1/FPCR must match.
//   A single difference turns the function off for good (and 824511E8 if one of its writers fails), leaves
//   the exact state of the original and writes "[material] DIFERENCIA" with the data. "[material]" line
//   every 10 s.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_informe_diferido.h"  // deferred reports
#include <rex/platform.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

REXCVAR_DEFINE_BOOL(nfsmw_material_nativo, true, "NFSMW",
                    "Parametros de material por dibujo (sub_824511E8 y las escritoras 8244F2E0, 82449360, 82449618, "
                    "82449988 y 82449C00) en nativo, identico bit a bit. Se comprueba contra la original al empezar y "
                    "1 de cada 4096 llamadas despues, y se apaga sola si difiere")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_824511E8);
REX_EXTERN(__imp__sub_8244F2E0);
REX_EXTERN(__imp__sub_82449360);
REX_EXTERN(__imp__sub_82449618);
REX_EXTERN(__imp__sub_82449988);
REX_EXTERN(__imp__sub_82449C00);

namespace nfsmw::material {
namespace {

// ---------------------------------------------------------------------------------------------------------------
// Guest memory: the same translation as REX_RAW_ADDR / REX_LOAD / REX_STORE in nfsmw_pch.h (the offset is
// computed from the address the macro receives, also for the aligned 16-byte accesses).
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
inline uint8_t Leer8(uint8_t* base, uint32_t direccion) {
  return *Puntero(base, direccion);
}
inline uint16_t Leer16(uint8_t* base, uint32_t direccion) {
  uint16_t v;
  std::memcpy(&v, Puntero(base, direccion), 2);
  return __builtin_bswap16(v);
}
inline uint32_t Leer32(uint8_t* base, uint32_t direccion) {
  uint32_t v;
  std::memcpy(&v, Puntero(base, direccion), 4);
  return __builtin_bswap32(v);
}
// 16 bytes from an aligned address, in memory order (lvx128 and lvlx read the whole block at once).
inline void LeerBloque(uint8_t* base, uint32_t alineada, uint8_t* salida) {
  std::memcpy(salida, Puntero(base, alineada), 16);
}

// ---------------------------------------------------------------------------------------------------------------
// Write with or without recording. Without recording (normal path) it costs nothing; when recording (guard) it
// saves address, size and previous bytes to undo, and the class for poisoning: "pura" is overwritten entirely,
// "or" is the dirty byte (read-modify-write with an OR of the bit).
// ---------------------------------------------------------------------------------------------------------------
enum Clase : uint8_t { kPura = 0, kOr = 1 };

struct Anotacion {
  uint32_t direccion;
  uint8_t bytes;
  uint8_t clase;
  uint8_t mascara;  // kOr: the bit the native version sets
  uint8_t antes[16];
  uint8_t despues[16];  // what the native version left when it finished
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

  inline void Anotar(uint32_t direccion, uint32_t bytes, uint8_t clase, uint8_t mascara) {
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
  inline void Escribir32(uint32_t direccion, uint32_t valor) {
    Anotar(direccion, 4, kPura, 0);
    valor = __builtin_bswap32(valor);
    std::memcpy(Puntero(base, direccion), &valor, 4);
  }
  inline void Escribir64(uint32_t direccion, uint64_t valor) {
    Anotar(direccion, 8, kPura, 0);
    valor = __builtin_bswap64(valor);
    std::memcpy(Puntero(base, direccion), &valor, 8);
  }
  inline void Escribir16B(uint32_t alineada, const uint8_t* bytes) {
    Anotar(alineada, 16, kPura, 0);
    std::memcpy(Puntero(base, alineada), bytes, 16);
  }
  // stbx of the dirty byte: the original writes (old | bit) with the old value it read before.
  inline void MarcarSucio(uint32_t direccion, uint8_t viejo, uint8_t bit) {
    Anotar(direccion, 1, kOr, bit);
    *Puntero(base, direccion) = uint8_t(viejo | bit);
  }
};

// ---------------------------------------------------------------------------------------------------------------
// Fixed addresses (PowerPC lis + offset; checked with the PC test).
// ---------------------------------------------------------------------------------------------------------------
constexpr uint32_t kTablaBits = 0x8290DB68;      // lis r8,-32111; addi r8,r8,-9368: the mask of each bit (8 bytes)
constexpr uint32_t kTablaFilas = 0x8208F7C0;     // lis r10,-32247; addi r7,r10,-2112: row masks of 82449C00
constexpr uint32_t kModo = 0x82A2CFB8;           // lwz r11,-12360(0x82A30000)
constexpr uint32_t kPartida = 0x82A39AD8;        // lis r11,-32092; addi r11,r11,-25928; lwz r11,32(r11)
constexpr uint32_t kBandera = 0x82A2D1A4;        // lbz r6,-11868(0x82A30000)
constexpr uint32_t kValor1 = 0x82A2CEE4;         // lwz r11,-12572(0x82A30000)
constexpr uint32_t kTablaValores = 0x82A15370;   // lis r11,-32095; addi r11,r11,21360
constexpr uint32_t kIndiceValor = 0x82A2D1A0;    // lwz r10,-11872(0x82A30000)
constexpr uint32_t kConst444 = 0x828FCC54;       // lfs f0,-13228(0x82900000)
constexpr uint32_t kConst480 = 0x8205E240;       // lfs f0,-7616(0x82060000)
constexpr uint32_t kConst440 = 0x82062864;       // lfs f0,10340(0x82060000)
constexpr uint32_t kConst460 = 0x828FCC78;       // lfs f31,-13192(0x82900000), for +460 and +464
constexpr uint32_t kPunteroVector = 0x82A2C4F8;  // lwz r11,-15112(0x82A30000); addi r5,r11,48
constexpr uint32_t kPunteroEnteros = 0x82A2D174; // lwz r11,-11916(0x82A30000); lhz 68 y 70
constexpr uint32_t kByte448 = 0x82A2D1A5;        // lbz r9,-11867(0x82A30000)

// ---------------------------------------------------------------------------------------------------------------
// The parameter's handle. m = (h & 1) - 1 in the original: both groups are always loaded and one is chosen with
// and/andc; here only the fields of the chosen group are loaded (reading has no side effects).
// ---------------------------------------------------------------------------------------------------------------
struct Grupo {
  uint32_t sucios;   // the group's dirty mask
  uint32_t tabla;    // table of 8-byte entries
  uint32_t destino;  // the group's constant registers
};
inline Grupo LeerGrupo(uint8_t* base, uint32_t efecto, uint32_t mango) {
  if ((mango & 1u) == 0) {
    return {efecto, Leer32(base, efecto + 264), Leer32(base, efecto + 296)};
  }
  return {Leer32(base, efecto + 256), Leer32(base, Leer32(base, efecto + 268)),
          Leer32(base, Leer32(base, efecto + 300))};
}
inline uint32_t Indice(uint32_t mango) {      // rlwinm r10,r4,31,15,31
  return (mango >> 1) & 0x1FFFFu;
}
inline uint32_t Entrada(uint32_t mango) {     // rlwinm rX,r4,17,15,28
  return (mango >> 15) & 0x1FFF8u;
}
inline uint32_t Registro16(uint32_t palabra) {  // rlwinm rX,rY,4,12,27
  return (palabra & 0xFFFFu) << 4;
}

// Word j (big-endian) of the vector that "lvlx vD,0,p" leaves: byte k = memory[(p & ~0xF) + s + k] if
// s + k < 16, otherwise 0 (s = p & 0xF). That is what the recompiled code does with VectorMaskL; stvewx stores
// word (ea & 0xF) >> 2.
inline uint32_t PalabraLvlx(const uint8_t* bloque, uint32_t s, uint32_t j) {
  const uint32_t k = s + 4 * j;
  if (k + 4 <= 16) {
    uint32_t v;
    std::memcpy(&v, bloque + k, 4);
    return __builtin_bswap32(v);
  }
  uint32_t v = 0;
  for (uint32_t b = 0; b < 4; ++b) {
    v = (v << 8) | (k + b < 16 ? bloque[k + b] : 0u);
  }
  return v;
}

// vcfsx vD,vS,0 of the recompiled code (simde_mm_cvtepi32_ps), for one word.
inline uint32_t FloatDeEntero(uint32_t entero) {
  const float f = simde_mm_cvtss_f32(simde_mm_cvtepi32_ps(simde_mm_set1_epi32(int32_t(entero))));
  return std::bit_cast<uint32_t>(f);
}

// ---------------------------------------------------------------------------------------------------------------
// The writers. Same relative order of reads and writes as the recompiled code (line numbers of the generated
// .cpp in parentheses).
// ---------------------------------------------------------------------------------------------------------------

// 8244F2E0 (nfsmw_recomp.120.cpp:16033): r3 = this, r4 = &mango (the handle), r5 = valor. Leaves r3 = the old dirty byte.
template <bool A>
inline uint32_t EscritoraF2E0(Memoria<A>& m, uint32_t self, uint32_t puntero_mango, uint32_t valor) {
  uint8_t* const base = m.base;
  const uint32_t mango = Leer32(base, puntero_mango);
  const uint32_t efecto = Leer32(base, self + 28);
  const Grupo g = LeerGrupo(base, efecto, mango);
  const uint32_t indice = Indice(mango);
  const uint8_t bit = Leer8(base, kTablaBits + (indice & 7u));
  const uint32_t sucio = g.sucios + (indice >> 3);
  const uint8_t viejo = Leer8(base, sucio);                                  // lbzx r3,r9,r11
  const uint32_t palabra = Leer32(base, g.tabla + Entrada(mango) + 4);       // lwz r4,4(r6), before the stbx
  m.MarcarSucio(sucio, viejo, bit);                                          // stbx r7,r9,r11
  m.Escribir32(Registro16(palabra) + g.destino, valor);                      // stwx r5,r10,r8 (sin alinear)
  return viejo;
}

// 82449360 (nfsmw_recomp.12.cpp:16845): r3 = effect, r4 = handle, r5 = &float. r3 does not change.
template <bool A>
inline void Escritora9360(Memoria<A>& m, uint32_t efecto, uint32_t mango, uint32_t p, uint32_t r1) {
  uint8_t* const base = m.base;
  const uint32_t indice = Indice(mango);
  const Grupo g = LeerGrupo(base, efecto, mango);
  m.Escribir32(r1 - 80, indice);                                             // stw r10,-80(r1)
  const uint8_t bit = Leer8(base, kTablaBits + (indice & 7u));
  m.Escribir32(r1 - 76, g.sucios);                                           // stw r10,-76(r1)
  const uint32_t sucio = g.sucios + (indice >> 3);
  const uint8_t viejo = Leer8(base, sucio);                                  // lbzx r7,r9,r10
  const uint32_t entrada = g.tabla + Entrada(mango);
  m.Escribir32(r1 - 72, entrada);                                            // stw r11,-72(r1)
  m.Escribir32(r1 - 68, g.destino);                                          // stw r8,-68(r1)
  uint8_t bloque[16];
  LeerBloque(base, p & ~0xFu, bloque);                                       // lvlx v0,0,r5
  m.MarcarSucio(sucio, viejo, bit);                                          // stbx r6,r9,r10
  const uint32_t palabra = Leer32(base, entrada + 4);                        // lwz r5,4(r11), after the stbx
  const uint32_t ea = (Registro16(palabra) + g.destino) & ~3u;
  m.Escribir32(ea, PalabraLvlx(bloque, p & 0xFu, (ea & 0xFu) >> 2));        // stvewx v0,r0,r4
}

// 82449618 (nfsmw_recomp.128.cpp:16693): like 82449360 but the value is an integer converted to float (vcfsx)
// and the stack scratch area goes in a different order. r3 does not change; the original leaves flush mode on.
template <bool A>
inline void Escritora9618(Memoria<A>& m, uint32_t efecto, uint32_t mango, uint32_t p, uint32_t r1) {
  uint8_t* const base = m.base;
  const uint32_t indice = Indice(mango);
  const Grupo g = LeerGrupo(base, efecto, mango);
  m.Escribir32(r1 - 80, indice);                                             // stw r10,-80(r1)
  const uint8_t bit = Leer8(base, kTablaBits + (indice & 7u));
  m.Escribir32(r1 - 72, g.destino);                                          // stw r8,-72(r1)
  const uint32_t sucio = g.sucios + (indice >> 3);
  const uint8_t viejo = Leer8(base, sucio);                                  // lbzx r7,r9,r11
  const uint32_t entrada = g.tabla + Entrada(mango);
  m.Escribir32(r1 - 68, g.sucios);                                           // stw r11,-68(r1)
  m.Escribir32(r1 - 76, entrada);                                            // stw r10,-76(r1)
  uint8_t bloque[16];
  LeerBloque(base, p & ~0xFu, bloque);                                       // lvlx v0,0,r5 + vcfsx v0,v0,0
  m.MarcarSucio(sucio, viejo, bit);                                          // stbx r6,r9,r11
  const uint32_t palabra = Leer32(base, entrada + 4);                        // lwz r5,4(r10), after the stbx
  const uint32_t ea = (Registro16(palabra) + g.destino) & ~3u;
  m.Escribir32(ea, FloatDeEntero(PalabraLvlx(bloque, p & 0xFu, (ea & 0xFu) >> 2)));  // stvewx v0,r0,r4
}

// 82449988 (nfsmw_recomp.43.cpp:16541): r3 = effect, r4 = handle, r5 = &vector. Aligned 16-byte copy.
// Leaves r3 = old dirty byte.
template <bool A>
inline uint32_t Escritora9988(Memoria<A>& m, uint32_t efecto, uint32_t mango, uint32_t p) {
  uint8_t* const base = m.base;
  uint8_t bloque[16];
  LeerBloque(base, p & ~0xFu, bloque);                                       // lvx128 v0,r0,r5 (the first thing)
  const Grupo g = LeerGrupo(base, efecto, mango);
  const uint32_t indice = Indice(mango);
  const uint8_t bit = Leer8(base, kTablaBits + (indice & 7u));
  const uint32_t sucio = g.sucios + (indice >> 3);
  const uint8_t viejo = Leer8(base, sucio);                                  // lbzx r3,r9,r10
  m.MarcarSucio(sucio, viejo, bit);                                          // stbx r7,r9,r10
  const uint32_t palabra = Leer32(base, g.tabla + Entrada(mango) + 4);       // lwz r6,4(r8), after the stbx
  m.Escribir16B((Registro16(palabra) + g.destino) & ~0xFu, bloque);          // stvx128 v0,r5,r11
  return viejo;
}

// 82449C00 (nfsmw_recomp.96.cpp:16517): r3 = effect, r4 = handle, r5 = &matrix (4 rows of 16 bytes). Each
// output row i is (row_i & ~M) | (column_i & M), with M the row mask chosen by the first word of the entry.
// These are bitwise operations on whole words: the byte order within each word does not matter.
// Leaves r3 = the mask of the dirty bit.
template <bool A>
inline uint32_t EscritoraC00(Memoria<A>& m, uint32_t efecto, uint32_t mango, uint32_t p) {
  uint8_t* const base = m.base;
  uint32_t fila[4][4];
  for (uint32_t i = 0; i < 4; ++i) {  // lvx128 v13/v12/v11/v10 of p, p+16, p+32 and p+48, each aligned
    LeerBloque(base, (p + 16 * i) & ~0xFu, reinterpret_cast<uint8_t*>(fila[i]));
  }
  const Grupo g = LeerGrupo(base, efecto, mango);
  const uint32_t entrada = g.tabla + Entrada(mango);
  const uint32_t palabra0 = Leer32(base, entrada);                           // lwz r4,0(r9)
  const uint32_t palabra1 = Leer32(base, entrada + 4);                       // lwz r4,4(r9), before the stbx
  const uint32_t filas = ((palabra0 >> 4) & 7u) + 1;                         // rlwinm r8,r4,28,29,31; addi r8,r8,1
  uint32_t mascara[4];
  LeerBloque(base, (((filas << 2) & 0xFFFFFFF0u) + kTablaFilas) & ~0xFu,     // rlwinm r31,r8,2,0,27; lvx128
             reinterpret_cast<uint8_t*>(mascara));
  const uint32_t indice = Indice(mango);
  const uint8_t bit = Leer8(base, kTablaBits + (indice & 7u));               // lbzx r3,r5,r6
  const uint32_t sucio = g.sucios + (indice >> 3);
  const uint8_t viejo = Leer8(base, sucio);                                  // lbzx r5,r7,r8
  m.MarcarSucio(sucio, viejo, bit);                                          // stbx r4,r7,r8
  const uint32_t destino = Registro16(palabra1) + g.destino;
  for (uint32_t i = 0; i < 4; ++i) {                                         // vsel + stvx (v9, v8, v7, v0)
    uint32_t salida[4];
    for (uint32_t k = 0; k < 4; ++k) {
      salida[k] = (fila[i][k] & ~mascara[k]) | (fila[k][i] & mascara[k]);
    }
    m.Escribir16B((destino + 16 * i) & ~0xFu, reinterpret_cast<const uint8_t*>(salida));
  }
  return bit;
}

// ---------------------------------------------------------------------------------------------------------------
// List of calls to writers (824511E8 guard): the native version records it when calling each one, and the
// original through the hooks in trace mode. dato = the 16 bytes of the value's block (or [r4] in 8244F2E0).
// ---------------------------------------------------------------------------------------------------------------
enum Tipo : uint8_t { kF2E0 = 0, k9360 = 1, k9618 = 2, k9988 = 3, kC00 = 4, kMaterial = 5, kTipos = 6 };
constexpr const char* kNombres[kTipos] = {"8244F2E0", "82449360", "82449618", "82449988", "82449C00", "824511E8"};

constexpr uint32_t kMaxLlamadas = 16;  // 824511E8 makes at most 11

struct LlamadaEscritora {
  uint8_t tipo;
  uint32_t r3, r4, r5;
  uint8_t dato[16];
};
struct Plan {
  LlamadaEscritora l[kMaxLlamadas];
  uint32_t n = 0;
  bool lleno = false;
};

inline void Apuntar(Plan* plan, uint8_t* base, uint8_t tipo, uint32_t r3, uint32_t r4, uint32_t r5) {
  if (plan == nullptr) {
    return;
  }
  if (plan->n >= kMaxLlamadas) {
    plan->lleno = true;
    return;
  }
  LlamadaEscritora& l = plan->l[plan->n++];
  l.tipo = tipo;
  l.r3 = r3;
  l.r4 = r4;
  l.r5 = r5;
  std::memset(l.dato, 0, sizeof(l.dato));
  if (tipo == kF2E0) {
    const uint32_t mango = Leer32(base, r4);
    std::memcpy(l.dato, &mango, 4);
  } else {
    LeerBloque(base, r5 & ~0xFu, l.dato);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// 824511E8 (nfsmw_recomp.95.cpp:16110). r3 = the material object: [this+12] = handle table, [this+28] = effect.
// ---------------------------------------------------------------------------------------------------------------
struct SalidaMaterial {
  uint32_t r3;
  uint32_t r12;  // and lr: what the epilogue rereads from r1-8
};

template <bool A>
SalidaMaterial MaterialNativo(Memoria<A>& m, uint32_t self, uint32_t r1o, uint64_t lr, Plan* plan) {
  uint8_t* const base = m.base;
  // Prologue: mflr r12; stw r12,-8(r1); std r30,-24(r1); std r31,-16(r1); stfd f31,-32(r1); stwu r1,-128(r1).
  // With the non-volatile registers as local variables (overrides.toml), r30, r31 and f31 are 0 when saved.
  m.Escribir32(r1o - 8, uint32_t(lr));
  m.Escribir64(r1o - 24, 0);
  m.Escribir64(r1o - 16, 0);
  m.Escribir64(r1o - 32, 0);
  const uint32_t r1 = r1o - 128;
  m.Escribir32(r1, r1o);
  const uint32_t hueco = r1 + 80;  // the value is passed to the writers through here
  uint32_t r3 = self;

  // +456 -> 8244F2E0(this, &handle, integer from the table at 0x82A15370)
  {
    const uint32_t mango = Leer32(base, Leer32(base, self + 12) + 456);
    if (mango != 0) {
      bool primero = false;  // loc_82451270 (entry 1 of the table) or loc_82451280 (the indexed one)
      if (Leer16(base, Leer32(base, kModo) + 4) == 7) {
        primero = Leer32(base, kPartida) != 6 || (Leer8(base, kBandera) == 0 && Leer32(base, kValor1) == 1);
      }
      const uint32_t valor = primero ? Leer32(base, kTablaValores + 4)
                                     : Leer32(base, ((Leer32(base, kIndiceValor) << 2) & 0xFFFFFFFCu) + kTablaValores);
      m.Escribir32(hueco, mango);  // stw r9,80(r1)
      if constexpr (A) Apuntar(plan, base, kF2E0, self, hueco, valor);
      r3 = EscritoraF2E0(m, self, hueco, valor);
    }
  }
  // +444, +480 and +440 -> 82449360 with a constant float (lfs + stfs = copy of the word). Before +480, the
  // original leaves r3 = [this+12] (lwz r3,12(r31)) even though it makes no call.
  const uint32_t kPasosFloat[3][2] = {{444, kConst444}, {480, kConst480}, {440, kConst440}};
  for (uint32_t i = 0; i < 3; ++i) {
    const uint32_t tabla = Leer32(base, self + 12);
    if (i == 1) {
      r3 = tabla;
    }
    const uint32_t mango = Leer32(base, tabla + kPasosFloat[i][0]);
    if (mango != 0) {
      const uint32_t efecto = Leer32(base, self + 28);
      m.Escribir32(hueco, Leer32(base, kPasosFloat[i][1]));  // lfs f0 + stfs f0,80(r1)
      if constexpr (A) Apuntar(plan, base, k9360, efecto, mango, hueco);
      Escritora9360(m, efecto, mango, hueco, r1);
      r3 = efecto;
    }
  }
  // +460 and +464 -> 82449360 with the same float (f31, read once before the first one).
  uint32_t f31 = 0;
  for (uint32_t i = 0; i < 2; ++i) {
    const uint32_t mango = Leer32(base, Leer32(base, self + 12) + (i == 0 ? 460u : 464u));
    if (i == 0) {
      f31 = Leer32(base, kConst460);  // lfs f31,-13192(r11)
    }
    if (mango != 0) {
      const uint32_t efecto = Leer32(base, self + 28);
      m.Escribir32(hueco, f31);  // stfs f31,80(r1)
      if constexpr (A) Apuntar(plan, base, k9360, efecto, mango, hueco);
      Escritora9360(m, efecto, mango, hueco, r1);
      r3 = efecto;
    }
  }
  // +476 -> 82449618 with the integer 1
  {
    const uint32_t mango = Leer32(base, Leer32(base, self + 12) + 476);
    if (mango != 0) {
      const uint32_t efecto = Leer32(base, self + 28);
      m.Escribir32(hueco, 1);  // li r7,1; stw r7,80(r1)
      if constexpr (A) Apuntar(plan, base, k9618, efecto, mango, hueco);
      Escritora9618(m, efecto, mango, hueco, r1);
      r3 = efecto;
    }
  }
  // +0 -> 82449988 with the vector at [0x82A2C4F8] + 48
  {
    const uint32_t mango = Leer32(base, Leer32(base, self + 12) + 0);
    if (mango != 0) {
      const uint32_t efecto = Leer32(base, self + 28);
      const uint32_t vector = Leer32(base, kPunteroVector) + 48;
      if constexpr (A) Apuntar(plan, base, k9988, efecto, mango, vector);
      r3 = Escritora9988(m, efecto, mango, vector);
    }
  }
  // +116 and +120 -> 82449360 with the signed 16-bit integers at [0x82A2D174] + 68 / + 70 converted to float
  // (extsh, std, lfd, fcfid, frsp, stfs). The std leaves the integer's 8 bytes in the slot; the stfs overwrites
  // the first 4 with the float and the other 4 remain (they are word 1 of the block the lvlx reads).
  for (uint32_t i = 0; i < 2; ++i) {
    const uint32_t mango = Leer32(base, Leer32(base, self + 12) + (i == 0 ? 116u : 120u));
    if (mango != 0) {
      const uint32_t enteros = Leer32(base, kPunteroEnteros);
      const uint32_t efecto = Leer32(base, self + 28);
      const int16_t e = int16_t(Leer16(base, enteros + (i == 0 ? 68u : 70u)));
      m.Escribir64(hueco, uint64_t(int64_t(e)));                 // std r9,80(r1)
      m.Escribir32(hueco, std::bit_cast<uint32_t>(float(e)));    // stfs f12,80(r1): exacto (|e| < 2^24)
      if constexpr (A) Apuntar(plan, base, k9360, efecto, mango, hueco);
      Escritora9360(m, efecto, mango, hueco, r1);
      r3 = efecto;
    }
  }
  // +448 -> 82449618 with the byte at 0x82A2D1A5
  {
    const uint32_t mango = Leer32(base, Leer32(base, self + 12) + 448);
    if (mango != 0) {
      const uint32_t efecto = Leer32(base, self + 28);
      m.Escribir32(hueco, Leer8(base, kByte448));  // lbz r9,-11867(r11); stw r9,80(r1)
      if constexpr (A) Apuntar(plan, base, k9618, efecto, mango, hueco);
      Escritora9618(m, efecto, mango, hueco, r1);
      r3 = efecto;
    }
  }
  // Epilogo: addi r1,r1,128; lwz r12,-8(r1); mtlr r12 (y lfd f31 / ld r30 / ld r31, variables locales).
  return {r3, Leer32(base, r1o - 8)};
}

// ---------------------------------------------------------------------------------------------------------------
// Counters, report and shutdown. No read-modify-write atomics on the normal path (A57 without LSE); if two
// threads count at the same time a count can be lost, which does not matter.
// ---------------------------------------------------------------------------------------------------------------
constexpr uint64_t kComprobaciones = 50000;  // of each function; then 1 of every 4096
constexpr uint32_t kMaxAnotEscritora = 8;    // 82449360/82449618: 4 stack + dirty + value; 82449C00: 5
constexpr uint32_t kMaxAnotMaterial = 96;    // 824511E8: 75 at most

struct Contadores {
  std::atomic<uint64_t> llamadas{0};      // all of them (decides which ones are checked)
  std::atomic<uint64_t> nativas{0};       // for the report period
  std::atomic<uint64_t> originales{0};
  std::atomic<uint64_t> comprobadas{0};
  std::atomic<uint64_t> comprobadas_total{0};
  std::atomic<bool> apagado{false};
};
Contadores g_c[kTipos];
std::atomic<int64_t> g_siguiente_ms{0};

// 824511E8 guard in progress: the context of the thread running it (its writers record into g_traza).
std::atomic<PPCContext*> g_traza_ctx{nullptr};
Plan g_traza;

template <typename T>
inline void Sumar(std::atomic<T>& c, T n) {
  c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

// The cvar is kInitOnly: it is read on the first call. No local "static" (its guard is an acquire load,
// ldar, on every call) because the writers are called hundreds of thousands of times per second.
std::atomic<int8_t> g_activo{-1};
inline bool Activo() {
  int8_t a = g_activo.load(std::memory_order_relaxed);
  if (a < 0) [[unlikely]] {
    a = REXCVAR_GET(nfsmw_material_nativo) ? 1 : 0;
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
    REXLOG_INFO("[material] parametros de material en nativo (824511E8 y sus escritoras); se comprueban contra la "
                "original las primeras {} llamadas de cada funcion y despues 1 de cada 4096",
                kComprobaciones);
    return;
  }
  std::string linea;
  for (uint32_t t = 0; t < kTipos; ++t) {
    Contadores& c = g_c[t];
    linea += fmt::format(" | {}: {} nativas, {} originales, {} comprobadas{}", kNombres[t],
                         c.nativas.exchange(0, std::memory_order_relaxed),
                         c.originales.exchange(0, std::memory_order_relaxed),
                         c.comprobadas.exchange(0, std::memory_order_relaxed),
                         c.apagado.load(std::memory_order_relaxed) ? " (APAGADA por diferencia)" : "");
  }
  NFSMW_INFORME_DIFERIDO("[material] ultimos 10 s{}", linea);
}

void Apagar(uint32_t tipo) {
  g_c[tipo].apagado.store(true, std::memory_order_relaxed);
  // 824511E8 has the four writers it calls inlined: if one fails, it is turned off as well.
  if (tipo == kF2E0 || tipo == k9360 || tipo == k9618 || tipo == k9988) {
    g_c[kMaterial].apagado.store(true, std::memory_order_relaxed);
  }
}

void NotarComprobada(uint32_t tipo) {
  Contadores& c = g_c[tipo];
  Sumar(c.comprobadas, uint64_t(1));
  const uint64_t total = c.comprobadas_total.load(std::memory_order_relaxed) + 1;
  c.comprobadas_total.store(total, std::memory_order_relaxed);
  if (total == kComprobaciones && !c.apagado.load(std::memory_order_relaxed)) {
    REXLOG_INFO("[material] sub_{}: {} llamadas comprobadas contra la original byte a byte, 0 diferencias: camino "
                "nativo en marcha",
                kNombres[tipo], kComprobaciones);
  }
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

// --- Guard operations on the recorded writes ---
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
// Returns the index of the first record whose memory is not what the native version left, or r.n if all match.
uint32_t PrimeraDistinta(const Registro& r, uint8_t* base) {
  for (uint32_t i = 0; i < r.n; ++i) {
    if (std::memcmp(Puntero(base, r.a[i].direccion), r.a[i].despues, r.a[i].bytes) != 0) {
      return i;
    }
  }
  return r.n;
}
// Poison: what was written, with the bytes inverted; for the dirty byte, the bit cleared. The original must
// leave exactly the same again.
void Envenenar(const Registro& r, uint8_t* base) {
  for (uint32_t i = 0; i < r.n; ++i) {
    uint8_t* p = Puntero(base, r.a[i].direccion);
    for (uint32_t b = 0; b < r.a[i].bytes; ++b) {
      p[b] = r.a[i].clase == kOr ? uint8_t(r.a[i].despues[b] & ~r.a[i].mascara) : uint8_t(~r.a[i].despues[b]);
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// The writers as hooks.
// ---------------------------------------------------------------------------------------------------------------
template <uint32_t T>
inline void LlamarOriginal(PPCContext& ctx, uint8_t* base) {
  if constexpr (T == kF2E0) {
    __imp__sub_8244F2E0(ctx, base);
  } else if constexpr (T == k9360) {
    __imp__sub_82449360(ctx, base);
  } else if constexpr (T == k9618) {
    __imp__sub_82449618(ctx, base);
  } else if constexpr (T == k9988) {
    __imp__sub_82449988(ctx, base);
  } else {
    __imp__sub_82449C00(ctx, base);
  }
}

constexpr bool PoneR3(uint32_t t) {
  return t == kF2E0 || t == k9988 || t == kC00;
}

// The native version without touching the context: returns the r3 the original leaves (if it changes it).
template <uint32_t T, bool A>
inline uint32_t Nativa(Memoria<A>& m, const PPCContext& ctx) {
  if constexpr (T == kF2E0) {
    return EscritoraF2E0(m, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  } else if constexpr (T == k9360) {
    Escritora9360(m, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r1.u32);
    return 0;
  } else if constexpr (T == k9618) {
    Escritora9618(m, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r1.u32);
    return 0;
  } else if constexpr (T == k9988) {
    return Escritora9988(m, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  } else {
    return EscritoraC00(m, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  }
}

// The registers someone can read afterwards (see header): r3 and, in 82449618, flush mode on.
template <uint32_t T>
inline void AplicarRegistros(PPCContext& ctx, uint32_t r3) {
  if constexpr (PoneR3(T)) {
    ctx.r3.u64 = r3;
  }
  if constexpr (T == k9618) {
    ctx.fpscr.enableFlushMode();
  }
}

// Volatile registers written by an original writer (to leave the ones from the first pass).
struct Volatiles {
  PPCRegister r[10];  // r3..r12
  uint64_t lr;
  PPCVRegister v[14];  // v0..v13
};
void GuardarVolatiles(const PPCContext& ctx, Volatiles& s) {
  const PPCRegister* r[10] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10, &ctx.r11, &ctx.r12};
  for (int i = 0; i < 10; ++i) s.r[i] = *r[i];
  s.lr = ctx.lr;
  const PPCVRegister* v[14] = {&ctx.v0, &ctx.v1, &ctx.v2, &ctx.v3, &ctx.v4, &ctx.v5, &ctx.v6,
                               &ctx.v7, &ctx.v8, &ctx.v9, &ctx.v10, &ctx.v11, &ctx.v12, &ctx.v13};
  for (int i = 0; i < 14; ++i) s.v[i] = *v[i];
}
void RestaurarVolatiles(PPCContext& ctx, const Volatiles& s) {
  PPCRegister* r[10] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10, &ctx.r11, &ctx.r12};
  for (int i = 0; i < 10; ++i) *r[i] = s.r[i];
  ctx.lr = s.lr;
  PPCVRegister* v[14] = {&ctx.v0, &ctx.v1, &ctx.v2, &ctx.v3, &ctx.v4, &ctx.v5, &ctx.v6,
                         &ctx.v7, &ctx.v8, &ctx.v9, &ctx.v10, &ctx.v11, &ctx.v12, &ctx.v13};
  for (int i = 0; i < 14; ++i) *v[i] = s.v[i];
}

// Guard of one writer. Always leaves the original's result in memory and registers.
template <uint32_t T>
[[gnu::noinline]] void ComprobarEscritora(PPCContext& ctx, uint8_t* base) {
  const PPCRegister r3e = ctx.r3, r4e = ctx.r4, r5e = ctx.r5;
  const uint64_t lre = ctx.lr;
  const uint32_t csr_esperado =
      T == k9618 ? uint32_t(ctx.fpscr.csr | uint32_t(PPCFPSCRRegister::FlushMask)) : ctx.fpscr.csr;
  Anotacion anotaciones[kMaxAnotEscritora];
  Registro reg{anotaciones, kMaxAnotEscritora};
  Memoria<true> m{base, &reg};
  const uint32_t r3n = Nativa<T>(m, ctx);
  Fotografiar(reg, base);
  Deshacer(reg, base);
  // 1) The original over the real state: same bytes, same r3, same FPCR.
  LlamarOriginal<T>(ctx, base);
  uint32_t mala = PrimeraDistinta(reg, base);
  const char* motivo = mala < reg.n ? "bytes distintos" : nullptr;
  if (!motivo && reg.lleno) motivo = "demasiadas escrituras";
  if (!motivo && PoneR3(T) && ctx.r3.u64 != r3n) motivo = "r3 distinto";
  if (!motivo && ctx.fpscr.csr != csr_esperado) motivo = "FPCR distinto";
  if (!motivo) {
    // 2) Poison what was written and run the original again: it must write exactly there (and nowhere else,
    // because each original writer does a fixed number of writes). The registers from 1) are kept.
    Volatiles primera;
    GuardarVolatiles(ctx, primera);
    Envenenar(reg, base);
    ctx.r3 = r3e;
    ctx.r4 = r4e;
    ctx.r5 = r5e;
    ctx.lr = lre;
    LlamarOriginal<T>(ctx, base);
    mala = PrimeraDistinta(reg, base);
    if (mala < reg.n) motivo = "la original escribe en otra direccion";
    RestaurarVolatiles(ctx, primera);
  }
  NotarComprobada(T);
  if (!motivo) {
    return;
  }
  // Difference: exact state of the original (undo what was recorded and run it again from the entry; its
  // writes are idempotent) and turn off for good.
  uint8_t ahora[16] = {};
  const Anotacion* a = mala < reg.n ? &reg.a[mala] : nullptr;
  if (a) std::memcpy(ahora, Puntero(base, a->direccion), a->bytes);
  Deshacer(reg, base);
  ctx.r3 = r3e;
  ctx.r4 = r4e;
  ctx.r5 = r5e;
  ctx.lr = lre;
  LlamarOriginal<T>(ctx, base);
  Apagar(T);
  REXLOG_INFO("[material] DIFERENCIA en sub_{} ({}; llamada {}): r3 0x{:08X} r4 0x{:08X} r5 0x{:08X} r1 0x{:08X}; "
              "direccion 0x{:08X} nativa {} original {}{}. Camino nativo APAGADO para siempre, se queda la original",
              kNombres[T], motivo, g_c[T].comprobadas_total.load(std::memory_order_relaxed), r3e.u32, r4e.u32,
              r5e.u32, ctx.r1.u32, a ? a->direccion : 0u, a ? Hex(a->despues, a->bytes) : std::string("-"),
              a ? Hex(ahora, a->bytes) : std::string("-"),
              PoneR3(T) ? fmt::format("; r3 nativa 0x{:X} original 0x{:X}", r3n, ctx.r3.u64) : std::string());
}

template <uint32_t T>
inline void Escritora(PPCContext& ctx, uint8_t* base) {
  Contadores& c = g_c[T];
  if (g_traza_ctx.load(std::memory_order_relaxed) == &ctx) [[unlikely]] {
    // Inside the 824511E8 guard: record the original's call and check the writer with it.
    Apuntar(&g_traza, base, uint8_t(T), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
    if (c.apagado.load(std::memory_order_relaxed)) {
      LlamarOriginal<T>(ctx, base);
    } else {
      ComprobarEscritora<T>(ctx, base);
    }
    return;
  }
  if (!Activo()) {
    LlamarOriginal<T>(ctx, base);
    return;
  }
  const uint64_t n = c.llamadas.load(std::memory_order_relaxed) + 1;
  c.llamadas.store(n, std::memory_order_relaxed);
  if (c.apagado.load(std::memory_order_relaxed)) [[unlikely]] {
    Sumar(c.originales, uint64_t(1));
    LlamarOriginal<T>(ctx, base);
  } else if (n <= kComprobaciones || (n & 4095) == 0) [[unlikely]] {
    ComprobarEscritora<T>(ctx, base);
  } else {
    Memoria<false> m{base, nullptr};
    AplicarRegistros<T>(ctx, Nativa<T>(m, ctx));
    Sumar(c.nativas, uint64_t(1));
  }
  if ((n & 4095) == 0) [[unlikely]] {
    Informe();
  }
}

// ---------------------------------------------------------------------------------------------------------------
// 824511E8 as a hook.
// ---------------------------------------------------------------------------------------------------------------
inline void MaterialRapido(PPCContext& ctx, uint8_t* base) {
  Memoria<false> m{base, nullptr};
  const SalidaMaterial s = MaterialNativo(m, ctx.r3.u32, ctx.r1.u32, ctx.lr, nullptr);
  ctx.fpscr.disableFlushMode();  // the original exits in scalar mode (lfd f31 of the epilogue)
  ctx.r3.u64 = s.r3;
  ctx.r12.u64 = s.r12;
  ctx.lr = s.r12;
}

bool MismoPlan(const Plan& a, const Plan& b, uint32_t& donde) {
  if (a.lleno || b.lleno) {
    donde = kMaxLlamadas;
    return false;
  }
  const uint32_t n = a.n < b.n ? a.n : b.n;
  for (donde = 0; donde < n; ++donde) {
    const LlamadaEscritora& x = a.l[donde];
    const LlamadaEscritora& y = b.l[donde];
    if (x.tipo != y.tipo || x.r3 != y.r3 || x.r4 != y.r4 || x.r5 != y.r5 ||
        std::memcmp(x.dato, y.dato, sizeof(x.dato)) != 0) {
      return false;
    }
  }
  return a.n == b.n;
}

// 824511E8 guard. Always leaves the original's result.
[[gnu::noinline]] void ComprobarMaterial(PPCContext& ctx, uint8_t* base, uint64_t n) {
  PPCContext* libre = nullptr;
  if (!g_traza_ctx.compare_exchange_strong(libre, &ctx, std::memory_order_acquire, std::memory_order_relaxed)) {
    // Another thread is in its guard: the original at first; afterwards, the already checked native version.
    if (n <= kComprobaciones) {
      Sumar(g_c[kMaterial].originales, uint64_t(1));
      __imp__sub_824511E8(ctx, base);
    } else {
      MaterialRapido(ctx, base);
      Sumar(g_c[kMaterial].nativas, uint64_t(1));
    }
    return;
  }
  const uint32_t self = ctx.r3.u32;
  const uint32_t r1o = ctx.r1.u32;
  const uint64_t r1e = ctx.r1.u64;
  const uint64_t lre = ctx.lr;
  const uint32_t csr_esperado = ctx.fpscr.csr & ~uint32_t(PPCFPSCRRegister::FlushMask);
  Anotacion anotaciones[kMaxAnotMaterial];
  Registro reg{anotaciones, kMaxAnotMaterial};
  Memoria<true> m{base, &reg};
  Plan plan;
  const SalidaMaterial s = MaterialNativo(m, self, r1o, lre, &plan);
  Fotografiar(reg, base);
  Deshacer(reg, base);
  g_traza.n = 0;
  g_traza.lleno = false;
  __imp__sub_824511E8(ctx, base);  // the original; its writers go through the hooks in trace mode
  g_traza_ctx.store(nullptr, std::memory_order_release);

  uint32_t donde = 0;
  const uint32_t mala = PrimeraDistinta(reg, base);
  const char* motivo = nullptr;
  if (reg.lleno) motivo = "demasiadas escrituras";
  else if (!MismoPlan(plan, g_traza, donde)) motivo = "llamadas a escritoras distintas";
  else if (mala < reg.n) motivo = "bytes distintos";
  else if (ctx.r3.u64 != s.r3) motivo = "r3 distinto";
  else if (ctx.r12.u64 != s.r12 || ctx.lr != s.r12) motivo = "r12/lr distintos";
  else if (ctx.r1.u64 != r1e) motivo = "r1 distinto";
  else if (ctx.fpscr.csr != csr_esperado) motivo = "FPCR distinto";
  NotarComprobada(kMaterial);
  if (!motivo) {
    return;
  }
  Apagar(kMaterial);  // the state is already the original's
  const Anotacion* a = mala < reg.n ? &reg.a[mala] : nullptr;
  REXLOG_INFO("[material] DIFERENCIA en sub_824511E8 ({}; llamada {}): this 0x{:08X} r1 0x{:08X}; escritoras "
              "nativa {} original {} (primera distinta: {}); direccion 0x{:08X} nativa {} original {}; r3 nativa "
              "0x{:X} original 0x{:X}. Camino nativo APAGADO para siempre, se queda la original",
              motivo, g_c[kMaterial].comprobadas_total.load(std::memory_order_relaxed), self, r1o, plan.n,
              g_traza.n, donde, a ? a->direccion : 0u, a ? Hex(a->despues, a->bytes) : std::string("-"),
              a ? Hex(Puntero(base, a->direccion), a->bytes) : std::string("-"), s.r3, ctx.r3.u64);
}

inline void Material(PPCContext& ctx, uint8_t* base) {
  if (!Activo()) {
    __imp__sub_824511E8(ctx, base);
    return;
  }
  Contadores& c = g_c[kMaterial];
  const uint64_t n = c.llamadas.load(std::memory_order_relaxed) + 1;
  c.llamadas.store(n, std::memory_order_relaxed);
  if (c.apagado.load(std::memory_order_relaxed)) [[unlikely]] {
    Sumar(c.originales, uint64_t(1));
    __imp__sub_824511E8(ctx, base);  // calls the writers' hooks (which stay native while they are valid)
  } else if (n <= kComprobaciones || (n & 4095) == 0) [[unlikely]] {
    ComprobarMaterial(ctx, base, n);
  } else {
    MaterialRapido(ctx, base);
    Sumar(c.nativas, uint64_t(1));
  }
  if ((n & 4095) == 0) [[unlikely]] {
    Informe();
  }
}

}  // namespace
}  // namespace nfsmw::material

// The hooks. The generated code's calls to these six addresses must go to sub_X and not to __imp__sub_X:
// tools/llamadas_directas.py leaves them as sub_X because this file names their addresses.
REX_HOOK_RAW(sub_824511E8) {
  nfsmw::material::Material(ctx, base);
}
REX_HOOK_RAW(sub_8244F2E0) {
  nfsmw::material::Escritora<nfsmw::material::kF2E0>(ctx, base);
}
REX_HOOK_RAW(sub_82449360) {
  nfsmw::material::Escritora<nfsmw::material::k9360>(ctx, base);
}
REX_HOOK_RAW(sub_82449618) {
  nfsmw::material::Escritora<nfsmw::material::k9618>(ctx, base);
}
REX_HOOK_RAW(sub_82449988) {
  nfsmw::material::Escritora<nfsmw::material::k9988>(ctx, base);
}
REX_HOOK_RAW(sub_82449C00) {
  nfsmw::material::Escritora<nfsmw::material::kC00>(ctx, base);
}
