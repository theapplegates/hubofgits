/* call-agent.c - Divert GPG operations to the agent.
 * Copyright (C) 2001-2003, 2006-2011, 2013 Free Software Foundation, Inc.
 * Copyright (C) 2013-2015  Werner Koch
 * Copyright (C) 2020       g10 Code GmbH
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#ifdef HAVE_LOCALE_H
#include <locale.h>
#endif

#include "gpg.h"
#include <assuan.h>
#include "../common/util.h"
#include "../common/membuf.h"
#include "options.h"
#include "../common/i18n.h"
#include "../common/asshelp.h"
#include "../common/sysutils.h"
#include "call-agent.h"
#include "../common/status.h"
#include "../common/shareddefs.h"
#include "../common/host2net.h"
#include "../common/ttyio.h"

#define CONTROL_D ('D' - 'A' + 1)


static assuan_context_t agent_ctx = NULL;
static int did_early_card_test;

struct confirm_parm_s
{
  char *desc;
  char *ok;
  char *notok;
};

struct default_inq_parm_s
{
  ctrl_t ctrl;
  assuan_context_t ctx;
  struct {
    u32 *keyid;
    u32 *mainkeyid;
    int pubkey_algo;
  } keyinfo;
  struct confirm_parm_s *confirm;
};

struct cipher_parm_s
{
  struct default_inq_parm_s *dflt;
  assuan_context_t ctx;
  unsigned char *ciphertext;
  size_t ciphertextlen;
};

struct writecert_parm_s
{
  struct default_inq_parm_s *dflt;
  const unsigned char *certdata;
  size_t certdatalen;
};

struct writekey_parm_s
{
  struct default_inq_parm_s *dflt;
  const unsigned char *keydata;
  size_t keydatalen;
};

struct genkey_parm_s
{
  struct default_inq_parm_s *dflt;
  const char *keyparms;
  const char *passphrase;
};

struct import_key_parm_s
{
  struct default_inq_parm_s *dflt;
  const void *key;
  size_t keylen;
};


struct cache_nonce_parm_s
{
  char **cache_nonce_addr;
  char **passwd_nonce_addr;
};


static gpg_error_t learn_status_cb (void *opaque, const char *line);



/* If RC is not 0, write an appropriate status message. */
static void
status_sc_op_failure (int rc)
{
  switch (gpg_err_code (rc))
    {
    case 0:
      break;
    case GPG_ERR_CANCELED:
    case GPG_ERR_FULLY_CANCELED:
      write_status_text (STATUS_SC_OP_FAILURE, "1");
      break;
    case GPG_ERR_BAD_PIN:
    case GPG_ERR_BAD_RESET_CODE:
      write_status_text (STATUS_SC_OP_FAILURE, "2");
      break;
    default:
      write_status (STATUS_SC_OP_FAILURE);
      break;
    }
}


/* This is the default inquiry callback.  It mainly handles the
   Pinentry notifications.  */
static gpg_error_t
default_inq_cb (void *opaque, const char *line)
{
  gpg_error_t err = 0;
  struct default_inq_parm_s *parm = opaque;
  const char *s;

  if (has_leading_keyword (line, "PINENTRY_LAUNCHED"))
    {
      err = gpg_proxy_pinentry_notify (parm->ctrl, line);
      if (err)
        log_error (_("failed to proxy %s inquiry to client\n"),
                   "PINENTRY_LAUNCHED");
      /* We do not pass errors to avoid breaking other code.  */
    }
  else if ((has_leading_keyword (line, "PASSPHRASE")
            || has_leading_keyword (line, "NEW_PASSPHRASE"))
           && opt.pinentry_mode == PINENTRY_MODE_LOOPBACK)
    {
      assuan_begin_confidential (parm->ctx);
      if (have_static_passphrase ())
        {
          s = get_static_passphrase ();
          err = assuan_send_data (parm->ctx, s, strlen (s));
        }
      else
        {
          char *pw;
          char buf[32];

          if (parm->keyinfo.keyid)
            emit_status_need_passphrase (parm->ctrl,
                                         parm->keyinfo.keyid,
                                         parm->keyinfo.mainkeyid,
                                         parm->keyinfo.pubkey_algo);

          snprintf (buf, sizeof (buf), "%u", 100);
          write_status_text (STATUS_INQUIRE_MAXLEN, buf);
          pw = cpr_get_hidden ("passphrase.enter", _("Enter passphrase: "));
          cpr_kill_prompt ();
          if (*pw == CONTROL_D && !pw[1])
            err = gpg_error (GPG_ERR_CANCELED);
          else
            err = assuan_send_data (parm->ctx, pw, strlen (pw));
          xfree (pw);
        }
      assuan_end_confidential (parm->ctx);
    }
  else if ((s = has_leading_keyword (line, "CONFIRM"))
           && opt.pinentry_mode == PINENTRY_MODE_LOOPBACK
           && parm->confirm)
    {
      int ask = atoi (s);
      int yes;

      if (ask)
        {
          yes = cpr_get_answer_is_yes (NULL, parm->confirm->desc);
          if (yes)
            err = assuan_send_data (parm->ctx, NULL, 0);
          else
            err = gpg_error (GPG_ERR_NOT_CONFIRMED);
        }
      else
        {
          tty_printf ("%s", parm->confirm->desc);
          err = assuan_send_data (parm->ctx, NULL, 0);
        }
    }
  else
    log_debug ("ignoring gpg-agent inquiry '%s'\n", line);

  return err;
}


/* Print a warning if the server's version number is less than our
   version number.  Returns an error code on a connection problem.  */
static gpg_error_t
warn_version_mismatch (assuan_context_t ctx, const char *servername, int mode)
{
  return warn_server_version_mismatch (ctx, servername, mode,
                                       write_status_strings2, NULL,
                                       !opt.quiet);
}


#define FLAG_FOR_CARD_SUPPRESS_ERRORS 2

/* Try to connect to the agent via socket or fork it off and work by
   pipes.  Handle the server's initial greeting */
static int
start_agent (ctrl_t ctrl, int flag_for_card)
{
  int rc;

  (void)ctrl;  /* Not yet used.  */

  /* Fixme: We need a context for each thread or serialize the access
     to the agent. */
  if (agent_ctx)
    rc = 0;
  else
    {
      rc = start_new_gpg_agent (&agent_ctx,
                                GPG_ERR_SOURCE_DEFAULT,
                                opt.agent_program,
                                opt.lc_ctype, opt.lc_messages,
                                opt.session_env,
                                opt.autostart?ASSHELP_FLAG_AUTOSTART:0,
                                opt.verbose, DBG_IPC,
                                NULL, NULL);
      if (!opt.autostart && gpg_err_code (rc) == GPG_ERR_NO_AGENT)
        {
          static int shown;

          if (!shown)
            {
              shown = 1;
              log_info (_("no gpg-agent running in this session\n"));
            }
        }
      else if (!rc
               && !(rc = warn_version_mismatch (agent_ctx, GPG_AGENT_NAME, 0)))
        {
          /* Tell the agent that we support Pinentry notifications.
             No error checking so that it will work also with older
             agents.  */
          assuan_transact (agent_ctx, "OPTION allow-pinentry-notify",
                           NULL, NULL, NULL, NULL, NULL, NULL);
          /* Tell the agent about what version we are aware.  This is
             here used to indirectly enable GPG_ERR_FULLY_CANCELED.  */
          assuan_transact (agent_ctx, "OPTION agent-awareness=2.1.0",
                           NULL, NULL, NULL, NULL, NULL, NULL);
          /* Pass on the pinentry mode.  */
          if (opt.pinentry_mode)
            {
              char *tmp = xasprintf ("OPTION pinentry-mode=%s",
                                     str_pinentry_mode (opt.pinentry_mode));
              rc = assuan_transact (agent_ctx, tmp,
                               NULL, NULL, NULL, NULL, NULL, NULL);
              xfree (tmp);
              if (rc)
                {
                  log_error ("setting pinentry mode '%s' failed: %s\n",
                             str_pinentry_mode (opt.pinentry_mode),
                             gpg_strerror (rc));
                  write_status_error ("set_pinentry_mode", rc);
                }
            }

          /* Pass on the request origin.  */
          if (opt.request_origin)
            {
              char *tmp = xasprintf ("OPTION pretend-request-origin=%s",
                                     str_request_origin (opt.request_origin));
              rc = assuan_transact (agent_ctx, tmp,
                               NULL, NULL, NULL, NULL, NULL, NULL);
              xfree (tmp);
              if (rc)
                {
                  log_error ("setting request origin '%s' failed: %s\n",
                             str_request_origin (opt.request_origin),
                             gpg_strerror (rc));
                  write_status_error ("set_request_origin", rc);
                }
            }

          /* In DE_VS mode under Windows we require that the JENT RNG
           * is active.  */
#ifdef HAVE_W32_SYSTEM
          if (!rc && opt.compliance == CO_DE_VS)
            {
              if (assuan_transact (agent_ctx, "GETINFO jent_active",
                                   NULL, NULL, NULL, NULL, NULL, NULL))
                {
                  rc = gpg_error (GPG_ERR_FORBIDDEN);
                  log_error (_("%s is not compliant with %s mode\n"),
                             GPG_AGENT_NAME,
                             gnupg_compliance_option_string (opt.compliance));
                  write_status_error ("random-compliance", rc);
                }
            }
#endif /*HAVE_W32_SYSTEM*/

        }
    }

  if (!rc && flag_for_card && !did_early_card_test)
    {
      /* Request the serial number of the card for an early test.  */
      struct agent_card_info_s info;

      memset (&info, 0, sizeof info);

      if (!(flag_for_card & FLAG_FOR_CARD_SUPPRESS_ERRORS))
        rc = warn_version_mismatch (agent_ctx, SCDAEMON_NAME, 2);
      if (!rc)
        rc = assuan_transact (agent_ctx,
                              opt.flags.use_only_openpgp_card?
                              "SCD SERIALNO openpgp" : "SCD SERIALNO",
                              NULL, NULL, NULL, NULL,
                              learn_status_cb, &info);
      if (rc && !(flag_for_card & FLAG_FOR_CARD_SUPPRESS_ERRORS))
        {
          switch (gpg_err_code (rc))
            {
            case GPG_ERR_NOT_SUPPORTED:
            case GPG_ERR_NO_SCDAEMON:
              write_status_text (STATUS_CARDCTRL, "6");
              break;
            case GPG_ERR_OBJ_TERM_STATE:
              write_status_text (STATUS_CARDCTRL, "7");
              break;
            default:
              write_status_text (STATUS_CARDCTRL, "4");
              log_info ("selecting card failed: %s\n", gpg_strerror (rc));
              break;
            }
        }

      if (!rc && is_status_enabled () && info.serialno)
        {
          char *buf;

          buf = xasprintf ("3 %s", info.serialno);
          write_status_text (STATUS_CARDCTRL, buf);
          xfree (buf);
        }

      agent_release_card_info (&info);

      if (!rc)
        did_early_card_test = 1;
    }


  return rc;
}


/* Return a new malloced string by unescaping the string S.  Escaping
   is percent escaping and '+'/space mapping.  A binary nul will
   silently be replaced by a 0xFF.  Function returns NULL to indicate
   an out of memory status. */
static char *
unescape_status_string (const unsigned char *s)
{
  return percent_plus_unescape (s, 0xff);
}


/* Take a 20 or 32 byte hexencoded string and put it into the provided
 * FPRLEN byte long buffer FPR in binary format.  Returns the actual
 * used length of the FPR buffer or 0 on error.  */
static unsigned int
unhexify_fpr (const char *hexstr, unsigned char *fpr, unsigned int fprlen)
{
  const char *s;
  int n;

  for (s=hexstr, n=0; hexdigitp (s); s++, n++)
    ;
  if ((*s && *s != ' ') || !(n == 40 || n == 64))
    return 0; /* no fingerprint (invalid or wrong length). */
  for (s=hexstr, n=0; *s && n < fprlen; s += 2, n++)
    fpr[n] = xtoi_2 (s);

  return (n == 20 || n == 32)? n : 0;
}

/* Take the serial number from LINE and return it verbatim in a newly
   allocated string.  We make sure that only hex characters are
   returned. */
static char *
store_serialno (const char *line)
{
  const char *s;
  char *p;

  for (s=line; hexdigitp (s); s++)
    ;
  p = xtrymalloc (s + 1 - line);
  if (p)
    {
      memcpy (p, line, s-line);
      p[s-line] = 0;
    }
  return p;
}



/* This is a dummy data line callback.  */
static gpg_error_t
dummy_data_cb (void *opaque, const void *buffer, size_t length)
{
  (void)opaque;
  (void)buffer;
  (void)length;
  return 0;
}

