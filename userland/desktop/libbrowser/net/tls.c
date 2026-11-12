/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * TLS for https through OpenSSL's libssl and libcrypto interface: zedBSD's
 * own libssl-compat and libcrypto-compat, or OpenSSL itself.  A build that
 * links the library into the program (WB_TLS_STATIC, the Windows build,
 * whose tls-static file fills the table of functions) uses it as linked.
 * Any other build loads the libraries through the operating-system layer
 * the first time a page asks for https, so that the program does not need
 * them to start: libssl-compat.so and libcrypto-compat.so in a build with
 * WB_TLS_COMPAT (zedBSD's), OpenSSL's libssl and libcrypto otherwise (a
 * host's tests).  Only the build's own choice is opened, never the other:
 * without it every https fetch fails with EPROTONOSUPPORT.
 *
 * Only the functions of struct tls_library are used; their types are
 * opaque here and they are called through pointers, so no header of the
 * library is needed.  The values of the few constants are OpenSSL's
 * (stable across 1.1 and 3, and the same in libssl-compat).  The members of
 * the table marked "OpenSSL only" are OpenSSL's alone: with OpenSSL the
 * program says to read no configuration file and refuses the certificates
 * the system distrusts itself; libssl-compat reads no configuration file
 * and refuses them in its own verification.
 *
 * A connection is TLS 1.2 or later, verifies the server's chain against
 * the system's trusted roots (with OpenSSL, the operating-system layer
 * hands them over where the system keeps them itself, elsewhere the
 * library's default paths, the ca-certificates package's /etc/ssl/cert.pem;
 * libssl-compat reads them itself through its default paths on every
 * system), the bundle of roots shipped with the program, and any file added
 * with net_tls_add_ca_file (the tests' own CA), refuses a certificate the
 * system distrusts anywhere in the chain, and checks the certificate's name
 * or address against the URL's host.  A failure is EPROTO, with its reason
 * kept in the caller's buffer (net_tls_open, net_tls_start) or in the
 * connection (net_tls_reason).
 *
 * Two threads use TLS: the page's (the blocking fetches) and the loader's
 * network thread.  The libraries are loaded and the context made once,
 * under tls_load_lock; a connection is used by one thread at a time; and
 * each thread keeps the reason of its own last failure.
 */

#include "net/net.h"
#include "base/thread.h"
#include "net/tls-library.h"
#include "base/os-system.h"

#include <errno.h>
#include <stdarg.h>
#include <limits.h>
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
#define TLS_INIT_NO_LOAD_CONFIG		((uint64_t)0x80)
#define TLS_CERT_REJECTED		28

/* The longest text of OpenSSL's error kept in a reason. */
#define TLS_ERROR_MAX		NET_TLS_REASON_MAX

/* The most extra CA files the tests may add. */
#define TLS_CA_FILES_MAX	4U

/* The size of a SHA-256 fingerprint of a certificate. */
#define TLS_FINGERPRINT_SIZE	32U

/* The most certificates the system may distrust that are remembered (more fails the context, never lets one through). */
#define TLS_DISTRUSTED_MAX	1024U

/* The longest path of the bundle of roots shipped with the program. */
#define TLS_BUNDLE_PATH_MAX	1024U

/* The bundle's path below the program's data directory. */
#define TLS_BUNDLE_NAME		"/certs/ca-certificates.pem"

/* The libraries a build opens, named in a failure to load them. */
#ifdef WB_TLS_COMPAT
#define TLS_LIBRARY_NAMES	"libssl-compat.so and libcrypto-compat.so"
#else
#define TLS_LIBRARY_NAMES	"OpenSSL's libssl and libcrypto"
#endif

#ifndef WB_TLS_STATIC

/*
 * One function tls_find_all finds: in libcrypto or libssl, its name, whether
 * the table may do without it ("OpenSSL only", see struct tls_library), and
 * its member of struct tls_library.
 */
struct tls_symbol {
	int crypto;
	int optional;
	const char *name;
	size_t offset;
};

/* The functions of struct tls_library, in its order.  The table is constant for the life of the program. */
static const struct tls_symbol tls_symbols[] = {
	{ 0, 0, "TLS_client_method", offsetof(struct tls_library, client_method) },
	{ 0, 0, "SSL_CTX_new", offsetof(struct tls_library, ctx_new) },
	{ 0, 0, "SSL_CTX_set_default_verify_paths", offsetof(struct tls_library, ctx_set_default_verify_paths) },
	{ 0, 0, "SSL_CTX_load_verify_locations", offsetof(struct tls_library, ctx_load_verify_locations) },
	{ 0, 0, "SSL_CTX_set_verify", offsetof(struct tls_library, ctx_set_verify) },
	{ 0, 0, "SSL_CTX_ctrl", offsetof(struct tls_library, ctx_ctrl) },
	{ 0, 0, "SSL_CTX_set_options", offsetof(struct tls_library, ctx_set_options) },
	{ 0, 0, "SSL_CTX_get_cert_store", offsetof(struct tls_library, ctx_get_cert_store) },
	{ 0, 0, "SSL_new", offsetof(struct tls_library, ssl_new) },
	{ 0, 0, "SSL_free", offsetof(struct tls_library, ssl_free) },
	{ 0, 0, "SSL_set_fd", offsetof(struct tls_library, set_fd) },
	{ 0, 0, "SSL_ctrl", offsetof(struct tls_library, ssl_ctrl) },
	{ 0, 0, "SSL_set1_host", offsetof(struct tls_library, set1_host) },
	{ 0, 0, "SSL_get0_param", offsetof(struct tls_library, get0_param) },
	{ 0, 0, "SSL_connect", offsetof(struct tls_library, connect) },
	{ 0, 0, "SSL_read", offsetof(struct tls_library, read) },
	{ 0, 0, "SSL_write", offsetof(struct tls_library, write) },
	{ 0, 0, "SSL_get_error", offsetof(struct tls_library, get_error) },
	{ 0, 0, "SSL_pending", offsetof(struct tls_library, pending) },
	{ 0, 0, "SSL_shutdown", offsetof(struct tls_library, shutdown) },
	{ 0, 0, "SSL_get_verify_result", offsetof(struct tls_library, get_verify_result) },
	{ 1, 0, "X509_VERIFY_PARAM_set1_ip_asc", offsetof(struct tls_library, param_set1_ip_asc) },
	{ 1, 0, "X509_verify_cert_error_string", offsetof(struct tls_library, verify_error_string) },
	{ 1, 0, "ERR_get_error", offsetof(struct tls_library, err_get_error) },
	{ 1, 0, "ERR_error_string_n", offsetof(struct tls_library, err_error_string_n) },
	{ 1, 0, "ERR_clear_error", offsetof(struct tls_library, err_clear_error) },
	{ 1, 0, "d2i_X509", offsetof(struct tls_library, d2i_x509) },
	{ 1, 0, "X509_STORE_add_cert", offsetof(struct tls_library, x509_store_add_cert) },
	{ 1, 0, "X509_free", offsetof(struct tls_library, x509_free) },

	/* OpenSSL only. */
	{ 0, 1, "OPENSSL_init_ssl", offsetof(struct tls_library, init_ssl) },
	{ 1, 1, "X509_digest", offsetof(struct tls_library, x509_digest) },
	{ 1, 1, "EVP_sha256", offsetof(struct tls_library, evp_sha256) },
	{ 1, 1, "X509_STORE_CTX_get_current_cert", offsetof(struct tls_library, store_ctx_get_current_cert) },
	{ 1, 1, "X509_STORE_CTX_set_error", offsetof(struct tls_library, store_ctx_set_error) }
};

#ifdef WB_TLS_COMPAT

/* The names libcrypto is looked for under: zedBSD's libcrypto-compat. */
static const char *const tls_crypto_names[] = { "libcrypto-compat.so", NULL };

/* The names libssl is looked for under: zedBSD's libssl-compat. */
static const char *const tls_ssl_names[] = { "libssl-compat.so", NULL };

#else

/* The names libcrypto is looked for under, in order (the package's, then a host's versioned one). */
static const char *const tls_crypto_names[] = { "libcrypto.so", "libcrypto.so.3", NULL };

/* The names libssl is looked for under, in the same order. */
static const char *const tls_ssl_names[] = { "libssl.so", "libssl.so.3", NULL };

#endif

#endif

/*
 * One TLS connection over a connected socket.
 */
struct net_tls {
	void *ssl;
	char reason[NET_TLS_REASON_MAX];
};

/*
 * The state of one enumeration of the system's trusted roots: where the
 * roots go and how many went there.
 */
struct tls_roots {
	void *store;
	size_t added;
};

/* The loaded library, once tls_loaded is set (set under tls_load_lock, then only read). */
static struct tls_library tls_library;

/* 0 before the first attempt to load the library, 1 once loaded, -1 when it cannot be (under tls_load_lock). */
static int tls_loaded;

/* The context every connection is made from (its roots and settings), made with the library under tls_load_lock. */
static void *tls_context;

/* The lock of the loading: the first thread to need TLS loads it, the others wait and find it loaded. */
static struct wb_static_mutex tls_load_lock = WB_STATIC_MUTEX_INIT;

/*
 * Why the library could not be loaded or the context made: written by the
 * one attempt (under tls_load_lock), then only read; every later caller
 * that finds TLS unusable is given this reason.
 */
static char tls_load_reason[NET_TLS_REASON_MAX];

/*
 * The SHA-256 fingerprints of the certificates the system says never to
 * trust, kept for the verification callback: tls_distrusted_count
 * fingerprints of TLS_FINGERPRINT_SIZE bytes each.  Filled once while the
 * context is made (under tls_load_lock), then only read; empty when the
 * system distrusts nothing or the library cannot take a fingerprint.
 */
static unsigned char (*tls_distrusted)[TLS_FINGERPRINT_SIZE];

/* How many fingerprints tls_distrusted holds. */
static size_t tls_distrusted_count;

/* How many fingerprints the memory of tls_distrusted has room for. */
static size_t tls_distrusted_capacity;

/* The CA files added for the tests, loaded into the context when it is made (set before the first fetch). */
static const char *tls_ca_files[TLS_CA_FILES_MAX];

/* How many entries of tls_ca_files are set. */
static size_t tls_ca_file_count;

static int tls_load(char *reason);
static int tls_load_once(void);
static int tls_bind_library(void);
static int tls_refuses_distrusted(void);
static int tls_add_roots(void);
static int tls_add_roots_each(void *context, const unsigned char *der, size_t length, int distrusted);
static int tls_remember_distrusted(const unsigned char *der, size_t length);
static void tls_add_bundle(void);
static int tls_verify(int preverified, void *store_context);
static void tls_set_reason(char *buffer, const char *format, ...) __attribute__((format(printf, 2, 3)));
#ifndef WB_TLS_STATIC
static int tls_find(struct wb_os_library *library, const char *name, void *pointer);
static int tls_find_all(struct wb_os_library *ssl, struct wb_os_library *crypto);
#endif
static int tls_attach(void *ssl, wb_os_socket socket, char *reason);
static int tls_make_context(void);
static int tls_set_host(void *ssl, const char *host);
static int tls_is_address(const char *host);
static void tls_fail(char *reason, const char *what, void *ssl, int result);

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
 * without the library, ENOMEM, or EPROTO.  reason (NET_TLS_REASON_MAX
 * bytes) receives why it failed ("" for no reason); a failure of the
 * connection afterwards is told by net_tls_reason.
 */
int
net_tls_open(
	wb_os_socket socket,
	const char *host,
	char *reason,
	struct net_tls **tls)
{
	struct net_tls *made;
	int result;
	int error;

	/* The library and the shared context. */
	*tls = NULL;
	tls_set_reason(reason, "%s", "");
	error = tls_load(reason);
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
	error = tls_attach(made->ssl, socket, reason);
	if (error != 0) {
		net_tls_close(made);
		return error;
	}
	error = tls_set_host(made->ssl, host);
	if (error != 0) {
		tls_fail(reason, "setup", made->ssl, 0);
		net_tls_close(made);
		return error;
	}

	/* The handshake, which verifies the chain and the name. */
	result = tls_library.connect(made->ssl);
	if (result != 1) {
		tls_fail(reason, "handshake", made->ssl, result);
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
	tls_fail(tls->reason, "read", tls->ssl, result);
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
		tls_fail(tls->reason, "write", tls->ssl, result);
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
	char *reason,
	struct net_tls **tls)
{
	struct net_tls *made;
	int error;

	/* The library and the shared context. */
	*tls = NULL;
	tls_set_reason(reason, "%s", "");
	error = tls_load(reason);
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
	error = tls_attach(made->ssl, socket, reason);
	if (error != 0) {
		net_tls_close(made);
		return error;
	}
	error = tls_set_host(made->ssl, host);
	if (error != 0) {
		tls_fail(reason, "setup", made->ssl, 0);
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
 * failed (net_tls_reason says why).
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
	tls_fail(tls->reason, "handshake", tls->ssl, result);
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
	tls_fail(tls->reason, "read", tls->ssl, result);
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
	tls_fail(tls->reason, "write", tls->ssl, result);
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
 * Tells why a connection failed ("" when it did not, or nothing was said).
 */
const char *
net_tls_reason(
	const struct net_tls *tls)
{
	/* No connection, no reason. */
	if (tls == NULL)
		return "";

	/* The connection's text. */
	return tls->reason;
}

/*
 * Loads the libraries and makes the context, once for all threads; returns
 * 0 or EPROTONOSUPPORT (reason, NET_TLS_REASON_MAX bytes, then says why),
 * or what taking the lock reported (ENOTSUP before the program gave its
 * threads, ENOMEM).
 */
static int
tls_load(
	char *reason)
{
	int error;

	/* The first thread loads; the others wait for it and find the outcome. */
	error = wb_static_mutex_lock(&tls_load_lock);
	if (error != 0) {
		tls_set_reason(reason, "cannot lock the loading of TLS (error %d)", error);
		return error;
	}

	error = tls_load_once();
	if (error != 0)
		tls_set_reason(reason, "%s", tls_load_reason);

	wb_static_mutex_unlock(&tls_load_lock);

	/* TLS cannot be used. */
	if (error != 0)
		return error;

	/* Succeeded: the library and the context are ready. */
	return 0;
}

/*
 * Binds the library and makes the context unless that was tried already
 * (under tls_load_lock).  A library that loaded stays loaded, even when
 * TLS cannot be used after all (libcrypto may have registered its own
 * clean-up at the program's exit).
 */
static int
tls_load_once(void)
{
	int error;

	/* Already done, or already known to be impossible. */
	if (tls_loaded > 0)
		return 0;
	if (tls_loaded < 0) {
		tls_set_reason(tls_load_reason, "cannot load the TLS libraries");
		return EPROTONOSUPPORT;
	}

	/* The functions, then the context. */
	tls_loaded = -1;
	error = tls_bind_library();
	if (error == 0)
		error = tls_make_context();
	if (error != 0)
		return EPROTONOSUPPORT;

	/* Succeeded: https works from now on (the libraries stay loaded). */
	tls_loaded = 1;
	return 0;
}

#ifdef WB_TLS_STATIC

/* Fills the table from the library linked into the program; returns 0 or an errno value. */
static int
tls_bind_library(void)
{
	int error;

	/* Nothing to open: the functions are in the program. */
	error = tls_static_library(&tls_library);
	if (error != 0) {
		tls_set_reason(tls_load_reason, "cannot bind the TLS library");
		return error;
	}

	/* Succeeded: every function is there. */
	return 0;
}

#else

/* Loads libcrypto, then libssl (which needs it), and finds the functions; returns 0 or an errno value. */
static int
tls_bind_library(void)
{
	struct wb_os_library *ssl;
	struct wb_os_library *crypto;
	int error;

	/* libcrypto first (libssl needs it), then libssl. */
	ssl = NULL;
	error = wb_os_library_open(tls_crypto_names, &crypto);
	if (error == 0)
		error = wb_os_library_open(tls_ssl_names, &ssl);
	if (error != 0) {
		tls_set_reason(tls_load_reason, "cannot load %s", TLS_LIBRARY_NAMES);
		return EPROTONOSUPPORT;
	}

	/* The functions. */
	error = tls_find_all(ssl, crypto);
	if (error != 0)
		return error;

	/* Succeeded: the functions are found. */
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
		tls_set_reason(tls_load_reason, "the TLS library has no %s", name);
		return ENOENT;
	}

	/* Stored through the member's address (every member is a function pointer of the same size). */
	memcpy(pointer, &found, sizeof(found));
	return 0;
}

/* Finds every function the connections use (tls_symbols); one marked optional that is missing stays NULL. */
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

		/* An OpenSSL-only function the library lacks: its feature is not used. */
		if (error != 0 && tls_symbols[index].optional)
			continue;
		if (error != 0)
			return error;
	}

	/* Succeeded: every function that is needed is there. */
	return 0;
}

#endif

/*
 * Gives a connection its socket, as the int SSL_set_fd takes.  Returns 0,
 * or EPROTO with the reason kept.
 */
static int
tls_attach(
	void *ssl,
	wb_os_socket socket,
	char *reason)
{
	int descriptor;
	int result;
	int error;

	/* The socket's int. */
	error = wb_os_socket_descriptor(socket, &descriptor);
	if (error != 0) {
		tls_set_reason(reason, "setup: the socket does not fit the TLS library's descriptor (error %d)", error);
		return EPROTO;
	}

	/* The connection reads and writes it. */
	result = tls_library.set_fd(ssl, descriptor);
	if (result != 1) {
		tls_fail(reason, "setup", ssl, 0);
		return EPROTO;
	}

	/* Succeeded: the connection has its socket. */
	return 0;
}

/*
 * Makes the context: TLS 1.2 or later, the peer verified against the
 * system's roots, the program's bundle and the added CA files, and a
 * certificate the system distrusts refused wherever it is in the chain.
 */
static int
tls_make_context(void)
{
	tls_verify_callback callback;
	size_t index;
	long set;
	int result;
	int error;

	/* A library that can says to read no configuration file (a user's openssl.cnf would run for this program). */
	if (tls_library.init_ssl != NULL) {
		result = tls_library.init_ssl(TLS_INIT_NO_LOAD_CONFIG, NULL);
		if (result != 1) {
			tls_set_reason(tls_load_reason, "cannot start OpenSSL without a configuration file");
			return EPROTO;
		}
	}

	/* The client context. */
	tls_context = tls_library.ctx_new(tls_library.client_method());
	if (tls_context == NULL) {
		tls_set_reason(tls_load_reason, "cannot make a TLS context");
		return ENOMEM;
	}

	/* TLS 1.2 or later. */
	set = tls_library.ctx_ctrl(tls_context, TLS_CTRL_SET_MIN_PROTO_VERSION, TLS_VERSION_1_2, NULL);
	if (set != 1) {
		tls_set_reason(tls_load_reason, "the TLS library refuses TLS 1.2 as the least version");
		return EPROTO;
	}

	/* The roots the system keeps for itself, and the certificates it distrusts. */
	error = tls_add_roots();
	if (error != 0)
		return error;

	/* The peer must verify, and a distrusted certificate in its chain fails it. */
	callback = NULL;
	if (tls_distrusted_count != 0)
		callback = tls_verify;
	tls_library.ctx_set_verify(tls_context, TLS_VERIFY_PEER, callback);
	tls_library.ctx_set_options(tls_context, TLS_OP_IGNORE_UNEXPECTED_EOF);

	/* The roots shipped with the program, then the added files. */
	tls_add_bundle();
	for (index = 0; index < tls_ca_file_count; index++) {
		result = tls_library.ctx_load_verify_locations(tls_context, tls_ca_files[index], NULL);
		if (result != 1) {
			tls_set_reason(tls_load_reason, "cannot read the CA file %s", tls_ca_files[index]);
			return EPROTO;
		}
	}

	/* Succeeded: connections can be made. */
	return 0;
}

/*
 * Tells whether the program refuses the certificates the system distrusts
 * itself: the library has OpenSSL's functions of a fingerprint and of the
 * verification's state.  A library without them (libssl-compat) reads the
 * system's stores and refuses those certificates in its default paths.
 */
static int
tls_refuses_distrusted(void)
{
	/* The fingerprint of a certificate. */
	if (tls_library.x509_digest == NULL)
		return 0;
	if (tls_library.evp_sha256 == NULL)
		return 0;

	/* The certificate a verification looks at, and its failure. */
	if (tls_library.store_ctx_get_current_cert == NULL)
		return 0;
	if (tls_library.store_ctx_set_error == NULL)
		return 0;

	/* OpenSSL: the program does it. */
	return 1;
}

/*
 * Gives the context the system's roots.  A library that refuses the
 * distrusted certificates itself reads the system's stores through its
 * default paths, on every system.  Otherwise (OpenSSL) the roots the
 * system hands over go into the store (tls_add_roots_each) and the
 * certificates it distrusts are remembered; a system that keeps its roots
 * where the library reads them itself hands over none, and then the
 * library's default paths are used (a missing file only leaves them empty).
 */
static int
tls_add_roots(void)
{
	struct tls_roots roots;
	int result;
	int error;

	/* The library's own reading: its failure would leave out the system's refusals, so it fails the context. */
	if (!tls_refuses_distrusted()) {
		result = tls_library.ctx_set_default_verify_paths(tls_context);
		if (result != 1) {
			tls_library.err_clear_error();
			tls_set_reason(tls_load_reason, "cannot read the system's trusted certificates");
			return EPROTO;
		}
		return 0;
	}

	/* Each certificate the system trusts goes into the context's store. */
	roots.store = tls_library.ctx_get_cert_store(tls_context);
	roots.added = 0;
	error = wb_os_trust_roots(tls_add_roots_each, &roots);
	if (error != 0) {
		tls_set_reason(tls_load_reason, "cannot read the system's trusted certificates (error %d)", error);
		return EPROTO;
	}

	/* Nothing handed over: the system's own file (or none) through the library's default paths. */
	if (roots.added == 0)
		tls_library.ctx_set_default_verify_paths(tls_context);

	/* Succeeded: the system's roots are in. */
	return 0;
}

/*
 * One certificate of the system's stores: a trusted one goes into the
 * context's store, a distrusted one's fingerprint is remembered.  A trusted
 * certificate that does not parse is skipped.  Returns 0 to go on or an errno
 * value to stop the enumeration.
 */
static int
tls_add_roots_each(
	void *context,
	const unsigned char *der,
	size_t length,
	int distrusted)
{
	struct tls_roots *roots;
	const unsigned char *cursor;
	void *certificate;
	int added;

	/* A certificate the system distrusts: its fingerprint is kept for the verification. */
	roots = context;
	if (distrusted)
		return tls_remember_distrusted(der, length);

	/* A certificate longer than the library's length can describe is not a certificate. */
	if (length > (size_t)LONG_MAX)
		return 0;

	/* The certificate, parsed. */
	cursor = der;
	certificate = tls_library.d2i_x509(NULL, &cursor, (long)length);
	if (certificate == NULL) {
		tls_library.err_clear_error();
		return 0;
	}

	/* Trusted as a root; the store keeps its own reference. */
	added = tls_library.x509_store_add_cert(roots->store, certificate);
	tls_library.x509_free(certificate);
	if (added != 1) {
		tls_library.err_clear_error();
		return 0;
	}

	/* One more root. */
	roots->added++;
	return 0;
}

/*
 * Remembers the SHA-256 fingerprint of a certificate the system distrusts
 * (only with a library that can take one, tls_refuses_distrusted).
 * Returns 0, or an errno value, which ends the enumeration and fails the
 * context: a list that cannot hold every distrusted certificate is not used.
 */
static int
tls_remember_distrusted(
	const unsigned char *der,
	size_t length)
{
	unsigned char (*grown)[TLS_FINGERPRINT_SIZE];
	unsigned char fingerprint[TLS_FINGERPRINT_SIZE];
	const unsigned char *cursor;
	unsigned int printed;
	size_t capacity;
	void *certificate;
	int done;

	/* A certificate longer than the library's length can describe is not a certificate. */
	if (length > (size_t)LONG_MAX)
		return 0;

	/* The certificate, parsed; one that does not parse cannot be in a chain. */
	cursor = der;
	certificate = tls_library.d2i_x509(NULL, &cursor, (long)length);
	if (certificate == NULL) {
		tls_library.err_clear_error();
		return 0;
	}

	/* Its fingerprint. */
	printed = 0;
	done = tls_library.x509_digest(certificate, tls_library.evp_sha256(), fingerprint, &printed);
	tls_library.x509_free(certificate);
	if (done != 1 || printed != TLS_FINGERPRINT_SIZE) {
		tls_library.err_clear_error();
		tls_set_reason(tls_load_reason, "cannot take the fingerprint of a distrusted certificate");
		return EIO;
	}

	/* Room for one more. */
	if (tls_distrusted_count == TLS_DISTRUSTED_MAX) {
		tls_set_reason(tls_load_reason, "the system distrusts more than %u certificates", TLS_DISTRUSTED_MAX);
		return ENOSPC;
	}
	if (tls_distrusted_count == tls_distrusted_capacity) {
		capacity = tls_distrusted_capacity * 2U;
		if (capacity == 0)
			capacity = 16U;
		grown = realloc(tls_distrusted, capacity * sizeof(*tls_distrusted));
		if (grown == NULL)
			return ENOMEM;
		tls_distrusted = grown;
		tls_distrusted_capacity = capacity;
	}

	/* Kept. */
	memcpy(tls_distrusted[tls_distrusted_count], fingerprint, TLS_FINGERPRINT_SIZE);
	tls_distrusted_count++;
	return 0;
}

/*
 * Adds the bundle of roots shipped with the program (the ca-certificates
 * package's PEM file, in the program's data directory) to the context.  A
 * program without the file simply has the roots it already has.
 */
static void
tls_add_bundle(void)
{
	char directory[TLS_BUNDLE_PATH_MAX];
	char path[TLS_BUNDLE_PATH_MAX];
	int length;
	int error;

	/* The data directory. */
	error = wb_os_data_directory(directory, sizeof(directory));
	if (error != 0)
		return;

	/* The bundle's path in it. */
	length = snprintf(path, sizeof(path), "%s%s", directory, TLS_BUNDLE_NAME);
	if (length < 0 || (size_t)length >= sizeof(path))
		return;

	/* Read when it is there; a file that is missing or unreadable leaves the roots as they are. */
	tls_library.ctx_load_verify_locations(tls_context, path, NULL);
	tls_library.err_clear_error();
}

/*
 * The verification callback, called for each certificate of a chain: a
 * certificate whose fingerprint the system distrusts fails the verification
 * (reason: certificate rejected), wherever it is in the chain and even when
 * it was also found as a root.  Returns 1 to go on, 0 to fail.
 */
static int
tls_verify(
	int preverified,
	void *store_context)
{
	unsigned char fingerprint[TLS_FINGERPRINT_SIZE];
	unsigned int printed;
	void *certificate;
	size_t index;
	int done;

	/* A certificate the library already refused stays refused. */
	if (!preverified)
		return 0;

	/* The certificate being looked at. */
	certificate = tls_library.store_ctx_get_current_cert(store_context);
	if (certificate == NULL)
		return 1;

	/* Its fingerprint; one that cannot be taken cannot be shown not to be distrusted. */
	printed = 0;
	done = tls_library.x509_digest(certificate, tls_library.evp_sha256(), fingerprint, &printed);
	if (done != 1 || printed != TLS_FINGERPRINT_SIZE) {
		tls_library.store_ctx_set_error(store_context, TLS_CERT_REJECTED);
		return 0;
	}

	/* A distrusted fingerprint fails the chain. */
	for (index = 0; index < tls_distrusted_count; index++) {
		if (memcmp(tls_distrusted[index], fingerprint, TLS_FINGERPRINT_SIZE) == 0) {
			tls_library.store_ctx_set_error(store_context, TLS_CERT_REJECTED);
			return 0;
		}
	}

	/* Not distrusted. */
	return 1;
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

/* Keeps the reason of a failure in a buffer: the certificate's verification, or OpenSSL's error queue. */
static void
tls_fail(
	char *reason,
	const char *what,
	void *ssl,
	int result)
{
	const char *described;
	unsigned long queued;
	long verified;
	int kind;
	char text[TLS_ERROR_MAX / 2U];

	/* A certificate that did not verify says so. */
	verified = tls_library.get_verify_result(ssl);
	if (verified != TLS_VERIFY_OK) {
		described = tls_library.verify_error_string(verified);
		tls_set_reason(reason, "%s: certificate verify failed: %s", what, described);
		tls_library.err_clear_error();
		return;
	}

	/* Otherwise the first queued error, or the kind of failure. */
	queued = tls_library.err_get_error();
	if (queued != 0) {
		tls_library.err_error_string_n(queued, text, sizeof(text));
		tls_set_reason(reason, "%s: %s", what, text);
		tls_library.err_clear_error();
		return;
	}

	/* No queued error: the connection ended or failed underneath. */
	kind = TLS_ERROR_SSL;
	if (result <= 0)
		kind = tls_library.get_error(ssl, result);
	tls_set_reason(reason, "%s: failed (TLS error %d)", what, kind);
}

/* Keeps the reason of a failure in a buffer of NET_TLS_REASON_MAX bytes (a reason that does not fit is cut; a NULL buffer keeps nothing). */
static void
tls_set_reason(
	char *buffer,
	const char *format,
	...)
{
	va_list arguments;

	/* A caller that keeps no reason. */
	if (buffer == NULL)
		return;

	/* The text. */
	va_start(arguments, format);
	vsnprintf(buffer, NET_TLS_REASON_MAX, format, arguments);
	va_end(arguments);
}
