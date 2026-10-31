/************************************************************
 **
 ** mips.h : PIC32 (MIPS32) back-end internal IR + code list
 **
 ** mips_code_t is a singly-linked list of MIPS instructions (one node
 ** per emitted instruction). Labels (MIPS_OP_LABEL) take no space.
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#ifndef _mips_h__
#define _mips_h__

#include "../libcore/pfile.h"
#include "../libcore/operator.h"
#include "../libcore/cmd.h"
#include "../libcore/value.h"
#include "../libcore/variable.h"
#include "../libcore/label.h"

/* MIPS32 register numbers (static-frame: params/locals in RAM).
 * Scratch: $t0 address LEA (also last in the caller-saved home pool),
 * $t1/$t2 operands/result. $t4 = multi-bit field bit index.
 * $gp = MIPS_RAM_BASE for gp-relative load/store.
 * $s3-$s7 = callee-saved home pool; soft-float keeps
 * $s0-$s2. Frame emit count is MIPS_S_HOME_EMIT (5 → $s3-$s7). */
#define MIPS_REG_ZERO  0
#define MIPS_REG_AT   1
#define MIPS_REG_V0    2
#define MIPS_REG_V1    3
#define MIPS_REG_A0    4
#define MIPS_REG_A1    5
#define MIPS_REG_A2    6
#define MIPS_REG_A3    7
#define MIPS_REG_T0    8
#define MIPS_REG_T1    9
#define MIPS_REG_T2    10
#define MIPS_REG_T3    11
#define MIPS_REG_T4    12
#define MIPS_REG_T5    13
#define MIPS_REG_T6    14
#define MIPS_REG_T7    15
#define MIPS_REG_S0    16
#define MIPS_REG_S1    17
#define MIPS_REG_S2    18
#define MIPS_REG_S3    19
#define MIPS_REG_S4    20
#define MIPS_REG_S5    21
#define MIPS_REG_S6    22
#define MIPS_REG_S7    23
#define MIPS_REG_T8    24
#define MIPS_REG_T9    25
#define MIPS_REG_K0    26
#define MIPS_REG_K1    27
#define MIPS_REG_GP    28
#define MIPS_REG_SP    29
#define MIPS_REG_RA    31

/* Callee-saved home pool: consecutive $s3..$s7. Opt matches any of these
 * in push/pop pairs; enter/leave emit only the first MIPS_S_HOME_EMIT. */
#define MIPS_S_HOME_MAX  5
#define MIPS_S_HOME_EMIT 5
#define MIPS_S_HOME_REG(i) ((unsigned) (MIPS_REG_S3 + (i)))

/* Device family: MZ EF (current support) or MX1/2 (MIPS32 M4K).
 * Detected from the device file; see mips_dev.c. */
typedef enum {
  MIPS_FAMILY_MZ_EF = 1,
  MIPS_FAMILY_MX1   = 2
} mips_family_t;

mips_family_t mips_family(pfile_t *pf);
unsigned long mips_ram_top(pfile_t *pf);
unsigned long mips_data_max(pfile_t *pf);
unsigned long mips_stack_reserve(pfile_t *pf);
unsigned long mips_code_size(pfile_t *pf);
unsigned long mips_image_end(pfile_t *pf);
unsigned long mips_ebase_page(pfile_t *pf);

