#!/usr/bin/env python3
"""Validate a JAL test on picsim (16F18857 model).

A test declares its expectations in a block:

  -- @picsim_validate begin
  -- steps: 20000
  -- device: PIC16F18857          (optional)
  -- expect byte v_a 0x13
  -- expect word v_ww 0x00A8
  -- expect dword_le v_d 0x0001B2C3
  -- pin_flips: RA0 16
  -- apin: RB3 2.5
  -- uart_contains: ##DONE##
  -- @picsim_validate end

The engine compiles the .jal with jalv3, resolves `expect` symbols against
the compiled .asm EQU lines, runs the hex through sims/picsim_runner (a
GPL helper linked against libpicsim) and compares RAM / pins / UART text.

Usage:
  python3 sims/picsim_validate_engine.py TEST.jal --picsim DIR
  PICSIM_DIR=... python3 sims/picsim_validate_engine.py TEST.jal
"""
from __future__ import annotations

import argparse
import os
import pty
import re
import select
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INCLUDE_DIRS = ("sims/shared", "demo_lib", "third_party/jallib/lib")


def parse_block(text: str) -> dict:
    m = re.search(r"@picsim_validate\s+begin(.*?)@picsim_validate\s+end", text, re.S)
    if not m:
        raise SystemExit("no @picsim_validate block found")
    spec: dict = {"expects": [], "steps": 20000, "device": "PIC16F18857",
                  "pin_flips": None, "apin": None, "uart_contains": None,
                  "frequency_hz": None}
    for raw in m.group(1).splitlines():
        line = raw.strip()
        if line.startswith("--"):
            line = line[2:].strip()
        if not line or ":" not in line and not line.startswith("expect"):
            continue
        if line.startswith("expect "):
            parts = line.split()
            if len(parts) != 4:
                raise SystemExit(f"bad expect line: {line!r}")
            _, kind, sym, val = parts
            if kind not in ("byte", "word", "dword_le"):
                raise SystemExit(f"bad expect type: {kind!r}")
            spec["expects"].append((kind, sym, int(val, 0)))
            continue
        key, _, val = line.partition(":")
        key, val = key.strip().lower(), val.strip()
        if key == "steps":
            spec["steps"] = int(val, 0)
        elif key == "device":
            spec["device"] = val
        elif key == "pin_flips":
            name, _, count = val.partition(" ")
            spec["pin_flips"] = (name, int(count, 0))
        elif key == "apin":
            name, _, volts = val.partition(" ")
            spec["apin"] = (name, float(volts))
        elif key == "uart_contains":
            spec["uart_contains"] = val
    return spec


def equ_map(asm_text: str) -> dict:
    out = {}
    for line in asm_text.splitlines():
        m = re.match(r"\s*(\w+)\s+EQU\s+0x([0-9A-Fa-f]+)", line, re.I)
        if m:
            out[m.group(1)] = int(m.group(2), 16)
    return out


def resolve(sym: str, equs: dict) -> int:
    m = re.match(r"^(\w+)(?:\+(\d+)|-(\d+))?$", sym)
    if not m:
        raise SystemExit(f"bad symbol {sym!r}")
    base = equs.get(m.group(1))
    if base is None:
        raise SystemExit(f"symbol {sym!r} not found in .asm (EQU)")
    if m.group(2):
        base += int(m.group(2))
    if m.group(3):
        base -= int(m.group(3))
    return base