/* A simple callback used to return the serialnumber of a card.  */
static gpg_error_t
get_serialno_cb (void *opaque, const char *line)
{
  char **serialno = opaque;
  const char *keyword = line;
  const char *s;
  int keywordlen, n;

  for (keywordlen=0; *line && !spacep (line); line++, keywordlen++)
    ;
  while (spacep (line))
    line++;

  if (keywordlen == 8 && !memcmp (keyword, "SERIALNO", keywordlen))
    {
      if (*serialno)
        return gpg_error (GPG_ERR_CONFLICT); /* Unexpected status line. */
      for (n=0,s=line; hexdigitp (s); s++, n++)
        ;
      if (!n || (n&1)|| !(spacep (s) || !*s) )
        return gpg_error (GPG_ERR_ASS_PARAMETER);
      *serialno = xtrymalloc (n+1);
      if (!*serialno)
        return out_of_core ();
      memcpy (*serialno, line, n);
      (*serialno)[n] = 0;
    }

  return 0;
}



/* Release the card info structure INFO. */
void
agent_release_card_info (struct agent_card_info_s *info)
{
  int i;

  if (!info)
    return;

  xfree (info->reader); info->reader = NULL;
  xfree (info->manufacturer_name); info->manufacturer_name = NULL;
  xfree (info->serialno); info->serialno = NULL;
  xfree (info->apptype); info->apptype = NULL;
  xfree (info->disp_name); info->disp_name = NULL;
  xfree (info->disp_lang); info->disp_lang = NULL;
  xfree (info->pubkey_url); info->pubkey_url = NULL;
  xfree (info->login_data); info->login_data = NULL;
  info->cafpr1len = info->cafpr2len = info->cafpr3len = 0;
  info->fpr1len = info->fpr2len = info->fpr3len = 0;
  for (i=0; i < DIM(info->private_do); i++)
    {
      xfree (info->private_do[i]);
      info->private_do[i] = NULL;
    }
  for (i=0; i < DIM(info->supported_keyalgo); i++)
    {
      free_strlist (info->supported_keyalgo[i]);
      info->supported_keyalgo[i] = NULL;
    }
}


static gpg_error_t
learn_status_cb (void *opaque, const char *line)
{
  struct agent_card_info_s *parm = opaque;
  const char *keyword = line;
  int keywordlen;
  int i;
  char *endp;

  for (keywordlen=0; *line && !spacep (line); line++, keywordlen++)
    ;
  while (spacep (line))
    line++;

  if (keywordlen == 6 && !memcmp (keyword, "READER", keywordlen))
    {
      xfree (parm->reader);
      parm->reader = unescape_status_string (line);
    }
  else if (keywordlen == 8 && !memcmp (keyword, "SERIALNO", keywordlen))
    {
      xfree (parm->serialno);
      parm->serialno = store_serialno (line);
      parm->is_v2 = (strlen (parm->serialno) >= 16
                     && (xtoi_2 (parm->serialno+12) == 0 /* Yubikey */
                         || xtoi_2 (parm->serialno+12) >= 2));
    }
  else if (keywordlen == 7 && !memcmp (keyword, "APPTYPE", keywordlen))
    {
      xfree (parm->apptype);
      parm->apptype = unescape_status_string (line);
    }
  else if (keywordlen == 10 && !memcmp (keyword, "APPVERSION", keywordlen))
    {
      unsigned int val = 0;

      sscanf (line, "%x", &val);
      parm->appversion = val;
    }
  else if (keywordlen == 9 && !memcmp (keyword, "DISP-NAME", keywordlen))
    {
      xfree (parm->disp_name);
      parm->disp_name = unescape_status_string (line);
    }
  else if (keywordlen == 9 && !memcmp (keyword, "DISP-LANG", keywordlen))
    {
      xfree (parm->disp_lang);
      parm->disp_lang = unescape_status_string (line);
    }
  else if (keywordlen == 8 && !memcmp (keyword, "DISP-SEX", keywordlen))
    {
      parm->disp_sex = *line == '1'? 1 : *line == '2' ? 2: 0;
    }
  else if (keywordlen == 10 && !memcmp (keyword, "PUBKEY-URL", keywordlen))
    {
      xfree (parm->pubkey_url);
      parm->pubkey_url = unescape_status_string (line);
    }
  else if (keywordlen == 10 && !memcmp (keyword, "LOGIN-DATA", keywordlen))
    {
      xfree (parm->login_data);
      parm->login_data = unescape_status_string (line);
    }
  else if (keywordlen == 11 && !memcmp (keyword, "SIG-COUNTER", keywordlen))
    {
      parm->sig_counter = strtoul (line, NULL, 0);
    }
  else if (keywordlen == 10 && !memcmp (keyword, "CHV-STATUS", keywordlen))
    {
      char *p, *buf;

      buf = p = unescape_status_string (line);
      if (buf)
        {
          while (spacep (p))
            p++;
          parm->chv1_cached = atoi (p);
          while (*p && !spacep (p))
            p++;
          while (spacep (p))
            p++;
          for (i=0; *p && i < 3; i++)
            {
              parm->chvmaxlen[i] = atoi (p);
              while (*p && !spacep (p))
                p++;
              while (spacep (p))
                p++;
            }
          for (i=0; *p && i < 3; i++)
            {
              parm->chvretry[i] = atoi (p);
              while (*p && !spacep (p))
                p++;
              while (spacep (p))
                p++;
            }
          xfree (buf);
        }
    }
  else if (keywordlen == 6 && !memcmp (keyword, "EXTCAP", keywordlen))
    {
      char *p, *p2, *buf;
      int abool;

      buf = p = unescape_status_string (line);
      if (buf)
        {
          for (p = strtok (buf, " "); p; p = strtok (NULL, " "))
            {
              p2 = strchr (p, '=');
              if (p2)
                {
                  *p2++ = 0;
                  abool = (*p2 == '1');
                  if (!strcmp (p, "ki"))
                    parm->extcap.ki = abool;
                  else if (!strcmp (p, "aac"))
                    parm->extcap.aac = abool;
                  else if (!strcmp (p, "bt"))
                    parm->extcap.bt = abool;
                  else if (!strcmp (p, "kdf"))
                    parm->extcap.kdf = abool;
                  else if (!strcmp (p, "si"))
                    parm->status_indicator = strtoul (p2, NULL, 10);
                }
            }
          xfree (buf);
        }
    }
  else if (keywordlen == 7 && !memcmp (keyword, "KEY-FPR", keywordlen))
    {
      int no = atoi (line);
      while (*line && !spacep (line))
        line++;
      while (spacep (line))
        line++;
      if (no == 1)
        parm->fpr1len = unhexify_fpr (line, parm->fpr1, sizeof parm->fpr1);
      else if (no == 2)
        parm->fpr2len = unhexify_fpr (line, parm->fpr2, sizeof parm->fpr2);
      else if (no == 3)
        parm->fpr3len = unhexify_fpr (line, parm->fpr3, sizeof parm->fpr3);
    }
  else if (keywordlen == 8 && !memcmp (keyword, "KEY-TIME", keywordlen))
    {
      int no = atoi (line);
      while (* line && !spacep (line))
        line++;
      while (spacep (line))
        line++;
      if (no == 1)
        parm->fpr1time = strtoul (line, NULL, 10);
      else if (no == 2)
        parm->fpr2time = strtoul (line, NULL, 10);
      else if (no == 3)
        parm->fpr3time = strtoul (line, NULL, 10);
    }
  else if (keywordlen == 11 && !memcmp (keyword, "KEYPAIRINFO", keywordlen))
    {
      const char *hexgrp = line;
      int no;

      while (*line && !spacep (line))
        line++;
      while (spacep (line))
        line++;
      if (strncmp (line, "OPENPGP.", 8))
        ;
      else if ((no = atoi (line+8)) == 1)
        unhexify_fpr (hexgrp, parm->grp1, sizeof parm->grp1);
      else if (no == 2)
        unhexify_fpr (hexgrp, parm->grp2, sizeof parm->grp2);
      else if (no == 3)
        unhexify_fpr (hexgrp, parm->grp3, sizeof parm->grp3);
    }
  else if (keywordlen == 6 && !memcmp (keyword, "CA-FPR", keywordlen))
    {
      int no = atoi (line);
      while (*line && !spacep (line))
        line++;
      while (spacep (line))
        line++;
      if (no == 1)
        parm->cafpr1len = unhexify_fpr (line, parm->cafpr1,sizeof parm->cafpr1);
      else if (no == 2)
        parm->cafpr2len = unhexify_fpr (line, parm->cafpr2,sizeof parm->cafpr2);
      else if (no == 3)
        parm->cafpr3len = unhexify_fpr (line, parm->cafpr3,sizeof parm->cafpr3);
    }
  else if (keywordlen == 8 && !memcmp (keyword, "KEY-ATTR", keywordlen))
    {
      int keyno = 0;
      int algo = PUBKEY_ALGO_RSA;
      int n = 0;

      sscanf (line, "%d %d %n", &keyno, &algo, &n);
      keyno--;
      if (keyno < 0 || keyno >= DIM (parm->key_attr))
        return 0;

      parm->key_attr[keyno].algo = algo;
      if (algo == PUBKEY_ALGO_RSA)
        parm->key_attr[keyno].nbits = strtoul (line+n+3, NULL, 10);
      else if (algo == PUBKEY_ALGO_ECDH || algo == PUBKEY_ALGO_ECDSA
               || algo == PUBKEY_ALGO_EDDSA)
        parm->key_attr[keyno].curve = openpgp_is_curve_supported (line + n,
                                                                  NULL, NULL);
    }
  else if (keywordlen == 12 && !memcmp (keyword, "PRIVATE-DO-", 11)
           && strchr("1234", keyword[11]))
    {
      int no = keyword[11] - '1';
      log_assert (no >= 0 && no <= 3);
      xfree (parm->private_do[no]);
      parm->private_do[no] = unescape_status_string (line);
    }
  else if (keywordlen == 12 && !memcmp (keyword, "MANUFACTURER", 12))
    {
      xfree (parm->manufacturer_name);
      parm->manufacturer_name = NULL;
      parm->manufacturer_id = strtoul (line, &endp, 0);
      while (endp && spacep (endp))
        endp++;
      if (endp && *endp)
        parm->manufacturer_name = xstrdup (endp);
    }
  else if (keywordlen == 3 && !memcmp (keyword, "KDF", 3))
    {
      unsigned char *data = unescape_status_string (line);

      if (data[2] != 0x03)
        parm->kdf_do_enabled = 0;
      else if (data[22] != 0x85)
        parm->kdf_do_enabled = 1;
      else
        parm->kdf_do_enabled = 2;
      xfree (data);
    }
  else if (keywordlen == 5 && !memcmp (keyword, "UIF-", 4)
           && strchr("123", keyword[4]))
    {
      unsigned char *data;
      int no = keyword[4] - '1';

      log_assert (no >= 0 && no <= 2);
      data = unescape_status_string (line);
      parm->uif[no] = (data[0] != 0xff);
      xfree (data);
    }
  else if (keywordlen == 13 && !memcmp (keyword, "KEY-ATTR-INFO", 13))
    {
      if (!strncmp (line, "OPENPGP.", 8))
        {
          int no;

          line += 8;
          no = atoi (line);
          if (no >= 1 && no <= 3)
            {
              no--;
              line++;
              while (spacep (line))
                line++;
              append_to_strlist (&parm->supported_keyalgo[no], xstrdup (line));
            }
        }
        /* Skip when it's not "OPENPGP.[123]".  */
    }

  return 0;
}


/* Call the scdaemon to learn about a smartcard.  Note that in
 * contradiction to the function's name, gpg-agent's LEARN command is
 * used and not the low-level "SCD LEARN".
 * Used by:
 *  card-util.c
 *  keyedit_menu
 *  card_store_key_with_backup  (With force to remove secret key data)
 */
int
agent_scd_learn (struct agent_card_info_s *info, int force)
{
  int rc;
  struct default_inq_parm_s parm;
  struct agent_card_info_s dummyinfo;

  if (!info)
    info = &dummyinfo;
  memset (info, 0, sizeof *info);
  memset (&parm, 0, sizeof parm);

  rc = start_agent (NULL, 1);
  if (rc)
    return rc;

  parm.ctx = agent_ctx;
  rc = assuan_transact (agent_ctx,
                        force ? "LEARN --sendinfo --force" : "LEARN --sendinfo",
                        dummy_data_cb, NULL, default_inq_cb, &parm,
                        learn_status_cb, info);
  /* Also try to get the key attributes.  */
  if (!rc)
    agent_scd_getattr ("KEY-ATTR", info);

  if (info == &dummyinfo)
    agent_release_card_info (info);

  return rc;
}



struct keypairinfo_cb_parm_s
{
  keypair_info_t kpinfo;
  keypair_info_t *kpinfo_tail;
};


