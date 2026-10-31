/************************************************************
 **
 ** esp_op.c : ESP32 (Xtensa LX6) -- translate cmd_t to esp_code_t
 **
 ** Load/store of scalars, the binary arithmetic / logic /
 ** shift operators (+ - * & | ^ << >>), unary NEG/NOTL/INCR/DECR,
 ** DIV/MOD via outlined __esp32_* helpers (the LX6 has no hardware
 ** divide), boolean materialization of comparisons, conditional
 ** branches (fused compare+branch and branch-on-bool) and
 ** unconditional gotos, procedures, pointers and delays.
 **
 ** Register convention (call0; static frames):
 **   a2 -- operand 1 / result
 **   a3 -- operand 2
 **   a4 -- load/store address base
 **   a5 -- shift amount / boolean scratch
 **   a0 -- call linkage for the outlined helpers (l32r + callx0)
 **
 ** Forward branch targets go through a fixup list (labels bind to
 ** code nodes only when their CMD_TYPE_LABEL is translated).
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include <string.h>

#include "../libutils/mem.h"
#include "../libcore/pfile.h"
#include "../libcore/cmd.h"
#include "../libcore/cmd_op.h"
#include "../libcore/cmd_brch.h"
#include "../libcore/cmd_usec.h"
#include "../libcore/operator.h"
#include "../libcore/value.h"
#include "../libcore/variable.h"
#include "../libcore/label.h"
#include "../libcore/pf_proc.h"
#include "../libcore/pf_log.h"
#include "../libcore/vardef.h"
#include "esp.h"
#include "esp_float.h"
#include "../libcore/wide_alg.h"
#include "../libcore/indirect_alg.h"

/* a branch already consumed by a fused compare+branch pair */

/* esp_helper_t is defined up here, next to the state that owns an
 * array of them (it used to sit 400 lines below). */
typedef struct {
  const char *name;      /* label                                    */
  boolean_t   used;
  boolean_t   signed_op; /* magnitude prep + result sign fixup       */
  boolean_t   remainder; /* result = remainder (a4), not quotient    */
} esp_helper_t;

/*
 * All mutable file-scope state, in ONE object (Tier-A A4; see
 * fix_esp_op_state_reset.md).
 *
 * Nearly all of it is the three growable pending tables -- branch fixups,
 * unconditional-goto literals and call literals -- which accumulate for a
 * whole resolve pass. The helper table is state too (its flags are filled
 * in as the helpers are synthesised); it is the FIRST member so the static
 * initializer below only has to name it and every other member zero-fills.
 *
 * NOT here: `const walg_ops_t esp_walg_ops` -- a constant operations
 * table, not state.
 */
typedef struct {
  esp_code_t node;
  label_t    lbl;
} esp_fixup_t;

typedef struct {
  esp_code_t lit;
  label_t    lbl;
} esp_pend_t;

#define ESP_OP_HELPERS_INIT                                          \
  { {"__esp32_udiv", BOOLEAN_FALSE, BOOLEAN_FALSE, BOOLEAN_FALSE},   \
    {"__esp32_umod", BOOLEAN_FALSE, BOOLEAN_FALSE, BOOLEAN_TRUE},    \
    {"__esp32_sdiv", BOOLEAN_FALSE, BOOLEAN_TRUE,  BOOLEAN_FALSE},   \
    {"__esp32_smod", BOOLEAN_FALSE, BOOLEAN_TRUE,  BOOLEAN_TRUE} }

typedef struct esp_op_state_ {
  esp_helper_t  helpers[4];
  cmd_t         skip_cmd;
  esp_fixup_t  *fixups;
  unsigned      fixup_ct;
  unsigned      fixup_cap;
  esp_pend_t   *goto_pend;
  unsigned      goto_pend_ct;
  unsigned      goto_pend_cap;
  esp_pend_t   *call_pend;
  unsigned      call_pend_ct;
  unsigned      call_pend_cap;
} esp_op_state_t;

static esp_op_state_t esp_op = {
  ESP_OP_HELPERS_INIT,      /* helpers      */
  0,                        /* skip_cmd     */
  0, 0, 0,                  /* fixups       */
  0, 0, 0,                  /* goto pend    */
  0, 0, 0                   /* call pend    */
};

/*
 * TEARDOWN reset -- distinct from esp_op_reset() below, which is the
 * LOGICAL per-compile reset (counters to 0, used flags cleared). This one
 * additionally frees the three pending tables and restores the helper
 * table, so a second compile in the same process starts clean AND
 * leak-free (A4). The tables were never freed by anything; they lived
 * until process exit.
 */
void esp_op_state_reset(void)
{
  static const esp_helper_t helpers_init[4] = ESP_OP_HELPERS_INIT;

  free(esp_op.fixups);
  free(esp_op.goto_pend);
  free(esp_op.call_pend);
  memset(&esp_op, 0, sizeof(esp_op));
  memcpy(esp_op.helpers, helpers_init, sizeof(esp_op.helpers));
}

static void esp_load_value(pfile_t *pf, value_t val, unsigned reg);
static void esp_store_value(pfile_t *pf, value_t dst, unsigned reg);
static boolean_t esp_value_is_bitlike(const value_t val);
static void esp_load_bit(pfile_t *pf, value_t val, unsigned reg);
static void esp_store_bit(pfile_t *pf, value_t val, unsigned vreg);
static void esp_load_bitfield(pfile_t *pf, value_t val, unsigned reg);
static void esp_store_bitfield(pfile_t *pf, value_t val, unsigned vreg);
static void esp_load_deref(pfile_t *pf, value_t ptr, unsigned reg,
    unsigned sz, boolean_t sgn);
static void esp_store_deref(pfile_t *pf, value_t ptr, unsigned reg,
    unsigned sz);
static boolean_t esp_value_is_ptr_deref(const value_t val);
static variable_t esp_bit_container(const value_t val);
static void esp_load_address_of(pfile_t *pf, value_t val, unsigned reg);
static void esp_wide_addr(pfile_t *pf, value_t val, unsigned reg);
static void esp_emit_call_proc(pfile_t *pf, const cmd_t cmd);

/* forward-label fixups: branch nodes waiting for their label's code.
 * Growable: entries accumulate for a whole resolve pass, so a fixed cap
 * silently dropped them on branch-heavy programs. */

static boolean_t esp_fixups_grow(void)
{
  unsigned ncap;
  void    *nd;

  ncap = esp_op.fixup_cap ? (esp_op.fixup_cap * 2U) : 256U;
  nd = REALLOC(esp_op.fixups, ncap * sizeof(*esp_op.fixups));
  if (!nd) {
    return BOOLEAN_FALSE;
  }
  esp_op.fixups = nd;
  esp_op.fixup_cap = ncap;
  return BOOLEAN_TRUE;
}

void esp_add_fixup(pfile_t *pf, esp_code_t node, label_t lbl)
{
  if ((esp_op.fixup_ct == esp_op.fixup_cap) && !esp_fixups_grow()) {
    pfile_log(pf, PFILE_LOG_ERR,
        "ESP32 out of memory growing the branch fixup table");
    return;
  }
  esp_op.fixups[esp_op.fixup_ct].node = node;
  esp_op.fixups[esp_op.fixup_ct].lbl  = lbl;   /* pfile-owned for the run */
  esp_op.fixup_ct++;
}

/* unconditional-goto literals: (pool LIT, label) pairs bound after the
 * labels exist (J's near form needs 4-aligned targets, which 3-byte
 * instruction streams rarely give; l32r + jx always works).  Growable
 * for the same reason as the fixup table. */

static boolean_t esp_goto_pend_grow(void)
{
  unsigned ncap;
  void    *nd;

  ncap = esp_op.goto_pend_cap ? (esp_op.goto_pend_cap * 2U) : 128U;
  nd = REALLOC(esp_op.goto_pend, ncap * sizeof(*esp_op.goto_pend));
  if (!nd) {
    return BOOLEAN_FALSE;
  }
  esp_op.goto_pend = nd;
  esp_op.goto_pend_cap = ncap;
  return BOOLEAN_TRUE;
}

/* pend a goto/proc-label literal for late binding (esp_op_resolve_fixups) */
static void esp_goto_pend_add(pfile_t *pf, label_t lbl, esp_code_t lit)
{
  if (!lit || !lbl) {
    return;
  }
  if ((esp_op.goto_pend_ct == esp_op.goto_pend_cap) && !esp_goto_pend_grow()) {
    pfile_log(pf, PFILE_LOG_ERR,
        "ESP32 out of memory growing the goto literal table");
    return;
  }
  esp_op.goto_pend[esp_op.goto_pend_ct].lit = lit;
  esp_op.goto_pend[esp_op.goto_pend_ct].lbl = lbl;
  esp_op.goto_pend_ct++;
}

/* bind a branch to a label now (backward) or via fixup (forward) */
static void esp_branch_target(pfile_t *pf, esp_code_t node, label_t lbl)
{
  if (node && lbl) {
    esp_code_t tgt = (esp_code_t) label_code_get(lbl);

    if (tgt) {
      node->target = tgt;
    } else {
      esp_add_fixup(pf, node, lbl);
    }
  }
}

/* emit one l32r and bind it to an existing pool literal */
void esp_emit_l32r(unsigned reg, esp_code_t lit)
{
  esp_code_t ldr = esp_code_alloc(ESP_OP_L32R, reg, 0, 0);

  if (ldr) {
    ldr->target = lit;
  }
}

/* extract unsigned bit field rd = rs >> shift (masked to width):
 * the extui src2 operand encodes width-1 */
static void esp_code_extui(unsigned rd, unsigned rs, unsigned shift,
    unsigned width)
{
  esp_code_t _e = esp_code_alloc(ESP_OP_EXTUI, rd, rs, (int) shift);

  if (_e) {
    _e->src2 = width - 1U;
  }
}

void esp_op_resolve_fixups(pfile_t *pf)
{
  unsigned ii;

  for (ii = 0; ii < esp_op.fixup_ct; ii++) {
    esp_code_t tgt = (esp_code_t) label_code_get(esp_op.fixups[ii].lbl);

    if (tgt) {
      esp_op.fixups[ii].node->target = tgt;
    } else {
      pfile_log(pf, PFILE_LOG_ERR,
          "ESP32 branch to undefined label %s",
          label_name_get(esp_op.fixups[ii].lbl));
    }
  }
  esp_op.fixup_ct = 0;
  for (ii = 0; ii < esp_op.goto_pend_ct; ii++) {
    esp_code_t tgt = (esp_code_t) label_code_get(esp_op.goto_pend[ii].lbl);

    if (tgt) {
      esp_op.goto_pend[ii].lit->target = tgt;
    } else {
      pfile_log(pf, PFILE_LOG_ERR,
          "ESP32 goto to undefined label %s",
          label_name_get(esp_op.goto_pend[ii].lbl));
    }
  }
  esp_op.goto_pend_ct = 0;
}

static void esp_load_const(pfile_t *pf, unsigned reg, long value)
{
  if ((value >= -2048L) && (value <= 2047L)) {
    (void) esp_code_alloc(ESP_OP_MOVI, reg, 0, (int) value);
  } else {
    esp_code_t lit = esp_pool_lit((unsigned long) value);
    (void) esp_emit_l32r(reg, lit);
    UNUSED(pf);
  }
}

static esp_opcode_t esp_load_op(unsigned sz, boolean_t sgn)
{
  switch (sz) {
    case 1:  return ESP_OP_L8UI;                     /* no l8si in ISA */
    case 2:  return sgn ? ESP_OP_L16SI : ESP_OP_L16UI;
    default: return ESP_OP_L32I;
  }
}

static esp_opcode_t esp_store_op(unsigned sz)
{
  switch (sz) {
    case 1:  return ESP_OP_S8I;
    case 2:  return ESP_OP_S16I;
    default: return ESP_OP_S32I;
  }
}

/* Xtensa has no signed 8-bit load (MIPS uses LB): widen the byte in
 * reg to 32 bits with its sign (b << 24, then arithmetic >> 24).
 * Scratch a6 (a7 when the value itself sits in a6). */
static void esp_load_sext8(unsigned reg)
{
  unsigned sc = (ESP_REG_A6 == reg) ? ESP_REG_A7 : ESP_REG_A6;

  (void) esp_code_alloc(ESP_OP_MOVI, sc, 0, 24);
  (void) esp_code_alloc(ESP_OP_SSL, 0, sc, 0);
  (void) esp_code_alloc(ESP_OP_SLL, reg, reg, 0);
  (void) esp_code_alloc(ESP_OP_SSR, 0, sc, 0);
  (void) esp_code_alloc(ESP_OP_SRA, reg, reg, 0);
}

/* Address of a variable: allocated GPRs are DRAM offsets from
 * ESP32_RAM_BASE; explicit `at` bases at/above ESP32_ABS_BASE are absolute
 * peripheral/SFR virtual addresses used as-is (mirrors mips_var_ram's
 * KSEG rule for the PIC32 back end). */
unsigned long esp_var_ram(variable_t v)
{
  unsigned long b;

  if (!v) {
    return ESP32_RAM_BASE;
  }
  b = (unsigned long) variable_base_get(v, 0);
  if (b >= ESP32_ABS_BASE) {
    return b;
  }
  return ESP32_RAM_BASE + b;
}

static void esp_load_value(pfile_t *pf, value_t val, unsigned reg);

/* a4 = base + val's baseofs (array subscript / structure member offset,
 * already in bytes).  A constant offset folds into the pool literal; a
 * runtime index is loaded into a5 and added.  The index is evaluated
 * before the base literal is placed because evaluating it may use a4. */
static void esp_emit_direct_addr(pfile_t *pf, unsigned long base, value_t val)
{
  value_t bofs;

  bofs = value_baseofs_get(val);
  if (bofs && !value_is_const(bofs)) {
    esp_load_value(pf, bofs, ESP_REG_A5);
    {
      esp_code_t lit = esp_pool_lit(base);
      (void) esp_emit_l32r(ESP_REG_A4, lit);
    }
    (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A4, ESP_REG_A4,
        (int) ESP_REG_A5);
    return;
  }
  {
    variable_const_t ofs = 0;
    unsigned long    addr = base;

    (void) value_baseofs_const_get(val, &ofs);
    addr += (unsigned long) ofs;
    esp_emit_l32r(ESP_REG_A4, esp_pool_lit(addr));
  }
}

/* load a scalar value into a register (a2/a3/a5); a4 is the base */
static void esp_load_value(pfile_t *pf, value_t val, unsigned reg)
{
  variable_t v;

  esp_value_ensure(pf, val);
  if (esp_value_is_bitlike(val)) {
    if (value_sz_get(val) > 1) {
      esp_load_bitfield(pf, val, reg);
    } else {
      esp_load_bit(pf, val, reg);
    }
    return;
  }
  if (esp_value_is_ptr_deref(val)) {
    /* p[i] / *p anywhere in an expression: load the pointee element
     * by the value's own size (mips_load_value centralizes the deref
     * the same way; a wider assign dst must not widen the load).
     * A pointer-typed deref result is a pointer-array element
     * (slots[i] of `var pointer slots[N] as ...`): the element itself
     * is a 4-byte address, not a 1-byte pointee. */
    unsigned dsz = (unsigned) value_byte_sz_get(val);

    if (!dsz) {
      dsz = 1;
    }
    if (value_is_pointer(val)) {
      dsz = 4;
    }
    esp_load_deref(pf, val, reg, dsz, value_is_signed(val));
    return;
  }
  if (value_is_pointer(val)
      || value_flag_test(val, VALUE_FLAG_PTR_SLOT_RAW)) {
    /* pointer slots are always 4 bytes; the def's baked-in size can
     * be smaller (mirrors mips_load_value's pointer branch) */
    v = value_variable_get(val);
    if (!v) {
      pfile_log(pf, PFILE_LOG_ERR,
          "ESP32: unsupported operand form");
      return;
    }
    esp_emit_direct_addr(pf, esp_var_ram(v), val);
    (void) esp_code_alloc(esp_load_op(4, BOOLEAN_FALSE), reg,
        ESP_REG_A4, 0);
    return;
  }
  if (value_is_const(val)) {
    esp_load_const(pf, reg, (long) value_const_get(val));
    return;
  }
  v = value_variable_get(val);
  if (!v) {
    pfile_log(pf, PFILE_LOG_ERR,
        "ESP32: unsupported operand form");
    return;
  }
  if (variable_is_const(v)) {
    if (variable_data_get(v)) {
      /* const array element: load from IRAM data.  The base address
       * comes from a late-bound pool literal (the data node is placed
       * after all code); the subscript offset is added on top. */
      esp_code_t       lit = esp_const_data_lit(v);
      value_t          bofs;
      variable_const_t ofs;

      bofs = value_baseofs_get(val);
      if (bofs && !value_is_const(bofs)) {
        esp_load_value(pf, bofs, ESP_REG_A5);
      }
      if (lit) {
        esp_emit_l32r(ESP_REG_A4, lit);
      }
      if (bofs && !value_is_const(bofs)) {
        (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A4, ESP_REG_A4,
            (int) ESP_REG_A5);
      } else {
        ofs = 0;
        (void) value_baseofs_const_get(val, &ofs);
        if (ofs) {
          if (ofs <= 127) {
            (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A4, ESP_REG_A4,
                (int) ofs);
          } else {
            esp_emit_l32r(ESP_REG_A5, esp_pool_lit((unsigned long) ofs));
            (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A4, ESP_REG_A4,
                (int) ESP_REG_A5);
          }
        }
      }
      (void) esp_code_alloc(esp_load_op((unsigned) value_byte_sz_get(val),
          value_is_signed(val)), reg, ESP_REG_A4, 0);
      if ((1 == value_byte_sz_get(val)) && value_is_signed(val)) {
        esp_load_sext8(reg);
      }
      return;
    }
    /* named constants (`true`/`false`) are const values with an
     * unplaced variable -- read the folded constant */
    esp_load_const(pf, reg, (long) value_const_get(val));
    return;
  }
  esp_emit_direct_addr(pf, esp_var_ram(v), val);
  (void) esp_code_alloc(esp_load_op((unsigned) value_byte_sz_get(val),
      value_is_signed(val)), reg, ESP_REG_A4, 0);
  if ((1 == value_byte_sz_get(val)) && value_is_signed(val)) {
    esp_load_sext8(reg);
  }
}

/* store reg into a direct RAM variable by its byte size */
static void esp_store_value(pfile_t *pf, value_t dst, unsigned reg)
{
  variable_t v;

  esp_value_ensure(pf, dst);
  if (esp_value_is_bitlike(dst)) {
    if (value_sz_get(dst) > 1) {
      esp_store_bitfield(pf, dst, reg);
    } else {
      esp_store_bit(pf, dst, reg);
    }
    return;
  }
  if (esp_value_is_ptr_deref(dst)) {
    /* p[i] = v: the value carries the pointee def (VALUE_FLAG_POINTER),
     * so the def-based pointer test below misses it (mips_store_value
     * checks the deref first for the same reason) */
    esp_store_deref(pf, dst, reg, (unsigned) value_byte_sz_get(dst));
    return;
  }
  if (value_is_pointer(dst)
      || value_flag_test(dst, VALUE_FLAG_PTR_SLOT_RAW)) {
    /* pointer slots are always 4 bytes; the def's baked-in size can
     * be smaller (mirrors mips_store_value's pointer branch) */
    v = value_variable_get(dst);
    if (!v) {
      pfile_log(pf, PFILE_LOG_ERR,
          "ESP32: assignment target is not a direct variable");
      return;
    }
    esp_emit_direct_addr(pf, esp_var_ram(v), dst);
    (void) esp_code_alloc(esp_store_op(4), reg, ESP_REG_A4, 0);
    return;
  }
  v = value_variable_get(dst);
  if (!v) {
    pfile_log(pf, PFILE_LOG_ERR,
        "ESP32: assignment target is not a direct variable");
    return;
  }
  esp_emit_direct_addr(pf, esp_var_ram(v), dst);
  (void) esp_code_alloc(esp_store_op((unsigned) value_byte_sz_get(dst)),
      reg, ESP_REG_A4, 0);
}

/* ---------------- divide / mod helpers (outlined) ---------------- */



/* the helper's label (created on first use; helpers_create binds it to
 * a code node placed after all main code). Ownership: the caller receives
 * one reference (find lock / alloc) and must label_release it — the block
 * label list keeps the label itself alive (mips_opfn.c convention). */
static label_t esp_helper_lbl_ensure(pfile_t *pf, unsigned idx)
{
  label_t lbl = pfile_label_find(pf, PFILE_LOG_NONE, esp_op.helpers[idx].name);

  if (!lbl) {
    lbl = pfile_label_alloc(pf, esp_op.helpers[idx].name);
  }
  return lbl;
}

/* pending call literals: (pool LIT, helper label) pairs bound when the
 * helper body is emitted at the end of the program */
/* late-binding helper-call literals: (pool LIT, label) pairs whose
 * label's code node is created after the main code (outlined div/mod,
 * float, task helpers).  Call sites accumulate for the whole compile,
 * so the table grows on demand -- a fixed 64-slot cap once silently
 * dropped entries and the L32R then failed with "without literal", far
 * from the cause. */

static boolean_t esp_call_pend_grow(void)
{
  unsigned ncap;
  void    *nd;

  ncap = esp_op.call_pend_cap ? (esp_op.call_pend_cap * 2U) : 64U;
  nd = REALLOC(esp_op.call_pend, ncap * sizeof(*esp_op.call_pend));
  if (!nd) {
    return BOOLEAN_FALSE;
  }
  esp_op.call_pend = nd;
  esp_op.call_pend_cap = ncap;
  return BOOLEAN_TRUE;
}

/* register a pool literal to be bound to lbl's code node once that
 * node exists (esp_op_helper_bind) */
void esp_pend_label_lit(pfile_t *pf, label_t lbl, esp_code_t lit)
{
  if (!lit || !lbl) {
    return;
  }
  if ((esp_op.call_pend_ct == esp_op.call_pend_cap) && !esp_call_pend_grow()) {
    pfile_log(pf, PFILE_LOG_ERR,
        "ESP32 out of memory growing the helper-call literal table");
    return;
  }
  esp_op.call_pend[esp_op.call_pend_ct].lit = lit;
  esp_op.call_pend[esp_op.call_pend_ct].lbl = lbl;
  esp_op.call_pend_ct++;
}

