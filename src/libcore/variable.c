/**********************************************************
 **
 ** variable.c : manipulators for variable_t
 **
 ** Copyright (c) 2004-2005, Kyle A. York 
 **               2018-2018, Rob Jansen
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: fork extensions (heavy rework)
 **
 ***********************************************************/
#include <assert.h>
#if defined(__linux__)
#include <execinfo.h>
#endif
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../libutils/mem.h"
/*#include "../libutils/array.h"*/
#include "pf_proc.h"
#include "value.h"
#include "vararray.h"
#include "variable.h"

variable_const_t variable_const_mask_get(unsigned bits)
{
  if (!bits) {
    return 0;
  }
  if (bits >= VARIABLE_CONST_BIT_CT) {
    return (variable_const_t) -1;
  }
  return (((variable_const_t) 1) << bits) - 1;
}

boolean_t variable_const_sign_bit_test(variable_const_t n, unsigned bits)
{
  if (!bits) {
    return BOOLEAN_FALSE;
  }
  if (bits > VARIABLE_CONST_BIT_CT) {
    bits = VARIABLE_CONST_BIT_CT;
  }
  return (n & ((variable_const_t) 1 << (bits - 1))) ? BOOLEAN_TRUE
    : BOOLEAN_FALSE;
}

variable_const_t variable_const_sign_extend(variable_const_t n, unsigned bits)
{
  variable_const_t mask;

  if (!bits || (bits >= VARIABLE_CONST_BIT_CT)) {
    return n;
  }
  mask = variable_const_mask_get(bits);
  n &= mask;
  if (variable_const_sign_bit_test(n, bits)) {
    n |= ~mask;
  }
  return n;
}

variable_const_t variable_const_normalize(variable_const_t n,
    variable_def_t def)
{
  variable_sz_t bits;

  if (!def) {
    return n;
  }
  bits = variable_def_sz_get(def);
  if (!variable_def_flag_test(def, VARIABLE_DEF_FLAG_BIT)) {
    bits *= 8;
  }
  if (VARIABLE_DEF_TYPE_BOOLEAN == variable_def_type_get(def)) {
    n = !!n;
  } else if ((VARIABLE_DEF_TYPE_FUNCTION == variable_def_type_get(def))
      || (VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(def))) {
    /* These constants carry addresses or aggregate data. */
  } else if (bits < VARIABLE_CONST_BIT_CT) {
    n &= variable_const_mask_get(bits);
    if (variable_def_flag_test(def, VARIABLE_DEF_FLAG_SIGNED)) {
      n = variable_const_sign_extend(n, bits);
    }
  }
  if (variable_def_flag_test(def, VARIABLE_DEF_FLAG_BIT)
    && (1 == bits) 
    && variable_def_flag_test(def, VARIABLE_DEF_FLAG_SIGNED)) {
    n = -n;
  }
  return n;
}

boolean_t variable_const_is_negative(variable_const_t n, unsigned bits)
{
  return variable_const_sign_bit_test(n, bits);
}

int variable_const_cmp(variable_const_t left, variable_const_t right,
    unsigned bits, boolean_t is_signed)
{
  if (!bits || (bits > VARIABLE_CONST_BIT_CT)) {
    bits = VARIABLE_CONST_BIT_CT;
  }
  left &= variable_const_mask_get(bits);
  right &= variable_const_mask_get(bits);
  if (is_signed) {
    boolean_t left_neg;
    boolean_t right_neg;

    left_neg = variable_const_is_negative(left, bits);
    right_neg = variable_const_is_negative(right, bits);
    if (left_neg != right_neg) {
      return left_neg ? -1 : 1;
    }
  }
  if (left < right) {
    return -1;
  }
  if (left > right) {
    return 1;
  }
  return 0;
}

unsigned char variable_const_byte_get(variable_const_t n, unsigned ofs)
{
  if (ofs >= sizeof(n)) {
    return 0;
  }
  return (unsigned char) ((n >> (8 * ofs)) & 0xff);
}

void variable_const_str_get(variable_const_t n, boolean_t is_signed,
    char *buf, size_t buf_sz)
{
  char tmp[50];
  size_t pos;
  boolean_t neg;

  if (!buf_sz) {
    return;
  }
  neg = BOOLEAN_FALSE;
  if (is_signed && variable_const_is_negative(n, VARIABLE_CONST_BIT_CT)) {
    neg = BOOLEAN_TRUE;
    n = ~n + 1;
  }
  pos = 0;
  do {
    tmp[pos++] = (char) ('0' + (unsigned) (n % 10));
    n /= 10;
  } while (n && (pos < sizeof(tmp)));
  if (neg && (pos < sizeof(tmp))) {
    tmp[pos++] = '-';
  }
  {
    size_t out;

    out = 0;
    while (pos && (out + 1 < buf_sz)) {
      buf[out++] = tmp[--pos];
    }
    buf[out] = 0;
  }
}

static unsigned variable_id;

typedef struct variable_ref_ {
  variable_t var;
  refct_t    ref_ct;
} variable_ref_t;
#define VARIABLE_REF_NONE ((variable_ref_t *) 0)

typedef struct variable_pointer_whereis_slot_ {
  variable_const_t slot_ofs;
  value_t          target;
} variable_pointer_whereis_slot_t;

struct variable_ {
  variable_t         link;
  refct_t            ref_ct;

  unsigned           id;

  tag_t              tag;
  unsigned           tag_n;

  flag_t             flags;
  variable_def_t     def;       /* the variable's definition  */
  variable_base_t    base[VARIABLE_MIRROR_CT];
                                /* base position, or offset from
                                   master's base position     */
  unsigned short     bit_offset;/* bit #                      */
  const void        *origin;    /* declaring source file (JAL `var`),
                                   used for explicit-pin conflict checks */
  ctr_t              ctr_assign;/* # of times assigned        */
  ctr_t              ctr_use;   /* # of times used            */
  unsigned char     *data;      /* misc. data; size = ct * sz */
  struct {
    float            single;
    float           *array;
  } cflt;