/* Callback for the agent_scd_keypairinfo function.  */
static gpg_error_t
scd_keypairinfo_status_cb (void *opaque, const char *line)
{
  struct keypairinfo_cb_parm_s *parm = opaque;
  gpg_error_t err = 0;
  const char *keyword = line;
  int keywordlen;
  char *line_buffer = NULL;
  keypair_info_t kpi = NULL;

  for (keywordlen=0; *line && !spacep (line); line++, keywordlen++)
    ;
  while (spacep (line))
    line++;

  if (keywordlen == 11 && !memcmp (keyword, "KEYPAIRINFO", keywordlen))
    {
      /* The format of such a line is:
       *   KEYPAIRINFO <hexgrip> <keyref> [usage] [keytime] [algostr]
       */
      const char *fields[4];
      int nfields;
      const char *hexgrp, *keyref, *usage;
      time_t atime;
      u32 keytime;

      line_buffer = xtrystrdup (line);
      if (!line_buffer)
        {
          err = gpg_error_from_syserror ();
          goto leave;
        }
      if ((nfields = split_fields (line_buffer, fields, DIM (fields))) < 2)
        goto leave;  /* not enough args - invalid status line - ignore  */

      hexgrp = fields[0];
      keyref = fields[1];
      if (nfields > 2)
        usage = fields[2];
      else
        usage = "";
      if (nfields > 3)
        {
          atime = parse_timestamp (fields[3], NULL);
          if (atime == (time_t)(-1))
            atime = 0;
          keytime = atime;
        }
      else
        keytime = 0;

      kpi = xtrycalloc (1, sizeof *kpi);
      if (!kpi)
        {
          err = gpg_error_from_syserror ();
          goto leave;
        }

      if (*hexgrp == 'X' && !hexgrp[1])
        *kpi->keygrip = 0; /* No hexgrip.  */
      else if (strlen (hexgrp) == 2*KEYGRIP_LEN)
        mem2str (kpi->keygrip, hexgrp, sizeof kpi->keygrip);
      else
        {
          err = gpg_error (GPG_ERR_INV_DATA);
          goto leave;
        }

      if (!*keyref)
        {
          err = gpg_error (GPG_ERR_INV_DATA);
          goto leave;
        }
      kpi->idstr = xtrystrdup (keyref);
      if (!kpi->idstr)
        {
          err = gpg_error_from_syserror ();
          goto leave;
        }

      /* Parse and set the usage.  */
      for (; *usage; usage++)
        {
          switch (*usage)
            {
            case 's': kpi->usage |= GCRY_PK_USAGE_SIGN; break;
            case 'c': kpi->usage |= GCRY_PK_USAGE_CERT; break;
            case 'a': kpi->usage |= GCRY_PK_USAGE_AUTH; break;
            case 'e': kpi->usage |= GCRY_PK_USAGE_ENCR; break;
            }
        }

      kpi->keytime = keytime;

      /* Append to the list.  */
      *parm->kpinfo_tail = kpi;
      parm->kpinfo_tail = &kpi->next;
      kpi = NULL;
    }

 leave:
  free_keypair_info (kpi);
  xfree (line_buffer);
  return err;
}


/* Read the keypairinfo lines of the current card directly from
 * scdaemon.  The list is returned as a string made up of the keygrip,
 * a space and the keyref.  The flags of the string carry the usage
 * bits.  If KEYREF is not NULL, only a single string is returned
 * which matches the given keyref. */
gpg_error_t
agent_scd_keypairinfo (ctrl_t ctrl, const char *keyref, keypair_info_t *r_list)
{
  gpg_error_t err;
  struct keypairinfo_cb_parm_s parm;
  struct default_inq_parm_s inq_parm;
  char line[ASSUAN_LINELENGTH];

  *r_list = NULL;
  err= start_agent (ctrl, 1);
  if (err)
    return err;
  memset (&inq_parm, 0, sizeof inq_parm);
  inq_parm.ctx = agent_ctx;

  parm.kpinfo = NULL;
  parm.kpinfo_tail = &parm.kpinfo;

  if (keyref)
    snprintf (line, DIM(line), "SCD READKEY --info-only %s", keyref);
  else
    snprintf (line, DIM(line), "SCD LEARN --keypairinfo");

  err = assuan_transact (agent_ctx, line,
                         NULL, NULL,
                         default_inq_cb, &inq_parm,
                         scd_keypairinfo_status_cb, &parm);
  if (!err && !parm.kpinfo)
    err = gpg_error (GPG_ERR_NO_DATA);

  if (err)
    free_keypair_info (parm.kpinfo);
  else
    *r_list = parm.kpinfo;
  return err;
}



/* Send an APDU to the current card.  On success the status word is
 * stored at R_SW unless R_SQ is NULL.  With HEXAPDU being NULL only a
 * RESET command is send to scd.  HEXAPDU may also be one of theseo
 * special strings:
 *
 *   "undefined"       :: Send the command "SCD SERIALNO undefined"
 *   "lock"            :: Send the command "SCD LOCK --wait"
 *   "trylock"         :: Send the command "SCD LOCK"
 *   "unlock"          :: Send the command "SCD UNLOCK"
 *   "reset-keep-lock" :: Send the command "SCD RESET --keep-lock"
 *
 * Used by:
 *  card-util.c
 */
gpg_error_t
agent_scd_apdu (const char *hexapdu, unsigned int *r_sw)
{
  gpg_error_t err;

  /* Start the agent but not with the card flag so that we do not
     autoselect the openpgp application.  */
  err = start_agent (NULL, 0);
  if (err)
    return err;

  if (!hexapdu)
    {
      err = assuan_transact (agent_ctx, "SCD RESET",
                             NULL, NULL, NULL, NULL, NULL, NULL);

    }
  else if (!strcmp (hexapdu, "reset-keep-lock"))
    {
      err = assuan_transact (agent_ctx, "SCD RESET --keep-lock",
                             NULL, NULL, NULL, NULL, NULL, NULL);
    }
  else if (!strcmp (hexapdu, "lock"))
    {
      err = assuan_transact (agent_ctx, "SCD LOCK --wait",
                             NULL, NULL, NULL, NULL, NULL, NULL);
    }
  else if (!strcmp (hexapdu, "trylock"))
    {
      err = assuan_transact (agent_ctx, "SCD LOCK",
                             NULL, NULL, NULL, NULL, NULL, NULL);
    }
  else if (!strcmp (hexapdu, "unlock"))
    {
      err = assuan_transact (agent_ctx, "SCD UNLOCK",
                             NULL, NULL, NULL, NULL, NULL, NULL);
    }
  else if (!strcmp (hexapdu, "undefined"))
    {
      err = assuan_transact (agent_ctx, "SCD SERIALNO undefined",
                             NULL, NULL, NULL, NULL, NULL, NULL);
    }
  else
    {
      char line[ASSUAN_LINELENGTH];
      membuf_t mb;
      unsigned char *data;
      size_t datalen;

      init_membuf (&mb, 256);

      snprintf (line, DIM(line), "SCD APDU %s", hexapdu);
      err = assuan_transact (agent_ctx, line,
                             put_membuf_cb, &mb, NULL, NULL, NULL, NULL);
      if (!err)
        {
          data = get_membuf (&mb, &datalen);
          if (!data)
            err = gpg_error_from_syserror ();
          else if (datalen < 2) /* Ooops */
            err = gpg_error (GPG_ERR_CARD);
          else
            {
              *r_sw = buf16_to_uint (data+datalen-2);
            }
          xfree (data);
        }
    }

  return err;
}

int
agent_keytotpm (ctrl_t ctrl, const char *hexgrip)
{
  int rc;
  char line[ASSUAN_LINELENGTH];
  struct default_inq_parm_s parm;

  snprintf(line, DIM(line), "KEYTOTPM %s\n", hexgrip);

  if (strchr (hexgrip, ','))
    {
      log_error ("storing a part of a dual key is not yet supported\n");
      return gpg_error (GPG_ERR_NOT_IMPLEMENTED);
    }

  rc = start_agent (ctrl, 0);
  if (rc)
    return rc;
  parm.ctx = agent_ctx;
  parm.ctrl = ctrl;

  rc = assuan_transact (agent_ctx, line, NULL, NULL, default_inq_cb, &parm,
			NULL, NULL);
  if (rc)
    log_log (GPGRT_LOGLVL_ERROR, _("error from TPM: %s\n"), gpg_strerror (rc));
  return rc;
}


/* Used by:
 *  card_store_subkey
 *  card_store_key_with_backup
 */
int
agent_keytocard (const char *hexgrip, int keyno, int force,
                 const char *serialno, const char *timestamp,
                 const char *ecdh_param_str)
{
  int rc;
  char line[ASSUAN_LINELENGTH];
  struct default_inq_parm_s parm;

  memset (&parm, 0, sizeof parm);

  if (strchr (hexgrip, ','))
    {
      log_error ("storing a part of a dual key is not yet supported\n");
      return gpg_error (GPG_ERR_NOT_IMPLEMENTED);
    }


  snprintf (line, DIM(line), "KEYTOCARD %s%s %s OPENPGP.%d %s%s%s",
            force?"--force ": "", hexgrip, serialno, keyno, timestamp,
            ecdh_param_str? " ":"", ecdh_param_str? ecdh_param_str:"");

  rc = start_agent (NULL, 1);
  if (rc)
    return rc;
  parm.ctx = agent_ctx;

  rc = assuan_transact (agent_ctx, line, NULL, NULL, default_inq_cb, &parm,
                        NULL, NULL);
  status_sc_op_failure (rc);
  return rc;
}



/* Object used with the agent_scd_getattr_one.  */
struct getattr_one_parm_s {
  const char *keyword;  /* Keyword to look for.  */
  char *data;           /* Malloced and unescaped data.  */
  gpg_error_t err;      /* Error code or 0 on success. */
};


/* Callback for agent_scd_getattr_one.  */
static gpg_error_t
getattr_one_status_cb (void *opaque, const char *line)
{
  struct getattr_one_parm_s *parm = opaque;
  const char *s;

  if (parm->data)
    return 0; /* We want only the first occurrence.  */

  if ((s=has_leading_keyword (line, parm->keyword)))
    {
      parm->data = percent_plus_unescape (s, 0xff);
      if (!parm->data)
        parm->err = gpg_error_from_syserror ();
    }

  return 0;
}


/* Simplified version of agent_scd_getattr.  This function returns
 * only the first occurrence of the attribute NAME and stores it at
 * R_VALUE.  A nul in the result is silennly replaced by 0xff.  On
 * error NULL is stored at R_VALUE.  */
gpg_error_t
agent_scd_getattr_one (const char *name, char **r_value)
{
  gpg_error_t err;
  char line[ASSUAN_LINELENGTH];
  struct default_inq_parm_s inqparm;
  struct getattr_one_parm_s parm;

  *r_value = NULL;

  if (!*name)
    return gpg_error (GPG_ERR_INV_VALUE);

  memset (&inqparm, 0, sizeof inqparm);
  inqparm.ctx = agent_ctx;

  memset (&parm, 0, sizeof parm);
  parm.keyword = name;

  /* We assume that NAME does not need escaping. */
  if (12 + strlen (name) > DIM(line)-1)
    return gpg_error (GPG_ERR_TOO_LARGE);
  stpcpy (stpcpy (line, "SCD GETATTR "), name);

  err = start_agent (NULL, 1);
  if (err)
    return err;

  err = assuan_transact (agent_ctx, line,
                         NULL, NULL,
                         default_inq_cb, &inqparm,
                         getattr_one_status_cb, &parm);
  if (!err && parm.err)
    err = parm.err;
  else if (!err && !parm.data)
    err = gpg_error (GPG_ERR_NO_DATA);

  if (!err)
    *r_value = parm.data;
  else
    xfree (parm.data);

  return err;
}



/* Call the agent to retrieve a data object.  This function returns
 * the data in the same structure as used by the learn command.  It is
 * allowed to update such a structure using this command.
 *
 *  Used by:
 *     build_sk_list
 *     enum_secret_keys
 *     get_signature_count
 *     card-util.c
 *     generate_keypair (KEY-ATTR)
 *     card_store_key_with_backup (SERIALNO)
 *     generate_card_subkeypair  (KEY-ATTR)
 */
int
agent_scd_getattr (const char *name, struct agent_card_info_s *info)
{
  int rc;
  char line[ASSUAN_LINELENGTH];
  struct default_inq_parm_s parm;

  memset (&parm, 0, sizeof parm);

  if (!*name)
    return gpg_error (GPG_ERR_INV_VALUE);

  /* We assume that NAME does not need escaping. */
  if (12 + strlen (name) > DIM(line)-1)
    return gpg_error (GPG_ERR_TOO_LARGE);
  stpcpy (stpcpy (line, "SCD GETATTR "), name);

  rc = start_agent (NULL, 1);
  if (rc)
    return rc;

  parm.ctx = agent_ctx;
  rc = assuan_transact (agent_ctx, line, NULL, NULL, default_inq_cb, &parm,
                        learn_status_cb, info);
  if (!rc && !strcmp (name, "KEY-FPR"))
    {
      /* Let the agent create the shadow keys if not yet done.  */
      if (info->fpr1len)
        assuan_transact (agent_ctx, "READKEY --card --no-data -- $SIGNKEYID",
                         NULL, NULL, NULL, NULL, NULL, NULL);
      if (info->fpr2len)
        assuan_transact (agent_ctx, "READKEY --card --no-data -- $ENCRKEYID",
                         NULL, NULL, NULL, NULL, NULL, NULL);
    }

  return rc;
}



/* Send an setattr command to the SCdaemon.
 * Used by:
 *   card-util.c
 */
