/****************************************************************
 **
 ** pic_access.h : access-plan recorder (Step 1a)
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ** The recorder is off unless JALV2_ACCESS_RECORD is on (pic_env_on
 ** truth table).  It writes one CSV row per computed access plan to
 ** JALV2_ACCESS_RECORD_OUT (default plan.log) for the kind x site
 ** matrix.  It must never change code generation.
 **
 ** Test hooks (A11/A12/S1):
 **   JALV2_ACCESS_STORE_FAULT  force the store growth to fail (fatal)
 **   JALV2_ACCESS_LOG_FAULT    force a recorder write failure (error)
 **   JALV2_ACCESS_STATS        print the store sizes at exit
 **   JALV2_ACCESS_COVERAGE     print the exercised recipe rows at exit
 **
 ****************************************************************/
#ifndef pic_access_h__
#define pic_access_h__

#include <stddef.h>

#include "../libcore/access_recipe.h"
#include "pic.h"

/* A1: the recipe key is the kind plus the operation/site family plus the
 * full fact vector.  `kind` is a priority summary and drops facts below
 * the winner, so it is never the key on its own. */
#define PIC_ACCESS_FACT_AOP           0x0001u
#define PIC_ACCESS_FACT_SLOT_POINTEE  0x0002u
#define PIC_ACCESS_FACT_CHUNKED       0x0004u
#define PIC_ACCESS_FACT_HOLED         0x0008u
#define PIC_ACCESS_FACT_SV_VIEW       0x0010u
#define PIC_ACCESS_FACT_RUNTIME_MAP   0x0020u
#define PIC_ACCESS_FACT_VALUE_MAP     0x0040u
#define PIC_ACCESS_FACT_BIND_WALK     0x0080u
#define PIC_ACCESS_FACT_PTR_LOOKUP    0x0100u
#define PIC_ACCESS_FACT_PTR_READ_CALL 0x0200u
#define PIC_ACCESS_FACT_PTR_PTR       0x0400u
#define PIC_ACCESS_FACT_PTR_EEPROM    0x0800u
#define PIC_ACCESS_FACT_PTR_FLASH     0x1000u
#define PIC_ACCESS_FACT_INDIRECT      0x2000u
#define PIC_ACCESS_FACT_PTR_CLASS     0x4000u
#define PIC_ACCESS_FACT_RUNTIME_PTR_CLASS 0x8000u
/* G3: the fat engine is a compile-wide mode, but it is an INPUT to the slot
 * bind decision (a slot store becomes a fat bind only when the engine is on
 * and the slot's pointee is scalar), so it rides the fact vector. */
#define PIC_ACCESS_FACT_FAT_ENGINE    0x10000u

typedef struct pic_access_key_ {
  unsigned kind;
  unsigned operation;   /* site family */
  unsigned facts;       /* bit mask of the fact vector */
} pic_access_key_t;

/* Step 4: the recipe table.  A row matches when kind and operation are
 * equal and the masked facts match; the facts mask lets a row ignore facts
 * its sequence does not use. */
#define PIC_ACCESS_OP_DEREF     1
#define PIC_ACCESS_OP_BIND      2
#define PIC_ACCESS_OP_PTR_WRITE 3
#define PIC_ACCESS_OP_PTR_READ  4
#define PIC_ACCESS_OP_ADDR_OF   5
#define PIC_ACCESS_OP_ASSIGN_CLASS 6
#define PIC_ACCESS_OP_BIND_DEST    7
#define PIC_ACCESS_OP_FSR_WALK     8
#define PIC_ACCESS_OP_FSR_DYNAMIC  9
#define PIC_ACCESS_OP_FSR_BIND    10
#define PIC_ACCESS_OP_INDIRECT_SLOT 11

/* a row may match any kind (the bind decision is about the facts) */
#define PIC_ACCESS_KIND_ANY          0xFFFFFFFFu
#define PIC_ACCESS_ACTION_NONE       0u
/* 1 is retired: G3 S1 made the scalar-pointee slot store FAT_BIND and
 * S3/D4 made the array-pointee slot store FAT_BIND too, so the old
 * NOT_FAT_BIND row lost its last reachable key; the engine-off path
 * returns before the table is consulted.  The remaining numbers stay
 * stable so no value is silently re-used. */
#define PIC_ACCESS_ACTION_PTR_WRITE_LOOKUP_IND 2u
#define PIC_ACCESS_ACTION_PTR_WRITE_LOOKUP     3u
#define PIC_ACCESS_ACTION_PTR_WRITE_LINEAR     4u
#define PIC_ACCESS_ACTION_PTR_READ_CALL_SLOT   5u
#define PIC_ACCESS_ACTION_PTR_READ_CALL        6u
#define PIC_ACCESS_ACTION_PTR_READ_PTR         7u
#define PIC_ACCESS_ACTION_PTR_READ_LOOKUP      8u
#define PIC_ACCESS_ACTION_PTR_READ_LINEAR      9u
#define PIC_ACCESS_ACTION_ADDR_OF_SLOT         10u
#define PIC_ACCESS_ACTION_ASSIGN_PTR_CLASS       11u
#define PIC_ACCESS_ACTION_ASSIGN_RUNTIME_PTR_CLASS 12u
#define PIC_ACCESS_ACTION_BIND_SLOT_CHUNKED       13u
#define PIC_ACCESS_ACTION_BIND_SLOT               14u
#define PIC_ACCESS_ACTION_BIND_PLAIN              15u
#define PIC_ACCESS_ACTION_FSR_WALK_RUNTIME        16u
#define PIC_ACCESS_ACTION_FSR_WALK_HOLED          17u
#define PIC_ACCESS_ACTION_FSR_WALK_PLAIN          18u
#define PIC_ACCESS_ACTION_FSR_DYNAMIC_RUNTIME     19u
#define PIC_ACCESS_ACTION_FSR_DYNAMIC_SV          20u
#define PIC_ACCESS_ACTION_FSR_DYNAMIC_PLAIN       21u
#define PIC_ACCESS_ACTION_FSR_BIND_SV             22u
#define PIC_ACCESS_ACTION_FSR_BIND_HOLED          23u
#define PIC_ACCESS_ACTION_FSR_BIND_PLAIN          24u
#define PIC_ACCESS_ACTION_SLOT_ARRAY              25u
#define PIC_ACCESS_ACTION_SLOT_SCALAR             26u
#define PIC_ACCESS_ACTION_SLOT_NOT                27u
/* G3: a scalar-pointee slot store under the fat engine stores the tagged
 * (id + object-relative offset) pointer value, not a physical address. */