  /* if this is an alias, master points to the controlling variable */
  variable_t         master;
  /* strdup'd first token of `p is <id>` for pseudo-'get rvalue resolution */
  char              *ptr_rvalue_link_name;
  /* cloned target lvalue from the last scalar `p is ...`; used by WHEREIS(p) */
  value_t            ptr_whereis_target;
  /* cloned target lvalues for indexed pointer slots; keyed by byte offset */
  array_t           *ptr_whereis_slots;
  /* must-bind tracking: real slot writes (binds/copies) count, and
   * through-accesses on pointer-slot arrays (const element offsets
   * used + runtime-indexed bind count for the element check) */
  ctr_t              ctr_ptr_bound;
  ctr_t              ctr_ptr_slot_rt_bind;
  array_t           *ptr_slot_uses;
  /* per-element pseudo links for pointer-slot arrays: the bound pseudo
   * NAME keyed by byte offset (static const-index dispatch) */
  array_t           *ptr_slot_links;
  /* stage-2 runtime dispatch helpers per marked pointer-slot array:
   * a byte kind[] mirror (1 = pseudo element) and a fn-ptr array of
   * the elements' 'put accessors (the slot itself stores 'get) */
  variable_t         ptr_slot_kind_var;
  variable_t         ptr_slot_puts_var;
  variable_t         ptr_slot_getcall_var;
  variable_t         ptr_slot_putcall_var;
  /* for functions this is the destination */
  pfile_proc_t      *proc;
  label_t            lbl;
  value_t            val; /* for whereis(val) */

  const char        *name;

  void              *user_data;

  array_t           *used_by;  /* variables used by this one */
  array_t           *uses;     /* variables this one uses    */
  variable_array_t  *interference;
};

static cache_t variable_cache;
static boolean_t variable_cache_is_init;

static void variable_cache_cleanup(void)
{
#if 0
  unsigned n;

  for (n = 1; n < variable_cache.next; n++) {
    struct variable_ *ptr;

    ptr = cache_element_seek(&variable_cache, n, BOOLEAN_FALSE);
    if (ptr->ref_ct != (refct_t) 0xefbe) {
      printf("var leaked: %u (refct = %u)\n", n, ptr->ref_ct);
    }
  }
#endif
  cache_cleanup(&variable_cache);
}

static variable_t variable_element_alloc(void)
{
  if (!variable_cache_is_init) {
    variable_cache_is_init = BOOLEAN_TRUE;
    atexit(variable_cache_cleanup);
    (void) cache_init(&variable_cache, sizeof(struct variable_), "variable");
  }
  return cache_element_alloc(&variable_cache);
}

static struct variable_ *variable_element_seek(variable_t el, boolean_t mod)
{
  struct variable_ *ptr;

  ptr  = cache_element_seek(&variable_cache, el, mod);
  return ptr;
}

/* basic functions */
variable_t variable_alloc(tag_t tag, variable_def_t def)
{
  variable_t var;

  var = variable_element_alloc();
  if (var) {
    struct variable_ *ptr;
    size_t            data_sz;

    ptr = variable_element_seek(var, BOOLEAN_TRUE);

    ptr->id = ++variable_id;
    if (tag) {
      tag_lock(tag);
      ptr->tag = tag;
      ptr->tag_n = tag_n_get(tag);
      tag_n_bump(tag);
    } else {
      ptr->tag   = 0;
      ptr->tag_n = 0;
    }

    ptr->data        = 0;
    ptr->cflt.single = 0.0;
    ptr->cflt.array  = 0;
    ptr->link        = 0;  /* Initialize link to NULL */
    data_sz = (variable_def_flag_test(def, VARIABLE_DEF_FLAG_CONST))
      ? variable_def_sz_get(def) : 0;
    if (data_sz && var) {
      ptr->data = MALLOC(data_sz);
      if (!ptr->data) {
        var = VARIABLE_NONE;
      } else {
	      memset(ptr->data, 0, data_sz);
	    }
      if ((VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(def)
        && (VARIABLE_DEF_TYPE_FLOAT == variable_def_type_get(
          variable_def_member_def_get(variable_def_member_get(def)))))) {
        size_t ct;

        ct = variable_def_member_ct_get(variable_def_member_get(def));
        ptr->cflt.array = MALLOC(ct * sizeof(*ptr->cflt.array));
        if (ptr->cflt.array) {
          while (ct--) {
            ptr->cflt.array[ct] = 0.0;
          }
        }
      }
    }

    if (var) {
      size_t ii;

      ptr->link       = VARIABLE_NONE;
      ptr->ref_ct     = 1;
      ptr->flags      = VARIABLE_FLAG_NONE;

      ptr->def        = def;
      for (ii = 0; ii < VARIABLE_MIRROR_CT; ii++) {
        ptr->base[ii] = VARIABLE_BASE_UNKNOWN;
      }
      ptr->bit_offset = 0;
      ptr->origin    = 0;
      ptr->ctr_assign = 0;
      ptr->ctr_use    = 0;
      ptr->master     = VARIABLE_NONE;
      ptr->ptr_rvalue_link_name = 0;
      ptr->ctr_ptr_bound        = 0;
      ptr->ctr_ptr_slot_rt_bind = 0;
      ptr->ptr_slot_uses        = 0;
      ptr->ptr_slot_links       = 0;
      ptr->ptr_slot_kind_var    = VARIABLE_NONE;
      ptr->ptr_slot_puts_var    = VARIABLE_NONE;
      ptr->ptr_slot_getcall_var = VARIABLE_NONE;
      ptr->ptr_slot_putcall_var = VARIABLE_NONE;
      ptr->ptr_whereis_target = VALUE_NONE;
      ptr->ptr_whereis_slots = array_alloc(0,
          sizeof(variable_pointer_whereis_slot_t));
	  ptr->val        = VALUE_NONE; 
      ptr->proc       = PFILE_PROC_NONE;
      ptr->lbl        = LABEL_NONE;
      ptr->name       = tag_name_get(tag);
      ptr->user_data  = 0;
      ptr->uses       = array_alloc(0, sizeof(variable_ref_t));
      ptr->used_by    = array_alloc(0, sizeof(variable_ref_t));
      ptr->interference  = variable_array_alloc(0);
    }
  }
  return var;
}

void variable_release(variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr && !--ptr->ref_ct) { 
    if (ptr->ptr_rvalue_link_name) {
      FREE(ptr->ptr_rvalue_link_name);
      ptr->ptr_rvalue_link_name = 0;
    }
    value_release(ptr->ptr_whereis_target);
    ptr->ptr_whereis_target = VALUE_NONE;
    if (ptr->ptr_slot_uses) {
      array_free(ptr->ptr_slot_uses);
      ptr->ptr_slot_uses = 0;
    }
    if (ptr->ptr_slot_links) {
      size_t li;

      for (li = 0; li < array_ct(ptr->ptr_slot_links); li++) {
        struct slot_link_rel {
          variable_const_t ofs;
          char            *name;
        };
        struct slot_link_rel *ent;

        ent = array_get(ptr->ptr_slot_links, li);
        if (ent && ent->name) {
          FREE(ent->name);
          ent->name = 0;
        }
      }
      array_free(ptr->ptr_slot_links);
      ptr->ptr_slot_links = 0;
    }
    if (ptr->ptr_whereis_slots) {
      size_t ii;

      for (ii = 0; ii < array_ct(ptr->ptr_whereis_slots); ii++) {
        variable_pointer_whereis_slot_t *slot;

        slot = array_get(ptr->ptr_whereis_slots, ii);
        if (slot) {
          value_release(slot->target);
          slot->target = VALUE_NONE;
        }
      }
      array_free(ptr->ptr_whereis_slots);
      ptr->ptr_whereis_slots = 0;
    }
    variable_master_set(var, VARIABLE_NONE);
    ptr = variable_element_seek(var, BOOLEAN_TRUE);
    array_free(ptr->used_by);
    array_free(ptr->uses);
    variable_array_free(ptr->interference);
    FREE(ptr->cflt.array);
    ptr->cflt.array = 0;
    variable_data_set(var, 0);
    variable_label_set(var, LABEL_NONE);
    tag_release(ptr->tag);
    cache_element_free(&variable_cache, var);
  }
}

