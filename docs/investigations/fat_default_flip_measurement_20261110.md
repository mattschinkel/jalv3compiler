# Fat-pointer default-flip differential (gate off vs gate on)

**Date:** 2026-11-11 (plan path dated 2026-11-10)
**Status:** measurement; the owner approved the flip and it landed 2026-11-11
(commit `2fd1965`), with the corpus HEX manifest refreshed (90 of 2,940
images changed, file count unchanged)
**Plan:** Phase 2, the flip gate (C4, T2, T3)
**Design:** [`fat_pointer_f5_survey_20261109.md`](fat_pointer_f5_survey_20261109.md)

## Corpus

The whole jallib sample tree as compiled by the unit gate: **2,941** files
under `third_party/jallib/samples/` (library files and programs), with
`-s demo_lib;third_party/jallib/lib;tests`.  Gate-off images are the
committed HEX manifest (`unit-tests/manifest/hex_manifest.txt`,
`unit-tests/Output_Current/hex`); gate-on images are a fresh
`JALV2_FAT_POINTERS=1` compile of the same tree.

## Image differential

| result | files |
|---|---:|
| identical image | 2,850 |
| changed image | 90 |
| new failure | 0 |
| known failure (the over-RAM allowlist entry) | 1 |

* **The 90 changed programs are exactly the programs that exercise fat**
  (the engine changes the escaped-pointer representation; the other 2,850
  have no escaped pointer surface and stay byte-identical).  This is the
  plan's positive control.
* The one failure is `16f630_stepper_unipolar_nonblocking.jal`, the
  over-RAM sample allowlisted in the unit gate; it fails under **both**
  gates (not a fat regression).

## Code size

Across the 90 changed programs:

| gate | words |
|---|---:|
| off | 276,610 |
| on | 283,735 |
| **delta** | **+7,125 (+2.58% on the exercising programs)** |

The unchanged 2,850 programs pay nothing.  Corpus-wide the delta is ~2.4
words per file on average (0.08% of the corpus code size); the per-program
cost on exercising programs averages **+79 words**.

Spot review of a small changed program (`12f1840_serial_hardware_hs.jal`):
the change is the tagged-pointer representation and the resulting RAM
address shifts (the fat dispatch tables), not an unrelated codegen change.

## Gate-mode pinning (T3)

The first `JALV2_FAT_POINTERS=1 make gates-l1` exposed **four unpinned
gate-off baselines**: `openarray_chunked_pins` (the indirect-call refusal
disappears under the engine), `multibank_array_errors` (an escaped
pointer-bound spanning array is allowed), `alloc_capacity` (pointer-array
capacities shift) and `access_plan_gate` (the plan rows change).  These are
mode assumptions, not runtime regressions; the runtime suites (gpsim, PIC32,
ESP32, picsim) all passed under the engine.

Every baseline script now sets its intended mode explicitly:
`test_openarray_chunked_pins.sh`, `test_multibank_array_errors.sh`,
`test_alloc_capacity.sh`, `test_access_recipe.sh`, `run_plan_gate.sh` and
`run_access_matrix.sh` export `JALV2_FAT_POINTERS=0`; the pin runner exports
the same default and asserts each baseline script states its mode; the
mode-specific pins (the fat pin's arms, the G3 witnesses, the env-switch pin)
set their own value per command.  The compile-pin gate now passes with the
ambient env **on** and **off** (72/72 both ways).

## Executed witnesses

`JALV2_FAT_POINTERS=1 make gates-l1` runs the whole L1 runtime ladder (gpsim,
PIC32 QEMU, ESP32 QEMU, the compile pins and the picsim suite) with the
engine on.  **Result (after T3 pinning):** gpsim, PIC32 181/181, ESP32, the
compile pins 72/72 and the demo kit pass; the picsim suite is **137/138** —
one witness fails under the engine:

* `16f18857_picsim_string_bare_decl.jal`: the program stalls after the first
  checkpoint (`_check_step` 1, not 2) inside the `print_string` path;
  `_string_heap+3` holds `'n'` instead of `'M'` (the payload copy is
  corrupted) and the capture sees nothing (`sc_n = 0`).  The same failure
  reproduces under `JALV2_FAT_POINTERS=1` with the binaries at `6187263` and
  `37d7f42`, i.e. **it predates the G3, heap and guard slices** — a
  pre-existing engine-on defect in the string/print/function-pointer path,
  not a regression of this batch.  It does not reproduce with the engine
  off.

**Root cause (fixed 2026-11-11):** the 14H 3-byte pointer form left the
pointer's LAST byte in W before `_pic_indirect`, which takes W = the PC
**high byte**.  With the 3-byte form that byte is `UPPER|0x80` (the ROM
marker), so `PCLATH` became 0x80 and the indirect **device call** inside
`print_string` jumped to the wrong code page -- into the string heap
allocator, where it livelocked and corrupted the heap.  The 2-byte form was
correct.  Both `_pic_indirect` call sites now stage the PC high byte (byte 1
on the 14-bit cores, byte 2 on PIC18) independent of the pointer width; see
[`../../fixes/fix_fat_ptr3_indirect_page.md`](../../fixes/fix_fat_ptr3_indirect_page.md).
The new self-enabling witness
`sims/picsim/16f18857_picsim_fat_ptr3_device_call.jal` fails on the pre-fix
binary and passes now.

**Re-run after the fix + T3 pinning:** `JALV2_FAT_POINTERS=1 make gates-l1`
is **ALL GATES PASS (728 s)**, picsim **139/139** (the two new witnesses
included), and the gate-off corpus HEX manifest stays **0 drift**.  Rollback
threshold 4 now passes; the flip decision is the owner's, on this evidence.

## Rollback thresholds (proposed)

The flip ships only if all hold:

1. **No new failure** on the corpus under gate-on (0 new, as measured).
2. **No image change outside the escaped-pointer surface**: every changed
   program has an escaped pointer (the 90), and the change is the tagged
   representation (spot review plus the fat pins' encoding arms).
3. **Code-size budget**: ≤5% on exercising programs (measured 2.58%) and
   ≤1% corpus-wide (measured ~0.08%).
4. **Every runtime witness passes under gate-on** (the fat-on L1 result).
5. `JALV2_FAT_POINTERS=0` restores the gate-off images exactly (the manifest
   is the rollback baseline), and `pragma target fat_pointers` overrides the
   environment per program.

Any failure of 1, 2 or 4 is a no-flip.  The owner decides at the phase
boundary; precedence stays **pragma > env > default**.
