/************************************************************
 **
 ** mips_opt.c : PIC32 (MIPS32) late IR optimizations
 **
 ** Peephole (before fill): drop identity ALU outside delay slots,
 ** fold `lui rt,0; ori rt,rt,imm` → `ori rt,$0,imm`, fold adjacent
 ** `addiu` chains when the sum fits signed 16-bit, drop redundant
 ** identical LUI in a basic block when the reg still holds imm<<16.
 ** Delay-slot fill + fall-through j (see 5a-5c).
 ** Unused $s3-$s5 home save/restore (5j) before leaf-$ra.
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include <string.h>

#include "../libutils/mem.h"
#include "../libcore/pfile.h"
#include "../libcore/pf_msg.h"
#include "mips.h"

typedef struct {
  unsigned long gpr;    /* bits 0..31 */
  unsigned      hilo;   /* bit 0 = HI, bit 1 = LO */
} mips_rset_t;

#define MIPS_HILO_HI  1u
#define MIPS_HILO_LO  2u

static unsigned long mips_gpr_bit(unsigned r)
{
  if (r >= 32 || r == MIPS_REG_ZERO) {
    return 0;
  }
  return 1UL << r;
}

static void mips_rset_add(mips_rset_t *s, unsigned r)
{
  s->gpr |= mips_gpr_bit(r);
}

static int mips_rset_overlap(mips_rset_t a, mips_rset_t b)
{
  return (a.gpr & b.gpr) || (a.hilo & b.hilo);
}

static int mips_op_is_control(mips_opcode_t op)
{
  return (MIPS_OP_BEQ == op) || (MIPS_OP_BNE == op)
      || (MIPS_OP_J == op) || (MIPS_OP_JAL == op)
      || (MIPS_OP_JR == op) || (MIPS_OP_JALR == op)
      || (MIPS_OP_ERET == op)
      || (MIPS_OP_BC1T == op) || (MIPS_OP_BC1F == op);
}

static int mips_op_fillable(mips_opcode_t op)
{
  return mips_op_is_control(op) && (MIPS_OP_ERET != op);
}

/* GPR / HILO read-write for one IR node. Layout holes and labels
 * do not touch registers. */
static void mips_code_rw(const mips_code_t c, mips_rset_t *rd,
    mips_rset_t *wr)
{
  memset(rd, 0, sizeof(*rd));
  memset(wr, 0, sizeof(*wr));
  if (!c) {
    return;
  }
  switch (c->op) {
    case MIPS_OP_LUI:
      mips_rset_add(wr, c->rt);
      break;
    case MIPS_OP_ADDIU:
    case MIPS_OP_ORI:
    case MIPS_OP_ANDI:
    case MIPS_OP_XORI:
    case MIPS_OP_LB:
    case MIPS_OP_LBU:
    case MIPS_OP_LH:
    case MIPS_OP_LHU:
    case MIPS_OP_LW:
    case MIPS_OP_MFC0:
    case MIPS_OP_MFC1:
      mips_rset_add(rd, c->rs);
      mips_rset_add(wr, c->rt);
      break;
    case MIPS_OP_SB:
    case MIPS_OP_SH:
    case MIPS_OP_SW:
    case MIPS_OP_BEQ:
    case MIPS_OP_BNE:
    case MIPS_OP_MTC0:
    case MIPS_OP_MTC1:
      mips_rset_add(rd, c->rs);
      mips_rset_add(rd, c->rt);
      break;
    case MIPS_OP_WRPGPR:
      /* copies the PREVIOUS set's rt into the CURRENT set's rd; as a
       * movable unit it reads rt and writes rd (same register names) */
      mips_rset_add(rd, c->rt);
      mips_rset_add(wr, c->rd);
      break;
    case MIPS_OP_ADDU:
    case MIPS_OP_SUBU:
    case MIPS_OP_AND:
    case MIPS_OP_OR:
    case MIPS_OP_XOR:
    case MIPS_OP_SLT:
    case MIPS_OP_SLTU:
    case MIPS_OP_SLLV:
    case MIPS_OP_SRLV:
    case MIPS_OP_SRAV:
      mips_rset_add(rd, c->rs);
      mips_rset_add(rd, c->rt);
      mips_rset_add(wr, c->rd);
      break;
    case MIPS_OP_SLL:
    case MIPS_OP_SRL:
    case MIPS_OP_SRA:
      mips_rset_add(rd, c->rt);
      mips_rset_add(wr, c->rd);
      break;
    case MIPS_OP_MULT:
    case MIPS_OP_MULTU:
    case MIPS_OP_DIV:
    case MIPS_OP_DIVU:
      mips_rset_add(rd, c->rs);
      mips_rset_add(rd, c->rt);
      wr->hilo = (unsigned) (MIPS_HILO_HI | MIPS_HILO_LO);
      break;
    case MIPS_OP_MFLO:
      rd->hilo = MIPS_HILO_LO;
      mips_rset_add(wr, c->rd);
      break;
    case MIPS_OP_MFHI:
      rd->hilo = MIPS_HILO_HI;
      mips_rset_add(wr, c->rd);
      break;
    case MIPS_OP_JR:
      mips_rset_add(rd, c->rs);
      break;
    case MIPS_OP_JALR:
      mips_rset_add(rd, c->rs);
      mips_rset_add(wr, c->rd ? c->rd : MIPS_REG_RA);
      break;
    case MIPS_OP_JAL:
      mips_rset_add(wr, MIPS_REG_RA);
      break;
    case MIPS_OP_J:
    case MIPS_OP_ERET:
    case MIPS_OP_NOP:
    case MIPS_OP_LABEL:
    case MIPS_OP_ORG:
    case MIPS_OP_NONE:
    default:
      break;
  }
}

