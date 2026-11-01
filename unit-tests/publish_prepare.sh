#!/usr/bin/env bash
# publish_prepare.sh — cut/refresh the local `public` branch for GitHub.
#
# The branch is built ONLY from files this script names: it stages the
# git-tracked contents of an explicit allowlist (never the private bench
# material: docs/old/, ../backups/, tmp/, hex_keep/, packs/, compilertest/,
# third_party/, fixes/, docs/investigations/, AGENTS.md, progress.md, ...),
# runs verification sweeps over the staging tree, and cuts an orphan
# `public` branch via git plumbing. This worktree and the private master
# history are never touched.
#
# Publishing is then a separate, explicit step:
#   git clone --branch public --single-branch . /tmp/publish
#   cd /tmp/publish && git push <url> public:master
#
# The .gitignore written onto the branch mirrors the allowlist ("private by
# default"), so content fetched at runtime (third_party/jallib, test output
# trees) stays untracked there forever.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

STAGE="$(mktemp -d)"
IDX="$(mktemp)"
SWEEP="$(mktemp -d)"
trap 'rm -rf "$STAGE" "$IDX" "$SWEEP"' EXIT

# ---------------------------------------------------------------- allowlist
# Fully public directories (tracked files only):
PUBLIC_DIRS="src demo_lib demo_sample sims unit-tests"
# Public files inside otherwise-private directories:
# (the public docs are README.md / NOTICE at the root plus the per-tree
#  READMEs inside the allowed directories; docs/ is investigations-only and
#  stays private)
ROOT_PUBLIC="README.md README.txt NOTICE"
# (pic2jal split out to its own repo 2026-10-28; tools/ is a local pointer only)

# ------------------------------------------------------------------- stage
: > "$STAGE/.filelist"
{
  git ls-files $PUBLIC_DIRS
  git ls-files $ROOT_PUBLIC
} | sort -u > "$STAGE/.filelist"

n_expected=$(wc -l < "$STAGE/.filelist")
while IFS= read -r f; do
  mkdir -p "$STAGE/$(dirname "$f")"
  cp "$ROOT/$f" "$STAGE/$f"
done < "$STAGE/.filelist"
rm "$STAGE/.filelist"

# ------------------------------------------------- allowlist .gitignore
cat > "$STAGE/.gitignore" <<'EOF'
# Private by default: nothing is tracked unless named below.
# third_party/, test output trees, and fetched corpora stay untracked.
/*
!/.gitignore
!/README.md
!/README.txt
!/NOTICE
!/src/
# build artifacts under the re-included trees stay untracked
src/*/.obj-*/
src/*/.dep/
src/*.a
src/*/*.a
src/jal/jal_build.h
!/demo_lib/
!/demo_sample/
!/sims/
!/unit-tests/
# compile artifacts under the re-included trees stay untracked
# (golden canary .hex fixtures are the tracked exception)
*.cod
*.asm
*.out
*.hex
!sims/golden/
!sims/golden/*.hex
EOF

# ------------------------------------------------------------------ sweeps
fail=0

echo "== sweep: personal/internal strings =="
# publish_prepare.sh self-excluded: this grep's own pattern literals live there.
if grep -rniE '/home/matt|powerfulhost|server[0-9]\.|192\.168\.|jalv2_32|mattschinkel@|hs_err' "$STAGE" \
    | grep -v '^Binary.*publish_prepare\.sh\|publish_prepare\.sh:' > "$SWEEP/hits"; then
  echo "PERSONAL STRINGS FOUND:"; head -20 "$SWEEP/hits"; fail=1
else
  echo "clean"
fi

echo "== sweep: symlinks / hidden files / large files =="
if [ -n "$(find "$STAGE" -type l -print -quit)" ]; then
  echo "SYMLINKS FOUND:"; find "$STAGE" -type l; fail=1
else echo "no symlinks"; fi
hidden=$(find "$STAGE" -name '.*' -not -name '.' -not -name '.gitignore' | sed "s|$STAGE/||")
if [ -n "$hidden" ]; then echo "HIDDEN FILES:"; echo "$hidden"; fail=1
else echo "no hidden files"; fi
big=$(find "$STAGE" -type f -size +2M -printf '%s %p\n' | awk -v s="$STAGE" '{printf "%.1fMB %s\n", $1/1048576, substr($2, length(s)+2)}')
if [ -n "$big" ]; then echo "FILES OVER 2MB:"; echo "$big"; fail=1
else
  echo "no files over 2MB (known large, fork-authored: demo_lib/large_array_*.jal ~1.3MB)"
fi

n_files=$(find "$STAGE" -type f | wc -l)
n_bytes=$(find "$STAGE" -type f -printf '%s\n' | awk '{s+=$1} END {printf "%.1f", s/1048576}')
echo "== budget: $n_files files, ${n_bytes} MB (expected $((n_expected + 1)): allowlist + .gitignore) =="
if [ "$n_files" -ne "$((n_expected + 1))" ]; then echo "FILE COUNT MISMATCH"; fail=1; fi

if [ "$fail" -ne 0 ]; then
  echo "PUBLISH SWEEPS FAILED — public branch NOT cut" >&2
  exit 1
fi

# ------------------------------------------------------- cut public branch
export GIT_INDEX_FILE="$IDX"
git read-tree --empty
GIT_WORK_TREE="$STAGE" git add -A
tree=$(git write-tree)
unset GIT_INDEX_FILE
msg="jalv3compiler ${1:-3.0a1-beta} — public tree

Built by unit-tests/publish_prepare.sh from the internal master: sources,
fork libraries, samples, test suites and tooling only. Jallib libraries
and the sample corpus are fetched (unit-tests/fetch_jallib.sh); internal
engineering records stay private. See NOTICE for attribution."
parent=$(git rev-parse -q --verify refs/heads/public 2>/dev/null || true)
if [ -n "$parent" ]; then
  commit=$(git commit-tree "$tree" -p "$parent" -m "$msg")
else
  commit=$(git commit-tree "$tree" -m "$msg")
fi
git update-ref -m "publish_prepare" refs/heads/public "$commit"

echo "== public branch cut: $(git rev-parse --short public) =="
git ls-tree -r --name-only public | awk -F/ '{print $1}' | sort | uniq -c | sort -rn
