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
#define JAL_ERROR_SOURCE_CORE   3

/* Name of the optional user procedure.  Defining it is the opt-in: the
 * compiler finds it by name at each raise site and calls it there. */
#define JAL_ERROR_HOOK_NAME     "_error_hook"
/* Cell holding the source of the current error (created with the hook). */
#define JAL_ERROR_SOURCE_NAME   "_error_source"

/* the task-core domain cell (`_core_error`): 0 none, 1 out of range.
 * Like `_string_error` it is consume-on-read and cleared by the hook. */
#define JAL_CORE_ERROR_NAME     "_core_error"
#define JAL_CORE_ERROR_NONE     0
#define JAL_CORE_ERROR_RANGE    1

/* create `_core_error` when it does not exist yet (the raise site and the
 * hook definition call this) */
boolean_t jal_error_core_cell_ensure(pfile_t *pf);

/* a user rvalue read of `_core_error` snapshots it into a temp and clears
 * the cell (consume-on-read; same contract as `_string_error`) */
void jal_error_core_rvalue_consumed(pfile_t *pf, int is_rvalue,
    value_t *pval);

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
