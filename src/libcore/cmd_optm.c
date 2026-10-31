/**********************************************************
 **
 ** cmdoptmz.c : command optimizier functions
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: fork extensions (heavy rework)
 **
 ***********************************************************/
#include <assert.h>
#include <string.h>

#include "pfile.h"
#include "cmd.h"
#include "pf_proc.h"
#include "pf_op.h"
#include "cmd_brch.h"
#include "cmd_op.h"
#include "cmd_asm.h"
#include "cmd_optm.h"
#include "label.h"

static boolean_t cmd_opt_label_same(label_t a, label_t b)
{
  if (a == b) {
    return BOOLEAN_TRUE;
  }
  if (!a || !b) {
    return BOOLEAN_FALSE;
  }
  return (0 == strcmp(label_name_get(a), label_name_get(b)));
}

/* After cmd_opt_branch, jal may lower "goto exit_label" to CMD_BRANCHTYPE_RETURN
 * when the target is a return site. Accept only if the next executable is
 * PROC_LEAVE for the same procedure (narrow — avoids bogus tail merges). */
static boolean_t cmd_opt_return_is_leave_of_proc(cmd_t ret_cmd,
    pfile_proc_t *cur)
{
  cmd_t scan;

  if (!cur) {
    return BOOLEAN_FALSE;
  }
  if ((CMD_TYPE_BRANCH != cmd_type_get(ret_cmd))
      || (CMD_BRANCHTYPE_RETURN != cmd_brtype_get(ret_cmd))
      || (CMD_BRANCHCOND_NONE != cmd_brcond_get(ret_cmd))) {
    return BOOLEAN_FALSE;
  }
  scan = cmd_next_exec_get(cmd_link_get(ret_cmd));
  return scan
    && (CMD_TYPE_PROC_LEAVE == cmd_type_get(scan))
    && (cmd_proc_get(scan) == cur);
}

/*
 * NAME
 *   cmd_opt_branch
 *
 * DESCRIPTION
 *   basic branch optimization
 *
 * PARAMETERS
 *   cmd : head of the cmd list
 *
 * RETURN
 *   none
 *
 * NOTES
 *   first, the true branch destination is determined by following
 *     any chains of absolute GOTOs
 *   next:
 *     1. call to a return --> removed
 *     2. call immediately followed by return --> call changed to goto
 *     3. goto a return --> goto changed to return
 *     4. if (cond) goto l1
 *        absolute branch
 *        l1:
 *        ...
 *        --> if (!cond) goto l2
 *     5. goto l1 (or if cond goto l1)
 *        l1:
 *        remove!
 *
 * nb: on 5 above, there's a separate conditional possibility introduced
 *     by the PIC backend. Namely:
 *        tmp = expr
 *        if tmp goto l1
 *        l1:
 *     tmp = expr might be removed, so this becomes:
 *        expr
 *        if () goto l1
 *        l1:
 *     where () is simply the flags resulting from `tmp = expr'
 */
/* on entry, cmd points to a GOTO or a CALL
 * the returned cmd is the *true* destination (aka, this follows
 * a string of absolute GOTOs until it reaches the end) */
static cmd_t cmd_branch_true_dest_get(cmd_t cmd, cmd_t *cmd_head, label_t *dlbl)
{
  label_t lbl;
  unsigned opt;

  opt = cmd_opt_get(cmd) + 1;

  do {
    cmd_opt_set(cmd, opt);
    lbl = cmd_brdst_get(cmd);
    cmd = cmd_next_exec_get(cmd_label_find(*cmd_head, lbl));
  } while ((CMD_BRANCHTYPE_GOTO == cmd_brtype_get(cmd))
    && (CMD_BRANCHCOND_NONE == cmd_brcond_get(cmd))
    && (opt != cmd_opt_get(cmd)));
  *dlbl = lbl;
  return cmd;
}

/*
 * NAME
 *   cmd_opt_tail_return
 *
 * DESCRIPTION
 *   Recognize  return f(...)  after other passes:
 *
 *   (1) pfile_opt_proc_return() often merges
 *         call f(..., tmp); assign ret <- tmp; goto exit
 *       into
 *         call f(..., ret); goto exit
 *
 *   (2) If the temp could not be merged, the assign remains.
 *
 *   In both cases, fuse into a tail transfer:
 *         goto f(..., ret_param)
 *   and drop the trailing goto exit, so the callee returns directly to
 *   the caller of the current procedure (legacy jal codegen behavior).
 */
