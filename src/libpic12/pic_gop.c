/************************************************************
 **
 ** pic_gop.c : PIC general optimization definitions
 **
 ** Copyright (c) 2010, Kyle A. York
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: fork extensions (heavy rework)
 **
 ************************************************************/
#include <errno.h>
#include <string.h>
#include "../libutils/mem.h"
#include "../libcore/pf_proc.h"
#include "../libcore/pfile.h"
#include "pic_msg.h"
#include "piccolst.h"
#include "pic.h"
#include "pic_gop.h"

/* Safety cap: backward GOTO or unusual CFG can otherwise loop the do-while. */
#define PIC_GOP_WALK_MAX 5000000U

/*
 * Growable analysis-state stack.
 *
 * The three analyze callbacks (databits, branchbits, pclath) keep one state
 * element per walk level.  They used to declare a fixed
 * `[PIC_CODE_GOP_DEPTH]` array; a program with enough sequential branches
 * walked deeper than 256 and the ENTER handler tripped
 * `assert(data->ix < PIC_CODE_GOP_DEPTH)`.  Growth is now the stack's job,
 * so no legal program can overflow it (only memory can).
 */
static void *pic_gop_stack_el(const pic_gop_stack_t *st, unsigned ix)
{
  return (void *) ((char *) st->tab + ((size_t) ix) * st->el_sz);
}

static boolean_t pic_gop_stack_ensure(pic_gop_stack_t *st, unsigned ct)
{
  boolean_t rc;

  rc = BOOLEAN_TRUE;
  if (ct > st->cap) {
    unsigned ncap;
    void    *ntab;

    ncap = (st->cap) ? st->cap : PIC_CODE_GOP_STACK_INIT;
    while (ncap < ct) {
      ncap *= 2U;
    }
    ntab = REALLOC(st->tab, ((size_t) ncap) * st->el_sz);
    if (!ntab) {
      rc = BOOLEAN_FALSE;
    } else {
      st->tab = ntab;
      st->cap = ncap;
    }
  }
  return rc;
}

void pic_gop_stack_init(pic_gop_stack_t *st, size_t el_sz)
{
  st->tab   = 0;
  st->el_sz = el_sz;
  st->cap   = 0U;
  st->ix    = PIC_GOP_STACK_NONE;
}

void pic_gop_stack_free(pic_gop_stack_t *st)
{
  FREE(st->tab);
  st->tab = 0;
  st->cap = 0U;
  st->ix  = PIC_GOP_STACK_NONE;
}

void pic_gop_stack_reset(pic_gop_stack_t *st)
{
  st->ix = PIC_GOP_STACK_NONE;
}

void *pic_gop_stack_base(pic_gop_stack_t *st)
{
  return pic_gop_stack_ensure(st, 1U) ? pic_gop_stack_el(st, 0U) : 0;
}

void *pic_gop_stack_top(const pic_gop_stack_t *st)
{
  return (PIC_GOP_STACK_NONE == st->ix) ? 0 : pic_gop_stack_el(st, st->ix);
}

boolean_t pic_gop_stack_push(pic_gop_stack_t *st)
{
  unsigned ix;

  ix = st->ix + 1U;   /* NONE + 1 wraps to 0: the first push is level 0 */
  if (!pic_gop_stack_ensure(st, ix + 1U)) {
    return BOOLEAN_FALSE;
  }
  if (0U != ix) {
    memcpy(pic_gop_stack_el(st, ix), pic_gop_stack_el(st, ix - 1U),
      st->el_sz);
  }
  st->ix = ix;
  return BOOLEAN_TRUE;
}

void pic_gop_stack_pop(pic_gop_stack_t *st)
{
  if (PIC_GOP_STACK_NONE != st->ix) {
    st->ix--;
  }
}

/*
 * One suspended walk level: where to resume after a branch target has been
 * walked, and the skip state that continuation starts with.
 */
typedef struct pic_gop_frame_ {
  pic_code_t cont;     /* fall-through after the branch target */
  boolean_t  pv_cond;  /* pv_code_is_cond for that continuation */
  pic_code_t code;     /* branch node whose POST tail is still pending */
} pic_gop_frame_t;

