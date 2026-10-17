#!/bin/bash
# Runs the code generator on one game folder (or edition tree) and the steps the build depends on.
#
#   REXGLUE=sdk/out/linux-arm64/rexglue tools/codegen.sh halo3
#   REXGLUE=out/host/rexglue tools/codegen.sh nfsmw_pal
#   REXGLUE=out/host/rexglue tools/codegen.sh nfsmw_usa nfsmw_usa/enganchadas.txt nfsmw_usa/tabla.tsv
#
# The second and third arguments are for the trees made by tools/editions/crear_arbol.py: the list of hooked
# functions of that edition and the address table used to create it (see docs/editions.md).
# Set PYTHON if the Python launcher is not "python" (on Windows it is usually "py").
set -e
RAIZ=$(cd "$(dirname "$0")/.." && pwd)
APP=$RAIZ/${1:?usage: tools/codegen.sh <game folder> [hooked functions] [address table]}
REXGLUE=${REXGLUE:?set REXGLUE to the rexglue executable built from sdk/}
PY=${PYTHON:-python}
cd "$APP"
echo "== codegen ($APP)"
MANIFEST=$(ls *_manifest.toml | head -n 1)
"$REXGLUE" codegen "$MANIFEST" > codegen.log 2>&1 || { echo "codegen failed, see $APP/codegen.log"; exit 1; }
cd "$RAIZ"
echo "== direct calls"
if [ -n "${2:-}" ]; then
  "$PY" tools/llamadas_directas.py --gen "$APP/generated/default" --enganchadas "$RAIZ/$2"
else
  "$PY" tools/llamadas_directas.py --gen "$APP/generated/default"
fi
# Literal copies of the functions that have native replacements with guards (only games that use them).
if ! grep -rqs --include="*.cpp" copias_literales "$APP/src"; then echo "done"; exit 0; fi
echo "== literal copies"
if [ -n "${3:-}" ]; then
  "$PY" tools/copia_literal.py "$APP/generated/default" "$APP/src/copias_literales" --tabla "$RAIZ/$3"
else
  "$PY" tools/copia_literal.py "$APP/generated/default" "$APP/src/copias_literales"
fi
echo "done"