static int mips_code_is_movable(const mips_code_t c)
{
  if (!c) {
    return 0;
  }
  if (mips_op_is_control(c->op)) {
    return 0;
  }
  switch (c->op) {
    case MIPS_OP_NOP:
    case MIPS_OP_LABEL:
    case MIPS_OP_ORG:
    case MIPS_OP_NONE:
    case MIPS_OP_MFC0:
    case MIPS_OP_MTC0:
    case MIPS_OP_WRPGPR:   /* SRSCtl-state dependent: must not float */
    case MIPS_OP_MFLO:
    case MIPS_OP_MFHI:
    case MIPS_OP_MULT:
    case MIPS_OP_MULTU:
    case MIPS_OP_DIV:
    case MIPS_OP_DIVU:
    case MIPS_OP_WAIT:
      return 0;
    default:
      return 1;
  }
}

/* Safe to execute `cand` in the delay slot of `br`. */
static int mips_delay_can_fill(const mips_code_t cand, const mips_code_t br)
{
  mips_rset_t cand_rd;
  mips_rset_t cand_wr;
  mips_rset_t br_rd;
  mips_rset_t br_wr;

  if (!mips_code_is_movable(cand) || !br) {
    return 0;
  }
  if (MIPS_OP_ERET == br->op) {
    return 0;
  }
  mips_code_rw(cand, &cand_rd, &cand_wr);
  mips_code_rw(br, &br_rd, &br_wr);
  if (mips_rset_overlap(cand_wr, br_rd)) {
    return 0;
  }
  /* JAL / JALR write $ra (or rd) in parallel with the slot. */
  if (mips_rset_overlap(cand_wr, br_wr)
      || mips_rset_overlap(cand_rd, br_wr)) {
    return 0;
  }
  return 1;
}

/* `j L; nop; L:` is a fall-through. Drop the jump and its slot. */
static unsigned mips_opt_drop_fallthrough_j(pfile_t *pf)
{
  mips_code_t head;
  mips_code_t tail;
  mips_code_t pred;
  mips_code_t c;
  unsigned    dropped;

  head = mips_code_list_head(pf);
  tail = mips_code_list_tail(pf);
  pred = 0;
  dropped = 0;
  c = head;
  while (c) {
    mips_code_t slot;
    mips_code_t lab;

    slot = c->link;
    lab = slot ? slot->link : 0;
    if ((MIPS_OP_J == c->op) && slot && (MIPS_OP_NOP == slot->op)
        && lab && (MIPS_OP_LABEL == lab->op) && (c->target == lab)) {
      if (pred) {
        pred->link = lab;
      } else {
        head = lab;
      }
      if (tail == slot) {
        tail = lab;
      } else if (tail == c) {
        tail = lab;
      }
      FREE(c);
      FREE(slot);
      dropped++;
      c = lab;
      continue;
    }
    pred = c;
    c = slot;
  }
  mips_code_list_set_ends(pf, head, tail);
  if (dropped) {
    pfile_log(pf, PFILE_LOG_DEBUG,
        "mips fallthrough-j: %u jump(s)", dropped);
  }
  return dropped;
}

