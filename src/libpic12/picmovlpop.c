/************************************************************
 **
 ** picmovlpop.c : PIC data optimization definitions
 **
 ** Copyright (c) 2010-2010, Kyle A. York
 **               2020-2020, Rob Jansen
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: minor fork edits
 **
 ************************************************************/
#include <assert.h>
#include <string.h>
#include "../libutils/mem.h"
#include "../libcore/pf_proc.h"
#include "pic_msg.h"
#include "pic_inst.h"
#include "piccolst.h"
#include "pic_gop.h"
#include "pic_brop.h"
#include "picmovlpop.h"

typedef struct pic_code_movlp_data_ {
  uchar    pclath[PIC_CODE_GOP_DEPTH];
  unsigned ix;
  unsigned changed;
} pic_code_pclath_data_t;

static pic_code_gop_rc_t pic_code_pclath_analyze(pfile_t *pf, pic_code_t code,
    pic_code_gop_which_t which, void *odata)
{
  pic_code_pclath_data_t *data;
  pic_code_gop_rc_t       rc;
  uchar                   pclath;
  pic_pclath_state_t      cpclath_state;

  data = odata;
  if (PIC_CODE_NONE != code) {
    pclath = data->pclath[data->ix];
    pic_code_pclath_get(code, &cpclath_state);
  } else {
    pclath = PIC_PCLATH_UNKNOWN;
    cpclath_state.before = PIC_PCLATH_UNKNOWN;
    cpclath_state.action = PIC_PCLATH_UNKNOWN;
  }
  rc = PIC_CODE_GOP_RC_CONTINUE;

  switch (which) {
    case PIC_CODE_GOP_WHICH_ENTER:
      data->ix++;
      assert(data->ix < PIC_CODE_GOP_DEPTH);
      if (data->ix > 0) {
        data->pclath[data->ix] = data->pclath[data->ix - 1];
      }
      break;
    case PIC_CODE_GOP_WHICH_EXIT:
      data->ix--;
      break;
    case PIC_CODE_GOP_WHICH_PRE:  
      if (cpclath_state.before == pclath) {
        /* this is what we expect, no change here */
        rc = PIC_CODE_GOP_RC_STOP;
      } else {
        pic_opcode_t op;

        if (PIC_PCLATH_UNKNOWN != cpclath_state.before) {
          if (cpclath_state.before != pclath) {
            pclath = PIC_PCLATH_INDETERMINATE;
          }
        }
        cpclath_state.before = pclath;
        op = pic_code_op_get(code);

        if ((PIC_OPCODE_MOVLP == op)
          || (PIC_OPCODE_MOVLP_NOP == op)) {
          pic_code_t   brcode;
          pic_opcode_t brop;
          label_t      lbl;
          pic_pc_t     brlbl_pc;

          brcode = code;
          lbl = pic_code_brdst_get(brcode);
          brlbl_pc = (pic_pc_t) label_pc_get(lbl);

          brop = (((brlbl_pc >> 8) == pclath) 
              && !pic_code_flag_test(code, PIC_CODE_FLAG_NO_OPTIMIZE))
              ? PIC_OPCODE_MOVLP_NOP : PIC_OPCODE_MOVLP;
          pclath = brlbl_pc >> 8;

          if (PIC_OPCODE_MOVLP_NOP == op) {
            /*
             * we've gone from NOP --> op, so make sure this can
             * never transition back to NOP
             */
            if (brop == PIC_OPCODE_MOVLP) {
              pic_code_flag_set(code, PIC_CODE_FLAG_MOVLP_LOCK);
            }
          }

          if (brop != op) {
            if (!pic_code_flag_test(code, PIC_CODE_FLAG_MOVLP_LOCK)
              || (PIC_OPCODE_MOVLP == brop)) {
              pfile_log(pf, PFILE_LOG_DEBUG, "%04x(%s:%04x): change %s to %s",
                  (unsigned) pic_code_pc_get(code),
                  label_name_get(lbl),
                  (unsigned) brlbl_pc,
                  pic_opcode_str(op), pic_opcode_str(brop));
              op = brop;
              pic_code_op_set(code, brop);
              data->changed++;
            }
          }
        }

        cpclath_state.action = pclath;
        pic_code_pclath_set(code, &cpclath_state);

      }
      break;

    case PIC_CODE_GOP_WHICH_POST:
      if (pic_code_is_suspend(pf, code)
        || (PIC_OPCODE_CALL == pic_code_op_get(code))) {
        pclath = PIC_PCLATH_INDETERMINATE;
      }
      break;
  }
  if (PIC_CODE_NONE != code) {
    data->pclath[data->ix] = pclath;
  }
  return rc;
}

static void pic_code_pclath_optimize0(pfile_t *pf,
    pic_code_gop_who_t who, void *odata)
{
  pic_code_pclath_data_t *data;

  UNUSED(pf);
  data = odata;
  switch (who) {
    case PIC_CODE_GOP_WHO_ISR:
    case PIC_CODE_GOP_WHO_USER:
      data->pclath[0] = 0;
      break;
    case PIC_CODE_GOP_WHO_INDIRECT:
      data->pclath[0] = PIC_PCLATH_INDETERMINATE;
      break;
  }
  data->ix = (unsigned) -1;
}

