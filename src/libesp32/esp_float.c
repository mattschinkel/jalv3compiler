/************************************************************
 **
 ** esp_float.c : ESP32 soft-float (3i)
 **
 ** IEEE754 single-precision helpers, register-only (a2-a11).
 ** a2 = operand1/result, a3 = operand2.  Returns via jx a0.
 **
 ** Semantics: round-to-nearest-even everywhere; NaNs dominate and
 ** propagate quieted; +-inf and +-0 follow the IEEE tables; denormal
 ** inputs are normalized on unpack; results underflow gradually
 ** through the denormals and overflow to inf.  float->int saturates
 ** (NaN -> 0, +-inf and |x| >= 2^31 -> INT_MAX/INT_MIN).
 ** Known limitation: _esp_i2f truncates for |int| >= 2^24.
 **
 ** Register contract inside a core:
 **   a2/a3 operands, a4 exponent (biased, signed), a5 significand
 **   packed S<<2 | G<<1 | R, a6/a7 operand temps + scratch,
 **   a8/a9 scratch, a10 sign (0/1), a11 sticky (0/nonzero).
 **   a0 is the return address (jx a0) and is never touched.
 **
 ** ISA constraints observed:
 **   EXTUI shift field: 4 bits (0-15) — larger shifts need SSR+SRL
 **   EXTUI mask field:  4 bits (1-16)
 **   SAR (SSL/SSR/SLL/SRL/SRA) uses the low 5 bits of the SAR reg
 **   Branches: as = left operand, at = right operand; alloc(op,
 **   at, as) branches when as <op> at
 **   Skip labels: placed AFTER the code to skip
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include "../libcore/pfile.h"
#include "../libcore/value.h"
#include "esp.h"

#define R3(op, rd, rs, rt) (void) esp_code_alloc(op, rd, rs, (int)(rt))
#define MI(at, v)          (void) esp_code_alloc(ESP_OP_MOVI, at, 0, (int)(v))
#define LBL()              esp_code_label_alloc()
#define RTN()              (void) esp_code_alloc(ESP_OP_JX, 0, ESP_REG_A0, 0)

/* float bit constants (pool literals via PL) */
#define FP_SIGN   0x80000000UL   /* sign bit */
#define FP_INF    0x7F800000UL   /* +inf bits */
#define FP_QNAN   0x7FC00000UL   /* canonical quiet NaN */
#define FP_QPROP  0x00400000UL   /* quiet bit forced on propagated NaNs */
#define FP_MANT   0x007FFFFFUL   /* mantissa field */
#define FP_IMPL   0x00800000UL   /* implicit leading significand bit */
#define FP_EXPMAX 0xFF000000UL   /* (x<<1) > this  =>  inf/NaN */

/* load a pool literal into reg */
static void PL(unsigned reg, unsigned long val)
{
  esp_emit_l32r(reg, esp_pool_lit(val));
}

/* extui rd, rs, sh (width w encodes w-1; sh <= 15, w <= 16) */
static void EXT(unsigned rd, unsigned rs, unsigned sh, unsigned w)
{
  esp_code_t e = esp_code_alloc(ESP_OP_EXTUI, rd, rs, (int) sh);

  if (e) { e->src2 = w - 1U; }
}

/* shift right/left by the count in sar (low 5 bits) */
static void SSR_SHIFT(unsigned rd, unsigned rs, unsigned sar)
{
  (void) esp_code_alloc(ESP_OP_SSR, 0, sar, 0);
  (void) esp_code_alloc(ESP_OP_SRL, rd, rs, 0);
}

static void SSL_SHIFT(unsigned rd, unsigned rs, unsigned sar)
{
  (void) esp_code_alloc(ESP_OP_SSL, 0, sar, 0);
  (void) esp_code_alloc(ESP_OP_SLL, rd, rs, 0);
}

