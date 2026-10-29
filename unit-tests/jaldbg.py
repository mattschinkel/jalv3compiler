#!/usr/bin/env python3
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""jaldbg -- symbol-aware investigation harness for the jalv3 compiler.

Born from the 2026-10-29 proc-entry-BSR chase: most of that session went
into hand-rolled hex parsing, ad-hoc gpsim driving, and scratch A/B
builds that got contaminated twice. This tool collapses those steps into
one place. It is a DIAGNOSIS layer -- the sims/gates stay the detection
layer and always run first; reach for this when a suite hands you a
failing file or a bench report needs a sim repro.

Subcommands:
  compile FILE [-o DIR]   compile a .jal with the standard include paths
                          (hex + asm listing + symbols into a scratch dir)
  symbols FILE            print the listing's variable EQU table
  watch FILE --sym NAME [--sym NAME2] [--steps N]
                          run under gpsim with write-logs on the named
                          variables; print a PC-attributed write timeline
                          (classic cores; 32-bit targets fall back to a
                          final-RAM snapshot via the QEMU helpers)
  trace FILE --at ADDR --count N
                          disassemble an instruction window, annotated
                          with symbols and the a/d bits. The opcode table
                          is parsed from src/libpic12/pic16.c at runtime,
                          so it cannot disagree with the compiler.
  uart FILE [--secs N]   capture the TXREG byte stream to text
  build REF [--asan]      clean out-of-tree build of any git ref into a
                          cached sandbox binary (git worktree -- never a
                          contaminated scratch tree)
  pin-check WITNESS --old REF [--new REF-or-work]
                          prove a witness PINS its fix: it must PASS on
                          the new build and FAIL on the old one
  cost --old REF [--new REF] [--samples N]
                          compile-time canary: time the corpus giants on
                          both builds (catches perf regressions like the
                          13x BSR-fixpoint first cut in seconds)

