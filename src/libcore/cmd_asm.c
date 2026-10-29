/**********************************************************
 **
 ** cmd_asm.c : manipulators for cmd_asm_t
 **
 ** Copyright (c) 2005, Kyle A. York
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: opcode made opaque; PIC semantics split to
 **   libpic12/pic_asmcmd.c (registered semantics)
 **
 ***********************************************************/
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "../libutils/mem.h"
#include "cmdd.h"
#include "cmd_asm.h"

/* The opcode-semantics slots route through hooks registered by the
 * classic back end (libpic12/pic_asmcmd.c, wired by jal_main): libcore
 * itself holds no opcode vocabulary. */
static const cmd_asm_semantics_t *cmd_asm_semantics;

void cmd_asm_semantics_set(const cmd_asm_semantics_t *sem)
{
  cmd_asm_semantics = sem;
}

boolean_t cmd_asm_op_is_flow_terminator(cmd_asm_op_t op)
{
  return (cmd_asm_semantics && cmd_asm_semantics->flow_terminator_p)
      ? cmd_asm_semantics->flow_terminator_p(op) : BOOLEAN_FALSE;
}

boolean_t cmd_asm_op_is_cond_skip(cmd_asm_op_t op)
{
  return (cmd_asm_semantics && cmd_asm_semantics->cond_skip_p)
      ? cmd_asm_semantics->cond_skip_p(op) : BOOLEAN_FALSE;
}

static void cmd_asm_dump(cmd_t cmd, FILE *dst)
{
  if (cmd_asm_semantics && cmd_asm_semantics->dump) {
    cmd_asm_semantics->dump(cmd, dst);
  }
}

static void cmd_asm_assigned_use_set(cmd_t cmd)
{
  if (cmd_asm_semantics && cmd_asm_semantics->assigned_use_set) {
    cmd_asm_semantics->assigned_use_set(cmd);
  }
}

static cmd_successor_rc_t cmd_asm_successor_get(cmd_t cmd, size_t ix,
  cmd_t *dst)
{
  return (cmd_asm_semantics && cmd_asm_semantics->successor_get)
      ? cmd_asm_semantics->successor_get(cmd, ix, dst)
      : CMD_SUCCESSOR_RC_IX_BAD;
}

static void cmd_asm_free(cmd_t cmd)
{
  cmd_asm_val_set(cmd, VALUE_NONE);
  cmd_asm_n_set(cmd, VALUE_NONE);
  cmd_asm_lbl_set(cmd, LABEL_NONE);
  cmd_asm_data_set(cmd, 0, 0);
}

static cmd_t cmd_asm_dup(const cmd_t cmd)
{
  size_t    dsz;
  uchar    *src;
  uchar    *copy;

  dsz  = cmd_asm_data_sz_get(cmd);
  src  = cmd_asm_data_get(cmd);
  copy = 0;
  if (dsz && src) {
    copy = MALLOC(dsz);
    if (copy) {
      memcpy(copy, src, dsz);
    }
  }
  return cmd_asm_alloc(cmd_asm_op_get(cmd), cmd_asm_val_get(cmd),
    cmd_asm_valofs_get(cmd),
    cmd_asm_opdst_get(cmd), cmd_asm_n_get(cmd), cmd_asm_lbl_get(cmd),
    cmd_asm_flag_get_all(cmd),
    copy ? dsz : 0,
    copy);
}

static void cmd_asm_label_remap(cmd_t cmd, const label_map_t *map)
{
  cmd_label_remap2(cmd, map, cmd_asm_lbl_get, cmd_asm_lbl_set);
}

static void cmd_asm_variable_remap(cmd_t cmd, const variable_map_t *map)
{
  cmd_variable_remap2(cmd, map, cmd_asm_val_get, cmd_asm_val_set);
  cmd_variable_remap2(cmd, map, cmd_asm_n_get, cmd_asm_n_set);
}

static void cmd_asm_value_remap(cmd_t cmd, const value_map_t *map)
{
  cmd_value_remap2(cmd, map, cmd_asm_val_get, cmd_asm_val_set);
  cmd_value_remap2(cmd, map, cmd_asm_n_get, cmd_asm_n_set);
}

static flag_t cmd_asm_variable_accessed(const cmd_t cmd, variable_t var)
{
  return ((value_variable_get(cmd_asm_val_get(cmd)) == var)
        || (value_variable_get(cmd_asm_n_get(cmd)) == var))
    ? CMD_VARIABLE_ACCESS_FLAG_READ | CMD_VARIABLE_ACCESS_FLAG_WRITTEN
    : CMD_VARIABLE_ACCESS_FLAG_NONE;
}

static flag_t cmd_asm_value_accessed(const cmd_t cmd, value_t val)
{
  return (cmd_asm_val_get(cmd) == val)
        || (cmd_asm_n_get(cmd) == val)
    ? CMD_VARIABLE_ACCESS_FLAG_READ | CMD_VARIABLE_ACCESS_FLAG_WRITTEN
    : CMD_VARIABLE_ACCESS_FLAG_NONE;
}

