/************************************************************
 **
 ** mem.h : memory debugging declarations
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 ** All rights reserved
 **
 ************************************************************/
#ifndef mem_h__
#define mem_h__

#include <stdlib.h>

#undef  MEMDEBUG

char *my_strdup(const char *src);
#define STRDUP(x)   my_strdup(x)

#ifdef MEMDEBUG

void *my_malloc(size_t x);
void *my_calloc(size_t x, size_t y);
void  my_free(void *ptr);
void *my_realloc(void *ptr, size_t sz);

#define MALLOC(x)    my_malloc(x)
#define CALLOC(x,y)  my_calloc(x, y)
#define FREE(x)      my_free(x)
#define REALLOC(x,y) my_realloc(x,y)

#else

#include <stdlib.h>

#define MALLOC(x)    mem_alloc(x)
#define CALLOC(x,y)  mem_calloc((x), (y))
#define REALLOC(x,y) mem_realloc((x), (y))
#define FREE(x)      do { if (x) free(x) ; } while (0)

#endif

/*
 * Checked allocation.  Memory exhaustion is an environment failure, not a
 * source error: there is no meaningful way to finish building the IR after
 * an allocation fails, so the first failure reports once and stops the
 * process.  The driver installs the handler for the duration of a compile
 * (pfile_open .. pfile_close); before one exists these return NULL and the
 * early-startup callers handle it.  See pfile_oom_report().
 */
void *mem_alloc(size_t sz);
void *mem_calloc(size_t ct, size_t sz);
void *mem_realloc(void *ptr, size_t sz);
void  mem_set_oom_handler(void (*fn)(void *), void *context);

#endif /* mem_h__ */

