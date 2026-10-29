/************************************************************
 **
 ** esp_task.c : ESP32 cooperative tasks (start / suspend)
 **
 ** Same model as libpic12 pic_task_* and libpic32 mips_task:
 **   _task_list[task_ct]  -- 4-byte resume/entry PCs (0 = empty)
 **   _task_paused[task_ct]-- per-slot pause flags (scheduler skips)
 **   _task_active         -- current slot (byte)
 **   start  -- store the entry PC (a2) in the task's STATIC slot (slot
 **             0 stays free for main's first suspend; first-declared
 **             task takes the highest slot, so the round-robin order
 **             after main is the declaration order)
 **   suspend -- save the resume PC (a2) in the current slot, then
 **             jump to the next non-zero, unpaused slot (wrap)
 **   task leave -- resume PC 0 (clear the slot) then suspend
 **   suspend/resume/kill/restart -- _task_* statements address a task by
 **             its static slot: pause sets the flag (self: then yields),
 **             kill clears the slot (self: the suicide sequence),
 **             restart stores the entry again (self: jump to the entry)
 **
 ** Task procs are leaf frames: entered by jx (no call0), their
 ** PROC_ENTER pushes no a0, and switching never swaps a1 -- every
 ** task runs on the starter's stack (mips model: tasks are
 ** jr-entered and skip this frame).
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
#include "esp32.h"
#include "esp.h"
#include "../libcore/task_alg.h"

#define ESP_TASK_YIELD_LABEL "__esp32_task_yield"
#define ESP_TASK_SLOT_SZ       4U

/* ---- shared policy via task_alg (libcore); this file keeps only the
 * Xtensa primitives, the yield/suicide sequences and the dispatcher ---- */

static const task_alg_ops_t esp_task_alg_ops;   /* fwd */

static value_t esp_task_list_get(pfile_t *pf)
{
  return task_alg_list_get(pf, &esp_task_alg_ops, ESP_TASK_SLOT_SZ);
}

static value_t esp_task_paused_get(pfile_t *pf)
{
  return task_alg_paused_get(pf, &esp_task_alg_ops);
}

static value_t esp_task_active_get(pfile_t *pf)
{
  return task_alg_active_get(pf, &esp_task_alg_ops);
}

static unsigned esp_task_slot_ct(pfile_t *pf)
{
  return task_alg_slot_ct(pf);
}

label_t esp_task_helper_label(pfile_t *pf, const char *name)
{
  label_t lbl;

  lbl = pfile_label_find(pf, PFILE_LOG_NONE, name);
  if (!lbl) {
    lbl = pfile_label_alloc(pf, name);
  }
  return lbl;
}

boolean_t esp_tasks_used(pfile_t *pf)
{
  return task_alg_used(pf);
}

void esp_task_alloc(pfile_t *pf)
{
  task_alg_alloc(pf, &esp_task_alg_ops);
}

/* _task_list[slot] = <entry label address>  (a2 carries the PC) */
static void esp_task_entry_store(pfile_t *pf, value_t proc_val,
    unsigned slot)
{
  value_t       list;
  unsigned long base;

  list = esp_task_list_get(pf);
  esp_emit_label_addr(pf, ESP_REG_A2,
      pfile_proc_label_get(value_proc_get(proc_val)));
  base = esp_var_ram(value_variable_get(list));
  esp_emit_l32r(ESP_REG_A4,
      esp_pool_lit(base + (unsigned long) (slot * ESP_TASK_SLOT_SZ)));
  (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A2, ESP_REG_A4, 0);
  value_release(list);
}

/* _task_list[slot] = 0 */
static void esp_task_entry_clear(pfile_t *pf, unsigned slot)
{
  value_t       list;
  unsigned long base;

  list = esp_task_list_get(pf);
  base = esp_var_ram(value_variable_get(list));
  esp_emit_l32r(ESP_REG_A4,
      esp_pool_lit(base + (unsigned long) (slot * ESP_TASK_SLOT_SZ)));
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A5, 0, 0);
  (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A5, ESP_REG_A4, 0);
  value_release(list);
}

/* _task_paused[slot] = val (0/1) */
static void esp_task_flag_store(pfile_t *pf, unsigned slot, int val)
{
  value_t       paused;
  unsigned long base;

  paused = esp_task_paused_get(pf);
  base = esp_var_ram(value_variable_get(paused));
  esp_emit_l32r(ESP_REG_A4,
      esp_pool_lit(base + (unsigned long) slot));
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A5, 0, val);
  (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A5, ESP_REG_A4, 0);
  value_release(paused);
}

static void esp_task_jump_entry(pfile_t *pf, value_t proc_val)
{
  esp_emit_label_addr(pf, ESP_REG_A5,
      pfile_proc_label_get(value_proc_get(proc_val)));
  (void) esp_code_alloc(ESP_OP_JX, 0, ESP_REG_A5, 0);
}

static const task_alg_ops_t esp_task_alg_ops = {
  esp_value_ensure,
  esp_task_entry_store,
  esp_task_entry_clear,
  esp_task_flag_store,
  esp_task_jump_entry,
  esp_emit_task_yield,
  esp_emit_task_suicide
};

/* statement emitters: policy in task_alg, Xtensa sequences in the ops */
void esp_emit_task_start(pfile_t *pf, const cmd_t cmd)
{
  task_alg_emit_start(pf, &esp_task_alg_ops, cmd);
}

void esp_emit_task_suspend(pfile_t *pf, const cmd_t cmd)
{
  task_alg_emit_suspend(pf, &esp_task_alg_ops, cmd);
}

