/************************************************************
 **
 ** mips_wide.c : PIC32 (MIPS32) N-byte / wide-integer codegen
 **
 ** Values that are not a native MIPS load/store width (1, 2 or 4
 ** bytes) cannot live in a single GPR access.  That includes 3-byte
 ** heap-string handles, qword/oword, and 5+ byte aggregates.  Copy,
 ** add/sub, logic, shifts and compares run little-endian per byte
 ** (or aligned 2/4-byte chunks when both addresses are compile-time
 ** constants).  Runtime offsets use only LBU/SB -- unaligned LW/SW
 ** can trap on MIPS.
 **
 ** Register convention (same as mips_op.c):
 **   $t0 -- address   $t1 -- operand/result   $t2 -- operand 2
 **   $t3 -- scratch   $at -- carry / shift bit
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include "../libutils/types.h"
#include "../libutils/mem.h"
#include "../libcore/pfile.h"
#include "../libcore/pf_log.h"
#include "../libcore/vardef.h"
#include "../libcore/variable.h"
#include "../libcore/cmd.h"
#include "../libcore/cmd_op.h"
#include "mips_reg.h"
#include "../libcore/cmd_brch.h"
#include "../libcore/operator.h"
#include "../libcore/value.h"
#include "../libcore/variable.h"
#include "../libcore/label.h"
#include "mips.h"
#include "mips_alloc.h"
#include "../libcore/wide_alg.h"

static boolean_t mips_wide_is_const(value_t val)
{
  if (!val || !value_is_const(val)) {
    return BOOLEAN_FALSE;
  }
  if (mips_value_is_ptr_deref(val)) {
    return BOOLEAN_FALSE;
  }
  return BOOLEAN_TRUE;
}

/* Width in bytes of the promoted integer.  For FLAG_BIT, value_sz_get is
 * a bit count (so `bit*24` is 24, not 3); use value_byte_sz_get only. */
static unsigned mips_wide_val_width(value_t val)
{
  unsigned s;
  unsigned t;

  if (!val) {
    return 0;
  }
  s = (unsigned) value_byte_sz_get(val);
  if (value_is_bit(val) && !value_is_pointer(val)) {
    /* Packed fields use the GPR extract/insert path up to 32 bits.
     * Do not treat `bit*24` (3 bytes) as a 3-byte integer copy. */
    return (s > 4) ? s : 1;
  }
  t = (unsigned) value_sz_get(val);
  return (t > s) ? t : s;
}

