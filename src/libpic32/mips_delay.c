/************************************************************
 **
 ** mips_delay.c : PIC32 (MIPS32) _usec_delay codegen
 **
 ** PIC uses Fosc/4 instruction cycles (`usec * clock / 4000000`).
 ** PIC32 MIPS issues one instruction per SYSCLK cycle, so the
 ** budget is `usec * target_clock / 1000000`.
 **
 ** Loop body (3 cycles per iteration):
 **   addiu $t1, $t1, -1
 **   bne   $t1, $zero, loop
 **   nop                 ; delay slot
 ** Remainder cycles are NOP padding. Tiny delays are NOP-only.
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include <math.h>
#include <stdlib.h>

#include "../libcore/pfile.h"
#include "../libcore/value.h"
#include "../libcore/pf_msg.h"
#include "../libcore/variable.h"
#include "mips.h"
#include "mips_reg.h"
#include "mips_alloc.h"

#define MIPS_DELAY_LOOP_CYCLES 3UL

static void mips_delay_nops(unsigned long n)
{
  while (n) {
    (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
    n--;
  }
}

static void mips_delay_load_iters(unsigned long iters)
{
  if (iters <= 0x7FFFUL) {
    (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1, MIPS_REG_ZERO,
        (int) iters);
  } else {
    int hi;
    int lo;

    hi = (int) ((iters >> 16) & 0xFFFF);
    lo = (int) (iters & 0xFFFF);
    (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T1, 0, hi);
    if (lo) {
      (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T1, MIPS_REG_T1, lo);
    }
  }
}

static unsigned mips_delay_setup_ct(unsigned long iters)
{
  if (iters <= 0x7FFFUL) {
    return 1;
  }
  return (iters & 0xFFFFUL) ? 2 : 1;
}


/* I-cache self-invalidation sweep over the first 4 KB of the KSEG0
 * mirror (where the whole program lives on the boot-flash build).
 * Executed at the head of every _usec_delay: while flash is intact the
 * lines are simply refetched (a few hundred cycles, compensated below);
 * after an ICSP reprogram the refetch reads erased flash (0xFF..) and
 * the Reserved Instruction exception fires, landing on the uncached
 * exception vector in the NEW flash -- which jumps into the new
 * startup.  This is what makes jalv2 images "crash into" the freshly
 * programmed program with no reset, the same accident that made the
 * MikroE app appear to restart instantly. */
#define MIPS_DELAY_SWEEP_LINES 128UL
#define MIPS_DELAY_SWEEP_CYCLES \
  (MIPS_DELAY_SWEEP_LINES * 5UL + 3UL)

static void mips_delay_sweep_emit(void)
{
  mips_code_t loop;
  mips_code_t skip;
  mips_code_t done;

  /* Guard: only sweep when the flash under us holds a VALID image.
   * An UNCACHED KSEG1 read of the boot word (first image word is
   * always the PRECON lui; erased flash reads 0xFFFFFFFF) detects the
   * erase window of an in-progress ICSP session -- sweeping there
   * derails the core into mixed old/new bytes mid-write. */
  (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T0, 0, 0xBFC0);
  (void) mips_code_alloc(MIPS_OP_LW, MIPS_REG_T2, MIPS_REG_T0, 0);
  (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T3, 0, 0xFFFF);
  (void) mips_code_alloc(MIPS_OP_ORI, MIPS_REG_T3, MIPS_REG_T3,
      (int) 0xFFFF);
  skip = mips_code_alloc(MIPS_OP_BEQ, MIPS_REG_T2, MIPS_REG_T3, 0);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);

  (void) mips_code_alloc(MIPS_OP_LUI, MIPS_REG_T0, 0, 0x8000);
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T2, MIPS_REG_ZERO,
      (int) MIPS_DELAY_SWEEP_LINES);
  loop = mips_code_label_alloc();
  (void) mips_code_alloc(MIPS_OP_CACHE, 0x10, MIPS_REG_T0, 0);
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T2, MIPS_REG_T2, -1);
  {
    mips_code_t bne = mips_code_alloc(MIPS_OP_BNE, MIPS_REG_T2,
        MIPS_REG_ZERO, 0);

    if (bne) {
      bne->target = loop;
    }
  }
  (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T0, MIPS_REG_T0, 32);
  (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);

  done = mips_code_label_alloc();
  if (skip) {
    skip->target = done;
  }
}


void mips_delay_create(pfile_t *pf, variable_const_t usec)
{
  value_t       freq;
  double        dcycles;
  unsigned long cycles;
  unsigned long clock;
  unsigned long setup;
  unsigned long remain;
  unsigned long iters;
  unsigned long leftover;
  unsigned      pass;

  if (!usec) {
    return;
  }

  freq = pfile_value_find(pf, PFILE_LOG_ERR, "target_clock");
  if (VALUE_NONE == freq) {
    return;
  }
  clock = (unsigned long) value_const_get(freq);
  value_release(freq);
  if (!clock) {
    pfile_log(pf, PFILE_LOG_ERR, "target_clock is 0");
    return;
  }
  mips_reg_cache_reset();
  mips_alloc_region_reset();

  /* 1 MIPS instruction ≈ 1 SYSCLK (not PIC Fosc/4). */
  dcycles = 1.0 * (double) usec * (double) clock / 1000000.0;
  cycles = (unsigned long) ceil(dcycles);
  if (!cycles) {
    cycles = 1;
  }

  cycles += MIPS_DELAY_SWEEP_CYCLES;

  if (cycles < (MIPS_DELAY_LOOP_CYCLES + 1UL)) {
    mips_delay_nops(cycles);
    return;
  }

  /* Two-pass: guess a 2-instruction setup, then recompute with the
   * setup that the chosen iteration count actually needs. */
  setup = 2;
  iters = 0;
  leftover = 0;
  for (pass = 0; pass < 2; pass++) {
    if (cycles <= setup) {
      mips_delay_nops(cycles);
      return;
    }
    remain = cycles - setup;
    iters = remain / MIPS_DELAY_LOOP_CYCLES;
    leftover = remain % MIPS_DELAY_LOOP_CYCLES;
    if (!iters) {
      mips_delay_nops(cycles);
      return;
    }
    setup = mips_delay_setup_ct(iters);
  }

  {
    char usec_buf[50];

    variable_const_str_get(usec, BOOLEAN_FALSE, usec_buf, sizeof(usec_buf));
    pfile_log(pf, PFILE_LOG_DEBUG,
        "delay %s requires %lu cycles (%lu x %lu + %lu setup + %lu nop)",
        usec_buf, cycles, iters, MIPS_DELAY_LOOP_CYCLES, setup, leftover);
  }

  /* The I-cache sweep is the crash-into-new-image mechanism (see
   * fixes/fix_pic32_crash_into_new_image.md).  JALV2_NO_DELAY_SWEEP=1
   * disables it for diagnosis / images that never live across an ICSP
   * reprogram. */
  if (!getenv("JALV2_NO_DELAY_SWEEP"))
    mips_delay_sweep_emit();
  mips_delay_load_iters(iters);
  {
    mips_code_t head;
    mips_code_t bne;

    head = mips_code_label_alloc();
    (void) mips_code_alloc(MIPS_OP_ADDIU, MIPS_REG_T1, MIPS_REG_T1, -1);
    bne = mips_code_alloc(MIPS_OP_BNE, MIPS_REG_ZERO, MIPS_REG_T1, 0);
    if (bne) {
      bne->target = head;
    }
    (void) mips_code_alloc(MIPS_OP_NOP, 0, 0, 0);
  }
  mips_delay_nops(leftover);
}