static const cmd_vtbl_t cmd_asm_vtbl = {
  cmd_asm_free,
  cmd_asm_dump,
  cmd_asm_dup,
  cmd_asm_label_remap,
  cmd_asm_variable_remap,
  cmd_asm_value_remap,
  cmd_asm_variable_accessed,
  cmd_asm_value_accessed,
  cmd_asm_assigned_use_set,
  cmd_asm_successor_get
};

cmd_t cmd_asm_alloc(cmd_asm_op_t op, value_t val, size_t valofs,
    cmd_asm_opdst_t opdst, value_t n, label_t lbl, flag_t flags,
    size_t data_sz, uchar *data)
{
  cmd_t cmd;

  cmd = cmd_alloc(CMD_TYPE_ASM, &cmd_asm_vtbl);
  if (cmd) {
    struct cmd_ *ptr;

    ptr = cmd_element_seek(cmd, BOOLEAN_TRUE);
    if (ptr) {
      ptr->u.op_asm.op      = op;
      ptr->u.op_asm.val     = VALUE_NONE;
      ptr->u.op_asm.valofs  = valofs;
      ptr->u.op_asm.opdst   = opdst;
      ptr->u.op_asm.n       = VALUE_NONE;
      ptr->u.op_asm.lbl     = LABEL_NONE;
      ptr->u.op_asm.flags   = flags;
      ptr->u.op_asm.data_sz = 0;
      ptr->u.op_asm.data    = 0;
      
      cmd_asm_val_set(cmd, val);
      cmd_asm_n_set(cmd, n);
      cmd_asm_lbl_set(cmd, lbl);
      cmd_asm_data_set(cmd, data_sz, data);
    }
  }
  return cmd;
}

cmd_asm_op_t cmd_asm_op_get(const cmd_t cmd)
{
  struct cmd_ *ptr;

  ptr = 0;
  if (CMD_TYPE_ASM == cmd_type_get(cmd)) {
    ptr = cmd_element_seek(cmd, BOOLEAN_FALSE);
  }
  return (ptr) ? ptr->u.op_asm.op
      : (cmd_asm_semantics ? cmd_asm_semantics->op_none : (cmd_asm_op_t) 0);
}

void cmd_asm_op_set(cmd_t cmd, cmd_asm_op_t op)
{
  assert(CMD_TYPE_ASM == cmd_type_get(cmd));
  if (CMD_TYPE_ASM == cmd_type_get(cmd)) {
    struct cmd_ *ptr;

    ptr = cmd_element_seek(cmd, BOOLEAN_TRUE);
    if (ptr) {
      ptr->u.op_asm.op = op;
    }
  }
}

value_t cmd_asm_val_get(const cmd_t cmd)
{
  const struct cmd_ *ptr;

  ptr = 0;
  if (CMD_TYPE_ASM == cmd_type_get(cmd)) {
    ptr = cmd_element_seek(cmd, BOOLEAN_FALSE);
  }
  return (ptr) ? ptr->u.op_asm.val : VALUE_NONE;
}

void cmd_asm_val_set(cmd_t cmd, value_t val)
{
  struct cmd_ *ptr;

  assert(CMD_TYPE_ASM == cmd_type_get(cmd));
  if (CMD_TYPE_ASM == cmd_type_get(cmd)) {
    ptr = cmd_element_seek(cmd, BOOLEAN_TRUE);
    if (ptr) {
      /* if a varaible is used in an assembly statement, mark it both
       * assigned *and* used */
      value_lock(val);
      value_assign_ct_bump(ptr->u.op_asm.val, CTR_BUMP_DECR);
      value_use_ct_bump(ptr->u.op_asm.val, CTR_BUMP_DECR);
      value_release(ptr->u.op_asm.val);
      ptr->u.op_asm.val = val;
      value_assign_ct_bump(val, CTR_BUMP_INCR);
      value_use_ct_bump(val, CTR_BUMP_INCR);
    }
  }
}

size_t cmd_asm_valofs_get(const cmd_t cmd)
{
  const struct cmd_ *ptr;

  ptr = 0;
  if (CMD_TYPE_ASM == cmd_type_get(cmd)) {
    ptr = cmd_element_seek(cmd, BOOLEAN_FALSE);
  }
  return (ptr) ? ptr->u.op_asm.valofs : 0;
}

void cmd_asm_valofs_set(cmd_t cmd, size_t valofs)
{

  assert(CMD_TYPE_ASM == cmd_type_get(cmd));
  if (CMD_TYPE_ASM == cmd_type_get(cmd)) {
    struct cmd_ *ptr;

    ptr = cmd_element_seek(cmd, BOOLEAN_TRUE);
    if (ptr) {
      /* if a varaible is used in an assembly statement, mark it both
       * assigned *and* used */
      ptr->u.op_asm.valofs = valofs;
    }
  }
}

