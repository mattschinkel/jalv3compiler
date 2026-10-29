#!/usr/bin/env python3
"""Normalize YYYYMMDD words in an Intel HEX stream (stdout).

Used by test_task_codegen_golden.sh: the MIPS loader boot metadata embeds
a build-date word (e.g. 0x01352890 = 20261008) that changes at every
midnight. Mask plausible date words (20200101..20991231) to zero and
recompute record checksums so the golden compare is date-independent;
every other byte passes through untouched.

Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""
import re
import sys

def norm(path):
    out = []
    for line in open(path):
        line = line.rstrip("\n").upper()
        m = re.match(r"^:([0-9A-F]{2})([0-9A-F]{4})([0-9A-F]{2})([0-9A-F]*)([0-9A-F]{2})$", line)
        if not m or m.group(3) not in ("00",):
            out.append(line)
            continue
        cnt, addr, typ, data, _cks = m.groups()
        b = bytearray.fromhex(data)
        i = 0
        while i + 4 <= len(b):
            w = b[i] | (b[i+1] << 8) | (b[i+2] << 16) | (b[i+3] << 24)
            if 20200101 <= w <= 20991231:
                b[i:i+4] = b"\x00\x00\x00\x00"
            i += 4
        nd = b.hex().upper()
        cks = (-sum(int(nd[k:k+2], 16) for k in range(0, len(nd), 2))
               - int(cnt, 16) - int(addr[:2], 16) - int(addr[2:], 16)
               - int(typ, 16)) & 0xFF
        out.append(":%s%s%s%s%02X" % (cnt, addr, typ, nd, cks))
    return out

if __name__ == "__main__":
    sys.stdout.write("\n".join(norm(sys.argv[1])) + "\n")
