# xbox360-nx

Xbox 360 games running natively on the Nintendo Switch. It's not an emulator: each game's PowerPC code is
statically recompiled to C++ with [ReXGlue](https://github.com/rexglue/rexglue-sdk) and built for the Switch, and
frames are drawn by a native renderer.

> [!NOTE]
> You need your own copy of each game. This repository contains no game data or game code.

## Games

| Game | Folder | Status |
| --- | --- | --- |
| Need for Speed: Most Wanted (2005) | [`nfsmw_pal/`](nfsmw_pal) | Playable, about 30 FPS ([details](README.nfsmw.md)). PAL Spanish; other editions (`nfsmw_usa/`, ...) are made from it and committed beside it ([editions](docs/editions.md)) |
| Halo 3 | [`halo3/`](halo3) | In progress, not playable yet |
| Dead Space 2 (TU2) | [`deadspace2/`](deadspace2) | Started: recompiled code from [Dead-Space-2-Recomp](https://github.com/bifftannen1/Dead-Space-2-Recomp) builds for ARM64; not run on the console yet ([notes](deadspace2/PORTING.md)) |

Every game is its own folder with its own NRO, and they all build on one shared SDK. A fix in `sdk/` reaches every
game. To add a game, see [porting another game](docs/porting-another-game.md).

## Layout

| Folder | Contents |
| --- | --- |
| `<game>/` | One per game: manifest, codegen fixes, hooks, renderer glue, CMake project |
| `sdk/` | **Submodule** → [`rexglue-sdk-nx`](https://github.com/nx-mod/rexglue-sdk-nx) (`switch` branch): the ReXGlue SDK + Horizon (Switch) layer + deko3d renderer, shared by every game |
| `shaders/` | Xenos shader translator and shader library tools |
| `tools/` | Codegen and build scripts |
| `docs/` | Documentation: start with [docs/README.md](docs/README.md) |

## Build

devkitA64 and libnx from [devkitPro](https://devkitpro.org), CMake, Ninja, Python, and your own copy of the game.
The steps are in [docs/building.md](docs/building.md).

## Credits

- **[StevensND](https://github.com/StevensND/nfsmw-nx)**: nfsmw-nx, the Switch port this repository is forked from:
  its Switch layer, renderer and documentation.
- **[madelrandel-blip](https://github.com/madelrandel-blip/NFSMW-Recompiled)**: NFSMW Recompiled.
- **[Tom Clay](https://github.com/rexglue/rexglue-sdk)**: the ReXGlue SDK, built on the work of the
  **[Xenia](https://xenia.jp)** team.
- ReXGlue forks whose codegen fixes are applied here, with credit in each commit:
  **[stuken](https://github.com/stuken/rexglue-sdk)** (D3D pack/unpack, vmsum, fctid/fres, FPSCR) and
  **[furqanagwan](https://github.com/furqanagwan/rexglue-sdk)** (stwcx. reservations, mffs, vsl).
- **[haloce-nx](https://github.com/nx-mod/haloce-nx)** (CC0): the first-launch copy of the game out of a disc image.
- **[summers-aaron](https://github.com/summers-aaron/xenon-pc-comp)**: xenon-pc-comp, Halo 3 on ReXGlue for Linux.
  Used as a reference for what Halo 3 needs; none of its code is copied.
- **[hedge-dev](https://github.com/hedge-dev/XenosRecomp)**: XenosRecomp.
- **[danfromtico](https://github.com/danfromtico/mesa-switch)** and **[NaGaa95](https://github.com/NaGaa95/mesa-switch)**:
  mesa-switch.
- **[centollOS](https://github.com/centollOS/SwitchWakerHD)**: the uam shader compiler patches.
- **[devkitPro](https://devkitpro.org) and [switchbrew](https://github.com/switchbrew/libnx)**: devkitA64 and libnx.
- The other libraries listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Legal

Unofficial fan ports with no affiliation to Microsoft, Bungie, Electronic Arts or EA Black Box. "Halo" is a trademark
of Microsoft; "Need for Speed" is a trademark of Electronic Arts. You provide your own legally obtained copy of each
game. Running homebrew requires custom firmware, which violates Nintendo's terms and can get a console banned.

Source code is under the GPL-3.0 license (see [LICENSE](LICENSE)), inherited from nfsmw-nx. The SDK changes are under
the SDK's BSD-3-Clause license, and the shader translator and Mesa changes under MIT.
