# Porting notes: Dead Space 2

Dead Space 2 (Xbox 360, Title Update 2) on the Nintendo Switch, as a homebrew NRO. This file records how it was
done, every problem met on the way and what fixed it, and what should carry over to the next Xbox 360 → Switch port.
Release history: [CHANGELOG.md](../CHANGELOG.md). Installation: [INSTALL.md](../INSTALL.md).

## Status (1.0.3)

Playable. Tested through the early chapters, the decompression scene, a space walk and a boss fight (saves reached
chapter 3); a full playthrough is not verified yet.

| | Stock clocks (CPU 1020 MHz, GPU 307 MHz) | Overclocked (CPU 1785 MHz, GPU 614-768 MHz) |
| --- | --- | --- |
| Light scenes (≤ 1,000 draws) | ~27-29 fps, GPU-bound (28-34 ms GPU per frame) | 30 fps (the game's cap) |
| Heavy fights (1,800-2,700 draws) | 12-16 fps, CPU-bound (the game's main thread) | comfortable |

## Architecture

```
default.xex + TU2 default.xexp           (your discs, tools/stage_sd.py)
        │
Dead-Space-2-Recomp src_tu2/              all 47,739 functions of the TU2 executable, statically recompiled to C++
        │                                 by ReXGlue v0.10.0 (bifftannen1); built here as it is
rexglue-sdk-nx (sdk/, + sdk-patches/)     kernel, memory, threads, audio (XMA via FFmpeg), input, deko3d presenter and
        │                                 the rex::x360 deko3d renderer (from NFSMW-NX)
deadspace2/src/
    ds2_gpu.cpp        PM4 ring reader: register mirror, draws → RenderDk, resolves/clears → Copiar, D3D clears
    ds2_shaders.cpp    identifies each shader the game loads in the library
    ds2_audio.cpp      audio hooks (compressor bypass, NEON mixing, stage timers)
    ds2_cpu.cpp        mtmsrd regions per thread, EA thread priority calls
    ds2_disc_swap.cpp  disc folders, title update, XamSwapDisc
shaders/                                  Xenos microcode → HLSL → SPIR-V → library (.dslib) → DKSH cache, ahead of time
```

No Xenos GPU emulation runs on the console. The game's command ring is read on a host thread and every draw goes to
a native deko3d renderer with shaders translated beforehand. This is what the NFS: Most Wanted, GTA IV and
Mushihimesama Futari Switch ports do too; Xenos emulation would be a few frames per second.

## 1. Code and build

- **The recompiled code is not generated here.** Dead-Space-2-Recomp checked in its codegen output (`src_tu2/`,
  ~235 MB of C++ in 157 units) with jump tables, missing functions and names from the prototype PDB already fixed on
  the PC. That work is most of a port; this repository builds it unchanged. `build_mac.sh` fetches it at a pinned
  commit (`DS2_RECOMP_COMMIT`).
- **Same ReXGlue version on both sides** (v0.10.0): the generated code only touches the SDK through `rex/ppc/*.h` and
  its PCH, and rexglue-sdk-nx only adds to those. The Switch layer in Dead-Space-2-Recomp's own patch targets
  Nintendo's official SDK (`nn::*`, nx-clang), which homebrew cannot use; the libnx layer of this SDK replaces it.
- **The PCH** (`tools/make_nx_pch.py`, run at configure time on a mirror of `src_tu2`):
  1. `DEFINE_REX_FUNC` needs a GCC branch: with the attributes before `extern "C"` GCC drops the weak alias silently
     and the link fails with every `sub_` name undefined.
  2. `REX_RAW_ADDR_W(x, n)` (DS2's codegen emits it for stores) is `REX_RAW_ADDR(x)` here.
  3. **The interrupt locks** (see 5.1): `mtmsrd` regions are a per-thread state, not a process-wide mutex.
- Flags of the recompiled units: `-O3 -fno-strict-aliasing -ffp-contract=off`. Both `-f` flags are needed for
  correctness (type-punned guest memory; PowerPC rounding without fused multiply-add). A full build of the 157 units is
  ~11 minutes on 10 cores of an M1 Pro; PCH changes rebuild all of them, so `make_nx_pch.py` writes only when the
  text changes.
- **SDK patches** (`sdk-patches/`, applied by `build_mac.sh`):

  | Patch | What |
  | --- | --- |
  | 0001-commit-granule | Physical memory committed in 4 MB chunks, virtual in 64 KB (below) |
  | 0002-deko-formats | The renderer work: formats, resolves, clears, vertex fetch, caches (sections 3-4) |
  | 0003-fps-counter | FPS counter, shortcuts on ZL+ZR (debug ones behind `switch_atajos_depuracion`), runtime stack sampling |
  | 0004-fixed-flush-mode | FPCR flush-to-zero fixed on: the 45,000 per-instruction mode switches of the code become no-ops |
  | 0005-yield-backoff | `MaybeYield`: after 16 yields within 20 µs, sleep 100 µs (busy waits) |
  | 0006-xma-locks | XMA kicks without the context lock, decoder woken only when asleep (5.3) |

## 2. Memory, discs, title update

- **Boot 1 crashed in 2 s** (`2001-0103`, "AllocFixed: no se pudo confirmar 0x1000 bytes"): the game commits physical
  memory 4 KB at a time; each commit was its own mapping and Horizon ran out of memory *blocks* long before memory.
  Committing in 4 MB chunks fixed it. Expect this on any 360 port: the limit is the number of mappings.
- **Discs are extracted folders**, not ISOs: the PC build maps the whole image in memory, which the Switch cannot.
  Both bigfiles are under 4 GB, so FAT32 works. Disc 2 is swapped in at chapter 4 by binding `XamSwapDisc`'s
  import (`0x82B0857C`) to a handler that re-points `d:` and signals the game's event.
- **The code is TU2**: it only matches `default.xex` + TU2's `default.xexp`, checked by hash
  (World: `928597dd…` and `8d386d37…`). Two of the four regional TU2 packages fit: Media ID 2915C8F8 (World) and
  2C199AEB (Russia, its own disc `7fac7f81…` and patch `6eb88610…`). Run through ReXGlue, the two patched executables
  generate byte-identical C++, so one NRO serves both; the Russian data comes from its own xex and bigfiles.
  `stage_sd.py` reads the STFS package itself (contiguous files: header size at 0x340, volume descriptor at 0x379,
  one or two hash tables per level by bit 0 of the block separation byte, a level-0 table every 0xAA data blocks).
- **Title takeover is required** (start a game holding R): applet mode does not have the memory.

## 3. Shaders

### Getting them

DS2 keeps its shaders RefPack-compressed inside the VIVs (a raw scan finds 129 of ~1,300). Instead the XDK's
`D3DDevice_CreateVertexShader` (0x82220568) / `CreatePixelShader` (0x822201C0) were wrapped and every container the
game creates was dumped (`ds2_shader_dump`, keyed by XXH3). DS2 creates its whole set at startup: 1,303 containers
(161 VS, 1,142 PS), 2008 format (`0x102A1100` PS, `0x102A1101` VS), which XenosRecomp reads as is. Dumping
synchronously on the loading thread froze it ~40 s: dump once, or asynchronously.

### Translating them (XenosRecomp, `shaders/`)

Fixes in `shader_recompiler.cpp`, each for a case that broke a DS2 shader:

- **Registers above r31** (up to r62) were not declared.
- **A sampler fetched as a cube while the constant table says 2D** (`g_envSampler`): the binding follows the fetch.
  The renderer must bind what the shader fetches, not what the constant table says.
- **Vertex textures** sit at fetch constants 16 + N; vertex 2D fetches use `SampleLevel` (no derivatives in VS).
- **Forward jumps as `if (pc <= N)` blocks** (`--saltos-lineales`): three light shaders are if/else-if ladders with
  unconditional forward jumps, which XenosRecomp turned into `while (true) switch (pc)`; uam's register allocator ran
  for minutes and several GB on them. Same semantics without the loop: GLSL 450 KB → 91 KB, 0.3 s per variant.
- **The vertex index (r0.x).** A Xenos VS starts with the vertex index in r0.x; vertex shaders that read it before
  writing it now get `SV_VertexID + g_IndiceBase` (the draw's first index, shared constant 488). Without it, particle
  billboards (which fetch at index / 4) and other index-driven shaders produced **streaks and broken triangles**.
- **Computed-index vertex fetches.** Some VS fetch with an index they compute (source register or swizzle not the
  plain vertex index). Those elements are fetched by the shader itself (`dsVfetch`) from storage buffers holding the
  whole vertex buffer, with format/offset/stride descriptors in a uniform buffer. Only 58 VS get this decoder.
- **Keep the debug names.** The DKSH cache key is a hash of the whole SPIR-V, debug names included: renaming the UBO
  structs (`NfsmwBloque*`, inherited) invalidated every cached shader and caused compile freezes during play.

### The DKSH cache

Every (shader, specialization) variant is precompiled on the PC (`DKSH=1 build_library.sh`, 10,642+ variants, all
compile). Two problems seen:

- Compiles of 4-9 s each on the console (uam) = freezes. The console's own cache (`cache/nfsmw_dksh.bin`) used to
  shadow the shipped PC cache entirely; now both are loaded and merged. When an exact VS variant is missing, the "all
  interpolators" variant is used while the exact one compiles in the background.
- Variants for each specialization value with and without the vertex-base bit must be in the default set, or the
  first draw of each compiles live.

## 4. Renderer (rex::x360 deko3d, ring reader)

### Ring reader (`ds2_gpu.cpp`)

- **WAIT_REG_MEM**: yield for the first millisecond, then sleep 100 µs. A Horizon sleep often lasts far longer than
  asked, and the game's main thread waits on GPU fences behind these waits.
- **Guest-visible GPU progress** (`g_sincronizaciones_anillo`) is bumped on memory writes, scratch-register mirrors and
  interrupts, not on every read-pointer writeback (that cost ~40 % of the ring thread).
- **Repeat VS loads** (D3D reloads the VS for every draw): an early-out on same address, length and three words
  avoids copying and hashing the microcode again.
- **16-bit indices**: read the 32-bit word, swap it, take the half (an endian bug, not the streaks).

### Direct3D clears

D3D clears by drawing a rectangle with its own internal shaders (unknown to the library). Emulated as the renderer's
clears (`EmulateClear`):

- **Multisampled, full-surface ones**: c0 cleared as color; c1-c3 on another base clear depth at that base (D3D writes
  depth through them); depth written over c0's EDRAM also clears the color image (the renderer keeps them apart).
- **The stencil value is the stencil reference** when depthcontrol does a REPLACE (00008777), not 0. DS2 clears it to
  its own value before the Scaleform menu masks; with 0 every **menu panel and the objective path** disappeared.
- **Single-sampled ones**, only in D3D's exact shape: rect list of 3 vertices, mask FFFF, an 8-bit target and a 0..1
  color in PS c0. The quarter-size particle buffer needs it (smoke piled up frame after frame). The same shape also
  writes the 80-wide float exposure target with game data (c0 like 0.027 0 -18.4 1): that is **not** a clear.
  Clearing every single-sampled full-surface draw wiped the scene depth (the lamps' 64x8 visibility pass).

### Targets and resolves

- Targets are images keyed by EDRAM base, format and pitch. EDRAM aliasing does not exist here: a 320-pitch pass on
  depth base 0 does not touch the 1280-pitch depth at base 0, which is mostly better than the console.
- **A deferred clear must end the current pass.** Clears are deferred until the target is next bound; a draw with the
  same targets does not bind again. In the decompression scene some particles bound the G-buffer before its clear, the
  clear then waited for the resolve and **wiped the whole G-buffer** (black scene, invisible Isaac).
- **Resolves of a rectangle not at (0, 0)** go into the texture at the copy's own base, at the rectangle's position.
  The emulation's habit of moving the base to the rectangle's 32-texel tile is the same bytes in guest memory but
  another texture here: the boss fight's blur texture kept garbage.
- **A texture fetched smaller than its resolve** (320x180 blur levels in 640-wide textures) is sampled from a copy of
  its corner; sampled whole, the final image came out in stretched vertical strips (space walks, boss).
- **New resolve textures are cleared once**: partly written ones otherwise show memory noise.
- **Several sizes per base**: one address is resolved at several sizes and formats in a frame (0A798000: 640x360 color
  and depth, 640x180); the other sizes are kept (`resueltas_otras_`) instead of recreated hundreds of times.
- **Depth resolves of a whole target swap images** instead of copying (the 960x960 shadow maps). Color does not: the
  1280-pitch targets are created 1280 tall, so their resolves (1280x720) never match.
- **Cube maps read as 2D** use face 0.

### Shadows

DS2 renders two 960x960 shadow maps per frame (depth-only, pitch 960) and its light shader does its own 4-tap
bilinear PCF with point-sampled taps at ±0.5 texel. The game sets its own depth bias (scale 0.32, offset 1e-5),
which the renderer applies. Some self-shadowing bands remain (open). The renderer's fallback bias
(`nfsmw_dk_sombras_sesgo_pitch`) only applies when the game sets none.

### The NFS shortcuts

The renderer came from NFS: Most Wanted with many game-specific shortcuts. For DS2: shadow-pass vegetation skipping
and "alpha-only PS" removal are off; "cheap PCF" is harmless (no DS2 shader uses it). Draw-skipping mode 1 (safe mode)
turns all of them off at once: the first thing to try when an artifact appears.

## 5. CPU at stock clocks

Three cores for 9-10 busy threads, at 1020 MHz. How it went, with the stack profile (section 6):

| Change | Effect |
| --- | --- |
| FPCR flush-to-zero fixed (0004), `MaybeYield` backoff (0005) | Removed per-instruction mode switches and yield storms |
| Audio compressor through its own bypass | A third of the mixer thread |
| GPU profile and frame traces only with telemetry | Removed periodic hitches |
| **`mtmsrd` regions per thread** (5.1) | Main thread from ~76 % (a quarter of it blocked) to ~97 % useful work; much better at stock |
| XMA kicks without the context lock (5.3), EA priority calls (5.2) | Audio thread lock time 18 % → 6 % |
| `MixWithGain` / `CopyWithGain` on NEON | ~14 % of a core in fights to a few % |

### 5.1 The interrupt lock

Xbox 360 code brackets its `lwarx`/`stwcx.` atomics with `mtmsrd` (interrupts off). ReXGlue's codegen turns each such
region into **one process-wide recursive mutex**. DS2's job scheduler (EARS: skinning and culling jobs, a main thread
queuing and four workers taking) takes it on every atomic, so all of them serialized, and with 3 cores a preempted
holder stalled everyone. The atomic itself is already a host compare-and-swap; the PCH now keeps the "interrupts off"
state per thread (what `mfmsr` reads back). `ds2_msr_cerrojo_global=true` restores the mutex. **Check this first on
any ReXGlue port that is slow with idle cores.**

### 5.2 Kernel object lookups

Every guest `WaitForSingleObject`, `ReleaseSemaphore` and `SetThreadPriority` takes the SDK's object-table lock
several times. EARS workers raise and drop their priority around every job (127 / -128); with
`ignore_thread_priorities` (the SDK default) the host priority never changes, so `EA::Thread::SetThreadPriority`
now just reports success.

### 5.3 Audio

RenderWare Audio's mixer (`RwAudioCore Dac`) was 50-95 % of a core in fights at stock clocks. Measured per stage
(`ds2_audio_tiempos`): voice playback/decoding ~25-36 %, `MixWithGain` 14 % (100-170k calls/s), sends 12 %, resampling
12 %, reverb only 2-4 %. The XMA decoder held each context's lock for a whole decode while the mixer's kick
(`XMAEnableContext`) took the same lock just to set a flag, and every kick (~1,500/s) signalled the decoder's event.

### 5.4 What remains

At stock the main thread is now game code (game logic, Havok, scene preparation, skinning `EARSGeom_SkinVerts*` ~2-6
%), with SDK time under 8 %. Light scenes are bound by the 307 MHz GPU (resolve copies 3.5-4.7 ms of ~30). Ideas not
done: color resolves by image swap (needs 720-tall 1280-pitch targets), native skinning (hand-scheduled VMX128 with
packed formats: risky), non-volatile guest loads (spin loops without barriers would hang).

**Videos at stock clocks (known issue).** DS2 decodes its full-motion videos (VP6) in software on the `Job Manager`
thread (`VP6_DecodeBlock`, `FilterBlock2dBil_FirstPass_Xenon`, ...). During the intro that thread sits at 99 % of a
core at 1020 MHz (about 88 % at 1785 MHz), while the presenter keeps flipping at 30 fps and the other threads are far
from busy: the decoder cannot produce frames fast enough and the video stutters. Streaming from the SD is not the cause
(~680 KB every ~2 s, ~10 ms each). A fix would be native NEON versions of the decoder's hottest functions, after a
stack profile taken during a video (`telemetria.flag` samples from startup).

## 6. Tools and how to debug

- **`telemetria.flag`** next to the NRO: screenshots every 5 s, frame dumps, GPU profile, frame traces, 1 ms stack
  sampler. Costs frames.
- **Shortcuts** (with `switch_atajos_depuracion=true`; ZL + ZR held): Down screenshot, Left frame dump (`volcado_NN/`,
  every resolve as BMP, depth with its range) and a trace of every draw of the next frame (state, vertex fetches,
  textures, depth base, resolves), Right draw-skipping modes (1 safe mode, then lights, additive, alpha-blended, quad
  lists, blended, alpha-tested) each followed by a capture, R stack sampling (the counter shows PRF), L GPU A/B lap.
- **The stack profile** (`logs/rex/rex_perfil.log`) gives per-thread pc/lr histograms and call chains as image
  offsets. Symbolize with `aarch64-none-elf-nm -n` of the same ELF (`deadspace2/out/sw/deadspace2`); aggregate across
  10-second windows. Self time in `svcArbitrateLock` = contended mutex; `svcWaitProcessWideKeyAtomic` = waiting.
- **Reading a frame**: the draw trace says what each draw had bound, the dump shows what each resolve produced. A
  resolve that is entirely the clear value (depth all at 1.0, color all 0) means the draws before it never landed: look
  for a clear or a pass change between them.

## 7. For the next port

1. Start from a recompiled tree that already runs on PC; keep the Switch glue outside it.
2. Expect `2001-0103` (mapping count), three cores instead of six hardware threads, and busy waits.
3. Dump shaders at creation (XDK `CreateVertexShader`/`CreatePixelShader`), translate ahead of time, precompile every
   variant to DKSH and ship that cache: runtime uam compiles are seconds each.
4. Handle D3D clears explicitly; know which draws are clears and which only look like them.
5. Treat resolves as writes into guest memory, not as "make a texture at this address".
6. Profile with stack sampling early: the `mtmsrd` global lock and kernel object locks cost more than any GPU work.
7. Ship no game data; check every input file by hash; keep the build reproducible (pin the recompiled tree).

## Credits

This port stands on: Dead-Space-2-Recomp (bifftannen1), NFSMW-NX (StevensND), the ReXGlue SDK (Tom Clay) and Xenia,
XenosRecomp (hedge-dev), the uam patches (centollOS), devkitPro/libnx/deko3d (switchbrew), and the libraries in
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md). Full list in the [README](../README.md#credits).
