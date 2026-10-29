/************************************************************
 **
 ** pic_stk.c : determine PIC stack usage definitions
 **
 ** Copyright (c) 2005, Kyle A. York
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: fork extensions (heavy rework)
 **
 ************************************************************/
#include <stdlib.h>
#include "../libcore/pf_proc.h"
#include "../libcore/pfile.h"
#include "../libcore/label.h"
#include "pic.h"
#include "pic_code.h"
#include "piccolst.h"
#include "pic_stk.h"

#define PIC_STACK_DEPTH_INFINITE 15

/* Lines remaining for pic_stack_depth debug (reset each pic_stack_depth_get). */
static unsigned jal_pic_stk_dbg_budget;

static unsigned jal_pic_stk_depth_before_isr;
static unsigned jal_pic_stk_depth_total;

/*
 * DFS path bookkeeping for cycle cutting. The depth walk computes the longest
 * ACYCLIC path: a node already on the current path (flagged
 * PIC_CODE_FLAG_STK_PATH) must not be re-entered from a recursion or the
 * linear walk of a nested invocation. Without this, static call-graph cycles
 * (tail-call goto chains through inline-expanded blocks that reach a caller's
 * call site) are re-walked at ever-greater depth by the "revisit when cached
 * depth differs" memo rule and diverge until the depth cap
 * (16f1825_vl53l0x_single: baseline 6 of 16, diverging walker 16 of 16).
 * See fix_pic_stack_depth_dfs_cycle_cut.md.
 */
static pic_code_t *jal_stk_path_stack;
static size_t      jal_stk_path_top;
static size_t      jal_stk_path_cap;

static void jal_stk_path_push(pfile_t *pf, pic_code_t code)
{
  UNUSED(pf);
  if (jal_stk_path_top == jal_stk_path_cap) {
    size_t          cap;
    pic_code_t     *stack;

    cap   = jal_stk_path_cap ? (2 * jal_stk_path_cap) : 256;
    stack = (pic_code_t *) realloc(jal_stk_path_stack,
      cap * sizeof(*stack));
    if (stack) {
      jal_stk_path_stack = stack;
      jal_stk_path_cap   = cap;
    } else {
      /* allocation failure: leave the node unmarked; worst case the walk
       * falls back to cap-bounded behaviour, never wrong results */
      return;
    }
  }
  jal_stk_path_stack[jal_stk_path_top++] = code;
  pic_code_flag_set(code, PIC_CODE_FLAG_STK_PATH);
}

static void jal_stk_path_pop(size_t mark)
{
  while (jal_stk_path_top > mark) {
    jal_stk_path_top--;
    pic_code_flag_clr(jal_stk_path_stack[jal_stk_path_top],
      PIC_CODE_FLAG_STK_PATH);
  }
}

/*
 * Walk PIC instructions for maximum hardware return-stack depth.
 * Thread code_is_cond like pic_code_mark_used: BTFSC/BTFSS/INCFSZ/DECFSZ
 * mark the *next* opcode as reached only via a conditional skip edge.
 */
