/****************************************************************
 **
 ** pic_access.c : access-plan recorder (Step 1a)
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ** Records one CSV row per computed access plan when
 ** JALV2_ACCESS_RECORD is on.  The recorder writes only; it never
 ** feeds an emitter and never changes the image.
 **
 ** Test hooks (A11/S1):
 **   JALV2_ACCESS_STORE_FAULT  the plan stores' growth fails, exactly as
 **                             a realloc failure would; the store is
 **                             fatal (pfile_log_syserr(ENOMEM)).
 **   JALV2_ACCESS_LOG_FAULT    the recorder write fails (ferror); while
 **                             recording that is a hard error.
 **   JALV2_ACCESS_STATS        at exit, print the two stores' live
 **                             records and capacities (growth test).
 **   JALV2_ACCESS_COVERAGE     at exit, print the recipe rows the
 **                             compile exercised (A12 per-row coverage).
 **
 ****************************************************************/
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../libcore/access_record.h"
#include "../libcore/pf_log.h"
#include "../libcore/pf_proc.h"
#include "pic_access.h"
#include "pic_var.h"

#define PIC_ACCESS_PROGRAM_MAX 256
#define PIC_ACCESS_RECIPE_MAX  32   /* coverage bitmap width */
#define PIC_ACCESS_SITE_MAX    384
#define PIC_ACCESS_EXPLAIN_MAX 64   /* rows printed for one source line */

typedef struct pic_access_entry_ {
  unsigned long     generation;
  boolean_t         valid;
  pic_access_plan_t plan;
} pic_access_entry_t;

/* the file, program and header plumbing lives in the neutral recorder
 * (libcore/access_record); this module keeps only the classic site and
 * ordinal bookkeeping */
static unsigned long pic_access_generation;

static pic_access_entry_t *pic_access_entries;
static size_t       pic_access_entries_ct;
static pic_access_entry_t *pic_access_var_entries;
static size_t       pic_access_var_entries_ct;
static unsigned long pic_access_value_stores;
static unsigned long pic_access_var_stores;
static unsigned long pic_access_recipe_seen;
static boolean_t    pic_access_report_armed;

/* E2 row identity: the command whose accesses are being converted.  The
 * emitter sets it per command (pic_cmd_out), including during the layout
 * trials, so every recorded row knows its site, ordinal and line. */
static cmd_t          pic_access_cmd;
static pfile_proc_t  *pic_access_proc;
static unsigned       pic_access_ordinal;      /* recorded plan rows   */
static unsigned       pic_access_note_ordinal; /* explain decisions    */

/* -explain-access: only the rows of the requested line are kept. */
typedef struct pic_access_event_ {
  unsigned seq;
  unsigned ordinal;
  unsigned kind;
  unsigned facts;
  unsigned operation;
  unsigned action;
  boolean_t body;     /* the matched row is a run body (FAT deref) */
  boolean_t whereis;  /* a WHEREIS address, not an emitted access */
  variable_base_t addr;
  variable_const_t ofs;
  unsigned  segments;
  char     site[PIC_ACCESS_SITE_MAX];
  char     var[64];
} pic_access_event_t;

static pic_access_event_t pic_access_events[PIC_ACCESS_EXPLAIN_MAX];
static size_t      pic_access_event_ct;
static unsigned    pic_access_event_seq;
static boolean_t   pic_access_explain_on;
static boolean_t   pic_access_explain_overflow;
static char        pic_access_explain_program[PIC_ACCESS_PROGRAM_MAX];
static char        pic_access_explain_file[PIC_ACCESS_SITE_MAX];
static unsigned    pic_access_explain_line;

static const char *pic_access_kind_name(unsigned kind)
{
  switch (kind) {
    case PIC_ACCESS_DIRECT:       return "DIRECT";
    case PIC_ACCESS_RESOLVE:      return "RESOLVE";
    case PIC_ACCESS_LINEAR:       return "LINEAR";
    case PIC_ACCESS_WALK_BAKED:   return "WALK_BAKED";
    case PIC_ACCESS_WALK_RUNTIME: return "WALK_RUNTIME";
    case PIC_ACCESS_FAT:          return "FAT";
    case PIC_ACCESS_REFUSE:       return "REFUSE";
    default:                      return "?";
  }
}

static const char *pic_access_operation_name(unsigned operation)
{
  switch (operation) {
    case PIC_ACCESS_OP_DEREF:         return "DEREF";
    case PIC_ACCESS_OP_BIND:          return "BIND";
    case PIC_ACCESS_OP_PTR_WRITE:     return "PTR_WRITE";
    case PIC_ACCESS_OP_PTR_READ:      return "PTR_READ";
    case PIC_ACCESS_OP_ADDR_OF:       return "ADDR_OF";
    case PIC_ACCESS_OP_ASSIGN_CLASS:  return "ASSIGN_CLASS";
    case PIC_ACCESS_OP_BIND_DEST:     return "BIND_DEST";
    case PIC_ACCESS_OP_FSR_WALK:      return "FSR_WALK";
    case PIC_ACCESS_OP_FSR_DYNAMIC:   return "FSR_DYNAMIC";
    case PIC_ACCESS_OP_FSR_BIND:      return "FSR_BIND";
    case PIC_ACCESS_OP_INDIRECT_SLOT: return "INDIRECT_SLOT";
    default:                          return "?";
  }
}

