/**********************************************************
 **
 ** cmd_asm.h : declarations for cmd_asm_t
 **
 ** Copyright (c) 2005, Kyle A. York
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: opcode made opaque; the classic-PIC semantics live in
 **   libpic12/pic_asmcmd.c and are registered via cmd_asm_semantics_set
 **
 ***********************************************************/
#ifndef cmd_asm_h__
#define cmd_asm_h__

#include <stdio.h>
#include "value.h"
#include "cmd.h"

/* The inline-assembly command stores the back-end's opcode vocabulary
 * OPAGUELY: libcore allocates, moves and compares it, while libpic12
 * defines it (pic_opco.h) and owns its semantics (pic_asmcmd.c,
 * registered below -- wired by jal_main like the back-end registry).
 * cmd_asm_op_t must stay wide enough for pic_opcode_t. */
typedef unsigned cmd_asm_op_t;    /* pic_opcode_t  */
typedef unsigned cmd_asm_opdst_t; /* pic_opdst_t   */

cmd_t cmd_asm_alloc(
    cmd_asm_op_t op, value_t val, size_t valofs, cmd_asm_opdst_t opdst,
    value_t n, label_t lbl, flag_t flags, size_t data_sz, uchar *data);

cmd_asm_op_t  cmd_asm_op_get(const cmd_t cmd);
void          cmd_asm_op_set(cmd_t cmd, cmd_asm_op_t op);

value_t      cmd_asm_val_get(const cmd_t cmd);
void         cmd_asm_val_set(cmd_t cmd, value_t val);

size_t       cmd_asm_valofs_get(const cmd_t cmd);
void         cmd_asm_valofs_set(cmd_t cmd, size_t val);

cmd_asm_opdst_t cmd_asm_opdst_get(const cmd_t cmd);
void            cmd_asm_opdst_set(cmd_t cmd, cmd_asm_opdst_t dst);

value_t      cmd_asm_n_get(const cmd_t cmd);
void         cmd_asm_n_set(cmd_t cmd, value_t n);

label_t      cmd_asm_lbl_get(const cmd_t cmd);
void         cmd_asm_lbl_set(cmd_t cmd, label_t lbl);

boolean_t    cmd_asm_flag_test(const cmd_t cmd, flag_t flag);
flag_t       cmd_asm_flag_get_all(const cmd_t cmd);

size_t       cmd_asm_data_sz_get(const cmd_t cmd);
uchar       *cmd_asm_data_get(const cmd_t cmd);
void         cmd_asm_data_set(cmd_t cmd, size_t sz, uchar *data);

/* Back-end semantics for asm commands: dump rendering, use/def
 * classification, control-flow successors, and the two properties the
 * shared optimizer asks about. Registered once at startup; a NULL slot
 * is a no-op (and the predicates answer FALSE) when nothing is
 * registered. */
typedef struct cmd_asm_semantics_ {
  void               (*dump)(cmd_t cmd, FILE *dst);
  void               (*assigned_use_set)(cmd_t cmd);
  cmd_successor_rc_t (*successor_get)(cmd_t cmd, size_t ix, cmd_t *dst);
  boolean_t          (*flow_terminator_p)(cmd_asm_op_t op);
  boolean_t          (*cond_skip_p)(cmd_asm_op_t op);
  /* vocabulary values libcore needs as accessor defaults for non-asm
   * commands (PIC_OPCODE_NONE / PIC_OPDST_NONE) */
  cmd_asm_op_t        op_none;
  cmd_asm_opdst_t     opdst_none;
} cmd_asm_semantics_t;

void      cmd_asm_semantics_set(const cmd_asm_semantics_t *sem);
boolean_t cmd_asm_op_is_flow_terminator(cmd_asm_op_t op);
boolean_t cmd_asm_op_is_cond_skip(cmd_asm_op_t op);

#endif /* cmd_asm_h__ */
