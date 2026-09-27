# Building

Players do not need any of this: the [installer page](https://stevensnd.github.io/nfsmw-nx-installer/) builds the
package from your own copy of the game, using the released NROs. This page is for building the NRO and the shader
library from source.

Everything here was done on Windows 10/11 x64. The scripts are PowerShell, bash (Git Bash or MSYS2) and Python;
nothing is tied to Windows except the Mesa build notes, but other hosts are untested.

## Requirements

- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with devkitA64 and libnx (default location `C:/devkitPro`,
  or set `DEVKITPRO`).
- CMake 3.25 or newer, Ninja, Git, Python 3.10 or newer.
- A host C++ compiler for the code generator (Clang 20 or newer was used).
- Your own copy of Need for Speed: Most Wanted for Xbox 360 (ISO or extracted files). Nothing from the game is in this
  repository, and nothing derived from it may be committed to it.
- About 16 GB of RAM. The recompiled code is large (about 144 MB of C++), and the link time optimization step alone
  takes around ten minutes on a desktop CPU.

Optional:

- MSYS2 and Rust, to build the Vulkan driver yourself (see [mesa.md](mesa.md)).
- The Vulkan SDK (DXC and spirv-val) and MinGW g++, to build the shader library natively
  (`shaders/nfsmw_regenerar_biblioteca_pcf.sh`).
- Emscripten, to rebuild the WebAssembly tools of the installer page (`shaders/wasm/`).

## 1. Third-party sources

`sdk/thirdparty` only holds the files this port changed. Fetch the rest from the ReXGlue SDK release this port is
based on (v0.10.0) and its submodules:

```sh
python tools/fetch_thirdparty.py
```

It never overwrites a file that is already there.

## 2. The game files

Extract the disc into `assets/game_root` (the code generator reads `assets/game_root/default.xex`):

```sh
python tools/fase1_extraer.py path/to/NFSMW.iso -o assets/game_root
```

`assets/` is ignored by git. Each edition of the game has a different `default.xex` and needs its own build; the
default tree `app/` is the PAL Spanish one. See [editions.md](editions.md) for the others.

## 3. The Vulkan driver

Build [danfromtico/mesa-switch](https://github.com/danfromtico/mesa-switch) at commit `1a8c1a66d6f` with
`mesa/mesa-switch-nfsmw.patch` applied, as described in [mesa/README.md](../mesa/README.md). The result is an SDK
folder; the app needs `<sdk>/opt/devkitpro/portlibs/switch`, which contains `lib/libvulkan.a`.

## 4. The code generator

The code generator (`rexglue`) runs on the PC. Build it from `sdk/` with a host compiler, for example:

```sh
cmake -S sdk -B out/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build out/host --target rexglue
```

`sdk/src/codegen` includes the change that lets the generated code keep guest registers in local variables shared
between the chunks of a function (`share_registers`, see [toolchain.md](toolchain.md)).

## 5. Generate the code

```sh
REXGLUE=out/host/rexglue tools/codegen.sh app
```

This runs the code generator on `app/nfsmw_manifest.toml` (output in `app/generated/default`, ignored by git) and
then the two steps the build depends on:

- `tools/llamadas_directas.py` turns calls between recompiled functions into direct calls (needed for LTO to inline
  across files).
- `tools/copia_literal.py` writes `app/src/copias_literales/`: literal copies of five game functions used by the
  self-checking guards of their native replacements (see [native-renderer.md](native-renderer.md)).

## 6. Build the NRO

From PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -MesaSdk C:\path\to\mesa-sdk\opt\devkitpro\portlibs\switch
```

or by hand:

```sh
cmake -S app -B app/out/sw8 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=tools/switch/cmake/switch-devkitA64.cmake \
  -DREXSDK_DIR=$PWD/sdk \
  -DREXGLUE_SWITCH_NVK_SDK=/path/to/mesa-sdk/opt/devkitpro/portlibs/switch \
  -DNFSMW_PGO=usar -DNFSMW_BUILD_LAUNCHER=OFF
cmake --build app/out/sw8 -j 4
```

The result is `app/out/sw8/nfsmw.nro`. Options in `app/CMakeLists.txt`:

| Option | Default | Meaning |
|---|---|---|
| `NFSMW_LTO` | ON | Link time optimization of the recompiled code and the app (`-flto=2`, balanced partitions; more jobs do not fit in 16 GB) |
| `NFSMW_PGO` | empty | `usar` builds with the profile in `pgo/pal_es`; `generar` builds an instrumented NRO that records one |
| `NFSMW_ORDEN_FUNCIONES` | ON | Places hot functions first in `.text` (`app/orden_funciones.ld`) |

The homebrew menu icon is not included; put a 256x256 JPEG at `tools/switch/icono/nfsmw_icono.jpg` or the NRO gets
the default libnx icon.

### Profile guided optimization

`pgo/<edition>/` holds the profile the released NROs were built with, recorded on the console while playing and
translated to each edition (`tools/editions/pgo/traducir_perfil.py`). GCC identifies a function of the profile
by a hash of its name, except for functions with internal linkage (static, anonymous namespaces), whose hash also
includes the path of the source file. Those only match when the sources are compiled from the same path the profile
was recorded with; elsewhere GCC builds them without profile (a small loss, no error).

To record a new profile: configure with `-DNFSMW_PGO=generar`, play (the NRO writes the counters to
`sdmc:/switch/nfsmw/pgo/` every three minutes), copy the `.gcda` files into `pgo/pal_es/` and build again with
`-DNFSMW_PGO=usar`. Details and pitfalls in [toolchain.md](toolchain.md).

## 7. The shader library

The game's shaders are Xbox 360 microcode inside the disc files. `nfsmw_shaders.nfsp` holds them translated to SPIR-V
(see [shaders.md](shaders.md)). The installer page builds it in the browser from the disc. To build it natively:

```sh
MESA=/path/to/mesa-switch shaders/nfsmw_regenerar_biblioteca_pcf.sh out/library /path/to/extracted/containers
```

The containers are extracted from the disc files by `shaders/nfsmw_buscar_contenedores.cpp`.

## 8. Put it on the SD card

The NRO expects this layout (the installer page produces the same one):

```
sdmc:/switch/nfsmw-nx/
    nfsmw-nx.nro
    nfsmw.toml
    nfsmw_shaders.nfsp
    game_root/          the files of the disc
```

`nfsmw.toml` is the configuration read at startup; the one the installer page ships is in its repository. Start the
NRO from the homebrew menu in title takeover mode (hold R while launching a game), or through a forwarder: applet
mode does not have enough memory.

## Other editions

Every `default.xex` is a different program. `tools/editions/crear_arbol.py` builds an `app_<edition>` tree with all
the addresses of our code translated, and `tools/editions/build_edition.ps1` builds it with the same paths as the
default tree so the profile matches. See [editions.md](editions.md).
