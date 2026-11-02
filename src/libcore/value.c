/**********************************************************
 **
 ** value.c : manipulators for value_t
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 **           (c) 2024-2024, Rob Jansen
** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: fork extensions (heavy rework)
 **
 ***********************************************************/
#include <string.h>
#include <assert.h>
#include "pf_proc.h"
#include "pf_log.h"
#include "value.h"
#include <stdlib.h>
#include <stdio.h>
/* execinfo (backtrace) is glibc/macOS only -- mingw has no such header, so
 * the Windows cross-build failed on it.  The symbolic half of the death
 * certificate is a convenience; the diagnostic itself ("value used after
 * release: id N") does not depend on it.  JAL_VALUE_HAVE_BACKTRACE keeps
 * the two halves in step instead of guarding each site separately. */
#if defined(JALV2_VALUE_DEBUG) && !defined(_WIN32)
#include <execinfo.h>
#define JAL_VALUE_HAVE_BACKTRACE 1
#endif

static unsigned value_id;

struct value_
{
  refct_t         ref_ct;
  unsigned        id;
  variable_t      var;       /* base variable                */
  value_t         baseofs;   /* offset from base of variable */
  variable_def_t  def;       /* the definition               */
  flag_t          flags;
  unsigned        bitofs_delta;
  const char     *name;
#ifdef JALV2_VALUE_DEBUG
  void           *dbg_bt[6];        /* release call stack (death certificate) */
  int             dbg_bt_ct;
#endif
};


/*
 * Compile-scoped region for value_t.
 *
 * Values used to be refcounted objects with manual release, with 1090
 * release sites and no compiler help: a forgotten release was silent and
 * accumulated (measured: 105 leaked per compile, linear across compiles).
 * A value is dead when its compile ends, so lifetime is the region's:
 * allocate by bumping a chunk, free the chunks at pfile_close.
 *
 * What is NOT deferred is the VARIABLE release. A value locks its variable
 * when it is created, and variable slots are reused as those references
 * drop; deferring that release to teardown changed the data placement (and
 * so the HEX) of 34 of 2937 corpus samples. value_release() therefore still
 * runs its cleanup at refcount zero -- only the element free is gone.
 */
#define VALUE_CHUNK_CT 8192

typedef struct value_chunk_ {
  struct value_chunk_ *next;
  size_t               used;
  struct value_        vals[VALUE_CHUNK_CT];
} value_chunk_t;

static value_chunk_t *value_chunks;
static value_chunk_t *value_cur;
static unsigned long  value_allocated;

unsigned long value_live_ct(void)
{
  return value_allocated;
}

void value_arena_begin(void)
{
  /* A region still standing here means the previous compile never reached
   * pfile_close (a fatal exit). Nothing can reference it any more, so it is
   * reclaimed rather than leaked; the assert flags the missing close in
   * debug builds, where that path is reachable by a failed compile. */
  assert(0 == value_chunks);
  if (value_chunks) {
    (void) value_arena_destroy();
  }
}

static value_t value_element_alloc(void)
{
  if ((0 == value_cur) || (VALUE_CHUNK_CT == value_cur->used)) {
    /* zeroed: the dead-value check reads a value's flags through
     * value_element_seek during value_alloc, BEFORE value_alloc has
     * initialised them -- garbage here used to trip the detector on a
     * fresh value. calloc costs one pass per 8192 values. */
    value_chunk_t *chunk = (value_chunk_t *) calloc(1, sizeof(*chunk));

    if (0 == chunk) {
      return 0;
    }
    chunk->next   = value_chunks;
    chunk->used   = 0;
    value_chunks  = chunk;
    value_cur     = chunk;
  }
  value_allocated++;
  return &value_cur->vals[value_cur->used++];
}

static struct value_ *value_element_seek(value_t el, boolean_t mod)
{
  (void) mod;                 /* elements are never moved or recycled */

