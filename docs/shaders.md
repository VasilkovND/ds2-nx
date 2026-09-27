# Shader translation

The native renderer draws with the game's own shaders, translated ahead of time from Xenos microcode
to SPIR-V. This page describes the translation path, the fixes that were needed for correctness, the
changes made for performance, and how each change was validated. The translator is
[XenosRecomp](https://github.com/hedge-dev/XenosRecomp); every change described here is kept under
the `NFSMW_RECOMP` define in [`shaders/`](../shaders) so that it can be compared with upstream.

## Path from the disc to the GPU

1. **Find the containers.** The game's shaders are stored as containers inside its data files and
   its executable. A scanner tests every byte position for the 2005 container signature
   (`10 2A 0E 00` for pixel shaders, `10 2A 0E 01` for vertex shaders) and accepts a match when its
   header sizes are consistent. This game has 204 containers on the disc and 3 in the executable.
   Names are assigned in the order they are found (`p_000123`, `v_000124`), with one counter across
   all files, so the scan order must never change.
2. **Convert the container.** Need for Speed: Most Wanted uses the **2005 XDK container layout**
   (24-byte header: flags, virtual size, physical size, then offsets of the definitions at +12, the
   constant table at +16 and the microcode at +20). XenosRecomp describes the later 2008 layout, so a
   small converter rewrites the header first. Older Xbox 360 games are likely to need the same.
3. **Translate** the microcode to HLSL with XenosRecomp and a shared header, `shader_common.h`.
4. **Compile** the HLSL to SPIR-V with DXC.
5. **Pack** all SPIR-V modules and the metadata the renderer needs into one library file
   (`nfsmw_shaders.nfsp`), indexed by a hash of the microcode.

The library is derived from the game's data, so it is not distributed. The installer web page runs
steps 1 to 5 in the browser (XenosRecomp, DXC and the packer compiled to WebAssembly) from the
player's own disc, and checks the SHA-256 of the result against the value expected for that edition.

At runtime the renderer finds the pixel shader of a draw by its microcode. **Vertex shaders cannot be
matched that way**: the game's Direct3D layer patches vertex shader microcode in place when it binds
it (fetch instructions reordered, swizzles rewritten for the vertex declaration, outputs the pixel
shader does not read replaced). The renderer therefore hooks the creation functions to map each
shader object to its original container. See [native-renderer.md](native-renderer.md).

Draw-time variations are selected with **specialization constants** rather than separate shaders:
the alpha test function, whether constants come from a uniform buffer, whether the inverse texture
size is available, and a few effect switches. The pipeline key includes the specialization mask.

## Correctness fixes

Each of these produced a visible defect before it was found.

- **Screen-space draws.** Videos and 2D draws are issued with clipping and the viewport transform
  disabled (`PA_CL_VTE_CNTL = 0x400`), so their positions are already in pixels. Vulkan always clips
  in normalised coordinates. The translator now emits
  `oPos.xy = oPos.xy * g_NdcScale + g_NdcOffset * oPos.w`, with the scale and offset in shared
  constants set by the renderer for each draw.
- **Normals, tangents and binormals.** XenosRecomp declared these inputs as `uint4` and read them with
  `asfloat` unless the format was 10_11_11. This game stores them as 16-bit integers or floats, so
  the directions came out degenerate: a flat one-colour rear-view mirror and a car body with no liveries.
  They are now declared `float4`, and the renderer uses SNORM, UNORM or float vertex formats.
- **`PRED_SET_INV`.** The scalar `SetpInv` opcode was translated without its `src == 1` case. Xenos
  computes `p0 = (a == 1); result = p0 ? 0 : (a == 0 ? 1 : a)`. The wrong predicate skipped blocks in
  the roadside grass shaders, which are drawn as stacked shells, and left a magenta ribbon along the
  road. When a single material shows impossible colours, compare every operation with the reference
  semantics (Xenia's `ucode.h` and translators) before debugging the renderer.
- **Alpha test.** All eight comparison functions, selected by a shared constant.
- **Depth-only draws.** With `RB_MODECONTROL` in depth mode, Xenos draws without a pixel shader. Some
  shadow casters are submitted with pixel shader object 0 and were being skipped.

## Performance changes

On the console the GPU is limited by fragment shading at 307.2 MHz (see
[performance-history.md](performance-history.md)), so translator output quality matters directly.

### Constants through a uniform buffer

The translated shaders read every constant through a 64-bit pointer (`RawBufferLoad`), which NAK
compiles to global memory loads: 12 to 81 per pixel shader. NVK promotes dynamic uniform buffers to
the hardware constant bank, also before Turing. Reading constants from a dynamic uniform buffer
instead:

- raised race frame rate by **18-23 %** in an A/B test alternating every 30 s in the same race (36 to
  43 FPS in that session; real GPU time per frame from 25.9 to 21.4 ms);
- cut frames above 33 ms from 13-15 % to 1-2 %;
- cost 1.1 µs of CPU per draw, for binding the extra descriptor set.

The change was validated statically first: all 3,211 constant accesses in the library were checked to
read the same data through both paths. A validation layer run caught that the upload buffer lacked
`UNIFORM_BUFFER_BIT`.

### Inverse texture size

The `tfetch2D` helper asked the texture for its size on every sample with an offset. Across the 109
pixel shaders, 160 of 435 texture instructions were size queries that draw nothing. The renderer now
writes `1 / size` for each texture slot into the shared constants and the shader uses it behind a
specialization bit: 160 queries became 0 with the same 435 samples. Two details matter: the size must
be the **host** image size (block-compressed images are rounded up to multiples of 4), and a ternary
in HLSL evaluates both branches, so the specialised path needs `[branch] if / else` for the unused
branch to disappear.

### Predicated blocks

XenosRecomp emitted one `if (p0)` per predicated instruction. The nine samples of a 3x3 PCF shadow
lookup ended up in nine basic blocks, and the GPU paid the texture latency nine times instead of
once. Consecutive instructions under the same predicate now share a block, closed right after any
instruction that writes `p0`: **1,806 `if (p0)` became 641** across 207 shaders, with 16 % fewer SPIR-V
basic blocks in the expensive ones.

### `max(a, a)` as a move

Xenos has no move instruction; assemblers write a move as `MAX dst, src, src`. The library contained
727 `max(a, a)` in pixel shaders and 482 in vertex shaders. They reached the hardware as real
multiplications: NIR rewrites `fmax(a, a)` as `fcanonicalize(a)`, and lowers that to `fmul(a, 1.0)`
unless the backend declares `has_fcanonicalize`, which no Mesa backend does. The translator now emits
the operand directly when both operands are the same expression. The result is identical, NaN and
signed zeros included; only the flushing of subnormals is lost, which Xenos applied to every
operation anyway. The general lesson: do not assume the driver cleans up obvious patterns. Look for
the rule in `nir_opt_algebraic.py` and check what it depends on.

### Shadow map passes

- When a pass has no colour attachment and the pixel shader cannot discard or write depth, the
  pipeline is built **without a fragment stage**. Only 21 of the 89 pixel shaders can discard;
  XenosRecomp adds the alpha-test `clip` to all of them.
- When the pixel shader must run only for its alpha test, a variant compiled from SPIR-V without the
  colour writes lets the driver remove everything that only fed the colour. This saved about 4 %;
  the shadow map shader turned out to be small already.

## A driver bug found through the translator: NAK on Maxwell

On the Switch the sea was black in one area of the map and correct on the PC. The water shader does
`saturate(r8.x - 0.4)`. The constant does not fit in a 20-bit float immediate, so NAK chose
`FADD32I`, which has no saturate bit on SM50, and silently dropped the `.SAT`. The fix legalises the
operation in NAK's SM50 backend (see [mesa.md](mesa.md)).

It was found by extracting the compiled SM50 binary from the application's pipeline cache and
disassembling it, after diagnostic shaders had shown that everything but the final colour was right.
Any fix to the shader compiler must also bump the compiler revision reported in the driver's
compiler flags; on Horizon the build ID is the package version, so otherwise the console keeps
using the old binaries from both shader caches.

## Validating translator changes

- **Predicate generations.** For every instruction of every shader, compute which write of `p0`
  governs it (the number of writes before the `if` that wraps it). If the numbers are identical in
  both versions of the output, the transformation cannot change the result. This proved the block
  merging correct on all 207 shaders.
- **Static equivalence of memory accesses**, as done for the constant buffer change.
- **Image comparison in a paused race**, alternating the setting every few seconds in the same
  session and comparing only pixels that do not change within each mode (see
  [measuring.md](measuring.md)).
- **Diagnostic shaders.** Replace one output with bands of intermediate values to see which stage
  goes wrong on the console.
- **Count instructions on specialised SPIR-V**, with the specialization masks the pipelines really
  use. Counting on unspecialised modules inflated the numbers five to ten times.
- **Track the library hash** everywhere a library is used. One diagnostic session on the PC was
  lost to an old library that drew the rear-view mirror with radial stripes.

## Measuring where fragments go

Pipeline statistics queries per pass, and one query per draw in one frame every few seconds, showed
which shaders fill the screen. For example, one smoke and light shaft effect drawn as five stacked
full-screen rectangles took 27-37 % of all scene fragments in half a draw per frame. Three pitfalls:

- two queries of the same type active at once (a pass query around draw queries) return garbage
  without any error;
- a measurement window tied to the rotation of the upload buffer catches a fraction of a frame, because
  the buffer rotates several times per frame;
- opening the window from the presentation thread catches a biased piece of the frame. Open it where
  frames are recorded.