void variable_lock(variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    ptr->ref_ct++;
  }
}

/* member retrieval */
boolean_t variable_dflag_test(const variable_t var, flag_t flags)
{
  return variable_def_flag_test(variable_def_get(var), flags);
}

flag_t variable_dflags_get_all(const variable_t var)
{
  return variable_def_flags_get_all(variable_def_get(var));
}

void variable_flag_set(variable_t var, flag_t flags)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    ptr->flags |= flags;
  }
}

void variable_flag_clr(variable_t var, flag_t flags)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    ptr->flags &= ~flags;
  }
}

boolean_t variable_flag_test(const variable_t var, flag_t flags)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr) ? ((ptr->flags & flags) == flags) : 0; /* BOOLEAN_FALSE; */
}

boolean_t variable_is_shared(const variable_t var)
{
  variable_t vptr;

  vptr = var;
  while (variable_master_get(vptr)) {
    vptr = variable_master_get(vptr);
  }
  return variable_flag_test(vptr, VARIABLE_FLAG_SHARED);
}

const char *variable_name_get(const variable_t var)
{
  struct variable_ *ptr;
  const char       *name;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  name = (ptr) ? tag_name_get(ptr->tag) : 0;
  if (ptr && !name) {
    name = variable_name_get(variable_master_get(var));
  }
  return name;
}

unsigned variable_tag_n_get(const variable_t var)
{
  struct variable_ *ptr;
  const char       *name;
  unsigned          n;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  name = (ptr) ? tag_name_get(ptr->tag) : 0;

  if (ptr && !name) {
    n = variable_tag_n_get(variable_master_get(var));
  } else if (ptr) {
    n = ptr->tag_n;
  } else {
    n = 0;
  }
  return n;
}

variable_sz_t   variable_sz_get(const variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr) ? variable_def_sz_get(ptr->def): 0;
}

variable_base_t variable_base_real_get(const variable_t var, size_t which)
{
  struct variable_ *ptr;
  variable_base_t   base;

  ptr  = variable_element_seek(var, BOOLEAN_FALSE);
  base = VARIABLE_BASE_UNKNOWN;
  if (ptr && (which < VARIABLE_MIRROR_CT)) {
    base = ptr->base[which];
  }
  return base;
}

variable_base_t variable_base_get(variable_t var, size_t which)
{
  variable_base_t base;

  base = VARIABLE_BASE_UNKNOWN;
  do {
    variable_base_t mbase;

    mbase = variable_base_real_get(var, which);
    if (VARIABLE_BASE_UNKNOWN != mbase) {
      if (VARIABLE_BASE_UNKNOWN == base) {
        base = mbase;
      } else {
        base += mbase;
      }
    }
    var = variable_master_get(var);
  } while (var);
  return base;
}

void variable_base_set(variable_t var, variable_base_t base, size_t which)
{
  struct variable_ *ptr;

  if (getenv("JALV2_DBG_VBS") && variable_name_get(var)) {
#if defined(__linux__)
    void *bt[8];
    int  n = backtrace(bt, 8);
    fprintf(stderr, "VBS %s -> 0x%04X\n", variable_name_get(var),
        (unsigned) base);
    backtrace_symbols_fd(bt, n, 2);
#else
    fprintf(stderr, "VBS %s -> 0x%04X\n", variable_name_get(var),
        (unsigned) base);
#endif
  }
  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    if (which < VARIABLE_MIRROR_CT) {
      ptr->base[which] = base;
    }
  }
}

variable_const_t variable_const_get(const variable_t var, 
    variable_def_t def, size_t ofs)
{
  struct variable_ *ptr;
  variable_const_t  n;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  if (ptr && (ptr->lbl || ptr->val)) {
    if (ptr->lbl) {
      n = label_pc_get(ptr->lbl);
    } else {
      variable_base_t base;
      value_t         baseofs;

      base = value_base_get(ptr->val);
      n = (VARIABLE_BASE_UNKNOWN == base) ? 0 : base;
      baseofs = value_baseofs_get(ptr->val);
      if (baseofs && value_is_const(baseofs)) {
        n += value_const_get(baseofs);
      }
    }
    if (ofs < sizeof(n)) {
      n >>= 8 * ofs;
    } else if (ofs) {
      n = 0;
    }
    n = variable_const_normalize(n, def);
  } else if (ptr && ptr->data) {
    size_t           ii;
    size_t           pos_mx;

    if (variable_dflag_test(var, VARIABLE_DEF_FLAG_BIT)) {
      pos_mx = (variable_sz_get(var) + 7) / 8;
    } else {
      pos_mx = variable_sz_get(var);
    }

    for (ii = 0, n = 0; (ii < variable_def_sz_get(def))
        && (ii < sizeof(n)); ii++) {
      /*n <<= 8UL;*/
      if (ofs + ii < pos_mx) {
        n |= ((variable_const_t) ptr->data[ofs + ii]) << (8 * ii);
      }
    }
    if (variable_def_flag_test(def, VARIABLE_DEF_FLAG_SIGNED)) {
      unsigned bits;

      bits = 8 * variable_def_sz_get(def);
      if (bits > VARIABLE_CONST_BIT_CT) {
        bits = VARIABLE_CONST_BIT_CT;
      }
      n = variable_const_sign_extend(n, bits);
    }
  } else {
    n = 0;
  }
  return n;
}

