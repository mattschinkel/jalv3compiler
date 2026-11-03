/************************************************************
 **
 ** jal_error.c : user-visible runtime error hook (`_error_hook`)
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ** A failed operation can be reported two ways:
 **   * poll   -- the domain cell (`_string_error`) is set and stays set
 **               until read (the historical behaviour);
 **   * hook   -- if the program defines `procedure _error_hook()`, the
 **               compiler sets `_error_source` and the domain cell, calls
 **               the hook in place, and clears both cells when it returns
 **               (delivered once, then gone).
 **
 ** The hook is found by NAME at each raise site, so defining the procedure
 ** is the whole opt-in: no include, no declaration, no bind, and a program
 ** without it emits exactly the code it emitted before.
 **
 ************************************************************/
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include "../libutils/mem.h"
#include "../libcore/pfile.h"
#include "../libcore/pf_proc.h"
#include "../libcore/pf_block.h"
#include "../libcore/pf_cmd.h"
#include "../libcore/pf_expr.h"
#include "../libcore/cmd_brch.h"
#include "../libcore/operator.h"
#include "../libcore/variable.h"
#include "../libcore/value.h"
#include "jal_error.h"
#include "jal_string.h"

typedef struct jal_error_state_ {
  unsigned  missed_sites;   /* raise sites emitted while no hook existed */
  boolean_t warned;         /* the ordering warning already fired */
  boolean_t bad_signature;  /* the hook signature diagnostic already fired */
  /* where the first raise site that could not dispatch was emitted (the
   * source is locked so the position stays printable until the end of the
   * parse) */
  boolean_t   first_site_valid;
  pfile_pos_t first_site;
} jal_error_state_t;

static jal_error_state_t jal_error_state;

void jal_error_state_reset(void)
{
  if (jal_error_state.first_site_valid && jal_error_state.first_site.src) {
    pfile_source_release(jal_error_state.first_site.src);
  }
  memset(&jal_error_state, 0, sizeof(jal_error_state));
}

/* one global byte constant (a compile-time equate: no storage) */
static boolean_t jal_error_const_alloc(pfile_t *pf, const char *name,
    variable_const_t init)
{
  variable_t     var;
  variable_def_t def;
  result_t       rc;

  var = pfile_variable_find(pf, PFILE_LOG_NONE, name, 0);
  if (var) {
    variable_release(var);
    return BOOLEAN_TRUE;
  }
  def = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
      VARIABLE_DEF_FLAG_CONST, 1);
  if (!def) {
    pfile_log_syserr(pf, ENOMEM);
    return BOOLEAN_FALSE;
  }
  var = VARIABLE_NONE;
  rc = pfile_variable_alloc(pf, PFILE_VARIABLE_ALLOC_GLOBAL, name, def,
      VARIABLE_NONE, &var);
  if (RESULT_OK != rc) {
    pfile_log_syserr(pf, rc);
    return BOOLEAN_FALSE;
  }
  variable_const_set(var, def, 0, init);
  variable_release(var);
  return BOOLEAN_TRUE;
}

/* the `_error_source` cell, created on first use (only when a hook exists) */
static value_t jal_error_source_cell_get(pfile_t *pf)
{
  variable_t     var;
  variable_def_t def;
  result_t       rc;
  value_t        val;

  var = pfile_variable_find(pf, PFILE_LOG_NONE, JAL_ERROR_SOURCE_NAME, 0);
  if (!var) {
    def = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
        VARIABLE_DEF_FLAG_NONE, 1);
    if (!def) {
      pfile_log_syserr(pf, ENOMEM);
      return VALUE_NONE;
    }
    rc = pfile_variable_alloc(pf, PFILE_VARIABLE_ALLOC_GLOBAL,
        JAL_ERROR_SOURCE_NAME, def, VARIABLE_NONE, &var);
    if (RESULT_OK != rc) {
      pfile_log_syserr(pf, rc);
      return VALUE_NONE;
    }
    variable_const_set(var, def, 0, 0);
    variable_release(var);
    var = pfile_variable_find(pf, PFILE_LOG_NONE, JAL_ERROR_SOURCE_NAME, 0);
    if (!var) {
      return VALUE_NONE;
    }
  }
  val = value_alloc(var);
  variable_release(var);
  return val;
}

/* assign a constant to an lvalue cell */
static void jal_error_cell_assign(pfile_t *pf, value_t cell,
    variable_const_t n)
{
  value_t cv;

  cv = pfile_constant_get(pf, n, VARIABLE_DEF_NONE);
  if (cv) {
    pfile_cmd_op_add(pf, OPERATOR_ASSIGN, &cell, cv, VALUE_NONE);
    value_release(cv);
  } else {
    pfile_log_syserr(pf, ENOMEM);
  }
}