boolean_t cmd_opt_tail_return(cmd_t *cmd_head)
{
#define JAL_TAIL_PROC_STACK 128
  pfile_proc_t *proc_stack[JAL_TAIL_PROC_STACK];
  unsigned      depth;
  cmd_t         cmd;
  boolean_t     changed;

  depth   = 0;
  changed = BOOLEAN_FALSE;
  for (cmd = *cmd_head; cmd; cmd = cmd_link_get(cmd)) {
    if (CMD_TYPE_PROC_ENTER == cmd_type_get(cmd)) {
      if (depth < JAL_TAIL_PROC_STACK) {
        proc_stack[depth] = cmd_proc_get(cmd);
        depth++;
      } else {
        depth = JAL_TAIL_PROC_STACK;
      }
    } else if (CMD_TYPE_PROC_LEAVE == cmd_type_get(cmd)) {
      if (depth) {
        depth--;
      }
    } else if ((CMD_TYPE_BRANCH == cmd_type_get(cmd))
        && (CMD_BRANCHTYPE_CALL == cmd_brtype_get(cmd))
        && (CMD_BRANCHCOND_NONE == cmd_brcond_get(cmd))
        && cmd_brproc_get(cmd)) {
      pfile_proc_t *cur;
      pfile_proc_t *callee;
      value_t       bproc;
      cmd_t         n1;
      cmd_t         n2;
      value_t       ret;
      value_t       tret;
      value_t       dst;
      value_t       src;

      if (!depth) {
        continue;
      }
      cur = proc_stack[depth - 1];
      bproc = cmd_brproc_get(cmd);
      callee = value_proc_get(bproc);
      if (!cur || !callee) {
        continue;
      }
      /* Only the root program procedure has no parent; never elide its leave. */
      if (!pfile_proc_parent_get(cur)) {
        continue;
      }
      if (pfile_proc_flag_test(cur, PFILE_PROC_FLAG_INTERRUPT)) {
        continue;
      }
      if (pfile_proc_flag_test(callee, PFILE_PROC_FLAG_REENTRANT)
          || pfile_proc_flag_test(callee, PFILE_PROC_FLAG_INDIRECT)
          || pfile_proc_flag_test(callee, PFILE_PROC_FLAG_TASK)
          || pfile_proc_flag_test(callee, PFILE_PROC_FLAG_NOSTACK)) {
        continue;
      }
      ret = pfile_proc_param_get(cur, 0);
      if (!ret) {
        continue;
      }
      tret = cmd_brproc_param_get(cmd, 0);
      /* cmd_next_exec_get(cmd) would return cmd when cmd is executable; need *successor* */
      n1 = cmd_next_exec_get(cmd_link_get(cmd));
      if (!n1) {
        continue;
      }
      /* Pattern B: call(..., tmp); assign ret <- tmp; goto exit (adjacent triple) */
      if ((CMD_TYPE_OPERATOR == cmd_type_get(n1))
          && operator_is_assign(cmd_optype_get(n1))) {
        boolean_t n2_ok;

        n2 = cmd_next_exec_get(cmd_link_get(n1));
        n2_ok = BOOLEAN_FALSE;
        if (n2) {
          if ((CMD_TYPE_PROC_LEAVE == cmd_type_get(n2))
              && (cmd_proc_get(n2) == cur)) {
            n2_ok = BOOLEAN_TRUE;
          } else if ((CMD_TYPE_BRANCH == cmd_type_get(n2))
              && (CMD_BRANCHCOND_NONE == cmd_brcond_get(n2))
              && (((CMD_BRANCHTYPE_GOTO == cmd_brtype_get(n2))
                  && cmd_opt_label_same(cmd_brdst_get(n2),
                      pfile_proc_exit_label_get(cur)))
                  || cmd_opt_return_is_leave_of_proc(n2, cur))) {
            n2_ok = BOOLEAN_TRUE;
          }
        }
        if (n2_ok) {
          dst = cmd_opdst_get(n1);
          src = cmd_opval1_get(n1);
          if (value_is_same(dst, ret)
              && value_is_same(src, tret)) {
            cmd_brproc_param_set(cmd, 0, dst);
            cmd_brdst_set(cmd, pfile_proc_label_get(callee));
            cmd_brtype_set(cmd, CMD_BRANCHTYPE_GOTO);
            if (CMD_TYPE_PROC_LEAVE == cmd_type_get(n2)) {
              if (depth) {
                depth--;
              }
            }
            cmd_remove(cmd_head, n2);
            cmd_remove(cmd_head, n1);
            changed = BOOLEAN_TRUE;
            continue;
          }
        }
      }
      /* call + (assign glue only)* + goto exit — tail-transfer to callee.
       * Force return slot to this procedure's OUT param so the callee writes
       * the result where the ultimate caller expects it. */
      {
        label_t xlb;
        cmd_t   gexit;
        cmd_t   scan;
        cmd_t   rm;
        unsigned gg;

        xlb = pfile_proc_exit_label_get(cur);
        if (!xlb) {
          continue;
        }
        gexit = CMD_NONE;
        if ((CMD_TYPE_BRANCH == cmd_type_get(n1))
            && (CMD_BRANCHCOND_NONE == cmd_brcond_get(n1))
            && (((CMD_BRANCHTYPE_GOTO == cmd_brtype_get(n1))
                && cmd_opt_label_same(cmd_brdst_get(n1), xlb))
                || cmd_opt_return_is_leave_of_proc(n1, cur))) {
          gexit = n1;
        } else {
          for (scan = cmd_link_get(cmd), gg = 0;
               scan && (gg < 2048);
               scan = cmd_link_get(scan), gg++) {
            if (CMD_TYPE_PROC_LEAVE == cmd_type_get(scan)) {
              if (cmd_proc_get(scan) == cur) {
                gexit = scan;
              }
              break;
            }
            if (!cmd_is_executable(scan)) {
              continue;
            }
            if ((CMD_TYPE_COMMENT == cmd_type_get(scan))
                || (CMD_TYPE_NOP == cmd_type_get(scan))) {
              continue;
            }
            if ((CMD_TYPE_BRANCH == cmd_type_get(scan))
                && (CMD_BRANCHCOND_NONE == cmd_brcond_get(scan))
                && (((CMD_BRANCHTYPE_GOTO == cmd_brtype_get(scan))
                    && cmd_opt_label_same(cmd_brdst_get(scan), xlb))
                    || cmd_opt_return_is_leave_of_proc(scan, cur))) {
              gexit = scan;
              break;
            }
          }
        }
        if (!gexit) {
          continue;
        }
        for (scan = cmd_link_get(cmd); scan && (scan != gexit);
             scan = cmd_link_get(scan)) {
          if (!cmd_is_executable(scan)) {
            continue;
          }
          if ((CMD_TYPE_COMMENT == cmd_type_get(scan))
              || (CMD_TYPE_NOP == cmd_type_get(scan))) {
            continue;
          }
          if ((CMD_TYPE_BRANCH == cmd_type_get(scan))
              && (CMD_BRANCHTYPE_CALL == cmd_brtype_get(scan))) {
            gexit = CMD_NONE;
            break;
          }
          if ((CMD_TYPE_OPERATOR != cmd_type_get(scan))
              || !operator_is_assign(cmd_optype_get(scan))) {
            gexit = CMD_NONE;
            break;
          }
          /* Glue must only copy temps: `dst <- src`.  Anything with a
           * second operand, or writing a user variable, is real code
           * (e.g. `i2c_scl_direction = low` before a tail _i2c_wait())
           * that the tail transfer would delete — bail out and keep the
           * ordinary call+return sequence. */
          if (cmd_opval2_get(scan)
              || !value_is_temp(cmd_opdst_get(scan))) {
            gexit = CMD_NONE;
            break;
          }
        }
        if (!gexit) {
          continue;
        }
        cmd_brproc_param_set(cmd, 0, ret);
        cmd_brdst_set(cmd, pfile_proc_label_get(callee));
        rm = cmd_link_get(cmd);
        while (rm && rm != gexit) {
          cmd_t nx;

          nx = cmd_link_get(rm);
          cmd_remove(cmd_head, rm);
          rm = nx;
        }
        if (rm == gexit) {
          if (CMD_TYPE_PROC_LEAVE == cmd_type_get(gexit)) {
            if (depth) {
              depth--;
            }
          }
          cmd_remove(cmd_head, gexit);
        }
        cmd_brtype_set(cmd, CMD_BRANCHTYPE_GOTO);
        changed = BOOLEAN_TRUE;
      }
    }
  }
  return changed;
#undef JAL_TAIL_PROC_STACK
}