static unsigned pic_stack_depth_get_user(pfile_t *pf,
    pic_code_t code, value_t pcl, unsigned depth)
{
  unsigned   depth_max;
  boolean_t  code_is_cond;
  boolean_t  next_is_cond;
  size_t     path_mark;

  depth_max    = depth;
  code_is_cond = BOOLEAN_FALSE;

  if (depth_max >= 16) {
    return depth_max;
  }

  path_mark = jal_stk_path_top;

  /*
   * logically, we could stop when depth_max reaches DEPTH_INFINITE, but I
   * want each bit of code marked for the debug output.
   */
  /*
   * Re-enter this instruction when the cached entry depth differs from the
   * current depth. A deeper visit first used to block a shallower visit
   * (only `cached < depth` was checked), which could miss a higher hardware
   * stack peak reachable only after a shallower call chain (shared PIC code
   * reached from multiple call depths). Same-depth revisits are skipped.
   * See fix_pic_stack_depth_shallow_revisit_blocked.md.
   */
  while (code
      /*&& (depth_max < 16)*/
      && ((-1U == pic_code_depth_get(code))
        || (pic_code_depth_get(code) != depth))) {
    pic_code_t   next;
    pic_opcode_t op;
    boolean_t    is_suspend;
    label_t      lbl;
    pic_code_t   lbl_code;
    unsigned     n;

    if (pic_code_flag_test(code, PIC_CODE_FLAG_STK_PATH)) {
      break;
    }

    op         = pic_code_op_get(code);
    is_suspend = pic_code_is_suspend(pf, code);

    if ((PIC_OPCODE_ORG == op) && depth > 0 && 0 == pic_code_pc_get(code)) {
      break;
    }

    pic_code_depth_set(code, depth);
    jal_stk_path_push(pf, code);
    next         = pic_code_next_get(code);
    next_is_cond = BOOLEAN_FALSE;

    if (jal_pic_stk_dbg_budget
        && depth >= 8
        && (pfile_debug_sub_test(pf, PFILE_DBGSUB_STACK)
          || pfile_flag_test(pf, PFILE_FLAG_DEBUG_COMPILER))) {
      const char *brname;
      label_t     brl;

      brname = "-";
      brl    = LABEL_NONE;
      if ((PIC_OPCODE_GOTO == op) || (PIC_OPCODE_CALL == op)
          || (PIC_OPCODE_RCALL == op) || (PIC_OPCODE_BRA == op)
          || (PIC_OPCODE_BC == op) || (PIC_OPCODE_BN == op)
          || (PIC_OPCODE_BNC == op) || (PIC_OPCODE_BNN == op)
          || (PIC_OPCODE_BNOV == op) || (PIC_OPCODE_BNZ == op)
          || (PIC_OPCODE_BOV == op) || (PIC_OPCODE_BZ == op)) {
        brl = pic_code_brdst_get(code);
        if (brl) {
          brname = label_name_get(brl);
        }
      }
      jal_pic_stk_dbg_budget--;
      pfile_log(pf, PFILE_LOG_DEBUG,
          "pic_stack_depth: depth=%u pc=0x%lx op=%u cond=%d susp=%d brdst=%s",
          depth, (unsigned long) pic_code_pc_get(code), (unsigned) op,
          (int) code_is_cond, (int) is_suspend, brname);
    }

    if (PIC_OPCODE_NONE != op) {
      switch (op) {
        case PIC_OPCODE_BTFSC:
        case PIC_OPCODE_BTFSS:
        case PIC_OPCODE_INCFSZ:
        case PIC_OPCODE_DECFSZ:
          next_is_cond = BOOLEAN_TRUE;
          break;

        case PIC_OPCODE_RETFIE:
        case PIC_OPCODE_RETLW:
        case PIC_OPCODE_RETURN:
          if (!code_is_cond) {
            next = PIC_CODE_NONE;
          } else {
            /*
             * Return may run (pop) or be skipped (fall-through). Following
             * "after return" in the same walk merges both with the same depth,
             * inflating stack (same class of bug as conditional GOTO
             * fall-through; see fix_pic_stack_depth_conditional_goto_fallthrough).
             */
            pic_code_t after;

            after = pic_code_next_get(code);
            if (after
                && !pic_code_flag_test(after, PIC_CODE_FLAG_STK_PATH)) {
              n = pic_stack_depth_get_user(pf, after, pcl, depth);
              if (n > depth_max) {
                depth_max = n;
              }
              if (depth_max >= 16) {
                jal_stk_path_pop(path_mark);
                return depth_max;
              }
            }
            next = PIC_CODE_NONE;
          }
          break;

        case PIC_OPCODE_GOTO:
        case PIC_OPCODE_BRA:
          /*
           * GOTO: absolute branch. BRA: PIC18 unconditional relative branch.
           * Same listing/CFG pattern (fix_pic_stack_depth_conditional_goto
           * fall-through applies to BRA under skip/suspend).
           */
          lbl = pic_code_brdst_get(code);
          lbl_code = (lbl) ? pic_code_label_find(pf, lbl) : PIC_CODE_NONE;
          if (!lbl_code) {
            /*
             * A GOTO always transfers control; when brdst metadata is absent
             * or the label no longer resolves (tail-call fused GOTOs at
             * procedure ends), the listing successor is the NEXT procedure's
             * body — following it linearly fabricates call-graph cycles and
             * diverges to the depth cap. End the path instead of falling
             * through. See fix_pic_stack_depth_goto_unresolved_fallthrough.md.
             */
            next = PIC_CODE_NONE;
          } else if (!code_is_cond && !is_suspend) {
            next = lbl_code;
          } else {
                pic_code_t after;

                if (!pic_code_flag_test(lbl_code, PIC_CODE_FLAG_STK_PATH)) {
                  n = pic_stack_depth_get_user(pf, lbl_code, pcl, depth);
                  if (n > depth_max) {
                    depth_max = n;
                  }
                  if (depth_max >= 16) {
                    jal_stk_path_pop(path_mark);
                    return depth_max;
                  }
                }
                /*
                 * Do not set next = pic_code_next_get(code): that merged the
                 * taken-GOTO path with the listing successor and inflated depth
                 * (fix_pic_stack_depth_conditional_goto_fallthrough).
                 *
                 * The successor after this GOTO is still reachable on the skip
                 * edge when the prior BTFSC/BTFSS skips this GOTO (same dual edge
                 * as pic_code_mark_used: recurse branch target and continue next).
                 * Walk it as a separate recursion at the same depth so we do not
                 * under-count (e.g. 18f4620 sample_pwm into l__pic_divide).
                 */
                after = pic_code_next_get(code);
                if (after
                    && !pic_code_flag_test(after, PIC_CODE_FLAG_STK_PATH)) {
                  n = pic_stack_depth_get_user(pf, after, pcl, depth);
                  if (n > depth_max) {
                    depth_max = n;
                  }
                  if (depth_max >= 16) {
                    jal_stk_path_pop(path_mark);
                    return depth_max;
                  }
                }
                next = PIC_CODE_NONE;
          }
          break;

        case PIC_OPCODE_BC:
        case PIC_OPCODE_BN:
        case PIC_OPCODE_BNC:
        case PIC_OPCODE_BNN:
        case PIC_OPCODE_BNOV:
        case PIC_OPCODE_BNZ:
        case PIC_OPCODE_BOV:
        case PIC_OPCODE_BZ:
          /*
           * PIC18 status-relative branches: always two successors (taken label
           * and fall-through). Same-depth recursions as conditional GOTO; do not
           * follow only pic_code_next or only brdst (parity pic_bsr_gop_state_succ
           * / pic_code_gop_analyze).
           */
          {
            pic_code_t after;

            lbl = pic_code_brdst_get(code);
            lbl_code = (lbl) ? pic_code_label_find(pf, lbl) : PIC_CODE_NONE;
            after = pic_code_next_get(code);
            if (lbl_code
                && !pic_code_flag_test(lbl_code, PIC_CODE_FLAG_STK_PATH)) {
              n = pic_stack_depth_get_user(pf, lbl_code, pcl, depth);
              if (n > depth_max) {
                depth_max = n;
              }
              if (depth_max >= 16) {
                jal_stk_path_pop(path_mark);
                return depth_max;
              }
            }
            if (after
                && !pic_code_flag_test(after, PIC_CODE_FLAG_STK_PATH)) {
              n = pic_stack_depth_get_user(pf, after, pcl, depth);
              if (n > depth_max) {
                depth_max = n;
              }
              if (depth_max >= 16) {
                jal_stk_path_pop(path_mark);
                return depth_max;
              }
            }
            next = PIC_CODE_NONE;
          }
          break;

        case PIC_OPCODE_CALL:
        case PIC_OPCODE_RCALL:
          lbl = pic_code_brdst_get(code);
          if (lbl) {
            lbl_code = pic_code_label_find(pf, lbl);
            if (lbl_code
                && !pic_code_flag_test(lbl_code, PIC_CODE_FLAG_STK_PATH)) {
              n = pic_stack_depth_get_user(pf, lbl_code, pcl, depth + 1);
              if (n > depth_max) {
                depth_max = n;
              }
              if (depth_max >= 16) {
                jal_stk_path_pop(path_mark);
                return depth_max;
              }
            }
          }
          break;

        default:
          /*
           * Any instruction that assigns into PCL ends this linear path.
           * Do not use value_is_same(..., pcl) alone: value_is_same() is
           * false when either side is named "_temp" (value.c), so PCL
           * writes through temporaries would still follow pic_code_next and
           * merge bogus CFG (e.g. inflated depth back toward reset glue).
           */
          if (!code_is_cond && pic_code_modifies_pcl(pf, code)) {
            next = PIC_CODE_NONE;
          }
          break;
      }
      code_is_cond = next_is_cond;
    }
    code = next;
  }
  jal_stk_path_pop(path_mark);
  return depth_max;
}

