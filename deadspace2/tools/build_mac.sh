#!/bin/bash
# Build ds2-nx.nro with devkitPro's toolchain: on a Mac (Apple Silicon or Intel), tested; on Linux, the same steps
# with your distribution's packages instead of Homebrew (not tested).
#
# Once:
#   1. devkitPro: install the pkg from https://github.com/devkitPro/pacman/releases (or
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
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN)}"

[ -x "$DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++" ] || { echo "devkitA64 not found in $DEVKITPRO (step 1)"; exit 1; }
[ -f "$DEVKITPRO/libnx/lib/libdeko3d.a" ] || { echo "deko3d not installed: sudo dkp-pacman -S deko3d"; exit 1; }

# The recompiled tree this release was built and tested against (Dead-Space-2-Recomp, 2026-10-17). A later commit may
# rename or change what the Switch glue relies on: DS2_RECOMP_COMMIT= (empty) takes whatever is checked out.
DS2_RECOMP_COMMIT="${DS2_RECOMP_COMMIT-97b9dac62ad10175874774ea0dcc8a9fd632893f}"
if [ ! -d "$DS2_RECOMP/src_tu2" ]; then
  echo "== Dead-Space-2-Recomp into $DS2_RECOMP"
  mkdir -p "$DS2_RECOMP"
  git -C "$DS2_RECOMP" init -q
  git -C "$DS2_RECOMP" remote add origin https://github.com/bifftannen1/Dead-Space-2-Recomp
  git -C "$DS2_RECOMP" fetch -q --depth 1 origin "${DS2_RECOMP_COMMIT:-HEAD}"
  git -C "$DS2_RECOMP" checkout -q FETCH_HEAD
fi
if [ -n "$DS2_RECOMP_COMMIT" ] && [ "$(git -C "$DS2_RECOMP" rev-parse HEAD 2>/dev/null)" != "$DS2_RECOMP_COMMIT" ]; then
  echo "warning: $DS2_RECOMP is not at the tested commit $DS2_RECOMP_COMMIT"
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

# Fixes to the SDK that this game needs and that are not upstream yet (deadspace2/sdk-patches/).
for p in "$ROOT"/deadspace2/sdk-patches/*.patch; do
  [ -f "$p" ] || continue
  if git -C sdk apply --reverse --check "$p" 2>/dev/null; then
    echo "== sdk patch already applied: $(basename "$p")"
  else
    echo "== sdk patch: $(basename "$p")"
    git -C sdk apply "$p"
  fi
done

# Configure only the first time (or with RECONFIGURE=1): afterwards Ninja re-runs CMake by itself when a CMake file
# changes, and an incremental build of a change to deadspace2/src/ takes seconds instead of rebuilding the game.
if [ ! -f deadspace2/out/sw/build.ninja ] || [ "${RECONFIGURE:-0}" = 1 ]; then
  echo "== configure"
  cmake -S deadspace2 -B deadspace2/out/sw -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/switch/cmake/switch-devkitA64.cmake" \
    -DREXSDK_DIR="$ROOT/sdk" -DDS2_RECOMP_DIR="$DS2_RECOMP" -DREXGLUE_SWITCH_DEKO3D=ON
fi

echo "== build ($JOBS jobs)"
cmake --build deadspace2/out/sw -j "$JOBS"
ls -la deadspace2/out/sw/deadspace2.nro
