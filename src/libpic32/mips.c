/************************************************************
 **
 ** mips.c : PIC32 (MIPS32) back-end -- code list + entry points
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include <string.h>
#include <stdio.h>

#include "../libutils/mem.h"
#include "../libcore/pfile.h"
#include "../libcore/pf_msg.h"
#include "../libcore/pf_proc.h"
#include "../libcore/label.h"
#include "../libcore/cmd.h"
#include "../libcore/variable.h"
#include "../libcore/vardef.h"
#include "pic32.h"
#include "mips.h"
#include "mips_clock.h"
#include "mips_alloc.h"
#include "mips_reg.h"

/* Emit `v_<name> EQU 0xNNNN` lines for allocated GPRs so @sim_validate
 * can resolve symbols the same way @gpsim_validate does.  Addresses are
 * offsets from MIPS_RAM_BASE (phys 0 in QEMU `xp`), matching pic's
 * `v_` / `v__name_N` naming.  Absolute KSEG SFR `at` bases are skipped. */
static void mips_asm_equ_dump(pfile_t *pf)
{
  pfile_proc_t *proc;

  pfile_write(pf, pfile_write_asm, "; MIPS RAM symbols (offset from 0x%08lx)\n",
      (unsigned long) MIPS_RAM_BASE);
  for (proc = pfile_proc_root_get(pf);
       proc;
       proc = pfile_proc_next(proc)) {
    pfile_block_t *blk;

    for (blk = pfile_proc_block_root_get(proc);
         blk;
         blk = pfile_block_next(blk)) {
      variable_t vptr;

      for (vptr = pfile_block_variable_list_head(blk);
           vptr;
           vptr = variable_link_get(vptr)) {
        const char     *nm;
        unsigned        tag_n;
        variable_base_t base;

        if (!variable_assign_ct_get(vptr) && !variable_use_ct_get(vptr)) {
          continue;
        }
        if (VARIABLE_DEF_TYPE_FUNCTION == variable_type_get(vptr)) {
          continue;
        }
        if (variable_is_const(vptr)) {
          continue;
        }
        nm = variable_name_get(vptr);
        if (!nm || !*nm) {
          continue;
        }
        base = variable_base_get(vptr, 0);
        if (VARIABLE_BASE_UNKNOWN == base) {
          continue;
        }
        /* Explicit `at` in KSEG (SFR) — not in the phys-0 QEMU dump. */
        if ((unsigned long) base >= 0x80000000UL) {
          continue;
        }
        tag_n = variable_tag_n_get(vptr);
        /* Match libpic12 pic_tag_dump: tagged = v___name_N, plain = v_name */
        if (tag_n) {
          pfile_write(pf, pfile_write_asm,
              "v___%s_%u EQU 0x%08x  ; %s\n",
              nm, tag_n, (unsigned) base, nm);
        } else {
          pfile_write(pf, pfile_write_asm,
              "v_%s EQU 0x%08x  ; %s\n",
              nm, (unsigned) base, nm);
        }
      }
    }
  }
  pfile_write(pf, pfile_write_asm, "\n");
}

/* Instruction listing: walks the SAME final laid-out code list
 * mips_hex_write serializes, in the same order, after every
 * fixup/layout pass -- so it cannot diverge from the object output. */
static const char *mips_reg_name(unsigned r)
{
  static const char *const regs[] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"
  };

  return (r < 32) ? regs[r] : "?";
}

static const char *mips_op_name(mips_opcode_t op)
{
  static const char *const names[] = {
    "none", "lui", "addiu", "ori", "andi", "xori",
    "lb", "lbu", "lh", "lhu", "lw", "sb", "sh", "sw",
    "addu", "subu", "and", "or", "xor",
    "sll", "srl", "sra", "sllv", "srlv", "srav",
    "mult", "multu", "div", "divu", "mflo", "mfhi",
    "slt", "sltu",
    "beq", "bne", "j", "jal", "jalr", "jr",
    "mfc0", "mtc0", "cache", "sync", "ehb", "eret", "mtc1", "mfc1",
    "add.s", "sub.s", "mul.s", "div.s", "neg.s",
    "cvt.s.w", "cvt.w.s", "c.eq.s", "c.lt.s", "c.le.s",
    "bc1t", "bc1f", "org", "nop", "label", "wait", "ei", "clz"
  };

  return ((unsigned) op < COUNT(names)) ? names[op] : "?";
}

