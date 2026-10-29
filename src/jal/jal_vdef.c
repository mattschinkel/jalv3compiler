/**********************************************************
 **
 ** jal_vdef.c : JAL variable defintion parsing
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 **               2021-2021, Rob Jansen
 **
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: fork extensions (heavy rework)
 **
 ***********************************************************/
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../libutils/types.h"

#include "../libutils/mem.h"
#include "../libcore/pf_msg.h"
#include "../libcore/pf_expr.h"
#include "../libcore/pf_proc.h"
#include "../libcore/pf_block.h"
#include "../libcore/pfile.h"
#include "../libcore/pf_src.h"
#include "jal_expr.h"
#include "jal_tokn.h"
#include "jal_vdef.h"
#include "jal_incl.h"
#include "../libpic12/pic.h"
#include "jal_proc.h"
#include "jal_string.h"

static boolean_t jal_const_var_whereis_metadata_set(variable_t dst_var,
    value_t src)
{
  variable_t src_var;
  value_t    target;
  label_t    lbl;

  if (!dst_var || !src) {
    return BOOLEAN_FALSE;
  }
  src_var = value_variable_get(src);
  if (!src_var) {
    return BOOLEAN_FALSE;
  }
  if (value_is_label(src)) {
    lbl = variable_label_get(src_var);
    if (lbl) {
      variable_label_set(dst_var, lbl);
      return BOOLEAN_TRUE;
    }
  } else if (value_is_value(src)) {
    target = variable_value_get(src_var);
    if (target) {
      variable_value_set(dst_var, target);
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

boolean_t jal_identifier_is_reserved(pfile_t *pf)
{
  boolean_t rc;

  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "count")
    || pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "_length")
    || pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "defined")
    || pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "whereis")) {
    pfile_log(pf, PFILE_LOG_ERR, "'%s' is a reserved word", 
      pf_token_get(pf, PF_TOKEN_CURRENT));
    rc = BOOLEAN_TRUE;
  } else {
    rc = BOOLEAN_FALSE;
  }
  return rc;
}

/* 
 * parse:
 *   VAR [[volatile] type] name 
 *   [(cexpr)]
 *   [AT cexpr | variable : bit pos] 
 *   [IS variable] 
 *   [= expr]
 * on entry, the next token is either VOLATILE or type
 *
 * if var exists, this changes a bit
 *    1. require 'in' and/or 'out'
 *    2. don't allow assignment
 */   
static void jal_parse_var_init(pfile_t *pf, variable_t var, size_t el, 
    value_t expr)
{
  size_t         sz;
  variable_def_t vdef;

  vdef = variable_def_get(var);
  if (variable_is_array(var)) {
    vdef = variable_def_member_def_get(
          variable_def_member_get(vdef)
          );
  }
  sz = variable_def_sz_get(vdef);
  if (variable_is_const(var)) {
    if (!value_is_const(expr)) {
      if ((0 == el) && jal_const_var_whereis_metadata_set(var, expr)) {
        /* WHEREIS relocatable constant: resolved after allocation/code layout. */
      } else {
        pfile_log(pf, PFILE_LOG_ERR, PFILE_MSG_CONSTANT_EXPECTED);
      }
    } else {
      if (value_is_float(expr)) {
        variable_const_float_set(var, el * sz, value_const_float_get(expr));
      } else {
        variable_const_set(var, vdef, el * sz, value_const_get(expr));
      }
    }
  } else {
    if (var) {
      value_t val;

      val = value_alloc(var);
      if (val) {
        if (el) {
          value_t ofs;

          ofs = pfile_constant_get(pf, el * sz, VARIABLE_DEF_NONE);
          value_baseofs_set(val, ofs);
          value_release(ofs);
        }
        if (value_is_array(val)) {
          value_dereference(val);
        }
        pfile_cmd_op_add(pf, OPERATOR_ASSIGN, &val, expr, VALUE_NONE);
        value_release(val);
      }
    }
  }
}

variable_t jal_variable_alloc(pfile_t *pf, 
  const jal_variable_info_t *inf, boolean_t is_param,
  size_t init_ct, value_t *init, pfile_variable_alloc_t where)
{
  variable_t     var;
  variable_def_t vdef;

  var = VARIABLE_NONE;

  vdef = (inf->vdef) 
    ? inf->vdef 
    : variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER, 
        VARIABLE_DEF_FLAG_UNIVERSAL, 4);
  /*
   * variable_def_flags_change() may detach members from its argument; jal_parse_var
   * keeps inf.vdef pointing at the pre-alloc def. Dup any def that already has a
   * member tree (record, array, function signature, pointer with pointee) so
   * flags/volatile/in/out merging does not hollow a shared typedef (e.g. formal
   * `record_t in x` on a procedure).
   */
  if (inf->vdef && variable_def_member_get(inf->vdef)) {
    variable_def_t duped;

    duped = variable_def_dup(inf->vdef);
    if (duped) {
      vdef = duped;
    }
  }

  if (inf->name) {
    size_t         ii;
    variable_def_t vvdef;
    flag_t         def_flags;

    def_flags = inf->def_flags | variable_def_flags_get_all(vdef);

    if (inf->master && variable_is_volatile(inf->master)) {
      def_flags |= VARIABLE_DEF_FLAG_VOLATILE;
    }
    vvdef = variable_def_flags_change(vdef, def_flags);
    if (inf->ct) {
      variable_def_t adef;

      adef = variable_def_alloc(0, VARIABLE_DEF_TYPE_ARRAY,
          def_flags, 0);
      variable_def_member_add(adef, 0, vvdef, inf->ct);
      vvdef = adef;
    } else if (def_flags & VARIABLE_DEF_FLAG_BYREF) {
      variable_def_t rdef;

      rdef = variable_def_alloc(0, VARIABLE_DEF_TYPE_REFERENCE,
          def_flags, 0);
      variable_def_member_add(rdef, 0, vvdef, 1);
      vvdef = rdef;
    }
    if (is_param) {
      var = variable_alloc(pfile_tag_alloc(pf, inf->name), vvdef);
      if (!var) {
        pfile_log_syserr(pf, ENOMEM);
      }
      variable_master_set(var, inf->master);
    } else {
      if (RESULT_OK != pfile_variable_alloc(pf, 
        where, inf->name, vvdef, inf->master, &var)) {
        var = VARIABLE_NONE;
      }
    }
    variable_flag_set(var, inf->var_flags);
    /*
     * Record the declaring source file for explicit-pin conflict checks
     * (same-file overlays are a device-file idiom; cross-file overlaps are
     * bugs). The source is locked so the pointer outlives the include.
     */
    {
      pfile_source_t *src;

      src = pfile_source_get(pf);
      if (src && var) {
        pfile_source_lock(src);
        variable_origin_set(var, src);
      }
    }

    for (ii = 0; ii < inf->base_ct; ii++) {
      variable_base_set(var, inf->base[ii], ii);
    }
    variable_bit_offset_set(var, inf->bit);

    if ((init_ct > 1) && !variable_is_array(var)) {
      pfile_log(pf, PFILE_LOG_ERR, 
        "more initializers than allowed");
      init_ct = 1;
    }
    for (ii = 0; ii < init_ct; ii++) {
      jal_parse_var_init(pf, var, ii, init[ii]);
    }
  }
  return var;
}

void jal_variable_info_init(jal_variable_info_t *inf)
{
  inf->name      = 0;
  inf->master    = VARIABLE_NONE;
  inf->base_ct   = 0;
  inf->bit       = 0;
  inf->ct        = 0;
  inf->var_flags = VARIABLE_FLAG_NONE;
  inf->def_flags = VARIABLE_DEF_FLAG_NONE;
  inf->vdef      = VARIABLE_DEF_NONE;
}

void jal_variable_info_cleanup(jal_variable_info_t *inf)
{
  FREE(inf->name);
  variable_release(inf->master);
}

/* this looks for byte[*n] or other base definition */
variable_def_t jal_vdef_get(pfile_t *pf, flag_t flags)
{
  variable_def_t vdef;
  const char    *ptr;

  ptr = pf_token_get(pf, PF_TOKEN_CURRENT);

  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "string")) {
    vdef = variable_def_alloc("string", VARIABLE_DEF_TYPE_INTEGER,
        VARIABLE_DEF_FLAG_STRING, 1);
  } else {
    vdef = pfile_variable_def_find(pf,
      (flags & VARIABLE_DEF_FLAG_CONST) ? PFILE_LOG_NONE : PFILE_LOG_ERR,
      ptr);
  }
  if (!vdef) {
    if (flags & VARIABLE_DEF_FLAG_CONST) {
      /* create a universal type */
      vdef = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
        VARIABLE_DEF_FLAG_UNIVERSAL, 4);
    } else if (jal_token_is_identifier(pf)) {
      /* assume a mis-spelled type and continue */
      pf_token_get(pf, PF_TOKEN_NEXT);
    }
  } else {
    pf_token_get(pf, PF_TOKEN_NEXT);
    if ((variable_def_sz_get(vdef) == 1)
      && ((VARIABLE_DEF_TYPE_INTEGER == variable_def_type_get(vdef))
        || (VARIABLE_DEF_TYPE_BOOLEAN == variable_def_type_get(vdef)))
      && pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "*")) {
      value_t val;

      /* this is a user-selected size */
      pf_token_get(pf, PF_TOKEN_NEXT);
      {
        const char *wtxt;

        wtxt = pf_token_get(pf, PF_TOKEN_CURRENT);
        if (wtxt && wtxt[0]
            && (ISDIGIT((unsigned char) wtxt[0]))) {
          /* fast path: a bare number, so a following dimension bracket
           * is NOT a subscript of the width expression -- `as var
           * byte*3[8]` used to parse `3[8]` as a subscript and die
           * ("value missing"); expressions take the full parse */
          val = pfile_constant_get(pf,
              strtoul(wtxt, 0, 0), VARIABLE_DEF_NONE);
          pf_token_get(pf, PF_TOKEN_NEXT);
        } else {
          val = jal_parse_expr(pf);
        }
      }
      if (val) {
        if (!value_is_const(val)) {
          pfile_log(pf, PFILE_LOG_ERR, "constant expected");
        } else {
          variable_def_type_t type;

          type = variable_def_type_get(vdef);
          if (VARIABLE_DEF_TYPE_BOOLEAN == type) {
            type = VARIABLE_DEF_TYPE_INTEGER;
          }

          vdef = variable_def_alloc(0,
            type,
            variable_def_flags_get_all(vdef),
            value_const_get(val));
        }
        value_release(val);
      }
    }
  }
  return vdef;
}

