/************************************************************
 **
 ** mips_reg.c : emit-time GPR value cache
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include "mips.h"
#include "mips_reg.h"
#include "mips_alloc.h"
#include "../libcore/pf_proc.h"

#define MIPS_REG_CACHE_CT 12

typedef struct mips_reg_cache_ent_ {
  variable_t v;
  unsigned   reg;
  unsigned   sz;
} mips_reg_cache_ent_t;

static mips_reg_cache_ent_t mips_reg_cache[MIPS_REG_CACHE_CT];
static unsigned             mips_reg_cache_used;

void mips_reg_cache_reset(void)
{
  /* BB-cache only. Alloc homes use mips_alloc_region_reset /
   * mips_alloc_call_reset at real CFG boundaries — not here, because
   * mips_code_label_alloc calls this mid-expression. */
  mips_reg_cache_used = 0;
}

void mips_reg_cache_clobber(unsigned reg)
{
  unsigned i;
  unsigned w;

  w = 0;
  for (i = 0; i < mips_reg_cache_used; i++) {
    if (mips_reg_cache[i].reg != reg) {
      if (w != i) {
        mips_reg_cache[w] = mips_reg_cache[i];
      }
      w++;
    }
  }
  mips_reg_cache_used = w;
}

static variable_t mips_reg_master(variable_t v)
{
  if (!v) {
    return 0;
  }
  while (variable_master_get(v)) {
    v = variable_master_get(v);
  }
  return v;
}

void mips_reg_cache_invalidate_var(variable_t v)
{
  variable_t master;
  unsigned   i;
  unsigned   w;

  master = mips_reg_master(v);
  if (!master) {
    return;
  }
  w = 0;
  for (i = 0; i < mips_reg_cache_used; i++) {
    if (mips_reg_master(mips_reg_cache[i].v) != master) {
      if (w != i) {
        mips_reg_cache[w] = mips_reg_cache[i];
      }
      w++;
    }
  }
  mips_reg_cache_used = w;
  /* Do not mips_alloc_home_invalidate here — bind() calls this and would
   * cold a home we just marked dirty. Alias stores call alloc invalidate
   * explicitly. */
}

boolean_t mips_reg_cacheable(const value_t val)
{
  variable_t v;
  value_t    b;
  unsigned   sz;

  if (!val || value_is_const(val) || value_is_label(val)
      || value_is_function(val) || value_is_value(val)) {
    return BOOLEAN_FALSE;
  }
  if (value_is_volatile(val) || value_is_pointer(val)
      || mips_value_is_ptr_deref(val)) {
    return BOOLEAN_FALSE;
  }
  if (value_is_single_bit(val) || value_is_multi_bit(val)) {
    return BOOLEAN_FALSE;
  }
  if (value_flag_test(val, VALUE_FLAG_PTR_SLOT_RAW)
      || value_flag_test(val, VALUE_FLAG_ARRAY)) {
    return BOOLEAN_FALSE;
  }
  v = value_variable_get(val);
  if (!v || variable_is_const(v)) {
    return BOOLEAN_FALSE;
  }
  b = value_baseofs_get(val);
  if (b) {
    variable_const_t off;

    if (!value_is_const(b)) {
      return BOOLEAN_FALSE;
    }
    off = value_const_get(b);
    if (off) {
      return BOOLEAN_FALSE;
    }
  }
  sz = value_byte_sz_get(val);
  if ((1 != sz) && (2 != sz) && (4 != sz)) {
    return BOOLEAN_FALSE;
  }
  return BOOLEAN_TRUE;
}

unsigned mips_reg_cache_lookup(const value_t val)
{
  variable_t v;
  unsigned   sz;
  unsigned   i;

  if (!mips_reg_cacheable(val)) {
    return (unsigned) -1;
  }
  v  = value_variable_get(val);
  sz = value_byte_sz_get(val);
  for (i = 0; i < mips_reg_cache_used; i++) {
    if ((mips_reg_cache[i].v == v) && (mips_reg_cache[i].sz == sz)) {
      return mips_reg_cache[i].reg;
    }
  }
  return (unsigned) -1;
}

void mips_reg_cache_bind(const value_t val, unsigned reg)
{
  variable_t v;
  unsigned   sz;
  unsigned   i;

  if (!mips_reg_cacheable(val) || (reg == MIPS_REG_ZERO)
      || (reg == MIPS_REG_SP) || (reg == MIPS_REG_GP)
      || (reg == MIPS_REG_RA)) {
    return;
  }
  v  = value_variable_get(val);
  sz = value_byte_sz_get(val);
  /* One binding per variable; one primary per register. */
  mips_reg_cache_invalidate_var(v);
  mips_reg_cache_clobber(reg);
  if (mips_reg_cache_used >= MIPS_REG_CACHE_CT) {
    /* Drop oldest. */
    for (i = 1; i < MIPS_REG_CACHE_CT; i++) {
      mips_reg_cache[i - 1] = mips_reg_cache[i];
    }
    mips_reg_cache_used = MIPS_REG_CACHE_CT - 1;
  }
  mips_reg_cache[mips_reg_cache_used].v   = v;
  mips_reg_cache[mips_reg_cache_used].reg = reg;
  mips_reg_cache[mips_reg_cache_used].sz  = sz;
  mips_reg_cache_used++;
}

/* $s3-$s7 — $s0-$s2 belong to soft-float helpers. */
static int mips_reg_is_s_home(unsigned reg)
{
  return (reg >= MIPS_REG_S3) && (reg <= MIPS_REG_S7);
}

void mips_reg_cache_preserve_across_call(pfile_proc_t *cur)
{
  unsigned i;
  unsigned w;

  /* Phase C1: allocator owns $s3-$s7 homes across calls. Keep any BB
   * bindings already in those regs; drop caller-saved cache entries
   * (no opportunistic OR-promote — that raced with alloc homes). */
  UNUSED(cur);
  w = 0;
  for (i = 0; i < mips_reg_cache_used; i++) {
    if (mips_reg_is_s_home(mips_reg_cache[i].reg)) {
      if (w != i) {
        mips_reg_cache[w] = mips_reg_cache[i];
      }
      w++;
    }
  }
  mips_reg_cache_used = w;
}