/* emit one l32r whose literal is pended for lbl's code node; the
 * literal is created even without a label so the failure surfaces at
 * assembly as an unbound L32R */
static void esp_emit_pend_l32r(pfile_t *pf, unsigned reg, label_t lbl)
{
  esp_code_t lit = esp_pool_lit_new();

  esp_pend_label_lit(pf, lbl, lit);
  if (lit) {
    esp_emit_l32r(reg, lit);
  }
}

/* emit one register call0 call through l32r + callx0; the literal is
 * bound to the helper's code node later (esp_helpers_create) */
static void esp_emit_call(pfile_t *pf, unsigned idx)
{
  label_t lbl = esp_helper_lbl_ensure(pf, idx);

  esp_emit_pend_l32r(pf, ESP_REG_A0, lbl);
  (void) esp_code_alloc(ESP_OP_CALLX0, 0, ESP_REG_A0, 0);
  if (lbl) {
    label_release(lbl);
  }
}

/* call / jump a late-created helper by label name (task intrinsics):
 * the body is emitted after the main code, so the literal is pended in
 * esp_op.call_pend and bound when the helper's label node appears */
void esp_emit_helper_call(pfile_t *pf, const char *name)
{
  label_t lbl = esp_task_helper_label(pf, name);

  esp_emit_pend_l32r(pf, ESP_REG_A0, lbl);
  (void) esp_code_alloc(ESP_OP_CALLX0, 0, ESP_REG_A0, 0);
  if (lbl) {
    label_release(lbl);
  }
}

void esp_emit_helper_jmp(pfile_t *pf, const char *name)
{
  label_t lbl = esp_task_helper_label(pf, name);

  esp_emit_pend_l32r(pf, ESP_REG_A5, lbl);
  (void) esp_code_alloc(ESP_OP_JX, 0, ESP_REG_A5, 0);
  if (lbl) {
    label_release(lbl);
  }
}

/* bind every pending helper literal referencing lbl to its code node */
void esp_op_helper_bind(label_t lbl, esp_code_t node)
{
  unsigned pp;

  for (pp = 0; pp < esp_op.call_pend_ct; pp++) {
    if ((esp_op.call_pend[pp].lbl == lbl)
        && !esp_op.call_pend[pp].lit->target) {
      esp_op.call_pend[pp].lit->target = node;
    }
  }
}

/* restoring shift-subtract core: a2 = num, a3 = den
 * -> a2 = quotient, a4 = remainder (clobbers a5-a7) */
static void esp_emit_udiv_core(void)
{
  esp_code_t loop;
  esp_code_t skip;
  esp_code_t br;

  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A6, 0, 32);   /* bit counter */
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A4, 0, 0);    /* remainder   */
  loop = esp_code_label_alloc();
  /* rem = (rem << 1) | (num >> 31); num <<= 1 */
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A5, 0, 31);
  (void) esp_code_alloc(ESP_OP_SSR, 0, ESP_REG_A5, 0);
  (void) esp_code_alloc(ESP_OP_SRL, ESP_REG_A7, ESP_REG_A2, 0);
  (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A4, ESP_REG_A4, (int) ESP_REG_A4);
  (void) esp_code_alloc(ESP_OP_OR,  ESP_REG_A4, ESP_REG_A4, (int) ESP_REG_A7);
  (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A2, ESP_REG_A2, (int) ESP_REG_A2);
  br = esp_code_alloc(ESP_OP_BLTU, ESP_REG_A3, ESP_REG_A4, 0);
  (void) esp_code_alloc(ESP_OP_SUB,  ESP_REG_A4, ESP_REG_A4, ESP_REG_A3);
  (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A2, ESP_REG_A2, 1);
  skip = esp_code_label_alloc();
  if (br) {
    br->target = skip;                    /* rem < den: keep subtracting off */
  }
  (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A6, ESP_REG_A6, -1);
  br = esp_code_alloc(ESP_OP_BNEZ, 0, ESP_REG_A6, 0);
  if (br) {
    br->target = loop;
  }
}

static void esp_emit_helper_body(unsigned idx)
{
  esp_code_t br;
  esp_code_t lbl;

  if (esp_op.helpers[idx].signed_op) {
    /* magnitudes first; a8 = num negative, a9 = den negative, a7 = 0.
     * The flags must live in a8/a9: the udiv core clobbers a5 (shift
     * scratch) and a6 (bit counter), and the post-core sign fix-ups
     * read the flags -- on a5/a6 they tested garbage (remainder
     * always negated; found via print_float's sbyte exponent). */
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A7, 0, 0);
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A8, 0, 0);
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A9, 0, 0);
    br = esp_code_alloc(ESP_OP_BGE, ESP_REG_A7, ESP_REG_A2, 0);
    (void) esp_code_alloc(ESP_OP_NEG, ESP_REG_A2, ESP_REG_A2, 0);
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A8, 0, 1);
    lbl = esp_code_label_alloc();
    if (br) { br->target = lbl; }
    br = esp_code_alloc(ESP_OP_BGE, ESP_REG_A7, ESP_REG_A3, 0);
    (void) esp_code_alloc(ESP_OP_NEG, ESP_REG_A3, ESP_REG_A3, 0);
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A9, 0, 1);
    lbl = esp_code_label_alloc();
    if (br) { br->target = lbl; }
    esp_emit_udiv_core();
    if (esp_op.helpers[idx].remainder) {
      /* remainder takes the dividend's sign (a8) */
      br = esp_code_alloc(ESP_OP_BEQZ, 0, ESP_REG_A8, 0);
      (void) esp_code_alloc(ESP_OP_NEG, ESP_REG_A4, ESP_REG_A4, 0);
      lbl = esp_code_label_alloc();
      if (br) { br->target = lbl; }
      (void) esp_code_alloc(ESP_OP_OR, ESP_REG_A2, ESP_REG_A4, ESP_REG_A4);
    } else {
      /* quotient negative iff exactly one operand was negative */
      (void) esp_code_alloc(ESP_OP_XOR, ESP_REG_A8, ESP_REG_A8, ESP_REG_A9);
      br = esp_code_alloc(ESP_OP_BEQZ, 0, ESP_REG_A8, 0);
      (void) esp_code_alloc(ESP_OP_NEG, ESP_REG_A2, ESP_REG_A2, 0);
      lbl = esp_code_label_alloc();
      if (br) { br->target = lbl; }
    }
  } else {
    esp_emit_udiv_core();
    if (esp_op.helpers[idx].remainder) {
      (void) esp_code_alloc(ESP_OP_OR, ESP_REG_A2, ESP_REG_A4, ESP_REG_A4);
    }
  }
  (void) esp_code_alloc(ESP_OP_JX, 0, ESP_REG_A0, 0);      /* return */
}

void esp_helpers_create(pfile_t *pf)
{
  unsigned ii;

  for (ii = 0; ii < 4; ii++) {
    if (esp_op.helpers[ii].used) {
      label_t    lbl = esp_helper_lbl_ensure(pf, ii);
      esp_code_t node;

      if (!lbl) {
        continue;
      }
      node = (esp_code_t) label_code_get(lbl);
      if (!node) {
        node = esp_code_label_alloc();
        label_code_set(lbl, node);
        esp_emit_helper_body(ii);
      }
      /* bind every pending call literal for this helper (the pended
       * labels are the same pfile label objects) */
      esp_op_helper_bind(lbl, node);
      label_release(lbl);
    }
  }
}

/* emit a div/mod: v1 (num) -> a2, v2 (den) -> a3, call, result in a2 */
static void esp_emit_divmod(pfile_t *pf, operator_t op,
    value_t v1, value_t v2)
{
  unsigned   idx;
  boolean_t  sgn;

  sgn = value_is_signed(v1) || value_is_signed(v2);
  idx = (OPERATOR_MOD == op) ? (sgn ? 3 : 1) : (sgn ? 2 : 0);
  esp_op.helpers[idx].used = BOOLEAN_TRUE;
  esp_load_value(pf, v2, ESP_REG_A3);
  esp_load_value(pf, v1, ESP_REG_A2);
  esp_emit_call(pf, idx);
}

/* ---------------- comparisons and branches ---------------- */

/* branch opcode TAKEN when the relation holds between (s=as, t=at);
 * LE/GT swap operands since the ISA has no ble/bgt. */
static esp_opcode_t esp_rel_branch(operator_t op, boolean_t sgn,
    boolean_t *swap)
{
  *swap = BOOLEAN_FALSE;
  switch (op) {
    case OPERATOR_EQ:  return ESP_OP_BEQ;
    case OPERATOR_NE:  return ESP_OP_BNE;
    case OPERATOR_LT:  return sgn ? ESP_OP_BLT  : ESP_OP_BLTU;
    case OPERATOR_GE:  return sgn ? ESP_OP_BGE  : ESP_OP_BGEU;
    case OPERATOR_GT:  *swap = BOOLEAN_TRUE;  return sgn ? ESP_OP_BLT  : ESP_OP_BLTU;
    case OPERATOR_LE:  *swap = BOOLEAN_TRUE;  return sgn ? ESP_OP_BGE  : ESP_OP_BGEU;
    default:           return ESP_OP_BEQ;
  }
}

static esp_opcode_t esp_rel_branch_invert(esp_opcode_t bop)
{
  switch (bop) {
    case ESP_OP_BEQ:  return ESP_OP_BNE;
    case ESP_OP_BNE:  return ESP_OP_BEQ;
    case ESP_OP_BLT:  return ESP_OP_BGE;
    case ESP_OP_BGE:  return ESP_OP_BLT;
    case ESP_OP_BLTU: return ESP_OP_BGEU;
    default:          return ESP_OP_BLTU;   /* BGEU */
  }
}

/* fused compare + conditional branch (relation with no dst pairs with
 * the following branch cmd) */
static void esp_emit_compare_branch(pfile_t *pf, const cmd_t cmd,
    operator_t op, value_t v1, value_t v2)
{
  cmd_t         next = cmd_next_exec_get(cmd_link_get(cmd));
  label_t       lbl;
  boolean_t     swap;
  boolean_t     sgn;
  esp_opcode_t  bop;
  esp_code_t    br;
  unsigned      rs;
  unsigned      rt;

  if (!next || (CMD_TYPE_BRANCH != cmd_type_get(next))
      || (CMD_BRANCHTYPE_GOTO != cmd_brtype_get(next))
      || (CMD_BRANCHCOND_NONE == cmd_brcond_get(next))) {
    return;
  }
  lbl = cmd_brdst_get(next);
  if (!lbl) {
    return;
  }
  sgn = value_is_signed(v1) || value_is_signed(v2);
  bop = esp_rel_branch(op, sgn, &swap);
  if (CMD_BRANCHCOND_FALSE == cmd_brcond_get(next)) {
    bop = esp_rel_branch_invert(bop);
  }
  esp_load_value(pf, v2, ESP_REG_A3);
  esp_load_value(pf, v1, ESP_REG_A2);
  rs = swap ? ESP_REG_A3 : ESP_REG_A2;
  rt = swap ? ESP_REG_A2 : ESP_REG_A3;
  br = esp_code_alloc(bop, rt, rs, 0);
  esp_branch_target(pf, br, lbl);
  esp_op.skip_cmd = next;          /* consume the branch cmd */
}

/* `!v` / truth-test `v` followed by goto true/false: branch directly
 * on the operand (mirrors mips_emit_not_branch / truth_branch).
 * invert=TRUE (NOTL): condition true means v == 0. */
static void esp_emit_truth_branch(pfile_t *pf, const cmd_t cmd, value_t v1,
    boolean_t invert)
{
  cmd_t       next = cmd_next_exec_get(cmd_link_get(cmd));
  label_t     lbl;
  esp_code_t  br;
  boolean_t   br_true;

  if (!next || (CMD_TYPE_BRANCH != cmd_type_get(next))
      || (CMD_BRANCHTYPE_GOTO != cmd_brtype_get(next))
      || (CMD_BRANCHCOND_NONE == cmd_brcond_get(next))) {
    return;
  }
  lbl = cmd_brdst_get(next);
  if (!lbl || !v1) {
    return;
  }
  br_true = (CMD_BRANCHCOND_TRUE == cmd_brcond_get(next));
  if (!br_true) {
    invert = !invert;
  }
  esp_load_value(pf, v1, ESP_REG_A2);
  br = esp_code_alloc(invert ? ESP_OP_BEQZ : ESP_OP_BNEZ, 0,
      ESP_REG_A2, 0);
  esp_branch_target(pf, br, lbl);
  esp_op.skip_cmd = next;          /* consume the branch cmd */
}

/* materialize a comparison into dst (movi 0 / branch-over / movi 1) */
static void esp_emit_compare_bool(pfile_t *pf, value_t dst,
    operator_t op, value_t v1, value_t v2)
{
  boolean_t     swap;
  boolean_t     sgn;
  esp_opcode_t  bop;
  esp_code_t    br;
  unsigned      rs;
  unsigned      rt;

  sgn = value_is_signed(v1) || value_is_signed(v2);
  bop = esp_rel_branch_invert(esp_rel_branch(op, sgn, &swap));
  esp_load_value(pf, v2, ESP_REG_A3);
  esp_load_value(pf, v1, ESP_REG_A2);
  rs = swap ? ESP_REG_A3 : ESP_REG_A2;
  rt = swap ? ESP_REG_A2 : ESP_REG_A3;
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A5, 0, 0);
  br = esp_code_alloc(bop, rt, rs, 0);
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A5, 0, 1);
  {
    esp_code_t skip = esp_code_label_alloc();

    if (br) {
      br->target = skip;        /* relation false: keep 0 */
    }
  }
  esp_store_value(pf, dst, ESP_REG_A5);
}

/* ---------------- float relations ---------------- */

/* Load two float operands into a2/a3 (an int operand converts
 * through _esp_i2f). */
static void esp_float_loads(pfile_t *pf, value_t v1, value_t v2)
{
  if (v2) {
    if (value_is_float(v2)) {
      esp_load_value(pf, v2, ESP_REG_A3);
    } else {
      esp_load_value(pf, v2, ESP_REG_A2);
      esp_float_call(pf, 5U);                 /* (float)v2 */
      (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A3, ESP_REG_A2, 0);
    }
  } else {
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A3, 0, 0);
  }
  esp_load_value(pf, v1, ESP_REG_A2);
  if (!value_is_float(v1)) {
    esp_float_call(pf, 5U);                   /* (float)v1 */
  }
}

/* Turn both a2/a3 float bit patterns into unsigned sort keys:
 * positives -> bits ^ 0x80000000 (upper half, order kept),
 * negatives -> ~bits (lower half, order reversed).  Unsigned key
 * order then equals float order, so the plain integer relation
 * branches work.  +-inf orders correctly; NaN and the -0.0/+0.0
 * pair are handled by esp_float_guards before the keys. */
static void esp_float_keyify(void)
{
  esp_emit_l32r(ESP_REG_A6, esp_pool_lit(0x80000000UL));
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A4, 0, 31);
  (void) esp_code_alloc(ESP_OP_SSR, 0, ESP_REG_A4, 0);
  (void) esp_code_alloc(ESP_OP_SRA, ESP_REG_A5, ESP_REG_A2, 0);
  (void) esp_code_alloc(ESP_OP_OR, ESP_REG_A5, ESP_REG_A5,
      (int) ESP_REG_A6);                        /* mask = 0x80000000|sgn */
  (void) esp_code_alloc(ESP_OP_XOR, ESP_REG_A2, ESP_REG_A2,
      (int) ESP_REG_A5);                        /* key1 */
  (void) esp_code_alloc(ESP_OP_SRA, ESP_REG_A5, ESP_REG_A3, 0);
  (void) esp_code_alloc(ESP_OP_OR, ESP_REG_A5, ESP_REG_A5,
      (int) ESP_REG_A6);
  (void) esp_code_alloc(ESP_OP_XOR, ESP_REG_A3, ESP_REG_A3,
      (int) ESP_REG_A5);                        /* key2 */
}

/* NaN / +-0 constant-result guards for float relations (IEEE):
 * any NaN makes every relation false (NE true); two zeros with
 * any sign combination compare equal.  Emitted after
 * esp_float_loads, before the keys.  Constant-result exit jumps
 * are collected in ftrue / ffalse for the caller to bind (the
 * caller knows where TRUE and FALSE continue).  Clobbers a8/a9. */
static void esp_float_guards(operator_t op,
    esp_code_t *ftrue, int *ntrue, esp_code_t *ffalse, int *nfalse)
{
  boolean_t  nt = (OPERATOR_NE == op);
  boolean_t  zt = (OPERATOR_EQ == op) || (OPERATOR_LE == op)
              || (OPERATOR_GE == op);
  esp_code_t bsp, bnn;
  unsigned   reg;

  for (reg = ESP_REG_A2; reg <= ESP_REG_A3; reg++) {
    /* NaN? (x << 1) > 0xFF000000 with mantissa != 0 */
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A9, 0, 1);
    (void) esp_code_alloc(ESP_OP_SSL, 0, ESP_REG_A9, 0);
    (void) esp_code_alloc(ESP_OP_SLL, ESP_REG_A8, reg, 0);
    esp_emit_l32r(ESP_REG_A9, esp_pool_lit(0xFF000000UL));
    /* alloc(op, X, Y) branches when (Y op X): taken -- skip the
     * constant exit -- for x<<1 <u 0xFF000000, i.e. normals and +-0 */
    bsp = esp_code_alloc(ESP_OP_BLTU, ESP_REG_A9, ESP_REG_A8, 0);
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A9, 0, 9);
    (void) esp_code_alloc(ESP_OP_SSL, 0, ESP_REG_A9, 0);
    (void) esp_code_alloc(ESP_OP_SLL, ESP_REG_A8, reg, 0);
    bnn = esp_code_alloc(ESP_OP_BEQZ, 0, ESP_REG_A8, 0);
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A9, 0, 0);
    if (nt) {
      ftrue[(*ntrue)++] = esp_code_alloc(ESP_OP_BEQZ, 0, ESP_REG_A9, 0);
    } else {
      ffalse[(*nfalse)++] = esp_code_alloc(ESP_OP_BEQZ, 0, ESP_REG_A9, 0);
    }
    {
      esp_code_t l = esp_code_label_alloc();

      if (bsp) {
        bsp->target = l;        /* not special: no NaN */
      }
      if (bnn) {
        bnn->target = l;        /* inf: no NaN */
      }
    }
  }
  /* both zeros (either sign)? */
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A9, 0, 1);
  (void) esp_code_alloc(ESP_OP_SSL, 0, ESP_REG_A9, 0);
  (void) esp_code_alloc(ESP_OP_SLL, ESP_REG_A8, ESP_REG_A2, 0);
  bsp = esp_code_alloc(ESP_OP_BNEZ, 0, ESP_REG_A8, 0);
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A9, 0, 1);
  (void) esp_code_alloc(ESP_OP_SSL, 0, ESP_REG_A9, 0);
  (void) esp_code_alloc(ESP_OP_SLL, ESP_REG_A8, ESP_REG_A3, 0);
  bnn = esp_code_alloc(ESP_OP_BNEZ, 0, ESP_REG_A8, 0);
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A9, 0, 0);
  if (zt) {
    ftrue[(*ntrue)++] = esp_code_alloc(ESP_OP_BEQZ, 0, ESP_REG_A9, 0);
  } else {
    ffalse[(*nfalse)++] = esp_code_alloc(ESP_OP_BEQZ, 0, ESP_REG_A9, 0);
  }
  {
    esp_code_t l = esp_code_label_alloc();

    if (bsp) {
      bsp->target = l;          /* operand1 nonzero */
    }
    if (bnn) {
      bnn->target = l;          /* operand2 nonzero */
    }
  }
}

/* float relation fused with the following goto: NaN/zero guards,
 * then branch on the sort keys */
static void esp_emit_float_compare_branch(pfile_t *pf, const cmd_t cmd,
    operator_t op, value_t v1, value_t v2)
{
  cmd_t         next = cmd_next_exec_get(cmd_link_get(cmd));
  label_t       lbl;
  boolean_t     swap;
  boolean_t     cond_false;
  esp_opcode_t  bop;
  esp_code_t    br;
  esp_code_t    ft[4];
  esp_code_t    ff[4];
  int           nft = 0;
  int           nff = 0;
  int           i;
  unsigned      rs;
  unsigned      rt;

  if (!next || (CMD_TYPE_BRANCH != cmd_type_get(next))
      || (CMD_BRANCHTYPE_GOTO != cmd_brtype_get(next))
      || (CMD_BRANCHCOND_NONE == cmd_brcond_get(next))) {
    return;
  }
  lbl = cmd_brdst_get(next);
  if (!lbl) {
    return;
  }
  cond_false = (CMD_BRANCHCOND_FALSE == cmd_brcond_get(next));
  bop = esp_rel_branch(op, BOOLEAN_FALSE, &swap);   /* keys: unsigned */
  if (cond_false) {
    bop = esp_rel_branch_invert(bop);
  }
  esp_float_loads(pf, v1, v2);
  esp_float_guards(op, ft, &nft, ff, &nff);
  esp_float_keyify();
  rs = swap ? ESP_REG_A3 : ESP_REG_A2;
  rt = swap ? ESP_REG_A2 : ESP_REG_A3;
  br = esp_code_alloc(bop, rt, rs, 0);
  esp_branch_target(pf, br, lbl);
  esp_op.skip_cmd = next;          /* consume the branch cmd */
  {
    esp_code_t l = esp_code_label_alloc();

    /* A cond-false goto is the skip-body branch of an `if`: the
     * relation being true must fall into the body, false must take
     * the goto -- the same continuations the key branch above uses.
     * Cond-true gotos are the mirror image. */
    if (cond_false) {
      for (i = 0; i < nft; i++) {
        if (ft[i]) {
          ft[i]->target = l;    /* constant true: fall into body */
        }
      }
      for (i = 0; i < nff; i++) {
        esp_branch_target(pf, ff[i], lbl);  /* constant false: skip */
      }
    } else {
      for (i = 0; i < nft; i++) {
        esp_branch_target(pf, ft[i], lbl);  /* constant true: goto */
      }
      for (i = 0; i < nff; i++) {
        if (ff[i]) {
          ff[i]->target = l;    /* constant false: fall through */
        }
      }
    }
  }
}

