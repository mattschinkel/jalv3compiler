/************************************************************
 **
 ** float_alg.c : shared soft-float kernel -- cut 1 (unpack/pack
 ** sub-helpers), transcribed verbatim from esp_float.c (the
 ** canonical algorithm per docs/investigations/float_alg_plan.md).
 ** Every helper body below emits EXACTLY the instruction stream the
 ** ESP32 macros emitted, through the ops table -- the ESP32 wiring
 ** is byte-identity-checked against the pre-extraction images.
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include "float_alg.h"
#include "../libutils/mem.h"

/* float bit constants (target load_const materializes them) */
#define FALG_SIGN   0x80000000UL   /* sign bit */
#define FALG_INF    0x7F800000UL   /* +inf bits */
#define FALG_QNAN   0x7FC00000UL   /* canonical quiet NaN */
#define FALG_QPROP  0x00400000UL   /* quiet bit forced on propagated NaNs */
#define FALG_MANT   0x007FFFFFUL   /* mantissa field */
#define FALG_IMPL   0x00800000UL   /* implicit leading significand bit */
#define FALG_EXPMAX 0xFF000000UL   /* (x<<1) > this  =>  inf/NaN */

/* kernel-local shorthands (ops-forwarding only) */
#define MI(rd, imm)        ops->movi(pf, (rd), (imm))
#define PL(rd, val)        ops->load_const(pf, (rd), (val))
#define ALU(op, rd, rs, rt) ops->alu(pf, (op), (rd), (rs), (rt))
#define ADDI(rd, rs, imm)  ops->addi(pf, (rd), (rs), (imm))
#define SSL(rd, rs, sar)   ops->sll_var(pf, (rd), (rs), (sar))
#define SSR(rd, rs, sar)   ops->srl_var(pf, (rd), (rs), (sar))
#define EXT(rd, rs, sh, w) ops->ext(pf, (rd), (rs), (sh), (w))
#define BR(cond, at, as)   ops->br(pf, (cond), (at), (as))
#define BIND(b)            ops->bind1(pf, (b))
#define BIND2(b1, b2)      ops->bind2(pf, (b1), (b2))
#define UJMP(scratch)      ops->ujmp(pf, (scratch))
#define RTN()              ops->ret(pf)

void falg_test_special(pfile_t *pf, const falg_ops_t *ops, int reg,
    falg_brh_t *b_zero, falg_brh_t *b_spec)
{
  falg_brh_t bz;
  falg_brh_t bs;

  MI(FA_T3, 1);
  SSL(FA_T2, reg, FA_T3);               /* t2 = x << 1 */
  bz = BR(FALG_BR_EQZ, 0, FA_T2);       /* == 0 */
  PL(FA_T3, FALG_EXPMAX);
  bs = BR(FALG_BR_GEU, FA_T3, FA_T2);   /* FF.. >= t2? no: t2 >= FF.. */
  if (b_zero) {
    *b_zero = bz;
  }
  if (b_spec) {
    *b_spec = bs;
  }
}

void falg_extract_normalize(pfile_t *pf, const falg_ops_t *ops,
    int src, int rexp, int rmant)
{
  falg_brh_t br;
  falg_brh_t uj;

  MI(FA_T3, 23);
  SSR(rexp, src, FA_T3);
  EXT(rexp, rexp, 0, 8);
  PL(FA_T2, FALG_MANT);
  ALU(FALG_ALU_AND, rmant, src, FA_T2);
  br = BR(FALG_BR_EQZ, 0, rexp);        /* exp == 0: denormal */
  /* normal: restore the implicit leading significand bit */
  PL(FA_T2, FALG_IMPL);
  ALU(FALG_ALU_OR, rmant, rmant, FA_T2);
  uj = UJMP(FA_T3);
  BIND(br);
  ops->clz(pf, FA_T3, rmant);
  MI(FA_T2, 8);
  ALU(FALG_ALU_SUB, FA_T3, FA_T3, FA_T2);   /* k = clz - 8 */
  SSL(rmant, rmant, FA_T3);
  MI(FA_T2, 1);
  ALU(FALG_ALU_SUB, rexp, FA_T2, FA_T3);    /* exp = 1 - k */
  BIND(uj);
}

void falg_return_qnan(pfile_t *pf, const falg_ops_t *ops)
{
  PL(FA_OP1, FALG_QNAN);
  RTN();
}