static void mips_asm_listing(pfile_t *pf)
{
  mips_code_t c;

  pfile_write(pf, pfile_write_asm,
      "; instruction listing (post-layout, same list as the HEX)\n");
  for (c = mips_code_list_head(pf); c; c = c->link) {
    unsigned long word;
    unsigned     sz;

    if (MIPS_OP_LABEL == c->op) {
      pfile_write(pf, pfile_write_asm, "%08lx:             <%lx>\n",
          c->addr, c->addr);
      continue;
    }
    if (MIPS_OP_ORG == c->op) {
      pfile_write(pf, pfile_write_asm, "%08lx:             .org\n",
          c->addr);
      continue;
    }
    sz = mips_inst_size(c);
    if (!sz) {
      continue;
    }
    word = mips_inst_encode(c);
    pfile_write(pf, pfile_write_asm, "%08lx: %04lx %04lx  ",
        c->addr, (word >> 16) & 0xFFFF, word & 0xFFFF);
    switch (c->op) {
      /* I-type: opcode rt, imm(rs) / shifts with sa */
      case MIPS_OP_LUI:
      case MIPS_OP_ADDIU:
      case MIPS_OP_ORI:
      case MIPS_OP_ANDI:
      case MIPS_OP_XORI:
        pfile_write(pf, pfile_write_asm, "%s $%s, $%s, 0x%x",
            mips_op_name(c->op), mips_reg_name(c->rt),
            mips_reg_name(c->rs), (unsigned) c->imm);
        break;
      case MIPS_OP_LB:
      case MIPS_OP_LBU:
      case MIPS_OP_LH:
      case MIPS_OP_LHU:
      case MIPS_OP_LW:
      case MIPS_OP_SB:
      case MIPS_OP_SH:
      case MIPS_OP_SW:
        pfile_write(pf, pfile_write_asm, "%s $%s, 0x%x($%s)",
            mips_op_name(c->op), mips_reg_name(c->rt),
            (unsigned) c->imm, mips_reg_name(c->rs));
        break;
      case MIPS_OP_CACHE:
        pfile_write(pf, pfile_write_asm, "%s 0x%x, 0x%x($%s)",
            mips_op_name(c->op), c->rt,
            (unsigned) c->imm, mips_reg_name(c->rs));
        break;
      case MIPS_OP_SLL:
      case MIPS_OP_SRL:
      case MIPS_OP_SRA:
        pfile_write(pf, pfile_write_asm, "%s $%s, $%s, %u",
            mips_op_name(c->op), mips_reg_name(c->rd),
            mips_reg_name(c->rt), (unsigned) c->imm);
        break;
      case MIPS_OP_SLLV:
      case MIPS_OP_SRLV:
      case MIPS_OP_SRAV:
      case MIPS_OP_ADDU:
      case MIPS_OP_SUBU:
      case MIPS_OP_AND:
      case MIPS_OP_OR:
      case MIPS_OP_XOR:
      case MIPS_OP_SLT:
      case MIPS_OP_SLTU:
        pfile_write(pf, pfile_write_asm, "%s $%s, $%s, $%s",
            mips_op_name(c->op), mips_reg_name(c->rd),
            mips_reg_name(c->rs), mips_reg_name(c->rt));
        break;
      case MIPS_OP_MULT:
      case MIPS_OP_MULTU:
      case MIPS_OP_DIV:
      case MIPS_OP_DIVU:
        pfile_write(pf, pfile_write_asm, "%s $%s, $%s",
            mips_op_name(c->op), mips_reg_name(c->rs),
            mips_reg_name(c->rt));
        break;
      case MIPS_OP_MFLO:
      case MIPS_OP_MFHI:
        pfile_write(pf, pfile_write_asm, "%s $%s",
            mips_op_name(c->op), mips_reg_name(c->rd));
        break;
      case MIPS_OP_CLZ:
        pfile_write(pf, pfile_write_asm, "%s $%s, $%s",
            mips_op_name(c->op), mips_reg_name(c->rd),
            mips_reg_name(c->rs));
        break;
      case MIPS_OP_BEQ:
      case MIPS_OP_BNE:
        pfile_write(pf, pfile_write_asm, "%s $%s, $%s, %08lx",
            mips_op_name(c->op), mips_reg_name(c->rs),
            mips_reg_name(c->rt),
            c->target ? c->target->addr : 0UL);
        break;
      case MIPS_OP_J:
      case MIPS_OP_JAL:
        pfile_write(pf, pfile_write_asm, "%s %08lx",
            mips_op_name(c->op),
            c->target ? c->target->addr : 0UL);
        break;
      case MIPS_OP_JR:
        pfile_write(pf, pfile_write_asm, "jr $%s", mips_reg_name(c->rs));
        break;
      case MIPS_OP_JALR:
        pfile_write(pf, pfile_write_asm, "jalr $%s, $%s",
            mips_reg_name(c->rd), mips_reg_name(c->rs));
        break;
      default:
        pfile_write(pf, pfile_write_asm, "%s", mips_op_name(c->op));
        break;
    }
    pfile_write(pf, pfile_write_asm, "\n");
  }
  pfile_write(pf, pfile_write_asm, "\n");
}

/* The code list is attached to the pfile so multiple compilations
 * (and the cleanup pass) stay independent. */
static mips_code_t mips_list_head;
static mips_code_t mips_list_tail;   /* tail pointer for O(1) append */

variable_sz_t mips_pointer_size_get(pfile_t *pf)
{
  UNUSED(pf);
  return 4;
}

/* the descriptor published through libpic12's back-end registry
 * (jal_main registers it for PIC_TARGET_CPU_MIPS32); the clock hook
 * adapts mips_clock_solve's boolean_t return to the descriptor shape */
static void mips_clock_solve_maybe(pfile_t *pf)
{
  (void) mips_clock_solve(pf);
}

const pic_backend_t *pic32_backend_get(void)
{
  static const pic_backend_t pic32_backend = {
    mips_cmd_generate,
    mips_cmd_dump,
    mips_code_cleanup,
    mips_code_emu,
    mips_pointer_size_get,
    mips_clock_solve_maybe,
    BOOLEAN_TRUE              /* task_locals_allowed */
  };

  return &pic32_backend;
}

mips_code_t mips_code_list_head(pfile_t *pf)
{
  UNUSED(pf);
  return mips_list_head;
}

mips_code_t mips_code_list_tail(pfile_t *pf)
{
  UNUSED(pf);
  return mips_list_tail;
}

void mips_code_list_set_ends(pfile_t *pf, mips_code_t head, mips_code_t tail)
{
  UNUSED(pf);
  mips_list_head = head;
  mips_list_tail = tail;
}

void mips_code_list_reset(pfile_t *pf)
{
  UNUSED(pf);
  while (mips_list_head) {
    mips_code_t next = mips_list_head->link;
    FREE(mips_list_head);
    mips_list_head = next;
  }
  mips_list_tail = 0;            /* avoid a dangling tail on reuse */
  mips_op_reset();              /* clear branch fixups + skip marker */
}

/* append a node to the tail (kept via a tail pointer for O(1) append) */
static mips_code_t mips_code_append(mips_code_t node)
{
  if (!mips_list_head) {
    mips_list_head = node;
  } else {
    mips_list_tail->link = node;
  }
  mips_list_tail = node;
  node->link = NULL;
  return node;
}

mips_code_t mips_code_alloc(mips_opcode_t op, unsigned rt,
    unsigned rs, int imm)
{
  mips_code_t node = MALLOC(sizeof(*node));

  if (node) {
    memset(node, 0, sizeof(*node));
    node->op  = op;
    node->rt  = rt;
    node->rs  = rs;
    node->imm = imm;
    node->enc_sz = (unsigned short) ((MIPS_OP_LABEL == op)
        || (MIPS_OP_ORG == op) ? 0 : MIPS_WORD_SZ);
    mips_code_append(node);
  }
  return node;
}

mips_code_t mips_code_r_alloc(mips_opcode_t op, unsigned rd,
    unsigned rs, unsigned rt, int sa)
{
  mips_code_t node = MALLOC(sizeof(*node));

  if (node) {
    memset(node, 0, sizeof(*node));
    node->op = op;
    node->rd = rd;
    node->rs = rs;
    node->rt = rt;
    node->imm = sa;
    node->enc_sz = (unsigned short) MIPS_WORD_SZ;
    mips_code_append(node);
  }
  return node;
}