static void mips_opt_check_delay_slots(pfile_t *pf)
{
  mips_code_t c;

  for (c = mips_code_list_head(pf); c; c = c->link) {
    mips_code_t slot;

    if (!mips_op_is_control(c->op)) {
      continue;
    }
    slot = c->link;
    while (slot && ((MIPS_OP_LABEL == slot->op)
        || (MIPS_OP_ORG == slot->op)
        || (MIPS_OP_NONE == slot->op))) {
      slot = slot->link;
    }
    if (!slot || mips_op_is_control(slot->op)) {
      pfile_log(pf, PFILE_LOG_ERR,
          "mips: control transfer with no delay slot (or slot is a branch)");
    }
  }
}


static void mips_code_delete(mips_code_t *head, mips_code_t *tail,
    mips_code_t pred, mips_code_t c)
{
  if (pred) {
    pred->link = c->link;
  } else {
    *head = c->link;
  }
  if (*tail == c) {
    *tail = pred;
  }
  FREE(c);
}

static int mips_imm16_ok(long v)
{
  return (v >= -32768L) && (v <= 32767L);
}

/* True if any control / LUI+ORI fixup points at `lab`. */
static int mips_label_is_referenced(pfile_t *pf, const mips_code_t lab)
{
  mips_code_t c;

  for (c = mips_code_list_head(pf); c; c = c->link) {
    if (c->target == lab) {
      return 1;
    }
  }
  return 0;
}

/*
 * Predecessor for addiu folding: skip LABEL/ORG/NONE holes that take no
 * code space. Refuse if a skipped LABEL is a branch/fixup target (merge
 * point — register state may differ on the other edge).
 */
static mips_code_t mips_code_prev_foldable(pfile_t *pf, mips_code_t pred)
{
  while (pred) {
    if ((MIPS_OP_LABEL == pred->op) || (MIPS_OP_ORG == pred->op)
        || (MIPS_OP_NONE == pred->op)) {
      if ((MIPS_OP_LABEL == pred->op)
          && mips_label_is_referenced(pf, pred)) {
        return 0;
      }
      /* need the node before `pred` — caller only has pred, not chain.
       * Walk from head (rare: few labels between a fold pair). */
      {
        mips_code_t walk;
        mips_code_t before;

        before = 0;
        for (walk = mips_code_list_head(pf); walk && walk != pred;
            walk = walk->link) {
          before = walk;
        }
        pred = before;
        continue;
      }
    }
    return pred;
  }
  return 0;
}

static int mips_code_is_identity(const mips_code_t c)
{
  if (!c) {
    return 0;
  }
  switch (c->op) {
    case MIPS_OP_ADDIU:
      return (0 == c->imm) && (c->rt == c->rs) && !c->target;
    case MIPS_OP_OR:
    case MIPS_OP_ADDU:
      if (MIPS_REG_ZERO == c->rt) {
        return c->rd == c->rs;
      }
      if (MIPS_REG_ZERO == c->rs) {
        return c->rd == c->rt;
      }
      return 0;
    case MIPS_OP_SLL:
    case MIPS_OP_SRL:
    case MIPS_OP_SRA:
      return (0 == c->imm) && (c->rd == c->rt);
    default:
      return 0;
  }
}

static unsigned mips_code_dest_reg(const mips_code_t c)
{
  mips_rset_t rd;
  mips_rset_t wr;
  unsigned    r;
  unsigned    found;

  mips_code_rw(c, &rd, &wr);
  if (wr.hilo) {
    return 32;
  }
  found = 32;
  for (r = 0; r < 32; r++) {
    if (wr.gpr & mips_gpr_bit(r)) {
      if (found != 32) {
        return 32;
      }
      found = r;
    }
  }
  return found;
}