boolean_t mips_is_wide(value_t dst, value_t v1, value_t v2)
{
  unsigned s;
  unsigned t;

  s = 0;
  if (dst) {
    s = mips_wide_val_width(dst);
  }
  if (v1 && !mips_wide_is_const(v1)) {
    t = mips_wide_val_width(v1);
    if (t > s) {
      s = t;
    }
  }
  if (v2 && !mips_wide_is_const(v2)) {
    t = mips_wide_val_width(v2);
    if (t > s) {
      s = t;
    }
  }
  /* Native GPR load/store is 1, 2 or 4 bytes.  3-byte handles (and
   * 5+ byte aggregates) must copy byte-wise; LW/SW of size 3 traps
   * or clobbers the following byte. */
  if ((1 == s) || (2 == s) || (4 == s)) {
    return BOOLEAN_FALSE;
  }
  return (s > 0) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* Smallest k whose upper const bytes match byte k-1's sign fill --
 * the real width of an unnamed literal (value_constant_get gives them
 * a synthetic 16-byte UNIVERSAL def and value_byte_sz_get clamps
 * universals to 4 bytes). */
static unsigned mips_wide_const_width(variable_const_t n)
{
  unsigned k;
  unsigned b;
  unsigned fill;

  for (k = 1; k <= (unsigned) sizeof(variable_const_t); k++) {
    fill = (variable_const_byte_get(n, k - 1) & 0x80U) ? 0xFFU : 0x00U;
    for (b = k; b < (unsigned) sizeof(variable_const_t); b++) {
      if (variable_const_byte_get(n, b) != fill) {
        break;
      }
    }
    if (b >= (unsigned) sizeof(variable_const_t)) {
      return k;
    }
  }
  return (unsigned) sizeof(variable_const_t);
}

static unsigned mips_wide_val_sz(value_t val)
{
  unsigned        s;
  variable_t      v;
  variable_def_t  def;

  if (!val) {
    return 0;
  }
  if (mips_wide_is_const(val)) {
    v = value_variable_get(val);
    if (v) {
      def = variable_def_get(v);
      if (def
          && variable_def_flag_test(def, VARIABLE_DEF_FLAG_UNIVERSAL)) {
        if (variable_const_is_negative(value_const_get(val),
            VARIABLE_CONST_BIT_CT)) {
          /* negative literal in an unsigned def: keep the full 16-byte
           * view -- every const byte is real (0xFF), so wide loops
           * never consult the (unsigned) fill */
          s = (unsigned) variable_sz_get(v);
          if (s) {
            return s;
          }
        }
        return mips_wide_const_width(value_const_get(val));
      }
      s = (unsigned) variable_sz_get(v);
      if (s) {
        return s;
      }
    }
  }
  s = (unsigned) value_byte_sz_get(val);
  if (!s) {
    s = (unsigned) value_sz_get(val);
  }
  return s;
}

static unsigned mips_wide_const_byte(value_t val, unsigned ofs)
{
  const uchar     *data;
  variable_t       v;
  unsigned         limit;
  variable_const_t off;

  v = value_variable_get(val);
  if (!v) {
    return variable_const_byte_get(value_const_get(val), ofs);
  }
  data  = (const uchar *) variable_data_get(v);
  limit = (unsigned) variable_sz_get(v);
  off   = 0;
  (void) value_baseofs_const_get(val, &off);
  if (data && ((unsigned) off + ofs) < limit) {
    return data[(unsigned) off + ofs];
  }
  if (ofs < (unsigned) sizeof(variable_const_t)) {
    return variable_const_byte_get(value_const_get(val), ofs);
  }
  return 0;
}

/* Compile-time byte `ofs` of val, with sign/zero fill past src_sz.
 * Returns 0..255, or -1 if the byte must be loaded at run time. */
static int mips_wide_known_byte(value_t val, unsigned ofs, unsigned src_sz)
{
  unsigned last;
  unsigned fill;

  if (!val) {
    return 0;
  }
  if (!mips_wide_is_const(val)) {
    return -1;
  }
  if (ofs < src_sz) {
    return (int) mips_wide_const_byte(val, ofs);
  }
  fill = 0;
  if (value_is_signed(val) && src_sz) {
    last = mips_wide_const_byte(val, src_sz - 1);
    if (last & 0x80U) {
      fill = 0xFFU;
    }
  }
  return (int) fill;
}

static void mips_wide_li8(unsigned reg, unsigned ch)
{
  (void) mips_code_alloc(MIPS_OP_ORI, reg, MIPS_REG_ZERO, (int) (ch & 0xFFU));
}

static boolean_t mips_wide_ofs_const(value_t val, variable_const_t *poff)
{
  variable_const_t off;

  off = 0;
  if (!value_baseofs_const_get(val, &off)) {
    return BOOLEAN_FALSE;
  }
  if (poff) {
    *poff = off;
  }
  return BOOLEAN_TRUE;
}

static unsigned long mips_wide_ram_addr(value_t val)
{
  variable_const_t off;

  off = 0;
  (void) value_baseofs_const_get(val, &off);
  return mips_var_ram(value_variable_get(val)) + (unsigned long) off;
}

static boolean_t mips_wide_write_through(value_t dst, value_t src)
{
  if (!dst || !src) {
    return BOOLEAN_FALSE;
  }
  /* Array → pointer is an address bind, not a deep write-through.
   * Writing through an unset open-array formal corrupts low RAM. */
  if (!value_is_pointer(dst) || value_is_pointer(src)
      || value_is_value(src) || value_is_label(src)
      || value_is_array(src)) {
    return BOOLEAN_FALSE;
  }
  if (mips_value_is_ptr_deref(dst)) {
    return BOOLEAN_FALSE;
  }
  return BOOLEAN_TRUE;
}

/* Address of byte 0 of val -> areg.  Does not use $at. */
static void mips_wide_lea(pfile_t *pf, value_t val, unsigned areg,
    unsigned scratch, boolean_t thru)
{
  value_t          b;
  variable_const_t off;
  unsigned         basereg;
  int              lo;

  mips_alloc_scratch_clobber(pf, areg);
  mips_value_ensure(pf, val);
  thru = (thru || mips_value_is_ptr_deref(val)) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  b    = value_baseofs_get(val);
  if (thru) {
    if (b && !mips_wide_ofs_const(val, &off)) {
      mips_load_value(pf, b, scratch);
      lo = mips_emit_addr_base(pf, mips_var_ram(value_variable_get(val)), &basereg);
      (void) mips_code_alloc(MIPS_OP_LW, areg, basereg, lo);
      (void) mips_code_r_alloc(MIPS_OP_ADDU, areg, areg, scratch, 0);
    } else {
      lo = mips_emit_addr_base(pf, mips_var_ram(value_variable_get(val)), &basereg);
      (void) mips_code_alloc(MIPS_OP_LW, areg, basereg, lo);
      if (b && value_is_const(b)) {
        mips_add_const_ofs(areg, scratch, value_const_get(b));
      }
    }
    return;
  }
  if (b && !mips_wide_ofs_const(val, &off)) {
    mips_load_value(pf, b, scratch);
    mips_emit_var_addr(areg, value_variable_get(val), 0);
    (void) mips_code_r_alloc(MIPS_OP_ADDU, areg, areg, scratch, 0);
  } else {
    off = 0;
    (void) value_baseofs_const_get(val, &off);
    mips_emit_var_addr(areg, value_variable_get(val), (unsigned long) off);
  }
}

static boolean_t mips_wide_can_abs(value_t val, boolean_t thru)
{
  variable_const_t off;

  if (thru || mips_value_is_ptr_deref(val)) {
    return BOOLEAN_FALSE;
  }
  if (mips_wide_is_const(val)) {
    return BOOLEAN_FALSE;
  }
  if (mips_var_is_flash_const(value_variable_get(val))) {
    return BOOLEAN_FALSE;
  }
  return mips_wide_ofs_const(val, &off);
}

static void mips_wide_sign_fill(unsigned dreg)
{
  (void) mips_code_r_alloc(MIPS_OP_SRL, dreg, 0, dreg, 7);
  (void) mips_code_alloc(MIPS_OP_ANDI, dreg, dreg, 1);
  (void) mips_code_r_alloc(MIPS_OP_SUBU, dreg, MIPS_REG_ZERO, dreg, 0);
  (void) mips_code_alloc(MIPS_OP_ANDI, dreg, dreg, 0xFF);
}

/* Load byte `ofs` of val into dreg.  scratch is a free GPR (not dreg, not
 * $at -- callers may hold carry in $at). */
static void mips_wide_load_byte(pfile_t *pf, value_t val, unsigned ofs,
    unsigned src_sz, unsigned dreg, unsigned scratch, boolean_t thru)
{
  int              known;
  unsigned         basereg;
  int              lo;
  unsigned long    addr;

  if (!val) {
    mips_wide_li8(dreg, 0);
    return;
  }
  known = mips_wide_known_byte(val, ofs, src_sz);
  if (known >= 0) {
    mips_wide_li8(dreg, (unsigned) known);
    return;
  }
  if (ofs >= src_sz) {
    if (value_is_signed(val) && src_sz) {
      mips_wide_load_byte(pf, val, src_sz - 1, src_sz, dreg, scratch, thru);
      mips_wide_sign_fill(dreg);
    } else {
      mips_wide_li8(dreg, 0);
    }
    return;
  }
  if (mips_wide_can_abs(val, thru)) {
    addr = mips_wide_ram_addr(val) + (unsigned long) ofs;
    lo   = mips_emit_addr_base(pf, addr, &basereg);
    (void) mips_code_alloc(MIPS_OP_LBU, dreg, basereg, lo);
    return;
  }
  mips_wide_lea(pf, val, MIPS_REG_T0, scratch, thru);
  mips_add_const_ofs(MIPS_REG_T0, scratch, (variable_const_t) ofs);
  (void) mips_code_alloc(MIPS_OP_LBU, dreg, MIPS_REG_T0, 0);
}

static void mips_wide_store_byte(pfile_t *pf, value_t val, unsigned ofs,
    unsigned sreg, unsigned scratch, boolean_t thru)
{
  unsigned      basereg;
  int           lo;
  unsigned long addr;

  if (mips_wide_can_abs(val, thru)) {
    addr = mips_wide_ram_addr(val) + (unsigned long) ofs;
    lo   = mips_emit_addr_base(pf, addr, &basereg);
    (void) mips_code_alloc(MIPS_OP_SB, sreg, basereg, lo);
    return;
  }
  mips_wide_lea(pf, val, MIPS_REG_T0, scratch, thru);
  mips_add_const_ofs(MIPS_REG_T0, scratch, (variable_const_t) ofs);
  (void) mips_code_alloc(MIPS_OP_SB, sreg, MIPS_REG_T0, 0);
}

static unsigned mips_wide_chunk_sz(unsigned long addr, unsigned remain)
{
  if ((remain >= 4) && (0 == (addr & 3UL))) {
    return 4;
  }
  if ((remain >= 2) && (0 == (addr & 1UL))) {
    return 2;
  }
  return 1;
}

static unsigned mips_wide_pack(value_t src, unsigned src_sz, unsigned off,
    unsigned csz)
{
  unsigned w;
  unsigned i;
  int      b;

  w = 0;
  for (i = 0; i < csz; i++) {
    b = mips_wide_known_byte(src, off + i, src_sz);
    if (b < 0) {
      b = 0;
    }
    w |= ((unsigned) b) << (8 * i);
  }
  return w;
}

static void mips_wide_store_chunk(pfile_t *pf, unsigned long addr,
    unsigned csz, unsigned w)
{
  unsigned basereg;
  int      lo;

  if (4 == csz) {
    mips_emit_imm32(MIPS_REG_T1, (unsigned long) w);
  } else if (2 == csz) {
    (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T1, MIPS_REG_ZERO,
        (int) (w & 0xFFFFU));
  } else {
    mips_wide_li8(MIPS_REG_T1, w & 0xFFU);
  }
  lo = mips_emit_addr_base(pf, addr, &basereg);
  if (4 == csz) {
    (void) mips_code_alloc(MIPS_OP_SW, MIPS_REG_T1, basereg, lo);
  } else if (2 == csz) {
    (void) mips_code_alloc(MIPS_OP_SH, MIPS_REG_T1, basereg, lo);
  } else {
    (void) mips_code_alloc(MIPS_OP_SB, MIPS_REG_T1, basereg, lo);
  }
}

void mips_wide_assign(pfile_t *pf, value_t dst, value_t src)
{
  unsigned   dst_sz;
  unsigned   src_sz;
  unsigned   off;
  boolean_t  thru;
  boolean_t  dst_abs;
  boolean_t  src_abs;

  if (!dst || !src) {
    return;
  }
  mips_alloc_scratch_clobber(pf, MIPS_REG_T0);
  if (value_is_const(dst) || value_is_label(dst) || value_is_function(dst)) {
    return;
  }
  if (value_is_same(dst, src)) {
    return;
  }
  thru   = mips_wide_write_through(dst, src);
  dst_sz = thru ? mips_wide_val_sz(src) : mips_wide_val_sz(dst);
  src_sz = mips_wide_val_sz(src);
  if (!dst_sz) {
    dst_sz = (unsigned) value_sz_get(dst);
  }
  dst_abs = mips_wide_can_abs(dst, thru);
  src_abs = mips_wide_can_abs(src, BOOLEAN_FALSE);

  if (dst_abs && mips_wide_is_const(src)) {
    off = 0;
    while (off < dst_sz) {
      unsigned long addr;
      unsigned      csz;
      unsigned      w;

      addr = mips_wide_ram_addr(dst) + (unsigned long) off;
      csz  = mips_wide_chunk_sz(addr, dst_sz - off);
      w    = mips_wide_pack(src, src_sz, off, csz);
      mips_wide_store_chunk(pf, addr, csz, w);
      off += csz;
    }
    return;
  }

  if (dst_abs && src_abs) {
    off = 0;
    while (off < dst_sz) {
      unsigned long da;
      unsigned long sa;
      unsigned      csz;
      unsigned      scsz;
      unsigned      basereg;
      int           lo;
      mips_opcode_t lop;
      mips_opcode_t sop;

      da   = mips_wide_ram_addr(dst) + (unsigned long) off;
      sa   = mips_wide_ram_addr(src) + (unsigned long) off;
      csz  = mips_wide_chunk_sz(da, dst_sz - off);
      scsz = (off < src_sz)
          ? mips_wide_chunk_sz(sa, src_sz - off)
          : 1;
      if (scsz < csz) {
        csz = scsz;
      }
      if (off >= src_sz) {
        mips_wide_load_byte(pf, src, off, src_sz, MIPS_REG_T1,
            MIPS_REG_T2, BOOLEAN_FALSE);
        mips_wide_store_byte(pf, dst, off, MIPS_REG_T1, MIPS_REG_T2, thru);
        off++;
        continue;
      }
      lop = (4 == csz) ? MIPS_OP_LW : ((2 == csz) ? MIPS_OP_LHU : MIPS_OP_LBU);
      sop = (4 == csz) ? MIPS_OP_SW : ((2 == csz) ? MIPS_OP_SH  : MIPS_OP_SB);
      lo  = mips_emit_addr_base(pf, sa, &basereg);
      (void) mips_code_alloc(lop, MIPS_REG_T1, basereg, lo);
      lo  = mips_emit_addr_base(pf, da, &basereg);
      (void) mips_code_alloc(sop, MIPS_REG_T1, basereg, lo);
      off += csz;
    }
    return;
  }

  for (off = 0; off < dst_sz; off++) {
    mips_wide_load_byte(pf, src, off, src_sz, MIPS_REG_T1, MIPS_REG_T2,
        BOOLEAN_FALSE);
    mips_wide_store_byte(pf, dst, off, MIPS_REG_T1, MIPS_REG_T2, thru);
  }
}

/* ---------- shared wide ALU/NEG/INCR-DECR primitives (wide_alg) ----
 * The loop structure lives in wide_alg.c; these implement the MIPS
 * idioms it drives.  mips_wide_add_sub / mips_wide_logic (and the
 * add_sub v2_mode INCR/DECR/NEG modes) are gone: ADD/SUB/logic route
 * through walg_alu, NEG/INCR/DECR through their own kernel loops.
 * One emission change: the add chain extracts its carry (SRL $at)
 * before the store instead of after -- the two are independent. */

static unsigned mips_walg_alu_width(pfile_t *pf, value_t dst, value_t v1)
{
  UNUSED(pf);
  if (mips_wide_write_through(dst, v1)) {
    return mips_wide_val_sz(v1);
  }
  return mips_wide_val_sz(dst);
}

/* clear the carry for the add/sub chains (the logic ops need none) */
static void mips_walg_alu_prologue(pfile_t *pf, operator_t op,
    value_t dst, value_t v1, value_t v2)
{
  UNUSED(pf);
  UNUSED(dst);
  UNUSED(v1);
  UNUSED(v2);
  if ((OPERATOR_ADD == op) || (OPERATOR_SUB == op)) {
    (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_AT, MIPS_REG_ZERO,
        MIPS_REG_ZERO, 0);
  }
}

static void mips_walg_alu_load_op1(pfile_t *pf, value_t v1, unsigned k,
    unsigned s1)
{
  mips_wide_load_byte(pf, v1, k, s1, MIPS_REG_T1, MIPS_REG_T3,
      BOOLEAN_FALSE);
}

static void mips_walg_alu_load_op2(pfile_t *pf, value_t v2, unsigned k,
    unsigned s2)
{
  mips_wide_load_byte(pf, v2, k, s2, MIPS_REG_T2, MIPS_REG_T3,
      BOOLEAN_FALSE);
}

/* sum in $t1, carry out in $at (bit 8) */
static void mips_walg_alu_step_add(void)
{
  (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_T1, MIPS_REG_T1,
      MIPS_REG_T2, 0);
  (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_T1, MIPS_REG_T1,
      MIPS_REG_AT, 0);
  (void) mips_code_r_alloc(MIPS_OP_SRL, MIPS_REG_AT, 0, MIPS_REG_T1, 8);
}

/* borrow: (a < b) | ((a-b) < bin); difference in $t1, borrow in $at */
static void mips_walg_alu_step_sub(void)
{
  (void) mips_code_r_alloc(MIPS_OP_SLTU, MIPS_REG_T3, MIPS_REG_T1,
      MIPS_REG_T2, 0);
  (void) mips_code_r_alloc(MIPS_OP_SUBU, MIPS_REG_T1, MIPS_REG_T1,
      MIPS_REG_T2, 0);
  (void) mips_code_r_alloc(MIPS_OP_SLTU, MIPS_REG_T2, MIPS_REG_T1,
      MIPS_REG_AT, 0);
  (void) mips_code_r_alloc(MIPS_OP_SUBU, MIPS_REG_T1, MIPS_REG_T1,
      MIPS_REG_AT, 0);
  (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_AT, MIPS_REG_T3,
      MIPS_REG_T2, 0);
}

static void mips_walg_alu_step_logic(operator_t op)
{
  if (OPERATOR_ANDB == op) {
    (void) mips_code_r_alloc(MIPS_OP_AND, MIPS_REG_T1, MIPS_REG_T1,
        MIPS_REG_T2, 0);
  } else if (OPERATOR_ORB == op) {
    (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_T1, MIPS_REG_T1,
        MIPS_REG_T2, 0);
  } else if (OPERATOR_CMPB == op) {
    (void) mips_code_alloc(MIPS_OP_XORI, MIPS_REG_T1, MIPS_REG_T1, 0xFF);
  } else {
    (void) mips_code_r_alloc(MIPS_OP_XOR, MIPS_REG_T1, MIPS_REG_T1,
        MIPS_REG_T2, 0);
  }
}

static void mips_walg_alu_store(pfile_t *pf, value_t dst, unsigned k,
    value_t v1)
{
  mips_wide_store_byte(pf, dst, k, MIPS_REG_T1, MIPS_REG_T3,
      mips_wide_write_through(dst, v1));
}

/* dst = -v1 as 0 - x per byte (the old add_sub NEG mode) */
static void mips_walg_neg_prologue(pfile_t *pf, value_t dst, value_t v1)
{
  UNUSED(pf);
  UNUSED(dst);
  UNUSED(v1);
  (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_AT, MIPS_REG_ZERO,
      MIPS_REG_ZERO, 0);
}

static void mips_walg_neg_step(pfile_t *pf, value_t v1, unsigned k,
    unsigned s1)
{
  mips_wide_li8(MIPS_REG_T1, 0);
  mips_wide_load_byte(pf, v1, k, s1, MIPS_REG_T2, MIPS_REG_T3,
      BOOLEAN_FALSE);
  mips_walg_alu_step_sub();
}

/* dst = v1 +/- 1: the one-byte operand is 1 at byte 0, 0 above (the
 * old add_sub INCR/DECR shared v2_mode) */
static void mips_walg_incr_prologue(pfile_t *pf, value_t dst, value_t v1)
{
  UNUSED(pf);
  UNUSED(dst);
  UNUSED(v1);
  (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_AT, MIPS_REG_ZERO,
      MIPS_REG_ZERO, 0);
}

static void mips_walg_incr_decr_step(pfile_t *pf, operator_t op,
    value_t v1, unsigned k, unsigned s1)
{
  UNUSED(pf);
  UNUSED(v1);
  UNUSED(s1);
  mips_wide_li8(MIPS_REG_T2, (0 == k) ? 1U : 0U);
  if (OPERATOR_INCR == op) {
    mips_walg_alu_step_add();
  } else {
    mips_walg_alu_step_sub();
  }
}

static unsigned mips_wide_fill_const(value_t src, unsigned src_sz,
    boolean_t arith)
{
  unsigned last;

  if (!arith || !src_sz) {
    return 0;
  }
  if (mips_wide_is_const(src)) {
    last = mips_wide_const_byte(src, src_sz - 1);
    return (last & 0x80U) ? 0xFFU : 0U;
  }
  if (!value_is_signed(src)) {
    return 0;
  }
  last = 0xFFU; /* runtime: caller loads the sign */
  return last;
}

/* ---------- shared wide SHIFT primitives (libcore wide_alg) ---------
 * The saturation / window / walk-order policy lives in wide_alg.c;
 * these replay the MIPS idioms exactly (this back end keeps its
 * pre-kernel emission). */

static const walg_ops_t mips_walg_ops;

static unsigned mips_walg_shift_width(pfile_t *pf, value_t dst,
    value_t src)
{
  unsigned n;

  UNUSED(pf);
  if (mips_wide_write_through(dst, src)) {
    return mips_wide_val_sz(src);
  }
  n = (unsigned) value_byte_sz_get(dst);
  if (!n) {
    n = mips_wide_val_sz(dst);
  }
  return n;
}

/* saturation loops: dst = 0 / dst = fill / dst = sign byte of src */
static void mips_walg_shift_zero_all(pfile_t *pf, value_t dst,
    unsigned n, value_t src)
{
  unsigned i;

  for (i = 0; i < n; i++) {
    mips_wide_li8(MIPS_REG_T1, 0);
    mips_wide_store_byte(pf, dst, i, MIPS_REG_T1, MIPS_REG_T3,
        mips_wide_write_through(dst, src));
  }
}

static void mips_walg_shift_fill_all_const(pfile_t *pf, value_t dst,
    unsigned n, value_t src, unsigned fill)
{
  unsigned i;

  for (i = 0; i < n; i++) {
    mips_wide_li8(MIPS_REG_T1, fill);
    mips_wide_store_byte(pf, dst, i, MIPS_REG_T1, MIPS_REG_T3,
        mips_wide_write_through(dst, src));
  }
}

static void mips_walg_shift_fill_all_runtime(pfile_t *pf, value_t dst,
    unsigned n, value_t src, unsigned src_sz)
{
  unsigned i;

  mips_wide_load_byte(pf, src, src_sz - 1U, src_sz, MIPS_REG_T1,
      MIPS_REG_T3, BOOLEAN_FALSE);
  mips_wide_sign_fill(MIPS_REG_T1);
  for (i = 0; i < n; i++) {
    mips_wide_store_byte(pf, dst, i, MIPS_REG_T1, MIPS_REG_T3,
        mips_wide_write_through(dst, src));
  }
}

static void mips_walg_shift_const_prologue(pfile_t *pf, value_t dst,
    value_t src)
{
  /* The const-shift byte loop clobbers $t0-$t3 as raw scratch (lea,
   * byte loads, merge ors) without touching the value cache -- drop
   * every binding to those registers up front, or a later cached read
   * (e.g. a for-loop count assigned just before the shift) reuses the
   * pre-shift register content: the float-str battery's renderer
   * skipped its entire x5^t scaling loop that way (the count fetch
   * was elided by a binding the shift had made stale). */
  mips_reg_cache_clobber(MIPS_REG_T0);
  mips_reg_cache_clobber(MIPS_REG_T1);
  mips_reg_cache_clobber(MIPS_REG_T2);
  mips_reg_cache_clobber(MIPS_REG_T3);
  UNUSED(pf);
  UNUSED(dst);
  UNUSED(src);
}

/* one dest byte i: window src[lo_i] into $t1 (and src[hi_i] into $t2
 * when the bit split straddles), combine, mask, store */
static void mips_walg_shift_const_byte(pfile_t *pf, value_t dst,
    value_t src, int i, int lo_i, int hi_i, unsigned bit, unsigned fill,
    boolean_t arith, boolean_t left, unsigned src_sz)
{
  boolean_t thru = mips_wide_write_through(dst, src);

  if ((lo_i < 0) || ((unsigned) lo_i >= src_sz)) {
    if (arith && !left && (lo_i >= (int) src_sz)) {
      if (mips_wide_is_const(src)) {
        mips_wide_li8(MIPS_REG_T1, fill);
      } else {
        mips_wide_load_byte(pf, src, src_sz - 1U, src_sz, MIPS_REG_T1,
            MIPS_REG_T3, BOOLEAN_FALSE);
        mips_wide_sign_fill(MIPS_REG_T1);
      }
    } else {
      mips_wide_li8(MIPS_REG_T1, 0);
    }
  } else {
    mips_wide_load_byte(pf, src, (unsigned) lo_i, src_sz, MIPS_REG_T1,
        MIPS_REG_T3, BOOLEAN_FALSE);
  }
  if (bit) {
    if ((hi_i < 0) || ((unsigned) hi_i >= src_sz)) {
      if (arith && !left && (hi_i >= (int) src_sz)) {
        if (mips_wide_is_const(src)) {
          mips_wide_li8(MIPS_REG_T2, fill);
        } else {
          mips_wide_load_byte(pf, src, src_sz - 1U, src_sz, MIPS_REG_T2,
              MIPS_REG_T3, BOOLEAN_FALSE);
          mips_wide_sign_fill(MIPS_REG_T2);
        }
      } else {
        mips_wide_li8(MIPS_REG_T2, 0);
      }
    } else {
      mips_wide_load_byte(pf, src, (unsigned) hi_i, src_sz, MIPS_REG_T2,
          MIPS_REG_T3, BOOLEAN_FALSE);
    }
    if (left) {
      (void) mips_code_r_alloc(MIPS_OP_SLL, MIPS_REG_T1, 0, MIPS_REG_T1,
          (int) bit);
      (void) mips_code_r_alloc(MIPS_OP_SRL, MIPS_REG_T2, 0, MIPS_REG_T2,
          (int) (8U - bit));
    } else {
      (void) mips_code_r_alloc(MIPS_OP_SRL, MIPS_REG_T1, 0, MIPS_REG_T1,
          (int) bit);
      (void) mips_code_r_alloc(MIPS_OP_SLL, MIPS_REG_T2, 0, MIPS_REG_T2,
          (int) (8U - bit));
    }
    (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_T1, MIPS_REG_T1,
        MIPS_REG_T2, 0);
  }
  (void) mips_code_alloc(MIPS_OP_ANDI, MIPS_REG_T1, MIPS_REG_T1, 0xFF);
  mips_wide_store_byte(pf, dst, (unsigned) i, MIPS_REG_T1, MIPS_REG_T3,
      thru);
}

/* Runtime shift: per dest byte, extract 8 bits starting at
 * bit (i*8 + count) of src (SHR) or (i*8 - count) of src (SHL).
 * $t2 = count, $t4 = byte index, $t5 = bit, $t6 = lo, $t7 = hi.
 * Loop order (left: descending) lives in wide_alg.c. */

static void mips_walg_shift_var_load_count(pfile_t *pf, value_t cntv)
{
  if (mips_wide_val_sz(cntv) > 4) {
    mips_wide_load_byte(pf, cntv, 0, mips_wide_val_sz(cntv),
        MIPS_REG_T2, MIPS_REG_T3, BOOLEAN_FALSE);
  } else {
    mips_load_value(pf, cntv, MIPS_REG_T2);
  }
}

static void mips_walg_shift_var_prologue(pfile_t *pf, value_t dst,
    value_t src)
{
  UNUSED(pf);
  UNUSED(dst);
  UNUSED(src);
}

static void mips_walg_shift_var_byte(pfile_t *pf, operator_t op,
    value_t dst, int i, value_t src, unsigned src_sz)
{
  boolean_t left  = (OPERATOR_SHIFT_LEFT == op) ? BOOLEAN_TRUE
      : BOOLEAN_FALSE;
  boolean_t arith = (OPERATOR_SHIFT_RIGHT_ARITHMETIC == op)
      ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  boolean_t thru  = mips_wide_write_through(dst, src);
  mips_code_t br_lo;
  mips_code_t br_hi;

  if (left) {
    (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_AT, MIPS_REG_ZERO,
        (int) ((unsigned) i * 8U));
    (void) mips_code_r_alloc(MIPS_OP_SUBU, MIPS_REG_AT, MIPS_REG_AT,
        MIPS_REG_T2, 0);
  } else {
    (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_AT, MIPS_REG_T2,
        (int) ((unsigned) i * 8U));
  }
  (void) mips_code_r_alloc(MIPS_OP_SRA, MIPS_REG_T4, 0, MIPS_REG_AT, 3);
  (void) mips_code_alloc(MIPS_OP_ANDI, MIPS_REG_T5, MIPS_REG_AT, 7);

  if (arith && !left) {
    mips_wide_load_byte(pf, src, src_sz ? src_sz - 1 : 0, src_sz,
        MIPS_REG_T6, MIPS_REG_T3, BOOLEAN_FALSE);
    mips_wide_sign_fill(MIPS_REG_T6);
  } else {
    mips_wide_li8(MIPS_REG_T6, 0);
  }
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T0, MIPS_REG_ZERO,
      (int) src_sz);
  (void) mips_code_r_alloc(MIPS_OP_SLTU, MIPS_REG_T0, MIPS_REG_T4,
      MIPS_REG_T0, 0);
  br_lo = mips_code_alloc(MIPS_OP_BEQ, MIPS_REG_T0, MIPS_REG_ZERO, 0);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
  mips_wide_lea(pf, src, MIPS_REG_T0, MIPS_REG_T1, BOOLEAN_FALSE);
  (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_T0, MIPS_REG_T0,
      MIPS_REG_T4, 0);
  (void) mips_code_alloc(MIPS_OP_LBU, MIPS_REG_T6, MIPS_REG_T0, 0);
  if (br_lo) {
    br_lo->target = mips_code_label_alloc();
  }

  if (arith && !left) {
    mips_wide_load_byte(pf, src, src_sz ? src_sz - 1 : 0, src_sz,
        MIPS_REG_T7, MIPS_REG_T3, BOOLEAN_FALSE);
    mips_wide_sign_fill(MIPS_REG_T7);
  } else {
    mips_wide_li8(MIPS_REG_T7, 0);
  }
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T0, MIPS_REG_T4, 1);
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1, MIPS_REG_ZERO,
      (int) src_sz);
  (void) mips_code_r_alloc(MIPS_OP_SLTU, MIPS_REG_T1, MIPS_REG_T0,
      MIPS_REG_T1, 0);
  br_hi = mips_code_alloc(MIPS_OP_BEQ, MIPS_REG_T1, MIPS_REG_ZERO, 0);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
  mips_wide_lea(pf, src, MIPS_REG_T0, MIPS_REG_T1, BOOLEAN_FALSE);
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1, MIPS_REG_T4, 1);
  (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_T0, MIPS_REG_T0,
      MIPS_REG_T1, 0);
  (void) mips_code_alloc(MIPS_OP_LBU, MIPS_REG_T7, MIPS_REG_T0, 0);
  if (br_hi) {
    br_hi->target = mips_code_label_alloc();
  }

  /* dest bit b comes from source bit bitbase+b for either direction,
   * so the combine is (lo >> bit) | (hi << (8 - bit)) for BOTH left
   * and right */
  (void) mips_code_r_alloc(MIPS_OP_SRLV, MIPS_REG_T6, MIPS_REG_T5,
      MIPS_REG_T6, 0);
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T0, MIPS_REG_ZERO, 8);
  (void) mips_code_r_alloc(MIPS_OP_SUBU, MIPS_REG_T0, MIPS_REG_T0,
      MIPS_REG_T5, 0);
  (void) mips_code_r_alloc(MIPS_OP_SLLV, MIPS_REG_T7, MIPS_REG_T0,
      MIPS_REG_T7, 0);
  (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_T1, MIPS_REG_T6,
      MIPS_REG_T7, 0);
  (void) mips_code_alloc(MIPS_OP_ANDI, MIPS_REG_T1, MIPS_REG_T1, 0xFF);
  mips_wide_store_byte(pf, dst, (unsigned) i, MIPS_REG_T1, MIPS_REG_T3,
      thru);
}