gpg_error_t
agent_scd_setattr (const char *name, const void *value_arg, size_t valuelen)
{
  gpg_error_t err;
  const unsigned char *value = value_arg;
  char line[ASSUAN_LINELENGTH];
  char *p;
  struct default_inq_parm_s parm;

  memset (&parm, 0, sizeof parm);

  if (!*name || !valuelen)
    return gpg_error (GPG_ERR_INV_VALUE);

  /* We assume that NAME does not need escaping. */
  if (12 + strlen (name) > DIM(line)-1)
    return gpg_error (GPG_ERR_TOO_LARGE);

  p = stpcpy (stpcpy (line, "SCD SETATTR "), name);
  *p++ = ' ';
  for (; valuelen; value++, valuelen--)
    {
      if (p >= line + DIM(line)-5 )
        return gpg_error (GPG_ERR_TOO_LARGE);
      if (*value < ' ' || *value == '+' || *value == '%')
        {
          sprintf (p, "%%%02X", *value);
          p += 3;
        }
      else if (*value == ' ')
        *p++ = '+';
      else
        *p++ = *value;
    }
  *p = 0;

  err = start_agent (NULL, 1);
  if (!err)
    {
      parm.ctx = agent_ctx;
      err = assuan_transact (agent_ctx, line, NULL, NULL,
                            default_inq_cb, &parm, NULL, NULL);
    }

  status_sc_op_failure (err);
  return err;
}



/* Handle a CERTDATA inquiry.  Note, we only send the data,
   assuan_transact takes care of flushing and writing the END
   command. */
static gpg_error_t
inq_writecert_parms (void *opaque, const char *line)
{
  int rc;
  struct writecert_parm_s *parm = opaque;

  if (has_leading_keyword (line, "CERTDATA"))
    {
      rc = assuan_send_data (parm->dflt->ctx,
                             parm->certdata, parm->certdatalen);
    }
  else
    rc = default_inq_cb (parm->dflt, line);

  return rc;
}


/* Send a WRITECERT command to the SCdaemon.
 * Used by:
 *  card-util.c
 */
int
agent_scd_writecert (const char *certidstr,
                     const unsigned char *certdata, size_t certdatalen)
{
  int rc;
  char line[ASSUAN_LINELENGTH];
  struct writecert_parm_s parms;
  struct default_inq_parm_s dfltparm;

  memset (&dfltparm, 0, sizeof dfltparm);

  rc = start_agent (NULL, 1);
  if (rc)
    return rc;

  memset (&parms, 0, sizeof parms);

  snprintf (line, DIM(line), "SCD WRITECERT %s", certidstr);
  dfltparm.ctx = agent_ctx;
  parms.dflt = &dfltparm;
  parms.certdata = certdata;
  parms.certdatalen = certdatalen;

  rc = assuan_transact (agent_ctx, line, NULL, NULL,
                        inq_writecert_parms, &parms, NULL, NULL);

  return rc;
}



/* Status callback for the SCD GENKEY command. */
static gpg_error_t
scd_genkey_cb (void *opaque, const char *line)
{
  u32 *createtime = opaque;
  const char *keyword = line;
  int keywordlen;

  for (keywordlen=0; *line && !spacep (line); line++, keywordlen++)
    ;
  while (spacep (line))
    line++;

 if (keywordlen == 14 && !memcmp (keyword,"KEY-CREATED-AT", keywordlen))
    {
      *createtime = (u32)strtoul (line, NULL, 10);
    }
  else if (keywordlen == 8 && !memcmp (keyword, "PROGRESS", keywordlen))
    {
      write_status_text (STATUS_PROGRESS, line);
    }

  return 0;
}

/* Send a GENKEY command to the SCdaemon.  If *CREATETIME is not 0,
 * the value will be passed to SCDAEMON with --timestamp option so that
 * the key is created with this.  Otherwise, timestamp was generated by
 * SCDEAMON.  On success, creation time is stored back to
 * CREATETIME.
 * Used by:
 *   gen_card_key
 */
int
agent_scd_genkey (int keyno, int force, u32 *createtime)
{
  int rc;
  char line[ASSUAN_LINELENGTH];
  gnupg_isotime_t tbuf;
  struct default_inq_parm_s dfltparm;

  memset (&dfltparm, 0, sizeof dfltparm);

  rc = start_agent (NULL, 1);
  if (rc)
    return rc;

  if (*createtime)
    epoch2isotime (tbuf, *createtime);
  else
    *tbuf = 0;

  snprintf (line, DIM(line), "SCD GENKEY %s%s %s %d",
            *tbuf? "--timestamp=":"", tbuf,
            force? "--force":"",
            keyno);

  dfltparm.ctx = agent_ctx;
  rc = assuan_transact (agent_ctx, line,
                        NULL, NULL, default_inq_cb, &dfltparm,
                        scd_genkey_cb, createtime);

  status_sc_op_failure (rc);
  return rc;
}



/* Return the serial number of the card or an appropriate error.  The
 * serial number is returned as a hexstring.  With DEMAND the active
 * card is switched to the card with that serialno.
 * Used by:
 *   card-util.c
 *   build_sk_list
 *   enum_secret_keys
 */
int
agent_scd_serialno (char **r_serialno, const char *demand)
{
  int err;
  char *serialno = NULL;
  char line[ASSUAN_LINELENGTH];

  if (r_serialno)
    *r_serialno = NULL;

  err = start_agent (NULL, (1 | FLAG_FOR_CARD_SUPPRESS_ERRORS));
  if (err)
    return err;

  if (!demand)
    strcpy (line, "SCD SERIALNO");
  else
    snprintf (line, DIM(line), "SCD SERIALNO --demand=%s", demand);

  err = assuan_transact (agent_ctx, line,
                         NULL, NULL, NULL, NULL,
                         get_serialno_cb, &serialno);
  if (err)
    {
      xfree (serialno);
      return err;
    }

  if (r_serialno)
    *r_serialno = serialno;
  else
    xfree (serialno);

  return 0;
}



/* Send a READCERT command to the SCdaemon.
 * Used by:
 *  card-util.c
 */
int
agent_scd_readcert (const char *certidstr,
                    void **r_buf, size_t *r_buflen)
{
  int rc;
  char line[ASSUAN_LINELENGTH];
  membuf_t data;
  size_t len;
  struct default_inq_parm_s dfltparm;

  memset (&dfltparm, 0, sizeof dfltparm);

  *r_buf = NULL;
  rc = start_agent (NULL, 1);
  if (rc)
    return rc;

  dfltparm.ctx = agent_ctx;

  init_membuf (&data, 2048);

  snprintf (line, DIM(line), "SCD READCERT %s", certidstr);
  rc = assuan_transact (agent_ctx, line,
                        put_membuf_cb, &data,
                        default_inq_cb, &dfltparm,
                        NULL, NULL);
  if (rc)
    {
      xfree (get_membuf (&data, &len));
      return rc;
    }
  *r_buf = get_membuf (&data, r_buflen);
  if (!*r_buf)
    return gpg_error (GPG_ERR_ENOMEM);

  return 0;
}


/* Callback for the agent_scd_readkey function.  */
static gpg_error_t
readkey_status_cb (void *opaque, const char *line)
{
  u32 *keytimep = opaque;
  gpg_error_t err = 0;
  const char *args;
  char *line_buffer = NULL;

  /* FIXME: Get that info from the KEYPAIRINFO line.  */
  if ((args = has_leading_keyword (line, "KEYPAIRINFO"))
      && !*keytimep)
    {
      /* The format of such a line is:
       *   KEYPAIRINFO <hexgrip> <keyref> [usage] [keytime]
       *
       * Note that we use only the first valid KEYPAIRINFO line.  More
       * lines are possible if a second card carries the same key.
       */
      const char *fields[4];
      int nfields;
      time_t atime;

      line_buffer = xtrystrdup (line);
      if (!line_buffer)
        {
          err = gpg_error_from_syserror ();
          goto leave;
        }
      if ((nfields = split_fields (line_buffer, fields, DIM (fields))) < 4)
        goto leave;  /* not enough args - ignore  */

      if (nfields > 3)
        {
          atime = parse_timestamp (fields[3], NULL);
          if (atime == (time_t)(-1))
            atime = 0;
          *keytimep = atime;
        }
      else
        *keytimep = 0;
    }

 leave:
  xfree (line_buffer);
  return err;
}


/* This is a variant of agent_readkey which sends a READKEY command
 * directly Scdaemon.  On success a new s-expression is stored at
 * R_RESULT.  If R_KEYTIME is not NULL the key cresation time of an
 * OpenPGP card is stored there - if that is not known 0 is stored.
 * In the latter case it is allowed to pass NULL for R_RESULT.  */
gpg_error_t
agent_scd_readkey (ctrl_t ctrl, const char *keyrefstr,
                   gcry_sexp_t *r_result, u32 *r_keytime)
{
  gpg_error_t err;
  char line[ASSUAN_LINELENGTH];
  membuf_t data;
  unsigned char *buf;
  size_t len, buflen;
  struct default_inq_parm_s dfltparm;
  u32 keytime;

  memset (&dfltparm, 0, sizeof dfltparm);
  dfltparm.ctx = agent_ctx;

  if (r_result)
    *r_result = NULL;
  if (r_keytime)
    *r_keytime = 0;
  err = start_agent (ctrl, 1);
  if (err)
    return err;

  init_membuf (&data, 1024);
  snprintf (line, DIM(line),
            "SCD READKEY --info%s -- %s",
            r_result? "":"-only", keyrefstr);
  keytime = 0;
  err = assuan_transact (agent_ctx, line,
                         put_membuf_cb, &data,
                         default_inq_cb, &dfltparm,
                         readkey_status_cb, &keytime);
  if (err)
    {
      xfree (get_membuf (&data, &len));
      return err;
    }
  buf = get_membuf (&data, &buflen);
  if (!buf)
    return gpg_error_from_syserror ();

  if (r_result)
    err = gcry_sexp_new (r_result, buf, buflen, 0);
  else
    err = 0;
  xfree (buf);

  if (!err && r_keytime)
    *r_keytime = keytime;
  return err;
}



struct card_cardlist_parm_s {
  int error;
  strlist_t list;
};


/* Callback function for agent_card_cardlist.  */
static gpg_error_t
card_cardlist_cb (void *opaque, const char *line)
{
  struct card_cardlist_parm_s *parm = opaque;
  const char *keyword = line;
  int keywordlen;

  for (keywordlen=0; *line && !spacep (line); line++, keywordlen++)
    ;
  while (spacep (line))
    line++;

  if (keywordlen == 8 && !memcmp (keyword, "SERIALNO", keywordlen))
    {
      const char *s;
      int n;

      for (n=0,s=line; hexdigitp (s); s++, n++)
        ;

      if (!n || (n&1) || *s)
        parm->error = gpg_error (GPG_ERR_ASS_PARAMETER);
      else
        add_to_strlist (&parm->list, line);
    }

  return 0;
}


/* Return a list of currently available cards.
 * Used by:
 *   card-util.c
 *   skclist.c
 */
int
agent_scd_cardlist (strlist_t *result)
{
  int err;
  char line[ASSUAN_LINELENGTH];
  struct card_cardlist_parm_s parm;

  memset (&parm, 0, sizeof parm);
  *result = NULL;
  err = start_agent (NULL, 1 | FLAG_FOR_CARD_SUPPRESS_ERRORS);
  if (err)
    return err;

  strcpy (line, "SCD GETINFO card_list");

  err = assuan_transact (agent_ctx, line,
                         NULL, NULL, NULL, NULL,
                         card_cardlist_cb, &parm);
  if (!err && parm.error)
    err = parm.error;

  if (!err)
    *result = parm.list;
  else
    free_strlist (parm.list);

  return 0;
}


/* Make the app APPNAME the one on the card.  This is sometimes
 * required to make sure no other process has switched a card to
 * another application.  The only useful APPNAME is "openpgp".  */
gpg_error_t
agent_scd_switchapp (const char *appname)
{
  int err;
  char line[ASSUAN_LINELENGTH];

  if (appname && !*appname)
    appname = NULL;

  err = start_agent (NULL, (1 | FLAG_FOR_CARD_SUPPRESS_ERRORS));
  if (err)
    return err;

  snprintf (line, DIM(line), "SCD SWITCHAPP --%s%s",
            appname? " ":"", appname? appname:"");
  return assuan_transact (agent_ctx, line,
                          NULL, NULL, NULL, NULL,
                          NULL, NULL);
}



struct card_keyinfo_parm_s {
  int error;
  keypair_info_t list;
};

