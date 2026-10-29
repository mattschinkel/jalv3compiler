/************************************************************
 **
 ** mips_alloc.c : PIC32 GPR home assignment (Phase A–E)
 **
 ** Dirty-WB, caller-saved interval reuse, free $t0, dst-home fusion,
 ** file-level $s keep (known callee write-sets). Dump: JALV2_MIPS_ALLOC_DUMP=1
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "../libutils/types.h"
#include "../libutils/mem.h"
#include "../libcore/pf_proc.h"
#include "../libcore/value.h"
#include "../libcore/variable.h"
#include "../libcore/cmd.h"
#include "../libcore/cmd_op.h"
#include "../libcore/cmd_brch.h"
#include "mips.h"
#include "mips_alloc.h"
#include "mips_reg.h"

#define MIPS_ALLOC_MAX 512

typedef struct {
  variable_t   v;
  pfile_proc_t *owner;
  unsigned     home; /* (unsigned)-1 = spill */
  unsigned     uses;
  unsigned     first_use;
  unsigned     last_use;
  pfile_proc_t *writer; /* first writing proc; multi → single_writer=0 */
  int          is_formal;
  int          is_nested;
  int          single_writer;
  int          live;
  int          dirty;
} mips_alloc_ent_t;

/* Every table below is GROWABLE (doubling, MALLOC'd at first use and freed
 * by mips_alloc_reset -- one pfile per process): the fixed caps used to
 * drop entries SILENTLY on overflow (2026-10-05 hardening), which lost
 * home candidates and -- worse -- call-graph edges, feeding wrong
 * callee-write-set decisions for the file-level $s keep. Growable arrays
 * replaced dropped-pend registries on the ESP32 back end for the same
 * reason (fixes/fix_esp32_latebind_registry_cleanup.md). */
static mips_alloc_ent_t *mips_alloc_tab;
static unsigned          mips_alloc_ct;
static unsigned          mips_alloc_cap;
static pfile_t          *mips_alloc_pf;

/* Caller-saved pool: $t0 last so address LEA prefers colder homes first. */
static const unsigned mips_alloc_caller_pool[] = {
  MIPS_REG_T8, MIPS_REG_T9, MIPS_REG_V1,
  MIPS_REG_A0, MIPS_REG_A1, MIPS_REG_A2, MIPS_REG_A3,
  MIPS_REG_T0
};
#define MIPS_ALLOC_CALLER_CT \
  (sizeof(mips_alloc_caller_pool) / sizeof(mips_alloc_caller_pool[0]))

/* Serials of LABEL / CALL / PROC_LEAVE — dirty flush / caller-saved cold. */
static unsigned *mips_alloc_flush_serial;
static unsigned  mips_alloc_flush_ct;
static unsigned  mips_alloc_flush_cap;

/* Direct CALL edges + OPERATOR write facts for file-level keep. */
static pfile_proc_t **mips_alloc_edge_from;
static pfile_proc_t **mips_alloc_edge_to;
static unsigned       mips_alloc_edge_ct;
static unsigned       mips_alloc_edge_cap;
static pfile_proc_t **mips_alloc_write_proc;
static variable_t    *mips_alloc_write_var;
static unsigned       mips_alloc_write_ct;
static unsigned       mips_alloc_write_cap;

/* generic doubling append; returns 0 on OOM (caller reports) */
static int mips_alloc_grow(void **arr, unsigned *cap, size_t elem_sz)
{
  unsigned ncap;
  void    *nd;

  ncap = *cap ? *cap * 2U : 64U;
  nd = REALLOC(*arr, (size_t) ncap * elem_sz);
  if (!nd) {
    return 0;
  }
  *arr = nd;
  *cap = ncap;
  return 1;
}

void mips_alloc_reset(void)
{
  mips_alloc_ct = 0;
  mips_alloc_pf = 0;
  mips_alloc_flush_ct = 0;
  mips_alloc_edge_ct = 0;
  mips_alloc_write_ct = 0;
}

void mips_alloc_pf_set(pfile_t *pf)
{
  mips_alloc_pf = pf;
}

int mips_alloc_reg_is_callee_home(unsigned reg)
{
  return (reg >= MIPS_REG_S3) && (reg <= MIPS_REG_S7);
}

