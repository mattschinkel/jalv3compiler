# TODOS

Live, prioritized backlog for jalv3. Sources: the one-access-model design
(`doc/designs/one-access-model.md`) and the open items carried out of the
workspace backlog (`/home/jal/AGENTS.md`, "TODO / pending work" — that
section keeps the detailed history and links; the open entries are mirrored
here). Each entry names its evidence and the fix/investigation doc.

## Land the E1+E2+E3 batch (DONE 2026-11-11 — commit `522468f`)

- **What:** Commit the access-model batch that is gate-green at L2 but
  uncommitted (compiler sources, the new libcore dispatcher, tests, the
  refreshed plan baseline/matrix, the tracked Linux binary), rebuild the
  tracked Windows `bin/beta/jalv3-x86-64.exe`, and run the L3 landing gate
  (second QEMU suite, ASan corpus, sample-kit sweep) before shipping.
- **Why:** The work exists only in the working tree; the tracked `.exe` is
  stale relative to this source, and L3 has not run for this batch.
- **Pros:** Makes the batch shippable and the tracked binaries consistent.
- **Cons:** None; it is the release step, not new work.
- **Context:** E1-E3 fix docs are in `fixes/`; final L2 was ALL GATES PASS
  (897 s) with hex manifest 0 drift. Needs the user's go-ahead to commit.
- **Effort:** human S / CC ~1h (plus the L3 run).
- **Priority:** P1.
- **Depends on:** nothing; the recipe-row coverage gap below does not block
  landing (the gate reports it).

## U3 registry boundary (DONE 2026-11-11 — `fixes/fix_u3_pair_attach.md`)

- **What:** The open-array (actual -> formal) pairs now live on the CALL
  branch command (`cmd_branch_pair_t`: dup/free/dump, the record rules
  attached in pass 1). The capture pass builds its per-formal aggregates
  from the IR in one walk, the call-site descriptor store reads the call
  being emitted, placement-time propagation walks the same IR,
  string-view-fed is a persistent per-formal aggregate, and the pair
  registry struct/accessors are deleted. The one-consumer
  `pic_var_openarray_pair_formal_runtime_get` predicate is deleted; the
  plan fill asks `pic_var_openarray_formal_map_get` directly.
- **Why:** The pair was cross-pass state (recorded in pass 1, read in pass 2)
  and a keyed mailbox between two ends of one call emission; §1d of
  `docs/investigations/u3_registry_boundary_20261109.md` withdrew the
  per-call context as unimplementable and named the IR attachment as the
  viable direction.
- **Pros:** The last parallel lookup machinery on this path is gone; the
  store cannot match another call site's pair.
- **Cons / deviations recorded:** the branch payload gained a lifecycle
  (deep-copied in `cmd_branch_dup`, freed in `cmd_branch_free`);
  `pic_var_chunk_map_propagate` walks the IR rather than "staying on a
  per-formal aggregate" (the relation it needs is actual -> formal, which
  only the IR carries); `pic_var_pointer_bind_walk_target` remains
  (`ptr_binds`'s refactor is deferred per the plan's A6).
- **Context:** the pre-coding call-shape matrix is
  `docs/investigations/u3_call_shape_matrix_20261110.md` (1,443 programs,
  76,475 calls, 0-3 pairs/call); the acceptance comparator re-derives the
  expected pairs per call and the `u3_pair_attach` pin proves it non-vacuous
  with drop/alter fault hooks; the corpus (2,940 images) and all 303
  classic sims are byte-identical.
- **Effort:** human ~2-3 wk / CC ~3-5 d (delivered).
- **Priority:** P1.
- **Depends on:** nothing.

## Fat pointers — F5 hardening and the default flip

