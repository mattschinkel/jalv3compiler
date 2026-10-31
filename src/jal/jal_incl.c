/************************************************************
 **
 ** jal_incl.c : JAL include and pragma definitions
 **
 ** Copyright (c) 2004-2006, Kyle A. York
 **               2018-2020, Rob Jansen
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: fork extensions (heavy rework)
 **
 ************************************************************/
#include <string.h>
#include <errno.h>
#include <assert.h>
#include "../libutils/mem.h"
#include "../libcore/pf_cmd.h"
#include "../libcore/pf_msg.h"
#include "../libcore/pf_proc.h"
#include "../libpic12/pic.h"
#include "jal_expr.h"
#include "jal_tokn.h"
#include "jal_vdef.h"
#include "jal_incl.h"
#include "../libcore/pfiled.h"
#include "../libcore/label.h"

/* list of known jal fuses */

jal_pragma_fuses_t *jal_fuses;


void jal_parse_include(pfile_t *pf, const pfile_pos_t *statement_start)
{
  char include_name[FILENAME_MAX];
  const char *ptr;
  size_t      ii;
  size_t      jj;

  UNUSED(statement_start);
  /* includes are COMPILE-TIME: inside a runtime-conditioned block (or a
   * loop body -- always block-scoped) the old behavior silently included
   * unconditionally with the declarations trapped block-local.  Clean
   * error instead (const conditions and defined() folds are fine -- they
   * never reach this depth).  The include is still processed after the
   * error so parsing recovers in one message. */
  if (pfile_runtime_block_test(pf)) {
    pfile_log(pf, PFILE_LOG_ERR,
        "include inside a runtime conditional or loop is not supported"
        " -- include runs at compile time; use a constant condition or"
        " defined()");
  }
  ptr = jal_token_parse_raw(pf);
  /* remove any prepended spaces */
  for (ii = 0;
       ISSPACE(ptr[ii]);
       ii++)
    ; /* empty body */

  /* grab the string either to the comment or to EOS */
  /* sizeof(include_name) - 5 --> 1 (EOS) 4 ('.JAL') */
  for (jj = 0; 
       (jj < sizeof(include_name) - 5)
       && ptr[ii]
       && (';' != ptr[ii])
       && !(('-' == ptr[ii]) && ('-' == ptr[ii + 1]));
       ii++, jj++) {
    include_name[jj] = ptr[ii];
  }
  /* remove any appended spaces */
  while (jj && ISSPACE(include_name[jj - 1])) {
    jj--;
  }
  /* nul terminate the string */
  include_name[jj] = 0;
  if (jj) {
    strcpy(include_name + jj, ".jal");
    pfile_include_process(pf, strlen(include_name), include_name);
  } else {
    pfile_log(pf, PFILE_LOG_ERR, "filename expected!");
  }
  pf_token_get(pf, PF_TOKEN_NEXT);
}

/* check that the currently compiled file has the correct name.
   of what possible use is this??? */
static void jal_parse_pragma_name(pfile_t *pf)
{
  pfile_source_t *src;
  const char     *name;
  const char     *ptr;
  size_t          sz;

  src  = pfile_source_get(pf);
  name = pfile_source_name_get(src);
  for (sz = strlen(name);
       sz && ('/' != name[sz-1]) && ('\\' != name[sz-1]);
       sz--)
    ; /* null body */
  ptr = pf_token_get(pf, PF_TOKEN_CURRENT);
  while (*ptr && name[sz] && (*ptr == name[sz])) {
    sz++;
    ptr++;
  }
  if (*ptr || ('.' != name[sz])) {
    pfile_log(pf, PFILE_LOG_ERR, "wrong file");
  }
  pf_token_get(pf, PF_TOKEN_NEXT);
}

/* force an error to occur */
static void jal_parse_pragma_error(pfile_t *pf)
{
  if (!pfile_codegen_disable_get(pf)) {
    const char *ptr;

    ptr = jal_token_parse_raw(pf);
    while (ISSPACE(*ptr)) {
      ptr++;
    }
    if (!*ptr) {
      ptr = "pragma error failed";
    }

    pfile_log(pf, PFILE_LOG_ERR, "%s", ptr);
    pf_token_get(pf, PF_TOKEN_NEXT);
  }
}

/* this is analogous to:
   const target_chip = pic_* */
static void jal_parse_pragma_target_chip(pfile_t *pf)
{
  const char *ptr;
  char       *chip;

  ptr = pf_token_get(pf, PF_TOKEN_CURRENT);
  chip = MALLOC(5 + strlen(ptr));
  if (!chip) {
    pfile_log_syserr(pf, ENOMEM);
  } else {
    value_t src;

    sprintf(chip, "pic_%s", ptr);
    src = pfile_value_find(pf, PFILE_LOG_ERR, chip);
    if (src) {
      if (!value_is_const(src)) {
        pfile_log(pf, PFILE_LOG_ERR, "pic_* must all be constant");
      } else {
        jal_variable_info_t inf;
        value_t            *name;
        size_t              ii;
        variable_def_t      vdef;

        jal_variable_info_init(&inf);
        inf.name      = "target_chip";
        inf.ct        = 0;
        inf.def_flags = VARIABLE_DEF_FLAG_CONST;

        variable_release(
            jal_variable_alloc(pf, &inf, BOOLEAN_FALSE, 1, &src,
              PFILE_VARIABLE_ALLOC_GLOBAL));

        /* also, create 'target_chip_name', an array of bytes that will hold
         * the chip name (for inclusion in the assembly file)
         */
        inf.name      = "target_chip_name";
        inf.ct        = strlen(ptr);
        vdef = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
            VARIABLE_DEF_FLAG_NONE, 1);
        inf.vdef = vdef;
        
        name = MALLOC(sizeof(*name) * inf.ct);
        if (!name) {
          pfile_log_syserr(pf, ENOMEM);
        } else {
          for (ii = 0; ii < inf.ct; ii++) {
            name[ii] = pfile_constant_get(pf, ptr[ii], VARIABLE_DEF_NONE);
          }
          variable_release(
              jal_variable_alloc(pf, &inf, BOOLEAN_FALSE, inf.ct,
                name, PFILE_VARIABLE_ALLOC_GLOBAL));
          for (ii = 0; ii < inf.ct; ii++) {
            value_release(name[ii]);
          }
        }
        FREE(name);
      }
      value_release(src);
    }
    FREE(chip);
  }
  pf_token_get(pf, PF_TOKEN_NEXT);
}

static void jal_pragma_variable_assign(pfile_t *pf, char *vname)
{
  value_t cexpr;

  cexpr = jal_parse_expr(pf);
  if (!cexpr) {
    /* Check if this is a fuse constant */
    const char *token_name = pf_token_ptr_get(pf);
    const jal_pragma_fuses_t *fuses;
    const jal_pragma_fuses_entry_t *entry;
    boolean_t found = BOOLEAN_FALSE;
    
    for (fuses = jal_fuses; fuses && !found; fuses = fuses->link) {
      for (entry = fuses->entry; entry && !found; entry = entry->link) {
        if (strcmp(entry->name, token_name) == 0) {
          /* Found a pragma constant - create a constant value */
          cexpr = pfile_value_temp_get(pf, VARIABLE_DEF_TYPE_INTEGER, 1);
          if (cexpr) {
            value_const_set(cexpr, entry->bits);
            found = BOOLEAN_TRUE;
            pf_token_get(pf, PF_TOKEN_NEXT); /* consume the token */
          }
        }
      }
    }
  }
  
  if (cexpr) {
    if (!value_is_const(cexpr)) {
      pfile_log(pf, PFILE_LOG_ERR, PFILE_MSG_CONSTANT_EXPECTED);
    } else {
      jal_variable_info_t inf;

      jal_variable_info_init(&inf);
      inf.name      = vname;
      inf.ct        = 0;
      inf.def_flags = VARIABLE_DEF_FLAG_CONST;

      variable_release(
          jal_variable_alloc(pf, &inf, BOOLEAN_FALSE, 1, &cexpr,
            PFILE_VARIABLE_ALLOC_GLOBAL));
    }
    value_release(cexpr);
  }
}

static void jal_pragma_const_assign(pfile_t *pf, char *vname,
    variable_const_t n)
{
  value_t cexpr;

  cexpr = pfile_constant_get(pf, n, VARIABLE_DEF_NONE);
  if (cexpr) {
    jal_variable_info_t inf;

    jal_variable_info_init(&inf);
    inf.name      = vname;
    inf.ct        = 0;
    inf.def_flags = VARIABLE_DEF_FLAG_CONST;
    variable_release(
        jal_variable_alloc(pf, &inf, BOOLEAN_FALSE, 1, &cexpr,
          PFILE_VARIABLE_ALLOC_GLOBAL));
    value_release(cexpr);
  }
}


