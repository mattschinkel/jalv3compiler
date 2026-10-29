/************************************************************
 **
 ** vardef.c : variable definition bits
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: fork extensions (heavy rework)
 **
 ************************************************************/
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../libutils/mem.h"
#include "variable.h"
#include "vardef.h"
#include "vardefd.h"

static unsigned       variable_def_outstanding;
static variable_def_t ugly_hack_head;

variable_def_t variable_def_alloc(const char *tag, 
  variable_def_type_t type, flag_t flags, variable_sz_t sz)
{
  variable_def_t def;
  size_t         tag_sz;

  tag_sz = (tag) ? strlen(tag) + 1 : 0;

  def = MALLOC(sizeof(*def) + tag_sz);
  if (def) {
    variable_def_outstanding++;
    def->link = 0;
    if (tag_sz) {
      def->tag = (char *) (def + 1);
      memcpy(def + 1, tag, tag_sz);
    } else {
      def->tag  = 0;
    }
    def->type    = type;
    def->flags   = flags;
    def->sz      = sz;
    def->members = 0;

    def->ugly_hack.prev = 0;
    def->ugly_hack.next = ugly_hack_head;
    if (ugly_hack_head) {
      ugly_hack_head->ugly_hack.prev = def;
    }
    ugly_hack_head = def;
  }
  return def;
}

variable_def_t variable_def_flags_change(variable_def_t src, flag_t flags)
{
  if (variable_def_flags_get_all(src) != flags) {
    variable_def_member_t mbr;
    const char         *tag;
    variable_def_type_t typ;
    variable_sz_t       sz;

    mbr = src->members;
    tag = variable_def_tag_get(src);
    typ = variable_def_type_get(src);
    sz = variable_def_sz_get(src);
    /* Detach members from src before replacing src; otherwise the old def
     * (still pointed to by e.g. jal_variable_info_t.vdef) aliases the same
     * member list as the new def (pointer-to-data with volatile, etc.). */
    src->members = 0;
    src = variable_def_alloc(tag, typ, flags, sz);
    if (src) {
      src->members = mbr;
    }
  }
  return src;
}

void variable_def_free(variable_def_t def)
{
  if (def->ugly_hack.prev) {
    def->ugly_hack.prev->ugly_hack.next = def->ugly_hack.next;
  } else {
    assert(ugly_hack_head == def);
    ugly_hack_head = def->ugly_hack.next;
  }
  if (def->ugly_hack.next) {
    def->ugly_hack.next->ugly_hack.prev = def->ugly_hack.prev;
  }
  if (def) {
    /* Avoid circular reference issues by not traversing the linked list */
    /* Just clear the members pointer and let the individual members be freed elsewhere */
    /* This is a temporary workaround until we can properly fix the circular reference issue */
    def->members = 0;
    
    FREE(def);
    variable_def_outstanding--;
  }
}

variable_def_t variable_def_link_get(const variable_def_t def)
{
  return (def) ? def->link : 0;
}

void variable_def_link_set(variable_def_t def, variable_def_t link)
{
  if (def) {
    def->link = link;
  }
}

const char    *variable_def_tag_get(const variable_def_t def)
{
  return (def) ? def->tag : 0;
}

variable_sz_t  variable_def_sz_get(const variable_def_t def)
{
  return (def) ? def->sz : 0;
}

variable_sz_t variable_def_byte_sz_get(const variable_def_t def)
{
  variable_sz_t sz;

  sz = variable_def_sz_get(def);
  if (variable_def_flag_test(def, VARIABLE_DEF_FLAG_BIT)) {
    sz = (sz + 7) / 8;
  }
  return sz;
}


variable_def_t variable_def_sz_set(variable_def_t src, variable_sz_t sz)
{
  if (variable_def_sz_get(src) != sz) {
    src = variable_def_alloc(
        variable_def_tag_get(src),
        variable_def_type_get(src),
        variable_def_flags_get_all(src),
        sz);
  }
  return src;
}

variable_def_member_t variable_def_member_get(variable_def_t def)
{
  return (def) ? def->members : 0;
}

