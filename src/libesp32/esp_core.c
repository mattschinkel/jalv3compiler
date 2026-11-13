/************************************************************
 **
 ** esp_core.c : the ESP32 second (APP) core -- boot and entry
 **
 ** Phase 9 (SMP branch).  The APP LX6 core is held in reset and
 ** stalled at power-on; the PRO core releases it through the
 ** DPORT APPCPU control block (see
 ** docs/investigations/esp32_second_core_design_20261112.md for
 ** the register/bit table and the release order):
 **
 **   1. CTRL_D bit 0 = 1        (OVLD: our boot address wins)
 **   2. CTRL_C = entry & 0xFFFFFF
 **   3. CTRL_A bit 0 = 0        (release reset)
 **   4. CTRL_B bit 1 = 0        (release run stall)
 **   5. CTRL_B bit 0 = 1        (APP clock enabled)
 **
 ** The entry body is the second core's dispatch loop: with a counter
 ** declared as `_esp32_app_hits` it bumps it once (the boot proof the
 ** witnesses observe) and then enters the cooperative SMP dispatcher
 ** (the shared task round-robin); without tasks it parks.
 **
 ** The feature is off by default and experimental: QEMU proves the
 ** scheduling, silicon concurrency needs its own validation plan.
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/


#include "../libcore/pfile.h"
#include "../libcore/value.h"
#include "../libcore/variable.h"
#include "../libcore/vardef.h"
#include "esp.h"

#define ESP_APP_ENTRY_NAME "_esp32_app_entry"
#define ESP_APP_HITS_NAME  "app_core_hits"
/* the APP core's private stack (the PRO stack at ESP32_STACK_TOP is in use
 * by the PRO core the moment the second core boots; sharing it corrupts
 * both cores' frames).  1024 integer slots = 4 KB, allocated in DRAM with
 * the other runtime cells so the linear allocator keeps it off everything
 * else. */
#define ESP_APP_STACK_NAME  "_esp32_app_stack"
#define ESP_APP_STACK_WORDS 1024U
#define ESP_DPORT_CTRL_A   0x3FF0002CUL
#define ESP_DPORT_CTRL_B   0x3FF00030UL
#define ESP_DPORT_CTRL_C   0x3FF00034UL
#define ESP_DPORT_CTRL_D   0x3FF00038UL
/* the ROM bootloader holds the APP core stalled in the RTC controller as
 * well as in DPORT: RTC_CNTL_SW_CPU_STALL[7:0] are SW_STALL_APPCPU_C1
 * [7:2] | SW_STALL_APPCPU_C0 [1:0].  Clearing DPORT alone leaves the APP
 * core stalled (measured on silicon 2026-11-12: SC-APP-MISS). */
#define ESP_RTC_SW_CPU_STALL 0x3FF480ACUL
/* Xtensa PS (the PRO startup's local copy lives in esp.c) */
#define ESP_SR_PS            230u

/* `pragma target enable_cores 2` (default 1): the second (APP) core is
 * enabled when the program asks for two cores.  The chip limit is
 * validated after parsing (jal_target_cores_validate). */
boolean_t esp_second_core_enabled(pfile_t *pf)
{
  value_t pragma;

  pragma = pfile_value_find(pf, PFILE_LOG_NONE, "target_enable_cores");
  if (pragma) {
    boolean_t on;

    on = (value_const_get(pragma) >= 2) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
    value_release(pragma);
    return on;
  }
  return BOOLEAN_FALSE;
}

/* the APP core's private stack cell, created with the other runtime cells
 * before the startup emitter runs (the entry loads its top). */
