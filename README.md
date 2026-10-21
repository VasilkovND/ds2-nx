# ds2-nx

Dead Space 2 (Xbox 360, Title Update 2) running natively on the Nintendo Switch. It's not an emulator: the game's
PowerPC code is statically recompiled to C++ with [ReXGlue](https://github.com/rexglue/rexglue-sdk) (the recompiled
tree comes from [Dead-Space-2-Recomp](https://github.com/bifftannen1/Dead-Space-2-Recomp)), built for the Switch as
a homebrew NRO, and frames are drawn by a native deko3d renderer from the game's translated shaders.

> [!NOTE]
> You need your own copy of the game (both discs and Title Update 2). This repository contains no game data or
> game code.

Status: **1.0.0** — playable. Installation: [INSTALL.md](INSTALL.md). Comfortable when overclocked; about 20-30 fps at stock clocks,
with drops in the heaviest fights. What changed, the controls and the known issues are in [CHANGELOG.md](CHANGELOG.md);
details, boot history and measurements in [deadspace2/PORTING.md](deadspace2/PORTING.md).

## Layout

| Folder | Contents |
| --- | --- |
| `deadspace2/` | The game project: CMake, Switch glue (`src/`: app, disc swap, GPU ring reader and renderer glue, shader dump and library), build and SD staging scripts (`tools/`), and our SDK patches (`sdk-patches/`) |
| `shaders/` | Shader toolchain: Xenos microcode -> HLSL -> SPIR-V -> shader library and DKSH cache ([shaders/README.md](shaders/README.md)) |
| `sdk/` | **Submodule** -> [`rexglue-sdk-nx`](https://github.com/nx-mod/rexglue-sdk-nx) (`switch` branch): the ReXGlue SDK + Horizon (Switch) layer + deko3d renderer. Our changes to it live as patches in `deadspace2/sdk-patches/`, applied by the build script |
| `tools/switch/cmake/` | `switch-devkitA64.cmake`, the CMake toolchain file for devkitA64 |

## Build the NRO

devkitA64, libnx and deko3d from [devkitPro](https://devkitpro.org) (`sudo dkp-pacman -S switch-dev deko3d`),
CMake, Ninja, Python 3. On a Mac, from the repository root:

```sh
deadspace2/tools/build_mac.sh [path/to/Dead-Space-2-Recomp]
```

It clones Dead-Space-2-Recomp if needed, initializes `sdk/`, applies `deadspace2/sdk-patches/`, configures the deko3d
build and writes `deadspace2/out/sw/deadspace2.nro`. Copy it to the SD card as `ds2-nx.nro`.

## Build the shader library

The game's shaders are dumped on the console (`shaders_dump/`, by `deadspace2/src/ds2_shader_dump.cpp`) and
translated on the PC (needs clang, and `dxc` and `spirv-val` from the Vulkan SDK):

```sh
shaders/build_library.sh <shaders_dump> <out>          # -> <out>/ds2_shaders.dslib
DKSH=1 shaders/build_library.sh <shaders_dump> <out>   # also <out>/shadercache_dksh.bin
```

The DKSH cache holds every shader precompiled for deko3d, so the console never runs the shader compiler.

## SD card

```
sdmc:/switch/ds2-nx/
    ds2-nx.nro
    ds2_shaders.dslib         shader library (shaders/build_library.sh)
    shadercache_dksh.bin      DKSH cache (DKSH=1)
    tu/                       default.xex (Disc 1) + default.xexp (Title Update 2)
    disc1/                    extracted Disc 1
    disc2/                    extracted Disc 2
    telemetria.flag           optional, debugging only
```

`deadspace2/tools/stage_sd.py` makes `tu/`, `disc1/` and `disc2/` from your ISOs and title update and checks the
hashes. An empty `telemetria.flag` turns on automatic screenshots every 5 s, frame dumps and a 1 ms CPU stack sampler;
it costs performance, so only use it for debugging.

## Credits

- **[bifftannen1](https://github.com/bifftannen1/Dead-Space-2-Recomp)**: Dead-Space-2-Recomp, the recompiled TU2
  code this port builds.
- **[StevensND](https://github.com/StevensND/nfsmw-nx)**: NFSMW-NX, the Switch port this repository was forked from:
  its Switch layer, deko3d renderer and shader tools.
- **[nx-mod](https://github.com/nx-mod/rexglue-sdk-nx)**: rexglue-sdk-nx, the Switch fork of the SDK this port builds
  on, and its Halo 3 port, where the PM4 ring reader (`deadspace2/src/ds2_gpu.cpp`) and the universal deko3d renderer
  (`rex::x360::RenderDk`) started.
- **[Tom Clay](https://github.com/rexglue/rexglue-sdk)**: the ReXGlue SDK, built on the work of the
  **[Xenia](https://xenia.jp)** team.
- **[hedge-dev](https://github.com/hedge-dev/XenosRecomp)**: XenosRecomp.
- **[centollOS](https://github.com/centollOS/SwitchWakerHD)**: the uam shader compiler patches.
- **[devkitPro](https://devkitpro.org) and [switchbrew](https://github.com/switchbrew/libnx)**: devkitA64, libnx
  and deko3d.
- **[Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)** (SciresM and contributors): the custom firmware every
  Switch homebrew runs on.
- **[DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler)** and the
  **[Vulkan SDK](https://vulkan.lunarg.com)** (spirv-val): building the shader library.
- **[FFmpeg](https://ffmpeg.org)**: the XMA (WMA Pro) audio decoding.
- The other libraries listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Legal

Unofficial fan port with no affiliation to Electronic Arts or Visceral Games. "Dead Space" is a trademark of
Electronic Arts. You provide your own legally obtained copy of the game. No prebuilt NRO is distributed: it contains
code recompiled from the game, so it is built from source on your own computer ([INSTALL.md](INSTALL.md)). Running homebrew requires custom firmware,
which violates Nintendo's terms and can get a console banned.

Source code is under the GPL-3.0 license (see [LICENSE](LICENSE)). The SDK changes are under the SDK's BSD-3-Clause
license, and the shader translator changes under MIT.
