/************************************************************
 **
 ** picbsrop.c : PIC data optimization definitions
 **
 ** Copyright (c) 2007, Kyle A. York
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: fork extensions (heavy rework)
 **
 ************************************************************/
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../libcore/pf_proc.h"
#include "../libcore/value.h"
#include "../libutils/mem.h"
#include "pic.h"
#include "pic_msg.h"
#include "piccolst.h"
#include "pic_gop.h"
#include "picbsrop.h"

/* in[][] before first merge from predecessors (separate from PIC_BSR_UNKNOWN) */
#define PIC_BSR_IN_UNINIT ((uchar) 0xfd)
#define PIC_BSR_WL_MAXRND 400U

typedef struct pic_bsr_ci_ {
  pic_code_t  c;
  unsigned    i;
} pic_bsr_ci_t;

static int pic_bsr_ci_cmp(const void *A, const void *B)
{
  uintptr_t a;
  uintptr_t b;

  a = (uintptr_t) ((const pic_bsr_ci_t *) A)->c;
  b = (uintptr_t) ((const pic_bsr_ci_t *) B)->c;
  if (a < b) {
    return -1;
  }
  if (a > b) {
    return 1;
  }
  return 0;
}

static unsigned pic_bsr_ixof(const pic_bsr_ci_t *m, size_t n, pic_code_t c)
{
  size_t lo;
  size_t hi;

  lo = 0;
  hi = n;
  while (lo < hi) {
    size_t   mid;
    uintptr_t mc;
    uintptr_t cc;

    mid = (lo + hi) / 2U;
    mc  = (uintptr_t) m[mid].c;
    cc  = (uintptr_t) c;
    if (mc < cc) {
      lo = mid + 1U;
    } else if (mc > cc) {
      hi = mid;
    } else {
      return m[mid].i;
    }
  }
  assert(0);
  return (unsigned) n;
}

static void pic_bsr_in_merge(uchar *slot, uchar b)
{
  if (b == PIC_BSR_IN_UNINIT) {
    return;
  }
  if (*slot == PIC_BSR_IN_UNINIT) {
    *slot = b;
  } else if (*slot == b) {
    return;
  } else if (PIC_BSR_INDETERMINATE == *slot || PIC_BSR_INDETERMINATE == b) {
    *slot = PIC_BSR_INDETERMINATE;
  } else {
    *slot = PIC_BSR_INDETERMINATE;
  }
}

static uchar pic_bsr_g_line(
  unsigned i, const uchar *in, const boolean_t *reached)
{
  unsigned   s0;
  unsigned   s1;
  uchar     a0;
  uchar     a1;

  s0    = 2U * i;
  s1    = s0 + 1U;
  a0    = in[s0];
  a1    = in[s1];
  if (!reached[s0] && !reached[s1]) {
    return PIC_BSR_IN_UNINIT;
  }
  if (reached[s0] && !reached[s1]) {
    return a0;
  }
  if (!reached[s0] && reached[s1]) {
    return a1;
  }
  if (a0 == PIC_BSR_IN_UNINIT) {
    return a1;
  }
  if (a1 == PIC_BSR_IN_UNINIT) {
    return a0;
  }
  if (a0 == a1) {
    return a0;
  }
  if (a0 == PIC_BSR_UNKNOWN || a1 == PIC_BSR_UNKNOWN) {
    if (a0 == PIC_BSR_UNKNOWN && a1 == PIC_BSR_UNKNOWN) {
      return PIC_BSR_UNKNOWN;
    }
    return PIC_BSR_INDETERMINATE;
  }
  return PIC_BSR_INDETERMINATE;
}

