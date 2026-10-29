#!/usr/bin/env python3
# Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""Unified negative-test table: compile-error assertions (SDCC survey item 8).

Each entry pins a .jal file under unit-tests/negative/ that MUST fail to
compile, with a required substring of the diagnostic.  This
institutionalizes what the per-fix scripts asserted one at a time
(SDCC survey: "diagnostics are recorded, never asserted").

Run:
  python3 unit-tests/run_negative_tests.py [--compiler ./bin/jalv3-x86-64] [--jobs 8]

Exit 0 iff every entry fails to compile with its expected diagnostic.
Compile-only -- no simulator, safe at any gate level.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_COMPILER = os.path.join(ROOT, "bin", "jalv3-x86-64")
_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIBS = f"{_REPO}/demo_lib;{_REPO}/third_party/jallib/lib"

# (file, required diagnostic substring).  Substrings match the first
# diagnostic line's message text; keep them tight enough to pin the
# semantic error, loose enough to survive line-number drift.
TABLE = [
    ("hex_bin_call_reserved.jal",
     ["'hex' is reserved for the text cast (a called name is always the cast)"]),
    ("task_var_classic.jal",
     ["variables cannot be declared inside a task body on this target"]),
    ("task_var_out_of_order.jal",
     ["optional parameters must come after all required parameters"]),
    ("task_default_not_const.jal",
     ["default must be a constant expression"]),
    ("task_default_on_out.jal",
     ["(got 'default')"]),
    ("mb_over_ceiling.jal",
     ["exceeds total available memory space",
      "Array of 'big' (4094 bytes)"]),
    ("ptr_bound_bank_span_script.jal",
     ["is bound to a pointer"]),
    ("fixed_str_overflow.jal",
     ["string literal is 17 characters but the fixed-string parameter's capacity is 16"]),
    ("ptr_subscript_scalar.jal",
     ["subscript on data pointer requires pointer-to-array"]),
    ("proc_ptr_needs_call.jal",
     ["Procedure pointer requires call syntax"]),
    ("cast_byte_of_string.jal",
     ["cannot cast a string to a numeric type"]),
    ("const_to_out.jal",
     ["type mismatch"]),
    ("string_heap_missing.jal",
     ["dynamic string storage requires const word _string_heap_size"]),
    # pointer-signature enforcement (recovered from the retired
    # scripts/run_pointer_negative_tests.py; its compilertest case
    # directory was never committed and was lost)
    ("ptr_sig_out_bound_to_in.jal",
     ["type mismatch"]),
    ("ptr_sig_inout_bound_to_in.jal",
     ["type mismatch"]),
    ("ptr_fn_return_mismatch.jal",
     ["type mismatch"]),
    ("ptr_fn_call_too_few.jal",
     ["Too few parameters"]),
    ("ptr_const_write.jal",
     ["cannot assign or modify through pointer to const data"]),
    ("ptr_arr_bound_to_scalar.jal",
     ["type mismatch"]),
    # native bit-array bounds (recovered from the retired
    # scripts/run_bit_array_negative_tests.py; the const-initializer
    # variant only WARNS "more initializers than expected" in the
    # current compiler and is not pinned here)
    ("bita_const_index_oob.jal",
     ["subscript out of range"]),
    ("bita_formal_index_oob.jal",
     ["subscript out of range"]),
    # PIC32MX MVEC table is 64 x 32 bytes (vector index 0..63)
    ("mx_mvec_vector_64.jal",
     ["PIC32MX multi-vector table has 64 entries",
      "out of range (0..63)"]),
    # for-loop counts the counter can never reach (fix_for_loop_count_range;
    # before, the loop-exit relation folded "always FALSE" and everything
    # after the never-exiting loop was silently dead-eliminated)
    ("for_count_byte_overflow.jal",
     ["loop count 300 doesn't fit the loop variable (max 255)"]),
    ("for_count_signed_overflow.jal",
     ["loop count 130 doesn't fit the loop variable (max 127)"]),
    ("for_count_word_overflow.jal",
     ["loop count 70000 doesn't fit the loop variable (max 65535)"]),
]

# generated on demand by gen_ptr_bound_script() (the proven script shape)
# ESCAPED bind (re-bound through q): keeps the holed-run refusal. An
# unescaped bind now walks the chunks (escape analysis, 2026-10-26) --
# the walk shapes live in unit-tests/test_openarray_chunked_pins.sh.
PTR_BOUND_SCRIPT = """include 16f1825
pragma target clock 20_000_000
var byte buf[200]
var pointer p as var byte[200]
var pointer q as var byte[200]
var byte x
var byte idx
procedure touch() is
   p is buf
   q is p
   idx = 5
   buf[0] = 42
   x = q[idx]
end procedure
touch()
forever loop
end loop
"""


def gen_ptr_bound_script(workdir):
    path = os.path.join(workdir, "ptr_bound_bank_span_script.jal")
    with open(path, "w") as f:
        f.write(PTR_BOUND_SCRIPT)
    return path


def compile_one(compiler, path):
    """Return (summary_errors, full_output)."""
    try:
        r = subprocess.run([compiler, "-s", LIBS, "-no-codfile", path],
                           capture_output=True, text=True, timeout=60)
        out = r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        return -1, "TIMEOUT"
    m = re.findall(r"(\d+) errors", out)
    errs = int(m[-1]) if m else -1
    return errs, out


def check_entry(compiler, workdir, entry):
    fname, wants = entry
    if fname == "ptr_bound_bank_span_script.jal":
        path = gen_ptr_bound_script(workdir)
    else:
        path = os.path.join(HERE, "negative", fname)
    if not os.path.exists(path):
        return (fname, False, "file not found")
    errs, out = compile_one(compiler, path)
    if errs < 1:
        return (fname, False, f"compiled clean ({errs} errors) — expected a failure")
    missing = [w for w in wants if w not in out]
    if missing:
        first = next((l for l in out.splitlines()
                      if re.match(r"^\S", l) and ":" in l), out[:200])
        return (fname, False,
                f"diagnostic missing {missing!r}; got: {first[:160]}")
    return (fname, True, f"{errs} errors, {len(wants)} diagnostic(s) pinned")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", default=DEFAULT_COMPILER)
    ap.add_argument("--jobs", type=int, default=4)
    args = ap.parse_args()

    workdir = tempfile.mkdtemp(prefix="jal_negative_")

    entries = []
    for key, want in TABLE:
        entries.append((key, want))

    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        results = list(ex.map(lambda e: check_entry(args.compiler, workdir, e),
                              entries))

    fails = 0
    for key, ok, msg in results:
        print(f"{'ok  ' if ok else 'FAIL'} {key}: {msg}")
        fails += 0 if ok else 1
    print(f"----\n{len(results) - fails}/{len(results)} negative assertions hold")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
