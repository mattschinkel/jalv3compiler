/************************************************************
 **
 ** picbsrop.h : PIC data optimization declarations
 **
 ** Copyright (c) 2007, Kyle A. York
 ** All rights reserved
 **
 ************************************************************/
#ifndef picbsrop_h__
#define picbsrop_h__

#include "pic.h"

void pic_code_bsr_optimize(pfile_t *pf);
void pic_code_bsr_remove(pfile_t *pf);

#endif

/* A4 teardown between compiles. */
void pic_bsr_state_reset(void);
 /* picbsrop_h__ */