- **What:** F5's remaining work (ASan, code size, docs) plus the string-heap /
  pointer-cast surface.  The **default flip landed 2026-11-11**
  (`fixes/fix_fat_default_flip.md`): `JALV2_FAT_POINTERS` defaults **on**,
  precedence is pragma > env > on, `JALV2_FAT_POINTERS=0` still restores the
  classic images for one release (one `JALV2-NOTICE` per compile) and
  `pragma target fat_pointers off` is the per-program opt-out.  The fat
  default-flip blocker landed 2026-11-11: the 14H 3-byte pointer
  form staged the ROM/UPPER byte as PCLATH for `_pic_indirect`, so an
  indirect device call jumped to the wrong page
  (`fixes/fix_fat_ptr3_indirect_page.md`); the fat-on L1 is green.  The
  walk-path null guard landed 2026-11-11
  (`fixes/fix_unbound_walk_guard.md`; F5 §4).  The chunked-heap append
  growth landed 2026-11-11
  (`fixes/fix_string_heap_append.md`; the F5 §1.2 item).  Strict `JALV2_*`
  switch parsing + `--features` landed 2026-11-11
  (`fixes/fix_env_switches.md`).  CLI validation (ENG S1) landed 2026-11-11:
  the `-explain-access` line must be 1..65535, the dump path non-empty, an
  unreadable source fails without an image and an unwritable dump fails
  without touching a pre-existing output
  (`fixes/fix_access_cli_validation.md`, pin `test_access_cli.sh`); the
  mingw toolchain version is recorded in
  `bin/beta/jalv3-x86-64.exe.provenance`.  G3 slot surfaces: **S1 (runtime element offset) and S2 (runtime slot
  index) landed 2026-11-11** (`fixes/fix_fat_slot_runtime_ofs.md`,
  `fixes/fix_fat_slot_runtime_index.md`); **S3's runtime pointee subscript
  landed** (`fixes/fix_fat_slot_array_subscript.md`).  S3's fat slot VALUE
  for array pointees (identity for a loaded pointer) is parked on a
  front-end split of the slot offset from the pointee subscript; the
  design and touchpoints are in
  `docs/investigations/fat_slot_g3_design_20261111.md`.
- **Why:** F2d (the O(1) `_obj_desc[]` dispatch) and the dynamic fat
  subscript landed 2026-11-09; the design's "mixed linear/fat comparison"
  and "offset overflow" edge cases turned out unreachable through JAL
  (pointers auto-dereference; `pic_fat_cmp` stays a harness rule), so what
  is left is hardening and the surface, not those cases.
- **Pros:** Ships the capability by default once hardened; one less env knob.
- **Cons:** The flip changes images for the 90 corpus programs with escaped
  pointers; the manifest was refreshed with the flip, and the switch is
  removed after one release.  **Retiring `=0` and deleting the classic-mode
  branches is deferred by owner decision (2026-11-11) to the release after
  this one**; every engine-off branch is still live through the rollback,
  so there is no dead classic code to delete before then.
- **Context:** `docs/investigations/fat_pointer_f5_survey_20261109.md`; F2d
  record `fixes/fix_fat_pointer_f2d_table.md`; flip measurement
  `docs/investigations/fat_default_flip_measurement_20261110.md`. The 14H
  3-byte automatic selection stays parked (size is fixed at parse time; the
  3-byte default removed the motivation) — reopen only if a pre-parse pass
  is scheduled.
- **Effort:** human ~1 wk / CC ~1 d for the remaining surfaces.
- **Priority:** P2.
- **Depends on:** nothing; the owner's flip decision was made and landed.

## S3 residual — fat slot VALUE for array pointees (G3 identity)

- **What:** a pointer slot bound to an ARRAY (`pa[0] is big`) stores a
  physical address today.  `pa[0][i]` works (the S3 runtime-subscript
  slice), but a **loaded** pointer does not carry the object identity, and
  a direct `pa[k][j]` with `k>0` stays off by the slot stride.  Two
  front-end facts block the fat-slot value:
  1. a subscript chain carries ONE base offset (`k*psz + j` for `pa[k][j]`),
     so the emitter cannot separate the slot address from the pointee
     subscript;
  2. `p is pa[0]` lowers as "the array the slot points to" (`&pa[0]`), not
     as a copy of the pointer stored in the slot.
  Deliver the front-end offset split (a second offset on the subscripted
  value view), then make an array-pointee slot value fat under the engine
  (design D4: the loaded value is TAG_DISPATCH, never the compile-time bind
  walk).
- **Why:** completes G3's third surface ("pointer-to-ARRAY slot identity")
  and closes the documented `k>0` gap; the current physical-address slot is
  correct only while the pointer is used through the slot itself.
- **Pros:** Removes the last known coordinate hole in the fat pointer
  model; makes a loaded pointer composable (subscript, pass, copy).
- **Cons:** The offset split touches every subscript lowering, so the
  change is front-end-wide: the corpus HEX manifest must stay 0 drift for
  programs without a pointer-array subscript, and the access matrix rows
  for the slot shapes may shift (intentional refresh).
