#!/usr/bin/env python3
"""
Parse @gpsim_validate blocks from .jal and validate gpsim `dump r` against .asm EQU symbols.
Resolves @config_expect / `pragma target clock` across local `include` siblings (see gather_jal_with_local_includes).
Used by sims/jal_gpsim_validate.sh — see sims/gpsim_validate_contract.txt

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


def gather_jal_with_local_includes(jal_path: str) -> str:
    """
    Concatenate the main .jal and any `include name` targets that resolve as
    sibling files next to the including file (same directory as that .jal).

    Used so @config_expect and `pragma target clock` can live in a shared
    sims/shared/regression_<device>_device.jal (PIC18F4620: regression_18f4620_device.jal;
    other MCUs use a different file) while check-config / meta still see them.
    Library includes (e.g. 18f4620 from demo_lib/) are skipped when not present locally.
    """
    root = Path(jal_path).resolve()

    def _walk(path: Path, seen: set[Path]) -> str:
        path = path.resolve()
        if path in seen or not path.is_file():
            return ""
        seen.add(path)
        text = path.read_text(encoding="utf-8", errors="replace")
        parts: list[str] = [text]
        here = path.parent
        for raw_line in text.splitlines():
            line = raw_line.split("--", 1)[0].strip()
            m = re.match(r"(?i)^include\s+(\w+)", line)
            if not m:
                continue
            child = here / f"{m.group(1).lower()}.jal"
            if child.is_file():
                parts.append(_walk(child, seen))
        return "\n".join(parts)

    return _walk(root, set())


def infer_clock_hz(jal_text: str) -> int | None:
    for line in jal_text.splitlines():
        line = line.split("--", 1)[0]
        if re.match(r"^\s*pragma\s+target\s+clock\s+", line, re.I):
            parts = line.split()
            if parts:
                raw = parts[-1].replace("_", "")
                if raw.isdigit():
                    return int(raw)
    return None


def parse_validate_block(jal_text: str) -> dict[str, Any]:
    """Return meta: has_block, steps (int|None), frequency_hz (int|'auto'|None), checks (list)."""
    m = re.search(
        r"@gpsim_validate\s+begin\s*(.*?)@gpsim_validate\s+end",
        jal_text,
        re.I | re.S,
    )
    if not m:
        return {
            "has_block": False,
            "steps": None,
            "frequency_hz": None,
            "compiler_args": [],
            "checks": [],
            "skip_gpsim": False,
            "allow_compiler_warnings": False,
            "expect_hw_stkptr_le_compiler_depth": False,
            "hw_stkptr_steps": None,
        }

    body = m.group(1)
    steps: int | None = None
    frequency_hz: int | None | str = None
    compiler_args: list[str] = []
    checks: list[dict[str, Any]] = []
    skip_gpsim = False
    allow_compiler_warnings = False
    expect_hw_stkptr_le_compiler_depth = False
    hw_stkptr_steps: int | None = None

    for raw in body.splitlines():
        line = raw.strip()
        if not line.startswith("--"):
            continue
        line = line[2:].strip()
        if not line:
            continue
        low = line.lower()

        if low.startswith("steps:"):
            rest = line.split(":", 1)[1].strip()
            if rest.isdigit():
                steps = int(rest)
            continue

        if low.startswith("frequency_hz:"):
            rest = line.split(":", 1)[1].strip().lower()
            if rest == "auto":
                frequency_hz = "auto"
            elif rest.isdigit():
                frequency_hz = int(rest)
            continue

        if low.startswith("compiler_args:"):
            rest = line.split(":", 1)[1].strip()
            if rest:
                compiler_args.extend(rest.split())
            continue

        if low.startswith("skip_gpsim:"):
            rest = line.split(":", 1)[1].strip().lower()
            skip_gpsim = rest in ("1", "true", "yes", "on")
            continue

        if low.startswith("allow_compiler_warnings:"):
            rest = line.split(":", 1)[1].strip().lower()
            allow_compiler_warnings = rest in ("1", "true", "yes", "on")
            continue

        if low.startswith("expect_hw_stkptr_le_compiler_depth:"):
            rest = line.split(":", 1)[1].strip().lower()
            expect_hw_stkptr_le_compiler_depth = rest in ("1", "true", "yes", "on")
            continue

        if low.startswith("hw_stkptr_steps:"):
            rest = line.split(":", 1)[1].strip()
            if rest.isdigit():
                hw_stkptr_steps = int(rest)
            continue

        if low.startswith("expect "):
            parts = line.split()
            # expect <kind> <sym> <args...>
            if len(parts) < 3:
                continue
            kind = parts[1].lower()
            sym = parts[2]
            args = parts[3:]
            checks.append({"kind": kind, "sym": sym, "args": args})
            continue

    return {
        "has_block": True,
        "steps": steps,
        "frequency_hz": frequency_hz,
        "compiler_args": compiler_args,
        "checks": checks,
        "skip_gpsim": skip_gpsim,
        "allow_compiler_warnings": allow_compiler_warnings,
        "expect_hw_stkptr_le_compiler_depth": expect_hw_stkptr_le_compiler_depth,
        "hw_stkptr_steps": hw_stkptr_steps,
    }


def extract_compiler_hardware_stack_depth(compiler_log: str) -> int | None:
    """
    Parse ``Hardware stack depth X of Y`` from jal compiler stderr/stdout.
    Returns the reported depth X (integrated static bound), or None if absent.
    """
    m = re.search(
        r"(?i)hardware\s+stack\s+depth\s+(\d+)\s+of\s+(\d+)",
        compiler_log,
    )
    if not m:
        return None
    return int(m.group(1))


def parse_config_expect_block(jal_text: str) -> dict[int, int] | None:
    """
    Parse -- @config_expect begin … end in .jal.
    Each line inside must be: -- 0xADDRESS 0xVALUE (two hex literals).
    For PIC18, VALUE is one configuration byte per __config line.
    For PIC14 enhanced, VALUE is the full config word (e.g. 0x3FFF) at word address 0x8007+.
    Returns None if the block is absent, {} if present but no valid lines (caller treats as error).
    """
    m = re.search(
        r"@config_expect\s+begin\s*(.*?)@config_expect\s+end",
        jal_text,
        re.I | re.S,
    )
    if not m:
        return None
    body = m.group(1)
    line_pat = re.compile(
        r"^\s*--\s*(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s*$",
        re.MULTILINE,
    )
    out: dict[int, int] = {}
    for mm in line_pat.finditer(body):
        addr = int(mm.group(1), 16)
        val = int(mm.group(2), 16)
        out[addr] = val
    return out


def extract_asm_config_words(asm_text: str) -> dict[int, int]:
    """Map config address -> byte from MPASM-style __config ADDR, VAL in .asm."""
    pat = re.compile(
        r"__config\s+(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)",
        re.I,
    )
    out: dict[int, int] = {}
    for mm in pat.finditer(asm_text):
        addr = int(mm.group(1), 16)
        val = int(mm.group(2), 16)
        out[addr] = val
    return out


def parse_intel_hex_memory(hex_text: str) -> dict[int, int]:
    """
    Decode Intel HEX into a linear address -> byte map (32-bit addresses).
    Handles record types 00 (data), 01 (EOF), 04 (extended linear address).
    Verifies per-line checksums (sum of all record bytes mod 256 == 0).
    """
    linear_hi = 0
    mem: dict[int, int] = {}
    for raw in hex_text.splitlines():
        line = raw.strip()
        if not line.startswith(":") or len(line) < 11:
            continue
        try:
            rec = bytes.fromhex(line[1:])
        except ValueError as e:
            raise ValueError(f"Intel HEX parse error: {line[:40]}... ({e})") from e
        if sum(rec) & 0xFF != 0:
            raise ValueError(f"Intel HEX checksum error: {line[:40]}...")
        n = rec[0]
        addr_lo = (rec[1] << 8) | rec[2]
        rtype = rec[3]
        data = rec[4 : 4 + n]
        if len(data) != n:
            raise ValueError(f"Intel HEX length mismatch: {line[:40]}...")

        if rtype == 0:
            base = (linear_hi << 16) | addr_lo
            for i, b in enumerate(data):
                mem[base + i] = b
        elif rtype == 4:
            if n != 2:
                raise ValueError(f"Intel HEX type 04 length {n}, expected 2")
            linear_hi = int.from_bytes(data, "big")
        elif rtype == 1:
            break
        elif rtype == 3:
            continue
        else:
            continue
    return mem


def equ_file_addr(asm_text: str, symbol: str) -> int | None:
    """First matching ^symbol EQU 0xNNNN (file / access-bank style address)."""
    esc = re.escape(symbol)
    pat = re.compile(rf"^{esc}\s+EQU\s+0x([0-9A-Fa-f]+)\s", re.MULTILINE)
    mm = pat.search(asm_text)
    if not mm:
        return None
    return int(mm.group(1), 16)


def expect_sym_file_addr(asm_text: str, sym: str) -> tuple[int | None, str]:
    """
    Resolve ``v__sym`` or ``v__sym+N`` (byte offset N) to a file address using
    the base symbol's EQU from .asm.
    """
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


def check_file_addr_ne(asm_text: str, sym: str, args: list[str]) -> tuple[bool, str]:
    """Assembler-only: sym+offset must not alias sym2 (see @gpsim_validate file_addr_ne)."""
    addr = equ_file_addr(asm_text, sym)
    if addr is None:
        return False, f"symbol {sym!r} not found in .asm (EQU)"
    if len(args) < 2:
        return False, "file_addr_ne: need <byte_offset> <other_asm_symbol>"
    try:
        off = int(args[0], 0)
    except ValueError:
        return False, f"file_addr_ne: bad offset {args[0]!r}"
    sym2 = args[1]
    addr2 = equ_file_addr(asm_text, sym2)
    if addr2 is None:
        return False, f"symbol {sym2!r} not found in .asm (EQU)"
    addr_a = addr + off
    if addr_a == addr2:
        return (
            False,
            f"file_addr_ne: {sym}+{off} @0x{addr_a:03x} aliases {sym2} (same EQU address)",
        )
    return (
        True,
        f"file_addr_ne: {sym}+{off} @0x{addr_a:03x} != {sym2} @0x{addr2:03x}",
    )


def byte_at_dump(dump_text: str, addr: int) -> int | None:
    """Resolve a linear file byte from gpsim `dump r` text.

    Rows may mix hex bytes with `--` (unmapped in the simulator). Any `-- cell
    for the requested address returns None. Lines need not be exactly 16 cells
    if gpsim omits trailing spaces.
    """
    for line in dump_text.splitlines():
        line = line.strip()
        m = re.match(
            r"^([0-9a-fA-F]{4}):\s*((?:(?:[0-9a-fA-F]{2}|--)\s+){1,16})",
            line,
        )
        if not m:
            continue
        base = int(m.group(1), 16)
        if base <= addr < base + 16:
            cells = m.group(2).split()
            off = addr - base
            if off >= len(cells):
                return None
            cell = cells[off]
            if cell == "--":
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


def cmd_check_config(jal_path: str, asm_path: str, hex_path: str | None = None) -> int:
    """
    If .jal (or a locally included fragment) contains @config_expect begin…end,
    require .asm __config words to match exactly; optionally the same bytes in Intel HEX
    (PIC18 config at 0x0030_0000+ from extended-linear-address records).
    """
    merged = gather_jal_with_local_includes(jal_path)
    expected = parse_config_expect_block(merged)
    if expected is None:
        return 0
    if not expected:
        print(
            "error: @config_expect block has no `-- 0xADDR 0xVAL` lines",
            file=sys.stderr,
        )
        return 1
    asm_text = _read_text(asm_path)
    got = extract_asm_config_words(asm_text)
    errs: list[str] = []
    for addr in sorted(set(expected) | set(got)):
        e = expected.get(addr)
        g = got.get(addr)
        if e is None:
            errs.append(f"  extra __config in .asm: {addr:#010x} = {g:#04x}")
        elif g is None:
            errs.append(f"  missing __config in .asm: {addr:#010x} (expected {e:#04x})")
        elif e != g:
            errs.append(
                f"  {addr:#010x}: .asm has {g:#04x}, @config_expect wants {e:#04x}",
            )
    if errs:
        print("error: @config_expect mismatch vs .asm __config lines:", file=sys.stderr)
        print("\n".join(errs), file=sys.stderr)
        return 1

    if hex_path and Path(hex_path).is_file():
        try:
            mem = parse_intel_hex_memory(_read_text(hex_path))
        except ValueError as ex:
            print(f"error: Intel HEX: {ex}", file=sys.stderr)
            return 1
        herrs: list[str] = []
        for addr in sorted(expected):
            exp = expected[addr]
            # PIC14 enhanced: __config word address 0x8000+ appears in Intel HEX as two bytes
            # at linear 0x10000 + 2*(addr - 0x8000) (INHX32, segment 0x0001).
            if 0x8000 <= addr < 0xC000:
                base_lin = 0x10000 + 2 * (addr - 0x8000)
                lo = mem.get(base_lin)
                hi = mem.get(base_lin + 1)
                if lo is None or hi is None:
                    herrs.append(
                        f"  missing PIC14 config word in .hex: logical {addr:#06x} "
                        f"(linear {base_lin:#06x}..{base_lin + 1:#06x})",
                    )
                    continue
                got_w = lo | (hi << 8)
                exp_w = exp & 0xFFFF
                if (got_w & 0x3FFF) != (exp_w & 0x3FFF):
                    herrs.append(
                        f"  PIC14 {addr:#06x}: .hex word 0x{got_w & 0x3FFF:04x}, "
                        f"@config_expect 0x{exp_w & 0x3FFF:04x}",
                    )
            else:
                hb = mem.get(addr)
                if hb is None:
                    herrs.append(f"  missing in .hex: {addr:#010x} (expected {exp:#04x})")
                elif hb != exp:
                    herrs.append(
                        f"  {addr:#010x}: .hex has {hb:#04x}, @config_expect wants {exp:#04x}",
                    )
        if herrs:
            print("error: @config_expect mismatch vs Intel HEX:", file=sys.stderr)
            print("\n".join(herrs), file=sys.stderr)
            return 1
        print(
            f"OK: __config: {len(expected)} location(s) match @config_expect in .asm and .hex",
        )
        return 0

    print(f"OK: __config: {len(expected)} word(s) match @config_expect (.asm)")
    return 0


def cmd_check_asm_symbols(jal_path: str, asm_path: str) -> int:
    """Exit 0 if every expect symbol in the @gpsim_validate block has an EQU in .asm."""
    jal_text = _read_text(jal_path)
    asm_text = _read_text(asm_path)
    meta = parse_validate_block(jal_text)
    if not meta["checks"]:
        return 0
    missing: list[str] = []
    for c in meta["checks"]:
        sym = c.get("sym")
        if not sym:
            continue
        base_sym = sym.rsplit("+", 1)[0] if "+" in sym else sym
        if equ_file_addr(asm_text, base_sym) is None:
            missing.append(sym)
        if c.get("kind") == "file_addr_ne":
            args = c.get("args") or []
            if len(args) >= 2 and equ_file_addr(asm_text, args[1]) is None:
                missing.append(args[1])
    if missing:
        uniq = ", ".join(sorted(set(missing)))
        print(
            f"error: expect symbol(s) missing from .asm (no EQU): {uniq}",
            file=sys.stderr,
        )
        return 1
    for c in meta["checks"]:
        if c.get("kind") != "file_addr_ne":
            continue
        sym = c.get("sym") or ""
        args = c.get("args") or []
        ok, msg = check_file_addr_ne(asm_text, sym, args)
        if not ok:
            print(f"error: {msg}", file=sys.stderr)
            return 1
        print(f"OK: {msg}")
    return 0


def cmd_check_contract(jal_path: str) -> int:
    """Exit 0 if @gpsim_validate exists with at least one expect line; else 1 + stderr."""
    meta = parse_validate_block(_read_text(jal_path))
    if not meta["has_block"]:
        print(
            "error: missing @gpsim_validate block in .jal — add:\n"
            "  -- @gpsim_validate begin\n"
            "  ... steps: / frequency_hz: / expect ...\n"
            "  -- @gpsim_validate end\n"
            "See sims/gpsim_validate_contract.txt",
            file=sys.stderr,
        )
        return 1
    if not meta["checks"]:
        print(
            "error: @gpsim_validate block has no expect lines — add at least one, e.g.\n"
            "  -- expect bit_clear v__bitbucket 0\n"
            "See sims/gpsim_validate_contract.txt",
            file=sys.stderr,
        )
        return 1
    return 0


def cmd_meta(jal_path: str) -> None:
    text = _read_text(jal_path)
    meta = parse_validate_block(text)
    fh = meta["frequency_hz"]
    if fh == "auto" or fh is None:
        merged = gather_jal_with_local_includes(jal_path)
        meta["frequency_hz_resolved"] = infer_clock_hz(merged)
    else:
        meta["frequency_hz_resolved"] = fh
    print(json.dumps(meta, separators=(",", ":")))


def cmd_validate(
    jal_path: str,
    asm_path: str,
    dump_path: str,
    expect_dword_cli: str,
) -> int:
    jal_text = _read_text(jal_path)
    asm_text = _read_text(asm_path)
    dump_text = _read_text(dump_path)

    meta = parse_validate_block(jal_text)
    if not meta["has_block"] or not meta["checks"]:
        print(
            "error: cannot validate — missing @gpsim_validate block or no expect lines "
            "(run: python3 sims/gpsim_validate_engine.py check-contract <file.jal>)",
            file=sys.stderr,
        )
        return 1

    checks = [
        {**c}
        for c in meta["checks"]
        if c.get("kind") != "file_addr_ne"
    ]
    if expect_dword_cli:
        for i, c in enumerate(checks):
            if c["kind"] == "dword_le" and c["sym"] == "v__d" and c.get("args"):
                checks[i] = {**c, "args": [expect_dword_cli]}

    ok_all = True
    for c in checks:
        kind = c["kind"]
        sym = c["sym"]
        args = c.get("args") or []
        ok, msg = run_check(kind, sym, args, asm_text, dump_text)
        print(("OK: " if ok else "FAIL: ") + msg)
        if not ok:
            ok_all = False
            # regression_checkpoint() (sims/shared/regression.jal) only counts
            # CLEAN checkpoints, so _check_step == expect-1 is almost always a
            # failed guard in the last checkpoint (often a wrong expect), not a
            # stalled program — the two otherwise look identical in a RAM dump.
            if sym == "v__check_step" and kind == "byte":
                m = re.search(r"got (0x[0-9a-fA-F]+|\d+) expected (0x[0-9a-fA-F]+|\d+)", msg)
                if m:
                    try:
                        got = int(m.group(1), 0)
                        exp = int(m.group(2), 0)
                    except ValueError:
                        got = exp = None
                    if got is not None and exp is not None and got < exp:
                        if got == exp - 1:
                            why = ("usually a failed guard in the LAST checkpoint — often just a wrong"
                                   " expect value")
                        else:
                            why = ("a guard failed at checkpoint %d (check its expect values)" % (got + 1))
                        print(
                            "note: _check_step short of expect: %s; regression_checkpoint only "
                            "counts clean checkpoints, so this is not by itself a stall. Add "
                            "`expect byte v__gpsim_regression_flags 0` to distinguish: bit1 set = "
                            "failed check, bit0 only (_bad, cleared just at the end) = never "
                            "reached the end (genuine stall/reset)." % why
                        )

    if ok_all:
        print("PASS: RAM validation (@gpsim_validate)")
        return 0
    return 1


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: gpsim_validate_engine.py meta <file.jal>", file=sys.stderr)
        print(
            "       gpsim_validate_engine.py check-contract <file.jal>",
            file=sys.stderr,
        )
        print(
            "       gpsim_validate_engine.py check-asm-symbols <file.jal> <file.asm>",
            file=sys.stderr,
        )
        print(
            "       gpsim_validate_engine.py check-config <file.jal> <file.asm> [<file.hex>]",
            file=sys.stderr,
        )
        print(
            "       gpsim_validate_engine.py validate <file.jal> <file.asm> <dump.txt> "
            "[expect_dword_override]",
            file=sys.stderr,
        )
        print(
            "       gpsim_validate_engine.py compiler-hw-depth <stdin: compiler log>",
            file=sys.stderr,
        )
        return 2
    cmd = sys.argv[1]
    if cmd == "check-config":
        if len(sys.argv) < 4:
            print(
                "usage: gpsim_validate_engine.py check-config <file.jal> <file.asm> [<file.hex>]",
                file=sys.stderr,
            )
            return 2
        hex_opt = sys.argv[4] if len(sys.argv) > 4 else None
        return cmd_check_config(sys.argv[2], sys.argv[3], hex_opt)
    if cmd == "check-asm-symbols":
        if len(sys.argv) < 4:
            print(
                "usage: gpsim_validate_engine.py check-asm-symbols <file.jal> <file.asm>",
                file=sys.stderr,
            )
            return 2
        return cmd_check_asm_symbols(sys.argv[2], sys.argv[3])
    if cmd == "check-contract":
        if len(sys.argv) < 3:
            print("usage: gpsim_validate_engine.py check-contract <file.jal>", file=sys.stderr)
            return 2
        return cmd_check_contract(sys.argv[2])
    if cmd == "meta":
        cmd_meta(sys.argv[2])
        return 0
    if cmd == "compiler-hw-depth":
        log = sys.stdin.read()
        d = extract_compiler_hardware_stack_depth(log)
        if d is None:
            print(
                "error: no 'Hardware stack depth X of Y' line in compiler output",
                file=sys.stderr,
            )
            return 1
        print(d)
        return 0
    if cmd == "validate":
        jal, asm, dump = sys.argv[2], sys.argv[3], sys.argv[4]
        ed = sys.argv[5] if len(sys.argv) > 5 else ""
        return cmd_validate(jal, asm, dump, ed)
    print("unknown command", cmd, file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
