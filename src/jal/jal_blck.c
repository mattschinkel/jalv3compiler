/************************************************************
 **
 ** jal_blck.c : JAL block processing definitions
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: fork extensions (heavy rework)
 **
 ************************************************************/
#include <errno.h>
#include <string.h>
#include <assert.h>
#include "../libutils/mem.h"
#include "../libcore/pf_cmd.h"
#include "../libcore/pf_proc.h"
#include "../libcore/pf_expr.h"
#include "../libcore/pf_msg.h"
#include "../libcore/variable.h"
#include "../libpic12/pic.h"
#include "jal_file.h"
#include "jal_tokn.h"
#include "jal_ctrl.h"
#include "jal_proc.h"
#include "jal_expr.h"
#include "jal_prnt.h"
#include "jal_asm.h"
#include "jal_incl.h"
#include "jal_vdef.h"
#include "jal_string.h"
#include "jal_blck.h"
#if defined(MSDOS) && !defined(__WATCOMC__)
#include <dir.h>
#define FILENAME_MAX MAXPATH
#endif


/* this traces the masters back to find one with a _get or _put function */

static boolean_t jal_value_const_baseofs_get(value_t val,
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

static boolean_t jal_pointer_signature_def_is_same(variable_def_t a,
    variable_def_t b)
{
  flag_t                sig_flags;
  variable_def_member_t mbr_a;
  variable_def_member_t mbr_b;

  if (a == b) {
    return BOOLEAN_TRUE;
  }
  if (!a || !b) {
    return BOOLEAN_FALSE;
  }
  sig_flags = VARIABLE_DEF_FLAG_SIGNED | VARIABLE_DEF_FLAG_BIT
      | VARIABLE_DEF_FLAG_IN | VARIABLE_DEF_FLAG_OUT;
  if (variable_def_type_get(a) != variable_def_type_get(b)
      || variable_def_sz_get(a) != variable_def_sz_get(b)
      || ((variable_def_flags_get_all(a) & sig_flags)
        != (variable_def_flags_get_all(b) & sig_flags))) {
    return BOOLEAN_FALSE;
  }
  mbr_a = variable_def_member_get(a);
  mbr_b = variable_def_member_get(b);
  while (mbr_a && mbr_b) {
    if (variable_def_member_ct_get(mbr_a) != variable_def_member_ct_get(mbr_b)
        || !jal_pointer_signature_def_is_same(
          variable_def_member_def_get(mbr_a),
          variable_def_member_def_get(mbr_b))) {
      return BOOLEAN_FALSE;
    }
    mbr_a = variable_def_member_link_get(mbr_a);
    mbr_b = variable_def_member_link_get(mbr_b);
  }
  return !mbr_a && !mbr_b;
}

static variable_def_t jal_pointer_pseudo_data_def_get(variable_def_t fn_def)
{
  variable_def_member_t mbr;
  variable_def_t        data_def;

  if (!fn_def || VARIABLE_DEF_TYPE_FUNCTION != variable_def_type_get(fn_def)) {
    return VARIABLE_DEF_NONE;
  }
  mbr = variable_def_member_get(fn_def);
  data_def = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
  if (data_def && VARIABLE_DEF_TYPE_NONE != variable_def_type_get(data_def)) {
    return data_def;
  }
  mbr = mbr ? variable_def_member_link_get(mbr) : 0;
  return mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
}

static boolean_t jal_pointer_assign_type_is_compatible(value_t dst, value_t src)
{
  variable_def_t        dst_def;
  variable_def_t        src_def;
  variable_def_member_t dst_mbr;
  variable_def_t        pointee_def;

  if (!dst || !src || !value_is_pointer(dst)) {
    return BOOLEAN_FALSE;
  }
  if (value_is_const(src) && !value_variable_get(src) && !value_is_function(src)) {
    return BOOLEAN_FALSE;
  }
  dst_def = value_def_get(dst);
  src_def = value_def_get(src);
  if (!dst_def || !src_def) {
    return BOOLEAN_FALSE;
  }
  if (value_is_pointer(src)) {
    return jal_pointer_signature_def_is_same(dst_def, src_def);
  }
  dst_mbr = variable_def_member_get(dst_def);
  pointee_def = dst_mbr ? variable_def_member_def_get(dst_mbr)
      : VARIABLE_DEF_NONE;
  if (!pointee_def) {
    return BOOLEAN_FALSE;
  }
  /* Fixed or dynamic string pointee vs `var string` / `var string[N]` RHS:
   * `value_type_get` / `value_is_array` are not reliable differentiators here;
   * compare defs (see `variable_def_is_same` vs `jal_pointer_signature_def_is_same`
   * for STRING flag handling). */
  if (variable_def_flag_test(pointee_def, VARIABLE_DEF_FLAG_STRING) && src) {
    variable_t sv;

    sv = value_variable_get(src);
    if (sv && variable_def_flag_test(variable_def_get(sv), VARIABLE_DEF_FLAG_STRING)
        && variable_def_is_same(pointee_def, variable_def_get(sv))) {
      return BOOLEAN_TRUE;
    }
    if (src_def && variable_def_flag_test(src_def, VARIABLE_DEF_FLAG_STRING)
        && variable_def_is_same(pointee_def, src_def)) {
      return BOOLEAN_TRUE;
    }
  }
  if (value_is_function(src)) {
    if (VARIABLE_DEF_TYPE_FUNCTION != variable_def_type_get(pointee_def)) {
      variable_def_t pseudo_data_def;

      pseudo_data_def = jal_pointer_pseudo_data_def_get(src_def);
      if (VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(pointee_def)) {
        /* indexed pseudo (pseudo ARRAY) vs `as var T[N]` pointee: the
         * GET's RETURN def is the ELEMENT type */
        variable_def_member_t am;
        variable_def_t        em;

        am = variable_def_member_get(pointee_def);
        em = am ? variable_def_member_def_get(am) : VARIABLE_DEF_NONE;
        return em && pseudo_data_def
            && variable_def_is_same(em, pseudo_data_def);
      }
      return pseudo_data_def && variable_def_is_same(pointee_def, pseudo_data_def);
    }
    return jal_pointer_signature_def_is_same(pointee_def, src_def);
  }
  if (value_is_array(src) || jal_string_is_dynamic(src)) {
    variable_def_member_t src_mbr;
    variable_def_t        src_member_def;

    if (VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(pointee_def)) {
      /* `var string` / `var string[N]` rhs may not satisfy `value_is_array`
       * while still carrying the same `variable_def` as the pointer pointee;
       * `variable_def_is_same` matches STRING layout including dynamic. */
      if (variable_def_flag_test(pointee_def, VARIABLE_DEF_FLAG_STRING)) {
        return variable_def_is_same(pointee_def, src_def);
      }
      return jal_pointer_signature_def_is_same(pointee_def, src_def);
    }
    src_mbr = variable_def_member_get(src_def);
    src_member_def = src_mbr ? variable_def_member_def_get(src_mbr)
        : VARIABLE_DEF_NONE;
    return src_member_def
        && jal_pointer_signature_def_is_same(pointee_def, src_member_def);
  }
  return jal_pointer_signature_def_is_same(pointee_def, src_def);
}

static variable_t jal_pointer_value_variable_get(value_t val)
{
  variable_t var;

  var = value_variable_get(val);
  return (var && variable_is_pointer(var)) ? var : VARIABLE_NONE;
}

static void jal_pointer_rvalue_link_name_clear(value_t ptr_val)
{
  variable_t ptr_var;

  ptr_var = jal_pointer_value_variable_get(ptr_val);
  if (ptr_var) {
    variable_pointer_rvalue_link_name_set(ptr_var, 0);
  }
}

static char *jal_pointer_rvalue_link_name_dup(pfile_t *pf, value_t ptr_val,
    const char *identifier)
{
  char        *copy;
  variable_t   hv;

  /* scalar pointer hosts, and pointer-array elements (the per-offset
   * element links -- the element's variable is the host ARRAY) */
  hv = jal_pointer_value_variable_get(ptr_val);
  if (!hv) {
    variable_def_t        hdef;
    variable_def_member_t hmbr;
    variable_def_t        hmdef;

    hv = value_variable_get(ptr_val);
    hdef = (hv && variable_is_array(hv)) ? variable_def_get(hv) : 0;
    hmbr = hdef ? variable_def_member_get(hdef) : 0;
    hmdef = hmbr ? variable_def_member_def_get(hmbr) : VARIABLE_DEF_NONE;
    if (!hmdef
        || (VARIABLE_DEF_TYPE_POINTER != variable_def_type_get(hmdef))) {
      hv = VARIABLE_NONE;
    }
  }
  if (!hv || !identifier || !identifier[0]) {
    return 0;
  }
  copy = STRDUP(identifier);
  if (!copy) {
    pfile_log_syserr(pf, ENOMEM);
  }
  return copy;
}

/* `p = x` where `p` was bound to a pseudo (`p is pv`): the write-side
 * mirror of the rvalue GET link -- find the pseudo's PUT.  Returns
 * VALUE_NONE with *is_pseudo FALSE when val has no pseudo link (plain
 * variable/array binds keep the through-slot store); VALUE_NONE with
 * *is_pseudo TRUE when the link names a GET-carrying pseudo without a
 * PUT (so `p = x` can error exactly like `pv = x` would). */
static value_t jal_pointer_put_link_get(pfile_t *pf, value_t ptr_val,
    boolean_t *is_pseudo)
{
  variable_t pv;
  const char *nm;
  value_t    jv[JAL_VAL_TYPE_CT];
  value_t    putv;

  *is_pseudo = BOOLEAN_FALSE;
  pv = jal_pointer_value_variable_get(ptr_val);
  nm = (pv) ? variable_pointer_rvalue_link_name_get(pv) : 0;
  if (!nm || !nm[0]) {
    return VALUE_NONE;
  }
  if (!jal_value_find_by_name(pf, nm, jv)) {
    return VALUE_NONE;
  }
  putv = jv[JAL_VAL_TYPE_PUT];
  jv[JAL_VAL_TYPE_PUT] = VALUE_NONE;
  *is_pseudo = (putv || (jv[JAL_VAL_TYPE_GET] && !jv[JAL_VAL_TYPE_BASE]))
    ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  jal_value_release(jv);
  return putv;
}

/* ---- stage-2 runtime dispatch for pseudo-marked pointer arrays ----
 *
 * A marked array dispatches runtime-indexed element access through a
 * kind[] byte mirror (1 = pseudo element, written at each const bind,
 * zeroed at synthesis) and a fn-ptr array of the elements' 'put
 * accessors (the slot itself stores 'get -- the two-address problem's
 * second half). All emission is generic pcode, so every back end
 * inherits it.
 *
 * DISABLED: the kind test, plain paths, helper synthesis and bind
 * stores are emission-verified, but loading a slot's pointer VALUE at
 * a runtime index and calling through it from hand-built pcode has a
 * temp-aliasing/def-view hazard on classic (the loaded call target
 * mis-stages) and traps on PIC32. The proven route is the
 * `fps[i](x)` path through jal_parse_call -- wiring that is the
 * remaining stage-2 work. Until then the runtime-index refusal
 * stands. */
static const boolean_t jal_pointer_slot_rt_on = BOOLEAN_TRUE;

boolean_t jal_pointer_slot_rt_enabled(void)
{
  return jal_pointer_slot_rt_on;
}

/* the canonical pointer-to-function(byte in) return byte def for
 * indirect 'put calls and slot-value 'get calls (cached per pfile) */
variable_def_t jal_pointer_fnptr_def_get(pfile_t *pf)
{
  variable_def_t       bdef;
  variable_def_t       rdef;
  variable_def_t       fdef;
  variable_def_t       pdef;
  static variable_def_t cached;

  if (cached) {
    return cached;
  }
  bdef = pfile_variable_def_find(pf, PFILE_LOG_NONE, "byte");
  if (!bdef) {
    return VARIABLE_DEF_NONE;
  }
  rdef = variable_def_flags_change(variable_def_dup(bdef),
      variable_def_flags_get_all(bdef) | VARIABLE_DEF_FLAG_OUT);
  fdef = variable_def_alloc(0, VARIABLE_DEF_TYPE_FUNCTION, 0, 0);
  pdef = variable_def_alloc(0, VARIABLE_DEF_TYPE_POINTER, 0, 0);
  if (!rdef || !fdef || !pdef) {
    return VARIABLE_DEF_NONE;
  }
  (void) variable_def_member_insert(fdef, 0, 0, rdef, 1);
  (void) variable_def_member_add(pdef, 0, fdef, 1);
  cached = pdef;
  return pdef;
}

/* emit `kind[ofs] = val` (const element store into the helper array) */
static void jal_pointer_slot_kind_store(pfile_t *pf, variable_t kind,
    variable_const_t ofs, variable_const_t val)
{
  value_t               dst;
  value_t               src;
  variable_def_t        edef;
  variable_def_member_t embr;

  dst = value_alloc(kind);
  if (!dst) {
    return;
  }
  src = pfile_constant_get(pf, ofs, VARIABLE_DEF_NONE);
  jal_val_baseofs_add(pf, dst, src);
  value_release(src);
  /* narrow to the ELEMENT def (an array-def dst makes the op layer
   * reject scalar assigns) */
  embr = variable_def_member_get(variable_def_get(kind));
  edef = embr ? variable_def_member_def_get(embr) : VARIABLE_DEF_NONE;
  if (edef) {
    value_def_set(dst, edef);
  }
  src = pfile_constant_get(pf, val, VARIABLE_DEF_NONE);
  pfile_cmd_op_add(pf, OPERATOR_ASSIGN, &dst, src, VALUE_NONE);
  value_release(src);
}

/* synthesize kind[]/puts[] for the marked array on the first pseudo
 * bind; kind[] is zeroed here (deterministic dispatch for elements
 * that were never bound: plain path, existing unbound behavior) */
static variable_t jal_pointer_slot_helpers_ensure(pfile_t *pf,
    variable_t arr, variable_const_t ct, variable_sz_t esz)
{
  variable_t       kind;
  variable_t       puts;
  variable_def_t   kdef;
  variable_def_t   pdef;
  pfile_block_t   *blk;
  char            *nm;
  variable_const_t ii;

  kind = variable_pointer_slot_kind_var_get(arr);
  if (kind) {
    return kind;
  }
  pdef = jal_pointer_fnptr_def_get(pf);
  if (!pdef) {
    return VARIABLE_NONE;
  }
  kdef = variable_def_alloc(0, VARIABLE_DEF_TYPE_ARRAY, 0, 0);
  pdef = variable_def_dup(pdef);
  if (!kdef || !pdef) {
    return VARIABLE_NONE;
  }
  /* kind[] is indexed by BYTE offset (the write path only has the
   * scaled element baseofs) */
  (void) variable_def_member_add(kdef, 0,
      pfile_variable_def_find(pf, PFILE_LOG_NONE, "byte"), ct * esz);
  blk = pfile_proc_block_active_get(pfile_proc_active_get(pf));
  if (!blk) {
    return VARIABLE_NONE;
  }
  nm = MALLOC(strlen(variable_name_get(arr)) + 8);
  if (nm) {
    strcpy(nm, "_");
    strcat(nm, variable_name_get(arr));
    strcat(nm, "_kind");
  }
  if ((RESULT_OK != pfile_block_variable_alloc(blk,
      pfile_tag_alloc(pf, nm ? nm : "_slotkind"), kdef, VARIABLE_NONE,
      pf, &kind)) || !kind) {
    FREE(nm);
    return VARIABLE_NONE;
  }
  FREE(nm);
  nm = MALLOC(strlen(variable_name_get(arr)) + 8);
  if (nm) {
    strcpy(nm, "_");
    strcat(nm, variable_name_get(arr));
    strcat(nm, "_puts");
  }
  {
    variable_def_t addef;

    addef = variable_def_alloc(0, VARIABLE_DEF_TYPE_ARRAY, 0, 0);
    if (!addef
        || (RESULT_OK != variable_def_member_add(addef, 0, pdef, ct))
        || (RESULT_OK != pfile_block_variable_alloc(blk,
            pfile_tag_alloc(pf, nm ? nm : "_slotputs"), addef,
            VARIABLE_NONE, pf, &puts)) || !puts) {
      FREE(nm);
      return VARIABLE_NONE;
    }
  }
  FREE(nm);
  variable_pointer_slot_kind_var_set(arr, kind);
  variable_pointer_slot_puts_var_set(arr, puts);
  variable_release(kind);
  variable_release(puts);
  /* the fn-ptr CALL variables: loaded by slot-copy assigns, then
   * called -- every op a proven shape with stable variable storage
   * (hand-rolled temp staging alias-collides) */
  {
    variable_t gcv;
    variable_t pcv;

    gcv = VARIABLE_NONE;
    pcv = VARIABLE_NONE;
    if ((RESULT_OK == pfile_block_variable_alloc(blk,
        pfile_tag_alloc(pf, "_slot_getcall"),
        variable_def_dup(jal_pointer_fnptr_def_get(pf)),
        VARIABLE_NONE, pf, &gcv)) && gcv
        && (RESULT_OK == pfile_block_variable_alloc(blk,
            pfile_tag_alloc(pf, "_slot_putcall"),
            variable_def_dup(jal_pointer_fnptr_def_get(pf)),
            VARIABLE_NONE, pf, &pcv)) && pcv) {
      variable_pointer_slot_getcall_var_set(arr, gcv);
      variable_pointer_slot_putcall_var_set(arr, pcv);
      variable_pointer_bound_ct_bump(gcv);
      variable_pointer_bound_ct_bump(pcv);
    }
    variable_release(gcv);
    variable_release(pcv);
  }
  for (ii = 0; ii < ct * esz; ii++) {
    jal_pointer_slot_kind_store(pf, kind, ii, 0);
  }
  /* the helper fn-ptr array is written by synthesized stores, not by
   * parsed binds -- keep the must-bind check quiet about it, and mark
   * it used so DCE does not eliminate it (the write dispatch's
   * hand-built CALL is its only reference) */
  variable_pointer_bound_ct_bump(puts);
  variable_use_ct_bump(puts, CTR_BUMP_INCR);
  return variable_pointer_slot_kind_var_get(arr);
}

static value_t jal_parse_pointer_assign_function_rhs_try(pfile_t *pf,
    value_t dst, const char *identifier)
{
  value_t expr;
  value_t jvals[JAL_VAL_TYPE_CT];

  expr = VALUE_NONE;
  if (jal_value_find(pf, identifier, jvals) && jvals[JAL_VAL_TYPE_BASE]
      && value_is_function(jvals[JAL_VAL_TYPE_BASE])) {
    jal_pointer_rvalue_link_name_clear(dst);
    expr = jvals[JAL_VAL_TYPE_BASE];
    jvals[JAL_VAL_TYPE_BASE] = VALUE_NONE;
    pf_token_get(pf, PF_TOKEN_NEXT);
  }
  jal_value_release(jvals);
  return expr;
}

static value_t jal_parse_pointer_assign_rhs(pfile_t *pf, value_t dst,
    char **ptr_rhs_dup)
{
  const char *identifier;
  value_t     expr;

  identifier = pf_token_get(pf, PF_TOKEN_CURRENT);
  if (jal_token_is_identifier(pf)) {
    expr = jal_parse_pointer_assign_function_rhs_try(pf, dst, identifier);
    if (expr) {
      return expr;
    }
    *ptr_rhs_dup = jal_pointer_rvalue_link_name_dup(pf, dst, identifier);
    /* LVALUE: `slot[i] is v` must keep variable_t on RHS for address-of
     * in the code generator; RVALUE via jal_parse_expr loses the link. */
    return jal_parse_value(pf, JAL_PARSE_VALUE_TYPE_LVALUE_FOR_POINTER_IS);
  }

  jal_pointer_rvalue_link_name_clear(dst);
  return jal_parse_expr(pf);
}

/* jal_parse_assign decomposition: the phases below hand this state
 * through; every value_t handle moves with its reference exactly as the
 * original single function did (helpers alias in / write back out). */
typedef struct jal_assign_ctx_ {
  pfile_t   *pf;
  value_t    jval[JAL_VAL_TYPE_CT]; /* resolved LHS candidates */
  boolean_t  success;
  boolean_t  is_put;
  value_t    val;               /* the LHS */
  value_t    idx;               /* 'put(idx, v) index actual */
  size_t     param_ct;
  boolean_t  lvalue_subscripted;
  operator_t assign_op;
  value_t    expr;              /* the RHS */
  char      *ptr_rhs_dup;       /* 'is' RHS name duplicate */
} jal_assign_ctx_t;


/* pick the LHS from the resolved name candidates ('put accessor, base
 * value, or 'iput); a pointer-to-indexed-pseudo LHS swaps to its
 * linked 'put here (the write-side mirror of the read swap) */
static void jal_assign_lhs_resolve(jal_assign_ctx_t *ctx)
{
  pfile_t   *pf      = ctx->pf;
  value_t   *jval    = ctx->jval;
  boolean_t  success = ctx->success;
  boolean_t  is_put  = BOOLEAN_FALSE;
  value_t    val     = VALUE_NONE;

  if (jval[JAL_VAL_TYPE_PUT]) {
    val = jval[JAL_VAL_TYPE_PUT];
    if (VARIABLE_DEF_TYPE_POINTER == value_type_get(val)) {
      value_assign_ct_bump(val, CTR_BUMP_INCR);
      value_dereference(val);
    }
    assert(VARIABLE_DEF_TYPE_FUNCTION == value_type_get(val));
    is_put = BOOLEAN_TRUE;
  } else if (jval[JAL_VAL_TYPE_BASE]) {
    val = jval[JAL_VAL_TYPE_BASE];
    if (value_vflag_test(val, VARIABLE_FLAG_READ)
      && !value_vflag_test(val, VARIABLE_FLAG_WRITE)) {
      pfile_log(pf, PFILE_LOG_ERR, "cannot write to %s",
        value_name_get(val));
    }
    /* `p[i] = v` where p is bound to an INDEXED pseudo (`p is parr`):
     * route to the linked 'put exactly like the direct `parr[i] = v`
     * LHS -- the is_put branch below then parses the subscript as the
     * first formal (const or runtime index). Scalar pseudo links
     * keep the store-routing block further down. */
    if (val && (VARIABLE_DEF_TYPE_POINTER == value_type_get(val))) {
      value_t igetv;

      igetv = jal_pointer_link_indexed_get_get(pf, value_variable_get(val));
      if (igetv) {
        value_t putv;
        boolean_t is_pseudo;

        value_release(igetv);
        putv = jal_pointer_put_link_get(pf, val, &is_pseudo);
        /* take val's reference from jval before replacing it */
        if (jval[JAL_VAL_TYPE_BASE] == val) {
          jval[JAL_VAL_TYPE_BASE] = VALUE_NONE;
        }
        value_release(val);
        if (putv) {
          if (VARIABLE_DEF_TYPE_POINTER == value_type_get(putv)) {
            value_assign_ct_bump(putv, CTR_BUMP_INCR);
            value_dereference(putv);
          }
          val    = putv;
          is_put = BOOLEAN_TRUE;
        } else if (is_pseudo) {
          pfile_log(pf, PFILE_LOG_ERR, "No PUT procedure");
          val = VALUE_NONE;
        } else {
          val = VALUE_NONE;
        }
      }
    }
    /* don't skip this token; this value will be reprocessed later */
  } else if (jval[JAL_VAL_TYPE_IPUT]) { 
    val = jval[JAL_VAL_TYPE_IPUT];
    assert(VARIABLE_DEF_TYPE_FUNCTION == value_type_get(val));
    is_put = BOOLEAN_TRUE;
  } else {
    success = BOOLEAN_FALSE;
    pfile_log(pf, PFILE_LOG_ERR, "No PUT procedure");
  }

  ctx->success = success;
  ctx->is_put  = is_put;
  ctx->val     = val;
}

/* non-put LHS: re-resolve the value in LVALUE context (the slot-element
 * pseudo dispatch must not consume an assignment LHS), then run the
 * postfix chain (records whether a subscript was seen) */
static void jal_assign_lvalue_parse(jal_assign_ctx_t *ctx)
{
  pfile_t   *pf  = ctx->pf;
  value_t    val = ctx->val;
  boolean_t  lvalue_subscripted = ctx->lvalue_subscripted;
  flag_t     postfix_flags;  /* decl from the original block */

  value_release(val);
  /* LVALUE context: the slot-element pseudo dispatch must not
   * consume an assignment LHS (`slots[k] = v` needs the element
   * view for the put routing) */
  pfile_parse_lvalue_ctx_push(pf);
  val = jal_parse_value(pf, JAL_PARSE_VALUE_TYPE_LVALUE);
  pfile_parse_lvalue_ctx_pop(pf);
  if (!val) {
    if (jal_token_is_identifier(pf)) {
      pfile_log(pf, PFILE_LOG_ERR, "\"%s\" not found", 
          pf_token_get(pf, PF_TOKEN_CURRENT));
      pf_token_get(pf, PF_TOKEN_NEXT);
    } else {
      pfile_log(pf, PFILE_LOG_ERR, "variable expected");
    }
  } else {
    postfix_flags = jal_parse_postfix(pf, &val, NULL);
    lvalue_subscripted = (postfix_flags
        & JAL_PARSE_POSTFIX_FLAG_SUBSCRIPT) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  }

  ctx->lvalue_subscripted = lvalue_subscripted;
  ctx->val                = val;
}

/* 'put LHS: walk the accessor's parameter shape -- scalar 'put(v)
 * takes param_ct 2, indexed 'put(idx, v) takes 3 and parses the
 * [index] actual here */
static void jal_assign_put_shape_parse(jal_assign_ctx_t *ctx)
{
  pfile_t    *pf = ctx->pf;
  value_t     val = ctx->val;
  value_t     idx = ctx->idx;
  size_t      param_ct = ctx->param_ct;

  variable_def_t        def;
  variable_def_member_t mbr;

  pf_token_get(pf, PF_TOKEN_NEXT);
  /* look to see if this is an array */
  def = value_def_get(val);
  mbr = variable_def_member_get(def); /* return value (none) */
  mbr = variable_def_member_link_get(mbr); /* index or parameter  */
  mbr = variable_def_member_link_get(mbr); /* parameter or none   */
  if (!mbr) {
    param_ct = 2;
  } else {
    param_ct = 3;
    /* parse the index */
    if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, "[")) {
      pf_token_get(pf, PF_TOKEN_NEXT);
      idx = jal_parse_expr(pf);
      if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, "]")) {
        pf_token_get(pf, PF_TOKEN_NEXT);
      }
    }
  }

  ctx->idx      = idx;
  ctx->param_ct = param_ct;
}

