#!/usr/bin/env python3
"""Lowest-priority drop for the test/sim entry scripts.

Each entry script (unittests.py, run_regression_suite.py,
run_pic32_regression.py, run_esp32_regression.py) lowers itself to the
lowest CPU (nice 19) and I/O (ionice idle class) priority before spawning
any work: the parallel compiler / gpsim / QEMU subprocesses and the
later-created worker threads inherit both, so full-suite runs never
compete with foreground work on this shared host -- and still run at full
speed on an otherwise idle machine (priority only arbitrates under
contention).  Lowering priority needs no privileges; failures are warned,
never fatal.  See fixes/fix_unit_test_low_priority.md.

Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys


def lower_own_priority() -> None:
    """Drop this process, and everything it later spawns, to the lowest
    CPU and I/O priority (inherited by child processes and by threads
    created after this call)."""
    try:
        os.setpriority(os.PRIO_PROCESS, 0, 19)
    except (AttributeError, OSError) as e:
        print(
            f"Warning: could not set lowest CPU priority (nice 19): {e}",
            file=sys.stderr,
        )
    ionice = shutil.which("ionice")
    if ionice:
        subprocess.run(
            [ionice, "-c", "3", "-p", str(os.getpid())],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
