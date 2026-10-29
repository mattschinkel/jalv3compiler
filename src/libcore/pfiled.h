/************************************************************
 **
 ** pfiled.h : pfile definitions
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: minor fork edits
 **
 ************************************************************/
#ifndef pfiled_h__
#define pfiled_h__

#include "pfile.h"

#define PFILE_MULTIPLY_PAIR_MAX 16

struct pfile_str_heap_defer_ {
  struct pfile_str_heap_defer_ *link;
  cmd_t   marker;
  value_t result;
};
typedef struct pfile_str_heap_defer_ pfile_str_heap_defer_t;

struct pfile_ {
  flag_t          flags;
  flag_t          debug_sub;      /* PFILE_DBGSUB_* (jal -debug=…) */
  pfile_source_t *src;
  FILE           *f_asm;          /* asm output  */
  FILE           *f_lst;          /* list output */
  FILE           *f_hex;          /* hex output  */
  FILE           *f_log;          /* log output  */
  ulong           loader_offset;  /* offset for loader18 */
  ulong           code_location;  /* pragma code_location: code base when
                                   * no pragma bootloader (device-file
                                   * default; 0 = backend default) */
  ulong           data_low;       /* lowest `pragma data` address seen */
  ulong           data_high;      /* highest `pragma data` address seen */

  const char      *COD_name;
  COD_directory_t *COD_dir;

  unsigned        errct;          /* # of errors found                   */
  unsigned        warnct;         /* # of warnings found                 */

  unsigned        relwarn_hold;   /* >0: suppress the always-TRUE/FALSE
                                   * relation diagnostics while compiler-
                                   * generated expansions build relations
                                   * (the fold itself still happens)     */

  pfile_pos_t     statement_start;

  unsigned        codegen_disable;/* non-zero to block code generation   */
                                  /* this is needed by JAL because the   */
                                  /* IF statement is used as both IF and */
                                  /* a preprocessor statement            */

  cmd_t           cmd_head;       /* list of commands          */
  cmd_t           cmd_tail;
  cmd_t           cmd_cursor;     /* where to put the next command
                                     (or CMD_NONE to append)   */

  varlist_t       var_const;      /* unnamed constants         */
  struct hash_   *const_idx;      /* value -> var_const member */

  unsigned        label_temp_ct;  /* next temp. id             */
  lbllist_t       label_temp;     /* temporary labels          */
  label_t         label_temp_ptr; /* pointer to next avail.
                                     temp. label               */
  label_t         label_main;         /* entry point to the program       */
  label_t         label_isr;          /* entry point to the program's isr */
  label_t         label_isr_preamble; /* entry to the isr preamble        */
  /* the following are for hex out */
  struct {
    unsigned      pc_msw;         /* PC most signficant word     */
    unsigned      pc;             /* PC at the start of the line */
    unsigned      ct;             /* # of bytes in the buffer    */
    unsigned char buf[16];        /* data buffer                 */
  } hex;

  pfile_proc_t   *proc_root;      /* root of the procedure tree */
  pfile_proc_t   *proc_active;    /* the active procedure       */

  /* each language must provide its own token parsing routines,
   * but all of the other functions can be handled in pfile */
  size_t          token_len;
  char            token[PFILE_TOKEN_SZ_MAX + 1];
  pfile_pos_t     token_start;

  pfile_vectors_t *vectors;
  void            *vector_arg;

  char            *include_path;
  variable_def_t   unnamed_vdefs;

  tag_t            tag_list;
  struct hash_    *tag_idx;       /* name -> tag_list member   */
  pfile_source_t  *src_list;
  pfile_stats_t    stats;
  pfile_token_ct_t token_ct;

  unsigned         task_ct; /* # of concurrent tasks */
  label_t          exit_label; /* exit label for breaking out of loops */
  /* `string_heap_compact` deferred: NOP marker in cmd list expanded after dynstr build */
  pfile_str_heap_defer_t *str_heap_defer;
  /* Sublist build for splicing: pfile_cmd_bld_start/end around emitter calls. */
  unsigned  cmd_bld;
  cmd_t     cmd_bld_head;
  cmd_t     cmd_bld_tail;
  /* dynamic `var string` (heap) handle list for string-heap relocation (Phase A) */
  variable_t        *dynstr_reg;
  size_t             dynstr_reg_ct;
  size_t             dynstr_reg_alloc;

  /* JAL string heap tuning (defaults in pfile_open). Heap size is only
   * `const word _string_heap_size` in user source (`pragma string heap` is ignored).
   * `alloc` policy: 0 = first-fit, 1 = best-fit.
   * `alloc retry` after failed alloc: 0 = off, 1 = one `string_heap_compact` + retry. */
  unsigned char      jal_str_heap_alloc_policy; /* 0=first, 1=best */
  unsigned char      jal_str_heap_alloc_retry;  /* 0=off, 1=one compact+retry */
  /* parse depth of RUNTIME-conditioned / loop bodies: `include` inside
   * these is a clean error (include is compile-time; the old behavior
   * silently included unconditionally, block-scoped). See jal_ctrl.c. */
  unsigned            parse_runtime_block_depth;
  /* PIC32 hex address form: 0 (default) = physical (0x1Dxxxxxx PFM /
   * 0x1FCxxxxx boot flash -- directly ipecmd-programmable, no splice);
   * 1 = KSEG virtual (0x9Dxxxxxx / 0xBFCxxxxx, the QEMU-loader form).
   * Set by the -sim-hex command-line option; see mips_hex.c. */
  unsigned char      mips_sim_hex;

  /*
   * track the number of different multiply routines needed.
   * the widths are in bytes. multiplier <= multiplicand
   * (since multiplication is ...)
   *   some back-ends can use this information to, for example,
   * inline multiplications that are only used once, or create
   * different versions for speed
   */
  pfile_multiply_width_table_entry_t multiply_widths[PFILE_MULTIPLY_PAIR_MAX];
  unsigned multiply_ct;

  /*
   * When non-zero, **jal_parse_value**(RVALUE) must not apply implicit
   * **value_dereference** on data pointers. Used while parsing function-call
   * arguments whose formal is a data pointer (e.g. **byte in name[]** lowered
   * to pointer), so the callee receives the pointer slot, not a loaded byte.
   */
  unsigned parse_suppress_implicit_data_ptr_deref;
  unsigned parse_count_operand;
  unsigned parse_lvalue_ctx;
};

#endif /* pfiled_h__ */

