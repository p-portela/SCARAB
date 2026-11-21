#!/usr/bin/env bash
# Run every verification gate.
#
# The gates themselves live in tests/CMakeLists.txt, so the commands are identical
# on every host; this script only ensures things are configured and built first.
# On Windows run tests/run_tests.ps1 (or: ctest --test-dir build -C Release).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${SCARAB_BUILD_DIR:-$ROOT/build}"

if [ ! -e "$ROOT/vendor" ]; then
  echo "vendor/ not linked; running scripts/link-artifacts.sh"
  "$ROOT/scripts/link-artifacts.sh"
fi

if [ ! -f "$BUILD/CMakeCache.txt" ]; then
  echo "not configured yet; running scripts/build.sh"
  "$ROOT/scripts/build.sh"
else
  cmake --build "$BUILD" --parallel
fi

echo
ctest --test-dir "$BUILD" --output-on-failure