  /* The region keeps dead values readable, so a use after release would
   * otherwise read plausible stale data. This is the single chokepoint --
   * struct value_ is opaque outside this file and every accessor lands
   * here -- so the check is ALWAYS on: one load of a word the caller is
   * about to dereference anyway. A stale handle must never ship silently.
   *
   * -DJALV2_VALUE_DEBUG (the SAN flavor) additionally records the release
   * stack, so the report names the code that released it. */
  if (el && (el->flags & VALUE_FLAG_DEAD)) {
    fprintf(stderr, "!!! value used after release: id %u\n", el->id);
#if defined(JALV2_VALUE_DEBUG) && defined(JAL_VALUE_HAVE_BACKTRACE)
    {
      char **sym = backtrace_symbols(el->dbg_bt, el->dbg_bt_ct);

      if (sym) {
        int i;

        fprintf(stderr, "    released from:\n");
        for (i = 0; i < el->dbg_bt_ct; i++) {
          fprintf(stderr, "    %s\n", sym[i]);
        }
        free(sym);
      }
    }
#endif
    abort();
  }
  return el;
}

unsigned long value_arena_destroy(void)
{
  value_chunk_t *chunk;
  unsigned long  freed;

  freed = value_allocated;
  chunk = value_chunks;
  while (chunk) {
    value_chunk_t *next = chunk->next;

    free(chunk);
    chunk = next;
  }
  value_chunks    = 0;
  value_cur       = 0;
  value_allocated = 0;

  return freed;
}

void value_variable_set(value_t val, variable_t var)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_TRUE);
  if (ptr && (ptr->var != var)) {
    if (ptr->var) {
      variable_release(ptr->var);
    }
    if (var) {
      variable_lock(var);
    }
    ptr = value_element_seek(val, BOOLEAN_TRUE);
    ptr->var   = var;
    ptr->def   = variable_def_get(var);
    ptr->name  = variable_name_get(var);
  }
}

value_t value_alloc(variable_t var)
{
  value_t        val;
  struct value_ *ptr;

  val = value_element_alloc();
  if (!val) {
    /* A chunk allocation failure is OOM, and the caller cannot be trusted
     * to notice a NULL value (the `at { ... }` parser reported "constant
     * expression expected" and carried on at 7 MB).  The policy is fatal
     * on the FIRST memory failure, so stop here rather than return NULL. */
    pfile_oom_report(0);
    return 0;
  }
  ptr = value_element_seek(val, BOOLEAN_TRUE);
  ptr->ref_ct    = 1;
  ptr->id        = ++value_id;
  ptr->var       = VARIABLE_NONE;
  ptr->baseofs   = VALUE_NONE;
  ptr->def       = VARIABLE_DEF_NONE;
  ptr->flags     = VALUE_FLAG_NONE;
  ptr->bitofs_delta = 0;
  ptr->name      = 0;

  value_variable_set(val, var);
  return val;
}

void value_def_set(value_t val, variable_def_t def)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_TRUE);
  if (ptr) {
    ptr->def = def;
  }
}

void value_indirect_set(value_t val)
{
  value_flag_set(val, VALUE_FLAG_INDIRECT);
}

void value_indirect_clear(value_t val)
{
  value_flag_clr(val, VALUE_FLAG_INDIRECT);
}

value_t value_clone(value_t src)
{
  variable_t      var;
  value_t         baseofs;
  variable_def_t  def;
  value_t         val;
  flag_t          flags;
  unsigned        bitofs_delta;

  var     = value_variable_get(src);
  baseofs = value_baseofs_get(src);
  def     = value_def_get(src);
  flags   = value_flag_get_all(src);
  bitofs_delta = value_bit_offset_get(src)
    - variable_bit_offset_get(value_variable_get(src));

  val = value_alloc(var);
  if (val) {
    value_baseofs_set(val, baseofs);
    value_def_set(val, def);
    value_flag_set_all(val, flags);
    value_bit_offset_delta_set(val, bitofs_delta);
  }
  return val;
}

void value_lock(value_t val)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_TRUE);
  if (ptr) {
    ptr->ref_ct++;
  }
}

refct_t value_ref_ct_get(const value_t val)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_FALSE);
  return (ptr) ? ptr->ref_ct : 0;
}

