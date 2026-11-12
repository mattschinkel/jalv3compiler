/****************************************************************
 **
 ** access_recipe.h : the target-neutral access-recipe dispatcher
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ** E3: one dispatcher every back end can share.  A recipe row is
 ** (kind, operation, facts, facts_mask) plus a run body; the facts
 ** are OPAQUE to this module -- each back end owns its bit meanings
 ** and its table.  The classic table stays in libpic12; PIC32 and
 ** ESP32 own theirs.
 **
 ** `kind` is likewise opaque: the target-independent class enum
 ** (access_class_t) is the shared vocabulary, and a back end maps
 ** its own kind values onto it.
 **
 ****************************************************************/
#ifndef access_recipe_h__
#define access_recipe_h__

#include <stddef.h>

#include "../libutils/types.h"

/* S4/Q1: the neutral classes.  Values are fixed; a back end keeps its
 * own kind numbering and maps to these. */
typedef enum access_class_ {
  ACCESS_CLASS_DIRECT = 0,      /* static address inside the object        */
  ACCESS_CLASS_LINEAR = 1,      /* address = pointer value + offset        */
  ACCESS_CLASS_RESOLVE = 2,     /* address computed from the value         */
  ACCESS_CLASS_WALK = 3,        /* walk a (baked or runtime) chunk map     */
  ACCESS_CLASS_TAG_DISPATCH = 4,/* dispatch on a tag at run time           */
  ACCESS_CLASS_REFUSE = 5       /* no legal access                         */
} access_class_t;

/* a row may match any kind (the decision is about the facts) */
#define ACCESS_RECIPE_KIND_ANY 0xFFFFFFFFu

typedef struct access_recipe_key_ {
  unsigned kind;       /* back-end kind id, compared as opaque data */
  unsigned operation;  /* back-end operation id                     */
  unsigned facts;      /* back-end fact bits (opaque here)          */
} access_recipe_key_t;

typedef struct access_recipe_ {
  unsigned kind;
  unsigned operation;
  unsigned facts;       /* the required bits                        */
  unsigned facts_mask;  /* the bits compared (others are ignored)   */
  /* Q3: the body sees the matched key and the caller's context */
  void (*run)(const access_recipe_key_t *key, void *ctx);
} access_recipe_t;

/* Q3: a miss returns a status the caller makes fatal (classic) or
 * falls back from (a back end may keep a defensive default). */
typedef enum access_recipe_rc_ {
  ACCESS_RECIPE_OK = 0,
  ACCESS_RECIPE_MISS
} access_recipe_rc_t;

/* first matching row in table order, or 0 */
const access_recipe_t *access_recipe_find(const access_recipe_t *table,
    size_t count, const access_recipe_key_t *key);
/* find, then run the row's body; MISS when no row matches */
access_recipe_rc_t access_recipe_dispatch(const access_recipe_t *table,
    size_t count, const access_recipe_key_t *key, void *ctx);
/* a row may only require bits it compares */
boolean_t access_recipe_row_consistent(const access_recipe_t *row);

#endif /* access_recipe_h__ */