/* parse the RHS for the assignment operator at hand: dynamic/fixed
 * string stores, 'is pointer-bind RHS (also duplicating the RHS name
 * for the link bookkeeping), or a plain expression -- with the
 * pseudo-bound pointer RHS refusal (`q = p` where p is pv's accessor) */
static void jal_assign_rhs_parse(jal_assign_ctx_t *ctx)
{
  pfile_t   *pf = ctx->pf;
  value_t    val = ctx->val;
  value_t    expr = VALUE_NONE;
  operator_t assign_op = ctx->assign_op;
  char      *ptr_rhs_dup = ctx->ptr_rhs_dup;

  if ((assign_op == OPERATOR_ASSIGN)
      && (jal_string_is_dynamic(val)
          || jal_string_pointer_dynamic_dst_p(val)
          || jal_string_is_fixed(val))) {
    (void) jal_string_assignment_parse_emit(pf, val);
    value_release(val);
    val = VALUE_NONE;
    expr = VALUE_NONE;
  } else if (assign_op == OPERATOR_POINTER_ASSIGN) {
    expr = jal_parse_pointer_assign_rhs(pf, val, &ptr_rhs_dup);
  } else {
    const char  *rhs_nm;
    value_t      rjv[JAL_VAL_TYPE_CT];
    variable_t   rhs_var;
    const char  *rhs_lnk;
    boolean_t    rhs_pseudo;

    /* `q = p` where p is bound to a pseudo-variable: the rvalue
     * GET swap turns the RHS into an accessor call whose result
     * the store-through path drops (the slot reload clobbers W),
     * and the slot-copy semantics for a pointer RHS can never
     * engage (the swap consumed the pointer). Refuse; plain-bound
     * pointer copies keep the back-end slot-copy rule (no GET on
     * the link name). */
    rhs_pseudo = BOOLEAN_FALSE;
    if (val && value_is_pointer(val) && jal_token_is_identifier(pf)) {
      rhs_nm = pf_token_get(pf, PF_TOKEN_CURRENT);
      if (jal_value_find_by_name(pf, rhs_nm, rjv)
          && rjv[JAL_VAL_TYPE_BASE]) {
        rhs_var = value_variable_get(rjv[JAL_VAL_TYPE_BASE]);
        rhs_lnk = (rhs_var && variable_is_pointer(rhs_var))
            ? variable_pointer_rvalue_link_name_get(rhs_var) : 0;
        if (rhs_lnk && rhs_lnk[0]) {
          value_t ljv[JAL_VAL_TYPE_CT];

          if (jal_value_find_by_name(pf, rhs_lnk, ljv)
              && ljv[JAL_VAL_TYPE_GET] && !ljv[JAL_VAL_TYPE_BASE]) {
            rhs_pseudo = BOOLEAN_TRUE;
          }
          jal_value_release(ljv);
        }
      }
      jal_value_release(rjv);
    }
    if (rhs_pseudo) {
      pfile_log(pf, PFILE_LOG_ERR,
          "\"%s\" is bound to a pseudo-variable:"
          " read it into a variable first",
          pf_token_get(pf, PF_TOKEN_CURRENT));
    }
    expr = jal_parse_expr(pf);
    if (rhs_pseudo) {
      value_release(expr);
      expr = VALUE_NONE;
      value_release(val);
      val = VALUE_NONE;
    }
  }

  ctx->expr       = expr;
  ctx->val        = val;
  ctx->ptr_rhs_dup = ptr_rhs_dup;
}

