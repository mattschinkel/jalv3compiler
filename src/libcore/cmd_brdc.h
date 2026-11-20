/************************************************************
 **
 ** cmd_brdc.h : p-code branching structure declarations
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 ** All rights reserved
 **
 ** Original Author: Kyle A. York
 ** Author: Matthew Schinkel, 2026
 **
 ************************************************************/
#ifndef cmd_brdc_h__
#define cmd_brdc_h__

#include "label.h"
#include "value.h"
#include "cmd_brch.h"

/* branch */
typedef struct cmd_branch_ {
  cmd_branchtype_t  type;
  cmd_branchcond_t  cond;
  label_t           dst;
  value_t           var;
  /* if this is a branch into a procedure, proc & proc_params
   * hold the necessary info */
  value_t           proc;
  value_t          *proc_params;
  /* U3: open-array actual/formal pairs owned by this CALL (null for a
   * non-call or a call with no open-array actuals).  See cmd_brch.h. */
  cmd_branch_pair_t *pairs;
  size_t             pair_ct;
  size_t             pair_alloc;
} cmd_branch_t;

#endif /* cmd_brdc_h__ */