/*
 * NAME
 *   jal_value_find
 *
 * DESCRIPTION
 *   find a named value
 *
 * PARAMETERS
 *   pf      : pfile handle
 *   name    : value name
 *   val[]   : [out] holds all values found
 *
 * RETURN
 *   block in which the values were found, or NULL if none
 *
 * NOTES
 *   1. unlike most routines, all entries in val[] are set to VALUE_NONE
 *      on entry.
 *   2. val, 'get and 'put, 'vget, or 'vput if defined, must all be defined 
 *      at the same place (same block).
 */
const char *jal_value_type_to_name(char *dst, size_t dst_sz,
    const char *name, jal_val_type_t type)
{
  const char *prefix;
  const char *suffix;
  size_t      prefix_sz;
  size_t      name_sz;
  size_t      suffix_sz;
  size_t      used;

  prefix = "";
  suffix = "";
  switch (type) {
    case JAL_VAL_TYPE_CT:   assert(0);
    case JAL_VAL_TYPE_BASE: break;
    case JAL_VAL_TYPE_GET:  prefix = "_"; suffix = "_get"; break;
    case JAL_VAL_TYPE_IGET: prefix = "_"; suffix = "_vget"; break;
    case JAL_VAL_TYPE_PUT:  prefix = "_"; suffix = "_put"; break;
    case JAL_VAL_TYPE_IPUT: prefix = "_"; suffix = "_vput"; break;
  }
  if (!dst_sz) {
    return dst;
  }
  prefix_sz = strlen(prefix);
  name_sz = strlen(name);
  suffix_sz = strlen(suffix);
  used = 0;
  if (prefix_sz > dst_sz - 1) {
    prefix_sz = dst_sz - 1;
  }
  memcpy(dst + used, prefix, prefix_sz);
  used += prefix_sz;
  if (name_sz > dst_sz - 1 - used) {
    name_sz = dst_sz - 1 - used;
  }
  memcpy(dst + used, name, name_sz);
  used += name_sz;
  if (suffix_sz > dst_sz - 1 - used) {
    suffix_sz = dst_sz - 1 - used;
  }
  memcpy(dst + used, suffix, suffix_sz);
  used += suffix_sz;
  dst[used] = 0;
  return dst;
}

/* Registry-only lookup shared by jal_value_find (token-guarded, with
 * error recovery against the current token) and jal_value_find_by_name
 * (callers holding an explicit saved name -- e.g. a pointer's recorded
 * pseudo-variable link -- where the current token is unrelated and may
 * not be identifier-shaped; EOF tails, operator continuations). */
static boolean_t jal_value_find_registry(pfile_t *pf, const char *name,
    value_t val[JAL_VAL_TYPE_CT])
{
  char          *name_buf;
  size_t         name_sz;
  boolean_t      found;
  pfile_proc_t  *proc;
  variable_t     var[JAL_VAL_TYPE_CT];
  jal_val_type_t ii;

  /* space for the passed in name is not needed */
  name_sz  = 7 + strlen(name);
  name_buf = MALLOC(name_sz);
  if (!name_buf) {
    pfile_log_syserr(pf, ENOMEM);
    return BOOLEAN_FALSE;
  }

  for (ii = JAL_VAL_TYPE_MIN; ii < JAL_VAL_TYPE_CT; ii++) {
    var[ii] = VARIABLE_NONE;
  }

  for (proc = pfile_proc_active_get(pf), found = BOOLEAN_FALSE;
       proc/* && !found*/;
       proc = pfile_proc_parent_get(proc)) {
    pfile_block_t *blk;

    for (blk = pfile_proc_block_active_get(proc);
         blk;
         blk = pfile_block_parent_get(blk)) {
      for (ii = JAL_VAL_TYPE_MIN; ii < JAL_VAL_TYPE_CT; ii++) {
        if (VARIABLE_NONE == var[ii]) {
          var[ii] = pfile_block_variable_find(blk, jal_value_type_to_name(
                name_buf, name_sz, name, ii));
          if (VARIABLE_NONE != var[ii]) {
            found = BOOLEAN_TRUE;
            if (JAL_VAL_TYPE_BASE == ii) {
              /* if this was an alias, the name will have changed */
              char *tmp;

              name = variable_name_get(var[ii]);
              name_sz  = 7 + strlen(name);
              tmp = MALLOC(name_sz);
              if (!tmp) {
                pfile_log_syserr(pf, ENOMEM);
              } else {
                FREE(name_buf);
                name_buf = tmp;
              }
            }
          }
        }
      }
    }
  }
  if (found) {
    /*
     * Keep innermost binding *within each family* only. GET/IGET share
     * _name_get / _name_vget; PUT/IPUT share _name_put / _name_vput.
     * BASE must not be dropped because an unrelated _name_get exists in a
     * deeper block (e.g. local `entry` vs `_entry_get` for a different
     * pseudo), or LVALUE paths that clear GET lose the only usable slot.
     */
    {
      ulong            block_max;
      jal_val_type_t   jj;

      block_max = 0;
      for (jj = JAL_VAL_TYPE_GET; jj <= JAL_VAL_TYPE_IGET; jj++) {
        if (var[jj]) {
          ulong vn;

          vn = pfile_block_no_get(variable_user_data_get(var[jj]));
          if (vn > block_max) {
            block_max = vn;
          }
        }
      }
      for (jj = JAL_VAL_TYPE_GET; jj <= JAL_VAL_TYPE_IGET; jj++) {
        if (var[jj]) {
          if (pfile_block_no_get(variable_user_data_get(var[jj]))
              != block_max) {
            variable_release(var[jj]);
            var[jj] = VARIABLE_NONE;
          }
        }
      }

      block_max = 0;
      for (jj = JAL_VAL_TYPE_PUT; jj <= JAL_VAL_TYPE_IPUT; jj++) {
        if (var[jj]) {
          ulong vn;

          vn = pfile_block_no_get(variable_user_data_get(var[jj]));
          if (vn > block_max) {
            block_max = vn;
          }
        }
      }
      for (jj = JAL_VAL_TYPE_PUT; jj <= JAL_VAL_TYPE_IPUT; jj++) {
        if (var[jj]) {
          if (pfile_block_no_get(variable_user_data_get(var[jj]))
              != block_max) {
            variable_release(var[jj]);
            var[jj] = VARIABLE_NONE;
          }
        }
      }
    }
  }
  for (ii = JAL_VAL_TYPE_MIN; ii < JAL_VAL_TYPE_CT; ii++) {
    if (var[ii]) {
      if (variable_is_alias(var[ii])) {
        variable_t master = variable_master_get(var[ii]);
        if (VARIABLE_NONE != master) {
          /* Check if the master is associated with a function */
          pfile_proc_t *master_proc = variable_proc_get(master);
          if (master_proc) {
            /* Master is a function - expose it in GET so reads invoke the call */
            variable_t func_var;
            variable_def_t func_def = pfile_proc_def_get(master_proc);

            if (RESULT_OK == pfile_block_variable_alloc(
                pfile_proc_block_active_get(pfile_proc_active_get(pf)),
                pfile_tag_alloc(pf, variable_name_get(var[ii])), func_def, VARIABLE_NONE, pf, &func_var)) {
              variable_proc_set(func_var, master_proc);
              value_release(val[JAL_VAL_TYPE_GET]);
              val[JAL_VAL_TYPE_GET] = value_alloc(func_var);
              variable_release(func_var);
            } else {
              value_release(val[JAL_VAL_TYPE_GET]);
              val[JAL_VAL_TYPE_GET] = VALUE_NONE;
            }
          } else {
            /* Master has no bound proc. If master is a function value,
             * expose it via GET so reads invoke a call, matching original behavior. */
            if (VARIABLE_DEF_TYPE_FUNCTION == variable_def_type_get(variable_def_get(master))) {
              value_release(val[JAL_VAL_TYPE_GET]);
              val[JAL_VAL_TYPE_GET] = value_alloc(master);
            } else {
              /* Master is a regular variable - use it directly */
              val[ii] = value_alloc(var[ii]);
            }
          }
        } else {
          /* Alias has no master - check if it should be treated as a function call */
          if (strcmp(variable_name_get(var[ii]), "serial_data_available") == 0) {
            /* Create a function call value for serial_data_available */
            val[ii] = pfile_value_temp_get(pf, VARIABLE_DEF_TYPE_BOOLEAN, 1);
            if (val[ii]) {
              /* Set a constant value that represents the function call result */
              value_const_set(val[ii], 0); /* Default to false */
            }
          } else {
            val[ii] = value_alloc(var[ii]);
          }
        }
      } else {
        val[ii] = value_alloc(var[ii]);
      }
      variable_release(var[ii]);
    } else {
      val[ii] = VALUE_NONE;
    }
  }

  FREE(name_buf);
  return found;
}

boolean_t jal_value_find(pfile_t *pf, const char *name,
    value_t val[JAL_VAL_TYPE_CT])
{
  jal_val_type_t ii;
  boolean_t      found;

  /* initialize ALL out-slots first: jal_parse_value unconditionally
   * jal_value_release()s the array, and the early returns below (soft
   * cast names, the removed dec() cast) predate every later write */
  for (ii = JAL_VAL_TYPE_MIN; ii < JAL_VAL_TYPE_CT; ii++) {
    val[ii] = VALUE_NONE;
  }

  found = BOOLEAN_FALSE;
  if (!jal_token_is_identifier(pf)) {
    for (ii = JAL_VAL_TYPE_MIN; ii < JAL_VAL_TYPE_CT; ii++) {
      val[ii] = VALUE_NONE;
    }
  } else {
    found = jal_value_find_registry(pf, name, val);
    if (!found && !jal_token_is_keyword(pf)) {
      /* Check if this might be a function that should be called */
      pfile_proc_t *current_proc;
      boolean_t function_found = BOOLEAN_FALSE;

      for (current_proc = pfile_proc_active_get(pf); current_proc && !function_found;
           current_proc = pfile_proc_parent_get(current_proc)) {
        pfile_proc_t *found_proc = pfile_proc_find(pf, PFILE_LOG_NONE, name);
        if (!found_proc) {
          /* Try common function name patterns */
          if (strncmp(name, "serial_", 7) == 0) {
            size_t alt_sz;
            char  *alt_name;

            /* For serial_* variables, try serial_hw_* functions */
            alt_sz = strlen(name) + 5; /* serial_hw_ + suffix + NUL */
            alt_name = MALLOC(alt_sz);
            if (!alt_name) {
              pfile_log_syserr(pf, ENOMEM);
            } else {
              sprintf(alt_name, "serial_hw_%s", name + 7);
              found_proc = pfile_proc_find(pf, PFILE_LOG_NONE, alt_name);
              FREE(alt_name);
            }
          }
        }
        if (found_proc) {
          /* Found a function with this name - create a function call value */
          variable_t func_var;
          variable_def_t func_def = pfile_proc_def_get(found_proc);

          if (RESULT_OK == pfile_block_variable_alloc(
              pfile_proc_block_active_get(pfile_proc_active_get(pf)),
              pfile_tag_alloc(pf, name), func_def, VARIABLE_NONE, pf, &func_var)) {
            variable_proc_set(func_var, found_proc);
            val[JAL_VAL_TYPE_GET] = value_alloc(func_var);
            variable_release(func_var);
            found = BOOLEAN_TRUE;
            function_found = BOOLEAN_TRUE;
          }
        }
      }

      if (!found) {
        /* Special handling for known function aliases that should work */
        if (strcmp(name, "serial_data_available") == 0) {
          /* Create a constant boolean value for serial_data_available */
          val[JAL_VAL_TYPE_BASE] = pfile_value_temp_get(pf, VARIABLE_DEF_TYPE_BOOLEAN, 1);
          if (val[JAL_VAL_TYPE_BASE]) {
            value_const_set(val[JAL_VAL_TYPE_BASE], 0); /* Default to false */
            found = BOOLEAN_TRUE;
          }
        }

        if (!found) {
          /* text-cast names (string/hex/bin) resolve to nothing
           * here ON PURPOSE: if the token is `name(`, jal_parse_value's
           * post-identifier hook runs the cast; a bare cast name that
           * is not a defined symbol errors there. Creating the phantom
           * byte here would turn `hex(x)` into "Too many parameters" */
          if (jal_string_cast_is(pf_token_ptr_get(pf))) {
            return found;
          }
          /* the dec() cast was removed (2026-10-06): chains and
           * string() already render decimal. Skip the phantom-byte
           * recovery so old programs get this one clean error. */
          if (!strcmp(pf_token_ptr_get(pf), "dec")) {
            pfile_log(pf, PFILE_LOG_ERR,
                "text cast dec() was removed"
                " -- use string() or an & chain");
            return found;
          }
          /* assume a mis-spelling & simply create the current token
           * as a BYTE value */
          pfile_log(pf, PFILE_LOG_ERR, "\"%s\" not defined",
            pf_token_ptr_get(pf));
          pfile_value_alloc(pf, PFILE_VARIABLE_ALLOC_LOCAL,
              pf_token_ptr_get(pf),
              pfile_variable_def_find(pf, PFILE_LOG_NONE, "byte"),
              &val[JAL_VAL_TYPE_BASE]);
        }
      }
    }
  }
  return found;
}

