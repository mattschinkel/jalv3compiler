/************************************************************
 **
 ** indirect_alg.h : shared indirect-call argument protocol
 **
 ** ESP32 (Xtensa) and PIC32 (MIPS) pass arguments to INDIRECT-flagged
 ** procs (pointer-called procs and volatile device thunks) through one
 ** shared marshalling INBOX: indirect callers cannot see the callee's
 ** param variables, so they marshal by signature member order.  The
 ** protocol lives exactly once, here:
 **
 **   - ialg_max_signature sizes the inbox (largest indirect signature)
 **   - ialg_marshal_in/out are the caller-side argument copies
 **   - ialg_param_in/out are the callee-side PROC_ENTER/LEAVE copies
 **     between the inbox and the proc's own linear param homes
 **
 ** Params must NOT live in the inbox: a proc whose params sat there
 ** would be clobbered by any nested indirect call it makes (every
 ** device write marshals through the same bytes).  All offset arithmetic
 ** derives from this module's one align rule and the target's one
 ** def_bytes callback, so caller and callee can never disagree on
 ** where an argument lands.
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#ifndef indirect_alg_h__
#define indirect_alg_h__

#include "pfile.h"
#include "pf_proc.h"
#include "value.h"
#include "vardef.h"

typedef struct ialg_ops_s {
  /* storage bytes of a def (one rule per target, used by every walk) */
  unsigned  (*def_bytes)(pfile_t *pf, variable_def_t def);
  boolean_t (*is_wide)(value_t dst, value_t v1, value_t v2);
  void      (*wide_assign)(pfile_t *pf, value_t dst, value_t src);
  /* dst <- src for a scalar through the target's scratch register */
  void      (*scalar_copy)(pfile_t *pf, value_t dst, value_t src);
} ialg_ops_t;

/* largest indirect signature in the program: INDIRECT procs' params
 * plus the fn-def param bytes of function-pointer variables */
unsigned ialg_max_signature(pfile_t *pf, const ialg_ops_t *ops);

/* place the inbox (back end assigns the address; base is relative to
 * the target's RAM base, like every other variable base) */
void     ialg_inbox_set(unsigned long base, unsigned limit);
unsigned ialg_inbox_limit(void);

/* fn-def unwrap of a call target (deref the pointer wrapper the front
 * end puts around fn defs; FUNCTION defs pass through) */
variable_def_t ialg_call_fn_def(value_t proc_val);

/* a value view of the inbox slot at ofs (base-relative), bounded by
 * the inbox limit; VALUE_NONE when out of range */
value_t ialg_slot_value(pfile_t *pf, const ialg_ops_t *ops,
    variable_def_t def, unsigned ofs);

/* caller side: marshal IN args into the inbox / pick OUT args (and
 * the function return) back out, by signature member order */
void ialg_marshal_in(pfile_t *pf, const ialg_ops_t *ops, value_t proc_val,
    value_t *params, size_t ct);
void ialg_marshal_out(pfile_t *pf, const ialg_ops_t *ops, value_t proc_val,
    value_t *params, size_t ct);

/* callee side (INDIRECT procs): copy the inbox into the proc's linear
 * param homes at PROC_ENTER (IN params), and copy OUT params back at
 * PROC_LEAVE -- before the body can make indirect calls of its own */
void ialg_param_in(pfile_t *pf, const ialg_ops_t *ops, pfile_proc_t *proc);
void ialg_param_out(pfile_t *pf, const ialg_ops_t *ops, pfile_proc_t *proc);

#endif /* indirect_alg_h__ */