static const char *pic_access_action_name(unsigned action)
{
  switch (action) {
    case PIC_ACCESS_ACTION_NONE:                    return "NONE";
    case PIC_ACCESS_ACTION_NOT_FAT_BIND:            return "NOT_FAT_BIND";
    case PIC_ACCESS_ACTION_FAT_BIND:                return "FAT_BIND";
    case PIC_ACCESS_ACTION_PTR_WRITE_LOOKUP_IND:    return "PTR_WRITE_LOOKUP_IND";
    case PIC_ACCESS_ACTION_PTR_WRITE_LOOKUP:        return "PTR_WRITE_LOOKUP";
    case PIC_ACCESS_ACTION_PTR_WRITE_LINEAR:        return "PTR_WRITE_LINEAR";
    case PIC_ACCESS_ACTION_PTR_READ_CALL_SLOT:      return "PTR_READ_CALL_SLOT";
    case PIC_ACCESS_ACTION_PTR_READ_CALL:           return "PTR_READ_CALL";
    case PIC_ACCESS_ACTION_PTR_READ_PTR:            return "PTR_READ_PTR";
    case PIC_ACCESS_ACTION_PTR_READ_LOOKUP:         return "PTR_READ_LOOKUP";
    case PIC_ACCESS_ACTION_PTR_READ_LINEAR:         return "PTR_READ_LINEAR";
    case PIC_ACCESS_ACTION_ADDR_OF_SLOT:            return "ADDR_OF_SLOT";
    case PIC_ACCESS_ACTION_ASSIGN_PTR_CLASS:        return "ASSIGN_PTR_CLASS";
    case PIC_ACCESS_ACTION_ASSIGN_RUNTIME_PTR_CLASS:
      return "ASSIGN_RUNTIME_PTR_CLASS";
    case PIC_ACCESS_ACTION_BIND_SLOT_CHUNKED:       return "BIND_SLOT_CHUNKED";
    case PIC_ACCESS_ACTION_BIND_SLOT:               return "BIND_SLOT";
    case PIC_ACCESS_ACTION_BIND_PLAIN:              return "BIND_PLAIN";
    case PIC_ACCESS_ACTION_FSR_WALK_RUNTIME:        return "FSR_WALK_RUNTIME";
    case PIC_ACCESS_ACTION_FSR_WALK_HOLED:          return "FSR_WALK_HOLED";
    case PIC_ACCESS_ACTION_FSR_WALK_PLAIN:          return "FSR_WALK_PLAIN";
    case PIC_ACCESS_ACTION_FSR_DYNAMIC_RUNTIME:     return "FSR_DYNAMIC_RUNTIME";
    case PIC_ACCESS_ACTION_FSR_DYNAMIC_SV:          return "FSR_DYNAMIC_SV";
    case PIC_ACCESS_ACTION_FSR_DYNAMIC_PLAIN:       return "FSR_DYNAMIC_PLAIN";
    case PIC_ACCESS_ACTION_FSR_BIND_SV:             return "FSR_BIND_SV";
    case PIC_ACCESS_ACTION_FSR_BIND_HOLED:          return "FSR_BIND_HOLED";
    case PIC_ACCESS_ACTION_FSR_BIND_PLAIN:          return "FSR_BIND_PLAIN";
    case PIC_ACCESS_ACTION_SLOT_ARRAY:              return "SLOT_ARRAY";
    case PIC_ACCESS_ACTION_SLOT_SCALAR:             return "SLOT_SCALAR";
    case PIC_ACCESS_ACTION_SLOT_NOT:                return "SLOT_NOT";
    default:                                        return "?";
  }
}

static void pic_access_append(char *dst, size_t dst_sz, const char *src)
{
  size_t len;

  len = strlen(dst);
  if ((len + 1) >= dst_sz) {
    return;
  }
  strncat(dst, src, dst_sz - len - 1);
}

static void pic_access_copy(char *dst, size_t dst_sz, const char *src)
{
  size_t len;

  len = strlen(src);
  if (len >= dst_sz) {
    len = dst_sz - 1;
  }
  if (len) {
    memcpy(dst, src, len);
  }
  dst[len] = 0;
}

/* the access's source file and line: the command's position when the
 * emitter announced one, else the statement start */
static void pic_access_origin_get(pfile_t *pf, const char **file,
    unsigned *line)
{
  pfile_pos_t pos;

  memset(&pos, 0, sizeof(pos));
  if (0 != pic_access_cmd) {
    cmd_pos_get(pic_access_cmd, &pos);
  } else {
    pfile_statement_start_get(pf, &pos);
  }
  *file = (0 != pos.src) ? pfile_source_name_get(pos.src) : "?";
  *line = pos.line;
}

/* A7: a stable, line-independent site id: file:function:label */
static void pic_access_site_id(pfile_t *pf, const char *label,
    char *dst, size_t dst_sz)
{
  const char *file;
  const char *func;
  unsigned    line;

  pic_access_origin_get(pf, &file, &line);
  func = (0 != pic_access_proc) ? pfile_proc_tag_get(pic_access_proc) : 0;
  dst[0] = 0;
  pic_access_append(dst, dst_sz, (file) ? file : "?");
  pic_access_append(dst, dst_sz, ":");
  pic_access_append(dst, dst_sz, (func) ? func : "-");
  pic_access_append(dst, dst_sz, ":");
  pic_access_append(dst, dst_sz, (label) ? label : "?");
}

