/************************************************************
 **
 ** pic_op_fsr.c : PIC operator generation -- FSR setup and dynamic indexing
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 **           (c) 2020-2025, Rob Jansen
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: split out of pic_op.c (Tier-A decomposition, 2026-10-31);
 **          code moved verbatim, forward declarations replaced by
 **          pic_op_priv.h prototypes.
 **
 ************************************************************/

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "../libutils/mem.h"

#include "../libcore/pf_proc.h"
#include "../libcore/pf_expr.h"
#include "../libcore/pf_op.h"
#include "pic_opfn.h"
#include "pic_stvar.h"
#include "pic_var.h"
#include "piccolst.h"
#include "pic_inst.h"
#include "pic_op.h"
#include "pic_op_priv.h"

/* RUNTIME CHUNK WALK (2026-11-02; per-formal map 2026-11-03; no chunk cap).
 *
 * An open-array formal serving several different layouts cannot carry a baked
 * chunk map, so it owns a small map buffer that every call site fills with the
 * ACTIVE layout just before the call (pic_chunk_desc_store):
 *
 *     [count][base0 lo, base0 hi, span0][base1 lo, base1 hi, span1]...
 *
 * This walk loops over that table until the index falls inside a chunk, so the
 * number of chunks is limited only by the map's size:
 *
 *     FSR0 = map + 1; count = map[0]
 *   loop: base = INDF++; hi = INDF++; span = INDF++
 *         if (t < span) goto done          ; t = index inside this chunk
 *         t -= span; if (--count) loop
 *   done: FSR0 = base + t                  ; leaves FSR0 at the element
 *
 * The map belongs to the FORMAL, so a nested call to another procedure fills
 * its own formal's map and cannot disturb this one.
 */
