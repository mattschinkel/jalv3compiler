/************************************************************
 **
 ** mips_inst.c : PIC32 (MIPS32) instruction encoding
 **
 ** Encodes the mips_code_t IR into 32-bit MIPS32 machine words.
 ** Reference: MIPS32 ISA (Release 2), PIC32MZ EF (MIPS32 r5 integer).
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include "../libcore/pfile.h"
#include "mips.h"

/* MIPS major opcodes (distinct from the mips_opcode_t enum in mips.h). */
#define MIPS_MAJ_LUI    0x0F
#define MIPS_MAJ_ADDIU  0x09
#define MIPS_MAJ_ORI    0x0D
#define MIPS_MAJ_ANDI   0x0C
#define MIPS_MAJ_XORI   0x0E
#define MIPS_MAJ_LB     0x20
#define MIPS_MAJ_LBU    0x24
#define MIPS_MAJ_LH     0x21
#define MIPS_MAJ_LHU    0x25
#define MIPS_MAJ_LW     0x23
#define MIPS_MAJ_SB     0x28
#define MIPS_MAJ_SH     0x29
#define MIPS_MAJ_SW     0x2B
#define MIPS_MAJ_BEQ    0x04
#define MIPS_MAJ_BNE    0x05
#define MIPS_MAJ_J      0x02
#define MIPS_MAJ_JAL    0x03
#define MIPS_MAJ_COP0   0x10
#define MIPS_MAJ_COP1   0x11
#define MIPS_MAJ_CACHE  0x2F
#define MIPS_MAJ_SPEC   0x00   /* R-type */

/* R-type function codes (major opcode = SPECIAL 0x00). */
#define MIPS_FN_SLL   0x00
#define MIPS_FN_SYNC  0x0F
#define MIPS_FN_SRL   0x02
#define MIPS_FN_SRA   0x03
#define MIPS_FN_SLLV  0x04
#define MIPS_FN_SRLV  0x06
#define MIPS_FN_SRAV  0x07
#define MIPS_FN_MFHI  0x10
#define MIPS_FN_MFLO  0x12
#define MIPS_FN_MULT  0x18
#define MIPS_FN_MULTU 0x19
#define MIPS_FN_DIV   0x1A
#define MIPS_FN_DIVU  0x1B
#define MIPS_FN_ADDU  0x21
#define MIPS_FN_SUBU  0x23
#define MIPS_FN_AND   0x24
#define MIPS_FN_OR    0x25
#define MIPS_FN_XOR   0x26
#define MIPS_FN_SLT   0x2A
#define MIPS_FN_SLTU  0x2B
#define MIPS_FN_JR    0x08
#define MIPS_FN_JALR  0x09
#define MIPS_FN_ERET  0x18
#define MIPS_FN_WAIT  0x20
#define MIPS_COP0_MF  0x00
#define MIPS_COP0_MT  0x04
#define MIPS_COP0_CO  0x10
#define MIPS_COP1_MF  0x00
#define MIPS_COP1_MT  0x04
#define MIPS_COP1_BC  0x08
#define MIPS_FMT_S    16
#define MIPS_FMT_W    20
#define MIPS_FN_ADD_S 0x00
#define MIPS_FN_SUB_S 0x01
#define MIPS_FN_MUL_S 0x02
#define MIPS_FN_DIV_S 0x03
#define MIPS_FN_NEG_S 0x07
#define MIPS_FN_CVT_S 0x20
#define MIPS_FN_CVT_W 0x24
#define MIPS_C_EQ     0x32
#define MIPS_C_OLT    0x3C
#define MIPS_C_OLE    0x3E

static unsigned long enc_i_type(unsigned op, unsigned rs,
    unsigned rt, int imm)
{
  return ((unsigned long) op << 26)
       | ((unsigned long) (rs & 0x1F) << 21)
       | ((unsigned long) (rt & 0x1F) << 16)
       | ((unsigned long) (imm & 0xFFFF));
}

static unsigned long enc_r_type(unsigned fcode, unsigned rd,
    unsigned rs, unsigned rt, unsigned shamt)
{
  unsigned long w;

  w  = ((unsigned long) MIPS_MAJ_SPEC) << 26;
  w |= ((unsigned long) (rs & 0x1F)) << 21;
  w |= ((unsigned long) (rt & 0x1F)) << 16;
  w |= ((unsigned long) (rd & 0x1F)) << 11;
  w |= ((unsigned long) (shamt & 0x1F)) << 6;
  w |= (unsigned long) fcode;
  return w;
}