void falg_pack_core(pfile_t *pf, const falg_ops_t *ops)
{
  falg_brh_t br;
  falg_brh_t br2;
  falg_brh_t bc;

  /* zero significand (defensive): signed zero */
  br = BR(FALG_BR_NEZ, 0, FA_MANT);
  MI(FA_T3, 31);
  SSL(FA_T3, FA_SIGN, FA_T3);
  ALU(FALG_ALU_OR, FA_OP1, FA_T3, FA_T3);
  RTN();
  BIND(br);
  /* renorm right when bit 26 set (add/mul carry); the dropped bit
   * (S bit 0) folds into sticky */
  MI(FA_T3, 26);
  SSR(FA_T2, FA_MANT, FA_T3);
  br = BR(FALG_BR_EQZ, 0, FA_T2);
  EXT(FA_T3, FA_MANT, 2, 1);
  br2 = BR(FALG_BR_EQZ, 0, FA_T3);
  MI(FA_STICKY, 1);
  BIND(br2);
  MI(FA_T2, 3);
  ALU(FALG_ALU_AND, FA_T3, FA_MANT, FA_T2);  /* g = G|R */
  MI(FA_T2, 3);
  SSR(FA_MANT, FA_MANT, FA_T2);
  MI(FA_T2, 2);
  SSL(FA_MANT, FA_MANT, FA_T2);
  ALU(FALG_ALU_OR, FA_MANT, FA_MANT, FA_T3);
  MI(FA_T3, 1);
  ALU(FALG_ALU_ADD, FA_EXP, FA_EXP, FA_T3);
  BIND(br);
  /* renorm left when mant < 2^25 (subtraction results): one exact
   * single step first (new G = old R, new R = 0), then a bulk
   * shift -- once G and R are 0, shifting cannot change the
   * rounding decision */
  MI(FA_T2, 25);
  SSR(FA_T2, FA_MANT, FA_T2);
  br = BR(FALG_BR_NEZ, 0, FA_T2);
  MI(FA_T2, 1);
  SSL(FA_MANT, FA_MANT, FA_T2);
  MI(FA_T3, 1);
  ALU(FALG_ALU_SUB, FA_EXP, FA_EXP, FA_T3);
  MI(FA_T2, 25);
  SSR(FA_T2, FA_MANT, FA_T2);
  br2 = BR(FALG_BR_NEZ, 0, FA_T2);
  ops->clz(pf, FA_T3, FA_MANT);
  MI(FA_T2, 6);
  ALU(FALG_ALU_SUB, FA_T3, FA_T3, FA_T2);  /* k = clz - 6 */
  SSL(FA_MANT, FA_MANT, FA_T3);
  ALU(FALG_ALU_SUB, FA_EXP, FA_EXP, FA_T3);
  BIND2(br, br2);
  /* round: G = bit 1; round up iff G && (R | sticky | LSB) */
  EXT(FA_T2, FA_MANT, 1, 1);
  br = BR(FALG_BR_EQZ, 0, FA_T2);
  MI(FA_T2, 5);
  ALU(FALG_ALU_AND, FA_T3, FA_MANT, FA_T2);  /* R | LSB */
  ALU(FALG_ALU_OR, FA_T3, FA_T3, FA_STICKY);
  br2 = BR(FALG_BR_EQZ, 0, FA_T3);
  ADDI(FA_MANT, FA_MANT, 4);
  /* round carry into bit 26: S' == 2^24 exactly; renorm right,
   * keeping G,R (the dropped bit folds into sticky -- the carry
   * proves it is set) */
  MI(FA_T3, 26);
  SSR(FA_T2, FA_MANT, FA_T3);
  bc = BR(FALG_BR_EQZ, 0, FA_T2);
  MI(FA_T3, 29);
  SSL(FA_T2, FA_MANT, FA_T3);           /* bit 2 -> 31 */
  ALU(FALG_ALU_OR, FA_STICKY, FA_STICKY, FA_T2);
  MI(FA_T2, 3);
  ALU(FALG_ALU_AND, FA_T3, FA_MANT, FA_T2);
  MI(FA_T2, 3);
  SSR(FA_MANT, FA_MANT, FA_T2);
  MI(FA_T2, 2);
  SSL(FA_MANT, FA_MANT, FA_T2);
  ALU(FALG_ALU_OR, FA_MANT, FA_MANT, FA_T3);
  MI(FA_T3, 1);
  ALU(FALG_ALU_ADD, FA_EXP, FA_EXP, FA_T3);
  BIND(bc);
  BIND2(br, br2);
  /* overflow: biased exp >= 255 -> inf */
  MI(FA_T2, 255);
  br = BR(FALG_BR_GE, FA_EXP, FA_T2);   /* 255 >= exp: skip */
  PL(FA_T3, FALG_INF);
  MI(FA_T2, 31);
  SSL(FA_T2, FA_SIGN, FA_T2);
  ALU(FALG_ALU_OR, FA_OP1, FA_T3, FA_T2);
  RTN();
  BIND(br);
  /* normal when exp >= 1 (E >= -126) */
  MI(FA_T2, 0);
  br = BR(FALG_BR_GE, FA_EXP, FA_T2);   /* 0 >= exp: denormal */
  MI(FA_T2, 2);
  SSR(FA_T2, FA_MANT, FA_T2);           /* S at bits [25:2] */
  PL(FA_T3, FALG_MANT);
  ALU(FALG_ALU_AND, FA_OP1, FA_T2, FA_T3);
  MI(FA_T3, 23);
  SSL(FA_T2, FA_EXP, FA_T3);
  ALU(FALG_ALU_OR, FA_OP1, FA_OP1, FA_T2);
  MI(FA_T3, 31);
  SSL(FA_T2, FA_SIGN, FA_T3);
  ALU(FALG_ALU_OR, FA_OP1, FA_OP1, FA_T2);
  RTN();
  BIND(br);
  /* denormal: exp in [-23, 0]; below -> signed zero (v < half the
   * minimum denormal, tie impossible) */
  MI(FA_T3, 0);
  ADDI(FA_T3, FA_T3, -23);
  br = BR(FALG_BR_GE, FA_T3, FA_EXP);   /* exp >= -23 */
  MI(FA_OP1, 0);
  RTN();
  BIND(br);
  /* d = 1 - exp in [1, 24]; fold G,R into sticky */
  MI(FA_T2, 1);
  ALU(FALG_ALU_SUB, FA_T2, FA_T2, FA_EXP);  /* t2 = d */
  MI(FA_T3, 3);
  ALU(FALG_ALU_AND, FA_T3, FA_MANT, FA_T3);
  ALU(FALG_ALU_OR, FA_STICKY, FA_STICKY, FA_T3);
  /* sticky from M bits below G' (M bits d..0): mant << (31 - d) */
  MI(FA_T3, 31);
  ALU(FALG_ALU_SUB, FA_T3, FA_T3, FA_T2);
  SSL(FA_T3, FA_MANT, FA_T3);
  ALU(FALG_ALU_OR, FA_STICKY, FA_STICKY, FA_T3);
  /* mant >>= d + 1: G' lands in bit 0, field LSB in bit 1 */
  ADDI(FA_T3, FA_T2, 1);
  SSR(FA_MANT, FA_MANT, FA_T3);
  /* round at bit 0: up iff G' (bit 0) && (field LSB (bit 1) | sticky) */
  MI(FA_T3, 3);
  ALU(FALG_ALU_AND, FA_T3, FA_MANT, FA_T3);
  EXT(FA_T2, FA_T3, 0, 1);              /* G' */
  br = BR(FALG_BR_EQZ, 0, FA_T2);
  MI(FA_T2, 1);
  SSR(FA_T3, FA_T3, FA_T2);             /* LSB -> bit 0 */
  ALU(FALG_ALU_OR, FA_T3, FA_T3, FA_STICKY);
  br2 = BR(FALG_BR_EQZ, 0, FA_T3);
  ADDI(FA_MANT, FA_MANT, 2);
  BIND2(br, br2);
  MI(FA_T2, 1);
  SSR(FA_MANT, FA_MANT, FA_T2);
  /* round-carry to 2^-126 (bit 23) becomes the smallest normal */
  MI(FA_T3, 23);
  SSR(FA_T3, FA_MANT, FA_T3);
  MI(FA_T2, 1);
  ALU(FALG_ALU_AND, FA_T3, FA_T3, FA_T2);
  br = BR(FALG_BR_EQZ, 0, FA_T3);
  MI(FA_T2, 1);
  MI(FA_T3, 23);
  SSL(FA_T2, FA_T2, FA_T3);
  MI(FA_T3, 31);
  SSL(FA_T3, FA_SIGN, FA_T3);
  ALU(FALG_ALU_OR, FA_OP1, FA_T2, FA_T3);
  RTN();
  BIND(br);
  MI(FA_T3, 31);
  SSL(FA_T3, FA_SIGN, FA_T3);
  ALU(FALG_ALU_OR, FA_OP1, FA_MANT, FA_T3);
}