static void pic_fsr_chunk_walk_emit_globals(pfile_t *pf, variable_t formal,
    value_t idx, size_t offset)
{
  variable_t map;
  unsigned t_wide;
  value_t t;
  value_t cnt;
  value_t base;
  value_t span;
  value_t ind;
  label_t loop;
  label_t done;

  long idx_const;

  map = pic_var_openarray_formal_map_get(formal);
  if ((VARIABLE_NONE == map)
      || (VARIABLE_BASE_UNKNOWN == variable_base_get(map, 0))) {
    return;
  }
  ind = pic_indirect_get(pf, PFILE_LOG_ERR, 0);
  {
    variable_t gc;
    variable_t gb;
    variable_t gs;

    if (!pic_var_chunk_walk_globals_get(pf, &gc, &gb, &gs)) {
      return;
    }
    /* the baked walk's width rule: a byte index gets the ONE-byte standard
     * temp (v__pic_temp), which the caller's out-parameter staging reads
     * back as the in-chunk offset -- asking for 2 bytes returned
     * _pic_temp+1 instead and the staged value went missing (the
     * echo3-prints-nothing hang) */
    if (!idx || value_is_const(idx)) {
      unsigned long tot;

      tot = offset + (idx ? (unsigned long) value_const_get(idx) : 0UL);
      t_wide = (tot > 0xFFU) ? 1 : 0;
      idx_const = (long) tot;
    } else {
      t_wide = ((pic_dynamic_index_byte_sz_get(idx) > 1) || (offset > 0xFFU))
        ? 1 : 0;
      idx_const = -1;
    }
    t    = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, t_wide ? 2 : 1);
    cnt  = value_alloc(gc);
    base = value_alloc(gb);
    span = value_alloc(gs);
    if (!ind || !t || !cnt || !base || !span) {
      if (t)    { pic_var_temp_release(pf, t); }
      if (cnt)  { value_release(cnt); }
      if (base) { value_release(base); }
      if (span) { value_release(span); }
      return;
    }
  }
  loop = pfile_label_alloc(pf, 0);
  done = pfile_label_alloc(pf, 0);

  /* t = idx + const offset */
  if (idx_const >= 0) {
    unsigned long tot;

    tot = (unsigned long) idx_const;
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, (unsigned) (tot & 0xFF));
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, t, 0);
    if (t_wide) {
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
          (unsigned) ((tot >> 8) & 0xFF));
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, t, 1);
    }
  } else {
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, idx, 0, PIC_OPDST_W);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, t, 0);
    if (t_wide) {
      pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, idx, 1, PIC_OPDST_W);
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, t, 1);
    }
    if (offset) {
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, (unsigned) (offset & 0xFF));
      pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, t, 0, PIC_OPDST_F);
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
      pic_instr_append_f_d(pf, PIC_OPCODE_INCF, t, 1, PIC_OPDST_F);
    }
  }
  /* FSR0 = map; count = *FSR0++; table walk starts at +1 */
  pic_fsr0_load_addr(pf, variable_base_get(map, 0));
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ind, 0, PIC_OPDST_W);
  pic_instr_append_f(pf, PIC_OPCODE_MOVWF, cnt, 0);
  pic_instr_append_reg_d(pf, PIC_OPCODE_INCF, "_fsr0l", PIC_OPDST_F);
  /* INCF does not touch C -- it sets Z on wrap, which is the carry out of
   * the low byte.  Testing C here used whatever the previous instruction
   * left there, so the table pointer jumped a whole 0x100 page whenever C
   * happened to be set (found 2026-11-02: `_strarg_map_ptr` 0x0256 walked
   * as 0x0356, base0 read as garbage, FSR0 ended at an invalid file
   * register). */
  pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
  pic_instr_append_reg_d(pf, PIC_OPCODE_INCF, "_fsr0h", PIC_OPDST_F);

  pic_instr_append_label(pf, loop);
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ind, 0, PIC_OPDST_W);
  pic_instr_append_f(pf, PIC_OPCODE_MOVWF, base, 0);
  pic_instr_append_reg_d(pf, PIC_OPCODE_INCF, "_fsr0l", PIC_OPDST_F);
  /* INCF does not touch C -- it sets Z on wrap, which is the carry out of
   * the low byte.  Testing C here used whatever the previous instruction
   * left there, so the table pointer jumped a whole 0x100 page whenever C
   * happened to be set (found 2026-11-02: `_strarg_map_ptr` 0x0256 walked
   * as 0x0356, base0 read as garbage, FSR0 ended at an invalid file
   * register). */
  pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
  pic_instr_append_reg_d(pf, PIC_OPCODE_INCF, "_fsr0h", PIC_OPDST_F);
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ind, 0, PIC_OPDST_W);
  pic_instr_append_f(pf, PIC_OPCODE_MOVWF, base, 1);
  pic_instr_append_reg_d(pf, PIC_OPCODE_INCF, "_fsr0l", PIC_OPDST_F);
  /* INCF does not touch C -- it sets Z on wrap, which is the carry out of
   * the low byte.  Testing C here used whatever the previous instruction
   * left there, so the table pointer jumped a whole 0x100 page whenever C
   * happened to be set (found 2026-11-02: `_strarg_map_ptr` 0x0256 walked
   * as 0x0356, base0 read as garbage, FSR0 ended at an invalid file
   * register). */
  pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
  pic_instr_append_reg_d(pf, PIC_OPCODE_INCF, "_fsr0h", PIC_OPDST_F);
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ind, 0, PIC_OPDST_W);
  pic_instr_append_f(pf, PIC_OPCODE_MOVWF, span, 0);
  pic_instr_append_reg_d(pf, PIC_OPCODE_INCF, "_fsr0l", PIC_OPDST_F);
  /* INCF does not touch C -- it sets Z on wrap, which is the carry out of
   * the low byte.  Testing C here used whatever the previous instruction
   * left there, so the table pointer jumped a whole 0x100 page whenever C
   * happened to be set (found 2026-11-02: `_strarg_map_ptr` 0x0256 walked
   * as 0x0356, base0 read as garbage, FSR0 ended at an invalid file
   * register). */
  pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_z");
  pic_instr_append_reg_d(pf, PIC_OPCODE_INCF, "_fsr0h", PIC_OPDST_F);
  /* t < span ? */
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, span, 0, PIC_OPDST_W);
  pic_instr_append_f_d(pf, PIC_OPCODE_SUBWF, t, 0, PIC_OPDST_W);
  pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_c");
  pic_instr_append_n(pf, PIC_OPCODE_GOTO, done);
  /* next chunk: t -= span; if (--count) loop */
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, span, 0, PIC_OPDST_W);
  pic_instr_append_f_d(pf, PIC_OPCODE_SUBWF, t, 0, PIC_OPDST_F);
  pic_instr_append_f_d(pf, PIC_OPCODE_DECFSZ, cnt, 0, PIC_OPDST_F);
  pic_instr_append_n(pf, PIC_OPCODE_GOTO, loop);

  pic_instr_append_label(pf, done);
  /* FSR0 = base + t */
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, base, 0, PIC_OPDST_W);
  pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0l");
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, base, 1, PIC_OPDST_W);
  pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0h");
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, t, 0, PIC_OPDST_W);
  pic_instr_append_reg_d(pf, PIC_OPCODE_ADDWF, "_fsr0l", PIC_OPDST_F);
  pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
  pic_instr_append_reg_d(pf, PIC_OPCODE_INCF, "_fsr0h", PIC_OPDST_F);

  label_release(done);
  label_release(loop);
  value_release(span);
  value_release(base);
  value_release(cnt);
  pic_var_temp_release(pf, t);
}