static unsigned long mips_inst_encode_mips32(const mips_code_t code)
{
  switch (code->op) {
    case MIPS_OP_LUI: {
      int imm;

      /* `target` set: materialize a label address (WHEREIS of a procedure).
       * High 16 bits of the laid-out flash address; resolved after layout. */
      imm = code->target
        ? (int) ((code->target->addr >> 16) & 0xFFFF)
        : code->imm;
      return enc_i_type(MIPS_MAJ_LUI, 0, code->rt, imm);
    }

    case MIPS_OP_ADDIU:
      return enc_i_type(MIPS_MAJ_ADDIU, code->rs, code->rt, code->imm);
    case MIPS_OP_ORI: {
      int imm;

      imm = code->target
        ? (int) (code->target->addr & 0xFFFF)
        : code->imm;
      return enc_i_type(MIPS_MAJ_ORI, code->rs, code->rt, imm);
    }
    case MIPS_OP_ANDI:
      return enc_i_type(MIPS_MAJ_ANDI, code->rs, code->rt, code->imm);
    case MIPS_OP_XORI:
      return enc_i_type(MIPS_MAJ_XORI, code->rs, code->rt, code->imm);

    case MIPS_OP_LB:
      return enc_i_type(MIPS_MAJ_LB,  code->rs, code->rt, code->imm);
    case MIPS_OP_LBU:
      return enc_i_type(MIPS_MAJ_LBU, code->rs, code->rt, code->imm);
    case MIPS_OP_LH:
      return enc_i_type(MIPS_MAJ_LH,  code->rs, code->rt, code->imm);
    case MIPS_OP_LHU:
      return enc_i_type(MIPS_MAJ_LHU, code->rs, code->rt, code->imm);
    case MIPS_OP_LW:
      return enc_i_type(MIPS_MAJ_LW,  code->rs, code->rt, code->imm);
    case MIPS_OP_SB:
      return enc_i_type(MIPS_MAJ_SB,  code->rs, code->rt, code->imm);
    case MIPS_OP_SH:
      return enc_i_type(MIPS_MAJ_SH,  code->rs, code->rt, code->imm);
    case MIPS_OP_SW:
      return enc_i_type(MIPS_MAJ_SW,  code->rs, code->rt, code->imm);

    case MIPS_OP_ADDU:
      return enc_r_type(MIPS_FN_ADDU, code->rd, code->rs, code->rt, 0);
    case MIPS_OP_SUBU:
      return enc_r_type(MIPS_FN_SUBU, code->rd, code->rs, code->rt, 0);
    case MIPS_OP_AND:
      return enc_r_type(MIPS_FN_AND,  code->rd, code->rs, code->rt, 0);
    case MIPS_OP_OR:
      return enc_r_type(MIPS_FN_OR,   code->rd, code->rs, code->rt, 0);
    case MIPS_OP_XOR:
      return enc_r_type(MIPS_FN_XOR,  code->rd, code->rs, code->rt, 0);

    case MIPS_OP_SLL:
      return enc_r_type(MIPS_FN_SLL, code->rd, 0, code->rt, code->imm);
    case MIPS_OP_SRL:
      return enc_r_type(MIPS_FN_SRL, code->rd, 0, code->rt, code->imm);
    case MIPS_OP_SRA:
      return enc_r_type(MIPS_FN_SRA, code->rd, 0, code->rt, code->imm);
    case MIPS_OP_SLLV:
      return enc_r_type(MIPS_FN_SLLV, code->rd, code->rs, code->rt, 0);
    case MIPS_OP_SRLV:
      return enc_r_type(MIPS_FN_SRLV, code->rd, code->rs, code->rt, 0);
    case MIPS_OP_SRAV:
      return enc_r_type(MIPS_FN_SRAV, code->rd, code->rs, code->rt, 0);

    case MIPS_OP_MULT:
      return enc_r_type(MIPS_FN_MULT, 0, code->rs, code->rt, 0);
    case MIPS_OP_MULTU:
      return enc_r_type(MIPS_FN_MULTU, 0, code->rs, code->rt, 0);
    case MIPS_OP_DIV:
      return enc_r_type(MIPS_FN_DIV,  0, code->rs, code->rt, 0);
    case MIPS_OP_DIVU:
      return enc_r_type(MIPS_FN_DIVU, 0, code->rs, code->rt, 0);
    case MIPS_OP_MFLO:
      return enc_r_type(MIPS_FN_MFLO, code->rd, 0, 0, 0);
    case MIPS_OP_MFHI:
      return enc_r_type(MIPS_FN_MFHI, code->rd, 0, 0, 0);
    case MIPS_OP_SLT:
      return enc_r_type(MIPS_FN_SLT, code->rd, code->rs, code->rt, 0);
    case MIPS_OP_SLTU:
      return enc_r_type(MIPS_FN_SLTU, code->rd, code->rs, code->rt, 0);
    case MIPS_OP_CLZ:
      /* SPECIAL2 (0x1C) CLZ: rt field must equal rd (r2 encoding) */
      return (0x1CUL << 26)
          | ((unsigned long) (code->rs & 0x1F) << 21)
          | ((unsigned long) (code->rd & 0x1F) << 16)
          | ((unsigned long) (code->rd & 0x1F) << 11)
          | 0x20UL;

    case MIPS_OP_BEQ:
    case MIPS_OP_BNE: {
      unsigned op  = (MIPS_OP_BEQ == code->op) ? MIPS_MAJ_BEQ : MIPS_MAJ_BNE;
      long   off;
      if (code->target) {
        off = ((long) code->target->addr - ((long) code->addr + 4)) >> 2;
      } else {
        off = code->imm;
      }
      return enc_i_type(op, code->rs, code->rt, (int) off);
    }

    case MIPS_OP_J: {
      unsigned long target;
      if (code->target) {
        target = (code->target->addr >> 2) & 0x3FFFFFF;
      } else {
        target = ((unsigned long) code->imm) & 0x3FFFFFF;
      }
      return ((unsigned long) MIPS_MAJ_J << 26) | target;
    }

    case MIPS_OP_JAL: {
      unsigned long target;
      if (code->target) {
        target = (code->target->addr >> 2) & 0x3FFFFFF;
      } else {
        target = ((unsigned long) code->imm) & 0x3FFFFFF;
      }
      return ((unsigned long) MIPS_MAJ_JAL << 26) | target;
    }

    case MIPS_OP_JR:
      return enc_r_type(MIPS_FN_JR, 0, code->rs, 0, 0);

    case MIPS_OP_JALR:
      /* jalr rd, rs — rd defaults to $ra when allocated as r-type. */
      return enc_r_type(MIPS_FN_JALR, code->rd ? code->rd : MIPS_REG_RA,
          code->rs, 0, 0);

    case MIPS_OP_MFC0:
    case MIPS_OP_MTC0: {
      unsigned mfmt;

      mfmt = (MIPS_OP_MFC0 == code->op) ? MIPS_COP0_MF : MIPS_COP0_MT;
      return ((unsigned long) MIPS_MAJ_COP0 << 26)
           | ((unsigned long) (mfmt & 0x1F) << 21)
           | ((unsigned long) (code->rt & 0x1F) << 16)
           | ((unsigned long) (code->rd & 0x1F) << 11)
           | ((unsigned long) (code->imm & 0x7));
    }

    case MIPS_OP_CACHE:
      return enc_i_type(MIPS_MAJ_CACHE, code->rs, code->rt, code->imm);

    case MIPS_OP_SYNC:
      return enc_r_type(MIPS_FN_SYNC, 0, 0, 0, 0);

    case MIPS_OP_EHB:
      return enc_r_type(MIPS_FN_SLL, 0, 0, 0, 3);

    case MIPS_OP_WRPGPR:
      /* wrpgpr rd, rt: GPR rt of the PREVIOUS shadow set becomes GPR rd
       * of the CURRENT set (COP0 CO-form; word family XC32-pinned --
       * wrpgpr $gp,$gp == 0x41DCE000). */
      return 0x41C00000UL
           | ((unsigned long) (code->rt & 0x1F) << 16)
           | ((unsigned long) (code->rd & 0x1F) << 11);

    case MIPS_OP_EI:
      /* ei (no operand): COP0, CO=0, rt=t0-ish encoding 0x41606020 --
       * GAS-pinned.  Sets Status.IE atomically; unlike the mfc0/mtc0
       * RMW it does not touch EXL/ERL (clearing those at startup hung
       * the core on silicon before the first user instruction --
       * 2026-10-09 ladder bisection: every IE-RMW variant silent, ei
       * variant + no-RMW variant print). */
      return 0x41606020UL;

    case MIPS_OP_ERET:
      return ((unsigned long) MIPS_MAJ_COP0 << 26)
           | ((unsigned long) MIPS_COP0_CO << 21)
           | (unsigned long) MIPS_FN_ERET;

    case MIPS_OP_WAIT:
      return ((unsigned long) MIPS_MAJ_COP0 << 26)
           | ((unsigned long) MIPS_COP0_CO << 21)
           | (unsigned long) MIPS_FN_WAIT;

    case MIPS_OP_MTC1:
    case MIPS_OP_MFC1: {
      unsigned mfmt;

      mfmt = (MIPS_OP_MTC1 == code->op) ? MIPS_COP1_MT : MIPS_COP1_MF;
      return ((unsigned long) MIPS_MAJ_COP1 << 26)
           | ((unsigned long) (mfmt & 0x1F) << 21)
           | ((unsigned long) (code->rt & 0x1F) << 16)
           | ((unsigned long) (code->rd & 0x1F) << 11);
    }

    case MIPS_OP_ADD_S:
    case MIPS_OP_SUB_S:
    case MIPS_OP_MUL_S:
    case MIPS_OP_DIV_S:
    case MIPS_OP_NEG_S:
    case MIPS_OP_CVT_S_W:
    case MIPS_OP_CVT_W_S: {
      unsigned fmt;
      unsigned fn;

      fmt = MIPS_FMT_S;
      fn = MIPS_FN_ADD_S;
      if (MIPS_OP_SUB_S == code->op) {
        fn = MIPS_FN_SUB_S;
      } else if (MIPS_OP_MUL_S == code->op) {
        fn = MIPS_FN_MUL_S;
      } else if (MIPS_OP_DIV_S == code->op) {
        fn = MIPS_FN_DIV_S;
      } else if (MIPS_OP_NEG_S == code->op) {
        fn = MIPS_FN_NEG_S;
      } else if (MIPS_OP_CVT_S_W == code->op) {
        fmt = MIPS_FMT_W;
        fn = MIPS_FN_CVT_S;
      } else if (MIPS_OP_CVT_W_S == code->op) {
        fn = MIPS_FN_CVT_W;
      }
      return ((unsigned long) MIPS_MAJ_COP1 << 26)
           | ((unsigned long) (fmt & 0x1F) << 21)
           | ((unsigned long) (code->rt & 0x1F) << 16)
           | ((unsigned long) (code->rs & 0x1F) << 11)
           | ((unsigned long) (code->rd & 0x1F) << 6)
           | (unsigned long) fn;
    }

    case MIPS_OP_C_EQ_S:
    case MIPS_OP_C_LT_S:
    case MIPS_OP_C_LE_S: {
      unsigned cond;

      cond = MIPS_C_EQ;
      if (MIPS_OP_C_LT_S == code->op) {
        cond = MIPS_C_OLT;
      } else if (MIPS_OP_C_LE_S == code->op) {
        cond = MIPS_C_OLE;
      }
      return ((unsigned long) MIPS_MAJ_COP1 << 26)
           | ((unsigned long) MIPS_FMT_S << 21)
           | ((unsigned long) (code->rt & 0x1F) << 16)
           | ((unsigned long) (code->rs & 0x1F) << 11)
           | (unsigned long) cond;
    }

    case MIPS_OP_BC1T:
    case MIPS_OP_BC1F: {
      long off;
      unsigned tf;

      if (code->target) {
        off = ((long) code->target->addr - ((long) code->addr + 4)) >> 2;
      } else {
        off = code->imm;
      }
      tf = (MIPS_OP_BC1T == code->op) ? 1u : 0u;
      return ((unsigned long) MIPS_MAJ_COP1 << 26)
           | ((unsigned long) MIPS_COP1_BC << 21)
           | ((unsigned long) (tf & 0x1F) << 16)
           | ((unsigned long) (off & 0xFFFF));
    }

    case MIPS_OP_ORG:
    case MIPS_OP_NOP:
    case MIPS_OP_LABEL:
    case MIPS_OP_NONE:
    default:
      return 0x00000000UL;   /* NOP = SLL $zero,$zero,0 */
  }
}