static void mips_wide_shift(pfile_t *pf, operator_t op, value_t dst,
    value_t src, value_t cntv)
{
  if (!cntv || mips_wide_is_const(cntv)) {
    unsigned count;

    count = cntv ? (unsigned) value_const_get(cntv) : 0;
    walg_shift_const(pf, &mips_walg_ops, op, dst, src, count);
    return;
  }
  walg_shift_var(pf, &mips_walg_ops, op, dst, src, cntv);
}

/* XOR-OR all bytes into $t1 (0 iff equal). */
static void mips_wide_xor_acc(pfile_t *pf, value_t v1, value_t v2)
{
  unsigned s1;
  unsigned s2;
  unsigned n;
  unsigned ofs;

  s1 = mips_wide_val_sz(v1);
  s2 = mips_wide_val_sz(v2);
  n  = (s1 > s2) ? s1 : s2;
  (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_AT, MIPS_REG_ZERO,
      MIPS_REG_ZERO, 0);
  for (ofs = 0; ofs < n; ofs++) {
    mips_wide_load_byte(pf, v1, ofs, s1, MIPS_REG_T1, MIPS_REG_T3,
        BOOLEAN_FALSE);
    mips_wide_load_byte(pf, v2, ofs, s2, MIPS_REG_T2, MIPS_REG_T3,
        BOOLEAN_FALSE);
    (void) mips_code_r_alloc(MIPS_OP_XOR, MIPS_REG_T1, MIPS_REG_T1,
        MIPS_REG_T2, 0);
    (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_AT, MIPS_REG_AT,
        MIPS_REG_T1, 0);
  }
  (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_T1, MIPS_REG_AT,
      MIPS_REG_ZERO, 0);
}