/* Callback function for agent_card_keylist.  */
static gpg_error_t
card_keyinfo_cb (void *opaque, const char *line)
{
  gpg_error_t err = 0;
  struct card_keyinfo_parm_s *parm = opaque;
  const char *keyword = line;
  int keywordlen;
  keypair_info_t keyinfo = NULL;

  for (keywordlen=0; *line && !spacep (line); line++, keywordlen++)
    ;
  while (spacep (line))
    line++;

  if (keywordlen == 7 && !memcmp (keyword, "KEYINFO", keywordlen))
    {
      const char *s;
      int n;
      keypair_info_t *l_p = &parm->list;

      while ((*l_p))
        l_p = &(*l_p)->next;

      keyinfo = xtrycalloc (1, sizeof *keyinfo);
      if (!keyinfo)
        goto alloc_error;

      for (n=0,s=line; hexdigitp (s); s++, n++)
        ;

      if (n != 40)
        goto parm_error;

      memcpy (keyinfo->keygrip, line, 40);
      keyinfo->keygrip[40] = 0;

      line = s;

      if (!*line)
        goto parm_error;

      while (spacep (line))
        line++;

      if (*line++ != 'T')
        goto parm_error;

      if (!*line)
        goto parm_error;

      while (spacep (line))
        line++;

      for (n=0,s=line; hexdigitp (s); s++, n++)
        ;

      if (!n)
        goto parm_error;

      keyinfo->serialno = xtrymalloc (n+1);
      if (!keyinfo->serialno)
        goto alloc_error;

      memcpy (keyinfo->serialno, line, n);
      keyinfo->serialno[n] = 0;

      line = s;

      if (!*line)
        goto parm_error;

      while (spacep (line))
        line++;

      if (!*line)
        goto parm_error;

      keyinfo->idstr = xtrystrdup (line);
      if (!keyinfo->idstr)
        goto alloc_error;

      *l_p = keyinfo;
    }

  return err;

 alloc_error:
  xfree (keyinfo);
  if (!parm->error)
    parm->error = gpg_error_from_syserror ();
  return 0;

 parm_error:
  xfree (keyinfo);
  if (!parm->error)
    parm->error = gpg_error (GPG_ERR_ASS_PARAMETER);
  return 0;
}


/* Free a keypair info list.  */
void
free_keypair_info (keypair_info_t l)
{
  keypair_info_t l_next;

  for (; l; l = l_next)
    {
      l_next = l->next;
      xfree (l->serialno);
      xfree (l->idstr);
      xfree (l);
    }
}

/* Call the scdaemon to check if a key of KEYGRIP is available, or
   retrieve list of available keys on cards.  With CAP, we can limit
   keys with specified capability.  On success, the allocated
   structure is stored at RESULT.  On error, an error code is returned
   and NULL is stored at RESULT.  */
gpg_error_t
agent_scd_keyinfo (const char *keygrip, int cap,
                   keypair_info_t *result)
{
  int err;
  struct card_keyinfo_parm_s parm;
  char line[ASSUAN_LINELENGTH];
  char *list_option;

  *result = NULL;

  switch (cap)
    {
    case                  0: list_option = "--list";      break;
    case GCRY_PK_USAGE_SIGN: list_option = "--list=sign"; break;
    case GCRY_PK_USAGE_ENCR: list_option = "--list=encr"; break;
    case GCRY_PK_USAGE_AUTH: list_option = "--list=auth"; break;
    default:                 return gpg_error (GPG_ERR_INV_VALUE);
    }

  memset (&parm, 0, sizeof parm);
  snprintf (line, sizeof line, "SCD KEYINFO %s",
            keygrip ? keygrip : list_option);

  err = start_agent (NULL, 1 | FLAG_FOR_CARD_SUPPRESS_ERRORS);
  if (err)
    return err;

  err = assuan_transact (agent_ctx, line,
                         NULL, NULL, NULL, NULL,
                         card_keyinfo_cb, &parm);
  if (!err && parm.error)
    err = parm.error;

  if (!err)
    *result = parm.list;
  else
    free_keypair_info (parm.list);

  return err;
}

/* Change the PIN of an OpenPGP card or reset the retry counter.
 * CHVNO 1: Change the PIN
 *       2: For v1 cards: Same as 1.
 *          For v2 cards: Reset the PIN using the Reset Code.
 *       3: Change the admin PIN
 *     101: Set a new PIN and reset the retry counter
 *     102: For v1 cars: Same as 101.
 *          For v2 cards: Set a new Reset Code.
 * SERIALNO is not used.
 * Used by:
 *  card-util.c
 */
int
agent_scd_change_pin (int chvno, const char *serialno)
{
  int rc;
  char line[ASSUAN_LINELENGTH];
  const char *reset = "";
  struct default_inq_parm_s dfltparm;

  memset (&dfltparm, 0, sizeof dfltparm);

  (void)serialno;

  if (chvno >= 100)
    reset = "--reset";
  chvno %= 100;

  rc = start_agent (NULL, 1);
  if (rc)
    return rc;
  dfltparm.ctx = agent_ctx;

  snprintf (line, DIM(line), "SCD PASSWD %s %d", reset, chvno);
  rc = assuan_transact (agent_ctx, line,
                        NULL, NULL,
                        default_inq_cb, &dfltparm,
                        NULL, NULL);
  status_sc_op_failure (rc);
  return rc;
}


/* Perform a CHECKPIN operation.  SERIALNO should be the serial
 * number of the card - optionally followed by the fingerprint;
 * however the fingerprint is ignored here.
 * Used by:
 *  card-util.c
 */
int
agent_scd_checkpin  (const char *serialno)
{
  int rc;
  char line[ASSUAN_LINELENGTH];
  struct default_inq_parm_s dfltparm;

  memset (&dfltparm, 0, sizeof dfltparm);

  rc = start_agent (NULL, 1);
  if (rc)
    return rc;
  dfltparm.ctx = agent_ctx;

  snprintf (line, DIM(line), "SCD CHECKPIN %s", serialno);
  rc = assuan_transact (agent_ctx, line,
                        NULL, NULL,
                        default_inq_cb, &dfltparm,
                        NULL, NULL);
  status_sc_op_failure (rc);
  return rc;
}



/* Note: All strings shall be UTF-8. On success the caller needs to
   free the string stored at R_PASSPHRASE. On error NULL will be
   stored at R_PASSPHRASE and an appropriate error code returned.
   Only called from passphrase.c:passphrase_get - see there for more
   comments on this ugly API. */
gpg_error_t
agent_get_passphrase (const char *cache_id,
                      const char *err_msg,
                      const char *prompt,
                      const char *desc_msg,
                      int newsymkey,
                      int repeat,
                      int check,
                      char **r_passphrase)
{
  int rc;
  char line[ASSUAN_LINELENGTH];
  char *arg1 = NULL;
  char *arg2 = NULL;
  char *arg3 = NULL;
  char *arg4 = NULL;
  membuf_t data;
  struct default_inq_parm_s dfltparm;
  int have_newsymkey, wasconf;

  memset (&dfltparm, 0, sizeof dfltparm);

  *r_passphrase = NULL;

  rc = start_agent (NULL, 0);
  if (rc)
    return rc;
  dfltparm.ctx = agent_ctx;

  /* Check that the gpg-agent understands the repeat option.  */
  if (assuan_transact (agent_ctx,
                       "GETINFO cmd_has_option GET_PASSPHRASE repeat",
                       NULL, NULL, NULL, NULL, NULL, NULL))
    return gpg_error (GPG_ERR_NOT_SUPPORTED);
  have_newsymkey = !(assuan_transact
                     (agent_ctx,
                      "GETINFO cmd_has_option GET_PASSPHRASE newsymkey",
                      NULL, NULL, NULL, NULL, NULL, NULL));

  if (cache_id && *cache_id)
    if (!(arg1 = percent_plus_escape (cache_id)))
      goto no_mem;
  if (err_msg && *err_msg)
    if (!(arg2 = percent_plus_escape (err_msg)))
      goto no_mem;
  if (prompt && *prompt)
    if (!(arg3 = percent_plus_escape (prompt)))
      goto no_mem;
  if (desc_msg && *desc_msg)
    if (!(arg4 = percent_plus_escape (desc_msg)))
      goto no_mem;

  /* CHECK && REPEAT or NEWSYMKEY is here an indication that a new
   * passphrase for symmetric encryption is requested; if the agent
   * supports this we enable the modern API by also passing --newsymkey.  */
  snprintf (line, DIM(line),
            "GET_PASSPHRASE --data --repeat=%d%s%s -- %s %s %s %s",
            repeat,
            ((repeat && check) || newsymkey)? " --check":"",
            (have_newsymkey && newsymkey)? " --newsymkey":"",
            arg1? arg1:"X",
            arg2? arg2:"X",
            arg3? arg3:"X",
            arg4? arg4:"X");
  xfree (arg1);
  xfree (arg2);
  xfree (arg3);
  xfree (arg4);

  init_membuf_secure (&data, 64);
  wasconf = assuan_get_flag (agent_ctx, ASSUAN_CONFIDENTIAL);
  assuan_begin_confidential (agent_ctx);
  rc = assuan_transact (agent_ctx, line,
                        put_membuf_cb, &data,
                        default_inq_cb, &dfltparm,
                        NULL, NULL);
  if (!wasconf)
    assuan_end_confidential (agent_ctx);

  if (rc)
    xfree (get_membuf (&data, NULL));
  else
    {
      put_membuf (&data, "", 1);
      *r_passphrase = get_membuf (&data, NULL);
      if (!*r_passphrase)
        rc = gpg_error_from_syserror ();
    }
  return rc;
 no_mem:
  rc = gpg_error_from_syserror ();
  xfree (arg1);
  xfree (arg2);
  xfree (arg3);
  xfree (arg4);
  return rc;
}


gpg_error_t
agent_clear_passphrase (const char *cache_id)
{
  int rc;
  char line[ASSUAN_LINELENGTH];
  struct default_inq_parm_s dfltparm;

  memset (&dfltparm, 0, sizeof dfltparm);

  if (!cache_id || !*cache_id)
    return 0;

  rc = start_agent (NULL, 0);
  if (rc)
    return rc;
  dfltparm.ctx = agent_ctx;

  snprintf (line, DIM(line), "CLEAR_PASSPHRASE %s", cache_id);
  return assuan_transact (agent_ctx, line,
                          NULL, NULL,
                          default_inq_cb, &dfltparm,
                          NULL, NULL);
}


/* Ask the agent to pop up a confirmation dialog with the text DESC
   and an okay and cancel button. */
gpg_error_t
gpg_agent_get_confirmation (const char *desc)
{
  int rc;
  char *tmp;
  char line[ASSUAN_LINELENGTH];
  struct default_inq_parm_s dfltparm;

  memset (&dfltparm, 0, sizeof dfltparm);

  rc = start_agent (NULL, 0);
  if (rc)
    return rc;
  dfltparm.ctx = agent_ctx;

  tmp = percent_plus_escape (desc);
  if (!tmp)
    return gpg_error_from_syserror ();
  snprintf (line, DIM(line), "GET_CONFIRMATION %s", tmp);
  xfree (tmp);

  rc = assuan_transact (agent_ctx, line,
                        NULL, NULL,
                        default_inq_cb, &dfltparm,
                        NULL, NULL);
  return rc;
}


/* Return the S2K iteration count as computed by gpg-agent.  On error
 * print a warning and return a default value. */
unsigned long
agent_get_s2k_count (void)
{
  gpg_error_t err;
  membuf_t data;
  char *buf;
  unsigned long count = 0;

  err = start_agent (NULL, 0);
  if (err)
    goto leave;

  init_membuf (&data, 32);
  err = assuan_transact (agent_ctx, "GETINFO s2k_count",
                        put_membuf_cb, &data,
                        NULL, NULL, NULL, NULL);
  if (err)
    xfree (get_membuf (&data, NULL));
  else
    {
      put_membuf (&data, "", 1);
      buf = get_membuf (&data, NULL);
      if (!buf)
        err = gpg_error_from_syserror ();
      else
        {
          count = strtoul (buf, NULL, 10);
          xfree (buf);
        }
    }

 leave:
  if (err || count < 65536)
    {
      /* Don't print an error if an older agent is used.  */
      if (err && gpg_err_code (err) != GPG_ERR_ASS_PARAMETER)
        log_error (_("problem with the agent: %s\n"), gpg_strerror (err));

      /* Default to 65536 which was used up to 2.0.13.  */
      count = 65536;
    }

  return count;
}



struct keyinfo_data_parm_s
{
  char *serialno;
  int is_smartcard;
  int passphrase_cached;
  int cleartext;
  int card_available;
};


static gpg_error_t
keyinfo_status_cb (void *opaque, const char *line)
{
  struct keyinfo_data_parm_s *data = opaque;
  char *s;

  if ((s = has_leading_keyword (line, "KEYINFO")) && data)
    {
      /* Parse the arguments:
       *      0        1        2        3       4          5
       *   <keygrip> <type> <serialno> <idstr> <cached> <protection>
       *
       *      6        7        8
       *   <sshfpr>  <ttl>  <flags>
       */
      const char *fields[9];

      if (split_fields (s, fields, DIM (fields)) == 9)
        {
          data->is_smartcard = (fields[1][0] == 'T');
          if (data->is_smartcard && !data->serialno && strcmp (fields[2], "-"))
            data->serialno = xtrystrdup (fields[2]);
          /* '1' for cached */
          data->passphrase_cached = (fields[4][0] == '1');
          /* 'P' for protected, 'C' for clear */
          data->cleartext = (fields[5][0] == 'C');
          /* 'A' for card is available */
          data->card_available = (fields[8][0] == 'A');
        }
    }
  return 0;
}


/* Ask the agent whether a secret key for the given public key is
 * available.  Returns 0 if not available.  Bigger value is preferred.
 * Will never return a value less than 0.   Defined return values are:
 *  0 := No key or error
 *  1 := Key available
 *  2 := Key available on a smartcard
 *  3 := Key available and passphrase cached
 *  4 := Key available on current smartcard
 */
