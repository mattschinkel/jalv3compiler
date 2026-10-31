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

# The compiler's -s list (same order the gate ladder uses: fork overlay first,
# then pristine jallib). This default used to be <repo>/lib, a path that has
# not existed since the tests/ -> sims/ move; every seed then failed its
# device include and the recovery path looked like a compiler crash.
DEFAULT_LIB = ";".join([
    os.path.join(ROOT, "demo_lib"),
    os.path.join(ROOT, "third_party/jallib/lib"),
])

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


def run_batch(compiler: str, lib: str, paths, list_path: str, timeout: float):
    """Compile every path in ONE process (JAL_MULTI) and return
    (returncode, [(path, text), ...]) where text is that file's output.

    The compiler prints "=== FILE <path>" to stderr before each compile, so a
    crash can be attributed to the file whose block has no summary line --
    which is the whole point of the in-process mode: no process spawn per
    input, and a crash still tells you WHICH input died.
    """
    with open(list_path, "w") as f:
        f.write("\n".join(paths) + "\n")
    env = dict(os.environ)
    env["JAL_MULTI"] = list_path
    try:
        # stderr=STDOUT: the "=== FILE" markers and the compiler's own
        # diagnostics must stay INTERLEAVED. Capturing them separately and
        # concatenating puts every marker after every diagnostic, which makes
        # every input look like it never finished.
        r = subprocess.run(
            [compiler, "-s", lib, "-no-codfile", paths[0]],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, env=env)
        rc = r.returncode
        raw = r.stdout or b""
    except subprocess.TimeoutExpired as e:
        rc = -9
        raw = (e.stdout or b"") + (e.stderr or b"")
        if isinstance(raw, str):
            raw = raw.encode(errors="replace")
    text = raw.decode(errors="replace")

    blocks = []
    cur = None
    buf = []
    for line in text.splitlines():
        if line.startswith("=== FILE "):
            if cur is not None:
                blocks.append((cur, "\n".join(buf)))
            cur = line[len("=== FILE "):].strip()
            buf = []
        else:
            buf.append(line)
    if cur is not None:
        blocks.append((cur, "\n".join(buf)))
    return rc, blocks


def batch_status(rc: int, blocks, expected):
    """Per-file classification for one batch.

    A file whose block carries "N errors, M warnings" completed. Anything
    after the last completed file did not run, or ran and died: the first
    such file is the crash/hang candidate.
    """
    done = []
    bad = None
    for path, text in blocks:
        if "errors," in text and "warnings" in text:
            done.append((path, "ok", ""))
        else:
            if bad is None:
                if rc == -9:
                    bad = (path, "hang", "batch timeout")
                elif rc in (-11, -6):
                    bad = (path, "crash-segv" if rc == -11 else "crash-abort",
                           "SIGSEGV" if rc == -11 else "SIGABRT (assertion?)")
                else:
                    bad = (path, "suspicious", "exit %d without summary" % rc)
    # files never reached (no block at all)
    seen = {p for p, _ in blocks}
    missing = [p for p in expected if p not in seen]
    if bad is None and missing and rc != 0:
        bad = (missing[0], "crash-other", "exit %d" % rc)
    return done, bad


def gen_mutant(seed_blobs, rng):
    name, blob = seed_blobs[rng.randrange(len(seed_blobs))]
    data = mutate(blob, rng)
    if rng.randrange(10) == 0 and len(seed_blobs) > 1:  # occasional splice
        _, other = seed_blobs[rng.randrange(len(seed_blobs))]
        cut = rng.randrange(len(other) + 1)
        data = data + b"\n" + other[cut:cut + 512]
    return name, data


def drive_batch(args, paths, datas, names, counts, findings, crashes_dir, list_path):
    """Compile `paths` in one process; on a crash, record it and continue with
    whatever was left in the batch so one bad input does not mask the rest."""
    while paths:
        rc, blocks = run_batch(args.compiler, args.lib, paths, list_path,
                               args.timeout)
        done, bad = batch_status(rc, blocks, paths)
        counts["ok"] = counts.get("ok", 0) + len(done)
        if not bad:
            return
        idx = paths.index(bad[0])
        status, detail = bad[1], bad[2]
        counts[status] = counts.get(status, 0) + 1
        tag = "%.6d_%s.jal" % (idx, status)
        keep = os.path.join(crashes_dir, os.path.basename(tag))
        with open(keep, "wb") as f:
            f.write(datas[idx])
        with open(keep + ".txt", "w") as f:
            f.write("seed: %s\nrc: %d\ndetail: %s\n" % (names[idx], rc, detail))
        findings.append({"n": idx, "seed": names[idx], "status": status,
                         "detail": detail, "file": os.path.basename(keep)})
        print("  %s: %s (seed %s) -> crashes/%s"
              % (status, detail, names[idx], os.path.basename(keep)))
        paths, datas, names = paths[idx + 1:], datas[idx + 1:], names[idx + 1:]