#define PIC_ACCESS_ACTION_FAT_BIND                28u

typedef struct pic_access_recipe_ {
  unsigned kind;
  unsigned operation;
  unsigned facts;
  unsigned facts_mask;
  unsigned action;   /* for decisions that do not move a body */
  void (*run)(pfile_t *pf, const pic_access_plan_t *plan,
              value_t val, size_t offset);
} pic_access_recipe_t;

const pic_access_recipe_t *pic_access_recipe_find(
    const pic_access_key_t *key);
void pic_fat_deref_recipe(pfile_t *pf, const pic_access_plan_t *plan,
    value_t val, size_t offset);

void      pic_access_key_fill(const pic_access_plan_t *plan,
              unsigned operation, pic_access_key_t *key);

/* A2: the whole plan, cached per value/generation; the portable
 * `access_kind` lives on value_t and guards the cache.  C4/S1: the
 * stores take the pfile so an allocation failure is the fatal
 * pfile_log_syserr(pf, ENOMEM) path, not a silent re-derive. */
boolean_t pic_access_plan_load(const value_t val, pic_access_plan_t *plan);
void      pic_access_plan_store(pfile_t *pf, const value_t val,
              const pic_access_plan_t *plan);
/* the variable-keyed half of the store (the six external fill_var sites) */
boolean_t pic_access_var_load(variable_t var, pic_access_plan_t *plan);
void      pic_access_var_store(pfile_t *pf, variable_t var,
              const pic_access_plan_t *plan);
/* fill (and record) a variable plan, then answer the recipe action */
unsigned  pic_access_var_action_get(pfile_t *pf, const char *site,
              variable_t var, unsigned op);
/* value-keyed action (the indirect family) */
unsigned  pic_access_value_action_get(pfile_t *pf, const char *site,
              const value_t val, unsigned op);
/* the fact mask and the unescaped bind target, without a plan read */
unsigned  pic_access_value_facts_get(pfile_t *pf, const value_t val,
              variable_t *bind_walk);
/* the fsr sites also need the unescaped bind target; return it without a
 * plan read in the emitter */
unsigned  pic_access_var_action_get2(pfile_t *pf, const char *site,
              variable_t var, unsigned op, variable_t *bind_walk);

boolean_t     pic_access_record_on(void);
/* -dump-plan=<file>: record to this path without an env toggle */
void          pic_access_dump_set(const char *path);
void          pic_access_record_begin(pfile_t *pf, const char *program);
void          pic_access_record_plan(pfile_t *pf, const char *site,
                  variable_t var, const pic_access_plan_t *plan);
void          pic_access_generation_bump(void);
unsigned long pic_access_generation_get(void);
void          pic_access_reset(void);

/* S4/Q1: the classic kind keeps its 0..6 values (FAT stays 5); this is
 * the explicit mapping onto the shared class vocabulary. */
access_class_t pic_access_neutral_class(pic_access_kind_t kind);
/* E3: the classic table's mask rule, checked once per process */
void          pic_access_recipe_audit(pfile_t *pf);

/* E2 row identity (A7/A15): the emitter announces each command before it
 * converts it, so a recorded row carries file:function:label, the
 * per-command ordinal and the source line (a non-key annotation). */
void          pic_access_cmd_set(pfile_t *pf, cmd_t cmd);
/* -explain-access <file>:<line>: collect the decisions on that line and
 * print them once the compile has finished.  Returns the exit code: 0 for
 * rows (or none), 1 when a REFUSE row is on the line. */
void          pic_access_explain_set(const char *program, const char *file,
                  unsigned line);
/* `recipe` is the matched row (or 0); a row with a `run` body prints as
 * FAT_DEREF, an action row prints as its action name. */
void          pic_access_explain_note(pfile_t *pf, const char *site,
                  variable_t var, const pic_access_plan_t *plan,
                  unsigned operation,
                  const pic_access_recipe_t *recipe);
/* WHEREIS address reporting: the compile-time address a line resolved, the
 * object it landed in and the resolution path (a linear base+offset add or
 * a chunk-map walk).  WHEREIS is not an emitted access, so it has no recipe
 * row; the address prints beside the access rows of the same source line,
 * in emission order.  The last resolution wins, because a layout trial
 * resolves before the installed layout does. */
void          pic_access_explain_whereis(pfile_t *pf, variable_t var,
                  const char *target, variable_const_t ofs,
                  variable_base_t addr);
int           pic_access_explain_print(void);

#endif /* pic_access_h__ */
