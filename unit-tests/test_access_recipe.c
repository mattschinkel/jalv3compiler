/* E3: the target-neutral access-recipe dispatcher.
 * Pure libcore test: no back end is linked; the facts here are made-up
 * bits, which is the point -- libcore compares them without knowing them. */
#include <assert.h>
#include <stdio.h>

#include "access_recipe.h"

#define FAC_A 0x0001u
#define FAC_B 0x0002u
#define FAC_C 0x0004u
#define OP1   1u
#define OP2   2u

static unsigned calls;
static unsigned seen_facts;
static unsigned seen_operation;

static void marker_run(const access_recipe_key_t *key, void *ctx)
{
  calls++;
  seen_facts = key->facts;
  seen_operation = key->operation;
  (void) ctx;
}

static void test_find(void)
{
  static const access_recipe_t table[] = {
    /* only A and B are compared; C is ignored by this row */
    { ACCESS_RECIPE_KIND_ANY, OP1, FAC_A | FAC_B, FAC_A | FAC_B, marker_run }
  };
  access_recipe_key_t key;
  const access_recipe_t *row;

  key.kind      = 7;
  key.operation = OP1;
  key.facts     = FAC_A | FAC_B | FAC_C;
  row = access_recipe_find(table, 1, &key);
  assert(row == &table[0]);

  key.facts = FAC_A;           /* B is required: no match */
  assert(0 == access_recipe_find(table, 1, &key));

  key.facts = FAC_A | FAC_B;
  key.operation = OP2;         /* operation must be equal */
  assert(0 == access_recipe_find(table, 1, &key));
}

static void test_kind_and_order(void)
{
  static const access_recipe_t table[] = {
    { ACCESS_RECIPE_KIND_ANY, OP1, FAC_A, FAC_A, marker_run },
    { 7, OP1, 0u, FAC_A | FAC_B, marker_run }
  };
  access_recipe_key_t key;
  const access_recipe_t *row;

  /* a key for kind 7 with A matches the ANY row first (table order) */
  key.kind = 7;
  key.operation = OP1;
  key.facts = FAC_A;
  row = access_recipe_find(table, 2, &key);
  assert(row == &table[0]);

  /* a key for kind 8 with only B matches no row (the ANY row requires A,
   * the specific row is kind 7) */
  key.kind = 8;
  key.facts = FAC_B;
  row = access_recipe_find(table, 2, &key);
  assert(0 == row);

  /* a key for kind 7 with no facts matches the specific row */
  key.kind = 7;
  key.facts = 0;
  row = access_recipe_find(table, 2, &key);
  assert(row == &table[1]);
}

static void test_dispatch(void)
{
  static const access_recipe_t table[] = {
    /* a total row: facts 0 compared against mask 0 matches every key */
    { ACCESS_RECIPE_KIND_ANY, OP1, 0u, 0u, marker_run }
  };
  access_recipe_key_t key;

  calls = 0;
  seen_facts = 0;
  key.kind = ACCESS_RECIPE_KIND_ANY;
  key.operation = OP1;
  key.facts = FAC_C;
  assert(ACCESS_RECIPE_OK
      == access_recipe_dispatch(table, 1, &key, (void *) 0x1234));
  assert(1 == calls);
  assert(FAC_C == seen_facts);
  assert(OP1 == seen_operation);

  key.operation = OP2;
  assert(ACCESS_RECIPE_MISS
      == access_recipe_dispatch(table, 1, &key, (void *) 0));
  assert(1 == calls);          /* the body did not run */
}

static void test_consistency(void)
{
  access_recipe_t row;

  row.kind = 0;
  row.operation = OP1;
  row.facts = FAC_A;
  row.facts_mask = FAC_A;
  row.run = 0;
  assert(BOOLEAN_TRUE == access_recipe_row_consistent(&row));

  /* requires B but does not compare it: exactly the dominant-bit mask
   * mistake the classic table had */
  row.facts = FAC_A | FAC_B;
  assert(BOOLEAN_FALSE == access_recipe_row_consistent(&row));

  assert(BOOLEAN_FALSE == access_recipe_row_consistent((void *) 0));
}

int main(void)
{
  test_find();
  test_kind_and_order();
  test_dispatch();
  test_consistency();
  printf("access recipe: all assertions held\n");
  return 0;
}
