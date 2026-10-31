/************************************************************
 **
 ** mips_op.c : PIC32 (MIPS32) -- translate cmd_t to mips_code_t
 **
 ** OPERATOR_ASSIGN (const/var -> var) and the binary arithmetic /
 ** logic / shift operators, plus unary NEG/INCR/DECR/CMPB.
 ** Comparisons (EQ/NE/LT/...) and conditional branches; calls and
 ** returns; indirect calls / pointers / WHEREIS / p[i] /
 ** WHEREIS(slots[idx]); `_usec_delay` (`mips_delay.c`, SYSCLK
 ** cycle-count loops); single-bit load/store (bitbucket + const/
 ** runtime bit index); N-byte / wide integer assign + ALU/shift/cmp
 ** (`mips_wide.c`); open `bit in bits[]` formals (normalize buffer +
 ** bit-through-pointer); multi-bit packed fields (`var bit*N`,
 ** overlays) (`mips_load_bitfield` / `mips_store_bitfield`); flash
 ** const pool (string literals / `const` arrays) (`mips_const.c`).
 **
 ** Register convention (fixed scratch roles + BB value cache):
 **   $t0 -- address base for loads/stores (reused per access)
 **   $t1 -- operand 1 / result
 **   $t2 -- operand 2
 **   $t3 -- extra scratch (offsets, bit masks, containing byte)
 **   $t4 -- runtime multi-bit field start (bit index)
 **   $at -- assembler temp (bit insert mask / chunk)
 **   $gp -- MIPS_RAM_BASE
 ** Scalar load/store consult mips_reg_cache_* within a basic block.
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include <string.h>
#include "../libutils/mem.h"
#include "../libcore/pfile.h"
#include "../libcore/cmd.h"
#include "../libcore/cmd_usec.h"
#include "../libcore/cmd_op.h"
#include "../libcore/cmd_brch.h"
#include "../libcore/operator.h"
#include "../libcore/value.h"
#include "../libcore/variable.h"
#include "../libcore/label.h"
#include "../libcore/pf_proc.h"
#include "../libcore/pf_log.h"
#include "../libcore/vardef.h"
#include "mips.h"
#include "mips_reg.h"
#include "mips_alloc.h"

static void mips_add_fixup(mips_code_t branch, label_t lbl);

/* Nesting of frames that save $s3-$s5. Preserve-across-call only
 * writes those homes when a save is active (not tasks / ISR entry). */

#define MIPS_PROC_STACK_CT 32

/*
 * Per-compile state, in ONE object (Tier-A A4; see
 * fix_mips_op_state_reset.md). The module's own mips_op_reset() is the
 * LOGICAL reset used mid-compile; mips_op_state_reset() is the TEARDOWN.
 */
typedef struct mips_op_state_ {
  label_t            mvec_hold[67];
  unsigned           mvec_hold_ct;
  unsigned           s_frame_depth;
  pfile_proc_t      *proc_stack[MIPS_PROC_STACK_CT];
  unsigned           proc_sp;
  pfile_t           *proc_pf;
  struct mips_fixup *fixups;
  size_t             fixup_ct;
  size_t             fixup_cap;
} mips_op_state_t;

static mips_op_state_t mips_op_state;

/* TEARDOWN between compiles: frees the fixup table and zeroes the rest. */
void mips_op_state_reset(void)
{
  free(mips_op_state.fixups);
  memset(&mips_op_state, 0, sizeof(mips_op_state));
}


static void mips_proc_push(pfile_proc_t *proc)
{
  if (mips_op_state.proc_sp < MIPS_PROC_STACK_CT) {
    mips_op_state.proc_stack[mips_op_state.proc_sp++] = proc;
  } else if (mips_op_state.proc_pf) {
    pfile_log(mips_op_state.proc_pf, PFILE_LOG_ERR,
        "PIC32 procedure nesting exceeds %u",
        (unsigned) MIPS_PROC_STACK_CT);
  }
}

static void mips_proc_pop(void)
{
  if (mips_op_state.proc_sp > 0) {
    mips_op_state.proc_sp--;
  }
}

static pfile_proc_t *mips_proc_current(void)
{
  return (mips_op_state.proc_sp > 0) ? mips_op_state.proc_stack[mips_op_state.proc_sp - 1]
      : (pfile_proc_t *) 0;
}

static void mips_reg_cache_at_call(pfile_proc_t *callee)
{
  if (mips_op_state.s_frame_depth > 0) {
    mips_reg_cache_preserve_across_call(mips_proc_current());
  } else {
    mips_reg_cache_reset();
  }
  mips_alloc_call_reset(callee);
}

/* Prepare a load/store base for `addr`. When `addr` lies within a
 * signed 16-bit offset of `$gp` (initialized to MIPS_RAM_BASE), emit
 * nothing and use `$gp`. Otherwise emit `lui $t0, hi` (with signed-lo
 * adjust) as before. Returns the signed offset; `*basereg` is the
 * base register for the following load/store. */
int mips_emit_addr_base(pfile_t *pf, unsigned long addr, unsigned *basereg)
{
  long diff;
  int  hi;
  int  lo;

  diff = (long) addr - (long) MIPS_RAM_BASE;
  if ((diff >= -32768L) && (diff <= 32767L)) {
    *basereg = MIPS_REG_GP;
    return (int) diff;
  }
  /* $t0 may be a caller-saved home — spill/cold before LUI. */
  mips_alloc_scratch_clobber(pf, MIPS_REG_T0);
  hi = (int) ((addr >> 16) & 0xFFFF);
  lo = (int) (addr & 0xFFFF);
  if (lo >= 0x8000) {
    hi = (hi + 1) & 0xFFFF;
    lo -= 0x10000;
  }
  (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T0, 0, hi);
  *basereg = MIPS_REG_T0;
  return lo;
}

static mips_opcode_t mips_load_op(unsigned sz, boolean_t sgn)
{
  if (1 == sz) return sgn ? MIPS_OP_LB  : MIPS_OP_LBU;
  if (2 == sz) return sgn ? MIPS_OP_LH  : MIPS_OP_LHU;
  if (4 == sz) return MIPS_OP_LW;
  return MIPS_OP_LBU;
}

static mips_opcode_t mips_store_op(unsigned sz)
{
  if (1 == sz) return MIPS_OP_SB;
  if (2 == sz) return MIPS_OP_SH;
  if (4 == sz) return MIPS_OP_SW;
  return MIPS_OP_SB;
}

/* Materialize a known 32-bit constant into `reg` via LUI+ORI (unsigned
 * halves).  Do not apply the signed-offset hi-adjust used by load/store:
 * ORI zero-extends the low 16 bits, so (addr>>16)/(addr&0xFFFF) is exact
 * for every address, including those with bit 15 of the low half set. */
void mips_emit_imm32(unsigned reg, unsigned long addr)
{
  int hi = (int) ((addr >> 16) & 0xFFFF);
  int lo = (int) (addr & 0xFFFF);

  mips_alloc_scratch_clobber((pfile_t *) 0, reg);
  if (hi) {
    (void) mips_code_alloc(MIPS_OP_LUI, reg, 0, hi);
    if (lo) {
      (void) mips_code_alloc(MIPS_OP_ORI, reg, reg, lo);
    }
  } else if (lo) {
    (void) mips_code_alloc(MIPS_OP_ORI, reg, MIPS_REG_ZERO, lo);
  } else {
    (void) mips_code_alloc(MIPS_OP_ADDIU, reg, MIPS_REG_ZERO, 0);
  }
}

/* Pointer to KSEG0 RAM: `addiu rd,$gp,ofs` when `addr` is in the signed
 * 16-bit window around MIPS_RAM_BASE. Data constants must keep using
 * mips_emit_imm32 (a value near 0x8000xxxx is not an address). */
void mips_emit_ram_ptr(unsigned reg, unsigned long addr)
{
  long diff;

  mips_alloc_scratch_clobber((pfile_t *) 0, reg);
  diff = (long) addr - (long) MIPS_RAM_BASE;
  if ((diff >= -32768L) && (diff <= 32767L)) {
    (void) mips_code_alloc(MIPS_OP_ADDIU, reg, MIPS_REG_GP, (int) diff);
    return;
  }
  mips_emit_imm32(reg, addr);
}

/* Materialize a code-label address into `reg`.  LUI+ORI carry `target` so
 * mips_inst_encode fills the immediates from the label's laid-out addr.
 * Forward WHEREIS(proc) uses the same fixup list as branches: do not allocate
 * a label node here (that would append it in the wrong place). */
void mips_emit_label_addr(unsigned reg, label_t lbl)
{
  mips_code_t tgt;
  mips_code_t lui;
  mips_code_t ori;

  /* the LUI+ORI overwrite `reg`: drop any variable the GPR value
   * cache believes lives there (a stale entry survived a thunk-address
   * materialization and fed the widened reuse of the dead value), and
   * the $s-home allocator's scratch discipline (mips_emit_imm32 rule) */
  mips_reg_cache_clobber(reg);
  mips_alloc_scratch_clobber((pfile_t *) 0, reg);
  tgt = lbl ? (mips_code_t) label_code_get(lbl) : 0;
  lui = mips_code_alloc(MIPS_OP_LUI, reg, 0, 0);
  ori = mips_code_alloc(MIPS_OP_ORI, reg, reg, 0);
  if (tgt) {
    if (lui) {
      lui->target = tgt;
    }
    if (ori) {
      ori->target = tgt;
    }
  } else if (lbl) {
    if (lui) {
      mips_add_fixup(lui, lbl);
    }
    if (ori) {
      mips_add_fixup(ori, lbl);
    }
  }
}

/* WHEREIS / address-of: load the 32-bit address of `val` into `reg`.
 * VALUE wrappers (jal_expr value_get_whereis) unwrap to the target lvalue;
 * LABEL wrappers are procedure/code addresses (flash).  Constant and
 * runtime offsets (WHEREIS(arr[k]) / WHEREIS(arr[i][j])) are added to
 * the RAM base. */
static void mips_load_address_of(pfile_t *pf, value_t val, unsigned reg)
{
  variable_const_t offset;
  unsigned long    addr;
  value_t          inner;
  value_t          bofs;

  if (value_is_value(val)) {
    inner = variable_value_get(value_variable_get(val));
    if (inner) {
      mips_load_address_of(pf, inner, reg);
      return;
    }
  }
  if (value_is_function(val)) {
    pfile_proc_t *pr;
    label_t       flbl;

    pr   = value_proc_get(val);
    flbl = pr ? pfile_proc_label_get(pr) : LABEL_NONE;
    mips_emit_label_addr(reg, flbl);
    return;
  }
  if (value_is_label(val)) {
    mips_emit_label_addr(reg, value_label_get(val));
    return;
  }
  {
    variable_t v;

    v = value_variable_get(val);
    if (mips_var_is_flash_const(v)) {
      offset = 0;
      (void) value_baseofs_const_get(val, &offset);
      mips_emit_flash_addr(reg, v, (unsigned long) offset);
      return;
    }
    addr = mips_var_ram(v);
  }
  bofs = value_baseofs_get(val);
  if (!bofs) {
    mips_emit_ram_ptr(reg, addr);
    return;
  }
  if (value_is_const(bofs)) {
    offset = 0;
    (void) value_baseofs_const_get(val, &offset);
    mips_emit_ram_ptr(reg, addr + (unsigned long) offset);
    return;
  }
  /* Runtime index: base + byte offset in `bofs`. */
  mips_emit_ram_ptr(reg, addr);
  mips_load_value(pf, bofs, MIPS_REG_T2);
  (void) mips_code_r_alloc(MIPS_OP_ADDU, reg, reg, MIPS_REG_T2, 0);
}

void mips_load_value(pfile_t *pf, value_t val, unsigned reg);
void mips_load_value_nofill(pfile_t *pf, value_t val, unsigned reg);
static void mips_load_value_common(pfile_t *pf, value_t val, unsigned reg,
  boolean_t fill_home);

unsigned long mips_var_ram(variable_t v)
{
  unsigned long b;

  if (!v) {
    return MIPS_RAM_BASE;
  }
  b = (unsigned long) variable_base_get(v, 0);
  /* Explicit `at` in KSEG (SFR / mapped I/O) is a full virtual address.
   * Allocated GPRs store an offset from MIPS_RAM_BASE. */
  if (b >= 0x80000000UL) {
    return b;
  }
  return MIPS_RAM_BASE + b;
}

static unsigned mips_ofs_scratch(unsigned dest)
{
  /* The binary-op emitters load operands into the T1/T2 pair with
   * v2 FIRST (so a home in the destination survives the v1 load); the
   * index scratch for a T1/T2-destination indexed access must avoid
   * BOTH.  The old T1->T2 choice clobbered the already-loaded v2:
   * `dig[i] + "0"` loaded the literal into T2, then the element's
   * index load overwrote T2, and the add computed dig[i] + i (the
   * float->string renderer emitted 0x01 where '1' belongs). */
  return (dest == MIPS_REG_T3) ? MIPS_REG_T2 : MIPS_REG_T3;
}

/* True for pointee access through a pointer (`ra = p`, `p[i]`, `*p`).
 * Not for pointer-slot copies (`p = q`) or WHEREIS slot reads. */
