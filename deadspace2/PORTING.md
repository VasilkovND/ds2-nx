# Porting notes: Dead Space 2

Dead Space 2 (Xbox 360, Title Update 2) on the Switch. Read
[docs/porting-another-game.md](../docs/porting-another-game.md) first; this file records what is
specific to this game, in the order it was done.

## Status

| Step of the general route | State |
| --- | --- |
| 1. Running on a PC with the recompiler | **Done upstream**: [Dead-Space-2-Recomp](https://github.com/bifftannen1/Dead-Space-2-Recomp) (PC playtest, TU2) |
| 2. Moved to the Switch system | Started: the game folder, SD layout and build glue are here; the recompiled tree compiles for ARM64 with GCC 14 (`-O2`, every unit). Not linked with devkitA64 or run on the console yet |
| 3. First frames | Two options in CMake: NVK + ReXGlue's Xenos emulation (draws, slow), or deko3d with the GPU stand-in (no draws) |
| 4.–6. Shaders, measuring, speed | Not started |

## Boots on the Switch so far

| Boot | Where it stopped | Cause | Fix |
|---|---|---|---|
| 1 | ~2 s in, right after the GPU ring started: `AllocFixed: no se pudo confirmar 0x1000 bytes ... Fallo 0x0000CE01; respaldo 465 MB` repeated, then the game hung | The game commits physical memory 4 KB at a time (from the top). Each commit was its own chunk, i.e. its own code-memory mapping, and Horizon ran out of memory blocks (2001-0103) long before memory | `sdk-patches/0001-commit-granule.patch`: commit physical memory in 4 MB chunks (as the fault path does) and virtual memory in 64 KB |

Boot 1 also confirmed: TU2 applied (`0.0.0.3 -> 0.0.2.3`), both disc folders identified and mounted, deko3d
presenter and ring reader up.

## 1. Where the code comes from

Unlike `nfsmw_pal/` and `halo3/`, this folder does not run codegen. Dead-Space-2-Recomp already did
step 1 and checked in its output, `src_tu2/`: all 47,739 functions of the TU2 executable, translated by
ReXGlue v0.10.0 (plus its `sdk/rexglue_ds2.patch`), named and laid out after the game's prototype PDB,
with its jump tables, missing functions and the rest already fixed on the PC. That work is the hard part
of a port, so this folder builds that tree as it is. `DS2_RECOMP_DIR` points at a checkout of it.

**Why it can be built against this SDK:** both are ReXGlue v0.10.0. The generated code only touches the
SDK through `rex/ppc/*.h` and its PCH; rexglue-sdk-nx only adds to those headers (new fields in
`PPCContext`, more exact vector helpers), it does not rename or remove anything the code uses.
Checked by compiling every unit against `sdk/include` for aarch64 (below).

**Why not the Switch layer inside Dead-Space-2-Recomp's patch:** `rexglue_ds2.patch` has its own
`REX_PLATFORM_NX` layer (memory, threads, audio, input, a Vulkan surface). It is written against Nintendo's
official SDK (`nn::os`, `nn::audio`, `nn::hid`, nx-clang), which homebrew cannot use. This repository's
SDK has the libnx equivalent (`REX_PLATFORM_SWITCH`), already proven by NFS: Most Wanted.

## 2. The PCH

The generated files include `ds2_rex_diagnostic_pch.h` with quotes, so they always find the copy next to
them. `tools/prepare_tree.py` (run by CMake at configure time) mirrors `src_tu2/` as symlinks into the
build folder and writes a different PCH there with `tools/make_nx_pch.py`. Two changes:

- **`DEFINE_REX_FUNC` for GCC.** DS2's PCH was rendered from upstream's template, which puts the
  `alias`/`weak` attributes before `extern "C"`. GCC (devkitA64) drops them there silently and the link
  fails with every `sub_` name undefined. rexglue-sdk-nx's template has the GCC branch; it is copied in.
- **`REX_RAW_ADDR_W(x, n)`.** DS2's codegen writes it for stores (its patch adds a store hook for the
  official-SDK memory model). Here it is `REX_RAW_ADDR(x)`.

