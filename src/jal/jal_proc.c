/**********************************************************
 **
 ** jal_proc.c : JAL proc parsing
 **
 ** Copyright (c) 2005, Kyle A. York
 **               2021..2021, Rob Jansen
 **
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: fork extensions (heavy rework)
 **
 ***********************************************************/
#include <errno.h>
#include <string.h>
#include <assert.h>
#include "../libutils/mem.h"
#include "../libcore/cmd_brch.h"
#include "../libcore/pf_msg.h"
#include "../libcore/pf_cmd.h"
#include "../libcore/pfile.h"
#include "../libcore/pf_expr.h"
#include "../libcore/pf_proc.h"
#include "../libcore/vardef.h"
#include "jal_vdef.h"
#include "jal_blck.h"
#include "jal_expr.h"
#include "jal_proc.h"
#include "jal_string.h"

/* ===================== optional parameters (2026-10-07) =====================
 * `procedure p(byte in x, optional word in y default 500) is ...`
 * The default is a constant expression typed by the formal's def; open
 * arrays may default to a string literal (or brace const list), stored
 * materialized once.  Calls with omitted trailing arguments are filled
 * CALLER-SIDE in jal_parse_call from this registry -- the marshalling
 * path sees an ordinary full-arity call (zero codegen change).
 *
 * Defaults are indexed by the proc_def MEMBER position (parms index at
 * the call site): registration resolves each declared param's member by
 * tag match, so the function-return slot and hidden _<name>_count
 * members need no special casing at fill time.  Direct calls only --
 * pointer-called procs keep the full-arity rule (signatures carry no
 * defaults). */
typedef struct jal_opt_default_ {
  variable_def_t           fn_def;
  value_t                 *defs;      /* [params_ct] by DECLARED ordinal */
  size_t                   params_ct;
  value_t                 *by_member; /* resolved lazily at first call */
  size_t                   member_ct;
  struct jal_opt_default_ *link;
} jal_opt_default_t;

static jal_opt_default_t *jal_opt_defaults;

static jal_opt_default_t *jal_opt_default_find(variable_def_t fn_def)
{
  jal_opt_default_t *entry;

  for (entry = jal_opt_defaults; entry; entry = entry->link) {
    if (entry->fn_def == fn_def) {
      return entry;
    }
  }
  return 0;
}

/* Resolve declared-param ordinals to member indices at FIRST CALL, when
 * the function def is final (the return slot is inserted at member 0
 * AFTER the parameter list parses, so declaration-time indexing is off
 * by one).  The count-pair rule mirrors the argument loop's need_ct
 * detection exactly (INTEGER member whose next member is a POINTER =
 * hidden _<name>_count for an open-array formal). */
static void jal_opt_default_resolve(pfile_t *pf, jal_opt_default_t *entry)
{
  variable_def_member_t mbr;
  size_t                member_ct;
  size_t                midx;
  size_t                ordinal;

  if (entry->by_member) {
    return;
  }
  member_ct = 0;
  for (mbr = variable_def_member_get(entry->fn_def); mbr;
      mbr = variable_def_member_link_get(mbr)) {
    member_ct++;
  }
  entry->by_member = CALLOC(member_ct, sizeof(*entry->by_member));
  entry->member_ct = member_ct;
  if (!entry->by_member) {
    pfile_log_syserr(pf, ENOMEM);
    return;
  }
  mbr = variable_def_member_link_get(variable_def_member_get(entry->fn_def));
  midx    = 1;    /* member 0 is the return/placeholder slot */
  ordinal = 0;
  while (mbr && (ordinal < entry->params_ct)) {
    variable_def_member_t nxt;

    nxt = variable_def_member_link_get(mbr);
    if (nxt
        && (VARIABLE_DEF_TYPE_INTEGER
          == variable_def_type_get(variable_def_member_def_get(mbr)))
        && (VARIABLE_DEF_TYPE_POINTER
          == variable_def_type_get(variable_def_member_def_get(nxt)))) {
      /* hidden count for the open-array param -- member order is
       * [_<name>_count, pointer] (the argument loop fills parms[ii-1]
       * for a present actual), so the count comes FIRST */
      if (entry->defs[ordinal]) {
        value_t ctv;

        ctv = pfile_constant_get(pf, value_ct_get(entry->defs[ordinal]),
            VARIABLE_DEF_NONE);
        if (ctv) {
          value_lock(ctv);
          entry->by_member[midx] = ctv;
        }
        value_lock(entry->defs[ordinal]);
        entry->by_member[midx + 1] = entry->defs[ordinal];
      }
      ordinal += 1;
      midx    += 2;
      mbr      = variable_def_member_link_get(nxt);
    } else {
      if (entry->defs[ordinal]) {
        value_t dv;

        dv = entry->defs[ordinal];
        /* DYNAMIC-string formal: the raw const-literal default would be
         * read AS a heap handle by the callee (COUNT = const-table
         * bytes). Stage it once into a compiler string cell and
         * register the cell -- every omitted call passes a real
         * handle ("materialized once", like open-array defaults). */
        if (variable_def_flag_test(variable_def_member_def_get(mbr),
            VARIABLE_DEF_FLAG_STRING)
            && variable_def_flag_test(variable_def_member_def_get(mbr),
              VARIABLE_DEF_FLAG_STRING_DYNAMIC)
            && !jal_string_is_dynamic(dv)
            && jal_string_is_source(dv)) {
          value_t cell = jal_string_stage_source_cell(pf, dv);

          if (cell) {
            dv = cell;
          }
        }
        value_lock(dv);
        entry->by_member[midx] = dv;
      }
      ordinal += 1;
      midx    += 1;
      mbr      = nxt;
    }
  }
}

/* register per-param defaults (defs[i], VALUE_NONE = required) against
 * the assembled proc_def; ownership of each non-NONE def value passes
 * to the registry (member indices resolve lazily at first call) */
static void jal_opt_default_register(pfile_t *pf, variable_def_t proc_def,
  value_t *defs, size_t param_ct)
{
  jal_opt_default_t *entry;
  size_t             ii;

  if (jal_opt_default_find(proc_def)) {
    for (ii = 0; ii < param_ct; ii++) {
      value_release(defs[ii]);
    }
    return;
  }
  entry = MALLOC(sizeof(*entry));
  if (!entry) {
    pfile_log_syserr(pf, ENOMEM);
    for (ii = 0; ii < param_ct; ii++) {
      value_release(defs[ii]);
    }
    return;
  }
  entry->fn_def     = proc_def;
  entry->params_ct  = param_ct;
  entry->by_member  = 0;
  entry->member_ct  = 0;
  entry->defs       = defs;   /* ownership passes */
  entry->link       = jal_opt_defaults;
  jal_opt_defaults  = entry;
}

/* Formal is a data pointer (not a procedure/function pointer). */
static boolean_t jal_formal_def_is_data_pointer_non_callable(
  variable_def_t def)
{
  variable_def_member_t pm;
  variable_def_t        inner;

  if (!def || VARIABLE_DEF_TYPE_POINTER != variable_def_type_get(def)) {
    return BOOLEAN_FALSE;
  }  pm = variable_def_member_get(def);
  inner = pm ? variable_def_member_def_get(pm) : VARIABLE_DEF_NONE;
  if (!inner) {
    return BOOLEAN_FALSE;
  }
  return VARIABLE_DEF_TYPE_FUNCTION != variable_def_type_get(inner);
}

/*
 * PROCEDURE name [( type [volatile] [in] [out] name [ = expr ] ) IS ... 
 *   END PROCEDURE
 * TASK name [( [type [volatile] in name[,...]] ) IS ... 
 *   END TASK
 * FUNCTION name [( type [volatile] [in] [out] name [ = expr ] ) RETURN type 
 *   IS ... END FUNCTION
 */    

/* allocate a function reference, return type is ret,
 * parameter type is parm */
static variable_def_t jal_variable_fn_def_alloc(pfile_t *pf,
  variable_def_t ret, variable_def_t parm, flag_t fn_flags)
{
  variable_def_t base;
  variable_def_t def;

  base = variable_def_alloc(0, VARIABLE_DEF_TYPE_POINTER,
      VARIABLE_DEF_FLAG_IN, pfile_pointer_width_get(pf));
  def  = variable_def_alloc(0, VARIABLE_DEF_TYPE_FUNCTION, fn_flags, 0);
  variable_def_member_add(base, 0, def, 1);
  if (ret) {
    ret = variable_def_flags_change(ret, VARIABLE_DEF_FLAG_OUT);
  }
  variable_def_member_add(def, 0, ret, 1);
  if (parm) {
    parm = variable_def_flags_change(parm, 
      (variable_def_flags_get_all(parm) & ~VARIABLE_DEF_FLAG_OUT)
      | VARIABLE_DEF_FLAG_IN);
    variable_def_member_add(def, 0, parm, 1);
  }
  return base;
}