static void pic_access_facts_name(unsigned facts, char *dst, size_t dst_sz)
{
  static const struct {
    unsigned bit;
    const char *name;
  } names[] = {
    { PIC_ACCESS_FACT_AOP,              "aop" },
    { PIC_ACCESS_FACT_SLOT_POINTEE,     "slot_pointee" },
    { PIC_ACCESS_FACT_CHUNKED,          "chunked" },
    { PIC_ACCESS_FACT_HOLED,            "holed" },
    { PIC_ACCESS_FACT_SV_VIEW,          "sv_view" },
    { PIC_ACCESS_FACT_RUNTIME_MAP,      "runtime_map" },
    { PIC_ACCESS_FACT_VALUE_MAP,        "value_map" },
    { PIC_ACCESS_FACT_BIND_WALK,        "bind_walk" },
    { PIC_ACCESS_FACT_PTR_LOOKUP,       "ptr_lookup" },
    { PIC_ACCESS_FACT_PTR_READ_CALL,    "ptr_read_call" },
    { PIC_ACCESS_FACT_PTR_PTR,          "ptr_ptr" },
    { PIC_ACCESS_FACT_PTR_EEPROM,       "ptr_eeprom" },
    { PIC_ACCESS_FACT_PTR_FLASH,        "ptr_flash" },
    { PIC_ACCESS_FACT_INDIRECT,         "indirect" },
    { PIC_ACCESS_FACT_PTR_CLASS,        "ptr_class" },
    { PIC_ACCESS_FACT_RUNTIME_PTR_CLASS,"runtime_ptr_class" }
  };
  size_t ii;

  dst[0] = 0;
  for (ii = 0; ii < sizeof(names) / sizeof(names[0]); ii++) {
    if (facts & names[ii].bit) {
      if (dst[0]) {
        pic_access_append(dst, dst_sz, "|");
      }
      pic_access_append(dst, dst_sz, names[ii].name);
    }
  }
  if (0 == dst[0]) {
    pic_access_append(dst, dst_sz, "-");
  }
}

void pic_access_cmd_set(pfile_t *pf, cmd_t cmd)
{
  unsigned typ;

  (void) pf;
  typ = cmd_type_get(cmd);
  if (CMD_TYPE_PROC_ENTER == typ) {
    pic_access_proc = cmd_proc_get(cmd);
  }
  if (cmd != pic_access_cmd) {
    pic_access_cmd = cmd;
    pic_access_ordinal = 0;
    pic_access_note_ordinal = 0;
  }
  if (CMD_TYPE_PROC_LEAVE == typ) {
    pic_access_proc = 0;
  }
}

void pic_access_explain_set(const char *program, const char *file,
    unsigned line)
{
  pic_access_event_ct = 0;
  pic_access_event_seq = 0;
  pic_access_explain_overflow = BOOLEAN_FALSE;
  pic_access_explain_line = line;
  pic_access_explain_on = BOOLEAN_TRUE;
  pic_access_explain_program[0] = 0;
  if (program) {
    pic_access_copy(pic_access_explain_program, PIC_ACCESS_PROGRAM_MAX,
        program);
  }
  pic_access_explain_file[0] = 0;
  if (file) {
    pic_access_copy(pic_access_explain_file, PIC_ACCESS_SITE_MAX, file);
  }
}

void pic_access_explain_note(pfile_t *pf, const char *label, variable_t var,
    const pic_access_plan_t *plan, unsigned operation,
    const pic_access_recipe_t *recipe)
{
  const char *file;
  const char *name;
  char        site_id[PIC_ACCESS_SITE_MAX];
  unsigned    line;
  unsigned    facts;
  unsigned    action;
  boolean_t   body;
  pic_access_key_t key;
  size_t      ii;
  pic_access_event_t *ev;

  if (!pic_access_explain_on || (0 == plan)) {
    return;
  }
  pic_access_origin_get(pf, &file, &line);
  if ((line != pic_access_explain_line) || (0 == file)
      || (0 != strcmp(file, pic_access_explain_file))) {
    return;
  }
  pic_access_site_id(pf, label, site_id, sizeof(site_id));
  pic_access_key_fill(plan, operation, &key);
  facts = key.facts;
  action = (0 != recipe) ? recipe->action : PIC_ACCESS_ACTION_NONE;
  body = ((0 != recipe) && (0 != recipe->run)) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
  name = (var && (VARIABLE_NONE != var)) ? variable_name_get(var) : "-";
  /* the same decision is re-made in every generation and trial; keep the
   * first occurrence (its seq fixes the operand order) */
  for (ii = 0; ii < pic_access_event_ct; ii++) {
    ev = &pic_access_events[ii];
    if ((ev->kind == (unsigned) plan->kind) && (ev->facts == facts)
        && (ev->operation == operation) && (ev->action == action)
        && (ev->body == body)
        && (0 == strcmp(ev->site, site_id))
        && (0 == strcmp(ev->var, (name) ? name : "-"))) {
      return;
    }
  }
  if (pic_access_event_ct >= PIC_ACCESS_EXPLAIN_MAX) {
    pic_access_explain_overflow = BOOLEAN_TRUE;
    return;
  }
  ev = &pic_access_events[pic_access_event_ct++];
  ev->seq = pic_access_event_seq++;
  ev->ordinal = pic_access_note_ordinal++;
  ev->kind = (unsigned) plan->kind;
  ev->facts = facts;
  ev->operation = operation;
  ev->action = action;
  ev->body = body;
  /* the array is reused across compiles in one process; a slot that held a
   * WHEREIS event must not keep the flag when it becomes an access row */
  ev->whereis = BOOLEAN_FALSE;
  pic_access_copy(ev->site, sizeof(ev->site), site_id);
  pic_access_copy(ev->var, sizeof(ev->var), (name) ? name : "-");
}

