/************************************************************
 **
 ** pic_gop.h : PIC general optimization declarations
 **
 ** Copyright (c) 2010, Kyle A. York
 **               2021, Rob Jansen
 **
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: minor fork edits
 **
 ************************************************************/
#ifndef pic_gop_h__
#define pic_gop_h__

#include "../libcore/pfile.h"
#include "pic_code.h"

/* This is the maximum depth the optimizer should reach! 
   It is an artbitrary choice. Changed from 128 to 256 in 
   jalv25r5, fixing issue #19. */
#define PIC_CODE_GOP_DEPTH 256

typedef enum {
  PIC_CODE_GOP_WHICH_ENTER, /* analyze fn entered */
  PIC_CODE_GOP_WHICH_EXIT,  /* analyze fn exited  */
  PIC_CODE_GOP_WHICH_PRE,   /* pre-code work      */
  PIC_CODE_GOP_WHICH_POST   /* post-code work     */
} pic_code_gop_which_t;

typedef enum {
  PIC_CODE_GOP_RC_CONTINUE,
  PIC_CODE_GOP_RC_STOP
} pic_code_gop_rc_t;

typedef pic_code_gop_rc_t (pic_code_gop_analyze_cb_t)(
    pfile_t             *pf,
    pic_code_t           code,
    pic_code_gop_which_t which,
    void                *data);

typedef enum pic_code_gop_who_ {
  PIC_CODE_GOP_WHO_ISR,
  PIC_CODE_GOP_WHO_USER,
  PIC_CODE_GOP_WHO_INDIRECT
} pic_code_gop_who_t;

typedef void (pic_code_gop_optimize_cb_t)(
    pfile_t             *pf, 
    pic_code_gop_who_t   who,
    void                *data);

void pic_code_gop_optimize(
  pfile_t                   *pf,
  pic_code_gop_optimize_cb_t optimize_cb,
  pic_code_gop_analyze_cb_t  analyze_cb,
  void                      *data
  );

/* One step of the same CFG the gop walk uses: (code, pv) -> up to 3
 * successors, where pv is "prev insn was a skip/conditional" (gop
 * pv_code_is_cond). GOTO/CALL/RCALL and PIC18 BRA / BC..BZ share the
 * branch+fall-through pattern (BRA also matches unconditional GOTO).
 * Used by BSR worklist. */
typedef struct pic_bsr_gop_succ_ {
  pic_code_t  c;
  boolean_t  pv;
} pic_bsr_gop_succ_t;

void pic_bsr_gop_state_succ(
  pfile_t          *pf,
  pic_code_t        code,
  boolean_t         pv_in,
  pic_bsr_gop_succ_t o[3],
  unsigned         *nout
  );

#endif /* pic_gop_h__ */