/* the assignment validation/consumption chain: heap-string subscript
 * stores and string assignment emission (consume both sides), self
 * binds, pseudo-bind refusals (scalar pointee, slot elements),
 * pointer-bind type compatibility; an arm that refuses releases the
 * LHS so the emission below is skipped */
static void jal_assign_rhs_refusals(jal_assign_ctx_t *ctx)
{
  pfile_t   *pf = ctx->pf;
  value_t    val = ctx->val;
  value_t    expr = ctx->expr;
  operator_t assign_op = ctx->assign_op;

  if ((assign_op == OPERATOR_ASSIGN)
      && jal_string_subscript_write_emit(pf, val, expr)) {
    value_release(expr);
    expr = VALUE_NONE;
    value_release(val);
    val = VALUE_NONE;
  } else
  /* Check for pointer dereference assignment (pointer = value) */
  if ((assign_op == OPERATOR_ASSIGN)
      && jal_string_assignment_emit(pf, val, expr)) {
    value_release(expr);
    expr = VALUE_NONE;
    value_release(val);
    val = VALUE_NONE;
  } else if ((assign_op == OPERATOR_POINTER_ASSIGN)
      && expr && val
      && value_variable_get(expr)
      && (value_variable_get(expr) == value_variable_get(val))) {
    /* `p is p`: a self-bind emits nothing (the slot-to-slot copy
     * optimizes away), yet it silences the must-bind check by
     * counting as an assignment -- the pointer stays zero and
     * every later access traps or reads garbage. */
    pfile_log(pf, PFILE_LOG_ERR,
        "pointer '%s' cannot be bound to itself",
        variable_name_get(value_variable_get(val)));
    value_release(val);
    val = VALUE_NONE;
  } else if ((assign_op == OPERATOR_POINTER_ASSIGN) && expr
      && jal_pointer_var_is_pseudo_linked(pf,
          value_variable_get(expr))) {
    /* `q is p` where p is bound to a pseudo: the slot copy
     * carries the accessor's code address while q's link would
     * name p (a pointer, no GET) -- reads through q fall to a
     * slot deref of that code address. Bind the pseudo
     * directly instead. */
    pfile_log(pf, PFILE_LOG_ERR,
        "pointer '%s' is bound to a pseudo-variable --"
        " bind the pseudo directly (q is pv)",
        variable_name_get(value_variable_get(expr)));
    value_release(val);
    val = VALUE_NONE;
  } else if ((assign_op == OPERATOR_POINTER_ASSIGN)
      && !jal_pointer_assign_type_is_compatible(val, expr)) {
    pfile_log(pf, PFILE_LOG_ERR, "type mismatch");
    value_release(val);
    val = VALUE_NONE;
  } else if ((assign_op == OPERATOR_POINTER_ASSIGN)
      && value_is_function(expr)
      && !jal_pointer_value_variable_get(val)) {
    /* pseudo bind into a pointer-array element (or a record
     * pointer member). CONST-index element binds are the
     * supported form: the slot arm below records the per-
     * element link for static dispatch. Everything else -- a
     * runtime-indexed element bind, or a record member -- has
     * no dispatch: the slot would hold the GET's code address
     * while accesses dereference it as data. Function-pointer
     * slots (pointee IS a function type) are unaffected. */
    variable_def_t        pdef;
    variable_def_member_t pmbr;
    variable_def_t        pointee_def;
    variable_t            hvar;
    boolean_t             elem_ok;

    elem_ok = BOOLEAN_FALSE;
    hvar = value_variable_get(val);
    if (hvar && variable_is_array(hvar)) {
      variable_def_member_t hmbr;
      variable_def_t        hmdef;
      variable_const_t      hofs;

      hmbr = variable_def_member_get(variable_def_get(hvar));
      hmdef = hmbr ? variable_def_member_def_get(hmbr)
          : VARIABLE_DEF_NONE;
      if (hmdef
          && (VARIABLE_DEF_TYPE_POINTER
              == variable_def_type_get(hmdef))
          && jal_value_const_baseofs_get(val, &hofs)) {
        elem_ok = BOOLEAN_TRUE;
      }
    }
    pdef = value_def_get(val);
    pmbr = variable_def_member_get(pdef);
    pointee_def = pmbr ? variable_def_member_def_get(pmbr)
        : VARIABLE_DEF_NONE;
    if (!elem_ok
        && pointee_def
        && (VARIABLE_DEF_TYPE_FUNCTION
            != variable_def_type_get(pointee_def))) {
      pfile_log(pf, PFILE_LOG_ERR,
          "a pseudo-variable can only be bound to a pointer-"
          "array element with a constant index (static"
          " dispatch; no accessor dispatch otherwise)");
      value_release(val);
      val = VALUE_NONE;
    }
  } else if ((assign_op == OPERATOR_POINTER_ASSIGN)
      && value_is_function(expr)) {
    /* `p is parr` on a SCALAR pointee where parr is an INDEXED
     * pseudo-variable (its 'get takes the array index as a
     * parameter, 'put takes index+value): the scalar-pointer
     * link machinery assumes a zero-arg 'get and a one-arg
     * 'put -- reads would error "Too few parameters" and the
     * write routing passes the value into the accessor's INDEX
     * formal (silent wrong-element store with an uninitialized
     * value formal). Pointer-TO-ARRAY pointees are the
     * supported bind (checked above via the element-def compat
     * branch) and never reach here. */
    variable_def_t        gfdef;
    variable_def_member_t gfmbr;
    variable_def_t        ppdef;
    variable_def_member_t ppmbr;
    variable_def_t        ppointee_def;

    gfdef = value_def_get(expr);
    gfmbr = variable_def_member_get(gfdef);
    gfmbr = gfmbr ? variable_def_member_link_get(gfmbr) : 0;
    if (gfmbr) {
      ppdef = value_def_get(val);
      ppmbr = variable_def_member_get(ppdef);
      ppointee_def = ppmbr ? variable_def_member_def_get(ppmbr)
          : VARIABLE_DEF_NONE;
      if (ppointee_def
          && (VARIABLE_DEF_TYPE_ARRAY
              != variable_def_type_get(ppointee_def))
          && (VARIABLE_DEF_TYPE_FUNCTION
              != variable_def_type_get(ppointee_def))) {
        pfile_log(pf, PFILE_LOG_ERR,
            "an indexed pseudo-variable (an array) cannot be"
            " bound to a scalar pointer -- use a pointer-to-array"
            " (as var byte[N]) or its subscript form directly");
        value_release(val);
        val = VALUE_NONE;
      }
    }
  } else if (!value_def_is_same(val, expr)
    && !(assign_op != OPERATOR_POINTER_ASSIGN
        && pfile_expr_string_fix(pf, expr))) {
    /* Check if this is a pointer dereference assignment */
    if (value_is_pointer(val) && assign_op == OPERATOR_ASSIGN) {
      /* This is pointer dereference assignment like temp_sensor = 30 */
      /* Don't dereference here - let the code generator handle it */
      /* Just continue with the pointer as the destination */
    } else if (assign_op == OPERATOR_POINTER_ASSIGN) {
      /* `p is buf`: pointer slot and pointee defs differ, but the
       * pointee shape still has to match the addressable RHS. */
    } else {
      if (assign_op == OPERATOR_POINTER_ASSIGN) {
        pfile_log(pf, PFILE_LOG_ERR, "type mismatch");
      }
      value_release(val);
      val = VALUE_NONE;
    }
  }

  ctx->val  = val;
  ctx->expr = expr;
}

