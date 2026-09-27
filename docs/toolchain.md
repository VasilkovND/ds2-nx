# Toolchain: code generation, LTO, PGO and function ordering

How the game's PowerPC code becomes an ARM64 NRO, and the compiler-level optimisations applied on top.
Most of this carries over unchanged to any other game built with ReXGlue for the Switch.

## Pipeline

1. **Code generation, on the PC.** The ReXGlue code generator (`rexglue codegen`, built for Windows
   from the SDK with the patches in [`sdk/`](../sdk)) reads the game's `default.xex` and writes C++,
   one function per guest function. The generated code is derived from the game and is never
   distributed; everyone generates it from their own copy.
2. **Post-passes on the generated code**, after every code generation:
   - `tools/lee_antes.py` looks for registers read before being written in split function fragments
     (see [register locals](#registers-as-c-locals)).
   - `tools/llamadas_directas.py` turns calls through weak aliases into direct calls
     (see [direct calls](#direct-calls)).
3. **Cross-compilation** with devkitA64 (GCC 16) through CMake and Ninja, with LTO, PGO and a section
   ordering file, then packaging as an NRO.
4. **Checks before shipping** (the release script runs them):
   - `nm nfsmw | grep " _start$"` must print address `0000000000000000` (see
     [function ordering](#function-ordering)).
   - `tools/comprobar_clave_textura.py` checks the texture key code in the final ELF (see
     [strict aliasing](#an-lto-and-pgo-pitfall-strict-aliasing-in-xxhash)).
   - The number of installed hooks must match the reference build.

## CMake on Windows

- **Never run CMake from the devkitPro MSYS shell.** The `cmake` found there is the MSYS build, which
  treats `D:/devkitPro` as a relative path and breaks the configuration. Always use the Windows build
  (`C:\Program Files\CMake\bin\cmake.exe`), from PowerShell, for both configuring and building. If a
  build directory was configured by the MSYS CMake, delete its `CMakeFiles/<version>` folder and
  reconfigure.
- Windows PowerShell 5.1 aborts `& cmake ... *> log` on the first line written to stderr, even a
  warning. Run long builds through `Start-Process` with redirected output instead.
- Keep a single Ninja version. An older Ninja (for example the one shipped with Visual Studio) drops
  the `.ninja_deps` written by a newer one and rebuilds every object, which for this game is around
  686 objects and half an hour instead of one minute.
- An incremental build (`cmake --build <dir>`) is the normal way to produce a new NRO. Check with
  `cmake --build <dir> -- -n` first: tens of targets is normal, hundreds means something forced a
  full rebuild.

## Registers as C++ locals

By default the generated code keeps every guest register in a context structure in memory. ReXGlue
has options to turn groups of registers into C++ locals, which the compiler can keep in real
registers:

| Option | Effect in this game |
|---|---|
| `cr_as_local` | condition register fields as locals |
| `xer_as_local` | 172,403 accesses to `ctx.xer` removed |
| `ctr_as_local` | 32,637 accesses to `ctx.ctr` removed |
| `non_volatile_as_local` | r14-r31 as locals; 23,857 calls to `__savegprlr_*` / `__restgprlr_*` removed |

The PowerPC ABI saves and restores r14-r31 through out-of-line helpers called from every prologue and
epilogue. On the console, about 14 % of the main game thread was spent inside `__restgprlr_29` and
`__savegprlr_29` alone.

Two things break when non-volatile registers become locals:

1. **Hooks that read a caller's non-volatile registers from the context.** A hook installed at the
   entry of a guest function that reads, for example, `ctx.r31` of its caller now reads a stale value.
   Mark those functions with `share_registers` in `overrides.toml` so their callers spill the locals
   into the context before calling them.
2. **Split functions.** The analyser splits some guest functions into fragments, and the parent jumps
   into a fragment (with `b`, `bctr` or a jump table) with r14-r31, `cr`, `ctr` or `xer` already set
   up. With locals, the fragment received them as zero, and the game crashed as soon as the first
   video started. The code generator was patched (`emit_call_sharing_registers`): a jump to a
   function marked `share_registers`, and every indirect `bctr`, passes the locals it has modified
   through the context and restores it afterwards, and functions with `share_registers` keep `cr`,
   `ctr` and `xer` in the context. 198 fragments are marked. `tools/lee_antes.py` finds registers read
   before written in the generated code; after marking, only two known false positives (loops)
   remain. Run it after every code generation, because the gap list the marks live in can be
   regenerated.

Rejected: `non_argument_as_local` (breaks the r12 convention of `__savevmx`) and `skip_lr` (21 reads
of `ctx.lr` in the game).

**Native CRT functions.** ReXGlue can replace guest C runtime functions with native implementations
(`[rexcrt]` in the configuration, by guest address). `memmove` (the game's real `memcpy`, 950 call
sites) and `strncpy` gained from it; `memset` was already hooked with a native implementation.

## Direct calls

With GCC, `DEFINE_REX_FUNC` makes each `sub_XXXXXXXX` a **weak** alias of `__imp__sub_XXXXXXXX`, so
that a hook (`REX_HOOK_RAW`) can replace it at link time. The generated code always calls
`sub_XXXXXXXX`. A weak symbol cannot be inlined, not within the same file and not with LTO, so link
time optimisation could barely touch the game code.

`tools/llamadas_directas.py` rewrites `sub_X(ctx, base);` into `__imp__sub_X(ctx, base);` whenever
`sub_X` has no hook: 79,612 of the 83,077 calls in this game.

- An address counts as hooked if it appears anywhere in the application sources, the SDK or the
  configuration files. Hooks are sometimes built with token pasting (`sub_##addr`), so looking for
  `REX_HOOK_RAW(sub_...)` is not enough. Verify with `nm` that every `sub_` defined by the compiled
  objects is on the hooked list.
- The dispatch table for indirect calls is left untouched, so indirect calls still reach hooks.
- `--deshacer` reverts the rewrite.
- A new code generation silently loses the direct calls. Run the tool after every code generation.

## LTO

Enabled for the Switch build only. Both the recompiled game library and the application are compiled
with `-flto -fno-fat-lto-objects` and linked with `-flto=2 -flto-partition=balanced`. The link needs a
lot of memory on the build machine; watch free memory and keep the number of LTO jobs low.

## PGO with GCC 16 on Horizon

Profile-guided optimisation is done in two builds from the same build directory, with the same
generated code and the same sources:

1. **Instrumented build** (`-fprofile-generate`, LTO off). For this game it took 66 minutes to build
   and produced an 82.7 MB NRO with 16 MB of `.bss` for the counters.
   - GCC embeds host paths for the `.gcda` files. A `--wrap=fopen` in the application rewrites them to
     a folder on the SD card.
   - A background thread calls `__gcov_dump` and `__gcov_reset` every three minutes, so a crash or a
     forced exit does not lose the session. libgcov adds to the counters already in each file.
   - Play normally for as long as possible: menus, races, videos, every mode.
2. **Optimised build** (`-fprofile-use=<folder> -fprofile-partial-training`, LTO on), with the
   `.gcda` files copied from the SD card under their original names. `-fprofile-prefix-path` must be
   the build directory in native Windows form, with backslashes, or the names do not match.

Pitfalls, each of which cost a build:

- **`-fno-profile-values` is mandatory.** libgcov's indirect call profiler reads thread-local storage
  with `mrs tpidr_el0`, which is 0 on Horizon because libnx uses `-mtp=soft`. The instrumented build
  crashed before `main`, during static initialisation. Check that the instrumented binary does not
  contain `__gcov_indirect_call_profiler`. Branch and function profiles still work.
- **`-fprofile-correction` is needed when using the profile.** With `-fprofile-update=single`,
  counters updated from several threads at once come out negative and GCC rejects them as corrupted
  profile data (49 errors in the first attempt).
- **Do not regenerate the code or edit sources between the two builds.** GCC matches profile entries
  to functions by identifiers derived from names and source locations:
  - a public function is identified by GCC's CRC32 (polynomial 0x04C11DB7, not reflected, including the
    terminating zero) of its assembler name, masked with 0x7FFFFFFF;
  - its line checksum is `crc(crc(line, path with /), name)`; a mismatch only produces a warning;
  - the identifier of a **local** function includes the source path, so a profile only applies to a
    build that uses the same paths.
- Warnings about profile mismatches in recompiled functions are hidden, because those functions are
  declared through a macro in a system header. Use `-Wsystem-headers` to see them, and test a
  deliberately corrupted profile to prove that lookups really work.

**Reusing a profile for another edition.** Other editions of the game are different executables with
their guest functions at other addresses, so their function names differ. `tools/editions` translates
the profile of the reference edition to another one by renaming each function's counters through the
address map between the two executables, and the build of the other edition runs from the same paths
as the reference build. See [editions.md](editions.md).

## Function ordering

`app/orden_funciones.ld` is passed as a section ordering file and places the hottest functions at
the start of `.text`, generated from profiles of the game.

The ordering file places what it lists **before** the sections of the linker script, including
`KEEP(*(.crt0))`. Four builds put `memcpy` at address 0 and moved the libnx start-up code, and none
of them booted (Atmosphère reported an undefined instruction at `+0x14`). The first line inside
`.text` must be `KEEP (*(.crt0))`, and every release checks that `_start` is at address 0. Relinking
"without changing a single instruction" can still break start-up.

With `memcpy` at 0x140, `addr2line` attributes addresses near zero to unrelated debug information
(Rust's `fmod` from discarded sections, TLS variables with small values). Below 0x3000, translate
addresses with `objdump -d` or with `nm -S` filtered to code symbols.

## An LTO and PGO pitfall: strict aliasing in xxHash

Three builds lost smoothness with no change in logic. xxHash 0.8.3 selects
`XXH_FORCE_MEMORY_ACCESS 1` for GCC, which reads unaligned `uint64_t` values without `may_alias`. Once
LTO and PGO inlined `XXH3_64bits` into the function that builds texture cache keys (it had been a call
until then), strict aliasing let GCC move an 8-byte load of the key above the store that completes
it. Keys carried four bytes of garbage from the previous call, lookups missed, and textures were
uploaded ten times more often (17,075 uploads against 1,743). The fix is `XXH_FORCE_MEMORY_ACCESS 0`
before `XXH_INLINE_ALL` in every file that includes xxHash, with an `#error` if it comes too late, a
runtime guard that recomputes a sample of keys, and `tools/comprobar_clave_textura.py`, which checks
the compiled code in the final ELF.

The lesson for LTO and PGO in general: inlining decisions change as the program grows, so undefined
behaviour that was harmless in one build can appear in the next one without touching the code
involved.

## Debugging crashes on the console

- `logs/rex/rex_crash.log` and the Atmosphère crash report give the full stack.
- Translate addresses with `aarch64-none-elf-addr2line -f -C -i -e <unstripped ELF>`, subtracting 4
  from return addresses. Keep the unstripped ELF of every build that leaves your machine.
- For hangs, the watchdog in the application dumps the stacks of every thread to `logs/`.

## Rebuilding the Vulkan driver

The NRO links Mesa statically from an installed SDK folder. See [mesa.md](mesa.md) for the build
environment. After installing a new driver build, **delete the linked executable** before building
the NRO: Ninja does not track the imported library, reports "no work to do" and packages the old
driver. Check that the new code is really in the executable, for example by looking for a string that
only the new code contains.