/* materialize a float relation into dst (0/1 via branch-over) */
static void esp_emit_float_compare_bool(pfile_t *pf, value_t dst,
    operator_t op, value_t v1, value_t v2)
{
  boolean_t     swap;
  esp_opcode_t  bop;
  esp_code_t    br;
  esp_code_t    ft[4];
  esp_code_t    ff[4];
  esp_code_t    l_true;
  esp_code_t    l_zero;
  int           nft = 0;
  int           nff = 0;
  int           i;
  unsigned      rs;
  unsigned      rt;

  bop = esp_rel_branch_invert(esp_rel_branch(op, BOOLEAN_FALSE, &swap));
  esp_float_loads(pf, v1, v2);
  esp_float_guards(op, ft, &nft, ff, &nff);
  esp_float_keyify();
  rs = swap ? ESP_REG_A3 : ESP_REG_A2;
  rt = swap ? ESP_REG_A2 : ESP_REG_A3;
  br = esp_code_alloc(bop, rt, rs, 0);      /* taken: relation false */
  l_true = esp_code_label_alloc();
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A7, 0, 1);
  {
    /* skip over the zeroing: a7 is 1 here, so this bnez is an
     * unconditional hop (near J needs a 4-aligned target) */
    esp_code_t lj = esp_code_alloc(ESP_OP_BNEZ, 0, ESP_REG_A7, 0);
    esp_code_t l_end;

    l_zero = esp_code_label_alloc();
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A7, 0, 0);
    l_end = esp_code_label_alloc();
    if (lj) {
      lj->target = l_end;
    }
    if (br) {
      br->target = l_zero;       /* keys say false */
    }
    for (i = 0; i < nff; i++) {
      if (ff[i]) {
        ff[i]->target = l_zero;  /* guard constant false */
      }
    }
  }
  for (i = 0; i < nft; i++) {
    if (ft[i]) {
      ft[i]->target = l_true;    /* constant true: 1 */
    }
  }
  esp_store_value(pf, dst, ESP_REG_A7);
}

/* branch on a boolean value (or unconditional goto when brcond NONE) */
static void esp_emit_branch(pfile_t *pf, const cmd_t cmd)
{
  cmd_branchcond_t bcond = cmd_brcond_get(cmd);
  label_t          lbl   = cmd_brdst_get(cmd);
  value_t          brv   = cmd_brval_get(cmd);
  esp_code_t       node;

  if (!lbl) {
    return;
  }
  node = (esp_code_t) label_code_get(lbl);
  if (CMD_BRANCHCOND_NONE == bcond) {
    esp_code_t lit;

    /* Tail return (`cmd_opt_tail_return`): GOTO + callee proc.  The
     * optimizer removes the wrapper's PROC_LEAVE after rewriting the
     * call.  Emit a real call (marshal IN, callx0, marshal OUT) plus
     * the missing leave (restore a0, pop the frame, jx a0) -- a plain
     * goto would leak the pushed a0 and corrupt the caller's frame. */
    if (cmd_brproc_get(cmd) && value_proc_get(cmd_brproc_get(cmd))) {
      esp_emit_call_proc(pf, cmd);
      (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A0, ESP_REG_A1, 0);
      (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A1, ESP_REG_A1, 4);
      (void) esp_code_alloc(ESP_OP_JX, 0, ESP_REG_A0, 0);
      return;
    }
    /* unconditional goto: near-J needs a 4-aligned delta (rare with
     * 3-byte instructions), so always use l32r + jx through a label
     * literal -- always encodable, any target, any direction */
    lit = esp_pool_lit_new();

    esp_goto_pend_add(pf, lbl, lit);
    if (lit) {
      esp_emit_l32r(ESP_REG_A5, lit);
    }
    (void) esp_code_alloc(ESP_OP_JX, 0, ESP_REG_A5, 0);
    return;
  }
  UNUSED(node);
  if (!brv) {
    return;
  }
  esp_load_value(pf, brv, ESP_REG_A2);
  {
    esp_code_t br = esp_code_alloc(
        (CMD_BRANCHCOND_FALSE == bcond)
            ? ESP_OP_BEQZ : ESP_OP_BNEZ, 0, ESP_REG_A2, 0);

    esp_branch_target(pf, br, lbl);
  }
}

/* ---------------- packed bits and multi-bit fields (3a / 3d) --- */

/* containing storage variable of a (possibly packed) value: walk the
 * master chain (`_btmp`/bucket aliases) to the real storage */
static variable_t esp_bit_container(const value_t val)
{
  variable_t v = value_variable_get(val);

  while (v) {
    variable_t m = variable_master_get(v);

    if (!m) {
      break;
    }
    v = m;
  }
  return v;
}

static boolean_t esp_bit_is_runtime(const value_t val)
{
  value_t b = value_baseofs_get(val);

  return (b && !value_is_const(b)) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

static boolean_t esp_value_is_bitlike(const value_t val)
{
  variable_def_t def = value_def_get(val);

  if (!def || !variable_def_flag_test(def, VARIABLE_DEF_FLAG_BIT)
      || value_is_pointer(val) || value_is_const(val)
      || !value_variable_get(val)) {
    /* `true`/`false` are FLAG_BIT consts with an unplaced variable --
     * they must load as plain constants, never as packed bits */
    return BOOLEAN_FALSE;
  }
  return BOOLEAN_TRUE;
}

/* Peripheral-space test: the DRAM allocator starts at 0x3FFB0000;
 * SFRs live below that (0x3F400000+ PERI, 0x3FF00000+ DPORT,
 * 0x3FF48000+ RTC, 0x60000000+ IO). Byte lanes (l8ui/s8i) on
 * peripheral registers are speed-sensitive on silicon: a byte RMW of
 * GPIO_OUT garbles the chip at 80/160 MHz while appearing fine at
 * 240 (measured 2026-10-08) -- SFR bit access must RMW in 32 bits. */
#define ESP_PERI_LO 0x3F400000UL
#define ESP_DRAM_BASE 0x3FFB0000UL
static boolean_t esp_addr_is_peripheral(unsigned long addr)
{
  return ((addr >= ESP_PERI_LO) && (addr < ESP_DRAM_BASE))
    || (addr >= 0x60000000UL) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* single-bit load: reg = 0/1.  Scratch a4/a5/a6 (never the operand
 * regs); a reg in the scratch set is staged through a7.
 * thru (3c): val reads through a pointer slot (open bit formal); the
 * index is buffer-relative (leaked bit_offset dropped), the 32-bit slot
 * is loaded into a4 and the byte offset added on top. */
static void esp_load_bit(pfile_t *pf, value_t val, unsigned reg)
{
  variable_t cv = esp_bit_container(val);
  variable_t sv = value_variable_get(val);
  boolean_t  thru = esp_value_is_ptr_deref(val);
  unsigned   rd = reg;

  if ((ESP_REG_A4 == reg) || (ESP_REG_A5 == reg) || (ESP_REG_A6 == reg)) {
    rd = ESP_REG_A7;
  }
  if (!cv) {
    pfile_log(pf, PFILE_LOG_ERR, "ESP32 bit without storage");
    return;
  }
  if (esp_bit_is_runtime(val)) {
    unsigned bit_base = thru ? 0U : value_bit_offset_get(val);

    /* a5 = bit index; a6 = byte offset; a7 = bit-in-byte */
    esp_load_value(pf, value_baseofs_get(val), ESP_REG_A5);
    if (bit_base) {
      /* ADDI's imm8 is SIGNED: a pool bit offset >= 128 wraps to
       * -128 and the byte address goes invalid (LoadStoreError) --
       * add large bases through a pool literal instead */
      if (bit_base <= 127U) {
        (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A5, ESP_REG_A5,
            (int) bit_base);
      } else {
        esp_emit_l32r(ESP_REG_A6, esp_pool_lit((unsigned long) bit_base));
        (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A5, ESP_REG_A5,
            (int) ESP_REG_A6);
      }
    }
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A6, 0, 3);
    (void) esp_code_alloc(ESP_OP_SSR, 0, ESP_REG_A6, 0);
    (void) esp_code_alloc(ESP_OP_SRL, ESP_REG_A6, ESP_REG_A5, 0);
    esp_code_extui(ESP_REG_A5, ESP_REG_A5, 0, 3);
    if (thru) {
      esp_code_t lit = esp_pool_lit(esp_var_ram(sv));
      (void) esp_emit_l32r(ESP_REG_A4, lit);
      (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A4, ESP_REG_A4, 0);
    } else {
      unsigned long addr = esp_var_ram(cv);
      esp_code_t lit = esp_pool_lit(addr);
      (void) esp_emit_l32r(ESP_REG_A4, lit);
    }
    (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A4, ESP_REG_A4, (int) ESP_REG_A6);
    (void) esp_code_alloc(ESP_OP_L8UI, rd, ESP_REG_A4, 0);
    (void) esp_code_alloc(ESP_OP_SSR, 0, ESP_REG_A5, 0);
    (void) esp_code_alloc(ESP_OP_SRL, rd, rd, 0);
    esp_code_extui(reg, rd, 0, 1);
    return;
  }
  {
    variable_const_t off = 0;
    unsigned         pos;
    unsigned         bit_in;

    (void) value_baseofs_const_get(val, &off);
    pos = (thru ? 0U : value_bit_offset_get(val)) + (unsigned) off;
    bit_in = pos & 7U;
    if (thru) {
      unsigned long by = (unsigned long) (pos >> 3);
      esp_code_t lit = esp_pool_lit(esp_var_ram(sv));
      (void) esp_emit_l32r(ESP_REG_A4, lit);
      (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A4, ESP_REG_A4, 0);
      if (by) {
        if (by <= 127UL) {
          (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A4, ESP_REG_A4,
              (int) by);
        } else {
          esp_emit_l32r(ESP_REG_A6, esp_pool_lit(by));
          (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A4, ESP_REG_A4,
              (int) ESP_REG_A6);
        }
      }
    } else {
      unsigned long addr = esp_var_ram(cv) + (pos >> 3);
      esp_code_t    lit;

      if (esp_addr_is_peripheral(addr)) {
        addr &= ~3UL;   /* SFR word access: aligned register (see below) */
      }
      lit = esp_pool_lit(addr);
      (void) esp_emit_l32r(ESP_REG_A4, lit);
    }
    if (esp_addr_is_peripheral(esp_var_ram(cv) + (pos >> 3))) {
      /* SFR: 32-bit read, shift within the word.  The word access must
       * target the ALIGNED register (base & ~3), not base + bit/8 --
       * bits >= 8 made l32i unaligned and faulted on silicon with
       * LoadStoreAlignment (QEMU does not enforce alignment, which is
       * why only bench use caught it, 2026-10-27); pos & 31 below
       * already carries the byte-position bits.  Direct accesses get
       * the alignment folded into the pool literal (the non-thru
       * `else` branch below aligns before esp_pool_lit). */
      unsigned bit_w = pos & 31U;
      if (thru) {
        (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A6, 0, -4);
        (void) esp_code_alloc(ESP_OP_AND, ESP_REG_A4, ESP_REG_A4,
            (int) ESP_REG_A6);
      }
      (void) esp_code_alloc(ESP_OP_L32I, rd, ESP_REG_A4, 0);
      if (bit_w) {
        (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A6, 0, (int) bit_w);
        (void) esp_code_alloc(ESP_OP_SSR, 0, ESP_REG_A6, 0);
        (void) esp_code_alloc(ESP_OP_SRL, rd, rd, 0);
      }
      esp_code_extui(reg, rd, 0, 1);
      return;
    }
    (void) esp_code_alloc(ESP_OP_L8UI, rd, ESP_REG_A4, 0);
    if (bit_in) {
      (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A6, 0, (int) bit_in);
      (void) esp_code_alloc(ESP_OP_SSR, 0, ESP_REG_A6, 0);
      (void) esp_code_alloc(ESP_OP_SRL, rd, rd, 0);
    }
    esp_code_extui(reg, rd, 0, 1);
  }
}

/* single-bit store: vreg holds 0/nonzero.  Branchless:
 * byte = (byte & ~mask) | (mask & v01).  a2 holds the incoming value
 * (moved here when the caller used a scratch reg); scratch a3/a5/a6/a7.
 * thru (3c): insert through a pointer slot, buffer-relative index. */
static void esp_store_bit(pfile_t *pf, value_t val, unsigned vreg)
{
  variable_t cv = esp_bit_container(val);
  variable_t sv = value_variable_get(val);
  boolean_t  thru = esp_value_is_ptr_deref(val);
  esp_code_t  br;
  esp_code_t  lset;

  if (!cv) {
    pfile_log(pf, PFILE_LOG_ERR, "ESP32 bit without storage");
    return;
  }
  if (ESP_REG_A2 != vreg) {
    (void) esp_code_alloc(ESP_OP_OR, ESP_REG_A2, (int) vreg, (int) vreg);
    vreg = ESP_REG_A2;
  }
  /* normalize to 0/1 */
  if (value_is_boolean(val)) {
    br = esp_code_alloc(ESP_OP_BEQZ, 0, vreg, 0);
    (void) esp_code_alloc(ESP_OP_MOVI, vreg, 0, 1);
    lset = esp_code_label_alloc();
    if (br) { br->target = lset; }
  } else {
    esp_code_extui(vreg, vreg, 0, 1);
  }
  if (esp_bit_is_runtime(val)) {
    unsigned bit_base = thru ? 0U : value_bit_offset_get(val);

    /* a5 = bit index; a6 = byte offset; a7 = mask */
    esp_load_value(pf, value_baseofs_get(val), ESP_REG_A5);
    if (bit_base) {
      /* ADDI's imm8 is SIGNED: a pool bit offset >= 128 wraps to
       * -128 and the byte address goes invalid (LoadStoreError) --
       * add large bases through a pool literal instead */
      if (bit_base <= 127U) {
        (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A5, ESP_REG_A5,
            (int) bit_base);
      } else {
        esp_emit_l32r(ESP_REG_A6, esp_pool_lit((unsigned long) bit_base));
        (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A5, ESP_REG_A5,
            (int) ESP_REG_A6);
      }
    }
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A6, 0, 3);
    (void) esp_code_alloc(ESP_OP_SSR, 0, ESP_REG_A6, 0);
    (void) esp_code_alloc(ESP_OP_SRL, ESP_REG_A6, ESP_REG_A5, 0);
    esp_code_extui(ESP_REG_A5, ESP_REG_A5, 0, 3);
    if (thru) {
      esp_code_t lit = esp_pool_lit(esp_var_ram(sv));
      (void) esp_emit_l32r(ESP_REG_A4, lit);
      (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A4, ESP_REG_A4, 0);
    } else {
      unsigned long addr = esp_var_ram(cv);
      esp_code_t lit = esp_pool_lit(addr);
      (void) esp_emit_l32r(ESP_REG_A4, lit);
    }
    (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A4, ESP_REG_A4, (int) ESP_REG_A6);
    /* mask = 1 << bit-in (a7); inverted in a6 */
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A7, 0, 1);
    (void) esp_code_alloc(ESP_OP_SSL, 0, ESP_REG_A5, 0);
    (void) esp_code_alloc(ESP_OP_SLL, ESP_REG_A7, ESP_REG_A7, 0);
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A6, 0, -1);
    (void) esp_code_alloc(ESP_OP_XOR, ESP_REG_A6, ESP_REG_A6, (int) ESP_REG_A7);
    (void) esp_code_alloc(ESP_OP_L8UI, ESP_REG_A3, ESP_REG_A4, 0);
    (void) esp_code_alloc(ESP_OP_AND, ESP_REG_A3, ESP_REG_A3, (int) ESP_REG_A6);
    /* gate via sign-extension (mask & 1 == 0 for bits > 0) */
    (void) esp_code_alloc(ESP_OP_NEG, ESP_REG_A2, (int) vreg, 0);
    (void) esp_code_alloc(ESP_OP_AND, ESP_REG_A7, ESP_REG_A7, (int) ESP_REG_A2);
    (void) esp_code_alloc(ESP_OP_OR, ESP_REG_A3, ESP_REG_A3, (int) ESP_REG_A7);
    (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A3, ESP_REG_A4, 0);
    return;
  }
  {
    variable_const_t off = 0;
    unsigned         pos;
    unsigned         bit_in;
    unsigned         mask;

    (void) value_baseofs_const_get(val, &off);
    pos = (thru ? 0U : value_bit_offset_get(val)) + (unsigned) off;
    bit_in = pos & 7U;
    mask = 1U << bit_in;
    if (thru) {
      unsigned long by = (unsigned long) (pos >> 3);
      esp_code_t lit = esp_pool_lit(esp_var_ram(sv));
      (void) esp_emit_l32r(ESP_REG_A4, lit);
      (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A4, ESP_REG_A4, 0);
      if (by) {
        if (by <= 127UL) {
          (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A4, ESP_REG_A4,
              (int) by);
        } else {
          esp_emit_l32r(ESP_REG_A6, esp_pool_lit(by));
          (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A4, ESP_REG_A4,
              (int) ESP_REG_A6);
        }
      }
    } else {
      unsigned long addr = esp_var_ram(cv) + (pos >> 3);
      esp_code_t    lit;

      if (esp_addr_is_peripheral(addr)) {
        addr &= ~3UL;   /* SFR word RMW: aligned register (see below) */
      }
      lit = esp_pool_lit(addr);
      (void) esp_emit_l32r(ESP_REG_A4, lit);
    }
    if (esp_addr_is_peripheral(esp_var_ram(cv) + (pos >> 3))) {
      /* SFR: full 32-bit RMW (byte lanes garble at 80/160 MHz).  The
       * RMW must target the ALIGNED register (base & ~3) -- bits >= 8
       * made the l32i/s32i pair unaligned and faulted on silicon
       * (LoadStoreAlignment; QEMU is not enforcing), 2026-10-27. */
      unsigned bit_w = pos & 31U;
      unsigned long mask_w = 1UL << bit_w;
      esp_code_t m_lit = esp_pool_lit(~mask_w);
      esp_code_t s_lit = esp_pool_lit(mask_w);

      if (thru) {
        (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A6, 0, -4);
        (void) esp_code_alloc(ESP_OP_AND, ESP_REG_A4, ESP_REG_A4,
            (int) ESP_REG_A6);
      }
      (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A3, ESP_REG_A4, 0);
      (void) esp_emit_l32r(ESP_REG_A5, m_lit);
      (void) esp_code_alloc(ESP_OP_AND, ESP_REG_A3, ESP_REG_A3,
        (int) ESP_REG_A5);
      (void) esp_emit_l32r(ESP_REG_A5, s_lit);
      (void) esp_code_alloc(ESP_OP_NEG, ESP_REG_A6, (int) vreg, 0);
      (void) esp_code_alloc(ESP_OP_AND, ESP_REG_A5, ESP_REG_A5,
        (int) ESP_REG_A6);
      (void) esp_code_alloc(ESP_OP_OR, ESP_REG_A3, ESP_REG_A3,
        (int) ESP_REG_A5);
      (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A3, ESP_REG_A4, 0);
      return;
    }
    (void) esp_code_alloc(ESP_OP_L8UI, ESP_REG_A3, ESP_REG_A4, 0);
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A5, 0, (int) (255U - mask));
    (void) esp_code_alloc(ESP_OP_AND, ESP_REG_A3, ESP_REG_A3, (int) ESP_REG_A5);
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A5, 0, (int) mask);
    /* gate: vreg is 0/1; mask & 1 = 0 for any bit > 0.  Sign-extend
     * to 0/~0 first, then AND with the mask. */
    (void) esp_code_alloc(ESP_OP_NEG, ESP_REG_A6, (int) vreg, 0);
    (void) esp_code_alloc(ESP_OP_AND, ESP_REG_A5, ESP_REG_A5, (int) ESP_REG_A6);
    (void) esp_code_alloc(ESP_OP_OR, ESP_REG_A3, ESP_REG_A3, (int) ESP_REG_A5);
    (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A3, ESP_REG_A4, 0);
  }
}

/* multi-bit field (bit*N) load into reg (width 2..32; 1 = bit above).
 * Scratch a4/a6/a7; a reg in the scratch set is staged through a5. */
static void esp_load_bitfield(pfile_t *pf, value_t val, unsigned reg)
{
  variable_t cv = esp_bit_container(val);
  variable_const_t off = 0;
  unsigned width = (unsigned) value_sz_get(val);
  unsigned pos;
  unsigned bit_in;
  unsigned nbytes;
  unsigned k;
  unsigned long addr;
  unsigned rd = reg;

  if ((ESP_REG_A4 == reg) || (ESP_REG_A6 == reg) || (ESP_REG_A7 == reg)) {
    rd = ESP_REG_A5;
  }
  if (!cv) {
    pfile_log(pf, PFILE_LOG_ERR, "ESP32 field without storage");
    return;
  }
  (void) value_baseofs_const_get(val, &off);
  pos = value_bit_offset_get(val) + (unsigned) off;
  bit_in = pos & 7U;
  nbytes = (bit_in + width + 7U) / 8U;
  if (nbytes > 4U) {
    /* runtime-subscripted fields arrive here too */
    pfile_log(pf, PFILE_LOG_ERR,
        "ESP32 field wider than a 4-byte window (w=%u)", width);
    return;
  }
  addr = esp_var_ram(cv) + (pos >> 3);
  {
    esp_code_t lit = esp_pool_lit(addr);
    (void) esp_emit_l32r(ESP_REG_A4, lit);
  }
  (void) esp_code_alloc(ESP_OP_L8UI, rd, ESP_REG_A4, 0);
  for (k = 1; k < nbytes; k++) {
    (void) esp_code_alloc(ESP_OP_L8UI, ESP_REG_A6, ESP_REG_A4, (int) k);
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A7, 0, 8);
    (void) esp_code_alloc(ESP_OP_SSL, 0, ESP_REG_A7, 0);
    (void) esp_code_alloc(ESP_OP_SLL, ESP_REG_A6, ESP_REG_A6, 0);
    (void) esp_code_alloc(ESP_OP_OR, rd, rd, (int) ESP_REG_A6);
  }
  if (bit_in) {
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A6, 0, (int) bit_in);
    (void) esp_code_alloc(ESP_OP_SSR, 0, ESP_REG_A6, 0);
    (void) esp_code_alloc(ESP_OP_SRL, rd, rd, 0);
  }
  if (width <= 16U) {
    esp_code_extui(reg, rd, 0, width);
  } else {
    unsigned long mask = (width >= 32U) ? 0xFFFFFFFFUL
        : ((1UL << width) - 1UL);
    esp_code_t lit = esp_pool_lit(mask);
    (void) esp_emit_l32r(ESP_REG_A6, lit);
    (void) esp_code_alloc(ESP_OP_AND, reg, rd, (int) ESP_REG_A6);
  }
}