/* depth-first procedure search from a root (pfile_proc_find walks up from
 * the ACTIVE proc, which finds nothing at file scope) */
static pfile_proc_t *jal_error_proc_find_deep(pfile_proc_t *root,
    const char *tag)
{
  pfile_proc_t *proc;
  pfile_proc_t *found;

  for (proc = (root) ? pfile_proc_child_get(root) : 0; proc;
       proc = pfile_proc_sibbling_get(proc)) {
    const char *ptag;

    ptag = pfile_proc_tag_get(proc);
    if (ptag && (0 == strcmp(ptag, tag))) {
      return proc;
    }
    found = jal_error_proc_find_deep(proc, tag);
    if (found) {
      return found;
    }
  }
  return 0;
}

/* the user's `_error_hook` procedure, when the program defines one */
static pfile_proc_t *jal_error_hook_find(pfile_t *pf)
{
  pfile_proc_t *proc;

  proc = pfile_proc_find(pf, PFILE_LOG_NONE, JAL_ERROR_HOOK_NAME);
  if (proc) {
    return proc;
  }
  /* pfile_proc_find walks up from the ACTIVE proc; at file scope (chain
   * parse in the main block) nothing is active -- search from the root */
  {
    pfile_proc_t *root;

    root = pfile_proc_root_get(pf);
    while (root && pfile_proc_parent_get(root)) {
      root = pfile_proc_parent_get(root);
    }
    proc = jal_error_proc_find_deep(root, JAL_ERROR_HOOK_NAME);
  }
  return proc;
}

/* call binding for a no-parameter procedure (mirrors jal_parse_call) */
static value_t jal_error_hook_call_value_get(pfile_t *pf,
    pfile_proc_t *proc)
{
  pfile_block_t *blk;
  variable_t     func_var;
  value_t        val;
  result_t       rc;

  blk = pfile_proc_block_active_get(pfile_proc_active_get(pf));
  if (!blk) {
    return VALUE_NONE;
  }
  func_var = pfile_variable_find(pf, PFILE_LOG_NONE, JAL_ERROR_HOOK_NAME, 0);
  if (!func_var) {
    rc = pfile_block_variable_alloc(blk,
        pfile_tag_alloc(pf, JAL_ERROR_HOOK_NAME),
        pfile_proc_def_get(proc), VARIABLE_NONE, pf, &func_var);
    if ((RESULT_OK != rc) || !func_var) {
      return VALUE_NONE;
    }
  } else {
    variable_lock(func_var);
  }
  variable_proc_set(func_var, proc);
  val = value_alloc(func_var);
  variable_release(func_var);
  return val;
}

/* user-visible constants: compile-time equates, no storage.  Created on
 * first use (the hook definition or a raise site) because a global allocated
 * before the first block exists would not be visible to the parser. */
static void jal_error_constants_ensure(pfile_t *pf)
{
  (void) jal_error_const_alloc(pf, "_string_error_none", 0);
  (void) jal_error_const_alloc(pf, "_string_error_heap_full", 1);
  (void) jal_error_const_alloc(pf, "_string_error_overflow", 2);
  (void) jal_error_const_alloc(pf, "_string_error_invalid", 3);
  (void) jal_error_const_alloc(pf, "_error_source_string",
      JAL_ERROR_SOURCE_STRING);
  (void) jal_error_const_alloc(pf, "_error_source_user",
      JAL_ERROR_SOURCE_USER);
  /* compatibility spellings (the constants had no leading underscore
   * before 2026-11-02; equates are free, so keep both) */
  (void) jal_error_const_alloc(pf, "string_error_none", 0);
  (void) jal_error_const_alloc(pf, "string_error_heap_full", 1);
  (void) jal_error_const_alloc(pf, "string_error_overflow", 2);
  (void) jal_error_const_alloc(pf, "string_error_invalid", 3);
  (void) jal_error_const_alloc(pf, "error_source_string",
      JAL_ERROR_SOURCE_STRING);
  (void) jal_error_const_alloc(pf, "error_source_user",
      JAL_ERROR_SOURCE_USER);
}

void jal_error_program_start(pfile_t *pf)
{
  jal_error_state_reset();
  /* created here, where the main block exists: a GLOBAL allocated before
   * that is invisible to the parser (found 2026-11-02) */
  jal_error_constants_ensure(pf);
}

void jal_error_hook_prepare(pfile_t *pf)
{
  value_t v;

  jal_error_constants_ensure(pf);
  v = jal_error_source_cell_get(pf);
  if (v) {
    value_release(v);
  }
  (void) jal_string_error_cell_ensure(pf);
}

