# Porting another Xbox 360 game to the Switch

What in this repository carries over to another game, and the order in which the work went. Most of the hard
problems of this port were not specific to Need for Speed: they come from the platform (Horizon, NVK on the Tegra X1)
and from the recompiler, and they will show up again in the next port.

## 1. Get the recompilation running on the PC

Start with [ReXGlue](https://github.com/rexglue/rexglue-sdk) on the PC: generate the code from the `default.xex`,
fix what the analysis gets wrong (functions it misses, jump tables, function chunks) and get the game to boot and play
with the SDK's own GPU backend. Every recompilation problem is easier to debug there than on the console. For this
game, [NFSMW Recompiled](https://github.com/madelrandel-blip/NFSMW-Recompiled) had done this stage.

Two things from this repository help at this stage:

- `tools/huecos.py` and `app/huecos.toml`: code the analysis did not reach, declared as functions.
- `share_registers` in the code generator (`sdk/src/codegen/builders`): keeps guest registers in C++ locals shared
  between the chunks of a function. It is what makes "registers as locals" safe, and it is a large CPU win on a slow
  core. See [toolchain.md](toolchain.md).

## 2. Bring the runtime to Horizon

The Horizon layer in `sdk/` is not tied to this game and should work as is: guest memory with its mirror views, the
exception handler, threads and their priorities, clocks, audio output and presentation. The traps it works around
are in [platform-notes.md](platform-notes.md). The ones that cost the most time:

- the kernel limit on memory mappings (error `2001-0103`) and how the mirror views of guest memory hit it;
- libnx's exception stack is global, so two threads faulting at the same time corrupt each other;
- `std::thread::detach()` on Horizon closes the game; threads are persistent instead;
- threads of equal priority are not time sliced except at the default game priority (`0x3B`); a busy thread starves
  the others;
- the game gets three CPU cores, not four.

## 3. First frames, then a native renderer

The SDK's GPU backend emulates the Xenos: it translates the command stream, emulates the EDRAM and converts shaders at
run time. It works on NVK, and it is the right way to see the first frames and check that the game logic runs. On the
Switch it was far too slow for this game (a few frames per second), for reasons explained in
[native-renderer.md](native-renderer.md).

The replacement keeps the game's own Direct3D layer and consumes the PM4 ring it writes, recording Vulkan directly on
a dedicated thread. What carries over from `app/src/nfsmw_nativo_*`:

- the ring decoder, the register state and the draw recording;
- render targets as plain Vulkan images, with resolves that avoid copies;
- the texture cache with untiling on upload, and the pipeline cache with prewarming;
- the pattern of replacing hot game functions with native code behind self-checking guards.

What is specific to this game are the addresses of the hooks and the knowledge of its render passes (which pass is
the shadow map, the reflections, the cubemap). Start with a tracing phase: wrap the game's Direct3D functions and log
every call (`app/src/nfsmw_d3d_trace.cpp`) to confirm each address before replacing anything.

## 4. Shaders ahead of time

Translate the shader microcode before the game runs: XenosRecomp to HLSL, DXC to SPIR-V, and a library keyed by a
hash of the microcode (`shaders/`, [shaders.md](shaders.md)). The translator needed several correctness fixes for
this game, and some translation choices had a large effect on GPU time (constants through a uniform buffer,
flattening predicated blocks). The installer page shows how to build the library in the browser from the user's disc,
so no game data is ever distributed.

## 5. Measure on the console

Read [measuring.md](measuring.md) before optimizing anything. In short: the PC tells you where the work is, never how
much it costs on the Switch; compare A and B in the same session; convert NVK timestamps (x1.627); look at the CPU of
each thread, not the total; sample stacks; and judge frame pacing by the distribution of frame times, not the
average.

## 6. Optimizations that transfer

In the order they paid off here ([performance-history.md](performance-history.md) has the numbers):

- The build: direct calls between recompiled functions, LTO, PGO and function ordering
  ([toolchain.md](toolchain.md)).
- The game's own busy waits: the Xbox 360 Direct3D spins on the GPU and on other threads. On three slow cores that
  steals time from the threads that do the work.
- CPU cost per draw in the renderer: caches instead of repeated work, no allocations or logging on the ring thread,
  deduplicated uploads, and a cheaper path through NVK ([mesa.md](mesa.md)).
- GPU time: fewer fragments shaded (ZCULL), cheaper shaders, and passes that the Xbox 360 needed but the Switch does
  not (predicated tiling of the EDRAM).
- Native versions of the hottest game functions, each with its guard.

## 7. Doors that stayed closed

- Lowering the internal resolution did not help: the frame was not limited by resolution and it cost time.
- FP16 at double rate and variable rate shading: not available with NAK on Maxwell ([mesa.md](mesa.md)).
- Extended dynamic state in NVK to bind fewer pipelines: each bind got slower, a net loss.
- Overclocking hides problems instead of solving them; this port was measured and tuned at stock clocks.
