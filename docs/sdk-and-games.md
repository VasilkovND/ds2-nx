# What goes in sdk/ and what stays in a game folder

## The rule

- **`sdk/`**: anything another game could use as it is. The runtime, the Switch layer, codegen, the deko3d
  presentation layer, renderer pieces that do not depend on a game, tools for any game.
- **A game folder** (`nfsmw_pal/`, `halo3/`, ...): what is about that game's executable. Its manifest and
  overrides, function and jump-table lists, hooks at its addresses, native replacements of its functions, and
  its own settings.

When something game-specific turns out to have a general part, the general part moves to `sdk/` and the game
keeps a thin layer on top (for example a hook table or a configuration).

## Done

| What | Now in | Came from |
|---|---|---|
| deko3d core: device, queue, memory, swapchain | `sdk/src/ui/deko3d/nucleo.*` | `nfsmw_pal/src/nfsmw_dk_nucleo.*` |
| SPIR-V to GLSL for uam, DKSH cache, uam compile worker | `sdk/src/ui/deko3d/{glsl,cache_dksh,compilador}.*` | `nfsmw_dk_*` |
| Provider, presenter and ImGui drawer on deko3d | `sdk/src/ui/deko3d/presentador.*` | `nfsmw_dk_presentador.*` |
| CMake for all of it (+ uam, SPIRV-Cross, xxHash) | `rexglue_switch_add_deko3d(<target>)` | NFS's CMakeLists |
| Guest DLLs on the Switch, imports between DLLs | `sdk/cmake/rexglue_switch.cmake`, codegen | Halo 3 |
| Disc image extraction (streamed, resumable) | `sdk/src/filesystem/disc_image_extract.cpp` | Halo 3 |
| `posix_memalign` for deko3d titles | `sdk/src/ui/switch_dk_libc.cpp` | `nfsmw_pal/src/nfsmw_dk_libc.cpp` |
| Codegen fixes (barriers, atomics, vector, FP) | `sdk/src/codegen` | upstream ReXGlue and forks |

The deko3d files keep their Spanish names and `nfsmw::deko` still works for NFS's code (a `using` in each
header) until NFS's renderer is renamed.

## Next

Halo 3 (testable now):

- `halo3/src/halo3_first_launch.cpp`: the first-launch copy with its progress screen, for any game.
- `halo3/src/halo3_gpu.cpp`: the graphics system for deko3d titles without the Xenos plugin (ring reader,
  interrupts, GPU writes, presenter). It is the small version of NFS's `nfsmw_nativo_sistema.cpp`; the two should
  become one SDK graphics system with a hook interface for what each game adds.

NFS (moved in one batch, then checked with a full NFS build):

- Renderer pieces that do not depend on the game: `nfsmw_nativo_xenos.h` (Xbox 360 GPU arithmetic),
  `nfsmw_nativo_vertices_dedupe.h`, `nfsmw_nativo_peticion.h` (what the ring reader hands a backend per draw),
  `nfsmw_microcodigo_versiones.h`, `nfsmw_nativo_captura.*` (PNG captures).
- Runtime helpers: `nfsmw_hilos_switch.cpp` (thread priority and core), `nfsmw_informe_diferido.h` (log lines off
  the working threads), `nfsmw_esperas_tiron.h` (stutter accounting).
- PC and development tools: `nfsmw_pgo.cpp`, `nfsmw_perfil_pc.*`, `nfsmw_pc_coma_flotante.cpp`.
- `nfsmw_video_wmv3.*`: WMV movies with FFmpeg, for any game that has them.

Stays in `nfsmw_pal/`: the sound-engine hooks (`nfsmw_audio_*`), the native replacements of NFS functions
(`*_nativo`), the race trims (`recortes_carrera`, `recorte_sombras`, `sombras_lod`, `escenario_lod`,
`streaming_anticipacion`, `reflejo_demanda`), its F4 settings, its D3D wait hooks and its cutscene hooks.

Controls and audio output are already in `sdk/` (`CreateDefaultInputSystem`, `SwitchAudioSystem`, the XMA
decoder) and every game uses them.
