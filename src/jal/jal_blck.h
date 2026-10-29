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

boolean_t jal_statement_process(pfile_t *pf);
void jal_block_process(pfile_t *pf, flag_t flags);
void jal_file_process(pfile_t *pf, char *pre_inc);

void jal_block_start_show(pfile_t *pf, const char *tag,
  const pfile_pos_t *pos);

#endif /* jal_blck_h__ */




/* canonical pointer-to-function(byte in) return byte def (stage-2
 * runtime dispatch for pseudo-marked pointer arrays) */
variable_def_t jal_pointer_fnptr_def_get(pfile_t *pf);
/* stage-2 runtime dispatch master switch (FALSE until the slot-value
 * indirect-call emission is wired through the fps[i](x) machinery) */
boolean_t jal_pointer_slot_rt_enabled(void);