void pic_access_explain_whereis(pfile_t *pf, variable_t var,
    const char *target, variable_const_t ofs, variable_base_t addr)
{
  const char      *file;
  unsigned         line;
  unsigned         segments;
  pic_mem_object_t obj;
  size_t           ii;
  pic_access_event_t *ev;

  if (!pic_access_explain_on) {
    return;
  }
  pic_access_origin_get(pf, &file, &line);
  if ((line != pic_access_explain_line) || (0 == file)
      || (0 != strcmp(file, pic_access_explain_file))) {
    return;
  }
  segments = 0;
  if (pic_mem_object_get(var, &obj)) {
    segments = (unsigned) obj.seg_ct;
  }
  /* a layout trial resolves the WHEREIS before the installed layout does;
   * the last resolution carries the address the image will use */
  for (ii = 0; ii < pic_access_event_ct; ii++) {
    ev = &pic_access_events[ii];
    if (ev->whereis && (ev->ofs == ofs)
        && (0 == strcmp(ev->var, target))) {
      ev->addr = addr;
      ev->ofs = ofs;
      ev->segments = segments;
      return;
    }
  }
  if (pic_access_event_ct >= PIC_ACCESS_EXPLAIN_MAX) {
    pic_access_explain_overflow = BOOLEAN_TRUE;
    return;
  }
  ev = &pic_access_events[pic_access_event_ct++];
  memset(ev, 0, sizeof(*ev));
  ev->seq = pic_access_event_seq++;
  ev->ordinal = pic_access_note_ordinal++;
  ev->whereis = BOOLEAN_TRUE;
  ev->addr = addr;
  ev->ofs = ofs;
  ev->segments = segments;
  pic_access_copy(ev->site, sizeof(ev->site), "-");
  pic_access_copy(ev->var, sizeof(ev->var), target);
}

int pic_access_explain_print(void)
{
  char     facts[192];
  size_t   ii;
  size_t   jj;
  int      rc;
  boolean_t fat;
  boolean_t refuse;

  if (!pic_access_explain_on) {
    return 0;
  }
  if (0 == pic_access_event_ct) {
    printf("no access at %s:%u\n", pic_access_explain_file,
        pic_access_explain_line);
    return 0;
  }
  /* emission order, which is operand order within the command */
  for (ii = 1; ii < pic_access_event_ct; ii++) {
    pic_access_event_t tmp;

    tmp = pic_access_events[ii];
    jj = ii;
    while ((jj > 0) && (pic_access_events[jj - 1].seq > tmp.seq)) {
      pic_access_events[jj] = pic_access_events[jj - 1];
      jj--;
    }
    pic_access_events[jj] = tmp;
  }
  fat = BOOLEAN_FALSE;
  refuse = BOOLEAN_FALSE;
  for (ii = 0; ii < pic_access_event_ct; ii++) {
    const pic_access_event_t *ev;

    ev = &pic_access_events[ii];
    if (ev->whereis) {
      printf("whereis: program=%s line=%u ordinal=%u target=%s ofs=%u"
          " addr=%u (0x%x) segments=%u resolve=%s\n",
          pic_access_explain_program, pic_access_explain_line, ev->ordinal,
          ev->var, (unsigned) ev->ofs, (unsigned) ev->addr,
          (unsigned) ev->addr, ev->segments,
          (ev->segments > 1) ? "chunk" : "linear");
      continue;
    }
    pic_access_facts_name(ev->facts, facts, sizeof(facts));
    printf("access: program=%s line=%u ordinal=%u site=%s var=%s kind=%s"
        " facts=%s op=%s recipe=%s\n",
        pic_access_explain_program, pic_access_explain_line, ev->ordinal,
        ev->site, ev->var,
        pic_access_kind_name(ev->kind), facts,
        pic_access_operation_name(ev->operation),
        (ev->body) ? "FAT_DEREF" : pic_access_action_name(ev->action));
    if (PIC_ACCESS_FAT == ev->kind) {
      fat = BOOLEAN_TRUE;
    }
    if (PIC_ACCESS_REFUSE == ev->kind) {
      refuse = BOOLEAN_TRUE;
    }
  }
  if (fat) {
    printf("note: FAT dispatch -- the pointer tag decides the target at run"
        " time\n");
  }
  if (pic_access_explain_overflow) {
    printf("note: more than %u accesses on this line; the rest are omitted\n",
        (unsigned) PIC_ACCESS_EXPLAIN_MAX);
  }
  rc = (refuse) ? 1 : 0;
  return rc;
}

void pic_access_key_fill(const pic_access_plan_t *plan, unsigned operation,
    pic_access_key_t *key)
{
  unsigned facts;

  facts = 0;
  if (plan->aop) {
    facts |= PIC_ACCESS_FACT_AOP;
  }
  if (plan->slot_pointee_array) {
    facts |= PIC_ACCESS_FACT_SLOT_POINTEE;
  }
  if (plan->chunked) {
    facts |= PIC_ACCESS_FACT_CHUNKED;
  }
  if (plan->holed) {
    facts |= PIC_ACCESS_FACT_HOLED;
  }
  if (plan->sv_view) {
    facts |= PIC_ACCESS_FACT_SV_VIEW;
  }
  if (plan->runtime_map) {
    facts |= PIC_ACCESS_FACT_RUNTIME_MAP;
  }
  if (plan->value_map) {
    facts |= PIC_ACCESS_FACT_VALUE_MAP;
  }
  if (VARIABLE_NONE != plan->bind_walk) {
    facts |= PIC_ACCESS_FACT_BIND_WALK;
  }
  if (plan->ptr_lookup) {
    facts |= PIC_ACCESS_FACT_PTR_LOOKUP;
  }
  if (plan->ptr_read_call) {
    facts |= PIC_ACCESS_FACT_PTR_READ_CALL;
  }
  if (plan->ptr_ptr) {
    facts |= PIC_ACCESS_FACT_PTR_PTR;
  }
  if (plan->ptr_eeprom) {
    facts |= PIC_ACCESS_FACT_PTR_EEPROM;
  }
  if (plan->ptr_flash) {
    facts |= PIC_ACCESS_FACT_PTR_FLASH;
  }
  if (plan->indirect) {
    facts |= PIC_ACCESS_FACT_INDIRECT;
  }
  if (plan->ptr_class) {
    facts |= PIC_ACCESS_FACT_PTR_CLASS;
  }
  if (plan->runtime_ptr_class) {
    facts |= PIC_ACCESS_FACT_RUNTIME_PTR_CLASS;
  }
  if (plan->fat_engine) {
    facts |= PIC_ACCESS_FACT_FAT_ENGINE;
  }
  key->kind = (unsigned) plan->kind;
  key->operation = operation;
  key->facts = facts;
}

