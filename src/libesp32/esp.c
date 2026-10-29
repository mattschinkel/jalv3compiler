/************************************************************
 **
 ** esp.c : ESP32 (Xtensa LX6) back-end -- code list + entry points
 **
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "../libutils/mem.h"
#include "../libcore/pfile.h"
#include "../libcore/pf_msg.h"
#include "../libcore/pf_proc.h"
#include "../libcore/label.h"
#include "../libcore/cmd.h"
#include "../libcore/variable.h"
#include "../libcore/vardef.h"
#include "esp32.h"
#include "esp.h"
#include "esp32_clock.h"
#include "esp_float.h"

void esp_const_pool_place(pfile_t *pf);

/* The code list is attached to file scope so multiple compilations
 * (and the cleanup pass) stay independent (mirrors mips.c). */
static esp_code_t esp_list_head;
static esp_code_t esp_list_tail;   /* tail pointer for O(1) append */
static esp_code_t esp_pool_head;   /* L32R literal pool (LIT nodes) */
static esp_code_t esp_pool_tail;
static unsigned long esp_entry_addr;
static esp_code_t esp_isr_vecbase_lit;  /* startup LIT, bound to the
                                         * vector-table base node when
                                         * the table is emitted */

/* ISR vector table body: 0x400 bytes of all-`ill` filler (a zero word
 * is `ill`); the level-1 entry slot at +0x340 is a real code node
 * appended between the two halves. */
static unsigned char esp_isr_vtab[0x400];

/* The single `pragma interrupt` procedure, or NULL.  A second one is a
 * hard error: all level-1 interrupts share one vector on the ESP32. */
static pfile_proc_t *esp_isr_proc_get(pfile_t *pf)
{
  pfile_proc_t *proc;
  pfile_proc_t *isr = NULL;

  for (proc = pfile_proc_root_get(pf);
      proc;
      proc = pfile_proc_next(proc)) {
    if (pfile_proc_flag_test(proc, PFILE_PROC_FLAG_INTERRUPT)) {
      if (isr) {
        pfile_log(pf, PFILE_LOG_ERR,
            "ESP32 supports a single `pragma interrupt` procedure");
        return isr;
      }
      isr = proc;
    }
  }
  return isr;
}

variable_sz_t esp_pointer_size_get(pfile_t *pf)
{
  UNUSED(pf);
  return 4;
}

/* the descriptor published through libpic12's back-end registry
 * (jal_main registers it for PIC_TARGET_CPU_ESP32); the clock hook
 * adapts esp32_clock_solve's boolean_t return to the descriptor shape */
static void esp32_clock_solve_maybe(pfile_t *pf)
{
  (void) esp32_clock_solve(pf);
}

const pic_backend_t *esp32_backend_get(void)
{
  static const pic_backend_t esp32_backend = {
    esp_cmd_generate,
    esp_cmd_dump,
    esp_code_cleanup,
    esp_code_emu,
    esp_pointer_size_get,
    esp32_clock_solve_maybe
  };

  return &esp32_backend;
}

esp_code_t esp_code_list_head(pfile_t *pf)
{
  UNUSED(pf);
  return esp_list_head;
}

esp_code_t esp_code_list_tail(pfile_t *pf)
{
  UNUSED(pf);
  return esp_list_tail;
}

void esp_code_list_set_ends(pfile_t *pf, esp_code_t head, esp_code_t tail)
{
  UNUSED(pf);
  esp_list_head = head;
  esp_list_tail = tail;
}

void esp_code_list_reset(pfile_t *pf)
{
  UNUSED(pf);
  while (esp_list_head) {
    esp_code_t next = esp_list_head->link;
    FREE(esp_list_head);
    esp_list_head = next;
  }
  esp_list_tail = 0;
  esp_pool_head = 0;
  esp_pool_tail = 0;
  esp_entry_addr = 0;
  esp_isr_vecbase_lit = 0;
  esp_op_reset();               /* branch fixups + helper state */
  esp_const_pool_reset();       /* 3e */
}