/* PIC32MZ KSEG virtual addresses (architecture constants). */
#define MIPS_FLASH_BASE  0xBFC00000UL
#define MIPS_FLASH_PFM   0x9D000000UL  /* cached program flash (USB HID app) */
#define MIPS_PFM_USB_TOP 0x9D0F3FF0UL  /* MikroE HID reserved from here up */
#define MIPS_RAM_BASE   0x80000000UL
#define MIPS_STACK_TOP  0x80080000UL   /* top of 512 KB SRAM; stack grows down */
/* GPR data must stay below this offset so the stack has a 64 KB reserve. */
#define MIPS_SRAM_DATA_MAX 0x00070000UL
#define MIPS_WORD_SZ     4
/* QEMU pic32mz VEIC + Status.BEV: external IRQ vectors to 0xBFC00200+0x200. */
#define MIPS_ISR_VECTOR  0xBFC00400UL
#define MIPS_ISR_OFS     0x400UL
#define MIPS_ISR_MVEC_OFS 0x1000UL  /* MX MVEC: 64x32B table at +0x200 */
#define MIPS_MICROMIPS_LABEL "_mips_micromips"
#define MIPS_MVEC_LABEL      "_mips_mvec"
#define MIPS_LOADER_PFM_LABEL "_mips_loader_pfm"
#define MIPS_CP0_STATUS  12
#define MIPS_CP0_CONFIG  16
#define MIPS_CP0_COUNT    9
#define MIPS_CP0_COMPARE 11
#define MIPS_CP0_EBASE   15   /* select 1 */
#define MIPS_CP0_CAUSE   13   /* Cause.IV (bit 23) gates EIC delivery */
#define MIPS_CP0_INTCTL  12   /* select 1: IntCtl.VS vector spacing */
#define MIPS_CP0_ERRCTL  26
#define MIPS_CP0_TAGLO   28
#define MIPS_CAUSE_IV    0x00800000UL
#define MIPS_INTCTL_VS1  0x00000020UL
#define MIPS_CP0_SRSCTL  12   /* select 2: shadow register set control */
#define MIPS_SRSCTL_ESS  0x000003C0UL  /* ESS field (current set), bits 9:6 */
#define MIPS_ST_IE       1
#define MIPS_ST_EXL      2
#define MIPS_ST_ERL      4
#define MIPS_ST_CU1      0x20000000UL  /* Status.CU1 — COP1 usable */
/* PIC32MX1/2 interrupt controller (M4K, no OFFx/PRISS) */
#define MIPS_INTCON_MX   0xBF881000UL
#define MIPS_INTCONSET_MX 0xBF881008UL
#define MIPS_INTSTAT_MX  0xBF881010UL
#define MIPS_IFS0_MX     0xBF881030UL
#define MIPS_IFS_CT_MX   3

/* Interrupt controller (KSEG1) — see demo_lib/32mz1024efh064.jal */
#define MIPS_INTCON      0xBF810000UL
#define MIPS_INTCONSET   0xBF810008UL
#define MIPS_PRISS       0xBF810010UL
#define MIPS_INTSTAT     0xBF810020UL
#define MIPS_INTCON_MVEC 0x00001000UL
/* NOTE: MZ INTCON has NO SS0 bit (INT0-4EP/TPC/MVEC/NMIKEY only); the
 * single-vector shadow mode bit is PRISS<0>.  A Phase-3j INTCONSET
 * 0x10000 wrote a reserved bit -- removed 2026-10-17. */
#define MIPS_PRISS_ALL   0x76543210UL
/* OFF0 for IRQ 0; multi-vector OFFx base (HW). QEMU ignores spacing. */
#define MIPS_OFF0        0xBF810540UL
#define MIPS_OFF_BASE    0xBF810540UL
/* Default OFFx value under BEV: same 0x200 slot as the single EIC stub. */
#define MIPS_OFF_DEFAULT 0x00000200UL
/* Loader boot init: EBASE vector page (KSEG0) + general-exception slot */
#define MIPS_EBASE_PAGE  0x9FC01000UL
#define MIPS_EBASE_GX    0x180UL

