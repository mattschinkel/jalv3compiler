#!/usr/bin/env python3
"""Single-instance guard for the test/sim entry scripts.

Each entry script acquires a named lock before doing any work; a second
instance refuses to run with a clear error instead of double-driving the
same output directories (a real footgun: interleaved runs corrupt report
archives and waste 15-20 min of compute before anyone notices).

Lock files live in /tmp (tmpfs: reboot clears them).  A lock whose PID is
no longer alive is stale and gets taken over with a warning.

Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""
from __future__ import annotations

import atexit
import os
import signal
import sys
import time

_LOCK_DIR = "/tmp"
_held: list[str] = []


def _alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def acquire(name: str) -> None:
    """Take the lock `name` or exit(1) if a live instance holds it."""
    path = os.path.join(_LOCK_DIR, "jalv2_%s.lock" % name)
    mine = "%d %d\n" % (os.getpid(), time.time())
    if os.path.exists(path):
        try:
            with open(path) as f:
                other = f.read().split()
            pid = int(other[0])
        except (OSError, ValueError, IndexError):
            pid = -1
        if pid > 0 and pid != os.getpid() and _alive(pid):
            sys.exit("refusing to start: another '%s' run is active (PID %d, "
                     "lock %s)" % (name, pid, path))
        sys.stderr.write("runlock: taking over stale lock %s (PID %s)\n"
                         % (path, pid if pid > 0 else "?"))
    with open(path, "w") as f:
        f.write(mine)
    _held.append(path)

    # SIGTERM/SIGINT skip atexit; release the lock on the way out so the
    # next run does not have to take over a stale lock.  (SIGKILL cannot
    # be handled -- stale takeover covers it.)
    for sig in (signal.SIGTERM, signal.SIGINT):
        try:
            prev = signal.getsignal(sig)

            def _on_sig(s, frame, prev=prev):  # noqa: F811
                _release()
                if callable(prev):
                    prev(s, frame)
                else:
                    sys.exit(128 + s)

            signal.signal(sig, _on_sig)
        except (ValueError, OSError):
            pass


def _release() -> None:
    while _held:
        path = _held.pop()
        try:
            with open(path) as f:
                pid = int(f.read().split()[0])
            if pid == os.getpid():
                os.unlink(path)
        except (OSError, ValueError, IndexError):
            pass


atexit.register(_release)
