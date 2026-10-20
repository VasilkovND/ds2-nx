# Third-party notices

DS2-NX (ds2-nx) is built on the work of other projects. Their licenses apply to their code and to the parts of this
repository derived from it.

## Projects this port is derived from

| Project | Used for | License |
|---|---|---|
| [Dead-Space-2-Recomp](https://github.com/bifftannen1/Dead-Space-2-Recomp) by bifftannen1 | The recompiled Title Update 2 code (`src_tu2/`, not in this repository) that `deadspace2/` builds, and the parts of `deadspace2/src` based on its PC app | See that repository |
| [NFSMW-NX](https://github.com/StevensND/nfsmw-nx) by StevensND | The port this repository was forked from: the Horizon (Switch) layer and deko3d renderer in `sdk/` (via `rexglue-sdk-nx`), and the shader tools in `shaders/` | GPL-3.0 (this repository), BSD-3-Clause (SDK changes), MIT (translator changes) |
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) by Tom Clay, with portions from [Xenia](https://xenia.jp) (Ben Vanik and the Xenia contributors) | `sdk/`: Xbox 360 kernel and runtime, GPU command processing, Switch layer | BSD-3-Clause (`sdk/LICENSE`) |
| [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) by hedge-dev and contributors | `shaders/XenosRecomp`: Xenos microcode to HLSL translator | MIT (`shaders/LICENSE.md`) |

The Horizon (Switch) layer and renderer in `sdk/` are under the SDK's BSD-3-Clause license, and the changes in
`shaders/XenosRecomp` under MIT, so they can be reused by other ports. The rest of this repository is GPL-3.0
(`LICENSE`).

## Libraries

Libraries in the SDK's third-party tree (`sdk/thirdparty`, set up for a Switch build by `sdk/tools/switch_setup.py`)
that the NRO or the shader tools use:

| Library | License |
|---|---|
| [FFmpeg](https://ffmpeg.org) (libavcodec, libavutil: the WMV3 decoder) | LGPL-2.1-or-later (configured without GPL parts) |
| [libmspack](https://www.cabextract.org.uk/libmspack/) by Stuart Caie (LZX decompression) | LGPL-2.1 |
| [fmt](https://github.com/fmtlib/fmt), [spdlog](https://github.com/gabime/spdlog) | MIT |
| [xxHash](https://github.com/Cyan4973/xxHash) | BSD-2-Clause |
| [SIMDe](https://github.com/simd-everywhere/simde) | MIT |
| [toml++](https://github.com/marzer/tomlplusplus) | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT |
| [o1heap](https://github.com/pavel-kirienko/o1heap) | MIT |
| [inja](https://github.com/pantor/inja) | MIT |
| [utfcpp](https://github.com/nemtrif/utfcpp) | BSL-1.0 |
| [CLI11](https://github.com/CLIUtils/CLI11) | BSD-3-Clause |
| AES-128 by LuoPeng (`sdk/thirdparty/aes_128`) | MIT |
| [deko3d](https://github.com/devkitPro/deko3d) (devkitPro's package) | zlib |
| [uam](https://github.com/devkitPro/uam), as vendored by [SwitchWakerHD](https://github.com/centollOS/SwitchWakerHD) (`runtime/third_party/uam`, with the patches its `PATCHES.md` lists) | zlib for uam's own files, MIT for the Mesa parts |
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) | Apache-2.0 |
| [libnx](https://github.com/switchbrew/libnx) and the devkitA64 runtime (newlib, libstdc++) | ISC; newlib and GCC runtime licenses |

The shader library is built on the PC with [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler)
and spirv-val from the Vulkan SDK; neither is part of this repository or of the NRO.

### FFmpeg and libmspack (LGPL)

The SDK modifies a few files of its FFmpeg tree (FFT/MDCT templates, the AArch64 Horizon configuration, a
configuration limited to the WMV3 decoder, the codec list, and the CMake build); the modified copies are in
`sdk/thirdparty` and the SDK's `switch/overlay/`. The rest of FFmpeg and libmspack is the unmodified upstream
source at the commits pinned by the SDK. Everything needed to rebuild and relink the NRO with a modified version of
either library is in this repository and the SDK (see [README.md](README.md)).

## Trademarks

Dead Space and Dead Space 2 are trademarks of Electronic Arts Inc. Nintendo Switch is a trademark of Nintendo.
Xbox 360 is a trademark of Microsoft. This project is not affiliated with or endorsed by any of them.
