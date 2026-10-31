/**********************************************************
 **
 ** jal_expr.c : JAL expression parsing
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: fork extensions (heavy rework)
 **
 ***********************************************************/
#include <errno.h>
#include <string.h>
#include "../libutils/mem.h"
#include "../libcore/pfile.h"
#include "../libcore/pf_cmd.h"
#include "../libcore/pf_block.h"
#include "../libcore/pf_expr.h"
#include "../libcore/variable.h"
#include "../libcore/pf_proc.h"
#include "jal_proc.h"
#include "jal_blck.h"
#include "jal_tokn.h"
#include "jal_vdef.h"
#include "jal_string.h"
#include "jal_expr.h"

static variable_def_t jal_pointer_pointee_def_get(const value_t val)
{
  variable_def_t        ptr_def;
  variable_def_member_t mbr;

  if (!val || VARIABLE_DEF_TYPE_POINTER != value_type_get(val)) {
    return VARIABLE_DEF_NONE;
  }
  ptr_def = value_def_get(val);
  if (!ptr_def) {
    return VARIABLE_DEF_NONE;
  }
  mbr = variable_def_member_get(ptr_def);
  return (mbr && !variable_def_member_link_get(mbr))
    ? variable_def_member_def_get(mbr)
    : VARIABLE_DEF_NONE;
}

/* Pointer whose pointee def is FUNCTION (procedure or function pointer). */
static boolean_t jal_value_points_to_callable_function(value_t val)
{
  variable_def_t inner;

  inner = jal_pointer_pointee_def_get(val);
  return inner
    && (VARIABLE_DEF_TYPE_FUNCTION == variable_def_type_get(inner));
}

/*
 * True when a data pointer's pointee type is an array (e.g. **`var pointer var byte p[3]`**
 * folded to pointer-to-**`byte[3]`**). Scalar pointee (**`var pointer var byte p`**) is false.
 */
static boolean_t jal_data_pointer_pointee_is_array(const value_t val)
{
  variable_def_t pto;

  pto = jal_pointer_pointee_def_get(val);
  return pto && (VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(pto));
}

/*
 * **`byte in name[]`** / byref formals: lowered to a **POINTER** whose def copies
 * **`VARIABLE_DEF_FLAG_IN`** (**`jal_proc.c`**). Subscript and **COUNT** must still work.
 */
static boolean_t jal_data_pointer_in_formal_allows_subscript(const value_t val)
{
  variable_def_t d;

  if (!val || !value_is_pointer(val)) {
    return BOOLEAN_FALSE;
  }
  if (jal_value_points_to_callable_function(val)) {
    return BOOLEAN_FALSE;
  }
  d = value_def_get(val);
  return d && variable_def_flag_test(d, VARIABLE_DEF_FLAG_IN);
}

/* **`_name_count`** for open-array formals: same slot index as **`pfile_proc_define`** (member list). */
static value_t jal_value_find_open_array_count_param(pfile_t *pf,
    const char *want_tag)
{
  pfile_proc_t              *proc;
  variable_def_member_t      mbr;
  size_t                     ii;
  value_t                    pv;

  proc = pfile_proc_active_get(pf);
  if (!proc || !want_tag) {
    return VALUE_NONE;
  }
  for (ii = 0, mbr = variable_def_member_get(pfile_proc_def_get(proc));
       mbr;
       ii++, mbr = variable_def_member_link_get(mbr)) {
    const char *tag;

    tag = variable_def_member_tag_get(mbr);
    if (tag && 0 == strcmp(tag, want_tag) && ii < pfile_proc_param_ct_get(proc)) {
      pv = pfile_proc_param_get(proc, ii);
      if (pv) {
        return value_clone(pv);
      }
      break;
    }
  }
  return VALUE_NONE;
}

/*
 * Illegal **`p[expr]`** on pointer-to-scalar: clear *pval, drop jvals slots that alias it,
 * and balance refcounts. When opt_locked_jvals is non-NULL, **value_lock** was applied in
 * **jal_parse_value** (two refs); otherwise one ref.
 */
static void jal_parse_subscript_abort_illegal_pointer_subscript(
    value_t *pval, value_t opt_locked_jvals[JAL_VAL_TYPE_CT])
{
  value_t victim;
  size_t  ii;

  victim = *pval;
  *pval  = VALUE_NONE;
  if (opt_locked_jvals) {
    for (ii = 0; ii < JAL_VAL_TYPE_CT; ii++) {
      if (opt_locked_jvals[ii] == victim) {
        opt_locked_jvals[ii] = VALUE_NONE;
      }
    }
    value_release(victim);
    value_release(victim);
  } else {
    value_release(victim);
  }
}

void jal_val_baseofs_add(pfile_t *pf, value_t val, value_t ofs)
{
  value_t baseofs;
  value_t rval;

  /* A runtime-indexed element of a const ROM table (`T[k]`) is an rvalue:
   * read it through the lookup machinery into a RAM temp before it becomes
   * an address dependency.  Attached raw, the classic back ends treat the
   * table variable itself as a file operand -- the element-size-1 path
   * (byte arrays) skips the scaling MUL that otherwise materializes the
   * lookup, so `arr[T[k]]` died with "banked data op (MOVLB) for operand
   * with unknown file address (T); 0xFFFF/256 is not a valid bank select"
   * on every classic core (see fixes/fix_const_table_runtime_subscript.md).
   * value_is_lookup is exactly "declared-const variable whose baseofs is
   * not constant", i.e. the runtime-indexed table read. */
  rval = ofs;
  if (ofs && value_is_lookup(ofs)) {
    value_t tmp;

    tmp = pfile_value_temp_get_from_def(pf, value_def_get(ofs));
    if (tmp) {
      pfile_cmd_op_add_assign(pf, OPERATOR_ASSIGN, &tmp, ofs);
      rval = tmp;
    }
  }

  baseofs = value_baseofs_get(val);
  if (VALUE_NONE == baseofs) {
    value_baseofs_set(val, rval);
  } else if (value_is_const(baseofs) && value_is_const(rval)) {
    value_t cval;

    cval = pfile_constant_get(pf,
        value_const_get(baseofs) + value_const_get(rval),
        VARIABLE_DEF_NONE);
    value_baseofs_set(val, cval);
    value_release(cval);
  } else {
    value_t tmp;

    tmp = VALUE_NONE;
    pfile_cmd_op_add(pf, OPERATOR_ADD, &tmp, baseofs, rval);
    value_baseofs_set(val, tmp);
    value_release(tmp);
  }
  if (rval != ofs) {
    value_release(rval);
  }
}

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

static boolean_t jal_variable_is_array_of_pointers(variable_t var)
{
  variable_def_t        def;
  variable_def_member_t mbr;
  variable_def_t        mdef;

  if (!var || !variable_is_array(var)) {
    return BOOLEAN_FALSE;
  }
  def = variable_def_get(var);
  mbr = def ? variable_def_member_get(def) : 0;
  mdef = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
  return mdef
    && (VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(mdef));
}

static boolean_t jal_variable_def_tree_contains_pointer(variable_def_t def)
{
  variable_def_member_t mbr;

  if (!def) {
    return BOOLEAN_FALSE;
  }
  if (VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(def)) {
    return BOOLEAN_TRUE;
  }
  if ((VARIABLE_DEF_TYPE_ARRAY != variable_def_type_get(def))
      && (VARIABLE_DEF_TYPE_REFERENCE != variable_def_type_get(def))) {
    return BOOLEAN_FALSE;
  }
  mbr = variable_def_member_get(def);
  return mbr && jal_variable_def_tree_contains_pointer(
      variable_def_member_def_get(mbr));
}

static boolean_t jal_variable_tree_contains_pointer(variable_t var)
{
  return var && jal_variable_def_tree_contains_pointer(variable_def_get(var));
}

