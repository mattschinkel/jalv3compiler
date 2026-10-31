/************************************************************
 **
 ** jal_string.c : dynamic string runtime helpers
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include "../libutils/mem.h"
#include "../libcore/pf_cmd.h"
#include "../libcore/pf_expr.h"
#include "../libcore/pfile.h"
#include "../libcore/pfiled.h"
#include "../libcore/pf_block.h"
#include "../libcore/pf_proc.h"
#include "../libcore/variable.h"
#include "../libcore/value.h"
#include "../libcore/cmd.h"
#include "jal_build.h"
#include "jal_expr.h"
#include "jal_string.h"
#include "jal_tokn.h"

typedef struct jal_str_heap_use_ {
  variable_t       var;
  variable_const_t max_len;
} jal_str_heap_use_t;

/*
 * Per-compile state, in ONE object (Tier-A A4; see
 * fix_jal_string_state_reset.md). The heap-use table was grown by the
 * string heap planner and never freed by anything.
 */
typedef struct jal_string_state_ {
  unsigned            silent_truncate_nest;
  boolean_t           silent_had_truncate;
  jal_str_heap_use_t *heap_use;
  size_t              heap_use_used;
  size_t              heap_use_alloc;
  boolean_t           heap_use_runtime;
  unsigned            dec_no_shared;
} jal_string_state_t;

static jal_string_state_t jal_string_state;

/* TEARDOWN between compiles: frees the heap-use table and zeroes. */
void jal_string_state_reset(void)
{
  free(jal_string_state.heap_use);
  memset(&jal_string_state, 0, sizeof(jal_string_state));
}


#define JAL_STRING_HEAP_NAME      "_string_heap"
#define JAL_STRING_FREE_HEAD_NAME "_string_free_head"
#define JAL_STRING_ERROR_NAME     "_string_error"

#define JAL_STRING_ERROR_NONE      0
#define JAL_STRING_ERROR_HEAP_FULL 1
#define JAL_STRING_ERROR_OVERFLOW  2
#define JAL_STRING_ERROR_INVALID   3

#define JAL_STRING_INDEX_AUTO  0
#define JAL_STRING_INDEX_BYTE  1
#define JAL_STRING_INDEX_WORD  2

/*
 * p-file assigns can warn when the compiler lowers wide max/index temps into
 * byte handle/heap fields; those truncations are intentional. Suppress
 * "assignment to smaller type" only while emitting string-heap p-code.
 */

static void jal_string_silent_truncate_begin(pfile_t *pf)
{
  if (0U == jal_string_state.silent_truncate_nest) {
    jal_string_state.silent_had_truncate
        = pfile_flag_test(pf, PFILE_FLAG_WARN_TRUNCATE);
    pfile_flag_clr(pf, PFILE_FLAG_WARN_TRUNCATE);
  }
  jal_string_state.silent_truncate_nest++;
}

static void jal_string_silent_truncate_end(pfile_t *pf)
{
  if (0U == jal_string_state.silent_truncate_nest) {
    return;
  }
  jal_string_state.silent_truncate_nest--;
  if (0U == jal_string_state.silent_truncate_nest
      && jal_string_state.silent_had_truncate) {
    pfile_flag_set(pf, PFILE_FLAG_WARN_TRUNCATE);
  }
}

#define JAL_STRING_FIELD_OFFSET 0
#define JAL_STRING_FIELD_LENGTH 1
#define JAL_STRING_FIELD_CAP    2

/* `string_heap_compact` work: one row per registered dynamic `var string`
 * (see `pfile_dynstr_reg_ct_get`), 4 fields per row (old_off, bsz, new_off,
 * reg_ix). Each field is stored as a little-endian u16 (2 bytes) so prefix
 * sums and offsets are not truncated when `pragma string index byte` is used
 * for the real heap. Buffer size is 8 * reg_ct bytes (flat byte array). */

static variable_sz_t jal_string_current_index_size_get(pfile_t *pf);
static void jal_string_heap_compact_fill_bld(pfile_t *pf, value_t res);
static value_t jal_string_heap_cpt_u16_temp_get(pfile_t *pf);

static variable_def_t jal_string_byte_def_alloc(flag_t flags)
{
  return variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER, flags, 1);
}

static variable_def_t jal_string_index_def_alloc(pfile_t *pf, flag_t flags)
{
  return variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER, flags,
      jal_string_current_index_size_get(pf));
}

static variable_const_t jal_string_index_max(variable_sz_t index_sz)
{
  switch (index_sz) {
    case JAL_STRING_INDEX_BYTE:
      return 255;
    case JAL_STRING_INDEX_WORD:
      return 65535;
    default:
      return 0;
  }
}

/* ---- heap sizing recommendation (Option A diagnostic) --------------
 * Per heap HANDLE (user `var string` plus the retained `_string_chainN`
 * evaluation cells), the largest compile-time-constant payload length
 * ever requested.  At end of parse the sum of payload + headers x1.25
 * is the smallest heap that can hold every literal-driven allocation
 * simultaneously with fragmentation headroom; a user heap below that
 * warns.  Any RUNTIME-length allocation (string_from_array, concat
 * with runtime operands, rendered numbers) makes the aggregate
 * uncomputable and skips the diagnostic entirely (agreed Option A). */
static variable_sz_t jal_string_index_size_get(pfile_t *pf,
    variable_const_t heap_sz);
static value_t jal_string_const_get(pfile_t *pf, variable_const_t n);



static void jal_string_heap_use_note(value_t dst, value_t len)
{
  variable_t var;
  size_t     ii;

  var = value_variable_get(dst);
  if (!var) {
    return;
  }
  if (!value_is_const(len)) {
    jal_string_state.heap_use_runtime = BOOLEAN_TRUE;
    return;
  }
  for (ii = 0; ii < jal_string_state.heap_use_used; ii++) {
    if (jal_string_state.heap_use[ii].var == var) {
      if (value_const_get(len) > jal_string_state.heap_use[ii].max_len) {
        jal_string_state.heap_use[ii].max_len = value_const_get(len);
      }
      return;
    }
  }
  if (jal_string_state.heap_use_used == jal_string_state.heap_use_alloc) {
    size_t              resize;
    jal_str_heap_use_t *tmp;

    resize = jal_string_state.heap_use_alloc ? 2 * jal_string_state.heap_use_alloc : 16;
    tmp    = REALLOC(jal_string_state.heap_use, resize * sizeof(*tmp));
    if (!tmp) {
      return; /* diagnostic only: skip silently on OOM */
    }
    jal_string_state.heap_use       = tmp;
    jal_string_state.heap_use_alloc = resize;
  }
  jal_string_state.heap_use[jal_string_state.heap_use_used].var     = var;
  jal_string_state.heap_use[jal_string_state.heap_use_used].max_len = value_const_get(len);
  jal_string_state.heap_use_used++;
}

/* `dst = <dynamic src>` with a RUNTIME length: when src's own static
 * max is known (literal- or const-chain-driven), the copy gives dst
 * the same static bound -- only an src with no static bound makes the
 * aggregate uncomputable (the report's skip rule). */
static void jal_string_heap_use_copy_note(pfile_t *pf, value_t dst,
    value_t src)
{
  variable_t dvar;
  variable_t svar;
  size_t     ii;

  dvar = value_variable_get(dst);
  svar = value_variable_get(src);
  if (!dvar || !svar || (dvar == svar)) {
    return;
  }
  for (ii = 0; ii < jal_string_state.heap_use_used; ii++) {
    if (jal_string_state.heap_use[ii].var == svar) {
      value_t cv;

      cv = jal_string_const_get(pf, jal_string_state.heap_use[ii].max_len);
      jal_string_heap_use_note(dst, cv);
      value_release(cv);
      return;
    }
  }
  jal_string_state.heap_use_runtime = BOOLEAN_TRUE;
}

void jal_string_heap_sizing_report(pfile_t *pf)
{
  value_t          heap_val;
  variable_const_t heap_sz;
  variable_const_t payload;
  unsigned long    header;
  unsigned long    total;
  unsigned long    recommended;
  variable_sz_t    index_sz;
  size_t           ii;

  if (!jal_string_state.heap_use_used || jal_string_state.heap_use_runtime) {
    return;
  }
  if (pfile_errct_get(pf)) {
    return; /* error runs stay clean (heap-fit errors already spoke) */
  }
  heap_val = pfile_value_find(pf, PFILE_LOG_NONE, "_string_heap_size");
  if (!heap_val) {
    return; /* no heap declared: jal_string_dynamic_def_alloc errored */
  }
  heap_sz = value_const_get(heap_val);
  value_release(heap_val);
  if (!heap_sz) {
    return;
  }

  payload = 0;
  for (ii = 0; ii < jal_string_state.heap_use_used; ii++) {
    payload += jal_string_state.heap_use[ii].max_len;
  }

  /* header from the chosen heap's index width; a recommendation that
   * crosses an index boundary widens the header and recomputes once
   * (a forced `pragma string index` pins the width -- no iteration) */
  index_sz = jal_string_index_size_get(pf, heap_sz);
  if (!index_sz) {
    return;
  }
  header = 3UL * (unsigned long) index_sz;
  total  = (unsigned long) payload
      + header * (unsigned long) jal_string_state.heap_use_used;
  recommended = (total * 5UL + 3UL) / 4UL;
  {
    variable_sz_t wide;

    wide = jal_string_index_size_get(pf, (variable_const_t) recommended);
    if (wide && (wide > index_sz)) {
      header = 3UL * (unsigned long) wide;
      total  = (unsigned long) payload
          + header * (unsigned long) jal_string_state.heap_use_used;
      recommended = (total * 5UL + 3UL) / 4UL;
    }
  }

  if ((variable_const_t) recommended > heap_sz) {
    pfile_log(pf, PFILE_LOG_WARN,
        "_string_heap_size is %lu but the literals assigned recommend "
        "at least %lu (%lu payload + %lu-byte headers across %lu "
        "string%s, 25%% headroom) -- runtime assigns will exhaust the "
        "heap and set _string_error",
        (unsigned long) heap_sz,
        recommended,
        (unsigned long) payload,
        header,
        (unsigned long) jal_string_state.heap_use_used,
        (1 == jal_string_state.heap_use_used) ? "" : "s");
  }
}

static value_t jal_string_pragma_const_get(pfile_t *pf, const char *name)
{
  value_t val;

  val = pfile_value_find(pf, PFILE_LOG_NONE, name);
  if (val && !value_is_const(val)) {
    pfile_log(pf, PFILE_LOG_ERR, "%s must be constant", name);
    value_release(val);
    val = VALUE_NONE;
  }
  return val;
}

static variable_sz_t jal_string_index_size_get(pfile_t *pf,
    variable_const_t heap_sz)
{
  value_t index_val;
  variable_const_t requested;
  variable_sz_t index_sz;

  requested = JAL_STRING_INDEX_AUTO;
  index_val = jal_string_pragma_const_get(pf, "_string_index_size");
  if (index_val) {
    requested = value_const_get(index_val);
    value_release(index_val);
  }
  if (JAL_STRING_INDEX_AUTO == requested) {
    if (heap_sz <= jal_string_index_max(JAL_STRING_INDEX_BYTE)) {
      index_sz = JAL_STRING_INDEX_BYTE;
    } else if (heap_sz <= jal_string_index_max(JAL_STRING_INDEX_WORD)) {
      index_sz = JAL_STRING_INDEX_WORD;
    } else {
      pfile_log(pf, PFILE_LOG_ERR,
          "dynamic string heap is currently limited to 65535 bytes");
      index_sz = JAL_STRING_INDEX_AUTO;
    }
  } else if ((JAL_STRING_INDEX_BYTE == requested)
      || (JAL_STRING_INDEX_WORD == requested)) {
    index_sz = (variable_sz_t) requested;
  } else {
    pfile_log(pf, PFILE_LOG_ERR,
        "pragma string index must be auto, byte, or word");
    index_sz = JAL_STRING_INDEX_AUTO;
  }
  if (index_sz && (heap_sz > jal_string_index_max(index_sz))) {
    pfile_log(pf, PFILE_LOG_ERR,
        "_string_heap_size does not fit pragma string index");
    index_sz = JAL_STRING_INDEX_AUTO;
  }
  return index_sz;
}

static variable_sz_t jal_string_current_index_size_get(pfile_t *pf)
{
  value_t heap_sz_val;
  variable_const_t heap_sz;

  heap_sz = 0;
  heap_sz_val = jal_string_pragma_const_get(pf, "_string_heap_size");
  if (heap_sz_val) {
    heap_sz = value_const_get(heap_sz_val);
    value_release(heap_sz_val);
  }
  return jal_string_index_size_get(pf, heap_sz);
}

static variable_sz_t jal_string_header_size_get(pfile_t *pf)
{
  return (variable_sz_t) (3 * jal_string_current_index_size_get(pf));
}

/* Bare `const string S = "..."`: a compile-time flash string, not a heap
 * handle. Open byte-count ARRAY carrying the STRING flag (no DYNAMIC);
 * the initializer's array-init path sizes it from the literal exactly
 * like `const byte x[] = "..."`, and every const-string consumer
 * (COUNT fold, [i], ==, chains, const-table marshalling) treats it like
 * the already-working sized form `const string[N]`. */
variable_def_t jal_string_const_def_alloc(void)
{
  variable_def_t byte_def;
  variable_def_t arr_def;

  byte_def = jal_string_byte_def_alloc(VARIABLE_DEF_FLAG_NONE);
  arr_def = variable_def_alloc(0, VARIABLE_DEF_TYPE_ARRAY,
      VARIABLE_DEF_FLAG_STRING, 0);
  if (!byte_def || !arr_def
      || (RESULT_OK != variable_def_member_add(arr_def, 0, byte_def,
          VARIABLE_CT_UNKNOWN))) {
    if (arr_def) {
      variable_def_free(arr_def);
    }
    arr_def = VARIABLE_DEF_NONE;
  }
  return arr_def;
}

variable_def_t jal_string_dynamic_def_alloc(pfile_t *pf)
{
  variable_def_t byte_def;
  variable_def_t arr_def;
  variable_sz_t index_sz;

  index_sz = jal_string_current_index_size_get(pf);
  if (!index_sz) {
    return VARIABLE_DEF_NONE;
  }
  byte_def = jal_string_byte_def_alloc(VARIABLE_DEF_FLAG_NONE);
  arr_def = variable_def_alloc(0, VARIABLE_DEF_TYPE_ARRAY,
      VARIABLE_DEF_FLAG_STRING | VARIABLE_DEF_FLAG_STRING_DYNAMIC, 0);
  if (!byte_def || !arr_def
      || (RESULT_OK != variable_def_member_add(arr_def, 0, byte_def,
          (variable_ct_t) (3 * index_sz)))) {
    if (arr_def) {
      variable_def_free(arr_def);
    }
    arr_def = VARIABLE_DEF_NONE;
  }
  return arr_def;
}

boolean_t jal_string_pointer_dynamic_dst_p(value_t val)
{
  variable_def_t        def;
  variable_def_member_t mbr;
  variable_def_t        pt;

  if (!val || !value_is_pointer(val)) {
    return BOOLEAN_FALSE;
  }
  def = value_def_get(val);
  mbr = def ? variable_def_member_get(def) : 0;
  pt = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
  return pt && variable_def_flag_test(pt,
      VARIABLE_DEF_FLAG_STRING | VARIABLE_DEF_FLAG_STRING_DYNAMIC);
}

/* Mutates `dst` in place: pointer slot -> indirect heap-string aggregate (same
 * model as data-pointer RVALUE deref). No-op if `dst` is not pointer-to-heap
 * string. */
static void jal_string_dynamic_dst_through_pointer_maybe(value_t dst)
{
  if (jal_string_pointer_dynamic_dst_p(dst)) {
    value_dereference(dst);
  }
}

static value_t jal_string_to_array_emit(pfile_t *pf, value_t dst,
    value_t src, value_t max, boolean_t truncate);

boolean_t jal_string_is_dynamic(value_t val)
{
  return val && value_dflag_test(val,
      VARIABLE_DEF_FLAG_STRING | VARIABLE_DEF_FLAG_STRING_DYNAMIC);
}

static value_t jal_string_const_get(pfile_t *pf, variable_const_t n)
{
  return pfile_constant_get(pf, n, VARIABLE_DEF_NONE);
}

static value_t jal_string_index_const_get(pfile_t *pf, variable_const_t n)
{
  value_t val;
  variable_def_t def;

  def = jal_string_index_def_alloc(pf, VARIABLE_DEF_FLAG_CONST);
  val = pfile_constant_get(pf, n, def);
  return val;
}

static value_t jal_string_temp_index_get(pfile_t *pf)
{
  return pfile_value_temp_get(pf, VARIABLE_DEF_TYPE_INTEGER,
      jal_string_current_index_size_get(pf));
}

/* Work temps that participate in `max` comparisons/assigns must be at least as
 * wide as the *max* operand: word actuals to byte|word in max_len must not be
 * silently narrowed into index-sized byte temps (pf_expr user-facing warning). */
static variable_sz_t jal_string_work_index_sz_for_max(pfile_t *pf, value_t max)
{
  variable_def_t  mdef;
  variable_sz_t   idx_sz;
  variable_sz_t   mwide;

  idx_sz = jal_string_current_index_size_get(pf);
  if (!max) {
    return idx_sz;
  }
  mdef = value_def_get(max);
  if (!mdef) {
    return idx_sz;
  }
  mwide = variable_def_sz_get(mdef);
  if (mwide > idx_sz) {
    return mwide;
  }
  return idx_sz;
}

static value_t jal_string_temp_index_get_for_max(pfile_t *pf, value_t max)
{
  variable_sz_t wsz;

  wsz = jal_string_work_index_sz_for_max(pf, max);
  if (!wsz) {
    wsz = 1;
  }
  return pfile_value_temp_get(pf, VARIABLE_DEF_TYPE_INTEGER, wsz);
}

/* When pragma string index is byte, a wide *integer* constant in max (e.g.
 * word 5) would otherwise mix with byte index temps and the length field. */
static value_t jal_string_max_index_coerce(pfile_t *pf, value_t max)
{
  variable_def_t  mdef;
  variable_def_type_t mty;
  variable_sz_t  msz;
  variable_def_t  bdef;
  value_t         b;
  variable_const_t n;

  if (!max || !value_is_const(max)) {
    return max;
  }
  mdef = value_def_get(max);
  if (!mdef) {
    return max;
  }
  mty = variable_def_type_get(mdef);
  if (mty != VARIABLE_DEF_TYPE_INTEGER && mty != VARIABLE_DEF_TYPE_BOOLEAN) {
    return max;
  }
  msz = variable_def_sz_get(mdef);
  if (msz < 2U) {
    return max;
  }
  if (JAL_STRING_INDEX_BYTE != jal_string_current_index_size_get(pf)) {
    return max;
  }
  n = value_const_get(max);
  if (n > 255U) {
    return max;
  }
  bdef = jal_string_byte_def_alloc(VARIABLE_DEF_FLAG_NONE);
  if (!bdef) {
    return max;
  }
  b = pfile_constant_get(pf, n, bdef);
  if (!b) {
    return max;
  }
  value_release(max);
  return b;
}

static void jal_string_assign_value(pfile_t *pf, value_t dst, value_t src)
{
  pfile_cmd_op_add(pf, OPERATOR_ASSIGN, &dst, src, VALUE_NONE);
}

/*
 * A user rvalue read of _string_error snapshots the value into a temp, then
 * clears the slot to STRING_ERROR_NONE. Compiler-internal p-code in this file
 * that assigns _string_error does not go through jal_parse_value, so it is
 * not affected.
 */
void jal_string_error_rvalue_consumed(pfile_t *pf, int is_rvalue, value_t *pval)
{
  const char       *nm;
  value_t           v;
  value_t           lval;
  value_t           z;
  value_t           temp;
  variable_def_t    vdef;
  variable_t        vvar;
  variable_t        lvar;

  v = pval ? *pval : VALUE_NONE;
  if (!v || !is_rvalue) {
    return;
  }
  nm = value_name_get(v);
  if (!nm || (0 != strcmp(nm, JAL_STRING_ERROR_NAME))
      || value_baseofs_get(v)) {
    return;
  }
  lval = pfile_value_find(pf, PFILE_LOG_NONE, JAL_STRING_ERROR_NAME);
  if (!lval) {
    return;
  }
  vvar = value_variable_get(v);
  lvar = value_variable_get(lval);
  if (vvar && lvar && (vvar != lvar)) {
    value_release(lval);
    return;
  }
  vdef = value_def_get(v);
  if (!vdef) {
    value_release(lval);
    return;
  }
  temp = pfile_value_temp_get_from_def(pf, vdef);
  pfile_cmd_op_add_assign(pf, OPERATOR_ASSIGN, &temp, v);
  value_release(v);
  z = pfile_constant_get(pf, JAL_STRING_ERROR_NONE, value_def_get(lval));
  pfile_cmd_op_add_assign(pf, OPERATOR_ASSIGN, &lval, z);
  value_release(z);
  value_release(lval);
  value_lock(temp);
  *pval = temp;
}

static value_t jal_string_binary_emit(pfile_t *pf, operator_t op,
    value_t a, value_t b)
{
  value_t dst;

  dst = VALUE_NONE;
  pfile_cmd_op_add(pf, op, &dst, a, b);
  return dst;
}

static value_t jal_string_add_const(pfile_t *pf, value_t val,
    variable_const_t n)
{
  value_t c;
  value_t r;

  c = jal_string_const_get(pf, n);
  r = jal_string_binary_emit(pf, OPERATOR_ADD, val, c);
  value_release(c);
  return r;
}

static value_t jal_string_array_elem_get(value_t arr, value_t ofs)
{
  value_t elem;

  elem = value_clone(arr);
  if (elem) {
    value_baseofs_set(elem, ofs);
    value_dereference(elem);
  }
  return elem;
}

static value_t jal_string_named_value_get(pfile_t *pf, const char *name)
{
  return pfile_value_find(pf, PFILE_LOG_ERR, name);
}

static value_t jal_string_heap_elem_get(pfile_t *pf, value_t ofs)
{
  value_t heap;
  value_t elem;

  heap = jal_string_named_value_get(pf, JAL_STRING_HEAP_NAME);
  elem = heap ? jal_string_array_elem_get(heap, ofs) : VALUE_NONE;
  value_release(heap);
  return elem;
}

static value_t jal_string_index_byte_lvalue_get(pfile_t *pf, value_t base,
    value_t ofs)
{
  UNUSED(pf);
  return jal_string_array_elem_get(base, ofs);
}

static value_t jal_string_index_load_emit(pfile_t *pf, value_t base,
    value_t ofs)
{
  variable_sz_t index_sz;
  value_t result;
  value_t zero;
  variable_sz_t ii;

  index_sz = jal_string_current_index_size_get(pf);
  result = jal_string_temp_index_get(pf);
  zero = jal_string_const_get(pf, 0);
  jal_string_assign_value(pf, result, zero);
  value_release(zero);
  for (ii = 0; ii < index_sz; ii++) {
    value_t byte_ofs;
    value_t byte_lval;
    value_t byte_val;

    byte_ofs = ii ? jal_string_add_const(pf, ofs, ii) : value_clone(ofs);
    byte_lval = jal_string_index_byte_lvalue_get(pf, base, byte_ofs);
    byte_val = byte_lval;
    if (ii) {
      value_t shift;
      value_t shifted;

      shift = jal_string_const_get(pf, 8 * ii);
      shifted = jal_string_binary_emit(pf, OPERATOR_SHIFT_LEFT,
          byte_lval, shift);
      value_release(shift);
      byte_val = shifted;
    }
    pfile_cmd_op_add(pf, OPERATOR_ADD, &result, result, byte_val);
    if (ii) {
      value_release(byte_val);
    }
    value_release(byte_lval);
    value_release(byte_ofs);
  }
  return result;
}

static void jal_string_index_store_emit(pfile_t *pf, value_t base,
    value_t ofs, value_t val)
{
  variable_sz_t index_sz;
  variable_sz_t ii;

  index_sz = jal_string_current_index_size_get(pf);
  for (ii = 0; ii < index_sz; ii++) {
    value_t byte_ofs;
    value_t byte_lval;
    value_t byte_val;

    byte_ofs = ii ? jal_string_add_const(pf, ofs, ii) : value_clone(ofs);
    byte_lval = jal_string_index_byte_lvalue_get(pf, base, byte_ofs);
    if (ii) {
      value_t shift;

      shift = jal_string_const_get(pf, 8 * ii);
      byte_val = jal_string_binary_emit(pf, OPERATOR_SHIFT_RIGHT,
          val, shift);
      value_release(shift);
    } else {
      byte_val = value_clone(val);
    }
    value_def_set(byte_val, jal_string_byte_def_alloc(VARIABLE_DEF_FLAG_NONE));
    jal_string_assign_value(pf, byte_lval, byte_val);
    value_release(byte_val);
    value_release(byte_lval);
    value_release(byte_ofs);
  }
}

static value_t jal_string_heap_value_get(pfile_t *pf)
{
  return jal_string_named_value_get(pf, JAL_STRING_HEAP_NAME);
}

static value_t jal_string_heap_index_get(pfile_t *pf, value_t ofs)
{
  value_t heap;
  value_t val;

  heap = jal_string_heap_value_get(pf);
  val = heap ? jal_string_index_load_emit(pf, heap, ofs) : VALUE_NONE;
  value_release(heap);
  return val;
}

static void jal_string_heap_index_set(pfile_t *pf, value_t ofs, value_t val)
{
  value_t heap;

  heap = jal_string_heap_value_get(pf);
  if (heap) {
    jal_string_index_store_emit(pf, heap, ofs, val);
  }
  value_release(heap);
}

static value_t jal_string_handle_field_ofs_get(pfile_t *pf,
    variable_const_t field)
{
  return jal_string_const_get(pf,
      field * jal_string_current_index_size_get(pf));
}

static value_t jal_string_handle_index_get(pfile_t *pf, value_t str,
    variable_const_t field)
{
  value_t ofs;
  value_t val;

  ofs = jal_string_handle_field_ofs_get(pf, field);
  val = jal_string_index_load_emit(pf, str, ofs);
  value_release(ofs);
  return val;
}

static void jal_string_handle_index_set(pfile_t *pf, value_t str,
    variable_const_t field, value_t val)
{
  value_t ofs;

  ofs = jal_string_handle_field_ofs_get(pf, field);
  jal_string_index_store_emit(pf, str, ofs, val);
  value_release(ofs);
}

/* Zero every handle field of a freshly declared dynamic string. Used by
 * the declaration-initializer lowering only: a power-on garbage
 * CAP >= len would make the runtime assign skip allocation and copy
 * through a garbage block pointer (classic cores do not zero RAM).
 * Statement assigns must NOT do this -- they rely on CAP to decide
 * in-place reuse of the already-allocated block. */
void jal_string_handle_zero_emit(pfile_t *pf, value_t str)
{
  value_t zero;

  zero = jal_string_index_const_get(pf, 0);
  if (zero) {
    jal_string_handle_index_set(pf, str, JAL_STRING_FIELD_OFFSET, zero);
    jal_string_handle_index_set(pf, str, JAL_STRING_FIELD_LENGTH, zero);
    jal_string_handle_index_set(pf, str, JAL_STRING_FIELD_CAP, zero);
    value_release(zero);
  }
}

boolean_t jal_string_is_source(value_t val)
{
  return jal_string_is_dynamic(val)
      || (val && value_is_array(val) && value_is_string(val));
}

/* Stage a const/fixed string SOURCE into a compiler string cell (the
 * `_string_chainN` global machinery) -- jal_parse_call uses this to
 * marshal literal actuals into DYNAMIC-string formals (a raw literal's
 * plain array def never matches the formal's heap-handle def). Empty
 * sources take the assign's len-0 path: COUNT sees 0. Returns the
 * assigned cell value or VALUE_NONE. */
static value_t jal_string_chain_var_get(pfile_t *pf);

value_t jal_string_stage_source_cell(pfile_t *pf, value_t src)
{
  value_t cell = jal_string_chain_var_get(pf);

  if (cell) {
    if (jal_string_assignment_emit(pf, cell, src)) {
      return cell;
    }
    value_release(cell);
  }
  return VALUE_NONE;
}

/* Stage a CONST string source into a compiler `var string[cap]` cell
 * for a FIXED-string formal (`string[N] in s`) -- the literal's
 * char-count def never matches the formal's capacity def (only the
 * exact count==N coincidence ever compiled). The fixed assign
 * zero-pads; a literal longer than the capacity is a clean compile
 * error (the runtime copy could never hold it). Returns the cell
 * value or VALUE_NONE. */
static void jal_string_fixed_assignment_emit(pfile_t *pf, value_t dst,
    value_t src);

value_t jal_string_stage_fixed_cell(pfile_t *pf, value_t src,
    variable_sz_t cap)
{
  static unsigned seq;
  char            name[32];
  variable_def_t  bdef;
  variable_def_t  adef;
  variable_t      var;
  value_t         val;

  if (!cap || (value_ct_get(src) > (variable_const_t) cap)) {
    pfile_log(pf, PFILE_LOG_ERR,
        "string literal is %lu characters but the fixed-string"
        " parameter's capacity is %u",
        (unsigned long) value_ct_get(src), (unsigned) cap);
    return VALUE_NONE;
  }
  bdef = jal_string_byte_def_alloc(VARIABLE_DEF_FLAG_NONE);
  adef = variable_def_alloc(0, VARIABLE_DEF_TYPE_ARRAY,
      VARIABLE_DEF_FLAG_STRING, 0);
  if (!bdef || !adef
      || (RESULT_OK != variable_def_member_add(adef, 0, bdef, cap))) {
    if (adef) {
      variable_def_free(adef);
    } else if (bdef) {
      variable_def_free(bdef);
    }
    return VALUE_NONE;
  }
  sprintf(name, "_string_fixarg%u", seq++);
  if (RESULT_OK != pfile_variable_alloc(pf, PFILE_VARIABLE_ALLOC_GLOBAL,
      name, adef, VARIABLE_NONE, &var)) {
    variable_def_free(adef);
    return VALUE_NONE;
  }
  val = value_alloc(var);
  variable_release(var);
  if (val) {
    jal_string_fixed_assignment_emit(pf, val, src);
  }
  return val;
}

