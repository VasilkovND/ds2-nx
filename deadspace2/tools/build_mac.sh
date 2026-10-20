#!/bin/bash
# Build ds2-nx.nro on a Mac (Apple Silicon or Intel) with devkitPro's own toolchain.
#
# Once:
#   1. devkitPro: install the pkg from https://github.com/devkitPro/installer/releases (or
#      https://devkitpro.org/wiki/Getting_Started), then:  sudo dkp-pacman -S switch-dev deko3d
#   2. Homebrew tools:  brew install cmake ninja python git
#
# Then, from the repository root:   deadspace2/tools/build_mac.sh [path/to/Dead-Space-2-Recomp]
# The NRO ends up in deadspace2/out/sw/deadspace2.nro (copy it as ds2-nx.nro).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DS2_RECOMP="${1:-$ROOT/../Dead-Space-2-Recomp}"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export PATH="$DEVKITPRO/tools/bin:$PATH"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"

[ -x "$DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++" ] || { echo "devkitA64 not found in $DEVKITPRO (step 1)"; exit 1; }
[ -f "$DEVKITPRO/libnx/lib/libdeko3d.a" ] || { echo "deko3d not installed: sudo dkp-pacman -S deko3d"; exit 1; }

if [ ! -d "$DS2_RECOMP/src_tu2" ]; then
  echo "== cloning Dead-Space-2-Recomp into $DS2_RECOMP"
  git clone --depth 1 https://github.com/bifftannen1/Dead-Space-2-Recomp "$DS2_RECOMP"
fi

cd "$ROOT"
if [ ! -f sdk/CMakeLists.txt ]; then
  echo "== sdk submodule"
  git submodule update --init sdk
fi
if [ ! -f sdk/thirdparty/uam/uam_api.h ]; then
  echo "== Switch third-party sources"
  (cd sdk && python3 tools/switch_setup.py)
fi

echo "== configure"
cmake -S deadspace2 -B deadspace2/out/sw -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/switch/cmake/switch-devkitA64.cmake" \
  -DREXSDK_DIR="$ROOT/sdk" -DDS2_RECOMP_DIR="$DS2_RECOMP" -DREXGLUE_SWITCH_DEKO3D=ON

echo "== build ($JOBS jobs)"
cmake --build deadspace2/out/sw -j "$JOBS"
ls -la deadspace2/out/sw/deadspace2.nro
