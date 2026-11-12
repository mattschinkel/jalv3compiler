/************************************************************
 **
 ** pic_op_indirect.c : PIC operator generation -- indirect / pointer-slot access
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 **           (c) 2020-2025, Rob Jansen
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: split out of pic_op.c (Tier-A decomposition, 2026-10-31);
 **          code moved verbatim, forward declarations replaced by
 **          pic_op_priv.h prototypes.
 **
 ************************************************************/

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "../libcore/pf_proc.h"
#include "../libcore/pf_expr.h"
#include "../libcore/pf_op.h"
#include "../libcore/pf_src.h"
#include "pic_access.h"
#include "pic_opfn.h"
#include "pic_stvar.h"
#include "pic_var.h"
#include "piccolst.h"
#include "pic_inst.h"
#include "pic_op.h"
#include "pic_op_priv.h"

/* defined in this TU (prototypes keep definition order free) */
static boolean_t pic_value_is_array_pointer_slot_storage(const value_t val);
static boolean_t pic_pointer_runtime_null_guard_eligible(variable_t v);
static void pic_pointer_slot_emit_null_guard(pfile_t *pf, value_t base,
    variable_sz_t slot_offs);
void pic_pointer_value_emit_null_guard(pfile_t *pf, value_t ptr);
static boolean_t pic_value_indirect_heap_string_through_pointer(const value_t val);
static value_t pic_indirect_value_get(value_t val1, value_t val2, value_t val3);

/* True array-of-pointers (e.g. **`var pointer var pointer var byte x[N]`**):
 * outer def is ARRAY of POINTER; FSR must load from the pointer *slot* words,
 * not linear *slot+i pointee math (MOVLW-on-indirect **`v__ind`** bug on PIC14).
 * Folded **`var pointer var byte p[N]`** is POINTER→ARRAY (one pointer slot).
 *
 * U2b-2 migrated the early callers to `aop`; the pass-1 escape analysis needs
 * the identity WITHOUT computing a plan (a plan_get would add a recorded
 * site), so the predicate is exported for that one use. */
boolean_t pic_value_is_array_of_pointers(const value_t val)
{
  variable_t              v;
  variable_def_t        vd;
  variable_def_member_t   mbr;
  variable_def_t        em;

  v = value_variable_get(val);
  if (!v || VARIABLE_NONE == v) {
    return BOOLEAN_FALSE;
  }
  vd = variable_def_get(v);
  if (!vd || VARIABLE_DEF_TYPE_ARRAY != variable_def_type_get(vd)) {
    return BOOLEAN_FALSE;
  }
  mbr = variable_def_member_get(vd);
  em  = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
  return (em && VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(em));
}

/* The slot's POINTEE is an array (pa[k][j]): its base offset is a pointee
 * subscript, so the walked-bind classification stays right. */
boolean_t pic_value_slot_pointee_array(const value_t val)
{
  variable_t              v;
  variable_def_t          adef;
  variable_def_member_t   ambr;
  variable_def_t          pdef;
  variable_def_member_t   pmbr;
  variable_def_t          pointee;

  if (!pic_value_is_array_of_pointers(val)) {
    return BOOLEAN_FALSE;
  }
  v     = value_variable_get(val);
  adef  = variable_def_get(v);
  ambr  = adef ? variable_def_member_get(adef) : 0;
  pdef  = ambr ? variable_def_member_def_get(ambr) : VARIABLE_DEF_NONE;
  pmbr  = pdef ? variable_def_member_get(pdef) : 0;
  pointee = pmbr ? variable_def_member_def_get(pmbr) : VARIABLE_DEF_NONE;
  return (pointee
      && (VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(pointee)))
    ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

static boolean_t pic_value_is_array_pointer_slot_storage(const value_t val)
{
  return value_flag_test(val, VALUE_FLAG_PTR_SLOT_RAW);
}

/* True if we should emit a runtime "all slot bytes zero" check before loading
 * FSR from a pointer variable. Skips legacy string / device_put paths that
 * are not normal `var pointer` GPR slots. */
static boolean_t pic_pointer_runtime_null_guard_eligible(variable_t v)
{
  const char *nm;

  if (!v || VARIABLE_NONE == v || !variable_is_pointer(v)) {
    return BOOLEAN_FALSE;
  }
  nm = variable_name_get(v);
  if (nm && (strstr(nm, "_device_put")
      || strstr(nm, "_str_")
      || strstr(nm, "___str"))) {
    return BOOLEAN_FALSE;
  }
  return BOOLEAN_TRUE;
}

/*
 * If every byte of the pointer slot at base[slot_offs..] is zero, jump to a
 * local infinite loop (never uses a bogus FSR). Z=1 after IOR means all bytes
 * read as 0 (typical cleared BSS / never bound). BTFSS STATUS,Z skips the
 * `goto lbl_ok` when Z=1 (all slot bytes zero) so execution falls into hang.
 */
static void pic_pointer_slot_emit_null_guard(pfile_t *pf, value_t base,
    variable_sz_t slot_offs)
{
  label_t       lbl_hang;
  label_t       lbl_ok;
  variable_sz_t psz;
  variable_sz_t jj;
  variable_t    v;

  v = value_variable_get(base);
  if (!pic_pointer_runtime_null_guard_eligible(v)) {
    return;
  }
  lbl_hang = pfile_label_alloc(pf, 0);
  lbl_ok   = pfile_label_alloc(pf, 0);
  psz = pic_pointer_size_get(pf);
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, base, slot_offs + 0, PIC_OPDST_W);
  for (jj = 1; jj < psz; jj++) {
    pic_instr_append_f_d(pf, PIC_OPCODE_IORWF, base, slot_offs + jj,
        PIC_OPDST_W);
  }
  pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
  pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_ok);
  pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_hang);
  pic_instr_append_label(pf, lbl_hang);
  pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_hang);
  pic_instr_append_label(pf, lbl_ok);
  label_release(lbl_hang);
  label_release(lbl_ok);
}

/* Runtime null guard for a pointer VALUE held in a scratch slot (the
 * runtime-index pointer-array element loads its pointer into _pic_pointer
 * before re-staging FSR).  Without this, an unbound element is dereferenced
 * as address zero / adjacent RAM; the const-index path already guards the
 * slot bytes. */
void pic_pointer_value_emit_null_guard(pfile_t *pf, value_t ptr)
{
  label_t       lbl_hang;
  label_t       lbl_ok;
  variable_sz_t psz;
  variable_sz_t jj;

  if (!ptr) {
    return;
  }
  lbl_hang = pfile_label_alloc(pf, 0);
  lbl_ok   = pfile_label_alloc(pf, 0);
  psz = pic_pointer_size_get(pf);
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ptr, 0, PIC_OPDST_W);
  for (jj = 1; jj < psz; jj++) {
    pic_instr_append_f_d(pf, PIC_OPCODE_IORWF, ptr, jj, PIC_OPDST_W);
  }
  pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
  pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_ok);
  pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_hang);
  pic_instr_append_label(pf, lbl_hang);
  pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_hang);
  pic_instr_append_label(pf, lbl_ok);
  label_release(lbl_hang);
  label_release(lbl_ok);
}

/* Dereferenced `var pointer … as var string`: pointee is STRING_DYNAMIC.
 * Indexing the handle must load FSR from the pointer slot (`*p`+field), not
 * LFSR to the slot's file address (`p`+field). */
static boolean_t pic_value_indirect_heap_string_through_pointer(const value_t val)
{
  variable_t            v;
  variable_def_t        def;
  variable_def_member_t mbr;
  variable_def_t        pt;

  if (!value_is_indirect(val) || !value_flag_test(val, VALUE_FLAG_POINTER)) {
    return BOOLEAN_FALSE;
  }
  v = value_variable_get(val);
  if (!v || !variable_is_pointer(v)) {
    return BOOLEAN_FALSE;
  }
  def = variable_def_get(v);
  if (!def) {
    return BOOLEAN_FALSE;
  }
  mbr = variable_def_member_get(def);
  if (!mbr) {
    return BOOLEAN_FALSE;
  }
  pt = variable_def_member_def_get(mbr);
  return (pt && variable_def_flag_test(pt,
      VARIABLE_DEF_FLAG_STRING | VARIABLE_DEF_FLAG_STRING_DYNAMIC))
      ? BOOLEAN_TRUE
      : BOOLEAN_FALSE;
}

/* FSR is already at a pointer SLOT: load the slot's pointer bytes into the
 * scratch, trap when unbound, and re-stage FSR at the loaded pointer so the
 * caller's INDF access hits the element's real target.  Shared by the
 * runtime-indexed slot arm and the chunked const-index arm (P2b-2). */
