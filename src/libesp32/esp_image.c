/************************************************************
 **
 ** esp_image.c : ESP32 boot image + Intel HEX writer
 **
 ** Builds the ESP image (we ARE the second-stage bootloader at
 ** flash offset 0x1000; format byte-pinned against
 ** esptool v4.7 and the ROM's own error messages):
 **   - 8-byte header: magic 0xE9, n_seg, flash_mode, size<<4|freq,
 **     entry (first instruction after the pool)
 **   - 16-byte extended header (wp 0xEE, cs/hd drv 0xFF, no SHA)
 **   - one IRAM segment: the pool + code bytes, padded to 4
 **   - checksum = XOR of segment data ^ 0xEF, placed as the last
 **     byte of a 16-byte block (0xFF padding before it)
 ** Every image byte is then pushed through libcore's
 ** pfile_write_hex() at address 0x1000+ (esptool v4+ flashes
 ** Intel HEX with embedded addresses directly).
 **
 ** Also carries the `v_<name> EQU` symbol dump for @sim_validate.
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include <string.h>
#include <stdio.h>

#include "../libutils/mem.h"
#include "../libcore/pfile.h"
#include "../libcore/pf_proc.h"
#include "../libcore/variable.h"
#include "../libcore/vardef.h"
#include "../libcore/pf_log.h"
#include "esp.h"

/* Emit `v_<name> EQU 0xNNNNNNNN` lines for allocated DRAM variables
 * so @sim_validate can resolve symbols the way @gpsim_validate does.
 * Addresses are absolute physical DRAM (QEMU `xp` dumps them
 * directly; no base subtraction). Explicit `at` SFRs are skipped. */
void esp_asm_equ_dump(pfile_t *pf)
{
  pfile_proc_t *proc;

  pfile_write(pf, pfile_write_asm,
      "; ESP32 DRAM symbols (absolute, base 0x%08lx)\n",
      (unsigned long) ESP32_RAM_BASE);
  for (proc = pfile_proc_root_get(pf);
      proc;
      proc = pfile_proc_next(proc)) {
    pfile_block_t *blk;

    for (blk = pfile_proc_block_root_get(proc);
        blk;
        blk = pfile_block_next(blk)) {
      variable_t vptr;

      for (vptr = pfile_block_variable_list_head(blk);
          vptr;
          vptr = variable_link_get(vptr)) {
        const char     *nm;
        unsigned        tag_n;
        variable_base_t base;

        if (!variable_assign_ct_get(vptr) && !variable_use_ct_get(vptr)) {
          continue;
        }
        if (VARIABLE_DEF_TYPE_FUNCTION == variable_type_get(vptr)) {
          continue;
        }
        if (variable_is_const(vptr)) {
          continue;
        }
        nm = variable_name_get(vptr);
        if (!nm || !*nm) {
          continue;
        }
        base = variable_base_get(vptr, 0);
        if (VARIABLE_BASE_UNKNOWN == base) {
          continue;
        }
        /* Explicit `at` in the peripheral/SFR range: not DRAM. */
        if ((unsigned long) base >= ESP32_ABS_BASE) {
          continue;
        }
        tag_n = variable_tag_n_get(vptr);
        if (tag_n) {
          pfile_write(pf, pfile_write_asm,
              "v___%s_%u EQU 0x%08x  ; %s\n",
              nm, tag_n,
              (unsigned) (ESP32_RAM_BASE + (unsigned long) base), nm);
        } else {
          pfile_write(pf, pfile_write_asm,
              "v_%s EQU 0x%08x  ; %s\n",
              nm,
              (unsigned) (ESP32_RAM_BASE + (unsigned long) base), nm);
        }
      }
    }
  }
  pfile_write(pf, pfile_write_asm, "\n");
}

/* growable byte buffer for the image */
typedef struct {
  unsigned char *data;
  unsigned long  len;
  unsigned long  cap;
} esp_buf_t;

