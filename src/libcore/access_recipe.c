/****************************************************************
 **
 ** access_recipe.c : the target-neutral access-recipe dispatcher
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ** E3: the matcher and the dispatcher.  Facts are opaque bits: this
 ** module compares them, it never interprets them.
 **
 ****************************************************************/
#include <stddef.h>

#include "access_recipe.h"

const access_recipe_t *access_recipe_find(const access_recipe_t *table,
    size_t count, const access_recipe_key_t *key)
{
  size_t ii;

  if ((0 == table) || (0 == key)) {
    return 0;
  }
  for (ii = 0; ii < count; ii++) {
    const access_recipe_t *row;

    row = &table[ii];
    if (((ACCESS_RECIPE_KIND_ANY == row->kind) || (row->kind == key->kind))
        && (row->operation == key->operation)
        && ((key->facts & row->facts_mask) == row->facts)) {
      return row;
    }
  }
  return 0;
}

access_recipe_rc_t access_recipe_dispatch(const access_recipe_t *table,
    size_t count, const access_recipe_key_t *key, void *ctx)
{
  const access_recipe_t *row;

  row = access_recipe_find(table, count, key);
  if (0 == row) {
    return ACCESS_RECIPE_MISS;
  }
  if (row->run) {
    row->run(key, ctx);
  }
  return ACCESS_RECIPE_OK;
}

boolean_t access_recipe_row_consistent(const access_recipe_t *row)
{
  /* a row may require only bits it compares; requiring an unmasked bit
   * would make the row depend on data it ignores */
  return (row && (0 == (row->facts & ~row->facts_mask)))
    ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}