void jal_parse_formal_parameter_list_after_open_paren(pfile_t *pf,
  const pfile_pos_t *statement_start,
  variable_def_t proc_def,
  jal_parse_proc_type type,
  variable_t **params,
  size_t *params_ct,
  size_t *params_alloc,
  boolean_t formal_name_optional,
  boolean_t allow_optional)
{
  unsigned formal_anon_idx;
  value_t *opt_defs;
  size_t   opt_alloc;

  formal_anon_idx = 0;
  opt_defs = 0;
  opt_alloc = 0;
  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, ")")) {
    pf_token_get(pf, PF_TOKEN_NEXT);
    if (opt_defs) {
      FREE(opt_defs);
    }
    return;
  } else {
    boolean_t is_first;
    boolean_t seen_optional;

    is_first = BOOLEAN_TRUE;
    seen_optional = BOOLEAN_FALSE;
    do {
      variable_t var;
      flag_t     flags;
      boolean_t  is_optional;

      if (is_first) {
        is_first = BOOLEAN_FALSE;
      } else {
        pf_token_get(pf, PF_TOKEN_NEXT);
      }
      /* `optional` is a contextual keyword in param-list position only;
       * pseudo-variable routines ('get/'put) and other non-user forms
       * must not carry defaults (user rule 2026-10-07) */
      is_optional = BOOLEAN_FALSE;
      if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "optional")) {
        is_optional = BOOLEAN_TRUE;
        pf_token_get(pf, PF_TOKEN_NEXT);
        if (!allow_optional) {
          pfile_log(pf, PFILE_LOG_ERR,
              "optional parameters are not allowed here");
        }
      }
      if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "volatile")) {
        flags = VARIABLE_DEF_FLAG_VOLATILE;
        pf_token_get(pf, PF_TOKEN_NEXT);
      } else {
        flags = VARIABLE_DEF_FLAG_NONE;
      }
      if (!is_optional
          && pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "optional")) {
        /* also accept volatile-then-optional ordering */
        is_optional = BOOLEAN_TRUE;
        pf_token_get(pf, PF_TOKEN_NEXT);
        if (!allow_optional) {
          pfile_log(pf, PFILE_LOG_ERR,
              "optional parameters are not allowed here");
        }
      }
      if (!is_optional && seen_optional) {
        pfile_log(pf, PFILE_LOG_ERR,
            "optional parameters must come after all required parameters");
      }
      if (is_optional) {
        seen_optional = BOOLEAN_TRUE;
      }
      var = VARIABLE_NONE;
      jal_parse_var_common(pf, statement_start, &var, flags,
          formal_name_optional,
          (formal_name_optional ? &formal_anon_idx : 0));
      {
        value_t dval;

        dval = VALUE_NONE;
        if (is_optional) {
          variable_def_t pdef;

          pdef = variable_def_get(var);
          if (variable_dflag_test(var, VARIABLE_DEF_FLAG_OUT)) {
            pfile_log(pf, PFILE_LOG_ERR,
              "default is only allowed on IN parameters");
          }
          if (!pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE,
              "default")) {
            pfile_log(pf, PFILE_LOG_ERR,
              "default expected after optional parameter");
          } else {
            pf_token_get(pf, PF_TOKEN_NEXT);
            dval = jal_parse_expr(pf);
            if (!dval || !value_is_const(dval)) {
              pfile_log(pf, PFILE_LOG_ERR,
                "default must be a constant expression");
              value_release(dval);
              dval = VALUE_NONE;
            } else if ((VARIABLE_DEF_TYPE_POINTER
                  == variable_def_type_get(pdef))
                || (VARIABLE_DEF_TYPE_ARRAY
                  == variable_def_type_get(pdef))) {
              /* open-array formal (still an ARRAY def here -- the
               * pointer+hidden-count rewrite happens after this check):
               * the default is the materialized literal (string or
               * brace const list); member defs must match like an
               * array-to-pointer actual */
              if (!value_is_array(dval)
                  || !variable_def_is_same(
                      variable_def_member_def_get(
                        variable_def_member_get(pdef)),
                      variable_def_member_def_get(
                        variable_def_member_get(value_def_get(dval))))) {
                pfile_log(pf, PFILE_LOG_ERR,
                  "default for an open-array parameter must be a"
                  " string or constant list of matching elements");
                value_release(dval);
                dval = VALUE_NONE;
              }
            } else if ((VARIABLE_DEF_TYPE_INTEGER
                  == variable_def_type_get(pdef))
                || (VARIABLE_DEF_TYPE_BOOLEAN
                  == variable_def_type_get(pdef))
                || (VARIABLE_DEF_TYPE_FLOAT
                  == variable_def_type_get(pdef))) {
              /* scalar: typed by the formal's def (range + conversion
               * diagnostics ride the existing machinery) */
              if (!value_is_number(dval)
                  || (value_is_array(dval))) {
                pfile_log(pf, PFILE_LOG_ERR,
                  "type mismatch in default value");
                value_release(dval);
                dval = VALUE_NONE;
              } else {
                value_def_set(dval, variable_def_dup(pdef));
              }
            } else {
              pfile_log(pf, PFILE_LOG_ERR,
                "optional parameters must be numeric or open arrays");
              value_release(dval);
              dval = VALUE_NONE;
            }
          }
        }
        /* keep defaults aligned with params[] (own growth mirroring the
         * params array below; entries below *params_ct stay intact) */
        if (!opt_defs) {
          opt_defs = MALLOC(sizeof(*opt_defs) * 16);
          if (opt_defs) {
            size_t zz;

            for (zz = 0; zz < 16; zz++) {
              opt_defs[zz] = VALUE_NONE;
            }
            opt_alloc = 16;
          }
        }
        if (opt_defs && ((*params_ct) + 1 > opt_alloc)) {
          void  *tmp;
          size_t grow;

          grow = opt_alloc * 2;
          tmp = REALLOC(opt_defs, sizeof(*opt_defs) * grow);
          if (tmp) {
            size_t zz;

            opt_defs = tmp;
            for (zz = opt_alloc; zz < grow; zz++) {
              opt_defs[zz] = VALUE_NONE;
            }
            opt_alloc = grow;
          }
        }
        if (opt_defs && ((*params_ct) < opt_alloc)) {
          opt_defs[*params_ct] = dval;
        } else if (dval) {
          value_release(dval);
        }
      }
      if ((JAL_PARSE_TASK == type)
          && variable_dflag_test(var, VARIABLE_DEF_FLAG_OUT)) {
        pfile_log(pf, PFILE_LOG_ERR,
            "OUT parameters are not allowed for TASKs");
      }
      if (variable_dflag_test(var, VARIABLE_DEF_FLAG_VOLATILE)
          && !variable_master_get(var)) {
        /* this is really a function call or two */
        char *vname;
        char *vname_ptr;

        vname = MALLOC(strlen(variable_name_get(var)) + 6);
        if (!vname) {
          pfile_log_syserr(pf, ENOMEM);
        } else {
          sprintf(vname, "_%s", variable_name_get(var));
          vname_ptr = vname + strlen(vname);
          if (variable_dflag_test(var, VARIABLE_DEF_FLAG_IN)) {
            /* change the name to: '_*_get', type = function,
             * return type = variable type, parameters = none
             * keep the OUT flag to let the caller know it needs
             * an OUT also */
            strcpy(vname_ptr, "_get");
            variable_def_member_add(
              proc_def, 
              vname, 
              jal_variable_fn_def_alloc(
                pf, 
                variable_def_get(var),
                VARIABLE_DEF_NONE, 
                VARIABLE_DEF_FLAG_IN
                | (variable_dflag_test(var, VARIABLE_DEF_FLAG_OUT)
                   ? VARIABLE_DEF_FLAG_OUT : 0)), 
              1);
          }
          if (variable_dflag_test(var, VARIABLE_DEF_FLAG_OUT)) {
            /* change the name to: '_*_put', type = function,
             * return type = none, parameters = variable type */
            strcpy(vname_ptr, "_put");
            variable_def_member_add(
              proc_def, 
              vname, 
              jal_variable_fn_def_alloc(
                pf, 
                VARIABLE_DEF_NONE, 
                variable_def_get(var), 
                VARIABLE_DEF_FLAG_OUT),
              1);
          }
          FREE(vname);
        }
      } else {
        /* if this is a variable with infinite (aka, no) dimension
         * turn it into a pointer; add the _name_count so the COUNT()
         * operator will work as expected
         */
        if (variable_dflag_test(var, VARIABLE_DEF_FLAG_BYREF)
            || (variable_is_array(var) 
              && ((variable_sz_t) -1 == variable_def_member_ct_get(
                  variable_def_member_get(
                    variable_def_get(var)
                    )
                  )
                )
              )) {
          variable_def_t vdef;

          if (variable_is_array(var)) {
            char *vname;

            vname = MALLOC(strlen(variable_name_get(var)) + 8);
            if (!vname) {
              pfile_log_syserr(pf, ENOMEM);
            } else {
              sprintf(vname, "_%s_count", variable_name_get(var));
              variable_def_member_add(proc_def, vname,
                variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
                  VARIABLE_DEF_FLAG_IN, 2), 1);
              FREE(vname);
            }
          }
          vdef = variable_def_alloc(0, VARIABLE_DEF_TYPE_POINTER,
              variable_def_flags_get_all(variable_def_get(var)), 
              pfile_pointer_width_get(pf));
          if (variable_is_array(var)) {
            variable_def_member_add(vdef,
                0, /* no tag */
                variable_def_dup(
                  variable_def_member_def_get(
                    variable_def_member_get(
                      variable_def_get(var)))),
                1);
          } else {
            variable_def_member_add(vdef,
                0, /* no tag */
                variable_def_dup(
                  variable_def_get(var)),
                1);
          }
          variable_def_set(var, vdef);
        }
        variable_def_member_add(proc_def, variable_name_get(var),
          variable_def_get(var), 1);
      }
      if (*params_ct == *params_alloc) {
        void  *tmp;
        size_t newalloc;

        newalloc = (*params_alloc) ? 2 * (*params_alloc) : 16;

        tmp = REALLOC(*params, sizeof(**params) * newalloc);
        if (tmp) {
          *params = tmp;
          *params_alloc = newalloc;
        } else {
          pfile_log_syserr(pf, ENOMEM);
        }
      }
      if (*params_ct < *params_alloc) {
        (*params)[*params_ct] = var;
      } else {
        variable_release(var);
      }
      (*params_ct)++;
    } while (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, ","));
    if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, ")")) {
      pf_token_get(pf, PF_TOKEN_NEXT);
    }
    if (opt_defs && *params_ct) {
      jal_opt_default_register(pf, proc_def, opt_defs, *params_ct);
    } else if (opt_defs) {
      FREE(opt_defs);
    }
  }
}

