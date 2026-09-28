# Porting another Xbox 360 game to the Switch

## In short

This guide tells you what you can reuse from this project for another game, and in which order to work. Most of the
hard problems of this port had nothing to do with Need for Speed: they came from the Switch itself and from the
recompiler, so your port will meet them too. Read it once from top to bottom before you start. If a word is new to
you, it is in the [glossary](glossary.md).

The order that worked:

1. Get the game running on a PC with the recompiler.
2. Move it to the Switch system.
3. See the first frames, then write a faster renderer.
4. Translate the shaders before playing.
5. Measure on the console.
6. Make it faster.

## 1. Get the game running on a PC first

**Why:** every problem of the translation is much easier to find and fix on a PC than on the console.

1. Use [ReXGlue](glossary.md#rexglue) on the PC to translate the game's `default.xex` to C++.
2. Fix what the translation gets wrong. It usually misses some functions, some jump tables (lists of addresses that the
   code jumps through) and some function chunks (functions that the executable stores in several pieces).
3. Get the game to start and play with ReXGlue's own graphics, which imitate the Xbox 360 GPU.

For this game, the [NFSMW Recompiled](https://github.com/madelrandel-blip/NFSMW-Recompiled) project had already done
this stage.

Two things from this repository help here:

- **`tools/huecos.py` and `app/huecos.toml`** ("huecos" means gaps). They declare as functions the pieces of code that
  the translation did not reach.
- **`share_registers`**, an option of the code generator (in `sdk/src/codegen/builders`). The translated code keeps
  the Xbox 360 processor's registers (its working slots) in normal C++ variables. This option lets the pieces of a
  split function share those variables, which is what makes it safe, and it saves a lot of CPU on the Switch's slow
  cores. [toolchain.md](toolchain.md) explains it.

## 2. Move it to the Switch system

The Switch layer in `sdk/` is not tied to this game, and it should work for another game as it is. It gives the game
its memory, catches its crashes (exceptions), runs its threads with their priorities, and provides clocks, audio output
and the showing of frames on screen.

The problems that cost the most time are explained in [platform-notes.md](platform-notes.md). The main ones, in short:

- **There is a limit on memory mappings.** [Horizon](glossary.md#horizon) limits how many pieces of memory a program
  can map (error `2001-0103`). The game's memory is also visible at several addresses at once ("mirror views"), as on
  the Xbox 360, and that uses up many of them.
- **Two crashes at the same time break each other.** [libnx](glossary.md#libnx) has one exception stack for the whole
  program, so if two threads crash at once, they overwrite each other's data.
- **`std::thread::detach()` closes the game** on Horizon. The port creates its threads once and keeps them instead.
- **Threads of the same priority do not share the processor**, except at the normal game priority (`0x3B`). A busy
  thread can leave the others waiting forever.
- **The game gets three processor cores, not four.**

## 3. First frames, then a faster renderer

ReXGlue's graphics imitate the Xbox 360 GPU: they translate its commands, imitate its [EDRAM](glossary.md#edram) and
convert shaders while playing. That works on the Switch, and it is the right way to see the first frames and check that
the game logic runs. But on the Switch it was far too slow for this game, a few frames per second.
[native-renderer.md](native-renderer.md) explains why.

So the port has its own renderer. The game keeps using its own [Direct3D](glossary.md#direct3d), which writes its
list of GPU commands (the [PM4 ring](glossary.md#pm4-ring)) as always. A thread of the port, the
[ring thread](glossary.md#ring-thread), reads that list and draws the same thing directly with Vulkan.

What you can reuse from `app/src/nfsmw_nativo_*` ("nativo" means native):

- the reader of the command list, the tracking of the GPU settings, and the recording of each draw;
- [render targets](glossary.md#render-target) as normal Vulkan images, and [resolves](glossary.md#resolve) that avoid
  extra copies;
- the texture cache, which puts textures back in normal order ("untiling") when it uploads them;
- the [pipeline cache](glossary.md#pipeline-cache), which compiles the known pipelines at startup;
- the pattern of replacing the busiest game functions with native code, each with a [guard](glossary.md#guard).

What is specific to this game: the addresses of the [hooks](glossary.md#hook), and knowing its render passes (which
pass draws the shadow map, the reflections, the cubemap). Start with a tracing phase: wrap the game's Direct3D
functions and write every call to the log (`app/src/nfsmw_d3d_trace.cpp`). That confirms each address before you
replace anything.

## 4. Translate the shaders before playing

**Why:** translating [shaders](glossary.md#shader) while playing causes stutters and costs CPU.

1. Translate the shader [microcode](glossary.md#microcode) before the game runs:
   [XenosRecomp](glossary.md#xenosrecomp) turns it into [HLSL](glossary.md#hlsl), and [DXC](glossary.md#dxc) turns
   the HLSL into [SPIR-V](glossary.md#spir-v).
2. Put everything in a [shader library](glossary.md#shader-library), where each shader is found by a fingerprint (a
   hash) of its microcode. The tools are in `shaders/`, and [shaders.md](shaders.md) explains them.

XenosRecomp needed several fixes for this game. Some translation choices also changed the GPU time a lot: passing
the shader constants in one buffer, and turning conditional blocks into straight code. The installer page shows how to
build the library in the browser from the user's own disc, so no game data is ever shared.

## 5. Measure on the console

Read [measuring.md](measuring.md) before you optimize anything. The key points:

- The PC tells you where the work is, but never how much it costs on the Switch.
- Compare two versions (A and B) in the same session.
- Multiply the GPU times that NVK reports by 1.627 to get real time.
- Look at the CPU time of each thread, not the total.
- Take samples of where each thread is (its "stack") to see what it is doing.
- Judge smoothness by how the [frame times](glossary.md#frame-time) are spread, not by the average FPS.

## 6. Optimizations you can reuse

In the order they paid off here ([performance-history.md](performance-history.md) has the numbers):

- **The build:** direct calls between translated functions, [LTO](glossary.md#lto), [PGO](glossary.md#pgo) and
  [function ordering](glossary.md#function-ordering). See [toolchain.md](toolchain.md).
- **The game's own busy waits.** The Xbox 360 Direct3D waits for the GPU and for other threads by spinning: checking
  again and again without resting. On three slow cores, that steals time from the threads that do real work.
- **CPU cost of each draw in the renderer:** caches instead of repeating work, no memory allocations and no log lines
  on the ring thread, no duplicate uploads, and a cheaper path through the driver (see [mesa.md](mesa.md)).
- **GPU time:** fewer pixels shaded ([ZCULL](glossary.md#zcull)), cheaper shaders, and skipping work the Xbox 360
  needed but the Switch does not, like drawing in strips for the EDRAM.
- **Native replacements** of the busiest game functions, each with its guard.

## 7. What did not work

- Lowering the internal resolution did not help: the frame was not limited by resolution, and it cost time.
- Half-precision math at double speed (FP16) and variable rate shading: the shader compiler, NAK, does not support them
  on this GPU (see [mesa.md](mesa.md)).
- Extended dynamic state in NVK, to change pipelines less often: each change got slower, so it was a loss.
- Overclocking hides problems instead of solving them. This port was measured and tuned at the console's normal
  clocks.