def build_runner(picsim: Path, out: Path) -> Path:
    runner = out / "picsim_runner"
    src = ROOT / "sims" / "picsim_runner.c"
    cmd = ["gcc", "-O2", "-o", str(runner), str(src),
           "-I", str(picsim / "include"), "-L", str(picsim / "src"),
           "-lpicsim", "-lm"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        raise SystemExit("failed to build picsim_runner")
    return runner


def read_ram(path: Path) -> dict:
    ram = {}
    for line in path.read_text().splitlines():
        a, v = line.split()
        ram[int(a, 16)] = int(v, 16)
    return ram


def run_runner(runner: Path, picsim: Path, spec: dict, hexf: Path,
               builddir: Path) -> tuple[dict, bytes]:
    ramout = builddir / "ram.txt"
    args = [str(runner), spec["device"], str(hexf), str(spec["steps"]), str(ramout)]
    if spec["pin_flips"]:
        args += ["--pin", spec["pin_flips"][0]]
    if spec["apin"]:
        args += ["--apin", f"{spec['apin'][0]}:{spec['apin'][1]}"]
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = str(picsim / "src")

    master = slave = None
    if spec["uart_contains"]:
        master, slave = pty.openpty()
        args += ["--uart", os.ttyname(slave)]

    proc = subprocess.Popen(args, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    uart = b""
    deadline = time.time() + 300
    while proc.poll() is None and time.time() < deadline:
        if master is not None:
            r, _, _ = select.select([master], [], [], 0.05)
            if r:
                try:
                    uart += os.read(master, 4096)
                except OSError:
                    break
        else:
            time.sleep(0.01)
    if master is not None:
        while True:
            r, _, _ = select.select([master], [], [], 0.05)
            if not r:
                break
            try:
                chunk = os.read(master, 4096)
            except OSError:
                break
            if not chunk:
                break
            uart += chunk
        os.close(master)
    if slave is not None:
        os.close(slave)

    out = proc.communicate()[0].decode("utf-8", "replace")
    info = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0] in ("pc", "unkop", "pin"):
            info.setdefault(parts[0], []).append(parts[1:])
    if os.environ.get("PICSIM_DEBUG"):
        sys.stderr.write(f"runner rc={proc.returncode} uart={uart[:60]!r} stdout={out[:200]!r}\n")
    if proc.returncode != 0:
        sys.stderr.write(out)
        raise SystemExit(f"picsim_runner exited {proc.returncode}")
    return info, uart


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("test")
    ap.add_argument("--picsim", default=os.environ.get("PICSIM_DIR"),
                    help="picsim source tree (with include/ and src/libpicsim)")
    ap.add_argument("--compiler", default=str(ROOT / "bin" / "beta" / "jalv3-x86-64"))
    ap.add_argument("--steps", type=int, default=None)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    if not args.picsim:
        raise SystemExit("set --picsim DIR or PICSIM_DIR (picsim source tree)")
    picsim = Path(args.picsim).resolve()
    if not (picsim / "src" / "libpicsim.a").exists() and not (picsim / "src" / "libpicsim.so").exists():
        raise SystemExit(f"{picsim}: no built libpicsim (run make -C src)")

    compiler = str(Path(args.compiler).expanduser().resolve())
    test = Path(args.test).resolve()
    spec = parse_block(test.read_text())
    if args.steps is not None:
        spec["steps"] = args.steps

    build = ROOT / "tmp" / "picsim_build" / test.stem
    if build.exists():
        shutil.rmtree(build)
    build.mkdir(parents=True)
    jal = build / test.name
    shutil.copy(test, jal)

    cmd = [compiler]
    for d in INCLUDE_DIRS:
        cmd += ["-s", str(ROOT / d)]
    cmd.append(str(jal))
    r = subprocess.run(cmd, cwd=build, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("compile failed")

    asm = jal.with_suffix(".asm")
    hexf = jal.with_suffix(".hex")
    equs = equ_map(asm.read_text()) if asm.exists() else {}

    runner = build_runner(picsim, build)
    info, uart = run_runner(runner, picsim, spec, hexf, build)
    ram = read_ram(build / "ram.txt")

    failures = []
    if info.get("unkop", [["1"]])[0][0] != "0":
        failures.append("unknown-opcode flag set")
    for kind, sym, want in spec["expects"]:
        addr = resolve(sym, equs)
        n = {"byte": 1, "word": 2, "dword_le": 4}[kind]
        got = sum(ram.get(addr + i, 0) << (8 * i) for i in range(n))
        if got != want:
            failures.append(f"{sym}: got 0x{got:0{2*n}X} want 0x{want:0{2*n}X}")
    if spec["pin_flips"]:
        name, want = spec["pin_flips"]
        line = info.get("pin", [])
        got = int(line[0][1]) if line and line[0][0] == name else -1
        if got != want:
            failures.append(f"pin {name}: {got} flips, want {want}")
    if spec["uart_contains"]:
        if spec["uart_contains"].encode() not in uart:
            failures.append(f"uart: {spec['uart_contains']!r} not in {uart[:80]!r}")

    if failures:
        print(f"FAIL {test.name}")
        for f in failures:
            print(f"  {f}")
        return 1
    print(f"PASS {test.name} ({spec['steps']} steps, {len(spec['expects'])} expects)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