/* Unsigned v1 < v2 -> $t1 (0/1) via borrow. */
static void mips_wide_ult(pfile_t *pf, value_t v1, value_t v2)
{
  unsigned s1;
  unsigned s2;
  unsigned n;
  unsigned ofs;

  s1 = mips_wide_val_sz(v1);
  s2 = mips_wide_val_sz(v2);
  n  = (s1 > s2) ? s1 : s2;
  (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_AT, MIPS_REG_ZERO,
      MIPS_REG_ZERO, 0);
  for (ofs = 0; ofs < n; ofs++) {
    mips_wide_load_byte(pf, v1, ofs, s1, MIPS_REG_T1, MIPS_REG_T3,
        BOOLEAN_FALSE);
    mips_wide_load_byte(pf, v2, ofs, s2, MIPS_REG_T2, MIPS_REG_T3,
        BOOLEAN_FALSE);
    (void) mips_code_r_alloc(MIPS_OP_SLTU, MIPS_REG_T3, MIPS_REG_T1,
        MIPS_REG_T2, 0);
    (void) mips_code_r_alloc(MIPS_OP_SUBU, MIPS_REG_T1, MIPS_REG_T1,
        MIPS_REG_T2, 0);
    (void) mips_code_r_alloc(MIPS_OP_SLTU, MIPS_REG_T2, MIPS_REG_T1,
        MIPS_REG_AT, 0);
    (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_AT, MIPS_REG_T3,
        MIPS_REG_T2, 0);
  }
  (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_T1, MIPS_REG_AT,
      MIPS_REG_ZERO, 0);
}