boolean_t mips_value_is_ptr_deref(const value_t val)
{
  if (!val || value_flag_test(val, VALUE_FLAG_PTR_SLOT_RAW)) {
    return BOOLEAN_FALSE;
  }
  if (value_flag_test(val, VALUE_FLAG_POINTER)) {
    return BOOLEAN_TRUE;
  }
  return (value_is_pointer(val) && value_is_indirect(val))
    ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* PIC skips const OUT dests (`pic.c` call marshalling).  An unused
 * function result is folded to a CONST-flagged dummy; storing it used
 * VARIABLE_BASE_UNKNOWN (0xFFFF) as a RAM offset and never returned. */
static boolean_t mips_value_is_writable(const value_t val)
{
  variable_t var;

  if (!val || value_is_const(val) || value_is_label(val)
      || value_is_function(val)) {
    return BOOLEAN_FALSE;
  }
  var = value_variable_get(val);
  if (var && variable_is_const(var)) {
    return BOOLEAN_FALSE;
  }
  return BOOLEAN_TRUE;
}

/* LW `dest` from a known RAM address ($gp window or lui $t0). */
static void mips_lw_abs(pfile_t *pf, unsigned long addr, unsigned dest)
{
  unsigned basereg;
  int      lo;

  mips_alloc_scratch_clobber(pf, dest);
  lo = mips_emit_addr_base(pf, addr, &basereg);
  (void) mips_code_alloc(MIPS_OP_LW, dest, basereg, lo);
}

/* Add val's baseofs (const or runtime, already in bytes) to `areg`.
 * Runtime index is loaded into `scratch`.  Callers that need `areg` to
 * already hold a pointer must load a runtime index *before* that pointer
 * if `scratch` load would clobber $t0. */
void mips_add_const_ofs(unsigned areg, unsigned scratch,
    variable_const_t c)
{
  if (!c) {
    return;
  }
  if (c <= 0x7FFFUL) {
    (void) mips_code_alloc(MIPS_OP_ADDIU, areg, areg, (int) c);
  } else {
    mips_emit_imm32(scratch, (unsigned long) c);
    (void) mips_code_r_alloc(MIPS_OP_ADDU, areg, areg, scratch, 0);
  }
}

/* True when `addr` is a legal LH/LW/SH/SW address for `sz` (1 is always
 * fine; 2 and 4 need natural alignment). */
static boolean_t mips_addr_aligned(unsigned long addr, unsigned sz)
{
  if (sz <= 1) {
    return BOOLEAN_TRUE;
  }
  if (2 == sz) {
    return (0 == (addr & 1UL)) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  }
  if (4 == sz) {
    return (0 == (addr & 3UL)) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  }
  return BOOLEAN_FALSE;
}

/* Little-endian load of `sz` bytes from 0(areg) into dest.
 * areg must differ from dest when sz > 1.  Uses $at as scratch. */
static void mips_load_le_bytes(unsigned dest, unsigned areg, unsigned sz,
    boolean_t sgn)
{
  unsigned i;
  unsigned scratch;

  if (sz <= 1) {
    (void) mips_code_alloc(sgn ? MIPS_OP_LB : MIPS_OP_LBU, dest, areg, 0);
    return;
  }
  scratch = (dest == MIPS_REG_AT) ? MIPS_REG_T3 : MIPS_REG_AT;
  (void) mips_code_alloc(MIPS_OP_LBU, dest, areg, 0);
  for (i = 1; i < sz; i++) {
    /* high byte of a signed value is LB so the OR sign-extends */
    (void) mips_code_alloc(
        (sgn && (i == sz - 1U)) ? MIPS_OP_LB : MIPS_OP_LBU,
        scratch, areg, (int) i);
    (void) mips_code_r_alloc(MIPS_OP_SLL, scratch, 0, scratch,
        (int) (8U * i));
    (void) mips_code_r_alloc(MIPS_OP_OR, dest, dest, scratch, 0);
  }
}

/* Little-endian store of `sz` bytes from vreg to 0(areg).  Uses $at. */
static void mips_store_le_bytes(unsigned vreg, unsigned areg, unsigned sz)
{
  unsigned i;
  unsigned scratch;

  if (sz <= 1) {
    (void) mips_code_alloc(MIPS_OP_SB, vreg, areg, 0);
    return;
  }
  scratch = (vreg == MIPS_REG_AT) ? MIPS_REG_T3 : MIPS_REG_AT;
  (void) mips_code_alloc(MIPS_OP_SB, vreg, areg, 0);
  for (i = 1; i < sz; i++) {
    (void) mips_code_r_alloc(MIPS_OP_SRL, scratch, 0, vreg, (int) (8U * i));
    (void) mips_code_alloc(MIPS_OP_SB, scratch, areg, (int) i);
  }
}

/* Load from RAM at (variable base + baseofs).  Size is explicit so
 * PTR_SLOT_RAW can force a 4-byte pointer-slot load.  LH/LW only when
 * the address is a compile-time constant and naturally aligned;
 * otherwise assemble bytes (MIPS AdEL on odd LH / unaligned LW). */
static void mips_load_ram(pfile_t *pf, value_t val, unsigned reg,
    unsigned sz, boolean_t sgn)
{
  variable_t       v;
  unsigned long    base;
  value_t          b;
  unsigned         scratch;
  mips_opcode_t    lop;

  v = value_variable_get(val);
  b = value_baseofs_get(val);
  scratch = mips_ofs_scratch(reg);
  lop = mips_load_op(sz, sgn);
  if (mips_var_is_flash_const(v)) {
    if (b && !value_is_const(b)) {
      mips_load_value(pf, b, scratch);
      /* the index load may leave the index cache-bound to the
       * scratch (the home-copy fast path binds val->reg); raw
       * emitters (bit machinery, wide kernel) clobber the scratch
       * without cache updates, so a binding left here goes stale --
       * drop it (the home still holds the value; later loads re-copy) */
      mips_reg_cache_clobber(scratch);
      mips_emit_flash_addr(MIPS_REG_T0, v, 0);
      (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_T0, MIPS_REG_T0,
          scratch, 0);
      if ((sz <= 1) || (sz > 4)) {
        (void) mips_code_alloc(lop, reg, MIPS_REG_T0, 0);
      } else {
        mips_load_le_bytes(reg, MIPS_REG_T0, sz, sgn);
      }
    } else {
      variable_const_t off;

      off = 0;
      (void) value_baseofs_const_get(val, &off);
      mips_emit_flash_addr(reg, v, (unsigned long) off);
      (void) mips_code_alloc(lop, reg, reg, 0);
    }
    return;
  }
  base = mips_var_ram(v);
  if (b && !value_is_const(b)) {
    mips_load_value(pf, b, scratch);
    mips_reg_cache_clobber(scratch);   /* staleness guard, see above */
    mips_emit_ram_ptr(MIPS_REG_T0, base);
    (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_T0, MIPS_REG_T0,
        scratch, 0);
    if ((1 == sz) || (sz > 4)) {
      (void) mips_code_alloc(lop, reg, MIPS_REG_T0, 0);
    } else {
      mips_load_le_bytes(reg, MIPS_REG_T0, sz, sgn);
    }
  } else {
    variable_const_t off;
    unsigned long    addr;
    int              lo;

    off = 0;
    (void) value_baseofs_const_get(val, &off);
    addr = base + (unsigned long) off;
    if (mips_addr_aligned(addr, sz)) {
      unsigned basereg;

      lo = mips_emit_addr_base(pf, addr, &basereg);
      (void) mips_code_alloc(lop, reg, basereg, lo);
    } else {
      mips_emit_ram_ptr(MIPS_REG_T0, addr);
      mips_load_le_bytes(reg, MIPS_REG_T0, sz, sgn);
    }
  }
}

static void mips_store_ram(pfile_t *pf, value_t val, unsigned vreg,
    unsigned sz)
{
  variable_t    v;
  unsigned long base;
  value_t       b;
  mips_opcode_t sop;

  v = value_variable_get(val);
  base = mips_var_ram(v);
  b = value_baseofs_get(val);
  sop = mips_store_op(sz);
  if (b && !value_is_const(b)) {
    mips_load_value(pf, b, MIPS_REG_T2);
    mips_emit_ram_ptr(MIPS_REG_T0, base);
    (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_T0, MIPS_REG_T0,
        MIPS_REG_T2, 0);
    if ((1 == sz) || (sz > 4)) {
      (void) mips_code_alloc(sop, vreg, MIPS_REG_T0, 0);
    } else {
      mips_store_le_bytes(vreg, MIPS_REG_T0, sz);
    }
  } else {
    variable_const_t off;
    unsigned long    addr;
    unsigned         basereg;
    int              lo;

    off = 0;
    (void) value_baseofs_const_get(val, &off);
    addr = base + (unsigned long) off;
    if (mips_addr_aligned(addr, sz)) {
      lo = mips_emit_addr_base(pf, addr, &basereg);
      (void) mips_code_alloc(sop, vreg, basereg, lo);
    } else {
      mips_emit_ram_ptr(MIPS_REG_T0, addr);
      mips_store_le_bytes(vreg, MIPS_REG_T0, sz);
    }
  }
}

/* TRUE when the value's base VARIABLE is an array of pointers
 * (`var pointer slots[N] as ...`; mirror of the front end's
 * jal_variable_is_array_of_pointers).  For such a value the subscript
 * selects a SLOT (the front end pre-scales the index by the pointer
 * size), so the baseofs is never an offset into a pointee. */
static boolean_t mips_base_is_pointer_array(const value_t val)
{
  variable_t            v;
  variable_def_t        def;
  variable_def_member_t mbr;
  variable_def_t        mdef;

  v = value_variable_get(val);
  if (!v || !variable_is_array(v)) {
    return BOOLEAN_FALSE;
  }
  def = variable_def_get(v);
  mbr = def ? variable_def_member_get(def) : 0;
  mdef = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
  return (mdef
      && (VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(mdef)))
    ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* byte size of a pointer value's POINTEE (the dst def carries it);
 * 0 when the def is missing or the member has no size */
static unsigned mips_pointee_byte_sz(const value_t ptr)
{
  variable_def_t        def;
  variable_def_member_t mbr;
  variable_def_t        mdef;

  def = value_def_get(ptr);
  if (!def || (VARIABLE_DEF_TYPE_POINTER != variable_def_type_get(def))) {
    return 0;
  }
  mbr = variable_def_member_get(def);
  mdef = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
  return mdef ? (unsigned) variable_def_byte_sz_get(mdef) : 0;
}

/* Load pointer slot, add pointee offset, load the pointee into `reg`. */
/* Runtime-address memory access (pointer deref) boundary: flush dirty
 * homes so RAM is authoritative for the access.  The spill stores use
 * $t0/$t2 as scratch, so cached bindings on those registers must go --
 * a later read would otherwise trust a clobbered copy. */
static void mips_deref_sync(pfile_t *pf)
{
  mips_alloc_flush_dirty(pf);
  mips_reg_cache_clobber(MIPS_REG_T0);
  mips_reg_cache_clobber(MIPS_REG_T2);
}

static void mips_load_deref(pfile_t *pf, value_t val, unsigned reg)
{
  variable_t    pv;
  unsigned long pa;
  value_t       b;
  unsigned      scratch;
  mips_opcode_t lop;

  /* The deref target is only known at runtime: a dirty home of the
   * target variable holds a newer value than RAM, and this read must
   * see it. Sync before reading (a read does not change RAM, so the
   * homes stay valid afterwards). */
  mips_deref_sync(pf);
  pv = value_variable_get(val);
  pa = mips_var_ram(pv);
  b = value_baseofs_get(val);
  scratch = mips_ofs_scratch(reg);
  if (mips_base_is_pointer_array(val)) {
    /* pointer-array element (`r = slots[i]`): the baseofs selects the
     * slot -- never a pointee offset.  Load the selected slot's
     * content as the target address; a pointer-def value (the element
     * itself) is done once that address is in reg. */
    if (b && !value_is_const(b)) {
      mips_load_value(pf, b, scratch);
      mips_emit_ram_ptr(reg, pa);
      (void) mips_code_r_alloc(MIPS_OP_ADDU, reg, reg, scratch, 0);
      (void) mips_code_alloc(MIPS_OP_LW, reg, reg, 0);
    } else {
      variable_const_t ofs = 0;

      (void) value_baseofs_const_get(val, &ofs);
      mips_lw_abs(pf, pa + (unsigned long) ofs, reg);
    }
    if (value_is_pointer(val)) {
      return;
    }
    lop = mips_load_op(value_byte_sz_get(val), value_is_signed(val));
    (void) mips_code_alloc(lop, reg, reg, 0);
    return;
  }
  if (b && !value_is_const(b)) {
    mips_load_value(pf, b, scratch);
    mips_reg_cache_clobber(scratch);   /* staleness guard, see load */
    mips_lw_abs(pf, pa, reg);
    (void) mips_code_r_alloc(MIPS_OP_ADDU, reg, reg, scratch, 0);
  } else {
    mips_lw_abs(pf, pa, reg);
    if (b && value_is_const(b)) {
      mips_add_const_ofs(reg, scratch, value_const_get(b));
    }
  }
  lop = mips_load_op(value_byte_sz_get(val), value_is_signed(val));
  (void) mips_code_alloc(lop, reg, reg, 0);
}

/* Store `vreg` through a pointer, adding pointee offset (`p[i] = x`).
 * `sz` is the pointee width (for `p = 10` the POINTER-typed dst is 2, so
 * the caller passes the source width).  Runtime index is loaded into $t2
 * *before* the pointer is loaded into $t0. */
static void mips_store_deref(pfile_t *pf, value_t dst, unsigned vreg,
    unsigned sz)
{
  variable_t    pv;
  unsigned long pa;
  value_t       b;
  mips_opcode_t sop;

  /* A store through a pointer hits an address only known at runtime.
   * Sync homes FIRST (the target's pre-store value belongs in RAM
   * before it is overwritten), and cold everything AFTER (any home
   * could alias the target -- a later spill of such a home would
   * clobber the stored value; `p is v; p = 42` lost the 42 exactly
   * that way). */
  mips_deref_sync(pf);
  pv = value_variable_get(dst);
  pa = mips_var_ram(pv);
  b = value_baseofs_get(dst);
  if (mips_base_is_pointer_array(dst)) {
    /* `slots[i] = v`: the baseofs selects the slot -- load the
     * selected slot's content (the pointee address) before storing
     * through it (the store-through lvalue keeps the POINTER def, and
     * a front-end-dereferenced pointee value arrives here the same
     * way through mips_store_value). */
    if (b && !value_is_const(b)) {
      mips_load_value(pf, b, MIPS_REG_T2);
      mips_emit_ram_ptr(MIPS_REG_T0, pa);
      (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_T0, MIPS_REG_T0,
          MIPS_REG_T2, 0);
      (void) mips_code_alloc(MIPS_OP_LW, MIPS_REG_T0, MIPS_REG_T0, 0);
    } else {
      variable_const_t ofs = 0;

      (void) value_baseofs_const_get(dst, &ofs);
      mips_lw_abs(pf, pa + (unsigned long) ofs, MIPS_REG_T0);
    }
    sop = mips_store_op(sz);
    (void) mips_code_alloc(sop, vreg, MIPS_REG_T0, 0);
    mips_reg_cache_reset();
    mips_alloc_region_reset();
    return;
  }
  if (b && !value_is_const(b)) {
    mips_load_value(pf, b, MIPS_REG_T2);
    mips_lw_abs(pf, pa, MIPS_REG_T0);
    (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_T0, MIPS_REG_T0,
        MIPS_REG_T2, 0);
  } else {
    mips_lw_abs(pf, pa, MIPS_REG_T0);
    if (b && value_is_const(b)) {
      mips_add_const_ofs(MIPS_REG_T0, MIPS_REG_T2, value_const_get(b));
    }
  }
  sop = mips_store_op(sz);
  (void) mips_code_alloc(sop, vreg, MIPS_REG_T0, 0);
  mips_reg_cache_reset();
  mips_alloc_region_reset();
}

/* Packed single-bit access (`var bit`, `flags[i]`).  The front end packs
 * bits into `_bitbucket`; `value_bit_offset_get` is the bit index of a
 * const element (plus the variable's packed start).  A runtime subscript
 * is `baseofs` in *bits* (not bytes — unlike byte arrays).  Result of a
 * load is 0 or 1 in `reg`. */

static boolean_t mips_bit_is_runtime(const value_t val)
{
  value_t b;

  b = value_baseofs_get(val);
  return (b && !value_is_const(b)) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* Const bit index.  Through a pointer the subscript is buffer-relative
 * (the normalize buffer re-bases packed bits at 0); any leaked
 * value_bit_offset_get is the actual's intra-byte offset and is dropped
 * — same contract as pic_assign_from_bit. */
static unsigned mips_bit_const_pos(const value_t val)
{
  variable_const_t off;

  off = 0;
  if (!mips_bit_is_runtime(val)) {
    (void) value_baseofs_const_get(val, &off);
  }
  if (mips_value_is_ptr_deref(val)) {
    return (unsigned) off;
  }
  return value_bit_offset_get(val) + (unsigned) off;
}

static void mips_value_to_bit01(unsigned vreg, const value_t dst)
{
  if (value_is_boolean(dst)) {
    (void) mips_code_r_alloc(MIPS_OP_SLTU, vreg, MIPS_REG_ZERO, vreg, 0);
  } else {
    (void) mips_code_alloc(MIPS_OP_ANDI, vreg, vreg, 1);
  }
}

static void mips_load_bit(pfile_t *pf, value_t val, unsigned reg)
{
  boolean_t thru;
  unsigned  bit_base;

  thru = mips_value_is_ptr_deref(val);
  if (thru) {
    mips_alloc_flush_dirty(pf);
  } else {
    mips_alloc_home_flush(pf, value_variable_get(val));
  }
  /* Through a pointer the index is buffer-relative; drop leaked bitofs. */
  bit_base = thru ? 0 : value_bit_offset_get(val);
  if (mips_bit_is_runtime(val)) {
    unsigned ireg;
    unsigned breg;

    ireg = mips_ofs_scratch(reg);
    mips_load_value(pf, value_baseofs_get(val), ireg);
    if (bit_base) {
      mips_add_const_ofs(ireg, MIPS_REG_AT, (variable_const_t) bit_base);
    }
    /* byte offset in $at, bit-in-byte stays in ireg until after the shift. */
    (void) mips_code_r_alloc(MIPS_OP_SRL, MIPS_REG_AT, 0, ireg, 3);
    (void) mips_code_alloc(MIPS_OP_ANDI, ireg, ireg, 7);
    /* ireg no longer holds the bare index (bit_base add + byte/bit
     * split): drop the cache binding the load left, or a later read
     * of the index variable picks up index+bit_base / the bit-in-byte
     * (sieve computed j = (i+1)^2 through exactly this stale binding). */
    mips_reg_cache_clobber(ireg);
    if (thru) {
      mips_lw_abs(pf, mips_var_ram(value_variable_get(val)), MIPS_REG_T0);
    } else {
      mips_emit_ram_ptr(MIPS_REG_T0, mips_var_ram(value_variable_get(val)));
    }
    (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_T0, MIPS_REG_T0,
        MIPS_REG_AT, 0);
    breg = (reg == ireg) ? MIPS_REG_AT : reg;
    (void) mips_code_alloc(MIPS_OP_LBU, breg, MIPS_REG_T0, 0);
    /* rd = rt >> rs : byte >> bit_in */
    (void) mips_code_r_alloc(MIPS_OP_SRLV, reg, ireg, breg, 0);
    (void) mips_code_alloc(MIPS_OP_ANDI, reg, reg, 1);
  } else {
    unsigned bitpos;
    unsigned bit_in;
    unsigned basereg;
    int      lo;

    bitpos = mips_bit_const_pos(val);
    bit_in = bitpos & 7U;
    if (thru) {
      mips_lw_abs(pf, mips_var_ram(value_variable_get(val)), MIPS_REG_T0);
      mips_add_const_ofs(MIPS_REG_T0, MIPS_REG_T3,
          (variable_const_t) (bitpos / 8U));
      (void) mips_code_alloc(MIPS_OP_LBU, reg, MIPS_REG_T0, 0);
    } else {
      lo = mips_emit_addr_base(pf,
          mips_var_ram(value_variable_get(val))
          + (unsigned long) (bitpos / 8U), &basereg);
      (void) mips_code_alloc(MIPS_OP_LBU, reg, basereg, lo);
    }
    if (bit_in) {
      (void) mips_code_r_alloc(MIPS_OP_SRL, reg, 0, reg, (int) bit_in);
    }
    (void) mips_code_alloc(MIPS_OP_ANDI, reg, reg, 1);
  }
}

static void mips_store_bit(pfile_t *pf, value_t val, unsigned vreg)
{
  boolean_t thru;

  /* Insert (vreg & 1) — or nonzero for a boolean dest — into the packed
   * bit.  $at holds the inverted mask; $t3 holds the containing byte. */
  thru = mips_value_is_ptr_deref(val);
  if (thru) {
    mips_alloc_flush_dirty(pf);
  } else {
    mips_alloc_home_flush(pf, value_variable_get(val));
  }
  mips_value_to_bit01(vreg, val);
  if (mips_bit_is_runtime(val)) {
    unsigned bit_base;

    bit_base = thru ? 0 : value_bit_offset_get(val);
    mips_load_value(pf, value_baseofs_get(val), MIPS_REG_T2);
    if (bit_base) {
      mips_add_const_ofs(MIPS_REG_T2, MIPS_REG_AT,
          (variable_const_t) bit_base);
    }
    (void) mips_code_r_alloc(MIPS_OP_SRL, MIPS_REG_T3, 0, MIPS_REG_T2, 3);
    (void) mips_code_alloc(MIPS_OP_ANDI, MIPS_REG_T2, MIPS_REG_T2, 7);
    /* $t2 holds the bit-in-byte now, not the bare index (see load_bit) */
    mips_reg_cache_clobber(MIPS_REG_T2);
    if (thru) {
      mips_lw_abs(pf, mips_var_ram(value_variable_get(val)), MIPS_REG_T0);
    } else {
      mips_emit_ram_ptr(MIPS_REG_T0, mips_var_ram(value_variable_get(val)));
    }
    (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_T0, MIPS_REG_T0,
        MIPS_REG_T3, 0);
    (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_AT, MIPS_REG_ZERO, 1);
    (void) mips_code_r_alloc(MIPS_OP_SLLV, MIPS_REG_AT, MIPS_REG_T2,
        MIPS_REG_AT, 0);
    (void) mips_code_alloc(MIPS_OP_LBU, MIPS_REG_T3, MIPS_REG_T0, 0);
    (void) mips_code_alloc(MIPS_OP_XORI, MIPS_REG_AT, MIPS_REG_AT, 0xFF);
    (void) mips_code_r_alloc(MIPS_OP_AND, MIPS_REG_T3, MIPS_REG_T3,
        MIPS_REG_AT, 0);
    (void) mips_code_r_alloc(MIPS_OP_SLLV, vreg, MIPS_REG_T2, vreg, 0);
    (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_T3, MIPS_REG_T3, vreg, 0);
    (void) mips_code_alloc(MIPS_OP_SB, MIPS_REG_T3, MIPS_REG_T0, 0);
  } else {
    unsigned bitpos;
    unsigned bit_in;
    unsigned mask;
    unsigned basereg;
    int      lo;

    bitpos = mips_bit_const_pos(val);
    bit_in = bitpos & 7U;
    mask = 1U << bit_in;
    basereg = MIPS_REG_T0;
    if (thru) {
      mips_lw_abs(pf, mips_var_ram(value_variable_get(val)), MIPS_REG_T0);
      mips_add_const_ofs(MIPS_REG_T0, MIPS_REG_T2,
          (variable_const_t) (bitpos / 8U));
      lo = 0;
    } else {
      lo = mips_emit_addr_base(pf,
          mips_var_ram(value_variable_get(val))
          + (unsigned long) (bitpos / 8U), &basereg);
    }
    (void) mips_code_alloc(MIPS_OP_LBU, MIPS_REG_T3, basereg, lo);
    (void) mips_code_alloc(MIPS_OP_ANDI, MIPS_REG_T3, MIPS_REG_T3,
        (int) ((~mask) & 0xFFU));
    if (bit_in) {
      (void) mips_code_r_alloc(MIPS_OP_SLL, vreg, 0, vreg, (int) bit_in);
    }
    (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_T3, MIPS_REG_T3, vreg, 0);
    (void) mips_code_alloc(MIPS_OP_SB, MIPS_REG_T3, basereg, lo);
  }
}

/* Promoted width of a packed field in one GPR (low 32 bits).
 *
 * For `var bit*N` with N <= 32 the GPR extract/insert path
 * (mips_load_bitfield / mips_store_bitfield) handles the full field.
 * For N > 32, mips_is_wide() routes every operation (assign, arith,
 * compare, proc-arg) through the N-byte wide path in mips_wide.c, so
 * this function is never reached with N > 32 in practice.  The cap
 * keeps the GPR path defensive if a future caller bypasses the wide
 * check — only the low 32 bits would be extracted, which is the
 * documented fallback. */
static unsigned mips_bitfield_width(const value_t val)
{
  unsigned n;

  n = (unsigned) value_sz_get(val);
  if (n > 32U) {
    n = 32U;
  }
  return n;
}

static void mips_bitfield_sign_extend(unsigned reg, unsigned bit_sz,
    const value_t val)
{
  unsigned sa;

  if (!value_is_signed(val) || (bit_sz >= 32U) || (0 == bit_sz)) {
    return;
  }
  sa = 32U - bit_sz;
  (void) mips_code_r_alloc(MIPS_OP_SLL, reg, 0, reg, (int) sa);
  (void) mips_code_r_alloc(MIPS_OP_SRA, reg, 0, reg, (int) sa);
}

/* Extract `var bit*N` (N>1) into `reg`, including overlays that span
 * bytes.  Const position is unrolled per containing byte; a runtime
 * `baseofs` (already in bits) is unrolled per bit.  Through a pointer
 * the index is buffer-relative (same contract as mips_load_bit). */
static void mips_load_bitfield(pfile_t *pf, value_t val, unsigned reg)
{
  unsigned  bit_sz;
  unsigned  bit_base;
  unsigned  pos;
  unsigned  got;
  unsigned  left;
  unsigned  bit_in;
  unsigned  take;
  unsigned  mask;
  unsigned  i;
  unsigned  sreg;
  boolean_t thru;
  unsigned  basereg;
  int       lo;

  bit_sz = mips_bitfield_width(val);
  if (!bit_sz) {
    (void) mips_code_alloc(MIPS_OP_ADDIU, reg, MIPS_REG_ZERO, 0);
    return;
  }
  thru = mips_value_is_ptr_deref(val);
  if (thru) {
    mips_alloc_flush_dirty(pf);
  } else {
    mips_alloc_home_flush(pf, value_variable_get(val));
  }
  if (mips_bit_is_runtime(val)) {
    bit_base = thru ? 0 : value_bit_offset_get(val);
    sreg = mips_ofs_scratch(reg);
    mips_load_value(pf, value_baseofs_get(val), sreg);
    if (bit_base) {
      mips_add_const_ofs(sreg, MIPS_REG_AT, (variable_const_t) bit_base);
      /* sreg now holds index+bit_base: drop the load's cache binding */
      mips_reg_cache_clobber(sreg);
    }
    (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_T4, sreg,
        MIPS_REG_ZERO, 0);
    (void) mips_code_alloc(MIPS_OP_ADDIU, reg, MIPS_REG_ZERO, 0);
    for (i = 0; i < bit_sz; i++) {
      (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T3, MIPS_REG_T4,
          (int) i);
      (void) mips_code_r_alloc(MIPS_OP_SRL, MIPS_REG_AT, 0, MIPS_REG_T3, 3);
      (void) mips_code_alloc(MIPS_OP_ANDI, MIPS_REG_T3, MIPS_REG_T3, 7);
      if (thru) {
        mips_lw_abs(pf, mips_var_ram(value_variable_get(val)),
            MIPS_REG_T0);
      } else {
        mips_emit_ram_ptr(MIPS_REG_T0, mips_var_ram(value_variable_get(val)));
      }
      (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_T0, MIPS_REG_T0,
          MIPS_REG_AT, 0);
      (void) mips_code_alloc(MIPS_OP_LBU, MIPS_REG_AT, MIPS_REG_T0, 0);
      (void) mips_code_r_alloc(MIPS_OP_SRLV, MIPS_REG_AT, MIPS_REG_T3,
          MIPS_REG_AT, 0);
      (void) mips_code_alloc(MIPS_OP_ANDI, MIPS_REG_AT, MIPS_REG_AT, 1);
      if (i) {
        (void) mips_code_r_alloc(MIPS_OP_SLL, MIPS_REG_AT, 0, MIPS_REG_AT,
            (int) i);
      }
      (void) mips_code_r_alloc(MIPS_OP_OR, reg, reg, MIPS_REG_AT, 0);
    }
    mips_bitfield_sign_extend(reg, bit_sz, val);
    return;
  }
  pos  = mips_bit_const_pos(val);
  got  = 0;
  left = bit_sz;
  (void) mips_code_alloc(MIPS_OP_ADDIU, reg, MIPS_REG_ZERO, 0);
  while (left) {
    bit_in = pos & 7U;
    take   = 8U - bit_in;
    if (take > left) {
      take = left;
    }
    mask = (1U << take) - 1U;
    if (thru) {
      mips_lw_abs(pf, mips_var_ram(value_variable_get(val)), MIPS_REG_T0);
      mips_add_const_ofs(MIPS_REG_T0, MIPS_REG_AT,
          (variable_const_t) (pos / 8U));
      (void) mips_code_alloc(MIPS_OP_LBU, MIPS_REG_T3, MIPS_REG_T0, 0);
    } else {
      lo = mips_emit_addr_base(pf,
          mips_var_ram(value_variable_get(val))
          + (unsigned long) (pos / 8U), &basereg);
      (void) mips_code_alloc(MIPS_OP_LBU, MIPS_REG_T3, basereg, lo);
    }
    if (bit_in) {
      (void) mips_code_r_alloc(MIPS_OP_SRL, MIPS_REG_T3, 0, MIPS_REG_T3,
          (int) bit_in);
    }
    (void) mips_code_alloc(MIPS_OP_ANDI, MIPS_REG_T3, MIPS_REG_T3,
        (int) mask);
    if (got) {
      (void) mips_code_r_alloc(MIPS_OP_SLL, MIPS_REG_T3, 0, MIPS_REG_T3,
          (int) got);
    }
    (void) mips_code_r_alloc(MIPS_OP_OR, reg, reg, MIPS_REG_T3, 0);
    pos  += take;
    got  += take;
    left -= take;
  }
  mips_bitfield_sign_extend(reg, bit_sz, val);
}

/* Insert the low `bit_sz` bits of `vreg` into a packed field, preserving
 * neighbouring bits in the containing byte(s). */
static void mips_store_bitfield(pfile_t *pf, value_t val, unsigned vreg)
{
  unsigned  bit_sz;
  unsigned  bit_base;
  unsigned  pos;
  unsigned  got;
  unsigned  left;
  unsigned  bit_in;
  unsigned  take;
  unsigned  mask;
  unsigned  clr;
  unsigned  i;
  boolean_t thru;
  unsigned  basereg;
  int       lo;

  bit_sz = mips_bitfield_width(val);
  if (!bit_sz) {
    return;
  }
  thru = mips_value_is_ptr_deref(val);
  if (thru) {
    mips_alloc_flush_dirty(pf);
  } else {
    mips_alloc_home_flush(pf, value_variable_get(val));
  }
  if (mips_bit_is_runtime(val)) {
    bit_base = thru ? 0 : value_bit_offset_get(val);
    mips_load_value(pf, value_baseofs_get(val), MIPS_REG_T2);
    if (bit_base) {
      mips_add_const_ofs(MIPS_REG_T2, MIPS_REG_AT,
          (variable_const_t) bit_base);
      /* $t2 now holds index+bit_base: drop the load's cache binding */
      mips_reg_cache_clobber(MIPS_REG_T2);
    }
    (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_T4, MIPS_REG_T2,
        MIPS_REG_ZERO, 0);
    for (i = 0; i < bit_sz; i++) {
      if (i) {
        (void) mips_code_r_alloc(MIPS_OP_SRL, MIPS_REG_AT, 0, vreg,
            (int) i);
      } else {
        (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_AT, vreg,
            MIPS_REG_ZERO, 0);
      }
      (void) mips_code_alloc(MIPS_OP_ANDI, MIPS_REG_AT, MIPS_REG_AT, 1);
      (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T3, MIPS_REG_T4,
          (int) i);
      (void) mips_code_r_alloc(MIPS_OP_SRL, MIPS_REG_T2, 0, MIPS_REG_T3, 3);
      (void) mips_code_alloc(MIPS_OP_ANDI, MIPS_REG_T3, MIPS_REG_T3, 7);
      (void) mips_code_r_alloc(MIPS_OP_SLLV, MIPS_REG_AT, MIPS_REG_T3,
          MIPS_REG_AT, 0);
      if (thru) {
        mips_lw_abs(pf, mips_var_ram(value_variable_get(val)),
            MIPS_REG_T0);
      } else {
        mips_emit_ram_ptr(MIPS_REG_T0, mips_var_ram(value_variable_get(val)));
      }
      (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_T0, MIPS_REG_T0,
          MIPS_REG_T2, 0);
      (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T2, MIPS_REG_ZERO, 1);
      (void) mips_code_r_alloc(MIPS_OP_SLLV, MIPS_REG_T2, MIPS_REG_T3,
          MIPS_REG_T2, 0);
      (void) mips_code_alloc(MIPS_OP_XORI, MIPS_REG_T2, MIPS_REG_T2, 0xFF);
      (void) mips_code_alloc(MIPS_OP_LBU, MIPS_REG_T3, MIPS_REG_T0, 0);
      (void) mips_code_r_alloc(MIPS_OP_AND, MIPS_REG_T3, MIPS_REG_T3,
          MIPS_REG_T2, 0);
      (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_T3, MIPS_REG_T3,
          MIPS_REG_AT, 0);
      (void) mips_code_alloc(MIPS_OP_SB, MIPS_REG_T3, MIPS_REG_T0, 0);
    }
    return;
  }
  pos  = mips_bit_const_pos(val);
  got  = 0;
  left = bit_sz;
  while (left) {
    bit_in = pos & 7U;
    take   = 8U - bit_in;
    if (take > left) {
      take = left;
    }
    mask = (1U << take) - 1U;
    clr  = ((~((mask << bit_in))) & 0xFFU);
    if (got) {
      (void) mips_code_r_alloc(MIPS_OP_SRL, MIPS_REG_AT, 0, vreg,
          (int) got);
    } else {
      (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_AT, vreg,
          MIPS_REG_ZERO, 0);
    }
    (void) mips_code_alloc(MIPS_OP_ANDI, MIPS_REG_AT, MIPS_REG_AT,
        (int) mask);
    if (bit_in) {
      (void) mips_code_r_alloc(MIPS_OP_SLL, MIPS_REG_AT, 0, MIPS_REG_AT,
          (int) bit_in);
    }
    basereg = MIPS_REG_T0;
    if (thru) {
      mips_lw_abs(pf, mips_var_ram(value_variable_get(val)), MIPS_REG_T0);
      mips_add_const_ofs(MIPS_REG_T0, MIPS_REG_T2,
          (variable_const_t) (pos / 8U));
      lo = 0;
    } else {
      lo = mips_emit_addr_base(pf,
          mips_var_ram(value_variable_get(val))
          + (unsigned long) (pos / 8U), &basereg);
    }
    (void) mips_code_alloc(MIPS_OP_LBU, MIPS_REG_T3, basereg, lo);
    (void) mips_code_alloc(MIPS_OP_ANDI, MIPS_REG_T3, MIPS_REG_T3,
        (int) clr);
    (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_T3, MIPS_REG_T3,
        MIPS_REG_AT, 0);
    (void) mips_code_alloc(MIPS_OP_SB, MIPS_REG_T3, basereg, lo);
    pos  += take;
    got  += take;
    left -= take;
  }
}

/* Load val (const, variable, WHEREIS wrapper, subscript, or `*p`) into `reg`.
 * Uses $t0 as an address scratch; `reg` must differ from $t0 ($t1/$t2).
 * BB value cache + allocated homes (warm within LABEL/CALL). */
/* Ensure `reg` holds `val` in canonical form: sub-word values widened
 * per their OWN signedness.  A cached home can carry a different view's
 * extension (e.g. sbyte -1 sign-extended, then re-read through a
 * `byte()` cast); the SLL+SR(A|L) pair converges to the requested view
 * and is a no-op on already-canonical registers. */
static void mips_widen_view(unsigned reg, value_t val)
{
  unsigned sz;
  unsigned sa;

  sz = (unsigned) value_byte_sz_get(val);
  if ((sz >= 1U) && (sz < 4U)) {
    sa = 32U - (sz * 8U);
    (void) mips_code_r_alloc(MIPS_OP_SLL, reg, 0, reg, (int) sa);
    (void) mips_code_r_alloc(
        value_is_signed(val) ? MIPS_OP_SRA : MIPS_OP_SRL,
        reg, 0, reg, (int) sa);
  }
}

void mips_load_value(pfile_t *pf, value_t val, unsigned reg)
{
  mips_load_value_common(pf, val, reg, BOOLEAN_TRUE);
}

/* Same as mips_load_value but WITHOUT the home-fill tail (the "fill the
 * variable's register home so later loads in this region hit" copy).
 * Callers that stage operands in fixed registers and consume them only
 * later must use this: the home-fill writes the home register
 * unconditionally and would silently destroy an already-staged operand
 * sitting there (the float-compare $t8/$t9 staging lost its first
 * operand exactly that way when the second operand's home was $t8). */
void mips_load_value_nofill(pfile_t *pf, value_t val, unsigned reg)
{
  mips_load_value_common(pf, val, reg, BOOLEAN_FALSE);
}

static void mips_load_value_common(pfile_t *pf, value_t val, unsigned reg,
  boolean_t fill_home)
{
  unsigned   hit;
  unsigned   home;
  variable_t v;

  mips_value_ensure(pf, val);
  hit = mips_reg_cache_lookup(val);
  if (hit != (unsigned) -1) {
    if (hit == reg) {
      mips_widen_view(reg, val);
      return;
    }
    mips_reg_cache_clobber(reg);
    (void) mips_code_r_alloc(MIPS_OP_OR, reg, hit, MIPS_REG_ZERO, 0);
    mips_widen_view(reg, val);
    mips_reg_cache_bind(val, reg);
    return;
  }
  v = value_variable_get(val);
  home = mips_alloc_home_reg(v);
  if ((home != (unsigned) -1) && mips_alloc_home_is_live(v)) {
    if (home == reg) {
      mips_widen_view(reg, val);
      mips_reg_cache_bind(val, reg);
      return;
    }
    mips_reg_cache_clobber(reg);
    (void) mips_code_r_alloc(MIPS_OP_OR, reg, home, MIPS_REG_ZERO, 0);
    mips_widen_view(reg, val);
    mips_reg_cache_bind(val, reg);
    return;
  }
  mips_reg_cache_clobber(reg);
  if (value_is_value(val) || value_is_label(val)) {
    mips_load_address_of(pf, val, reg);
    return;
  }
  if (value_is_const(val)) {
    variable_const_t c = value_const_get(val);

    if (c <= 0x7FFF) {
      (void) mips_code_alloc(MIPS_OP_ADDIU, reg, MIPS_REG_ZERO, (int) c);
    } else {
      mips_emit_imm32(reg, (unsigned long) c);
    }
    return;
  }
  if (value_flag_test(val, VALUE_FLAG_PTR_SLOT_RAW)) {
    /* WHEREIS(slots[idx]): read the 4-byte address stored in the slot. */
    mips_load_ram(pf, val, reg, 4, BOOLEAN_FALSE);
    return;
  }
  /* FLAG_BIT on a POINTER def is the pointee's flag (jal_proc copies
   * array flags onto the pointer); the slot is a 4-byte address. */
  if (value_is_single_bit(val) && !value_is_pointer(val)) {
    mips_load_bit(pf, val, reg);
    return;
  }
  if (value_is_multi_bit(val) && !value_is_pointer(val)) {
    mips_load_bitfield(pf, val, reg);
    return;
  }
  if (mips_value_is_ptr_deref(val)) {
    mips_load_deref(pf, val, reg);
    return;
  }
  if (value_is_pointer(val)) {
    mips_load_ram(pf, val, reg, 4, BOOLEAN_FALSE);
    return;
  }
  mips_load_ram(pf, val, reg, value_byte_sz_get(val), value_is_signed(val));
  /* Fill allocated home from RAM so later loads in this region hit. */
  if (fill_home && (home != (unsigned) -1) && mips_reg_cacheable(val)) {
    unsigned sz;

    sz = value_byte_sz_get(val);
    if (sz < variable_sz_get(v)) {
      /* Narrower view of the variable (e.g. byte(word) cast): the home
       * must not be filled with a truncated value -- the full-width
       * content would be destroyed for every later user of the home
       * (loop counter re-bound to byte(idx) loops forever at 256). */
      mips_reg_cache_bind(val, reg);
      return;
    }
    if (home != reg) {
      (void) mips_code_r_alloc(MIPS_OP_OR, home, reg, MIPS_REG_ZERO, 0);
    }
    /* narrow to the value's width: ANDI zero-extends, which would
     * destroy the sign of a signed sub-word value in the home reg */
    if (1 == sz) {
      if (value_is_signed(val)) {
        (void) mips_code_r_alloc(MIPS_OP_SLL, home, 0, home, 24);
        (void) mips_code_r_alloc(MIPS_OP_SRA, home, 0, home, 24);
      } else {
        (void) mips_code_alloc(MIPS_OP_ANDI, home, home, 0xFF);
      }
    } else if (2 == sz) {
      if (value_is_signed(val)) {
        (void) mips_code_r_alloc(MIPS_OP_SLL, home, 0, home, 16);
        (void) mips_code_r_alloc(MIPS_OP_SRA, home, 0, home, 16);
      } else {
        (void) mips_code_alloc(MIPS_OP_ANDI, home, home, 0xFFFF);
      }
    }
    mips_alloc_home_set_live(v);
    mips_reg_cache_bind(val, home);
  }
}

/* Store `reg` into val (direct, pointer slot, or `p[i]` / `*p`). */
void mips_store_value(pfile_t *pf, value_t val, unsigned reg)
{
  if (!mips_value_is_writable(val)) {
    return;
  }
  mips_value_ensure(pf, val);
  if (value_is_single_bit(val) && !value_is_pointer(val)) {
    mips_store_bit(pf, val, reg);
    mips_reg_cache_invalidate_var(value_variable_get(val));
    mips_alloc_home_invalidate(value_variable_get(val));
    return;
  }
  if (value_is_multi_bit(val) && !value_is_pointer(val)) {
    mips_store_bitfield(pf, val, reg);
    mips_reg_cache_invalidate_var(value_variable_get(val));
    mips_alloc_home_invalidate(value_variable_get(val));
    return;
  }
  if (mips_value_is_ptr_deref(val)) {
    mips_store_deref(pf, val, reg, value_byte_sz_get(val));
    mips_reg_cache_reset();
    mips_alloc_region_reset();
    return;
  }
  if (value_is_pointer(val) || value_flag_test(val, VALUE_FLAG_PTR_SLOT_RAW)) {
    mips_store_ram(pf, val, reg, 4);
    mips_reg_cache_invalidate_var(value_variable_get(val));
    mips_alloc_home_invalidate(value_variable_get(val));
    return;
  }
  if (mips_reg_cacheable(val)) {
    unsigned   home;
    variable_t v;
    unsigned   sz;

    v = value_variable_get(val);
    home = mips_alloc_home_reg(v);
    sz = value_byte_sz_get(val);
    if (home != (unsigned) -1) {
      if (sz < variable_sz_get(v)) {
        /* Narrower view of the variable: a sub-width store keeps RAM
         * semantics (mips_store_ram writes the low bytes only); the
         * home must not be marked dirty with a truncated value. */
        mips_alloc_home_invalidate(v);
        mips_store_ram(pf, val, reg, sz);
        mips_reg_cache_bind(val, reg);
        return;
      }
      /* Dirty write-back: update home only. */
      if (home != reg) {
        (void) mips_code_r_alloc(MIPS_OP_OR, home, reg, MIPS_REG_ZERO, 0);
      }
      /* narrow to the value's width (sign-extend signed, else the
       * cached home reads back zero-extended and signed compares
       * against it fail, e.g. sbyte 127+1 vs -128) */
      if (1 == sz) {
        if (value_is_signed(val)) {
          (void) mips_code_r_alloc(MIPS_OP_SLL, home, 0, home, 24);
          (void) mips_code_r_alloc(MIPS_OP_SRA, home, 0, home, 24);
        } else {
          (void) mips_code_alloc(MIPS_OP_ANDI, home, home, 0xFF);
        }
      } else if (2 == sz) {
        if (value_is_signed(val)) {
          (void) mips_code_r_alloc(MIPS_OP_SLL, home, 0, home, 16);
          (void) mips_code_r_alloc(MIPS_OP_SRA, home, 0, home, 16);
        } else {
          (void) mips_code_alloc(MIPS_OP_ANDI, home, home, 0xFFFF);
        }
      }
      mips_alloc_home_set_dirty(v);
      mips_reg_cache_bind(val, home);
      return;
    }
    mips_store_ram(pf, val, reg, sz);
    mips_reg_cache_bind(val, reg);
    return;
  }
  mips_store_ram(pf, val, reg, value_byte_sz_get(val));
  {
    variable_t v;

    v = value_variable_get(val);
    if (v) {
      mips_reg_cache_invalidate_var(v);
      mips_alloc_home_invalidate(v);
    }
  }
}

/* Spill allocated home to RAM (dirty write-back). */
void mips_alloc_emit_home_spill(pfile_t *pf, variable_t v, unsigned home)
{
  value_t val;

  if (!pf || !v || (home == (unsigned) -1)) {
    return;
  }
  val = value_alloc(v);
  if (!val) {
    return;
  }
  mips_value_ensure(pf, val);
  mips_store_ram(pf, val, home, (unsigned) value_byte_sz_get(val));
  value_release(val);
}

/* ---- forward-branch fixups (label defined after the branch) ---- */
struct mips_fixup {
  mips_code_t branch;
  label_t      lbl;
};
static cmd_t              mips_skip_cmd;   /* branch consumed by a paired cmp */
static unsigned           mips_bitarg_ct;  /* `_mips_bitarg_N` name counter */

static void mips_emit_call(pfile_t *pf, const cmd_t cmd);  /* fwd */
static void mips_emit_frame_return(pfile_t *pf, pfile_proc_t *proc);

void mips_op_reset(void)
{
  if (mips_op_state.fixups) { FREE(mips_op_state.fixups); }
  mips_op_state.fixups    = 0;
  mips_op_state.mvec_hold_ct = 0;
  mips_op_state.fixup_ct  = 0;
  mips_op_state.fixup_cap = 0;
  mips_const_pool_reset();
  mips_wide_reset();
  mips_skip_cmd  = 0;
  mips_bitarg_ct = 0;
  mips_op_state.s_frame_depth = 0;
  mips_op_state.proc_sp = 0;
  mips_op_state.proc_pf = 0;
}

void mips_op_resolve_fixups(pfile_t *pf)
{
  size_t ii;
  for (ii = 0; ii < mips_op_state.fixup_ct; ii++) {
    mips_code_t tgt = (mips_code_t) label_code_get(mips_op_state.fixups[ii].lbl);
    if (tgt) {
      mips_op_state.fixups[ii].branch->target = tgt;
    } else if (pf) {
      const char *nm;

      nm = mips_op_state.fixups[ii].lbl
        ? label_name_get(mips_op_state.fixups[ii].lbl) : 0;
      pfile_log(pf, PFILE_LOG_ERR, "unresolved branch target '%s'",
          nm ? nm : "?");
    }
  }
  mips_op_state.fixup_ct = 0;
}

static void mips_add_fixup(mips_code_t branch, label_t lbl)
{
  if (mips_op_state.fixup_ct >= mips_op_state.fixup_cap) {
    size_t ncap = mips_op_state.fixup_cap ? mips_op_state.fixup_cap * 2 : 16;
    mips_op_state.fixups = REALLOC(mips_op_state.fixups, ncap * sizeof(*mips_op_state.fixups));
    mips_op_state.fixup_cap = ncap;
  }
  mips_op_state.fixups[mips_op_state.fixup_ct].branch = branch;
  mips_op_state.fixups[mips_op_state.fixup_ct].lbl    = lbl;
  mips_op_state.fixup_ct++;
}

/* Emit a conditional branch (BEQ/BNE) rs,rt -> lbl, with a NOP delay slot. */
void mips_op_consume_branch(pfile_t *pf, cmd_t cmd)
{
  /* Fusing the partner branch bypasses the CMD_TYPE_BRANCH GOTO path,
   * which spills dirty homes before any branch (the target label's own
   * flush only serves predecessors in emission order). Spill here too:
   * a loop back-edge that keeps a dirty home unspilled loses every
   * iteration's accumulator (label colds the home, body reloads RAM). */
  mips_alloc_spill_dirty_keep(pf);
  mips_skip_cmd = cmd;
}

mips_code_t mips_emit_cond_branch(mips_opcode_t bop, unsigned rs,
    unsigned rt, label_t lbl)
{
  mips_code_t node = mips_code_alloc(bop, rt, rs, 0);
  mips_code_t tgt  = lbl ? (mips_code_t) label_code_get(lbl) : 0;
  if (tgt) {
    node->target = tgt;
  } else if (lbl) {
    mips_add_fixup(node, lbl);
  }
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);   /* branch delay slot */
  return node;
}

/* Emit an unconditional jump J -> lbl, with a NOP delay slot. */
mips_code_t mips_emit_j(label_t lbl)
{
  mips_code_t node = mips_code_alloc(MIPS_OP_J, 0, 0, 0);
  mips_code_t tgt  = lbl ? (mips_code_t) label_code_get(lbl) : 0;
  if (tgt) {
    node->target = tgt;
  } else if (lbl) {
    mips_add_fixup(node, lbl);
  }
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);   /* branch delay slot */
  return node;
}

/* Emit JAL -> lbl with a NOP delay slot (soft-float / outlined helpers). */
mips_code_t mips_emit_jal(label_t lbl)
{
  mips_code_t node;
  mips_code_t tgt;

  /* Spill before JAL — delay-slot fill must not leave spills after the call. */
  mips_reg_cache_at_call((pfile_proc_t *) 0);
  node = mips_code_alloc(MIPS_OP_JAL, 0, 0, 0);
  tgt  = lbl ? (mips_code_t) label_code_get(lbl) : 0;
  if (tgt) {
    node->target = tgt;
  } else if (lbl) {
    mips_add_fixup(node, lbl);
  }
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
  return node;
}

/* Emit a standalone CMD_TYPE_BRANCH (GOTO; conditional on brval). */
static void mips_emit_branch(pfile_t *pf, const cmd_t cmd)
{
  cmd_branchtype_t btype = cmd_brtype_get(cmd);
  cmd_branchcond_t bcond = cmd_brcond_get(cmd);
  label_t          lbl  = cmd_brdst_get(cmd);
  value_t          brv  = cmd_brval_get(cmd);

  if (CMD_BRANCHTYPE_GOTO == btype) {
    /* Tail return (`cmd_opt_tail_return`): GOTO + callee proc.  The
     * optimizer removes the wrapper's PROC_LEAVE / goto-exit after
     * rewriting the call.  PIC jumps because returns share W/_temp.
     * MIPS has a per-function return slot, so emit a real call (marshal
     * IN, JAL, marshal OUT) and then the missing leave (pop $ra; jr). */
    if ((CMD_BRANCHCOND_NONE == bcond) && cmd_brproc_get(cmd)
        && value_proc_get(cmd_brproc_get(cmd))) {
      mips_emit_call(pf, cmd);
      mips_emit_frame_return(pf, mips_proc_current());
      return;
    }
    if (CMD_BRANCHCOND_NONE == bcond) {
      (void) mips_emit_j(lbl);
      return;
    }
    /* conditional on brval: branch if brval != 0 (TRUE) or == 0 (FALSE) */
    mips_load_value(pf, brv, MIPS_REG_T1);
    if (CMD_BRANCHCOND_TRUE == bcond) {
      (void) mips_emit_cond_branch(MIPS_OP_BNE, MIPS_REG_T1, MIPS_REG_ZERO, lbl);
    } else {
      (void) mips_emit_cond_branch(MIPS_OP_BEQ, MIPS_REG_T1, MIPS_REG_ZERO, lbl);
    }
    return;
  }
  if (CMD_BRANCHTYPE_CALL == btype) {
    mips_emit_call(pf, cmd);
    return;
  }
  if (CMD_BRANCHTYPE_TASK_START == btype) {
    mips_emit_task_start(pf, cmd);
    return;
  }
  if (CMD_BRANCHTYPE_TASK_YIELD == btype) {
    mips_emit_task_yield(pf);
    return;
  }
  if (CMD_BRANCHTYPE_TASK_SUSPEND == btype) {
    mips_emit_task_suspend(pf, cmd);
    return;
  }
  if (CMD_BRANCHTYPE_TASK_RESUME == btype) {
    mips_emit_task_resume(pf, cmd);
    return;
  }
  if (CMD_BRANCHTYPE_TASK_KILL == btype) {
    mips_emit_task_kill(pf, cmd);
    return;
  }
  if (CMD_BRANCHTYPE_TASK_RESTART == btype) {
    mips_emit_task_restart(pf, cmd);
    return;
  }
}

static void mips_emit_push(unsigned reg)
{
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_SP, MIPS_REG_SP, -4);
  (void) mips_code_alloc(MIPS_OP_SW, reg, MIPS_REG_SP, 0);
}