/* Propagate b along edges from s; n_succ and succ_* are outputs. */
static void pic_bsr_wl_xfr(
  pfile_t    *pf,
  pic_code_t  code,
  unsigned    n_codes,
  const pic_bsr_ci_t *map,
  const boolean_t *reached,
  const uchar *in,
  unsigned    sid,
  unsigned   *n_succ,
  unsigned   *succ_sid,
  uchar      *succ_b)
{
  boolean_t     pv0;
  uchar        b_in;
  pic_opcode_t op;
  unsigned     k;
  unsigned     w;
  pic_bsr_gop_succ_t o[3];
  uchar        b_flow;
  uchar        b_pre_post; /* after PRE, before POST */
  uchar        b_post;

  *n_succ   = 0U;
  pv0       = (0 != (sid % 2U)) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  b_in      = in[sid];
  if (b_in == PIC_BSR_IN_UNINIT) {
    return;
  }
  {
    unsigned  i0;
    uchar     G;
    unsigned  s0;
    unsigned  s1;

    i0 = sid / 2U;
    G  = pic_bsr_g_line(i0, in, reached);
    s0 = 2U * i0;
    s1 = s0 + 1U;
    /* GOP PRE STOP on G==b_in only when both (code,pv) halves are reachable.
     * The lone half at RESET had G==b_in with only pv=0 reachable, which
     * suppressed all outgoing edges from the first instruction (gpsim jallib). */
    if (G == b_in && reached[s0] && reached[s1]) {
      return;
    }
  }
  op  = pic_code_op_get(code);
  b_flow = b_in;
  if (PIC_OPCODE_MOVLB == op) {
    b_flow = (uchar) value_const_get(pic_code_literal_get(code));
  }
  b_pre_post = b_flow;
  b_post     = b_pre_post;
  if (pic_code_is_suspend(pf, code)
      || (PIC_OPCODE_CALL == op)
      || (PIC_OPCODE_RCALL == op)) {
    b_post = PIC_BSR_INDETERMINATE;
  }
  w = 0U;
  pic_bsr_gop_state_succ(pf, code, pv0, o, &w);
  for (k = 0; k < w; k++) {
    unsigned  tix;
    boolean_t ptv;
    unsigned  ts;
    uchar     b_edge;

    tix = pic_bsr_ixof(map, (size_t) n_codes, o[k].c);
    ptv = o[k].pv;
    ts  = 2U * tix + (ptv ? 1U : 0U);
    if ((PIC_OPCODE_CALL == op) || (PIC_OPCODE_RCALL == op)) {
      b_edge = (0U == k) ? b_pre_post : b_post;
    } else {
      b_edge = b_post;
    }
    if (*n_succ < 3U) {
      succ_sid[*n_succ]  = ts;
      succ_b[*n_succ]    = b_edge;
      *n_succ            = *n_succ + 1U;
    }
  }
}

/* Structural reach in the (code,pv) product graph. */
static void pic_bsr_wl_bfs(
  pfile_t *pf,
  const pic_code_t *at, unsigned n,
  const pic_bsr_ci_t *map,
  boolean_t *reached, unsigned nst,
  const unsigned *entry, unsigned n_entry)
{
  unsigned *q;
  unsigned  qh;
  unsigned  qt;
  unsigned  u;

  memset(reached, 0, nst * sizeof (boolean_t));
  q  = (unsigned *) MALLOC((size_t) nst * sizeof (unsigned));
  if (!q) {
    return;
  }
  qh  = 0U;
  qt  = 0U;
  for (u = 0U; u < n_entry; u++) {
    unsigned e;

    e = entry[u];
    if (e < nst && !reached[e]) {
      reached[e]  = BOOLEAN_TRUE;
      assert(qt < nst);
      q[qt++]  = e;
    }
  }
  while (qh < qt) {
    unsigned  sid;
    pic_code_t c;
    boolean_t  pv0;
    pic_bsr_gop_succ_t o[3];
    unsigned   w;
    unsigned   k;

    sid  = q[qh++];
    c    = at[sid / 2U];
    pv0  = (0 != (sid % 2U)) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
    w    = 0U;
    pic_bsr_gop_state_succ(pf, c, pv0, o, &w);
    for (k = 0; k < w; k++) {
      unsigned  tix;
      unsigned  ts;

      tix  = pic_bsr_ixof(map, n, o[k].c);
      ts   = 2U * tix + (o[k].pv ? 1U : 0U);
      if (ts < nst && !reached[ts]) {
        reached[ts]  = BOOLEAN_TRUE;
        if (qt < nst) {
          q[qt++]  = ts;
        }
      }
    }
  }
  FREE(q);
}

