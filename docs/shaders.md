# How the game's shaders are translated

## In short

- The game's [shaders](glossary.md#shader) are small GPU programs written for the Xbox 360 GPU. The Switch GPU cannot
  run them, so they are translated before playing into [SPIR-V](glossary.md#spir-v), the format Vulkan uses.
- The translator is [XenosRecomp](glossary.md#xenosrecomp). This port's version is in [`shaders/`](../shaders), and
  every change is inside code marked `NFSMW_RECOMP`, so it can be compared with the original tool.
- This page explains the path from the disc to the GPU, the fixes needed for the image to look right, the changes
  that made the shaders faster, and how each change was checked.

## From the disc to the GPU

1. **Find the shaders on the disc.** They are stored in "containers" inside the game's data files and its executable.
   A scanner looks at every byte position for the container signature (`10 2A 0E 00` for pixel shaders, `10 2A 0E 01`
   for vertex shaders), and accepts a match when its header sizes make sense. This game has 204 containers on the disc
   and 3 in the executable. They are named in the order they are found (`p_000123`, `v_000124`), with one counter for
   all files, so the order of the scan must never change.
2. **Convert the container format.** This game uses the 2005 container format: a 24-byte header with flags, virtual
   size and physical size, then where the definitions (+12), the constant table (+16) and the microcode (+20) start.
   XenosRecomp expects the later 2008 format, so a small converter rewrites the header first. Older Xbox 360 games
   probably need the same.
3. **Translate** the [microcode](glossary.md#microcode) to [HLSL](glossary.md#hlsl) with XenosRecomp and a shared
   header file, `shader_common.h`.
4. **Compile** the HLSL to SPIR-V with [DXC](glossary.md#dxc).
5. **Pack** all the SPIR-V shaders, and the extra information the renderer needs, into one file: `nfsmw_shaders.nfsp`,
   the [shader library](glossary.md#shader-library). Each shader is found in it by a fingerprint (a hash) of its
   microcode.

The library is made from the game's data, so it cannot be shared. The installer page does steps 1 to 5 in the browser,
from the player's own disc: XenosRecomp, DXC and the packer are compiled to WebAssembly so that a browser can run them.
The page then checks the SHA-256 of the result against the value expected for that edition.

While playing, the renderer finds the pixel shader of each draw by its microcode. **Vertex shaders cannot be found
that way**: when the game's Direct3D uses a vertex shader, it changes its microcode on the spot (it reorders the
instructions that read the vertices, rewrites how they read them for each vertex layout, and replaces the outputs the
pixel shader does not use). So the renderer [hooks](glossary.md#hook) the functions that create shaders and notes which
original container each one comes from (see [native-renderer.md](native-renderer.md)).

Small variations of a shader are chosen with **specialization constants** (values fixed when the pipeline is created)
instead of separate shaders: the alpha test comparison, whether the constants come from a buffer, whether 1/size of the
textures is available, and a few effect switches. They are part of the pipeline key.

## Fixes needed for a correct image

Each of these caused a visible defect before it was found.

- **Drawing in screen pixels.** Videos and 2D elements are drawn with the GPU's clipping and viewport transform turned
  off (`PA_CL_VTE_CNTL = 0x400`), so their positions are already in pixels. Vulkan always expects coordinates from -1
  to 1. The translator now writes `oPos.xy = oPos.xy * g_NdcScale + g_NdcOffset * oPos.w`, with the scale and the
  offset set by the renderer for each draw.
- **Normals, tangents and binormals** (the directions used for lighting). XenosRecomp read them as whole numbers
  (declared `uint4` and read with `asfloat`) unless their format was 10_11_11. This game stores them as 16-bit integers
  or floats, so the directions came out wrong: a rear-view mirror of one flat color, and a car body without its vinyls.
  They are now read as floats (`float4`), and the renderer tells Vulkan the right vertex format (SNORM, UNORM or float).
- **`PRED_SET_INV`.** One instruction (`SetpInv`) was translated without one of its cases (`src == 1`). On the Xbox 360
  it does `p0 = (a == 1); result = p0 ? 0 : (a == 0 ? 1 : a)`. The wrong result skipped blocks in the roadside grass
  shaders (drawn as stacked layers) and left a magenta stripe along the road. **Lesson:** when one material shows
  impossible colors, compare every operation with the reference behavior (Xenia's `ucode.h` and its translators)
  before you debug the renderer.
- **Alpha test.** All eight comparison functions are supported, chosen by a shared constant.
- **Depth-only draws.** When `RB_MODECONTROL` is in depth mode, the Xbox 360 GPU draws without a pixel shader. Some
  shadow casters come with pixel shader 0, and they were being skipped.

## Changes that made the shaders faster

On the console, the GPU's limit was shading pixels (at 307.2 MHz in handheld mode for most of the project, see
[performance-history.md](performance-history.md)), so the quality of the translated shaders matters directly.

### Constants through a uniform buffer

Before, the translated shaders read every constant through a 64-bit pointer (`RawBufferLoad`). [NAK](glossary.md#nak)
turns that into loads from main memory: 12 to 81 of them per pixel shader. [NVK](glossary.md#nvk) can put a dynamic
uniform buffer into the GPU's fast constant memory, even on this old GPU generation. Reading the constants from a
dynamic uniform buffer instead:

- raised the race frame rate by **18-23 %** (from 36 to 43 FPS in that test, switching the setting every 30 s in the
  same race; GPU time per frame went from 25.9 to 21.4 ms);
- cut the frames longer than 33 ms from 13-15 % to 1-2 %;
- cost 1.1 µs of CPU per draw, for binding one more descriptor set.

It was checked before it ever ran: all 3,211 constant reads in the library were confirmed to read the same data both
ways. A run with Vulkan's validation layer also found that the upload buffer was missing a flag,
`UNIFORM_BUFFER_BIT`.

### 1/size of each texture

The `tfetch2D` helper asked the texture for its size every time it sampled with an offset. In the 109 pixel shaders,
160 of the 435 texture instructions were those size questions, which draw nothing. Now the renderer writes `1 / size`
of each texture into the shared constants, and the shader uses it (turned on by a specialization bit): the 160
questions became 0, with the same 435 samples. Two details matter:

- the size must be the size of the Switch-side image, and compressed images are rounded up to multiples of 4;
- in HLSL, a `? :` evaluates both sides, so the specialized path needs `[branch] if / else` for the unused side to
  disappear.

### Conditional blocks

The Xbox 360 GPU can make instructions depend on a flag, `p0`. XenosRecomp wrote one `if (p0)` for each of them. The
nine samples of a 3x3 shadow filter (PCF) ended up in nine separate blocks, and the GPU waited for the texture nine
times instead of once. Now consecutive instructions under the same condition share one block, which is closed right
after any instruction that changes `p0`. **1,806 `if (p0)` became 641** across 207 shaders, with 16 % fewer SPIR-V
blocks in the expensive ones.

### `max(a, a)` used as a copy

The Xbox 360 GPU has no copy instruction, so a copy is written as `MAX dst, src, src`. The library had 727 `max(a, a)`
in pixel shaders and 482 in vertex shaders, and they reached the GPU as real multiplications: Mesa's optimizer turns
`fmax(a, a)` into `fcanonicalize(a)`, and then into `fmul(a, 1.0)` unless the backend declares `has_fcanonicalize`,
which no Mesa backend does. The translator now writes the value directly when both sides are the same. The result is
identical, NaN and negative zero included; the only thing lost is the flushing of tiny numbers (subnormals), which the
Xbox 360 did on every operation anyway.

**Lesson:** do not assume the driver cleans up obvious patterns. Find the rule in `nir_opt_algebraic.py` and check
what it depends on.

### Shadow map passes

- When a pass has no color image, and the pixel shader can neither discard pixels nor write depth, the pipeline is
  built **without a pixel shader** at all. Only 21 of the 89 pixel shaders can discard, but XenosRecomp adds the alpha
  test's `clip` to all of them.
- When the pixel shader only needs to run for its alpha test, a version compiled without the color writes lets the
  driver remove everything that only fed the color. This saved about 4 %; the shadow map shader was already small.

## A driver bug found through the shaders: the black sea

On the Switch, the sea was black in one area of the map, and fine on the PC. The water shader does
`saturate(r8.x - 0.4)`: a subtraction, then a clamp to the range 0-1. The constant 0.4 does not fit in the short form of
the instruction, so NAK chose `FADD32I`, which on this GPU generation (SM50) has no clamp bit, and silently dropped the
clamp (`.SAT`). The fix is in NAK's SM50 part (see [mesa.md](mesa.md)).

It was found by taking the compiled shader out of the pipeline cache and disassembling it, after diagnostic shaders
showed that everything except the final color was right.

Any fix to the shader compiler must also raise the compiler revision that the driver reports. On Horizon the build ID
is the package version, so otherwise the console keeps using the old compiled shaders from both caches.

## How translator changes were checked

- **Predicate generations.** For every instruction of every shader, count which change of `p0` controls it (how many
  writes to `p0` come before the `if` that wraps it). If the numbers are the same in both versions, the change cannot
  alter the result. This proved the block merging correct on all 207 shaders.
- **Same memory reads both ways**, as with the constant buffer change.
- **Comparing images in a paused race**, switching the setting every few seconds in the same session and comparing
  only the pixels that do not change within each mode (see [measuring.md](measuring.md)).
- **Diagnostic shaders.** Replace one output with bands of in-between values, to see which step goes wrong on the
  console.
- **Count instructions on specialized SPIR-V**, with the specialization values the pipelines really use. Counting on
  unspecialized shaders inflated the numbers five to ten times.
- **Keep track of the library's hash** wherever a library is used. One PC session was lost to an old library that
  drew the rear-view mirror with radial stripes.

## Measuring where the pixels go

GPU statistics queries around each pass, and one query per draw in one frame every few seconds, showed which shaders
fill the screen. For example, one smoke and light-ray effect, drawn as five stacked full-screen rectangles, took
27-37 % of all the pixels of the scene. Three traps:

- two queries of the same type active at once (one around a pass, others around its draws) return garbage, without
  any error;
- a measurement window tied to the rotation of the upload buffer catches only part of a frame, because that buffer
  rotates several times per frame;
- opening the window from the presentation thread catches a biased part of the frame. Open it where frames are
  recorded.