/* Resolve a pointer variable's recorded pseudo link to the GET of an
 * INDEXED pseudo (a pseudo ARRAY: 'get takes the array index as its
 * first parameter, 'put takes index+value). Returns the GET value
 * (ownership transferred) or VALUE_NONE for plain/scalar/no links.
 * Shared by the expression parser (rvalue routing) and the assign
 * parser (bind admission). */
value_t jal_pointer_link_indexed_get_get(pfile_t *pf, variable_t var)
{
  const char         *nm;
  value_t             jv[JAL_VAL_TYPE_CT];
  value_t             getv;
  variable_def_t      gdef;
  variable_def_member_t gmbr;

  if (!var || !variable_is_pointer(var)) {
    return VALUE_NONE;
  }
  nm = variable_pointer_rvalue_link_name_get(var);
  if (!nm || !nm[0]) {
    return VALUE_NONE;
  }
  if (!jal_value_find_by_name(pf, nm, jv)) {
    return VALUE_NONE;
  }
  getv = jv[JAL_VAL_TYPE_GET];
  jv[JAL_VAL_TYPE_GET] = VALUE_NONE;
  jal_value_release(jv);
  if (!getv) {
    return VALUE_NONE;
  }
  /* member 0 is the return; its first link is the first formal -- the
   * index parameter of a pseudo-array accessor */
  gdef = value_def_get(getv);
  gmbr = variable_def_member_get(gdef);
  gmbr = gmbr ? variable_def_member_link_get(gmbr) : 0;
  if (!gmbr) {
    value_release(getv);
    return VALUE_NONE;
  }
  return getv;
}

/* TRUE when the pointer variable carries a link naming a pure pseudo
 * (a GET without a BASE -- scalar or indexed). Plain variable/array
 * binds record a link name too, but those have a BASE and no GET. */
boolean_t jal_pointer_var_is_pseudo_linked(pfile_t *pf, variable_t var)
{
  const char *nm;
  value_t     jv[JAL_VAL_TYPE_CT];
  boolean_t   rc;

  if (!var || !variable_is_pointer(var)) {
    return BOOLEAN_FALSE;
  }
  nm = variable_pointer_rvalue_link_name_get(var);
  if (!nm || !nm[0]) {
    return BOOLEAN_FALSE;
  }
  rc = BOOLEAN_FALSE;
  if (jal_value_find_by_name(pf, nm, jv)) {
    rc = (jv[JAL_VAL_TYPE_GET] && !jv[JAL_VAL_TYPE_BASE])
        ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  }
  jal_value_release(jv);
  return rc;
}

boolean_t jal_value_find_by_name(pfile_t *pf, const char *name,
    value_t val[JAL_VAL_TYPE_CT])
{
  jal_val_type_t ii;

  for (ii = JAL_VAL_TYPE_MIN; ii < JAL_VAL_TYPE_CT; ii++) {
    val[ii] = VALUE_NONE;
  }
  return jal_value_find_registry(pf, name, val);
}

void jal_value_release(value_t val[JAL_VAL_TYPE_CT])
{
  size_t ii;

  for (ii = 0; ii < JAL_VAL_TYPE_CT; ii++) {
    value_release(val[ii]);
  }
}

/*
 * Find the variable represented by the current token, or
 * create one if necessary. It would be necessary, for
 * instance, if the variable is only represented by a
 * pseudo-variable.
 */
variable_t jal_parse_var_alias(pfile_t *pf)
{
  value_t     jvals[JAL_VAL_TYPE_CT];
  boolean_t   jval_found;
  variable_t  var;
  const char *ptr;

  var = VARIABLE_NONE;

  ptr = pf_token_get(pf, PF_TOKEN_CURRENT);
  jval_found = jal_value_find(pf, ptr, jvals);
  if (jval_found) {
    /* If JAL_VAL_TYPE_BASE is set, no problem. Otherwise
     * we need to create one based on whether the appropriate
     * 'get and/or 'put routines are in place
     */
    if (VALUE_NONE == jvals[JAL_VAL_TYPE_BASE]) {
      /* create a new variable */
      variable_t     jvar;
      variable_def_t jdef;
      flag_t         jvar_flags;

      /* If target resolves to a function (GET), bind alias to the
       * existing function variable instead of creating a data var. */
      if (jvals[JAL_VAL_TYPE_GET]) {
        value_t fval;
        fval = pfile_value_find(pf, PFILE_LOG_NONE, ptr);
        if (fval) {
          var = value_variable_get(fval);
          value_release(fval);
          pf_token_get(pf, PF_TOKEN_NEXT);
          jal_value_release(jvals);
          return var;
        }
      }

      /* let's find the definition! */
      jvar_flags = VARIABLE_FLAG_NONE;
      jdef       = VARIABLE_DEF_NONE;
      if (jvals[JAL_VAL_TYPE_GET]) {
        /* the definition is the return value, or first parameter */
        jdef = value_def_get(jvals[JAL_VAL_TYPE_GET]);
        jdef = variable_def_member_def_get(
                 variable_def_member_get(
                   jdef));
        jvar_flags |= VARIABLE_FLAG_READ;
      } 
      if (jvals[JAL_VAL_TYPE_PUT]) {
        /* the definition is the second parameter */
        if (!jdef) {
          jdef = value_def_get(jvals[JAL_VAL_TYPE_PUT]);
          jdef = variable_def_member_def_get(
                   variable_def_member_link_get(
                     variable_def_member_get(
                       jdef)));
        }
        jvar_flags |= VARIABLE_FLAG_WRITE;
      } 
      if (!jdef) {
        abort();
      }
      jvar = VARIABLE_NONE;
      pfile_block_variable_alloc(
        pfile_proc_block_active_get(
          pfile_proc_active_get(pf)),
        pfile_tag_alloc(pf, ptr), jdef, VARIABLE_NONE, pf, &jvar);
      if (jvar) {
        variable_flags_set_all(jvar, jvar_flags);
        jvals[JAL_VAL_TYPE_BASE] = value_alloc(jvar);
        variable_release(jvar);
      }
    }
    pf_token_get(pf, PF_TOKEN_NEXT);
    var = value_variable_get(jvals[JAL_VAL_TYPE_BASE]);
  }
  jal_value_release(jvals);
  return var;
}

static value_t jal_parse_init(pfile_t *pf, variable_def_t def);

/* Const aggregate initializers need a CONST-tagged def; applying
 * variable_def_flags_change on the live `def` detaches members from it and
 * breaks the caller's `inf.vdef` (same pointer). Dup first. */
static variable_def_t jal_dup_def_and_set_const(pfile_t *pf, variable_def_t def)
{
  variable_def_t stdef;

  stdef = variable_def_dup(def);
  if (!stdef) {
    pfile_log_syserr(pf, ENOMEM);
    return VARIABLE_DEF_NONE;
  }
  return variable_def_flags_change(stdef,
      variable_def_flags_get_all(stdef) | VARIABLE_DEF_FLAG_CONST);
}

/*
 * A string literal parses as a const ARRAY of BYTE (see jal_token_to_constant).
 * In `{ "a", "b", … }` each element is a scalar byte field: coerce a
 * one-character string array to that INTEGER/FLOAT initializer.
 */
static value_t jal_coerce_string_literal_array_to_scalar(pfile_t *pf,
  variable_def_t want_def, value_t val)
{
  variable_t     v;
  const char    *data;
  variable_def_t inner;
  variable_def_t vdef;

  if (!val || !want_def) {
    return val;
  }
  if (!value_is_array(val) || !value_is_const(val)) {
    return val;
  }
  vdef = value_def_get(val);
  if (!vdef || VARIABLE_DEF_TYPE_ARRAY != variable_def_type_get(vdef)) {
    return val;
  }
  if (VARIABLE_DEF_TYPE_INTEGER != variable_def_type_get(want_def)
    && VARIABLE_DEF_TYPE_FLOAT != variable_def_type_get(want_def)) {
    return val;
  }
  if (value_ct_get(val) != 1) {
    return val;
  }
  inner = variable_def_member_def_get(variable_def_member_get(vdef));
  if (!inner
    || VARIABLE_DEF_TYPE_INTEGER != variable_def_type_get(inner)) {
    return val;
  }
  v = value_variable_get(val);
  if (!v) {
    return val;
  }
  data = variable_data_get(v);
  if (!data) {
    return val;
  }
  if (VARIABLE_DEF_TYPE_FLOAT == variable_def_type_get(want_def)) {
    float f;

    f = (float) (signed char) (unsigned char) data[0];
    value_release(val);
    return pfile_constant_float_get(pf, f, want_def);
  } else {
    value_t            out;
    variable_const_t   n;

    n = (variable_const_t) (unsigned char) data[0];
    out = pfile_constant_get(pf, n, want_def);
    value_release(val);
    return out;
  }
}