/* multi-bit field store from vreg (width 2..32).  a2 holds the incoming
 * value (moved here when the caller used a scratch reg); scratch
 * a3/a4/a6/a7. */
static void esp_store_bitfield(pfile_t *pf, value_t val, unsigned vreg)
{
  variable_t cv = esp_bit_container(val);
  variable_const_t off = 0;
  unsigned width = (unsigned) value_sz_get(val);
  unsigned pos;
  unsigned bit_in;
  unsigned nbytes;
  unsigned k;
  unsigned long addr;

  if (!cv) {
    pfile_log(pf, PFILE_LOG_ERR, "ESP32 field without storage");
    return;
  }
  if (ESP_REG_A2 != vreg) {
    (void) esp_code_alloc(ESP_OP_OR, ESP_REG_A2, (int) vreg, (int) vreg);
    vreg = ESP_REG_A2;
  }
  (void) value_baseofs_const_get(val, &off);
  pos = value_bit_offset_get(val) + (unsigned) off;
  bit_in = pos & 7U;
  nbytes = (bit_in + width + 7U) / 8U;
  if (nbytes > 4U) {
    pfile_log(pf, PFILE_LOG_ERR,
        "ESP32 field wider than a 4-byte window (w=%u)", width);
    return;
  }
  /* normalize the incoming value to the field width */
  if (width <= 16U) {
    esp_code_extui(vreg, vreg, 0, width);
  } else if (width < 32U) {
    unsigned long mask = (1UL << width) - 1UL;
    esp_code_t lit = esp_pool_lit(mask);
    (void) esp_emit_l32r(ESP_REG_A6, lit);
    (void) esp_code_alloc(ESP_OP_AND, vreg, vreg, (int) ESP_REG_A6);
  }
  addr = esp_var_ram(cv) + (pos >> 3);
  {
    esp_code_t lit = esp_pool_lit(addr);
    (void) esp_emit_l32r(ESP_REG_A4, lit);
  }
  /* window (a3) = existing bytes */
  (void) esp_code_alloc(ESP_OP_L8UI, ESP_REG_A3, ESP_REG_A4, 0);
  for (k = 1; k < nbytes; k++) {
    (void) esp_code_alloc(ESP_OP_L8UI, ESP_REG_A6, ESP_REG_A4, (int) k);
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A7, 0, 8);
    (void) esp_code_alloc(ESP_OP_SSL, 0, ESP_REG_A7, 0);
    (void) esp_code_alloc(ESP_OP_SLL, ESP_REG_A6, ESP_REG_A6, 0);
    (void) esp_code_alloc(ESP_OP_OR, ESP_REG_A3, ESP_REG_A3, (int) ESP_REG_A6);
  }
  /* clear the field: window &= ~(((1<<w)-1) << bit_in) */
  {
    unsigned long fmask = (width >= 32U) ? 0xFFFFFFFFUL
        : ((1UL << width) - 1UL);
    esp_code_t lit = esp_pool_lit(fmask);
    (void) esp_emit_l32r(ESP_REG_A6, lit);
    if (bit_in) {
      (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A7, 0, (int) bit_in);
      (void) esp_code_alloc(ESP_OP_SSL, 0, ESP_REG_A7, 0);
      (void) esp_code_alloc(ESP_OP_SLL, ESP_REG_A6, ESP_REG_A6, 0);
    }
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A7, 0, -1);
    (void) esp_code_alloc(ESP_OP_XOR, ESP_REG_A7, ESP_REG_A7, (int) ESP_REG_A6);
    (void) esp_code_alloc(ESP_OP_AND, ESP_REG_A3, ESP_REG_A3, (int) ESP_REG_A7);
    /* insert: window |= (value << bit_in) */
    if (bit_in) {
      (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A7, 0, (int) bit_in);
      (void) esp_code_alloc(ESP_OP_SSL, 0, ESP_REG_A7, 0);
      (void) esp_code_alloc(ESP_OP_SLL, vreg, vreg, 0);
    }
    (void) esp_code_alloc(ESP_OP_OR, ESP_REG_A3, ESP_REG_A3, (int) vreg);
  }
  (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A3, ESP_REG_A4, 0);
  for (k = 1; k < nbytes; k++) {
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A7, 0, 8);
    (void) esp_code_alloc(ESP_OP_SSR, 0, ESP_REG_A7, 0);
    (void) esp_code_alloc(ESP_OP_SRL, ESP_REG_A3, ESP_REG_A3, 0);
    (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A3, ESP_REG_A4, (int) k);
  }
}

/* ---------------- wide integers (3b core) ----------------------- */

static boolean_t esp_is_wide(value_t dst, value_t v1, value_t v2)
{
  value_t vals[3];
  unsigned ii;

  vals[0] = dst; vals[1] = v1; vals[2] = v2;
  for (ii = 0; ii < 3; ii++) {
    if (vals[ii] && !value_is_pointer(vals[ii])
        && (value_byte_sz_get(vals[ii]) > 4U)) {
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

/* Compile-time byte `ofs` of a const operand.  variable_const_t holds 8
 * bytes, so variable_const_byte_get returns 0 past that -- sign fill for
 * wider destinations comes from esp_wide_known_byte instead. */
static unsigned esp_wide_const_byte(value_t val, unsigned ofs)
{
  variable_t    v;
  unsigned char *data;
  unsigned      limit;
  variable_const_t off;

  v = value_variable_get(val);
  if (!v) {
    return variable_const_byte_get(value_const_get(val), ofs);
  }
  data  = (unsigned char *) variable_data_get(v);
  limit = (unsigned) variable_sz_get(v);
  off   = 0;
  (void) value_baseofs_const_get(val, &off);
  if (data && ((unsigned) off + ofs) < limit) {
    return data[(unsigned) off + ofs];
  }
  if (ofs < (unsigned) sizeof(variable_const_t)) {
    return variable_const_byte_get(value_const_get(val), ofs);
  }
  return 0;
}

/* Byte `ofs` of a const source with sign/zero fill past src_sz.
 * Returns 0..255, or -1 when the byte must be loaded at run time. */
static int esp_wide_known_byte(value_t val, unsigned ofs, unsigned src_sz)
{
  unsigned last;
  unsigned fill;

  if (!val || !value_is_const(val) || esp_value_is_ptr_deref(val)) {
    return -1;
  }
  if (ofs < src_sz) {
    return (int) esp_wide_const_byte(val, ofs);
  }
  fill = 0;
  if (value_is_signed(val) && src_sz) {
    last = esp_wide_const_byte(val, src_sz - 1);
    fill = (0x80U & last) ? 0xFFU : 0U;
  }
  return (int) fill;
}

/* Real storage width of a wide operand: a promoted value (sqword
 * widened to soword) still lives in the narrower variable home; reads
 * past its storage must sign/zero fill instead of touching adjacent
 * RAM (mips_wide_val_sz rule).  Unnamed integer literals carry a
 * synthetic 16-byte UNIVERSAL def (value_constant_get) and
 * value_byte_sz_get clamps universals to 4 bytes; their real width is
 * the smallest k whose upper const bytes match byte k-1's sign fill,
 * or wide compares would loop 16 bytes (mismatching the variable's
 * sign fill past its top) or read zero-filled bytes above 4. */
static unsigned esp_wide_const_width(variable_const_t n)
{
  unsigned k;
  unsigned b;
  unsigned fill;

  for (k = 1; k <= (unsigned) sizeof(variable_const_t); k++) {
    fill = (variable_const_byte_get(n, k - 1) & 0x80U) ? 0xFFU : 0x00U;
    for (b = k; b < (unsigned) sizeof(variable_const_t); b++) {
      if (variable_const_byte_get(n, b) != fill) {
        break;
      }
    }
    if (b >= (unsigned) sizeof(variable_const_t)) {
      return k;
    }
  }
  return (unsigned) sizeof(variable_const_t);
}

static unsigned esp_wide_val_sz(value_t val)
{
  unsigned        s;
  variable_t      v;
  variable_def_t  def;

  if (!val) {
    return 0;
  }
  v = value_variable_get(val);
  if (v && value_is_const(val)) {
    def = variable_def_get(v);
    if (def
        && variable_def_flag_test(def, VARIABLE_DEF_FLAG_UNIVERSAL)) {
      if (variable_const_is_negative(value_const_get(val),
          VARIABLE_CONST_BIT_CT)) {
        /* negative literal in an unsigned def: keep the full 16-byte
         * view -- every const byte is real (0xFF), so wide loops never
         * consult the (unsigned) fill */
        s = (unsigned) variable_sz_get(v);
        if (s) {
          return s;
        }
      }
      return esp_wide_const_width(value_const_get(val));
    }
    s = (unsigned) variable_sz_get(v);
    if (s) {
      return s;
    }
  }
  s = (unsigned) value_byte_sz_get(val);
  if (!s) {
    s = (unsigned) value_sz_get(val);
  }
  return s;
}

/* Operand byte: compile-time const bytes as MOVI, else L8UI below the
 * storage width and sign/zero fill above it (mips_wide_load_byte rule).
 * Const operands need no address -- all their bytes are known. */
static void esp_wide_load_byte(value_t val, unsigned ofs, unsigned src_sz,
    unsigned reg, unsigned basereg)
{
  int known;

  known = esp_wide_known_byte(val, ofs, src_sz);
  if (known >= 0) {
    (void) esp_code_alloc(ESP_OP_MOVI, reg, 0, known);
    return;
  }
  if (ofs < src_sz) {
    (void) esp_code_alloc(ESP_OP_L8UI, reg, (int) basereg, (int) ofs);
  } else if (value_is_signed(val) && src_sz) {
    (void) esp_code_alloc(ESP_OP_MOVI, reg, 0, 0xFF);
  } else {
    (void) esp_code_alloc(ESP_OP_MOVI, reg, 0, 0);
  }
}

/* ---------------- wide relations (compare / compare-branch) ------- */

/* branch partner of a dest-less test cmd, or NULL */
static cmd_t esp_wide_branch_partner(const cmd_t cmd)
{
  cmd_t next = cmd_next_exec_get(cmd_link_get(cmd));

  if (!next || (CMD_TYPE_BRANCH != cmd_type_get(next))
      || (CMD_BRANCHTYPE_GOTO != cmd_brtype_get(next))
      || (CMD_BRANCHCOND_NONE == cmd_brcond_get(next))) {
    return (cmd_t) 0;
  }
  if (!cmd_brdst_get(next)) {
    return (cmd_t) 0;
  }
  return next;
}

/* a8 = OR of all byte differences: nonzero iff v1 != v2 */
static void esp_wide_xor_acc(pfile_t *pf, value_t v1, value_t v2)
{
  unsigned s1 = esp_wide_val_sz(v1);
  unsigned s2 = esp_wide_val_sz(v2);
  unsigned n  = (s1 > s2) ? s1 : s2;
  unsigned ofs;

  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A8, 0, 0);
  esp_wide_addr(pf, v1, ESP_REG_A4);
  esp_wide_addr(pf, v2, ESP_REG_A6);
  for (ofs = 0; ofs < n; ofs++) {
    esp_wide_load_byte(v1, ofs, s1, ESP_REG_A2, ESP_REG_A4);
    esp_wide_load_byte(v2, ofs, s2, ESP_REG_A3, ESP_REG_A6);
    (void) esp_code_alloc(ESP_OP_XOR, ESP_REG_A2, ESP_REG_A2,
        (int) ESP_REG_A3);
    (void) esp_code_alloc(ESP_OP_OR, ESP_REG_A8, ESP_REG_A8,
        (int) ESP_REG_A2);
  }
}

/* a8 = (v1 < v2), byte-wise MSB-first scan.  Signed compares flip bit 7
 * of the top bytes: at the first difference equal signs cancel and
 * differing signs decide, exactly like a signed compare of that byte. */
static void esp_wide_ult(pfile_t *pf, value_t v1, value_t v2)
{
  unsigned   s1 = esp_wide_val_sz(v1);
  unsigned   s2 = esp_wide_val_sz(v2);
  unsigned   n  = (s1 > s2) ? s1 : s2;
  boolean_t  sgn = (value_is_signed(v1) || value_is_signed(v2))
      ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  esp_code_t br;
  esp_code_t eq_br;
  esp_code_t diff_brs[16];
  unsigned   diff_br_ct = 0;
  esp_code_t ldiff;
  esp_code_t ldone;
  unsigned   oi;
  int        i;

  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A8, 0, 0);
  esp_wide_addr(pf, v1, ESP_REG_A4);
  esp_wide_addr(pf, v2, ESP_REG_A6);
  for (i = (int) n - 1; i >= 0; i--) {
    esp_wide_load_byte(v1, (unsigned) i, s1, ESP_REG_A2, ESP_REG_A4);
    esp_wide_load_byte(v2, (unsigned) i, s2, ESP_REG_A3, ESP_REG_A6);
    if (sgn && ((unsigned) i == (n - 1)) && n) {
      (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A9, 0, 0x80);
      (void) esp_code_alloc(ESP_OP_XOR, ESP_REG_A2, ESP_REG_A2,
          (int) ESP_REG_A9);
      (void) esp_code_alloc(ESP_OP_XOR, ESP_REG_A3, ESP_REG_A3,
          (int) ESP_REG_A9);
    }
    br = esp_code_alloc(ESP_OP_BNE, ESP_REG_A3, ESP_REG_A2, 0);
    if (br && (diff_br_ct < 16)) {
      diff_brs[diff_br_ct++] = br;
    }
  }
  /* all bytes equal: a8 = 0 (not less); jump around the diff block */
  eq_br = esp_code_alloc(ESP_OP_BEQZ, 0, ESP_REG_A8, 0);
  ldiff = esp_code_label_alloc();       /* first difference lands here */
  for (oi = 0; oi < diff_br_ct; oi++) {
    diff_brs[oi]->target = ldiff;
  }
  /* a2/a3 hold the differing bytes: not less -> done (a8 stays 0) */
  br = esp_code_alloc(ESP_OP_BGEU, ESP_REG_A3, ESP_REG_A2, 0);
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A8, 0, 1);
  ldone = esp_code_label_alloc();       /* join */
  if (br) {
    br->target = ldone;
  }
  if (eq_br) {
    eq_br->target = ldone;
  }
  UNUSED(pf);
}

static const walg_ops_t esp_walg_ops;

/* dest-less wide relation fused with the following goto: the relation
 * chain and branch-sense live in wide_alg.c; this side owns the
 * branch-partner discovery (skip-list aware).  Returns FALSE when
 * there is no branch partner (plain stripped test). */
static boolean_t esp_wide_compare_branch(pfile_t *pf, const cmd_t cmd,
    operator_t op, value_t v1, value_t v2)
{
  cmd_t     next = esp_wide_branch_partner(cmd);
  label_t   lbl;
  boolean_t br_true;

  if (!next || !v1 || !v2) {
    return BOOLEAN_FALSE;
  }
  lbl     = cmd_brdst_get(next);
  br_true = (CMD_BRANCHCOND_TRUE == cmd_brcond_get(next))
      ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  esp_op.skip_cmd = next;
  walg_compare_branch_emit(pf, &esp_walg_ops, op, br_true, lbl, v1, v2);
  return BOOLEAN_TRUE;
}

/* dest-less wide value fused with a goto: condition true iff v == 0
 * (not_form, NOTL/CMPB) or v != 0 (truth test).  The or-accumulate
 * and branch polarity live in wide_alg.c. */
static boolean_t esp_wide_value_branch(pfile_t *pf, const cmd_t cmd,
    value_t v1, boolean_t not_form)
{
  cmd_t     next = esp_wide_branch_partner(cmd);
  label_t   lbl;
  boolean_t br_true;

  if (!next || !v1) {
    return BOOLEAN_FALSE;
  }
  lbl     = cmd_brdst_get(next);
  br_true = (CMD_BRANCHCOND_TRUE == cmd_brcond_get(next))
      ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  esp_op.skip_cmd = next;
  walg_value_branch_emit(pf, &esp_walg_ops, not_form, br_true, lbl, v1);
  return BOOLEAN_TRUE;
}

static void esp_wide_addr(pfile_t *pf, value_t val, unsigned reg)
{
  variable_t       v = value_variable_get(val);
  unsigned long    addr;
  value_t          bofs;
  variable_const_t ofs = 0;

  if (!v) {
    return;
  }
  esp_value_ensure(pf, val);
  addr = esp_var_ram(v);
  /* element/member lvalue: fold a constant subscript into the base
   * (a runtime subscript would need a second scratch reg inside the
   * per-byte wide loops) */
  bofs = value_baseofs_get(val);
  if (bofs && !value_is_const(bofs)) {
    pfile_log(pf, PFILE_LOG_ERR,
        "ESP32: wide operand with a runtime subscript");
    return;
  }
  (void) value_baseofs_const_get(val, &ofs);
  addr += (unsigned long) ofs;
  esp_emit_l32r(reg, esp_pool_lit(addr));
}

/* wide assign: unrolled byte copy; source bytes past the operand's
 * storage sign/zero fill (esp_wide_known_byte) */
static void esp_wide_assign(pfile_t *pf, value_t dst, value_t src)
{
  unsigned sz = (unsigned) value_byte_sz_get(dst);
  unsigned s_src = esp_wide_val_sz(src);
  unsigned k;

  UNUSED(pf);

  if (value_is_const(src)) {
    esp_wide_addr(pf, dst, ESP_REG_A4);
    for (k = 0; k < sz; k++) {
      (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A2, 0,
          (int) esp_wide_known_byte(src, k, s_src));
      (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A2, ESP_REG_A4, (int) k);
    }
    return;
  }
  esp_wide_addr(pf, src, ESP_REG_A4);
  esp_wide_addr(pf, dst, ESP_REG_A5);
  for (k = 0; k < sz; k++) {
    esp_wide_load_byte(src, k, s_src, ESP_REG_A2, ESP_REG_A4);
    (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A2, ESP_REG_A5, (int) k);
  }
}

/* wide ALU: per-byte with carry.  ADD carry = bit 8 of the 32-bit sum
 * (no branches needed).  SUB borrow = (x < y + borrow) via one compare. */
/* esp_wide_alu now lives in the shared kernel: walg_alu over the
 * esp_walg_alu_* primitives below. */

/* wide shift by a constant count: per dest byte, assemble from the
 * two source bytes that straddle the bit boundary (mips model).  A
 * right-arith shift sign-fills past the top source byte (and saturates
 * to the sign byte when the count swallows the whole width). */
/* ---------- shared wide SHIFT primitives (libcore wide_alg) ---------
 * The saturation / window / walk-order policy lives in wide_alg.c;
 * these replay the Xtensa idioms.  Two small improvements over the
 * pre-kernel code, unified on the kernel policy: a const source's
 * arithmetic saturation and out-of-window sign fills now const-fold
 * (MOVI of the known sign byte) instead of loading at run time. */

static unsigned esp_walg_shift_width(pfile_t *pf, value_t dst, value_t src)
{
  UNUSED(pf);
  UNUSED(src);
  return (unsigned) value_byte_sz_get(dst);
}

static boolean_t esp_walg_shift_is_const(value_t src)
{
  return value_is_const(src) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

static unsigned esp_walg_shift_fill_const(value_t src, unsigned src_sz,
    boolean_t arith)
{
  unsigned last;

  if (!arith || !src_sz) {
    return 0;
  }
  if (value_is_const(src)) {
    last = esp_wide_known_byte(src, src_sz - 1U, src_sz);
    return (last & 0x80U) ? 0xFFU : 0U;
  }
  if (!value_is_signed(src)) {
    return 0;
  }
  return 0xFFU;
}

/* saturation loops: dst = 0 / dst = fill / dst = sign byte of src */
static void esp_walg_shift_zero_all(pfile_t *pf, value_t dst, unsigned n,
    value_t src)
{
  unsigned k;

  UNUSED(src);
  esp_wide_addr(pf, dst, ESP_REG_A4);
  for (k = 0; k < n; k++) {
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A2, 0, 0);
    (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A2, (int) ESP_REG_A4,
        (int) k);
  }
}

static void esp_walg_shift_fill_all_const(pfile_t *pf, value_t dst,
    unsigned n, value_t src, unsigned fill)
{
  unsigned k;

  UNUSED(src);
  esp_wide_addr(pf, dst, ESP_REG_A4);
  for (k = 0; k < n; k++) {
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A2, 0, (int) fill);
    (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A2, (int) ESP_REG_A4,
        (int) k);
  }
}

static void esp_walg_shift_fill_all_runtime(pfile_t *pf, value_t dst,
    unsigned n, value_t src, unsigned src_sz)
{
  unsigned k;

  esp_wide_addr(pf, src, ESP_REG_A4);
  esp_wide_load_byte(src, src_sz ? src_sz - 1U : 0U, src_sz,
      ESP_REG_A2, ESP_REG_A4);
  esp_code_extui(ESP_REG_A2, ESP_REG_A2, 7, 1);
  (void) esp_code_alloc(ESP_OP_NEG, ESP_REG_A2, (int) ESP_REG_A2, 0);
  esp_code_extui(ESP_REG_A2, ESP_REG_A2, 0, 8);
  esp_wide_addr(pf, dst, ESP_REG_A5);
  for (k = 0; k < n; k++) {
    (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A2, (int) ESP_REG_A5,
        (int) k);
  }
}