/* Record which fuse fields the user has set, for the 32-bit back ends:
 * `pragma target <fuse_def entry> <value>` applies its bits to _FUSES
 * AND ORs its field mask into _FUSES_EDIT[word]; the hex writer merges
 * exactly the marked fields into the solved DEVCFG words (the solved
 * words would otherwise overwrite the user's bits, and diffing _FUSES
 * against defaults cannot reconstruct the touched fields).  Classic
 * device files declare no _FUSES_EDIT, so this is a no-op there. */
static void jal_fuse_edit_mark(pfile_t *pf, variable_ct_t word,
  unsigned mask)
{
  value_t edits;

  edits = pfile_value_find(pf, PFILE_LOG_NONE, "_fuses_edit");
  if (edits) {
    if (value_is_array(edits) && (word < value_ct_get(edits))) {
      value_t e;

      e = value_subscript_set(edits, word);
      if (e) {
        value_const_set(e, value_const_get(e) | (variable_const_t) mask);
        value_release(e);
      }
    }
    value_release(edits);
  }
}


/* this is analogous to:
   const target_clock = cexpr */
static void jal_parse_pragma_target_cpu(pfile_t *pf)
{
  jal_pragma_variable_assign(pf, "target_cpu");
}
static void jal_parse_pragma_target_flash_mode(pfile_t *pf)
{
  jal_pragma_variable_assign(pf, "_flash_mode");
}
static void jal_parse_pragma_target_flash_size(pfile_t *pf)
{
  jal_pragma_variable_assign(pf, "_flash_size");
}
static void jal_parse_pragma_target_flash_freq(pfile_t *pf)
{
  jal_pragma_variable_assign(pf, "_flash_freq");
}

static void jal_parse_pragma_target_clock(pfile_t *pf)
{
  jal_pragma_variable_assign(pf, "target_clock");
  pic32_clock_solve_maybe(pf);   /* OSC-then-clock order re-solves */
}

static void jal_parse_pragma_target_bank(pfile_t *pf)
{
  jal_pragma_variable_assign(pf, "target_bank_size");
}

/* The PIC instruction set is normalized in the device file to a number. 
   only special cases are handled by the compiler, not all. */
static void jal_parse_pragma_target_instruction_set(pfile_t* pf)
{
    jal_pragma_variable_assign(pf, "target_instruction_set");
}

static void jal_parse_pragma_target_page(pfile_t *pf)
{
  jal_pragma_variable_assign(pf, "target_page_size");
}

/* Legacy `pragma string heap <expr>` — ignored (no diagnostic). Heap arena size
 * is only `const word _string_heap_size = <bytes>` in user source. */
static void jal_parse_pragma_string_heap(pfile_t *pf)
{
  value_t cexpr;

  cexpr = jal_parse_expr(pf);
  value_release(cexpr);
}

static void jal_parse_pragma_string_index(pfile_t *pf)
{
  static const struct {
    const char       *name;
    variable_const_t  size;
  } map[] = {
    {"auto",  0},
    {"byte",  1},
    {"word",  2}
  };
  size_t ii;

  for (ii = 0; (ii < COUNT(map))
      && !pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, map[ii].name);
      ii++)
    ;
  if (ii == COUNT(map)) {
    pfile_log(pf, PFILE_LOG_ERR,
        "expected auto, byte, or word");
    pf_token_get(pf, PF_TOKEN_NEXT);
  } else {
    jal_pragma_const_assign(pf, "_string_index_size", map[ii].size);
    pf_token_get(pf, PF_TOKEN_NEXT);
  }
}

/* pragma string alloc first_fit | best_fit */
static void jal_parse_pragma_string_alloc(pfile_t *pf)
{
  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "first_fit")) {
    pf->jal_str_heap_alloc_policy = 0U;
    pf_token_get(pf, PF_TOKEN_NEXT);
  } else if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "best_fit")) {
    pf->jal_str_heap_alloc_policy = 1U;
    pf_token_get(pf, PF_TOKEN_NEXT);
  } else {
    pfile_log(pf, PFILE_LOG_ERR,
        "expected first_fit or best_fit (pragma string alloc)");
    pf_token_get(pf, PF_TOKEN_NEXT);
  }
}

/* pragma string retry on | off — after failed alloc, one string_heap_compact + retry */
static void jal_parse_pragma_string_retry(pfile_t *pf)
{
  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "on")) {
    pf->jal_str_heap_alloc_retry = 1U;
    pf_token_get(pf, PF_TOKEN_NEXT);
  } else if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "off")) {
    pf->jal_str_heap_alloc_retry = 0U;
    pf_token_get(pf, PF_TOKEN_NEXT);
  } else {
    pfile_log(pf, PFILE_LOG_ERR, "expected on or off (pragma string retry)");
    pf_token_get(pf, PF_TOKEN_NEXT);
  }
}

static void jal_parse_pragma_target_fuses(pfile_t *pf)
{
  value_t cexpr;

  cexpr = jal_parse_expr(pf);
  if (!cexpr || !value_is_const(cexpr)) {
    pfile_log(pf, PFILE_LOG_ERR, PFILE_MSG_CONSTANT_EXPECTED);
  } else {
    value_t n;

    n = pfile_value_find(pf, PFILE_LOG_ERR, "_fuses");
    if (n) {
      if (value_is_array(n)) {
        if (value_const_get(cexpr) >= value_ct_get(n)) {
          pfile_log(pf, PFILE_LOG_ERR, "constant out of range");
          value_release(n);
          n = VALUE_NONE;
        } else {
          value_t tmp;

          tmp = value_subscript_set(n,
              (variable_ct_t) value_const_get(cexpr));
          /* whole-word user edit: mark every bit for the 32-bit merge */
          jal_fuse_edit_mark(pf,
              (variable_ct_t) value_const_get(cexpr), ~0U);
          value_release(cexpr);
          value_release(n);
          n = tmp;
          cexpr = jal_parse_expr(pf);
          if (!cexpr || !value_is_const(cexpr)) {
            pfile_log(pf, PFILE_LOG_ERR, PFILE_MSG_CONSTANT_EXPECTED);
          }
        }
      }
      if (n) {
        value_const_set(n, value_const_get(cexpr));
        value_release(n);
      }
    }
  }
  value_release(cexpr);
}


static void jal_parse_pragma_target_plldiv(pfile_t *pf)
{
  jal_pragma_variable_assign(pf, "target_plldiv");
}

static void jal_parse_pragma_target_cpudiv(pfile_t *pf)
{
  jal_pragma_variable_assign(pf, "target_cpudiv");
}

static void jal_parse_pragma_target_wdt(pfile_t *pf)
{
  jal_pragma_variable_assign(pf, "target_wdt");
}

static void jal_parse_pragma_target_xinst(pfile_t *pf)
{
  jal_pragma_variable_assign(pf, "target_xinst");
}

static void jal_parse_pragma_target_debug(pfile_t *pf)
{
  jal_pragma_variable_assign(pf, "target_debug");
}

static void jal_parse_pragma_target_fcmem(pfile_t *pf)
{
  jal_pragma_variable_assign(pf, "target_fcmem");
}

static void jal_parse_pragma_target_ieso(pfile_t *pf)
{
  jal_pragma_variable_assign(pf, "target_ieso");
}

/* PIC32 clock selection (docs/investigations/pic32_clock_select_plan.md):
 * `pragma target OSC <mode>` stores the numeric mode in target_osc.
 * The mode names are parser keywords (classic fuse-entry style), NOT
 * device-file consts -- consts in the device file cost RAM/zero-init
 * code on every image (probed 2026-10-07), keywords are free. */