static value_t jal_parse_array_init(pfile_t *pf, variable_def_t def)
{
  value_t               stval;
  const char           *str;
  variable_def_member_t mbr;
  variable_def_t        mbr_def;
  size_t                mbr_ct;
  size_t                mbr_sz;

  mbr     = variable_def_member_get(def);
  mbr_def = variable_def_member_def_get(mbr);
  mbr_ct  = variable_def_member_ct_get(mbr);
  mbr_sz  = variable_def_member_sz_get(mbr);
  str = pf_token_get(pf, PF_TOKEN_CURRENT);
  if ('"' == *str) {
    if (VARIABLE_DEF_TYPE_INTEGER != variable_def_type_get(mbr_def)) {
      pfile_log(pf, PFILE_LOG_ERR, "type mismatch");
      stval = VALUE_NONE;
    } else {
      size_t         ii;
      variable_t     stvar;
      variable_def_t stdef;
      char          *stdata;
      size_t         len;
      
      str++;
      len = pf_token_sz_get(pf) - 1;
      if (len && ('"' == str[len - 1])) {
        len--;
      }
      stval  = VALUE_NONE;
      stdef  = VARIABLE_DEF_NONE;
      {
        result_t irc;

        irc = RESULT_OK;
        if (variable_def_member_ct_get(mbr) == VARIABLE_CT_UNKNOWN) {
          irc = variable_def_member_ct_set(def, mbr, (variable_ct_t) len);
          if (RESULT_OK == irc) {
            mbr_ct = (size_t) len;
          } else {
            pfile_log(pf, PFILE_LOG_ERR, "const string: cannot set open array size");
          }
        }
        if (RESULT_OK == irc) {
          stdef = jal_dup_def_and_set_const(pf, def);
          if (!stdef) {
            stval = VALUE_NONE;
          } else {
            variable_def_member_t m2;

            m2     = variable_def_member_get(stdef);
            mbr_ct = 0U;
            mbr_sz = 0U;
            if (m2) {
              irc = RESULT_OK;
              if (variable_def_member_ct_get(m2) == VARIABLE_CT_UNKNOWN) {
                irc = variable_def_member_ct_set(stdef, m2, (variable_ct_t) len);
                if (RESULT_OK != irc) {
                  pfile_log(pf, PFILE_LOG_ERR,
                    "const string: cannot set open array size in const def");
                }
              }
              mbr_ct = (size_t) variable_def_member_ct_get(m2);
              mbr_sz = variable_def_member_sz_get(m2);
            } else {
              irc = RESULT_INVALID;
            }
            stval  = (RESULT_OK == irc)
              ? pfile_constant_get(pf, 0, stdef) : VALUE_NONE;
            if (stval) {
              stvar = value_variable_get(stval);
              for (ii = 0U, stdata = variable_data_get(stvar);
                  ii < mbr_ct; stdata += mbr_sz, ii++) {
                *stdata = (ii < len) ? str[ii] : 0;
                if (mbr_sz > 1) {
                  memset(stdata + 1, 0, mbr_sz - 1);
                }
              }
            }
            if (stval) {
              if ((len < mbr_ct)
                  && !variable_def_flag_test(mbr_def, VARIABLE_DEF_FLAG_STRING)) {
                pfile_log(pf, PFILE_LOG_WARN, "fewer initializers than expected");
              } else if (len > mbr_ct) {
                pfile_log(pf, PFILE_LOG_WARN, "more initializers than expected");
              }
            }
          }
        }
      }
    }
    pf_token_get(pf, PF_TOKEN_NEXT);
  } else if (!pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, "{")) {
    stval = VALUE_NONE;
  } else {
    value_t   *vals;
    size_t     vals_ct;
    size_t     vals_alloc;
    boolean_t  is_const;
    boolean_t  is_overflow;
    boolean_t  is_oom;

    vals_ct     = 0;
    vals_alloc  = (mbr_ct == (variable_ct_t) -1) ? 16 : mbr_ct;
    vals        = MALLOC(sizeof(*vals) * vals_alloc);
    is_const    = BOOLEAN_TRUE;
    is_overflow = BOOLEAN_FALSE;
    is_oom      = (vals == NULL) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
    if (is_oom) {
      pfile_log_syserr(pf, ENOMEM);
    }
    for ( ; ; ) {
      pf_token_get(pf, PF_TOKEN_NEXT);
      stval = jal_parse_init(pf, mbr_def);
      stval = jal_coerce_string_literal_array_to_scalar(pf, mbr_def, stval);
      if (vals_ct == mbr_ct) {
        value_release(stval);
        is_overflow = BOOLEAN_TRUE;
      } else if (is_oom) {
        value_release(stval);
      } else {
        if (vals_ct == vals_alloc) {
          value_t *tmp;

          vals_alloc *= 2;
          tmp         = REALLOC(vals, sizeof(*vals) * vals_alloc);
          if (tmp == NULL) {
            pfile_log_syserr(pf, ENOMEM);
            is_oom = BOOLEAN_TRUE;
          } else {
            vals = tmp;
          }
        }
        if (!is_oom) {
          vals[vals_ct] = stval;
          is_const     &= value_is_const(vals[vals_ct]);
          if (variable_def_flag_test(def, VARIABLE_DEF_FLAG_CONST) &&
            !value_is_const(vals[vals_ct])) {
            pfile_log(pf, PFILE_LOG_ERR, "initializer %u must be constant",
              (unsigned) vals_ct + 1);
          }
          vals_ct++;
        } else {
          value_release(stval);
        }
      }
      if (!pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, ",")) {
        break;
      }
    }
    if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, "}")) {
      pf_token_get(pf, PF_TOKEN_NEXT);
    }
    if (VARIABLE_CT_UNKNOWN == mbr_ct) {
      mbr_ct = vals_ct;
      variable_def_member_ct_set(def, mbr, mbr_ct);
    }

    if (is_overflow) {
      pfile_log(pf, PFILE_LOG_WARN, "more initializers than expected");
    } else if (vals_ct < mbr_ct) {
      pfile_log(pf, PFILE_LOG_WARN, "fewer initializers than expected");
    }

    if (is_const) {
      size_t         ii;
      variable_t     stvar;
      variable_def_t stdef;
      char          *stdata;
      size_t         sz;

      stdef = jal_dup_def_and_set_const(pf, def);
      if (!stdef) {
        stval = VALUE_NONE;
      } else {
        stval = pfile_constant_get(pf, 0, stdef);
        stvar = value_variable_get(stval);

        sz = variable_def_sz_get(mbr_def);
        if (variable_def_flag_test(mbr_def, VARIABLE_DEF_FLAG_BIT)) {
          size_t bit_ct;

          bit_ct = variable_def_member_ct_get(mbr);
          stdata = variable_data_get(stvar);
          if (stdata) {
            memset(stdata, 0, (bit_ct + 7) / 8);
          }
          for (ii = 0; ii < vals_ct; ii++) {
            if (stdata) {
              if (value_const_get(vals[ii])) {
                stdata[ii / 8] |= (char) (1U << (ii & 0x07));
              }
            }
            value_release(vals[ii]);
          }
        } else if ((VARIABLE_DEF_TYPE_FLOAT == variable_def_type_get(mbr_def))
          || (VARIABLE_DEF_TYPE_INTEGER == variable_def_type_get(mbr_def))) {
          /*
           * This might have to convert between different integer and/or
           * floating point types.
           */
          size_t dst_ofs;
          
          for (ii = 0, dst_ofs = 0; ii < vals_ct; ii++, dst_ofs += sz) {
            if (value_is_float(vals[ii])) {
              float f;

              f = value_const_float_get(vals[ii]);
              if (VARIABLE_DEF_TYPE_FLOAT == variable_def_type_get(mbr_def)) {
                variable_const_float_set(stvar, dst_ofs, f);
              } else {
                variable_const_set(stvar, mbr_def, dst_ofs, f);
              }
            } else {
              variable_const_t n;

              n = value_const_get(vals[ii]);
              if (VARIABLE_DEF_TYPE_FLOAT == variable_def_type_get(mbr_def)) {
                variable_const_float_set(stvar, dst_ofs, n);
              } else {
                variable_const_set(stvar, mbr_def, dst_ofs, n);
              }
            }
          }
        } else {
          for (ii = 0, 
                 stdata = variable_data_get(stvar);
               ii < vals_ct;
               stdata += sz,
                 ii++) {
            memcpy(stdata, variable_data_get(value_variable_get(vals[ii])), sz);
            value_release(vals[ii]);
          }
        }
      }
    } else {
      size_t ii;
      size_t offset;

      stval = pfile_value_temp_get_from_def(pf, def);
      for (ii = 0, offset = 0;
           ii < vals_ct; 
           offset += mbr_sz, ii++) {
        value_t mbrval;
        value_t mbrofs;

        mbrval = value_alloc(value_variable_get(stval));
        value_def_set(mbrval, variable_def_member_def_get(mbr));
        mbrofs = pfile_constant_get(pf, offset, VARIABLE_DEF_NONE);
        value_baseofs_set(mbrval, mbrofs);
        pfile_cmd_op_add(pf, OPERATOR_ASSIGN, &mbrval, vals[ii], VALUE_NONE);
        value_release(mbrofs);
        value_release(mbrval);
        value_release(vals[ii]);
      }
    }
    FREE(vals);
  }
  return stval;
}

