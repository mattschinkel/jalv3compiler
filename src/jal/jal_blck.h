/************************************************************
 **
 ** jal_blck.h : JAL block processing declarations
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 ** All rights reserved
 **
 ** Original Author: Kyle A. York
 ** Author: Matthew Schinkel, 2026
 **
 ************************************************************/

#ifndef jal_blck_h__
#define jal_blck_h__

#include "../libcore/pfile.h"

#define JAL_BLOCK_PROCESS_FLAG_NONE      0x0000
#define JAL_BLOCK_PROCESS_FLAG_NO_BLOCK  0x0001 /* don't wrap block/eob */

/* bound on nested statement blocks (see jal_blck.c for the rationale) */
#define JAL_BLOCK_DEPTH_MAX 1024U

boolean_t jal_statement_process(pfile_t *pf);
void jal_block_process(pfile_t *pf, flag_t flags);
void jal_file_process(pfile_t *pf, char *pre_inc);

void jal_block_start_show(pfile_t *pf, const char *tag,
  const pfile_pos_t *pos);

/* statement-nesting cap (see jal_blck.c): once the limit fires the rest of
 * the input is consumed and every enclosing construct must stop looking
 * for its own terminator */
boolean_t jal_parse_aborted(pfile_t *pf);
void      jal_parse_abort(pfile_t *pf);
void      jal_block_state_reset(void);

/* canonical pointer-to-function(byte in) return byte def (stage-2
 * runtime dispatch for pseudo-marked pointer arrays) */
variable_def_t jal_pointer_fnptr_def_get(pfile_t *pf);
/* stage-2 runtime dispatch master switch (FALSE until the slot-value
 * indirect-call emission is wired through the fps[i](x) machinery) */
boolean_t jal_pointer_slot_rt_enabled(void);

/* defer a string subscript load (base + index) to the post-parse
 * semantic lowering pass */
void jal_sem_str_load_emit(pfile_t *pf, value_t dst, value_t base,
    value_t idx);
/* defer an RVALUE read of a formal subscript; returns the read's temp */
value_t jal_sem_str_load_emit_formal(pfile_t *pf, value_t val);

/* is VAL a 'put/'get accessor of the procedure being parsed?  (its body must
 * not resolve its own name back to itself -- that is a self-call) */
boolean_t jal_sem_accessor_self_call(pfile_t *pf, value_t val);

/* phase 2: defer COUNT(formal) to the post-parse lowering pass */
boolean_t jal_sem_count_wanted(pfile_t *pf, value_t val);
void      jal_sem_count_emit(pfile_t *pf, value_t dst, value_t src);

/* is VAL a whole formal of the active procedure?  (a semantic node that
 * names one is re-lowered with that procedure's formal representation) */
boolean_t jal_sem_formal_value_p(pfile_t *pf, value_t val);
/* a whole formal whose representation a clone can change (pointer/array) */
boolean_t jal_sem_formal_reflavor_p(pfile_t *pf, value_t val);

/* phase 2: defer a whole-string relation (== / !=) to the lowering pass */
void jal_sem_relation_emit(pfile_t *pf, value_t dst, value_t lhs,
    value_t rhs, operator_t op);

#endif /* jal_blck_h__ */
