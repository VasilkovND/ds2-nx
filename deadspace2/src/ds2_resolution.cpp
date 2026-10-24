// deadspace2 - the scene resolution: the game's own 1280x720 or 960x540 (three quarters, the same 16:9 shape, scaled to
// the 1280x720 back buffer as the game already scales its other scene sizes). 960x540 is for stock clocks: the GPU
// does about 56 % of the pixel work, and light scenes were GPU-bound at 307.2 MHz.
//
// Displ_CreateDevice (EARS lldisp_xenon, 0x8290C380) writes the scene size every part of the renderer reads (u16
// 0x82E3CFF8 width, 0x82E3CFFC height: 1280x720 in HD, 1200x720 with the engine's own "reduced HD targets", 1024x768
// in SD) and the back buffer's (0x82E3D014 / 0x82E3D018: 1280x720 or 640x480, kept). After it runs, 960x540 replaces
// 1280x720 when that is the choice (ds2_resolution.h: the menu's saved choice, ds2.toml, or the CPU clock). The game
// sets up its display once, so a new choice takes effect at the next start. As in ds3-nx (src/ds3_resolution.cpp,
// there 864x540 for its 1152x720).

#include "ds2_rex_diagnostic_pch.h"

#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>

#include "ds2_resolution.h"

REXCVAR_DEFINE_INT32(ds2_resolucion, 0, "DS2",
                     "Resolucion de la escena si el menu (ZL + ZR + Menos) no guardo ninguna: 720 (1280x720, la del "
                     "juego) o 540 (960x540, para frecuencias de serie). 0 = segun la CPU: 720 desde 1,5 GHz, si no "
                     "540. Al reiniciar")
    .range(0, 720);

REX_EXTERN(__imp__Displ_CreateDevice_8290C380);
REX_EXTERN(Displ_CreateDevice_8290C380) {
  __imp__Displ_CreateDevice_8290C380(ctx, base);
  namespace r = ds2::resolucion;
  const uint16_t ancho = rex::memory::load_and_swap<uint16_t>(REX_RAW_ADDR(r::kAnchoEscena));
  const uint16_t alto = rex::memory::load_and_swap<uint16_t>(REX_RAW_ADDR(r::kAltoEscena));
  const uint32_t guardada = r::Guardada();
  const uint32_t elegida = guardada ? guardada : r::PorDefecto();
  if (elegida == 540 && ancho == 1280 && alto == 720) {
    rex::memory::store_and_swap<uint16_t>(REX_RAW_ADDR(r::kAnchoEscena), 960);
    rex::memory::store_and_swap<uint16_t>(REX_RAW_ADDR(r::kAltoEscena), 540);
  }
  REXLOG_INFO("ds2: escena {}x{} (el juego puso {}x{}; elegida {}p, {}; CPU {} MHz)",
              rex::memory::load_and_swap<uint16_t>(REX_RAW_ADDR(r::kAnchoEscena)),
              rex::memory::load_and_swap<uint16_t>(REX_RAW_ADDR(r::kAltoEscena)), ancho, alto, elegida,
              guardada ? "guardada en el menu" : "por defecto", r::CpuMhz());
}