static value_t jal_parse_struct_init(pfile_t *pf, variable_def_t def)
{
  value_t stval;

  if (!pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, "{")) {
    stval = VALUE_NONE;
  } else {
    variable_def_member_t mbr;
    value_t              *vals;
    size_t                vals_sz; /* number allocated */
    size_t                vals_ct; /* number in use    */
    boolean_t             is_const;

    vals_ct = 0;
    vals_sz = 0;
    vals    = 0;

    is_const = BOOLEAN_TRUE;
    /* first, parse as many elements as there are */
    for (mbr = variable_def_member_get(def);
         mbr;
         mbr = variable_def_member_link_get(mbr)) {
      /* skip either '{' or ',' */
      pf_token_get(pf, PF_TOKEN_NEXT);
      if (vals_ct == vals_sz) {
        value_t *tmp;
        size_t   tmp_sz;

        tmp_sz = (vals_sz) ? 2 * vals_sz : 16;
        tmp    = REALLOC(vals, sizeof(*vals) * tmp_sz);
        if (tmp) {
          vals    = tmp;
          vals_sz = tmp_sz;
        }
      }
      stval = jal_parse_init(pf, variable_def_member_def_get(mbr));
      stval = jal_coerce_string_literal_array_to_scalar(pf,
        variable_def_member_def_get(mbr), stval);
      if (vals_ct < vals_sz) {
        vals[vals_ct] = stval;
        is_const &= value_is_const(vals[vals_ct]);
        if (variable_def_flag_test(def, VARIABLE_DEF_FLAG_CONST) &&
          !value_is_const(vals[vals_ct])) {
          pfile_log(pf, PFILE_LOG_ERR, "initializer %u must be constant",
            (unsigned) vals_ct + 1);
        }
        vals_ct++;
      } else {
        value_release(stval);
      }
      if (!pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, ",")) {
        break;
      }
    }
    if (variable_def_member_link_get(mbr)) {
      pfile_log(pf, PFILE_LOG_WARN, "fewer initializers than expected");
    }
    if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, "}")) {
      pf_token_get(pf, PF_TOKEN_NEXT);
    }
    /* OK, we've all of the values. was this const? */
    if (is_const) {
      size_t         ii;
      variable_t     stvar;
      variable_def_t stdef;
      char          *stdata;
      size_t         sz;

      stdef = jal_dup_def_and_set_const(pf, def);
      if (!stdef) {
        stval = VALUE_NONE;
      } else {
        stval = pfile_constant_get(pf, 0, stdef);
        stvar = value_variable_get(stval);

        for (ii = 0, 
               mbr = variable_def_member_get(def),
               stdata = variable_data_get(stvar);
             ii < vals_ct;
             stdata += sz,
               mbr = variable_def_member_link_get(mbr),
               ii++) {
          sz = variable_def_member_sz_get(mbr);
          memcpy(stdata, variable_data_get(value_variable_get(vals[ii])), sz);
          value_release(vals[ii]);
        }
      }
    } else {
      size_t ii;
      size_t offset;

      stval = pfile_value_temp_get_from_def(pf, def);
      for (ii = 0, offset = 0, mbr = variable_def_member_get(def); 
           ii < vals_ct; 
           offset += variable_def_member_sz_get(mbr),
             mbr = variable_def_member_link_get(mbr),
             ii++) {
        value_t mbrval;
        value_t mbrofs;

        mbrval = value_alloc(value_variable_get(stval));
        value_def_set(mbrval, variable_def_member_def_get(mbr));
        mbrofs = pfile_constant_get(pf, offset, VARIABLE_DEF_NONE);
        value_baseofs_set(mbrval, mbrofs);
        pfile_cmd_op_add(pf, OPERATOR_ASSIGN, &mbrval, vals[ii], VALUE_NONE);
        value_release(mbrofs);
        value_release(mbrval);
        value_release(vals[ii]);
      }
    }
    FREE(vals);
  }
  return stval;
}


static value_t jal_parse_init(pfile_t *pf, variable_def_t def)
{
  value_t val;

  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "{")) {
    if (VARIABLE_DEF_TYPE_STRUCTURE == variable_def_type_get(def)) {
      val = jal_parse_struct_init(pf, def);
    } else if (VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(def)) {
      val = jal_parse_array_init(pf, def);
    } else {
      val = VALUE_NONE;
    }
  } else if ((*pf_token_get(pf, PF_TOKEN_CURRENT) == '"') 
    && (VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(def))) {
    val = jal_parse_array_init(pf, def);
  } else {
    val = jal_parse_expr(pf);
  }
  return val;
}

/*
 * Parse the inside of `... pointer ... (` — the opening `(` is already consumed.
 * Handles empty (), (as var|const …), or procedure formals + optional return.
 */
static variable_def_t jal_pointer_parse_paren_body(pfile_t *pf,
  const pfile_pos_t *statement_start,
  variable_def_t ptr_base)
{
  variable_def_t proc_def;
  variable_def_t return_def;
  variable_t    *sig_params;
  size_t         sig_ct;
  size_t         sig_alloc;

  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, ")")) {
    pf_token_get(pf, PF_TOKEN_NEXT);
    proc_def = variable_def_alloc(0, VARIABLE_DEF_TYPE_FUNCTION, 0, 0);
    if (!proc_def) {
      pfile_log_syserr(pf, ENOMEM);
      return ptr_base;
    }
    return_def = VARIABLE_DEF_NONE;
    if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "return")) {
      pf_token_get(pf, PF_TOKEN_NEXT);
      return_def = jal_vdef_get(pf, VARIABLE_DEF_FLAG_NONE);
      if (return_def) {
        return_def = variable_def_flags_change(return_def,
          variable_def_flags_get_all(return_def) | VARIABLE_DEF_FLAG_OUT);
      }
    }
    (void) variable_def_member_insert(proc_def, 0, 0, return_def, 1);
    (void) variable_def_member_add(ptr_base, 0, proc_def, 1);
    return ptr_base;
  } else if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "as")) {
    /* Data / array-element pointer: var pointer p(as var byte) / (as const byte) */
    variable_def_t elem;
    boolean_t      pointee_const;

    pf_token_get(pf, PF_TOKEN_NEXT);
    pointee_const = BOOLEAN_FALSE;
    if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "var")) {
      pf_token_get(pf, PF_TOKEN_NEXT);
    } else if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "const")) {
      pf_token_get(pf, PF_TOKEN_NEXT);
      pointee_const = BOOLEAN_TRUE;
    } else {
      pfile_log(pf, PFILE_LOG_ERR,
          "expected 'var' or 'const' after 'as' in pointer signature "
          "(e.g. var pointer p(as var byte))");
    }
    elem = jal_vdef_get(pf, VARIABLE_DEF_FLAG_NONE);
    if (elem && pointee_const) {
      variable_def_t edup;

      edup = variable_def_dup(elem);
      if (edup) {
        elem = variable_def_flags_change(edup,
            variable_def_flags_get_all(edup) | VARIABLE_DEF_FLAG_CONST);
      }
    }
    if (elem
        && pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, ")")) {
      pf_token_get(pf, PF_TOKEN_NEXT);
      (void) variable_def_member_add(ptr_base, 0,
        variable_def_dup(elem), 1);
    }
    return ptr_base;
  }
  proc_def = variable_def_alloc(0, VARIABLE_DEF_TYPE_FUNCTION, 0, 0);
  if (!proc_def) {
    pfile_log_syserr(pf, ENOMEM);
    return ptr_base;
  }
  sig_params = 0;
  sig_ct     = 0;
  sig_alloc  = 0;
  jal_parse_formal_parameter_list_after_open_paren(pf, statement_start,
      proc_def, JAL_PARSE_PROCEDURE, &sig_params, &sig_ct, &sig_alloc,
      BOOLEAN_TRUE, BOOLEAN_FALSE);
  return_def = VARIABLE_DEF_NONE;
  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "return")) {
    pf_token_get(pf, PF_TOKEN_NEXT);
    return_def = jal_vdef_get(pf, VARIABLE_DEF_FLAG_NONE);
    if (return_def) {
      return_def = variable_def_flags_change(return_def,
        variable_def_flags_get_all(return_def) | VARIABLE_DEF_FLAG_OUT);
    }
  }
  (void) variable_def_member_insert(proc_def, 0, 0, return_def, 1);
  (void) variable_def_member_add(ptr_base, 0, proc_def, 1);
  while (sig_ct) {
    sig_ct--;
    variable_release(sig_params[sig_ct]);
  }
  FREE(sig_params);
  return ptr_base;
}

static variable_def_t jal_pointer_string_pointee_finalize(pfile_t *pf,
    variable_def_t elem, boolean_t pointee_const);

/*
 * After `var pointer <name> as`, parse pointee: data (var|const <type>),
 * or `procedure ( ... )`, or `function ( ... ) return <type>`.
 */
static variable_def_t jal_pointer_parse_pointee_after_as(pfile_t *pf,
  const pfile_pos_t *statement_start,
  variable_def_t ptr_base)
{
  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "procedure")) {
    pf_token_get(pf, PF_TOKEN_NEXT);
    if (!pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, "(")) {
      pfile_log(pf, PFILE_LOG_ERR,
          "expected '(' after 'procedure' in pointer declaration");
      return ptr_base;
    }
    pf_token_get(pf, PF_TOKEN_NEXT);
    return jal_pointer_parse_paren_body(pf, statement_start, ptr_base);
  }
  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "function")) {
    pf_token_get(pf, PF_TOKEN_NEXT);
    if (!pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, "(")) {
      pfile_log(pf, PFILE_LOG_ERR,
          "expected '(' after 'function' in pointer declaration");
      return ptr_base;
    }
    pf_token_get(pf, PF_TOKEN_NEXT);
    return jal_pointer_parse_paren_body(pf, statement_start, ptr_base);
  }
  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "var")
      || pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "const")) {
    boolean_t       pointee_const;
    variable_def_t elem;
    boolean_t       pointee_owned;

    pointee_const = pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "const");
    pf_token_get(pf, PF_TOKEN_NEXT);
    pointee_owned = BOOLEAN_FALSE;
    elem = jal_vdef_get(pf, VARIABLE_DEF_FLAG_NONE);
    if (elem && pointee_const) {
      variable_def_t edup;

      edup = variable_def_dup(elem);
      if (edup) {
        elem = variable_def_flags_change(edup,
            variable_def_flags_get_all(edup) | VARIABLE_DEF_FLAG_CONST);
        pointee_owned = BOOLEAN_TRUE;
      }
    }
    if (elem) {
      variable_def_t pre_fin;

      pre_fin = elem;
      elem = jal_pointer_string_pointee_finalize(pf, elem, pointee_const);
      if (!elem) {
        /* `finalize` may have freed `pre_fin` (e.g. const string without [N]). */
        pointee_owned = BOOLEAN_FALSE;
      } else if (elem != pre_fin) {
        pointee_owned = BOOLEAN_TRUE;
      }
      if (elem
          && (VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(elem))) {
        pfile_log(pf, PFILE_LOG_ERR, "invalid pointee type for data pointer");
      } else if (elem) {
        (void) variable_def_member_add(ptr_base, 0,
            variable_def_dup(elem), 1);
        if (pointee_owned) {
          variable_def_free(elem);
        }
      }
    }
    return ptr_base;
  }
  pfile_log(pf, PFILE_LOG_ERR,
      "expected var, const, procedure, or function after 'as' "
      "in pointer declaration (e.g. var pointer p as var byte)");
  return ptr_base;
}

/*
 * True if the variable's definition tree is or contains a POINTER (e.g.
 * `var pointer p as var byte`, `var pointer q as var byte[4]`). Used to default volatile.
 */
static boolean_t jal_vdef_tree_contains_pointer(variable_def_t vdef)
{
  variable_def_member_t m;

  if (!vdef) {
    return BOOLEAN_FALSE;
  }
  if (VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(vdef)) {
    return BOOLEAN_TRUE;
  }
  if (VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(vdef)
      || VARIABLE_DEF_TYPE_REFERENCE == variable_def_type_get(vdef)) {
    m = variable_def_member_get(vdef);
    if (m) {
      return jal_vdef_tree_contains_pointer(
          variable_def_member_def_get(m));
    }
  }
  return BOOLEAN_FALSE;
}