/*
 * Fixed-capacity string `var string[N]`: a plain byte array whose outer def
 * carries the STRING flag (no heap handle). Assignment and `&` concatenation
 * on these copy effective text bytes into the destination and zero-fill the
 * remainder; a total longer than N leaves the destination unchanged and
 * sets `_string_error` (STRING_ERROR_OVERFLOW).
 */
boolean_t jal_string_is_fixed(value_t val)
{
  return val && value_is_array(val) && value_is_string(val)
      && !jal_string_is_dynamic(val);
}

static boolean_t jal_string_global_byte_alloc(pfile_t *pf, const char *name,
  flag_t flags, variable_const_t init);
static boolean_t jal_string_global_const_alloc(pfile_t *pf, const char *name,
  variable_const_t init);

static boolean_t jal_string_fixed_runtime_ensure(pfile_t *pf)
{
  variable_t err;

  err = pfile_variable_find(pf, PFILE_LOG_NONE, JAL_STRING_ERROR_NAME, 0);
  if (err) {
    variable_release(err);
    return BOOLEAN_TRUE;
  }
  /* fixed strings need no heap: only the shared error byte and the
   * symbolic error constants */
  return jal_string_global_byte_alloc(pf, JAL_STRING_ERROR_NAME,
           VARIABLE_DEF_FLAG_NONE, JAL_STRING_ERROR_NONE)
      && jal_string_global_const_alloc(pf, "string_error_none",
           JAL_STRING_ERROR_NONE)
      && jal_string_global_const_alloc(pf, "string_error_heap_full",
           JAL_STRING_ERROR_HEAP_FULL)
      && jal_string_global_const_alloc(pf, "string_error_overflow",
           JAL_STRING_ERROR_OVERFLOW)
      && jal_string_global_const_alloc(pf, "string_error_invalid",
           JAL_STRING_ERROR_INVALID);
}

static value_t jal_string_parse_operand(pfile_t *pf);
static boolean_t jal_string_is_numeric_value(value_t val);
static value_t jal_string_chain_item_resolve(pfile_t *pf, value_t item,
    value_t *len);
static value_t jal_string_number_source_emit(pfile_t *pf, value_t v,
    value_t *len_out);
value_t jal_string_concat_chain_emit(pfile_t *pf, value_t first);

value_t jal_string_length_value_get(pfile_t *pf, value_t val)
{
  UNUSED(pf);
  if (!jal_string_is_dynamic(val)) {
    return VALUE_NONE;
  }
  return jal_string_handle_index_get(pf, val, JAL_STRING_FIELD_LENGTH);
}

value_t jal_string_capacity_value_get(pfile_t *pf, value_t val)
{
  UNUSED(pf);
  if (!jal_string_is_dynamic(val)) {
    return VALUE_NONE;
  }
  return jal_string_handle_index_get(pf, val, JAL_STRING_FIELD_CAP);
}

/* String actual -> open byte-array formal coercion (call arguments).
 * Allocates a temp byte buffer (capacity bounded by the string heap,
 * or 255 when no heap const is visible), copies the string content in
 * with the string_to_array machinery, and returns the buffer value.
 * The caller passes the buffer to the formal; pair with
 * jal_string_length_value_get for the hidden count parameter (the
 * buffer capacity would over-report). Returns VALUE_NONE when val is
 * not a string. */
value_t jal_string_arg_byte_array(pfile_t *pf, value_t src)
{
  variable_def_t bdef;
  variable_def_t adef;
  value_t        buf;
  value_t        cap_val;
  value_t        res;

  /* only heap strings coerce; const literals already work as arrays */
  if (!jal_string_is_dynamic(src)) {
    return VALUE_NONE;
  }
  cap_val = pfile_value_find(pf, PFILE_LOG_NONE, "_string_heap_size");
  if (!cap_val) {
    cap_val = pfile_constant_get(pf, 255, VARIABLE_DEF_NONE);
  }
  /* byte[cvalue] def: build array-of-byte with a constant count from
   * the cap value (heap const or the 255 fallback) */
  {
    variable_const_t cap = value_const_get(cap_val);

    value_release(cap_val);
    if (!cap || (cap > 32768)) {
      cap = 255;
    }
    bdef = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
        VARIABLE_DEF_FLAG_NONE, 1);
    if (!bdef) {
      return VALUE_NONE;
    }
    adef = variable_def_alloc(0, VARIABLE_DEF_TYPE_ARRAY,
        VARIABLE_DEF_FLAG_NONE, 1);
    if (!adef) {
      return VALUE_NONE;
    }
    variable_def_member_add(adef, 0, bdef, (size_t) cap);
  }
  {
    static unsigned long seq;
    char        nbuf[32];
    result_t    rc;

    /* a real local variable (temps carry no RAM backing, and the
     * callee reads the bytes through the pointer) */
    sprintf(nbuf, "_strarg%lu", seq++);
    rc = pfile_value_alloc(pf, PFILE_VARIABLE_ALLOC_LOCAL, nbuf, adef,
        &buf);
    if (RESULT_OK != rc) {
      pfile_log_syserr(pf, rc);
      return VALUE_NONE;
    }
  }
  res = jal_string_to_array_emit(pf, buf, src, VALUE_NONE,
      BOOLEAN_FALSE);
  value_release(res);
  return buf;
}

static boolean_t jal_string_global_byte_alloc(pfile_t *pf, const char *name,
    flag_t flags, variable_const_t init)
{
  variable_t var;
  variable_def_t def;
  result_t rc;

  var = pfile_variable_find(pf, PFILE_LOG_NONE, name, 0);
  if (var) {
    variable_release(var);
    return BOOLEAN_TRUE;
  }
  var = VARIABLE_NONE;
  def = jal_string_byte_def_alloc(flags);
  if (!def) {
    pfile_log_syserr(pf, ENOMEM);
    return BOOLEAN_FALSE;
  }
  rc = pfile_variable_alloc(pf, PFILE_VARIABLE_ALLOC_GLOBAL, name, def,
      VARIABLE_NONE, &var);
  if (RESULT_OK != rc) {
    pfile_log_syserr(pf, rc);
    return BOOLEAN_FALSE;
  }
  variable_const_set(var, def, 0, init);
  variable_release(var);
  return BOOLEAN_TRUE;
}

static boolean_t jal_string_global_index_alloc(pfile_t *pf, const char *name,
    flag_t flags, variable_const_t init)
{
  variable_t var;
  variable_def_t def;
  result_t rc;

  var = pfile_variable_find(pf, PFILE_LOG_NONE, name, 0);
  if (var) {
    variable_release(var);
    return BOOLEAN_TRUE;
  }
  def = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER, flags,
      jal_string_current_index_size_get(pf));
  if (!def) {
    pfile_log_syserr(pf, ENOMEM);
    return BOOLEAN_FALSE;
  }
  var = VARIABLE_NONE;
  rc = pfile_variable_alloc(pf, PFILE_VARIABLE_ALLOC_GLOBAL, name, def,
      VARIABLE_NONE, &var);
  if (RESULT_OK != rc) {
    pfile_log_syserr(pf, rc);
    return BOOLEAN_FALSE;
  }
  variable_const_set(var, def, 0, init);
  variable_release(var);
  return BOOLEAN_TRUE;
}

static boolean_t jal_string_global_const_alloc(pfile_t *pf, const char *name,
    variable_const_t init)
{
  return jal_string_global_byte_alloc(pf, name, VARIABLE_DEF_FLAG_CONST, init);
}

/* Emit initial single free list node at heap[0] (runtime state). */
static void jal_string_runtime_heap_fresh_bootstrap_emit(pfile_t *pf,
    variable_const_t heap_sz, variable_sz_t index_sz, variable_sz_t header_sz,
    variable_const_t free_none)
{
  value_t ofs;
  value_t val;

  ofs = jal_string_const_get(pf, 0);
  val = jal_string_index_const_get(pf, free_none);
  jal_string_heap_index_set(pf, ofs, val);
  value_release(val);
  value_release(ofs);

  ofs = jal_string_const_get(pf, index_sz);
  val = jal_string_index_const_get(pf, heap_sz - header_sz);
  jal_string_heap_index_set(pf, ofs, val);
  value_release(val);
  value_release(ofs);

  ofs = jal_string_const_get(pf, 2 * index_sz);
  val = jal_string_const_get(pf, 0);
  jal_string_heap_index_set(pf, ofs, val);
  value_release(val);
  value_release(ofs);

  ofs = jal_string_named_value_get(pf, JAL_STRING_FREE_HEAD_NAME);
  val = jal_string_const_get(pf, 0);
  jal_string_assign_value(pf, ofs, val);
  value_release(val);
  value_release(ofs);

  ofs = jal_string_named_value_get(pf, JAL_STRING_ERROR_NAME);
  val = jal_string_const_get(pf, JAL_STRING_ERROR_NONE);
  jal_string_assign_value(pf, ofs, val);
  value_release(val);
  value_release(ofs);
}

boolean_t jal_string_runtime_ensure(pfile_t *pf)
{
  value_t heap_sz_val;
  variable_const_t heap_sz;
  variable_sz_t index_sz;
  variable_sz_t header_sz;
  variable_const_t free_none;
  variable_t heap;
  variable_def_t byte_def;
  variable_def_t heap_def;
  result_t rc;

  heap = pfile_variable_find(pf, PFILE_LOG_NONE, JAL_STRING_HEAP_NAME, 0);
  if (heap) {
    variable_release(heap);
    return BOOLEAN_TRUE;
  }

  heap_sz_val = pfile_value_find(pf, PFILE_LOG_NONE, "_string_heap_size");
  if (!heap_sz_val) {
    pfile_log(pf, PFILE_LOG_ERR,
        "dynamic string storage requires const word _string_heap_size = <bytes>");
    return BOOLEAN_FALSE;
  }
  if (!value_is_const(heap_sz_val)) {
    pfile_log(pf, PFILE_LOG_ERR,
        "_string_heap_size must be a compile-time constant");
    value_release(heap_sz_val);
    return BOOLEAN_FALSE;
  }
  heap_sz = value_const_get(heap_sz_val);
  value_release(heap_sz_val);
  index_sz = jal_string_index_size_get(pf, heap_sz);
  if (!index_sz) {
    return BOOLEAN_FALSE;
  }
  /* JAL string runtime uses at most word-sized heap indices; keep in sync
   * with `jal_string_index_max(JAL_STRING_INDEX_WORD)`. */
  if (heap_sz > 65535U) {
    pfile_log(pf, PFILE_LOG_ERR,
        "dynamic string heap is currently limited to 65535 bytes");
    return BOOLEAN_FALSE;
  }
  header_sz = (variable_sz_t) (3 * index_sz);
  if (heap_sz < (variable_const_t) header_sz + 1) {
    pfile_log(pf, PFILE_LOG_ERR,
        "_string_heap_size must reserve at least one payload byte after heap headers");
    return BOOLEAN_FALSE;
  }
  free_none = jal_string_index_max(index_sz);

  byte_def = jal_string_byte_def_alloc(VARIABLE_DEF_FLAG_NONE);
  heap_def = variable_def_alloc(0, VARIABLE_DEF_TYPE_ARRAY,
      VARIABLE_DEF_FLAG_NONE, 0);
  if (!byte_def || !heap_def
      || (RESULT_OK != variable_def_member_add(heap_def, 0, byte_def,
          (variable_ct_t) heap_sz))) {
    pfile_log_syserr(pf, ENOMEM);
    return BOOLEAN_FALSE;
  }
  heap = VARIABLE_NONE;
  rc = pfile_variable_alloc(pf, PFILE_VARIABLE_ALLOC_GLOBAL,
      JAL_STRING_HEAP_NAME, heap_def, VARIABLE_NONE, &heap);
  if (RESULT_OK != rc) {
    pfile_log_syserr(pf, rc);
    return BOOLEAN_FALSE;
  }
  variable_release(heap);

  if (!jal_string_global_index_alloc(pf, JAL_STRING_FREE_HEAD_NAME,
      VARIABLE_DEF_FLAG_NONE, 0)
      || !jal_string_global_byte_alloc(pf, JAL_STRING_ERROR_NAME,
        VARIABLE_DEF_FLAG_NONE, JAL_STRING_ERROR_NONE)
      || !jal_string_global_const_alloc(pf, "string_error_none",
        JAL_STRING_ERROR_NONE)
      || !jal_string_global_const_alloc(pf, "string_error_heap_full",
        JAL_STRING_ERROR_HEAP_FULL)
      || !jal_string_global_const_alloc(pf, "string_error_overflow",
        JAL_STRING_ERROR_OVERFLOW)
      || !jal_string_global_const_alloc(pf, "string_error_invalid",
        JAL_STRING_ERROR_INVALID)) {
    return BOOLEAN_FALSE;
  }
  jal_string_runtime_heap_fresh_bootstrap_emit(pf, heap_sz, index_sz, header_sz,
      free_none);
  return BOOLEAN_TRUE;
}

static void jal_string_branch_if_const_cmp(pfile_t *pf, operator_t op,
    value_t val, variable_const_t n, cmd_branchcond_t cond, label_t lbl)
{
  value_t c;
  value_t test;

  c = jal_string_index_const_get(pf, n);
  /* compiler-generated relation: fold quietly (a heap-size cap vs an
   * index-typed length is provably dead at cap == index max, e.g.) */
  pfile_relation_warning_hold(pf);
  test = jal_string_binary_emit(pf, op, val, c);
  pfile_relation_warning_release(pf);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, cond, lbl, test,
      VALUE_NONE, 0);
  value_release(test);
  value_release(c);
}

static void jal_string_branch_if_value_cmp(pfile_t *pf, operator_t op,
    value_t a, value_t b, cmd_branchcond_t cond, label_t lbl)
{
  value_t test;

  pfile_relation_warning_hold(pf);
  test = jal_string_binary_emit(pf, op, a, b);
  pfile_relation_warning_release(pf);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, cond, lbl, test,
      VALUE_NONE, 0);
  value_release(test);
}

static value_t jal_string_source_byte_get(pfile_t *pf, value_t src,
    value_t idx)
{
  value_t src_byte;

  if (jal_string_is_dynamic(src)) {
    value_t src_off;
    value_t src_payload;

    src_off = jal_string_handle_index_get(pf, src, JAL_STRING_FIELD_OFFSET);
    src_payload = jal_string_add_const(pf, src_off,
        jal_string_header_size_get(pf));
    value_release(src_off);
    {
      value_t tmp;

      tmp = jal_string_binary_emit(pf, OPERATOR_ADD, src_payload, idx);
      value_release(src_payload);
      src_payload = tmp;
    }
    src_byte = jal_string_heap_elem_get(pf, src_payload);
    value_release(src_payload);
  } else {
    src_byte = jal_string_array_elem_get(src, idx);
  }
  return src_byte;
}

static void jal_string_copy_at_emit(pfile_t *pf, value_t dst, value_t src,
    value_t len, value_t dst_start)
{
  label_t loop_lbl;
  label_t done_lbl;
  value_t idx;
  value_t dst_off;
  value_t dst_payload;
  value_t src_byte;
  value_t dst_byte;
  value_t one;

  loop_lbl = pfile_label_alloc(pf, 0);
  done_lbl = pfile_label_alloc(pf, 0);
  idx = jal_string_temp_index_get(pf);
  one = jal_string_const_get(pf, 1);
  src_byte = VALUE_NONE;
  dst_byte = VALUE_NONE;
  dst_payload = VALUE_NONE;
  dst_off = jal_string_handle_index_get(pf, dst, JAL_STRING_FIELD_OFFSET);

  {
    value_t zero;

    zero = jal_string_const_get(pf, 0);
    jal_string_assign_value(pf, idx, zero);
    value_release(zero);
  }
  pfile_cmd_label_add(pf, loop_lbl);
  jal_string_branch_if_value_cmp(pf, OPERATOR_GE, idx, len,
      CMD_BRANCHCOND_TRUE, done_lbl);

  dst_payload = jal_string_add_const(pf, dst_off,
      jal_string_header_size_get(pf));
  {
    value_t tmp;
    value_t dst_idx;

    dst_idx = jal_string_binary_emit(pf, OPERATOR_ADD, dst_start, idx);
    tmp = jal_string_binary_emit(pf, OPERATOR_ADD, dst_payload, dst_idx);
    value_release(dst_idx);
    value_release(dst_payload);
    dst_payload = tmp;
  }
  dst_byte = jal_string_heap_elem_get(pf, dst_payload);

  src_byte = jal_string_source_byte_get(pf, src, idx);
  jal_string_assign_value(pf, dst_byte, src_byte);
  pfile_cmd_op_add(pf, OPERATOR_ADD, &idx, idx, one);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
      loop_lbl, VALUE_NONE, VALUE_NONE, 0);
  pfile_cmd_label_add(pf, done_lbl);

  value_release(dst_byte);
  value_release(src_byte);
  value_release(dst_payload);
  value_release(dst_off);
  value_release(idx);
  value_release(one);
  label_release(done_lbl);
  label_release(loop_lbl);
}

static value_t jal_string_bool_result_get(pfile_t *pf, boolean_t init)
{
  value_t result;
  value_t val;

  result = pfile_value_temp_get(pf, VARIABLE_DEF_TYPE_BOOLEAN, 1);
  val = jal_string_const_get(pf, init ? 1 : 0);
  jal_string_assign_value(pf, result, val);
  value_release(val);
  return result;
}

static void jal_string_bool_assign(pfile_t *pf, value_t dst, boolean_t val)
{
  value_t cval;

  cval = jal_string_const_get(pf, val ? 1 : 0);
  jal_string_assign_value(pf, dst, cval);
  value_release(cval);
}

static value_t jal_string_heap_payload_byte_get(pfile_t *pf, value_t off,
    value_t idx)
{
  value_t payload;
  value_t src_byte;
  value_t tmp;

  payload = jal_string_add_const(pf, off, jal_string_header_size_get(pf));
  tmp = jal_string_binary_emit(pf, OPERATOR_ADD, payload, idx);
  value_release(payload);
  payload = tmp;
  src_byte = jal_string_heap_elem_get(pf, payload);
  value_release(payload);
  return src_byte;
}

/* --- heap-string subscripts (str[i]: read/write the i-th character) ---
 * A subscripted dynamic string parses to the normal element view (byte
 * def, the string variable, baseofs = the UNSCALED index -- string
 * elements are bytes so the MUL-by-esz path is skipped). That view on a
 * heap string would read the HANDLE's storage, so rvalue uses are
 * replaced at parse time with this emitted payload load, and the
 * assignment path routes the element view to the payload store below.
 * Unchecked, exactly like array subscripts: an index at or past the
 * effective length reads/writes whatever shares the block, and a write
 * past the block capacity corrupts the next header. */

boolean_t jal_string_subscript_value_is(value_t val)
{
  variable_t var;

  var = val ? value_variable_get(val) : VARIABLE_NONE;
  return (var && (VARIABLE_NONE != var)
      && variable_def_flag_test(variable_def_get(var),
           VARIABLE_DEF_FLAG_STRING_DYNAMIC)
      && (VALUE_NONE != value_baseofs_get(val)))
    ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* base string value of a subscripted element view (drops the index,
 * restores the STRING def so the handle machinery sees a dynamic
 * string) plus a locked clone of the index itself */
static boolean_t jal_string_subscript_split(pfile_t *pf, value_t elem,
    value_t *pbase, value_t *pidx)
{
  variable_t var;
  value_t    base;
  value_t    idx;

  UNUSED(pf);
  var = value_variable_get(elem);
  if (!var || (VARIABLE_NONE == var)) {
    return BOOLEAN_FALSE;
  }
  base = value_clone(elem);
  idx  = value_clone(value_baseofs_get(elem));
  if (!base || !idx) {
    value_release(base);
    value_release(idx);
    return BOOLEAN_FALSE;
  }
  value_baseofs_set(base, VALUE_NONE);
  value_def_set(base, variable_def_get(var));
  value_lock(idx);
  *pbase = base;
  *pidx  = idx;
  return BOOLEAN_TRUE;
}

value_t jal_string_subscript_read_emit(pfile_t *pf, value_t elem)
{
  value_t base;
  value_t idx;
  value_t off;
  value_t byte;

  if (!jal_string_runtime_ensure(pf)
      || !jal_string_subscript_split(pf, elem, &base, &idx)) {
    return VALUE_NONE;
  }
  off = jal_string_handle_index_get(pf, base, JAL_STRING_FIELD_OFFSET);
  byte = jal_string_heap_payload_byte_get(pf, off, idx);
  value_release(off);
  value_release(idx);
  value_release(base);
  return byte;
}

boolean_t jal_string_subscript_write_emit(pfile_t *pf, value_t dst,
    value_t src)
{
  value_t base;
  value_t idx;
  value_t off;
  value_t ofs;
  value_t hdr;

  if (!src || !jal_string_subscript_value_is(dst)
      || value_is_bit(src)
      || (value_is_string(src) && !value_is_const(src))) {
    return BOOLEAN_FALSE;
  }
  if (!jal_string_runtime_ensure(pf)
      || !jal_string_subscript_split(pf, dst, &base, &idx)) {
    return BOOLEAN_FALSE;
  }
  if (1 != value_byte_sz_get(src)) {
    pfile_log(pf, PFILE_LOG_ERR,
        "string element assignment requires a byte value");
  } else {
    off = jal_string_handle_index_get(pf, base, JAL_STRING_FIELD_OFFSET);
    hdr = jal_string_add_const(pf, off, jal_string_header_size_get(pf));
    value_release(off);
    ofs = jal_string_binary_emit(pf, OPERATOR_ADD, hdr, idx);
    value_release(hdr);
    jal_string_heap_index_set(pf, ofs, src);
    value_release(ofs);
  }
  value_release(idx);
  value_release(base);
  return BOOLEAN_TRUE;
}

static void jal_string_copy_heap_at_emit(pfile_t *pf, value_t dst,
    value_t src_off, value_t len, value_t dst_start)
{
  label_t loop_lbl;
  label_t done_lbl;
  value_t idx;
  value_t dst_off;
  value_t dst_payload;
  value_t src_byte;
  value_t dst_byte;
  value_t one;
  value_t zero;

  loop_lbl = pfile_label_alloc(pf, 0);
  done_lbl = pfile_label_alloc(pf, 0);
  idx = jal_string_temp_index_get(pf);
  one = jal_string_const_get(pf, 1);
  zero = jal_string_const_get(pf, 0);
  dst_off = jal_string_handle_index_get(pf, dst, JAL_STRING_FIELD_OFFSET);
  src_byte = VALUE_NONE;
  dst_byte = VALUE_NONE;
  dst_payload = VALUE_NONE;

  jal_string_assign_value(pf, idx, zero);
  pfile_cmd_label_add(pf, loop_lbl);
  jal_string_branch_if_value_cmp(pf, OPERATOR_GE, idx, len,
      CMD_BRANCHCOND_TRUE, done_lbl);

  dst_payload = jal_string_add_const(pf, dst_off,
      jal_string_header_size_get(pf));
  {
    value_t tmp;
    value_t dst_idx;

    dst_idx = jal_string_binary_emit(pf, OPERATOR_ADD, dst_start, idx);
    tmp = jal_string_binary_emit(pf, OPERATOR_ADD, dst_payload, dst_idx);
    value_release(dst_idx);
    value_release(dst_payload);
    dst_payload = tmp;
  }
  dst_byte = jal_string_heap_elem_get(pf, dst_payload);
  src_byte = jal_string_heap_payload_byte_get(pf, src_off, idx);
  jal_string_assign_value(pf, dst_byte, src_byte);
  pfile_cmd_op_add(pf, OPERATOR_ADD, &idx, idx, one);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
      loop_lbl, VALUE_NONE, VALUE_NONE, 0);
  pfile_cmd_label_add(pf, done_lbl);

  value_release(dst_byte);
  value_release(src_byte);
  value_release(dst_payload);
  value_release(dst_off);
  value_release(zero);
  value_release(one);
  value_release(idx);
  label_release(done_lbl);
  label_release(loop_lbl);
}

static void jal_string_copy_emit(pfile_t *pf, value_t dst, value_t src,
    value_t len)
{
  value_t zero;

  zero = jal_string_const_get(pf, 0);
  jal_string_copy_at_emit(pf, dst, src, len, zero);
  value_release(zero);
}

static void jal_string_copy_to_array_emit(pfile_t *pf, value_t dst,
    value_t src, value_t len)
{
  label_t loop_lbl;
  label_t done_lbl;
  value_t idx;
  value_t src_byte;
  value_t dst_byte;
  value_t one;
  value_t zero;

  loop_lbl = pfile_label_alloc(pf, 0);
  done_lbl = pfile_label_alloc(pf, 0);
  idx = jal_string_temp_index_get(pf);
  one = jal_string_const_get(pf, 1);
  zero = jal_string_const_get(pf, 0);
  src_byte = VALUE_NONE;
  dst_byte = VALUE_NONE;

  jal_string_assign_value(pf, idx, zero);
  pfile_cmd_label_add(pf, loop_lbl);
  jal_string_branch_if_value_cmp(pf, OPERATOR_GE, idx, len,
      CMD_BRANCHCOND_TRUE, done_lbl);
  dst_byte = jal_string_array_elem_get(dst, idx);
  src_byte = jal_string_source_byte_get(pf, src, idx);
  jal_string_assign_value(pf, dst_byte, src_byte);
  pfile_cmd_op_add(pf, OPERATOR_ADD, &idx, idx, one);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
      loop_lbl, VALUE_NONE, VALUE_NONE, 0);
  pfile_cmd_label_add(pf, done_lbl);

  value_release(dst_byte);
  value_release(src_byte);
  value_release(zero);
  value_release(one);
  value_release(idx);
  label_release(done_lbl);
  label_release(loop_lbl);
}

static void jal_string_assign_min_emit(pfile_t *pf, value_t dst,
    value_t candidate)
{
  label_t done_lbl;

  done_lbl = pfile_label_alloc(pf, 0);
  jal_string_branch_if_value_cmp(pf, OPERATOR_GE, candidate, dst,
      CMD_BRANCHCOND_TRUE, done_lbl);
  jal_string_assign_value(pf, dst, candidate);
  pfile_cmd_label_add(pf, done_lbl);
  label_release(done_lbl);
}

