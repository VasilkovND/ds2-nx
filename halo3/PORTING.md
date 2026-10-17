# Porting notes: Halo 3

What it took to bring Halo 3 into this repository, in the order it was done. Each step says why it was needed, so
the next game (ODST, Reach, ...) can follow the same path. Most steps are not about Halo: they are what any game
meets on this SDK. Updated as the port goes on.

For the general route, read [docs/porting-another-game.md](../docs/porting-another-game.md) first. This file is
the concrete record of one port.

## Boots on the Switch so far

| Boot | Where it stopped | Cause | Fix |
|---|---|---|---|
| 1 | `Call to invalid or unregistered function at 0x822F7B10` | A function that only a vtable or callback reaches; ReXGlue folded it into the function before it | [Missing functions](#4-missing-functions) |
| 2 | Same error at `0x88065DB8`, in WaveShell-Xbox.dll | The same, in DLL code | Same list per DLL |
| 3 | No crash; stuck before its first frame | No graphics system: the render thread waited for a GPU that never read its commands | [Graphics system](#5-a-graphics-system) |
| 4 | Write to guest address 0 in `sub_822AF7D8` (a decompressor) | XAPI fibers: `SwitchToFiber` cannot be translated as plain code | [Fibers](#6-fibers) |
| 5 | No crash: about 24 frames/s with 4 draws each, black screen, audio static | Nothing is drawn yet; the static is underrun: the game fills only ~310 of ~1,880 audio requests per 10 s, with its three cores at 298 % | Stack samples (`logs/rex/perfil_pilas.flag`), see boot 7 |
| 6 | Closed after ~15 s, same as 5 | Too short for the profiler, which writes every 10 s | Run a full minute when measuring |
| 7 | CPU exception (`brk`) in `sub_8213FB68`: "Switch case out of range" | A jump table ReXGlue sized at one entry; the index has no bounds check before the jump | [Jump tables](#7-jump-tables) |
| 7 | (same run, stack samples) one game thread at 98 % in one loop | A worker polling its queue without resting, built for the 360's six hardware threads | [Busy waits](#8-busy-waits) |
| 8 | Read of guest `0xE0` in `sub_82107DD8`, called from `sub_8231D8D0` on the main thread, ~14 s in, just after the game gave music control back (a transition) | A game object looked up by handle was missing (its slot in the per-thread object table was empty). Suspect: content signature checks failing, since `XeCryptBnQwNeRsaPubCrypt` wrote no result | [Kernel exports](#9-kernel-exports-that-only-worked-on-windows) (to be confirmed) |

## 1. The game folder

1. **Extract the disc** and keep it out of git: `halo3/assets/game_root` (ignored) points at it.
2. **Scaffold** with `rexglue init --project-name halo3 --xex-path <default.xex> --game-root <disc> --project-root
   halo3 --scan-dll`. `--scan-dll` finds the DLLs the game loads (Halo 3: `WaveShell-Xbox.dll`, `WavesLibDLL.dll`,
   `waves/L360.dll`, `waves/Q10.dll`), but it lowercases their file paths: fix `file_path` in
   `halo3_manifest.toml` to the case on disk. `guest_path` stays lowercase.
3. **Codegen** with `REXGLUE=sdk/out/linux-arm64/rexglue tools/codegen.sh halo3`. It finds the game's
   `*_manifest.toml` and writes `generated/` (ignored).
4. **The NRO**: the `REXGLUE_PLATFORM_SWITCH` block in `CMakeLists.txt` (`rexglue_switch_add_nro`). The icon is the
   game's own title image: `rexglue init ... achievements` extracts it to `assets/icons/title.png`; scale it to a
   256x256 `assets/icon.jpg`.
5. **Where the game is on the SD card**: `src/halo3_app.h` (`OnConfigurePaths`) looks for `game_root/`, `game/`, the
   NRO's own folder, then an `.iso`, all next to the NRO. On the console: `sdmc:/halo3-nx/halo3-nx.nro` and the disc
   in `sdmc:/halo3-nx/game/`.

## 2. SDK changes the first build needed

These live in `sdk/` and serve every game.

- **Guest DLLs on the Switch.** There is no dynamic loading on Horizon, so each DLL module is built as a static
  archive and linked into the NRO (`rexglue_switch_link_module` in `sdk/cmake/rexglue_switch.cmake`). Its
  `ReXModule_Register` symbols are renamed per module, and `RegisterStaticLibrary` lets the Switch's
  `DynamicLibrary::Load` find it by name.
- **Imports between DLLs.** L360 and Q10 import from WavesLibDLL by ordinal; codegen only resolved kernel exports
  (120 undefined thunks). Unresolved module imports are now recorded, and their thunks call
  `rex::runtime::ResolveModuleImport(library, ordinal)` at run time.
- **Strict float flags on the game's own targets** (`-ffp-contract=off -fno-strict-aliasing`, in
  `rexglue_apply_target_settings`). `add_compile_options` in the SDK only reached SDK targets, so the recompiled
  code could be fused into FMA and round differently from the Xbox 360. Learned from fc3-nx.
- **`posix_memalign`** moved from NFS's folder into the SDK (`sdk/src/ui/switch_dk_libc.cpp`): every deko3d build
  needs it.
- **`dll_targets_cmake.inja`**: the codegen dependency only exists in the host build, so it is wrapped in
  `if(TARGET ...)`.

## 3. Two tail-call targets

Codegen stopped on two branch targets reached only by a plain `b` (a tail call), which the analysis does not follow
into a new function. Both were function starts in the earlier XenonRecomp port's map, so they are declared in
`overrides.toml`. The same happened twice more after step 4 (a split leaves a `b` pointing into the middle of a new
function); those are declared there too.

## 4. Missing functions

ReXGlue's analysis follows calls and branches. A function reached only through a pointer in data (a vtable, a
callback table) is folded into the function before it, and the game stops the first time it is called:
`Call to invalid or unregistered function at <address>`.

What fixed it here: the earlier port's XenonAnalyse function map (`assets/xenonanalyse_func_mapping.cpp` for the
game, and its per-DLL maps) minus ReXGlue's own table. The difference is declared in
`xenonanalyse_functions.toml` (game) and `xenonanalyse_functions_<dll>.toml` (one per DLL, included from each
`[[modules]]` entry of the manifest).

- **Leave out import thunks.** XenonAnalyse lists them as functions, ReXGlue maps them as imports. Declaring them
  makes codegen print `[functions] 0x... outranked by import`, drop the entry, and still emit calls to a `sub_` name
  that has no body: 290 undefined symbols at link time.
- **Check before building** that every name in `generated/<module>/*_init.cpp` has a `DEFINE_REX_FUNC` body; only
  `__imp__` names may lack one.
- Without an earlier map, fc3-nx's pointer sweep (`tools/pointer_fixpoint.py`, with the image from
  `REXGLUE_DUMP_IMAGE=out/halo3_image.bin` during codegen) finds the same kind of function from the data side.

## 5. A graphics system

The deko3d build has no Xenos emulation plugin. Without any graphics system the runtime drops the game's `Vd*` calls
(`no GPU emulation loaded`), and the render thread waits forever for the GPU to read its command ring.

`src/halo3_gpu.cpp` stands in for the GPU, following NFS's native renderer (`nfsmw_pal/src/nfsmw_nativo_sistema.cpp`)
and the SDK's command processor: MMIO registers, the ring read pointer, vblank and ring interrupts, and the GPU's
writes to memory. It is installed from `OnPreSetup` in `src/halo3_app.h`. It draws nothing yet; every 10 s it logs
swaps, draws and packets, so the log shows whether the game advances. The deko3d renderer will be built on it.

## 6. Fibers

Halo 3 runs its map decompression on XAPI fibers, which are linked into `default.xex`. The real `SwitchToFiber` saves
the caller's registers, loads the target fiber's (r1 and LR too) and returns into the target. Recompiled code returns
through the host stack, so that cannot be translated: after a switch, registers came back wrong (r26 was 0) and the
decompressor wrote to address 0.

The SDK already replaces XAPI's fiber functions with host fibers (`sdk/src/kernel/crt/threading.cpp`). They only need
the addresses, in `[rexcrt]` in `overrides.toml`. All five go together: the SDK's `SwitchToFiber` only knows the fibers
its own `CreateFiber` or `ConvertThreadToFiber` registered.

How to find them in another game: `SwitchToFiber`'s body is unmistakable (it reads `r13` -> `+0x100` and stores every
non-volatile register into the fiber). The others sit next to it and read the same KTHREAD field (`+0x164`, the
current fiber); `ConvertThreadToFiber` fails with error 1280, `ConvertFiberToThread` with 1281, `CreateFiber` rounds
its stack up to 16 KiB.

