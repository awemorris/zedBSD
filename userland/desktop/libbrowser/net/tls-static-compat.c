/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libssl-compat and libcrypto-compat linked into the program, as the table
 * tls.c calls through.  A build that links them statically (the Windows
 * build) compiles this file with tls.c's WB_TLS_STATIC; the other builds do
 * not list it and open the shared libraries instead.
 *
 * Each member of struct tls_library is a function of the same shape with
 * the library's types in place of the opaque ones, so each is one small
 * function that passes its arguments on: no function pointer of one type
 * is converted into another.  The members marked "OpenSSL only" stay NULL:
 * the library reads no configuration file, and its default paths read the
 * system's stores and refuse the certificates the system distrusts.
 */

#include "net/tls-library.h"

#include <compat/crypto/err.h>
#include <compat/crypto/x509.h>
#include <compat/ssl/ssl.h>

/*
 * The verification callback of the context, which the library calls with
 * its own store-context type; tls_static_ctx_set_verify keeps it for
 * tls_static_verify_trampoline.  Set while the context is made (under
 * tls.c's lock), then only read.
 */
static tls_verify_callback tls_static_verify_callback;

static const void *tls_static_client_method(void);
static void *tls_static_ctx_new(const void *method);
static int tls_static_ctx_set_default_verify_paths(void *ctx);
static int tls_static_ctx_load_verify_locations(void *ctx, const char *file, const char *path);
static int tls_static_verify_trampoline(int preverified, X509_STORE_CTX *store_context);
static void tls_static_ctx_set_verify(void *ctx, int mode, tls_verify_callback callback);
static long tls_static_ctx_ctrl(void *ctx, int command, long number, void *pointer);
static uint64_t tls_static_ctx_set_options(void *ctx, uint64_t options);
static void *tls_static_ctx_get_cert_store(const void *ctx);
static void *tls_static_ssl_new(void *ctx);
static void tls_static_ssl_free(void *ssl);
static int tls_static_set_fd(void *ssl, int descriptor);
static long tls_static_ssl_ctrl(void *ssl, int command, long number, void *pointer);
static int tls_static_set1_host(void *ssl, const char *host);
static void *tls_static_get0_param(void *ssl);
static int tls_static_connect(void *ssl);
static int tls_static_read(void *ssl, void *bytes, int length);
static int tls_static_write(void *ssl, const void *bytes, int length);
static int tls_static_get_error(const void *ssl, int result);
static int tls_static_pending(const void *ssl);
static int tls_static_shutdown(void *ssl);
static long tls_static_get_verify_result(const void *ssl);
static int tls_static_param_set1_ip_asc(void *param, const char *address);
static const char *tls_static_verify_error_string(long result);
static unsigned long tls_static_err_get_error(void);
static void tls_static_err_error_string_n(unsigned long error, char *text, size_t length);
static void tls_static_err_clear_error(void);
static void *tls_static_d2i_x509(void **certificate, const unsigned char **bytes, long length);
static int tls_static_x509_store_add_cert(void *store, void *certificate);
static void tls_static_x509_free(void *certificate);

/*
 * Fills the table with the linked library's functions.  Returns 0.
 */
int
tls_static_library(
	struct tls_library *library)
{
	/* The context and its settings. */
	library->client_method = tls_static_client_method;
	library->ctx_new = tls_static_ctx_new;
	library->ctx_set_default_verify_paths = tls_static_ctx_set_default_verify_paths;
	library->ctx_load_verify_locations = tls_static_ctx_load_verify_locations;
	library->ctx_set_verify = tls_static_ctx_set_verify;
	library->ctx_ctrl = tls_static_ctx_ctrl;
	library->ctx_set_options = tls_static_ctx_set_options;
	library->ctx_get_cert_store = tls_static_ctx_get_cert_store;

	/* A connection. */
	library->ssl_new = tls_static_ssl_new;
	library->ssl_free = tls_static_ssl_free;
	library->set_fd = tls_static_set_fd;
	library->ssl_ctrl = tls_static_ssl_ctrl;
	library->set1_host = tls_static_set1_host;
	library->get0_param = tls_static_get0_param;
	library->connect = tls_static_connect;
	library->read = tls_static_read;
	library->write = tls_static_write;
	library->get_error = tls_static_get_error;
	library->pending = tls_static_pending;
	library->shutdown = tls_static_shutdown;
	library->get_verify_result = tls_static_get_verify_result;

	/* Names, errors and certificates. */
	library->param_set1_ip_asc = tls_static_param_set1_ip_asc;
	library->verify_error_string = tls_static_verify_error_string;
	library->err_get_error = tls_static_err_get_error;
	library->err_error_string_n = tls_static_err_error_string_n;
	library->err_clear_error = tls_static_err_clear_error;
	library->d2i_x509 = tls_static_d2i_x509;
	library->x509_store_add_cert = tls_static_x509_store_add_cert;
	library->x509_free = tls_static_x509_free;

	/* OpenSSL only: not in this library. */
	library->init_ssl = NULL;
	library->x509_digest = NULL;
	library->evp_sha256 = NULL;
	library->store_ctx_get_current_cert = NULL;
	library->store_ctx_set_error = NULL;

	/* Succeeded: the table is full. */
	return 0;
}

/* TLS_client_method. */
static const void *
tls_static_client_method(void)
{
	return TLS_client_method();
}

/* SSL_CTX_new. */
static void *
tls_static_ctx_new(
	const void *method)
{
	return SSL_CTX_new(method);
}

