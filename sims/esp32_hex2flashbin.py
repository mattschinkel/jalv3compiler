#!/usr/bin/env python3
"""esp32_hex2flashbin.py -- Intel HEX (flash-image addressed) -> raw flash bin.

The ESP32 back-end emits its boot image as Intel HEX addressed at flash
offset 0x1000 (the second-stage-bootloader slot). QEMU's -M esp32 needs a
raw flash image passed as an MTD drive, and esptool write-flash also
accepts the raw bin. This script converts (default 4 MB image, matching
the compiler's image-header flash-size field).

Usage: esp32_hex2flashbin.py <input.hex> <output.bin> [size-hex]


Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""
import os
import sys
import time

FLASH_SIZE = 0x400000
FLASH_OFF = 0x1000


def hex_to_flash(hex_path: str, flash: bytearray) -> int:
    hi = 0
    n = 0
    for line in open(hex_path):
        line = line.strip()
        if not line.startswith(':'):
            continue
        data = bytes.fromhex(line[1:])
        cnt, addr, typ = data[0], (data[1] << 8) | data[2], data[3]
        payload = data[4:4 + cnt]
        if typ == 4:                       # extended linear address
            hi = (payload[0] << 8) | payload[1]
        elif typ == 0:                     # data
            base = (hi << 16) | addr
            if base + cnt > len(flash):
                raise SystemExit(
                    f"hex data at {base:#x} exceeds flash size {len(flash):#x}")
            flash[base:base + cnt] = payload
            n += cnt
        elif typ == 1:                     # EOF
            break
    return n


def write_stamp(hex_path: str, bin_path: str) -> None:
    """Record the build identity next to the image. runpark.py refuses to
    run an image whose stamp does not match (stale-image guard)."""
    hb, bb = os.stat(hex_path), os.stat(bin_path)
    stamp = bin_path + '.build'
    with open(stamp, 'w') as f:
        f.write(f"hex={os.path.abspath(hex_path)}\n")
        f.write(f"hex_size={hb.st_size}\n")
        f.write(f"hex_mtime_ns={hb.st_mtime_ns}\n")
        f.write(f"bin_size={bb.st_size}\n")
        f.write(f"bin_mtime_ns={bb.st_mtime_ns}\n")


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print(f"usage: {sys.argv[0]} <input.hex> <output.bin> [size-hex]",
              file=sys.stderr)
        return 2
    size = int(sys.argv[3], 16) if len(sys.argv) == 4 else FLASH_SIZE
    flash = bytearray(size)
    n = hex_to_flash(sys.argv[1], flash)
    if n == 0:
        raise SystemExit("no data records found in hex")
    open(sys.argv[2], 'wb').write(bytes(flash))
    write_stamp(sys.argv[1], sys.argv[2])
    print(f"{sys.argv[1]}: {n} bytes -> {sys.argv[2]} "
          f"({size:#x} flash, image at {FLASH_OFF:#x})")
    return 0


if __name__ == '__main__':
    sys.exit(main())
