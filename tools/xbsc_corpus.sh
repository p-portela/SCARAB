#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Pedro Portela
# Run the vendor disassembler over the extracted RDNA2 corpus, in batches.
#
#   tools/xbsc_corpus.sh [limit] [chunk]
#
# One process per chunk (not per blob): the DLL is loaded once and every blob in
# the chunk goes through it. A chunk that faults costs only its own remainder --
# the last emitted "@@ <file>" record names the offender.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ART="${SCARAB_ARTIFACTS:-$HOME/Documents/scarab-artifacts}"
CORPUS="$ART/rdna2/rdna2"
DLL="${SCARAB_VENDOR:-$ROOT/vendor}/x64_dx12/xbsc_xs.dll"
CC="${SCARAB_CC:-$HOME/opt/llvm-mingw/bin/x86_64-w64-mingw32-clang}"
WINE="${SCARAB_WINE:-$HOME/opt/wine/bin/wine}"
LIMIT="${1:-0}"
CHUNK="${2:-2000}"
OUT="${SCARAB_XBSC_OUT:-/tmp/xbsc-corpus}"
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-scarab}" WINEDEBUG=-all

mkdir -p "$OUT"
"$CC" -O1 -municode -o "$OUT/xbsc_probe.exe" "$ROOT/tools/xbsc_probe.c" || exit 1
[ -f "$DLL" ] || { echo "no DLL at $DLL"; exit 1; }
[ -d "$CORPUS" ] || { echo "no corpus at $CORPUS"; exit 1; }

# manifest: every stream as a Windows path (Wine maps Z: to /)
find "$CORPUS" -name '*.rdna2' | sort > "$OUT/all.txt"
TOTAL=$(wc -l < "$OUT/all.txt")
[ "$LIMIT" -gt 0 ] && head -n "$LIMIT" "$OUT/all.txt" > "$OUT/sel.txt" || cp "$OUT/all.txt" "$OUT/sel.txt"
SEL=$(wc -l < "$OUT/sel.txt")
echo "corpus=$TOTAL selected=$SEL chunk=$CHUNK"

sed "s|^|Z:|" "$OUT/sel.txt" > "$OUT/manifest.txt"
# drop chunks from a previous run: `split` only writes as many as this run needs, so
# leftovers would be reprocessed and inflate the record count.
rm -f "$OUT"/chunk-*
split -l "$CHUNK" -d -a 4 "$OUT/manifest.txt" "$OUT/chunk-"

: > "$OUT/vendor.txt"
DLLW="Z:$DLL"
n=0
for c in "$OUT"/chunk-*; do
  n=$((n+1))
  before=$(grep -c '^@@ ' "$OUT/vendor.txt" 2>/dev/null)
  timeout 900 "$WINE" "$OUT/xbsc_probe.exe" "$DLLW" "Z:$c" batch \
      >> "$OUT/vendor.txt" 2>>"$OUT/stderr.log"
  rc=$?
  after=$(grep -c '^@@ ' "$OUT/vendor.txt" 2>/dev/null)
  printf '  chunk %s: rc=%s records=%s\n' "$(basename "$c")" "$rc" "$((after-before))"
  [ "$rc" -ne 0 ] && echo "    (non-zero: that chunk ends at the record below)"
done
echo "records emitted: $(grep -c '^@@ ' "$OUT/vendor.txt")"
exit 0
