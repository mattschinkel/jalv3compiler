/************************************************************
 **
 ** wide_alg.c : shared wide MUL / DIV / MOD algorithms
 **
 ** See wide_alg.h.  The algorithms below were lifted verbatim
 ** from the ESP32 implementation (esp_op.c) -- the ESP32 ops
 ** table reproduces its original emission exactly, so wiring
 ** that back end to this kernel is a pure code motion (suite
 ** bodies stay HEX byte-identical).
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include "pfile.h"
#include "pf_log.h"
#include "operator.h"
#include "value.h"
#include "wide_alg.h"

/* n-byte RAM temporaries, reused across wide ops of one compile
 * (esp_wide_tmp / mips_wide_tmp rule: linear placement, no home
 * block variable) */
static value_t walg_div_num;
static value_t walg_div_den;
static value_t walg_div_rem;
static value_t walg_div_quot;
static value_t walg_div_s1;
static value_t walg_div_s2;
static value_t walg_mul_a;
static value_t walg_mul_b;

static value_t walg_tmp_reuse(pfile_t *pf, const walg_ops_t *ops,
    value_t *slot, unsigned n)
{
  if (*slot && (ops->val_sz(*slot) >= n)) {
    return *slot;
  }
  if (*slot) {
    value_release(*slot);
    *slot = VALUE_NONE;
  }
  *slot = ops->tmp_new(pf, n);
  return *slot;
}

/* dst = v1 * v2: unsigned schoolbook byte multiply, truncated to
 * the destination width (low bytes are sign-agnostic).  A
 * dst-aliased operand is copied to a temp first: dst is zeroed
 * up front. */
void walg_mul(pfile_t *pf, const walg_ops_t *ops, value_t dst,
    value_t v1, value_t v2)
{
  unsigned n;
  unsigned i;
  unsigned j;
  value_t  t1 = v1;
  value_t  t2 = v2;

  n = ops->mul_width(pf, dst, v1);
  if (!n) {
    return;
  }
  if (ops->aliases(dst, v1)) {
    t1 = walg_tmp_reuse(pf, ops, &walg_mul_a, n);
    if (t1) {
      ops->assign(pf, t1, v1);
    }
  }
  if (ops->aliases(dst, v2)) {
    t2 = walg_tmp_reuse(pf, ops, &walg_mul_b, n);
    if (t2) {
      ops->assign(pf, t2, v2);
    }
  }
  if (!t1 || !t2) {
    return;
  }
  ops->mul_prologue(pf, t1, t2, dst, n);
  for (i = 0; i < n; i++) {
    ops->mul_row_init();
    for (j = 0; (i + j) < n; j++) {
      ops->mul_row_step(pf, t1, t2, dst, i, j, n);
    }
  }
}

/* dst = v1 / v2 or v1 % v2: restoring division, one bit per
 * iteration (n*8 loop trips).  Sign handling is sign-magnitude
 * with the sign bits held in 1-byte cells across the loop; a
 * zero runtime divisor skips the whole computation (dst stays
 * zero). */
