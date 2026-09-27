#!/bin/bash
# Incremental build of mesa-switch from the MSYS2 MINGW64 shell, after a first full build with build-unified.sh.
# Runs ninja on builddir-unified and installs only what changed.
#
#   MESA=/c/src/mesa-switch DEVKITPRO=/c/devkitPro ./build_mesa_msys2.sh
#
# builddir-unified was configured with the GNU Rust toolchain (its proc macros are GNU DLLs). If rustup defaults to
# MSVC, the build fails; RUSTUP_TOOLCHAIN pins the GNU one.
set -u
MESA=${MESA:?set MESA to the mesa-switch source tree}
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export RUSTUP_TOOLCHAIN=${RUSTUP_TOOLCHAIN:-stable-x86_64-pc-windows-gnu}
INSTALL=${INSTALL:-$MESA/mesa-unified-install}
cd "$MESA" || exit 1
echo "== ninja"
ninja -C builddir-unified -j2 || exit $?
echo "== meson install"
meson install -C builddir-unified --destdir "$INSTALL" --only-changed 2>&1 | tail -3
echo "== libraries"
find "$INSTALL" \( -name "libnvk.a" -o -name "libvulkan.a" -o -name "libnak*.a" \) -exec ls -la {} \;