boolean_t cmd_opt_branch(cmd_t *cmd_head)
{
  cmd_t cmd;
  unsigned rem_ct;
  cmd_t    cmd_prev;

  /* first, move cmd to the first *executable* instruction */
  rem_ct = 0;
  cmd = cmd_next_exec_get(*cmd_head);
  cmd_prev = CMD_NONE;

  while (cmd) {
    cmd_t cmd_next;

    cmd_next = cmd_link_get(cmd);

    if (value_is_const(cmd_brval_get(cmd))
        && ((CMD_BRANCHCOND_TRUE == cmd_brcond_get(cmd))
          || (CMD_BRANCHCOND_FALSE == cmd_brcond_get(cmd)))) {
      /* we know the condition, can we remove the instruction? */
      boolean_t rem;

      if (value_const_get(cmd_brval_get(cmd))) {
        rem = CMD_BRANCHCOND_FALSE == cmd_brcond_get(cmd);
      } else {
        rem = CMD_BRANCHCOND_TRUE == cmd_brcond_get(cmd);
      }
      if (rem) {
        cmd_remove(cmd_head, cmd);
        rem_ct++;
        cmd = CMD_NONE;
      } else {
        cmd_brcond_set(cmd, CMD_BRANCHCOND_NONE);
      }
    }
    if ((CMD_BRANCHTYPE_GOTO == cmd_brtype_get(cmd))
      || (CMD_BRANCHTYPE_CALL == cmd_brtype_get(cmd))
      || (CMD_BRANCHTYPE_TASK_START == cmd_brtype_get(cmd))) {
      /* find the TRUE destination (aka, follow the call chain) */
      cmd_t   cmd_lbl_exec;
      label_t lbl;
      value_t brpv;
      pfile_proc_t *brproc;

      /* Tail GOTO to callee entry (return f()): brdst matches the procedure's
       * entry label. Do not follow the callee's internal goto chain — it may
       * jump to a shared epilogue whose next insn looks like RETURN, which
       * would delete this branch or mis-fold it. */
      brpv   = cmd_brproc_get(cmd);
      brproc = 0;
      if (brpv && (VARIABLE_DEF_TYPE_FUNCTION == value_type_get(brpv))) {
        brproc = value_proc_get(brpv);
      }
      if ((CMD_BRANCHTYPE_GOTO == cmd_brtype_get(cmd)) && brproc) {
        label_t el;

        el = pfile_proc_label_get(brproc);
        if (el && cmd_opt_label_same(cmd_brdst_get(cmd), el)) {
          cmd_lbl_exec = cmd_next_exec_get(cmd_label_find(*cmd_head, el));
          lbl          = el;
        } else {
          cmd_lbl_exec = cmd_branch_true_dest_get(cmd, cmd_head, &lbl);
        }
      } else {
        cmd_lbl_exec = cmd_branch_true_dest_get(cmd, cmd_head, &lbl);
      }
      /* lbl == TRUE destination
       * cmd_lbl_exec = first executable statement after lbl */
      cmd_brdst_set(cmd, lbl);
      if ((CMD_BRANCHTYPE_RETURN == cmd_brtype_get(cmd_lbl_exec))
        && (CMD_BRANCHCOND_NONE == cmd_brcond_get(cmd_lbl_exec))) {
        if (CMD_BRANCHTYPE_GOTO == cmd_brtype_get(cmd)) {
          /* GOTO destination is a RETURN; replace GOTO with RETURN.
           * Skip when brproc is set: cmd_opt_tail_return uses GOTO+callee
           * for tail transfer. The callee's first executable can be RETURN;
           * folding here would leave brproc set so pic_branch_proc emits
           * CALL (RETURN is not tail-GOTO), inflating hardware stack depth. */
          if (!cmd_brproc_get(cmd)) {
            cmd_brtype_set(cmd, CMD_BRANCHTYPE_RETURN);
          }
        } else {
          /* CALL destination is a RETURN; delete the CALL */
          cmd_remove(cmd_head, cmd);
          rem_ct++;
        }
      } else if (CMD_BRANCHTYPE_CALL == cmd_brtype_get(cmd)) {
        /* look at the next exectuteable statement, if it's a return,
         * change the CALL to a GOTO */
        cmd_t cmd_ptr;

        cmd_ptr = cmd_next_exec_get(cmd_link_get(cmd));
        if ((CMD_BRANCHTYPE_RETURN == cmd_brtype_get(cmd_ptr))
            && (CMD_BRANCHCOND_NONE == cmd_brcond_get(cmd_ptr))) {
          cmd_brtype_set(cmd, CMD_BRANCHTYPE_GOTO);
        }
      }
      if (CMD_BRANCHTYPE_GOTO == cmd_brtype_get(cmd)) {
        /* is this goto 1 forward? if so, get rid of it */
        cmd_t cmd_ptr;

        for (cmd_ptr = cmd_link_get(cmd);
             cmd_ptr
             && !cmd_is_executable(cmd_ptr)
             && (cmd_label_get(cmd_ptr) != cmd_brdst_get(cmd));
             cmd_ptr = cmd_link_get(cmd_ptr)) {
        }
        /* A GOTO with brproc set is a tail transfer (cmd_opt_tail_return):
         * the callee will return straight to OUR caller, so falling through
         * into the callee body is *not* equivalent — the current procedure's
         * frame / return marshaling is skipped (infinite $ra loop on MIPS).
         * Keep the branch. */
        if (cmd_brproc_get(cmd)) {
          cmd_ptr = CMD_NONE;
        }
        if (cmd_ptr && (cmd_label_get(cmd_ptr) == cmd_brdst_get(cmd))) {
          /* goto following label --> remove cmd */
          /* KY: 2010-May-03 -- don't know exactly what this is supposed
           * to do, but it breaks some code on the 16 bit cores
           * I think it's just supposed to make the result prettier
           * KY: 2012-Nov-14 -- I need to put this back (and fix it)!
           * It handles the case of an empty IF statement:
           *    IF expr THEN
           *    END IF
           * Without it, the expression is evaluated which leads to code
           * bloat.
           */
          /* Corrected version of KY's disabled removal: an empty IF
           * strands the stripped condition test when its branch is
           * removed (pic_cmd_optimize already cleared the branch value
           * and the test destination), so remove the test too -- but
           * only when cmd_prev provably IS a stripped test: an operator
           * cmd of the kind pic_cmd_optimize strips, with no destination
           * and no label. The 2010 form never verified cmd_prev was an
           * operator -- cmd_opdst_get() returns 0 for any non-operator
           * cmd (inline asm, sleep, ...), which is what broke the 16
           * bit cores. */
          if (cmd_prev
            && !cmd_label_get(cmd_prev)
            && (CMD_TYPE_OPERATOR == cmd_type_get(cmd_prev))
            && (CMD_BRANCHCOND_NONE != cmd_brcond_get(cmd))
            && !cmd_brval_get(cmd)
            && !cmd_opdst_get(cmd_prev)
            && (operator_is_relation(cmd_optype_get(cmd_prev))
              || (OPERATOR_ASSIGN == cmd_optype_get(cmd_prev))
              || (OPERATOR_LOGICAL == cmd_optype_get(cmd_prev))
              || (OPERATOR_NOTL == cmd_optype_get(cmd_prev))
              || (OPERATOR_CMPB == cmd_optype_get(cmd_prev)))) {
            cmd_remove(cmd_head, cmd_prev);
            cmd_prev = CMD_NONE;
            rem_ct++;
          }
          cmd_remove(cmd_head, cmd);
          rem_ct++;
          /* keep the walk's bookkeeping off the removed cmds
           * (same pattern as the const-condition removal above) */
          cmd = CMD_NONE;
        } else if ((CMD_BRANCHCOND_NONE != cmd_brcond_get(cmd))
          && (CMD_BRANCHTYPE_GOTO == cmd_brtype_get(cmd_ptr))
          && (CMD_BRANCHCOND_NONE == cmd_brcond_get(cmd_ptr))) {
          /* so far we've 
           *   if (cond) goto l1:
           *   goto l2:
           *   ???
           */
          cmd_t cmd_ptr2;

          for (cmd_ptr2 = cmd_link_get(cmd_ptr);
               cmd_ptr2
               && !cmd_is_executable(cmd_ptr2)
               && (cmd_label_get(cmd_ptr2) != cmd_brdst_get(cmd));
               cmd_ptr2 = cmd_link_get(cmd_ptr2))
            ; /* null body */
          if (cmd_label_get(cmd_ptr2) == cmd_brdst_get(cmd)) {
          }
        }
      }
    }
    if (cmd != cmd_next) {
      cmd_prev = cmd;
    }
    cmd = cmd_next;
  }
  return rem_ct != 0;
}