/* runtime-map walk entry for callers outside this TU (the pointer path in
 * pic_op_indirect.c, the ROM/data dispatch in pic_op_ptr.c); `formal` is the
 * open-array formal whose map is walked and `idx` may be VALUE_NONE/const for
 * a constant subscript */
void pic_fsr_chunk_walk_emit_runtime(pfile_t *pf, variable_t formal,
    value_t idx, size_t offset)
{
  pic_fsr_chunk_walk_emit_globals(pf, formal, idx, offset);
}

/* defined in this TU (prototypes keep definition order free) */
static void pic_fsr_chunk_walk_emit(pfile_t *pf, variable_t var,
    value_t idx, boolean_t hybrid);
static boolean_t pic_lookup_index_ok_for_movf(value_t baseofs);

value_t pic_fsr_get(pfile_t *pf)
{
  pic_stvar_fsr_mark(pf);
  return pfile_value_find(pf, PFILE_LOG_ERR, 
    (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf))
    ? "_fsr0l"
    : "_fsr");
}

void pic_fsr_setup(pfile_t *pf, value_t val)
{
  pic_stvar_fsr_mark(pf);
  if (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf)) {
    /* Check if this is a function parameter that should use direct addressing */
    variable_t var = value_variable_get(val);
    if (var) {
      const char *name = variable_name_get(var);
      /* Skip FSR0 for function parameters, preserve for actual pointer operations */
      if (name && (strstr(name, "_device_put") ||
                   strstr(name, "_str_") ||
                   strstr(name, "___str"))) {
        /* Skip FSR0 for specific string-parameter hacks; do not use strstr(..., "str")
         * — it matches identifiers like string1/string2 and breaks FSR0 for indexing. */
        return;
      }
    }
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, val, 1, PIC_OPDST_W);
    pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0h");
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, val, 0, PIC_OPDST_W);
    pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0l");
  } else {
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, val, 0, PIC_OPDST_W);
    pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr");
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_irp");
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, val, 1, PIC_OPDST_F);
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status", "_irp");
  }
}

variable_sz_t pic_dynamic_index_byte_sz_get(value_t idx)
{
  variable_sz_t idx_sz;

  idx_sz = value_byte_sz_get(idx);
  return (idx_sz < 1) ? 1 : idx_sz;
}

void pic_fsr0_add_dynamic_index(pfile_t *pf, value_t idx,
    value_t array_val)
{
  variable_sz_t idx_sz;
  variable_t    walk_var;

  walk_var = value_variable_get(array_val);
  if (pic_var_openarray_pair_formal_runtime_get(walk_var)) {
    pic_fsr_chunk_walk_emit_globals(pf, walk_var, idx, 0);
    return;
  }
  if (!pic_variable_chunks_holed(walk_var)) {
    /* unescaped pointer bind: walk the TARGET's chunks */
    walk_var = pic_var_pointer_bind_walk_target(walk_var);
  }
  if (pic_variable_chunks_holed(walk_var)) {
    pic_fsr_chunk_walk_emit(pf, walk_var, idx, BOOLEAN_TRUE);
    return;
  }
  idx_sz = pic_dynamic_index_byte_sz_get(idx);
  /* the subscript byte offset cannot exceed the array's span, so an
   * array of at most 256 bytes has a single-byte index however wide
   * the index expression's type is (e.g. word `address / 2` into a
   * byte[256] sector buffer). Narrowing drops the high-byte
   * addwfc/addwf fsr0h pair per access site; the carry fixup below
   * keeps an unaligned base correct. */
  {
    variable_t array_var;

    array_var = value_variable_get(array_val);
    if (array_var) {
      variable_sz_t span;

      /* the SPAN must come from the array variable's def, not the
       * subscripted value's def: `arr[i]` carries the ELEMENT view
       * (storage need 1), which made this guard see span==1 for every
       * array and take the carry-free fast path whenever the BASE fit
       * a 256 window -- base+index then wrapped into low RAM (the
       * PIC18 FSR-carry defect, docs/investigations/
       * pic18_runtime_index_fsr_carry_20261005.md) */
      span = pic_variable_def_storage_need_get(pf,
          variable_def_get(array_var), 0);
      if (span && (span <= 256)) {
        idx_sz = 1;
        /* when the allocated base is known and the whole array fits
         * below the next 256 boundary, base_lo + idx cannot carry
         * into FSR0H and the fixup is dead (checked at emission:
         * bases are known from generate pass 2 on) */
        {
          variable_base_t base;

          base = value_base_get(array_val);
          if ((VARIABLE_BASE_UNKNOWN != base)
              && (((base & 0xFFU) + span) <= 256U)) {
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, idx, 0, PIC_OPDST_W);
            pic_instr_append_reg_d(pf, PIC_OPCODE_ADDWF, "_fsr0l",
                PIC_OPDST_F);
            return;
          }
        }
      }
    }
  }
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, idx, 0, PIC_OPDST_W);
  pic_instr_append_reg_d(pf, PIC_OPCODE_ADDWF, "_fsr0l", PIC_OPDST_F);
  if (idx_sz > 1) {
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, 0);
    pic_instr_append_f_d(pf, PIC_OPCODE_ADDWFc, idx, 1, PIC_OPDST_W);
    pic_instr_append_reg_d(pf, PIC_OPCODE_ADDWF, "_fsr0h", PIC_OPDST_F);
  } else {
    pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
    pic_instr_append_reg_d(pf, PIC_OPCODE_INCF, "_fsr0h", PIC_OPDST_F);
  }
}

