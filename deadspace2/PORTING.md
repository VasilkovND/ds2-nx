# Porting notes: Dead Space 2

Dead Space 2 (Xbox 360, Title Update 2) on the Switch. This file records what was done for this game, in
the order it was done. The Switch layer and the deko3d renderer in `sdk/` come from the NFSMW-NX port this
repository was forked from.

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
| 2 | Not stopped: menus and gameplay run (music, menu and in-game sounds), 28-30 swaps/s, up to ~820 draws per frame, XMA audio decoding, saves listed. Screen black in menus, white in game | Expected with the deko3d stand-in: only resolves/clears reach the renderer (4-9 copies per frame), geometry draws are counted but not decoded (`ds2_gpu.cpp`, PM4_DRAW_INDX), so what is presented is a cleared target | Next: the renderer (see below) |
| 3 (two runs, shader dump on) | Not stopped. Run 1 froze for ~40 s once loading started (after the intro video) | The dump wrote all ~1,300 containers to the SD synchronously on the loading thread, ~30 ms each. Run 2 found them on the SD (4 new) and did not freeze | One-off; run 1 was closed mid-write and left one truncated file (`ps_4cbaa81c381c8cad.bin`, 1024 of 1980 bytes) that the dump never rewrites: delete it before the next dump run |

Boot 3 measurements (run 2, ~6 min: menus, intro, early chapter 1; stack sampling was off):

- **Shaders.** DS2 creates its whole shader set at startup, in one burst on a loading thread: 1,303 containers
  (161 VS, 1,142 PS), and the minutes of gameplay after it added none. All are the 2008 format XenosRecomp reads as
  is (`0x102A1100` PS, `0x102A1101` VS; constant tables from compiler 2.0.20209.0), so no converter for an older
  container header is needed. 1,284 distinct microcodes; 110 KiB of VS and 480 KiB of PS microcode.
- **First pass through the shader tools** (on a Mac: `shaders/ds_hlsl.cpp` built with Apple clang, DXC 1.9.5512
  and spirv-val from Vulkan SDK 1.4.363.0, the deko3d flags now in `shaders/build_library.sh`). HLSL: 1,302 of 1,302 translated. SPIR-V: **1,302 of 1,302** compile and pass spirv-val
  (deko3d flags; the 19 shaders below also for the Vulkan path). The first pass rejected 19, for gaps in
  `XenosRecomp/shader_recompiler.cpp`, now fixed there (all at the end of `recompile()` or in the texture fetch):
  - 9 PS use GPRs above r31 (up to r62); only r0-r31 were declared. Now the body is scanned and r32-r127 that
    it uses are declared next to the others.
  - 8 PS fetch `g_envSampler` as a cube map while the constant table says `Sampler2D`, so the deko3d binding was a
    Texture2D and `_TextureCubeDescriptorIndex` was undefined. When every fetch of a sampler is a cube and its
    binding is 2D, the binding becomes a TextureCube (without the 2D pair). **The renderer must bind a cube
    there**, not what the constant table says.
  - 2 VS fetch texture constant 17 for `modulate_sampler_vs` = s1: vertex textures sit at fetch constants
    16 + N. The lookup now tries `constIndex - 16` in vertex shaders. These fetches then hit `Sample` (needs
    derivatives, pixel shaders only), so vertex 2D fetches call `tfetch2DVertice` (SampleLevel at mip 0), written
    into that shader after `shader_common.h` only when it is used.
  Each fix only acts on a case that made DXC reject the shader. Checked on DS2: the 1,283 shaders that compiled
  before give byte-identical HLSL and SPIR-V. Not checked yet: the LOD vertex fetches really use (mip 0
  assumed), and whether the `shader_common.h` conventions inherited from the NFS port fit DS2's shaders.