/* append a node to the tail (kept via a tail pointer for O(1) append) */
static esp_code_t esp_code_append(esp_code_t node)
{
  if (!esp_list_head) {
    esp_list_head = node;
  } else {
    esp_list_tail->link = node;
  }
  esp_list_tail = node;
  node->link = NULL;
  return node;
}

esp_code_t esp_code_alloc(esp_opcode_t op, unsigned at,
    unsigned as, int imm)
{
  esp_code_t node = MALLOC(sizeof(*node));

  if (node) {
    memset(node, 0, sizeof(*node));
    node->op  = op;
    node->at  = at;
    node->as  = as;
    node->imm = imm;
    node->enc_sz = (unsigned short) ((ESP_OP_LABEL == op)
        ? 0 : ESP32_INSN_SZ);
    esp_code_append(node);
  }
  return node;
}

esp_code_t esp_code_label_alloc(void)
{
  return esp_code_alloc(ESP_OP_LABEL, 0, 0, 0);
}

/* literal pool: linear dedup on value (the LUI-CSE analogue) */
esp_code_t esp_pool_lit(unsigned long value)
{
  esp_code_t node;

  for (node = esp_pool_head; node; node = node->link) {
    if (node->imm32 == value) {
      return node;
    }
  }
  node = MALLOC(sizeof(*node));
  if (node) {
    memset(node, 0, sizeof(*node));
    node->op     = ESP_OP_LIT;
    node->imm32  = value;
    node->enc_sz = ESP32_LIT_SZ;
    node->link   = NULL;
    if (!esp_pool_head) {
      esp_pool_head = node;
    } else {
      esp_pool_tail->link = node;
    }
    esp_pool_tail = node;
  }
  return node;
}

unsigned long esp_entry_addr_get(void)
{
  return esp_entry_addr;
}

/* label-address literal: imm32 is filled from the label's code node
 * during layout (addresses are only final then) */
esp_code_t esp_pool_label(esp_code_t label_node)
{
  esp_code_t node;

  if (!label_node) {
    return (esp_code_t) 0;
  }
  for (node = esp_pool_head; node; node = node->link) {
    if ((ESP_OP_LIT == node->op) && (node->target == label_node)) {
      return node;
    }
  }
  node = MALLOC(sizeof(*node));
  if (node) {
    memset(node, 0, sizeof(*node));
    node->op     = ESP_OP_LIT;
    node->target = label_node;
    node->enc_sz = ESP32_LIT_SZ;
    node->link   = NULL;
    if (!esp_pool_head) {
      esp_pool_head = node;
    } else {
      esp_pool_tail->link = node;
    }
    esp_pool_tail = node;
  }
  return node;
}

/* fresh unbound pool literal (no dedup) -- the caller binds ->target
 * to a label's code node later (e.g. outlined helper call sites) */
esp_code_t esp_pool_lit_new(void)
{
  esp_code_t node = MALLOC(sizeof(*node));

  if (node) {
    memset(node, 0, sizeof(*node));
    node->op     = ESP_OP_LIT;
    node->enc_sz = ESP32_LIT_SZ;
    node->link   = NULL;
    if (!esp_pool_head) {
      esp_pool_head = node;
    } else {
      esp_pool_tail->link = node;
    }
    esp_pool_tail = node;
  }
  return node;
}

/* Prepend the literal pool to the code list, then assign addresses:
 * pool first (so every L32R looks backwards), then instructions.
 * Labels take no space. The image entry point is the first node
 * with a size (ROM jumps there, after the pool). */
