/************************************************************
 **
 ** hash.c : chained hash table
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include <string.h>
#include "mem.h"
#include "hash.h"

typedef struct hash_entry_ *hash_entry_t;

struct hash_entry_ {
  hash_entry_t next;
  void        *value;
  /* key: by pointer, or a key_sz copy, stored here */
};

struct hash_ {
  hash_hash_cb_t hf;
  hash_eq_cb_t   eqf;
  size_t         key_sz;
  unsigned long  bucket_ct;
  unsigned long  entry_ct;
  hash_entry_t  *buckets;
  boolean_t      oom;
};

#define HASH_BUCKET_CT_INITIAL 64

static const void *hash_entry_key_get(hash_t hsh, hash_entry_t ent)
{
  return hsh->key_sz
    ? (const void *) (ent + 1)
    : *(const void **) (ent + 1);
}

static boolean_t hash_buckets_alloc(hash_t hsh, unsigned long ct)
{
  hsh->buckets = MALLOC(ct * sizeof(*hsh->buckets));
  if (hsh->buckets) {
    memset(hsh->buckets, 0, ct * sizeof(*hsh->buckets));
    hsh->bucket_ct = ct;
    return BOOLEAN_TRUE;
  }
  return BOOLEAN_FALSE;
}

/* entries append at the tail of their bucket so that, for equal keys,
 * hash_find returns the earliest-added match -- the same order a linear
 * scan over a list fed by the same adds would produce */
static void hash_bucket_append(hash_t hsh, hash_entry_t ent,
    unsigned long idx)
{
  hash_entry_t *pprev;

  for (pprev = &hsh->buckets[idx]; *pprev; pprev = &(*pprev)->next)
    ; /* null body */
  *pprev = ent;
  ent->next = 0;
}

hash_t hash_alloc(hash_hash_cb_t hf, hash_eq_cb_t eqf, size_t key_sz)
{
  hash_t hsh;

  hsh = MALLOC(sizeof(*hsh));
  if (hsh) {
    hsh->hf        = hf;
    hsh->eqf       = eqf;
    hsh->key_sz    = key_sz;
    hsh->entry_ct  = 0;
    hsh->oom       = BOOLEAN_FALSE;
    hsh->bucket_ct = 0;
    hsh->buckets   = 0;
    if (!hash_buckets_alloc(hsh, HASH_BUCKET_CT_INITIAL)) {
      FREE(hsh);
      hsh = 0;
    }
  }
  return hsh;
}

void hash_free(hash_t hsh)
{
  unsigned long ii;

  if (hsh) {
    for (ii = 0; ii < hsh->bucket_ct; ii++) {
      hash_entry_t ent;

      for (ent = hsh->buckets[ii]; ent;) {
        hash_entry_t next;

        next = ent->next;
        FREE(ent);
        ent = next;
      }
    }
    FREE(hsh->buckets);
    FREE(hsh);
  }
}

static void hash_grow(hash_t hsh)
{
  hash_entry_t     *old_buckets;
  unsigned long     old_ct;
  unsigned long     ii;

  old_buckets = hsh->buckets;
  old_ct      = hsh->bucket_ct;
  if (hash_buckets_alloc(hsh, old_ct * 4)) {
    for (ii = 0; ii < old_ct; ii++) {
      hash_entry_t ent;

      for (ent = old_buckets[ii]; ent;) {
        hash_entry_t next;

        next = ent->next;
        hash_bucket_append(hsh, ent,
          hsh->hf(hash_entry_key_get(hsh, ent)) % hsh->bucket_ct);
        ent = next;
      }
    }
    FREE(old_buckets);
  } else {
    /* keep the old buckets; the table degrades to longer chains only */
    hsh->buckets  = old_buckets;
    hsh->bucket_ct = old_ct;
  }
}

boolean_t hash_add(hash_t hsh, const void *key, void *value)
{
  hash_entry_t ent;

  if (!hsh || hsh->oom || !key) {
    return BOOLEAN_FALSE;
  }
  if ((hsh->entry_ct > 2 * hsh->bucket_ct)) {
    hash_grow(hsh);
  }
  ent = MALLOC(sizeof(*ent) + (hsh->key_sz
      ? hsh->key_sz : sizeof(const void *)));
  if (!ent) {
    hsh->oom = BOOLEAN_TRUE;
    return BOOLEAN_FALSE;
  }
  ent->value = value;
  if (hsh->key_sz) {
    memcpy(ent + 1, key, hsh->key_sz);
  } else {
    *(const void **) (ent + 1) = key;
  }
  hash_bucket_append(hsh, ent, hsh->hf(key) % hsh->bucket_ct);
  hsh->entry_ct++;
  return BOOLEAN_TRUE;
}

void *hash_find(hash_t hsh, const void *key)
{
  hash_entry_t ent;
  void        *value;

  value = 0;
  if (hsh && !hsh->oom && key) {
    for (ent = hsh->buckets[hsh->hf(key) % hsh->bucket_ct];
         ent && !value;
         ent = ent->next) {
      if (hsh->eqf(key, ent->value)) {
        value = ent->value;
      }
    }
  }
  return value;
}

void hash_remove(hash_t hsh, const void *key, void *value)
{
  hash_entry_t *pprev;

  if (hsh && !hsh->oom && key) {
    pprev = &hsh->buckets[hsh->hf(key) % hsh->bucket_ct];
    while (*pprev) {
      hash_entry_t ent;

      ent = *pprev;
      if (ent->value == value) {
        *pprev = ent->next;
        FREE(ent);
        hsh->entry_ct--;
        return;
      }
      pprev = &ent->next;
    }
  }
}