void pic_classic_irp_set_from_w(pfile_t *pf)
{
  value_t hi;

  hi = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, 1);
  if (!hi) {
    pfile_log_syserr(pf, ENOMEM);
    return;
  }
  pic_instr_append_f(pf, PIC_OPCODE_MOVWF, hi, 0);
  pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_irp");
  pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, hi, 0, 0);
  pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status", "_irp");
  pic_var_temp_release(pf, hi);
}

/* the per-chunk ladder itself: t holds the byte offset (the walk CONSUMES
 * it: compare, then subtract each chunk's size) and the routine leaves the
 * element address in FSR0 (14H / 16-bit) or in _fsr with IRP set (classic).
 * Emitted inline by the offset/rare paths and ONCE per chunked variable by
 * pic_chunk_walk_helpers_create (the shared form -- see its comment). */
static void pic_fsr_chunk_ladder(pfile_t *pf, variable_t var, value_t t,
    boolean_t t_wide, boolean_t hybrid)
{
  variable_ct_t  ct;
  variable_ct_t  ii;
  label_t        done;

  ct = pic_variable_chunk_ct_get(var);
  done = pfile_label_alloc(pf, 0);
  for (ii = 0; ii < ct; ii++) {
    variable_sz_t    sz;
    variable_base_t  cb;
    label_t          next;
    value_t          cval;

    sz = pic_variable_chunk_sz_get(var, ii);
    cb = pic_variable_chunk_base_get(var, ii);
    next = LABEL_NONE;
    if (ii + 1 < ct) {
      next = pfile_label_alloc(pf, 0);
      if (t_wide) {
        /* offset >= 256 cannot be in this (< 256 byte) chunk */
        pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, t, 1, PIC_OPDST_W);
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
        pic_instr_append_n(pf, PIC_OPCODE_GOTO, next);
      }
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, sz);
      pic_instr_append_f_d(pf, PIC_OPCODE_SUBWF, t, 0, PIC_OPDST_W);
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, next);
    }
    if (hybrid) {
      /* seventh copy of the absolute load -- the chunk walk's base */
      pic_fsr0_load_addr(pf, cb);
      pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, t, 0, PIC_OPDST_W);
      pic_instr_append_reg_d(pf, PIC_OPCODE_ADDWF, "_fsr0l", PIC_OPDST_F);
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
      pic_instr_append_reg_d(pf, PIC_OPCODE_INCF, "_fsr0h", PIC_OPDST_F);
      if (t_wide) {
        /* a single-chunk map never runs the subtract ladder, so t can
         * still hold the FULL (>= 256) linear index -- its high byte
         * belongs in fsr0h (ladder-reduced chunks carry t+1 == 0 and
         * the add is a no-op) */
        pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, t, 1, PIC_OPDST_W);
        pic_instr_append_reg_d(pf, PIC_OPCODE_ADDWF, "_fsr0h",
            PIC_OPDST_F);
      }
    } else {
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, cb & 0xff);
      pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, t, 0, PIC_OPDST_W);
      pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr");
      /* IRP = (chunk base >= 0x100) OR carry of the low-byte add: a
       * chunk in banks 2/3 needs IRP set even when the low-byte sum
       * does not carry (base 0x110 + offset 0x19 computed 0x23 with
       * IRP left clear and landed in bank 0 -- found by the 16f877
       * >256-byte runtime witness, 2026-10-26) */
      if (cb & 0x100) {
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status", "_irp");
      } else {
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_irp");
      }
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status", "_irp");
      if (t_wide) {
        /* full linear index still in t (single-chunk map): IRP takes
         * t's high byte too */
        pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, t, 1, PIC_OPDST_W);
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
        pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status", "_irp");
      }
    }
    if (LABEL_NONE != next) {
      pic_instr_append_n(pf, PIC_OPCODE_GOTO, done);
      pic_instr_append_label(pf, next);
      cval = pfile_constant_get(pf, sz, VARIABLE_DEF_NONE);
      if (cval) {
        pic_op(pf, OPERATOR_SUB, t, t, cval);
        value_release(cval);
      } else {
        pfile_log_syserr(pf, ENOMEM);
      }
      label_release(next);
    }
  }
  pic_instr_append_label(pf, done);
  label_release(done);
}