void jal_parse_procedure_common(pfile_t *pf, jal_parse_proc_type type,
  const pfile_pos_t *statement_start)
{
  size_t          sz;
  variable_def_t  return_def;  /* return value definition */
  variable_def_t  var_def;     /* for GET & PUT, this is the required def */
  variable_def_t  proc_def;    /* full procedure definition */
  char           *proc_name_buf;
  char           *proc_name_ptr;
  pfile_proc_t   *proc;
  size_t          params_ct;
  size_t          params_expected; /* for 'get & 'put */
  variable_t     *params;
  size_t          params_alloc;
  enum {
    PROC_USER, /* user defined procedure */
    PROC_GET,  /* GET procedure          */
    PROC_PUT   /* PUT procedure          */
  } proc_type;

  return_def      = VARIABLE_DEF_NONE;
  var_def         = VARIABLE_DEF_NONE;
  params_ct       = 0;
  params_expected = 0;
  params_alloc    = 0;
  params          = 0;

  proc_def = variable_def_alloc(0, VARIABLE_DEF_TYPE_FUNCTION, 0, 0);

  jal_identifier_is_reserved(pf);

  sz = strlen(pf_token_ptr_get(pf));
  proc_name_buf = MALLOC(sz + 8);
  if (!proc_name_buf) {
    pfile_log_syserr(pf, ENOMEM);
    proc_name_ptr = 0;
  } else {
    proc_name_buf[0] = '_';
    proc_name_buf[1] = '_';
    proc_name_ptr = proc_name_buf + 2;
    memcpy(proc_name_ptr, pf_token_ptr_get(pf), sz + 1);
  }
  pf_token_get(pf, PF_TOKEN_NEXT);
  if ((JAL_PARSE_TASK != type)
    && pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "'")) {
    const char *expect;

    expect = 0;
    switch (type) {
      case JAL_PARSE_PROCEDURE: expect = "put"; break;
      case JAL_PARSE_FUNCTION:  expect = "get"; break;
      case JAL_PARSE_TASK:      break; /* cannot get here */
    }
    if (pf_token_is(pf, PF_TOKEN_NEXT, PFILE_LOG_ERR, expect)) {
      pfile_block_t *blk;
      variable_t     var;

      blk = pfile_proc_block_active_get(
        pfile_proc_active_get(pf));
      proc_type = (JAL_PARSE_PROCEDURE == type)
        ? PROC_PUT
        : PROC_GET;
      /* first look for a corresponding variable to find its definition */
      var = pfile_block_variable_find(blk, proc_name_ptr);
      if (var) {
        var_def = variable_def_get(var);
        if (VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(var_def)) {
          /* this is an array, so find out what type it *really* is */
          var_def = variable_def_member_def_get(
              variable_def_member_get(var_def));
          params_expected = (JAL_PARSE_PROCEDURE == type)
            ? 2 : 1;
        } else {
          params_expected = (JAL_PARSE_PROCEDURE == type)
            ? 1 : 0;
        }
        variable_release(var);
      } else {
        char *ptr;
        /* no variable, look for a corresponding _get or _put */
		/* RJ Put breakpoint here to see the name of the procedure for debugging. */
        ptr = proc_name_ptr + strlen(proc_name_ptr);
        memcpy(ptr, (JAL_PARSE_PROCEDURE == type) ? "_get" : "_put",
            (size_t) 5);
        proc_name_ptr[-1] = '_';
        proc = pfile_proc_find(pf, PFILE_LOG_NONE, proc_name_ptr - 1);
        if (proc) {
          variable_def_member_t mbr;

          /* mbr points to the return value */
          for (mbr = variable_def_member_get(pfile_proc_def_get(proc));
               variable_def_member_link_get(mbr);
               mbr = variable_def_member_link_get(mbr), params_expected++)
            ; /* empty body */
          params_expected--; /* ignore the return parameter */
          if (JAL_PARSE_PROCEDURE == type) {
            params_expected--;
          }
          if (PROC_GET == proc_type) {
            /* we need the definition of the last member
             * (the 1st member is the return value, members
             * after that are indices */
            var_def = variable_def_member_def_get(mbr);
          } else {
            var_def = pfile_proc_return_def_get(proc);
          }
        } else {
          params_expected = -1;
        }
        *ptr = 0;
      }
      {
        char *suffix_ptr;

        suffix_ptr = proc_name_ptr + strlen(proc_name_ptr);
        *suffix_ptr++ = '_';
        memcpy(suffix_ptr, expect, strlen(expect) + 1);
      }
      proc_name_ptr--;
      pf_token_get(pf, PF_TOKEN_NEXT);
    } else {
      proc_type     = PROC_USER;
      proc_name_ptr = 0;
    }
  } else {
    proc_type = PROC_USER;
  }
  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "(")) {
    /* parse the parameters */
    pf_token_get(pf, PF_TOKEN_NEXT);
    jal_parse_formal_parameter_list_after_open_paren(pf, statement_start,
        proc_def, type, &params, &params_ct, &params_alloc, BOOLEAN_FALSE,
        (PROC_USER == proc_type));
  }
  if (JAL_PARSE_FUNCTION == type) {
    if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, "return")) {
      pf_token_get(pf, PF_TOKEN_NEXT);
      return_def = jal_vdef_get(pf, VARIABLE_DEF_FLAG_NONE);
      if (return_def) {
        /* Dup before flags_change: variable_def_flags_change detaches members from
         * its input def, which would hollow a shared named type (e.g. record). */
        return_def = variable_def_dup(return_def);
        if (return_def) {
          return_def = variable_def_flags_change(return_def,
            variable_def_flags_get_all(return_def) | VARIABLE_DEF_FLAG_OUT);
        }
      }
    }
  }
  switch (proc_type) {
    case PROC_USER: 
      break;
    case PROC_GET:
      if ((size_t) -1 == params_expected) {
        if (params_ct < 2) {
          params_expected = params_ct;
        } else {
          pfile_log(pf, PFILE_LOG_ERR,
              "GET functions can have 0 or 1 parameters");
          params_expected = 0;
        }
      }
      if (0 == params_expected) {
        if (params_ct) {
          pfile_log(pf, PFILE_LOG_ERR, "no parameters expected");
        }
        if (var_def && !variable_def_is_same(return_def, var_def)) {
          pfile_log(pf, PFILE_LOG_ERR, "type mismatch");
        }
      } else if (1 == params_expected) {
        /* the first parameter must be IN only */
        variable_def_t ix_def;

        ix_def = variable_def_member_def_get(
          variable_def_member_get(proc_def));
        if (params_ct != params_expected) {
          pfile_log(pf, PFILE_LOG_ERR, "%lu parameters expected", 
              (ulong) params_expected);
        } else {
          if (VARIABLE_DEF_TYPE_INTEGER != variable_def_type_get(ix_def)) {
            pfile_log(pf, PFILE_LOG_ERR, "Index must be an integer");
          }
          if (!variable_def_flag_test(ix_def, VARIABLE_DEF_FLAG_IN)) {
            pfile_log(pf, PFILE_LOG_ERR, "Index must be an IN parameter");
          }
          if (variable_def_flag_test(ix_def, VARIABLE_DEF_FLAG_OUT)) {
            pfile_log(pf, PFILE_LOG_ERR, "Index must NOT be an OUT parameter");
          }
        }
        /* the return type must match */
        if (var_def && !variable_def_is_same(return_def, var_def)) {
          /*variable_def_member_def_get(variable_def_member_get(var_def)))) {*/
          pfile_log(pf, PFILE_LOG_ERR, "type mismatch");
        }
      } else {
        pfile_log(pf, PFILE_LOG_ERR, 
          "A GET function can have 0 or 1 parameters.");
      }
      break;
    case PROC_PUT:
      {
        variable_def_t        param_def;
        variable_def_member_t param_mbr;
        size_t                ct;

        if ((size_t) -1 == params_expected) {
          if (params_ct < 3) {
            params_expected = params_ct;
          } else {
            pfile_log(pf, PFILE_LOG_ERR, 
                "PUT procedures can have 1 or 2 parameters");
            params_expected = 1;
          }
        }
        param_mbr = variable_def_member_get(proc_def);
        param_def = variable_def_member_def_get(param_mbr);
        ct = params_ct;

        if (2 == ct) {
          /* the first member is the index. it must be IN only */
          if (VARIABLE_DEF_TYPE_INTEGER != variable_def_type_get(param_def)) {
            pfile_log(pf, PFILE_LOG_ERR, "Index must be an integer");
          }
          if (!variable_def_flag_test(param_def, VARIABLE_DEF_FLAG_IN)) {
            pfile_log(pf, PFILE_LOG_ERR, "Index must be IN");
          }
          if (variable_def_flag_test(param_def, VARIABLE_DEF_FLAG_OUT)) {
            pfile_log(pf, PFILE_LOG_ERR, "Index must *not* be OUT");
          }
          param_mbr = variable_def_member_link_get(param_mbr);
          param_def = variable_def_member_def_get(param_mbr);
          /* dereference var_def */
          ct--;
        }
        if (1 != ct) {
          pfile_log(pf, PFILE_LOG_ERR, 
            "PUT function require one or two parameters");
        } 
        if (var_def
          && !variable_def_is_same(var_def, param_def)) {
          pfile_log(pf, PFILE_LOG_ERR, "type mismatch");
        }
        if (!variable_def_flag_test(param_def, VARIABLE_DEF_FLAG_IN)) {
          pfile_log(pf, PFILE_LOG_ERR, "PUT requires an IN parameter");
        }
        if (variable_def_flag_test(param_def, VARIABLE_DEF_FLAG_OUT)) {
          pfile_log(pf, PFILE_LOG_ERR, "PUT parameter cannot be OUT");
        }
      }
      break;
  }
  /* add the return def */
  variable_def_member_insert(proc_def, 0, 0, return_def, 1);

  /* create the procedure */
  if (!proc_name_ptr) {
    proc = PFILE_PROC_NONE;
  } else {
    proc = pfile_proc_find(pf, PFILE_LOG_NONE, proc_name_ptr);
    if (proc) {
      if (!variable_def_is_same(proc_def, pfile_proc_def_get(proc))) {
        pfile_log(pf, PFILE_LOG_ERR, "function signature is different");
      }
    } else {
      boolean_t      proc_created;
      pfile_block_t *root_blk;
      variable_t     sh;
      variable_t     m;
      pfile_proc_t    *pfrom;

      proc = PFILE_PROC_NONE;
      proc_created = BOOLEAN_FALSE;
      root_blk = pfile_proc_block_active_get(pfile_proc_active_get(pf));
      sh = pfile_block_variable_find_shallow(root_blk, proc_name_ptr);
      if (sh && variable_is_alias(sh)) {
        m = variable_master_get(sh);
        while (m && variable_is_alias(m)) {
          m = variable_master_get(m);
        }
        if (m) {
          pfrom = variable_proc_get(m);
          if (pfrom) {
            if (variable_def_is_same(proc_def, pfile_proc_def_get(pfrom))) {
              variable_t bind;

              pfile_block_variable_remove(root_blk, sh);
              variable_release(sh);
              bind = VARIABLE_NONE;
              if (RESULT_OK == pfile_variable_alloc(pf,
                  PFILE_VARIABLE_ALLOC_LOCAL, proc_name_ptr,
                  pfile_proc_def_get(pfrom), VARIABLE_NONE, &bind)) {
                variable_proc_set(bind, pfrom);
                variable_release(bind);
                proc = pfrom;
              } else {
                pfile_log_syserr(pf, ENOMEM);
              }
            } else {
              pfile_log(pf, PFILE_LOG_ERR,
                "procedure does not match forward declaration");
              variable_release(sh);
            }
          } else {
            variable_release(sh);
          }
        } else {
          variable_release(sh);
        }
      } else if (sh) {
        variable_release(sh);
      }
      if (!proc) {
        proc = pfile_proc_create(pf, proc_name_ptr, proc_def);
        proc_created = BOOLEAN_TRUE;
      }
      if (proc_created && (JAL_PARSE_TASK == type)) {
        pfile_proc_flag_set(proc, PFILE_PROC_FLAG_TASK);
      }
      
      /* For pseudo variables (procedures with ' syntax), also create variable entries */
      if ((PROC_GET == proc_type) || (PROC_PUT == proc_type)) {
        char *base_name;
        char *var_name_buf;
        pfile_block_t *blk;
        variable_t existing_var;
        variable_def_t pseudo_def;
        variable_t pseudo_var;
        result_t rc;
        
        /* Create variables with the names that jal_value_find expects */
        base_name = proc_name_ptr + 2; /* Skip the "__" prefix */
        var_name_buf = MALLOC(strlen(base_name) + 10);
        
        if (var_name_buf) {
          /* Create the quote syntax variable name */
          sprintf(var_name_buf, "%s%s", base_name, 
                (PROC_GET == proc_type) ? "'get" : "'put");
          
          /* Check if variable already exists */
          blk = pfile_proc_block_active_get(pfile_proc_active_get(pf));
          existing_var = pfile_block_variable_find(blk, var_name_buf);
          if (VARIABLE_NONE == existing_var) {
            /* Create a variable that represents this pseudo variable */
            /* Use the same definition as the procedure */
            pseudo_def = pfile_proc_def_get(proc);
            if (pseudo_def != VARIABLE_DEF_NONE) {
              rc = pfile_variable_alloc(pf, 
                  PFILE_VARIABLE_ALLOC_LOCAL, var_name_buf, pseudo_def, 
                  VARIABLE_NONE, &pseudo_var);
              if (RESULT_OK == rc) {
                /* Store the procedure in the variable */
                variable_proc_set(pseudo_var, proc);
                variable_release(pseudo_var);
              }
            }
          }
          
          /* Also create the base variable name for the pseudo variable (only for GET) */
          if (PROC_GET == proc_type) {
            sprintf(var_name_buf, "%s", base_name);
            existing_var = pfile_block_variable_find(blk, var_name_buf);
            if (VARIABLE_NONE == existing_var) {
              /* Create a variable that represents the base pseudo variable */
              /* Use the same definition as the procedure */
              pseudo_def = pfile_proc_def_get(proc);
              if (pseudo_def != VARIABLE_DEF_NONE) {
                rc = pfile_variable_alloc(pf, 
                    PFILE_VARIABLE_ALLOC_LOCAL, var_name_buf, pseudo_def, 
                    VARIABLE_NONE, &pseudo_var);
                if (RESULT_OK == rc) {
                  /* Store the procedure in the variable */
                  variable_proc_set(pseudo_var, proc);
                  variable_release(pseudo_var);
                }
              }
            }
          }
          
          FREE(var_name_buf);
        }
      }
      
    }
  }
  FREE(proc_name_buf);
  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "is")) {
    const char        *etype;
    label_t            proc_lbl;

    pf_token_get(pf, PF_TOKEN_NEXT);
    /* it looks like ``begin'' is optional if it follows ``is'' */
    if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "begin")) {
      pf_token_get(pf, PF_TOKEN_NEXT);
    }

    proc_lbl = pfile_proc_label_get(proc);

    if (proc) {
      size_t ii;

      pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO,
          CMD_BRANCHCOND_NONE, pfile_proc_skip_label_get(proc), 
            VALUE_NONE, VALUE_NONE, 0);
      pfile_proc_enter(pf, pfile_proc_tag_get(proc));
      pfile_block_enter(pf);

      if (label_flag_test(proc_lbl, LABEL_FLAG_DEFINED)) {
        pfile_log(pf, PFILE_LOG_ERR, "function already defined");
      } else {
        label_flag_test(proc_lbl, LABEL_FLAG_DEFINED);
      }
      pfile_proc_define(proc, proc_def, pf);
      /* proc_define takes the proc_def and creates the necessary
       * variables from it. Unfortunately, proc_def doesn't contain
       * information about masters, or bases, so that is passed
       * in here. */
      for (ii = 0; (ii < params_ct) && (ii < params_alloc); ii++) {
        if (variable_master_get(params[ii])
          || (VARIABLE_BASE_UNKNOWN != variable_base_real_get(params[ii], 0))) {
          variable_t tmp;

          /* remember, proc[0] = return value */
          tmp = value_variable_get(pfile_proc_param_get(proc, ii+1));
          variable_bit_offset_set(tmp, variable_bit_offset_get(params[ii]));
          variable_base_set(tmp, variable_base_real_get(params[ii], 0), 0);
          variable_master_set(tmp, variable_master_get(params[ii]));
          if (variable_is_alias(params[ii])) {
            variable_flag_set(tmp, VARIABLE_FLAG_ALIAS);
          }
        }
      }
    }

    label_flag_set(pfile_proc_label_get(proc), LABEL_FLAG_DEFINED);
    /*pfile_emit_enter_cmd(pf);*/
    jal_block_process(pf, JAL_BLOCK_PROCESS_FLAG_NO_BLOCK);
    etype = "???";
    switch (type) {
      case JAL_PARSE_PROCEDURE: etype = "procedure"; break;
      case JAL_PARSE_FUNCTION:  etype = "function";  break;
      case JAL_PARSE_TASK:      etype = "task";      break;
    }
    if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, "end")
      && pf_token_is(pf, PF_TOKEN_NEXT, PFILE_LOG_ERR, etype)) {
      pf_token_get(pf, PF_TOKEN_NEXT);
    } else {
      jal_block_start_show(pf, etype, statement_start);
    }
    if (proc) {
      pfile_cmd_label_add(pf, pfile_proc_exit_label_get(proc));
      pfile_block_leave(pf);
      pfile_proc_leave(pf);
      pfile_cmd_label_add(pf, pfile_proc_skip_label_get(proc));
    }
  }
  while (params_ct--) {
    if (params_ct < params_alloc) {
      variable_release(params[params_ct]);
    }
  }
  FREE(params);
}

