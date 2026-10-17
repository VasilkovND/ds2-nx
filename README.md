# halo3-nx

A native Nintendo Switch port of **Halo 3** (Xbox 360). It's not an emulator: the game's PowerPC code is statically
recompiled to C++ with [ReXGlue](https://github.com/rexglue/rexglue-sdk) and built for the Switch, and frames are
drawn by a native deko3d renderer.

> [!NOTE]
> Work in progress, not playable yet. You need your own copy of the Xbox 360 game.

## How it works

This repository is a fork of [nfsmw-nx](https://github.com/StevensND/nfsmw-nx), the Switch port of Need for Speed:
Most Wanted. Halo 3 sits beside it as a second game on the same Switch layer:

| Folder | Contents |
| --- | --- |
| `halo3/` | Halo 3: manifest, codegen fixes, game hooks |
| `app/` | Need for Speed: Most Wanted, from upstream ([its README](README.nfsmw.md)) |
| `sdk/` | ReXGlue SDK with the Horizon layer |
| `shaders/` | Xenos shader translator and library tools |
| `docs/` | Documentation, including [porting another game](docs/porting-another-game.md) |

## Credits

- **Bungie and Microsoft**: creators of Halo 3. This is an unofficial fan port with no affiliation.
- **[StevensND](https://github.com/StevensND/nfsmw-nx)**: nfsmw-nx, the Switch layer, renderer and docs this port
  is built on.
- **[Tom Clay](https://github.com/rexglue/rexglue-sdk)**: the ReXGlue SDK, built on the work of the
  **[Xenia](https://xenia.jp)** team.
- **[summers-aaron](https://github.com/summers-aaron/xenon-pc-comp)**: xenon-pc-comp, Halo 3 on ReXGlue for Linux.
  Used as a reference for what Halo 3 needs; none of its code is copied.
- **[hedge-dev](https://github.com/hedge-dev/XenosRecomp)**: XenosRecomp.
- **[centollOS](https://github.com/centollOS/SwitchWakerHD)**: the uam shader compiler patches.
- **[devkitPro](https://devkitpro.org) and [switchbrew](https://github.com/switchbrew/libnx)**: devkitA64 and libnx.
- The other libraries listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Legal

No affiliation with Microsoft or Bungie. "Halo" is a trademark of Microsoft. This repository contains no assets or
program code from the game; you provide your own legally obtained copy.

Source code is under the GPL-3.0 license (see [LICENSE](LICENSE)), inherited from nfsmw-nx. The SDK changes are under
the SDK's BSD-3-Clause license.
