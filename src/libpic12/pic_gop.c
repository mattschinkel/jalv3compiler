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
#include "../libcore/pf_proc.h"
#include "../libcore/pfile.h"
#include "pic_msg.h"
#include "piccolst.h"
#include "pic.h"
#include "pic_gop.h"

/* Safety cap: backward GOTO or unusual CFG can otherwise loop the do-while. */
#define PIC_GOP_WALK_MAX 5000000U

static void pic_code_gop_analyze(
    pfile_t                   *pf,
    pic_code_t                 code,
    pic_code_gop_analyze_cb_t  analyze_fn,
    void                      *data
    )
{
  boolean_t pv_code_is_cond;
  unsigned  walkct;

  pv_code_is_cond = BOOLEAN_FALSE;
  walkct   = 0U;
  analyze_fn(pf, PIC_CODE_NONE, PIC_CODE_GOP_WHICH_ENTER, data);
  do {
    boolean_t  code_is_cond;
    pic_code_t next;

    if (++walkct > PIC_GOP_WALK_MAX) {
      pfile_log(pf, PFILE_LOG_WARN, "GOP: walk cap exceeded; optimization may be partial");
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
                  pic_code_gop_analyze(pf, brcode, analyze_fn, data);
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
        if (pic_code_modifies_pcl(pf, code)) {
          next = PIC_CODE_NONE;
        }
        switch (analyze_fn(pf, code, PIC_CODE_GOP_WHICH_POST, data)) {
          case PIC_CODE_GOP_RC_CONTINUE:
            break;
          case PIC_CODE_GOP_RC_STOP:
            next = PIC_CODE_NONE;
            break;
        }
        break;
    }
    pv_code_is_cond = code_is_cond;
    code = next;
  } while (code);
  analyze_fn(pf, PIC_CODE_NONE, PIC_CODE_GOP_WHICH_EXIT, data);
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
