/************************************************************
 **
 ** vardef.h : variable definition API definitions
 **
 ** Copyright (c) 2005, Kyle A. York
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: minor fork edits
 **
 ************************************************************/
#ifndef vardef_h__
#define vardef_h__

#include <stdint.h>
#include "../libutils/types.h"

typedef struct variable_def_        *variable_def_t;
typedef struct variable_def_member_ *variable_def_member_t;

typedef enum {
  VARIABLE_DEF_TYPE_NONE = 0,
  VARIABLE_DEF_TYPE_BOOLEAN,
  VARIABLE_DEF_TYPE_INTEGER,
  VARIABLE_DEF_TYPE_FLOAT,
  VARIABLE_DEF_TYPE_POINTER,
  VARIABLE_DEF_TYPE_REFERENCE,
  VARIABLE_DEF_TYPE_FUNCTION,
  VARIABLE_DEF_TYPE_STRUCTURE,
  VARIABLE_DEF_TYPE_UNION,
  VARIABLE_DEF_TYPE_ARRAY,
  VARIABLE_DEF_TYPE_LABEL,
  VARIABLE_DEF_TYPE_VALUE
} variable_def_type_t;
/* Aggregate byte size (e.g. array total) and per-dimension / element counts;
 * 32-bit so internal compiler arrays (e.g. string compact work table) are not
 * limited to 65535 bytes/elements. Scalar element sizes remain small (1,2,4,…). */
typedef uint32_t variable_sz_t;
typedef uint32_t variable_ct_t;
#define VARIABLE_CT_UNKNOWN ((variable_ct_t) -1)

#define VARIABLE_DEF_NONE ((variable_def_t) 0)

variable_def_t variable_def_alloc(const char *tag, 
  variable_def_type_t type, flag_t flags, variable_sz_t sz);

variable_def_t variable_def_dup(const variable_def_t src);
variable_def_t variable_def_flags_change(variable_def_t src, flag_t flags);

void           variable_def_free(variable_def_t def);

void           variable_def_ugly_hack_cleanup(void);

variable_def_t variable_def_link_get(const variable_def_t def);
void           variable_def_link_set(variable_def_t def, variable_def_t link);

const char    *variable_def_tag_get(const variable_def_t def);
variable_sz_t  variable_def_sz_get(const variable_def_t def);
variable_sz_t  variable_def_byte_sz_get(const variable_def_t def);
variable_def_t variable_def_sz_set(variable_def_t def, variable_sz_t sz);

variable_def_type_t variable_def_type_get(const variable_def_t def);
flag_t         variable_def_flags_get_all(const variable_def_t def);
boolean_t      variable_def_flag_test(const variable_def_t def, flag_t flag);

variable_def_member_t variable_def_member_get(variable_def_t def);
result_t              variable_def_member_insert(variable_def_t def,
                         variable_def_member_t after,
                         const char *tag, const variable_def_t mdef,
                         variable_ct_t ct);
result_t              variable_def_member_add(variable_def_t def,
                         const char *tag, const variable_def_t mdef,
                         variable_ct_t ct);

/*
 * For a data pointer whose pointee is a single scalar (not already an array),
 * not a nested data pointer and not a procedure/function signature, fold
 * `name[N]` after the name into pointer-to-array-of-N (one pointer slot).
 * Matches `var pointer const byte p[N]` and `var pointer var byte q[N]`.
 * Returns RESULT_INVALID if `ptr` is not eligible (e.g. `var pointer var
 * pointer var byte x[N]` stays an array of N pointers).
 */
result_t              variable_def_pointer_fold_scalar_pointee_array(
                         variable_def_t ptr, variable_ct_t dim);

variable_def_member_t variable_def_member_alloc(const char *tag, 
  const variable_def_t def, variable_ct_t ct);
void                  variable_def_member_free(variable_def_member_t mbr);

variable_def_member_t variable_def_member_link_get(
  const variable_def_member_t mbr);
void                  variable_def_member_link_set(
  variable_def_member_t mbr, variable_def_member_t link);

const char *variable_def_member_tag_get(const variable_def_member_t mbr);
variable_def_t variable_def_member_def_get(
  const variable_def_member_t mbr);
variable_ct_t variable_def_member_ct_get(const variable_def_member_t mbr);
result_t variable_def_member_ct_set(variable_def_t def, 
  variable_def_member_t mbr, variable_ct_t ct);
variable_ct_t variable_def_member_sz_get(const variable_def_member_t mbr);

boolean_t variable_def_is_intrinsic(const variable_def_t def);

boolean_t variable_def_is_same(const variable_def_t a, const variable_def_t b);
boolean_t variable_def_member_is_same(const variable_def_member_t a,
  const variable_def_member_t b);

char variable_def_type_to_ch(variable_def_type_t type);
const char *variable_def_type_to_str(variable_def_type_t type);

boolean_t variable_def_type_is_number(variable_def_type_t type);
#endif /* vardef_h__ */


