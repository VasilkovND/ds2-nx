// deadspace2 - the game's graphics system on the Switch (see ds2_gpu.cpp; copied from halo3/src/halo3_gpu.cpp, game-neutral).

#pragma once

#include <cstdint>
#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace ds2::gpu {

// Stands in for the Xbox 360 GPU: answers what the game's Direct3D waits for, and reads its command ring.
std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem();

// The game's gamma ramp as Direct3D keeps it after D3DDevice_SetGammaRamp (three planes of 256 big-endian u16: red,
// green, blue; each a 10-bit value shifted left by 6, as GammaRampCorrected writes it). Applied by the renderer at the
// next frame it presents (the Xbox 360 applied it in its display output; the in-game brightness setting is this ramp).
void PonerRampaGamma(const uint8_t* rampa_be);

}  // namespace ds2::gpu
