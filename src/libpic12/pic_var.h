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
void pic_var_openarray_pair_record(variable_t actual, variable_t formal,
    const pfile_pos_t *pos, boolean_t passon);
/* mark a recorded (actual, formal) pair as a string payload view (the
 * formal's runtime map gains the base-word format) */
void pic_var_openarray_pair_strview_mark(variable_t actual,
    variable_t formal);
boolean_t pic_var_openarray_pair_layout_get(variable_t actual,
    variable_ct_t *ct, variable_sz_t *sz, variable_base_t *base);
/* a PASS-ON call site copies the source formal's runtime map (it has no
 * compile-time layout of its own to hand over) */
boolean_t pic_var_openarray_pair_chain_get(variable_t actual, variable_t formal,
    variable_t *src, variable_sz_t *sz);
/* runtime chunk-map capacity: a marshalling buffer is split by RAM bank
 * nodes; the call-site store refuses (with a diagnostic) beyond this */
#define PIC_CHUNK_MAP_MAX 8
boolean_t pic_var_openarray_pair_formal_runtime_get(variable_t formal);
/* the runtime chunk map of ONE open-array formal (its own storage, filled by
 * every call site), or VARIABLE_NONE while the formal uses its baked map */
variable_t pic_var_openarray_formal_map_get(variable_t formal);
/* a formal whose call sites cannot all hand over a layout keeps the baked
 * map (the actual is a bare pointer, or the callee is reached through a
 * function pointer / as a reentrant procedure) */
void pic_var_openarray_formal_block(variable_t formal);
/* STRING PAYLOAD VIEW (fixes/fix_string_arg_view.md): a call site whose
 * open-array actual is a heap-string payload registers the actual VALUE
 * (a value of the `_string_heap` array) with the staging global that holds
 * the payload's runtime base offset; the call-site map fill copies that
 * global into the formal's map base word.  Keyed by value identity -- the
 * same value handle reaches the call emission through the cmd params. */
void pic_var_strview_site_record(value_t actual, variable_t base);
variable_t pic_var_strview_site_get(value_t actual);
/* does this formal's runtime map carry a base word (a string-view-fed
 * formal -- its map is [count][base lo,hi][base,hi,span]... and its walk
 * adds the base word to the index before chunk-walking)? */
boolean_t pic_var_openarray_formal_strview_get(variable_t formal);
/* CLASSIC 14-bit cores have no runtime-map walk: a string-view formal
 * there walks its BAKED copy of the heap's chunk map (the ladder's
 * classic path), and the payload base offset travels in a per-formal
 * cell every call site of that formal fills.  This returns the cell
 * (VARIABLE_NONE before pic_var_chunk_prepare_emit created it). */
variable_t pic_var_strview_formal_base_get(variable_t formal);
/* does ANY string payload-view pair feed this formal (all cores)? */
boolean_t pic_var_openarray_formal_strview_fed(variable_t formal);
/* place every variable that was allocated after an earlier placement round
 * again (a layout trial emits code that creates scratch, and that scratch
 * belongs to the layout the trial threw away) */
void pic_var_late_replace_all(pfile_t *pf);
/* called once, after the FINAL placement: capture each open-array pair's
 * layout, give every formal that needs per-call addressing a map of its own
 * and place the map + the walk scratch (no later placement round may move
 * them).  Layout trials re-place variables, so this must run after them. */
void pic_var_chunk_prepare_emit(pfile_t *pf);
boolean_t pic_var_chunk_walk_globals_get(pfile_t *pf, variable_t *cnt,
    variable_t *base, variable_t *span);
variable_t pic_var_chunk_walk_arg_get(void);
boolean_t pic_var_escapes_to_call(variable_t var);

/* late (post-placement) allocation for emit-time scratch variables */
boolean_t pic_var_placement_done_get(void);
boolean_t pic_variable_late_alloc(pfile_t *pf, variable_t var);