static const pic_access_recipe_t pic_access_recipes[] = {
  { PIC_ACCESS_FAT, PIC_ACCESS_OP_DEREF, 0u, 0u,
    PIC_ACCESS_ACTION_NONE, pic_fat_deref_recipe },
  /* G3: under the fat engine a slot store IS a fat bind (the slot holds the
   * tagged id + object-relative offset, so a runtime element offset needs no
   * bind-time chunk walk).  S3: this now includes the ARRAY-pointee slot --
   * its stored value is the ordinary fat pointer, so a loaded pointer carries
   * the object identity (design D4). */
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_BIND,
    PIC_ACCESS_FACT_AOP | PIC_ACCESS_FACT_FAT_ENGINE,
    PIC_ACCESS_FACT_AOP | PIC_ACCESS_FACT_FAT_ENGINE,
    PIC_ACCESS_ACTION_FAT_BIND, 0 },
  /* pointer write: the storage class and the indirect shape */
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_PTR_WRITE,
    PIC_ACCESS_FACT_PTR_LOOKUP | PIC_ACCESS_FACT_INDIRECT,
    PIC_ACCESS_FACT_PTR_LOOKUP | PIC_ACCESS_FACT_INDIRECT,
    PIC_ACCESS_ACTION_PTR_WRITE_LOOKUP_IND, 0 },
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_PTR_WRITE,
    PIC_ACCESS_FACT_PTR_LOOKUP,
    PIC_ACCESS_FACT_PTR_LOOKUP | PIC_ACCESS_FACT_INDIRECT,
    PIC_ACCESS_ACTION_PTR_WRITE_LOOKUP, 0 },
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_PTR_WRITE,
    0u, PIC_ACCESS_FACT_PTR_LOOKUP,
    PIC_ACCESS_ACTION_PTR_WRITE_LINEAR, 0 },
  /* pointer read: helper call (slot vs generic), ptr, lookup, linear */
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_PTR_READ,
    PIC_ACCESS_FACT_PTR_READ_CALL | PIC_ACCESS_FACT_AOP,
    PIC_ACCESS_FACT_PTR_READ_CALL | PIC_ACCESS_FACT_AOP,
    PIC_ACCESS_ACTION_PTR_READ_CALL_SLOT, 0 },
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_PTR_READ,
    PIC_ACCESS_FACT_PTR_READ_CALL,
    PIC_ACCESS_FACT_PTR_READ_CALL | PIC_ACCESS_FACT_AOP,
    PIC_ACCESS_ACTION_PTR_READ_CALL, 0 },
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_PTR_READ,
    PIC_ACCESS_FACT_PTR_PTR,
    PIC_ACCESS_FACT_PTR_PTR | PIC_ACCESS_FACT_PTR_READ_CALL,
    PIC_ACCESS_ACTION_PTR_READ_PTR, 0 },
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_PTR_READ,
    PIC_ACCESS_FACT_PTR_LOOKUP,
    PIC_ACCESS_FACT_PTR_LOOKUP | PIC_ACCESS_FACT_PTR_READ_CALL,
    PIC_ACCESS_ACTION_PTR_READ_LOOKUP, 0 },
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_PTR_READ,
    0u,
    PIC_ACCESS_FACT_PTR_READ_CALL | PIC_ACCESS_FACT_PTR_PTR
      | PIC_ACCESS_FACT_PTR_LOOKUP,
    PIC_ACCESS_ACTION_PTR_READ_LINEAR, 0 },
  /* address-of a chunked element: fold the element offset through the map */
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_ADDR_OF, PIC_ACCESS_FACT_AOP,
    PIC_ACCESS_FACT_AOP, PIC_ACCESS_ACTION_ADDR_OF_SLOT, 0 },
  /* assign-family class predicates */
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_ASSIGN_CLASS,
    PIC_ACCESS_FACT_RUNTIME_PTR_CLASS,
    PIC_ACCESS_FACT_RUNTIME_PTR_CLASS,
    PIC_ACCESS_ACTION_ASSIGN_RUNTIME_PTR_CLASS, 0 },
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_ASSIGN_CLASS,
    PIC_ACCESS_FACT_PTR_CLASS,
    PIC_ACCESS_FACT_PTR_CLASS | PIC_ACCESS_FACT_RUNTIME_PTR_CLASS,
    PIC_ACCESS_ACTION_ASSIGN_PTR_CLASS, 0 },
  /* bind destination: slot (chunked or not) vs plain */
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_BIND_DEST,
    PIC_ACCESS_FACT_AOP | PIC_ACCESS_FACT_CHUNKED,
    PIC_ACCESS_FACT_AOP | PIC_ACCESS_FACT_CHUNKED,
    PIC_ACCESS_ACTION_BIND_SLOT_CHUNKED, 0 },
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_BIND_DEST,
    PIC_ACCESS_FACT_AOP,
    PIC_ACCESS_FACT_AOP | PIC_ACCESS_FACT_CHUNKED,
    PIC_ACCESS_ACTION_BIND_SLOT, 0 },
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_BIND_DEST,
    0u, PIC_ACCESS_FACT_AOP,
    PIC_ACCESS_ACTION_BIND_PLAIN, 0 },
  /* fsr walk */
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_FSR_WALK,
    PIC_ACCESS_FACT_RUNTIME_MAP,
    PIC_ACCESS_FACT_RUNTIME_MAP,
    PIC_ACCESS_ACTION_FSR_WALK_RUNTIME, 0 },
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_FSR_WALK,
    PIC_ACCESS_FACT_HOLED,
    PIC_ACCESS_FACT_RUNTIME_MAP | PIC_ACCESS_FACT_HOLED,
    PIC_ACCESS_ACTION_FSR_WALK_HOLED, 0 },
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_FSR_WALK,
    0u, PIC_ACCESS_FACT_RUNTIME_MAP | PIC_ACCESS_FACT_HOLED,
    PIC_ACCESS_ACTION_FSR_WALK_PLAIN, 0 },
  /* fsr dynamic index */
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_FSR_DYNAMIC,
    PIC_ACCESS_FACT_RUNTIME_MAP,
    PIC_ACCESS_FACT_RUNTIME_MAP,
    PIC_ACCESS_ACTION_FSR_DYNAMIC_RUNTIME, 0 },
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_FSR_DYNAMIC,
    PIC_ACCESS_FACT_SV_VIEW,
    PIC_ACCESS_FACT_RUNTIME_MAP | PIC_ACCESS_FACT_SV_VIEW,
    PIC_ACCESS_ACTION_FSR_DYNAMIC_SV, 0 },
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_FSR_DYNAMIC,
    0u, PIC_ACCESS_FACT_RUNTIME_MAP | PIC_ACCESS_FACT_SV_VIEW,
    PIC_ACCESS_ACTION_FSR_DYNAMIC_PLAIN, 0 },
  /* fsr bind */
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_FSR_BIND,
    PIC_ACCESS_FACT_SV_VIEW,
    PIC_ACCESS_FACT_SV_VIEW,
    PIC_ACCESS_ACTION_FSR_BIND_SV, 0 },
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_FSR_BIND,
    PIC_ACCESS_FACT_HOLED,
    PIC_ACCESS_FACT_SV_VIEW | PIC_ACCESS_FACT_HOLED,
    PIC_ACCESS_ACTION_FSR_BIND_HOLED, 0 },
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_FSR_BIND,
    0u, PIC_ACCESS_FACT_SV_VIEW | PIC_ACCESS_FACT_HOLED,
    PIC_ACCESS_ACTION_FSR_BIND_PLAIN, 0 },
  /* indirect family: the slot (array-of-pointers) shape */
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_INDIRECT_SLOT,
    PIC_ACCESS_FACT_AOP | PIC_ACCESS_FACT_SLOT_POINTEE,
    PIC_ACCESS_FACT_AOP | PIC_ACCESS_FACT_SLOT_POINTEE,
    PIC_ACCESS_ACTION_SLOT_ARRAY, 0 },
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_INDIRECT_SLOT,
    PIC_ACCESS_FACT_AOP,
    PIC_ACCESS_FACT_AOP | PIC_ACCESS_FACT_SLOT_POINTEE,
    PIC_ACCESS_ACTION_SLOT_SCALAR, 0 },
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_INDIRECT_SLOT,
    0u, PIC_ACCESS_FACT_AOP,
    PIC_ACCESS_ACTION_SLOT_NOT, 0 },
  /* a slot store is never a fat bind with the engine off (the G3 FAT_BIND row
   * above wins for BOTH slot shapes under the engine; this row is last so the
   * other family's indices do not move) */
  { PIC_ACCESS_KIND_ANY, PIC_ACCESS_OP_BIND, PIC_ACCESS_FACT_AOP,
    PIC_ACCESS_FACT_AOP, PIC_ACCESS_ACTION_NOT_FAT_BIND, 0 }
};

