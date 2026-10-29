/************************************************************
 **
 ** pic_backend.h : non-classic back-end descriptor
 **
 ** The delegation seam through which libpic12 hands code
 ** generation to the other back-end libraries (libpic32,
 ** libesp32).  The descriptor type lives in this leaf header
 ** so those libraries can publish their descriptors without
 ** pulling in the rest of libpic12's interface; the registry
 ** that maps target CPUs to descriptors is declared in pic.h
 ** and wired by the composition root (jal_main).
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#ifndef _pic_backend_h__
#define _pic_backend_h__

#include "../libcore/pfile.h"

/* back-end descriptor: the per-target delegation seam. A NULL
 * descriptor selects the classic PIC12/14/14H/18/SX implementation in
 * libpic12; the other back ends register theirs at startup
 * (pic_backend_register, jal_main) -- libpic12 itself never references
 * them. pic_cmd_optimize is IR-level and shared by every target, so it
 * is not part of the descriptor. */
typedef struct pic_backend_s {
  void          (*cmd_generate)(pfile_t *pf, const cmd_t cmd);
  void          (*cmd_dump)(pfile_t *pf, const cmd_t cmd, boolean_t first);
  void          (*code_cleanup)(pfile_t *pf);
  void          (*code_emu)(pfile_t *pf);
  variable_sz_t (*pointer_size_get)(pfile_t *pf);
  /* optional: clock-tree solve hook (pragma target OSC/clock), routed
   * by pic32_clock_solve_maybe -- NULL means no clock solver */
  void          (*clock_solve_maybe)(pfile_t *pf);
} pic_backend_t;

#endif /* _pic_backend_h__ */
