#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Pedro Portela
# Configure and build every component. Cross-compiles for 64-bit Windows.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${SCARAB_BUILD_DIR:-$ROOT/build}"

if [ ! -e "$ROOT/vendor" ]; then
  echo "vendor/ not linked; running scripts/link-artifacts.sh"
  "$ROOT/scripts/link-artifacts.sh"
fi

cmake -S "$ROOT" -B "$BUILD" \
      -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-mingw.cmake" \
      -DCMAKE_BUILD_TYPE="${SCARAB_BUILD_TYPE:-Release}" \
      "$@"

cmake --build "$BUILD" --parallel

echo
echo "built artefacts:"
find "$BUILD" -name '*.dll' -o -name '*.exe' | sort