void mips_alloc_scratch_clobber(pfile_t *pf, unsigned reg)
{
  unsigned i;

  if (reg == (unsigned) -1) {
    return;
  }
  if (!pf) {
    pf = mips_alloc_pf;
  }
  for (i = 0; i < mips_alloc_ct; i++) {
    if (mips_alloc_tab[i].home != reg) {
      continue;
    }
    if (!mips_alloc_tab[i].live) {
      continue;
    }
    if (mips_alloc_tab[i].dirty && pf) {
      mips_alloc_emit_home_spill(pf, mips_alloc_tab[i].v, reg);
    }
    mips_alloc_tab[i].live  = 0;
    mips_alloc_tab[i].dirty = 0;
  }
}

/* CFG-edge spill.  Keep $s homes dirty (the target label's own flush
 * still serves other predecessors), but a caller-saved home is NOT
 * valid on every edge: a back-edge that ran through a CALL reaches the
 * label with the register already clobbered.  Spill it here, then cold
 * it so the label flush cannot re-store a dead register. */
void mips_alloc_spill_dirty_keep(pfile_t *pf)
{
  unsigned i;

  if (!pf) {
    pf = mips_alloc_pf;
  }
  if (!pf) {
    return;
  }
  for (i = 0; i < mips_alloc_ct; i++) {
    if (!mips_alloc_tab[i].dirty) {
      continue;
    }
    if (mips_alloc_tab[i].home == (unsigned) -1) {
      continue;
    }
    mips_alloc_emit_home_spill(pf, mips_alloc_tab[i].v,
        mips_alloc_tab[i].home);
    if (!mips_alloc_reg_is_callee_home(mips_alloc_tab[i].home)) {
      mips_alloc_tab[i].dirty = 0;
      mips_alloc_tab[i].live  = 0;
      mips_reg_cache_invalidate_var(mips_alloc_tab[i].v);
    }
  }
}

/* Label-entry edge spill: only the fall-through predecessor executes
 * these stores (they are emitted BEFORE the label node, so branch and
 * back-edge predecessors skip them).  Caller-saved homes must never be
 * stored from the label itself -- a back-edge may have called through a
 * clobber (2026-10-20 print_sdword_dec hang). */
void mips_alloc_spill_dirty_caller(pfile_t *pf)
{
  unsigned i;

  if (!pf) {
    pf = mips_alloc_pf;
  }
  if (!pf) {
    return;
  }
  for (i = 0; i < mips_alloc_ct; i++) {
    if (!mips_alloc_tab[i].dirty) {
      continue;
    }
    if (mips_alloc_tab[i].home == (unsigned) -1) {
      continue;
    }
    if (mips_alloc_reg_is_callee_home(mips_alloc_tab[i].home)) {
      continue;
    }
    mips_alloc_emit_home_spill(pf, mips_alloc_tab[i].v,
        mips_alloc_tab[i].home);
    mips_alloc_tab[i].dirty = 0;
    mips_alloc_tab[i].live  = 0;
    mips_reg_cache_invalidate_var(mips_alloc_tab[i].v);
  }
}

void mips_alloc_flush_dirty(pfile_t *pf)
{
  unsigned i;

  if (!pf) {
    pf = mips_alloc_pf;
  }
  if (!pf) {
    return;
  }
  for (i = 0; i < mips_alloc_ct; i++) {
    if (!mips_alloc_tab[i].dirty) {
      continue;
    }
    if (mips_alloc_tab[i].home == (unsigned) -1) {
      mips_alloc_tab[i].dirty = 0;
      continue;
    }
    mips_alloc_emit_home_spill(pf, mips_alloc_tab[i].v,
        mips_alloc_tab[i].home);
    mips_alloc_tab[i].dirty = 0;
  }
}

void mips_alloc_region_reset(void)
{
  unsigned i;

  mips_alloc_flush_dirty(mips_alloc_pf);
  for (i = 0; i < mips_alloc_ct; i++) {
    mips_alloc_tab[i].live  = 0;
    mips_alloc_tab[i].dirty = 0;
  }
}

static void mips_alloc_note_write_fact(pfile_proc_t *proc, variable_t master)
{
  unsigned i;

  if (!proc || !master) {
    return;
  }
  for (i = 0; i < mips_alloc_write_ct; i++) {
    if ((mips_alloc_write_proc[i] == proc)
        && (mips_alloc_write_var[i] == master)) {
      return;
    }
  }
  if (mips_alloc_write_ct >= mips_alloc_write_cap) {
    unsigned ocap;
    int      ok;

    ocap = mips_alloc_write_cap;
    ok = mips_alloc_grow((void **) &mips_alloc_write_proc,
        &mips_alloc_write_cap, sizeof(*mips_alloc_write_proc));
    if (ok && (mips_alloc_write_cap != ocap)) {
      /* write_var rides the same cap; REALLOC moved alongside */
      ok = (mips_alloc_write_var = REALLOC(mips_alloc_write_var,
          (size_t) mips_alloc_write_cap * sizeof(*mips_alloc_write_var)))
        != 0;
    }
    if (!ok) {
      mips_alloc_write_ct = 0;   /* drop the fact table, keep the edges */
      return;
    }
  }
  mips_alloc_write_proc[mips_alloc_write_ct] = proc;
  mips_alloc_write_var[mips_alloc_write_ct]  = master;
  mips_alloc_write_ct++;
}

