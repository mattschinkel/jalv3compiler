/************************************************************
 **
 ** pic_op_indirect.c : PIC operator generation -- indirect / pointer-slot access
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

/* defined in this TU (prototypes keep definition order free) */
static boolean_t pic_value_is_array_pointer_slot_storage(const value_t val);
static boolean_t pic_pointer_runtime_null_guard_eligible(variable_t v);
static void pic_pointer_slot_emit_null_guard(pfile_t *pf, value_t base,
    variable_sz_t slot_offs);
static boolean_t pic_value_indirect_heap_string_through_pointer(const value_t val);
static value_t pic_indirect_value_get(value_t val1, value_t val2, value_t val3);

/* True array-of-pointers (e.g. **`var pointer var pointer var byte x[N]`**):
 * outer def is ARRAY of POINTER; FSR must load from the pointer *slot* words,
 * not linear *slot+i pointee math (MOVLW-on-indirect **`v__ind`** bug on PIC14).
 * Folded **`var pointer var byte p[N]`** is POINTER→ARRAY (one pointer slot). */
boolean_t pic_value_variable_is_array_of_pointers(const value_t val)
{
  variable_t              v;
  variable_def_t        vd;
  variable_def_member_t   mbr;
  variable_def_t        em;

  v = value_variable_get(val);
  if (!v || VARIABLE_NONE == v) {
    return BOOLEAN_FALSE;
  }
  vd = variable_def_get(v);
  if (!vd || VARIABLE_DEF_TYPE_ARRAY != variable_def_type_get(vd)) {
    return BOOLEAN_FALSE;
  }
  mbr = variable_def_member_get(vd);
  em  = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
  return (em && VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(em));
}

static boolean_t pic_value_is_array_pointer_slot_storage(const value_t val)
{
  return value_flag_test(val, VALUE_FLAG_PTR_SLOT_RAW);
}

/* True if we should emit a runtime "all slot bytes zero" check before loading
 * FSR from a pointer variable. Skips legacy string / device_put paths that
 * are not normal `var pointer` GPR slots. */
static boolean_t pic_pointer_runtime_null_guard_eligible(variable_t v)
{
  const char *nm;

  if (!v || VARIABLE_NONE == v || !variable_is_pointer(v)) {
    return BOOLEAN_FALSE;
  }
  nm = variable_name_get(v);
  if (nm && (strstr(nm, "_device_put")
      || strstr(nm, "_str_")
      || strstr(nm, "___str"))) {
    return BOOLEAN_FALSE;
  }
  return BOOLEAN_TRUE;
}

/*
 * If every byte of the pointer slot at base[slot_offs..] is zero, jump to a
 * local infinite loop (never uses a bogus FSR). Z=1 after IOR means all bytes
 * read as 0 (typical cleared BSS / never bound). BTFSS STATUS,Z skips the
 * `goto lbl_ok` when Z=1 (all slot bytes zero) so execution falls into hang.
 */
static void pic_pointer_slot_emit_null_guard(pfile_t *pf, value_t base,
    variable_sz_t slot_offs)
{
  label_t       lbl_hang;
  label_t       lbl_ok;
  variable_sz_t psz;
  variable_sz_t jj;
  variable_t    v;

  v = value_variable_get(base);
  if (!pic_pointer_runtime_null_guard_eligible(v)) {
    return;
  }
  lbl_hang = pfile_label_alloc(pf, 0);
  lbl_ok   = pfile_label_alloc(pf, 0);
  psz = pic_pointer_size_get(pf);
  pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, base, slot_offs + 0, PIC_OPDST_W);
  for (jj = 1; jj < psz; jj++) {
    pic_instr_append_f_d(pf, PIC_OPCODE_IORWF, base, slot_offs + jj,
        PIC_OPDST_W);
  }
  pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSS, "_status", "_z");
  pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_ok);
  pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_hang);
  pic_instr_append_label(pf, lbl_hang);
  pic_instr_append_n(pf, PIC_OPCODE_GOTO, lbl_hang);
  pic_instr_append_label(pf, lbl_ok);
  label_release(lbl_hang);
  label_release(lbl_ok);
}

/* Dereferenced `var pointer … as var string`: pointee is STRING_DYNAMIC.
 * Indexing the handle must load FSR from the pointer slot (`*p`+field), not
 * LFSR to the slot's file address (`p`+field). */
