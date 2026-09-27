# The native renderer

ReXGlue runs recompiled Xbox 360 games with an emulated Xenos GPU: the game's Direct3D layer writes
PM4 command packets into a ring buffer, and the runtime interprets them the way the real GPU would,
EDRAM included, on top of Vulkan or Direct3D 12. On the Switch that path could not reach playable
frame rates, so this port replaces it with a renderer written for this game. This page explains why,
how it works, and what made it fast.

## Why the emulated GPU was replaced

Measured on the console in September 2026, with the first working build of the port:

| Scene | Emulated Xenos | Without resolves | Without any GPU work |
|---|---|---|---|
| Title screen | 10 FPS | 26.7 FPS | 58.6 FPS |
| Menu (about 800 draws) | 3.3 FPS | | 14 FPS |
| Race | 2.0-2.2 FPS | | 2.1 FPS |

- Removing **all** GPU work did not change the race frame rate. The cost was on the CPU: about 80 µs
  per emulated draw in the command processor, the guest Direct3D driver itself, and the page fault
  handling of the guest memory (see [platform-notes.md](platform-notes.md#exceptions)).
- Each resolve (EDRAM to texture copy) cost 3-4 ms of GPU time.
- Two days of cuts on the emulation side (disabling the game's tiled rendering, cutting passes in
  races, updating shadows less often) brought the race to about 3.7 FPS.

The ceiling was far below 30 FPS, so the decision was to stop emulating the GPU and draw the game's
frames with Vulkan directly.

## Design

### Consume the ring, do not replace Direct3D

The first idea was the model used by other recompilation ports (for example Sonic Unleashed and
Marathon): place host objects in guest memory and hook the game's Direct3D creation and lock
functions. It does not fit this game:

- many textures are bound through Direct3D headers embedded in loaded game data, so `SetTexture`
  receives objects that never went through `CreateTexture`;
- `SetTexture` copies the header's fetch constant straight into the device;
- the device keeps a mirror of the Xenos registers (constants, booleans and register blocks) that
  the game's own setters update.

So the renderer keeps the game's Direct3D layer running unchanged and **consumes the PM4 ring**: the
register writes, draws, copies and the Swap packet. It never touches EDRAM emulation. A small set of
hooks on the game thread adds what the ring lacks:

- **Shader identity.** Vertex shaders reach the ring already patched by Direct3D and cannot be matched
  by microcode (see [shaders.md](shaders.md)). Hooks on the shader creation functions map each object
  to its original container. Each Direct3D draw call pushes the bound vertex and pixel shader objects
  into a queue that is paired with ring draws by primitive type and count, cross-checked with the last
  shader load packet (`IM_LOAD`) of the ring. A composite quad that Direct3D emits outside its own
  draw functions is identified from the ring alone.
- **Synchronisation.** The ring sink writes `SCRATCH_REG` to `SCRATCH_ADDR` like the real GPU, or the
  game stalls on `WAIT_REG_MEM` within a second.

### Threads

| Thread | Work |
|---|---|
| Game threads (guest) | run the game and its Direct3D layer, write the ring |
| Ring thread | decodes PM4 packets, tracks register state, records Vulkan commands |
| Vertex copy thread | copies and byte-swaps vertex data into the upload buffer |
| Presentation | composes the output and presents |

The ring thread is the heart of the renderer. For most of the project, the frame lasted exactly as
long as one loop of that thread.

### Frame pipeline

- **Work slots.** Each frame records into one of three slots. A slot owns its command buffers, fence,
  a 64 MB upload buffer and its read-back buffers. The CPU records frame N+1 while the GPU executes
  frame N; recording only waits if the slot it needs is still in flight. Read-back buffers must be per
  slot, or two frames in flight race on the same mapped memory.
- **Render targets instead of EDRAM.** The game's render targets become Vulkan images, and resolves
  become copies into textures. Many copies are unnecessary: when the next operation clears the target
  anyway, the renderer **swaps the images** instead of copying ("resolve without copy"). When the
  swapped content is needed again later, it is restored with barriers.
- **CPU read-backs.** The game measures scene brightness for its auto-exposure by reading a small
  resolved texture on the CPU, so a few resolves must be written back to guest memory, tiled and
  byte-swapped exactly as the Xbox 360 would. Only the 64x64 targets are needed for the exposure to
  look right, which removed 98 % of the read-back cost.
- **Presentation** goes through ReXGlue's presenter with IMMEDIATE mode (see
  [platform-notes.md](platform-notes.md#presentation)).

### Draw recording

For each draw, the ring thread:

1. reads the render state from its register mirror and derives the pipeline key;
2. resolves textures through a two-level cache (per texture register, then per full fetch constant);
3. copies vertices (deduplicated) and indices into the upload buffer, byte-swapped;
4. writes the shader constants that changed into a dynamic uniform buffer;
5. binds what changed and records the draw.

Rules that the implementation follows:

- **Hash raw guest bytes, untile only on change.** Textures are hashed from their tiled guest memory
  (exact tiled extent), untiled only when the hash changes, and stable textures are rechecked with a
  back-off from every frame to every 32 frames. On the PC this took texture preparation from 66 µs to
  4.2 µs per draw in the menu.
- **Structures that are hashed or compared byte by byte have no padding.** A pipeline key with four
  uninitialised padding bytes created duplicate pipelines from stack garbage (125 to 203 depending on
  the build). Every such structure has
  `static_assert(std::has_unique_object_representations_v<T>)`.
- **Keep hot loops free of aliasing.** A vertex copy loop that read the source, destination and count
  from a structure passed by reference got 1.9 times slower, because writes through the `uint8_t*`
  destination could modify the structure. Copy the fields into locals before the loop.
- **Sample the stopwatches** (one in 64 to 128 packets, one in 8 draws). See
  [measuring.md](measuring.md#counters-that-lie).
- **Counters written by a single thread are not atomic read-modify-writes.** The Cortex-A57 is ARMv8.0,
  without the ARMv8.1 atomics, so every `fetch_add` is an exclusive load/store loop that also steals
  the cache line. About 83,000 packets per frame were counted that way: 1.1 ms per frame. Before
  removing an atomic, list every caller: one of the counters turned out to be written from the vblank
  thread.

### Pipelines

- Pipelines are keyed by render state, shaders and specialization constants, and created through a
  persistent `VkPipelineCache` saved on the SD card. On NVK a pipeline took about 59 ms to create
  without it (82 ms for the ones created during the first race).
- On top of the cache, the renderer records **the list of pipelines the game uses** and recreates
  them on a background thread at start-up, so the first race does not stutter: 113 of 113 pipelines
  prewarmed in 0.3 s, and no slow creation during the race. Both live in one file
  (`cache/nfsmw_nativo_pipelines.bin`).
- Dynamic state from `VK_EXT_extended_dynamic_state` 1, 2 and 3 was tried to reduce pipeline
  switches and **lost** on NVK/Maxwell: binding a pipeline went from 2.95 to 4.36 µs per call for only
  13 % fewer binds.

### Textures and memory

- The texture cache is limited (384 MB); above the limit, textures not used for at least 120 frames
  are evicted until it is back under 75 %. Without a limit the cache grew by about 20 MB every 40 s of racing, because the
  game streams new textures into new addresses as the car moves through the world.
- Textures are sub-allocated from slabs (16 MB in the released configuration) instead of one dedicated allocation each
  (see [platform-notes.md](platform-notes.md#nvk-on-horizon)).
- Untiling works on 16-byte groups with NEON byte swapping; the first version, block by block with
  variable-size `memcpy`, cost around 30 ms whenever a burst of new textures arrived.

### Game functions in native code

Where the game thread itself was the bottleneck, some hot guest functions were rewritten in C++ and
installed as hooks: the material setup, the effect setup, the per-draw matrices, the visibility
query, the render entry of the scenery, the Direct3D register dump into the ring and the draw glue.
Every one of them is protected by the same guard:

- the first 50,000 to 200,000 calls, and then one in 4,096, run **both** the native and the original
  code and compare every output;
- the original's result is the one used;
- any difference disables the native version for the rest of the session and logs `DIFERENCIA`.

Bit-exact results need care: the game is compiled with FMA contraction, so the native code must write
each expression in the same shape and operand order for GCC to fuse it the same way. Instrumentation
placed between a multiply and an add splits the basic block and changes the fusion, which makes test
harnesses report false differences. Only the registers that someone reads later need to be
reproduced, which an interprocedural liveness analysis over the generated code determines. Inputs
with NaNs are left to the original.

### Guards that verify themselves

Changes that depend on recognising a draw, a shader or a pass by its signature ship in three phases,
decided at runtime: observe without changing anything; apply only after the signature has been seen
cleanly for N frames; and switch off for the rest of the session if the known symptom of failure
appears. The worst case becomes "does nothing", and a visual regression is no longer a possible
outcome. This was adopted after two builds broke the image by enabling a deferred sky pass blindly.

## What made it fast

From the first native build on the console to the release, the work alternated between two
bottlenecks: the GPU (fragment shading at 307.2 MHz) and the ring thread (CPU per draw). The
chronology, with the measurements behind each step, is in
[performance-history.md](performance-history.md). The main levers, grouped:

**GPU**
- constants through a dynamic uniform buffer instead of global memory loads;
- resolve without copy, and a shadow map resolve replaced by a minimum operation;
- ZCULL enabled in the driver;
- translator output: merged predicate blocks, no `max(a, a)` moves, no texture size queries;
- only resetting query pools that the frame will use (2,144 query resets per frame were costing
  1.2 ms of GPU idle time);
- shadow map geometry: level of detail and distance limits for the four emitters of the shadow pass;
- a shadow slope bias for the shadow pass (it fixed acne, which a cheaper PCF had made visible).

**CPU**
- no busy-waits in the game's Direct3D layer;
- the ring thread at a priority that does not starve presentation;
- deduplicated vertex uploads, a helper that copies when the copy thread falls behind, and texture
  hashing on another thread;
- caches for shader loads, texture fetch constants and pipelines;
- native versions of the hottest game functions;
- asynchronous logging (the periodic report used to be formatted and written by the ring thread);
- a read cache for the game's streaming, which re-read the same zone packs from the SD card on every
  lap.

**Build**
- registers as C++ locals, direct calls, LTO, PGO and function ordering
  (see [toolchain.md](toolchain.md)).