static void mips_alloc_note_call_edge(pfile_proc_t *from, pfile_proc_t *to)
{
  unsigned i;

  if (!from || !to || (from == to)) {
    return;
  }
  for (i = 0; i < mips_alloc_edge_ct; i++) {
    if ((mips_alloc_edge_from[i] == from)
        && (mips_alloc_edge_to[i] == to)) {
      return;
    }
  }
  if (mips_alloc_edge_ct >= mips_alloc_edge_cap) {
    unsigned ocap;
    int      ok;

    ocap = mips_alloc_edge_cap;
    ok = mips_alloc_grow((void **) &mips_alloc_edge_from,
        &mips_alloc_edge_cap, sizeof(*mips_alloc_edge_from));
    if (ok && (mips_alloc_edge_cap != ocap)) {
      ok = (mips_alloc_edge_to = REALLOC(mips_alloc_edge_to,
          (size_t) mips_alloc_edge_cap * sizeof(*mips_alloc_edge_to))) != 0;
    }
    if (!ok) {
      return;
    }
  }
  mips_alloc_edge_from[mips_alloc_edge_ct] = from;
  mips_alloc_edge_to[mips_alloc_edge_ct]   = to;
  mips_alloc_edge_ct++;
}

/* Transitive: does `proc` or anyone it calls write `v`? */
static int mips_alloc_proc_may_write(pfile_proc_t *proc, variable_t v)
{
  /* seen[]/stack[] are proc-count bounded (a path visits each proc once);
   * they are allocated per call from the growable-table counts. On OOM the
   * answer degrades to "assume writes" -- the conservative direction. */
  pfile_proc_t **stack;
  pfile_proc_t **seen;
  unsigned       nmax;
  unsigned       sp;
  unsigned       sn;
  unsigned       i;
  int            rc;

  if (!proc || !v) {
    return 1; /* unknown — assume writes */
  }
  nmax = mips_alloc_ct + mips_alloc_edge_ct + 1U;
  stack = MALLOC(nmax * sizeof(*stack));
  seen = MALLOC(nmax * sizeof(*seen));
  if (!stack || !seen) {
    FREE(stack);
    FREE(seen);
    return 1;   /* conservative */
  }
  sp = 0;
  sn = 0;
  stack[sp++] = proc;
  rc = 0;
  while (sp > 0) {
    pfile_proc_t *p;
    unsigned      s;

    p = stack[--sp];
    for (s = 0; s < sn; s++) {
      if (seen[s] == p) {
        break;
      }
    }
    if (s < sn) {
      continue;
    }
    seen[sn++] = p;
    for (i = 0; i < mips_alloc_write_ct; i++) {
      if ((mips_alloc_write_proc[i] == p)
          && (mips_alloc_write_var[i] == v)) {
        rc = 1;
        goto done;
      }
    }
    for (i = 0; i < mips_alloc_edge_ct; i++) {
      if (mips_alloc_edge_from[i] == p) {
        stack[sp++] = mips_alloc_edge_to[i];
      }
    }
  }
done:
  FREE(stack);
  FREE(seen);
  return rc;
}

static int mips_alloc_keep_across_call(unsigned i, pfile_proc_t *callee)
{
  if (mips_alloc_tab[i].home == (unsigned) -1) {
    return 0;
  }
  if (mips_alloc_tab[i].is_formal) {
    return 0;
  }
  if (!mips_alloc_reg_is_callee_home(mips_alloc_tab[i].home)) {
    return 0;
  }
  if (mips_alloc_tab[i].is_nested) {
    return 1;
  }
  /* File-level $s: keep only with a known callee that does not write it. */
  if (!callee) {
    return 0;
  }
  if (!mips_alloc_tab[i].single_writer) {
    return 0;
  }
  if (mips_alloc_proc_may_write(callee, mips_alloc_tab[i].v)) {
    return 0;
  }
  return 1;
}

