# NFSMW-NX

A native Nintendo Switch port of **Need for Speed: Most Wanted** (2005), built from the Xbox 360 version.

It is not an emulator. The PowerPC code of the game's `default.xex` is translated ahead of time into C++ and compiled
for the Switch's ARM CPU, and the game's rendering runs on a native Vulkan renderer. It targets 30 FPS at the
console's stock clocks, with no overclock.

**To play it, use the installer page: https://stevensnd.github.io/nfsmw-nx-installer/**. It reads your own copy of
the game in the browser (nothing is uploaded) and builds a package ready to copy to the SD card. This repository contains no game files and nothing derived from them: you need your own legally obtained copy
of the game.

## How it works

- **Recompilation.** [ReXGlue](https://github.com/rexglue/rexglue-sdk) translates every function of the executable
  to C++ and provides the Xbox 360 kernel, file system, audio and input. This port adds a Horizon (Switch) layer to
  it: memory mapping, threads, exceptions, clocks, audio output and presentation.
- **Native renderer.** Instead of emulating the Xenos GPU, the renderer reads the game's PM4 command ring and records
  Vulkan directly, with render targets, textures and shaders managed natively. Frequently called parts of the game's
  Direct3D layer are replaced by native code, each with a guard that checks it against the original.
- **Shaders.** The game's shader microcode is translated ahead of time with
  [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) to HLSL, compiled to SPIR-V with DXC and packed into a
  library. The installer page does this from your disc.
- **Driver.** Vulkan runs on NVK (Mesa) through [mesa-switch](https://github.com/danfromtico/mesa-switch), with the
  changes in `mesa/`: ZCULL, NAK compiler fixes and a cheaper draw path.
- **Performance.** From a few frames per second at the start to 30 FPS at stock clocks: the recompiled code built
  with LTO, PGO and function ordering, the renderer's CPU cost per draw cut down, hot game functions moved to native
  code, and the GPU time per frame reduced. The few changes that alter the image (a cheaper shadow filter, no radial
  blur on the final image, no vegetation in the shadow maps) are settings in `nfsmw.toml`.

## Documentation

For anyone porting another Xbox 360 game, or curious about how this one was done:

| Document | Contents |
|---|---|
| [docs/building.md](docs/building.md) | Building the NRO, the driver and the shader library |
| [docs/native-renderer.md](docs/native-renderer.md) | The native renderer: from the PM4 ring to Vulkan |
| [docs/performance-history.md](docs/performance-history.md) | How the frame rate went from a few FPS to 30, step by step |
| [docs/measuring.md](docs/measuring.md) | Measuring on the console without fooling yourself |
| [docs/toolchain.md](docs/toolchain.md) | Code generation, direct calls, LTO, PGO, function ordering |
| [docs/shaders.md](docs/shaders.md) | Shader translation and the fixes it needed |
| [docs/mesa.md](docs/mesa.md) | Mesa, NVK and NAK on Horizon |
| [docs/platform-notes.md](docs/platform-notes.md) | Horizon: memory, threads, clocks, costs of the platform |
| [docs/audio-and-video.md](docs/audio-and-video.md) | XMA audio and the WMV3 cutscenes |
| [docs/editions.md](docs/editions.md) | Supporting every edition of the game |
| [docs/porting-another-game.md](docs/porting-another-game.md) | Where to start with another game |

## Repository layout

| Folder | Contents |
|---|---|
| `app/` | The game: hooks, native renderer, audio, video, configuration, CMake project |
| `sdk/` | ReXGlue SDK with the Horizon layer and the code generator changes |
| `shaders/` | XenosRecomp with this port's changes, the library tools and their WebAssembly builds |
| `mesa/` | The patch for mesa-switch |
| `pgo/` | The profiles for profile guided optimization, one per edition |
| `tools/` | Code generation steps, build scripts, edition support |
| `docs/` | Documentation |

## Credits

- [NFSMW Recompiled](https://github.com/madelrandel-blip/NFSMW-Recompiled) by madelrandel-blip, the recompilation
  project this port started from.
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) by Tom Clay, and [Xenia](https://xenia.jp), on which it is
  based.
- [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) by hedge-dev.
- [mesa-switch](https://github.com/danfromtico/mesa-switch) by danfromtico, and the Mesa, NVK and NAK developers.
- [devkitPro](https://devkitpro.org) and [libnx](https://github.com/switchbrew/libnx).
- The other libraries listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

GPL-3.0, inherited from NFSMW Recompiled (see [LICENSE](LICENSE)). The SDK changes are under the SDK's BSD-3-Clause
license and the shader translator and Mesa changes under MIT, so other ports can reuse them. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Need for Speed and Need for Speed: Most Wanted are trademarks of Electronic Arts Inc. This project is not affiliated
with or endorsed by Electronic Arts, Nintendo or Microsoft.

## Support

If you want to support this work: [Ko-fi](https://ko-fi.com/stevenss) or [PayPal](https://www.paypal.com/paypalme/stevensnd).
