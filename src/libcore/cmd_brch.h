/************************************************************
 **
 ** cmd_brch.h : cmd branch declarations
 **
 ** Copyright (c) 2005, Kyle A. York
 ** All rights reserved
 **
 ************************************************************/
#ifndef cmd_brch_h__
#define cmd_brch_h__

#include "cmd.h"

/* APPEND-ONLY: new members go at the END. Every addition must also add a
 * case to the dump switch (cmd_brch.c), the cmd_analyze fall-through group
 * (cmd_optm.c), and each back end's dispatch (pic.c has two switches incl.
 * an assert(0) site; mips_op.c / esp_op.c) -- a missed switch aborts the
 * compiler instead of erroring cleanly. */
typedef enum {
  CMD_BRANCHTYPE_NONE,
  CMD_BRANCHTYPE_GOTO,
  CMD_BRANCHTYPE_CALL,
  CMD_BRANCHTYPE_RETURN,
  CMD_BRANCHTYPE_TASK_START,
  CMD_BRANCHTYPE_TASK_END,
  CMD_BRANCHTYPE_TASK_YIELD,
  CMD_BRANCHTYPE_TASK_SUSPEND,
  CMD_BRANCHTYPE_TASK_RESUME,
  CMD_BRANCHTYPE_TASK_KILL,
  CMD_BRANCHTYPE_TASK_RESTART
} cmd_branchtype_t;

typedef enum {
  CMD_BRANCHCOND_NONE, /* unconditional */
  CMD_BRANCHCOND_FALSE,
  CMD_BRANCHCOND_TRUE
} cmd_branchcond_t;

cmd_t cmd_branch_alloc(
  cmd_branchtype_t type, cmd_branchcond_t cond, label_t dst, 
  value_t var, value_t proc, value_t *proc_params);

/* branch operations */
cmd_branchtype_t cmd_brtype_get(const cmd_t cmd);
void             cmd_brtype_set(cmd_t cmd, cmd_branchtype_t brtype);

cmd_branchcond_t cmd_brcond_get(const cmd_t cmd);
void             cmd_brcond_set(cmd_t cmd, cmd_branchcond_t brcond);

label_t cmd_brdst_get(const cmd_t cmd);
void     cmd_brdst_set(cmd_t cmd, label_t dst);

value_t cmd_brval_get(const cmd_t cmd);
void    cmd_brval_set(cmd_t cmd, value_t val);

value_t       cmd_brproc_get(const cmd_t cmd);
void          cmd_brproc_set(cmd_t cmd, value_t proc);
value_t      *cmd_brproc_params_get(const cmd_t cmd);
size_t        cmd_brproc_param_ct_get(const cmd_t cmd);

value_t       cmd_brproc_param_get(const cmd_t cmd, size_t n);
void          cmd_brproc_param_set(cmd_t cmd, size_t n, value_t val);

#endif /* cmd_brch_h__ */