/* ---- SHARED STATIC CHUNK WALK (2026-11-02) ----------------------------
 *
 * The chunk map of a holed (multi-bank) array is a compile-time constant, so
 * the ladder does not belong at every access site.  It used to be inlined
 * there: ~40 instructions for a two-chunk map, per site, which is what made
 * a `_string_heap_size` over one RAM bank explode the code -- Rob's 16F1825
 * program with one `&` chain reached 8334 words against an 8192 limit, and a
 * minimal concat probe went 3424 -> 7343 words for an 80 -> 100 byte heap.
 *
 * One helper per chunked VARIABLE is emitted at the end of codegen (the
 * intrinsic hook, see pic_chunk_walk_helpers_create); a site becomes
 *
 *     MOVF idx,W ; MOVWF _pic_chunk_t [, high byte] ; CALL l__chunkwalk_x
 *
 * The index travels in the compiler-owned `_pic_chunk_t` (2 bytes) because
 * the caller's temp area can already hold a staged value (`device = str[i]`
 * stages the RHS in _pic_temp -- the older temp-based walk clobbered it).
 */
typedef struct pic_chunk_walk_helper_ {
  variable_t var;
  label_t    lbl;
} pic_chunk_walk_helper_t;

static pic_chunk_walk_helper_t *pic_cw_help;
static size_t                   pic_cw_help_used;
static size_t                   pic_cw_help_alloc;

void pic_chunk_walk_state_reset(void)
{
  size_t ii;


  for (ii = 0; ii < pic_cw_help_used; ii++) {
    label_release(pic_cw_help[ii].lbl);
  }
  if (pic_cw_help) {
    FREE(pic_cw_help);
  }
  pic_cw_help       = 0;
  pic_cw_help_used  = 0;
  pic_cw_help_alloc = 0;
}

/* find (or register) the helper label of one chunked variable */
static label_t pic_chunk_walk_helper_find(pfile_t *pf, variable_t var)
{
  const char *vnm;
  label_t     lbl;
  char        name[64];
  size_t      ii;

  for (ii = 0; ii < pic_cw_help_used; ii++) {
    if (pic_cw_help[ii].var == var) {
      return pic_cw_help[ii].lbl;
    }
  }
  vnm = variable_name_get(var);
  if (!vnm || (strlen(vnm) > 40)) {
    return LABEL_NONE;
  }
  sprintf(name, "_pic_chunkwalk_%s_%u", vnm,
      (unsigned) variable_tag_n_get(var));
  lbl = pic_label_find(pf, name, BOOLEAN_TRUE);
  if (!lbl) {
    return LABEL_NONE;
  }
  if (pic_cw_help_used == pic_cw_help_alloc) {
    size_t                  nsz;
    pic_chunk_walk_helper_t *tmp;

    nsz = pic_cw_help_alloc ? 2 * pic_cw_help_alloc : 8;
    tmp = REALLOC(pic_cw_help, nsz * sizeof(*pic_cw_help));
    if (!tmp) {
      label_release(lbl);
      return LABEL_NONE;
    }
    pic_cw_help       = tmp;
    pic_cw_help_alloc = nsz;
  }
  pic_cw_help[pic_cw_help_used].var = var;
  pic_cw_help[pic_cw_help_used].lbl = lbl;
  pic_cw_help_used++;
  return lbl;
}

/* emit the CALL form; returns BOOLEAN_FALSE when the program has no walk
 * scratch (then the caller falls back to the inline ladder) */