/* branch: alloc(op, at, as), taken when as <op> at */
static esp_code_t BR(esp_opcode_t op, unsigned at, unsigned as)
{
  return esp_code_alloc(op, at, as, 0);
}

/* bind one/two pending forward branches to a label placed here */
static void BIND(esp_code_t br)
{
  esp_code_t l = LBL();

  if (br) { br->target = l; }
}

static void BIND2(esp_code_t b1, esp_code_t b2)
{
  esp_code_t l = LBL();

  if (b1) { b1->target = l; }
  if (b2) { b2->target = l; }
}

static void BIND4(esp_code_t b1, esp_code_t b2, esp_code_t b3,
    esp_code_t b4)
{
  esp_code_t l = LBL();

  if (b1) { b1->target = l; }
  if (b2) { b2->target = l; }
  if (b3) { b3->target = l; }
  if (b4) { b4->target = l; }
}

/* unconditional forward jump (BEQZ on a zeroed scratch; the
 * relaxation pass widens it if the skip grows past BRI8) */
static esp_code_t UJMP(unsigned scratch)
{
  MI(scratch, 0);
  return esp_code_alloc(ESP_OP_BEQZ, 0, scratch, 0);
}

/* ---- cut 1: the unpack/pack sub-helpers live in libcore/float_alg.c
 * (verbatim algorithm, ops-table machine primitives).  The ESP32 ops
 * table below maps each primitive to exactly the instruction the
 * local macros emitted pre-extraction -- byte-identity-checked. */
#include "../libcore/float_alg.h"

static void falg_movi(pfile_t *pf, int rd, long imm)
{
  (void) pf;
  (void) esp_code_alloc(ESP_OP_MOVI, (unsigned) rd, 0, (int) imm);
}

static void falg_load_const(pfile_t *pf, int rd, unsigned long val)
{
  (void) pf;
  PL((unsigned) rd, val);
}

static void falg_alu(pfile_t *pf, falg_alu_t op, int rd, int rs, int rt)
{
  esp_opcode_t eop;

  (void) pf;
  switch (op) {
    case FALG_ALU_ADD: eop = ESP_OP_ADD; break;
    case FALG_ALU_SUB: eop = ESP_OP_SUB; break;
    case FALG_ALU_AND: eop = ESP_OP_AND; break;
    case FALG_ALU_OR:  eop = ESP_OP_OR;  break;
    default:           eop = ESP_OP_XOR; break;
  }
  R3(eop, (unsigned) rd, (unsigned) rs, (unsigned) rt);
}

static void falg_addi(pfile_t *pf, int rd, int rs, long imm)
{
  (void) pf;
  (void) esp_code_alloc(ESP_OP_ADDI, (unsigned) rd, (unsigned) rs,
      (int) imm);
}

static void falg_sll_var(pfile_t *pf, int rd, int rs, int sar)
{
  (void) pf;
  SSL_SHIFT((unsigned) rd, (unsigned) rs, (unsigned) sar);
}

static void falg_srl_var(pfile_t *pf, int rd, int rs, int sar)
{
  (void) pf;
  SSR_SHIFT((unsigned) rd, (unsigned) rs, (unsigned) sar);
}

static void falg_ext(pfile_t *pf, int rd, int rs, int sh, int w)
{
  (void) pf;
  EXT((unsigned) rd, (unsigned) rs, (unsigned) sh, (unsigned) w);
}

static void falg_clz(pfile_t *pf, int rd, int rs)
{
  (void) pf;
  (void) esp_code_alloc(ESP_OP_NSAU, (unsigned) rd, (unsigned) rs, 0);
}

