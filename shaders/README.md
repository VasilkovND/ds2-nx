# Shaders

This folder has the shader translator and the tools around it. Look here to rebuild the shader library, or to find
the code of one step of the translation. How the translation works, and why, is in
[docs/shaders.md](../docs/shaders.md). The command to rebuild the library on a PC is in step 7 of
[docs/building.md](../docs/building.md).

The game's [shaders](../docs/glossary.md#shader) (the small programs the GPU runs) are Xbox 360
([Xenos](../docs/glossary.md#xenos)) [microcode](../docs/glossary.md#microcode). They are stored in containers (the
files that hold them) inside the disc files and the executable. They are translated ahead of time into a
[SPIR-V](../docs/glossary.md#spir-v) library, `nfsmw_shaders.nfsp` (the
[shader library](../docs/glossary.md#shader-library)), which the renderer loads at startup. The steps are: find the
containers, translate the microcode to [HLSL](../docs/glossary.md#hlsl), compile the HLSL to SPIR-V with
[DXC](../docs/glossary.md#dxc), and pack the result into the library.

| File | What it does |
|---|---|
| `XenosRecomp/` | [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) (by hedge-dev), with this port's changes under `NFSMW_RECOMP`: it translates microcode to HLSL. |
| `shader_common.h` | The HLSL helpers every translated shader includes (texture fetches, specialization constants). |
| `nfsmw_contenedor.h` ("container") | Reads the game's shader containers (the 2005 layout of this game). |
| `nfsmw_buscar_contenedores.cpp` ("find containers") | Finds shader containers in the disc files. |
| `nfsmw_hlsl.cpp` | Translates every container of a folder to HLSL. |
| `nfsmw_empaquetar.cpp` ("pack") | Packs the SPIR-V into the library, keyed by a fingerprint of each container. |
| `nfsmw_lzx.cpp` | LZX decompression of the executable image (with libmspack), done the same way as the runtime does it. |
| `nfsmw_probar_contenedor.cpp`, `nfsmw_probar_biblioteca.cpp` ("test container", "test library") | Regression checks of the container reader and of a built library. |
| `nfsmw_regenerar_biblioteca_pcf.sh` ("regenerate library") | The whole pipeline on the PC: translate, rewrite the shadow and blur paths, compile with DXC, validate, pack. |
| `pch_min.h` | Precompiled header of the translator build. |
| `wasm/` | The WebAssembly builds (programs compiled to run in a browser) used by the installer page: `build_wasm_tools.bat` and `link_dxc_wasm.bat` build them, and `dxc_web.cpp` is the entry point of DXC in the browser. |

The installer page runs the same steps in the browser (`lib/shaders.js` in the
[installer's repository](https://github.com/StevensND/nfsmw-nx-installer)). It checks that the result has the
SHA-256 (a fingerprint of the whole file) of the library each NRO was tested with.