static boolean_t esp_buf_put(esp_buf_t *b, unsigned char c)
{
  if (b->len == b->cap) {
    unsigned long ncap = b->cap ? b->cap * 2 : 256;
    unsigned char *nd  = REALLOC(b->data, ncap);

    if (!nd) {
      return BOOLEAN_FALSE;
    }
    b->data = nd;
    b->cap  = ncap;
  }
  b->data[b->len++] = c;
  return BOOLEAN_TRUE;
}

/* Instruction listing (dev tooling 2026-10-05, mips_asm_listing port):
 * walks the SAME laid-out code list the image writer serializes, in the
 * same order, printing each encoded instruction (3 bytes), pool literal
 * (4 bytes) and label anchor -- the -asm file becomes a faithful
 * companion of the flash image (qemu_inspect.py trace/diff consume it). */
static const char *esp_op_mnemonic(esp_opcode_t op)
{
  switch (op) {
    case ESP_OP_MOVI:    return "movi";
    case ESP_OP_L32R:    return "l32r";
    case ESP_OP_S8I:     return "s8i";
    case ESP_OP_S16I:    return "s16i";
    case ESP_OP_S32I:    return "s32i";
    case ESP_OP_J:       return "j";
    case ESP_OP_ADD:     return "add";
    case ESP_OP_SUB:     return "sub";
    case ESP_OP_AND:     return "and";
    case ESP_OP_OR:      return "or";
    case ESP_OP_XOR:     return "xor";
    case ESP_OP_NEG:     return "neg";
    case ESP_OP_MULL:    return "mull";
    case ESP_OP_MULUH:   return "muluh";
    case ESP_OP_SSL:     return "ssl";
    case ESP_OP_SSR:     return "ssr";
    case ESP_OP_SLL:     return "sll";
    case ESP_OP_SRL:     return "srl";
    case ESP_OP_SRA:     return "sra";
    case ESP_OP_ADDI:    return "addi";
    case ESP_OP_ADDMI:   return "addmi";
    case ESP_OP_L8UI:    return "l8ui";
    case ESP_OP_L16UI:   return "l16ui";
    case ESP_OP_L16SI:   return "l16si";
    case ESP_OP_L32I:    return "l32i";
    case ESP_OP_BEQ:     return "beq";
    case ESP_OP_BNE:     return "bne";
    case ESP_OP_BLT:     return "blt";
    case ESP_OP_BGE:     return "bge";
    case ESP_OP_BLTU:    return "bltu";
    case ESP_OP_BGEU:    return "bgeu";
    case ESP_OP_BEQZ:    return "beqz";
    case ESP_OP_BNEZ:    return "bnez";
    case ESP_OP_JX:      return "jx";
    case ESP_OP_CALLX0:  return "callx0";
    case ESP_OP_RSR:     return "rsr";
    case ESP_OP_WSR:     return "wsr";
    case ESP_OP_ESYNC:   return "esync";
    case ESP_OP_EXTUI:   return "extui";
    case ESP_OP_NSAU:    return "nsau";
    case ESP_OP_RFI:     return "rfi";
    case ESP_OP_RFE:     return "rfe";
    default:             return "?";
  }
}