static void jal_string_free_old_block_emit(pfile_t *pf, value_t old_off,
    value_t old_cap)
{
  label_t done_lbl;
  label_t find_lbl;
  label_t insert_lbl;
  label_t insert_head_lbl;
  label_t inserted_lbl;
  label_t no_next_merge_lbl;
  label_t no_prev_merge_lbl;
  value_t free_head;
  value_t old_cap_ofs;
  value_t old_len_ofs;
  value_t old_end;
  value_t old_next;
  value_t prev;
  value_t prev_cap;
  value_t prev_cap_ofs;
  value_t prev_end;
  value_t cur;
  value_t cur_next;
  value_t cur_cap;
  value_t cur_cap_ofs;
  value_t free_none;
  value_t zero;
  value_t tmp;

  done_lbl = pfile_label_alloc(pf, 0);
  find_lbl = pfile_label_alloc(pf, 0);
  insert_lbl = pfile_label_alloc(pf, 0);
  insert_head_lbl = pfile_label_alloc(pf, 0);
  inserted_lbl = pfile_label_alloc(pf, 0);
  no_next_merge_lbl = pfile_label_alloc(pf, 0);
  no_prev_merge_lbl = pfile_label_alloc(pf, 0);
  jal_string_branch_if_const_cmp(pf, OPERATOR_EQ, old_cap, 0,
      CMD_BRANCHCOND_TRUE, done_lbl);

  free_head = jal_string_named_value_get(pf, JAL_STRING_FREE_HEAD_NAME);
  prev = jal_string_temp_index_get(pf);
  cur = jal_string_temp_index_get(pf);
  cur_next = jal_string_temp_index_get(pf);
  cur_cap = jal_string_temp_index_get(pf);
  prev_cap = jal_string_temp_index_get(pf);
  old_next = jal_string_temp_index_get(pf);
  old_end = jal_string_temp_index_get(pf);
  prev_end = jal_string_temp_index_get(pf);
  free_none = jal_string_index_const_get(pf,
      jal_string_index_max(jal_string_current_index_size_get(pf)));
  zero = jal_string_const_get(pf, 0);

  jal_string_assign_value(pf, prev, free_none);
  jal_string_assign_value(pf, cur, free_head);

  pfile_cmd_label_add(pf, find_lbl);
  jal_string_branch_if_const_cmp(pf, OPERATOR_EQ, cur,
      jal_string_index_max(jal_string_current_index_size_get(pf)),
      CMD_BRANCHCOND_TRUE, insert_lbl);
  jal_string_branch_if_value_cmp(pf, OPERATOR_LT, old_off, cur,
      CMD_BRANCHCOND_TRUE, insert_lbl);
  tmp = jal_string_heap_index_get(pf, cur);
  jal_string_assign_value(pf, cur_next, tmp);
  value_release(tmp);
  jal_string_assign_value(pf, prev, cur);
  jal_string_assign_value(pf, cur, cur_next);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
      find_lbl, VALUE_NONE, VALUE_NONE, 0);

  pfile_cmd_label_add(pf, insert_lbl);
  old_cap_ofs = jal_string_add_const(pf, old_off,
      jal_string_current_index_size_get(pf));
  old_len_ofs = jal_string_add_const(pf, old_off,
      2 * jal_string_current_index_size_get(pf));
  jal_string_heap_index_set(pf, old_cap_ofs, old_cap);
  jal_string_heap_index_set(pf, old_len_ofs, zero);
  jal_string_heap_index_set(pf, old_off, cur);
  jal_string_branch_if_const_cmp(pf, OPERATOR_EQ, prev,
      jal_string_index_max(jal_string_current_index_size_get(pf)),
      CMD_BRANCHCOND_TRUE, insert_head_lbl);
  jal_string_heap_index_set(pf, prev, old_off);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
      inserted_lbl, VALUE_NONE, VALUE_NONE, 0);
  pfile_cmd_label_add(pf, insert_head_lbl);
  jal_string_assign_value(pf, free_head, old_off);

  pfile_cmd_label_add(pf, inserted_lbl);
  jal_string_branch_if_const_cmp(pf, OPERATOR_EQ, cur,
      jal_string_index_max(jal_string_current_index_size_get(pf)),
      CMD_BRANCHCOND_TRUE, no_next_merge_lbl);
  cur_cap_ofs = jal_string_add_const(pf, cur,
      jal_string_current_index_size_get(pf));
  tmp = jal_string_heap_index_get(pf, cur_cap_ofs);
  jal_string_assign_value(pf, cur_cap, tmp);
  value_release(tmp);
  tmp = jal_string_add_const(pf, old_off, jal_string_header_size_get(pf));
  jal_string_assign_value(pf, old_end, tmp);
  value_release(tmp);
  pfile_cmd_op_add(pf, OPERATOR_ADD, &old_end, old_end, old_cap);
  jal_string_branch_if_value_cmp(pf, OPERATOR_NE, old_end, cur,
      CMD_BRANCHCOND_TRUE, no_next_merge_lbl);
  tmp = jal_string_heap_index_get(pf, cur);
  jal_string_assign_value(pf, cur_next, tmp);
  value_release(tmp);
  tmp = jal_string_add_const(pf, old_cap, jal_string_header_size_get(pf));
  jal_string_assign_value(pf, old_cap, tmp);
  value_release(tmp);
  pfile_cmd_op_add(pf, OPERATOR_ADD, &old_cap, old_cap, cur_cap);
  jal_string_heap_index_set(pf, old_off, cur_next);
  jal_string_heap_index_set(pf, old_cap_ofs, old_cap);
  value_release(cur_cap_ofs);

  pfile_cmd_label_add(pf, no_next_merge_lbl);
  jal_string_branch_if_const_cmp(pf, OPERATOR_EQ, prev,
      jal_string_index_max(jal_string_current_index_size_get(pf)),
      CMD_BRANCHCOND_TRUE, no_prev_merge_lbl);
  prev_cap_ofs = jal_string_add_const(pf, prev,
      jal_string_current_index_size_get(pf));
  tmp = jal_string_heap_index_get(pf, prev_cap_ofs);
  jal_string_assign_value(pf, prev_cap, tmp);
  value_release(tmp);
  tmp = jal_string_add_const(pf, prev, jal_string_header_size_get(pf));
  jal_string_assign_value(pf, prev_end, tmp);
  value_release(tmp);
  pfile_cmd_op_add(pf, OPERATOR_ADD, &prev_end, prev_end, prev_cap);
  jal_string_branch_if_value_cmp(pf, OPERATOR_NE, prev_end, old_off,
      CMD_BRANCHCOND_TRUE, no_prev_merge_lbl);
  tmp = jal_string_heap_index_get(pf, old_off);
  jal_string_assign_value(pf, old_next, tmp);
  value_release(tmp);
  tmp = jal_string_add_const(pf, prev_cap, jal_string_header_size_get(pf));
  jal_string_assign_value(pf, prev_cap, tmp);
  value_release(tmp);
  pfile_cmd_op_add(pf, OPERATOR_ADD, &prev_cap, prev_cap, old_cap);
  jal_string_heap_index_set(pf, prev, old_next);
  jal_string_heap_index_set(pf, prev_cap_ofs, prev_cap);
  value_release(prev_cap_ofs);

  pfile_cmd_label_add(pf, no_prev_merge_lbl);

  value_release(zero);
  value_release(free_none);
  value_release(prev_end);
  value_release(old_end);
  value_release(old_next);
  value_release(prev_cap);
  value_release(cur_cap);
  value_release(cur_next);
  value_release(cur);
  value_release(prev);
  value_release(old_len_ofs);
  value_release(old_cap_ofs);
  value_release(free_head);
  pfile_cmd_label_add(pf, done_lbl);
  label_release(no_prev_merge_lbl);
  label_release(no_next_merge_lbl);
  label_release(inserted_lbl);
  label_release(insert_head_lbl);
  label_release(insert_lbl);
  label_release(find_lbl);
  label_release(done_lbl);
}

static void jal_string_clear_emit(pfile_t *pf, value_t dst)
{
  value_t old_off;
  value_t old_cap;
  value_t old_off_lval;
  value_t old_cap_lval;
  value_t zero;

  old_off = jal_string_temp_index_get(pf);
  old_cap = jal_string_temp_index_get(pf);
  old_off_lval = jal_string_handle_index_get(pf, dst,
      JAL_STRING_FIELD_OFFSET);
  old_cap_lval = jal_string_handle_index_get(pf, dst,
      JAL_STRING_FIELD_CAP);
  zero = jal_string_const_get(pf, 0);

  jal_string_assign_value(pf, old_off, old_off_lval);
  jal_string_assign_value(pf, old_cap, old_cap_lval);
  jal_string_free_old_block_emit(pf, old_off, old_cap);
  jal_string_handle_index_set(pf, dst, JAL_STRING_FIELD_OFFSET, zero);
  jal_string_handle_index_set(pf, dst, JAL_STRING_FIELD_LENGTH, zero);
  jal_string_handle_index_set(pf, dst, JAL_STRING_FIELD_CAP, zero);

  value_release(zero);
  value_release(old_cap_lval);
  value_release(old_off_lval);
  value_release(old_cap);
  value_release(old_off);
}

static void jal_string_allocate_emit_ex(pfile_t *pf, value_t dst, value_t len,
    label_t fail_lbl, label_t have_storage_lbl, boolean_t free_old,
    value_t *old_off_keep, value_t *old_cap_keep)
{
  label_t loop_lbl;
  label_t best_loop_lbl;
  label_t take_best_lbl;
  label_t skip_best_lbl;
  label_t after_best_walk_lbl;
  label_t found_lbl;
  label_t no_split_lbl;
  label_t unlink_lbl;
  label_t scan_fail_lbl;
  label_t compact_fail_lbl;
  value_t free_head;
  value_t prev;
  value_t cur;
  value_t old_next;
  value_t free_cap;
  value_t leftover;
  value_t replacement;
  value_t alloc_cap;
  value_t cur_next_ofs;
  value_t cur_cap_ofs;
  value_t cur_cap_slot;
  value_t old_off;
  value_t old_off_lval;
  value_t old_cap;
  value_t old_cap_lval;
  value_t dst_off;
  value_t dst_cap;
  value_t split_ofs;
  value_t split_next;
  value_t split_cap;
  value_t split_len;
  value_t tmp;
  value_t zero;
  value_t free_nonev;
  variable_const_t idx_max;
  boolean_t best_fit;
  boolean_t use_retry;
  value_t tried;
  value_t best_prev;
  value_t best_cur;
  value_t best_cap;
  value_t cpt_res;

  tried = VALUE_NONE;
  best_prev = VALUE_NONE;
  best_cur = VALUE_NONE;
  best_cap = VALUE_NONE;
  cpt_res = VALUE_NONE;
  best_fit = (0U != pf->jal_str_heap_alloc_policy);
  use_retry = (0U != pf->jal_str_heap_alloc_retry);

  /* A compile-time-constant request that cannot fit even in a fresh heap
   * (header + payload > _string_heap_size) can never succeed -- compaction
   * and retry only reclaim, they cannot grow the heap. Diagnose it here
   * instead of leaving it to the runtime _string_error path (the string
   * stays silently empty). Runtime-length requests keep the runtime
   * contract (check _string_error after the assign). */
  if (value_is_const(len)) {
    value_t          heap_sz_val;
    variable_const_t heap_sz;

    heap_sz_val = pfile_value_find(pf, PFILE_LOG_NONE, "_string_heap_size");
    if (heap_sz_val) {
      heap_sz = value_const_get(heap_sz_val);
      value_release(heap_sz_val);
      if ((variable_const_t) jal_string_header_size_get(pf)
          + value_const_get(len) > heap_sz) {
        pfile_log(pf, PFILE_LOG_ERR,
            "string needs %lu heap bytes (%lu header + %lu payload)"
            " but _string_heap_size is %lu",
            (unsigned long) (jal_string_header_size_get(pf)
              + value_const_get(len)),
            (unsigned long) jal_string_header_size_get(pf),
            (unsigned long) value_const_get(len),
            (unsigned long) heap_sz);
      }
    }
  }

  idx_max = jal_string_index_max(jal_string_current_index_size_get(pf));

  loop_lbl = pfile_label_alloc(pf, 0);
  best_loop_lbl = pfile_label_alloc(pf, 0);
  take_best_lbl = pfile_label_alloc(pf, 0);
  skip_best_lbl = pfile_label_alloc(pf, 0);
  after_best_walk_lbl = pfile_label_alloc(pf, 0);
  found_lbl = pfile_label_alloc(pf, 0);
  no_split_lbl = pfile_label_alloc(pf, 0);
  unlink_lbl = pfile_label_alloc(pf, 0);
  scan_fail_lbl = pfile_label_alloc(pf, 0);
  compact_fail_lbl = pfile_label_alloc(pf, 0);
  free_head = jal_string_named_value_get(pf, JAL_STRING_FREE_HEAD_NAME);
  prev = jal_string_temp_index_get(pf);
  cur = jal_string_temp_index_get(pf);
  old_next = jal_string_temp_index_get(pf);
  free_cap = jal_string_temp_index_get(pf);
  leftover = jal_string_temp_index_get(pf);
  replacement = jal_string_temp_index_get(pf);
  alloc_cap = jal_string_temp_index_get(pf);
  zero = jal_string_const_get(pf, 0);

  old_off = jal_string_temp_index_get(pf);
  old_cap = jal_string_temp_index_get(pf);
  old_off_lval = jal_string_handle_index_get(pf, dst,
      JAL_STRING_FIELD_OFFSET);
  old_cap_lval = jal_string_handle_index_get(pf, dst,
      JAL_STRING_FIELD_CAP);
  jal_string_assign_value(pf, old_off, old_off_lval);
  jal_string_assign_value(pf, old_cap, old_cap_lval);
  value_release(old_cap_lval);
  value_release(old_off_lval);

  free_nonev = jal_string_index_const_get(pf, idx_max);
  jal_string_assign_value(pf, prev, free_nonev);
  jal_string_assign_value(pf, cur, free_head);
  if (use_retry) {
    tried = pfile_value_temp_get(pf, VARIABLE_DEF_TYPE_BOOLEAN, 1);
    if (VALUE_NONE != tried) {
      jal_string_assign_value(pf, tried, zero);
    }
  }
  if (best_fit) {
    best_prev = jal_string_temp_index_get(pf);
    best_cur = jal_string_temp_index_get(pf);
    best_cap = jal_string_temp_index_get(pf);
    jal_string_assign_value(pf, best_prev, free_nonev);
    jal_string_assign_value(pf, best_cur, free_nonev);
    pfile_cmd_label_add(pf, best_loop_lbl);
    jal_string_branch_if_const_cmp(pf, OPERATOR_EQ, cur, idx_max,
        CMD_BRANCHCOND_TRUE, after_best_walk_lbl);
    cur_cap_ofs = jal_string_add_const(pf, cur,
        jal_string_current_index_size_get(pf));
    cur_cap_slot = jal_string_heap_index_get(pf, cur_cap_ofs);
    jal_string_assign_value(pf, free_cap, cur_cap_slot);
    jal_string_branch_if_value_cmp(pf, OPERATOR_LT, free_cap, len,
        CMD_BRANCHCOND_TRUE, skip_best_lbl);
    jal_string_branch_if_const_cmp(pf, OPERATOR_EQ, best_cur, idx_max,
        CMD_BRANCHCOND_TRUE, take_best_lbl);
    jal_string_branch_if_value_cmp(pf, OPERATOR_LT, free_cap, best_cap,
        CMD_BRANCHCOND_TRUE, take_best_lbl);
    pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
        skip_best_lbl, VALUE_NONE, VALUE_NONE, 0);
    pfile_cmd_label_add(pf, take_best_lbl);
    jal_string_assign_value(pf, best_prev, prev);
    jal_string_assign_value(pf, best_cur, cur);
    jal_string_assign_value(pf, best_cap, free_cap);
    pfile_cmd_label_add(pf, skip_best_lbl);
    cur_next_ofs = jal_string_heap_index_get(pf, cur);
    jal_string_assign_value(pf, prev, cur);
    jal_string_assign_value(pf, cur, cur_next_ofs);
    value_release(cur_next_ofs);
    pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
        best_loop_lbl, VALUE_NONE, VALUE_NONE, 0);
    pfile_cmd_label_add(pf, after_best_walk_lbl);
    jal_string_branch_if_const_cmp(pf, OPERATOR_EQ, best_cur, idx_max,
        CMD_BRANCHCOND_TRUE, scan_fail_lbl);
    jal_string_assign_value(pf, prev, best_prev);
    jal_string_assign_value(pf, cur, best_cur);
    jal_string_assign_value(pf, free_cap, best_cap);
    pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
        found_lbl, VALUE_NONE, VALUE_NONE, 0);
  } else {
    pfile_cmd_label_add(pf, loop_lbl);
    jal_string_branch_if_const_cmp(pf, OPERATOR_EQ, cur, idx_max,
        CMD_BRANCHCOND_TRUE, scan_fail_lbl);
    cur_cap_ofs = jal_string_add_const(pf, cur,
        jal_string_current_index_size_get(pf));
    cur_cap_slot = jal_string_heap_index_get(pf, cur_cap_ofs);
    jal_string_assign_value(pf, free_cap, cur_cap_slot);
    jal_string_branch_if_value_cmp(pf, OPERATOR_GE, free_cap, len,
        CMD_BRANCHCOND_TRUE, found_lbl);
    cur_next_ofs = jal_string_heap_index_get(pf, cur);
    jal_string_assign_value(pf, prev, cur);
    jal_string_assign_value(pf, cur, cur_next_ofs);
    pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
        loop_lbl, VALUE_NONE, VALUE_NONE, 0);
  }

  pfile_cmd_label_add(pf, scan_fail_lbl);
  if (use_retry && (VALUE_NONE != tried)) {
    jal_string_branch_if_const_cmp(pf, OPERATOR_NE, tried, 0,
        CMD_BRANCHCOND_TRUE, fail_lbl);
    jal_string_bool_assign(pf, tried, BOOLEAN_TRUE);
    cpt_res = pfile_value_temp_get(pf, VARIABLE_DEF_TYPE_BOOLEAN, 1U);
    if (VALUE_NONE == cpt_res) {
      pfile_log_syserr(pf, ENOMEM);
      pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
          fail_lbl, VALUE_NONE, VALUE_NONE, 0);
    } else {
      jal_string_heap_compact_fill_bld(pf, cpt_res);
      jal_string_branch_if_const_cmp(pf, OPERATOR_EQ, cpt_res, 0,
          CMD_BRANCHCOND_TRUE, compact_fail_lbl);
      value_release(cpt_res);
      cpt_res = VALUE_NONE;
      jal_string_assign_value(pf, prev, free_nonev);
      jal_string_assign_value(pf, cur, free_head);
      if (best_fit) {
        jal_string_assign_value(pf, best_prev, free_nonev);
        jal_string_assign_value(pf, best_cur, free_nonev);
        pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
            best_loop_lbl, VALUE_NONE, VALUE_NONE, 0);
      } else {
        pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
            loop_lbl, VALUE_NONE, VALUE_NONE, 0);
      }
      pfile_cmd_label_add(pf, compact_fail_lbl);
      value_release(cpt_res);
      pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
          fail_lbl, VALUE_NONE, VALUE_NONE, 0);
    }
  } else {
    pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
        fail_lbl, VALUE_NONE, VALUE_NONE, 0);
  }

  pfile_cmd_label_add(pf, found_lbl);
  cur_next_ofs = jal_string_heap_index_get(pf, cur);
  jal_string_assign_value(pf, old_next, cur_next_ofs);
  tmp = jal_string_binary_emit(pf, OPERATOR_SUB, free_cap, len);
  jal_string_assign_value(pf, leftover, tmp);
  value_release(tmp);
  jal_string_branch_if_const_cmp(pf, OPERATOR_LT, leftover,
      jal_string_header_size_get(pf) + 1,
      CMD_BRANCHCOND_TRUE, no_split_lbl);

  split_ofs = jal_string_add_const(pf, cur, jal_string_header_size_get(pf));
  tmp = jal_string_binary_emit(pf, OPERATOR_ADD, split_ofs, len);
  value_release(split_ofs);
  split_ofs = tmp;
  split_next = jal_string_heap_elem_get(pf, split_ofs);
  tmp = jal_string_add_const(pf, split_ofs,
      jal_string_current_index_size_get(pf));
  split_cap = tmp;
  tmp = jal_string_add_const(pf, split_ofs,
      2 * jal_string_current_index_size_get(pf));
  split_len = tmp;
  jal_string_heap_index_set(pf, split_ofs, old_next);
  {
    value_t header_sz;

    header_sz = jal_string_index_const_get(pf, jal_string_header_size_get(pf));
    tmp = jal_string_binary_emit(pf, OPERATOR_SUB, leftover, header_sz);
    value_release(header_sz);
    jal_string_heap_index_set(pf, split_cap, tmp);
    value_release(tmp);
  }
  jal_string_heap_index_set(pf, split_len, zero);
  jal_string_assign_value(pf, replacement, split_ofs);
  jal_string_assign_value(pf, alloc_cap, len);
  value_release(split_len);
  value_release(split_cap);
  value_release(split_next);
  value_release(split_ofs);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
      unlink_lbl, VALUE_NONE, VALUE_NONE, 0);

  pfile_cmd_label_add(pf, no_split_lbl);
  jal_string_assign_value(pf, replacement, old_next);
  jal_string_assign_value(pf, alloc_cap, free_cap);

  pfile_cmd_label_add(pf, unlink_lbl);
  {
    label_t prev_is_head_lbl;
    label_t after_unlink_lbl;
    value_t prev_next;

    prev_is_head_lbl = pfile_label_alloc(pf, 0);
    after_unlink_lbl = pfile_label_alloc(pf, 0);
    jal_string_branch_if_const_cmp(pf, OPERATOR_EQ, prev,
        jal_string_index_max(jal_string_current_index_size_get(pf)),
        CMD_BRANCHCOND_TRUE, prev_is_head_lbl);
    prev_next = jal_string_heap_index_get(pf, prev);
    jal_string_heap_index_set(pf, prev, replacement);
    value_release(prev_next);
    pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
        after_unlink_lbl, VALUE_NONE, VALUE_NONE, 0);
    pfile_cmd_label_add(pf, prev_is_head_lbl);
    jal_string_assign_value(pf, free_head, replacement);
    pfile_cmd_label_add(pf, after_unlink_lbl);
    label_release(after_unlink_lbl);
    label_release(prev_is_head_lbl);
  }

  dst_off = jal_string_handle_field_ofs_get(pf, JAL_STRING_FIELD_OFFSET);
  dst_cap = jal_string_handle_field_ofs_get(pf, JAL_STRING_FIELD_CAP);
  jal_string_handle_index_set(pf, dst, JAL_STRING_FIELD_OFFSET, cur);
  jal_string_handle_index_set(pf, dst, JAL_STRING_FIELD_CAP, alloc_cap);
  if (free_old) {
    jal_string_free_old_block_emit(pf, old_off, old_cap);
  } else {
    if (old_off_keep) {
      value_lock(old_off);
      *old_off_keep = old_off;
    }
    if (old_cap_keep) {
      value_lock(old_cap);
      *old_cap_keep = old_cap;
    }
  }
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
      have_storage_lbl, VALUE_NONE, VALUE_NONE, 0);

  value_release(dst_cap);
  value_release(dst_off);
  value_release(old_cap);
  value_release(old_off);
  value_release(zero);
  value_release(cur_cap_slot);
  value_release(cur_cap_ofs);
  value_release(alloc_cap);
  value_release(replacement);
  value_release(leftover);
  value_release(free_cap);
  value_release(old_next);
  value_release(cur);
  value_release(prev);
  value_release(free_head);
  value_release(free_nonev);
  if (VALUE_NONE != tried) {
    value_release(tried);
  }
  if (best_fit) {
    value_release(best_cap);
    value_release(best_cur);
    value_release(best_prev);
  }
  label_release(unlink_lbl);
  label_release(no_split_lbl);
  label_release(found_lbl);
  label_release(compact_fail_lbl);
  label_release(scan_fail_lbl);
  label_release(after_best_walk_lbl);
  label_release(skip_best_lbl);
  label_release(take_best_lbl);
  label_release(best_loop_lbl);
  label_release(loop_lbl);
}

static void jal_string_allocate_emit(pfile_t *pf, value_t dst, value_t len,
    label_t fail_lbl, label_t have_storage_lbl)
{
  jal_string_allocate_emit_ex(pf, dst, len, fail_lbl, have_storage_lbl,
      BOOLEAN_TRUE, 0, 0);
}

static value_t jal_string_source_length_snapshot_get(pfile_t *pf, value_t src)
{
  value_t len;
  value_t snap;

  if (jal_string_is_dynamic(src)) {
    len = jal_string_length_value_get(pf, src);
  } else {
    len = jal_string_index_const_get(pf, value_ct_get(src));
  }
  snap = jal_string_temp_index_get(pf);
  jal_string_assign_value(pf, snap, len);
  value_release(len);
  return snap;
}

value_t jal_string_relation_parse_emit(pfile_t *pf, value_t left,
    operator_t op)
{
  value_t right;
  value_t left_len;
  value_t right_len;
  value_t result;
  value_t idx;
  value_t one;
  value_t zero;
  label_t mismatch_lbl;
  label_t done_lbl;
  label_t loop_lbl;

  if (!jal_string_is_source(left)
      || ((OPERATOR_EQ != op) && (OPERATOR_NE != op))) {
    return VALUE_NONE;
  }
  right = jal_string_parse_operand(pf);
  if (!right) {
    return VALUE_NONE;
  }
  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "&")) {
    /* `greeting == "a" & name` — the comparison operand is a chain;
     * evaluate it into a heap cell first (the chain takes ownership
     * of `right`).  Relations whose LEFT is not a string never get
     * here, so the `cond1 & cond2` idiom keeps its ANDB parse. */
    value_t temp;

    temp = jal_string_concat_chain_emit(pf, right);
    right = temp;
  }
  if (!right) {
    return VALUE_NONE;
  }
  left_len = jal_string_source_length_snapshot_get(pf, left);
  right_len = jal_string_source_length_snapshot_get(pf, right);
  result = pfile_value_temp_get(pf, VARIABLE_DEF_TYPE_BOOLEAN, 1);
  idx = jal_string_temp_index_get(pf);
  one = jal_string_const_get(pf, 1);
  zero = jal_string_const_get(pf, 0);
  mismatch_lbl = pfile_label_alloc(pf, 0);
  done_lbl = pfile_label_alloc(pf, 0);
  loop_lbl = pfile_label_alloc(pf, 0);

  jal_string_assign_value(pf, result,
      (OPERATOR_EQ == op) ? one : zero);
  jal_string_branch_if_value_cmp(pf, OPERATOR_NE, left_len, right_len,
      CMD_BRANCHCOND_TRUE, mismatch_lbl);
  jal_string_assign_value(pf, idx, zero);

  pfile_cmd_label_add(pf, loop_lbl);
  jal_string_branch_if_value_cmp(pf, OPERATOR_GE, idx, left_len,
      CMD_BRANCHCOND_TRUE, done_lbl);
  {
    value_t left_byte;
    value_t right_byte;

    left_byte = jal_string_source_byte_get(pf, left, idx);
    right_byte = jal_string_source_byte_get(pf, right, idx);
    jal_string_branch_if_value_cmp(pf, OPERATOR_NE, left_byte, right_byte,
        CMD_BRANCHCOND_TRUE, mismatch_lbl);
    value_release(right_byte);
    value_release(left_byte);
  }
  pfile_cmd_op_add(pf, OPERATOR_ADD, &idx, idx, one);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
      loop_lbl, VALUE_NONE, VALUE_NONE, 0);

  pfile_cmd_label_add(pf, mismatch_lbl);
  jal_string_assign_value(pf, result,
      (OPERATOR_EQ == op) ? zero : one);
  pfile_cmd_label_add(pf, done_lbl);

  label_release(loop_lbl);
  label_release(done_lbl);
  label_release(mismatch_lbl);
  value_release(zero);
  value_release(one);
  value_release(idx);
  value_release(right_len);
  value_release(left_len);
  value_release(right);
  return result;
}

static boolean_t jal_string_value_const_baseofs_get(value_t val,
    variable_const_t *baseofs_out)
{
  value_t baseofs;

  if (!val || !baseofs_out) {
    return BOOLEAN_FALSE;
  }
  baseofs = value_baseofs_get(val);
  if (!baseofs) {
    *baseofs_out = 0;
    return BOOLEAN_TRUE;
  }
  if (!value_is_const(baseofs)) {
    return BOOLEAN_FALSE;
  }
  *baseofs_out = value_const_get(baseofs);
  return BOOLEAN_TRUE;
}

/*
 * True when `pvar` is a pointer (or array-of-pointer slots) whose last
 * compatible `is` binding targets the same `var string` backing as `heap_var`,
 * using `p_side_val` for constant slot index when `pvar` is an array.
 */
static boolean_t jal_string_pointer_bind_targets_var(variable_t pvar,
    variable_t heap_var, value_t p_side_val)
{
  value_t          wt;
  variable_const_t slot_ofs;

  if (!pvar || !heap_var) {
    return BOOLEAN_FALSE;
  }
  if (variable_is_pointer(pvar)) {
    wt = variable_pointer_whereis_target_get(pvar);
    return wt && value_variable_get(wt) == heap_var;
  }
  if (variable_is_array(pvar)
      && jal_string_value_const_baseofs_get(p_side_val, &slot_ofs)) {
    wt = variable_pointer_whereis_slot_target_get(pvar, slot_ofs);
    return wt && value_variable_get(wt) == heap_var;
  }
  return BOOLEAN_FALSE;
}

/* Same heap `var string` backing: direct `a == a`, or `*p` vs `a` after `p is a`. */
static boolean_t jal_string_same_dynamic_storage(value_t dst, value_t src)
{
  variable_t dv;
  variable_t sv;

  if (!jal_string_is_dynamic(dst) || !jal_string_is_dynamic(src)) {
    return BOOLEAN_FALSE;
  }
  dv = value_variable_get(dst);
  sv = value_variable_get(src);
  if (!dv || !sv) {
    return BOOLEAN_FALSE;
  }
  if (dv == sv) {
    return BOOLEAN_TRUE;
  }
  if (jal_string_pointer_bind_targets_var(dv, sv, dst)) {
    return BOOLEAN_TRUE;
  }
  if (jal_string_pointer_bind_targets_var(sv, dv, src)) {
    return BOOLEAN_TRUE;
  }
  return BOOLEAN_FALSE;
}

static void jal_string_concat_emit(pfile_t *pf, value_t dst,
    value_t *items, value_t *lens, size_t item_ct)
{
  label_t fail_lbl;
  label_t copy_lbl;
  label_t clear_lbl;
  label_t done_lbl;
  value_t total_len;
  value_t offset;
  value_t err;
  value_t zero;
  value_t heap_full;
  value_t old_off;
  value_t old_cap;
  size_t ii;

  jal_string_dynamic_dst_through_pointer_maybe(dst);
  if (!jal_string_runtime_ensure(pf)) {
    return;
  }
  fail_lbl = pfile_label_alloc(pf, 0);
  copy_lbl = pfile_label_alloc(pf, 0);
  clear_lbl = pfile_label_alloc(pf, 0);
  done_lbl = pfile_label_alloc(pf, 0);
  total_len = jal_string_temp_index_get(pf);
  offset = jal_string_temp_index_get(pf);
  err = jal_string_named_value_get(pf, JAL_STRING_ERROR_NAME);
  zero = jal_string_const_get(pf, JAL_STRING_ERROR_NONE);
  heap_full = jal_string_const_get(pf, JAL_STRING_ERROR_HEAP_FULL);
  old_off = VALUE_NONE;
  old_cap = VALUE_NONE;

  jal_string_assign_value(pf, err, zero);
  jal_string_assign_value(pf, total_len, zero);
  for (ii = 0; ii < item_ct; ii++) {
    pfile_cmd_op_add(pf, OPERATOR_ADD, &total_len, total_len, lens[ii]);
  }

  /* Option A heap-sizing diagnostic: the pcode total rides runtime ADDs
   * and snapshot temps even when every operand is statically sized --
   * recover each operand's static length here (const len value, const
   * source count, or a dynamic source's propagated table bound); any
   * operand without one skips the report */
  {
    variable_const_t static_total;
    boolean_t        all_const;

    all_const = (item_ct > 0);
    static_total = 0;
    for (ii = 0; ii < item_ct; ii++) {
      if (value_is_const(lens[ii])) {
        static_total += value_const_get(lens[ii]);
      } else if (value_is_const(items[ii]) && value_is_array(items[ii])) {
        /* const source (literal, const number render): exact static
         * count -- a NON-const non-dynamic array is a runtime scratch
         * (rendered number buffer) whose capacity is not a length */
        static_total += value_ct_get(items[ii]);
      } else {
        variable_t ivar;
        size_t     jj;

        ivar = value_variable_get(items[ii]);
        for (jj = 0; ivar && (jj < jal_string_state.heap_use_used); jj++) {
          if (jal_string_state.heap_use[jj].var == ivar) {
            static_total += jal_string_state.heap_use[jj].max_len;
            break;
          }
        }
        if (jj == jal_string_state.heap_use_used) {
          all_const = BOOLEAN_FALSE;
          break;
        }
      }
    }
    if (all_const) {
      value_t cv;

      cv = jal_string_const_get(pf, static_total);
      jal_string_heap_use_note(dst, cv);
      value_release(cv);
    } else {
      jal_string_heap_use_note(dst, total_len);
    }
  }

  jal_string_branch_if_const_cmp(pf, OPERATOR_EQ, total_len, 0,
      CMD_BRANCHCOND_TRUE, clear_lbl);
  jal_string_allocate_emit_ex(pf, dst, total_len, fail_lbl, copy_lbl,
      BOOLEAN_FALSE, &old_off, &old_cap);

  pfile_cmd_label_add(pf, copy_lbl);
  {
    value_t len_store;
    size_t  jj;

    len_store = jal_string_temp_index_get(pf);
    jal_string_assign_value(pf, len_store, zero);
    for (jj = 0; jj < item_ct; jj++) {
      pfile_cmd_op_add(pf, OPERATOR_ADD, &len_store, len_store, lens[jj]);
    }
    jal_string_handle_index_set(pf, dst, JAL_STRING_FIELD_LENGTH, len_store);
    value_release(len_store);
  }
  jal_string_assign_value(pf, offset, zero);
  for (ii = 0; ii < item_ct; ii++) {
    /* After allocate, dst's handle points at new storage; an operand that is
     * the same heap string (e.g. `p = p & "c"` after `p is a`) must be read
     * from old_off, not via jal_string_source_byte_get (which would use the
     * new handle). Same idea as src_is_dst in jal_string_append_truncate_emit.
     */
    if (jal_string_same_dynamic_storage(dst, items[ii])) {
      jal_string_copy_heap_at_emit(pf, dst, old_off, lens[ii], offset);
    } else {
      jal_string_copy_at_emit(pf, dst, items[ii], lens[ii], offset);
    }
    pfile_cmd_op_add(pf, OPERATOR_ADD, &offset, offset, lens[ii]);
  }
  if (old_off && old_cap) {
    jal_string_free_old_block_emit(pf, old_off, old_cap);
  }
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
      done_lbl, VALUE_NONE, VALUE_NONE, 0);

  pfile_cmd_label_add(pf, clear_lbl);
  jal_string_clear_emit(pf, dst);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
      done_lbl, VALUE_NONE, VALUE_NONE, 0);

  pfile_cmd_label_add(pf, fail_lbl);
  jal_string_assign_value(pf, err, heap_full);
  pfile_cmd_label_add(pf, done_lbl);

  value_release(old_cap);
  value_release(old_off);
  value_release(heap_full);
  value_release(zero);
  value_release(err);
  value_release(offset);
  value_release(total_len);
  label_release(done_lbl);
  label_release(clear_lbl);
  label_release(copy_lbl);
  label_release(fail_lbl);
}