mips_code_t mips_code_label_alloc(void)
{
  /* Labels are BB boundaries for the emit-time value cache. */
  mips_reg_cache_reset();
  return mips_code_alloc(MIPS_OP_LABEL, 0, 0, 0);
}

boolean_t mips_marker_present(pfile_t *pf, const char *name)
{
  label_t lbl;

  lbl = pfile_label_find(pf, PFILE_LOG_NONE, name);
  if (lbl) {
    label_release(lbl);
    return BOOLEAN_TRUE;
  }
  return BOOLEAN_FALSE;
}

void mips_marker_ensure(pfile_t *pf, const char *name)
{
  label_t lbl;

  lbl = pfile_label_find(pf, PFILE_LOG_NONE, name);
  if (!lbl) {
    lbl = pfile_label_alloc(pf, name);
  }
  if (lbl) {
    label_release(lbl);
  }
}

boolean_t mips_micromips_enabled(pfile_t *pf)
{
  return mips_marker_present(pf, MIPS_MICROMIPS_LABEL);
}

static int mips_mm_bound;

void mips_isa_bind(pfile_t *pf)
{
  mips_mm_bound = mips_micromips_enabled(pf) ? 1 : 0;
}

int mips_isa_is_micromips(void)
{
  return mips_mm_bound;
}

boolean_t mips_mvec_enabled(pfile_t *pf)
{
  return mips_marker_present(pf, MIPS_MVEC_LABEL);
}

/* Effective interrupt context: shadow register set (SRS) or software
 * save/restore.  `pragma interrupt fast` opts in explicitly; `pragma
 * interrupt raw` opts out explicitly; otherwise a SINGLE-VECTOR
 * interrupt in a loader/PFM image defaults to SRS (2026-10-17:
 * silicon-proven shape -- PRISS mapping + seeded $sp/$gp, no 26-reg
 * push).  Boot-flash images, numbered-IRQ/MVEC programs, and every
 * other target keep the universally-safe software save.  Codegen is
 * identical for QEMU and silicon (QEMU lacks the SRS switch, making
 * it the harsher -- never more permissive -- gate). */
boolean_t mips_isr_srs_active(pfile_t *pf)
{
  pfile_proc_t *proc;

  /* The MX1/2 has no PRISS register, no INTCON.SS0 and no FSRS config
   * field: shadow register sets are not available, so every ISR uses
   * the software save.  (mips_isr_vector_emit rejects `pragma
   * interrupt fast` explicitly.) */
  if (MIPS_FAMILY_MX1 == mips_family(pf)) {
    return BOOLEAN_FALSE;
  }
  if (pfile_flag_test(pf, PFILE_FLAG_MISC_INTERRUPT_FAST)) {
    return BOOLEAN_TRUE;
  }
  if (pfile_flag_test(pf, PFILE_FLAG_MISC_INTERRUPT_RAW)) {
    return BOOLEAN_FALSE;
  }
  if (!mips_loader_pfm(pf) || !pfile_isr_entry_active(pf)) {
    return BOOLEAN_FALSE;
  }
  if (mips_mvec_enabled(pf)) {
    return BOOLEAN_FALSE;
  }
  for (proc = pfile_proc_root_get(pf); proc; proc = pfile_proc_next(proc)) {
    if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INTERRUPT)
        && (pfile_proc_irq_get(proc) >= 0)) {
      return BOOLEAN_FALSE;
    }
  }
  return BOOLEAN_TRUE;
}

boolean_t mips_loader_pfm(pfile_t *pf)
{
  return mips_marker_present(pf, MIPS_LOADER_PFM_LABEL);
}

unsigned long mips_code_base(pfile_t *pf)
{
  ulong off;
  ulong loc;

  if (!mips_loader_pfm(pf)) {
    loc = pfile_code_location_get(pf);
    if (loc) {
      return loc;   /* pragma code_location (device default or program) */
    }
    return MIPS_FLASH_BASE;
  }
  off = pfile_loader_offset_get(pf);
  if (off) {
    return (unsigned long) off;  /* pragma bootloader always wins */
  }
  return MIPS_FLASH_PFM;
}

/* Boot-flash builds lay the code body out at the KSEG0 mirror of boot
 * flash and enter through a 4-word KSEG1 trampoline at reset: KSEG1 is
 * architecturally uncached, so this is the only way bare-metal code can
 * benefit from the L1 cache enabled by the silicon init.  Loader/PFM
 * images are placed in KSEG0 already; an explicit `pragma code`
 * placement is respected as-is (no mirror). */
static boolean_t mips_k0_mirror_active(pfile_t *pf)
{
  ulong loc;

  if (mips_loader_pfm(pf)) {
    return BOOLEAN_FALSE;
  }
  /* The front end defaults code_location to the device boot sector
   * (0xBFC00000 for PIC32); mirror only when the effective base IS
   * boot flash -- an explicit non-default placement is respected. */
  loc = pfile_code_location_get(pf);
  return (boolean_t) (!loc || (MIPS_FLASH_BASE == loc));
}

/* PIC32MX multi-vector mode: a numbered/mvec interrupt program on the
 * MX1/2 family.  MX has no EVIC OFFx registers -- its hardware table is
 * 64 32-byte stubs at EBASE+0x200, so a full table needs the app code
 * moved past it (MIPS_ISR_MVEC_OFS) and EBASE pinned to the app base. */
