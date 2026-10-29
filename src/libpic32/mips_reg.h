/************************************************************
 **
 ** mips_reg.h : emit-time GPR value cache
 **
 ** Tracks which scalar RAM variables currently live in GPRs so
 ** mips_load_value can skip redundant loads within a basic block.
 ** Invalidated at labels, calls, tasks, delays, and register clobbers.
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#ifndef _mips_reg_h__
#define _mips_reg_h__

#include "../libcore/pfile.h"
#include "../libcore/value.h"
#include "../libcore/variable.h"

/* Drop every cached binding (BB boundary / unknown clobber). */
void mips_reg_cache_reset(void);

/* Forget bindings that name `reg` (about to overwrite that GPR). */
void mips_reg_cache_clobber(unsigned reg);

/*
 * Forget BB bindings whose master matches `v`, and cold that master's
 * allocated home (Phase D — bit / overlay / non-cacheable stores).
 */
void mips_reg_cache_invalidate_var(variable_t v);

/* True if val is a direct scalar RAM cell safe to cache. */
boolean_t mips_reg_cacheable(const value_t val);

/*
 * If `val` is live in some GPR, return that register; else (unsigned)-1.
 * Size must match the binding.
 */
unsigned mips_reg_cache_lookup(const value_t val);

/* Record that `reg` holds `val` (after a load or store). */
void mips_reg_cache_bind(const value_t val, unsigned reg);

/*
 * Before JAL/JALR: keep BB bindings already in $s3-$s7; drop the rest.
 * Allocated $s homes stay warm via mips_alloc_call_reset.
 */
void mips_reg_cache_preserve_across_call(pfile_proc_t *cur);

#endif /* _mips_reg_h__ */