unsigned value_id_get(const value_t val)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_FALSE);
  return (ptr) ? ptr->id : 0;
}

void value_release(value_t val)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_TRUE);
  if (val && (val->flags & VALUE_FLAG_DEAD)) {
    fprintf(stderr, "!!! value released twice: id %u\n", val->id);
    abort();
  }
  if (ptr && !--ptr->ref_ct) {
    /* the memory belongs to the compile's region; the variable reference
     * does not, and must be dropped here (see the note above) */
    value_baseofs_set(val, 0);
    value_variable_set(val, 0);
    ptr = value_element_seek(val, BOOLEAN_TRUE);
    if (ptr) {
      ptr->flags |= VALUE_FLAG_DEAD;
#ifdef JALV2_VALUE_DEBUG
      ptr->dbg_bt_ct = 0;
#ifdef JAL_VALUE_HAVE_BACKTRACE
      ptr->dbg_bt_ct = backtrace(ptr->dbg_bt, 6);
#endif
#endif
    }
  }
}

variable_sz_t value_sz_get(const value_t val)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_FALSE);
  return (ptr) ? variable_def_sz_get(ptr->def) : 0;
}

variable_sz_t value_byte_sz_get(const value_t val)
{
  variable_sz_t sz;

  sz = value_sz_get(val);
  if (value_is_bit(val)) {
    sz = (sz + 7) / 8;
  } else if (value_is_universal(val)) {
    variable_const_t cn;
    uchar            n0, n1, n2, n3;

    cn = value_const_get(val);
    n0 = (uchar)  (cn        & 0xff);
    n1 = (uchar) ((cn >>  8) & 0xff);
    n2 = (uchar) ((cn >> 16) & 0xff);
    n3 = (uchar) ((cn >> 24) & 0xff);

    sz = 4;
    if (value_is_signed(val) && (n3 & 0x80)) {
      if ((0xff == n3) && (n2 & 0x80)) {
        sz--;
        if ((0xff == n2) && (n1 & 0x80)) {
          sz--;
          if ((0xff == n1) && (n0 & 0x80)) {
            sz--;
          }
        }
      }
    } else {
      if (0x00 == n3) {
        sz--;
        if (0x00 == n2) {
          sz--;
          if (0x00 == n1) {
            sz--;
          }
        }
      }
    }
  }
  return sz;
}

boolean_t value_dflag_test(const value_t val, flag_t flag)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_FALSE);
  return (ptr) ? variable_def_flag_test(ptr->def, flag) : 0;
}

boolean_t value_vflag_test(const value_t val, flag_t flags)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_FALSE);
  return (ptr) ? variable_flag_test(ptr->var, flags) : 0;
}

void value_vflag_set(value_t val, flag_t flags)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_TRUE);
  if (ptr) {
    variable_flag_set(ptr->var, flags);
  }
}

void value_vflag_clr(value_t val, flag_t flags)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_TRUE);
  if (ptr) {
    variable_flag_clr(ptr->var, flags);
  }
}

flag_t value_flag_get_all(value_t val)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_FALSE);
  return (ptr) ? ptr->flags : 0;
}

void value_flag_set_all(value_t val, flag_t flags)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_TRUE);
  if (ptr) {
    ptr->flags = flags;
  }
}


const char *value_name_get(const value_t val)
{
  return variable_name_get(value_variable_get(val));
}

variable_const_t value_const_get(const value_t val)
{
  variable_const_t n;
  struct value_   *ptr;

  ptr = value_element_seek(val, BOOLEAN_FALSE);
  if (!ptr) {
    n = 0;
  } else {
    variable_const_t pos;
    if (ptr->baseofs && !value_is_const(ptr->baseofs)) {
      /* Lookup / indexed const with runtime subscript: no single compile-time
       * offset (GitHub jalv2compiler #40). */
      return 0;
    }
    pos = value_const_get(ptr->baseofs);
    ptr = value_element_seek(val, BOOLEAN_FALSE);
    n = variable_const_get(ptr->var, value_def_get(val), pos);
  }
  return n;
}

/* Some codegen paths need a constant base offset even after the parser has
 * represented it through a generated value instead of a direct const. */
