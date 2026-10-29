/************************************************************
 **
 ** wide_alg.h : target-independent wide MUL / DIV / MOD
 **
 ** The wide multiply and restoring-division algorithms exist
 ** exactly once, here, programmed against a table of back-end
 ** primitives (walg_ops_t).  A back-end supplies the ISA-level
 ** pieces: byte chains, bit extraction/insertion idioms,
 ** branches and labels.  Everything algorithmic -- loop
 ** structure, dst-alias temp copying, sign handling, the
 ** zero-divisor skip -- is owned by this module so a fix can
 ** no longer land on one target and miss the other.
 **
 ** The DIV loop-state registers are back-end assigned but
 ** kernel visible: reg_flag carries the ult/or_acc result,
 ** reg_ctr is the bit counter driven by movi/addi.
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#ifndef wide_alg_h__
#define wide_alg_h__

#include "pfile.h"
#include "operator.h"
#include "value.h"
#include "label.h"

typedef void *walg_br_t;    /* pending branch */
typedef void *walg_lab_t;   /* label bound at the current position */

typedef struct walg_ops_s {
  /* kernel loop-state registers (back-end assigned) */
  unsigned reg_flag;                 /* ult / or_acc result (0/1)   */
  unsigned reg_ctr;                  /* division bit counter        */
  /* queries */
  unsigned    (*val_sz)(value_t val);
  unsigned    (*mul_width)(pfile_t *pf, value_t dst, value_t v1);
  unsigned    (*div_base_width)(pfile_t *pf, value_t dst, value_t v1);
  boolean_t   (*aliases)(value_t a, value_t b);
  /* temporaries and whole-value moves */
  value_t     (*tmp_new)(pfile_t *pf, unsigned n);
  void        (*assign)(pfile_t *pf, value_t dst, value_t src);
  /* byte chains (back-end implementations of the coarse steps) */
  void        (*zero)(pfile_t *pf, value_t val, unsigned n, value_t plain);
  void        (*or_acc)(pfile_t *pf, value_t val, unsigned n);
  void        (*neg_inplace)(pfile_t *pf, value_t val, unsigned n);
  void        (*ult)(pfile_t *pf, value_t v1, value_t v2);
  void        (*sub_wide)(pfile_t *pf, value_t dst, value_t v1, value_t v2);
  void        (*shift_left_1)(pfile_t *pf, value_t val);
  /* division bit idioms */
  void        (*sign_to_cell)(pfile_t *pf, value_t cell, value_t val,
                    unsigned n);
  walg_br_t   (*cell_bez)(pfile_t *pf, value_t cell);
  walg_br_t   (*cell_pair_xor_bez)(pfile_t *pf, value_t c1, value_t c2);
  void        (*loop_prologue)(pfile_t *pf, value_t num, value_t rem,
                    value_t quot);
  void        (*bit_load)(pfile_t *pf, value_t num);
  void        (*bit_or_low)(pfile_t *pf, value_t rem);
  void        (*bit_set)(pfile_t *pf, value_t quot);
  /* multiply idioms */
  void        (*mul_prologue)(pfile_t *pf, value_t t1, value_t t2,
                    value_t dst, unsigned n);
  void        (*mul_row_init)(void);
  void        (*mul_row_step)(pfile_t *pf, value_t t1, value_t t2,
                    value_t dst, unsigned i, unsigned j, unsigned n);
  /* register glue and control flow */
  void        (*movi)(unsigned reg, int imm);
  void        (*addi)(unsigned reg, int imm);
  walg_br_t   (*br_flag_zero)(void);
  walg_br_t   (*br_flag_nonzero)(void);
  walg_br_t   (*br_ctr_nonzero)(void);
  walg_lab_t  (*label_here)(void);
  void        (*br_target)(walg_br_t br, walg_lab_t lab);
  /* byte ALU chains (ADD / SUB / AND / OR / XOR; op1 in reg_op1,
   * op2 in the back-end's second operand register) */
  unsigned    (*alu_width)(pfile_t *pf, value_t dst, value_t v1);
  void        (*alu_prologue)(pfile_t *pf, operator_t op, value_t dst,
                    value_t v1, value_t v2);
  void        (*alu_load_op1)(pfile_t *pf, value_t v1, unsigned k,
                    unsigned s1);
  void        (*alu_load_op2)(pfile_t *pf, value_t v2, unsigned k,
                    unsigned s2);
  void        (*alu_step_add)(void);
  void        (*alu_step_sub)(void);
  void        (*alu_step_logic)(operator_t op);
  void        (*alu_store)(pfile_t *pf, value_t dst, unsigned k,
                    value_t v1);
  /* negation and increment/decrement chains -- the kernel owns the
   * read-v1[k]-before-writing-dst[k] loop order (dst may alias v1) */
  void        (*neg_prologue)(pfile_t *pf, value_t dst, value_t v1);
  void        (*neg_step)(pfile_t *pf, value_t v1, unsigned k,
                    unsigned s1);
  void        (*incr_prologue)(pfile_t *pf, value_t dst, value_t v1);
  void        (*incr_decr_step)(pfile_t *pf, operator_t op, value_t v1,
                    unsigned k, unsigned s1);
  /* const-count shifts: the kernel owns saturation policy (count that
   * swallows the width -> sign fill for arithmetic right, zeros
   * otherwise, const-folded when the source is known), the lo/hi byte
   * window indexes and the descending left walk (in-place alias
   * safety); the primitives replay the per-byte ISA sequences */
  unsigned    (*shift_width)(pfile_t *pf, value_t dst, value_t src);
  boolean_t   (*shift_is_const)(value_t src);
  unsigned    (*shift_fill_const)(value_t src, unsigned src_sz,
                    boolean_t arith);
  void        (*shift_zero_all)(pfile_t *pf, value_t dst, unsigned n,
                    value_t src);
  void        (*shift_fill_all_const)(pfile_t *pf, value_t dst,
                    unsigned n, value_t src, unsigned fill);
  void        (*shift_fill_all_runtime)(pfile_t *pf, value_t dst,
                    unsigned n, value_t src, unsigned src_sz);
  void        (*shift_const_prologue)(pfile_t *pf, value_t dst,
                    value_t src);
  void        (*shift_const_byte)(pfile_t *pf, value_t dst, value_t src,
                    int i, int lo_i, int hi_i, unsigned bit,
                    unsigned fill, boolean_t arith, boolean_t left,
                    unsigned src_sz);
  /* runtime-count shifts */
  void        (*shift_var_load_count)(pfile_t *pf, value_t cntv);
  void        (*shift_var_prologue)(pfile_t *pf, value_t dst,
                    value_t src);
  void        (*shift_var_byte)(pfile_t *pf, operator_t op, value_t dst,
                    int i, value_t src, unsigned src_sz);
  /* relations / truth tests (cut 4): the kernel owns the relation
   * mapping -- which chain runs (equality accumulator vs signed
   * less-than), the operand order (GT/LE swap), the inversion policy
   * (branch-sense flip, never a flag XOR) -- and the branch-sense
   * logic.  The flag lands in the back-end's relation flag register;
   * cmd-stream branch-partner discovery stays per-target. */
  void        (*eq_acc)(pfile_t *pf, value_t v1, value_t v2);
  void        (*lt_signed)(pfile_t *pf, value_t v1, value_t v2);
  void        (*or_self)(pfile_t *pf, value_t v1);
  void        (*br_flag_to)(pfile_t *pf, boolean_t take_on_nonzero,
                    label_t lbl);
  void        (*bool_eq_finish)(pfile_t *pf, value_t dst, operator_t op);
  void        (*bool_lt_finish)(pfile_t *pf, value_t dst,
                    boolean_t inverted);
} walg_ops_t;

