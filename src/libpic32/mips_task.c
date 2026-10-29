/************************************************************
 **
 ** mips_task.c : PIC32 cooperative tasks (start / suspend)
 **
 ** Same model as libpic12 pic_task_* :
 **   _task_list[task_ct]  -- 4-byte resume/entry PCs (0 = empty)
 **   _task_active         -- current slot (byte)
 **   start  -- store entry in the last empty slot (scan from the end
 **             so slot 0 stays free for main's first suspend)
 **   suspend -- save resume PC in the current slot, then jr to the
 **             next non-zero slot (wrap)
 **   task leave -- resume PC 0 (clear the slot) then suspend
 **
 ** start is JAL (returns to the starter). suspend is J (does not
 ** return; the resume label is after the jump).
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include "../libutils/mem.h"
#include "../libcore/pfile.h"
#include "../libcore/pf_proc.h"
#include "../libcore/pf_log.h"
#include "../libcore/value.h"
#include "../libcore/variable.h"
#include "../libcore/vardef.h"
#include "../libcore/label.h"
#include "../libcore/cmd.h"
#include "../libcore/cmd_brch.h"
#include "mips.h"
#include "mips_alloc.h"
#include "../libcore/task_alg.h"

/* ---- shared policy via task_alg (libcore); this file keeps only the
 * MIPS primitives, the yield/suicide sequences and the dispatcher ---- */

static const task_alg_ops_t mips_task_alg_ops;   /* fwd */

static value_t mips_task_list_get(pfile_t *pf)
{
  return task_alg_list_get(pf, &mips_task_alg_ops, MIPS_WORD_SZ);
}

static value_t mips_task_paused_get(pfile_t *pf)
{
  return task_alg_paused_get(pf, &mips_task_alg_ops);
}

static value_t mips_task_active_get(pfile_t *pf)
{
  return task_alg_active_get(pf, &mips_task_alg_ops);
}

static unsigned mips_task_slot_ct(pfile_t *pf)
{
  return task_alg_slot_ct(pf);
}

static label_t mips_task_label(pfile_t *pf, const char *name)
{
  label_t lbl;

  lbl = pfile_label_find(pf, PFILE_LOG_NONE, name);
  if (!lbl) {
    lbl = pfile_label_alloc(pf, name);
  }
  return lbl;
}

boolean_t mips_tasks_used(pfile_t *pf)
{
  return task_alg_used(pf);
}

void mips_task_alloc(pfile_t *pf)
{
  task_alg_alloc(pf, &mips_task_alg_ops);
}

/* _task_list[slot] = <entry label address>  ($t1 carries the PC) */
static void mips_task_entry_store(pfile_t *pf, value_t proc_val,
    unsigned slot)
{
  value_t       list;
  unsigned long base;
  unsigned      breg;
  int           lo;

  list = mips_task_list_get(pf);
  mips_emit_label_addr(MIPS_REG_T1,
      pfile_proc_label_get(value_proc_get(proc_val)));
  base = mips_var_ram(value_variable_get(list));
  lo = mips_emit_addr_base(pf, base + (unsigned long) (slot * MIPS_WORD_SZ),
      &breg);
  (void) mips_code_alloc(MIPS_OP_SW, MIPS_REG_T1, breg, lo);
  value_release(list);
}

/* _task_list[slot] = 0 */
static void mips_task_entry_clear(pfile_t *pf, unsigned slot)
{
  value_t       list;
  unsigned long base;
  unsigned      breg;
  int           lo;

  list = mips_task_list_get(pf);
  base = mips_var_ram(value_variable_get(list));
  lo = mips_emit_addr_base(pf, base + (unsigned long) (slot * MIPS_WORD_SZ),
      &breg);
  (void) mips_code_alloc(MIPS_OP_SW, MIPS_REG_ZERO, breg, lo);
  value_release(list);
}