static void mips_wide_slt(pfile_t *pf, value_t v1, value_t v2)
{
  unsigned s1;
  unsigned s2;
  unsigned n;
  boolean_t sgn;

  s1  = mips_wide_val_sz(v1);
  s2  = mips_wide_val_sz(v2);
  n   = (s1 > s2) ? s1 : s2;
  sgn = (value_is_signed(v1) || value_is_signed(v2)) ? BOOLEAN_TRUE
      : BOOLEAN_FALSE;
  mips_wide_ult(pf, v1, v2); /* $t1 = unsigned < */
  if (!sgn || !n) {
    return;
  }
  /* signed_lt = (sa ^ sb) ? sa : unsigned_lt
   * sa = sign of high byte of v1, etc. */
  (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_AT, MIPS_REG_T1,
      MIPS_REG_ZERO, 0); /* save unsigned_lt */
  mips_wide_load_byte(pf, v1, n - 1, s1, MIPS_REG_T1, MIPS_REG_T3,
      BOOLEAN_FALSE);
  (void) mips_code_r_alloc(MIPS_OP_SRL, MIPS_REG_T1, 0, MIPS_REG_T1, 7);
  (void) mips_code_alloc(MIPS_OP_ANDI, MIPS_REG_T1, MIPS_REG_T1, 1);
  mips_wide_load_byte(pf, v2, n - 1, s2, MIPS_REG_T2, MIPS_REG_T3,
      BOOLEAN_FALSE);
  (void) mips_code_r_alloc(MIPS_OP_SRL, MIPS_REG_T2, 0, MIPS_REG_T2, 7);
  (void) mips_code_alloc(MIPS_OP_ANDI, MIPS_REG_T2, MIPS_REG_T2, 1);
  (void) mips_code_r_alloc(MIPS_OP_XOR, MIPS_REG_T3, MIPS_REG_T1,
      MIPS_REG_T2, 0); /* signs differ */
  (void) mips_code_r_alloc(MIPS_OP_AND, MIPS_REG_T1, MIPS_REG_T3,
      MIPS_REG_T1, 0); /* differ & sa */
  (void) mips_code_alloc(MIPS_OP_XORI, MIPS_REG_T3, MIPS_REG_T3, 1);
  (void) mips_code_r_alloc(MIPS_OP_AND, MIPS_REG_T3, MIPS_REG_T3,
      MIPS_REG_AT, 0); /* same & ult */
  (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_T1, MIPS_REG_T1,
      MIPS_REG_T3, 0);
}