/* ---- cut 2: MUL (verbatim from esp_float.c emit_mul_core) ---- */
void falg_mul_core(pfile_t *pf, const falg_ops_t *ops)
{
  falg_brh_t bs1, bs2, bz1, bz2, bl, ujoin, bn, bn2, bn3;

  /* classify both (NaN must win over 0/inf via the handlers) */
  falg_test_special(pf, ops, FA_OP1, &bz1, &bs1);
  falg_test_special(pf, ops, FA_OP2, &bz2, &bs2);
  /* === normal path === */
  falg_extract_normalize(pf, ops, FA_OP1, FA_EXP, FA_MANT);
  falg_extract_normalize(pf, ops, FA_OP2, FA_T0, FA_T1);
  /* SIGN_OF(FA_OP1): sign = bit 31 */
  MI(FA_T3, 31);
  SSR(FA_SIGN, FA_OP1, FA_T3);
  MI(FA_T3, 31);
  SSR(FA_T3, FA_OP2, FA_T3);
  ALU(FALG_ALU_XOR, FA_SIGN, FA_SIGN, FA_T3);
  ALU(FALG_ALU_ADD, FA_EXP, FA_EXP, FA_T0);
  MI(FA_T3, 127);
  ALU(FALG_ALU_SUB, FA_EXP, FA_EXP, FA_T3);
  MI(FA_STICKY, 0);
  ops->mul_lo(pf, FA_T3, FA_MANT, FA_T1);
  ops->mul_hi(pf, FA_MANT, FA_MANT, FA_T1);
  EXT(FA_T2, FA_MANT, 15, 1);           /* P bit 47 */
  bl = BR(FALG_BR_EQZ, 0, FA_T2);
  /* implicit bit at 47: M = P >> 22, exp = e1+e2-126 */
  MI(FA_T2, 10);
  SSL(FA_MANT, FA_MANT, FA_T2);
  MI(FA_T2, 22);
  SSR(FA_T2, FA_T3, FA_T2);
  ALU(FALG_ALU_ADD, FA_MANT, FA_MANT, FA_T2);
  ALU(FALG_ALU_OR, FA_STICKY, FA_STICKY, FA_T3);  /* lo != 0 */
  MI(FA_T3, 1);
  ALU(FALG_ALU_ADD, FA_EXP, FA_EXP, FA_T3);
  ujoin = UJMP(FA_T3);
  /* implicit bit at 46: M = P >> 21, exp = e1+e2-127 */
  BIND(bl);
  MI(FA_T2, 11);
  SSL(FA_MANT, FA_MANT, FA_T2);
  MI(FA_T2, 21);
  SSR(FA_T2, FA_T3, FA_T2);
  ALU(FALG_ALU_ADD, FA_MANT, FA_MANT, FA_T2);
  ALU(FALG_ALU_OR, FA_STICKY, FA_STICKY, FA_T3);  /* lo != 0 */
  BIND(ujoin);
  falg_pack_core(pf, ops);
  RTN();
  /* === handlers === */
  /* op1 inf/NaN (NaN checked first: 0 * NaN and inf * NaN = NaN) */
  BIND(bs1);
  MI(FA_T3, 9);
  SSL(FA_T3, FA_OP1, FA_T3);
  bn = BR(FALG_BR_EQZ, 0, FA_T3);
  PL(FA_T3, FALG_QPROP);
  ALU(FALG_ALU_OR, FA_OP1, FA_OP1, FA_T3);
  RTN();
  BIND(bn);
  /* op1 inf: op2 +-0 or NaN -> NaN; op2 inf/finite -> inf s1^s2 */
  MI(FA_T3, 1);
  SSL(FA_T3, FA_OP2, FA_T3);             /* t3 = op2 << 1 */
  bn2 = BR(FALG_BR_EQZ, 0, FA_T3);       /* +-0: invalid */
  PL(FA_T2, FALG_EXPMAX);
  bn3 = BR(FALG_BR_GEU, FA_T2, FA_T3);   /* op2<<1 >= FF..: inf/NaN */
  /* op2 inf/finite: inf with s1^s2 */
  PL(FA_T3, FALG_SIGN);
  ALU(FALG_ALU_AND, FA_T3, FA_OP2, FA_T3);
  ALU(FALG_ALU_XOR, FA_OP1, FA_OP1, FA_T3);
  RTN();
  BIND(bn2);
  BIND(bn3);
  falg_return_qnan(pf, ops);
  /* op2 inf/NaN (op1 finite non-special) */
  BIND(bs2);
  MI(FA_T3, 9);
  SSL(FA_T3, FA_OP2, FA_T3);
  bn = BR(FALG_BR_EQZ, 0, FA_T3);
  PL(FA_T3, FALG_QPROP);
  ALU(FALG_ALU_OR, FA_OP1, FA_OP2, FA_T3);
  RTN();
  BIND(bn);
  /* finite * inf: inf with s1^s2 */
  PL(FA_T3, FALG_SIGN);
  ALU(FALG_ALU_AND, FA_T3, FA_OP1, FA_T3);
  ALU(FALG_ALU_XOR, FA_OP1, FA_OP2, FA_T3);
  RTN();
  /* op1 +-0: op2 is untested here -- NaN dominates, 0 * inf is an
   * invalid operation, 0 * finite is +-0 with s1^s2 */
  BIND(bz1);
  {
    falg_brh_t bz3, bs3, cn;

    falg_test_special(pf, ops, FA_OP2, &bz3, &bs3);
    /* fall (op2 finite normal) and bz3 (op2 +-0): the formula */
    BIND(bz3);
    PL(FA_T3, FALG_SIGN);
    ALU(FALG_ALU_XOR, FA_T2, FA_OP1, FA_OP2);
    ALU(FALG_ALU_AND, FA_T2, FA_T2, FA_T3);
    ALU(FALG_ALU_OR, FA_OP1, FA_T2, FA_T2);
    RTN();
    BIND(bs3);
    MI(FA_T3, 9);
    SSL(FA_T3, FA_OP2, FA_T3);
    cn = BR(FALG_BR_EQZ, 0, FA_T3);
    PL(FA_T3, FALG_QPROP);
    ALU(FALG_ALU_OR, FA_OP1, FA_OP2, FA_T3);
    RTN();
    BIND(cn);
    falg_return_qnan(pf, ops);           /* 0 * inf */
  }
  /* op2 +-0 (op1 finite non-special): +-0 with s1^s2 */
  BIND(bz2);
  PL(FA_T3, FALG_SIGN);
  ALU(FALG_ALU_XOR, FA_T2, FA_OP1, FA_OP2);
  ALU(FALG_ALU_AND, FA_T2, FA_T2, FA_T3);
  ALU(FALG_ALU_OR, FA_OP1, FA_T2, FA_T2);
  RTN();
}