void pic_stack_depth_diag_get(unsigned *before_isr, unsigned *total)
{
  if (before_isr) {
    *before_isr = jal_pic_stk_depth_before_isr;
  }
  if (total) {
    *total = jal_pic_stk_depth_total;
  }
}

unsigned pic_stack_depth_get(pfile_t *pf)
{
  unsigned      depth;
  label_t       lbl;
  value_t       pcl;
  pfile_proc_t *proc;
  pic_code_t    c;

  /* pic_stack_depth_get_user() may revisit the same pic_code at different
   * entry depths; clear cached depths before each integrated walk. Also make
   * sure no STK_PATH marks leak from an earlier walk. */
  for (c = pic_code_list_head_get(pf); c; c = pic_code_next_get(c)) {
    pic_code_depth_set(c, (unsigned) -1);
    pic_code_flag_clr(c, PIC_CODE_FLAG_STK_PATH);
  }
  jal_stk_path_top = 0;

  jal_pic_stk_dbg_budget = 48;

  pcl = pfile_value_find(pf, PFILE_LOG_ERR, "_pcl");

  lbl = pfile_user_entry_get(pf);
  if (lbl) {
    pic_code_t start;

    /* Walk from the user program entry, not the list head. The head is
     * ORG/reset/vector glue; starting there can traverse linkage that does
     * not match execution from reset into main and inflates return-stack
     * depth. Tasks already use label_code_get(proc); main should too. */
    start = (pic_code_t) label_code_get(lbl);
    if (start) {
      depth = pic_stack_depth_get_user(pf, start, pcl, 0);
    } else {
      if (pfile_debug_sub_test(pf, PFILE_DBGSUB_STACK)
          || pfile_flag_test(pf, PFILE_FLAG_DEBUG_COMPILER)) {
        pfile_log(pf, PFILE_LOG_DEBUG,
            "pic_stack_depth_get: user entry label has no pic_code; "
            "using list head");
      }
      depth = pic_stack_depth_get_user(pf, pic_code_list_head_get(pf), pcl, 0);
    }
    label_release(lbl);
  } else {
    depth = pic_stack_depth_get_user(pf, pic_code_list_head_get(pf), pcl, 0);
  }
  for (proc = pfile_proc_root_get(pf);
       proc;
       proc = pfile_proc_next(proc)) {
    if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_TASK)) {
      pic_code_t code;

      lbl = pfile_proc_label_get(proc);
      code = label_code_get(lbl);
      if (code) {
        unsigned task_depth;

        task_depth = pic_stack_depth_get_user(pf, code, pcl, 0);
        if (task_depth > depth) {
          depth = task_depth;
        }
      }
    }
  }
  jal_pic_stk_depth_before_isr = depth;
  if (depth != PIC_STACK_DEPTH_INFINITE) {
    lbl = pfile_isr_entry_get(pf);
    if (lbl) {
      /*
       * we must assume an ISR can trigger at the deepest stack
       * depth
       */
      pic_code_t code;

      for (code = pic_code_list_head_get(pf);
           code && (4 != pic_code_pc_get(code));
           code = pic_code_next_get(code))
        ;
      if (code) {
        depth = pic_stack_depth_get_user(pf, (pic_code_t) label_code_get(lbl),
            pcl, depth + 1);
      }
      label_release(lbl);
    }
  }
  jal_pic_stk_depth_total = depth;
  value_release(pcl);
  return depth;
}
