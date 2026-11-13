/************************************************************
 **
 ** access_record.h : the target-neutral access RECORDER (ENG A4)
 **
 ** The classic recorder lives in libpic12 and is shaped by the
 ** classic plan; the 32-bit back ends need the same file
 ** discipline without including the classic header.  This module
 ** owns the file, the program/generation bookkeeping, the header
 ** and the row plumbing; a back end supplies its own column
 ** names and values (opaque here), so the SAME writer serves
 ** every back end and the per-backend plan-diff subset stays
 ** one tool.
 **
 ** Switches (strict JALV2_* rule):
 **   JALV2_ACCESS_RECORD=1     enable the recorder
 **   JALV2_ACCESS_RECORD_OUT   the output path when -dump-plan is absent
 **   JALV2_ACCESS_LOG_FAULT=1  the write-failure negative control
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#ifndef access_record_h__
#define access_record_h__

#include "pfile.h"

/* -dump-plan: record to PATH (enables the recorder) */
void          access_record_set_path(const char *path);

/* is the recorder on?  (the env switch or a -dump-plan path) */
boolean_t     access_record_on(void);

/* open the log once, write the column header, remember the program */
void          access_record_begin(pfile_t *pf, const char *program,
    const char *const *headers, unsigned ncols);

/* append one row: program,generation,<vals...> */
void          access_record_row(pfile_t *pf, const char *site,
    const char *var, const unsigned long *vals, unsigned ncols);

/* the generation column (a back end identifies a layout trial with it) */
void          access_record_generation_bump(void);
unsigned long access_record_generation_get(void);

/* per-compile reset (the in-process seam) */
void          access_record_reset(void);

#endif /* access_record_h__ */
