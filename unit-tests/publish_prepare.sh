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
# Everything the repo TRACKS is publishable; this list is the whole of it.
# .github is the CI workflows, doc/ the documentation sources, documentation/
# the manuals (jalv3.pdf is the language reference).  bin/old_versions/ is
# deliberately NOT here -- 20.3 MB of prebuilt third-party binaries.
PUBLIC_DIRS=".github doc documentation src demo_lib demo_sample sims unit-tests"
# Public files inside otherwise-private directories:
# (the public docs are README.md / NOTICE at the root plus the per-tree
#  READMEs inside the allowed directories; docs/ is investigations-only and
#  stays private)
ROOT_PUBLIC="README.md README.txt NOTICE"
# Release compilers, published as STRIPPED copies of bin/ (see the strip step
# below).  Named individually: bin/ also holds old_versions/ and the
# instrumented -cov/-san flavors, which must never be published.
PUBLIC_BIN="bin/jalv3-x86-64 bin/jalv3-win64.exe"
# bin/old_versions/ is the RELEASED jalv2 line (jalv24q6*, jalv25*, r1..r9;
# 35 files, 20.3 MB, largest 1.3 MB).  Published AS RELEASED -- these are the
# artifacts as shipped, so they are NOT stripped.  A versioned jALv3 binary
# never goes here: jALv3 is beta and its builds are replaced in place.
PUBLIC_BIN_ARCHIVE="bin/old_versions"
# (pic2jal split out to its own repo 2026-10-28; tools/ is a local pointer only)

# ------------------------------------------------------------------- stage
: > "$STAGE/.filelist"
{
  git ls-files $PUBLIC_DIRS
  git ls-files $ROOT_PUBLIC
  git ls-files $PUBLIC_BIN_ARCHIVE
  for f in $PUBLIC_BIN; do echo "$f"; done
} | sort -u > "$STAGE/.filelist"

n_expected=$(wc -l < "$STAGE/.filelist")
while IFS= read -r f; do
  mkdir -p "$STAGE/$(dirname "$f")"
  cp "$ROOT/$f" "$STAGE/$f"
done < "$STAGE/.filelist"
rm "$STAGE/.filelist"

# Publish STRIPPED compilers.  The internal builds carry -g, so their DWARF
# holds absolute build paths, which the personal-string sweep must reject --
# and it also brings both well under the 2 MB sweep limit
# (jalv3-x86-64 4193920 -> ~953k, jalv3-win64.exe 4837025 -> ~909k).
# The private bin/ artifacts are left untouched.
for f in $PUBLIC_BIN; do
  case "$f" in
    *.exe) strip_cmd="x86_64-w64-mingw32-strip" ;;
    *)     strip_cmd="strip" ;;
  esac
  if ! command -v "$strip_cmd" >/dev/null 2>&1; then
    echo "publish_prepare: $strip_cmd not found; cannot publish a stripped $f" >&2
    exit 1
  fi
  if ! "$strip_cmd" "$STAGE/$f"; then
    echo "publish_prepare: strip failed for $f" >&2
    exit 1
  fi
done

# ------------------------------------------------- allowlist .gitignore
cat > "$STAGE/.gitignore" <<'EOF'
# Private by default: nothing is tracked unless named below.
# third_party/, test output trees, and fetched corpora stay untracked.
/*
!/.gitignore
!/README.md
!/README.txt
!/NOTICE
!/.github/
!/doc/
# doc/ build intermediates are already untracked upstream; keep them out here too
doc/pdf/
doc/html/
!/documentation/
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
!/bin/
# bin/: the two current compilers plus the released jalv2 line; never the
# instrumented -cov/-san flavors, never a versioned jALv3 build
/bin/*
!/bin/jalv3-x86-64
!/bin/jalv3-win64.exe
# released jalv2 binaries, but never a versioned jALv3 (beta) build
!/bin/old_versions/
/bin/old_versions/jalv3*
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
# .github is a tracked dot-DIRECTORY (the CI workflows); every other dot entry
# is an accident worth failing on (.DS_Store, .env, editor droppings).
hidden=$(find "$STAGE" -name '.*' -not -name '.' -not -name '.gitignore' \
    -not -path "$STAGE/.github" | sed "s|$STAGE/||")
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