static void mips_emit_pop(unsigned reg)
{
  (void) mips_code_alloc(MIPS_OP_LW, reg, MIPS_REG_SP, 0);
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_SP, MIPS_REG_SP, 4);
}

/* Normal frames save $s3.. (MIPS_S_HOME_EMIT). ISR entry is wrapped by
 * the vector's full GPR save; tasks are jr-entered and skip this frame. */
static int mips_proc_saves_s_homes(pfile_proc_t *proc)
{
  if (!proc) {
    return 1;
  }
  if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_TASK)
      || pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INTERRUPT)) {
    return 0;
  }
  return 1;
}

/* CMD_TYPE_PROC_ENTER: emit the proc's entry label (the CALL target), then
 * push $ra so nested calls work (static-frame model; no recursion). */
static void mips_emit_indirect_param_in(pfile_t *pf, pfile_proc_t *proc);
static void mips_emit_indirect_param_out(pfile_t *pf, pfile_proc_t *proc);

static void mips_emit_proc_enter(pfile_t *pf, pfile_proc_t *proc)
{
  label_t    lbl  = pfile_proc_label_get(proc);
  mips_code_t node = mips_code_label_alloc();

  UNUSED(pf);
  if (lbl) {
    label_code_set(lbl, node);
  }
  mips_proc_push(proc);
  /* Caller-saved homes from the emission-order outer context are not
   * valid inside this body: at runtime the entry arrives from a call
   * site that already spilled them, and the registers hold the caller's
   * leftovers. Cold without spilling ($s homes are frame-saved). */
  mips_alloc_proc_enter_cold();
  /* Tasks are entered by jr from the dispatcher, not JAL. */
  if (proc && pfile_proc_flag_test(proc, PFILE_PROC_FLAG_TASK)) {
    return;
  }
  mips_emit_push(MIPS_REG_RA);
  if (mips_proc_saves_s_homes(proc)) {
    unsigned i;

    for (i = 0; i < MIPS_S_HOME_EMIT; i++) {
      mips_emit_push(MIPS_S_HOME_REG(i));
    }
    mips_op_state.s_frame_depth++;
  }
  /* INDIRECT procs: the inbox holds the marshalled args -- pull them
   * into the linear homes before the body runs (the body's own
   * indirect calls reuse the inbox) */
  if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INDIRECT)) {
    mips_emit_indirect_param_in(pf, proc);
  }
}

