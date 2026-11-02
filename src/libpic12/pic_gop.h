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

/*
 * The optimizer's per-walk state used to be a fixed
 * `[PIC_CODE_GOP_DEPTH]` array (256, "an arbitrary choice", raised from
 * 128 in jalv25r5 for issue #19).  That was a crash waiting for a big
 * program: the walk descends once per branch target, so ~2 000 sequential
 * IF statements overflowed the array and tripped an assertion in the
 * analyze callbacks (pic_daop/pic_brop/picmovlpop).  The state is now a
 * growable stack and the walk itself is iterative (no C recursion), so
 * both the array depth and the walk depth are bounded only by memory.
 */
#define PIC_CODE_GOP_STACK_INIT 64U

/* growable stack of analysis states, one element per walk level */
typedef struct pic_gop_stack_ {
  void    *tab;    /* element storage */
  size_t   el_sz;  /* sizeof one element */
  unsigned cap;    /* elements allocated */
  unsigned ix;     /* current element, PIC_GOP_STACK_NONE when empty */
} pic_gop_stack_t;

#define PIC_GOP_STACK_NONE ((unsigned) -1)

void      pic_gop_stack_init(pic_gop_stack_t *st, size_t el_sz);
void      pic_gop_stack_free(pic_gop_stack_t *st);
/* discard the contents (keeps the allocation); the base state is then
 * written through pic_gop_stack_base() before the walk starts */
void      pic_gop_stack_reset(pic_gop_stack_t *st);
void     *pic_gop_stack_base(pic_gop_stack_t *st);   /* element 0 */
void     *pic_gop_stack_top(const pic_gop_stack_t *st); /* NULL when empty */
boolean_t pic_gop_stack_push(pic_gop_stack_t *st);   /* FALSE on OOM */
void      pic_gop_stack_pop(pic_gop_stack_t *st);

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