/*
 * look for:
 *   _temp = !x
 *   branch [true|false] _temp, ...
 *   --> branch [false|true] x
 *
 *   _temp = !!x
 *   branch [true|false] _temp, ...
 *   --> branch [true|false] x
 */
boolean_t cmd_opt_branch2(cmd_t *cmd_head)
{
  cmd_t    cmd;
  cmd_t    cmd_pv;
  unsigned rem_ct;

  rem_ct = 0;

  cmd_pv = CMD_NONE;
  cmd    = cmd_next_exec_get(*cmd_head);
  while (cmd) {
    cmd_t cmd_next;

    cmd_next = cmd_next_exec_get(cmd_link_get(cmd));
    if (((OPERATOR_NOTL == cmd_optype_get(cmd)
        || OPERATOR_LOGICAL == cmd_optype_get(cmd)))
      && value_is_temp(cmd_opdst_get(cmd))
      && value_is_same(cmd_opdst_get(cmd), cmd_brval_get(cmd_next))
      && ((CMD_BRANCHCOND_TRUE == cmd_brcond_get(cmd_next))
        || (CMD_BRANCHCOND_FALSE == cmd_brcond_get(cmd_next)))) {
      /* remove cmd */
      cmd_branchcond_t brcond;

      brcond = cmd_brcond_get(cmd_next);
      if (OPERATOR_NOTL == cmd_optype_get(cmd)) {
        /* invert the condition */
        brcond = (CMD_BRANCHCOND_TRUE == cmd_brcond_get(cmd_next))
            ? CMD_BRANCHCOND_FALSE
            : CMD_BRANCHCOND_TRUE;
      }
      if (cmd_pv) {
        /*
         * cmd_pv points to the previous *executable* statement,
         * it need to move to the actual previous statement
         */  
        while (cmd_link_get(cmd_pv) != cmd) {
          cmd_pv = cmd_link_get(cmd_pv);
        }
        cmd_link_set(cmd_pv, cmd_link_get(cmd));
      } else {
        *cmd_head = cmd_link_get(cmd);
      }
      cmd_brcond_set(cmd_next, brcond);
      cmd_brval_set(cmd_next, cmd_opval1_get(cmd));
      cmd_free(cmd);
      rem_ct++;
    } else {
      cmd_pv = cmd;
    }
    cmd = cmd_next;
  }
  return rem_ct != 0;
}

