/************************************************************
 **
 ** mips_opfn.c : PIC32 IEEE754 single-precision soft-float
 **
 ** Outlined MIPS helpers mirroring pic_opfn.c float routines:
 ** SUB/ADD/MUL/DIV/SCONV/CONV/TOINT.  Operands in _mips_fval1 /
 ** _mips_fval2; integer convert buffer _mips_fconv.  Correct for
 ** normals and zero; NaN/Inf flush to 0.
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include <string.h>
#include <stdio.h>

#include "../libutils/mem.h"
#include "../libcore/pfile.h"
#include "../libcore/float_alg.h"
#include "../libcore/pf_proc.h"
#include "../libcore/value.h"
#include "../libcore/variable.h"
#include "../libcore/vardef.h"
#include "../libcore/label.h"
#include "../libcore/operator.h"
#include "../libcore/cmd.h"
#include "../libcore/cmd_brch.h"
#include "mips.h"

/* ---- scratch RAM ---- */

static value_t mips_float_value_get(pfile_t *pf, const char *name,
    variable_def_type_t type, variable_sz_t sz)
{
  value_t val;

  val = pfile_value_find(pf, PFILE_LOG_NONE, name);
  if (!val && ((variable_sz_t) -1 != sz)) {
    variable_t     var;
    variable_def_t def;

    def = variable_def_alloc(0, type, VARIABLE_DEF_FLAG_NONE, sz);
    var = VARIABLE_NONE;
    if (def
        && (RESULT_OK == pfile_variable_alloc(pf,
            PFILE_VARIABLE_ALLOC_GLOBAL, name, def, VARIABLE_NONE, &var))) {
      if (VARIABLE_BASE_UNKNOWN == variable_base_get(var, 0)) {
        mips_var_place(pf, var, sz);
      }
      val = value_alloc(var);
      variable_release(var);
    } else if (def) {
      variable_def_free(def);
    }
  }
  if (val) {
    if ((value_type_get(val) != type)
        || ((((variable_sz_t) -1) != sz) && (value_sz_get(val) != sz))) {
      variable_def_t def;
      variable_t     var;

      def = variable_def_alloc(0, type, VARIABLE_DEF_FLAG_NONE, sz);
      var = value_variable_get(val);
      if (def && var && (sz > (variable_sz_t) value_sz_get(val))) {
        variable_def_set(var, def);
        if (VARIABLE_BASE_UNKNOWN == variable_base_get(var, 0)) {
          mips_var_place(pf, var, sz);
        }
      }
      if (def) {
        value_def_set(val, def);
      }
    }
    mips_value_ensure(pf, val);
  }
  return val;
}

static value_t mips_fval1_get(pfile_t *pf)
{
  /* Touch companion scratch so layout matches the PIC float set. */
  value_release(mips_float_value_get(pf, "_mips_fexp1",
      VARIABLE_DEF_TYPE_INTEGER, 1));
  value_release(mips_float_value_get(pf, "_mips_fexp2",
      VARIABLE_DEF_TYPE_INTEGER, 1));
  return mips_float_value_get(pf, "_mips_fval1",
      VARIABLE_DEF_TYPE_FLOAT, 4);
}

static value_t mips_fval2_get(pfile_t *pf)
{
  return mips_float_value_get(pf, "_mips_fval2",
      VARIABLE_DEF_TYPE_FLOAT, 4);
}

static value_t mips_fconv_get(pfile_t *pf, variable_sz_t sz)
{
  value_t val;

  if ((variable_sz_t) -1 == sz || sz < 1) {
    sz = 4;
  }
  if (sz > 4) {
    sz = 4;
  }
  val = mips_float_value_get(pf, "_mips_fconv",
      VARIABLE_DEF_TYPE_INTEGER, -1);
  if (!val || (value_sz_get(val) < sz)) {
    if (val) {
      value_release(val);
    }
    val = mips_float_value_get(pf, "_mips_fconv",
        VARIABLE_DEF_TYPE_INTEGER, sz);
  }
  return val;
}

static label_t mips_float_label(pfile_t *pf, const char *name,
    boolean_t alloc)
{
  label_t lbl;

  lbl = pfile_label_find(pf, PFILE_LOG_NONE, name);
  if (!lbl && alloc) {
    lbl = pfile_label_alloc(pf, name);
  }
  return lbl;
}

static void mips_float_label_here(label_t lbl)
{
  mips_code_t node;

  node = mips_code_label_alloc();
  if (lbl && node) {
    label_code_set(lbl, node);
  }
}

static void mips_float_return(void)
{
  (void) mips_code_r_alloc(MIPS_OP_JR, 0, MIPS_REG_RA, 0, 0);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
}

static label_t mips_anon_label(pfile_t *pf)
{
  return pfile_label_alloc(pf, 0);
}

static void mips_beqz(unsigned rs, label_t lbl)
{
  (void) mips_emit_cond_branch(MIPS_OP_BEQ, rs, MIPS_REG_ZERO, lbl);
}

static void mips_bnez(unsigned rs, label_t lbl)
{
  (void) mips_emit_cond_branch(MIPS_OP_BNE, rs, MIPS_REG_ZERO, lbl);
}

/* 0x80000000 sign mask into `reg`. */
static void mips_load_sign_mask(unsigned reg)
{
  (void) mips_code_alloc(MIPS_OP_LUI, reg, 0, 0x8000);
}

/* Copy rs -> rd. */
static void mips_move(unsigned rd, unsigned rs)
{
  (void) mips_code_r_alloc(MIPS_OP_ADDU, rd, rs, MIPS_REG_ZERO, 0);
}