static value_t jal_string_parse_operand(pfile_t *pf)
{
  value_t   val;
  boolean_t neg_lit;

  neg_lit = BOOLEAN_FALSE;
  if (jal_token_is_identifier(pf)) {
    val = jal_parse_value(pf, JAL_PARSE_VALUE_TYPE_RVALUE);
  } else {
    if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "-")) {
      neg_lit = BOOLEAN_TRUE;
      pf_token_get(pf, PF_TOKEN_NEXT);
    }
    if (neg_lit) {
      /* negative literal: fold the minus onto the constant at parse
       * time (a runtime SUB op would not be const when the resolver
       * runs) -- a SIGNED UNIVERSAL constant renders "-N" */
      variable_def_t sdef;
      variable_const_t n;

      val = jal_token_to_constant(pf, PFILE_LOG_ERR);
      if (val) {
        pf_token_get(pf, PF_TOKEN_NEXT);
        if (VARIABLE_DEF_TYPE_FLOAT == value_type_get(val)) {
          /* negative float literal: negate the float constant */
          float nf;

          nf = -value_const_float_get(val);
          value_release(val);
          val = pfile_constant_float_get(pf, nf, value_def_get(val));
          if (!val) {
            pfile_log_syserr(pf, ENOMEM);
          }
        } else {
          n = value_const_get(val);
          n = (~n) + 1U;
          value_release(val);
          sdef = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
              VARIABLE_DEF_FLAG_UNIVERSAL | VARIABLE_DEF_FLAG_CONST
                | VARIABLE_DEF_FLAG_SIGNED,
              16);
          val = sdef ? pfile_constant_get(pf, n, sdef) : VALUE_NONE;
          if (!val) {
            pfile_log_syserr(pf, ENOMEM);
          }
        }
      }
    } else {
      val = jal_token_to_constant(pf, PFILE_LOG_ERR);
      if (val) {
        pf_token_get(pf, PF_TOKEN_NEXT);
      }
    }
  }
  /* arrays (bare or string-flagged) pass to the chain resolver: byte
   * arrays become verbatim sources there, other element types get the
   * explanatory error */
  if (val && !jal_string_is_source(val)
      && !jal_string_is_numeric_value(val)
      && !value_is_array(val)) {
    pfile_log(pf, PFILE_LOG_ERR, "string value expected");
    value_release(val);
    val = VALUE_NONE;
  }
  return val;
}

/* Integer scalars/constants (stage 2a/2b: byte..oword, signed and
 * unsigned).  Bits, arrays and floats are not numeric operands here
 * (floats arrive with stage 2d). */
static boolean_t jal_string_is_numeric_value(value_t val)
{
  variable_def_t def;

  if (!val || value_is_bit(val)) {
    return BOOLEAN_FALSE;
  }
  def = value_def_get(val);
  return (def
      && ((VARIABLE_DEF_TYPE_INTEGER == variable_def_type_get(def))
        || (VARIABLE_DEF_TYPE_FLOAT == variable_def_type_get(def))))
    ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

static value_t jal_string_number_source_emit_ex(pfile_t *pf, value_t v,
    value_t *len_out, unsigned base, unsigned pad);
static value_t jal_string_text_const(pfile_t *pf, const char *pfx,
    const char *txt, unsigned len);
static value_t jal_string_const_int_text(pfile_t *pf, variable_const_t cn,
    unsigned neg, variable_sz_t sz, unsigned base, unsigned width);
static value_t jal_string_jal_proc_call_value_get(pfile_t *pf,
    const char *tag);
#define JAL_STRING_FLTBUF_NAME_SZ 24

/* stage 2d v2: runtime float operand -> CALL _string_float_str(f, buf)
 * (strings_dynamic' battery-validated renderer). Mirrors
 * jal_parse_call's conventions exactly: parms[0] is the OUT-only
 * return temp (the rendered text length), parms[1] the float IN
 * actual, parms[2] a persistent 48-byte compiler buffer bound to the
 * fixed-size OUT array formal (the call marshalling copies the bytes
 * back). The chain item is the buffer; len_out is the return temp --
 * the same (array, runtime length) shape the number machinery
 * produces. The parms array is owned by the emitted cmd. */
static value_t jal_string_float_runtime_source_emit(pfile_t *pf,
    value_t *len_out, value_t v)
{
  static unsigned  flt_no;
  char             name[JAL_STRING_FLTBUF_NAME_SZ];
  variable_def_t   byte_def;
  variable_def_t   arr_def;
  variable_t       scratch_var;
  value_t         *parms;
  value_t          call_val;
  value_t          scratch;

  if (!jal_string_runtime_ensure(pf)) {
    value_release(v);
    return VALUE_NONE;
  }
  sprintf(name, "_string_flt%u", flt_no);
  flt_no++;
  byte_def = jal_string_byte_def_alloc(VARIABLE_DEF_FLAG_NONE);
  arr_def = variable_def_alloc(0, VARIABLE_DEF_TYPE_ARRAY,
      VARIABLE_DEF_FLAG_NONE, 0);
  scratch = VALUE_NONE;
  if (!byte_def || !arr_def) {
    if (byte_def) {
      variable_def_free(byte_def);
    }
    if (arr_def) {
      variable_def_free(arr_def);
    }
    value_release(v);
    return VALUE_NONE;
  }
  if ((RESULT_OK != variable_def_member_add(arr_def, 0, byte_def, 48U))
      || (RESULT_OK != pfile_variable_alloc(pf,
          PFILE_VARIABLE_ALLOC_GLOBAL, name, arr_def, VARIABLE_NONE,
          &scratch_var))) {
    variable_def_free(arr_def);
    value_release(v);
    return VALUE_NONE;
  }
  scratch = value_alloc(scratch_var);
  variable_release(scratch_var);
  call_val = jal_string_jal_proc_call_value_get(pf, "_string_float_str");
  if (!call_val || !scratch) {
    if (!call_val) {
      pfile_log(pf, PFILE_LOG_ERR,
          "float renderer _string_float_str not found "
          "(include strings_dynamic)");
    }
    value_release(scratch);
    value_release(v);
    return VALUE_NONE;
  }
  parms = CALLOC(3U, sizeof(*parms));
  if (!parms) {
    pfile_log_syserr(pf, ENOMEM);
    value_release(scratch);
    value_release(v);
    return VALUE_NONE;
  }
  parms[0] = pfile_value_temp_get(pf, VARIABLE_DEF_TYPE_INTEGER, 1);
  /* the emitted cmd consumes one reference per parm value (ROM-A
   * convention): lock ours so the caller's release stays balanced */
  value_lock(v);
  parms[1] = v;
  value_lock(scratch);
  parms[2] = scratch;
  if (!parms[0]) {
    FREE(parms);
    value_release(scratch);
    value_release(v);
    return VALUE_NONE;
  }
  /* the emitted cmd owns parms (jal_parse_call's convention) */
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_CALL, CMD_BRANCHCOND_NONE,
      LABEL_NONE, VALUE_NONE, call_val, parms);
  *len_out = parms[0];
  value_lock(parms[0]);
  return scratch;
}

static value_t jal_string_number_source_emit(pfile_t *pf, value_t v,
    value_t *len_out)
{
  return jal_string_number_source_emit_ex(pf, v, len_out, 10U, 0U);
}

/* Bare byte-array chain operand (2026-10-06): `s & arr` concatenates the
 * array's bytes verbatim -- the same semantics as string(arr) and
 * string_from_array.  Const arrays fold to a flash string; runtime arrays
 * become one chain source by STRING-flagging a dup of the array def
 * (length = element count, a constant).  BYTE elements only; other
 * element types keep the explicit string()/per-element forms. */
static value_t jal_string_array_chain_source(pfile_t *pf, value_t item,
    value_t *len)
{
  variable_def_t        odef;
  variable_def_member_t mbr;
  variable_def_t        inner;

  *len = VALUE_NONE;
  odef = value_def_get(item);
  mbr  = variable_def_member_get(odef);
  inner = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
  if (!inner
      || (VARIABLE_DEF_TYPE_INTEGER != variable_def_type_get(inner))
      || (1 != variable_def_sz_get(inner))
      || variable_def_flag_test(inner, VARIABLE_DEF_FLAG_BIT)) {
    pfile_log(pf, PFILE_LOG_ERR,
        "byte array expected -- `&` concatenates BYTE arrays verbatim;"
        " use string() or a per-element render for other element types");
    value_release(item);
    return VALUE_NONE;
  }
  if (value_is_const(item)) {
    variable_t   ovar;
    const char  *data;
    variable_ct_t ct;
    value_t      rc;

    ovar = value_variable_get(item);
    data = ovar ? variable_data_get(ovar) : 0;
    ct   = mbr ? variable_def_member_ct_get(mbr) : 0;
    if (!data || !ct || (ct > 64U)) {
      pfile_log(pf, PFILE_LOG_ERR,
          "const array chain operand too large (limit 64 elements)");
      value_release(item);
      return VALUE_NONE;
    }
    rc = jal_string_text_const(pf, "chain", data, (unsigned) ct);
    value_release(item);
    item = rc;
  } else {
    variable_def_t sdef;

    sdef = variable_def_dup(odef);
    if (sdef) {
      sdef = variable_def_flags_change(sdef,
          variable_def_flags_get_all(sdef) | VARIABLE_DEF_FLAG_STRING);
    }
    if (!sdef) {
      pfile_log_syserr(pf, ENOMEM);
      value_release(item);
      return VALUE_NONE;
    }
    value_def_set(item, sdef);
  }
  if (!item) {
    return VALUE_NONE;
  }
  *len = jal_string_source_length_snapshot_get(pf, item);
  return item;
}

/* Stage 2a/2b resolve: a chain item that parsed as a numeric scalar is
 * converted to a string source (compile-time for constants).  Takes
 * ownership of `item`, returns the resolved item and sets `len`. */
static value_t jal_string_chain_item_resolve(pfile_t *pf, value_t item,
    value_t *len)
{
  value_t conv;
  value_t nlen;

  if (item && !jal_string_is_source(item)) {
    if (value_is_array(item)) {
      item = jal_string_array_chain_source(pf, item, len);
      return item;
    }
    nlen = VALUE_NONE;
    conv = jal_string_number_source_emit(pf, item, &nlen);
    value_release(item);
    if (!conv) {
      pfile_log(pf, PFILE_LOG_ERR, "string value expected");
      return VALUE_NONE;
    }
    item = conv;
    if (nlen) {
      *len = nlen;
      return item;
    }
  }
  *len = jal_string_source_length_snapshot_get(pf, item);
  return item;
}

#define JAL_STRING_CHAIN_NAME_SZ 24

/* One compiler-generated heap-string cell per chain site (`_string_chainN`,
 * like `_cstrN`): the chain result must keep its STRING def flags (the
 * proc-temp machinery strips them) and must not alias other live chains
 * within the same statement.  Statement-atomic evaluation makes a global
 * cell safe with tasks (no `suspend` inside an expression). */
static value_t jal_string_chain_var_get(pfile_t *pf)
{
  static unsigned chain_no;
  char            name[JAL_STRING_CHAIN_NAME_SZ];
  variable_def_t  vdef;
  variable_t      var;
  value_t         val;

  sprintf(name, "_string_chain%u", chain_no);
  chain_no++;
  val = pfile_value_find(pf, PFILE_LOG_NONE, name);
  if (val) {
    return val;
  }
  vdef = jal_string_dynamic_def_alloc(pf);
  if (!vdef) {
    return VALUE_NONE;
  }
  /* pfile_variable_alloc takes ownership of the def on success */
  if (RESULT_OK != pfile_variable_alloc(pf, PFILE_VARIABLE_ALLOC_GLOBAL,
      name, vdef, VARIABLE_NONE, &var)) {
    variable_def_free(vdef);
    return VALUE_NONE;
  }
  val = value_alloc(var);
  variable_release(var);
  return val;
}

/* ---- stage 2a/2b: integer operands in `&` chains -------------------- */

#define JAL_STRING_NUMBUF_NAME_SZ 24

/* worst-case decimal digits (3/byte covers byte..oword) + sign + slack */
static unsigned jal_string_num_dig_max(variable_sz_t sz)
{
  return (unsigned) sz * 3U + 2U;
}

/* Render a UNIVERSAL constant as decimal text (compile time); `neg`
 * prepends '-' for negative signed constants. */

static value_t jal_string_number_const_source_emit(pfile_t *pf,
    variable_const_t cn, unsigned neg)
{
  char            name[JAL_STRING_NUMBUF_NAME_SZ];
  char            digits[44];
  unsigned        ndig;
  unsigned        ii;
  variable_def_t  byte_def;
  variable_def_t  arr_def;
  variable_t      var;
  value_t         val;

  ndig = 0;
  do {
    digits[ndig++] = (char) ('0' + (int) (cn % 10U));
    cn /= 10U;
  } while (cn != 0);
  if (neg) {
    digits[ndig++] = '-';
  }
  /* digits (and any '-') were collected LSB-first: always reverse */
  for (ii = 0; ii < (ndig / 2U); ii++) {
    char tmpc = digits[ii];
    digits[ii] = digits[ndig - 1U - ii];
    digits[ndig - 1U - ii] = tmpc;
  }
  sprintf(name, "_string_dec%u", jal_string_state.dec_no_shared);
  jal_string_state.dec_no_shared++;
  byte_def = jal_string_byte_def_alloc(VARIABLE_DEF_FLAG_CONST);
  arr_def = variable_def_alloc(0, VARIABLE_DEF_TYPE_ARRAY,
      VARIABLE_DEF_FLAG_CONST | VARIABLE_DEF_FLAG_STRING, 0);
  val = VALUE_NONE;
  if (!byte_def || !arr_def) {
    if (byte_def) {
      variable_def_free(byte_def);
    }
    if (arr_def) {
      variable_def_free(arr_def);
    }
    return VALUE_NONE;
  }
  if ((RESULT_OK != variable_def_member_add(arr_def, 0, byte_def,
          (variable_ct_t) ndig))
      || (RESULT_OK != pfile_variable_alloc(pf,
          PFILE_VARIABLE_ALLOC_GLOBAL, name, arr_def, VARIABLE_NONE,
          &var))) {
    variable_def_free(arr_def);
    return VALUE_NONE;
  }
  /* pfile_variable_alloc took ownership of arr_def */
  for (ii = 0; ii < ndig; ii++) {
    variable_const_set(var, arr_def, ii,
        (variable_const_t) (unsigned char) digits[ii]);
  }
  val = value_alloc(var);
  variable_release(var);
  return val;
}

/* Render a float constant as shortest-round-trip decimal text (compile
 * time): the shortest %.<p>g spelling that reads back to the same
 * IEEE754 binary32 value (<= 9 significant digits always suffices).
 * Stage 2d v1 covers CONSTANTS; runtime float variables are a clean
 * compile error until the runtime renderer lands. */
static value_t jal_string_float_const_source_emit(pfile_t *pf, float f)
{
  char            buf[40];
  char            digits[40];
  const char     *txt;
  unsigned        len;
  unsigned        ii;
  int             prec;
  boolean_t       neg;
  variable_def_t  byte_def;
  variable_def_t  arr_def;
  variable_t      var;
  value_t         val;

  txt = buf;
  if (f != f) {
    txt = "nan";
  } else if ((f > 3.402823466e+38f) || (f < -3.402823466e+38f)) {
    neg = (f < 0.0f) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
    txt = neg ? "-inf" : "inf";
  } else {
    char *ep;

    for (prec = 1; prec <= 9; prec++) {
      sprintf(buf, "%.*g", prec, (double) f);
      if ((float) atof(buf) == f) {
        break;
      }
    }
    if (prec > 9) {
      /* cannot happen for binary32, but stay safe */
      sprintf(buf, "%.9g", (double) f);
    }
    /* prefer plain decimal text when the %g form went exponential:
     * re-render with fixed decimals (e.g. 100 -> "1e+02" -> "100",
     * 1e-8 -> "1e-08" -> "0.00000001"), trimmed of trailing zeros */
    ep = strchr(buf, 'e');
    if (ep) {
      long e10;
      long frac;
      char fix[44];
      char *dot;

      e10 = atol(ep + 1);
      frac = (long) prec - 1 - e10;
      if (frac < 0) {
        frac = 0;
      }
      if (frac <= 17) {
        sprintf(fix, "%.*f", (int) frac, (double) f);
        if ((float) atof(fix) == f) {
          dot = strchr(fix, '.');
          if (dot) {
            while ((dot < fix + strlen(fix) - 1)
                && ('0' == fix[strlen(fix) - 1])) {
              fix[strlen(fix) - 1] = 0;
            }
            if (dot == fix + strlen(fix) - 1) {
              fix[strlen(fix) - 1] = 0;
            }
          }
          if (strlen(fix) < sizeof(buf)) {
            strcpy(buf, fix);
          }
        }
      }
    }
  }
  len = (unsigned) strlen(txt);
  if (!len || (len >= sizeof(digits))) {
    return VALUE_NONE;
  }
  for (ii = 0; ii < len; ii++) {
    digits[ii] = txt[ii];
  }

  sprintf(buf, "_string_dec%u", jal_string_state.dec_no_shared++);
  byte_def = jal_string_byte_def_alloc(VARIABLE_DEF_FLAG_CONST);
  arr_def = variable_def_alloc(0, VARIABLE_DEF_TYPE_ARRAY,
      VARIABLE_DEF_FLAG_CONST | VARIABLE_DEF_FLAG_STRING, 0);
  val = VALUE_NONE;
  if (!byte_def || !arr_def) {
    if (byte_def) {
      variable_def_free(byte_def);
    }
    if (arr_def) {
      variable_def_free(arr_def);
    }
    return VALUE_NONE;
  }
  if ((RESULT_OK != variable_def_member_add(arr_def, 0, byte_def,
          (variable_ct_t) len))
      || (RESULT_OK != pfile_variable_alloc(pf,
          PFILE_VARIABLE_ALLOC_GLOBAL, buf, arr_def, VARIABLE_NONE,
          &var))) {
    variable_def_free(arr_def);
    return VALUE_NONE;
  }
  for (ii = 0; ii < len; ii++) {
    variable_const_set(var, arr_def, ii,
        (variable_const_t) (unsigned char) digits[ii]);
  }
  val = value_alloc(var);
  variable_release(var);
  return val;
}

/* `value_t` for a JAL procedure/function symbol (used only as CALL target). */
static pfile_proc_t *jal_string_proc_find_deep(pfile_proc_t *root,
    const char *tag)
{
  pfile_proc_t *proc;
  pfile_proc_t *found;

  for (proc = pfile_proc_child_get(root); proc;
      proc = pfile_proc_sibbling_get(proc)) {
    if (!strcmp(pfile_proc_tag_get(proc), tag)) {
      return proc;
    }
    found = jal_string_proc_find_deep(proc, tag);
    if (found) {
      return found;
    }
  }
  return 0;
}

static value_t jal_string_jal_proc_call_value_get(pfile_t *pf, const char *tag)
{
  pfile_proc_t  *proc;
  pfile_block_t *blk;
  variable_t     func_var;
  value_t        val;
  result_t       rc;

  proc = pfile_proc_find(pf, PFILE_LOG_NONE, tag);
  if (!proc) {
    /* pfile_proc_find walks from the ACTIVE proc up; at file scope
     * (chain parse in the main block) nothing is active -- search the
     * whole tree from the root instead */
    proc = jal_string_proc_find_deep(pf->proc_root, tag);
  }
  if (!proc) {
    return VALUE_NONE;
  }
  blk = pfile_proc_block_active_get(pfile_proc_active_get(pf));
  if (!blk) {
    return VALUE_NONE;
  }
  /* one proc-binding variable per tag: re-allocation on a second call
   * returns RESULT_EXISTS */
  func_var = pfile_variable_find(pf, PFILE_LOG_NONE, tag, 0);
  if (!func_var) {
    rc = pfile_block_variable_alloc(blk, pfile_tag_alloc(pf, tag),
        pfile_proc_def_get(proc), VARIABLE_NONE, pf, &func_var);
    if (RESULT_OK != rc || !func_var) {
      return VALUE_NONE;
    }
  } else {
    variable_lock(func_var);
  }
  variable_proc_set(func_var, proc);
  val = value_alloc(func_var);
  variable_release(func_var);
  return val;
}


/* unnamed byte-array scratch variable (`_string_numN`) */
static boolean_t jal_string_num_scratch_emit(pfile_t *pf, const char *name,
    variable_ct_t ct, value_t *val)
{
  variable_def_t  byte_def;
  variable_def_t  arr_def;
  variable_t      var;

  byte_def = jal_string_byte_def_alloc(VARIABLE_DEF_FLAG_NONE);
  arr_def = variable_def_alloc(0, VARIABLE_DEF_TYPE_ARRAY,
      VARIABLE_DEF_FLAG_NONE, 0);
  if (!byte_def || !arr_def) {
    if (byte_def) {
      variable_def_free(byte_def);
    }
    if (arr_def) {
      variable_def_free(arr_def);
    }
    return BOOLEAN_FALSE;
  }
  if ((RESULT_OK != variable_def_member_add(arr_def, 0, byte_def, ct))
      || (RESULT_OK != pfile_variable_alloc(pf,
          PFILE_VARIABLE_ALLOC_GLOBAL, name, arr_def, VARIABLE_NONE,
          &var))) {
    variable_def_free(arr_def);
    return BOOLEAN_FALSE;
  }
  *val = value_alloc(var);
  variable_release(var);
  return BOOLEAN_TRUE;
}

/* Render an integer operand (any width, signed or unsigned) as decimal
 * digits.  Constants fold at compile time; runtime values use ONE
 * per-site scratch array whose loop state lives at fixed constant
 * offsets (proc-temp slots all alias one storage cell, so loop-carried
 * state must live in real variables):
 *
 *   [0 .. maxdig-1]            output: '-' at [0] when negative,
 *                              digits after it
 *   [maxdig .. 2*maxdig-1]     digits, extracted LSB-first
 *   [2*maxdig .. +sz-1]        magnitude working copy (UNSIGNED def --
 *                              a fresh def; flags_change on the live
 *                              operand def would hollow it)
 *   then len, j, k (index-size), d, ch, neg (1 byte each)
 *
 * Returns the scratch array as the chain item with its runtime length
 * in *len_out; VALUE_NONE for non-integer operands (floats: 2d). */
