# Mesa (NVK on Horizon) changes

The NRO links the Vulkan driver statically: `libvulkan.a` from a Mesa build for Horizon. This port uses
[danfromtico/mesa-switch](https://github.com/danfromtico/mesa-switch) (Mesa 26.2.1 with a Horizon backend for
nouveau and NVK) plus the changes in `mesa-switch-nfsmw.patch`.

- Base commit: `1a8c1a66d6f` of danfromtico/mesa-switch.
- `mesa-switch-nfsmw.patch` applied on that commit gives exactly the tree the released NROs were built with
  (35 files).
- The Switch-only parts are in the Horizon backend (`src/nouveau/horizon/` and `src/nouveau/vulkan/nvkmd/switch/`,
  where ZCULL lives) or inside `#ifdef HAVE_SWITCH_PLATFORM` blocks: the draw path, the set 4 deltas and the
  structure shared with the app for measurement, in `nvk_cmd_buffer.c/h`, `nvk_cmd_draw.c` and `vk_pipeline.c/h`.
- `HAVE_SWITCH_PLATFORM` is not written in the patch or in any header: mesa-switch's `meson.build` generates it.
  It passes `-DHAVE_<PLATFORM>_PLATFORM` for every platform of the build
  ([lines 620-622](https://github.com/danfromtico/mesa-switch/blob/1a8c1a66d6f/meson.build#L620-L622), the same
  mechanism as upstream's `HAVE_X11_PLATFORM`), and a build for Horizon has the platform `switch`
  ([lines 478-479](https://github.com/danfromtico/mesa-switch/blob/1a8c1a66d6f/meson.build#L478-L479)), so every
  file of a Switch build is compiled with `-DHAVE_SWITCH_PLATFORM`.
- The NAK compiler changes, the fixes taken from later mesa-switch commits and the build changes are not guarded:
  they apply wherever this tree is built. Some changes can also be switched off at run time with the environment
  variables listed below, which the app sets through its cvars.

```sh
git clone https://github.com/danfromtico/mesa-switch.git
cd mesa-switch
git checkout 1a8c1a66d6f
git apply ../nfsmw-nx/mesa/mesa-switch-nfsmw.patch
```

Then build with the scripts of mesa-switch (`build-unified.sh`, see its README) or incrementally with
`build_mesa_msys2.sh`. The build produces an SDK folder; the app is configured with
`-DREXGLUE_SWITCH_NVK_SDK=<sdk>/opt/devkitpro/portlibs/switch` (see [docs/building.md](../docs/building.md)).

## What the patch changes

Descriptions of why each change exists and what it was worth are in [docs/mesa.md](../docs/mesa.md). In short:

| Area | Files | What |
|---|---|---|
| ZCULL | `nouveau_horizon*.c/h`, `nvkmd_switch_pdev.c`, `nvkmd_switch_dev.c`, `nvk_image.c` | Hierarchical Z culling on GM20B under Horizon: ZCULL geometry from the kernel, a ZCULL buffer per depth image, context switch setup |
| Copy engine | `nvk_cmd_meta.c`, `nvk_device.c` | Buffer copies on the copy engine instead of compute shaders. Off unless `NVK_COPY_ENGINE=1` |
| Uncached CPU writes | `nvk_*` memory paths | Command, stream and CPU-write-only memory mapped uncached on Horizon (`NVK_SWITCH_*_UNCACHED`) |
| Set 4 by differences | `nvk_cmd_buffer.c/h`, `nvk_cmd_draw.c` | The app's per-draw uniform set is written as a delta against the previous draw instead of in full |
| Draw path | `nvk_cmd_draw.c`, `nvk_cmd_buffer.c/h`, `vk_pipeline.c/h` | Cheaper per-draw emission and constant buffer rebinding, dynamic state shortcuts and prefetch of the pipeline state; a `nvk_switch_dibujo` structure shared with the app for measurement |
| NAK scheduling | `opt_instr_sched_common.rs`, `nak_nir.c` | Texture and global memory latency raised from 32 to 200 cycles for scheduling; NIR `peephole_select` limit 0 -> 8 |
| NAK SM50 fix | `sm50.rs` | `FADD` with a long immediate and `.SAT`: FADD32I has no saturate bit on Maxwell, so the immediate goes to a register |
| Shader cache | `nvk_shader.c` | A revision of these compiler changes is part of the cache key, so old binaries are not reused |
| Upstream fixes | `nvk_mem_arena.*`, `nvk_descriptor_table.*`, `nvk_query_pool.c`, `nvk_heap.h`, `nvk_queue.c`, ... | Fixes taken from later danfromtico/mesa-switch commits |
| Build | `build-*.sh`, `rustc-*-wrapper.sh`, `bindgen-switch-wrapper.sh`, `meson.build` | Building on MSYS2 MINGW64: `__sFILE` opaque for bindgen, LLVM `demangle` component, linker detection, `-Doptimization=2` |

Run-time switches read by the driver: `NVK_COPY_ENGINE`, `NVK_SWITCH_CMD_MEM_CPU_UNCACHED`,
`NVK_SWITCH_CPU_WRITE_MEM_UNCACHED`, `NVK_SWITCH_MEM_STREAM_CPU_UNCACHED`, `NVK_SWITCH_NO_UBO_CBUF`,
`NVK_SWITCH_DYN_UBO_DELTA`, `NVK_SHADER_STATS` and `NVK_SUBTILING_KNOB`. The app can set any of them with the cvar
`nfsmw_mesa_entorno` (`"VAR=value;VAR=value"`); the released configuration leaves it empty, so the driver defaults
apply. ZCULL, the set 4 deltas and the draw path changes are requested by the app itself (cvars `nfsmw_nativo_zcull`,
`nfsmw_nativo_set4_diferencias` and `nfsmw_nativo_nvk_*`, all on by default) through symbols both sides share. The
set 4 deltas and the draw path changes carry a self-check in the driver that compares their result with the
original path on sampled draws and turns them off if it ever sees a difference.

## Rebuilding after a change

1. Incremental build and install: `build_mesa_msys2.sh` (MSYS2 MINGW64 shell).
2. Copy `libvulkan.a` (NAK is linked into it), `libnvk.a` and `libnak_rs.a` to the SDK folder the app uses.
3. Delete `app/out/sw8/nfsmw` and `app/out/sw8/nfsmw.nro` before building the app again: Ninja does not see a
   changed static library outside the build tree and would keep the old driver in the NRO.
4. When the change touches NAK, raise the revision in `nvk_shader.c`: on Horizon the driver id is the package
   version, so the pipeline cache UUID and the disk cache keys would not change otherwise.
