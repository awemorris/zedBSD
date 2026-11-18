/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The textual encoding of certificates (PEM, RFC 7468 (April 2015)) and
 * the base64 it uses (RFC 4648 (October 2006) section 4).
 *
 * A CA bundle (the ca-certificates package's cert.pem) is a sequence of
 * "-----BEGIN CERTIFICATE-----" ... "-----END CERTIFICATE-----" blocks
 * with free text between them; only CERTIFICATE blocks are read (RFC
 * 7468 5.1), and "TRUSTED CERTIFICATE" blocks (OpenSSL's own form, not
 * in RFC 7468) are skipped like any other label.  The parser follows the
 * lax grammar of RFC 7468 section 3 (laxtextualmsg): whitespace and line
 * ends of either kind inside the base64 are skipped, explanatory text
 * before a block is ignored.  The input is untrusted.
 */

#ifndef LIBCRYPTO_COMPAT_CRYC_PEM_H
#define LIBCRYPTO_COMPAT_CRYC_PEM_H

#include "cryc.h"

/*
 * Decodes base64 (RFC 4648 section 4, the standard alphabet of Table 1)
 * from size characters at text into out.
 *
 * Whitespace (space, tab, CR, LF) between characters is skipped; any
 * other character outside the alphabet and "=" is EINVAL; padding is
 * required to make the count of significant characters a multiple of 4
 * (RFC 7468 section 3 requires it); bits after the last full byte must be
 * zero (RFC 4648 3.5; EINVAL otherwise).  out has room for room bytes;
 * *written receives the bytes decoded.
 *
 * Returns 0, EINVAL, or ENOBUFS when room is too small (*written is then
 * 0; out may have been partly written).
 */
int cryc_base64_decode(const char *text, size_t size, uint8_t *out, size_t room, size_t *written);

/*
 * Finds the next CERTIFICATE block in a PEM text, starting at *position,
 * and decodes it.
 *
 * text is size bytes (not necessarily NUL-terminated).  On success
 * *der receives a malloc'd buffer of *der_size bytes (the caller frees
 * it) and *position moves past the block's END line.  When no further
 * CERTIFICATE block exists, returns ENOENT with *position at size.  A
 * BEGIN line without a matching END line with the same label, or base64
 * that does not decode, returns EINVAL with *position moved past the
 * BEGIN line (so that a caller loading a bundle may skip one broken
 * block and continue).  ENOMEM when the buffer cannot be allocated.
 *
 * Returns 0, ENOENT, EINVAL or ENOMEM.  On every error *der is NULL.
 */
int cryc_pem_next_certificate(const char *text, size_t size, size_t *position, uint8_t **der, size_t *der_size);

#endif
