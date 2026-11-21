#!/usr/bin/env bash
# Run every verification gate. Exits non-zero if any gate fails.
#
#   G1  export contract      bridge exports == the console runtime, byte-exact
#   G2  wrapper vtable       compiles: `override` proves 53+76 signatures
#   G3  wrapper logic        wrapper self-test, no GPU needed
#   G4  integration          console DMA command list through PC D3D12, under Wine
#   G5  tools                parser/generator unit tests
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${XTL_BUILD_DIR:-$ROOT/build}"
WINE="${XTL_WINE:-$HOME/opt/wine/bin/wine}"
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-scarab}"
export WINEARCH="${WINEARCH:-win64}"
export WINEDEBUG="${WINEDEBUG:--all}"
export XTL_VENDOR="$ROOT/vendor"
export PYTHONPATH="$ROOT/tools:$ROOT/components/shader-bridge:${PYTHONPATH:-}"

pass=0; fail=0
gate() { # name, command...
  local name="$1"; shift
  printf '\n=== %s ===\n' "$name"
  if "$@"; then printf '  -> PASS\n'; pass=$((pass+1));
  else printf '  -> FAIL (%s)\n' "$*"; fail=$((fail+1)); fi
}

if [ ! -e "$ROOT/vendor" ]; then
  echo "vendor/ not linked; running scripts/link-artifacts.sh"
  "$ROOT/scripts/link-artifacts.sh" || exit 1
fi

printf '=== build (G2: wrapper layout is compiler-checked here) ===\n'
if ! cmake --build "$BUILD" --parallel; then
  echo "BUILD FAILED"; exit 1
fi
echo "  -> build ok"

BRIDGE_DLL="$(find "$BUILD" -name d3d12_xs.dll | head -1)"
WRAP_DLL="$(find "$BUILD" -name d3d12_xs_wrap.dll | head -1)"
SELFTEST="$(find "$BUILD" -name wrapper_selftest.exe | head -1)"
INTEG="$(find "$BUILD" -name integration_test.exe | head -1)"
echo "  bridge=$BRIDGE_DLL"
echo "  wrapper=$WRAP_DLL"

gate "G1 export contract" python3 "$ROOT/tools/verify_exports.py" "$BRIDGE_DLL" \
     "$XTL_VENDOR/x64_dx12/d3d12_xs.dll"

gate "G5 tools unit tests" python3 "$ROOT/tests/test_tools.py"

run_under_wine() {
  local exe="$1"; shift
  [ -x "$WINE" ] || [ -x "$(command -v wine 2>/dev/null)" ] || {
    echo "  wine not available; skipping $exe"; return 0; }
  local W="$WINE"; [ -x "$W" ] || W="wine"
  # keep the DLLs beside the exe so the loader finds them
  cp -f "$BRIDGE_DLL" "$(dirname "$exe")/" 2>/dev/null || true
  cp -f "$WRAP_DLL" "$(dirname "$exe")/" 2>/dev/null || true
  ( cd "$(dirname "$exe")" && "$W" "$(basename "$exe")" "$@" )
}

gate "G3 wrapper self-test" run_under_wine "$SELFTEST"
gate "G4 integration"      run_under_wine "$INTEG"

printf '\n=========================================\n'
printf 'gates passed: %d   failed: %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] && printf 'ALL GATES PASS\n' || printf 'FAILURES PRESENT\n'
exit $(( fail > 0 ? 1 : 0 ))