cmd_asm_opdst_t  cmd_asm_opdst_get(const cmd_t cmd)
{
  const struct cmd_ *ptr;

  ptr = 0;
  if (CMD_TYPE_ASM == cmd_type_get(cmd)) {
    ptr = cmd_element_seek(cmd, BOOLEAN_FALSE);
  }
  return (ptr) ? ptr->u.op_asm.opdst
      : (cmd_asm_semantics ? cmd_asm_semantics->opdst_none
         : (cmd_asm_opdst_t) 0);
}

void cmd_asm_opdst_set(cmd_t cmd, cmd_asm_opdst_t dst)
{
  assert(CMD_TYPE_ASM == cmd_type_get(cmd));
  if (CMD_TYPE_ASM == cmd_type_get(cmd)) {
    struct cmd_ *ptr;

    ptr = cmd_element_seek(cmd, BOOLEAN_TRUE);
    if (ptr) {
      ptr->u.op_asm.opdst = dst;
    }
  }
}

value_t cmd_asm_n_get(const cmd_t cmd)
{
  const struct cmd_ *ptr;

  ptr = 0;
  if (CMD_TYPE_ASM == cmd_type_get(cmd)) {
    ptr = cmd_element_seek(cmd, BOOLEAN_FALSE);
  }
  return (ptr) ? ptr->u.op_asm.n : VALUE_NONE;
}

void         cmd_asm_n_set(cmd_t cmd, value_t n)
{
  assert(CMD_TYPE_ASM == cmd_type_get(cmd));
  if (CMD_TYPE_ASM == cmd_type_get(cmd)) {
    struct cmd_ *ptr;

    ptr = cmd_element_seek(cmd, BOOLEAN_TRUE);
    if (ptr) {
      value_lock(n);
      value_use_ct_bump(ptr->u.op_asm.n, CTR_BUMP_DECR);
      value_release(ptr->u.op_asm.n);
      ptr->u.op_asm.n = n;
      value_use_ct_bump(n, CTR_BUMP_INCR);
    }
  }
}

label_t cmd_asm_lbl_get(const cmd_t cmd)
{
  struct cmd_ *ptr;

  ptr = 0;
  if (CMD_TYPE_ASM == cmd_type_get(cmd)) {
    ptr = cmd_element_seek(cmd, BOOLEAN_FALSE);
  }
  return (ptr) ? ptr->u.op_asm.lbl : LABEL_NONE;
}

void cmd_asm_lbl_set(cmd_t cmd, label_t lbl)
{
  assert(CMD_TYPE_ASM == cmd_type_get(cmd));
  if (CMD_TYPE_ASM == cmd_type_get(cmd)) {
    struct cmd_ *ptr;

    ptr = cmd_element_seek(cmd, BOOLEAN_TRUE);
    if (ptr) {
      label_lock(lbl);
      label_usage_bump(ptr->u.op_asm.lbl, CTR_BUMP_DECR);
      label_release(ptr->u.op_asm.lbl);
      ptr->u.op_asm.lbl = lbl;
      label_usage_bump(lbl, CTR_BUMP_INCR);
    }
  }
}

boolean_t cmd_asm_flag_test(const cmd_t cmd, flag_t flag)
{
  const struct cmd_ *ptr;

  ptr = 0;
  if (CMD_TYPE_ASM == cmd_type_get(cmd)) {
    ptr = cmd_element_seek(cmd, BOOLEAN_FALSE);
  }
  return (ptr) ? (ptr->u.op_asm.flags & flag) == flag : 0;
}

flag_t cmd_asm_flag_get_all(const cmd_t cmd)
{
  const struct cmd_ *ptr;

  ptr = 0;
  if (CMD_TYPE_ASM == cmd_type_get(cmd)) {
    ptr = cmd_element_seek(cmd, BOOLEAN_FALSE);
  }
  return (ptr) ? ptr->u.op_asm.flags : 0;
}

size_t cmd_asm_data_sz_get(const cmd_t cmd)
{
  const struct cmd_ *ptr;

  ptr = 0;
  if (CMD_TYPE_ASM == cmd_type_get(cmd)) {
    ptr = cmd_element_seek(cmd, BOOLEAN_FALSE);
  }
  return (ptr) ? ptr->u.op_asm.data_sz : 0;
}

uchar *cmd_asm_data_get(const cmd_t cmd)
{
  const struct cmd_ *ptr;

  ptr = 0;
  if (CMD_TYPE_ASM == cmd_type_get(cmd)) {
    ptr = cmd_element_seek(cmd, BOOLEAN_FALSE);
  }
  return (ptr) ? ptr->u.op_asm.data : 0;
}

void   cmd_asm_data_set(cmd_t cmd, size_t sz, uchar *data)
{
  assert(CMD_TYPE_ASM == cmd_type_get(cmd));
  if (CMD_TYPE_ASM == cmd_type_get(cmd)) {
    struct cmd_ *ptr;

    ptr = cmd_element_seek(cmd, BOOLEAN_TRUE);
    if (ptr) {
      FREE(ptr->u.op_asm.data);
      ptr->u.op_asm.data_sz = sz;
      ptr->u.op_asm.data    = data;
    }
  }
}