/* ---- cut 3: ADD/SUB (verbatim from esp_float.c emit_align_tail +
 * emit_addsub_core; SWAP_MANTS/SIGN_OF inlined through the ops) ---- */
static void falg_align_tail(pfile_t *pf, const falg_ops_t *ops,
    boolean_t is_add)
{
  falg_brh_t br, br2, uj;

  /* clamp d to 32 (shift counts are mod-32; B beyond 32 is
   * sticky-only) */
  MI(FA_T3, 32);
  br = BR(FALG_BR_GE, FA_T2, FA_T3);      /* 32 >= d: keep */
  MI(FA_T2, 32);
  BIND(br);
  MI(FA_T3, 2);
  br = BR(FALG_BR_LTU, FA_T3, FA_T2);     /* d < 2: left shift */
  /* sticky for d >= 3: B's bits below R, before the aligning shift
   * drops them (B << (34 - d) is nonzero exactly when those bits
   * are -- only true of the unshifted mantissa) */
  MI(FA_T3, 2);
  br2 = BR(FALG_BR_GE, FA_T2, FA_T3);     /* 2 >= d: none */
  MI(FA_T3, 34);
  ALU(FALG_ALU_SUB, FA_T3, FA_T3, FA_T2);
  SSL(FA_T3, FA_T1, FA_T3);
  ALU(FALG_ALU_OR, FA_STICKY, FA_STICKY, FA_T3);
  BIND(br2);
  /* d >= 2: aligned = B >> (d - 2) (B's G,R land in bits 1,0) */
  ADDI(FA_T3, FA_T2, -2);
  SSR(FA_T1, FA_T1, FA_T3);
  uj = UJMP(FA_T3);
  BIND(br);
  /* d in {0, 1}: left shift 2-d, nothing below G */
  MI(FA_T3, 2);
  ALU(FALG_ALU_SUB, FA_T3, FA_T3, FA_T2);
  SSL(FA_T1, FA_T1, FA_T3);
  BIND(uj);
  /* sum / difference on the guard-bit grid.  An exact cancel
   * (mant == 0) falls into pack, whose zero guard returns the
   * correctly signed zero: with sticky != 0 the true value is a
   * tiny remainder of the subtrahend's sign, still zero under
   * round-to-nearest. */
  MI(FA_T3, 2);
  SSL(FA_MANT, FA_MANT, FA_T3);
  if (is_add) {
    ALU(FALG_ALU_ADD, FA_MANT, FA_MANT, FA_T1);
  } else {
    ALU(FALG_ALU_SUB, FA_MANT, FA_MANT, FA_T1);
  }
}

