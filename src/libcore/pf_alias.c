/************************************************************
 **
 ** pf_alias.c : whole-program alias/escape summary (Phase 1)
 **
 ** Phase 1 skeleton: a flow-insensitive per-procedure read/write
 ** variable summary over the final p-code.  The direct call graph is
 ** the proc->calls list the parser already builds; this module adds
 ** the variable sets and a dump.  No codegen change.
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/

#include <stdlib.h>
#include <string.h>

#include "../libutils/mem.h"
#include "../libutils/types.h"
#include "cmd.h"
#include "cmd_brch.h"
#include "cmd_op.h"
#include "pf_alias.h"
#include "pf_proc.h"
#include "pfile.h"
#include "value.h"
#include "variable.h"

/* per-procedure summary: distinct variables read / written (direct and
 * transitive through the call graph) */
typedef struct {
  pfile_proc_t *proc;
  variable_t   *reads;
  size_t        reads_ct;
  size_t        reads_alloc;
  variable_t   *writes;
  size_t        writes_ct;
  size_t        writes_alloc;
  variable_t   *reads_trans;
  size_t        reads_trans_ct;
  size_t        reads_trans_alloc;
  variable_t   *writes_trans;
  size_t        writes_trans_ct;
  size_t        writes_trans_alloc;
} pfile_alias_proc_t;

static pfile_alias_proc_t *alias_tab;
static size_t              alias_tab_used;
static size_t              alias_tab_alloc;
static boolean_t           alias_dump_checked;
static boolean_t           alias_dump_on;
/* variables whose address escapes (pointer-bound or byref/open-array actual) */
static variable_t         *escaped_vars;
static size_t              escaped_vars_ct;
static size_t              escaped_vars_alloc;

/* accepted switch words (same rule as the access recorder) */
static boolean_t alias_env_on(const char *name)
{
  const char *v;
  char        buf[16];
  size_t      n;
  size_t      ii;

  v = getenv(name);
  if ((0 == v) || (0 == *v)) {
    return BOOLEAN_FALSE;
  }
  n = strlen(v);
  if (n >= sizeof(buf)) {
    n = sizeof(buf) - 1;
  }
  for (ii = 0; ii < n; ii++) {
    char c;

    c = v[ii];
    buf[ii] = ((c >= 'A') && (c <= 'Z')) ? (char) (c + ('a' - 'A')) : c;
  }
  buf[n] = 0;
  if ((0 == strcmp(buf, "1")) || (0 == strcmp(buf, "true"))
      || (0 == strcmp(buf, "yes")) || (0 == strcmp(buf, "on"))) {
    return BOOLEAN_TRUE;
  }
  if ((0 == strcmp(buf, "0")) || (0 == strcmp(buf, "false"))
      || (0 == strcmp(buf, "no")) || (0 == strcmp(buf, "off"))) {
    return BOOLEAN_FALSE;
  }
  return BOOLEAN_FALSE;
}

boolean_t pfile_alias_dump_enabled(void)
{
  if (!alias_dump_checked) {
    alias_dump_checked = BOOLEAN_TRUE;
    alias_dump_on      = alias_env_on("JALV2_ALIAS_DUMP");
  }
  return alias_dump_on;
}

void pfile_alias_state_reset(void)
{
  size_t ii;

  for (ii = 0; ii < alias_tab_used; ii++) {
    FREE(alias_tab[ii].reads);
    FREE(alias_tab[ii].writes);
    FREE(alias_tab[ii].reads_trans);
    FREE(alias_tab[ii].writes_trans);
  }
  FREE(alias_tab);
  FREE(escaped_vars);
  alias_tab          = 0;
  alias_tab_used     = 0;
  alias_tab_alloc    = 0;
  escaped_vars       = 0;
  escaped_vars_ct    = 0;
  escaped_vars_alloc = 0;
  alias_dump_checked = BOOLEAN_FALSE;
  alias_dump_on      = BOOLEAN_FALSE;
}

/* find the summary slot for proc (NONE when unrecorded) */
static pfile_alias_proc_t *alias_proc_find(pfile_proc_t *proc)
{
  size_t ii;

  for (ii = 0; ii < alias_tab_used; ii++) {
    if (alias_tab[ii].proc == proc) {
      return &alias_tab[ii];
    }
  }
  return 0;
}