static boolean_t pic_value_indirect_heap_string_through_pointer(const value_t val)
{
  variable_t            v;
  variable_def_t        def;
  variable_def_member_t mbr;
  variable_def_t        pt;

  if (!value_is_indirect(val) || !value_flag_test(val, VALUE_FLAG_POINTER)) {
    return BOOLEAN_FALSE;
  }
  v = value_variable_get(val);
  if (!v || !variable_is_pointer(v)) {
    return BOOLEAN_FALSE;
  }
  def = variable_def_get(v);
  if (!def) {
    return BOOLEAN_FALSE;
  }
  mbr = variable_def_member_get(def);
  if (!mbr) {
    return BOOLEAN_FALSE;
  }
  pt = variable_def_member_def_get(mbr);
  return (pt && variable_def_flag_test(pt,
      VARIABLE_DEF_FLAG_STRING | VARIABLE_DEF_FLAG_STRING_DYNAMIC))
      ? BOOLEAN_TRUE
      : BOOLEAN_FALSE;
}

void pic_indirect_setup(pfile_t *pf, value_t val, size_t offset)
{
  value_t baseofs;

  pic_stvar_fsr_mark(pf);
  baseofs = value_baseofs_get(val);
  if (value_is_bit(val)) {
    offset += (value_bit_offset_get(val) + 7) / 8;
  }

  if (value_flag_test(val, VALUE_FLAG_PTR_SLOT_RAW)) {
    variable_base_t base;
    value_t         fsr;

    fsr = pic_fsr_get(pf);
    base = value_base_get(val) + offset;
    if (value_is_const(baseofs)) {
      base += (variable_base_t) value_const_get(baseofs);
      base = pic_var_chunk_addr_resolve(value_variable_get(val), base);
      if (pic_is_16bit(pf)) {
        pic_instr_append_lfsr(pf, 0, val, value_const_get(baseofs));
      } else if (pic_is_14bit_hybrid(pf)) {
        pic_fsr0_load_addr(pf, base);
      } else {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base);
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, fsr, 0);
        pic_instr_append(pf, (base >= 256)
           ? PIC_OPCODE_IRP_SET : PIC_OPCODE_IRP_CLR);
      }
    } else {
      if (pic_is_16bit(pf)) {
        pic_instr_append_lfsr(pf, 0, val, 0);
        if (VALUE_NONE != baseofs) {
          pic_fsr0_add_dynamic_index(pf, baseofs, val);
        }
      } else if (pic_is_14bit_hybrid(pf)) {
        pic_fsr0_load_addr(pf, base);
        if (VALUE_NONE != baseofs) {
          pic_fsr0_add_dynamic_index(pf, baseofs, val);
        }
      } else if (VALUE_NONE != baseofs) {
        pic_classic_fsr_load_base_dynamic_index(pf,
            value_variable_get(val), base, baseofs, 0);
      } else {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base);
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, fsr, 0);
        pic_instr_append(pf, (base >= 256)
           ? PIC_OPCODE_IRP_SET : PIC_OPCODE_IRP_CLR);
      }
    }
    value_release(fsr);
    return;
  }

  if (value_is_indirect(val) || value_flag_test(val, VALUE_FLAG_POINTER)) {
    /* STATUS<IRP>:FSR = *value. Const-pointee indexed reads can retain POINTER
     * while INDIRECT is clear (baseofs is const 0), so value_is_indirect alone
     * would skip this block and emit movf INDF before FSR is loaded. */
    value_t tmp;

    tmp = value_clone(val);
    value_baseofs_set(tmp, VALUE_NONE);
    {
      /* chunk-walked addressing: an open-array formal carrying a
       * propagated chunk map (a direct call specialized to a chunked
       * actual), or an UNESCAPED pointer bind whose target is chunked
       * (the pointer is only ever dereferenced, so every access can
       * walk the target's chunks) -- the flat home base / linear
       * base+index cannot address across the holes */
      variable_t walk_var;
      boolean_t  formal_map;

      formal_map = pic_variable_chunks_holed(value_variable_get(val));
      if (!formal_map
          && (VALUE_NONE != baseofs) && value_is_const(baseofs)
          && (pic_var_pointer_bind_walk_target(
                value_variable_get(val)) != VARIABLE_NONE)) {
        /* const subscript through an unescaped walked bind: resolve
         * the linear offset through the TARGET's chunk map and load
         * the absolute address (the home-slot base path below would
         * address linearly across the holes) */
        variable_t wt;
        variable_base_t linear;
        variable_base_t phys;

        wt = pic_var_pointer_bind_walk_target(value_variable_get(val));
        linear = variable_base_get(wt, 0)
          + (variable_base_t) value_const_get(baseofs)
          + (variable_base_t) offset;
        phys = pic_var_chunk_addr_resolve(wt, linear);
        if (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf)) {
          pic_fsr0_load_addr(pf, phys);
        } else {
          pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW,
              (unsigned) (phys & 0xff));
          pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr");
          pic_instr_append(pf, (phys >= 256)
              ? PIC_OPCODE_IRP_SET : PIC_OPCODE_IRP_CLR);
        }
        value_release(tmp);
        return;
      }
      if ((VALUE_NONE != baseofs) && !value_is_const(baseofs)
          && (formal_map
            || (pic_var_pointer_bind_walk_target(
                  value_variable_get(val)) != VARIABLE_NONE))) {
        /* const baseofs on a map-carrying formal falls through to the
         * const path below, which resolves through the chunk map */
        walk_var = formal_map
          ? value_variable_get(val)
          : pic_var_pointer_bind_walk_target(value_variable_get(val));
        pic_fsr_chunk_walk_emit_ofs(pf, walk_var, baseofs, offset,
            pic_is_16bit(pf) || pic_is_14bit_hybrid(pf));
        value_release(tmp);
        return;
      }
    }

    /* `var pointer … p` then `p[i]` (pointee is array): linear *p+index.
     * True **`ARRAY` of `POINTER`** (nested data pointer): each element is a
     * pointer slot; use the same path as a plain subscripted pointer.
     * Heap `var string` through `var pointer … as var string`: see
     * `pic_value_indirect_heap_string_through_pointer`. */
    if (value_flag_test(val, VALUE_FLAG_ARRAY)
        && !pic_value_indirect_heap_string_through_pointer(val)
        && (!pic_value_variable_is_array_of_pointers(val)
          || pic_value_is_array_pointer_slot_storage(val))) {
      pic_opcode_t op;

      if (value_is_const(baseofs)) {
        offset += value_const_get(baseofs);
      }
      value_baseofs_set(tmp, VALUE_NONE);
      {
        value_t gtmp;

        gtmp = value_clone(tmp);
        value_baseofs_set(gtmp, VALUE_NONE);
        value_flag_clr(gtmp, VALUE_FLAG_POINTER);
        value_flag_clr(gtmp, VALUE_FLAG_INDIRECT);
        if (!pic_value_is_array_pointer_slot_storage(val)) {
          pic_pointer_slot_emit_null_guard(pf, gtmp, 0);
        }
        value_release(gtmp);
      }
      if (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf)) {
        if (pic_is_16bit(pf)) {
          pic_instr_append_lfsr(pf, 0, tmp, offset);
        } else {
          variable_base_t base;

          base = pic_var_chunk_addr_resolve(value_variable_get(tmp),
              value_base_get(tmp) + offset);
          pic_fsr0_load_addr(pf, base);
        }
        if ((VALUE_NONE != baseofs) && !value_is_const(baseofs)) {
          pic_fsr0_add_dynamic_index(pf, baseofs, val);
        }
      } else {
        if ((VALUE_NONE != baseofs) && !value_is_const(baseofs)) {
          if (value_is_bit(baseofs)) {
            pic_op_assign(pf, OPERATOR_ASSIGN, VALUE_NONE, baseofs, 
                VALUE_NONE);
            pic_instr_append_f(pf, PIC_OPCODE_ADDLW, tmp, offset);
            pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr");
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, 1, PIC_OPDST_W);
            pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
            pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, 1);
            pic_classic_irp_set_from_w(pf);
          } else {
            variable_sz_t range;

            range = (variable_sz_t) (value_byte_sz_get(tmp) - offset);
            pic_classic_fsr_load_base_dynamic_index(pf,
                value_variable_get(tmp),
                value_base_get(tmp) + offset, baseofs, range);
          }
        } else {
            pic_instr_append_f(pf, PIC_OPCODE_MOVLW, tmp, offset);
            pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr");
            op = (value_base_get(tmp) >= 256)
              ? PIC_OPCODE_IRP_SET
              : PIC_OPCODE_IRP_CLR;
            pic_instr_append(pf, op);
        }
      }
    } else {
      /* assert(value_is_pointer(val)); */
      value_flag_clr(tmp, VALUE_FLAG_INDIRECT);
      /* Element is typed as the pointee; POINTER stays set and makes
       * value_is_indirect() true (baseofs is often const 0). pic_instr_append_f_d
       * would then treat tmp as an INDF read (movf 0,w) before FSR is loaded.
       * Clear POINTER so MOV reads the pointer slot (see const-pointee loads). */
      value_flag_clr(tmp, VALUE_FLAG_POINTER);

      {
        variable_sz_t loff;
        boolean_t     slot_array;

        /* `p[i]` / `p`+offset: const baseofs is an index into the *pointee*
         * (MOVF p,0 then ADDLW). `slot[i]` on `var pointer … slot[N]`: const
         * baseofs is a byte offset within the pointer *aggregate* (MOVF slot+2). */
        slot_array = pic_value_variable_is_array_of_pointers(val);
        loff = 0;
        if (slot_array && VALUE_NONE != baseofs && value_is_const(baseofs)) {
          loff = (variable_sz_t) value_const_get(baseofs);
        }
        pic_pointer_slot_emit_null_guard(pf, tmp, loff);
        if ((VALUE_NONE != baseofs) && !value_is_const(baseofs)) {
          if (slot_array) {
            /* ARRAY-of-pointers element at a runtime index: DOUBLE
             * indirection -- FSR = &slot[k], load the slot's pointer
             * bytes, then re-stage FSR (and IRP) from the loaded
             * pointer so the caller's INDF access hits the element's
             * real target. The previous `*slot0 + index` staging used
             * slot 0's VALUE (a bound code address) as the data base
             * (the classic runtime-index read+write defect + the
             * stage-2 call blocker, 2026-10-28). 16-bit core + hybrid
             * variant added 2026-10-28: those cores previously never
             * compiled here (the staging emitted 14-bit _fsr/_irp
             * names) and the generic FSR0 tail re-staged from the
             * ARRAY's own storage -- silently wrong element targets. */
            value_t indv;
            value_t fsrv;
            value_t ptmp;
            boolean_t wide16;

            wide16 = pic_is_16bit(pf) || pic_is_14bit_hybrid(pf);
            indv = pic_indirect_get(pf, PFILE_LOG_ERR, 0);
            fsrv = pic_fsr_get(pf);
            ptmp = pic_var_pointer_get(pf);
            pic_classic_fsr_load_base_dynamic_index(pf,
                value_variable_get(tmp), value_base_get(tmp),
                baseofs, (variable_sz_t) value_byte_sz_get(tmp));
            /* load the slot's own pointer into the SCRATCH (not tmp --
             * tmp maps to the array's own storage and would clobber
             * slot 0) */
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, indv, 0,
                PIC_OPDST_W);
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, ptmp, 0);
            pic_instr_append_f_d(pf, PIC_OPCODE_INCF, fsrv, 0,
                PIC_OPDST_F);
            if (wide16) {
                /* INCF _fsr0l alone cannot cross a 256-byte boundary */
                pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC,
                    "_status", "_z");
                pic_instr_append_reg(pf, PIC_OPCODE_INCF, "_fsr0h");
            }
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, indv, 0,
                PIC_OPDST_W);
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, ptmp, 1);
            /* FSR = the loaded pointer: fsr0l/fsr0h on wide cores,
             * fsr + IRP-latch from bit 8 on the 14-bit core */
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ptmp, 0,
                PIC_OPDST_W);
            pic_instr_append_f(pf, PIC_OPCODE_MOVWF, fsrv, 0);
            if (wide16) {
                pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, ptmp, 1, 0);
                pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0h");
            } else {
                pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status",
                    "_irp");
                pic_instr_append_f_d(pf, PIC_OPCODE_BTFSC, ptmp, 1, 0);
                pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status",
                    "_irp");
            }
            pic_var_pointer_release(pf, ptmp);
            value_release(fsrv);
            value_release(indv);
            /* the caller's INDF access now hits the element's real
             * target; the common FSR/IRP tail below would re-stage
             * from the value's own storage and clobber it */
            value_release(tmp);
            return;
          } else if (variable_is_array(value_variable_get(tmp))) {
            /* plain ARRAY element at a runtime index: the base is the
             * array's own address (MOVLW), not its first element's
             * value (MOVF would read arr[0] as the base) */
            pic_classic_fsr_load_base_dynamic_index(pf,
                value_variable_get(tmp), value_base_get(tmp),
                baseofs, (variable_sz_t) value_byte_sz_get(tmp));
          } else {
            pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, 0,
                PIC_OPDST_W);
            pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, baseofs, 0,
                PIC_OPDST_W);
          }
        } else if (slot_array) {
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, loff, PIC_OPDST_W);
        } else {
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, 0, PIC_OPDST_W);
          if (baseofs) {
            if (value_is_const(baseofs)) {
              if (value_const_get(baseofs)) {
                pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW,
                    value_const_get(baseofs));
              }
            } else {
              pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, baseofs, 0,
                  PIC_OPDST_W);
            }
          }
        }
      }
      if (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf)) {
        /* Check if this is a function parameter that should use direct addressing */
        variable_t var = value_variable_get(val);
        if (var) {
          const char *name = variable_name_get(var);
          /* Skip FSR0 for function parameters, preserve for actual pointer operations */
          if (name && (strstr(name, "_device_put") ||
                       strstr(name, "_str_") ||
                       strstr(name, "___str"))) {
            return;
          }
        }
        pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0l");
        if ((VALUE_NONE != baseofs) && !value_is_const(baseofs)
            && !pic_value_variable_is_array_of_pointers(val)) {
          variable_sz_t idx_sz;

          idx_sz = pic_dynamic_index_byte_sz_get(baseofs);
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, 1, PIC_OPDST_W);
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
          pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, 1);
          if (idx_sz > 1) {
            pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, baseofs, 1,
                PIC_OPDST_W);
          }
        } else if ((VALUE_NONE != baseofs) && !value_is_const(baseofs)) {
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, 1, PIC_OPDST_W);
        } else if (pic_value_variable_is_array_of_pointers(val)) {
          variable_sz_t loff;

          loff = 0;
          if (VALUE_NONE != baseofs && value_is_const(baseofs)) {
            loff = (variable_sz_t) value_const_get(baseofs);
          }
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, loff + 1, PIC_OPDST_W);
        } else {
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, 1, PIC_OPDST_W);
        }
        pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr0h");
      } else {
        variable_sz_t loff;
        variable_sz_t irp_byte;

        pic_instr_append_reg(pf, PIC_OPCODE_MOVWF, "_fsr");
        loff = 0;
        if (pic_value_variable_is_array_of_pointers(val)
            && VALUE_NONE != baseofs && value_is_const(baseofs)) {
          loff = (variable_sz_t) value_const_get(baseofs);
        }
        if ((VALUE_NONE != baseofs) && !value_is_const(baseofs)
            && !pic_value_variable_is_array_of_pointers(val)) {
          variable_sz_t idx_sz;

          idx_sz = pic_dynamic_index_byte_sz_get(baseofs);
          pic_instr_append_f_d(pf, PIC_OPCODE_MOVF, tmp, 1, PIC_OPDST_W);
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BTFSC, "_status", "_c");
          pic_instr_append_w_kn(pf, PIC_OPCODE_ADDLW, 1);
          if (idx_sz > 1) {
            pic_instr_append_f_d(pf, PIC_OPCODE_ADDWF, baseofs, 1,
                PIC_OPDST_W);
          }
          pic_classic_irp_set_from_w(pf);
        } else {
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BCF, "_status", "_irp");
          irp_byte = pic_value_variable_is_array_of_pointers(val)
              ? (variable_sz_t) (loff + pic_pointer_size_get(pf) - 1)
              : (variable_sz_t) (pic_pointer_size_get(pf) - 1);
          pic_instr_append_f_bn(pf, PIC_OPCODE_BTFSC, tmp, irp_byte, 0);
          pic_instr_append_reg_flag(pf, PIC_OPCODE_BSF, "_status", "_irp");
        }
      }
    }
    value_release(tmp);
  } else {
    variable_base_t base;
    value_t         fsr;

    fsr = pic_fsr_get(pf);
    base = value_base_get(val) + offset;
    if (value_is_const(baseofs)) {
      /* fsr = value_base_get(value) + value_const_get(baseofs) + sz */
      base += (variable_base_t) value_const_get(baseofs);
      base = pic_var_chunk_addr_resolve(value_variable_get(val), base);
      if (pic_is_16bit(pf)) {
        pic_instr_append_lfsr(pf, 0, val, value_const_get(baseofs));
      } else if (pic_is_14bit_hybrid(pf)) {
        pic_fsr0_load_addr(pf, base);
      } else {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base);
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, fsr, 0);
        pic_instr_append(pf, (base >= 256)
           ? PIC_OPCODE_IRP_SET : PIC_OPCODE_IRP_CLR);
      }
    } else {
      /* fsr = base (+ sz - 1) */
      if (pic_is_16bit(pf)) {
        pic_instr_append_lfsr(pf, 0, val, 0);
      } else if (pic_is_14bit_hybrid(pf)) {
        pic_fsr0_load_addr(pf, base);
      } else {
        pic_instr_append_w_kn(pf, PIC_OPCODE_MOVLW, base);
        pic_instr_append_f(pf, PIC_OPCODE_MOVWF, fsr, 0);
        pic_instr_append(pf, (base >= 256)
           ? PIC_OPCODE_IRP_SET : PIC_OPCODE_IRP_CLR);
      }
      if (VALUE_NONE != baseofs) {
        if (pic_is_16bit(pf) || pic_is_14bit_hybrid(pf)) {
          pic_fsr0_add_dynamic_index(pf, baseofs, val);
        } else {
          pic_classic_fsr_load_base_dynamic_index(pf,
              value_variable_get(val), base, baseofs,
              (variable_sz_t) (value_byte_sz_get(val) - offset));
        }
      }
    }
    value_release(fsr);
  }
}