/*
 * NAME
 *   cmd_opt_branch4
 *
 * DESCRIPTION
 *   do branch4 optimizations (see below)
 *
 * PARAMETERS
 *   cmd_head : start of cmd list
 *
 * RETURN
 *   BOOLEAN_TRUE : something was changed
 *
 * NOTES
 *   This looks for operations of the form:
 *     val = cexpr
 *     goto [true|false] val, label
 *   and either changes the goto to an unconditional, or removes it  
 *   then, if val doesn't exist or is temporary & not further used,
 *   it will also be removed
 */
boolean_t cmd_opt_branch4(pfile_t *pf, cmd_t *cmd_head)
{
  cmd_t    cmd;
  cmd_t    cmd_pv;
  unsigned rem_ct;

  rem_ct = 0;

  cmd_pv = CMD_NONE;
  cmd    = *cmd_head; /* cmd_next_exec_get(*cmd_head); */
  while (cmd) {
    cmd_t cmd_next;

    cmd_next = cmd_link_get(cmd);
    if ((CMD_BRANCHCOND_NONE != cmd_brcond_get(cmd_next))
        && ((cmd_brval_get(cmd_next) 
            && value_is_pseudo_const(cmd_brval_get(cmd_next)))
          || (CMD_TYPE_OPERATOR == cmd_type_get(cmd)
            && (value_is_same(cmd_opdst_get(cmd), cmd_brval_get(cmd_next)))
            && value_is_pseudo_const(cmd_opval1_get(cmd))
            && value_is_pseudo_const(cmd_opval2_get(cmd))))) {
      variable_const_t n;

      if (cmd_brval_get(cmd_next)
          && value_is_pseudo_const(cmd_brval_get(cmd_next))) {
        n = value_is_const(cmd_brval_get(cmd_next))
          ? value_const_get(cmd_brval_get(cmd_next))
          : 0;
      } else {
        pf_const_t c;

        c = pfile_op_const_exec(pf, cmd_optype_get(cmd), 
            cmd_opval1_get(cmd), cmd_opval2_get(cmd));
        n = 0;
        switch (c.type) {
          case PF_CONST_TYPE_FLOAT:   n = c.u.f; break;
          case PF_CONST_TYPE_INTEGER: n = c.u.n; break;
          case PF_CONST_TYPE_NONE:
            break;
        }
      }
      /* determine whether to take the branch or not */
      if (((CMD_BRANCHCOND_FALSE == cmd_brcond_get(cmd_next)) && !n)
        || ((CMD_BRANCHCOND_TRUE == cmd_brcond_get(cmd_next)) && n)) {
        /* this becomes an absolute branch */
        cmd_brcond_set(cmd_next, CMD_BRANCHCOND_NONE);
        cmd_brval_set(cmd_next, VALUE_NONE);
      } else {
        /* this is removed */
        cmd_link_set(cmd, cmd_link_get(cmd_next));
        cmd_free(cmd_next);
        rem_ct++;
      }
      if (CMD_TYPE_OPERATOR == cmd_type_get(cmd)) {
        flag_t accessed;

        accessed = CMD_VARIABLE_ACCESS_FLAG_NONE;
        if (cmd_opdst_get(cmd)) {
          for (cmd_next = cmd_link_get(cmd);
               cmd_next 
               && (CMD_TYPE_BLOCK_END != cmd_type_get(cmd_next));
               cmd_next = cmd_link_get(cmd_next)) {
            accessed = cmd_value_accessed_get(cmd_next, cmd_opdst_get(cmd));
            if (accessed) {
              break;
            }
          }
        }
        if (!(accessed & CMD_VARIABLE_ACCESS_FLAG_READ)) {
          /* if the result of cmd is not used again, remove it */
          if (cmd_pv) {
            while (cmd_link_get(cmd_pv) != cmd) {
              cmd_pv = cmd_link_get(cmd_pv);
            }
            cmd_link_set(cmd_pv, cmd_link_get(cmd));
          } else {
            *cmd_head = cmd_link_get(cmd);
          }
          cmd_free(cmd);
          cmd = (cmd_pv) ? cmd_pv : *cmd_head;
          rem_ct++;
        }
      }
    }
    cmd_pv = cmd;
    cmd = cmd_link_get(cmd);
  }
  return rem_ct != 0;
}