void jal_parse_procedure(pfile_t *pf, const pfile_pos_t *statement_start)
{
  jal_parse_procedure_common(pf, JAL_PARSE_PROCEDURE, statement_start);
}

void jal_parse_function(pfile_t *pf, const pfile_pos_t *statement_start)
{
  jal_parse_procedure_common(pf, JAL_PARSE_FUNCTION, statement_start);
}

void jal_parse_return(pfile_t *pf, const pfile_pos_t *statement_start)
{
  label_t       lbl;
  pfile_proc_t *proc;

  UNUSED(statement_start);
  proc = pfile_proc_active_get(pf);
  lbl = pfile_proc_exit_label_get(proc);
  if (!lbl) {
    pfile_log(pf, PFILE_LOG_ERR, PFILE_MSG_SYNTAX_ERROR);
  } else {
    value_t val;
    value_t expr;

    val = pfile_proc_param_get(proc, 0);
    if (val) {
      expr = jal_parse_expr(pf);
      if (expr) {
        pfile_cmd_op_add(pf, OPERATOR_ASSIGN, &val, expr, VALUE_NONE);
        value_release(expr);
      }
    }
    pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO,
        CMD_BRANCHCOND_NONE, lbl, VALUE_NONE, VALUE_NONE, 0);
  }
}