static void esp_walg_shift_const_prologue(pfile_t *pf, value_t dst,
    value_t src)
{
  esp_wide_addr(pf, src, ESP_REG_A4);
  esp_wide_addr(pf, dst, ESP_REG_A5);
}

/* a2 = src[lo_i] with the out-of-window policy applied (a4 = src) */
static void esp_walg_shift_const_lo(value_t src, int lo_i,
    unsigned src_sz, unsigned fill, boolean_t arith, boolean_t left,
    unsigned reg)
{
  if ((lo_i < 0) || ((unsigned) lo_i >= src_sz)) {
    if (arith && !left && (lo_i >= (int) src_sz)) {
      if (value_is_const(src)) {
        (void) esp_code_alloc(ESP_OP_MOVI, reg, 0, (int) fill);
      } else {
        esp_wide_load_byte(src, src_sz - 1U, src_sz, reg,
            ESP_REG_A4);
        esp_code_extui(reg, reg, 7, 1);
        (void) esp_code_alloc(ESP_OP_NEG, reg, (int) reg, 0);
        esp_code_extui(reg, reg, 0, 8);
      }
    } else {
      (void) esp_code_alloc(ESP_OP_MOVI, reg, 0, 0);
    }
  } else {
    (void) esp_code_alloc(ESP_OP_L8UI, reg, (int) ESP_REG_A4, lo_i);
  }
}

/* one dest byte i of a const-count shift (a4/a5 = src/dst pins) */
static void esp_walg_shift_const_byte(pfile_t *pf, value_t dst,
    value_t src, int i, int lo_i, int hi_i, unsigned bit, unsigned fill,
    boolean_t arith, boolean_t left, unsigned src_sz)
{
  UNUSED(pf);
  UNUSED(dst);
  esp_walg_shift_const_lo(src, lo_i, src_sz, fill, arith, left,
      ESP_REG_A2);
  if (bit) {
    esp_walg_shift_const_lo(src, hi_i, src_sz, fill, arith, left,
        ESP_REG_A3);
    /* combine: (lo << bit) | (hi >> (8-bit)) for left,
     *          (lo >> bit) | (hi << (8-bit)) for right */
    if (left) {
      (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A7, 0, (int) bit);
      (void) esp_code_alloc(ESP_OP_SSL, 0, (int) ESP_REG_A7, 0);
      (void) esp_code_alloc(ESP_OP_SLL, ESP_REG_A2, (int) ESP_REG_A2, 0);
      (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A7, 0, (int) (8U - bit));
      (void) esp_code_alloc(ESP_OP_SSR, 0, (int) ESP_REG_A7, 0);
      (void) esp_code_alloc(ESP_OP_SRL, ESP_REG_A3, (int) ESP_REG_A3, 0);
    } else {
      (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A7, 0, (int) bit);
      (void) esp_code_alloc(ESP_OP_SSR, 0, (int) ESP_REG_A7, 0);
      (void) esp_code_alloc(ESP_OP_SRL, ESP_REG_A2, (int) ESP_REG_A2, 0);
      (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A7, 0, (int) (8U - bit));
      (void) esp_code_alloc(ESP_OP_SSL, 0, (int) ESP_REG_A7, 0);
      (void) esp_code_alloc(ESP_OP_SLL, ESP_REG_A3, (int) ESP_REG_A3, 0);
    }
    (void) esp_code_alloc(ESP_OP_OR, ESP_REG_A2, (int) ESP_REG_A2,
        (int) ESP_REG_A3);
  }
  /* mask to 8 bits */
  esp_code_extui(ESP_REG_A2, ESP_REG_A2, 0, 8);
  (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A2, (int) ESP_REG_A5, i);
}

/* ---------------- wide MUL/DIV/MOD + runtime shifts (3b follow-up) -- */

/* underlying master variable of an operand (overlay-safe identity) */
static variable_t esp_wide_master(value_t val)
{
  variable_t v = val ? value_variable_get(val) : (variable_t) 0;

  while (v && variable_master_get(v)) {
    v = variable_master_get(v);
  }
  return v;
}

/* dst and operand share storage (`q = q * b` would zero its own input) */
static boolean_t esp_wide_aliases(value_t a, value_t b)
{
  variable_t va = esp_wide_master(a);
  variable_t vb = esp_wide_master(b);

  return (va && vb && (va == vb)) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* n-byte RAM temporary (linear placement, no home block variable);
 * the reuse slots themselves live in the shared wide_alg kernel */
static value_t esp_wide_tmp(pfile_t *pf, unsigned n)
{
  variable_def_t def;
  variable_t     v;
  value_t        val;

  def = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
      VARIABLE_DEF_FLAG_NONE, (variable_sz_t) n);
  if (!def) {
    return VALUE_NONE;
  }
  v = variable_alloc(TAG_NONE, def);
  if (!v) {
    return VALUE_NONE;
  }
  esp_var_place(pf, v, (variable_sz_t) n);
  val = value_alloc(v);
  variable_release(v);
  return val;
}

/* dst = 0 for n bytes */
static void esp_wide_zero(pfile_t *pf, value_t val, unsigned n)
{
  unsigned ofs;

  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A2, 0, 0);
  esp_wide_addr(pf, val, ESP_REG_A4);
  for (ofs = 0; ofs < n; ofs++) {
    (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A2, ESP_REG_A4, (int) ofs);
  }
}

/* a8 = OR of all bytes (nonzero iff val != 0) */
static void esp_wide_or_acc(pfile_t *pf, value_t val, unsigned n)
{
  unsigned ofs;

  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A8, 0, 0);
  esp_wide_addr(pf, val, ESP_REG_A4);
  for (ofs = 0; ofs < n; ofs++) {
    esp_wide_load_byte(val, ofs, n, ESP_REG_A2, ESP_REG_A4);
    (void) esp_code_alloc(ESP_OP_OR, ESP_REG_A8, ESP_REG_A8,
        (int) ESP_REG_A2);
  }
}

/* val = -val in place (NOT + 1 carry chain).  The NOT must be masked
 * to the byte BEFORE the carry extract: xor -1 sets bits 8-31, and
 * extui(sum, 8, 1) would read a poisoned carry (mips XORI 0xFF rule). */
static void esp_wide_neg_inplace(pfile_t *pf, value_t val, unsigned n)
{
  unsigned ofs;

  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A9, 0, 1);  /* carry in */
  esp_wide_addr(pf, val, ESP_REG_A4);
  for (ofs = 0; ofs < n; ofs++) {
    esp_wide_load_byte(val, ofs, n, ESP_REG_A2, ESP_REG_A4);
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A3, 0, -1);
    (void) esp_code_alloc(ESP_OP_XOR, ESP_REG_A2, ESP_REG_A2,
        (int) ESP_REG_A3);
    esp_code_extui(ESP_REG_A2, ESP_REG_A2, 0, 8);
    (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A2, ESP_REG_A2,
        (int) ESP_REG_A9);
    esp_code_extui(ESP_REG_A9, ESP_REG_A2, 8, 1);
    esp_code_extui(ESP_REG_A2, ESP_REG_A2, 0, 8);
    (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A2, ESP_REG_A4, (int) ofs);
  }
}

/* ---------- shared wide ALU/NEG/INCR-DECR primitives (wide_alg) ----
 * The loop structure lives in wide_alg.c; these implement the Xtensa
 * idioms it drives.  Emission order matches the pre-kernel code
 * exactly (pure code motion: suite bodies stay HEX byte-identical). */

static unsigned esp_walg_alu_width(pfile_t *pf, value_t dst, value_t v1)
{
  UNUSED(pf);
  UNUSED(v1);
  return (unsigned) value_byte_sz_get(dst ? dst : v1);
}

/* pin operand/dst base addresses (a4/a6/a5) and clear the carry for
 * the add/sub chains; a const op2 needs no base pin */
static void esp_walg_alu_prologue(pfile_t *pf, operator_t op,
    value_t dst, value_t v1, value_t v2)
{
  if (value_is_const(v2)) {
    esp_wide_addr(pf, v1, ESP_REG_A4);
  } else {
    esp_wide_addr(pf, v1, ESP_REG_A4);
    esp_wide_addr(pf, v2, ESP_REG_A6);
  }
  esp_wide_addr(pf, dst, ESP_REG_A5);
  if ((OPERATOR_ADD == op) || (OPERATOR_SUB == op)) {
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A8, 0, 0);
  }
}

static void esp_walg_alu_load_op1(pfile_t *pf, value_t v1, unsigned k,
    unsigned s1)
{
  UNUSED(pf);
  esp_wide_load_byte(v1, k, s1, ESP_REG_A2, ESP_REG_A4);
}

/* const op2 bytes materialize as MOVI (esp_wide_load_byte known-byte
 * path); the a6 pin is dead for them */
static void esp_walg_alu_load_op2(pfile_t *pf, value_t v2, unsigned k,
    unsigned s2)
{
  UNUSED(pf);
  esp_wide_load_byte(v2, k, s2, ESP_REG_A3, ESP_REG_A6);
}

/* carry = bit 8 of (x + y + cin) -- branchless */
static void esp_walg_alu_step_add(void)
{
  (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A2, (int) ESP_REG_A2,
      (int) ESP_REG_A3);
  (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A2, (int) ESP_REG_A2,
      (int) ESP_REG_A8);
  esp_code_extui(ESP_REG_A8, ESP_REG_A2, 8, 1);
}

/* borrow = (x <u y + borrow); result = x - (y + borrow).
 * alloc(op, X, Y) branches when (Y op X): at=X, as=Y -- the skip is
 * taken when x >=u (y + borrow), i.e. no borrow */
static void esp_walg_alu_step_sub(void)
{
  (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A9, (int) ESP_REG_A3,
      (int) ESP_REG_A8);
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A8, 0, 0);
  {
    esp_code_t br = esp_code_alloc(ESP_OP_BGEU, ESP_REG_A9,
        (int) ESP_REG_A2, 0);
    esp_code_t lskip;

    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A8, 0, 1);
    lskip = esp_code_label_alloc();
    if (br) {
      br->target = lskip;
    }
  }
  (void) esp_code_alloc(ESP_OP_SUB, ESP_REG_A2, (int) ESP_REG_A2,
      (int) ESP_REG_A9);
}

static void esp_walg_alu_step_logic(operator_t op)
{
  esp_opcode_t alu;

  switch (op) {
    case OPERATOR_ANDB: alu = ESP_OP_AND; break;
    case OPERATOR_ORB:  alu = ESP_OP_OR;  break;
    default:            alu = ESP_OP_XOR; break;
  }
  (void) esp_code_alloc(alu, ESP_REG_A2, (int) ESP_REG_A2,
      (int) ESP_REG_A3);
}

static void esp_walg_alu_store(pfile_t *pf, value_t dst, unsigned k,
    value_t v1)
{
  UNUSED(pf);
  UNUSED(dst);
  UNUSED(v1);
  (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A2, (int) ESP_REG_A5,
      (int) k);
}

/* dst = -v1: NOT + 1 carry chain.  The NOT must be masked to the byte
 * BEFORE the carry extract: xor -1 sets bits 8-31, and extui(sum, 8, 1)
 * would read a poisoned carry (mips XORI 0xFF rule). */
static void esp_walg_neg_prologue(pfile_t *pf, value_t dst, value_t v1)
{
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A8, 0, 1);  /* carry in */
  esp_wide_addr(pf, v1, ESP_REG_A4);
  esp_wide_addr(pf, dst, ESP_REG_A5);
}

static void esp_walg_neg_step(pfile_t *pf, value_t v1, unsigned k,
    unsigned s1)
{
  UNUSED(pf);
  esp_wide_load_byte(v1, k, s1, ESP_REG_A2, ESP_REG_A4);
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A3, 0, -1);
  (void) esp_code_alloc(ESP_OP_XOR, ESP_REG_A2, (int) ESP_REG_A2,
      (int) ESP_REG_A3);
  esp_code_extui(ESP_REG_A2, ESP_REG_A2, 0, 8);
  (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A2, (int) ESP_REG_A2,
      (int) ESP_REG_A8);
  esp_code_extui(ESP_REG_A8, ESP_REG_A2, 8, 1);
  esp_code_extui(ESP_REG_A2, ESP_REG_A2, 0, 8);
}

/* dst = v1 +/- 1: increment carry / decrement borrow chain */
static void esp_walg_incr_prologue(pfile_t *pf, value_t dst, value_t v1)
{
  esp_wide_addr(pf, v1, ESP_REG_A4);
  esp_wide_addr(pf, dst ? dst : v1, ESP_REG_A5);
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A8, 0, 1);
}

static void esp_walg_incr_decr_step(pfile_t *pf, operator_t op,
    value_t v1, unsigned k, unsigned s1)
{
  UNUSED(pf);
  esp_wide_load_byte(v1, k, s1, ESP_REG_A2, ESP_REG_A4);
  if (OPERATOR_INCR == op) {
    (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A2, (int) ESP_REG_A2,
        (int) ESP_REG_A8);
    esp_code_extui(ESP_REG_A8, ESP_REG_A2, 8, 1);
  } else {
    /* borrow = (x <u borrow): result = x - borrow (alu SUB shape) */
    (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A9, (int) ESP_REG_A8, 0);
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A8, 0, 0);
    {
      esp_code_t br = esp_code_alloc(ESP_OP_BGEU, ESP_REG_A9,
          (int) ESP_REG_A2, 0);
      esp_code_t lskip;

      (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A8, 0, 1);
      lskip = esp_code_label_alloc();
      if (br) {
        br->target = lskip;
      }
    }
    (void) esp_code_alloc(ESP_OP_SUB, ESP_REG_A2, (int) ESP_REG_A2,
        (int) ESP_REG_A9);
  }
  esp_code_extui(ESP_REG_A2, ESP_REG_A2, 0, 8);
}

/* ---------- shared wide MUL/DIV primitives (libcore wide_alg) -----
 * The loop structure lives in wide_alg.c; these implement the Xtensa
 * idioms it drives.  Emission order matches the pre-kernel code
 * exactly (pure code motion: suite bodies stay HEX byte-identical). */

static unsigned esp_walg_mul_width(pfile_t *pf, value_t dst, value_t v1)
{
  UNUSED(pf);
  UNUSED(v1);
  return esp_wide_val_sz(dst);
}

static unsigned esp_walg_div_base_width(pfile_t *pf, value_t dst,
    value_t v1)
{
  UNUSED(pf);
  UNUSED(v1);
  return esp_wide_val_sz(dst);
}

static void esp_walg_zero(pfile_t *pf, value_t val, unsigned n,
    value_t plain)
{
  UNUSED(n);
  UNUSED(plain);
  esp_wide_zero(pf, val, (unsigned) value_byte_sz_get(val));
}

static const walg_ops_t esp_walg_ops;

static void esp_walg_sub_wide(pfile_t *pf, value_t dst, value_t v1,
    value_t v2)
{
  walg_alu(pf, &esp_walg_ops, OPERATOR_SUB, dst, v1, v2);
}

static void esp_walg_shift_left_1(pfile_t *pf, value_t val)
{
  walg_shift_const(pf, &esp_walg_ops, OPERATOR_SHIFT_LEFT, val, val, 1);
}

/* cell[0] = sign bit of val's top byte (0/1) */
static void esp_walg_sign_to_cell(pfile_t *pf, value_t cell, value_t val,
    unsigned n)
{
  esp_wide_addr(pf, val, ESP_REG_A4);
  esp_wide_load_byte(val, n - 1U, n, ESP_REG_A2, ESP_REG_A4);
  esp_code_extui(ESP_REG_A2, ESP_REG_A2, 7, 1);
  esp_wide_addr(pf, cell, ESP_REG_A5);
  (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A2, (int) ESP_REG_A5, 0);
}

/* load cell[0] into a2 and branch past the guarded body when it is 0 */
static walg_br_t esp_walg_cell_bez(pfile_t *pf, value_t cell)
{
  esp_wide_addr(pf, cell, ESP_REG_A4);
  (void) esp_code_alloc(ESP_OP_L8UI, ESP_REG_A2, (int) ESP_REG_A4, 0);
  return esp_code_alloc(ESP_OP_BEQZ, 0, (int) ESP_REG_A2, 0);
}

/* a2 = c1[0] ^ c2[0]; branch when the signs are equal */
static walg_br_t esp_walg_cell_pair_xor_bez(pfile_t *pf, value_t c1,
    value_t c2)
{
  esp_wide_addr(pf, c1, ESP_REG_A4);
  (void) esp_code_alloc(ESP_OP_L8UI, ESP_REG_A2, (int) ESP_REG_A4, 0);
  esp_wide_addr(pf, c2, ESP_REG_A5);
  (void) esp_code_alloc(ESP_OP_L8UI, ESP_REG_A3, (int) ESP_REG_A5, 0);
  (void) esp_code_alloc(ESP_OP_XOR, ESP_REG_A2, (int) ESP_REG_A2,
      (int) ESP_REG_A3);
  return esp_code_alloc(ESP_OP_BEQZ, 0, (int) ESP_REG_A2, 0);
}

/* division loop state: num/rem/quot addresses pinned in a11-a13 */
static void esp_walg_loop_prologue(pfile_t *pf, value_t num, value_t rem,
    value_t quot)
{
  esp_wide_addr(pf, num, ESP_REG_A11);
  esp_wide_addr(pf, rem, ESP_REG_A12);
  esp_wide_addr(pf, quot, ESP_REG_A13);
}

/* a14 = bit a10 of num (a11 holds num's address) */
static void esp_walg_bit_load(pfile_t *pf, value_t num)
{
  UNUSED(pf);
  UNUSED(num);
  esp_code_extui(ESP_REG_A14, ESP_REG_A10, 3, 5);
  esp_code_extui(ESP_REG_A15, ESP_REG_A10, 0, 3);
  (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A14, (int) ESP_REG_A11,
      (int) ESP_REG_A14);
  (void) esp_code_alloc(ESP_OP_L8UI, ESP_REG_A14, (int) ESP_REG_A14, 0);
  (void) esp_code_alloc(ESP_OP_SSR, 0, (int) ESP_REG_A15, 0);
  (void) esp_code_alloc(ESP_OP_SRL, ESP_REG_A14, (int) ESP_REG_A14, 0);
  esp_code_extui(ESP_REG_A14, ESP_REG_A14, 0, 1);
}

/* rem[0] |= a14 (a12 holds rem's address) */
static void esp_walg_bit_or_low(pfile_t *pf, value_t rem)
{
  UNUSED(pf);
  UNUSED(rem);
  (void) esp_code_alloc(ESP_OP_L8UI, ESP_REG_A15, (int) ESP_REG_A12, 0);
  (void) esp_code_alloc(ESP_OP_OR, ESP_REG_A15, (int) ESP_REG_A15,
      (int) ESP_REG_A14);
  (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A15, (int) ESP_REG_A12, 0);
}

/* set bit a10 of quot to 1 (a13 holds quot's address) */
static void esp_walg_bit_set(pfile_t *pf, value_t quot)
{
  UNUSED(pf);
  UNUSED(quot);
  esp_code_extui(ESP_REG_A14, ESP_REG_A10, 3, 5);
  (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A14, (int) ESP_REG_A13,
      (int) ESP_REG_A14);
  (void) esp_code_alloc(ESP_OP_L8UI, ESP_REG_A2, (int) ESP_REG_A14, 0);
  esp_code_extui(ESP_REG_A15, ESP_REG_A10, 0, 3);
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A3, 0, 1);
  (void) esp_code_alloc(ESP_OP_SSL, 0, (int) ESP_REG_A15, 0);
  (void) esp_code_alloc(ESP_OP_SLL, ESP_REG_A3, (int) ESP_REG_A3, 0);
  (void) esp_code_alloc(ESP_OP_OR, ESP_REG_A2, (int) ESP_REG_A2,
      (int) ESP_REG_A3);
  (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A2, (int) ESP_REG_A14, 0);
}

/* zero dst, then pin t1/dst/t2 base addresses in a4/a5/a6 */
static void esp_walg_mul_prologue(pfile_t *pf, value_t t1, value_t t2,
    value_t dst, unsigned n)
{
  UNUSED(n);
  esp_wide_zero(pf, dst, (unsigned) value_byte_sz_get(dst));
  esp_wide_addr(pf, t1, ESP_REG_A4);
  esp_wide_addr(pf, t2, ESP_REG_A6);
  esp_wide_addr(pf, dst, ESP_REG_A5);
}

static void esp_walg_mul_row_init(void)
{
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A9, 0, 0);  /* carry */
}

/* dst[i+j] = t1[i]*t2[j] + dst[i+j] + carry (carry in a9) */
static void esp_walg_mul_row_step(pfile_t *pf, value_t t1, value_t t2,
    value_t dst, unsigned i, unsigned j, unsigned n)
{
  unsigned s1 = esp_wide_val_sz(t1);
  unsigned s2 = esp_wide_val_sz(t2);

  UNUSED(pf);
  UNUSED(dst);
  UNUSED(n);
  esp_wide_load_byte(t1, i, s1, ESP_REG_A2, ESP_REG_A4);
  esp_wide_load_byte(t2, j, s2, ESP_REG_A3, ESP_REG_A6);
  (void) esp_code_alloc(ESP_OP_MULL, ESP_REG_A2, (int) ESP_REG_A2,
      (int) ESP_REG_A3);
  (void) esp_code_alloc(ESP_OP_L8UI, ESP_REG_A3, (int) ESP_REG_A5,
      (int) (i + j));
  (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A2, (int) ESP_REG_A2,
      (int) ESP_REG_A3);
  (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A2, (int) ESP_REG_A2,
      (int) ESP_REG_A9);
  esp_code_extui(ESP_REG_A9, ESP_REG_A2, 8, 8);
  esp_code_extui(ESP_REG_A2, ESP_REG_A2, 0, 8);
  (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A2, (int) ESP_REG_A5,
      (int) (i + j));
}

