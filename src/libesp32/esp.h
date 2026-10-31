/************************************************************
 **
 ** esp.h : ESP32 (Xtensa LX6) back-end internal IR + code list
 **
 ** esp_code_t is a singly-linked list of Xtensa instructions
 ** (one node per instruction, 3 bytes each; density
 ** 16-bit forms are a later opt-in). ESP_OP_LABEL nodes take no
 ** space; ESP_OP_LIT nodes are 4-byte literal-pool words used by
 ** L32R (the pool must sit below every use -- the assembler
 ** enforces this and so does our layout).
 **
 ** Encoding convention (byte vectors pinned against the golden
 ** xtensa-esp32-elf assembler): instructions are 3 bytes in memory.
 ** Per-instruction byte
 ** recipes live in esp_inst.c. objdump displays the bytes
 ** MSB-first (reversed vs memory) -- never copy display output.
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#ifndef _esp_h__
#define _esp_h__

#include "../libcore/pfile.h"
#include "../libcore/operator.h"
#include "../libcore/cmd.h"
#include "../libcore/value.h"
#include "../libcore/variable.h"
#include "../libcore/label.h"

/* Xtensa registers under the call0 ABI (no window rotation).
 * a0 = link (call0/ret), a1 = stack pointer; a2-a7 are the
 * primary scratch set. */
#define ESP_REG_A0  0
#define ESP_REG_A1  1
#define ESP_REG_A2  2
#define ESP_REG_A3  3
#define ESP_REG_A4  4
#define ESP_REG_A5  5
#define ESP_REG_A6  6
#define ESP_REG_A7  7
#define ESP_REG_A8  8
#define ESP_REG_A9  9
#define ESP_REG_A10 10
#define ESP_REG_A11 11
#define ESP_REG_A12 12
#define ESP_REG_A13 13
#define ESP_REG_A14 14
#define ESP_REG_A15 15

/* ESP32-WROOM-32 memory map (classic ESP32). */
#define ESP32_IRAM_BASE   0x40080000UL  /* code + literal pool        */
/* Variables start 4 KB above the classic 0x3FFB0000 DRAM base: the
 * boot ROM re-stages the image at 0x3FFB0000 on every reset (watchdog
 * included), which races RAM dumps and would clobber low variables
 * after any re-boot.  0x3FFB1000 is clear of that scratch (QEMU-
 * verified). */
#define ESP32_RAM_BASE    0x3FFB1000UL
#define ESP32_ABS_BASE    0x3FF00000UL  /* explicit `at` at/above this is an
                                           absolute peripheral/SFR virtual
                                           address (device files), not a
                                           DRAM offset                */
#define ESP32_STACK_TOP   0x3FFFF000UL  /* grows down                */
#define ESP32_UART0_BASE  0x3FF40000UL
/* GPR data must stay below this offset so the stack has a reserve. */
#define ESP32_SRAM_DATA_MAX 0x0004F000UL
#define ESP32_INSN_SZ     3
/* Xtensa special registers (ISR): */
#define ESP32_SR_EPC1      177   /* exception PC for level 1 */
#define ESP32_SR_INTENABLE 228   /* interrupt enable mask */
#define ESP32_SR_VECBASE   231   /* exception vector base */
#define ESP32_SR_INTERRUPT 232   /* interrupt pending */
#define ESP32_LIT_SZ      4
/* ESP boot image at flash offset 0x1000 (we ARE the 2nd stage). */
#define ESP32_FLASH_OFF   0x1000UL

