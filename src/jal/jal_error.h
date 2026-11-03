/************************************************************
 **
 ** jal_error.h : user-visible runtime error hook (`_error_hook`)
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#ifndef jal_error_h__
#define jal_error_h__

#include "../libcore/pfile.h"

/* compiler-defined error sources (JAL constants `_ERROR_SOURCE_*`) */
#define JAL_ERROR_SOURCE_STRING 1
#define JAL_ERROR_SOURCE_USER   2

/* Name of the optional user procedure.  Defining it is the opt-in: the
 * compiler finds it by name at each raise site and calls it there. */
#define JAL_ERROR_HOOK_NAME     "_error_hook"
/* Cell holding the source of the current error (created with the hook). */
#define JAL_ERROR_SOURCE_NAME   "_error_source"

/* Per-compile setup: zero the counters and create the user-visible error
 * constants (`_STRING_ERROR_*`, `_ERROR_SOURCE_*`).  The constants are
 * compile-time equates, so this costs nothing. */
void jal_error_program_start(pfile_t *pf);

/* Called when the parser defines a procedure named `_error_hook`: make the
 * cells its body reads (`_error_source`, `_string_error`) exist first. */
void jal_error_hook_prepare(pfile_t *pf);

/* One raise site: store `code` into the `code_cell` variable and, when the
 * program defines `_error_hook`, set `_error_source`, call the hook (no
 * parameters) and clear both cells on return -- delivered once, then gone.
 * Without a hook only the store happens (the historical behaviour) and the
 * site is counted for jal_error_program_end()'s ordering warning. */
void jal_error_raise(pfile_t *pf, unsigned source, const char *code_cell,
    unsigned code);

/* End of parse: warn when the program defines `_error_hook` but some raise
 * sites were emitted before the definition (those will not dispatch). */
void jal_error_program_end(pfile_t *pf);

/* Between compiles in one process. */
void jal_error_state_reset(void);

#endif /* jal_error_h__ */