/*
 * True for procedures that must be linked from pfile_proc_root for
 * pic_variable_alloc_proc even when their body only expands to compiler
 * builtins (no normal CALL edge): `pragma inline` JAL shims, and the
 * outlined `strings_dynamic` `string_*` forwarders (same MOVLB /
 * VARIABLE_BASE_UNKNOWN allocation issue if omitted).
 */
static boolean_t
cmd_proc_needs_root_link_for_alloc(const pfile_proc_t *proc)
{
  const char *t;

  if (!proc) {
    return BOOLEAN_FALSE;
  }
  if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INLINE)) {
    return BOOLEAN_TRUE;
  }
  t = pfile_proc_tag_get(proc);
  if (!t) {
    return BOOLEAN_FALSE;
  }
  if (0 == strcmp(t, "string_append")) {
    return BOOLEAN_TRUE;
  }
  if (0 == strcmp(t, "string_append_truncate")) {
    return BOOLEAN_TRUE;
  }
  if (0 == strcmp(t, "string_truncate")) {
    return BOOLEAN_TRUE;
  }
  if (0 == strcmp(t, "string_from_array")) {
    return BOOLEAN_TRUE;
  }
  if (0 == strcmp(t, "string_from_array_truncate")) {
    return BOOLEAN_TRUE;
  }
  if (0 == strcmp(t, "string_to_array")) {
    return BOOLEAN_TRUE;
  }
  if (0 == strcmp(t, "string_to_array_truncate")) {
    return BOOLEAN_TRUE;
  }
  if (0 == strcmp(t, "string_heap_compact")) {
    return BOOLEAN_TRUE;
  }
  return BOOLEAN_FALSE;
}

/*
 * Ensure every `pragma inline` JAL forwarder in the pfile is linked from
 * pfile_proc_root for pic_variable_alloc_proc, even when the only expansion
 * is a builtin and no p-code call lists that JAL procedure (e.g. Phase B
 * string opcodes with dst/src formals and MOVLB). Also link outlined
 * `string_*` shims from `demo_lib/strings_dynamic.jal` (no longer `pragma inline`).
 */