void mips_alloc_call_reset(pfile_proc_t *callee)
{
  unsigned i;

  /* Always spill dirty before CALL — RAM must match for getters. */
  if (mips_alloc_pf) {
    for (i = 0; i < mips_alloc_ct; i++) {
      if (!mips_alloc_tab[i].dirty) {
        continue;
      }
      if (mips_alloc_tab[i].home == (unsigned) -1) {
        mips_alloc_tab[i].dirty = 0;
        continue;
      }
      mips_alloc_emit_home_spill(mips_alloc_pf, mips_alloc_tab[i].v,
          mips_alloc_tab[i].home);
      mips_alloc_tab[i].dirty = 0;
    }
  }
  for (i = 0; i < mips_alloc_ct; i++) {
    if (mips_alloc_keep_across_call(i, callee)) {
      continue;
    }
    mips_alloc_tab[i].live  = 0;
    mips_alloc_tab[i].dirty = 0;
  }
}

void mips_alloc_proc_enter_cold(void)
{
  unsigned i;

  for (i = 0; i < mips_alloc_ct; i++) {
    if (mips_alloc_tab[i].home == (unsigned) -1) {
      continue;
    }
    if (mips_alloc_reg_is_callee_home(mips_alloc_tab[i].home)) {
      continue;
    }
    mips_alloc_tab[i].live  = 0;
    mips_alloc_tab[i].dirty = 0;
  }
}

static int mips_alloc_is_formal(pfile_t *pf, variable_t var)
{
  pfile_proc_t *proc;
  size_t        ii;
  unsigned      n;

  if (!var || !pf) {
    return 0;
  }
  for (proc = pfile_proc_root_get(pf); proc; proc = pfile_proc_next(proc)) {
    n = (unsigned) pfile_proc_param_ct_get(proc);
    for (ii = 0; ii < n; ii++) {
      value_t pv;

      pv = pfile_proc_param_get(proc, ii);
      if (pv && (value_variable_get(pv) == var)) {
        return 1;
      }
    }
  }
  return 0;
}

static int mips_alloc_eligible_var(variable_t v)
{
  value_t val;
  int     ok;

  if (!v) {
    return 0;
  }
  while (variable_master_get(v)) {
    v = variable_master_get(v);
  }
  val = value_alloc(v);
  if (!val) {
    return 0;
  }
  ok = mips_reg_cacheable(val) ? 1 : 0;
  value_release(val);
  return ok;
}

static int mips_alloc_owner_ok(pfile_proc_t *owner)
{
  if (!owner) {
    return 1;
  }
  if (pfile_proc_flag_test(owner, PFILE_PROC_FLAG_INTERRUPT)
      || pfile_proc_flag_test(owner, PFILE_PROC_FLAG_TASK)) {
    return 0;
  }
  return 1;
}

/* Temps / bitbuckets must not hold homes (reuse / aliases). */
static int mips_alloc_name_is_temp(const char *nm)
{
  if (!nm || !nm[0]) {
    return 1;
  }
  if (0 == strcmp(nm, "_bitbucket") || 0 == strcmp(nm, "_btmp")) {
    return 1;
  }
  if (0 == strncmp(nm, "_temp", 5)) {
    return 1;
  }
  return 0;
}

static int mips_alloc_find(variable_t master)
{
  unsigned i;

  for (i = 0; i < mips_alloc_ct; i++) {
    if (mips_alloc_tab[i].v == master) {
      return (int) i;
    }
  }
  return -1;
}

static void mips_alloc_add(pfile_t *pf, variable_t v, pfile_proc_t *owner)
{
  variable_t   master;
  pfile_proc_t *vproc;

  if (!v) {
    return;
  }
  if ((mips_alloc_ct >= mips_alloc_cap)
      && !mips_alloc_grow((void **) &mips_alloc_tab, &mips_alloc_cap,
          sizeof(*mips_alloc_tab))) {
    pfile_log_syserr(pf, ENOMEM);
    return;
  }
  master = v;
  while (variable_master_get(master)) {
    master = variable_master_get(master);
  }
  if (mips_alloc_find(master) >= 0) {
    return;
  }
  if (!mips_alloc_eligible_var(master)) {
    return;
  }
  vproc = variable_proc_get(master);
  if (!vproc) {
    vproc = owner;
  }
  mips_alloc_tab[mips_alloc_ct].v             = master;
  mips_alloc_tab[mips_alloc_ct].owner         = vproc;
  mips_alloc_tab[mips_alloc_ct].home          = (unsigned) -1;
  mips_alloc_tab[mips_alloc_ct].uses          = 0;
  mips_alloc_tab[mips_alloc_ct].first_use     = 0;
  mips_alloc_tab[mips_alloc_ct].last_use      = 0;
  mips_alloc_tab[mips_alloc_ct].writer        = 0;
  mips_alloc_tab[mips_alloc_ct].is_formal     = mips_alloc_is_formal(pf, master);
  mips_alloc_tab[mips_alloc_ct].is_nested     =
      (vproc && pfile_proc_parent_get(vproc)) ? 1 : 0;
  mips_alloc_tab[mips_alloc_ct].single_writer = 1;
  mips_alloc_tab[mips_alloc_ct].live          = 0;
  mips_alloc_tab[mips_alloc_ct].dirty         = 0;
  mips_alloc_ct++;
}