/*
 * NAME
 *   pic_gop_post
 *
 * DESCRIPTION
 *   apply the post-visit step of one instruction: a PCL write ends the
 *   sequence, and the analyze callback may stop it too
 *
 * PARAMETERS
 *   pf   : pfile
 *   code : the instruction
 *   fn   : analyze callback
 *   data : callback state
 *   next : current successor
 *
 * RETURN
 *   the successor to walk (PIC_CODE_NONE to end the sequence)
 */
static pic_code_t pic_gop_post(pfile_t *pf, pic_code_t code,
  pic_code_gop_analyze_cb_t fn, void *data, pic_code_t next)
{
  if (pic_code_modifies_pcl(pf, code)) {
    next = PIC_CODE_NONE;
  }
  switch (fn(pf, code, PIC_CODE_GOP_WHICH_POST, data)) {
    case PIC_CODE_GOP_RC_CONTINUE:
      break;
    case PIC_CODE_GOP_RC_STOP:
      next = PIC_CODE_NONE;
      break;
  }
  return next;
}

/*
 * NAME
 *   pic_code_gop_analyze
 *
 * DESCRIPTION
 *   walk the code graph, bracketing each branch target in
 *   ENTER/EXIT callbacks so the analyze functions keep a per-level state
 *
 * NOTES
 *   This used to recurse into the branch target, once per nested branch,
 *   so a long chain of IF statements put one C frame per statement on the
 *   stack.  The walk is now iterative: a branch target pushes its
 *   fall-through continuation and the loop descends; when a walk sequence
 *   ends the continuation is popped.  ENTER/EXIT are still issued for
 *   every descent, so the analyze callbacks are unchanged.
 */