boolean_t value_baseofs_const_get(const value_t val, variable_const_t *dst)
{
  struct value_ *ptr;

  if (dst) {
    *dst = 0;
  }
  ptr = value_element_seek(val, BOOLEAN_FALSE);
  if (!dst || !ptr) {
    return BOOLEAN_FALSE;
  }
  if (!ptr->baseofs) {
    return BOOLEAN_TRUE;
  }
  if (value_is_const(ptr->baseofs)) {
    *dst = value_const_get(ptr->baseofs);
    return BOOLEAN_TRUE;
  }
  {
    value_t          baseofs;
    variable_const_t pos;
    variable_t       var;

    baseofs = ptr->baseofs;
    if (!value_baseofs_const_get(baseofs, &pos)) {
      return BOOLEAN_FALSE;
    }
    var = value_variable_get(baseofs);
    if (!var) {
      return BOOLEAN_FALSE;
    }
    if (variable_is_const(var) && variable_data_get(var)) {
      *dst = variable_const_get(var, value_def_get(baseofs), pos);
    } else if (!variable_assign_ct_get(var)) {
      *dst = value_const_get(baseofs);
    } else {
      return BOOLEAN_FALSE;
    }
  }
  return BOOLEAN_TRUE;
}

void value_const_set(value_t val, variable_const_t c)
{
  struct value_   *ptr;

  ptr = value_element_seek(val, BOOLEAN_FALSE);
  if (ptr) {
    variable_const_t pos;
    if (ptr->baseofs && !value_is_const(ptr->baseofs)) {
      return;
    }
    pos = value_const_get(ptr->baseofs);
    ptr = value_element_seek(val, BOOLEAN_FALSE);
    variable_const_set(ptr->var, value_def_get(val), pos, c);
  }
}

float value_const_float_get(const value_t val)
{
  float            n;
  struct value_   *ptr;

  ptr = value_element_seek(val, BOOLEAN_FALSE);
  if (!ptr) {
    n = 0.0;
  } else {
    variable_const_t pos;
    if (ptr->baseofs && !value_is_const(ptr->baseofs)) {
      return 0.0f;
    }
    pos = value_const_get(ptr->baseofs);
    ptr = value_element_seek(val, BOOLEAN_FALSE);
    n = variable_const_float_get(ptr->var, pos);
  }
  return n;
}

void value_const_float_set(value_t val, float c)
{
  struct value_   *ptr;

  ptr = value_element_seek(val, BOOLEAN_FALSE);
  if (ptr) {
    variable_const_t pos;
    if (ptr->baseofs && !value_is_const(ptr->baseofs)) {
      return;
    }
    pos = value_const_get(ptr->baseofs);
    ptr = value_element_seek(val, BOOLEAN_FALSE);
    variable_const_float_set(ptr->var, pos, c);
  }
}

void value_dump(const value_t val, FILE *dst)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_FALSE);
  if (ptr) {
    fprintf(dst, "%u[%c%c%c%c%c%c%c%u]:", value_id_get(val),
        variable_def_type_to_ch(value_type_get(val)),
        value_dflag_test(val, VARIABLE_DEF_FLAG_CONST) ? 'C' : '-',
        value_dflag_test(val, VARIABLE_DEF_FLAG_VOLATILE) ? 'V' : '-',
        value_dflag_test(val, VARIABLE_DEF_FLAG_SIGNED) ? 'S' : '-',
        value_flag_test(val, VALUE_FLAG_INDIRECT) ? 'I' : '-',
        value_flag_test(val, VALUE_FLAG_ARRAY)    ? 'A' : '-',
        value_flag_test(val, VALUE_FLAG_POINTER)  ? 'P' : '-',
          value_sz_get(val));
    variable_dump(ptr->var, dst);
    if (ptr->baseofs) {
      value_t baseofs;

      baseofs = ptr->baseofs;
      if (!value_name_get(baseofs)) {
        char cbuf[50];

        variable_const_str_get(value_const_get(baseofs), BOOLEAN_FALSE,
          cbuf, sizeof(cbuf));
        fprintf(dst, "+%s", cbuf);
      } else {
        char cbuf[50];

        variable_const_str_get(value_const_get(baseofs), BOOLEAN_FALSE,
          cbuf, sizeof(cbuf));
        fprintf(dst, "+%s(%s)", value_name_get(baseofs), cbuf);
      }
    }
  }
}

