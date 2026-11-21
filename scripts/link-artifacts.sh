#!/usr/bin/env bash
# Link the artefacts this project builds against and analyses.
#
# The repository never contains Microsoft's binaries or the publisher's game data, so
# builds reach them through symlinks created here. Point XTL_ARTIFACTS at the
# directory holding vendor/ and the extracted corpora.
#
#   XTL_ARTIFACTS=~/Documents/scarab-artifacts   (default)
set -euo pipefail

ART="${XTL_ARTIFACTS:-$HOME/Documents/scarab-artifacts}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ ! -d "$ART" ]; then
  echo "artifacts directory not found: $ART" >&2
  echo "set XTL_ARTIFACTS to the directory holding vendor/ and the corpora" >&2
  exit 1
fi

[ -d "$ART/vendor" ] || { echo "missing $ART/vendor (see docs/PROVENANCE.md)" >&2; exit 1; }
ln -sfn "$ART/vendor" "$ROOT/vendor"

mkdir -p "$ROOT/corpora"
for c in shaders rdna2 incoming; do
  if [ -e "$ART/$c" ]; then
    ln -sfn "$ART/$c" "$ROOT/corpora/$c"
  fi
done

echo "linked artefacts from $ART:"
ls -la "$ROOT/vendor" | head -8
echo
ls -la "$ROOT/corpora"