static void pic_pointer_slot_deref_stage(pfile_t *pf)
{
  value_t       indv;
  value_t       fsrv;
  value_t       ptmp;
  boolean_t     wide16;
  variable_sz_t jj;
  variable_sz_t psz;

  wide16 = pic_is_16bit(pf) || pic_is_14bit_hybrid(pf);
  psz = pic_pointer_size_get(pf);
  indv = pic_indirect_get(pf, PFILE_LOG_ERR, 0);
  fsrv = pic_fsr_get(pf);
  ptmp = pic_var_pointer_get(pf);
  for (jj = 0; jj < psz; jj++) {
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, indv, 0, PIC_OPDST_W);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, ptmp, jj);
    if ((jj + 1) < psz) {
      pic_instr_append_f_d(pf, PIC_OPCODE_INCF, fsrv, 0, PIC_OPDST_F);
      if (wide16) {
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
        pic_instr_append_reg(pf, PIC_OPCODE_INCF, "_fsr0h");
      }
    }
  }
  pic_pointer_value_emit_null_guard(pf, ptmp);
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ptmp, 0, PIC_OPDST_W);
  pic_instr_append_f(pf, PIC_OPCODE_MOVWF, fsrv, 0);
  if (wide16) {
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ptmp, 1, 0);
    pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0h");
  } else {
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_irp");
    pic_instr_append_f_d(pf, PIC_OPCODE_BTFSC, ptmp, 1, 0);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status", "_irp");
  }
  pic_var_pointer_release(pf, ptmp);
  value_release(fsrv);
  value_release(indv);
}

/* U4-4: the plan must be internally consistent.  The audit sweep over the
 * corpus (864 programs, 75,037 accesses) found no violation; this keeps it
 * that way -- an edit that breaks the kind/facts relation is reported the
 * moment the audit is on, instead of waiting for a witness to notice. */
static boolean_t pic_access_plan_consistent(const pic_access_plan_t *p)
{
  if (p->aop) {
    return (PIC_ACCESS_RESOLVE == p->kind) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  }
  switch (p->kind) {
  case PIC_ACCESS_WALK_BAKED:
    return p->value_map ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  case PIC_ACCESS_WALK_RUNTIME:
    return p->runtime_map ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  case PIC_ACCESS_DIRECT:
    return (p->value_map || p->runtime_map || p->chunked)
      ? BOOLEAN_FALSE : BOOLEAN_TRUE;
  default:
    return BOOLEAN_TRUE;
  }
}

/*
 * DX F1: STRICT JALV2_* switch parsing.
 *
 * The project rule: unset/empty means the switch's DEFAULT; ON is
 * 1/true/yes/on and OFF is 0/false/no/off, case-insensitive and with
 * surrounding whitespace ignored.  Anything else prints ONE stderr line
 * with the stable code JALV2-SWITCH and the effective mode, and the default
 * is used.  The old rule ("anything not 0/false/no/off is ON") made a typo
 * or `=FALSE` silently enable a feature, so the documented rollback
 * (`JALV2_FAT_POINTERS=0`) could fail while the compile still succeeded.
 */
static int pic_env_lower(int c)
{
  return ((c >= 'A') && (c <= 'Z')) ? c + ('a' - 'A') : c;
}

