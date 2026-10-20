// deadspace2 - the game's graphics system on the Switch (see ds2_gpu.cpp; copied from halo3/src/halo3_gpu.cpp, game-neutral).

#pragma once

#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace ds2::gpu {

// Stands in for the Xbox 360 GPU: answers what the game's Direct3D waits for, and reads its command ring.
std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem();

}  // namespace ds2::gpu
