/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The table of the TLS library's functions that tls.c calls, shared with
 * tls-static.c, which fills it from a library linked into the program.
 * The types are opaque (void pointers) so that tls.c needs no OpenSSL
 * header; a function that takes a callback takes one of the function pointer
 * types below.
 */

#ifndef KEILAND_BROWSER_NET_TLS_LIBRARY_H
#define KEILAND_BROWSER_NET_TLS_LIBRARY_H

#include <stddef.h>
#include <stdint.h>

/*
 * The callback the library calls for each certificate of a chain it is
 * verifying (the signature of OpenSSL's SSL_verify_cb).  preverified is 1
 * when the library found the certificate acceptable so far; the callback
 * returns 1 to go on and 0 to fail the verification, and may first set the
 * failure with the store-context function of struct tls_library.
 */
typedef int (*tls_verify_callback)(int preverified, void *store_context);

/*
 * The library's functions, looked up in the loaded libraries or taken from
 * the static library.  The pointers are set once, when the library is
 * bound, and never change after.  A member marked "OpenSSL only" is one the
 * other implementation of the interface need not have: when it is NULL, the
 * feature that uses it (reading no configuration file, refusing the
 * certificates the system distrusts) is not used and TLS works without it.
 */
struct tls_library {
	const void *(*client_method)(void);
	void *(*ctx_new)(const void *method);
	int (*ctx_set_default_verify_paths)(void *ctx);
	int (*ctx_load_verify_locations)(void *ctx, const char *file, const char *path);
	void (*ctx_set_verify)(void *ctx, int mode, tls_verify_callback callback);
	long (*ctx_ctrl)(void *ctx, int command, long number, void *pointer);
	uint64_t (*ctx_set_options)(void *ctx, uint64_t options);
	void *(*ctx_get_cert_store)(const void *ctx);
	void *(*ssl_new)(void *ctx);
	void (*ssl_free)(void *ssl);
	int (*set_fd)(void *ssl, int descriptor);
	long (*ssl_ctrl)(void *ssl, int command, long number, void *pointer);
	int (*set1_host)(void *ssl, const char *host);
	void *(*get0_param)(void *ssl);
	int (*connect)(void *ssl);
	int (*read)(void *ssl, void *bytes, int length);
	int (*write)(void *ssl, const void *bytes, int length);
	int (*get_error)(const void *ssl, int result);
	int (*pending)(const void *ssl);
	int (*shutdown)(void *ssl);
	long (*get_verify_result)(const void *ssl);
	int (*param_set1_ip_asc)(void *param, const char *address);
	const char *(*verify_error_string)(long result);
	unsigned long (*err_get_error)(void);
	void (*err_error_string_n)(unsigned long error, char *text, size_t length);
	void (*err_clear_error)(void);
	void *(*d2i_x509)(void **certificate, const unsigned char **bytes, long length);
	int (*x509_store_add_cert)(void *store, void *certificate);
	void (*x509_free)(void *certificate);

	/* OpenSSL only: reading no configuration file. */
	int (*init_ssl)(uint64_t options, const void *settings);

	/* OpenSSL only: the fingerprint of a certificate, to refuse the distrusted ones. */
	int (*x509_digest)(const void *certificate, const void *digest, unsigned char *fingerprint, unsigned int *length);
	const void *(*evp_sha256)(void);

	/* OpenSSL only: the certificate a verification is looking at, and its failure. */
	void *(*store_ctx_get_current_cert)(const void *store_context);
	void (*store_ctx_set_error)(void *store_context, int error);
};

/*
 * Fills a table from the TLS library linked into the program (tls-static.c,
 * which a build that links the library statically compiles in place of
 * opening shared libraries).  Returns 0, or an errno value when the table
 * cannot be filled.
 */
int tls_static_library(struct tls_library *library);

#endif
