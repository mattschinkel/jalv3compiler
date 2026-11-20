/************************************************************
 **
 ** pf_alias.h : whole-program alias/escape summary (Phase 1)
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#ifndef pf_alias_h__
#define pf_alias_h__

#include <stdio.h>

#include "pfile.h"
#include "variable.h"

/* Whole-program alias/escape summary skeleton.  Phase 1 builds the
 * per-procedure read/write variable sets over the final p-code and reuses
 * the proc->calls list for the direct call graph.  It changes no codegen:
 * JALV2_ALIAS_DUMP=1 prints the graph + summary to stderr. */

/* Reset the per-compile table (call between compiles). */
void        pfile_alias_state_reset(void);
/* TRUE when JALV2_ALIAS_DUMP is on (checked once per compile). */
boolean_t   pfile_alias_dump_enabled(void);
/* Build the per-procedure read/write sets from pf's command list. */
void        pfile_alias_summary_build(pfile_t *pf);
/* Print the call graph + per-procedure read/write summary. */
void        pfile_alias_summary_dump(pfile_t *pf, FILE *out);

/* per-procedure query API (the later phases' consumers) */
variable_t *pfile_alias_proc_reads_get(pfile_proc_t *proc);
size_t      pfile_alias_proc_reads_ct(pfile_proc_t *proc);
variable_t *pfile_alias_proc_writes_get(pfile_proc_t *proc);
size_t      pfile_alias_proc_writes_ct(pfile_proc_t *proc);

/* Phase 2: transitive (whole-program) sets -- a proc's own reads/writes
 * plus every callee's, to a fixpoint. */
variable_t *pfile_alias_proc_reads_trans_get(pfile_proc_t *proc);
size_t      pfile_alias_proc_reads_trans_ct(pfile_proc_t *proc);
variable_t *pfile_alias_proc_writes_trans_get(pfile_proc_t *proc);
size_t      pfile_alias_proc_writes_trans_ct(pfile_proc_t *proc);

/* Phase 2: TRUE when the variable's address escapes (pointer-bound or
 * passed to an open-array/byref formal). */
boolean_t   pfile_alias_var_escaped(variable_t var);

#endif /* pf_alias_h__ */