static value_t esp_app_stack_get(pfile_t *pf)
{
  value_t       val;
  variable_def_t vdef;
  variable_def_t def;

  val = pfile_value_find(pf, PFILE_LOG_NONE, ESP_APP_STACK_NAME);
  if (val) {
    esp_value_ensure(pf, val);
    return val;
  }
  vdef = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
      VARIABLE_DEF_FLAG_NONE, 4);
  def  = variable_def_alloc(0, VARIABLE_DEF_TYPE_ARRAY,
      VARIABLE_DEF_FLAG_NONE, 0);
  if (vdef && def) {
    variable_def_member_add(def, 0, vdef, (int) ESP_APP_STACK_WORDS);
    val = VALUE_NONE;
    if (RESULT_OK == pfile_value_alloc(pf, PFILE_VARIABLE_ALLOC_GLOBAL,
        ESP_APP_STACK_NAME, def, &val)) {
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
  return val;
}

/* allocate the APP stack with the runtime cells (called from the task
 * allocator, which runs inside esp_var_alloc, before the startup emit) */
void esp_second_core_alloc(pfile_t *pf)
{
  value_t stack;

  if (!esp_second_core_enabled(pf)) {
    return;
  }
  stack = esp_app_stack_get(pf);
  if (stack) {
    value_release(stack);
  }
}

/* the DPORT release sequence, emitted at startup when the feature is on.
 *
 * The register map is the SoC header's (pic2jal/reference/esp32/
 * dport_reg.h, verified 2026-11-12 against silicon):
 *   CTRL_A[0]   RESETTING      1 = held in reset
 *   CTRL_B[0]   CLKGATE_EN     1 = APP clock enabled
 *   CTRL_C[0]   RUNSTALL       1 = run stalled
 *   CTRL_D[31:0] APPCPU_BOOT_ADDR
 * There is NO OVLD bit on this part: the boot address is written straight
 * to CTRL_D.  The first cut wrote the address to CTRL_C and 1 to CTRL_D,
 * which released the stall and booted the core at address 1. */
void esp_second_core_startup_emit(pfile_t *pf)
{
  label_t entry;

  if (!esp_second_core_enabled(pf)) {
    return;
  }
  entry = pfile_label_find(pf, PFILE_LOG_NONE, ESP_APP_ENTRY_NAME);
  if (!entry) {
    /* the entry body is emitted later; create the label now so the boot
     * address literal can bind to it */
    entry = pfile_label_alloc(pf, ESP_APP_ENTRY_NAME);
  }
  /* 1: CTRL_D = the APP entry address */
  esp_emit_l32r(ESP_REG_A5, esp_pool_lit(ESP_DPORT_CTRL_D));
  if (entry) {
    esp_emit_label_addr(pf, ESP_REG_A4, entry);
    label_release(entry);
  } else {
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A4, 0, 0);
  }
  (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A4, ESP_REG_A5, 0);
  /* diagnostic: read the boot address back into `app_boot_seen` when the
   * program declares it (silicon bring-up of the second core) */
  {
    value_t seen;

    seen = pfile_value_find(pf, PFILE_LOG_NONE, "app_boot_seen");
    if (seen && (VARIABLE_NONE != value_variable_get(seen))) {
      esp_emit_l32r(ESP_REG_A3,
          esp_pool_lit(esp_var_ram(value_variable_get(seen))));
      (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A2, ESP_REG_A5, 0);
      (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A2, ESP_REG_A3, 0);
      value_release(seen);
    }
  }
  /* 2: clear the RTC stall of the APP core (bits 0..7) */
  esp_emit_l32r(ESP_REG_A5, esp_pool_lit(ESP_RTC_SW_CPU_STALL));
  (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A4, ESP_REG_A5, 0);
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A6, 0, -256);
  (void) esp_code_alloc(ESP_OP_AND, ESP_REG_A4, ESP_REG_A4, (int) ESP_REG_A6);
  (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A4, ESP_REG_A5, 0);
  /* 3: CTRL_A bit 0 = 0 (release reset) */
  esp_emit_l32r(ESP_REG_A5, esp_pool_lit(ESP_DPORT_CTRL_A));
  (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A4, ESP_REG_A5, 0);
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A6, 0, -2);
  (void) esp_code_alloc(ESP_OP_AND, ESP_REG_A4, ESP_REG_A4, (int) ESP_REG_A6);
  (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A4, ESP_REG_A5, 0);
  /* 4: CTRL_C bit 0 = 0 (release the run stall) */
  esp_emit_l32r(ESP_REG_A5, esp_pool_lit(ESP_DPORT_CTRL_C));
  (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A4, ESP_REG_A5, 0);
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A6, 0, -2);
  (void) esp_code_alloc(ESP_OP_AND, ESP_REG_A4, ESP_REG_A4, (int) ESP_REG_A6);
  (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A4, ESP_REG_A5, 0);
  /* 5: CTRL_B bit 0 = 1 (APP clock enabled) */
  esp_emit_l32r(ESP_REG_A5, esp_pool_lit(ESP_DPORT_CTRL_B));
  (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A4, ESP_REG_A5, 0);
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A6, 0, 1);
  (void) esp_code_alloc(ESP_OP_OR, ESP_REG_A4, ESP_REG_A4, (int) ESP_REG_A6);
  (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A4, ESP_REG_A5, 0);
}