boolean_t jal_parse_subscript(pfile_t *pf, value_t *pval,
    value_t opt_locked_jvals[JAL_VAL_TYPE_CT])
{
  boolean_t rc;
  value_t   val;

  rc  = BOOLEAN_FALSE;
  val = *pval;
  if (!val) {
    return BOOLEAN_FALSE;
  }
  if ((value_is_array(val) || value_is_pointer(val))
    && pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "[")) {
      value_t n;

      if (value_is_pointer(val)
          && !jal_data_pointer_pointee_is_array(val)
          && !jal_data_pointer_in_formal_allows_subscript(val)) {
        pfile_log(pf, PFILE_LOG_ERR,
            "subscript on data pointer requires pointer-to-array "
            "(e.g. var pointer var byte p[3]); for pointer-to-scalar use the name "
            "without [...] or index the backing array");
        pf_token_get(pf, PF_TOKEN_NEXT);
        (void) jal_parse_expr(pf);
        if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, "]")) {
          pf_token_get(pf, PF_TOKEN_NEXT);
        }
        jal_parse_subscript_abort_illegal_pointer_subscript(pval, opt_locked_jvals);
        return BOOLEAN_TRUE;
      }

      rc = BOOLEAN_TRUE;
      pf_token_get(pf, PF_TOKEN_NEXT);
      n = jal_parse_expr(pf);
      if (n) {
        /* n *= sz */
        value_t          tmp;
        variable_const_t bound;
        boolean_t        pointer_callable_slot;
        boolean_t        array_pointer_slot;
        boolean_t        bit_array_slot;
        boolean_t        dyn_string;

        /* pointer-slot array element with a per-offset pseudo link:
         * const index -> static dispatch to the linked 'get (the call
         * replaces the element read; the index is passed when the
         * accessor takes one, ownership mirrors jal_parse_call);
         * runtime index on a marked array -> clean refusal */
        {
          variable_t psv;

          psv = value_variable_get(val);
          if (psv && variable_is_array(psv)
              && !pfile_parse_lvalue_ctx(pf)
              && variable_pointer_slot_links_any(psv)) {
            variable_def_t        pmdef;
            variable_def_member_t pmmbr;
            variable_sz_t         psz;

            pmmbr = variable_def_member_get(variable_def_get(psv));
            pmdef = pmmbr ? variable_def_member_def_get(pmmbr)
                : VARIABLE_DEF_NONE;
            psz   = pmdef ? pfile_flat_def_storage_sz(pf, pmdef) : 0;
            if (psz && value_is_const(n)) {
              const char *plink;

              plink = variable_pointer_slot_link_name_get(psv,
                  value_const_get(n) * psz);
              if (plink) {
                value_t pjv[JAL_VAL_TYPE_CT];

                if (jal_value_find_by_name(pf, plink, pjv)
                    && pjv[JAL_VAL_TYPE_GET]
                    && (VARIABLE_DEF_TYPE_FUNCTION
                        == value_type_get(pjv[JAL_VAL_TYPE_GET]))) {
                  value_t               pget;
                  value_t               ptmp;
                  value_t              *pparms;
                  variable_def_member_t pret;
                  variable_def_member_t pidx;
                  size_t                pn;

                  pget = pjv[JAL_VAL_TYPE_GET];
                  pjv[JAL_VAL_TYPE_GET] = VALUE_NONE;
                  jal_value_release(pjv);
                  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR,
                      "]")) {
                    pf_token_get(pf, PF_TOKEN_NEXT);
                  }
                  /* member 0 is the return; its first link is the
                   * index formal when the accessor takes one */
                  pret = variable_def_member_get(value_def_get(pget));
                  pidx = pret ? variable_def_member_link_get(pret) : 0;
                  pn   = pidx ? 2 : 1;
                  pparms = CALLOC(pn, sizeof(*pparms));
                  if (!pparms) {
                    pfile_log_syserr(pf, ENOMEM);
                  } else {
                    pparms[0] = pfile_value_temp_get_from_def(pf,
                        pret ? variable_def_member_def_get(pret)
                             : VARIABLE_DEF_NONE);
                    if (pidx) {
                      pparms[1] = n;
                      value_lock(n);
                    }
                    ptmp = pparms[0];
                    value_lock(ptmp);
                    pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_CALL,
                        CMD_BRANCHCOND_NONE, LABEL_NONE, VALUE_NONE,
                        pget, pparms);
                    value_release(*pval);
                    *pval = ptmp;
                    return BOOLEAN_TRUE;
                  }
                  value_release(pget);
                  value_release(n);
                  return BOOLEAN_TRUE;
                }
                jal_value_release(pjv);
              }
            } else if (!value_is_const(n)) {
              /* stage-2 runtime dispatch: kind[k] test selects the
               * pseudo path (indirect zero-arg call through the slot
               * value, which stores 'get) or the plain deref */
              variable_t hkind;
              boolean_t  emitted;

              emitted = BOOLEAN_FALSE;
              hkind = variable_pointer_slot_kind_var_get(psv);
              if (jal_pointer_slot_rt_enabled() && hkind) {
                value_t        tkind;
                value_t        tkind_src;
                value_t        tres;
                label_t        l_pseudo;
                label_t        l_done;

                if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR,
                    "]")) {
                  pf_token_get(pf, PF_TOKEN_NEXT);
                }
                l_pseudo = pfile_label_alloc(pf, 0);
                l_done   = pfile_label_alloc(pf, 0);
                tkind    = VALUE_NONE;
                tres     = VALUE_NONE;
                tkind_src = value_alloc(hkind);
                if (tkind_src && l_pseudo && l_done) {
                  value_t               nclone;
                  variable_def_member_t embr;

                  /* tkind = kind[k] -- kind[] is byte-offset
                   * indexed; scale the index like the slot elements */
                  nclone = VALUE_NONE;
                  {
                    value_t szv4;

                    szv4 = pfile_constant_get(pf, psz,
                        VARIABLE_DEF_NONE);
                    pfile_cmd_op_add(pf, OPERATOR_MUL, &nclone,
                        value_clone(n), szv4);
                    value_release(szv4);
                  }
                  jal_val_baseofs_add(pf, tkind_src, nclone);
                  value_release(nclone);
                  embr = variable_def_member_get(variable_def_get(hkind));
                  if (embr) {
                    value_def_set(tkind_src,
                        variable_def_member_def_get(embr));
                  }
                  /* branch directly on the kind element value (an
                   * intermediate temp aliases the baseofs temp) */
                  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO,
                      CMD_BRANCHCOND_TRUE, l_pseudo, tkind_src, 0, 0);
                  /* plain: tres = *(slot k) -- narrow the clone to
                   * the ELEMENT def before the deref (the clone keeps
                   * the array def otherwise and the assign verifier
                   * rejects the op) */
                  {
                    value_t elemsrc;

                    elemsrc = value_clone(val);
                    if (elemsrc) {
                      variable_def_member_t embr2;
                      variable_def_t        edef2;
                      value_t               nscaled;

                      /* scale the index by the element size (the
                       * subscript machinery's n *= sz) */
                      nscaled = VALUE_NONE;
                      {
                        value_t szv;

                        szv = pfile_constant_get(pf, psz,
                            VARIABLE_DEF_NONE);
                        pfile_cmd_op_add(pf, OPERATOR_MUL, &nscaled,
                            value_clone(n), szv);
                        value_release(szv);
                      }
                      jal_val_baseofs_add(pf, elemsrc, nscaled);
                      value_release(nscaled);
                      embr2 = variable_def_member_get(value_def_get(val));
                      edef2 = embr2
                          ? variable_def_member_def_get(embr2)
                          : VARIABLE_DEF_NONE;
                      if (edef2) {
                        value_def_set(elemsrc, edef2);
                      }
                      value_dereference(elemsrc);
                      pfile_cmd_op_add(pf, OPERATOR_ASSIGN, &tres,
                          elemsrc, VALUE_NONE);
                    }
                  }
                  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO,
                      CMD_BRANCHCOND_NONE, l_done, VALUE_NONE, 0, 0);
                  /* pseudo: call through the slot element directly --
                   * pic_branch_proc stages &slot[k] and loads the fn
                   * address for runtime-baseofs call targets */
                  pfile_cmd_label_add(pf, l_pseudo);
                  {
                    value_t felem;

                    felem = value_clone(val);
                    if (felem) {
                      value_t nscaled3;
                      variable_def_member_t fembr;

                      nscaled3 = VALUE_NONE;
                      {
                        value_t szv3;

                        szv3 = pfile_constant_get(pf, psz,
                            VARIABLE_DEF_NONE);
                        pfile_cmd_op_add(pf, OPERATOR_MUL, &nscaled3,
                            value_clone(n), szv3);
                        value_release(szv3);
                      }
                      jal_val_baseofs_add(pf, felem, nscaled3);
                      value_release(nscaled3);
                      fembr = variable_def_member_get(value_def_get(val));
                      if (fembr) {
                        value_def_set(felem,
                            variable_def_member_def_get(fembr));
                      }
                      /* view as pointer-to-function so pic_branch_proc
                       * treats the call as indirect-fn (return pickup,
                       * param staging; a byte pointee gets no return) */
                      {
                        variable_def_t fnptr_def;

                        fnptr_def = jal_pointer_fnptr_def_get(pf);
                        if (fnptr_def) {
                          value_def_set(felem, fnptr_def);
                        }
                      }
                      {
                        value_t *fparms;

                        fparms = CALLOC(1, sizeof(*fparms));
                        if (fparms) {
                          value_t tret;

                          tret = pfile_value_temp_get_from_def(pf,
                              pfile_variable_def_find(pf,
                                  PFILE_LOG_NONE, "byte"));
                          fparms[0] = tret;
                          if (tret) {
                            value_lock(tret);
                            value_lock(felem);
                            pfile_cmd_branch_add(pf,
                                CMD_BRANCHTYPE_CALL,
                                CMD_BRANCHCOND_NONE, LABEL_NONE,
                                VALUE_NONE, felem, fparms);
                            pfile_cmd_op_add(pf, OPERATOR_ASSIGN,
                                &tres, tret, VALUE_NONE);
                          } else {
                            FREE(fparms);
                          }
                        }
                      }
                    }
                  }
                  pfile_cmd_label_add(pf, l_done);
                  value_release(*pval);
                  *pval = tres;
                  if (tres) {
                    value_lock(tres);
                  }
                  value_release(n);
                  emitted = BOOLEAN_TRUE;
                } else {
                  value_release(tkind);
                  value_release(tkind_src);
                  value_release(tres);
                }
              }
              if (!emitted) {
                pfile_log(pf, PFILE_LOG_ERR,
                    "runtime-indexed access to a pointer array with"
                    " pseudo-bound elements is not supported yet"
                    " (use a const index)");
                value_release(n);
                if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE,
                    "]")) {
                  pf_token_get(pf, PF_TOKEN_NEXT);
                }
              }
              return BOOLEAN_TRUE;
            }
          }
        }

        dyn_string = jal_string_is_dynamic(val);
        pointer_callable_slot = BOOLEAN_FALSE;
        array_pointer_slot = BOOLEAN_FALSE;
        bit_array_slot = value_is_array(val)
          && variable_def_flag_test(value_def_get(val), VARIABLE_DEF_FLAG_BIT);
        bound = value_is_array(val)
          ? value_ct_get(val)
          : (variable_const_t) -1;
        if (((variable_const_t) -1) == bound && value_is_pointer(val)) {
          variable_def_t        pto;

          pto = jal_pointer_pointee_def_get(val);
          if (pto && VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(pto)) {
            variable_def_member_t am;

            am = variable_def_member_get(pto);
            if (am && !variable_def_member_link_get(am)) {
              bound = variable_def_member_ct_get(am);
            }
          }
        }
        value_use_ct_bump(val, CTR_BUMP_INCR);
        tmp = value_clone(val);
        if (value_is_array(tmp)) {
          variable_def_member_t am;

          am = variable_def_member_get(value_def_get(tmp));
          array_pointer_slot = am
            && variable_def_member_def_get(am)
            && (VARIABLE_DEF_TYPE_POINTER
                == variable_def_type_get(variable_def_member_def_get(am)));
        }
        if (value_is_pointer(tmp)) {
          variable_def_type_t   after_type;
          variable_def_t        pointee_saved;

          pointee_saved = jal_pointer_pointee_def_get(tmp);
          pointer_callable_slot = pointee_saved
            && (VARIABLE_DEF_TYPE_FUNCTION == variable_def_type_get(pointee_saved));

          value_dereference(tmp);
          after_type = value_type_get(tmp);
          if (VARIABLE_DEF_TYPE_ARRAY == after_type && pointee_saved
              && VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(pointee_saved)) {
            variable_def_member_t am;

            am = variable_def_member_get(value_def_get(tmp));
            if (am && !variable_def_member_link_get(am)) {
              value_def_set(tmp, variable_def_member_def_get(am));
              after_type = value_type_get(tmp);
            }
          }
          if (after_type == VARIABLE_DEF_TYPE_NONE) {
            /* Dereference left no element type (e.g. missing member). Build a
             * temp from the pointer's pointee so const/volatile/size match and
             * const stores are still rejected (see fix_pointer_const_pointee_write.md). */
            variable_def_t elem_def;

            value_release(tmp);
            elem_def = VARIABLE_DEF_NONE;
            if (pointee_saved
                && VARIABLE_DEF_TYPE_NONE != variable_def_type_get(pointee_saved)) {
              elem_def = variable_def_dup(pointee_saved);
            }
            if (!elem_def) {
              elem_def = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
                  VARIABLE_DEF_FLAG_NONE, 1);
            }
            tmp = pfile_value_temp_get_from_def(pf, elem_def);
            if (tmp) {
              value_flag_set(tmp, VALUE_FLAG_POINTER);
              value_indirect_set(tmp);
            }
          }
        } else {
          value_dereference(tmp);
        }
        value_release(val);
        val = tmp;

        if (value_is_const(n)) {
          variable_const_t nc;

          nc = value_const_get(n);
          if ((nc >= bound) && !dyn_string) {
            /* heap strings: no static bound (the handle's 3-byte array
             * count is NOT the text capacity); unchecked like open
             * arrays -- str[i] reads/writes the i-th character */
            pfile_log(pf, PFILE_LOG_ERR, "subscript out of range");
          } else if (bit_array_slot) {
            /* bit-array subscript: constant offsets accumulate in the
             * bit-offset delta (bit units), scaled by the sub-array's
             * size for inner dimensions of a multi-dimensional grid.
             * The delta composes with later dimensions and with the
             * runtime-index baseofs on every back end; a const baseofs
             * is deliberately NOT used (the classic operand folder
             * would read it as a byte displacement). */
            variable_t bv;
            unsigned   ofs;

            bv = value_variable_get(val);
            ofs = value_bit_offset_get(val)
                - (bv ? variable_bit_offset_get(bv) : 0);
            if (value_is_array(val)) {
              ofs += (unsigned) (nc * (variable_const_t) value_sz_get(val));
            } else {
              /* 1D bit array: the element's bit width scales the const
               * subscript exactly like the runtime one (bit*N elements
               * start at bit idx*N; unscaled, const and runtime paths
               * disagreed -- the 2026-10-27 defect's const half). val
               * here already carries the ELEMENT def (value_sz_get is
               * the width in bits, the same source the runtime branch
               * uses) */
              ofs += (unsigned) (nc * (variable_const_t)
                value_sz_get(val));
            }
            value_bit_offset_delta_set(val, ofs);
          } else {
            nc = nc * ((pointer_callable_slot || array_pointer_slot)
              ? pfile_pointer_width_get(pf)
              : value_sz_get(val));
            value_release(n);
            n = pfile_constant_get(pf, nc, VARIABLE_DEF_NONE);
            jal_val_baseofs_add(pf, val, n);
          }
        } else {
          if (variable_def_flag_test(value_def_get(val),
               VARIABLE_DEF_FLAG_UNIVERSAL)) {
            pfile_log(pf, PFILE_LOG_ERR,
                "universal typed arrays require a constant subscript");
          } else {
            if (bit_array_slot && !value_is_array(val)) {
              /* 1D bit array: the index IS the bit offset -- for a
               * `var bit` element (width 1). A `bit*N` element (width
               * N, N > 1) starts at bit offset idx*N, so the runtime
               * index must be scaled exactly like the const branch;
               * unscaled, element k addressed bit k of the wrong byte
               * (the 2026-10-27 bit*N runtime-write defect) */
              variable_sz_t esz;

              /* bit*N elements scale the index by the width IN BITS;
               * the product must not wrap modulo 256 (the same
               * promotion the multi-dimensional branch applies) or
               * every write past bit offset 255 lands in byte 0 --
               * found by the bit*N element-matrix chase (2026-10-27) */
              esz = value_sz_get(val);
              if (1 == esz) {
                tmp = n;
                value_lock(tmp);
              } else {
                value_t          bmul;
                variable_def_t   odef;

                odef = VARIABLE_DEF_NONE;
                if ((((variable_const_t) -1) != bound) && (esz > 1)) {
                  variable_const_t moff;

                  moff = (bound - 1) * ((variable_const_t) esz);
                  if (moff > 255) {
                    variable_sz_t needs;

                    needs = (moff > 0xFFFFFFFFUL) ? 8
                      : ((moff > 0xFFFFUL) ? 4 : 2);
                    if (needs > variable_def_sz_get(value_def_get(n))) {
                      odef = variable_def_alloc(0,
                          VARIABLE_DEF_TYPE_INTEGER,
                          VARIABLE_DEF_FLAG_NONE, needs);
                    }
                  }
                }
                bmul = pfile_constant_get(pf, esz, odef);
                tmp = VALUE_NONE;
                if (bmul) {
                  pfile_cmd_op_add(pf, OPERATOR_MUL, &tmp, n, bmul);
                  value_release(bmul);
                }
              }
            } else {
              value_t          sz;
              variable_def_t   odef;
              variable_sz_t    esz;

              /* multi-dimensional bit arrays scale by the sub-array's
               * size, which value_sz_get reports in BITS for bit-flagged
               * defs; everything else scales by the element's byte size */
              esz = (bit_array_slot)
                ? value_sz_get(val)
                : ((pointer_callable_slot || array_pointer_slot)
                  ? (variable_sz_t) pfile_pointer_width_get(pf)
                  : value_sz_get(val));
              if (1 == esz) {
                tmp = n;
                value_lock(tmp);
              } else {
                /* the scaled offset (bound-1)*esz must fit the offset
                 * expression: with a byte index the MUL result would
                 * otherwise take the byte def (operator promotion) and
                 * wrap modulo 256, mis-addressing elements past flat
                 * offset 255 (multi-dimensional rows, word arrays) */
                odef = VARIABLE_DEF_NONE;
                if ((((variable_const_t) -1) != bound) && (esz > 1)) {
                  variable_const_t moff;

                  moff = (bound - 1) * ((variable_const_t) esz);
                  if (moff > 255) {
                    variable_sz_t needs;

                    needs = (moff > 0xFFFFFFFFUL) ? 8
                      : ((moff > 0xFFFFUL) ? 4 : 2);
                    if (needs > variable_def_sz_get(value_def_get(n))) {
                      odef = variable_def_alloc(0,
                          VARIABLE_DEF_TYPE_INTEGER,
                          VARIABLE_DEF_FLAG_NONE, needs);
                    }
                  }
                }
                sz = pfile_constant_get(pf, esz, odef);
                tmp = VALUE_NONE;
                pfile_cmd_op_add(pf, OPERATOR_MUL, &tmp, n, sz);
                value_release(sz);
              }
            }
            jal_val_baseofs_add(pf, val, tmp);
            value_release(tmp);
          }
        }
        value_release(n);
        *pval = val;
      }
      if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, "]")) {
        pf_token_get(pf, PF_TOKEN_NEXT);
      }
    }
    return rc;
}