/* Same entry BSR as pic_code_bsr_opt + pic_code_gop: reset=0, isr/indirect=INDET. */
static void pic_bsr_wl_entry_inits(
  pfile_t *pf, const pic_bsr_ci_t *map, size_t n, uchar *in, unsigned nst)
{
  label_t  lbl;
  pfile_proc_t *proc;
  pic_code_t code;
  unsigned  s;

  (void) nst;
  lbl  = pfile_label_find(pf, PFILE_LOG_ERR, PIC_LABEL_RESET);
  if (lbl) {
    code  = pic_code_label_find(pf, lbl);
    if (code) {
      s  = 2U * pic_bsr_ixof(map, n, code) + 0U;
      /* Match GOP-era RESET entry (bank 0). UNKNOWN was tried to avoid
       * over-removing MOVLB before early high-bank SFR init on some PIC18K22
       * parts; that pessimism bloated ROM. Redundant MOVLB removal uses the
       * worklist fixpoint; keep entry at 0 like pic_code_bsr_opt WHO_USER. */
      pic_bsr_in_merge(&in[s], 0);
    }
    label_release(lbl);
  }
  lbl  = pfile_label_find(pf, PFILE_LOG_NONE, PIC_LABEL_ISR);
  if (lbl) {
    code  = pic_code_label_find(pf, lbl);
    if (code) {
      s  = 2U * pic_bsr_ixof(map, n, code) + 0U;
      pic_bsr_in_merge(&in[s], PIC_BSR_INDETERMINATE);
    }
    label_release(lbl);
  }
  for (proc = pfile_proc_root_get(pf);
       proc;
       proc = pfile_proc_next(proc)) {
    if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INDIRECT)
        || pfile_proc_flag_test(proc, PFILE_PROC_FLAG_TASK)) {
      lbl  = pfile_proc_label_get(proc);
      if (lbl) {
        code  = (pic_code_t) label_code_get(lbl);
        if (code) {
          s  = 2U * pic_bsr_ixof(map, n, code) + 0U;
          pic_bsr_in_merge(&in[s], PIC_BSR_INDETERMINATE);
        }
      }
    }
  }
}