- **Context:** design memo
  `docs/investigations/fat_slot_g3_design_20261111.md` (D4/D5 + the
  measurement arms); landed slice `fixes/fix_fat_slot_array_subscript.md`;
  witnesses `sims/picsim/16f18857_picsim_pointer_slot_array_runtime.jal`
  and `sims/gpsim/16f887_gpsim_pointer_slot_array_runtime.jal`; pin
  `unit-tests/test_fat_pointer_deref.sh` (the G3 control arms).
- **Effort:** human ~1 wk / CC ~1-2 d.
- **Priority:** P2.
- **Depends on:** nothing (independent of the classic-code removal).

## Recipe-row coverage fixtures (A12)

- **What:** Gate fixtures for the recipe rows no corpus program reaches.
  **Phase 3 landed 2026-11-12**: rows **17** (FSR walk, holed) and **20**
  (FSR dynamic, classic string-view) now have witnesses
  (`sims/picsim/16f18857_picsim_whereis_slot_runtime.jal`,
  `sims/gpsim/16f887_gpsim_string_arg_view_in.jal`) and the baseline pins
  **23/29**.  Rows **1, 25** were already covered.  The six remaining rows
  are audited in `fixes/fix_recipe_row_fixtures.md`: **3, 4, 7, 9** are
  structurally unreachable through the current routers (a lookup write is
  refused before emit; the ptr-class router always sets PTR_LOOKUP at
  PTR_WRITE and the read rows 7/9 need a key the router never produces);
  **16** needs pointer-array formals, which the grammar lacks (only
  `pic_fsr0_add_dynamic_index` on a runtime-map variable would reach it);
  **22** has no constructible program (every classic string-view formal
  takes the walk branch, i.e. the FSR_DYNAMIC_SV row).  Retiring the rows
  is unsafe while their actions stay semantically load-bearing
  (FSR_BIND_SV drives the classic view base add), so they stay uncovered
  and pinned as the gap.
- **Why:** A12 asserts every recipe row is exercised; the gate pins the set
  that is and prints the gap.
- **Pros:** Closes the exercisable part of the A12 obligation and records
  why the rest are unreachable.
- **Cons:** The residual six are unreachable only while the routers and the
  grammar stay as they are; a pointer-formal or router change must revisit
  rows 3/4/7/9/16/22.
- **Context:** `unit-tests/plan-matrix/coverage.txt` pins the current set and
  `run_access_matrix.sh --check` fails on regressions;
  `fixes/fix_recipe_row_fixtures.md` records the row-by-row audit.
- **Effort:** human ~1d / CC ~2h for the residual if the grammar changes.
- **Priority:** P3.
- **Depends on:** none; E2's row identity does not affect it.

## WHEREIS of a runtime pointer slot under the fat engine

- **What:** `WHEREIS(pa[k])` with a runtime `k` reports the RAW stored
  pointer when the engine is on: the low byte is the object-relative offset,
  not the physical address (`16f18857`: `0x64` for `big[100]`, whose
  physical address is `0xC0`).  With `JALV2_FAT_POINTERS=0` the same
  expression reports the physical address, and the const-index form
  (`WHEREIS(pa[1])`) reports the physical address under BOTH modes, so the
  runtime path is the odd one out.  Fix: resolve the loaded tag through the
  descriptor (`_seg_addr`) before storing the WHEREIS result, the way a
  deref does.
- **Why:** WHEREIS is documented to report the address a pointer targets;
  a program comparing `WHEREIS(p)` with `WHEREIS(target)` silently fails
  under the default engine.
- **Pros:** One visible inconsistency removed; the runtime and const forms
  agree.
- **Cons:** The fix extends the dispatch emitter to produce an address as a
  value (today it leaves it in FSR for the INDF access), so it is a real
  emitter change with a runtime witness per form.
- **Context:** found 2026-11-12 while building the A12 row-17 fixture; the
  probe values are in `fixes/fix_recipe_row_fixtures.md`; the fixture
  deliberately asserts only `!= 0` until this is fixed.
- **Effort:** human ~2-3 d / CC ~1 d.
- **Priority:** P2.

## Full front-end IR — residual

- **What:** Phases 3 and 4 landed (the block-dup clone at `2e65a20`, the
  phase-4 measured decision memo). The residual: delete the dead
  `body_pos`/`body_pos_valid` eligibility marker and fix the clone/comment
  text (plan Phase 7), and keep the accepted per-formal clone mask width
  (64-bit, ~31 array formals) recorded as unwitnessed.
- **Why:** The original Phase 3/4 work is already in the tree; only the
  cleanup and the accepted note remain.
