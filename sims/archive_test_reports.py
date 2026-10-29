#!/usr/bin/env python3
"""
Post-run snapshots of report trees + pre-run cleanup of working directories.

After a successful (or completed) unit test / compare / gpsim publish pass, a **copy** of the
current `sims/reports/` and `sims/output_compare/` trees is stored under:

  sims/report_archive/<YYYYMMDD_HHMMSS_microseconds>_<label>/
    reports/
    Output_Compare/

Snapshots skip **.jal**, **.hex**, and **.asm** (full trees remain under `Output_Compare/`; archives
keep summaries, logs, `.cod`, and other non-skipped files).

The canonical directories stay in place until the **next** run, which **clears** them first
(`unittests.py` / `run_regression_suite.py` / compare mode) so each run starts fresh.
Unit-test clears may **preserve** selected files (see `clear_comparison_reports_and_output_compare` /
`clear_output_compare_only`) so `publish_unit_test_web.py` still receives both run summaries.

Web dashboard: before publish, `data/` is cleared; after new files are copied, a snapshot of
the fresh `data/` goes to `JALV2_UNIT_TEST_WEB_ROOT/archive/<timestamp>_publish/data/`.

Disable snapshots only: set JALV2_NO_REPORT_ARCHIVE to 1, true, or yes (clearing still runs).

After unit-test or gpsim regression runs, `append_changelog_test_automation()` can insert one line
into repo-root `CHANGELOG.txt` with a results summary and `report_archive` / `web_archive` paths.
Set `JALV2_NO_CHANGELOG_ENTRY=1` to skip.
For gpsim (`run_regression_suite.py` / `unittests.py --gpsim-regression`), a line is written only when
`gpsim_regression_run.json` reports `changelog_eligible` (full ordered suite list and no checkpoint
abort), unless `JALV2_GPSIM_CHANGELOG_PARTIAL=1`.


Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""

from __future__ import annotations

import os
import re
import shutil
from datetime import datetime
from pathlib import Path
from typing import Optional

# First line of dated entries in CHANGELOG.txt (YYYY-MM-DD at column 0).
_CHANGELOG_DATED_LINE = re.compile(r"^\d{4}-\d{2}-\d{2}")
CHANGELOG_FILENAME = "CHANGELOG.txt"

# Omit bulky compiler / source copies from report_archive (working dirs are unchanged).
_ARCHIVE_SKIP_SUFFIXES = frozenset({".jal", ".hex", ".asm"})


def _copytree_omit_suffixes(src: Path, dst: Path) -> None:
    """Copy directory tree; skip files whose suffix (case-insensitive) is in _ARCHIVE_SKIP_SUFFIXES."""
    skip = _ARCHIVE_SKIP_SUFFIXES
    for path in src.rglob("*"):
        if not path.is_file():
            continue
        if path.suffix.lower() in skip:
            continue
        rel = path.relative_to(src)
        out = dst / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, out, follow_symlinks=True)


def _env_disables_snapshot() -> bool:
    v = os.environ.get("JALV2_NO_REPORT_ARCHIVE", "").strip().lower()
    return v in ("1", "true", "yes", "on")


def _env_disables_changelog_append() -> bool:
    v = os.environ.get("JALV2_NO_CHANGELOG_ENTRY", "").strip().lower()
    return v in ("1", "true", "yes", "on")


def changelog_run_timestamp_local() -> str:
    """Local wall time + zone abbreviation for CHANGELOG lines (matches repo convention)."""
    now = datetime.now().astimezone()
    tz = now.tzname() or now.strftime("%z")
    return f"{now.strftime('%Y-%m-%d %H:%M:%S')} {tz}"


def append_changelog_test_automation(
    repo_root: str | Path,
    *,
    summary: str,
    report_snapshot: Optional[Path] = None,
    web_snapshot: Optional[Path] = None,
    quiet: bool = False,
) -> None:
    """
    Insert one line at the top of the dated section in CHANGELOG.txt (after the header).

    Set JALV2_NO_CHANGELOG_ENTRY=1 to skip. Failures are printed as warnings, never raised.
    """
    if _env_disables_changelog_append():
        return

    root = Path(repo_root).resolve()
    changelog = root / CHANGELOG_FILENAME
    if not changelog.is_file():
        if not quiet:
            print(f"Warning: {CHANGELOG_FILENAME} not found; skipping changelog append")
        return

    parts = [changelog_run_timestamp_local(), " — ", summary.strip()]
    if report_snapshot is not None:
        try:
            rel = report_snapshot.resolve().relative_to(root)
            rel_s = rel.as_posix()
        except ValueError:
            rel_s = str(report_snapshot.resolve())
        parts.append(f"; report_archive: **`{rel_s}`**")
    else:
        parts.append(
            "; report_archive: **(none — JALV2_NO_REPORT_ARCHIVE or nothing to snapshot)**"
        )
    if web_snapshot is not None:
        parts.append(f"; web_archive: **`{web_snapshot.resolve().as_posix()}`**")

    line = "".join(parts).replace("\n", " ").replace("\r", " ")
    if len(line) > 8000:
        line = line[:7997] + "..."

    try:
        raw = changelog.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        if not quiet:
            print(f"Warning: could not read {changelog}: {e}")
        return

    lines = raw.splitlines()
    insert_at = 0
    for i, ln in enumerate(lines):
        if _CHANGELOG_DATED_LINE.match(ln):
            insert_at = i
            break
    else:
        insert_at = len(lines)

    lines.insert(insert_at, line)
    if not raw.endswith("\n") and lines:
        pass
    new_body = "\n".join(lines) + ("\n" if lines else "")

    try:
        changelog.write_text(new_body, encoding="utf-8", newline="\n")
    except OSError as e:
        if not quiet:
            print(f"Warning: could not write {changelog}: {e}")
        return

    if not quiet:
        print(f"Appended run line to {CHANGELOG_FILENAME} (dated section).")


def _dir_has_files(path: Path) -> bool:
    if not path.is_dir():
        return False
    for p in path.rglob("*"):
        if p.is_file():
            return True
    return False


def _safe_label(label: str) -> str:
    s = re.sub(r"[^a-zA-Z0-9_.-]+", "_", label.strip()).strip("_")
    return (s[:80] if s else "run")


def snapshot_test_report_trees(
    repo_root: str | Path,
    *,
    label: str = "run",
    quiet: bool = False,
) -> Optional[Path]:
    """
    Copy Comparison/reports and Output_Compare (when they contain files) into
    report_archive/<timestamp>_<label>/. Skips *.jal, *.hex, *.asm. Leaves originals unchanged.
    """
    if _env_disables_snapshot():
        return None

    root = Path(repo_root).resolve()
    reports = root / "sims" / "reports"
    compare = root / "sims" / "output_compare"
    archive_root = root / "sims" / "report_archive"

    pairs: list[tuple[str, Path]] = []
    if _dir_has_files(reports):
        pairs.append(("reports", reports))
    if _dir_has_files(compare):
        pairs.append(("Output_Compare", compare))
    if not pairs:
        return None

    ts = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    dest_parent = archive_root / f"{ts}_{_safe_label(label)}"
    dest_parent.mkdir(parents=True, exist_ok=False)

    for name, src in pairs:
        _copytree_omit_suffixes(src, dest_parent / name)

    if not quiet:
        try:
            rel = dest_parent.relative_to(root)
        except ValueError:
            rel = dest_parent
        print(f"Saved report snapshot to {rel}")
    return dest_parent


# Backward-compatible name (older call sites / docs)
archive_test_report_trees = snapshot_test_report_trees


def clear_comparison_reports_and_output_compare(
    repo_root: str | Path,
    *,
    quiet: bool = False,
    preserve_reports: tuple[str, ...] = (),
) -> None:
    """Remove working report trees before baseline/current unit test run; recreate empty dirs.

    ``preserve_reports`` lists filenames under ``Comparison/reports/`` to keep across a clear
    (e.g. keep ``baseline_test_summary.txt`` when starting a **current** run, and
    ``current_test_summary.txt`` when starting a **baseline** run) so both summaries can coexist
    for the web dashboard and ``publish_unit_test_web.py``.
    """
    root = Path(repo_root).resolve()
    reports = root / "sims" / "reports"
    oc = root / "sims" / "output_compare"
    saved: dict[str, bytes] = {}
    for name in preserve_reports:
        p = reports / name
        if p.is_file():
            saved[name] = p.read_bytes()
    for p in (reports, oc):
        if p.is_dir():
            shutil.rmtree(p)
    reports.mkdir(parents=True, exist_ok=True)
    oc.mkdir(parents=True, exist_ok=True)
    for name, data in saved.items():
        (reports / name).write_bytes(data)
    if not quiet:
        print("Cleared sims/reports and Output_Compare for a fresh run.")


def clear_output_compare_only(
    repo_root: str | Path,
    *,
    quiet: bool = False,
    preserve_files: tuple[str, ...] = (),
) -> None:
    """Remove Output_Compare before regenerating compare reports.

    ``preserve_files`` lists filenames under ``Output_Compare/`` to keep (e.g.
    ``current_missing_hex.txt`` from the last ``--current`` run, which ``--compare`` would
    otherwise delete before repopulating compare outputs).
    """
    root = Path(repo_root).resolve()
    oc = root / "sims" / "output_compare"
    saved: dict[str, bytes] = {}
    for name in preserve_files:
        p = oc / name
        if p.is_file():
            saved[name] = p.read_bytes()
    if oc.is_dir():
        shutil.rmtree(oc)
    oc.mkdir(parents=True, exist_ok=True)
    for name, data in saved.items():
        (oc / name).write_bytes(data)
    if not quiet:
        print("Cleared sims/output_compare for a fresh compare run.")


def clear_gpsim_regression_summary(repo_root: str | Path) -> None:
    """Delete previous gpsim summary and run-stats sidecar before a suite run (other files under reports/ are kept)."""
    rep = Path(repo_root).resolve() / "sims" / "reports"
    for name in ("gpsim_regression_summary.txt", "gpsim_regression_run.json"):
        p = rep / name
        if p.is_file():
            p.unlink()


def clear_web_dashboard_data_dir(web_root: str | Path, *, quiet: bool = False) -> None:
    """Empty WEB_ROOT/data/ before publishing new dashboard files."""
    wr = Path(web_root).resolve()
    data_dir = wr / "data"
    if data_dir.is_dir():
        shutil.rmtree(data_dir)
    data_dir.mkdir(parents=True, exist_ok=True)
    if not quiet:
        print(f"Cleared {data_dir} before publish.")


def snapshot_web_dashboard_data_dir(
    web_root: str | Path,
    *,
    label: str = "publish",
    quiet: bool = False,
) -> Optional[Path]:
    """
    After publish: copy the new data/ tree to WEB_ROOT/archive/<ts>_<label>/data/.
    """
    if _env_disables_snapshot():
        return None

    wr = Path(web_root).resolve()
    data_dir = wr / "data"
    if not _dir_has_files(data_dir):
        return None

    ts = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    dest_parent = wr / "archive" / f"{ts}_{_safe_label(label)}"
    dest_parent.mkdir(parents=True, exist_ok=False)
    _copytree_omit_suffixes(data_dir, dest_parent / "data")

    if not quiet:
        print(f"Saved web dashboard snapshot to {dest_parent}")
    return dest_parent


# Backward-compatible name
def archive_web_dashboard_data(
    web_root: str | Path,
    *,
    label: str = "publish",
    quiet: bool = False,
) -> Optional[Path]:
    """Deprecated alias: use snapshot_web_dashboard_data_dir after publish instead."""
    return snapshot_web_dashboard_data_dir(web_root, label=label, quiet=quiet)


if __name__ == "__main__":
    r = Path(__file__).resolve().parent
    out = snapshot_test_report_trees(r, label="manual_cli")
    print(out or "(nothing to snapshot)")
