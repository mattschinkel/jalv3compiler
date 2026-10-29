/************************************************************
 **
 ** esp_const.c : ESP32 const pool (3e)
 **
 ** Const arrays (`_cstrN`, `const byte tbl[N] = {...}`) have no RAM.
 ** Their initializer data (front-end `variable_data_get`) is emitted
 ** as ESP_OP_CONST_DATA nodes after the last instruction but before
 ** the halt loop.  The address is materialized via L32R through the
 ** literal pool, exactly like any other constant.
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include <string.h>

#include "../libutils/mem.h"
#include "../libcore/pfile.h"
#include "../libcore/variable.h"
#include "../libcore/vardef.h"
#include "../libcore/pf_log.h"
#include "esp.h"

/* Growable registry: the esp32_xtensa_lx6 device file alone registers
 * 60+ const tables (pps/font/adc), so a fixed cap silently dropped
 * late entries (string literals `_cstrN`, lazily registered at first
 * codegen use). */
static struct {
  variable_t var;
  esp_code_t lit;    /* unbound LIT for the data address; filled with the
                     * DRAM load address by esp_const_pool_place */
} *esp_cpool;
static unsigned esp_cpool_ct;
static unsigned esp_cpool_cap;
static unsigned long esp_cpool_base; /* DRAM base of the const segment */

boolean_t esp_var_is_const_data(variable_t v)
{
  if (!v || !variable_is_const(v)) {
    return BOOLEAN_FALSE;
  }
  return variable_data_get(v) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

static boolean_t esp_cpool_grow(void)
{
  unsigned ncap;
  void     *nd;

  ncap = esp_cpool_cap ? (esp_cpool_cap * 2U) : 64U;
  nd = REALLOC(esp_cpool, ncap * sizeof(*esp_cpool));
  if (!nd) {
    return BOOLEAN_FALSE;
  }
  esp_cpool = nd;
  esp_cpool_cap = ncap;
  return BOOLEAN_TRUE;
}

void esp_const_pool_reset(void)
{
  esp_cpool_ct = 0;
  esp_cpool_base = 0;
}

void esp_const_pool_add(variable_t v)
{
  if (!esp_var_is_const_data(v)) {
    return;
  }
  if ((esp_cpool_ct == esp_cpool_cap) && !esp_cpool_grow()) {
    return;
  }
  esp_cpool[esp_cpool_ct].var = v;
  esp_cpool[esp_cpool_ct].lit = 0;
  esp_cpool_ct++;
}

/* Get-or-create the pool literal that will hold `v`'s IRAM data address.
 * The literal starts unbound: esp_const_pool_place() binds it to the
 * ESP_OP_CONST_DATA node once all code exists, and the final layout
 * fills it with the node's laid-out address (the LIT ->target channel
 * the branch relaxation uses).  Registers `v` on first use -- string
 * literals (`_cstrN`) are value-attached and can be missed by the
 * block-variable walk. */
esp_code_t esp_const_data_lit(variable_t v)
{
  unsigned ii;

  if (!esp_var_is_const_data(v)) {
    return 0;
  }
  for (ii = 0; ii < esp_cpool_ct; ii++) {
    if (esp_cpool[ii].var == v) {
      if (!esp_cpool[ii].lit) {
        esp_cpool[ii].lit = esp_pool_lit_new();
      }
      return esp_cpool[ii].lit;
    }
  }
  esp_const_pool_add(v);
  if (!esp_cpool_ct || esp_cpool[esp_cpool_ct - 1U].var != v) {
    return 0;
  }
  esp_cpool[esp_cpool_ct - 1U].lit = esp_pool_lit_new();
  return esp_cpool[esp_cpool_ct - 1U].lit;
}

/* Resolve each const variable to its DRAM load address (the image
 * writer emits the data as a second boot segment -- real silicon
 * cannot D-side load from the 0x4008xxxx I-bus region where code
 * lives, so const bytes must arrive in DRAM via the ROM loader, the
 * same way real firmware loads .data).  Called from
 * esp_cmd_generate timing (after all code, before the halt). */
void esp_const_pool_place(pfile_t *pf)
{
  unsigned      ii;
  unsigned      used = 0;
  unsigned long run_off = 0;   /* offset within the DRAM const segment,
                                * each variable 4-aligned like the image */

  esp_cpool_base = esp_data_end();
  for (ii = 0; ii < esp_cpool_ct; ii++) {
    variable_t     v  = esp_cpool[ii].var;
    esp_code_t     lit = esp_cpool[ii].lit;
    unsigned       sz = v ? (unsigned) variable_sz_get(v) : 0;

    /* lit == 0: registered by the block walk but never referenced in
     * codegen (the device file carries 60+ unused pps/font tables) --
     * drop them so they neither bloat the image nor claim DRAM */
    if (!lit || !v || !sz || !variable_data_get(v)) {
      continue;
    }
    lit->target = 0;
    lit->imm32  = esp_cpool_base + run_off;
    variable_base_set(v,
        (variable_base_t) (esp_cpool_base + run_off), 0);
    run_off += (sz + 3UL) & ~3UL;
    esp_cpool[used] = esp_cpool[ii];
    used++;
  }
  esp_cpool_ct = used;
  UNUSED(pf);
}

unsigned esp_const_pool_count(void)
{
  return esp_cpool_ct;
}

variable_t esp_const_pool_var(unsigned ii)
{
  return (ii < esp_cpool_ct) ? esp_cpool[ii].var : 0;
}

unsigned long esp_const_pool_base(void)
{
  return esp_cpool_base;
}