/* ---- float_alg kernel wiring (cuts 2-5): the ESP-canonical IEEE
 * algorithm expressed over MIPS32r2 primitives.  Register map keeps
 * the existing helper contract ($s0/$s1 operands+result, result
 * stored to the _mips_fval1 cell at every exit -- the kernel's
 * helper-return op) and stays inside the register set today's float
 * helpers already clobber ($t0..$t7, $s0..$s2; $at is the fused-
 * compare scratch, free inside these leaf routines). */
#define MFALG_OP1    MIPS_REG_S0
#define MFALG_OP2    MIPS_REG_S1
#define MFALG_EXP    MIPS_REG_T3
#define MFALG_MANT   MIPS_REG_T5
#define MFALG_T0     MIPS_REG_T4
#define MFALG_T1     MIPS_REG_T6
#define MFALG_T2     MIPS_REG_T7
#define MFALG_T3     MIPS_REG_T2
#define MFALG_SIGN   MIPS_REG_S2
#define MFALG_STICKY MIPS_REG_T0

/* abstract FA_* index -> physical register (the map above); ops
 * primitives receive ABSTRACT indices from the kernel */
static unsigned mfalg_reg(int r)
{
  switch (r) {
    case FA_OP1:    return MFALG_OP1;
    case FA_OP2:    return MFALG_OP2;
    case FA_EXP:    return MFALG_EXP;
    case FA_MANT:   return MFALG_MANT;
    case FA_T0:     return MFALG_T0;
    case FA_T1:     return MFALG_T1;
    case FA_T2:     return MFALG_T2;
    case FA_T3:     return MFALG_T3;
    case FA_SIGN:   return MFALG_SIGN;
    default:        return MFALG_STICKY;   /* FA_STICKY */
  }
}

static void mfalg_movi(pfile_t *pf, int rd, long imm)
{
  (void) pf;
  (void) mips_code_alloc(MIPS_OP_ADDIU, mfalg_reg(rd), MIPS_REG_ZERO,
      (int) imm);
}

static void mfalg_load_const(pfile_t *pf, int rd, unsigned long val)
{
  (void) pf;
  (void) mips_code_alloc(MIPS_OP_LUI, mfalg_reg(rd), 0,
      (int) ((val >> 16) & 0xFFFF));
  (void) mips_code_alloc(MIPS_OP_ORI, mfalg_reg(rd), mfalg_reg(rd),
      (int) (val & 0xFFFF));
}

static void mfalg_alu(pfile_t *pf, falg_alu_t op, int rd, int rs, int rt)
{
  mips_opcode_t mop;

  (void) pf;

  switch (op) {
    case FALG_ALU_ADD: mop = MIPS_OP_ADDU; break;
    case FALG_ALU_SUB: mop = MIPS_OP_SUBU; break;
    case FALG_ALU_AND: mop = MIPS_OP_AND;  break;
    case FALG_ALU_OR:  mop = MIPS_OP_OR;   break;
    default:           mop = MIPS_OP_XOR;  break;
  }
  (void) mips_code_r_alloc(mop, mfalg_reg(rd), mfalg_reg(rs),
      mfalg_reg(rt), 0);
}

static void mfalg_addi(pfile_t *pf, int rd, int rs, long imm)
{
  (void) pf;
  (void) mips_code_alloc(MIPS_OP_ADDIU, mfalg_reg(rd), mfalg_reg(rs),
      (int) imm);
}

static void mfalg_sll_var(pfile_t *pf, int rd, int rs, int sar)
{
  /* SLLV/SRLV semantics: rd = rt <</>> (rs & 31) -- the VALUE rides
   * the rt field, the COUNT the rs field (encoder maps r_alloc's
   * (rd, rs, rt) straight through) */
  (void) pf;
  (void) mips_code_r_alloc(MIPS_OP_SLLV, mfalg_reg(rd),
      mfalg_reg(sar), mfalg_reg(rs), 0);
}

static void mfalg_srl_var(pfile_t *pf, int rd, int rs, int sar)
{
  (void) pf;
  (void) mips_code_r_alloc(MIPS_OP_SRLV, mfalg_reg(rd),
      mfalg_reg(sar), mfalg_reg(rs), 0);
}

static void mfalg_ext(pfile_t *pf, int rd, int rs, int sh, int w)
{
  (void) pf;
  (void) mips_code_r_alloc(MIPS_OP_SRL, mfalg_reg(rd), 0,
      mfalg_reg(rs), (unsigned) sh);
  (void) mips_code_alloc(MIPS_OP_ANDI, mfalg_reg(rd), mfalg_reg(rd),
      (int) ((1UL << w) - 1UL));
}

static void mfalg_clz(pfile_t *pf, int rd, int rs)
{
  (void) pf;
  (void) mips_code_r_alloc(MIPS_OP_CLZ, mfalg_reg(rd), mfalg_reg(rs),
      0, 0);
}

/* branch handles are labels; bind places the label here (mips branch
 * fixups resolve at layout) */