/* `slot[k] = v` where slot k is bound to a pseudo: const-index
 * elements route statically to the linked 'put (rewriting this call
 * into an is_put); runtime-indexed access takes the stage-2 kind[]
 * dispatch when enabled, else refuses */
static void jal_assign_slot_dispatch(jal_assign_ctx_t *ctx)
{
  pfile_t   *pf = ctx->pf;
  value_t    val = ctx->val;
  value_t    expr = ctx->expr;
  value_t    idx = ctx->idx;
  size_t     param_ct = ctx->param_ct;
  boolean_t  is_put = ctx->is_put;
  operator_t assign_op = ctx->assign_op;

  /* `slot[k] = v` where slot k is bound to a pseudo
   * (pointer-array element link): route to the linked 'put
   * with the element index as the first formal -- static
   * const-index dispatch. Runtime-indexed access to a
   * pseudo-marked pointer array refuses (no runtime
   * dispatch exists yet). */
  if ((assign_op == OPERATOR_ASSIGN) && val
      && (value_is_pointer(val)
          || value_flag_test(val, VALUE_FLAG_POINTER))) {
    variable_t sv;

    sv = value_variable_get(val);
    if (sv && variable_is_array(sv)
        && variable_pointer_slot_links_any(sv)) {
      variable_const_t      sofs;
      variable_def_t        smdef;
      variable_def_member_t smmbr;
      variable_sz_t         ssz;

      smmbr = variable_def_member_get(variable_def_get(sv));
      smdef = smmbr ? variable_def_member_def_get(smmbr)
          : VARIABLE_DEF_NONE;
      ssz   = smdef ? pfile_flat_def_storage_sz(pf, smdef) : 0;
      if (ssz && jal_value_const_baseofs_get(val, &sofs)) {
        const char *slink;

        slink = variable_pointer_slot_link_name_get(sv, sofs);
        if (slink) {
          value_t sjv[JAL_VAL_TYPE_CT];

          if (jal_value_find_by_name(pf, slink, sjv)
              && sjv[JAL_VAL_TYPE_PUT]
              && (VARIABLE_DEF_TYPE_FUNCTION
                  == value_type_get(sjv[JAL_VAL_TYPE_PUT]))) {
            value_t               sput;
            variable_def_member_t sm1;
            variable_def_member_t sm2;

            sput = sjv[JAL_VAL_TYPE_PUT];
            sjv[JAL_VAL_TYPE_PUT] = VALUE_NONE;
            jal_value_release(sjv);
            value_release(val);
            val    = sput;
            is_put = BOOLEAN_TRUE;
            /* scalar 'put(v) takes just the value; an indexed
             * 'put(idx, v) takes the element index first --
             * the same member walk the is_put branch uses */
            sm1 = variable_def_member_get(value_def_get(sput));
            sm1 = sm1 ? variable_def_member_link_get(sm1) : 0;
            sm2 = sm1 ? variable_def_member_link_get(sm1) : 0;
            if (sm2) {
              idx      = pfile_constant_get(pf, sofs / ssz,
                  VARIABLE_DEF_NONE);
              param_ct = 3;
            } else {
              param_ct = 2;
            }
          } else {
            jal_value_release(sjv);
          }
        }
      } else {
        /* stage-2 runtime dispatch: kind[k] test selects the
         * pseudo path (indirect 'put call through the
         * parallel puts[] table) or the plain store-through */
        variable_t hkw;

        hkw = variable_pointer_slot_kind_var_get(sv);
        if (jal_pointer_slot_rt_enabled() && hkw && expr) {
          value_t        tkind;
          value_t        tkind_src;
          label_t        l_pseudo;
          label_t        l_done;

          l_pseudo = pfile_label_alloc(pf, 0);
          l_done   = pfile_label_alloc(pf, 0);
          tkind    = VALUE_NONE;
          tkind_src = value_alloc(hkw);
          if (tkind_src && l_pseudo && l_done) {
            value_t               nscaled;
            variable_def_member_t kembr;

            /* tkind = kind[k] -- the index is the element's
             * baseofs value (runtime for runtime k) */
            nscaled = value_clone(value_baseofs_get(val));
            jal_val_baseofs_add(pf, tkind_src, nscaled);
            value_release(nscaled);
            kembr = variable_def_member_get(
                variable_def_get(hkw));
            if (kembr) {
              value_def_set(tkind_src,
                  variable_def_member_def_get(kembr));
            }
            /* branch directly on the kind element value */
            pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO,
                CMD_BRANCHCOND_TRUE, l_pseudo, tkind_src, 0, 0);
            /* plain: *(slot k) = expr (the ordinary
             * store-through op) */
            value_lock(expr);
            pfile_cmd_op_add(pf, OPERATOR_ASSIGN, &val,
                value_clone(expr), VALUE_NONE);
            pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO,
                CMD_BRANCHCOND_NONE, l_done, VALUE_NONE, 0, 0);
            /* pseudo: DIRECT-CALL CHAIN over the array's
             * const-index pseudo links (each element's put
             * target is known at compile time). An indirect
             * call through a synthesized puts table gets
             * dead-eliminated by the back end; direct calls
             * to known targets always survive. */
            pfile_cmd_label_add(pf, l_pseudo);
            /* build a compare chain over each linked offset */
            {
              variable_const_t esz2;
              size_t           ct2;
              size_t           ei;

              esz2 = ssz;
              ct2 = (size_t)
                  variable_def_member_ct_get(
                    variable_def_member_get(
                      variable_def_get(sv)));
              for (ei = 0; ei < ct2; ei++) {
                const char *lk_name;
                value_t     jv[5];
                label_t     lk_call;
                label_t     lk_skip;

                lk_name = variable_pointer_slot_link_name_get(
                    sv, (variable_const_t)(ei * esz2));
                if (!lk_name) {
                  continue;
                }
                lk_call = pfile_label_alloc(pf, 0);
                lk_skip = pfile_label_alloc(pf, 0);
                if (jal_value_find_by_name(pf, lk_name, jv)
                    && jv[JAL_VAL_TYPE_PUT]) {
                  value_t cmpv;
                  value_t kofs;
                  value_t cofs;

                  /* if baseofs == ei*esz2 goto call */
                  kofs = value_clone(
                      value_baseofs_get(val));
                  cofs = pfile_constant_get(pf,
                      ei * esz2, VARIABLE_DEF_NONE);
                  cmpv = VALUE_NONE;
                  pfile_cmd_op_add(pf, OPERATOR_EQ,
                      &cmpv, kofs, cofs);
                  pfile_cmd_branch_add(pf,
                      CMD_BRANCHTYPE_GOTO,
                      CMD_BRANCHCOND_TRUE, lk_call,
                      cmpv, 0, 0);
                  pfile_cmd_branch_add(pf,
                      CMD_BRANCHTYPE_GOTO,
                      CMD_BRANCHCOND_NONE, lk_skip,
                      VALUE_NONE, 0, 0);
                  /* call name'put(expr) */
                  pfile_cmd_label_add(pf, lk_call);
                  {
                    value_t *cparms;

                    cparms = CALLOC(2, sizeof(*cparms));
                    if (cparms) {
                      value_t dputv;

                      dputv = jv[JAL_VAL_TYPE_PUT];
                      jv[JAL_VAL_TYPE_PUT] = VALUE_NONE;
                      cparms[0] = VALUE_NONE;
                      cparms[1] = value_clone(expr);
                      value_lock(cparms[1]);
                      pfile_cmd_branch_add(pf,
                          CMD_BRANCHTYPE_CALL,
                          CMD_BRANCHCOND_NONE, LABEL_NONE,
                          VALUE_NONE, dputv, cparms);
                    }
                  }
                  pfile_cmd_label_add(pf, lk_skip);
                }
                jal_value_release(jv);
              }
            }
            pfile_cmd_label_add(pf, l_done);
            val  = VALUE_NONE;
            idx  = VALUE_NONE;
            /* both paths consumed their expr clones; the
             * outer release still owns the original */
          } else {
            value_release(tkind);
            value_release(tkind_src);
            pfile_log(pf, PFILE_LOG_ERR,
                "runtime-indexed access to a pointer array"
                " with pseudo-bound elements is not supported"
                " yet (use a const index)");
            value_release(val);
            val = VALUE_NONE;
          }
        } else {
          pfile_log(pf, PFILE_LOG_ERR,
              "runtime-indexed access to a pointer array with"
              " pseudo-bound elements is not supported yet"
              " (use a const index)");
          value_release(val);
          val = VALUE_NONE;
        }
      }
    }
  }

  ctx->val      = val;
  ctx->expr     = expr;
  ctx->idx      = idx;
  ctx->param_ct = param_ct;
  ctx->is_put   = is_put;
}

