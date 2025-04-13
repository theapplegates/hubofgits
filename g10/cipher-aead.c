/* cipher-aead.c - Enciphering filter for AEAD modes
 * Copyright (C) 2018 Werner koch
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
 * SPDX-License-Identifier: GPL-3.0+
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "gpg.h"
#include "../common/status.h"
#include "../common/iobuf.h"
#include "../common/util.h"
#include "filter.h"
#include "packet.h"
#include "options.h"
#include "main.h"


/* The size of the buffer we allocate to encrypt the data.  This must
 * be a multiple of the OCB blocksize (16 byte).  */
#define AEAD_ENC_BUFFER_SIZE (64*1024)


/* Wrapper around iobuf_write to make sure that a proper error code is
 * always returned.  */
static gpg_error_t
my_iobuf_write (iobuf_t a, const void *buffer, size_t buflen)
{
  if (iobuf_write (a, buffer, buflen))
    {
      gpg_error_t err = iobuf_error (a);
      if (!err || !gpg_err_code (err)) /* (The latter should never happen) */
        err = gpg_error (GPG_ERR_EIO);
      return err;
    }
  return 0;
}


/* Set the nonce and the additional data for the current chunk.  If
 * FINAL is set the final AEAD chunk is processed.  This also reset
 * the encryption machinery so that the handle can be used for a new
 * chunk.  */
static gpg_error_t
set_nonce_and_ad (cipher_filter_context_t *cfx, int final)
{
  gpg_error_t err;
  unsigned char nonce[16];
  unsigned char ad[21];
  int i;

  switch (cfx->dek->use_aead)
    {
    case AEAD_ALGO_OCB:
      memcpy (nonce, cfx->startiv, 15);
      i = 7;
      break;

    case AEAD_ALGO_EAX:
      memcpy (nonce, cfx->startiv, 16);
      i = 8;
      break;

    default:
      BUG ();
    }

  nonce[i++] ^= cfx->chunkindex >> 56;
  nonce[i++] ^= cfx->chunkindex >> 48;
  nonce[i++] ^= cfx->chunkindex >> 40;
  nonce[i++] ^= cfx->chunkindex >> 32;
  nonce[i++] ^= cfx->chunkindex >> 24;
  nonce[i++] ^= cfx->chunkindex >> 16;
  nonce[i++] ^= cfx->chunkindex >>  8;
  nonce[i++] ^= cfx->chunkindex;

  if (DBG_CRYPTO)
    log_printhex (nonce, 15, "nonce:");
  err = gcry_cipher_setiv (cfx->cipher_hd, nonce, i);
  if (err)
    return err;

  ad[0] = (0xc0 | PKT_ENCRYPTED_AEAD);
  ad[1] = 1;
  ad[2] = cfx->dek->algo;
  ad[3] = cfx->dek->use_aead;
  ad[4] = cfx->chunkbyte;
  ad[5] = cfx->chunkindex >> 56;
  ad[6] = cfx->chunkindex >> 48;
  ad[7] = cfx->chunkindex >> 40;
  ad[8] = cfx->chunkindex >> 32;
  ad[9] = cfx->chunkindex >> 24;
  ad[10]= cfx->chunkindex >> 16;
  ad[11]= cfx->chunkindex >>  8;
  ad[12]= cfx->chunkindex;
  if (final)
    {
      ad[13] = cfx->total >> 56;
      ad[14] = cfx->total >> 48;
      ad[15] = cfx->total >> 40;
      ad[16] = cfx->total >> 32;
      ad[17] = cfx->total >> 24;
      ad[18] = cfx->total >> 16;
      ad[19] = cfx->total >>  8;
      ad[20] = cfx->total;
    }
  if (DBG_CRYPTO)
    log_printhex (ad, final? 21 : 13, "authdata:");
  return gcry_cipher_authenticate (cfx->cipher_hd, ad, final? 21 : 13);
}


