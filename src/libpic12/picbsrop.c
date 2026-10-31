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
#define PIC_BSR_WL_MAXRND 20000U

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

  UNUSED(reached);
  *n_succ   = 0U;
  pv0       = (0 != (sid % 2U)) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  b_in      = in[sid];
  if (b_in == PIC_BSR_IN_UNINIT) {
    return;
  }
  /* (the GOP-era PRE STOP that used to open this function is removed:
   * in a worklist it suppressed a state's out-edges every round, so a
   * successor needing this state's contribution merged only its other
   * in-edges -- with in-place propagation the worklist terminates on
   * its own and the stop is neither needed nor sound) */
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

/* Labels that are the target of a CALL instruction.  They are entry points
 * just like procedures: the caller's BSR is arbitrary, so the first banked
 * op of their body must select its own bank and the MOVLB that does it may
 * never be removed.  Compiler-synthesized helpers (`_pic_indirect`,
 * `_pic_multiply`, the float/wide kernels, ...) are ordinary labels, not
 * pfile procs, so the proc-entry protection alone missed them -- the j50
 * float demo read `_pic_pointer+1` through a stale BSR and jumped through a
 * garbage indirect-call target (2026-10-30). */
static int pic_bsr_lbl_cmp(const void *A, const void *B)
{
  uintptr_t a;
  uintptr_t b;

  a = (uintptr_t) *(const label_t *) A;
  b = (uintptr_t) *(const label_t *) B;
  return (a < b) ? -1 : ((a > b) ? 1 : 0);
}

static label_t *pic_bsr_call_lbl;
static size_t   pic_bsr_call_lbl_ct;

/* A4 teardown between compiles: release the CALL-target label table.
 * pic_bsr_call_targets_build() rebuilds it per pass but nothing freed it
 * at the end of a compile. */
void pic_bsr_state_reset(void)
{
  FREE(pic_bsr_call_lbl);
  pic_bsr_call_lbl    = 0;
  pic_bsr_call_lbl_ct = 0;
}

static void pic_bsr_call_targets_build(pfile_t *pf)
{
  pic_code_t code;
  size_t     n;

  FREE(pic_bsr_call_lbl);
  pic_bsr_call_lbl    = 0;
  pic_bsr_call_lbl_ct = 0;
  n = 0;
  for (code = pic_code_list_head_get(pf); code;
       code = pic_code_next_get(code)) {
    if (PIC_OPCODE_CALL == pic_code_op_get(code)) {
      n++;
    }
  }
  if (0 == n) {
    return;
  }
  pic_bsr_call_lbl = (label_t *) MALLOC(n * sizeof(label_t));
  if (!pic_bsr_call_lbl) {
    return;
  }
  for (code = pic_code_list_head_get(pf); code;
       code = pic_code_next_get(code)) {
    if (PIC_OPCODE_CALL == pic_code_op_get(code)) {
      label_t lbl;

      lbl = pic_code_brdst_get(code);
      if ((LABEL_NONE != lbl) && (pic_bsr_call_lbl_ct < n)) {
        pic_bsr_call_lbl[pic_bsr_call_lbl_ct++] = lbl;
      }
    }
  }
  qsort(pic_bsr_call_lbl, pic_bsr_call_lbl_ct, sizeof(label_t),
      pic_bsr_lbl_cmp);
}

static void pic_bsr_call_targets_free(void)
{
  FREE(pic_bsr_call_lbl);
  pic_bsr_call_lbl    = 0;
  pic_bsr_call_lbl_ct = 0;
}