boolean_t jal_parse_structure(pfile_t *pf, value_t *pval)
{
  boolean_t rc;
  value_t   val;

  rc  = BOOLEAN_FALSE;
  val = *pval;
  if (value_is_structure(val) 
    && pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, ".")) {
    const char            *ptr;
    variable_def_t         vdef;
    variable_def_member_t  mbr;
    size_t                 ofs;

    rc   = BOOLEAN_TRUE;
    vdef = value_def_get(val);
    ptr  = pf_token_get(pf, PF_TOKEN_NEXT);
    for (ofs = 0, mbr = variable_def_member_get(vdef);
         mbr && strcmp(variable_def_member_tag_get(mbr), ptr);
         ofs += variable_def_member_sz_get(mbr),
           mbr = variable_def_member_link_get(mbr))
      ; /* null body */
    if (!mbr) {
      pfile_log(pf, PFILE_LOG_ERR, "No member named \"%s\"",
        ptr);
    } else {
      value_t n;

      value_def_set(val, variable_def_member_def_get(mbr));
      n = pfile_constant_get(pf, ofs, VARIABLE_DEF_NONE);
      jal_val_baseofs_add(pf, val, n);
      value_release(n);
    }
    pf_token_get(pf, PF_TOKEN_NEXT);
  }
  return rc;
}