static gpg_error_t
write_header (cipher_filter_context_t *cfx, iobuf_t a)
{
  gpg_error_t err;
  PACKET pkt;
  PKT_encrypted ed;
  unsigned int blocksize;
  unsigned int startivlen;
  enum gcry_cipher_modes ciphermode;

  log_assert (cfx->dek->use_aead);

  blocksize = openpgp_cipher_get_algo_blklen (cfx->dek->algo);
  if (blocksize != 16 )
    log_fatal ("unsupported blocksize %u for AEAD\n", blocksize);

  err = openpgp_aead_algo_info (cfx->dek->use_aead, &ciphermode, &startivlen);
  if (err)
    goto leave;

  log_assert (opt.chunk_size >= 6 && opt.chunk_size <= 62);
  cfx->chunkbyte = opt.chunk_size - 6;
  cfx->chunksize = (uint64_t)1 << (cfx->chunkbyte + 6);
  cfx->chunklen = 0;
  cfx->bufsize = AEAD_ENC_BUFFER_SIZE;
  cfx->buflen = 0;
  cfx->buffer = xtrymalloc (cfx->bufsize);
  if (!cfx->buffer)
    return gpg_error_from_syserror ();

  memset (&ed, 0, sizeof ed);
  ed.new_ctb = 1;  /* (Is anyway required for the packet type).  */
  ed.len = 0; /* fixme: cfx->datalen */
  ed.extralen    = startivlen + 16; /* (16 is the taglen) */
  ed.cipher_algo = cfx->dek->algo;
  ed.aead_algo   = cfx->dek->use_aead;
  ed.chunkbyte   = cfx->chunkbyte;

  init_packet (&pkt);
  pkt.pkttype = PKT_ENCRYPTED_AEAD;
  pkt.pkt.encrypted = &ed;

  if (DBG_FILTER)
    log_debug ("aead packet: len=%lu extralen=%d\n",
               (unsigned long)ed.len, ed.extralen);

  print_cipher_algo_note (cfx->dek->algo);

  if (build_packet( a, &pkt))
    log_bug ("build_packet(ENCRYPTED_AEAD) failed\n");

  log_assert (sizeof cfx->startiv >= startivlen);
  gcry_randomize (cfx->startiv, startivlen, GCRY_STRONG_RANDOM);
  err = my_iobuf_write (a, cfx->startiv, startivlen);
  if (err)
    goto leave;

  err = openpgp_cipher_open (&cfx->cipher_hd,
                             cfx->dek->algo,
                             ciphermode,
                             GCRY_CIPHER_SECURE);
  if (err)
    goto leave;

  if (DBG_CRYPTO)
    log_printhex (cfx->dek->key, cfx->dek->keylen, "thekey:");
  err = gcry_cipher_setkey (cfx->cipher_hd, cfx->dek->key, cfx->dek->keylen);
  if (err)
    return err;

  cfx->wrote_header = 1;

 leave:
  return err;
}


/* Get and write the auth tag to stream A.  */
static gpg_error_t
write_auth_tag (cipher_filter_context_t *cfx, iobuf_t a)
{
  gpg_error_t err;
  char tag[16];

  err = gcry_cipher_gettag (cfx->cipher_hd, tag, 16);
  if (err)
    goto leave;
  err = my_iobuf_write (a, tag, 16);
  if (err)
    goto leave;

 leave:
  if (err)
    log_error ("write_auth_tag failed: %s\n", gpg_strerror (err));
  return err;
}


/* Write the final chunk to stream A.  */
static gpg_error_t
write_final_chunk (cipher_filter_context_t *cfx, iobuf_t a)
{
  gpg_error_t err;
  char dummy[1];

  err = set_nonce_and_ad (cfx, 1);
  if (err)
    goto leave;

  gcry_cipher_final (cfx->cipher_hd);

  /* Encrypt an empty string.  */
  err = gcry_cipher_encrypt (cfx->cipher_hd, dummy, 0, NULL, 0);
  if (err)
    goto leave;

  err = write_auth_tag (cfx, a);

 leave:
  return err;
}