/* dst = v1 * v2 (unsigned schoolbook, truncated to dst width) */
void walg_mul(pfile_t *pf, const walg_ops_t *ops, value_t dst,
    value_t v1, value_t v2);

/* dst = v1 / v2 (op == OPERATOR_DIV) or v1 % v2 (OPERATOR_MOD):
 * restoring division, one bit per iteration, sign-magnitude with
 * a runtime zero-divisor skip */
void walg_div(pfile_t *pf, const walg_ops_t *ops, operator_t op,
    value_t dst, value_t v1, value_t v2);

/* dst = v1 <op> v2 for the per-byte chains: ADD / SUB propagate
 * carry / borrow, ANDB / ORB / XORB apply bitwise.  A NULL v2 (CMPB
 * one's-complement on targets that route it here) skips the op2 load.
 * Loop width is alu_width's answer (write-through aware where the
 * back-end has the concept). */
void walg_alu(pfile_t *pf, const walg_ops_t *ops, operator_t op,
    value_t dst, value_t v1, value_t v2);

/* dst = -v1, per byte, read-before-write (dst may alias v1) */
void walg_neg(pfile_t *pf, const walg_ops_t *ops, value_t dst, value_t v1);

/* dst = v1 +/- 1: increment carry / decrement borrow chain,
 * read-before-write (dst may alias v1; dst NULL means v1 in place) */
void walg_incr_decr(pfile_t *pf, const walg_ops_t *ops, operator_t op,
    value_t dst, value_t v1);

/* dst = src shifted by the constant COUNT (LEFT / RIGHT /
 * RIGHT_ARITHMETIC).  Saturation and the per-byte window policy live
 * here; a zero-width source saturates to zeros. */
void walg_shift_const(pfile_t *pf, const walg_ops_t *ops, operator_t op,
    value_t dst, value_t src, unsigned count);

/* dst = src shifted by a runtime count held in cntv.  Per dest byte
 * the bits [i*8 - count, i*8 - count + 8) (left) or [i*8 + count,
 * ...) (right) are reassembled; out-of-window bytes contribute zero,
 * or the sign byte for arithmetic right. */
void walg_shift_var(pfile_t *pf, const walg_ops_t *ops, operator_t op,
    value_t dst, value_t src, value_t cntv);

/* fused relation branch: emit the chain for v1 <op> v2 and a branch
 * to lbl taken when the relation holds (br_true = the partner goto
 * fires on condition TRUE).  The caller has already found and
 * consumed/skipped the branch cmd.  The inversion for GE/LE is a
 * branch-sense flip, not a flag complement. */
void walg_compare_branch_emit(pfile_t *pf, const walg_ops_t *ops,
    operator_t op, boolean_t br_true, label_t lbl, value_t v1,
    value_t v2);

/* fused value branch: flag = (v1 != 0); branch taken when the value
 * is zero (not_form, NOTL/CMPB) or nonzero (truth test) according to
 * br_true.  Caller has consumed/skipped the branch cmd. */
void walg_value_branch_emit(pfile_t *pf, const walg_ops_t *ops,
    boolean_t not_form, boolean_t br_true, label_t lbl, value_t v1);

/* dst = (v1 <op> v2) materialized as boolean 0/1 */
void walg_compare_bool(pfile_t *pf, const walg_ops_t *ops, value_t dst,
    operator_t op, value_t v1, value_t v2);

/* release the kernel-held wide temporaries (per-compile reset;
 * called from each back-end's cleanup) */
void walg_reset(void);

#endif /* wide_alg_h__ */