- **Pros:** Removes the last stale state on the retired re-parse.
- **Cons:** None; bookkeeping.
- **Context:** Plan Phase 7; `docs/investigations/full_ir_phase4_decision_20261107.md`.
- **Effort:** human ~3-5 d / CC ~1 d.
- **Priority:** P2.
- **Depends on:** none.

## ISR coverage gaps (ESP32 + PIC32)

- **What:** (a) an external-pin (GPIO) or UART-source ISR body on each
  target; (b) ESP32 interrupt levels > 1 (the PIC32 priority case landed);
  (c) hardware evidence: the PIC32 ISR path on silicon and the real-silicon
  check of the ESP32 vector-table 0x400-alignment rule (the compile defect is
  fixed; the silicon half is open).
- **Why:** QEMU diverges from silicon; these are the remaining untested ISR
  surfaces. Landed already: baseline single-source bodies
  (`esp32_body_isr.jal`, `32mz1024efh064_isr.jal`), the dual-source body
  (`esp32_body_isr_dual.jal`, TIMG0+TIMG1), PIC32 multi-vector
  (`32mz1024efh064_mvec.jal`), PIC32 priorities
  (`32mx270f256b_isr_priority.jal`, `32mz1024efh064_isr_priority.jal`) and
  `pragma interrupt fast` (`32mz1024efh064_isr_fast.jal`).
- **Pros:** Closes the benchmarked handoff pattern (ISR bumps `tick`, main
  observes, parks on `done`) across both targets.