static value_t jal_string_number_source_emit_ex(pfile_t *pf, value_t v,
    value_t *len_out, unsigned base, unsigned pad)
{
  static unsigned   num_no;
  char              name[JAL_STRING_NUMBUF_NAME_SZ];
  variable_def_t    def;
  variable_def_t    udef;
  variable_sz_t     sz;
  variable_sz_t     isz;
  unsigned          maxdig;
  unsigned          ofs_tmpv;
  unsigned          ofs_len;
  unsigned          ofs_j;
  unsigned          ofs_k;
  unsigned          ofs_d;
  unsigned          ofs_ch;
  unsigned          ofs_neg;
  unsigned          total;
  variable_const_t  cn;
  unsigned          neg;
  boolean_t         is_signed;
  value_t           scratch;
  value_t           tmpv;
  value_t           len_v;
  value_t           j_v;
  value_t           k_v;
  value_t           d_v;
  value_t           ch_v;
  value_t           neg_v;
  value_t           ten;
  value_t           one;
  value_t           c45;
  value_t           c48;
  value_t           zero;
  value_t           nine;
  value_t           c39;
  value_t           padv;
  label_t           lbl_dg;
  label_t           lbl_pad;
  label_t           lbl_padend;
  label_t           lbl1;
  label_t           lbl2;
  label_t           lbl_sgn;
  label_t           lbl_nm;
  value_t           elem;
  value_t           item;
  value_t           t;
  flag_t            warn_saved;

  warn_saved = 0;
  scratch = VALUE_NONE;
  tmpv = VALUE_NONE;
  len_v = VALUE_NONE;
  j_v = VALUE_NONE;
  k_v = VALUE_NONE;
  d_v = VALUE_NONE;
  ch_v = VALUE_NONE;
  neg_v = VALUE_NONE;
  nine = VALUE_NONE;
  c39 = VALUE_NONE;
  padv = VALUE_NONE;
  lbl_dg = LABEL_NONE;
  lbl_pad = LABEL_NONE;
  lbl_padend = LABEL_NONE;
  ten = VALUE_NONE;
  one = VALUE_NONE;
  c45 = VALUE_NONE;
  c48 = VALUE_NONE;
  zero = VALUE_NONE;
  lbl1 = LABEL_NONE;
  lbl2 = LABEL_NONE;
  lbl_sgn = LABEL_NONE;
  lbl_nm = LABEL_NONE;
  elem = VALUE_NONE;
  item = VALUE_NONE;
  t = VALUE_NONE;

  /* compile-time: constants render to a decimal const string */
  if (value_is_const(v)) {
    if (VARIABLE_DEF_TYPE_FLOAT == value_type_get(v)) {
      item = jal_string_float_const_source_emit(pf,
          value_const_float_get(v));
      if (item) {
        *len_out = VALUE_NONE;
      }
      return item;
    }
    cn = value_const_get(v);
    neg = 0U;
    if (value_is_signed(v)
        && (0 != (cn >> (VARIABLE_CONST_BIT_CT - 1)))) {
      cn = (~cn) + 1U;   /* magnitude of the two's-complement pattern */
      neg = 1U;
    }
    if ((10U != base) || pad) {
      variable_sz_t msz;
      value_t       mitem;

      msz = 1;
      while ((msz < 8U) && (0U != (cn >> (8U * (unsigned) msz)))) {
        msz++;
      }
      mitem = jal_string_const_int_text(pf, cn, neg, msz, base, pad);
      if (mitem) {
        *len_out = VALUE_NONE;
        return mitem;
      }
    }
    item = jal_string_number_const_source_emit(pf, cn, neg);
    if (item) {
      *len_out = VALUE_NONE;
    }
    return item;
  }

  def = value_def_get(v);
  if (def && (VARIABLE_DEF_TYPE_FLOAT == variable_def_type_get(def))) {
    return jal_string_float_runtime_source_emit(pf, len_out, v);
  }
  if (!def || (VARIABLE_DEF_TYPE_INTEGER != variable_def_type_get(def))
      || variable_def_flag_test(def, VARIABLE_DEF_FLAG_BIT)) {
    return VALUE_NONE;
  }
  sz = variable_def_sz_get(def);
  if ((sz < 1) || (sz > 16)) {
    return VALUE_NONE;
  }
  is_signed = variable_def_flag_test(def, VARIABLE_DEF_FLAG_SIGNED);
  maxdig = jal_string_num_dig_max(sz);
  isz = jal_string_current_index_size_get(pf);
  ofs_tmpv = 2 * maxdig;
  ofs_len = ofs_tmpv + sz;
  ofs_j = ofs_len + isz;
  ofs_k = ofs_j + isz;
  ofs_d = ofs_k + isz;
  ofs_ch = ofs_d + 1;
  ofs_neg = ofs_ch + 1;
  total = ofs_neg + 1;

  /* the reinterpret copy (unsigned tmpv = signed v) and the digit
   * clamps are compiler-internal lowering: suspend the conversion /
   * truncation warnings for this emitter and restore them after */
  warn_saved = pfile_flag_test(pf, PFILE_FLAG_WARN_CONVERSION)
    ? PFILE_FLAG_WARN_CONVERSION : 0UL;
  warn_saved |= pfile_flag_test(pf, PFILE_FLAG_WARN_TRUNCATE)
    ? PFILE_FLAG_WARN_TRUNCATE : 0UL;
  /* flag_change keeps bits in mask: clear = mask without the warn
   * bits, restore = OR the saved bits back over a keep-everything mask */
  pfile_flag_change(pf,
      ~(PFILE_FLAG_WARN_CONVERSION | PFILE_FLAG_WARN_TRUNCATE), 0);

  sprintf(name, "_string_num%u", num_no);
  num_no++;
  if (!jal_string_num_scratch_emit(pf, name, (variable_ct_t) total,
      &scratch)) {
    pfile_flag_change(pf, ~(flag_t) 0, warn_saved);
    return VALUE_NONE;
  }

  /* scalar slices at fixed constant offsets; tmpv is a FRESH unsigned
   * def of the same size (never the operand's def) */
  udef = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
      VARIABLE_DEF_FLAG_NONE, sz);
  tmpv = value_clone(scratch);
  len_v = value_clone(scratch);
  j_v = value_clone(scratch);
  k_v = value_clone(scratch);
  d_v = value_clone(scratch);
  ch_v = value_clone(scratch);
  neg_v = value_clone(scratch);
  if (tmpv) {
    value_def_set(tmpv, udef);
    value_baseofs_set(tmpv, jal_string_index_const_get(pf, ofs_tmpv));
  }
  if (len_v) {
    value_def_set(len_v, jal_string_index_def_alloc(pf,
        VARIABLE_DEF_FLAG_NONE));
    value_baseofs_set(len_v, jal_string_index_const_get(pf, ofs_len));
  }
  if (j_v) {
    value_def_set(j_v, jal_string_index_def_alloc(pf,
        VARIABLE_DEF_FLAG_NONE));
    value_baseofs_set(j_v, jal_string_index_const_get(pf, ofs_j));
  }
  if (k_v) {
    value_def_set(k_v, jal_string_index_def_alloc(pf,
        VARIABLE_DEF_FLAG_NONE));
    value_baseofs_set(k_v, jal_string_index_const_get(pf, ofs_k));
  }
  if (d_v) {
    value_def_set(d_v, jal_string_byte_def_alloc(
        VARIABLE_DEF_FLAG_NONE));
    value_baseofs_set(d_v, jal_string_index_const_get(pf, ofs_d));
  }
  if (ch_v) {
    value_def_set(ch_v, jal_string_byte_def_alloc(
        VARIABLE_DEF_FLAG_NONE));
    value_baseofs_set(ch_v, jal_string_index_const_get(pf, ofs_ch));
  }
  if (neg_v) {
    value_def_set(neg_v, jal_string_byte_def_alloc(
        VARIABLE_DEF_FLAG_NONE));
    value_baseofs_set(neg_v, jal_string_index_const_get(pf, ofs_neg));
  }
  /* index-sized constants: UNIVERSAL (16-byte) consts would route the
   * loop arithmetic through the wide ALU, whose runtime-indexed
   * destinations the ESP32 back end rejects */
  ten = jal_string_index_const_get(pf, (10U == base) ? 10U : base);
  one = jal_string_index_const_get(pf, 1);
  c45 = jal_string_index_const_get(pf, '-');
  c48 = jal_string_index_const_get(pf, '0');
  zero = jal_string_index_const_get(pf, 0);
  if (16U == base) {
    nine = jal_string_index_const_get(pf, 9);
    c39 = jal_string_index_const_get(pf, 39);
    lbl_dg = pfile_label_alloc(pf, 0);
  }
  if (pad) {
    padv = jal_string_index_const_get(pf, pad);
    lbl_pad = pfile_label_alloc(pf, 0);
    lbl_padend = pfile_label_alloc(pf, 0);
  }
  lbl1 = pfile_label_alloc(pf, 0);
  lbl2 = pfile_label_alloc(pf, 0);
  lbl_sgn = pfile_label_alloc(pf, 0);
  lbl_nm = pfile_label_alloc(pf, 0);
  if (!udef || !tmpv || !len_v || !j_v || !k_v || !d_v || !ch_v
      || !neg_v || !ten || !one || !c45 || !c48 || !zero
      || ((16U == base) && (!nine || !c39 || (LABEL_NONE == lbl_dg)))
      || (pad && (!padv || (LABEL_NONE == lbl_pad)
          || (LABEL_NONE == lbl_padend)))
      || (LABEL_NONE == lbl1) || (LABEL_NONE == lbl2)
      || (LABEL_NONE == lbl_sgn) || (LABEL_NONE == lbl_nm)) {
    goto numfail;
  }

  /* neg = 0; tmpv = v (bit copy); if signed & v < 0:
   *     neg = 1; tmpv = 0 - tmpv (unsigned; INT_MIN exact) */
  jal_string_assign_value(pf, neg_v, zero);
  jal_string_assign_value(pf, tmpv, v);
  if (is_signed) {
    /* branch over the negate block when v >= 0; the zero matches v's
     * signedness so the compare does not warn */
    value_t zero_s;
    variable_def_t zdef;

    zdef = variable_def_dup(def);
    zero_s = zdef ? pfile_constant_get(pf, 0, zdef) : VALUE_NONE;
    if (!zero_s) {
      goto numfail;
    }
    jal_string_branch_if_value_cmp(pf, OPERATOR_LT, v, zero_s,
        CMD_BRANCHCOND_FALSE, lbl_sgn);
    value_release(zero_s);
    jal_string_assign_value(pf, neg_v, one);
    t = VALUE_NONE;
    pfile_cmd_op_add(pf, OPERATOR_SUB, &t, zero, tmpv);
    jal_string_assign_value(pf, tmpv, t);
    value_release(t);
    pfile_cmd_label_add(pf, lbl_sgn);
  }
  jal_string_assign_value(pf, len_v, zero);

  /* digit extraction, LSB-first into scratch[maxdig + len] */
  pfile_cmd_label_add(pf, lbl1);
  t = VALUE_NONE;
  pfile_cmd_op_add(pf, OPERATOR_MOD, &t, tmpv, ten);
  jal_string_assign_value(pf, d_v, t);      /* clamp to 1 byte */
  value_release(t);
  t = VALUE_NONE;
  pfile_cmd_op_add(pf, OPERATOR_DIV, &t, tmpv, ten);
  jal_string_assign_value(pf, tmpv, t);
  value_release(t);
  t = VALUE_NONE;
  pfile_cmd_op_add(pf, OPERATOR_ADD, &t, d_v, c48);
  jal_string_assign_value(pf, ch_v, t);     /* clamp to 1 byte */
  value_release(t);
  if (16U == base) {
    /* hex digit: d in 10..15 renders 'a'..'f' (ch += 39 when d > 9) */
    jal_string_branch_if_value_cmp(pf, OPERATOR_LE, d_v, nine,
        CMD_BRANCHCOND_TRUE, lbl_dg);
    t = VALUE_NONE;
    pfile_cmd_op_add(pf, OPERATOR_ADD, &t, ch_v, c39);
    jal_string_assign_value(pf, ch_v, t);
    value_release(t);
    pfile_cmd_label_add(pf, lbl_dg);
  }
  {
    value_t byte_ofs;

    byte_ofs = jal_string_add_const(pf, len_v, maxdig);
    elem = jal_string_index_byte_lvalue_get(pf, scratch, byte_ofs);
    value_release(byte_ofs);
    if (!elem) {
      goto numfail;
    }
    jal_string_assign_value(pf, elem, ch_v);
    value_release(elem);
    elem = VALUE_NONE;
  }
  t = VALUE_NONE;
  pfile_cmd_op_add(pf, OPERATOR_ADD, &t, len_v, one);
  jal_string_assign_value(pf, len_v, t);
  value_release(t);
  jal_string_branch_if_value_cmp(pf, OPERATOR_NE, tmpv, zero,
      CMD_BRANCHCOND_TRUE, lbl1);

  if (pad) {
    /* zero-fill to the pad minimum: while (len < pad) digit[len] = '0' */
    pfile_cmd_label_add(pf, lbl_pad);
    jal_string_branch_if_value_cmp(pf, OPERATOR_GE, len_v, padv,
        CMD_BRANCHCOND_TRUE, lbl_padend);
    {
      value_t byte_ofs;

      byte_ofs = jal_string_add_const(pf, len_v, maxdig);
      elem = jal_string_index_byte_lvalue_get(pf, scratch, byte_ofs);
      value_release(byte_ofs);
      if (!elem) {
        goto numfail;
      }
      jal_string_assign_value(pf, elem, c48);
      value_release(elem);
      elem = VALUE_NONE;
    }
    t = VALUE_NONE;
    pfile_cmd_op_add(pf, OPERATOR_ADD, &t, len_v, one);
    jal_string_assign_value(pf, len_v, t);
    value_release(t);
    pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
        lbl_pad, VALUE_NONE, VALUE_NONE, 0);
    pfile_cmd_label_add(pf, lbl_padend);
  }

  /* '-' at [0] when negative (digits land at [neg..]) */
  if (is_signed) {
    jal_string_branch_if_value_cmp(pf, OPERATOR_EQ, neg_v, zero,
        CMD_BRANCHCOND_TRUE, lbl_nm);
    elem = jal_string_index_byte_lvalue_get(pf, scratch, zero);
    if (!elem) {
      goto numfail;
    }
    jal_string_assign_value(pf, elem, c45);
    value_release(elem);
    elem = VALUE_NONE;
    pfile_cmd_label_add(pf, lbl_nm);
  }

  /* reverse scratch[maxdig + k] -> scratch[j + neg] */
  jal_string_assign_value(pf, j_v, zero);
  pfile_cmd_label_add(pf, lbl2);
  t = VALUE_NONE;
  pfile_cmd_op_add(pf, OPERATOR_SUB, &t, len_v, j_v);
  jal_string_assign_value(pf, k_v, t);
  value_release(t);
  t = VALUE_NONE;
  pfile_cmd_op_add(pf, OPERATOR_SUB, &t, k_v, one);
  jal_string_assign_value(pf, k_v, t);
  value_release(t);
  {
    value_t byte_ofs;
    value_t csrc;
    value_t dst_ofs;

    byte_ofs = jal_string_add_const(pf, k_v, maxdig);
    csrc = jal_string_source_byte_get(pf, scratch, byte_ofs);
    value_release(byte_ofs);
    dst_ofs = VALUE_NONE;
    pfile_cmd_op_add(pf, OPERATOR_ADD, &dst_ofs, j_v, neg_v);
    elem = jal_string_index_byte_lvalue_get(pf, scratch, dst_ofs);
    value_release(dst_ofs);
    if (!elem || !csrc) {
      if (csrc) {
        value_release(csrc);
      }
      goto numfail;
    }
    jal_string_assign_value(pf, elem, csrc);
    value_release(csrc);
    value_release(elem);
    elem = VALUE_NONE;
  }
  t = VALUE_NONE;
  pfile_cmd_op_add(pf, OPERATOR_ADD, &t, j_v, one);
  jal_string_assign_value(pf, j_v, t);
  value_release(t);
  jal_string_branch_if_value_cmp(pf, OPERATOR_LT, j_v, len_v,
      CMD_BRANCHCOND_TRUE, lbl2);

  /* len += neg; the item is the scratch array itself */
  if (is_signed) {
    t = VALUE_NONE;
    pfile_cmd_op_add(pf, OPERATOR_ADD, &t, len_v, neg_v);
    jal_string_assign_value(pf, len_v, t);
    value_release(t);
  }
  item = value_clone(scratch);
  *len_out = value_clone(len_v);
  value_release(scratch);
  value_release(tmpv);
  value_release(len_v);
  value_release(j_v);
  value_release(k_v);
  value_release(d_v);
  value_release(ch_v);
  value_release(neg_v);
  value_release(ten);
  value_release(one);
  value_release(c45);
  value_release(c48);
  value_release(zero);
  label_release(lbl1);
  label_release(lbl2);
  label_release(lbl_sgn);
  label_release(lbl_nm);
  if (LABEL_NONE != lbl_dg) {
    label_release(lbl_dg);
  }
  if (LABEL_NONE != lbl_pad) {
    label_release(lbl_pad);
  }
  if (LABEL_NONE != lbl_padend) {
    label_release(lbl_padend);
  }
  pfile_flag_change(pf, ~(flag_t) 0, warn_saved);
  return item;

numfail:
  if (scratch) {
    value_release(scratch);
  }
  if (tmpv) {
    value_release(tmpv);
  }
  if (len_v) {
    value_release(len_v);
  }
  if (j_v) {
    value_release(j_v);
  }
  if (k_v) {
    value_release(k_v);
  }
  if (d_v) {
    value_release(d_v);
  }
  if (ch_v) {
    value_release(ch_v);
  }
  if (neg_v) {
    value_release(neg_v);
  }
  if (ten) {
    value_release(ten);
  }
  if (one) {
    value_release(one);
  }
  if (c45) {
    value_release(c45);
  }
  if (c48) {
    value_release(c48);
  }
  if (zero) {
    value_release(zero);
  }
  if (nine) {
    value_release(nine);
  }
  if (c39) {
    value_release(c39);
  }
  if (padv) {
    value_release(padv);
  }
  if (LABEL_NONE != lbl_dg) {
    label_release(lbl_dg);
  }
  if (LABEL_NONE != lbl_pad) {
    label_release(lbl_pad);
  }
  if (LABEL_NONE != lbl_padend) {
    label_release(lbl_padend);
  }
  if (LABEL_NONE != lbl1) {
    label_release(lbl1);
  }
  if (LABEL_NONE != lbl2) {
    label_release(lbl2);
  }
  if (LABEL_NONE != lbl_sgn) {
    label_release(lbl_sgn);
  }
  if (LABEL_NONE != lbl_nm) {
    label_release(lbl_nm);
  }
  pfile_flag_change(pf, ~(flag_t) 0, warn_saved);
  return VALUE_NONE;
}

/* General expression position (stage 1 of the generalized `&` plan):
 * `first` is an already-parsed string source; consume further
 * `& operand` tokens and evaluate the whole chain into a compiler-
 * generated heap temp, which then behaves like any `var string` value
 * (comparison operands, procedure arguments, ...).  Takes ownership of
 * `first`; the caller must not release it.  Returns the locked temp,
 * or VALUE_NONE on error. */
value_t jal_string_concat_chain_emit(pfile_t *pf, value_t first)
{
  value_t  *items;
  value_t  *lens;
  size_t    item_ct;
  size_t    item_alloc;
  value_t   temp;
  boolean_t ok;

  items = 0;
  lens = 0;
  item_ct = 0;
  item_alloc = 0;
  temp = VALUE_NONE;
  ok = BOOLEAN_TRUE;
  do {
    value_t item;
    value_t len;

    if (0 == item_ct) {
      item = first;
    } else {
      item = jal_string_parse_operand(pf);
      if (!item) {
        ok = BOOLEAN_FALSE;
        break;
      }
    }
    jal_string_dynamic_dst_through_pointer_maybe(item);
    if (item_ct == item_alloc) {
      size_t new_alloc;
      value_t *new_items;
      value_t *new_lens;

      new_alloc = item_alloc ? item_alloc * 2 : 4;
      new_items = MALLOC(new_alloc * sizeof(*items));
      new_lens = MALLOC(new_alloc * sizeof(*lens));
      if (!new_items || !new_lens) {
        pfile_log_syserr(pf, ENOMEM);
        FREE(new_items);
        FREE(new_lens);
        if (0 == item_ct) {
          value_release(item);
        }
        ok = BOOLEAN_FALSE;
        break;
      }
      if (item_ct) {
        memcpy(new_items, items, item_ct * sizeof(*items));
        memcpy(new_lens, lens, item_ct * sizeof(*lens));
      }
      FREE(items);
      FREE(lens);
      items = new_items;
      lens = new_lens;
      item_alloc = new_alloc;
    }
    item = jal_string_chain_item_resolve(pf, item, &len);
    if (!item) {
      ok = BOOLEAN_FALSE;
      break;
    }
    items[item_ct] = item;
    lens[item_ct] = len;
    item_ct++;
    if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "&")) {
      pf_token_get(pf, PF_TOKEN_NEXT);
    } else {
      break;
    }
  } while (BOOLEAN_TRUE);

  if (ok && item_ct) {
    temp = jal_string_chain_var_get(pf);
    if (temp) {
      jal_string_concat_emit(pf, temp, items, lens, item_ct);
      value_lock(temp);
    } else {
      pfile_log_syserr(pf, ENOMEM);
      ok = BOOLEAN_FALSE;
    }
  }
  while (item_ct) {
    item_ct--;
    value_release(lens[item_ct]);
    value_release(items[item_ct]);
  }
  FREE(lens);
  FREE(items);
  if (!ok) {
    temp = VALUE_NONE;
  }
  return temp;
}

/* Effective text length of one concat/assign operand as a runtime value:
 * heap dynamic -> handle length field; const literal -> compile-time count;
 * fixed variable -> runtime scan to the first zero byte, capped at capacity. */
static value_t jal_string_fixed_operand_len_emit(pfile_t *pf, value_t src)
{
  label_t loop_lbl;
  label_t done_lbl;
  value_t  len;
  value_t  idx;
  value_t  one;
  value_t  zero;
  value_t  cap;
  value_t  byte;

  if (jal_string_is_dynamic(src)) {
    return jal_string_length_value_get(pf, src);
  }
  if (value_is_const(src)) {
    return jal_string_index_const_get(pf, value_ct_get(src));
  }
  loop_lbl = pfile_label_alloc(pf, 0);
  done_lbl = pfile_label_alloc(pf, 0);
  idx   = jal_string_temp_index_get(pf);
  len   = jal_string_temp_index_get(pf);
  one   = jal_string_const_get(pf, 1);
  zero  = jal_string_const_get(pf, 0);
  cap   = jal_string_index_const_get(pf, value_ct_get(src));
  byte  = VALUE_NONE;

  jal_string_assign_value(pf, idx, zero);
  pfile_cmd_label_add(pf, loop_lbl);
  jal_string_branch_if_value_cmp(pf, OPERATOR_GE, idx, cap,
      CMD_BRANCHCOND_TRUE, done_lbl);
  byte = jal_string_array_elem_get(src, idx);
  jal_string_branch_if_value_cmp(pf, OPERATOR_EQ, byte, zero,
      CMD_BRANCHCOND_TRUE, done_lbl);
  pfile_cmd_op_add(pf, OPERATOR_ADD, &idx, idx, one);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
      loop_lbl, VALUE_NONE, VALUE_NONE, 0);
  pfile_cmd_label_add(pf, done_lbl);
  jal_string_assign_value(pf, len, idx);

  value_release(byte);
  value_release(cap);
  value_release(zero);
  value_release(one);
  value_release(idx);
  label_release(done_lbl);
  label_release(loop_lbl);
  return len;
}

/* copy len bytes of src into dst at dst-base ofs */
static void jal_string_fixed_copy_at_emit(pfile_t *pf, value_t dst,
    value_t src, value_t len, value_t ofs)
{
  label_t loop_lbl;
  label_t done_lbl;
  value_t  idx;
  value_t  one;
  value_t  zero;
  value_t  dst_ofs;
  value_t  dst_byte;
  value_t  src_byte;

  loop_lbl = pfile_label_alloc(pf, 0);
  done_lbl = pfile_label_alloc(pf, 0);
  idx  = jal_string_temp_index_get(pf);
  one  = jal_string_const_get(pf, 1);
  zero = jal_string_const_get(pf, 0);
  dst_ofs = VALUE_NONE;
  dst_byte = VALUE_NONE;
  src_byte = VALUE_NONE;

  jal_string_assign_value(pf, idx, zero);
  pfile_cmd_label_add(pf, loop_lbl);
  jal_string_branch_if_value_cmp(pf, OPERATOR_GE, idx, len,
      CMD_BRANCHCOND_TRUE, done_lbl);
  dst_ofs = jal_string_binary_emit(pf, OPERATOR_ADD, ofs, idx);
  dst_byte = jal_string_array_elem_get(dst, dst_ofs);
  src_byte = jal_string_source_byte_get(pf, src, idx);
  jal_string_assign_value(pf, dst_byte, src_byte);
  pfile_cmd_op_add(pf, OPERATOR_ADD, &idx, idx, one);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
      loop_lbl, VALUE_NONE, VALUE_NONE, 0);
  pfile_cmd_label_add(pf, done_lbl);

  value_release(src_byte);
  value_release(dst_byte);
  value_release(dst_ofs);
  value_release(zero);
  value_release(one);
  value_release(idx);
  label_release(done_lbl);
  label_release(loop_lbl);
}

/* zero dst[from .. cap) */
static void jal_string_fixed_zero_fill_emit(pfile_t *pf, value_t dst,
    value_t from)
{
  label_t loop_lbl;
  label_t done_lbl;
  value_t  idx;
  value_t  one;
  value_t  zero;
  value_t  cap;
  value_t  dst_byte;

  loop_lbl = pfile_label_alloc(pf, 0);
  done_lbl = pfile_label_alloc(pf, 0);
  idx  = jal_string_temp_index_get(pf);
  one  = jal_string_const_get(pf, 1);
  zero = jal_string_const_get(pf, 0);
  cap  = jal_string_index_const_get(pf, value_ct_get(dst));
  dst_byte = VALUE_NONE;

  jal_string_assign_value(pf, idx, from);
  pfile_cmd_label_add(pf, loop_lbl);
  jal_string_branch_if_value_cmp(pf, OPERATOR_GE, idx, cap,
      CMD_BRANCHCOND_TRUE, done_lbl);
  dst_byte = jal_string_array_elem_get(dst, idx);
  jal_string_assign_value(pf, dst_byte, zero);
  pfile_cmd_op_add(pf, OPERATOR_ADD, &idx, idx, one);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
      loop_lbl, VALUE_NONE, VALUE_NONE, 0);
  pfile_cmd_label_add(pf, done_lbl);

  value_release(dst_byte);
  value_release(cap);
  value_release(zero);
  value_release(one);
  value_release(idx);
  label_release(done_lbl);
  label_release(loop_lbl);
}

/* dst = src (single operand). Effective length semantics; a source longer
 * than the capacity copies only N bytes and sets _string_error OVERFLOW. */
static void jal_string_fixed_assignment_emit(pfile_t *pf, value_t dst,
    value_t src)
{
  label_t ok_lbl;
  value_t  len;
  value_t  cap;
  value_t  err;
  value_t  zero;
  value_t  overflow;

  if (!jal_string_fixed_runtime_ensure(pf)) {
    return;
  }
  ok_lbl = pfile_label_alloc(pf, 0);
  len  = jal_string_fixed_operand_len_emit(pf, src);
  cap  = jal_string_index_const_get(pf, value_ct_get(dst));
  err  = jal_string_named_value_get(pf, JAL_STRING_ERROR_NAME);
  zero = jal_string_const_get(pf, JAL_STRING_ERROR_NONE);
  overflow = jal_string_const_get(pf, JAL_STRING_ERROR_OVERFLOW);

  jal_string_assign_value(pf, err, zero);
  if (value_is_const(len)) {
    /* compile-time fit decision: no runtime guard (avoids folded always-true
     * compare); constant overflow reports without copying */
    if (value_const_get(len) > (variable_const_t) value_const_get(cap)) {
      jal_string_assign_value(pf, err, overflow);
      value_release(len);
      len = cap;
      cap = VALUE_NONE;
    }
  } else {
    jal_string_branch_if_value_cmp(pf, OPERATOR_LE, len, cap,
        CMD_BRANCHCOND_TRUE, ok_lbl);
    jal_string_assign_value(pf, err, overflow);
    jal_string_assign_value(pf, len, cap);
    pfile_cmd_label_add(pf, ok_lbl);
  }
  jal_string_fixed_copy_at_emit(pf, dst, src, len,
    jal_string_const_get(pf, 0));
  jal_string_fixed_zero_fill_emit(pf, dst, len);

  value_release(overflow);
  value_release(zero);
  value_release(err);
  value_release(cap);
  value_release(len);
  label_release(ok_lbl);
}

/* dst = items[0] & items[1] & ... ; overflow leaves dst unchanged */
static void jal_string_fixed_concat_emit(pfile_t *pf, value_t dst,
    value_t *items, size_t item_ct)
{
  label_t done_lbl;
  label_t fits_lbl;
  value_t *lens;
  value_t  total;
  value_t  offset;
  value_t  cap;
  value_t  err;
  value_t  zero;
  value_t  overflow;
  value_t  zero_const;
  variable_const_t total_const;
  boolean_t  all_const;
  size_t   ii;

  if (!jal_string_fixed_runtime_ensure(pf)) {
    return;
  }
  lens = MALLOC(item_ct * sizeof(*lens));
  if (!lens) {
    pfile_log_syserr(pf, ENOMEM);
    return;
  }
  done_lbl = pfile_label_alloc(pf, 0);
  fits_lbl = pfile_label_alloc(pf, 0);
  total  = jal_string_temp_index_get(pf);
  offset = jal_string_temp_index_get(pf);
  cap  = jal_string_index_const_get(pf, value_ct_get(dst));
  err  = jal_string_named_value_get(pf, JAL_STRING_ERROR_NAME);
  zero = jal_string_const_get(pf, JAL_STRING_ERROR_NONE);
  overflow = jal_string_const_get(pf, JAL_STRING_ERROR_OVERFLOW);
  zero_const = jal_string_const_get(pf, 0);

  jal_string_assign_value(pf, err, zero);
  all_const = BOOLEAN_TRUE;
  total_const = 0;
  for (ii = 0; ii < item_ct; ii++) {
    lens[ii] = jal_string_fixed_operand_len_emit(pf, items[ii]);
    if (value_is_const(lens[ii])) {
      total_const += (variable_const_t) value_const_get(lens[ii]);
    } else {
      all_const = BOOLEAN_FALSE;
    }
    pfile_cmd_op_add(pf, OPERATOR_ADD, &total, total, lens[ii]);
  }
  if (all_const) {
    /* compile-time total: static fit decision (no folded always-true
     * compare); constant overflow reports and leaves dst unchanged */
    if (total_const > (variable_const_t) value_const_get(cap)) {
      jal_string_assign_value(pf, err, overflow);
      pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
          done_lbl, VALUE_NONE, VALUE_NONE, 0);
    }
  } else {
    jal_string_branch_if_value_cmp(pf, OPERATOR_LE, total, cap,
        CMD_BRANCHCOND_TRUE, fits_lbl);
    jal_string_assign_value(pf, err, overflow);
    pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
        done_lbl, VALUE_NONE, VALUE_NONE, 0);
  }
  pfile_cmd_label_add(pf, fits_lbl);
  jal_string_assign_value(pf, offset, zero_const);
  for (ii = 0; ii < item_ct; ii++) {
    jal_string_fixed_copy_at_emit(pf, dst, items[ii], lens[ii], offset);
    pfile_cmd_op_add(pf, OPERATOR_ADD, &offset, offset, lens[ii]);
  }
  jal_string_fixed_zero_fill_emit(pf, dst, offset);
  pfile_cmd_label_add(pf, done_lbl);

  value_release(zero_const);
  value_release(overflow);
  value_release(zero);
  value_release(err);
  value_release(cap);
  value_release(offset);
  value_release(total);
  label_release(fits_lbl);
  label_release(done_lbl);
  for (ii = 0; ii < item_ct; ii++) {
    value_release(lens[ii]);
  }
  FREE(lens);
}

boolean_t jal_string_assignment_parse_emit(pfile_t *pf, value_t dst)
{
  value_t  *items;
  value_t  *lens;
  size_t    item_ct;
  size_t    item_alloc;
  boolean_t saw_concat;
  boolean_t ok;

  jal_string_dynamic_dst_through_pointer_maybe(dst);
  if (!jal_string_is_dynamic(dst) && !jal_string_is_fixed(dst)) {
    return BOOLEAN_FALSE;
  }
  items = 0;
  lens = 0;
  item_ct = 0;
  item_alloc = 0;
  saw_concat = BOOLEAN_FALSE;
  ok = BOOLEAN_TRUE;
  do {
    value_t item;
    value_t len;

    if (item_ct == item_alloc) {
      size_t new_alloc;
      value_t *new_items;
      value_t *new_lens;

      new_alloc = item_alloc ? item_alloc * 2 : 4;
      new_items = MALLOC(new_alloc * sizeof(*items));
      new_lens = MALLOC(new_alloc * sizeof(*lens));
      if (!new_items || !new_lens) {
        pfile_log_syserr(pf, ENOMEM);
        FREE(new_items);
        FREE(new_lens);
        ok = BOOLEAN_FALSE;
        break;
      }
      if (item_ct) {
        memcpy(new_items, items, item_ct * sizeof(*items));
        memcpy(new_lens, lens, item_ct * sizeof(*lens));
      }
      FREE(items);
      FREE(lens);
      items = new_items;
      lens = new_lens;
      item_alloc = new_alloc;
    }
    item = jal_string_parse_operand(pf);
    if (!item) {
      ok = BOOLEAN_FALSE;
      break;
    }
    jal_string_dynamic_dst_through_pointer_maybe(item);
    item = jal_string_chain_item_resolve(pf, item, &len);
    if (!item) {
      ok = BOOLEAN_FALSE;
      break;
    }
    items[item_ct] = item;
    lens[item_ct] = len;
    item_ct++;
    if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "&")) {
      saw_concat = BOOLEAN_TRUE;
      pf_token_get(pf, PF_TOKEN_NEXT);
    } else {
      break;
    }
  } while (BOOLEAN_TRUE);

  if (ok && item_ct) {
    if (jal_string_is_fixed(dst)) {
      if (saw_concat) {
        jal_string_fixed_concat_emit(pf, dst, items, item_ct);
      } else {
        jal_string_fixed_assignment_emit(pf, dst, items[0]);
      }
    } else if (saw_concat) {
      jal_string_concat_emit(pf, dst, items, lens, item_ct);
    } else {
      jal_string_assignment_emit(pf, dst, items[0]);
    }
  }
  while (item_ct) {
    item_ct--;
    value_release(lens[item_ct]);
    value_release(items[item_ct]);
  }
  FREE(lens);
  FREE(items);
  return BOOLEAN_TRUE;
}