/* find or create the summary slot for proc */
static pfile_alias_proc_t *alias_proc_get(pfile_proc_t *proc)
{
  pfile_alias_proc_t *slot;

  slot = alias_proc_find(proc);
  if (slot) {
    return slot;
  }
  if (alias_tab_used == alias_tab_alloc) {
    size_t              n;
    pfile_alias_proc_t *t;

    n = alias_tab_alloc ? (2 * alias_tab_alloc) : 16;
    t = REALLOC(alias_tab, n * sizeof(*t));
    if (!t) {
      return 0;
    }
    alias_tab       = t;
    alias_tab_alloc = n;
  }
  slot                = &alias_tab[alias_tab_used++];
  slot->proc          = proc;
  slot->reads         = 0;
  slot->reads_ct      = 0;
  slot->reads_alloc   = 0;
  slot->writes        = 0;
  slot->writes_ct     = 0;
  slot->writes_alloc  = 0;
  slot->reads_trans       = 0;
  slot->reads_trans_ct    = 0;
  slot->reads_trans_alloc = 0;
  slot->writes_trans       = 0;
  slot->writes_trans_ct    = 0;
  slot->writes_trans_alloc = 0;
  return slot;
}

/* add var to a deduped set */
static void alias_var_add(variable_t **set, size_t *ct, size_t *alloc,
    variable_t var)
{
  size_t ii;

  for (ii = 0; ii < *ct; ii++) {
    if ((*set)[ii] == var) {
      return;
    }
  }
  if (*ct == *alloc) {
    size_t      n;
    variable_t *t;

    n = *alloc ? (2 * (*alloc)) : 16;
    t = REALLOC(*set, n * sizeof(*t));
    if (!t) {
      return;
    }
    *set   = t;
    *alloc = n;
  }
  (*set)[(*ct)++] = var;
}

/* union src into *dst; TRUE when *dst gained an element */
static boolean_t alias_set_union(variable_t **dst, size_t *dst_ct,
    size_t *dst_alloc, variable_t *src, size_t src_ct)
{
  boolean_t changed;
  size_t    ii;

  changed = BOOLEAN_FALSE;
  for (ii = 0; ii < src_ct; ii++) {
    size_t jj;

    for (jj = 0; jj < *dst_ct; jj++) {
      if ((*dst)[jj] == src[ii]) {
        break;
      }
    }
    if (jj == *dst_ct) {
      alias_var_add(dst, dst_ct, dst_alloc, src[ii]);
      changed = BOOLEAN_TRUE;
    }
  }
  return changed;
}

/* Phase 2: transitive read/write closure through the call graph, to a
 * fixpoint (sets only grow, so it converges; the round cap bounds a
 * pathological cycle). */
#define JAL_ALIAS_FIX_ROUNDS 64

static void alias_trans_close(void)
{
  unsigned round;
  boolean_t changed;
  size_t   ii;

  /* seed: trans(P) = direct(P) */
  for (ii = 0; ii < alias_tab_used; ii++) {
    pfile_alias_proc_t *slot;

    slot = &alias_tab[ii];
    (void) alias_set_union(&slot->reads_trans, &slot->reads_trans_ct,
        &slot->reads_trans_alloc, slot->reads, slot->reads_ct);
    (void) alias_set_union(&slot->writes_trans, &slot->writes_trans_ct,
        &slot->writes_trans_alloc, slot->writes, slot->writes_ct);
  }
  for (round = 0; round < JAL_ALIAS_FIX_ROUNDS; round++) {
    changed = BOOLEAN_FALSE;
    for (ii = 0; ii < alias_tab_used; ii++) {
      pfile_alias_proc_t *slot;
      size_t              ct;
      size_t              jj;

      slot = &alias_tab[ii];
      ct   = pfile_proc_calls_ct_get(slot->proc);
      for (jj = 0; jj < ct; jj++) {
        pfile_proc_t       *callee;
        pfile_alias_proc_t *cslot;

        callee = pfile_proc_calls_get(slot->proc, jj);
        cslot  = alias_proc_find(callee);
        if (!cslot) {
          continue;
        }
        if (alias_set_union(&slot->reads_trans, &slot->reads_trans_ct,
              &slot->reads_trans_alloc, cslot->reads_trans,
              cslot->reads_trans_ct)) {
          changed = BOOLEAN_TRUE;
        }
        if (alias_set_union(&slot->writes_trans, &slot->writes_trans_ct,
              &slot->writes_trans_alloc, cslot->writes_trans,
              cslot->writes_trans_ct)) {
          changed = BOOLEAN_TRUE;
        }
      }
    }
    if (!changed) {
      break;
    }
  }
}

