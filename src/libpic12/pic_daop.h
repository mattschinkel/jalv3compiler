/************************************************************
 **
 ** pic_daop.h : PIC data optimization declarations
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: minor fork edits
 **
 ************************************************************/
#ifndef pic_daop_h__
#define pic_daop_h__

#include "pic.h"
#include "pic_gop.h"

pic_code_gop_rc_t pic_code_daop_analyze(pfile_t *pf, pic_code_t code,
    pic_code_gop_which_t which, void *odata);

void pic_code_databits_optimize(pfile_t *pf);
void pic_code_databits_remove(pfile_t *pf);

#endif /* pic_daop_h__ */