static void pic_code_gop_analyze(
    pfile_t                   *pf,
    pic_code_t                 code,
    pic_code_gop_analyze_cb_t  analyze_fn,
    void                      *data
    )
{
  pic_gop_frame_t *frames;
  size_t           frame_cap;
  size_t           frame_ct;
  boolean_t        pv_code_is_cond;
  unsigned         walkct;

  frames    = 0;
  frame_cap = 0U;
  frame_ct  = 0U;
  pv_code_is_cond = BOOLEAN_FALSE;
  walkct    = 0U;

  analyze_fn(pf, PIC_CODE_NONE, PIC_CODE_GOP_WHICH_ENTER, data);
  for (;;) {
    boolean_t  code_is_cond;
    pic_code_t next;
    boolean_t  descend;
    pic_code_t descend_to;

    if (PIC_CODE_NONE == code) {
      /* this walk sequence is done: leave its level and resume the parent */
      analyze_fn(pf, PIC_CODE_NONE, PIC_CODE_GOP_WHICH_EXIT, data);
      if (0U == frame_ct) {
        break;
      }
      frame_ct--;
      /* the branch node's post-visit was deferred until its target had been
       * walked (that is the order the recursive walker used) */
      code            = pic_gop_post(pf, frames[frame_ct].code,
                          analyze_fn, data, frames[frame_ct].cont);
      pv_code_is_cond = frames[frame_ct].pv_cond;
      continue;
    }
    descend    = BOOLEAN_FALSE;
    descend_to = PIC_CODE_NONE;
    if (++walkct > PIC_GOP_WALK_MAX) {
      pfile_log(pf, PFILE_LOG_WARN,
        "GOP: walk cap exceeded; optimization may be partial");
      /* leave the current level and every suspended one */
      analyze_fn(pf, PIC_CODE_NONE, PIC_CODE_GOP_WHICH_EXIT, data);
      while (frame_ct) {
        frame_ct--;
        analyze_fn(pf, PIC_CODE_NONE, PIC_CODE_GOP_WHICH_EXIT, data);
      }
      break;
    }
    code_is_cond = BOOLEAN_FALSE;
    next = pic_code_next_get(code);

    switch (analyze_fn(pf, code, PIC_CODE_GOP_WHICH_PRE, data)) {
      case PIC_CODE_GOP_RC_STOP:
        next = PIC_CODE_NONE;
        break;
      case PIC_CODE_GOP_RC_CONTINUE:
        switch (pic_code_op_get(code)) {
          case PIC_OPCODE_RETURN:
          case PIC_OPCODE_RETFIE:
          case PIC_OPCODE_RETLW:
          case PIC_OPCODE_RESET:
            /* if this is unconditional we're done otherwise simply continue */
            if (!pv_code_is_cond) {
              next = 0;
            }
            break;
          case PIC_OPCODE_GOTO:
          case PIC_OPCODE_CALL:
          case PIC_OPCODE_RCALL:
          case PIC_OPCODE_BRA:
          case PIC_OPCODE_BC:
          case PIC_OPCODE_BN:
          case PIC_OPCODE_BNC:
          case PIC_OPCODE_BNN:
          case PIC_OPCODE_BNOV:
          case PIC_OPCODE_BNZ:
          case PIC_OPCODE_BOV:
          case PIC_OPCODE_BZ:
            {
              pic_code_t brcode;
              pic_opcode_t hop;

              hop    = pic_code_op_get(code);
              brcode = pic_code_label_find(pf, pic_code_brdst_get(code));

              if ((PIC_OPCODE_GOTO == hop || PIC_OPCODE_BRA == hop)
                  && !pv_code_is_cond
                  && !pic_code_is_suspend(pf, code)) {
                /* Unconditional branch-to-label; listing successor is not
                 * executed (same as GOTO). */
                next = brcode;
              } else {
                /* Conditional GOTO/BRA (skip edge), call/rcall, or PIC18
                 * relative conditionals: analyze branch target, then walk
                 * the fall-through successor. */
                if (brcode) {
                  if (frame_ct == frame_cap) {
                    size_t ncap;
                    void  *nf;

                    ncap = (frame_cap) ? 2U * frame_cap : 64U;
                    nf = REALLOC(frames, ncap * sizeof(*frames));
                    if (!nf) {
                      pfile_log_syserr(pf, ENOMEM);
                      break;
                    }
                    frames    = nf;
                    frame_cap = ncap;
                  }
                  frames[frame_ct].cont    = next;
                  frames[frame_ct].pv_cond = code_is_cond;
                  frames[frame_ct].code    = code;
                  frame_ct++;
                  analyze_fn(pf, PIC_CODE_NONE,
                    PIC_CODE_GOP_WHICH_ENTER, data);
                  descend    = BOOLEAN_TRUE;
                  descend_to = brcode;
                }
              }
            }
            break;
          case PIC_OPCODE_INCFSZ:
          case PIC_OPCODE_DECFSZ:
          case PIC_OPCODE_BTFSC:
          case PIC_OPCODE_BTFSS:
            code_is_cond = BOOLEAN_TRUE;
            break;
          default:
            break;
        }
        if (descend) {
          /* the branch target is walked before this node's post-visit: the
           * tail waits in the frame so unwinding applies it in the order
           * the recursive walker used */
          code            = descend_to;
          pv_code_is_cond = BOOLEAN_FALSE;
          continue;
        }
        next = pic_gop_post(pf, code, analyze_fn, data, next);
        break;
    }
    pv_code_is_cond = code_is_cond;
    code = next;
  }
  FREE(frames);
}

