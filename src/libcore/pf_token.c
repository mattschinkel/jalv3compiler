/**********************************************************
 **
 ** pf_token.c : token parsing bits
 **
 ** Copyright (c) 2004-2005, Kyle A. York
 ** All rights reserved
 **
 ***********************************************************/
#include <string.h>
#include "pfile.h"
#include "pfiled.h"
#include "pf_msg.h"
#include "pf_token.h"

/*
 * NAME
 *   pf_token_parse_allow
 *
 * DESCRIPTION
 *   parse the token while the character is allowed
 *
 * PARAMETERS
 *   pf    : returned by pf_open()
 *   allow : allow function
 *   flags : determine what to do with the characters
 *
 * RETURN
 *   first character not allowed
 *
 * NOTES
 */
int pf_token_parse_allow(
  pfile_t                  *pf, 
  pf_token_parse_allow_cb_t allow, 
  flag_t                    flags)
{
  int ch;

  for ( ; ; ) {
    ch = pfile_ch_get(pf);
    if ((EOF == ch) || !allow(ch)) {
      break; /* done !*/
    } else if (flags & PF_TOKEN_PARSE_ALLOW_STORE) {
      pf_token_ch_append(pf, TOLOWER(ch));
    }
  }
  if (flags & PF_TOKEN_PARSE_ALLOW_UNGET) {
    pfile_ch_unget(pf, ch);
  }
  return ch;
}

/*
 * NAME
 *   pf_token_get
 *
 * DESCRIPTION
 *   retrieve a token
 *
 * PARAMETERS
 *   pf       : returned by pfile_open()
 *   which    : which token to get (first, current, next)
 *
 * RETURN
 *   the token text
 */
const char *pf_token_get(pfile_t *pf, pf_token_get_t which)
{
  switch (which) {
    case PF_TOKEN_FIRST:
      pf->token_ct = 1;
      break;
    case PF_TOKEN_NEXT:
      pf->token_ct++;
      break;
    case PF_TOKEN_CURRENT:
      break;
  }
  return (pf->vectors->pf_token_get_fn(pf, which));
}


boolean_t pf_token_is(pfile_t *pf, pf_token_get_t which, 
    pfile_log_t plog, const char *str)
{
  boolean_t   rc;
  const char *ptr;

  ptr = pf_token_get(pf, which);
  rc = (str) ? (0 == strcmp(ptr, str)) : 0;
  if (!rc) {
    pfile_log(pf, plog, PFILE_MSG_EXPECTED, str, ptr);
  }
  return rc;
}

boolean_t pf_token_is_eof(pfile_t *pf)
{
  const char *ptr;

  ptr = pf_token_get(pf, PF_TOKEN_CURRENT);
  return (0 == *ptr);
}

const char *pf_token_ptr_get(pfile_t *pf)
{
  return pf->token;
}

void pf_token_set(pfile_t *pf, const char *tokn)
{
  size_t sz;

  sz = strlen(tokn);
  if (sz > PFILE_TOKEN_SZ_MAX) {
    sz = PFILE_TOKEN_SZ_MAX;
  }
  memcpy(pf->token, tokn, sz);
  pf->token[sz] = 0;
  pf->token_len = sz;
}

size_t pf_token_sz_get(pfile_t *pf)
{
  return pf->token_len;
}

void pf_token_ch_append(pfile_t *pf, int ch)
{
  if (EOF == ch) {
    if (!pf->token_len) {
      pf->token[0] = 0;
    }
  } else {
    if (pf->token_len < PFILE_TOKEN_SZ_MAX) {
      pf->token[pf->token_len++] = ch;
      pf->token[pf->token_len] = 0;
    }
  }
}

void pf_token_reset(pfile_t *pf)
{
  pfile_pos_get(pf, &pf->token_start);
  pf->token_len = 0;
  pf->token[0] = 0;
}

void pf_token_start_get(pfile_t *pf, pfile_pos_t *pos)
{
  if (pf && pos) {
    *pos = pf->token_start;
  }
}