static boolean_t pic_fsr_chunk_walk_emit_call(pfile_t *pf, variable_t var,
    value_t idx)
{
  variable_t    arg;
  value_t       argval;
  label_t       lbl;
  variable_sz_t idx_sz;

  arg = pic_var_chunk_walk_arg_get();
  if (!arg || (VARIABLE_NONE == arg)) {
    return BOOLEAN_FALSE;
  }
  lbl = pic_chunk_walk_helper_find(pf, var);
  if (LABEL_NONE == lbl) {
    return BOOLEAN_FALSE;
  }
  argval = value_alloc(arg);
  if (!argval) {
    return BOOLEAN_FALSE;
  }
  idx_sz = pic_dynamic_index_byte_sz_get(idx);
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, idx, 0, PIC_OPDST_W);
  pic_instr_append_f(pf, PIC_OPCODE_MOVWF, argval, 0);
  if (idx_sz > 1) {
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, idx, 1, PIC_OPDST_W);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, argval, 1);
  } else {
    pic_instr_append_f(pf, PIC_OPCODE_CLRF, argval, 1);
  }
  pic_instr_append_n(pf, PIC_OPCODE_CALL, lbl);
  value_release(argval);
  return BOOLEAN_TRUE;
}

/* Register a helper for every holed (multi-bank) variable in the program.
 *
 * This runs at the START of the preamble, before the reset-vector decision,
 * because (a) the helper bodies are emitted at the top of the image like the
 * intrinsic bodies -- so the entry must be a page-jump when any exists -- and
 * (b) a body can only be emitted for a helper that is registered, while the
 * call sites are emitted LATER (the procedures come after the preamble).
 * Registering from the variable list (not from the call sites) is what makes
 * the body exist for the final pass: a call site discovered in pass 3 after
 * the preamble would otherwise emit a CALL to a label with no body. */
void pic_chunk_walk_helpers_register(pfile_t *pf)
{
  pfile_proc_t *proc;

  for (proc = pfile_proc_root_get(pf); proc; proc = pfile_proc_next(proc)) {
    pfile_block_t *blk;

    for (blk = pfile_proc_block_root_get(proc); blk;
         blk = pfile_block_next(blk)) {
      variable_t var;

      for (var = pfile_block_variable_list_head(blk); var;
           var = variable_link_get(var)) {
        if (pic_variable_chunks_holed(var)) {
          (void) pic_chunk_walk_helper_find(pf, var);
        }
      }
    }
  }
}

/* does any shared chunk-walk helper exist?  A helper body is emitted at the
 * top of the image like the intrinsic bodies, so the reset vector needs the
 * page-jump form whenever one exists (otherwise address 0 becomes a helper
 * and the program never reaches its entry -- found 2026-11-02) */
boolean_t pic_chunk_walk_helpers_exist(void)
{
  return (pic_cw_help_used != 0) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* emit the helpers that were referenced; called once, at the same point as
 * the intrinsic bodies (after every call site has been emitted) */
void pic_chunk_walk_helpers_create(pfile_t *pf)
{
  variable_t arg;
  boolean_t  hybrid;
  size_t     ii;

  arg = pic_var_chunk_walk_arg_get();
  if (!arg || (VARIABLE_NONE == arg)) {
    return;
  }
  hybrid = (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf))
    ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  for (ii = 0; ii < pic_cw_help_used; ii++) {
    value_t tv;

    tv = value_alloc(arg);
    if (!tv) {
      return;
    }
    pic_instr_append_label(pf, pic_cw_help[ii].lbl);
    /* the argument is always a 2-byte index here: the caller zeroes the
     * high byte for a byte-sized index */
    pic_fsr_chunk_ladder(pf, pic_cw_help[ii].var, tv, BOOLEAN_TRUE, hybrid);
    pic_instr_append(pf, PIC_OPCODE_RETURN);
    value_release(tv);
  }
}

/* Layer 2: a variable placed across bank holes (non-adjacent chunks).
 * Contiguous multi-node runs (PIC18 bank splits) are still linear. */