/*
 * NAME
 *   jal_volatile_function_create
 *
 * DESCRIPTION
 *   create an implicit GET or PUT function
 *
 * PARAMETERS
 *   pf       : pfile handle
 *   var_name : variable name
 *   fn_name  : function name
 *
 * RETURN
 *   value
 *
 * NOTES
 */
typedef enum {
  JAL_VOLATILE_FN_GET,
  JAL_VOLATILE_FN_PUT
} jal_volatile_fn_type;

static value_t jal_volatile_function_create(pfile_t *pf, 
    jal_volatile_fn_type type, const char *var_name)
{
  value_t tmp;
  value_t val;

  val = pfile_value_find(pf, PFILE_LOG_ERR, var_name);
  tmp = VALUE_NONE;
  if (val) {
    variable_def_t  proc_def;
    variable_def_t  return_def;
    variable_def_t  param_def;
    pfile_proc_t   *proc;
    variable_t      var;
    pfile_block_t  *blk;
    char           *fn_name;
    size_t          fn_name_sz;
    const char     *fn_suffix;

    fn_name_sz = 7 + strlen(var_name);

    proc_def   = variable_def_alloc(0, VARIABLE_DEF_TYPE_FUNCTION, 0, 0);
    return_def = VARIABLE_DEF_NONE;
    param_def  = VARIABLE_DEF_NONE;
    fn_suffix  = "";
    switch (type) {
      case JAL_VOLATILE_FN_GET:
        return_def = value_def_get(val);
        return_def = variable_def_flags_change(return_def,
            variable_def_flags_get_all(param_def) | VARIABLE_DEF_FLAG_OUT);
        fn_suffix = "vget";
        break;
      case JAL_VOLATILE_FN_PUT:
        param_def = value_def_get(val);
        param_def = variable_def_flags_change(param_def,
            (variable_def_flags_get_all(param_def) 
            & ~VARIABLE_DEF_FLAG_VOLATILE)
            | VARIABLE_DEF_FLAG_IN);
        fn_suffix = "vput";
        break;
    }

    variable_def_member_add(proc_def, 0, return_def, (return_def) ? 1 : 0);
    if (param_def) {
      variable_def_member_add(proc_def, "n", param_def, 1);
    }

    fn_name = MALLOC(fn_name_sz);
    if (!fn_name) {
      pfile_log_syserr(pf, ENOMEM);
      tmp = VALUE_NONE;
    } else {
      pfile_proc_t  *pfproc_active;   /* current active proc */
      pfile_block_t *pfblock_active; /* current active block within proc */
      pfile_proc_t  *pfproc_owner;   /* proc owning the variable's block */
      pfile_block_t *pfblock_owner;  /* owner proc's active block */
      boolean_t      entered;

      /*
       * this function needs to be created in the same
       * proc and block of the named variable. So...
       * first, find exactly where the variable is
       */
      sprintf(fn_name, "_%s_%s", var_name, fn_suffix);
      var = pfile_variable_find(pf, PFILE_LOG_ERR, var_name, &blk);
      variable_release(var);
      /*
       * use the information to create the procedures in the
       * correct place
       */
      pfproc_active  = pfile_proc_active_get(pf);
      pfblock_active = pfile_proc_block_active_get(pfproc_active);
      pfproc_owner   = pfile_block_owner_get(blk);
      pfblock_owner  = pfile_proc_block_active_get(pfproc_owner);

      /* The whole synthesis must run under the VARIABLE's owner proc, not
       * the caller's: pfile_proc_enter() searches only pf->proc_active's
       * children, and pfile_proc_leave() pops whatever proc_active is.
       * Restoring the caller before enter/leave made the first accessor
       * synthesis for a file-scope pseudo variable (e.g. `serial_data`
       * used inside a procedure) fail the enter and then pop the CALLER
       * proc -- the rest of the file parsed at the wrong scope, with a
       * cascade of bogus "not defined" errors and orphaned label refs
       * (the 2026-10-29 `label leak: ... _l567` 16f1825 report). */
      pfile_proc_active_set(pf, pfproc_owner);
      pfile_proc_block_active_set(pfproc_owner, blk);
      proc = pfile_proc_create(pf, fn_name, proc_def);

      pfile_cmd_cursor_set(pf, pfile_block_cmd_get(blk));
      pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_GOTO,
          CMD_BRANCHCOND_NONE, pfile_proc_skip_label_get(proc),
          VALUE_NONE, VALUE_NONE, 0);
      entered = (boolean_t) (RESULT_OK
          == pfile_proc_enter(pf, pfile_proc_tag_get(proc)));
      if (entered) {
        pfile_block_enter(pf);
        pfile_proc_define(proc, proc_def, pf);
        switch (type) {
          case JAL_VOLATILE_FN_GET:
            tmp = pfile_proc_param_get(proc, 0);
            pfile_cmd_op_add(pf, OPERATOR_ASSIGN, &tmp, val, VALUE_NONE);
            break;
          case JAL_VOLATILE_FN_PUT:
            tmp = pfile_proc_param_get(proc, 1);
            pfile_cmd_op_add(pf, OPERATOR_ASSIGN, &val, tmp, VALUE_NONE);
            break;
        }
        pfile_block_leave(pf);
        pfile_proc_leave(pf);
      } else {
        /* the accessor proc exists but could not be entered: report and
         * degrade to "no accessor" rather than corrupting the scope */
        pfile_log(pf, PFILE_LOG_ERR,
            "cannot enter the %s accessor procedure", fn_name);
        tmp = VALUE_NONE;
      }
      pfile_cmd_label_add(pf, pfile_proc_skip_label_get(proc));
      pfile_cmd_cursor_set(pf, CMD_NONE);
      pfile_proc_block_active_set(pfproc_owner, pfblock_owner);
      pfile_proc_active_set(pf, pfproc_active);
      pfile_proc_block_active_set(pfproc_active, pfblock_active);
      if (entered) {
        tmp = pfile_value_find(pf, PFILE_LOG_ERR, fn_name);
      }
      value_release(val);
    }
  }
  return tmp;
}

/*
 * NAME
 *   jal_parse_call
 *
 * DESCRIPTION
 *   parse a call
 *
 * PARAMETERS
 *   pf  : pfile handle
 *   val : a function, a function pointer, or a function reference
 *
 * RETURN
 *   The resulting value, or VALUE_NONE
 *
 * NOTES
 *   This will automatically deference val.
 */
/* function-pointer formal: bind the caller's _get/_put (or synthesize
 * the volatile accessor); an IN|OUT pair consumes the next formal
 * slot (advances ii and the member walk) */