/* peep transforms always on */
static unsigned mips_opt_peephole_once(pfile_t *pf)
{
  mips_code_t head;
  mips_code_t tail;
  mips_code_t pred_pred;
  mips_code_t pred;
  mips_code_t c;
  unsigned    removed;
  unsigned    addiu_folds;
  int         lui_valid[32];
  int         lui_imm[32];

  head = mips_code_list_head(pf);
  tail = mips_code_list_tail(pf);
  pred_pred = 0;
  pred = 0;
  removed = 0;
  addiu_folds = 0;
  memset(lui_valid, 0, sizeof(lui_valid));
  memset(lui_imm, 0, sizeof(lui_imm));
  c = head;
  while (c) {
    mips_code_t next;
    unsigned    dest;
    int         in_slot;

    next = c->link;
    in_slot = pred && mips_op_is_control(pred->op);

    if ((MIPS_OP_LABEL == c->op) || (MIPS_OP_ORG == c->op)) {
      memset(lui_valid, 0, sizeof(lui_valid));
      pred_pred = pred;
      pred = c;
      c = next;
      continue;
    }

    /* lui rt,0 ; ori rt,rt,imm(!=0) → ori rt,$0,imm.
     * Skip ori imm==0: flash const-pool fixups use lui/ori with imm 0
     * until mips_const_pool_resolve (after this pass). */
    if (pred && (MIPS_OP_LUI == pred->op) && !pred->target
        && (0 == pred->imm) && (MIPS_OP_ORI == c->op) && !c->target
        && (0 != c->imm) && (c->rt == pred->rt) && (c->rs == pred->rt)) {
      mips_code_t before_lui;

      before_lui = pred_pred;
      c->rs = MIPS_REG_ZERO;
      mips_code_delete(&head, &tail, before_lui, pred);
      removed++;
      pred = before_lui;
      in_slot = pred && mips_op_is_control(pred->op);
    }

    if (!in_slot && mips_code_is_identity(c)) {
      mips_code_delete(&head, &tail, pred, c);
      removed++;
      c = next;
      continue;
    }

    /* addiu r,x,a ; [unref labels] ; addiu r,r,b → addiu r,x,a+b.
     * Do not fold if `base` is a delay slot (e.g. `jr; addiu sp,4`
     * next to the following `addiu sp,-4` prologue). */
    {
      mips_code_t base;

      base = mips_code_prev_foldable(pf, pred);
      if (base && (MIPS_OP_ADDIU == base->op) && (MIPS_OP_ADDIU == c->op)
          && !base->target && !c->target
          && (base->rt == c->rs) && (c->rt == base->rt)) {
        mips_code_t base_pred;
        mips_code_t walk;
        int         base_in_slot;

        base_pred = 0;
        for (walk = head; walk && walk != base; walk = walk->link) {
          base_pred = walk;
        }
        base_in_slot = base_pred && mips_op_is_control(base_pred->op);
        if (!base_in_slot) {
          long sum;

          sum = (long) base->imm + (long) c->imm;
          if (mips_imm16_ok(sum)) {
            base->imm = (int) sum;
            mips_code_delete(&head, &tail, pred, c);
            removed++;
            addiu_folds++;
            c = pred ? pred->link : head;
            continue;
          }
        }
      }
    }

    /* Redundant identical LUI. Skip imm==0 — pending flash const-pool
     * LUI/ORI pairs still have imm 0 until resolve-after-layout. */
    if (!in_slot && (MIPS_OP_LUI == c->op) && !c->target && (c->rt < 32)
        && (0 != c->imm) && lui_valid[c->rt]
        && (lui_imm[c->rt] == c->imm)) {
      mips_code_delete(&head, &tail, pred, c);
      removed++;
      c = next;
      continue;
    }

    dest = mips_code_dest_reg(c);
    if (dest < 32) {
      lui_valid[dest] = 0;
    }
    if ((MIPS_OP_LUI == c->op) && !c->target && (c->rt < 32)
        && (0 != c->imm)) {
      lui_valid[c->rt] = 1;
      lui_imm[c->rt] = c->imm;
    }

    if (pred && ((MIPS_OP_J == pred->op) || (MIPS_OP_JAL == pred->op)
        || (MIPS_OP_JR == pred->op) || (MIPS_OP_JALR == pred->op)
        || (MIPS_OP_ERET == pred->op))) {
      memset(lui_valid, 0, sizeof(lui_valid));
    }

    pred_pred = pred;
    pred = c;
    c = next;
  }
  mips_code_list_set_ends(pf, head, tail);
  if (addiu_folds) {
    pfile_log(pf, PFILE_LOG_DEBUG,
        "mips peephole addiu-fold: %u", addiu_folds);
  }
  return removed;
}

