/************************************************************
 **
 ** cod_file.c : COD file definitions
 **
 ** Copyright (c) 2007-2007, Kyle A. York
 **               2020-2020, Rob Jansen
 ** All rights reserved
 **
 ************************************************************/
#include <string.h>
#include <stddef.h>
#include <ctype.h>
#include <time.h>
#define ARRAY_DEFINE
#include "cod_file.h"

#define COUNT(x) (sizeof(x)/sizeof((x)[0]))

static void uchar_write(FILE *dst, uchar ch)
{
  fputc(ch, dst);
}

static void ushort_write(FILE *dst, ushort n)
{
  uchar_write(dst, (uchar) (n >> 0));
  uchar_write(dst, (uchar) (n >> 8));
}


static void ulong_write(FILE *dst, ulong n)
{
  uchar_write(dst, (uchar) (n >>  0));
  uchar_write(dst, (uchar) (n >>  8));
  uchar_write(dst, (uchar) (n >> 16));
  uchar_write(dst, (uchar) (n >> 24));
}

static void string_write(FILE *dst, size_t len, const uchar *str)
{
  if (str) {
    (void) fwrite(str, len, 1, dst);
  } else {
    while (len--) {
      uchar_write(dst, 0);
    }
  }
}

#define COD_CODE_MAP_ENTRY_SIZE (4)

static void COD_code_map_entry_write(const COD_directory_t *dir,
    const void *ptr)
{
    const COD_code_map_entry_t *ety;

  ety = ptr;
  if (ety) {
    ushort_write(dir->io, ety->start);
    ushort_write(dir->io, ety->end);
  } else {
    ushort_write(dir->io, 0);
    ushort_write(dir->io, 0);
  }
}

typedef ushort (*blocks_need_t)(const COD_directory_t *dir);
typedef void   (*entry_write_t)(const COD_directory_t *dir,
                                const void *ptr);
void COD_table_write(
    COD_directory_t *dir,
    size_t           tbl_offset,
    ushort           entry_sz,
    entry_write_t    entry_write)
{
  uchar   *dir_base;
  array_t *tbl;
  ushort   sz;
  size_t   ii;

  dir_base = (uchar *) dir;
  tbl      = *(array_t **) (dir_base + tbl_offset);

  sz = 0;
  for (ii = 0; ii < array_ct(tbl); ii++) {
    if (sz + entry_sz > COD_BLOCK_SIZE) {
      while (sz < COD_BLOCK_SIZE) {
        uchar_write(dir->io, 0);
        sz++;
      }
      sz = 0;
    }
    entry_write(dir, array_get(tbl, ii));
    sz += entry_sz;
  }
  if (sz) {
    while (sz < COD_BLOCK_SIZE) {
      uchar_write(dir->io, 0);
      sz++;
    }
  }
}

#define COD_SYMBOL_ENTRY_SIZE (16)

static void COD_symbol_entry_write(const COD_directory_t *dir, const void *ptr)
{
  const COD_symbol_entry_t *ety;
  
  ety = ptr;

  if (ety) {
    string_write(dir->io, sizeof(ety->label), ety->label);
    uchar_write(dir->io, ety->type);
    ushort_write(dir->io, ety->value);
  } else {
    string_write(dir->io, sizeof(ety->label), 0);
    uchar_write(dir->io, 0);
    ushort_write(dir->io, 0);
  }
}

#define COD_NAME_ENTRY_SIZE (64)

static void COD_name_entry_write(const COD_directory_t *dir, const void *ptr)
{
  const COD_name_entry_t *ety;

  ety = ptr;
  if (ety) {
    string_write(dir->io, sizeof(ety->label), ety->label);
  } else {
    string_write(dir->io, sizeof(ety->label), 0);
  }
}

#define COD_LINE_SYMBOL_ENTRY_SIZE (6)

static void COD_line_symbol_entry_write(const COD_directory_t *dir,
    const void *ptr)
{
  const COD_line_symbol_entry_t *ety;

  ety = ptr;
  if (ety) {
    uchar_write(dir->io, ety->file);
    uchar_write(dir->io, ety->smod);
    ushort_write(dir->io, ety->line);
    ushort_write(dir->io, ety->sloc);
  } else {
    uchar_write(dir->io, 0);
    uchar_write(dir->io, 0);
    ushort_write(dir->io, 0);
    ushort_write(dir->io, 0);
  }
}