void esp_asm_listing(pfile_t *pf)
{
  esp_code_t c;

  pfile_write(pf, pfile_write_asm,
      "; instruction listing (post-layout, same list as the image)\n");
  for (c = esp_code_list_head(pf); c; c = c->link) {
    if (ESP_OP_LABEL == c->op) {
      pfile_write(pf, pfile_write_asm, "%08lx:             <%lx>\n",
          c->addr, c->addr);
      continue;
    }
    if (ESP_OP_LIT == c->op) {
      pfile_write(pf, pfile_write_asm,
          "%08lx: %02x %02x %02x %02x  .lit 0x%08lx\n",
          c->addr,
          (unsigned) (c->imm32 & 0xFF), (unsigned) ((c->imm32 >> 8) & 0xFF),
          (unsigned) ((c->imm32 >> 16) & 0xFF),
          (unsigned) ((c->imm32 >> 24) & 0xFF), c->imm32);
      continue;
    }
    if (ESP_OP_CONST_DATA == c->op) {
      pfile_write(pf, pfile_write_asm, "%08lx:             .data %u bytes\n",
          c->addr, c->at);
      continue;
    }
    if (ESP32_INSN_SZ == c->enc_sz) {
      unsigned char out[ESP32_INSN_SZ];

      esp_inst_encode(pf, c, out);
      pfile_write(pf, pfile_write_asm, "%08lx: %02x %02x %02x  ",
          c->addr, out[0], out[1], out[2]);
      switch (c->op) {
        case ESP_OP_L32R:
          pfile_write(pf, pfile_write_asm, "l32r a%u, [%08lx]",
              c->at, c->target ? c->target->addr : 0UL);
          break;
        case ESP_OP_MOVI:
        case ESP_OP_SSL:
        case ESP_OP_SSR:
        case ESP_OP_BEQZ:
        case ESP_OP_BNEZ:
        case ESP_OP_NSAU:
          pfile_write(pf, pfile_write_asm, "%s a%u, 0x%x",
              esp_op_mnemonic(c->op), c->at, c->imm);
          break;
        case ESP_OP_ADDI:
        case ESP_OP_ADDMI:
          pfile_write(pf, pfile_write_asm, "%s a%u, a%u, 0x%x",
              esp_op_mnemonic(c->op), c->at, c->as, c->imm);
          break;
        case ESP_OP_L8UI:
        case ESP_OP_L16UI:
        case ESP_OP_L16SI:
        case ESP_OP_L32I:
        case ESP_OP_S8I:
        case ESP_OP_S16I:
        case ESP_OP_S32I:
          pfile_write(pf, pfile_write_asm, "%s a%u, a%u, %d",
              esp_op_mnemonic(c->op), c->at, c->as, c->imm);
          break;
        case ESP_OP_ADD:
        case ESP_OP_SUB:
        case ESP_OP_AND:
        case ESP_OP_OR:
        case ESP_OP_XOR:
        case ESP_OP_MULL:
        case ESP_OP_MULUH:
        case ESP_OP_SLL:
        case ESP_OP_SRL:
        case ESP_OP_SRA:
          pfile_write(pf, pfile_write_asm, "%s a%u, a%u, a%d",
              esp_op_mnemonic(c->op), c->at, c->as, c->imm);
          break;
        case ESP_OP_NEG:
          pfile_write(pf, pfile_write_asm, "neg a%u, a%u", c->at, c->as);
          break;
        case ESP_OP_BEQ:
        case ESP_OP_BNE:
        case ESP_OP_BLT:
        case ESP_OP_BGE:
        case ESP_OP_BLTU:
        case ESP_OP_BGEU:
          pfile_write(pf, pfile_write_asm, "%s a%u, a%u, %08lx",
              esp_op_mnemonic(c->op), c->as, c->at,
              c->target ? c->target->addr : 0UL);
          break;
        case ESP_OP_J:
          pfile_write(pf, pfile_write_asm, "j %08lx",
              c->target ? c->target->addr : 0UL);
          break;
        case ESP_OP_JX:
        case ESP_OP_CALLX0:
          pfile_write(pf, pfile_write_asm, "%s a%u",
              esp_op_mnemonic(c->op), c->as);
          break;
        case ESP_OP_RSR:
        case ESP_OP_WSR:
          pfile_write(pf, pfile_write_asm, "%s a%u, %d",
              esp_op_mnemonic(c->op), c->at, c->imm);
          break;
        case ESP_OP_EXTUI:
          pfile_write(pf, pfile_write_asm, "extui a%u, a%u, %d, %u",
              c->at, c->as, c->imm, c->src2);
          break;
        default:
          pfile_write(pf, pfile_write_asm, "%s",
              esp_op_mnemonic(c->op));
          break;
      }
      pfile_write(pf, pfile_write_asm, "\n");
    }
  }
  pfile_write(pf, pfile_write_asm, "\n");
}

