/************************************************************
 **
 ** cmd_asmd.h : inline assembly bits
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: opcode/opdst are the opaque cmd_asm types (cmd_asm.h) --
 **   no back-end header is included from libcore anymore
 **
 ************************************************************/
#ifndef cmdasmd_h__
#define cmdasmd_h__

#include "cmd_asm.h"

typedef struct cmd_asm_ {
  cmd_asm_op_t    op;
  cmd_asm_opdst_t opdst;
  value_t         val;
  size_t          valofs;
  value_t         n;
  label_t         lbl;
  flag_t          flags;
  size_t          data_sz;
  uchar          *data;
} cmd_asm_t;

#endif /*cmdasmd_h__ */