static void mips_alloc_note_use(variable_t master, unsigned serial)
{
  int ix;

  ix = mips_alloc_find(master);
  if (ix < 0) {
    return;
  }
  mips_alloc_tab[ix].uses++;
  if ((0 == mips_alloc_tab[ix].first_use)
      || (serial < mips_alloc_tab[ix].first_use)) {
    mips_alloc_tab[ix].first_use = serial;
  }
  if (serial > mips_alloc_tab[ix].last_use) {
    mips_alloc_tab[ix].last_use = serial;
  }
}

static void mips_alloc_note_write(variable_t master, pfile_proc_t *proc,
    unsigned serial)
{
  int ix;

  mips_alloc_note_use(master, serial);
  mips_alloc_note_write_fact(proc, master);
  ix = mips_alloc_find(master);
  if (ix < 0) {
    return;
  }
  if (!mips_alloc_tab[ix].writer) {
    mips_alloc_tab[ix].writer = proc;
  } else if (mips_alloc_tab[ix].writer != proc) {
    mips_alloc_tab[ix].single_writer = 0;
  }
}

static void mips_alloc_bump_val(value_t val, unsigned serial, int is_write,
    pfile_proc_t *proc)
{
  variable_t master;

  if (!val) {
    return;
  }
  master = value_variable_get(val);
  if (!master) {
    return;
  }
  while (variable_master_get(master)) {
    master = variable_master_get(master);
  }
  if (is_write) {
    mips_alloc_note_write(master, proc, serial);
  } else {
    mips_alloc_note_use(master, serial);
  }
}

static void mips_alloc_note_flush(unsigned serial)
{
  if ((mips_alloc_flush_ct >= mips_alloc_flush_cap)
      && !mips_alloc_grow((void **) &mips_alloc_flush_serial,
          &mips_alloc_flush_cap, sizeof(*mips_alloc_flush_serial))) {
    return;   /* flush-between becomes conservative -- see caller_reg_free */
  }
  mips_alloc_flush_serial[mips_alloc_flush_ct++] = serial;
}

static void mips_alloc_count_uses(cmd_t head)
{
  cmd_t          cmd;
  unsigned       serial;
  pfile_proc_t  *proc;

  serial = 0;
  proc   = 0;
  mips_alloc_flush_ct = 0;
  for (cmd = head; cmd; cmd = cmd_link_get(cmd)) {
    cmd_type_t type;

    serial++;
    type = cmd_type_get(cmd);
    if (CMD_TYPE_PROC_ENTER == type) {
      proc = cmd_proc_get(cmd);
    } else if (CMD_TYPE_PROC_LEAVE == type) {
      proc = 0;
      mips_alloc_note_flush(serial);
    } else if (CMD_TYPE_LABEL == type) {
      mips_alloc_note_flush(serial);
    } else if (CMD_TYPE_BRANCH == type) {
      if (CMD_BRANCHTYPE_CALL == cmd_brtype_get(cmd)) {
        value_t       pv;
        pfile_proc_t *callee;

        mips_alloc_note_flush(serial);
        pv = cmd_brproc_get(cmd);
        callee = pv ? value_proc_get(pv) : 0;
        if (proc && callee) {
          mips_alloc_note_call_edge(proc, callee);
        }
      }
    }
    if (!cmd_is_reachable(cmd)) {
      continue;
    }
    if (CMD_TYPE_OPERATOR == type) {
      mips_alloc_bump_val(cmd_opdst_get(cmd), serial, 1, proc);
      mips_alloc_bump_val(cmd_opval1_get(cmd), serial, 0, proc);
      mips_alloc_bump_val(cmd_opval2_get(cmd), serial, 0, proc);
    }
  }
}