/* `p = x` through a pointer bound to a pseudo (`p is pv`): route the
 * store to the pseudo's 'put -- the write-side mirror of the rvalue
 * GET link (the through-slot store would corrupt the bind) */
static void jal_assign_pseudo_put_route(jal_assign_ctx_t *ctx)
{
  pfile_t   *pf = ctx->pf;
  value_t    val = ctx->val;
  value_t    expr = ctx->expr;
  boolean_t  is_put = ctx->is_put;
  size_t     param_ct = ctx->param_ct;
  operator_t assign_op = ctx->assign_op;
  value_t    putv;
  boolean_t  is_pseudo;

  /* `p = x` through a pointer bound to a pseudo (`p is pv`):
   * route the store to the pseudo's PUT -- the write-side
   * mirror of the rvalue GET link.  The slot holds no
   * pointee address for a pseudo, so the through-slot store
   * would corrupt whatever the bind left there.  Scalar RHS
   * only (pointer/array/WHEREIS RHS keep the back-end rules);
   * plain variable/array binds have no PUT and keep the
   * through-slot store. */
  if ((assign_op == OPERATOR_ASSIGN) && value_is_pointer(val)
      && expr && !value_is_pointer(expr) && !value_is_array(expr)
      && !value_is_value(expr) && !value_is_label(expr)) {
    putv = jal_pointer_put_link_get(pf, val, &is_pseudo);
    if (putv) {
      if (VARIABLE_DEF_TYPE_POINTER == value_type_get(putv)) {
        value_assign_ct_bump(putv, CTR_BUMP_INCR);
        value_dereference(putv);
      }
      value_release(val);
      val = putv;
      is_put = BOOLEAN_TRUE;
      param_ct = 2;
    } else if (is_pseudo) {
      pfile_log(pf, PFILE_LOG_ERR, "No PUT procedure");
      value_release(val);
      val = VALUE_NONE;
    }
  }

  ctx->val      = val;
  ctx->is_put   = is_put;
  ctx->param_ct = param_ct;
}

