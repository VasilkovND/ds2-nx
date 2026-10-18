# What goes in the SDK and what stays in a game folder

`sdk/` is now a **git submodule** → [`nx-mod/rexglue-sdk-nx`](https://github.com/nx-mod/rexglue-sdk-nx)
on its `switch` branch (a ReXGlue v0.10.0 fork). Its own layout and provenance live in
`sdk/switch/README.md` and `sdk/switch/PORTED.md`.

## The rule

- **`sdk/`** — anything another game could use unchanged: the runtime, Switch platform layer,
  codegen, the deko3d presentation layer, the game-neutral `rex::x360` renderer, shared tools.
- **A game folder** (`halo3/`, `nfsmw_pal/`) — only what is about that game's executable: its
  manifest, function/jump-table lists, address hooks, native function replacements, settings.

When a game-specific piece turns out to have a general part, the general part moves into the SDK
and the game keeps a thin layer (a hook table or config) on top.

## Status

- The deko3d layer, the `rex::x360` renderer **backend** (`RenderDk`), the shared renderer types,
  the universal shader descriptor and the Switch platform layer are all **in the SDK** now.
- **Vulkan/D3D12 stay in the SDK** for desktop; the Switch build simply does not link them
  (deko3d is the Switch renderer). Selected per platform in CMake.
- The deko files still carry Spanish names; an English pass is tracked in the SDK repo.

## Still game-side (by design)

Halo 3: `halo3_gpu.cpp` (its deko3d graphics system — ring reader, interrupts, presenter) and
`halo3_first_launch.cpp`. NFS: sound-engine hooks, native function replacements, race trims,
F4 settings, D3D wait and cutscene hooks. Controls and audio output are already shared in the SDK.
