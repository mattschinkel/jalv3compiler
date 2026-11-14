<!-- /autoplan restore point: "/home/jal/.gstack/tmp/autoplan/master-autoplan-restore-20261113-232512.md" -->
# Implementation plan: Finish A7 — the always-on allocator report

Branch: master
Repo: mattschinkel/jalv3compiler
Source design: doc/designs/finish-allocator-report.md (APPROVED)
Prior CEO review: doc/designs/finish-allocator-report.ceo-review.md (CLEAR)

## Implementation plan

1. Fill `pic_blist_info_log` (`src/libpic12/pic_var.c:6136`) with a data-usage line at INFO, reading only `pic_var.map`. Factor the six-number computation (data/shared used/free/largest) into one static helper used by both `pic_var_segment_audit` (`pic_var.c:5792-5806`) and `pic_blist_info_log`, so the two agree by construction. Give the line a label distinct from the existing `PIC_MSG_DATA_USED` "Data area: X of Y used" (`pic.c:3455`, which excludes pins), e.g. `RAM: data used %u/%u free %u largest %u; shared used %u/%u free %u largest %u`, and note that relationship. The call site is gated on `_code_size` present AND zero errors, and prints before the `CODE_USED`/`DATA_USED` lines; the pin asserts by format, not position. Gate: L2 HEX manifest 0 drift.
2. Promote the placement-invariant violation verdict from WARN to ERR (`src/libpic12/pic_var.c:5808`), so a violation fails the compile (non-zero exit, no image). Gate: run the full-corpus sweep (L2 + L3 sample sweep) FIRST; land the ERR only when violations == 0; if any violation appears, fix it before promoting (or defer the promotion and keep WARN). The piece-3 residual is a model-purity cleanup, not a known violation.
3. Update `unit-tests/test_alloc_segment_audit.sh` arm 7 to assert a non-zero exit (primary) and an error-level "placement invariant violation" (secondary); add a new arm that (a) asserts the exact INFO format defined above on the clean witness, (b) when `JALV2_ALLOC_AUDIT=1`, parses both outputs and asserts the six numbers are identical (data/shared used/free/largest), and (c) asserts the INFO line is absent on the fault witness (the report sits inside the `!pfile_errct_get` no-error block).
4. Write the fix doc under `jalv3/fixes/` and record the A7 status (with the piece-3 residual) in `TODOS.md`.


<!-- autoplan-accepted:ceo -->
- The report line derives each of its six numbers from the same list the audit uses; the format is pinned by the new pin arm; it prints only on zero-error compiles.
- The WARN→ERR promotion gates on a full-corpus 0-violation sweep (L2 + L3 sample sweep), not just the two witnesses.
- Arm 7 asserts non-zero exit (primary) and the error message (secondary); a new arm pins the INFO format and asserts no double-report under JALV2_ALLOC_AUDIT.
<!-- /autoplan-accepted:ceo -->
## Review record

<!-- autoplan-accepted:ceo -->
- The report line derives each of its six numbers from the same list the audit uses; the format is pinned by the new pin arm; it prints only on zero-error compiles.
- The WARN→ERR promotion gates on a full-corpus 0-violation sweep (L2 + L3 sample sweep), not just the two witnesses.
- Arm 7 asserts non-zero exit (primary) and the error message (secondary); a new arm pins the INFO format and asserts no double-report under JALV2_ALLOC_AUDIT.
<!-- /autoplan-accepted:ceo -->

<!-- autoplan-accepted:eng -->
- One static helper computes the six usage numbers for both `pic_blist_info_log` and `pic_var_segment_audit`; the INFO line has a distinct label; the pin asserts the exact format and that the INFO numbers equal the `JALV2_ALLOC_AUDIT` numbers.
- WARN→ERR gates on a full-corpus sweep run first; land ERR only at violations == 0.
<!-- /autoplan-accepted:eng -->

## NOT in scope
- Escape-hatch env (demote the ERR back to WARN, e.g. `JALV2_ALLOC_AUDIT=warn`) for jalv2→jalv3 migration — deferred to TODOS.md (single-model Taste finding; out of HOLD SCOPE).
- Actionable ERR message naming the specific object/segment (the current message names counts only) — deferred polish.
- Reframing the INFO line as an off-by-default diagnostic mode — rejected; the always-on report is the chosen A7 direction.