typedef enum {
  ESP_OP_NONE = 0,
  ESP_OP_MOVI,    /* at = imm12 (signed)                    */
  ESP_OP_L32R,    /* at = [target LIT]                      */
  ESP_OP_S8I,     /* [as + imm*1] = at                      */
  ESP_OP_S16I,    /* [as + imm*2] = at                      */
  ESP_OP_S32I,    /* [as + imm*4] = at                      */
  ESP_OP_J,       /* pc = target (label node)               */
  ESP_OP_LIT,     /* literal-pool word (imm32), 4 bytes     */
  ESP_OP_LABEL,   /* label, no space                         */
  /* encodings pinned against the golden assembler: */
  ESP_OP_ADD,     /* RRR: at = as + imm(reg r)              */
  ESP_OP_SUB,     /* at = as - r                            */
  ESP_OP_AND,     /* at = as & r                            */
  ESP_OP_OR,      /* at = as | r                            */
  ESP_OP_XOR,     /* at = as ^ r                            */
  ESP_OP_NEG,     /* at = 0 - as                            */
  ESP_OP_MULL,    /* at = as * r (low 32)                   */
  ESP_OP_MULUH,   /* at = high32(as * r, unsigned)          */
  ESP_OP_SSL,     /* SAR = 32 - as                          */
  ESP_OP_SSR,     /* SAR = as                               */
  ESP_OP_SLL,     /* at = as << SAR                         */
  ESP_OP_SRL,     /* at = as >> SAR (logical)               */
  ESP_OP_SRA,     /* at = as >> SAR (arith)                 */
  ESP_OP_ADDI,    /* at = as + imm8 (signed)                */
  ESP_OP_ADDMI,   /* at = as + imm (multiple of 256)        */
  ESP_OP_L8UI,    /* at = [as + imm*1] (unsigned byte)      */
  ESP_OP_L16UI,   /* at = [as + imm*2]                      */
  ESP_OP_L16SI,   /* at = [as + imm*2] (signed)             */
  ESP_OP_L32I,    /* at = [as + imm*4]                      */
  ESP_OP_BEQ,     /* if (as == at) pc = target (byte off)   */
  ESP_OP_BNE,     /* if (as != at) pc = target              */
  ESP_OP_BLT,     /* if (as <  at) signed                   */
  ESP_OP_BGE,     /* if (as >= at) signed                   */
  ESP_OP_BLTU,    /* if (as <  at) unsigned                 */
  ESP_OP_BGEU,    /* if (as >= at) unsigned                 */
  ESP_OP_BEQZ,    /* if (as == 0) pc = target (byte off)    */
  ESP_OP_BNEZ,    /* if (as != 0) pc = target               */
  ESP_OP_JX,      /* pc = as                                */
  ESP_OP_CALLX0,  /* call as (call0 ABI: a0 = return)       */
  ESP_OP_RSR,     /* at = special reg [imm] (230=PS, 3=SAR) */
  ESP_OP_WSR,     /* special reg [imm] = at                 */
  ESP_OP_ESYNC,   /* barrier for WSR                        */
  ESP_OP_EXTUI,   /* at = (as >> imm) & ((1<<(imm2))-1):      */
  ESP_OP_NSAU,    /* at = leading-zero count of as             */
  ESP_OP_RFI,     /* return from level >= 2 interrupt         */
  ESP_OP_RFE,     /* return from level-1 interrupt/exception  */
  ESP_OP_CONST_DATA /* raw data bytes from the variable (3e);   */
                  /* imm = byte count; imm32 = data pointer     */
                  /* imm = shift 0..31, src2 rides in enc     */
  /* Add new opcodes at the end so existing .o enum values stay stable. */
} esp_opcode_t;

typedef struct esp_code_ *esp_code_t;
struct esp_code_ {
  esp_opcode_t   op;
  unsigned       at;       /* target register                    */
  unsigned       as;       /* source / base register             */
  int            imm;      /* immediate / scaled store offset    */
  unsigned       src2;     /* EXTUI mask-1 (second immediate)    */
  unsigned long  imm32;    /* LIT value                          */
  const unsigned char *cdata; /* CONST_DATA: the bytes (imm = len) */
  esp_code_t     link;
  esp_code_t     target;   /* J: label node; L32R: LIT node      */
  unsigned long  addr;     /* laid-out address                    */
  unsigned short enc_sz;   /* 3 (insn) / 4 (LIT) / 0 (label)     */
  unsigned       isr_pad;  /* CONST_DATA: grow to 0x400-align the
                            * next node (ISR vector-table base)  */
};

esp_code_t esp_code_alloc(esp_opcode_t op, unsigned at,
    unsigned as, int imm);
esp_code_t esp_code_label_alloc(void);
void       esp_code_list_reset(pfile_t *pf);
void       esp_code_layout(pfile_t *pf);
esp_code_t esp_code_list_head(pfile_t *pf);
esp_code_t esp_code_list_tail(pfile_t *pf);
void       esp_code_list_set_ends(pfile_t *pf, esp_code_t head,
               esp_code_t tail);
/* entry = first instruction after the pool (image entry point) */
unsigned long esp_entry_addr_get(void);

/* literal pool: dedup on value; returns the LIT node.  A label-address
 * literal (target = label's code node) is filled in during layout. */
