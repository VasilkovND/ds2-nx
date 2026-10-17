// nfsmw - guest memory access for the sound engine functions implemented in native code
//
// Big-endian reads and writes like the REX_LOAD_*/REX_STORE_* macros of the generated code (nfsmw_pch.h), but
// without volatile pointers, so that the compiler can merge them. On Windows the guest physical addresses
// (0xE0000000 and above) are 0x1000 bytes further on (REX_PHYS_HOST_OFFSET); on the Switch they are not.
// Used by nfsmw_audio_remuestreo.cpp and nfsmw_audio_filtro.cpp.

#pragma once

#include <cstdint>
#include <cstring>

#include <rex/platform.h>

namespace nfsmw::audio_nativo {

inline uint8_t* Dir(uint8_t* base, uint32_t dir) {
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
  return base + dir + (dir >= 0xE0000000u ? 0x1000u : 0u);
#else
  return base + dir;
#endif
}

inline uint32_t Leer32(uint8_t* base, uint32_t dir) {
  uint32_t v = 0;
  std::memcpy(&v, Dir(base, dir), sizeof(v));
  return __builtin_bswap32(v);
}

inline uint16_t Leer16(uint8_t* base, uint32_t dir) {
  uint16_t v = 0;
  std::memcpy(&v, Dir(base, dir), sizeof(v));
  return __builtin_bswap16(v);
}

inline void Escribir32(uint8_t* base, uint32_t dir, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(Dir(base, dir), &v, sizeof(v));
}

inline uint32_t Bits(float f) {
  uint32_t b = 0;
  std::memcpy(&b, &f, sizeof(b));
  return b;
}

inline float LeerFloat(uint8_t* base, uint32_t dir) {
  const uint32_t bits = Leer32(base, dir);
  float f = 0.0f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

inline void EscribirFloat(uint8_t* base, uint32_t dir, float f) {
  Escribir32(base, dir, Bits(f));
}

// Whether two guest address ranges touch. A range that wraps around 4 GB counts as overlapping.
inline bool Solapan(uint32_t a, uint64_t tam_a, uint32_t b, uint64_t tam_b) {
  if (uint64_t(a) + tam_a > 0x100000000ull || uint64_t(b) + tam_b > 0x100000000ull) {
    return true;
  }
  return uint64_t(a) < uint64_t(b) + tam_b && uint64_t(b) < uint64_t(a) + tam_a;
}

}  // namespace nfsmw::audio_nativo