static falg_brh_t mfalg_br(pfile_t *pf, falg_br_t cond, int at, int as)
{
  label_t lbl = mips_anon_label(pf);

  switch (cond) {
    case FALG_BR_EQZ:
      mips_beqz(mfalg_reg(as), lbl);
      break;
    case FALG_BR_NEZ:
      mips_bnez(mfalg_reg(as), lbl);
      break;
    case FALG_BR_GEU:
      /* as >=u at  <=>  !(as < at) */
      (void) mips_code_r_alloc(MIPS_OP_SLTU, MIPS_REG_AT,
          mfalg_reg(as), mfalg_reg(at), 0);
      mips_beqz(MIPS_REG_AT, lbl);
      break;
    case FALG_BR_LTU:
      /* as <u at */
      (void) mips_code_r_alloc(MIPS_OP_SLTU, MIPS_REG_AT,
          mfalg_reg(as), mfalg_reg(at), 0);
      mips_bnez(MIPS_REG_AT, lbl);
      break;
    case FALG_BR_LT:
      /* as < at signed */
      (void) mips_code_r_alloc(MIPS_OP_SLT, MIPS_REG_AT,
          mfalg_reg(as), mfalg_reg(at), 0);
      mips_bnez(MIPS_REG_AT, lbl);
      break;
    case FALG_BR_NE:
      (void) mips_emit_cond_branch(MIPS_OP_BNE, mfalg_reg(as),
          mfalg_reg(at), lbl);
      break;
    default: /* FALG_BR_GE: as >= at signed <=> !(as < at) */
      (void) mips_code_r_alloc(MIPS_OP_SLT, MIPS_REG_AT,
          mfalg_reg(as), mfalg_reg(at), 0);
      mips_beqz(MIPS_REG_AT, lbl);
      break;
  }
  return (falg_brh_t) lbl;
}

static void mfalg_bind1(pfile_t *pf, falg_brh_t b)
{
  label_t lbl = (label_t) b;

  (void) pf;

  if (lbl) {
    mips_float_label_here(lbl);
    label_release(lbl);
  }
}

static void mfalg_bind2(pfile_t *pf, falg_brh_t b1, falg_brh_t b2)
{
  (void) pf;
  mfalg_bind1(pf, b1);
  mfalg_bind1(pf, b2);
}

static void mfalg_bind4(pfile_t *pf, falg_brh_t b1, falg_brh_t b2,
    falg_brh_t b3, falg_brh_t b4)
{
  (void) pf;
  mfalg_bind1(pf, b1);
  mfalg_bind1(pf, b2);
  mfalg_bind1(pf, b3);
  mfalg_bind1(pf, b4);
}

static void mfalg_neg(pfile_t *pf, int rd)
{
  /* two's complement via subu from zero */
  (void) pf;
  (void) mips_code_r_alloc(MIPS_OP_SUBU, mfalg_reg(rd), MIPS_REG_ZERO,
      mfalg_reg(rd), 0);
}

static void mfalg_mul_lo(pfile_t *pf, int rd, int rs, int rt)
{
  (void) pf;
  (void) mips_code_r_alloc(MIPS_OP_MULTU, 0, mfalg_reg(rs),
      mfalg_reg(rt), 0);
  (void) mips_code_r_alloc(MIPS_OP_MFLO, mfalg_reg(rd), 0, 0, 0);
}

static void mfalg_mul_hi(pfile_t *pf, int rd, int rs, int rt)
{
  (void) pf;
  (void) mips_code_r_alloc(MIPS_OP_MULTU, 0, mfalg_reg(rs),
      mfalg_reg(rt), 0);
  (void) mips_code_r_alloc(MIPS_OP_MFHI, mfalg_reg(rd), 0, 0, 0);
}

/* cut 4 (DIV) shift-count preload: no count register on MIPS --
 * remember which register ssr_set named, shift by it each time */

static unsigned mfalg_sar_phys;

/* A4 teardown: forget the cached SAR register index. */
void mips_opfn_state_reset(void)
{
  mfalg_sar_phys = 0;
}

static void mfalg_ssr_set(pfile_t *pf, int sar)
{
  (void) pf;
  mfalg_sar_phys = mfalg_reg(sar);
}

static void mfalg_srl_sar(pfile_t *pf, int rd, int rs, int sar)
{
  (void) pf;
  (void) sar;
  (void) mips_code_r_alloc(MIPS_OP_SRLV, mfalg_reg(rd),
      mfalg_sar_phys, mfalg_reg(rs), 0);
}

static falg_brh_t mfalg_label_here(pfile_t *pf)
{
  label_t lbl = mips_anon_label(pf);

  mips_float_label_here(lbl);
  return (falg_brh_t) lbl;
}

static void mfalg_br_back(pfile_t *pf, falg_br_t cond, int at, int as,
    falg_brh_t target)
{
  label_t lbl = (label_t) target;

  (void) pf;

  switch (cond) {
    case FALG_BR_EQZ:
      mips_beqz(mfalg_reg(as), lbl);
      break;
    case FALG_BR_NEZ:
      mips_bnez(mfalg_reg(as), lbl);
      break;
    case FALG_BR_LTU:
      (void) mips_code_r_alloc(MIPS_OP_SLTU, MIPS_REG_AT,
          mfalg_reg(as), mfalg_reg(at), 0);
      mips_bnez(MIPS_REG_AT, lbl);
      break;
    default:
      break;
  }
  label_release(lbl);
}

static falg_brh_t mfalg_ujmp(pfile_t *pf, int scratch)
{
  label_t lbl = mips_anon_label(pf);

  (void) scratch;
  (void) mips_emit_j(lbl);
  return (falg_brh_t) lbl;
}

/* every kernel exit has the result in FA_OP1: store it to the result
 * cell, then return (the caller reads the cell) */
static void mfalg_ret(pfile_t *pf)
{
  value_t f1 = mips_fval1_get(pf);

  mips_store_value(pf, f1, MFALG_OP1);
  value_release(f1);
  mips_float_return();
}