void
cmd_analyze_link_inline_procs_for_alloc(pfile_t *pf)
{
  pfile_proc_t *root;
  pfile_proc_t *proc;

  root = pfile_proc_root_get(pf);
  if (!root) {
    return;
  }
  for (proc = root; proc; proc = pfile_proc_next(proc)) {
    if (cmd_proc_needs_root_link_for_alloc(proc)) {
      (void) pfile_proc_call_add(root, proc);
    }
  }
}

/*
 * NAME
 *   cmd_analyze
 *
 * DESCRIPTION
 *   analyze the command list for dead code
 *
 * PARAMETERS
 *   cmd_head : head of the cmd list
 *   cmd      : current entry
 *   flag     : CMD_FLAG_USER      : analyzing user area
 *              CMD_FLAG_INTERRUPT : analyzing an ISR
 *
 * RETURN
 *   none
 *
 * NOTES
 *   this doesn't solve the NP problem, it simply takes each
 *   path to determine if it's possible to get there from here.
 *   After this pass, pfile.c calls cmd_analyze_link_inline_procs_for_alloc
 *   so pfile_proc_root has call edges to every `pragma inline` JAL forwarder
 *   for pic_variable_alloc_proc (Phase B / builtins may not list them).
 */

static size_t depth_max;
/* A4 teardown between compiles (debug statistic). */
void cmd_optm_state_reset(void)
{
  depth_max = 0;
}

/* Compare logical procedures (value_proc_get may not match cmd_proc_get ptr). */
static boolean_t cmd_proc_matches(const pfile_proc_t *a,
    const pfile_proc_t *b)
{
  const char *ta;
  const char *tb;

  if (!a || !b) {
    return BOOLEAN_FALSE;
  }
  if (a == b) {
    return BOOLEAN_TRUE;
  }
  ta = pfile_proc_tag_get(a);
  tb = pfile_proc_tag_get(b);
  return ta && tb && (0 == strcmp(ta, tb));
}

/* Resolve the {enter} cmd for proc. Do not use cmd_label_find + forward links:
 * a leading "goto skip" may not be the physical predecessor of {enter} in the
 * cmd list, so link-walking can miss the procedure body. */
static cmd_t cmd_proc_enter_cmd_get(cmd_t cmd_top, pfile_proc_t *proc)
{
  cmd_t scan;

  if (!proc) {
    return CMD_NONE;
  }
  for (scan = cmd_top; scan; scan = cmd_link_get(scan)) {
    if ((CMD_TYPE_PROC_ENTER == cmd_type_get(scan))
        && cmd_proc_matches(cmd_proc_get(scan), proc)) {
      return scan;
    }
  }
  return CMD_NONE;
}

