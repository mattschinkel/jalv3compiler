#!/usr/bin/env python3
"""
PIC32 QEMU helper — free-run a MIPS HEX, dump SRAM at phys 0.

Promoted from tmp/pic32_run.py for the Phase-4 @sim_validate harness.
Uses a unique monitor socket per process so --jobs N is safe.

Library API:
  ram = run_hex(hex_path, machine="pic32mz-explorer16", duration_s=0.5)
  # ram is a list[int] of bytes from phys 0

CLI (ad-hoc):
  python3 scripts/pic32_qemu.py [hex] [machine] [duration_s] [off=expect...]


Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""
from __future__ import annotations

import os
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

def _qemu_default() -> str:
    # PATH lookup first so a stock install works without /opt; the
    # sergev/qemu build (all 6 PIC32 machines) is the /opt fallback.
    p = shutil.which("qemu-system-mipsel")
    return p if p else "/opt/qemu-pic32/mipsel-softmmu/qemu-system-mipsel"


QEMU_DEFAULT = _qemu_default()
DEFAULT_MACHINE = "pic32mz-explorer16"
DEFAULT_DURATION = 0.5


class Monitor:
    def __init__(self, path: str) -> None:
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(50):
            if os.path.exists(path):
                break
            time.sleep(0.05)
        self.s.connect(path)
        self.buf = b""
        self._drain(0.6)

    def _drain(self, t: float) -> bytes:
        self.s.settimeout(t)
        try:
            while True:
                chunk = self.s.recv(4096)
                if not chunk:
                    break
                self.buf += chunk
        except socket.timeout:
            pass
        out = self.buf
        self.buf = b""
        return out

    def cmd(self, line: str, t: float = 0.3) -> bytes:
        self.s.sendall((line + "\n").encode())
        return self._drain(t)

    def quit(self) -> None:
        try:
            self.s.sendall(b"quit\n")
        except OSError:
            pass
        self.s.close()


def parse_xp(data: bytes) -> list[tuple[int, list[int]]]:
    """Parse qemu `xp /Nbx ADDR` lines into (addr, [bytes])."""
    res: list[tuple[int, list[int]]] = []
    for ln in data.decode(errors="replace").splitlines():
        ln = ln.strip()
        if ":" not in ln:
            continue
        if not all(c in "0123456789abcdefxABCDEFX " for c in ln.replace(":", "")):
            continue
        left, right = ln.split(":", 1)
        try:
            addr = int(left, 16)
            words = [int(w, 16) for w in right.split()]
        except ValueError:
            continue
        res.append((addr, words))
    return res


def xp_to_dump_text(ram: list[int], base: int = 0) -> str:
    """Format RAM bytes as gpsim-like `AAAA: bb bb ...` rows for sim_validate."""
    lines: list[str] = []
    i = 0
    while i < len(ram):
        chunk = ram[i : i + 16]
        hex_bytes = " ".join(f"{b:02x}" for b in chunk)
        # Pad trailing spaces so byte_at_dump-style parsers see cells.
        lines.append(f"{base + i:04x}: {hex_bytes} ")
        i += 16
    return "\n".join(lines) + ("\n" if lines else "")


def run_hex(
    hex_path: str | Path,
    machine: str = DEFAULT_MACHINE,
    duration_s: float = DEFAULT_DURATION,
    dump_bytes: int = 256,
    qemu: str | None = None,
    capture_serial: bool | None = None,
    serial_input: bytes | None = None,
) -> list[int] | tuple[list[int], bytes]:
    """
    Free-run HEX for duration_s, stop, dump dump_bytes from phys 0.
    Returns the byte list (may be shorter than dump_bytes on parse failure).

    With capture_serial=True QEMU's first serial port is captured and the
    bytes received are returned alongside the RAM dump as a tuple.

    With serial_input=<bytes> the serial backend switches from `file:` to
    `stdio` so the guest can RECEIVE bytes (the QEMU PIC32 UART model
    raises URXDA / the RX interrupt on the chardev's receive callback).
    Input is written right after `cont`; captured output has QEMU's
    machine banner stripped.  The `file:` backend stays the default for
    output-only captures (the unix-socket backend changes guest timing;
    `file:` matches silicon transcripts byte-for-byte).
    """
    hex_path = Path(hex_path)
    if not hex_path.is_file():
        raise FileNotFoundError(hex_path)
    qemu_bin = qemu or os.environ.get("PIC32_QEMU", QEMU_DEFAULT)
    if not Path(qemu_bin).is_file():
        raise FileNotFoundError(f"qemu not found: {qemu_bin}")

    sock = f"/tmp/pic32_run.{os.getpid()}.{time.time_ns()}.sock"
    if os.path.exists(sock):
        os.remove(sock)

    capture = bool(capture_serial)
    use_stdio = serial_input is not None
    ser_file = f"/tmp/pic32_uart.{os.getpid()}.{time.time_ns()}.bin"

    if use_stdio:
        args = [
            qemu_bin,
            "-M", machine,
            "-nographic",
            "-display", "none",
            "-serial", "stdio",
            "-monitor", f"unix:{sock},server,nowait",
            "-S",
            "-kernel", str(hex_path),
        ]
        proc = subprocess.Popen(
            args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        captured = bytearray()

        def _drain() -> None:
            assert proc.stdout is not None
            while True:
                chunk = proc.stdout.read(1)
                if not chunk:
                    break
                captured.extend(chunk)

        threading.Thread(target=_drain, daemon=True).start()
        try:
            # let the machine banner flush before the serial marker
            time.sleep(0.4)
            mon = Monitor(sock)
            serial_start = len(captured)
            mon.cmd("cont", 0.2)
            if serial_input:
                # The PIC32 UART model has a single-byte RX holding
                # register: it accepts the next byte only after the guest
                # has read UxRXREG.  Pace the feed so bytes are not
                # dropped by uart_receive().
                def _feed() -> None:
                    for byte in serial_input:
                        time.sleep(1.0)
                        try:
                            assert proc.stdin is not None
                            proc.stdin.write(bytes((byte,)))
                            proc.stdin.flush()
                        except (BrokenPipeError, ValueError):
                            break

                threading.Thread(target=_feed, daemon=True).start()
            time.sleep(duration_s)
            mon.cmd("stop", 0.3)
            serial_bytes = bytes(captured[serial_start:])
            # stdio carries QEMU's own banner / unsupported-register log
            # lines on stdout; drop those before treating the rest as the
            # guest's serial stream.
            noise = (b"Board:", b"Processor:", b"RAM size:",
                     b"Load file:", b"--- ")
            serial_bytes = b"\n".join(
                line for line in serial_bytes.split(b"\n")
                if not line.startswith(noise))
            need = max(16, int(dump_bytes))
            raw = mon.cmd("xp /%dbx 0" % need, 0.5)
            mon.quit()
            ram: list[int] = []
            for _a, ws in parse_xp(raw):
                ram.extend(ws)
            if capture:
                return ram, serial_bytes
            return ram
        finally:
            try:
                if proc.stdin is not None:
                    proc.stdin.close()
            except OSError:
                pass
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
            if os.path.exists(sock):
                os.remove(sock)

    # UART capture uses -serial file: -- the unix-socket backend changes
    # guest timing/execution on long programs (documented QEMU pic32
    # divergence: mid-stream stalls and diverged RAM state; the file
    # backend matches silicon transcripts byte-for-byte).
    args = [
        qemu_bin,
        "-M",
        machine,
        "-nographic",
        "-display",
        "none",
        "-serial",
        f"file:{ser_file}" if capture else "none",
        "-monitor",
        f"unix:{sock},server,nowait",
        "-S",
        "-kernel",
        str(hex_path),
    ]
    proc = subprocess.Popen(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    try:
        serial_bytes = b""
        mon = Monitor(sock)
        mon.cmd("cont", 0.2)
        time.sleep(duration_s)
        mon.cmd("stop", 0.3)
        if capture and os.path.exists(ser_file):
            with open(ser_file, "rb") as fh:
                serial_bytes = fh.read()
        need = max(16, int(dump_bytes))
        raw = mon.cmd("xp /%dbx 0" % need, 0.5)
        mon.quit()
        ram: list[int] = []
        for _a, ws in parse_xp(raw):
            ram.extend(ws)
        if capture:
            return ram, serial_bytes
        return ram
    finally:
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
        if os.path.exists(sock):
            os.remove(sock)
        if os.path.exists(ser_file):
            os.remove(ser_file)


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    hex_path = argv[0] if argv else "tmp/32mz1024efh064_call_probe.hex"
    machine = argv[1] if len(argv) > 1 else DEFAULT_MACHINE
    duration = float(argv[2]) if len(argv) > 2 else 2.0
    checks: list[tuple[int, int]] = []
    for a in argv[3:]:
        if "=" in a:
            o, e = a.split("=", 1)
            checks.append((int(o), int(e)))
    if not checks:
        checks = [(0, 8)]

    need = max(off + 1 for off, _ in checks)
    need = max(need, 16)
    print(f"=== run freely for {duration}s ===")
    ram = run_hex(hex_path, machine=machine, duration_s=duration, dump_bytes=need)
    print(f"=== RAM @ phys 0 (first {len(ram)} bytes) ===")
    print(xp_to_dump_text(ram).rstrip())
    ok = True
    for off, exp in checks:
        got = ram[off] if off < len(ram) else None
        good = got == exp
        ok = ok and good
        print(f"  RAM[{off}] = {got}  (expected {exp}) -> {'OK' if good else 'FAIL'}")
    print(f"\n=== RESULT: {'PASS' if ok else 'FAIL'} ===")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
