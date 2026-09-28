# From the game's code to the NRO (the toolchain)

## In short

- This page explains how the game's Xbox 360 code becomes a Switch program (an [NRO](glossary.md#nro)), and the
  compiler options that make it faster.
- Almost all of it works the same for any other game built with [ReXGlue](glossary.md#rexglue) for the Switch.
- For the exact commands, see [building.md](building.md). This page explains what those commands do and why.

## The path from the game to the NRO

1. **Translate the game to C++, on the PC.** ReXGlue's code generator (`rexglue codegen`, built for Windows from
   [`sdk/`](../sdk), which has this port's changes) reads the game's `default.xex` and writes C++: one C++ function for
   each function of the game. This code is made from the game, so it is never shared: everyone makes it from their own
   copy.
2. **Fix up the generated code.** Two tools run after every code generation:
   - `tools/lee_antes.py` ("read before") looks for registers that are read before they are written in functions that
     were split in pieces (see [Registers as C++ locals](#registers-as-c-locals)).
   - `tools/llamadas_directas.py` ("direct calls") makes calls between game functions direct, so the compiler can
     optimize them (see [Direct calls](#direct-calls)).
3. **Compile for the Switch** with devkitA64 (GCC 16), through CMake and Ninja, with [LTO](glossary.md#lto),
   [PGO](glossary.md#pgo) and [function ordering](glossary.md#function-ordering). The result is packed as an NRO.
4. **Check the result** before you share a build:
   - `nm nfsmw | grep " _start$"` must print the address `0000000000000000`. If not, the NRO will not start (see
     [Function ordering](#function-ordering)).
   - `tools/comprobar_clave_textura.py` ("check texture key") checks one piece of compiled code in the final program
     (see [the xxHash pitfall](#an-lto-and-pgo-pitfall-strict-aliasing-in-xxhash)).
   - The number of installed [hooks](glossary.md#hook) must be the same as in the build you started from.

## CMake on Windows

Four traps that cost a lot of time:

- **Never run CMake from the devkitPro MSYS window.** The `cmake` found there is a different build, which treats
  `D:/devkitPro` as a relative path and breaks the configuration. Always use the Windows one
  (`C:\Program Files\CMake\bin\cmake.exe`), from PowerShell, to configure and to build. If a build folder was already
  configured by the wrong CMake, delete its `CMakeFiles/<version>` folder and configure again.
- **Windows PowerShell 5.1 stops `& cmake ... *> log` at the first line written to stderr**, even a harmless warning.
  Run long builds with `Start-Process` and send the output to a file instead.
- **Keep one single version of Ninja.** An older Ninja (for example the one that comes with Visual Studio) throws away
  the build records of a newer one and rebuilds everything: for this game, about 686 files and half an hour instead of
  one minute.
- **Normally you only rebuild what changed** (`cmake --build <dir>`). Check first with `cmake --build <dir> -- -n`,
  which only lists what would be built: tens of items is normal; hundreds means something forced a full rebuild.

## Registers as C++ locals

**What it is:** a processor keeps the values it is working with in registers (its working slots). By default, the
translated code keeps every Xbox 360 register in a structure in memory (`ctx`), which is slow. ReXGlue can turn groups
of registers into normal C++ variables instead, which the compiler keeps in the Switch processor's own registers:

| Option | What it removed in this game |
|---|---|
| `cr_as_local` | the condition register (the results of comparisons) as variables |
| `xer_as_local` | 172,403 uses of `ctx.xer` |
| `ctr_as_local` | 32,637 uses of `ctx.ctr` |
| `non_volatile_as_local` | registers r14-r31 as variables; 23,857 calls to `__savegprlr_*` / `__restgprlr_*` |

**Why it matters:** on the Xbox 360, every function saves and restores registers r14-r31 by calling small helper
functions at its start and at its end. On the console, about 14 % of the game's main thread was spent inside just two
of them, `__restgprlr_29` and `__savegprlr_29`.

**Two things break when r14-r31 become variables:**

1. **Hooks that read their caller's registers from `ctx`.** A hook at the start of a game function that reads, for
   example, `ctx.r31` of the function that called it, now reads an old value. Mark those functions with
   `share_registers` in `app/overrides.toml`: then their callers write the variables back into `ctx` before calling
   them.
2. **Functions split in pieces.** The translator splits some game functions into pieces, and the main piece jumps into
   another one (with `b`, `bctr` or a jump table) with r14-r31, `cr`, `ctr` or `xer` already set. With variables, the
   other piece received them as zero, and the game crashed as soon as the first video started. The code generator was
   changed (`emit_call_sharing_registers`) so that:
   - a jump to a function marked `share_registers`, and every indirect jump (`bctr`), passes the variables it changed
     through `ctx`, and takes them back afterwards;
   - functions marked `share_registers` keep `cr`, `ctr` and `xer` in `ctx`.

   198 pieces are marked. `tools/lee_antes.py` finds registers that are read before they are written in the generated
   code; after marking, only two known false alarms (loops) are left. Run it after every code generation, because the
   list where the marks are kept can be regenerated.

Tried and rejected: `non_argument_as_local` (it breaks the way `__savevmx` uses register r12) and `skip_lr` (the game
reads `ctx.lr` in 21 places).

**Native C library functions.** ReXGlue can replace some functions of the game's C library with native versions
(`[rexcrt]` in the configuration, by the function's address in the game). `memmove` (which the game uses as its real
`memcpy`, from 950 places) and `strncpy` got faster that way; `memset` already had a native version.

## Direct calls

**The problem:** with GCC, `DEFINE_REX_FUNC` makes each game function `sub_XXXXXXXX` a **weak** alias of
`__imp__sub_XXXXXXXX`. A weak alias is a name that a [hook](glossary.md#hook) (`REX_HOOK_RAW`) can replace when the
program is linked. The generated code always calls `sub_XXXXXXXX`. But the compiler can never inline a call through a
weak alias (copy the called function into the caller), not in the same file and not with LTO, so LTO could barely
improve the game code.

**The fix:** `tools/llamadas_directas.py` rewrites `sub_X(ctx, base);` as `__imp__sub_X(ctx, base);` whenever `sub_X`
has no hook: 79,612 of the 83,077 calls in this game.

- A function counts as hooked if its address appears anywhere in the app's sources, the SDK or the configuration
  files. Hooks are sometimes built by joining pieces of names (`sub_##addr`), so searching for
  `REX_HOOK_RAW(sub_...)` is not enough. Check with `nm` that every `sub_` defined in the compiled files is on the list
  of hooked ones.
- The table used for indirect calls is not touched, so indirect calls still reach the hooks.
- `--deshacer` ("undo") reverts the rewrite.
- A new code generation silently loses the direct calls. Run the tool again after every code generation.

## LTO

[LTO](glossary.md#lto) is only used for the Switch build. Both the translated game and the app are compiled with
`-flto -fno-fat-lto-objects` and linked with `-flto=2 -flto-partition=balanced`. The link needs a lot of memory on the
build machine: watch the free memory, and keep the number of LTO jobs low.

## PGO with GCC 16 on Horizon

[PGO](glossary.md#pgo) takes two builds, from the same build folder, with the same generated code and the same
sources:

1. **The recording build** (`-fprofile-generate`, LTO off). For this game it took 66 minutes to build, and made an
   82.7 MB NRO with 16 MB of memory for the counters.
   - GCC writes PC paths for its counter files (`.gcda`) into the program. A `--wrap=fopen` in the app changes them to
     a folder on the SD card.
   - A background thread saves the counters and resets them every three minutes (`__gcov_dump` and `__gcov_reset`),
     so a crash or closing the game does not lose the session. Each save adds to the counters already in the files.
   - For a good profile, play as much as you can: menus, races, videos, every mode. (The profile in `pgo/` comes from
     a small test, not from a full playthrough.)
2. **The optimized build** (`-fprofile-use=<folder> -fprofile-partial-training`, LTO on), with the `.gcda` files
   copied from the SD card, keeping their names. `-fprofile-prefix-path` must be the build folder written the Windows
   way, with backslashes, or the names do not match.

Traps, each of which cost a build:

- **`-fno-profile-values` is required.** One part of GCC's recording code (the indirect call profiler) reads a
  per-thread value with `mrs tpidr_el0`, which is always 0 on Horizon because libnx does not use it
  (`-mtp=soft`). The recording build crashed before `main`. Check that the recording program does not contain
  `__gcov_indirect_call_profiler`. The rest of the profile (branches and functions) still works.
- **`-fprofile-correction` is needed in the optimized build.** Counters updated by several threads at once come out
  negative, and GCC rejects them as a broken profile (49 errors in the first attempt). This option fixes them.
- **Do not generate the code again or edit the sources between the two builds.** GCC matches the profile to each
  function with an identifier made from its name and its place in the sources:
  - a public function is found by a CRC32 fingerprint of its compiled name (GCC's own variant: polynomial
    0x04C11DB7, not reflected, including the final zero, masked with 0x7FFFFFFF);
  - its line checksum is `crc(crc(line, path with /), name)`, and a mismatch only gives a warning;
  - the identifier of a function that only exists inside one file also includes the path of that file, so the profile
    only works for a build that uses the same folder paths.
- **The warnings about mismatched profiles are hidden for the translated functions**, because they are declared
  through a macro in a system header. Use `-Wsystem-headers` to see them, and try a deliberately broken profile once,
  to prove that the matching really works.

**Using the profile for another edition.** Other [editions](glossary.md#edition) of the game are different programs,
with their functions at other addresses, so the function names are different. `tools/editions` translates the profile
of the main edition to another one: it renames the counters of each function using the address map between the two
programs. The other edition is then built from the same folder paths as the main one. See [editions.md](editions.md).

## Function ordering

`app/orden_funciones.ld` ("function order") lists the functions that run most, taken from profiles of the game. It is
passed to the linker, which puts them at the start of the program's code (`.text`), close together.

**The trap:** the linker places what this file lists **before** everything in its own script, including the start-up
code of libnx (`KEEP(*(.crt0))`). Four builds ended up with `memcpy` at address 0 and the start-up code moved, and none
of them started (Atmosphère reported an undefined instruction at `+0x14`). So the first line inside `.text` must be
`KEEP (*(.crt0))`, and every release checks that `_start` is at address 0. Even relinking "without changing a single
instruction" can break the start-up.

With `memcpy` at 0x140, `addr2line` gives wrong names for addresses near zero. Below 0x3000, translate addresses with
`objdump -d`, or with `nm -S` showing only code symbols.

## An LTO and PGO pitfall: strict aliasing in xxHash

**What happened:** three builds became less smooth, with no change in their logic. xxHash 0.8.3 (the hashing library)
chooses `XXH_FORCE_MEMORY_ACCESS 1` for GCC, a mode that reads memory in a way that breaks C++'s "strict aliasing"
rule. Once LTO and PGO copied `XXH3_64bits` into the function that builds the texture cache keys (until then it had
been a normal call), GCC was allowed to read the key before its last part had been written. The keys carried four
bytes of garbage, the cache did not find the textures, and textures were uploaded ten times more often (17,075 uploads
against 1,743).

**The fix:**

- `XXH_FORCE_MEMORY_ACCESS 0` before `XXH_INLINE_ALL` in every file that includes xxHash, with an `#error` if it comes
  too late;
- a check while the game runs, which recomputes a sample of keys;
- `tools/comprobar_clave_textura.py`, which checks that compiled code in the final program.

**The lesson:** with LTO and PGO, the compiler's decisions change as the program grows. So a hidden bug in the code
(undefined behavior) that did no harm in one build can appear in the next one, without anyone touching that code.

## Debugging crashes on the console

- `logs/rex/rex_crash.log` and the Atmosphère crash report give the full list of calls (the stack) at the crash.
- Translate the addresses to function names with
  `aarch64-none-elf-addr2line -f -C -i -e <unstripped ELF>`, subtracting 4 from return addresses. Keep the full
  (unstripped) ELF of every build that leaves your machine, or you will not be able to translate its crashes later.
- If the game freezes instead of crashing, the app's watchdog writes the stacks of every thread to `logs/`.

## Rebuilding the Vulkan driver

The NRO includes Mesa, taken from an SDK folder on your PC (see [mesa.md](mesa.md) and
[mesa/README.md](../mesa/README.md)). After you install a new build of the driver, **delete the linked program**
before you build the NRO. Ninja does not watch that library, so it says "no work to do" and packs the old driver.
Check that the new code is really in the program, for example by looking for a text that only the new code has.
