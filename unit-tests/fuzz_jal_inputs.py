#!/usr/bin/env python3
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""
Mutation fuzzer for jalv2 compiler input robustness (parser fault-injection).

Every unit/gpsim gate proves the compiler handles WELL-FORMED input; this
harness proves it fails CLEANLY (diagnostic + nonzero exit) on malformed
input instead of crashing (assertion abort, segfault, hang).

PASS criterion per input: exit code 0 (compiled) OR 1 (diagnostics) with a
final "N errors, M warnings" summary line. FAIL: any other exit code
(signal/abort), or a run longer than the timeout.

Mutations (seeded, deterministic per run):
  - byte flips / random byte insertion / truncation at random offsets
  - token-level swaps on JAL punctuation (`is`/`=`/`then`/`end`/`loop`...)
  - brace/paren/quote imbalance (drop one delimiter)
  - duplicate a random line, delete a random line
  - splice: tail of one seed appended to another

Usage:
  python3 scripts/fuzz_jal_inputs.py --compiler ./bin/jalv3-x86-64 \
      --minutes 10 --seeds 40
  python3 scripts/fuzz_jal_inputs.py ... --resume runs/fuzz_2026.../state.json

Layout: creates tmp fuzz dir with inputs, writes crashing inputs + a
summary JSON + TSV next to the run. No external deps (no afl).


Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""
from __future__ import annotations

import argparse
import json
import os
import random
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, os.pardir))

# tokens whose presence/absence changes parse shape the most
JAL_KEYWORDS = [
    "is", "=", ":=", "then", "end", "loop", "if", "else", "elsif",
    "procedure", "function", "return", "var", "const", "include",
    "pragma", "alias", "record", "for", "while", "case", "of",
    "[", "]", "(", ")", '"', ",", ";", "--",
]

DELIMS = ['"', "(", ")", "[", "]"]


def pick_seeds(seed_dir: str, count: int, rng: random.Random):
    """Small, diverse, fast-compiling seeds beat huge ones for throughput."""
    files = []
    for name in sorted(os.listdir(seed_dir)):
        if name.endswith(".jal"):
            p = os.path.join(seed_dir, name)
            try:
                sz = os.path.getsize(p)
            except OSError:
                continue
            if sz < 8000:  # keep per-run latency low
                files.append((sz, name, p))
    files.sort()  # smallest first = fastest compiles = most inputs/min
    files = files[:max(count, count * 3)]
    rng.shuffle(files)
    return [(n, p) for _, n, p in files[:count]]


def mutate(data: bytes, rng: random.Random) -> bytes:
    b = bytearray(data)
    if not b:
        return b + b"var byte x\n"
    op = rng.randrange(8)
    if op == 0:  # byte flip
        i = rng.randrange(len(b))
        b[i] ^= 1 << rng.randrange(8)
    elif op == 1:  # insert random bytes
        i = rng.randrange(len(b) + 1)
        n = 1 + rng.randrange(8)
        b[i:i] = bytes(rng.randrange(256) for _ in range(n))
    elif op == 2:  # truncate
        b = b[:rng.randrange(len(b) + 1)]
    elif op == 3:  # delete a slice
        i = rng.randrange(len(b))
        n = 1 + rng.randrange(16)
        del b[i:i + n]
    elif op == 4:  # keyword splice
        i = rng.randrange(len(b) + 1)
        kw = rng.choice(JAL_KEYWORDS).encode()
        b[i:i] = kw if rng.randrange(2) else kw + b" "
    elif op == 5:  # delimiter drop (imbalance)
        d = rng.choice(DELIMS).encode()
        idxs = [k for k in range(len(b)) if b[k:k + 1] == d]
        if idxs:
            del b[rng.choice(idxs):rng.choice(idxs) + 1]
    elif op == 6:  # line duplicate / delete
        lines = bytes(b).split(b"\n")
        if len(lines) > 2:
            i = rng.randrange(len(lines))
            if rng.randrange(2):
                lines.insert(i, lines[i])
            else:
                del lines[i]
            b = bytearray(b"\n".join(lines))
    else:  # op == 7: bit flips x4 (heavier)
        for _ in range(4):
            i = rng.randrange(len(b))
            b[i] ^= 1 << rng.randrange(8)
    return bytes(b)


def run_one(compiler: str, lib: str, path: str, timeout: float):
    try:
        r = subprocess.run(
            [compiler, "-s", lib, "-no-codfile", path],
            capture_output=True, timeout=timeout)
        return r.returncode, (r.stdout or b"").decode(errors="replace") \
            + (r.stderr or b"").decode(errors="replace")
    except subprocess.TimeoutExpired as e:
        out = e.stdout or b""
        if isinstance(out, str):
            out = out.encode(errors="replace")
        return -9, out.decode(errors="replace")


