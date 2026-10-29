/**********************************************************
 **
 ** cmd_lbl.c : the cmd_type label functions
 **
 ** Copyright (c) 2008, Kyle A. York
 ** All rights reserved
 **
 ** Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
 ** Changes: minor fork edits
 **
 ***********************************************************/
#include "cmdd.h"

static void cmd_label_free(cmd_t cmd)
{
  cmd_label_set(cmd, LABEL_NONE);
}

static void cmd_label_dump(cmd_t cmd, FILE *dst)
{
  fprintf(dst, "%s:", label_name_get(cmd_label_get(cmd)));
}

static cmd_t cmd_label_dup(const cmd_t cmd)
{
  /* we cannot simply pass the label to this, 'cause the label
   * points to a command & we don't want to change that. fortunately,
   * this should only be used when the labels are going to be remapped
   */ 
  UNUSED(cmd);
  return cmd_label_alloc(LABEL_NONE);
}

static void cmd_label_label_remap(cmd_t cmd, const label_map_t *map)
{
  label_t lbl;

  lbl = label_map_find(map, cmd_label_get(cmd));
  if (lbl) {
    cmd_label_set(cmd, lbl);
  }
}

static const cmd_vtbl_t cmd_label_vtbl = {
  cmd_label_free,
  cmd_label_dump,
  cmd_label_dup,
  cmd_label_label_remap,
  0, /* ignore variable map          */
  0, /* ignore value map             */
  0, /* ignore cmd variable accessed */
  0, /* ignore cmd value accessed    */
  0, /* ignore assign/used set       */
  0  /* use default successor set    */
};

cmd_t cmd_label_alloc(label_t lbl)
{
  cmd_t cmd;

  cmd = cmd_alloc(CMD_TYPE_LABEL, &cmd_label_vtbl);
  if (cmd) {
    struct cmd_ *ptr;

    ptr = cmd_element_seek(cmd, BOOLEAN_TRUE);
    if (ptr) {
      ptr->u.label = LABEL_NONE;
      cmd_label_set(cmd, lbl);
    }
  }
  return cmd;
}

label_t cmd_label_get(const cmd_t cmd)
{
  struct cmd_ *ptr;
  label_t lbl;

  lbl = LABEL_NONE;
  ptr = cmd_element_seek(cmd, BOOLEAN_FALSE);
  if (ptr) {
    if (CMD_TYPE_LABEL == cmd_type_get(cmd)) {
      lbl = ptr->u.label;
    } else if (CMD_TYPE_PROC_ENTER == cmd_type_get(cmd)) {
      lbl = pfile_proc_label_get(ptr->u.proc);
    }
  }
  return lbl;
}

void cmd_label_set(cmd_t cmd, label_t lbl)
{
  if (CMD_TYPE_LABEL == cmd_type_get(cmd)) {
    struct cmd_ *ptr;

    ptr = cmd_element_seek(cmd, BOOLEAN_TRUE);
    if (ptr) {
      label_lock(lbl);
      if (ptr->u.label) {
        label_t old;

        old = ptr->u.label;
        if (old != lbl) {
          /* If refct>0 the label stays alive; clear its cmd back-pointer before
           * this command drops the label. */
          label_cmd_set(old, CMD_NONE);
        }
        label_release(old);
      }
      ptr->u.label = lbl;
      label_cmd_set(lbl, cmd);
    }
  }
}

/*
 * NAME
 *   cmd_label_find
 *
 * DESCRIPTION
 *   find a label in the command list
 *
 * PARAMETERS
 *   cmd : head of list
 *
 * RETURN
 *   label, or 0
 *
 * NOTES
 */
cmd_t cmd_label_find(cmd_t cmd_head, const label_t lbl)
{
  cmd_t cmd;
  cmd_t scan;

  if (!lbl) {
    return 0;
  }
  /* Fast path: label records its defining command. */
  cmd = label_cmd_get(lbl);
  if (cmd && cmd_label_get(cmd) == lbl) {
    return cmd;
  }
  /* Stale label->cmd (e.g. label moved to another command but still referenced):
   * scan the list. */
  for (scan = cmd_head; scan; scan = cmd_link_get(scan)) {
    if (cmd_label_get(scan) == lbl) {
      return scan;
    }
  }
  return 0;
}

