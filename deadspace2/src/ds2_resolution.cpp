// deadspace2 - the scene resolution: the game's own 1280x720 or 960x540 (three quarters, the same 16:9 shape, scaled to
// the 1280x720 back buffer as the game already scales its other scene sizes). 960x540 is for stock clocks: the GPU
// does about 56 % of the pixel work, and light scenes were GPU-bound at 307.2 MHz.
//
// Displ_CreateDevice (EARS lldisp_xenon, 0x8290C380) writes the scene size every part of the renderer reads (u16
// 0x82E3CFF8 width, 0x82E3CFFC height: 1280x720 in HD, 1200x720 with the engine's own "reduced HD targets", 1024x768
// in SD) and the back buffer's (0x82E3D014 / 0x82E3D018: 1280x720 or 640x480, kept). After it runs, 960x540 replaces
// 1280x720 when that is the choice. The game sets up its display once, so ds2_resolucion takes effect at the next
// start. As in ds3-nx (src/ds3_resolution.cpp, there 864x540 for its 1152x720).

#include "ds2_rex_diagnostic_pch.h"

#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>

#if defined(__SWITCH__)
#include <switch.h>
#endif

REXCVAR_DEFINE_INT32(ds2_resolucion, 0, "DS2",
                     "Resolucion de la escena: 720 (1280x720, la del juego) o 540 (960x540, para frecuencias de "
                     "serie). 0 = segun la CPU: 720 desde 1,5 GHz (consola con overclock), si no 540. Al reiniciar")
    .range(0, 720);

namespace {

constexpr uint32_t kAnchoEscena = 0x82E3CFF8;
constexpr uint32_t kAltoEscena = 0x82E3CFFC;

// The CPU clock in MHz (0 if it cannot be read).
uint32_t CpuMhz() {
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

}  // namespace

REX_EXTERN(__imp__Displ_CreateDevice_8290C380);
REX_EXTERN(Displ_CreateDevice_8290C380) {
  __imp__Displ_CreateDevice_8290C380(ctx, base);
  const uint16_t ancho = rex::memory::load_and_swap<uint16_t>(REX_RAW_ADDR(kAnchoEscena));
  const uint16_t alto = rex::memory::load_and_swap<uint16_t>(REX_RAW_ADDR(kAltoEscena));
  const int32_t pedida = REXCVAR_GET(ds2_resolucion);
  const uint32_t mhz = CpuMhz();
  const int32_t elegida = pedida == 720 || pedida == 540 ? pedida : (mhz == 0 || mhz >= 1500 ? 720 : 540);
  if (elegida == 540 && ancho == 1280 && alto == 720) {
    rex::memory::store_and_swap<uint16_t>(REX_RAW_ADDR(kAnchoEscena), 960);
    rex::memory::store_and_swap<uint16_t>(REX_RAW_ADDR(kAltoEscena), 540);
  }
  REXLOG_INFO("ds2: escena {}x{} (el juego puso {}x{}; elegida {}p, {}; CPU {} MHz)",
              rex::memory::load_and_swap<uint16_t>(REX_RAW_ADDR(kAnchoEscena)),
              rex::memory::load_and_swap<uint16_t>(REX_RAW_ADDR(kAltoEscena)), ancho, alto, elegida,
              pedida == 720 || pedida == 540 ? "ds2_resolucion" : "segun la CPU", mhz);
}
