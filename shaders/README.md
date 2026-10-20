# Shaders

The shader toolchain. Dead Space 2's shaders are Xbox 360 (Xenos) microcode in containers of the 2008 format. The
console dumps every container the game creates (`shaders_dump/{vs,ps}_<xxh3>.bin`, by
`deadspace2/src/ds2_shader_dump.cpp`); these tools translate them on the PC, ahead of time, into a SPIR-V library,
`ds2_shaders.dslib`, and optionally a DKSH cache, `shadercache_dksh.bin`, which the deko3d renderer loads at startup.

```sh
shaders/build_library.sh <shaders_dump> <out>          # -> <out>/ds2_shaders.dslib
DKSH=1 shaders/build_library.sh <shaders_dump> <out>   # also <out>/shadercache_dksh.bin
```

`VULKAN_BIN` points at the folder with `dxc` and `spirv-val` (default: `~/VulkanSDK/*/macOS/bin`), `JOBS` sets the
number of parallel DXC processes. Besides the library, `<out>` gets the HLSL and SPIR-V of each shader and, in
`err/`, the errors of the ones that did not compile. Copy the `.dslib` and the cache next to the NRO.

## Steps

1. **`ds_hlsl`**: containers -> HLSL, with XenosRecomp's translator. `build_library.sh` always passes
   `--saltos-lineales`: forward-only jumps become `if (pc <= N)` blocks instead of `while (true) switch (pc)`, which
   uam cannot compile in reasonable time (`ShaderRecompiler::saltosLineales`).
2. **DXC + spirv-val**: HLSL -> SPIR-V for deko3d, validated:
   ```
   dxc -spirv -T ps_6_6|vs_6_6 -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout -Werror=parameter-usage
   spirv-val --target-env vulkan1.2 --scalar-block-layout
   ```
3. **`ds_empaquetar`** ("pack"): the containers that compiled and their SPIR-V -> `ds2_shaders.dslib`, then checks
   that every container is found in the result.
4. **`ds_dksh`** (only with `DKSH=1`): the library -> DKSH cache, the way the console would compile it
   (SPIR-V -> GLSL with SPIRV-Cross -> DKSH with uam).

## Files

| File | What it does |
|---|---|
| `XenosRecomp/` | [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) (by hedge-dev, MIT, see [LICENSE.md](LICENSE.md)), trimmed to the deko3d output only: translates microcode to HLSL. `shader_common.h` holds the HLSL helpers every translated shader includes (texture fetches, the shared constants `DS_COMPARTIDA_*`, specialization constants). |
| `ds_hlsl.cpp` | Translates every container of a folder to HLSL (`ds_hlsl <input> <output> <shader_common.h> [--saltos-lineales]`). |
| `ds_shader_library.h`, `.cpp` | The library format (namespace `ds`): magic `DS2SPV`, versioned, little-endian, with integrity checks; each original container with its SPIR-V, looked up by the container. Accepts only format 2008 containers (`0x102A1100` PS, `0x102A1101` VS). Shared with the console code, `deadspace2/src/ds2_shaders.cpp`. |
| `ds_empaquetar.cpp` | `ds_empaquetar <containers> <validated spirv> <new output>`: packs the library. |
| `dksh/ds_dksh.cpp`, `dksh/CMakeLists.txt` | The DKSH cache builder. Uses the SDK's GLSL and cache code (`sdk/src/ui/deko3d`) and `sdk/thirdparty` (SPIRV-Cross, uam, xxHash). Each variant (shader, specialization value) compiles in its own child process with a time limit, on every core. Options: `--hilos N` (processes), `--limite SECONDS` (per variant, 120 by default), `--contenedores DIR` (names in the report), `--glsl DIR` (writes the GLSL), `--informe FILE.tsv` (per-variant result, uam output and times). Exit code 0 only if every variant compiled. |
| `build_library.sh` | The whole chain above. |
