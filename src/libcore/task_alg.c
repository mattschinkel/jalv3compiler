/************************************************************
 **
 ** task_alg.c : cooperative-task policy kernel (shared)
 **
 ** See task_alg.h.  The three scheduler variables, the static slot
 ** assignment and the six task statements' semantics live here once;
 ** the back ends (esp_task.c / mips_task.c) keep only their op tables,
 ** the yield/suicide sequences and the dispatcher helper bodies.
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved.
 **
 ************************************************************/
#include <string.h>
#include "pfiled.h"
#include "pf_proc.h"
#include "cmd_brch.h"
#include "task_alg.h"

static value_t task_alg_array_get(pfile_t *pf, const task_alg_ops_t *ops,
    const char *name, unsigned elem_sz)
{
  value_t  val;
  unsigned ct;

  val = pfile_value_find(pf, PFILE_LOG_NONE, name);
  if (val) {
    (*ops->value_ensure)(pf, val);
    return val;
  }
  ct = task_alg_slot_ct(pf);
  {
    variable_def_t vdef;
    variable_def_t def;

    vdef = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
        VARIABLE_DEF_FLAG_NONE, elem_sz);
    def  = variable_def_alloc(0, VARIABLE_DEF_TYPE_ARRAY,
        VARIABLE_DEF_FLAG_NONE, 0);
    if (vdef && def) {
      variable_def_member_add(def, 0, vdef, (variable_ct_t) ct);
      val = VALUE_NONE;
      if (RESULT_OK == pfile_value_alloc(pf, PFILE_VARIABLE_ALLOC_GLOBAL,
          name, def, &val)) {
        variable_flag_set(value_variable_get(val), VARIABLE_FLAG_AUTO);
        (*ops->value_ensure)(pf, val);
      }
    } else {
      if (vdef) {
        variable_def_free(vdef);
      }
      if (def) {
        variable_def_free(def);
      }
    }
  }
  return val;
}

value_t task_alg_list_get(pfile_t *pf, const task_alg_ops_t *ops,
    unsigned word_sz)
{
  return task_alg_array_get(pf, ops, "_task_list", word_sz);
}

value_t task_alg_paused_get(pfile_t *pf, const task_alg_ops_t *ops)
{
  return task_alg_array_get(pf, ops, "_task_paused", 1);
}

value_t task_alg_active_get(pfile_t *pf, const task_alg_ops_t *ops)
{
  value_t val;

  val = pfile_value_find(pf, PFILE_LOG_NONE, "_task_active");
  if (val) {
    (*ops->value_ensure)(pf, val);
    return val;
  }
  {
    variable_def_t def;

    def = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
        VARIABLE_DEF_FLAG_NONE, 1);
    val = VALUE_NONE;
    if (def
        && (RESULT_OK == pfile_value_alloc(pf, PFILE_VARIABLE_ALLOC_GLOBAL,
            "_task_active", def, &val))) {
      variable_flag_set(value_variable_get(val), VARIABLE_FLAG_AUTO);
      (*ops->value_ensure)(pf, val);
    } else if (def) {
      variable_def_free(def);
    }
  }
  return val;
}