flag_t jal_parse_postfix(pfile_t *pf, value_t *pval,
    value_t opt_locked_jvals[JAL_VAL_TYPE_CT])
{
  flag_t flags;

  flags = 0;
  while (1) {
    if (jal_parse_structure(pf, pval)) {
      flags |= JAL_PARSE_POSTFIX_FLAG_STRUCTURE;
      continue;
    }
    if (jal_parse_subscript(pf, pval, opt_locked_jvals)) {
      flags |= JAL_PARSE_POSTFIX_FLAG_SUBSCRIPT;
      continue;
    }
    break;
  }
  return flags;
}

value_t jal_parse_value(pfile_t *pf, jal_parse_value_type_t type)
{
  const char    *ptr;
  value_t        val;
  variable_def_t cast_def;

  ptr = pf_token_get(pf, PF_TOKEN_CURRENT);
  if ((JAL_PARSE_VALUE_TYPE_RVALUE == type) && jal_string_builtin_is(ptr)) {
    return jal_string_builtin_parse_emit(pf, ptr);
  }
  cast_def = pfile_variable_def_find(pf, PFILE_LOG_NONE, ptr);
  /* Basetypes (byte, dword, …) live on the root procedure's block_root.
   * pfile_variable_def_find walks proc parents, but some nested contexts
   * can miss them; fall back to the global root block (matches where
   * jal_file_process registers basetypes). */
  if (!cast_def && pfile_proc_root_get(pf)) {
    cast_def = pfile_block_variable_def_find(
      pfile_proc_block_root_get(pfile_proc_root_get(pf)), ptr);
  }
  if (cast_def) {
    pf_token_get(pf, PF_TOKEN_NEXT);
    if ((VARIABLE_DEF_TYPE_INTEGER == variable_def_type_get(cast_def))
          && !variable_def_flag_test(cast_def, VARIABLE_DEF_FLAG_BIT)
          && (1 == variable_def_sz_get(cast_def))
          && pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "*")) {
      value_t cval;

      pf_token_get(pf, PF_TOKEN_NEXT);
      cval = jal_parse_expr(pf);
      if (!value_is_const(cval)) {
        pfile_log(pf, PFILE_LOG_ERR, "constant expression expected");
        value_release(cval);
      } else {
        cast_def = variable_def_alloc(0,
            variable_def_type_get(cast_def),
            variable_def_flags_get_all(cast_def),
            value_const_get(cval));
      }
      value_release(cval);
    }
    if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, "(")) {
      pf_token_get(pf, PF_TOKEN_NEXT);
    }
    val = jal_parse_expr(pf);
    if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, ")")) {
      pf_token_get(pf, PF_TOKEN_NEXT);
    }
    if (val) {
      boolean_t in_temp;

      /* numeric/bit/pointer casts must reject string operands: a heap
       * string's storage is its HANDLE (off/cap/len) and a fixed or
       * const string is a byte array, so re-typing either to a number
       * reads metadata or the first character -- silent, layout-
       * dependent garbage (byte(dyn) returned the block offset).
       * Exception: byte("A") / sbyte("A") -- a ONE-character const
       * literal to a 1-byte integer -- is the char-code idiom and
       * reads that character (same conversion as `b = "A"`). The text
       * casts (string/dec/hex/bin) route elsewhere. Read a character
       * with str[index]. */
      if (variable_def_flag_test(value_def_get(val),
            VARIABLE_DEF_FLAG_STRING)
          && (VARIABLE_DEF_TYPE_BOOLEAN == variable_def_type_get(cast_def)
            || VARIABLE_DEF_TYPE_INTEGER == variable_def_type_get(cast_def)
            || VARIABLE_DEF_TYPE_FLOAT == variable_def_type_get(cast_def)
            || VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(cast_def))
          && !(value_is_const(val)
              && (1 == value_ct_get(val))
              && (VARIABLE_DEF_TYPE_INTEGER == variable_def_type_get(cast_def))
              && !variable_def_flag_test(cast_def, VARIABLE_DEF_FLAG_BIT)
              && (1 == variable_def_sz_get(cast_def)))) {
        pfile_log(pf, PFILE_LOG_ERR,
            "cannot cast a string to a numeric type; read a character "
            "with str[index] (fixed and const strings) or "
            "string_to_array() (heap strings)");
        value_release(val);
        return VALUE_NONE;
      }
      if (value_is_float(val) 
        ^ (VARIABLE_DEF_TYPE_FLOAT == variable_def_type_get(cast_def))) {
        /* casting to/from float, must be done in a temp! */
        in_temp = BOOLEAN_TRUE;
      } else {
        variable_sz_t val_sz;
        variable_sz_t cast_sz;

        val_sz = value_sz_get(val);
        if (!value_is_bit(val)) {
          val_sz *= 8;
        }
        cast_sz = variable_def_sz_get(cast_def);
        if (!variable_def_flag_test(cast_def, VARIABLE_DEF_FLAG_BIT)) {
          cast_sz *= 8;
        }
        if ((val_sz >= cast_sz) || value_is_const(val)) {
          if (value_is_const(val)) {
            cast_def = variable_def_flags_change(cast_def, 
                          variable_def_flags_get_all(cast_def)
                          | VARIABLE_DEF_FLAG_CONST);
          }
          value_def_set(val, cast_def);
          in_temp = BOOLEAN_FALSE;
        } else {
          in_temp = BOOLEAN_TRUE;
        }
      }
      if (in_temp) {
        /* if we're casting to a larger value, we need to sock this
         * away into a temporary */
        value_t tmp;

        tmp = pfile_value_temp_get(pf, variable_def_type_get(cast_def),
            variable_def_sz_get(cast_def));
        value_def_set(tmp, cast_def);
        pfile_cmd_op_add_assign(pf, OPERATOR_ASSIGN, &tmp, val);
        value_release(val);
        val = tmp;
      }
    }
  } else {
    value_t   jvals[JAL_VAL_TYPE_CT];
    boolean_t jval_found;
    flag_t    parse_flags;
    char      hard_cast_nm[6];

    hard_cast_nm[0] = 0;
    parse_flags = JAL_PARSE_CALL_FLAG_NONE;
    jval_found = jal_value_find(pf, ptr, jvals);
    
    
    if (JAL_PARSE_VALUE_TYPE_RVALUE == type) {
      value_release(jvals[JAL_VAL_TYPE_PUT]);
      jvals[JAL_VAL_TYPE_PUT] = VALUE_NONE;
      value_release(jvals[JAL_VAL_TYPE_IPUT]);
      jvals[JAL_VAL_TYPE_IPUT] = VALUE_NONE;
    } else if (JAL_PARSE_VALUE_TYPE_LVALUE == type) {
      value_release(jvals[JAL_VAL_TYPE_GET]);
      jvals[JAL_VAL_TYPE_GET] = VALUE_NONE;
      value_release(jvals[JAL_VAL_TYPE_IGET]);
      jvals[JAL_VAL_TYPE_IGET] = VALUE_NONE;
    } else if (JAL_PARSE_VALUE_TYPE_LVALUE_FOR_POINTER_IS == type) {
      /* Do not strip GET/IGET — POINTER_ASSIGN RHS may be pseudo-only (no BASE). */
    }
    val = VALUE_NONE;
    if (jval_found) {
      /* If a local BASE shadows the same name as a pseudo-'get GET slot (e.g.
       * `var word servo_slave` inside `function servo_slave'get`), prefer BASE:
       * newer (deeper) block_no wins. Otherwise GET is chosen first and
       * `servo_slave` is treated as a zero-arg pseudo call ("Too few parameters"). */
      if (jvals[JAL_VAL_TYPE_GET] && jvals[JAL_VAL_TYPE_BASE]) {
        variable_t      vg;
        variable_t      vb;
        ulong           bg_no;
        ulong           bb_no;

        vg = value_variable_get(jvals[JAL_VAL_TYPE_GET]);
        vb = value_variable_get(jvals[JAL_VAL_TYPE_BASE]);
        /* Some candidates are synthesized and do not carry block user-data.
         * Treat missing metadata as block 0 so a real inner BASE can still
         * shadow an outer or synthetic GET. */
        bg_no = pfile_block_no_get((pfile_block_t *)
            ((vg) ? variable_user_data_get(vg) : 0));
        bb_no = pfile_block_no_get((pfile_block_t *)
            ((vb) ? variable_user_data_get(vb) : 0));
        if (bb_no > bg_no) {
          value_release(jvals[JAL_VAL_TYPE_GET]);
          jvals[JAL_VAL_TYPE_GET] = VALUE_NONE;
        }
      }
      if (jvals[JAL_VAL_TYPE_GET]
          && (JAL_PARSE_VALUE_TYPE_LVALUE_FOR_POINTER_IS != type)) {
        val = jvals[JAL_VAL_TYPE_GET];
        if (VARIABLE_DEF_TYPE_POINTER == value_type_get(val)) {
          value_dereference(val);
        }
        parse_flags = JAL_PARSE_CALL_FLAG_GET;
      } else if (jvals[JAL_VAL_TYPE_BASE]) {
        val = jvals[JAL_VAL_TYPE_BASE];
      } else if ((JAL_PARSE_VALUE_TYPE_LVALUE_FOR_POINTER_IS == type)
          && jvals[JAL_VAL_TYPE_GET]) {
        /* `p is pv` where `pv` is pseudo: use GET slot for address-of, no call here. */
        val = jvals[JAL_VAL_TYPE_GET];
        if (VARIABLE_DEF_TYPE_POINTER == value_type_get(val)) {
          value_dereference(val);
        }
      } else if (jvals[JAL_VAL_TYPE_PUT]) {
        pfile_log(pf, PFILE_LOG_ERR, 
            "Attempted read from an OUT volatile parameter (%s)", ptr);
      }
      /* `p` bound to an INDEXED pseudo (`p is parr`, pointer-to-array):
       * route the rvalue to the linked 'get BEFORE postfix so `p[i]`
       * flows through the call machinery exactly like direct `parr[i]`
       * (const and runtime i, EOF tails, operator contexts -- shared).
       * Scalar pseudo links keep the post-postfix swap below. */
      if ((JAL_PARSE_VALUE_TYPE_RVALUE == type) && val
          && (VARIABLE_DEF_TYPE_POINTER == value_type_get(val))
          && !jal_value_points_to_callable_function(val)
          && !pfile_parse_call_data_ptr_arg(pf)
          && !pfile_parse_count_operand(pf)) {
        value_t igetv;

        igetv = jal_pointer_link_indexed_get_get(pf, value_variable_get(val));
        if (igetv) {
          /* take val's reference from jvals before replacing it (the
           * value_lock that would balance a plain release comes later) */
          if (jvals[JAL_VAL_TYPE_BASE] == val) {
            jvals[JAL_VAL_TYPE_BASE] = VALUE_NONE;
          }
          value_release(val);
          val         = igetv;
          parse_flags = JAL_PARSE_CALL_FLAG_GET;
        }
      }
    }
    if (val) {
      boolean_t want_call;

      value_lock(val);
      /* capture the identifier before the token buffer is overwritten
       * by pf_token_get below (hard-reserved cast names are <= 5 bytes) */
      if (jal_string_cast_hard_is(ptr)) {
        strcpy(hard_cast_nm, ptr);
      }
      pf_token_get(pf, PF_TOKEN_NEXT);
      /* hard-reserved text casts (2026-10-06): the `hex(` / `bin(` call
       * shape is ALWAYS the cast, even when a symbol of that name
       * resolves. Undefined names reach the cast through the
       * not-found hook below; this site defeats the one remaining
       * conflicting shape -- a user-defined function of that name being
       * called. Bare uses keep symbol semantics (library formals named
       * `bin`). The call is parsed anyway so tokens stay in sync. */
      if (hard_cast_nm[0]
          && (JAL_PARSE_VALUE_TYPE_RVALUE == type)
          && pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "(")) {
        pfile_log(pf, PFILE_LOG_ERR,
            "'%s' is reserved for the text cast"
            " (a called name is always the cast)", hard_cast_nm);
      }
      /* Match **`jal_parse_assign`**: after the identifier, allow **`.member`** and **`[idx]`**
       * so **`p is arr[1]`** (POINTER_ASSIGN RHS) parses as an lvalue, not leaving **`[`** at
       * top level (would become "unexpected token" at EOF). */
      (void) jal_parse_postfix(pf, &val, jvals);
      /* RVALUE heap-string subscript: str[i] must read the i-th
       * CHARACTER, not the handle's storage (off/cap/len) -- the normal
       * element view on a heap string would do exactly that. Swap the
       * view for the emitted payload load so the byte composes with
       * everything downstream (==, chains, call arguments). */
      if ((JAL_PARSE_VALUE_TYPE_RVALUE == type) && val
          && jal_string_subscript_value_is(val)) {
        value_t byte_val;

        byte_val = jal_string_subscript_read_emit(pf, val);
        if (byte_val) {
          value_release(val);
          val = byte_val;
        }
      }
      /* RVALUE: data pointer without a pseudo-GET reads through the slot (like *p in C).
       * Apply after `[` / `.` postfix so `p[i]` still parses on pointer-to-array. */
      if ((JAL_PARSE_VALUE_TYPE_RVALUE == type) && val
          && (VARIABLE_DEF_TYPE_POINTER == value_type_get(val))
          && !jal_value_points_to_callable_function(val)) {
        if (!pfile_parse_call_data_ptr_arg(pf)) {
          variable_t    rpv;
          variable_const_t rofs;

          /* through-read (`x = p`, `x = p[i]`, `x = slot[j]`): a USE of
           * the slot -- the must-bind check must not be silenced by the
           * assign bump store forms incur; const element offsets feed
           * the pointer-slot-array element check */
          rpv = value_variable_get(val);
          if (rpv) {
            variable_use_ct_bump(rpv, CTR_BUMP_INCR);
            if (jal_value_const_baseofs_get(val, &rofs)) {
              variable_pointer_slot_use_note(rpv, rofs);
            }
          }
          value_dereference(val);
        }
      }
      /* `p is sym` (data pointer): if `sym` has `'get`, scalar rvalue `p` uses GET like `sym`. */
      if ((JAL_PARSE_VALUE_TYPE_RVALUE == type) && val
          && value_is_indirect(val)
          && value_flag_test(val, VALUE_FLAG_POINTER)
          && !jal_value_points_to_callable_function(val)
          && !pfile_parse_call_data_ptr_arg(pf)
          && !value_is_array(val)) {
        variable_t    pv;
        const char   *nm;

        pv = value_variable_get(val);
        nm = (pv) ? variable_pointer_rvalue_link_name_get(pv) : 0;
        if (nm && nm[0]) {
          value_t jv[JAL_VAL_TYPE_CT];

          if (jal_value_find_by_name(pf, nm, jv) && jv[JAL_VAL_TYPE_GET]) {
            value_t getv;

            getv = jv[JAL_VAL_TYPE_GET];
            jv[JAL_VAL_TYPE_GET] = VALUE_NONE;
            jal_value_release(jv);
            value_release(val);
            val         = getv;
            parse_flags = JAL_PARSE_CALL_FLAG_GET;
            if (VARIABLE_DEF_TYPE_POINTER == value_type_get(val)) {
              value_dereference(val);
            }
          } else {
            jal_value_release(jv);
          }
        }
      }
      /* Functions used as rvalues should be called implicitly, matching original compiler behavior.
       * Procedure/function pointers: only treat as a call when `(` follows (e.g. fp(42)). */
      want_call = (parse_flags & JAL_PARSE_CALL_FLAG_GET)
          || ((JAL_PARSE_VALUE_TYPE_LVALUE_FOR_POINTER_IS != type)
              && (VARIABLE_DEF_TYPE_FUNCTION == value_type_get(val)))
          || ((JAL_PARSE_VALUE_TYPE_RVALUE == type)
              && jal_value_points_to_callable_function(val)
              && pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "("));
      if (want_call) {
        value_t tval;

        tval = jal_parse_call(pf, val, parse_flags);
        value_release(val);
        val = tval;
        if (!tval) {
          pfile_log(pf, PFILE_LOG_ERR, "no return value");
        }
      }
    }
    jal_value_release(jvals);
  }
  if (!val
    && (JAL_PARSE_VALUE_TYPE_RVALUE == type)
    && jal_string_cast_is(ptr)) {
    /* a text-cast name that resolved to no symbol: the identifier
     * token is still current (nothing was consumed on the not-found
     * path), so the cast shape `name(` is the one remaining meaning */
    val = jal_string_cast_parse_emit(pf);
  }
  if (val) {
    jal_string_error_rvalue_consumed(pf,
        (JAL_PARSE_VALUE_TYPE_RVALUE == type) ? 1 : 0, &val);
  }
  return val;
}