/* True if [lo,hi] contains a flush serial (exclusive of lo, inclusive hi). */
static int mips_alloc_flush_between(unsigned lo, unsigned hi)
{
  unsigned i;

  if ((0 == lo) || (0 == hi) || (hi <= lo)) {
    return 0;
  }
  for (i = 0; i < mips_alloc_flush_ct; i++) {
    unsigned s;

    s = mips_alloc_flush_serial[i];
    if ((s > lo) && (s <= hi)) {
      return 1;
    }
  }
  return 0;
}

static int mips_alloc_intervals_overlap(unsigned a0, unsigned a1,
    unsigned b0, unsigned b1)
{
  if ((0 == a0) || (0 == a1) || (0 == b0) || (0 == b1)) {
    return 1; /* unknown extent — treat as conflict */
  }
  return (a0 <= b1) && (b0 <= a1);
}

/*
 * Caller-saved reg free for [first,last] if no assigned occupant overlaps,
 * and any prior occupant on that reg is separated by a flush (so dirty
 * cannot leak into the next live range).
 */
static int mips_alloc_caller_reg_free(unsigned reg, unsigned first,
    unsigned last)
{
  unsigned i;

  for (i = 0; i < mips_alloc_ct; i++) {
    unsigned f;
    unsigned l;

    if (mips_alloc_tab[i].home != reg) {
      continue;
    }
    f = mips_alloc_tab[i].first_use;
    l = mips_alloc_tab[i].last_use;
    if (mips_alloc_intervals_overlap(first, last, f, l)) {
      return 0;
    }
    /* Require flush in the gap so the earlier dirty home is spilled. */
    if (l < first) {
      if (!mips_alloc_flush_between(l, first)) {
        return 0;
      }
    } else if (last < f) {
      if (!mips_alloc_flush_between(last, f)) {
        return 0;
      }
    }
  }
  return 1;
}

/*
 * $s homes stay 1:1 (reuse here grew frames / memops). Caller-saved may
 * share a physical reg across non-overlapping intervals separated by a
 * flush point (LABEL / CALL / PROC_LEAVE).
 */