/* emit the 'put/'iput CALL: params are [index,] value; expr and idx
 * are locked into the command here */
static void jal_assign_call_emit(jal_assign_ctx_t *ctx)
{
  pfile_t *pf = ctx->pf;
  value_t  val = ctx->val;
  value_t  idx = ctx->idx;
  value_t  expr = ctx->expr;
  size_t   param_ct = ctx->param_ct;

  value_t *params;

  params = MALLOC(param_ct * sizeof(*params));
  if (!params) {
    pfile_log_syserr(pf, ENOMEM);
  } else {
    params[0] = VALUE_NONE;
    if (3 == param_ct) {
      params[1] = idx;
      params[2] = expr;
    } else {
      params[1] = expr;
    }
    value_lock(expr);
    value_lock(idx);
    pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_CALL,
      CMD_BRANCHCOND_NONE, LABEL_NONE, VALUE_NONE,
      val, params);
  }

}

/* emit the assignment: pointer-slot re-resolution for folded pointer
 * consts, the 'is link-name install, the pointer-bind bookkeeping
 * (whereis targets, bound counters, per-element pseudo links + the
 * stage-2 kind[]/puts[] mirrors), the must-bind use counting, and
 * the final operator command */
static void jal_assign_assign_emit(jal_assign_ctx_t *ctx)
{
  pfile_t   *pf = ctx->pf;
  value_t    val = ctx->val;
  value_t    expr = ctx->expr;
  operator_t assign_op = ctx->assign_op;
  boolean_t  lvalue_subscripted = ctx->lvalue_subscripted;
  char      *ptr_rhs_dup = ctx->ptr_rhs_dup;

  if (assign_op == OPERATOR_POINTER_ASSIGN && val
      && !lvalue_subscripted && !value_is_pointer(val)) {
    /* Folded `var pointer const byte p[N]` can leave `val` as the pointee
     * aggregate (INDF) while the symbol is still the pointer variable.
     * Re-resolve by user name so `value_alloc` gets POINTER storage. */
    const char *nm;
    variable_t  vv;
    value_t     slot;
    variable_t  vslot;

    slot = VALUE_NONE;
    nm   = value_name_get(val);
    if (nm && nm[0]) {
      vv = pfile_variable_find(pf, PFILE_LOG_NONE, nm, 0);
      if (vv && VARIABLE_NONE != vv) {
        slot = value_alloc(vv);
        variable_release(vv);
      }
    }
    if (!slot) {
      vslot = value_variable_get(val);
      if (vslot && VARIABLE_NONE != vslot) {
        slot = value_alloc(vslot);
      }
    }
    if (slot) {
      value_release(val);
      val = slot;
    }
  }
  if (ptr_rhs_dup) {
    variable_t dvv;

    dvv = value_variable_get(val);
    if (dvv && variable_is_pointer(dvv)) {
      if (!ptr_rhs_dup || !expr || value_is_pointer(expr)) {
        variable_pointer_rvalue_link_name_set(dvv, 0);
      } else {
        variable_pointer_rvalue_link_name_set(dvv, ptr_rhs_dup);
      }
    }
  }
  if (assign_op == OPERATOR_POINTER_ASSIGN) {
    variable_t dvv;

    /* the RHS variable's address escapes into the pointer
     * slot: without counting it as assigned, a
     * never-directly-written target folds to CONST and the
     * pointer becomes ROM-bound (stores refused) */
    pfile_pointer_bind_target_note(value_variable_get(expr));
    dvv = value_variable_get(val);
    if (dvv && variable_is_pointer(dvv)) {
      variable_pointer_whereis_target_set(dvv, expr);
      variable_pointer_bound_ct_bump(dvv);
    } else if (dvv
        && (value_is_pointer(val)
            || value_flag_test(val, VALUE_FLAG_POINTER))) {
      variable_const_t slot_ofs;

      /* slot-array element bind: counts as the ARRAY being
       * bound; runtime-indexed binds also suppress the
       * element-granular check (any element could move) */
      variable_pointer_bound_ct_bump(dvv);
      if (jal_value_const_baseofs_get(val, &slot_ofs)) {
        variable_pointer_whereis_slot_target_set(dvv, slot_ofs,
            expr);
        /* per-element pseudo link for const-index static
         * dispatch -- ONLY for actual pseudos: recording
         * plain names would mark the array and refuse its
         * runtime-indexed element access */
        if (ptr_rhs_dup && ptr_rhs_dup[0]) {
          value_t lkjv[JAL_VAL_TYPE_CT];

          if (jal_value_find_by_name(pf, ptr_rhs_dup, lkjv)
              && lkjv[JAL_VAL_TYPE_GET]) {
            variable_def_t        hhdef;
            variable_def_member_t hhdef_mbr;
            variable_sz_t         hhsz;
            variable_t            hkind;

            variable_pointer_slot_link_name_set(dvv, slot_ofs,
                ptr_rhs_dup);
            /* stage 2: kind[k]=1 mirror + puts[k]='put */
            hhdef_mbr = variable_def_member_get(
                variable_def_get(dvv));
            hhdef = hhdef_mbr
                ? variable_def_member_def_get(hhdef_mbr)
                : VARIABLE_DEF_NONE;
            hhsz = hhdef
                ? pfile_flat_def_storage_sz(pf, hhdef) : 0;
            hkind = (jal_pointer_slot_rt_enabled()
                && hhsz
                && variable_is_array(dvv))
                ? jal_pointer_slot_helpers_ensure(pf, dvv,
                    variable_def_member_ct_get(
                      variable_def_member_get(
                        variable_def_get(dvv))), hhsz)
                : VARIABLE_NONE;
            if (hkind) {
              value_t pdst;
              value_t pput;

              jal_pointer_slot_kind_store(pf, hkind,
                  slot_ofs, 1);
              pdst = value_alloc(
                  variable_pointer_slot_puts_var_get(dvv));
              if (pdst) {
                value_t               pofs;
                variable_def_t        pedef;
                variable_def_member_t pembr;

                pofs = pfile_constant_get(pf, slot_ofs,
                    VARIABLE_DEF_NONE);
                jal_val_baseofs_add(pf, pdst, pofs);
                value_release(pofs);
                pembr = variable_def_member_get(
                    variable_def_get(
                      variable_pointer_slot_puts_var_get(dvv)));
                pedef = pembr
                    ? variable_def_member_def_get(pembr)
                    : VARIABLE_DEF_NONE;
                if (pedef) {
                  value_def_set(pdst, pedef);
                }
                pput = lkjv[JAL_VAL_TYPE_PUT];
                lkjv[JAL_VAL_TYPE_PUT] = VALUE_NONE;
                pfile_cmd_op_add(pf, OPERATOR_POINTER_ASSIGN,
                    &pdst, pput, VALUE_NONE);
                value_release(pput);
              }
            }
          }
          jal_value_release(lkjv);
        } else {
          /* plain rebind of a formerly pseudo element:
           * restore kind[k]=0 when helpers exist */
          variable_t hkind;

          hkind = variable_pointer_slot_kind_var_get(dvv);
          if (hkind) {
            variable_def_t        hhdef;
            variable_def_member_t hhdef_mbr;
            variable_sz_t         hhsz;

            hhdef_mbr = variable_def_member_get(
                variable_def_get(dvv));
            hhdef = hhdef_mbr
                ? variable_def_member_def_get(hhdef_mbr)
                : VARIABLE_DEF_NONE;
            hhsz = hhdef
                ? pfile_flat_def_storage_sz(pf, hhdef) : 0;
            if (hhsz) {
              jal_pointer_slot_kind_store(pf, hkind,
                  slot_ofs, 0);
            }
          }
        }
      } else {
        variable_pointer_slot_rt_bind_ct_bump(dvv);
      }
    }
  }
  if ((assign_op == OPERATOR_ASSIGN)
      && (value_is_pointer(val)
          || value_flag_test(val, VALUE_FLAG_POINTER))) {
    variable_t dvv;

    dvv = value_variable_get(val);
    if (dvv) {
      if (expr
          && (value_is_array(expr) || value_is_function(expr)
              || value_is_pointer(expr))) {
        /* slot WRITE (`p = arr`, `fps[0] = fnc`, fn-pointer
         * copies `q = fp` -- the underef'd pointer RHS): the
         * slot is genuinely written -- count as bound. A
         * data-pointer RHS never survives the rvalue parse
         * underef'd: plain `q = p` is store-through (read
         * *p, write through *q), a USE, not a copy */
        variable_pointer_bound_ct_bump(dvv);
      } else {
        /* store-through (`p = x`, `p[i] = x`, `slot[j] = x`):
         * a USE of the slot -- must not silence the
         * must-bind check (cmd_opdst_set bumps ctr_assign) */
        variable_const_t slot_ofs;

        variable_use_ct_bump(dvv, CTR_BUMP_INCR);
        if (jal_value_const_baseofs_get(val, &slot_ofs)) {
          variable_pointer_slot_use_note(dvv, slot_ofs);
        }
      }
    }
  }
  pfile_cmd_op_add(pf, assign_op, &val, expr, VALUE_NONE);

  ctx->val = val;
}

/* no '=' and no '(' at the LHS tail: the procedure-pointer and
 * pointer/array-indexing call-syntax diagnostics */
