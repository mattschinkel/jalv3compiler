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
  (897 s) with hex manifest 0 drift. Committed 2026-11-11 (`522468f`) with
  both tracked binaries; the `Commit:` headers in the E-series fix docs are
  still `(pending)` and want backfilling, as does the L3 line in
  `fixes/fix_whereis_runtime_slot_fat.md`.
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

## U3 residual — the pointer-bind registry (`ptr_binds`)

- **What:** Move the pointer-bind target and its escaped bit onto the object
  model and delete `pic_var.ptr_binds`, the twin of the pair-registry
  deletion U3 landed.  `pic_var_pointer_bind_record` still appends
  `(pointer, target, escaped)` triples at every pointer assign; the escaped
  bit is read by `pic_fat_bind_forced()` (`src/libpic12/pic_op_ptr.c:80`)
  and by the classic bank-spanning refusal
  (`pic_var_pointer_bind_check_escaped`, `src/libpic12/pic_var.c:684`).
- **Why:** it is the last file-static registry the plan path reads, and U3's
  close-out defers it by name ("`ptr_binds` stays persistent (its refactor
  was deferred in the plan)", `fixes/fix_u3_pair_attach.md`).  Under the fat
  engine the bind decision already comes from the `BIND` recipe rows, so the
  registry is now a fallback plus the classic-only refusal.
- **Pros:** completes the deletion arc U3 started and removes cross-pass
  state from the pointer path.
- **Cons / ordering:** the classic half of the refusal returns early when the
  fat engine is on, so that half disappears entirely with the
  `JALV2_FAT_POINTERS=0` rollback switch (owner-deferred to the release
  after this one).  Porting only the fat-side escaped bit is possible now;
  doing both at once is cheaper AFTER the switch is removed.
- **Context:** `fixes/fix_u3_pair_attach.md` "Deferred / open";
  `src/libpic12/pic_var.c` (the registry and its accessors, ~600-730);
  consumers `src/libpic12/pic_op_ptr.c:80` and `src/libpic12/pic.c:6813`.
- **Effort:** human ~3-5 d / CC ~1-2 d (registry-scale, not a linkage edit).
- **Priority:** P3.
- **Depends on:** none; the rollback-switch removal is the natural moment
  (so the classic half is deleted rather than ported).

## Fat pointers — F5 (DONE 2026-11-11; S3 split out below, `=0` retirement deferred)

- **What:** every F5 surface except the G3 slot identity (tracked as the S3
  residual below) is closed and the **default flip landed
  2026-11-11** (`fixes/fix_fat_default_flip.md`): `JALV2_FAT_POINTERS`
  defaults **on**, precedence is pragma > env > on, `JALV2_FAT_POINTERS=0`
  still restores the classic images for one release (one `JALV2-NOTICE` per
  compile) and `pragma target fat_pointers off` is the per-program opt-out.
  The fat default-flip blocker landed 2026-11-11: the 14H 3-byte pointer
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
  (pointers auto-dereference; `pic_fat_cmp` stays a harness rule), the
  survey's pointer-cast surface is "sound / not reachable" (§1.3), and the
  string-heap append, walk-path null guard and code-size/docs items all
  landed.  **Nothing on F5 is open**; the one deferred piece is the
  rollback switch below.
- **Pros:** Ships the capability by default; one less env knob.
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
- **Effort:** none for F5; the `=0` retirement is part of the next release's
  switch removal.
- **Priority:** P3 (record; nothing to schedule here).
- **Depends on:** nothing.

## S3 residual — fat slot VALUE for array pointees (G3 identity) — DONE 2026-11-14

- **Status:** LANDED — `fixes/fix_fat_slot_array_identity.md`.  A pointer
  slot bound to an ARRAY now stores the ordinary fat pointer value
  `(id(big), offset)` under the engine (design D4), so a **loaded** pointer
  (`p = pa[0]`) carries the object identity and `p[i]` reuses the O(1)
  dispatch.  The front-end offset split (`baseofs2` + `VALUE_FLAG_SLOT_POINTEE`)
  separates the slot address from the pointee subscript; the gate-off
  physical path re-folds them and stays byte-identical (HEX manifest
  **0 drift, 2940 identical**).
