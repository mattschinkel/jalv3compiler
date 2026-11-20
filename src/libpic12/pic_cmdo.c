/************************************************************
 **
 ** pic_cmdo.c : pic cmd optimization definitions
 **
 ** Copyright (c) 2005, Kyle A. York
 ** All rights reserved
 **
 ** Original Author: Kyle A. York
 ** Author: Matthew Schinkel, 2026
 ** Changes: minor fork edits
 **
 ************************************************************/
#include "../libcore/value.h"
#include "../libcore/cmd_op.h"
#include "../libcore/cmd_brch.h"
#include "../libcore/cmd_optm.h"
#include "../libcore/pfile.h"
#include "../libcore/pf_op.h"
#include "../libcore/byref_tag.h"
#include "../libcore/pf_proc.h"
#include "pic.h"
#include "pic_cmdo.h"


/* this occurs after all pfile optimization, but before variable
 * definition
 *
 * currently, it looks for
 *   tmp = expr
 *   branch condition tmp --> x
 * and removes tmp from (tmp = relational)
 */ 
/* The test/branch strip in pic_cmd_optimize relies on the stripped
 * op's STATUS FLAGS surviving to the branch.  A CMD_TYPE_LABEL between
 * the two (e.g. the mismatch/done join of jal_string's relation loop,
 * where `result = 0' sits on ONE incoming path only) means control can
 * reach the branch without executing the op -- the branch would test
 * stale flags (observed: `t == "lit"' inverted on classic cores). */