void esp_emit_task_resume(pfile_t *pf, const cmd_t cmd)
{
  task_alg_emit_resume(pf, &esp_task_alg_ops, cmd);
}

void esp_emit_task_kill(pfile_t *pf, const cmd_t cmd)
{
  task_alg_emit_kill(pf, &esp_task_alg_ops, cmd);
}

void esp_emit_task_restart(pfile_t *pf, const cmd_t cmd)
{
  task_alg_emit_restart(pf, &esp_task_alg_ops, cmd);
}

void esp_emit_task_yield(pfile_t *pf)
{
  label_t    resume;
  esp_code_t node;

  resume = pfile_label_alloc(pf, 0);
  esp_emit_label_addr(pf, ESP_REG_A2, resume);
  esp_emit_helper_jmp(pf, ESP_TASK_YIELD_LABEL);
  node = esp_code_label_alloc();
  if (resume) {
    label_code_set(resume, node);
    label_release(resume);
  }
}

/* task leave: clear the slot (resume 0), then suspend */
void esp_emit_task_suicide(pfile_t *pf)
{
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A2, 0, 0);
  esp_emit_helper_jmp(pf, ESP_TASK_YIELD_LABEL);
}

/* a4 = &_task_list[idx]  (idx in a3, 4-byte slots) */
static void esp_task_slot_addr(variable_t list, unsigned idxreg,
    unsigned dstreg)
{
  esp_emit_l32r(dstreg, esp_pool_lit(esp_var_ram(list)));
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A6, 0, 2);
  (void) esp_code_alloc(ESP_OP_SSL, 0, ESP_REG_A6, 0);
  (void) esp_code_alloc(ESP_OP_SLL, ESP_REG_A6, idxreg, 0);
  (void) esp_code_alloc(ESP_OP_ADD, dstreg, dstreg, (int) ESP_REG_A6);
}

static void esp_task_suspend_create(pfile_t *pf)
{
  label_t    lbl;
  label_t    lbl_scan;
  label_t    lbl_dec;
  value_t    list;
  value_t    paused;
  value_t    active;
  unsigned   ct;
  esp_code_t node;
  esp_code_t br;

  lbl = pfile_label_find(pf, PFILE_LOG_NONE, ESP_TASK_YIELD_LABEL);
  if (!lbl) {
    return;
  }
  node = esp_code_label_alloc();
  label_code_set(lbl, node);
  esp_op_helper_bind(lbl, node);
  label_release(lbl);

  list = esp_task_list_get(pf);
  paused = esp_task_paused_get(pf);
  active = esp_task_active_get(pf);
  ct = esp_task_slot_ct(pf);
  lbl_scan = pfile_label_alloc(pf, 0);
  lbl_dec = pfile_label_alloc(pf, 0);

  /* a3 = _task_active; store a2 (resume or 0) into that slot */
  esp_emit_l32r(ESP_REG_A4,
      esp_pool_lit(esp_var_ram(value_variable_get(active))));
  (void) esp_code_alloc(ESP_OP_L8UI, ESP_REG_A3, ESP_REG_A4, 0);
  esp_task_slot_addr(value_variable_get(list), ESP_REG_A3, ESP_REG_A5);
  (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A2, ESP_REG_A5, 0);

  node = esp_code_label_alloc();
  if (lbl_scan) {
    label_code_set(lbl_scan, node);
  }
  /* if (active == 0) active = task_ct */
  br = esp_code_alloc(ESP_OP_BNEZ, 0, ESP_REG_A3, 0);
  if (br && lbl_dec) {
    br->target = (esp_code_t) label_code_get(lbl_dec);
    if (!br->target) {
      esp_add_fixup(pf, br, lbl_dec);
    }
  }
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A3, 0, (int) ct);
  node = esp_code_label_alloc();
  if (lbl_dec) {
    label_code_set(lbl_dec, node);
  }
  (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A3, ESP_REG_A3, -1);
  esp_emit_l32r(ESP_REG_A4,
      esp_pool_lit(esp_var_ram(value_variable_get(active))));
  (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A3, ESP_REG_A4, 0);
  esp_task_slot_addr(value_variable_get(list), ESP_REG_A3, ESP_REG_A5);
  (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A7, ESP_REG_A5, 0);
  br = esp_code_alloc(ESP_OP_BEQZ, 0, ESP_REG_A7, 0);
  if (br && lbl_scan) {
    br->target = (esp_code_t) label_code_get(lbl_scan);
    if (!br->target) {
      esp_add_fixup(pf, br, lbl_scan);
    }
  }
  /* paused slots keep their resume PC but are skipped */
  esp_emit_l32r(ESP_REG_A6,
      esp_pool_lit(esp_var_ram(value_variable_get(paused))));
  (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A6, ESP_REG_A6,
      (int) ESP_REG_A3);
  (void) esp_code_alloc(ESP_OP_L8UI, ESP_REG_A6, ESP_REG_A6, 0);
  br = esp_code_alloc(ESP_OP_BNEZ, 0, ESP_REG_A6, 0);
  if (br && lbl_scan) {
    br->target = (esp_code_t) label_code_get(lbl_scan);
    if (!br->target) {
      esp_add_fixup(pf, br, lbl_scan);
    }
  }
  (void) esp_code_alloc(ESP_OP_JX, 0, ESP_REG_A7, 0);

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

void esp_task_intrinsics_create(pfile_t *pf)
{
  if (!esp_tasks_used(pf)) {
    return;
  }
  esp_task_suspend_create(pf);
}