typedef enum {
  MIPS_OP_NONE = 0,
  MIPS_OP_LUI,
  MIPS_OP_ADDIU,
  MIPS_OP_ORI,
  MIPS_OP_ANDI,
  MIPS_OP_XORI,
  MIPS_OP_LB,
  MIPS_OP_LBU,
  MIPS_OP_LH,
  MIPS_OP_LHU,
  MIPS_OP_LW,
  MIPS_OP_SB,
  MIPS_OP_SH,
  MIPS_OP_SW,
  MIPS_OP_ADDU,
  MIPS_OP_SUBU,
  MIPS_OP_AND,
  MIPS_OP_OR,
  MIPS_OP_XOR,
  MIPS_OP_SLL,     /* rd = rt << sa          (sa in imm) */
  MIPS_OP_SRL,     /* rd = rt >> sa (logical) */
  MIPS_OP_SRA,     /* rd = rt >> sa (arith)   */
  MIPS_OP_SLLV,    /* rd = rt << (rs & 31)    */
  MIPS_OP_SRLV,    /* rd = rt >> (rs & 31)    */
  MIPS_OP_SRAV,    /* rd = rt >> (rs & 31) arith */
  MIPS_OP_MULT,    /* lo = rs * rt            */
  MIPS_OP_MULTU,
  MIPS_OP_DIV,     /* lo = rs / rt, hi = rs % rt */
  MIPS_OP_DIVU,
  MIPS_OP_MFLO,    /* rd = lo                 */
  MIPS_OP_MFHI,    /* rd = hi                 */
  MIPS_OP_SLT,    /* rd = (rs < rt) signed    */
  MIPS_OP_SLTU,   /* rd = (rs < rt) unsigned  */
  MIPS_OP_BEQ,
  MIPS_OP_BNE,
  MIPS_OP_J,
  MIPS_OP_JAL,    /* call: $ra = PC+8, jump to target */
  MIPS_OP_JALR,   /* call register: $rd = PC+8, jump to rs */
  MIPS_OP_JR,     /* jump register: jump to rs       */
  MIPS_OP_MFC0,   /* rt = CP0[rd,sel]                */
  MIPS_OP_MTC0,   /* CP0[rd,sel] = rt                */
  MIPS_OP_CACHE,  /* cache op-field(rt), offset(rs)  */
  MIPS_OP_SYNC,   /* synchronization barrier         */
  MIPS_OP_EHB,    /* execution hazard barrier        */
  MIPS_OP_WRPGPR, /* rd(current SRS) <- rt(prev SRS)  */
  MIPS_OP_ERET,   /* return from exception           */
  MIPS_OP_MTC1,   /* COP1 fs = GPR rt                */
  MIPS_OP_MFC1,   /* GPR rt = COP1 fs                */
  MIPS_OP_ADD_S,  /* fd = fs + ft  (fmt S)           */
  MIPS_OP_SUB_S,
  MIPS_OP_MUL_S,
  MIPS_OP_DIV_S,
  MIPS_OP_NEG_S,  /* fd = -fs                        */
  MIPS_OP_CVT_S_W,/* fd = (float) fs  (word)         */
  MIPS_OP_CVT_W_S,/* fd = (int) fs                   */
  MIPS_OP_C_EQ_S,
  MIPS_OP_C_LT_S,
  MIPS_OP_C_LE_S,
  MIPS_OP_BC1T,
  MIPS_OP_BC1F,
  MIPS_OP_ORG,    /* layout hole: next insn at addr  */
  MIPS_OP_NOP,
  MIPS_OP_LABEL,
  MIPS_OP_WAIT,   /* COP0 wait (JAL sleep) */
  MIPS_OP_EI,     /* Status.IE = 1 (atomic; no RMW)   */
  MIPS_OP_CLZ     /* rd = count leading zeros of rs (r2 SPECIAL2) */

  /* Add new opcodes at the end so existing .o enum values stay stable. */
} mips_opcode_t;

#define MIPS_FPR0  0
#define MIPS_FPR1  1
#define MIPS_FPR2  2
#define MIPS_FPU_HW_LABEL "_mips_fpu_hardware"

typedef struct mips_code_ *mips_code_t;
struct mips_code_ {
  mips_opcode_t   op;
  unsigned        rd;       /* R-type destination             */
  unsigned        rs;       /* source 1 / base / shift-reg    */
  unsigned        rt;       /* source 2 / dest(I-type) / value */
  int             imm;      /* I-type immediate / shift sa    */
  mips_code_t     link;
  mips_code_t     target;   /* branch/jump target (a label)   */
  unsigned long   addr;     /* laid-out address                */
  unsigned short  enc_sz;   /* 2 (microMIPS 16) or 4; 0 = hole */
};

mips_code_t mips_code_list_head(pfile_t *pf);
mips_code_t mips_code_list_tail(pfile_t *pf);
void       mips_code_list_set_ends(pfile_t *pf, mips_code_t head,
               mips_code_t tail);
void       mips_opt_delay_slot_fill(pfile_t *pf);
mips_code_t mips_code_alloc(mips_opcode_t op, unsigned rt,
    unsigned rs, int imm);
mips_code_t mips_code_r_alloc(mips_opcode_t op, unsigned rd,
    unsigned rs, unsigned rt, int sa);
