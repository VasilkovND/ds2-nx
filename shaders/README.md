# Shaders

The game's shaders are Xbox 360 (Xenos) microcode stored in containers inside the disc files and the executable. They
are translated ahead of time into a SPIR-V library, `nfsmw_shaders.nfsp`, that the renderer loads at startup. See
[docs/shaders.md](../docs/shaders.md).

| File | What it does |
|---|---|
| `XenosRecomp/` | [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) (hedge-dev), with this port's changes under `NFSMW_RECOMP`: microcode to HLSL |
| `shader_common.h` | The HLSL helpers every translated shader includes (texture fetches, specialization constants) |
| `nfsmw_contenedor.h` | Reads the game's shader containers (the 2005 layout of this game) |
| `nfsmw_buscar_contenedores.cpp` | Finds shader containers in the disc files |
| `nfsmw_hlsl.cpp` | Translates every container of a folder to HLSL |
| `nfsmw_empaquetar.cpp` | Packs the SPIR-V into the library, keyed by a fingerprint of each container |
| `nfsmw_lzx.cpp` | LZX decompression of the executable image (libmspack), as the runtime does it |
| `nfsmw_probar_contenedor.cpp`, `nfsmw_probar_biblioteca.cpp` | Regression checks of the container reader and of a built library |
| `nfsmw_regenerar_biblioteca_pcf.sh` | The whole native pipeline: translate, rewrite the shadow and blur paths, compile with DXC, validate, pack |
| `pch_min.h` | Precompiled header of the translator build |
| `wasm/` | The WebAssembly builds used by the installer page, and `dxc_web.cpp`, the entry point of DXC in the browser |

The installer page runs the same steps in the browser (`lib/shaders.js` in its repository) and checks that the result
has the SHA-256 of the library each NRO was tested with.