/* CFG worklist fixpoint: same (code, pv) semantics as pic_gop, without DFS stack. */
static void pic_code_bsr_wl(pfile_t *pf)
{
  pic_code_t  code;
  unsigned   n;
  unsigned   nst;
  unsigned   s;
  unsigned   t;
  unsigned   u;
  pic_code_t *at;
  pic_bsr_ci_t *map;
  uchar     *in_cur;
  uchar     *in_nxt;
  boolean_t *reached;
  unsigned  *ent;
  unsigned  ent_ct;
  unsigned  rnd;
  int       chg;
  label_t  lbl;
  pfile_proc_t *proc;
  unsigned  ns;
  unsigned  ss[3U];
  uchar     sb[3U];
  unsigned  kx;

  n  = 0U;
  for (code = pic_code_list_head_get(pf); code; code = pic_code_next_get(code)) {
    n++;
  }
  if (0U == n) {
    return;
  }
  nst  = 2U * n;
  at   = (pic_code_t *) MALLOC((size_t) n * sizeof (pic_code_t));
  map  = (pic_bsr_ci_t *) MALLOC((size_t) n * sizeof (pic_bsr_ci_t));
  in_cur  = (uchar *) MALLOC((size_t) nst * sizeof (uchar));
  in_nxt  = (uchar *) MALLOC((size_t) nst * sizeof (uchar));
  reached  = (boolean_t *) MALLOC((size_t) nst * sizeof (boolean_t));
  /* entry states: RESET + ISR + every proc label (all-proc BSR-indeterminate
   * entry) -- size by the proc count, big libraries exceed any fixed cap
   * (the network/mp3 corpus samples found the 64-entry heap overflow) */
  {
    unsigned ent_need;

    ent_need  = 2U;
    for (proc = pfile_proc_root_get(pf); proc;
        proc = pfile_proc_next(proc)) {
      ent_need++;
    }
    ent  = (unsigned *) MALLOC((size_t) ent_need * sizeof (unsigned));
  }
  if (!at || !map || !in_cur || !in_nxt || !reached || !ent) {
    FREE(ent);
    FREE(reached);
    FREE(in_nxt);
    FREE(in_cur);
    FREE(map);
    FREE(at);
    return;
  }
  n    = 0U;
  for (code = pic_code_list_head_get(pf); code; code = pic_code_next_get(code)) {
    at[n]      = code;
    map[n].c   = code;
    map[n].i   = n;
    n++;
  }
  qsort(map, (size_t) n, sizeof (pic_bsr_ci_t), pic_bsr_ci_cmp);
  ent_ct  = 0U;
  lbl  = pfile_label_find(pf, PFILE_LOG_NONE, PIC_LABEL_ISR);
  if (lbl) {
    pic_code_t ic;

    ic  = pic_code_label_find(pf, lbl);
    if (ic) {
      ent[ent_ct++]  = 2U * pic_bsr_ixof(map, (size_t) n, ic) + 0U;
    }
    label_release(lbl);
  }
  lbl  = pfile_label_find(pf, PFILE_LOG_ERR, PIC_LABEL_RESET);
  if (lbl) {
    pic_code_t uc;

    uc  = pic_code_label_find(pf, lbl);
    if (uc) {
      ent[ent_ct++]  = 2U * pic_bsr_ixof(map, (size_t) n, uc) + 0U;
    }
    label_release(lbl);
  }
  for (proc = pfile_proc_root_get(pf);
       proc;
       proc = pfile_proc_next(proc)) {
    if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INDIRECT)
        || pfile_proc_flag_test(proc, PFILE_PROC_FLAG_TASK)) {
      lbl  = pfile_proc_label_get(proc);
      if (lbl) {
        code  = (pic_code_t) label_code_get(lbl);
        if (code) {
          ent[ent_ct++]  = 2U * pic_bsr_ixof(map, (size_t) n, code) + 0U;
        }
      }
    }
  }
  pic_bsr_wl_bfs(pf, at, n, map, reached, nst, ent, ent_ct);
  for (s = 0U; s < nst; s++) {
    in_cur[s]  = PIC_BSR_IN_UNINIT;
  }
  pic_bsr_wl_entry_inits(pf, map, (size_t) n, in_cur, nst);
  rnd  = 0U;
  chg  = 1;
  while (0 != chg && rnd < PIC_BSR_WL_MAXRND) {
    rnd++;
    for (t = 0U; t < nst; t++) {
      in_nxt[t]  = PIC_BSR_IN_UNINIT;
    }
    pic_bsr_wl_entry_inits(pf, map, (size_t) n, in_nxt, nst);
    for (s = 0U; s < nst; s++) {
      if (!reached[s] || in_cur[s] == PIC_BSR_IN_UNINIT) {
        continue;
      }
      ns  = 0U;
      pic_bsr_wl_xfr(
        pf, at[s / 2U], n, map, reached, in_cur, s, &ns, &ss[0], &sb[0]);
      for (kx = 0U; kx < ns; kx++) {
        if (ss[kx] < nst) {
          pic_bsr_in_merge(&in_nxt[ss[kx]], sb[kx]);
        }
      }
    }
    chg  = 0;
    for (u = 0U; u < nst; u++) {
      if (in_nxt[u] != in_cur[u]) {
        chg  = 1;
        break;
      }
    }
    for (u = 0U; u < nst; u++) {
      in_cur[u]  = in_nxt[u];
    }
  }
  for (t = 0U; t < n; t++) {
    uchar G;

    G  = pic_bsr_g_line(t, in_cur, reached);
    if (G != PIC_BSR_IN_UNINIT) {
      pic_code_bsr_set(at[t], G);
    }
  }
  FREE(ent);
  FREE(reached);
  FREE(in_nxt);
  FREE(in_cur);
  FREE(map);
  FREE(at);
}

/* Clear BSR state on every instruction so a new dataflow pass starts fresh. */
static void pic_code_bsr_annotation_reset(pfile_t *pf)
{
  pic_code_t code;

  for (code = pic_code_list_head_get(pf); code; code = pic_code_next_get(code)) {
    pic_code_bsr_set(code, PIC_BSR_UNKNOWN);
  }
}

/* A MOVLB in the straight-line prologue of a proc (from the entry label
 * to the first control-flow op) executes with the CALLER's arbitrary BSR:
 * the runtime call edge is invisible to the linear fixpoint, which only
 * sees the physical list predecessor. Never remove such a MOVLB -- the
 * prologue's first banked op depends on it (the j50 stale-print defect).
 * MOVLBs after any other label, or below control flow, are ordinary CFG
 * merges and stay optimizable. */
