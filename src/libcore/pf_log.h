/************************************************************
 **
 ** pf_log.h : pfile logging declarations
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 ** All rights reserved
 **
 ************************************************************/
#ifndef pf_log_h__
#define pf_log_h__

/* this is a *really* useful way to make sure the format
   and arguments to a function match */
#ifdef __GNUC__
#define ATTRIBUTE __attribute__ ((__format__ (__printf__, 3, 4)))
#else
#define ATTRIBUTE
#endif

#include "univ.h"

typedef enum pfile_log_ {
  PFILE_LOG_NONE,
  PFILE_LOG_CRIT,
  PFILE_LOG_ERR,
  PFILE_LOG_WARN,
  PFILE_LOG_INFO,
  PFILE_LOG_DEBUG
} pfile_log_t;

void pfile_log(pfile_t *pf, pfile_log_t sev, const char *fmt, ...)
     ATTRIBUTE;
/* as pfile_log(), but reported even while code generation is disabled
 * (hard limits: nesting depth, OOM); see pfile.c */
void pfile_log_force(pfile_t *pf, pfile_log_t sev, const char *fmt, ...)
     ATTRIBUTE;
void pfile_log_syserr(pfile_t *pf, result_t rc);

/*
 * Out-of-memory policy (see fix_oom_policy.md).
 *
 * An allocation failure used to be logged and then ignored: every caller
 * carried on with a NULL object, producing a cascade of unrelated
 * diagnostics and, on some limits, an abort inside the value detector.
 * The first memory failure now reports once and stops the process with a
 * non-zero status; recovery is not attempted because a half-built IR has
 * no safe continuation.
 */
void pfile_oom_report(pfile_t *pf);

#endif /* pf_log_h__ */