static value_t value_get_length_common(pfile_t *pf, boolean_t allow_scalar)
{
  value_t val;

  val = VALUE_NONE;
  if (pf_token_is(pf, PF_TOKEN_NEXT, PFILE_LOG_ERR, "(")) {
    value_t tmp;

    pf_token_get(pf, PF_TOKEN_NEXT);
    pfile_parse_count_operand_push(pf);
    tmp = jal_parse_expr(pf);
    pfile_parse_count_operand_pop(pf);
    if (VALUE_NONE != tmp) {
      if (jal_pointer_var_is_pseudo_linked(pf, value_variable_get(tmp))) {
        /* the pseudo array has no bound; the pointee [N] would lie */
        pfile_log(pf, PFILE_LOG_ERR,
            "COUNT() of a pointer bound to a pseudo array is not known");
        value_release(tmp);
        tmp = VALUE_NONE;
      } else if (jal_string_is_dynamic(tmp)) {
        val = jal_string_length_value_get(pf, tmp);
      } else if (value_is_array(tmp)) {
        val = pfile_constant_get(pf,
            variable_def_member_ct_get(
              variable_def_member_get(
                value_def_get(
                  tmp))),
            VARIABLE_DEF_NONE);
      } else if (value_is_pointer(tmp)
          || (value_flag_test(tmp, VALUE_FLAG_POINTER)
              && value_is_indirect(tmp))) {
        const char *vname;
        char       *ct_name;

        /* `jal_parse_value`(RVALUE) derefs data pointers to the pointee type;
         * COUNT still needs the backing `_name_count` slot. */
        vname = value_name_get(tmp);
        ct_name = MALLOC(8 + strlen(vname));
        if (!ct_name) {
          pfile_log_syserr(pf, ENOMEM);
        } else {
          sprintf(ct_name, "_%s_count", vname);
          val = pfile_value_find(pf, PFILE_LOG_NONE, ct_name);
          if (VALUE_NONE == val) {
            val = jal_value_find_open_array_count_param(pf, ct_name);
          }
          FREE(ct_name);
        }
      }
      if ((VALUE_NONE == val) && allow_scalar) {
        val = pfile_constant_get(pf, value_byte_sz_get(tmp),
            VARIABLE_DEF_NONE);
      }
      if (VALUE_NONE == val) {
        pfile_log(pf, PFILE_LOG_ERR, "\"%s\" is not an array",
            value_name_get(tmp));
      }
      value_release(tmp);
    }
    if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, ")")) {
      pf_token_get(pf, PF_TOKEN_NEXT);
    }
  }
  return val;
}