static boolean_t pic_bsr_label_is_call_target(pfile_t *pf, label_t lbl)
{
  size_t lo;
  size_t hi;

  UNUSED(pf);
  /* the array is sorted at build time; membership is checked once per
   * candidate MOVLB, so keep it O(log n) */
  lo = 0;
  hi = pic_bsr_call_lbl_ct;
  while (lo < hi) {
    size_t mid;

    mid = lo + (hi - lo) / 2;
    if (pic_bsr_call_lbl[mid] == lbl) {
      return BOOLEAN_TRUE;
    }
    if ((uintptr_t) pic_bsr_call_lbl[mid] < (uintptr_t) lbl) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return BOOLEAN_FALSE;
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
  /* nb: CALL targets are not seeded INDETERMINATE here either -- their
   * entry MOVLB is never removed (pic_bsr_movlb_is_proc_entry), i.e. the
   * bank is re-established before any downstream op depends on it. */
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
  /* nb: CALL targets are deliberately NOT added to the BFS entry set.
   * They are reached through the linear list (the helper bodies are laid out
   * contiguously), and seeding every call target explodes the state worklist
   * on big programs (18f67j50_fat32_usb_msd_pata ~11x slower).  The entry
   * MOVLB protection below is what keeps their prologue correct. */
  pic_bsr_wl_bfs(pf, at, n, map, reached, nst, ent, ent_ct);
  for (s = 0U; s < nst; s++) {
    in_cur[s]  = PIC_BSR_IN_UNINIT;
  }
  pic_bsr_wl_entry_inits(pf, map, (size_t) n, in_cur, nst);
  /* Dirty-state FIFO worklist. The previous round-robin form advanced
   * exactly one hop per round (xfr read the previous round's array), so
   * loops and nested callee chains needed far more rounds than the cap
   * -- the truncated fixpoint dropped later-arriving edges, label merges
   * "converged" without the loop-body contribution, and post_analyze
   * removed load-bearing MOVLBs (the 16F1825 string-compare defect:
   * `var string == <literal>` read false with byte-exact content).
   * Raising the cap alone fixed correctness but made the fat32/mp3
   * giants explode (thousands of full sweeps). This processes ONLY
   * states whose in[] changed: correct by construction and linear in
   * relabelings. The lattice is monotone toward INDETERMINATE, so every
   * state changes value at most twice -- the loop terminates without
   * the cap (kept as a safety valve). */
  {
    unsigned *queue;
    boolean_t *queued;
    unsigned qh;
    unsigned qt;
    unsigned processed;

    queue   = (unsigned *) MALLOC(((size_t) nst + 1U) * sizeof (unsigned));
    queued  = (boolean_t *) MALLOC((size_t) nst * sizeof (boolean_t));
    if (queue && queued) {
      for (s = 0U; s < nst; s++) {
        queued[s] = BOOLEAN_FALSE;
      }
      qh  = 0U;
      qt  = 0U;
      /* seed: every reached state with an entry value (RESET/ISR/...) */
      for (s = 0U; s < nst; s++) {
        if (reached[s] && (PIC_BSR_IN_UNINIT != in_cur[s])) {
          queue[qt++]      = s;
          queued[s]        = BOOLEAN_TRUE;
        }
      }
      processed = 0U;
      while ((qh != qt) && (processed < PIC_BSR_WL_MAXRND)) {
        s = queue[qh++];
        if (qh > nst) {
          qh = 0U;
        }
        queued[s]  = BOOLEAN_FALSE;
        processed++;
        ns  = 0U;
        pic_bsr_wl_xfr(
          pf, at[s / 2U], n, map, reached, in_cur, s, &ns, &ss[0], &sb[0]);
        for (kx = 0U; kx < ns; kx++) {
          uchar  before;

          if (ss[kx] >= nst) {
            continue;
          }
          before  = in_cur[ss[kx]];
          pic_bsr_in_merge(&in_cur[ss[kx]], sb[kx]);
          if ((in_cur[ss[kx]] != before)
              && !queued[ss[kx]]
              && reached[ss[kx]]) {
            /* ring capacity nst+1 with at most nst entries queued: never full */
            queue[qt++]      = ss[kx];
            if (qt > nst) {
              qt = 0U;
            }
            queued[ss[kx]]  = BOOLEAN_TRUE;
          }
        }
      }
    }
    FREE(queued);
    FREE(queue);
  }
  UNUSED(chg);
  UNUSED(rnd);
  UNUSED(u);
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
      return pic_bsr_label_is_call_target(pf, lbl);
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
  pic_bsr_call_targets_build(pf);
  pic_code_bsr_annotation_reset(pf);
  if (0U != pfile_errct_get(pf)) {
    pfile_log(pf, PFILE_LOG_DEBUG, "...bsr: skip (pfile errors)");
    pfile_log(pf, PFILE_LOG_DEBUG, "...post analyze");
    (void) pic_code_bsr_post_analyze(pf);
    pic_bsr_call_targets_free();
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
  pic_bsr_call_targets_free();
}