/* two values are the same iff they point to the same variable
 * and have the same offset and subscript */
boolean_t value_is_same(const value_t val1, const value_t val2)
{
  boolean_t rc;

  rc = (val1 == val2)
    || (value_is_const(val1)
      && value_is_const(val2)
      && value_def_is_same(val1, val2)
      && (value_const_get(val1) == value_const_get(val2)));
  if (!rc) {
    const char *name1;
    const char *name2;

    name1 = value_name_get(val1);
    name2 = value_name_get(val2);

    if ((name1 && !strcmp(name1, "_temp")) 
      || (name2 && !strcmp(name2, "_temp"))) {
      rc = BOOLEAN_FALSE;
    } else {
      rc = (value_variable_get(val1) == value_variable_get(val2))
        && (value_def_is_same(val1, val2))
        && (value_bit_offset_get(val1) == value_bit_offset_get(val2));

      if (rc) {
        value_t base1;
        value_t base2;

        base1 = value_baseofs_get(val1);
        base2 = value_baseofs_get(val2);

        if (base1 == base2) {
          /* still true */
        } else if (value_is_const(base1) 
          && value_is_const(base2)
          && (value_const_get(base1) == value_const_get(base2))) {
          /* sitll true */
        } else {
          rc = BOOLEAN_FALSE;
        }
      }
    }
  }
  return rc;
}

value_t value_baseofs_get(const value_t val)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_FALSE);
  return (ptr) ? ptr->baseofs : 0;
}

boolean_t value_baseofs_depends(const value_t val, const value_t dep)
{
  value_t  ofs;
  unsigned depth;

  /* a subscripted value (e.g. a heap-array element) is addressed at run
   * time through its baseofs chain: any temp in that chain is read by
   * the instruction that uses the value, even though it is not a direct
   * cmd operand. Depth cap guards against pathological nesting. */
  for (ofs = val ? value_baseofs_get(val) : 0, depth = 0;
      ofs && (depth < 4);
      ofs = value_baseofs_get(ofs), depth++) {
    if (ofs == dep) {
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

void value_baseofs_set(value_t val, value_t baseofs)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_TRUE);
  if (ptr && (ptr->baseofs != baseofs)) {
    if (baseofs) {
      value_lock(baseofs);
    }
    ptr = value_element_seek(val, BOOLEAN_TRUE);
    if (ptr->baseofs) {
      value_release(ptr->baseofs);
    }
    ptr = value_element_seek(val, BOOLEAN_TRUE);
    ptr->baseofs = baseofs;
  }
}

value_t value_constant_get(variable_const_t n, variable_def_t def)
{
  value_t             val;
  variable_t          var;

  if (!def) {
    def = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER, 
        VARIABLE_DEF_FLAG_UNIVERSAL | VARIABLE_DEF_FLAG_CONST, 16);
  } else if (!variable_def_flag_test(def, VARIABLE_DEF_FLAG_CONST)) {
    def = variable_def_flags_change(def,
        variable_def_flags_get_all(def) | VARIABLE_DEF_FLAG_CONST);
  }
  var = variable_alloc(0, def);
  if (!var) {
    val = VALUE_NONE;
  } else {
    variable_const_set(var, variable_def_get(var), 0, n);
    val = value_alloc(var);
    variable_release(var);
  }
  return val;
}

value_t value_constant_float_get(float n, variable_def_t def)
{
  value_t    val;
  variable_t var;

  if (!def) {
    def = variable_def_alloc(0, VARIABLE_DEF_TYPE_FLOAT, 
        VARIABLE_DEF_FLAG_CONST | VARIABLE_DEF_FLAG_SIGNED, 4);
  } else if (!variable_def_flag_test(def, VARIABLE_DEF_FLAG_CONST)) {
    def = variable_def_flags_change(def,
        variable_def_flags_get_all(def) | VARIABLE_DEF_FLAG_CONST);
  }
  var = variable_alloc(0, def);
  if (!var) {
    val = VALUE_NONE;
  } else {
    variable_const_float_set(var, 0, n);
    val = value_alloc(var);
    variable_release(var);
  }
  return val;
}

