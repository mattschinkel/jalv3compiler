/************************************************************
 **
 ** pic_mem.h : the placement model -- one request, one result
 **
 ** Two descriptions, two jobs:
 **
 **   pic_mem_request_t  WHAT an object needs, before placement:
 **                      size, stride, pointer slots, region, contiguity,
 **                      chunkability.  Built from a variable (or a group)
 **                      by the adapter, consumed by the search.
 **
 **   pic_mem_object_t   WHERE it landed, after placement: the segments or
 **                      the one base.  A borrowed view of the chunk map;
 **                      no lifetime of its own.
 **
 ** Placement reads only the request (plus the map).  The variable list in
 ** the request is the commit target: it is where the resulting base and
 ** chunk map are written, and where the diagnostics get a name -- never a
 ** source of placement facts.
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#ifndef pic_mem_h__
#define pic_mem_h__

#include "../libcore/pfile.h"
#include "../libcore/variable.h"
#include "../libcore/pf_proc.h"

/* U1b: the allocator is region-aware.  The free lists are per-region (the
 * shared window overlaps the data area by design), so callers ask for a
 * region instead of naming a list. */
typedef enum pic_mem_region_ {
  PIC_MEM_REGION_DATA = 0,
  PIC_MEM_REGION_SHARED
} pic_mem_region_t;

/*
 * U0 of the segmented memory model: ONE description of an object's memory
 * layout.  This is a VIEW, not a registry -- it borrows the chunk map's
 * arrays when the object is segmented and uses an inline single segment
 * otherwise, so it cannot disagree with the allocator and has no lifetime
 * of its own.
 */
typedef struct pic_mem_object_ {
  variable_t             var;
  variable_sz_t          size;      /* def size: bytes (bits for bit arrays) */
  variable_sz_t          stride;    /* element size in bytes, 1 for scalars */
  variable_ct_t          seg_ct;    /* number of physical segments */
  variable_base_t        one_base;  /* contiguous case: segment 0 base */
  variable_sz_t          one_len;   /* contiguous case: segment 0 length */
  const variable_base_t *seg_base;  /* segmented case: borrowed from the map */
  const variable_sz_t   *seg_len;
} pic_mem_object_t;

typedef enum {
  PIC_MEM_PLACE_SINGLE = 0, /* one variable: one window (defer optional)   */
  PIC_MEM_PLACE_FRAME,      /* one contiguous run of `need` bytes          */
  PIC_MEM_PLACE_GROUP       /* alias group: window -> shared -> segmented  */
} pic_mem_place_policy_t;

typedef enum {
  PIC_MEM_OK = 0,
  PIC_MEM_DEFERRED,
  PIC_MEM_REFUSED
} pic_mem_status_t;

/* WHAT the object needs.  Every search decision reads these fields; none of
 * them is re-derived from the variable at placement time. */
typedef struct pic_mem_request_ {
  pic_mem_place_policy_t  policy;
  pic_mem_region_t        region;
  variable_sz_t           need;       /* bytes the object occupies          */
  variable_sz_t           padded;     /* need + pointer-slot padding        */
  variable_ct_t           slots;      /* pointer slots (padding source)     */
  variable_sz_t           stride;     /* element bytes; 1 for scalars/bit   */
  boolean_t               chunkable;  /* array or bit array: may segment    */
  boolean_t               bit_array;  /* packed bits: always walkable       */
  boolean_t               linear_only;/* escaped/pointer-bound: one run     */
  boolean_t               pointer_target; /* for the refusal record        */
  variable_base_t         virt;       /* group key (virtual base)           */
  const char             *name;       /* diagnostics                        */
  pfile_proc_t           *proc;       /* diagnostics (frame subject)        */
  boolean_t               may_defer;  /* SINGLE: defer when reuse is on     */
  boolean_t               report;     /* log the refusal here               */
  /* commit target: SINGLE/FRAME one variable, GROUP the alias group */
  variable_t              var;
  struct variable_      **members;
  size_t                  member_ct;  /* group end index                    */
  size_t                 *placed;     /* group: in/out member index         */
  variable_base_t        *base;       /* out: FRAME base                    */
} pic_mem_request_t;

/* Build a request for one variable (SINGLE). */
void pic_mem_request_var(pfile_t *pf, variable_t var,
    pic_mem_place_policy_t policy, pic_mem_request_t *req);

/* Build a request for one contiguous frame (FRAME): a raw byte run with no
 * variable -- the base is written back through `req->base`. */
void pic_mem_request_frame(pfile_proc_t *proc, variable_sz_t need,
    boolean_t report, variable_base_t *base, pic_mem_request_t *req);

/* Build a request for one alias group (GROUP): every member shares `virt`;
 * the facts are the group's (largest member) need/padding and the HEAD's
 * stride/chunkability, exactly as the placement has always read them. */
void pic_mem_request_group(pfile_t *pf, struct variable_ **members,
    size_t *pii, size_t jj, pic_mem_request_t *req);

/* ONE placement entry: apply the policy, hand out the base(s), log the one
 * refusal.  No code path outside this function may derive a base. */
pic_mem_status_t pic_mem_place(pfile_t *pf, const pic_mem_request_t *req);

#endif /* pic_mem_h__ */