mips_code_t mips_code_label_alloc(void);
void       mips_code_list_reset(pfile_t *pf);
void       mips_code_layout(pfile_t *pf);

/* variable allocation (mips_var.c) */
void       mips_var_alloc(pfile_t *pf);
/* Place `v` at the next free RAM offset (used for emit-time
 * `_mips_bitarg_N` buffers created after the alloc pass). */
void       mips_var_place(pfile_t *pf, variable_t v, variable_sz_t sz);
void       mips_var_ensure(pfile_t *pf, variable_t v);
variable_sz_t mips_def_storage_sz(pfile_t *pf, variable_def_t def);
/* the indirect-call protocol ops (defined in mips_var.c) */
#include "../libcore/indirect_alg.h"
extern const ialg_ops_t mips_ialg_ops;
void       mips_value_ensure(pfile_t *pf, value_t val);
void       mips_wide_reset(void);

/* cmd_t -> mips_code_t (mips_op.c) */
void       mips_op_generate(pfile_t *pf, const cmd_t cmd);
void       mips_op_resolve_fixups(pfile_t *pf);
void       mips_op_reset(void);

/* TEARDOWN between compiles (A4): frees the fixup table and zeroes the
 * module state. mips_op_reset() above is the LOGICAL mid-compile reset. */
void       mips_op_state_reset(void);

void       mips_clock_reset(void);

void       mips_var_state_reset(void);

void       mips_opfn_state_reset(void);
void       mips_op_consume_branch(pfile_t *pf, cmd_t cmd);
/* Reset stub + EIC vector + Status.IE when a pragma interrupt exists. */
void       mips_isr_vector_emit(pfile_t *pf);
void       mips_isr_enable_ie(pfile_t *pf);
boolean_t  mips_marker_present(pfile_t *pf, const char *name);
void       mips_marker_ensure(pfile_t *pf, const char *name);
boolean_t  mips_micromips_enabled(pfile_t *pf);
void       mips_isa_bind(pfile_t *pf);
int        mips_isa_is_micromips(void);
boolean_t  mips_mvec_enabled(pfile_t *pf);
boolean_t  mips_loader_pfm(pfile_t *pf);
boolean_t  mips_isr_srs_active(pfile_t *pf);
unsigned long mips_code_base(pfile_t *pf);
unsigned long mips_isr_vector_addr(pfile_t *pf);
boolean_t mips_mx_mvec_active(pfile_t *pf);
void mips_mvec_labels_release(void);
unsigned   mips_inst_size(const mips_code_t code);
void       mips_opt_micromips_compress(pfile_t *pf);

/* helpers shared with mips_wide.c */
void          mips_emit_imm32(unsigned reg, unsigned long addr);
void          mips_emit_ram_ptr(unsigned reg, unsigned long addr);
void          mips_emit_label_addr(unsigned reg, label_t lbl);
int           mips_emit_addr_base(pfile_t *pf, unsigned long addr,
                  unsigned *basereg);
void          mips_load_value(pfile_t *pf, value_t val, unsigned reg);
/* no home-fill copy: for fixed-register staging that consumes the
 * operand only later (see mips_op.c) */
void          mips_load_value_nofill(pfile_t *pf, value_t val, unsigned reg);
void          mips_store_value(pfile_t *pf, value_t val, unsigned reg);
void          mips_add_const_ofs(unsigned areg, unsigned scratch,
                  variable_const_t ofs);
unsigned long mips_var_ram(variable_t v);
boolean_t     mips_value_is_ptr_deref(const value_t val);
mips_code_t   mips_emit_cond_branch(mips_opcode_t bop, unsigned rs,
                  unsigned rt, label_t lbl);

/* N-byte / wide integer (mips_wide.c) -- 5+ byte assign, ALU, shift, cmp */
boolean_t  mips_is_wide(value_t dst, value_t v1, value_t v2);
void       mips_wide_assign(pfile_t *pf, value_t dst, value_t src);
boolean_t  mips_wide_generate(pfile_t *pf, operator_t op, value_t dst,
               value_t v1, value_t v2, const cmd_t cmd);