value_t value_subscript_set(value_t val, variable_ct_t subscript)
{
  value_t rval;

  if (value_is_array(val) && (subscript < value_ct_get(val))) {
    rval = value_clone(val);
    if (rval) {
      variable_def_t def;
      
      def = variable_def_member_def_get(
          variable_def_member_get(
            value_def_get(
              rval)));
      {
        /* same shape as pic_opfn: value_baseofs_set() locks its argument,
         * so the reference value_constant_get() handed us must be dropped */
        value_t kofs = value_constant_get(
            subscript * variable_def_sz_get(def), VARIABLE_DEF_NONE);

        value_baseofs_set(rval, kofs);
        value_release(kofs);
      }
      value_dereference(rval);
    }
  } else {
    rval = VALUE_NONE;
  }
  return rval;
}

variable_t value_variable_get(const value_t val)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_FALSE);
  return (ptr) ? ptr->var : 0;
}

ctr_t value_assign_ct_get(const value_t val)
{
  return variable_assign_ct_get(value_variable_get(val));
}

void value_assign_ct_bump(value_t val, ctr_bump_t dir)
{
  variable_assign_ct_bump(value_variable_get(val), dir);
  value_use_ct_bump(value_baseofs_get(val), dir);
}

ctr_t value_use_ct_get(const value_t val)
{
  return variable_use_ct_get(value_variable_get(val));
}

void  value_use_ct_bump(value_t val, ctr_bump_t dir)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_TRUE);
  if (ptr) {
    variable_use_ct_bump(ptr->var, dir);
    value_use_ct_bump(ptr->baseofs, dir);
  }
}

/* the underlying variable might have become const which wouldn't be
 * reflected in the value definition so it must also be checked
 * (this is the case of a variable being used without ever being
 * assigned)
 */
boolean_t value_is_const(const value_t val)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_FALSE);
  if (!ptr) {
    return BOOLEAN_FALSE;
  }
  /* A pointer variable holds a runtime address.  CONST on the pointee
   * (`var pointer p as const byte[]`; `p[i]`) must not make
   * value_const_get return 0 from an empty payload.  Direct
   * `const byte tbl[i]` stays ARRAY and still folds. */
  if (ptr->var && variable_is_pointer(ptr->var)) {
    return BOOLEAN_FALSE;
  }
  return !variable_label_get(ptr->var)
    && !variable_value_get(ptr->var)
    && ((value_dflag_test(val, VARIABLE_DEF_FLAG_CONST) 
          || variable_dflag_test(ptr->var, VARIABLE_DEF_FLAG_CONST))
        && (!ptr->baseofs || value_is_const(ptr->baseofs)));
}

boolean_t value_is_pseudo_const(const value_t val)
{
  variable_t v;

  if (!val) {
    return BOOLEAN_FALSE;
  }
  v = value_variable_get(val);
  if (!v) {
    /* Unary cmds (e.g. POINTER_ASSIGN) leave val2 unset; NULL must not read
     * as pseudo-const or pfile_cexpr_reduction rewrites the op to ASSIGN. */
    return BOOLEAN_FALSE;
  }
  return variable_is_pseudo_const(v)
      && (!value_baseofs_get(val) || value_is_const(value_baseofs_get(val)));
}

boolean_t value_is_signed(const value_t val)
{
  return value_dflag_test(val, VARIABLE_DEF_FLAG_SIGNED);
}

boolean_t value_is_lookup(const value_t val)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_FALSE);
  return ptr
    && ptr->baseofs
    && (variable_is_const(value_variable_get(val))) 
    && !value_is_const(ptr->baseofs);
}

variable_base_t value_base_get(const value_t val)
{
  return variable_base_get(value_variable_get(val), 0);
}