static void esp_walg_movi(unsigned reg, int imm)
{
  (void) esp_code_alloc(ESP_OP_MOVI, reg, 0, imm);
}

static void esp_walg_addi(unsigned reg, int imm)
{
  (void) esp_code_alloc(ESP_OP_ADDI, reg, (int) reg, imm);
}

static walg_br_t esp_walg_br_flag_zero(void)
{
  return esp_code_alloc(ESP_OP_BEQZ, 0, (int) ESP_REG_A8, 0);
}

static walg_br_t esp_walg_br_flag_nonzero(void)
{
  return esp_code_alloc(ESP_OP_BNEZ, 0, (int) ESP_REG_A8, 0);
}

static walg_br_t esp_walg_br_ctr_nonzero(void)
{
  return esp_code_alloc(ESP_OP_BNEZ, 0, (int) ESP_REG_A10, 0);
}

static walg_lab_t esp_walg_label_here(void)
{
  return esp_code_label_alloc();
}

static void esp_walg_br_target(walg_br_t br, walg_lab_t lab)
{
  if (br) {
    ((esp_code_t) br)->target = (esp_code_t) lab;
  }
}

static void esp_walg_shift_var_load_count(pfile_t *pf, value_t cntv);
static void esp_walg_shift_var_prologue(pfile_t *pf, value_t dst,
    value_t src);
static void esp_walg_shift_var_byte(pfile_t *pf, operator_t op,
    value_t dst, int i, value_t src, unsigned src_sz);
static void esp_walg_or_self(pfile_t *pf, value_t v1);
static void esp_walg_br_flag_to(pfile_t *pf, boolean_t take_on_nonzero,
    label_t lbl);
static void esp_walg_bool_eq_finish(pfile_t *pf, value_t dst,
    operator_t op);
static void esp_walg_bool_lt_finish(pfile_t *pf, value_t dst,
    boolean_t inverted);

static const walg_ops_t esp_walg_ops = {
  ESP_REG_A8,                            /* reg_flag  */
  ESP_REG_A10,                           /* reg_ctr   */
  esp_wide_val_sz,
  esp_walg_mul_width,
  esp_walg_div_base_width,
  esp_wide_aliases,
  esp_wide_tmp,
  esp_wide_assign,
  esp_walg_zero,
  esp_wide_or_acc,
  esp_wide_neg_inplace,
  esp_wide_ult,
  esp_walg_sub_wide,
  esp_walg_shift_left_1,
  esp_walg_sign_to_cell,
  esp_walg_cell_bez,
  esp_walg_cell_pair_xor_bez,
  esp_walg_loop_prologue,
  esp_walg_bit_load,
  esp_walg_bit_or_low,
  esp_walg_bit_set,
  esp_walg_mul_prologue,
  esp_walg_mul_row_init,
  esp_walg_mul_row_step,
  esp_walg_movi,
  esp_walg_addi,
  esp_walg_br_flag_zero,
  esp_walg_br_flag_nonzero,
  esp_walg_br_ctr_nonzero,
  esp_walg_label_here,
  esp_walg_br_target,
  esp_walg_alu_width,
  esp_walg_alu_prologue,
  esp_walg_alu_load_op1,
  esp_walg_alu_load_op2,
  esp_walg_alu_step_add,
  esp_walg_alu_step_sub,
  esp_walg_alu_step_logic,
  esp_walg_alu_store,
  esp_walg_neg_prologue,
  esp_walg_neg_step,
  esp_walg_incr_prologue,
  esp_walg_incr_decr_step,
  esp_walg_shift_width,
  esp_walg_shift_is_const,
  esp_walg_shift_fill_const,
  esp_walg_shift_zero_all,
  esp_walg_shift_fill_all_const,
  esp_walg_shift_fill_all_runtime,
  esp_walg_shift_const_prologue,
  esp_walg_shift_const_byte,
  esp_walg_shift_var_load_count,
  esp_walg_shift_var_prologue,
  esp_walg_shift_var_byte,
  esp_wide_xor_acc,
  esp_wide_ult,
  esp_walg_or_self,
  esp_walg_br_flag_to,
  esp_walg_bool_eq_finish,
  esp_walg_bool_lt_finish
};

/* Runtime shift count: per dest byte i, extract 8 bits of src starting
 * at bit (i*8 - count) (left) / (i*8 + count) (right); byteidx = floor
 * of that / 8, bit = remainder.  Out-of-range lo/hi bytes contribute 0
 * (right-arith: the sign byte).  Combine is (lo >> bit) | (hi << (8-bit))
 * for BOTH directions -- dest bit b comes from source bit bitbase+b in
 * either case.  Loop order (left: descending) lives in wide_alg.c. */

static void esp_walg_shift_var_load_count(pfile_t *pf, value_t cntv)
{
  unsigned s_cnt = esp_wide_val_sz(cntv);

  if (s_cnt > 4) {
    esp_wide_addr(pf, cntv, ESP_REG_A4);
    esp_wide_load_byte(cntv, 0, s_cnt, ESP_REG_A8, ESP_REG_A4);
  } else {
    esp_load_value(pf, cntv, ESP_REG_A8);
  }
}

static void esp_walg_shift_var_prologue(pfile_t *pf, value_t dst,
    value_t src)
{
  esp_wide_addr(pf, src, ESP_REG_A6);
  esp_wide_addr(pf, dst, ESP_REG_A5);
}

/* one dest byte i (count in a8; a6/a5 = src/dst pins) */
static void esp_walg_shift_var_byte(pfile_t *pf, operator_t op,
    value_t dst, int i, value_t src, unsigned src_sz)
{
  boolean_t left  = (OPERATOR_SHIFT_LEFT == op) ? BOOLEAN_TRUE
      : BOOLEAN_FALSE;
  boolean_t arith = (OPERATOR_SHIFT_RIGHT_ARITHMETIC == op)
      ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  esp_code_t br_lo;
  esp_code_t br_hi;
  esp_code_t lbl;

  UNUSED(pf);
  UNUSED(dst);
  UNUSED(src);
  /* a9 = bitbase */
  if (left) {
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A9, 0, (int) (i * 8));
    (void) esp_code_alloc(ESP_OP_SUB, ESP_REG_A9, (int) ESP_REG_A9,
        (int) ESP_REG_A8);
  } else {
    (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A9, (int) ESP_REG_A8,
        (int) (i * 8));
  }
  esp_code_extui(ESP_REG_A7, ESP_REG_A9, 0, 3);   /* bit in byte */
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A3, 0, 3);
  (void) esp_code_alloc(ESP_OP_SSR, 0, (int) ESP_REG_A3, 0);
  /* byte index = bitbase >> 3 ARITHMETIC: a left shift's bitbase can
   * be negative (byte window before byte 0), and the logical shift
   * turned -1 into 0x1FFFFFFF -- the hi-byte guard then skipped the
   * in-range src[0] load and the byte yielded 0 (runtime left shifts
   * by counts crossing the byte boundary read 0).  Arithmetic keeps
   * byteidx -1, so the lo guard still skips (unsigned huge) while
   * the hi guard loads src[0]; non-negative bases are unaffected. */
  (void) esp_code_alloc(ESP_OP_SRA, ESP_REG_A9, (int) ESP_REG_A9, 0);
  /* a2 = src[byteidx] (0 / sign byte when out of range) */
  if (arith && !left) {
    esp_wide_load_byte(src, src_sz - 1U, src_sz, ESP_REG_A2,
        ESP_REG_A6);
    esp_code_extui(ESP_REG_A2, ESP_REG_A2, 7, 1);
    (void) esp_code_alloc(ESP_OP_NEG, ESP_REG_A2, (int) ESP_REG_A2, 0);
    esp_code_extui(ESP_REG_A2, ESP_REG_A2, 0, 8);
  } else {
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A2, 0, 0);
  }
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A4, 0, (int) src_sz);
  /* skip the load when byteidx >=u src_sz */
  br_lo = esp_code_alloc(ESP_OP_BGEU, (int) ESP_REG_A4,
      (int) ESP_REG_A9, 0);
  (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A3, (int) ESP_REG_A6,
      (int) ESP_REG_A9);
  (void) esp_code_alloc(ESP_OP_L8UI, ESP_REG_A2, (int) ESP_REG_A3, 0);
  lbl = esp_code_label_alloc();
  if (br_lo) {
    br_lo->target = lbl;
  }
  /* a3 = src[byteidx + 1] */
  if (arith && !left) {
    esp_wide_load_byte(src, src_sz - 1U, src_sz, ESP_REG_A3,
        ESP_REG_A6);
    esp_code_extui(ESP_REG_A3, ESP_REG_A3, 7, 1);
    (void) esp_code_alloc(ESP_OP_NEG, ESP_REG_A3, (int) ESP_REG_A3, 0);
    esp_code_extui(ESP_REG_A3, ESP_REG_A3, 0, 8);
  } else {
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A3, 0, 0);
  }
  (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A9, (int) ESP_REG_A9, 1);
  br_hi = esp_code_alloc(ESP_OP_BGEU, (int) ESP_REG_A4,
      (int) ESP_REG_A9, 0);
  (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A9, (int) ESP_REG_A6,
      (int) ESP_REG_A9);
  (void) esp_code_alloc(ESP_OP_L8UI, ESP_REG_A3, (int) ESP_REG_A9, 0);
  lbl = esp_code_label_alloc();
  if (br_hi) {
    br_hi->target = lbl;
  }
  /* dst[i] = (lo >> bit) | (hi << (8 - bit)) */
  (void) esp_code_alloc(ESP_OP_SSR, 0, (int) ESP_REG_A7, 0);
  (void) esp_code_alloc(ESP_OP_SRL, ESP_REG_A2, (int) ESP_REG_A2, 0);
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A9, 0, 8);
  (void) esp_code_alloc(ESP_OP_SUB, ESP_REG_A9, (int) ESP_REG_A9,
      (int) ESP_REG_A7);
  (void) esp_code_alloc(ESP_OP_SSL, 0, (int) ESP_REG_A9, 0);
  (void) esp_code_alloc(ESP_OP_SLL, ESP_REG_A3, (int) ESP_REG_A3, 0);
  (void) esp_code_alloc(ESP_OP_OR, ESP_REG_A2, (int) ESP_REG_A2,
      (int) ESP_REG_A3);
  esp_code_extui(ESP_REG_A2, ESP_REG_A2, 0, 8);
  (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A2, (int) ESP_REG_A5,
      (int) i);
}

/* wide comparison materialized into dst (boolean 0/1 in a9) */
/* ---------- shared wide RELATION primitives (libcore wide_alg) -------
 * The relation mapping (chain choice, operand order, inversion) and
 * branch-sense logic live in wide_alg.c; these replay the Xtensa
 * idioms exactly. */

/* a8 = (v1 != 0): the single-value or-accumulate */
static void esp_walg_or_self(pfile_t *pf, value_t v1)
{
  unsigned s = esp_wide_val_sz(v1);
  unsigned ofs;

  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A8, 0, 0);
  esp_wide_addr(pf, v1, ESP_REG_A4);
  for (ofs = 0; ofs < s; ofs++) {
    esp_wide_load_byte(v1, ofs, s, ESP_REG_A2, ESP_REG_A4);
    (void) esp_code_alloc(ESP_OP_OR, ESP_REG_A8, ESP_REG_A8,
        (int) ESP_REG_A2);
  }
}

static void esp_walg_br_flag_to(pfile_t *pf, boolean_t take_on_nonzero,
    label_t lbl)
{
  esp_code_t br;

  br = esp_code_alloc(take_on_nonzero ? ESP_OP_BNEZ : ESP_OP_BEQZ, 0,
      ESP_REG_A8, 0);
  esp_branch_target(pf, br, lbl);
}

/* EQ/NE materialization: the equality accumulator already ran (a8
 * nonzero iff the values differ) */
static void esp_walg_bool_eq_finish(pfile_t *pf, value_t dst,
    operator_t op)
{
  esp_code_t br;
  esp_code_t ldone;

  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A9, 0,
      (OPERATOR_NE == op) ? 0 : 1);    /* preset "relation false" */
  br = esp_code_alloc(
      (OPERATOR_NE == op) ? ESP_OP_BEQZ : ESP_OP_BNEZ,
      0, ESP_REG_A8, 0);
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A9, 0,
      (OPERATOR_NE == op) ? 1 : 0);    /* relation true */
  ldone = esp_code_label_alloc();
  if (br) {
    br->target = ldone;
  }
  esp_store_value(pf, dst, ESP_REG_A9);
}

/* ordered-relation materialization: the lt chain already ran (a8) */
static void esp_walg_bool_lt_finish(pfile_t *pf, value_t dst,
    boolean_t inverted)
{
  esp_code_t br;
  esp_code_t ldone;

  if (!inverted) {
    (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A9, (int) ESP_REG_A8, 0);
  } else {                             /* GE/LE: invert */
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A9, 0, 0);
    br = esp_code_alloc(ESP_OP_BNEZ, 0, ESP_REG_A8, 0);
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A9, 0, 1);
    ldone = esp_code_label_alloc();
    if (br) {
      br->target = ldone;
    }
  }
  esp_store_value(pf, dst, ESP_REG_A9);
}

static void esp_wide_compare_bool(pfile_t *pf, value_t dst,
    operator_t op, value_t v1, value_t v2)
{
  if (!dst || !v1 || !v2) {
    return;
  }
  walg_compare_bool(pf, &esp_walg_ops, dst, op, v1, v2);
}

/* ---------------- procedures (static-frame calls) ---------------- */

/* call a proc: marshal IN args into the callee's param vars, call0
 * through l32r+a0+callx0, marshal OUT args (incl. the function return,
 * param index 0) back to the caller's destinations */
/* ---------------- open bit formals (3c) ---------------- */

static unsigned esp_bitarg_ct;   /* `_esp_bitarg_N` name counter */

/* container variable of a packed bit variable (master walk) */
static variable_t esp_var_bit_container(variable_t v)
{
  while (v) {
    variable_t m = variable_master_get(v);

    if (!m) {
      break;
    }
    v = m;
  }
  return v;
}

/* extract absolute bit `pos` of the packed container at `ram` into reg
 * (0/1).  a4 = address, a6 = shift scratch. */
static void esp_bit_extract_abs(unsigned long ram, unsigned pos,
    unsigned reg)
{
  unsigned   bit_in = pos & 7U;

  esp_emit_l32r(ESP_REG_A4, esp_pool_lit(ram + (pos >> 3)));
  (void) esp_code_alloc(ESP_OP_L8UI, reg, ESP_REG_A4, 0);
  if (bit_in) {
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A6, 0, (int) bit_in);
    (void) esp_code_alloc(ESP_OP_SSR, 0, ESP_REG_A6, 0);
    (void) esp_code_alloc(ESP_OP_SRL, reg, reg, 0);
  }
  esp_code_extui(reg, reg, 0, 1);
}

/* insert reg (0/1) into absolute bit `pos` of the packed container at
 * `ram` (branchless RMW, same shape as esp_store_bit's const path). */
static void esp_bit_insert_abs(unsigned long ram, unsigned pos,
    unsigned reg)
{
  unsigned   bit_in = pos & 7U;
  unsigned   mask = 1U << bit_in;

  esp_emit_l32r(ESP_REG_A4, esp_pool_lit(ram + (pos >> 3)));
  (void) esp_code_alloc(ESP_OP_L8UI, ESP_REG_A3, ESP_REG_A4, 0);
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A5, 0, (int) (255U - mask));
  (void) esp_code_alloc(ESP_OP_AND, ESP_REG_A3, ESP_REG_A3,
      (int) ESP_REG_A5);
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A5, 0, (int) mask);
  (void) esp_code_alloc(ESP_OP_NEG, ESP_REG_A6, (int) reg, 0);
  (void) esp_code_alloc(ESP_OP_AND, ESP_REG_A5, ESP_REG_A5,
      (int) ESP_REG_A6);
  (void) esp_code_alloc(ESP_OP_OR, ESP_REG_A3, ESP_REG_A3,
      (int) ESP_REG_A5);
  (void) esp_code_alloc(ESP_OP_S8I, ESP_REG_A3, ESP_REG_A4, 0);
}

/* copy `ct` packed bits: src bit (src_bit + i) -> dst bit (dst_bit + i) */
static void esp_copy_packed_bits(unsigned long src_ram, unsigned src_bit,
    unsigned long dst_ram, unsigned dst_bit, unsigned ct)
{
  unsigned i;

  for (i = 0; i < ct; i++) {
    esp_bit_extract_abs(src_ram, src_bit + i, ESP_REG_A2);
    esp_bit_insert_abs(dst_ram, dst_bit + i, ESP_REG_A2);
  }
}

/* POINTER formal whose pointee is a bit (open `bit in bits[]`) */
static boolean_t esp_formal_is_pointer_to_bit(value_t formal)
{
  variable_def_t        def;
  variable_def_member_t mbr;
  variable_def_t        pointee;

  if (!formal || !value_is_pointer(formal)) {
    return BOOLEAN_FALSE;
  }
  def = value_def_get(formal);
  mbr = def ? variable_def_member_get(def) : 0;
  pointee = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
  return (pointee
      && variable_def_flag_test(pointee, VARIABLE_DEF_FLAG_BIT))
      ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* Dedicated `_esp_bitarg_N` INTEGER buffer: packed actual re-based at
 * bit 0; the caller then binds the formal pointer to the buffer's
 * address.  Buffers created during translation are placed immediately
 * (esp_var_place keeps assigning after the alloc pass). */
static value_t esp_bit_array_actual_normalize(pfile_t *pf, value_t actual)
{
  variable_sz_t bit_ct;
  variable_sz_t byte_ct;
  value_t       tmp;
  char          name[32];

  if (!actual || !value_is_array(actual) || !value_is_bit(actual)) {
    return VALUE_NONE;
  }
  bit_ct  = value_sz_get(actual);
  byte_ct = (bit_ct + 7) / 8;
  if (byte_ct < 1) {
    byte_ct = 1;
  }
  sprintf(name, "_esp_bitarg_%u", esp_bitarg_ct);
  {
    variable_t var;

    var = pfile_proc_variable_find(pfile_proc_root_get(pf), name, 0);
    if (!var) {
      variable_def_t def;

      def = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
          VARIABLE_DEF_FLAG_VOLATILE, byte_ct);
      var = VARIABLE_NONE;
      if (def
          && (RESULT_OK == pfile_variable_alloc(pf,
              PFILE_VARIABLE_ALLOC_GLOBAL, name, def, VARIABLE_NONE,
              &var))) {
        variable_flag_set(var, VARIABLE_FLAG_AUTO);
        variable_flag_set(var, VARIABLE_FLAG_STICKY);
        esp_bitarg_ct++;
      } else {
        variable_def_free(def);
        pfile_log_syserr(pf, RESULT_MEMORY);
        var = VARIABLE_NONE;
      }
    }
    if (var && (VARIABLE_BASE_UNKNOWN == variable_base_get(var, 0))) {
      esp_var_place(pf, var, byte_ct);
    }
    tmp = var ? value_alloc(var) : VALUE_NONE;
    variable_release(var);
  }
  if (tmp) {
    variable_t av = esp_var_bit_container(value_variable_get(actual));

    value_assign_ct_bump(tmp, CTR_BUMP_INCR);
    value_use_ct_bump(tmp, CTR_BUMP_INCR);
    esp_copy_packed_bits(esp_var_ram(av),
        (unsigned) value_bit_offset_get(actual),
        esp_var_ram(value_variable_get(tmp)), 0, (unsigned) bit_ct);
  }
  return tmp;
}

/* load a procedure/function label's final IRAM address into reg via
 * the literal pool (forward labels pend like call/goto targets) */
void esp_emit_label_addr(pfile_t *pf, unsigned reg, label_t lbl)
{
  esp_code_t node = lbl ? (esp_code_t) label_code_get(lbl) : 0;

  if (node) {
    esp_emit_l32r(reg, esp_pool_label(node));
    return;
  }
  {
    esp_code_t lit = esp_pool_lit_new();

    esp_goto_pend_add(pf, lbl, lit);
    if (lit) {
      esp_emit_l32r(reg, lit);
    }
  }
}

/* ---- indirect calls (volatile formals are call-by-thunk; mirrors
 * the PIC32 back end's JALR path) ---- */

/* Call through a slot: `call &_a_get` is FUNCTION + VALUE_FLAG_INDIRECT
 * (or a pointer-to-function), never a direct proc label. */