static boolean_t pic_env_word_is(const char *v, size_t n, const char *word)
{
  size_t ii;

  for (ii = 0; ii < n; ii++) {
    if ((0 == word[ii])
        || (pic_env_lower((unsigned char) v[ii]) != word[ii])) {
      return BOOLEAN_FALSE;
    }
  }
  return (0 == word[n]) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* one warning per switch per process (the switches are read many times) */
static const char *pic_env_warned[16];
static size_t      pic_env_warned_ct;

static boolean_t pic_env_warn_once(const char *name)
{
  size_t ii;

  for (ii = 0; ii < pic_env_warned_ct; ii++) {
    if (0 == strcmp(pic_env_warned[ii], name)) {
      return BOOLEAN_FALSE;
    }
  }
  if (pic_env_warned_ct < COUNT(pic_env_warned)) {
    pic_env_warned[pic_env_warned_ct++] = name;
  }
  return BOOLEAN_TRUE;
}

boolean_t pic_env_on_default(const char *name, boolean_t def)
{
  const char *v;
  const char *s;
  const char *e;
  size_t      n;

  v = getenv(name);
  if (0 == v) {
    return def;
  }
  s = v;
  while ((' ' == *s) || ('\t' == *s)) {
    s++;
  }
  e = s + strlen(s);
  while ((e > s) && ((' ' == e[-1]) || ('\t' == e[-1]))) {
    e--;
  }
  n = (size_t) (e - s);
  if (0 == n) {
    return def;
  }
  if (pic_env_word_is(s, n, "1") || pic_env_word_is(s, n, "true")
      || pic_env_word_is(s, n, "yes") || pic_env_word_is(s, n, "on")) {
    return BOOLEAN_TRUE;
  }
  if (pic_env_word_is(s, n, "0") || pic_env_word_is(s, n, "false")
      || pic_env_word_is(s, n, "no") || pic_env_word_is(s, n, "off")) {
    return BOOLEAN_FALSE;
  }
  if (pic_env_warn_once(name)) {
    fprintf(stderr,
        "JALV2-SWITCH: %s='%.*s' is not on/off; using %s "
        "(doc/designs/one-access-model.md#feature-switches)\n",
        name, (int) n, s, def ? "on" : "off");
  }
  return def;
}

/* the historical test-hook rule: unset/empty/invalid default to OFF */
boolean_t pic_env_on(const char *name)
{
  return pic_env_on_default(name, BOOLEAN_FALSE);
}

void pic_features_report(void)
{
  printf("JALV2 feature switches (environment; a program's pragma overrides "
      "the fat width per compile):\n");
  printf("  fat pointers      : %s  (JALV2_FAT_POINTERS; default on)\n",
      pic_env_on_default("JALV2_FAT_POINTERS", BOOLEAN_TRUE) ? "on" : "off");
  printf("  fat force (test)  : %s  (JALV2_FAT_FORCE)\n",
      pic_env_on("JALV2_FAT_FORCE") ? "on" : "off");
  {
    const char *ptr3;

    ptr3 = getenv("JALV2_FAT_PTR3");
    if (ptr3 && *ptr3) {
      printf("  14H pointer width : %s  (JALV2_FAT_PTR3 override)\n",
          pic_env_on_default("JALV2_FAT_PTR3", BOOLEAN_TRUE)
            ? "3-byte" : "2-byte");
    } else {
      printf("  14H pointer width : default (3-byte when the engine is on; "
          "JALV2_FAT_PTR3 overrides)\n");
    }
  }
  printf("  layout trials     : %s  (JALV2_NO_LAYOUT_TRIALS disables)\n",
      pic_env_on("JALV2_NO_LAYOUT_TRIALS") ? "off" : "on");
  printf("  access recorder   : %s  (JALV2_ACCESS_RECORD)\n",
      pic_env_on("JALV2_ACCESS_RECORD") ? "on" : "off");
  printf("  access audit      : %s  (JALV2_ACCESS_AUDIT)\n",
      pic_env_on("JALV2_ACCESS_AUDIT") ? "on" : "off");
  printf("  coverage report   : %s  (JALV2_ACCESS_COVERAGE)\n",
      pic_env_on("JALV2_ACCESS_COVERAGE") ? "on" : "off");
}

static boolean_t pic_access_audit_on(void)
{
  static int state = -1;

  if (state < 0) {
    state = pic_env_on("JALV2_ACCESS_AUDIT") ? 1 : 0;
  }
  return state ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/*
 * NAME
 *   pic_access_plan_fill_var
 *
 * DESCRIPTION
 *   U3-3: fill the VARIABLE-derived half of the access plan.  The FSR walk
 *   helpers hold a variable_t, not a value_t, so they use this entry point
 *   directly; pic_access_plan_get() calls it after computing the value-only
 *   facts.  The value-derived fields (aop, kind) are left zero here.
 *
 *   chunked     : the object view says more than one segment.
 *   runtime_map : an open-array formal fed a runtime chunk map.
 *   holed       : the object is split across the data area.
 *   sv_view     : a classic string-view formal (its base rides a cell).
 *   value_map   : the OR of holed/runtime_map/sv_view -- the walk question.
 *   bind_walk   : the unescaped bind target, or VARIABLE_NONE.
 *   ptr_lookup / ptr_read_call: the two pointer-storage-class answers.
 */
void pic_access_plan_fill_var(variable_t var, pic_access_plan_t *plan)
{
  memset(plan, 0, sizeof(*plan));
  if ((VARIABLE_NONE == var) || !var) {
    return;
  }
  plan->chunked = pic_mem_object_is_chunked(var);
  plan->runtime_map = (VARIABLE_NONE
      != pic_var_openarray_formal_map_get(var)) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  plan->holed = pic_variable_chunks_holed(var);
  plan->sv_view = pic_var_strview_classic_formal(var);
  /* U2b (walk family): a value walks when its own object is holed, when it is
   * a runtime-map formal, when it is a classic string-view formal (the
   * payload base rides a per-formal cell), or when it is an unescaped bind
   * whose target is chunked.  The first three were OR'd inline in the
   * indirect setup; the fourth looked the same target up at four branches. */
  plan->value_map = (boolean_t) (plan->holed || plan->runtime_map
      || plan->sv_view);
  plan->bind_walk = pic_var_pointer_bind_walk_target(var);
  plan->ptr_lookup = variable_flag_test(var, VARIABLE_FLAG_PTR_LOOKUP);
  plan->ptr_ptr = variable_flag_test(var, VARIABLE_FLAG_PTR_PTR);
  plan->ptr_eeprom = variable_flag_test(var, VARIABLE_FLAG_PTR_EEPROM);
  plan->ptr_flash = variable_flag_test(var, VARIABLE_FLAG_PTR_FLASH);
  plan->ptr_read_call = (boolean_t) ((plan->ptr_lookup && plan->ptr_ptr)
      || plan->ptr_eeprom || plan->ptr_flash);
  plan->ptr_class = (boolean_t) (plan->ptr_lookup || plan->ptr_read_call);
  plan->runtime_ptr_class = (boolean_t) (plan->runtime_map
      && plan->ptr_class);
}

/*
 * NAME
 *   pic_access_plan_compute
 *
 * DESCRIPTION
 *   U2a: classify how a value is reached, ONCE, so an emitter asks one
 *   question instead of re-deriving the answer from the object view, the
 *   chunk map and the pointer flags at every step.  pic_indirect_setup is
 *   the first consumer; U2b migrates the other emitter families and U3
 *   deletes the parallel mechanisms the plan replaces.
 *
 *   aop     : an array-of-pointers element.  The slot address is RESOLVED
 *             through the object's segments, but the slot itself must never
 *             be walked: the walk would leave FSR at the slot and the caller
 *             would read the pointer bytes as data (P2b-2).  This is the
 *             single decision behind the five "do not walk" branches.
 *   chunked : the object has more than one segment (the object view's
 *             answer, not a second reading of the chunk map -- U0c).
 *
 *   The value-only facts (aop, kind) are decided here; the rest comes from
 *   pic_access_plan_fill_var().
 */
static void pic_access_plan_compute(pfile_t *pf, const value_t val,
    pic_access_plan_t *plan)
{
  boolean_t      aop;
  boolean_t      fat;
  boolean_t      slot_const_ofs;
  variable_t     var;
  variable_def_t def;

  aop = pic_value_is_array_of_pointers(val);
  slot_const_ofs = ((VALUE_NONE == value_baseofs_get(val))
      || value_is_const(value_baseofs_get(val)))
    ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  var = value_variable_get(val);
  pic_access_plan_fill_var(var, plan);
  plan->fat_engine = pic_fat_pointers_enabled(pf);
  plan->aop = aop;
  plan->indirect = value_is_indirect(val);
  /* G3: the slot's POINTEE type.  An ARRAY pointee keeps the walked-bind
   * classification (its base offset carries the pointee subscript); a
   * scalar pointee's base offset is only the slot byte offset, so the deref
   * must read the slot's value instead. */
  plan->slot_pointee_array = pic_value_slot_pointee_array(val);
  /* U4-3: the fat family's question, answered once.  `value_is_pointer()` is
   * false at the deref site, so the DEFINITION answers -- this is the test
   * the emitter used to apply itself.  Step 4 removes the field: `kind`
   * carries the answer and the recipe table dispatches on it. */
  fat = BOOLEAN_FALSE;
  if (!aop && (VARIABLE_NONE != var) && (0 != var)
      && pic_fat_pointers_enabled(pf) && (0 != pic_fat_object_ct())) {
    def = variable_def_get(var);
    fat = ((0 != def)
        && (VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(def)))
      ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  }

  /* U4-2: `kind` answers how the OBJECT is addressed, so the walk
   * classification wins over LINEAR -- an indirect value into a chunked
   * object is still walked (the audit printed `kind=2 chunked=1 vmap=1`
   * before this, which described the value, not the access).  `kind` has no
   * behavioural consumer yet; this makes it truthful for when it gets one. */
  if (plan->aop) {
    /* G3: a SCALAR-pointee slot under the fat engine holds a tagged fat
     * pointer, so the deref dispatches on the slot's tag (the slot staging
     * has already loaded the slot's value; tag 0 keeps it).  An ARRAY-pointee
     * slot stays on the walked-bind classification until surface 3. */
    plan->kind = (pic_fat_pointers_enabled(pf)
        && (0 != pic_fat_object_ct()) && !plan->slot_pointee_array
        && slot_const_ofs)
      ? PIC_ACCESS_FAT : PIC_ACCESS_RESOLVE;
  } else if (fat) {
    /* a fat deref overrides the staging: the tag decides at run time */
    plan->kind = PIC_ACCESS_FAT;
  } else if (plan->runtime_map) {
    plan->kind = PIC_ACCESS_WALK_RUNTIME;
  } else if (plan->chunked || plan->value_map) {
    plan->kind = PIC_ACCESS_WALK_BAKED;
  } else if (value_is_indirect(val)) {
    plan->kind = PIC_ACCESS_LINEAR;
  } else {
    plan->kind = PIC_ACCESS_DIRECT;
  }
}

/*
 * NAME
 *   pic_access_plan_get
 *
 * DESCRIPTION
 *   Cache wrapper: return the stored plan when the value's record and the
 *   layout generation match, otherwise compute and store it.  Under
 *   JALV2_ACCESS_VERIFY a cache hit is recomputed and compared; a
 *   disagreement is the fatal access-plan mismatch (C2).
 */
void pic_access_plan_get(pfile_t *pf, const value_t val,
    pic_access_plan_t *plan)
{
  variable_t var;

  if (pic_access_plan_load(val, plan)) {
    /* A3: the stored record is verified against a fresh computation under
     * the gate; a disagreement is fatal (C2), never a silent stale plan. */
    if (pic_env_on("JALV2_ACCESS_VERIFY")) {
      pic_access_plan_t fresh;

      pic_access_plan_compute(pf, val, &fresh);
      if (pic_env_on("JALV2_ACCESS_VERIFY_FAULT")) {
        fresh.ptr_lookup = (boolean_t) !fresh.ptr_lookup;
      }
      if (0 != memcmp(&fresh, plan, sizeof(fresh))) {
        pfile_log(pf, PFILE_LOG_ERR,
            "internal error: access plan mismatch kind=%u/%u aop=%u/%u"
            " lookup=%u/%u rt=%u/%u generation=%lu",
            (unsigned) plan->kind, (unsigned) fresh.kind,
            (unsigned) plan->aop, (unsigned) fresh.aop,
            (unsigned) plan->ptr_lookup, (unsigned) fresh.ptr_lookup,
            (unsigned) plan->runtime_map, (unsigned) fresh.runtime_map,
            pic_access_generation_get());
      }
    }
    return;
  }
  pic_access_plan_compute(pf, val, plan);
  var = value_variable_get(val);
  pic_access_plan_store(pf, val, plan);
  if (pic_access_record_on()) {
    pfile_source_t *src;

    src = pfile_source_get(pf);
    pic_access_record_begin(pf, (src) ? pfile_source_name_get(src) : "?");
    pic_access_record_plan(pf, "plan_get", var, plan);
  }
  if (pic_access_audit_on()) {
    if (pic_env_on("JALV2_ACCESS_AUDIT_FAULT")) {
      /* test hook: break kind so the checker below must fire */
      plan->kind = PIC_ACCESS_DIRECT;
    }
    if (!pic_access_plan_consistent(plan)) {
      /* once per compile: the point is that it fired, not how often */
      static int reported = 0;

      if (!reported) {
        reported = 1;
        fprintf(stderr,
            "jal access audit: INCONSISTENT plan kind=%u aop=%u vmap=%u rt=%u\n",
            (unsigned) plan->kind, (unsigned) plan->aop,
            (unsigned) plan->value_map, (unsigned) plan->runtime_map);
      }
    }
  }
}

/*
 * NAME
 *   pic_access_plan_fill_var_at
 *
 * DESCRIPTION
 *   Variable-only external sites: fill the variable-derived plan, then
 *   record the variable-keyed matrix row (Step 1b).  The internal call
 *   from pic_access_plan_get stays on pic_access_plan_fill_var so one
 *   value is recorded once.
 */
void pic_access_plan_fill_var_at(pfile_t *pf, const char *site,
    variable_t var, pic_access_plan_t *plan)
{
  if (!pic_access_var_load(var, plan)) {
    pic_access_plan_fill_var(var, plan);
    plan->fat_engine = pic_fat_pointers_enabled(pf);
    pic_access_var_store(pf, var, plan);
  }
  if (pic_access_record_on()) {
    pfile_source_t *src;

    src = pfile_source_get(pf);
    pic_access_record_begin(pf, (src) ? pfile_source_name_get(src) : "?");
    pic_access_record_plan(pf, site, var, plan);
  }
}

/* F2d: add the RUNTIME part of a subscript to the object-relative offset.
 * `pic_indirect_setup`'s linear staging has already added the subscript to
 * the FSR, but a fat pointer does not use that linear address: the walk's
 * `_pic_chunk_t` is a separate 16-bit object offset, and without this add it
 * was `pointer offset + constant access offset` only -- so `p[i]` resolved
 * offset 0 and read `p[0]`.  The high half is added only when the index has
 * one; an in-object offset never exceeds 16 bits. */
static void pic_fat_offset_add_runtime(pfile_t *pf, value_t t, value_t idx)
{
  variable_sz_t idx_sz;

  if ((VALUE_NONE == idx) || !idx || value_is_const(idx)) {
    return;
  }
  idx_sz = (value_byte_sz_get(idx) > 1) ? 2 : 1;
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, idx, 0, PIC_OPDST_W);
  pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, t, 0, PIC_OPDST_F);
  pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
  pic_instr_append_f_d(pf, PIC_OPCODE_INCF, t, 1, PIC_OPDST_F);
  if (2 == idx_sz) {
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, idx, 1, PIC_OPDST_W);
    pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, t, 1, PIC_OPDST_F);
  }
}