static void jal_call_fnptr_param_parse(pfile_t *pf, const char *ptr, variable_def_t def, value_t *parms, size_t param_ct, size_t *pii, variable_def_member_t *pmbr)
{
  size_t               ii  = *pii;
  variable_def_member_t mbr = *pmbr;
  value_t              val;

  /* this *must be* a function reference. this means a volatile
     parameter. look at the function defintion flags, they'll
     either be:
       IN     -- the _get function is needed
       IN|OUT -- the _get + _put functions are needed
       OUT    -- the _put function is needed */
  value_t        jvals[JAL_VAL_TYPE_CT];
  variable_def_t fn_def;
  variable_def_t fn_def2;

  jal_value_find(pf, ptr, jvals);

  fn_def = variable_def_member_def_get( variable_def_member_get(def));
  if (variable_def_flag_test(fn_def, VARIABLE_DEF_FLAG_IN)) {
    if (jvals[JAL_VAL_TYPE_GET]) {
      val = jvals[JAL_VAL_TYPE_GET];
      value_lock(val);
    } else if (jvals[JAL_VAL_TYPE_IGET]) {
      val = jvals[JAL_VAL_TYPE_IGET];
      value_lock(val);
    } else {
      /* nb: the name might have changed (via alias), so if there's
             a found entry, use its name instead */
      size_t valn;

      for (valn = JAL_VAL_TYPE_MIN;
           (valn < JAL_VAL_TYPE_CT) && (VALUE_NONE == jvals[valn]);
           valn++)
        ; /* empty body */
      if (valn < JAL_VAL_TYPE_CT) {
        ptr = value_name_get(jvals[valn]);
      }
      val = jal_volatile_function_create(pf, JAL_VOLATILE_FN_GET, 
          ptr);
    }

    fn_def2 = value_def_get(val);
    if (VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(fn_def2)) {
      fn_def2 = variable_def_member_def_get(
        variable_def_member_get(fn_def2));
    }
    if (!variable_def_is_same(fn_def, fn_def2)) {
      pfile_log(pf, PFILE_LOG_ERR, "type mismatch");
      value_release(val);
      val = VALUE_NONE;
    }
    if (parms && (ii < param_ct)) {
      parms[ii] = val;
    } else {
      value_release(val);
    }
  }
  if (variable_def_flag_test(fn_def, VARIABLE_DEF_FLAG_OUT)) {
    if (variable_def_flag_test(fn_def, VARIABLE_DEF_FLAG_IN)) {
      /* this is an in/out so the next parameter must be the OUT
         parameter */
      ii++;
      mbr = variable_def_member_link_get(mbr);
      fn_def = variable_def_member_def_get(
        variable_def_member_get(
          variable_def_member_def_get(mbr)));
    }

    if (jvals[JAL_VAL_TYPE_PUT]) {
      val = jvals[JAL_VAL_TYPE_PUT];
      value_lock(val);
    } else if (jvals[JAL_VAL_TYPE_IPUT]) {
      val = jvals[JAL_VAL_TYPE_IPUT];
      value_lock(val);
    } else {
      val = jal_volatile_function_create(pf, JAL_VOLATILE_FN_PUT, ptr);
    }
    fn_def2 = value_def_get(val);
    if (VARIABLE_DEF_TYPE_POINTER
      == variable_def_type_get(fn_def2)) {
      fn_def2 = variable_def_member_def_get(
        variable_def_member_get(fn_def2));
    }
    if (!variable_def_is_same(fn_def, fn_def2)) {
      pfile_log(pf, PFILE_LOG_ERR, "parameter %u type mismatch",
        (unsigned) ii);
      value_release(val);
      val = VALUE_NONE;
    }
    if (parms && (ii < param_ct)) {
      parms[ii] = val;
    } else {
      value_release(val);
    }
  }
  jal_value_release(jvals);
  pf_token_get(pf, PF_TOKEN_NEXT);

  *pii = ii;
  *pmbr = mbr;
}

/* OUT formal: a PUT procedure (its 'put is called after the callee
 * returns, value in tow) or a plain lvalue */
static void jal_call_out_param_parse(pfile_t *pf, const char *ptr, variable_def_t def, size_t ii, size_t param_ct, value_t *parms, value_t *put_parms)
{
  value_t val;

  /* this needs to be a simple variable or PUT procedure */
  value_t jvals[JAL_VAL_TYPE_CT];

  jal_value_find(pf, ptr, jvals);

  if (jvals[JAL_VAL_TYPE_PUT]) {
    variable_def_t        put_def;
    variable_def_member_t put_mbr;
    value_t               tval;

    val = jvals[JAL_VAL_TYPE_PUT];
    value_lock(val);
    tval = variable_def_flag_test(def, VARIABLE_DEF_FLAG_IN)
      ? jal_parse_value(pf, JAL_PARSE_VALUE_TYPE_RVALUE)
      : VALUE_NONE;
    if (put_parms && (ii < param_ct)) {
      put_parms[ii] = val;
    } else {
      value_release(val);
    }
    if (!tval || !value_is_temp(tval)) {
      put_def = value_def_get(val); /* this should be a function */
      put_mbr = variable_def_member_get(put_def);
      put_mbr = variable_def_member_link_get(put_mbr);
      put_def = variable_def_member_def_get(put_mbr);
      val = pfile_value_temp_get_from_def(pf, put_def);
      if (tval) {
        pfile_cmd_op_add(pf, OPERATOR_ASSIGN, &val, tval, VALUE_NONE);
        value_release(tval);
      }
    } else {
      val = tval;
    }
    put_def = value_def_get(val);
    put_def = variable_def_flags_change(put_def,
        variable_def_flags_get_all(put_def)
        | VARIABLE_DEF_FLAG_OUT);
    value_def_set(val, put_def);
    if (!tval) {
      pf_token_get(pf, PF_TOKEN_NEXT);
    }
  } else {
    val = jal_parse_value(pf, JAL_PARSE_VALUE_TYPE_LVALUE);
    if (value_is_const(val)) {
      pfile_log(pf, PFILE_LOG_ERR, "%s cannot be a constant",
          value_name_get(val));
    } else if (value_vflag_test(val, VARIABLE_FLAG_READ)
      && !value_vflag_test(val, VARIABLE_FLAG_WRITE)) {
      pfile_log(pf, PFILE_LOG_ERR, "cannot read %s",
        value_name_get(val));
    }
    while (jal_parse_structure(pf, &val)
      || jal_parse_subscript(pf, &val, NULL))
      ; /* null body */
  }
  if (!((variable_def_type_is_number(variable_def_type_get(def))
      && value_is_number(val))
      || variable_def_is_same(def, value_def_get(val)))) {
    pfile_log(pf, PFILE_LOG_ERR, "type mismatch");
    value_release(val);
    val = VALUE_NONE;
  }
  if (parms && (ii < param_ct)) {
    parms[ii] = val;
  } else {
    value_release(val);
  }
  jal_value_release(jvals);

}

/* IN formal: parse the expression, apply the optional-empty-string
 * sentinel, marshal string actuals (byte-buffer + runtime length,
 * literal staging), type-check, and fill the hidden _count slot */
static void jal_call_in_param_parse(pfile_t *pf, const char *ptr, variable_def_t def, size_t ii, size_t param_ct, value_t *parms, boolean_t need_ct, flag_t flags, value_t call_val)
{
  value_t val;

  /* this must be IN only! */
  boolean_t      ok;
  variable_def_t vdef;
  boolean_t      suppress_ptr_deref;
  boolean_t      opt_sentinel;
  value_t        str_rt_len = VALUE_NONE;

  opt_sentinel = BOOLEAN_FALSE;
  suppress_ptr_deref = jal_formal_def_is_data_pointer_non_callable(
      def);
  if (suppress_ptr_deref) {
    pfile_parse_call_data_ptr_arg_push(pf);
  }
  val = jal_parse_expr(pf);
  if (suppress_ptr_deref) {
    pfile_parse_call_data_ptr_arg_pop(pf);
  }
  /* Optional-formal empty-string sentinel: `report("")` on a
   * NON-string OPTIONAL formal means OMITTED -- fill the
   * registered default exactly as a missing trailing argument
   * would (print-style callers pass position-preserving
   * "nothing here" placeholders).  Only the empty const string
   * is the sentinel: a non-empty string keeps the normal type
   * error, a REQUIRED formal never gets the idiom (no
   * registered default), and STRING-typed formals take "" as a
   * real empty string. */
  if (val
      && (ii < param_ct)
      && !(flags & (JAL_PARSE_CALL_FLAG_PUT
          | JAL_PARSE_CALL_FLAG_GET))
      && call_val
      && (VARIABLE_DEF_TYPE_FUNCTION == value_type_get(call_val))
      && value_is_const(val)
      && value_is_array(val)
      && value_dflag_test(val, VARIABLE_DEF_FLAG_STRING)
      && (0 == value_ct_get(val))
      && !variable_def_flag_test(def, VARIABLE_DEF_FLAG_STRING)) {
    jal_opt_default_t *oent;

    oent = jal_opt_default_find(value_def_get(call_val));
    if (oent) {
      jal_opt_default_resolve(pf, oent);
      if ((ii < oent->member_ct) && oent->by_member[ii]) {
        value_t dv;

        dv = value_clone(oent->by_member[ii]);
        if (dv) {
          value_lock(dv);
        }
        value_release(val);
        val         = dv;
        opt_sentinel = BOOLEAN_TRUE;
        if (need_ct && (ii >= 1)
            && ((ii - 1) < oent->member_ct)
            && oent->by_member[ii - 1]) {
          /* open-array formal: the hidden count slot takes the
           * default's registered count (the generic path would
           * rebuild it from the value name) */
          value_t ctv;

          ctv = value_clone(oent->by_member[ii - 1]);
          if (ctv) {
            value_lock(ctv);
          }
          if (parms) {
            value_release(parms[ii - 1]);
            parms[ii - 1] = ctv;
          } else {
            value_release(ctv);
          }
        }
      }
    }
  }
  /* String actual into an open byte-array formal (e.g.
   * print_string(serial, s) with `var string s`): marshal the
   * CONTENT -- copy it into a temp byte buffer, and remember
   * the runtime length for the hidden count parameter (the
   * buffer capacity would over-report). */
  if (val
      && (VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(def))
      && value_dflag_test(val, VARIABLE_DEF_FLAG_STRING)) {
    value_t sbuf = jal_string_arg_byte_array(pf, val);

    if (sbuf) {
      str_rt_len = jal_string_length_value_get(pf, val);
      value_release(val);
      val = sbuf;
    }
  }
  if (value_vflag_test(val, VARIABLE_FLAG_WRITE)
    && !value_vflag_test(val, VARIABLE_FLAG_READ)) {
    pfile_log(pf, PFILE_LOG_ERR, "cannot read from %s",
      value_name_get(val));
  }
  /* Const/fixed string LITERAL actual into a DYNAMIC-string
   * formal: stage the text into a compiler string cell (the
   * chain-cell machinery) and pass the cell -- the formal's
   * def is a heap HANDLE, which a raw literal's plain array
   * def never matches (a 3-char literal only passed by the
   * count==handle-size coincidence; "abc" worked while
   * "ab"/"abcd"/"" all errored). Empty literal: the assign's
   * len-0 path clears the cell -- COUNT(formal) sees 0. */
  if (val
      && (ii < param_ct)
      && value_is_const(val)
      && !jal_string_is_dynamic(val)
      && jal_string_is_source(val)
      && variable_def_flag_test(def, VARIABLE_DEF_FLAG_STRING)) {
    value_t cell;

    if (variable_def_flag_test(def,
          VARIABLE_DEF_FLAG_STRING_DYNAMIC)) {
      cell = jal_string_stage_source_cell(pf, val);
    } else if (VARIABLE_DEF_TYPE_ARRAY
        == variable_def_type_get(def)) {
      cell = jal_string_stage_fixed_cell(pf, val,
          (variable_sz_t) variable_def_member_ct_get(
            variable_def_member_get(def)));
    } else {
      cell = VALUE_NONE;
    }
    if (cell) {
      value_release(val);
      val = cell;
    }
  }
  if (ii < param_ct) {
    vdef = value_def_get(val);
    ok = variable_def_type_is_number(variable_def_type_get(def))
            && value_is_number(val);
    if (ok) {
      pfile_value_def_type_check(pf, OPERATOR_ASSIGN, def, val,
        VALUE_NONE);
      if (value_is_universal(val)) {
        pfile_value_def_overflow_check(pf, OPERATOR_ASSIGN, def, val);
      }
    }
    ok = ok || variable_def_is_same(def, vdef);
    /*
     * allow the assignment of an array to a pointer
     */
    ok = ok 
      || ((VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(def))
            && (VARIABLE_DEF_TYPE_ARRAY 
              == variable_def_type_get(vdef))
            && variable_def_is_same(
              variable_def_member_def_get(
                variable_def_member_get(def)
                ),
              variable_def_member_def_get(
                variable_def_member_get(vdef)
                )
              )
            );
    /*
     * if vdef is a one element array, and def is a single element
     * of the same type, allow this
     */
    ok = ok
      || ((VARIABLE_DEF_TYPE_ARRAY == variable_def_type_get(vdef))
        && variable_def_type_is_number(
          variable_def_type_get(def))
        && variable_def_type_is_number(
             variable_def_type_get(
               variable_def_member_def_get(
                 variable_def_member_get(vdef))))
        && (1 == variable_def_member_ct_get(
          variable_def_member_get(vdef))));
    if (!val || !ok) {
      if (val) {
        pfile_log(pf, PFILE_LOG_ERR, "type mismatch");
        value_release(val);
        val = VALUE_NONE;
      }
    }
  }
  if (parms && (ii < param_ct)) {
    parms[ii] = val;
    if (val && need_ct && !opt_sentinel) {
      char *ct_name;

      ptr     = value_name_get(val);
      ct_name = MALLOC(8 + strlen(ptr));
      if (!ct_name) {
        pfile_log_syserr(pf, ENOMEM);
      } else {
        value_t ct_val;

        sprintf(ct_name, "_%s_count", ptr);
        ct_val = pfile_value_find(pf, PFILE_LOG_NONE, ct_name);
        if (!ct_val) {
          ct_val = pfile_constant_get(pf, value_ct_get(val),
            VARIABLE_DEF_NONE);
        }
        if (str_rt_len) {
          /* string coercion: the count is the runtime length */
          value_release(ct_val);
          ct_val = str_rt_len;
          str_rt_len = VALUE_NONE;
        }
        parms[ii - 1] = ct_val;
        FREE(ct_name);
      }
    }
  } else {
    value_release(val);
  }

}