- **Cons:** The hardware half needs the bench boards.
- **Context:** Gap list in `/home/jal/AGENTS.md` ("Interrupt (ISR) test
  coverage"); items (b) and (c) there have since landed on the PIC32 side.
- **Effort:** human ~1-2 d / CC ~3-4 h (software half).
- **Priority:** P3.
- **Depends on:** bench access for (c).

## Hardware-bound ISR work — MZ nesting + MZ boot-flash ISR

- **What:** Reopen MZ nested/preemptible ISR (the tree was reverted; probe
  parked at `tmp/isr_nested_probe_20261103.jal`) and the MZ064 boot-flash ISR
  only with a working bench or a new chip.
- **Why:** On MZ the EIC re-takes the pending same-priority request the
  moment `EXL` clears (needs a controller-side mask), and the breadboard
  MZ064 latches a CacheErr from its defective boot-flash fetch line before
  user code runs — interrupts are impossible on that chip. The audited
  interrupt list has only UART-source (RX) ISR open besides this.
- **Pros:** Unblocks the last interrupt surfaces on MZ.
- **Cons:** Bench-bound; not observable in QEMU.
- **Context:** `docs/investigations/mz_isr_nesting_20261103.md`; the
  ErrorEPC line-map method is ready for any candidate chip.
- **Effort:** bench-bound; CC ~2 h per attempt.
- **Priority:** P3.
- **Depends on:** a working MZ bench (or a new chip).

## PIC32 bootloader written in JAL

- **What:** A jalv2 bare-metal boot-flash build: compiler startup, UART1
  transport, NVM row-writes (SYSKEY/NVMCON sequence), KSEG0 jump to the
  `pragma bootloader pic32` app; the compiler emits the paired boot flash
  directly (the vendor flow has no in-bootloader dispatch).
- **Why:** Future direction (noted 2026-10-01); when a bootloader is wanted
  it will be a JAL program, not the vendor one.
- **Pros:** End-to-end JAL PIC32 toolchain.
- **Cons:** Prerequisite: fix the PFM-layout 2x quirk first; USB HID
  transport is a much bigger lift than UART.
- **Context:** `/home/jal/AGENTS.md` ("PIC32 bootloader written in JAL") —
  the vendor-flow reverse-engineering intel is recorded in that entry (the
  linked retired doc is not in this checkout). `pragma bootloader pic32`
  already exists for the app side. The PFM-layout 2x quirk prerequisite is
  still OPEN — `fixes/fix_pic32_startup_silicon_cache.md` ("Open quirk --
  PFM layout runs 2x slow with the prologue").
- **Effort:** human ~1-2 wk / CC ~3-5 d.
- **Priority:** P3.
- **Depends on:** the PFM-layout 2x quirk.

## ESP32 second core

- **What:** Use the APP core: startup on it (own stack, entry via the ROM
  APP-core boot register), a memory-model decision (shared DRAM with
  ownership rules vs message passing), and a scheduling story (core affinity
  or an SMP-aware dispatcher using Xtensa S32C1I).
- **Why:** The ESP32-WROOM-32 is dual-core but the compiler uses one core;
  low priority (user-noted 2026-10-05) — nothing depends on it.
- **Pros:** Real parallelism for bit-banged protocol + heavy compute shapes.
- **Cons:** Large and isolated; the classic/PIC32 targets are single-core.
- **Context:** QEMU's esp32 machine emulates both cores (SMP `cpu-count`),
  so a runtime gate is possible.
- **Effort:** human XL / CC ~3-5 d.
- **Priority:** P3.
- **Depends on:** none.

## Test-suite gaps left from the SDCC survey

- **What:** Float range sweeps (an exhaustive per-type value sweep). Every
  other survey item has landed: boundary bodies on PC/ESP32 and both PIC32
  parts (`esp32_body_boundary`, `32mx270f256b_boundary_regression`,
  `32mz1024efh064_boundary_regression`), optimizer-semantics bodies on all
  three targets (`*_body_optsem` / `*_optsem_regression`), CRC16+sort
  known-answer bodies on all three, the SHA-256 workload on 18F4620
  (`18f4620_gpsim_sha256.jal`), and the unified negative-test table
  (2026-10-17).
- **Why:** The SDCC survey adopted the matrix; the float sweeps are the one
  remaining item. (The ISR matrix it listed is tracked separately above.)
- **Pros:** Broadens the known-answer corpus per target.
- **Cons:** Much new test-body work for a low defect yield.
- **Context:** `/home/jal/AGENTS.md` ("Test-suite gap survey vs SDCC"); the
  survey document itself is not in this checkout.
- **Effort:** human ~2-3 d / CC ~1 d.
- **Priority:** P3.
- **Depends on:** none.

## Module / separate-compilation architecture

- **What:** Design the module mechanism (explicit interface, pre-compiled
  once, linked into the image) as a target-portable feature, with a plan doc
  covering interface/implementation split, per-target ABI and calling
  convention, precise rebuild invalidation, and byte-identical module-cached
  vs uncached output. Drop the hash-keyed parse cache; classic cores never
  opt in.
- **Why:** Strategic direction updated 2026-09-23 (eventual x86/x64 host
  targets); deferred by decision 2026-09-25 while correctness/backlog items
  run.
- **Pros:** Required at OS scale; keeps the small-MCU whole-program path.
- **Cons:** Very large; a design gate must pass first.
- **Context:** `/home/jal/AGENTS.md` ("Module / separate-compilation
  architecture").
- **Effort:** human XL / CC design gate ~1-2 d, implementation weeks.
- **Priority:** P3.
- **Depends on:** the original decision gate (measure a real large project).

## `whereis --recipe` output

- **What:** Add the recipe to `whereis` address reporting.
- **Why:** `whereis` already reports the physical address of a chunked byte; the recipe would explain the access that produced it.
- **Pros:** One command explains both address and access.
- **Cons:** No proven consumer yet; `whereis` resolves through the chunk map, not the plan.
- **Context:** Revisit if a user asks for it.
- **Effort:** human S / CC ~30min.
- **Priority:** P3.
- **Depends on:** none, but low value until requested.

## Full classic-family recipe interpreter

- **What:** Turn the action switches in the converted families into a true recipe interpreter.
- **Why:** The current families use recipe actions with the bodies still in code; a full interpreter is the design's end state.
- **Pros:** The decision and the emitted sequence become one data structure.
- **Cons:** Large; the action form is already plan-field-free and gate-proven.
- **Context:** The cross-target interface settled (E3 spike passed 2026-11-10); revisit when the action form starts to hurt.
- **Effort:** human XL / CC ~3-5d.
- **Priority:** P3.
- **Depends on:** none (the interface spike is done).

## ESP32 + full cross-target adoption

- **What:** Extend the shared access-recipe interface from the PIC32 pointer-read spike to ESP32 and the full operation set, including the per-backend plan-diff subset (it needs the access recorder adopted in libpic32; the spike's witness is the byte-identical image plus QEMU).
- **Why:** The accepted CEO spike passed (2026-11-10: the pointer-read table is byte-identical and the QEMU witness passes); this is the remaining cross-target work.
- **Pros:** One access model across every backend.
- **Cons:** XL; the other backends do not share the classic facts.
- **Context:** The PIC32 spike passed its byte-identical gate (2026-11-10); this is the next cross-target step.
- **Effort:** human XL / CC ~3-5d.
- **Priority:** P3.
- **Depends on:** none (the spike is done).