boolean_t pic_variable_chunks_holed(variable_t var)
{
  variable_ct_t ct;
  variable_ct_t ii;

  ct = var ? pic_variable_chunk_ct_get(var) : 0;
  if (ct <= 1) {
    return BOOLEAN_FALSE;
  }
  for (ii = 1; ii < ct; ii++) {
    if (pic_variable_chunk_base_get(var, ii)
        != (variable_base_t) (pic_variable_chunk_base_get(var, ii - 1)
          + pic_variable_chunk_sz_get(var, ii - 1))) {
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

/* Layer 2: holed linear arrays -- walk the chunk map at run time. The
 * byte offset is copied to a scratch (the walk consumes it: compare,
 * then subtract the chunk size), and each chunk branch computes the
 * element address completely before jumping to done. Chunk sizes are
 * bank-node bounded (< 256), so a byte compare with a high-byte
 * zero-test decides membership; the subtract rides the generic
 * OPERATOR_SUB emitter (carry-correct for wide offsets). Modeled on
 * the dynamic-bit chunk walk (pic_dynamic_bit_ref_addr_setup). */
static void pic_fsr_chunk_walk_emit(pfile_t *pf, variable_t var,
    value_t idx, boolean_t hybrid)
{
  pic_fsr_chunk_walk_emit_ofs(pf, var, idx, 0, hybrid);
}

/* offset-aware walk: the scratch t is initialized to idx + offset (the
 * const part folded out of a pointer-view subscript, e.g. p[256 + i]).
 * W-staged values around the caller are NOT touched -- the offset add
 * rides the walk's own scratch (found the hard way: emitting pic_op
 * arithmetic mid-addressing destroyed the staged RHS of the store). */
void pic_fsr_chunk_walk_emit_ofs(pfile_t *pf, variable_t var,
    value_t idx, size_t offset, boolean_t hybrid)
{
  variable_sz_t  idx_sz;
  unsigned       t_wide;
  value_t        t;

  if (pic_var_openarray_pair_formal_runtime_get(var)) {
    pic_fsr_chunk_walk_emit_globals(pf, var, idx, offset);
    return;
  }
  if ((!idx || value_is_const(idx))
      && (pic_variable_chunk_ct_get(var) > 1)) {
    /* CONSTANT subscript through a compile-time chunk map (e.g.
     * `data[149]` inside a procedure fed one multi-bank buffer): there is
     * no index register to copy -- fold idx and the offset into the
     * scratch and run the ladder.  Without this the caller's linear
     * `deref + offset` sequence addressed across the holes (element 149 of
     * a 150-byte array split 73/77 landed 45 bytes short).  A variable with
     * no chunk map (ct <= 1) is addressed linearly by the caller instead. */
    unsigned long tot;

    tot    = offset
      + (idx ? (unsigned long) value_const_get(idx) : 0UL);
    t_wide = (tot > 0xFFU) ? 1 : 0;
    t      = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE, t_wide ? 2 : 1);
    if (!t) {
      pfile_log_syserr(pf, ENOMEM);
      return;
    }
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, (unsigned) (tot & 0xFFU));
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, t, 0);
    if (t_wide) {
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
          (unsigned) ((tot >> 8) & 0xFFU));
      pic_instr_append_f(pf, PIC_OPCODE_MOVWF, t, 1);
    }
    pic_fsr_chunk_ladder(pf, var, t, t_wide ? BOOLEAN_TRUE : BOOLEAN_FALSE,
        hybrid);
    pic_var_temp_release(pf, t);
    return;
  }
  if ((0 == offset) && pic_fsr_chunk_walk_emit_call(pf, var, idx)) {
    return;
  }
  idx_sz = pic_dynamic_index_byte_sz_get(idx);
  t_wide = ((idx_sz > 1) || (offset > 0xFFU)) ? 1 : 0;
  t = pic_var_temp_get(pf, VARIABLE_DEF_FLAG_NONE,
      t_wide ? 2 : 1);
  if (!t) {
    pfile_log_syserr(pf, ENOMEM);
    return;
  }
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, idx, 0, PIC_OPDST_W);
  pic_instr_append_f(pf, PIC_OPCODE_MOVWF, t, 0);
  if (idx_sz > 1) {
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, idx, 1, PIC_OPDST_W);
    pic_instr_append_f(pf, PIC_OPCODE_MOVWF, t, 1);
  } else if (offset > 0xFFU) {
    pic_instr_append_f(pf, PIC_OPCODE_CLRF, t, 1);
  }
  if (offset) {
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
        (unsigned) (offset & 0xFFU));
    pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, t, 0, PIC_OPDST_F);
    if ((idx_sz > 1) || (offset > 0xFFU)) {
      pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
      pic_instr_append_f_d(pf, PIC_OPCODE_INCF, t, 1, PIC_OPDST_F);
      if (offset > 0xFFU) {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
            (unsigned) ((offset >> 8) & 0xFFU));
        pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, t, 1, PIC_OPDST_F);
      }
    }
  }

  pic_fsr_chunk_ladder(pf, var, t, t_wide ? BOOLEAN_TRUE : BOOLEAN_FALSE,
      hybrid);
  pic_var_temp_release(pf, t);
}

/* idx_range: statically-known byte span the index may legally cover
 * (array byte size minus element offset); 0 = unknown. var: the array
 * variable (VARIABLE_NONE for pointer-slot storage that has no chunk
 * map of its own). */
