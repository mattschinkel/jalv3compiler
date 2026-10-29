/************************************************************
 **
 ** pic_asmcmd.c : classic-PIC semantics for inline-asm commands
 **
 ** The asm command lives in the shared IR (libcore/cmd_asm.c) with an
 ** OPAQUE opcode; this file owns the vocabulary's meaning -- dump
 ** rendering, use/def classification, control-flow successors, and the
 ** two properties the shared optimizer asks about (flow terminator,
 ** conditional skip). Registered once at startup via
 ** cmd_asm_semantics_set (jal_main), mirroring the back-end registry.
 **
 ** Copyright (c) 2005, Kyle A. York
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: split out of libcore/cmd_asm.c so the shared IR layer
 **   carries no back-end opcode vocabulary
 **
 ************************************************************/
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "../libutils/mem.h"
#include "../libcore/cmd.h"
#include "../libcore/cmdd.h"
#include "../libcore/cmd_asm.h"
#include "pic_inst.h"

static void pic_asm_dump_value(value_t val, const char *str, FILE *dst)
{
  if (val) {
    if (value_name_get(val)) {
      fprintf(dst, "%s%s", str, value_name_get(val));
    } else {
      fprintf(dst, "%s%u", str, (unsigned) value_const_get(val));
    }
  }
}

static void pic_asm_dump(cmd_t cmd, FILE *dst)
{
  pic_optype_t type;

  fprintf(dst, "%s", pic_opcode_str((pic_opcode_t) cmd_asm_op_get(cmd)));
  type = pic_optype_get((pic_opcode_t) cmd_asm_op_get(cmd));
  switch (type) {
    case PIC_OPTYPE_NONE:
      break;
    case PIC_OPTYPE_F:
    case PIC_OPTYPE_F_D:
    case PIC_OPTYPE_F_B:
      pic_asm_dump_value(cmd_asm_val_get(cmd), " ", dst);
      if (PIC_OPTYPE_F_D == type) {
        fprintf(dst, ", %s", pic_opdst_str((pic_opdst_t) cmd_asm_opdst_get(cmd)));
      } else if (PIC_OPTYPE_F_B == type) {
        pic_asm_dump_value(cmd_asm_n_get(cmd), ", ", dst);
      }
      break;
    case PIC_OPTYPE_N:
    case PIC_OPTYPE_TRIS:
      pic_asm_dump_value(cmd_asm_n_get(cmd), " ", dst);
      break;
    case PIC_OPTYPE_K:
      if (cmd_asm_lbl_get(cmd)) {
        fprintf(dst, " %s", label_name_get(cmd_asm_lbl_get(cmd)));
      } else {
        pic_asm_dump_value(cmd_asm_n_get(cmd), " ", dst);
      }
      break;
    case PIC_OPTYPE_DB:
      {
        uchar *data;
        size_t data_sz;
        size_t ii;

        data_sz = cmd_asm_data_sz_get(cmd);
        data    = cmd_asm_data_get(cmd);
        for (ii = 0; ii < data_sz; ii++) {
          fprintf(dst, "%u%s", data[ii], (ii + 1 < data_sz) ? "," : "");
        }
      }
      break;
  }
}