void esp_code_layout(pfile_t *pf)
{
  esp_code_t   c;
  unsigned long addr;
  unsigned     pass;
  ulong        loader_offset;

  /* Prepend the pool (done once) */
  if (esp_pool_head) {
    esp_pool_tail->link = esp_list_head;
    esp_list_head = esp_pool_head;
    if (!esp_list_tail) {
      esp_list_tail = esp_pool_tail;
    }
    esp_pool_head = 0;
    esp_pool_tail = 0;
  }

  /* Iterative layout + branch relaxation:
   * out-of-range BRI8 branches are rewritten in-place as:
   *   [inverted branch, skip 6 bytes]  ← this node (3 bytes)
   *   [l32r a5, pool_lit(target_addr)]  ← NEW node (3 bytes)
   *   [jx a5]                           ← NEW node (3 bytes)
   * The skip target is a new LABEL node placed after the jx.
   * Pool literals are added via esp_pool_lit_new() (goes to the
   * separate pool list, prepended on the next layout pass). */
  for (pass = 0; pass < 100; pass++) {
    unsigned long relaxed_ct = 0;
    unsigned long pool_bytes = 0;

    /* compute pool size and assign pool literal addresses
     * (L32R nodes in the relaxation reference pool literals whose
     * addresses are needed for backward-offset computation) */
    addr = ESP32_IRAM_BASE;
    for (c = esp_pool_head; c; c = c->link) {
      c->addr = addr;
      addr += 4UL;
      pool_bytes += 4UL;
    }

    /* assign addresses (pool first, then code).  CONST_DATA starts
     * are 4-aligned: the image writer pads each const block back to
     * a 4 boundary, so the layout must place them the same way or
     * the data drifts ahead of the bound pool literals. */
    addr = ESP32_IRAM_BASE + pool_bytes;
    for (c = esp_list_head; c; c = c->link) {
      if (ESP_OP_CONST_DATA == c->op) {
        unsigned sz = ((unsigned) c->at + 3U) & ~3U;

        addr = (addr + 3UL) & ~3UL;
        if (c->isr_pad) {
          /* grow the pad so the vector table that follows starts
           * 0x400-aligned (VECBASE masking on hardware); recomputed
           * every pass, so branch-relaxation shifts stay covered */
          c->at = (unsigned) ((0x400UL - (addr & 0x3FFUL)) & 0x3FFUL);
          sz    = ((unsigned) c->at + 3U) & ~3U;
        }
        c->addr = addr;
        addr += (unsigned long) sz;
      } else {
        c->addr = addr;
        if (c->enc_sz) {
          addr += (unsigned long) c->enc_sz;
        }
      }
    }

    /* scan for out-of-range branches and relax them */
    for (c = esp_list_head; c; c = c->link) {
      boolean_t is_branch;
      boolean_t is_zbranch;
      long     delta;
      long     limit;

      if (ESP_OP_CONST_DATA == c->op || !c->enc_sz) {
        continue;
      }
      is_branch = (ESP_OP_BEQ == c->op || ESP_OP_BNE == c->op
          || ESP_OP_BLT == c->op || ESP_OP_BGE == c->op
          || ESP_OP_BLTU == c->op || ESP_OP_BGEU == c->op);
      is_zbranch = (ESP_OP_BEQZ == c->op || ESP_OP_BNEZ == c->op);
      if ((!is_branch && !is_zbranch) || !c->target) {
        continue;
      }
      /* BRI8 compare branches reach -128..127; BEQZ/BNEZ (BRI12)
       * reach -2048..2047 */
      limit = is_zbranch ? 2048L : 128L;
      delta = (long) c->target->addr - (long) (c->addr + 3UL);
      if ((delta >= -limit) && (delta <= (limit - 1L))) {
        continue;  /* fits */
      }

      /* Out of range: rewrite this node as the INVERTED branch
       * (skipping over the l32r+jx), then insert after it:
       * l32r (from pool) + jx + skip-label. */
      {
        esp_code_t ldr;
        esp_code_t jx;
        esp_code_t skip;
        esp_code_t tgt;
        esp_code_t after;

        tgt = c->target;  /* save original target */
        after = c->link;

        ldr = MALLOC(sizeof(*ldr));
        jx = MALLOC(sizeof(*jx));
        skip = MALLOC(sizeof(*skip));
        if (!ldr || !jx || !skip) {
          continue;
        }
        memset(ldr, 0, sizeof(*ldr));
        memset(jx, 0, sizeof(*jx));
        memset(skip, 0, sizeof(*skip));

        /* 1. Create a pool literal for the target address.
         * esp_pool_lit_new() creates an unbound LIT in the pool
         * list.  Bind it to the target code node so layout fills it. */
        {
          esp_code_t lit = esp_pool_lit_new();

          if (lit) {
            lit->target = tgt;  /* layout will set lit->imm32 = tgt->addr */
            /* 2. Set up the l32r to load from this pool literal */
            ldr->op = ESP_OP_L32R;
            ldr->at = ESP_REG_A5;
            ldr->enc_sz = ESP32_INSN_SZ;
            ldr->target = lit;
          }
        }

        /* 3. Set up the jx */
        jx->op = ESP_OP_JX;
        jx->as = ESP_REG_A5;
        jx->enc_sz = ESP32_INSN_SZ;

        /* 4. Set up the skip label (target of the inverted branch) */
        skip->op = ESP_OP_LABEL;
        skip->enc_sz = 0;

        /* 5. Invert the branch condition */
        switch (c->op) {
          case ESP_OP_BEQ:  c->op = ESP_OP_BNE;  break;
          case ESP_OP_BNE:  c->op = ESP_OP_BEQ;  break;
          case ESP_OP_BLT:  c->op = ESP_OP_BGE;  break;
          case ESP_OP_BGE:  c->op = ESP_OP_BLT;  break;
          case ESP_OP_BLTU: c->op = ESP_OP_BGEU; break;
          case ESP_OP_BEQZ: c->op = ESP_OP_BNEZ; break;
          case ESP_OP_BNEZ: c->op = ESP_OP_BEQZ; break;
          default:          c->op = ESP_OP_BLTU; break;
        }
        /* The inverted branch targets the skip label */
        c->target = skip;

        /* 6. Link: branch → l32r → jx → skip → old_next */
        c->link = ldr;
        ldr->link = jx;
        jx->link = skip;
        skip->link = after;

        /* advance past the inserted nodes */
        c = skip;
        relaxed_ct++;
      }
    }

    if (0 == relaxed_ct) {
      break;  /* fixpoint reached */
    }
    /* Merge any newly-created pool literals into the pool list */
    /* (esp_pool_lit_new() already adds to esp_pool_head/tail) */
    /* Recompute pool size on the next iteration */
  }

  /* FINAL layout: merge pool into code list (for the image writer) */
  if (esp_pool_head) {
    esp_pool_tail->link = esp_list_head;
    esp_list_head = esp_pool_head;
    if (!esp_list_tail) {
      esp_list_tail = esp_pool_tail;
    }
    esp_pool_head = 0;
    esp_pool_tail = 0;
  }

  /* entry address */
  addr = ESP32_IRAM_BASE;
  esp_entry_addr = 0;
  loader_offset = pfile_loader_offset_get(pf);
  
  /* If using bootloader offset (e.g., 0x10000), adjust entry point calculation */
  if (loader_offset == 0x10000) {
    /* Arduino bootloader scenario: code will be copied from 0x10000 to 0x0000 */
    /* Entry point should be relative to where the code will actually execute */
    for (c = esp_list_head; c; c = c->link) {
      if (ESP_OP_CONST_DATA == c->op) {
        unsigned sz = ((unsigned) c->at + 3U) & ~3U;

        addr = (addr + 3UL) & ~3UL;   /* matches the image writer */
        c->addr = addr;
        if (!esp_entry_addr) {
          esp_entry_addr = addr;
        }
        addr += (unsigned long) sz;
      } else {
        c->addr = addr;
        if (c->enc_sz) {
          if (!esp_entry_addr && (ESP_OP_LIT != c->op)) {
            esp_entry_addr = addr;
          }
          addr += (unsigned long) c->enc_sz;
        }
      }
    }
  } else {
    /* Normal entry point calculation */
    for (c = esp_list_head; c; c = c->link) {
      if (ESP_OP_CONST_DATA == c->op) {
        unsigned sz = ((unsigned) c->at + 3U) & ~3U;

        addr = (addr + 3UL) & ~3UL;   /* matches the image writer */
        c->addr = addr;
        if (!esp_entry_addr) {
          esp_entry_addr = addr;
        }
        addr += (unsigned long) sz;
      } else {
        c->addr = addr;
        if (c->enc_sz) {
          if (!esp_entry_addr && (ESP_OP_LIT != c->op)) {
            esp_entry_addr = addr;
          }
          addr += (unsigned long) c->enc_sz;
        }
      }
    }
  }
  /* label-address literals (l32r+jx/callx0 targets) need the final
   * code addresses, which are only known after the walk above */
  for (c = esp_list_head; c; c = c->link) {
    if ((ESP_OP_LIT == c->op) && c->target) {
      c->imm32 = c->target->addr;
    }
  }
  UNUSED(pf);
}