static falg_brh_t falg_br(pfile_t *pf, falg_br_t cond, int at, int as)
{
  esp_opcode_t eop;

  (void) pf;
  switch (cond) {
    case FALG_BR_EQZ: eop = ESP_OP_BEQZ; break;
    case FALG_BR_NEZ: eop = ESP_OP_BNEZ; break;
    case FALG_BR_GEU: eop = ESP_OP_BGEU; break;
    case FALG_BR_LTU: eop = ESP_OP_BLTU; break;
    case FALG_BR_LT:  eop = ESP_OP_BLT;  break;
    case FALG_BR_NE:  eop = ESP_OP_BNE;  break;
    default:          eop = ESP_OP_BGE;  break;
  }
  return BR(eop, (unsigned) at, (unsigned) as);
}

static void falg_bind1(pfile_t *pf, falg_brh_t b)
{
  (void) pf;
  BIND((esp_code_t) b);
}

static void falg_bind2(pfile_t *pf, falg_brh_t b1, falg_brh_t b2)
{
  (void) pf;
  BIND2((esp_code_t) b1, (esp_code_t) b2);
}

static void falg_bind4(pfile_t *pf, falg_brh_t b1, falg_brh_t b2,
    falg_brh_t b3, falg_brh_t b4)
{
  (void) pf;
  BIND4((esp_code_t) b1, (esp_code_t) b2, (esp_code_t) b3,
      (esp_code_t) b4);
}

static void falg_neg(pfile_t *pf, int rd)
{
  (void) pf;
  (void) esp_code_alloc(ESP_OP_NEG, (unsigned) rd, (unsigned) rd, 0);
}

static void falg_mul_lo(pfile_t *pf, int rd, int rs, int rt)
{
  (void) pf;
  R3(ESP_OP_MULL, (unsigned) rd, (unsigned) rs, (unsigned) rt);
}

static void falg_mul_hi(pfile_t *pf, int rd, int rs, int rt)
{
  (void) pf;
  R3(ESP_OP_MULUH, (unsigned) rd, (unsigned) rs, (unsigned) rt);
}

static void falg_ssr_set(pfile_t *pf, int sar)
{
  (void) pf;
  (void) esp_code_alloc(ESP_OP_SSR, 0, (unsigned) sar, 0);
}

static void falg_srl_sar(pfile_t *pf, int rd, int rs, int sar)
{
  (void) pf;
  (void) sar;
  (void) esp_code_alloc(ESP_OP_SRL, (unsigned) rd, (unsigned) rs, 0);
}

static falg_brh_t falg_label_here(pfile_t *pf)
{
  (void) pf;
  return LBL();
}

static void falg_br_back(pfile_t *pf, falg_br_t cond, int at, int as,
    falg_brh_t target)
{
  esp_code_t brn;

  (void) pf;
  brn = falg_br(pf, cond, at, as);
  if (brn) {
    brn->target = (esp_code_t) target;
  }
}

static falg_brh_t falg_ujmp(pfile_t *pf, int scratch)
{
  (void) pf;
  return UJMP((unsigned) scratch);
}

static void falg_ret(pfile_t *pf)
{
  (void) pf;
  RTN();
}

static const falg_ops_t esp_falg_ops = {
  falg_movi,
  falg_load_const,
  falg_alu,
  falg_addi,
  falg_sll_var,
  falg_srl_var,
  falg_ext,
  falg_clz,
  falg_br,
  falg_bind1,
  falg_bind2,
  falg_bind4,
  falg_mul_lo,
  falg_mul_hi,
  falg_neg,
  falg_ssr_set,
  falg_srl_sar,
  falg_label_here,
  falg_br_back,
  falg_ujmp,
  falg_ret
};

/* ---- alignment tail (both mantissas on the S<<2 grid) ----
 * in: a4 = larger exp, a5 = A, a7 = B, a8 = d (already absolute,
 * clamped here to 32), a10 = result sign, a11 = 0.
 * out: a5 = S<<2|G<<1|R, a11 sticky. */
static void emit_addsub_core(boolean_t is_sub)
{
  falg_addsub_core(0, &esp_falg_ops, is_sub);
}

static void emit_add_core(void)
{
  emit_addsub_core(BOOLEAN_FALSE);
}