/* fill omitted trailing arguments from the callee's registered
 * defaults (direct calls only -- signatures carry no defaults, so
 * pointer-called procs keep full arity) */
static void jal_call_optional_defaults_fill(pfile_t *pf, value_t call_val, flag_t flags, value_t *parms, size_t param_ct, size_t *pii)
{
  size_t ii = *pii;

  jal_opt_default_t *entry;

  entry = (call_val
      && (VARIABLE_DEF_TYPE_FUNCTION == value_type_get(call_val))
      && !(flags & (JAL_PARSE_CALL_FLAG_PUT | JAL_PARSE_CALL_FLAG_GET)))
    ? jal_opt_default_find(value_def_get(call_val))
    : 0;
  if (entry) {
    size_t kk;

    jal_opt_default_resolve(pf, entry);
    for (kk = ii; kk < param_ct; kk++) {
      if ((kk < entry->member_ct) && entry->by_member[kk]) {
        value_t dv;

        dv = value_clone(entry->by_member[kk]);
        if (dv) {
          value_lock(dv);
        }
        parms[kk] = dv;
        ii++;
      } else {
        break;
      }
    }
  }

  *pii = ii;
}

value_t jal_parse_call(pfile_t *pf, value_t call_val, flag_t flags)
{
  value_t              *parms;
  value_t              *put_parms; /* needed for any 'put */
  size_t                param_ct;
  variable_def_t        def;
  variable_def_member_t mbr;
  value_t               val;
  boolean_t             is_put;
  const char           *param_open;
  const char           *param_close;
  size_t                ii;

  is_put = flags & JAL_PARSE_CALL_FLAG_PUT;
  assert((VALUE_NONE == call_val)
      || (VARIABLE_DEF_TYPE_FUNCTION == value_type_get(call_val))
      || (VARIABLE_DEF_TYPE_POINTER == value_type_get(call_val)));
  
  /* Handle function pointers by getting the target function definition */
  if (call_val && (VARIABLE_DEF_TYPE_POINTER == value_type_get(call_val))) {
    /* For function pointers, get the member definition (what the pointer points to) */
    variable_def_t ptr_def = value_def_get(call_val);
    variable_def_member_t ptr_mbr = variable_def_member_get(ptr_def);
    if (ptr_mbr) {
      variable_def_t target_def = variable_def_member_def_get(ptr_mbr);
      if (target_def && (VARIABLE_DEF_TYPE_FUNCTION == variable_def_type_get(target_def))) {
        def = target_def;
      } else {
        /* Member definition exists but isn't a function - use original approach */
        def = ptr_def;
      }
    } else {
      /* No member definition - this shouldn't happen after our fix */
      def = ptr_def; /* fallback */
    }
  } else {
    def = value_def_get(call_val);
  }
  for (mbr = variable_def_member_get(def), param_ct = 0;
       mbr;
       mbr = variable_def_member_link_get(mbr), param_ct++)
    ; /* null body */

  if (!param_ct) {
    param_ct = 1;
  }
  parms = CALLOC(param_ct, sizeof(*parms));
  put_parms = CALLOC(param_ct, sizeof(*put_parms));
  if (!parms || !put_parms) {
    pfile_log_syserr(pf, ENOMEM);
    FREE(parms);
    FREE(put_parms);
    return VALUE_NONE;
  }

  mbr = variable_def_member_get(def);
  if (parms) {
    /* parms[0] is only the implicit function-result slot (OUT, not IN).
     * Procedures have no [0] return value; their first member is the first
     * formal (e.g. word in offset). Allocating parms[0] from an IN formal was
     * wrong; only allocate a temp when member 0 is OUT-only (function return). */
    if (variable_def_member_def_get(mbr)) {
      variable_def_t d0;

      d0 = variable_def_member_def_get(mbr);
      if (variable_def_flag_test(d0, VARIABLE_DEF_FLAG_OUT)
          && !variable_def_flag_test(d0, VARIABLE_DEF_FLAG_IN)) {
        parms[0] = pfile_value_temp_get_from_def(pf, d0);
      } else {
        parms[0] = VALUE_NONE;
      }
    } else {
      parms[0] = VALUE_NONE;
    }
  }

  if (flags & (JAL_PARSE_CALL_FLAG_PUT | JAL_PARSE_CALL_FLAG_GET)) {
    param_open = "[";
    param_close = "]";
  } else {
    param_open = "(";
    param_close = ")";
  }
  ii = 1;
  if (is_put 
      || pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, param_open)) {
    boolean_t need_close;

    need_close = !is_put && pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE,
        param_open);

    if (is_put || need_close) {

      if (need_close) {
        pf_token_get(pf, PF_TOKEN_NEXT);
      }
      if (!pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, param_close)) {
        for (ii = 1;
            /*(ii < param_ct) */
            ((1 == ii)
              || (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, ",")));
             ii++) {
          const char *ptr;
          /*const char *vname;*/
          boolean_t   need_ct;

          /* skip either the opening '(' or the last ',' */
          if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, ",")) {
            ptr = pf_token_get(pf, PF_TOKEN_NEXT);
          } else {
            ptr = pf_token_get(pf, PF_TOKEN_CURRENT);
          }
          mbr = variable_def_member_link_get(mbr);
          if (!mbr) {
            val = jal_parse_expr(pf);
            value_release(val);
            continue;
          }
          def = variable_def_member_def_get(mbr);
          if ((ii + 1 < param_ct)
            && (VARIABLE_DEF_TYPE_INTEGER == variable_def_type_get(def))
            && VARIABLE_DEF_TYPE_POINTER
              == variable_def_type_get(
                variable_def_member_def_get(
                  variable_def_member_link_get(mbr)))) {
            /*
             * this must be the count associated with the pointer. 
             * skip it, it will be filled in later 
             */
            need_ct = BOOLEAN_TRUE;
            mbr     = variable_def_member_link_get(mbr);
            def     = variable_def_member_def_get(mbr);
            ii++;
          } else {
            need_ct = BOOLEAN_FALSE;
          }

          if ((VARIABLE_DEF_TYPE_POINTER == variable_def_type_get(def)) 
              && (VARIABLE_DEF_TYPE_FUNCTION == variable_def_type_get(
                  variable_def_member_def_get(
                    variable_def_member_get(
                      def)
                    )
                  )
                )) {
            jal_call_fnptr_param_parse(pf, ptr, def, parms, param_ct,
                &ii, &mbr);
          } else if (variable_def_flag_test(def, VARIABLE_DEF_FLAG_OUT)) {
            jal_call_out_param_parse(pf, ptr, def, ii, param_ct, parms,
                put_parms);
          } else {
            jal_call_in_param_parse(pf, ptr, def, ii, param_ct, parms,
                need_ct, flags, call_val);
          }
        }
      }
      if (need_close 
          && pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, param_close)) {
        pf_token_get(pf, PF_TOKEN_NEXT);
      }
    }
  }
  if (ii < param_ct) {
    /* optional parameters: fill the omitted trailing arguments from the
     * callee's registered defaults (direct calls only -- signatures
     * carry no defaults, so pointer-called procs keep full arity) */
    jal_call_optional_defaults_fill(pf, call_val, flags, parms,
        param_ct, &ii);
  }
  if (ii != param_ct) {
    pfile_log(pf, PFILE_LOG_ERR,
        "Too %s parameters (%lu expected, %lu found)",
        (ii < param_ct) ? "few" : "many",
        (ulong) (param_ct - 1), (ulong) (ii - 1));
  }
  val = parms[0];
  value_lock(val);
  if (call_val) {
    pfile_cmd_branch_add(pf, 
        (flags & JAL_PARSE_CALL_FLAG_START)
          ? CMD_BRANCHTYPE_TASK_START
          : CMD_BRANCHTYPE_CALL,
        CMD_BRANCHCOND_NONE, LABEL_NONE,
        VALUE_NONE, call_val, parms);
  }
  /* look for any PUT params, and do the necessary calls here... */
  if (put_parms) {
    for (ii = 0; ii < param_ct; ii++) {
      if (put_parms[ii]) {
        /* need to call put_params[ii] with params[ii] as the parameter */
        value_t        *parms2;

        parms2 = CALLOC(2, sizeof(*parms2));
        if (!parms2) {
          pfile_log_syserr(pf, ENOMEM);
        } else {
          parms2[0] = VALUE_NONE; /* no return value */
          value_lock(parms[ii]);
          parms2[1] = parms[ii];
          pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_CALL,
              CMD_BRANCHCOND_NONE, LABEL_NONE, VALUE_NONE,
              put_parms[ii], parms2);
        }
        value_release(put_parms[ii]);
      }
    }
    FREE(put_parms);
  }

  return val;
}