#define COD_LOCAL_VARS_ENTRY_SIZE (16)

static void COD_local_vars_entry_write(const COD_directory_t *dir, 
    const COD_local_vars_entry_t *ety)
{
  string_write(dir->io, 8, (uchar *) "\007__LOCAL");
  ulong_write(dir->io, ety->header.start);
  ulong_write(dir->io, ety->header.end);
}

void COD_local_vars_table_write(const COD_directory_t *dir)
{
  size_t ii;
  size_t sz;

  sz = 0;
  for (ii = 0;
       ii < COD_local_vars_table_entry_ct(dir->local_vars_table);
       ii++) {
    const COD_local_vars_entry_t *ety;
    size_t                        jj;

    ety = COD_local_vars_table_entry_get(dir->local_vars_table, ii);
    COD_local_vars_entry_write(dir, ety);
    sz += COD_LOCAL_VARS_ENTRY_SIZE;
    for (jj = 0; jj < COD_symbol_table_entry_ct(ety->symbols); jj++) {
      COD_symbol_entry_write(dir,
          COD_symbol_table_entry_get(ety->symbols, jj));
      sz += COD_SYMBOL_ENTRY_SIZE;
    }
  }
  sz %= 512;
  if (sz) {
    while (sz < 512) {
      uchar_write(dir->io, 0);
      sz++;
    }
  }
}

static ushort COD_local_vars_table_need_calc(const COD_directory_t *dir)
{
  ushort n;
  size_t ii;

  n = 0;
  for (ii = 0; 
       ii < COD_local_vars_table_entry_ct(dir->local_vars_table); 
       ii++) {
    const COD_local_vars_entry_t *ety;

    ety = COD_local_vars_table_entry_get(dir->local_vars_table, ii);
    n += 1 + COD_symbol_table_entry_ct(ety->symbols);
  }
  return (n + 31) / 32;
}

static unsigned COD_long_symbol_table_write_0(FILE *dst,
    const COD_long_symbol_table_t *tbl)
{
  unsigned sz_total;

  sz_total = 0;
  if (tbl) {
    unsigned sz;
    size_t   ii;

    sz = 0;
    for (ii = 0; 
         ii < COD_long_symbol_table_entry_ct(tbl);
         ii++) {
      const COD_long_symbol_entry_t *ety;
      unsigned                       req;
      size_t                         jj;

      ety = COD_long_symbol_table_entry_get(tbl, ii);

      req = 1U + ety->name[0] + 1U + 1U + 4U;
      if (sz + req > COD_BLOCK_SIZE) {
        if (dst) {
          while (sz < COD_BLOCK_SIZE) {
            uchar_write(dst, 0);
            sz++;
          }
        }
        sz_total += COD_BLOCK_SIZE - sz;
        sz = 0;
      }
      if (dst) {
        for (jj = 0; jj < 1 + (size_t) (ety->name[0]); jj++) {
          uchar_write(dst, ety->name[jj]);
        }
        uchar_write(dst, ety->stype0);
        uchar_write(dst, ety->stype1);
        /* for some reason, the long symbol table writes the entries
         * BIG endian */
        uchar_write(dst, (uchar) (ety->svalue >> 24));
        uchar_write(dst, (uchar) (ety->svalue >> 16));
        uchar_write(dst, (uchar) (ety->svalue >>  8));
        uchar_write(dst, (uchar) (ety->svalue >>  0));
      }
      sz       += req;
      sz_total += req;
    }
    if (sz) {
      if (dst) {
        while (sz < COD_BLOCK_SIZE) {
          uchar_write(dst, 0);
          sz++;
        }
      }
      sz_total += COD_BLOCK_SIZE - sz;
    }
  }
  return sz_total / COD_BLOCK_SIZE;
}

void COD_long_symbol_table_write(const COD_directory_t *dir)
{
  COD_long_symbol_table_write_0(dir->io, dir->long_symbol_table);
}

static ushort COD_long_symbol_table_need_calc(const COD_directory_t *dir)
{
  return (ushort) COD_long_symbol_table_write_0(0, dir->long_symbol_table);
}

