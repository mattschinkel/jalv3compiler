/************************************************************
 **
 ** jal_incl.h : JAL include and pragma declarations
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: minor fork edits
 **
 ************************************************************/
#ifndef jal_incl_h__
#define jal_incl_h__

#include "../libcore/pfile.h"

/* Forward declarations for pragma fuse structures */
typedef struct jal_pragma_fuses_entry_ {
  struct jal_pragma_fuses_entry_ *link;
  const char                     *name;
  unsigned                        bits;
} jal_pragma_fuses_entry_t;

typedef struct jal_pragma_fuses_ {
  struct jal_pragma_fuses_ *link;
  const char               *name;
  unsigned                  mask;
  unsigned                  config_word; /* which config word */
  jal_pragma_fuses_entry_t *entry;
} jal_pragma_fuses_t;

/* External reference to the global fuses list */
extern jal_pragma_fuses_t *jal_fuses;

/*
 * these are defined both here and in libpic12.
 * it would be far better to have them only defined
 * in one place, but I cannot see how to do that :(
 */
#define JAL_EEPROM                 "_eeprom"
#define JAL_EEPROM_BASE JAL_EEPROM "_base"
#define JAL_EEPROM_USED JAL_EEPROM "_used"

#define JAL_ID                 "_id0"
#define JAL_ID_BASE JAL_ID     "_base"
#define JAL_ID_USED JAL_ID     "_used"

void jal_parse_include(pfile_t *pf, const pfile_pos_t *statement_start);
void jal_parse_pragma(pfile_t *pf, const pfile_pos_t *statement_start);

#endif /* jal_incl_h__ */