/* this returns TRUE if a label has crossed as code boundary. in that
   case the entire analyze phase must be done again! 
   
   pre_add is TRUE if one of the branch-bits transitioned from NOP to SET/CLR
   once this becomes TRUE it remains TRUE and no other branchbits will be
   removed
*/
boolean_t pic_code_pclath_post_analyze(pfile_t *pf)
{
  pic_code_t code;
  unsigned   ins_ct; /* instruction ct */
  unsigned   rem_ct;
  boolean_t  lbl_fix;
  unsigned   lbl_ct;
  unsigned   lbl_ix;
  pic_pc_t  *lbl_hi_was;

  lbl_fix = BOOLEAN_FALSE;
  pic_branchbits_pc_set(pf, PIC_BRANCHBITS_PC_SET_FLAG_NONE);

  lbl_ct = 0;
  for (code = pic_code_list_head_get(pf);
       code;
       code = pic_code_next_get(code)) {
    if (pic_code_label_get(code)) {
      lbl_ct++;
    }
  }
  lbl_hi_was = (pic_pc_t *) MALLOC(sizeof(*lbl_hi_was) * (lbl_ct ? lbl_ct : 1));
  if (!lbl_hi_was) {
    pfile_log(pf, PFILE_LOG_ERR, "pic_code_pclath_post_analyze: out of memory");
    return BOOLEAN_FALSE;
  }
  lbl_ix = 0;
  for (code = pic_code_list_head_get(pf);
       code;
       code = pic_code_next_get(code)) {
    if (pic_code_label_get(code)) {
      label_t lbl;

      lbl = pic_code_label_get(code);
      lbl_hi_was[lbl_ix++] =
          (pic_pc_t) ((unsigned) label_pc_get(lbl) & ~0xffU);
    }
  }

  for (ins_ct = 0, rem_ct = 0, code = pic_code_list_head_get(pf);
       code;
       ins_ct++) {
    pic_code_t next;

    next = pic_code_next_get(code);
    if (!pic_code_flag_test(code, PIC_CODE_FLAG_NO_OPTIMIZE)) {
      pic_pclath_state_t cpclath_state;

      pic_code_pclath_get(code, &cpclath_state);

      if ((PIC_OPCODE_MOVLP == pic_code_op_get(code))
        && (cpclath_state.before == cpclath_state.action)
        && !pic_code_flag_test(code, PIC_CODE_FLAG_MOVLP_LOCK)) {
        pic_code_op_set(code, PIC_OPCODE_MOVLP_NOP);
        rem_ct++;
      }
    }
    code = next;
  }

  pic_branchbits_pc_set(pf, PIC_BRANCHBITS_PC_SET_FLAG_NONE);

  lbl_ix = 0;
  for (code = pic_code_list_head_get(pf);
       code;
       code = pic_code_next_get(code)) {
    if (pic_code_label_get(code)) {
      label_t  lbl;
      pic_pc_t newhi;

      lbl = pic_code_label_get(code);
      newhi = (pic_pc_t) ((unsigned) label_pc_get(lbl) & ~0xffU);
      if (lbl_hi_was[lbl_ix++] != newhi) {
        pfile_log(pf, PFILE_LOG_DEBUG,
            "...%s MOVLP page changed (was 0x%04x now 0x%04x) %p",
            label_name_get(lbl),
            (unsigned) lbl_hi_was[lbl_ix - 1],
            (unsigned) newhi,
            (void *) lbl);
        lbl_fix = BOOLEAN_TRUE;
      }
    }
  }
  FREE(lbl_hi_was);

  pfile_log(pf, PFILE_LOG_DEBUG, "...removed %u of %u instructions", 
      rem_ct, ins_ct);
  pic_branchbits_pc_set(pf, PIC_BRANCHBITS_PC_SET_FLAG_CHECK);
  return lbl_fix;
}

/* i must set all branchbits to unknown before calling analyze */
static void pic_code_pclath_pre_analyze(pfile_t *pf)
{
  pic_code_t         code;
  pic_pclath_state_t brstate;
  uchar              pclath;

  pclath         = PIC_PCLATH_UNKNOWN;
  brstate.before = pclath;
  brstate.action = pclath;
  for (code = pic_code_list_head_get(pf);
       code;
       code = pic_code_next_get(code)) {
    pic_code_pclath_set(code, &brstate);
  }
}

/*
 * when all else is done, remove any 'movlp_nop' pseudo-instructions
 */
static void pic_code_movlp_nop_remove(pfile_t *pf)
{
  pic_code_t code;
  pic_code_t next;
  unsigned   rem_ct;

  for (code = pic_code_list_head_get(pf), rem_ct = 0;
       code;
       code = next) {
    next = pic_code_next_get(code);
    if (!pic_code_flag_test(code, PIC_CODE_FLAG_NO_OPTIMIZE)
      && (PIC_OPCODE_MOVLP_NOP == pic_code_op_get(code))) {
      pic_code_list_remove(pf, code);
      pic_code_free(code);
      rem_ct++;
    }
  }
  pfile_log(pf, PFILE_LOG_DEBUG, "removed %u movlp_nop", rem_ct);
}

void pic_code_movlp_optimize(pfile_t *pf)
{
  unsigned               pass;
  pic_code_pclath_data_t data;

  pass = 0;
  pic_branchbits_pc_set(pf, PIC_BRANCHBITS_PC_SET_FLAG_NONE);
  do {
    data.changed = 0;
    pfile_log(pf, PFILE_LOG_DEBUG, PIC_MSG_FIXING_PCLATH, ++pass);
    pic_code_pclath_pre_analyze(pf);
    pic_code_gop_optimize(pf, pic_code_pclath_optimize0,
      pic_code_pclath_analyze, &data);
    if (data.changed) {
      pic_branchbits_pc_set(pf, PIC_BRANCHBITS_PC_SET_FLAG_NONE);
    }
  } while (pic_code_pclath_post_analyze(pf) || data.changed);
  pic_code_movlp_nop_remove(pf);
}