static const struct {
  size_t               tbl_offset;
  size_t               map_start_offset;
  size_t               map_end_offset;
  ushort               entry_sz;
  entry_write_t        entry_write;
} COD_table_endpoints[] = {
  { offsetof(COD_directory_t, symbol_table),
    offsetof(COD_directory_t, symbol_table_start),
    offsetof(COD_directory_t, symbol_table_end),
    COD_SYMBOL_ENTRY_SIZE,
    COD_symbol_entry_write
  },
  { offsetof(COD_directory_t, name_table),
    offsetof(COD_directory_t, name_table_start),
    offsetof(COD_directory_t, name_table_end),
    COD_NAME_ENTRY_SIZE,
    COD_name_entry_write},
  { offsetof(COD_directory_t, xref_table),
    offsetof(COD_directory_t, list_table_start),
    offsetof(COD_directory_t, list_table_end),
    COD_LINE_SYMBOL_ENTRY_SIZE,
    COD_line_symbol_entry_write},
  { offsetof(COD_directory_t, code_map_table),
    offsetof(COD_directory_t, code_map_start),
    offsetof(COD_directory_t, code_map_end),
    COD_CODE_MAP_ENTRY_SIZE,
    COD_code_map_entry_write},
};

void COD_directory_free(COD_directory_t *dir)
{
  if (dir) {
    size_t ii;

    for (ii = 0; ii < 128; ii++) {
      free(dir->code[ii]);
    }
    COD_symbol_table_free(dir->symbol_table);
    COD_name_table_free(dir->name_table);
    COD_line_symbol_table_free(dir->xref_table);
    COD_code_map_table_free(dir->code_map_table);
    COD_local_vars_table_free(dir->local_vars_table);
    COD_long_symbol_table_free(dir->long_symbol_table);
    COD_message_table_free(dir->message_table);
    free(dir);
  }
}

static void string_assign(uchar *dst, size_t dst_sz, const uchar *src)
{
  size_t sz;

  sz = (src) ? strlen((const char *) src) : 0;
  if (sz > dst_sz - 1) {
    sz = dst_sz - 1;
  }
  memset(dst, 0, dst_sz);
  dst[0] = (uchar) sz;
  if (sz) {
    memcpy(dst + 1, src, sz);
  }
}

uchar COD_name_entry_add(COD_directory_t *dir, const uchar *label)
{
  uchar ret;

  ret = (uchar) -1;

  if (dir) {
    if (label) {
      COD_name_entry_t *ety;

      if (!dir->name_table) {
        dir->name_table = COD_name_table_alloc(1);
        string_assign(dir->source, sizeof(dir->source), label);
      }
      if (dir->name_table) {
        ety = COD_name_table_entry_append(dir->name_table, 0);
        if (ety) {
          string_assign(ety->label, sizeof(ety->label), label);
          ret = (uchar) (COD_name_table_entry_ct(dir->name_table) - 1);
        }
      }
    }
  }
  return ret;
}

void COD_symbol_entry_add(COD_directory_t   *dir,
                          const uchar       *label,
                          COD_symbol_type_t  type,
                          ushort             value)
{
  /* first, add it to the small symbol table */
  if (dir) {
    if (!dir->symbol_table) {
      dir->symbol_table = COD_symbol_table_alloc(16);
    }
    if (dir->symbol_table) {
      COD_symbol_entry_t *ety;

      ety = COD_symbol_table_entry_append(dir->symbol_table, 0);
      if (ety) {
        string_assign(ety->label, sizeof(ety->label), label);
        ety->type  = type;
        ety->value = value;
      }
    }
    /* next, add it to the long symbol table */
    if (!dir->long_symbol_table) {
      dir->long_symbol_table = COD_long_symbol_table_alloc(16);
    }
    if (dir->long_symbol_table) {
      COD_long_symbol_entry_t *ety;

      ety = COD_long_symbol_table_entry_append(dir->long_symbol_table, 0);
      if (ety) {
        string_assign(ety->name, sizeof(ety->name), label);
        ety->stype0 = type;
        ety->stype1 = 0;
        ety->svalue = value;
      }
    }
  }
}

void COD_line_symbol_entry_add(COD_directory_t *dir,
                               uchar            file,
                               uchar            smod,
                               ushort           line,
                               ushort           sloc)
{
  if (dir) {
    if (!dir->xref_table) {
      dir->xref_table = COD_line_symbol_table_alloc(128);
    }
    if (dir->xref_table) {
      COD_line_symbol_entry_t *ety;

      ety = COD_line_symbol_table_entry_append(dir->xref_table, 0);
      if (ety) {
        ety->file = file;
        ety->smod = smod;
        ety->sloc = sloc;
        ety->line = line;
      }
    }
  }
}