static void jal_parse_pragma_target_osc(pfile_t *pf)
{
  typedef struct pragma_osc_mode_ {
    const char *name;
    unsigned long mode;
  } pragma_osc_mode_t;
  static const pragma_osc_mode_t mips_modes[] = {
    {"frc", 1}, {"frcdiv", 2}, {"frc_spll", 3}, {"posc", 4},
    {"posc_spll", 5}, {"ec", 6}, {"ec_spll", 7}
  };
  static const pragma_osc_mode_t esp32_modes[] = {
    {"crystal", 2}, {"pll", 3}
  };
  const pragma_osc_mode_t *modes;
  size_t mode_ct;
  const char *vocab;
  const char *tok = pf_token_ptr_get(pf);
  value_t cpu;
  size_t i;

  cpu = pfile_value_find(pf, PFILE_LOG_NONE, "target_cpu");
  if (cpu && (PIC_TARGET_CPU_ESP32 == (pic_target_cpu_t) value_const_get(cpu))) {
    modes = esp32_modes;
    mode_ct = COUNT(esp32_modes);
    vocab = "CRYSTAL, PLL";
  } else {
    modes = mips_modes;
    mode_ct = COUNT(mips_modes);
    vocab = "FRC, FRCDIV, FRC_SPLL, POSC, POSC_SPLL, EC, EC_SPLL";
  }
  if (cpu) {
    value_release(cpu);
  }
  for (i = 0; i < mode_ct; i++) {
    if (!strcmp(modes[i].name, tok)) {
      break;
    }
  }
  if (i < mode_ct) {
    pf_token_get(pf, PF_TOKEN_NEXT);
    jal_pragma_const_assign(pf, "target_osc", modes[i].mode);
  } else {
    pfile_log(pf, PFILE_LOG_ERR,
      "unknown oscillator mode: %s (%s)", tok, vocab);
    /* Consume the bad token: pragma handlers leave the stream positioned for
     * the main parser, and a keyword left here is re-parsed as a STATEMENT
     * (`pragma target OSC case hs` became a phantom `case` statement whose
     * half-built expression reached the p-code builder with a NULL operand).
     * Recovery: drop it and let the rest of the line produce its own
     * diagnostics. */
    if (!pf_token_is_eof(pf)) {
      pf_token_get(pf, PF_TOKEN_NEXT);
    }
  }
}

/* PIC32 config pragmas (fix_pic32_clock_select.md addendum 3): classic
 * sample vocabulary (WDT CONTROL/DISABLED, DEBUG DISABLED, MCLR
 * EXTERNAL...) stored as 0/1 consts the clock solver masks into the
 * solved DEVCFG words. Absent pragma = the proven default bits, so
 * images without these pragmas stay byte-identical. Values are parser
 * keywords, keeping the device file symbol-only (probed: device-file
 * consts cost RAM/zero-init on every image). */
static void jal_parse_pragma_flag(pfile_t *pf, char *name,
  const char *vname, unsigned long on_val, unsigned long off_val)
{
  const char *tok = pf_token_ptr_get(pf);

  if (!strcmp("disabled", tok) || !strcmp("internal", tok)) {
    pf_token_get(pf, PF_TOKEN_NEXT);
    jal_pragma_const_assign(pf, name, off_val);
  } else if (!strcmp("enabled", tok) || !strcmp("control", tok)
    || !strcmp("external", tok)) {
    pf_token_get(pf, PF_TOKEN_NEXT);
    jal_pragma_const_assign(pf, name, on_val);
  } else {
    pfile_log(pf, PFILE_LOG_ERR, "unknown %s setting: %s", vname, tok);
  }
}

static void jal_parse_pragma_pic32_wdt(pfile_t *pf)
{
  jal_parse_pragma_flag(pf, "target_wdt", "WDT", 1, 0);
}

static void jal_parse_pragma_pic32_dmt(pfile_t *pf)
{
  jal_parse_pragma_flag(pf, "target_dmt", "DMT", 1, 0);
}

static void jal_parse_pragma_pic32_debug(pfile_t *pf)
{
  jal_parse_pragma_flag(pf, "target_debug", "DEBUG", 1, 0);
}

static void jal_parse_pragma_pic32_ieso(pfile_t *pf)
{
  jal_parse_pragma_flag(pf, "target_ieso", "IESO", 1, 0);
}

static void jal_parse_pragma_pic32_sosc(pfile_t *pf)
{
  jal_parse_pragma_flag(pf, "target_sosc", "SOSC", 1, 0);
}

static void jal_parse_pragma_pic32_mclr(pfile_t *pf)
{
  jal_parse_pragma_flag(pf, "target_mclr", "MCLR", 1, 0);
}

/* `pragma target oscillator <hz>` -- the external source frequency
 * (crystal or clock module); jallib readability: full words, no
 * datasheet abbreviations in sample files. */
static void jal_parse_pragma_target_oscillator(pfile_t *pf)
{
  jal_pragma_variable_assign(pf, "target_oscillator");
}

/* `pragma target image pfm` -- honest-name form of the image shape the
 * 32mz sample kit uses: app in program flash (PFM) at 0x9D000000 plus
 * the compiler-emitted boot block. No bootloader is involved; the
 * legacy `pragma bootloader pic32` predates the own-boot flow and
 * remains fully supported. PIC32-only (the flag/offset semantics are
 * meaningless on classic cores). */
static void jal_parse_pragma_target_image(pfile_t *pf);
static void jal_parse_pragma_loader32(pfile_t *pf);

static void jal_parse_pragma_target_image(pfile_t *pf)
{
  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "pfm")) {
    value_t cpu = pfile_value_find(pf, PFILE_LOG_NONE, "target_cpu");
    boolean_t mips = cpu
      && (PIC_TARGET_CPU_MIPS32 == (pic_target_cpu_t) value_const_get(cpu));

    if (cpu) {
      value_release(cpu);
    }
    if (!mips) {
      pfile_log(pf, PFILE_LOG_ERR,
        "pragma target image pfm requires the PIC32 back end");
    } else {
      pf_token_get(pf, PF_TOKEN_NEXT);
      jal_parse_pragma_loader32(pf);
    }
  } else {
    pfile_log(pf, PFILE_LOG_ERR, "unknown image form: %s (pfm)",
      pf_token_ptr_get(pf));
  }
}

static void jal_parse_pragma_target(pfile_t *pf)
{
  static const struct {
    const char *tag;
    void      (*action)(pfile_t *);
  } targets[] = {
    {"chip",  jal_parse_pragma_target_chip},
    {"clock", jal_parse_pragma_target_clock},
    {"fuses", jal_parse_pragma_target_fuses},
    {"cpu",   jal_parse_pragma_target_cpu},
    {"flash_mode", jal_parse_pragma_target_flash_mode},
    {"flash_size", jal_parse_pragma_target_flash_size},
    {"flash_freq", jal_parse_pragma_target_flash_freq},
    {"bank",  jal_parse_pragma_target_bank},
    {"inst",  jal_parse_pragma_target_instruction_set},
    {"page",  jal_parse_pragma_target_page},
    {"PLLDIV", jal_parse_pragma_target_plldiv},
    {"CPUDIV", jal_parse_pragma_target_cpudiv},
    {"WDT",   jal_parse_pragma_target_wdt},
    {"XINST", jal_parse_pragma_target_xinst},
    {"DEBUG", jal_parse_pragma_target_debug},
    {"FCMEM", jal_parse_pragma_target_fcmem},
    {"IESO",  jal_parse_pragma_target_ieso},
    {"image", jal_parse_pragma_target_image}
  };
  size_t cmd;

  for (cmd = 0; 
       (cmd < COUNT(targets)) 
       && !pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, 
         targets[cmd].tag); 
       cmd++)
    ; /* null body */
  if (cmd < COUNT(targets)) {
    pf_token_get(pf, PF_TOKEN_NEXT);
    targets[cmd].action(pf);
  } else {
    const jal_pragma_fuses_t *fuses;

    for (fuses = jal_fuses;
         fuses && !pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE,
           fuses->name);
         fuses = fuses->link)
      ;
    if (!fuses) {
      /* No fuse group with this name: the PIC32 clock/config keyword
       * pragmas take over (classic cores always declare fuse groups
       * for osc/wdt/debug/ieso/mclr, so they never reach this branch;
       * identifiers arrive lowercased from the tokenizer). */
      static const struct {
        const char *tag;
        void      (*action)(pfile_t *);
      } pic32_pragmas[] = {
        {"osc",        jal_parse_pragma_target_osc},
        {"oscillator", jal_parse_pragma_target_oscillator},
        {"wdt",   jal_parse_pragma_pic32_wdt},
        {"dmt",   jal_parse_pragma_pic32_dmt},
        {"debug", jal_parse_pragma_pic32_debug},
        {"ieso",  jal_parse_pragma_pic32_ieso},
        {"sosc",  jal_parse_pragma_pic32_sosc},
        {"mclr",  jal_parse_pragma_pic32_mclr}
      };
      size_t pic32_i;

      for (pic32_i = 0;
           (pic32_i < COUNT(pic32_pragmas))
           && !pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE,
             pic32_pragmas[pic32_i].tag);
           pic32_i++)
        ;
      if (pic32_i < COUNT(pic32_pragmas)) {
        pf_token_get(pf, PF_TOKEN_NEXT);
        pic32_pragmas[pic32_i].action(pf);
        pic32_clock_solve_maybe(pf);
      } else {
        pfile_log(pf, PFILE_LOG_ERR, "unknown pragma target: %s",
          pf_token_ptr_get(pf));
      }
    } else {
      /* figure out which one it is */
      const jal_pragma_fuses_entry_t *entry;

      pf_token_get(pf, PF_TOKEN_NEXT);
      for (entry = fuses->entry;
           entry && !pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE,
             entry->name);
           entry = entry->link)
        ;
      pf_token_get(pf, PF_TOKEN_NEXT);
      if (!entry) {
        pfile_log(pf, PFILE_LOG_ERR, "unknown pragma target: %s %s",
          fuses->name, pf_token_ptr_get(pf));
      } else {
        /* _fuses = (_fuses & fuses->mask) | entry->bits */
        value_t n;

        n = pfile_value_find(pf, PFILE_LOG_ERR, "_fuses");
        if (n) {
          /* n can be either a simple constant, or an array of constants */
          if (value_is_array(n)) {
            value_t m;

            jal_fuse_edit_mark(pf,
                (variable_ct_t) fuses->config_word, fuses->mask);
            m = value_subscript_set(n, fuses->config_word);
            value_release(n);
            n = m;
          }
          value_const_set(n, 
              (value_const_get(n) & ~fuses->mask) | entry->bits);
          value_release(n);
        }
      }
    }
  }
}