## 7. Jump tables

ReXGlue sizes a jump table from the bounds check (`cmplwi`) before the `bctr`. Where the index range is guaranteed
elsewhere there is no check, and it guessed one entry: the generated `switch` traps (`__builtin_trap(); // Switch case
out of range`) on every other index.

- The earlier port's tables (XenonAnalyse, corrected by its `tools/h3analysis.py`) are converted to
  `[[switch_tables]]` in `xenonanalyse_switch_tables.toml` and `xenonanalyse_switch_tables_<dll>.toml`, included
  like the function lists. `address` is the `bctr` (XenonRecomp's `base`), `register` its `r`.
- Five more tables that XenonAnalyse missed as well are in `overrides.toml`. Their size comes from the image: a table
  starts right after its `bctr` and ends where the code resumes, at its lowest target.
- Check: count the generated `switch` statements with a single `case`. It should be 0, or each one explained.

## 8. Busy waits

Stack samples (create `logs/rex/perfil_pilas.flag` next to the logs; `rex_perfil.log` then lists, per thread, where
it spends its time) showed a worker thread at 98 % in `sub_821A8500`: it polls a slot of its queue and the kernel's
millisecond counter for up to 66 ms without resting. With three cores that starves everything else, and the audio
underran (static). A `[[midasm_hook]]` at the loop's repeat (`0x821A85C4`) calls `Halo3WorkerPollYield` in
`src/halo3_hooks.cpp`, which sleeps 100 us. Expect more of these: the 360 had six hardware threads, the Switch gives
the game three cores.

## 9. Kernel exports that only worked on Windows

`XeCryptBnQwNeRsaPubCrypt` (the RSA public operation, `b = a^e mod n`) was implemented with BCrypt on Windows only;
elsewhere it logged an error, wrote nothing and returned success, so every signature the game checks with it fails.
Halo 3 calls it several times at startup. `sdk/src/kernel/xboxkrnl/xboxkrnl_crypt.cpp` now has a portable version
(multi-word arithmetic, checked against Python's `pow` on random 64- to 2048-bit numbers).

The lead came from xenon-pc-comp's notes (retail Halo 3 on ReXGlue for Linux; read as reference, no code copied).
They list other runtime pieces worth checking for the next game: `XeKeysConsolePrivateKeySign` must return TRUE
(Reach retries forever otherwise), the utility-partition cache drives, and a pause behind the `db16cyc` spin hint.

## How each crash was read

- **Logs on the console**: `<NRO folder>/logs/` (one `halo3_NNN.log` per run) and `<NRO folder>/logs/rex/`
  (`rex_crash.log`, appended on every crash: read the last `====` block; `rex_perfil.log`, CPU per thread every 10 s).
  Fetch them over FTP.
- **Where a thread spends its time**: `logs/rex/perfil_pilas.flag` turns on stack samples in `rex_perfil.log`.
  Offsets are symbolized the same way as crashes.
- **Which function crashed**: the crash log gives `imagen+0x...` offsets. The ELF links at 0, so they are ELF
  addresses: look them up in `aarch64-none-elf-nm -n out/sw/halo3`, and disassemble around them with
  `aarch64-none-elf-objdump -d --start-address=...` to see which guest register held the bad address (the context
  is `x19`; `ctx.rN` is at offset `N * 8`).
- **The guest code**: the generated C++ keeps every PowerPC instruction as a comment, so it doubles as a
  disassembly. Raw words come from the image dump (`REXGLUE_DUMP_IMAGE`).
- **The earlier port** (`~/switch/halo3-nx-xenonrecomp`, archived) already met several of these problems; its
  config and hooks say where (for example, which addresses it hooked for fibers).

## Still open

- Nothing is drawn: the deko3d renderer, and Halo's shaders (they come from the map files, not the executable).
- `cache0:` / `cache1:` (the 360's cache partition) are left unmounted, as upstream does; the game reads from the
  disc instead.
- `XeKeysConsolePrivateKeySign` returns 0 (failure, as a BOOL); Halo 3 has not been seen calling it.
- `db16cyc` (the 360's "pause for 16 cycles" hint, 12 sites in Halo 3) compiles to nothing, so loops built around
  it spin flat out. A pause there would help every game; it needs a codegen change.
- The cross-DLL import lookup tries the bare name before `.dll`, which floods the log with
  `'WavesLibDLL' -> [no device]`.