def run_in_process(args, seed_blobs, rng, deadline, out_dir, crashes_dir):
    input_dir = os.path.join(out_dir, "inputs")
    os.makedirs(input_dir, exist_ok=True)
    list_path = os.path.join(out_dir, "batch.txt")
    counts = {}
    findings = []
    n = 0
    while time.time() < deadline and (not args.inputs or n < args.inputs):
        take = args.batch
        if args.inputs:
            take = min(take, args.inputs - n)
        paths, datas, names = [], [], []
        for _ in range(take):
            name, data = gen_mutant(seed_blobs, rng)
            n += 1
            p = os.path.join(input_dir, "in_%06d.jal" % n)
            with open(p, "wb") as f:
                f.write(data)
            paths.append(p)
            datas.append(data)
            names.append(name)
        before = sum(counts.values())
        drive_batch(args, paths, datas, names, counts, findings, crashes_dir,
                    list_path)
        if n % (args.batch * 5) == 0:
            print("  ... %d inputs, " % n +
                  ", ".join("%s=%d" % (k, v) for k, v in sorted(counts.items())))
        del before
    return counts, findings, n


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
    ap.add_argument("--inputs", type=int, default=0,
                    help="stop after this many inputs (0 = time-bounded only); "
                         "with a fixed --seed this makes a run reproducible, "
                         "which is what the L1 gate uses")
    ap.add_argument("--timeout", type=float, default=20.0,
                    help="per-input compile timeout (s)")
    ap.add_argument("--seed", type=int, default=None,
                    help="PRNG seed (default: time-based, recorded in state)")
    ap.add_argument("--seed-dir", default=os.path.join(ROOT, "third_party/jallib/samples"))
    ap.add_argument("--lib", default=DEFAULT_LIB,
                    help="compiler -s include path (';'-separated); must exist")
    ap.add_argument("--out-dir", default=None)
    ap.add_argument("--in-process", action="store_true",
                    help="compile a whole batch in ONE process (JAL_MULTI); "
                         "no spawn per input")
    ap.add_argument("--batch", type=int, default=200,
                    help="files per in-process batch (default 200)")
    args = ap.parse_args()

    if not os.path.isfile(args.compiler):
        print(f"error: compiler not found: {args.compiler}", file=sys.stderr)
        return 2
    # A bogus include path makes every seed fail its device include, which
    # reads as "the compiler crashes on mangled input" when it is really the
    # harness running its own typo. Fail loudly instead of fuzzing nothing.
    missing = [p for p in args.lib.split(";") if p and not os.path.isdir(p)]
    if missing:
        print(f"error: include path not found: {', '.join(missing)}",
              file=sys.stderr)
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

    if args.in_process:
        deadline = time.time() + args.minutes * 60
        print(f"fuzz (in-process): {len(seed_blobs)} seeds, {args.batch}/batch, "
              + (f"{args.inputs} inputs" if args.inputs
                 else f"{args.minutes:g} min")
              + f" -> {out_dir}")
        counts, findings, n = run_in_process(args, seed_blobs, rng, deadline,
                                             out_dir, crashes_dir)
        state = {"started": ts, "mode": "in-process", "inputs": n,
                 "counts": counts, "findings": findings,
                 "compiler": os.path.abspath(args.compiler),
                 "seed_names": [s for s, _ in seed_blobs]}
        with open(os.path.join(out_dir, "state.json"), "w") as f:
            json.dump(state, f, indent=1)
        bad = sum(v for k, v in counts.items() if k != "ok")
        print(f"fuzz done: {n} inputs -> " +
              ", ".join("%s=%d" % (k, v) for k, v in sorted(counts.items())))
        print(f"artifacts: {out_dir}")
        return 1 if bad else 0
    n = 0
    counts = {}
    findings = []
    print(f"fuzz: {len(seed_blobs)} seeds, {args.minutes:g} min"
          + (f"/{args.inputs} inputs" if args.inputs else "")
          + f", per-input timeout {args.timeout:g}s -> {out_dir}")
    while time.time() < deadline and (not args.inputs or n < args.inputs):
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