static boolean_t esp_call_is_indirect(value_t proc_val)
{
  pfile_proc_t *proc;
  label_t       lbl;

  if (!proc_val) {
    return BOOLEAN_FALSE;
  }
  proc = value_proc_get(proc_val);
  lbl  = proc ? pfile_proc_label_get(proc) : LABEL_NONE;
  if (lbl) {
    return BOOLEAN_FALSE;
  }
  if (value_is_pointer(proc_val)) {
    return BOOLEAN_TRUE;
  }
  return (value_is_function(proc_val)
      && value_flag_test(proc_val, VALUE_FLAG_INDIRECT))
    ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* the FUNCTION def behind a slot (derefs the pointer wrapper) */
/* Integer/bit return of an indirect call: first member of the
 * function def is the return. */
static boolean_t esp_fn_ptr_has_return(value_t proc_val)
{
  variable_def_t        def;
  variable_def_member_t mbr;
  variable_def_t        rdef;

  if (!proc_val) {
    return BOOLEAN_FALSE;
  }
  def = value_def_get(proc_val);
  if (def && (VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(def))) {
    mbr = variable_def_member_get(def);
    def = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
  }
  if (!def || (VARIABLE_DEF_TYPE_FUNCTION != variable_def_type_get(def))) {
    return BOOLEAN_FALSE;
  }
  mbr  = variable_def_member_get(def);
  rdef = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
  return (rdef && (variable_def_sz_get(rdef) > 0)
      && (variable_def_byte_sz_get(rdef) <= 4)
      && (VARIABLE_DEF_TYPE_FLOAT != variable_def_type_get(rdef)))
    ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* ---------- shared indirect-call protocol (libcore indirect_alg) -----
 * The inbox sizing, caller marshalling and callee PROC_ENTER/LEAVE
 * copies live once in indirect_alg.c; this table supplies the Xtensa
 * byte movers. */

static unsigned esp_ialg_def_bytes(pfile_t *pf, variable_def_t def)
{
  return (unsigned) esp_def_storage_sz(pf, def);
}

static void esp_ialg_scalar_copy(pfile_t *pf, value_t dst, value_t src)
{
  esp_load_value(pf, src, ESP_REG_A2);
  esp_store_value(pf, dst, ESP_REG_A2);
}

const ialg_ops_t esp_ialg_ops = {
  esp_ialg_def_bytes,
  esp_is_wide,
  esp_wide_assign,
  esp_ialg_scalar_copy
};

static void esp_emit_indirect_marshal_in(pfile_t *pf, value_t proc_val,
    value_t *params, size_t ct)
{
  ialg_marshal_in(pf, &esp_ialg_ops, proc_val, params, ct);
}

static void esp_emit_indirect_marshal_out(pfile_t *pf, value_t proc_val,
    value_t *params, size_t ct)
{
  ialg_marshal_out(pf, &esp_ialg_ops, proc_val, params, ct);
}

/* INDIRECT callee entry: copy the inbox (where every caller of an
 * INDIRECT-flagged proc marshals, by member-def order) into the
 * proc's own linear param homes.  The homes must not live in the
 * inbox itself -- a nested indirect call from inside the proc would
 * clobber them (every device write marshals through the inbox). */
static void esp_emit_indirect_param_in(pfile_t *pf, pfile_proc_t *proc)
{
  ialg_param_in(pf, &esp_ialg_ops, proc);
}

/* INDIRECT callee exit: copy OUT params from their linear homes back
 * into the inbox so the caller's marshal-out sees them */
static void esp_emit_indirect_param_out(pfile_t *pf, pfile_proc_t *proc)
{
  ialg_param_out(pf, &esp_ialg_ops, proc);
}

/* load a 4-byte function-pointer slot into reg: pointer defs load via
 * the normal path; FUNCTION+INDIRECT slots read the variable's RAM */
static void esp_load_fn_slot(pfile_t *pf, value_t proc_val, unsigned reg)
{
  variable_t v;

  if (value_is_pointer(proc_val)) {
    esp_load_value(pf, proc_val, reg);
    return;
  }
  v = value_variable_get(proc_val);
  if (!v) {
    pfile_log(pf, PFILE_LOG_ERR, "ESP32: indirect call has no slot");
    return;
  }
  esp_emit_direct_addr(pf, esp_var_ram(v), proc_val);
  (void) esp_code_alloc(ESP_OP_L32I, reg, ESP_REG_A4, 0);
}

static void esp_emit_call_proc(pfile_t *pf, const cmd_t cmd)
{
  value_t       proc_val = cmd_brproc_get(cmd);
  pfile_proc_t *proc     = value_proc_get(proc_val);
  value_t      *params   = cmd_brproc_params_get(cmd);
  size_t        ct       = cmd_brproc_param_ct_get(cmd);
  size_t        pct;
  label_t       lbl;
  size_t        ii;
  value_t      *bitbufs;

  pct = proc ? pfile_proc_param_ct_get(proc) : 0;
  if (pct > ct) {
    ct = pct;
  }

  lbl = proc ? pfile_proc_label_get(proc) : LABEL_NONE;
  /* The inbox protocol: pointer-slot calls (no label) AND every call
   * to an INDIRECT-flagged proc -- its PROC_ENTER copies the inbox
   * into the proc's linear param homes, so all callers must marshal
   * there.  Known-label targets load the label into a0; pointer slots
   * load the 4-byte slot and callx0 through it.  The scalar return
   * rides a2 (`call &_a_get` is FUNCTION + INDIRECT or pointer-typed). */
  if (esp_call_is_indirect(proc_val)
      || (proc && pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INDIRECT))) {
    esp_emit_indirect_marshal_in(pf, proc_val, params, ct);
    if (proc && lbl) {
      esp_emit_label_addr(pf, ESP_REG_A0, lbl);
    } else {
      esp_load_fn_slot(pf, proc_val, ESP_REG_A0);
    }
    (void) esp_code_alloc(ESP_OP_CALLX0, 0, ESP_REG_A0, 0);
    if (params && params[0] && esp_fn_ptr_has_return(proc_val)
        && !esp_is_wide(params[0], VALUE_NONE, VALUE_NONE)) {
      /* the scalar return rides a2: store it BEFORE the OUT copies --
       * their scratch use of a2 would clobber it (an out formal
       * together with a return read the out formal's value as the
       * return) */
      esp_store_value(pf, params[0], ESP_REG_A2);
    }
    esp_emit_indirect_marshal_out(pf, proc_val, params, ct);
    return;
  }
  if (!lbl) {
    pfile_log(pf, PFILE_LOG_ERR, "ESP32 call without a procedure");
    return;
  }
  bitbufs = ct ? CALLOC(ct, sizeof(*bitbufs)) : 0;
  /* pass 1: copy each IN arg into the callee's param var */
  for (ii = 0; ii < ct; ii++) {
    value_t pval = pfile_proc_param_get(proc, ii);
    value_t arg  = params ? params[ii] : VALUE_NONE;
    variable_t pv;

    if (!pval || !arg) { continue; }
    if (!value_dflag_test(pval, VARIABLE_DEF_FLAG_IN)) { continue; }
    pv = value_variable_get(pval);
    /* Unused `_name_count` (open array) is assigned by the caller but
     * never used in the callee, so it has no RAM -- do not store. */
    if (!pv || (VARIABLE_BASE_UNKNOWN == variable_base_get(pv, 0))) {
      if (!esp_formal_is_pointer_to_bit(pval)) {
        continue;
      }
    }
    if (esp_formal_is_pointer_to_bit(pval)
        && value_is_array(arg) && value_is_bit(arg)
        && !value_is_pointer(arg)) {
      /* packed bit-array actual: copy bits into `_esp_bitarg_N`
       * (re-based at bit 0) and bind the formal to its address */
      value_t buf = esp_bit_array_actual_normalize(pf, arg);

      if (buf) {
        esp_load_address_of(pf, buf, ESP_REG_A2);
        esp_store_value(pf, pval, ESP_REG_A2);
        if (bitbufs) {
          bitbufs[ii] = buf;
        } else {
          value_release(buf);
        }
        continue;
      }
    }
    /* Volatile IN getters: a function actual into a pointer-to-function
     * formal passes the thunk's address, not a value copy. */
    if (value_is_pointer(pval) && !value_is_pointer(arg)
        && value_is_function(arg)) {
      esp_load_address_of(pf, arg, ESP_REG_A2);
      esp_store_value(pf, pval, ESP_REG_A2);
      continue;
    }
    /* Open-array / data-pointer formals (`byte in data[]`): pass an
     * address, never a value copied through an unset slot (mips rule) */
    if (value_is_pointer(pval) && !value_is_pointer(arg)
        && (value_is_array(arg) || value_is_value(arg)
          || value_is_label(arg))) {
      esp_load_address_of(pf, arg, ESP_REG_A2);
      esp_store_value(pf, pval, ESP_REG_A2);
      continue;
    }
    if (esp_is_wide(pval, arg, VALUE_NONE)) {
      esp_wide_assign(pf, pval, arg);
    } else {
      esp_load_value(pf, arg, ESP_REG_A2);
      esp_store_value(pf, pval, ESP_REG_A2);
    }
  }
  /* the call itself: l32r a0, <proc label> + callx0 a0 */
  esp_emit_label_addr(pf, ESP_REG_A0, lbl);
  (void) esp_code_alloc(ESP_OP_CALLX0, 0, ESP_REG_A0, 0);
  /* pass 2: copy each OUT var (incl. the function return) back */
  for (ii = 0; ii < ct; ii++) {
    value_t pval = pfile_proc_param_get(proc, ii);
    value_t arg  = params ? params[ii] : VALUE_NONE;

    if (!pval || !arg) { continue; }
    if (!value_dflag_test(pval, VARIABLE_DEF_FLAG_OUT)) { continue; }
    if (bitbufs && bitbufs[ii]) {
      /* IN OUT bit formal: unpack the buffer back onto the actual */
      if (!value_is_const(arg)
          && !variable_is_const(value_variable_get(arg))) {
        variable_t av = esp_var_bit_container(value_variable_get(arg));

        esp_copy_packed_bits(
            esp_var_ram(value_variable_get(bitbufs[ii])), 0,
            esp_var_ram(av),
            (unsigned) value_bit_offset_get(arg),
            (unsigned) value_sz_get(arg));
      }
      continue;
    }
    if (esp_is_wide(arg, pval, VALUE_NONE)) {
      esp_wide_assign(pf, arg, pval);
    } else {
      esp_load_value(pf, pval, ESP_REG_A2);
      esp_store_value(pf, arg, ESP_REG_A2);
    }
  }
  if (bitbufs) {
    for (ii = 0; ii < ct; ii++) {
      if (bitbufs[ii]) {
        value_release(bitbufs[ii]);
      }
    }
    FREE(bitbufs);
  }
}

/* ---------------- pointers ---------------- */

/* load the 32-bit address of a variable into reg (WHEREIS unwrap:
 * jal_expr wraps the target lvalue in a VALUE) */
static void esp_load_address_of(pfile_t *pf, value_t val, unsigned reg)
{
  variable_t v;

  if (value_is_value(val)) {
    value_t inner = variable_value_get(value_variable_get(val));

    if (inner) {
      esp_load_address_of(pf, inner, reg);
      return;
    }
  }
  /* address of a function = its entry label (volatile IN thunks,
   * `alias p is proc` style handlers) */
  if (value_is_function(val)) {
    pfile_proc_t *pr   = value_proc_get(val);
    label_t       flbl = pr ? pfile_proc_label_get(pr) : LABEL_NONE;

    esp_emit_label_addr(pf, reg, flbl);
    return;
  }
  if (value_is_label(val)) {
    esp_emit_label_addr(pf, reg, value_label_get(val));
    return;
  }
  v = value_variable_get(val);

  if (v && variable_is_const(v) && variable_data_get(v)) {
    /* const pool variable: address via the late-bound data literal
     * (its base is still UNKNOWN at translate time -- the data node
     * is placed after all code) */
    esp_code_t       lit = esp_const_data_lit(v);
    variable_const_t ofs = 0;

    (void) value_baseofs_const_get(val, &ofs);
    if (value_baseofs_get(val) && !value_is_const(value_baseofs_get(val))) {
      unsigned scratch = (reg == ESP_REG_A5) ? ESP_REG_A6 : ESP_REG_A5;

      esp_load_value(pf, value_baseofs_get(val), scratch);
      if (lit) {
        esp_emit_l32r(reg, lit);
      }
      (void) esp_code_alloc(ESP_OP_ADD, reg, reg, (int) scratch);
      return;
    }
    if (lit) {
      esp_emit_l32r(reg, lit);
    }
    if (ofs) {
      if (ofs <= 127) {
        (void) esp_code_alloc(ESP_OP_ADDI, reg, reg, (int) ofs);
      } else {
        unsigned scratch = (reg == ESP_REG_A5) ? ESP_REG_A6 : ESP_REG_A5;

        esp_emit_l32r(scratch, esp_pool_lit((unsigned long) ofs));
        (void) esp_code_alloc(ESP_OP_ADD, reg, reg, (int) scratch);
      }
    }
    return;
  }

  if (v && (VARIABLE_BASE_UNKNOWN != variable_base_get(v, 0))) {
    unsigned long addr;
    value_t       bofs;

    addr = esp_var_ram(v);
    bofs = value_baseofs_get(val);
    if (bofs && !value_is_const(bofs)) {
      /* element address: base + runtime byte offset.  The index load
       * uses a4/a5 internally, so keep it in a6 when reg is a5. */
      unsigned scratch = (reg == ESP_REG_A5) ? ESP_REG_A6 : ESP_REG_A5;

      esp_load_value(pf, bofs, scratch);
      esp_emit_l32r(reg, esp_pool_lit(addr));
      (void) esp_code_alloc(ESP_OP_ADD, reg, reg, (int) scratch);
      return;
    }
    {
      variable_const_t ofs = 0;
      unsigned long    a = addr;

      (void) value_baseofs_const_get(val, &ofs);
      a += (unsigned long) ofs;
      esp_emit_l32r(reg, esp_pool_lit(a));
    }
    return;
  }
  pfile_log(pf, PFILE_LOG_ERR,
      "ESP32: address-of needs a placed variable (labels have no DRAM placement)");
}

/* byte size of a pointer value's POINTEE (the dst def carries it);
 * 0 when the def is missing or the member has no size */
static unsigned esp_pointee_byte_sz(const value_t ptr)
{
  variable_def_t        def;
  variable_def_member_t mbr;
  variable_def_t        mdef;

  def = value_def_get(ptr);
  if (!def || (VARIABLE_DEF_TYPE_POINTER != variable_def_type_get(def))) {
    return 0;
  }
  mbr = variable_def_member_get(def);
  mdef = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
  return mdef ? (unsigned) variable_def_byte_sz_get(mdef) : 0;
}

/* JAL pointer read = deref: mips_value_is_ptr_deref mirror */
static boolean_t esp_value_is_ptr_deref(const value_t val)
{
  if (!val || value_flag_test(val, VALUE_FLAG_PTR_SLOT_RAW)) {
    return BOOLEAN_FALSE;
  }
  if (value_flag_test(val, VALUE_FLAG_POINTER)) {
    return BOOLEAN_TRUE;
  }
  return (value_is_pointer(val) && value_is_indirect(val))
    ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* single-bit access THROUGH a pointer (`bits[i]` on an open bit
 * formal): the deref value carries the pointee's FLAG_BIT def (or the
 * pointer def with FLAG_BIT leaked from the pointee) */
static boolean_t esp_value_is_bit_deref(const value_t val)
{
  variable_def_t        def;
  variable_def_member_t mbr;
  variable_def_t        pointee;

  if (!esp_value_is_ptr_deref(val)) {
    return BOOLEAN_FALSE;
  }
  def = value_def_get(val);
  if (def && variable_def_flag_test(def, VARIABLE_DEF_FLAG_BIT)
      && (VARIABLE_DEF_TYPE_POINTER != variable_def_type_get(def))) {
    return BOOLEAN_TRUE;
  }
  mbr = def ? variable_def_member_get(def) : 0;
  pointee = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
  return (pointee
      && variable_def_flag_test(pointee, VARIABLE_DEF_FLAG_BIT))
      ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* a4 = pointer target + baseofs: the slot is read, then the pointee
 * offset (already in bytes) is added.  Runtime index first (its load
 * uses a4); constant offsets up to 127 use ADDI, larger ones a pool
 * literal + ADD. */
/* TRUE for a subscripted element of a pointer array (slots[i]): the
 * address is &slots + i*ptrsz directly, with no slot deref. A bare
 * `p = x` store-through is also pointer-typed but carries no
 * subscript (baseofs), and p[i] on `as var byte` carries the pointee
 * def -- both keep the classic deref. */
static boolean_t esp_elem_is_pointer_array_slot(const value_t ptr)
{
  return value_is_pointer(ptr)
    && (VALUE_NONE != value_baseofs_get(ptr));
}

/* TRUE when the value's base VARIABLE is an array of pointers
 * (`var pointer slots[N] as ...`; mirror of the front end's
 * jal_variable_is_array_of_pointers).  For such a value the subscript
 * selects a SLOT (the front end pre-scales the index by the pointer
 * size), so the baseofs is never an offset into a pointee. */
static boolean_t esp_base_is_pointer_array(const value_t ptr)
{
  variable_t            v;
  variable_def_t        def;
  variable_def_member_t mbr;
  variable_def_t        mdef;

  v = value_variable_get(ptr);
  if (!v || !variable_is_array(v)) {
    return BOOLEAN_FALSE;
  }
  def = variable_def_get(v);
  mbr = def ? variable_def_member_get(def) : 0;
  mdef = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
  return (mdef
      && (VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(mdef)))
    ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

static void esp_emit_deref_addr(pfile_t *pf, value_t ptr)
{
  variable_t    v;
  unsigned long addr;
  value_t       bofs;

  v = value_variable_get(ptr);
  addr = esp_var_ram(v);
  bofs = value_baseofs_get(ptr);
  /* THROUGH-access of a pointer-array element (`r = slots[i]`,
   * `slots[i] = v` with the pointee def on the value): the baseofs
   * selects a slot -- it is never an offset into a pointee.  Compose
   * the slot address (&slots + i*ptrsz), then load the slot's content
   * as the target address.  The pointer-def element form (binds
   * `slots[i] is x`, WHEREIS) keeps the plain slot address below. */
  if (esp_base_is_pointer_array(ptr)
      && !esp_elem_is_pointer_array_slot(ptr)) {
    if (bofs && !value_is_const(bofs)) {
      esp_load_value(pf, bofs, ESP_REG_A5);
      esp_emit_l32r(ESP_REG_A4, esp_pool_lit(addr));
      (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A4, ESP_REG_A4,
          (int) ESP_REG_A5);
    } else {
      variable_const_t ofs = 0;

      (void) value_baseofs_const_get(ptr, &ofs);
      esp_emit_l32r(ESP_REG_A4, esp_pool_lit(addr + (unsigned long) ofs));
    }
    (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A4, ESP_REG_A4, 0);
    return;
  }
  /* an element of a pointer array (`var pointer slots[N] as ...`)
   * is direct storage at &slots + i*ptrsz: the front end scales the
   * index by the pointer size for pointer slots (both constant and
   * runtime subscripts), so no striding here -- just skip the
   * slot-load deref for such elements. Pointee-typed values (p[i] on
   * `as var byte`, bare `p = x` store-through) keep the deref. */
  if (bofs && !value_is_const(bofs)) {
    esp_load_value(pf, bofs, ESP_REG_A5);
    esp_emit_l32r(ESP_REG_A4, esp_pool_lit(addr));
    if (!esp_elem_is_pointer_array_slot(ptr)) {
      (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A4, ESP_REG_A4, 0);
    }
    (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A4, ESP_REG_A4,
        (int) ESP_REG_A5);
    return;
  }
  esp_emit_l32r(ESP_REG_A4, esp_pool_lit(addr));
  if (!esp_elem_is_pointer_array_slot(ptr)) {
    (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A4, ESP_REG_A4, 0);
  }
  {
    variable_const_t ofs = 0;

    (void) value_baseofs_const_get(ptr, &ofs);
    if (ofs) {
      /* constant indices arrive already byte-scaled by the front end
       * (jal_expr.c const subscript path multiplies by the pointer
       * size for pointer slots); only runtime indices need pstride */
      if (ofs <= 127) {
        (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A4, ESP_REG_A4,
            (int) ofs);
      } else {
        esp_emit_l32r(ESP_REG_A5, esp_pool_lit((unsigned long) ofs));
        (void) esp_code_alloc(ESP_OP_ADD, ESP_REG_A4, ESP_REG_A4,
            (int) ESP_REG_A5);
      }
    }
  }
}

/* store reg through a pointer value (indirect store) */
static void esp_store_deref(pfile_t *pf, value_t ptr, unsigned reg,
    unsigned sz)
{
  if (!value_variable_get(ptr)) {
    pfile_log(pf, PFILE_LOG_ERR,
        "ESP32: indirect store target is not a pointer variable");
    return;
  }
  esp_emit_deref_addr(pf, ptr);
  if (esp_elem_is_pointer_array_slot(ptr)) {
    /* `slots[i] = v`: the store-through lvalue keeps the POINTER def,
     * so deref_addr stopped at the slot address -- load the slot's
     * content (the pointee address) before storing through it.  Binds
     * (`slots[i] is x`) never come through here. */
    (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A4, ESP_REG_A4, 0);
  }
  (void) esp_code_alloc(esp_store_op(sz), reg, ESP_REG_A4, 0);
}

/* load through a pointer value (indirect load) into reg */
static void esp_load_deref(pfile_t *pf, value_t ptr, unsigned reg,
    unsigned sz, boolean_t sgn)
{
  if (!value_variable_get(ptr)) {
    pfile_log(pf, PFILE_LOG_ERR,
        "ESP32: indirect load source is not a pointer variable");
    return;
  }
  esp_emit_deref_addr(pf, ptr);
  (void) esp_code_alloc(esp_load_op(sz, sgn), reg, ESP_REG_A4, 0);
}

/* ISR epilogue: undo the enter-pushed save area (a0, a2-a7, SAR) and
 * return from the level-1 interrupt.  Emitted at ISR_CLEANUP (the
 * normal interrupt-proc tail) and kept at PROC_LEAVE for safety.
 * RFE restores pc from EPC1 and clears PS.EXCM; PS.INTLEVEL is not
 * raised for level-1, so no PS restore is needed (our program runs
 * with PS = 0 throughout). */
static void esp_isr_leave_emit(void)
{
  (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A3, ESP_REG_A1, 12);
  (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A4, ESP_REG_A1, 16);
  (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A5, ESP_REG_A1, 20);
  (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A6, ESP_REG_A1, 24);
  (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A7, ESP_REG_A1, 28);
  (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A2, ESP_REG_A1, 8);
  (void) esp_code_alloc(ESP_OP_WSR, ESP_REG_A2, 0, 3); /* SAR */
  (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A2, ESP_REG_A1, 4);
  (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A0, ESP_REG_A1, 0);
  (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A1, ESP_REG_A1, 32);
  (void) esp_code_alloc(ESP_OP_RFE, 0, 0, 0);
}

/* ---------------- main translation ---------------- */

void esp_op_generate(pfile_t *pf, const cmd_t cmd)
{
  cmd_type_t type;

  if (cmd == esp_op.skip_cmd) {
    esp_op.skip_cmd = 0;
    return;
  }

  type = cmd_type_get(cmd);

  if (CMD_TYPE_LABEL == type) {
    label_t     lbl  = cmd_label_get(cmd);
    esp_code_t node = esp_code_label_alloc();

    if (lbl) {
      label_code_set(lbl, node);
    }
    return;
  }

  if (CMD_TYPE_BRANCH == type) {
    if (CMD_BRANCHTYPE_CALL == cmd_brtype_get(cmd)) {
      esp_emit_call_proc(pf, cmd);
    } else if (CMD_BRANCHTYPE_TASK_START == cmd_brtype_get(cmd)) {
      esp_emit_task_start(pf, cmd);
    } else if (CMD_BRANCHTYPE_TASK_YIELD == cmd_brtype_get(cmd)) {
      esp_emit_task_yield(pf);
    } else if (CMD_BRANCHTYPE_TASK_SUSPEND == cmd_brtype_get(cmd)) {
      esp_emit_task_suspend(pf, cmd);
    } else if (CMD_BRANCHTYPE_TASK_RESUME == cmd_brtype_get(cmd)) {
      esp_emit_task_resume(pf, cmd);
    } else if (CMD_BRANCHTYPE_TASK_KILL == cmd_brtype_get(cmd)) {
      esp_emit_task_kill(pf, cmd);
    } else if (CMD_BRANCHTYPE_TASK_RESTART == cmd_brtype_get(cmd)) {
      esp_emit_task_restart(pf, cmd);
    } else {
      esp_emit_branch(pf, cmd);
    }
    return;
  }

  if (CMD_TYPE_END == type) {
    esp_code_t loop = esp_code_label_alloc();
    esp_code_t j = esp_code_alloc(ESP_OP_J, 0, 0, 0);

    if (j) {
      j->target = loop;
    }
    return;
  }

  if ((CMD_TYPE_BLOCK_START == type) || (CMD_TYPE_BLOCK_END == type)
      || (CMD_TYPE_STATEMENT_END == type) || (CMD_TYPE_COMMENT == type)
      || (CMD_TYPE_LOG == type) || (CMD_TYPE_NOP == type)) {
    return;
  }

  if (CMD_TYPE_ASM == type) {
    pfile_log(pf, PFILE_LOG_ERR,
        "inline assembly is not supported on ESP32");
    return;
  }

  if (CMD_TYPE_PROC_ENTER == type) {
    pfile_proc_t *proc = cmd_proc_get(cmd);
    label_t       lbl  = pfile_proc_label_get(proc);
    esp_code_t    node = esp_code_label_alloc();

    if (lbl) {
      label_code_set(lbl, node);
    }
    if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_TASK)) {
      /* tasks are jx-entered leaf frames (mips model): no a0 push,
       * and the leave path suicides instead of returning */
      return;
    }
    if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INTERRUPT)) {
      /* ISR entry (3g): call0 = flat a0-a15, and a2-a7 are
       * call-clobbered -- an interrupt is an involuntary call, so the
       * interrupted code may hold live values in any of them.  Push
       * a0, a2-a7 + SAR on the interrupted task's stack (leaf v1
       * assumed for the stack budget; nested interrupts are masked by
       * PS.EXCM while the handler runs).  The matching restore + rfe
       * is emitted at ISR_CLEANUP (interrupt procs end there, not at
       * PROC_LEAVE -- same cmd shape as the PIC32 back-end). */
      (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A1, ESP_REG_A1, -32);
      (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A0, ESP_REG_A1, 0);
      (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A2, ESP_REG_A1, 4);
      (void) esp_code_alloc(ESP_OP_RSR, ESP_REG_A2, 0, 3); /* SAR */
      (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A2, ESP_REG_A1, 8);
      (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A3, ESP_REG_A1, 12);
      (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A4, ESP_REG_A1, 16);
      (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A5, ESP_REG_A1, 20);
      (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A6, ESP_REG_A1, 24);
      (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A7, ESP_REG_A1, 28);
      return;
    }
    (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A1, ESP_REG_A1, -4);
    (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A0, ESP_REG_A1, 0);
    /* INDIRECT procs: the inbox holds the marshalled args -- pull them
     * into the linear homes before the body runs (the body's own
     * indirect calls reuse the inbox) */
    if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INDIRECT)) {
      esp_emit_indirect_param_in(pf, proc);
    }
    return;
  }
  if (CMD_TYPE_PROC_LEAVE == type) {
    pfile_proc_t *proc = cmd_proc_get(cmd);
    label_t       lbl  = pfile_proc_exit_label_get(proc);
    esp_code_t    node = esp_code_label_alloc();

    if (lbl) {
      label_code_set(lbl, node);
    }
    if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_TASK)) {
      esp_emit_task_suicide(pf);
      return;
    }
    if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INTERRUPT)) {
      /* kept for safety: interrupt procs normally end at ISR_CLEANUP */
      esp_isr_leave_emit();
      return;
    }
    /* Indirect-called procs (volatile thunks) hand their scalar return
     * to the caller in a2: an indirect call site cannot see the
     * callee's param home (mirrors the PIC32 back end's V0 rule).
     * OUT params go back through the inbox first. */
    if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INDIRECT)) {
      variable_def_t rdef = pfile_proc_return_def_get(proc);

      esp_emit_indirect_param_out(pf, proc);

      if (rdef
          && (variable_def_sz_get(rdef) > 0)
          && (variable_def_byte_sz_get(rdef) <= 4)
          && (VARIABLE_DEF_TYPE_FLOAT != variable_def_type_get(rdef))) {
        value_t retval = pfile_proc_param_get(proc, 0);

        if (retval) {
          esp_load_value(pf, retval, ESP_REG_A2);
        }
      }
    }
    (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A0, ESP_REG_A1, 0);
    (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A1, ESP_REG_A1, 4);
    (void) esp_code_alloc(ESP_OP_JX, 0, ESP_REG_A0, 0);
    return;
  }
  if (CMD_TYPE_ISR_CLEANUP == type) {
    /* interrupt proc tail: restore the enter-pushed registers and
     * return from the level-1 interrupt.  RFE restores pc from EPC1
     * and clears PS.EXCM (PS.INTLEVEL is not raised for level-1, so
     * no PS restore is needed -- our program runs with PS = 0). */
    esp_isr_leave_emit();
    return;
  }
  if (CMD_TYPE_USEC_DELAY == type) {
    esp_delay_create(pf, cmd_usec_delay_get(cmd));
    return;
  }
  if ((CMD_TYPE_SLEEP == type) || (CMD_TYPE_ASSERT == type)) {
    pfile_log(pf, PFILE_LOG_ERR,
        "ESP32: unexpected command type %u",
        (unsigned) type);
    return;
  }

  if (CMD_TYPE_OPERATOR != type) {
    pfile_log(pf, PFILE_LOG_ERR,
        "ESP32: unhandled command type %u", (unsigned) type);
    return;
  }

  {
    operator_t op  = cmd_optype_get(cmd);
    value_t    dst = cmd_opdst_get(cmd);
    value_t    v1  = cmd_opval1_get(cmd);
    value_t    v2  = cmd_opval2_get(cmd);

    /* soft-float routing (3i): check BEFORE the wide/scalar paths.
     * Mixed int/float operands convert through _esp_i2f (a2 in/out);
     * a float result stored into an integer dst converts through
     * _esp_f2i after the operation. */
    if (value_is_float(dst) || (v1 && value_is_float(v1))
        || (v2 && value_is_float(v2))) {
      unsigned hidx;

      if (!dst) {
        if (operator_is_relation(op) && v1 && v2) {
          esp_emit_float_compare_branch(pf, cmd, op, v1, v2);
        }
        return;
      }
      if (!v1) {
        return;
      }
      if (operator_is_relation(op) && v2) {
        esp_emit_float_compare_bool(pf, dst, op, v1, v2);
        return;
      }
      if (OPERATOR_NEG == op && v1) {
        /* float negation flips the sign bit (exact for every float,
         * incl. +-0, inf and NaN); the integer NEG would leave the
         * realm of valid encodings */
        esp_load_value(pf, v1, ESP_REG_A2);
        esp_emit_l32r(ESP_REG_A9, esp_pool_lit(0x80000000UL));
        (void) esp_code_alloc(ESP_OP_XOR, ESP_REG_A2, ESP_REG_A2,
            (int) ESP_REG_A9);
        esp_store_value(pf, dst, ESP_REG_A2);
        return;
      }
      if (OPERATOR_ASSIGN == op && !v2) {
        if (value_is_float(dst) != value_is_float(v1)) {
          /* int <-> float conversion assign */
          esp_load_value(pf, v1, ESP_REG_A2);
          esp_float_call(pf, value_is_float(dst) ? 5U : 4U);
        } else {
          esp_load_value(pf, v1, ESP_REG_A2);
        }
        esp_store_value(pf, dst, ESP_REG_A2);
        return;
      }
      if (v2) {
        if (value_is_float(v2)) {
          esp_load_value(pf, v2, ESP_REG_A3);
        } else {
          /* (float)v2: convert in a2, move to a3 (_esp_i2f preserves
           * a3) */
          esp_load_value(pf, v2, ESP_REG_A2);
          esp_float_call(pf, 5U);
          (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A3, ESP_REG_A2, 0);
        }
      } else {
        (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A3, 0, 0);
      }
      esp_load_value(pf, v1, ESP_REG_A2);
      if (!value_is_float(v1)) {
        esp_float_call(pf, 5U);          /* (float)v1 in a2 */
      }
      switch (op) {
        case OPERATOR_ADD:  hidx = 0; break;
        case OPERATOR_SUB:  hidx = 1; break;
        case OPERATOR_MUL:  hidx = 2; break;
        case OPERATOR_DIV:  hidx = 3; break;
        default:            hidx = 0; break;
      }
      esp_float_call(pf, hidx);
      if (!value_is_float(dst)) {
        esp_float_call(pf, 4U);          /* (int)result */
      }
      esp_store_value(pf, dst, ESP_REG_A2);
      return;
    }

    if ((OPERATOR_POINTER_ASSIGN != op) && esp_is_wide(dst, v1, v2)) {
      if (!dst) {
        /* dest-less INCR/DECR (`q++` statement) writes v1 itself */
        if ((OPERATOR_INCR == op) || (OPERATOR_DECR == op)) {
          if (v1) {
            walg_incr_decr(pf, &esp_walg_ops, op, v1, v1);
          }
          return;
        }
        /* dest-less test fused with the following goto */
        if (operator_is_relation(op) && v1 && v2) {
          (void) esp_wide_compare_branch(pf, cmd, op, v1, v2);
          return;
        }
        if ((OPERATOR_NOTL == op) || (OPERATOR_CMPB == op)) {
          (void) esp_wide_value_branch(pf, cmd, v1, BOOLEAN_TRUE);
          return;
        }
        if ((OPERATOR_ASSIGN == op) || (OPERATOR_LOGICAL == op)) {
          (void) esp_wide_value_branch(pf, cmd, v1, BOOLEAN_FALSE);
          return;
        }
        return;                   /* stripped test: no code */
      }
      if (OPERATOR_ASSIGN == op) {
        if (v1) {
          esp_wide_assign(pf, dst, v1);
        }
        return;
      }
      if ((OPERATOR_ADD == op) || (OPERATOR_SUB == op)
          || (OPERATOR_ANDB == op) || (OPERATOR_ORB == op)
          || (OPERATOR_XORB == op)) {
        if (v1 && v2) {
          walg_alu(pf, &esp_walg_ops, op, dst, v1, v2);
        }
        return;
      }
      if ((OPERATOR_SHIFT_LEFT == op) || (OPERATOR_SHIFT_RIGHT == op)
          || (OPERATOR_SHIFT_RIGHT_ARITHMETIC == op)) {
        if (v1 && v2) {
          if (value_is_const(v2)) {
            walg_shift_const(pf, &esp_walg_ops, op, dst, v1,
                (unsigned) value_const_get(v2));
          } else {
            walg_shift_var(pf, &esp_walg_ops, op, dst, v1, v2);
          }
        }
        return;
      }
      if ((OPERATOR_MUL == op) && v1 && v2) {
        walg_mul(pf, &esp_walg_ops, dst, v1, v2);
        return;
      }
      if (((OPERATOR_DIV == op) || (OPERATOR_MOD == op)) && v1 && v2) {
        walg_div(pf, &esp_walg_ops, op, dst, v1, v2);
        return;
      }
      if ((OPERATOR_NEG == op) && v1) {
        walg_neg(pf, &esp_walg_ops, dst, v1);
        return;
      }
      if (((OPERATOR_INCR == op) || (OPERATOR_DECR == op)) && v1) {
        walg_incr_decr(pf, &esp_walg_ops, op, dst, v1);
        return;
      }
      if (operator_is_relation(op) && v1 && v2) {
        esp_wide_compare_bool(pf, dst, op, v1, v2);
        return;
      }
      pfile_log(pf, PFILE_LOG_ERR,
          "ESP32 wide operator %u: 3b follow-up", (unsigned) op);
      return;
    }

    if (OPERATOR_ASSIGN == op) {
      unsigned sz;

      if (!dst || !v1) {
        return;
      }
      if (value_is_value(v1)) {
        /* x = WHEREIS(v): materialize the 32-bit address (wrappers
         * unwrap inside esp_load_address_of) */
        esp_load_address_of(pf, v1, ESP_REG_A2);
        esp_store_value(pf, dst, ESP_REG_A2);
        return;
      }
      if (value_is_pointer(dst) && value_is_array(v1)
          && !value_is_pointer(v1)) {
        /* Array → pointer slot (open-array bind / `p = buf`): store
         * the array's ADDRESS into the slot (mips rule).  Pre-fix
         * this fell to the generic tail, which loaded the array's
         * first bytes and stored them AS the pointer -- a silent
         * corrupted bind (slot held the data, not the address). */
        esp_load_address_of(pf, v1, ESP_REG_A2);
        esp_store_value(pf, dst, ESP_REG_A2);
        return;
      }
      if (value_is_pointer(dst) && !value_is_pointer(v1)
          && !value_is_array(v1) && !value_is_value(v1)
          && !value_is_label(v1)) {
        /* p = x: store x THROUGH the pointer (const allowed, mips
         * rule).  The width is the POINTEE's (the dst def carries
         * it) -- a const RHS has no byte size of its own and used to
         * truncate word/dword pointees to their low byte. */
        sz = esp_pointee_byte_sz(dst);
        if (!sz) {
          sz = (unsigned) value_byte_sz_get(v1);
        }
        if (!sz) {
          sz = 1;
        }
        esp_load_value(pf, v1, ESP_REG_A2);
        esp_store_deref(pf, dst, ESP_REG_A2, sz);
        return;
      }
      if (esp_value_is_bit_deref(v1)) {
        /* x = bits[i]: extract the bit through the pointer (the byte
         * deref below would load the whole container byte) */
        esp_load_value(pf, v1, ESP_REG_A2);
        esp_store_value(pf, dst, ESP_REG_A2);
        return;
      }
      esp_load_value(pf, v1, ESP_REG_A2);
      esp_store_value(pf, dst, ESP_REG_A2);
      return;
    }

    if (OPERATOR_POINTER_ASSIGN == op) {
      /* p is x: store the 32-bit address of x into the pointer slot.
       * An element target (slots[i] is proc) is direct storage at
       * &slots + i*ptrsz (baseofs + stride); a plain pointer target
       * writes its own 4-byte slot. */
      if (!dst || !v1) {
        return;
      }
      esp_load_address_of(pf, v1, ESP_REG_A2);
      {
        variable_t    v = value_variable_get(dst);
        unsigned long addr;

        if (!v) {
          pfile_log(pf, PFILE_LOG_ERR,
              "ESP32: 'is' target is not a pointer variable");
          return;
        }
        if (esp_elem_is_pointer_array_slot(dst)) {
          esp_emit_deref_addr(pf, dst);
        } else {
          addr = esp_var_ram(v);
          esp_emit_l32r(ESP_REG_A4, esp_pool_lit(addr));
        }
      }
      (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A2, ESP_REG_A4, 0);
      return;
    }

    if (!dst) {
      if (operator_is_relation(op) && v1 && v2) {
        esp_emit_compare_branch(pf, cmd, op, v1, v2);
        return;
      }
      if ((OPERATOR_NOTL == op) || (OPERATOR_CMPB == op)) {
        /* `!v; goto cond` / `v == false; goto cond`: complement --
         * condition true means v == 0 (mips_emit_not_branch rule) */
        esp_emit_truth_branch(pf, cmd, v1, BOOLEAN_TRUE);
      } else if (OPERATOR_LOGICAL == op) {
        /* `v; goto cond`: truth test -- condition true means v != 0 */
        esp_emit_truth_branch(pf, cmd, v1, BOOLEAN_FALSE);
      }
      return;                   /* stripped test: no code */
    }

    if (operator_is_relation(op)) {
      if (v1 && v2) {
        esp_emit_compare_bool(pf, dst, op, v1, v2);
      }
      return;
    }

    if ((OPERATOR_NOTL == op) || (OPERATOR_LOGICAL == op)) {
      /* materialize !v (v==0 -> 1, else 0) or normalize v to 0/1
       * (OPERATOR_LOGICAL shares the shape).  Build the result in
       * a3: the taken branch (v != 0) keeps the default, fallthrough
       * sets the other value. */
      boolean_t compl_ = (OPERATOR_NOTL == op);

      if (v1) {
        esp_code_t br;
        esp_code_t skip;

        esp_load_value(pf, v1, ESP_REG_A2);
        (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A3, 0,
            compl_ ? 0 : 1);
        br = esp_code_alloc(ESP_OP_BNEZ, 0, ESP_REG_A2, 0);
        (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A3, 0,
            compl_ ? 1 : 0);
        skip = esp_code_label_alloc();
        if (br) {
          br->target = skip;
        }
        esp_store_value(pf, dst, ESP_REG_A3);
      }
      return;
    }

    if (OPERATOR_CMPB == op) {
      /* `!x` is a bitwise complement (jal_expr maps it to CMPB);
       * only a single-bit dst narrows it to 0/1 (mips XORI -1 rule).
       * The fused `if !bit` branch keeps the boolean shape above. */
      if (v1) {
        esp_load_value(pf, v1, ESP_REG_A2);
        (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A3, 0, -1);
        (void) esp_code_alloc(ESP_OP_XOR, ESP_REG_A2, ESP_REG_A2,
            (int) ESP_REG_A3);
        if (value_is_single_bit(dst)) {
          (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A3, 0, 1);
          (void) esp_code_alloc(ESP_OP_AND, ESP_REG_A2, ESP_REG_A2,
              (int) ESP_REG_A3);
        }
        esp_store_value(pf, dst, ESP_REG_A2);
      }
      return;
    }

    if ((OPERATOR_DIV == op) || (OPERATOR_MOD == op)) {
      if (v1 && v2) {
        esp_emit_divmod(pf, op, v1, v2);
        esp_store_value(pf, dst, ESP_REG_A2);
      }
      return;
    }

    if ((OPERATOR_ADD == op) || (OPERATOR_SUB == op)
        || (OPERATOR_MUL == op) || (OPERATOR_ANDB == op)
        || (OPERATOR_ORB == op) || (OPERATOR_XORB == op)) {
      static const esp_opcode_t map[6] = {
        ESP_OP_ADD, ESP_OP_SUB, ESP_OP_MULL,
        ESP_OP_AND, ESP_OP_OR, ESP_OP_XOR
      };
      int idx = (OPERATOR_ADD == op) ? 0 : (OPERATOR_SUB == op) ? 1
             : (OPERATOR_MUL == op) ? 2 : (OPERATOR_ANDB == op) ? 3
             : (OPERATOR_ORB == op)  ? 4 : 5;

      if (!v1) {
        return;
      }
      if (v2) {
        esp_load_value(pf, v2, ESP_REG_A3);
      }
      esp_load_value(pf, v1, ESP_REG_A2);
      (void) esp_code_alloc(map[idx], ESP_REG_A2, ESP_REG_A2,
          (int) ESP_REG_A3);
      esp_store_value(pf, dst, ESP_REG_A2);
      return;
    }

    if ((OPERATOR_SHIFT_LEFT == op) || (OPERATOR_SHIFT_RIGHT == op)
        || (OPERATOR_SHIFT_RIGHT_ARITHMETIC == op)) {
      if (!v1) {
        return;
      }
      /* SAR is programmed only after both operand loads: loading the
       * value (bit reads, sign extension) clobbers SAR itself */
      esp_load_value(pf, v1, ESP_REG_A2);
      if (v2) {
        esp_load_value(pf, v2, ESP_REG_A5);
      }
      (void) esp_code_alloc(OPERATOR_SHIFT_LEFT == op ? ESP_OP_SSL : ESP_OP_SSR,
          0, ESP_REG_A5, 0);
      (void) esp_code_alloc(OPERATOR_SHIFT_LEFT == op ? ESP_OP_SLL
          : ((OPERATOR_SHIFT_RIGHT_ARITHMETIC == op)
              || value_is_signed(v1) ? ESP_OP_SRA : ESP_OP_SRL),
          ESP_REG_A2, ESP_REG_A2, 0);
      esp_store_value(pf, dst, ESP_REG_A2);
      return;
    }

    if (OPERATOR_NEG == op) {
      if (!v1) {
        return;
      }
      esp_load_value(pf, v1, ESP_REG_A2);
      (void) esp_code_alloc(ESP_OP_NEG, ESP_REG_A2, ESP_REG_A2, 0);
      esp_store_value(pf, dst, ESP_REG_A2);
      return;
    }

    if ((OPERATOR_INCR == op) || (OPERATOR_DECR == op)) {
      if (!v1) {
        return;
      }
      esp_load_value(pf, v1, ESP_REG_A2);
      (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A2, ESP_REG_A2,
          (OPERATOR_INCR == op) ? 1 : -1);
      esp_store_value(pf, dst ? dst : v1, ESP_REG_A2);
      return;
    }

    pfile_log(pf, PFILE_LOG_ERR,
        "ESP32: unhandled operator %u", (unsigned) op);
  }
}

void esp_op_reset(void)
{
  unsigned ii;

  esp_op.skip_cmd = 0;
  esp_op.fixup_ct = 0;
  esp_op.call_pend_ct = 0;
  esp_op.goto_pend_ct = 0;
  esp_bitarg_ct = 0;
  for (ii = 0; ii < 4; ii++) {
    esp_op.helpers[ii].used = BOOLEAN_FALSE;
  }
  /* wide RAM temporaries (kernel-held) */
  walg_reset();
}
