#!/bin/bash
# Builds the deko3d shader library from the containers the game dumped on the console (shaders_dump/):
#   1. ds_hlsl         containers -> HLSL (XenosRecomp's translator, --saltos-lineales)
#   2. dxc, spirv-val  HLSL -> SPIR-V for deko3d, validated (Vulkan SDK)
#   3. ds_empaquetar   containers + SPIR-V -> ds2_shaders.dslib
#   4. ds_dksh         (optional, DKSH=1) the library -> shadercache_dksh.bin, so the console never runs uam
#
# Usage: shaders/build_library.sh <shaders_dump> <output folder>
#   VULKAN_BIN   folder with dxc and spirv-val (default: ~/VulkanSDK/*/macOS/bin)
#   DKSH=1       also build dksh/ (needs the SDK's thirdparty submodules) and the DKSH cache
#   JOBS         parallel DXC processes (default: every core)
# Writes <output>/ds2_shaders.dslib (and shadercache_dksh.bin), the HLSL and SPIR-V of each shader, and the
# errors of the ones that did not compile in <output>/err/. Copy the .dslib and the cache next to the NRO.
set -euo pipefail

if [ $# -ne 2 ]; then
  sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
  exit 1
fi
entrada=$(cd "$1" && pwd)
mkdir -p "$2"
salida=$(cd "$2" && pwd)
raiz=$(cd "$(dirname "$0")/.." && pwd)
shaders="$raiz/shaders"
terceros="$raiz/sdk/thirdparty"
VULKAN_BIN=${VULKAN_BIN:-$(ls -d "$HOME"/VulkanSDK/*/macOS/bin 2>/dev/null | tail -1)}
DXC="$VULKAN_BIN/dxc"
VAL="$VULKAN_BIN/spirv-val"
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
for herramienta in "$DXC" "$VAL"; do
  [ -x "$herramienta" ] || { echo "missing $herramienta (set VULKAN_BIN)"; exit 1; }
done

bin="$salida/bin"
mkdir -p "$bin"
echo "== tools"
clang++ -std=c++2b -O1 -I"$shaders" -I"$shaders/XenosRecomp" -I"$terceros/fmt/include" -I"$terceros/xxHash" \
  -include "$shaders/XenosRecomp/pch_min.h" -DFMT_HEADER_ONLY -DXXH_INLINE_ALL \
  -Wno-switch -Wno-unused-variable -Wno-null-arithmetic -fms-extensions -fexperimental-library \
  "$shaders/ds_hlsl.cpp" "$shaders/XenosRecomp/shader_recompiler.cpp" -o "$bin/ds_hlsl"
clang++ -std=c++20 -O1 -I"$shaders" -I"$terceros/xxHash" \
  "$shaders/ds_empaquetar.cpp" "$shaders/ds_shader_library.cpp" -o "$bin/ds_empaquetar"

echo "== HLSL"
rm -rf "$salida/hlsl" "$salida/spirv" "$salida/err"
"$bin/ds_hlsl" "$entrada" "$salida/hlsl" "$shaders/XenosRecomp/shader_common.h" --saltos-lineales > "$salida/ds_hlsl.log"
tail -1 "$salida/ds_hlsl.log"

echo "== SPIR-V"
mkdir -p "$salida/spirv" "$salida/err"
compilar() {
  local f=$1 base tipo
  base=$(basename "$f" .hlsl)
  if [[ "$base" == ps_* ]]; then tipo=ps_6_6; else tipo=vs_6_6; fi
  if ! "$DXC" -spirv -T "$tipo" -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout \
       -Werror=parameter-usage -Fo "$salida/spirv/$base.spv" "$f" > "$salida/err/$base.dxc" 2>&1; then
    echo "$base DXC"; rm -f "$salida/spirv/$base.spv"; return
  fi
  if ! "$VAL" --target-env vulkan1.2 --scalar-block-layout "$salida/spirv/$base.spv" > "$salida/err/$base.val" 2>&1; then
    echo "$base VAL"; rm -f "$salida/spirv/$base.spv"; return
  fi
  rm -f "$salida/err/$base.dxc" "$salida/err/$base.val"
  echo "$base OK"
}
export -f compilar
export DXC VAL salida
ls "$salida"/hlsl/*.hlsl | xargs -P "$JOBS" -I{} bash -c 'compilar "$1"' _ {} > "$salida/spirv.log"
echo "$(grep -c ' OK$' "$salida/spirv.log") OK, $(grep -vc ' OK$' "$salida/spirv.log") failed (see err/)"

echo "== library"
# Only the containers that compiled go in.
contenedores="$salida/contenedores"
rm -rf "$contenedores" && mkdir -p "$contenedores"
for spv in "$salida"/spirv/*.spv; do
  ln -s "$entrada/$(basename "$spv" .spv).bin" "$contenedores/"
done
rm -f "$salida/ds2_shaders.dslib"
"$bin/ds_empaquetar" "$contenedores" "$salida/spirv" "$salida/ds2_shaders.dslib"

if [ "${DKSH:-0}" = 1 ]; then
  echo "== DKSH"
  cmake -S "$shaders/dksh" -B "$salida/build_dksh" -G Ninja -DCMAKE_BUILD_TYPE=Release > "$salida/dksh_cmake.log"
  cmake --build "$salida/build_dksh" > "$salida/dksh_build.log"
  "$salida/build_dksh/ds_dksh" "$salida/ds2_shaders.dslib" "$salida/shadercache_dksh.bin" \
    --contenedores "$contenedores" --informe "$salida/dksh.tsv" > "$salida/dksh.log" 2>&1 ||
    echo "some variants failed: $salida/dksh.tsv"
  tail -3 "$salida/dksh.log"
fi