static void mips_opt_peephole(pfile_t *pf)
{
  unsigned total;
  unsigned n;
  unsigned passes;

  total = 0;
  passes = 0;
  do {
    n = mips_opt_peephole_once(pf);
    total += n;
    passes++;
  } while (n && (passes < 8));
  if (total) {
    pfile_log(pf, PFILE_LOG_DEBUG,
        "mips peephole: %u insn(s) in %u pass(es)", total, passes);
  }
}

/* `addiu $sp,$sp,-4; sw $ra,0($sp)` — procedure $ra frame. */
static int mips_is_ra_prologue(const mips_code_t c)
{
  mips_code_t sw;

  if (!c || (MIPS_OP_ADDIU != c->op) || (MIPS_REG_SP != c->rt)
      || (MIPS_REG_SP != c->rs) || (-4 != c->imm)) {
    return 0;
  }
  sw = c->link;
  if (!sw || (MIPS_OP_SW != sw->op) || (MIPS_REG_RA != sw->rt)
      || (MIPS_REG_SP != sw->rs) || (0 != sw->imm)) {
    return 0;
  }
  return 1;
}

static int mips_is_s_home_reg(unsigned r)
{
  return (r >= MIPS_REG_S3) && (r <= MIPS_REG_S7);
}

/* `addiu $sp,$sp,-4; sw $sN,0($sp)` — 5j home save. */
static int mips_is_s_push(const mips_code_t c, unsigned *reg)
{
  mips_code_t sw;

  if (!c || (MIPS_OP_ADDIU != c->op) || (MIPS_REG_SP != c->rt)
      || (MIPS_REG_SP != c->rs) || (-4 != c->imm)) {
    return 0;
  }
  sw = c->link;
  if (!sw || (MIPS_OP_SW != sw->op) || (MIPS_REG_SP != sw->rs)
      || (0 != sw->imm) || !mips_is_s_home_reg(sw->rt)) {
    return 0;
  }
  if (reg) {
    *reg = sw->rt;
  }
  return 1;
}

/* `lw $sN,0($sp); addiu $sp,$sp,4` — 5j home restore. */
static int mips_is_s_pop(const mips_code_t c, unsigned *reg)
{
  mips_code_t addiu;

  if (!c || (MIPS_OP_LW != c->op) || (MIPS_REG_SP != c->rs)
      || (0 != c->imm) || !mips_is_s_home_reg(c->rt)) {
    return 0;
  }
  addiu = c->link;
  if (!addiu || (MIPS_OP_ADDIU != addiu->op) || (MIPS_REG_SP != addiu->rt)
      || (MIPS_REG_SP != addiu->rs) || (4 != addiu->imm)) {
    return 0;
  }
  if (reg) {
    *reg = c->rt;
  }
  return 1;
}

static mips_code_t mips_code_before(mips_code_t head, mips_code_t node)
{
  mips_code_t p;
  mips_code_t c;

  p = 0;
  for (c = head; c && (c != node); c = c->link) {
    p = c;
  }
  return (c == node) ? p : 0;
}

static void mips_delete_pair(mips_code_t *head, mips_code_t *tail,
    mips_code_t first)
{
  mips_code_t before;
  mips_code_t second;

  if (!first || !first->link) {
    return;
  }
  second = first->link;
  before = mips_code_before(*head, first);
  mips_code_delete(head, tail, before, first);
  mips_code_delete(head, tail, before, second);
}

static int mips_code_writes_reg(const mips_code_t c, unsigned reg)
{
  mips_rset_t rd;
  mips_rset_t wr;

  mips_code_rw(c, &rd, &wr);
  return (wr.gpr & mips_gpr_bit(reg)) ? 1 : 0;
}

