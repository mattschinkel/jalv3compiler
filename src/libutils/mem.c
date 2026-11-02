/************************************************************
 **
 ** mem.c : memory checking routines
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 ** All rights reserved
 **
 ************************************************************/
#include <stdio.h>
#include <string.h>
#include <assert.h>
#if defined (MSDOS) && !defined (__WATCOMC__)
#include <alloc.h>
#endif
#include "mem.h"

#define PREMAGIC 0x13579UL
#define POSTMAGIC 0x97531UL

#if defined (MSDOS) && !defined (__WATCOMC__)
unsigned long farcore_init;
unsigned long farcore_min;
#endif

typedef unsigned long magic_t;

typedef struct mem_block_ {
  magic_t premagic;
  size_t  sz;
  magic_t postmagic;
} mem_block_t;

static struct {
  unsigned long ct;
  unsigned long total;
  unsigned long current;
  unsigned long maximum;
} stats;

static int stat_init;


static void my_cleanup(void)
{
  fprintf(stderr,
         "Memory statistics:\n"
         "  allocations:   %10lu\n"
         "  total bytes:   %10lu\n"
         "  current bytes: %10lu\n"
         "  maximum bytes: %10lu\n",
         stats.ct,
         stats.total,
         stats.current,
         stats.maximum);
#if defined (MSDOS) && !defined (__WATCOMC__)
  fprintf(stderr,
         "Memory used:     %10lu\n"
         "       left:     %10lu\n",
         farcore_init - farcore_min,
         farcore_min);
#endif
}

/*
 * Checked-allocation policy for the shipping build (MALLOC/CALLOC/REALLOC
 * map here; my_* below are the MEMDEBUG variants, which are not compiled
 * into the gate builds because mem.h undefines MEMDEBUG).
 *
 * The handler is installed by pfile_open() for the duration of a compile.
 * With one installed, an allocation failure can never return NULL to a
 * caller that would dereference it: it reports once and exits non-zero.
 * That is deliberate -- a half-built IR has no safe continuation, and the
 * alternative (per-site checks) is exactly how a NULL deref survived at
 * 7.5 MB in one build and crashed the CI runner while passing locally.
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

void *my_malloc(size_t sz)
{
  mem_block_t *blk;

  if (!stat_init) {
    atexit(my_cleanup);
    stat_init = 1;
#if defined (MSDOS) && !defined (__WATCOMC__)
    farcore_init = farcoreleft();
    farcore_min  = farcore_init;
#endif
  }
#if defined (MSDOS) && !defined (__WATCOMC__)
  if (farcoreleft() < farcore_min) {
    farcore_min = farcoreleft();
  }
#endif
  stats.ct++;
  blk = malloc(sizeof(*blk) + sz);
  if (!blk) {
    mem_oom();
  }
  if (blk) {
    unsigned char *ptr;
    unsigned char  poison[] = {0xde, 0xad, 0xbe, 0xef};
    size_t         ii;

    blk->premagic  = PREMAGIC;
    blk->postmagic = POSTMAGIC;
    blk->sz = sz;
    stats.current += sz;
    stats.total   += sz;
    if (stats.current > stats.maximum) {
      stats.maximum = stats.current;
    }
    blk++;
    ptr = (void *) blk;
    for (ii = 0; ii < sz; ii++) {
      ptr[ii] = poison[ii & 3];
    }
  }
  return blk;
}

void *my_calloc(size_t ct, size_t sz)
{
  void *ptr;

  sz *= ct;
  ptr = my_malloc(sz);
  if (ptr) {
    memset(ptr, 0, sz);
  }
  return ptr;
}

void my_free(void *ptr)
{
  if (ptr) {
    mem_block_t *blk;

    blk = ptr;
    blk--;
    assert(PREMAGIC == blk->premagic);
    assert(POSTMAGIC == blk->postmagic);
    stats.current -= blk->sz;
    memset(blk+1, 0xff, blk->sz);
    blk->premagic = 0xdeadbeefUL;
    blk->postmagic = 0xdeadbeefUL;
    free(blk);
  }
}

void *my_realloc(void *ptr, size_t sz)
{
  if (!ptr) {
    ptr = my_malloc(sz);
  } else {
    mem_block_t *blk;

    blk = ptr;
    blk--;

    ptr = my_malloc(sz);
    if (sz > blk->sz) {
      sz = blk->sz;
    }
    memcpy(ptr, (blk + 1), sz);
    my_free(blk + 1);
  }
  return ptr;
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