/* CMD_TYPE_PROC_LEAVE: emit the proc's exit label (target of early
 * `return` GOTOs), pop $ra, and return via `jr $ra` (with a NOP delay slot). */
/* CMD_TYPE_PROC_LEAVE: emit the proc's exit label (target of early
 * `return` GOTOs), pop $ra, and return via `jr $ra` (with a NOP delay slot).
 * Indirect functions (volatile IN getters) also leave the return in $v0. */
static void mips_emit_proc_leave(pfile_t *pf, pfile_proc_t *proc)
{
  label_t        lbl;
  mips_code_t    node;
  variable_def_t rdef;
  value_t        retval;

  lbl  = pfile_proc_exit_label_get(proc);
  node = mips_code_label_alloc();
  if (lbl) {
    label_code_set(lbl, node);
  }
  /* Tail-return already emitted this frame's pop. A leftover PROC_LEAVE
   * must not pop the parent. Bind the exit label only. */
  if (proc && (proc != mips_proc_current())) {
    return;
  }
  if (proc && pfile_proc_flag_test(proc, PFILE_PROC_FLAG_TASK)) {
    mips_emit_task_suicide(pf);
    mips_proc_pop();
    return;
  }
  retval = VALUE_NONE;
  if (proc && pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INDIRECT)) {
    /* OUT params go back through the inbox before the return rides $v0 */
    mips_emit_indirect_param_out(pf, proc);
    rdef = pfile_proc_return_def_get(proc);
    if (rdef && (variable_def_sz_get(rdef) > 0)
        && (variable_def_byte_sz_get(rdef) <= 4)
        && (VARIABLE_DEF_TYPE_FLOAT != variable_def_type_get(rdef))) {
      retval = pfile_proc_param_get(proc, 0);
    }
  }
  if (retval) {
    mips_load_value(pf, retval, MIPS_REG_V0);
  }
  mips_emit_frame_return(pf, proc);
}