/*
 * Drop unused $s3-$s7 push/pop pairs that this frame never writes. Nested
 * frames are skipped (they have their own saves). Run before leaf-$ra
 * so the $ra prologue/epilogue pattern stays adjacent. Recognizes up to
 * MIPS_S_HOME_MAX saves even while enter emits only MIPS_S_HOME_EMIT.
 */
static int mips_is_ra_epilogue(const mips_code_t c);

static void mips_opt_unused_s(pfile_t *pf)
{
  mips_code_t head;
  mips_code_t tail;
  mips_code_t c;
  unsigned    stripped;

  head = mips_code_list_head(pf);
  tail = mips_code_list_tail(pf);
  stripped = 0;
  c = head;
  while (c) {
    mips_code_t sw;
    mips_code_t scan;
    mips_code_t after_sw;
    mips_code_t epi[32];
    unsigned    epi_ct;
    unsigned    pushed[MIPS_S_HOME_MAX];
    unsigned    npush;
    unsigned    used;
    unsigned    depth;
    unsigned    i;
    unsigned    r;

    if (!mips_is_ra_prologue(c)) {
      c = c->link;
      continue;
    }
    sw = c->link;
    after_sw = sw->link;
    npush = 0;
    scan = after_sw;
    while ((npush < MIPS_S_HOME_MAX) && mips_is_s_push(scan, &r)) {
      pushed[npush++] = r;
      scan = scan->link->link;
    }
    if (0 == npush) {
      c = after_sw;
      continue;
    }
    used = 0;
    epi_ct = 0;
    depth = 1;
    while (scan && (depth > 0)) {
      if (mips_is_ra_prologue(scan)) {
        depth++;
      } else if (mips_is_ra_epilogue(scan)) {
        if ((1 == depth) && (epi_ct < 32)) {
          epi[epi_ct++] = scan;
        }
        depth--;
        scan = scan->link->link; /* jr */
        if (!scan) {
          break;
        }
        scan = scan->link;
        continue;
      } else if (1 == depth) {
        unsigned pr;

        /* Epilogue pops write $sN; they are not a use. */
        if (mips_is_s_pop(scan, &pr)) {
          scan = scan->link->link;
          continue;
        }
        for (i = 0; i < npush; i++) {
          if (mips_code_writes_reg(scan, pushed[i])) {
            used |= (1u << i);
          }
        }
      }
      scan = scan->link;
    }
    for (i = 0; i < npush; i++) {
      mips_code_t push;
      unsigned    e;

      if (used & (1u << i)) {
        continue;
      }
      push = after_sw;
      {
        unsigned k;
        unsigned pr;

        for (k = 0; k < npush; k++) {
          if (!mips_is_s_push(push, &pr)) {
            push = 0;
            break;
          }
          if (pr == pushed[i]) {
            break;
          }
          push = push->link->link;
        }
        if (!push || (k >= npush)) {
          continue;
        }
      }
      mips_delete_pair(&head, &tail, push);
      stripped++;
      if (push == after_sw) {
        after_sw = (sw && sw->link) ? sw->link : after_sw;
      }
      for (e = 0; e < epi_ct; e++) {
        mips_code_t win[MIPS_S_HOME_MAX * 2];
        unsigned    wlen;
        mips_code_t w;
        unsigned    need;
        unsigned    j;

        need = npush * 2;
        wlen = 0;
        for (w = head; w && (w != epi[e]); w = w->link) {
          if (wlen < need) {
            win[wlen++] = w;
          } else {
            for (j = 1; j < wlen; j++) {
              win[j - 1] = win[j];
            }
            win[wlen - 1] = w;
          }
        }
        if (!w || (wlen != need)) {
          continue;
        }
        for (j = 0; j < npush; j++) {
          unsigned pr;
          mips_code_t pop;

          pop = win[j * 2];
          if (!mips_is_s_pop(pop, &pr) || (pr != pushed[npush - 1 - j])) {
            pop = 0;
          }
          if (pop && (pr == pushed[i])) {
            mips_delete_pair(&head, &tail, pop);
            stripped++;
            break;
          }
        }
      }
      npush--;
      for (r = i; r < npush; r++) {
        pushed[r] = pushed[r + 1];
      }
      i--;
    }
    c = after_sw;
  }
  mips_code_list_set_ends(pf, head, tail);
  if (stripped) {
    pfile_log(pf, PFILE_LOG_DEBUG,
        "mips unused-$s: stripped %u frame insn(s)", stripped);
  }
}