static boolean_t jal_string_assignment_string_from_array_lib_call_emit(
    pfile_t *pf, value_t dst, value_t src);

boolean_t jal_string_assignment_emit(pfile_t *pf, value_t dst, value_t src)
{
  label_t fail_lbl;
  label_t copy_lbl;
  label_t clear_lbl;
  label_t done_lbl;
  value_t len;
  value_t dst_cap;
  value_t err;
  value_t zero;
  value_t heap_full;

  jal_string_dynamic_dst_through_pointer_maybe(dst);
  if (!jal_string_is_dynamic(dst)) {
    return BOOLEAN_FALSE;
  }
  if (jal_string_same_dynamic_storage(dst, src)) {
    return BOOLEAN_TRUE;
  }
  if (!jal_string_is_dynamic(src)
      && !(value_is_array(src) && value_is_string(src))) {
    pfile_log(pf, PFILE_LOG_ERR, "type mismatch");
    return BOOLEAN_TRUE;
  }
  if (!jal_string_runtime_ensure(pf)) {
    return BOOLEAN_TRUE;
  }

  if (!jal_string_is_dynamic(src)
      && jal_string_assignment_string_from_array_lib_call_emit(pf, dst, src)) {
    return BOOLEAN_TRUE;
  }

  if (jal_string_is_dynamic(src)) {
    len = jal_string_length_value_get(pf, src);
    /* Option A heap-sizing diagnostic: a copy of a string whose static
     * max is known propagates the bound to dst; an unknown-length
     * source skips the report (agreed semantics) */
    jal_string_heap_use_copy_note(pf, dst, src);
  } else {
    len = jal_string_index_const_get(pf, value_ct_get(src));
    jal_string_heap_use_note(dst, len);
  }
  fail_lbl = pfile_label_alloc(pf, 0);
  copy_lbl = pfile_label_alloc(pf, 0);
  clear_lbl = pfile_label_alloc(pf, 0);
  done_lbl = pfile_label_alloc(pf, 0);
  err = jal_string_named_value_get(pf, JAL_STRING_ERROR_NAME);
  zero = jal_string_const_get(pf, JAL_STRING_ERROR_NONE);
  heap_full = jal_string_const_get(pf, JAL_STRING_ERROR_HEAP_FULL);

  jal_string_assign_value(pf, err, zero);
  dst_cap = jal_string_handle_index_get(pf, dst, JAL_STRING_FIELD_CAP);

  jal_string_branch_if_const_cmp(pf, OPERATOR_EQ, len, 0,
      CMD_BRANCHCOND_TRUE, clear_lbl);
  jal_string_branch_if_value_cmp(pf, OPERATOR_GE, dst_cap, len,
      CMD_BRANCHCOND_TRUE, copy_lbl);
  jal_string_allocate_emit(pf, dst, len, fail_lbl, copy_lbl);

  pfile_cmd_label_add(pf, copy_lbl);
  jal_string_handle_index_set(pf, dst, JAL_STRING_FIELD_LENGTH, len);
  jal_string_copy_emit(pf, dst, src, len);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
      done_lbl, VALUE_NONE, VALUE_NONE, 0);

  pfile_cmd_label_add(pf, clear_lbl);
  jal_string_clear_emit(pf, dst);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
      done_lbl, VALUE_NONE, VALUE_NONE, 0);

  pfile_cmd_label_add(pf, fail_lbl);
  jal_string_assign_value(pf, err, heap_full);
  pfile_cmd_label_add(pf, done_lbl);

  value_release(dst_cap);
  value_release(heap_full);
  value_release(zero);
  value_release(err);
  value_release(len);
  label_release(done_lbl);
  label_release(clear_lbl);
  label_release(copy_lbl);
  label_release(fail_lbl);
  return BOOLEAN_TRUE;
}

static void jal_string_dynamic_copy_len_emit(pfile_t *pf, value_t dst,
    value_t src, value_t len, value_t result, boolean_t update_error)
{
  label_t fail_lbl;
  label_t copy_lbl;
  label_t clear_lbl;
  label_t done_lbl;
  value_t dst_cap;
  value_t err;
  value_t zero;
  value_t heap_full;

  if (!jal_string_runtime_ensure(pf)) {
    jal_string_bool_assign(pf, result, BOOLEAN_FALSE);
    return;
  }
  fail_lbl = pfile_label_alloc(pf, 0);
  copy_lbl = pfile_label_alloc(pf, 0);
  clear_lbl = pfile_label_alloc(pf, 0);
  done_lbl = pfile_label_alloc(pf, 0);
  dst_cap = jal_string_handle_index_get(pf, dst, JAL_STRING_FIELD_CAP);
  err = update_error ? jal_string_named_value_get(pf, JAL_STRING_ERROR_NAME)
    : VALUE_NONE;
  zero = jal_string_const_get(pf, JAL_STRING_ERROR_NONE);
  heap_full = jal_string_const_get(pf, JAL_STRING_ERROR_HEAP_FULL);

  if (err) {
    jal_string_assign_value(pf, err, zero);
  }
  jal_string_branch_if_const_cmp(pf, OPERATOR_EQ, len, 0,
      CMD_BRANCHCOND_TRUE, clear_lbl);
  jal_string_branch_if_value_cmp(pf, OPERATOR_GE, dst_cap, len,
      CMD_BRANCHCOND_TRUE, copy_lbl);
  jal_string_allocate_emit(pf, dst, len, fail_lbl, copy_lbl);

  pfile_cmd_label_add(pf, copy_lbl);
  jal_string_handle_index_set(pf, dst, JAL_STRING_FIELD_LENGTH, len);
  jal_string_copy_emit(pf, dst, src, len);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
      done_lbl, VALUE_NONE, VALUE_NONE, 0);

  pfile_cmd_label_add(pf, clear_lbl);
  jal_string_clear_emit(pf, dst);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
      done_lbl, VALUE_NONE, VALUE_NONE, 0);

  pfile_cmd_label_add(pf, fail_lbl);
  jal_string_bool_assign(pf, result, BOOLEAN_FALSE);
  if (err) {
    jal_string_assign_value(pf, err, heap_full);
  }
  pfile_cmd_label_add(pf, done_lbl);

  value_release(heap_full);
  value_release(zero);
  value_release(err);
  value_release(dst_cap);
  label_release(done_lbl);
  label_release(clear_lbl);
  label_release(copy_lbl);
  label_release(fail_lbl);
}

static value_t jal_string_truncate_emit(pfile_t *pf, value_t dst, value_t max)
{
  label_t keep_lbl;
  label_t clear_lbl;
  label_t done_lbl;
  value_t len;
  value_t result;

  result = jal_string_bool_result_get(pf, BOOLEAN_TRUE);
  len = jal_string_length_value_get(pf, dst);
  keep_lbl = pfile_label_alloc(pf, 0);
  clear_lbl = pfile_label_alloc(pf, 0);
  done_lbl = pfile_label_alloc(pf, 0);

  jal_string_branch_if_value_cmp(pf, OPERATOR_LE, len, max,
      CMD_BRANCHCOND_TRUE, done_lbl);
  jal_string_bool_assign(pf, result, BOOLEAN_FALSE);
  jal_string_branch_if_const_cmp(pf, OPERATOR_EQ, max, 0,
      CMD_BRANCHCOND_TRUE, clear_lbl);
  jal_string_handle_index_set(pf, dst, JAL_STRING_FIELD_LENGTH, max);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
      keep_lbl, VALUE_NONE, VALUE_NONE, 0);
  pfile_cmd_label_add(pf, clear_lbl);
  jal_string_clear_emit(pf, dst);
  pfile_cmd_label_add(pf, keep_lbl);
  pfile_cmd_label_add(pf, done_lbl);

  value_release(len);
  label_release(done_lbl);
  label_release(clear_lbl);
  label_release(keep_lbl);
  return result;
}

/* Open-array formal `byte in name[]`: procedure member tag `_name_count` (see
 * `jal_expr.c` `jal_value_find_open_array_count_param`). */
static value_t jal_string_open_array_count_param_get(pfile_t *pf,
    const char *want_tag)
{
  pfile_proc_t             *proc;
  variable_def_member_t     mbr;
  size_t                    ii;
  value_t                   pv;

  proc = pfile_proc_active_get(pf);
  if (!proc || !want_tag) {
    return VALUE_NONE;
  }
  for (ii = 0, mbr = variable_def_member_get(pfile_proc_def_get(proc));
       mbr;
       ii++, mbr = variable_def_member_link_get(mbr)) {
    const char *tag;

    tag = variable_def_member_tag_get(mbr);
    if (tag && 0 == strcmp(tag, want_tag)
        && ii < pfile_proc_param_ct_get(proc)) {
      pv = pfile_proc_param_get(proc, ii);
      if (pv) {
        return value_clone(pv);
      }
      break;
    }
  }
  return VALUE_NONE;
}

/* True for `var byte[N]` / `byte[]` / formal pointer-to-byte-array (not JAL
 * `string` / dynamic string storage). */
static boolean_t jal_string_def_is_byte_data_array(variable_def_t def)
{
  variable_def_member_t mbr;
  variable_def_t        elem;

  if (!def) {
    return BOOLEAN_FALSE;
  }
  if (VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(def)) {
    variable_def_t sub;

    mbr = variable_def_member_get(def);
    if (!mbr) {
      return BOOLEAN_FALSE;
    }
    sub = variable_def_member_def_get(mbr);
    if (!sub) {
      return BOOLEAN_FALSE;
    }
    /* Fixed `byte[N]` / `byte[]` formals: open arrays are lowered to a POINTER
     * whose member is the *element* def, not VARIABLE_DEF_TYPE_ARRAY (see
     * `jal_proc.c` formal handling). */
    if (VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(sub)) {
      return jal_string_def_is_byte_data_array(sub);
    }
    return VARIABLE_DEF_TYPE_INTEGER == variable_def_type_get(sub)
        && 1 == variable_def_sz_get(sub)
        && !variable_def_flag_test(sub, VARIABLE_DEF_FLAG_BIT)
        && !variable_def_flag_test(sub, VARIABLE_DEF_FLAG_STRING);
  }
  if (VARIABLE_DEF_TYPE_ARRAY != variable_def_type_get(def)) {
    return BOOLEAN_FALSE;
  }
  /* Heap `var string` uses STRING_DYNAMIC; const JAL `"..."` arrays are
   * CONST+STRING payload and are valid `byte in []` sources. */
  if (variable_def_flag_test(def, VARIABLE_DEF_FLAG_STRING)) {
    if (!variable_def_flag_test(def, VARIABLE_DEF_FLAG_CONST)
        || variable_def_flag_test(def, VARIABLE_DEF_FLAG_STRING_DYNAMIC)) {
      return BOOLEAN_FALSE;
    }
  }
  mbr = variable_def_member_get(def);
  if (!mbr) {
    return BOOLEAN_FALSE;
  }
  elem = variable_def_member_def_get(mbr);
  return elem
      && VARIABLE_DEF_TYPE_INTEGER == variable_def_type_get(elem)
      && 1 == variable_def_sz_get(elem)
      && !variable_def_flag_test(elem, VARIABLE_DEF_FLAG_BIT);
}

static boolean_t jal_string_builtin_byte_array_arg_ok(const value_t val)
{
  if (!val) {
    return BOOLEAN_FALSE;
  }
  return jal_string_def_is_byte_data_array(value_def_get(val));
}



#if JALV2_STRING_HEAP_COMPACT_CALL

/* Emit `CALL string_heap_compact` (outlined `strings_dynamic`) so the
 * pcode expands once inside the library body instead of at every builtin site.
 * Falls back to defer when the procedure is absent (no `include strings_dynamic`). */
static boolean_t jal_string_heap_compact_jal_call_emit(pfile_t *pf, value_t result)
{
  pfile_proc_t          *proc;
  variable_def_t        def;
  variable_def_member_t mbr;
  size_t                param_ct;
  value_t              *parms;
  value_t               call_val;
  variable_def_t        mdef0;

  proc = pfile_proc_find(pf, PFILE_LOG_NONE, "string_heap_compact");
  if (!proc) {
    return BOOLEAN_FALSE;
  }
  def = pfile_proc_def_get(proc);
  for (mbr = variable_def_member_get(def), param_ct = 0U;
       mbr;
       mbr = variable_def_member_link_get(mbr), param_ct++) {
  }
  if (param_ct < 1U) {
    param_ct = 1U;
  }
  /* `function string_heap_compact() return bit` — one member: OUT return slot. */
  if (param_ct != 1U) {
    return BOOLEAN_FALSE;
  }
  parms = CALLOC(param_ct, sizeof(*parms));
  if (!parms) {
    pfile_log_syserr(pf, ENOMEM);
    return BOOLEAN_FALSE;
  }
  mbr = variable_def_member_get(def);
  mdef0 = variable_def_member_def_get(mbr);
  if (!mdef0 || !variable_def_flag_test(mdef0, VARIABLE_DEF_FLAG_OUT)
      || variable_def_flag_test(mdef0, VARIABLE_DEF_FLAG_IN)) {
    FREE(parms);
    return BOOLEAN_FALSE;
  }
  value_lock(result);
  parms[0] = result;

  call_val = jal_string_jal_proc_call_value_get(pf, "string_heap_compact");
  if (!call_val) {
    value_release(result);
    FREE(parms);
    return BOOLEAN_FALSE;
  }
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_CALL, CMD_BRANCHCOND_NONE,
      LABEL_NONE, VALUE_NONE, call_val, parms);
  return BOOLEAN_TRUE;
}

#endif /* JALV2_STRING_HEAP_COMPACT_CALL */

#if JALV2_ROM_A_STRING_LITERAL_CALL

/* Emit `CALL string_from_array(dst, literal)` (outlined `strings_dynamic`)
 * instead of inlining `jal_string_assignment_emit`. Matches formal layout:
 * [0] return bit, [1] dst in/out, [2] `_src_array_count`, [3] `src_array`. */
static boolean_t jal_string_assignment_string_from_array_lib_call_emit(
    pfile_t *pf, value_t dst, value_t src)
{
  pfile_proc_t          *proc;
  variable_def_t        def;
  variable_def_member_t mbr;
  size_t                param_ct;
  value_t              *parms;
  value_t               call_val;
  variable_def_t        mdef0;
  value_t               err;
  value_t               zero;
  value_t               ct_val;

  if (!jal_string_builtin_byte_array_arg_ok(src)) {
    return BOOLEAN_FALSE;
  }
  /* Short literals: inline when below threshold (see jal_build.h
   * JALV2_ROM_A_STRING_LITERAL_CALL_MIN_BYTES; default 2). */
  if (value_ct_get(src)
      < (variable_ct_t) JALV2_ROM_A_STRING_LITERAL_CALL_MIN_BYTES) {
    return BOOLEAN_FALSE;
  }
  proc = pfile_proc_find(pf, PFILE_LOG_NONE, "string_from_array");
  if (!proc) {
    return BOOLEAN_FALSE;
  }
  def = pfile_proc_def_get(proc);
  for (mbr = variable_def_member_get(def), param_ct = 0U;
       mbr;
       mbr = variable_def_member_link_get(mbr), param_ct++) {
  }
  if (param_ct < 1U) {
    param_ct = 1U;
  }
  /* Expect: return bit, `dst`, open-array count, `src_array` pointer. */
  if (param_ct != 4U) {
    return BOOLEAN_FALSE;
  }
  parms = CALLOC(param_ct, sizeof(*parms));
  if (!parms) {
    pfile_log_syserr(pf, ENOMEM);
    return BOOLEAN_FALSE;
  }
  mbr = variable_def_member_get(def);
  mdef0 = variable_def_member_def_get(mbr);
  if (mdef0 && variable_def_flag_test(mdef0, VARIABLE_DEF_FLAG_OUT)
      && !variable_def_flag_test(mdef0, VARIABLE_DEF_FLAG_IN)) {
    parms[0] = pfile_value_temp_get_from_def(pf, mdef0);
  } else {
    parms[0] = VALUE_NONE;
  }
  value_lock(dst);
  parms[1] = dst;
  ct_val = jal_string_index_const_get(pf, value_ct_get(src));
  if (!ct_val) {
    value_release(dst);
    if (parms[0]) {
      value_release(parms[0]);
    }
    FREE(parms);
    return BOOLEAN_FALSE;
  }
  parms[2] = ct_val;
  value_lock(src);
  parms[3] = src;

  err = jal_string_named_value_get(pf, JAL_STRING_ERROR_NAME);
  zero = jal_string_const_get(pf, JAL_STRING_ERROR_NONE);
  jal_string_assign_value(pf, err, zero);

  call_val = jal_string_jal_proc_call_value_get(pf, "string_from_array");
  if (!call_val) {
    value_release(src);
    value_release(dst);
    value_release(ct_val);
    if (parms[0]) {
      value_release(parms[0]);
    }
    FREE(parms);
    value_release(zero);
    value_release(err);
    return BOOLEAN_FALSE;
  }
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_CALL, CMD_BRANCHCOND_NONE,
      LABEL_NONE, VALUE_NONE, call_val, parms);
  value_release(zero);
  value_release(err);
  return BOOLEAN_TRUE;
}

#else /* !JALV2_ROM_A_STRING_LITERAL_CALL */

static boolean_t jal_string_assignment_string_from_array_lib_call_emit(
    pfile_t *pf, value_t dst, value_t src)
{
  (void) pf;
  (void) dst;
  (void) src;
  return BOOLEAN_FALSE;
}

#endif /* JALV2_ROM_A_STRING_LITERAL_CALL */

/* Element count as an index-sized value: fixed arrays use compile-time ct;
 * open-array formals use the `_name_count` hidden parameter. */
static value_t jal_string_byte_array_byte_length_get(pfile_t *pf, value_t arr)
{
  variable_ct_t         ct;
  const char            *nm;
  char                  *buf;
  value_t               len;
  variable_def_t        def;
  variable_def_member_t mbr;
  variable_def_t        inner;

  if (!arr) {
    return VALUE_NONE;
  }
  /* Open-array formals are `POINTER` → element `def` only (`jal_proc.c`);
   * `value_ct_get` would read the inner member ct (often **0**), not
   * `VARIABLE_CT_UNKNOWN` — length must come from `_name_count`. */
  def = value_def_get(arr);
  if (def && VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(def)) {
    mbr = variable_def_member_get(def);
    inner = (mbr) ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
    if (inner && VARIABLE_DEF_TYPE_INTEGER == variable_def_type_get(inner)
        && 1 == variable_def_sz_get(inner)
        && !variable_def_flag_test(inner, VARIABLE_DEF_FLAG_BIT)) {
      nm = value_name_get(arr);
      if (nm && *nm) {
        buf = MALLOC(8 + strlen(nm));
        if (!buf) {
          pfile_log_syserr(pf, ENOMEM);
          return VALUE_NONE;
        }
        sprintf(buf, "_%s_count", nm);
        len = jal_string_open_array_count_param_get(pf, buf);
        FREE(buf);
        return len;
      }
      return VALUE_NONE;
    }
  }
  ct = value_ct_get(arr);
  if (VARIABLE_CT_UNKNOWN != ct) {
    return jal_string_index_const_get(pf, ct);
  }
  nm = value_name_get(arr);
  if (!nm || !*nm) {
    return VALUE_NONE;
  }
  buf = MALLOC(8 + strlen(nm));
  if (!buf) {
    pfile_log_syserr(pf, ENOMEM);
    return VALUE_NONE;
  }
  sprintf(buf, "_%s_count", nm);
  len = jal_string_open_array_count_param_get(pf, buf);
  FREE(buf);
  return len;
}

static value_t jal_string_from_array_emit(pfile_t *pf, value_t dst,
    value_t src, value_t max, boolean_t truncate)
{
  label_t keep_lbl;
  value_t result;
  value_t len;
  value_t copy_len;

  result = jal_string_bool_result_get(pf, BOOLEAN_TRUE);
  len = jal_string_byte_array_byte_length_get(pf, src);
  if (!len) {
    pfile_log(pf, PFILE_LOG_ERR, "byte array source expected");
    jal_string_bool_assign(pf, result, BOOLEAN_FALSE);
    return result;
  }
  if (truncate) {
    copy_len = jal_string_temp_index_get_for_max(pf, max);
  } else {
    copy_len = jal_string_temp_index_get(pf);
  }
  jal_string_assign_value(pf, copy_len, len);
  if (truncate) {
    keep_lbl = pfile_label_alloc(pf, 0);
    jal_string_branch_if_value_cmp(pf, OPERATOR_LE, len, max,
        CMD_BRANCHCOND_TRUE, keep_lbl);
    jal_string_bool_assign(pf, result, BOOLEAN_FALSE);
    jal_string_assign_value(pf, copy_len, max);
    pfile_cmd_label_add(pf, keep_lbl);
    label_release(keep_lbl);
  }
  jal_string_dynamic_copy_len_emit(pf, dst, src, copy_len, result,
      truncate ? BOOLEAN_FALSE : BOOLEAN_TRUE);
  value_release(copy_len);
  value_release(len);
  return result;
}

static value_t jal_string_to_array_emit(pfile_t *pf, value_t dst,
    value_t src, value_t max, boolean_t truncate)
{
  label_t copy_lbl;
  value_t result;
  value_t src_len;
  value_t copy_len;
  value_t cap;

  result = jal_string_bool_result_get(pf, BOOLEAN_TRUE);
  src_len = jal_string_source_length_snapshot_get(pf, src);
  if (truncate) {
    copy_len = jal_string_temp_index_get_for_max(pf, max);
  } else {
    copy_len = jal_string_temp_index_get(pf);
  }
  cap = jal_string_byte_array_byte_length_get(pf, dst);
  if (!cap) {
    pfile_log(pf, PFILE_LOG_ERR, "byte array destination expected");
    value_release(src_len);
    value_release(copy_len);
    jal_string_bool_assign(pf, result, BOOLEAN_FALSE);
    return result;
  }
  jal_string_assign_value(pf, copy_len, src_len);
  copy_lbl = pfile_label_alloc(pf, 0);

  /* The base form behaves like the truncate form with an unlimited max:
   * a too-small destination COPIES THE PREFIX THAT FITS and returns
   * FALSE (bytes were dropped) -- never leaving the buffer untouched
   * (the old skip-copy failure mode read as a silent hang). */
  if (truncate && max) {
    jal_string_assign_min_emit(pf, copy_len, max);
  }
  jal_string_assign_min_emit(pf, copy_len, cap);
  jal_string_branch_if_value_cmp(pf, OPERATOR_LE, src_len, copy_len,
      CMD_BRANCHCOND_TRUE, copy_lbl);
  jal_string_bool_assign(pf, result, BOOLEAN_FALSE);
  pfile_cmd_label_add(pf, copy_lbl);
  jal_string_copy_to_array_emit(pf, dst, src, copy_len);

  value_release(cap);
  value_release(copy_len);
  value_release(src_len);
  label_release(copy_lbl);
  return result;
}

static value_t jal_string_append_truncate_emit(pfile_t *pf, value_t dst,
    value_t src, value_t max)
{
  label_t truncated_lbl;
  label_t non_empty_lbl;
  label_t shorten_lbl;
  label_t append_in_place_lbl;
  label_t alloc_fail_lbl;
  label_t alloc_copy_lbl;
  label_t done_lbl;
  value_t result;
  value_t dst_len;
  value_t src_len;
  value_t total_len;
  value_t copy_len;
  value_t second_len;
  value_t dst_cap;
  value_t old_off;
  value_t old_cap;
  value_t zero;
  boolean_t src_is_dst;
  variable_sz_t work_sz;
  variable_sz_t idx_sz;
  value_t w;

  if (!jal_string_runtime_ensure(pf)) {
    return jal_string_bool_result_get(pf, BOOLEAN_FALSE);
  }
  result = jal_string_bool_result_get(pf, BOOLEAN_TRUE);
  work_sz = jal_string_work_index_sz_for_max(pf, max);
  idx_sz = jal_string_current_index_size_get(pf);
  dst_len = jal_string_length_value_get(pf, dst);
  src_len = jal_string_source_length_snapshot_get(pf, src);
  dst_cap = jal_string_handle_index_get(pf, dst, JAL_STRING_FIELD_CAP);
  if (work_sz > idx_sz) {
    w = pfile_value_temp_get(pf, VARIABLE_DEF_TYPE_INTEGER, work_sz);
    jal_string_assign_value(pf, w, dst_len);
    value_release(dst_len);
    dst_len = w;
    w = pfile_value_temp_get(pf, VARIABLE_DEF_TYPE_INTEGER, work_sz);
    jal_string_assign_value(pf, w, src_len);
    value_release(src_len);
    src_len = w;
    w = pfile_value_temp_get(pf, VARIABLE_DEF_TYPE_INTEGER, work_sz);
    jal_string_assign_value(pf, w, dst_cap);
    value_release(dst_cap);
    dst_cap = w;
  }
  total_len = jal_string_temp_index_get_for_max(pf, max);
  copy_len = jal_string_temp_index_get_for_max(pf, max);
  second_len = jal_string_temp_index_get_for_max(pf, max);
  zero = jal_string_const_get(pf, 0);
  old_off = VALUE_NONE;
  old_cap = VALUE_NONE;
  src_is_dst = jal_string_same_dynamic_storage(dst, src);
  truncated_lbl = pfile_label_alloc(pf, 0);
  non_empty_lbl = pfile_label_alloc(pf, 0);
  shorten_lbl = pfile_label_alloc(pf, 0);
  append_in_place_lbl = pfile_label_alloc(pf, 0);
  alloc_fail_lbl = pfile_label_alloc(pf, 0);
  alloc_copy_lbl = pfile_label_alloc(pf, 0);
  done_lbl = pfile_label_alloc(pf, 0);

  jal_string_assign_value(pf, total_len, dst_len);
  pfile_cmd_op_add(pf, OPERATOR_ADD, &total_len, total_len, src_len);
  jal_string_assign_value(pf, copy_len, total_len);
  jal_string_branch_if_value_cmp(pf, OPERATOR_LE, total_len, max,
      CMD_BRANCHCOND_TRUE, truncated_lbl);
  jal_string_bool_assign(pf, result, BOOLEAN_FALSE);
  jal_string_assign_value(pf, copy_len, max);
  pfile_cmd_label_add(pf, truncated_lbl);

  jal_string_branch_if_const_cmp(pf, OPERATOR_NE, copy_len, 0,
      CMD_BRANCHCOND_TRUE, non_empty_lbl);
  jal_string_clear_emit(pf, dst);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
      done_lbl, VALUE_NONE, VALUE_NONE, 0);

  pfile_cmd_label_add(pf, non_empty_lbl);
  jal_string_branch_if_value_cmp(pf, OPERATOR_LE, copy_len, dst_len,
      CMD_BRANCHCOND_TRUE, shorten_lbl);
  pfile_cmd_op_add(pf, OPERATOR_SUB, &second_len, copy_len, dst_len);
  jal_string_branch_if_value_cmp(pf, OPERATOR_GE, dst_cap, copy_len,
      CMD_BRANCHCOND_TRUE, append_in_place_lbl);
  jal_string_allocate_emit_ex(pf, dst, copy_len, alloc_fail_lbl,
      alloc_copy_lbl, BOOLEAN_FALSE, &old_off, &old_cap);

  pfile_cmd_label_add(pf, alloc_copy_lbl);
  jal_string_handle_index_set(pf, dst, JAL_STRING_FIELD_LENGTH, copy_len);
  jal_string_copy_heap_at_emit(pf, dst, old_off, dst_len, zero);
  if (src_is_dst) {
    jal_string_copy_heap_at_emit(pf, dst, old_off, second_len, dst_len);
  } else {
    jal_string_copy_at_emit(pf, dst, src, second_len, dst_len);
  }
  jal_string_free_old_block_emit(pf, old_off, old_cap);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
      done_lbl, VALUE_NONE, VALUE_NONE, 0);

  pfile_cmd_label_add(pf, append_in_place_lbl);
  jal_string_handle_index_set(pf, dst, JAL_STRING_FIELD_LENGTH, copy_len);
  jal_string_copy_at_emit(pf, dst, src, second_len, dst_len);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
      done_lbl, VALUE_NONE, VALUE_NONE, 0);

  pfile_cmd_label_add(pf, shorten_lbl);
  jal_string_handle_index_set(pf, dst, JAL_STRING_FIELD_LENGTH, copy_len);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
      done_lbl, VALUE_NONE, VALUE_NONE, 0);

  pfile_cmd_label_add(pf, alloc_fail_lbl);
  jal_string_bool_assign(pf, result, BOOLEAN_FALSE);
  pfile_cmd_label_add(pf, done_lbl);

  value_release(old_cap);
  value_release(old_off);
  value_release(zero);
  value_release(dst_cap);
  value_release(second_len);
  value_release(copy_len);
  value_release(total_len);
  value_release(src_len);
  value_release(dst_len);
  label_release(done_lbl);
  label_release(alloc_copy_lbl);
  label_release(alloc_fail_lbl);
  label_release(append_in_place_lbl);
  label_release(shorten_lbl);
  label_release(non_empty_lbl);
  label_release(truncated_lbl);
  return result;
}