void COD_code_entry_add(COD_directory_t *dir,
                        ushort           sloc,
                        uchar            code)
{
  /* add some code! */
  ushort block = sloc / COD_BLOCK_SIZE;

  if (dir) {
    if (!dir->code[block]) {
      dir->code[block] = malloc(sizeof(*dir->code[block]));
    }
    if (dir->code[block]) {
      dir->code[block]->data[sloc % COD_BLOCK_SIZE] = code;
      if (!dir->code_entry) {
        if (!dir->code_map_table) {
          dir->code_map_table = COD_code_map_table_alloc(16);
        }
        if (dir->code_map_table) {
          dir->code_entry = COD_code_map_table_entry_append(
              dir->code_map_table, 0);
          dir->code_entry->start = sloc;
          dir->code_entry->end   = sloc;
        }
      }
      if (dir->code_entry) {
        if (sloc + 1 == dir->code_entry->start) {
          dir->code_entry->start = sloc;
        } else if (sloc - 1 == dir->code_entry->end) {
          dir->code_entry->end = sloc;
        } else if ((sloc < dir->code_entry->start)
          || (sloc > dir->code_entry->end)) {
          /* start a new entry */
          dir->code_entry = COD_code_map_table_entry_append(
              dir->code_map_table, 0);
          if (dir->code_entry) {
            dir->code_entry->start = sloc;
            dir->code_entry->end   = sloc;
          }
        }
      }
    }
  }
}


COD_directory_t *COD_directory_alloc(const uchar *compiler)
{
  COD_directory_t *dir;
  size_t           ii;

  dir = malloc(sizeof(*dir));
  if (dir) {
    time_t    now;
    struct tm tm;
    static const char *months =
      "JanFebMarAprMayJun"
      "JulAugSepOctNovDec";
    uchar date[9];

    (void) time(&now);
    tm = *localtime(&now);
    sprintf((char *) date, "%02d%.3s%02d",
        tm.tm_mday, months + 3 * tm.tm_mon, tm.tm_year % 100);
    
    dir->io                       = 0;
    dir->block_start              = 0;
    dir->symbol_table             = 0;
    dir->name_table               = 0;
    dir->xref_table               = 0;
    dir->code_map_table           = 0;
    dir->local_vars_table         = 0;
    dir->long_symbol_table        = 0;
    dir->message_table            = 0;
    for (ii = 0; ii < COUNT(dir->code); ii++) {
      dir->code[ii] = 0;
    }
    dir->code_entry = 0;
    for (ii = 0; ii < COUNT(dir->index); ii++) {
      dir->index[ii] = 0;
    }
    string_assign(dir->source, sizeof(dir->source), 0);
    string_assign(dir->date, sizeof(dir->date), date);
    dir->time = (ushort) (tm.tm_hour * 100U + tm.tm_min);
    string_assign(dir->version, sizeof(dir->version), 0);
    string_assign(dir->compiler, sizeof(dir->compiler), compiler);
    string_assign(dir->notice, sizeof(dir->notice), 0);
    dir->symbol_table_start       = 0;
    dir->symbol_table_end         = 0;
    dir->name_table_start         = 0;
    dir->name_table_end           = 0;
    dir->list_table_start         = 0;
    dir->list_table_end           = 0;
    dir->addr_size                = 0;
    dir->high_addr                = 0;
    dir->next_dir                 = 0;
    dir->code_map_start           = 0;
    dir->code_map_end             = 0;
    dir->local_vars_start         = 0;
    dir->local_vars_end           = 0;
    dir->COD_type                 = 0;
    string_assign(dir->processor, sizeof(dir->processor), 0);
    dir->long_symbol_start        = 0;
    dir->long_symbol_end          = 0;
    dir->message_start            = 0;
    dir->message_end              = 0;
  }
  return dir;
}