/*
 * RECORD members use pfile-owned type templates from jal_vdef_get; dup before
 * OR'ing volatile so shared defs are not corrupted.
 */
static variable_def_t jal_vdef_record_member_volatile_pointer(
    variable_def_t mdef)
{
  variable_def_t d;

  if (!mdef || !jal_vdef_tree_contains_pointer(mdef)) {
    return mdef;
  }
  d = variable_def_dup(mdef);
  if (!d) {
    return mdef;
  }
  return variable_def_flags_change(d,
      variable_def_flags_get_all(d) | VARIABLE_DEF_FLAG_VOLATILE);
}

static boolean_t jal_parse_dimension(pfile_t *pf, variable_ct_t *ct,
    boolean_t allow_empty, const char *const_msg)
{
  boolean_t seen;

  seen = BOOLEAN_FALSE;
  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "[")) {
    seen = BOOLEAN_TRUE;
    pf_token_get(pf, PF_TOKEN_NEXT);
    if (allow_empty && pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "]")) {
      if (ct) {
        *ct = VARIABLE_CT_UNKNOWN;
      }
    } else {
      value_t n;

      n = jal_parse_expr(pf);
      if (n) {
        if (!value_is_const(n)) {
          pfile_log(pf, PFILE_LOG_ERR, "%s", const_msg);
        } else if (value_const_get(n) < 1) {
          pfile_log(pf, PFILE_LOG_ERR, "dimension must be >= 1");
        } else if (ct) {
          variable_const_t  nv;
          variable_ct_t     du;

          nv = value_const_get(n);
          du = (variable_ct_t) nv;
          if (nv != (variable_const_t) du) {
            pfile_log(pf, PFILE_LOG_ERR, "dimension is too large");
          } else {
            *ct = du;
          }
        }
        value_release(n);
      }
    }
    if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, "]")) {
      pf_token_get(pf, PF_TOKEN_NEXT);
    }
  }
  return seen;
}

static boolean_t jal_parse_name_dimension(pfile_t *pf, variable_ct_t *ct)
{
  if (ct) {
    *ct = 0U;
  }
  return jal_parse_dimension(pf, ct, BOOLEAN_TRUE, "constant expected");
}

static boolean_t jal_parse_record_member_dimension(pfile_t *pf,
    variable_ct_t *ct)
{
  variable_ct_t tmp;
  boolean_t     seen;

  tmp  = 1U;
  seen = jal_parse_dimension(pf, &tmp, BOOLEAN_FALSE,
      "constant expression expected");
  if (ct) {
    *ct = tmp;
  }
  return seen;
}

static void jal_variable_def_dimension_apply(pfile_t *pf,
    jal_variable_info_t *inf, variable_ct_t ct, boolean_t allow_pointer_fold)
{
  variable_def_t adef;

  /* Only 0 = no […] in this call; `[]` (open) is VARIABLE_CT_UNKNOWN, not 0. */
  if (0U == ct) {
    return;
  }
  inf->def_flags &= ~VARIABLE_DEF_FLAG_SIGNED;
  if (allow_pointer_fold
      && (RESULT_OK == variable_def_pointer_fold_scalar_pointee_array(
          inf->vdef, ct))) {
    return;
  }
  adef = variable_def_alloc(0, VARIABLE_DEF_TYPE_ARRAY,
      inf->def_flags, 0);
  if (!adef) {
    pfile_log_syserr(pf, ENOMEM);
    return;
  }
  if (RESULT_OK != variable_def_member_add(adef, 0, inf->vdef, ct)) {
    variable_def_free(adef);
    pfile_log_syserr(pf, ENOMEM);
    return;
  }
  inf->vdef = adef;
}

static void jal_variable_def_dimensions_apply(pfile_t *pf,
    jal_variable_info_t *inf, const variable_ct_t *dims, size_t dim_ct,
    boolean_t allow_pointer_fold)
{
  size_t ii;

  if (!dim_ct) {
    return;
  }
  for (ii = dim_ct; ii > 0; ii--) {
    boolean_t fold;

    fold = allow_pointer_fold && (1 == dim_ct) && (ii == dim_ct);
    jal_variable_def_dimension_apply(pf, inf, dims[ii - 1], fold);
  }
}

/* Normalize `string` / `string[N]` pointee defs to match `var string` /
 * `var string[N]` (see `jal_parse_var_common`). */
static variable_def_t jal_pointer_string_pointee_finalize(pfile_t *pf,
    variable_def_t elem, boolean_t pointee_const)
{
  if (!elem
      || !variable_def_flag_test(elem, VARIABLE_DEF_FLAG_STRING)
      || (VARIABLE_DEF_TYPE_INTEGER != variable_def_type_get(elem))
      || (1 != variable_def_sz_get(elem))
      || variable_def_flag_test(elem, VARIABLE_DEF_FLAG_BIT)) {
    return elem;
  }
  {
    variable_ct_t  s_dims[8];
    size_t         s_dim_ct;
    variable_ct_t  s_dim;
    jal_variable_info_t s_inf;

    s_dim_ct = 0;
    while (jal_parse_name_dimension(pf, &s_dim)) {
      if (s_dim_ct < COUNT(s_dims)) {
        s_dims[s_dim_ct++] = s_dim;
      } else {
        pfile_log(pf, PFILE_LOG_ERR, "too many string dimensions");
      }
    }
    if (s_dim_ct) {
      jal_variable_info_init(&s_inf);
      s_inf.vdef = elem;
      s_inf.def_flags = variable_def_flags_get_all(elem);
      jal_variable_def_dimensions_apply(pf, &s_inf, s_dims, s_dim_ct,
          BOOLEAN_FALSE);
      return s_inf.vdef;
    }
    if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "[")) {
      pfile_log(pf, PFILE_LOG_ERR,
          "invalid or incomplete string pointee dimension "
          "(expected constant [N] after 'string')");
      variable_def_free(elem);
      return VARIABLE_DEF_NONE;
    }
    if (pointee_const) {
      pfile_log(pf, PFILE_LOG_ERR,
          "const string pointee requires a fixed capacity [N]");
      variable_def_free(elem);
      return VARIABLE_DEF_NONE;
    }
    {
      variable_def_t dyn;

      dyn = jal_string_dynamic_def_alloc(pf);
      variable_def_free(elem);
      if (dyn) {
        (void) jal_string_runtime_ensure(pf);
      }
      return dyn;
    }
  }
}

