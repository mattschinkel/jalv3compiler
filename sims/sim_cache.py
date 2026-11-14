#!/usr/bin/env python3
"""sim_cache.py — sim-result cache for the jalv3 test gates.

A sample's simulator run is deterministic given three things:
  1. the compiled HEX (date-masked: the MIPS loader embeds a build-date
     word that changes at midnight, so raw bytes are NOT day-stable —
     see sims/_hex_datemask.py),
  2. the simulator engine and its version string,
  3. the sample source (its @sim_validate / @gpsim_validate / @picsim_validate
     block: steps, expects, uart, etc.).

When the gate runner exports JAL_SIM_CACHE=<dir>, the validate scripts
compute this key after compiling and skip the simulator when today's
marker already records a pass.  Markers live under
<dir>/<engine>/<YYYYMMDD>/<key> (UTC), so the first run of a new day is a
full run by construction; the L3 landing run forces a full run by not
exporting the variable.  The cache is fail-safe: any mismatch only causes
a cache miss, never a skipped re-run.

CLI (called once per sample by the validate scripts):
  python3 sims/sim_cache.py lookup ENGINE VERSION HEX SRC
    prints "HIT <key>" when today's marker exists, else "MISS <key>";
    always exits 0 (the string is the signal, not the exit code).

The same functions are imported directly by sims/picsim_validate_engine.py.

Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""
from __future__ import annotations

import hashlib
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

_THIS = Path(__file__).resolve().parent


def masked_hex_sha256(hex_path: Path) -> str:
    """sha256 of the date-masked Intel HEX text (see _hex_datemask.norm)."""
    sys.path.insert(0, str(_THIS))
    import _hex_datemask  # noqa: F401  (local module in sims/)
    lines = _hex_datemask.norm(str(hex_path))
    return hashlib.sha256(("\n".join(lines) + "\n").encode("utf-8")).hexdigest()


def compute_key(engine: str, version: str, hex_path, src_path) -> str:
    """Cache key: engine, engine version, date-masked HEX, source bytes."""
    h = hashlib.sha256()
    parts = (
        engine.encode("utf-8", "replace"),
        version.encode("utf-8", "replace"),
        masked_hex_sha256(Path(hex_path)).encode("ascii"),
        hashlib.sha256(Path(src_path).read_bytes()).hexdigest().encode("ascii"),
    )
    for part in parts:
        h.update(part)
        h.update(b"\n")
    return h.hexdigest()


def cache_root() -> str:
    return os.environ.get("JAL_SIM_CACHE", "")


def marker_path(root: str, engine: str, key: str) -> Path:
    day = datetime.now(timezone.utc).strftime("%Y%m%d")
    return Path(root) / engine / day / key


def lookup(engine: str, version: str, hex_path, src_path) -> tuple[bool, str]:
    key = compute_key(engine, version, hex_path, src_path)
    root = cache_root()
    if not root:
        return False, key
    return marker_path(root, engine, key).is_file(), key


def main() -> int:
    if len(sys.argv) != 6 or sys.argv[1] != "lookup":
        print(__doc__, file=sys.stderr)
        return 2
    _, _, engine, version, hex_path, src_path = sys.argv
    hit, key = lookup(engine, version, hex_path, src_path)
    print(("HIT " if hit else "MISS ") + key)
    return 0


if __name__ == "__main__":
    sys.exit(main())