/* Real startup: stack init, PS.WOE clear (call0 guarantee: windowed
 * exceptions can never fire), watchdog disables so silicon matches
 * the QEMU dump-while-parked contract.  WDT register offsets are
 * hardware-verified: TIMG0/TIMG1 WDTWPROTECT/WDTCONFIG0 + RTC_CNTL. */
#define ESP32_SR_PS       230
#define ESP32_SR_INTENABLE 228
#define ESP32_SR_VECBASE  231
#define ESP32_WDT_KEY     0x50D83AA1UL
#define ESP32_TG0_WPRO    0x3FF5F064UL
#define ESP32_TG0_WCFG    0x3FF5F048UL
#define ESP32_TG1_WPRO    0x3FF60064UL
#define ESP32_TG1_WCFG    0x3FF60048UL
#define ESP32_RTC_WPRO    0x3FF480A4UL   /* IDF v4.4 rtc_cntl_reg.h */
#define ESP32_RTC_WCFG    0x3FF4808CUL

static void esp_startup_wdt_off(unsigned long wpro, unsigned long wcfg)
{
  esp_emit_l32r(ESP_REG_A2, esp_pool_lit(ESP32_WDT_KEY));
  esp_emit_l32r(ESP_REG_A4, esp_pool_lit(wpro));
  (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A2, ESP_REG_A4, 0);
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A2, 0, 0);
  esp_emit_l32r(ESP_REG_A4, esp_pool_lit(wcfg));
  (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A2, ESP_REG_A4, 0);
  /* relock (write 0 to WDTWPRO) skipped: bare metal keeps them off */
}

