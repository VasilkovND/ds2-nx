# Supporting other editions and languages

Every different `default.xex` is a different program and needs its own build. The installer page
picks the build by the SHA-256 of the player's executable and, for editions that were verified with
a specific disc, by the size of one data file (`NFS/ZZDATA0.BIN`).

## Editions

| Edition | Relation to the reference (PAL Spanish) |
|---|---|
| PAL Spanish | reference edition; every address in the sources refers to it |
| PAL German, PAL Italian | the same compilation of the game ("NfsMWEurope...Release"); only five instructions that load the language constant and some strings in `.rdata` differ. Same addresses, same profile, only five generated files differ |
| PAL English | a different compilation, close to the US one; also used by a community Russian translation, which only replaces `NFS/ZZDATA0.BIN` |
| USA | a different compilation: `.text`, `.rdata` and the embedded sections move |
| Japan | a different compilation where **`.data` moves too**, piecewise (mostly by +0x5A0/+0x5A4) |

Other PAL languages are probably the same compilation as the Spanish, German and Italian ones with a
different language constant.

Some shaders that the renderer recognises by hash have different hashes in some editions (for
example the glow and sky shaders of the US and Japanese builds), and the final composition shader
has a different container name in each edition. The renderer and the installer find it by content,
not by name.

## Porting the hooks to another edition

All hooks, overrides and function boundaries are written against the addresses of the reference
executable. The tools in [`tools/editions/`](../tools/editions) translate them:

1. **Match the two executables.** `emparejar.py` aligns the code of both executables with normalised
   PowerPC instruction 12-grams as anchors and a longest increasing subsequence to keep only
   consistent matches. `.rdata` is matched by content with windows of 32 to 1,024 bytes. The result is
   a map from every address of the reference to the other edition (`direcciones_todas.txt`).
2. **Verify the hooked functions.** `verificar_ganchos.py` compares every hooked function in full
   between the two executables.
3. **Translate `.data` references.** `.data` normally stays at the same address, but in the Japanese
   edition it moves in pieces. `datos_por_referencias.py` follows the `lis` + low-part instruction
   pairs of the code to find where each referenced datum went, and `verificar_parejas.py` checks
   every pair strictly (856 pairs for the Japanese edition, all consistent).
4. **Create the edition tree.** `crear_arbol.py <edition> <map> <xex> --parejas <json>` creates a copy
   of the application with every address translated, seeds the function partition of the code
   generator with the reference one (exact pairs first), and copies the list of weak callees from the
   reference's generated code. Split names built by token pasting (for example
   `UNIR_(__imp__sub_824F, D7C0)`) are translated too.
5. **Generate the code** for the edition. If only the partition changed, delete `codegen.stamp` to
   force a new generation. Then run the post-passes (`llamadas_directas.py` with the list of hooked
   addresses of that edition). `comparar_reparto.py` compares the partition with the reference and
   `tools/huecos.py` shows the gaps that need manual pairs; the Japanese edition needed six.
6. **Translate the PGO profile.** `pgo/traducir_perfil.py` renames each function's counters through
   the address map, so the other edition gets the same optimisation without playing it again (see
   [toolchain.md](toolchain.md#pgo-with-gcc-16-on-horizon)).
7. **Build from the reference paths.** The identifier of a local function in a GCC profile includes
   the path of its source file, so the edition tree is built from the same paths as the reference: the
   build script swaps the edition tree into place for the duration of the build and swaps it back.

Before shipping an edition build, the same checks as for the reference apply: `_start` at address 0,
the texture key check, and the number of installed hooks equal to the reference.

## Installer manifest

The page reads `release/manifest.json`. Each entry gives the edition name, the SHA-256 of its
executable, the NRO to use and its SHA-256, the expected SHA-256 and size of the shader library built
from that disc, and the SHA-256 of the final composition shader container, which the page uses to find
that shader by content. Entries can also name a disc file and its size, so that a known executable on
an untested disc (a reprint or a translation) is reported instead of producing a build that nobody
has verified.