/* COUNT(...) — canonical length / element-count operator (same rules as _LENGTH). */
static value_t value_get_count(pfile_t *pf)
{
  return value_get_length_common(pf, BOOLEAN_TRUE);
}

/* _LENGTH(...) — legacy spelling of COUNT() */
static value_t value_get_length(pfile_t *pf)
{
  return value_get_length_common(pf, BOOLEAN_TRUE);
}

static value_t value_get_whereis(pfile_t *pf)
{
  value_t val;

  val = VALUE_NONE;
  if (pf_token_is(pf, PF_TOKEN_NEXT, PFILE_LOG_ERR, "(")) {
    pf_token_get(pf, PF_TOKEN_NEXT);
  }
  if (!jal_token_is_identifier(pf)) {
    pfile_log(pf, PFILE_LOG_ERR, "identifier expected");
  } else {
    const char *ptr;
    label_t     lbl;
    boolean_t   direct_result;
    boolean_t   root_pointer_slot;
    boolean_t   root_const_scalar;

    ptr = pf_token_get(pf, PF_TOKEN_CURRENT);
    lbl = pfile_label_find(pf, PFILE_LOG_NONE, ptr);
    direct_result = BOOLEAN_FALSE;
    root_pointer_slot = BOOLEAN_FALSE;
    root_const_scalar = BOOLEAN_FALSE;
    if (lbl) {
      pf_token_get(pf, PF_TOKEN_NEXT);
    } else {
      variable_t root_var;

      root_var = pfile_variable_find(pf, PFILE_LOG_NONE, ptr, 0);
      if (root_var) {
        root_pointer_slot = variable_is_pointer(root_var)
          || jal_variable_is_array_of_pointers(root_var)
          || jal_variable_tree_contains_pointer(root_var);
        root_const_scalar = variable_is_const(root_var)
          && !variable_is_array(root_var);
        variable_release(root_var);
      }
      val = jal_parse_value(pf, JAL_PARSE_VALUE_TYPE_LVALUE_FOR_POINTER_IS);
      if (val) {
        variable_t ptr_var;

        ptr_var = value_variable_get(val);
        if (root_const_scalar && value_is_const(val)
            && !value_baseofs_get(val) && !value_is_array(val)) {
          pfile_log(pf, PFILE_LOG_ERR, "Cannot use whereis() on a constant");
          value_release(val);
          val = VALUE_NONE;
        } else if (root_pointer_slot
            || (ptr_var
              && (variable_is_pointer(ptr_var)
                || jal_variable_is_array_of_pointers(ptr_var)
                || jal_variable_tree_contains_pointer(ptr_var)
                || value_is_pointer(val)
                || value_flag_test(val, VALUE_FLAG_POINTER)))) {
          value_t target;
          variable_const_t slot_ofs;

          target = VALUE_NONE;
          if (jal_value_const_baseofs_get(val, &slot_ofs)) {
            if (ptr_var && variable_is_pointer(ptr_var)) {
              target = variable_pointer_whereis_target_get(ptr_var);
            }
            if (!target && ptr_var) {
              target = variable_pointer_whereis_slot_target_get(ptr_var,
                  slot_ofs);
            }
          } else {
            value_t direct;

            direct = value_clone(val);
            if (direct) {
              variable_def_t def;

              def = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
                  VARIABLE_DEF_FLAG_NONE, 2);
              value_def_set(direct, def);
              value_indirect_clear(direct);
              value_flag_clr(direct, VALUE_FLAG_POINTER);
              value_flag_set(direct, VALUE_FLAG_PTR_SLOT_RAW);
              value_release(val);
              val = direct;
              direct_result = BOOLEAN_TRUE;
            }
          }
          if (target) {
            pfile_proc_t *proc;

            value_release(val);
            proc = value_is_function(target) ? value_proc_get(target)
              : PFILE_PROC_NONE;
            if (proc) {
              lbl = pfile_proc_label_get(proc);
              label_lock(lbl);
              val = VALUE_NONE;
            } else {
              val = value_clone(target);
            }
          } else if (!direct_result) {
            value_release(val);
            val = pfile_constant_get(pf, 0, VARIABLE_DEF_NONE);
            direct_result = BOOLEAN_TRUE;
          }
        }
      } else {
        pfile_log(pf, PFILE_LOG_ERR, "%s not found", ptr);
      }
    }
    if (lbl || (val && !direct_result)) {
      variable_def_t def;
      variable_t     var;

      def = variable_def_alloc(0, (lbl) ? VARIABLE_DEF_TYPE_LABEL
          : VARIABLE_DEF_TYPE_VALUE, VARIABLE_DEF_FLAG_VOLATILE, 2);
      var = variable_alloc(TAG_NONE, def);
      if (lbl) {
        variable_label_set(var, lbl);
      } else {
        variable_value_set(var, val);
      }
      value_release(val);
      val = value_alloc(var);
      variable_release(var);
      label_release(lbl);
    }
  }
  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, ")")) {
    pf_token_get(pf, PF_TOKEN_NEXT);
  }
  return val;
}