void jal_error_raise(pfile_t *pf, unsigned source, const char *code_cell,
    unsigned code)
{
  pfile_proc_t *hook;
  value_t       cell;
  value_t       src_cell;
  value_t       call_val;
  value_t      *parms;

  /* the domain's own cell first: this is the whole no-hook behaviour */
  cell = pfile_value_find(pf, PFILE_LOG_NONE, code_cell);
  if (cell) {
    jal_error_cell_assign(pf, cell, code);
    value_release(cell);
  }

  hook = jal_error_hook_find(pf);
  if (!hook) {
    if (!jal_error_state.first_site_valid) {
      pfile_pos_t pos;

      pfile_statement_start_get(pf, &pos);
      if (pos.src) {
        /* hold the source so the position can be printed at the end of the
         * parse (includes are closed before then) */
        pfile_source_lock(pos.src);
        jal_error_state.first_site = pos;
        jal_error_state.first_site_valid = BOOLEAN_TRUE;
      }
    }
    if (jal_error_state.missed_sites < 1000000U) {
      jal_error_state.missed_sites++;
    }
    return;
  }
  /* parms[0] is the return slot; a real parameter carries the IN flag */
  if (pfile_proc_return_def_get(hook)) {
    if (!jal_error_state.bad_signature) {
      jal_error_state.bad_signature = BOOLEAN_TRUE;
      pfile_log(pf, PFILE_LOG_ERR,
          "%s must be a procedure, not a function "
          "(e.g. procedure _error_hook() is ... end procedure)",
          JAL_ERROR_HOOK_NAME);
    }
    return;
  }
  {
    size_t ii;
    size_t ct;

    ct = pfile_proc_param_ct_get(hook);
    for (ii = 0; ii < ct; ii++) {
      value_t pv;

      pv = pfile_proc_param_get(hook, ii);
      if (pv && value_dflag_test(pv, VARIABLE_DEF_FLAG_IN)) {
        if (!jal_error_state.bad_signature) {
          jal_error_state.bad_signature = BOOLEAN_TRUE;
          pfile_log(pf, PFILE_LOG_ERR,
              "%s must be declared with no parameters "
              "(e.g. procedure _error_hook() is ... end procedure)",
              JAL_ERROR_HOOK_NAME);
        }
        return;
      }
    }
  }
  src_cell = jal_error_source_cell_get(pf);
  call_val = jal_error_hook_call_value_get(pf, hook);
  if (!src_cell || !call_val) {
    if (src_cell) {
      value_release(src_cell);
    }
    if (call_val) {
      value_release(call_val);
    }
    return;
  }
  parms = CALLOC(1, sizeof(*parms));
  if (!parms) {
    pfile_log_syserr(pf, ENOMEM);
    value_release(src_cell);
    value_release(call_val);
    return;
  }
  parms[0] = VALUE_NONE;         /* no return value */
  jal_error_cell_assign(pf, src_cell, source);
  pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_CALL, CMD_BRANCHCOND_NONE,
      LABEL_NONE, VALUE_NONE, call_val, parms);
  /* delivered once, then gone: clear both cells on return */
  jal_error_cell_assign(pf, src_cell, 0);
  cell = pfile_value_find(pf, PFILE_LOG_NONE, code_cell);
  if (cell) {
    jal_error_cell_assign(pf, cell, 0);
    value_release(cell);
  }
  value_release(src_cell);
  value_release(call_val);
}

void jal_error_program_end(pfile_t *pf)
{
  pfile_pos_t saved;

  if (jal_error_state.warned || !jal_error_state.missed_sites) {
    return;
  }
  if (!jal_error_hook_find(pf)) {
    return;
  }
  jal_error_state.warned = BOOLEAN_TRUE;
  if (!jal_error_state.first_site_valid || !jal_error_state.first_site.src) {
    pfile_log(pf, PFILE_LOG_WARN,
        "a runtime error may occur before %s was defined; %s will not be "
        "called here; define %s above the first string declaration",
        JAL_ERROR_HOOK_NAME, JAL_ERROR_HOOK_NAME, JAL_ERROR_HOOK_NAME);
    return;
  }
  /* report AT the first site that could not dispatch */
  pfile_statement_start_get(pf, &saved);
  if (saved.src) {
    pfile_source_lock(saved.src);
  }
  pfile_statement_start_set(pf, &jal_error_state.first_site);
  pfile_log(pf, PFILE_LOG_WARN,
      "a runtime error may occur before %s was defined. %s will not be "
      "called here. define %s above line %u.",
      JAL_ERROR_HOOK_NAME, JAL_ERROR_HOOK_NAME, JAL_ERROR_HOOK_NAME,
      jal_error_state.first_site.line);
  pfile_statement_start_set(pf, &saved);
  if (saved.src) {
    pfile_source_release(saved.src);
  }
  pfile_source_release(jal_error_state.first_site.src);
  jal_error_state.first_site_valid = BOOLEAN_FALSE;
  jal_error_state.first_site.src = 0;
}
