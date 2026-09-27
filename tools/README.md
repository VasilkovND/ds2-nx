# Tools

| File | What it does |
|---|---|
| `fetch_thirdparty.py` | Fetches the third-party sources of the SDK into `sdk/thirdparty` |
| `fase1_extraer.py` | Extracts the Xbox 360 ISO into `assets/game_root` and prints the XEX information |
| `codegen.sh` | Runs the code generator on an edition tree, then `llamadas_directas.py` and `copia_literal.py` |
| `llamadas_directas.py` | Turns calls between recompiled functions into direct calls, except for hooked ones (needed by LTO) |
| `copia_literal.py` | Writes the literal copies of five game functions used by the guards of their native versions |
| `huecos.py`, `huecos_excluir.txt` | Finds code the analysis left without a function and writes it to `app/huecos.toml` |
| `lee_antes.py` | Lists, per recompiled function, the non-volatile registers it reads before writing them |
| `calientes.py` | Resolves the samples of the console profiler to functions, hottest first (input for `app/orden_funciones.ld`) |
| `comprobar_clave_textura.py` | Checks in the built ELF that the texture key is written before it is hashed (an LTO/PGO aliasing pitfall) |
| `build.ps1` | Configures and builds the NRO of the default tree |
| `switch/cmake/` | The CMake toolchain for devkitA64 and libnx |
| `editions/` | Support for the other editions: address matching, tree creation, checks, profile translation, per-edition build ([docs/editions.md](../docs/editions.md)) |
