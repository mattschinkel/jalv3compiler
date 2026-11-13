/************************************************************
 **
 ** access_record.c : the target-neutral access RECORDER (ENG A4)
 **
 ** See access_record.h.  The row format is
 **
 **   # program,generation,site,var,<headers...>
 **   <program>,<generation>,<site>,<var>,<vals...>
 **
 ** so a back end's rows are self-describing and the classic CSV is
 ** exactly the classic header plus the classic values (the access
 ** matrix gate proves the bytes did not move).
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/

#include <stdio.h>
#include <string.h>

#include "../libutils/types.h"
#include "access_record.h"
#include "pfile.h"
#include "pf_msg.h"

#define ACCESS_RECORD_PROGRAM_MAX 96

static const char *access_record_dump_path;
static boolean_t   access_record_checked;
static boolean_t   access_record_is_on;
static boolean_t   access_record_broken;
static FILE       *access_record_log;
static char        access_record_program[ACCESS_RECORD_PROGRAM_MAX];
static unsigned long access_record_generation;

/* the strict on/off rule: unset/empty = off; 1/true/yes/on = on;
 * 0/false/no/off = off; anything else warns once and uses off (the
 * shared parser lives in libpic12, so this module keeps its own copy of
 * the accepted words) */
static boolean_t access_record_env_on(const char *name)
{
  const char *v;
  char        buf[16];
  size_t      n;
  size_t      ii;

  v = getenv(name);
  if ((0 == v) || (0 == *v)) {
    return BOOLEAN_FALSE;
  }
  n = strlen(v);
  if (n >= sizeof(buf)) {
    n = sizeof(buf) - 1;
  }
  for (ii = 0; ii < n; ii++) {
    char c;

    c = v[ii];
    buf[ii] = ((c >= 'A') && (c <= 'Z')) ? (char) (c + ('a' - 'A')) : c;
  }
  buf[n] = 0;
  if ((0 == strcmp(buf, "1")) || (0 == strcmp(buf, "true"))
      || (0 == strcmp(buf, "yes")) || (0 == strcmp(buf, "on"))) {
    return BOOLEAN_TRUE;
  }
  if ((0 == strcmp(buf, "0")) || (0 == strcmp(buf, "false"))
      || (0 == strcmp(buf, "no")) || (0 == strcmp(buf, "off"))) {
    return BOOLEAN_FALSE;
  }
  fprintf(stderr,
      "JALV2-SWITCH: %s='%s' is not on/off; using off "
      "(doc/designs/one-access-model.md#feature-switches)\n", name, v);
  return BOOLEAN_FALSE;
}

boolean_t access_record_on(void)
{
  if (!access_record_checked) {
    access_record_checked = BOOLEAN_TRUE;
    access_record_is_on  = access_record_env_on("JALV2_ACCESS_RECORD");
  }
  return access_record_is_on;
}

void access_record_set_path(const char *path)
{
  access_record_dump_path = path;
  access_record_checked  = BOOLEAN_TRUE;
  access_record_is_on    = BOOLEAN_TRUE;
}

void access_record_begin(pfile_t *pf, const char *program,
    const char *const *headers, unsigned ncols)
{
  const char *path;
  unsigned    ii;

  if (!access_record_on() || access_record_log) {
    return;
  }
  path = access_record_dump_path;
  if ((0 == path) || (0 == *path)) {
    path = getenv("JALV2_ACCESS_RECORD_OUT");
  }
  if ((0 == path) || (0 == *path)) {
    path = "plan.log";
  }
  access_record_log = fopen(path, "a");
  if (0 == access_record_log) {
    /* the recorder was requested, so a log that cannot be written is an
     * error, not a silent degradation */
    pfile_log_force(pf, PFILE_LOG_ERR,
        "internal error: access recorder cannot open %s", path);
    return;
  }
  if (program) {
    strncpy(access_record_program, program, ACCESS_RECORD_PROGRAM_MAX - 1);
    access_record_program[ACCESS_RECORD_PROGRAM_MAX - 1] = 0;
  }
  fprintf(access_record_log, "# program,generation,site,var");
  for (ii = 0; ii < ncols; ii++) {
    fprintf(access_record_log, ",%s", headers[ii]);
  }
  fputc('\n', access_record_log);
}

void access_record_row(pfile_t *pf, const char *site, const char *var,
    const unsigned long *vals, unsigned ncols)
{
  unsigned ii;

  if (!access_record_on() || (0 == access_record_log) || access_record_broken) {
    return;
  }
  fprintf(access_record_log, "%s,%lu,%s,%s",
      access_record_program, access_record_generation,
      site ? site : "-", var ? var : "-");
  for (ii = 0; ii < ncols; ii++) {
    fprintf(access_record_log, ",%lu", vals[ii]);
  }
  fputc('\n', access_record_log);
  if (access_record_env_on("JALV2_ACCESS_LOG_FAULT")
      || (0 != ferror(access_record_log))) {
    /* while recording, a write failure is a hard error.  Report once and
     * stop writing; the error count fails the compile. */
    access_record_broken = BOOLEAN_TRUE;
    pfile_log_force(pf, PFILE_LOG_ERR,
        "internal error: access recorder write failed");
  }
}

void access_record_generation_bump(void)
{
  access_record_generation++;
}

unsigned long access_record_generation_get(void)
{
  return access_record_generation;
}

void access_record_reset(void)
{
  if (access_record_log) {
    fclose(access_record_log);
  }
  access_record_log       = 0;
  access_record_broken    = BOOLEAN_FALSE;
  access_record_generation = 0;
  access_record_program[0] = 0;
  access_record_dump_path = 0;
  access_record_checked   = BOOLEAN_FALSE;
  access_record_is_on     = BOOLEAN_FALSE;
}
