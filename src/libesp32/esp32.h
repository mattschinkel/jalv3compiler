/************************************************************
 **
 ** esp32.h : ESP32 (Xtensa LX6, call0 ABI) back-end interface
 **
 ** ESP32 is an Xtensa core. It shares only the libcore IR
 ** (cmd_t / value_t / variable_t) with the PIC back-ends; the
 ** machine IR, literal pool, encoder and image writer are new
 ** (src/libesp32/). The back-end is selected at run time by
 ** `pragma target cpu ESP32`; this library publishes its descriptor
 ** (esp32_backend_get) and jal_main registers it with libpic12's
 ** back-end registry (pic_backend_register, pic.c).
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#ifndef _esp32_h__
#define _esp32_h__

#include "../libcore/pfile.h"
#include "../libpic12/pic_backend.h"

/* back-end entry points (mirrors the libpic12 vtable seam).
 * pic_cmd_optimize is IR-level and shared -- no ESP32 variant. */
void          esp_cmd_generate(pfile_t *pf, const cmd_t cmd);
void          esp_cmd_dump(pfile_t *pf, const cmd_t cmd, boolean_t first);
void          esp_code_cleanup(pfile_t *pf);
void          esp_code_emu(pfile_t *pf);
variable_sz_t esp_pointer_size_get(pfile_t *pf);

/* the descriptor handed to pic_backend_register at startup */
const pic_backend_t *esp32_backend_get(void);

#endif /* _esp32_h__ */
