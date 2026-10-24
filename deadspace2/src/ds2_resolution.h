// deadspace2 - the scene resolution choice, shared by the hook that applies it (ds2_resolution.cpp) and the options menu
// (ds2_menu.cpp, ZL + ZR + Minus). As in ds3-nx (src/ds3_resolution.h).
//
// What decides, first that is set: ds2_resolucion.txt beside the NRO ("720" or "540", written by the menu), then
// ds2_resolucion in ds2.toml (720 or 540), then the CPU clock: 720 from 1.5 GHz up (an overclocked console), else 540.

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>

#include <rex/cvar.h>
#include <rex/filesystem.h>

#if defined(__SWITCH__)
#include <switch.h>
#endif

REXCVAR_DECLARE(int32_t, ds2_resolucion);

namespace ds2::resolucion {

// Where the game keeps its scene size (big-endian u16), written by Displ_CreateDevice.
constexpr uint32_t kAnchoEscena = 0x82E3CFF8;
constexpr uint32_t kAltoEscena = 0x82E3CFFC;

inline std::filesystem::path Fichero() { return rex::filesystem::GetExecutableFolder() / "ds2_resolucion.txt"; }

// The menu's saved choice (720 or 540), 0 if there is none.
inline uint32_t Guardada() {
  FILE* f = std::fopen(Fichero().string().c_str(), "r");
  if (!f) return 0;
  unsigned valor = 0;
  const bool leido = std::fscanf(f, "%u", &valor) == 1;
  std::fclose(f);
  return leido && (valor == 720 || valor == 540) ? valor : 0;
}

inline bool Guardar(uint32_t valor) {
  FILE* f = std::fopen(Fichero().string().c_str(), "w");
  if (!f) return false;
  const bool escrito = std::fprintf(f, "%u\n", unsigned(valor)) > 0;
  return std::fclose(f) == 0 && escrito;
}

// The CPU clock in MHz (0 if it cannot be read).
inline uint32_t CpuMhz() {
#if defined(__SWITCH__)
  uint32_t hz = 0;
  if (R_SUCCEEDED(clkrstInitialize())) {
    ClkrstSession sesion;
    if (R_SUCCEEDED(clkrstOpenSession(&sesion, PcvModuleId_CpuBus, 3))) {
      clkrstGetClockRate(&sesion, &hz);
      clkrstCloseSession(&sesion);
    }
    clkrstExit();
  }
  return hz / 1000000;
#else
  return 0;
#endif
}

inline uint32_t PorDefecto() {
  const int32_t pedida = REXCVAR_GET(ds2_resolucion);
  if (pedida == 720 || pedida == 540) return uint32_t(pedida);
  const uint32_t mhz = CpuMhz();
  return mhz == 0 || mhz >= 1500 ? 720 : 540;
}

inline uint32_t Elegida() {
  const uint32_t guardada = Guardada();
  return guardada ? guardada : PorDefecto();
}

}  // namespace ds2::resolucion