static void jal_assign_expected_eq_diag(jal_assign_ctx_t *ctx)
{
  pfile_t *pf = ctx->pf;
  value_t  val = ctx->val;

  /* Check if this is a procedure pointer that needs call syntax */
  if (val && value_is_pointer(val)) {
    variable_def_t def = value_def_get(val);
    if (VARIABLE_DEF_TYPE_POINTER == value_type_get(val)) {
      /* Check if this is a procedure pointer by looking at the member definition */
      variable_def_member_t mbr = variable_def_member_get(def);
      if (mbr) {
        variable_def_t member_def = variable_def_member_def_get(mbr);
        if (member_def && VARIABLE_DEF_TYPE_FUNCTION == variable_def_type_get(member_def)) {
          pfile_log(pf, PFILE_LOG_ERR, "Procedure pointer requires call syntax: %s() (got '%s')", 
                    value_name_get(val), pf_token_get(pf, PF_TOKEN_CURRENT));
        } else {
          pfile_log(pf, PFILE_LOG_ERR, "Procedure pointer array requires call syntax with parentheses: %s[index]() (got '%s')", 
                    value_name_get(val), pf_token_get(pf, PF_TOKEN_CURRENT));
        }
      } else {
        pfile_log(pf, PFILE_LOG_ERR, "Procedure pointer array requires call syntax with parentheses: %s[index](value,..) (got '%s')", 
                  value_name_get(val), pf_token_get(pf, PF_TOKEN_CURRENT));
      }
    } else {
      pfile_log(pf, PFILE_LOG_ERR, "'=' expected (got '%s')", 
                pf_token_get(pf, PF_TOKEN_CURRENT));
    }
  } else if (val && VARIABLE_DEF_TYPE_ARRAY == value_type_get(val)) {
    /* Check if this is a procedure pointer array */
    variable_def_t def = value_def_get(val);
    variable_def_member_t mbr = variable_def_member_get(def);
    if (mbr) {
      variable_def_t member_def = variable_def_member_def_get(mbr);
      if (member_def && VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(member_def)) {
        /* Check if the pointer points to a function */
        variable_def_member_t ptr_mbr = variable_def_member_get(member_def);
        if (ptr_mbr) {
          variable_def_t ptr_member_def = variable_def_member_def_get(ptr_mbr);
          if (ptr_member_def && VARIABLE_DEF_TYPE_FUNCTION == variable_def_type_get(ptr_member_def)) {
            pfile_log(pf, PFILE_LOG_ERR, "Procedure pointer array requires indexing and call syntax: %s[index]() (got '%s')", 
                      value_name_get(val), pf_token_get(pf, PF_TOKEN_CURRENT));
          } else {
            pfile_log(pf, PFILE_LOG_ERR, "Pointer array requires indexing: %s[index] (got '%s')", 
                      value_name_get(val), pf_token_get(pf, PF_TOKEN_CURRENT));
          }
        } else {
          pfile_log(pf, PFILE_LOG_ERR, "Pointer array requires indexing: %s[index] (got '%s')", 
                    value_name_get(val), pf_token_get(pf, PF_TOKEN_CURRENT));
        }
      } else {
        pfile_log(pf, PFILE_LOG_ERR, "Array requires indexing: %s[index] (got '%s')", 
                  value_name_get(val), pf_token_get(pf, PF_TOKEN_CURRENT));
      }
    } else {
      pfile_log(pf, PFILE_LOG_ERR, "Array requires indexing: %s[index] (got '%s')", 
                value_name_get(val), pf_token_get(pf, PF_TOKEN_CURRENT));
    }
  } else {
    pfile_log(pf, PFILE_LOG_ERR, "'=' expected (got '%s')", 
              pf_token_get(pf, PF_TOKEN_CURRENT));
  }

}

static boolean_t jal_parse_assign(pfile_t *pf)
{
  const char      *ptr;
  jal_assign_ctx_t ctx;

  ptr     = pf_token_get(pf, PF_TOKEN_CURRENT);
  ctx.pf      = pf;
  ctx.success = jal_value_find(pf, ptr, ctx.jval);
  ctx.idx     = VALUE_NONE;
  ctx.param_ct = 0;
  ctx.lvalue_subscripted = BOOLEAN_FALSE;
  /* nb: precedence of use:
   *     val_put : highest
   *     val
   *     val_vput
   */
  if (ctx.success) {
    jal_assign_lhs_resolve(&ctx);
    value_lock(ctx.val);
    if (!ctx.is_put
        && (VARIABLE_DEF_TYPE_FUNCTION == value_type_get(ctx.val))) {
      value_t ret_val;

      pf_token_get(pf, PF_TOKEN_NEXT);
      ret_val = jal_parse_call(pf, ctx.val, JAL_PARSE_CALL_FLAG_NONE);
      if (ret_val) {
        value_release(ret_val);
      }
    } else {
      if (!ctx.is_put) {
        jal_assign_lvalue_parse(&ctx);
      } else {
        jal_assign_put_shape_parse(&ctx);
      }
      if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "(")) {
        /* Check if this is a function pointer call */
        if (ctx.val && value_is_pointer(ctx.val)) {
          /* This is a function pointer call like my_proc(42) */
          /* Pass the pointer directly to jal_parse_call and let it handle the dereference */
          value_release(jal_parse_call(pf, ctx.val, JAL_PARSE_CALL_FLAG_NONE));
        } else if (!ctx.val) {
          value_release(jal_parse_call(pf, VALUE_NONE, JAL_PARSE_CALL_FLAG_NONE));
        } else {
          /* Regular function call */
          value_release(jal_parse_call(pf, VALUE_NONE, JAL_PARSE_CALL_FLAG_NONE));
        }
      } else if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "=")
                 || pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "is")) {
        ctx.ptr_rhs_dup = 0;
        /* Determine which assignment operator to use */
        if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "is")) {
          ctx.assign_op = OPERATOR_POINTER_ASSIGN;
        } else {
          ctx.assign_op = OPERATOR_ASSIGN;
        }

        pf_token_get(pf, PF_TOKEN_NEXT);

        jal_assign_rhs_parse(&ctx);
        if (ctx.expr) {
          jal_assign_rhs_refusals(&ctx);
          if (ctx.val) {
            jal_assign_slot_dispatch(&ctx);
            jal_assign_pseudo_put_route(&ctx);

            if (ctx.is_put) {
              jal_assign_call_emit(&ctx);
            } else {
              jal_assign_assign_emit(&ctx);
            }
          }
          value_release(ctx.expr);
        }
        if (ctx.ptr_rhs_dup) {
          FREE(ctx.ptr_rhs_dup);
        }
      } else {
        jal_assign_expected_eq_diag(&ctx);
      }
      value_release(ctx.idx);
    }
    value_release(ctx.val);
    jal_value_release(ctx.jval);
  }
  return ctx.success;
}


/* parse BLOCK...END BLOCK */
static void jal_parse_block(pfile_t *pf, const pfile_pos_t *statement_start)
{
  jal_block_process(pf, JAL_BLOCK_PROCESS_FLAG_NONE);
  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, "end")
    && pf_token_is(pf, PF_TOKEN_NEXT, PFILE_LOG_ERR, "block")) {
    pf_token_get(pf, PF_TOKEN_NEXT);
  } else {
    jal_block_start_show(pf, "BLOCK", statement_start);
  }
}

static void jal_parse_assert(pfile_t *pf, const pfile_pos_t *statement_start)
{
  value_t expr;

  UNUSED(statement_start);

  if (!pfile_flag_test(pf, PFILE_FLAG_DEBUG_EMULATOR)) {
    pfile_codegen_disable(pf, BOOLEAN_TRUE);
  }
  expr = jal_parse_expr(pf);
  if ((VALUE_NONE != expr) 
      && pfile_flag_test(pf, PFILE_FLAG_DEBUG_EMULATOR)) {
    pfile_cmd_add(pf, cmd_assert_alloc(expr));
  }
  value_release(expr);
  if (!pfile_flag_test(pf, PFILE_FLAG_DEBUG_EMULATOR)) {
    pfile_codegen_disable(pf, BOOLEAN_FALSE);
  }
}

/*
 * ALIAS name IS var
 */
static void jal_parse_alias(pfile_t *pf, const pfile_pos_t *statement_start)
{
  UNUSED(statement_start);
  if (!jal_token_is_identifier(pf)) {
    pfile_log(pf, PFILE_LOG_ERR, "identifier expected");
  } else {
    char       *id;
    variable_t  var;

    id = STRDUP(pf_token_get(pf, PF_TOKEN_CURRENT));
    if (!id) {
      pfile_log_syserr(pf, ENOMEM);
    } else {
      if (pf_token_is(pf, PF_TOKEN_NEXT, PFILE_LOG_ERR, "is")) {
        pf_token_get(pf, PF_TOKEN_NEXT);
      }
      var = jal_parse_var_alias(pf);
      if (var) {
        variable_t avar;

        if (RESULT_OK == pfile_variable_alloc(pf, PFILE_VARIABLE_ALLOC_LOCAL,
          id, variable_def_get(var), var, &avar)) {
          variable_flag_set(avar, VARIABLE_FLAG_ALIAS);
          variable_release(avar);
        }
      }
      FREE(id);
    }
  }
}

/*
 * NAME
 *   jal_statement_process
 *
 * DESCRIPTION
 *   process a single JAL statement
 *
 * PARAMETERS
 *   pf : pfile handle
 *
 * RETURN
 *   BOOLEAN_TRUE : a statement was processed
 *   BOOLEAN_FALSE: no statement was processed
 *
 * NOTES
 */