/*
 * given three values, determine which one is indirect. This assumes
 * (obviously) that only one is indirect!
 */
static value_t pic_indirect_value_get(value_t val1, value_t val2, value_t val3)
{
  value_t val;

  if (value_is_indirect(val1)
      || (val1 && value_flag_test(val1, VALUE_FLAG_POINTER))) {
    assert((value_is_same(val1, val2) || !value_is_indirect(val2))
           && (value_is_same(val1, val3) || !value_is_indirect(val3)));
    val = val1;
  } else if (value_is_indirect(val2)) {
    assert(value_is_same(val2, val3) || !value_is_indirect(val3));
    val = val2;
  } else if (value_is_indirect(val3)) {
    val = val3;
  } else {
    val = VALUE_NONE;
  }
  return val;
}

/*
 * given three values (only one of which is indirect) and an offset,
 * setup the irp:ind to the requested offset. This is a no-op
 * if none of the values is indirect
 *   ipos is used to track the relative indirect offset & should
 *        *never* be modified except by pic_indirect_bump3
 */
void pic_indirect_setup3(pfile_t *pf, value_t val1, value_t val2,
  value_t val3, variable_sz_t ofs, variable_sz_t *ipos)
{
  value_t val;

  val = pic_indirect_value_get(val1, val2, val3);
  if (VALUE_NONE != val) {
    variable_sz_t n;

    n = (ofs >= value_sz_get(val))
      ? value_sz_get(val) - 1
      : ofs;
    pic_indirect_setup(pf, val, n);
  }
  *ipos = ofs;
}

/*
 * if any of the three values is indirect, and the position is in range
 * bump the position in the desired direction
 */
void pic_indirect_bump3(pfile_t *pf, value_t val1, value_t val2,
  value_t val3, variable_sz_t ix, variable_sz_t *ipos)
{
  value_t val;

  val = pic_indirect_value_get(val1, val2, val3);
  if (VALUE_NONE != val) {
    /* we can only move one in either direction */
    value_t fsr;
    
    assert((ix == *ipos)
        || (ix + 1 == *ipos) 
        || (ix - 1 == *ipos));

    fsr = pic_fsr_get(pf);
    if ((ix > *ipos) && (ix < value_sz_get(val))) {
      pic_instr_append_f_d(pf, PIC_OPCODE_INCF, fsr, 0, PIC_OPDST_F);
    } else if ((ix < *ipos) && (*ipos < value_sz_get(val))) {
      pic_instr_append_f_d(pf, PIC_OPCODE_DECF, fsr, 0, PIC_OPDST_F);
    }
    value_release(fsr);
    *ipos = ix;
  }
}