void esp_image_write(pfile_t *pf)
{
  esp_buf_t   seg = {NULL, 0, 0};
  esp_buf_t   dseg = {NULL, 0, 0};
  esp_buf_t   img = {NULL, 0, 0};
  esp_code_t  c;
  unsigned long total;
  unsigned long ii;
  unsigned long loader_offset;
  unsigned      nsegs;
  unsigned char chk;

  /* segment 0 data: pool LITs + instruction bytes (+ ISR vector table
   * CONST_DATA nodes), in laid-out order */
  for (c = esp_code_list_head(pf); c; c = c->link) {
    if (ESP_OP_LIT == c->op) {
      if (!esp_buf_put(&seg, (unsigned char) (c->imm32))
          || !esp_buf_put(&seg, (unsigned char) (c->imm32 >> 8))
          || !esp_buf_put(&seg, (unsigned char) (c->imm32 >> 16))
          || !esp_buf_put(&seg, (unsigned char) (c->imm32 >> 24))) {
        goto oom;
      }
    } else if (ESP_OP_CONST_DATA == c->op) {
      const unsigned char *data = c->cdata;
      unsigned       k;

      for (k = 0; k < (unsigned) c->at; k++) {
        if (!esp_buf_put(&seg, data[k])) {
          goto oom;
        }
      }
      /* pad to 4-byte alignment */
      while (seg.len & 3UL) {
        if (!esp_buf_put(&seg, 0)) {
          goto oom;
        }
      }
    } else if (c->enc_sz == ESP32_INSN_SZ) {
      unsigned char out[ESP32_INSN_SZ];
      unsigned      k;

      esp_inst_encode(pf, c, out);
      for (k = 0; k < ESP32_INSN_SZ; k++) {
        if (!esp_buf_put(&seg, out[k])) {
          goto oom;
        }
      }
    }
  }
  if (!seg.len) {
    return;
  }

  /* segment 1 data: const-pool variables, loaded by the ROM straight
   * into DRAM (real silicon cannot D-side load from the 0x4008xxxx
   * I-bus region where segment 0 lives).  Each variable 4-aligned,
   * mirroring esp_const_pool_place's run layout. */
  for (ii = 0; ii < esp_const_pool_count(); ii++) {
    variable_t     v    = esp_const_pool_var((unsigned) ii);
    unsigned       sz   = v ? (unsigned) variable_sz_get(v) : 0;
    unsigned char *data = v ? (unsigned char *) variable_data_get(v) : 0;
    unsigned       k;

    if (!data) {
      continue;
    }
    for (k = 0; k < sz; k++) {
      if (!esp_buf_put(&dseg, data[k])) {
        goto oom;
      }
    }
    while (dseg.len & 3UL) {
      if (!esp_buf_put(&dseg, 0)) {
        goto oom;
      }
    }
  }
  nsegs = dseg.len ? 2U : 1U;

  /* header: 1 segment, flash mode/size/freq from pragmas (3h);
   * defaults DIO / 4 MB / 40 MHz */
  {
    unsigned flash_mode = 2;   /* DIO */
    unsigned flash_size = 0x2; /* 4 MB */
    unsigned flash_freq = 0x0; /* 40 MHz */
    value_t  v;

    v = pfile_value_find(pf, PFILE_LOG_NONE, "_flash_mode");
    if (v) {
      flash_mode = (unsigned) value_const_get(v) & 0xFF;
      value_release(v);
    }
    v = pfile_value_find(pf, PFILE_LOG_NONE, "_flash_size");
    if (v) {
      flash_size = (unsigned) value_const_get(v) & 0xF;
      value_release(v);
    }
    v = pfile_value_find(pf, PFILE_LOG_NONE, "_flash_freq");
    if (v) {
      flash_freq = (unsigned) value_const_get(v) & 0xF;
      value_release(v);
    }
    if (!esp_buf_put(&img, 0xE9) || !esp_buf_put(&img, nsegs)
        || !esp_buf_put(&img, (unsigned char) flash_mode)
        || !esp_buf_put(&img,
            (unsigned char) ((flash_size << 4) | flash_freq))) {
      goto oom;
    }
  }
  {
    unsigned long entry = esp_entry_addr_get();

    for (ii = 0; ii < 4; ii++) {
      if (!esp_buf_put(&img, (unsigned char) (entry >> (8 * ii)))) {
        goto oom;
      }
    }
  }
  /* extended header (16 bytes; byte-pinned) */
  {
    static const unsigned char ext[16] = {
      0xEE, 0, 0, 0, 0, 0, 0, 0,
      0, 0xFF, 0xFF, 0, 0, 0, 0, 0
    };

    for (ii = 0; ii < sizeof(ext); ii++) {
      if (!esp_buf_put(&img, ext[ii])) {
        goto oom;
      }
    }
  }
  /* segment header: load address + length (data padded to 4) */
  {
    unsigned long addr = ESP32_IRAM_BASE;
    unsigned long len  = (seg.len + 3UL) & ~3UL;

    for (ii = 0; ii < 4; ii++) {
      if (!esp_buf_put(&img, (unsigned char) (addr >> (8 * ii)))) {
        goto oom;
      }
    }
    for (ii = 0; ii < 4; ii++) {
      if (!esp_buf_put(&img, (unsigned char) (len >> (8 * ii)))) {
        goto oom;
      }
    }
  }
  /* segment data + zero padding to 4 */
  for (ii = 0; ii < seg.len; ii++) {
    if (!esp_buf_put(&img, seg.data[ii])) {
      goto oom;
    }
  }
  while (img.len & 3UL) {
    if (!esp_buf_put(&img, 0)) {
      goto oom;
    }
  }
  /* const-pool segment header + data (load address = DRAM data end) */
  if (dseg.len) {
    unsigned long daddr = esp_const_pool_base();

    for (ii = 0; ii < 4; ii++) {
      if (!esp_buf_put(&img, (unsigned char) (daddr >> (8 * ii)))) {
        goto oom;
      }
    }
    for (ii = 0; ii < 4; ii++) {
      if (!esp_buf_put(&img, (unsigned char) (dseg.len >> (8 * ii)))) {
        goto oom;
      }
    }
    for (ii = 0; ii < dseg.len; ii++) {
      if (!esp_buf_put(&img, dseg.data[ii])) {
        goto oom;
      }
    }
  }
  /* checksum over all segment data (incl. padding), 0xFF pad so it
   * lands as the last byte of a 16-byte block */
  chk = 0xEF;
  for (ii = 0; ii < seg.len; ii++) {
    chk ^= seg.data[ii];
  }
  total = (seg.len + 3UL) & ~3UL;
  for (ii = seg.len; ii < total; ii++) {
    chk ^= 0;                     /* padding contributes 0 */
  }
  for (ii = 0; ii < dseg.len; ii++) {
    chk ^= dseg.data[ii];         /* const-pool segment */
  }
  while (((img.len + 1) & 0xFUL) != 0) {
    if (!esp_buf_put(&img, 0xFF)) {
      goto oom;
    }
  }
  if (!esp_buf_put(&img, chk)) {
    goto oom;
  }

  /* write every image byte into the Intel HEX:
   * - pragma bootloader <offset>: image placed at that flash offset so it
   *   can be flashed beside an existing bootloader (e.g. Arduino app slot
   *   0x10000) without disturbing the boot path -- the bootloader pragma
   *   always wins
   * - else pragma code_location (device-file default or program): image
   *   at that flash offset
   * - else the second-stage-bootloader slot 0x1000, which the ESP32 ROM
   *   loads directly (QEMU and bare hardware) */
  loader_offset = pfile_loader_offset_get(pf);
  {
    ulong code_loc = pfile_code_location_get(pf);
    ulong write_base = loader_offset
        ? loader_offset
        : (code_loc ? code_loc : ESP32_FLASH_OFF);

    for (ii = 0; ii < img.len; ii++) {
      pfile_write_hex(pf, write_base + ii, img.data[ii]);
    }
  }

  FREE(seg.data);
  FREE(dseg.data);
  FREE(img.data);
  return;

oom:
  FREE(seg.data);
  FREE(dseg.data);
  FREE(img.data);
  pfile_log(pf, PFILE_LOG_ERR, "ESP32 image build out of memory");
}
