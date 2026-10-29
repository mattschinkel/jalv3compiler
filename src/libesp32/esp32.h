/************************************************************
 **
 ** esp32.h : ESP32 (Xtensa LX6, call0 ABI) back-end interface
 **
 ** ESP32 is an Xtensa core. It shares only the libcore IR
 ** (cmd_t / value_t / variable_t) with the PIC back-ends; the
 ** machine IR, literal pool, encoder and image writer are new
 ** (src/libesp32/). The back-end is selected at run time by
 ** `pragma target cpu ESP32`; the libpic12 entry points delegate
 ** here through the back-end descriptor (pic_backend_get, pic.c).
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#ifndef _esp32_h__
#define _esp32_h__

#include "../libcore/pfile.h"

/* back-end entry points (mirrors the libpic12 vtable seam).
 * pic_cmd_optimize is IR-level and shared -- no ESP32 variant. */
void          esp_cmd_generate(pfile_t *pf, const cmd_t cmd);
void          esp_cmd_dump(pfile_t *pf, const cmd_t cmd, boolean_t first);
void          esp_code_cleanup(pfile_t *pf);
void          esp_code_emu(pfile_t *pf);
variable_sz_t esp_pointer_size_get(pfile_t *pf);

#endif /* _esp32_h__ */
