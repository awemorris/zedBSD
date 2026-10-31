/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The structures behind libssl-compat's public object types (SSL,
 * SSL_CTX, SSL_METHOD of <compat/ssl/ssl.h>), shared by its api-ssl.c
 * and its unit tests; not installed.
 *
 * An SSL_CTX is set up by one thread and then may be read by several
 * (SSL_new copies its settings under lock); an SSL belongs to one thread
 * at a time.  Reference counts change only with cryc_os_atomic_add.
 *
 * Locks: an SSL_CTX's lock may be held while its store's lock is taken,
 * never the reverse; no file is read and no user callback is called while
 * either is held (SSL_CTX_load_verify_locations reads the file first).
 *
 * An SSL never connected (started 0) is freed without touching conn;
 * SSL_shutdown on it returns -1 (SSL_R_SHUTDOWN_WHILE_IN_INIT).
 */

#ifndef LIBSSL_COMPAT_SSLC_API_H
#define LIBSSL_COMPAT_SSLC_API_H

#include <compat/ssl/ssl.h>

#include "../libcrypto-compat/cryc-api.h"
#include "sslc-conn.h"

/* The client method: only its role. */
struct ssl_method_st {
	int client;
};

/*
 * A context.  lock guards the settings below while SSLs are made from
 * other threads.  min_version and max_version are 0 (unset: TLS 1.2 to
 * 1.3) or a version; options the SSL_OP_ bits; verify_mode and
 * verify_callback as set; store holds one reference.
 */
struct ssl_ctx_st {
	volatile long references;
	struct cryc_os_mutex lock;
	uint16_t min_version;
	uint16_t max_version;
	uint64_t options;
	int verify_mode;
	SSL_verify_cb verify_callback;
	X509_STORE *store;
};

/*
 * A connection object.
 *
 * ctx holds one reference.  descriptor is SSL_set_fd's value (-1 before).
 * param the verification parameters (SSL_get0_param).  server_name the
 * SNI name (normalized, empty when none).  conn the connection, made at
 * the first SSL_connect, SSL_read or SSL_write (started is 1 then).
 * last_error is the SSL_ERROR_ value SSL_get_error gives for a failing
 * call, set by every public call; last_errno the errno saved for
 * SSL_ERROR_SYSCALL.  verify_result the X509_V_ code of the handshake.
 * store_ctx is what the verify callback is given.
 */
struct ssl_st {
	SSL_CTX *ctx;
	intptr_t descriptor;
	X509_VERIFY_PARAM param;
	char server_name[CRYC_NAME_HOST_MAX + 1U];
	struct sslc_conn conn;
	int started;
	int last_error;
	int last_errno;
	long verify_result;
	X509_STORE_CTX store_ctx;
};

/*
 * The reason texts libssl-compat registers with cryc_err_register_ssl
 * (constant tables in api-ssl.c, own wording).
 */
extern const unsigned sslc_reason_numbers[];
extern const char *const sslc_reason_texts[];
extern const size_t sslc_reason_count;

#endif