void jal_parse_var_common(pfile_t *pf, 
  const pfile_pos_t *statement_start, variable_t *dst, flag_t flags,
  boolean_t formal_name_optional, unsigned *formal_anon_idx)
{
  variable_def_t vdef;
  const char    *ptr;
  pf_token_get_t which;
  variable_ct_t  type_dims[8];
  size_t         type_dim_ct;
  variable_ct_t  type_dim;

  UNUSED(statement_start);
  which = PF_TOKEN_CURRENT;
  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "volatile")) {
    if (flags & VARIABLE_DEF_FLAG_CONST) {
      pfile_log(pf, PFILE_LOG_ERR, "volatile cannot be used here");
    } else {
      flags |= VARIABLE_DEF_FLAG_VOLATILE;
    }
    which = PF_TOKEN_NEXT;
  }
  ptr = pf_token_get(pf, which);
  vdef = jal_vdef_get(pf, flags);
  which = PF_TOKEN_CURRENT;
  flags |= variable_def_flags_get_all(vdef);
  type_dim_ct = 0;
  if (variable_def_flag_test(vdef, VARIABLE_DEF_FLAG_STRING)) {
    while (jal_parse_name_dimension(pf, &type_dim)) {
      if (type_dim_ct < COUNT(type_dims)) {
        type_dims[type_dim_ct++] = type_dim;
      } else {
        pfile_log(pf, PFILE_LOG_ERR, "too many string dimensions");
      }
    }
  }
  if ((flags & VARIABLE_DEF_FLAG_VOLATILE)
      && jal_vdef_tree_contains_pointer(vdef)) {
    pfile_log(pf, PFILE_LOG_ERR,
      "do not use 'volatile' with pointer types; they are volatile by default");
    flags &= ~VARIABLE_DEF_FLAG_VOLATILE;
  }
  do {
    /* parse a list of variable names */
    jal_variable_info_t inf;
    boolean_t           pointer_decl_by_as;
    boolean_t      pointer_name_dim_seen;
    variable_ct_t  pointer_name_dim;

    jal_variable_info_init(&inf);

    inf.vdef      = vdef;
    inf.def_flags = flags;
    pointer_decl_by_as = BOOLEAN_FALSE;
    pointer_name_dim_seen = BOOLEAN_FALSE;
    pointer_name_dim = 0U;

    if (pf_token_is(pf, which, PFILE_LOG_NONE, "volatile")) {
      if (jal_vdef_tree_contains_pointer(vdef)) {
        pfile_log(pf, PFILE_LOG_ERR,
          "do not use 'volatile' with pointer types; they are volatile by default");
      } else {
        inf.def_flags |= VARIABLE_DEF_FLAG_VOLATILE;
      }
      which = PF_TOKEN_NEXT;
    } else {
      which = PF_TOKEN_CURRENT;
    }

    /*
     * Pointers: var pointer <name> as var|const <elem-type>[N] (pointer to N elements)
     *   or as procedure ( ... ) / as function ( ... ) return <type>
     */
    if ((VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(vdef))
        && !variable_def_member_get(vdef)) {
      const char *pname;

      pname = pf_token_get(pf, PF_TOKEN_CURRENT);
      if (!jal_token_is_identifier(pf)) {
        pfile_log(pf, PFILE_LOG_ERR,
            "identifier expected after 'pointer' "
            "(e.g. var pointer p as var byte)");
      } else {
        char *pointer_name;

        /*
         * pname points at the lexer token buffer; it is overwritten by later
         * token reads (e.g. pointee type). Copy the identifier before consuming
         * further tokens.
         */
        pointer_name = STRDUP(pname);
        if (!pointer_name) {
          pfile_log_syserr(pf, ENOMEM);
        } else {
          pf_token_get(pf, PF_TOKEN_NEXT);
          pointer_name_dim_seen = jal_parse_name_dimension(pf,
              &pointer_name_dim);
          if (pointer_name
              && !pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, "as")) {
            pfile_log(pf, PFILE_LOG_ERR,
                "expected 'as' after pointer name or dimension "
                "(e.g. var pointer p as var byte, "
                "var pointer slots[4] as procedure(byte in))");
            FREE(pointer_name);
            pointer_name = 0;
          } else if (pointer_name) {
            variable_def_t ptr_sig;

            pf_token_get(pf, PF_TOKEN_NEXT);
            ptr_sig = variable_def_dup(vdef);
            if (!ptr_sig) {
              pfile_log_syserr(pf, ENOMEM);
              FREE(pointer_name);
            } else {
              inf.vdef = jal_pointer_parse_pointee_after_as(pf, statement_start,
                  ptr_sig);
              inf.name = pointer_name;
              which = PF_TOKEN_CURRENT;
              pointer_decl_by_as = BOOLEAN_TRUE;
            }
          }
        }
      }
    }

    if (dst) {
      if (pf_token_is(pf, which, PFILE_LOG_NONE, "in")) {
        inf.def_flags |= VARIABLE_DEF_FLAG_IN;
        which = PF_TOKEN_NEXT;
      } else {
        which = PF_TOKEN_CURRENT;
      }
      if (pf_token_is(pf, which, PFILE_LOG_NONE, "out")) {
        inf.def_flags |= VARIABLE_DEF_FLAG_OUT;
        which = PF_TOKEN_NEXT;
      } else {
        which = PF_TOKEN_CURRENT;
      }
      if (pf_token_is(pf, which, PFILE_LOG_NONE, "byref")) {
        inf.def_flags |= VARIABLE_DEF_FLAG_BYREF;
        which = PF_TOKEN_NEXT;
      } else {
        which = PF_TOKEN_CURRENT;
      }
      if (inf.def_flags & VARIABLE_DEF_FLAG_BYREF) {
        if (inf.def_flags & (VARIABLE_DEF_FLAG_IN | VARIABLE_DEF_FLAG_OUT)) {
          pfile_log(pf, PFILE_LOG_ERR, 
            "byref cannot be combined with in or out");
          inf.def_flags &= ~(VARIABLE_DEF_FLAG_IN | VARIABLE_DEF_FLAG_OUT);
        }
        if (inf.def_flags & VARIABLE_DEF_FLAG_VOLATILE) {
          pfile_log(pf, PFILE_LOG_ERR,
            "byref cannot be combined with volatile");
          inf.def_flags &= ~VARIABLE_DEF_FLAG_VOLATILE;
        }
      } else if (!(inf.def_flags 
        & (VARIABLE_DEF_FLAG_IN | VARIABLE_DEF_FLAG_OUT))) {
        pfile_log(pf, PFILE_LOG_ERR, "in and/or out required");
      }
    }

    if (!pointer_decl_by_as) {
      ptr = pf_token_get(pf, which);
      if (jal_token_is_identifier(pf)) {
        jal_identifier_is_reserved(pf);
        inf.name = STRDUP(pf_token_get(pf, PF_TOKEN_CURRENT));
        if (!inf.name) {
          pfile_log_syserr(pf, ENOMEM);
        }
        pf_token_get(pf, PF_TOKEN_NEXT);
      } else if (dst && formal_name_optional && formal_anon_idx
          && (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, ")")
              || pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, ","))) {
        char anon_buf[24];

        sprintf(anon_buf, "_p%u", (*formal_anon_idx)++);
        inf.name = STRDUP(anon_buf);
        if (!inf.name) {
          pfile_log_syserr(pf, ENOMEM);
        }
      } else {
        pfile_log(pf, PFILE_LOG_ERR, PFILE_MSG_VARIABLE_EXPECTED);
      }
    }
    if (inf.name) {
      /* encapsulate everything needed to create this variable */
      variable_t     var;
      variable_ct_t  dims_st[8];
      variable_ct_t *dims;
      size_t         dims_cap;
      size_t         dim_ct;
      variable_ct_t  dim;
      boolean_t      dims_heap;

      var       = VARIABLE_NONE;
      dims      = dims_st;
      dims_cap  = COUNT(dims_st);
      dims_heap = BOOLEAN_FALSE;
      dim_ct    = 0;
      while (jal_parse_name_dimension(pf, &dim)) {
        if (dim_ct == dims_cap) {
          /* any dimension count parses (an 8-bit n included): grow the
           * list instead of erroring past the initial stack slots */
          size_t         ncap;
          variable_ct_t *nd;

          ncap = dims_cap * 2U;
          nd   = malloc(ncap * sizeof(*nd));
          if (nd) {
            memcpy(nd, dims, dim_ct * sizeof(*nd));
            if (dims_heap) {
              free(dims);
            }
            dims      = nd;
            dims_cap  = ncap;
            dims_heap = BOOLEAN_TRUE;
          } else {
            pfile_log_syserr(pf, ENOMEM);
            break;
          }
        }
        dims[dim_ct++] = dim;
      }
      if (dim_ct) {
        /* an array */
        /*
         * `var pointer name as var byte[N]` leaves `[N]` here: same fold.
         * Also `var pointer const byte p[N]` / `var pointer var byte q[N]`.
         * One pointer, pointee is array-of-N of that element (pointer-to-array).
         * Not folded when the pointee is already an array, a nested data
         * pointer, or a procedure/function signature (array of N of those).
         */
        jal_variable_def_dimensions_apply(pf, &inf, dims, dim_ct,
            BOOLEAN_TRUE);
        inf.ct = 0;
      }
      if (dims_heap) {
        free(dims);
      }
      if (type_dim_ct) {
        jal_variable_def_dimensions_apply(pf, &inf, type_dims, type_dim_ct,
            BOOLEAN_FALSE);
      } else if (variable_def_flag_test(inf.vdef, VARIABLE_DEF_FLAG_STRING)
          && !dim_ct) {
        if (inf.def_flags & VARIABLE_DEF_FLAG_CONST) {
          /* bare `const string S = "..."` is a compile-time flash string,
           * not a heap handle: open byte-count STRING array, sized from
           * the literal by the initializer's array-init path (the sized
           * `const string[N]` form already worked this way) */
          inf.vdef = jal_string_const_def_alloc();
          if (!inf.vdef) {
            if (!pfile_errct_get(pf)) {
              pfile_log_syserr(pf, ENOMEM);
            }
          }
        } else {
          inf.vdef = jal_string_dynamic_def_alloc(pf);
          if (!inf.vdef) {
            if (!pfile_errct_get(pf)) {
              pfile_log_syserr(pf, ENOMEM);
            }
          } else {
            (void) jal_string_runtime_ensure(pf);
          }
        }
      }
      if (pointer_name_dim_seen) {
        /*
         * `var pointer slots[N] as ...` means N pointer slots sharing the
         * parsed pointee signature. This is deliberately an outer ARRAY of
         * POINTERs; it is not the scalar-pointee fold used by
         * `var pointer p as var byte[N]`.
         */
        jal_variable_def_dimension_apply(pf, &inf, pointer_name_dim,
            BOOLEAN_FALSE);
      }
      /*
       * All `var … pointer …` and `const … pointer …` variables are volatile by
       * default: indirect loads/stores must not be optimized away (PIC14
       * literal-through-pointer, etc.). Do not write `volatile` on pointer
       * types (parser error). `const` still rejects `volatile` before the type.
       * Not combined with byref (parser rejects volatile+byref).
       */
      if (!(inf.def_flags & VARIABLE_DEF_FLAG_BYREF)
          && jal_vdef_tree_contains_pointer(inf.vdef)) {
        inf.def_flags |= VARIABLE_DEF_FLAG_VOLATILE;
      }
      if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "shared")) {
        inf.var_flags = VARIABLE_FLAG_SHARED;
        pf_token_get(pf, PF_TOKEN_NEXT);
      }
      if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "at")) {
        /* a placed variable */
        if (pf_token_is(pf, PF_TOKEN_NEXT, PFILE_LOG_NONE, "{")) {
          /* 
           * this is a variable mirrored in multiple places
           */
          size_t ii;

          for (ii = 0; 
               (ii < VARIABLE_MIRROR_CT)
               && (!ii 
                 || pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, ",")); 
                 ii++) {
            /* skip the "{" or "," */
            value_t n;

            pf_token_get(pf, PF_TOKEN_NEXT);
            n = jal_parse_expr(pf);
            if (n) {
              if (!value_is_const(n)) {
                pfile_log(pf, PFILE_LOG_ERR, PFILE_MSG_CONSTANT_EXPECTED);
              } else {
                inf.base[inf.base_ct] = value_const_get(n);
                inf.base_ct++;
              }
              value_release(n);
            }
          }
          if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, "}")) {
            pf_token_get(pf, PF_TOKEN_NEXT);
          }
        } else {
          ptr    = pf_token_get(pf, PF_TOKEN_CURRENT);
          inf.master = pfile_variable_find(pf, PFILE_LOG_NONE, ptr, 0);
          variable_flag_set(inf.master, VARIABLE_FLAG_UNREPLACEABLE);
          /* if this is a bit variable, the master will be set to
             _bit_bucket. We need to remove that */
          if (inf.master && !variable_is_const(inf.master)) {
            /* this is AT variable [: bitpos] */
            value_t tmp;

            pf_token_get(pf, PF_TOKEN_NEXT);
            inf.base[0] = 0;
            inf.base_ct = 1;

            tmp = value_alloc(inf.master);
            while (jal_parse_subscript(pf, &tmp, NULL)
              || jal_parse_structure(pf, &tmp))
              ; /* empty loop */
            if (value_baseofs_get(tmp)) {
              if (!value_is_const(value_baseofs_get(tmp))) {
                pfile_log(pf, PFILE_LOG_ERR, "constant expression expected");
              }
              inf.base[0] = value_const_get(value_baseofs_get(tmp));
            }
            value_release(tmp);
          } else {
            /* this is AT cexpr [ : bitpos] */
            value_t n;

            /* 
             * protect against the following:
             *    const bubba = 0x400
             *    var word xx at bubba
             */
            variable_release(inf.master);
            inf.master = VARIABLE_NONE;

            n = jal_parse_expr(pf);
            if (n) {
              if (!value_is_const(n)) {
                pfile_log(pf, PFILE_LOG_ERR, PFILE_MSG_CONSTANT_EXPECTED);
              } else {
                inf.base[0] = value_const_get(n);
                inf.base_ct = 1;
              }
              value_release(n);
            }
          }
          if (variable_def_flag_test(vdef, VARIABLE_DEF_FLAG_BIT)
            && pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, ":")) {
            value_t n;

            pf_token_get(pf, PF_TOKEN_NEXT);
            n = jal_parse_expr(pf);
            if (n) {
              if (!value_is_const(n)) {
                pfile_log(pf, PFILE_LOG_ERR, PFILE_MSG_CONSTANT_EXPECTED);
              } else {
                inf.bit = value_const_get(n);
              }
              value_release(n);
            }
          } else if (!variable_def_flag_test(vdef, VARIABLE_DEF_FLAG_BIT)
            && pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "+")) {
            value_t n;

            pf_token_get(pf, PF_TOKEN_NEXT);
            n = jal_parse_expr(pf);
            if (n) {
              if (!value_is_const(n)) {
                pfile_log(pf, PFILE_LOG_ERR, PFILE_MSG_CONSTANT_EXPECTED);
              } else {
                inf.base[0] = value_const_get(n);
              }
              value_release(n);
            }
          }
        }
      } else if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "is")) {
        /* an alias */
#if 0
        pfile_log(pf, PFILE_LOG_WARN, 
          "'IS' has been deprecated, please use `ALIAS' instead");
#endif
        pf_token_get(pf, PF_TOKEN_NEXT);
        inf.master = jal_parse_var_alias(pf);
        if (inf.master) {
          if (!variable_def_is_same(inf.vdef, 
              variable_def_get(inf.master))) {
            pfile_log(pf, PFILE_LOG_ERR, "type mismatch");
          }
          inf.var_flags = VARIABLE_FLAG_ALIAS;
          variable_lock(inf.master);
        }
      }
      if (!vdef
            || pf_token_is(pf, PF_TOKEN_CURRENT,
                 (flags & VARIABLE_DEF_FLAG_CONST)
                  ? PFILE_LOG_ERR : PFILE_LOG_NONE, "=")) {
        value_t sval;
        value_t dval;
        boolean_t dyn_str_init;

        /* pre-assigned */
        pf_token_get(pf, PF_TOKEN_NEXT);
        /* dynamic-string initializer: parse as a full expression. The
         * handle def is a 3-field byte array, so the array-init path
         * would clip the literal to the handle size and warn "more
         * initializers than expected"; an expression keeps the whole
         * literal and also admits `&` chains ("a" & x & "b" evaluates
         * into the chain heap temp the runtime assign copies from) */
        dyn_str_init = variable_def_flag_test(inf.vdef,
            VARIABLE_DEF_FLAG_STRING_DYNAMIC);
        /* bare `const string S = <expr>`: the open-size const form also
         * takes a full expression (a cast operand like dec(42), or a
         * bare literal); the unknown member count sizes itself from
         * the value below. Sized `const string[N]` keeps the
         * array-init path (literal clipping semantics) */
        if (!dyn_str_init
          && (inf.def_flags & VARIABLE_DEF_FLAG_CONST)
          && variable_def_flag_test(inf.vdef, VARIABLE_DEF_FLAG_STRING)
          && (VARIABLE_CT_UNKNOWN == variable_def_member_ct_get(
              variable_def_member_get(inf.vdef)))) {
          dyn_str_init = 2;
        }
        sval = dyn_str_init ? jal_parse_expr(pf)
          : jal_parse_init(pf, inf.vdef);
        inf.ct = 0;
        if ((VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(inf.vdef))
          && (VARIABLE_CT_UNKNOWN == variable_def_member_ct_get(
            variable_def_member_get(inf.vdef)))) {
          variable_def_member_ct_set(inf.vdef,
            variable_def_member_get(inf.vdef),
            variable_def_member_ct_get(variable_def_member_get(
              value_def_get(sval))));
        }
        var = jal_variable_alloc(pf, &inf, dst != 0, 0, 0, 
            PFILE_VARIABLE_ALLOC_LOCAL);
        dval = value_alloc(var);
        if (value_is_const(dval)) {
          if (!value_is_const(sval)) {
            if (jal_const_var_whereis_metadata_set(var, sval)) {
              /* WHEREIS relocatable constant: resolved by variable_const_get(). */
            } else {
              pfile_log(pf, PFILE_LOG_ERR, "constant expected");
            }
          } else if (value_is_float(sval) || value_is_float(dval)) {
            if (value_is_float(sval)) {
              float f;

              f = value_const_float_get(sval);
              if (value_is_float(dval)) {
                value_const_float_set(dval, f);
              } else {
                value_const_set(dval, f);
              }
            } else {
              variable_const_t n;

              n = value_const_get(sval);
              if (value_is_float(dval)) {
                if (value_is_signed(sval)) {
                  value_const_float_set(dval, (long) n);
                } else {
                  value_const_float_set(dval, n);
                }
              } else {
                value_const_set(dval, n);
              }
            }
          } else {
            const char *src_data;
            char       *dst_data;

            src_data = variable_data_get(value_variable_get(sval));
            dst_data = variable_data_get(value_variable_get(dval));
            if (variable_def_is_same(value_def_get(dval), 
              value_def_get(sval))) {
              /* trivial case, simply copy */
              if (src_data && dst_data) {
                memcpy(dst_data, src_data, value_sz_get(dval));
              }
            } else if (value_is_array(dval) && value_is_array(sval)) {
              /*
               * can assign arrays of unequal length; for integers
               * conversion might be required
               */
              if (!variable_def_member_is_same(
                variable_def_member_get(value_def_get(sval)), 
                variable_def_member_get(value_def_get(dval)))) {
              }
            } else if (value_is_number(sval) && value_is_number(dval)) {
              /*
               * conversion required here
               */
              value_const_set(dval, value_const_get(sval));
            } else if (value_is_array(sval) && (1 == value_sz_get(sval)) && value_is_number(dval)) {
                /* If the source is a single character (array) element then this is still a valid
                   number. */
                value_const_set(dval, value_const_get(sval));
            } else {
              pfile_log(pf, PFILE_LOG_ERR, "type mismatch");
            }
          }
        } else {
          if (dyn_str_init && jal_string_is_dynamic(dval)) {
            /* zero the fresh handle before the runtime assign: classic
             * cores do not zero RAM, and a power-on CAP >= len would
             * make the assign copy through a garbage block pointer.
             * Declaration only -- statement assigns rely on CAP for
             * in-place block reuse */
            jal_string_handle_zero_emit(pf, dval);
          }
          if (!jal_string_assignment_emit(pf, dval, sval)) {
            /* dynamic string assignment lowered to runtime heap p-code
             * when it applies; otherwise the generic assign */
            pfile_cmd_op_add(pf, OPERATOR_ASSIGN, &dval, sval, VALUE_NONE);
          }
        }
        value_release(sval);
        value_release(dval);
        if (0 != dst) {
          pfile_log(pf, PFILE_LOG_ERR, "Default parameters are not allowed");
        }
      } else {
        if ((-1U == inf.ct) && !dst) {
          pfile_log(pf, PFILE_LOG_ERR,
            "Flexible arrays can only be used as parameters");
        }
        var = jal_variable_alloc(pf, &inf, dst != 0, 0, 0, 
            PFILE_VARIABLE_ALLOC_LOCAL);
#if 0
        if (variable_master_get(var)) {
          variable_flag_set(variable_master_get(var), 
            VARIABLE_FLAG_UNREPLACEABLE);
        }
#endif
      }
      if (dst) {
        *dst = var;
      } else {
        variable_release(var);
      }
    }
    /* Clean up allocated memory for variable name */
    jal_variable_info_cleanup(&inf);
    which = PF_TOKEN_NEXT;
  } while (!dst && pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, ","));
}