- **What (for the record):** a pointer slot bound to an ARRAY (`pa[0] is big`)
  stored a physical address.  `pa[0][i]` worked (the runtime-subscript
  slice), but a loaded pointer carried no identity, and a direct `pa[k][j]`
  with `k>0` stayed off by the slot stride.  Two front-end facts blocked the
  fat-slot value: a subscript chain carried ONE base offset, and
  `p = pa[0]` lowered as `&pa[0]`.
- **Why:** completes G3's third surface ("pointer-to-ARRAY slot identity")
  and closes the documented `k>0` gap for the loaded-pointer form.
- **Context:** design memo
  `docs/investigations/fat_slot_g3_design_20261111.md` (D4/D5 + the
  measurement arms); landed slice `fixes/fix_fat_slot_array_subscript.md`;
  witnesses `sims/picsim/16f18857_picsim_pointer_slot_array_runtime.jal`
  and `sims/gpsim/16f887_gpsim_pointer_slot_array_runtime.jal` (loaded-pointer
  arm added); pin `unit-tests/test_fat_pointer_deref.sh` (G3 S3 encoding
  refreshed); access-matrix/coverage baseline refreshed (22/29 rows —
  `NOT_FAT_BIND` is shadowed by `FAT_BIND` under the engine, the intentional
  D4 flip).
- **Effort:** human ~1 wk / CC ~1-2 d.
- **Priority:** P2.

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

## `WHEREIS` of a runtime subscript silently drops the offset (DONE 2026-11-14 — refused, `fixes/fix_whereis_runtime_subscript.md`)

- **What:** `c = whereis(big[i])` with a runtime `i` returns the array BASE.
  On 16F18857 with `big[200]` at 0x22, `i = 100` emits `movlw 34; movwf c`
  and nothing else, so `c` is 0x22 for every `i`.  Pick one: refuse the shape
  with one error, or add the index at run time after the chunk resolution.
  The user documentation (`doc/dita/jalv3/operators.dita` note 6) defines
  WHEREIS for a variable, a procedure/function, a label and a constant
  array — a subscripted element is not in the list; the only element shape in
  the tree is the constant-offset `whereis(buffer[1])` at
  `unit-tests/syntax_test.jal:453`.
- **Why:** a silent wrong address.  `whereis(big[i]) - whereis(big[0])` is 0
  for every `i`, and a program that derives a pointer from it reads the wrong
  byte.  This is NOT the runtime-slot form fixed 2026-11-12
  (`fixes/fix_whereis_runtime_slot_fat.md`, which is `WHEREIS(pa[k])` on a
  pointer slot and resolves the fat tag); this is a plain array with a
  runtime index.
- **Pros:** Closes the last silent `WHEREIS` shape; the refusal variant is a
  small pin.
- **Cons:** Supporting it needs the runtime index added AFTER the chunk
  resolution, a different shape from the other walk sites.
- **Context:** 12-line probe on 16F18857 (compiled 2026-11-12); the const
  subscript is chunk-resolved through `pic_var_chunk_addr_resolve` in
  `src/libpic12/pic_op_assign.c`.  The address-report item below landed
  2026-11-14: a refused runtime subscript returns before the emit site, so it
  prints no `whereis:` line (the error is the report).
- **Effort:** human S / CC ~1 h (refuse) | ~3 h (support).
- **Priority:** done (refused — the small variant; support deferred).
- **Depends on:** none.

## Grown dynamic-string element read on the classic 14-bit core

