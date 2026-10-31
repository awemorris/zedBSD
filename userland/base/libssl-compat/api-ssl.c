/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The public SSL_ and SSL_CTX_ functions.
 */

#include "sslc-api.h"

/* The reasons libssl-compat queues, in the order of sslc_reason_texts. */
const unsigned sslc_reason_numbers[] = { 0 };

/* The texts of those reasons, in libssl-compat's own words. */
const char *const sslc_reason_texts[] = { "" };

/* The number of reasons in the two tables. */
const size_t sslc_reason_count = 0;

/*
 * Returns the client method (a constant object).
 */
const SSL_METHOD *
TLS_client_method(void)
{
	/* Not implemented: the failure value. */
	return NULL;
}

/*
 * Makes a context: the client method only (another is NULL with an error);
 * versions TLS 1.2 to 1.3; SSL_VERIFY_NONE; an empty store.
 */
SSL_CTX *
SSL_CTX_new(
	const SSL_METHOD *method)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(method);

	/* Not implemented: the failure value. */
	return NULL;
}

/*
 * Drops one reference to a context (each SSL holds one); NULL is ignored.
 */
void
SSL_CTX_free(
	SSL_CTX *ctx)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
}

/*
 * Sets the verify mode (SSL_VERIFY_NONE or SSL_VERIFY_PEER; other bits
 * ignored) and callback (may be NULL) of SSLs made after.
 */
void
SSL_CTX_set_verify(
	SSL_CTX *ctx,
	int mode,
	SSL_verify_cb callback)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(mode);
	UNUSED_PARAMETER(callback);
}

/*
 * Adds the CERTIFICATE blocks of the PEM file at file (UTF-8) to the
 * store.
 */
int
SSL_CTX_load_verify_locations(
	SSL_CTX *ctx,
	const char *file,
	const char *path)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(file);
	UNUSED_PARAMETER(path);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Adds the system's roots: /etc/ssl/cert.pem on zedBSD; on Windows the
 * ROOT stores (minus Disallowed) and cert.pem beside the program's .exe,
 * and the Disallowed certificates are refused in every chain from then on.
 */
int
SSL_CTX_set_default_verify_paths(
	SSL_CTX *ctx)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Adds options (OR) and returns the new set.
 */
uint64_t
SSL_CTX_set_options(
	SSL_CTX *ctx,
	uint64_t options)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(options);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * The context ctrls: SSL_CTRL_SET_MIN_PROTO_VERSION and _MAX_ (number 0
 * for the lowest or highest supported, TLS1_2_VERSION or TLS1_3_VERSION;
 * another value returns 0), _GET_MIN_ and _GET_MAX_ (return the setting, 0
 * when unset).
 */
long
SSL_CTX_ctrl(
	SSL_CTX *ctx,
	int command,
	long number,
	void *pointer)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(command);
	UNUSED_PARAMETER(number);
	UNUSED_PARAMETER(pointer);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Returns the context's store (owned by the context; X509_STORE_add_cert
 * adds to it).
 */
X509_STORE *
SSL_CTX_get_cert_store(
	const SSL_CTX *ctx)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);

	/* Not implemented: the failure value. */
	return NULL;
}

/*
 * Makes a connection object from a context (taking a reference to it);
 * NULL on failure.
 */
SSL *
SSL_new(
	SSL_CTX *ctx)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);

	/* Not implemented: the failure value. */
	return NULL;
}

/*
 * Frees a connection (without sending anything; SSL_shutdown first for a
 * clean close); NULL is ignored.
 */
void
SSL_free(
	SSL *ssl)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ssl);
}

/*
 * Sets the socket: a file descriptor on zedBSD, a SOCKET (as int) on
 * Windows.
 */
int
SSL_set_fd(
	SSL *ssl,
	int descriptor)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ssl);
	UNUSED_PARAMETER(descriptor);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * The connection ctrls: SSL_CTRL_SET_TLSEXT_HOSTNAME with
 * TLSEXT_NAMETYPE_host_name and a host name (the SNI name; 1, or 0 for a
 * name that is not a valid host name), and the version ctrls as
 * SSL_CTX_ctrl.
 */
long
SSL_ctrl(
	SSL *ssl,
	int command,
	long number,
	void *pointer)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ssl);
	UNUSED_PARAMETER(command);
	UNUSED_PARAMETER(number);
	UNUSED_PARAMETER(pointer);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Sets the host name the certificate must match
 * (X509_VERIFY_PARAM_set1_host on the SSL's parameters).
 */
int
SSL_set1_host(
	SSL *ssl,
	const char *host)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ssl);
	UNUSED_PARAMETER(host);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Returns the SSL's verification parameters (owned by the SSL).
 */
X509_VERIFY_PARAM *
SSL_get0_param(
	SSL *ssl)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ssl);

	/* Not implemented: the failure value. */
	return NULL;
}

/*
 * Runs the handshake.
 */
int
SSL_connect(
	SSL *ssl)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ssl);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Reads up to length bytes of application data (running the handshake
 * first when it is not done).
 */
int
SSL_read(
	SSL *ssl,
	void *buffer,
	int length)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ssl);
	UNUSED_PARAMETER(buffer);
	UNUSED_PARAMETER(length);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Writes length bytes (all of them, in records of at most 16 KiB) and
 * returns length, or 0 or -1 as SSL_read.
 */
int
SSL_write(
	SSL *ssl,
	const void *buffer,
	int length)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ssl);
	UNUSED_PARAMETER(buffer);
	UNUSED_PARAMETER(length);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Classifies the result of the last SSL_connect, SSL_read, SSL_write or
 * SSL_shutdown on ssl (result is its return value): SSL_ERROR_NONE for a
 * positive result; WANT_READ or WANT_WRITE when the socket would block;
 * ZERO_RETURN for a clean close; SYSCALL for a socket failure (errno set;
 * an EOF without close_notify, without SSL_OP_IGNORE_UNEXPECTED_EOF, is
 * SSL_ERROR_SSL with SSL_R_UNEXPECTED_EOF_WHILE_READING queued, as in
 * OpenSSL 3); SSL_ERROR_SSL otherwise.
 */
int
SSL_get_error(
	const SSL *ssl,
	int result)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ssl);
	UNUSED_PARAMETER(result);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Returns the decrypted bytes buffered and not yet read.
 */
int
SSL_pending(
	const SSL *ssl)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ssl);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Sends close_notify (once).
 */
int
SSL_shutdown(
	SSL *ssl)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ssl);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Returns the X509_V_ result of the last handshake's verification
 * (X509_V_OK before one).
 */
long
SSL_get_verify_result(
	const SSL *ssl)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ssl);

	/* Not implemented: the failure value. */
	return 0;
}