variable_def_t value_def_get(const value_t val)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_FALSE);
  return (ptr) ? ptr->def : 0;
} 

boolean_t value_def_is_same(const value_t val1, const value_t val2)
{
  variable_def_t var1, var2;
  var1 = value_def_get(val1);
  var2 = value_def_get(val2);
  return variable_def_is_same(var1, var2);
}

boolean_t value_is_record(const value_t val)
{
  variable_def_t def;

  def = value_def_get(val);
  return (def && variable_def_member_get(def));
}

boolean_t value_is_function(const value_t val)
{
  return VARIABLE_DEF_TYPE_FUNCTION == value_type_get(val);
}

boolean_t value_is_label(const value_t val)
{
  return VARIABLE_DEF_TYPE_LABEL == value_type_get(val);
}

boolean_t value_is_value(const value_t val)
{
  return VARIABLE_DEF_TYPE_VALUE == value_type_get(val);
}

boolean_t value_is_indirect(const value_t val)
{
  boolean_t rc;

  if (value_is_lookup(val)) {
    rc = BOOLEAN_FALSE;
  } else {
    value_t baseofs;

    baseofs = value_baseofs_get(val);
    rc = value_flag_test(val, VALUE_FLAG_INDIRECT)
      || (baseofs && !value_is_const(baseofs));
  }
  return rc;
}

boolean_t value_is_volatile(const value_t val)
{
  return variable_is_volatile(value_variable_get(val));
}

boolean_t value_is_auto(const value_t val)
{
  return variable_is_auto(value_variable_get(val));
}

unsigned value_tag_n_get(const value_t val)
{
  return variable_tag_n_get(value_variable_get(val));
}

unsigned value_bit_offset_get(const value_t val)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_FALSE);
  return (ptr)
    ? variable_bit_offset_get(ptr->var) + ptr->bitofs_delta
    : 0;
}

void value_bit_offset_delta_set(value_t val, unsigned delta)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_TRUE);
  if (ptr) {
    ptr->bitofs_delta = delta;
  }
}

pfile_proc_t *value_proc_get(const value_t val)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_FALSE);
  return (ptr) ? variable_proc_get(ptr->var) : PFILE_PROC_NONE;
}

label_t value_label_get(const value_t val)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_FALSE);
  return (ptr) ? variable_label_get(ptr->var) : LABEL_NONE;
}

variable_def_type_t value_type_get(const value_t val)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_FALSE);
  return (ptr) ? variable_def_type_get(ptr->def) : VARIABLE_DEF_TYPE_NONE;
}

boolean_t value_is_number(const value_t val)
{
  return variable_def_type_is_number(value_type_get(val));
}

boolean_t value_is_bit(const value_t val)
{
  return value_dflag_test(val, VARIABLE_DEF_FLAG_BIT);
}

boolean_t value_is_array(const value_t val)
{
  return VARIABLE_DEF_TYPE_ARRAY == value_type_get(val);
}

boolean_t value_is_pointer(const value_t val)
{
  return VARIABLE_DEF_TYPE_POINTER == value_type_get(val);
}

boolean_t value_is_shared(const value_t val)
{
  return variable_is_shared(value_variable_get(val));
}

boolean_t value_is_float(const value_t val)
{
  return VARIABLE_DEF_TYPE_FLOAT == value_type_get(val);
}

boolean_t value_is_structure(const value_t val)
{
  return VARIABLE_DEF_TYPE_STRUCTURE == value_type_get(val);
}

boolean_t value_is_boolean(const value_t val)
{
  return VARIABLE_DEF_TYPE_BOOLEAN == value_type_get(val);
}