/* note that the current function is part of the interrupt chain */
static void jal_parse_pragma_interrupt(pfile_t *pf)
{
  pfile_proc_t *proc;
  value_t       irqval;

  proc = pfile_proc_active_get(pf);

  if (!pfile_proc_parent_get(proc)) {
    pfile_log(pf, PFILE_LOG_ERR,
        "pragma interrupt can only be used in a procedure!");
  } else if ((pfile_proc_param_ct_get(proc) > 1)
    || pfile_proc_return_def_get(proc)) {
    pfile_log(pf, PFILE_LOG_ERR, 
        "an interrupt entry must be a procedure with no parameters!");
  } else if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_NOSTACK)) {
    pfile_log(pf, PFILE_LOG_ERR,
        "pragma interrupt cannot be used with pragma nostack");
  } else {
    pfile_proc_flag_set(proc, PFILE_PROC_FLAG_INTERRUPT);
    label_usage_bump(pfile_proc_label_get(proc), CTR_BUMP_INCR);
    irqval = jal_token_to_constant(pf, PFILE_LOG_NONE);
    if (irqval) {
      variable_const_t n;

      n = value_const_get(irqval);
      value_release(irqval);
      pf_token_get(pf, PF_TOKEN_NEXT);
      if (n > 255) {
        pfile_log(pf, PFILE_LOG_ERR,
            "pragma interrupt vector must be 0..255");
      } else {
        pfile_proc_irq_set(proc, (int) n);
      }
    }
  }
}

static void jal_marker_label(pfile_t *pf, const char *name)
{
  label_t lbl;

  lbl = pfile_label_find(pf, PFILE_LOG_NONE, name);
  if (!lbl) {
    lbl = pfile_label_alloc(pf, name);
  }
  if (lbl) {
    label_release(lbl);
  }
}

static void jal_parse_pragma_interrupt_mvec(pfile_t *pf)
{
  jal_marker_label(pf, "_mips_mvec");
}

static void jal_parse_pragma_interrupt_normal(pfile_t *pf)
{
  pfile_flag_clr(pf, PFILE_FLAG_MISC_INTERRUPT_RAW);
  pfile_flag_clr(pf, PFILE_FLAG_MISC_INTERRUPT_FAST);
  jal_parse_pragma_interrupt(pf);
}

static void jal_parse_pragma_interrupt_raw(pfile_t *pf)
{
  pfile_flag_set(pf, PFILE_FLAG_MISC_INTERRUPT_RAW);
  pfile_flag_clr(pf, PFILE_FLAG_MISC_INTERRUPT_FAST);
  jal_parse_pragma_interrupt(pf);
}

static void jal_parse_pragma_interrupt_fast(pfile_t *pf)
{
  pfile_flag_clr(pf, PFILE_FLAG_MISC_INTERRUPT_RAW);
  pfile_flag_set(pf, PFILE_FLAG_MISC_INTERRUPT_FAST);
  jal_parse_pragma_interrupt(pf);
}

/* note that the current function contains a jump table.
   nb : i'll likely ignore this and instead look for 
        ``addwf pcl,f; keeping everthing from there to the end
        of either the procedure or block in one bank */
/* Opt-in PIC32 COP1 (MZ EF). Default remains IEEE soft-float so QEMU
 * EC (no FPU) stays correct. Marker label is looked up in libpic32. */
static void jal_parse_pragma_fpu_hardware(pfile_t *pf)
{
  label_t lbl;

  lbl = pfile_label_find(pf, PFILE_LOG_NONE, "_mips_fpu_hardware");
  if (!lbl) {
    lbl = pfile_label_alloc(pf, "_mips_fpu_hardware");
  }
  if (lbl) {
    label_release(lbl);
  }
}

static void jal_parse_pragma_jump_table(pfile_t *pf)
{
  pfile_proc_t *proc;

  proc = pfile_proc_active_get(pf);

  if (!pfile_proc_parent_get(proc)) {
    pfile_log(pf, PFILE_LOG_ERR,
        "pragma jump_table can only be used in procedures & functions!");
  } else {
    if (pfile_flag_test(pf, PFILE_FLAG_WARN_MISC)) {
      pfile_log(pf, PFILE_LOG_WARN,
          "pragma jump_table has no effect. Use lookup tables instead.");
    }
  }
}

/* note that the BANK and PAGE bit must not be optimized away */
static void jal_parse_pragma_keep(pfile_t *pf)
{
  pf_token_get_t which;

  which = PF_TOKEN_CURRENT;
  do {
    if (pf_token_is(pf, which, PFILE_LOG_NONE, "page")) {
      pf_token_get(pf, PF_TOKEN_NEXT);
      pfile_proc_flag_set(pfile_proc_active_get(pf), 
          PFILE_PROC_FLAG_KEEP_PAGE);
    } else if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "bank")) {
      pfile_proc_flag_set(pfile_proc_active_get(pf), 
          PFILE_PROC_FLAG_KEEP_BANK);
      pf_token_get(pf, PF_TOKEN_NEXT);
    } else {
      pfile_log(pf, PFILE_LOG_ERR, "PAGE or BANK expected");
    }
    which = PF_TOKEN_NEXT;
  } while (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, ","));
}

static void jal_parse_pragma_eeprom_or_id(pfile_t *pf,
  const char *name,
  const char *name_base,
  const char *name_used)
{
  value_t start;

  start = jal_parse_expr(pf);
  if (start) {
    if (!value_is_const(start)) {
      pfile_log(pf, PFILE_LOG_ERR, PFILE_MSG_CONSTANT_EXPECTED);
    } else if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, ",")) {
      value_t sz;

      pf_token_get(pf, PF_TOKEN_NEXT);
      sz = jal_parse_expr(pf);
      if (sz) {
        if (!value_is_const(sz)) {
          pfile_log(pf, PFILE_LOG_ERR, PFILE_MSG_CONSTANT_EXPECTED);
        } else if (!value_const_get(sz)) {
          pfile_log(pf, PFILE_LOG_ERR, "size must be > 0");
        } else {
          variable_t eeprom_or_id;
          
          eeprom_or_id = pfile_variable_find(pf, PFILE_LOG_NONE, name, 0);
          if (eeprom_or_id) {
            pfile_log(pf, PFILE_LOG_ERR, "%s already defined", name + 1);
            variable_release(eeprom_or_id);
          } else {
            variable_def_t vdef; /* variable definition */
            variable_def_t mdef; /* array member        */

            mdef = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
                VARIABLE_DEF_FLAG_NONE, 1);
            vdef = variable_def_alloc(0, VARIABLE_DEF_TYPE_ARRAY,
                VARIABLE_DEF_FLAG_CONST, 0);  
            variable_def_member_add(vdef, 0, mdef, 
              (variable_ct_t) value_const_get(sz));

            pfile_variable_alloc(pf,
                  PFILE_VARIABLE_ALLOC_GLOBAL,
                  name, vdef, VARIABLE_NONE, 0);

            vdef = variable_def_alloc(0, VARIABLE_DEF_TYPE_INTEGER,
                VARIABLE_DEF_FLAG_CONST, 4);

            pfile_variable_alloc(pf,
                PFILE_VARIABLE_ALLOC_GLOBAL,
                name_used, vdef, VARIABLE_NONE, 0);

            if (RESULT_OK == pfile_variable_alloc(pf,
                PFILE_VARIABLE_ALLOC_GLOBAL,
                name_base, vdef, VARIABLE_NONE, &eeprom_or_id)) {
              variable_const_set(eeprom_or_id, variable_def_get(eeprom_or_id),
                  0, value_const_get(start));
              variable_release(eeprom_or_id);
            }
          }
        }
        value_release(sz);
      }
    }
    value_release(start);
  }
}