/* record a value's variable as a read (write=0) or write (write!=0) */
static void alias_value_note(pfile_alias_proc_t *slot, value_t val,
    boolean_t write)
{
  variable_t var;

  if (!val || value_is_temp(val) || value_is_const(val)) {
    return;
  }
  var = value_variable_get(val);
  if (!var) {
    return;
  }
  if (write) {
    alias_var_add(&slot->writes, &slot->writes_ct, &slot->writes_alloc, var);
  } else {
    alias_var_add(&slot->reads, &slot->reads_ct, &slot->reads_alloc, var);
  }
}

#define JAL_ALIAS_PROC_STACK 128

/* mark a variable as address-escaped (deduped) */
static void alias_var_escaped_note(variable_t var)
{
  alias_var_add(&escaped_vars, &escaped_vars_ct, &escaped_vars_alloc, var);
}

void pfile_alias_summary_build(pfile_t *pf)
{
  pfile_proc_t *proc_stack[JAL_ALIAS_PROC_STACK];
  unsigned      depth;
  cmd_t         cmd;

  pfile_alias_state_reset();
  /* the root proc's body has no PROC_ENTER: top-level commands are at
   * depth 0, so start the stack on the root */
  proc_stack[0] = pfile_proc_root_get(pf);
  depth = 1;
  for (cmd = pfile_cmd_head_get(pf); cmd; cmd = cmd_link_get(cmd)) {
    cmd_type_t type;

    type = cmd_type_get(cmd);
    if (CMD_TYPE_PROC_ENTER == type) {
      if (depth < JAL_ALIAS_PROC_STACK) {
        proc_stack[depth++] = cmd_proc_get(cmd);
      }
    } else if (CMD_TYPE_PROC_LEAVE == type) {
      if (depth) {
        depth--;
      }
    } else if ((CMD_TYPE_OPERATOR == type) && depth) {
      pfile_alias_proc_t *slot;

      slot = alias_proc_get(proc_stack[depth - 1]);
      if (!slot) {
        return;
      }
      alias_value_note(slot, cmd_opdst_get(cmd), BOOLEAN_TRUE);
      alias_value_note(slot, cmd_opval1_get(cmd), BOOLEAN_FALSE);
      alias_value_note(slot, cmd_opval2_get(cmd), BOOLEAN_FALSE);
      /* a pointer bind (`p is x`) takes x's address */
      if ((OPERATOR_POINTER_ASSIGN == cmd_optype_get(cmd))
          && cmd_opval1_get(cmd)) {
        variable_t target;

        target = value_variable_get(cmd_opval1_get(cmd));
        if (target) {
          alias_var_escaped_note(target);
        }
      }
    } else if ((CMD_TYPE_BRANCH == type)
        && (CMD_BRANCHTYPE_CALL == cmd_brtype_get(cmd))) {
      /* an open-array/byref actual has its address passed to the callee */
      cmd_branch_pair_t *pairs;
      size_t             pct;
      size_t             pi;

      pairs = cmd_brpairs_get(cmd);
      pct   = cmd_brpairs_ct_get(cmd);
      for (pi = 0; pi < pct; pi++) {
        if (pairs[pi].actual) {
          alias_var_escaped_note(pairs[pi].actual);
        }
      }
    }
  }
  alias_trans_close();
}

variable_t *pfile_alias_proc_reads_get(pfile_proc_t *proc)
{
  pfile_alias_proc_t *slot;

  slot = alias_proc_find(proc);
  return (slot) ? slot->reads : 0;
}

