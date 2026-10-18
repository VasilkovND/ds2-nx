// Halo 3 shader common header: NFS's shared helpers, plus what Halo's shaders need and NFS's never did.
// Passed to halo3_hlsl as the <shader_common.h> argument, and inlined at the top of every translated shader.
//
// Compile Halo's HLSL with the shaders/XenosRecomp directory on the include path (for shader_common.h), and with
// -DHALO3_RECOMP so XenosRecomp emits the Halo forms below.
#include "shader_common.h"

// Halo 3 uses the full Xbox 360 boolean register space (addresses 0-255), far past the 16 bits per stage NFS packs
// into g_Booleans. They live in their own 256-bit block, read by constant index A (the boolean address is always a
// constant in the control flow, so uam sees no dynamic component index). The renderer fills this block per draw
// from the Xbox 360 boolean constants (SQ_... in the register file).
#ifdef NFSMW_DEKO3D
struct Halo3BooleanBlock { uint4 b[2]; };
[[vk::binding(3, 4)]] ConstantBuffer<Halo3BooleanBlock> g_Halo3Booleans;
#define halo_bool(A) ((g_Halo3Booleans.b[((A) >> 7) & 1][((A) >> 5) & 3] >> ((A) & 31)) & 1u)
#else
// Non-deko (PC/DXC reference) path: the booleans come from the shared push-constant buffer at byte 512.
#define halo_bool(A) ((vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 512 + (((A) >> 5) & 7) * 4) >> ((A) & 31)) & 1u)
#endif