boolean_t jal_statement_process(pfile_t *pf)
{
  size_t      ii;
  cmd_t       cmd_init;
  pfile_pos_t pos;
  boolean_t   rc;

  /*
   * note : JAL is much simpler than BASIC because everything
   *        must be predefined. This means only a single pass
   *        through the source.
   *        Also, there is no end of statement to deal with.
   */       
  /* NONE   -- immediately call the action routine
   * ABSORB -- get the next statement before calling the action routine
   */
#define JAL_STATEMENT_TOKEN_NONE   0x0000
#define JAL_STATEMENT_TOKEN_ABSORB 0x0001
  static struct {
    const char *tag;
    void      (*fn)(pfile_t *, const pfile_pos_t *statement_start);
    unsigned    flags;
  } keyword_table[] = {
    {"var",         jal_parse_var,        JAL_STATEMENT_TOKEN_ABSORB},
    {"const",       jal_parse_const,      JAL_STATEMENT_TOKEN_ABSORB},
    {"if",          jal_parse_if,         JAL_STATEMENT_TOKEN_ABSORB},
    {"while",       jal_parse_while,      JAL_STATEMENT_TOKEN_ABSORB},
    {"for",         jal_parse_for,        JAL_STATEMENT_TOKEN_ABSORB},
    {"forever",     jal_parse_forever,    JAL_STATEMENT_TOKEN_ABSORB},
    {"asm",         jal_parse_asm,        JAL_STATEMENT_TOKEN_ABSORB},
    {"assembler",   jal_parse_assembler,  JAL_STATEMENT_TOKEN_ABSORB},
    {"return",      jal_parse_return,     JAL_STATEMENT_TOKEN_ABSORB},
    {"procedure",   jal_parse_procedure,  JAL_STATEMENT_TOKEN_ABSORB},
    {"function",    jal_parse_function,   JAL_STATEMENT_TOKEN_ABSORB},
    {"include",     jal_parse_include,    JAL_STATEMENT_TOKEN_NONE},
    {"pragma",      jal_parse_pragma,     JAL_STATEMENT_TOKEN_ABSORB},
    {"task",        jal_parse_task,       JAL_STATEMENT_TOKEN_ABSORB},
    {"_task_start", jal_parse_start,      JAL_STATEMENT_TOKEN_ABSORB},
    {"_task_yield", jal_parse_task_yield, JAL_STATEMENT_TOKEN_ABSORB},
    {"suspend",     jal_parse_task_yield_deprecated, JAL_STATEMENT_TOKEN_ABSORB},
    {"_task_suspend", jal_parse_task_suspend,       JAL_STATEMENT_TOKEN_ABSORB},
    {"_task_resume",  jal_parse_task_resume,  JAL_STATEMENT_TOKEN_ABSORB},
    {"_task_kill",    jal_parse_task_kill,    JAL_STATEMENT_TOKEN_ABSORB},
    {"_task_restart", jal_parse_task_restart, JAL_STATEMENT_TOKEN_ABSORB},
    {"_usec_delay", jal_parse_usec_delay, JAL_STATEMENT_TOKEN_ABSORB},
    {"_warn",       jal_parse_warn,       JAL_STATEMENT_TOKEN_ABSORB},
    {"_error",      jal_parse_error,      JAL_STATEMENT_TOKEN_ABSORB},
    {"_debug",      jal_parse_debug,      JAL_STATEMENT_TOKEN_ABSORB},
    {"block",       jal_parse_block,      JAL_STATEMENT_TOKEN_ABSORB},
    {"case",        jal_parse_case,       JAL_STATEMENT_TOKEN_ABSORB},
    {"assert",      jal_parse_assert,     JAL_STATEMENT_TOKEN_ABSORB},
    {"exit",        jal_parse_exit,       JAL_STATEMENT_TOKEN_ABSORB},
    {"repeat",      jal_parse_repeat,     JAL_STATEMENT_TOKEN_ABSORB},
    {"alias",       jal_parse_alias,      JAL_STATEMENT_TOKEN_ABSORB},
    {"record",      jal_parse_record,     JAL_STATEMENT_TOKEN_ABSORB}
  };

  pf_token_start_get(pf, &pos);
  pfile_statement_start_set(pf, &pos);

  cmd_init = pfile_cmdlist_tail_get(pf);
  for (ii = 0; 
       (ii < COUNT(keyword_table))
       && !pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE,
         keyword_table[ii].tag);
       ii++)
    ;
  if (ii < COUNT(keyword_table)) {
    if (keyword_table[ii].flags & JAL_STATEMENT_TOKEN_ABSORB) {
      pf_token_get(pf, PF_TOKEN_NEXT);
    }
    keyword_table[ii].fn(pf, &pos);
    rc = BOOLEAN_TRUE;
  } else if (jal_parse_assign(pf)) {
    rc = BOOLEAN_TRUE;
  } else {
    rc = BOOLEAN_FALSE;
  }
  if (pfile_cmdlist_tail_get(pf) != cmd_init) {
    pfile_cmd_special_add(pf, CMD_TYPE_STATEMENT_END, 0);
  }
  return rc;
}


/*
 * NAME
 *   jal_block_process
 *
 * DESCRIPTION
 *   process until a block end occurs
 *
 * PARAMETERS
 *   pf    : returned by pfile_open()
 *   flags : see JAL_BLOCK_PROCESS_FLAG_* in jal_blck.h
 *
 * RETURN
 *   none
 *
 * NOTES
 *   return on EOF or the first non parsable token
 */
void jal_block_process(pfile_t *pf, flag_t flags)
{
  if (!(flags & JAL_BLOCK_PROCESS_FLAG_NO_BLOCK)) {
    pfile_block_enter(pf);
  }
  while (jal_statement_process(pf))
    ; /* empty body */
  if (!(flags & JAL_BLOCK_PROCESS_FLAG_NO_BLOCK)) {
    pfile_block_leave(pf);
  }
}

/* Start of the processing of the source file. */
void jal_source_process(pfile_t *pf)
{
  label_t     lbl;
  pfile_pos_t pos;

  pf_token_get(pf, PF_TOKEN_FIRST);
  pf_token_start_get(pf, &pos);
  pfile_statement_start_set(pf, &pos);
  lbl = pfile_label_alloc(pf, "_main");
  pfile_user_entry_set(pf, lbl);
  pfile_cmd_label_add(pf, lbl);
  label_release(lbl);
  jal_block_process(pf, JAL_BLOCK_PROCESS_FLAG_NO_BLOCK);
  if (!pf_token_is_eof(pf)) {
    pfile_log(pf, PFILE_LOG_ERR, "unexpected token: %s",
      pf_token_get(pf, PF_TOKEN_CURRENT));
  }
  pfile_cmd_special_add(pf, CMD_TYPE_END, 0);
}


void jal_file_process(pfile_t *pf, char *pre_inc)
{
  size_t        ii;

  static const struct {
    const char         *tag;
    variable_def_type_t type;
    flag_t              flags;
    variable_sz_t       sz;
  } basetypes[] = {
    {"bit",     VARIABLE_DEF_TYPE_BOOLEAN, VARIABLE_DEF_FLAG_BIT,    1},
    {"byte",    VARIABLE_DEF_TYPE_INTEGER, VARIABLE_DEF_FLAG_NONE,   1},
    {"sbyte",   VARIABLE_DEF_TYPE_INTEGER, VARIABLE_DEF_FLAG_SIGNED, 1},
    {"word",    VARIABLE_DEF_TYPE_INTEGER, VARIABLE_DEF_FLAG_NONE,   2},
    {"sword",   VARIABLE_DEF_TYPE_INTEGER, VARIABLE_DEF_FLAG_SIGNED, 2},
    {"dword",   VARIABLE_DEF_TYPE_INTEGER, VARIABLE_DEF_FLAG_NONE,   4},
    {"sdword",  VARIABLE_DEF_TYPE_INTEGER, VARIABLE_DEF_FLAG_SIGNED, 4},
    {"qword",   VARIABLE_DEF_TYPE_INTEGER, VARIABLE_DEF_FLAG_NONE,   8},
    {"sqword",  VARIABLE_DEF_TYPE_INTEGER, VARIABLE_DEF_FLAG_SIGNED, 8},
    {"oword",   VARIABLE_DEF_TYPE_INTEGER, VARIABLE_DEF_FLAG_NONE,   16},
    {"soword",  VARIABLE_DEF_TYPE_INTEGER, VARIABLE_DEF_FLAG_SIGNED, 16},
    {"float",   VARIABLE_DEF_TYPE_FLOAT,   VARIABLE_DEF_FLAG_SIGNED, 4},
    {"pointer", VARIABLE_DEF_TYPE_POINTER, VARIABLE_DEF_FLAG_NONE,   2}
  };

  for (ii = 0; ii < COUNT(basetypes); ii++) {
    variable_def_t vdef;

    vdef = variable_def_alloc(basetypes[ii]. tag,basetypes[ii].type,  
      basetypes[ii].flags, basetypes[ii].sz);
    if (vdef) {
      pfile_variable_def_add(pf, vdef);
    }
  }

  pic_init(pf); /* intialize the pic-specific bits */
  pfile_log(pf, PFILE_LOG_INFO, "generating p-code");
  while (pre_inc) {
    char *next;

    next = strchr(pre_inc, ';');
    if (next) {
      *next = 0;
    }
    pfile_include_process(pf, (next) ? (next - pre_inc) : 0, pre_inc);
    if (next) {
      *next = ';';
      next++;
    }
    pre_inc = next;
  }
  jal_source_process(pf);
  /* end-of-parse diagnostics over the complete program (Option A:
   * string heap sizing recommendation) */
  jal_string_heap_sizing_report(pf);
}

void jal_block_start_show(pfile_t *pf, const char *tag, 
  const pfile_pos_t *statement_start)
{
  pfile_log(pf, PFILE_LOG_ERR,
    "{%s starts at %s:%u}", tag,
    pfile_source_name_get(statement_start->src),
    statement_start->line);
}

