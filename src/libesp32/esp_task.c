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
#define ESP_TASK_ACTIVE_CORE "_task_active_core"
#define ESP_TASK_RUN_CORE    "_task_run_core"
#define ESP_TASK_AFFINITY    "_task_affinity"
#define ESP_TASK_LAST_CORE   "_task_last_core"

/* ---- shared policy via task_alg (libcore); this file keeps only the
 * Xtensa primitives, the yield/suicide sequences and the dispatcher ---- */

static const task_alg_ops_t esp_task_alg_ops;   /* fwd */
static value_t esp_task_core_cell_get(pfile_t *pf, const char *name,
    unsigned ct);
static value_t esp_task_active_core_get(pfile_t *pf);   /* fwd */

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
  if (esp_second_core_enabled(pf)) {
    /* the SMP dispatcher's per-core cells (placed with the task cells:
     * current slot and claim count) and the APP core's private stack */
    value_t core;

    unsigned ct;

    esp_second_core_alloc(pf);
    ct = task_alg_slot_ct(pf);
    core = esp_task_active_core_get(pf);
    if (core) {
      value_release(core);
    }
    core = esp_task_core_cell_get(pf, ESP_TASK_RUN_CORE, 2U);
    if (core) {
      value_release(core);
    }
    core = esp_task_core_cell_get(pf, ESP_TASK_AFFINITY, ct);
    if (core) {
      value_release(core);
    }
    core = esp_task_core_cell_get(pf, ESP_TASK_LAST_CORE, ct);
    if (core) {
      value_release(core);
    }
  }
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

/* `_task_start core(<expr>) proc()`: store the slot's affinity before the
 * entry (task_alg_emit_start calls this first).  No cell = the feature is
 * off, so there is nothing to pin. */
static void esp_task_affinity_store(pfile_t *pf, unsigned slot,
    value_t core)
{
  value_t       cell;
  unsigned long base;

  cell = pfile_value_find(pf, PFILE_LOG_NONE, ESP_TASK_AFFINITY);
  if (!cell) {
    return;
  }
  base = esp_var_ram(value_variable_get(cell));
  esp_emit_l32r(ESP_REG_A6, esp_pool_lit(base + slot));
  if (core && (VALUE_NONE != core)) {
    esp_emit_value_load(pf, core, ESP_REG_A2);
  } else {
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A2, 0, 0);
  }
  (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A2, ESP_REG_A6, 0);
  value_release(cell);
}