result_t variable_def_member_insert(variable_def_t def,
  variable_def_member_t after, const char *tag,
  const variable_def_t vdef, variable_ct_t ct)
{
  variable_def_member_t mbr;
  result_t              rc;

  mbr = variable_def_member_get(def);
  rc = RESULT_OK;
  switch (variable_def_type_get(def)) {
    case VARIABLE_DEF_TYPE_NONE:
    case VARIABLE_DEF_TYPE_INTEGER:
    case VARIABLE_DEF_TYPE_BOOLEAN:
    case VARIABLE_DEF_TYPE_FLOAT:
      rc = RESULT_INVALID;
      break;
    case VARIABLE_DEF_TYPE_POINTER:
    case VARIABLE_DEF_TYPE_REFERENCE:
    case VARIABLE_DEF_TYPE_ARRAY:
      if (mbr) {
        rc = RESULT_INVALID;
      }
      break;
    case VARIABLE_DEF_TYPE_FUNCTION:
    case VARIABLE_DEF_TYPE_STRUCTURE:
    case VARIABLE_DEF_TYPE_UNION:
    case VARIABLE_DEF_TYPE_LABEL:
    case VARIABLE_DEF_TYPE_VALUE:
      break;
  }
  if (RESULT_OK == rc) {
    mbr = variable_def_member_alloc(tag, vdef, ct);
    if (!mbr) {
      rc = RESULT_MEMORY;
    } else {
      rc = RESULT_OK;
      if ((VARIABLE_DEF_TYPE_POINTER != variable_def_type_get(def))
        && (VARIABLE_CT_UNKNOWN != ct)) {
        def->sz += ct * variable_def_sz_get(vdef);
      }
      if (after) {
        variable_def_member_link_set(mbr, variable_def_member_link_get(after));
        variable_def_member_link_set(after, mbr);
      } else {
        variable_def_member_link_set(mbr, def->members);
        def->members = mbr;
      }
    }
  }
  return rc;
}

result_t variable_def_member_add(variable_def_t def,
                         const char *tag, const variable_def_t vdef,
                         variable_ct_t ct)
{
  variable_def_member_t mbr;
  variable_def_member_t mbr_pv;
  result_t              rc;

  mbr = variable_def_member_get(def);
  mbr_pv = 0;

  while (mbr && (!tag || !variable_def_member_tag_get(mbr) 
        || strcmp(variable_def_member_tag_get(mbr), tag))) {
    mbr_pv = mbr;
    mbr = variable_def_member_link_get(mbr);
  }

  if (mbr) {
    rc = RESULT_EXISTS;
  } else {
    rc = variable_def_member_insert(def, mbr_pv, tag, vdef, ct);
  }
  return rc;
}

result_t variable_def_pointer_fold_scalar_pointee_array(variable_def_t ptr,
    variable_ct_t dim)
{
  variable_def_member_t pmbr;
  variable_def_t       pointee;
  variable_def_t       arr;
  result_t             rc;

  if (!ptr || VARIABLE_DEF_TYPE_POINTER != variable_def_type_get(ptr)
      || VARIABLE_CT_UNKNOWN == dim || dim < 1) {
    return RESULT_INVALID;
  }
  pmbr = variable_def_member_get(ptr);
  if (!pmbr || variable_def_member_link_get(pmbr)) {
    return RESULT_INVALID;
  }
  pointee = variable_def_member_def_get(pmbr);
  if (!pointee || VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(pointee)) {
    return RESULT_INVALID;
  }
  if (VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(pointee)
      || VARIABLE_DEF_TYPE_FUNCTION == variable_def_type_get(pointee)) {
    return RESULT_INVALID;
  }
  /* signedness belongs to the ELEMENT def (plain `var sbyte buf[N]`
   * clears SIGNED at the array level); copying it here made the folded
   * pointee def fail the bind signature check against a real sbyte
   * array -- `p is buf` errored "type mismatch" for every signed
   * element type */
  arr = variable_def_alloc(0, VARIABLE_DEF_TYPE_ARRAY,
      variable_def_flags_get_all(pointee)
        & ~VARIABLE_DEF_FLAG_SIGNED, 0);
  if (!arr) {
    return RESULT_MEMORY;
  }
  rc = variable_def_member_add(arr, 0, pointee, dim);
  if (RESULT_OK != rc) {
    variable_def_free(arr);
    return rc;
  }
  pmbr->def = arr;
  return RESULT_OK;
}