/* ---- SUB: a2 = a2 - a3 ---- */
static void emit_sub_body(void)
{
  emit_addsub_core(BOOLEAN_TRUE);
}

/* ---- MUL: sign = s1^s2, exp = e1+e2-127; G,R,S cut straight
 * from the 48-bit product ---- */
static void emit_mul_core(void)
{
  falg_mul_core(0, &esp_falg_ops);
}

/* ---- DIV: sign = s1^s2, exp = e1-e2+127; 49 restoring iterations
 * produce Q = floor(N * 2^25 / D) = S<<2|G<<1|R, remainder gives
 * the sticky ---- */
static void emit_div_core(void)
{
  falg_div_core(0, &esp_falg_ops);
}

static void emit_i2f_core(void)
{
  falg_i2f_core(0, &esp_falg_ops);
}

/* ---- float -> int (F2I): a2 (float) -> a2 (int, truncating,
 * saturating at INT_MIN/INT_MAX; NaN -> 0) ---- */
static void emit_f2i_core(void)
{
  falg_f2i_core(0, &esp_falg_ops);
}

/* ---- registry + dispatch ---- */
static struct {
  const char *name;
  boolean_t   used;
} esp_fh[6] = {
  {"_esp_fadd", BOOLEAN_FALSE},
  {"_esp_fsub", BOOLEAN_FALSE},
  {"_esp_fmul", BOOLEAN_FALSE},
  {"_esp_fdiv", BOOLEAN_FALSE},
  {"_esp_f2i",  BOOLEAN_FALSE},
  {"_esp_i2f",  BOOLEAN_FALSE}
};

void esp_float_reset(void)
{
  unsigned i;
  for (i = 0; i < 6; i++) {
    esp_fh[i].used = BOOLEAN_FALSE;
  }
}

static label_t esp_fh_lbl(pfile_t *pf, unsigned idx)
{
  label_t lbl = pfile_label_find(pf, PFILE_LOG_NONE, esp_fh[idx].name);
  if (!lbl) {
    lbl = pfile_label_alloc(pf, esp_fh[idx].name);
  }
  return lbl;
}

void esp_float_intrinsics_create(pfile_t *pf)
{
  unsigned i;

  for (i = 0; i < 6; i++) {
    if (!esp_fh[i].used) {
      continue;
    }
    {
      label_t    lbl  = esp_fh_lbl(pf, i);
      esp_code_t node = (esp_code_t) label_code_get(lbl);

      if (node) {
        continue;
      }
      node = LBL();
      if (lbl) {
        label_code_set(lbl, node);
      }
      /* bind pending call literals (same pfile label objects) */
      esp_op_helper_bind(lbl, node);
      switch (i) {
        case 0:  emit_add_core();  RTN(); break;
        case 1:  emit_sub_body();  RTN(); break;
        case 2:  emit_mul_core();  RTN(); break;
        case 3:  emit_div_core();               break; /* RTN inside */
        case 4:  emit_f2i_core();  RTN(); break;
        case 5:  emit_i2f_core();               break; /* RTN inside */
      }
      label_release(lbl);      /* list keeps the label alive */
    }
  }
}

boolean_t esp_float_intrinsics_needed(pfile_t *pf)
{
  unsigned i;
  UNUSED(pf);
  for (i = 0; i < 6; i++) {
    if (esp_fh[i].used) {
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

void esp_float_call(pfile_t *pf, unsigned idx)
{
  label_t    lbl = esp_fh_lbl(pf, idx);
  esp_code_t lit = esp_pool_lit_new();

  esp_fh[idx].used = BOOLEAN_TRUE;
  esp_pend_label_lit(pf, lbl, lit);
  if (lit) {
    esp_emit_l32r(ESP_REG_A0, lit);
  }
  (void) esp_code_alloc(ESP_OP_CALLX0, 0, ESP_REG_A0, 0);
  label_release(lbl);      /* list keeps the label alive */
}