static void mips_alloc_assign_homes(void)
{
  unsigned taken_s[MIPS_S_HOME_MAX];
  unsigned t;
  unsigned assigned;
  unsigned i;

  for (t = 0; t < MIPS_S_HOME_MAX; t++) {
    taken_s[t] = 0;
  }

  assigned = 0;
  while (assigned < MIPS_S_HOME_EMIT) {
    int      best;
    unsigned best_uses;

    best = -1;
    best_uses = 1;
    for (i = 0; i < mips_alloc_ct; i++) {
      const char *nm;

      if (mips_alloc_tab[i].home != (unsigned) -1) {
        continue;
      }
      if (!mips_alloc_owner_ok(mips_alloc_tab[i].owner)) {
        continue;
      }
      if (mips_alloc_tab[i].is_formal || !mips_alloc_tab[i].is_nested) {
        continue;
      }
      nm = variable_name_get(mips_alloc_tab[i].v);
      if (mips_alloc_name_is_temp(nm)) {
        continue;
      }
      if (mips_alloc_tab[i].uses > best_uses) {
        best_uses = mips_alloc_tab[i].uses;
        best = (int) i;
      }
    }
    if (best < 0) {
      break;
    }
    for (t = 0; t < MIPS_S_HOME_EMIT; t++) {
      if (!taken_s[t]) {
        mips_alloc_tab[best].home = MIPS_S_HOME_REG(t);
        taken_s[t] = 1;
        assigned++;
        break;
      }
    }
  }

  /* Remaining $s: file-level single_writer (keep gated by callee writes). */
  while (assigned < MIPS_S_HOME_EMIT) {
    int      best;
    unsigned best_uses;

    best = -1;
    best_uses = 1;
    for (i = 0; i < mips_alloc_ct; i++) {
      const char *nm;

      if (mips_alloc_tab[i].home != (unsigned) -1) {
        continue;
      }
      if (!mips_alloc_owner_ok(mips_alloc_tab[i].owner)) {
        continue;
      }
      if (mips_alloc_tab[i].is_formal || mips_alloc_tab[i].is_nested) {
        continue;
      }
      if (!mips_alloc_tab[i].single_writer) {
        continue;
      }
      nm = variable_name_get(mips_alloc_tab[i].v);
      if (mips_alloc_name_is_temp(nm)) {
        continue;
      }
      /* Skip `_` file-level (shared regression helpers) — 5j class. */
      if (!nm || (nm[0] == '_')) {
        continue;
      }
      if (mips_alloc_tab[i].uses > best_uses) {
        best_uses = mips_alloc_tab[i].uses;
        best = (int) i;
      }
    }
    if (best < 0) {
      break;
    }
    for (t = 0; t < MIPS_S_HOME_EMIT; t++) {
      if (!taken_s[t]) {
        mips_alloc_tab[best].home = MIPS_S_HOME_REG(t);
        taken_s[t] = 1;
        assigned++;
        break;
      }
    }
  }

  /* Caller-saved: greedy by uses, allowing interval reuse. */
  {
    unsigned char refuse[MIPS_ALLOC_MAX];

    for (i = 0; i < MIPS_ALLOC_MAX; i++) {
      refuse[i] = 0;
    }
    for (;;) {
      int      best;
      unsigned best_uses;
      unsigned reg;
      unsigned found;

      best = -1;
      best_uses = 1;
      for (i = 0; i < mips_alloc_ct; i++) {
        const char *nm;

        if (refuse[i] || (mips_alloc_tab[i].home != (unsigned) -1)) {
          continue;
        }
        if (!mips_alloc_owner_ok(mips_alloc_tab[i].owner)) {
          continue;
        }
        if (mips_alloc_tab[i].is_formal) {
          continue;
        }
        nm = variable_name_get(mips_alloc_tab[i].v);
        if (mips_alloc_name_is_temp(nm)) {
          continue;
        }
        if (!nm || (nm[0] == '_')) {
          continue;
        }
        if ((0 == mips_alloc_tab[i].first_use)
            || (0 == mips_alloc_tab[i].last_use)) {
          continue;
        }
        if (mips_alloc_tab[i].uses > best_uses) {
          best_uses = mips_alloc_tab[i].uses;
          best = (int) i;
        }
      }
      if (best < 0) {
        break;
      }
      found = 0;
      for (t = 0; t < MIPS_ALLOC_CALLER_CT; t++) {
        reg = mips_alloc_caller_pool[t];
        if (mips_alloc_caller_reg_free(reg, mips_alloc_tab[best].first_use,
            mips_alloc_tab[best].last_use)) {
          mips_alloc_tab[best].home = reg;
          found = 1;
          break;
        }
      }
      if (!found) {
        refuse[best] = 1;
      }
    }
  }
}

unsigned mips_alloc_home_reg(variable_t v)
{
  variable_t master;
  int        ix;

  if (!v) {
    return (unsigned) -1;
  }
  master = v;
  while (variable_master_get(master)) {
    master = variable_master_get(master);
  }
  ix = mips_alloc_find(master);
  if (ix < 0) {
    return (unsigned) -1;
  }
  return mips_alloc_tab[ix].home;
}

int mips_alloc_home_is_live(variable_t v)
{
  variable_t master;
  int        ix;

  if (!v) {
    return 0;
  }
  master = v;
  while (variable_master_get(master)) {
    master = variable_master_get(master);
  }
  ix = mips_alloc_find(master);
  if (ix < 0) {
    return 0;
  }
  if (mips_alloc_tab[ix].home == (unsigned) -1) {
    return 0;
  }
  return mips_alloc_tab[ix].live;
}

void mips_alloc_home_set_live(variable_t v)
{
  variable_t master;
  int        ix;

  if (!v) {
    return;
  }
  master = v;
  while (variable_master_get(master)) {
    master = variable_master_get(master);
  }
  ix = mips_alloc_find(master);
  if (ix < 0) {
    return;
  }
  if (mips_alloc_tab[ix].home != (unsigned) -1) {
    mips_alloc_tab[ix].live  = 1;
    mips_alloc_tab[ix].dirty = 0;
  }
}

void mips_alloc_home_set_dirty(variable_t v)
{
  variable_t master;
  int        ix;

  if (!v) {
    return;
  }
  master = v;
  while (variable_master_get(master)) {
    master = variable_master_get(master);
  }
  ix = mips_alloc_find(master);
  if (ix < 0) {
    return;
  }
  if (mips_alloc_tab[ix].home != (unsigned) -1) {
    mips_alloc_tab[ix].live  = 1;
    mips_alloc_tab[ix].dirty = 1;
  }
}

void mips_alloc_home_invalidate(variable_t v)
{
  variable_t master;
  int        ix;

  if (!v) {
    return;
  }
  master = v;
  while (variable_master_get(master)) {
    master = variable_master_get(master);
  }
  ix = mips_alloc_find(master);
  if (ix < 0) {
    return;
  }
  mips_alloc_tab[ix].live  = 0;
  mips_alloc_tab[ix].dirty = 0;
}