void jal_parse_task(pfile_t *pf, const pfile_pos_t *statement_start)
{
  jal_parse_procedure_common(pf, JAL_PARSE_TASK, statement_start);
}

void jal_parse_start(pfile_t *pf, const pfile_pos_t *statement_start)
{
  pfile_proc_t *proc;

  UNUSED(statement_start);

  proc = pfile_proc_active_get(pf);
  if (pfile_proc_parent_get(proc)) {
    pfile_log(pf, PFILE_LOG_ERR,
        "_task_start can only be used in the main program");
  } else {
    value_t   jvals[JAL_VAL_TYPE_CT];
    boolean_t jval_found;

    jval_found = jal_value_find(pf, pf_token_get(pf, PF_TOKEN_CURRENT), jvals);
    proc = value_proc_get(jvals[JAL_VAL_TYPE_BASE]);
    if (!jval_found
        || !proc 
        || !pfile_proc_flag_test(proc, PFILE_PROC_FLAG_TASK)) {
      pfile_log(pf, PFILE_LOG_ERR, "task expected");
    } else {
      pf_token_get(pf, PF_TOKEN_NEXT);
      value_release(
          jal_parse_call(pf, jvals[JAL_VAL_TYPE_BASE], 
            JAL_PARSE_CALL_FLAG_START));
      jal_value_release(jvals);
    }
  }
}

void jal_parse_task_yield(pfile_t *pf, const pfile_pos_t *statement_start)
{
  pfile_proc_t *proc;

  UNUSED(statement_start);

  proc = pfile_proc_active_get(pf);
  if ((proc != pfile_proc_root_get(pf))
    && !pfile_proc_flag_test(proc, PFILE_PROC_FLAG_TASK)) {
    pfile_log(pf, PFILE_LOG_ERR, "_task_yield can only be used in a TASK");
  } else {
    pfile_cmd_branch_add(pf, CMD_BRANCHTYPE_TASK_YIELD,
      CMD_BRANCHCOND_NONE, LABEL_NONE, VALUE_NONE, VALUE_NONE, 0);
  }
}

/* _task_suspend / _task_resume / _task_kill / _task_restart name():
 * control another task (or, for pause/kill/restart, the running one)
 * from the main program or from a task body.  The cmd carries the
 * target task proc in brproc; the `var` field holds a 0/1 constant
 * recording whether the statement names the task it is written in
 * (the self forms emit differently, and the back ends keep no
 * proc stack at emit time). */
static void jal_parse_task_control(pfile_t *pf, cmd_branchtype_t btype)
{
  pfile_proc_t *proc;

  proc = pfile_proc_active_get(pf);
  if ((proc != pfile_proc_root_get(pf))
    && !pfile_proc_flag_test(proc, PFILE_PROC_FLAG_TASK)) {
    pfile_log(pf, PFILE_LOG_ERR,
        "task control statements can only be used in the main program or in a TASK");
  } else {
    value_t   jvals[JAL_VAL_TYPE_CT];
    boolean_t jval_found;

    jval_found = jal_value_find(pf, pf_token_get(pf, PF_TOKEN_CURRENT), jvals);
    proc = value_proc_get(jvals[JAL_VAL_TYPE_BASE]);
    if (!jval_found
        || !proc
        || !pfile_proc_flag_test(proc, PFILE_PROC_FLAG_TASK)) {
      pfile_log(pf, PFILE_LOG_ERR, "task expected");
    } else {
      variable_def_member_t mbr;
      size_t                param_ct;
      value_t              *parms;

      pf_token_get(pf, PF_TOKEN_NEXT);
      if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "(")) {
        pf_token_get(pf, PF_TOKEN_NEXT);
        if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, ")")) {
          pf_token_get(pf, PF_TOKEN_NEXT);
        }
      }
      /* cmd_branch_alloc walks the callable's members against
       * proc_params[], so give it the same implicit parms[0] result
       * slot jal_parse_call builds (tasks take no formals). */
      param_ct = 0;
      for (mbr = variable_def_member_get(
               value_def_get(jvals[JAL_VAL_TYPE_BASE]));
           mbr;
           mbr = variable_def_member_link_get(mbr)) {
        param_ct++;
      }
      if (!param_ct) {
        param_ct = 1;
      }
      parms = CALLOC(param_ct, sizeof(*parms));
      if (parms) {
        cmd_t ncmd;

        /* build the branch directly: these statements are scheduler
         * operations, not invocations -- going through
         * pfile_cmd_branch_add would add a caller->callee CALL EDGE,
         * and a task referencing itself (`_task_suspend me()`) then
         * reads as a call cycle to the reentrancy analysis, which
         * REENTRANT-flags the task (global param block + _r_ counter)
         * and breaks its leaf-frame emission. */
        ncmd = cmd_branch_alloc(btype, CMD_BRANCHCOND_NONE, LABEL_NONE,
          pfile_constant_get(pf,
            (proc == pfile_proc_active_get(pf)) ? 1 : 0,
            VARIABLE_DEF_NONE),
          jvals[JAL_VAL_TYPE_BASE], parms);
        if (ncmd) {
          pfile_cmd_add(pf, ncmd);
        }
      } else {
        pfile_log_syserr(pf, ENOMEM);
      }
    }
    jal_value_release(jvals);
  }
}

/* pre-rename keyword, kept working for old sources */
void jal_parse_task_yield_deprecated(pfile_t *pf,
    const pfile_pos_t *statement_start)
{
  pfile_log(pf, PFILE_LOG_WARN,
      "the 'suspend' keyword is deprecated; use '_task_yield'");
  jal_parse_task_yield(pf, statement_start);
}

void jal_parse_task_suspend(pfile_t *pf, const pfile_pos_t *statement_start)
{
  UNUSED(statement_start);
  jal_parse_task_control(pf, CMD_BRANCHTYPE_TASK_SUSPEND);
}

void jal_parse_task_resume(pfile_t *pf, const pfile_pos_t *statement_start)
{
  UNUSED(statement_start);
  jal_parse_task_control(pf, CMD_BRANCHTYPE_TASK_RESUME);
}

void jal_parse_task_kill(pfile_t *pf, const pfile_pos_t *statement_start)
{
  UNUSED(statement_start);
  jal_parse_task_control(pf, CMD_BRANCHTYPE_TASK_KILL);
}

void jal_parse_task_restart(pfile_t *pf, const pfile_pos_t *statement_start)
{
  UNUSED(statement_start);
  jal_parse_task_control(pf, CMD_BRANCHTYPE_TASK_RESTART);
}