/* microMIPS32 major opcodes (bits 31:26). I-type fields are rt|rs|imm. */
#define MM_ADDIU32  0x0C
#define MM_ORI32    0x14
#define MM_ANDI32   0x34
#define MM_XORI32   0x1C
#define MM_LB32     0x07
#define MM_LBU32    0x05
#define MM_LH32     0x0F
#define MM_LHU32    0x0D
#define MM_LW32     0x3F
#define MM_SB32     0x06
#define MM_SH32     0x0E
#define MM_SW32     0x3E
#define MM_BEQ32    0x25
#define MM_BNE32    0x2D
#define MM_J32      0x35
#define MM_JAL32    0x3D
#define MM_POOL32A  0x00
#define MM_POOL32I  0x10
#define MM_POOL32F  0x15
#define MM_POOL32AXF 0x3C
#define MM_LUI_MIN  0x0D
#define MM_ADDU32   0x05
#define MM_SUBU32   0x07
#define MM_AND32    0x09
#define MM_OR32     0x0A
#define MM_XOR32    0x0C
#define MM_SLT32    0x0D
#define MM_SLTU32   0x0E
#define MM_SLL32    0x00
#define MM_SRL32    0x01
#define MM_SRA32    0x02
#define MM_SLLV32   0x00
#define MM_SRLV32   0x01
#define MM_SRAV32   0x02
#define MM_ARITH    0x10
#define MM_SHIFTI   0x00
#define MM_MFC0     0x03
#define MM_MTC0     0x0B
#define MM_ERET_EXT 0x0D
#define MM_ERET_MIN 0x0F
#define MM_WAIT_MIN 0x09
#define MM_JALR_EXT 0x3C
#define MM_MUL_EXT  0x2C
#define MM_MULT     0x08
#define MM_MULTU    0x09
#define MM_DIV      0x0A
#define MM_DIVU     0x0B
#define MM_HILO_EXT 0x35
#define MM_MFHI32   0x00
#define MM_MFLO32   0x01
#define MM_POOL32FXF 0x3B
#define MM_MFC1     0x80
#define MM_MTC1     0xA0
/* POOL32FXF bits 15:6 (qemu FLOAT_*_FMT). NEG.S fmt S; CVT.S.W =
 * (FMT_DWL_W<<7)|CVT_S; CVT.W.S = (FMT_SD_S<<8)|CVT_W. */
