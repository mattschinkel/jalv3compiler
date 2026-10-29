/************************************************************
 **
 ** esp_float.h : ESP32 soft-float intrinsics interface
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#ifndef _esp_float_h__
#define _esp_float_h__

#include "../libcore/pfile.h"

void       esp_float_intrinsics_create(pfile_t *pf);
boolean_t  esp_float_intrinsics_needed(pfile_t *pf);
void       esp_float_call(pfile_t *pf, unsigned idx);
void       esp_float_reset(void);

#define MI(at, v)  (void) esp_code_alloc(ESP_OP_MOVI, at, 0, (int)(v))

#endif