- **What:** A constant-index read of a dynamic string that has GROWN past its
  first heap chunk (`last = s[79]` after 80 appends) stalls on classic 14.
  The read emits `base + 6 + index` and a chunk walk over `_string_heap`; the
  defect is not yet localized, so the first deliverable is a witness that
  reproduces it and names the stage that stalls.
- **Why:** recorded as a residual in `fixes/fix_string_heap_append.md`
  (2026-11-11) and it reproduces with the pre-fix binary, so it is a
  pre-existing defect rather than a regression from the growth policy; it
  had no ticket until now.
- **Pros:** A dynamic string becomes safely indexable after any growth, which
  is the obvious next thing a user does with one.
- **Cons:** Measure first: the stall may sit in the caller rather than the
  read path, in which case the deliverable is a witness plus a record, not a
  fix.
- **Context:** "Residual" in `fixes/fix_string_heap_append.md`; the same
  section records that `pragma string alloc best_fit` still only affects the
  exact request.  Heap growth itself is fixed (2026-11-11).
- **Effort:** human ~1 d / CC ~2-3 h.
- **Priority:** P3.
- **Depends on:** none.

## Full front-end IR — one accepted note (Phases 3/4/7 done)

- **What:** Phases 3 and 4 landed (the block-dup clone at `2e65a20`, the
  phase-4 measured decision memo).  **Phase 7 landed 2026-11-12**
  (`fixes/fix_clone_eligibility_marker.md`): the dead
  `body_pos`/`body_pos_valid` marker is gone (its only reader was the
  clone-eligibility test; the plan's `block_ct > 0` replacement is
  unusable because the block count is only generated by the -dump path,
  so eligibility stays on the DEFINED-body predicate), the stale comment
  and the clone DESCRIPTION are corrected, `JALV2_PROC_ELIG=1` exposes
  the decision, and a new pin covers forward-declared/defined, guarded,
  empty and call-before-body cases.  Remaining note: the accepted
  per-formal clone mask width (64-bit, ~31 array formals) stays recorded
  as unwitnessed.
- **Why:** The original Phase 3/4 work is already in the tree; the cleanup
  is in, and the only thing left is the accepted note below.
- **Pros:** Removes the last stale state on the retired re-parse.
- **Cons:** None; bookkeeping.
- **Context:** Plan Phase 7; `docs/investigations/full_ir_phase4_decision_20261107.md`;
  `fixes/fix_clone_eligibility_marker.md`.
- **Effort:** human ~2-4 h / CC ~1 h — either write the ~31-formal clone
  witness that drives the mask width, or retire the note.
- **Priority:** P3.
- **Depends on:** none.

## Access-recipe adoption on the 32-bit back ends (DONE 2026-11-12 — `fixes/fix_access_recipe_32bit_families.md`)

- **What:** Phase 8.  **Landed 2026-11-12:** the PIC32 pointer-WRITE family
  and the ESP32 deref-ADDRESS family (shared by its read and write paths)
  now go through the libcore `access_recipe` dispatcher, beside the
  existing PIC32 pointer-read spike; the PIC32 audit checks both tables.
  The 32-bit family inventory (grep-audited): **pointer read**, **pointer
  write**, **deref/array address**, **pointer-slot bind**.  The classic-only
  families (assign-class, addr-of-slot, slot shapes, FSR walk/dynamic/bind)
  do not exist in the mips/esp emitters (no FSR or chunk machinery; the
  grep shows no `FSR_*`, `BIND_DEST` or `ADDR_OF` sites in
  `mips_op.c`/`esp_op.c`), so they are recorded N/A rather than dropped.
  **Also landed 2026-11-12:** the neutral recorder
  (`src/libcore/access_record.{c,h}`, ENG A4) that the classic recorder
  now bridges (CSV byte-identical); PIC32/ESP32 emit their family rows;
  `run_plan_gate.sh --backend pic32|esp32` with `plan-baselines/pic32.txt`
  (116 rows) and `plan-baselines/esp32.txt` (79 rows), the DX F5 failure
  contract and `plan_dupcheck.py --key-fields`.  The pointer-slot BIND
  family is NOT a family on the 32-bit back ends (bind is the generic
  pointer-value store there); QEMU coverage is the existing pointer/slot
  witnesses, which the backend gates compile and pin.