access_class_t pic_access_neutral_class(pic_access_kind_t kind)
{
  switch (kind) {
    case PIC_ACCESS_DIRECT:       return ACCESS_CLASS_DIRECT;
    case PIC_ACCESS_LINEAR:       return ACCESS_CLASS_LINEAR;
    case PIC_ACCESS_RESOLVE:      return ACCESS_CLASS_RESOLVE;
    case PIC_ACCESS_WALK_BAKED:
    case PIC_ACCESS_WALK_RUNTIME: return ACCESS_CLASS_WALK;
    case PIC_ACCESS_FAT:          return ACCESS_CLASS_TAG_DISPATCH;
    case PIC_ACCESS_REFUSE:       break;
  }
  return ACCESS_CLASS_REFUSE;
}

void pic_access_recipe_audit(pfile_t *pf)
{
  static boolean_t done;
  size_t ii;

  if (done) {
    return;
  }
  done = BOOLEAN_TRUE;
  for (ii = 0;
      ii < sizeof(pic_access_recipes) / sizeof(pic_access_recipes[0]);
      ii++) {
    access_recipe_t probe;

    probe.kind       = pic_access_recipes[ii].kind;
    probe.operation  = pic_access_recipes[ii].operation;
    probe.facts      = pic_access_recipes[ii].facts;
    probe.facts_mask = pic_access_recipes[ii].facts_mask;
    probe.run        = 0;
    if (!access_recipe_row_consistent(&probe)) {
      pfile_log(pf, PFILE_LOG_ERR,
          "internal error: inconsistent access recipe row %u",
          (unsigned) ii);
    }
  }
}

const pic_access_recipe_t *pic_access_recipe_find(const pic_access_key_t *key)
{
  size_t ii;

  for (ii = 0;
      ii < sizeof(pic_access_recipes) / sizeof(pic_access_recipes[0]);
      ii++) {
    const pic_access_recipe_t *recipe;

    recipe = &pic_access_recipes[ii];
    if (((PIC_ACCESS_KIND_ANY == recipe->kind)
          || (recipe->kind == key->kind))
        && (recipe->operation == key->operation)
        && ((key->facts & recipe->facts_mask) == recipe->facts)) {
      if (ii < PIC_ACCESS_RECIPE_MAX) {
        pic_access_recipe_seen |= 1UL << ii;
      }
      return recipe;
    }
  }
  return 0;
}

