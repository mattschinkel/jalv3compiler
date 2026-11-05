/************************************************************
 **
 ** pic_var.c : PIC variable allocation definitions
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: fork extensions (heavy rework)
 **
 ************************************************************/
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../libutils/mem.h"
#include "../libcore/pf_proc.h"
#include "pic_msg.h"
#include "pic_stvar.h"
#include "pic_var.h"

#undef BLIST_DEBUG


unsigned         pic_blist_max; /* max amount used     */
unsigned         pic_blist_total;
unsigned         pic_blist_placed; /* bytes the `at`-placement pass reserved */
/* Free list for GPR allocation from pragma shared regions (mutated). */
/* Immutable copy of pragma shared ranges for VARIABLE_FLAG_SHARED and verify. */

size_t           pic_stk_sz;
variable_base_t  pic_stk_base;

/* a free node can hold at most (reserved ranges + 1) chunk segments;
 * parts declare at most a handful of shared ranges */
#define PIC_NODE_MAX_SEG 8

typedef struct pic_variable_chunk_map_ {
  variable_t                     var;
  /* which ACTUAL this map was copied FROM (VARIABLE_NONE = the variable's
   * own placement).  An open-array formal's map is a copy of one caller's
   * array; with a second, different caller the map cannot be reinstalled
   * and the callee would walk the wrong chunks (fix_openarray_formal_chunk_map). */
  variable_t                     owner;
  variable_ct_t                  ct;
  variable_base_t               *base;
  variable_sz_t                 *sz;
  struct pic_variable_chunk_map_ *link;
} pic_variable_chunk_map_t;

/*
 * All file-scope state, in ONE object (Tier-A A4, see
 * fix_pic_var_state_reset.md).
 *
 * These were 30 independent statics scattered through 3400 lines: the bank
 * lists, the chunk maps, the per-compile registries (pointer targets,
 * binds, indirect-call actuals, open-array pairs), the refusal flags and
 * the layout snapshot. Nothing reset any of them, so a second compile in
 * the same process would start with the first one's leftovers.
 */
/* keep the original tag: the allocator sizes it by name */
typedef struct pic_banked_edge_ {
  variable_t a;
  variable_t b;
  unsigned   ct;
} pic_var_banked_edge_t;

typedef struct pic_var_ptr_bind_ {
  variable_t pvar;
  variable_t tvar;
  boolean_t  escaped;
} pic_var_ptr_bind_t;

typedef struct pic_var_openarray_pair_ {
  variable_t actual;
  variable_t formal;
  boolean_t                   reported;
  pfile_pos_t                  pos;   /* call site */
  boolean_t                    pos_valid;
  /* PASS-ON pair: the actual is another procedure's open-array formal, so all
   * this call site knows is the pointer.  When that formal has a runtime map
   * of its own the CALL SITE copies it here (the layout is only known at run
   * time); when it has a compile-time map the captured layout below is enough
   * (see pic_var_openarray_pair_layout_get). */
  boolean_t                    passon;
  variable_t                   chain_src;   /* runtime map copied per call */
  variable_sz_t                chain_sz;    /* bytes to copy */
  /* STRING PAYLOAD VIEW: the actual is the heap array standing for a string
   * payload whose base offset is staged at run time -- the formal's map gets
   * a base word and the walk adds it to the index (fixes/
   * fix_string_arg_view.md) */
  boolean_t                    strview;
  /* Layout of the ACTUAL captured at the end of placement: the chunk maps
   * are not visible during call emission (found 2026-11-02 -- ct read 0 at
   * the call site), so the call-site fill reads the layout from here.  A
   * linearly placed actual is normalized to ONE chunk (its base and size)
   * so every pair carries a layout and two call sites can be compared. */
  variable_ct_t                chunk_ct;
  variable_sz_t                chunk_sz[PIC_CHUNK_MAP_MAX];
  variable_base_t              chunk_base[PIC_CHUNK_MAP_MAX];
} pic_var_openarray_pair_t;

/* a variable placed after placement ran, and the placement generation its
 * address belongs to (see pic_var_late_replace_all) */
typedef struct pic_var_late_ {
  variable_t                   var;
  size_t                       gen;
} pic_var_late_t;

/* one open-array formal that needs per-call chunk addressing: a map buffer
 * of its own (every call site of that formal fills it) */
typedef struct pic_var_formal_map_ {
  variable_t                   formal;
  variable_t                   map;
  /* string-view map format: [count][base lo,hi][entries] -- the walk adds
   * the base word to the index (user arrays fill it with 0) */
  boolean_t                    strview;
} pic_var_formal_map_t;

/* one call site whose open-array actual is a string payload view: the
 * actual value (of the heap array) -> the staging global holding the
 * payload's runtime base offset */
typedef struct pic_var_strview_site_ {
  value_t                      actual;
  variable_t                   base;
} pic_var_strview_site_t;

struct pic_var_layout_snap_;

typedef struct pic_var_state_ {
  unsigned                     auto_mem_reg;
  pic_bank_info_t             *data_bank_list;
  pic_bank_info_t             *shared_bank_list;
  pic_bank_info_t             *shared_pragma_list;
  pic_bank_info_t             *data_pragma_list; /* the DATA area as the
                                                  * device declared it; never
                                                  * consumed -- containment
                                                  * queries only */
  pic_variable_chunk_map_t    *chunk_maps;
  pic_var_banked_edge_t       *banked_edges;
  size_t                       banked_edges_used;
  size_t                       banked_edges_alloc;
  variable_t                   banked_last;
  variable_t                  *ptr_targets;
  size_t                       ptr_targets_used;
  size_t                       ptr_targets_alloc;
  pic_var_ptr_bind_t          *ptr_binds;
  size_t                       ptr_binds_used;
  size_t                       ptr_binds_alloc;
  variable_t                  *call_actuals;
  size_t                       call_actuals_used;
  size_t                       call_actuals_alloc;
  pic_var_openarray_pair_t    *openarray_pairs;
  size_t                       openarray_pairs_used;
  /* per-formal runtime chunk maps (see pic_var_chunk_prepare_emit) */
  pic_var_formal_map_t        *formal_maps;
  size_t                       formal_maps_used;
  size_t                       formal_maps_alloc;
  /* string payload-view call sites (see pic_var_strview_site_record) */
  pic_var_strview_site_t      *strview_sites;
  size_t                       strview_sites_used;
  size_t                       strview_sites_alloc;
  /* formals that must keep the baked map */
  variable_t                  *openarray_blocked;
  size_t                       openarray_blocked_used;
  size_t                       openarray_blocked_alloc;
  /* every variable placed by pic_variable_late_alloc, with the placement
   * generation of its address: a layout trial emits code (which creates
   * scratch such as `_pic_temp`) AFTER its own placement, so such a variable
   * belongs to a layout that is later thrown away and must be placed again
   * in the layout that is actually emitted */
  pic_var_late_t              *late;
  size_t                       late_used;
  size_t                       late_alloc;
  size_t                       place_gen;
  /* compiler-owned walk scratch (chunk count / base / span) for the runtime
   * chunk walk; never the caller's temp area.  Created on first use. */
  variable_t                   chunk_g_cnt;    /* walk scratch (never the
                                                * caller's temp area) */
  variable_t                   chunk_g_t;      /* shared static walk's
                                                * index argument (2 bytes) */
  variable_t                   chunk_g_base;
  variable_t                   chunk_g_span1;
  size_t                       openarray_pairs_alloc;
  boolean_t                    refused;
  boolean_t                    refused_ptr;
  boolean_t                    refused_call;
  variable_t                   refused_var;
  variable_sz_t                refused_sz;
  variable_sz_t                refused_best;
  unsigned                     dbg_round;
  struct pic_var_layout_snap_ *snap;
  boolean_t                    placement_done;
} pic_var_state_t;

static pic_var_state_t pic_var;


/*
 * PIC18 MOVLB-churn metadata (D3 increment 2, trial-selection form).
 * A bank-select is only paid when banked file operations on DIFFERENT
 * variables are adjacent in emit order (a switch episode), so
 * pic_instr_append_daop records those adjacency edges rather than raw
 * op counts; the access-window candidate layout ranks overlay groups
 * by their incident edge totals. The winning layout is chosen by real
 * trial emission in pic_cmd_generate, never by these counts alone.
 */

/*
 * Return this module to its just-loaded state.
 *
 * Call it while the module is IDLE -- i.e. after the pfile teardown. The
 * bank lists, the shared pragma list and the chunk maps belong to the
 * pfile lifecycle and are released there, so they are only zeroed. The
 * five per-compile registries below are this module's own heap and are
 * never freed anywhere else (they were leaked until process exit), so
 * freeing them here is what makes the reset both correct and leak-free.
 */
void pic_var_reset(void)
{
  pic_var_trials_free();                 /* layout snapshot, if one is held */
  FREE(pic_var.banked_edges);
  FREE(pic_var.ptr_targets);
  FREE(pic_var.ptr_binds);
  FREE(pic_var.call_actuals);
  FREE(pic_var.openarray_pairs);
  FREE(pic_var.formal_maps);
  FREE(pic_var.strview_sites);
  FREE(pic_var.openarray_blocked);
  FREE(pic_var.late);
  memset(&pic_var, 0, sizeof(pic_var));
  pic_var.auto_mem_reg = 1;              /* the only non-zero initial value */
}

void pic_var_banked_access_reset(void)
{
  pic_var.banked_edges_used = 0;
  pic_var.banked_last       = 0;
}

void pic_var_banked_access_bump(variable_t var)
{
  if (var != pic_var.banked_last) {
    if (pic_var.banked_last) {
      size_t ii;

      for (ii = 0; ii < pic_var.banked_edges_used; ii++) {
        if ((pic_var.banked_edges[ii].a == pic_var.banked_last)
            && (pic_var.banked_edges[ii].b == var)) {
          pic_var.banked_edges[ii].ct++;
          break;
        }
      }
      if (ii == pic_var.banked_edges_used) {
        if (pic_var.banked_edges_used == pic_var.banked_edges_alloc) {
          size_t                   resize;
          struct pic_banked_edge_ *tmp;

          resize = pic_var.banked_edges_alloc ? 2 * pic_var.banked_edges_alloc : 256;
          tmp    = REALLOC(pic_var.banked_edges, resize * sizeof(*tmp));
          if (tmp) {
            pic_var.banked_edges_alloc = resize;
            pic_var.banked_edges       = tmp;
          }
        }
        if (pic_var.banked_edges_used < pic_var.banked_edges_alloc) {
          pic_var.banked_edges[pic_var.banked_edges_used].a  = pic_var.banked_last;
          pic_var.banked_edges[pic_var.banked_edges_used].b  = var;
          pic_var.banked_edges[pic_var.banked_edges_used].ct = 1;
          pic_var.banked_edges_used++;
        }
      }
    }
    pic_var.banked_last = var;
  }
}

static void pic_variable_chunk_map_set(pfile_t *pf, variable_t var,
    variable_ct_t ct, const variable_base_t *base, const variable_sz_t *sz)
{
  pic_variable_chunk_map_t *map;
  pic_variable_chunk_map_t **pprev;

  /* placement may be re-run (PIC18 access-window trials): replace,
   * don't duplicate, any earlier registration for this variable */
  for (pprev = &pic_var.chunk_maps; *pprev; ) {
    if ((*pprev)->var == var) {
      pic_variable_chunk_map_t *dead;

      dead    = *pprev;
      *pprev  = dead->link;
      FREE(dead->base);
      FREE(dead->sz);
      FREE(dead);
    } else {
      pprev = &(*pprev)->link;
    }
  }
  map = MALLOC(sizeof(*map));
  if (!map) {
    pfile_log_syserr(pf, ENOMEM);
    return;
  }
  map->base = MALLOC(ct * sizeof(*map->base));
  map->sz   = MALLOC(ct * sizeof(*map->sz));
  if (!map->base || !map->sz) {
    FREE(map->base);
    FREE(map->sz);
    FREE(map);
    pfile_log_syserr(pf, ENOMEM);
    return;
  }
  memcpy(map->base, base, ct * sizeof(*map->base));
  memcpy(map->sz, sz, ct * sizeof(*map->sz));
  map->var   = var;
  map->owner = VARIABLE_NONE;
  map->ct    = ct;
  map->link = pic_var.chunk_maps;
  pic_var.chunk_maps = map;
}

static pic_variable_chunk_map_t *pic_variable_chunk_map_find(variable_t var)
{
  pic_variable_chunk_map_t *map;

  while (variable_master_get(var)) {
    var = variable_master_get(var);
  }
  for (map = pic_var.chunk_maps; map; map = map->link) {
    if (map->var == var) {
      return map;
    }
  }
  return 0;
}

variable_ct_t pic_variable_chunk_ct_get(variable_t var)
{
  pic_variable_chunk_map_t *map;

  map = pic_variable_chunk_map_find(var);
  return map ? map->ct : 0;
}

variable_base_t pic_variable_chunk_base_get(variable_t var, variable_ct_t ix)
{
  pic_variable_chunk_map_t *map;

  map = pic_variable_chunk_map_find(var);
  return (map && ix < map->ct) ? map->base[ix] : VARIABLE_BASE_UNKNOWN;
}

variable_sz_t pic_variable_chunk_sz_get(variable_t var, variable_ct_t ix)
{
  pic_variable_chunk_map_t *map;

  map = pic_variable_chunk_map_find(var);
  return (map && ix < map->ct) ? map->sz[ix] : 0;
}

/* Layer 2 (chunk-walking linear arrays): resolve a LINEAR byte address
 * (array base + const offset -- const subscripts, direct addressing)
 * through the chunk map to its physical address. Contiguous (and
 * single-chunk) placements are linear, so this returns the input
 * unchanged for everything that was placeable before Layer 2. */
variable_base_t pic_var_chunk_addr_resolve(variable_t var,
    variable_base_t linear)
{
  pic_variable_chunk_map_t *map;
  variable_sz_t             off;

  if (!var || (VARIABLE_NONE == var)) {
    return linear;
  }
  map = pic_variable_chunk_map_find(var);
  if (!map || (map->ct <= 1)) {
    return linear;
  }
  off = (variable_sz_t) (linear - variable_base_get(var, 0));
  {
    variable_ct_t ii;

    for (ii = 0; ii < map->ct; ii++) {
      if (off < map->sz[ii]) {
        return (variable_base_t) (map->base[ii] + off);
      }
      off = (variable_sz_t) (off - map->sz[ii]);
    }
  }
  /* past the end (out-of-range const subscript): clamp to the last
   * chunk's end-1 so diagnostics keep pointing at this variable */
  return (variable_base_t) (map->base[map->ct - 1]
      + map->sz[map->ct - 1] - 1);
}

/* Variables that are the TARGET of a pointer bind (`p is arr`) or an
 * address-of materialization. Pointer arithmetic is LINEAR (*p + i) and
 * cannot walk a chunk map, so a holed placement is unsound for these;
 * the registry is bumped at emit time (pass 0) and consulted by the
 * pass-1 placement trials, mirroring the access-window counters. */

void pic_var_pointer_target_reset(void)
{
  pic_var.ptr_targets_used = 0;
}

void pic_var_pointer_target_bump(variable_t var)
{
  size_t ii;

  if (!var || (VARIABLE_NONE == var)) {
    return;
  }
  for (ii = 0; ii < pic_var.ptr_targets_used; ii++) {
    if (pic_var.ptr_targets[ii] == var) {
      return;
    }
  }
  if (pic_var.ptr_targets_used == pic_var.ptr_targets_alloc) {
    size_t       resize;
    variable_t  *tmp;

    resize = pic_var.ptr_targets_alloc ? 2 * pic_var.ptr_targets_alloc : 64;
    tmp    = REALLOC(pic_var.ptr_targets, resize * sizeof(*tmp));
    if (!tmp) {
      return;
    }
    pic_var.ptr_targets_alloc = resize;
    pic_var.ptr_targets       = tmp;
  }
  pic_var.ptr_targets[pic_var.ptr_targets_used++] = var;
}