void pic_classic_fsr_load_base_dynamic_index(pfile_t *pf,
    variable_t var, variable_base_t base, value_t idx,
    variable_sz_t idx_range)
{
  variable_sz_t idx_sz;
  boolean_t     wide16;

  if (!pic_variable_chunks_holed(var)) {
    var = pic_var_pointer_bind_walk_target(var);
  }
  if (pic_variable_chunks_holed(var)) {
    pic_fsr_chunk_walk_emit(pf, var, idx, BOOLEAN_FALSE);
    return;
  }
  idx_sz = pic_dynamic_index_byte_sz_get(idx);
  wide16 = pic_is_16bit(pf) || pic_is_14bit_hybrid(pf);
  /* const index: fold into the base and use a literal high half (ADDWF
   * with a const value would read the const's value as a REGISTER
   * address -- offset 2 reads PCL) */
  if (value_is_const(idx)) {
    variable_base_t abs_addr;

    abs_addr = base + (variable_base_t) value_const_get(idx);
    if (wide16) {
      /* the sixth hand-rolled copy of this sequence -- it was hidden from
       * the greps by the wide16 ? "_fsr0l" : "_fsr" register choice */
      pic_fsr0_load_addr(pf, abs_addr);
    } else {
      pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
          (unsigned) (abs_addr & 0xff));
      pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr");
      pic_instr_append(pf, (abs_addr >= 256)
          ? PIC_OPCODE_IRP_SET : PIC_OPCODE_IRP_CLR);
    }
    return;
  }
  /* Static-window fast path: a byte-sized index whose legal range keeps
   * base+index inside the first 256-byte FSR window needs no 16-bit
   * carry chain and a statically clear IRP.  Out-of-range subscripts
   * are undefined (no bounds checking), so the range precondition is
   * the language contract; jalv25r9 emitted this exact shape. */
  if ((1 == idx_sz) && (idx_range > 0)
      && ((unsigned long) base + (unsigned long) idx_range) <= 0x100UL) {
    pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base & 0xff);
    pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, idx, 0, PIC_OPDST_W);
    pic_instr_append_reg(pf, PIC_OPCODE_MOVWF,
        wide16 ? "_fsr0l" : "_fsr");
    if (wide16) {
      pic_instr_append_reg(pf, PIC_OPCODE_CLRF, "_fsr0h");
    } else {
      pic_instr_append(pf, PIC_OPCODE_IRP_CLR);
    }
    return;
  }
  pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base & 0xff);
  pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, idx, 0, PIC_OPDST_W);
  pic_instr_append_reg(pf, PIC_OPCODE_MOVWF,
      wide16 ? "_fsr0l" : "_fsr");
  pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, (base >> 8) & 0xff);
  pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
  pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, 1);
  if (idx_sz > 1) {
    pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, idx, 1, PIC_OPDST_W);
  }
  if (wide16) {
    /* W holds the computed high byte; the 16-bit FSR needs it stored,
     * not an IRP latch (there is none on these cores) */
    pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0h");
  } else {
    pic_classic_irp_set_from_w(pf);
  }
}

/* PIC14 lookup CALL needs the table index in W. pic_instr_append_f_d(MOVF)
 * is only correct for a plain file register; indexed RAM (INDF / v__ind)
 * needs FSR setup from pic_op_assign (nested index into var array as lookup
 * subscript — GitHub jalv2compiler #40). */
static boolean_t pic_lookup_index_ok_for_movf(value_t baseofs)
{
  variable_t v;

  if (!baseofs) {
    return BOOLEAN_FALSE;
  }
  if (value_is_bit(baseofs) || value_is_indirect(baseofs)) {
    return BOOLEAN_FALSE;
  }
  if (value_flag_test(baseofs, VALUE_FLAG_POINTER)) {
    return BOOLEAN_FALSE;
  }
  v = value_variable_get(baseofs);
  if (!v || VARIABLE_NONE == v) {
    return BOOLEAN_FALSE;
  }
  if (VALUE_NONE != value_baseofs_get(baseofs)) {
    return BOOLEAN_FALSE;
  }
  return BOOLEAN_TRUE;
}

void pic_lookup_index_load_w(pfile_t *pf, value_t baseofs)
{
  if (value_is_bit(baseofs)) {
    pic_op_assign(pf, OPERATOR_ASSIGN, VALUE_NONE, baseofs, VALUE_NONE);
  } else if (pic_lookup_index_ok_for_movf(baseofs)) {
    pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, baseofs, 0, PIC_OPDST_W);
  } else {
    pic_op_assign(pf, OPERATOR_ASSIGN, VALUE_NONE, baseofs, VALUE_NONE);
  }
}
