# Mesa, NVK and NAK on Horizon

The native renderer speaks Vulkan to NVK, the open source Vulkan driver for NVIDIA GPUs in Mesa. On the Switch it
runs through [danfromtico/mesa-switch](https://github.com/danfromtico/mesa-switch), which adds a Horizon backend for
nouveau (channels, memory through NvMap, fences) and builds a static `libvulkan.a` for libnx programs. The driver
is linked into the NRO; NAK, the shader compiler, is linked into `libvulkan.a`.

Everything this port changed is in [mesa/mesa-switch-nfsmw.patch](../mesa/mesa-switch-nfsmw.patch), against
commit `1a8c1a66d6f`. This page explains the changes and what they were worth. [mesa/README.md](../mesa/README.md)
has the file list and the rebuild steps.

## Building Mesa for Horizon on Windows

mesa-switch expects a Linux container. It also builds natively on Windows with MSYS2, which is how every driver
used by this port was built:

- MSYS2 with the MINGW64 environment and `mingw-w64-x86_64-{gcc,clang,llvm,spirv-llvm-translator,pkgconf,cmake,
  python,python-packaging,python-setuptools,python-yaml,meson,ninja}`, plus `flex`, `bison` and `git`. It does not
  touch devkitPro.
- Rust with the GNU host toolchain (`stable-x86_64-pc-windows-gnu`) and the `aarch64-unknown-linux-gnu` target, plus
  `bindgen` and `cbindgen`. The build directory must keep using the toolchain it was configured with: its proc
  macros are DLLs of that toolchain. `RUSTUP_TOOLCHAIN=stable-x86_64-pc-windows-gnu` pins it.
- Problems solved on the way, in the order they appear:
  1. `SOURCE_DATE_EPOCH` comes out empty when the tree is not a git checkout (a downloaded archive); export it.
  2. `ALLOW_DIRTY=1` for the same reason.
  3. The host cross file pointed at `clang64`; it has to match the MSYS2 environment in use (`mingw64`).
  4. Python modules `packaging`, `setuptools` and `yaml`.
  5. The rustc wrappers had the linker hardcoded to UCRT64; the patch makes them pick the one that exists.
  6. `cbindgen` pointed at an ARM64 MSYS2 environment.
  7. bindgen did not find the devkitA64 sysroot, and its layout tests fail on `__sFILE` (bindgen computes 184 bytes,
     the compiler 176) and on Linux DRM structures that do not exist on Horizon. The patch passes
     `--no-layout-tests` and declares `__sFILE` opaque: Mesa only uses it through pointers.

The install step leaves an SDK folder. The app points `REXGLUE_SWITCH_NVK_SDK` at its
`opt/devkitpro/portlibs/switch`. After replacing the libraries, delete `app/out/sw8/nfsmw` and `nfsmw.nro`: Ninja does
not track a static library outside the build tree and would keep linking the old driver.

## ZCULL

A race frame is dominated by pixel shading: at one point the scene pass alone was half of the GPU time, with
millions of fragments and a few hundred operations each. Anything that does not remove fragments or make them cheaper
barely moves the frame rate. ZCULL, the hierarchical depth test of the hardware, rejects hidden pixels before they are
shaded.

In mesa-switch it was compiled out for the Switch (`#ifndef __SWITCH__` in `nvk_image.c`). Enabling that code is not
enough; there were three independent blockers:

1. `has_zcull_info` was never true on the Vulkan path. `nvkmd_switch_pdev.c` used the static GM20B description; the
   function that asks the kernel for the ZCULL geometry was only called by the Gallium path.
2. Nothing bound a ZCULL context to the Vulkan 3D channel. `nouveau_horizon_channel_bind_zcull()` existed, but only
   Gallium called it.
3. Depth images that request `TRANSFER_DST` are not eligible. The renderer's depth images asked for it for a restore
   path that almost never runs; the renderer no longer requests it.

Two details matter: `LOAD_ZCULL` on uninitialized data kills the GPU context, so the ZCULL context buffer is zeroed;
and a quick way to check that ZCULL is alive without instrumentation is that the `VkMemoryRequirements.size` of a
depth image grows. With ZCULL on, the race scene got about 2 ms of GPU time cheaper. Once a depth image has its ZCULL
plane, the draw code uses it without requiring `loadOp = CLEAR`, so no render pass had to change.

## NAK

**Scheduling latencies.** NAK's instruction scheduler assumed 32 cycles for texture fetches and global memory loads.
On a SoC with shared LPDDR4 a texture miss costs hundreds of cycles. Raising the value to 200 lets the pre-pass
scheduler separate a fetch from its first use and keep more requests in flight. It only changes scheduling priority;
the scoreboard barriers are computed separately, so it cannot produce wrong code.

**Branch flattening.** NAK called NIR's `peephole_select` with a limit of 0, which only flattens `if`s with two empty
sides (ANV and RADV use 8). NAK has no if-to-predicate pass of its own, so every `if` that survives NIR becomes a real
branch (SSY/BRA/SYNC) on Maxwell. The limit is now 8. Flattening raises register pressure, and a spill to local
memory is expensive on GM20B: check with `NVK_SHADER_STATS=1` that the local memory size stays at 0.

**FADD32I and saturation (the black sea).** The water shader computes the foam of the waves with
`saturate(r8.x - 0.4)`. Maxwell floating point instructions only take 20-bit immediates and -0.4 (`0xBECCCCCD`) does
not fit, so NAK picked `FADD32I`, which has no saturate bit on SM50. The legalization in `sm50.rs` already avoided
`FADD32I` when a rounding mode was set, but not with `.SAT`, and the encoder dropped the flag silently. The foam
reached -0.4, was subtracted from the water color and the sea came out black with only fog on top. It was found by
disassembling the water pipeline from the app's pipeline cache: `FADD32I R8 = R18 + -0.4`, with no saturation after
it. The fix moves the long immediate to a register when the add saturates.

**Cache revision.** On Horizon the driver id is the package version, so after a compiler fix the pipeline cache UUID
and the disk cache keys stay the same and old, wrongly compiled binaries would be reused. `nvk_shader.c` hashes a
revision number of these NAK changes into the compiler flags; raise it with every NAK change.

**Closed doors.** FP16 at double rate exists on the Tegra X1, but NAK has no SM50 encoder for `HADD2`, `HMUL2` or
`HFMA2`, and `nak_nir.c` lowers 16-bit float to 32-bit below SM70; that is why NVK only exposes `shaderFloat16` on
Turing and later. Variable rate shading needs Turing too.

## Memory and synchronization

On Horizon, CPU mappings of GPU memory through NvMap are expensive to keep coherent: every write to a cached mapping
needs cache maintenance before the GPU sees it. Command buffers, streaming uploads and other CPU-write-only memory are
mapped uncached instead (`NVK_SWITCH_CMD_MEM_CPU_UNCACHED`, `NVK_SWITCH_MEM_STREAM_CPU_UNCACHED`,
`NVK_SWITCH_CPU_WRITE_MEM_UNCACHED`, all on by default). The patch also carries fixes from later mesa-switch commits,
among them: clean the GPU L2 only for fences the CPU waits on, no redundant clean of the descriptor tables on every
submit, query pools without GPU caching, and a GM20B hang with a global load inside a hardware loop.

See [platform-notes.md](platform-notes.md) for the costs of Vulkan calls on this platform (fences, submits, memory
allocation).

## The draw path

A race frame records around 2,000 draws on the CPU, and the thread that records them was the bottleneck for a long
time (see [native-renderer.md](native-renderer.md)). Part of the cost per draw is inside NVK: binding the pipeline,
the descriptor sets and the constant buffers, and emitting the state. The patch adds, for Horizon only:

- **A shared measurement structure** (`nvk_switch_dibujo`): versioned, filled by the driver and read by the app
  through weak symbols, so the app works with a driver that does not have it. It splits the cost of a draw into parts.
- **Cheaper emission** of the per-draw state, **fewer constant buffer rebinds** (38 to 45 % fewer in a race),
  **shortcuts for the dynamic state** the renderer uses, and a **prefetch** of the pipeline state that
  `vk_graphics_pipeline_cmd_bind` and the dynamic state copy read (`vk_pipeline.c`).
- **Set 4 by differences**: the renderer's per-draw dynamic uniform set is written as a delta against the previous
  draw (1 write instead of 4).

Each of these is requested by the app (cvars `nfsmw_nativo_nvk_*` and `nfsmw_nativo_set4_diferencias`) and has a
self-check in the driver. A negative result worth knowing: moving to extended dynamic state (EDS 1/2/3) with a
canonical pipeline key cut pipeline binds by only 13 % and made each bind slower (2.95 to 4.36 us per call), a net loss
on NVK/Maxwell. It is off.

## Other NVK ports

[NXVK](https://github.com/PalindromicBreadLoaf/nxvk) is another port of NVK to the Switch. It was compared file by file:
it uses a different kernel backend (`nvkmd/nvgpu`) without the memory fixes of the Horizon backend, and it publishes no
performance figures. The copy engine path and the scheduler latency change came from reading it; ZCULL was confirmed
by it (it has the same code enabled).