/* eeprom is implemented using two internal variables
     _eeprom      is an array of bytes
     _eeprom_base is where the eeprom goes
     _eeprom_used is a counter */
static void jal_parse_pragma_eeprom(pfile_t *pf)
{
  jal_parse_pragma_eeprom_or_id(pf, JAL_EEPROM, JAL_EEPROM_BASE, 
    JAL_EEPROM_USED);
}

/* id is implemented using two internal variables
     _id      is an array of bytes
     _id_base is where the id goes
     _id_used is a counter */
static void jal_parse_pragma_id(pfile_t *pf)
{
  jal_parse_pragma_eeprom_or_id(pf, PIC_ID, PIC_ID_BASE, PIC_ID_USED);
}

typedef enum {
  JAL_PARSE_PRAGMA_DATA_COMMON_DATA,
  JAL_PARSE_PRAGMA_DATA_COMMON_SHARED,
  JAL_PARSE_PRAGMA_DATA_COMMON_DATA_REMOVE
} jal_parse_pragma_data_common_t;

static void jal_parse_pragma_data_common(pfile_t *pf,
    jal_parse_pragma_data_common_t which)
{
  boolean_t is_first;

  is_first = BOOLEAN_TRUE;
  do {
    value_t val;

    if (is_first) {
      is_first = BOOLEAN_FALSE;
    } else {
      pf_token_get(pf, PF_TOKEN_NEXT);
    }

    val = jal_token_to_constant(pf, PFILE_LOG_ERR);
    if (val) {
      size_t  lo;
      size_t  hi;

      lo = value_const_get(val);
      hi = lo;

      value_release(val);
      val = VALUE_NONE;
      if (pf_token_is(pf, PF_TOKEN_NEXT, PFILE_LOG_NONE, "-")) {
        hi = -1;
        pf_token_get(pf, PF_TOKEN_NEXT);
        val = jal_token_to_constant(pf, PFILE_LOG_ERR);
        if (val) {
          pf_token_get(pf, PF_TOKEN_NEXT);
          hi = value_const_get(val);
        }
      }
      if ((size_t) -1 != hi) {
        switch (which) {
          case JAL_PARSE_PRAGMA_DATA_COMMON_DATA:
            pic_data_bank_list_add(pf, lo, hi);
            pfile_data_range_add(pf, (ulong) lo, (ulong) hi);
            break;
          case JAL_PARSE_PRAGMA_DATA_COMMON_SHARED:
            pic_shared_bank_list_add(pf, lo, hi);
            break;
          case JAL_PARSE_PRAGMA_DATA_COMMON_DATA_REMOVE:
            pic_data_bank_list_remove_range(pf, lo, hi);
            break;
        }
      }
    }
    value_release(val);
  } while (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, ","));
}

static void jal_parse_pragma_data(pfile_t *pf)
{
  jal_parse_pragma_data_common(pf, JAL_PARSE_PRAGMA_DATA_COMMON_DATA);
}

static void jal_parse_pragma_data_remove(pfile_t *pf)
{
  jal_parse_pragma_data_common(pf, JAL_PARSE_PRAGMA_DATA_COMMON_DATA_REMOVE);
}

static void jal_parse_pragma_shared(pfile_t *pf)
{
  jal_parse_pragma_data_common(pf, JAL_PARSE_PRAGMA_DATA_COMMON_SHARED);
}

static void jal_parse_pragma_code_or_stack(pfile_t *pf, char *name)
{
  value_t sz;

  sz = jal_parse_expr(pf);
  if (sz) {
    if (!value_is_const(sz)) {
      pfile_log(pf, PFILE_LOG_ERR, PFILE_MSG_CONSTANT_EXPECTED);
    } else if (!value_const_get(sz)) {
      pfile_log(pf, PFILE_LOG_ERR, "size must be > 0");
    } else {
      jal_variable_info_t inf;

      jal_variable_info_init(&inf);
      inf.name      = name;
      inf.ct        = 0;
      inf.def_flags = VARIABLE_DEF_FLAG_CONST;

      variable_release(
          jal_variable_alloc(pf, &inf, BOOLEAN_FALSE, 1, &sz,
            PFILE_VARIABLE_ALLOC_GLOBAL));
    }
    value_release(sz);
  }
}

static void jal_parse_pragma_code(pfile_t *pf)
{
  jal_parse_pragma_code_or_stack(pf, "_code_size");
}

/* pragma code_location <const-expr>
 * Where code lands when the program has no `pragma bootloader` (the
 * bootloader pragma always wins when present).  The default lives in
 * the device file -- for generated PIC32 files it is computed from the
 * .PIC boot sector (reset-vector flash mapped through kseg1).  A
 * program-side pragma overrides the device default (last one wins). */
static void jal_parse_pragma_code_location(pfile_t *pf)
{
  value_t addr;

  addr = jal_parse_expr(pf);
  if (addr) {
    if (!value_is_const(addr)) {
      pfile_log(pf, PFILE_LOG_ERR, PFILE_MSG_CONSTANT_EXPECTED);
    } else if (!value_const_get(addr)) {
      pfile_log(pf, PFILE_LOG_ERR, "code location must be > 0");
    } else {
      pfile_code_location_set(pf, value_const_get(addr));
    }
    value_release(addr);
  }
}

static void jal_parse_pragma_stack(pfile_t *pf)
{
  jal_parse_pragma_code_or_stack(pf, "_stack_size");
}

static void jal_parse_pragma_eedata_or_iddata(pfile_t *pf,
  const char *name, const char *name_used)
{
  value_t   eeprom;
  value_t   eeprom_used;
  value_t   val;
  boolean_t full;
  size_t    eeprom_ct;

  eeprom = pfile_value_find(pf, PFILE_LOG_ERR, name);
  eeprom_ct   = value_ct_get(eeprom);
  eeprom_used = pfile_value_find(pf, PFILE_LOG_ERR, name_used);
  value_dereference(eeprom);
  value_baseofs_set(eeprom, eeprom_used);
  val = VALUE_NONE;
  /* determine if we're already full so we only report the error once */
  full = value_const_get(eeprom_used) == value_sz_get(eeprom);
  do {
    const char *ptr;

    if (val) {
      pf_token_get(pf, PF_TOKEN_NEXT); /* skip the comma */
    }
    ptr = pf_token_ptr_get(pf);
    if ('"' == *ptr) {
      size_t sz;

      sz = pf_token_sz_get(pf);
      sz--;
      ptr++;
      if (sz && ('"' == ptr[sz-1])) {
        sz--;
      }
      while (sz--) {
        if (value_const_get(eeprom_used) == eeprom_ct) {
          if (!full) {
            pfile_log(pf, PFILE_LOG_ERR, "%s is full", name + 1);
          }
          full = BOOLEAN_TRUE;
        } else {
          value_const_set(eeprom, *ptr);
          value_const_set(eeprom_used, value_const_get(eeprom_used) + 1);
        }
        ptr++;
      }
      pf_token_get(pf, PF_TOKEN_NEXT);
      val = eeprom; /* set to a non-zero dummy value */
    } else {
      val = jal_parse_expr(pf);
      if (val) {
        if (!value_is_const(val)) {
          pfile_log(pf, PFILE_LOG_ERR, PFILE_MSG_CONSTANT_EXPECTED);
        } else if (value_const_get(val) >= 256) {
          pfile_log(pf, PFILE_LOG_ERR, "constant out of range");
        } else if (eeprom) {
          if (value_const_get(eeprom_used) == eeprom_ct) {
            if (!full) {
              pfile_log(pf, PFILE_LOG_ERR, "%s is full", name + 1);
            }
          } else {
            value_const_set(eeprom, value_const_get(val));
            value_const_set(eeprom_used, value_const_get(eeprom_used) + 1);
          }
        }
        value_release(val);
      }
    }
  } while (val && pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, ","));
  value_release(eeprom_used);
  value_release(eeprom);
}

static void jal_parse_pragma_eedata(pfile_t *pf)
{
  jal_parse_pragma_eedata_or_iddata(pf, JAL_EEPROM, JAL_EEPROM_USED);
}

static void jal_parse_pragma_iddata(pfile_t *pf)
{
  jal_parse_pragma_eedata_or_iddata(pf, JAL_ID, JAL_ID_USED);
}

/*
 * pragma fuse_def tag mask '{'
 *   tag '=' bits
 *   ...
 * '}'
 */ 

