/************************************************************
 **
 ** pic32.h : PIC32 (MIPS32) back-end interface
 **
 ** PIC32 is a MIPS32 core. It shares only the libcore IR
 ** (cmd_t / value_t / variable_t) with the PIC back-end; the
 ** machine IR, register allocator and HEX writer are entirely
 ** new (src/libpic32/). The back-end is selected at run time by
 ** `pragma target cpu PIC_32`; the libpic12 entry points delegate
 ** here through the back-end descriptor (pic_backend_get, pic.c).
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#ifndef _pic32_h__
#define _pic32_h__

#include "../libcore/pfile.h"

/* back-end entry points (mirrors the libpic12 vtable seam).
 * pic_cmd_optimize is IR-level and shared -- no PIC32 variant. */
void          mips_cmd_generate(pfile_t *pf, const cmd_t cmd);
void          mips_cmd_dump(pfile_t *pf, const cmd_t cmd, boolean_t first);
void          mips_code_cleanup(pfile_t *pf);
void          mips_code_emu(pfile_t *pf);
variable_sz_t mips_pointer_size_get(pfile_t *pf);

#endif /* _pic32_h__ */