/* `lw $ra,0($sp); addiu $sp,$sp,4; jr $ra` — matching epilogue. */
static int mips_is_ra_epilogue(const mips_code_t c)
{
  mips_code_t addiu;
  mips_code_t jr;

  if (!c || (MIPS_OP_LW != c->op) || (MIPS_REG_RA != c->rt)
      || (MIPS_REG_SP != c->rs) || (0 != c->imm)) {
    return 0;
  }
  addiu = c->link;
  if (!addiu || (MIPS_OP_ADDIU != addiu->op) || (MIPS_REG_SP != addiu->rt)
      || (MIPS_REG_SP != addiu->rs) || (4 != addiu->imm)) {
    return 0;
  }
  jr = addiu->link;
  if (!jr || (MIPS_OP_JR != jr->op) || (MIPS_REG_RA != jr->rs)) {
    return 0;
  }
  return 1;
}

/*
 * Leaf $ra elision: if a procedure's $ra frame contains no JAL/JALR at
 * frame depth 1, drop the push and every matching pop (keep `jr $ra`).
 * Nested procedures are emitted between the parent's prologue and
 * epilogue, so the region is tracked by $ra-frame nesting depth — not
 * "until the next prologue" (that falsely treated parents of nested
 * leaves as leaves and stripped their frames). Soft-float / task
 * helpers use JAL without pfile_proc_call_add, so this is IR-based.
 */
static void mips_opt_leaf_ra(pfile_t *pf)
{
  mips_code_t head;
  mips_code_t tail;
  mips_code_t pred;
  mips_code_t c;
  unsigned    stripped;

  head = mips_code_list_head(pf);
  tail = mips_code_list_tail(pf);
  pred = 0;
  stripped = 0;
  c = head;
  while (c) {
    mips_code_t sw;
    mips_code_t scan;
    mips_code_t after_sw;
    int         has_call;
    int         depth;
    mips_code_t epi[32];
    unsigned    epi_ct;
    unsigned    i;

    if (!mips_is_ra_prologue(c)) {
      pred = c;
      c = c->link;
      continue;
    }
    sw = c->link;
    after_sw = sw->link;
    has_call = 0;
    epi_ct = 0;
    depth = 1;
    scan = after_sw;
    while (scan && (depth > 0)) {
      if (mips_is_ra_prologue(scan)) {
        depth++;
      } else if (mips_is_ra_epilogue(scan)) {
        if ((1 == depth) && (epi_ct < 32)) {
          epi[epi_ct++] = scan;
        }
        depth--;
        /* skip lw, addiu, jr; continue after jr (delay slot or next) */
        scan = scan->link->link; /* jr */
        if (!scan) {
          break;
        }
        scan = scan->link;
        continue;
      } else if ((1 == depth)
          && ((MIPS_OP_JAL == scan->op) || (MIPS_OP_JALR == scan->op))) {
        has_call = 1;
      }
      scan = scan->link;
    }
    /* Require a matching epilogue — never strip a lone push (main /
     * unbalanced frames). Continue from after_sw so nested leaves
     * inside a kept parent are still visited. */
    if (!has_call && (epi_ct > 0) && (0 == depth)) {
      mips_code_delete(&head, &tail, pred, c);
      mips_code_delete(&head, &tail, pred, sw);
      stripped++;
      for (i = 0; i < epi_ct; i++) {
        mips_code_t lw;
        mips_code_t addiu;
        mips_code_t before;
        mips_code_t walk;

        lw = epi[i];
        addiu = lw->link;
        before = 0;
        for (walk = head; walk && walk != lw; walk = walk->link) {
          before = walk;
        }
        mips_code_delete(&head, &tail, before, lw);
        mips_code_delete(&head, &tail, before, addiu);
        stripped++;
      }
      c = after_sw;
      continue;
    }
    pred = sw;
    c = after_sw;
  }
  mips_code_list_set_ends(pf, head, tail);
  if (stripped) {
    pfile_log(pf, PFILE_LOG_DEBUG,
        "mips leaf-$ra: stripped %u frame insn(s)", stripped);
  }
}