void falg_addsub_core(pfile_t *pf, const falg_ops_t *ops,
    boolean_t is_sub)
{
  falg_brh_t b1z, b1s, b2z, b2s, bun, ujoin, br, uj2, uj3, uj5;

  if (is_sub) {
    /* effective operand2 = -operand2 */
    PL(FA_T2, FALG_SIGN);
    ALU(FALG_ALU_XOR, FA_OP2, FA_OP2, FA_T2);
  }
  falg_test_special(pf, ops, FA_OP1, &b1z, &b1s);
  falg_test_special(pf, ops, FA_OP2, &b2z, &b2s);
  /* like effective signs -> magnitude add; unlike -> mag sub */
  ALU(FALG_ALU_XOR, FA_T2, FA_OP1, FA_OP2);
  MI(FA_T3, 31);
  SSR(FA_T3, FA_T2, FA_T3);
  bun = BR(FALG_BR_NEZ, 0, FA_T3);
  /* === magnitude add (common sign) === */
  /* SIGN_OF(FA_OP1) */
  MI(FA_T3, 31);
  SSR(FA_SIGN, FA_OP1, FA_T3);
  falg_extract_normalize(pf, ops, FA_OP1, FA_EXP, FA_MANT);
  falg_extract_normalize(pf, ops, FA_OP2, FA_T0, FA_T1);
  /* grid order: larger exponent's mantissa in MANT */
  ALU(FALG_ALU_SUB, FA_T2, FA_EXP, FA_T0);  /* d = e1 - e2 */
  MI(FA_T3, 0);
  br = BR(FALG_BR_LT, FA_T2, FA_T3);      /* d > 0: keep order */
  /* SWAP_MANTS: MANT <-> T1 through T3 */
  ALU(FALG_ALU_OR, FA_T3, FA_MANT, FA_MANT);
  ALU(FALG_ALU_OR, FA_MANT, FA_T1, FA_T1);
  ALU(FALG_ALU_OR, FA_T1, FA_T3, FA_T3);
  ALU(FALG_ALU_OR, FA_EXP, FA_T0, FA_T0);
  ops->neg(pf, FA_T2);
  BIND(br);
  MI(FA_STICKY, 0);
  falg_align_tail(pf, ops, BOOLEAN_TRUE);
  ujoin = UJMP(FA_T3);
  /* === magnitude subtract (larger magnitude's sign) === */
  BIND(bun);
  falg_extract_normalize(pf, ops, FA_OP1, FA_EXP, FA_MANT);
  falg_extract_normalize(pf, ops, FA_OP2, FA_T0, FA_T1);
  ALU(FALG_ALU_SUB, FA_T2, FA_EXP, FA_T0);  /* d = e1 - e2 */
  MI(FA_T3, 0);
  br = BR(FALG_BR_GE, FA_T2, FA_T3);      /* 0 >= d: compare */
  /* d > 0: no swap, larger is operand1 */
  /* SIGN_OF(FA_OP1) */
  MI(FA_T3, 31);
  SSR(FA_SIGN, FA_OP1, FA_T3);
  uj2 = UJMP(FA_T3);
  BIND(br);
  {
    falg_brh_t beq = BR(FALG_BR_EQZ, 0, FA_T2);
    falg_brh_t bltu;
    falg_brh_t uj4;

    /* d < 0: swap; larger is operand2 */
    ALU(FALG_ALU_OR, FA_T3, FA_MANT, FA_MANT);
    ALU(FALG_ALU_OR, FA_MANT, FA_T1, FA_T1);
    ALU(FALG_ALU_OR, FA_T1, FA_T3, FA_T3);
    ALU(FALG_ALU_OR, FA_EXP, FA_T0, FA_T0);
    ops->neg(pf, FA_T2);
    /* SIGN_OF(FA_OP2) */
    MI(FA_T3, 31);
    SSR(FA_SIGN, FA_OP2, FA_T3);
    uj5 = UJMP(FA_T3);
    BIND(beq);
    /* d == 0: m1 < m2 -> swap */
    bltu = BR(FALG_BR_LTU, FA_T1, FA_MANT);  /* m1 < m2 */
    /* SIGN_OF(FA_OP1) */
    MI(FA_T3, 31);
    SSR(FA_SIGN, FA_OP1, FA_T3);
    uj3 = UJMP(FA_T3);
    BIND(bltu);
    ALU(FALG_ALU_OR, FA_T3, FA_MANT, FA_MANT);
    ALU(FALG_ALU_OR, FA_MANT, FA_T1, FA_T1);
    ALU(FALG_ALU_OR, FA_T1, FA_T3, FA_T3);
    /* SIGN_OF(FA_OP2) */
    MI(FA_T3, 31);
    SSR(FA_SIGN, FA_OP2, FA_T3);
    /* d stays 0 */
    uj4 = UJMP(FA_T3);
    ops->bind4(pf, uj2, uj5, uj3, uj4);
  }
  MI(FA_STICKY, 0);
  falg_align_tail(pf, ops, BOOLEAN_FALSE);
  /* === pack === */
  BIND(ujoin);
  falg_pack_core(pf, ops);
  RTN();
  /* === special handlers === */
  /* operand1 +-0: result = operand2; when both are zero the IEEE
   * sign rule applies (add: s1&s2, sub: s1&~s2 -- the flip already
   * applied for SUB) */
  BIND(b1z);
  {
    falg_brh_t bnz;

    MI(FA_T3, 1);
    SSL(FA_T2, FA_OP2, FA_T3);
    bnz = BR(FALG_BR_NEZ, 0, FA_T2);
    /* both zero: add rule s1 & s2 */
    MI(FA_T3, 31);
    SSR(FA_SIGN, FA_OP1, FA_T3);
    MI(FA_T3, 31);
    SSR(FA_T3, FA_OP2, FA_T3);               /* t3 = s2 */
    ALU(FALG_ALU_AND, FA_T3, FA_T3, FA_SIGN);
    MI(FA_T2, 31);
    SSL(FA_T2, FA_T3, FA_T2);
    ALU(FALG_ALU_OR, FA_OP1, FA_T2, FA_T2);
    RTN();
    BIND(bnz);
    /* operand2 nonzero: result = operand2 */
    ALU(FALG_ALU_OR, FA_OP1, FA_OP2, FA_OP2);
    RTN();
  }
  /* operand2 +-0 (operand1 finite): result = operand1 */
  BIND(b2z);
  RTN();
  /* operand1 inf/NaN */
  BIND(b1s);
  {
    falg_brh_t bn, bz3, bs3, bn2, bnn, uj;

    MI(FA_T3, 9);
    SSL(FA_T3, FA_OP1, FA_T3);
    bn = BR(FALG_BR_EQZ, 0, FA_T3);   /* mant 0: inf */
    /* NaN: quiet and propagate */
    PL(FA_T3, FALG_QPROP);
    ALU(FALG_ALU_OR, FA_OP1, FA_OP1, FA_T3);
    RTN();
    BIND(bn);
    /* op1 inf: check operand2 */
    falg_test_special(pf, ops, FA_OP2, &bz3, &bs3);
    uj = UJMP(FA_T3);
    /* op2 +-0: inf + 0 = inf (op1) */
    BIND(bz3);
    RTN();
    /* op2 inf/NaN */
    BIND(bs3);
    MI(FA_T3, 9);
    SSL(FA_T3, FA_OP2, FA_T3);
    bn2 = BR(FALG_BR_EQZ, 0, FA_T3);
    /* op2 NaN wins */
    PL(FA_T3, FALG_QPROP);
    ALU(FALG_ALU_OR, FA_OP1, FA_OP2, FA_T3);
    RTN();
    BIND(bn2);
    /* both inf: same effective sign -> op1; opposite -> NaN */
    PL(FA_T3, FALG_SIGN);
    ALU(FALG_ALU_XOR, FA_T2, FA_OP1, FA_OP2);
    ALU(FALG_ALU_AND, FA_T2, FA_T2, FA_T3);
    bnn = BR(FALG_BR_EQZ, 0, FA_T2);
    falg_return_qnan(pf, ops);
    BIND(bnn);
    RTN();
    BIND(uj);
    RTN();                                 /* op2 finite: result = op1 */
  }
  /* operand2 inf/NaN (operand1 finite) */
  BIND(b2s);
  {
    falg_brh_t bn;

    MI(FA_T3, 9);
    SSL(FA_T3, FA_OP2, FA_T3);
    bn = BR(FALG_BR_EQZ, 0, FA_T3);
    /* NaN: quiet and propagate */
    PL(FA_T3, FALG_QPROP);
    ALU(FALG_ALU_OR, FA_OP1, FA_OP2, FA_T3);
    RTN();
    BIND(bn);
    /* op2 inf: result = op2 (effective sign already applied) */
    ALU(FALG_ALU_OR, FA_OP1, FA_OP2, FA_OP2);
    RTN();
  }
}

