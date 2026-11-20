/************************************************************
 **
 ** pic_var.h : PIC variable allocation declarations
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 ** All rights reserved
 **
 ** Original Author: Kyle A. York
 ** Author: Matthew Schinkel, 2026
 ** Changes: minor fork edits
 **
 ************************************************************/
#ifndef pic_var_h__
#define pic_var_h__

#include "pic.h"
#include "pic_mem.h"   /* the placement request/result model */
extern unsigned pic_blist_max;   /* max amount used     */
extern unsigned pic_blist_cur;   /* current amount used */
extern unsigned pic_blist_total; /* total available */
extern unsigned pic_blist_placed; /* bytes reserved by explicit `at` placements */
unsigned pic_data_bank_free_bytes_get(void); /* bytes still free on the data bank list */

/* Fill `obj` from `var`'s current placement.  FALSE when the variable is
 * unplaced and has no chunk map (nothing to describe). */
boolean_t pic_mem_object_get(variable_t var, pic_mem_object_t *obj);

/* Physical file address of byte offset `ofs` inside the object.  Segmented
 * objects walk their segments; a contiguous object is base + ofs.  Out of
 * range clamps to the last segment's end - 1 for segmented objects (so
 * diagnostics keep pointing at the object). */

/* TRUE when the object is placed in more than one segment (the chunk maps
 * the walk emitters consult).  A map-less variable is single-segment. */
boolean_t pic_mem_object_is_chunked(variable_t var);

/*
 * F2b: the fat-object registry.  An object that a fat pointer may address
 * needs a stable id for the lifetime of the compilation (the deref dispatches
 * on it), so ids are assigned on first demand and never change.  Id 0 is
 * reserved for "linear"; ids are 1..PIC_FAT_TAG_MAX(target).
 *
 * The registry is intentionally small and refuses rather than growing past
 * what the target's tag can encode: a program with more fat objects than the
 * tag holds is outside the feature's regime and gets a diagnostic.
 */
boolean_t     pic_fat_object_register(pfile_t *pf, variable_t var,
                  unsigned *id);
variable_ct_t pic_fat_object_ct(void);
variable_t    pic_fat_object_get(unsigned id);   /* id is 1-based */

/*
 * F2d: the O(1) dispatch's data.  Created after the FINAL placement (pass 2)
 * because their contents are the placed segment bases: one descriptor per
 * fat object in the runtime chunk-map format, plus `_obj_desc[]`, a table of
 * their addresses indexed by id.  Both are filled by the preuser init
 * (pic_fat_desc_fill_emit), before the user entry runs.
 *
 * pic_fat_table_ready() is FALSE whenever that data does not exist (fat
 * pointers off, or no fat object, or a creation failure already reported):
 * the dispatch then emits nothing and the compile has failed.
 */
boolean_t     pic_fat_table_ready(void);
variable_t    pic_fat_id_get(void);              /* the dispatch's id cell */
variable_t    pic_fat_desc_tbl_get(void);        /* `_obj_desc[]` */
variable_t    pic_fat_desc_var_get(unsigned id); /* 1-based; VARIABLE_NONE */
void          pic_fat_desc_prepare(pfile_t *pf);
void          pic_fat_desc_fill_emit(pfile_t *pf);

/*
 * U1: the one memory map.  U1a makes the four free/declared lists fields of
 * one object (mechanical, behaviour unchanged); U1b adds the region table and
 * the free-segment invariants; U1c moves the allocator onto it.
 *
 *   data          free GPR segments (mutable; the allocator's free list)
 *   data_pragma   the DATA area as the device declared it (immutable, for
 *                 containment queries only)
 *   shared        free segments inside `pragma shared` (mutable; the
 *                 access-window/fallback free list)
 *   shared_pragma the declared shared ranges (immutable)
 */
typedef struct pic_mem_map_ {
  pic_bank_info_t *data;
  pic_bank_info_t *data_pragma;
  pic_bank_info_t *shared;
  pic_bank_info_t *shared_pragma;
} pic_mem_map_t;

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
void pic_var_pointer_bind_ir_set(const cmd_t cmd_head);
void pic_var_pointer_bind_escape(variable_t pvar);
boolean_t pic_var_pointer_bind_escaped_var(variable_t pvar);
boolean_t pic_var_pointer_bind_is_pvar(variable_t pvar);
variable_t pic_var_pointer_bind_walk_target(variable_t pvar);
void pic_var_pointer_bind_check_escaped(pfile_t *pf);
/* runtime chunk-map capacity: a marshalling buffer is split by RAM bank
 * nodes; the call-site store refuses (with a diagnostic) beyond this */
#define PIC_CHUNK_MAP_MAX 8

/* U3 Phase 1: the CALL command owns its open-array pairs
 * (cmd_branch_pair_t, attached by the pass-1 record site).  The capture
 * pass and the call-site descriptor store read those attachments; this
 * records the program command list once at the start of pass 2. */
void pic_var_openarray_ir_set(const cmd_t cmd_head);
/* non-vacuous acceptance comparator: re-derives the expected pair set per
 * CALL from the parameters and compares it with the attachments.  Prints
 * `pair-compare: N calls, M pairs, X mismatches`; X != 0 is an error. */
void pic_var_pair_compare(pfile_t *pf, const cmd_t cmd_head);
/* the runtime chunk map of ONE open-array formal (its own storage, filled by
 * every call site), or VARIABLE_NONE while the formal uses its baked map */
variable_t pic_var_openarray_formal_map_get(variable_t formal);
/* a formal whose call sites cannot all hand over a layout keeps the baked
 * map (the actual is a bare pointer, or the callee is reached through a
 * function pointer / as a reentrant procedure) */
void pic_var_openarray_formal_block(variable_t formal);
/* STRING PAYLOAD VIEW (docs/fixes/2026/fix_string_arg_view.md): a call site whose
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
/* a CLASSIC-core string-view formal (fed + its per-formal base cell
 * exists): even a constant subscript must walk, and even a contiguous
 * heap needs the cell -- the base is runtime data */
boolean_t pic_var_strview_classic_formal(variable_t formal);
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
/* pointer-size scratch holding the PHYSICAL address of a runtime
 * pointer-slot WHEREIS (the resolution of a fat slot's tag); created on
 * first use, after placement */
variable_t pic_var_whereis_scratch_get(pfile_t *pf);
boolean_t pic_var_escapes_to_call(variable_t var);

/* late (post-placement) allocation for emit-time scratch variables */
boolean_t pic_var_placement_done_get(void);
boolean_t pic_variable_late_alloc(pfile_t *pf, variable_t var);
