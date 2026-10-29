/************************************************************
 **
 ** hash.h : chained hash table declarations
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#ifndef hash_h__
#define hash_h__

#include <stddef.h>

#include "types.h"

/*
 * Generic chained hash: values are caller-owned pointers; the table
 * owns only its entries.  Keys are either stored by pointer (key_sz 0)
 * or copied into the entry (key_sz bytes, for stack keys such as
 * constant-value lookups).  Pointer keys MUST reference storage the
 * caller keeps alive for the life of the table: a rehash re-reads
 * every stored key, so a stale key pointer scatters its entry into
 * the wrong bucket and later lookups miss.
 */
typedef struct hash_ *hash_t;

typedef unsigned long (*hash_hash_cb_t)(const void *key);
typedef boolean_t     (*hash_eq_cb_t)(const void *key, void *value);

hash_t    hash_alloc(hash_hash_cb_t hf, hash_eq_cb_t eqf,
              size_t key_sz);
void      hash_free(hash_t hsh);

/* FALSE only on out-of-memory; the table is unusable afterwards and
 * every lookup must fall back to the caller's linear path */
boolean_t hash_add(hash_t hsh, const void *key, void *value);

/* returns the first value whose key matches (insertion order within
 * a key), or 0; no locking is performed */
void     *hash_find(hash_t hsh, const void *key);

void      hash_remove(hash_t hsh, const void *key, void *value);

#endif /* hash_h__ */
