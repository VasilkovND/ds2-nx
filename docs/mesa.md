# The graphics driver: Mesa, NVK and NAK on the Switch

## In short

- The port draws with [Vulkan](glossary.md#vulkan). On the Switch, the Vulkan driver is [NVK](glossary.md#nvk), the
  open source driver for NVIDIA GPUs from the [Mesa](glossary.md#mesa) project. Its shader compiler is
  [NAK](glossary.md#nak).
- It runs on the Switch thanks to [danfromtico/mesa-switch](https://github.com/danfromtico/mesa-switch), which adds
  the Switch-specific part (it talks to the Switch's GPU driver: channels, memory through NvMap, fences) and builds one
  library, `libvulkan.a`, for homebrew programs. That library, with NAK inside, is included in the NRO.
- Everything this port changed is in one file, [mesa/mesa-switch-nfsmw.patch](../mesa/mesa-switch-nfsmw.patch),
  made against commit `1a8c1a66d6f` of mesa-switch.
- This page explains why each change was made and what it gained. [mesa/README.md](../mesa/README.md) has the steps to
  get the source, the list of files and how to rebuild.

## Building Mesa for the Switch on Windows

mesa-switch expects to be built in a Linux container. It also builds directly on Windows with MSYS2, and that is how
every driver of this port was built. You need:

- **MSYS2**, in its MINGW64 environment, with these packages:
  `mingw-w64-x86_64-{gcc,clang,llvm,spirv-llvm-translator,pkgconf,cmake,python,python-packaging,python-setuptools,
  python-yaml,meson,ninja}`, plus `flex`, `bison` and `git`. It does not touch devkitPro.
- **Rust**, with the Windows GNU toolchain (`stable-x86_64-pc-windows-gnu`), the `aarch64-unknown-linux-gnu` target,
  and the `bindgen` and `cbindgen` tools. A build folder must keep using the toolchain it was configured with, because
  some of its parts are libraries of that exact toolchain. Set `RUSTUP_TOOLCHAIN=stable-x86_64-pc-windows-gnu` to be
  sure.

Seven problems appeared on the way, in this order. The patch already solves the ones marked "patch"; the others you
solve when you build:

1. `SOURCE_DATE_EPOCH` comes out empty when the source is not a git checkout (for example a downloaded archive).
   Set it yourself.
2. For the same reason, set `ALLOW_DIRTY=1`.
3. The cross file for the PC pointed at the `clang64` environment; it must match the MSYS2 environment you use
   (`mingw64`).
4. Python needs the modules `packaging`, `setuptools` and `yaml`.
5. (patch) The rustc wrappers had the linker fixed to the UCRT64 environment; now they pick the one that exists.
6. `cbindgen` pointed at an ARM64 MSYS2 environment.
7. bindgen did not find devkitA64's system headers: add their folders to `c_args` in the cross file, and set
   `DEVKITPRO`. (patch) Its layout tests also failed on `__sFILE` (bindgen computes 184 bytes, the compiler 176) and on
   Linux DRM structures that do not exist on the Switch. The patch passes `--no-layout-tests` and declares `__sFILE`
   opaque: Mesa only uses it through pointers.

The install step leaves an SDK folder. The app is pointed at its `opt/devkitpro/portlibs/switch` subfolder with
`REXGLUE_SWITCH_NVK_SDK`. **After you replace the libraries, delete `app/out/sw8/nfsmw` and `nfsmw.nro`**: Ninja does
not notice a library that changed outside its build folder, and would keep the old driver.

## ZCULL: skipping hidden pixels

**Why it matters:** a race frame is dominated by shading pixels. At one point the scene pass alone was half of the GPU
time: millions of pixels, with a few hundred operations each. Anything that does not remove pixels, or make them
cheaper, barely changes the frame rate. [ZCULL](glossary.md#zcull) is the GPU's own early depth test: it throws away
pixels that are hidden behind others before their shader runs.

In mesa-switch, the ZCULL code was left out for the Switch (`#ifndef __SWITCH__` in `nvk_image.c`). Turning it back on
was not enough, because three separate things blocked it:

1. **The driver never knew the ZCULL sizes.** `has_zcull_info` was never true for Vulkan: `nvkmd_switch_pdev.c` used a
   fixed description of the GPU, and the function that asks the system for the ZCULL sizes was only called by
   Mesa's other (Gallium) path.
2. **Nothing connected ZCULL to the Vulkan drawing channel.** `nouveau_horizon_channel_bind_zcull()` existed, but only
   the Gallium path called it.
3. **Depth images that ask for `TRANSFER_DST` cannot use ZCULL.** The renderer's depth images asked for it for a
   restore path that almost never runs; the renderer no longer asks for it.

Two details matter:

- `LOAD_ZCULL` on uninitialized data kills the GPU, so the ZCULL buffer is filled with zeros first.
- A quick way to see that ZCULL is working, without any tools: the `VkMemoryRequirements.size` of a depth image gets
  bigger.

**What it gained:** with ZCULL on, the race scene got about 2 ms of GPU time cheaper per frame. Once a depth image has
its ZCULL part, the drawing code uses it without needing `loadOp = CLEAR`, so no render pass had to change.

## NAK: the shader compiler

**Waiting times for memory.** NAK orders the instructions of each shader so that the GPU does not wait. It assumed
that reading a texture or main memory takes 32 cycles. On the Switch, where the CPU and the GPU share slow memory
(LPDDR4), a texture read that misses the cache takes hundreds of cycles. With 200 instead of 32, NAK separates each
read from the first instruction that uses it, and keeps more reads going at once. It only changes the order: the waits
that make the code correct are computed separately, so it cannot produce wrong code.

**Small branches become straight code.** NAK called Mesa's `peephole_select` step with a limit of 0, which only
removes `if`s whose two sides are empty (the Intel and AMD drivers, ANV and RADV, use 8). NAK has no step of its own
to turn `if`s into conditional instructions, so on this GPU every `if` left became a real jump (SSY/BRA/SYNC). The
limit is now 8. Doing this uses more registers, and running out of registers (a "spill" to memory) is expensive on this
GPU: check with `NVK_SHADER_STATS=1` that the local memory size stays at 0.

**`FADD32I` and the clamp (the black sea).** The water shader computes the foam of the waves with
`saturate(r8.x - 0.4)`: subtract 0.4, then clamp the result to the range 0-1. On this GPU, floating point instructions
only take short 20-bit constants, and -0.4 (`0xBECCCCCD`) does not fit. So NAK chose `FADD32I`, which on this GPU
generation (SM50) has no clamp bit. The code in `sm50.rs` already avoided `FADD32I` when a rounding mode was set, but
not with a clamp (`.SAT`), and the clamp was silently dropped. The foam went down to -0.4, was subtracted from the
water color, and the sea came out black, with only the fog on top. It was found by taking the water pipeline out of
the app's pipeline cache and disassembling it: `FADD32I R8 = R18 + -0.4`, with no clamp after it. The fix puts the long
constant in a register when the add must clamp.

**Old compiled shaders must be thrown away.** On the Switch, the driver's version id is the package version. So after
a fix to the compiler, the id of the pipeline cache and the keys of the disk cache stay the same, and old, wrongly
compiled shaders would be reused. `nvk_shader.c` adds a revision number of these NAK changes to the compiler flags.
**Raise it with every change to NAK.**

**What is not possible.** The Tegra X1 can do half-precision math (FP16) at double speed, but NAK cannot write those
instructions (`HADD2`, `HMUL2`, `HFMA2`) for this GPU generation, and `nak_nir.c` turns 16-bit floats into 32-bit ones
on GPUs older than Turing (SM70). That is why NVK only offers `shaderFloat16` on Turing and later. Variable rate
shading needs Turing too.

## Memory and synchronization

On the Switch, keeping the CPU's cached view of GPU memory up to date is expensive: after every write through a cached
view, the CPU cache must be cleaned before the GPU sees the data. So the memory that the CPU only writes to (command
buffers, uploads) is used without the CPU cache (`NVK_SWITCH_CPU_WRITE_MEM_UNCACHED`, on by default). It replaces two
older variables, `NVK_SWITCH_CMD_MEM_CPU_UNCACHED` and `NVK_SWITCH_MEM_STREAM_CPU_UNCACHED`, which the driver no longer
reads.

The patch also brings fixes from later versions of mesa-switch, among them:

- clean the GPU's cache only for the fences the CPU waits for;
- no needless cleaning of the descriptor tables on every submit;
- query pools without GPU caching;
- a GPU hang with a memory read inside a hardware loop.

[platform-notes.md](platform-notes.md) explains what Vulkan calls cost on this platform (fences, submits, memory
allocation).

## The draw path: less CPU per draw

A race frame records about 2,000 draws on the CPU, and the thread that records them (the
[ring thread](glossary.md#ring-thread)) was the slowest part for a long time (see
[native-renderer.md](native-renderer.md)). Part of the cost of each draw is inside NVK: binding the pipeline, the
descriptor sets and the constant buffers, and writing the GPU state. The patch adds, only for the Switch:

- **A shared measurement structure** (`nvk_switch_dibujo`, "dibujo" means draw). The driver fills it and the app reads
  it, and it splits the cost of a draw into parts. It is versioned and found through weak symbols, so the app still
  works with a driver that does not have it.
- **Cheaper writing** of the state of each draw, **fewer constant buffer rebinds** (38 to 45 % fewer in a race),
  **shortcuts for the dynamic state** the renderer uses, and **early loading (prefetch)** of the pipeline data that
  `vk_graphics_pipeline_cmd_bind` and the dynamic state copy read (`vk_pipeline.c`).
- **Set 4 by differences.** The renderer's per-draw constants (descriptor set 4) are written as a difference against
  the previous draw: 1 write instead of 4.

The app turns each of these on with its own settings (`nfsmw_nativo_nvk_*` and `nfsmw_nativo_set4_diferencias`), and
each one checks itself inside the driver.

Tried and dropped: moving to Vulkan's extended dynamic state (EDS 1, 2 and 3) with a simpler pipeline key. It cut
pipeline binds by only 13 % and made each bind slower (from 2.95 to 4.36 µs per call): a loss on this GPU. It is off.

## Other NVK ports

[NXVK](https://github.com/PalindromicBreadLoaf/nxvk) is another port of NVK to the Switch. It was compared file by
file: it talks to the system in a different way (`nvkmd/nvgpu`), without the memory fixes of mesa-switch, and it
publishes no performance figures. The copy engine path and the change to the waiting times came from reading it, and it
confirmed the ZCULL work (it has the same code turned on).