static const falg_ops_t mips_falg_ops = {
  mfalg_movi,
  mfalg_load_const,
  mfalg_alu,
  mfalg_addi,
  mfalg_sll_var,
  mfalg_srl_var,
  mfalg_ext,
  mfalg_clz,
  mfalg_br,
  mfalg_bind1,
  mfalg_bind2,
  mfalg_bind4,
  mfalg_mul_lo,
  mfalg_mul_hi,
  mfalg_neg,
  mfalg_ssr_set,
  mfalg_srl_sar,
  mfalg_label_here,
  mfalg_br_back,
  mfalg_ujmp,
  mfalg_ret
};

/* cut 5 (TOINT): same primitives, but every kernel exit stores the
 * result to the integer conversion cell _mips_fconv instead of the
 * float result cell */
static void mfalg_ret_conv(pfile_t *pf)
{
  value_t conv = mips_fconv_get(pf, 4);

  mips_store_value(pf, conv, MFALG_OP1);
  value_release(conv);
  mips_float_return();
}

static const falg_ops_t mips_falg_conv_ops = {
  mfalg_movi,
  mfalg_load_const,
  mfalg_alu,
  mfalg_addi,
  mfalg_sll_var,
  mfalg_srl_var,
  mfalg_ext,
  mfalg_clz,
  mfalg_br,
  mfalg_bind1,
  mfalg_bind2,
  mfalg_bind4,
  mfalg_mul_lo,
  mfalg_mul_hi,
  mfalg_neg,
  mfalg_ssr_set,
  mfalg_srl_sar,
  mfalg_label_here,
  mfalg_br_back,
  mfalg_ujmp,
  mfalg_ret_conv
};

/* ================================================================
 * SUB / ADD
 * ================================================================ */

static void mips_float_sub_create(pfile_t *pf)
{
  value_t f1;
  value_t f2;

  /* cut 3: ESP-canonical IEEE SUB through the float_alg kernel (the
   * kernel flips operand2's effective sign itself) */
  f1 = mips_fval1_get(pf);
  f2 = mips_fval2_get(pf);
  mips_load_value(pf, f1, MFALG_OP1);
  mips_load_value(pf, f2, MFALG_OP2);
  value_release(f2);
  value_release(f1);
  falg_addsub_core(pf, &mips_falg_ops, BOOLEAN_TRUE);
}

static void mips_float_add_create(pfile_t *pf)
{
  value_t f1;
  value_t f2;

  /* cut 3: ESP-canonical IEEE ADD through the float_alg kernel
   * (pre-fix: truncating pack, no inf/NaN/denormals, overflow -> 0) */
  f1 = mips_fval1_get(pf);
  f2 = mips_fval2_get(pf);
  mips_load_value(pf, f1, MFALG_OP1);
  mips_load_value(pf, f2, MFALG_OP2);
  value_release(f2);
  value_release(f1);
  falg_addsub_core(pf, &mips_falg_ops, BOOLEAN_FALSE);
}

/* ================================================================
 * MUL
 * ================================================================ */

static void mips_float_multiply_create(pfile_t *pf)
{
  value_t f1;
  value_t f2;

  /* cut 2: the ESP-canonical IEEE MUL through the float_alg kernel
   * (pre-fix: truncating pack, no inf/NaN/denormals, overflow -> 0) */
  f1 = mips_fval1_get(pf);
  f2 = mips_fval2_get(pf);
  mips_load_value(pf, f1, MFALG_OP1);
  mips_load_value(pf, f2, MFALG_OP2);
  value_release(f2);
  value_release(f1);
  falg_mul_core(pf, &mips_falg_ops);
}

/* ================================================================
 * DIV  (restoring divide on 24-bit mantissas)
 * ================================================================ */

static void mips_float_divide_create(pfile_t *pf)
{
  value_t f1;
  value_t f2;

  /* cut 4: ESP-canonical IEEE DIV through the float_alg kernel
   * (pre-fix: truncating pack, no inf/NaN/denormals, overflow -> 0;
   * the 47-iteration forms differed) */
  f1 = mips_fval1_get(pf);
  f2 = mips_fval2_get(pf);
  mips_load_value(pf, f1, MFALG_OP1);
  mips_load_value(pf, f2, MFALG_OP2);
  value_release(f2);
  value_release(f1);
  falg_div_core(pf, &mips_falg_ops);
}

static void mips_float_sconv_create(pfile_t *pf)
{
  value_t conv;
  value_t exp1;
  label_t lbl_conv;

  /* cut 5: the kernel i2f reads the sign from the VALUE itself (the
   * signed source's bit pattern is already in _mips_fconv), so the
   * old negate-plus-flag dance is gone; keep the CONV fall-through
   * label order and clear the vestigial flag cell */
  conv = mips_fconv_get(pf, 4);
  exp1 = mips_float_value_get(pf, "_mips_fexp1",
      VARIABLE_DEF_TYPE_INTEGER, 1);
  lbl_conv = mips_float_label(pf, MIPS_LABEL_FLOAT_CONV, BOOLEAN_TRUE);
  label_release(lbl_conv);
  mips_store_value(pf, exp1, MIPS_REG_ZERO);
  value_release(exp1);
  value_release(conv);
  /* fall through to CONV */
}

static void mips_float_conv_create(pfile_t *pf)
{
  value_t f1;
  value_t conv;

  /* cut 5: ESP-canonical i2f through the float_alg kernel (pre-fix:
   * shift-loop normalize, truncation, no saturation story on the
   * toint side) */
  f1   = mips_fval1_get(pf);
  conv = mips_fconv_get(pf, 4);
  mips_load_value(pf, conv, MFALG_OP1);
  value_release(conv);
  value_release(f1);
  falg_i2f_core(pf, &mips_falg_ops);
}

/* ================================================================
 * TOINT  (float -> integer in _mips_fconv)
 * ================================================================ */

