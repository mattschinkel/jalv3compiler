/************************************************************
 **
 ** mem.c : checked allocation
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 ** All rights reserved
 **
 ************************************************************/
#include <stdlib.h>
#include <string.h>
#include "mem.h"

/*
 * Checked-allocation policy.  MALLOC/CALLOC/REALLOC map here.
 *
 * The handler is installed by pfile_open() for the duration of a compile.
 * With one installed, an allocation failure can never return NULL to a
 * caller that would dereference it: it reports once and exits non-zero.
 * That is deliberate -- a half-built IR has no safe continuation, and the
 * alternative (per-site checks) is exactly how a NULL deref survived at
 * 7.5 MB in one build and crashed the CI runner while passing locally.
 *
 * The old MEMDEBUG poison/statistics allocator (my_malloc/my_free/...) lived
 * here but was unreachable: mem.h undefined MEMDEBUG, so MALLOC never mapped
 * to it and nothing called it.  It was removed 2026-11-02; git holds it.
 */
static void (*oom_handler)(void *);
static void *oom_context;

void
mem_set_oom_handler(void (*fn)(void *), void *context)
{
  oom_handler = fn;
  oom_context = context;
}

static void
mem_oom(void)
{
  if (oom_handler) {
    oom_handler(oom_context);
  }
}

void *
mem_alloc(size_t sz)
{
  void *ptr;

  ptr = malloc(sz);
  if (!ptr && sz) {
    mem_oom();
  }
  return ptr;
}

void *
mem_calloc(size_t ct, size_t sz)
{
  size_t total;
  void  *ptr;

  total = ct * sz;
  ptr   = mem_alloc(total);
  if (ptr) {
    memset(ptr, 0, total);
  }
  return ptr;
}

void *
mem_realloc(void *ptr, size_t sz)
{
  void *new_ptr;

  new_ptr = realloc(ptr, sz);
  if (!new_ptr && sz) {
    mem_oom();
  }
  return new_ptr;
}

char *my_strdup(const char *src)
{
  size_t sz;
  char  *dst;

  sz = strlen(src) + 1;
  dst = MALLOC(sz);
  if (dst) {
    memcpy(dst, src, sz);
  }
  return dst;
}