/* Pop $s homes (when this frame saved them) then $ra and jr. Shared by
 * PROC_LEAVE and tail-return so the stack order always matches enter. */
static void mips_emit_frame_return(pfile_t *pf, pfile_proc_t *proc)
{
  mips_alloc_flush_dirty(pf);
  if (mips_proc_saves_s_homes(proc)) {
    unsigned i;

    for (i = MIPS_S_HOME_EMIT; i > 0; i--) {
      mips_emit_pop(MIPS_S_HOME_REG(i - 1));
    }
    if (mips_op_state.s_frame_depth > 0) {
      mips_op_state.s_frame_depth--;
    }
  }
  mips_emit_pop(MIPS_REG_RA);
  (void) mips_code_r_alloc(MIPS_OP_JR, 0, MIPS_REG_RA, 0, 0);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
  mips_proc_pop();
}

/* Interrupt procs have PROC_ENTER (push $ra) but end in ISR_CLEANUP
 * instead of PROC_LEAVE. Return to the EIC vector via jr $ra. */
static void mips_emit_isr_cleanup(pfile_t *pf)
{
  UNUSED(pf);
  (void) mips_code_alloc(MIPS_OP_LW, MIPS_REG_RA, MIPS_REG_SP, 0);
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_SP, MIPS_REG_SP, 4);
  (void) mips_code_r_alloc(MIPS_OP_JR, 0, MIPS_REG_RA, 0, 0);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
  mips_proc_pop();
}

/* Volatile GPRs the fixed-scratch codegen uses, plus Phase B homes and
 * the interrupted $ra. */
static const unsigned mips_isr_save_regs[] = {
  MIPS_REG_AT, MIPS_REG_V1,
  MIPS_REG_A0, MIPS_REG_A1, MIPS_REG_A2, MIPS_REG_A3,
  MIPS_REG_T0, MIPS_REG_T1, MIPS_REG_T2,
  MIPS_REG_T3, MIPS_REG_T4, MIPS_REG_T5, MIPS_REG_T6,
  MIPS_REG_T7, MIPS_REG_T8, MIPS_REG_T9,
  MIPS_REG_S0, MIPS_REG_S1, MIPS_REG_S2,
  MIPS_REG_S3, MIPS_REG_S4, MIPS_REG_S5, MIPS_REG_S6, MIPS_REG_S7,
  MIPS_REG_GP, MIPS_REG_RA
};
#define MIPS_ISR_SAVE_CT \
  (sizeof(mips_isr_save_regs) / sizeof(mips_isr_save_regs[0]))

static void mips_isr_save(void)
{
  unsigned i;
  int      frame;

  frame = (int) (MIPS_ISR_SAVE_CT * (unsigned) MIPS_WORD_SZ);
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_SP, MIPS_REG_SP, -frame);
  for (i = 0; i < MIPS_ISR_SAVE_CT; i++) {
    (void) mips_code_alloc(MIPS_OP_SW, mips_isr_save_regs[i],
        MIPS_REG_SP, (int) (i * MIPS_WORD_SZ));
  }
}

static void mips_isr_restore(void)
{
  unsigned i;
  int      frame;

  frame = (int) (MIPS_ISR_SAVE_CT * (unsigned) MIPS_WORD_SZ);
  for (i = 0; i < MIPS_ISR_SAVE_CT; i++) {
    (void) mips_code_alloc(MIPS_OP_LW, mips_isr_save_regs[i],
        MIPS_REG_SP, (int) (i * MIPS_WORD_SZ));
  }
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_SP, MIPS_REG_SP, frame);
}

/* Store a 32-bit immediate to an absolute KSEG address (uses $t0/$t1). */
static void mips_store_imm32_abs(unsigned long addr, unsigned long val)
{
  unsigned basereg;
  int      lo;

  mips_emit_imm32(MIPS_REG_T1, val);
  lo = mips_emit_addr_base(0, addr, &basereg);
  (void) mips_code_alloc(MIPS_OP_SW, MIPS_REG_T1, basereg, lo);
}

static void mips_isr_stub_body(pfile_t *pf, label_t isr, boolean_t fast)
{
  mips_code_t jal;

  UNUSED(pf);
  if (!fast) {
    mips_isr_save();
  }
  jal = mips_code_alloc(MIPS_OP_JAL, 0, 0, 0);
  mips_add_fixup(jal, isr);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
  if (!fast) {
    mips_isr_restore();
  }
  (void) mips_code_alloc(MIPS_OP_ERET, 0, 0, 0);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
}

static label_t mips_offstub_label(pfile_t *pf, unsigned irq)
{
  char    name[32];
  label_t lbl;

  sprintf(name, "_mips_offstub_%u", irq);
  lbl = pfile_label_find(pf, PFILE_LOG_NONE, name);
  if (!lbl) {
    lbl = pfile_label_alloc(pf, name);
  }
  return lbl;
}

/* ---- PIC32MX multi-vector table (M4K) ------------------------------
 * The MX has no EVIC OFFx registers: hardware expects 64 x 32-byte
 * stubs at EBASE+0x200, selected by INTCON.MVEC + IntCtl.VS.  Code and
 * the per-IRQ handler stubs are moved past the table
 * (MIPS_ISR_MVEC_OFS) and EBASE is pinned to the app base.
 *
 * QEMU's VEIC model forces spacing 0 and always enters EBASE+0x200, so
 * slot 0 is a dispatcher: it reads INTSTAT.VEC and jumps to the selected
 * slot.  On silicon only IRQ0 enters slot 0 (then the dispatcher jumps
 * to the IRQ0 stub); every other vector is hardware-dispatched straight
 * to its slot.  The dispatcher uses $k0/$k1 (26/27), which the compiler
 * never allocates.
 *
 * Forward `j` targets (slots before stubs) are resolved by the normal
 * fixup pass, so those labels are held until mips_mvec_labels_release()
 * runs after mips_op_resolve_fixups().
 */

static void mips_mvec_hold_add(label_t lbl)
{
  if (lbl && (mips_op_state.mvec_hold_ct < 67)) {
    mips_op_state.mvec_hold[mips_op_state.mvec_hold_ct++] = lbl;
  }
}

void mips_mvec_labels_release(void)
{
  while (mips_op_state.mvec_hold_ct > 0) {
    mips_op_state.mvec_hold_ct--;
    label_release(mips_op_state.mvec_hold[mips_op_state.mvec_hold_ct]);
    mips_op_state.mvec_hold[mips_op_state.mvec_hold_ct] = (label_t) 0;
  }
}

static void mips_mx_mvec_slot(label_t target)
{
  int i;

  (void) mips_emit_j(target);        /* J + delay-slot NOP */
  for (i = 2; i < 8; i++) {
    (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
  }
}

static void mips_mx_mvec_emit(pfile_t *pf, label_t isr, boolean_t fast)
{
  label_t       stubs[64];
  label_t       dispatch;
  label_t       deflbl;
  label_t       vec0;
  label_t       stub;
  label_t       plbl;
  mips_code_t   jreset;
  mips_code_t   start;
  mips_code_t   node;
  mips_code_t   org;
  pfile_proc_t *proc;
  int           i;
  int           irq;

  for (i = 0; i < 64; i++) {
    stubs[i] = (label_t) 0;
  }
  dispatch = pfile_label_alloc(pf, "_mips_mvec_dispatch");
  deflbl   = pfile_label_alloc(pf, "_mips_mvec_default");
  vec0     = pfile_label_alloc(pf, "_mips_mvec_vec0");
  mips_mvec_hold_add(dispatch);
  mips_mvec_hold_add(deflbl);
  mips_mvec_hold_add(vec0);

  /* Collect the per-IRQ stub labels first (slots reference them). */
  for (proc = pfile_proc_root_get(pf);
       proc;
       proc = pfile_proc_next(proc)) {
    if (!pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INTERRUPT)) {
      continue;
    }
    irq = pfile_proc_irq_get(proc);
    if (irq >= 64) {
      pfile_log(pf, PFILE_LOG_ERR,
          "PIC32MX multi-vector table has 64 entries; interrupt vector "
          "%d is out of range (0..63)", irq);
      continue;
    }
    if (irq < 0) {
      continue;
    }
    stub = mips_offstub_label(pf, (unsigned) irq);
    stubs[irq] = stub;
    mips_mvec_hold_add(stub);
  }

  jreset = mips_code_alloc(MIPS_OP_J, 0, 0, 0);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);

  for (i = 0; i < 64; i++) {
    org = mips_code_alloc(MIPS_OP_ORG, 0, 0, 0);
    if (org) {
      org->addr = mips_ebase_page(pf) + 0x200UL
                + (unsigned long) i * 32UL;
    }
    if (0 == i) {
      mips_mx_mvec_slot(dispatch);
    } else if (stubs[i]) {
      mips_mx_mvec_slot(stubs[i]);
    } else {
      mips_mx_mvec_slot(deflbl);
    }
  }

  /* Code + handler stubs past the 2 KB table. */
  org = mips_code_alloc(MIPS_OP_ORG, 0, 0, 0);
  if (org) {
    org->addr = mips_isr_vector_addr(pf);
  }

  for (proc = pfile_proc_root_get(pf);
       proc;
       proc = pfile_proc_next(proc)) {
    if (!pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INTERRUPT)) {
      continue;
    }
    irq = pfile_proc_irq_get(proc);
    if ((irq < 0) || (irq >= 64)) {
      continue;
    }
    stub = stubs[irq];
    node = mips_code_label_alloc();
    if (stub && node) {
      label_code_set(stub, node);
    }
    plbl = pfile_proc_label_get(proc);
    if (plbl) {
      mips_isr_stub_body(pf, plbl, fast);
    }
  }

  /* Dispatcher (slot 0 / QEMU's only entry). */
  node = mips_code_label_alloc();
  if (dispatch && node) {
    label_code_set(dispatch, node);
  }
  if (isr && node) {
    label_code_set(isr, node);
  }

  (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_K0, 0, 0xBF88);
  (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_K0, MIPS_REG_K0, 0x1010);
  (void) mips_code_alloc(MIPS_OP_LW, MIPS_REG_K0, MIPS_REG_K0, 0);
  (void) mips_code_alloc(MIPS_OP_ANDI, MIPS_REG_K0, MIPS_REG_K0, 0x3F);
  (void) mips_emit_cond_branch(MIPS_OP_BEQ, MIPS_REG_K0, MIPS_REG_ZERO,
      vec0);
  (void) mips_code_r_alloc(MIPS_OP_SLL, MIPS_REG_K0, 0, MIPS_REG_K0, 5);
  (void) mips_code_r_alloc(MIPS_OP_MFC0, MIPS_CP0_EBASE, 0,
      MIPS_REG_K1, 1);
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_K1, MIPS_REG_K1, 0x200);
  (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_K1, MIPS_REG_K1,
      MIPS_REG_K0, 0);
  (void) mips_code_r_alloc(MIPS_OP_JR, 0, MIPS_REG_K1, 0, 0);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);

  node = mips_code_label_alloc();
  if (vec0 && node) {
    label_code_set(vec0, node);
  }
  if (stubs[0]) {
    (void) mips_emit_j(stubs[0]);
  } else {
    (void) mips_emit_j(deflbl);
  }

  /* Default (unassigned vector): clear the takEN vector's IFS bit and
   * ERET.  A bare ERET would re-enter forever if the flag is left
   * pending.  Only $at/$k0/$k1 are touched; $at/$k1 are saved on the
   * interrupted stack first ($k0/$k1 are compiler-reserved). */
  node = mips_code_label_alloc();
  if (deflbl && node) {
    label_code_set(deflbl, node);
  }
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_SP, MIPS_REG_SP, -8);
  (void) mips_code_alloc(MIPS_OP_SW, MIPS_REG_AT, MIPS_REG_SP, 0);
  (void) mips_code_alloc(MIPS_OP_SW, MIPS_REG_K1, MIPS_REG_SP, 4);
  (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_K0, 0, 0xBF88);
  (void) mips_code_alloc(MIPS_OP_LW, MIPS_REG_K0, MIPS_REG_K0, 0x1010);
  (void) mips_code_alloc(MIPS_OP_ANDI, MIPS_REG_K0, MIPS_REG_K0, 0x3F);
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_K1, MIPS_REG_ZERO, 1);
  (void) mips_code_r_alloc(MIPS_OP_SLLV, MIPS_REG_K1, MIPS_REG_K0,
      MIPS_REG_K1, 0);                 /* k1 = 1 << (vec & 31) */
  (void) mips_code_r_alloc(MIPS_OP_SRL, MIPS_REG_K0, 0, MIPS_REG_K0, 5);
  (void) mips_code_r_alloc(MIPS_OP_SLL, MIPS_REG_K0, 0, MIPS_REG_K0, 4);
  (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_AT, 0, 0xBF88);
  (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_AT, MIPS_REG_AT, 0x1034);
  (void) mips_code_r_alloc(MIPS_OP_ADDU, MIPS_REG_AT, MIPS_REG_AT,
      MIPS_REG_K0, 0);                 /* IFSnCLR */
  (void) mips_code_alloc(MIPS_OP_SW, MIPS_REG_K1, MIPS_REG_AT, 0);
  (void) mips_code_alloc(MIPS_OP_LW, MIPS_REG_K1, MIPS_REG_SP, 4);
  (void) mips_code_alloc(MIPS_OP_LW, MIPS_REG_AT, MIPS_REG_SP, 0);
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_SP, MIPS_REG_SP, 8);
  (void) mips_code_alloc(MIPS_OP_ERET, 0, 0, 0);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);

  start = mips_code_label_alloc();
  if (jreset) {
    jreset->target = start;
  }
}

/* Reset J to _mips_start, EIC vector at code_base+0x400 (QEMU BEV+VEIC).
 * `pragma interrupt`   — software save of volatiles, then JAL ISR, restore, ERET.
 * `pragma interrupt fast` — no software save (SRS / INTCON.SS0); still JAL so
 *   PROC_ENTER's push of $ra is valid; ERET after return.
 * Numbered `pragma interrupt N` adds a per-IRQ stub for OFFx (HW MVEC). */
void mips_isr_vector_emit(pfile_t *pf)
{
  label_t       isr;
  mips_code_t   jreset;
  mips_code_t   start;
  mips_code_t   org;
  boolean_t     fast;
  pfile_proc_t *proc;

  isr = pfile_isr_entry_get(pf);
  if (!isr) {
    return;
  }
  if ((MIPS_FAMILY_MX1 == mips_family(pf))
      && pfile_flag_test(pf, PFILE_FLAG_MISC_INTERRUPT_FAST)) {
    pfile_log(pf, PFILE_LOG_ERR,
      "pragma interrupt fast (shadow register set) is not supported on "
      "this PIC32 family (no PRISS/INTCON.SS0); use `pragma interrupt raw`");
  }
  fast = mips_isr_srs_active(pf);

  if (mips_mx_mvec_active(pf)) {
    mips_mx_mvec_emit(pf, isr, fast);
    label_release(isr);
    return;
  }

  jreset = mips_code_alloc(MIPS_OP_J, 0, 0, 0);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);

  org = mips_code_alloc(MIPS_OP_ORG, 0, 0, 0);
  if (org) {
    org->addr = mips_isr_vector_addr(pf);
  }

  mips_isr_stub_body(pf, isr, fast);

  for (proc = pfile_proc_root_get(pf);
       proc;
       proc = pfile_proc_next(proc)) {
    int     irq;
    label_t stub;
    label_t plbl;
    mips_code_t node;

    if (!pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INTERRUPT)) {
      continue;
    }
    irq = pfile_proc_irq_get(proc);
    if (irq < 0) {
      continue;
    }
    stub = mips_offstub_label(pf, (unsigned) irq);
    node = mips_code_label_alloc();
    if (stub && node) {
      label_code_set(stub, node);
    }
    plbl = pfile_proc_label_get(proc);
    if (plbl) {
      mips_isr_stub_body(pf, plbl, fast);
    }
    if (stub) {
      label_release(stub);
    }
  }

  start = mips_code_label_alloc();
  if (jreset) {
    jreset->target = start;
  }
  label_release(isr);
}

/* Clear EXL/ERL and set Status.IE so EIC IRQs can fire. $sp is live.
 * `pragma interrupt fast`: set INTCON.SS0 so single-vector uses a shadow
 * register set on real silicon (WRITEOP via INTCONSET — safe on QEMU).
 * Numbered IRQs / `pragma interrupt mvec`: INTCON.MVEC + OFFx = stub-EBase.
 * QEMU maps OFFx but forces VEIC spacing 0 (still hits the default stub).
 * Do not write PRISS (unmapped on QEMU). */
