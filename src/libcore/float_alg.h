/************************************************************
 **
 ** float_alg.h : shared soft-float kernel (fourth libcore kernel
 ** after wide_alg, indirect_alg, task_alg)
 **
 ** The ALGORITHM lives once here -- ESP32's shape is canonical
 ** (shared unpack/pack sub-helpers, NSAU normalize, fused
 ** compare-branches) -- while each back end supplies machine
 ** primitives through falg_ops_t.  Cut 1 (this file) shares the
 ** unpack/pack sub-helpers: test_special, extract_normalize,
 ** pack_core, return_qnan.
 **
 ** Register contract (abstract indices; each target maps them to
 ** physical word registers):
 **   FA_OP1   result / operand1      FA_T0..FA_T3 scratch
 **   FA_OP2   operand2               FA_SIGN  result sign (0/1)
 **   FA_EXP   biased exponent        FA_STICKY rounding sticky
 **   FA_MANT  significand S<<2|G<<1|R
 ** FA_RET is the helper return-address register (never touched).
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#ifndef float_alg_h__
#define float_alg_h__

#include "pfile.h"

/* abstract registers (ESP32: a2..a11 identity) */
#define FA_OP1    2
#define FA_OP2    3
#define FA_EXP    4
#define FA_MANT   5
#define FA_T0     6
#define FA_T1     7
#define FA_T2     8
#define FA_T3     9
#define FA_SIGN  10
#define FA_STICKY 11

typedef enum falg_alu_ {
  FALG_ALU_ADD,
  FALG_ALU_SUB,
  FALG_ALU_AND,
  FALG_ALU_OR,
  FALG_ALU_XOR
} falg_alu_t;

/* branch conditions: taken when LEFT <cond> RIGHT (the `at` parameter
 * is the RIGHT operand; LEFT is `as`) -- encodes the ESP BR() calling
 * convention verbatim so the kernel body is byte-faithful */
typedef enum falg_br_ {
  FALG_BR_EQZ,    /* left == 0 (right ignored) */
  FALG_BR_NEZ,    /* left != 0 */
  FALG_BR_GEU,    /* left >=u right */
  FALG_BR_GE,     /* left >= right, signed */
  FALG_BR_LTU,    /* left <u right */
  FALG_BR_LT,     /* left < right, signed */
  FALG_BR_NE      /* left != right (registers) */
} falg_br_t;

/* branch/label handles are opaque (esp_code_t / mips fixup node) */
typedef void *falg_brh_t;

typedef struct falg_ops_ {
  void     (*movi)(pfile_t *pf, int rd, long imm);
  void     (*load_const)(pfile_t *pf, int rd, unsigned long val);
  void     (*alu)(pfile_t *pf, falg_alu_t op, int rd, int rs, int rt);
  void     (*addi)(pfile_t *pf, int rd, int rs, long imm);
  /* variable-count shifts: count comes from the `sar` register */
  void     (*sll_var)(pfile_t *pf, int rd, int rs, int sar);
  void     (*srl_var)(pfile_t *pf, int rd, int rs, int sar);
  /* bitfield extract: rd = rs[sh+w-1 .. sh] (0<=sh, 1<=w<=16) */
  void     (*ext)(pfile_t *pf, int rd, int rs, int sh, int w);
  /* count leading zeros of rs (kernel domain: rs < 2^27, so NSAU on
   * Xtensa and CLZ on MIPS32r2 agree) */
  void     (*clz)(pfile_t *pf, int rd, int rs);
  falg_brh_t (*br)(pfile_t *pf, falg_br_t cond, int at, int as);
  void     (*bind1)(pfile_t *pf, falg_brh_t b1);
  void     (*bind2)(pfile_t *pf, falg_brh_t b1, falg_brh_t b2);
  void     (*bind4)(pfile_t *pf, falg_brh_t b1, falg_brh_t b2,
              falg_brh_t b3, falg_brh_t b4);
  /* 32x32 multiply: low word / unsigned high word (cut 2: MUL) */
  void     (*mul_lo)(pfile_t *pf, int rd, int rs, int rt);
  void     (*mul_hi)(pfile_t *pf, int rd, int rs, int rt);
  /* two's-complement negation (cut 3: ADD/SUB) */
  void     (*neg)(pfile_t *pf, int rd);
  /* cut 4 (DIV): shift-count preload -- ssr_set loads the machine
   * shift-count register from `sar`; srl_sar shifts by whatever it
   * holds (the kernel guarantees ssr_set is the only count write
   * between the pair).  Targets without a count register implement
   * ssr_set as a no-op and srl_sar as a variable shift by `sar`
   * (recording which register ssr_set last named). */
  void     (*ssr_set)(pfile_t *pf, int sar);
  void     (*srl_sar)(pfile_t *pf, int rd, int rs, int sar);
  /* backward branch: label placed at the CURRENT position (use the
   * handle only as a backward target) */
  falg_brh_t (*label_here)(pfile_t *pf);
  void     (*br_back)(pfile_t *pf, falg_br_t cond, int at, int as,
              falg_brh_t target);
  /* unconditional forward jump (handle for later binding) */
  falg_brh_t (*ujmp)(pfile_t *pf, int scratch);
  /* helper return */
  void     (*ret)(pfile_t *pf);
} falg_ops_t;

