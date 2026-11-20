/************************************************************
 **
 ** cmd_brch.h : cmd branch declarations
 **
 ** Copyright (c) 2005, Kyle A. York
 ** All rights reserved
 **
 ** Original Author: Kyle A. York
 ** Author: Matthew Schinkel, 2026
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

/* U3 (Phase 1): an open-array (actual -> formal) pair ATTACHED to the CALL
 * command that forms it.  It replaces the global (actual, formal) registry
 * (docs/investigations/u3_registry_boundary_20261109.md §1d): the record
 * site is in pass 1, the capture and the call-site descriptor store are in
 * pass 2, and the CALL command is the one thing both passes share.  The
 * captured layout lives in the pair because the descriptor store emits it
 * for the call being emitted.
 *
 * The chunk width is a caller-side bound: libpic12 asserts it equals
 * PIC_CHUNK_MAP_MAX (the classic runtime-map width). */
#define CMD_BRANCH_PAIR_CHUNK_MAX 8

#define CMD_BRANCH_PAIR_FLAG_PASSON   0x0001U
#define CMD_BRANCH_PAIR_FLAG_STRVIEW  0x0002U
#define CMD_BRANCH_PAIR_FLAG_REPORTED 0x0004U
#define CMD_BRANCH_PAIR_FLAG_OVER_CAP 0x0008U

typedef struct cmd_branch_pair_ {
  variable_t      actual;
  variable_t      formal;
  flag_t          flags;
  /* layout of the actual captured after placement (a linear actual is
   * normalized to one chunk) */
  variable_ct_t   chunk_ct;
  variable_sz_t   chunk_sz[CMD_BRANCH_PAIR_CHUNK_MAX];
  variable_base_t chunk_base[CMD_BRANCH_PAIR_CHUNK_MAX];
  /* pass-on pair: the source formal's runtime map copied per call */
  variable_t      chain_src;
  variable_sz_t   chain_sz;
} cmd_branch_pair_t;

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

/* U3: the call's open-array pair list.  A pair is added at most once per
 * (actual, formal); `cmd_brpair_add` returns BOOLEAN_FALSE on ENOMEM (the
 * caller makes that fatal before any emission). */
cmd_branch_pair_t *cmd_brpairs_get(const cmd_t cmd);
size_t             cmd_brpairs_ct_get(const cmd_t cmd);
cmd_branch_pair_t *cmd_brpair_find(const cmd_t cmd, variable_t actual,
    variable_t formal);
boolean_t          cmd_brpair_add(cmd_t cmd, variable_t actual,
    variable_t formal, flag_t flags);
/* test-only fault hook (JALV2_U3_PAIR_FAULT=drop): remove the last pair so
 * the non-vacuous comparator is proven to fail on a dropped attachment */
void               cmd_brpair_drop_last(cmd_t cmd);

#endif /* cmd_brch_h__ */