/* F2d: the O(1) dispatch.  The old compare chain cost one
 * `movf/andlw/xorlw/btfss/goto` block PER OBJECT ID at EVERY deref site, so
 * its code grew with the number of fat objects (a 127-id program was ~635
 * instructions per site).  Here the tag is shifted into the object id,
 * range-checked, and handed to the one shared `_seg_addr` walk, which looks
 * the object's descriptor up in `_obj_desc[]`.  Cost per site is constant.
 *
 * Tag 0 still leaves the linear staging standing (which already carries a
 * runtime subscript); a tag past the id table traps exactly like an
 * unmatched chain arm did. */
static void pic_fat_deref_table_emit(pfile_t *pf, value_t pv,
    value_t idx, size_t ofs_total, unsigned top, unsigned shift,
    unsigned mask, unsigned ofs_mask)
{
  variable_t idv;
  variable_t arg;
  value_t    idval;
  value_t    argval;
  label_t    seg;
  label_t    done;
  label_t    trap;
  unsigned   ids;
  unsigned   ii;

  idv  = pic_fat_id_get();
  arg  = pic_var_chunk_walk_arg_get();
  seg  = pic_fat_seg_addr_label();
  ids  = (unsigned) pic_fat_object_ct();
  if ((VARIABLE_NONE == idv) || !arg || (VARIABLE_NONE == arg)
      || (LABEL_NONE == seg)) {
    return;
  }
  idval  = value_alloc(idv);
  argval = value_alloc(arg);
  if (!idval || !argval) {
    if (idval)  { value_release(idval); }
    if (argval) { value_release(argval); }
    return;
  }
  done = pfile_label_alloc(pf, 0);
  trap = pfile_label_alloc(pf, 0);

  /* tag 0: the linear staging that already ran is the answer */
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, pv, (variable_sz_t) top,
      PIC_OPDST_W);
  pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, (uchar) mask);
  pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
  pic_instr_append_n(pf, PIC_OPCODE_GOTO, done);

  /* id = tag >> shift.  A shift of 4+ is one SWAPF plus the remainder: the
   * tag sits above bit 3, so swapping the nibbles brings it down 4 bits and
   * the masked bits below it are zero. */
  pic_instr_append_f(pf, PIC_OPCODE_MOVWF, idval, 0);
  if (shift >= 4) {
    pic_instr_append_f_d(pf, PIC_OPCODE_SWAPF, idval, 0, PIC_OPDST_W);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, idval, 0);
    shift -= 4;
  }
  for (ii = 0; ii < shift; ii++) {
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_c");
    pic_instr_append_f_d(pf, PIC_OPCODE_RRF, idval, 0, PIC_OPDST_F);
  }

  /* id <= ids ?  SUBLW sets C when ids >= id (the id table has ids entries) */
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, idval, 0, PIC_OPDST_W);
  pic_instr_append_w_kn(pf, PIC_OPCODE_SUBLW, (uchar) ids);
  pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_c");
  pic_instr_append_n(pf, PIC_OPCODE_GOTO, trap);

  /* arg = pointer offset + access offset (the add is always emitted, so the
   * carry into the high byte is this ADD's, never a stale one) */
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, pv, 0, PIC_OPDST_W);
  pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, (uchar) (ofs_total & 0xFF));
  pic_instr_append_f(pf, PIC_OPCODE_MOVWF, argval, 0);
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, pv, 1, PIC_OPDST_W);
  if (0xFFU != ofs_mask) {
    pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, (uchar) ofs_mask);
  }
  pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
  pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, 1);
  if (ofs_total >> 8) {
    pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW,
        (uchar) ((ofs_total >> 8) & 0xFF));
  }
  pic_instr_append_f(pf, PIC_OPCODE_MOVWF, argval, 1);
  /* the runtime part of a subscript (p[i]): the linear staging put it in the
   * FSR, which this path overwrites, so it has to join the object offset */
  pic_fat_offset_add_runtime(pf, argval, idx);
  pic_instr_append_n(pf, PIC_OPCODE_CALL, seg);
  pic_instr_append_n(pf, PIC_OPCODE_GOTO, done);

  /* the tag named no object: trap, as the chain's unmatched fall-through did */
  pic_instr_append_label(pf, trap);
  pic_instr_append_n(pf, PIC_OPCODE_GOTO, trap);
  pic_instr_append_label(pf, done);

  label_release(trap);
  label_release(done);
  value_release(argval);
  value_release(idval);
}

/* F2b/F2d: the fat deref.  Emitted AFTER the linear staging (for a fat
 * pointer that address is unused), so every legacy path stays byte-identical.
 * A 2-byte pointer packs the tag into the offset's top byte, so its high
 * offset byte is masked before the carry add (ANDLW leaves the carry alone).
 *
 * The dispatch is O(1) (pic_fat_deref_table_emit).  The descriptor tables
 * only exist in the final emission pass -- pic_fat_desc_prepare runs after
 * the pass-2 layout trials -- so an earlier pass emits no dispatch; a final
 * pass that could not build them has already logged the error, and leaving
 * the linear staging is then the safest failure (the compile has failed). */
static void pic_fat_deref_emit(pfile_t *pf, value_t val, size_t offset,
    const pic_access_plan_t *plan)
{
  pic_access_key_t           key;
  const pic_access_recipe_t *recipe;

  /* Step 4: the emitter no longer tests the plan field; the key is
   * (kind, operation, facts) and the recipe table answers. */
  assert(value_access_valid_get(val));
  pic_access_key_fill(plan, PIC_ACCESS_OP_DEREF, &key);
  if (pic_env_on("JALV2_ACCESS_RECIPE_FAULT")
      && (PIC_ACCESS_FAT == plan->kind)) {
    key.kind = 0xFFu; /* force a miss for the fault test */
  }
  recipe = pic_access_recipe_find(&key);
  pic_access_explain_note(pf, "deref", value_variable_get(val), plan,
      PIC_ACCESS_OP_DEREF, recipe);
  if (0 == recipe) {
    if (PIC_ACCESS_FAT == plan->kind) {
      pfile_log(pf, PFILE_LOG_ERR,
          "internal error: no access recipe for kind=%u site=deref",
          (unsigned) plan->kind);
    }
    return;
  }
  recipe->run(pf, plan, val, offset);
}

/* FAT deref recipe: the tag dispatch (F2d).  The sequence is the one the
 * guard used to select inline. */
void pic_fat_deref_recipe(pfile_t *pf, const pic_access_plan_t *plan,
    value_t val, size_t offset)
{
  pic_fat_target_t kind;
  variable_t       var;
  unsigned         top;
  unsigned         shift;
  unsigned         mask;
  unsigned         ofs_mask;
  variable_sz_t    psz;
  value_t          pv;
  value_t          baseofs;
  size_t           ofs_total;
  boolean_t        pv_owned;

  var = value_variable_get(val);
  pv       = val;
  pv_owned = BOOLEAN_FALSE;
  baseofs  = value_baseofs_get(val);
  ofs_total = offset;
  if (plan->aop) {
    /* G3: a pointer SLOT element (`pa[k]`) is typed as a scalar, so its
     * value handle is indirect; the tag lives in the SLOT's bytes.  Read the
     * slot as a plain view of the pointer array with the slot byte offset
     * kept (a clone keeps only the low bytes: the value is a 2-/3-byte
     * pointer, not the pointee), so pv[top] is the slot's tag byte.  The
     * slot offset is the ADDRESS here, not a data offset. */
    pv = value_clone(val);
    if (pv) {
      value_flag_clr(pv, VALUE_FLAG_INDIRECT);
      value_flag_clr(pv, VALUE_FLAG_POINTER);
      pv_owned = BOOLEAN_TRUE;
    } else {
      pv = val;
    }
    baseofs   = VALUE_NONE;
    ofs_total = offset;
  } else if ((VALUE_NONE != baseofs) || value_is_indirect(val)) {
    /* A SUBSCRIPTED pointer (`q[37]`) arrives as an indirect value with the
     * subscript as its base offset; the tag lives in the POINTER's high byte,
     * not at q+37 (the dispatch used to read INDF through the data pointer and
     * decoded the data byte as a tag -- 14H measured got=0x00).  Use the base
     * variable directly and fold the base offset into the access offset. */
    if (VALUE_NONE != baseofs) {
      ofs_total = offset + (size_t) value_const_get(baseofs);
    }
    if ((0 != var) && (VARIABLE_NONE != var)) {
      pv = value_alloc(var);
      if (pv) {
        pv_owned = BOOLEAN_TRUE;
      } else {
        pv = val;
      }
    }
  }
  kind = pic_fat_target_kind(pf);
  psz = pic_pointer_size_get(pf);
  top = PIC_FAT_TOP_BYTE(kind);
  shift = PIC_FAT_TAG_SHIFT(kind);
  mask = PIC_FAT_TAG_MAX(kind) << shift;
  ofs_mask = (3 == (unsigned) psz) ? 0xFFU : pic_fat_off_mask(kind);

  if (pic_fat_table_ready()) {
    pic_fat_deref_table_emit(pf, pv, baseofs, ofs_total, top, shift, mask,
        ofs_mask);
  }
  if (pv_owned) {
    value_release(pv);
  }
}

