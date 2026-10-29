/************************************************************
 **
 ** task_alg.h : cooperative-task policy kernel (shared)
 **
 ** The task POLICY lives once here -- scheduler variable layout,
 ** static slot assignment, the semantics of the six task statements
 ** (start / yield / suspend / resume / kill / restart, incl. their
 ** self forms) -- while each back end supplies only the machine
 ** primitives through task_alg_ops_t (third kernel after wide_alg
 ** and indirect_alg; same rationale: fixes must not land twice).
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved.
 **
 ************************************************************/
#ifndef task_alg_h__
#define task_alg_h__

#include "pfile.h"
#include "cmd.h"
#include "label.h"

typedef struct task_alg_ops_ {
  /* place a global value in the target's linear RAM layout */
  void (*value_ensure)(pfile_t *pf, value_t val);
  /* materialize the entry code address and store it into
   * _task_list[slot] (make the task runnable); receives the task's
   * proc VALUE (classic codegen keys its MOVLW-label load on it) */
  void (*entry_store)(pfile_t *pf, value_t proc_val, unsigned slot);
  /* store 0 into _task_list[slot] (remove from scheduling) */
  void (*entry_clear)(pfile_t *pf, unsigned slot);
  /* store 0/1 into _task_paused[slot] */
  void (*flag_store)(pfile_t *pf, unsigned slot, int val);
  /* restart-self: jump straight to the task entry */
  void (*jump_entry)(pfile_t *pf, value_t proc_val);
  /* one-slice yield: resume point + jump to the dispatcher */
  void (*emit_yield)(pfile_t *pf);
  /* self-kill: clear own slot, then yield away forever */
  void (*emit_suicide)(pfile_t *pf);
} task_alg_ops_t;

/* scheduler variables (created on first use; GLOBAL + AUTO) */
value_t task_alg_list_get(pfile_t *pf, const task_alg_ops_t *ops,
             unsigned word_sz);
value_t task_alg_paused_get(pfile_t *pf, const task_alg_ops_t *ops);
value_t task_alg_active_get(pfile_t *pf, const task_alg_ops_t *ops);

boolean_t task_alg_used(pfile_t *pf);
/* static slot: slot 0 = main; first-declared task = highest slot, so
 * the round-robin (downward scan) visits tasks in declaration order */
unsigned task_alg_slot_of(pfile_t *pf, pfile_proc_t *proc);
/* slot count (== pfile_task_slot_ct_get) */
unsigned task_alg_slot_ct(pfile_t *pf);

void task_alg_alloc(pfile_t *pf, const task_alg_ops_t *ops);

/* statement emitters: cmd carries the target task (brproc) and the
 * parse-time self constant (brval). */
void task_alg_emit_start(pfile_t *pf, const task_alg_ops_t *ops,
             const cmd_t cmd);
void task_alg_emit_suspend(pfile_t *pf, const task_alg_ops_t *ops,
             const cmd_t cmd);
void task_alg_emit_resume(pfile_t *pf, const task_alg_ops_t *ops,
             const cmd_t cmd);
void task_alg_emit_kill(pfile_t *pf, const task_alg_ops_t *ops,
             const cmd_t cmd);
void task_alg_emit_restart(pfile_t *pf, const task_alg_ops_t *ops,
             const cmd_t cmd);

#endif /* task_alg_h__ */