/* SSL_CTX_set_default_verify_paths. */
static int
tls_static_ctx_set_default_verify_paths(
	void *ctx)
{
	return SSL_CTX_set_default_verify_paths(ctx);
}

/* SSL_CTX_load_verify_locations. */
static int
tls_static_ctx_load_verify_locations(
	void *ctx,
	const char *file,
	const char *path)
{
	return SSL_CTX_load_verify_locations(ctx, file, path);
}

/* The callback the library calls: tls.c's, with the store context as the opaque pointer it takes. */
static int
tls_static_verify_trampoline(
	int preverified,
	X509_STORE_CTX *store_context)
{
	return tls_static_verify_callback(preverified, store_context);
}

/* SSL_CTX_set_verify, with the callback kept for the trampoline. */
static void
tls_static_ctx_set_verify(
	void *ctx,
	int mode,
	tls_verify_callback callback)
{
	/* No callback: the library's own verification only. */
	tls_static_verify_callback = callback;
	if (callback == NULL) {
		SSL_CTX_set_verify(ctx, mode, NULL);
		return;
	}

	/* A callback: through the trampoline. */
	SSL_CTX_set_verify(ctx, mode, tls_static_verify_trampoline);
}

/* SSL_CTX_ctrl. */
static long
tls_static_ctx_ctrl(
	void *ctx,
	int command,
	long number,
	void *pointer)
{
	return SSL_CTX_ctrl(ctx, command, number, pointer);
}

/* SSL_CTX_set_options. */
static uint64_t
tls_static_ctx_set_options(
	void *ctx,
	uint64_t options)
{
	return SSL_CTX_set_options(ctx, options);
}

/* SSL_CTX_get_cert_store. */
static void *
tls_static_ctx_get_cert_store(
	const void *ctx)
{
	return SSL_CTX_get_cert_store(ctx);
}

/* SSL_new. */
static void *
tls_static_ssl_new(
	void *ctx)
{
	return SSL_new(ctx);
}

/* SSL_free. */
static void
tls_static_ssl_free(
	void *ssl)
{
	SSL_free(ssl);
}

/* SSL_set_fd. */
static int
tls_static_set_fd(
	void *ssl,
	int descriptor)
{
	return SSL_set_fd(ssl, descriptor);
}

/* SSL_ctrl. */
static long
tls_static_ssl_ctrl(
	void *ssl,
	int command,
	long number,
	void *pointer)
{
	return SSL_ctrl(ssl, command, number, pointer);
}

/* SSL_set1_host. */
static int
tls_static_set1_host(
	void *ssl,
	const char *host)
{
	return SSL_set1_host(ssl, host);
}

/* SSL_get0_param. */
static void *
tls_static_get0_param(
	void *ssl)
{
	return SSL_get0_param(ssl);
}

/* SSL_connect. */
static int
tls_static_connect(
	void *ssl)
{
	return SSL_connect(ssl);
}

/* SSL_read. */
static int
tls_static_read(
	void *ssl,
	void *bytes,
	int length)
{
	return SSL_read(ssl, bytes, length);
}

/* SSL_write. */
static int
tls_static_write(
	void *ssl,
	const void *bytes,
	int length)
{
	return SSL_write(ssl, bytes, length);
}

/* SSL_get_error. */
static int
tls_static_get_error(
	const void *ssl,
	int result)
{
	return SSL_get_error(ssl, result);
}

/* SSL_pending. */
static int
tls_static_pending(
	const void *ssl)
{
	return SSL_pending(ssl);
}

/* SSL_shutdown. */
static int
tls_static_shutdown(
	void *ssl)
{
	return SSL_shutdown(ssl);
}

/* SSL_get_verify_result. */
static long
tls_static_get_verify_result(
	const void *ssl)
{
	return SSL_get_verify_result(ssl);
}

/* X509_VERIFY_PARAM_set1_ip_asc. */
static int
tls_static_param_set1_ip_asc(
	void *param,
	const char *address)
{
	return X509_VERIFY_PARAM_set1_ip_asc(param, address);
}

/* X509_verify_cert_error_string. */
static const char *
tls_static_verify_error_string(
	long result)
{
	return X509_verify_cert_error_string(result);
}

/* ERR_get_error. */
static unsigned long
tls_static_err_get_error(void)
{
	return ERR_get_error();
}

/* ERR_error_string_n. */
static void
tls_static_err_error_string_n(
	unsigned long error,
	char *text,
	size_t length)
{
	ERR_error_string_n(error, text, length);
}

/* ERR_clear_error. */
static void
tls_static_err_clear_error(void)
{
	ERR_clear_error();
}

/* d2i_X509. */
static void *
tls_static_d2i_x509(
	void **certificate,
	const unsigned char **bytes,
	long length)
{
	X509 *parsed;
	X509 *given;

	/* A certificate given to be reused is the library's own type. */
	given = NULL;
	if (certificate != NULL)
		given = *certificate;

	/* The parse. */
	parsed = d2i_X509(&given, bytes, length);
	if (certificate != NULL)
		*certificate = given;
	return parsed;
}

/* X509_STORE_add_cert. */
static int
tls_static_x509_store_add_cert(
	void *store,
	void *certificate)
{
	return X509_STORE_add_cert(store, certificate);
}

/* X509_free. */
static void
tls_static_x509_free(
	void *certificate)
{
	X509_free(certificate);
}