- **Why:** E3's neutral dispatcher is the shared route; the classic table
  stays classic-only.
- **Pros:** One decision table per back end, with the same row discipline
  (mask rule audited) as the classic family.
- **Cons:** The recorder is the orthogonal half; without it the per-backend
  plan-diff subset cannot exist.
- **Context:** `fixes/fix_access_recipe_32bit_families.md` (the A/B:
  246/246 images byte-identical across sims/pic32 + sims/esp32);
  `src/libcore/access_recipe.h`; the spike record in E3.
- **Effort:** delivered 2026-11-12; the phase doc states nothing in the
  Phase 8 bullet list remains open.
- **Priority:** P3.

## Full classic-family recipe interpreter (closed: NO-GO, measured)

- **What:** Phase 6 (the plan's P3): separate the decision table from a
  row-keyed step table over the emitter primitives, with a dual-path
  comparison mode.  **Milestone measured 2026-11-12 and stopped at the
  plan's kill point**: the pointer family's 14 action uses need **+351
  lines** to replace **2** of them, and the first family already needed a
  conditional step (`LINEAR_IF_OPEN`) because the OPENARRAY fact is not in
  the recipe key.  The read family's bodies are entangled with temp/label/
  value state and each row's body is unique, so a step list relocates
  code rather than deleting it.
- **Why:** the plan's C12 payoff metric: the interpreter must shrink the
  special cases to earn its complexity; it did not.
- **Context:** `fixes/fix_access_interpreter_no_go.md` (the prototype, the
  dual-path/fault-hook proof, and the measurements).  Reopen only after
  the recipe KEY carries the deciding facts (OPENARRAY, pointee
  mutability) so a row's behavior is a function of the key alone.
- **Priority:** P3 (closed).

## ISR coverage gaps (closed 2026-11-12 — software half landed, hardware excluded)

- **What:** nothing is open.  (a) the external-pin / UART-source gap is
  closed by **Phase 4 (2026-11-12)**: UART-source bodies for ESP32
  (`esp32_body_isr_uart.jal`) and PIC32 (`32mx270f256b_isr_uart.jal`), an
  ESP32 body on CPU interrupt 5 (`esp32_body_isr_cpu5.jal`), and the
  negative controls as `unit-tests/test_isr_source_wiring.sh` (matrix route
  mismatch, muted source, priority 0 -- each must NOT dispatch); the classic
  target already had the external-pin INT0 body
  (`16f18857_picsim_isr_int0.jal`).  (b) ESP32 levels 2..7 stay a refused
  shape **by design** (`pragma interrupt 6` and up is one error, pinned by
  `test_esp32_isr_cpu_int.py`).  (c) the silicon half is excluded under the
  owner's hardware-ISR exclusion, so it is not backlog work.
- **Why:** QEMU diverges from silicon, which is why the software half needed
  real bodies rather than compile-only coverage.  Already landed before
  Phase 4: baseline single-source bodies (`esp32_body_isr.jal`,
  `32mz1024efh064_isr.jal`), the dual-source body
  (`esp32_body_isr_dual.jal`, TIMG0+TIMG1), PIC32 multi-vector
  (`32mz1024efh064_mvec.jal`), PIC32 priorities
  (`32mx270f256b_isr_priority.jal`, `32mz1024efh064_isr_priority.jal`) and
  `pragma interrupt fast` (`32mz1024efh064_isr_fast.jal`).
- **Pros:** The benchmarked handoff pattern (ISR bumps `tick`, main
  observes, parks on `done`) is now covered on both targets.
- **Cons:** The silicon half stays untested by decision, not by omission.
- **Context:** gap list in `/home/jal/AGENTS.md` ("Interrupt (ISR) test
  coverage"); `fixes/fix_isr_source_bodies.md` records the Phase 4 landing
  and the excluded hardware half.
- **Effort:** none.
- **Priority:** P3 (record).
- **Depends on:** nothing; (c) is excluded by owner decision.

## ESP32 second core — SMP dispatcher (Phase 9, silicon-validated)

- **What:** the owner chose the **full SMP dispatcher** branch.  **Landed
  2026-11-12 (commit `8eb7100`):** the design memo (register/bit table for the DPORT APPCPU
  block and the release order, the shared-DRAM ownership memory model,
  the cooperative-SMP dispatcher design) at
  `docs/investigations/esp32_second_core_design_20261112.md`; the enabling
  surface (`pragma target second_core on|off`, `JALV2_SECOND_CORE=1`
  override with the strict on/off rule, off by default, an invalid pragma
  value a compile error); the DPORT release sequence and the APP entry
  body in the ESP32 emitter; and the QEMU runner `--smp`/`--cpu-count`
  (validate-script `--smp` + a per-witness `smp:` key, `run_hex(cpu_count)`).
  The single-core suite is byte-identical (246/246 32-bit images).
- **SILICON PASS 2026-11-12 (boot):** on the owner's ESP32 the APP core
  boots through the DPORT release (`SC-BOOT` then `SC-APP-OK`).  The
  correct map is CTRL_A[0]=RESETTING, CTRL_B[0]=CLKGATE_EN,
  CTRL_C[0]=RUNSTALL, CTRL_D=BOOT_ADDR (no OVLD bit), plus clearing
  RTC_CNTL_SW_CPU_STALL[7:0]; two codegen bugs were fixed on the way
  (the entry label was never allocated; the entry body was emitted after
  the fixup pass).
- **SILICON PASS 2026-11-12 (SMP dispatcher):** the SMP witness (then
  `sims/esp32/second_core/esp32_silicon_second_core_smp.jal`, renamed to
  `sims/esp32/esp32_body_smp_dispatch.jal` in `bf09f94`) prints `SMP-BOOT` then `SMP-OK`,
  stable over repeated resets: the APP core claims the started `painter`
  task with `S32C1I`, runs it on its own stack, and the PRO core resumes
  main, which sees the shared counter pass 1000.  Four measured causes
  were fixed on the way (recorded in `fixes/fix_smp_dispatcher.md`):
  the DPORT release preceded the `.bss` zeroing (the APP core claimed
  staging garbage as a task PC); both cores shared one stack (now a
  private 4 KB `_esp32_app_stack`); `S32C1I` against DRAM does not store
  at the reset `ATOMCTL` 0x28 or at RCW 0x29, it needs 0x2A; and the
  claim's `BNE` was emitted as `bne a10, a0` (the register-branch
  encoder takes `(t, s)`), so every successful claim looked lost.
  Main's resume PC is now stored into slot 0 when the core has no
  current slot.  **At that point QEMU did not run the DPORT-released core**
  even though `-smp 2` reported two CPUs (superseded by the VALIDATION
  bullet below, which found the cause in `fix_smp_dispatcher.md`); the
  witnesses were parked at `sims/esp32/second_core/` meanwhile.
- **VALIDATION 2026-11-12 (C7):** the three SMP bodies are in the ESP32
  suite (`esp32_body_smp_{dispatch,contention,control}.jal`, `smp: 2`,
  UART + `v_smp_ok` expectations).  QEMU runs the DPORT-released core
  (the earlier "it never runs" conclusion was an artifact of the bugs in
  `fix_smp_dispatcher.md`), and the contention body asserts both cores
  claimed tasks via `_task_run_core[0/1]`, each task ran its exact count,
  its procedure ran each iteration, and no duplicate instance was seen;
  the control body checks cross-core pause/resume/kill/restart.  Silicon
  passes all three, stable over resets.
- **SURFACE 2026-11-12 (JALv3 multi-core):** `pragma target enable_cores N`
  (default 1, chip-limited: 3 on ESP32 or 2 on a PIC is a compile error)
  replaces `pragma target second_core` and `JALV2_SECOND_CORE`, both
  removed; `pragma task` is removed too (slots are sized from the task
  declarations; `-task` only pre-sizes). `_task_start core(<expr>) task()`
  (clause before or after, constant or runtime) pins a task; the
  dispatcher skips a slot pinned to another core. A runtime pin outside
  0..N runs the task on any core and raises the new
  `_ERROR_SOURCE_CORE` / `_core_error` runtime error through
  `_error_hook` (`demo_lib/error_hook.jal` prints `[core out of range]`).
  `_task_core(task())` reports the core that last ran a task.
  `unit-tests/test_cores_pragma.sh` and `test_task_yield_scope.sh` pin the
  surface; the new suite bodies `esp32_body_smp_affinity.jal` and
  `esp32_body_smp_affinity_error.jal` pass under QEMU and on silicon
  (`SMPA-OK`; `SMPE-BOOT` / `[core out of range]` / `SMPE-OK`).
- **DEFAULT DECISION (C7):** stays opt-in.  It changes every image
  (entry, 4 KB APP stack, DPORT setup) and buys nothing for programs
  without tasks; the suite now protects it, so a default flip is possible
  but not useful.  Documented in `doc/dita/jalv3/second_core.dita`; the
  `--features` report shows the switch.
- **Remaining:** preemption stays out of this cut (it needs a per-core
  timer interrupt and context save).
- **Priority:** P3.
- **Depends on:** nothing; silicon-validated 2026-11-12 on the owner's board.

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

## Module / separate-compilation architecture

- **What:** Design the module mechanism (explicit interface, pre-compiled
  once, linked into the image) as a target-portable feature, with a plan doc
  covering interface/implementation split, per-target ABI and calling
  convention, precise rebuild invalidation, and byte-identical module-cached
  vs uncached output.  Classic cores never opt in (the hash-keyed parse
  cache this item used to name no longer exists in `src/`).
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

## `whereis` address reporting in `-explain-access` (DONE 2026-11-14 — `fixes/fix_whereis_explain_access.md`)

- **What:** `-explain-access <file>:<line>` now reports a WHEREIS resolved on
  the line, not just the access rows.  The new `whereis:` line carries the
  target name, the object-relative offset, the physical address and the
  resolution path: `target=big ofs=100 addr=188 (0xbc) segments=3
  resolve=chunk` (or `segments=1 resolve=linear`).  It prints in emission
  order beside the line's access rows.  The hook is in
  `pic_assign_from_value` after `pic_var_chunk_addr_resolve`, pass 2 only;
  the last resolution wins so a layout trial cannot leak its address.
- **Why:** `whereis` already resolved the physical address of a chunked byte
  (`fix_whereis_chunk_addr.md`) but the value only appeared in the image;
  a line containing only a WHEREIS reported `no access`.  One command now
  explains where the object is and how the line will access it.
- **No fabricated recipe:** WHEREIS is not an emitted access and resolves
  through the chunk map, not the plan, so it has no recipe row.  The address
  line is the join; the access rows are the recipe half and are unchanged.
- **Context:** `fixes/fix_whereis_explain_access.md`; pin arms in
  `unit-tests/test_access_cli.sh` (chunked + linear, both fail pre-fix);
  images byte-identical against the HEAD binary.  A runtime-slot
  `WHEREIS(pa[k])` has no compile-time address and prints no line.
- **Effort:** delivered (~1 h).
- **Priority:** done.

## A3 — one placement policy (DONE 2026-11-13 — `fixes/fix_pic_mem_place.md`)

- **What:** the placement subsystem now has **one entry**, `pic_mem_place()`:
  a request carries a policy -- SINGLE (one window, with an optional defer),
  FRAME (one contiguous run) and GROUP (window -> shared -> segmented);
  deferral is a status (DEFERRED), never a policy.
  Every class goes through it: volatile scalars, ISR locals, indirect and
  re-entrant frames, leftover variables, `-no-var-reuse`, the normal alias
  groups, and the emit-time scratch.  The group core was extracted verbatim
  into `pic_group_place()` first (byte-identical), then routed through the
  entry; the scatter's manual "need vs largest run" pre-check and its
  duplicate defer code are gone, and `leftover` now uses the same search (a
  leftover that cannot fit one window is segmented instead of refused).
- **Why:** seven searches with seven failure behaviours are how the silent
  `VARIABLE_BASE_UNKNOWN` bug happened (two paths allocated without checking
  the result).  The guards closed that class; the entry makes it structurally
  impossible for a class to grow its own variant again.
- **Behavior:** byte-identical -- L2 HEX manifest 2940 identical / 0 changed,
  access matrix 0 drift.  The only programs that can change are ones that
  used to refuse (leftover / fragmented single-window objects), which now
  defer and are placed.
- **Descriptor:** `fixes/fix_pic_mem_descriptor.md` -- `pic_mem.h` now owns
  the request (`pic_mem_request_t`: need/padding/slots/stride/chunkable/
  bit-array/linear-only/pointer-target/virt/region/policy) and the result
  view; the adapters read the variable once, and the search reads only the
  request (grep-verified: zero variable-fact reads in the group placement).
  Byte-identical (L2 2940 identical / 0 drift).
- **Residual:** the `at` pin pass reserves bytes directly (no search); the
  commit target is a variable/group (every placed object is
  variable-backed today, including compiler scratch); the request is a pic12
  backend API, and the 32-bit allocators remain separate by design.
- **Context:** `fixes/fix_pic_mem_place.md`, the frame/walk fix docs,
  `docs/investigations/unified_allocator_20261108.md` (A3).
- **Facade finish** (`doc/designs/finish-placement-facade.md`): a review of
  the landed A3 found the architecture correct but the facade unfinished --
  dead DEFER/PINNED policies, hand-rolled FRAME requests, a three-way
  element-stride rule, and `req->region` dead for GROUP.  Step 1 (drop the
  dead policies, factor the inline defer into `pic_mem_place_defer()`),
  step 2 (add `pic_mem_request_frame()`, route the three FRAME call sites
  through it), step 3 (one `pic_mem_stride_get()` rule) and step 4 (drop
  the dead region field) all landed 2026-11-13 byte-identical.
- **Priority:** done; facade finished.

## A7 — the always-on allocator report (DONE 2026-11-14 — `fixes/fix_alloc_report_a7.md`)

- **What:** the allocator now reports its own usage.  One static helper
  `pic_mem_usage_get()` computes data/shared used/free/largest; the end-of-
  compile report (`pic_blist_info_log`) prints an INFO `RAM:` line, and the
  always-on audit summary reads the same helper, so the two agree by
  construction.  A placement-invariant violation is now an ERROR (U4 "a
  violation is an internal error"): it fails the compile instead of warning.
- **Why:** closes the A7 rung of `docs/investigations/unified_allocator_20261108.md`;
  a clean compile could not answer "what did you place, where".
- **Behavior:** image byte-identical (the report is console output); only the
  fault path changed.  Pin `alloc_segment_audit` arms 7/8 assert the error
  verdict and the RAM/audit agreement.
- **Residual (deferred):** the ERR escape hatch (`JALV2_ALLOC_AUDIT=warn`
  demote), and piece-3 model purity (the coverage check still reads
  `pic_variable_storage_need_get` instead of the object view).
- **Context:** `fixes/fix_alloc_report_a7.md`,
  `doc/designs/finish-allocator-report.md`,
  `doc/designs/finish-allocator-report.ceo-review.md`,
  `docs/investigations/unified_allocator_20261108.md` (A7).
- **Priority:** done; residual deferred.