/* Relation result in $t1.  EQ/NE leave $t1 = xor-or (0 iff equal);
 * ordered compares leave 0/1.  *eq_raw is TRUE for EQ/NE. */
/* ---------- shared wide RELATION primitives (libcore wide_alg) -------
 * The relation mapping (chain choice, operand order, inversion) and
 * branch-sense logic live in wide_alg.c; these replay the MIPS
 * idioms.  Two emission improvements from the kernel's uniform
 * branch-sense decomposition: GE/LE fused compares no longer XORI
 * the flag, and the not-branch no longer normalizes + inverts (the
 * polarity rides the branch sense). */

static const walg_ops_t mips_walg_ops;

/* $t1 = (v1 != 0) */
static void mips_walg_or_self(pfile_t *pf, value_t v1)
{
  unsigned s = mips_wide_val_sz(v1);
  unsigned ofs;

  (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_T1, MIPS_REG_ZERO,
      MIPS_REG_ZERO, 0);
  for (ofs = 0; ofs < s; ofs++) {
    mips_wide_load_byte(pf, v1, ofs, s, MIPS_REG_T2, MIPS_REG_T3,
        BOOLEAN_FALSE);
    (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_T1, MIPS_REG_T1,
        MIPS_REG_T2, 0);
  }
}

static void mips_walg_br_flag_to(pfile_t *pf, boolean_t take_on_nonzero,
    label_t lbl)
{
  UNUSED(pf);
  (void) mips_emit_cond_branch(
      take_on_nonzero ? MIPS_OP_BNE : MIPS_OP_BEQ,
      MIPS_REG_T1, MIPS_REG_ZERO, lbl);
}

/* EQ/NE materialization: the equality accumulator already ran ($t1
 * nonzero iff the values differ) */
static void mips_walg_bool_eq_finish(pfile_t *pf, value_t dst,
    operator_t op)
{
  (void) mips_code_r_alloc(MIPS_OP_SLTU, MIPS_REG_T1, MIPS_REG_ZERO,
      MIPS_REG_T1, 0);
  if (OPERATOR_EQ == op) {
    (void) mips_code_alloc(MIPS_OP_XORI, MIPS_REG_T1, MIPS_REG_T1, 1);
  }
  mips_store_value(pf, dst, MIPS_REG_T1);
}

/* ordered-relation materialization: the lt chain already ran ($t1) */
static void mips_walg_bool_lt_finish(pfile_t *pf, value_t dst,
    boolean_t inverted)
{
  if (inverted) {
    (void) mips_code_alloc(MIPS_OP_XORI, MIPS_REG_T1, MIPS_REG_T1, 1);
  }
  mips_store_value(pf, dst, MIPS_REG_T1);
}

static boolean_t mips_wide_compare_branch(pfile_t *pf, const cmd_t cmd,
    operator_t op, value_t v1, value_t v2)
{
  cmd_t            next;
  cmd_branchcond_t bcond;
  label_t          lbl;
  boolean_t        br_true;

  next = cmd_link_get(cmd);
  if (!next || (CMD_TYPE_BRANCH != cmd_type_get(next))) {
    return BOOLEAN_FALSE;
  }
  if (CMD_BRANCHTYPE_GOTO != cmd_brtype_get(next)) {
    return BOOLEAN_FALSE;
  }
  bcond = cmd_brcond_get(next);
  if (CMD_BRANCHCOND_NONE == bcond) {
    return BOOLEAN_FALSE;
  }
  lbl     = cmd_brdst_get(next);
  br_true = (CMD_BRANCHCOND_TRUE == bcond) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  mips_op_consume_branch(pf, next);
  walg_compare_branch_emit(pf, &mips_walg_ops, op, br_true, lbl, v1, v2);
  return BOOLEAN_TRUE;
}

static void mips_wide_compare_bool(pfile_t *pf, value_t dst, operator_t op,
    value_t v1, value_t v2)
{
  walg_compare_bool(pf, &mips_walg_ops, dst, op, v1, v2);
}

/* dest-less wide value fused with a goto: condition true iff v == 0
 * (not_form, NOTL/CMPB) or v != 0 (truth test); the accumulate and
 * polarity live in wide_alg.c */
static boolean_t mips_wide_value_branch(pfile_t *pf, const cmd_t cmd,
    value_t v1, boolean_t not_form)
{
  cmd_t            next;
  cmd_branchcond_t bcond;
  label_t          lbl;
  boolean_t        br_true;

  next = cmd_link_get(cmd);
  if (!next || (CMD_TYPE_BRANCH != cmd_type_get(next))) {
    return BOOLEAN_FALSE;
  }
  if (CMD_BRANCHTYPE_GOTO != cmd_brtype_get(next)) {
    return BOOLEAN_FALSE;
  }
  bcond = cmd_brcond_get(next);
  if (CMD_BRANCHCOND_NONE == bcond) {
    return BOOLEAN_FALSE;
  }
  lbl     = cmd_brdst_get(next);
  br_true = (CMD_BRANCHCOND_TRUE == bcond) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  mips_op_consume_branch(pf, next);
  walg_value_branch_emit(pf, &mips_walg_ops, not_form, br_true, lbl, v1);
  return BOOLEAN_TRUE;
}

void mips_wide_reset(void)
{
  /* the wide temporaries are kernel-held (libcore wide_alg) */
  walg_reset();
}

static value_t mips_wide_tmp(pfile_t *pf, unsigned n)
{
  variable_def_t def;
  variable_t     v;
  value_t        val;

  def = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
      VARIABLE_DEF_FLAG_NONE, (variable_sz_t) n);
  if (!def) {
    return VALUE_NONE;
  }
  v = variable_alloc(TAG_NONE, def);
  if (!v) {
    return VALUE_NONE;
  }
  mips_var_place(pf, v, (variable_sz_t) n);
  val = value_alloc(v);
  variable_release(v);
  return val;
}

/* the reuse slots live in the shared wide_alg kernel (wide_alg.c) */

static void mips_wide_zero(pfile_t *pf, value_t dst, unsigned n,
    boolean_t thru)
{
  unsigned ofs;

  mips_wide_li8(MIPS_REG_T1, 0);
  for (ofs = 0; ofs < n; ofs++) {
    mips_wide_store_byte(pf, dst, ofs, MIPS_REG_T1, MIPS_REG_T3, thru);
  }
}

/* Two's-complement product truncated to n bytes (unsigned schoolbook).
 * A dst-aliased operand is copied to a temp first: dst is zeroed up
 * front (`q = q * 2` would otherwise multiply zeros). */