#define MM_NEG_S    0x2D
#define MM_CVT_S_W  0xED
#define MM_CVT_W_S  0x24
#define MM_FP_ARITH 0x30
#define MM_ADD_FMT  0x00
#define MM_SUB_FMT  0x01
#define MM_MUL_FMT  0x02
#define MM_DIV_FMT  0x03
#define MM_C_COND   0x3C
#define MM_BC1F     0x1C
#define MM_BC1T     0x1D
#define MM_MOVE16   0x03
#define MM_LWSP16   0x12
#define MM_SWSP16   0x32
#define MM_POOL16D  0x13

static unsigned long enc_mm_i(unsigned op, unsigned rt, unsigned rs, int imm)
{
  return ((unsigned long) op << 26)
       | ((unsigned long) (rt & 0x1F) << 21)
       | ((unsigned long) (rs & 0x1F) << 16)
       | ((unsigned long) (imm & 0xFFFF));
}

static unsigned long enc_mm_pool32a(unsigned rt, unsigned rs, unsigned rd,
    unsigned minor6, unsigned bits5_0)
{
  return ((unsigned long) (rt & 0x1F) << 21)
       | ((unsigned long) (rs & 0x1F) << 16)
       | ((unsigned long) (rd & 0x1F) << 11)
       | ((unsigned long) (minor6 & 0x1F) << 6)
       | (unsigned long) (bits5_0 & 0x3F);
}