/* WHEREIS of a pointer SLOT at a runtime index reads the slot's stored
 * pointer value.  Under the fat engine that value is tagged (id + object-
 * relative offset), not an address; the const-index form and the engine-off
 * path both report the PHYSICAL address, so the runtime form must resolve
 * the tag through the shared segment walk before the caller copies the
 * bytes.
 *
 * Called with the walk register pointing at the stored pointer; leaves it
 * pointing at the scratch holding the resolved address (top byte zero), so
 * the caller's INDF read copies the address bytes.  Tag 0 is the linear
 * form: its low bytes are already the address.  An unknown tag traps like
 * the deref dispatch does.
 *
 * (The defect this fixes: TODOS `whereis-runtime-slot-fat`.) */
static void pic_fat_whereis_resolve_emit(pfile_t *pf)
{
  variable_t       idv;
  variable_t       arg;
  variable_t       scratch;
  pic_fat_target_t kind;
  variable_sz_t    psz;
  unsigned         top;
  unsigned         shift;
  unsigned         mask;
  unsigned         ofs_mask;
  unsigned         ids;
  unsigned         ii;
  size_t           aofs;
  boolean_t        wide;
  boolean_t        staged;
  value_t          sval;
  value_t          idval;
  value_t          argval;
  value_t          ind;
  value_t          fsrv;
  label_t          linear;
  label_t          trap;
  label_t          end;
  label_t          seg;

  if (!pic_fat_table_ready()) {
    return;
  }
  idv     = pic_fat_id_get();
  arg     = pic_var_chunk_walk_arg_get();
  seg     = pic_fat_seg_addr_label();
  ids     = (unsigned) pic_fat_object_ct();
  if ((VARIABLE_NONE == idv) || (VARIABLE_NONE == arg)
      || (LABEL_NONE == seg) || (0 == ids)) {
    return;
  }
  kind  = pic_fat_target_kind(pf);
  psz   = pic_pointer_size_get(pf);
  scratch = pic_var_whereis_scratch_get(pf);
  if (VARIABLE_NONE == scratch) {
    return;
  }
  top   = PIC_FAT_TOP_BYTE(kind);
  shift = PIC_FAT_TAG_SHIFT(kind);
  mask  = PIC_FAT_TAG_MAX(kind) << shift;
  ofs_mask = (3 == (unsigned) psz) ? 0xFFU : pic_fat_off_mask(kind);
  wide  = (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf))
    ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  aofs  = (size_t) psz;          /* the classic address cell follows the
                                  * resolved bytes in the scratch */

  ind = pic_indirect_get(pf, PFILE_LOG_ERR, 0);
  sval = value_alloc(scratch);
  idval = value_alloc(idv);
  argval = value_alloc(arg);
  fsrv = VALUE_NONE;
  if (!ind || !sval || !idval || !argval) {
    if (ind)    { value_release(ind); }
    if (sval)   { value_release(sval); }
    if (idval)  { value_release(idval); }
    if (argval) { value_release(argval); }
    return;
  }
  linear = pfile_label_alloc(pf, 0);
  trap   = pfile_label_alloc(pf, 0);
  end    = pfile_label_alloc(pf, 0);
  staged = BOOLEAN_TRUE;

  /* --- read the stored pointer bytes ---------------------------------- */
  if (wide) {
    /* offset lo/hi, then the tag byte at `top` */
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ind, 0, PIC_OPDST_W);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, sval, 0);
    for (ii = 1; ii <= top; ii++) {
      pic_instr_append_reg_d(pf, PIC_OPCODE_INCF, "_fsr0l", PIC_OPDST_F);
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
      pic_instr_append_reg_d(pf, PIC_OPCODE_INCF, "_fsr0h", PIC_OPDST_F);
      pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ind, 0, PIC_OPDST_W);
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, sval, ii);
    }
  } else {
    /* classic 14-bit: _fsr is the low byte, IRP bit 8.  The slot address
     * travels through the scratch cell so the second byte can cross the
     * IRP page. */
    fsrv = pic_fsr_get(pf);
    if (!fsrv) {
      staged = BOOLEAN_FALSE;
    } else {
      pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, fsrv, 0, PIC_OPDST_W);
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, sval, aofs);
      pic_instr_append_f(pf, PIC_OPCODE_CLRF, sval, aofs + 1);
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_irp");
      pic_instr_append_f_bn(pf, PIC_OPCODE_BSF, sval, aofs + 1, 0);
      /* low byte from INDF (FSR still points at it) */
      pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ind, 0, PIC_OPDST_W);
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, sval, 0);
      /* address += 1 */
      pic_instr_append_f_d(pf, PIC_OPCODE_INCF, sval, aofs, PIC_OPDST_F);
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
      pic_instr_append_f_d(pf, PIC_OPCODE_INCF, sval, aofs + 1,
          PIC_OPDST_F);
      /* stage FSR = the advanced address, then read the high byte */
      pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, sval, aofs, PIC_OPDST_W);
      pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr");
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_irp");
      pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, sval, aofs + 1, 0);
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status", "_irp");
      pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ind, 0, PIC_OPDST_W);
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, sval, 1);
    }
  }

  if (staged) {
    /* --- tag 0: the linear form; the low bytes are the address --------- */
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, sval, top, PIC_OPDST_W);
    pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, (uchar) mask);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
    pic_instr_append_n(pf, PIC_OPCODE_GOTO, linear);

    /* id = tag >> shift (the deref dispatch's decode) */
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, idval, 0);
    if (shift >= 4) {
      pic_instr_append_f_d(pf, PIC_OPCODE_SWAPF, idval, 0, PIC_OPDST_W);
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, idval, 0);
      shift -= 4;
    }
    for (ii = 0; ii < shift; ii++) {
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_c");
      pic_instr_append_f_d(pf, PIC_OPCODE_RRF, idval, 0, PIC_OPDST_F);
    }
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, idval, 0, PIC_OPDST_W);
    pic_instr_append_w_kn(pf, PIC_OPCODE_SUBLW, (uchar) ids);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_c");
    pic_instr_append_n(pf, PIC_OPCODE_GOTO, trap);

    /* the walk's offset argument: the low bytes, tag bits masked */
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, sval, 0, PIC_OPDST_W);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, argval, 0);
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, sval, 1, PIC_OPDST_W);
    if (0xFFU != ofs_mask) {
      pic_instr_append_w_kn(pf, PIC_OPCODE_ANDLW, (uchar) ofs_mask);
    }
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, argval, 1);
    pic_instr_append_n(pf, PIC_OPCODE_CALL, seg);

    /* the walk left the physical address in FSR0 / _fsr+IRP */
    if (wide) {
      pic_instr_append_reg_d(pf, PIC_OPCODE_MOVF, "_fsr0l", PIC_OPDST_W);
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, sval, 0);
      pic_instr_append_reg_d(pf, PIC_OPCODE_MOVF, "_fsr0h", PIC_OPDST_W);
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, sval, 1);
    } else {
      pic_instr_append_reg_d(pf, PIC_OPCODE_MOVF, "_fsr", PIC_OPDST_W);
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, sval, 0);
      pic_instr_append_f(pf, PIC_OPCODE_CLRF, sval, 1);
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_irp");
      pic_instr_append_f_bn(pf, PIC_OPCODE_BSF, sval, 1, 0);
    }
  }

  pic_instr_append_label(pf, linear);
  if (psz > 2) {
    pic_instr_append_f(pf, PIC_OPCODE_CLRF, sval, 2);
  }
  /* point the walk register AT the resolved bytes (the caller's INDF read
   * copies the scratch, so the value staged here is the scratch ADDRESS,
   * not its contents) */
  {
    variable_base_t sbase;

    sbase = variable_base_get(scratch, 0);
    if (VARIABLE_BASE_UNKNOWN == sbase) {
      /* not placed: leave the raw slot bytes staged (no resolution) */
    } else if (wide) {
      pic_fsr0_load_addr(pf, sbase);
    } else {
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
          (unsigned) (sbase & 0xff));
      pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr");
      pic_instr_append(pf, (sbase >= 256)
          ? PIC_OPCODE_IRP_SET : PIC_OPCODE_IRP_CLR);
    }
  }
  pic_instr_append_n(pf, PIC_OPCODE_GOTO, end);
  pic_instr_append_label(pf, trap);
  pic_instr_append_n(pf, PIC_OPCODE_GOTO, trap);
  pic_instr_append_label(pf, end);

  if (fsrv) {
    value_release(fsrv);
  }
  value_release(sval);
  value_release(idval);
  value_release(argval);
  value_release(ind);
  label_release(linear);
  label_release(trap);
  label_release(end);
}