void mips_isr_enable_ie(pfile_t *pf)
{
  label_t       isr;
  pfile_proc_t *proc;
  int           any_irq;

  isr = pfile_isr_entry_get(pf);
  if (!isr) {
    return;
  }
  label_release(isr);

  if (mips_isr_srs_active(pf)) {
    mips_code_t loop;

    if (mips_isa_is_micromips()) {
      /* the shadow-set seed needs wrpgpr, which has no microMIPS
       * encoding (the encoder's 32-bit fallback word would decode as
       * an unrelated microMIPS instruction) */
      pfile_log(pf, PFILE_LOG_ERR,
          "the interrupt shadow-register-set path (pragma interrupt "
          "fast, or the loader-image default) is not supported with "
          "pragma isa micromips -- wrpgpr has no microMIPS encoding; "
          "use pragma interrupt raw");
    }
    /* PRISS = 0x76543211: map priority i -> shadow set i AND set the
     * SS0 mode bit (PRISS<0>, 1 bit on MZ -- NOT an INTCON bit: INTCON
     * has no SS0 on MZ and the Phase-3j INTCONSET 0x10000 wrote a
     * reserved bit).  Without SS0 the single-vector ISR runs on main's
     * registers and corrupts them (2026-10-17 bench: prints degenerated
     * mid-line).  QEMU leaves PRISS unmapped; the write is tolerated by
     * the softened local build. */
    mips_store_imm32_abs(MIPS_PRISS, MIPS_PRISS_ALL | 0x1);

    /* Seed every shadow set's $sp and $gp from the current (main) set:
     * select set N through SRSCtl.ESS, wrpgpr copies the PREVIOUS set's
     * register into the CURRENT set's -- walking N = 7..1 propagates
     * main's sp/gp down the chain (XC32's crt0 does this for $gp; a
     * fast ISR also needs a valid $sp for its PROC_ENTER push and $gp
     * for gp-relative data). */
    (void) mips_code_r_alloc(MIPS_OP_MFC0, MIPS_CP0_SRSCTL, 0,
        MIPS_REG_T0, 2);
    (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_T2, MIPS_REG_T0,
        MIPS_REG_ZERO, 0);
    (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T4, MIPS_REG_ZERO, 7);
    loop = mips_code_label_alloc();
    (void) mips_code_alloc(MIPS_OP_ANDI, MIPS_REG_T0, MIPS_REG_T0,
        (int) (~MIPS_SRSCTL_ESS & 0xFFFF));
    (void) mips_code_r_alloc(MIPS_OP_SLL, MIPS_REG_T1, 0, MIPS_REG_T4, 6);
    (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_T0, MIPS_REG_T0,
        MIPS_REG_T1, 0);
    (void) mips_code_r_alloc(MIPS_OP_MTC0, MIPS_CP0_SRSCTL, 0,
        MIPS_REG_T0, 2);
    (void) mips_code_alloc(MIPS_OP_EHB, 0, 0, 0);
    (void) mips_code_r_alloc(MIPS_OP_WRPGPR, MIPS_REG_SP, 0,
        MIPS_REG_SP, 0);
    (void) mips_code_r_alloc(MIPS_OP_WRPGPR, MIPS_REG_GP, 0,
        MIPS_REG_GP, 0);
    (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T4, MIPS_REG_T4, -1);
    {
      mips_code_t bne = mips_code_alloc(MIPS_OP_BNE, MIPS_REG_T4,
          MIPS_REG_ZERO, 0);

      bne->target = loop;
    }
    (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
    (void) mips_code_r_alloc(MIPS_OP_MTC0, MIPS_CP0_SRSCTL, 0,
        MIPS_REG_T2, 2);
    (void) mips_code_alloc(MIPS_OP_EHB, 0, 0, 0);
  }

  any_irq = 0;
  for (proc = pfile_proc_root_get(pf);
       proc;
       proc = pfile_proc_next(proc)) {
    if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INTERRUPT)
        && (pfile_proc_irq_get(proc) >= 0)) {
      any_irq = 1;
      break;
    }
  }
  if (any_irq || mips_mvec_enabled(pf)) {
    if (MIPS_FAMILY_MX1 == mips_family(pf)) {
      /* MX1/2 has no EVIC OFFx registers: INTCON.MVEC selects the
       * 64 x 32-byte stub table at EBASE+0x200 (emitted by
       * mips_mx_mvec_emit).  No OFFx programming. */
      mips_store_imm32_abs(MIPS_INTCONSET_MX, MIPS_INTCON_MVEC);
    } else {
    mips_store_imm32_abs(MIPS_INTCONSET, MIPS_INTCON_MVEC);
    if (!any_irq) {
      mips_store_imm32_abs(MIPS_OFF0, MIPS_OFF_DEFAULT);
    }
    for (proc = pfile_proc_root_get(pf);
         proc;
         proc = pfile_proc_next(proc)) {
      int     irq;
      label_t stub;
      unsigned basereg;
      int      lo;

      if (!pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INTERRUPT)) {
        continue;
      }
      irq = pfile_proc_irq_get(proc);
      if (irq < 0) {
        continue;
      }
      stub = mips_offstub_label(pf, (unsigned) irq);
      mips_emit_label_addr(MIPS_REG_T1, stub);
      mips_emit_imm32(MIPS_REG_T0, mips_code_base(pf));
      (void) mips_code_r_alloc(MIPS_OP_SUBU, MIPS_REG_T1, MIPS_REG_T1,
          MIPS_REG_T0, 0);
      lo = mips_emit_addr_base(pf,
          MIPS_OFF_BASE + (unsigned long) irq * 4UL, &basereg);
      (void) mips_code_alloc(MIPS_OP_SW, MIPS_REG_T1, basereg, lo);
      if (stub) {
        label_release(stub);
      }
    }
    }
  }

  /* clear every IFSn flag before arming IE: silicon (2026-10-09 ladder
   * bisection) shows a pending flag at entry dispatches immediately
   * through the single vector -- EBASE+0x200 held nothing in the
   * pre-fix images, so the core crash-looped before the first print.
   * IFS0..IFS8 sit at 0xBF810030 + n*0x10 (n = 0..8). */
  {
    int n;
    int ct;
    unsigned long base;

    if (MIPS_FAMILY_MX1 == mips_family(pf)) {
      base = MIPS_IFS0_MX;
      ct   = MIPS_IFS_CT_MX;
    } else {
      /* MZ: IFS0..IFS7 at 0xBF810040 + n*0x10 (0x30 is RESERVED) */
      base = 0xBF810040UL;
      ct   = 8;
    }
    for (n = 0; n < ct; n++) {
      unsigned long ifs = base + ((unsigned long) n * 0x10UL);

      mips_emit_imm32(MIPS_REG_T0, ifs);
      (void) mips_code_alloc(MIPS_OP_SW, MIPS_REG_ZERO, MIPS_REG_T0, 0);
    }
  }

  /* Cause.IV = 1 and IntCtl.VS = 0x20 (XC32 recipe, 2026-10-17 silicon
   * A/B): the EIC delivers interrupts only with Cause.IV set -- without
   * it MVEC requests are never presented and single-vector delivery
   * wedges the core outright.  QEMU ignores both (it dispatches
   * regardless), so the QEMU suites are unaffected. */
  (void) mips_code_r_alloc(MIPS_OP_MFC0, MIPS_CP0_CAUSE, 0,
      MIPS_REG_T0, 0);
  (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T1, 0,
      (int) ((MIPS_CAUSE_IV >> 16) & 0xFFFF));
  (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_T0, MIPS_REG_T0,
      MIPS_REG_T1, 0);
  (void) mips_code_r_alloc(MIPS_OP_MTC0, MIPS_CP0_CAUSE, 0,
      MIPS_REG_T0, 0);
  (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T0, MIPS_REG_ZERO,
      (int) MIPS_INTCTL_VS1);
  (void) mips_code_r_alloc(MIPS_OP_MTC0, MIPS_CP0_INTCTL, 0,
      MIPS_REG_T0, 1);

  /* Status &= ~(EXL|ERL); Status |= IE.  ~(2|4) == ~6 == -7.  Both
   * clears are required for dispatch (QEMU-reg-dumped 2026-10-09:
   * with either bit still set, a pending request shows in Cause.IP
   * while nothing dispatches), and this sequence passes the QEMU
   * single-vector + mvec bodies.  OPEN ITEM (2026-10-09 bench
   * bisection, single-vector LOADER images on the MZ EF silicon):
   * this same RMW hung the core before the first user instruction in
   * every loader variant tried (with/without the EBASE+0x200 vector
   * block and IFS hygiene), while `ei` alone runs but cannot
   * dispatch.  Boot-flash (non-loader) images are unaffected on
   * silicon.  Next step when resumed: a diagnostic build that stores
   * mfc0 Status to RAM, to see the true pre/post Status on silicon. */
  (void) mips_code_r_alloc(MIPS_OP_MFC0, MIPS_CP0_STATUS, 0, MIPS_REG_T0, 0);
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1, MIPS_REG_ZERO, -7);
  (void) mips_code_r_alloc(MIPS_OP_AND, MIPS_REG_T0, MIPS_REG_T0,
      MIPS_REG_T1, 0);
  (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T0, MIPS_REG_T0, MIPS_ST_IE);
  (void) mips_code_r_alloc(MIPS_OP_MTC0, MIPS_CP0_STATUS, 0, MIPS_REG_T0, 0);
  /* execution hazard barrier: interrupt recognition right after an
   * mtc0 Status is unpredictable without one (XC32 arms via `ei`,
   * which carries the barrier implicitly) */
  (void) mips_code_alloc(MIPS_OP_EHB, 0, 0, 0);
}

/* Formal is a pointer whose pointee is packed bits (`bit in bits[]` /
 * `bit in bits[N]`).  Same predicate as pic_formal_is_pointer_to_bit. */
static boolean_t mips_formal_is_pointer_to_bit(value_t formal)
{
  variable_def_t        def;
  variable_def_member_t mbr;
  variable_def_t        pointee;

  if (!formal || !value_is_pointer(formal)) {
    return BOOLEAN_FALSE;
  }
  def = value_def_get(formal);
  mbr = def ? variable_def_member_get(def) : 0;
  pointee = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
  return pointee && variable_def_flag_test(pointee, VARIABLE_DEF_FLAG_BIT);
}

/* Call through a slot: `call &_a_get` is FUNCTION + VALUE_FLAG_INDIRECT
 * (cmd dump prints `&`), not necessarily POINTER-typed.  Direct `call f`
 * has a proc label. */