static boolean_t task_alg_program_has_task_proc(pfile_t *pf)
{
  pfile_proc_t *proc;

  for (proc = pfile_proc_root_get(pf);
      proc;
      proc = pfile_proc_next(proc)) {
    if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_TASK)) {
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

boolean_t task_alg_used(pfile_t *pf)
{
  if (pfile_task_ct_get(pf) >= 2U) {
    return BOOLEAN_TRUE;
  }
  return task_alg_program_has_task_proc(pf);
}

unsigned task_alg_slot_ct(pfile_t *pf)
{
  return pfile_task_slot_ct_get(pf);
}

unsigned task_alg_slot_of(pfile_t *pf, pfile_proc_t *proc)
{
  pfile_proc_t *p;
  unsigned      ct;
  unsigned      rank;

  ct = task_alg_slot_ct(pf);
  rank = 0;
  for (p = pfile_proc_root_get(pf); p; p = pfile_proc_next(p)) {
    if (pfile_proc_flag_test(p, PFILE_PROC_FLAG_TASK)) {
      rank++;
      if (p == proc) {
        break;
      }
    }
  }
  if (!p || (ct < rank + 1U)) {
    pfile_log(pf, PFILE_LOG_ERR,
        "task count (%u) too small for the declared tasks", ct);
    return 0;
  }
  return rank;
}

void task_alg_alloc(pfile_t *pf, const task_alg_ops_t *ops)
{
  value_t list;
  value_t paused;
  value_t active;

  if (!task_alg_used(pf)) {
    return;
  }
  /* creation order fixes the linear layout: list, active, paused --
   * the order both back ends used before the kernel (byte-identity) */
  list = task_alg_list_get(pf, ops, 4);
  active = task_alg_active_get(pf, ops);
  paused = task_alg_paused_get(pf, ops);
  if (list) {
    value_release(list);
  }
  if (paused) {
    value_release(paused);
  }
  if (active) {
    value_release(active);
  }
}

/* target task + parse-time self flag of a task-control statement */
static pfile_proc_t *task_alg_target(pfile_t *pf, const cmd_t cmd,
    const task_alg_ops_t *ops, unsigned *pslot)
{
  value_t       proc_val;
  pfile_proc_t *proc;

  UNUSED(ops);
  proc_val = cmd_brproc_get(cmd);
  proc = value_proc_get(proc_val);
  if (pslot) {
    *pslot = proc ? task_alg_slot_of(pf, proc) : 0;
  }
  return proc;
}

static boolean_t task_alg_is_self(const cmd_t cmd)
{
  value_t val;

  val = cmd_brval_get(cmd);
  return (val && value_is_const(val) && value_const_get(val))
    ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

void task_alg_emit_start(pfile_t *pf, const task_alg_ops_t *ops,
    const cmd_t cmd)
{
  value_t  proc_val;
  unsigned slot;

  proc_val = cmd_brproc_get(cmd);
  slot = task_alg_slot_of(pf, value_proc_get(proc_val));
  /* static slot: store the entry and clear any stale suspend flag (a
   * start on a suspended/finished task makes it runnable again) */
  (*ops->entry_store)(pf, proc_val, slot);
  (*ops->flag_store)(pf, slot, 0);
}

void task_alg_emit_suspend(pfile_t *pf, const task_alg_ops_t *ops,
    const cmd_t cmd)
{
  unsigned slot;

  (void) task_alg_target(pf, cmd, ops, &slot);
  (*ops->flag_store)(pf, slot, 1);
  if (task_alg_is_self(cmd)) {
    /* the flag gates this task's own dispatch from the next yield on */
    (*ops->emit_yield)(pf);
  }
}

void task_alg_emit_resume(pfile_t *pf, const task_alg_ops_t *ops,
    const cmd_t cmd)
{
  unsigned slot;

  (void) task_alg_target(pf, cmd, ops, &slot);
  (*ops->flag_store)(pf, slot, 0);
}

void task_alg_emit_kill(pfile_t *pf, const task_alg_ops_t *ops,
    const cmd_t cmd)
{
  unsigned slot;

  if (task_alg_is_self(cmd)) {
    (*ops->emit_suicide)(pf);
    return;
  }
  (void) task_alg_target(pf, cmd, ops, &slot);
  (*ops->entry_clear)(pf, slot);
}

void task_alg_emit_restart(pfile_t *pf, const task_alg_ops_t *ops,
    const cmd_t cmd)
{
  value_t  proc_val;
  unsigned slot;

  proc_val = cmd_brproc_get(cmd);
  slot = task_alg_slot_of(pf, value_proc_get(proc_val));
  if (task_alg_is_self(cmd)) {
    /* re-run this body from the top right now; the entry label is the
     * dispatcher's jump target, so a plain jump re-enters it cleanly */
    (*ops->jump_entry)(pf, proc_val);
    return;
  }
  /* discard the saved resume point and re-run from the entry */
  (*ops->entry_store)(pf, proc_val, slot);
  (*ops->flag_store)(pf, slot, 0);
}