/* U4-2: the access audit.  Under JALV2_ACCESS_AUDIT every indirect access
 * prints the plan it was classified by, so a pin can assert the planner's
 * decisions on a witness and catch classification drift even when the image
 * happens to be unchanged.  Off by default; the switch is read once, and the
 * OFF values follow the project rule (unset/0/false/no/off are OFF). */
/*
 * NAME
 *   pic_indirect_setup
 *
 * DESCRIPTION
 *   setup irp:fsr
 *
 * PARAMETERS
 *   pf    : file
 *   val   : value
 *   offset: offset from start of value
 *
 * NOTES
 *   1. A value is assumed to not span banks
 *   2. Normally this is called if val.baseofs is non-const
 */
void pic_indirect_setup(pfile_t *pf, value_t val, size_t offset)
{
  value_t           baseofs;
  pic_access_plan_t plan;
  boolean_t         aop;
  boolean_t         slot_pointee;
  unsigned          slot_action;
  unsigned          facts;
  variable_t        bind;

  pic_access_plan_get(pf, val, &plan);
  /* the slot shape is a recipe action; the emitters below read no plan
   * field for it */
  slot_action = pic_access_value_action_get(pf, "indirect:slot", val,
      PIC_ACCESS_OP_INDIRECT_SLOT);
  aop = (boolean_t) (PIC_ACCESS_ACTION_SLOT_NOT != slot_action);
  slot_pointee = (boolean_t) (PIC_ACCESS_ACTION_SLOT_ARRAY == slot_action);
  facts = pic_access_value_facts_get(pf, val, &bind);
  if (pic_access_audit_on()) {
    fprintf(stderr,
        "jal access audit: kind=%u aop=%u chunked=%u vmap=%u rt=%u"
        " bind=%u\n",
        (unsigned) plan.kind, (unsigned) aop,
        (unsigned) ((facts & PIC_ACCESS_FACT_CHUNKED) != 0u), (unsigned) ((facts & PIC_ACCESS_FACT_VALUE_MAP) != 0u),
        (unsigned) ((facts & PIC_ACCESS_FACT_RUNTIME_MAP) != 0u),
        (unsigned) (VARIABLE_NONE != bind));
  }

  pic_stvar_fsr_mark(pf);
  baseofs = value_baseofs_get(val);
  if (value_is_bit(val)) {
    offset += (value_bit_offset_get(val) + 7) / 8;
  }

  if (value_flag_test(val, VALUE_FLAG_PTR_SLOT_RAW)) {
    variable_base_t base;
    value_t         fsr;

    fsr = pic_fsr_get(pf);
    base = value_base_get(val) + offset;
    if (value_is_const(baseofs)) {
      base += (variable_base_t) value_const_get(baseofs);
      base = pic_var_chunk_addr_resolve(value_variable_get(val), base);
      if (pic_is_16bit(pf)) {
        pic_instr_append_lfsr(pf, 0, val, value_const_get(baseofs));
      } else if (pic_is_14bit_hybrid(pf)) {
        pic_fsr0_load_addr(pf, base);
      } else {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base);
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, fsr, 0);
        pic_instr_append(pf, (base >= 256)
           ? PIC_OPCODE_IRP_SET : PIC_OPCODE_IRP_CLR);
      }
    } else {
      if (pic_is_16bit(pf)) {
        pic_instr_append_lfsr(pf, 0, val, 0);
        if (VALUE_NONE != baseofs) {
          pic_fsr0_add_dynamic_index(pf, baseofs, val);
        }
      } else if (pic_is_14bit_hybrid(pf)) {
        pic_fsr0_load_addr(pf, base);
        if (VALUE_NONE != baseofs) {
          pic_fsr0_add_dynamic_index(pf, baseofs, val);
        }
      } else if (VALUE_NONE != baseofs) {
        pic_classic_fsr_load_base_dynamic_index(pf,
            value_variable_get(val), base, baseofs, 0);
      } else {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base);
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, fsr, 0);
        pic_instr_append(pf, (base >= 256)
           ? PIC_OPCODE_IRP_SET : PIC_OPCODE_IRP_CLR);
      }
    }
    /* WHEREIS of a runtime slot reports the slot's stored pointer; under
     * the fat engine that is a tagged (id, offset) value, so resolve it to
     * the PHYSICAL address the const-index form and the engine-off path
     * report.  Leaves the walk register at the resolved bytes. */
    if (pic_fat_pointers_enabled(pf)) {
      pic_fat_whereis_resolve_emit(pf);
    }
    value_release(fsr);
    return;
  }

  if (value_is_indirect(val) || value_flag_test(val, VALUE_FLAG_POINTER)) {
    /* STATUS<IRP>:FSR = *value. Const-pointee indexed reads can retain POINTER
     * while INDIRECT is clear (baseofs is const 0), so value_is_indirect alone
     * would skip this block and emit movf INDF before FSR is loaded. */
    value_t tmp;

    tmp = value_clone(val);
    value_baseofs_set(tmp, VALUE_NONE);
    {
      /* chunk-walked addressing: an open-array formal carrying a
       * propagated chunk map (a direct call specialized to a chunked
       * actual), or an UNESCAPED pointer bind whose target is chunked
       * (the pointer is only ever dereferenced, so every access can
       * walk the target's chunks) -- the flat home base / linear
       * base+index cannot address across the holes */
      variable_t walk_var;

      /* U2b (walk family): the plan already answered "does this value carry
       * a walk map" (holed object, runtime-map formal, or classic string-view
       * formal -- the last walks even when the heap placed contiguously,
       * because its payload base rides a per-formal cell) and resolved the
       * unescaped bind target once; the setup no longer re-derives either. */
      /* a runtime-map formal has no compile-time layout at all, so even a
       * CONSTANT subscript must walk the map the call site handed over */
      if ((VALUE_NONE != baseofs) && value_is_const(baseofs)
          && ((facts & PIC_ACCESS_FACT_RUNTIME_MAP) != 0u)) {
        pic_fsr_chunk_walk_emit_runtime(pf, value_variable_get(val),
            VALUE_NONE, offset + (size_t) value_const_get(baseofs));
        value_release(tmp);
        return;
      }
      if (!((facts & PIC_ACCESS_FACT_VALUE_MAP) != 0u)
          && (!aop || slot_pointee)
          && (VALUE_NONE != baseofs) && value_is_const(baseofs)
          && (VARIABLE_NONE != bind)) {
        /* const subscript through an unescaped walked bind: resolve
         * the linear offset through the TARGET's chunk map and load
         * the absolute address (the home-slot base path below would
         * address linearly across the holes) */
        variable_t wt;
        variable_base_t linear;
        variable_base_t phys;

        /* F5 section 4: a conditionally-bound pointer dereferenced through
         * the COMPILE-TIME walk never reads the pointer's value, so it cannot
         * notice a null bind (it silently read the target's first byte).
         * Guard it exactly like the fat and slot paths.  aop slots keep their
         * own slot handling (the array-pointee slot path is the S3 residual). */
        if (!aop) {
          value_t gtmp;

          /* a plain view of the pointer VARIABLE (tmp still carries the
           * subscript's INDIRECT/POINTER flags, which would make the guard
           * read INDF instead of the slot bytes) */
          gtmp = value_clone(tmp);
          if (gtmp) {
            value_flag_clr(gtmp, VALUE_FLAG_INDIRECT);
            value_flag_clr(gtmp, VALUE_FLAG_POINTER);
            pic_pointer_slot_emit_null_guard(pf, gtmp, 0);
            value_release(gtmp);
          }
        }
        wt = bind;
        linear = variable_base_get(wt, 0)
          + (variable_base_t) value_const_get(baseofs)
          + (variable_base_t) offset;
        phys = pic_var_chunk_addr_resolve(wt, linear);
        if (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf)) {
          pic_fsr0_load_addr(pf, phys);
        } else {
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
              (unsigned) (phys & 0xff));
          pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr");
          pic_instr_append(pf, (phys >= 256)
              ? PIC_OPCODE_IRP_SET : PIC_OPCODE_IRP_CLR);
        }
        value_release(tmp);
        return;
      }
      if ((VALUE_NONE != baseofs) && !value_is_const(baseofs)
          /* G3 S3: an ARRAY-pointee slot's runtime offset is a POINTEE
           * subscript, so it walks like a plain pointer; a scalar-pointee
           * slot's runtime offset is the slot index (the slot staging). */
          && (!aop || slot_pointee)
          && (((facts & PIC_ACCESS_FACT_VALUE_MAP) != 0u) || (VARIABLE_NONE != bind))) {
        /* const baseofs on a map-carrying formal falls through to the
         * const path below, which resolves through the chunk map */
        walk_var = ((facts & PIC_ACCESS_FACT_VALUE_MAP) != 0u)
          ? value_variable_get(val) : bind;
        /* F5 section 4: the runtime walk also ignores the pointer value */
        if (!aop) {
          value_t gtmp;

          gtmp = value_clone(tmp);
          if (gtmp) {
            value_flag_clr(gtmp, VALUE_FLAG_INDIRECT);
            value_flag_clr(gtmp, VALUE_FLAG_POINTER);
            pic_pointer_slot_emit_null_guard(pf, gtmp, 0);
            value_release(gtmp);
          }
        }
        pic_fsr_chunk_walk_emit_ofs(pf, walk_var, baseofs, offset,
            pic_is_16bit(pf) || pic_is_14bit_hybrid(pf));
        value_release(tmp);
        return;
      }
    }

    /* `var pointer … p` then `p[i]` (pointee is array): linear *p+index.
     * True **`ARRAY` of `POINTER`** (nested data pointer): each element is a
     * pointer slot; use the same path as a plain subscripted pointer.
     * Heap `var string` through `var pointer … as var string`: see
     * `pic_value_indirect_heap_string_through_pointer`. */
    if (value_flag_test(val, VALUE_FLAG_ARRAY)
        && !pic_value_indirect_heap_string_through_pointer(val)
        && (!aop
          || pic_value_is_array_pointer_slot_storage(val))) {
      pic_opcode_t op;

      if (value_is_const(baseofs)) {
        offset += value_const_get(baseofs);
      }
      value_baseofs_set(tmp, VALUE_NONE);
      {
        value_t gtmp;

        gtmp = value_clone(tmp);
        value_baseofs_set(gtmp, VALUE_NONE);
        value_flag_clr(gtmp, VALUE_FLAG_POINTER);
        value_flag_clr(gtmp, VALUE_FLAG_INDIRECT);
        if (!pic_value_is_array_pointer_slot_storage(val)) {
          pic_pointer_slot_emit_null_guard(pf, gtmp, 0);
        }
        value_release(gtmp);
      }
      if (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf)) {
        if (pic_is_16bit(pf)) {
          pic_instr_append_lfsr(pf, 0, tmp, offset);
        } else {
          variable_base_t base;

          base = pic_var_chunk_addr_resolve(value_variable_get(tmp),
              value_base_get(tmp) + offset);
          pic_fsr0_load_addr(pf, base);
        }
        if ((VALUE_NONE != baseofs) && !value_is_const(baseofs)) {
          pic_fsr0_add_dynamic_index(pf, baseofs, val);
        }
      } else {
        if ((VALUE_NONE != baseofs) && !value_is_const(baseofs)) {
          if (value_is_bit(baseofs)) {
            pic_op_assign(pf, OPERATOR_ASSIGN, VALUE_NONE, baseofs, 
                VALUE_NONE);
            pic_instr_append_f(pf, PIC_OPCODE_ADDLW, tmp, offset);
            pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr");
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, 1, PIC_OPDST_W);
            pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
            pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, 1);
            pic_classic_irp_set_from_w(pf);
          } else {
            variable_sz_t range;

            range = (variable_sz_t) (value_byte_sz_get(tmp) - offset);
            pic_classic_fsr_load_base_dynamic_index(pf,
                value_variable_get(tmp),
                value_base_get(tmp) + offset, baseofs, range);
          }
        } else {
            pic_instr_append_f(pf, PIC_OPCODE_MOVLW, tmp, offset);
            pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr");
            op = (value_base_get(tmp) >= 256)
              ? PIC_OPCODE_IRP_SET
              : PIC_OPCODE_IRP_CLR;
            pic_instr_append(pf, op);
        }
      }
    } else {
      /* assert(value_is_pointer(val)); */
      /* A CONSTANT subscript through a pointer whose target is split across
       * RAM banks (an open-array formal carrying a chunk map, or an unescaped
       * pointer bind to a chunked array) used to fall through to the linear
       * `deref + offset` sequence below, which cannot see the holes: element
       * 149 of a 150-byte array split 73/77 landed 45 bytes short of its real
       * address.  Walk the map instead (no index register: the offset is a
       * constant, exactly like the runtime-formal const case above). */
      {
        variable_t        walk;
        boolean_t         sv_view;
        pic_access_plan_t wplan;
        pic_access_key_t  wkey;
        unsigned          wfacts;

        /* U3-3: the plan already classified this value (the walk helpers used
         * to re-derive it); the bind target gets its own fill so the test
         * below is the same question on the same variable.  sv_view stays the
         * ORIGINAL variable's answer, exactly as before the rebind. */
        walk = value_variable_get(tmp);
        sv_view = ((facts & PIC_ACCESS_FACT_SV_VIEW) != 0u);
        if (!((facts & PIC_ACCESS_FACT_HOLED) != 0u) && !sv_view) {
          walk = bind;
        }
        pic_access_plan_fill_var(walk, &wplan);
        pic_access_key_fill(&wplan, PIC_ACCESS_OP_INDIRECT_SLOT, &wkey);
        wfacts = wkey.facts;
        if ((VALUE_NONE != baseofs) && value_is_const(baseofs)
            && !aop
            && walk && (VARIABLE_NONE != walk)
            && (((wfacts & PIC_ACCESS_FACT_HOLED) != 0u)
                || ((wfacts & PIC_ACCESS_FACT_RUNTIME_MAP) != 0u)
                || sv_view)) {
          /* the offset/rare entry picks the runtime map walk for a formal
           * that has one and the baked ladder for a compile-time map */
          pic_fsr_chunk_walk_emit_ofs(pf, walk, VALUE_NONE,
              offset + (size_t) value_const_get(baseofs),
              (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf))
                ? BOOLEAN_TRUE : BOOLEAN_FALSE);
          value_release(tmp);
          return;
        }
      }
      value_flag_clr(tmp, VALUE_FLAG_INDIRECT);
      /* Element is typed as the pointee; POINTER stays set and makes
       * value_is_indirect() true (baseofs is often const 0). pic_instr_append_f_d
       * would then treat tmp as an INDF read (movf 0,w) before FSR is loaded.
       * Clear POINTER so MOV reads the pointer slot (see const-pointee loads). */
      value_flag_clr(tmp, VALUE_FLAG_POINTER);

      {
        variable_sz_t loff;
        boolean_t     slot_array;

        /* `p[i]` / `p`+offset: const baseofs is an index into the *pointee*
         * (MOVF p,0 then ADDLW). `slot[i]` on `var pointer … slot[N]`: const
         * baseofs is a byte offset within the pointer *aggregate* (MOVF slot+2). */
        slot_array = aop;
        loff = 0;
        if (slot_array && VALUE_NONE != baseofs && value_is_const(baseofs)) {
          loff = (variable_sz_t) value_const_get(baseofs);
        }
        pic_pointer_slot_emit_null_guard(pf, tmp, loff);
        if ((VALUE_NONE != baseofs) && !value_is_const(baseofs)) {
          if (slot_array) {
            /* ARRAY-of-pointers element at a runtime index: DOUBLE
             * indirection -- FSR = &slot[k], load the slot's pointer
             * bytes, then re-stage FSR (and IRP) from the loaded
             * pointer so the caller's INDF access hits the element's
             * real target. The previous `*slot0 + index` staging used
             * slot 0's VALUE (a bound code address) as the data base
             * (the classic runtime-index read+write defect + the
             * stage-2 call blocker, 2026-10-28). 16-bit core + hybrid
             * variant added 2026-10-28: those cores previously never
             * compiled here (the staging emitted 14-bit _fsr/_irp
             * names) and the generic FSR0 tail re-staged from the
             * ARRAY's own storage -- silently wrong element targets. */
            pic_classic_fsr_load_base_dynamic_index(pf,
                value_variable_get(tmp), value_base_get(tmp),
                baseofs, (variable_sz_t) value_byte_sz_get(tmp));
            pic_pointer_slot_deref_stage(pf);
            /* the caller's INDF access now hits the element's real
             * target; the common FSR/IRP tail below would re-stage
             * from the value's own storage and clobber it */
            value_release(tmp);
            return;
          } else if (variable_is_array(value_variable_get(tmp))) {
            /* plain ARRAY element at a runtime index: the base is the
             * array's own address (MOVLW), not its first element's
             * value (MOVF would read arr[0] as the base) */
            pic_classic_fsr_load_base_dynamic_index(pf,
                value_variable_get(tmp), value_base_get(tmp),
                baseofs, (variable_sz_t) value_byte_sz_get(tmp));
          } else {
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, 0,
                PIC_OPDST_W);
            pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, baseofs, 0,
                PIC_OPDST_W);
          }
        } else if (slot_array
            && pic_mem_object_is_chunked(value_variable_get(val))) {
          /* const-index slot in a CHUNKED table: the linear `MOVF tmp,loff`
           * below reads the wrong chunk's byte.  Stage FSR at the slot's
           * chunk-resolved address and load the pointer through INDF. */
          variable_base_t addr;

          addr = pic_var_chunk_addr_resolve(value_variable_get(tmp),
              value_base_get(tmp) + loff);
          if (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf)) {
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, addr & 0xff);
            pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0l");
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, (addr >> 8) & 0xff);
            pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0h");
          } else {
            pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, addr & 0xff);
            pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr");
            pic_instr_append(pf, (addr >= 256)
                ? PIC_OPCODE_IRP_SET : PIC_OPCODE_IRP_CLR);
          }
          pic_pointer_slot_deref_stage(pf);
          value_release(tmp);
          return;
        } else if (slot_array) {
          /* the chunked arm above only runs once the chunk map exists (the
           * layout pass); make the pointer scratch a known layout variable
           * even in the pass that has no map yet, or its first use is a
           * post-placement MOVLB on an unplaced scratch (P2b-2) */
          value_t layout_scratch;

          layout_scratch = pic_var_pointer_get(pf);
          pic_var_pointer_release(pf, layout_scratch);
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, loff, PIC_OPDST_W);
        } else {
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, 0, PIC_OPDST_W);
          if (baseofs) {
            if (value_is_const(baseofs)) {
              if (value_const_get(baseofs)) {
                pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW,
                    value_const_get(baseofs));
              }
            } else {
              pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, baseofs, 0,
                  PIC_OPDST_W);
            }
          }
        }
      }
      if (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf)) {
        /* Check if this is a function parameter that should use direct addressing */
        variable_t var = value_variable_get(val);
        if (var) {
          const char *name = variable_name_get(var);
          /* Skip FSR0 for function parameters, preserve for actual pointer operations */
          if (name && (strstr(name, "_device_put") ||
                       strstr(name, "_str_") ||
                       strstr(name, "___str"))) {
            return;
          }
        }
        pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0l");
        if ((VALUE_NONE != baseofs) && !value_is_const(baseofs)
            && !aop) {
          variable_sz_t idx_sz;

          idx_sz = pic_dynamic_index_byte_sz_get(baseofs);
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, 1, PIC_OPDST_W);
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
          pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, 1);
          if (idx_sz > 1) {
            pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, baseofs, 1,
                PIC_OPDST_W);
          }
        } else if ((VALUE_NONE != baseofs) && !value_is_const(baseofs)) {
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, 1, PIC_OPDST_W);
        } else if (aop) {
          variable_sz_t loff;

          loff = 0;
          if (VALUE_NONE != baseofs && value_is_const(baseofs)) {
            loff = (variable_sz_t) value_const_get(baseofs);
          }
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, loff + 1, PIC_OPDST_W);
        } else {
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, 1, PIC_OPDST_W);
        }
        pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0h");
      } else {
        variable_sz_t loff;
        variable_sz_t irp_byte;

        pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr");
        loff = 0;
        if (aop
            && VALUE_NONE != baseofs && value_is_const(baseofs)) {
          loff = (variable_sz_t) value_const_get(baseofs);
        }
        if ((VALUE_NONE != baseofs) && !value_is_const(baseofs)
            && !aop) {
          variable_sz_t idx_sz;

          idx_sz = pic_dynamic_index_byte_sz_get(baseofs);
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, 1, PIC_OPDST_W);
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
          pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, 1);
          if (idx_sz > 1) {
            pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, baseofs, 1,
                PIC_OPDST_W);
          }
          pic_classic_irp_set_from_w(pf);
        } else {
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_irp");
          irp_byte = aop
              ? (variable_sz_t) (loff + pic_pointer_size_get(pf) - 1)
              : (variable_sz_t) (pic_pointer_size_get(pf) - 1);
          pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, tmp, irp_byte, 0);
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status", "_irp");
        }
      }
    }
    value_release(tmp);
  } else {
    variable_base_t base;
    value_t         fsr;

    fsr = pic_fsr_get(pf);
    base = value_base_get(val) + offset;
    if (value_is_const(baseofs)) {
      /* fsr = value_base_get(value) + value_const_get(baseofs) + sz */
      base += (variable_base_t) value_const_get(baseofs);
      base = pic_var_chunk_addr_resolve(value_variable_get(val), base);
      if (pic_is_16bit(pf)) {
        pic_instr_append_lfsr(pf, 0, val, value_const_get(baseofs));
      } else if (pic_is_14bit_hybrid(pf)) {
        pic_fsr0_load_addr(pf, base);
      } else {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base);
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, fsr, 0);
        pic_instr_append(pf, (base >= 256)
           ? PIC_OPCODE_IRP_SET : PIC_OPCODE_IRP_CLR);
      }
    } else {
      /* fsr = base (+ sz - 1) */
      if (pic_is_16bit(pf)) {
        pic_instr_append_lfsr(pf, 0, val, 0);
      } else if (pic_is_14bit_hybrid(pf)) {
        pic_fsr0_load_addr(pf, base);
      } else {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base);
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, fsr, 0);
        pic_instr_append(pf, (base >= 256)
           ? PIC_OPCODE_IRP_SET : PIC_OPCODE_IRP_CLR);
      }
      if (VALUE_NONE != baseofs) {
        if (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf)) {
          pic_fsr0_add_dynamic_index(pf, baseofs, val);
        } else {
          pic_classic_fsr_load_base_dynamic_index(pf,
              value_variable_get(val), base, baseofs,
              (variable_sz_t) (value_byte_sz_get(val) - offset));
        }
      }
    }
    value_release(fsr);
  }
  pic_fat_deref_emit(pf, val, offset, &plan);
}

