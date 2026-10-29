#!/usr/bin/env python3
"""
ESP32 QEMU helper — free-run an ESP boot image, dump DRAM at ESP32_RAM_BASE.

ESP32 twin of scripts/pic32_qemu.py for the @sim_validate harness
(sims/jal_esp32_validate.sh + run_esp32_regression.py).  Mirrors
runpark.py's launch contract: espressif qemu, -icount 3, WDT disabled via
the timg global (belt+braces beside the startup WDT-off code), MTD flash
drive built in a per-process temp dir so --jobs N is safe.

Library API:
  ram = run_hex(hex_path, machine="esp32", duration_s=1.0, dump_bytes=1024)
  # ram is a list[int] of DRAM bytes starting at ESP32_RAM_BASE (0x3FFB1000)


Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""
from __future__ import annotations

import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from esp32_hex2flashbin import FLASH_SIZE, hex_to_flash  # noqa: E402

QEMU_DEFAULT = "/opt/qemu-esp32/qemu/bin/qemu-system-xtensa"
QEMU_LIB = "/opt/qemu-esp32/qemu/lib/x86_64-linux-gnu"
DEFAULT_MACHINE = "esp32"
# Under -icount 3 the ESP32 ROM boot itself takes 1.5-3 s of wall time
# before user code runs (measured 2026-09-15); anything shorter dumps a
# still-zero DRAM.
DEFAULT_DURATION = 3.0
RAM_BASE = 0x3FFB1000  # src/libesp32/esp.h ESP32_RAM_BASE


class Monitor:
    def __init__(self, path: str) -> None:
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        # The socket file appears before QEMU listens on it, and a busy
        # host can delay that further -- retry the connect, not just the
        # file-existence wait (a bare connect raced FileNotFoundError /
        # ConnectionRefusedError under load).
        deadline = time.time() + 5.0
        while True:
            try:
                self.s.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.time() >= deadline:
                    raise
                time.sleep(0.05)
        self.buf = b""
        self._drain(0.6)

    def _drain(self, t: float) -> bytes:
        self.s.settimeout(t)
        try:
            while True:
                chunk = self.s.recv(65536)
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


def xp_to_dump_text(ram: list[int], base: int = RAM_BASE) -> str:
    """Format RAM bytes as gpsim-like `AAAA: bb bb ...` rows for sim_validate.

    Addresses are the absolute DRAM addresses the ESP32 `v_` EQU dump
    emits (sim_validate_engine byte_at_dump accepts 4..8 hex digits)."""
    lines: list[str] = []
    i = 0
    while i < len(ram):
        chunk = ram[i : i + 16]
        hex_bytes = " ".join(f"{b:02x}" for b in chunk)
        lines.append(f"{base + i:08x}: {hex_bytes} ")
        i += 16
    return "\n".join(lines) + ("\n" if lines else "")


def run_hex(
    hex_path: str | Path,
    machine: str = DEFAULT_MACHINE,
    duration_s: float = DEFAULT_DURATION,
    dump_bytes: int = 1024,
    qemu: str | None = None,
    capture_serial: bool | None = None,
    icount_shift: int = 3,
) -> list[int] | tuple[list[int], bytes]:
    """
    Convert HEX to a flash image, free-run duration_s (wall), stop, dump
    dump_bytes from ESP32_RAM_BASE.  Returns the byte list.

    With capture_serial=True, QEMU's first serial port (UART0) is written
    to a file backend and the bytes received are returned alongside the
    RAM dump as a tuple.

    icount_shift is QEMU's -icount shift: virtual ns per instruction is
    2^shift.  The default 3 matches the suite bodies.  Programs that run
    the 240 MHz clock bring-up (esp32_clock_init) dilate ~10-20x under
    icount -- a higher shift collapses their wall time (a @sim_validate
    `icount:` key overrides this per file).
    """
    hex_path = Path(hex_path)
    if not hex_path.is_file():
        raise FileNotFoundError(hex_path)
    qemu_bin = qemu or os.environ.get("ESP32_QEMU", QEMU_DEFAULT)
    if not Path(qemu_bin).is_file():
        raise FileNotFoundError(f"qemu not found: {qemu_bin}")

    tag = f"{os.getpid()}.{time.time_ns()}"
    sock = f"/tmp/esp32_run.{tag}.sock"
    capture = bool(capture_serial)
    # UART capture uses -serial file: -- the unix-socket backend stalls
    # mid-stream on long programs (same documented QEMU behaviour as the
    # pic32 machine; the file backend matches the silicon transcripts).
    ser_file = f"/tmp/esp32_uart.{tag}.bin"

    tmpdir = tempfile.mkdtemp(prefix="esp32_sim_")
    flash_path = os.path.join(tmpdir, "flash.bin")
    flash = bytearray(FLASH_SIZE)
    hex_to_flash(str(hex_path), flash)
    with open(flash_path, "wb") as f:
        f.write(flash)

    env = dict(os.environ, LD_LIBRARY_PATH=QEMU_LIB)
    args = [
        qemu_bin,
        "-M",
        machine,
        "-display",
        "none",
        "-icount",
        str(icount_shift),
        "-global",
        "driver=timer.esp32.timg,property=wdt_disable,value=true",
        "-serial",
        f"file:{ser_file}" if capture else "none",
        "-monitor",
        f"unix:{sock},server=on,wait=off",
        "-S",
        "-drive",
        f"file={flash_path},if=mtd,format=raw",
    ]
    proc = subprocess.Popen(
        args,
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        serial_bytes = b""
        mon = Monitor(sock)
        mon.cmd("cont", 0.1)
        time.sleep(duration_s)
        mon.cmd("stop", 0.3)
        if capture and os.path.exists(ser_file):
            with open(ser_file, "rb") as fh:
                serial_bytes = fh.read()
        need = max(16, int(dump_bytes))
        raw = mon.cmd("xp /%dbx %#x" % (need, RAM_BASE), 1.0)
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
        for p in (sock, ser_file):
            if os.path.exists(p):
                os.remove(p)
        try:
            os.remove(flash_path)
            os.rmdir(tmpdir)
        except OSError:
            pass


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    hex_path = argv[0] if argv else None
    machine = argv[1] if len(argv) > 1 else DEFAULT_MACHINE
    duration = float(argv[2]) if len(argv) > 2 else DEFAULT_DURATION
    checks: list[tuple[int, int]] = []
    for a in argv[3:]:
        if "=" in a:
            o, e = a.split("=", 1)
            checks.append((int(o, 0), int(e, 0)))
    if not checks:
        checks = [(0, 8)]

    need = max(off + 1 for off, _ in checks)
    need = max(need, 16)
    print(f"=== run freely for {duration}s ===")
    ram = run_hex(hex_path, machine=machine, duration_s=duration, dump_bytes=need)
    print(f"=== DRAM @ {RAM_BASE:#x} (first {len(ram)} bytes) ===")
    print(xp_to_dump_text(ram).rstrip())
    ok = True
    for off, exp in checks:
        got = ram[off] if off < len(ram) else None
        good = got == exp
        ok = ok and good
        print(f"  DRAM[{off:#x}] = {got}  (expected {exp}) -> {'OK' if good else 'FAIL'}")
    print(f"\n=== RESULT: {'PASS' if ok else 'FAIL'} ===")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