void COD_directory_write(COD_directory_t *dir, const char *fname)
{
  if (dir && fname) {
    ushort block;
    size_t ii;
    ushort need;


    dir->io = fopen(fname, "wb");
    /* block 0 = directory
     * block 1 = first free block
     */
    block = 1;
    /* fixup any code */
    for (ii = 0; ii < 128; ii++) {
      if (dir->code[ii]) {
        dir->index[ii] = block;
        block++;
      }
    }
    for (ii = 0; ii < COUNT(COD_table_endpoints); ii++) {
      unsigned entries_per_block;
      array_t *tbl;

      tbl = *(array_t **) 
        (((uchar *) dir) + COD_table_endpoints[ii].tbl_offset);

      entries_per_block = (unsigned) (COD_BLOCK_SIZE 
          / COD_table_endpoints[ii].entry_sz);
      need = (ushort) ((array_ct(tbl) + entries_per_block - 1) 
          / entries_per_block);
      if (need) {
        uchar *base;

        base = (uchar *) dir;
        *(ushort *) (base + COD_table_endpoints[ii].map_start_offset) = block;
        *(ushort *) (base + COD_table_endpoints[ii].map_end_offset)   
          = block + need - 1;
        block += need;
      }
    }
    need = COD_local_vars_table_need_calc(dir);
    if (need) {
      dir->local_vars_start = block;
      dir->local_vars_end   = block + need - 1;
      block += need;
    }
    need = COD_long_symbol_table_need_calc(dir);
    if (need) {
      dir->long_symbol_start = block;
      dir->long_symbol_end   = block + need - 1;
      block += need;
    }
    /* write the index block     */
    for (ii = 0; ii < 128; ii++) {
      ushort_write(dir->io, dir->index[ii]);
    }
    /* write the directory block */
    string_write(dir->io, sizeof(dir->source),   dir->source);
    string_write(dir->io, sizeof(dir->date),     dir->date);
    ushort_write(dir->io, dir->time);
    string_write(dir->io, sizeof(dir->version),  dir->version);
    string_write(dir->io, sizeof(dir->compiler), dir->compiler);
    string_write(dir->io, sizeof(dir->notice),   dir->notice);
    ushort_write(dir->io, dir->symbol_table_start);
    ushort_write(dir->io, dir->symbol_table_end);
    ushort_write(dir->io, dir->name_table_start);
    ushort_write(dir->io, dir->name_table_end);
    ushort_write(dir->io, dir->list_table_start);
    ushort_write(dir->io, dir->list_table_end);
    uchar_write (dir->io, dir->addr_size);
    ushort_write(dir->io, dir->high_addr);
    ushort_write(dir->io, dir->next_dir);
    ushort_write(dir->io, dir->code_map_start);
    ushort_write(dir->io, dir->code_map_end);
    ushort_write(dir->io, dir->local_vars_start);
    ushort_write(dir->io, dir->local_vars_end);
    ushort_write(dir->io, dir->COD_type);
    string_write(dir->io, sizeof(dir->processor), dir->processor);
    ushort_write(dir->io, dir->long_symbol_start);
    ushort_write(dir->io, dir->long_symbol_end);
    ushort_write(dir->io, dir->message_start);
    ushort_write(dir->io, dir->message_end);
    /*
     * fill out the remaining bytes to the end of the block
     */
    for (ii = 0; ii < 42; ii++) {
      uchar_write(dir->io, 0);
    }
    /*
     * write any code
     */
    for (ii = 0; ii < 128; ii++) {
      if (dir->code[ii]) {
        (void) fwrite(dir->code[ii]->data, COD_BLOCK_SIZE, 1, dir->io);
      }
    }
    /*
     * finally, write any tables
     */
    for (ii = 0; ii < COUNT(COD_table_endpoints); ii++) {
      COD_table_write(dir,
          COD_table_endpoints[ii].tbl_offset,
          COD_table_endpoints[ii].entry_sz,
          COD_table_endpoints[ii].entry_write);
    }
    COD_local_vars_table_write(dir);
    COD_long_symbol_table_write(dir);
    fclose(dir->io);
  }
}

void COD_directory_processor_set(COD_directory_t *dir,
                                             const uchar *processor)
{
  if (dir) {
    size_t ii;

    string_assign(dir->processor, sizeof(dir->processor), processor);
    for (ii = 0; ii < sizeof(dir->processor); ii++) {
      dir->processor[ii] = (uchar) toupper((uchar) dir->processor[ii]);
    }
  }
}

