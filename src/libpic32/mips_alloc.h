/************************************************************
 **
 ** mips_alloc.h : PIC32 GPR home assignment
 **
 ** Dirty write-back: home stores skip RAM; flush at CFG boundaries /
 ** CALL / bit RMW — never from mips_code_label_alloc (mid-expr joins).
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#ifndef _mips_alloc_h__
#define _mips_alloc_h__

#include "../libcore/pfile.h"
#include "../libcore/cmd.h"
#include "../libcore/variable.h"

void mips_alloc_run(pfile_t *pf, cmd_t cmd_head);
void mips_alloc_reset(void);

/* Remember pf for spill emission. */
void mips_alloc_pf_set(pfile_t *pf);

/* Spill every dirty home to RAM. */
void mips_alloc_flush_dirty(pfile_t *pf);

/*
 * Spill every dirty home at a CFG edge.  $s homes stay dirty (the
 * target label's flush still serves other predecessors); caller-saved
 * homes are spilled AND colded, because a back-edge through a CALL
 * reaches the label with that register clobbered.
 */
void mips_alloc_spill_dirty_keep(pfile_t *pf);

/*
 * Spill + cold dirty CALLER-SAVED homes before a source LABEL node
 * (only the fall-through edge executes it).  Never store a
 * caller-saved home from the label itself: a branch or back-edge
 * predecessor may have called through a clobber.
 */
void mips_alloc_spill_dirty_caller(pfile_t *pf);

/*
 * Flush all dirty, then cold every home. Use at source LABEL, ptr-deref,
 * delay, and other unknown clobbers — not at mips_code_label_alloc.
 */
void mips_alloc_region_reset(void);

/*
 * Spill every dirty home (RAM must match for getters / address), then
 * cold homes that must not stay live across this CALL. Nested $s stay
 * warm; file-level $s stay warm only when `callee` is known and does
 * not write that master (transitive). Pass NULL for indirect/unknown.
 */
void mips_alloc_call_reset(pfile_proc_t *callee);

/*
 * PROC_ENTER boundary: cold every caller-saved home WITHOUT spilling.
 * A callee body is reached at runtime from a call site whose own
 * boundary already spilled what was dirty, and the register content at
 * entry is whatever the runtime caller left there — an emission-order
 * spill here would land inside the callee and store garbage. $s homes
 * are callee-saved (frames push/pop them) and may stay live.
 */
void mips_alloc_proc_enter_cold(void);

unsigned mips_alloc_home_reg(variable_t v);
int mips_alloc_home_is_live(variable_t v);
void mips_alloc_home_set_live(variable_t v);
void mips_alloc_home_set_dirty(variable_t v);
void mips_alloc_home_invalidate(variable_t v);
/* Spill master if dirty (bit/bitfield RMW must see RAM == home). */
void mips_alloc_home_flush(pfile_t *pf, variable_t v);
int mips_alloc_reg_is_callee_home(unsigned reg);

/*
 * If `reg` currently holds a live allocated home, spill (if dirty) and
 * cold it. Call before emitting into a scratch that may be in the home
 * pool (e.g. $t0 after free-$t0).
 */
void mips_alloc_scratch_clobber(pfile_t *pf, unsigned reg);

/* Emit RAM store of home (mips_op.c). */
void mips_alloc_emit_home_spill(pfile_t *pf, variable_t v, unsigned home);

#endif /* _mips_alloc_h__ */