/* ---- cut 4: DIV (verbatim from esp_float.c emit_div_core) ----
 * register plan per the original block comment: MANT=N, T0=D,
 * T1=R, T2=Q, T3 scratch, STICKY counter (then sticky). */
void falg_div_core(pfile_t *pf, const falg_ops_t *ops)
{
  falg_brh_t bs1, bs2, bz1, bz2, bloop, bn, bn2, br;

  /* classify both (NaN must win over 0/inf via the handlers) */
  falg_test_special(pf, ops, FA_OP1, &bz1, &bs1);
  falg_test_special(pf, ops, FA_OP2, &bz2, &bs2);
  /* === normal path ===
   * MANT = N, T0 = D, T1 = R, T2 = Q, T3 scratch, STICKY counter */
  falg_extract_normalize(pf, ops, FA_OP1, FA_EXP, FA_MANT);
  falg_extract_normalize(pf, ops, FA_OP2, FA_T1, FA_T0);
  /* SIGN_OF(FA_OP1) */
  MI(FA_T3, 31);
  SSR(FA_SIGN, FA_OP1, FA_T3);
  MI(FA_T3, 31);
  SSR(FA_T3, FA_OP2, FA_T3);
  ALU(FALG_ALU_XOR, FA_SIGN, FA_SIGN, FA_T3);
  ALU(FALG_ALU_SUB, FA_EXP, FA_EXP, FA_T1);
  MI(FA_T3, 127);
  ALU(FALG_ALU_ADD, FA_EXP, FA_EXP, FA_T3);
  /* if N < D: N <<= 1, exp-- (quotient into [1, 2)) */
  br = BR(FALG_BR_GEU, FA_T0, FA_MANT);  /* N >= D: skip */
  ALU(FALG_ALU_ADD, FA_MANT, FA_MANT, FA_MANT);
  MI(FA_T3, 1);
  ALU(FALG_ALU_SUB, FA_EXP, FA_EXP, FA_T3);
  BIND(br);
  /* N in [D, 2D) puts the MSB at bit 23 (no prescale) or 24 (N was
   * scaled up); the sign is already out of OP1, so it is free.
   * EXT's position field is narrow on some targets, so the SRL does
   * the move */
  MI(FA_T3, 24);
  SSR(FA_OP1, FA_MANT, FA_T3);           /* op1 = N >> 24 */
  EXT(FA_OP1, FA_OP1, 0, 1);             /* MSB at 24? */
  MI(FA_T1, 0);                          /* R = 0 */
  MI(FA_T2, 0);                          /* Q = 0 */
  /* numerator bits + 25 zero bits: Q = floor(N * 2^25 / D) in
   * [2^25, 2^26) = S<<2 with G,R in bits 1,0.  A numerator that
   * spans 25 bits needs its MSB fed (SAR 24) and one extra
   * restoring step to keep the same quotient grid. */
  MI(FA_STICKY, 49);
  ALU(FALG_ALU_ADD, FA_STICKY, FA_STICKY, FA_OP1);
  MI(FA_T3, 23);
  ALU(FALG_ALU_ADD, FA_OP1, FA_OP1, FA_T3);
  ops->ssr_set(pf, FA_OP1);
  bloop = ops->label_here(pf);
  ops->srl_sar(pf, FA_T3, FA_MANT, FA_OP1);
  EXT(FA_T3, FA_T3, 0, 1);   /* keep one bit */
  ALU(FALG_ALU_ADD, FA_T1, FA_T1, FA_T1);   /* R <<= 1 */
  ALU(FALG_ALU_ADD, FA_T1, FA_T1, FA_T3);   /* R |= bit */
  ALU(FALG_ALU_ADD, FA_MANT, FA_MANT, FA_MANT);   /* N <<= 1 */
  ALU(FALG_ALU_ADD, FA_T2, FA_T2, FA_T2);   /* Q <<= 1 */
  br = BR(FALG_BR_LTU, FA_T0, FA_T1);        /* R < D */
  ALU(FALG_ALU_SUB, FA_T1, FA_T1, FA_T0);
  ADDI(FA_T2, FA_T2, 1);
  BIND(br);
  ADDI(FA_STICKY, FA_STICKY, -1);
  ops->br_back(pf, FALG_BR_NEZ, 0, FA_STICKY, bloop);
  /* sticky = remainder != 0 */
  MI(FA_STICKY, 0);
  bn = BR(FALG_BR_EQZ, 0, FA_T1);
  MI(FA_STICKY, 1);
  BIND(bn);
  ALU(FALG_ALU_OR, FA_MANT, FA_T2, FA_T2);  /* MANT = Q */
  falg_pack_core(pf, ops);
  RTN();
  /* === handlers === */
  /* op1 inf/NaN */
  BIND(bs1);
  MI(FA_T3, 9);
  SSL(FA_T3, FA_OP1, FA_T3);
  bn = BR(FALG_BR_EQZ, 0, FA_T3);
  PL(FA_T3, FALG_QPROP);
  ALU(FALG_ALU_OR, FA_OP1, FA_OP1, FA_T3);
  RTN();
  BIND(bn);
  /* op1 inf: op2 NaN or inf -> NaN; else inf with s1^s2 */
  MI(FA_T3, 1);
  SSL(FA_T3, FA_OP2, FA_T3);            /* t3 = op2 << 1 */
  PL(FA_T2, FALG_EXPMAX);
  bn2 = BR(FALG_BR_GEU, FA_T2, FA_T3);  /* op2<<1 >= FF..: inf/NaN */
  PL(FA_T3, FALG_SIGN);
  ALU(FALG_ALU_AND, FA_T3, FA_OP2, FA_T3);
  ALU(FALG_ALU_XOR, FA_OP1, FA_OP1, FA_T3);
  RTN();
  BIND(bn2);
  falg_return_qnan(pf, ops);
  /* op2 inf/NaN (op1 finite non-special) */
  BIND(bs2);
  MI(FA_T3, 9);
  SSL(FA_T3, FA_OP2, FA_T3);
  bn = BR(FALG_BR_EQZ, 0, FA_T3);
  PL(FA_T3, FALG_QPROP);
  ALU(FALG_ALU_OR, FA_OP1, FA_OP2, FA_T3);
  RTN();
  BIND(bn);
  /* finite / inf = +-0 with s1^s2 */
  PL(FA_T3, FALG_SIGN);
  ALU(FALG_ALU_XOR, FA_T2, FA_OP1, FA_OP2);
  ALU(FALG_ALU_AND, FA_T2, FA_T2, FA_T3);
  ALU(FALG_ALU_OR, FA_OP1, FA_T2, FA_T2);
  RTN();
  /* op2 +-0 (op1 finite non-special): finite / 0 = inf with s1^s2 */
  BIND(bz2);
  PL(FA_T3, FALG_SIGN);
  ALU(FALG_ALU_XOR, FA_T2, FA_OP1, FA_OP2);
  ALU(FALG_ALU_AND, FA_T2, FA_T2, FA_T3);
  PL(FA_T3, FALG_INF);
  ALU(FALG_ALU_OR, FA_T2, FA_T2, FA_T3);
  ALU(FALG_ALU_OR, FA_OP1, FA_T2, FA_T2);
  RTN();
  /* op1 +-0: op2 is untested here -- 0/0 invalid, 0/NaN propagates,
   * 0/inf and 0/finite give +-0 (= op1) */
  BIND(bz1);
  {
    falg_brh_t bz3, bs3, cn;

    falg_test_special(pf, ops, FA_OP2, &bz3, &bs3);
    RTN();                                 /* op2 finite: +-0 / finite */
    BIND(bz3);
    falg_return_qnan(pf, ops);             /* 0 / 0 */
    BIND(bs3);
    MI(FA_T3, 9);
    SSL(FA_T3, FA_OP2, FA_T3);
    cn = BR(FALG_BR_EQZ, 0, FA_T3);
    PL(FA_T3, FALG_QPROP);
    ALU(FALG_ALU_OR, FA_OP1, FA_OP2, FA_T3);  /* 0 / NaN */
    RTN();
    BIND(cn);
    RTN();                                 /* 0 / inf = +-0 (= op1) */
  }
}

