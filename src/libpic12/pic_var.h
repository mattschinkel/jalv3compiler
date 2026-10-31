/************************************************************
 **
 ** pic_var.h : PIC variable allocation declarations
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: minor fork edits
 **
 ************************************************************/
#ifndef pic_var_h__
#define pic_var_h__

#include "pic.h"
extern unsigned pic_blist_max;   /* max amount used     */
extern unsigned pic_blist_cur;   /* current amount used */
extern unsigned pic_blist_total; /* total available */
extern unsigned pic_blist_placed; /* bytes reserved by explicit `at` placements */
unsigned pic_data_bank_free_bytes_get(void); /* bytes still free on the data bank list */

void pic_variable_alloc(pfile_t *pf);
/* total storage bytes of a def (arrays: ct * element size) */
variable_sz_t pic_variable_def_storage_need_get(pfile_t *pf,
    variable_def_t def, variable_ct_t *pointer_slots);
boolean_t pic_variable_alloc_one(pfile_t *pf, pfile_proc_t *proc,
    variable_t var);
void pic_blist_free(pfile_t *pf);
void pic_blist_info_log(pfile_t *pf);
pic_bank_info_t *pic_bank_info_get(pfile_t *pf);

/* A4: zero this module's file-scope state so a second compile in the
 * same process starts clean. Call while the module is idle (after the
 * pfile teardown). */
void pic_var_reset(void);

#endif /* pic_var_h__ */


void pic_var_call_actual_bump(variable_t var);
boolean_t pic_var_pointer_target_get(size_t ix, variable_t *var);
void pic_var_pointer_bind_record(variable_t pvar, variable_t tvar);
void pic_var_pointer_bind_escape(variable_t pvar);
boolean_t pic_var_pointer_bind_is_pvar(variable_t pvar);
boolean_t pic_var_pointer_bind_escaped_target(variable_t tvar);
variable_t pic_var_pointer_bind_walk_target(variable_t pvar);
void pic_var_pointer_bind_check_escaped(pfile_t *pf);
void pic_var_openarray_pair_record(variable_t actual, variable_t formal);
boolean_t pic_var_escapes_to_call(variable_t var);

/* late (post-placement) allocation for emit-time scratch variables */
boolean_t pic_var_placement_done_get(void);
boolean_t pic_variable_late_alloc(pfile_t *pf, variable_t var);