void esp_startup_emit(pfile_t *pf)
{
  /* a1 = stack top (grows down) */
  esp_emit_l32r(ESP_REG_A1, esp_pool_lit(ESP32_STACK_TOP));
  /* clear PS.WOE (bit 18): rsr a2,ps ; and ; wsr a2,ps ; esync */
  (void) esp_code_alloc(ESP_OP_RSR, ESP_REG_A2, 0, ESP32_SR_PS);
  esp_emit_l32r(ESP_REG_A3, esp_pool_lit(0xFFFBFFFFUL));
  (void) esp_code_alloc(ESP_OP_AND, ESP_REG_A2, ESP_REG_A2, (int) ESP_REG_A3);
  (void) esp_code_alloc(ESP_OP_WSR, ESP_REG_A2, 0, ESP32_SR_PS);
  (void) esp_code_alloc(ESP_OP_ESYNC, 0, 0, 0);
  /* ISR programs (3g): point VECBASE at the emitted vector table and
   * unmask the CPU interrupt the interrupt matrix routes to (int 1,
   * intlevel 1).  VECBASE first -- the literal is unbound until the
   * table node exists (bound in esp_cmd_generate, layout fills it).
   * INTENABLE after VECBASE so no interrupt can fire through a stale
   * table.  The LIT value 0 is a placeholder; ->target overrides it. */
  if (esp_isr_proc_get(pf)) {
    esp_isr_vecbase_lit = esp_pool_lit(0UL);
    esp_emit_l32r(ESP_REG_A3, esp_isr_vecbase_lit);
    (void) esp_code_alloc(ESP_OP_WSR, ESP_REG_A3, 0, ESP32_SR_VECBASE);
    (void) esp_code_alloc(ESP_OP_ESYNC, 0, 0, 0);
    (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A2, 0, 2); /* CPU int 1 */
    (void) esp_code_alloc(ESP_OP_WSR, ESP_REG_A2, 0, ESP32_SR_INTENABLE);
    (void) esp_code_alloc(ESP_OP_ESYNC, 0, 0, 0);
  }
  /* watchdogs off: unlock, config0 = 0, (relock skipped: bare metal) */
  esp_startup_wdt_off(ESP32_TG0_WPRO, ESP32_TG0_WCFG);
  esp_startup_wdt_off(ESP32_TG1_WPRO, ESP32_TG1_WCFG);
  esp_startup_wdt_off(ESP32_RTC_WPRO, ESP32_RTC_WCFG);
  /* delay timer: TIMG1 T0 free-run, divider 2 -> one tick per 25 ns
   * (APB domain -- exact on silicon, modeled by QEMU).  _usec_delay
   * polls T1LO (esp_delay_create); the timer also serves as a rough
   * uptime reference for programs that never delay. */
  esp_emit_l32r(ESP_REG_A2, esp_pool_lit(0x3FF60000UL)); /* TIMG1 T1CONFIG */
  esp_emit_l32r(ESP_REG_A3, esp_pool_lit(0xC0004000UL)); /* EN|INC|div2 */
  (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A3, ESP_REG_A2, 0);
  /* .bss-style zeroing of the data section: QEMU's ESP32 ROM boot
   * leaves staging garbage in DRAM, and uninitialized JAL globals
   * (heap string handles, string temps) must read as zero.  Real
   * firmware clears .bss the same way before main. */
  {
    unsigned long end = esp_data_end();

    if (end > ESP32_RAM_BASE) {
      esp_code_t loop;
      esp_code_t br;

      esp_emit_l32r(ESP_REG_A6, esp_pool_lit(ESP32_RAM_BASE));
      esp_emit_l32r(ESP_REG_A7, esp_pool_lit(end));
      (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A5, 0, 0);
      loop = esp_code_label_alloc();
      (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A5, ESP_REG_A6, 0);
      (void) esp_code_alloc(ESP_OP_ADDI, ESP_REG_A6, ESP_REG_A6, 4);
      br = esp_code_alloc(ESP_OP_BLTU, ESP_REG_A7, ESP_REG_A6, 0);
      if (br) {
        br->target = loop;          /* while (a6 < a7) *a6++ = 0 */
      }
    }
  }
  /* CPU clock select: NOT emitted inline in startup (known issue:
   * the full cold bring-up sequence crashes at the PLL source flip
   * when emitted inline, though the identical register sequence runs
   * reliably from a called procedure (demo_lib/esp32_clock.jal).
   * pragma target clock still drives the delay
   * budget; the clock switch itself is a library call. */
  UNUSED(pf);
}