static void pic_access_report(void)
{
  unsigned long ii;
  unsigned long value_live;
  unsigned long var_live;

  if (pic_env_on("JALV2_ACCESS_STATS")) {
    /* C3: both stores realloc to `id + 1` on each new id; the growth
     * test asserts the capacity passed the old one-record ceiling and
     * that the live records still match a fresh computation (VERIFY). */
    value_live = 0;
    for (ii = 0; ii < pic_access_entries_ct; ii++) {
      if (pic_access_entries[ii].valid
          && (pic_access_entries[ii].generation == pic_access_generation)) {
        value_live++;
      }
    }
    var_live = 0;
    for (ii = 0; ii < pic_access_var_entries_ct; ii++) {
      if (pic_access_var_entries[ii].valid
          && (pic_access_var_entries[ii].generation == pic_access_generation)) {
        var_live++;
      }
    }
    fprintf(stderr,
        "jal access stats: value=%lu/%lu var=%lu/%lu stores=%lu/%lu\n",
        value_live, (unsigned long) pic_access_entries_ct,
        var_live, (unsigned long) pic_access_var_entries_ct,
        pic_access_value_stores, pic_access_var_stores);
  }
  if (pic_env_on("JALV2_ACCESS_COVERAGE")) {
    /* A12: one bit per recipe row that matched at least once.  The
     * matrix gate unions the bitmap over the corpus and asserts every
     * row is exercised. */
    fprintf(stderr, "jal access coverage: rows=%u bitmap=%lx\n",
        (unsigned)
          (sizeof(pic_access_recipes) / sizeof(pic_access_recipes[0])),
        pic_access_recipe_seen);
  }
}

static void pic_access_report_arm(void)
{
  if (!pic_access_report_armed
      && (pic_env_on("JALV2_ACCESS_STATS")
        || pic_env_on("JALV2_ACCESS_COVERAGE"))) {
    pic_access_report_armed = BOOLEAN_TRUE;
    (void) atexit(pic_access_report);
  }
}

boolean_t pic_access_plan_load(const value_t val, pic_access_plan_t *plan)
{
  unsigned id;

  if ((0 == val) || !value_access_valid_get(val)) {
    return BOOLEAN_FALSE;
  }
  if (value_access_generation_get(val) != pic_access_generation) {
    return BOOLEAN_FALSE;
  }
  id = value_id_get(val);
  if ((0 == id) || ((size_t) id >= pic_access_entries_ct)
      || !pic_access_entries[id].valid
      || (pic_access_entries[id].generation != pic_access_generation)) {
    return BOOLEAN_FALSE;
  }
  *plan = pic_access_entries[id].plan;
  return BOOLEAN_TRUE;
}

boolean_t pic_access_var_load(variable_t var, pic_access_plan_t *plan)
{
  unsigned id;

  if ((VARIABLE_NONE == var) || (0 == var)) {
    return BOOLEAN_FALSE;
  }
  id = variable_id_get(var);
  if ((0 == id) || ((size_t) id >= pic_access_var_entries_ct)
      || !pic_access_var_entries[id].valid
      || (pic_access_var_entries[id].generation != pic_access_generation)) {
    return BOOLEAN_FALSE;
  }
  *plan = pic_access_var_entries[id].plan;
  return BOOLEAN_TRUE;
}

void pic_access_var_store(pfile_t *pf, variable_t var,
    const pic_access_plan_t *plan)
{
  unsigned id;
  size_t   want;
  size_t   ii;
  pic_access_entry_t *grown;

  pic_access_report_arm();
  if ((VARIABLE_NONE == var) || (0 == var) || (0 == plan)) {
    return;
  }
  id = variable_id_get(var);
  if (0 == id) {
    return;
  }
  pic_access_var_stores++;
  want = (size_t) id + 1;
  if (want > pic_access_var_entries_ct) {
    grown = (pic_access_entry_t *) realloc(pic_access_var_entries,
        want * sizeof(pic_access_entry_t));
    if (pic_env_on("JALV2_ACCESS_STORE_FAULT")) {
      /* A11/S1: act as if this realloc had failed */
      if (0 != grown) {
        free(grown);
      }
      grown = 0;
    }
    if (0 == grown) {
      /* C4/S1: the dispatch-feeding store is fatal on allocation
       * failure -- no safe continuation exists with a poisoned key. */
      free(pic_access_var_entries);
      pic_access_var_entries = 0;
      pic_access_var_entries_ct = 0;
      pfile_log_syserr(pf, ENOMEM);
      return;
    }
    for (ii = pic_access_var_entries_ct; ii < want; ii++) {
      grown[ii].generation = 0;
      grown[ii].valid = BOOLEAN_FALSE;
    }
    pic_access_var_entries = grown;
    pic_access_var_entries_ct = want;
  }
  pic_access_var_entries[id].generation = pic_access_generation;
  pic_access_var_entries[id].valid = BOOLEAN_TRUE;
  pic_access_var_entries[id].plan = *plan;
}

unsigned pic_access_var_action_get2(pfile_t *pf, const char *site,
    variable_t var, unsigned op, variable_t *bind_walk)
{
  pic_access_plan_t plan;
  pic_access_key_t  key;
  const pic_access_recipe_t *recipe;

  pic_access_plan_fill_var_at(pf, site, var, &plan);
  if (bind_walk) {
    *bind_walk = plan.bind_walk;
  }
  pic_access_key_fill(&plan, op, &key);
  recipe = pic_access_recipe_find(&key);
  pic_access_explain_note(pf, site, var, &plan, op, recipe);
  return (0 != recipe) ? recipe->action : PIC_ACCESS_ACTION_NONE;
}