int
agent_probe_secret_key (ctrl_t ctrl, PKT_public_key *pk)
{
  gpg_error_t err;
  char line[ASSUAN_LINELENGTH];
  char *hexgrip, *p;
  struct keyinfo_data_parm_s keyinfo;
  int result, result2;

  memset (&keyinfo, 0, sizeof keyinfo);

  err = start_agent (ctrl, 0);
  if (err)
    return 0;

  err = hexkeygrip_from_pk (pk, &hexgrip);
  if (err)
    return 0;
  if ((p=strchr (hexgrip, ',')))
    *p++ = 0;

  snprintf (line, sizeof line, "KEYINFO %s", hexgrip);

  err = assuan_transact (agent_ctx, line, NULL, NULL, NULL, NULL,
                         keyinfo_status_cb, &keyinfo);
  xfree (keyinfo.serialno);
  if (err)
    result = 0;
  else if (keyinfo.card_available)
    result = 4;
  else if (keyinfo.passphrase_cached)
    result = 3;
  else if (keyinfo.is_smartcard)
    result = 2;
  else
    result = 1;

  if (!p)
    {
      xfree (hexgrip);
      return result;  /* Not a dual algo - we are ready.  */
    }

  /* Now check the second keygrip.  */
  memset (&keyinfo, 0, sizeof keyinfo);
  snprintf (line, sizeof line, "KEYINFO %s", p);

  err = assuan_transact (agent_ctx, line, NULL, NULL, NULL, NULL,
                         keyinfo_status_cb, &keyinfo);
  xfree (keyinfo.serialno);
  if (err)
    result2 = 0;
  else if (keyinfo.card_available)
    result2 = 4;
  else if (keyinfo.passphrase_cached)
    result2 = 3;
  else if (keyinfo.is_smartcard)
    result2 = 2;
  else
    result2 = 1;

  xfree (hexgrip);

  if (result == result2)
    return result;  /* Both keys have the same status.  */
  else if (!result && result2)
    return 0;       /* Only first key available - return no key.  */
  else if (result && !result2)
    return 0;       /* Only second key not available - return no key.  */
  else if (result == 4 || result == 2)
    return result;  /* First key on card - don't care where the second is.  */
  else
    return result;
}


/* Ask the agent whether a secret key is available for any of the
   keys (primary or sub) in KEYBLOCK.  Returns 0 if available.  */
gpg_error_t
agent_probe_any_secret_key (ctrl_t ctrl, kbnode_t keyblock)
{
  gpg_error_t err;
  char line[ASSUAN_LINELENGTH];
  char *p;
  kbnode_t kbctx, node;
  int nkeys;  /* (always zero in secret_keygrips mode)  */
  unsigned char grip[KEYGRIP_LEN];
  unsigned char grip2[KEYGRIP_LEN];
  int grip2_valid;
  const unsigned char *s;
  unsigned int n;

  err = start_agent (ctrl, 0);
  if (err)
    return err;

  /* If we have not yet issued a "HAVEKEY --list" do that now.  We use
   * a more or less arbitrary limit of 1000 keys.  */
  if (ctrl && !ctrl->secret_keygrips && !ctrl->no_more_secret_keygrips)
    {
      membuf_t data;

      init_membuf (&data, 4096);
      err = assuan_transact (agent_ctx, "HAVEKEY --list=1000",
                             put_membuf_cb, &data,
                             NULL, NULL, NULL, NULL);
      if (err)
        xfree (get_membuf (&data, NULL));
      else
        {
          ctrl->secret_keygrips = get_membuf (&data,
                                              &ctrl->secret_keygrips_len);
          if (!ctrl->secret_keygrips)
            err = gpg_error_from_syserror ();
          if ((ctrl->secret_keygrips_len % 20))
            {
              err = gpg_error (GPG_ERR_INV_DATA);
              xfree (ctrl->secret_keygrips);
              ctrl->secret_keygrips = NULL;
            }
        }
      if (err)
        {
          if (!opt.quiet)
            log_info ("problem with fast path key listing: %s - ignored\n",
                      gpg_strerror (err));
          err = 0;
        }
      /* We want to do this only once.  */
      ctrl->no_more_secret_keygrips = 1;
    }

  err = gpg_error (GPG_ERR_NO_SECKEY); /* Just in case no key was
                                          found in KEYBLOCK.  */
  p = stpcpy (line, "HAVEKEY");
  for (kbctx=NULL, nkeys=0; (node = walk_kbnode (keyblock, &kbctx, 0)); )
    if (node->pkt->pkttype == PKT_PUBLIC_KEY
        || node->pkt->pkttype == PKT_PUBLIC_SUBKEY
        || node->pkt->pkttype == PKT_SECRET_KEY
        || node->pkt->pkttype == PKT_SECRET_SUBKEY)
      {
        if (ctrl && ctrl->secret_keygrips)
          {
            /* We got an array with all secret keygrips.  Check this.  */
            err = keygrip_from_pk (node->pkt->pkt.public_key, grip, 0);
            if (err)
              return err;
            err = keygrip_from_pk (node->pkt->pkt.public_key, grip2, 1);
            if (err && gpg_err_code (err) != GPG_ERR_FALSE)
              return err;
            grip2_valid = !err;

            for (s=ctrl->secret_keygrips, n = 0;
                 n < ctrl->secret_keygrips_len;
                 s += 20, n += 20)
              {
                if (!memcmp (s, grip, 20))
                  return 0;
                if (grip2_valid && !memcmp (s, grip2, 20))
                  return 0;
              }
            err = gpg_error (GPG_ERR_NO_SECKEY);
            /* Keep on looping over the keyblock.  Never bump nkeys.  */
          }
        else
          {
            if (nkeys
                && ((p - line) + 4*KEYGRIP_LEN+1+1) > (ASSUAN_LINELENGTH - 2))
              {
                err = assuan_transact (agent_ctx, line,
                                       NULL, NULL, NULL, NULL, NULL, NULL);
                if (err != gpg_err_code (GPG_ERR_NO_SECKEY))
                  break; /* Seckey available or unexpected error - ready.  */
                p = stpcpy (line, "HAVEKEY");
                nkeys = 0;
              }

            err = keygrip_from_pk (node->pkt->pkt.public_key, grip, 0);
            if (err)
              return err;
            *p++ = ' ';
            bin2hex (grip, 20, p);
            p += 40;
            nkeys++;

            err = keygrip_from_pk (node->pkt->pkt.public_key, grip2, 1);
            if (err)
              {
                if (gpg_err_code (err) == GPG_ERR_FALSE) /* No second keygrip.  */
                  err = 0;
                else
                  return err;
              }
            else /* Add the second keygrip from dual algos.  */
              {
                *p++ = ' ';
                bin2hex (grip2, 20, p);
                p += 40;
                nkeys++;
              }
          }
      }

  if (!err && nkeys)
    err = assuan_transact (agent_ctx, line,
                           NULL, NULL, NULL, NULL, NULL, NULL);

  return err;
}



/* Return the serial number for a secret key.  If the returned serial
   number is NULL, the key is not stored on a smartcard.  Caller needs
   to free R_SERIALNO.

   if r_cleartext is not NULL, the referenced int will be set to 1 if
   the agent's copy of the key is stored in the clear, or 0 otherwise
*/
gpg_error_t
agent_get_keyinfo (ctrl_t ctrl, const char *hexkeygrip,
                   char **r_serialno, int *r_cleartext)
{
  gpg_error_t err;
  char line[ASSUAN_LINELENGTH];
  struct keyinfo_data_parm_s keyinfo;
  const char *s;

  memset (&keyinfo, 0,sizeof keyinfo);

  *r_serialno = NULL;

  err = start_agent (ctrl, 0);
  if (err)
    return err;

  /* FIXME: Support dual keys.  Maybe under the assumption that the
   *        first key might be on a card.  */
  if (!hexkeygrip)
    return gpg_error (GPG_ERR_INV_VALUE);
  s = strchr (hexkeygrip, ',');
  if (!s)
    s = hexkeygrip + strlen (hexkeygrip);
  if (s - hexkeygrip != 40)
    return gpg_error (GPG_ERR_INV_VALUE);

  /* Note that for a dual algo we only get info for the first key.
   * FIXME: We need to see how we can show the status of the second
   * key in a key listing. */
  snprintf (line, DIM(line), "KEYINFO %.40s", hexkeygrip);

  err = assuan_transact (agent_ctx, line, NULL, NULL, NULL, NULL,
                         keyinfo_status_cb, &keyinfo);
  if (!err && keyinfo.serialno)
    {
      /* Sanity check for bad characters.  */
      if (strpbrk (keyinfo.serialno, ":\n\r"))
        err = GPG_ERR_INV_VALUE;
    }
  if (err)
    xfree (keyinfo.serialno);
  else
    {
      *r_serialno = keyinfo.serialno;
      if (r_cleartext)
        *r_cleartext = keyinfo.cleartext;
    }
  return err;
}


/* Status callback for agent_import_key, agent_export_key and
   agent_genkey.  */
static gpg_error_t
cache_nonce_status_cb (void *opaque, const char *line)
{
  struct cache_nonce_parm_s *parm = opaque;
  const char *s;

  if ((s = has_leading_keyword (line, "CACHE_NONCE")))
    {
      if (parm->cache_nonce_addr)
        {
          xfree (*parm->cache_nonce_addr);
          *parm->cache_nonce_addr = xtrystrdup (s);
        }
    }
  else if ((s = has_leading_keyword (line, "PASSWD_NONCE")))
    {
      if (parm->passwd_nonce_addr)
        {
          xfree (*parm->passwd_nonce_addr);
          *parm->passwd_nonce_addr = xtrystrdup (s);
        }
    }
  else if ((s = has_leading_keyword (line, "PROGRESS")))
    {
      if (opt.enable_progress_filter)
        write_status_text (STATUS_PROGRESS, s);
    }

  return 0;
}



/* Handle a KEYPARMS inquiry.  Note, we only send the data,
   assuan_transact takes care of flushing and writing the end */
static gpg_error_t
inq_genkey_parms (void *opaque, const char *line)
{
  struct genkey_parm_s *parm = opaque;
  gpg_error_t err;

  if (has_leading_keyword (line, "KEYPARAM"))
    {
      err = assuan_send_data (parm->dflt->ctx,
                              parm->keyparms, strlen (parm->keyparms));
    }
  else if (has_leading_keyword (line, "NEWPASSWD") && parm->passphrase)
    {
      err = assuan_send_data (parm->dflt->ctx,
                              parm->passphrase,  strlen (parm->passphrase));
    }
  else
    err = default_inq_cb (parm->dflt, line);

  return err;
}


/* Call the agent to generate a new key.  KEYPARMS is the usual
   S-expression giving the parameters of the key.  gpg-agent passes it
   gcry_pk_genkey.  If NO_PROTECTION is true the agent is advised not
   to protect the generated key.  If NO_PROTECTION is not set and
   PASSPHRASE is not NULL the agent is requested to protect the key
   with that passphrase instead of asking for one.  TIMESTAMP is the
   creation time of the key or zero.  */
gpg_error_t
agent_genkey (ctrl_t ctrl, char **cache_nonce_addr, char **passwd_nonce_addr,
              const char *keyparms, int no_protection,
              const char *passphrase, time_t timestamp, gcry_sexp_t *r_pubkey)
{
  gpg_error_t err;
  struct genkey_parm_s gk_parm;
  struct cache_nonce_parm_s cn_parm;
  struct default_inq_parm_s dfltparm;
  membuf_t data;
  size_t len;
  unsigned char *buf;
  char timestamparg[16 + 16];  /* The 2nd 16 is sizeof(gnupg_isotime_t) */
  char line[ASSUAN_LINELENGTH];

  memset (&dfltparm, 0, sizeof dfltparm);
  dfltparm.ctrl = ctrl;

  *r_pubkey = NULL;
  err = start_agent (ctrl, 0);
  if (err)
    return err;
  dfltparm.ctx = agent_ctx;

  /* Do not use our cache of secret keygrips anymore - this command
   * would otherwise requiring to update that cache.  */
  if (ctrl && ctrl->secret_keygrips)
    {
      xfree (ctrl->secret_keygrips);
      ctrl->secret_keygrips = 0;
    }

  if (timestamp)
    {
      strcpy (timestamparg, " --timestamp=");
      epoch2isotime (timestamparg+13, timestamp);
    }
  else
    *timestamparg = 0;

  if (passwd_nonce_addr && *passwd_nonce_addr)
    ; /* A RESET would flush the passwd nonce cache.  */
  else
    {
      err = assuan_transact (agent_ctx, "RESET",
                             NULL, NULL, NULL, NULL, NULL, NULL);
      if (err)
        return err;
    }

  init_membuf (&data, 1024);
  gk_parm.dflt     = &dfltparm;
  gk_parm.keyparms = keyparms;
  gk_parm.passphrase = passphrase;
  snprintf (line, sizeof line, "GENKEY%s%s%s%s%s%s",
            *timestamparg? timestamparg : "",
            no_protection? " --no-protection" :
            passphrase   ? " --inq-passwd" :
            /*          */ "",
            passwd_nonce_addr && *passwd_nonce_addr? " --passwd-nonce=":"",
            passwd_nonce_addr && *passwd_nonce_addr? *passwd_nonce_addr:"",
            cache_nonce_addr && *cache_nonce_addr? " ":"",
            cache_nonce_addr && *cache_nonce_addr? *cache_nonce_addr:"");
  cn_parm.cache_nonce_addr = cache_nonce_addr;
  cn_parm.passwd_nonce_addr = NULL;
  err = assuan_transact (agent_ctx, line,
                         put_membuf_cb, &data,
                         inq_genkey_parms, &gk_parm,
                         cache_nonce_status_cb, &cn_parm);
  if (err)
    {
      xfree (get_membuf (&data, &len));
      return err;
    }

  buf = get_membuf (&data, &len);
  if (!buf)
    err = gpg_error_from_syserror ();
  else
    {
      err = gcry_sexp_sscan (r_pubkey, NULL, buf, len);
      xfree (buf);
    }
  return err;
}