void pic_code_gop_optimize(
  pfile_t                   *pf,
  pic_code_gop_optimize_cb_t optimize_cb,
  pic_code_gop_analyze_cb_t  analyze_cb,
  void                      *data
  )
{
  label_t        lbl;
  pfile_proc_t  *proc;

  /* After semantic errors the pcode CFG can be incomplete or unusually deep.
   * Bounded GOP walks use a fixed stack (PIC_CODE_GOP_DEPTH); skip all such
   * passes so the user sees the recorded error, not an assertion in an
   * analyze callback (BSR / databits / branchbits / PCLATH). */
  if (0 != pfile_errct_get(pf)) {
    return;
  }

  /* first -- analyze the ISR bits */
  lbl = pfile_label_find(pf, PFILE_LOG_NONE, PIC_LABEL_ISR);
  if (lbl) {
    pic_code_t code;

    code = pic_code_label_find(pf, lbl);
    if (code) {
      optimize_cb(pf, PIC_CODE_GOP_WHO_ISR, data);
      pfile_log(pf, PFILE_LOG_DEBUG, "...analyzing ISR");
      pic_code_gop_analyze(pf, code, analyze_cb, data);
    }
    label_release(lbl);
  }

  /* second -- analyze the USER bits */
  lbl = pfile_label_find(pf, PFILE_LOG_ERR, PIC_LABEL_RESET);
  if (lbl) {
    pic_code_t code;

    code = pic_code_label_find(pf, lbl);
    if (code) {
      pfile_log(pf, PFILE_LOG_DEBUG, "...analyzing USER");
      optimize_cb(pf, PIC_CODE_GOP_WHO_USER, data);
      pic_code_gop_analyze(pf, code, analyze_cb, data);
    }
    label_release(lbl);
  }
  /* find all procedures that are called indirectly as these might not have
   * been picked up earlier
   */
  for (proc = pfile_proc_root_get(pf);
       proc;
       proc = pfile_proc_next(proc)) {
    if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INDIRECT)
        || pfile_proc_flag_test(proc, PFILE_PROC_FLAG_TASK)) {
      lbl = pfile_proc_label_get(proc);
      if (lbl) {
        pic_code_t code;

        code = (pic_code_t) label_code_get(lbl);
        if (code) {
          optimize_cb(pf, PIC_CODE_GOP_WHO_INDIRECT, data);
          pic_code_gop_analyze(pf, code, analyze_cb, data);
        }
      }
    }
  }
  /* once the analysis is done, we need to run through & make
     sure no further label fixups are needed, or fix them up
     as necessary */
  pfile_log(pf, PFILE_LOG_DEBUG, "...post analyze");
}

void pic_bsr_gop_state_succ(
  pfile_t              *pf,
  pic_code_t            code,
  boolean_t             pv_in,
  pic_bsr_gop_succ_t    o[3],
  unsigned             *nout)
{
  boolean_t  code_is_cond;
  pic_code_t next;
  unsigned   n;
  pic_opcode_t op;

  n = 0U;
  *nout = 0U;
  code_is_cond = BOOLEAN_FALSE;
  next = pic_code_next_get(code);
  op = pic_code_op_get(code);

  switch (op) {
    case PIC_OPCODE_RETURN:
    case PIC_OPCODE_RETFIE:
    case PIC_OPCODE_RETLW:
    case PIC_OPCODE_RESET:
      if (!pv_in) {
        return;
      }
      break;
    case PIC_OPCODE_GOTO:
    case PIC_OPCODE_CALL:
    case PIC_OPCODE_RCALL:
    case PIC_OPCODE_BRA:
    case PIC_OPCODE_BC:
    case PIC_OPCODE_BN:
    case PIC_OPCODE_BNC:
    case PIC_OPCODE_BNN:
    case PIC_OPCODE_BNOV:
    case PIC_OPCODE_BNZ:
    case PIC_OPCODE_BOV:
    case PIC_OPCODE_BZ:
      {
        pic_code_t br;

        br = pic_code_label_find(pf, pic_code_brdst_get(code));
        if ((PIC_OPCODE_GOTO == op || PIC_OPCODE_BRA == op) && !pv_in
            && !pic_code_is_suspend(pf, code)) {
          if ((PIC_OPCODE_GOTO == op) && pic_code_modifies_pcl(pf, code)) {
            return;
          }
          if (br) {
            o[0].c  = br;
            o[0].pv = BOOLEAN_FALSE;
            *nout   = 1U;
          }
          return;
        }
        if (br) {
          o[0].c  = br;
          o[0].pv = BOOLEAN_FALSE;
          n = 1U;
        }
      }
      break;
    case PIC_OPCODE_INCFSZ:
    case PIC_OPCODE_DECFSZ:
    case PIC_OPCODE_BTFSC:
    case PIC_OPCODE_BTFSS:
      code_is_cond = BOOLEAN_TRUE;
      break;
    default:
      break;
  }
  if (pic_code_modifies_pcl(pf, code)) {
    next = PIC_CODE_NONE;
  }
  if (next) {
    o[n].c  = next;
    o[n].pv = code_is_cond;
    n++;
  }
  *nout = n;
}