void variable_const_set(variable_t var, variable_def_t vdef,
    size_t ofs, variable_const_t n)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr && ptr->data) {
    size_t ii;
    size_t ofs_mx;

    if (variable_dflag_test(var, VARIABLE_DEF_FLAG_BIT)) {
      ofs_mx = (variable_sz_get(var) + 7) / 8;
      if (VARIABLE_DEF_TYPE_BOOLEAN == variable_type_get(var)) {
        n = !!n;
      } else {
        n &= variable_const_mask_get(variable_sz_get(var));
      }
      if (variable_is_signed(var)
        && variable_const_sign_bit_test(n, variable_sz_get(var))) {
        /* high bit is set, so negate n */
        n = -n;
      }
    } else {
      ofs_mx = variable_sz_get(var);
    }
    for (ii = 0; ii < variable_def_sz_get(vdef); ii++) {
      if (ofs + ii < ofs_mx) {
        ptr->data[ofs + ii] = variable_const_byte_get(n, 0);
      }
      n >>= 8;
    }
  }
}

float variable_const_float_get(const variable_t var, size_t ofs)
{
  struct variable_ *ptr;
  float             f;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  f   = 0.0;
  if (ptr && ptr->data) {
    if (variable_is_array(var)) {
      assert((ofs % 4) == 0);
      f = ptr->cflt.array[ofs / 4];
    } else {
      assert(0 == ofs);
      f = ptr->cflt.single;
    }
  }
  return f;
}

void variable_const_float_set(variable_t var, size_t ofs, float n)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr && ptr->data) {
    int           e;
    unsigned long mantissa;

    /* 
     * convert the floating point to data. I don't know that
     * all compilers use IEEE754 (though it appears to be
     * required by the C9x standard), and don't know if some
     * are little or big endian, so I convert manually here
     * using frexp(), and assuming little endiant:
     *   m0 m1 m2:exp0 exp1..exp7:sign
     */
    if (variable_is_array(var)) {
      assert((ofs % 4) == 0);
      ptr->cflt.array[ofs/4] = n;
    } else {
      assert(0 == ofs);
      ptr->cflt.single = n;
    }
    if (n == 0) {
      /* -0.0 == +0.0 numerically; keep the sign of a negative zero
       * (1/-0.0 is -inf, 1/+0.0 is +inf) */
      mantissa = (1.0f / n < 0.0f) ? 0x80000000UL : 0;
    } else if (n != n) {
      /* NaN: frexp(NaN) is undefined; store the quiet-NaN pattern */
      mantissa = 0x7FC00000UL;
    } else if ((n > 3.402823466e+38f) || (n < -3.402823466e+38f)) {
      /* infinity: same -- store the IEEE inf pattern with the sign */
      mantissa = (n < 0.0f) ? 0xFF800000UL : 0x7F800000UL;
    } else {
      mantissa = ((unsigned long) (0x01000000UL * frexp(fabs(n), &e))) 
        & 0x007fffffUL;
      mantissa = mantissa | ((e + 0x7eUL) << 23);
      if (n < 0) {
        mantissa = mantissa | 0x80000000;
      }
    }
    ptr->data[ofs]   = (mantissa      ) & 0xff;
    ptr->data[ofs+1] = (mantissa >>  8) & 0xff;
    ptr->data[ofs+2] = (mantissa >> 16) & 0xff;
    ptr->data[ofs+3] = (mantissa >> 24);
  }
}

void variable_dump(const variable_t var, FILE *out)
{
  const char *fmt;

#if 0
  fprintf(out, "{%lx:", (unsigned long) var);
#else
  fprintf(out, "{");
#endif
  if (VARIABLE_DEF_TYPE_POINTER == variable_type_get(var)) {
    fputc('*', out);
  }
  if (variable_name_get(var)) {
    fmt = (variable_tag_n_get(var)) ? "__%s_%u" : "%s";
    fprintf(out, fmt, variable_name_get(var), variable_tag_n_get(var));
  } else {
    variable_t master;

    master = variable_master_get(var);
    if (variable_name_get(master)) {
      fmt = (variable_tag_n_get(var)) ? "__%s_%u" : "%s";
      fprintf(out, fmt, variable_name_get(master), 
          variable_tag_n_get(master));
    }
  }
  fprintf(out, "[%c%c%c%c%s%u]", 
      variable_def_type_to_ch(variable_type_get(var)), 
      variable_dflag_test(var, VARIABLE_DEF_FLAG_CONST) ? 'C' : '-',
      variable_dflag_test(var, VARIABLE_DEF_FLAG_VOLATILE) ? 'V' : '-',
      variable_dflag_test(var, VARIABLE_DEF_FLAG_SIGNED) ? 'S' : '-',
    variable_dflag_test(var, VARIABLE_DEF_FLAG_BIT) ? ":" : "",
    variable_sz_get(var));
  if (variable_is_const(var)) {
    char cbuf[50];

    variable_const_str_get(variable_const_get(var, variable_def_get(var), 0),
        variable_dflag_test(var, VARIABLE_DEF_FLAG_SIGNED), cbuf,
        sizeof(cbuf));
    fputs(cbuf, out);
  }
  fputc('}', out);
}

refct_t variable_refct_get(const variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr) ? ptr->ref_ct : 0;
}

variable_t variable_link_get(const variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr) ? ptr->link : 0;
}

void variable_link_set(variable_t var, variable_t link)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    /* Comprehensive circular reference prevention */
    if (var == link) {
      /* Direct self-reference - definitely circular */
      return;
    }
    
    /* Check for circular chains by following links */
    {
      variable_t current = link;
      int depth = 0;
      while (current && current != VARIABLE_NONE && depth < 10) {
        if (current == var) {
          /* Circular reference detected - don't create the link */
          return;
        }
        current = variable_link_get(current);
        depth++;
      }
      
      /* If chain is too long, assume it might be circular */
      if (depth >= 10) {
        return;
      }
    }
    
    ptr->link = link;
  }
}

ctr_t variable_assign_ct_get(const variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr) ? ptr->ctr_assign : 0;
}

void variable_assign_ct_set(variable_t var, ctr_t ctr)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    ptr->ctr_assign = ctr;
  }
}

void variable_assign_ct_bump(variable_t var, ctr_bump_t dir)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    switch (dir) {
      case CTR_BUMP_INCR: 
        ptr->ctr_assign++; 
        break;
      case CTR_BUMP_DECR: 
        if (ptr->ctr_assign) {
          ptr->ctr_assign--; 
        } else {
          /*fprintf(stderr, "%s: assign is 0\n", variable_name_get(var));*/
        }
        break;
    }
    if (ptr->master) {
      variable_assign_ct_bump(ptr->master, dir);
    }
  }
}

ctr_t variable_use_ct_get(const variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr) ? ptr->ctr_use : 0;
}

void variable_use_ct_set(variable_t var, ctr_t ctr)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    ptr->ctr_use = ctr;
  }
}