/* Add the Link attribute to both given keys.  */
gpg_error_t
agent_crosslink_keys (ctrl_t ctrl, const char *hexgrip1, const char *hexgrip2)
{
  gpg_error_t err;
  char line[ASSUAN_LINELENGTH];

  err = start_agent (ctrl, 0);
  if (err)
    goto leave;

  snprintf (line, sizeof line, "KEYATTR %s Link: %s", hexgrip1, hexgrip2);
  err = assuan_transact (agent_ctx, line, NULL, NULL, NULL, NULL, NULL, NULL);
  if (err)
    goto leave;

  snprintf (line, sizeof line, "KEYATTR %s Link: %s", hexgrip2, hexgrip1);
  err = assuan_transact (agent_ctx, line, NULL, NULL, NULL, NULL, NULL, NULL);

 leave:
  return err;
}



/* Call the agent to read the public key part for a given keygrip.
 * Values from FROMCARD:
 *   0 - Standard
 *   1 - The key is read from the current card
 *       via the agent and a stub file is created.
 */
gpg_error_t
agent_readkey (ctrl_t ctrl, int fromcard, const char *hexkeygrip,
               unsigned char **r_pubkey)
{
  gpg_error_t err;
  membuf_t data;
  size_t len;
  unsigned char *buf;
  char line[ASSUAN_LINELENGTH];
  struct default_inq_parm_s dfltparm;

  memset (&dfltparm, 0, sizeof dfltparm);
  dfltparm.ctrl = ctrl;

  *r_pubkey = NULL;
  err = start_agent (ctrl, 0);
  if (err)
    return err;
  dfltparm.ctx = agent_ctx;

  err = assuan_transact (agent_ctx, "RESET",NULL, NULL, NULL, NULL, NULL, NULL);
  if (err)
    return err;

  if (fromcard)
    snprintf (line, DIM(line), "READKEY --card -- %s", hexkeygrip);
  else
    snprintf (line, DIM(line), "READKEY -- %s", hexkeygrip);

  init_membuf (&data, 1024);
  err = assuan_transact (agent_ctx, line,
                         put_membuf_cb, &data,
                         default_inq_cb, &dfltparm,
                         NULL, NULL);
  if (err)
    {
      xfree (get_membuf (&data, &len));
      return err;
    }
  buf = get_membuf (&data, &len);
  if (!buf)
    return gpg_error_from_syserror ();
  if (!gcry_sexp_canon_len (buf, len, NULL, NULL))
    {
      xfree (buf);
      return gpg_error (GPG_ERR_INV_SEXP);
    }
  *r_pubkey = buf;
  return 0;
}



/* Call the agent to do a sign operation using the key identified by
   the hex string KEYGRIP.  DESC is a description of the key to be
   displayed if the agent needs to ask for the PIN.  DIGEST and
   DIGESTLEN is the hash value to sign and DIGESTALGO the algorithm id
   used to compute the digest.  If CACHE_NONCE is used the agent is
   advised to first try a passphrase associated with that nonce. */
gpg_error_t
agent_pksign (ctrl_t ctrl, const char *cache_nonce,
              const char *keygrip, const char *desc,
              u32 *keyid, u32 *mainkeyid, int pubkey_algo,
              unsigned char *digest, size_t digestlen, int digestalgo,
              gcry_sexp_t *r_sigval)
{
  gpg_error_t err;
  char line[ASSUAN_LINELENGTH];
  membuf_t data;
  struct default_inq_parm_s dfltparm;

  memset (&dfltparm, 0, sizeof dfltparm);
  dfltparm.ctrl = ctrl;
  dfltparm.keyinfo.keyid       = keyid;
  dfltparm.keyinfo.mainkeyid   = mainkeyid;
  dfltparm.keyinfo.pubkey_algo = pubkey_algo;

  *r_sigval = NULL;
  err = start_agent (ctrl, 0);
  if (err)
    return err;
  dfltparm.ctx = agent_ctx;

  if (digestlen*2 + 50 > DIM(line))
    return gpg_error (GPG_ERR_GENERAL);

  err = assuan_transact (agent_ctx, "RESET",
                         NULL, NULL, NULL, NULL, NULL, NULL);
  if (err)
    return err;

  snprintf (line, DIM(line), "SIGKEY %s", keygrip);
  err = assuan_transact (agent_ctx, line, NULL, NULL, NULL, NULL, NULL, NULL);
  if (err)
    return err;

  if (desc)
    {
      snprintf (line, DIM(line), "SETKEYDESC %s", desc);
      err = assuan_transact (agent_ctx, line,
                            NULL, NULL, NULL, NULL, NULL, NULL);
      if (err)
        return err;
    }

  snprintf (line, sizeof line, "SETHASH %d ", digestalgo);
  bin2hex (digest, digestlen, line + strlen (line));
  err = assuan_transact (agent_ctx, line, NULL, NULL, NULL, NULL, NULL, NULL);
  if (err)
    return err;

  init_membuf (&data, 1024);

  snprintf (line, sizeof line, "PKSIGN%s%s",
            cache_nonce? " -- ":"",
            cache_nonce? cache_nonce:"");

  if (DBG_CLOCK)
    log_clock ("enter signing");
  err = assuan_transact (agent_ctx, line,
                         put_membuf_cb, &data,
                         default_inq_cb, &dfltparm,
                         NULL, NULL);
  if (DBG_CLOCK)
    log_clock ("leave signing");

  if (err)
    xfree (get_membuf (&data, NULL));
  else
    {
      unsigned char *buf;
      size_t len;

      buf = get_membuf (&data, &len);
      if (!buf)
        err = gpg_error_from_syserror ();
      else
        {
          err = gcry_sexp_sscan (r_sigval, NULL, buf, len);
          xfree (buf);
        }
    }
  return err;
}



/* Handle a CIPHERTEXT inquiry.  Note, we only send the data,
   assuan_transact takes care of flushing and writing the END. */
static gpg_error_t
inq_ciphertext_cb (void *opaque, const char *line)
{
  struct cipher_parm_s *parm = opaque;
  int rc;

  if (has_leading_keyword (line, "CIPHERTEXT"))
    {
      assuan_begin_confidential (parm->ctx);
      rc = assuan_send_data (parm->dflt->ctx,
                             parm->ciphertext, parm->ciphertextlen);
      assuan_end_confidential (parm->ctx);
    }
  else
    rc = default_inq_cb (parm->dflt, line);

  return rc;
}


/* Check whether there is any padding info from the agent.  */
static gpg_error_t
padding_info_cb (void *opaque, const char *line)
{
  int *r_padding = opaque;
  const char *s;

  if ((s=has_leading_keyword (line, "PADDING")))
    {
      *r_padding = atoi (s);
    }

  return 0;
}


/* Call the agent to do a decrypt operation using the key identified
   by the hex string KEYGRIP and the input data S_CIPHERTEXT.  On the
   success the decoded value is stored verbatim at R_BUF and its
   length at R_BUF; the callers needs to release it.  KEYID, MAINKEYID
   and PUBKEY_ALGO are used to construct additional promots or status
   messages.   The padding information is stored at R_PADDING with -1
   for not known.  */
gpg_error_t
agent_pkdecrypt (ctrl_t ctrl, const char *keygrip, const char *desc,
                 u32 *keyid, u32 *mainkeyid, int pubkey_algo,
                 gcry_sexp_t s_ciphertext,
                 unsigned char **r_buf, size_t *r_buflen, int *r_padding)
{
  gpg_error_t err;
  char line[ASSUAN_LINELENGTH];
  membuf_t data;
  size_t n, len;
  char *p, *buf, *endp;
  const char *keygrip2 = NULL;
  struct default_inq_parm_s dfltparm;

  memset (&dfltparm, 0, sizeof dfltparm);
  dfltparm.ctrl = ctrl;
  dfltparm.keyinfo.keyid       = keyid;
  dfltparm.keyinfo.mainkeyid   = mainkeyid;
  dfltparm.keyinfo.pubkey_algo = pubkey_algo;

  if (!keygrip || !s_ciphertext || !r_buf || !r_buflen || !r_padding)
    return gpg_error (GPG_ERR_INV_VALUE);

  *r_buf = NULL;
  *r_padding = -1;

  /* Parse the keygrip in case of a dual algo.  */
  keygrip2 = strchr (keygrip, ',');
  if (!keygrip2)
    keygrip2 = keygrip + strlen (keygrip);
  if (keygrip2 - keygrip != 40)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (*keygrip2)
    {
      keygrip2++;
      if (strlen (keygrip2) != 40)
        return gpg_error (GPG_ERR_INV_VALUE);
    }


  err = start_agent (ctrl, 0);
  if (err)
    return err;
  dfltparm.ctx = agent_ctx;

  err = assuan_transact (agent_ctx, "RESET",
                         NULL, NULL, NULL, NULL, NULL, NULL);
  if (err)
    return err;

  snprintf (line, sizeof line, "SETKEY %.40s", keygrip);
  err = assuan_transact (agent_ctx, line, NULL, NULL, NULL, NULL, NULL, NULL);
  if (err)
    return err;

  if (*keygrip2)
    {
      snprintf (line, sizeof line, "SETKEY --another %.40s", keygrip2);
      err = assuan_transact (agent_ctx, line, NULL, NULL,NULL,NULL,NULL,NULL);
      if (err)
        return err;
    }

  if (desc)
    {
      snprintf (line, DIM(line), "SETKEYDESC %s", desc);
      err = assuan_transact (agent_ctx, line,
                            NULL, NULL, NULL, NULL, NULL, NULL);
      if (err)
        return err;
    }

  init_membuf_secure (&data, 1024);
  {
    struct cipher_parm_s parm;

    parm.dflt = &dfltparm;
    parm.ctx = agent_ctx;
    err = make_canon_sexp (s_ciphertext, &parm.ciphertext, &parm.ciphertextlen);
    if (err)
      return err;
    err = assuan_transact (agent_ctx,
                           *keygrip2? "PKDECRYPT --kem=PQC-PGP":"PKDECRYPT",
                           put_membuf_cb, &data,
                           inq_ciphertext_cb, &parm,
                           padding_info_cb, r_padding);
    xfree (parm.ciphertext);
  }
  if (err)
    {
      xfree (get_membuf (&data, &len));
      return err;
    }

  buf = get_membuf (&data, &len);
  if (!buf)
    return gpg_error_from_syserror ();

  if (len == 0 || *buf != '(')
    {
      xfree (buf);
      return gpg_error (GPG_ERR_INV_SEXP);
    }

  if (len < 12 || memcmp (buf, "(5:value", 8) ) /* "(5:valueN:D)" */
    {
      xfree (buf);
      return gpg_error (GPG_ERR_INV_SEXP);
    }
  while (buf[len-1] == 0)
    len--;
  if (buf[len-1] != ')')
    return gpg_error (GPG_ERR_INV_SEXP);
  len--; /* Drop the final close-paren. */
  p = buf + 8; /* Skip leading parenthesis and the value tag. */
  len -= 8;   /* Count only the data of the second part. */

  n = strtoul (p, &endp, 10);
  if (!n || *endp != ':')
    {
      xfree (buf);
      return gpg_error (GPG_ERR_INV_SEXP);
    }
  endp++;
  if (endp-p+n > len)
    {
      xfree (buf);
      return gpg_error (GPG_ERR_INV_SEXP); /* Oops: Inconsistent S-Exp. */
    }

  memmove (buf, endp, n);

  *r_buflen = n;
  *r_buf = buf;
  return 0;
}



/* Retrieve a key encryption key from the agent.  With FOREXPORT true
   the key shall be used for export, with false for import.  On success
   the new key is stored at R_KEY and its length at R_KEKLEN.  */
gpg_error_t
agent_keywrap_key (ctrl_t ctrl, int forexport, void **r_kek, size_t *r_keklen)
{
  gpg_error_t err;
  membuf_t data;
  size_t len;
  unsigned char *buf;
  char line[ASSUAN_LINELENGTH];
  struct default_inq_parm_s dfltparm;

  memset (&dfltparm, 0, sizeof dfltparm);
  dfltparm.ctrl = ctrl;

  *r_kek = NULL;
  err = start_agent (ctrl, 0);
  if (err)
    return err;
  dfltparm.ctx = agent_ctx;

  snprintf (line, DIM(line), "KEYWRAP_KEY %s",
            forexport? "--export":"--import");

  init_membuf_secure (&data, 64);
  err = assuan_transact (agent_ctx, line,
                         put_membuf_cb, &data,
                         default_inq_cb, &dfltparm,
                         NULL, NULL);
  if (err)
    {
      xfree (get_membuf (&data, &len));
      return err;
    }
  buf = get_membuf (&data, &len);
  if (!buf)
    return gpg_error_from_syserror ();
  *r_kek = buf;
  *r_keklen = len;
  return 0;
}