static value_t jal_string_append_emit(pfile_t *pf, value_t dst, value_t src)
{
  value_t result;
  value_t dst_len;
  value_t src_len;
  value_t max;
  value_t err;
  value_t none;
  value_t heap_full;
  label_t done_lbl;

  err = jal_string_named_value_get(pf, JAL_STRING_ERROR_NAME);
  none = jal_string_const_get(pf, JAL_STRING_ERROR_NONE);
  heap_full = jal_string_const_get(pf, JAL_STRING_ERROR_HEAP_FULL);
  dst_len = jal_string_length_value_get(pf, dst);
  src_len = jal_string_source_length_snapshot_get(pf, src);
  max = jal_string_temp_index_get(pf);
  jal_string_assign_value(pf, err, none);
  jal_string_assign_value(pf, max, dst_len);
  pfile_cmd_op_add(pf, OPERATOR_ADD, &max, max, src_len);
  result = jal_string_append_truncate_emit(pf, dst, src, max);
  done_lbl = pfile_label_alloc(pf, 0);
  jal_string_branch_if_const_cmp(pf, OPERATOR_NE, result, 0,
      CMD_BRANCHCOND_TRUE, done_lbl);
  jal_string_assign_value(pf, err, heap_full);
  jal_string_bool_assign(pf, result, BOOLEAN_FALSE);
  pfile_cmd_label_add(pf, done_lbl);
  label_release(done_lbl);
  value_release(max);
  value_release(src_len);
  value_release(dst_len);
  value_release(heap_full);
  value_release(none);
  value_release(err);
  return result;
}

/* Public spellings are `string_*` (the outlined strings_dynamic wrappers). Canonical
 * compiler builtin tokens are the same name with one leading underscore
 * (`_string_append`, …). JAL `alias` cannot bind builtins, so both forms are
 * accepted here. */
static boolean_t jal_string_builtin_pub_internal_eq(const char *name,
    const char *pub)
{
  char     intrn[40];
  size_t   plen;

  if (!name || !pub) {
    return BOOLEAN_FALSE;
  }
  if (0 == strcmp(name, pub)) {
    return BOOLEAN_TRUE;
  }
  plen = strlen(pub);
  if (plen + 2U > sizeof(intrn)) {
    return BOOLEAN_FALSE;
  }
  intrn[0] = '_';
  memcpy(intrn + 1, pub, plen + 1U);
  return (0 == strcmp(name, intrn));
}

boolean_t jal_string_builtin_is(const char *name)
{
  return name
      && (jal_string_builtin_pub_internal_eq(name, "string_append")
        || jal_string_builtin_pub_internal_eq(name,
            "string_append_truncate")
        || jal_string_builtin_pub_internal_eq(name, "string_truncate")
        || jal_string_builtin_pub_internal_eq(name, "string_from_array")
        || jal_string_builtin_pub_internal_eq(name,
            "string_from_array_truncate")
        || jal_string_builtin_pub_internal_eq(name, "string_to_array")
        || jal_string_builtin_pub_internal_eq(name,
            "string_to_array_truncate")
        || jal_string_builtin_pub_internal_eq(name, "string_heap_compact"));
}

static boolean_t jal_string_builtin_sep_get(pfile_t *pf, const char *sep)
{
  if (!pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, sep)) {
    return BOOLEAN_FALSE;
  }
  pf_token_get(pf, PF_TOKEN_NEXT);
  return BOOLEAN_TRUE;
}

/* ===================== text casts: string/dec/hex/bin =====================
 * (2026-10-06 stage 1: names reserved in expression position; const
 * operands fold to flash strings; runtime operands are a clean staged
 * error until the renderer generalization lands)
 *
 * `name(` is ALWAYS the cast (user decision 2026-10-05 -- reservation
 * like `f(`): collision scans across the corpus and both library trees
 * found no definitions or uses of these names. */

typedef enum jal_string_cast_e {
  JAL_STRING_CAST_STRING,
  JAL_STRING_CAST_HEX,
  JAL_STRING_CAST_BIN
} jal_string_cast_t;

static boolean_t jal_string_cast_from_name(const char *nm,
    jal_string_cast_t *which)
{
  if (!strcmp(nm, "string")) {
    *which = JAL_STRING_CAST_STRING;
  } else if (!strcmp(nm, "hex")) {
    *which = JAL_STRING_CAST_HEX;
  } else if (!strcmp(nm, "bin")) {
    *which = JAL_STRING_CAST_BIN;
  } else {
    return BOOLEAN_FALSE;
  }
  return BOOLEAN_TRUE;
}

boolean_t jal_string_cast_is(const char *nm)
{
  jal_string_cast_t unused;

  return jal_string_cast_from_name(nm, &unused);
}

/* hard-reserved cast names (user decision 2026-10-06): the `name(`
 * call shape is ALWAYS the cast, even when a symbol of that name
 * resolves. Bare names keep ordinary resolution (library formals
 * named `bin` keep working). `dec` was removed entirely -- chains
 * and string() already render decimal. */
boolean_t jal_string_cast_hard_is(const char *nm)
{
  return (!strcmp(nm, "hex") || !strcmp(nm, "bin"))
    ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* a fresh global const string carrying exactly `txt` */
static value_t jal_string_text_const(pfile_t *pf, const char *pfx,
    const char *txt, unsigned len)
{
  static unsigned txt_no;
  char            name[JAL_STRING_NUMBUF_NAME_SZ];
  variable_def_t  byte_def;
  variable_def_t  arr_def;
  variable_t      var;
  value_t         val;
  unsigned        ii;

  sprintf(name, "_string_%s%u", pfx, txt_no);
  txt_no++;
  byte_def = jal_string_byte_def_alloc(VARIABLE_DEF_FLAG_CONST);
  arr_def = variable_def_alloc(0, VARIABLE_DEF_TYPE_ARRAY,
      VARIABLE_DEF_FLAG_CONST | VARIABLE_DEF_FLAG_STRING, 0);
  val = VALUE_NONE;
  if (!byte_def || !arr_def) {
    if (byte_def) {
      variable_def_free(byte_def);
    }
    if (arr_def) {
      variable_def_free(arr_def);
    }
    return VALUE_NONE;
  }
  if ((RESULT_OK != variable_def_member_add(arr_def, 0, byte_def,
          (variable_ct_t) len))
      || (RESULT_OK != pfile_variable_alloc(pf,
          PFILE_VARIABLE_ALLOC_GLOBAL, name, arr_def, VARIABLE_NONE,
          &var))) {
    variable_def_free(arr_def);
    return VALUE_NONE;
  }
  for (ii = 0; ii < len; ii++) {
    variable_const_set(var, arr_def, ii,
        (variable_const_t) (unsigned char) txt[ii]);
  }
  val = value_alloc(var);
  variable_release(var);
  return val;
}

/* digits of cn in `base`, zero-padded to at least `width` (pad-is-
 * minimum: a value needing more digits renders full width); '-' first
 * when neg. Natural widths: hex 2*sz, bin 8*sz, dec minimal. */
static value_t jal_string_const_int_text(pfile_t *pf, variable_const_t cn,
    unsigned neg, variable_sz_t sz, unsigned base, unsigned width)
{
  char              digits[132];
  unsigned          ndig;
  unsigned          ii;
  unsigned          natural;
  variable_const_t  v;

  if ((base != 10U) && (base != 16U) && (base != 2U)) {
    return VALUE_NONE;
  }
  natural = (16U == base) ? (2U * (unsigned) sz)
    : ((2U == base) ? (8U * (unsigned) sz) : 1U);
  if (width < natural) {
    width = natural;
  }
  if (width > COUNT(digits) - 2U) {
    width = COUNT(digits) - 2U;
  }
  v = cn;
  ndig = 0;
  do {
    digits[ndig++] = (char) ('0' + (int) (v % (variable_const_t) base));
    v /= (variable_const_t) base;
  } while (v != 0U);
  while (ndig < width) {
    digits[ndig++] = '0';
  }
  if (neg) {
    digits[ndig++] = '-';
  }
  for (ii = 0; ii < (ndig / 2U); ii++) {
    char tmpc = digits[ii];
    digits[ii] = digits[ndig - 1U - ii];
    digits[ndig - 1U - ii] = tmpc;
  }
  /* hex letters for digits >= 10 */
  if (16U == base) {
    for (ii = 0; ii < ndig; ii++) {
      if ((digits[ii] >= 'a') && (digits[ii] <= 'f')) {
        /* unreachable via '0'+d; map 0x3a..0x3f here */
      }
      if ((digits[ii] > '9') && (digits[ii] < 'a')) {
        digits[ii] = (char) ('a' + (digits[ii] - '9') - 1);
      }
    }
  }
  return jal_string_text_const(pf, "cast", digits, ndig);
}

/* stage 2: runtime scalar cast -- render through the base/pad-
 * generalized number machinery and build the result exactly like a
 * one-item & chain (compiler heap-string cell, concat-emit copy) */
static value_t jal_string_cast_runtime_result(pfile_t *pf, value_t operand,
    unsigned base, unsigned pad)
{
  value_t items[1];
  value_t lens[1];
  value_t temp;
  value_t item;
  value_t len;

  if (!jal_string_runtime_ensure(pf)) {
    value_release(operand);
    return VALUE_NONE;
  }
  /* bits render "1"/"0": materialize through a byte temp (the number
   * machinery rejects bit defs) */
  if (value_is_bit(operand)) {
    value_t tmp;

    tmp = pfile_value_temp_get(pf, VARIABLE_DEF_TYPE_INTEGER, 1);
    if (tmp) {
      jal_string_assign_value(pf, tmp, operand);
      value_release(operand);
      operand = tmp;
    }
  }
  len = VALUE_NONE;
  item = jal_string_number_source_emit_ex(pf, operand, &len, base, pad);
  value_release(operand);
  if (!item) {
    value_release(len);
    return VALUE_NONE;
  }
  temp = jal_string_chain_var_get(pf);
  if (!temp) {
    value_release(len);
    value_release(item);
    return VALUE_NONE;
  }
  items[0] = item;
  lens[0]  = len;
  jal_string_concat_emit(pf, temp, items, lens, 1);
  value_lock(temp);
  value_release(len);
  value_release(item);
  return temp;
}

/* stage 2b: runtime ARRAY cast. `string()` copies the bytes verbatim
 * (a STRING-flagged def clone makes the array a plain chain source);
 * hex/dec/bin render each element unrolled through the generalized
 * number machinery (pad raised to the element's natural width, exactly
 * the stage-1 const semantics) and concat-emit appends all items. */
#define JAL_STRING_CAST_ARRAY_MAX 16

static value_t jal_string_cast_runtime_array(pfile_t *pf, value_t operand,
    jal_string_cast_t which, unsigned pad)
{
  variable_def_t         odef;
  variable_def_member_t  mbr;
  variable_def_t         inner;
  variable_ct_t          ct;
  variable_sz_t          esz;
  value_t               *items;
  value_t               *lens;
  value_t                temp;
  variable_ct_t          ii;

  odef = value_def_get(operand);
  mbr  = variable_def_member_get(odef);
  inner = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
  ct   = mbr ? variable_def_member_ct_get(mbr) : 0;
  if (!inner || !ct) {
    pfile_log(pf, PFILE_LOG_ERR,
        "cast: array operand needs a known element count");
    value_release(operand);
    return VALUE_NONE;
  }
  /* the per-element unroll limit applies to hex/bin/dec; the STRING
   * cast is ONE verbatim source (no unroll), so byte buffers of any
   * size work -- same capability as the bare `s & arr` chain form */
  if ((JAL_STRING_CAST_STRING != which)
      && (ct > JAL_STRING_CAST_ARRAY_MAX)) {
    pfile_log(pf, PFILE_LOG_ERR,
        "cast: runtime array operand limited to %u elements "
        "(constant arrays fold without limit)",
        (unsigned) JAL_STRING_CAST_ARRAY_MAX);
    value_release(operand);
    return VALUE_NONE;
  }
  if (!jal_string_runtime_ensure(pf)) {
    value_release(operand);
    return VALUE_NONE;
  }
  esz = variable_def_sz_get(inner);
  items = MALLOC((size_t) ct * sizeof(*items));
  lens  = MALLOC((size_t) ct * sizeof(*lens));
  temp  = VALUE_NONE;
  if (!items || !lens) {
    pfile_log_syserr(pf, ENOMEM);
    FREE(items);
    FREE(lens);
    value_release(operand);
    return VALUE_NONE;
  }
  if (JAL_STRING_CAST_STRING == which) {
    /* bytes-as-text: a STRING-flagged dup of the array def makes the
     * whole array a single chain source (length = element count) */
    variable_def_t sdef;

    sdef = variable_def_dup(odef);
    if (sdef) {
      sdef = variable_def_flags_change(sdef,
          variable_def_flags_get_all(sdef) | VARIABLE_DEF_FLAG_STRING);
    }
    if (!sdef) {
      pfile_log_syserr(pf, ENOMEM);
    } else {
      value_def_set(operand, sdef);
      lens[0] = jal_string_source_length_snapshot_get(pf, operand);
      temp = jal_string_chain_var_get(pf);
      if (temp) {
        items[0] = operand;
        jal_string_concat_emit(pf, temp, items, lens, 1);
        value_lock(temp);
        value_release(items[0]);
      }
      value_release(lens[0]);
    }
    if (!temp) {
      value_release(operand);
    }
    FREE(items);
    FREE(lens);
    return temp;
  }
  if (variable_def_flag_test(inner, VARIABLE_DEF_FLAG_BIT)
    || (VARIABLE_DEF_TYPE_INTEGER != variable_def_type_get(inner))
    || (esz > 4U)) {
    pfile_log(pf, PFILE_LOG_ERR,
        "cast: array elements must be byte/word/dword");
    value_release(operand);
    FREE(items);
    FREE(lens);
    return VALUE_NONE;
  }
  {
    unsigned base;
    unsigned natural;
    unsigned pad_e;

    base = (JAL_STRING_CAST_HEX == which) ? 16U
      : ((JAL_STRING_CAST_BIN == which) ? 2U : 10U);
    natural = (16U == base) ? (2U * (unsigned) esz)
      : ((2U == base) ? (8U * (unsigned) esz) : 1U);
    pad_e = (pad > natural) ? pad : natural;
    for (ii = 0; ii < ct; ii++) {
      value_t elem;
      value_t ofs;

      elem = value_clone(operand);
      ofs  = pfile_constant_get(pf, (variable_const_t) ii * esz,
          VARIABLE_DEF_NONE);
      items[ii] = VALUE_NONE;
      lens[ii]  = VALUE_NONE;
      if (elem && ofs) {
        value_def_set(elem, inner);
        value_baseofs_set(elem, ofs);
        items[ii] = jal_string_number_source_emit_ex(pf, elem, &lens[ii],
            base, pad_e);
      }
      value_release(ofs);
      value_release(elem);
      if (!items[ii]) {
        pfile_log(pf, PFILE_LOG_ERR,
            "cast: element %u failed to render", (unsigned) ii);
        while (ii) {
          ii--;
          value_release(lens[ii]);
          value_release(items[ii]);
        }
        value_release(operand);
        FREE(items);
        FREE(lens);
        return VALUE_NONE;
      }
    }
    value_release(operand);
    temp = jal_string_chain_var_get(pf);
    if (temp) {
      jal_string_concat_emit(pf, temp, items, lens, ct);
      value_lock(temp);
    }
    for (ii = 0; ii < ct; ii++) {
      value_release(lens[ii]);
      value_release(items[ii]);
    }
  }
  FREE(items);
  FREE(lens);
  return temp;
}

/* cast entry: CURRENT is the cast name, which has already FAILED
 * ordinary symbol resolution (jal_value_find returns clean FALSE for
 * the four names). Only the call shape `name(` is the cast; anything
 * else is the not-defined error. */
value_t jal_string_cast_parse_emit(pfile_t *pf)
{
  jal_string_cast_t which;
  const char       *name;
  char             *save;
  value_t           operand;
  unsigned          pad;
  value_t           result;

  result   = VALUE_NONE;
  name = pf_token_get(pf, PF_TOKEN_CURRENT);
  if (!jal_string_cast_from_name(name, &which)) {
    return VALUE_NONE;
  }
  save = STRDUP(name);
  if (!save) {
    pfile_log_syserr(pf, ENOMEM);
    return VALUE_NONE;
  }
  pf_token_get(pf, PF_TOKEN_NEXT);
  if (!pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, "(")) {
    pfile_log(pf, PFILE_LOG_ERR,
        "expected '(' -- '%s' is not defined (reserved for the text "
        "cast)", save);
    FREE(save);
    return VALUE_NONE;
  }
  pf_token_get(pf, PF_TOKEN_NEXT);
  operand = jal_parse_expr(pf);
  pad = 0U;
  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, ",")) {
    value_t padv;

    pf_token_get(pf, PF_TOKEN_NEXT);
    padv = jal_parse_expr(pf);
    if (padv) {
      if (!value_is_const(padv)) {
        pfile_log(pf, PFILE_LOG_ERR,
            "constant expression expected (pad digits)");
      } else {
        pad = (unsigned) value_const_get(padv);
        if (pad > 128U) {
          pfile_log(pf, PFILE_LOG_ERR, "pad digits out of range");
          pad = 0U;
        }
      }
      value_release(padv);
    }
  }
  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, ")")) {
    pf_token_get(pf, PF_TOKEN_NEXT);
  }
  if (!operand || pfile_errct_get(pf)) {
    value_release(operand);
    FREE(save);
    return VALUE_NONE;
  }

  /* string(s) on a string-typed operand is the identity */
  if ((JAL_STRING_CAST_STRING == which)
      && jal_string_is_source(operand)
      && !value_is_array(operand)) {
    result = operand;
    FREE(save);
    return result;
  }

  if (value_is_const(operand)) {
    variable_const_t cn;
    unsigned        neg;
    variable_sz_t   sz;

    if (value_is_array(operand)) {
      variable_t        ovar;
      variable_def_t    odef;
      variable_def_member_t mbr;
      variable_def_t    inner;
      const char       *data;
      variable_ct_t     ct;
      char             *txt;
      unsigned          txtlen;
      unsigned          ii;
      value_t           rc;

      ovar = value_variable_get(operand);
      odef = value_def_get(operand);
      mbr = variable_def_member_get(odef);
      inner = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
      data = ovar ? variable_data_get(ovar) : 0;
      ct = mbr ? variable_def_member_ct_get(mbr) : 0;
      if (!data || !ct || (ct > 64U)) {
        pfile_log(pf, PFILE_LOG_ERR,
            "cast: const array too large (limit 64 elements)");
        value_release(operand);
        FREE(save);
        return VALUE_NONE;
      }
      if (JAL_STRING_CAST_STRING == which) {
        if (inner
          && (VARIABLE_DEF_TYPE_INTEGER == variable_def_type_get(inner))
          && (1 == variable_def_sz_get(inner))
          && !variable_def_flag_test(inner, VARIABLE_DEF_FLAG_BIT)) {
          rc = jal_string_text_const(pf, "cast", data, (unsigned) ct);
          value_release(operand);
          result = rc;
          FREE(save);
          return result;
        }
        pfile_log(pf, PFILE_LOG_ERR,
            "string(...) of a const array needs byte elements");
        value_release(operand);
        FREE(save);
        return VALUE_NONE;
      }
      /* hex/bin/dec of a const numeric array: element-wise render */
      if (!inner
        || (VARIABLE_DEF_TYPE_INTEGER != variable_def_type_get(inner))
        || variable_def_flag_test(inner, VARIABLE_DEF_FLAG_BIT)
        || (variable_def_sz_get(inner) > 4U)) {
        pfile_log(pf, PFILE_LOG_ERR,
            "cast: array elements must be byte/word/dword");
        value_release(operand);
        FREE(save);
        return VALUE_NONE;
      }
      txt = MALLOC((size_t) ct * 33U + 2U);
      if (!txt) {
        pfile_log_syserr(pf, ENOMEM);
        value_release(operand);
        FREE(save);
        return VALUE_NONE;
      }
      txtlen = 0;
      for (ii = 0; ii < (unsigned) ct; ii++) {
        variable_const_t ev;
        unsigned        esz;
        unsigned        eb;

        esz = variable_def_sz_get(inner);
        ev = 0;
        memcpy(&ev, data + ((size_t) ii * esz), esz);
        eb = (JAL_STRING_CAST_HEX == which) ? 16U : 2U;
        {
          char           one[40];
          unsigned       od;
          unsigned       width;
          variable_const_t w2;
          unsigned       natural;

          natural = (16U == eb) ? (2U * esz) : ((2U == eb) ? (8U * esz) : 1U);
          width = (pad > natural) ? pad : natural;
          w2 = ev;
          od = 0;
          do {
            one[od++] = (char) ('0' + (int) (w2 % (variable_const_t) eb));
            w2 /= (variable_const_t) eb;
          } while (w2 != 0U);
          while (od < width) {
            one[od++] = '0';
          }
          while (od > 0U) {
            char ch = one[--od];
            if ((ch > '9') && (ch < 'a')) {
              ch = (char) ('a' + (ch - '9') - 1);
            }
            txt[txtlen++] = ch;
          }
        }
      }
      rc = jal_string_text_const(pf, "cast", txt, txtlen);
      FREE(txt);
      value_release(operand);
      result = rc;
      FREE(save);
      return result;
    }
    if (VARIABLE_DEF_TYPE_FLOAT == value_type_get(operand)) {
      value_t rc;

      if ((JAL_STRING_CAST_HEX == which)
        || (JAL_STRING_CAST_BIN == which)) {
        pfile_log(pf, PFILE_LOG_ERR,
            "%s(...) of a float is not supported", save);
        value_release(operand);
        FREE(save);
        return VALUE_NONE;
      }
      rc = jal_string_float_const_source_emit(pf,
          value_const_float_get(operand));
      value_release(operand);
      result = rc;
      FREE(save);
      return result;
    }
    cn = value_const_get(operand);
    neg = 0U;
    sz = 1;
    if (value_is_signed(operand)
        && (0 != (cn >> (VARIABLE_CONST_BIT_CT - 1)))) {
      cn = (~cn) + 1U;
      neg = 1U;
    }
    /* natural width from the VALUE's magnitude -- universal const defs
     * are 16 bytes and would pad every hex to 32 digits */
    while ((sz < 8U) && (0U != (cn >> (8U * (unsigned) sz)))) {
      sz++;
    }
    {
      value_t rc;
      unsigned base;

      base = (JAL_STRING_CAST_HEX == which) ? 16U : 2U;
      rc = (JAL_STRING_CAST_STRING == which)
        ? jal_string_number_const_source_emit(pf, cn, neg)
        : jal_string_const_int_text(pf, cn, neg, sz, base, pad);
      value_release(operand);
      result = rc;
      FREE(save);
      return result;
    }
  }

  /* stage 2: runtime scalars render through the generalized number
   * machinery into a compiler heap-string cell; runtime arrays render
   * per element (stage 2b) */
  if (value_is_array(operand)) {
    result = jal_string_cast_runtime_array(pf, operand, which, pad);
    FREE(save);
    return result;
  }
  {
    unsigned base;

    base = (JAL_STRING_CAST_HEX == which) ? 16U
      : ((JAL_STRING_CAST_BIN == which) ? 2U : 10U);
    result = jal_string_cast_runtime_result(pf, operand, base, pad);
    FREE(save);
    return result;
  }
}

value_t jal_string_builtin_parse_emit(pfile_t *pf, const char *name)
{
  value_t dst;
  value_t src;
  value_t max;
  value_t result;
  boolean_t is_append;
  boolean_t is_append_truncate;
  boolean_t is_truncate;
  boolean_t is_from_array;
  boolean_t is_from_array_truncate;
  boolean_t is_to_array;
  boolean_t is_to_array_truncate;
  boolean_t is_heap_compact;

  dst = VALUE_NONE;
  src = VALUE_NONE;
  max = VALUE_NONE;
  result = VALUE_NONE;
  is_append = jal_string_builtin_pub_internal_eq(name, "string_append");
  is_append_truncate = jal_string_builtin_pub_internal_eq(name,
      "string_append_truncate");
  is_truncate = jal_string_builtin_pub_internal_eq(name, "string_truncate");
  is_from_array = jal_string_builtin_pub_internal_eq(name,
      "string_from_array");
  is_from_array_truncate = jal_string_builtin_pub_internal_eq(name,
      "string_from_array_truncate");
  is_to_array = jal_string_builtin_pub_internal_eq(name, "string_to_array");
  is_to_array_truncate = jal_string_builtin_pub_internal_eq(name,
      "string_to_array_truncate");
  is_heap_compact = jal_string_builtin_pub_internal_eq(name,
      "string_heap_compact");
  pf_token_get(pf, PF_TOKEN_NEXT);
  if (!jal_string_builtin_sep_get(pf, "(")) {
    return VALUE_NONE;
  }
  if (is_heap_compact) {
    cmd_t nopc;

    result = pfile_value_temp_get(pf, VARIABLE_DEF_TYPE_BOOLEAN, 1U);
    if (VALUE_NONE == result) {
      pfile_log_syserr(pf, ENOMEM);
      return VALUE_NONE;
    }
    if (!jal_string_builtin_sep_get(pf, ")")) {
      pfile_log(pf, PFILE_LOG_ERR,
          "string_heap_compact() / _string_heap_compact() has no parameters");
      value_release(result);
      return VALUE_NONE;
    }
#if JALV2_STRING_HEAP_COMPACT_CALL
    /* Only the public builtin may outline to `CALL string_heap_compact`.
     * `_string_heap_compact()` is what the strings_dynamic wrappers call from the
     * wrapper body; it must stay NOP+defer or we recurse into the same JAL
     * procedure instead of expanding the pcode (gpsim: compact “fails”). */
    if (0 == strcmp(name, "string_heap_compact")
        && jal_string_heap_compact_jal_call_emit(pf, result)) {
      goto done;
    }
#endif
    nopc = cmd_alloc(CMD_TYPE_NOP, 0U);
    if (CMD_NONE == nopc) {
      pfile_log_syserr(pf, ENOMEM);
      value_release(result);
      return VALUE_NONE;
    }
    pfile_cmd_add(pf, nopc);
    pfile_str_heap_defer_add(pf, nopc, result);
    goto done;
  }
  if (is_append_truncate || is_append || is_truncate || is_from_array
      || is_from_array_truncate) {
    dst = jal_parse_value(pf, JAL_PARSE_VALUE_TYPE_LVALUE);
    if (!dst || !jal_string_is_dynamic(dst)) {
      pfile_log(pf, PFILE_LOG_ERR, "dynamic string destination expected");
      goto done;
    }
  } else {
    dst = jal_parse_value(pf, JAL_PARSE_VALUE_TYPE_LVALUE);
    if (!dst || !jal_string_builtin_byte_array_arg_ok(dst)) {
      pfile_log(pf, PFILE_LOG_ERR, "byte array destination expected");
      goto done;
    }
  }
  if (!jal_string_builtin_sep_get(pf, ",")) {
    goto done;
  }
  if (is_truncate) {
    max = jal_parse_expr(pf);
    if (!max) {
      goto done;
    }
    max = jal_string_max_index_coerce(pf, max);
    if (jal_string_builtin_sep_get(pf, ")")) {
      jal_string_silent_truncate_begin(pf);
      result = jal_string_truncate_emit(pf, dst, max);
      jal_string_silent_truncate_end(pf);
    }
    goto done;
  }
  if (is_from_array || is_from_array_truncate) {
    /* Keep `byte in name[]` as POINTER (do not rvalue-deref to a scalar). */
    pfile_parse_call_data_ptr_arg_push(pf);
  }
  src = jal_parse_expr(pf);
  if (is_from_array || is_from_array_truncate) {
    pfile_parse_call_data_ptr_arg_pop(pf);
  }
  if (!src) {
    goto done;
  }
  if (is_append_truncate || is_append || is_to_array
      || is_to_array_truncate) {
    if (!jal_string_is_source(src)) {
      pfile_log(pf, PFILE_LOG_ERR, "string source expected");
      goto done;
    }
  } else if (!jal_string_builtin_byte_array_arg_ok(src)) {
    pfile_log(pf, PFILE_LOG_ERR, "byte array source expected");
    goto done;
  }
  if (is_append_truncate || is_from_array_truncate
      || is_to_array_truncate) {
    if (!jal_string_builtin_sep_get(pf, ",")) {
      goto done;
    }
    max = jal_parse_expr(pf);
    if (!max) {
      goto done;
    }
    max = jal_string_max_index_coerce(pf, max);
  }
  if (!jal_string_builtin_sep_get(pf, ")")) {
    goto done;
  }
  jal_string_silent_truncate_begin(pf);
  if (is_append) {
    result = jal_string_append_emit(pf, dst, src);
  } else if (is_append_truncate) {
    result = jal_string_append_truncate_emit(pf, dst, src, max);
  } else if (is_from_array) {
    result = jal_string_from_array_emit(pf, dst, src, VALUE_NONE,
        BOOLEAN_FALSE);
  } else if (is_from_array_truncate) {
    result = jal_string_from_array_emit(pf, dst, src, max, BOOLEAN_TRUE);
  } else if (is_to_array) {
    result = jal_string_to_array_emit(pf, dst, src, VALUE_NONE,
        BOOLEAN_FALSE);
  } else if (is_to_array_truncate) {
    result = jal_string_to_array_emit(pf, dst, src, max, BOOLEAN_TRUE);
  }
  jal_string_silent_truncate_end(pf);

done:
  value_release(max);
  value_release(src);
  value_release(dst);
  return result;
}

static value_t jal_str_cpt_wk_bofs(pfile_t *pf, value_t row, unsigned c)
{
  value_t r2;
  value_t r4;
  value_t r8;
  value_t base;
  value_t cval;
  value_t a;

  UNUSED(pf);
  /* Byte offset of field c in row: row * 8 + c * 2 (four little-endian u16). */
  r2  = jal_string_binary_emit(pf, OPERATOR_ADD, row, row);
  r4  = jal_string_binary_emit(pf, OPERATOR_ADD, r2, r2);
  value_release(r2);
  r8  = jal_string_binary_emit(pf, OPERATOR_ADD, r4, r4);
  value_release(r4);
  base = r8;
  if (0U == c) {
    return base;
  }
  cval = jal_string_const_get(pf, (variable_const_t)(c * 2U));
  a    = jal_string_binary_emit(pf, OPERATOR_ADD, base, cval);
  value_release(base);
  value_release(cval);
  return a;
}

