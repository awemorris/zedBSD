/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The error queue behind the public ERR_* functions (<compat/crypto/
 * err.h>): one queue per thread, kept with cryc_os_thread_value, shared
 * by libcrypto-compat and libssl-compat (libssl-compat calls
 * cryc_err_put through libcrypto-compat's private interface, so a program
 * sees one queue, as with OpenSSL).
 *
 * A code is ERR_PACK-shaped: (library & 0xff) << 23 | (reason &
 * 0x7fffff) (the layout of OpenSSL 3's err.h; values referenced: OpenSSL
 * 3.5.8 include/openssl/err.h.in, ERR_LIB_OFFSET and ERR_REASON_MASK).
 * The reasons are OpenSSL's numbers for the same conditions (below, cited
 * at their block), so a program that compares ERR_GET_REASON with an
 * SSL_R_ or X509_R_ constant reads the same.  The text of each reason is
 * libcrypto-compat's own wording.
 *
 * The queue holds the CRYC_ERR_QUEUE newest codes; an older one is
 * dropped when it is full.  When the queue cannot be allocated (no
 * memory), codes are dropped silently: error reporting never fails a
 * caller.
 */

#ifndef LIBCRYPTO_COMPAT_CRYC_ERR_H
#define LIBCRYPTO_COMPAT_CRYC_ERR_H

#include "cryc.h"

/* The codes a thread's queue keeps. */
#define CRYC_ERR_QUEUE		16U

/*
 * The library numbers.
 * Values referenced: OpenSSL 3.5.8 include/openssl/err.h.in (ERR_LIB_SYS,
 * ERR_LIB_EVP, ERR_LIB_X509, ERR_LIB_ASN1, ERR_LIB_CRYPTO, ERR_LIB_SSL,
 * ERR_LIB_RAND).
 */
#define CRYC_LIB_SYS		2U
#define CRYC_LIB_EVP		6U
#define CRYC_LIB_X509		11U
#define CRYC_LIB_ASN1		13U
#define CRYC_LIB_CRYPTO		15U
#define CRYC_LIB_SSL		20U
#define CRYC_LIB_RAND		36U

/*
 * The common reasons (any library), with their flag bits.
 * Values referenced: OpenSSL 3.5.8 include/openssl/err.h.in
 * (ERR_R_MALLOC_FAILURE, ERR_R_INTERNAL_ERROR, ERR_R_PASSED_NULL_PARAMETER,
 * ERR_R_PASSED_INVALID_ARGUMENT, ERR_R_UNSUPPORTED, ERR_RFLAG_FATAL,
 * ERR_RFLAG_COMMON).
 */
#define CRYC_R_MALLOC_FAILURE		(256U | 0x000c0000U)
#define CRYC_R_PASSED_NULL_PARAMETER	(258U | 0x000c0000U)
#define CRYC_R_INTERNAL_ERROR		(259U | 0x000c0000U)
#define CRYC_R_PASSED_INVALID_ARGUMENT	(262U | 0x00080000U)
#define CRYC_R_UNSUPPORTED		(268U | 0x00080000U)

/*
 * The reasons of the ASN.1, X.509 and EVP libraries used.
 * Values referenced: OpenSSL 3.5.8 include/openssl/asn1err.h
 * (ASN1_R_WRONG_TAG, ASN1_R_HEADER_TOO_LONG), x509err.h
 * (X509_R_INVALID_ATTRIBUTES), evperr.h (EVP_R_INVALID_KEY,
 * EVP_R_INVALID_KEY_LENGTH, EVP_R_OPERATION_NOT_INITIALIZED,
 * EVP_R_BUFFER_TOO_SMALL, EVP_R_UNSUPPORTED_ALGORITHM,
 * EVP_R_DIFFERENT_KEY_TYPES).
 */
#define CRYC_R_ASN1_WRONG_TAG			168U
#define CRYC_R_ASN1_HEADER_TOO_LONG		123U
#define CRYC_R_X509_INVALID_ATTRIBUTES		138U
#define CRYC_R_EVP_DIFFERENT_KEY_TYPES		101U
#define CRYC_R_EVP_INVALID_KEY_LENGTH		130U
#define CRYC_R_EVP_OPERATION_NOT_INITIALIZED	151U
#define CRYC_R_EVP_BUFFER_TOO_SMALL		155U
#define CRYC_R_EVP_UNSUPPORTED_ALGORITHM	156U
#define CRYC_R_EVP_INVALID_KEY			163U

/* Packs a library and a reason into a code. */
unsigned long cryc_err_pack(unsigned library, unsigned reason);

/* Adds a code to the calling thread's queue (dropping the oldest when full; dropped silently without memory). */
void cryc_err_put(unsigned library, unsigned reason);

/* Removes and returns the oldest code, or 0 when empty (ERR_get_error). */
unsigned long cryc_err_get(void);

/* Returns the oldest code without removing it, or 0 (ERR_peek_error). */
unsigned long cryc_err_peek(void);

/* Empties the calling thread's queue (ERR_clear_error). */
void cryc_err_clear(void);

/*
 * Writes "error:%08lX:<library>::<reason>" for code into buffer (length
 * bytes, NUL-terminated, cut to fit; nothing when length is 0).  The
 * library names are OpenSSL's ("SSL routines", "x509 certificate
 * routines", ...; own wording allowed); a reason without a text is written
 * as "reason(<number>)" (ERR_error_string_n).
 */
void cryc_err_string(unsigned long code, char *buffer, size_t length);

/*
 * Registers libssl-compat's reason texts: table is count pairs of a
 * reason number and its text, constant for the life of the process.
 * Called once by libssl-compat's initialization (a constructor of its
 * shared object, or its first SSL_CTX_new); a second call with the same
 * table is a no-op.  Thread safe.
 */
void cryc_err_register_ssl(const unsigned *reasons, const char *const *texts, size_t count);

#endif