static void pic_asm_assigned_use_set(cmd_t cmd)
{
  variable_t var;

  var = value_variable_get(cmd_asm_n_get(cmd));
  if ((VARIABLE_NONE != var) && !variable_is_const(var)) {
    switch (cmd_asm_op_get(cmd)) {
      case PIC_OPCODE_ORG:
      case PIC_OPCODE_END:
      case PIC_OPCODE_NONE:
      case PIC_OPCODE_TRIS:
      case PIC_OPCODE_BC:
      case PIC_OPCODE_BN:
      case PIC_OPCODE_BNC:
      case PIC_OPCODE_BNN:
      case PIC_OPCODE_BNOV:
      case PIC_OPCODE_BNZ:
      case PIC_OPCODE_BOV:
      case PIC_OPCODE_BZ:
      case PIC_OPCODE_BRA:
      case PIC_OPCODE_RCALL:
      case PIC_OPCODE_CALL:
      case PIC_OPCODE_GOTO:
      case PIC_OPCODE_CLRW:
      case PIC_OPCODE_CLRWDT:
      case PIC_OPCODE_DAW:
      case PIC_OPCODE_NOP:
      case PIC_OPCODE_OPTION:
      case PIC_OPCODE_POP:
      case PIC_OPCODE_PUSH:
      case PIC_OPCODE_RESET:
      case PIC_OPCODE_RETURN:
      case PIC_OPCODE_RETFIE:
      case PIC_OPCODE_SLEEP:
      case PIC_OPCODE_TBLRD:
      case PIC_OPCODE_TBLWT:
      case PIC_OPCODE_DB:
      case PIC_OPCODE_ADDLW:
      case PIC_OPCODE_ANDLW:
      case PIC_OPCODE_IORLW:
      case PIC_OPCODE_MOVLW:
      case PIC_OPCODE_MULLW:
      case PIC_OPCODE_RETLW:
      case PIC_OPCODE_SUBLW:
      case PIC_OPCODE_XORLW:
      case PIC_OPCODE_BRANCHLO_SET:
      case PIC_OPCODE_BRANCHLO_CLR:
      case PIC_OPCODE_BRANCHLO_NOP:
      case PIC_OPCODE_BRANCHHI_SET:
      case PIC_OPCODE_BRANCHHI_CLR:
      case PIC_OPCODE_BRANCHHI_NOP:
      case PIC_OPCODE_MOVLP_NOP:
        break; /* should never get here */
      case PIC_OPCODE_ADDWF:
      case PIC_OPCODE_ADDWFc:
      case PIC_OPCODE_ANDWF:
      case PIC_OPCODE_COMF:
      case PIC_OPCODE_DECF:
      case PIC_OPCODE_DCFSNZ:
      case PIC_OPCODE_DECFSZ:
      case PIC_OPCODE_INCF:
      case PIC_OPCODE_INCFSZ:
      case PIC_OPCODE_INFSNZ:
      case PIC_OPCODE_IORWF:
      case PIC_OPCODE_MOVF:
      case PIC_OPCODE_RLF:
      case PIC_OPCODE_RLCF:
      case PIC_OPCODE_RLNCF:
      case PIC_OPCODE_RRF:
      case PIC_OPCODE_RRCF:
      case PIC_OPCODE_RRNCF:
      case PIC_OPCODE_SUBFWB:
      case PIC_OPCODE_SUBWF:
      case PIC_OPCODE_SUBWFB:
      case PIC_OPCODE_SWAPF:
      case PIC_OPCODE_XORWF:
        cmd_gen_add(cmd, var);
        if (PIC_OPDST_F == cmd_asm_opdst_get(cmd)) {
          cmd_kill_add(cmd, var);
        }
        break;
      case PIC_OPCODE_CPFSEQ:
      case PIC_OPCODE_CPFSGT:
      case PIC_OPCODE_CPFSLT:
      case PIC_OPCODE_TSTFSZ:
      case PIC_OPCODE_BTFSC:
      case PIC_OPCODE_BTFSS:
      case PIC_OPCODE_LFSR:
      case PIC_OPCODE_MOVLB:
      case PIC_OPCODE_MOVLP:
      case PIC_OPCODE_DATALO_SET:
      case PIC_OPCODE_DATALO_CLR:
      case PIC_OPCODE_DATAHI_SET:
      case PIC_OPCODE_DATAHI_CLR:
      case PIC_OPCODE_IRP_SET:
      case PIC_OPCODE_IRP_CLR:
        cmd_gen_add(cmd, var);
        break;
      case PIC_OPCODE_NEGF:
      case PIC_OPCODE_BTG:
        cmd_gen_add(cmd, var);
        cmd_kill_add(cmd, var);
        break;
      case PIC_OPCODE_SETF:
      case PIC_OPCODE_BCF:
      case PIC_OPCODE_BSF:
      case PIC_OPCODE_MOVWF:
      case PIC_OPCODE_MULWF:
      case PIC_OPCODE_CLRF:
        cmd_kill_add(cmd, var);
        break;
      case PIC_OPCODE_MOVFF:
        cmd_gen_add(cmd, var);
        /* cmd_kill_add(cmd, var2); */
        break;
    }
  }
}