static const task_alg_ops_t esp_task_alg_ops = {
  esp_value_ensure,
  esp_task_entry_store,
  esp_task_entry_clear,
  esp_task_flag_store,
  esp_task_jump_entry,
  esp_emit_task_yield,
  esp_emit_task_suicide,
  esp_task_affinity_store
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

/* --- Phase 9: the SMP dispatcher --------------------------------------
 *
 * With `pragma target second_core on` both cores run this dispatcher.
 * Each core keeps ITS current slot in `_task_active_core[cpu]` (cpu = the
 * PRID bit 13), and a slot is claimed atomically with S32C1I on its
 * `_task_list[]` word: the claim stores 0 with the expected value set to
 * the task's resume PC, so exactly one core wins a runnable slot.  A
 * yielding task writes its resume PC back before the scan.  The claim
 * leaves a running task's slot 0 (in flight) so a second core cannot
 * re-enter it. */

#define ESP_SR_PRID           235u
#define ESP_SR_SCOMPARE1       12u
#define ESP_PRID_CORE_SHIFT    13u

/* a per-core byte cell: `_task_active_core[cpu]` (the slot the core is
 * running) or `_task_run_core[cpu]` (claims made by the core, wrapping) */
static value_t esp_task_core_cell_get(pfile_t *pf, const char *name,
    unsigned ct)
{
  value_t val;

  val = pfile_value_find(pf, PFILE_LOG_NONE, name);
  if (val) {
    esp_value_ensure(pf, val);
    return val;
  }
  {
    variable_def_t vdef;
    variable_def_t def;

    vdef = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
        VARIABLE_DEF_FLAG_NONE, 1);
    def  = variable_def_alloc(0, VARIABLE_DEF_TYPE_ARRAY,
        VARIABLE_DEF_FLAG_NONE, 0);
    if (vdef && def) {
      variable_def_member_add(def, 0, vdef, (int) ct);
      val = VALUE_NONE;
      if (RESULT_OK == pfile_value_alloc(pf, PFILE_VARIABLE_ALLOC_GLOBAL,
          name, def, &val)) {
        variable_flag_set(value_variable_get(val), VARIABLE_FLAG_AUTO);
        esp_value_ensure(pf, val);
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

static value_t esp_task_active_core_get(pfile_t *pf)
{
  return esp_task_core_cell_get(pf, ESP_TASK_ACTIVE_CORE, 2U);
}

/* the SMP dispatcher: per-core current slot, S32C1I slot claiming, and
 * the scan that both cores run (slot 0 last) */
static void esp_task_suspend_create_smp(pfile_t *pf)
{
  label_t    lbl;
  label_t    lbl_scan;
  label_t    lbl_have;
  label_t    lbl_nocur;
  label_t    lbl_claim;
  value_t    list;
  value_t    paused;
  value_t    core;
  value_t    runcore;
  value_t    affinity;
  value_t    lastcore;
  unsigned   ct;
  esp_code_t node;
  esp_code_t br;
  esp_code_t blk;
  unsigned long list_ram;
  unsigned long runcore_ram;
  unsigned long affinity_ram;
  unsigned long lastcore_ram;

  lbl = pfile_label_find(pf, PFILE_LOG_NONE, ESP_TASK_YIELD_LABEL);
  if (!lbl) {
    return;
  }
  node = esp_code_label_alloc();
  label_code_set(lbl, node);
  esp_op_helper_bind(lbl, node);
  label_release(lbl);

  list   = esp_task_list_get(pf);
  paused = esp_task_paused_get(pf);
  core   = esp_task_active_core_get(pf);
  runcore = esp_task_core_cell_get(pf, ESP_TASK_RUN_CORE, 2U);
  ct     = esp_task_slot_ct(pf);
  affinity = esp_task_core_cell_get(pf, ESP_TASK_AFFINITY, ct);
  lastcore = esp_task_core_cell_get(pf, ESP_TASK_LAST_CORE, ct);
  if (!list || !paused || !core || !runcore || !affinity || !lastcore
      || (ct < 2)) {
    if (list)   { value_release(list); }
    if (paused) { value_release(paused); }
    if (core)   { value_release(core); }
    if (runcore) { value_release(runcore); }
    if (affinity) { value_release(affinity); }
    if (lastcore) { value_release(lastcore); }
    return;
  }
  list_ram = esp_var_ram(value_variable_get(list));
  runcore_ram = esp_var_ram(value_variable_get(runcore));
  affinity_ram = esp_var_ram(value_variable_get(affinity));
  lastcore_ram = esp_var_ram(value_variable_get(lastcore));
  lbl_scan = pfile_label_alloc(pf, 0);
  lbl_have = pfile_label_alloc(pf, 0);
  lbl_nocur = pfile_label_alloc(pf, 0);
  lbl_claim = pfile_label_alloc(pf, 0);

  /* a3 = this core's id; a4 = &_task_active_core[core] */
  (void) esp_code_alloc(ESP_OP_RSR, ESP_REG_A3, 0, (int) ESP_SR_PRID);
  esp_emit_extui(ESP_REG_A3, ESP_REG_A3, ESP_PRID_CORE_SHIFT, 1);
  esp_emit_l32r(ESP_REG_A4,
      esp_pool_lit(esp_var_ram(value_variable_get(core))));
  (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A4, ESP_REG_A4,
      (int) ESP_REG_A3);

  /* cur = active_core[core]; if cur != 0, list[cur] = a2 (resume PC).
   * cur == 0 means this core is about to suspend MAIN (slot 0 is main's
   * resume slot): store a2 into list[0] too.  The APP core's first entry
   * passes a2 = 0 and must not clear main's slot, hence the test. */
  (void) esp_code_alloc(ESP_OP_L8UI, ESP_REG_A5, ESP_REG_A4, 0);
  br = esp_code_alloc(ESP_OP_BEQZ, 0, ESP_REG_A5, 0);
  if (br) {
    blk = (esp_code_t) label_code_get(lbl_nocur);
    if (blk) {
      br->target = blk;
    } else {
      esp_add_fixup(pf, br, lbl_nocur);
    }
  }
  esp_emit_l32r(ESP_REG_A6, esp_pool_lit(list_ram));
  (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A7, ESP_REG_A5, (int) ESP_REG_A5);
  (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A7, ESP_REG_A7, (int) ESP_REG_A7);
  (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A6, ESP_REG_A6, (int) ESP_REG_A7);
  (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A2, ESP_REG_A6, 0);
  /* unconditional goto: near-J needs a 4-aligned delta (3-byte
   * instructions rarely give one), so use l32r + jx like the cmd layer */
  esp_emit_label_addr(pf, ESP_REG_A7, lbl_scan);
  (void) esp_code_alloc(ESP_OP_JX, 0, ESP_REG_A7, 0);
  /* no current slot: a2 == 0 (a fresh core entry) saves nothing */
  node = esp_code_label_alloc();
  if (lbl_nocur) {
    label_code_set(lbl_nocur, node);
  }
  br = esp_code_alloc(ESP_OP_BEQZ, 0, ESP_REG_A2, 0);
  if (br) {
    blk = (esp_code_t) label_code_get(lbl_scan);
    if (blk) {
      br->target = blk;
    } else {
      esp_add_fixup(pf, br, lbl_scan);
    }
  }
  esp_emit_l32r(ESP_REG_A6, esp_pool_lit(list_ram));
  (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A2, ESP_REG_A6, 0);

  /* scan (the single-core dispatcher's walk): at the loop top a slot of
   * 0 wraps to ct; then the slot index is decremented and CHECKED --
   * slot 0 (the main task) is the last one visited */
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A5, 0, (int) ct);
  node = esp_code_label_alloc();
  if (lbl_scan) {
    label_code_set(lbl_scan, node);
  }
  br = esp_code_alloc(ESP_OP_BNEZ, 0, ESP_REG_A5, 0);
  if (br) {
    blk = (esp_code_t) label_code_get(lbl_have);
    if (blk) {
      br->target = blk;
    } else {
      esp_add_fixup(pf, br, lbl_have);
    }
  }
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A5, 0, (int) ct);
  node = esp_code_label_alloc();
  if (lbl_have) {
    label_code_set(lbl_have, node);
  }
  (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A5, ESP_REG_A5, -1);

  /* pc = list[k] */
  esp_emit_l32r(ESP_REG_A6, esp_pool_lit(list_ram));
  (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A7, ESP_REG_A5, (int) ESP_REG_A5);
  (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A7, ESP_REG_A7, (int) ESP_REG_A7);
  (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A6, ESP_REG_A6, (int) ESP_REG_A7);
  (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A8, ESP_REG_A6, 0);
  br = esp_code_alloc(ESP_OP_BEQZ, 0, ESP_REG_A8, 0);
  if (br) {
    blk = (esp_code_t) label_code_get(lbl_scan);
    if (blk) {
      br->target = blk;
    } else {
      esp_add_fixup(pf, br, lbl_scan);
    }
  }
  /* paused slots keep their resume PC but are skipped */
  esp_emit_l32r(ESP_REG_A9,
      esp_pool_lit(esp_var_ram(value_variable_get(paused))));
  (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A9, ESP_REG_A9,
      (int) ESP_REG_A5);
  (void) esp_code_alloc(ESP_OP_L8UI, ESP_REG_A9, ESP_REG_A9, 0);
  br = esp_code_alloc(ESP_OP_BNEZ, 0, ESP_REG_A9, 0);
  if (br) {
    blk = (esp_code_t) label_code_get(lbl_scan);
    if (blk) {
      br->target = blk;
    } else {
      esp_add_fixup(pf, br, lbl_scan);
    }
  }
  /* affinity: skip a slot pinned to another core (0 = any) */
  esp_emit_l32r(ESP_REG_A11, esp_pool_lit(affinity_ram));
  (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A11, ESP_REG_A11,
      (int) ESP_REG_A5);
  (void) esp_code_alloc(ESP_OP_L8UI, ESP_REG_A11, ESP_REG_A11, 0);
  br = esp_code_alloc(ESP_OP_BEQZ, 0, ESP_REG_A11, 0);
  if (br) {
    blk = (esp_code_t) label_code_get(lbl_claim);
    if (blk) {
      br->target = blk;
    } else {
      esp_add_fixup(pf, br, lbl_claim);
    }
  }
  (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A12, ESP_REG_A3, 1);
  br = esp_code_alloc(ESP_OP_BEQ, ESP_REG_A12, ESP_REG_A11, 0);
  if (br) {
    blk = (esp_code_t) label_code_get(lbl_claim);
    if (blk) {
      br->target = blk;
    } else {
      esp_add_fixup(pf, br, lbl_claim);
    }
  }
  /* pinned elsewhere: back to the scan (l32r+jx: near-J needs a
   * 4-aligned delta, which 3-byte instructions rarely give) */
  esp_emit_label_addr(pf, ESP_REG_A11, lbl_scan);
  (void) esp_code_alloc(ESP_OP_JX, 0, ESP_REG_A11, 0);
  node = esp_code_label_alloc();
  if (lbl_claim) {
    label_code_set(lbl_claim, node);
  }
  /* claim: SCOMPARE1 = pc; S32C1I 0 into list[k]; a10 = the old word */
  (void) esp_code_alloc(ESP_OP_WSR, ESP_REG_A8, 0, (int) ESP_SR_SCOMPARE1);
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A10, 0, 0);
  (void) esp_code_alloc(ESP_OP_S32C1I, ESP_REG_A10, ESP_REG_A6, 0);
  /* the BRANCH encoders take (at, as) = (t, s): pass both registers so
   * the emitted compare is a10 vs a8, not a10 vs a0 */
  br = esp_code_alloc(ESP_OP_BNE, ESP_REG_A8, ESP_REG_A10, 0);
  if (br) {
    blk = (esp_code_t) label_code_get(lbl_scan);
    if (blk) {
      br->target = blk;
    } else {
      esp_add_fixup(pf, br, lbl_scan);
    }
  }
  /* claimed: active_core[core] = k; last_core[k] = core + 1;
   * bump run_core[core]; run the task */
  (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A5, ESP_REG_A4, 0);
  esp_emit_l32r(ESP_REG_A11, esp_pool_lit(lastcore_ram));
  (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A11, ESP_REG_A11,
      (int) ESP_REG_A5);
  (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A12, ESP_REG_A3, 1);
  (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A12, ESP_REG_A11, 0);
  esp_emit_l32r(ESP_REG_A11, esp_pool_lit(runcore_ram));
  (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A11, ESP_REG_A11,
      (int) ESP_REG_A3);
  (void) esp_code_alloc(ESP_OP_L8UI, ESP_REG_A12, ESP_REG_A11, 0);
  (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A12, ESP_REG_A12, 1);
  (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A12, ESP_REG_A11, 0);
  (void) esp_code_alloc(ESP_OP_JX, 0, ESP_REG_A8, 0);

  label_release(lbl_scan);
  label_release(lbl_have);
  label_release(lbl_nocur);
  label_release(lbl_claim);
  value_release(list);
  value_release(paused);
  value_release(core);
  value_release(runcore);
  value_release(affinity);
  value_release(lastcore);
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

  if (esp_second_core_enabled(pf)) {
    esp_task_suspend_create_smp(pf);
    return;
  }
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