static void jal_parse_pragma_fuse_def(pfile_t *pf)
{
  jal_pragma_fuses_t *fuse;
  const char         *ptr;
  size_t              sz;
  value_t             val;
  

  ptr = pf_token_get(pf, PF_TOKEN_CURRENT);
  sz  = 1 + strlen(ptr);

  fuse = MALLOC(sizeof(*fuse) + sz);
  if (!fuse) {
    pfile_log_syserr(pf, ENOMEM);
  } else {
    fuse->link        = jal_fuses;
    jal_fuses         = fuse;
    fuse->name        = (void *) (fuse + 1);
    memcpy(fuse + 1, ptr, sz);
    fuse->mask        = 0;
    fuse->entry       = 0;
    fuse->config_word = 0;
  }
  if (pf_token_is(pf, PF_TOKEN_NEXT, PFILE_LOG_NONE, ":")) {
    /* set the config_word entry */
    value_t fuses;

    pf_token_get(pf, PF_TOKEN_NEXT);
    val = jal_parse_expr(pf);

    fuses = pfile_value_find(pf, PFILE_LOG_ERR, "_fuses");
    if (!val || !value_is_const(val)) {
      pfile_log(pf, PFILE_LOG_ERR, PFILE_MSG_CONSTANT_EXPECTED);
    } else if (value_const_get(val) >= value_ct_get(fuses)) {
      pfile_log(pf, PFILE_LOG_ERR, "value out of range");
    } else if (fuse) {
      fuse->config_word = value_const_get(val);
    }
    value_release(val);
    value_release(fuses);
  }

  val = jal_parse_expr(pf);
  if (!val || !value_is_const(val)) {
    pfile_log(pf, PFILE_LOG_ERR, PFILE_MSG_CONSTANT_EXPECTED);
  } else if (fuse) {
    fuse->mask = value_const_get(val);
  }
  value_release(val);
  if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_ERR, "{")) {
    pf_token_get(pf, PF_TOKEN_NEXT);
    while (!pf_token_is_eof(pf)
        && !pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, "}")) {
      jal_pragma_fuses_entry_t *entry;

      ptr = pf_token_get(pf, PF_TOKEN_CURRENT);
      if (!fuse) {
        entry = 0;
      } else {
        sz  = 1 + strlen(ptr);

        entry = MALLOC(sizeof(*entry) + sz);
        if (entry) {
          entry->link = fuse->entry;
          fuse->entry = entry;
          entry->name = (void *) (entry + 1);
          memcpy(entry + 1, ptr, sz);
          entry->bits = 0;
        }
      }
      if (pf_token_is(pf, PF_TOKEN_NEXT, PFILE_LOG_ERR, "=")) {
        pf_token_get(pf, PF_TOKEN_NEXT);
        val = jal_parse_expr(pf);
        if (!val || !value_is_const(val)) {
          pfile_log(pf, PFILE_LOG_ERR, PFILE_MSG_CONSTANT_EXPECTED);
        } else if (entry) {
          entry->bits = value_const_get(val);
        }
        value_release(val);
      }
    }
  }
  pf_token_get(pf, PF_TOKEN_NEXT);
}


static void jal_parse_pragma_inline(pfile_t *pf)
{
  pfile_proc_t *proc;

  proc = pfile_proc_active_get(pf);

  if (!pfile_proc_parent_get(proc)) {
    pfile_log(pf, PFILE_LOG_ERR,
        "pragma inline can only be used in a procedure!");
  } else {
    pfile_proc_flag_set(proc, PFILE_PROC_FLAG_INLINE);
  }
}

static void jal_parse_pragma_task(pfile_t *pf)
{
  value_t task_ct;

  task_ct = jal_parse_expr(pf);
  if (task_ct) {
    if (!value_is_const(task_ct)) {
      pfile_log(pf, PFILE_LOG_ERR, "constant expression expected");
    } else if (value_const_get(task_ct) < 1) {
      pfile_log(pf, PFILE_LOG_ERR, "constant out of range");
    } else {
      pfile_task_ct_set(pf, value_const_get(task_ct));
    }
    value_release(task_ct);
  }
}

static void jal_parse_pragma_frame(pfile_t *pf)
{
  pfile_proc_t *proc;

  proc = pfile_proc_active_get(pf);
  if (!pfile_proc_parent_get(proc)) {
    pfile_log(pf, PFILE_LOG_ERR,
      "pragma frame can only be used in a procedure or function!");
  } else {
    pfile_proc_flag_set(proc, PFILE_PROC_FLAG_FRAME);
  }
}

static void jal_parse_pragma_nostack(pfile_t *pf)
{
  pfile_proc_t *proc;

  proc = pfile_proc_active_get(pf);
  if (!pfile_proc_parent_get(proc)) {
    pfile_log(pf, PFILE_LOG_ERR,
      "pragma nostack can only be used in a procedure or function!");
  } else if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INTERRUPT)
    || pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INDIRECT)) {
    pfile_log(pf, PFILE_LOG_ERR,
      "pragma nostack cannot be used with pragma interrupt or"
      " indirect functions");
  } else {
    pfile_proc_flag_set(proc, PFILE_PROC_FLAG_NOSTACK);
  }
}


static void jal_parse_pragma_speed(pfile_t *pf)
{
  pfile_flag_set(pf, PFILE_FLAG_CMD_SPEED);
}

static void jal_parse_pragma_size(pfile_t *pf)
{
  pfile_flag_clr(pf, PFILE_FLAG_CMD_SPEED);
}

typedef enum pragma_action_ {
  PRAGMA_ACTION_NONE = 0, /* unused                  */
  PRAGMA_ACTION_FOLLOW,   /* follow the 'child' link */
  PRAGMA_ACTION_FLAG,     /* use mask/set on a flag  */
  PRAGMA_ACTION_FN,       /* execute [fn]            */
  PRAGMA_ACTION_EOL       /* end of list             */
} pragma_action_t;

#define PRAGMA_WORD_FLAG_NONE   0x0000
#define PRAGMA_WORD_FLAG_ABSORB 0x0001 /* absorb the next token */
typedef void (*pragma_word_fn_t)(pfile_t *pf);
#define PRAGMA_WORD_FN_NONE ((pragma_word_fn_t) 0)

typedef struct pragma_word_ {
  const char                *key;
  pragma_action_t            action;
  const struct pragma_word_ *child;
  struct {
    flag_t                   mask;
    flag_t                   set;
  } flag;
  pragma_word_fn_t           fn;
  flag_t                     flags;
} pragma_word_t;

#define DEFINE_FN(key, fn, flag) \
  { key, PRAGMA_ACTION_FN, 0, {0, 0}, fn, flag }
#define DEFINE_FOLLOW(key, child) \
  { key, PRAGMA_ACTION_FOLLOW, child, {0, 0}, PRAGMA_WORD_FN_NONE, \
    PRAGMA_WORD_FLAG_ABSORB }
#define DEFINE_FLAG(key, mask, set) \
  { key, PRAGMA_ACTION_FLAG, 0, {mask, set}, PRAGMA_WORD_FN_NONE, \
    PRAGMA_WORD_FLAG_ABSORB }
#define DEFINE_END_OF_LIST \
  { 0, PRAGMA_ACTION_EOL, 0, {0, 0}, PRAGMA_WORD_FN_NONE, \
    PRAGMA_WORD_FLAG_NONE }

static const pragma_word_t pragma_clear[] = {
  DEFINE_FLAG("yes",     
    ~PFILE_FLAG_MISC_CLEAR_BSS, 
    PFILE_FLAG_MISC_CLEAR_BSS),
  DEFINE_FLAG("no",      
    ~PFILE_FLAG_MISC_CLEAR_BSS, 
    0),
  DEFINE_FLAG(0,         
    ~PFILE_FLAG_MISC_CLEAR_BSS, 
    0),
  DEFINE_END_OF_LIST
};

#define BOOTLOADER_FLAGS       \
  (PFILE_FLAG_BOOT_RICK         \
   | PFILE_FLAG_BOOT_LONG_START \
   | PFILE_FLAG_BOOT_BLOADER    \
   | PFILE_FLAG_BOOT_LOADER18)

static void jal_parse_pragma_loader18(pfile_t *pf)
{
  value_t val;

  pfile_flag_change(pf, ~BOOTLOADER_FLAGS, PFILE_FLAG_BOOT_LOADER18);
  val = jal_token_to_constant(pf, PFILE_LOG_NONE);
  if (val) {
    pf_token_get(pf, PF_TOKEN_NEXT);
    pfile_loader_offset_set(pf, value_const_get(val));
    value_release(val);
  } else {
    pfile_loader_offset_set(pf, 0x0800);
  }
}


