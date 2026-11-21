#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Pedro Portela
# Build xbsc_probe.exe and walk the candidate-signature matrix.
#
# One probe per process: a wrong guess that faults only loses its own probe.
#
#   tools/xbsc_probe.sh [blob.rdna2]
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DLL="${SCARAB_VENDOR:-$ROOT/vendor}/x64_dx12/xbsc_xs.dll"
BLOB="${1:-$HOME/Documents/scarab-artifacts/rdna2/rdna2/VS_BlitGenAmbCube_0cfdb358_Wrapped.a9dd3024.rdna2}"
OUT=/tmp/xbsc_probe
CC="${SCARAB_CC:-$HOME/opt/llvm-mingw/bin/x86_64-w64-mingw32-clang}"

mkdir -p "$OUT"
"$CC" -O1 -municode -o "$OUT/xbsc_probe.exe" "$ROOT/tools/xbsc_probe.c" || exit 1
[ -f "$DLL" ]  || { echo "no DLL at $DLL"; exit 1; }
[ -f "$BLOB" ] || { echo "no blob at $BLOB (pass one as \$1)"; exit 1; }
echo "dll=$DLL"; echo "blob=$BLOB ($(stat -c%s "$BLOB") bytes)"

WINE="${SCARAB_WINE:-$HOME/opt/wine/bin/wine}"
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-scarab}"
export WINEDEBUG=-all

for p in 0 1 2 3 4 5; do
  echo "---------- probe $p ----------"
  timeout 120 "$WINE" "$OUT/xbsc_probe.exe" "$(winepath -w "$DLL" 2>/dev/null || echo "$DLL")" \
      "$(winepath -w "$BLOB" 2>/dev/null || echo "$BLOB")" "$p" 2>&1 | grep -v '^wine:'
  echo "exit=$?"
done