void cmd_analyze(cmd_t cmd_top, cmd_t cmd, flag_t flag, size_t depth,
  pfile_t *pf)
{
  boolean_t pv_cond;

  pv_cond = BOOLEAN_FALSE;
  if (depth > depth_max) {
    depth_max = depth;
  }
  while (cmd && !cmd_flag_test(cmd, flag)) {
    cmd_flag_set(cmd, flag);
    if (CMD_TYPE_OPERATOR == cmd_type_get(cmd)) {
      if ((OPERATOR_ASSIGN == cmd_optype_get(cmd))
          || (OPERATOR_POINTER_ASSIGN == cmd_optype_get(cmd))) {
        if (VARIABLE_DEF_TYPE_FUNCTION
            == value_type_get(cmd_opval1_get(cmd))) {
          /* assigning a function to a variable. eventually this
           * variable will be called (it's probably worth checking
           * use first), so go ahead & make the call here */
          pfile_proc_t *prc;
          cmd_t         ptr;

          prc = value_proc_get(cmd_opval1_get(cmd));
          if (prc && pfile_proc_flag_test(prc, PFILE_PROC_FLAG_INLINE)) {
            /* see CALL param loop: inline JAL / builtin shims */
          } else {
            ptr = cmd_proc_enter_cmd_get(cmd_top, prc);
            if (ptr) {
              cmd_analyze(cmd_top, ptr, flag, depth + 1, pf);
            }
          }
        }
      }
      pv_cond = BOOLEAN_FALSE;
      cmd = cmd_link_get(cmd);
    } else if (cmd_asm_op_is_flow_terminator(cmd_asm_op_get(cmd))) {
      cmd = (pv_cond) ? cmd_link_get(cmd) : CMD_NONE;
    } else if (CMD_TYPE_BRANCH == cmd_type_get(cmd)) {
      switch (cmd_brtype_get(cmd)) {
        case CMD_BRANCHTYPE_GOTO:
        case CMD_BRANCHTYPE_CALL:
        case CMD_BRANCHTYPE_TASK_START:
          {
            value_t  proc;
            value_t *params;
            size_t   param_ct;
            size_t   ii;

            proc     = cmd_brproc_get(cmd);
            param_ct = cmd_brproc_param_ct_get(cmd);
            params   = cmd_brproc_params_get(cmd);

            if (cmd_brdst_get(cmd)) {
              /* look for the label */
              cmd_t ptr;

              ptr = cmd_label_find(cmd_top, cmd_brdst_get(cmd));
              if ((CMD_BRANCHTYPE_GOTO == cmd_brtype_get(cmd))
                && (cmd_brdst_get(cmd) == cmd_label_get(cmd_link_get(ptr)))) {
                /* GOTO (cmd+1) -- this is unnecessary! */
                cmd_flag_clr(ptr, flag);
                cmd = cmd_link_get(cmd);
              } else if ((CMD_BRANCHTYPE_GOTO == cmd_brtype_get(cmd))
                  && (CMD_BRANCHCOND_NONE == cmd_brcond_get(cmd))) {
                /* unconditional goto, simply change cmd */
                cmd = ptr;
              } else {
                /* recurse using ptr, then continue */
                cmd_analyze(cmd_top, ptr, flag, depth + 1, pf);
                cmd = cmd_link_get(cmd);
              }
            } else if (proc) {
              /* this is an indirect call */
              value_t val;
              pfile_proc_t *dprc;

              val = proc;
              if (VARIABLE_DEF_TYPE_FUNCTION == value_type_get(val)) {
                cmd_t ptr;

                dprc = value_proc_get(val);
                ptr = cmd_proc_enter_cmd_get(cmd_top, dprc);
                if (ptr) {
                  cmd_analyze(cmd_top, ptr, flag, depth + 1, pf);
                }
                cmd = cmd_link_get(cmd);
              } else if (VARIABLE_DEF_TYPE_POINTER == value_type_get(val)) {
                cmd = cmd_link_get(cmd);
              }
            }
            /* all the parameter must also be checked. any function
               references or pointers will have to be assumed to have
               been called. */
            /* parameter 0 is the return value & can be ignored */
            /* Do not analyze inline JAL wrappers: cmd_analyze_link_inline... */
            for (ii = 1; ii < param_ct; ii++) {
              value_t         fpv;
              pfile_proc_t   *cproc_ref;
              cmd_t           ptr;
              label_t         lbl;

              fpv = params[ii];
              if (VARIABLE_DEF_TYPE_FUNCTION != value_type_get(fpv)) {
                continue;
              }
              cproc_ref = value_proc_get(fpv);
              if (!cproc_ref
                || pfile_proc_flag_test(cproc_ref, PFILE_PROC_FLAG_INLINE)) {
                continue;
              }
              lbl = pfile_proc_label_get(
                variable_proc_get(
                  value_variable_get(
                    params[ii])
                )
              );
              assert(lbl != LABEL_NONE);
              if (lbl) {
                ptr = cmd_proc_enter_cmd_get(cmd_top,
                    value_proc_get(params[ii]));
                if (ptr) {
                  cmd_analyze(cmd_top, ptr, flag, depth + 1, pf);
                }
              }
            }
          }
          pv_cond = BOOLEAN_FALSE;
          break;
        case CMD_BRANCHTYPE_RETURN:
          if (CMD_BRANCHCOND_NONE == cmd_brcond_get(cmd)) {
            /* at the end of the chain, so exit */
            pv_cond = BOOLEAN_FALSE;
            cmd = 0;
            break;
          }
          /* fall through */
        case CMD_BRANCHTYPE_NONE: 
        case CMD_BRANCHTYPE_TASK_END:
        case CMD_BRANCHTYPE_TASK_YIELD:
        case CMD_BRANCHTYPE_TASK_SUSPEND:
        case CMD_BRANCHTYPE_TASK_RESUME:
        case CMD_BRANCHTYPE_TASK_KILL:
        case CMD_BRANCHTYPE_TASK_RESTART:
          pv_cond = BOOLEAN_FALSE;
          cmd = cmd_link_get(cmd);
          break;
      }
    } else if ((CMD_TYPE_END == cmd_type_get(cmd))
      || (CMD_TYPE_PROC_LEAVE == cmd_type_get(cmd))
      || (CMD_TYPE_ISR_CLEANUP == cmd_type_get(cmd))) {
      pv_cond = BOOLEAN_FALSE;
      cmd = 0; /* we're done */
    } else if (CMD_TYPE_ASM == cmd_type_get(cmd)) {
      label_t lbl;
      cmd_asm_op_t op;

      lbl = cmd_asm_lbl_get(cmd);
      if (LABEL_NONE != lbl) {
        cmd_t ptr;

        ptr = cmd_label_find(cmd_top, lbl);
        cmd_analyze(cmd_top, ptr, flag, depth + 1, pf);
      }
      op = cmd_asm_op_get(cmd);
#if 0
      cmd = (!pv_cond
            && ((PIC_OPCODE_GOTO == op)
              || (PIC_OPCODE_BRA == op)
              || (PIC_OPCODE_RESET == op)
              || (PIC_OPCODE_RETURN == op)
              || (PIC_OPCODE_RETFIE == op)))
              ? CMD_NONE
              : cmd_link_get(cmd);
#else
      cmd = cmd_link_get(cmd);
#endif

      pv_cond = cmd_asm_op_is_cond_skip(op);
    } else {
      if (cmd_is_executable(cmd)) {
        cmd_asm_op_t op;

        op = cmd_asm_op_get(cmd);
        pv_cond = cmd_asm_op_is_cond_skip(op);
      }
      cmd = cmd_link_get(cmd);
    }
  }
}