static cmd_successor_rc_t pic_asm_successor_get(cmd_t cmd, size_t ix, 
  cmd_t *dst)
{
  cmd_successor_rc_t rc;

  rc = CMD_SUCCESSOR_RC_IX_BAD;
  switch (cmd_asm_op_get(cmd)) {
    case PIC_OPCODE_ORG:
    case PIC_OPCODE_END:
    case PIC_OPCODE_NONE:
    case PIC_OPCODE_TRIS:
    case PIC_OPCODE_CLRW:
    case PIC_OPCODE_CLRWDT:
    case PIC_OPCODE_DAW:
    case PIC_OPCODE_NOP:
    case PIC_OPCODE_OPTION:
    case PIC_OPCODE_POP:
    case PIC_OPCODE_PUSH:
    case PIC_OPCODE_RESET:
    case PIC_OPCODE_RETURN:
    case PIC_OPCODE_RETFIE:
    case PIC_OPCODE_SLEEP:
    case PIC_OPCODE_TBLRD:
    case PIC_OPCODE_TBLWT:
    case PIC_OPCODE_DB:
    case PIC_OPCODE_ADDLW:
    case PIC_OPCODE_ANDLW:
    case PIC_OPCODE_IORLW:
    case PIC_OPCODE_MOVLW:
    case PIC_OPCODE_MULLW:
    case PIC_OPCODE_RETLW:
    case PIC_OPCODE_SUBLW:
    case PIC_OPCODE_XORLW:
    case PIC_OPCODE_BRANCHLO_SET:
    case PIC_OPCODE_BRANCHLO_CLR:
    case PIC_OPCODE_BRANCHLO_NOP:
    case PIC_OPCODE_BRANCHHI_SET:
    case PIC_OPCODE_BRANCHHI_CLR:
    case PIC_OPCODE_BRANCHHI_NOP:
    case PIC_OPCODE_MOVLP_NOP:
    case PIC_OPCODE_ADDWF:
    case PIC_OPCODE_ADDWFc:
    case PIC_OPCODE_ANDWF:
    case PIC_OPCODE_COMF:
    case PIC_OPCODE_DECF:
    case PIC_OPCODE_INCF:
    case PIC_OPCODE_IORWF:
    case PIC_OPCODE_MOVF:
    case PIC_OPCODE_RLF:
    case PIC_OPCODE_RLCF:
    case PIC_OPCODE_RLNCF:
    case PIC_OPCODE_RRF:
    case PIC_OPCODE_RRCF:
    case PIC_OPCODE_RRNCF:
    case PIC_OPCODE_SUBFWB:
    case PIC_OPCODE_SUBWF:
    case PIC_OPCODE_SUBWFB:
    case PIC_OPCODE_SWAPF:
    case PIC_OPCODE_XORWF:
    case PIC_OPCODE_MULWF:
    case PIC_OPCODE_LFSR:
    case PIC_OPCODE_MOVLB:
    case PIC_OPCODE_MOVLP:
    case PIC_OPCODE_DATALO_SET:
    case PIC_OPCODE_DATALO_CLR:
    case PIC_OPCODE_DATAHI_SET:
    case PIC_OPCODE_DATAHI_CLR:
    case PIC_OPCODE_IRP_SET:
    case PIC_OPCODE_IRP_CLR:
    case PIC_OPCODE_NEGF:
    case PIC_OPCODE_BTG:
    case PIC_OPCODE_SETF:
    case PIC_OPCODE_BCF:
    case PIC_OPCODE_BSF:
    case PIC_OPCODE_MOVWF:
    case PIC_OPCODE_CLRF:
    case PIC_OPCODE_MOVFF:
      if (0 == ix) {
        *dst = cmd_link_get(cmd);
        rc = CMD_SUCCESSOR_RC_DONE;
      } 
      break;
    case PIC_OPCODE_DCFSNZ:
    case PIC_OPCODE_DECFSZ:
    case PIC_OPCODE_INCFSZ:
    case PIC_OPCODE_INFSNZ:
    case PIC_OPCODE_CPFSEQ:
    case PIC_OPCODE_CPFSGT:
    case PIC_OPCODE_CPFSLT:
    case PIC_OPCODE_TSTFSZ:
    case PIC_OPCODE_BTFSC:
    case PIC_OPCODE_BTFSS:
      /* successor is both the next command or the command that follows */
      if (0 == ix) {
        *dst = cmd_link_get(cmd);
        rc = CMD_SUCCESSOR_RC_MORE;
      } else if (1 == ix) {
        *dst = cmd_link_get(cmd_link_get(cmd));
        rc = CMD_SUCCESSOR_RC_DONE;
      }
      break;
    case PIC_OPCODE_BC:
    case PIC_OPCODE_BN:
    case PIC_OPCODE_BNC:
    case PIC_OPCODE_BNN:
    case PIC_OPCODE_BNOV:
    case PIC_OPCODE_BNZ:
    case PIC_OPCODE_BOV:
    case PIC_OPCODE_BZ:
    case PIC_OPCODE_BRA:
    case PIC_OPCODE_RCALL:
    case PIC_OPCODE_CALL:
      /* successor is both the next command or the label */
      if (0 == ix) {
        *dst = cmd_link_get(cmd);
        rc = CMD_SUCCESSOR_RC_MORE;
      } else if (1 == ix) {
        *dst = label_cmd_get(cmd_asm_lbl_get(cmd));
        rc = CMD_SUCCESSOR_RC_DONE;
      }
      break;
    case PIC_OPCODE_GOTO:
      /* successor is only the label */
      if (0 == ix) {
        *dst = label_cmd_get(cmd_asm_lbl_get(cmd));
        rc = CMD_SUCCESSOR_RC_DONE;
      }
      break;
  }
  return rc;
}

/* the shared optimizer's two opcode questions (cmd_analyze):
 * RESET terminates flow; the skip instructions are conditional */
static boolean_t pic_asm_flow_terminator_p(cmd_asm_op_t op)
{
  return (PIC_OPCODE_RESET == (pic_opcode_t) op)
      ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

static boolean_t pic_asm_cond_skip_p(cmd_asm_op_t op)
{
  switch ((pic_opcode_t) op) {
    case PIC_OPCODE_INCFSZ:
    case PIC_OPCODE_DECFSZ:
    case PIC_OPCODE_BTFSC:
    case PIC_OPCODE_BTFSS:
      return BOOLEAN_TRUE;
    default:
      return BOOLEAN_FALSE;
  }
}

const cmd_asm_semantics_t pic_asm_semantics = {
  pic_asm_dump,
  pic_asm_assigned_use_set,
  pic_asm_successor_get,
  pic_asm_flow_terminator_p,
  pic_asm_cond_skip_p,
  PIC_OPCODE_NONE,
  PIC_OPDST_NONE
};
