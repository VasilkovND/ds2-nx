# How the port draws the game (the native renderer)

## In short

- [ReXGlue](glossary.md#rexglue) normally imitates the Xbox 360 GPU. On the Switch that was far too slow for this game:
  a race ran at about 2 frames per second.
- So this port draws the game its own way. The game still believes it is talking to an Xbox 360 GPU; the port reads
  the game's GPU commands and draws the same frame with [Vulkan](glossary.md#vulkan).
- This page explains why the imitation was replaced, how the new renderer works, and what made it fast.
- The code is in `app/src/nfsmw_nativo_*` ("nativo" means native).

## Why the imitated GPU was replaced

Measured on the console in September 2026, with the first build that worked:

| Scene | Imitated Xbox 360 GPU | Without resolves | Without any GPU work |
|---|---|---|---|
| Title screen | 10 FPS | 26.7 FPS | 58.6 FPS |
| Menu (about 800 draws) | 3.3 FPS | | 14 FPS |
| Race | 2.0-2.2 FPS | | 2.1 FPS |

What the numbers say:

- Removing **all** the GPU work did not make the race faster. So the problem was the CPU: about 80 µs of CPU per draw
  to imitate the GPU, plus the game's own [Direct3D](glossary.md#direct3d), plus handling the memory faults of the
  game's memory (see [platform-notes.md](platform-notes.md#exceptions)).
- Each [resolve](glossary.md#resolve) cost 3 to 4 ms of GPU time.
- Two days of cuts on the imitation side (turning off the game's drawing in strips, removing passes in races, updating
  the shadows less often) only reached about 3.7 FPS in a race.

The limit was far below 30 FPS, so the decision was to stop imitating the GPU and draw the game's frames with Vulkan
directly.

## How it works

### The idea: read the game's commands, keep its Direct3D

Other recompiled ports, like Sonic Unleashed and Marathon, replace the game's Direct3D functions: they put their own
objects in the game's memory and [hook](glossary.md#hook) the functions that create and lock resources. That does not
fit this game:

- many textures come with their Direct3D description already inside the game data, so they never go through
  `CreateTexture`, the function you would hook;
- `SetTexture` copies the texture's description (its "fetch constant") straight into the Direct3D device;
- the device keeps its own copy of the GPU's settings, which the game updates itself.

So the port lets the game's Direct3D run as it is, and reads what it writes: the list of GPU commands (the
[PM4 ring](glossary.md#pm4-ring)), with its settings, draws, copies and the "frame finished" command (Swap). It never
imitates the [EDRAM](glossary.md#edram).

A few hooks on the game's thread add what the command list does not say:

- **Which shader is which.** When vertex shaders reach the command list, Direct3D has already modified them, so they
  cannot be recognized by their microcode (see [shaders.md](shaders.md)). Hooks on the functions that create shaders
  note which original shader each one is. Each Direct3D draw call also notes which shaders it used, and the ring
  thread pairs those notes with the draws it reads: by type and number of vertices, checked against the last shader
  load in the list (`IM_LOAD`). One full-screen quad that Direct3D draws on its own is recognized from the command list
  alone.
- **Keeping the game in step.** The game waits (`WAIT_REG_MEM`) until the GPU writes a value (`SCRATCH_REG` to
  `SCRATCH_ADDR`). The port writes it, as the real GPU would; otherwise the game would freeze within a second.

### The threads

| Thread | What it does |
|---|---|
| Game threads ([guest](glossary.md#guest)) | Run the game and its Direct3D, which writes the command list |
| [Ring thread](glossary.md#ring-thread) | Reads the commands, keeps track of the GPU settings and records the Vulkan commands |
| Vertex copy thread | Copies vertex data into the upload buffer, turning the Xbox 360's byte order into the Switch's |
| Presentation | Composes the final image and shows it |

The ring thread is the heart of the renderer. For most of the project, a frame took exactly as long as one loop of
that thread.

### One frame

- **Three work slots.** Each frame is recorded into one of three slots. Each slot has its own command buffers, its own
  fence (a signal the GPU gives when it finishes), a 64 MB upload buffer and its own read-back buffers. So the CPU
  records the next frame while the GPU draws the current one, and only waits if the slot it needs is still busy. The
  read-back buffers must be per slot, or two frames would write over each other.
- **Render targets instead of EDRAM.** The game's [render targets](glossary.md#render-target) become normal Vulkan
  images, and resolves become copies into textures. Many of those copies are not needed: when the next thing the game
  does is clear the target anyway, the port swaps the two images instead of copying ("resolve without copy"). If the
  old content is needed later, it is brought back.
- **Some images must go back to the game.** The game measures how bright the scene is (for its automatic exposure) by
  reading a small image on the CPU. So a few resolved images are written back into the game's memory, in the exact
  Xbox 360 format. Only the 64x64 ones are needed for the exposure to look right, which removed 98 % of this cost.
- **Showing the frame** goes through ReXGlue's presenter in IMMEDIATE mode (see
  [platform-notes.md](platform-notes.md#presentation)).

### Each draw

For each draw, the ring thread:

1. reads the drawing settings from its copy of the GPU settings, and works out which [pipeline](glossary.md#pipeline)
   it needs;
2. finds the textures in a two-level cache;
3. copies the vertices (without duplicates) and the indices into the upload buffer, turning their byte order;
4. writes the shader constants that changed into a buffer the shaders read (a dynamic uniform buffer);
5. binds what changed and records the draw.

Lessons that shaped the code (each one cost time to find):

- **Check textures cheaply.** Each texture gets a fingerprint (a hash) of its raw bytes in the game's memory, and is
  only put back in normal order ("untiled", see [tiling](glossary.md#tiling)) when the fingerprint changes. Textures
  that do not change are checked less and less often, down to once every 32 frames. On the PC this took the texture
  work from 66 µs to 4.2 µs per draw in the menu.
- **Structures that are fingerprinted or compared byte by byte must have no gaps.** A pipeline key with four
  uninitialized gap bytes ("padding") created duplicate pipelines from leftover memory: 125 to 203, depending on the
  build. Every such structure has `static_assert(std::has_unique_object_representations_v<T>)`, which makes the
  compiler refuse a structure with gaps.
- **In busy loops, copy what you need into local variables first.** A vertex copy loop that read the source, the
  destination and the count from a structure passed by reference got 1.9 times slower: the compiler could not be sure
  that the writes did not change the structure, so it read it again every time.
- **Do not time everything.** Timers are only read on a sample: one in 128 packets, and one in 8 or 64 draws depending
  on the timer. See [measuring.md](measuring.md#counters-that-lie).
- **Counters that only one thread uses must not be atomic.** The Switch's processor (Cortex-A57, ARMv8.0) has no fast
  atomic instructions, so each atomic `fetch_add` is a slow loop that also disturbs the other cores. About 83,000
  packets per frame were counted that way: 1.1 ms per frame. Before removing an atomic, check every place that writes
  it: one of these counters turned out to be written by another thread too (the vblank thread).

### Pipelines

- A [pipeline](glossary.md#pipeline) is found by its drawing settings, shaders and specialization constants, and
  created through a Vulkan [pipeline cache](glossary.md#pipeline-cache) saved on the SD card. Without it, creating one
  on NVK took about 59 ms (82 ms for the ones created during the first race).
- On top of that, the port saves **the list of pipelines the game uses** and creates them again on a background thread
  when the game starts, so the first race does not stutter: 113 of 113 pipelines ready in 0.3 s, and none created
  slowly during the race. Both things live in one file, `cache/nfsmw_nativo_pipelines.bin`.
- Tried and dropped: Vulkan's dynamic state extensions (`VK_EXT_extended_dynamic_state` 1, 2 and 3), to change
  pipelines less often. On NVK with this GPU each change got slower (from 2.95 to 4.36 µs) for only 13 % fewer
  changes: a loss.

### Textures and memory

- The texture cache has a limit: 512 MB in the released version (setting `nfsmw_nativo_texturas_mb_max`). It was
  384 MB until the resolution became automatic, because larger resolutions need more room. Above the limit, textures
  not used for at least 120 frames are released until the cache is back under 75 %. Without a limit, the cache grew by
  about 20 MB every 40 s of racing, because the game streams new textures into new addresses as the car drives through
  the city.
- Textures take their memory from big blocks (16 MB each in the released settings) instead of one allocation each,
  because every allocation is slow on the Switch (see [platform-notes.md](platform-notes.md#nvk-on-horizon)).
- Untiling works on groups of 16 bytes with NEON (the processor's instructions for several values at once). The first
  version, block by block, cost around 30 ms each time a burst of new textures arrived.

### Game functions in native code

Where the game's own thread was the slowest part, some of its busiest functions were rewritten in C++ and installed as
[hooks](glossary.md#hook): the material setup, the effect setup, the matrices of each draw, the visibility check, the
drawing of the scenery, the dump of Direct3D settings into the command list, and the code that issues each draw.

Every one is protected by the same [guard](glossary.md#guard):

- for the first 50,000 to 200,000 calls, and then one in 4,096, the port runs both the native and the original
  function and compares everything they produce;
- the original's result is the one used;
- any difference turns the native version off until the game is closed, and writes `DIFERENCIA` in the log.

Getting exactly the same results needs care:

- The game was compiled with FMA (a multiply and an add in one instruction, which rounds slightly differently). The
  native code must write each expression in the same shape and order, so that GCC fuses it the same way. Test code
  placed between a multiply and an add changes that, and makes the test report false differences.
- Only the registers that something reads later need to match. An analysis of the translated code finds which ones.
- Inputs with NaNs (invalid numbers) are left to the original function.

### Guards that check themselves

Some changes depend on recognizing a draw, a shader or a render pass by its signature. They switch on in three stages,
decided while the game runs:

1. watch, without changing anything;
2. apply, but only after the signature has been seen cleanly for a number of frames;
3. switch off until the game is closed if the known sign of failure ever appears.

So the worst case is "it does nothing", never a broken image. This was adopted after two builds broke the image by
moving the sky pass without checking first.

## What made it fast

From the first native build to the release, the work went back and forth between two limits: the GPU (shading pixels,
at 307.2 MHz in handheld mode for most of the project) and the ring thread (CPU time per draw).
[performance-history.md](performance-history.md) tells it step by step, with the measurements. The main changes:

**GPU**

- shader constants through one buffer (a dynamic uniform buffer) instead of reading them from memory one by one;
- resolves without copies, and one shadow map copy replaced by taking the minimum of two shadow maps;
- [ZCULL](glossary.md#zcull) turned on in the driver;
- cleaner translated shaders: merged conditional blocks, no `max(a, a)` moves, no texture size queries;
- resetting only the GPU query pools the frame will use (2,144 resets per frame were costing 1.2 ms of GPU idle time);
- less geometry in the shadow map: level of detail and distance limits;
- a slope bias for the shadows. It removed "shadow acne", a pattern of dots that a cheaper shadow filter had made
  visible.

**CPU**

- no busy waits in the game's Direct3D;
- the ring thread at a priority that does not starve the presentation;
- vertex uploads without duplicates, a helper that copies when the copy thread falls behind, and texture fingerprints
  made on another thread;
- caches for shader loads, texture descriptions and pipelines;
- native versions of the busiest game functions;
- log lines written by another thread (the periodic report used to be formatted by the ring thread);
- a read cache for the game's streaming, which read the same zone packs from the SD card again on every lap.

**Build**

- registers as C++ variables, direct calls, [LTO](glossary.md#lto), [PGO](glossary.md#pgo) and
  [function ordering](glossary.md#function-ordering) (see [toolchain.md](toolchain.md)).
