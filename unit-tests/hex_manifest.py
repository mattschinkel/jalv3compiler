#!/usr/bin/env python3
"""hex_manifest.py — md5 manifest of the corpus HEX tree (test-gate L2 A/B).

The manifest replaces the old "second full corpus run" HEX A/B: it is
COMMITTED with every compiler change, so each later `unittests.py
--current` doubles as the byte-identity check — diff the fresh tree
against the manifest. Empty diff = codegen-neutral proof. Intentional
codegen changes refresh the manifest in the same commit (`store`), which
also makes the drift reviewable in the commit itself.

Usage:
  python3 unit-tests/hex_manifest.py store  [--hex-dir D] [--manifest F]
  python3 unit-tests/hex_manifest.py diff   [--hex-dir D] [--manifest F]

`store` writes the manifest from the current tree (run after a green
`unittests.py --current`). `diff` exits 1 on any drift (changed/added/
removed .hex) and prints the offending names.

Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""
import argparse
import hashlib
import sys
from datetime import datetime
from pathlib import Path

DEFAULT_HEX_DIR = Path("unit-tests/Output_Current/hex")
DEFAULT_MANIFEST = Path("unit-tests/manifest/hex_manifest.txt")


def tree_md5(hex_dir):
    out = {}
    for path in sorted(hex_dir.glob("*.hex")):
        out[path.name] = hashlib.md5(path.read_bytes()).hexdigest()
    return out


def manifest_read(path):
    if not path.is_file():
        return None
    hashes = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) == 2:
            hashes[parts[1]] = parts[0]
    return hashes


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["store", "diff"])
    ap.add_argument("--hex-dir", type=Path, default=DEFAULT_HEX_DIR)
    ap.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    args = ap.parse_args()

    current = tree_md5(args.hex_dir)
    if not current:
        print(f"error: no .hex files under {args.hex_dir} "
              "(run unittests.py --current first)", file=sys.stderr)
        return 2

    if args.action == "store":
        args.manifest.parent.mkdir(parents=True, exist_ok=True)
        stamp = datetime.now().strftime("%Y-%m-%d %H:%M")
        lines = [f"# jalv2 corpus HEX manifest (md5) — {stamp} — "
                 f"{len(current)} files"]
        lines += [f"{h}  {name}" for name, h in sorted(current.items())]
        args.manifest.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"stored {len(current)} hashes -> {args.manifest}")
        return 0

    stored = manifest_read(args.manifest)
    if stored is None:
        print(f"no manifest at {args.manifest} — create one with: "
              f"python3 {Path(__file__).name} store", file=sys.stderr)
        return 2

    changed = sorted(n for n in stored.keys() & current.keys()
                     if stored[n] != current[n])
    added = sorted(current.keys() - stored.keys())
    removed = sorted(stored.keys() - current.keys())
    same = len(stored.keys() & current.keys()) - len(changed)
    print(f"hex manifest A/B: {same} identical, {len(changed)} changed, "
          f"{len(added)} added, {len(removed)} removed "
          f"({len(stored)} stored / {len(current)} current)")
    for label, names in (("changed", changed), ("added", added),
                         ("removed", removed)):
        for name in names[:20]:
            print(f"  {label}: {name}")
        if len(names) > 20:
            print(f"  {label}: ... {len(names) - 20} more")
    if changed or added or removed:
        print("HEX DRIFT vs manifest — if intended, refresh with: "
              f"python3 {Path(__file__).name} store "
              "and commit the manifest with the change", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