static void jal_parse_pragma_loader32(pfile_t *pf)
{
  value_t val;

  jal_marker_label(pf, "_mips_loader_pfm");
  pfile_flag_clr(pf, PFILE_FLAG_BOOT_FUSES);
  val = jal_token_to_constant(pf, PFILE_LOG_NONE);
  if (val) {
    pfile_loader_offset_set(pf, value_const_get(val));
    value_release(val);
    pf_token_get(pf, PF_TOKEN_NEXT);
  } else {
    pfile_loader_offset_set(pf, 0x9D000000UL);
  }
}

static const pragma_word_t pragma_bootloader[] = {
  DEFINE_FLAG("rickpic", 
    ~BOOTLOADER_FLAGS,
    PFILE_FLAG_BOOT_RICK),
  DEFINE_FLAG("long_start", 
    ~BOOTLOADER_FLAGS,
    PFILE_FLAG_BOOT_LONG_START),
  DEFINE_FLAG("bloader",
    ~BOOTLOADER_FLAGS,
    PFILE_FLAG_BOOT_BLOADER),
  DEFINE_FN("loader18", jal_parse_pragma_loader18, PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN("pic32", jal_parse_pragma_loader32, PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FLAG(0,
    ~BOOTLOADER_FLAGS,
    0),
  DEFINE_END_OF_LIST
};
#undef BOOTLOADER_FLAGS

static const pragma_word_t pragma_opt_expr_reduce[] = {
  DEFINE_FLAG("yes",     
    ~PFILE_FLAG_OPT_EXPR_REDUCTION, 
    PFILE_FLAG_OPT_EXPR_REDUCTION ),
  DEFINE_FLAG("no",      
    ~PFILE_FLAG_OPT_EXPR_REDUCTION,
    0 ),
  DEFINE_FLAG(0,         
    ~PFILE_FLAG_OPT_EXPR_REDUCTION, 
    PFILE_FLAG_OPT_EXPR_REDUCTION ),
  DEFINE_END_OF_LIST
};

static const pragma_word_t pragma_opt_cexpr_reduce[] = {
  DEFINE_FLAG("yes",     
    ~PFILE_FLAG_OPT_CEXPR_REDUCTION, 
    PFILE_FLAG_OPT_CEXPR_REDUCTION ),
  DEFINE_FLAG("no",      
    ~PFILE_FLAG_OPT_CEXPR_REDUCTION, 
    0 ),
  DEFINE_FLAG(0,         
    ~PFILE_FLAG_OPT_CEXPR_REDUCTION, 
    PFILE_FLAG_OPT_CEXPR_REDUCTION ),
  DEFINE_END_OF_LIST
};

static const pragma_word_t pragma_opt_temp_reduce[] = {
  DEFINE_FLAG("yes",     
    ~PFILE_FLAG_OPT_TEMP_REDUCE, 
    PFILE_FLAG_OPT_TEMP_REDUCE ),
  DEFINE_FLAG("no",     
    ~PFILE_FLAG_OPT_TEMP_REDUCE, 
    0 ),
  DEFINE_FLAG(0,         
    ~PFILE_FLAG_OPT_TEMP_REDUCE, 
    0 ),
  DEFINE_END_OF_LIST
};

static const pragma_word_t pragma_opt_variable_reduce[] = {
  DEFINE_FLAG("yes",     
    ~PFILE_FLAG_OPT_VARIABLE_REDUCE, 
    PFILE_FLAG_OPT_VARIABLE_REDUCE ),
  DEFINE_FLAG("no",      
    ~PFILE_FLAG_OPT_VARIABLE_REDUCE, 
    0 ),
  DEFINE_FLAG(0,         
    ~PFILE_FLAG_OPT_VARIABLE_REDUCE, 
    PFILE_FLAG_OPT_VARIABLE_REDUCE ),
  DEFINE_END_OF_LIST
};

static const pragma_word_t pragma_opt_load_reduce[] = {
  DEFINE_FLAG("yes",     
    ~PFILE_FLAG_OPT_LOAD_REDUCE, 
    PFILE_FLAG_OPT_LOAD_REDUCE ),
  DEFINE_FLAG("no",      
    ~PFILE_FLAG_OPT_LOAD_REDUCE, 
    0 ),
  DEFINE_FLAG(0,         
    ~PFILE_FLAG_OPT_LOAD_REDUCE, 
    0 ),
  DEFINE_END_OF_LIST
};

static const pragma_word_t pragma_opt_const_detect[] = {
  DEFINE_FLAG("yes",
    ~PFILE_FLAG_OPT_CONST_DETECT,
    PFILE_FLAG_OPT_CONST_DETECT ),
  DEFINE_FLAG("no",
    ~PFILE_FLAG_OPT_CONST_DETECT,
    0 ),
  DEFINE_FLAG(0,
    ~PFILE_FLAG_OPT_CONST_DETECT,
    0 ),
  DEFINE_END_OF_LIST
};

static const pragma_word_t pragma_opt_variable_frame[] = {
  DEFINE_FLAG("yes",
      ~PFILE_FLAG_OPT_VARIABLE_FRAME,
      PFILE_FLAG_OPT_VARIABLE_FRAME),
  DEFINE_FLAG("no",
      ~PFILE_FLAG_OPT_VARIABLE_FRAME,
      0),
  DEFINE_FLAG(0,         
    ~PFILE_FLAG_OPT_VARIABLE_REDUCE, 
    PFILE_FLAG_OPT_VARIABLE_REDUCE ),
  DEFINE_END_OF_LIST
};

static const pragma_word_t pragma_opt[] = {
  DEFINE_FOLLOW("expr_reduce",     pragma_opt_expr_reduce),
  DEFINE_FOLLOW("cexpr_reduce",    pragma_opt_cexpr_reduce),
  DEFINE_FOLLOW("temp_reduce",     pragma_opt_temp_reduce),
  DEFINE_FOLLOW("variable_reduce", pragma_opt_variable_reduce),
  DEFINE_FOLLOW("load_reduce",     pragma_opt_load_reduce),
  DEFINE_FOLLOW("const_detect",    pragma_opt_const_detect),
  DEFINE_FOLLOW("variable_frame",  pragma_opt_variable_frame)
};

static const pragma_word_t pragma_warn_all[] = {
  DEFINE_FLAG("yes",
      ~PFILE_FLAG_WARN_ALL,
      PFILE_FLAG_WARN_ALL ),
  DEFINE_FLAG("no",
      ~PFILE_FLAG_WARN_ALL,
      0 ),
  DEFINE_END_OF_LIST
};

static const pragma_word_t pragma_warn_conversion[] = {
  DEFINE_FLAG("yes",
      ~PFILE_FLAG_WARN_CONVERSION,
      PFILE_FLAG_WARN_CONVERSION ),
  DEFINE_FLAG("no",
      ~PFILE_FLAG_WARN_CONVERSION,
      0 ),
  DEFINE_FLAG(0,
      ~PFILE_FLAG_WARN_CONVERSION,
      PFILE_FLAG_WARN_CONVERSION ),
  DEFINE_END_OF_LIST
};

static const pragma_word_t pragma_warn_directives[] = {
  DEFINE_FLAG("yes",
      ~PFILE_FLAG_WARN_DIRECTIVES,
      PFILE_FLAG_WARN_DIRECTIVES ),
  DEFINE_FLAG("no",
      ~PFILE_FLAG_WARN_DIRECTIVES,
      0 ),
  DEFINE_FLAG(0,
      ~PFILE_FLAG_WARN_DIRECTIVES,
      0 ),
  DEFINE_END_OF_LIST
};

static const pragma_word_t pragma_warn_misc[] = {
  DEFINE_FLAG("yes",
      ~PFILE_FLAG_WARN_MISC,
      PFILE_FLAG_WARN_MISC ),
  DEFINE_FLAG("no",
      ~PFILE_FLAG_WARN_MISC,
      0 ),
  DEFINE_FLAG(0,
      ~PFILE_FLAG_WARN_MISC,
      PFILE_FLAG_WARN_MISC ),
  DEFINE_END_OF_LIST
};

static const pragma_word_t pragma_warn_range[] = {
  DEFINE_FLAG("yes",
      ~PFILE_FLAG_WARN_RANGE,
      PFILE_FLAG_WARN_RANGE ),
  DEFINE_FLAG("no",
      ~PFILE_FLAG_WARN_RANGE,
      0 ),
  DEFINE_FLAG(0,
      ~PFILE_FLAG_WARN_RANGE,
      PFILE_FLAG_WARN_RANGE ),
  DEFINE_END_OF_LIST
};

static const pragma_word_t pragma_warn_backend[] = {
  DEFINE_FLAG("yes",
      ~PFILE_FLAG_WARN_BACKEND,
      PFILE_FLAG_WARN_BACKEND ),
  DEFINE_FLAG("no",
      ~PFILE_FLAG_WARN_BACKEND,
      0 ),
  DEFINE_FLAG(0,
      ~PFILE_FLAG_WARN_BACKEND,
      PFILE_FLAG_WARN_BACKEND ),
  DEFINE_END_OF_LIST
};

static const pragma_word_t pragma_warn_stack_overflow[] = {
  DEFINE_FLAG("yes",
      ~PFILE_FLAG_WARN_STACK_OVERFLOW,
      PFILE_FLAG_WARN_STACK_OVERFLOW ),
  DEFINE_FLAG("no",
      ~PFILE_FLAG_WARN_STACK_OVERFLOW,
      0 ),
  DEFINE_FLAG(0,
      ~PFILE_FLAG_WARN_STACK_OVERFLOW,
      0 )
};

static const pragma_word_t pragma_warn_truncate[] = {
  DEFINE_FLAG("yes",
      ~PFILE_FLAG_WARN_TRUNCATE,
      PFILE_FLAG_WARN_TRUNCATE ),
  DEFINE_FLAG("no",
      ~PFILE_FLAG_WARN_TRUNCATE,
      0 ),
  DEFINE_FLAG(0,
      ~PFILE_FLAG_WARN_TRUNCATE,
      PFILE_FLAG_WARN_TRUNCATE ),
  DEFINE_END_OF_LIST
};

static const pragma_word_t pragma_warn[] = {
  DEFINE_FOLLOW("all",             pragma_warn_all),
  DEFINE_FOLLOW("backend",         pragma_warn_backend),
  DEFINE_FOLLOW("conversion",      pragma_warn_conversion),
  DEFINE_FOLLOW("directives",      pragma_warn_directives),
  DEFINE_FOLLOW("misc",            pragma_warn_misc),
  DEFINE_FOLLOW("range",           pragma_warn_range),
  DEFINE_FOLLOW("stack_overflow",  pragma_warn_stack_overflow),
  DEFINE_FOLLOW("truncate",        pragma_warn_truncate),
  DEFINE_END_OF_LIST
};

static const pragma_word_t pragma_debug_codegen[] = {
  DEFINE_FLAG("yes",
      ~PFILE_FLAG_DEBUG_CODEGEN,
      PFILE_FLAG_DEBUG_CODEGEN),
  DEFINE_FLAG("no",
      ~PFILE_FLAG_DEBUG_CODEGEN,
      0),
  DEFINE_FLAG(0,
      ~PFILE_FLAG_DEBUG_CODEGEN,
      PFILE_FLAG_DEBUG_CODEGEN),
  DEFINE_END_OF_LIST
};

static const pragma_word_t pragma_debug_pcode[] = {
  DEFINE_FLAG("yes",
      ~PFILE_FLAG_DEBUG_PCODE,
      PFILE_FLAG_DEBUG_PCODE),
  DEFINE_FLAG("no",
      ~PFILE_FLAG_DEBUG_PCODE,
      0),
  DEFINE_FLAG(0,
      ~PFILE_FLAG_DEBUG_PCODE,
      0),
  DEFINE_END_OF_LIST
};

static const pragma_word_t pragma_debug[] = {
  DEFINE_FOLLOW("codegen",         pragma_debug_codegen),
  DEFINE_FOLLOW("pcode",           pragma_debug_pcode),
  DEFINE_END_OF_LIST
};

static const pragma_word_t pragma_fuses[] = {
  DEFINE_FLAG("yes",
      ~PFILE_FLAG_BOOT_FUSES,
      PFILE_FLAG_BOOT_FUSES),
  DEFINE_FLAG("no",
      ~PFILE_FLAG_BOOT_FUSES,
      0),
  DEFINE_FLAG(0,
      ~PFILE_FLAG_BOOT_FUSES,
      PFILE_FLAG_BOOT_FUSES),
  DEFINE_END_OF_LIST
};

static const pragma_word_t pragma_interrupt[] = {
  DEFINE_FN("normal", jal_parse_pragma_interrupt_normal, 
    PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN("raw",    jal_parse_pragma_interrupt_raw, PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN("fast",   jal_parse_pragma_interrupt_fast, 
    PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN("mvec",   jal_parse_pragma_interrupt_mvec,
    PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN(0, jal_parse_pragma_interrupt, PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_END_OF_LIST
};

static void jal_parse_pragma_isa_micromips(pfile_t *pf)
{
  jal_marker_label(pf, "_mips_micromips");
}

static const pragma_word_t pragma_isa[] = {
  DEFINE_FN("micromips", jal_parse_pragma_isa_micromips,
    PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_END_OF_LIST
};

static const pragma_word_t pragma_fpu[] = {
  DEFINE_FN("hardware", jal_parse_pragma_fpu_hardware,
    PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_END_OF_LIST
};

static const pragma_word_t pragma_string[] = {
  DEFINE_FN("heap", jal_parse_pragma_string_heap, PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN("index", jal_parse_pragma_string_index, PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN("alloc", jal_parse_pragma_string_alloc, PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN("retry", jal_parse_pragma_string_retry, PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_END_OF_LIST
};

static const pragma_word_t base[] = {
  DEFINE_FN(    "name",       jal_parse_pragma_name, PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN(    "error",      jal_parse_pragma_error, PRAGMA_WORD_FLAG_NONE),
  DEFINE_FN(    "target",     jal_parse_pragma_target, 
    PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FOLLOW("interrupt",  pragma_interrupt),
  DEFINE_FN(    "jump_table", jal_parse_pragma_jump_table, 
    PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN(    "keep",       jal_parse_pragma_keep, PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN(    "eeprom",     jal_parse_pragma_eeprom, 
    PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN(    "data",       jal_parse_pragma_data, PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN(    "data_remove", jal_parse_pragma_data_remove,
    PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN(    "shared",     jal_parse_pragma_shared,
    PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN(    "code",       jal_parse_pragma_code, PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN(    "code_location", jal_parse_pragma_code_location,
    PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN(    "stack",      jal_parse_pragma_stack, PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN(    "eedata",     jal_parse_pragma_eedata, 
    PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN(    "fuse_def",   jal_parse_pragma_fuse_def, 
    PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN(    "inline",     jal_parse_pragma_inline, PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN(    "frame",      jal_parse_pragma_frame, PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN(    "nostack",    jal_parse_pragma_nostack, 
    PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN(    "task",       jal_parse_pragma_task, PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FOLLOW("fpu",        pragma_fpu),
  DEFINE_FOLLOW("isa",        pragma_isa),
  DEFINE_FOLLOW("string",     pragma_string),
  DEFINE_FOLLOW("bootloader", pragma_bootloader),
  DEFINE_FOLLOW("clear",      pragma_clear),
  DEFINE_FOLLOW("opt",        pragma_opt),
  DEFINE_FOLLOW("warn",       pragma_warn),
  DEFINE_FOLLOW("debug",      pragma_debug),
  DEFINE_FOLLOW("fuses",      pragma_fuses),
  DEFINE_FN(    "speed",      jal_parse_pragma_speed, PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN(    "size",       jal_parse_pragma_size, PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN(    "id",         jal_parse_pragma_id, PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_FN(    "iddata",     jal_parse_pragma_iddata, PRAGMA_WORD_FLAG_ABSORB),
  DEFINE_END_OF_LIST
};

void jal_parse_pragma(pfile_t *pf, const pfile_pos_t *statement_start)
{
  static const pragma_word_t *cmd;

  UNUSED(statement_start);
  for (cmd = base;
       cmd->key && (PRAGMA_ACTION_EOL != cmd->action);
       /* nothing */
      ) {
    if (pf_token_is(pf, PF_TOKEN_CURRENT, PFILE_LOG_NONE, cmd->key)) {
      if (cmd->flags & PRAGMA_WORD_FLAG_ABSORB) {
        pf_token_get(pf, PF_TOKEN_NEXT);
      }
      if (PRAGMA_ACTION_FOLLOW == cmd->action) {
        cmd = cmd->child;
      } else {
        break; /* found it! */
      }
    } else {
      cmd++;
    }
  }
  if (cmd) {
    switch (cmd->action) {
      case PRAGMA_ACTION_NONE:
      case PRAGMA_ACTION_FOLLOW:
        break; /* shouldn't get here */
      case PRAGMA_ACTION_FLAG:
        pfile_flag_change(pf, cmd->flag.mask, cmd->flag.set);
        break;
      case PRAGMA_ACTION_FN:
        cmd->fn(pf);
        break;
      case PRAGMA_ACTION_EOL:
        pfile_log(pf, PFILE_LOG_ERR, "unexpected token: '%s'",
            pf_token_get(pf, PF_TOKEN_CURRENT));
        break;
    }
    /* Don't advance tokens after pragma processing - let the main parser handle it */
  } else {
  }
}