/* The core of the flush sub-function of cipher_filter_aead.   */
static gpg_error_t
do_flush (cipher_filter_context_t *cfx, iobuf_t a, byte *buf, size_t size)
{
  gpg_error_t err = 0;
  int finalize = 0;
  size_t n;

  /* Put the data into a buffer, flush and encrypt as needed.  */
  if (DBG_FILTER)
    log_debug ("flushing %zu bytes (cur buflen=%zu)\n", size, cfx->buflen);
  do
    {
      const unsigned fast_threshold = 512;
      const byte *src_buf = NULL;
      int enc_now = 0;

      if (cfx->buflen + size < cfx->bufsize)
        n = size;
      else
        n = cfx->bufsize - cfx->buflen;

      if (cfx->buflen % fast_threshold != 0)
	{
	  /* Attempt to align cfx->buflen to fast threshold size first. */
	  size_t nalign = fast_threshold - (cfx->buflen % fast_threshold);
	  if (nalign < n)
	    {
	      n = nalign;
	    }
	}
      else if (cfx->buflen == 0 && n >= fast_threshold)
	{
	  /* Handle large input buffers as multiple of cipher blocksize. */
	  n = (n / 16) * 16;
	}

      if (cfx->chunklen + cfx->buflen + n >= cfx->chunksize)
        {
          size_t n1 = cfx->chunksize - (cfx->chunklen + cfx->buflen);
          finalize = 1;
          if (DBG_FILTER)
            log_debug ("chunksize %llu reached;"
                       " cur buflen=%zu using %zu of %zu\n",
                       (unsigned long long)cfx->chunksize, cfx->buflen,
                       n1, n);
          n = n1;
        }

      if (!finalize && cfx->buflen % 16 == 0 && cfx->buflen > 0
	  && size >= fast_threshold)
	{
	  /* If cfx->buffer is aligned and remaining input buffer length
	   * is long, encrypt cfx->buffer inplace now to allow fast path
	   * handling on next loop iteration. */
	  src_buf = cfx->buffer;
	  enc_now = 1;
	  n = 0;
	}
      else if (cfx->buflen == 0 && n >= fast_threshold)
	{
	  /* Fast path for large input buffer. This avoids memcpy and
	   * instead encrypts directly from input to cfx->buffer. */
	  log_assert (n % 16 == 0 || finalize);
	  src_buf = buf;
	  cfx->buflen = n;
	  buf += n;
	  size -= n;
	  enc_now = 1;
	}
      else if (n > 0)
	{
	  memcpy (cfx->buffer + cfx->buflen, buf, n);
	  src_buf = cfx->buffer;
	  cfx->buflen += n;
	  buf  += n;
	  size -= n;
	}

      if (cfx->buflen == cfx->bufsize || enc_now || finalize)
        {
          if (DBG_FILTER)
            log_debug ("encrypting: size=%zu buflen=%zu %s%s n=%zu\n",
                       size, cfx->buflen, finalize?"(finalize)":"",
		       enc_now?"(now)":"", n);

          if (!cfx->chunklen)
            {
              if (DBG_FILTER)
                log_debug ("start encrypting a new chunk\n");
              err = set_nonce_and_ad (cfx, 0);
              if (err)
                goto leave;
            }

          if (finalize)
            gcry_cipher_final (cfx->cipher_hd);
          if (DBG_FILTER)
            {
              if (finalize)
                log_printhex (src_buf, cfx->buflen, "plain(1):");
              else if (cfx->buflen > 32)
                log_printhex (src_buf + cfx->buflen - 32, 32,
                              "plain(last32):");
            }

          /* Take care: even with a buflen of zero an encrypt needs to
           * be called after gcry_cipher_final and before
           * gcry_cipher_gettag - at least with libgcrypt 1.8 and OCB
           * mode.  */
	  err = gcry_cipher_encrypt (cfx->cipher_hd, cfx->buffer,
				     cfx->buflen, src_buf, cfx->buflen);
          if (err)
            goto leave;
          if (finalize && DBG_FILTER)
            log_printhex (cfx->buffer, cfx->buflen, "ciphr(1):");
          err = my_iobuf_write (a, cfx->buffer, cfx->buflen);
          if (err)
            goto leave;
          cfx->chunklen += cfx->buflen;
          cfx->total += cfx->buflen;
          cfx->buflen = 0;

          if (finalize)
            {
              if (DBG_FILTER)
                log_debug ("writing tag: chunklen=%ju total=%ju\n",
                           (uintmax_t)cfx->chunklen, (uintmax_t)cfx->total);
              err = write_auth_tag (cfx, a);
              if (err)
                goto leave;

              cfx->chunkindex++;
              cfx->chunklen = 0;
              finalize = 0;
            }
        }
    }
  while (size);

 leave:
  return err;
}