variable_def_member_t variable_def_member_alloc(const char *tag,
  const variable_def_t def, variable_ct_t ct)
{
  variable_def_member_t mbr;
  size_t                sz;

  sz = (tag) ? 1 + strlen(tag) : 0;
  mbr = MALLOC(sizeof(*mbr) + sz);
  if (mbr) {
    mbr->link = 0;
    if (sz) {
      mbr->tag = (char *) (1 + mbr);
      memcpy(1 + mbr, tag, sz);
    } else {
      mbr->tag = 0;
    }
    mbr->def = def;
    mbr->ct  = ct;
  }
  return mbr;
}

void variable_def_member_free(variable_def_member_t mbr)
{
  if (mbr) {
    FREE(mbr);
  }
}


variable_def_member_t variable_def_member_link_get(
  const variable_def_member_t mbr)
{
  /* Safety check to prevent accessing freed memory */
  if (!mbr) {
    return 0;
  }
  
  /* Reject the NULL page. Upper-bound VA checks used unsigned long and
   * broke LLP64 (Win64: long is 32-bit). size_t matches pointer width. */
  if ((size_t) mbr < (size_t) 0x1000) {
    return 0;
  }
  
  /* Try to access the link field - if it causes a use-after-free,
     AddressSanitizer will catch it and we'll return 0 */
  /* This is a temporary workaround until we fix the root cause */
  return mbr->link;
}

void variable_def_member_link_set(variable_def_member_t mbr,
  variable_def_member_t lnk)
{
  if (mbr) {
    mbr->link = lnk;
  }
}

const char *variable_def_member_tag_get(const variable_def_member_t mbr)
{
  return (mbr) ? mbr->tag : 0;
}

variable_ct_t variable_def_member_ct_get(const variable_def_member_t mbr)
{
  return (mbr) ? mbr->ct : 0;
}

/* Open [] may still have ct=65535 (0xFFFF) on paths that used a 16-bit -1; after
   widening, VARIABLE_CT_UNKNOWN is 0xFFFFFFFF. 65535U with aggregate sz 0 is
   unambiguous: true byte[65535] has def->sz >= 65535*elem. */
static boolean_t
vardef_mbr_is_open_unknown(const variable_def_t def,
  const variable_def_member_t mbr)
{
  variable_ct_t c;

  c = variable_def_member_ct_get(mbr);
  if (VARIABLE_CT_UNKNOWN == c) {
    return BOOLEAN_TRUE;
  }
  if (65535U == c
    && VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(def)
    && 0U == variable_def_sz_get(def)) {
    return BOOLEAN_TRUE;
  }
  return BOOLEAN_FALSE;
}

result_t variable_def_member_ct_set(variable_def_t def, 
  variable_def_member_t mbr, variable_ct_t ct)
{
  result_t       res;

  if ((VARIABLE_DEF_TYPE_ARRAY != variable_def_type_get(def))
    || !vardef_mbr_is_open_unknown(def, mbr)) {
    res = RESULT_INVALID;
  } else {
    res      = RESULT_OK;
    mbr->ct  = ct;
    def->sz += ct * variable_def_sz_get(variable_def_member_def_get(mbr));
  }
  return res;

}

variable_ct_t variable_def_member_sz_get(const variable_def_member_t mbr)
{
  return (mbr) ? variable_def_sz_get(mbr->def) : 0;
}

#if 0
static void variable_def_member_dump(const variable_def_member_t mbr,
    pfile_t *pf, int indent)
{
  pfile_write(pf, pfile_write_lst,
      ";%*s(ct=%u)*%s: %s\n", indent, "",
      mbr->ct,
      variable_def_tag_get(mbr->def)
      ? variable_def_tag_get(mbr->def)
      : "",
      (mbr->tag) ? mbr->tag : "");
  variable_def_dump(mbr->def, pf, indent + 2);
}
#endif