void mips_alloc_home_flush(pfile_t *pf, variable_t v)
{
  variable_t master;
  int        ix;

  if (!v) {
    return;
  }
  master = v;
  while (variable_master_get(master)) {
    master = variable_master_get(master);
  }
  ix = mips_alloc_find(master);
  if (ix < 0) {
    return;
  }
  if (!mips_alloc_tab[ix].dirty) {
    return;
  }
  if (mips_alloc_tab[ix].home == (unsigned) -1) {
    mips_alloc_tab[ix].dirty = 0;
    return;
  }
  if (!pf) {
    pf = mips_alloc_pf;
  }
  if (pf) {
    mips_alloc_emit_home_spill(pf, mips_alloc_tab[ix].v,
        mips_alloc_tab[ix].home);
  }
  mips_alloc_tab[ix].dirty = 0;
}

static void mips_alloc_dump(pfile_t *pf)
{
  unsigned i;
  unsigned n_home;
  unsigned n_s;
  unsigned n_dirty_cap;
  unsigned n_sw;

  UNUSED(pf);
  n_home = n_s = n_dirty_cap = n_sw = 0;
  for (i = 0; i < mips_alloc_ct; i++) {
    if (mips_alloc_tab[i].single_writer && !mips_alloc_tab[i].is_formal) {
      n_sw++;
    }
    if (mips_alloc_tab[i].home != (unsigned) -1) {
      n_home++;
      n_dirty_cap++;
      if (mips_alloc_reg_is_callee_home(mips_alloc_tab[i].home)) {
        n_s++;
      }
    }
  }
  fprintf(stderr,
      "mips_alloc: eligible=%u homes=%u s_homes=%u single_writer=%u\n",
      mips_alloc_ct, n_home, n_s, n_sw);
  for (i = 0; i < mips_alloc_ct; i++) {
    const char *nm;

    if (mips_alloc_tab[i].home == (unsigned) -1
        && mips_alloc_tab[i].uses < 2) {
      continue;
    }
    nm = variable_name_get(mips_alloc_tab[i].v);
    if (mips_alloc_tab[i].home == (unsigned) -1) {
      fprintf(stderr,
          "  %s home=spill uses=%u sw=%d nested=%d [%u..%u]\n",
          nm ? nm : "?", mips_alloc_tab[i].uses,
          mips_alloc_tab[i].single_writer, mips_alloc_tab[i].is_nested,
          mips_alloc_tab[i].first_use, mips_alloc_tab[i].last_use);
    } else {
      fprintf(stderr,
          "  %s home=$%u uses=%u sw=%d nested=%d [%u..%u]\n",
          nm ? nm : "?", mips_alloc_tab[i].home, mips_alloc_tab[i].uses,
          mips_alloc_tab[i].single_writer, mips_alloc_tab[i].is_nested,
          mips_alloc_tab[i].first_use, mips_alloc_tab[i].last_use);
    }
  }
}

void mips_alloc_run(pfile_t *pf, cmd_t cmd_head)
{
  pfile_proc_t *proc;
  const char   *dump;

  mips_alloc_reset();
  mips_alloc_pf = pf;
  if (!pf) {
    return;
  }
  for (proc = pfile_proc_root_get(pf);
       proc;
       proc = pfile_proc_next(proc)) {
    pfile_block_t *blk;

    for (blk = pfile_proc_block_root_get(proc);
         blk;
         blk = pfile_block_next(blk)) {
      variable_t v;

      for (v = pfile_block_variable_list_head(blk);
           v;
           v = variable_link_get(v)) {
        mips_alloc_add(pf, v, proc);
      }
    }
    {
      size_t   ii;
      unsigned n;

      n = (unsigned) pfile_proc_param_ct_get(proc);
      for (ii = 0; ii < n; ii++) {
        value_t pv;

        pv = pfile_proc_param_get(proc, ii);
        if (pv) {
          mips_alloc_add(pf, value_variable_get(pv), proc);
        }
      }
    }
  }
  mips_alloc_count_uses(cmd_head);
  mips_alloc_assign_homes();
  mips_alloc_pf_set(pf);
  dump = getenv("JALV2_MIPS_ALLOC_DUMP");
  if (dump && dump[0] && (dump[0] != '0')) {
    mips_alloc_dump(pf);
  }
}