/* Read u16 from work-table byte offset (always 2 bytes); see `wk_bofs`. */
static value_t jal_str_cpt_wk_load_u16(pfile_t *pf, value_t w, value_t bofs)
{
  value_t lo;
  value_t b1;
  value_t hi;
  value_t sh;
  value_t hi8;

  lo = jal_string_index_byte_lvalue_get(pf, w, value_clone(bofs));
  b1 = jal_string_add_const(pf, bofs, 1);
  hi = jal_string_index_byte_lvalue_get(pf, w, b1);
  value_release(b1);
  sh = jal_string_const_get(pf, 8);
  hi8 = jal_string_binary_emit(pf, OPERATOR_SHIFT_LEFT, hi, sh);
  value_release(sh);
  return jal_string_binary_emit(pf, OPERATOR_ADD, lo, hi8);
}

/* Write u16 to work-table byte offset (always 2 bytes, little-endian). */
static void jal_str_cpt_wk_store_u16(pfile_t *pf, value_t w, value_t bofs,
    value_t val)
{
  value_t l0;
  value_t b1;
  value_t l1;
  value_t v0;
  value_t sh;
  value_t sr;
  value_t v1c;

  l0 = jal_string_index_byte_lvalue_get(pf, w, value_clone(bofs));
  b1 = jal_string_add_const(pf, bofs, 1);
  l1 = jal_string_index_byte_lvalue_get(pf, w, b1);
  value_release(b1);
  v0 = value_clone(val);
  value_def_set(v0, jal_string_byte_def_alloc(VARIABLE_DEF_FLAG_NONE));
  jal_string_assign_value(pf, l0, v0);
  value_release(v0);
  sh = jal_string_const_get(pf, 8);
  sr = jal_string_binary_emit(pf, OPERATOR_SHIFT_RIGHT, val, sh);
  value_release(sh);
  v1c = value_clone(sr);
  value_release(sr);
  value_def_set(v1c, jal_string_byte_def_alloc(VARIABLE_DEF_FLAG_NONE));
  jal_string_assign_value(pf, l1, v1c);
  value_release(v1c);
  value_release(l1);
  value_release(l0);
}

static value_t jal_str_cpt_wk_get(pfile_t *pf, value_t w, value_t row,
    unsigned c)
{
  value_t bofs;
  value_t v;

  bofs = jal_str_cpt_wk_bofs(pf, row, c);
  if (2U == jal_string_current_index_size_get(pf)) {
    v = jal_string_index_load_emit(pf, w, bofs);
  } else {
    v = jal_str_cpt_wk_load_u16(pf, w, bofs);
  }
  value_release(bofs);
  return v;
}

static void jal_str_cpt_wk_set(pfile_t *pf, value_t w, value_t row,
    unsigned c, value_t val)
{
  value_t bofs;

  bofs = jal_str_cpt_wk_bofs(pf, row, c);
  if (2U == jal_string_current_index_size_get(pf)) {
    jal_string_index_store_emit(pf, w, bofs, val);
  } else {
    jal_str_cpt_wk_store_u16(pf, w, bofs, val);
  }
  value_release(bofs);
}

static void jal_str_cpt_wk_row_swap_emit(pfile_t *pf, value_t w, value_t ra,
    value_t rb)
{
  value_t  v;
  value_t  v2;
  unsigned bi;

  for (bi = 0U; bi < 4U; bi++) {
    v  = jal_str_cpt_wk_get(pf, w, ra, bi);
    v2 = jal_str_cpt_wk_get(pf, w, rb, bi);
    jal_str_cpt_wk_set(pf, w, ra, bi, v2);
    jal_str_cpt_wk_set(pf, w, rb, bi, v);
    value_release(v2);
    value_release(v);
  }
}

/* Compaction prefix sums and heap block offsets can exceed 255 while
 * still using pragma string index byte temps; widen selected
 * accumulators. */
static value_t jal_string_heap_cpt_u16_temp_get(pfile_t *pf)
{
  return pfile_value_temp_get(pf, VARIABLE_DEF_TYPE_INTEGER,
      (variable_sz_t) 2);
}

/* Assign a byte-sized dynamic value into a u16 temp without narrowing the
 * destination (plain ASSIGN from byte can shrink the temp's def so later
 * `src_base + k` is emitted as a byte add and FSR indexing wraps past 255). */
static void jal_string_heap_cpt_u16_assign_from_byte(pfile_t *pf, value_t dst_w,
    value_t src_b)
{
  value_t zword;

  zword = pfile_constant_get(pf, 0, value_def_get(dst_w));
  jal_string_assign_value(pf, dst_w, zword);
  value_release(zword);
  pfile_cmd_op_add(pf, OPERATOR_ADD, &dst_w, dst_w, src_b);
}

static void jal_str_cpt_heap_block_copy_emit(pfile_t *pf, value_t src0,
    value_t d0, value_t blen)
{
  label_t loop;
  label_t done;
  value_t k;
  value_t src_base;
  value_t dst_base;
  value_t sum_src;
  value_t sum_dst;
  value_t sbyte;
  value_t dbyte;
  value_t one;
  value_t z;

  loop = pfile_label_alloc(pf, 0U);
  done = pfile_label_alloc(pf, 0U);
  k        = jal_string_heap_cpt_u16_temp_get(pf);
  src_base = jal_string_heap_cpt_u16_temp_get(pf);
  dst_base = jal_string_heap_cpt_u16_temp_get(pf);
  sum_src  = jal_string_heap_cpt_u16_temp_get(pf);
  sum_dst  = jal_string_heap_cpt_u16_temp_get(pf);
  one  = jal_string_const_get(pf, 1);
  z    = pfile_constant_get(pf, 0, value_def_get(k));
  sbyte = VALUE_NONE;
  dbyte = VALUE_NONE;
  jal_string_heap_cpt_u16_assign_from_byte(pf, src_base, src0);
  jal_string_heap_cpt_u16_assign_from_byte(pf, dst_base, d0);
  jal_string_assign_value(pf, k, z);
  value_release(z);
  pfile_cmd_label_add(pf, loop);
  jal_string_branch_if_value_cmp(pf, OPERATOR_GE, k, blen,
      CMD_BRANCHCOND_TRUE, done);
  /* `src_base + k` / `dst_base + k` can exceed 255. Route ADD into preallocated
   * u16 temps so the cmd destination is word-sized; a fresh byte temp from
   * `jal_string_binary_emit` can truncate the sum before FSR setup. */
  pfile_cmd_op_add(pf, OPERATOR_ADD, &sum_src, src_base, k);
  sbyte = jal_string_heap_elem_get(pf, sum_src);
  pfile_cmd_op_add(pf, OPERATOR_ADD, &sum_dst, dst_base, k);
  dbyte = jal_string_heap_elem_get(pf, sum_dst);
  jal_string_assign_value(pf, dbyte, sbyte);
  value_release(dbyte);
  value_release(sbyte);
  pfile_cmd_op_add(pf, OPERATOR_ADD, &k, k, one);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE,
      loop, VALUE_NONE, VALUE_NONE, 0);
  pfile_cmd_label_add(pf, done);
  value_release(one);
  value_release(sum_dst);
  value_release(sum_src);
  value_release(dst_base);
  value_release(src_base);
  value_release(k);
  label_release(done);
  label_release(loop);
}

static value_t jal_str_cpt_work_ensure(pfile_t *pf, size_t row_ct)
{
  value_t         wk;
  variable_t      v;
  variable_def_t  bd;
  variable_def_t  ad;
  result_t         rc;
  size_t          needb;
  variable_ct_t   elem_ct;

  wk = pfile_value_find(pf, PFILE_LOG_NONE, "_pfile_strc_cpt");
  if (VALUE_NONE != wk) {
    return wk;
  }
  if (0U == jal_string_current_index_size_get(pf) || 0U == row_ct) {
    return VALUE_NONE;
  }
  needb = 8U * row_ct;
  if (needb > (size_t) UINT32_MAX) {
    pfile_log(pf, PFILE_LOG_ERR,
        "string_heap_compact: work table is %lu bytes (8 * handle_count), "
        "exceeding internal limit %lu",
        (unsigned long) needb, (unsigned long) (size_t) UINT32_MAX);
    return VALUE_NONE;
  }
  elem_ct = (variable_ct_t) needb;
  v   = VARIABLE_NONE;
  bd  = jal_string_byte_def_alloc(VARIABLE_DEF_FLAG_NONE);
  ad  = variable_def_alloc(0U, VARIABLE_DEF_TYPE_ARRAY,
      VARIABLE_DEF_FLAG_NONE, 0U);
  if (!bd || !ad) {
    pfile_log_syserr(pf, ENOMEM);
    if (ad) {
      variable_def_free(ad);
    }
    if (bd) {
      variable_def_free(bd);
    }
    return VALUE_NONE;
  }
  if (RESULT_OK != variable_def_member_add(ad, 0, bd, elem_ct)) {
    pfile_log_syserr(pf, ENOMEM);
    variable_def_free(ad);
    return VALUE_NONE;
  }
  rc = pfile_variable_alloc(pf, PFILE_VARIABLE_ALLOC_GLOBAL,
      "_pfile_strc_cpt", ad, VARIABLE_NONE, &v);
  if (RESULT_OK != rc) {
    pfile_log_syserr(pf, rc);
    variable_def_free(ad);
    return VALUE_NONE;
  }
  wk = value_alloc(v);
  variable_release(v);
  return wk;
}

static void jal_str_cpt_bubble_sort_emit(pfile_t *pf, value_t w, value_t nmo,
    value_t i, value_t j, value_t inlim)
{
  label_t  ohead;
  label_t  oend;
  label_t  ihead;
  label_t  oiinc;
  label_t  swap_lbl;
  label_t  jjoin;
  value_t  a0;
  value_t  b0;
  value_t  onev;
  value_t  rb;
  value_t  limi;
  value_t  z;

  ohead    = pfile_label_alloc(pf, 0U);
  oend     = pfile_label_alloc(pf, 0U);
  ihead    = pfile_label_alloc(pf, 0U);
  oiinc    = pfile_label_alloc(pf, 0U);
  swap_lbl = pfile_label_alloc(pf, 0U);
  jjoin    = pfile_label_alloc(pf, 0U);
  onev     = jal_string_const_get(pf, 1U);
  z        = jal_string_const_get(pf, 0U);
  jal_string_assign_value(pf, i, z);
  pfile_cmd_label_add(pf, ohead);
  jal_string_branch_if_value_cmp(pf, OPERATOR_GE, i, nmo, CMD_BRANCHCOND_TRUE,
      oend);
  limi = jal_string_binary_emit(pf, OPERATOR_SUB, nmo, i);
  jal_string_assign_value(pf, inlim, limi);
  /* limi: pfile already releases assign-source in jal_string_assign_value */
  jal_string_assign_value(pf, j, z);
  pfile_cmd_label_add(pf, ihead);
  jal_string_branch_if_value_cmp(pf, OPERATOR_GE, j, inlim, CMD_BRANCHCOND_TRUE,
      oiinc);
  rb = jal_string_add_const(pf, j, 1U);
  a0 = jal_str_cpt_wk_get(pf, w, j, 0U);
  b0 = jal_str_cpt_wk_get(pf, w, rb, 0U);
  jal_string_branch_if_value_cmp(pf, OPERATOR_GT, a0, b0, CMD_BRANCHCOND_TRUE,
      swap_lbl);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE, jjoin,
      VALUE_NONE, VALUE_NONE, 0);
  pfile_cmd_label_add(pf, swap_lbl);
  jal_str_cpt_wk_row_swap_emit(pf, w, j, rb);
  pfile_cmd_label_add(pf, jjoin);
  value_release(b0);
  value_release(a0);
  value_release(rb);
  pfile_cmd_op_add(pf, OPERATOR_ADD, &j, j, onev);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE, ihead,
      VALUE_NONE, VALUE_NONE, 0);
  pfile_cmd_label_add(pf, oiinc);
  pfile_cmd_op_add(pf, OPERATOR_ADD, &i, i, onev);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE, ohead,
      VALUE_NONE, VALUE_NONE, 0);
  pfile_cmd_label_add(pf, oend);
  value_release(z);
  value_release(onev);
  label_release(jjoin);
  label_release(swap_lbl);
  label_release(oiinc);
  label_release(ihead);
  label_release(oend);
  label_release(ohead);
}

/* endu_out: 16-bit packed tail after prefix (sum of block sizes). Used by
 * free_tail; must not use a byte temp — totals can reach heap_sz (255) with
 * non-zero remainder in intermediate sums, and byte acc would truncate. */
static void jal_str_cpt_prefix_emit(pfile_t *pf, value_t w, value_t n,
    value_t k, value_t endu_out)
{
  label_t  head;
  label_t  end;
  value_t  bsz;
  value_t  nnew;
  value_t  onev;
  value_t  z;
  value_t  acc16;

  bsz  = VALUE_NONE;
  nnew = VALUE_NONE;
  head = pfile_label_alloc(pf, 0U);
  end  = pfile_label_alloc(pf, 0U);
  acc16 = jal_string_heap_cpt_u16_temp_get(pf);
  onev = jal_string_const_get(pf, 1U);
  z    = jal_string_const_get(pf, 0U);
  jal_string_assign_value(pf, k, z);
  jal_string_assign_value(pf, acc16, z);
  pfile_cmd_label_add(pf, head);
  value_release(z);
  jal_string_branch_if_value_cmp(pf, OPERATOR_GE, k, n, CMD_BRANCHCOND_TRUE,
      end);
  bsz  = jal_str_cpt_wk_get(pf, w, k, 1U);
  jal_str_cpt_wk_set(pf, w, k, 2U, acc16);
  nnew = jal_string_binary_emit(pf, OPERATOR_ADD, acc16, bsz);
  /* bsz: pfile released as operand of add; nnew: released as assign source */
  jal_string_assign_value(pf, acc16, nnew);
  pfile_cmd_op_add(pf, OPERATOR_ADD, &k, k, onev);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE, head,
      VALUE_NONE, VALUE_NONE, 0);
  pfile_cmd_label_add(pf, end);
  jal_string_assign_value(pf, endu_out, acc16);
  value_release(acc16);
  value_release(onev);
  label_release(end);
  label_release(head);
}

static void jal_str_cpt_moves_emit(pfile_t *pf, value_t w, value_t n,
    value_t nmo, value_t mi, value_t krow)
{
  label_t  mhead;
  label_t  mdone;
  label_t  mskip;
  value_t  oold;
  value_t  nnewv;
  value_t  bsz;
  value_t  onev;
  value_t  z;
  value_t  subv;

  mhead  = pfile_label_alloc(pf, 0U);
  mdone  = pfile_label_alloc(pf, 0U);
  mskip  = pfile_label_alloc(pf, 0U);
  oold   = VALUE_NONE;
  nnewv  = VALUE_NONE;
  bsz    = VALUE_NONE;
  onev   = jal_string_const_get(pf, 1U);
  z      = jal_string_const_get(pf, 0U);
  jal_string_assign_value(pf, mi, z);
  pfile_cmd_label_add(pf, mhead);
  value_release(z);
  jal_string_branch_if_value_cmp(pf, OPERATOR_GE, mi, n, CMD_BRANCHCOND_TRUE,
      mdone);
  subv = jal_string_binary_emit(pf, OPERATOR_SUB, nmo, mi);
  jal_string_assign_value(pf, krow, subv);
  /* subv: released as assign source */
  oold  = jal_str_cpt_wk_get(pf, w, krow, 0U);
  nnewv = jal_str_cpt_wk_get(pf, w, krow, 2U);
  bsz   = jal_str_cpt_wk_get(pf, w, krow, 1U);
  jal_string_branch_if_value_cmp(pf, OPERATOR_EQ, oold, nnewv,
      CMD_BRANCHCOND_TRUE, mskip);
  jal_str_cpt_heap_block_copy_emit(pf, oold, nnewv, bsz);
  pfile_cmd_label_add(pf, mskip);
  value_release(bsz);
  value_release(nnewv);
  value_release(oold);
  pfile_cmd_op_add(pf, OPERATOR_ADD, &mi, mi, onev);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE, mhead,
      VALUE_NONE, VALUE_NONE, 0);
  pfile_cmd_label_add(pf, mdone);
  value_release(onev);
  label_release(mskip);
  label_release(mdone);
  label_release(mhead);
}

/* rem = heap_sz - endu. If rem is too small for a free block header, no new
 * node. Else one node: [next, cap, len] = fmax, (rem - header), 0 at endu.
 * Header is 3 index fields (3 * isz) bytes, matching the allocator. */
static void jal_str_cpt_free_tail_emit(pfile_t *pf, value_t endu,
    variable_const_t hsz, variable_const_t fmax, variable_sz_t isz)
{
  label_t  lsmall;
  label_t  ldone;
  value_t  c4;
  value_t  t3;
  value_t  fhsz;
  value_t  frem;
  value_t  fnon;
  value_t  fcapc;
  value_t  fzer;
  value_t  o1;
  value_t  o2;
  value_t  fhv;
  variable_const_t  hdr3;

  hdr3   = (variable_const_t)(3U * (unsigned)isz);
  lsmall = pfile_label_alloc(pf, 0U);
  ldone  = pfile_label_alloc(pf, 0U);
  c4     = jal_string_const_get(pf, (variable_const_t)(hdr3 + 1U));
  fhsz   = jal_string_index_const_get(pf, hsz);
  frem   = jal_string_binary_emit(pf, OPERATOR_SUB, fhsz, endu);
  value_release(fhsz);
  /* endu: still the running compact tail. frem, c4, t3: pfile in branch/sub. */
  jal_string_branch_if_value_cmp(pf, OPERATOR_LT, frem, c4, CMD_BRANCHCOND_TRUE,
      lsmall);
  value_release(c4);
  t3     = jal_string_const_get(pf, hdr3);
  fcapc  = jal_string_binary_emit(pf, OPERATOR_SUB, frem, t3);
  value_release(frem);
  value_release(t3);
  fnon   = jal_string_index_const_get(pf, fmax);
  fzer   = jal_string_const_get(pf, 0U);
  o1     = jal_string_add_const(pf, endu, (unsigned)isz);
  o2     = jal_string_add_const(pf, endu, (unsigned)(2U * (unsigned)isz));
  jal_string_heap_index_set(pf, endu, fnon);
  value_release(fnon);
  jal_string_heap_index_set(pf, o1, fcapc);
  value_release(fcapc);
  jal_string_heap_index_set(pf, o2, fzer);
  value_release(fzer);
  value_release(o1);
  value_release(o2);
  fhv  = jal_string_named_value_get(pf, JAL_STRING_FREE_HEAD_NAME);
  t3  = endu;
  value_lock(t3);
  jal_string_assign_value(pf, fhv, t3);
  value_release(fhv);
  value_release(t3);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE, ldone,
      VALUE_NONE, VALUE_NONE, 0U);
  pfile_cmd_label_add(pf, lsmall);
  /* frem, c4: c4 was released in C right after the branch-compare emit. */
  fnon  = jal_string_index_const_get(pf, fmax);
  fhv  = jal_string_named_value_get(pf, JAL_STRING_FREE_HEAD_NAME);
  jal_string_assign_value(pf, fhv, fnon);
  value_release(fnon);
  value_release(fhv);
  pfile_cmd_label_add(pf, ldone);
  label_release(ldone);
  label_release(lsmall);
}

static void jal_str_cpt_reg_patch_one_emit(pfile_t *pf, value_t w, value_t n,
    value_t h, value_t kk, variable_const_t ridx)
{
  label_t  lloop;
  label_t  lset;
  label_t  ldone;
  value_t  rbyte;
  value_t  nnewv;
  value_t  oneb;
  value_t  z;

  lloop = pfile_label_alloc(pf, 0U);
  lset  = pfile_label_alloc(pf, 0U);
  ldone = pfile_label_alloc(pf, 0U);
  z     = jal_string_const_get(pf, 0U);
  jal_string_assign_value(pf, kk, z);
  value_release(z);
  pfile_cmd_label_add(pf, lloop);
  jal_string_branch_if_value_cmp(pf, OPERATOR_GE, kk, n, CMD_BRANCHCOND_TRUE,
      ldone);
  rbyte = jal_str_cpt_wk_get(pf, w, kk, 3U);
  jal_string_branch_if_const_cmp(pf, OPERATOR_EQ, rbyte, ridx, CMD_BRANCHCOND_TRUE,
      lset);
  value_release(rbyte);
  oneb = jal_string_const_get(pf, 1U);
  pfile_cmd_op_add(pf, OPERATOR_ADD, &kk, kk, oneb);
  value_release(oneb);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE, lloop,
      VALUE_NONE, VALUE_NONE, 0U);
  pfile_cmd_label_add(pf, lset);
  nnewv = jal_str_cpt_wk_get(pf, w, kk, 2U);
  jal_string_handle_index_set(pf, h, JAL_STRING_FIELD_OFFSET, nnewv);
  value_release(nnewv);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE, ldone,
      VALUE_NONE, VALUE_NONE, 0U);
  pfile_cmd_label_add(pf, ldone);
  label_release(ldone);
  label_release(lset);
  label_release(lloop);
}

static void jal_str_cpt_reg_patch_all_emit(pfile_t *pf, value_t w, value_t n,
    value_t kk, size_t reg_ct)
{
  size_t   rr;
  value_t  h;
  variable_t  vart;

  for (rr = 0U; rr < reg_ct; ++rr) {
    vart = pfile_dynstr_reg_get(pf, rr);
    h    = value_alloc(vart);
    if (VALUE_NONE == h) {
      pfile_log_syserr(pf, ENOMEM);
      return;
    }
    jal_str_cpt_reg_patch_one_emit(pf, w, n, h, kk, (variable_const_t) rr);
    value_release(h);
  }
}

static void jal_string_heap_compact_fill_bld(pfile_t *pf, value_t res)
{
  value_t            wk;
  value_t            nct;
  value_t            nmo;
  value_t            i;
  value_t            j;
  value_t            inlim;
  value_t            k;
  value_t            endu_w;
  value_t            mi;
  value_t            krow;
  value_t            kk;
  value_t            heapv;
  value_t            capu;
  value_t            ofu;
  value_t            hdrb;
  value_t            bszc;
  value_t            c_r;
  value_t            z0;
  value_t            onev;
  label_t            skipr;
  label_t            empty_cpt;
  label_t            cpt_out;
  variable_const_t   hszc;
  variable_const_t   fmaxb;
  boolean_t          heap_ok;
  variable_sz_t      isz;
  size_t             reg_ct;
  size_t             r;
  value_t            z1;

  wk     = VALUE_NONE;
  nct    = VALUE_NONE;
  nmo    = VALUE_NONE;
  i      = VALUE_NONE;
  j      = VALUE_NONE;
  inlim  = VALUE_NONE;
  k      = VALUE_NONE;
  endu_w = VALUE_NONE;
  mi     = VALUE_NONE;
  krow   = VALUE_NONE;
  kk     = VALUE_NONE;
  heapv  = VALUE_NONE;
  heap_ok = jal_string_runtime_ensure(pf);
  isz     = heap_ok ? jal_string_current_index_size_get(pf) : 0U;
  reg_ct  = heap_ok ? pfile_dynstr_reg_ct_get(pf) : 0U;
  if (!heap_ok) {
    jal_string_bool_assign(pf, res, BOOLEAN_FALSE);
    return;
  }
  fmaxb  = jal_string_index_max(isz);
  if (0U == reg_ct) {
    jal_string_bool_assign(pf, res, BOOLEAN_TRUE);
    return;
  }
  heapv = jal_string_pragma_const_get(pf, "_string_heap_size");
  if (VALUE_NONE == heapv) {
    pfile_log(pf, PFILE_LOG_ERR,
        "string_heap_compact: const word _string_heap_size = <bytes> missing");
    jal_string_bool_assign(pf, res, BOOLEAN_FALSE);
    return;
  }
  hszc  = value_const_get(heapv);
  value_release(heapv);
  jal_string_silent_truncate_begin(pf);
  wk   = jal_str_cpt_work_ensure(pf, reg_ct);
  if (VALUE_NONE == wk) {
    /* OOM (`variable` alloc), oversize work table (see `work_ensure` message), */
    /* or bad internal args. */
    jal_string_silent_truncate_end(pf);
    jal_string_bool_assign(pf, res, BOOLEAN_FALSE);
    return;
  }
  nct  = jal_string_temp_index_get(pf);
  z0  = jal_string_const_get(pf, 0U);
  jal_string_assign_value(pf, nct, z0);
  value_release(z0);
  /* Defer result temp must start FALSE: some failure exits do not assign
   * `res`, and cexpr reduction can otherwise fold the expanded chain to a
   * constant TRUE (gpsim: `if !string_heap_compact()` / assignment of the
   * call result always saw success). */
  jal_string_bool_assign(pf, res, BOOLEAN_FALSE);
  for (r = 0U; r < reg_ct; ++r) {
    value_t  h;
    variable_t  vrr;

    vrr = pfile_dynstr_reg_get(pf, r);
    h   = value_alloc(vrr);
    skipr = pfile_label_alloc(pf, 0U);
    if (VALUE_NONE == h) {
      pfile_log_syserr(pf, ENOMEM);
      goto str_cpt_err2;
    }
    capu = jal_string_handle_index_get(pf, h, JAL_STRING_FIELD_CAP);
    jal_string_branch_if_const_cmp(pf, OPERATOR_EQ, capu, 0U, CMD_BRANCHCOND_TRUE,
        skipr);
    ofu  = jal_string_handle_index_get(pf, h, JAL_STRING_FIELD_OFFSET);
    hdrb = jal_string_index_const_get(pf, jal_string_header_size_get(pf));
    bszc = jal_string_binary_emit(pf, OPERATOR_ADD, capu, hdrb);
    value_release(capu);
    value_release(hdrb);
    z0   = jal_string_const_get(pf, 0U);
    c_r  = jal_string_const_get(pf, (variable_const_t) r);
    jal_str_cpt_wk_set(pf, wk, nct, 0U, ofu);
    jal_str_cpt_wk_set(pf, wk, nct, 1U, bszc);
    jal_str_cpt_wk_set(pf, wk, nct, 2U, z0);
    jal_str_cpt_wk_set(pf, wk, nct, 3U, c_r);
    value_release(ofu);
    value_release(bszc);
    value_release(z0);
    value_release(c_r);
    z1   = jal_string_const_get(pf, 1U);
    pfile_cmd_op_add(pf, OPERATOR_ADD, &nct, nct, z1);
    value_release(z1);
    pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE, skipr,
        VALUE_NONE, VALUE_NONE, 0U);
    pfile_cmd_label_add(pf, skipr);
    value_release(h);
    label_release(skipr);
  }
  empty_cpt = pfile_label_alloc(pf, 0U);
  cpt_out  = pfile_label_alloc(pf, 0U);
  jal_string_branch_if_const_cmp(pf, OPERATOR_EQ, nct, 0U, CMD_BRANCHCOND_TRUE,
      empty_cpt);
  nmo  = jal_string_temp_index_get(pf);
  i   = jal_string_temp_index_get(pf);
  j   = jal_string_temp_index_get(pf);
  inlim = jal_string_temp_index_get(pf);
  k  = jal_string_temp_index_get(pf);
  endu_w = jal_string_heap_cpt_u16_temp_get(pf);
  mi  = jal_string_temp_index_get(pf);
  krow = jal_string_temp_index_get(pf);
  onev = jal_string_const_get(pf, 1U);
  z0  = jal_string_binary_emit(pf, OPERATOR_SUB, nct, onev);
  jal_string_assign_value(pf, nmo, z0);
  value_release(z0);
  value_release(onev);
  jal_str_cpt_bubble_sort_emit(pf, wk, nmo, i, j, inlim);
  jal_str_cpt_prefix_emit(pf, wk, nct, k, endu_w);
  jal_str_cpt_moves_emit(pf, wk, nct, nmo, mi, krow);
  jal_str_cpt_free_tail_emit(pf, endu_w, hszc, fmaxb, isz);
  value_release(endu_w);
  kk   = jal_string_temp_index_get(pf);
  z0  = jal_string_const_get(pf, 0U);
  jal_string_assign_value(pf, kk, z0);
  value_release(z0);
  jal_str_cpt_reg_patch_all_emit(pf, wk, nct, kk, reg_ct);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE, cpt_out,
      VALUE_NONE, VALUE_NONE, 0U);
  pfile_cmd_label_add(pf, empty_cpt);
  jal_string_runtime_heap_fresh_bootstrap_emit(pf, hszc, isz,
      (variable_sz_t)(3U * (unsigned)isz), fmaxb);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO, CMD_BRANCHCOND_NONE, cpt_out,
      VALUE_NONE, VALUE_NONE, 0U);
  pfile_cmd_label_add(pf, cpt_out);
  label_release(cpt_out);
  label_release(empty_cpt);
  jal_string_bool_assign(pf, res, BOOLEAN_TRUE);
  jal_string_silent_truncate_end(pf);
  return;
str_cpt_err2:
  jal_string_silent_truncate_end(pf);
  value_release(nct);
  value_release(wk);
  jal_string_bool_assign(pf, res, BOOLEAN_FALSE);
  return;
}

void jal_string_heap_compact_defer_expand(pfile_t *pf)
{
  pfile_str_heap_defer_t *d;
  pfile_str_heap_defer_t *n;
  pfile_str_heap_defer_t *l;
  cmd_t                    nh;
  cmd_t                    nt;

  l = pf->str_heap_defer;
  pf->str_heap_defer = 0;
  for (d = l; d; d = n) {
    n = d->link;
    pfile_cmd_bld_start(pf);
    jal_string_heap_compact_fill_bld(pf, d->result);
    pfile_cmd_bld_end(pf, &nh, &nt);
    pfile_cmd_replace1_with_chain(pf, d->marker, nh, nt);
    /* the expanded chain's cmds hold their own references; drop the
     * reference taken by pfile_str_heap_defer_add */
    if (d->result) {
      value_release(d->result);
    }
    FREE(d);
  }
}