/* ---- cut 1: the shared unpack/pack sub-helpers ----
 * All bodies assume and clobber per the ESP register contract (see
 * esp_float.c block comments); callers divert specials first. */

/* branch out to *b_zero when reg is +-0, to *b_spec when inf/NaN;
 * falls through for normals and denormals.  Clobbers FA_T2/FA_T3. */
void falg_test_special(pfile_t *pf, const falg_ops_t *ops, int reg,
          falg_brh_t *b_zero, falg_brh_t *b_spec);

/* unpack src into biased exp (rexp) + 24-bit mantissa with implicit
 * bit (rmant); denormals normalized (exp = 1 - k).  Zero/inf/NaN must
 * have been diverted already.  Clobbers FA_T2/FA_T3. */
void falg_extract_normalize(pfile_t *pf, const falg_ops_t *ops,
          int src, int rexp, int rmant);

/* FA_OP1 = canonical NaN (invalid operation), then return */
void falg_return_qnan(pfile_t *pf, const falg_ops_t *ops);

/* pack + round-to-nearest-even + range clamp:
 * in: FA_EXP = biased exp (signed), FA_MANT = S<<2|G<<1|R (or 0),
 *     FA_SIGN = sign (0/1), FA_STICKY = sticky.  out: FA_OP1 = bits.
 * Clobbers FA_MANT, FA_T2, FA_T3, FA_STICKY. */
void falg_pack_core(pfile_t *pf, const falg_ops_t *ops);

/* ---- cut 2: MUL ----
 * FA_OP1 x FA_OP2 -> FA_OP1, IEEE-754 (RNE, specials, denormals),
 * through the shared helpers.  Helper contract: operands in
 * FA_OP1/FA_OP2, result in FA_OP1, returns itself. */
void falg_mul_core(pfile_t *pf, const falg_ops_t *ops);

/* ---- cut 3: ADD/SUB ----
 * FA_OP1 +/- FA_OP2 -> FA_OP1, IEEE-754 (effective-sign handling,
 * magnitude add/sub with alignment, exact-cancel signed zeros,
 * inf/NaN tables). */
void falg_addsub_core(pfile_t *pf, const falg_ops_t *ops,
          boolean_t is_sub);

/* ---- cut 4: DIV ----
 * FA_OP1 / FA_OP2 -> FA_OP1, IEEE-754: 49 restoring iterations
 * produce Q = floor(N * 2^25 / D) = S<<2|G<<1|R, remainder gives the
 * sticky (round-to-nearest-even division). */
void falg_div_core(pfile_t *pf, const falg_ops_t *ops);

/* ---- cut 5: conversions ----
 * i2f: FA_OP1 (integer) -> FA_OP1 (float); truncation for
 * |int| >= 2^24 is the documented limitation.
 * f2i: FA_OP1 (float) -> FA_OP1 (integer; truncating, saturating at
 * INT_MIN/INT_MAX; NaN -> 0). */
void falg_i2f_core(pfile_t *pf, const falg_ops_t *ops);
void falg_f2i_core(pfile_t *pf, const falg_ops_t *ops);

#endif /* float_alg_h__ */