/* IEEE754 single soft-float (mips_opfn.c) -- outlined helpers */
#define MIPS_LABEL_FLOAT_SUB      "_mips_float_sub"
#define MIPS_LABEL_FLOAT_ADD      "_mips_float_add"
#define MIPS_LABEL_FLOAT_MULTIPLY "_mips_float_multiply"
#define MIPS_LABEL_FLOAT_DIVIDE   "_mips_float_divide"
#define MIPS_LABEL_FLOAT_SCONV    "_mips_float_sconv"
#define MIPS_LABEL_FLOAT_CONV     "_mips_float_conv"
#define MIPS_LABEL_FLOAT_TOINT    "_mips_float_toint"

boolean_t  mips_op_is_float(value_t dst, value_t v1, value_t v2);
boolean_t  mips_fpu_hw_enabled(pfile_t *pf);
void       mips_fpu_enable_cu1(pfile_t *pf);
boolean_t  mips_float_intrinsics_needed(pfile_t *pf);
void       mips_float_intrinsics_create(pfile_t *pf);
void       mips_float_assign_to(pfile_t *pf, value_t dst, value_t src);
void       mips_float_assign_from(pfile_t *pf, value_t dst, value_t src);
void       mips_float_binop(pfile_t *pf, operator_t op, value_t dst,
               value_t v1, value_t v2);
void       mips_float_neg(pfile_t *pf, value_t dst, value_t src);
boolean_t  mips_float_compare_branch(pfile_t *pf, const cmd_t cmd,
               operator_t op, value_t v1, value_t v2);
void       mips_float_compare_bool(pfile_t *pf, value_t dst, operator_t op,
               value_t v1, value_t v2);

/* Cooperative tasks (mips_task.c) -- PIC `_task_list` / start / suspend */
#define MIPS_LABEL_TASK_YIELD "_mips_task_yield"

boolean_t  mips_tasks_used(pfile_t *pf);
void       mips_task_alloc(pfile_t *pf);
void       mips_task_zero(pfile_t *pf);
void       mips_emit_task_start(pfile_t *pf, const cmd_t cmd);
void       mips_emit_task_yield(pfile_t *pf);
void       mips_emit_task_suicide(pfile_t *pf);
void       mips_emit_task_suspend(pfile_t *pf, const cmd_t cmd);
void       mips_emit_task_resume(pfile_t *pf, const cmd_t cmd);
void       mips_emit_task_kill(pfile_t *pf, const cmd_t cmd);
void       mips_emit_task_restart(pfile_t *pf, const cmd_t cmd);
void       mips_task_intrinsics_create(pfile_t *pf);

/* Call / jump helpers used by soft-float and other outlined code. */
mips_code_t mips_emit_jal(label_t lbl);
mips_code_t mips_emit_j(label_t lbl);

/* _usec_delay (mips_delay.c) -- SYSCLK cycle-count loops */
void       mips_delay_create(pfile_t *pf, variable_const_t usec);

/* instruction encoding (mips_inst.c) */
unsigned long mips_inst_encode(const mips_code_t code);

/* HEX writer (mips_hex.c) */
void       mips_hex_write(pfile_t *pf);

/* Flash const pool (mips_const.c) -- `_cstrN` / `const byte tbl[]` */
boolean_t  mips_var_is_flash_const(variable_t v);
void       mips_const_pool_add(variable_t v);
void       mips_const_pool_reset(void);

/* TEARDOWN between compiles (A4): frees both growable tables. */
void       mips_const_state_reset(void);
void       mips_const_pool_resolve(pfile_t *pf);
void       mips_const_pool_hex_write(pfile_t *pf);
void       mips_emit_flash_addr(unsigned reg, variable_t v,
               unsigned long extra);
void       mips_emit_var_addr(unsigned reg, variable_t v,
               unsigned long extra);

/* hex address form (mips_hex.c): the default emits PHYSICAL addresses
 * (PFM app at 0x1Dxxxxxx, boot flash at 0x1FCxxxxx) so ipecmd programs
 * the image directly -- no pic32_program_hex_build.py translation
 * needed.  The -sim-hex option keeps the legacy KSEG virtual form
 * (0x9Dxxxxxx / 0xBFCxxxxx) for the QEMU loader, which consumes
 * records at their literal (virtual) addresses. */
unsigned long mips_hex_addr(const pfile_t *pf, unsigned long addr);

#endif /* _mips_h__ */