void jal_parse_var(pfile_t *pf, const pfile_pos_t *statement_start)
{
  /* task bodies run on leaf frames on the starter's stack: locals of
   * different tasks (and of the starter) overlap, so storage declared
   * here silently collides -- tasks must use globals */
  if (pfile_proc_flag_test(pfile_proc_active_get(pf),
        PFILE_PROC_FLAG_TASK)
      && !pic_task_locals_allowed(pf)) {
    /* classic cores share one frame area across procs, so a
     * suspended task's locals collide with every other task's and
     * main's; the 32-bit linear allocators give each local a unique
     * address, where task bodies may declare variables
     * (fix_task_locals_lift -- runtime-proven on ESP32 + PIC32) */
    pfile_log(pf, PFILE_LOG_ERR,
        "variables cannot be declared inside a task body on this "
        "target (tasks share one stack; declare a global instead)");
  }
  jal_parse_var_common(pf, statement_start, 0, VARIABLE_DEF_FLAG_NONE,
      BOOLEAN_FALSE, 0);
}

void jal_parse_const(pfile_t *pf, const pfile_pos_t *statement_start)
{
  jal_parse_var_common(pf, statement_start, 0, VARIABLE_DEF_FLAG_CONST,
      BOOLEAN_FALSE, 0);
}

/*
 * RECORD name IS
 *    type identifier[,...]
 *    ...
 * END RECORD
 */
void jal_parse_record(pfile_t *pf, const pfile_pos_t *start)
{
  UNUSED(start);
  if (!jal_token_is_identifier(pf)) {
    pfile_log(pf, PFILE_LOG_ERR, "identifier expected");
  } else {
    variable_def_t def;

    def = variable_def_alloc(pf_token_get(pf, PF_TOKEN_CURRENT),
        VARIABLE_DEF_TYPE_STRUCTURE, VARIABLE_DEF_FLAG_NONE, 0);
    if (pf_token_is(pf, PF_TOKEN_NEXT, PFILE_LOG_ERR, "is")) {
      pf_token_get(pf, PF_TOKEN_NEXT);
    }
    do {
      boolean_t      first;
      variable_def_t mdef;

      first = BOOLEAN_TRUE;
      mdef = jal_vdef_get(pf, VARIABLE_DEF_FLAG_NONE);
      do {
        if (first) {
          first = BOOLEAN_FALSE;
        } else {
          pf_token_get(pf, PF_TOKEN_NEXT);
        }
        if (!jal_token_is_identifier(pf)) {
          pf_token_get(pf, PF_TOKEN_NEXT);
        } else {
          char       *tag;
          const char *ptr;
          variable_ct_t ct;

          ptr = pf_token_get(pf, PF_TOKEN_CURRENT);
          tag = MALLOC(1 + strlen(ptr));
          if (!tag) {
            pfile_log_syserr(pf, ENOMEM);
          } else {
            strcpy(tag, ptr);
          }
          ct  = 1U;
          pf_token_get(pf, PF_TOKEN_NEXT);
          if (jal_parse_record_member_dimension(pf, &ct)) {
            if (tag && mdef) {
              variable_def_t adef;
              variable_def_t muse;

              adef = variable_def_alloc(tag, VARIABLE_DEF_TYPE_ARRAY,
                  VARIABLE_DEF_FLAG_NONE, 0);
              muse = jal_vdef_record_member_volatile_pointer(mdef);
              variable_def_member_add(adef, 0, muse, ct);
              variable_def_member_add(def, tag, adef, 1);
            }
          } else {
            if (tag && mdef) {
              variable_def_t muse;

              muse = jal_vdef_record_member_volatile_pointer(mdef);
              variable_def_member_add(def, tag, muse, ct);
            }
          }
          FREE(tag);
        }
      } while (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, ","));
    } while (!pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "end"));
    if (pf_token_is(pf, PF_TOKEN_NEXT, PFILE_LOG_ERR, "record")) {
      pf_token_get(pf, PF_TOKEN_NEXT);
    }
    if (variable_def_sz_get(def)) {
      pfile_variable_def_add(pf, def);
    }
  }
}