/* _usec_delay polls TIMG1 T0 (free-run, divider 2 -> one tick per 25 ns
 * of APB time; configured in esp_startup_emit).  The old CPU-loop delay
 * calibrated a fixed cycles-per-iteration constant, but the loop's cost
 * is program-dependent: the instruction after the bnez IS its unfilled
 * delay slot and executes every iteration -- bench-measured 5 vs 6
 * cycles/iter on the SAME board in different programs (a 1.2 s
 * "1-second" delay; stopwatch-probed 2026-10-09).  The timer is exact
 * (APB domain; UART-proven) and follower-independent.  Pre-clock-switch
 * calls (esp32_clock.jal's internal tolerance waits) run at the 40 MHz
 * boot APB: half tick rate, so those waits double -- harmless. */
void esp_delay_create(pfile_t *pf, variable_const_t usec)
{
  unsigned long ticks = (unsigned long) usec * 40UL; /* 25 ns per tick */
  esp_code_t    loop;
  esp_code_t    br;

  UNUSED(pf);
  if (ticks < 40UL) {
    ticks = 40UL;  /* floor: one microsecond */
  }
  if (ticks > 0xFFFFFFF0UL) {
    ticks = 0xFFFFFFF0UL;  /* dword cap: ~107 s */
  }
  esp_emit_l32r(ESP_REG_A2, esp_pool_lit(0x3FF6000CUL)); /* T1UPDATE */
  esp_emit_l32r(ESP_REG_A3, esp_pool_lit(0x3FF60004UL)); /* T1LO */
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A4, 0, 0);
  (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A4, ESP_REG_A2, 0); /* latch */
  (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A5, ESP_REG_A3, 0); /* start */
  esp_emit_l32r(ESP_REG_A6, esp_pool_lit(ticks));
  loop = esp_code_label_alloc();
  (void) esp_code_alloc(ESP_OP_S32I, ESP_REG_A4, ESP_REG_A2, 0); /* latch */
  (void) esp_code_alloc(ESP_OP_L32I, ESP_REG_A7, ESP_REG_A3, 0); /* now */
  (void) esp_code_alloc(ESP_OP_SUB, ESP_REG_A7, ESP_REG_A7,
      (int) ESP_REG_A5);                                        /* elapsed */
  br = esp_code_alloc(ESP_OP_BLTU, ESP_REG_A6, ESP_REG_A7, 0);  /* elapsed < ticks */
  if (br) {
    br->target = loop;
  }
  /* explicit delay-slot filler: the insn after a taken branch is its
   * delay slot and executes every poll -- own it with a harmless movi
   * instead of letting the next statement's first insn run a million
   * times a second (the old loop got away with it only because its
   * follower's registers were stable) */
  (void) esp_code_alloc(ESP_OP_MOVI, ESP_REG_A4, 0, 0);
}