/*
 * given three values, determine which one is indirect. This assumes
 * (obviously) that only one is indirect!
 */
static value_t pic_indirect_value_get(value_t val1, value_t val2, value_t val3)
{
  value_t val;

  if (value_is_indirect(val1)
      || (val1 && value_flag_test(val1, VALUE_FLAG_POINTER))) {
    assert((value_is_same(val1, val2) || !value_is_indirect(val2))
           && (value_is_same(val1, val3) || !value_is_indirect(val3)));
    val = val1;
  } else if (value_is_indirect(val2)) {
    assert(value_is_same(val2, val3) || !value_is_indirect(val3));
    val = val2;
  } else if (value_is_indirect(val3)) {
    val = val3;
  } else {
    val = VALUE_NONE;
  }
  return val;
}

/*
 * given three values (only one of which is indirect) and an offset,
 * setup the irp:ind to the requested offset. This is a no-op
 * if none of the values is indirect
 *   ipos is used to track the relative indirect offset & should
 *        *never* be modified except by pic_indirect_bump3
 */
void pic_indirect_setup3(pfile_t *pf, value_t val1, value_t val2,
  value_t val3, variable_sz_t ofs, variable_sz_t *ipos)
{
  value_t val;

  val = pic_indirect_value_get(val1, val2, val3);
  if (VALUE_NONE != val) {
    variable_sz_t n;

    n = (ofs >= value_sz_get(val))
      ? value_sz_get(val) - 1
      : ofs;
    pic_indirect_setup(pf, val, n);
  }
  *ipos = ofs;
}

/*
 * if any of the three values is indirect, and the position is in range
 * bump the position in the desired direction
 */
void pic_indirect_bump3(pfile_t *pf, value_t val1, value_t val2,
  value_t val3, variable_sz_t ix, variable_sz_t *ipos)
{
  value_t val;

  val = pic_indirect_value_get(val1, val2, val3);
  if (VALUE_NONE != val) {
    /* we can only move one in either direction */
    value_t fsr;
    
    assert((ix == *ipos)
        || (ix + 1 == *ipos) 
        || (ix - 1 == *ipos));

    fsr = pic_fsr_get(pf);
    if ((ix > *ipos) && (ix < value_sz_get(val))) {
      pic_instr_append_f_d(pf, PIC_OPCODE_INCF, fsr, 0, PIC_OPDST_F);
    } else if ((ix < *ipos) && (*ipos < value_sz_get(val))) {
      pic_instr_append_f_d(pf, PIC_OPCODE_DECF, fsr, 0, PIC_OPDST_F);
    }
    value_release(fsr);
    *ipos = ix;
  }
}
