/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libcrypto-compat's error queue, with OpenSSL's interface (ERR_*): each
 * thread has its own queue of error codes; a failing function of either
 * library adds a code; the program reads them oldest first.
 *
 * A code packs a library number and a reason number as OpenSSL 3 does,
 * so ERR_GET_LIB and ERR_GET_REASON read it; the reason numbers used are
 * OpenSSL's for the same conditions (SSL_R_* in <compat/ssl/ssl.h>).  The
 * text of ERR_error_string_n has OpenSSL 3's layout,
 * "error:<code in 8 hex digits>:<library>::<reason>", with libcrypto-
 * compat's own wording of the reason.
 */

#ifndef KERN_COMPAT_CRYPTO_ERR_H
#define KERN_COMPAT_CRYPTO_ERR_H

#include <compat/crypto/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The library numbers and the packing of a code.
 * Values referenced: OpenSSL 3.5.8 include/openssl/err.h.in (ERR_LIB_*,
 * ERR_LIB_OFFSET, ERR_LIB_MASK, ERR_REASON_MASK).
 */
#define ERR_LIB_NONE		1
#define ERR_LIB_SYS		2
#define ERR_LIB_EVP		6
#define ERR_LIB_PEM		9
#define ERR_LIB_X509		11
#define ERR_LIB_ASN1		13
#define ERR_LIB_CRYPTO		15
#define ERR_LIB_SSL		20
#define ERR_LIB_RAND		36
#define ERR_LIB_OFFSET		23L
#define ERR_LIB_MASK		0xFF
#define ERR_REASON_MASK		0X7FFFFF

/* The library of a code (as OpenSSL's inline function; codes here never carry the system flag). */
#define ERR_GET_LIB(code)	((int)(((unsigned long)(code) >> ERR_LIB_OFFSET) & ERR_LIB_MASK))

/* The reason of a code. */
#define ERR_GET_REASON(code)	((int)((unsigned long)(code) & ERR_REASON_MASK))

/*
 * Removes and returns the oldest code of the calling thread's queue, or 0
 * when the queue is empty.
 */
unsigned long ERR_get_error(void);

/* Returns the oldest code without removing it, or 0. */
unsigned long ERR_peek_error(void);

/*
 * Writes the text of code into buffer (length bytes, NUL-terminated,
 * cut to fit; nothing when length is 0).  An unknown code gives a text
 * with its numbers.
 */
void ERR_error_string_n(unsigned long code, char *buffer, size_t length);

/* Empties the calling thread's queue. */
void ERR_clear_error(void);

#ifdef __cplusplus
}
#endif

#endif
