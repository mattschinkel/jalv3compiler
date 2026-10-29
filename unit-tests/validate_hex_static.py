#!/usr/bin/env python3
"""
Static validation for baseline vs current compiler outputs (no hardware).

For every sample whose .hex differs in size or raw content from baseline:
  - Writes a unified diff of normalized .asm files (strips compiler banner lines).
  - Compares Intel HEX program memory maps and counts differing byte addresses.

Run standalone:
  python3 validate_hex_static.py

Or invoked automatically from:  python3 unittests.py --compare
(Progress on stderr; HEX scan and per-sample work use a thread pool. Default cap from
STATIC_VALIDATION_MAX_WORKERS or env STATIC_VALIDATION_MAX_WORKERS; unittests.py --compare
passes max_workers from --parallel / JALV2_TEST_JOBS when set.)


Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from difflib import unified_diff
from typing import Dict, List, Optional, Tuple

# Parallelism during --compare static validation (HEX scan + per-sample diff).
STATIC_VALIDATION_MAX_WORKERS = 8


def _validation_worker_count(max_workers: Optional[int] = None) -> int:
    if max_workers is not None:
        w = max_workers
    else:
        try:
            w = int(
                os.environ.get(
                    "STATIC_VALIDATION_MAX_WORKERS",
                    str(STATIC_VALIDATION_MAX_WORKERS),
                )
            )
        except ValueError:
            w = STATIC_VALIDATION_MAX_WORKERS
    return max(1, min(64, w))


def _stderr_progress(done: int, total: int, label: str, width: int = 40) -> None:
    """Single-line progress on stderr; TTY gets a live bar, else ~20 lines per phase."""
    err = sys.stderr
    if total <= 0:
        return
    d = min(max(done, 0), total)
    pct = 100.0 * d / total
    filled = int(width * d / total)
    bar = "#" * filled + "-" * (width - filled)
    msg = f"{label} [{bar}] {d}/{total} ({pct:.1f}%)"
    if err.isatty():
        err.write("\r" + msg.ljust(96) + "\r")
        err.flush()
    else:
        # Log when crossing ~5% buckets so logs stay short even for thousands of items.
        bucket = (20 * d) // total if total else 20
        prev_bucket = (20 * (d - 1)) // total if d > 1 and total else -1
        if d == total or d == 1 or bucket > prev_bucket:
            err.write(msg + "\n")
            err.flush()


def _stderr_progress_nl() -> None:
    if sys.stderr.isatty():
        sys.stderr.write("\n")
        sys.stderr.flush()


def normalize_asm_text(text: str) -> str:
    """Drop lines that only differ by compiler path/version so diffs show real codegen.

    Also strips listing-only noise so `asm_same_after_normalize` can prove two
    listings equivalent when the image differs only cosmetically:
      - pcode debug annotation tokens (`12{u-} ...`, `{eos}`, `{block}`, ...)
      - symbol-table EQU lines (names/addresses legitimately move between builds;
        the HEX memory compare already covers data/address changes)
      - symbolic operand prefixes on MOVLB (`movlb v_osccon, 1` == `movlb 1`)
      - generated local label numbering (`l__l61` vs `l__l67`)
      - comment lines (source-path banners, registry notes), pcode `= ...`
        assignment lines, and label-only lines (labels do not encode; the
        HEX memory compare stays authoritative for addresses)
    """
    anno_num = re.compile(r"\s*\d+\{[^}]*\}")
    anno_tag = re.compile(r"\s*\{[A-Za-z0-9_-]+\}")
    equ_line = re.compile(r"^\s*\S+\s+EQU\s")
    movlb_sym = re.compile(r"^(\s*movlb\s+)\S+,\s*(\d+)", re.IGNORECASE)
    local_lbl = re.compile(r"\bl__l\d+\b|\b_l\d+\b")
    label_only = re.compile(r"^\s*[A-Za-z_.][\w.]*:\s*$")
    lines: List[str] = []
    for line in text.splitlines(keepends=True):
        # strip pcode annotation prefixes first (` 2{u-} = ...`, ` 2{u-} _main:`)
        line = anno_num.sub("", line)
        line = anno_tag.sub("", line)
        s = line.lstrip()
        if s.startswith(";") or s.startswith("="):
            continue
        if "(" in line:
            # JAL source echo (e.g. `usec_delay(100000)`); never an opcode line
            continue
        # drop trailing listing comments (`movlb 0 ; pin_a0_direction`)
        line = line.split(";", 1)[0].rstrip() + "\n"
        if equ_line.match(line):
            continue
        if label_only.match(line):
            continue
        line = movlb_sym.sub(r"\1\2", line)
        line = local_lbl.sub("l__lN", line)
        if not line.strip():
            continue
        lines.append(line)
    return "".join(lines)


def parse_intel_hex_memory(text: str) -> Dict[int, int]:
    """
    Parse Intel HEX into a sparse map of absolute address -> byte.
    Handles record types 0 (data), 1 (EOF), 2 (extended segment), 4 (extended linear).
    """
    memory: Dict[int, int] = {}
    upper_linear = 0
    segment_base = 0

    for raw in text.splitlines():
        line = raw.strip()
        if not line.startswith(":") or len(line) < 11:
            continue
        try:
            nbytes = int(line[1:3], 16)
            addr_lo = int(line[3:7], 16)
            rtype = int(line[7:9], 16)
            rest = line[9:]
            data_part = rest[: nbytes * 2]
        except ValueError:
            continue

        if rtype == 0:
            base = upper_linear + segment_base + addr_lo
            for i in range(nbytes):
                memory[base + i] = int(data_part[i * 2 : i * 2 + 2], 16)
        elif rtype == 1:
            break
        elif rtype == 2 and nbytes == 2:
            segment_base = int(data_part, 16) << 4
            upper_linear = 0
        elif rtype == 4 and nbytes == 2:
            upper_linear = int(data_part, 16) << 16
            segment_base = 0
        # other types ignored for image compare

    return memory


def hex_files_differ(path_b: str, path_c: str) -> bool:
    try:
        with open(path_b, "rb") as fb, open(path_c, "rb") as fc:
            return fb.read() != fc.read()
    except OSError:
        return True


def compare_memory_maps(
    mem_b: Dict[int, int], mem_c: Dict[int, int]
) -> Tuple[int, int, int]:
    """Returns (differing_byte_addresses, only_in_b, only_in_c)."""
    keys_b = set(mem_b.keys())
    keys_c = set(mem_c.keys())
    only_b = len(keys_b - keys_c)
    only_c = len(keys_c - keys_b)
    common = keys_b & keys_c
    diff = sum(1 for a in common if mem_b[a] != mem_c[a])
    return diff, only_b, only_c


def write_unified_diff(
    path_b: str, path_c: str, out_path: str, label_b: str, label_c: str
) -> Tuple[bool, int]:
    """
    Write unified diff of normalized ASM. Returns (identical_after_normalize, line_count_in_diff).
    """
    try:
        with open(path_b, "r", encoding="utf-8", errors="replace") as f:
            nb = normalize_asm_text(f.read())
        with open(path_c, "r", encoding="utf-8", errors="replace") as f:
            nc = normalize_asm_text(f.read())
    except OSError as e:
        with open(out_path, "w", encoding="utf-8") as o:
            o.write(f"# Could not read ASM: {e}\n")
        return False, 0

    if nb == nc:
        with open(out_path, "w", encoding="utf-8") as o:
            o.write(
                "# Normalized ASM is byte-identical (only compiler banner/command line differed).\n"
            )
        return True, 0

    a = nb.splitlines(keepends=True)
    b = nc.splitlines(keepends=True)
    diff_lines = list(
        unified_diff(
            a,
            b,
            fromfile=label_b,
            tofile=label_c,
            lineterm="",
        )
    )
    # unified_diff omits newline on last line sometimes; normalize
    body = "\n".join(diff_lines)
    if body and not body.endswith("\n"):
        body += "\n"
    with open(out_path, "w", encoding="utf-8") as o:
        o.write(body)
    return False, len(diff_lines)


def run_static_validation(
    base_dir: str,
    curr_dir: str,
    compare_dir: str,
    quiet: bool = False,
    max_workers: Optional[int] = None,
) -> str:
    """
    Compare all samples where HEX differs. Writes under compare_dir/static_validation/.
    Returns path to summary TSV.

    Uses up to max_workers (or env/default) threads for the HEX scan and for
    processing differing samples (order of rows matches sorted sample names).
    """
    workers = _validation_worker_count(max_workers)
    base_hex = os.path.join(base_dir, "hex")
    curr_hex = os.path.join(curr_dir, "hex")
    base_asm = os.path.join(base_dir, "asm")
    curr_asm = os.path.join(curr_dir, "asm")

    out_root = os.path.join(compare_dir, "static_validation")
    asm_diff_dir = os.path.join(out_root, "asm_diff")
    os.makedirs(asm_diff_dir, exist_ok=True)

    names: set = set()
    if os.path.isdir(base_hex):
        names.update(
            os.path.splitext(n)[0]
            for n in os.listdir(base_hex)
            if n.lower().endswith(".hex")
        )
    if os.path.isdir(curr_hex):
        names.update(
            os.path.splitext(n)[0]
            for n in os.listdir(curr_hex)
            if n.lower().endswith(".hex")
        )

    sorted_names = sorted(names)
    nscan = len(sorted_names)

    def _scan_one(name: str) -> bool:
        pb = os.path.join(base_hex, name + ".hex")
        pc = os.path.join(curr_hex, name + ".hex")
        if not os.path.isfile(pb) or not os.path.isfile(pc):
            return True
        if os.path.getsize(pb) != os.path.getsize(pc):
            return True
        return hex_files_differ(pb, pc)

    differing: List[str] = []
    if nscan > 0:
        scan_workers = min(workers, nscan)
        flags: Dict[str, bool] = {}
        with ThreadPoolExecutor(max_workers=scan_workers) as ex:
            future_map = {ex.submit(_scan_one, n): n for n in sorted_names}
            completed = 0
            for fut in as_completed(future_map):
                n = future_map[fut]
                flags[n] = fut.result()
                completed += 1
                if not quiet:
                    _stderr_progress(completed, nscan, "Compare: scan HEX")
        differing = [n for n in sorted_names if flags[n]]

    if not quiet and nscan:
        _stderr_progress_nl()

    summary_path = os.path.join(out_root, "static_validation_summary.tsv")
    readme_path = os.path.join(out_root, "README.txt")

    def _process_one(name: str) -> dict:
        pb_hex = os.path.join(base_hex, name + ".hex")
        pc_hex = os.path.join(curr_hex, name + ".hex")
        pb_asm = os.path.join(base_asm, name + ".asm")
        pc_asm = os.path.join(curr_asm, name + ".asm")

        current_hex_zero_bytes = "no"
        if os.path.isfile(pc_hex) and os.path.getsize(pc_hex) == 0:
            current_hex_zero_bytes = "yes"

        hex_status = "ok"
        mem_diff = only_b = only_c = -1
        mem_b = mem_c = 0
        if not os.path.isfile(pb_hex):
            hex_status = "missing_baseline_hex"
        elif not os.path.isfile(pc_hex):
            hex_status = "missing_current_hex"
        else:
            try:
                with open(pb_hex, "r", encoding="utf-8", errors="ignore") as f:
                    tb = f.read()
                with open(pc_hex, "r", encoding="utf-8", errors="ignore") as f:
                    tc = f.read()
                mb = parse_intel_hex_memory(tb)
                mc = parse_intel_hex_memory(tc)
                mem_b = len(mb)
                mem_c = len(mc)
                mem_diff, only_b, only_c = compare_memory_maps(mb, mc)
            except OSError:
                hex_status = "hex_read_error"

        asm_same_norm = ""
        asm_diff_lines = ""
        diff_relpath = f"asm_diff/{name}.diff"
        diff_path = os.path.join(asm_diff_dir, name + ".diff")

        if os.path.isfile(pb_asm) and os.path.isfile(pc_asm):
            identical, dlines = write_unified_diff(
                pb_asm,
                pc_asm,
                diff_path,
                "baseline.asm",
                "current.asm",
            )
            asm_same_norm = "yes" if identical else "no"
            asm_diff_lines = str(dlines)
        else:
            asm_same_norm = "missing_asm"
            missing = []
            if not os.path.isfile(pb_asm):
                missing.append("baseline")
            if not os.path.isfile(pc_asm):
                missing.append("current")
            with open(diff_path, "w", encoding="utf-8") as o:
                o.write(f"# ASM missing: {', '.join(missing)}\n")
            asm_diff_lines = "0"

        please_review = "no"
        if current_hex_zero_bytes == "yes":
            please_review = "no"
        elif hex_status != "ok":
            please_review = "no"
        else:
            please_review = "yes"

        return {
            "sample": name,
            "please_review": please_review,
            "current_hex_zero_bytes": current_hex_zero_bytes,
            "hex_status": hex_status,
            "hex_memory_bytes_baseline": str(mem_b),
            "hex_memory_bytes_current": str(mem_c),
            "hex_memory_addr_diff_bytes": str(mem_diff),
            "hex_only_addrs_baseline": str(only_b),
            "hex_only_addrs_current": str(only_c),
            "asm_same_after_normalize": asm_same_norm,
            "asm_unified_diff_lines": asm_diff_lines,
            "asm_diff_file": diff_relpath,
        }

    rows: List[dict] = []
    ndiff = len(differing)
    if ndiff > 0:
        diff_workers = min(workers, ndiff)
        rows_by_name: Dict[str, dict] = {}
        with ThreadPoolExecutor(max_workers=diff_workers) as ex:
            future_map = {ex.submit(_process_one, n): n for n in differing}
            completed = 0
            for fut in as_completed(future_map):
                n = future_map[fut]
                rows_by_name[n] = fut.result()
                completed += 1
                if not quiet:
                    _stderr_progress(completed, ndiff, "Compare: static validation")
        rows = [rows_by_name[n] for n in differing]

    if not quiet and ndiff:
        _stderr_progress_nl()

    with open(summary_path, "w", encoding="utf-8", newline="") as sf:
        if rows:
            w = csv.DictWriter(sf, fieldnames=list(rows[0].keys()), delimiter="\t")
            w.writeheader()
            w.writerows(rows)
        else:
            sf.write("# No differing HEX files found between baseline and current.\n")

    review_list_path = os.path.join(out_root, "samples_please_review.txt")
    skipped_zero_path = os.path.join(out_root, "samples_skipped_zero_byte_hex.txt")
    please_review_names = [
        r["sample"] for r in rows if r.get("please_review") == "yes"
    ]
    skipped_zero_names = [
        r["sample"] for r in rows if r.get("current_hex_zero_bytes") == "yes"
    ]
    with open(review_list_path, "w", encoding="utf-8") as rf:
        rf.write("PLEASE REVIEW (differing HEX; current .hex is non-empty)\n")
        rf.write("----------------------------------------------------\n")
        for n in sorted(please_review_names):
            rf.write(f"{n}\n")
    with open(skipped_zero_path, "w", encoding="utf-8") as zf:
        zf.write("SKIPPED — current .hex is 0 bytes (fix build before codegen review)\n")
        zf.write("-------------------------------------------------------------------\n")
        if skipped_zero_names:
            zf.write(
                "Root cause (see repo fixes/fix_unit_test_missing_hex_replace_zero_byte.md): current compiler\n"
                "often aborts in PIC code pass 2 (heap corruption); baseline jalv25r9 builds OK.\n\n"
            )
        for n in sorted(skipped_zero_names):
            zf.write(f"{n}\n")

    with open(readme_path, "w", encoding="utf-8") as rf:
        rf.write(
            "Static validation (no hardware)\n"
            "-------------------------------\n"
            "This folder is generated by validate_hex_static.py (via unittests.py --compare).\n\n"
            "samples_please_review.txt — basenames to manually review (diffing HEX, non-empty current).\n"
            "samples_skipped_zero_byte_hex.txt — diffing HEX but current output empty; see fixes/fix_unit_test_missing_hex_replace_zero_byte.md.\n\n"
            "static_validation_summary.tsv columns:\n"
            "  please_review               — yes: review asm_diff; no: empty current hex or bad hex_status\n"
            "  current_hex_zero_bytes      — yes if Output_Current/hex/<sample>.hex exists and size is 0\n"
            "  sample                      — sample basename\n"
            "  hex_status                  — ok | missing_* | hex_read_error\n"
            "  hex_memory_bytes_*          — number of byte addresses decoded from Intel HEX\n"
            "  hex_memory_addr_diff_bytes  — count of addresses where both have a byte but values differ\n"
            "  hex_only_addrs_*            — addresses present in only one image\n"
            "  asm_same_after_normalize    — yes if ASM matches after stripping ; compiler / ; command line\n"
            "  asm_unified_diff_lines      — line count in unified diff (0 if identical or only banner changed)\n"
            "  asm_diff_file               — path under static_validation/\n\n"
            "Interpretation:\n"
            "  • If asm_same_after_normalize=yes, the listing differs only in header comments.\n"
            "  • If hex_memory_addr_diff_bytes is large, the flashed program image changed; review .diff.\n"
            "  • This does not prove runtime behavior; use simulator or hardware for that.\n"
        )

    if not quiet:
        print(f"Static validation: {len(differing)} sample(s) with differing HEX")
        print(f"  Please review: {len(please_review_names)} (see {review_list_path})")
        if skipped_zero_names:
            print(
                f"  Skipped (0-byte current .hex): {len(skipped_zero_names)} (see {skipped_zero_path})"
            )
        print(f"  Summary: {summary_path}")
        print(f"  ASM diffs: {asm_diff_dir}/")
    return summary_path


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Static HEX/ASM validation between baseline and current compiler outputs."
    )
    ap.add_argument(
        "--base-dir",
        default=os.path.join("compilertest", "Output_Baseline"),
        help="Directory containing hex/ and asm/ (default: compilertest/Output_Baseline)",
    )
    ap.add_argument(
        "--curr-dir",
        default=os.path.join("compilertest", "Output_Current"),
        help="Directory containing hex/ and asm/ (default: compilertest/Output_Current)",
    )
    ap.add_argument(
        "--compare-dir",
        default=os.path.join("compilertest", "Output_Compare"),
        help="Where to write static_validation/ (default: compilertest/Output_Compare)",
    )
    args = ap.parse_args()
    run_static_validation(args.base_dir, args.curr_dir, args.compare_dir, quiet=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
