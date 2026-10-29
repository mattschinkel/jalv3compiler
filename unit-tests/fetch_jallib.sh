#!/usr/bin/env bash
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
# fetch_jallib.sh — download the pinned pristine jallib tree and populate
# third_party/jallib/{lib,samples}. This repository does not vendor jallib:
# the libraries and sample corpus are fetched from upstream (ZLIB licensed)
# at a pinned tag, checksum-verified, then shaped the way this repo's gates
# expect:
#   - include/**.jal flattens into third_party/jallib/lib  (the -s library
#     search is flat; the upstream tree has no basename collisions)
#   - sample/**.jal flattens into third_party/jallib/samples
#   - scripts/jallib_exclusions.txt files are dropped (dead device libs and
#     their samples, absent from the corpus this repo is gated against)
#   - tests/corpus_overlays/* overwrite their corpus counterparts (fork
#     adaptations recorded in name_changes.md, e.g. 12f617_tasks.jal)
#
# Usage: unit-tests/fetch_jallib.sh [--force]
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${JALLIB_DEST:-$ROOT/third_party/jallib}"
PIN="$(dirname "$0")/jallib_pinned.txt"
EXCL="$ROOT/unit-tests/jallib_exclusions.txt"
OVERLAYS="$ROOT/unit-tests/corpus_overlays"

TAG="$(sed -n 's/^tag=//p' "$PIN")"
URL="$(sed -n 's/^url=//p' "$PIN")"
SHA="$(sed -n 's/^sha256=//p' "$PIN")"
SRC_DIR="jallib-$TAG"

if [ -d "$DEST/lib" ] && [ -n "$(ls -A "$DEST/lib" 2>/dev/null)" ] && [ "$1" != "--force" ]; then
  echo "third_party/jallib/lib already populated; use --force to refetch" >&2
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "Downloading $URL"
curl -sL -o "$TMP/jallib.tar.gz" "$URL"
echo "$SHA  $TMP/jallib.tar.gz" | sha256sum -c -

tar xzf "$TMP/jallib.tar.gz" -C "$TMP" "$SRC_DIR/include" "$SRC_DIR/sample"

mkdir -p "$DEST/lib" "$DEST/samples"
find "$TMP/$SRC_DIR/include" -name '*.jal' -exec sh -c 'cp "$1" "'"$DEST"'/lib/$(basename "$1")"' _ {} \;
find "$TMP/$SRC_DIR/sample"  -name '*.jal' -exec sh -c 'cp "$1" "'"$DEST"'/samples/$(basename "$1")"' _ {} \;

if [ -f "$EXCL" ]; then
  while IFS= read -r f; do
    case "$f" in '#'*) continue ;; esac
    [ -n "$f" ] || continue
    rm -f "$DEST/lib/$f" "$DEST/samples/$f"
  done < "$EXCL"
fi

if [ -d "$OVERLAYS" ]; then
  for f in "$OVERLAYS"/*.jal; do
    [ -e "$f" ] || continue
    cp "$f" "$DEST/samples/$(basename "$f")"
  done
fi

echo "third_party/jallib/lib:     $(find "$DEST/lib" -name '*.jal' | wc -l) libraries"
echo "third_party/jallib/samples: $(find "$DEST/samples" -name '*.jal' | wc -l) samples"