/* the APP entry body: bump `_esp32_app_hits` once (the boot proof), then
 * park.  Placed after the program so its node exists before the layout. */
void esp_second_core_entry_create(pfile_t *pf)
{
  label_t    lbl;
  value_t    hits;
  value_t    stack;
  esp_code_t node;
  esp_code_t loop;

  if (!esp_second_core_enabled(pf)) {
    return;
  }
  lbl = pfile_label_find(pf, PFILE_LOG_NONE, ESP_APP_ENTRY_NAME);
  if (!lbl) {
    lbl = pfile_label_alloc(pf, ESP_APP_ENTRY_NAME);
  }
  if (!lbl) {
    return;
  }
  node = esp_code_label_alloc();
  label_code_set(lbl, node);
  label_release(lbl);
  hits = pfile_value_find(pf, PFILE_LOG_NONE, ESP_APP_HITS_NAME);
  if (hits && (VARIABLE_NONE != value_variable_get(hits))) {
    esp_emit_l32r(ESP_REG_A3,
        esp_pool_lit(esp_var_ram(value_variable_get(hits))));
    (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A2, ESP_REG_A3, 0);
    (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A2, ESP_REG_A2, 1);
    (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A2, ESP_REG_A3, 0);
    value_release(hits);
  }
  /* the APP core's private stack (allocated with the runtime cells); the
   * PRO core is already using the ESP32_STACK_TOP stack, so the two cores
   * must not share one */
  stack = pfile_value_find(pf, PFILE_LOG_NONE, ESP_APP_STACK_NAME);
  if (stack && (VARIABLE_NONE != value_variable_get(stack))) {
    esp_emit_l32r(ESP_REG_A1, esp_pool_lit(
        esp_var_ram(value_variable_get(stack))
        + ((unsigned long) ESP_APP_STACK_WORDS * 4UL)));
    value_release(stack);
  } else {
    esp_emit_l32r(ESP_REG_A1, esp_pool_lit(ESP32_STACK_TOP));
  }
  /* reset may leave PS.WOE set: clear it before a0 is ever used as a
   * return address (same guarantee as the PRO startup) */
  (void) esp_code_alloc(ESP_OP_RSR, ESP_REG_A2, 0, ESP_SR_PS);
  esp_emit_l32r(ESP_REG_A3, esp_pool_lit(0xFFFBFFFFUL));
  (void) esp_code_alloc(ESP_OP_AND, ESP_REG_A2, ESP_REG_A2, (int) ESP_REG_A3);
  (void) esp_code_alloc(ESP_OP_WSR, ESP_REG_A2, 0, ESP_SR_PS);
  (void) esp_code_alloc(ESP_OP_ESYNC, 0, 0, 0);
  /* ATOMCTL on this core too: S32C1I against DRAM does not store with
   * the reset value 0x28 (bypass field 0 = "exception"); 0x2A selects
   * the core's internal operation, measured working on silicon
   * 2026-11-12 (0x29/RCW does not store on this part) */
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A2, 0, 0x2A);
  (void) esp_code_alloc(ESP_OP_WSR, ESP_REG_A2, 0, 99);
  if (esp_tasks_used(pf)) {
    /* Phase 9 SMP: enter the shared cooperative dispatcher as a task-less
     * core (a2 = 0: no resume PC of its own) and claim whatever slot is
     * runnable */
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A2, 0, 0);
    esp_emit_helper_jmp(pf, "__esp32_task_yield");
    return;
  }
  /* park: a self-loop owned by the APP core */
  loop = esp_code_label_alloc();
  {
    esp_code_t j;

    j = esp_code_alloc(ESP_OP_J, 0, 0, 0);
    if (j) {
      j->target = loop;
    }
  }
}