static boolean_t mips_wide_aliases(value_t a, value_t b)
{
  variable_t va = a ? value_variable_get(a) : (variable_t) 0;
  variable_t vb = b ? value_variable_get(b) : (variable_t) 0;

  while (va && variable_master_get(va)) {
    va = variable_master_get(va);
  }
  while (vb && variable_master_get(vb)) {
    vb = variable_master_get(vb);
  }
  return (va && vb && (va == vb)) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* ---------- shared wide MUL/DIV primitives (libcore wide_alg) -----
 * The loop structure lives in wide_alg.c; these implement the MIPS
 * idioms it drives.  Sign state for division is held in 1-byte RAM
 * cells (the kernel's convention) instead of live $t6/$t7. */

static unsigned mips_walg_mul_width(pfile_t *pf, value_t dst, value_t v1)
{
  UNUSED(pf);
  if (mips_wide_write_through(dst, v1)) {
    return mips_wide_val_sz(v1);
  }
  return mips_wide_val_sz(dst);
}

static unsigned mips_walg_div_base_width(pfile_t *pf, value_t dst,
    value_t v1)
{
  UNUSED(pf);
  if (mips_wide_write_through(dst, v1)) {
    return mips_wide_val_sz(v1);
  }
  return mips_wide_val_sz(dst);
}

static void mips_walg_zero(pfile_t *pf, value_t val, unsigned n,
    value_t plain)
{
  boolean_t thru;

  thru = plain ? mips_wide_write_through(val, plain) : BOOLEAN_FALSE;
  mips_wide_zero(pf, val, thru ? n : mips_wide_val_sz(val), thru);
}

static void mips_walg_sub_wide(pfile_t *pf, value_t dst, value_t v1,
    value_t v2)
{
  walg_alu(pf, &mips_walg_ops, OPERATOR_SUB, dst, v1, v2);
}

static void mips_walg_shift_left_1(pfile_t *pf, value_t val)
{
  walg_shift_const(pf, &mips_walg_ops, OPERATOR_SHIFT_LEFT, val, val, 1);
}

/* cell[0] = sign bit (0/1) of val's top byte */
static void mips_walg_sign_to_cell(pfile_t *pf, value_t cell, value_t val,
    unsigned n)
{
  mips_wide_load_byte(pf, val, n - 1U, n, MIPS_REG_T1, MIPS_REG_T3,
      BOOLEAN_FALSE);
  (void) mips_code_alloc(MIPS_OP_ANDI, MIPS_REG_T1, MIPS_REG_T1, 0x80);
  (void) mips_code_r_alloc(MIPS_OP_SRL, MIPS_REG_T1, 0, MIPS_REG_T1, 7);
  mips_wide_store_byte(pf, cell, 0, MIPS_REG_T1, MIPS_REG_T3,
      BOOLEAN_FALSE);
}

/* load cell[0] into $t1 and branch past the guarded body when it is 0 */
static walg_br_t mips_walg_cell_bez(pfile_t *pf, value_t cell)
{
  walg_br_t br;

  mips_wide_load_byte(pf, cell, 0, 1, MIPS_REG_T1, MIPS_REG_T3,
      BOOLEAN_FALSE);
  br = mips_code_alloc(MIPS_OP_BEQ, MIPS_REG_T1, MIPS_REG_ZERO, 0);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
  return br;
}

/* $t1 = c1[0] ^ c2[0]; branch when the signs are equal */
static walg_br_t mips_walg_cell_pair_xor_bez(pfile_t *pf, value_t c1,
    value_t c2)
{
  walg_br_t br;

  mips_wide_load_byte(pf, c1, 0, 1, MIPS_REG_T1, MIPS_REG_T3,
      BOOLEAN_FALSE);
  mips_wide_load_byte(pf, c2, 0, 1, MIPS_REG_T2, MIPS_REG_T3,
      BOOLEAN_FALSE);
  (void) mips_code_r_alloc(MIPS_OP_XOR, MIPS_REG_T1, MIPS_REG_T1,
      MIPS_REG_T2, 0);
  br = mips_code_alloc(MIPS_OP_BEQ, MIPS_REG_T1, MIPS_REG_ZERO, 0);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
  return br;
}

/* the MIPS loop re-materializes addresses per use: nothing to pin */
static void mips_walg_loop_prologue(pfile_t *pf, value_t num, value_t rem,
    value_t quot)
{
  UNUSED(pf);
  UNUSED(num);
  UNUSED(rem);
  UNUSED(quot);
}

/* $t1 = bit $t4 of num */
static void mips_walg_bit_load(pfile_t *pf, value_t num)
{
  mips_wide_lea(pf, num, MIPS_REG_T0, MIPS_REG_T3, BOOLEAN_FALSE);
  (void) mips_code_r_alloc(MIPS_OP_SRL, MIPS_REG_T5, 0, MIPS_REG_T4, 3);
  (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_T0, MIPS_REG_T0,
      MIPS_REG_T5, 0);
  (void) mips_code_alloc(MIPS_OP_LBU, MIPS_REG_T1, MIPS_REG_T0, 0);
  (void) mips_code_alloc(MIPS_OP_ANDI, MIPS_REG_T2, MIPS_REG_T4, 7);
  (void) mips_code_r_alloc(MIPS_OP_SRLV, MIPS_REG_T1, MIPS_REG_T2,
      MIPS_REG_T1, 0);
  (void) mips_code_alloc(MIPS_OP_ANDI, MIPS_REG_T1, MIPS_REG_T1, 1);
}

/* rem[0] |= $t1 */
static void mips_walg_bit_or_low(pfile_t *pf, value_t rem)
{
  unsigned n = mips_wide_val_sz(rem);

  mips_wide_load_byte(pf, rem, 0, n, MIPS_REG_T2, MIPS_REG_T3,
      BOOLEAN_FALSE);
  (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_T2, MIPS_REG_T2,
      MIPS_REG_T1, 0);
  mips_wide_store_byte(pf, rem, 0, MIPS_REG_T2, MIPS_REG_T3,
      BOOLEAN_FALSE);
}

/* set bit $t4 of quot to 1 */
static void mips_walg_bit_set(pfile_t *pf, value_t quot)
{
  mips_wide_lea(pf, quot, MIPS_REG_T0, MIPS_REG_T3, BOOLEAN_FALSE);
  (void) mips_code_r_alloc(MIPS_OP_SRL, MIPS_REG_T5, 0, MIPS_REG_T4, 3);
  (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_T0, MIPS_REG_T0,
      MIPS_REG_T5, 0);
  (void) mips_code_alloc(MIPS_OP_LBU, MIPS_REG_T1, MIPS_REG_T0, 0);
  (void) mips_code_alloc(MIPS_OP_ANDI, MIPS_REG_T2, MIPS_REG_T4, 7);
  (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T3, MIPS_REG_ZERO, 1);
  (void) mips_code_r_alloc(MIPS_OP_SLLV, MIPS_REG_T3, MIPS_REG_T2,
      MIPS_REG_T3, 0);
  (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_T1, MIPS_REG_T1,
      MIPS_REG_T3, 0);
  (void) mips_code_alloc(MIPS_OP_SB, MIPS_REG_T1, MIPS_REG_T0, 0);
}

/* zero dst (the byte-loop form is width-exact for both thru modes) */
static void mips_walg_mul_prologue(pfile_t *pf, value_t t1, value_t t2,
    value_t dst, unsigned n)
{
  UNUSED(t2);
  mips_wide_zero(pf, dst, n, mips_wide_write_through(dst, t1));
}

static void mips_walg_mul_row_init(void)
{
  (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_T5, MIPS_REG_ZERO,
      MIPS_REG_ZERO, 0);
}

/* dst[i+j] = t1[i]*t2[j] + dst[i+j] + carry (carry in $t5) */
static void mips_walg_mul_row_step(pfile_t *pf, value_t t1, value_t t2,
    value_t dst, unsigned i, unsigned j, unsigned n)
{
  unsigned  s1 = mips_wide_val_sz(t1);
  unsigned  s2 = mips_wide_val_sz(t2);
  boolean_t thru = mips_wide_write_through(dst, t1);

  mips_wide_load_byte(pf, t1, i, s1, MIPS_REG_T1, MIPS_REG_T3,
      BOOLEAN_FALSE);
  mips_wide_load_byte(pf, t2, j, s2, MIPS_REG_T2, MIPS_REG_T3,
      BOOLEAN_FALSE);
  (void) mips_code_r_alloc(MIPS_OP_MULTU, 0, MIPS_REG_T1, MIPS_REG_T2, 0);
  (void) mips_code_r_alloc(MIPS_OP_MFLO, MIPS_REG_T1, 0, 0, 0);
  mips_wide_load_byte(pf, dst, i + j, n, MIPS_REG_T2, MIPS_REG_T3, thru);
  (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_T1, MIPS_REG_T1,
      MIPS_REG_T2, 0);
  (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_T1, MIPS_REG_T1,
      MIPS_REG_T5, 0);
  mips_wide_store_byte(pf, dst, i + j, MIPS_REG_T1, MIPS_REG_T3, thru);
  (void) mips_code_r_alloc(MIPS_OP_SRL, MIPS_REG_T5, 0, MIPS_REG_T1, 8);
}

static void mips_walg_movi(unsigned reg, int imm)
{
  (void) mips_code_alloc(MIPS_OP_ORI, reg, MIPS_REG_ZERO, imm);
}

static void mips_walg_addi(unsigned reg, int imm)
{
  (void) mips_code_alloc(MIPS_OP_ADDIU, reg, (int) reg, imm);
}

static walg_br_t mips_walg_br_flag_zero(void)
{
  walg_br_t br;

  br = mips_code_alloc(MIPS_OP_BEQ, MIPS_REG_T1, MIPS_REG_ZERO, 0);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
  return br;
}

static walg_br_t mips_walg_br_flag_nonzero(void)
{
  walg_br_t br;

  br = mips_code_alloc(MIPS_OP_BNE, MIPS_REG_T1, MIPS_REG_ZERO, 0);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
  return br;
}

static walg_br_t mips_walg_br_ctr_nonzero(void)
{
  walg_br_t br;

  br = mips_code_alloc(MIPS_OP_BNE, MIPS_REG_T4, MIPS_REG_ZERO, 0);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
  return br;
}

static walg_lab_t mips_walg_label_here(void)
{
  return mips_code_label_alloc();
}

static void mips_walg_br_target(walg_br_t br, walg_lab_t lab)
{
  if (br) {
    ((mips_code_t) br)->target = (mips_code_t) lab;
  }
}

static void mips_wide_neg_inplace(pfile_t *pf, value_t val, unsigned n);
static void mips_wide_or_acc(pfile_t *pf, value_t val, unsigned n);

static const walg_ops_t mips_walg_ops = {
  MIPS_REG_T1,                           /* reg_flag */
  MIPS_REG_T4,                           /* reg_ctr  */
  mips_wide_val_sz,
  mips_walg_mul_width,
  mips_walg_div_base_width,
  mips_wide_aliases,
  mips_wide_tmp,
  mips_wide_assign,
  mips_walg_zero,
  mips_wide_or_acc,
  mips_wide_neg_inplace,
  mips_wide_ult,
  mips_walg_sub_wide,
  mips_walg_shift_left_1,
  mips_walg_sign_to_cell,
  mips_walg_cell_bez,
  mips_walg_cell_pair_xor_bez,
  mips_walg_loop_prologue,
  mips_walg_bit_load,
  mips_walg_bit_or_low,
  mips_walg_bit_set,
  mips_walg_mul_prologue,
  mips_walg_mul_row_init,
  mips_walg_mul_row_step,
  mips_walg_movi,
  mips_walg_addi,
  mips_walg_br_flag_zero,
  mips_walg_br_flag_nonzero,
  mips_walg_br_ctr_nonzero,
  mips_walg_label_here,
  mips_walg_br_target,
  mips_walg_alu_width,
  mips_walg_alu_prologue,
  mips_walg_alu_load_op1,
  mips_walg_alu_load_op2,
  mips_walg_alu_step_add,
  mips_walg_alu_step_sub,
  mips_walg_alu_step_logic,
  mips_walg_alu_store,
  mips_walg_neg_prologue,
  mips_walg_neg_step,
  mips_walg_incr_prologue,
  mips_walg_incr_decr_step,
  mips_walg_shift_width,
  mips_wide_is_const,
  mips_wide_fill_const,
  mips_walg_shift_zero_all,
  mips_walg_shift_fill_all_const,
  mips_walg_shift_fill_all_runtime,
  mips_walg_shift_const_prologue,
  mips_walg_shift_const_byte,
  mips_walg_shift_var_load_count,
  mips_walg_shift_var_prologue,
  mips_walg_shift_var_byte,
  mips_wide_xor_acc,
  mips_wide_slt,
  mips_walg_or_self,
  mips_walg_br_flag_to,
  mips_walg_bool_eq_finish,
  mips_walg_bool_lt_finish
};

static void mips_wide_neg_inplace(pfile_t *pf, value_t val, unsigned n)
{
  unsigned ofs;

  (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_AT, MIPS_REG_ZERO,
      MIPS_REG_ZERO, 0);
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_AT, MIPS_REG_ZERO, 1);
  for (ofs = 0; ofs < n; ofs++) {
    mips_wide_load_byte(pf, val, ofs, n, MIPS_REG_T1, MIPS_REG_T3,
        BOOLEAN_FALSE);
    (void) mips_code_alloc(MIPS_OP_XORI, MIPS_REG_T1, MIPS_REG_T1, 0xFF);
    (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_T1, MIPS_REG_T1,
        MIPS_REG_AT, 0);
    mips_wide_store_byte(pf, val, ofs, MIPS_REG_T1, MIPS_REG_T3,
        BOOLEAN_FALSE);
    (void) mips_code_r_alloc(MIPS_OP_SRL, MIPS_REG_AT, 0, MIPS_REG_T1, 8);
  }
}

static void mips_wide_or_acc(pfile_t *pf, value_t val, unsigned n)
{
  unsigned ofs;

  (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_T1, MIPS_REG_ZERO,
      MIPS_REG_ZERO, 0);
  for (ofs = 0; ofs < n; ofs++) {
    mips_wide_load_byte(pf, val, ofs, n, MIPS_REG_T2, MIPS_REG_T3,
        BOOLEAN_FALSE);
    (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_T1, MIPS_REG_T1,
        MIPS_REG_T2, 0);
  }
}

boolean_t mips_wide_generate(pfile_t *pf, operator_t op, value_t dst,
    value_t v1, value_t v2, const cmd_t cmd)
{
  if (!dst && operator_is_relation(op)) {
    return mips_wide_compare_branch(pf, cmd, op, v1, v2);
  }
  if (!dst && ((OPERATOR_CMPB == op) || (OPERATOR_NOTL == op))) {
    return mips_wide_value_branch(pf, cmd, v1, BOOLEAN_TRUE);
  }
  if (!dst && ((OPERATOR_ASSIGN == op) || (OPERATOR_LOGICAL == op))) {
    return mips_wide_value_branch(pf, cmd, v1, BOOLEAN_FALSE);
  }
  if (!dst) {
    return BOOLEAN_FALSE;
  }
  if (operator_is_relation(op)) {
    mips_wide_compare_bool(pf, dst, op, v1, v2);
    return BOOLEAN_TRUE;
  }
  if (OPERATOR_ASSIGN == op) {
    if (!v1) {
      return BOOLEAN_TRUE;
    }
    mips_wide_assign(pf, dst, v1);
    return BOOLEAN_TRUE;
  }
  if (!v1) {
    return BOOLEAN_FALSE;
  }
  switch (op) {
    case OPERATOR_ADD:
      walg_alu(pf, &mips_walg_ops, op, dst, v1, v2);
      return BOOLEAN_TRUE;
    case OPERATOR_SUB:
      walg_alu(pf, &mips_walg_ops, op, dst, v1, v2);
      return BOOLEAN_TRUE;
    case OPERATOR_INCR:
      walg_incr_decr(pf, &mips_walg_ops, op, dst, v1);
      return BOOLEAN_TRUE;
    case OPERATOR_DECR:
      walg_incr_decr(pf, &mips_walg_ops, op, dst, v1);
      return BOOLEAN_TRUE;
    case OPERATOR_NEG:
      walg_neg(pf, &mips_walg_ops, dst, v1);
      return BOOLEAN_TRUE;
    case OPERATOR_ANDB:
    case OPERATOR_ORB:
    case OPERATOR_XORB:
    case OPERATOR_CMPB:
      walg_alu(pf, &mips_walg_ops, op, dst, v1, v2);
      return BOOLEAN_TRUE;
    case OPERATOR_SHIFT_LEFT:
    case OPERATOR_SHIFT_RIGHT:
    case OPERATOR_SHIFT_RIGHT_ARITHMETIC:
      mips_wide_shift(pf, op, dst, v1, v2);
      return BOOLEAN_TRUE;
    case OPERATOR_MUL:
      walg_mul(pf, &mips_walg_ops, dst, v1, v2);
      return BOOLEAN_TRUE;
    case OPERATOR_DIV:
    case OPERATOR_MOD:
      walg_div(pf, &mips_walg_ops, op, dst, v1, v2);
      return BOOLEAN_TRUE;
    case OPERATOR_NOTL: {
      unsigned s;
      unsigned ofs;

      s = mips_wide_val_sz(v1);
      (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_T1, MIPS_REG_ZERO,
          MIPS_REG_ZERO, 0);
      for (ofs = 0; ofs < s; ofs++) {
        mips_wide_load_byte(pf, v1, ofs, s, MIPS_REG_T2, MIPS_REG_T3,
            BOOLEAN_FALSE);
        (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_T1, MIPS_REG_T1,
            MIPS_REG_T2, 0);
      }
      (void) mips_code_r_alloc(MIPS_OP_SLTU, MIPS_REG_T1, MIPS_REG_ZERO,
          MIPS_REG_T1, 0);
      (void) mips_code_alloc(MIPS_OP_XORI, MIPS_REG_T1, MIPS_REG_T1, 1);
      mips_store_value(pf, dst, MIPS_REG_T1);
      return BOOLEAN_TRUE;
    }
    default:
      return BOOLEAN_FALSE;
  }
}