boolean_t mips_mx_mvec_active(pfile_t *pf)
{
  pfile_proc_t *proc;

  if (MIPS_FAMILY_MX1 != mips_family(pf)) {
    return BOOLEAN_FALSE;
  }
  if (mips_mvec_enabled(pf)) {
    return BOOLEAN_TRUE;
  }
  for (proc = pfile_proc_root_get(pf); proc; proc = pfile_proc_next(proc)) {
    if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INTERRUPT)
        && (pfile_proc_irq_get(proc) >= 0)) {
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

unsigned long mips_isr_vector_addr(pfile_t *pf)
{
  if (mips_mx_mvec_active(pf)) {
    return mips_code_base(pf) + MIPS_ISR_MVEC_OFS;
  }
  return mips_code_base(pf) + MIPS_ISR_OFS;
}

/* EBASE vector page.  Loader/PFM images with an interrupt entry keep
 * the vectors in their own PFM app region (silicon-proven; the EIC
 * delivery from the boot-flash KSEG0 mirror page wedges the core).
 * Boot-flash ISR images were moved to a PFM vector page on 2026-10-17
 * and REVERTED the same day: the shape never dispatched on silicon and
 * it broke QEMU coverage (QEMU's EVIC dispatches only the BEV bootstrap
 * vector 0xBFC00400 -- every EBASE-style image, loader images included,
 * fails under QEMU).  They keep the BEV shape: QEMU-true, silicon
 * dispatch open (blocked: this bench's boot flash reached erase
 * end-of-life mid-investigation). */
unsigned long mips_ebase_page(pfile_t *pf)
{
  if (MIPS_FAMILY_MX1 == mips_family(pf)) {
    unsigned long size = mips_code_size(pf);
    unsigned long base = mips_code_base(pf);

    /* MVEC keeps EBASE at the app base: the 64-slot table lives at
     * EBASE+0x200 and the code was moved to +0x1000. */
    if (mips_mx_mvec_active(pf)) {
      return base;
    }

    /* The MX1/2 boot flash is 3 KB, so the MZ page (0x9FC01000) does
     * not exist.  Use the top 4 KB of program flash (the XC32
     * exception_mem convention); mips_image_end()/the HEX writer cap
     * the app just below it. */
    if (mips_loader_pfm(pf) && (size > 0x2000UL)) {
      return base + size - 0x1000UL;
    }
    return base;
  }
  if (mips_loader_pfm(pf) && pfile_isr_entry_active(pf)) {
    return mips_code_base(pf);
  }
  return MIPS_EBASE_PAGE;
}

unsigned mips_inst_size(const mips_code_t code)
{
  if (!code || (MIPS_OP_LABEL == code->op) || (MIPS_OP_ORG == code->op)) {
    return 0;
  }
  if (code->enc_sz) {
    return (unsigned) code->enc_sz;
  }
  return (unsigned) MIPS_WORD_SZ;
}

/* lay out: enc_sz bytes per instruction, starting at the reset / PFM base */
void mips_code_layout(pfile_t *pf)
{
  mips_code_t   c;
  unsigned long addr;

  addr = mips_code_base(pf);
  for (c = mips_list_head; c; c = c->link) {
    if (MIPS_OP_ORG == c->op) {
      addr = c->addr;       /* hole: next insn at the preset address */
    } else if (MIPS_OP_LABEL == c->op) {
      c->addr = addr;       /* label takes no space */
    } else {
      c->addr = addr;
      addr += (unsigned long) mips_inst_size(c);
    }
  }
}

/* Silicon bring-up init (2026-09-30 first-silicon finding, see
 * fixes/fix_pic32_mini32_silicon_bringup.md): until the prefetch
 * module is configured and the KSEG0 L1 cache is invalidated and
 * enabled (CP0 Config.K0), every program-flash fetch costs ~8 cycles
 * on real hardware (ECC + wait states, no cache).  PRECON (0xBF8E0000)
 * gets PREFEN = 11 plus PFMWS derived from target_clock per DS60001320
 * Table 37-13 ("with ECC" column, safe for both ECC settings); the
 * caches must be invalidated by index before enabling (garbage tags
 * at reset hang the core).  The cache op has no microMIPS emission
 * here, so that opt-in mode gets the PRECON write only.  Touches
 * $t0/$t1 only, and starts from a label to drop value-cache state. */
static mips_code_t mips_cmd_entry;

/* Hit-invalidate every cache line over [0x80000000, 0x80000000+count*stride)
 * using $t0/$t1; a hit-invalidate on a non-cached address is a no-op. */
static void mips_cache_sweep(unsigned op, unsigned count, int stride)
{
  mips_code_t loop;

  (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T0, 0, 0x8000);
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1, MIPS_REG_ZERO,
      (int) count);
  loop = mips_code_label_alloc();
  (void) mips_code_alloc(MIPS_OP_CACHE, op, MIPS_REG_T0, 0);
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1, MIPS_REG_T1, -1);
  {
    mips_code_t bne = mips_code_alloc(MIPS_OP_BNE, MIPS_REG_T1,
        MIPS_REG_ZERO, 0);

    if (bne) {
      bne->target = loop;
    }
  }
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T0, MIPS_REG_T0,
      stride);
}