static unsigned long enc_mm_pool32axf(unsigned rt, unsigned rs,
    unsigned bits15_12, unsigned ext11_6)
{
  return ((unsigned long) (rt & 0x1F) << 21)
       | ((unsigned long) (rs & 0x1F) << 16)
       | ((unsigned long) (bits15_12 & 0xF) << 12)
       | ((unsigned long) (ext11_6 & 0x3F) << 6)
       | (unsigned long) MM_POOL32AXF;
}

static long mm_branch_off(const mips_code_t code)
{
  unsigned sz;

  if (!code->target) {
    return code->imm;
  }
  sz = mips_inst_size(code);
  if (!sz) {
    sz = (unsigned) MIPS_WORD_SZ;
  }
  return ((long) code->target->addr - ((long) code->addr + (long) sz)) >> 1;
}

static unsigned long enc_mm_16(const mips_code_t code)
{
  if (MIPS_OP_NOP == code->op) {
    return (unsigned long) (MM_MOVE16 << 10);
  }
  if ((MIPS_OP_ADDU == code->op)
      && ((0 == code->rt) || (0 == code->rs))) {
    unsigned rd;
    unsigned rs;

    rd = code->rd;
    rs = code->rt ? code->rt : code->rs;
    return ((unsigned long) MM_MOVE16 << 10)
         | ((unsigned long) (rd & 0x1F) << 5)
         | (unsigned long) (rs & 0x1F);
  }
  if ((MIPS_OP_LW == code->op) && (MIPS_REG_SP == code->rs)
      && (code->imm >= 0) && (code->imm <= 124) && (0 == (code->imm & 3))) {
    return ((unsigned long) MM_LWSP16 << 10)
         | ((unsigned long) (code->rt & 0x1F) << 5)
         | (unsigned long) ((code->imm >> 2) & 0x1F);
  }
  if ((MIPS_OP_SW == code->op) && (MIPS_REG_SP == code->rs)
      && (code->imm >= 0) && (code->imm <= 124) && (0 == (code->imm & 3))) {
    return ((unsigned long) MM_SWSP16 << 10)
         | ((unsigned long) (code->rt & 0x1F) << 5)
         | (unsigned long) ((code->imm >> 2) & 0x1F);
  }
  if ((MIPS_OP_ADDIU == code->op) && (MIPS_REG_SP == code->rt)
      && (MIPS_REG_SP == code->rs) && (0 == (code->imm & 3))) {
    int decoded;
    int encoded;

    decoded = code->imm >> 2;
    encoded = -1;
    if ((decoded >= 2) && (decoded <= 255)) {
      encoded = decoded;
    } else if ((decoded >= -256) && (decoded <= -3)) {
      encoded = decoded + 512;
    } else if (256 == decoded) {
      encoded = 0;
    } else if (257 == decoded) {
      encoded = 1;
    } else if (-258 == decoded) {
      encoded = 510;
    } else if (-257 == decoded) {
      encoded = 511;
    }
    if (encoded >= 0) {
      return ((unsigned long) MM_POOL16D << 10)
           | ((unsigned long) (encoded & 0x1FF) << 1)
           | 1UL;
    }
  }
  return 0;
}