void variable_use_ct_bump(variable_t var, ctr_bump_t dir)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    switch (dir) {
      case CTR_BUMP_INCR: 
        ptr->ctr_use++; 
        break;
      case CTR_BUMP_DECR: 
        if (ptr->ctr_use) {
          ptr->ctr_use--; 
        } else {
          /*fprintf(stderr, "%s:%u use is 0\n", variable_name_get(var),
              variable_tag_n_get(var));*/
        }
        break;
    }
    if (ptr->master) {
      variable_use_ct_bump(ptr->master, dir);
    }
  }
}

ctr_t variable_ref_ct_get(const variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr) ? ptr->ref_ct : 0;
}

void variable_list_init(varlist_t *lst)
{
  lst->head = 0;
  lst->tail = 0;
  lst->ct   = 0;
}

void variable_list_append(varlist_t *lst, variable_t var)
{
  variable_lock(var);

  if (lst->tail) {
    variable_link_set(lst->tail, var);
  } else {
    lst->head = var;
  }
  lst->tail = var;
  lst->ct++;
}

void variable_list_remove(varlist_t *lst, variable_t victim)
{
  variable_t cur;
  variable_t prev;

  if (!lst || !victim) {
    return;
  }
  prev = 0;
  for (cur = lst->head; cur; cur = variable_link_get(cur)) {
    if (cur == victim) {
      if (prev) {
        variable_link_set(prev, variable_link_get(cur));
      } else {
        lst->head = variable_link_get(cur);
      }
      if (lst->tail == victim) {
        lst->tail = prev;
      }
      if (lst->ct) {
        lst->ct--;
      }
      variable_link_set(victim, 0);
      variable_release(victim);
      return;
    }
    prev = cur;
  }
}

variable_t variable_list_find(varlist_t *lst, varfind_cb_t cb, void *cb_arg,
    const void *data)
{
  variable_t var;

  for (var = lst->head;
       var && !cb(cb_arg, var, data);
       var = variable_link_get(var))
    ;
  if (var) {
    variable_lock(var);
  }
  return var;
}

#if 0
/* nb : with the introduction of master variables (aka, aliases)
        it's necessary to run through the list *twice* to find
        if there's a leak */
static void variable_leak_test(variable_t var, pfile_t *pf)
{
  if (variable_refct_get(var) > 1) {
    if (variable_name_get(var)) {
      pfile_log(pf, PFILE_LOG_DEBUG, PFILE_MSG_DBG_VAR_LEAK, 
          var, variable_refct_get(var), variable_name_get(var));
    } else {
      pfile_log(pf, PFILE_LOG_DEBUG, PFILE_MSG_DBG_CONST_LEAK, 
          var, variable_refct_get(var), 
          variable_const_get(var, variable_def_get(var), 0));
    }
  }
}
#endif

void variable_list_reset(varlist_t *lst)
{
  variable_t var;

  /* first, set all of the masters to 0 */
  for (var = lst->head;
       var;
       var = variable_link_get(var)) {
    variable_master_set(var, VARIABLE_NONE);
  }
  while (lst->head) {
    var = lst->head;
    lst->head = variable_link_get(var);
    /*variable_leak_test(var, pf);*/
    variable_release(var);
  }
  lst->tail = 0;
  lst->ct = 0;
}

size_t variable_list_ct_get(const varlist_t *lst)
{
  return (lst) ? lst->ct : 0;
}

variable_t variable_list_head(varlist_t *lst)
{
  return lst->head;
}

variable_def_t variable_def_get(const variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr) ? ptr->def : 0;
}

void variable_def_set(variable_t var, variable_def_t def)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    ptr->def = def;
  }
}

/* given a constant, n, determine the smallest variable size
   that will hold it */
void variable_calc_sz_min(variable_const_t n, 
  variable_sz_t *psz, variable_def_type_t *ptype,
  flag_t *pflags)
{
  unsigned char       bytes[sizeof(variable_const_t)];
  size_t              ix;
  variable_sz_t       sz;
  variable_def_type_t type;
  flag_t              flags;

  for (ix = 0; ix < sizeof(bytes); ix++) {
    bytes[ix] = variable_const_byte_get(n, (unsigned) ix);
  }
  sz = (variable_sz_t) sizeof(bytes);
  type  = VARIABLE_DEF_TYPE_INTEGER;
  flags = VARIABLE_DEF_FLAG_NONE;
  while ((sz > 1) && (0 == bytes[sz - 1])) {
    sz--;
  }
  if ((sizeof(bytes) == sz) && (bytes[sz - 1] & 0x80)) {
    flags |= VARIABLE_DEF_FLAG_SIGNED;
    while ((sz > 1) && (0xff == bytes[sz - 1])
        && (bytes[sz - 2] & 0x80)) {
      sz--;
    }
  }
  if (psz) {
    *psz = sz;
  }
  if (ptype) {
    *ptype = type;
  }
  if (pflags) {
    *pflags = flags;
  }
}

void variable_origin_set(variable_t var, const void *origin)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    ptr->origin = origin;
  }
}

const void *variable_origin_get(const variable_t var)
{
  struct variable_ *ptr;
  const void      *origin;

  ptr    = variable_element_seek(var, BOOLEAN_TRUE);
  origin = (ptr) ? ptr->origin : 0;
  return origin;
}

void variable_bit_offset_set(variable_t var, ushort bit)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    ptr->bit_offset = bit;
  }
}

unsigned variable_bit_offset_get(const variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr) ? ptr->bit_offset : 0;
}

variable_t variable_master_get(const variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr) ? ptr->master : 0;
}

const char *variable_pointer_rvalue_link_name_get(const variable_t ptr_var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(ptr_var, BOOLEAN_FALSE);
  return (ptr) ? ptr->ptr_rvalue_link_name : 0;
}

void variable_pointer_rvalue_link_name_set(variable_t ptr_var,
    const char *link_name)
{
  struct variable_ *ptr;
  char              *copy;

  ptr = variable_element_seek(ptr_var, BOOLEAN_TRUE);
  if (!ptr) {
    return;
  }
  if (ptr->ptr_rvalue_link_name) {
    FREE(ptr->ptr_rvalue_link_name);
    ptr->ptr_rvalue_link_name = 0;
  }
  if (link_name && *link_name) {
    copy = STRDUP(link_name);
    if (copy) {
      ptr->ptr_rvalue_link_name = copy;
    }
  }
}

value_t variable_pointer_whereis_target_get(const variable_t ptr_var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(ptr_var, BOOLEAN_FALSE);
  return (ptr) ? ptr->ptr_whereis_target : VALUE_NONE;
}