esp_code_t esp_pool_lit(unsigned long value);
esp_code_t esp_pool_label(esp_code_t label_node);
esp_code_t esp_pool_lit_new(void);

unsigned long esp_var_ram(variable_t v);
void       esp_add_fixup(pfile_t *pf, esp_code_t node, label_t lbl);
void       esp_emit_l32r(unsigned reg, esp_code_t lit);
void       esp_pend_label_lit(pfile_t *pf, label_t lbl, esp_code_t lit);
void       esp_emit_label_addr(pfile_t *pf, unsigned reg, label_t lbl);
label_t    esp_task_helper_label(pfile_t *pf, const char *name);
void       esp_emit_helper_call(pfile_t *pf, const char *name);
void       esp_emit_helper_jmp(pfile_t *pf, const char *name);
void       esp_op_helper_bind(label_t lbl, esp_code_t node);
boolean_t  esp_tasks_used(pfile_t *pf);
void       esp_task_alloc(pfile_t *pf);
void       esp_emit_task_start(pfile_t *pf, const cmd_t cmd);
void       esp_emit_task_yield(pfile_t *pf);
void       esp_emit_task_suicide(pfile_t *pf);
void       esp_emit_task_suspend(pfile_t *pf, const cmd_t cmd);
void       esp_emit_task_resume(pfile_t *pf, const cmd_t cmd);
void       esp_emit_task_kill(pfile_t *pf, const cmd_t cmd);
void       esp_emit_task_restart(pfile_t *pf, const cmd_t cmd);
void       esp_task_intrinsics_create(pfile_t *pf);

/* variable allocation (esp_var.c) */
void       esp_var_alloc(pfile_t *pf);
unsigned long esp_data_end(void);
variable_sz_t esp_def_storage_sz(pfile_t *pf, variable_def_t def);
/* the indirect-call protocol ops (defined in esp_op.c; the var
 * allocator sizes the shared inbox through them) */
#include "../libcore/indirect_alg.h"
extern const ialg_ops_t esp_ialg_ops;
/* emit-time placement (3c `_esp_bitarg_N` normalize buffers) */
void       esp_var_place(pfile_t *pf, variable_t v, variable_sz_t sz);
void       esp_var_ensure(pfile_t *pf, variable_t v);
void       esp_value_ensure(pfile_t *pf, value_t val);

/* const pool (3e): const arrays with initializer data are emitted as a
 * second boot segment the ROM loads into DRAM (real silicon cannot
 * D-side load from the I-bus region where code lives); address
 * literals point at the DRAM copies */
boolean_t  esp_var_is_const_data(variable_t v);
void       esp_const_pool_add(variable_t v);
void       esp_const_pool_reset(void);
esp_code_t esp_const_data_lit(variable_t v);
void       esp_const_pool_place(pfile_t *pf);
unsigned       esp_const_pool_count(void);
variable_t     esp_const_pool_var(unsigned ii);
unsigned long  esp_const_pool_base(void);

/* soft-float (esp_float.c) */
void       esp_float_intrinsics_create(pfile_t *pf);
boolean_t  esp_float_intrinsics_needed(pfile_t *pf);
void       esp_float_call(pfile_t *pf, unsigned idx);
void       esp_float_reset(void);

/* cmd_t -> esp_code_t (esp_op.c) */
void       esp_op_generate(pfile_t *pf, const cmd_t cmd);
void       esp_helpers_create(pfile_t *pf);   /* __esp32_*div/mod */
void       esp_op_resolve_fixups(pfile_t *pf);
void       esp_op_reset(void);

/* TEARDOWN: like esp_op_reset() but also frees the pending tables, so a
 * second compile in the same process starts clean and leak-free (A4).
 * Call while the back end is idle. */
void       esp_op_state_reset(void);

void       esp_var_state_reset(void);
void       esp_startup_emit(pfile_t *pf);   /* sp/WOE/WDT */
void       esp_delay_create(pfile_t *pf, variable_const_t usec);

/* instruction encoding (esp_inst.c): writes 3 bytes */
void       esp_inst_encode(pfile_t *pf, const esp_code_t code,
               unsigned char out[ESP32_INSN_SZ]);

/* ESP image + Intel HEX writer (esp_image.c) */
void       esp_image_write(pfile_t *pf);
void       esp_asm_equ_dump(pfile_t *pf);
void       esp_asm_listing(pfile_t *pf);

#endif /* _esp_h__ */