static unsigned long mips_inst_encode_micromips(const mips_code_t code)
{
  if (code->enc_sz == 2) {
    return enc_mm_16(code);
  }
  switch (code->op) {
    case MIPS_OP_LUI: {
      int imm;

      imm = code->target
        ? (int) ((code->target->addr >> 16) & 0xFFFF)
        : code->imm;
      return ((unsigned long) MM_POOL32I << 26)
           | ((unsigned long) MM_LUI_MIN << 21)
           | ((unsigned long) (code->rt & 0x1F) << 16)
           | ((unsigned long) (imm & 0xFFFF));
    }
    case MIPS_OP_ADDIU:
      return enc_mm_i(MM_ADDIU32, code->rt, code->rs, code->imm);
    case MIPS_OP_ORI: {
      int imm;

      imm = code->target
        ? (int) (code->target->addr & 0xFFFF)
        : code->imm;
      return enc_mm_i(MM_ORI32, code->rt, code->rs, imm);
    }
    case MIPS_OP_ANDI:
      return enc_mm_i(MM_ANDI32, code->rt, code->rs, code->imm);
    case MIPS_OP_XORI:
      return enc_mm_i(MM_XORI32, code->rt, code->rs, code->imm);
    case MIPS_OP_LB:
      return enc_mm_i(MM_LB32, code->rt, code->rs, code->imm);
    case MIPS_OP_LBU:
      return enc_mm_i(MM_LBU32, code->rt, code->rs, code->imm);
    case MIPS_OP_LH:
      return enc_mm_i(MM_LH32, code->rt, code->rs, code->imm);
    case MIPS_OP_LHU:
      return enc_mm_i(MM_LHU32, code->rt, code->rs, code->imm);
    case MIPS_OP_LW:
      return enc_mm_i(MM_LW32, code->rt, code->rs, code->imm);
    case MIPS_OP_SB:
      return enc_mm_i(MM_SB32, code->rt, code->rs, code->imm);
    case MIPS_OP_SH:
      return enc_mm_i(MM_SH32, code->rt, code->rs, code->imm);
    case MIPS_OP_SW:
      return enc_mm_i(MM_SW32, code->rt, code->rs, code->imm);
    case MIPS_OP_ADDU:
      return enc_mm_pool32a(code->rt, code->rs, code->rd,
          MM_ADDU32, MM_ARITH);
    case MIPS_OP_SUBU:
      return enc_mm_pool32a(code->rt, code->rs, code->rd,
          MM_SUBU32, MM_ARITH);
    case MIPS_OP_AND:
      return enc_mm_pool32a(code->rt, code->rs, code->rd,
          MM_AND32, MM_ARITH);
    case MIPS_OP_OR:
      return enc_mm_pool32a(code->rt, code->rs, code->rd,
          MM_OR32, MM_ARITH);
    case MIPS_OP_XOR:
      return enc_mm_pool32a(code->rt, code->rs, code->rd,
          MM_XOR32, MM_ARITH);
    case MIPS_OP_SLT:
      return enc_mm_pool32a(code->rt, code->rs, code->rd,
          MM_SLT32, MM_ARITH);
    case MIPS_OP_SLTU:
      return enc_mm_pool32a(code->rt, code->rs, code->rd,
          MM_SLTU32, MM_ARITH);
    case MIPS_OP_SLL:
      return enc_mm_pool32a(code->rd, code->rt, (unsigned) code->imm,
          MM_SLL32, MM_SHIFTI);
    case MIPS_OP_SRL:
      return enc_mm_pool32a(code->rd, code->rt, (unsigned) code->imm,
          MM_SRL32, MM_SHIFTI);
    case MIPS_OP_SRA:
      return enc_mm_pool32a(code->rd, code->rt, (unsigned) code->imm,
          MM_SRA32, MM_SHIFTI);
    case MIPS_OP_SLLV:
      return enc_mm_pool32a(code->rt, code->rs, code->rd,
          MM_SLLV32, MM_ARITH);
    case MIPS_OP_SRLV:
      return enc_mm_pool32a(code->rt, code->rs, code->rd,
          MM_SRLV32, MM_ARITH);
    case MIPS_OP_SRAV:
      return enc_mm_pool32a(code->rt, code->rs, code->rd,
          MM_SRAV32, MM_ARITH);
    case MIPS_OP_MULT:
      return enc_mm_pool32axf(code->rt, code->rs, MM_MULT, MM_MUL_EXT);
    case MIPS_OP_MULTU:
      return enc_mm_pool32axf(code->rt, code->rs, MM_MULTU, MM_MUL_EXT);
    case MIPS_OP_CLZ:
      /* POOL32AXF 0x2C group, index 5; the DEST rides the rt field
       * (QEMU decode: gen_cl(rd=rt, rs)) -- this encoder's 2nd slot */
      return enc_mm_pool32axf(code->rs, code->rd, 0x05, MM_MUL_EXT);
    case MIPS_OP_DIV:
      return enc_mm_pool32axf(code->rt, code->rs, MM_DIV, MM_MUL_EXT);
    case MIPS_OP_DIVU:
      return enc_mm_pool32axf(code->rt, code->rs, MM_DIVU, MM_MUL_EXT);
    case MIPS_OP_MFLO:
      return enc_mm_pool32axf(0, code->rd, MM_MFLO32, MM_HILO_EXT);
    case MIPS_OP_MFHI:
      return enc_mm_pool32axf(0, code->rd, MM_MFHI32, MM_HILO_EXT);
    case MIPS_OP_BEQ:
      return enc_mm_i(MM_BEQ32, code->rt, code->rs, (int) mm_branch_off(code));
    case MIPS_OP_BNE:
      return enc_mm_i(MM_BNE32, code->rt, code->rs, (int) mm_branch_off(code));
    case MIPS_OP_J: {
      unsigned long target;

      target = code->target
        ? ((code->target->addr >> 1) & 0x3FFFFFFUL)
        : (((unsigned long) code->imm) & 0x3FFFFFFUL);
      return ((unsigned long) MM_J32 << 26) | target;
    }
    case MIPS_OP_JAL: {
      unsigned long target;

      target = code->target
        ? ((code->target->addr >> 1) & 0x3FFFFFFUL)
        : (((unsigned long) code->imm) & 0x3FFFFFFUL);
      return ((unsigned long) MM_JAL32 << 26) | target;
    }
    case MIPS_OP_JR:
      return enc_mm_pool32axf(0, code->rs, 0, MM_JALR_EXT);
    case MIPS_OP_JALR:
      return enc_mm_pool32axf(code->rd ? code->rd : MIPS_REG_RA,
          code->rs, 0, MM_JALR_EXT);
    case MIPS_OP_MFC0:
      return enc_mm_pool32axf(code->rt, code->rd, 0, MM_MFC0)
           | ((unsigned long) (code->imm & 0x7) << 11);
    case MIPS_OP_MTC0:
      return enc_mm_pool32axf(code->rt, code->rd, 0, MM_MTC0)
           | ((unsigned long) (code->imm & 0x7) << 11);
    case MIPS_OP_ERET:
      return enc_mm_pool32axf(0, 0, MM_ERET_MIN, MM_ERET_EXT);
    case MIPS_OP_WAIT:
      return enc_mm_pool32axf(0, 0, MM_WAIT_MIN, MM_ERET_EXT);
    case MIPS_OP_MTC1:
      return ((unsigned long) MM_POOL32F << 26)
           | ((unsigned long) (code->rt & 0x1F) << 21)
           | ((unsigned long) (code->rd & 0x1F) << 16)
           | ((unsigned long) MM_MTC1 << 6)
           | (unsigned long) MM_POOL32FXF;
    case MIPS_OP_MFC1:
      return ((unsigned long) MM_POOL32F << 26)
           | ((unsigned long) (code->rt & 0x1F) << 21)
           | ((unsigned long) (code->rd & 0x1F) << 16)
           | ((unsigned long) MM_MFC1 << 6)
           | (unsigned long) MM_POOL32FXF;
    case MIPS_OP_ADD_S:
    case MIPS_OP_SUB_S:
    case MIPS_OP_MUL_S:
    case MIPS_OP_DIV_S: {
      unsigned fmtop;

      fmtop = MM_ADD_FMT;
      if (MIPS_OP_SUB_S == code->op) {
        fmtop = MM_SUB_FMT;
      } else if (MIPS_OP_MUL_S == code->op) {
        fmtop = MM_MUL_FMT;
      } else if (MIPS_OP_DIV_S == code->op) {
        fmtop = MM_DIV_FMT;
      }
      return ((unsigned long) MM_POOL32F << 26)
           | ((unsigned long) (code->rt & 0x1F) << 21)
           | ((unsigned long) (code->rs & 0x1F) << 16)
           | ((unsigned long) (code->rd & 0x1F) << 11)
           | ((unsigned long) fmtop << 6)
           | (unsigned long) MM_FP_ARITH;
    }
    case MIPS_OP_C_EQ_S:
    case MIPS_OP_C_LT_S:
    case MIPS_OP_C_LE_S: {
      unsigned cond;

      cond = 2;
      if (MIPS_OP_C_LT_S == code->op) {
        cond = 12;
      } else if (MIPS_OP_C_LE_S == code->op) {
        cond = 14;
      }
      return ((unsigned long) MM_POOL32F << 26)
           | ((unsigned long) (code->rt & 0x1F) << 21)
           | ((unsigned long) (code->rs & 0x1F) << 16)
           | ((unsigned long) (cond & 0xF) << 6)
           | (unsigned long) MM_C_COND;
    }
    case MIPS_OP_BC1T:
    case MIPS_OP_BC1F: {
      unsigned min;

      min = (MIPS_OP_BC1T == code->op) ? MM_BC1T : MM_BC1F;
      return ((unsigned long) MM_POOL32I << 26)
           | ((unsigned long) min << 21)
           | ((unsigned long) (mm_branch_off(code) & 0xFFFF));
    }
    case MIPS_OP_NEG_S:
    case MIPS_OP_CVT_S_W:
    case MIPS_OP_CVT_W_S: {
      unsigned ext;

      ext = MM_NEG_S;
      if (MIPS_OP_CVT_S_W == code->op) {
        ext = MM_CVT_S_W;
      } else if (MIPS_OP_CVT_W_S == code->op) {
        ext = MM_CVT_W_S;
      }
      return ((unsigned long) MM_POOL32F << 26)
           | ((unsigned long) (code->rd & 0x1F) << 21)
           | ((unsigned long) (code->rs & 0x1F) << 16)
           | ((unsigned long) (ext & 0x3FF) << 6)
           | (unsigned long) MM_POOL32FXF;
    }
    case MIPS_OP_WRPGPR:
      /* no microMIPS form: keep the 32-bit word (mixed width is legal) */
      return mips_inst_encode_mips32(code);
    case MIPS_OP_ORG:
    case MIPS_OP_NOP:
    case MIPS_OP_LABEL:
    case MIPS_OP_NONE:
    default:
      return 0;
  }
}

unsigned long mips_inst_encode(const mips_code_t code)
{
  if (mips_isa_is_micromips()) {
    return mips_inst_encode_micromips(code);
  }
  return mips_inst_encode_mips32(code);
}