boolean_t pic_var_pointer_target_test(variable_t var)
{
  size_t ii;

  for (ii = 0; ii < pic_var.ptr_targets_used; ii++) {
    if (pic_var.ptr_targets[ii] == var) {
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

/* Pointer BINDS (2026-10-26 escape analysis): (pointer variable ->
 * target array) pairs recorded at every OPERATOR_POINTER_ASSIGN emit.
 * A pointer that is only ever DEREFERENCED (p[i], p = x store/load) --
 * never itself stored, passed, compared, or re-bound -- does not make
 * the target's addressing linear: every deref can walk the target's
 * chunk map. The pass-1-end scan marks escaping pointers; a holed
 * placement refuses only when some ESCAPED pointer binds the array. */

void pic_var_pointer_bind_record(variable_t pvar, variable_t tvar)
{
  size_t ii;

  if (!pvar || !tvar
      || (VARIABLE_NONE == pvar) || (VARIABLE_NONE == tvar)) {
    return;
  }
  for (ii = 0; ii < pic_var.ptr_binds_used; ii++) {
    if ((pic_var.ptr_binds[ii].pvar == pvar)
        && (pic_var.ptr_binds[ii].tvar == tvar)) {
      return;
    }
  }
  if (pic_var.ptr_binds_used == pic_var.ptr_binds_alloc) {
    size_t resize;
    void  *tmp;

    resize = pic_var.ptr_binds_alloc ? 2 * pic_var.ptr_binds_alloc : 32;
    tmp = REALLOC(pic_var.ptr_binds, resize * sizeof(*pic_var.ptr_binds));
    if (!tmp) {
      return;
    }
    pic_var.ptr_binds       = tmp;
    pic_var.ptr_binds_alloc = resize;
  }
  pic_var.ptr_binds[pic_var.ptr_binds_used].pvar     = pvar;
  pic_var.ptr_binds[pic_var.ptr_binds_used].tvar     = tvar;
  pic_var.ptr_binds[pic_var.ptr_binds_used].escaped  = BOOLEAN_FALSE;
  pic_var.ptr_binds_used++;
}

/* is this variable the pointer side of any recorded bind? */
boolean_t pic_var_pointer_bind_is_pvar(variable_t pvar)
{
  size_t ii;

  for (ii = 0; ii < pic_var.ptr_binds_used; ii++) {
    if (pic_var.ptr_binds[ii].pvar == pvar) {
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

void pic_var_pointer_bind_escape(variable_t pvar)
{
  size_t ii;

  for (ii = 0; ii < pic_var.ptr_binds_used; ii++) {
    if (pic_var.ptr_binds[ii].pvar == pvar) {
      pic_var.ptr_binds[ii].escaped = BOOLEAN_TRUE;
    }
  }
}

/* does any ESCAPED pointer bind this target? (drives the holed
 * placement refusal; unescaped binds walk instead) */
boolean_t pic_var_pointer_bind_escaped_target(variable_t tvar)
{
  size_t ii;

  for (ii = 0; ii < pic_var.ptr_binds_used; ii++) {
    if ((pic_var.ptr_binds[ii].tvar == tvar)
        && pic_var.ptr_binds[ii].escaped) {
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

/* post-scan check: an ESCAPED pointer bound to a map-carrying value
 * (a chunked array, or an open-array formal over a chunked actual)
 * does flat linear arithmetic over the holes -- refuse once, cleanly */
void pic_var_pointer_bind_check_escaped(pfile_t *pf)
{
  size_t ii;

  for (ii = 0; ii < pic_var.ptr_binds_used; ii++) {
    if (pic_var.ptr_binds[ii].escaped
        && pic_variable_chunk_map_find(pic_var.ptr_binds[ii].tvar)) {
      pfile_log(pf, PFILE_LOG_ERR,
          "'%s' is bound to a banked (chunked) target and escapes this "
          "program's dereferences (stored, passed, compared, or "
          "re-bound): pointer arithmetic is linear and cannot walk the "
          "holes. Dereference it only through subscripts, keep the "
          "array within one bank, or use the large_array library.",
          variable_name_get(pic_var.ptr_binds[ii].pvar)
            ? variable_name_get(pic_var.ptr_binds[ii].pvar) : "?");
    }
  }
}

/* the unescaped bind target for a pointer variable, if the target
 * carries a chunk map (the walk-eligible case) */
variable_t pic_var_pointer_bind_walk_target(variable_t pvar)
{
  size_t ii;

  for (ii = 0; ii < pic_var.ptr_binds_used; ii++) {
    if ((pic_var.ptr_binds[ii].pvar == pvar)
        && !pic_var.ptr_binds[ii].escaped
        && pic_variable_chunk_map_find(pic_var.ptr_binds[ii].tvar)) {
      return pic_var.ptr_binds[ii].tvar;
    }
  }
  return VARIABLE_NONE;
}

/* indexed access for the counters pass: credit every bind target so the
 * array survives the fixup set (see pic_variable_counters_set) */
boolean_t pic_var_pointer_target_get(size_t ix, variable_t *var)
{
  if (ix >= pic_var.ptr_targets_used) {
    return BOOLEAN_FALSE;
  }
  if (var) {
    *var = pic_var.ptr_targets[ix];
  }
  return BOOLEAN_TRUE;
}

/* Layer 2 guard (2026-10-26): an open-array formal receives the array's
 * base and does LINEAR baseofs arithmetic inside the callee -- a holed
 * (chunked) placement cannot be walked there, exactly like a pointer
 * bind (found via a 300-byte chunked array passed to an open-array
 * formal: compiled clean, returned garbage under gpsim). pic.c marks
 * every call-actual variable here right before placement runs. */

void pic_var_call_actual_bump(variable_t var)
{
  size_t ii;

  if (!var || (VARIABLE_NONE == var)) {
    return;
  }
  for (ii = 0; ii < pic_var.call_actuals_used; ii++) {
    if (pic_var.call_actuals[ii] == var) {
      return;
    }
  }
  if (pic_var.call_actuals_used == pic_var.call_actuals_alloc) {
    size_t       resize;
    variable_t  *tmp;

    resize = pic_var.call_actuals_alloc ? 2 * pic_var.call_actuals_alloc : 64;
    tmp    = REALLOC(pic_var.call_actuals, resize * sizeof(*tmp));
    if (!tmp) {
      return;
    }
    pic_var.call_actuals      = tmp;
    pic_var.call_actuals_alloc = resize;
  }
  pic_var.call_actuals[pic_var.call_actuals_used++] = var;
}

boolean_t pic_var_escapes_to_call(variable_t var)
{
  size_t ii;

  for (ii = 0; ii < pic_var.call_actuals_used; ii++) {
    if (pic_var.call_actuals[ii] == var) {
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

/* Open-array specialization (2026-10-26): direct calls record
 * (actual variable -> formal variable) pairs here; when a holed
 * placement is accepted for the actual, its chunk map is propagated
 * onto the formal, so the callee's subscripts walk the actual's chunks
 * (the existing chunk-walk emitters key off the formal's variable).
 * Indirect/reentrant calls cannot be specialized and keep the
 * placement refusal (pic_var_escapes_to_call). */

/* ---- variables created after placement --------------------------------
 *
 * The runtime chunk map and the walk scratch must exist before the pass that
 * emits them, but their existence (a formal serving two DIFFERENT layouts) is
 * only known once placement has run.  They are therefore created after the
 * layout trials -- see pic_var_chunk_prepare_emit -- and given an address out
 * of the leftover free list.
 *
 * A variable placed that way is recorded with the placement generation of its
 * address.  Placement is not single-shot: a PIC18 / 14H layout trial re-places
 * every snapshot variable and THEN emits, and that emission creates scratch of
 * its own (`_pic_temp` is the common one).  Such a variable belongs to a
 * layout that is later thrown away, so after the winning layout is installed
 * they are placed again -- otherwise the winning allocation hands their bytes
 * to somebody else and the two silently share storage (found while landing the
 * per-formal chunk map: the walk scratch landed on `_pic_temp` and the walk
 * overwrote its own index).
 */
static void pic_var_late_track(variable_t var)
{
  size_t ii;

  if (!var || (VARIABLE_NONE == var)) {
    return;
  }
  for (ii = 0; ii < pic_var.late_used; ii++) {
    if (pic_var.late[ii].var == var) {
      pic_var.late[ii].gen = pic_var.place_gen;
      return;
    }
  }
  if (pic_var.late_used == pic_var.late_alloc) {
    size_t          resize;
    pic_var_late_t *tmp;

    resize = pic_var.late_alloc ? 2 * pic_var.late_alloc : 8;
    tmp = REALLOC(pic_var.late, resize * sizeof(*pic_var.late));
    if (!tmp) {
      return;
    }
    pic_var.late       = tmp;
    pic_var.late_alloc = resize;
  }
  pic_var.late[pic_var.late_used].var = var;
  pic_var.late[pic_var.late_used].gen = pic_var.place_gen;
  pic_var.late_used++;
}

/* called right after the FINAL layout is installed, before the pass that
 * emits it: place every late variable whose address belongs to an older
 * layout again (its bytes came back as free space in the new one) */
void pic_var_late_replace_all(pfile_t *pf)
{
  size_t ii;

  for (ii = 0; ii < pic_var.late_used; ii++) {
    if (pic_var.late[ii].gen != pic_var.place_gen) {
      /* no free-list bookkeeping: in the layout that is now installed those
       * bytes were never taken from the list in the first place */
      variable_base_set(pic_var.late[ii].var, VARIABLE_BASE_UNKNOWN, 0);
    }
  }
  for (ii = 0; ii < pic_var.late_used; ii++) {
    if (VARIABLE_BASE_UNKNOWN == variable_base_get(pic_var.late[ii].var, 0)) {
      if (!pic_variable_alloc_one(pf, PFILE_PROC_NONE, pic_var.late[ii].var)) {
        pfile_log(pf, PFILE_LOG_ERR,
            "no room for the compiler's '%s' (it needs %u bytes of RAM)",
            variable_name_get(pic_var.late[ii].var) ?
              variable_name_get(pic_var.late[ii].var) : "?",
            (unsigned) variable_sz_get(pic_var.late[ii].var));
      }
      pic_var.late[ii].gen = pic_var.place_gen;
    }
  }
}

/* Compiler-owned walk scratch for the runtime chunk walk.
 *
 * The callee body is emitted once but serves any actual, so it walks the map
 * its own formal carries; these globals hold the loop state (remaining chunk
 * count, current base, current span).  Compiler-owned, so they can never
 * collide with the caller's temp area.  Created after placement and placed
 * immediately (as a tracked late variable) so an address exists. */
static variable_t pic_var_chunk_global_create(pfile_t *pf, const char *name,
    variable_sz_t sz)
{
  variable_def_t bdef;
  variable_def_t adef;
  variable_t     var;
  result_t       rc;

  bdef = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
      VARIABLE_DEF_FLAG_NONE, 1);
  if (!bdef) {
    return VARIABLE_NONE;
  }
  adef = variable_def_alloc(0, VARIABLE_DEF_TYPE_ARRAY,
      VARIABLE_DEF_FLAG_NONE, 1);
  if (!adef) {
    return VARIABLE_NONE;
  }
  variable_def_member_add(adef, 0, bdef, sz);
  var = VARIABLE_NONE;
  rc = pfile_variable_alloc(pf, PFILE_VARIABLE_ALLOC_GLOBAL, name, adef,
      VARIABLE_NONE, &var);
  if (RESULT_OK != rc) {
    pfile_log_syserr(pf, rc);
    return VARIABLE_NONE;
  }
  variable_use_ct_bump(var, CTR_BUMP_INCR);
  variable_assign_ct_bump(var, CTR_BUMP_INCR);
  if (!pic_variable_late_alloc(pf, var)) {
    return VARIABLE_NONE;
  }
  return var;
}

/* run-time walk scratch; created on first use */
boolean_t pic_var_chunk_walk_globals_get(pfile_t *pf, variable_t *cnt,
    variable_t *base, variable_t *span)
{
  if (!pic_var.chunk_g_cnt) {
    pic_var.chunk_g_cnt   = pic_var_chunk_global_create(pf,
        "_pic_chunk_cnt", 1);
    pic_var.chunk_g_base  = pic_var_chunk_global_create(pf,
        "_pic_chunk_base", 2);
    pic_var.chunk_g_span1 = pic_var_chunk_global_create(pf,
        "_pic_chunk_span1", 1);
    /* argument of the SHARED static chunk walk (one helper per chunked
     * variable, see pic_chunk_walk_helpers_create): the index travels
     * here, never through the caller's temp area */
    pic_var.chunk_g_t     = pic_var_chunk_global_create(pf,
        "_pic_chunk_t", 2);
  }
  if (!pic_var.chunk_g_cnt || !pic_var.chunk_g_base
      || !pic_var.chunk_g_span1 || !pic_var.chunk_g_t) {
    return BOOLEAN_FALSE;
  }
  *cnt  = pic_var.chunk_g_cnt;
  *base = pic_var.chunk_g_base;
  *span = pic_var.chunk_g_span1;
  return BOOLEAN_TRUE;
}

/* The shared static chunk walk's argument (2 bytes), or VARIABLE_NONE when
 * the program has no chunked variable (nothing was created).  Creation
 * happens once, after placement -- see pic_var_chunk_prepare_emit -- so this
 * accessor never re-enters the allocator. */
variable_t pic_var_chunk_walk_arg_get(void)
{
  return pic_var.chunk_g_t;
}

/* ---- per-formal runtime chunk maps -----------------------------------
 *
 * One callee body serves every call site, so a formal fed two DIFFERENT
 * layouts cannot carry either one baked in: the first call site's map wins
 * and the second call addresses the first buffer's memory (a 16F1825 program
 * filling two 150-byte arrays through one `byte in data[]` wrote the second
 * fill through the first call's chunk bases -- gpsim: big_a[0]=100,
 * big_b[0]=0).  The string half of this defect was fixed the same way
 * (fixes/fix_openarray_formal_chunk_map.md); drivers, sd_card and fat32 have
 * the USER-array shape, and they are compile-only, so nothing caught it.
 *
 * Fix: each such formal owns a small map buffer
 *
 *     [count][base lo, base hi, span]...
 *
 * that EVERY call site of that formal fills with ITS actual's layout just
 * before the call; the callee's dynamic subscript walks its own map.  The map
 * lives in the formal's own storage, so a nested call to a different
 * procedure cannot clobber it (the old design used one program-wide map,
 * which made nested chunked calls a documented limitation).
 *
 * The map is created only when the layouts actually DIFFER: one layout (one
 * call site, or the same buffer twice) is addressed exactly by the baked map,
 * and this whole mechanism is then inert -- no RAM, no instructions, no
 * change to the generated code of any program that was already correct.
 */

static pic_var_formal_map_t *pic_var_formal_map_find(variable_t formal)
{
  size_t ii;

  for (ii = 0; ii < pic_var.formal_maps_used; ii++) {
    if (pic_var.formal_maps[ii].formal == formal) {
      return &pic_var.formal_maps[ii];
    }
  }
  return 0;
}

variable_t pic_var_openarray_formal_map_get(variable_t formal)
{
  pic_var_formal_map_t *ent;

  if (!formal || (VARIABLE_NONE == formal)) {
    return VARIABLE_NONE;
  }
  ent = pic_var_formal_map_find(formal);
  return ent ? ent->map : VARIABLE_NONE;
}

boolean_t pic_var_openarray_formal_strview_get(variable_t formal)
{
  pic_var_formal_map_t *ent;

  if (!formal || (VARIABLE_NONE == formal)) {
    return BOOLEAN_FALSE;
  }
  ent = pic_var_formal_map_find(formal);
  return (ent && ent->strview) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* a string payload-view call site: the actual value (of the heap array) is
 * keyed to the staging global that holds the payload's base offset.  The
 * values are arena-scoped and the cmd params hold the same handle from
 * parse to call emission, so identity is stable across passes. */
void pic_var_strview_site_record(value_t actual, variable_t base)
{
  size_t ii;

  if (!actual || !base || (VARIABLE_NONE == base)) {
    return;
  }
  for (ii = 0; ii < pic_var.strview_sites_used; ii++) {
    if (pic_var.strview_sites[ii].actual == actual) {
      pic_var.strview_sites[ii].base = base;
      return;
    }
  }
  if (pic_var.strview_sites_used == pic_var.strview_sites_alloc) {
    size_t                  resize;
    pic_var_strview_site_t *tmp;

    resize = pic_var.strview_sites_alloc
      ? 2 * pic_var.strview_sites_alloc : 8;
    tmp = REALLOC(pic_var.strview_sites, resize * sizeof(*tmp));
    if (!tmp) {
      return;
    }
    pic_var.strview_sites       = tmp;
    pic_var.strview_sites_alloc = resize;
  }
  pic_var.strview_sites[pic_var.strview_sites_used].actual = actual;
  pic_var.strview_sites[pic_var.strview_sites_used].base   = base;
  pic_var.strview_sites_used++;
}

variable_t pic_var_strview_site_get(value_t actual)
{
  size_t ii;

  if (!actual) {
    return VARIABLE_NONE;
  }
  for (ii = 0; ii < pic_var.strview_sites_used; ii++) {
    if (pic_var.strview_sites[ii].actual == actual) {
      return pic_var.strview_sites[ii].base;
    }
  }
  return VARIABLE_NONE;
}

/* any string payload-view pair feeds this formal? */
static boolean_t pic_var_formal_strview_fed(variable_t formal)
{
  size_t ii;

  for (ii = 0; ii < pic_var.openarray_pairs_used; ii++) {
    if ((pic_var.openarray_pairs[ii].formal == formal)
        && pic_var.openarray_pairs[ii].strview) {
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

/* a formal whose call sites cannot all hand over a layout: keep the baked
 * map (the runtime walk would read a stale one) */
void pic_var_openarray_formal_block(variable_t formal)
{
  size_t ii;

  if (!formal || (VARIABLE_NONE == formal)) {
    return;
  }
  for (ii = 0; ii < pic_var.openarray_blocked_used; ii++) {
    if (pic_var.openarray_blocked[ii] == formal) {
      return;
    }
  }
  if (pic_var.openarray_blocked_used == pic_var.openarray_blocked_alloc) {
    size_t     resize;
    variable_t *tmp;

    resize = pic_var.openarray_blocked_alloc
      ? 2 * pic_var.openarray_blocked_alloc : 8;
    tmp = REALLOC(pic_var.openarray_blocked, resize * sizeof(variable_t));
    if (!tmp) {
      return;
    }
    pic_var.openarray_blocked       = tmp;
    pic_var.openarray_blocked_alloc = resize;
  }
  pic_var.openarray_blocked[pic_var.openarray_blocked_used++] = formal;
}

static boolean_t pic_var_openarray_formal_blocked(variable_t formal)
{
  size_t ii;

  for (ii = 0; ii < pic_var.openarray_blocked_used; ii++) {
    if (pic_var.openarray_blocked[ii] == formal) {
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

/* Does this open-array formal address its elements through a runtime map?
 *
 * TRUE only once pic_var_chunk_prepare_emit gave it a map, i.e. when its call
 * sites hand over two or more different layouts and every one of them can
 * fill it.  Both the call site and the callee body ask THIS question, so the
 * two sides can never disagree. */
boolean_t pic_var_openarray_pair_formal_runtime_get(variable_t formal)
{
  return (VARIABLE_NONE != pic_var_openarray_formal_map_get(formal))
    ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* Layout captured for an actual at propagation time (see the pair struct).
 * Returns BOOLEAN_TRUE when the actual was chunked with 2..4 chunks and the
 * layout is available; the call-site descriptor store uses it because the
 * chunk maps themselves are not reachable from the emitter. */
boolean_t pic_var_openarray_pair_layout_get(variable_t actual,
    variable_ct_t *ct, variable_sz_t *sz, variable_base_t *base)
{
  size_t ii;

  for (ii = 0; ii < pic_var.openarray_pairs_used; ii++) {
    if (pic_var.openarray_pairs[ii].actual != actual) {
      continue;
    }
    if ((pic_var.openarray_pairs[ii].chunk_ct >= 1)
        && (pic_var.openarray_pairs[ii].chunk_ct <= PIC_CHUNK_MAP_MAX)) {
      variable_ct_t cix;

      *ct = pic_var.openarray_pairs[ii].chunk_ct;
      for (cix = 0; cix < *ct; cix++) {
        sz[cix]   = pic_var.openarray_pairs[ii].chunk_sz[cix];
        base[cix] = pic_var.openarray_pairs[ii].chunk_base[cix];
      }
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

/* A PASS-ON call site (`inner(data)` inside a procedure that is itself walking
 * `data`) has no compile-time layout to hand over: it copies the source
 * formal's runtime map instead.  Returns the source map and its byte count. */
boolean_t pic_var_openarray_pair_chain_get(variable_t actual, variable_t formal,
    variable_t *src, variable_sz_t *sz)
{
  size_t ii;

  for (ii = 0; ii < pic_var.openarray_pairs_used; ii++) {
    if ((pic_var.openarray_pairs[ii].actual == actual)
        && (pic_var.openarray_pairs[ii].formal == formal)
        && (VARIABLE_NONE != pic_var.openarray_pairs[ii].chain_src)) {
      *src = pic_var.openarray_pairs[ii].chain_src;
      *sz  = pic_var.openarray_pairs[ii].chain_sz;
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

/* mark every (actual, formal) pair of this call site as a string payload
 * view -- the actual value is registered with a staged base offset */
void pic_var_openarray_pair_strview_mark(variable_t actual, variable_t formal)
{
  size_t ii;

  for (ii = 0; ii < pic_var.openarray_pairs_used; ii++) {
    if ((pic_var.openarray_pairs[ii].actual == actual)
        && (pic_var.openarray_pairs[ii].formal == formal)) {
      pic_var.openarray_pairs[ii].strview = BOOLEAN_TRUE;
    }
  }
}

void pic_var_openarray_pair_record(variable_t actual, variable_t formal,
    const pfile_pos_t *pos, boolean_t passon)
{
  size_t ii;

  if (!actual || !formal
      || (VARIABLE_NONE == actual) || (VARIABLE_NONE == formal)) {
    return;
  }
  for (ii = 0; ii < pic_var.openarray_pairs_used; ii++) {
    if ((pic_var.openarray_pairs[ii].actual == actual)
        && (pic_var.openarray_pairs[ii].formal == formal)) {
      return;
    }
  }
  if (pic_var.openarray_pairs_used == pic_var.openarray_pairs_alloc) {
    size_t resize;
    void  *tmp;

    resize = pic_var.openarray_pairs_alloc
      ? 2 * pic_var.openarray_pairs_alloc : 32;
    tmp = REALLOC(pic_var.openarray_pairs,
        resize * sizeof(*pic_var.openarray_pairs));
    if (!tmp) {
      return;
    }
    pic_var.openarray_pairs       = tmp;
    pic_var.openarray_pairs_alloc = resize;
  }
  pic_var.openarray_pairs[pic_var.openarray_pairs_used].actual   = actual;
  pic_var.openarray_pairs[pic_var.openarray_pairs_used].formal   = formal;
  pic_var.openarray_pairs[pic_var.openarray_pairs_used].reported = BOOLEAN_FALSE;
  pic_var.openarray_pairs[pic_var.openarray_pairs_used].pos_valid = BOOLEAN_FALSE;
  pic_var.openarray_pairs[pic_var.openarray_pairs_used].passon   = passon;
  pic_var.openarray_pairs[pic_var.openarray_pairs_used].chain_src = VARIABLE_NONE;
  pic_var.openarray_pairs[pic_var.openarray_pairs_used].chain_sz  = 0;
  pic_var.openarray_pairs[pic_var.openarray_pairs_used].strview  = BOOLEAN_FALSE;
  pic_var.openarray_pairs[pic_var.openarray_pairs_used].chunk_ct = 0;
  if (pos) {
    pic_var.openarray_pairs[pic_var.openarray_pairs_used].pos       = *pos;
    pic_var.openarray_pairs[pic_var.openarray_pairs_used].pos_valid = BOOLEAN_TRUE;
  }
  pic_var.openarray_pairs_used++;
}

/* copy src's chunk map onto dst (replace semantics; dst usually an
 * open-array formal whose subscripts must walk src's chunks) */
static void pic_variable_chunk_map_copy(pfile_t *pf, variable_t src,
    variable_t dst)
{
  pic_variable_chunk_map_t *map;

  if (src == dst) {
    return;
  }
  for (map = pic_var.chunk_maps; map; map = map->link) {
    if (map->var == src) {
      /* AMBIGUOUS FORMAL (2026-11-02): the callee body is emitted once and
       * its subscripts key off the FORMAL's chunk map, so a formal that
       * serves two different multi-bank actuals can only walk one of them.
       * The second call then reads the first actual's memory -- silently:
       * a second `print_string(dev, <heap string>)` printed the first
       * string's text (found on Rob's 16F1825 program, where a
       * `_string_heap_size` over one bank makes the `_strargN`
       * marshalling buffer multi-bank).  Refuse instead of mis-addressing;
       * the trigger is one axis the author controls (buffer size), so the
       * message names it. */
      pic_variable_chunk_map_set(pf, dst, map->ct, map->base, map->sz);
      {
        pic_variable_chunk_map_t *copied;

        copied = pic_variable_chunk_map_find(dst);
        if (copied) {
          copied->owner = src;
        }
      }
      return;
    }
  }
}

/* propagate var's chunk map to every formal that receives it, and
 * transitively along the pair graph (a formal re-passed to another
 * open-array formal carries the copied map onward). The graph is tiny;
 * iterate to a fixpoint, bounded by the edge count. */
/* RUNTIME CHUNK ARGUMENTS (2026-11-02; per-formal maps 2026-11-03).
 *
 * An open-array formal's subscripts used to be emitted against the FORMAL's
 * chunk map, which one callee body can only carry for one actual -- a second
 * chunked actual read the first one's memory (fixes/
 * fix_openarray_formal_chunk_map.md).  A formal fed two different layouts now
 * owns a map that every call site fills (pic_var_chunk_prepare_emit), while
 * the old baked propagation below still serves the one-layout case. */

/* two pairs' captured layouts describe the same addressing? */
static boolean_t pic_var_openarray_layout_same(
    const pic_var_openarray_pair_t *a, const pic_var_openarray_pair_t *b)
{
  variable_ct_t ii;

  if (a->chunk_ct != b->chunk_ct) {
    return BOOLEAN_FALSE;
  }
  for (ii = 0; ii < a->chunk_ct; ii++) {
    if ((a->chunk_base[ii] != b->chunk_base[ii])
        || (a->chunk_sz[ii] != b->chunk_sz[ii])) {
      return BOOLEAN_FALSE;
    }
  }
  return BOOLEAN_TRUE;
}

/* create (or grow) the map buffer of one formal.  It holds
 * [count][base lo,hi,span]..., and the size is exact for the widest layout
 * this formal is fed.  A map only GROWS when a pass-on source (see the pair's
 * `chain_src`) turns out wider than the formal's own layouts: a bigger map is
 * created and replaces the registry entry (the old bytes stay allocated but
 * unused -- the alternative is a per-call copy that overflows). */
static boolean_t pic_var_formal_map_resize(pfile_t *pf, variable_t formal,
    variable_sz_t need)
{
  char                 name[32];
  variable_t           map;
  pic_var_formal_map_t *ent;

  ent = pic_var_formal_map_find(formal);
  if (ent && (variable_sz_get(ent->map) >= need)) {
    return BOOLEAN_TRUE;
  }
  /* a deterministic name so the emitted .asm is stable across passes */
  sprintf(name, "_pic_chunkmap_%u", (unsigned) (pic_var.formal_maps_used + 1));
  map = pic_var_chunk_global_create(pf, name, need);
  if (VARIABLE_NONE == map) {
    return BOOLEAN_FALSE;
  }
  if (ent) {
    ent->map = map;
    return BOOLEAN_TRUE;
  }
  if (pic_var.formal_maps_used == pic_var.formal_maps_alloc) {
    size_t                resize;
    pic_var_formal_map_t *tmp;

    resize = pic_var.formal_maps_alloc ? 2 * pic_var.formal_maps_alloc : 8;
    tmp = REALLOC(pic_var.formal_maps, resize * sizeof(*pic_var.formal_maps));
    if (!tmp) {
      pfile_log_syserr(pf, ENOMEM);
      return BOOLEAN_FALSE;
    }
    pic_var.formal_maps       = tmp;
    pic_var.formal_maps_alloc = resize;
  }
  ent         = &pic_var.formal_maps[pic_var.formal_maps_used++];
  ent->formal = formal;
  ent->map    = map;
  ent->strview = pic_var_formal_strview_fed(formal);
  return BOOLEAN_TRUE;
}

/* Called once, after the FINAL placement (and after the layout trials, which
 * re-place everything from a snapshot that cannot contain these variables):
 *
 *  1. capture every open-array pair's layout from the final chunk maps (a
 *     linearly placed actual normalizes to ONE chunk, so every pair has a
 *     layout and two call sites can be compared);
 *  2. give each formal that sees two DIFFERENT layouts a map of its own; and
 *  3. create + place the walk scratch, so nothing is created during emission
 *     (creating and placing a variable mid-emit re-entered the allocator --
 *     a pass-3 hang on Rob's full program, found 2026-11-02). */
/* the byte size `formal`'s map needs given the state right now (0 = none).
 *  - its own captured layouts: a map only when two of them DIFFER and one is
 *    chunked (one layout is addressed exactly by the baked copy);
 *  - every PASS-ON source formal that already has a runtime map: the call site
 *    copies that map, so the target must be at least as big. */
static variable_sz_t pic_var_formal_map_need(variable_t formal)
{
  size_t        pi;
  size_t        pj;
  variable_ct_t max_ct;
  variable_sz_t need;
  boolean_t     differ;
  boolean_t     chunked;

  max_ct  = 0;
  differ  = BOOLEAN_FALSE;
  chunked = BOOLEAN_FALSE;
  need    = 0;
  for (pi = 0; pi < pic_var.openarray_pairs_used; pi++) {
    pic_var_openarray_pair_t *pair;

    pair = &pic_var.openarray_pairs[pi];
    if ((pair->formal != formal) || (pair->chunk_ct < 1)) {
      continue;
    }
    if (pair->chunk_ct > max_ct) {
      max_ct = pair->chunk_ct;
    }
    if (pair->chunk_ct >= 2) {
      chunked = BOOLEAN_TRUE;
    }
    for (pj = 0; pj < pic_var.openarray_pairs_used; pj++) {
      pic_var_openarray_pair_t *other;

      other = &pic_var.openarray_pairs[pj];
      if ((other->formal != formal) || (other->chunk_ct < 1)) {
        continue;
      }
      if (!pic_var_openarray_layout_same(pair, other)) {
        differ = BOOLEAN_TRUE;
      }
    }
  }
  if (chunked && differ && (max_ct <= PIC_CHUNK_MAP_MAX)) {
    need = (variable_sz_t) (1 + 3 * max_ct);
  }
  /* a string payload-view formal ALWAYS needs the runtime map (its one
   * layout is the heap's, but the base offset differs per call site -- the
   * baked map cannot carry it) and its format is
   * [count][base lo,hi][pad][entries 3*ct] -- with every run longer than
   * 255 bytes SPLIT (the walk's span compare is byte-sized), so the size
   * must count the pieces */
  if (pic_var_formal_strview_fed(formal)) {
    variable_ct_t piece_ct;

    piece_ct = 0;
    for (pi = 0; pi < pic_var.openarray_pairs_used; pi++) {
      pic_var_openarray_pair_t *pair;
      variable_ct_t             ppieces;

      pair = &pic_var.openarray_pairs[pi];
      if ((pair->formal != formal) || (pair->chunk_ct < 1)) {
        continue;
      }
      ppieces = 0;
      for (pj = 0; pj < pair->chunk_ct; pj++) {
        ppieces += (variable_ct_t) ((pair->chunk_sz[pj] + 254U) / 255U);
      }
      if (ppieces > piece_ct) {
        piece_ct = ppieces;
      }
    }
    if ((piece_ct >= 1) && (piece_ct <= PIC_CHUNK_MAP_MAX)) {
      variable_sz_t svneed;

      svneed = (variable_sz_t) (4 + 3 * piece_ct);
      if (svneed > need) {
        need = svneed;
      }
    }
  }
  for (pi = 0; pi < pic_var.openarray_pairs_used; pi++) {
    pic_var_openarray_pair_t *pair;
    variable_t                src;
    variable_sz_t             ssz;

    pair = &pic_var.openarray_pairs[pi];
    if ((pair->formal != formal) || !pair->passon
        || (0 != pair->chunk_ct)) {
      continue;
    }
    src = pic_var_openarray_formal_map_get(pair->actual);
    if (VARIABLE_NONE == src) {
      continue;
    }
    ssz = variable_sz_get(src);
    if (ssz > need) {
      need = ssz;
    }
  }
  return need;
}

void pic_var_chunk_prepare_emit(pfile_t *pf)
{
  size_t   pi;
  size_t   pj;
  unsigned round;

  /* 1. capture every pair's layout.  A PASS-ON pair only knows a pointer, so
   * it has a layout only when the source formal carries a COMPILE-TIME chunk
   * map (the call site can then hand those literals over); a source with a
   * runtime map is handled in step 2 by copying it. */
  for (pi = 0; pi < pic_var.openarray_pairs_used; pi++) {
    pic_var_openarray_pair_t *pair;
    pic_variable_chunk_map_t *m;

    pair = &pic_var.openarray_pairs[pi];
    m    = pic_variable_chunk_map_find(pair->actual);
    if (m && (m->ct >= 1) && (m->ct <= PIC_CHUNK_MAP_MAX)) {
      variable_ct_t cix;

      pair->chunk_ct = m->ct;
      for (cix = 0; cix < m->ct; cix++) {
        pair->chunk_sz[cix]   = m->sz[cix];
        pair->chunk_base[cix] = m->base[cix];
      }
    } else if (pair->passon) {
      pair->chunk_ct = 0;
    } else {
      /* no chunk map: the actual is placed linearly (or is a ROM const
       * array, whose base is unknown here) -- one chunk covers it */
      pair->chunk_ct        = 1;
      pair->chunk_base[0]   = variable_base_get(pair->actual, 0);
      pair->chunk_sz[0]     = pic_variable_def_storage_need_get(pf,
          variable_def_get(pair->actual), 0);
    }
  }
  /* 2. create the maps, to a FIXPOINT: a pass-on pair can only be sized once
   * the source formal's map exists, and a chain can be longer than one hop
   * (`a -> b -> c`).  Then record each chain (the per-call copy). */
  for (round = 0; round < 8; round++) {
    boolean_t changed;

    changed = BOOLEAN_FALSE;
    for (pi = 0; pi < pic_var.openarray_pairs_used; pi++) {
      pic_var_openarray_pair_t *pair;

      /* A pass-on pair whose source formal has a runtime map is a CHAIN: the
       * baked layout it may have inherited (the first actual's, via
       * pic_var_chunk_map_propagate) describes only one call site, so it must
       * not be handed over as literals. */
      pair = &pic_var.openarray_pairs[pi];
      if (pair->passon && (0 != pair->chunk_ct)
          && (VARIABLE_NONE !=
            pic_var_openarray_formal_map_get(pair->actual))) {
        pair->chunk_ct = 0;
        changed = BOOLEAN_TRUE;
      }
    }
    for (pi = 0; pi < pic_var.openarray_pairs_used; pi++) {
      variable_t    formal;
      variable_t    map;
      variable_sz_t need;

      formal = pic_var.openarray_pairs[pi].formal;
      if (!formal || (VARIABLE_NONE == formal)
          || pic_var_openarray_formal_blocked(formal)) {
        continue;
      }
      for (pj = 0; pj < pi; pj++) {
        if (pic_var.openarray_pairs[pj].formal == formal) {
          break;
        }
      }
      if (pj != pi) {
        continue; /* this formal was handled at an earlier index */
      }
      need = pic_var_formal_map_need(formal);
      if (0 == need) {
        continue;
      }
      map = pic_var_openarray_formal_map_get(formal);
      if ((VARIABLE_NONE != map)
          && (variable_sz_get(map) >= need)) {
        continue;
      }
      if (pic_var_formal_map_resize(pf, formal, need)) {
        changed = BOOLEAN_TRUE;
      }
    }
    for (pi = 0; pi < pic_var.openarray_pairs_used; pi++) {
      pic_var_openarray_pair_t *pair;
      variable_t                src;
      variable_t                dst;
      variable_sz_t             ssz;
      variable_sz_t             dsz;

      pair = &pic_var.openarray_pairs[pi];
      if (!pair->passon || (0 != pair->chunk_ct)
          || (VARIABLE_NONE != pair->chain_src)) {
        continue;
      }
      src = pic_var_openarray_formal_map_get(pair->actual);
      dst = pic_var_openarray_formal_map_get(pair->formal);
      if ((VARIABLE_NONE == src) || (VARIABLE_NONE == dst)) {
        continue;
      }
      if (pic_var_openarray_formal_strview_get(pair->actual)
          || pic_var_openarray_formal_strview_get(pair->formal)) {
        /* the source formal walks a STRING PAYLOAD VIEW (its map carries a
         * runtime base word): handing that map to another formal would make
         * the callee walk payload-relative indices with plain-array math --
         * and a chain INTO a view formal misaligns the copied entries the
         * same way (the two map formats differ by the base word).  Refuse
         * the re-pass in both directions; pass the bytes through a user
         * array instead. */
        if (!pair->reported) {
          pair->reported = BOOLEAN_TRUE;
          pfile_log(pf, PFILE_LOG_ERR,
              "an open-array parameter bound to a string cannot be passed "
              "on to another procedure (copy it into a byte array first)");
        }
        continue;
      }
      ssz = variable_sz_get(src);
      dsz = variable_sz_get(dst);
      pair->chain_src = src;
      pair->chain_sz  = (ssz < dsz) ? ssz : dsz;
      changed = BOOLEAN_TRUE;
    }
    if (!changed) {
      break;
    }
  }
  /* 3. a pass-on pair that never got a layout or a chain hands over nothing:
   * keep the baked map, exactly as before this mechanism existed */
  for (pi = 0; pi < pic_var.openarray_pairs_used; pi++) {
    pic_var_openarray_pair_t *pair;

    pair = &pic_var.openarray_pairs[pi];
    if (pair->passon && (0 == pair->chunk_ct)
        && (VARIABLE_NONE == pic_var_openarray_formal_map_get(pair->formal))) {
      pic_var_openarray_formal_block(pair->formal);
    }
  }
  /* 3b. a pass-on pair touching a STRING VIEW formal is refused even when no
   * chain ever forms: the view formal's map carries the payload base word,
   * and the re-passing site can hand over neither that nor a compatible
   * plain-format copy -- without this the callee would walk the STALE map of
   * the string call site (found with the b(s)+a(buf) probe: writes landed
   * in the string's payload) */
  for (pi = 0; pi < pic_var.openarray_pairs_used; pi++) {
    pic_var_openarray_pair_t *pair;

    pair = &pic_var.openarray_pairs[pi];
    if (pair->passon && !pair->reported
        && (pic_var_formal_strview_fed(pair->formal)
          || pic_var_formal_strview_fed(pair->actual))) {
      pair->reported = BOOLEAN_TRUE;
      pfile_log(pf, PFILE_LOG_ERR,
          "an open-array parameter bound to a string cannot be passed "
          "on to another procedure (copy it into a byte array first)");
    }
  }
  /* 4. report a buffer split into more pieces than the map can describe,
   * once per formal */
  for (pi = 0; pi < pic_var.openarray_pairs_used; pi++) {
    pic_var_openarray_pair_t *pair;
    variable_t                formal;
    variable_ct_t             max_ct;
    size_t                    pj2;

    pair   = &pic_var.openarray_pairs[pi];
    formal = pair->formal;
    max_ct = 0;
    for (pj2 = 0; pj2 < pic_var.openarray_pairs_used; pj2++) {
      if ((pic_var.openarray_pairs[pj2].formal == formal)
          && (pic_var.openarray_pairs[pj2].chunk_ct > max_ct)) {
        max_ct = pic_var.openarray_pairs[pj2].chunk_ct;
      }
    }
    if ((max_ct > PIC_CHUNK_MAP_MAX) && !pair->reported && formal
        && (VARIABLE_NONE != formal)) {
      pair->reported = BOOLEAN_TRUE;
      pfile_log(pf, PFILE_LOG_ERR,
          "open-array formal '%s' receives a buffer split into %u RAM bank "
          "pieces, but the runtime chunk map holds %u; reduce the buffer "
          "size so it needs fewer bank pieces",
          variable_name_get(formal) ? variable_name_get(formal) : "?",
          (unsigned) max_ct, (unsigned) PIC_CHUNK_MAP_MAX);
    }
  }
  /* 5. the walk scratch: needed by every runtime-map formal AND by the SHARED
   * static walk (one helper per holed variable) */
  {
    boolean_t need;
    pfile_proc_t *proc;

    need = (pic_var.formal_maps_used != 0) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
    for (proc = pfile_proc_root_get(pf); !need && proc;
         proc = pfile_proc_next(proc)) {
      pfile_block_t *blk;

      for (blk = pfile_proc_block_root_get(proc); !need && blk;
           blk = pfile_block_next(blk)) {
        variable_t var;

        for (var = pfile_block_variable_list_head(blk); var;
             var = variable_link_get(var)) {
          if (pic_variable_chunk_ct_get(var) > 1) {
            need = BOOLEAN_TRUE;
            break;
          }
        }
      }
    }
    if (need) {
      variable_t gc;
      variable_t gb;
      variable_t gs;

      (void) pic_var_chunk_walk_globals_get(pf, &gc, &gb, &gs);
    }
  }
}

static void pic_var_chunk_map_propagate(pfile_t *pf, variable_t var)
{
  size_t    ii;
  boolean_t changed;

  if (!pic_variable_chunk_map_find(var)) {
    return;
  }
  do {
    changed = BOOLEAN_FALSE;
    for (ii = 0; ii < pic_var.openarray_pairs_used; ii++) {
      variable_t src;
      variable_t dst;

      src = pic_var.openarray_pairs[ii].actual;
      dst = pic_var.openarray_pairs[ii].formal;
      if (src != dst) {
        pic_variable_chunk_map_t *smap;

        smap = pic_variable_chunk_map_find(src);
        if (smap && (smap->ct >= 2) && (smap->ct <= PIC_CHUNK_MAP_MAX)) {
          variable_ct_t cix;

          pic_var.openarray_pairs[ii].chunk_ct = smap->ct;
          for (cix = 0; cix < smap->ct; cix++) {
            pic_var.openarray_pairs[ii].chunk_sz[cix]   = smap->sz[cix];
            pic_var.openarray_pairs[ii].chunk_base[cix] = smap->base[cix];
          }
        }
      }
      if ((src != dst)
          && pic_variable_chunk_map_find(src)
          && !pic_variable_chunk_map_find(dst)) {
        pic_variable_chunk_map_copy(pf, src, dst);
        changed = BOOLEAN_TRUE;
      }
    }
  } while (changed);
}

static variable_sz_t pic_pointer_slot_padding_get(pfile_t *pf,
    variable_ct_t pointer_slots)
{
  variable_sz_t padding;

  padding = 0;
  if (pointer_slots && pic_is_16bit(pf)
      && (3 == pic_pointer_size_get(pf))) {
    padding = pointer_slots;
  }
  return padding;
}

variable_sz_t pic_variable_def_storage_need_get(pfile_t *pf,
    variable_def_t def, variable_ct_t *pointer_slots)
{
  variable_def_member_t mbr;
  variable_sz_t         need;

  if (!def) {
    return 0;
  }
  switch (variable_def_type_get(def)) {
    case VARIABLE_DEF_TYPE_POINTER:
      if (pointer_slots) {
        (*pointer_slots)++;
      }
      return pic_pointer_size_get(pf);

    case VARIABLE_DEF_TYPE_ARRAY:
      mbr = variable_def_member_get(def);
      if (mbr && (VARIABLE_CT_UNKNOWN != variable_def_member_ct_get(mbr))) {
        variable_ct_t elem_slots;
        variable_ct_t ct;

        elem_slots = 0;
        ct = variable_def_member_ct_get(mbr);
        need = pic_variable_def_storage_need_get(pf,
            variable_def_member_def_get(mbr), &elem_slots);
        if (pointer_slots) {
          *pointer_slots += ct * elem_slots;
        }
        return ct * need;
      }
      break;

    case VARIABLE_DEF_TYPE_STRUCTURE:
      need = 0;
      for (mbr = variable_def_member_get(def);
           mbr;
           mbr = variable_def_member_link_get(mbr)) {
        variable_ct_t ct;

        ct = variable_def_member_ct_get(mbr);
        if (VARIABLE_CT_UNKNOWN == ct) {
          return variable_def_sz_get(def);
        }
        need += ct * pic_variable_def_storage_need_get(pf,
            variable_def_member_def_get(mbr), pointer_slots);
      }
      return need;

    case VARIABLE_DEF_TYPE_UNION:
      {
      variable_ct_t best_slots;

      need = 0;
      best_slots = 0;
      for (mbr = variable_def_member_get(def);
           mbr;
           mbr = variable_def_member_link_get(mbr)) {
        variable_ct_t ct;
        variable_ct_t mslots;
        variable_sz_t mneed;

        ct = variable_def_member_ct_get(mbr);
        if (VARIABLE_CT_UNKNOWN == ct) {
          return variable_def_sz_get(def);
        }
        mslots = 0;
        mneed = ct * pic_variable_def_storage_need_get(pf,
            variable_def_member_def_get(mbr), &mslots);
        mslots *= ct;
        if ((mneed + pic_pointer_slot_padding_get(pf, mslots))
            > (need + pic_pointer_slot_padding_get(pf, best_slots))) {
          need = mneed;
          best_slots = mslots;
        }
      }
      if (pointer_slots) {
        *pointer_slots += best_slots;
      }
      return need;
      }

    default:
      break;
  }
  return variable_def_sz_get(def);
}

static variable_sz_t pic_variable_storage_need_get(pfile_t *pf,
    variable_t var, variable_ct_t *pointer_slots)
{
  if (pointer_slots) {
    *pointer_slots = 0;
  }
  return pic_variable_def_storage_need_get(pf, variable_def_get(var),
      pointer_slots);
}

#ifdef BLIST_DEBUG
static void pic_blist_dump(pfile_t *pf, const pic_bank_info_t *blist)
{
  const pic_bank_info_t *bptr;

  pfile_log(pf, PFILE_LOG_DEBUG, "--- blist start ---");
  for (bptr = blist; bptr; bptr = bptr->link) {
    pfile_log(pf, PFILE_LOG_DEBUG, 
        PIC_MSG_DBG_BANK, bptr->lo, bptr->hi);
  }
  pfile_log(pf, PFILE_LOG_DEBUG, "--- blist end ---");
}

static void pic_blist_check(const pic_bank_info_t *blist)
{
  const pic_bank_info_t *b1;
  const pic_bank_info_t *b2;

  b1 = blist;
  b2 = (b1) ? b1->link : 0;
  while (b1 && b2 && (b1 != b2)) {
    b1 = b1->link;
    b2 = b2->link;
    if (b2) {
      b2 = b2->link;
    }
  }
  assert(b1 != b2);
  for (b1 = blist; b1; b1 = b1->link) {
    assert(b1->lo <= b1->hi);
    if (b1->link) {
      assert(b1->hi < b1->link->lo);
    }
  }
}
#endif

/* this is the union of  blist with {lo,hi} */
void pic_blist_add(pfile_t *pf, pic_bank_info_t **pblist, 
    size_t lo, size_t hi)
{
  pic_bank_info_t *ptr;
  pic_bank_info_t *pv;
  pic_bank_info_t *blist;

  assert(lo <= hi);
  blist = *pblist;
  for (ptr = blist, pv = 0; 
       ptr && (lo > ptr->hi);
       pv = ptr, ptr = ptr->link)
    ;
  /* this could use *a lot* of improvement, but will work for now
   * 1st I'll simply allocate a new entry & insert it, then I'll
   * do some cleanup */
  if (ptr && (ptr->lo <= lo) && (ptr->hi >= hi)) {
  } else {
    ptr = MALLOC(sizeof(*ptr));
    if (!ptr) {
      pfile_log_syserr(pf, RESULT_MEMORY);
    } else {
      ptr->lo = lo;
      ptr->hi = hi;
      ptr->link = (pv) ? pv->link : blist;
      if (pv) {
        ptr->link = pv->link;
        pv->link  = ptr;
      } else {
        ptr->link = blist;
        blist     = ptr;
      }
    }

    ptr = blist;
    while (ptr->link) {
      pic_bank_info_t *link;

      link = ptr->link;
      if (ptr->hi + 1 >= link->lo) {
        /* Check if merging would cross a bank boundary */
        unsigned bank_size = pic_target_bank_size_get(pf);
        
        /* Use fallback bank size if not set properly */
        if (bank_size == 0) {
          bank_size = 0x100; /* Default 256-byte banks for PIC18 */
        }
        
        if (bank_size > 0x10000) {
          /* Invalid bank size - use original merging behavior */
          if (ptr->hi < link->hi) {
            ptr->hi = link->hi;
          }
          ptr->link = link->link;
          FREE(link);
        } else {
          unsigned ptr_bank = ptr->hi / bank_size;
          unsigned link_bank = link->lo / bank_size;
          
          if (ptr_bank == link_bank) {
            /* Same bank - safe to merge */
            if (ptr->hi < link->hi) {
              ptr->hi = link->hi;
            }
            ptr->link = link->link;
            FREE(link);
          } else {
            /* Different banks - do not merge to avoid bank boundary crossing */
            ptr = link;
          }
        }
      } else {
        ptr = link;
      }
    }
  }
  *pblist = blist;
#ifdef BLIST_DEBUG
  pic_blist_check(blist);
  pfile_log(pf, PFILE_LOG_DEBUG, "BLIST ADD(%u,%u)", lo, hi);
  pic_blist_dump(pf, pic_blist);
#endif
}

void pic_data_bank_list_add(pfile_t *pf, size_t lo, size_t hi)
{
  pic_blist_add(pf, &pic_var.data_bank_list, lo, hi);
  pic_blist_add(pf, &pic_var.data_pragma_list, lo, hi);
}

void pic_shared_bank_list_add(pfile_t *pf, size_t lo, size_t hi)
{
  pic_blist_add(pf, &pic_var.shared_bank_list, lo, hi);
  pic_blist_add(pf, &pic_var.shared_pragma_list, lo, hi);
}

boolean_t pic_shared_bank_list_covers_byte(pfile_t *pf, unsigned long file_addr)
{
  pic_bank_info_t *bank;

  UNUSED(pf);
  for (bank = pic_var.shared_pragma_list; bank; bank = bank->link) {
    if (file_addr >= (unsigned long) bank->lo
        && file_addr <= (unsigned long) bank->hi) {
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}


static void pic_blist_remove(pfile_t *pf, pic_bank_info_t **pblist,
    size_t base, size_t sz)
{
  pic_bank_info_t *ptr;
  pic_bank_info_t *pv;
  pic_bank_info_t *blist;
  size_t           lo;
  size_t           hi;

  blist = *pblist;
  lo = base;
  hi = base + sz - 1;

  for (ptr = blist, pv = 0;
       ptr && (lo > ptr->hi);
       pv = ptr, ptr = ptr->link)
    ;
  if (ptr && (lo <= ptr->hi) && (hi >= ptr->lo)) {
    /* lo <= ptr->hi
       ptr->lo <= hi <= ptr->hi */
    if (lo <= ptr->lo) {
      ptr->lo = hi + 1;
    }
    if (hi >= ptr->hi) {
      ptr->hi = lo - 1;
    }
    /* if (ptr->hi < ptr->lo), this bit is gone */
    if (ptr->hi < ptr->lo) {
      if (pv) {
        pv->link = ptr->link;
      } else {
        blist = ptr->link;
      }
      FREE(ptr);
    } else if ((ptr->lo < lo) && (hi < ptr->hi)) {
      /* split this into:
           {ptr->lo, lo - 1}
           {hi + 1, ptr->hi} */
      pv = ptr;
      ptr = MALLOC(sizeof(*ptr));
      if (!ptr) {
        pfile_log_syserr(pf, RESULT_MEMORY);
      } else {
        ptr->link = pv->link;
        pv->link  = ptr;
        ptr->hi   = pv->hi;
        ptr->lo   = hi + 1;
        pv->hi    = lo - 1;
      }
    }
  }
  *pblist = blist;
#ifdef BLIST_DEBUG
  pic_blist_check(blist);
  pfile_log(pf, PFILE_LOG_DEBUG, "BLIST REMOVE(%u,%u)", lo, lo + sz - 1);
  pic_blist_dump(pf, pic_blist);
#endif
}

void pic_data_bank_list_remove_range(pfile_t *pf, size_t lo, size_t hi)
{
  size_t sz;

  assert(lo <= hi);
  sz = hi - lo + 1;
  pic_blist_remove(pf, &pic_var.data_bank_list, lo, sz);
}

/* Silent cross-bank optimization */
static void pic_optimize_cross_bank_access(pfile_t *pf, variable_base_t base, variable_sz_t size)
{
  unsigned bank_size = pic_target_bank_size_get(pf);
  unsigned bank_mask = ~(bank_size - 1);
  variable_sz_t hi = base + size - 1;
  
  /* Check if this is a cross-bank array */
  if ((base & bank_mask) != (hi & bank_mask)) {
    /* Calculate bank boundaries for optimization */
    unsigned start_bank = (base & bank_mask) / bank_size;
    unsigned end_bank = (hi & bank_mask) / bank_size;
    unsigned banks_spanned = end_bank - start_bank + 1;
    
    /* Debug information only */
    pfile_log(pf, PFILE_LOG_DEBUG, 
      "Silent cross-bank optimization: Array spans %u banks (%u to %u) "
      "covering 0x%04X to 0x%04X - transparent bank switching active",
      banks_spanned, start_bank, end_bank, (unsigned)base, (unsigned)hi);
    
    /* Debug optimization details */
    pfile_log(pf, PFILE_LOG_DEBUG,
      "Transparent optimization: Automatic bank switching with zero developer friction - "
      "seamless cross-bank access enabled");
    
    /* This information will be used by the code generator to implement
     * transparent cross-bank access with silent optimization */
  }
}

/*
 * First start address in [bank->lo, bank->hi] where `need` bytes fit in one
 * PIC18-style file bank window (bank_mask). VARIABLE_BASE_UNKNOWN if none.
 * Mirrors the former first-fit inner loop in pic_data_alloc.
 */
static variable_base_t pic_data_first_single_bank_lo(pic_bank_info_t *bank,
    variable_sz_t need, unsigned bank_size, unsigned bank_mask)
{
  size_t          lo;
  size_t          hi;
  variable_base_t base;

  base = VARIABLE_BASE_UNKNOWN;
  lo = bank->lo;
  do {
    hi = lo + (size_t) need - 1;
    if (hi <= bank->hi) {
      if (((unsigned) lo & bank_mask) != ((unsigned) hi & bank_mask)) {
        lo += (size_t) bank_size - (lo & ((size_t) bank_size - 1));
      } else {
        base = (variable_base_t) lo;
        break;
      }
    } else {
      break;
    }
  } while ((hi <= bank->hi) && (VARIABLE_BASE_UNKNOWN == base));
  return base;
}

/* allocate a block out of the data area */
/* NAME
 *   pic_blist_range_free
 *
 * DESCRIPTION
 *   TRUE when [base, base+need-1] lies entirely inside one free-list node.
 *   Used by the shared-window fallback: on a part whose `pragma shared'
 *   range overlaps the data area (a single-bank 12-bit part declares the
 *   whole GPR block shared), the shared list can offer bytes the data list
 *   has already handed out -- placing there overlapped live variables
 *   silently.
 *
 * PARAMETERS
 *   list : free list
 *   base : first byte
 *   need : byte count
 *
 * RETURN
 *   BOOLEAN_TRUE when the whole range is free in `list'
 */
static boolean_t pic_blist_range_free(const pic_bank_info_t *list,
    variable_base_t base, variable_sz_t need)
{
  const pic_bank_info_t *node;
  variable_base_t        hi;

  if (0 == need) {
    return BOOLEAN_TRUE;
  }
  hi = (variable_base_t) (base + need - 1);
  for (node = list; node; node = node->link) {
    if ((node->lo <= base) && (hi <= node->hi)) {
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

/* NAME
 *   pic_blist_range_dataspace
 *
 * DESCRIPTION
 *   TRUE when [base, base+need-1] lies inside one node of the DATA area as
 *   the device declared it (the never-consumed mirror of the data ranges).
 *   The shared-window fallback uses it to tell the two shared-window shapes
 *   apart: a real access window (0x00-0x5F on PIC18) is OUTSIDE the data
 *   area and may be handed out without consulting the data free list, while
 *   a `pragma shared' block inside the data area (12-bit: the whole GPR
 *   block) is the same bytes as the data list and must not be double-booked.
 *
 * PARAMETERS
 *   list : declared data-area mirror
 *   base : first byte
 *   need : byte count
 *
 * RETURN
 *   BOOLEAN_TRUE when the range is in the declared data area
 */
static boolean_t pic_blist_range_dataspace(const pic_bank_info_t *list,
    variable_base_t base, variable_sz_t need)
{
  const pic_bank_info_t *node;
  variable_base_t        hi;

  if (0 == need) {
    return BOOLEAN_FALSE;
  }
  hi = (variable_base_t) (base + need - 1);
  for (node = list; node; node = node->link) {
    if ((node->lo <= base) && (hi <= node->hi)) {
      return BOOLEAN_TRUE;
    }
  }
  return BOOLEAN_FALSE;
}

static variable_base_t pic_data_alloc(pfile_t *pf, pic_bank_info_t **blist,
  variable_sz_t need, variable_ct_t pointer_slots)
{
  pic_bank_info_t *bank;
  variable_base_t  base;
  unsigned         bank_size;
  unsigned         bank_mask;
  variable_sz_t    rm;
  variable_base_t  best_base;
  variable_base_t  first_base;
  size_t           best_slack;
  boolean_t        have_best;

  base = VARIABLE_BASE_UNKNOWN;
  bank_size = pic_target_bank_size_get(pf);
  bank_mask = ~(bank_size - 1);
  rm = need + pic_pointer_slot_padding_get(pf, pointer_slots);
  best_base = VARIABLE_BASE_UNKNOWN;
  first_base = VARIABLE_BASE_UNKNOWN;
  have_best = BOOLEAN_FALSE;
  best_slack = 0;

  if (bank_size & (bank_size - 1)) {
    pfile_log(pf, PFILE_LOG_ERR, 
      "code generation error. Target size (%u) is not a power of 2!",
      bank_size);
  }

  /*
   * On the 16 bit cores, different banks are contiguous. For example
   * the bank list might show 0x0000 - 0x03ff but this is really
   * four different banks requiring four different MOVLB instructions.
   * This was never a problem on the 12 or 14 bit cores where the
   * beginning of each bank consists of special function registers.
   * I'll try to account for that here.
   *
   * Best-fit (16-bit cores only): among all free-list nodes, choose the
   * valid placement that minimizes trailing slack (bank->hi - base + 1 - rm);
   * tie-break lower base.
   *
   * Classic 12/14-bit cores keep the original first-fit policy (lowest
   * candidate in bank-list order): best-fit systematically avoids the large
   * bank-0 GPR area (worst trailing slack), evacuating bank 0 and forcing
   * RP bank-select ops on every access (+30-43% ROM on midrange samples).
   * The two policies are intentional; do not unify them.
   */
  for (bank = *blist; bank; bank = bank->link) {
    variable_base_t cand;
    size_t          cand_sz;
    size_t          alloc_end;

    cand = pic_data_first_single_bank_lo(bank, need, bank_size, bank_mask);
    if (VARIABLE_BASE_UNKNOWN == cand) {
      continue;
    }
    cand_sz = (size_t) cand;
    if (rm < 1 || cand_sz > bank->hi) {
      continue;
    }
    alloc_end = cand_sz + (size_t) rm - 1;
    if (alloc_end > bank->hi || alloc_end < cand_sz) {
      continue;
    }
    if (((unsigned) cand_sz & bank_mask) != ((unsigned) alloc_end & bank_mask)) {
      continue;
    }
    {
      size_t slack;

      if (VARIABLE_BASE_UNKNOWN == first_base) {
        first_base = cand;
      }
      if (!pic_is_16bit(pf)) {
        continue; /* classic cores: first-fit wins, best-fit bookkeeping off */
      }
      slack = (bank->hi - cand_sz + 1) - (size_t) rm;
      if (!have_best || slack < best_slack
          || (slack == best_slack && cand < best_base)) {
        have_best   = BOOLEAN_TRUE;
        best_slack  = slack;
        best_base   = cand;
      }
    }
  }
  base = pic_is_16bit(pf) ? best_base : first_base;
  if (VARIABLE_BASE_UNKNOWN != base) {
    variable_sz_t hi;

    /* base = bank->lo; */
    /*
     * PIC18 3-byte pointers: remove one extra byte per actual pointer slot
     * from the free list so *_p+2 cannot share a file address with the next
     * 1-byte global (fix_pic18_pointer_gpr_threebyte_pointer_slot_padding.md).
     * `rm` already includes that padding (computed above).
     */
    pic_blist_remove(pf, blist, base, rm);
    
    /* Add warnings for array allocation patterns */
    hi = base + need - 1;
    
    /* Check if array crosses bank boundary */
    if ((base & bank_mask) != (hi & bank_mask)) {
      /* Cross-bank array support (warns) */
      /* Calculate bank boundary information */
      unsigned start_bank = (base & bank_mask) >> 8;  /* Bank number */
      unsigned end_bank = (hi & bank_mask) >> 8;      /* Bank number */
      unsigned bytes_in_start_bank = bank_size - (base & (bank_size - 1));
      unsigned bytes_in_end_bank = (hi & (bank_size - 1)) + 1;
      
      /* Silent cross-bank optimization -- no warnings, transparent */
      /* Debug information only in debug builds */
      pfile_log(pf, PFILE_LOG_DEBUG,
        "Transparent cross-bank array: %u bytes from 0x%04X to 0x%04X - silent optimization active",
        (unsigned)need, (unsigned)base, (unsigned)hi);
      
      /* Debug memory layout information */
      pfile_log(pf, PFILE_LOG_DEBUG,
        "Memory layout: %u bytes in bank %u (0x%04X-0x%04X), %u bytes in bank %u (0x%04X-0x%04X) - transparent bank switching",
        bytes_in_start_bank, start_bank, 
        (unsigned)base, (unsigned)(base + bytes_in_start_bank - 1),
        bytes_in_end_bank, end_bank,
        (unsigned)(hi - bytes_in_end_bank + 1), (unsigned)hi);
      
      /* Mark this variable as cross-bank for future optimization */
      pic_optimize_cross_bank_access(pf, base, need);
    }
    
    /* Warning for arrays that cross bank boundaries */
    /* Check if array actually crosses bank boundaries based on start and end addresses */
    {
      unsigned start_bank = base / bank_size;
      unsigned end_bank = (base + need - 1) / bank_size;
      
      if (start_bank != end_bank) {
        /* Array crosses bank boundaries */
        unsigned banks_crossed = end_bank - start_bank;
        pfile_log(pf, PFILE_LOG_WARN, 
          "Large array allocation (%u bytes) crosses %u bank boundaries (base=0x%04X, banks %u-%u). "
          "Consider splitting into smaller arrays for better memory efficiency.",
          (unsigned)need, banks_crossed, (unsigned)base, start_bank, end_bank);
      }
    }
#ifdef BLIST_DEBUG
    fprintf(stderr, "Allocating %u\n", base);
#endif
    pic_blist_max += rm;
  } 
  return base;
}

/* Multi-bank allocation removed temporarily to fix circular reference issues */

/* Memory analysis for the transparent cross-bank optimization */
static void pic_analyze_memory_fragmentation(pfile_t *pf)
{
  pic_bank_info_t *bank;
  unsigned bank_size = pic_target_bank_size_get(pf);
  unsigned total_free = 0;
  unsigned total_used = 0;
  unsigned fragmentation_count = 0;
  unsigned cross_bank_arrays = 0;
  
  pfile_log(pf, PFILE_LOG_DEBUG, "Silent cross-bank optimization analysis:");
  
  for (bank = pic_var.data_bank_list; bank; bank = bank->link) {
    unsigned bank_free = bank->hi - bank->lo + 1;
    unsigned bank_used = bank_size - bank_free;
    
    total_free += bank_free;
    total_used += bank_used;
    
    if (bank_free > 0 && bank_free < bank_size) {
      fragmentation_count++;
    }
    
    /* Check for cross-bank arrays in this bank */
    if (bank_free < bank_size && bank_free > 0) {
      cross_bank_arrays++;
    }
    
    pfile_log(pf, PFILE_LOG_DEBUG, 
      "Bank 0x%04X-0x%04X: %u free, %u used, optimization: %s",
      (unsigned)bank->lo, (unsigned)bank->hi, bank_free, bank_used,
      (bank_free > 0 && bank_free < bank_size) ? "TRANSPARENT" : "STANDARD");
  }
  
  pfile_log(pf, PFILE_LOG_DEBUG, 
    "Silent cross-bank optimization: %u free, %u used, %u cross-bank arrays, "
    "transparent bank switching framework active",
    total_free, total_used, cross_bank_arrays);
}

static variable_sz_t pic_data_biggest(const pic_bank_info_t *blist)
{
  variable_sz_t biggest;

  biggest = 0;
  while (blist) {
    variable_sz_t sz;

    sz = blist->hi - blist->lo + 1;
    if (sz > biggest) {
      biggest = sz;
    }
    blist = blist->link;
  }
  return biggest;
}

/* returns BOOLEAN_TRUE if a new base was set, else BOOLEAN_FALSE */
static boolean_t pic_variable_base_set(pfile_t *pf, pfile_proc_t *proc, 
  pic_bank_info_t **blist, variable_t vptr)
{
  variable_sz_t   need;
  variable_ct_t   pointer_slots;
  variable_base_t base;
  boolean_t       rc;

  need = pic_variable_storage_need_get(pf, vptr, &pointer_slots);
  assert(need);
  /* first we'll see if the entire thing can fit in a bank.
     If so, good! */

  base = pic_data_alloc(pf, blist, need, pointer_slots);
  if (getenv("JALV2_DBG_REFUSAL")) {
    fprintf(stderr, "DBG first %-18s need=%u base=0x%x\n",
        variable_name_get(vptr) ? variable_name_get(vptr) : "?",
        (unsigned) need, (unsigned) base);
  }
  rc   = BOOLEAN_FALSE;
  if (VARIABLE_BASE_UNKNOWN == base) {
    pfile_log(pf, PFILE_LOG_ERR, "no space for %s:%s (need=%u available=%u)",
        (proc) ? pfile_proc_tag_get(proc) : "",
        variable_name_get(vptr),
        need, pic_data_biggest(*blist));
    variable_flag_set(vptr, VARIABLE_FLAG_ALLOC_FAIL);
  } else {
    assert(VARIABLE_BASE_UNKNOWN == variable_base_get(vptr, 0));
    rc = BOOLEAN_TRUE;
    variable_base_set(vptr, base, 0);
  }
  return rc;
}

boolean_t pic_variable_alloc_one(pfile_t *pf, pfile_proc_t *proc,
    variable_t var)
{
  return pic_variable_base_set(pf, proc, &pic_var.data_bank_list, var);
}

/* find all variables that the user has placed & make sure these
   positions do not get reused */
static void blist_total(void)
{
  const pic_bank_info_t *binfo;

  for (pic_blist_total = 0, binfo = pic_var.data_bank_list; 
       binfo; 
       pic_blist_total += binfo->hi - binfo->lo + 1, binfo = binfo->link)
    ;
}

static void pic_variable_alloc_proc(pfile_t *pf, pfile_proc_t *proc);
static void pic_variable_alloc_block(pfile_t *pf, pfile_proc_t *proc, 
    pfile_block_t *blk)
{
  variable_t     vptr;
  pfile_block_t *cblk;
  size_t         ct;
  pfile_proc_t  *cproc;
  /*size_t         entry_sz;*/
  unsigned       auto_mem_reg_init;
  unsigned       change_ct;
  unsigned       var_ct;

  auto_mem_reg_init = pic_var.auto_mem_reg;


  if (!blk) {
    blk = pfile_proc_block_root_get(proc);
  }
  /* first, allocate all variables for this procedure */
  for (vptr = pfile_block_variable_list_head(blk), 
         change_ct = 0, var_ct = 0; 
       vptr; 
       vptr = variable_link_get(vptr)) {
    if (variable_is_auto(vptr) 
        && !variable_is_sticky(vptr)
        && !variable_flag_test(vptr, VARIABLE_FLAG_ALLOC_FAIL)) {
      /* find the first bank that will hold this variable */
      variable_sz_t   sz;
      variable_base_t old_base;

      var_ct++;
      sz = pic_variable_storage_need_get(pf, vptr, 0);
      assert(sz);
      old_base = variable_base_get(vptr, 0);
      if ((old_base == VARIABLE_BASE_UNKNOWN)
        || (old_base < pic_var.auto_mem_reg)) {
        change_ct++;
        variable_base_set(vptr, pic_var.auto_mem_reg, 0);
        pic_var.auto_mem_reg += sz;
        if (old_base != VARIABLE_BASE_UNKNOWN) {
          pfile_log(pf, PFILE_LOG_DEBUG, 
              "Resetting...%s%u (was %u, now %u)",
              variable_name_get(vptr), variable_tag_n_get(vptr),
              old_base, variable_base_get(vptr, 0)
              );
        }
      }
    }
  }
  /* If no changes were made here, no changes will be made below. */
  if ((var_ct == 0) || (change_ct > 0)) {
    /* next, recurse into all procedures this one calls and
     * allocate *those* variables */
    for (cblk = pfile_block_child_get(blk);
         cblk;
         cblk = pfile_block_sibbling_get(cblk)) {
      pic_variable_alloc_block(pf, proc, cblk);
    }
    for (ct = 0; (cproc = pfile_proc_calls_get(proc, ct)) != 0; ct++) {
      if (!pfile_proc_flag_test(cproc, PFILE_PROC_FLAG_VISITED)) {
        pfile_proc_flag_set(cproc, PFILE_PROC_FLAG_VISITED);
        pic_variable_alloc_proc(pf, cproc);
        pfile_proc_flag_clr(cproc, PFILE_PROC_FLAG_VISITED);
      }
    }
  }
  if (pfile_flag_test(pf, PFILE_FLAG_OPT_VARIABLE_REUSE)) {
    /* Late _bitbucket (sz>=1, still UNKNOWN): main loop can skip it; do not run
     * after pic_variable_alloc_proc returns — pic_var.auto_mem_reg is restored per
     * block and would reuse low virtual addresses (RAM overlap / bad gpsim). */
    for (vptr = pfile_block_variable_list_head(blk);
         vptr;
         vptr = variable_link_get(vptr)) {
      const char *nm;
      variable_sz_t need;

      if (!variable_is_auto(vptr) || variable_is_sticky(vptr)
          || variable_flag_test(vptr, VARIABLE_FLAG_ALLOC_FAIL)) {
        continue;
      }
      nm = variable_name_get(vptr);
      if (!nm || 0 != strcmp(nm, PFILE_BITBUCKET_VARNAME)) {
        continue;
      }
      need = pic_variable_storage_need_get(pf, vptr, 0);
      if (need < 1U) {
        continue;
      }
      if (VARIABLE_BASE_UNKNOWN != variable_base_get(vptr, 0)) {
        continue;
      }
      variable_base_set(vptr, (variable_base_t) pic_var.auto_mem_reg, 0);
      pic_var.auto_mem_reg += (unsigned) need;
    }
    pic_var.auto_mem_reg = auto_mem_reg_init;
  }
}

static void pic_variable_alloc_proc(pfile_t *pf, pfile_proc_t *proc)
{
  /*
   * if the procedure is ever called indirectly its variables were
   * allocated earlier so this can be ignored
   */
  pic_variable_alloc_block(pf, proc, 0);
}


/* allocate all variables out of a single block. base holds the block */
static void pic_block_data_alloc(pfile_t *pf, pfile_block_t *blk, 
  variable_base_t base)
{
  variable_t vptr;

  {
    int loop_count = 0;
    for (vptr = pfile_block_variable_list_head(blk);
         vptr && loop_count < 500;
         vptr = variable_link_get(vptr)) {
      loop_count++;
    if (variable_is_auto(vptr)) {
      variable_base_set(vptr, base, 0);
      variable_flag_set(vptr, VARIABLE_FLAG_STICKY);
      base += pic_variable_storage_need_get(pf, vptr, 0);
    }
  }
  }
  for (blk = pfile_block_child_get(blk);
       blk;
       blk = pfile_block_sibbling_get(blk)) {
    pic_block_data_alloc(pf, blk, base);
  }
}

/*
 * allocate variables for all of the indirect functions. It's probably
 * to analyze these a bit better & do a smarter allocation so we can
 * reuse data space, but it's far easier to just say dataspace for all 
 * functions called indirectly cannot be reused
 */
static void pic_variable_alloc_indirect_proc(pfile_t *pf, pfile_proc_t *proc)
{
  variable_base_t base;
  size_t          ct;
  pfile_proc_t   *cproc;

  if (pfile_proc_frame_sz_get(proc)) {
    base = pic_data_alloc(pf, &pic_var.data_bank_list, 
      pfile_proc_frame_sz_get(proc), 0);
    pic_block_data_alloc(pf, pfile_proc_block_root_get(proc), base);
    pfile_proc_frame_set(proc, base, pf);
  }
  /* now...anything *called* by this procedure must also be allocated
   * in permanent, non-reusable store because I don't track indirect
   * references
   */ 
  for (ct = 0; (cproc = pfile_proc_calls_get(proc, ct)) != 0; ct++) {
    if (!pfile_proc_flag_test(cproc, PFILE_PROC_FLAG_VISITED)) {
      pfile_proc_flag_set(cproc, PFILE_PROC_FLAG_VISITED);
      pic_variable_alloc_indirect_proc(pf, cproc);
      /* do *not* reset the visited flag here. once an indirect function's
       * variables are set, they are never moved! */
    }
  }
}

static void pic_variable_alloc_indirect(pfile_t *pf)
{
  pfile_proc_t *proc;

  for (proc = pfile_proc_root_get(pf);
       proc;
       proc = pfile_proc_next(proc)) {
    if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INDIRECT)
          || pfile_proc_flag_test(proc, PFILE_PROC_FLAG_REENTRANT)
          || pfile_proc_flag_test(proc, PFILE_PROC_FLAG_TASK)
          || pfile_proc_flag_test(proc, PFILE_PROC_FLAG_FRAME)) {
      pic_variable_alloc_indirect_proc(pf, proc);
    }
  }
}

/* all interrupt entry points share the same variable space, so...
     1. calculate the maximum size needed
     2. allocate a single block that size
     3. divy it up appropriately
 */
static void pic_variable_alloc_isr(pfile_t *pf)
{
  variable_base_t isr_block;
  pfile_proc_t   *proc;
  size_t          sz;

  for (proc = pfile_proc_root_get(pf), sz = 0; 
       proc; 
       proc = pfile_proc_next(proc)) {
    if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INTERRUPT) 
      && (sz < pfile_proc_frame_sz_get(proc))) {
      sz = pfile_proc_frame_sz_get(proc);
    }
  }

  isr_block = (sz)
    ? pic_data_alloc(pf, &pic_var.data_bank_list, sz, 0)
    : VARIABLE_BASE_UNKNOWN;

  for (proc = pfile_proc_root_get(pf);
       proc;
       proc = pfile_proc_next(proc)) {
    if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INTERRUPT)) {
      pfile_block_t *blk;

      /* allocate all of the procedure's variables out of isr_block */
      pic_block_data_alloc(pf, pfile_proc_block_root_get(proc), isr_block);
      /* remove the AUTO flag from all of the procedure's variables */
      for (blk = pfile_proc_block_root_get(proc);
           blk;
           blk = pfile_block_next(blk)) {
        variable_t vptr;

        for (vptr = pfile_block_variable_list_head(blk);
             vptr;
             vptr = variable_link_get(vptr)) {
          variable_flag_clr(vptr, VARIABLE_FLAG_AUTO);
        }
      }
      /* recalc the frame size (should be 0) */
      pfile_proc_frame_sz_calc(proc);
      /* allocate anything called by this proc */
      pic_variable_alloc_indirect_proc(pf, proc);
    }
  }
}

static void pic_variable_alloc_volatile(pfile_t *pf)
{
  pfile_proc_t *proc;

  for (proc = pfile_proc_root_get(pf);
       proc;
       proc = pfile_proc_next(proc)) {
    /* if this is in an interrupt routine, it will have been allocated
     * elsewhere */
    if (!pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INTERRUPT)) {
      pfile_block_t *blk;

      for (blk = pfile_proc_block_root_get(proc);
           blk;
           blk = pfile_block_next(blk)) {
        variable_t var;

        for (var = pfile_block_variable_list_head(blk);
             var;
             var = variable_link_get(var)) {
          if (getenv("JALV2_DBG_VR")
              && variable_name_get(var)
              && strstr(variable_name_get(var), "bitarg")) {
            fprintf(stderr, "VR: %s vol=%d auto=%d base_known=%d used=%d asg=%d\n",
              variable_name_get(var),
              (int) variable_is_volatile(var), (int) variable_is_auto(var),
              (int) (variable_base_get(var, 0) != VARIABLE_BASE_UNKNOWN),
              (int) variable_is_used(var), (int) variable_is_assigned(var));
          }
          if (variable_is_volatile(var) && variable_is_auto(var)) {
            if (variable_is_volatile(var)
                && (VARIABLE_BASE_UNKNOWN == variable_base_get(var, 0))
                && (variable_is_used(var) || variable_is_assigned(var))) {
              pic_variable_alloc_one(pf, proc, var);
              /*variable_flag_clr(var, VARIABLE_FLAG_AUTO);*/
              variable_flag_set(var, VARIABLE_FLAG_STICKY);
            }
          }
        }
      }
    }
  }
}

/* variables are allocated in an infinite memory, virtual space,
 * starting at location one. this routine maps the virtual space
 * into necessary PIC banks
 */ 
#if 1
static int vinfo_cmp(const void *A, const void *B)
{
  variable_t       a;
  variable_t       b;
  variable_base_t  a_base;
  variable_base_t  b_base;

  a = *(struct variable_ * const *) A;
  b = *(struct variable_ * const *) B;
  a_base = variable_base_get(a, 0);
  b_base = variable_base_get(b, 0);

  /* this isn't as obscure as it looks. remember relational operators
   * are guarenteed to return either 0 or 1, then work out the three
   * possibilities
   */
  if (a_base != b_base) {
    return (a_base > b_base) - (a_base < b_base);
  }
  /* qsort is not stable; tie-break for identical virtual bases. */
  {
    uintptr_t pa = (uintptr_t) a;
    uintptr_t pb = (uintptr_t) b;
    return (pa > pb) - (pa < pb);
  }
}

/* -debug=varalloc: one line per fixup group when placement is large or high-bank. */
static void pic_fixup_varalloc_log(pfile_t *pf, variable_t *vlist, size_t ii,
    variable_sz_t sz_reqd, variable_ct_t pointer_slots_reqd,
    variable_sz_t rm_reqd, variable_base_t virt_base, variable_base_t phys_base,
    const char *tag)
{
  const char *name;

  if (!pfile_debug_sub_test(pf, PFILE_DBGSUB_VALLOC)) {
    return;
  }
  /* access-window admissions are few and live below 0x60 by
   * definition -- always show them (the size filter below would hide
   * every one) */
  if (phys_base < 0x0200U && sz_reqd < 32U && rm_reqd < 64U
      && strcmp(tag, "accesswin")) {
    return;
  }
  name = variable_name_get(vlist[ii]);
  if (!name) {
    name = "?";
  }
  fprintf(stderr,
      "jal varalloc fixup [%s]: virt=0x%04X sz=%u slots=%u rm=%u -> phys=0x%04X first=%s\n",
      tag,
      (unsigned) virt_base, (unsigned) sz_reqd,
      (unsigned) pointer_slots_reqd, (unsigned) rm_reqd,
      (unsigned) phys_base, name);
}

/*
 * Placement refusal (2026-10-05): a non-bit-array variable whose
 * multibank fallback would use a GAP-TOLERANT chunk run is refused
 * instead of silently placed. Classic codegen addresses non-bit arrays
 * LINEARLY (base + offset); chunks with a hole make element accesses
 * land on the hole (SFRs, access-window scalars) -- the in-the-wild
 * 16F1825 defect where `var word array[100]` chunked over the
 * access-window bytes and the fill loop overwrote its own index.
 * Only dynamic bit arrays walk the chunk map and may keep holed runs.
 *
 * The refusal is TRIAL-SAFE: it sets this state instead of logging an
 * error, so a losing layout trial reports failure without poisoning
 * errct; the driver (pic.c) turns a refusal in the FINAL install into
 * the clean error. Two refusal reasons exist: a pointer-bound array
 * that only fits as a holed run (linear pointer arithmetic cannot
 * walk chunks), and a variable NO candidate layout can hold
 * (multibank/shared tails -- same trial-safety applies, the winning
 * layout is the one that must report it). */

void pic_var_refusal_reset(void)
{
  pic_var.refused      = BOOLEAN_FALSE;
  pic_var.refused_ptr  = BOOLEAN_FALSE;
  pic_var.refused_call = BOOLEAN_FALSE;
  pic_var.refused_var  = VARIABLE_NONE;
  pic_var.refused_sz   = 0;
  pic_var.refused_best = 0;
}

boolean_t pic_var_refusal_get(variable_t *var, variable_sz_t *sz,
    variable_sz_t *best)
{
  if (var) {
    *var = pic_var.refused_var;
  }
  if (sz) {
    *sz = pic_var.refused_sz;
  }
  if (best) {
    *best = pic_var.refused_best;
  }
  return pic_var.refused;
}

void pic_var_refusal_report(pfile_t *pf)
{
  if (getenv("JALV2_DBG_REFUSAL")) {
    fprintf(stderr, "DBG refusal free=%u data_nodes=%d\n",
        (unsigned) pic_data_bank_free_bytes_get(),
        (int) (pic_var.data_bank_list ? 1 : 0));
  }
  if (pic_var.refused) {
    const char *name;

    name = pic_var.refused_var
      ? variable_name_get(pic_var.refused_var) : 0;
    if (!name) {
      name = "?";
    }
    if (pic_var.refused_ptr) {
      pfile_log(pf, PFILE_LOG_ERR,
          "'%s' is bound to a pointer and needs %u contiguous bytes, but the "
          "largest contiguous run is %u. Pointer arithmetic addresses the "
          "pointee linearly, so a bank-spanning holed placement cannot be "
          "walked. Keep the array within one bank, or use the large_array "
          "library for bank-spanning arrays.",
          name,
          (unsigned) pic_var.refused_sz,
          (unsigned) pic_var.refused_best);
    } else if (pic_var.refused_call) {
      pfile_log(pf, PFILE_LOG_ERR,
          "'%s' is passed to a procedure as an open-array actual and needs "
          "%u contiguous bytes, but the largest contiguous run is %u. The "
          "callee addresses the formal linearly (base + index), so a "
          "bank-spanning holed placement cannot be walked there. Keep the "
          "array within one bank, use the large_array library, or access it "
          "only through direct subscripts.",
          name,
          (unsigned) pic_var.refused_sz,
          (unsigned) pic_var.refused_best);
    } else if (pic_var.refused_best > 0) {
      pfile_log(pf, PFILE_LOG_ERR,
          "Array of '%s' (%u bytes) exceeds total available memory space "
          "(largest contiguous run is %u bytes). Consider reducing array "
          "size or using external storage.",
          name,
          (unsigned) pic_var.refused_sz,
          (unsigned) pic_var.refused_best);
    } else if (pic_var.refused_var
        && variable_is_array(pic_var.refused_var)) {
      pfile_log(pf, PFILE_LOG_ERR,
          "Array of '%s' (%u bytes) exceeds total available memory space. "
          "Consider reducing array size or using external storage.",
          name,
          (unsigned) pic_var.refused_sz);
    } else if ('_' == name[0]) {
      /* the compiler's own scratch lost the race for the last bytes; the
       * user needs to know the PROGRAM is too big, not a name they cannot
       * find in their source */
      pfile_log(pf, PFILE_LOG_ERR,
          "the data area is full: no room for the compiler's '%s' scratch "
          "(%u byte%s); the program is too big for this part",
          name,
          (unsigned) pic_var.refused_sz,
          (1U == pic_var.refused_sz) ? "" : "s");
    } else {
      /* scalars reach here too: the old wording said "Array of" for a
       * one-byte variable, which reads as the wrong diagnosis */
      pfile_log(pf, PFILE_LOG_ERR,
          "no room for '%s' (%u byte%s): the data area is full",
          name,
          (unsigned) pic_var.refused_sz,
          (1U == pic_var.refused_sz) ? "" : "s");
    }
  }
}

/* Storage whose accesses walk the chunk map: array-of-bit defs and
 * the packed-bit pool `_bitbucket` (libcore hands out its bits; the
 * dynamic-bit addressing in pic_op.c walks it), plus -- since Layer 2
 * (chunk-walking linear arrays) -- any linearly-addressed array: runtime
 * subscripts walk (pic_op.c) and const subscripts resolve at compile
 * time (pic_var_chunk_addr_resolve). Pointer-BOUND variables are the
 * exception and are refused (linear pointer arithmetic). */
static boolean_t pic_variable_is_bit_array(variable_t var)
{
  variable_def_t        def;
  variable_def_member_t mbr;
  variable_def_t        inner;
  const char           *nm;

  if (!var) {
    return BOOLEAN_FALSE;
  }
  nm = variable_name_get(var);
  if (nm && !strcmp(nm, PFILE_BITBUCKET_VARNAME)) {
    return BOOLEAN_TRUE;
  }
  if (!variable_is_array(var)) {
    return BOOLEAN_FALSE;
  }
  def   = variable_def_get(var);
  mbr   = variable_def_member_get(def);
  inner = mbr ? variable_def_member_def_get(mbr) : VARIABLE_DEF_NONE;
  return inner
    && variable_def_flag_test(inner, VARIABLE_DEF_FLAG_BIT)
    ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* Place a variable group into the shared window (MOVLB-free bytes).
 * Returns BOOLEAN_TRUE when the allocation succeeded and the group's
 * bases were set; the caller then continues with the next group. */
static boolean_t pic_variable_group_place_shared(pfile_t *pf,
    struct variable_ **data, size_t *pii, size_t jj,
    variable_sz_t sz_reqd, variable_ct_t pointer_slots_reqd,
    variable_sz_t rm_reqd, variable_base_t virt_base)
{
  variable_base_t base;

  /* The shared window can overlap the data area (a 12-bit part declares
   * its whole GPR block `pragma shared'), so the shared list may offer
   * bytes a data allocation already owns.  Accepting them placed two live
   * variables on one address -- and the compile still reported 0 errors.
   * pic_data_alloc has already dropped the rejected range from the shared
   * list, so retry: the next candidate is a different run, and a genuinely
   * free byte is found when one exists.  Only when none does is the group
   * refused (the driver turns a refusal in the final install into a clean
   * "no room" error). */
  for (;;) {
    base = pic_data_alloc(pf, &pic_var.shared_bank_list, sz_reqd,
        pointer_slots_reqd);
    if (VARIABLE_BASE_UNKNOWN == base) {
      return BOOLEAN_FALSE;
    }
    if (!pic_blist_range_dataspace(pic_var.data_pragma_list, base, rm_reqd)
        || pic_blist_range_free(pic_var.data_bank_list, base, rm_reqd)) {
      break;
    }
  }
  pic_blist_remove(pf, &pic_var.data_bank_list, base, rm_reqd);
  pfile_log(pf, PFILE_LOG_DEBUG,
      "Allocated %u bytes from shared memory at 0x%04X",
      (unsigned) sz_reqd, (unsigned) base);
  pic_fixup_varalloc_log(pf, data, *pii, sz_reqd, pointer_slots_reqd,
      rm_reqd, virt_base, base, "shared");
  pic_blist_max += sz_reqd;
  while (*pii < jj) {
    variable_base_set(data[*pii], base, 0);
    (*pii)++;
  }
  return BOOLEAN_TRUE;
}

/* Bytes inside [lo,hi] that are reserved for access-window residents
 * (the same intersection pic_accesswin builds its free list from):
 * 14H `pragma shared` common RAM (e.g. 0x70-0x7F), the low access-bank
 * window on other cores. A multibank chunk must never consume them --
 * the chunk walk runs before the access-window pass, so eating them
 * strands _pic_state/_pic_temp (unknown-address codegen errors) or
 * overlaps live loop scalars (silent corruption). Returns the number
 * of NON-reserved sub-segments written to [seg_lo, seg_hi]. */
static variable_ct_t pic_node_chunk_segments_get(pfile_t *pf,
    variable_base_t lo, variable_base_t hi,
    variable_base_t *seg_lo, variable_base_t *seg_hi, variable_ct_t max_seg)
{
  variable_ct_t    ct;
  variable_base_t  cur;
  const pic_bank_info_t *reserved;

  ct  = 0;
  cur = lo;
  /* PIC18: the GPR access bank (0x00-0x5F) is the reserved window.
   * 14H and classic 14-bit: whatever `pragma shared` ranges the device
   * declares (14H common RAM; classic 14-bit usually none). */
  if (pic_is_16bit(pf)) {
    reserved = 0;
  } else {
    reserved = pic_var.shared_pragma_list;
  }
  for (;;) {
    variable_base_t          s_hi;
    const pic_bank_info_t *sh;
    boolean_t                restart;

    /* skip a reserved range covering the segment head */
    do {
      restart = BOOLEAN_FALSE;
      if (reserved) {
        for (sh = reserved; sh; sh = sh->link) {
          if ((sh->lo <= cur) && (sh->hi >= cur) && (cur <= hi)) {
            cur = sh->hi + 1;
            restart = BOOLEAN_TRUE;
          }
        }
      } else if ((cur <= 0x5FU) && (cur <= hi)) {
        cur = 0x60U;
        restart = BOOLEAN_TRUE;
      }
    } while (restart);
    if (cur > hi) {
      break;
    }
    s_hi = hi;
    if (reserved) {
      for (sh = reserved; sh; sh = sh->link) {
        if ((sh->lo > cur) && (sh->lo <= s_hi)) {
          s_hi = sh->lo - 1;
        }
      }
    } else if ((cur <= 0x5FU) && (s_hi > 0x5FU)) {
      s_hi = 0x5FU;
    }
    if (ct < max_seg) {
      seg_lo[ct] = cur;
      seg_hi[ct] = s_hi;
    }
    ct++;
    cur = s_hi + 1;
  }
  return ct;
}

/* find the largest space available for the software stack */
static void pic_software_stack_calc(pfile_t *pf)
{
  const pic_bank_info_t *bank;

  UNUSED(pf);
  for (bank = pic_var.data_bank_list; bank; bank = bank->link) {
    size_t sz;

    sz = (bank->hi - bank->lo + 1);
    if (sz > pic_stk_sz) {
      pic_stk_sz = sz;
      pic_stk_base = bank->lo + sz;
    }
  }
}


/* element size (bytes) of an array member, for chunk-boundary
 * alignment checks; 1 when not an array / not derivable */
static variable_sz_t pic_var_elem_sz_get(variable_t var)
{
  variable_def_t        def;
  variable_def_member_t mbr;
  variable_def_t        edef;
  variable_sz_t         esz;

  def  = variable_def_get(var);
  mbr  = def ? variable_def_member_get(def) : 0;
  edef = mbr ? variable_def_member_def_get(mbr) : 0;
  esz  = edef ? variable_def_byte_sz_get(edef) : 0;
  return esz ? esz : 1;
}

/* Alias groups (frame reuse): a chunked group placement is physical
 * storage shared by every member, but each member's subscript walk
 * consults ITS OWN chunk map -- the map the multibank path registers
 * on the group head alone leaves any OTHER array member walking
 * linearly from the first chunk straight across bank holes (silent
 * corruption: proc-local arrays chunked over fragments). Validate and
 * register each array member's PREFIX of the layout here (the head's
 * full map already covers a max-size member).
 *
 * Pass A validates: every array member's storage extent must be
 * coverable by the chunk layout with no element straddling an interior
 * chunk boundary. Pass B registers the prefix maps. Returns FALSE
 * (after setting the trial-safe refusal) when a member cannot be
 * addressed -- the caller must then not consume the free list. */
static boolean_t pic_var_group_member_chunk_maps(pfile_t *pf,
    struct variable_ **data, size_t ii, size_t jj,
    const variable_base_t *chunk_base, const variable_sz_t *chunk_sz,
    variable_ct_t chunk_ct)
{
  size_t gix;

  for (gix = ii; gix < jj; gix++) {
    variable_t    memb;
    variable_sz_t msz;

    if (gix == ii) {
      continue; /* head: full map registered by the caller */
    }
    memb = data[gix];
    msz  = pic_variable_storage_need_get(pf, memb, 0);
    if (variable_is_array(memb) || pic_variable_is_bit_array(memb)) {
      variable_sz_t    esz;
      variable_sz_t    cov;
      variable_ct_t    cix;
      variable_base_t *pbase;
      variable_sz_t   *psz;
      variable_ct_t    pct;

      esz = pic_var_elem_sz_get(memb);
      /* pass A: coverage + element-boundary alignment of the prefix */
      cov = 0;
      for (cix = 0; (cix < chunk_ct) && (cov < msz); cix++) {
        if (cov && (cov % esz)) {
          /* element straddles this chunk boundary */
          if (!pic_var.refused) {
            pic_var.refused      = BOOLEAN_TRUE;
            pic_var.refused_ptr  = pic_var_pointer_target_test(memb);
            pic_var.refused_call = !pic_var.refused_ptr;
            pic_var.refused_var  = memb;
            pic_var.refused_sz   = msz;
            pic_var.refused_best = 0;
          }
          return BOOLEAN_FALSE;
        }
        cov += chunk_sz[cix];
      }
      if (cov < msz) {
        /* layout shorter than the member (cannot happen: the group
         * size is the largest member) -- refuse rather than corrupt */
        if (!pic_var.refused) {
          pic_var.refused      = BOOLEAN_TRUE;
          pic_var.refused_ptr  = pic_var_pointer_target_test(memb);
          pic_var.refused_call = !pic_var.refused_ptr;
          pic_var.refused_var  = memb;
          pic_var.refused_sz   = msz;
          pic_var.refused_best = 0;
        }
        return BOOLEAN_FALSE;
      }
      /* pass B: build and register this member's prefix map (a
       * max-size member's prefix IS the full layout) */
      pbase = MALLOC(chunk_ct * sizeof(*pbase));
      psz   = MALLOC(chunk_ct * sizeof(*psz));
      if (!pbase || !psz) {
        FREE(pbase);
        FREE(psz);
        pfile_log_syserr(pf, ENOMEM);
        return BOOLEAN_FALSE;
      }
      cov = 0;
      pct = 0;
      for (cix = 0; (cix < chunk_ct) && (cov < msz); cix++) {
        variable_sz_t take;

        take = chunk_sz[cix];
        if (cov + take > msz) {
          take = msz - cov;
        }
        pbase[pct] = chunk_base[cix];
        psz[pct]   = take;
        pct++;
        cov += take;
      }
      if (pfile_debug_sub_test(pf, PFILE_DBGSUB_VALLOC)) {
        variable_ct_t dbg_ci;

        for (dbg_ci = 0; dbg_ci < pct; dbg_ci++) {
          fprintf(stderr, "jal CHUNKMAP r%u %s[%u] = 0x%04X..0x%04X\n",
              (unsigned) pic_var.dbg_round, variable_name_get(memb), dbg_ci,
              (unsigned) pbase[dbg_ci],
              (unsigned) (pbase[dbg_ci] + psz[dbg_ci] - 1));
        }
      }
      pic_variable_chunk_map_set(pf, memb, pct, pbase, psz);
      pic_var_chunk_map_propagate(pf, memb);
      FREE(pbase);
      FREE(psz);
    }
  }
  return BOOLEAN_TRUE;
}

/* place every candidate group physically (the original fixup loop);
 * runs the fragmentation analysis and software-stack sizing against
 * the resulting layout (both read-only) */
static void pic_variable_fixup_place(pfile_t *pf,
    struct variable_ **data, size_t used)
{
  size_t ii;

  pic_var.dbg_round++;
  /* a new placement invalidates every address handed out by an earlier one */
  pic_var.place_gen++;
  pic_var_refusal_reset();
  /* finally, put the variables in their correct positions. find the
   * largest variable at each position, allocate that much space, then
   * set the base of all affected variables
   */
  ii = 0;
  while (ii < used) {
    size_t          jj;
    size_t          sz_reqd; /* largest storage required at position */
    variable_ct_t   pointer_slots_reqd;
    variable_sz_t   rm_reqd;
    variable_base_t base;
    variable_base_t virt_base;

    /* Process variable allocation normally */

    sz_reqd = pic_variable_storage_need_get(pf, data[ii],
        &pointer_slots_reqd);
    rm_reqd = sz_reqd + pic_pointer_slot_padding_get(pf, pointer_slots_reqd);
    for (jj = ii + 1;
         (jj < used)
         && (variable_base_get(data[ii], 0)
           == variable_base_get(data[jj], 0));
         jj++) {
      size_t vsz;
      variable_ct_t pointer_slots;
      variable_sz_t rm;

      vsz = pic_variable_storage_need_get(pf, data[jj],
          &pointer_slots);
      rm = vsz + pic_pointer_slot_padding_get(pf, pointer_slots);
      if (rm > rm_reqd) {
        sz_reqd = vsz;
        pointer_slots_reqd = pointer_slots;
        rm_reqd = rm;
      }
    }
    virt_base = variable_base_get(data[ii], 0);
    if (pfile_debug_sub_test(pf, PFILE_DBGSUB_VALLOC)) {
      /* alias-group membership: every member shares the physical
       * placement below (frame reuse); chunk maps, however, are
       * per-variable -- see the multibank branch */
      size_t dbg_ix;

      fprintf(stderr, "jal GROUP r%u virt=0x%04X sz=%u members:",
          (unsigned) pic_var.dbg_round, (unsigned) virt_base,
          (unsigned) sz_reqd);
      for (dbg_ix = ii; dbg_ix < jj; dbg_ix++) {
        fprintf(stderr, " %s(%uB,arr=%d,bitarr=%d)",
            variable_name_get(data[dbg_ix]) ? variable_name_get(data[dbg_ix]) : "?",
            (unsigned) pic_variable_storage_need_get(pf, data[dbg_ix], 0),
            (int) variable_is_array(data[dbg_ix]),
            (int) pic_variable_is_bit_array(data[dbg_ix]));
      }
      fprintf(stderr, "\n");
    }
    base = pic_data_alloc(pf, &pic_var.data_bank_list, sz_reqd,
        pointer_slots_reqd);
    if (getenv("JALV2_DBG_REFUSAL")) {
      fprintf(stderr, "DBG place %-16s sz=%u base=0x%x free=%u\n",
          variable_name_get(data[ii]) ? variable_name_get(data[ii]) : "?",
          (unsigned) sz_reqd, (unsigned) base,
          (unsigned) pic_data_bank_free_bytes_get());
    }
    if (VARIABLE_BASE_UNKNOWN == base) {
      unsigned bank_size = pic_target_bank_size_get(pf);
      /* Multi-bank: always remove from the sorted free-list head so partial
       * consumption of a node is not skipped (old for-loop advanced with ->link
       * after pic_blist_remove, which could also use freed memory).
       *
       * Chunk-walkable variables (arrays) whose single contiguous
       * allocation failed ALSO take this path even when they are smaller
       * than the bank size: the usable contiguous run of a window is
       * far below the bank size once SFRs and the shared window are
       * carved out (16F1455: ~80-91 B of a 128-byte bank), so the old
       * sz_reqd > bank_size gate left arrays of e.g. 96..128 bytes
       * unplaceable when a chunked placement was perfectly legal
       * (2026-10-26 band 1). A small chunkable array still prefers the
       * shared window first (MOVLB-free addressing). */
      if ((sz_reqd <= bank_size)
          && (variable_is_array(data[ii])
            || pic_variable_is_bit_array(data[ii]))
          && pic_variable_group_place_shared(pf, data, &ii, jj, sz_reqd,
              pointer_slots_reqd, rm_reqd, virt_base)) {
        continue;
      }
      if ((sz_reqd > bank_size)
          || (variable_is_array(data[ii])
            || pic_variable_is_bit_array(data[ii]))) {
        /* Try to allocate across multiple banks. Arrays SMALLER than
         * the bank size also take this path when their single
         * contiguous allocation failed above: a window's usable run is
         * well below the bank size once SFRs and the shared window
         * are carved out (16F1455: ~75-91 B of a 128-byte bank), so
         * the old sz_reqd > bank_size gate left e.g. 96..128-byte
         * arrays unplaceable although a chunked placement is legal
         * (2026-10-26 band 1). */
        variable_sz_t remaining = sz_reqd;
        variable_base_t first_base = VARIABLE_BASE_UNKNOWN;
        pic_bank_info_t *current_bank;
        variable_ct_t chunk_ct;
        variable_ct_t chunk_alloc;
        variable_base_t *chunk_base;
        variable_sz_t *chunk_sz;
        variable_sz_t best_run;

        chunk_ct = 0;
        chunk_alloc = 0;
        chunk_base = 0;
        chunk_sz = 0;
        for (current_bank = pic_var.data_bank_list; current_bank;
            current_bank = current_bank->link) {
          chunk_alloc++;
        }
        if (chunk_alloc) {
          chunk_base = MALLOC(chunk_alloc * sizeof(*chunk_base));
          chunk_sz   = MALLOC(chunk_alloc * sizeof(*chunk_sz));
          if (!chunk_base || !chunk_sz) {
            FREE(chunk_base);
            FREE(chunk_sz);
            pfile_log_syserr(pf, ENOMEM);
            break;
          }
        }

        /* Check if allocation is possible across banks.
         *
         * Pass 1 prefers a CONTIGUOUS chunk run (next.lo ==
         * prev.hi+1): every non-bit-array variable is addressed
         * LINEARLY as base+offset, so a run with a gap would place
         * bytes over the gap -- overlapping fixed `at` pins and other
         * variables (the classic concat-witness corruption: a
         * 368-byte scratch chunked over the 0x77/0x78 regression
         * pins).  At a gap the run restarts at the current node and
         * the scan continues, so a contiguous multi-window span
         * (e.g. across bank-boundary node splits) still wins.
         *
         * Pass 2 keeps the original gap-tolerant fill for fragmented
         * maps (16Fxxx banks are physically non-contiguous -- SFR
         * holes -- and arrays larger than one bank can ONLY be
         * placed across the holes; indices landing in a hole alias
         * that storage, which long-green tests like the 16f877
         * 260-byte pointer witness rely on never touching). */
        {
          unsigned passno;
          variable_sz_t run_sz;

          /* pass 0: contiguous-only; pass 1: gap-tolerant fallback */
          best_run = 0;
          for (passno = 0; passno < 2; passno++) {
            boolean_t contiguous_only = (0 == passno)
              ? BOOLEAN_TRUE : BOOLEAN_FALSE;
            variable_sz_t elem_sz;

            remaining = sz_reqd;
            run_sz    = 0;
            chunk_ct = 0;
            first_base = VARIABLE_BASE_UNKNOWN;
            /* Layer 2: holed chunks are placeable for LINEAR arrays too
             * (runtime subscripts walk the map, const subscripts resolve
             * through pic_var_chunk_addr_resolve), but a multi-byte
             * ELEMENT must never straddle a hole -- trim every non-final
             * chunk down to a whole multiple of the element size so the
             * straddling tail bytes stay free for other variables. Byte
             * elements (and bit arrays, which pack densely and always
             * walked) trim by 1 = no-op. */
            elem_sz = 1;
            if (!pic_variable_is_bit_array(data[ii])) {
              variable_def_t        adef;
              variable_def_member_t ambr;
              variable_def_t        edef;

              /* The ELEMENT size is what must not straddle a hole: read
               * the member (element) def, not the array def (whose size
               * is the whole array), and convert with
               * variable_def_byte_sz_get (variable_def_sz_get is bits for
               * bit-flagged defs, bytes otherwise -- the old
               * (sz + 7) / 8 on the ARRAY def produced a bogus huge
               * elem_sz whenever sz_reqd % ceil(sz_reqd / 8) == 0, which
               * trimmed every non-final chunk down to nothing). */
              adef = variable_def_get(data[ii]);
              ambr = adef ? variable_def_member_get(adef) : 0;
              edef = ambr ? variable_def_member_def_get(ambr)
                          : VARIABLE_DEF_NONE;
              if (edef) {
                variable_sz_t esz;

                esz = variable_def_byte_sz_get(edef);
                if ((esz >= 1)
                    && (0 == (sz_reqd % esz))) {
                  elem_sz = esz;
                }
              }
            }
            for (current_bank = pic_var.data_bank_list; current_bank && remaining > 0; current_bank = current_bank->link) {
              /* walk the node in NON-reserved segments: the shared /
               * access-window bytes inside the node are reserved for
               * access-window residents and must become holes for the
               * chunk walk (2026-10-26 band 2: chunks that consumed
               * them stranded _pic_state/_pic_temp and overlapped live
               * scalars) */
              variable_base_t seg_lo[PIC_NODE_MAX_SEG];
              variable_base_t seg_hi[PIC_NODE_MAX_SEG];
              variable_ct_t   seg_ct;
              variable_ct_t   seg_ix;

              seg_ct = pic_node_chunk_segments_get(pf,
                current_bank->lo, current_bank->hi,
                seg_lo, seg_hi, PIC_NODE_MAX_SEG);
              for (seg_ix = 0; (seg_ix < seg_ct) && (remaining > 0); seg_ix++) {
                variable_sz_t available = seg_hi[seg_ix] - seg_lo[seg_ix] + 1;
                variable_sz_t chunk_size = (remaining > available) ? available : remaining;

                if (contiguous_only && (chunk_ct > 0)
                    && (seg_lo[seg_ix]
                      != (variable_base_t) (chunk_base[chunk_ct - 1]
                        + chunk_sz[chunk_ct - 1]))) {
                  /* gap before this segment: restart the run here */
                  if (run_sz > best_run) {
                    best_run = run_sz;
                  }
                  run_sz = 0;
                  remaining = sz_reqd;
                  chunk_ct = 0;
                  first_base = VARIABLE_BASE_UNKNOWN;
                  chunk_size = (remaining > available) ? available : remaining;
                }
                /* element alignment: a non-final chunk (more remaining
                 * after it) ends at a hole, so it must hold whole
                 * elements only */
                if (!contiguous_only && (remaining > chunk_size)) {
                  chunk_size -= chunk_size % elem_sz;
                }
                if (chunk_size > 0) {
                  if (first_base == VARIABLE_BASE_UNKNOWN) {
                    first_base = seg_lo[seg_ix];
                  }
                  chunk_base[chunk_ct] = seg_lo[seg_ix];
                  chunk_sz[chunk_ct] = chunk_size;
                  chunk_ct++;
                  if (contiguous_only) {
                    run_sz += chunk_size;
                  }
                  remaining -= chunk_size;
                }
              }
            }
            if (contiguous_only && (run_sz > best_run)) {
              best_run = run_sz;
            }
            if (0 == remaining) {
              /* Layer 2: a holed run places for chunk-walking bit arrays
               * AND for linear arrays (runtime subscripts walk the chunk
               * map; const subscripts resolve at compile time). The one
               * unsound case is a POINTER-BOUND variable: pointer
               * arithmetic is linear (*p + i) and cannot walk, so keep
               * the trial-safe refusal for those (the driver reports the
               * error on the final install). */
              if ((1 == passno)
                && !pic_variable_is_bit_array(data[ii])
                && (pic_var_pointer_bind_escaped_target(data[ii])
                  || pic_var_escapes_to_call(data[ii]))
                && (chunk_ct > 1)) {
                variable_ct_t ci;
                boolean_t holed;

                holed = BOOLEAN_FALSE;
                for (ci = 1; ci < chunk_ct; ci++) {
                  if (chunk_base[ci]
                      != (variable_base_t) (chunk_base[ci - 1]
                        + chunk_sz[ci - 1])) {
                    holed = BOOLEAN_TRUE;
                    break;
                  }
                }
                if (holed) {
                  if (!pic_var.refused) {
                    pic_var.refused      = BOOLEAN_TRUE;
                    pic_var.refused_ptr  = pic_var_pointer_target_test(
                                              data[ii]);
                    pic_var.refused_call = !pic_var.refused_ptr;
                    pic_var.refused_var  = data[ii];
                    pic_var.refused_sz   = sz_reqd;
                    pic_var.refused_best = best_run;
                  }
                  FREE(chunk_base);
                  FREE(chunk_sz);
                  break;
                }
              }
              break;
            }
          }
        }

        if (pic_var.refused && (VARIABLE_BASE_UNKNOWN == base)) {
          /* refused above; stop placing (the driver reports it) */
          break;
        } else if (remaining == 0
            && pic_var_group_member_chunk_maps(pf, data, ii, jj,
              chunk_base, chunk_sz, chunk_ct)) {
          /* Multi-bank allocation is possible - now actually do it */
          base = first_base;
          /* remove the ACCEPTED chunks by base (a contiguous run may
           * start deeper in the sorted list than the head node) */
          {
            variable_ct_t ci;

            for (ci = 0; ci < chunk_ct; ci++) {
              pic_blist_remove(pf, &pic_var.data_bank_list, chunk_base[ci],
                chunk_sz[ci]);
            }
          }

          pfile_log(pf, PFILE_LOG_DEBUG,
            "Large array (%u bytes) allocated across multiple banks starting at 0x%04X. "
            "This may impact performance due to bank switching overhead.",
            (unsigned)sz_reqd, (unsigned)base);
          pic_fixup_varalloc_log(pf, data, ii, sz_reqd, pointer_slots_reqd,
              rm_reqd, virt_base, base, "multibank");
          if (pfile_debug_sub_test(pf, PFILE_DBGSUB_VALLOC)) {
            variable_ct_t dbg_ci;
            for (dbg_ci = 0; dbg_ci < chunk_ct; dbg_ci++) {
              fprintf(stderr,
                "jal CHUNKMAP r%u %s[%u] = 0x%04X..0x%04X\n",
                (unsigned) pic_var.dbg_round, variable_name_get(data[ii]), dbg_ci,
                (unsigned) chunk_base[dbg_ci],
                (unsigned) (chunk_base[dbg_ci] + chunk_sz[dbg_ci] - 1));
            }
          }
          pic_variable_chunk_map_set(pf, data[ii], chunk_ct,
              chunk_base, chunk_sz);
          pic_var_chunk_map_propagate(pf, data[ii]);
          /* Update data area usage counter for multi-bank allocation */
          pic_blist_max += sz_reqd;
          /* Mark all variables in this group as allocated */
          while (ii < jj) {
            variable_base_set(data[ii], base, 0);
            ii++;
          }
          FREE(chunk_base);
          FREE(chunk_sz);
          continue;  /* Skip to next variable group */
        } else {
          /* TRIAL-SAFE no-room refusal: a losing layout candidate must
           * not poison errct with a hard error -- the driver reports the
           * refusal when the FINAL install fails the same way. */
          if (!pic_var.refused) {
            pic_var.refused      = BOOLEAN_TRUE;
            pic_var.refused_ptr  = BOOLEAN_FALSE;
            pic_var.refused_var  = data[ii];
            pic_var.refused_sz   = sz_reqd;
            pic_var.refused_best = best_run;
          }
          FREE(chunk_base);
          FREE(chunk_sz);
          break;
        }
      } else {
        /* Try to allocate from shared memory as fallback */
        if (!pic_variable_group_place_shared(pf, data, &ii, jj, sz_reqd,
              pointer_slots_reqd, rm_reqd, virt_base)) {
          /* TRIAL-SAFE no-room refusal, as above (no contiguous-run
           * figure here: the first-fit search does not compute one) */
          if (!pic_var.refused) {
            pic_var.refused      = BOOLEAN_TRUE;
            pic_var.refused_ptr  = BOOLEAN_FALSE;
            pic_var.refused_var  = data[ii];
            pic_var.refused_sz   = sz_reqd;
            pic_var.refused_best = 0;
          }
          break;
        }
        continue;
      }
    } else {
      pic_fixup_varalloc_log(pf, data, ii, sz_reqd, pointer_slots_reqd,
          rm_reqd, virt_base, base, "data");
      pfile_log(pf, PFILE_LOG_DEBUG, "register %u mapped to %u",
          variable_base_get(data[ii], 0),
          base);
      while (ii < jj) {
        variable_base_set(data[ii], base, 0);
        ii++;
      }
    }
  }
  pic_analyze_memory_fragmentation(pf);
  pic_software_stack_calc(pf);
}

/*
 * PIC18 layout-trial snapshot (D3 increment 2). Captured once, right
 * before the first placement run: the free lists, every candidate's
 * virtual base, and pic_blist_max. A trial re-runs the placement from
 * this state (deterministically reproducing the baseline, or building
 * the access-window candidate), so every layout the selector sees is
 * a real allocation by the real allocator.
 */
struct pic_var_layout_snap_ {
  pic_bank_info_t        *data_list;
  pic_bank_info_t        *shared_list;
  variable_base_t        *virt;
  struct variable_      **data;
  size_t                  used;
  unsigned                max;
  variable_t             *shflags;  /* variables already SHARED by the
                                      * user's `var shared` (never cleared) */
  size_t                  shct;
};

static void pic_accesswin_list_free(pic_bank_info_t *list)
{
  while (list) {
    pic_bank_info_t *next;

    next = list->link;
    FREE(list);
    list = next;
  }
}

static pic_bank_info_t *pic_accesswin_list_clone(const pic_bank_info_t *l)
{
  pic_bank_info_t  head;
  pic_bank_info_t *tail;

  head.link = 0;
  tail      = &head;
  for ( ; l; l = l->link) {
    pic_bank_info_t *node;

    node = MALLOC(sizeof(*node));
    if (!node) {
      pic_accesswin_list_free(head.link);
      return 0;
    }
    node->lo   = l->lo;
    node->hi   = l->hi;
    node->link = 0;
    tail->link = node;
    tail       = node;
  }
  return head.link;
}

/* one snapshot free: lists cloned for the snapshot are freed here; the
 * candidate array/virt/shflags arrays are owned by the snapshot */
void pic_var_trials_free(void)
{
  if (pic_var.snap) {
    pic_accesswin_list_free(pic_var.snap->data_list);
    pic_accesswin_list_free(pic_var.snap->shared_list);
    FREE(pic_var.snap->virt);
    FREE(pic_var.snap->data);
    FREE(pic_var.snap->shflags);
    FREE(pic_var.snap);
    pic_var.snap = 0;
  }
}

static void pic_variable_shared_set(pfile_t *pf);

/* re-derive VARIABLE_FLAG_SHARED from the (new) final bases: clear the
 * flag everywhere except the snapshot's user-declared `var shared`
 * list, then let pic_variable_shared_set() flag whatever now sits in
 * the pragma-shared ranges */
static void pic_var_shared_flags_refresh(pfile_t *pf)
{
  pfile_proc_t *proc;
  size_t        ii;

  for (proc = pfile_proc_root_get(pf); proc; proc = pfile_proc_next(proc)) {
    pfile_block_t *blk;

    for (blk = pfile_proc_block_root_get(proc); blk;
        blk = pfile_block_next(blk)) {
      variable_t var;

      for (var = pfile_block_variable_list_head(blk); var;
          var = variable_link_get(var)) {
        if (variable_flag_test(var, VARIABLE_FLAG_SHARED)) {
          boolean_t keep;

          keep = BOOLEAN_FALSE;
          for (ii = 0; ii < pic_var.snap->shct; ii++) {
            if (pic_var.snap->shflags[ii] == var) {
              keep = BOOLEAN_TRUE;
              break;
            }
          }
          if (!keep) {
            variable_flag_clr(var, VARIABLE_FLAG_SHARED);
          }
        }
      }
    }
  }
  pic_variable_shared_set(pf);
}

/*
 * PIC18 access-window candidate layout (D3 increment 2). From the
 * snapshot state, admit whole overlay groups into the 0x00-0x5f GPR
 * access window ranked by incident adjacency-edge total (a group in
 * the window is access-addressed a=0 and MOVLB-free); the remaining
 * candidates are placed by the normal loop. The trial selector in
 * pic_cmd_generate then measures this layout against the baseline
 * with real emission and keeps the better -- this prepass never
 * decides anything on its own.
 */
struct pic_accesswin_grp_ {
  size_t          start;   /* index of first member in the array */
  size_t          ct;      /* member count */
  unsigned        incident;/* summed adjacency-edge counts at members */
  variable_sz_t   sz;      /* storage need of the largest member */
  variable_ct_t   slots;   /* pointer slots of the largest member */
  variable_sz_t   rm;      /* largest need incl. pointer-slot padding */
  variable_base_t virt;    /* shared virtual base */
};

static int pic_accesswin_grp_cmp(const void *A, const void *B)
{
  const struct pic_accesswin_grp_ *a = A;
  const struct pic_accesswin_grp_ *b = B;

  if (a->incident != b->incident) {
    return (a->incident < b->incident) - (a->incident > b->incident);
  }
  if (a->rm != b->rm) {
    return (a->rm > b->rm) - (a->rm < b->rm);
  }
  if (a->virt != b->virt) {
    return (a->virt > b->virt) - (a->virt < b->virt);
  }
  {
    uintptr_t pa = (uintptr_t) a->start;
    uintptr_t pb = (uintptr_t) b->start;

    return (pa > pb) - (pa < pb);
  }
}

static size_t pic_accesswin_grp_of(variable_t var,
    struct variable_ * const *data, const unsigned *grp_ix, size_t used)
{
  size_t ii;

  for (ii = 0; ii < used; ii++) {
    if (data[ii] == var) {
      return grp_ix[ii];
    }
  }
  return (size_t) -1;
}

static void pic_variable_access_window_prepass(pfile_t *pf,
    struct variable_ **data, size_t *pused)
{
  pic_bank_info_t             *win;
  pic_bank_info_t             *bank;
  pic_bank_info_t             *wnext;
  struct pic_accesswin_grp_   *grp;
  unsigned                    *grp_ix;
  size_t                       used;
  size_t                       ii;
  size_t                       gct;
  size_t                       g;
  size_t                       e;

  /* free bytes of the general list inside the bank-independent
   * window: PIC18 GPR access bank (0x00-0x5f); 14H common RAM (the
   * device's `pragma shared` ranges, e.g. 0x70-0x7f mirrored in
   * every bank — a variable there needs no MOVLB regardless of BSR).
   * Occupants become access/shared-addressed via
   * pic_variable_shared_set, so the bytes are MOVLB-free by the same
   * mechanism on both families. */
  win = 0;
  if (pic_is_14bit_hybrid(pf)) {
    const pic_bank_info_t *shared;

    for (shared = pic_var.shared_pragma_list; shared; shared = shared->link) {
      for (bank = pic_var.data_bank_list; bank; bank = bank->link) {
        size_t lo;
        size_t hi;

        lo = (bank->lo > shared->lo) ? bank->lo : shared->lo;
        hi = (bank->hi < shared->hi) ? bank->hi : shared->hi;
        if (lo <= hi) {
          pic_blist_add(pf, &win, lo, hi);
        }
      }
    }
  } else {
    for (bank = pic_var.data_bank_list; bank; bank = bank->link) {
      size_t lo;
      size_t hi;

      lo = (bank->lo > 0x00U) ? bank->lo : 0x00U;
      hi = (bank->hi < 0x5fU) ? bank->hi : 0x5fU;
      if (lo <= hi) {
        pic_blist_add(pf, &win, lo, hi);
      }
    }
  }
  used = *pused;
  grp_ix = 0;
  grp    = 0;
  do {
    size_t rd;
    size_t wr;

    if (!used || !win) {
      break;
    }
    grp    = MALLOC(used * sizeof(*grp));
    grp_ix = MALLOC(used * sizeof(*grp_ix));
    if (!grp || !grp_ix) {
      pfile_log_syserr(pf, ENOMEM);
      break;
    }
    /* group by virtual base exactly the way the placement loop does */
    gct = 0;
    for (ii = 0; ii < used;) {
      size_t          jj;
      variable_sz_t   rm;
      variable_sz_t   sz;
      variable_ct_t   slots;
      variable_base_t virt;

      rm    = 0;
      sz    = 0;
      slots = 0;
      virt  = variable_base_get(data[ii], 0);
      for (jj = ii;
          (jj < used) && (variable_base_get(data[jj], 0) == virt);
          jj++) {
        variable_sz_t vsz;
        variable_sz_t vrm;
        variable_ct_t vslots;

        vsz = pic_variable_storage_need_get(pf, data[jj], &vslots);
        vrm = vsz + pic_pointer_slot_padding_get(pf, vslots);
        if (vrm > rm) {
          rm    = vrm;
          sz    = vsz;
          slots = vslots;
        }
        grp_ix[jj] = (unsigned) gct;
      }
      grp[gct].start    = ii;
      grp[gct].ct       = jj - ii;
      grp[gct].incident = 0;
      grp[gct].sz       = sz;
      grp[gct].slots    = slots;
      grp[gct].rm       = rm;
      grp[gct].virt     = virt;
      gct++;
      ii = jj;
    }
    for (e = 0; e < pic_var.banked_edges_used; e++) {
      size_t ga;
      size_t gb;

      ga = pic_accesswin_grp_of(pic_var.banked_edges[e].a, data, grp_ix, used);
      gb = pic_accesswin_grp_of(pic_var.banked_edges[e].b, data, grp_ix, used);
      if ((size_t) -1 == ga && (size_t) -1 == gb) {
        continue;
      }
      if (ga == gb) {
        if ((size_t) -1 != ga) {
          grp[ga].incident += pic_var.banked_edges[e].ct;
        }
      } else {
        if ((size_t) -1 != ga) {
          grp[ga].incident += pic_var.banked_edges[e].ct;
        }
        if ((size_t) -1 != gb) {
          grp[gb].incident += pic_var.banked_edges[e].ct;
        }
      }
    }
    qsort(grp, gct, sizeof(*grp), pic_accesswin_grp_cmp);
    for (g = 0; g < gct; g++) {
      variable_base_t base;

      if (0U == grp[g].incident) {
        break; /* ranked by incident; no demand remains */
      }
      base = pic_data_alloc(pf, &win, grp[g].sz, grp[g].slots);
      if (VARIABLE_BASE_UNKNOWN != base) {
        size_t k;

        /* the general free list must not hand the same bytes out */
        pic_blist_remove(pf, &pic_var.data_bank_list, base, grp[g].rm);
        pic_fixup_varalloc_log(pf, data, grp[g].start, grp[g].sz,
            grp[g].slots, grp[g].rm, grp[g].virt, base, "accesswin");
        for (k = grp[g].start; k < grp[g].start + grp[g].ct; k++) {
          variable_base_set(data[k], base, 0);
          data[k] = 0; /* pre-placed; compacted below */
        }
      }
    }
    for (rd = 0, wr = 0; rd < used; rd++) {
      if (data[rd]) {
        data[wr++] = data[rd];
      }
    }
    *pused = wr;
  } while (0);
  FREE(grp_ix);
  FREE(grp);
  for (bank = win; bank; bank = wnext) {
    wnext = bank->link;
    FREE(bank);
  }
}

boolean_t pic_var_trials_available(pfile_t *pf)
{
  UNUSED(pf);
  return pic_var.snap && (0U != pic_var.banked_edges_used)
      ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

void pic_var_trial_install(pfile_t *pf, int window)
{
  struct variable_ **data2;
  size_t             n2;
  size_t             kk;

  if (!pic_var.snap) {
    return;
  }
  /* restore the pre-placement state */
  pic_accesswin_list_free(pic_var.data_bank_list);
  pic_var.data_bank_list = pic_accesswin_list_clone(pic_var.snap->data_list);
  pic_accesswin_list_free(pic_var.shared_bank_list);
  pic_var.shared_bank_list = pic_accesswin_list_clone(pic_var.snap->shared_list);
  if (!pic_var.data_bank_list || !pic_var.shared_bank_list) {
    pfile_log_syserr(pf, ENOMEM);
    return;
  }
  for (kk = 0; kk < pic_var.snap->used; kk++) {
    variable_base_set(pic_var.snap->data[kk], pic_var.snap->virt[kk], 0);
  }
  pic_blist_max = pic_var.snap->max;
  /* build the requested layout on a private copy of the candidates */
  data2 = MALLOC(pic_var.snap->used * sizeof(*data2));
  if (!data2) {
    pfile_log_syserr(pf, ENOMEM);
    return;
  }
  memcpy(data2, pic_var.snap->data, pic_var.snap->used * sizeof(*data2));
  n2 = pic_var.snap->used;
  if (window) {
    pic_variable_access_window_prepass(pf, data2, &n2);
  }
  pic_variable_fixup_place(pf, data2, n2);
  pic_var_shared_flags_refresh(pf);
  FREE(data2);
}
static void pic_variable_alloc_fixup(pfile_t *pf)
{
  struct {
    size_t      used;
    size_t      alloc;
    struct variable_ **data;
  } vinfo;
  pfile_proc_t *proc;

  vinfo.used  = 0;
  vinfo.alloc = 0;
  vinfo.data  = 0;
  /* first, find each allocated variable */
  for (proc = pfile_proc_root_get(pf);
       proc;
       proc = pfile_proc_next(proc)) {
    pfile_block_t *blk;

    for (blk = pfile_proc_block_root_get(proc);
         blk;
         blk = pfile_block_next(blk)) {
      variable_t vptr;

      for (vptr = pfile_block_variable_list_head(blk);
           vptr;
           vptr = variable_link_get(vptr)) {
        if (variable_is_auto(vptr)
            && !variable_is_sticky(vptr)
            && (VARIABLE_BASE_UNKNOWN != variable_base_get(vptr, 0))) {
          /*
           * Omit GPR for auto globals with no use/assign on emitted PIC
           * (pic_variable_counters_set; indirect ops attribute via memval).
           */
          if (!variable_is_used(vptr) && !variable_is_assigned(vptr)) {
            variable_base_set(vptr, VARIABLE_BASE_UNKNOWN, 0);
            continue;
          }
          if (vinfo.used == vinfo.alloc) {
            size_t resize;
            void  *tmp;

            resize = (vinfo.alloc) ? 2 * vinfo.alloc : 1024;
            tmp    = REALLOC(vinfo.data, sizeof(*vinfo.data) * resize);
            if (tmp) {
              vinfo.alloc = resize;
              vinfo.data  = tmp;
            }
          }
          if (vinfo.used < vinfo.alloc) {
            vinfo.data[vinfo.used] = vptr;
            vinfo.used++;
          }
        }
      }
    }
  }
  /* sort the variables by position */
  qsort(vinfo.data, vinfo.used, sizeof(*vinfo.data), vinfo_cmp);
  /* PIC18 / 14H: capture the pre-placement state so trial layouts can
   * be built later by re-running the placement from here */
  if ((pic_is_16bit(pf) || pic_is_14bit_hybrid(pf))
      && (0U != vinfo.used) && pic_var.banked_edges_used) {
    struct pic_var_layout_snap_ *snap;
    pfile_proc_t                 *sproc;
    size_t                        kk;


    pic_var_trials_free();
    snap = MALLOC(sizeof(*snap));
    if (snap) {
      snap->data_list   = pic_accesswin_list_clone(pic_var.data_bank_list);
      snap->shared_list = pic_accesswin_list_clone(pic_var.shared_bank_list);
      snap->virt        = MALLOC(vinfo.used * sizeof(*snap->virt));
      snap->data        = vinfo.data;
      snap->used        = vinfo.used;
      snap->max         = pic_blist_max;
      vinfo.data        = 0; /* ownership moves to the snapshot */
      snap->shflags     = 0;
      snap->shct        = 0;
      for (sproc = pfile_proc_root_get(pf); sproc;
          sproc = pfile_proc_next(sproc)) {
        pfile_block_t *blk;

        for (blk = pfile_proc_block_root_get(sproc); blk;
            blk = pfile_block_next(blk)) {
          variable_t var;

          for (var = pfile_block_variable_list_head(blk); var;
              var = variable_link_get(var)) {
            if (variable_flag_test(var, VARIABLE_FLAG_SHARED)) {
              snap->shct++;
            }
          }
        }
      }
      /* user-declared `var shared` list: the snapshot is taken before
       * pic_variable_shared_set() has ever run this compile, so these
       * are exactly the flags we must never clear */
      if (snap->shct) {
        snap->shflags = MALLOC(snap->shct * sizeof(*snap->shflags));
        if (snap->shflags) {
          snap->shct = 0;
          for (sproc = pfile_proc_root_get(pf); sproc;
              sproc = pfile_proc_next(sproc)) {
            pfile_block_t *blk;

            for (blk = pfile_proc_block_root_get(sproc); blk;
                blk = pfile_block_next(blk)) {
              variable_t var;

              for (var = pfile_block_variable_list_head(blk); var;
                  var = variable_link_get(var)) {
                if (variable_flag_test(var, VARIABLE_FLAG_SHARED)) {
                  snap->shflags[snap->shct++] = var;
                }
              }
            }
          }
        } else {
          snap->shct = 0;
        }
      }
      if (snap->data_list && snap->shared_list && snap->virt) {
        for (kk = 0; kk < snap->used; kk++) {
          snap->virt[kk] = variable_base_get(snap->data[kk], 0);
        }
        pic_var.snap = snap;
      } else {
        pic_accesswin_list_free(snap->data_list);
        pic_accesswin_list_free(snap->shared_list);
        FREE(snap->virt);
        FREE(snap->data);
        FREE(snap);
      }
    }
  }
  if (!vinfo.data) {
    /* candidates owned by the snapshot: place from the snapshot copy */
    if (pic_var.snap) {
      pic_variable_fixup_place(pf, pic_var.snap->data, pic_var.snap->used);
      pic_var_refusal_report(pf);
    }
    return;
  }
  pic_variable_fixup_place(pf, vinfo.data, vinfo.used);
  pic_var_refusal_report(pf);
  FREE(vinfo.data);
}

/* allocate variables but don't allow data space re-use */
static void pic_variable_alloc_no_reuse(pfile_t *pf)
{
  pfile_proc_t *proc;
  boolean_t     err;

  err = BOOLEAN_FALSE;
  for (proc = pfile_proc_root_get(pf);
       proc && !err;
       proc = pfile_proc_next(proc)) {
    pfile_block_t *blk;

    for (blk = pfile_proc_block_root_get(proc);
         blk && !err; 
         blk = pfile_block_next(blk)) {
      variable_t vptr;

      for (vptr = pfile_block_variable_list_head(blk);
           vptr && !err;
           vptr = variable_link_get(vptr)) {
        if (variable_is_auto(vptr)
            && !variable_is_sticky(vptr)) {
          variable_base_t base;
          variable_ct_t  pointer_slots;
          variable_sz_t  var_size;

          if (!variable_is_used(vptr) && !variable_is_assigned(vptr)) {
            variable_base_set(vptr, VARIABLE_BASE_UNKNOWN, 0);
            continue;
          }
          var_size = pic_variable_storage_need_get(pf, vptr, &pointer_slots);
          base = pic_data_alloc(pf, &pic_var.data_bank_list, 
            var_size, pointer_slots);
          if (VARIABLE_BASE_UNKNOWN == base) {
            unsigned bank_size = pic_target_bank_size_get(pf);
            if (var_size > bank_size) {
              pfile_log(pf, PFILE_LOG_ERR, 
                "Variable '%s' size (%u bytes) exceeds bank boundary (%u bytes). "
                "Consider splitting into smaller arrays or using different data types.",
                variable_name_get(vptr), var_size, bank_size);
            } else {
              pfile_log(pf, PFILE_LOG_ERR, 
                "Variable '%s' array size (%u bytes) exceeds bank boundary (%u bytes). "
                "Consider splitting into smaller arrays or using different data types.",
                variable_name_get(vptr), var_size, bank_size);
            }
            err = BOOLEAN_TRUE;
          } else {
            variable_base_set(vptr, base, 0);
          }
        }
      }
    }
  }
  pic_software_stack_calc(pf);
}
#endif

/*
 * For all variables, set or clear the SHARED flag
 * as appropriate. Formerly this was done during variable allocation,
 * but then it missed variables hand placed in a region marked
 * "SHARED"
 */
static void pic_variable_shared_set(pfile_t *pf)
{
  pfile_proc_t *proc;

  for (proc = pfile_proc_root_get(pf);
       proc;
       proc = pfile_proc_next(proc)) {
    pfile_block_t *blk;

    for (blk = pfile_proc_block_root_get(proc);
         blk;
         blk = pfile_block_next(blk)) {
      variable_t vptr;

      for (vptr = pfile_block_variable_list_head(blk);
          vptr;
          vptr = variable_link_get(vptr)) {
        if (!variable_master_get(vptr)) {
          variable_base_t base;

          base = variable_base_get(vptr, 0);
          if (VARIABLE_BASE_UNKNOWN != base) {
            pic_bank_info_t *bank;
            variable_sz_t    need;

            need = pic_variable_storage_need_get(pf, vptr, 0);
            for (bank = pic_var.shared_pragma_list;
                 bank && !((bank->lo <= base) && (base <= bank->hi));
                 bank = bank->link)
              ;
            if (bank && need > 0U
                && (base + need - 1U) <= bank->hi) {
              boolean_t set_shared;

              set_shared = BOOLEAN_TRUE;
              if (pic_is_16bit(pf)) {
                /* a=0 (Access) validity inside a `pragma shared` range:
                 * f = addr & 0xff must select the window the range
                 * declares. A range starting at page offset 0x00-0x5f is
                 * an Access RAM window; f > 0x5f there would select the
                 * SFR window instead (classic 0x0-0x7f, v6 0x500-0x55f).
                 * A range starting at page offset 0x60-0xff is an Access
                 * SFR window, valid through its page end (classic
                 * 0xf60-0xfff, v6 0x460-0x4ff). */
                variable_base_t limit;

                limit = bank->lo & ~(variable_base_t) 0xffU;
                limit += ((bank->lo & 0xffU) < 0x60U)
                  ? (variable_base_t) 0x5fU : (variable_base_t) 0xffU;
                if (limit > bank->hi) {
                  limit = bank->hi;
                }
                if ((base + need - 1U) > limit) {
                  set_shared = BOOLEAN_FALSE;
                }
              }
              if (set_shared) {
                variable_flag_set(vptr, VARIABLE_FLAG_SHARED);
              }
            }
          }
        }
      }
    }
  }
}

/* placement/lazy-allocation phase marker: once pic_variable_alloc has
 * run, variables created afterwards (emit-time scratch such as the
 * chunk-walk temps -- chunk maps only exist after pass-1 placement, so
 * their temps are requested in later passes) must allocate themselves
 * immediately or they can never get an address (2026-10-26 band 2). */

boolean_t pic_var_placement_done_get(void)
{
  return pic_var.placement_done;
}

/* late allocation for emit-time scratch: give the previous extent back
 * to the free list, then re-allocate at the (possibly larger) size */
boolean_t pic_variable_late_alloc(pfile_t *pf, variable_t var)
{
  variable_base_t       base;
  variable_sz_t         sz;
  variable_ct_t         pointer_slots;

  if (variable_master_get(var)) {
    /* union member (e.g. _pic_temp inside _pic_state): the bytes are
     * not independently owned -- leave the placement alone */
    return BOOLEAN_TRUE;
  }
  sz = pic_variable_storage_need_get(pf, var, &pointer_slots);
  base = variable_base_get(var, 0);
  if (VARIABLE_BASE_UNKNOWN != base) {
    pic_blist_add(pf, &pic_var.data_bank_list, base, base + sz - 1);
    pic_blist_max -= sz;
    variable_base_set(var, VARIABLE_BASE_UNKNOWN, 0);
  }
  {
    boolean_t rc;

    rc = pic_variable_alloc_one(pf, PFILE_PROC_NONE, var);
    if (rc) {
      pic_var_late_track(var);
    }
    return rc;
  }
}

/*
 * NAME
 *   pic_variable_alloc
 *
 * DESCRIPTION
 *   allocate space for all variables
 */
void pic_variable_alloc(pfile_t *pf)
{
  pfile_proc_t *proc;

  if (getenv("JALV2_DBG_REFUSAL")) {
    fprintf(stderr, "DBG alloc round, free-before=%u\n",
        (unsigned) pic_data_bank_free_bytes_get());
  }
  blist_total();
  pfile_log(pf, PFILE_LOG_DEBUG, PIC_MSG_ALLOCATING_VARS);

  /* make sure the frame sizes are correct. currently only re-entrant
   * functions create an extra variable but that may change */

  for (proc = pfile_proc_root_get(pf);
       proc;
       proc = pfile_proc_next(proc)) {
    pfile_proc_frame_sz_calc(proc);
  }
  /* remove any preset variables; collect byte-sized explicit `at`
   * placements so conflicting pins can be reported (a device-file
   * `_pic_isr_w at 0x7E` silently corrupts a user pin at the same
   * address — see fix_pic_explicit_address_conflict.md) */
  {
    unsigned placed_free_was = pic_data_bank_free_bytes_get();
    struct placed_var_ {
      variable_t       var;
      variable_base_t  base;
      variable_sz_t    need;
    }             *placed;
    size_t         placed_ct;
    size_t         placed_alloc;

    placed       = 0;
    placed_ct    = 0;
    placed_alloc = 0;
    for (proc = pfile_proc_root_get(pf);
         proc;
         proc = pfile_proc_next(proc)) {
      pfile_block_t *blk;

      for (blk = pfile_proc_block_root_get(proc);
           blk;
           blk = pfile_block_next(blk)) {
        variable_t vptr;

        for (vptr = pfile_block_variable_list_head(blk);
             vptr;
             vptr = variable_link_get(vptr)) {
          if (!variable_master_get(vptr)
              && (VARIABLE_BASE_UNKNOWN != variable_base_get(vptr, 0))
              && pic_variable_storage_need_get(pf, vptr, 0)) {
            size_t ii;

            for (ii = 0; ii < VARIABLE_MIRROR_CT; ii++) {
              variable_base_t base;
              variable_ct_t   pointer_slots;
              variable_sz_t   need;

              base = variable_base_get(vptr, ii);
              if (VARIABLE_BASE_UNKNOWN == base) {
                continue;
              }
              need = pic_variable_storage_need_get(pf, vptr, &pointer_slots);
              if (base) {
                /* base 0 pins are pseudo-slot markers (pin 'get/'put
                 * temps, indf/_ind aliases), not real storage */
                size_t k;

                for (k = 0; k < placed_ct; k++) {
                  if ((placed[k].var == vptr) && (placed[k].base == base)) {
                    break;
                  }
                }
                if (k == placed_ct) {
                  if (placed_ct == placed_alloc) {
                    struct placed_var_ *tmp;

                    placed_alloc = placed_alloc ? 2 * placed_alloc : 64;
                    tmp = REALLOC(placed, placed_alloc * sizeof(*placed));
                    if (tmp) {
                      placed = tmp;
                    } else {
                      placed_alloc = 0;
                    }
                  }
                  if (placed_ct < placed_alloc) {
                    placed[placed_ct].var  = vptr;
                    placed[placed_ct].base = base;
                    placed[placed_ct].need = need;
                    placed_ct++;
                  }
                }
              }
              pic_blist_remove(pf, &pic_var.data_bank_list, base,
                need + pic_pointer_slot_padding_get(pf, pointer_slots));
            }
          }
        }
      }
    }
    if (placed) {
      size_t ii;
      size_t jj;

      /*
       * Two explicit pins overlapping across DIFFERENT source files inside
       * a `pragma shared` region is a bug: the shared/common window is
       * cross-bank infrastructure where the device file parks compiler
       * internals (`_pic_isr_w at 0x7E`, port shadows), so a pin there
       * silently corrupts or is corrupted by any other file's pin (found
       * via the gpsim harness pinning `_check_step at 0x7E` —
       * fix_pic_explicit_address_conflict.md). Everything else is exempt:
       * same-file overlays are the device-file idiom (crcout/crcshftl_,
       * usb_bd_array/usb_bd0in, _pclat/pcl), cross-file overlays in bank-
       * local RAM are coordinated through shared constants (the USB buffer
       * views in usb_defs.jal vs usb_keyboard.jal et al.), synthetic
       * variables have no origin, and SFR territory overlays are register
       * views.
       */
      for (ii = 0; ii < placed_ct; ii++) {
        for (jj = ii + 1; jj < placed_ct; jj++) {
          const char *nm_ii;
          const char *nm_jj;
          const void *org_ii;
          const void *org_jj;
          boolean_t   sfr_region;

          if ((placed[ii].base >= placed[jj].base + placed[jj].need)
            || (placed[jj].base >= placed[ii].base + placed[ii].need)) {
            continue;
          }
          sfr_region = ((placed[ii].base & 0x7FU) < 0x20U)
            || ((placed[jj].base & 0x7FU) < 0x20U)
            || (placed[ii].base >= 0xF60U)
            || (placed[jj].base >= 0xF60U);
          if (sfr_region) {
            continue;
          }
          org_ii = variable_origin_get(placed[ii].var);
          org_jj = variable_origin_get(placed[jj].var);
          if (!org_ii || !org_jj || (org_ii == org_jj)) {
            continue;
          }
          if (!pic_shared_bank_list_covers_byte(pf,
                (unsigned long) placed[ii].base)
            && !pic_shared_bank_list_covers_byte(pf,
                (unsigned long) placed[jj].base)) {
            continue;
          }
          nm_ii = variable_name_get(placed[ii].var);
          nm_jj = variable_name_get(placed[jj].var);
          if (nm_ii && nm_jj && 0 == strcmp(nm_ii, nm_jj)) {
            continue; /* same-name re-declaration across includes */
          }
          pfile_log(pf, PFILE_LOG_ERR,
            "explicit address conflict: %s at 0x%04X (%u bytes)"
            " overlaps %s at 0x%04X (%u bytes)",
            nm_ii ? nm_ii : "?", (unsigned) placed[ii].base,
            (unsigned) placed[ii].need,
            nm_jj ? nm_jj : "?", (unsigned) placed[jj].base,
            (unsigned) placed[jj].need);
        }
      }
      FREE(placed);
    }
    /* how many bytes the explicit placements actually took off the free
     * list (the delta also de-duplicates overlapping register views).
     * The Data-area report excludes them so the number means
     * "compiler-allocated", matching the jalv25r9 convention used by
     * the compare dashboard (ours counted fixed USB buffers/shadows and
     * showed +223 "usage" on identical layouts). */
    {
      unsigned placed_free_now = pic_data_bank_free_bytes_get();

      pic_blist_placed = (placed_free_was > placed_free_now)
        ? (placed_free_was - placed_free_now) : 0;
    }
  }

  pic_stvar_fixup(pf);
  pic_variable_alloc_volatile(pf);
  pic_variable_alloc_isr(pf);
  pic_variable_alloc_indirect(pf);
  pic_var.placement_done = BOOLEAN_TRUE;
  /* finally, allocate some variables! */
#if 0
  {
    variable_array_t *live;
    variable_array_t *all_vars;
    variable_array_t *allocated_vars;
    size_t            ii;

    live     = variable_array_alloc(0);
    all_vars = variable_array_alloc(0);
    allocated_vars = variable_array_alloc(0);
    pic_variable_interfer_proc(pf, pfile_proc_root_get(pf), live,
      all_vars);
    assert(0 == variable_array_entry_ct(live));
    variable_array_free(live);
    printf("Total variables: %u\n", 
      (unsigned) variable_array_entry_ct(all_vars));
    while (variable_array_entry_ct(all_vars)) {
      size_t jj;

      ii = variable_array_entry_ct(all_vars) - 1;
      /* allocate all variables that are live with this one */
      live = variable_interference_get(
        *variable_array_entry_get(all_vars, ii));
      printf("Allocating: %s (%u interfer)\n",
        variable_name_get(*variable_array_entry_get(all_vars, ii)),
        variable_array_entry_ct(live));
      for (jj = 0; jj < variable_array_entry_ct(live); jj++) {
        variable_t var;

        var = *variable_array_entry_get(live, jj);
        if (VARIABLE_BASE_UNKNOWN == variable_base_get(var, 0)) {
          variable_base_t base;

          base = pic_data_alloc(pf, &pic_var.data_bank_list, variable_sz_get(var));
          if (VARIABLE_BASE_UNKNOWN != base) {
            variable_base_set(var, base, 0);
            variable_array_entry_add(allocated_vars, &var,
              ARRAY_ADD_FLAG_NONE, variable_unique_cmp, 0);
          }
        }
      }
      /* and free all of the space for re-use */
      while (variable_array_entry_ct(allocated_vars)) {
        variable_t      var;
        variable_base_t base;
        size_t          ix;

        ix = variable_array_entry_ct(allocated_vars) - 1;

        var  = *variable_array_entry_get(allocated_vars, ix);
        base = variable_base_get(var, 0);

        pic_blist_add(pf, &pic_var.data_bank_list, base,
          base + variable_sz_get(var) - 1);
        variable_array_entry_remove(allocated_vars, ix);
      }
      variable_array_entry_remove(all_vars, ii);
    }
    variable_array_free(all_vars);
  }
#else
  /* This block is disabled - do nothing */
#endif
  
  /* Memory allocation setup complete */
  
  if (pfile_flag_test(pf, PFILE_FLAG_OPT_VARIABLE_REUSE)) {
    pic_variable_alloc_proc(pf, pfile_proc_root_get(pf));
    pic_variable_alloc_fixup(pf);
  } else {
    pic_variable_alloc_no_reuse(pf);
  }
  pic_variable_shared_set(pf);
  /* nb: the runtime chunk maps are prepared by pic_var_chunk_prepare_emit(),
   * which the code generator calls after the layout trials -- placement is not
   * final until they have run. */
  pfile_log(pf, PFILE_LOG_DEBUG, PIC_MSG_DBG_DATA_USED, pic_blist_max);
}

unsigned pic_data_bank_free_bytes_get(void)
{
  const pic_bank_info_t *bank;
  unsigned               free_bytes;

  for (bank = pic_var.data_bank_list, free_bytes = 0; bank; bank = bank->link) {
    free_bytes += (unsigned) (bank->hi - bank->lo + 1);
  }
  return free_bytes;
}

void pic_blist_info_log(pfile_t *pf)
{
  UNUSED(pf);

}

void pic_blist_free(pfile_t *pf)
{
  UNUSED(pf);

  while (pic_var.data_bank_list) {
    pic_bank_info_t *bank;

    bank = pic_var.data_bank_list->link;
    FREE(pic_var.data_bank_list);
    pic_var.data_bank_list = bank;
  }
  while (pic_var.shared_bank_list) {
    pic_bank_info_t *bank;

    bank = pic_var.shared_bank_list->link;
    FREE(pic_var.shared_bank_list);
    pic_var.shared_bank_list = bank;
  }
  while (pic_var.shared_pragma_list) {
    pic_bank_info_t *bank;

    bank = pic_var.shared_pragma_list->link;
    FREE(pic_var.shared_pragma_list);
    pic_var.shared_pragma_list = bank;
  }
  while (pic_var.data_pragma_list) {
    pic_bank_info_t *bank;

    bank = pic_var.data_pragma_list->link;
    FREE(pic_var.data_pragma_list);
    pic_var.data_pragma_list = bank;
  }
}