/* _task_paused[slot] = val (0/1; the 0 form keeps the bare sb $zero) */
static void mips_task_flag_store(pfile_t *pf, unsigned slot, int val)
{
  value_t       paused;
  unsigned long base;
  unsigned      breg;
  int           lo;

  paused = mips_task_paused_get(pf);
  if (val) {
    (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T2, MIPS_REG_ZERO, val);
  }
  base = mips_var_ram(value_variable_get(paused));
  lo = mips_emit_addr_base(pf, base + (unsigned long) slot, &breg);
  (void) mips_code_alloc(MIPS_OP_SB,
      val ? MIPS_REG_T2 : MIPS_REG_ZERO, breg, lo);
  value_release(paused);
}

static void mips_task_jump_entry(pfile_t *pf, value_t proc_val)
{
  mips_alloc_spill_dirty_keep(pf);
  (void) mips_emit_j(pfile_proc_label_get(value_proc_get(proc_val)));
}

static const task_alg_ops_t mips_task_alg_ops = {
  mips_value_ensure,
  mips_task_entry_store,
  mips_task_entry_clear,
  mips_task_flag_store,
  mips_task_jump_entry,
  mips_emit_task_yield,
  mips_emit_task_suicide
};

void mips_task_zero(pfile_t *pf)
{
  value_t       list;
  value_t       paused;
  value_t       active;
  unsigned      i;
  unsigned      ct;
  unsigned long base;
  unsigned      breg;
  int           lo;

  if (!mips_tasks_used(pf)) {
    return;
  }
  list = mips_task_list_get(pf);
  paused = mips_task_paused_get(pf);
  active = mips_task_active_get(pf);
  if (list) {
    ct = mips_task_slot_ct(pf);
    base = mips_var_ram(value_variable_get(list));
    for (i = 0; i < ct; i++) {
      lo = mips_emit_addr_base(pf, base + (unsigned long) (i * MIPS_WORD_SZ), &breg);
      (void) mips_code_alloc(MIPS_OP_SW, MIPS_REG_ZERO, breg, lo);
    }
    value_release(list);
  }
  if (active) {
    lo = mips_emit_addr_base(pf, mips_var_ram(value_variable_get(active)), &breg);
    (void) mips_code_alloc(MIPS_OP_SB, MIPS_REG_ZERO, breg, lo);
    value_release(active);
  }
  if (paused) {
    ct = mips_task_slot_ct(pf);
    base = mips_var_ram(value_variable_get(paused));
    for (i = 0; i < ct; i++) {
      lo = mips_emit_addr_base(pf, base + (unsigned long) i, &breg);
      (void) mips_code_alloc(MIPS_OP_SB, MIPS_REG_ZERO, breg, lo);
    }
    value_release(paused);
  }
}

/* Target task + static slot of a task-control statement. */
/* statement emitters: policy in task_alg, MIPS sequences in the ops */
void mips_emit_task_start(pfile_t *pf, const cmd_t cmd)
{
  task_alg_emit_start(pf, &mips_task_alg_ops, cmd);
}

void mips_emit_task_suspend(pfile_t *pf, const cmd_t cmd)
{
  task_alg_emit_suspend(pf, &mips_task_alg_ops, cmd);
}

void mips_emit_task_resume(pfile_t *pf, const cmd_t cmd)
{
  task_alg_emit_resume(pf, &mips_task_alg_ops, cmd);
}

void mips_emit_task_kill(pfile_t *pf, const cmd_t cmd)
{
  task_alg_emit_kill(pf, &mips_task_alg_ops, cmd);
}

void mips_emit_task_restart(pfile_t *pf, const cmd_t cmd)
{
  task_alg_emit_restart(pf, &mips_task_alg_ops, cmd);
}

void mips_emit_task_yield(pfile_t *pf)
{
  label_t resume;
  label_t helper;
  mips_code_t node;

  resume = pfile_label_alloc(pf, 0);
  helper = mips_task_label(pf, MIPS_LABEL_TASK_YIELD);
  mips_emit_label_addr(MIPS_REG_T1, resume);
  (void) mips_emit_j(helper);
  node = mips_code_label_alloc();
  if (resume) {
    label_code_set(resume, node);
    label_release(resume);
  }
  if (helper) {
    label_release(helper);
  }
}