void walg_div(pfile_t *pf, const walg_ops_t *ops, operator_t op,
    value_t dst, value_t v1, value_t v2)
{
  unsigned   n;
  unsigned   s1;
  unsigned   s2;
  boolean_t  sgn;
  value_t    num;
  value_t    den;
  value_t    rem;
  value_t    quot;
  value_t    sc1;
  value_t    sc2;
  walg_br_t  skip;
  walg_br_t  br;
  walg_br_t  back;
  walg_lab_t loop;

  s1 = ops->val_sz(v1);
  s2 = ops->val_sz(v2);
  n  = ops->div_base_width(pf, dst, v1);
  if (n < s1) {
    n = s1;
  }
  if (n < s2) {
    n = s2;
  }
  if (!n) {
    return;
  }
  if (v2 && value_is_const(v2) && (0 == value_const_get(v2))) {
    pfile_log(pf, PFILE_LOG_ERR, "division by zero");
    return;
  }
  sgn  = (value_is_signed(v1) || value_is_signed(v2)) ? BOOLEAN_TRUE
      : BOOLEAN_FALSE;
  num  = walg_tmp_reuse(pf, ops, &walg_div_num, n);
  den  = walg_tmp_reuse(pf, ops, &walg_div_den, n);
  rem  = walg_tmp_reuse(pf, ops, &walg_div_rem, n);
  quot = walg_tmp_reuse(pf, ops, &walg_div_quot, n);
  sc1  = walg_tmp_reuse(pf, ops, &walg_div_s1, 1);
  sc2  = walg_tmp_reuse(pf, ops, &walg_div_s2, 1);
  if (!num || !den || !rem || !quot || !sc1 || !sc2) {
    return;
  }
  ops->assign(pf, num, v1);
  ops->assign(pf, den, v2);
  if (sgn) {
    /* sign bytes of num/den into the sc1/sc2 cells */
    ops->sign_to_cell(pf, sc1, num, n);
    ops->sign_to_cell(pf, sc2, den, n);
    /* if sc1: num = -num */
    br = ops->cell_bez(pf, sc1);
    ops->neg_inplace(pf, num, n);
    ops->br_target(br, ops->label_here());
    /* if sc2: den = -den */
    br = ops->cell_bez(pf, sc2);
    ops->neg_inplace(pf, den, n);
    ops->br_target(br, ops->label_here());
  }
  ops->zero(pf, dst, n, num);
  ops->or_acc(pf, den, n);                     /* flag = den != 0 */
  skip = ops->br_flag_zero();
  ops->zero(pf, rem, n, VALUE_NONE);
  ops->zero(pf, quot, n, VALUE_NONE);
  ops->loop_prologue(pf, num, rem, quot);
  ops->movi(ops->reg_ctr, (int) (n * 8U));
  loop = ops->label_here();
  ops->addi(ops->reg_ctr, -1);
  ops->shift_left_1(pf, rem);
  ops->bit_load(pf, num);                      /* bit ctr -> bit reg */
  ops->bit_or_low(pf, rem);                    /* rem[0] |= bit      */
  ops->ult(pf, rem, den);                      /* flag = rem < den   */
  br = ops->br_flag_nonzero();
  ops->sub_wide(pf, rem, rem, den);
  ops->bit_set(pf, quot);                      /* set bit ctr in quot */
  ops->br_target(br, ops->label_here());
  back = ops->br_ctr_nonzero();
  ops->br_target(back, loop);
  if (sgn) {
    /* signs differ: quot = -quot */
    br = ops->cell_pair_xor_bez(pf, sc1, sc2);
    ops->neg_inplace(pf, quot, n);
    ops->br_target(br, ops->label_here());
    /* dividend negative: rem = -rem */
    br = ops->cell_bez(pf, sc1);
    ops->neg_inplace(pf, rem, n);
    ops->br_target(br, ops->label_here());
  }
  ops->assign(pf, dst, (OPERATOR_DIV == op) ? quot : rem);
  ops->br_target(skip, ops->label_here());
}

/* dst = v1 <op> v2 per byte: ADD/SUB walk carry/borrow, the bitwise
 * ops are pure.  The kernel owns the loop structure and the op2-load
 * policy (a const op2 is materialized per byte by the back-end's
 * load_op2; a NULL v2 skips it -- CMPB-style one's complement). */
void walg_alu(pfile_t *pf, const walg_ops_t *ops, operator_t op,
    value_t dst, value_t v1, value_t v2)
{
  unsigned  n;
  unsigned  s1;
  unsigned  s2;
  unsigned  k;
  boolean_t have_v2;

  have_v2 = (VALUE_NONE != v2) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  n  = ops->alu_width(pf, dst, v1);
  s1 = ops->val_sz(v1);
  s2 = have_v2 ? ops->val_sz(v2) : 0;
  if (!n) {
    return;
  }
  ops->alu_prologue(pf, op, dst, v1, v2);
  for (k = 0; k < n; k++) {
    ops->alu_load_op1(pf, v1, k, s1);
    if (have_v2) {
      ops->alu_load_op2(pf, v2, k, s2);
    }
    if (OPERATOR_ADD == op) {
      ops->alu_step_add();
    } else if (OPERATOR_SUB == op) {
      ops->alu_step_sub();
    } else {
      ops->alu_step_logic(op);
    }
    ops->alu_store(pf, dst, k, v1);
  }
}

/* dst = -v1.  The per-byte negation idiom is the back-end's
 * (NOT + add carry on Xtensa, 0 - x on MIPS); the kernel enforces the
 * read-before-write order so dst may alias v1. */