void esp_cmd_generate(pfile_t *pf, const cmd_t cmd)
{
  cmd_t cmd_ptr;

  /* pass 1: allocate variables linearly in DRAM */
  esp_var_alloc(pf);

  /* real startup: sp init, PS.WOE clear, watchdogs off */
  esp_startup_emit(pf);

  /* pass 2: translate cmd_t -> esp_code_t */
  for (cmd_ptr = cmd; cmd_ptr; cmd_ptr = cmd_link_get(cmd_ptr)) {
    if (cmd_is_reachable(cmd_ptr)) {
      esp_op_generate(pf, cmd_ptr);
    }
  }
  esp_op_resolve_fixups(pf);

  /* outlined divide/mod helpers (after main, before the halt loop) */
  esp_helpers_create(pf);

  /* cooperative task start/suspend helpers (4h port); their forward
   * branches need a fixup pass of their own (created after the main
   * resolve above).  Only when the program actually uses tasks --
   * the pass walks all emitted code. */
  if (esp_tasks_used(pf)) {
    esp_task_intrinsics_create(pf);
    esp_op_resolve_fixups(pf);
  }

  /* const pool data nodes (3e): after all code+helpers */
  esp_float_intrinsics_create(pf);

  esp_const_pool_place(pf);

  /* append a forever loop so the program does not run off the end */  {
    esp_code_t loop = esp_code_label_alloc();
    esp_code_t j = esp_code_alloc(ESP_OP_J, 0, 0, 0);

    if (j) {
      j->target = loop;
    }
  }

  /* ISR vector table (3g), after everything: an alignment pad, 0x340
   * bytes of `ill` filler, the level-1 entry slot (user-exception
   * vector at VECBASE+0x340 -- level-1 interrupts arrive as EXCCAUSE
   * 2), then filler up to 0x400.  The pad grows at layout time so the
   * table base is 0x400-aligned: QEMU relocates vectors additively
   * (VECBASE + vecofs) and tolerated any base, but hardware masks the
   * VECBASE low bits.  The slot is l32r + jx, not a near J: a J needs
   * a 4-aligned ISR entry, which the 3-byte instruction stream does
   * not guarantee (only short programs landed aligned by luck). */
  {
    pfile_proc_t *isr = esp_isr_proc_get(pf);

    if (isr) {
      label_t    lbl  = pfile_proc_label_get(isr);
      esp_code_t pad  = esp_code_alloc(ESP_OP_CONST_DATA, 0, 0, 0);
      esp_code_t fill = esp_code_alloc(ESP_OP_CONST_DATA, 0x340, 0, 0);
      esp_code_t ldr;
      esp_code_t jx;

      if (pad) {
        pad->cdata   = esp_isr_vtab;
        pad->isr_pad = 1;
      }
      if (fill) {
        fill->cdata = esp_isr_vtab;
        if (esp_isr_vecbase_lit) {
          esp_isr_vecbase_lit->target = fill;  /* VECBASE = table base */
        }
      }
      ldr = esp_code_alloc(ESP_OP_L32R, ESP_REG_A5, 0, 0);
      jx  = esp_code_alloc(ESP_OP_JX, 0, ESP_REG_A5, 0);
      if (ldr && jx && lbl) {
        esp_code_t lit = esp_pool_label((esp_code_t) label_code_get(lbl));

        if (lit) {
          ldr->target = lit;  /* layout fills the literal from the node */
          jx->enc_sz  = ESP32_INSN_SZ;
        }
      }
      /* the layout 4-aligns CONST_DATA starts, so the trailing fill
       * sits at 0x346 (l32r + jx = 6 bytes + align pad) */
      fill = esp_code_alloc(ESP_OP_CONST_DATA, 0x400 - 0x346, 0, 0);
      if (fill) {
        fill->cdata = esp_isr_vtab + 0x346;
      }
    }
  }
}

