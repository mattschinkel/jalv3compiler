/************************************************************
 **
 ** esp_inst.c : ESP32 (Xtensa LX6) 24-bit instruction encoding
 **
 ** Byte recipes pinned against the golden xtensa-esp32-elf
 ** assembler (GAS 2.46).  All instructions are the 24-bit core
 ** forms; density (16-bit)
 ** encodings are a later opt-in. objdump displays instruction
 ** bytes MSB-first (reversed vs memory) -- do not copy its output.
 **
 ** L32R: vAddr = ((PC+3) & ~3) + (imm16 - 0x10000) * 4
 **       (pool word must sit BELOW the load, 4..256 KB back)
 ** J:    target = PC + 4 + byte-offset (offset = 18-bit signed,
 **        4-aligned; encoded little-endian over the 24-bit word)
 ** Copyright (c) 2026, Matthew Schinkel
 ** All rights reserved
 **
 ************************************************************/
#include "../libcore/pfile.h"
#include "../libcore/pf_log.h"
#include "esp.h"

void esp_inst_encode(pfile_t *pf, const esp_code_t code,
    unsigned char out[ESP32_INSN_SZ])
{
  switch (code->op) {
    case ESP_OP_MOVI: {
      /* MOVI at, imm12:  [ (at<<4)|2, 0xA0|(imm>>8), imm ] */
      int imm = code->imm;

      out[0] = (unsigned char) (((code->at & 0xF) << 4) | 0x2);
      out[1] = (unsigned char) (0xA0 | ((imm >> 8) & 0xF));
      out[2] = (unsigned char) (imm & 0xFF);
      break;
    }

    case ESP_OP_L32R: {
      /* L32R at, lit:  [ (at<<4)|1, imm16_lo, imm16_hi ] with
       * imm16 = 0x10000 - (offset/4), offset = aligned_base - lit.
       * GAS-verified (golden vectors, lit at 0, insns at 0x4/0x9/
       * 0xe/0x13): base = align4(addr+3) for every alignment --
       * addr+4 for 4-aligned addresses reads one slot forward. */
      unsigned long base = (code->addr + 3UL) & ~3UL;
      long          offset;
      unsigned      imm16;

      if (!code->target) {
        pfile_log(pf, PFILE_LOG_ERR, "ESP32 L32R without literal");
        offset = 4;
      } else {
        offset = (long) (base - code->target->addr);
      }
      if ((offset < 4) || (offset > 0x40000L) || (offset & 3)) {
        pfile_log(pf, PFILE_LOG_ERR,
            "ESP32 L32R literal out of reach (%ld)", offset);
        offset = 4;
      }
      imm16 = (unsigned) (0x10000UL - ((unsigned long) offset >> 2));
      out[0] = (unsigned char) (((code->at & 0xF) << 4) | 0x1);
      out[1] = (unsigned char) (imm16 & 0xFF);
      out[2] = (unsigned char) (imm16 >> 8);
      break;
    }

    case ESP_OP_S8I:
      /* S8I at, as, imm:  [ (at<<4)|2, 0x40|as, imm ] */
      out[0] = (unsigned char) (((code->at & 0xF) << 4) | 0x2);
      out[1] = (unsigned char) (0x40 | (code->as & 0xF));
      out[2] = (unsigned char) (code->imm & 0xFF);
      break;

    case ESP_OP_S16I:
      /* S16I at, as, imm: [ (at<<4)|2, 0x50|as, imm ] (imm = bytes/2) */
      out[0] = (unsigned char) (((code->at & 0xF) << 4) | 0x2);
      out[1] = (unsigned char) (0x50 | (code->as & 0xF));
      out[2] = (unsigned char) (code->imm & 0xFF);
      break;

    case ESP_OP_S32I:
      /* S32I at, as, imm: [ (at<<4)|2, 0x60|as, imm ] (imm = bytes/4) */
      out[0] = (unsigned char) (((code->at & 0xF) << 4) | 0x2);
      out[1] = (unsigned char) (0x60 | (code->as & 0xF));
      out[2] = (unsigned char) (code->imm & 0xFF);
      break;

    case ESP_OP_J: {
      /* J near target: m0=0x06, imm16 = delta/4 (delta = target-(PC+4),
       * must be 4-aligned); bytes [06, imm_lo, imm_hi]. Far/unaligned
       * targets go through l32r + jx at emission instead. */
      long offset;
      int  imm;

      if (!code->target) {
        pfile_log(pf, PFILE_LOG_ERR, "ESP32 J without target");
        offset = -4;
      } else {
        offset = (long) code->target->addr - (long) (code->addr + 4UL);
      }
      if ((offset & 3L) || (offset < -131072L) || (offset > 131068L)) {
        pfile_log(pf, PFILE_LOG_ERR,
            "ESP32 J target out of near range (%ld)", offset);
        offset = -4;
      }
      imm = (int) (offset >> 2);
      out[0] = 0x06;
      out[1] = (unsigned char) (imm & 0xFF);
      out[2] = (unsigned char) ((imm >> 8) & 0xFF);
      break;
    }

    case ESP_OP_RSR:
      /* RSR at, sr: [ (at<<4), sr, 0x03 ] (rsr.ps a2 = 20 e6 03) */
      out[0] = (unsigned char) ((code->at & 0xF) << 4);
      out[1] = (unsigned char) (code->imm & 0xFF);
      out[2] = 0x03;
      break;

    case ESP_OP_WSR:
      /* WSR at, sr: [ (at<<4), sr, 0x13 ] (wsr.ps a2 = 20 e6 13) */
      out[0] = (unsigned char) ((code->at & 0xF) << 4);
      out[1] = (unsigned char) (code->imm & 0xFF);
      out[2] = 0x13;
      break;

    case ESP_OP_ESYNC:
      out[0] = 0x20;
      out[1] = 0x20;
      out[2] = 0x00;
      break;

    case ESP_OP_EXTUI: {
      /* EXTUI at, as, shimm, mask: [ (as<<4), (at<<4)|sh,
       * ((mask-1)<<4)|4 ]  (extui a2,a3,5,4 = 30 25 34) */
      unsigned sh  = (unsigned) code->imm & 0x1F;
      unsigned msk = ((unsigned) code->src2) & 0xF;

      out[0] = (unsigned char) ((code->as & 0xF) << 4);
      out[1] = (unsigned char) (((code->at & 0xF) << 4) | sh);
      out[2] = (unsigned char) ((msk << 4) | 0x4);
      break;
    }

    case ESP_OP_NSAU:
      /* GAS: nsau a2,a3 = 20 f3 40; nsau a4,a5 = 40 f5 40
       * out[0] = at<<4 (low nibble 0), out[1] = 0xF0|as, out[2] = 0x40 */
      out[0] = (unsigned char) ((code->at & 0xF) << 4);
      out[1] = (unsigned char) (0xF0 | (code->as & 0xF));
      out[2] = 0x40;
      break;

    case ESP_OP_RFI:
      /* RFI n: [ 0x10, 0x30|n, 0x00 ] -- level >= 2 only (EPS/EPCn);
       * level-1 state lives in EPC1 with EXCM, exits via RFE. */
      out[0] = 0x10;
      out[1] = (unsigned char) (0x30 | (code->imm & 7));
      out[2] = 0x00;
      break;

    case ESP_OP_RFE:
      /* RFE: [ 0x00, 0x30, 0x00 ] (gas: 003000) -- level-1 exit:
       * pc = EPC1, PS.EXCM cleared (QEMU translate_rfe rule) */
      out[0] = 0x00;
      out[1] = 0x30;
      out[2] = 0x00;
      break;

    case ESP_OP_JX:
      /* JX as: [ 0xA0, as, 0 ]  (jx a5 = a0 05 00, golden-verified;
       * the register rides in m1 -- m0-low is NOT the register field,
       * that encoding is the windowed call8 family!) */
      out[0] = 0xA0;
      out[1] = (unsigned char) (code->as & 0xF);
      out[2] = 0;
      break;

    case ESP_OP_CALLX0:
      /* CALLX0 as: [ 0xC0, as, 0 ]  (callx0 a5 = c0 05 00) */
      out[0] = 0xC0;
      out[1] = (unsigned char) (code->as & 0xF);
      out[2] = 0;
      break;

    case ESP_OP_ADDI:
      out[0] = (unsigned char) (((code->at & 0xF) << 4) | 0x2);
      out[1] = (unsigned char) (0xC0 | (code->as & 0xF));
      out[2] = (unsigned char) (code->imm & 0xFF);
      break;

    case ESP_OP_ADDMI:
      out[0] = (unsigned char) (((code->at & 0xF) << 4) | 0x2);
      out[1] = (unsigned char) (0xD0 | (code->as & 0xF));
      out[2] = (unsigned char) ((code->imm / 256) & 0xFF);
      break;

    case ESP_OP_L8UI:
      out[0] = (unsigned char) (((code->at & 0xF) << 4) | 0x2);
      out[1] = (unsigned char) (0x00 | (code->as & 0xF));
      out[2] = (unsigned char) (code->imm & 0xFF);
      break;

    case ESP_OP_L16UI:
      out[0] = (unsigned char) (((code->at & 0xF) << 4) | 0x2);
      out[1] = (unsigned char) (0x10 | (code->as & 0xF));
      out[2] = (unsigned char) (code->imm & 0xFF);
      break;

    case ESP_OP_L16SI:
      out[0] = (unsigned char) (((code->at & 0xF) << 4) | 0x2);
      out[1] = (unsigned char) (0x90 | (code->as & 0xF));
      out[2] = (unsigned char) (code->imm & 0xFF);
      break;

    case ESP_OP_L32I:
      out[0] = (unsigned char) (((code->at & 0xF) << 4) | 0x2);
      out[1] = (unsigned char) (0x20 | (code->as & 0xF));
      out[2] = (unsigned char) (code->imm & 0xFF);
      break;

    case ESP_OP_BEQ:
    case ESP_OP_BNE:
    case ESP_OP_BLT:
    case ESP_OP_BGE:
    case ESP_OP_BLTU:
    case ESP_OP_BGEU: {
      /* register branch: out[0] = (t<<4)|7, out[1] = (op<<4)|s,
       * out[2] = signed byte offset from PC+4.
       * The SECOND register (at) is in out[0][7:4] — confirmed by GAS:
       *   beq a2, a3 → 37 12 XX (t=3 in out[0][7:4])
       *   bge a7, a8 → 87 A7 XX (t=8 in out[0][7:4], from vec3)
       * Previously out[0] was hardcoded 0x37 (= t=a3), which only
       * worked for comparisons against a3. */
      static const unsigned char opn[6] = {0x1, 0x9, 0x2, 0xA, 0x3, 0xB};
      long delta;
      int  idx;

      idx = (int) (code->op - ESP_OP_BEQ);
      if (!code->target) {
        pfile_log(pf, PFILE_LOG_ERR, "ESP32 branch without target");
        delta = 0;
      } else {
        delta = (long) code->target->addr - (long) (code->addr + 4UL);
      }
      if ((delta < -128L) || (delta > 127L)) {
        pfile_log(pf, PFILE_LOG_ERR,
            "ESP32 branch target out of BRI8 range (%ld); "
            "relaxation pass pending", delta);
        delta = 0;
      }
      out[0] = (unsigned char) (((code->at & 0xF) << 4) | 0x7);
      out[1] = (unsigned char) ((opn[idx] << 4) | (code->as & 0xF));
      out[2] = (unsigned char) (delta & 0xFF);
      break;
    }

    case ESP_OP_BEQZ:
    case ESP_OP_BNEZ: {
      /* BRI12: m0 = 0x16 (beqz) / 0x56 (bnez), imm12 = byte offset:
       * m1 = (imm[3:0]<<4)|r, m2 = imm[11:4] */
      long delta;

      if (!code->target) {
        pfile_log(pf, PFILE_LOG_ERR, "ESP32 branch without target");
        delta = 0;
      } else {
        delta = (long) code->target->addr - (long) (code->addr + 4UL);
      }
      if ((delta < -2048L) || (delta > 2047L)) {
        pfile_log(pf, PFILE_LOG_ERR,
            "ESP32 branch target out of BRI12 range (%ld); "
            "relaxation pass pending", delta);
        delta = 0;
      }
      out[0] = (ESP_OP_BEQZ == code->op) ? 0x16 : 0x56;
      out[1] = (unsigned char) (((delta & 0xF) << 4) | (code->as & 0xF));
      out[2] = (unsigned char) ((delta >> 4) & 0xFF);
      break;
    }

    case ESP_OP_SLL:
      /* SLL at, as: [ 0, (at<<4)|as, 0xA1 ] (golden: sll a2,a3 = 00 23 a1) */
      out[0] = 0;
      out[1] = (unsigned char) (((code->at & 0xF) << 4) | (code->as & 0xF));
      out[2] = 0xA1;
      break;

    case ESP_OP_SRL:
    case ESP_OP_SRA:
      /* SRL/SRA at, as: [ (as<<4), (at<<4), 0x91/0xB1 ]
       * (golden: srl a2,a3 = 30 20 91 -- `as` rides in m0, unlike SLL) */
      out[0] = (unsigned char) ((code->as & 0xF) << 4);
      out[1] = (unsigned char) ((code->at & 0xF) << 4);
      out[2] = (ESP_OP_SRL == code->op) ? 0x91 : 0xB1;
      break;

    case ESP_OP_ADD:
    case ESP_OP_SUB:
    case ESP_OP_AND:
    case ESP_OP_OR:
    case ESP_OP_XOR:
    case ESP_OP_MULL:
    case ESP_OP_MULUH: {
      /* RRR: m0 = (r<<4), m1 = (t<<4)|s, m2 = opcode byte */
      unsigned char byte2;

      switch (code->op) {
        case ESP_OP_ADD:  byte2 = 0x80; break;
        case ESP_OP_SUB:  byte2 = 0xC0; break;
        case ESP_OP_AND:  byte2 = 0x10; break;
        case ESP_OP_OR:   byte2 = 0x20; break;
        case ESP_OP_XOR:  byte2 = 0x30; break;
        case ESP_OP_MULUH: byte2 = 0xA2; break;
        default:          byte2 = 0x82; break; /* MULL */
      }
      out[0] = (unsigned char) ((code->imm & 0xF) << 4);
      out[1] = (unsigned char) (((code->at & 0xF) << 4) | (code->as & 0xF));
      out[2] = byte2;
      break;
    }

    case ESP_OP_NEG:
      /* NEG at, as: [ (as<<4), at<<4, 0x60 ] */
      out[0] = (unsigned char) ((code->as & 0xF) << 4);
      out[1] = (unsigned char) ((code->at & 0xF) << 4);
      out[2] = 0x60;
      break;

    case ESP_OP_SSL:
      /* SSL as: [ 0, (1<<4)|as, 0x40 ] */
      out[0] = 0;
      out[1] = (unsigned char) (0x10 | (code->as & 0xF));
      out[2] = 0x40;
      break;

    case ESP_OP_SSR:
      /* SSR as: [ 0, (0<<4)|as, 0x40 ] */
      out[0] = 0;
      out[1] = (unsigned char) (code->as & 0xF);
      out[2] = 0x40;
      break;

    default:
      pfile_log(pf, PFILE_LOG_ERR,
          "ESP32 encoder: unhandled opcode %u", (unsigned) code->op);
      out[0] = out[1] = out[2] = 0;
      break;
  }
}