/* Handle the inquiry for an IMPORT_KEY command.  */
static gpg_error_t
inq_import_key_parms (void *opaque, const char *line)
{
  struct import_key_parm_s *parm = opaque;
  gpg_error_t err;

  if (has_leading_keyword (line, "KEYDATA"))
    {
      err = assuan_send_data (parm->dflt->ctx, parm->key, parm->keylen);
    }
  else
    err = default_inq_cb (parm->dflt, line);

  return err;
}


/* Call the agent to import a key into the agent.  */
gpg_error_t
agent_import_key (ctrl_t ctrl, const char *desc, char **cache_nonce_addr,
                  const void *key, size_t keylen, int unattended, int force,
		  u32 *keyid, u32 *mainkeyid, int pubkey_algo, u32 timestamp)
{
  gpg_error_t err;
  struct import_key_parm_s parm;
  struct cache_nonce_parm_s cn_parm;
  char timestamparg[16 + 16];  /* The 2nd 16 is sizeof(gnupg_isotime_t) */
  char line[ASSUAN_LINELENGTH];
  struct default_inq_parm_s dfltparm;

  memset (&dfltparm, 0, sizeof dfltparm);
  dfltparm.ctrl = ctrl;
  dfltparm.keyinfo.keyid       = keyid;
  dfltparm.keyinfo.mainkeyid   = mainkeyid;
  dfltparm.keyinfo.pubkey_algo = pubkey_algo;

  err = start_agent (ctrl, 0);
  if (err)
    return err;
  dfltparm.ctx = agent_ctx;

  /* Do not use our cache of secret keygrips anymore - this command
   * would otherwise requiring to update that cache.  */
  if (ctrl && ctrl->secret_keygrips)
    {
      xfree (ctrl->secret_keygrips);
      ctrl->secret_keygrips = 0;
    }

  if (timestamp)
    {
      strcpy (timestamparg, " --timestamp=");
      epoch2isotime (timestamparg+13, timestamp);
    }
  else
    *timestamparg = 0;

  if (desc)
    {
      snprintf (line, DIM(line), "SETKEYDESC %s", desc);
      err = assuan_transact (agent_ctx, line,
                            NULL, NULL, NULL, NULL, NULL, NULL);
      if (err)
        return err;
    }

  parm.dflt   = &dfltparm;
  parm.key    = key;
  parm.keylen = keylen;

  snprintf (line, sizeof line, "IMPORT_KEY%s%s%s%s%s",
            *timestamparg? timestamparg : "",
            unattended? " --unattended":"",
            force? " --force":"",
            cache_nonce_addr && *cache_nonce_addr? " ":"",
            cache_nonce_addr && *cache_nonce_addr? *cache_nonce_addr:"");
  cn_parm.cache_nonce_addr = cache_nonce_addr;
  cn_parm.passwd_nonce_addr = NULL;
  err = assuan_transact (agent_ctx, line,
                         NULL, NULL,
                         inq_import_key_parms, &parm,
                         cache_nonce_status_cb, &cn_parm);
  return err;
}



/* Receive a secret key from the agent.  HEXKEYGRIP is the hexified
   keygrip, DESC a prompt to be displayed with the agent's passphrase
   question (needs to be plus+percent escaped).  if OPENPGP_PROTECTED
   is not zero, ensure that the key material is returned in RFC
   4880-compatible passphrased-protected form; if instead MODE1003 is
   not zero the raw gpg-agent private key format is requested (either
   protected or unprotected).  If CACHE_NONCE_ADDR is not NULL the
   agent is advised to first try a passphrase associated with that
   nonce.  On success the key is stored as a canonical S-expression at
   R_RESULT and R_RESULTLEN.  */
gpg_error_t
agent_export_key (ctrl_t ctrl, const char *hexkeygrip, const char *desc,
                  int openpgp_protected, int mode1003, char **cache_nonce_addr,
                  unsigned char **r_result, size_t *r_resultlen,
		  u32 *keyid, u32 *mainkeyid, int pubkey_algo)
{
  gpg_error_t err;
  struct cache_nonce_parm_s cn_parm;
  membuf_t data;
  size_t len;
  unsigned char *buf;
  char line[ASSUAN_LINELENGTH];
  struct default_inq_parm_s dfltparm;

  memset (&dfltparm, 0, sizeof dfltparm);
  dfltparm.ctrl = ctrl;
  dfltparm.keyinfo.keyid       = keyid;
  dfltparm.keyinfo.mainkeyid   = mainkeyid;
  dfltparm.keyinfo.pubkey_algo = pubkey_algo;

  *r_result = NULL;

  err = start_agent (ctrl, 0);
  if (err)
    return err;
  dfltparm.ctx = agent_ctx;

  /* Check that the gpg-agent supports the --mode1003 option.  */
  if (mode1003 && assuan_transact (agent_ctx,
                                   "GETINFO cmd_has_option EXPORT_KEY mode1003",
                                   NULL, NULL, NULL, NULL, NULL, NULL))
    return gpg_error (GPG_ERR_NOT_SUPPORTED);

  if (desc)
    {
      snprintf (line, DIM(line), "SETKEYDESC %s", desc);
      err = assuan_transact (agent_ctx, line,
                             NULL, NULL, NULL, NULL, NULL, NULL);
      if (err)
        return err;
    }

  snprintf (line, DIM(line), "EXPORT_KEY %s%s%s %s",
            mode1003? "--mode1003" : openpgp_protected ? "--openpgp ":"",
            cache_nonce_addr && *cache_nonce_addr? "--cache-nonce=":"",
            cache_nonce_addr && *cache_nonce_addr? *cache_nonce_addr:"",
            hexkeygrip);

  init_membuf_secure (&data, 1024);
  cn_parm.cache_nonce_addr = cache_nonce_addr;
  cn_parm.passwd_nonce_addr = NULL;
  err = assuan_transact (agent_ctx, line,
                         put_membuf_cb, &data,
                         default_inq_cb, &dfltparm,
                         cache_nonce_status_cb, &cn_parm);
  if (err)
    {
      xfree (get_membuf (&data, &len));
      return err;
    }
  buf = get_membuf (&data, &len);
  if (!buf)
    return gpg_error_from_syserror ();
  *r_result = buf;
  *r_resultlen = len;
  return 0;
}


/* Status callback for handling confirmation.  */
static gpg_error_t
confirm_status_cb (void *opaque, const char *line)
{
  struct confirm_parm_s *parm = opaque;
  const char *s;

  if ((s = has_leading_keyword (line, "SETDESC")))
    {
      xfree (parm->desc);
      parm->desc = unescape_status_string (s);
    }
  else if ((s = has_leading_keyword (line, "SETOK")))
    {
      xfree (parm->ok);
      parm->ok = unescape_status_string (s);
    }
  else if ((s = has_leading_keyword (line, "SETNOTOK")))
    {
      xfree (parm->notok);
      parm->notok = unescape_status_string (s);
    }

  return 0;
}

/* Ask the agent to delete the key identified by HEXKEYGRIP.  If DESC
   is not NULL, display DESC instead of the default description
   message.  If FORCE is true the agent is advised not to ask for
   confirmation. */
gpg_error_t
agent_delete_key (ctrl_t ctrl, const char *hexkeygrip, const char *desc,
                  int force)
{
  gpg_error_t err;
  char line[ASSUAN_LINELENGTH];
  struct default_inq_parm_s dfltparm;
  struct confirm_parm_s confirm_parm;

  memset (&confirm_parm, 0, sizeof confirm_parm);
  memset (&dfltparm, 0, sizeof dfltparm);
  dfltparm.ctrl = ctrl;
  dfltparm.confirm = &confirm_parm;

  err = start_agent (ctrl, 0);
  if (err)
    return err;
  dfltparm.ctx = agent_ctx;

  if (!hexkeygrip || strlen (hexkeygrip) != 40)
    return gpg_error (GPG_ERR_INV_VALUE);

  if (desc)
    {
      snprintf (line, DIM(line), "SETKEYDESC %s", desc);
      err = assuan_transact (agent_ctx, line,
                             NULL, NULL, NULL, NULL, NULL, NULL);
      if (err)
        return err;
    }

  /* FIXME: Shall we add support to DELETE_KEY for dual keys?  */
  snprintf (line, DIM(line), "DELETE_KEY%s %s",
            force? " --force":"", hexkeygrip);
  err = assuan_transact (agent_ctx, line, NULL, NULL,
                         default_inq_cb, &dfltparm,
                         confirm_status_cb, &confirm_parm);
  xfree (confirm_parm.desc);
  xfree (confirm_parm.ok);
  xfree (confirm_parm.notok);
  return err;
}



/* Ask the agent to change the passphrase of the key identified by
 * HEXKEYGRIP.  If DESC is not NULL, display DESC instead of the
 * default description message.  If CACHE_NONCE_ADDR is not NULL the
 * agent is advised to first try a passphrase associated with that
 * nonce.  If PASSWD_NONCE_ADDR is not NULL the agent will try to use
 * the passphrase associated with that nonce for the new passphrase.
 * If VERIFY is true the passphrase is only verified.  */
gpg_error_t
agent_passwd (ctrl_t ctrl, const char *hexkeygrip, const char *desc, int verify,
              char **cache_nonce_addr, char **passwd_nonce_addr)
{
  gpg_error_t err;
  struct cache_nonce_parm_s cn_parm;
  char line[ASSUAN_LINELENGTH];
  struct default_inq_parm_s dfltparm;

  memset (&dfltparm, 0, sizeof dfltparm);
  dfltparm.ctrl = ctrl;

  err = start_agent (ctrl, 0);
  if (err)
    return err;
  dfltparm.ctx = agent_ctx;

  if (!hexkeygrip || strlen (hexkeygrip) != 40)
    return gpg_error (GPG_ERR_INV_VALUE);

  if (desc)
    {
      snprintf (line, DIM(line), "SETKEYDESC %s", desc);
      err = assuan_transact (agent_ctx, line,
                             NULL, NULL, NULL, NULL, NULL, NULL);
      if (err)
        return err;
    }

  if (verify)
    snprintf (line, DIM(line), "PASSWD %s%s --verify %s",
              cache_nonce_addr && *cache_nonce_addr? "--cache-nonce=":"",
              cache_nonce_addr && *cache_nonce_addr? *cache_nonce_addr:"",
              hexkeygrip);
  else
    snprintf (line, DIM(line), "PASSWD %s%s %s%s %s",
              cache_nonce_addr && *cache_nonce_addr? "--cache-nonce=":"",
              cache_nonce_addr && *cache_nonce_addr? *cache_nonce_addr:"",
              passwd_nonce_addr && *passwd_nonce_addr? "--passwd-nonce=":"",
              passwd_nonce_addr && *passwd_nonce_addr? *passwd_nonce_addr:"",
              hexkeygrip);
  cn_parm.cache_nonce_addr = cache_nonce_addr;
  cn_parm.passwd_nonce_addr = passwd_nonce_addr;
  err = assuan_transact (agent_ctx, line, NULL, NULL,
                         default_inq_cb, &dfltparm,
                         cache_nonce_status_cb, &cn_parm);
  return err;
}


/* Enable or disable the ephemeral mode.  In ephemeral mode keys are
 * created,searched and used in a per-session key store and not in the
 * on-disk file.  Set ENABLE to 1 to enable this mode, to 0 to disable
 * this mode and to -1 to only query the current mode.  If R_PREVIOUS
 * is given the previously used state of the ephemeral mode is stored
 * at that address.  */
gpg_error_t
agent_set_ephemeral_mode (ctrl_t ctrl, int enable, int *r_previous)
{
  gpg_error_t err;

  err = start_agent (ctrl, 0);
  if (err)
    goto leave;

  if (r_previous)
    {
      err = assuan_transact (agent_ctx, "GETINFO ephemeral",
                             NULL, NULL, NULL, NULL, NULL, NULL);
      if (!err)
        *r_previous = 1;
      else if (gpg_err_code (err) == GPG_ERR_FALSE)
        *r_previous = 0;
      else
        goto leave;
    }

  /* Skip setting if we are only querying or if the mode is already set. */
  if (enable == -1 || (r_previous && !!*r_previous == !!enable))
    err = 0;
  else
    err = assuan_transact (agent_ctx,
                           enable? "OPTION ephemeral=1" : "OPTION ephemeral=0",
                           NULL, NULL, NULL, NULL, NULL, NULL);
 leave:
  return err;
}


/* Return the version reported by gpg-agent.  */
gpg_error_t
agent_get_version (ctrl_t ctrl, char **r_version)
{
  gpg_error_t err;

  err = start_agent (ctrl, 0);
  if (err)
    return err;

  err = get_assuan_server_version (agent_ctx, 0, r_version);
  return err;
}