static value_t value_get_defined(pfile_t *pf)
{
  value_t val;

  val = VALUE_NONE;
  if (pf_token_is(pf, PF_TOKEN_NEXT, PFILE_LOG_ERR, "(")) {
    pf_token_get(pf, PF_TOKEN_NEXT);
  }
  if (!jal_token_is_identifier(pf)) {
    pfile_log(pf, PFILE_LOG_ERR, "identifier expected");
  } else {
    const char      *ptr;
    label_t          lbl;
    value_t          tst_val;
    variable_def_t   vdef;

    vdef    = variable_def_alloc(0, VARIABLE_DEF_TYPE_BOOLEAN, 0, 1);
    ptr     = pf_token_get(pf, PF_TOKEN_CURRENT);
    lbl     = pfile_label_find(pf, PFILE_LOG_NONE, ptr);
    tst_val = pfile_value_find(pf, PFILE_LOG_NONE, ptr);
    /* Must test before release: tst_val/lbl handles are invalid after release. */
    val = pfile_constant_get(pf,
        (lbl != LABEL_NONE) || (tst_val != VALUE_NONE),
        vdef);
    value_release(tst_val);
    label_release(lbl);
    pf_token_get(pf, PF_TOKEN_NEXT);
  }
  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, ")")) {
    pf_token_get(pf, PF_TOKEN_NEXT);
  }
  return val;
}