void variable_pointer_whereis_target_set(variable_t ptr_var, value_t target)
{
  struct variable_ *ptr;
  value_t           copy;

  copy = VALUE_NONE;
  if (target) {
    copy = value_clone(target);
  }
  ptr = variable_element_seek(ptr_var, BOOLEAN_TRUE);
  if (ptr) {
    value_release(ptr->ptr_whereis_target);
    ptr->ptr_whereis_target = copy;
  } else {
    value_release(copy);
  }
}

static variable_pointer_whereis_slot_t *variable_pointer_whereis_slot_find(
    array_t *slots, variable_const_t slot_ofs)
{
  size_t ii;

  if (!slots) {
    return 0;
  }
  for (ii = 0; ii < array_ct(slots); ii++) {
    variable_pointer_whereis_slot_t *slot;

    slot = array_get(slots, ii);
    if (slot && slot->slot_ofs == slot_ofs) {
      return slot;
    }
  }
  return 0;
}

value_t variable_pointer_whereis_slot_target_get(const variable_t ptr_var,
    variable_const_t slot_ofs)
{
  struct variable_                 *ptr;
  variable_pointer_whereis_slot_t  *slot;

  ptr = variable_element_seek(ptr_var, BOOLEAN_FALSE);
  if (!ptr) {
    return VALUE_NONE;
  }
  slot = variable_pointer_whereis_slot_find(ptr->ptr_whereis_slots, slot_ofs);
  return slot ? slot->target : VALUE_NONE;
}

void variable_pointer_whereis_slot_target_set(variable_t ptr_var,
    variable_const_t slot_ofs, value_t target)
{
  struct variable_                *ptr;
  variable_pointer_whereis_slot_t *slot;
  value_t                          copy;

  copy = VALUE_NONE;
  if (target) {
    copy = value_clone(target);
  }
  ptr = variable_element_seek(ptr_var, BOOLEAN_TRUE);
  if (!ptr) {
    value_release(copy);
    return;
  }
  if (!ptr->ptr_whereis_slots) {
    ptr->ptr_whereis_slots = array_alloc(0,
        sizeof(variable_pointer_whereis_slot_t));
  }
  slot = variable_pointer_whereis_slot_find(ptr->ptr_whereis_slots, slot_ofs);
  if (!slot) {
    variable_pointer_whereis_slot_t new_slot;

    new_slot.slot_ofs = slot_ofs;
    new_slot.target = VALUE_NONE;
    slot = array_append(ptr->ptr_whereis_slots, &new_slot);
  }
  if (slot) {
    value_release(slot->target);
    slot->target = copy;
  } else {
    value_release(copy);
  }
}

/* must-bind tracking: a pointer counts as BOUND only when its slot was
 * genuinely written -- `p is x` binds, slot copies (`q = p`, `p = arr`)
 * -- never by store-through accesses (`p = 42` bumps ctr_assign via
 * cmd_opdst_set and must not silence the check) */
void variable_pointer_bound_ct_bump(const variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    ptr->ctr_ptr_bound++;
  }
}

boolean_t variable_pointer_bound_p(const variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr && ptr->ctr_ptr_bound) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

void variable_pointer_slot_use_note(const variable_t var,
    variable_const_t slot_ofs)
{
  struct variable_ *ptr;
  size_t            ii;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (!ptr) {
    return;
  }
  if (!ptr->ptr_slot_uses) {
    ptr->ptr_slot_uses = array_alloc(0, sizeof(variable_const_t));
  }
  if (ptr->ptr_slot_uses) {
    for (ii = 0; ii < array_ct(ptr->ptr_slot_uses); ii++) {
      variable_const_t *used;

      used = array_get(ptr->ptr_slot_uses, ii);
      if (used && (*used == slot_ofs)) {
        return;
      }
    }
    array_append(ptr->ptr_slot_uses, &slot_ofs);
  }
}

size_t variable_pointer_slot_use_ct_get(const variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr && ptr->ptr_slot_uses) ? array_ct(ptr->ptr_slot_uses) : 0;
}

variable_const_t variable_pointer_slot_use_get(const variable_t var,
    size_t index)
{
  struct variable_   *ptr;
  variable_const_t   *ofs;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  ofs = (ptr && ptr->ptr_slot_uses && (index < array_ct(ptr->ptr_slot_uses)))
      ? array_get(ptr->ptr_slot_uses, index) : 0;
  return (ofs) ? *ofs : 0;
}

void variable_pointer_slot_rt_bind_ct_bump(const variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    ptr->ctr_ptr_slot_rt_bind++;
  }
}

void variable_pointer_slot_link_name_set(const variable_t var,
    variable_const_t slot_ofs, const char *link_name)
{
  struct slot_link_entry {
    variable_const_t ofs;
    char            *name;
  };
  struct variable_    *ptr;
  struct slot_link_entry *ent;
  size_t               ii;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (!ptr || !link_name) {
    return;
  }
  if (!ptr->ptr_slot_links) {
    ptr->ptr_slot_links = array_alloc(0, sizeof(struct slot_link_entry));
  }
  if (ptr->ptr_slot_links) {
    for (ii = 0; ii < array_ct(ptr->ptr_slot_links); ii++) {
      ent = array_get(ptr->ptr_slot_links, ii);
      if (ent && (ent->ofs == slot_ofs)) {
        if (ent->name) {
          FREE(ent->name);
        }
        ent->name = STRDUP(link_name);
        return;
      }
    }
    {
      struct slot_link_entry neu;

      neu.ofs  = slot_ofs;
      neu.name = STRDUP(link_name);
      array_append(ptr->ptr_slot_links, &neu);
    }
  }
}

const char *variable_pointer_slot_link_name_get(const variable_t var,
    variable_const_t slot_ofs)
{
  struct slot_link_entry {
    variable_const_t ofs;
    char            *name;
  };
  struct variable_    *ptr;
  struct slot_link_entry *ent;
  size_t               ii;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  if (!ptr || !ptr->ptr_slot_links) {
    return 0;
  }
  for (ii = 0; ii < array_ct(ptr->ptr_slot_links); ii++) {
    ent = array_get(ptr->ptr_slot_links, ii);
    if (ent && (ent->ofs == slot_ofs)) {
      return ent->name;
    }
  }
  return 0;
}

variable_t variable_pointer_slot_kind_var_get(const variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr) ? ptr->ptr_slot_kind_var : VARIABLE_NONE;
}

void variable_pointer_slot_kind_var_set(const variable_t var,
    variable_t helper)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    ptr->ptr_slot_kind_var = helper;
  }
}

variable_t variable_pointer_slot_getcall_var_get(const variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr) ? ptr->ptr_slot_getcall_var : VARIABLE_NONE;
}