static mips_code_t mips_silicon_startup_init(pfile_t *pf)
{
  value_t    freq;
  unsigned long clock;
  int        ws;
  mips_code_t loop;
  mips_code_t entry = mips_code_label_alloc();

  freq = pfile_value_find(pf, PFILE_LOG_NONE, "target_clock");
  clock = freq ? (unsigned long) value_const_get(freq) : 0;
  if (freq) {
    value_release(freq);
  }
  ws = !clock ? 2
     : ((clock <= 60000000UL) ? 0
     : ((clock <= 120000000UL) ? 1 : 2));

  if (MIPS_FAMILY_MZ_EF == mips_family(pf)) {
    /* MZ prefetch/wait-state module.  The MX1/2 has no PRECON (and no
     * CHECON); writing this address there is at best ignored and at
     * worst a bus error, so it is family-gated. */
    (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T0, 0, 0xBF8E);
    (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1, MIPS_REG_ZERO,
        0x30 | ws);
    (void) mips_code_alloc(MIPS_OP_SW, MIPS_REG_T1, MIPS_REG_T0, 0);
  }

  if (mips_micromips_enabled(pf)) {
    return entry;
  }

  if (MIPS_FAMILY_MX1 == mips_family(pf)) {
    /* MX1/2: no documented L1 geometry.  Hit-invalidate every line
     * over 32 KB with a 16-byte stride (covers 16- or 32-byte lines);
     * hit-invalidate on an address that is not cached is a no-op, so
     * this is safe whether or not the part has caches. */
    mips_cache_sweep(0x10, 2048, 16);
    mips_cache_sweep(0x11, 2048, 16);
  } else {

  /* I-cache: hit-invalidate by index, 32-byte stride over KSEG0 */
  (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T0, 0, 0x8000);
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1, MIPS_REG_ZERO, 1024);
  loop = mips_code_label_alloc();
  (void) mips_code_alloc(MIPS_OP_CACHE, 0x10, MIPS_REG_T0, 0);
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1, MIPS_REG_T1, -1);
  {
    mips_code_t bne = mips_code_alloc(MIPS_OP_BNE, MIPS_REG_T1,
        MIPS_REG_ZERO, 0);

    bne->target = loop;
  }
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T0, MIPS_REG_T0, 32);

  /* D-cache: hit-invalidate by index, 16-byte stride over KSEG0 */
  (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T0, 0, 0x8000);
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1, MIPS_REG_ZERO, 2048);
  loop = mips_code_label_alloc();
  (void) mips_code_alloc(MIPS_OP_CACHE, 0x11, MIPS_REG_T0, 0);
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1, MIPS_REG_T1, -1);
  {
    mips_code_t bne = mips_code_alloc(MIPS_OP_BNE, MIPS_REG_T1,
        MIPS_REG_ZERO, 0);

    bne->target = loop;
  }
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T0, MIPS_REG_T0, 16);
  }

  /* enable the KSEG0 cache: Config.K0 = 0b011 (cacheable) */
  (void) mips_code_r_alloc(MIPS_OP_MFC0, MIPS_CP0_CONFIG, 0,
      MIPS_REG_T0, 0);
  (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T0, MIPS_REG_T0, 3);
  (void) mips_code_r_alloc(MIPS_OP_MTC0, MIPS_CP0_CONFIG, 0,
      MIPS_REG_T0, 0);
  (void) mips_code_alloc(MIPS_OP_SYNC, 0, 0, 0);
  (void) mips_code_alloc(MIPS_OP_EHB, 0, 0, 0);

  /* Enter the KSEG0 mirror of the code that follows (clear bit 29 of
   * the laid-out KSEG1 address): KSEG1 is architecturally uncached, so
   * the cache enabled above only pays off after this jump.  The low 28
   * bits are identical in both views, so J targets and relative
   * branches stay correct; absolute label values (const pool,
   * function pointers) remain KSEG1 and simply run uncached. */
  if (mips_k0_mirror_active(pf)) {
    label_t k0entry;

    k0entry = pfile_label_alloc(pf, "_mips_k0_entry");
    mips_emit_label_addr(MIPS_REG_T0, k0entry);
    (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T1, 0, 0xDFFF);
    (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T1, MIPS_REG_T1,
        0xFFFF);
    (void) mips_code_r_alloc(MIPS_OP_AND, MIPS_REG_T0, MIPS_REG_T0,
        MIPS_REG_T1, 0);
    (void) mips_code_r_alloc(MIPS_OP_JR, 0, MIPS_REG_T0, 0, 0);
    (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
    {
      mips_code_t node = mips_code_label_alloc();

      if (k0entry && node) {
        label_code_set(k0entry, node);
        label_release(k0entry);
      }
    }
  }
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
  return entry;
}

/* Loader/PFM boot-flash bring-up (2026-10-04 silicon finding, see
 * fixes/fix_pic32_bare_metal_200mhz.md): a PFM app entered by a bare
 * jump from reset boots 0/10 on silicon -- the app's own prologue runs
 * too late to establish the pre-entry state.  This is the
 * compiler-emitted form of the hand init proven 10/10 on silicon:
 * settle, Status BEV/ERL cleanup + EXL, sp, app-region cache
 * invalidation, Config.K0, timer quiet, EBASE with restart vectors,
 * EVIC clear, INTCON MVEC, PRECON -- then enter the PFM app.  Emitted
 * after all other code with no ORG-back, so the layout places it as an
 * isolated boot-flash block; the entry uses lui/jr because a J from
 * boot flash cannot cross PC regions into KSEG0.  $t0/$t1 only. */