/* ---- cut 5: conversions (verbatim from esp_float.c) ---- */
void falg_i2f_core(pfile_t *pf, const falg_ops_t *ops)
{
  falg_brh_t br;

  /* if OP1 != 0, run the conversion; zero returns 0.0 unchanged */
  br = BR(FALG_BR_NEZ, 0, FA_OP1);
  RTN();
  BIND(br);

  /* save sign, make positive */
  MI(FA_T3, 31);
  ops->ssr_set(pf, FA_T3);
  ops->srl_sar(pf, FA_SIGN, FA_OP1, FA_T3);
  br = BR(FALG_BR_EQZ, 0, FA_SIGN);
  ops->neg(pf, FA_OP1);
  BIND(br);

  /* find the highest set bit: CLZ gives leading zeros */
  ops->clz(pf, FA_T3, FA_OP1);
  /* exponent = 127 + 31 - clz = 158 - clz */
  MI(FA_T2, 158);
  ALU(FALG_ALU_SUB, FA_EXP, FA_T2, FA_T3);

  /* mantissa: shift OP1 left so the leading 1 moves to bit 23.
   * shift = 23 - (31 - clz) = clz - 8.  If clz > 8: shift left by
   * (clz - 8).  If clz < 8: shift right by (8 - clz).  Bits
   * shifted out are dropped (truncation for |int| >= 2^24 --
   * documented limitation). */
  MI(FA_T2, 8);
  ALU(FALG_ALU_SUB, FA_T3, FA_T3, FA_T2);  /* t3 = clz - 8 */

  /* if t3 >= 0: shift left.  t3 == 0 (input in [2^23, 2^24)) must
   * run too: the by-0 shift is the copy that defines MANT */
  MI(FA_T2, 0);
  br = BR(FALG_BR_LT, FA_T2, FA_T3);   /* t3 < 0: skip */
  SSL(FA_MANT, FA_OP1, FA_T3);
  BIND(br);

  /* if t3 < 0: shift right by -t3 (signed test: a negative t3
   * reads huge unsigned and would wrongly skip under LTU) */
  MI(FA_T2, 0);
  br = BR(FALG_BR_GE, FA_T2, FA_T3);   /* t3 >= 0: skip */
  ops->neg(pf, FA_T3);
  SSR(FA_MANT, FA_OP1, FA_T3);
  BIND(br);

  /* MANT now has the leading 1 at bit 23 (mantissa + implicit bit) */

  /* reassemble: OP1 = sign<<31 | exp<<23 | (mant & 0x7FFFFF) */
  PL(FA_T2, FALG_MANT);
  ALU(FALG_ALU_AND, FA_OP1, FA_MANT, FA_T2);
  MI(FA_T3, 23);
  SSL(FA_T3, FA_EXP, FA_T3);
  ALU(FALG_ALU_OR, FA_OP1, FA_OP1, FA_T3);
  MI(FA_T3, 31);
  SSL(FA_T3, FA_SIGN, FA_T3);
  ALU(FALG_ALU_OR, FA_OP1, FA_OP1, FA_T3);
  RTN();
}