void walg_neg(pfile_t *pf, const walg_ops_t *ops, value_t dst, value_t v1)
{
  unsigned n;
  unsigned s1;
  unsigned k;

  n  = ops->alu_width(pf, dst, v1);
  s1 = ops->val_sz(v1);
  if (!n) {
    return;
  }
  ops->neg_prologue(pf, dst, v1);
  for (k = 0; k < n; k++) {
    ops->neg_step(pf, v1, k, s1);
    ops->alu_store(pf, dst, k, v1);
  }
}

/* dst = v1 +/- 1 (dst NULL means v1 in place).  Increment carries,
 * decrement borrows; the kernel enforces read-before-write so dst
 * may alias v1. */
void walg_incr_decr(pfile_t *pf, const walg_ops_t *ops, operator_t op,
    value_t dst, value_t v1)
{
  unsigned n;
  unsigned s1;
  unsigned k;

  n  = ops->alu_width(pf, dst, v1);
  s1 = ops->val_sz(v1);
  if (!n) {
    return;
  }
  ops->incr_prologue(pf, dst, v1);
  for (k = 0; k < n; k++) {
    ops->incr_decr_step(pf, op, v1, k, s1);
    ops->alu_store(pf, dst, k, v1);
  }
}

/* dst = src << / >> count (count a compile-time constant).  Policy
 * here: saturation (count >= width: sign fill for arithmetic right --
 * const-folded when the source is known -- zeros otherwise; a
 * zero-width source is zeros), the lo/hi source-byte window per dest
 * byte, and the byte walk order -- descending for left shifts, since
 * byte i reads source bytes at or below i and an ascending store
 * order would feed later bytes the already-shifted neighbors when
 * dst == src (the div kernel's rem <<= 1 relies on this). */
void walg_shift_const(pfile_t *pf, const walg_ops_t *ops, operator_t op,
    value_t dst, value_t src, unsigned count)
{
  unsigned  n;
  unsigned  src_sz;
  unsigned  bit;
  unsigned  bytes;
  unsigned  fill;
  boolean_t left;
  boolean_t arith;
  unsigned  k;

  left  = (OPERATOR_SHIFT_LEFT == op) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  arith = (OPERATOR_SHIFT_RIGHT_ARITHMETIC == op) ? BOOLEAN_TRUE
      : BOOLEAN_FALSE;
  n      = ops->shift_width(pf, dst, src);
  src_sz = ops->val_sz(src);
  if (!n || !src_sz) {
    ops->shift_zero_all(pf, dst, n, src);
    return;
  }
  if (count >= src_sz * 8U) {
    if (arith) {
      if (ops->shift_is_const(src)) {
        fill = ops->shift_fill_const(src, src_sz, BOOLEAN_TRUE);
        ops->shift_fill_all_const(pf, dst, n, src, fill);
      } else {
        ops->shift_fill_all_runtime(pf, dst, n, src, src_sz);
      }
    } else {
      ops->shift_zero_all(pf, dst, n, src);
    }
    return;
  }
  bit   = count & 7U;
  bytes = count / 8U;
  fill  = ops->shift_fill_const(src, src_sz, arith);
  ops->shift_const_prologue(pf, dst, src);
  for (k = 0; k < n; k++) {
    int i;
    int lo_i;
    int hi_i;

    i = left ? (int) (n - 1U - k) : (int) k;
    if (left) {
      lo_i = i - (int) bytes;
      hi_i = lo_i - 1;
    } else {
      lo_i = i + (int) bytes;
      hi_i = lo_i + 1;
    }
    ops->shift_const_byte(pf, dst, src, i, lo_i, hi_i, bit, fill,
        arith, left, src_sz);
  }
}

/* dst = src shifted by a runtime count.  The count is staged once;
 * per dest byte the kernel walks i descending for left shifts (same
 * in-place alias rule as the const shift). */