- **DKSH (deko3d's compiler, uam)**, with `shaders/dksh` and the default variant set (every VS, every PS with
  the renderer's specialization values): 10,642 distinct variants, **10,615 compile**, 0 rejected, 27 out of time: all 9
  variants of the only 3 shaders whose control flow XenosRecomp could not keep structured (`while (true)` +
  `switch (pc)`): `ps_22ae31642e2730a7`, `ps_ae3ade667ec0386b`, `ps_d58e4007116e7306`. Their GLSL is ~450 KB (a
  loop copying dozens of values per edge) and uam's register allocator (`nv50_ir::GCRA`) runs for minutes and
  grows to several GB on them (one sat over nine minutes before it was stopped). On the console the runtime
  compile worker would hang the same way.
  **Fixed in the translator: now 10,642 of 10,642 compile** (whole run 17 s; p50 9.2 ms, p99 87 ms, max 0.44 s per
  variant; cache 20.8 MB, `shadercache_dksh.bin` from `ds2_shaders.dslib`, 11.6 MB). Those three shaders
  are if / else-if ladders, one per light type (`g_WorldLightParameters(N).x`): a conditional jump to the
  `else`, and an unconditional jump forward to the join at the end of each branch. XenosRecomp only nests
  conditional forward jumps, so any unconditional one sent the whole shader to `while (true) switch (pc)`; the
  loop's back edge is what made SPIRV-Cross copy every live value on every edge. Every jump in them goes
  forward and there is no loop, so with `ShaderRecompiler::saltosLineales` (`ds_hlsl ... --saltos-lineales`)
  each control flow instruction becomes `if (pc <= N) { ... }` in order, a jump only raises pc, and the end of
  the shader is a `return`: same semantics, no loop. GLSL ~450 KB -> 91 KB, uam 0.23-0.39 s per variant. Off by
  default: without the option all 1,302 DS2 shaders translate byte for byte as before; with it, only those
  three change. **DS2's library has to be built with `--saltos-lineales`** (`build_library.sh` does).
  Tool changes made for this: `shaders/dksh` builds again after the GLSL/DKSH code moved into the SDK
  (`sdk/src/ui/deko3d`); the library (`ds_shader_library.cpp`) and `ds_empaquetar` accept 2008 containers;
  `ds_dksh` compiles each variant in its own process, on every core (`--hilos`), kills one after `--limite`
  seconds (120 by default) and reports `ok` / `fallo` / `tiempo` / `senal` per variant (whole run: 6 min on a Mac
  instead of a hang).
- **Frame rate.** 30 swaps/s in menus and the first scenes (70-150 draws per frame there), then 14-16 swaps/s in
  heavier gameplay, at 2,700-3,100 draws per frame. That is without drawing anything, so it is CPU time.
- **CPU.** Job Manager is at 96-99 % only during loading screens and videos, and at 1.7 % in gameplay: its busy wait
  costs load time, not frame rate. In gameplay the main thread is at 70-80 %, EARSJob×4 at 40-50 % together,
  everything else ~20 %, ~200-250 % of 300 % in total. RwAudioCore Dac (35-40 % in menus) goes to 0 in game, when
  EARS takes over audio. Next measure: stacks of the main thread at 15 fps (`perfil_pilas.flag`).
- **Backend warnings**, once each: render target format (causa 114, on the first in-game load), copy format
  (causa 204), copy to a 3D/array texture (causa 3). Resolves are formats 6 (1280x720, 1280x360, 640x360,
  640x180) and 22 (depth, 640x360, 960x960 - shadow map size).
- **SD reads.** Occasional 20-60 ms small reads of the bigfiles (`LENTO`); no stalls seen in the swap counts.

Boot 1 also confirmed: TU2 applied (`0.0.0.3 -> 0.0.2.3`), both disc folders identified and mounted, deko3d
presenter and ring reader up.

## Renderer: the plan

What other Xbox 360 -> Switch ports do (checked October 2026): NFS: Most Wanted (where this renderer comes
from), GTA IV (kabba536, on LibertyRecompAndroid), Mushihimesama Futari (ChanseyIsTheBest, from masseffect-nx)
all draw the game's PM4 ring with a native renderer, Vulkan on NVK or deko3d, and none runs Xenos emulation.
Their shaders are always translated ahead of time (XenosRecomp -> DXC -> SPIR-V). The renderers themselves
are game-specific on top of a common core (GTA IV's is ~70,000 lines of RAGE knowledge); the reusable core
here is the SDK's `rex::x360` deko3d backend, fed by a ring reader (`src/ds2_gpu.cpp`).

1. **Shaders** (`src/ds2_shader_dump.cpp`). DS2 keeps them in RefPack-compressed entries of the VIVs, so
   a scan of the disc for container signatures finds only 129 in 6.6 GB. Instead the XDK's
   `D3DDevice_CreateVertexShader` (0x82220568) / `D3DDevice_CreatePixelShader` (0x822201C0) are wrapped:
   every container the game creates is written to `shaders_dump/`, keyed by XXH3 like XenosRecomp. Play
   through menus and a few chapters, then build the library from that folder (`shaders/build_library.sh`,
   see [shaders/README.md](../shaders/README.md)); the console loads it as `ds2_shaders.dslib`
   (`src/ds2_shaders.cpp`).
2. **Ring decoder** (`src/ds2_gpu.cpp`, deko3d build): register mirror, IM_LOAD / IM_LOAD_IMMEDIATE
   identify the translated shader (`src/ds2_shaders.cpp`), draws -> `PeticionDibujo` for `RenderDk`, resolves
   and clears -> its `Copiar`. In progress.
3. **Render targets and formats** the backend does not handle yet (boot 2: "formato de destino de render
   todavia no soportado", causa 114).

## 1. Where the code comes from

This folder does not run codegen. Dead-Space-2-Recomp already did
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
SDK has the libnx equivalent (`REX_PLATFORM_SWITCH`), already proven by the port this repository was forked from.

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
sdmc:/switch/ds2-nx/          (the NRO's folder)
    ds2-nx.nro
    ds2_shaders.dslib     shader library (shaders/build_library.sh)
    shadercache_dksh.bin  DKSH cache (DKSH=1 shaders/build_library.sh)
    tu/       default.xex (Disc 1) + default.xexp (TU2)    -> game:
    disc1/    extracted Disc 1                             -> d:, \Device\Cdrom0
    disc2/    extracted Disc 2 (swapped in at chapter 4 by XamSwapDisc)
```

An empty `telemetria.flag` next to the NRO turns on automatic screenshots every 5 s, frame dumps and a 1 ms CPU
stack sampler. It costs performance: debugging only.

`tools/stage_sd.py --disc1 <iso> [--disc2 <iso>] --tu <TU2 package or xexp> -o <dir>/ds2-nx` makes it and
checks every hash. The disc swap follows the PC build: `XamSwapDisc`'s import (`0x82B0857C`) is bound to
a handler that re-points `d:` and signals the game's event.

## 4. Build

On a Mac: `deadspace2/tools/build_mac.sh` does every step below (devkitPro from its macOS installer, then
`sudo dkp-pacman -S switch-dev deko3d`), including applying `sdk-patches/` to `sdk/`. Output:
`deadspace2/out/sw/deadspace2.nro`.

Toolchain: devkitA64, libnx, deko3d, CMake, Ninja, Python 3.10+. No host codegen step.

```sh
git clone https://github.com/bifftannen1/Dead-Space-2-Recomp ../Dead-Space-2-Recomp
cmake -S deadspace2 -B deadspace2/out/sw -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=tools/switch/cmake/switch-devkitA64.cmake \
  -DREXSDK_DIR=$PWD/sdk -DDS2_RECOMP_DIR=$PWD/../Dead-Space-2-Recomp \
  -DREXGLUE_SWITCH_DEKO3D=ON
cmake --build deadspace2/out/sw -j 4
```

With `-DREXGLUE_SWITCH_DEKO3D=ON` the build uses `src/ds2_gpu.cpp` (first written as a GPU stand-in for a
Halo 3 port): it services the PM4 ring, and draws through the SDK's deko3d backend with the shader library.
The CMake file still has the other option, `-DREXGLUE_SWITCH_NVK_SDK=<Mesa NVK SDK>` (ReXGlue's Xenos
emulation on NVK), which is not used here.

Size: `src_tu2` is about 235 MB of C++ in 157 units. On ARM64 with GCC 14 `-O2`, a unit takes ~30-45 s
on one core.

## 5. What to expect next (from the other ports)

In the order a Halo 3 port met them; DS2's PC work removes some:

- **Missing functions and jump tables** — already fixed in `src_tu2` on the PC.
- **Memory mappings** (`2001-0103`), **three cores, not six hardware threads**, **busy waits** — Switch
  problems; expect them. EA's job manager (`EA/job_manager`) spawns worker threads per hardware thread.
- **Fibers**: not used. Nothing in `src_tu2` (nor its symbol map) names `SwitchToFiber`,
  `CreateFiber` or `ConvertThreadToFiber`, so Halo 3's `[rexcrt]` fix is not needed.
- **setjmp/longjmp**: DS2's PCH has `ppc_longjmp`; `non_volatile_as_local`-style options are off in the
  checked-in code, so nothing to undo.
- **Renderer**: Xenos emulation would be a few frames per second, so the native renderer on the PM4 ring
  (`rex::x360` in the SDK) is the road taken.
