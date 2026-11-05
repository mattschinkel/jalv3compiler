/************************************************************
 **
 ** jal_string.h : dynamic string runtime helpers
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#ifndef jal_string_h__
#define jal_string_h__

#include "../libcore/operator.h"
#include "../libcore/pfile.h"

boolean_t jal_string_is_dynamic(value_t val);
/* Report (once per compile) that this target core cannot host strings.
 * Every string entry point calls it; on a 12-bit core the diagnostic keeps
 * codegen from running, so the user sees one error instead of a page of
 * resolver failures. */
void      jal_string_core_gate(pfile_t *pf);
/* Validate a `const _string_heap_size = ...` initializer BEFORE it is
 * stored in its declared type (a word constant would already be truncated
 * to 16 bits, hiding 65599 -> 63). Returns FALSE after one diagnostic and
 * marks the heap unusable for the rest of the compile. */
boolean_t jal_string_heap_size_const_check(pfile_t *pf, const char *name,
    variable_def_t def, value_t expr);
value_t   jal_string_arg_payload_ptr(pfile_t *pf, value_t str,
    value_t *len_out);
/* the string payload as a VIEW for `out`/`in out` byte-array formals: the
 * actual becomes the heap array value; the base offset is staged into a
 * compiler word the back end installs as the formal map's base word */
value_t   jal_string_arg_payload_view(pfile_t *pf, value_t str,
    value_t *len_out);
void      jal_string_note_realloc(pfile_t *pf);
/* alias-aware reallocation taint (fixes/fix_string_arg_view.md): growth
 * sites that know their destination record the exact string variable;
 * the propagation substitutes a callee's formals with each call's
 * actuals; the consumer asks whether a callee can move ONE given string */
void      jal_string_note_realloc_dst(pfile_t *pf, value_t dst);
void      jal_string_grow_propagate(pfile_t *pf, pfile_proc_t *caller,
    pfile_proc_t *callee, value_t *parms, size_t param_ct);
boolean_t jal_string_grow_aliases(pfile_proc_t *callee, value_t actual);
/* stores through open-array formals (clone-vs-view decision) */
void      jal_string_formal_store_note(pfile_t *pf, variable_t var);
boolean_t jal_string_formal_stored_get(pfile_proc_t *proc, variable_t var);
void      jal_string_heap_size_legacy_declared(pfile_t *pf);
/* create `_string_error` + the error constants without allocating a heap
 * (used when the user defines `_error_hook`, whose body reads the cell) */
boolean_t jal_string_error_cell_ensure(pfile_t *pf);
boolean_t jal_string_pointer_dynamic_dst_p(value_t val);
boolean_t jal_string_runtime_ensure(pfile_t *pf);
variable_def_t jal_string_dynamic_def_alloc(pfile_t *pf);
variable_def_t jal_string_const_def_alloc(pfile_t *pf);
value_t jal_string_length_value_get(pfile_t *pf, value_t val);
value_t jal_string_arg_byte_array(pfile_t *pf, value_t src);
boolean_t jal_string_assignment_emit(pfile_t *pf, value_t dst, value_t src);
void      jal_string_handle_zero_emit(pfile_t *pf, value_t str);
boolean_t jal_string_assignment_parse_emit(pfile_t *pf, value_t dst);
boolean_t jal_string_is_fixed(value_t val);
/* heap-string subscripts: str[i] reads/writes the i-th CHARACTER */
boolean_t jal_string_subscript_value_is(value_t val);
value_t   jal_string_subscript_read_emit(pfile_t *pf, value_t elem);
boolean_t jal_string_store_outline_emit(pfile_t *pf, value_t dst,
    value_t src);
boolean_t jal_string_subscript_write_emit(pfile_t *pf, value_t dst,
    value_t src);
/* heap string, directly (`var string s`) or through a data pointer
 * (`var pointer p as var string`) */
boolean_t jal_string_pointer_is(value_t val);
boolean_t jal_string_is_source(value_t val);
/* a lexer-generated one-character string literal (`_cstrN`): a char code
 * in numeric contexts, a string when compared with another string */
boolean_t jal_string_char_literal_is(value_t val);
/* Stage a const/fixed string source into a compiler string cell
 * (`_string_chainN`) -- call-argument marshalling for literal actuals
 * into dynamic-string formals. Returns the assigned cell or
 * VALUE_NONE. */
value_t jal_string_stage_source_cell(pfile_t *pf, value_t src);
/* Stage a CONST string source into a compiler `var string[cap]` cell
 * for a FIXED-string formal (string[N] in s) -- literal actuals whose
 * char-count def cannot match the capacity def. Over-capacity literals
 * are a compile error. Returns the cell value or VALUE_NONE. */
value_t jal_string_stage_fixed_cell(pfile_t *pf, value_t src,
    variable_sz_t cap);
boolean_t jal_string_cast_is(const char *nm);
boolean_t jal_string_cast_hard_is(const char *nm);

/* text-cast parse: CURRENT is the cast name and ordinary symbol
 * resolution has already failed for it; only `name(` is the cast */
value_t   jal_string_cast_parse_emit(pfile_t *pf);
value_t   jal_string_concat_chain_emit(pfile_t *pf, value_t first);
value_t jal_string_relation_parse_emit(pfile_t *pf, value_t left,
    operator_t op);
boolean_t jal_string_builtin_is(const char *name);
value_t jal_string_builtin_parse_emit(pfile_t *pf, const char *name);
/* Expand deferred `string_heap_compact` NOPs after the pfile handle registry. */
void jal_string_heap_compact_defer_expand(pfile_t *pf);
void jal_string_error_rvalue_consumed(pfile_t *pf, int is_rvalue,
  value_t *pval);
/* End-of-parse heap-sizing diagnostic: warn when _string_heap_size is
 * below the recommendation for the const literals assigned (skipped
 * when any runtime-length assign exists; see jal_string.c). */
void jal_string_heap_sizing_report(pfile_t *pf);

/* TEARDOWN between compiles (A4): frees the string-heap-use table and
 * zeroes the module state. */
void jal_string_state_reset(void);

#endif /* jal_string_h__ */