size_t pfile_alias_proc_reads_ct(pfile_proc_t *proc)
{
  pfile_alias_proc_t *slot;

  slot = alias_proc_find(proc);
  return (slot) ? slot->reads_ct : 0;
}

variable_t *pfile_alias_proc_writes_get(pfile_proc_t *proc)
{
  pfile_alias_proc_t *slot;

  slot = alias_proc_find(proc);
  return (slot) ? slot->writes : 0;
}

size_t pfile_alias_proc_writes_ct(pfile_proc_t *proc)
{
  pfile_alias_proc_t *slot;

  slot = alias_proc_find(proc);
  return (slot) ? slot->writes_ct : 0;
}

variable_t *pfile_alias_proc_reads_trans_get(pfile_proc_t *proc)
{
  pfile_alias_proc_t *slot;

  slot = alias_proc_find(proc);
  return (slot) ? slot->reads_trans : 0;
}

size_t pfile_alias_proc_reads_trans_ct(pfile_proc_t *proc)
{
  pfile_alias_proc_t *slot;

  slot = alias_proc_find(proc);
  return (slot) ? slot->reads_trans_ct : 0;
}

variable_t *pfile_alias_proc_writes_trans_get(pfile_proc_t *proc)
{
  pfile_alias_proc_t *slot;

  slot = alias_proc_find(proc);
  return (slot) ? slot->writes_trans : 0;
}

size_t pfile_alias_proc_writes_trans_ct(pfile_proc_t *proc)
{
  pfile_alias_proc_t *slot;

  slot = alias_proc_find(proc);
  return (slot) ? slot->writes_trans_ct : 0;
}

boolean_t pfile_alias_var_escaped(variable_t var)
{
  size_t ii;

  for (ii = 0; ii < escaped_vars_ct; ii++) {
    if (escaped_vars[ii] == var) {
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

/* print one comma-separated variable name list */
static void alias_varlist_dump(FILE *out, variable_t *set, size_t ct)
{
  size_t ii;

  for (ii = 0; ii < ct; ii++) {
    const char *name;

    name = variable_name_get(set[ii]);
    fprintf(out, "%s%s", (0 == ii) ? "" : ",", name ? name : "?");
  }
}

void pfile_alias_summary_dump(pfile_t *pf, FILE *out)
{
  pfile_proc_t *proc;

  fprintf(out, "# alias summary (phase 2): per-procedure reads/writes "
      "(direct + transitive)\n");
  for (proc = pfile_proc_root_get(pf); proc; proc = pfile_proc_next(proc)) {
    const char *name;
    size_t      ct;
    size_t      ii;

    name = pfile_proc_tag_get(proc);
    if (!name) {
      name = "?";
    }
    fprintf(out, "proc %s calls[", name);
    ct = pfile_proc_calls_ct_get(proc);
    for (ii = 0; ii < ct; ii++) {
      pfile_proc_t *callee;
      const char   *cname;

      callee = pfile_proc_calls_get(proc, ii);
      cname  = (callee) ? pfile_proc_tag_get(callee) : "?";
      fprintf(out, "%s%s", (0 == ii) ? "" : ",", cname ? cname : "?");
    }
    fprintf(out, "] reads[");
    alias_varlist_dump(out, pfile_alias_proc_reads_get(proc),
        pfile_alias_proc_reads_ct(proc));
    fprintf(out, "] writes[");
    alias_varlist_dump(out, pfile_alias_proc_writes_get(proc),
        pfile_alias_proc_writes_ct(proc));
    fprintf(out, "] t_reads[");
    alias_varlist_dump(out, pfile_alias_proc_reads_trans_get(proc),
        pfile_alias_proc_reads_trans_ct(proc));
    fprintf(out, "] t_writes[");
    alias_varlist_dump(out, pfile_alias_proc_writes_trans_get(proc),
        pfile_alias_proc_writes_trans_ct(proc));
    fprintf(out, "]\n");
  }
  fprintf(out, "escaped[");
  alias_varlist_dump(out, escaped_vars, escaped_vars_ct);
  fprintf(out, "]\n");
}