void falg_f2i_core(pfile_t *pf, const falg_ops_t *ops)
{
  falg_brh_t br, br2, bn;

  /* save sign before overwriting OP1 */
  MI(FA_T3, 31);
  ops->ssr_set(pf, FA_T3);
  ops->srl_sar(pf, FA_SIGN, FA_OP1, FA_T3);

  /* extract exponent */
  MI(FA_T3, 23);
  ops->ssr_set(pf, FA_T3);
  ops->srl_sar(pf, FA_EXP, FA_OP1, FA_T3);
  EXT(FA_EXP, FA_EXP, 0, 8);

  /* |x| >= 2^31 (exp >= 158), +-inf, NaN: special results (the
   * shift path would wrap through a 5-bit count) */
  MI(FA_T2, 158);
  br = BR(FALG_BR_LTU, FA_T2, FA_EXP);  /* exp < 158: normal */
  /* NaN? exp == 255 with mantissa != 0 */
  MI(FA_T2, 255);
  br2 = BR(FALG_BR_NE, FA_T2, FA_EXP);  /* exp != 255: sat */
  MI(FA_T3, 9);
  SSL(FA_T3, FA_OP1, FA_T3);
  bn = BR(FALG_BR_EQZ, 0, FA_T3);       /* mant 0: inf */
  MI(FA_OP1, 0);                        /* NaN -> 0 */
  RTN();
  BIND(br2);
  BIND(bn);
  /* saturate: sign ? INT_MIN : INT_MAX */
  {
    falg_brh_t bnz = BR(FALG_BR_NEZ, 0, FA_SIGN);

    PL(FA_OP1, 0x7FFFFFFFUL);
    RTN();
    BIND(bnz);
    PL(FA_OP1, FALG_SIGN);
    RTN();
  }
  BIND(br);

  /* if exp >= 127 (|value| >= 1), convert; smaller return 0
   * (covers +-0 and all denormals) */
  MI(FA_T3, 127);
  br = BR(FALG_BR_GEU, FA_T3, FA_EXP);
  MI(FA_OP1, 0);
  RTN();
  BIND(br);

  /* extract mantissa with implicit bit */
  PL(FA_T2, FALG_MANT);
  ALU(FALG_ALU_AND, FA_MANT, FA_OP1, FA_T2);
  PL(FA_T2, 0x800000UL);
  ALU(FALG_ALU_OR, FA_MANT, FA_MANT, FA_T2);

  /* shift = exp - 127 - 23 = exp - 150.  exp <= 150 (|value| <
   * 2^23): shift RIGHT by 150 - exp; exp > 150: shift LEFT by
   * exp - 150 (max 7: the >= 158 case was diverted above). */
  MI(FA_T3, 150);
  ALU(FALG_ALU_SUB, FA_T3, FA_EXP, FA_T3);
  MI(FA_T2, 0);
  br = BR(FALG_BR_GE, FA_T3, FA_T2);   /* t3 <= 0: right */
  SSL(FA_OP1, FA_MANT, FA_T3);
  BIND(br);
  MI(FA_T2, 0);
  br = BR(FALG_BR_GE, FA_T2, FA_T3);   /* t3 >= 0: done */
  ops->neg(pf, FA_T3);
  SSR(FA_OP1, FA_MANT, FA_T3);
  BIND(br);

  /* negate if original sign was set */
  br = BR(FALG_BR_EQZ, 0, FA_SIGN);
  ops->neg(pf, FA_OP1);
  BIND(br);
}