void mips_emit_task_suicide(pfile_t *pf)
{
  label_t helper;

  helper = mips_task_label(pf, MIPS_LABEL_TASK_YIELD);
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1, MIPS_REG_ZERO, 0);
  (void) mips_emit_j(helper);
  if (helper) {
    label_release(helper);
  }
}

/* $t0 = &_task_list[index]  (index in `ireg`, word slots). */
static void mips_task_slot_addr(pfile_t *pf, value_t list, unsigned ireg)
{
  unsigned long base;

  UNUSED(pf);
  base = mips_var_ram(value_variable_get(list));
  mips_emit_ram_ptr(MIPS_REG_T0, base);
  (void) mips_code_r_alloc(MIPS_OP_SLL, MIPS_REG_T3, 0, ireg, 2);
  (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_T0, MIPS_REG_T0,
      MIPS_REG_T3, 0);
}

static void mips_task_suspend_create(pfile_t *pf)
{
  label_t       lbl;
  label_t       lbl_scan;
  label_t       lbl_dec;
  value_t       list;
  value_t       paused;
  value_t       active;
  unsigned      ct;
  mips_code_t   node;
  unsigned      breg;
  int           lo;

  lbl = pfile_label_find(pf, PFILE_LOG_NONE, MIPS_LABEL_TASK_YIELD);
  if (!lbl) {
    return;
  }
  node = mips_code_label_alloc();
  label_code_set(lbl, node);
  label_release(lbl);

  list = mips_task_list_get(pf);
  paused = mips_task_paused_get(pf);
  active = mips_task_active_get(pf);
  ct = mips_task_slot_ct(pf);
  lbl_scan = pfile_label_alloc(pf, 0);
  lbl_dec = pfile_label_alloc(pf, 0);

  /* $t2 = _task_active; store $t1 (resume or 0) into that slot. */
  lo = mips_emit_addr_base(pf, mips_var_ram(value_variable_get(active)), &breg);
  (void) mips_code_alloc(MIPS_OP_LBU, MIPS_REG_T2, breg, lo);
  mips_task_slot_addr(pf, list, MIPS_REG_T2);
  (void) mips_code_alloc(MIPS_OP_SW, MIPS_REG_T1, MIPS_REG_T0, 0);

  node = mips_code_label_alloc();
  if (lbl_scan) {
    label_code_set(lbl_scan, node);
  }
  /* if (active == 0) active = task_ct */
  (void) mips_emit_cond_branch(MIPS_OP_BNE, MIPS_REG_T2, MIPS_REG_ZERO,
      lbl_dec);
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T2, MIPS_REG_ZERO, (int) ct);
  node = mips_code_label_alloc();
  if (lbl_dec) {
    label_code_set(lbl_dec, node);
  }
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T2, MIPS_REG_T2, -1);
  lo = mips_emit_addr_base(pf, mips_var_ram(value_variable_get(active)), &breg);
  (void) mips_code_alloc(MIPS_OP_SB, MIPS_REG_T2, breg, lo);
  mips_task_slot_addr(pf, list, MIPS_REG_T2);
  (void) mips_code_alloc(MIPS_OP_LW, MIPS_REG_T4, MIPS_REG_T0, 0);
  (void) mips_emit_cond_branch(MIPS_OP_BEQ, MIPS_REG_T4, MIPS_REG_ZERO,
      lbl_scan);
  /* paused slots keep their resume PC but are skipped */
  mips_emit_ram_ptr(MIPS_REG_T0, mips_var_ram(value_variable_get(paused)));
  (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_T0, MIPS_REG_T0,
      MIPS_REG_T2, 0);
  (void) mips_code_alloc(MIPS_OP_LBU, MIPS_REG_T5, MIPS_REG_T0, 0);
  (void) mips_emit_cond_branch(MIPS_OP_BNE, MIPS_REG_T5, MIPS_REG_ZERO,
      lbl_scan);
  (void) mips_code_r_alloc(MIPS_OP_JR, 0, MIPS_REG_T4, 0, 0);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);

  if (lbl_scan) {
    label_release(lbl_scan);
  }
  if (lbl_dec) {
    label_release(lbl_dec);
  }
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

void mips_task_intrinsics_create(pfile_t *pf)
{
  mips_task_suspend_create(pf);
}
