#!/usr/bin/env python3
"""
Parse @sim_validate blocks and validate QEMU SRAM dumps against .asm EQU symbols.

Mirrors sims/gpsim_validate_engine.py for the PIC32 / QEMU path.
See sims/sim_validate_contract.txt.

Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from typing import Any


def _read_text(path: str) -> str:
    return Path(path).read_text(encoding="utf-8", errors="replace")


def parse_validate_block(jal_text: str) -> dict[str, Any]:
    m = re.search(
        r"@sim_validate\s+begin\s*(.*?)@sim_validate\s+end",
        jal_text,
        re.I | re.S,
    )
    if not m:
        return {
            "has_block": False,
            "duration_s": None,
            "machine": None,
            "icount": None,
            "compiler_args": [],
            "uart_out": None,
            "uart_contains": None,
            "uart_input": None,
            "checks": [],
        }

    body = m.group(1)
    duration_s: float | None = None
    machine: str | None = None
    icount: int | None = None
    compiler_args: list[str] = []
    uart_out: str | None = None
    uart_contains: str | None = None
    uart_input: str | None = None
    checks: list[dict[str, Any]] = []

    for raw in body.splitlines():
        line = raw.strip()
        if not line.startswith("--"):
            continue
        line = line[2:].strip()
        if not line:
            continue
        low = line.lower()

        if low.startswith("duration_s:"):
            rest = line.split(":", 1)[1].strip()
            try:
                duration_s = float(rest)
            except ValueError:
                pass
            continue

        if low.startswith("machine:"):
            machine = line.split(":", 1)[1].strip()
            continue

        if low.startswith("icount:"):
            # ESP32-only: QEMU -icount shift override (virtual ns per
            # instruction = 2^shift).  Programs that run the 240 MHz
            # clock bring-up dilate ~10-20x at the default 3.
            try:
                icount = int(line.split(":", 1)[1].strip())
            except ValueError:
                pass
            continue

        if low.startswith("compiler_args:"):
            rest = line.split(":", 1)[1].strip()
            if rest:
                compiler_args.extend(rest.split())
            continue

        if low.startswith("uart_out:"):
            # expected literal UART output captured by the QEMU harness
            uart_out = line.split(":", 1)[1].strip()
            continue

        if low.startswith("uart_contains:"):
            # substring variant: the captured UART stream must contain this
            # text (ESP32 boots with a ROM banner on the same port, so an
            # exact match is only possible for programs that wait it out)
            uart_contains = line.split(":", 1)[1].strip()
            continue

        if low.startswith("uart_input:"):
            # bytes fed into the guest UART before/while it runs; the QEMU
            # PIC32 UART model raises URXDA/RX-IRQ from this stream
            uart_input = line.split(":", 1)[1].strip()
            continue

        if low.startswith("expect "):
            parts = line.split()
            # expect <kind> <sym> [args...]
            if len(parts) < 3:
                continue
            kind = parts[1].lower()
            sym = parts[2]
            args = parts[3:]
            checks.append({"kind": kind, "sym": sym, "args": args})
            continue

    return {
        "has_block": True,
        "duration_s": duration_s,
        "machine": machine,
        "icount": icount,
        "compiler_args": compiler_args,
        "uart_out": uart_out,
        "uart_contains": uart_contains,
        "uart_input": uart_input,
        "checks": checks,
    }


def equ_file_addr(asm_text: str, symbol: str) -> int | None:
    """First matching ^symbol EQU 0xNNNN (space after hex, same as gpsim)."""
    esc = re.escape(symbol)
    pat = re.compile(rf"^{esc}\s+EQU\s+0x([0-9A-Fa-f]+)\s", re.MULTILINE)
    mm = pat.search(asm_text)
    if not mm:
        return None
    return int(mm.group(1), 16)


def expect_sym_file_addr(asm_text: str, sym: str) -> tuple[int | None, str]:
    m = re.match(r"^(.+)\+(\d+)$", sym)
    if m:
        base = m.group(1)
        off = int(m.group(2), 10)
        a = equ_file_addr(asm_text, base)
        if a is None:
            return None, sym
        return a + off, sym
    a = equ_file_addr(asm_text, sym)
    return a, sym


def byte_at_dump(dump_text: str, addr: int) -> int | None:
    """Resolve a byte from gpsim-like `AAAA: bb bb ...` dump text."""
    for line in dump_text.splitlines():
        line = line.strip()
        # Do not require a trailing space after the last cell: strip() would
        # remove it and drop offsets 0xNNf (end of each 16-byte row).
        m = re.match(
            # 4 digits: PIC32 (dump relative to phys 0); 4..8: ESP32
            # (absolute DRAM addresses, e.g. 3ffb1000, from esp32_qemu.py).
            r"^([0-9a-fA-F]{4,8}):\s*(.*)$",
            line,
        )
        if not m:
            continue
        base = int(m.group(1), 16)
        cells = m.group(2).split()
        if not cells:
            continue
        if base <= addr < base + len(cells):
            off = addr - base
            cell = cells[off]
            if cell == "--":
                return None
            if not re.fullmatch(r"[0-9a-fA-F]{2}", cell):
                return None
            return int(cell, 16)
    return None


def parse_int_token(s: str) -> int:
    s = s.strip().lower()
    if s.startswith("0x"):
        return int(s, 16)
    return int(s, 10)


def run_check(
    kind: str,
    sym: str,
    args: list[str],
    asm_text: str,
    dump_text: str,
) -> tuple[bool, str]:
    addr, sym_disp = expect_sym_file_addr(asm_text, sym)
    if addr is None:
        return False, f"symbol {sym!r} not found in .asm (EQU)"

    if kind == "bit_clear":
        bit = int(args[0]) if args else 0
        b = byte_at_dump(dump_text, addr)
        if b is None:
            return False, f"no dump row for file 0x{addr:03x} ({sym_disp})"
        if (b >> bit) & 1:
            return False, f"{sym_disp} @0x{addr:03x}: byte 0x{b:02x} bit{bit} set (expected clear)"
        return True, f"{sym_disp} @0x{addr:03x}: bit{bit} clear"

    if kind == "bit_set":
        bit = int(args[0]) if args else 0
        b = byte_at_dump(dump_text, addr)
        if b is None:
            return False, f"no dump row for file 0x{addr:03x} ({sym_disp})"
        if not ((b >> bit) & 1):
            return False, f"{sym_disp} @0x{addr:03x}: byte 0x{b:02x} bit{bit} clear (expected set)"
        return True, f"{sym_disp} @0x{addr:03x}: bit{bit} set"

    if kind == "byte":
        if not args:
            return False, f"byte: missing value for {sym_disp}"
        exp = parse_int_token(args[0])
        b = byte_at_dump(dump_text, addr)
        if b is None:
            return False, f"no dump row for file 0x{addr:03x} ({sym_disp})"
        if b != exp:
            return False, f"{sym_disp} @0x{addr:03x}: got 0x{b:02x} expected 0x{exp:02x}"
        return True, f"{sym_disp} @0x{addr:03x}: byte 0x{b:02x}"

    if kind == "word_le":
        if not args:
            return False, f"word_le: missing value for {sym}"
        exp = parse_int_token(args[0])
        lo = byte_at_dump(dump_text, addr)
        hi = byte_at_dump(dump_text, addr + 1)
        if lo is None or hi is None:
            return False, f"no dump for word at 0x{addr:03x} ({sym_disp})"
        val = lo | (hi << 8)
        if val != exp:
            return False, f"{sym_disp} @0x{addr:03x}: word_le {val} expected {exp}"
        return True, f"{sym_disp} @0x{addr:03x}: word_le {val}"

    if kind == "dword_le":
        if not args:
            return False, f"dword_le: missing value for {sym}"
        exp = parse_int_token(args[0])
        bs = [byte_at_dump(dump_text, addr + i) for i in range(4)]
        if None in bs:
            return False, f"no dump for dword at 0x{addr:03x} ({sym_disp})"
        val = bs[0] | (bs[1] << 8) | (bs[2] << 16) | (bs[3] << 24)
        if val != exp:
            return False, f"{sym_disp} @0x{addr:03x}: dword_le {val} expected {exp}"
        return True, f"{sym_disp} @0x{addr:03x}: dword_le {val}"

    return False, f"unknown expect kind {kind!r}"


def cmd_check_contract(jal_path: str) -> int:
    text = _read_text(jal_path)
    meta = parse_validate_block(text)
    if not meta["has_block"]:
        print(
            f"error: {jal_path}: missing @sim_validate begin…end block",
            file=sys.stderr,
        )
        return 1
    if not meta["checks"]:
        print(
            f"error: {jal_path}: @sim_validate block has no expect lines",
            file=sys.stderr,
        )
        return 1
    return 0


def cmd_check_asm_symbols(jal_path: str, asm_path: str) -> int:
    meta = parse_validate_block(_read_text(jal_path))
    asm_text = _read_text(asm_path)
    missing: list[str] = []
    for chk in meta["checks"]:
        sym = chk["sym"]
        base = re.sub(r"\+\d+$", "", sym)
        if equ_file_addr(asm_text, base) is None:
            missing.append(sym)
    if missing:
        uniq = ", ".join(sorted(set(missing)))
        print(
            f"error: expect symbol(s) missing from .asm (no EQU): {uniq}",
            file=sys.stderr,
        )
        return 1
    return 0


def cmd_meta(jal_path: str) -> int:
    meta = parse_validate_block(_read_text(jal_path))
    out = {
        "has_block": meta["has_block"],
        "duration_s": meta["duration_s"] if meta["duration_s"] is not None else 0.5,
        "machine": meta["machine"] or "pic32mz-explorer16",
        "icount": meta["icount"],
        "compiler_args": meta["compiler_args"],
        "n_checks": len(meta["checks"]),
    }
    print(json.dumps(out))
    return 0


def cmd_validate(
    jal_path: str, asm_path: str, dump_path: str, uart_out_path: str | None = None
) -> int:
    meta = parse_validate_block(_read_text(jal_path))
    asm_text = _read_text(asm_path)
    dump_text = _read_text(dump_path)
    ok = True
    for chk in meta["checks"]:
        good, msg = run_check(
            chk["kind"], chk["sym"], chk["args"], asm_text, dump_text
        )
        print(("OK  " if good else "FAIL") + " " + msg)
        ok = ok and good
    if (meta.get("uart_out") is not None) or (meta.get("uart_contains") is not None):
        got = ""
        if uart_out_path:
            p = Path(uart_out_path)
            if p.is_file():
                got = p.read_text(
                    encoding="utf-8", errors="replace"
                ).strip()
    if meta.get("uart_out") is not None:
        good = got == meta["uart_out"]
        print(
            ("OK  " if good else "FAIL")
            + f" uart_out {meta['uart_out']!r}"
            + ("" if good else f" got {got!r}")
        )
        ok = ok and good
    if meta.get("uart_contains") is not None:
        good = meta["uart_contains"] in got
        print(
            ("OK  " if good else "FAIL")
            + f" uart_contains {meta['uart_contains']!r}"
            + ("" if good else f" got {got!r}")
        )
        ok = ok and good
    if not ok:
        print("RESULT: FAIL", file=sys.stderr)
        return 1
    print("RESULT: PASS")
    return 0


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv:
        print(
            "usage: sim_validate_engine.py "
            "check-contract|check-asm-symbols|meta|validate …",
            file=sys.stderr,
        )
        return 2
    cmd = argv[0]
    if cmd == "check-contract" and len(argv) >= 2:
        return cmd_check_contract(argv[1])
    if cmd == "check-asm-symbols" and len(argv) >= 3:
        return cmd_check_asm_symbols(argv[1], argv[2])
    if cmd == "meta" and len(argv) >= 2:
        return cmd_meta(argv[1])
    if cmd == "validate" and len(argv) >= 4:
        return cmd_validate(argv[1], argv[2], argv[3], argv[4] if len(argv) > 4 else None)
    print(f"error: bad args for {cmd!r}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