unsigned pic_access_var_action_get(pfile_t *pf, const char *site,
    variable_t var, unsigned op)
{
  return pic_access_var_action_get2(pf, site, var, op, 0);
}

unsigned pic_access_value_action_get(pfile_t *pf, const char *site,
    const value_t val, unsigned op)
{
  pic_access_plan_t plan;
  pic_access_key_t  key;
  const pic_access_recipe_t *recipe;

  pic_access_plan_get(pf, val, &plan);
  pic_access_key_fill(&plan, op, &key);
  recipe = pic_access_recipe_find(&key);
  pic_access_explain_note(pf, site, value_variable_get(val), &plan, op,
      recipe);
  return (0 != recipe) ? recipe->action : PIC_ACCESS_ACTION_NONE;
}

unsigned pic_access_value_facts_get(pfile_t *pf, const value_t val,
    variable_t *bind_walk)
{
  pic_access_plan_t plan;
  pic_access_key_t  key;

  pic_access_plan_get(pf, val, &plan);
  if (bind_walk) {
    *bind_walk = plan.bind_walk;
  }
  pic_access_key_fill(&plan, PIC_ACCESS_OP_INDIRECT_SLOT, &key);
  return key.facts;
}

void pic_access_plan_store(pfile_t *pf, const value_t val,
    const pic_access_plan_t *plan)
{
  unsigned id;
  size_t   want;
  size_t   ii;
  pic_access_entry_t *grown;

  pic_access_report_arm();
  if ((0 == val) || (0 == plan)) {
    return;
  }
  id = value_id_get(val);
  if (0 == id) {
    return;
  }
  pic_access_value_stores++;
  want = (size_t) id + 1;
  if (want > pic_access_entries_ct) {
    grown = (pic_access_entry_t *) realloc(pic_access_entries,
        want * sizeof(pic_access_entry_t));
    if (pic_env_on("JALV2_ACCESS_STORE_FAULT")) {
      /* A11/S1: act as if this realloc had failed */
      if (0 != grown) {
        free(grown);
      }
      grown = 0;
    }
    if (0 == grown) {
      /* C4/S1: the dispatch-feeding store is fatal on allocation
       * failure -- no safe continuation exists with a poisoned key. */
      free(pic_access_entries);
      pic_access_entries = 0;
      pic_access_entries_ct = 0;
      pfile_log_syserr(pf, ENOMEM);
      return;
    }
    for (ii = pic_access_entries_ct; ii < want; ii++) {
      grown[ii].generation = 0;
      grown[ii].valid = BOOLEAN_FALSE;
    }
    pic_access_entries = grown;
    pic_access_entries_ct = want;
  }
  pic_access_entries[id].generation = pic_access_generation;
  pic_access_entries[id].valid = BOOLEAN_TRUE;
  pic_access_entries[id].plan = *plan;
  value_access_record_set(val, (unsigned) plan->kind,
      pic_access_generation);
}

boolean_t pic_access_record_on(void)
{
  return access_record_on();
}

void pic_access_dump_set(const char *path)
{
  access_record_set_path(path);
}

/* the classic columns after (program,generation,site,var) */
static const char *const pic_access_columns[] = {
  "ordinal", "kind", "aop", "chunked", "holed", "sv_view", "runtime_map",
  "value_map", "ptr_lookup", "ptr_read_call", "line"
};

void pic_access_record_begin(pfile_t *pf, const char *program)
{
  access_record_begin(pf, program, pic_access_columns,
      (unsigned) (sizeof(pic_access_columns)
        / sizeof(pic_access_columns[0])));
}

void pic_access_record_plan(pfile_t *pf, const char *site,
    variable_t var, const pic_access_plan_t *plan)
{
  const char *name;
  const char *file;
  char        site_id[PIC_ACCESS_SITE_MAX];
  unsigned long vals[11];
  unsigned    line;

  if (!pic_access_record_on() || (0 == plan)
      || (VARIABLE_NONE == var) || (0 == var)) {
    return;
  }
  name = variable_name_get(var);
  pic_access_site_id(pf, site, site_id, sizeof(site_id));
  pic_access_origin_get(pf, &file, &line);
  vals[0]  = (unsigned long) pic_access_ordinal++;
  vals[1]  = (unsigned long) plan->kind;
  vals[2]  = (unsigned long) plan->aop;
  vals[3]  = (unsigned long) plan->chunked;
  vals[4]  = (unsigned long) plan->holed;
  vals[5]  = (unsigned long) plan->sv_view;
  vals[6]  = (unsigned long) plan->runtime_map;
  vals[7]  = (unsigned long) plan->value_map;
  vals[8]  = (unsigned long) plan->ptr_lookup;
  vals[9]  = (unsigned long) plan->ptr_read_call;
  vals[10] = (unsigned long) line;
  access_record_row(pf, site_id, name, vals,
      (unsigned) (sizeof(vals) / sizeof(vals[0])));
}

void pic_access_generation_bump(void)
{
  pic_access_generation++;
  access_record_generation_bump();
}

unsigned long pic_access_generation_get(void)
{
  return pic_access_generation;
}

void pic_access_reset(void)
{
  access_record_reset();
  free(pic_access_entries);
  pic_access_entries = 0;
  pic_access_entries_ct = 0;
  free(pic_access_var_entries);
  pic_access_var_entries = 0;
  pic_access_var_entries_ct = 0;
  pic_access_generation = 0;
  pic_access_value_stores = 0;
  pic_access_var_stores = 0;
  pic_access_recipe_seen = 0;
  /* pic_access_report_armed stays: atexit cannot be cancelled */
}