static void mips_float_toint_create(pfile_t *pf)
{
  value_t f1;

  /* cut 5: ESP-canonical f2i (truncating, saturating at
   * INT_MIN/INT_MAX, NaN -> 0) through the float_alg kernel with the
   * conv-ret table (exits store _mips_fconv) */
  f1 = mips_fval1_get(pf);
  mips_load_value(pf, f1, MFALG_OP1);
  value_release(f1);
  falg_f2i_core(pf, &mips_falg_conv_ops);
  mfalg_ret_conv(pf);
}

static const struct {
  const char *tag;
  void (*fn)(pfile_t *);
} mips_float_intrinsics[] = {
  {MIPS_LABEL_FLOAT_SUB,      mips_float_sub_create},
  {MIPS_LABEL_FLOAT_ADD,      mips_float_add_create},
  {MIPS_LABEL_FLOAT_MULTIPLY, mips_float_multiply_create},
  {MIPS_LABEL_FLOAT_DIVIDE,   mips_float_divide_create},
  {MIPS_LABEL_FLOAT_SCONV,    mips_float_sconv_create},
  {MIPS_LABEL_FLOAT_CONV,     mips_float_conv_create},
  {MIPS_LABEL_FLOAT_TOINT,    mips_float_toint_create}
};

boolean_t mips_float_intrinsics_needed(pfile_t *pf)
{
  size_t  ii;
  label_t lbl;

  for (ii = 0; ii < sizeof(mips_float_intrinsics) / sizeof(mips_float_intrinsics[0]);
       ii++) {
    lbl = pfile_label_find(pf, PFILE_LOG_NONE, mips_float_intrinsics[ii].tag);
    if (lbl) {
      label_release(lbl);
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

void mips_float_intrinsics_create(pfile_t *pf)
{
  size_t ii;

  for (ii = 0; ii < sizeof(mips_float_intrinsics) / sizeof(mips_float_intrinsics[0]);
       ii++) {
    label_t lbl;

    lbl = pfile_label_find(pf, PFILE_LOG_NONE, mips_float_intrinsics[ii].tag);
    if (lbl) {
      mips_float_label_here(lbl);
      label_release(lbl);
      mips_float_intrinsics[ii].fn(pf);
    }
  }
}

boolean_t mips_op_is_float(value_t dst, value_t v1, value_t v2)
{
  if (dst && value_is_float(dst)) {
    return BOOLEAN_TRUE;
  }
  if (v1 && value_is_float(v1)) {
    return BOOLEAN_TRUE;
  }
  if (v2 && value_is_float(v2)) {
    return BOOLEAN_TRUE;
  }
  return BOOLEAN_FALSE;
}

static void mips_float_call(pfile_t *pf, const char *name)
{
  label_t lbl;

  lbl = mips_float_label(pf, name, BOOLEAN_TRUE);
  (void) mips_emit_jal(lbl);
  label_release(lbl);
}

boolean_t mips_fpu_hw_enabled(pfile_t *pf)
{
  label_t lbl;

  lbl = pfile_label_find(pf, PFILE_LOG_NONE, MIPS_FPU_HW_LABEL);
  if (lbl) {
    label_release(lbl);
    return BOOLEAN_TRUE;
  }
  return BOOLEAN_FALSE;
}

void mips_fpu_enable_cu1(pfile_t *pf)
{
  if (!mips_fpu_hw_enabled(pf)) {
    return;
  }
  (void) mips_code_r_alloc(MIPS_OP_MFC0, MIPS_CP0_STATUS, 0, MIPS_REG_T0, 0);
  (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T1, 0,
      (int) ((MIPS_ST_CU1 >> 16) & 0xFFFF));
  (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_T0, MIPS_REG_T0,
      MIPS_REG_T1, 0);
  (void) mips_code_r_alloc(MIPS_OP_MTC0, MIPS_CP0_STATUS, 0, MIPS_REG_T0, 0);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
}

static void mips_fpu_mtc1(unsigned fpr, unsigned gpr)
{
  (void) mips_code_r_alloc(MIPS_OP_MTC1, fpr, 0, gpr, 0);
}

static void mips_fpu_mfc1(unsigned gpr, unsigned fpr)
{
  (void) mips_code_r_alloc(MIPS_OP_MFC1, fpr, 0, gpr, 0);
}

static void mips_fpu_val_to_fpr(pfile_t *pf, value_t val, unsigned fpr,
    unsigned gpr)
{
  if (value_is_float(val)) {
    mips_load_value(pf, val, gpr);
    mips_fpu_mtc1(fpr, gpr);
  } else {
    mips_load_value(pf, val, gpr);
    mips_fpu_mtc1(fpr, gpr);
    (void) mips_code_r_alloc(MIPS_OP_CVT_S_W, fpr, fpr, 0, 0);
  }
}

void mips_float_assign_to(pfile_t *pf, value_t dst, value_t src)
{
  value_t conv;
  variable_sz_t sz;
  const char *which;

  if (!dst || !src) {
    return;
  }
  if (mips_fpu_hw_enabled(pf) && !value_is_float(src)
      && !(value_is_const(src) && !value_is_float(src))) {
    mips_load_value(pf, src, MIPS_REG_T1);
    mips_fpu_mtc1(MIPS_FPR0, MIPS_REG_T1);
    (void) mips_code_r_alloc(MIPS_OP_CVT_S_W, MIPS_FPR0, MIPS_FPR0, 0, 0);
    mips_fpu_mfc1(MIPS_REG_T1, MIPS_FPR0);
    mips_store_value(pf, dst, MIPS_REG_T1);
    return;
  }
  if (value_is_const(src) && !value_is_float(src)) {
    value_t tmp;
    float   f;

    if (value_is_signed(src)) {
      f = (float) (long) value_const_get(src);
    } else {
      f = (float) value_const_get(src);
    }
    tmp = pfile_constant_float_get(pf, f, value_def_get(dst));
    mips_load_value(pf, tmp, MIPS_REG_T1);
    mips_store_value(pf, dst, MIPS_REG_T1);
    value_release(tmp);
    return;
  }
  if (value_is_float(src)) {
    mips_load_value(pf, src, MIPS_REG_T1);
    mips_store_value(pf, dst, MIPS_REG_T1);
    return;
  }

  sz = value_byte_sz_get(src);
  conv = mips_fconv_get(pf, sz);
  mips_load_value(pf, src, MIPS_REG_T1);
  mips_store_value(pf, conv, MIPS_REG_T1);
  /* clear sign flag */
  {
    value_t exp1;

    exp1 = mips_float_value_get(pf, "_mips_fexp1",
        VARIABLE_DEF_TYPE_INTEGER, 1);
    mips_store_value(pf, exp1, MIPS_REG_ZERO);
    value_release(exp1);
  }
  which = value_is_signed(src) ? MIPS_LABEL_FLOAT_SCONV
                               : MIPS_LABEL_FLOAT_CONV;
  mips_float_call(pf, which);
  {
    value_t f1;

    f1 = mips_fval1_get(pf);
    mips_load_value(pf, f1, MIPS_REG_T1);
    mips_store_value(pf, dst, MIPS_REG_T1);
    value_release(f1);
  }
  value_release(conv);
}

void mips_float_assign_from(pfile_t *pf, value_t dst, value_t src)
{
  value_t f1;
  value_t conv;

  if (!dst || !src) {
    return;
  }
  if (mips_fpu_hw_enabled(pf)) {
    mips_load_value(pf, src, MIPS_REG_T1);
    mips_fpu_mtc1(MIPS_FPR0, MIPS_REG_T1);
    (void) mips_code_r_alloc(MIPS_OP_CVT_W_S, MIPS_FPR0, MIPS_FPR0, 0, 0);
    mips_fpu_mfc1(MIPS_REG_T1, MIPS_FPR0);
    mips_store_value(pf, dst, MIPS_REG_T1);
    return;
  }
  f1 = mips_fval1_get(pf);
  mips_load_value(pf, src, MIPS_REG_T1);
  mips_store_value(pf, f1, MIPS_REG_T1);
  mips_float_call(pf, MIPS_LABEL_FLOAT_TOINT);
  conv = mips_fconv_get(pf, value_byte_sz_get(dst));
  mips_load_value(pf, conv, MIPS_REG_T1);
  mips_store_value(pf, dst, MIPS_REG_T1);
  value_release(conv);
  value_release(f1);
}

void mips_float_binop(pfile_t *pf, operator_t op, value_t dst,
    value_t v1, value_t v2)
{
  value_t     f1;
  value_t     f2;
  const char *which;

  if (!v1 || !v2) {
    return;
  }
  if (mips_fpu_hw_enabled(pf)) {
    mips_opcode_t cop;

    cop = MIPS_OP_ADD_S;
    if (OPERATOR_SUB == op) {
      cop = MIPS_OP_SUB_S;
    } else if (OPERATOR_MUL == op) {
      cop = MIPS_OP_MUL_S;
    } else if (OPERATOR_DIV == op) {
      cop = MIPS_OP_DIV_S;
    } else if (OPERATOR_ADD != op) {
      return;
    }
    mips_fpu_val_to_fpr(pf, v1, MIPS_FPR0, MIPS_REG_T1);
    mips_fpu_val_to_fpr(pf, v2, MIPS_FPR1, MIPS_REG_T2);
    (void) mips_code_r_alloc(cop, MIPS_FPR0, MIPS_FPR0, MIPS_FPR1, 0);
    if (dst) {
      mips_fpu_mfc1(MIPS_REG_T1, MIPS_FPR0);
      mips_store_value(pf, dst, MIPS_REG_T1);
    }
    return;
  }
  f1 = mips_fval1_get(pf);
  f2 = mips_fval2_get(pf);

  /* Convert v2 first so assign_to (which writes fval1) cannot clobber v1. */
  if (value_is_float(v2)) {
    mips_load_value(pf, v2, MIPS_REG_T1);
    mips_store_value(pf, f2, MIPS_REG_T1);
  } else {
    mips_float_assign_to(pf, f2, v2);
  }
  if (value_is_float(v1)) {
    mips_load_value(pf, v1, MIPS_REG_T1);
    mips_store_value(pf, f1, MIPS_REG_T1);
  } else {
    mips_float_assign_to(pf, f1, v1);
  }

  switch (op) {
    case OPERATOR_ADD: which = MIPS_LABEL_FLOAT_ADD; break;
    case OPERATOR_SUB: which = MIPS_LABEL_FLOAT_SUB; break;
    case OPERATOR_MUL: which = MIPS_LABEL_FLOAT_MULTIPLY; break;
    case OPERATOR_DIV: which = MIPS_LABEL_FLOAT_DIVIDE; break;
    default:
      value_release(f2);
      value_release(f1);
      return;
  }
  mips_float_call(pf, which);
  if (dst) {
    mips_load_value(pf, f1, MIPS_REG_T1);
    mips_store_value(pf, dst, MIPS_REG_T1);
  }
  value_release(f2);
  value_release(f1);
}

void mips_float_neg(pfile_t *pf, value_t dst, value_t src)
{
  if (!dst || !src) {
    return;
  }
  mips_load_value(pf, src, MIPS_REG_T1);
  mips_load_sign_mask(MIPS_REG_T2);
  (void) mips_code_r_alloc(MIPS_OP_XOR, MIPS_REG_T1, MIPS_REG_T1,
      MIPS_REG_T2, 0);
  mips_store_value(pf, dst, MIPS_REG_T1);
}

/*
 * Compare via subtract; inspect sign/exp of result (PIC-style).
 * Leaves boolean 0/1 in $t1.
 */
static void mips_float_cmp_result(pfile_t *pf, operator_t op,
    value_t v1, value_t v2)
{
  if (mips_fpu_hw_enabled(pf)) {
    mips_opcode_t cop;
    mips_opcode_t bop;
    label_t      l_fpu;

    /* t1=0; c.cond; bc1F/T done; addiu t1,1; done:
     * BC1F skips the set-1 when the cond is false. */
    cop = MIPS_OP_C_EQ_S;
    bop = MIPS_OP_BC1F;
    if ((OPERATOR_EQ == op) || (OPERATOR_NE == op)) {
      cop = MIPS_OP_C_EQ_S;
      bop = (OPERATOR_EQ == op) ? MIPS_OP_BC1F : MIPS_OP_BC1T;
    } else if ((OPERATOR_LT == op) || (OPERATOR_GE == op)) {
      cop = MIPS_OP_C_LT_S;
      bop = (OPERATOR_LT == op) ? MIPS_OP_BC1F : MIPS_OP_BC1T;
    } else if ((OPERATOR_LE == op) || (OPERATOR_GT == op)) {
      cop = MIPS_OP_C_LE_S;
      bop = (OPERATOR_LE == op) ? MIPS_OP_BC1F : MIPS_OP_BC1T;
    }
    mips_fpu_val_to_fpr(pf, v1, MIPS_FPR0, MIPS_REG_T1);
    mips_fpu_val_to_fpr(pf, v2, MIPS_FPR1, MIPS_REG_T2);
    (void) mips_code_r_alloc(cop, 0, MIPS_FPR0, MIPS_FPR1, 0);
    mips_move(MIPS_REG_T1, MIPS_REG_ZERO);
    l_fpu = mips_anon_label(pf);
    (void) mips_emit_cond_branch(bop, 0, 0, l_fpu);
    (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1, MIPS_REG_ZERO, 1);
    mips_float_label_here(l_fpu);
    label_release(l_fpu);
    return;
  }

  /* ESP-style relation (float_alg cut 4 companion): NaN guard per
   * operand (unordered: NE true, everything else false), both-zeros
   * guard (mixed +-0 compares equal), then an UNSIGNED compare of
   * order-preserving sort keys: positives -> bits | 0x80000000,
   * negatives -> ~bits.  The old subtract-and-inspect path misread
   * denormal differences as zero and NaN relations arbitrarily. */
  {
    label_t l_nan  = mips_anon_label(pf);
    label_t l_zeros = mips_anon_label(pf);
    label_t l_keys = mips_anon_label(pf);
    label_t l_done = mips_anon_label(pf);
    label_t l_true = mips_anon_label(pf);
    unsigned regs[2];
    int      ii;

    /* operand bits: $t8 = v1, $t9 = v2 (outside the kernel helper
     * map; the caller holds nothing live in them).  nofill: both
     * operands stay staged in $t8/$t9 until the key compare consumes
     * them, so the home-fill copy must not run here -- with the fill,
     * loading v2 fills its register home and silently overwrites the
     * v1 operand when that home is $t8 (pi > radius compared pi with
     * pi exactly this way). */
    mips_load_value_nofill(pf, v1, MIPS_REG_T8);
    mips_load_value_nofill(pf, v2, MIPS_REG_T9);

    /* NaN guard: exp==0xFF && mant!=0, per operand -> unordered */
    regs[0] = MIPS_REG_T8;
    regs[1] = MIPS_REG_T9;
    for (ii = 0; ii < 2; ii++) {
      unsigned r = regs[ii];

      (void) mips_code_r_alloc(MIPS_OP_SRL, MIPS_REG_T2, 0, r, 23);
      (void) mips_code_alloc(MIPS_OP_ANDI, MIPS_REG_T2,
          MIPS_REG_T2, 0xFF);
      (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T3,
          MIPS_REG_ZERO, 0xFF);
      (void) mips_code_r_alloc(MIPS_OP_SLTU, MIPS_REG_T4,
          MIPS_REG_T2, MIPS_REG_T3, 0);
      /* t4 = (exp <u 0xFF): clean -- skip the mantissa test */
      {
        label_t l_ok = mips_anon_label(pf);

        mips_bnez(MIPS_REG_T4, l_ok);
        (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T3, 0, 0x007F);
        (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T3, MIPS_REG_T3,
            0xFFFF);
        (void) mips_code_r_alloc(MIPS_OP_AND, MIPS_REG_T4, r,
            MIPS_REG_T3, 0);
        mips_bnez(MIPS_REG_T4, l_nan);      /* mant != 0: NaN */
        mips_float_label_here(l_ok);
        label_release(l_ok);
      }
    }
    /* clean operands: skip the unordered result */
    (void) mips_emit_j(l_zeros);
    mips_float_label_here(l_nan);
    /* unordered: result = (op == NE) */
    if (OPERATOR_NE == op) {
      (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1,
          MIPS_REG_ZERO, 1);
    } else {
      mips_move(MIPS_REG_T1, MIPS_REG_ZERO);
    }
    (void) mips_emit_j(l_done);

    /* both zeros (either sign)?  (bits << 1) == 0 for both */
    mips_float_label_here(l_zeros);
    (void) mips_code_r_alloc(MIPS_OP_SLL, MIPS_REG_T2, 0,
        MIPS_REG_T8, 1);
    (void) mips_code_r_alloc(MIPS_OP_SLL, MIPS_REG_T3, 0,
        MIPS_REG_T9, 1);
    (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_T2, MIPS_REG_T2,
        MIPS_REG_T3, 0);
    mips_bnez(MIPS_REG_T2, l_keys);
    /* mixed +-0: equal -- EQ/LE/GE true, LT/GT/NE false */
    mips_move(MIPS_REG_T1, MIPS_REG_ZERO);
    if ((OPERATOR_EQ == op) || (OPERATOR_LE == op)
        || (OPERATOR_GE == op)) {
      (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1,
          MIPS_REG_ZERO, 1);
    }
    (void) mips_emit_j(l_done);

    /* sort keys: key = bits ^ ((bits >>a 31) | 0x80000000) --
     * positives -> bits | 0x80000000 (upper half), negatives ->
     * ~bits (lower half); unsigned key order = float order */
    mips_float_label_here(l_keys);
    for (ii = 0; ii < 2; ii++) {
      unsigned r = regs[ii];
      unsigned k = regs[ii];

      (void) mips_code_r_alloc(MIPS_OP_SRA, MIPS_REG_T2, 0, r, 31);
      (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T3, 0, 0x8000);
      (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_T2, MIPS_REG_T2,
          MIPS_REG_T3, 0);
      (void) mips_code_r_alloc(MIPS_OP_XOR, k, r, MIPS_REG_T2, 0);
    }
    /* $t4 = k1 <u k2;  $t5 = k2 <u k1 */
    (void) mips_code_r_alloc(MIPS_OP_SLTU, MIPS_REG_T4, MIPS_REG_T8,
        MIPS_REG_T9, 0);
    (void) mips_code_r_alloc(MIPS_OP_SLTU, MIPS_REG_T5, MIPS_REG_T9,
        MIPS_REG_T8, 0);
    mips_move(MIPS_REG_T1, MIPS_REG_ZERO);
    switch (op) {
      case OPERATOR_LT:
        mips_beqz(MIPS_REG_T4, l_done);
        (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1,
            MIPS_REG_ZERO, 1);
        break;
      case OPERATOR_LE:
        mips_bnez(MIPS_REG_T5, l_done);
        (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1,
            MIPS_REG_ZERO, 1);
        break;
      case OPERATOR_GT:
        mips_beqz(MIPS_REG_T5, l_done);
        (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1,
            MIPS_REG_ZERO, 1);
        break;
      case OPERATOR_GE:
        mips_bnez(MIPS_REG_T4, l_done);
        (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1,
            MIPS_REG_ZERO, 1);
        break;
      case OPERATOR_EQ:
        mips_bnez(MIPS_REG_T4, l_done);
        mips_bnez(MIPS_REG_T5, l_done);
        (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1,
            MIPS_REG_ZERO, 1);
        break;
      default: /* NE */
        mips_bnez(MIPS_REG_T4, l_true);
        mips_bnez(MIPS_REG_T5, l_true);
        (void) mips_emit_j(l_done);
        mips_float_label_here(l_true);
        (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1,
            MIPS_REG_ZERO, 1);
        break;
    }
    mips_float_label_here(l_done);
    label_release(l_nan);
    label_release(l_zeros);
    label_release(l_keys);
    label_release(l_done);
    label_release(l_true);
  }
}

boolean_t mips_float_compare_branch(pfile_t *pf, const cmd_t cmd,
    operator_t op, value_t v1, value_t v2)
{
  cmd_t           next;
  cmd_branchcond_t bcond;
  label_t         lbl;
  boolean_t       br_true;
  operator_t      use_op;

  next = cmd_next_exec_get(cmd_link_get(cmd));
  if (!next || (CMD_TYPE_BRANCH != cmd_type_get(next))) {
    return BOOLEAN_FALSE;
  }
  if (CMD_BRANCHTYPE_GOTO != cmd_brtype_get(next)) {
    return BOOLEAN_FALSE;
  }
  if (CMD_BRANCHCOND_NONE == cmd_brcond_get(next)) {
    return BOOLEAN_FALSE;
  }
  bcond = cmd_brcond_get(next);
  lbl = cmd_brdst_get(next);
  mips_op_consume_branch(pf, next);
  br_true = (CMD_BRANCHCOND_TRUE == bcond);

  use_op = op;
  /* Prefer only LT/LE/EQ/NE via swap for GT/GE */
  if ((OPERATOR_GT == op) || (OPERATOR_GE == op)) {
    value_t tmp;

    tmp = v1;
    v1 = v2;
    v2 = tmp;
    use_op = (OPERATOR_GT == op) ? OPERATOR_LT : OPERATOR_LE;
  }

  mips_float_cmp_result(pf, use_op, v1, v2);
  (void) mips_emit_cond_branch(br_true ? MIPS_OP_BNE : MIPS_OP_BEQ,
      MIPS_REG_T1, MIPS_REG_ZERO, lbl);
  return BOOLEAN_TRUE;
}

void mips_float_compare_bool(pfile_t *pf, value_t dst, operator_t op,
    value_t v1, value_t v2)
{
  operator_t use_op;

  use_op = op;
  if ((OPERATOR_GT == op) || (OPERATOR_GE == op)) {
    value_t tmp;

    tmp = v1;
    v1 = v2;
    v2 = tmp;
    use_op = (OPERATOR_GT == op) ? OPERATOR_LT : OPERATOR_LE;
  }
  mips_float_cmp_result(pf, use_op, v1, v2);
  mips_store_value(pf, dst, MIPS_REG_T1);
}