Usage: python3 unit-tests/jaldbg.py <subcommand> ...
       (run from the repo root; REPO is derived from this file's path)
"""
from __future__ import annotations
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
COMPILER = REPO / "bin/jalv3-x86-64"
LIBS = f"{REPO}/demo_lib;{REPO}/third_party/jallib/lib;{REPO}/sims/shared"
BUILD_CACHE = REPO / "unit-tests/.cache/jaldbg-builds"

# device include -> (sim, processor/machine, default clock)
DEVICES = {
    "18f4620": ("gpsim", "p18f4620", 20_000_000),
    "18f26k22": ("gpsim", "p18f26k22", 16_000_000),
    "18f67j50": ("none", None, 48_000_000),
    "16f877": ("gpsim", "p16f877", 20_000_000),
    "16f1825": ("gpsim", "p16f1825", 32_000_000),
    "12f1840": ("gpsim", "p12f1840", 32_000_000),
    "32mz1024efh064": ("qemu32", "pic32mz-explorer16", 200_000_000),
    "32mx270f256b": ("qemu32", "pic32mx7-explorer16", 40_000_000),
    "esp32_xtensa_lx6": ("esp32", "esp32", 240_000_000),
}

# ---------------------------------------------------------------- compile

def do_compile(jal: Path, outdir: Path | None, compiler: Path) -> dict:
    outdir = outdir or Path(tempfile.mkdtemp(prefix="jaldbg_"))
    outdir.mkdir(parents=True, exist_ok=True)
    hexf = outdir / (jal.stem + ".hex")
    asmf = outdir / (jal.stem + ".asm")
    r = subprocess.run(
        [str(compiler), "-s", LIBS, "-no-codfile",
         "-asm", str(asmf), "-hex", str(hexf), str(jal)],
        capture_output=True, text=True, cwd=str(REPO))
    tail = [l for l in (r.stdout + r.stderr).splitlines()
            if "errors," in l or "error" in l.lower()][:3]
    if r.returncode != 0 or not hexf.is_file():
        print("\n".join(tail) or (r.stdout + r.stderr)[-800:], file=sys.stderr)
        raise SystemExit(f"compile failed: {jal}")
    print(f"compiled: {hexf}")
    print("\n".join(tail))
    return {"hex": hexf, "asm": asmf, "out": outdir}

def detect_device(jal: Path) -> str | None:
    txt = jal.read_text(errors="replace")[:4000]
    for inc in re.findall(r"^include\s+([0-9a-z_]+)", txt, re.M | re.I):
        if inc.lower() in DEVICES:
            return inc.lower()
    return None

# ---------------------------------------------------------------- symbols

def load_symbols(asm: Path) -> dict[int, str]:
    syms = {}
    for m in re.finditer(
            r"^(v_\S+)\s+EQU (0x[0-9a-fA-F]+)", asm.read_text(errors="replace"), re.M):
        syms.setdefault(int(m.group(2), 16), m.group(1))
    return syms

def cmd_symbols(args):
    out = ensure_compiled(args.jal, args.compiler)
    for addr in sorted(load_symbols(out["asm"])):
        print(f"0x{addr:04x}  {load_symbols(out['asm'])[addr]}")

def ensure_compiled(jal: Path, compiler: Path, outdir: Path | None = None) -> dict:
    if outdir:
        return do_compile(jal, outdir, compiler)
    # reuse the compiler's own output dir convention under /tmp
    return do_compile(jal, Path(tempfile.mkdtemp(prefix="jaldbg_")), compiler)

# ---------------------------------------------------------------- gpsim

def gpsim_session(hexf: Path, extra_cmds: list[str], steps: int,
                  proc: str, freq: int) -> tuple[int, str]:
    td = tempfile.mkdtemp(prefix="jaldbg_sim_")
    log = Path(td) / "log.txt"
    script = [
        f"load {hexf}", f"frequency {freq}", f"log on {log}",
        *extra_cmds,
        f"step {steps}", "log off", "quit",
    ]
    r = subprocess.run(
        ["gpsim", "-i", "-S", "disable", "-p", proc, str(hexf)],
        input="\n".join(script) + "\n", text=True,
        capture_output=True, cwd=str(REPO))
    txt = log.read_text(errors="replace") if log.is_file() else ""
    return r.returncode, txt

def cmd_watch(args):
    jal = Path(args.jal)
    out = ensure_compiled(jal, args.compiler)
    dev = detect_device(jal) or ""
    sim, proc, freq = DEVICES.get(dev, ("none", None, 20_000_000))
    syms = load_symbols(out["asm"])
    wanted = {}
    for name in args.sym:
        needle = f"v_{name}".lstrip("v_v")  # accept both raw and v_ forms
        hit = [a for a, n in syms.items() if n == name or n == f"v_{name}"]
        if not hit:
            hits = [a for a, n in syms.items() if name in n]
            if not hits:
                raise SystemExit(f"symbol not found: {name} (see `symbols`)")
            hit = hits[:1]
        wanted[hit[0]] = name if syms[hit[0]] == name else syms[hit[0]]

    if sim == "gpsim":
        cmds = [f"log w 0x{a:x}" for a in wanted]
        rc, txt = gpsim_session(out["hex"], cmds, args.steps, proc, freq)
        print(f"== write timeline ({dev}/{proc}, {args.steps} steps) ==")
        events = 0
        cur_pc = "?"
        for line in txt.splitlines():
            m = re.match(r"^0x[0-9A-Fa-f]+\s+\S+\s+(0x[0-9A-Fa-f]+)", line.strip())
            if m:
                cur_pc = m.group(1)
                continue
            m = re.search(r"Wrote:\s*(0x[0-9a-fA-F]+)\s+to REG[0-9A-F]+\((0x[0-9A-Fa-f]+)\)"
                          r"(?:\s+was\s+(0x[0-9a-fA-F]+))?", line)
            if m:
                val, addr = int(m.group(1), 16), int(m.group(2), 16)
                if addr in wanted:
                    was = f" (was {int(m.group(3), 16):#x})" if m.group(3) else ""
                    print(f"PC {cur_pc}: {wanted[addr]}(0x{addr:x}) = {val:#04x}{was}")
                    events += 1
        if not events:
            print("(no writes to watched symbols observed)")
    elif sim in ("qemu32", "esp32"):
        print(f"== {sim}: final-RAM snapshot (no write timeline on QEMU) ==")
        sys.path.insert(0, str(REPO / "sims"))
        if sim == "qemu32":
            from pic32_qemu import run_hex  # type: ignore
            ram = run_hex(str(out["hex"]), machine=proc, duration_s=args.secs)
        else:
            from esp32_qemu import run_hex as esp_run  # type: ignore
            ram = esp_run(str(out["hex"]), duration_s=args.secs, dump_bytes=0x2000)
        base = 0 if sim == "qemu32" else int(getattr(ram, "base", 0) or 0)
        for a, n in sorted(wanted.items()):
            if a < len(ram):
                print(f"{n}(0x{a:x}) = {ram[a]:#04x}")
    else:
        raise SystemExit(f"no sim target for device '{dev}' (or none detected)")

def cmd_uart(args):
    jal = Path(args.jal)
    out = ensure_compiled(jal, args.compiler)
    dev = detect_device(jal) or ""
    sim, proc, freq = DEVICES.get(dev, ("none", None, None))
    if sim == "gpsim":
        rc, txt = gpsim_session(out["hex"], ["log w txreg"], args.steps, proc, freq or 20_000_000)
        outb = [int(m.group(1), 16) & 0xFF
                for m in re.finditer(r"Wrote:\s*(0x[0-9a-fA-F]+)\s+to txreg", txt)]
        print(bytes(outb).decode("latin-1", errors="replace"))
    else:
        sys.path.insert(0, str(REPO / "sims"))
        if sim == "esp32":
            from esp32_qemu import run_hex as esp_run  # type: ignore
            ram = esp_run(str(out["hex"]), duration_s=args.secs, capture_serial=True)
            print((ram[1] if isinstance(ram, tuple) else b"").decode(
                "latin-1", errors="replace"))
        else:
            raise SystemExit(f"uart capture not supported for {dev}")

# ---------------------------------------------------------------- trace

_PIC16_C = REPO / "src/libpic12/pic16.c"

def load_opcode_table() -> list[dict]:
    """Parse the compiler's own PIC18 encoding table out of pic16.c so the
    disassembler cannot disagree with the emitter. The mask clears every
    bit the emitter ORs in (dst bit 9, access bit 8, f/literal low bits),
    so each entry matches exactly its opcode family."""
    if not _PIC16_C.is_file():
        return []
    table = []
    for m in re.finditer(
            r"case PIC_OPCODE_(\w+):\s*\n?\s*pcode_lo = 0x([0-9A-Fa-f]{4})\s*\|"
            r"((?:\s*(?:dst|access|bit|lit\d+|val|shadow)\s*\|?)*)",
            _PIC16_C.read_text(errors="replace")):
        name, base, fields = m.group(1), int(m.group(2), 16), m.group(3)
        mask = 0xFFFF
        if "dst" in fields:
            mask &= ~0x0200
        if "access" in fields:
            mask &= ~0x0100
        if "val" in fields or "bit" in fields or "lit" in fields:
            mask &= ~0x00FF
        table.append({"name": name.lower(), "base": base, "mask": mask,
                      "d": "dst" in fields, "a": "access" in fields})
    # most specific (longest mask) first so families do not shadow
    table.sort(key=lambda t: -bin(t["mask"]).count("1"))
    return table

def load_hex_words(hexf: Path) -> dict[int, int]:
    mem = {}
    hi = 0
    for line in hexf.read_text(errors="replace").splitlines():
        if not line.startswith(":"):
            continue
        b = bytes.fromhex(line[1:].strip())
        cnt, addr, typ = b[0], (b[1] << 8) | b[2], b[3]
        if typ == 0:
            for i in range(cnt):
                mem[hi + addr + i] = b[4 + i]
        elif typ == 4:
            hi = ((b[4] << 8) | b[5]) << 16
    words = {}
    for a in sorted(mem):
        if a % 2 == 0 and a + 1 in mem:
            words[a] = mem[a] | (mem[a + 1] << 8)
    return words

def cmd_trace(args):
    jal = Path(args.jal)
    out = ensure_compiled(jal, args.compiler)
    syms = load_symbols(out["asm"])
    words = load_hex_words(out["hex"])
    table = load_opcode_table()
    if not table:
        raise SystemExit("cannot parse the opcode table from src/libpic12/pic16.c")
    addr = int(args.at, 0)
    shown = 0
    a = addr & ~1
    while shown < args.count and a in words:
        w = words[a]
        txt = decode_word(w, words, table, a)
        loc = ""
        if a in syms:
            loc = f"  [{syms[a]}]"
        print(f"0x{a:04x}: {w:04x}  {txt}{loc}")
        a += 2
        shown += 1

def decode_word(w: int, words: dict, table: list, a: int) -> str:
    for t in table:
        if (w & t["mask"]) == (t["base"] & t["mask"]) and t["base"] != 0:
            f_ = w & 0xFF
            d = (w >> 9) & 1
            ab = (w >> 8) & 1
            name = t["name"]
            if name in ("movlw", "retlw"):
                return f"{name} 0x{w & 0xFF:02x}"
            if name == "movlb":
                return f"movlb 0x{w & 0xFF:02x}"
            if name in ("call", "goto", "bra"):
                if a + 2 in words:
                    w2 = words[a + 2]
                    tgt = ((w2 & 0xFFF) << 8 | (w & 0xFF)) * 2
                    return f"{name} 0x{tgt:04x}   (2-word)"
                return f"{name} ?       (2nd word missing)"
            if t["a"] and t["d"]:
                am = "banked(BSR)" if ab else "access"
                return f"{name} 0x{f_:02x} d={'f' if d else 'w'} {am}"
            if t["a"]:
                am = "banked(BSR)" if ab else "access"
                return f"{name} 0x{f_:02x} {am}"
            if t["d"]:
                return f"{name} 0x{f_:02x} d={'f' if d else 'w'}"
            return f"{name} 0x{f_:02x}"
    return f"?0x{w:04x}"

# ---------------------------------------------------------------- build

def do_build(ref: str, asan: bool) -> Path:
    key = ref.replace("/", "_") + ("-asan" if asan else "")
    binp = BUILD_CACHE / key / "jalv3-x86-64"
    if binp.is_file():
        print(f"cached: {binp}")
        return binp
    BUILD_CACHE.mkdir(parents=True, exist_ok=True)
    wt = Path(tempfile.mkdtemp(prefix=f"jaldbg_wt_{key}_"))
    r = subprocess.run(["git", "worktree", "add", "--detach", str(wt), ref],
                       cwd=str(REPO), capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"git worktree add {ref} failed:\n{r.stderr}")
    try:
        cmd = ["make", "-C", str(wt / "src"), f"-j{os.cpu_count() or 4}"]
        if asan:
            cmd.append("SAN=1")
        r2 = subprocess.run(cmd, capture_output=True, text=True)
        built = wt / "bin/jalv3-x86-64"
        if r2.returncode != 0 or not built.is_file():
            # parallel races hit this Makefile before; retry serial
            r2 = subprocess.run(["make", "-C", str(wt / "src")], capture_output=True, text=True)
        if not built.is_file():
            raise SystemExit(f"build of {ref} failed:\n"
                             f"{(r2.stdout + r2.stderr)[-1200:]}")
        dest = BUILD_CACHE / key
        dest.mkdir(parents=True, exist_ok=True)
        shutil.copy2(built, binp)
        print(f"built {ref}: {binp}")
        return binp
    finally:
        subprocess.run(["git", "worktree", "remove", "--force", str(wt)],
                       cwd=str(REPO), capture_output=True)

# ---------------------------------------------------------------- pin/cost

def run_witness(witness: Path, compiler: Path) -> tuple[int, str]:
    if witness.suffix == ".py":
        r = subprocess.run([sys.executable, str(witness), "--compiler", str(compiler)],
                           capture_output=True, text=True, cwd=str(REPO))
    else:
        r = subprocess.run(
            [str(REPO / "sims/jal_gpsim_validate.sh"), "--compiler", str(compiler),
             str(witness)], capture_output=True, text=True, cwd=str(REPO))
    return r.returncode, (r.stdout + r.stderr)[-1500:]

def cmd_pin_check(args):
    new_bin = Path(args.new) if args.new else COMPILER
    old_bin = do_build(args.old, False)
    rc_new, out_new = run_witness(Path(args.witness), new_bin)
    rc_old, out_old = run_witness(Path(args.witness), old_bin)
    print(f"new ({new_bin}): {'PASS' if rc_new == 0 else 'FAIL'}")
    print(f"old ({args.old}): {'PASS' if rc_old == 0 else 'FAIL'}")
    if rc_new != 0:
        print("\nnew output tail:\n" + out_new)
    if rc_old == 0:
        print("\nVERDICT: witness does NOT pin the fix "
              "(passes on the old build) -- same class as the j50 "
              "stale-print witness gap")
        return 1
    if rc_new != 0:
        print("\nVERDICT: witness fails on BOTH builds -- the new build "
              "is broken or the witness is not for this fix")
        return 1
    print("\nVERDICT: pinned (fails old, passes new)")
    return 0

COST_CANARIES = [
    "18f4620_network_tcp_client",
    "18f4525_mp3_decoder_vs1053b_pata_hard_disk",
    "18f4620_network_webserver_enc28j60",
]

def cmd_cost(args):
    bins = {"old": do_build(args.old, False)}
    bins["new"] = Path(args.new) if args.new else COMPILER
    for name, b in bins.items():
        t0 = time.time()
        for s in COST_CANARIES[:args.samples]:
            subprocess.run(
                [str(b), "-s", LIBS, "-no-codfile",
                 str(REPO / f"third_party/jallib/samples/{s}.jal")],
                capture_output=True, cwd=str(REPO))
        print(f"{name} ({b}): {time.time() - t0:.1f}s "
              f"for {min(args.samples, len(COST_CANARIES))} giants")

# ---------------------------------------------------------------- main

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--compiler", type=Path, default=COMPILER,
                    help="compiler binary (default: bin/jalv3-x86-64)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("compile")
    p.add_argument("jal", type=Path)
    p.add_argument("-o", "--outdir", type=Path)
    p.set_defaults(fn=lambda a: do_compile(a.jal, a.outdir, a.compiler))

    p = sub.add_parser("symbols")
    p.add_argument("jal", type=Path)
    p.set_defaults(fn=cmd_symbols)

    p = sub.add_parser("watch")
    p.add_argument("jal", type=Path)
    p.add_argument("--sym", action="append", required=True)
    p.add_argument("--steps", type=int, default=2_000_000)
    p.add_argument("--secs", type=float, default=1.0)
    p.set_defaults(fn=cmd_watch)

    p = sub.add_parser("trace")
    p.add_argument("jal", type=Path)
    p.add_argument("--at", required=True, help="start byte address (0x..)")
    p.add_argument("--count", type=int, default=20)
    p.set_defaults(fn=cmd_trace)

    p = sub.add_parser("uart")
    p.add_argument("jal", type=Path)
    p.add_argument("--steps", type=int, default=30_000_000)
    p.add_argument("--secs", type=float, default=2.0)
    p.set_defaults(fn=cmd_uart)

    p = sub.add_parser("build")
    p.add_argument("ref", help="git ref (HEAD~2, 791000a, ...)")
    p.add_argument("--asan", action="store_true")
    p.set_defaults(fn=lambda a: (do_build(a.ref, a.asan), None)[1])

    p = sub.add_parser("pin-check")
    p.add_argument("witness", type=Path)
    p.add_argument("--old", required=True, help="git ref of the pre-fix build")
    p.add_argument("--new", default=None, help="new binary or ref (default: work tree)")
    p.set_defaults(fn=cmd_pin_check)

    p = sub.add_parser("cost")
    p.add_argument("--old", required=True)
    p.add_argument("--new", default=None)
    p.add_argument("--samples", type=int, default=2)
    p.set_defaults(fn=cmd_cost)

    args = ap.parse_args()
    rc = args.fn(args)
    return 0 if rc is None else rc

if __name__ == "__main__":
    sys.exit(main())