static boolean_t mips_call_is_indirect(value_t proc_val)
{
  pfile_proc_t *proc;
  label_t       lbl;

  if (!proc_val) {
    return BOOLEAN_FALSE;
  }
  proc = value_proc_get(proc_val);
  lbl  = proc ? pfile_proc_label_get(proc) : LABEL_NONE;
  if (lbl) {
    return BOOLEAN_FALSE;
  }
  if (value_is_pointer(proc_val)) {
    return BOOLEAN_TRUE;
  }
  return (value_is_function(proc_val)
      && value_flag_test(proc_val, VALUE_FLAG_INDIRECT))
    ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* Integer/bit return of an indirect call (pointer-to-function or
 * FUNCTION + INDIRECT slot).  First member of the function def is the
 * return. */
static boolean_t mips_fn_ptr_has_return(value_t proc_val)
{
  variable_def_t        def;
  variable_def_member_t mbr;
  variable_def_t        rdef;

  if (!proc_val) {
    return BOOLEAN_FALSE;
  }
  def = value_def_get(proc_val);
  if (def && (VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(def))) {
    mbr = variable_def_member_get(def);
    def = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
  }
  if (!def || (VARIABLE_DEF_TYPE_FUNCTION != variable_def_type_get(def))) {
    return BOOLEAN_FALSE;
  }
  mbr  = variable_def_member_get(def);
  rdef = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
  return (rdef && (variable_def_sz_get(rdef) > 0)
      && (variable_def_byte_sz_get(rdef) <= 4)
      && (VARIABLE_DEF_TYPE_FLOAT != variable_def_type_get(rdef)))
    ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* One packed bit of a const bit array (data[] little-endian bits). */
static unsigned mips_packed_const_bit(value_t src, unsigned i)
{
  unsigned              pos;
  variable_t            v;
  const unsigned char  *data;

  pos  = value_bit_offset_get(src) + i;
  v    = value_variable_get(src);
  data = v ? (const unsigned char *) variable_data_get(v) : 0;
  if (data) {
    return (unsigned) ((data[pos / 8U] >> (pos & 7U)) & 1U);
  }
  return (variable_const_byte_get(value_const_get(src), pos / 8U)
      >> (pos & 7U)) & 1U;
}

/* Load packed bit `bitpos` at known RAM base into `reg` (0 or 1). */
static void mips_bit_extract_abs(pfile_t *pf, unsigned long ram,
    unsigned bitpos, unsigned reg)
{
  unsigned bit_in;
  unsigned basereg;
  int      lo;

  bit_in = bitpos & 7U;
  lo = mips_emit_addr_base(pf, ram + (unsigned long) (bitpos / 8U), &basereg);
  (void) mips_code_alloc(MIPS_OP_LBU, reg, basereg, lo);
  if (bit_in) {
    (void) mips_code_r_alloc(MIPS_OP_SRL, reg, 0, reg, (int) bit_in);
  }
  (void) mips_code_alloc(MIPS_OP_ANDI, reg, reg, 1);
}

/* Insert (vreg & 1) into packed bit `bitpos` at known RAM base. */
static void mips_bit_insert_abs(pfile_t *pf, unsigned long ram,
    unsigned bitpos, unsigned vreg)
{
  unsigned bit_in;
  unsigned mask;
  unsigned basereg;
  int      lo;

  bit_in = bitpos & 7U;
  mask   = 1U << bit_in;
  (void) mips_code_alloc(MIPS_OP_ANDI, vreg, vreg, 1);
  lo = mips_emit_addr_base(pf, ram + (unsigned long) (bitpos / 8U), &basereg);
  (void) mips_code_alloc(MIPS_OP_LBU, MIPS_REG_T3, basereg, lo);
  (void) mips_code_alloc(MIPS_OP_ANDI, MIPS_REG_T3, MIPS_REG_T3,
      (int) ((~mask) & 0xFFU));
  if (bit_in) {
    (void) mips_code_r_alloc(MIPS_OP_SLL, vreg, 0, vreg, (int) bit_in);
  }
  (void) mips_code_r_alloc(MIPS_OP_OR, MIPS_REG_T3, MIPS_REG_T3, vreg, 0);
  (void) mips_code_alloc(MIPS_OP_SB, MIPS_REG_T3, basereg, lo);
}

/* Copy packed bits from `src` into INTEGER buffer `buf`, re-based at bit 0.
 * Do not use mips_wide_assign (that would LBU the bitbucket as bytes). */
static void mips_copy_packed_bits_to_buf(pfile_t *pf, value_t buf, value_t src)
{
  unsigned     bit_ct;
  unsigned     byte_ct;
  unsigned     src_base;
  unsigned long dst_ram;
  variable_t   sv;
  boolean_t    src_const;

  bit_ct   = (unsigned) value_sz_get(src);
  byte_ct  = (bit_ct + 7U) / 8U;
  if (byte_ct < 1) {
    byte_ct = 1;
  }
  src_base = value_bit_offset_get(src);
  dst_ram  = mips_var_ram(value_variable_get(buf));
  sv       = value_variable_get(src);
  src_const = (value_is_const(src) || (sv && variable_is_const(sv)))
      ? BOOLEAN_TRUE : BOOLEAN_FALSE;

  if (src_const) {
    unsigned b;

    for (b = 0; b < byte_ct; b++) {
      unsigned acc;
      unsigned bi;
      unsigned basereg;
      int      lo;

      acc = 0;
      for (bi = 0; bi < 8U; bi++) {
        unsigned ix;

        ix = b * 8U + bi;
        if (ix >= bit_ct) {
          break;
        }
        if (mips_packed_const_bit(src, ix)) {
          acc |= 1U << bi;
        }
      }
      lo = mips_emit_addr_base(pf, dst_ram + b, &basereg);
      (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1, MIPS_REG_ZERO,
          (int) acc);
      (void) mips_code_alloc(MIPS_OP_SB, MIPS_REG_T1, basereg, lo);
    }
  } else {
    unsigned     i;
    unsigned long src_ram;

    src_ram = mips_var_ram(sv);
    for (i = 0; i < bit_ct; i++) {
      mips_bit_extract_abs(pf, src_ram, src_base + i, MIPS_REG_T1);
      mips_bit_insert_abs(pf, dst_ram, i, MIPS_REG_T1);
    }
  }
}

/* Copy buffer bits (base 0) back onto packed `dst` (the caller's actual). */
static void mips_copy_buf_to_packed_bits(pfile_t *pf, value_t dst, value_t buf)
{
  unsigned      bit_ct;
  unsigned      dst_base;
  unsigned      i;
  unsigned long src_ram;
  unsigned long dst_ram;

  bit_ct   = (unsigned) value_sz_get(dst);
  dst_base = value_bit_offset_get(dst);
  src_ram  = mips_var_ram(value_variable_get(buf));
  dst_ram  = mips_var_ram(value_variable_get(dst));
  for (i = 0; i < bit_ct; i++) {
    mips_bit_extract_abs(pf, src_ram, i, MIPS_REG_T1);
    mips_bit_insert_abs(pf, dst_ram, dst_base + i, MIPS_REG_T1);
  }
}

/* Dedicated `_mips_bitarg_N` INTEGER buffer: packed actual re-based at
 * bit 0, then the formal pointer is bound to it.  PIC32 has no volatile
 * retry pass, so the base is assigned here via mips_var_place. */
static value_t mips_bit_array_actual_normalize(pfile_t *pf, value_t actual)
{
  variable_sz_t bit_ct;
  variable_sz_t byte_ct;
  value_t       tmp;
  char          name[32];

  if (!actual || !value_is_array(actual) || !value_is_bit(actual)) {
    return VALUE_NONE;
  }
  bit_ct  = value_sz_get(actual);
  byte_ct = (bit_ct + 7) / 8;
  if (byte_ct < 1) {
    byte_ct = 1;
  }
  sprintf(name, "_mips_bitarg_%u", mips_bitarg_ct);
  {
    variable_t var;

    var = pfile_proc_variable_find(pfile_proc_root_get(pf), name, 0);
    if (!var) {
      variable_def_t def;

      def = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
          VARIABLE_DEF_FLAG_VOLATILE, byte_ct);
      var = VARIABLE_NONE;
      if (def
          && (RESULT_OK == pfile_variable_alloc(pf,
              PFILE_VARIABLE_ALLOC_GLOBAL, name, def, VARIABLE_NONE,
              &var))) {
        variable_flag_set(var, VARIABLE_FLAG_AUTO);
        variable_flag_set(var, VARIABLE_FLAG_STICKY);
        mips_bitarg_ct++;
      } else {
        variable_def_free(def);
        pfile_log_syserr(pf, RESULT_MEMORY);
        var = VARIABLE_NONE;
      }
    }
    if (var && (VARIABLE_BASE_UNKNOWN == variable_base_get(var, 0))) {
        mips_var_place(pf, var, byte_ct);
    }
    tmp = var ? value_alloc(var) : VALUE_NONE;
    variable_release(var);
  }
  if (tmp) {
    value_assign_ct_bump(tmp, CTR_BUMP_INCR);
    value_use_ct_bump(tmp, CTR_BUMP_INCR);
    mips_copy_packed_bits_to_buf(pf, tmp, actual);
  }
  return tmp;
}

/* ---------- shared indirect-call protocol (libcore indirect_alg) -----
 * Caller marshalling and the callee PROC_ENTER/LEAVE copies live once
 * in indirect_alg.c over the mips_ialg_ops table (mips_var.c). */

static void mips_emit_indirect_marshal_in(pfile_t *pf, value_t proc_val,
    value_t *params, size_t ct)
{
  ialg_marshal_in(pf, &mips_ialg_ops, proc_val, params, ct);
}

static void mips_emit_indirect_marshal_out(pfile_t *pf, value_t proc_val,
    value_t *params, size_t ct)
{
  ialg_marshal_out(pf, &mips_ialg_ops, proc_val, params, ct);
}

/* INDIRECT callee entry: copy the inbox into the proc's own linear
 * param homes (the body's indirect calls reuse the inbox) */
static void mips_emit_indirect_param_in(pfile_t *pf, pfile_proc_t *proc)
{
  ialg_param_in(pf, &mips_ialg_ops, proc);
}

/* INDIRECT callee exit: copy OUT params back into the inbox */
static void mips_emit_indirect_param_out(pfile_t *pf, pfile_proc_t *proc)
{
  ialg_param_out(pf, &mips_ialg_ops, proc);
}

/* CMD_TYPE_BRANCH CALL: marshal IN args into the callee's param vars, JAL to
 * the proc's entry label, then marshal OUT args (and the function return)
 * back to the caller's destinations. Static-frame model (no recursion):
 * each proc's params/locals live at fixed RAM offsets. */
static void mips_emit_call(pfile_t *pf, const cmd_t cmd)
{
  value_t       proc_val = cmd_brproc_get(cmd);
  pfile_proc_t *proc    = value_proc_get(proc_val);
  value_t      *params  = cmd_brproc_params_get(cmd);
  size_t        ct      = cmd_brproc_param_ct_get(cmd);
  size_t        pct;
  label_t       lbl     = proc ? pfile_proc_label_get(proc) : LABEL_NONE;
  size_t        ii;
  value_t      *bitbufs;
  boolean_t     indirect;

  pct = proc ? pfile_proc_param_ct_get(proc) : 0;
  if (pct > ct) {
    ct = pct;
  }

  /* The inbox protocol: pointer-slot calls AND every call to an
   * INDIRECT-flagged proc -- its PROC_ENTER copies the inbox into the
   * proc's linear param homes, so all callers must marshal there.
   * Known-label targets JAL the entry label; pointer slots JALR
   * through the 4-byte slot. */
  indirect = mips_call_is_indirect(proc_val)
      || (proc && pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INDIRECT));
  if (indirect) {
    mips_emit_indirect_marshal_in(pf, proc_val, params, ct);
    if (proc && lbl) {
      mips_code_t jal;

      mips_reg_cache_at_call(proc);
      jal = mips_code_alloc(MIPS_OP_JAL, 0, 0, 0);
      mips_add_fixup(jal, lbl);
      (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
    } else {
      if (value_is_pointer(proc_val)) {
        mips_load_value(pf, proc_val, MIPS_REG_T1);
      } else {
        mips_load_ram(pf, proc_val, MIPS_REG_T1, 4, BOOLEAN_FALSE);
      }
      mips_reg_cache_at_call((pfile_proc_t *) 0);
      (void) mips_code_r_alloc(MIPS_OP_JALR, MIPS_REG_RA, MIPS_REG_T1, 0, 0);
      (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
    }
    mips_emit_indirect_marshal_out(pf, proc_val, params, ct);
    if (params && params[0] && mips_fn_ptr_has_return(proc_val)
        && !mips_is_wide(params[0], VALUE_NONE, VALUE_NONE)) {
      mips_store_value(pf, params[0], MIPS_REG_V0);
    }
    return;
  }

  bitbufs = ct ? CALLOC(ct, sizeof(*bitbufs)) : 0;

  /* pass 1: copy each IN arg into the callee's param var */
  for (ii = 0; ii < ct; ii++) {
    value_t    pval = pfile_proc_param_get(proc, ii);
    value_t    arg  = params ? params[ii] : VALUE_NONE;
    variable_t pv;
    if (!pval || !arg) continue;
    if (!value_dflag_test(pval, VARIABLE_DEF_FLAG_IN)) continue;
    pv = value_variable_get(pval);
    /* Unused `_name_count` (open array) is assigned by the caller but
     * never used in the callee, so it has no RAM — do not SH to 0xFFFF. */
    if (!pv || (VARIABLE_BASE_UNKNOWN == variable_base_get(pv, 0))) {
      if (!mips_formal_is_pointer_to_bit(pval)) {
        continue;
      }
    }
    if (mips_formal_is_pointer_to_bit(pval)
        && value_is_array(arg) && value_is_bit(arg)
        && !value_is_pointer(arg)) {
      value_t buf;

      buf = mips_bit_array_actual_normalize(pf, arg);
      if (buf) {
        mips_emit_ram_ptr(MIPS_REG_T1,
            mips_var_ram(value_variable_get(buf)));
        mips_store_value(pf, pval, MIPS_REG_T1);
        if (bitbufs) {
          bitbufs[ii] = buf;
        } else {
          value_release(buf);
        }
        continue;
      }
    }
    /* Open-array / data-pointer formals (`byte in data[]`) and volatile
     * IN getters (function actual into a pointer-to-function slot): pass
     * an address, never a value copied through an unset slot. */
    if (value_is_pointer(pval) && !value_is_pointer(arg)) {
      if (value_is_function(arg)) {
        mips_load_address_of(pf, arg, MIPS_REG_T1);
        mips_store_value(pf, pval, MIPS_REG_T1);
        continue;
      }
      if (value_is_array(arg) || value_is_value(arg)
          || value_is_label(arg)) {
        mips_load_address_of(pf, arg, MIPS_REG_T1);
        mips_store_value(pf, pval, MIPS_REG_T1);
        continue;
      }
    }
    if (mips_is_wide(pval, arg, VALUE_NONE)) {
      mips_wide_assign(pf, pval, arg);
      /* the raw wide copy clobbers the t-scratch WITHOUT updating the
       * value cache -- drop bindings for those registers so later
       * loads of other values cannot hit stale copies (the float-str
      * battery caught a caller reading its call return as the last
       * copied word) */
      mips_reg_cache_clobber(MIPS_REG_T1);
      mips_reg_cache_clobber(MIPS_REG_T2);
      mips_reg_cache_clobber(MIPS_REG_T3);
    } else {
      mips_load_value(pf, arg, MIPS_REG_T1);
      mips_store_value(pf, pval, MIPS_REG_T1);
    }
  }

  /* call — spill dirty homes before JAL (not after: delay-slot fill
   * would otherwise leave later spills past the call). */
  mips_reg_cache_at_call(proc);
  {
    mips_code_t node = mips_code_alloc(MIPS_OP_JAL, 0, 0, 0);
    mips_code_t tgt  = lbl ? (mips_code_t) label_code_get(lbl) : 0;

    if (tgt) {
      node->target = tgt;
    } else if (lbl) {
      mips_add_fixup(node, lbl);
    }
    (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);   /* branch delay slot */
  }

  /* pass 2: copy each OUT var (incl. the function return) to the caller dst */
  for (ii = 0; ii < ct; ii++) {
    value_t pval = pfile_proc_param_get(proc, ii);
    value_t arg  = params ? params[ii] : VALUE_NONE;
    if (!pval || !arg) continue;
    if (!value_dflag_test(pval, VARIABLE_DEF_FLAG_OUT)) continue;
    if (bitbufs && bitbufs[ii]) {
      if (!value_is_const(arg)
          && !variable_is_const(value_variable_get(arg))) {
        mips_copy_buf_to_packed_bits(pf, arg, bitbufs[ii]);
      }
      continue;
    }
    /* Open-array OUT: callee already wrote through the pointer slot. */
    if (value_is_pointer(pval) && !value_is_pointer(arg)
        && (value_is_array(arg) || value_is_value(arg))) {
      continue;
    }
    /* Unused result (`_db = f(...)` never read) is a CONST dummy. */
    if (!mips_value_is_writable(arg)) {
      continue;
    }
    if (mips_is_wide(arg, pval, VALUE_NONE)) {
      mips_wide_assign(pf, arg, pval);
      /* raw wide copy clobbers the t-scratch -- drop stale bindings
       * (same guard as pass 1; the length check of the float-str
       * battery read its call return through a stale T1 binding) */
      mips_reg_cache_clobber(MIPS_REG_T1);
      mips_reg_cache_clobber(MIPS_REG_T2);
      mips_reg_cache_clobber(MIPS_REG_T3);
    } else {
      mips_load_value(pf, pval, MIPS_REG_T1);
      mips_store_value(pf, arg, MIPS_REG_T1);
    }
  }
  if (bitbufs) {
    for (ii = 0; ii < ct; ii++) {
      if (bitbufs[ii]) {
        value_release(bitbufs[ii]);
      }
    }
    FREE(bitbufs);
  }
}

/* Same skip list as pic_cmd_optimize / cmd_next_exec_get: LABEL, NOP,
 * BLOCK_*, STATEMENT_END may sit between a stripped test and its
 * branch.  Immediate cmd_link_get misses those pairs and the branch
 * then has VALUE_NONE for brval. */
static cmd_t mips_cond_branch_partner(const cmd_t cmd)
{
  cmd_t next;

  next = cmd_next_exec_get(cmd_link_get(cmd));
  if (!next || (CMD_TYPE_BRANCH != cmd_type_get(next))) {
    return CMD_NONE;
  }
  if (CMD_BRANCHTYPE_GOTO != cmd_brtype_get(next)) {
    return CMD_NONE;
  }
  if (CMD_BRANCHCOND_NONE == cmd_brcond_get(next)) {
    return CMD_NONE;
  }
  return next;
}

/* Emit a comparison (no dst) paired with the following conditional branch.
 * Combines the compare and the branch into MIPS instructions, inverting the
 * comparison when the branch condition is FALSE. Marks the branch cmd as
 * consumed so the main loop skips it. */
static void mips_emit_compare_branch(pfile_t *pf, const cmd_t cmd,
    operator_t op, value_t v1, value_t v2)
{
  cmd_t             next;
  cmd_branchcond_t   bcond;
  label_t           lbl;
  boolean_t         sgn;
  mips_opcode_t     slt;
  boolean_t         br_true;   /* TRUE: branch taken when relation holds */

  next = mips_cond_branch_partner(cmd);
  if (!next) {
    return;
  }
  bcond = cmd_brcond_get(next);
  lbl = cmd_brdst_get(next);
  mips_op_consume_branch(pf, next);
  br_true = (CMD_BRANCHCOND_TRUE == bcond);

  mips_load_value(pf, v1, MIPS_REG_T1);
  mips_load_value(pf, v2, MIPS_REG_T2);
  sgn = value_is_signed(v1) || value_is_signed(v2);
  slt = sgn ? MIPS_OP_SLT : MIPS_OP_SLTU;

  switch (op) {
    case OPERATOR_EQ:
      (void) mips_emit_cond_branch(br_true ? MIPS_OP_BEQ : MIPS_OP_BNE,
        MIPS_REG_T1, MIPS_REG_T2, lbl);
      return;
    case OPERATOR_NE:
      (void) mips_emit_cond_branch(br_true ? MIPS_OP_BNE : MIPS_OP_BEQ,
        MIPS_REG_T1, MIPS_REG_T2, lbl);
      return;
    case OPERATOR_LT:                       /* $t3 = ($t1 < $t2) */
      (void) mips_code_r_alloc(slt, MIPS_REG_T3, MIPS_REG_T1, MIPS_REG_T2, 0);
      (void) mips_emit_cond_branch(br_true ? MIPS_OP_BNE : MIPS_OP_BEQ,
        MIPS_REG_T3, MIPS_REG_ZERO, lbl);
      return;
    case OPERATOR_GE:                        /* $t3 = ($t1 < $t2); GE = !LT */
      (void) mips_code_r_alloc(slt, MIPS_REG_T3, MIPS_REG_T1, MIPS_REG_T2, 0);
      (void) mips_emit_cond_branch(br_true ? MIPS_OP_BEQ : MIPS_OP_BNE,
        MIPS_REG_T3, MIPS_REG_ZERO, lbl);
      return;
    case OPERATOR_GT:                        /* $t3 = ($t2 < $t1) = ($t1 > $t2) */
      (void) mips_code_r_alloc(slt, MIPS_REG_T3, MIPS_REG_T2, MIPS_REG_T1, 0);
      (void) mips_emit_cond_branch(br_true ? MIPS_OP_BNE : MIPS_OP_BEQ,
        MIPS_REG_T3, MIPS_REG_ZERO, lbl);
      return;
    case OPERATOR_LE:                        /* $t3 = ($t2 < $t1); LE = !GT */
      (void) mips_code_r_alloc(slt, MIPS_REG_T3, MIPS_REG_T2, MIPS_REG_T1, 0);
      (void) mips_emit_cond_branch(br_true ? MIPS_OP_BEQ : MIPS_OP_BNE,
        MIPS_REG_T3, MIPS_REG_ZERO, lbl);
      return;
    default:
      pfile_log(pf, PFILE_LOG_ERR, "unhandled comparison operator");
      return;
  }
}

/* Emit a comparison with a dst (boolean result 0/1 stored into dst). */
static void mips_emit_compare_bool(pfile_t *pf, value_t dst, operator_t op,
    value_t v1, value_t v2)
{
  boolean_t     sgn;
  mips_opcode_t slt;

  mips_load_value(pf, v1, MIPS_REG_T1);
  mips_load_value(pf, v2, MIPS_REG_T2);
  mips_reg_cache_clobber(MIPS_REG_T1);
  sgn = value_is_signed(v1) || value_is_signed(v2);
  slt = sgn ? MIPS_OP_SLT : MIPS_OP_SLTU;

  switch (op) {
    case OPERATOR_EQ:                        /* $t1 = ($t1 == $t2) */
      (void) mips_code_r_alloc(MIPS_OP_XOR, MIPS_REG_T1, MIPS_REG_T1,
        MIPS_REG_T2, 0);
      (void) mips_code_r_alloc(MIPS_OP_SLTU, MIPS_REG_T1, MIPS_REG_ZERO,
        MIPS_REG_T1, 0);                     /* $t1 = (0 < $t1) = ($t1!=0) */
      (void) mips_code_alloc(MIPS_OP_XORI, MIPS_REG_T1, MIPS_REG_T1, 1);
      break;
    case OPERATOR_NE:                        /* $t1 = ($t1 != $t2) */
      (void) mips_code_r_alloc(MIPS_OP_XOR, MIPS_REG_T1, MIPS_REG_T1,
        MIPS_REG_T2, 0);
      (void) mips_code_r_alloc(MIPS_OP_SLTU, MIPS_REG_T1, MIPS_REG_ZERO,
        MIPS_REG_T1, 0);
      break;
    case OPERATOR_LT:
      (void) mips_code_r_alloc(slt, MIPS_REG_T1, MIPS_REG_T1, MIPS_REG_T2, 0);
      break;
    case OPERATOR_GE:
      (void) mips_code_r_alloc(slt, MIPS_REG_T1, MIPS_REG_T1, MIPS_REG_T2, 0);
      (void) mips_code_alloc(MIPS_OP_XORI, MIPS_REG_T1, MIPS_REG_T1, 1);
      break;
    case OPERATOR_GT:
      (void) mips_code_r_alloc(slt, MIPS_REG_T1, MIPS_REG_T2, MIPS_REG_T1, 0);
      break;
    case OPERATOR_LE:
      (void) mips_code_r_alloc(slt, MIPS_REG_T1, MIPS_REG_T2, MIPS_REG_T1, 0);
      (void) mips_code_alloc(MIPS_OP_XORI, MIPS_REG_T1, MIPS_REG_T1, 1);
      break;
    default:
      pfile_log(pf, PFILE_LOG_ERR, "unhandled comparison operator");
      return;
  }
  mips_store_value(pf, dst, MIPS_REG_T1);
}

/* Dest-less ASSIGN / LOGICAL: `tmp = x` was stripped; branch on whether
 * x is nonzero.  Same pairing as pic_cmd_optimize. */
static void mips_emit_truth_branch(pfile_t *pf, const cmd_t cmd, value_t v1)
{
  cmd_t            next;
  cmd_branchcond_t bcond;
  label_t          lbl;
  boolean_t        br_true;

  next = mips_cond_branch_partner(cmd);
  if (!next || !v1) {
    return;
  }
  bcond = cmd_brcond_get(next);
  lbl = cmd_brdst_get(next);
  mips_op_consume_branch(pf, next);
  br_true = (CMD_BRANCHCOND_TRUE == bcond);
  mips_load_value(pf, v1, MIPS_REG_T1);
  (void) mips_emit_cond_branch(br_true ? MIPS_OP_BNE : MIPS_OP_BEQ,
      MIPS_REG_T1, MIPS_REG_ZERO, lbl);
}

/* CMD_TYPE_OPERATOR CMPB/NOTL with no dst, paired with the following
 * conditional branch (`if !x then`).  Logical-not: result is 1 iff v1==0. */
static void mips_emit_not_branch(pfile_t *pf, const cmd_t cmd, value_t v1)
{
  cmd_t            next;
  cmd_branchcond_t bcond;
  label_t          lbl;
  boolean_t        br_true;

  next = mips_cond_branch_partner(cmd);
  if (!next) {
    return;
  }
  bcond = cmd_brcond_get(next);
  lbl = cmd_brdst_get(next);
  mips_op_consume_branch(pf, next);
  br_true = (CMD_BRANCHCOND_TRUE == bcond);

  mips_load_value(pf, v1, MIPS_REG_T1);
  mips_reg_cache_clobber(MIPS_REG_T1);
  (void) mips_code_r_alloc(MIPS_OP_SLTU, MIPS_REG_T1, MIPS_REG_ZERO,
      MIPS_REG_T1, 0);
  (void) mips_code_alloc(MIPS_OP_XORI, MIPS_REG_T1, MIPS_REG_T1, 1);
  (void) mips_emit_cond_branch(br_true ? MIPS_OP_BNE : MIPS_OP_BEQ,
      MIPS_REG_T1, MIPS_REG_ZERO, lbl);
}

void mips_op_generate(pfile_t *pf, const cmd_t cmd)
{
  cmd_type_t  type;
  operator_t  op;
  value_t     dst;
  value_t     v1;
  value_t     v2;

  mips_op_state.proc_pf = pf;
  /* a branch already consumed by a paired comparison */
  if (cmd == mips_skip_cmd) {
    mips_skip_cmd = 0;
    return;
  }

  type = cmd_type_get(cmd);

  if (CMD_TYPE_LABEL == type) {
    label_t     lbl;
    mips_code_t node;

    /* Caller-saved homes: spill on the FALL-THROUGH edge, before the
     * label node.  The label's own post-node flush must not store them:
     * a branch or loop back-edge predecessor can reach the label after
     * a CALL clobbered the register (2026-10-20 print_sdword_dec hang:
     * `counter` in $t8 was re-stored dead on the loop back-edge, the
     * divisor underflowed to 0, and the digit loop spun forever). */
    mips_alloc_spill_dirty_caller(pf);
    lbl  = cmd_label_get(cmd);
    node = mips_code_label_alloc();
    if (lbl) {
      label_code_set(lbl, node);
    }
    /* Remaining ($s) dirty homes: spill, then cold. */
    mips_alloc_region_reset();
    return;
  }

  if (CMD_TYPE_BRANCH == type) {
    /* Spill dirty $s homes BEFORE any branch: the target label's own
     * flush (mips_alloc_region_reset at LABEL emission) only serves
     * predecessors in emission order -- a jumping path's register
     * state would otherwise be lost and the join would read a stale
     * RAM home (QEMU-pinned: promoted local set on an if-taken path
     * returned the previous call's value). */
    if ((CMD_BRANCHTYPE_GOTO == cmd_brtype_get(cmd))
        || (CMD_BRANCHCOND_NONE != cmd_brcond_get(cmd))) {
      /* Spill dirty homes for the jumping path; $s stay dirty (the
       * target label's flush must still fire for OTHER incoming
       * paths), caller-saved are colded inside.  Duplicate spills are
       * harmless; a missing one is not. */
      mips_alloc_spill_dirty_keep(pf);
    }
    mips_emit_branch(pf, cmd);
    return;
  }

  if (CMD_TYPE_PROC_ENTER == type) {
    mips_emit_proc_enter(pf, cmd_proc_get(cmd));
    return;
  }
  if (CMD_TYPE_PROC_LEAVE == type) {
    mips_emit_proc_leave(pf, cmd_proc_get(cmd));
    return;
  }
  if (CMD_TYPE_ISR_CLEANUP == type) {
    mips_emit_isr_cleanup(pf);
    return;
  }

  if (CMD_TYPE_USEC_DELAY == type) {
    mips_delay_create(pf, cmd_usec_delay_get(cmd));
    return;
  }

  if ((CMD_TYPE_NOP == type) || (CMD_TYPE_ASSERT == type)) {
    (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
    return;
  }
  if (CMD_TYPE_SLEEP == type) {
    (void) mips_code_alloc(MIPS_OP_WAIT, 0, 0, 0);
    return;
  }
  if (CMD_TYPE_END == type) {
    {
      mips_code_t loop;
      mips_code_t beq;

      loop = mips_code_label_alloc();
      beq = mips_code_alloc(MIPS_OP_BEQ, MIPS_REG_ZERO, MIPS_REG_ZERO, 0);
      if (beq) {
        beq->target = loop;
      }
      (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
    }
    return;
  }
  if (CMD_TYPE_ASM == type) {
    pfile_log(pf, PFILE_LOG_ERR,
        "inline assembly is not supported on PIC32");
    return;
  }
  if ((CMD_TYPE_BLOCK_START == type) || (CMD_TYPE_BLOCK_END == type)
      || (CMD_TYPE_STATEMENT_END == type) || (CMD_TYPE_COMMENT == type)
      || (CMD_TYPE_LOG == type)) {
    return;
  }

  if (CMD_TYPE_OPERATOR != type) {
    pfile_log(pf, PFILE_LOG_ERR, "unhandled command type");
    return;
  }
  op  = cmd_optype_get(cmd);
  dst = cmd_opdst_get(cmd);
  v1  = cmd_opval1_get(cmd);
  v2  = cmd_opval2_get(cmd);

  /* 5+ byte values: N-byte copy / multi-word ALU (mips_wide.c) */
  if ((OPERATOR_POINTER_ASSIGN != op) && mips_is_wide(dst, v1, v2)) {
    if (mips_wide_generate(pf, op, dst, v1, v2, cmd)) {
      return;
    }
    /* Dest-less ASSIGN/test with no branch partner: same as PIC orphan. */
    if (!dst) {
      return;
    }
    pfile_log(pf, PFILE_LOG_ERR, "unhandled wide-integer operator");
    return;
  }

  /* IEEE754 single soft-float (mips_opfn.c) */
  if (mips_op_is_float(dst, v1, v2)) {
    if (!dst && operator_is_relation(op)) {
      if (mips_float_compare_branch(pf, cmd, op, v1, v2)) {
        return;
      }
    }
    if (dst && operator_is_relation(op)) {
      mips_float_compare_bool(pf, dst, op, v1, v2);
      return;
    }
    if (OPERATOR_ASSIGN == op) {
      if (!v1) {
        return;
      }
      if (value_is_float(dst) && !value_is_float(v1)) {
        mips_float_assign_to(pf, dst, v1);
      } else if (!value_is_float(dst) && value_is_float(v1)) {
        mips_float_assign_from(pf, dst, v1);
      } else {
        mips_load_value(pf, v1, MIPS_REG_T1);
        mips_store_value(pf, dst, MIPS_REG_T1);
      }
      return;
    }
    if (OPERATOR_NEG == op) {
      mips_float_neg(pf, dst, v1);
      return;
    }
    if ((OPERATOR_ADD == op) || (OPERATOR_SUB == op)
        || (OPERATOR_MUL == op) || (OPERATOR_DIV == op)) {
      mips_float_binop(pf, op, dst, v1, v2);
      return;
    }
    pfile_log(pf, PFILE_LOG_ERR, "unhandled floating-point operator");
    return;
  }

  /* comparison with no dst: pair with the following conditional branch */
  if (!dst && operator_is_relation(op)) {
    mips_emit_compare_branch(pf, cmd, op, v1, v2);
    return;
  }
  if (!dst && ((OPERATOR_CMPB == op) || (OPERATOR_NOTL == op))) {
    mips_emit_not_branch(pf, cmd, v1);
    return;
  }
  if (!dst && ((OPERATOR_ASSIGN == op) || (OPERATOR_LOGICAL == op))) {
    mips_emit_truth_branch(pf, cmd, v1);
    return;
  }

  if (!dst) {
    return;                       /* logical/cast/...: later */
  }

  /* comparison with a dst: boolean result into dst */
  if (operator_is_relation(op)) {
    mips_emit_compare_bool(pf, dst, op, v1, v2);
    return;
  }

  if (OPERATOR_POINTER_ASSIGN == op) {
    /* `p is ha` / `slots[i] is ha`: store the 32-bit address of the target
     * into the pointer slot (4 bytes, plus any slot baseofs). */
    if (!v1 || !dst) {
      return;
    }
    mips_load_address_of(pf, v1, MIPS_REG_T1);
    mips_store_ram(pf, dst, MIPS_REG_T1, 4);
    return;
  }

  if (OPERATOR_ASSIGN == op) {
    if (!v1) {
      return;
    }
    if (value_is_pointer(dst) && value_is_array(v1)
        && !value_is_pointer(v1)) {
      /* Array → pointer slot (open-array bind / `p = buf`): store address. */
      mips_load_address_of(pf, v1, MIPS_REG_T1);
      mips_store_ram(pf, dst, MIPS_REG_T1, 4);
    } else if (value_is_pointer(dst) && value_is_function(v1)) {
      /* Volatile IN getter / PUT: store the function's code address. */
      mips_load_address_of(pf, v1, MIPS_REG_T1);
      mips_store_ram(pf, dst, MIPS_REG_T1, 4);
    } else if (value_is_pointer(dst) && !value_is_pointer(v1)
        && !value_is_value(v1) && !value_is_label(v1)
        && !value_is_array(v1)) {
      /* `p = x`: store x THROUGH the pointer.  dst is POINTER-typed with
       * INDIRECT clear; p[i] is integer+I+P and goes through store_value.
       * Arrays are excluded (bind address above); same rule as PIC.  The
       * width is the POINTEE's (the dst def carries it) -- a const RHS
       * has no byte size of its own and would truncate word/dword
       * pointees to their low byte. */
      unsigned psz;

      psz = mips_pointee_byte_sz(dst);
      if (!psz) {
        psz = value_byte_sz_get(v1);
      }
      if (!psz) {
        psz = 1;
      }
      mips_load_value(pf, v1, MIPS_REG_T1);
      mips_store_deref(pf, dst, MIPS_REG_T1, psz);
    } else {
      unsigned rd;

      /* Expr vreg: load into dst home when assigned (skip $t1 bounce). */
      rd = MIPS_REG_T1;
      if (mips_reg_cacheable(dst)) {
        unsigned home;

        home = mips_alloc_home_reg(value_variable_get(dst));
        if (home != (unsigned) -1) {
          rd = home;
        }
      }
      mips_load_value(pf, v1, rd);
      mips_store_value(pf, dst, rd);
    }
    return;
  }

  /* binary / unary: dst = v1 op v2  (v2 NULL for unary) */
  if (!v1) {
    return;
  }
  {
    unsigned rd;
    variable_t dv;

    rd = MIPS_REG_T1;
    dv = value_variable_get(dst);
    if (mips_reg_cacheable(dst) && dv) {
      unsigned home;

      home = mips_alloc_home_reg(dv);
      if (home != (unsigned) -1) {
        rd = home;
      }
    }
    /* Load v2 first so a home in rd is copied out before v1 overwrites it. */
    if (v2) {
      mips_load_value(pf, v2, MIPS_REG_T2);
    }
    mips_load_value(pf, v1, rd);
    /* ALU writes rd — drop any cache entry that named the old value. */
    mips_reg_cache_clobber(rd);

    switch (op) {
      case OPERATOR_ADD:
        (void) mips_code_r_alloc(MIPS_OP_ADDU, rd, rd, MIPS_REG_T2, 0);
        break;
      case OPERATOR_SUB:
        (void) mips_code_r_alloc(MIPS_OP_SUBU, rd, rd, MIPS_REG_T2, 0);
        break;
      case OPERATOR_ANDB:
        (void) mips_code_r_alloc(MIPS_OP_AND, rd, rd, MIPS_REG_T2, 0);
        break;
      case OPERATOR_ORB:
        (void) mips_code_r_alloc(MIPS_OP_OR, rd, rd, MIPS_REG_T2, 0);
        break;
      case OPERATOR_XORB:
        (void) mips_code_r_alloc(MIPS_OP_XOR, rd, rd, MIPS_REG_T2, 0);
        break;
      case OPERATOR_MUL:
        (void) mips_code_r_alloc(
            (value_is_signed(v1) || value_is_signed(v2))
            ? MIPS_OP_MULT : MIPS_OP_MULTU,
            0, rd, MIPS_REG_T2, 0);
        (void) mips_code_r_alloc(MIPS_OP_MFLO, rd, 0, 0, 0);
        break;
      case OPERATOR_DIV:
      case OPERATOR_MOD: {
        mips_code_t zbr;
        mips_code_t join;

        if (v2 && value_is_const(v2) && (0 == value_const_get(v2))) {
          pfile_log(pf, PFILE_LOG_ERR, "division by zero");
          return;
        }
        zbr = mips_code_alloc(MIPS_OP_BEQ, MIPS_REG_T2, MIPS_REG_ZERO, 0);
        (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
        (void) mips_code_r_alloc(
            (value_is_signed(v1) || value_is_signed(v2))
            ? MIPS_OP_DIV : MIPS_OP_DIVU,
            0, rd, MIPS_REG_T2, 0);
        if (OPERATOR_DIV == op) {
          (void) mips_code_r_alloc(MIPS_OP_MFLO, rd, 0, 0, 0);
        } else {
          (void) mips_code_r_alloc(MIPS_OP_MFHI, rd, 0, 0, 0);
        }
        join = mips_code_alloc(MIPS_OP_BEQ, MIPS_REG_ZERO, MIPS_REG_ZERO, 0);
        (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
        if (zbr) {
          zbr->target = mips_code_label_alloc();
        }
        (void) mips_code_r_alloc(MIPS_OP_ADDU, rd, MIPS_REG_ZERO,
            MIPS_REG_ZERO, 0);
        if (join) {
          join->target = mips_code_label_alloc();
        }
        break;
      }
      case OPERATOR_SHIFT_LEFT:
        (void) mips_code_r_alloc(MIPS_OP_SLLV, rd, MIPS_REG_T2, rd, 0);
        break;
      case OPERATOR_SHIFT_RIGHT:
        (void) mips_code_r_alloc(MIPS_OP_SRLV, rd, MIPS_REG_T2, rd, 0);
        break;
      case OPERATOR_SHIFT_RIGHT_ARITHMETIC:
        (void) mips_code_r_alloc(MIPS_OP_SRAV, rd, MIPS_REG_T2, rd, 0);
        break;
      case OPERATOR_NEG:
        (void) mips_code_r_alloc(MIPS_OP_SUBU, rd, MIPS_REG_ZERO, rd, 0);
        break;
      case OPERATOR_CMPB:
        (void) mips_code_alloc(MIPS_OP_XORI, rd, rd, -1);
        if (value_is_single_bit(dst)) {
          (void) mips_code_alloc(MIPS_OP_ANDI, rd, rd, 1);
        }
        break;
      case OPERATOR_NOTL:
        (void) mips_code_r_alloc(MIPS_OP_SLTU, rd, MIPS_REG_ZERO, rd, 0);
        (void) mips_code_alloc(MIPS_OP_XORI, rd, rd, 1);
        break;
      case OPERATOR_LOGICAL:
        (void) mips_code_r_alloc(MIPS_OP_SLTU, rd, MIPS_REG_ZERO, rd, 0);
        break;
      case OPERATOR_INCR:
        (void) mips_code_alloc(MIPS_OP_ADDIU, rd, rd, 1);
        break;
      case OPERATOR_DECR:
        (void) mips_code_alloc(MIPS_OP_ADDIU, rd, rd, -1);
        break;
      default:
        pfile_log(pf, PFILE_LOG_ERR, "unhandled operator");
        return;
    }
    /* Sub-word results must be narrowed to the destination type: the ALU
     * ran in 32 bits, so e.g. `word 0xFFFF << 4` is 0x000FFFF0 until
     * masked.  Stores truncate anyway, but a compare on the expression
     * result is forwarded from the GPR value cache without a store/reload
     * round-trip and would see the wide value. */
    {
      unsigned dsz = (unsigned) value_byte_sz_get(dst);
      unsigned sa;

      if ((dsz >= 1U) && (dsz < 4U)) {
        sa = 32U - (dsz * 8U);
        (void) mips_code_r_alloc(MIPS_OP_SLL, rd, 0, rd, (int) sa);
        (void) mips_code_r_alloc(
            value_is_signed(dst) ? MIPS_OP_SRA : MIPS_OP_SRL,
            rd, 0, rd, (int) sa);
      }
    }
    mips_store_value(pf, dst, rd);
  }
}