static void mips_loader_boot_init_emit(pfile_t *pf)
{
  value_t       freq;
  unsigned long clock;
  unsigned long base = mips_code_base(pf);
  unsigned long top = mips_ram_top(pf);
  int           ws;
  int           hi = (int) ((top >> 16) & 0xFFFF);
  int           lo = (int) (top & 0xFFFF);
  int           i;
  int           intcon_val;
  pfile_proc_t *sc_proc;
  mips_code_t   org;
  mips_code_t   loop;
  mips_code_t   jexp;

  freq = pfile_value_find(pf, PFILE_LOG_NONE, "target_clock");
  clock = freq ? (unsigned long) value_const_get(freq) : 0;
  if (freq) {
    value_release(freq);
  }
  ws = !clock ? 2
     : ((clock <= 60000000UL) ? 0
     : ((clock <= 120000000UL) ? 1 : 2));
  if (lo >= 0x8000) { hi = (hi + 1) & 0xFFFF; lo -= 0x10000; }

  org = mips_code_alloc(MIPS_OP_ORG, 0, 0, 0);
  if (org) {
    org->addr = MIPS_FLASH_BASE;
  }

  /* settle before the first CP0/SFR touch */
  for (i = 0; i < 8; i++) {
    (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
  }

  /* Status: clear BEV (22) and ERL (2), set EXL (1) */
  (void) mips_code_r_alloc(MIPS_OP_MFC0, MIPS_CP0_STATUS, 0,
      MIPS_REG_T0, 0);
  (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T1, 0, 0xFFBF);
  (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T1, MIPS_REG_T1, 0xFFFF);
  (void) mips_code_r_alloc(MIPS_OP_AND, MIPS_REG_T0, MIPS_REG_T0,
      MIPS_REG_T1, 0);
  (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T1, 0, 0xFFFF);
  (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T1, MIPS_REG_T1, 0xFFFD);
  (void) mips_code_r_alloc(MIPS_OP_AND, MIPS_REG_T0, MIPS_REG_T0,
      MIPS_REG_T1, 0);
  (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T0, MIPS_REG_T0, 2);
  (void) mips_code_r_alloc(MIPS_OP_MTC0, MIPS_CP0_STATUS, 0,
      MIPS_REG_T0, 0);

  /* sp: top of SRAM (the app re-inits it; part of the proven set) */
  (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_SP, 0, hi);
  if (lo) {
    (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_SP, MIPS_REG_SP, lo);
  }

  /* I-cache index-invalidate over the first 16 K of the app (op 8).
   * ISR images first zero ErrCtl/TagLo -- the canonical cache-op prelude
   * (XC32's cache init does the same). */
  if (mips_loader_pfm(pf) && pfile_isr_entry_active(pf)) {
    (void) mips_code_r_alloc(MIPS_OP_MTC0, MIPS_CP0_ERRCTL, 0,
        MIPS_REG_ZERO, 0);
    (void) mips_code_r_alloc(MIPS_OP_MTC0, MIPS_CP0_TAGLO, 0,
        MIPS_REG_ZERO, 0);
    (void) mips_code_alloc(MIPS_OP_EHB, 0, 0, 0);
  }
  (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T0, 0,
      (int) ((base >> 16) & 0xFFFF));
  (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T1, MIPS_REG_T0, 0x4000);
  loop = mips_code_label_alloc();
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T0, MIPS_REG_T0, 16);
  {
    mips_code_t bne = mips_code_alloc(MIPS_OP_BNE, MIPS_REG_T0,
        MIPS_REG_T1, 0);

    bne->target = loop;
  }
  (void) mips_code_alloc(MIPS_OP_CACHE, 8, MIPS_REG_T0, -4);

  /* D-cache index-invalidate (op 9) */
  (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T0, 0, 0x8000);
  (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T1, MIPS_REG_T0, 0x2000);
  loop = mips_code_label_alloc();
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T0, MIPS_REG_T0, 16);
  {
    mips_code_t bne = mips_code_alloc(MIPS_OP_BNE, MIPS_REG_T0,
        MIPS_REG_T1, 0);

    bne->target = loop;
  }
  (void) mips_code_alloc(MIPS_OP_CACHE, 9, MIPS_REG_T0, -4);
  (void) mips_code_alloc(MIPS_OP_SYNC, 0, 0, 0);

  /* Config.K0 = 0b011 (cacheable KSEG0) */
  (void) mips_code_r_alloc(MIPS_OP_MFC0, MIPS_CP0_CONFIG, 0,
      MIPS_REG_T0, 0);
  (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T1, 0, 0xFFFF);
  (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T1, MIPS_REG_T1, 0xFFF8);
  (void) mips_code_r_alloc(MIPS_OP_AND, MIPS_REG_T0, MIPS_REG_T0,
      MIPS_REG_T1, 0);
  (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T0, MIPS_REG_T0, 3);
  (void) mips_code_r_alloc(MIPS_OP_MTC0, MIPS_CP0_CONFIG, 0,
      MIPS_REG_T0, 0);

  /* Count = 0, Compare = -1 (timer quiet) */
  (void) mips_code_r_alloc(MIPS_OP_MTC0, MIPS_CP0_COUNT, 0,
      MIPS_REG_ZERO, 0);
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1, MIPS_REG_ZERO, -1);
  (void) mips_code_r_alloc(MIPS_OP_MTC0, MIPS_CP0_COMPARE, 0,
      MIPS_REG_T1, 0);

  /* EBASE = the vector page (CP0 15, select 1): the PFM app base for
   * loader images with an interrupt entry (see mips_ebase_page), the
   * boot-flash mirror page otherwise. */
  {
    unsigned long ebase = mips_ebase_page(pf);

    (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T0, 0,
        (int) ((ebase >> 16) & 0xFFFF));
    (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T0, MIPS_REG_T0,
        (int) (ebase & 0xFFFF));
    (void) mips_code_r_alloc(MIPS_OP_MTC0, MIPS_CP0_EBASE, 0,
        MIPS_REG_T0, 1);
  }

  /* EVIC clear: OFFx register block, 214 consecutive words (MZ only;
   * the MX1/2 has no OFFx block). */
  if (MIPS_FAMILY_MZ_EF == mips_family(pf)) {
    (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T0, 0,
        (int) ((MIPS_OFF0 >> 16) & 0xFFFF));
    (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T0, MIPS_REG_T0,
        (int) (MIPS_OFF0 & 0xFFFF));
    (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1, MIPS_REG_ZERO,
        0xD6);
    loop = mips_code_label_alloc();
    (void) mips_code_alloc(MIPS_OP_SW, MIPS_REG_ZERO, MIPS_REG_T0, 0);
    (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T0, MIPS_REG_T0, 4);
    (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1, MIPS_REG_T1, -1);
    {
      mips_code_t bne = mips_code_alloc(MIPS_OP_BNE, MIPS_REG_T1,
          MIPS_REG_ZERO, 0);

      bne->target = loop;
    }
    (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
  }

  /* INTCON vector mode: MVEC only for multi-vector / numbered-IRQ
   * programs.  A single-vector `pragma interrupt` program dispatches
   * with MVEC=0 -- writing MVEC here anyway (the pre-2026-10-09
   * behavior) made every interrupt on silicon run through OFF0=0:
   * PC slides into zeroed RAM, exception loop, boot-restart loop with
   * no UART output (QEMU's EVIC laxness hid it; the single-vector
   * suite body is a boot-flash image where INTCON keeps its reset 0).
   * Explicit write either way: a no-POR reprogram inherits INTCON
   * from the previously running image. */
  {
    intcon_val = 0;
    for (sc_proc = pfile_proc_root_get(pf);
         sc_proc;
         sc_proc = pfile_proc_next(sc_proc)) {
      if (pfile_proc_flag_test(sc_proc, PFILE_PROC_FLAG_INTERRUPT)
          && (pfile_proc_irq_get(sc_proc) >= 0)) {
        intcon_val = MIPS_INTCON_MVEC;
        break;
      }
    }
    if (!intcon_val && mips_mvec_enabled(pf)) {
      intcon_val = MIPS_INTCON_MVEC;
    }
  }
  {
    unsigned long intcon = (MIPS_FAMILY_MX1 == mips_family(pf))
                         ? MIPS_INTCON_MX : MIPS_INTCON;

    (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T0, 0,
        (int) ((intcon >> 16) & 0xFFFF));
    (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T1, MIPS_REG_ZERO,
        (int) intcon_val);
    (void) mips_code_alloc(MIPS_OP_SW, MIPS_REG_T1, MIPS_REG_T0, 0);
  }

  /* PRECON: prefetch + pragma-derived wait states, before entering PFM
   * (MZ only -- the MX1/2 has no PRECON/CHECON). */
  if (MIPS_FAMILY_MZ_EF == mips_family(pf)) {
    (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T0, 0, 0xBF8E);
    (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1, MIPS_REG_ZERO,
        0x30 | ws);
    (void) mips_code_alloc(MIPS_OP_SW, MIPS_REG_T1, MIPS_REG_T0, 0);
  }

  /* enter the PFM app (lui/jr: J cannot cross PC regions) */
  (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T0, 0,
      (int) ((base >> 16) & 0xFFFF));
  if (base & 0xFFFF) {
    (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T0, MIPS_REG_T0,
        (int) (base & 0xFFFF));
  }
  (void) mips_code_r_alloc(MIPS_OP_JR, 0, MIPS_REG_T0, 0, 0);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);

  /* BEV general-exception vector (0xBFC00380): clear EXL|ERL, restart */
  org = mips_code_alloc(MIPS_OP_ORG, 0, 0, 0);
  if (org) {
    org->addr = MIPS_FLASH_BASE + 0x380UL;
  }
  (void) mips_code_r_alloc(MIPS_OP_MFC0, MIPS_CP0_STATUS, 0,
      MIPS_REG_T0, 0);
  (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T1, 0, 0xFFFF);
  (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T1, MIPS_REG_T1, 0xFFF9);
  (void) mips_code_r_alloc(MIPS_OP_AND, MIPS_REG_T0, MIPS_REG_T0,
      MIPS_REG_T1, 0);
  (void) mips_code_r_alloc(MIPS_OP_MTC0, MIPS_CP0_STATUS, 0,
      MIPS_REG_T0, 0);
  (void) mips_code_alloc(MIPS_OP_EHB, 0, 0, 0);
  (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T0, 0,
      (int) ((base >> 16) & 0xFFFF));
  if (base & 0xFFFF) {
    (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T0, MIPS_REG_T0,
        (int) (base & 0xFFFF));
  }
  (void) mips_code_r_alloc(MIPS_OP_JR, 0, MIPS_REG_T0, 0, 0);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);

  /* EBASE general-exception vector (same PC region as the app: J ok --
   * true for both pages, the boot-flash mirror and the PFM base) */
  org = mips_code_alloc(MIPS_OP_ORG, 0, 0, 0);
  if (org) {
    org->addr = mips_ebase_page(pf) + MIPS_EBASE_GX;
  }
  (void) mips_code_r_alloc(MIPS_OP_MFC0, MIPS_CP0_STATUS, 0,
      MIPS_REG_T0, 0);
  (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T1, 0, 0xFFFF);
  (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T1, MIPS_REG_T1, 0xFFF9);
  (void) mips_code_r_alloc(MIPS_OP_AND, MIPS_REG_T0, MIPS_REG_T0,
      MIPS_REG_T1, 0);
  (void) mips_code_r_alloc(MIPS_OP_MTC0, MIPS_CP0_STATUS, 0,
      MIPS_REG_T0, 0);
  (void) mips_code_alloc(MIPS_OP_EHB, 0, 0, 0);
  jexp = mips_code_alloc(MIPS_OP_J, 0, 0, 0);
  if (jexp) {
    jexp->target = mips_cmd_entry;
  }
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);

  /* single-vector interrupt vector: EBASE + 0x200 (MIPS_OFF_DEFAULT
   * -- the slot the EIC fetches for EVERY interrupt in single-vector
   * mode with BEV cleared above).  Jump to the app's stub block
   * (mips_isr_vector_addr = base + MIPS_ISR_OFS; lui/jr because the
   * stub lives in another PC region).  Without this block the fetch
   * hits erased flash -- single-vector ISR in a loader image was
   * QEMU-only (its EVIC laxness + BEV=1 boot-flash bodies hid it).
   * Multi-vector loader images: MX270 silicon DISPATCHES them
   * (2026-10-24 bench, mvec_hw3 -- both numbered slots through the
   * 64x32B table; see fixes/fix_mx_bench_day_20261024.md), so the
   * earlier "OFFx dispatch is EBASE-relative while the stubs sit in
   * the app region" claim described a modeling limitation, not
   * silicon.  MZ loader MVEC remains bench-unverified. */
  if (pfile_isr_entry_active(pf) && !intcon_val) {
    unsigned long stub = mips_isr_vector_addr(pf);

    org = mips_code_alloc(MIPS_OP_ORG, 0, 0, 0);
    if (org) {
      org->addr = mips_ebase_page(pf) + MIPS_OFF_DEFAULT;
    }
    (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T0, 0,
        (int) ((stub >> 16) & 0xFFFF));
    if (stub & 0xFFFF) {
      (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T0, MIPS_REG_T0,
          (int) (stub & 0xFFFF));
    }
    (void) mips_code_r_alloc(MIPS_OP_JR, 0, MIPS_REG_T0, 0, 0);
    (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
    (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
  }
}

void mips_cmd_generate(pfile_t *pf, const cmd_t cmd)
{
  cmd_t cmd_ptr;

  mips_reg_cache_reset();
  mips_isa_bind(pf);

  /* The MX1/2 boot flash is 3 KB and the generated device file's const
   * pool alone (SFR/fuse/USERID constants) is larger than that, so a
   * boot-flash image cannot be programmed.  Require the PFM shape. */
  if ((MIPS_FAMILY_MX1 == mips_family(pf)) && !mips_loader_pfm(pf)) {
    pfile_log(pf, PFILE_LOG_ERR,
      "PIC32MX boot flash is only 3 KB (and the device-file constant "
      "pool does not fit); use `pragma target image pfm` (app in program "
      "flash + compiler boot block)");
  }

  /* pass 1: allocate variables linearly in RAM */
  mips_var_alloc(pf);
  /* Phase A/B: collect eligible scalars; assign caller-saved homes. */
  mips_alloc_run(pf, cmd);

  /* When a pragma interrupt exists: reset J over the EIC vector at
   * 0xBFC00400, then _mips_start. $sp must be live before IE is set. */
  mips_isr_vector_emit(pf);

  {
    mips_cmd_entry = mips_silicon_startup_init(pf);
  }

  /* initialize $sp to the top of SRAM (stack grows down); needed before
   * any procedure call pushes $ra. $gp holds MIPS_RAM_BASE for
   * gp-relative GPR load/store (signed 16-bit window). */
  {
    unsigned long top = mips_ram_top(pf);
    int hi = (int) ((top >> 16) & 0xFFFF);
    int lo = (int) (top & 0xFFFF);

    if (lo >= 0x8000) { hi = (hi + 1) & 0xFFFF; lo -= 0x10000; }
    (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_SP, 0, hi);
    if (lo) {
      (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_SP, MIPS_REG_SP, lo);
    }
    (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_GP, 0,
        (int) ((MIPS_RAM_BASE >> 16) & 0xFFFF));
  }

  /* Zero the bottom of SRAM before anything touches it: PIC32MZ RAM is
   * ECC-checked, and reading a never-written location can raise an ECC
   * exception.  All static homes ($gp window: variables, constants,
   * compiler loop temps) live here.  Register-only loop, 16 KB.
   * The store MUST go through the KSEG1 (uncached) view: a cached
   * store-miss performs a D-cache line fill, which READS the
   * uninitialized ECC line and faults -- the very fault this avoids. */
  {
    mips_code_t zloop;
    unsigned long zwords = 4096;

    if ((mips_data_max(pf) / 4UL) < zwords) {
      zwords = mips_data_max(pf) / 4UL;
    }
    (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T0, 0,
        (int) (((MIPS_RAM_BASE | 0x20000000UL) >> 16) & 0xFFFF));
    (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1, MIPS_REG_ZERO,
        (int) zwords);
    zloop = mips_code_label_alloc();
    (void) mips_code_alloc(MIPS_OP_SW, MIPS_REG_ZERO, MIPS_REG_T0, 0);
    (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1, MIPS_REG_T1, -1);
    {
      mips_code_t bne = mips_code_alloc(MIPS_OP_BNE, MIPS_REG_T1,
          MIPS_REG_ZERO, 0);

      bne->target = zloop;
    }
    (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T0, MIPS_REG_T0, 4);
  }
  mips_fpu_enable_cu1(pf);
  mips_task_zero(pf);
  mips_isr_enable_ie(pf);

  /* Volatile IN getters are inserted at the start of the variable's
   * block (with a skip-goto on PIC).  Fallthrough from reset would
   * execute them as the first instructions (`jr $ra` with $ra == 0).
   * Jump to `_main` like PIC's reset `goto` user entry. */
  {
    label_t user;

    user = pfile_user_entry_get(pf);
    if (user) {
      (void) mips_emit_j(user);
      label_release(user);
    }
  }

  /* Crash-into-new-image vector (see mips_silicon_startup_init): after
   * ICSP reprogramming the old program keeps executing from the L1
   * cache (no reset occurs), and the delay generator's self-invalidation
   * makes it refetch erased flash within one delay call -> exception.
   * With BEV=1 (reset default, not cleared by non-ISR programs) the
   * general exception vector is 0xBFC00200 -- UNCACHED KSEG1, i.e. the
   * NEW flash -- so a jump here re-enters the freshly programmed
   * startup (which re-initializes the caches) and the new program runs
   * with no reset, power cycle or MCLR needed.  Emitted last with no
   * ORG-back so translated user code simply follows the vector. */
  if (!pfile_isr_entry_active(pf)
      && !mips_loader_pfm(pf)) {
    /* Both bootstrap vectors (BEV=1): 0x200 = TLB refill, 0x380 =
     * general exception.  A crash on erased flash raises Reserved
     * Instruction -> tight exception loop re-fetching 0x380; the
     * instant programming completes, the vector holds the jump and
     * the next iteration enters the new startup -- self-recovering
     * even for crashes that begin during the erase window. */
    mips_code_t org;
    mips_code_t jexp;
    unsigned long vec;

    for (vec = 0x200UL; vec <= 0x380UL; vec += 0x180UL) {
      org = mips_code_alloc(MIPS_OP_ORG, 0, 0, 0);
      if (org) {
        org->addr = MIPS_FLASH_BASE + vec;
      }
      /* We arrive here FROM an exception: Status.EXL is set (ERL may
       * be).  Left set, the first exception in the new program
       * re-vectors here and the image restart-loops with I/O latched
       * (pinned: post-program crash-into-new-image ran solid-LED until
       * POR/MCLR cleared the bit).  Clear EXL|ERL before the jump. */
      (void) mips_code_r_alloc(MIPS_OP_MFC0, MIPS_CP0_STATUS, 0,
          MIPS_REG_T0, 0);
      (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T1, 0, 0xFFFF);
      (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T1, MIPS_REG_T1,
          (int) 0xFFF9);
      (void) mips_code_r_alloc(MIPS_OP_AND, MIPS_REG_T0, MIPS_REG_T0,
          MIPS_REG_T1, 0);
      (void) mips_code_r_alloc(MIPS_OP_MTC0, MIPS_CP0_STATUS, 0,
          MIPS_REG_T0, 0);
      (void) mips_code_alloc(MIPS_OP_EHB, 0, 0, 0);
      jexp = mips_code_alloc(MIPS_OP_J, 0, 0, 0);
      if (jexp) {
        jexp->target = mips_cmd_entry;
      }
      (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
    }
  }

  /* pass 2: translate cmd_t -> mips_code_t */
  for (cmd_ptr = cmd; cmd_ptr; cmd_ptr = cmd_link_get(cmd_ptr)) {
    if (cmd_is_reachable(cmd_ptr)) {
      mips_op_generate(pf, cmd_ptr);
    }
  }

  /* Soft-float + cooperative-task helpers (after main, before forever). */
  mips_float_intrinsics_create(pf);
  mips_task_intrinsics_create(pf);

  /* Expect-visible: spill remaining dirty homes before the hang loop. */
  mips_alloc_flush_dirty(pf);

  /* append a forever loop so the program does not run off the end */
  {
    mips_code_t loop = mips_code_label_alloc();
    mips_code_t beq = mips_code_alloc(MIPS_OP_BEQ,
        MIPS_REG_ZERO, MIPS_REG_ZERO, 0);

    beq->target = loop;
    (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
  }

  /* Loader/PFM images carry their own boot-flash init block (last, so
   * the ORG placement terminates the layout stream cleanly). */
  if (mips_loader_pfm(pf)) {
    mips_loader_boot_init_emit(pf);
  }

  /* resolve forward branches + JAL to float helpers */
  mips_op_resolve_fixups(pf);
  mips_opt_delay_slot_fill(pf);
  /* 16-bit microMIPS after fill so delay slots stay 32-bit. */
  mips_opt_micromips_compress(pf);

  /* MVEC slot/stub labels are held through the fixup pass, then freed. */
  mips_mvec_labels_release();
}

void mips_cmd_dump(pfile_t *pf, const cmd_t cmd, boolean_t first)
{
  UNUSED(cmd);
  if (first) {
    mips_isa_bind(pf);
    mips_code_layout(pf);
    mips_const_pool_resolve(pf);
    mips_asm_equ_dump(pf);
    mips_asm_listing(pf);
    mips_hex_write(pf);
  }
}

void mips_code_cleanup(pfile_t *pf)
{
  mips_code_list_reset(pf);
}

void mips_code_emu(pfile_t *pf)
{
  UNUSED(pf);
}