void variable_pointer_slot_getcall_var_set(const variable_t var,
    variable_t helper)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    ptr->ptr_slot_getcall_var = helper;
  }
}

variable_t variable_pointer_slot_putcall_var_get(const variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr) ? ptr->ptr_slot_putcall_var : VARIABLE_NONE;
}

void variable_pointer_slot_putcall_var_set(const variable_t var,
    variable_t helper)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    ptr->ptr_slot_putcall_var = helper;
  }
}

variable_t variable_pointer_slot_puts_var_get(const variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr) ? ptr->ptr_slot_puts_var : VARIABLE_NONE;
}

void variable_pointer_slot_puts_var_set(const variable_t var,
    variable_t helper)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    ptr->ptr_slot_puts_var = helper;
  }
}

boolean_t variable_pointer_slot_links_any(const variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr && ptr->ptr_slot_links
      && array_ct(ptr->ptr_slot_links)) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

ctr_t variable_pointer_slot_rt_bind_ct_get(const variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr) ? ptr->ctr_ptr_slot_rt_bind : 0;
}

static void variable_master_counters_adjust(variable_t var,
    ctr_t ctr_assign, ctr_t ctr_use)
{
  while (var) {
    struct variable_ *ptr;

    ptr = variable_element_seek(var, BOOLEAN_TRUE);
    ptr->ctr_assign += ctr_assign;
    ptr->ctr_use    += ctr_use;
    var = variable_master_get(var);
  }
}

void variable_master_set(variable_t var, variable_t master)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    ctr_t ctr_assign;
    ctr_t ctr_use;

    ctr_assign = variable_assign_ct_get(var);
    ctr_use    = variable_use_ct_get(var);

    variable_master_counters_adjust(
        variable_master_get(var), -ctr_assign, -ctr_use);
    /* now, remove the master */
    variable_lock(master);
    ptr = variable_element_seek(var, BOOLEAN_TRUE);
    variable_release(ptr->master);
    ptr = variable_element_seek(var, BOOLEAN_TRUE);
    ptr->master = master;
    if (master && (VARIABLE_BASE_UNKNOWN == ptr->base[0])) {
      ptr->base[0] = 0;
    }
    variable_master_counters_adjust(master, ctr_assign, ctr_use);
    if (master) {
      variable_flag_set(master, VARIABLE_FLAG_OVERLAY_HOST);
    }
  }
}

variable_def_type_t variable_type_get(const variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr) ? variable_def_type_get(ptr->def) : VARIABLE_DEF_TYPE_NONE;
}

pfile_proc_t *variable_proc_get(const variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr) ? ptr->proc : PFILE_PROC_NONE;
}

void variable_proc_set(variable_t var, pfile_proc_t *proc)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    ptr->proc = proc;
  }
}

value_t variable_value_get(const variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr) ? ptr->val : VALUE_NONE;
}

void variable_value_set(variable_t var, value_t val)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    value_lock(val);
    value_release(ptr->val);
    ptr->val = val;
  }
}

label_t variable_label_get(const variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr) ? ptr->lbl : LABEL_NONE;
}

void variable_label_set(variable_t var, label_t lbl)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    label_lock(lbl);
    label_usage_bump(ptr->lbl, CTR_BUMP_DECR);
    label_release(ptr->lbl);
    label_usage_bump(lbl, CTR_BUMP_INCR);
    ptr->lbl = lbl;
  }
}

boolean_t variable_is_float(const variable_t var)
{
  return VARIABLE_DEF_TYPE_FLOAT == variable_type_get(var);
}

boolean_t variable_is_function(const variable_t var)
{
  return VARIABLE_DEF_TYPE_FUNCTION == variable_type_get(var);
}

boolean_t variable_is_bit(const variable_t var)
{
  return variable_dflag_test(var, VARIABLE_DEF_FLAG_BIT);
}

boolean_t variable_is_pointer(const variable_t var)
{
  return VARIABLE_DEF_TYPE_POINTER == variable_type_get(var);
}

boolean_t variable_is_volatile(const variable_t var)
{
  return variable_dflag_test(var, VARIABLE_DEF_FLAG_VOLATILE);
}

boolean_t variable_is_signed(const variable_t var)
{
  return variable_dflag_test(var, VARIABLE_DEF_FLAG_SIGNED);
}

boolean_t variable_is_alias(const variable_t var)
{
  return variable_flag_test(var, VARIABLE_FLAG_ALIAS);
}

boolean_t variable_is_auto(const variable_t var)
{
  return variable_flag_test(var, VARIABLE_FLAG_AUTO);
}

boolean_t variable_is_sticky(const variable_t var)
{
  return variable_flag_test(var, VARIABLE_FLAG_STICKY);
}

boolean_t variable_is_lookup(const variable_t var)
{
  return variable_flag_test(var, VARIABLE_FLAG_LOOKUP);
}

boolean_t variable_is_const(const variable_t var)
{
  return variable_dflag_test(var, VARIABLE_DEF_FLAG_CONST);
}

/* declared CONST and never assigned.  Const-folding
 * (pfile_block_variable_fixup) marks assigned-but-never-read variables
 * CONST, so the flag alone does not prove the cmd stream is free of
 * write references -- back ends may only pool/fold a const when the
 * assignment count is still zero. */
boolean_t variable_is_pure_const(const variable_t var)
{
  return variable_is_const(var) && (0 == variable_assign_ct_get(var));
}

boolean_t variable_is_pseudo_const(const variable_t var)
{
  boolean_t rc;

  if (!var) {
    return BOOLEAN_FALSE;
  }
  rc = variable_is_const(var);
  if (!rc) {
    if (variable_master_get(var)) {
      rc = variable_is_pseudo_const(variable_master_get(var));
    } else {
      rc = !(variable_is_volatile(var) || variable_assign_ct_get(var));
    }
  }
  return rc;
}

boolean_t variable_is_array(const variable_t var)
{
  return VARIABLE_DEF_TYPE_ARRAY == variable_type_get(var);
}

/* a variable is assigned if it's volatile and used,
 * or if any of it's masters are assigned */
boolean_t variable_is_assigned(const variable_t var)
{
  boolean_t  rc;
  variable_t tvar;

  for (tvar = var, rc = BOOLEAN_FALSE;
       tvar && !rc;
       tvar = variable_master_get(tvar)) {
    rc = 0 != variable_assign_ct_get(tvar);
	/* RJ: When volatile and referenced (ref count > 1) it should not be optimized away. 
           This fixes compiler issue #1. */
	if (!rc && variable_is_volatile(tvar)) {
		rc = 1 != variable_ref_ct_get(tvar);
    }
  }
  return rc;
}