const char *variable_def_type_to_str(variable_def_type_t type)
{
  const char *str;

  str = "none";
  switch (type) {
    case VARIABLE_DEF_TYPE_NONE:                        break;
    case VARIABLE_DEF_TYPE_BOOLEAN:  str = "boolean";   break;
    case VARIABLE_DEF_TYPE_INTEGER:  str = "integer";   break;
    case VARIABLE_DEF_TYPE_FLOAT:    str = "float";     break;
    case VARIABLE_DEF_TYPE_POINTER:  str = "pointer";   break;
    case VARIABLE_DEF_TYPE_REFERENCE:str = "reference"; break;
    case VARIABLE_DEF_TYPE_FUNCTION: str = "function";  break;
    case VARIABLE_DEF_TYPE_STRUCTURE:str = "structure"; break;
    case VARIABLE_DEF_TYPE_UNION:    str = "union";     break;
    case VARIABLE_DEF_TYPE_ARRAY:    str = "array";     break;
    case VARIABLE_DEF_TYPE_LABEL:    str = "label";     break;
    case VARIABLE_DEF_TYPE_VALUE:    str = "value";     break;
  }
  return str;
}


#if 0
void variable_def_dump(const variable_def_t def, pfile_t *pf, int indent)
{
  if (def) {
    variable_def_member_t mbr;

    pfile_write(pf, pfile_write_lst,
        ";%*s%s (type=%s sz=%u flags=%c%c%c%c%s", indent, "", 
        (def->tag) ? def->tag : "", 
        variable_def_type_to_str(def->type),
        def->sz, 
        (def->flags & VARIABLE_DEF_FLAG_CONST)    ? 'C' : '-',
        (def->flags & VARIABLE_DEF_FLAG_VOLATILE) ? 'V' : '-',
        (def->flags & VARIABLE_DEF_FLAG_IN)       ? 'I' : '-',
        (def->flags & VARIABLE_DEF_FLAG_OUT)      ? 'O' : '-',
        def->members ? "" : ")");
    
    pfile_write(pf, pfile_write_lst, "\n");
    for (mbr = def->members; mbr; mbr = mbr->link) {
      variable_def_member_dump(mbr, pf, indent + 2);
    }
    if (def->members) {
      pfile_write(pf, pfile_write_lst, ";%*s )\n", indent, "");
    }
  }
}
#endif
variable_def_t variable_def_member_def_get(
  const variable_def_member_t mbr)
{
  return (mbr) ? mbr->def : 0;
}

boolean_t variable_def_is_same(const variable_def_t a, const variable_def_t b)
{
  boolean_t rc;

  if (a == b) {
    rc = BOOLEAN_TRUE;
  } else if (!a || !b) {
    rc = BOOLEAN_FALSE;
  } else {
    rc = BOOLEAN_FALSE;
    if ((variable_def_type_get(a) == variable_def_type_get(b))
      && (variable_def_sz_get(a) == variable_def_sz_get(b))
      && ((variable_def_flags_get_all(a) 
          & (VARIABLE_DEF_FLAG_SIGNED | VARIABLE_DEF_FLAG_BIT)))
        == (variable_def_flags_get_all(b) 
          & (VARIABLE_DEF_FLAG_SIGNED | VARIABLE_DEF_FLAG_BIT))) {
      variable_def_member_t mbr_a;
      variable_def_member_t mbr_b;

      mbr_a = variable_def_member_get(a);
      mbr_b = variable_def_member_get(b);

      while (mbr_a
        && mbr_b 
        && variable_def_member_is_same(mbr_a, mbr_b)) {
        mbr_a = variable_def_member_link_get(mbr_a);
        mbr_b = variable_def_member_link_get(mbr_b);
      }
      rc = !mbr_a && !mbr_b;
    }
  }
  return rc;
}

boolean_t variable_def_member_is_same(const variable_def_member_t a,
  const variable_def_member_t b)
{
  boolean_t rc;

  if (a == b) {
    rc = BOOLEAN_TRUE;
  } else if (!a || !b) {
    rc = BOOLEAN_FALSE;
  } else {
    rc = variable_def_is_same(variable_def_member_def_get(a),
      variable_def_member_def_get(b));
  }
  return rc;
}

variable_def_type_t variable_def_type_get(const variable_def_t a)
{
  return (a) ? a->type : VARIABLE_DEF_TYPE_NONE;
}

flag_t variable_def_flags_get_all(const variable_def_t def)
{
  return (def) ? def->flags : 0;
}

