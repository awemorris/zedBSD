/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * TLS for https (plan/ws074/design.md §9, §17 D2): the OpenSSL package's
 * libssl and libcrypto, loaded through the operating-system layer the
 * first time a page asks for https, so that the base build does not depend on the package.  Without
 * the package every https fetch fails with EPROTONOSUPPORT.
 *
 * Only the functions below are used; their types are opaque here and they
 * are called through pointers, so no OpenSSL header is needed.  The
 * values of the few constants are OpenSSL's (stable across 1.1 and 3).
 *
 * A connection is TLS 1.2 or later, verifies the server's chain against
 * the default roots (the ca-certificates package's /etc/ssl/cert.pem) and
 * any file added with net_tls_add_ca_file (the tests' own CA), and checks
 * the certificate's name or address against the URL's host.  A failure is
 * EPROTO, with its reason kept for net_tls_error.
 *
 * Two threads use TLS: the page's (the blocking fetches) and the loader's
 * network thread.  The libraries are loaded and the context made once,
 * under tls_load_lock; a connection is used by one thread at a time; and
 * each thread keeps the reason of its own last failure.
 */

#include "net/net.h"
#include "base/os-system.h"

#include <errno.h>
#include <stdarg.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* OpenSSL's constants that are used (ssl.h, tls1.h, x509_vfy.h). */
#define TLS_VERIFY_PEER			1
#define TLS_CTRL_SET_TLSEXT_HOSTNAME	55
#define TLS_NAMETYPE_HOST_NAME		0
#define TLS_CTRL_SET_MIN_PROTO_VERSION	123
#define TLS_VERSION_1_2			0x0303
#define TLS_OP_IGNORE_UNEXPECTED_EOF	((uint64_t)1 << 7)
#define TLS_ERROR_SSL			1
#define TLS_ERROR_WANT_READ		2
#define TLS_ERROR_WANT_WRITE		3
#define TLS_ERROR_SYSCALL		5
#define TLS_ERROR_ZERO_RETURN		6
#define TLS_VERIFY_OK			0

/* The longest reason kept for net_tls_error. */
#define TLS_ERROR_MAX		256U

/* The most extra CA files the tests may add. */
#define TLS_CA_FILES_MAX	4U

/*
 * The library's functions, looked up in the loaded libraries.  The pointers are set once,
 * when the libraries are loaded, and never change after.
 */
struct tls_library {
	const void *(*client_method)(void);
	void *(*ctx_new)(const void *method);
	int (*ctx_set_default_verify_paths)(void *ctx);
	int (*ctx_load_verify_locations)(void *ctx, const char *file, const char *path);
	void (*ctx_set_verify)(void *ctx, int mode, void *callback);
	long (*ctx_ctrl)(void *ctx, int command, long number, void *pointer);
	uint64_t (*ctx_set_options)(void *ctx, uint64_t options);
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
};

/*
 * One function tls_find_all finds: in libcrypto or libssl, its name, and
 * its member of struct tls_library.
 */
struct tls_symbol {
	int crypto;
	const char *name;
	size_t offset;
};

/* The functions of struct tls_library, in its order.  The table is constant for the life of the program. */
static const struct tls_symbol tls_symbols[] = {
	{ 0, "TLS_client_method", offsetof(struct tls_library, client_method) },
	{ 0, "SSL_CTX_new", offsetof(struct tls_library, ctx_new) },
	{ 0, "SSL_CTX_set_default_verify_paths", offsetof(struct tls_library, ctx_set_default_verify_paths) },
	{ 0, "SSL_CTX_load_verify_locations", offsetof(struct tls_library, ctx_load_verify_locations) },
	{ 0, "SSL_CTX_set_verify", offsetof(struct tls_library, ctx_set_verify) },
	{ 0, "SSL_CTX_ctrl", offsetof(struct tls_library, ctx_ctrl) },
	{ 0, "SSL_CTX_set_options", offsetof(struct tls_library, ctx_set_options) },
	{ 0, "SSL_new", offsetof(struct tls_library, ssl_new) },
	{ 0, "SSL_free", offsetof(struct tls_library, ssl_free) },
	{ 0, "SSL_set_fd", offsetof(struct tls_library, set_fd) },
	{ 0, "SSL_ctrl", offsetof(struct tls_library, ssl_ctrl) },
	{ 0, "SSL_set1_host", offsetof(struct tls_library, set1_host) },
	{ 0, "SSL_get0_param", offsetof(struct tls_library, get0_param) },
	{ 0, "SSL_connect", offsetof(struct tls_library, connect) },
	{ 0, "SSL_read", offsetof(struct tls_library, read) },
	{ 0, "SSL_write", offsetof(struct tls_library, write) },
	{ 0, "SSL_get_error", offsetof(struct tls_library, get_error) },
	{ 0, "SSL_pending", offsetof(struct tls_library, pending) },
	{ 0, "SSL_shutdown", offsetof(struct tls_library, shutdown) },
	{ 0, "SSL_get_verify_result", offsetof(struct tls_library, get_verify_result) },
	{ 1, "X509_VERIFY_PARAM_set1_ip_asc", offsetof(struct tls_library, param_set1_ip_asc) },
	{ 1, "X509_verify_cert_error_string", offsetof(struct tls_library, verify_error_string) },
	{ 1, "ERR_get_error", offsetof(struct tls_library, err_get_error) },
	{ 1, "ERR_error_string_n", offsetof(struct tls_library, err_error_string_n) },
	{ 1, "ERR_clear_error", offsetof(struct tls_library, err_clear_error) }
};

/* The names libcrypto is looked for under, in order (the package's, then a host's versioned one). */
static const char *const tls_crypto_names[] = { "libcrypto.so", "libcrypto.so.3", NULL };

/* The names libssl is looked for under, in the same order. */
static const char *const tls_ssl_names[] = { "libssl.so", "libssl.so.3", NULL };

/*
 * One TLS connection over a connected socket.
 */
struct net_tls {
	void *ssl;
};

/* The loaded library, once tls_loaded is set (set under tls_load_lock, then only read). */
static struct tls_library tls_library;

/* 0 before the first attempt to load the library, 1 once loaded, -1 when it cannot be (under tls_load_lock). */
static int tls_loaded;

/* The context every connection is made from (its roots and settings), made with the library under tls_load_lock. */
static void *tls_context;

/* The lock of the loading: the first thread to need TLS loads it, the others wait and find it loaded. */
static pthread_mutex_t tls_load_lock = PTHREAD_MUTEX_INITIALIZER;

/* The CA files added for the tests, loaded into the context when it is made (set before the first fetch). */
static const char *tls_ca_files[TLS_CA_FILES_MAX];

/* How many entries of tls_ca_files are set. */
static size_t tls_ca_file_count;

/*
 * The key of each thread's reason of its last failure, for net_tls_error:
 * a buffer of TLS_ERROR_MAX bytes made the first time the thread keeps a
 * reason and freed when it ends.  tls_reason_ready says the key was made
 * (both are set once, under tls_reason_once).
 */
static pthread_key_t tls_reason_key;
static int tls_reason_ready;
static pthread_once_t tls_reason_once = PTHREAD_ONCE_INIT;

static int tls_load(void);
static int tls_load_once(void);
static void tls_make_reason_key(void);
static char *tls_reason_buffer(void);
static void tls_set_reason(const char *format, ...) __attribute__((format(printf, 1, 2)));
static int tls_find(struct wb_os_library *library, const char *name, void *pointer);
static int tls_find_all(struct wb_os_library *ssl, struct wb_os_library *crypto);
static int tls_attach(void *ssl, wb_os_socket socket);
static int tls_make_context(void);
static int tls_set_host(void *ssl, const char *host);
static int tls_is_address(const char *host);
static void tls_fail(const char *what, void *ssl, int result);

/*
 * Trusts the CA certificates of a PEM file besides the default roots
 * (the tests' own CA, --ca-file).  Must come before the first https fetch.
 */
int
net_tls_add_ca_file(
	const char *path)
{
	/* Kept until the context is made. */
	if (tls_ca_file_count == TLS_CA_FILES_MAX)
		return ENOSPC;
	tls_ca_files[tls_ca_file_count] = path;
	tls_ca_file_count++;
	return 0;
}

/*
 * Starts TLS on a connected socket to a host (the URL's host: a name, an
 * IPv4 address, or an IPv6 address in brackets): the handshake, the
 * chain and the name.  Returns 0 and the connection, EPROTONOSUPPORT
 * without the library, ENOMEM, or EPROTO (net_tls_error says why).
 */
int
net_tls_open(
	wb_os_socket socket,
	const char *host,
	struct net_tls **tls)
{
	struct net_tls *made;
	int result;
	int error;

	/* The library and the shared context. */
	*tls = NULL;
	tls_set_reason("%s", "");
	error = tls_load();
	if (error != 0)
		return error;

	/* The connection's state. */
	made = calloc(1, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	tls_library.err_clear_error();
	made->ssl = tls_library.ssl_new(tls_context);
	if (made->ssl == NULL) {
		free(made);
		return ENOMEM;
	}

	/* The socket, then the name sent (SNI) and the name checked. */
	error = tls_attach(made->ssl, socket);
	if (error != 0) {
		net_tls_close(made);
		return error;
	}
	error = tls_set_host(made->ssl, host);
	if (error != 0) {
		tls_fail("setup", made->ssl, 0);
		net_tls_close(made);
		return error;
	}

	/* The handshake, which verifies the chain and the name. */
	result = tls_library.connect(made->ssl);
	if (result != 1) {
		tls_fail("handshake", made->ssl, result);
		net_tls_close(made);
		return EPROTO;
	}

	/* Succeeded: the connection is secure. */
	*tls = made;
	return 0;
}

/*
 * Reads what the server sent, up to length bytes: *received is 0 at the
 * end of the connection.  Returns 0 or an errno value.
 */
int
net_tls_read(
	struct net_tls *tls,
	unsigned char *bytes,
	size_t length,
	size_t *received)
{
	int count;
	int result;
	int reason;
	int error;

	/* One read (OpenSSL takes an int). */
	*received = 0;
	count = (int)length;
	if (length > INT_MAX)
		count = INT_MAX;
	for (;;) {
		result = tls_library.read(tls->ssl, bytes, count);
		if (result > 0) {
			*received = (size_t)result;
			return 0;
		}

		/* A record that is not complete yet is read again. */
		reason = tls_library.get_error(tls->ssl, result);
		if (reason == TLS_ERROR_WANT_READ || reason == TLS_ERROR_WANT_WRITE)
			continue;
		break;
	}

	/* The server's close (or a close without close_notify, which the context allows). */
	if (reason == TLS_ERROR_ZERO_RETURN)
		return 0;
	if (reason == TLS_ERROR_SYSCALL) {
		error = wb_os_socket_last_error();
		if (error != 0)
			return error;
	}

	/* Anything else is a broken connection. */
	tls_fail("read", tls->ssl, result);
	return EPROTO;
}

/*
 * Sends all of a buffer.  Returns 0 or an errno value.
 */
int
net_tls_write(
	struct net_tls *tls,
	const unsigned char *bytes,
	size_t length)
{
	size_t done;
	int count;
	int result;
	int reason;

	/* Until everything is sent. */
	for (done = 0; done < length; done += (size_t)result) {
		count = (int)(length - done);
		if (length - done > INT_MAX)
			count = INT_MAX;
		result = tls_library.write(tls->ssl, bytes + done, count);
		if (result > 0)
			continue;

		/* A write that must wait is tried again. */
		reason = tls_library.get_error(tls->ssl, result);
		if (reason == TLS_ERROR_WANT_READ || reason == TLS_ERROR_WANT_WRITE) {
			result = 0;
			continue;
		}

		/* Anything else ends the request. */
		tls_fail("write", tls->ssl, result);
		return EPROTO;
	}

	/* Succeeded: the request is sent. */
	return 0;
}

/*
 * Starts TLS over a connected socket without waiting (the asynchronous
 * loader's way): the connection's state with its names set; the handshake
 * is then driven by net_tls_handshake as the socket becomes ready.
 */
int
net_tls_start(
	wb_os_socket socket,
	const char *host,
	struct net_tls **tls)
{
	struct net_tls *made;
	int error;

	/* The library and the shared context. */
	*tls = NULL;
	tls_set_reason("%s", "");
	error = tls_load();
	if (error != 0)
		return error;

	/* The connection's state. */
	made = calloc(1, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	tls_library.err_clear_error();
	made->ssl = tls_library.ssl_new(tls_context);
	if (made->ssl == NULL) {
		free(made);
		return ENOMEM;
	}

	/* The socket, then the name sent (SNI) and the name checked. */
	error = tls_attach(made->ssl, socket);
	if (error != 0) {
		net_tls_close(made);
		return error;
	}
	error = tls_set_host(made->ssl, host);
	if (error != 0) {
		tls_fail("setup", made->ssl, 0);
		net_tls_close(made);
		return error;
	}

	/* Succeeded: the handshake can start. */
	*tls = made;
	return 0;
}

/*
 * Takes the handshake as far as the socket allows: 0 when it is done,
 * EAGAIN when it waits for the socket (*wants says for reading,
 * WB_OS_POLL_READABLE, or writing, WB_OS_POLL_WRITABLE), EPROTO when it
 * failed (net_tls_error says why).
 */
int
net_tls_handshake(
	struct net_tls *tls,
	unsigned *wants)
{
	int result;
	int reason;

	/* One step of the handshake. */
	*wants = 0;
	result = tls_library.connect(tls->ssl);
	if (result == 1)
		return 0;

	/* A handshake that waits for the socket. */
	reason = tls_library.get_error(tls->ssl, result);
	if (reason == TLS_ERROR_WANT_READ) {
		*wants = WB_OS_POLL_READABLE;
		return EAGAIN;
	}

	/* Or for the socket to take more. */
	if (reason == TLS_ERROR_WANT_WRITE) {
		*wants = WB_OS_POLL_WRITABLE;
		return EAGAIN;
	}

	/* Anything else failed (the chain or the name did not verify). */
	tls_fail("handshake", tls->ssl, result);
	return EPROTO;
}

/*
 * Reads what TLS can give without waiting: *received bytes (0 at the end
 * of the connection), EAGAIN when it must wait for the socket (*wants
 * says for what), or an error.
 */
int
net_tls_read_some(
	struct net_tls *tls,
	unsigned char *bytes,
	size_t length,
	size_t *received,
	unsigned *wants)
{
	int count;
	int result;
	int reason;
	int error;

	/* One read (OpenSSL takes an int). */
	*received = 0;
	*wants = 0;
	count = (int)length;
	if (length > INT_MAX)
		count = INT_MAX;
	result = tls_library.read(tls->ssl, bytes, count);
	if (result > 0) {
		*received = (size_t)result;
		return 0;
	}

	/* A record that is not complete yet waits for the socket. */
	reason = tls_library.get_error(tls->ssl, result);
	if (reason == TLS_ERROR_WANT_READ) {
		*wants = WB_OS_POLL_READABLE;
		return EAGAIN;
	}

	/* Or for the socket to take more. */
	if (reason == TLS_ERROR_WANT_WRITE) {
		*wants = WB_OS_POLL_WRITABLE;
		return EAGAIN;
	}

	/* The server's close (or a close without close_notify, which the context allows). */
	if (reason == TLS_ERROR_ZERO_RETURN)
		return 0;
	if (reason == TLS_ERROR_SYSCALL) {
		error = wb_os_socket_last_error();
		if (error != 0)
			return error;
	}

	/* Anything else is a broken connection. */
	tls_fail("read", tls->ssl, result);
	return EPROTO;
}

/*
 * Writes what TLS can take without waiting: *sent bytes, EAGAIN when it
 * must wait for the socket (*wants says for what), or an error.
 */
int
net_tls_write_some(
	struct net_tls *tls,
	const unsigned char *bytes,
	size_t length,
	size_t *sent,
	unsigned *wants)
{
	int count;
	int result;
	int reason;

	/* One write. */
	*sent = 0;
	*wants = 0;
	count = (int)length;
	if (length > INT_MAX)
		count = INT_MAX;
	result = tls_library.write(tls->ssl, bytes, count);
	if (result > 0) {
		*sent = (size_t)result;
		return 0;
	}

	/* A write that waits for the socket. */
	reason = tls_library.get_error(tls->ssl, result);
	if (reason == TLS_ERROR_WANT_READ) {
		*wants = WB_OS_POLL_READABLE;
		return EAGAIN;
	}

	/* Or for the socket to take more. */
	if (reason == TLS_ERROR_WANT_WRITE) {
		*wants = WB_OS_POLL_WRITABLE;
		return EAGAIN;
	}

	/* Anything else ends the request. */
	tls_fail("write", tls->ssl, result);
	return EPROTO;
}

/*
 * Tells whether bytes already decrypted wait to be read (then the socket
 * need not be readable for the next read).
 */
int
net_tls_pending(
	const struct net_tls *tls)
{
	int pending;

	/* The bytes OpenSSL holds. */
	pending = tls_library.pending(tls->ssl);
	if (pending > 0)
		return 1;

	/* Nothing held: the next read waits for the socket. */
	return 0;
}

/*
 * Ends a connection (sends close_notify once, without waiting for the
 * server's) and frees it; the socket stays open for the caller to close.
 */
void
net_tls_close(
	struct net_tls *tls)
{
	/* Nothing to end. */
	if (tls == NULL)
		return;

	/* The close, then the state. */
	tls_library.shutdown(tls->ssl);
	tls_library.ssl_free(tls->ssl);
	free(tls);
}

/*
 * Forgets the reason of an earlier failure (each fetch starts without one).
 */
void
net_tls_clear_error(void)
{
	/* The kept text. */
	tls_set_reason("%s", "");
}

/*
 * Sets the calling thread's reason of the last TLS failure: the loader
 * hands the page's thread the reason its network thread kept for a
 * request ("" for none).
 */
void
net_tls_set_error(
	const char *reason)
{
	/* The text, cut to what is kept. */
	tls_set_reason("%s", reason);
}

/*
 * The reason of the calling thread's last TLS failure (for the error
 * line), or "".
 */
const char *
net_tls_error(void)
{
	const char *buffer;

	/* The text kept by tls_fail, or none when the thread kept nothing. */
	buffer = tls_reason_buffer();
	if (buffer == NULL)
		return "";

	/* The thread's text. */
	return buffer;
}

/* Loads the libraries and makes the context, once for all threads; returns 0 or EPROTONOSUPPORT. */
static int
tls_load(void)
{
	int error;

	/* The first thread loads; the others wait for it and find the outcome. */
	pthread_mutex_lock(&tls_load_lock);

	error = tls_load_once();

	pthread_mutex_unlock(&tls_load_lock);

	/* TLS cannot be used. */
	if (error != 0)
		return error;

	/* Succeeded: the library and the context are ready. */
	return 0;
}

/*
 * Loads the libraries and makes the context unless that was tried already
 * (under tls_load_lock).  A library that loaded stays loaded, even when
 * TLS cannot be used after all (libcrypto may have registered its own
 * clean-up at the program's exit).
 */
static int
tls_load_once(void)
{
	struct wb_os_library *ssl;
	struct wb_os_library *crypto;
	int error;

	/* Already done, or already known to be impossible. */
	if (tls_loaded > 0)
		return 0;
	if (tls_loaded < 0) {
		tls_set_reason("cannot load the OpenSSL libraries");
		return EPROTONOSUPPORT;
	}

	/* libcrypto first (libssl needs it), then libssl. */
	tls_loaded = -1;
	ssl = NULL;
	error = wb_os_library_open(tls_crypto_names, &crypto);
	if (error == 0)
		error = wb_os_library_open(tls_ssl_names, &ssl);
	if (error != 0) {
		tls_set_reason("cannot load the OpenSSL libraries");
		return EPROTONOSUPPORT;
	}

	/* The functions, then the context. */
	error = tls_find_all(ssl, crypto);
	if (error == 0)
		error = tls_make_context();
	if (error != 0)
		return EPROTONOSUPPORT;

	/* Succeeded: https works from now on (the libraries stay loaded). */
	tls_loaded = 1;
	return 0;
}

/* Finds one function; returns 0 or ENOENT (with the reason kept). */
static int
tls_find(
	struct wb_os_library *library,
	const char *name,
	void *pointer)
{
	wb_os_function found;

	/* The function. */
	found = wb_os_library_symbol(library, name);
	if (found == NULL) {
		tls_set_reason("OpenSSL has no %s", name);
		return ENOENT;
	}

	/* Stored through the member's address (every member is a function pointer of the same size). */
	memcpy(pointer, &found, sizeof(found));
	return 0;
}

/* Finds every function the connections use (tls_symbols). */
static int
tls_find_all(
	struct wb_os_library *ssl,
	struct wb_os_library *crypto)
{
	struct wb_os_library *library;
	size_t index;
	int error;

	/* Each symbol, from libcrypto or libssl, into its member. */
	for (index = 0; index < sizeof(tls_symbols) / sizeof(tls_symbols[0]); index++) {
		library = ssl;
		if (tls_symbols[index].crypto)
			library = crypto;
		error = tls_find(library, tls_symbols[index].name, (char *)&tls_library + tls_symbols[index].offset);
		if (error != 0)
			return error;
	}

	/* Succeeded: every function is there. */
	return 0;
}

/*
 * Gives a connection its socket, as the int SSL_set_fd takes.  Returns 0,
 * or EPROTO with the reason kept.
 */
static int
tls_attach(
	void *ssl,
	wb_os_socket socket)
{
	int descriptor;
	int result;
	int error;

	/* The socket's int. */
	error = wb_os_socket_descriptor(socket, &descriptor);
	if (error != 0) {
		tls_set_reason("setup: the socket does not fit OpenSSL's descriptor (error %d)", error);
		return EPROTO;
	}

	/* The connection reads and writes it. */
	result = tls_library.set_fd(ssl, descriptor);
	if (result != 1) {
		tls_fail("setup", ssl, 0);
		return EPROTO;
	}

	/* Succeeded: the connection has its socket. */
	return 0;
}

/* Makes the context: TLS 1.2 or later, the peer verified against the default roots and the added CA files. */
static int
tls_make_context(void)
{
	size_t index;
	long set;
	int result;

	/* The client context. */
	tls_context = tls_library.ctx_new(tls_library.client_method());
	if (tls_context == NULL) {
		tls_set_reason("cannot make an OpenSSL context");
		return ENOMEM;
	}

	/* TLS 1.2 or later. */
	set = tls_library.ctx_ctrl(tls_context, TLS_CTRL_SET_MIN_PROTO_VERSION, TLS_VERSION_1_2, NULL);
	if (set != 1) {
		tls_set_reason("OpenSSL refuses TLS 1.2 as the least version");
		return EPROTO;
	}

	/* The peer must verify; a close without close_notify reads as the end. */
	tls_library.ctx_set_verify(tls_context, TLS_VERIFY_PEER, NULL);
	tls_library.ctx_set_options(tls_context, TLS_OP_IGNORE_UNEXPECTED_EOF);

	/* The roots: the default ones (a missing file only leaves them empty), then the added files. */
	tls_library.ctx_set_default_verify_paths(tls_context);
	for (index = 0; index < tls_ca_file_count; index++) {
		result = tls_library.ctx_load_verify_locations(tls_context, tls_ca_files[index], NULL);
		if (result != 1) {
			tls_set_reason("cannot read the CA file %s", tls_ca_files[index]);
			return EPROTO;
		}
	}

	/* Succeeded: connections can be made. */
	return 0;
}

/* Sets the name sent and checked: a name goes in SNI and is matched; an address is matched only. */
static int
tls_set_host(
	void *ssl,
	const char *host)
{
	char address[64];
	size_t length;
	long sent;
	int is_address;
	int result;

	/* An address, without an IPv6 address's brackets, is checked against the certificate's addresses. */
	is_address = tls_is_address(host);
	if (is_address) {
		length = strlen(host);
		if (host[0] == '[' && length >= 2U && length - 2U < sizeof(address)) {
			memcpy(address, host + 1, length - 2U);
			address[length - 2U] = '\0';
		} else if (length < sizeof(address)) {
			memcpy(address, host, length + 1U);
		} else {
			return EINVAL;
		}

		/* The address the certificate must name. */
		result = tls_library.param_set1_ip_asc(tls_library.get0_param(ssl), address);
		if (result != 1)
			return EPROTO;
		return 0;
	}

	/* A name: sent to the server, and matched against the certificate. */
	sent = tls_library.ssl_ctrl(ssl, TLS_CTRL_SET_TLSEXT_HOSTNAME, TLS_NAMETYPE_HOST_NAME, (void *)host);
	if (sent != 1)
		return EPROTO;
	result = tls_library.set1_host(ssl, host);
	if (result != 1)
		return EPROTO;

	/* Succeeded: the name is set. */
	return 0;
}

/* Tells whether a URL's host is an address (IPv6 in brackets, or IPv4: a domain never ends in a digit after parsing). */
static int
tls_is_address(
	const char *host)
{
	size_t length;

	/* The two forms the host parser writes. */
	length = strlen(host);
	if (length == 0)
		return 0;
	if (host[0] == '[')
		return 1;
	if (host[length - 1U] >= '0' && host[length - 1U] <= '9')
		return 1;
	return 0;
}

/* Keeps the reason of a failure: the certificate's verification, or OpenSSL's error queue. */
static void
tls_fail(
	const char *what,
	void *ssl,
	int result)
{
	const char *described;
	unsigned long queued;
	long verified;
	int reason;
	char text[TLS_ERROR_MAX / 2U];

	/* A certificate that did not verify says so. */
	verified = tls_library.get_verify_result(ssl);
	if (verified != TLS_VERIFY_OK) {
		described = tls_library.verify_error_string(verified);
		tls_set_reason("%s: certificate verify failed: %s", what, described);
		tls_library.err_clear_error();
		return;
	}

	/* Otherwise the first queued error, or the kind of failure. */
	queued = tls_library.err_get_error();
	if (queued != 0) {
		tls_library.err_error_string_n(queued, text, sizeof(text));
		tls_set_reason("%s: %s", what, text);
		tls_library.err_clear_error();
		return;
	}

	/* No queued error: the connection ended or failed underneath. */
	reason = TLS_ERROR_SSL;
	if (result <= 0)
		reason = tls_library.get_error(ssl, result);
	tls_set_reason("%s: failed (OpenSSL error %d)", what, reason);
}

/* Makes the key of the threads' reasons (once). */
static void
tls_make_reason_key(void)
{
	int status;

	/* Each thread's buffer is freed when the thread ends. */
	status = pthread_key_create(&tls_reason_key, free);
	if (status != 0)
		return;

	/* Succeeded: the threads may keep reasons. */
	tls_reason_ready = 1;
}

/* Finds the calling thread's reason buffer, made the first time; NULL without memory. */
static char *
tls_reason_buffer(void)
{
	char *buffer;
	int status;

	/* The key, made by the first thread that needs it. */
	pthread_once(&tls_reason_once, tls_make_reason_key);
	if (!tls_reason_ready)
		return NULL;

	/* The thread's buffer, when it has one. */
	buffer = pthread_getspecific(tls_reason_key);
	if (buffer != NULL)
		return buffer;

	/* A new one, empty. */
	buffer = calloc(1, TLS_ERROR_MAX);
	if (buffer == NULL)
		return NULL;

	/* Kept for the thread. */
	status = pthread_setspecific(tls_reason_key, buffer);
	if (status != 0) {
		free(buffer);
		return NULL;
	}

	/* Succeeded: the thread's buffer. */
	return buffer;
}

/* Keeps the calling thread's reason of a failure (a reason that does not fit is cut; without memory it is lost). */
static void
tls_set_reason(
	const char *format,
	...)
{
	va_list arguments;
	char *buffer;

	/* The thread's buffer. */
	buffer = tls_reason_buffer();
	if (buffer == NULL)
		return;

	/* The text. */
	va_start(arguments, format);
	vsnprintf(buffer, TLS_ERROR_MAX, format, arguments);
	va_end(arguments);
}
