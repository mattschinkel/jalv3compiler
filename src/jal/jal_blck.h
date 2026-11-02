/************************************************************
 **
 ** jal_blck.h : JAL block processing declarations
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 ** All rights reserved
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

#endif /* jal_blck_h__ */