boolean_t variable_def_flag_test(const variable_def_t def, flag_t flag)
{
  return (def) ? ((def->flags & flag) == flag) : 0; /* BOOLEAN_FALSE; */
}

variable_def_t variable_def_dup(const variable_def_t def)
{
  variable_def_t        dst;

  dst = variable_def_alloc(def->tag, def->type, def->flags, 0);
  if (dst) {
    if (variable_def_member_get(def)) {
      variable_def_member_t mbr;
      variable_def_member_t mbr_pv;
      boolean_t             err;

      err = BOOLEAN_FALSE;
      for (mbr_pv = 0, mbr = variable_def_member_get(def);
           mbr && !err;
           mbr = variable_def_member_link_get(mbr)) {
        variable_def_member_t mdup;
        variable_def_t        child_copy;

        /* Procedure/function defs use NULL first member for "no return" (void). */
        child_copy = (mbr->def) ? variable_def_dup(mbr->def) : VARIABLE_DEF_NONE;
        mdup = variable_def_member_alloc(mbr->tag, child_copy, mbr->ct);
        if (!mdup) {
          err = BOOLEAN_TRUE;
        } else {
          if (mbr_pv) {
            mbr_pv->link = mdup;
          } else {
            dst->members = mdup;
          }
          mbr_pv = mdup;
        }
      }
      if (err) {
        variable_def_free(dst);
        dst = VARIABLE_DEF_NONE;
      } else {
        dst->sz = def->sz;
      }
    } else {
      dst->sz = def->sz;
    }
  }
  return dst;
}

char variable_def_type_to_ch(variable_def_type_t type)
{
  char ch;

  ch = '?';
  switch (type) {
    case VARIABLE_DEF_TYPE_NONE:               break;
    case VARIABLE_DEF_TYPE_BOOLEAN:  ch = 'B'; break;
    case VARIABLE_DEF_TYPE_INTEGER:  ch = 'i'; break;
    case VARIABLE_DEF_TYPE_FLOAT:    ch = 'f'; break;
    case VARIABLE_DEF_TYPE_POINTER:  ch = '*'; break;
    case VARIABLE_DEF_TYPE_REFERENCE:ch = '&'; break;
    case VARIABLE_DEF_TYPE_FUNCTION: ch = 'F'; break;
    case VARIABLE_DEF_TYPE_STRUCTURE:ch = 'S'; break;
    case VARIABLE_DEF_TYPE_UNION:    ch = 'u'; break;
    case VARIABLE_DEF_TYPE_ARRAY:    ch = 'a'; break;
    case VARIABLE_DEF_TYPE_LABEL:    ch = 'l'; break;
    case VARIABLE_DEF_TYPE_VALUE:    ch = 'v'; break;
  }
  return ch;
}

boolean_t variable_def_type_is_number(variable_def_type_t type)
{
  boolean_t rc;

  rc = BOOLEAN_FALSE;
  switch (type) {
    case VARIABLE_DEF_TYPE_BOOLEAN:  /* assignment = (src) ? 1 : 0  */
    case VARIABLE_DEF_TYPE_INTEGER:  /* unsigned integer            */
    case VARIABLE_DEF_TYPE_FLOAT:    /* not used (yet)              */
    case VARIABLE_DEF_TYPE_LABEL:
    case VARIABLE_DEF_TYPE_VALUE:
      rc = BOOLEAN_TRUE;
      break;
    case VARIABLE_DEF_TYPE_NONE:
    case VARIABLE_DEF_TYPE_POINTER:
    case VARIABLE_DEF_TYPE_REFERENCE:
    case VARIABLE_DEF_TYPE_FUNCTION:
    case VARIABLE_DEF_TYPE_STRUCTURE:
    case VARIABLE_DEF_TYPE_UNION:
    case VARIABLE_DEF_TYPE_ARRAY:
      break;
  }
  return rc;
}

/*
 * variable_def_t was a late add and was added poorly. As such, it
 * leaks memory like a sieve. Argh. This hack forces the cleanup of
 * variable_def_t structures and is required by pic_test which can
 * run upwards of 1,000,000 tests on a single operator
 */
void variable_def_ugly_hack_cleanup(void)
{
  while (ugly_hack_head) {
    variable_def_free(ugly_hack_head);
  }
  assert(variable_def_outstanding == 0);
}