void mips_opt_delay_slot_fill(pfile_t *pf)
{
  mips_code_t head;
  mips_code_t tail;
  mips_code_t pred_pred;
  mips_code_t pred;
  mips_code_t c;
  unsigned    filled;

  mips_opt_unused_s(pf);
  mips_opt_leaf_ra(pf);
  mips_opt_peephole(pf);
  head = mips_code_list_head(pf);
  tail = mips_code_list_tail(pf);
  pred_pred = 0;
  pred = 0;
  filled = 0;
  c = head;
  while (c) {
    mips_code_t slot;

    slot = c->link;
    if (mips_op_fillable(c->op) && slot && (MIPS_OP_NOP == slot->op)
        && pred && pred_pred
        && (MIPS_OP_LABEL != pred_pred->op)
        && (MIPS_OP_ORG != pred_pred->op)
        && !mips_op_is_control(pred_pred->op)
        && mips_delay_can_fill(pred, c)) {
      mips_code_t after;

      after = slot->link;
      if (pred_pred) {
        pred_pred->link = c;
      } else {
        head = c;
      }
      c->link = pred;
      pred->link = after;
      if (tail == slot) {
        tail = pred;
      }
      FREE(slot);
      filled++;
      pred_pred = c;
      /* pred is now the filled slot; continue after it */
      c = after;
      continue;
    }
    pred_pred = pred;
    pred = c;
    c = slot;
  }
  mips_code_list_set_ends(pf, head, tail);
  if (filled) {
    pfile_log(pf, PFILE_LOG_DEBUG,
        "mips delay-slot fill: %u slot(s)", filled);
  }
  (void) mips_opt_drop_fallthrough_j(pf);
  /* New adjacencies after fill / fall-through (e.g. across dead labels). */
  mips_opt_peephole(pf);
  mips_opt_check_delay_slots(pf);
}

static int mips_mm_can_compress(const mips_code_t c)
{
  if (!c) {
    return 0;
  }
  if (MIPS_OP_NOP == c->op) {
    return 1;
  }
  if ((MIPS_OP_ADDU == c->op) && ((0 == c->rt) || (0 == c->rs))) {
    return 1;
  }
  if ((MIPS_OP_LW == c->op) && (MIPS_REG_SP == c->rs)
      && (c->imm >= 0) && (c->imm <= 124) && (0 == (c->imm & 3))) {
    return 1;
  }
  if ((MIPS_OP_SW == c->op) && (MIPS_REG_SP == c->rs)
      && (c->imm >= 0) && (c->imm <= 124) && (0 == (c->imm & 3))) {
    return 1;
  }
  if ((MIPS_OP_ADDIU == c->op) && (MIPS_REG_SP == c->rt)
      && (MIPS_REG_SP == c->rs) && (0 == (c->imm & 3))) {
    int decoded;

    decoded = c->imm >> 2;
    if ((decoded >= -256) && (decoded <= 257) && (decoded != 0)
        && (decoded != 1) && (decoded != -1) && (decoded != -2)) {
      return 1;
    }
  }
  return 0;
}

/* 16-bit microMIPS for NOP / MOVE / LWSP / SWSP / ADDIUSP. Never
 * shrink a 32-bit branch delay slot (BDS32). */
void mips_opt_micromips_compress(pfile_t *pf)
{
  mips_code_t c;
  mips_code_t pred;
  unsigned    n;

  if (!mips_micromips_enabled(pf)) {
    return;
  }
  n = 0;
  pred = 0;
  for (c = mips_code_list_head(pf); c; pred = c, c = c->link) {
    if (pred && mips_op_fillable(pred->op)) {
      continue;
    }
    if (c->enc_sz != (unsigned short) MIPS_WORD_SZ) {
      continue;
    }
    if (mips_mm_can_compress(c)) {
      c->enc_sz = 2;
      n++;
    }
  }
  if (n) {
    pfile_log(pf, PFILE_LOG_DEBUG,
        "mips microMIPS compress: %u insn(s)", n);
  }
}