static boolean_t pic_cmd_label_between(cmd_t from, cmd_t to)
{
  cmd_t it;

  for (it = cmd_link_get(from); it && (it != to);
      it = cmd_link_get(it)) {
    if (CMD_TYPE_LABEL == cmd_type_get(it)) {
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

/* A relation with both operands compile-time constants (the T6 byref tag
 * fold is one producer).  Returns the folded truth value. */
static boolean_t pic_cmd_const_relation(pfile_t *pf, cmd_t cmd,
    boolean_t *pres)
{
  operator_t op;
  value_t    v1;
  value_t    v2;
  pf_const_t c;

  op = cmd_optype_get(cmd);
  if (!operator_is_relation(op)) {
    return BOOLEAN_FALSE;
  }
  v1 = cmd_opval1_get(cmd);
  v2 = cmd_opval2_get(cmd);
  if (!v1 || !v2 || !value_is_const(v1) || !value_is_const(v2)) {
    return BOOLEAN_FALSE;
  }
  c = pfile_op_const_exec(pf, op, v1, v2);
  if (PF_CONST_TYPE_INTEGER == c.type) {
    *pres = (0 != c.u.n) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
    return BOOLEAN_TRUE;
  }
  if (PF_CONST_TYPE_FLOAT == c.type) {
    *pres = (0.0 != c.u.f) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
    return BOOLEAN_TRUE;
  }
  return BOOLEAN_FALSE;
}

/* value equality for the fold's temporaries.  value_is_same() deliberately
 * treats a `_temp' operand as never equal to anything (pointer-comparison
 * caller rule), so it cannot see that `tmp = tag & mask' is read by the
 * following relation.  Compare the variable/def/bit instead; constants keep
 * the value_is_same semantics. */
static boolean_t pic_value_same(value_t a, value_t b)
{
  if (a == b) {
    return BOOLEAN_TRUE;
  }
  if (!a || !b) {
    return BOOLEAN_FALSE;
  }
  if (value_is_const(a) && value_is_const(b)) {
    return value_is_same(a, b);
  }
  if (!((value_variable_get(a) == value_variable_get(b))
      && value_def_is_same(a, b)
      && (value_bit_offset_get(a) == value_bit_offset_get(b)))) {
    return BOOLEAN_FALSE;
  }
  {
    value_t b1;
    value_t b2;

    b1 = value_baseofs_get(a);
    b2 = value_baseofs_get(b);
    if (b1 == b2) {
      return BOOLEAN_TRUE;
    }
    if (b1 && b2 && value_is_const(b1) && value_is_const(b2)
        && (value_const_get(b1) == value_const_get(b2))) {
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

/* is `val' read by any command other than skip1/skip2? */
static boolean_t pic_cmd_value_used(cmd_t head, value_t val,
    cmd_t skip1, cmd_t skip2)
{
  cmd_t    c;
  size_t   ii;
  size_t   ct;
  value_t *params;
  value_t  v;

  for (c = head; c; c = cmd_link_get(c)) {
    if ((c == skip1) || (c == skip2)) {
      continue;
    }
    v = cmd_opval1_get(c);
    if (v && pic_value_same(val, v)) {
      return BOOLEAN_TRUE;
    }
    v = cmd_opval2_get(c);
    if (v && pic_value_same(val, v)) {
      return BOOLEAN_TRUE;
    }
    v = cmd_brval_get(c);
    if (v && pic_value_same(val, v)) {
      return BOOLEAN_TRUE;
    }
    ct = cmd_brproc_param_ct_get(c);
    params = cmd_brproc_params_get(c);
    for (ii = 0; ii < ct; ii++) {
      if (params[ii] && pic_value_same(val, params[ii])) {
        return BOOLEAN_TRUE;
      }
    }
  }
  return BOOLEAN_FALSE;
}

/* Replace every read of `oldv' with `newv' (a locked constant).  A call
 * parameter has no setter here; such a use makes the caller leave the
 * defining command in place (correct, just not folded). */
static boolean_t pic_cmd_replace_uses(cmd_t head, value_t oldv, value_t newv,
    cmd_t skip)
{
  cmd_t     c;
  value_t   v;
  boolean_t changed;

  changed = BOOLEAN_FALSE;
  for (c = head; c; c = cmd_link_get(c)) {
    if (c == skip) {
      continue;
    }
    v = cmd_opval1_get(c);
    if (v && pic_value_same(oldv, v)) {
      cmd_opval1_set(c, newv);
      changed = BOOLEAN_TRUE;
    }
    v = cmd_opval2_get(c);
    if (v && pic_value_same(oldv, v)) {
      cmd_opval2_set(c, newv);
      changed = BOOLEAN_TRUE;
    }
    v = cmd_brval_get(c);
    if (v && pic_value_same(oldv, v)) {
      cmd_brval_set(c, newv);
      changed = BOOLEAN_TRUE;
    }
  }
  return changed;
}

/* Fold the masked predicate chain `tmp = tag & mask' + `dst = !tmp' (or
 * `dst = tmp == value' / `dst = !!tmp') when the tag fold has made both AND
 * operands constant.  The AND destination is a `_temp' whose slot is reused
 * by later statements, so it must NOT be replaced globally: compute the
 * predicate result, replace only the following consumer's BIT destination,
 * then remove the two commands.  This is also why the chain is matched by
 * the immediately following command rather than by a value scan. */
static boolean_t pic_cmd_fold_const_bits(pfile_t *pf, cmd_t *cmd_head)
{
  cmd_t cmd;

  for (cmd = cmd_next_exec_get(*cmd_head);
       cmd;
       cmd = cmd_next_exec_get(cmd_link_get(cmd))) {
    operator_t       op;
    value_t          v1;
    value_t          v2;
    value_t          dst;
    cmd_t            next;
    operator_t       nop;
    value_t          nv1;
    value_t          nv2;
    value_t          other;
    value_t          ndst;
    value_t          cv;
    variable_const_t n;
    variable_const_t e;

    op = cmd_optype_get(cmd);
    if (OPERATOR_ANDB != op) {
      continue;
    }
    v1 = cmd_opval1_get(cmd);
    v2 = cmd_opval2_get(cmd);
    if (!v1 || !v2 || !value_is_const(v1) || !value_is_const(v2)) {
      continue;
    }
    dst = cmd_opdst_get(cmd);
    if (!dst || !value_is_temp(dst)) {
      continue;
    }
    next = cmd_next_exec_get(cmd_link_get(cmd));
    if (!next) {
      continue;
    }
    nop  = cmd_optype_get(next);
    nv1  = cmd_opval1_get(next);
    nv2  = cmd_opval2_get(next);
    ndst = cmd_opdst_get(next);
    if (!ndst || !value_is_temp(ndst)) {
      continue;
    }
    if (pic_value_same(dst, nv1)) {
      other = nv2;
    } else if (pic_value_same(dst, nv2)) {
      other = nv1;
    } else {
      continue;
    }
    n = value_const_get(v1) & value_const_get(v2);
    if (OPERATOR_NOTL == nop) {
      e = n ? 0 : 1;
    } else if (OPERATOR_LOGICAL == nop) {
      e = n ? 1 : 0;
    } else if (operator_is_relation(nop) && other
        && value_is_const(other)) {
      value_t    nv;
      pf_const_t c;

      nv = pfile_constant_get(pf, n, value_def_get(dst));
      if (!nv) {
        continue;
      }
      c = pfile_op_const_exec(pf, nop, nv, other);
      value_release(nv);
      if (PF_CONST_TYPE_INTEGER == c.type) {
        e = c.u.n ? 1 : 0;
      } else if (PF_CONST_TYPE_FLOAT == c.type) {
        e = (0.0 != c.u.f) ? 1 : 0;
      } else {
        continue;
      }
    } else {
      continue;
    }
    cv = pfile_constant_get(pf, e, value_def_get(ndst));
    if (!cv) {
      continue;
    }
    if (pic_cmd_replace_uses(*cmd_head, ndst, cv, next)
        && !pic_cmd_value_used(*cmd_head, ndst, next, CMD_NONE)) {
      cmd_remove(cmd_head, next);
      cmd_remove(cmd_head, cmd);
      value_release(cv);
      return BOOLEAN_TRUE;
    }
    value_release(cv);
  }
  return BOOLEAN_FALSE;
}

/* Fold `const & bit' / `const | bit' feeding a branch.  A mask predicate is
 * reduced to a constant by the chain fold above; the AND/OR consumer then
 * has one constant and one runtime bit.  `1 & x' and `0 | x' retarget the
 * branch to x; the annihilator values fold it outright. */
static boolean_t pic_cmd_fold_const_logical_value(cmd_t *cmd_head)
{
  cmd_t use;

  for (use = cmd_next_exec_get(*cmd_head);
       use;
       use = cmd_next_exec_get(cmd_link_get(use))) {
    operator_t op;
    value_t    a;
    value_t    b;
    value_t    other;
    value_t    constv;
    value_t    dst_use;
    cmd_t      branch;
    boolean_t  truth;
    boolean_t  identity;

    op = cmd_optype_get(use);
    if ((OPERATOR_ANDB != op) && (OPERATOR_ORB != op)) {
      continue;
    }
    a = cmd_opval1_get(use);
    b = cmd_opval2_get(use);
    if (a && value_is_const(a) && b && !value_is_const(b)) {
      constv = a;
      other  = b;
    } else if (b && value_is_const(b) && a && !value_is_const(a)) {
      constv = b;
      other  = a;
    } else {
      continue;
    }
    if (!value_is_bit(other)) {
      continue;
    }
    dst_use = cmd_opdst_get(use);
    if (!dst_use || !value_is_bit(dst_use)) {
      continue;
    }
    branch = cmd_next_exec_get(cmd_link_get(use));
    if (!branch || (CMD_TYPE_BRANCH != cmd_type_get(branch))) {
      continue;
    }
    if (CMD_BRANCHCOND_NONE == cmd_brcond_get(branch)) {
      continue;
    }
    if (!value_is_same(dst_use, cmd_brval_get(branch))) {
      continue;
    }
    if (pic_cmd_label_between(use, branch)) {
      continue;
    }
    if (pic_cmd_value_used(*cmd_head, dst_use, use, branch)) {
      continue;
    }
    truth = value_const_get(constv) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
    identity = ((OPERATOR_ANDB == op) && truth)
      || ((OPERATOR_ORB == op) && !truth);
    if (identity) {
      cmd_brval_set(branch, other);
    } else {
      boolean_t taken;

      taken = ((CMD_BRANCHCOND_TRUE == cmd_brcond_get(branch)) && truth)
        || ((CMD_BRANCHCOND_FALSE == cmd_brcond_get(branch)) && !truth);
      if (taken) {
        cmd_brcond_set(branch, CMD_BRANCHCOND_NONE);
        cmd_brval_set(branch, VALUE_NONE);
      } else {
        cmd_remove(cmd_head, branch);
      }
    }
    cmd_remove(cmd_head, use);
    return BOOLEAN_TRUE;
  }
  return BOOLEAN_FALSE;
}

/* Constant bit propagation one level: a folded `_is_*' relation used as an
 * operand of `&'/`|' immediately feeding a branch.  `true & x' and
 * `false | x' retarget the branch to x; the annihilator values fold the
 * branch outright.  The caller iterates, so nested expressions reduce as
 * outer levels expose their operands. */
static boolean_t pic_cmd_fold_const_logical(pfile_t *pf, cmd_t *cmd_head)
{
  cmd_t rel;

  for (rel = cmd_next_exec_get(*cmd_head);
       rel;
       rel = cmd_next_exec_get(cmd_link_get(rel))) {
    boolean_t truth;
    value_t   d;
    cmd_t     use;

    if (!pic_cmd_const_relation(pf, rel, &truth)) {
      continue;
    }
    d = cmd_opdst_get(rel);
    if (!d || !value_is_temp(d)) {
      continue;
    }
    for (use = cmd_next_exec_get(*cmd_head);
         use;
         use = cmd_next_exec_get(cmd_link_get(use))) {
      operator_t op;
      value_t    a;
      value_t    b;
      value_t    other;
      value_t    dst_use;
      cmd_t      branch;
      boolean_t  identity;

      op = cmd_optype_get(use);
      if ((OPERATOR_ANDB != op) && (OPERATOR_ORB != op)) {
        continue;
      }
      a = cmd_opval1_get(use);
      b = cmd_opval2_get(use);
      if (a && value_is_same(a, d)) {
        other = b;
      } else if (b && value_is_same(b, d)) {
        other = a;
      } else {
        continue;
      }
      if (!other) {
        continue;
      }
      if (!value_is_bit(other) && !value_is_const(other)) {
        continue;
      }
      dst_use = cmd_opdst_get(use);
      if (!dst_use || !value_is_bit(dst_use)) {
        continue;
      }
      branch = cmd_next_exec_get(cmd_link_get(use));
      if (!branch || (CMD_TYPE_BRANCH != cmd_type_get(branch))) {
        continue;
      }
      if (CMD_BRANCHCOND_NONE == cmd_brcond_get(branch)) {
        continue;
      }
      if (!value_is_same(dst_use, cmd_brval_get(branch))) {
        continue;
      }
      if (pic_cmd_label_between(use, branch)) {
        continue;
      }
      if (pic_cmd_value_used(*cmd_head, dst_use, use, branch)) {
        continue;
      }
      identity = ((OPERATOR_ANDB == op) && truth)
        || ((OPERATOR_ORB == op) && !truth);
      if (identity) {
        cmd_brval_set(branch, other);
      } else {
        boolean_t taken;

        taken = ((CMD_BRANCHCOND_TRUE == cmd_brcond_get(branch)) && truth)
          || ((CMD_BRANCHCOND_FALSE == cmd_brcond_get(branch)) && !truth);
        if (taken) {
          cmd_brcond_set(branch, CMD_BRANCHCOND_NONE);
          cmd_brval_set(branch, VALUE_NONE);
        } else {
          cmd_remove(cmd_head, branch);
        }
      }
      cmd_remove(cmd_head, use);
      if (!pic_cmd_value_used(*cmd_head, d, rel, CMD_NONE)) {
        cmd_remove(cmd_head, rel);
      }
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

void pic_cmd_optimize(pfile_t *pf, cmd_t *cmd_head)
{
  cmd_t         cmd;
  pfile_proc_t *proc;

  UNUSED(pf);

  /* T6: single-type byref tag fold -- replace a `_<formal>_tag == N`
   * relation with `K == N` (both constants) when every call site passed
   * the same tag, so the back end folds it and the tag member drops. */
  cmd = cmd_next_exec_get(*cmd_head);
  while (cmd) {
    if ((OPERATOR_EQ == cmd_optype_get(cmd))
        || (OPERATOR_ANDB == cmd_optype_get(cmd))) {
      value_t op1 = cmd_opval1_get(cmd);
      value_t op2 = cmd_opval2_get(cmd);
      int     k;

      if (op1 && op2 && value_is_const(op2)) {
        k = variable_byref_tag_const_get(value_variable_get(op1));
        if (k >= 0) {
          value_t cv = pfile_constant_get(pf, (variable_const_t) k,
              value_def_get(op1));
          if (cv) {
            cmd_opval1_set(cmd, cv);
            value_release(cv);
          }
        }
      } else if (op1 && op2 && value_is_const(op1)) {
        k = variable_byref_tag_const_get(value_variable_get(op2));
        if (k >= 0) {
          value_t cv = pfile_constant_get(pf, (variable_const_t) k,
              value_def_get(op2));
          if (cv) {
            cmd_opval2_set(cmd, cv);
            value_release(cv);
          }
        }
      }
    }
    cmd = cmd_next_exec_get(cmd_link_get(cmd));
  }
  /* The tag fold above can leave `K == N` relations; let the generic
   * constant-branch pass turn each into an unconditional/deleted branch and
   * drop the dead operator.  The bit-level pass first propagates a folded
   * `_is_*` through a `&'/`|' consumer (e.g. `_is_array() & cond`), which
   * the direct relation+branch pass cannot see.  The strip below is the
   * fallback for any case neither can take. */
  {
    unsigned opt_pass;

    opt_pass = 0;
    while ((opt_pass < 64) && pic_cmd_fold_const_bits(pf, cmd_head)) {
      opt_pass++;
    }
    opt_pass = 0;
    while ((opt_pass < 64)
        && pic_cmd_fold_const_logical_value(cmd_head)) {
      opt_pass++;
    }
    opt_pass = 0;
    while ((opt_pass < 64) && pic_cmd_fold_const_logical(pf, cmd_head)) {
      opt_pass++;
    }
    opt_pass = 0;
    while ((opt_pass < 32) && cmd_opt_branch4(pf, cmd_head)) {
      opt_pass++;
    }
  }

  cmd = cmd_next_exec_get(*cmd_head);
  while (cmd) {
    cmd_t     cmd_next;
    boolean_t rel_const;

    cmd_next = cmd_next_exec_get(cmd_link_get(cmd));
    /* A relation whose two operands have become constants (the T6 byref
     * tag fold above is one producer) still needs its destination: the
     * back end folds it to a constant bit store, and the branch must test
     * THAT bit.  Stripping the destination here turns the branch into a
     * STATUS<Z> test with no surviving compare -- a stale-flag read. */
    rel_const = BOOLEAN_FALSE;
    if (operator_is_relation(cmd_optype_get(cmd))
        && value_is_const(cmd_opval1_get(cmd))
        && value_is_const(cmd_opval2_get(cmd))) {
      rel_const = BOOLEAN_TRUE;
    }
    if ((operator_is_relation(cmd_optype_get(cmd))
          || (OPERATOR_ASSIGN == cmd_optype_get(cmd))
          || (OPERATOR_LOGICAL == cmd_optype_get(cmd))
          || (OPERATOR_NOTL == cmd_optype_get(cmd))
          || (OPERATOR_CMPB == cmd_optype_get(cmd)))
        && value_is_temp(cmd_opdst_get(cmd))
        && value_is_same(cmd_opdst_get(cmd), cmd_brval_get(cmd_next))
        && (CMD_BRANCHCOND_NONE != cmd_brcond_get(cmd_next))
        && !pic_cmd_label_between(cmd, cmd_next)
        && !rel_const) {
      cmd_opdst_set(cmd, VALUE_NONE);
      cmd_brval_set(cmd_next, VALUE_NONE);
    }
    cmd = cmd_next;
  }
  /* look for any re-entrant functions and mark then `frame'
   * similarly, any indirect functions need to be marked `frame'
   * because for now these all need to be permanently allocated.
   *   In the future, any time a function is called indirectly
   *   I should assume any function with the same signature might
   *   be called, and add that information to the interference
   *   graph. 
   * Any procedure marked `nostack' needs a procedure level variable,
   *   _return, to hold the return address */
  for (proc = pfile_proc_root_get(pf);
       proc;
       proc = pfile_proc_next(proc)) {
    if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_REENTRANT)
      || pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INDIRECT)) {
      pfile_proc_flag_set(proc, PFILE_PROC_FLAG_FRAME);
    }
  }
}