/* a variable is used if it's volatile and assigned,
 * or if any of it's masters are used */
boolean_t variable_is_used(const variable_t var)
{
  boolean_t rc;
  variable_t tvar;

  for (tvar = var, rc = BOOLEAN_FALSE;
       tvar && !rc;
       tvar = variable_master_get(tvar)) {
    rc = 0 != variable_use_ct_get(tvar);
    if (!rc && variable_is_volatile(tvar)) {
      rc = 0 != variable_assign_ct_get(tvar);
    }
  }
  return rc;
}

void variable_data_set(variable_t var, void *x)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    if (ptr->data != x) {
      FREE(ptr->data);
    }
    ptr->data = x;
  }
}

void *variable_data_get(const variable_t var)
{
  const struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr) ? ptr->data : 0;
}

variable_t variable_map_find(const variable_map_t *map, variable_t var)
{
  variable_t ret;

  if (!var) {
    ret = var;
  } else {
    size_t ii;

    for (ii = 0; (ii < map->used) && (map->map[ii].old != var); ii++)
      ;
    ret = (ii < map->used) ? map->map[ii].new : VARIABLE_NONE;
  }
#if 0
  if (ret) {
    printf("variable remap %s:%u --> %s:%u\n",
        variable_name_get(var), variable_tag_n_get(var),
        variable_name_get(ret), variable_tag_n_get(ret));
  }
#endif
  return ret;
}

flag_t variable_flags_get_all(variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr) ? ptr->flags : 0;
}

void variable_flags_set_all(variable_t var, flag_t flags)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    ptr->flags = flags;
  }
}

void  variable_user_data_set(variable_t var, void *data)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    ptr->user_data = data;
  }
}

void *variable_user_data_get(const variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr) ? ptr->user_data : 0;
}

static variable_ref_t *variable_ref_find(array_t *array, variable_t var)
{
  size_t          ii;
  variable_ref_t *fnd;

  fnd = VARIABLE_REF_NONE;
  for (ii = 0; ii < array_ct(array); ii++) {
    variable_ref_t *ref;

    ref = array_get(array, ii);
    if (ref->var == var) {
      fnd = ref;
      break;
    }
  }
  return fnd;
}

static void variable_ref_add(array_t *array, variable_t var)
{
  variable_ref_t *fnd;

  fnd = variable_ref_find(array, var);
  if (fnd) {
    fnd->ref_ct++;
  } else {
    variable_ref_t new_ref;

    new_ref.var    = var;
    new_ref.ref_ct = 1;
    (void) array_append(array, &new_ref);
  }
}

static void variable_ref_remove(array_t *array, variable_t var)
{
  variable_ref_t *fnd;

  fnd = variable_ref_find(array, var);
  if (fnd) {
    assert(fnd->ref_ct > 0);
    if (fnd->ref_ct > 0) {
      fnd->ref_ct--;
    }
  }
}

static void variable_used_by_add(variable_t var, variable_t uses)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    variable_ref_add(ptr->used_by, uses);
  }
}

static void variable_used_by_remove(variable_t var, variable_t uses)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  if (ptr) {
    variable_ref_remove(ptr->used_by, uses);
  }
}

variable_t variable_used_by_get(variable_t var, size_t ix)
{
  struct variable_ *ptr;
  variable_ref_t   *ref;

  ptr = variable_element_seek(var, BOOLEAN_TRUE);
  ref = VARIABLE_REF_NONE;
  if (ptr) {
    ref = array_get(ptr->used_by, ix);
  }
  return (ref) ? ref->var : VARIABLE_NONE;
}

void variable_uses_add(variable_t var, variable_t uses)
{
  if ((VARIABLE_NONE != var) && (VARIABLE_NONE != uses)) {
    struct variable_ *ptr;

    ptr = variable_element_seek(var, BOOLEAN_TRUE);
    if (ptr) {
      variable_ref_add(ptr->uses, uses);
      variable_used_by_add(uses, var);
    }
  }
}

void variable_uses_remove(variable_t var, variable_t uses)
{
  if ((VARIABLE_NONE != var) && (VARIABLE_NONE != uses)) {
    struct variable_ *ptr;

    ptr = variable_element_seek(var, BOOLEAN_TRUE);
    if (ptr) {
      variable_ref_remove(ptr->uses, uses);
      variable_used_by_remove(uses, var);
    }
  }
}

variable_t variable_uses_get(variable_t var, size_t ix)
{
  struct variable_ *ptr;
  variable_ref_t   *ref;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  ref = VARIABLE_REF_NONE;
  if (ptr) {
    ref = array_get(ptr->uses, ix);
  }
  return (ref) ? ref->var : VARIABLE_NONE;
}

variable_t variable_interfer_get(variable_t var, size_t ix)
{
  struct variable_ *ptr;
  variable_ref_t   *ref;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  ref = VARIABLE_REF_NONE;
  if (ptr) {
    ref = array_get(ptr->interference, ix);
  }
  return (ref) ? ref->var : VARIABLE_NONE;
}

int variable_unique_cmp(void *arg, const void *A, const void *B)
{
  variable_t a;
  variable_t b;

  UNUSED(arg);

  a = *(variable_t const *) A;
  b = *(variable_t const *) B;

  return (a > b) - (b > a);
}

void variable_interference_add(variable_t var1, variable_t var2)
{
  while (variable_master_get(var1)) {
    var1 = variable_master_get(var1);
  }

  while (variable_master_get(var2)) {
    var2 = variable_master_get(var2);
  }

  if (/* variable_is_auto(var1) 
    && variable_is_auto(var2) */
    (VARIABLE_BASE_UNKNOWN == variable_base_get(var1, 0))
    && (VARIABLE_BASE_UNKNOWN == variable_base_get(var2, 0))) {
    struct variable_ *ptr;

    ptr = variable_element_seek(var1, BOOLEAN_TRUE);
    if (ptr) {
      variable_array_entry_add(ptr->interference, &var2,
        ARRAY_ADD_FLAG_NONE, variable_unique_cmp, 0);
    }

    ptr = variable_element_seek(var2, BOOLEAN_TRUE);
    if (ptr) {
      variable_array_entry_add(ptr->interference, &var1,
        ARRAY_ADD_FLAG_NONE, variable_unique_cmp, 0);
    }
  }
}

variable_array_t *variable_interference_get(variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr) ? ptr->interference : VARIABLE_ARRAY_NONE;
}

unsigned variable_id_get(variable_t var)
{
  struct variable_ *ptr;

  ptr = variable_element_seek(var, BOOLEAN_FALSE);
  return (ptr) ? ptr->id : 0;
}