/*
 * NAME
 *   value_get
 *
 * DESCRIPTION
 *   get an expression's value
 *
 * PARAMETERS
 *   pf    : return from pfile_open()
 *   var   : [out] holds result
 *
 * RETURN
 *
 * NOTES
 */
static value_t value_get(pfile_t *pf)
{
  value_t     val;

  /* valid values: 
       constant ('%' or '$' or decimal #)
       variable name (alpha + alpha|digit|'_')
       '(' -- subfunction
       '!' -- logical NOT
       '-' -- negation (two's complement)
       '~' -- complement (one's complement) */
  val = VALUE_NONE;
  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "count")) {
    val = value_get_count(pf);
  } else if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "_length")) {
    val = value_get_length(pf);
  } else if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "whereis")) {
    val = value_get_whereis(pf);
  } else if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "defined")) {
    val = value_get_defined(pf);
  } else {
    const char *ptr;
    operator_t  op;

    ptr = pf_token_get(pf, PF_TOKEN_CURRENT);
    op = OPERATOR_NULL;
    switch (*ptr) {
      case '!':
        op = ('!' == ptr[1]) ? OPERATOR_LOGICAL : OPERATOR_CMPB;
        /* fall through */
      case '-':
      case '+':
        if ('-' == *ptr) {
          op = OPERATOR_NEG;
        }
        pf_token_get(pf, PF_TOKEN_NEXT); /* this one's done */
        val = value_get(pf);   /* var is the next value */
        /* create the expression:
         *   _t# = var op
         *   and return _t#
         */
        if (val && (OPERATOR_NULL != op)) {
          value_t tmp;

          if ((OPERATOR_NEG == op) && value_is_const(val)) {
            if (value_is_float(val)) {
              tmp = pfile_constant_float_get(pf,
                -value_const_float_get(val), VARIABLE_DEF_NONE);
            } else {
              variable_def_t      def;
              variable_sz_t       sz;
              variable_def_type_t type;
              flag_t              flags;
              variable_const_t    n;

              n = -value_const_get(val);

              if (value_name_get(val)) {
                sz    = value_sz_get(val);
                type  = VARIABLE_DEF_TYPE_INTEGER;
                flags = VARIABLE_DEF_FLAG_SIGNED;
              } else {
                variable_calc_sz_min(n, &sz, &type, &flags);
              }
              if (value_is_universal(val)) {
                flags |= VARIABLE_DEF_FLAG_UNIVERSAL;
              }
              def = variable_def_alloc(0, type, flags, sz);
              tmp = pfile_constant_get(pf, n, def);
            }
            value_release(val);
            val = tmp;
          } else {
            tmp = VALUE_NONE;
            pfile_cmd_op_add(pf, op, &tmp, val, 0);
            value_release(val);
            val = tmp;
          }
        }
        break;
      case '(':
        pf_token_get(pf, PF_TOKEN_NEXT);
        val = jal_parse_expr(pf);
        /* prepend the existing result to the new result */
        if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, ")")) {
          pf_token_get(pf, PF_TOKEN_NEXT);
        }
        break;
      default:
        if (jal_token_is_identifier(pf)) {
          val = jal_parse_value(pf, JAL_PARSE_VALUE_TYPE_RVALUE);
        } else {
          val = jal_token_to_constant(pf, PFILE_LOG_ERR);
          if (val) {
            pf_token_get(pf, PF_TOKEN_NEXT);
          }
        }
        break;
    }
  }
  (void) jal_parse_postfix(pf, &val, NULL);
  return val;
}

/*
 * NAME
 *   operator_get
 *
 * DESCRIPTION
 *   get an operator
 *
 * PARAMETERS
 *   expr : expression
 *   pf   : returned by pfile_open()
 *
 * RETURN
 *   0      : no error
 *   EINVAL : no operator present
 *
 * NOTES
 */
static operator_t operator_get(pfile_t *pf)
{
  static const struct {
    const char *token;
    operator_t     op;
  } operator_token_map[] = {
    {"*",    OPERATOR_MUL},
    {"/",    OPERATOR_DIV},
    {"%",    OPERATOR_MOD},
    {"+",    OPERATOR_ADD},
    {"-",    OPERATOR_SUB},
    {"<<",   OPERATOR_SHIFT_LEFT},
    {">>",   OPERATOR_SHIFT_RIGHT},
    {"<",    OPERATOR_LT},
    {"<=",   OPERATOR_LE},
    {"==",   OPERATOR_EQ},
    {"!=",   OPERATOR_NE},
    {">=",   OPERATOR_GE},
    {">",    OPERATOR_GT},
    {"&",    OPERATOR_ANDB},
    {"|",    OPERATOR_ORB},
    {"^",    OPERATOR_XORB},
    {"!!",   OPERATOR_LOGICAL},
    {".",    OPERATOR_DOT},
    {"[",    OPERATOR_SUBSCRIPT},
  };
  size_t ii;
  operator_t op;

  for (ii = 0; 
       (ii < COUNT(operator_token_map))
        && !pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE,
          operator_token_map[ii].token); 
        ii++)
    ;
  if (ii == COUNT(operator_token_map)) {
    op = OPERATOR_NULL;
  } else {
    op = operator_token_map[ii].op;
    pf_token_get(pf, PF_TOKEN_NEXT);
  }
  return op;
}

/*
 * NAME
 *   expr_parse
 *
 * DESCRIPTION
 *   parse an expression
 *
 * PARAMETERS
 *   dst   : [out] holds result
 *   pf    : parse file
 *   lhvar : for assignments, this is the base left-hand variable
 *
 * RETURN
 *
 * NOTES
 */
value_t jal_parse_expr(pfile_t *pf)
{
  operator_t  op;
  operator_t  prev_op;
  expr_t     *exprstk;
  value_t     val;

  exprstk  = 0;
  op       = OPERATOR_NULL;
  prev_op  = OPERATOR_NULL;
  do {
    val = value_get(pf);
    if (val
        && (OPERATOR_NULL == prev_op
          || !operator_is_relation(prev_op))
        && pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "&")
        && jal_string_is_source(val)) {
      /* String-typed `&` in expression position: evaluate the concat
       * chain into a heap cell (stage 1 of the generalized `&` plan);
       * the chain takes ownership of `val`.  Suppressed when this
       * primary is the RHS of a relation -- `c1 == "a" & c2 == "b"` is
       * the bitwise-AND-of-conditions idiom (JAL has no `and`). */
      value_t temp;

      temp = jal_string_concat_chain_emit(pf, val);
      val = temp;
    }
    if (val && (jal_string_is_dynamic(val)
          || (value_is_array(val) && value_is_string(val)))
        && (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "==")
          || pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "!="))) {
      operator_t string_op;
      value_t    rel;

      string_op = pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "==")
        ? OPERATOR_EQ : OPERATOR_NE;
      pf_token_get(pf, PF_TOKEN_NEXT);
      rel = jal_string_relation_parse_emit(pf, val, string_op);
      value_release(val);
      val = rel;
    }
    op = operator_get(pf);
    if (val) {
      pfile_expr_push(pf, &exprstk, val, op);
      value_release(val);
    }
    prev_op = op;
  } while (OPERATOR_NULL != op);
  if (exprstk) {
    val = expr_val_get(exprstk);
    value_lock(val);
    expr_list_free(&exprstk);
  }
  return val;
}