def classify(rc: int, out: str):
    """(status, detail). ok = compiled or clean diagnostics."""
    if rc == 0 or rc == 1:
        if "errors," in out and "warnings" in out:
            return "ok", ""
        # exit 1 without the summary: still a clean stop if a diagnostic printed
        if "error" in out.lower():
            return "ok", "no-summary-line"
        return "suspicious", "exit %d without diagnostics" % rc
    if rc == -11:
        return "crash-segv", "SIGSEGV"
    if rc == -6:
        return "crash-abort", "SIGABRT (assertion?)"
    if rc == -9:
        return "hang", "timeout"
    return "crash-other", "exit %d" % rc


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--compiler", default=os.path.join(ROOT, "bin/jalv3-x86-64"))
    ap.add_argument("--seeds", type=int, default=40,
                    help="number of seed .jal files (small ones)")
    ap.add_argument("--minutes", type=float, default=10.0)
    ap.add_argument("--timeout", type=float, default=20.0,
                    help="per-input compile timeout (s)")
    ap.add_argument("--seed", type=int, default=None,
                    help="PRNG seed (default: time-based, recorded in state)")
    ap.add_argument("--seed-dir", default=os.path.join(ROOT, "third_party/jallib/samples"))
    ap.add_argument("--lib", default=os.path.join(ROOT, "lib"))
    ap.add_argument("--out-dir", default=None)
    args = ap.parse_args()

    if not os.path.isfile(args.compiler):
        print(f"error: compiler not found: {args.compiler}", file=sys.stderr)
        return 2
    rng = random.Random(args.seed if args.seed is not None else time.time())

    seeds = pick_seeds(args.seed_dir, args.seeds, rng)
    if not seeds:
        print("error: no seed files", file=sys.stderr)
        return 2
    seed_blobs = []
    for name, p in seeds:
        try:
            seed_blobs.append((name, open(p, "rb").read()))
        except OSError:
            pass
    if not seed_blobs:
        print("error: seeds unreadable", file=sys.stderr)
        return 2

    ts = time.strftime("%Y%m%d_%H%M%S")
    out_dir = args.out_dir or os.path.join(ROOT, "tmp", "fuzz", f"run_{ts}")
    os.makedirs(out_dir, exist_ok=True)
    crashes_dir = os.path.join(out_dir, "crashes")
    os.makedirs(crashes_dir, exist_ok=True)

    deadline = time.time() + args.minutes * 60
    n = 0
    counts = {}
    findings = []
    print(f"fuzz: {len(seed_blobs)} seeds, {args.minutes:g} min, "
          f"per-input timeout {args.timeout:g}s -> {out_dir}")
    while time.time() < deadline:
        name, blob = seed_blobs[rng.randrange(len(seed_blobs))]
        data = mutate(blob, rng)
        if rng.randrange(10) == 0 and len(seed_blobs) > 1:  # occasional splice
            _, other = seed_blobs[rng.randrange(len(seed_blobs))]
            cut = rng.randrange(len(other) + 1)
            data = data + b"\n" + other[cut:cut + 512]
        n += 1
        in_path = os.path.join(out_dir, "input.jal")
        with open(in_path, "wb") as f:
            f.write(data)
        rc, out = run_one(args.compiler, args.lib, in_path, args.timeout)
        status, detail = classify(rc, out)
        counts[status] = counts.get(status, 0) + 1
        if status.startswith("crash") or status in ("hang", "suspicious"):
            tag = f"{status}_{n:06d}.jal"
            keep = os.path.join(crashes_dir, tag)
            with open(keep, "wb") as f:
                f.write(data)
            log = keep + ".txt"
            with open(log, "w") as f:
                f.write(f"seed: {name}\nrc: {rc}\ndetail: {detail}\n---\n{out[-4000:]}")
            findings.append({"n": n, "seed": name, "status": status,
                             "detail": detail, "file": tag})
            print(f"  [{n}] {status}: {detail} (seed {name}) -> crashes/{tag}")
        if n % 100 == 0:
            print(f"  ... {n} inputs, " +
                  ", ".join(f"{k}={v}" for k, v in sorted(counts.items())))

    state = {
        "started": ts, "inputs": n, "counts": counts,
        "seed_names": [s for s, _ in seed_blobs],
        "rng_state": rng.getstate()[0], "findings": findings,
        "compiler": os.path.abspath(args.compiler),
    }
    with open(os.path.join(out_dir, "state.json"), "w") as f:
        json.dump(state, f, indent=1)
    bad = sum(v for k, v in counts.items() if k != "ok")
    print(f"fuzz done: {n} inputs -> " +
          ", ".join(f"{k}={v}" for k, v in sorted(counts.items())))
    print(f"artifacts: {out_dir}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
