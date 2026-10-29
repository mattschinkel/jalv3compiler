/************************************************************
 **
 ** jal_proc.h : JAL procedure declarations
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: minor fork edits
 **
 ************************************************************/
#ifndef jal_proc_h__
#define jal_proc_h__

#include "../libcore/pfile.h"

typedef enum {
  JAL_PARSE_PROCEDURE,
  JAL_PARSE_FUNCTION,
  JAL_PARSE_TASK
} jal_parse_proc_type;

/*
 * Parse formal parameters after the opening `(` of a procedure, function,
 * task, or pointer signature. Consumes through the closing `)`.
 * formal_name_optional: TRUE for var pointer ( ... ) signatures — formal
 * names may be omitted (e.g. byte in); FALSE for procedures/functions/tasks.
 * Parameter variables are stored for jal_parse_procedure_common fixup and
 * must be released by the caller; pointer path in jal_vdef releases temps.
 */
void jal_parse_formal_parameter_list_after_open_paren(pfile_t *pf,
  const pfile_pos_t *statement_start,
  variable_def_t proc_def,
  jal_parse_proc_type type,
  variable_t **params,
  size_t *params_ct,
  size_t *params_alloc,
  boolean_t formal_name_optional,
  boolean_t allow_optional);

void jal_parse_procedure(pfile_t *pf, const pfile_pos_t *start);
void jal_parse_function(pfile_t *pf, const pfile_pos_t *start);
void jal_parse_return(pfile_t *pf, const pfile_pos_t *start);

#define JAL_PARSE_CALL_FLAG_NONE  0x0000
#define JAL_PARSE_CALL_FLAG_PUT   0x0001
#define JAL_PARSE_CALL_FLAG_GET   0x0002
#define JAL_PARSE_CALL_FLAG_START 0x0004

value_t jal_parse_call(pfile_t *pf, value_t val, flag_t jal_parse_call_flag);

void jal_parse_task(pfile_t *pf, const pfile_pos_t *start);
void jal_parse_start(pfile_t *pf, const pfile_pos_t *start);
void jal_parse_task_yield(pfile_t *pf, const pfile_pos_t *start);
void jal_parse_task_yield_deprecated(pfile_t *pf, const pfile_pos_t *start);
void jal_parse_task_suspend(pfile_t *pf, const pfile_pos_t *start);
void jal_parse_task_resume(pfile_t *pf, const pfile_pos_t *start);
void jal_parse_task_kill(pfile_t *pf, const pfile_pos_t *start);
void jal_parse_task_restart(pfile_t *pf, const pfile_pos_t *start);

#endif /* jal_proc_h__ */