static boolean_t pic_bsr_movlb_is_proc_entry(pfile_t *pf, pic_code_t code)
{
  pic_code_t    pr;
  pfile_proc_t *proc;

  /* walk back through the prologue: only non-control-flow instructions
   * may separate the entry label from this MOVLB (e.g. a leading MOVLW
   * or an access-mode op). Any control flow, another MOVLB, or a
   * non-entry label ends the protected region. */
  for (pr = pic_code_prev_get(code); pr; pr = pic_code_prev_get(pr)) {
    pic_opcode_t op;

    op = pic_code_op_get(pr);
    /* label instructions are PIC_OPCODE_NONE with the label attached */
    if ((PIC_OPCODE_NONE == op) && (LABEL_NONE != pic_code_label_get(pr))) {
      label_t lbl;

      lbl = pic_code_label_get(pr);
      for (proc = pfile_proc_root_get(pf); proc;
          proc = pfile_proc_next(proc)) {
        if (pfile_proc_label_get(proc) == lbl) {
          return BOOLEAN_TRUE;
        }
      }
      return BOOLEAN_FALSE;
    }
    switch (op) {
      case PIC_OPCODE_GOTO:
      case PIC_OPCODE_CALL:
      case PIC_OPCODE_RETURN:
      case PIC_OPCODE_RETLW:
      case PIC_OPCODE_RETFIE:
      case PIC_OPCODE_RESET:
      case PIC_OPCODE_BRA:
      case PIC_OPCODE_BTFSC:
      case PIC_OPCODE_BTFSS:
      case PIC_OPCODE_DECFSZ:
      case PIC_OPCODE_INCFSZ:
      case PIC_OPCODE_DCFSNZ:
      case PIC_OPCODE_INFSNZ:
      case PIC_OPCODE_TSTFSZ:
      case PIC_OPCODE_MOVLB:
        return BOOLEAN_FALSE;
      default:
        break;
    }
  }
  return BOOLEAN_FALSE;
}

static unsigned pic_code_bsr_post_analyze(pfile_t *pf)
{
  pic_code_t code;
  unsigned   ins_ct; /* instruction ct */
  unsigned   rem_ct;

  for (ins_ct = 0, rem_ct = 0, code = pic_code_list_head_get(pf);
       code;
       ins_ct++) {
    pic_code_t next;

    next  = pic_code_next_get(code);
    if (!pic_code_flag_test(code, PIC_CODE_FLAG_NO_OPTIMIZE)) {
      boolean_t  cremove;

      switch (pic_code_op_get(code)) {
        case PIC_OPCODE_MOVLB:
          /* Incoming BSR at this line (from worklist fixpoint) matches the
           * literal: the MOVLB is redundant (same rule as GOP-era pass) --
           * unless it anchors a proc entry (caller BSR is arbitrary). */
          cremove = (value_const_get(pic_code_literal_get(code))
              == pic_code_bsr_get(code))
              && !pic_bsr_movlb_is_proc_entry(pf, code);
          break;
        default:
          cremove = BOOLEAN_FALSE;
          break;
      }
      if (cremove) {
        pic_code_list_remove(pf, code);
        pic_code_free(code);
        rem_ct++;
      }
    }
    code  = next;
  }
  pfile_log(
    pf, PFILE_LOG_DEBUG, "...removed %u of %u instructions", rem_ct, ins_ct);
  return rem_ct;
}

/* Removes unneeded movlb instructions (CFG worklist + pic_code_bsr_post_analyze). */
void pic_code_bsr_optimize(pfile_t *pf)
{
  unsigned round;
  unsigned tot_rem;

  pfile_log(pf, PFILE_LOG_DEBUG, PIC_MSG_FIXING_BSR);
  pic_code_bsr_annotation_reset(pf);
  if (0U != pfile_errct_get(pf)) {
    pfile_log(pf, PFILE_LOG_DEBUG, "...bsr: skip (pfile errors)");
    pfile_log(pf, PFILE_LOG_DEBUG, "...post analyze");
    (void) pic_code_bsr_post_analyze(pf);
    return;
  }
  tot_rem = 0U;
  for (round = 0U; round < 12U; round++) {
    unsigned rem;

    pic_code_bsr_annotation_reset(pf);
    pic_code_bsr_wl(pf);
    pfile_log(pf, PFILE_LOG_DEBUG, "...post analyze (round %u)", round);
    rem = pic_code_bsr_post_analyze(pf);
    tot_rem += rem;
    if (0U == rem) {
      break;
    }
  }
  if (tot_rem > 0U) {
    pfile_log(
      pf, PFILE_LOG_DEBUG, "...bsr: removed %u redundant MOVLB total", tot_rem);
  }
}