/* The core of the free sub-function of cipher_filter_aead.   */
static gpg_error_t
do_free (cipher_filter_context_t *cfx, iobuf_t a)
{
  gpg_error_t err = 0;

  if (DBG_FILTER)
    log_debug ("do_free: buflen=%zu\n", cfx->buflen);

  if (cfx->chunklen || cfx->buflen)
    {
      if (DBG_FILTER)
        log_debug ("encrypting last %zu bytes of the last chunk\n",cfx->buflen);

      if (!cfx->chunklen)
        {
          if (DBG_FILTER)
            log_debug ("start encrypting a new chunk\n");
          err = set_nonce_and_ad (cfx, 0);
          if (err)
            goto leave;
        }

      gcry_cipher_final (cfx->cipher_hd);
      err = gcry_cipher_encrypt (cfx->cipher_hd, cfx->buffer, cfx->buflen,
                                 NULL, 0);
      if (err)
        goto leave;
      err = my_iobuf_write (a, cfx->buffer, cfx->buflen);
      if (err)
        goto leave;
      /* log_printhex (cfx->buffer, cfx->buflen, "wrote:"); */
      cfx->chunklen += cfx->buflen;
      cfx->total += cfx->buflen;

      /* Get and write the authentication tag.  */
      if (DBG_FILTER)
        log_debug ("writing tag: chunklen=%ju total=%ju\n",
                   (uintmax_t)cfx->chunklen, (uintmax_t)cfx->total);
      err = write_auth_tag (cfx, a);
      if (err)
        goto leave;
      cfx->chunkindex++;
      cfx->chunklen = 0;
    }

  /* Write the final chunk.  */
  if (DBG_FILTER)
    log_debug ("creating final chunk\n");
  err = write_final_chunk (cfx, a);

 leave:
  xfree (cfx->buffer);
  cfx->buffer = NULL;
  gcry_cipher_close (cfx->cipher_hd);
  cfx->cipher_hd = NULL;
  return err;
}


/*
 * This filter is used to encrypt data with an AEAD algorithm
 */
int
cipher_filter_aead (void *opaque, int control,
                    iobuf_t a, byte *buf, size_t *ret_len)
{
  cipher_filter_context_t *cfx = opaque;
  size_t size = *ret_len;
  int rc = 0;

  if (control == IOBUFCTRL_UNDERFLOW) /* decrypt */
    {
      rc = -1; /* not used */
    }
  else if (control == IOBUFCTRL_FLUSH) /* encrypt */
    {
      if (!cfx->wrote_header && (rc=write_header (cfx, a)))
        ;
      else
        rc = do_flush (cfx, a, buf, size);
    }
  else if (control == IOBUFCTRL_FREE)
    {
      rc = do_free (cfx, a);
    }
  else if (control == IOBUFCTRL_DESC)
    {
      mem2str (buf, "cipher_filter_aead", *ret_len);
    }
  else if (control == IOBUFCTRL_INIT)
    {
      write_status_printf (STATUS_BEGIN_ENCRYPTION, "0 %d %d",
                           cfx->dek->algo, cfx->dek->use_aead);
    }

  return rc;
}