/* image size from the last dump (the code list may be consumed by the
 * time cleanup runs; capture it where it is alive) */
static unsigned long esp_image_code_sz;

void esp_cmd_dump(pfile_t *pf, const cmd_t cmd, boolean_t first)
{
  UNUSED(cmd);
  if (first) {
    esp_code_t c;

    esp_code_layout(pf);
    esp_asm_equ_dump(pf);
    esp_asm_listing(pf);
    esp_image_write(pf);
    esp_image_code_sz = 24UL;  /* image header (8) + ext header (16) */
    for (c = esp_code_list_head(pf); c; c = c->link) {
      esp_image_code_sz += (unsigned long) c->enc_sz;
    }
  }
}

void esp_code_cleanup(pfile_t *pf)
{
  {
    /* order-lenient clock solve that never completed: OSC pragma
     * without a clock pragma (see esp32_clock.c) */
    value_t osc = pfile_value_find(pf, PFILE_LOG_NONE, "target_osc");

    if (osc) {
      value_t clk = pfile_value_find(pf, PFILE_LOG_NONE, "target_clock");

      if (!clk) {
        pfile_log(pf, PFILE_LOG_ERR,
          "pragma target OSC requires `pragma target clock`");
      }
      if (clk) {
        value_release(clk);
      }
      value_release(osc);
    }
  }
  if (!pfile_errct_get(pf)) {
    /* Usage summary (classic PICs print theirs in pic_code_cleanup;
     * figures here are the image layout, not PIC ROM words). */
    static const unsigned long flash_tab[16] = {
      512UL * 1024UL, 256UL * 1024UL, 1024UL * 1024UL, 2UL * 1024UL * 1024UL,
      4UL * 1024UL * 1024UL, 8UL * 1024UL * 1024UL, 16UL * 1024UL * 1024UL,
      32UL * 1024UL * 1024UL, 0, 0, 0, 0, 0, 0, 0, 0
    };
    unsigned long code_sz;
    unsigned long data_sz;
    unsigned long dram_sz;
    unsigned long flash_sz = 4UL * 1024UL * 1024UL;
    value_t      v;

    code_sz = esp_image_code_sz;
    v = pfile_value_find(pf, PFILE_LOG_NONE, "_flash_size");
    if (v) {
      unsigned nib = (unsigned) value_const_get(v) & 0xFU;

      if (flash_tab[nib]) {
        flash_sz = flash_tab[nib];
      }
      value_release(v);
    }
    pfile_log(pf, PFILE_LOG_INFO,
      "Code area: %lu of %lu used (bytes)", code_sz, flash_sz);

    data_sz = esp_data_end() - ESP32_RAM_BASE;
    dram_sz = ESP32_STACK_TOP - ESP32_RAM_BASE;
    pfile_log(pf, PFILE_LOG_INFO, "Data area: %lu of %lu used",
      data_sz, dram_sz);
    pfile_log(pf, PFILE_LOG_INFO,
      "Software stack available: %lu bytes", dram_sz - data_sz);
    /* no hardware call stack on Xtensa call0: nothing to report */
  }
  esp_code_list_reset(pf);
}

void esp_code_emu(pfile_t *pf)
{
  UNUSED(pf);
}