void walg_shift_var(pfile_t *pf, const walg_ops_t *ops, operator_t op,
    value_t dst, value_t src, value_t cntv)
{
  unsigned  n;
  unsigned  src_sz;
  boolean_t left;
  unsigned  k;

  left   = (OPERATOR_SHIFT_LEFT == op) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  n      = ops->shift_width(pf, dst, src);
  src_sz = ops->val_sz(src);
  if (!n || !src_sz) {
    return;
  }
  ops->shift_var_load_count(pf, cntv);
  ops->shift_var_prologue(pf, dst, src);
  for (k = 0; k < n; k++) {
    ops->shift_var_byte(pf, op, dst,
        left ? (int) (n - 1U - k) : (int) k, src, src_sz);
  }
}

/* the relation mapping, owned once: EQ/NE run the equality
 * accumulator (flag nonzero iff the values differ -- so NE is the
 * nonzero-true form); the ordered relations run the signed less-than
 * chain, GT/LE with swapped operands.  A relation is "nonzero-true"
 * when a nonzero flag means it holds; GE/LE are the negations of
 * LT/GT, expressed by flipping the branch sense rather than
 * complementing the flag. */
static void walg_rel_chain(pfile_t *pf, const walg_ops_t *ops,
    operator_t op, value_t v1, value_t v2, boolean_t *eq_raw,
    boolean_t *rel_nonzero)
{
  *eq_raw = ((OPERATOR_EQ == op) || (OPERATOR_NE == op))
      ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  if (*eq_raw) {
    ops->eq_acc(pf, v1, v2);
    *rel_nonzero = (OPERATOR_NE == op) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
    return;
  }
  if ((OPERATOR_GT == op) || (OPERATOR_LE == op)) {
    ops->lt_signed(pf, v2, v1);
  } else {
    ops->lt_signed(pf, v1, v2);
  }
  *rel_nonzero = ((OPERATOR_LT == op) || (OPERATOR_GT == op))
      ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

void walg_compare_branch_emit(pfile_t *pf, const walg_ops_t *ops,
    operator_t op, boolean_t br_true, label_t lbl, value_t v1,
    value_t v2)
{
  boolean_t eq_raw;
  boolean_t rel_nonzero;

  walg_rel_chain(pf, ops, op, v1, v2, &eq_raw, &rel_nonzero);
  ops->br_flag_to(pf,
      (rel_nonzero == br_true) ? BOOLEAN_TRUE : BOOLEAN_FALSE, lbl);
}

void walg_value_branch_emit(pfile_t *pf, const walg_ops_t *ops,
    boolean_t not_form, boolean_t br_true, label_t lbl, value_t v1)
{
  boolean_t take_on_nonzero;

  ops->or_self(pf, v1);
  take_on_nonzero = not_form
      ? ((BOOLEAN_FALSE == br_true) ? BOOLEAN_TRUE : BOOLEAN_FALSE)
      : br_true;
  ops->br_flag_to(pf, take_on_nonzero, lbl);
}

void walg_compare_bool(pfile_t *pf, const walg_ops_t *ops, value_t dst,
    operator_t op, value_t v1, value_t v2)
{
  boolean_t eq_raw;
  boolean_t rel_nonzero;

  walg_rel_chain(pf, ops, op, v1, v2, &eq_raw, &rel_nonzero);
  if (eq_raw) {
    ops->bool_eq_finish(pf, dst, op);
  } else {
    ops->bool_lt_finish(pf, dst, (BOOLEAN_TRUE == rel_nonzero)
        ? BOOLEAN_FALSE : BOOLEAN_TRUE);
  }
}

void walg_reset(void)
{
  if (walg_div_num) {
    value_release(walg_div_num);
  }
  if (walg_div_den) {
    value_release(walg_div_den);
  }
  if (walg_div_rem) {
    value_release(walg_div_rem);
  }
  if (walg_div_quot) {
    value_release(walg_div_quot);
  }
  if (walg_div_s1) {
    value_release(walg_div_s1);
  }
  if (walg_div_s2) {
    value_release(walg_div_s2);
  }
  if (walg_mul_a) {
    value_release(walg_mul_a);
  }
  if (walg_mul_b) {
    value_release(walg_mul_b);
  }
  walg_div_num  = VALUE_NONE;
  walg_div_den  = VALUE_NONE;
  walg_div_rem  = VALUE_NONE;
  walg_div_quot = VALUE_NONE;
  walg_div_s1   = VALUE_NONE;
  walg_div_s2   = VALUE_NONE;
  walg_mul_a    = VALUE_NONE;
  walg_mul_b    = VALUE_NONE;
}