void value_dereference(value_t val)
{
  variable_def_t        def;
  variable_def_member_t mbr;
  variable_t            var;

  assert((VARIABLE_DEF_TYPE_POINTER == value_type_get(val))
      || (VARIABLE_DEF_TYPE_ARRAY == value_type_get(val)));
      
  /* Mark the variable as used when dereferencing (array[index] or pointer access) */
  var = value_variable_get(val);
  if (var) {
    variable_use_ct_bump(var, CTR_BUMP_INCR);
  }
      
  value_flag_set(val, 
      (value_is_pointer(val)) ? VALUE_FLAG_POINTER : VALUE_FLAG_ARRAY);
  def = value_def_get(val);
  mbr = variable_def_member_get(def);
  value_def_set(val, variable_def_member_def_get(mbr));
  if (value_flag_test(val, VALUE_FLAG_POINTER)) {
    value_indirect_set(val);
  }
}

boolean_t value_is_single_bit(const value_t val)
{
  return value_is_bit(val) && (1 == value_sz_get(val));
}

boolean_t value_is_multi_bit(const value_t val)
{
  return value_is_bit(val) && (1 < value_sz_get(val));
}

variable_ct_t value_ct_get(const value_t val)
{
  variable_ct_t  ct;

  if ((VARIABLE_DEF_TYPE_POINTER != value_type_get(val))
    && (VARIABLE_DEF_TYPE_ARRAY != value_type_get(val))) {
    ct = 1;
  } else {
    ct = variable_def_member_ct_get(
        variable_def_member_get(
          value_def_get(val)));
  }
  return ct;
}

boolean_t value_flag_test(const value_t val, flag_t flags)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_FALSE);
  return (ptr) ? (ptr->flags & flags) == flags : 0; /* BOOLEAN_FALSE; */
}

void value_flag_set(value_t val, flag_t flags)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_TRUE);
  if (ptr) {
    ptr->flags |= flags;
  }
}

void value_flag_clr(value_t val, flag_t flags)
{
  struct value_ *ptr;

  ptr = value_element_seek(val, BOOLEAN_TRUE);
  if (ptr) {
    ptr->flags &= ~flags;
  }
}

boolean_t value_is_assigned(const value_t val)
{
  return variable_is_assigned(value_variable_get(val));
}

boolean_t value_is_used(const value_t val)
{
  return variable_is_used(value_variable_get(val));
}

boolean_t value_is_universal(const value_t val)
{
  return value_dflag_test(val, VARIABLE_DEF_FLAG_UNIVERSAL);
}

boolean_t value_is_string(const value_t val)
{
  return value_dflag_test(val, VARIABLE_DEF_FLAG_STRING);
}

value_t value_map_find(const value_map_t *map, value_t val)
{
  value_t ret;

  if (!val) {
    ret = val;
  } else {
    size_t ii;

    for (ii = 0; (ii < map->used) && (map->map[ii].old != val); ii++)
      ;
    ret = (ii < map->used) ? map->map[ii].new : VALUE_NONE;
  }

  return ret;
}

value_t value_variable_remap(value_t val, const variable_map_t *map)
{
  value_t tmp;

  tmp = VALUE_NONE;
  if (val) {
    variable_t var;
    value_t    baseofs;

    baseofs = value_variable_remap(value_baseofs_get(val), map);
    if (baseofs) {
      /* create a new value for baseofs */
      tmp = value_clone(val);
      value_baseofs_set(tmp, baseofs);
      value_release(baseofs);
    }
    var = variable_map_find(map, value_variable_get(val));
    if (var) {
      boolean_t fix_indirect;
      variable_def_t def;

      fix_indirect = (value_is_indirect(val) && !variable_is_pointer(var));

      if (!tmp) {
        tmp = value_clone(val);
      }
      /* the def *might be* different from that var's def, so
       * save it & reset it here (the case of temporary variables)
       * or de-referenced arrays */
      def = value_def_get(tmp);
      value_variable_set(tmp, var);
      value_def_set(tmp, def);
      if (variable_is_array(var)) {
        value_flag_set(tmp, VALUE_FLAG_ARRAY);
      }

      if (fix_indirect) {
        value_flag_clr(tmp, VALUE_FLAG_INDIRECT);
        value_flag_clr(tmp, VALUE_FLAG_POINTER);
      }
    }
  }
  return tmp;
}

value_t value_remap(value_t val, const value_map_t *map)
{
  UNUSED(val);
  UNUSED(map);

  assert(0);
  return VALUE_NONE;
}

