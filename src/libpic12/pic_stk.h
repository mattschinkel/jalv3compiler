/************************************************************
 **
 ** pic_stk.c : determine PIC stack usage declarations
 **
 ** Copyright (c) 2005, Kyle A. York
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: minor fork edits
 **
 ************************************************************/
#ifndef pic_stk_h__
#define pic_stk_h__

#include "../libcore/pfile.h"

unsigned pic_stack_depth_get(pfile_t *pf);

/* Last values from the most recent pic_stack_depth_get() (for diagnostics). */
void pic_stack_depth_diag_get(unsigned *before_isr, unsigned *total);

#endif /* pic_stk_h__ */