## 3. Discs and title update

The recompiled code is TU2's, so it only matches the TU2-patched image: `default.xexp` of Title Update 2
is required, as on the PC build. Hashes (the same pins as the PC launcher):

| File | SHA-256 |
| --- | --- |
| Disc 1 `default.xex` (World, En/Fr/Es; Media ID `2915C8F8`) | `928597ddb80240b8aefeef0a70a9476ee85d6de733035b326dd155c2094a3408` |
| TU2 `disc001/default.xexp` | `8d386d37e8f71937b9d9c4b34bdbf85240101479c8aaece1002d94e7cdc01ec2` |

The PC build mounts the ISOs with `DiscImageDevice`, which maps the whole image in memory; the Switch
cannot (see `sdk/include/rex/filesystem/disc_image_extract.h`). So the discs are extracted folders,
mounted with `HostPathDevice` (`src/ds2_disc_swap.cpp`). Both bigfiles are under 4 GB, so FAT32 works.

```
sdmc:/ds2-nx/
    ds2-nx.nro
    tu/       default.xex (Disc 1) + default.xexp (TU2)    -> game:
    disc1/    extracted Disc 1                             -> d:, \Device\Cdrom0
    disc2/    extracted Disc 2 (swapped in at chapter 4 by XamSwapDisc)
```

`tools/stage_sd.py --disc1 <iso> [--disc2 <iso>] --tu <TU2 package or xexp> -o <dir>/ds2-nx` makes it and
checks every hash. The disc swap follows the PC build: `XamSwapDisc`'s import (`0x82B0857C`) is bound to
a handler that re-points `d:` and signals the game's event.

## 4. Build

On a Mac: `deadspace2/tools/build_mac.sh` does every step below (devkitPro from its macOS installer, then
`sudo dkp-pacman -S switch-dev deko3d`).

Same toolchain as the other games ([docs/building.md](../docs/building.md)): devkitA64, libnx, CMake,
Ninja, Python 3.10+. No host codegen step.

```sh
git clone https://github.com/bifftannen1/Dead-Space-2-Recomp ../Dead-Space-2-Recomp
cmake -S deadspace2 -B deadspace2/out/sw -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=tools/switch/cmake/switch-devkitA64.cmake \
  -DREXSDK_DIR=$PWD/sdk -DDS2_RECOMP_DIR=$PWD/../Dead-Space-2-Recomp \
  -DREXGLUE_SWITCH_NVK_SDK=/path/to/mesa-sdk/opt/devkitpro/portlibs/switch
cmake --build deadspace2/out/sw -j 4
```

With `-DREXGLUE_SWITCH_DEKO3D=ON` (and no NVK SDK) the build uses `src/ds2_gpu.cpp`, the GPU stand-in
copied from Halo 3: the game runs past its first frame and logs swaps, draws and packets, nothing is
drawn. That is the cheapest way to check that the game logic boots before building Mesa.

Size: `src_tu2` is about 235 MB of C++ in 157 units. On ARM64 with GCC 14 `-O2`, a unit takes ~30-45 s
on one core.

## 5. What to expect next (from the other ports)

In the order Halo 3 met them ([halo3/PORTING.md](../halo3/PORTING.md)); DS2's PC work removes some:

- **Missing functions and jump tables** — already fixed in `src_tu2` on the PC.
- **Memory mappings** (`2001-0103`), **three cores, not six hardware threads**, **busy waits** — Switch
  problems; expect them. EA's job manager (`EA/job_manager`) spawns worker threads per hardware thread.
- **Fibers**: not used. Nothing in `src_tu2` (nor its symbol map) names `SwitchToFiber`,
  `CreateFiber` or `ConvertThreadToFiber`, so Halo 3's `[rexcrt]` fix is not needed.
- **setjmp/longjmp**: DS2's PCH has `ppc_longjmp`; `non_volatile_as_local`-style options are off in the
  checked-in code, so nothing to undo.
- **Renderer**: Xenos emulation will be a few frames per second, as it was for NFS. A native renderer on
  the PM4 ring (`rex::x360` in the SDK) is the long road.
