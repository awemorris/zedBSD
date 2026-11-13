/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * TLS for https through libssl-compat and libcrypto-compat, zedBSD's own
 * TLS library (the interface and the numeric values are OpenSSL 3.5's), called
 * directly.  On zedBSD libbrowser.so links the two shared libraries; on
 * Windows the program links them statically.
 *
 * A connection is TLS 1.2 or later, verifies the server's chain against the
 * roots, and checks the certificate's name or address against the URL's
 * host.  The roots are the library's default paths (zedBSD: the
 * ca-certificates package's /etc/ssl/cert.pem; Windows: the system's ROOT
 * stores and the cert.pem beside the .exe; a certificate the system
 * distrusts is refused by the library anywhere in the chain), the bundle of
 * roots shipped with the program, and any file added with
 * net_tls_add_ca_file (the tests' own CA).  A failure is EPROTO, with its
 * reason kept in the caller's buffer (net_tls_open, net_tls_start) or in the
 * connection (net_tls_reason).
 *
 * Two threads use TLS: the page's (the blocking fetches) and the loader's
 * network thread.  The context is made once, under tls_load_lock; a
 * connection is used by one thread at a time; and each thread keeps the
 * reason of its own last failure.
 */

#include "net/net.h"
#include "base/thread.h"
#include "base/os-system.h"

#include <errno.h>
#include <stdarg.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/ssl/ssl.h>


/* The longest text of the library's error kept in a reason. */
#define TLS_ERROR_MAX		NET_TLS_REASON_MAX

/* The most extra CA files the tests may add. */
#define TLS_CA_FILES_MAX	4U

/* The longest path of the bundle of roots shipped with the program. */
#define TLS_BUNDLE_PATH_MAX	1024U

/* The bundle's path below the program's data directory. */
#define TLS_BUNDLE_NAME		"/certs/ca-certificates.pem"

/*
 * One TLS connection over a connected socket.
 */
struct net_tls {
	SSL *ssl;
	char reason[NET_TLS_REASON_MAX];
};

/* 0 before the first attempt to make the context, 1 once made, -1 when it cannot be (under tls_load_lock). */
static int tls_loaded;

/* The context every connection is made from (its roots and settings), made once under tls_load_lock. */
static SSL_CTX *tls_context;

/* The lock of the setup: the first thread to need TLS makes the context, the others wait and find it made. */
static struct wb_static_mutex tls_load_lock = WB_STATIC_MUTEX_INIT;

/*
 * Why the context could not be made: written by the one attempt (under
 * tls_load_lock), then only read; every later caller that finds TLS unusable
 * is given this reason.
 */
static char tls_load_reason[NET_TLS_REASON_MAX];

/* The CA files added for the tests, loaded into the context when it is made (set before the first fetch). */
static const char *tls_ca_files[TLS_CA_FILES_MAX];

/* How many entries of tls_ca_files are set. */
static size_t tls_ca_file_count;

static int tls_load(char *reason);
static int tls_load_once(void);
static int tls_add_roots(void);
static void tls_add_bundle(void);
static void tls_set_reason(char *buffer, const char *format, ...) __attribute__((format(printf, 2, 3)));
static int tls_attach(SSL *ssl, wb_os_socket socket, char *reason);
static int tls_make_context(void);
static int tls_set_host(SSL *ssl, const char *host);
static int tls_is_address(const char *host);
static void tls_fail(char *reason, const char *what, SSL *ssl, int result);

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
 * when TLS cannot be set up, ENOMEM, or EPROTO.  reason (NET_TLS_REASON_MAX
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

	/* The shared context. */
	*tls = NULL;
	tls_set_reason(reason, "%s", "");
	error = tls_load(reason);
	if (error != 0)
		return error;

	/* The connection's state. */
	made = calloc(1, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	ERR_clear_error();
	made->ssl = SSL_new(tls_context);
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
	result = SSL_connect(made->ssl);
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

	/* One read (the library takes an int). */
	*received = 0;
	count = (int)length;
	if (length > INT_MAX)
		count = INT_MAX;
	for (;;) {
		result = SSL_read(tls->ssl, bytes, count);
		if (result > 0) {
			*received = (size_t)result;
			return 0;
		}

		/* A record that is not complete yet is read again. */
		reason = SSL_get_error(tls->ssl, result);
		if (reason == SSL_ERROR_WANT_READ || reason == SSL_ERROR_WANT_WRITE)
			continue;
		break;
	}

	/* The server's close (or a close without close_notify, which the context allows). */
	if (reason == SSL_ERROR_ZERO_RETURN)
		return 0;
	if (reason == SSL_ERROR_SYSCALL) {
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
		result = SSL_write(tls->ssl, bytes + done, count);
		if (result > 0)
			continue;

		/* A write that must wait is tried again. */
		reason = SSL_get_error(tls->ssl, result);
		if (reason == SSL_ERROR_WANT_READ || reason == SSL_ERROR_WANT_WRITE) {
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

	/* The shared context. */
	*tls = NULL;
	tls_set_reason(reason, "%s", "");
	error = tls_load(reason);
	if (error != 0)
		return error;

	/* The connection's state. */
	made = calloc(1, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	ERR_clear_error();
	made->ssl = SSL_new(tls_context);
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
	result = SSL_connect(tls->ssl);
	if (result == 1)
		return 0;

	/* A handshake that waits for the socket. */
	reason = SSL_get_error(tls->ssl, result);
	if (reason == SSL_ERROR_WANT_READ) {
		*wants = WB_OS_POLL_READABLE;
		return EAGAIN;
	}

	/* Or for the socket to take more. */
	if (reason == SSL_ERROR_WANT_WRITE) {
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

	/* One read (the library takes an int). */
	*received = 0;
	*wants = 0;
	count = (int)length;
	if (length > INT_MAX)
		count = INT_MAX;
	result = SSL_read(tls->ssl, bytes, count);
	if (result > 0) {
		*received = (size_t)result;
		return 0;
	}

	/* A record that is not complete yet waits for the socket. */
	reason = SSL_get_error(tls->ssl, result);
	if (reason == SSL_ERROR_WANT_READ) {
		*wants = WB_OS_POLL_READABLE;
		return EAGAIN;
	}

	/* Or for the socket to take more. */
	if (reason == SSL_ERROR_WANT_WRITE) {
		*wants = WB_OS_POLL_WRITABLE;
		return EAGAIN;
	}

	/* The server's close (or a close without close_notify, which the context allows). */
	if (reason == SSL_ERROR_ZERO_RETURN)
		return 0;
	if (reason == SSL_ERROR_SYSCALL) {
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
	result = SSL_write(tls->ssl, bytes, count);
	if (result > 0) {
		*sent = (size_t)result;
		return 0;
	}

	/* A write that waits for the socket. */
	reason = SSL_get_error(tls->ssl, result);
	if (reason == SSL_ERROR_WANT_READ) {
		*wants = WB_OS_POLL_READABLE;
		return EAGAIN;
	}

	/* Or for the socket to take more. */
	if (reason == SSL_ERROR_WANT_WRITE) {
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

	/* The bytes the library holds. */
	pending = SSL_pending(tls->ssl);
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
	SSL_shutdown(tls->ssl);
	SSL_free(tls->ssl);
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
 * Makes the context once for all threads; returns 0 or EPROTONOSUPPORT
 * (reason, NET_TLS_REASON_MAX bytes, then says why), or what taking the
 * lock reported (ENOTSUP before the program gave its threads, ENOMEM).
 */
static int
tls_load(
	char *reason)
{
	int error;

	/* The first thread makes it; the others wait for it and find the outcome. */
	error = wb_static_mutex_lock(&tls_load_lock);
	if (error != 0) {
		tls_set_reason(reason, "cannot lock the setup of TLS (error %d)", error);
		return error;
	}

	error = tls_load_once();
	if (error != 0)
		tls_set_reason(reason, "%s", tls_load_reason);

	wb_static_mutex_unlock(&tls_load_lock);

	/* TLS cannot be used. */
	if (error != 0)
		return error;

	/* Succeeded: the context is ready. */
	return 0;
}

/*
 * Makes the context unless that was tried already (under tls_load_lock).
 */
static int
tls_load_once(void)
{
	int error;

	/* Already done, or already known to be impossible. */
	if (tls_loaded > 0)
		return 0;
	if (tls_loaded < 0) {
		tls_set_reason(tls_load_reason, "cannot set up TLS");
		return EPROTONOSUPPORT;
	}

	/* The context. */
	tls_loaded = -1;
	error = tls_make_context();
	if (error != 0)
		return EPROTONOSUPPORT;

	/* Succeeded: https works from now on. */
	tls_loaded = 1;
	return 0;
}

/*
 * Gives a connection its socket, as the int SSL_set_fd takes.  Returns 0,
 * or EPROTO with the reason kept.
 */
static int
tls_attach(
	SSL *ssl,
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
	result = SSL_set_fd(ssl, descriptor);
	if (result != 1) {
		tls_fail(reason, "setup", ssl, 0);
		return EPROTO;
	}

	/* Succeeded: the connection has its socket. */
	return 0;
}

/*
 * Makes the context: TLS 1.2 or later, the peer verified against the
 * library's default roots, the program's bundle and the added CA files.
 */
static int
tls_make_context(void)
{
	size_t index;
	long set;
	int result;
	int error;

	/* The client context. */
	tls_context = SSL_CTX_new(TLS_client_method());
	if (tls_context == NULL) {
		tls_set_reason(tls_load_reason, "cannot make a TLS context");
		return ENOMEM;
	}

	/* TLS 1.2 or later. */
	set = SSL_CTX_set_min_proto_version(tls_context, TLS1_2_VERSION);
	if (set != 1) {
		tls_set_reason(tls_load_reason, "the TLS library refuses TLS 1.2 as the least version");
		return EPROTO;
	}

	/* The roots the library keeps for the system. */
	error = tls_add_roots();
	if (error != 0)
		return error;

	/* The peer must verify, and a close without close_notify is the server's close. */
	SSL_CTX_set_verify(tls_context, SSL_VERIFY_PEER, NULL);
	SSL_CTX_set_options(tls_context, SSL_OP_IGNORE_UNEXPECTED_EOF);

	/* The roots shipped with the program, then the added files. */
	tls_add_bundle();
	for (index = 0; index < tls_ca_file_count; index++) {
		result = SSL_CTX_load_verify_locations(tls_context, tls_ca_files[index], NULL);
		if (result != 1) {
			tls_set_reason(tls_load_reason, "cannot read the CA file %s", tls_ca_files[index]);
			return EPROTO;
		}
	}

	/* Succeeded: connections can be made. */
	return 0;
}

/*
 * Gives the context the system's roots through the library's default paths.
 * The library's failure to read them fails the context, so that a root the
 * system trusts (or a refusal it makes) is never silently left out.
 */
static int
tls_add_roots(void)
{
	int result;

	/* The default paths. */
	result = SSL_CTX_set_default_verify_paths(tls_context);
	if (result != 1) {
		ERR_clear_error();
		tls_set_reason(tls_load_reason, "cannot read the system's trusted certificates");
		return EPROTO;
	}

	/* Succeeded: the system's roots are in. */
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
	SSL_CTX_load_verify_locations(tls_context, path, NULL);
	ERR_clear_error();
}

/* Sets the name sent and checked: a name goes in SNI and is matched; an address is matched only. */
static int
tls_set_host(
	SSL *ssl,
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
		result = X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), address);
		if (result != 1)
			return EPROTO;
		return 0;
	}

	/* A name: sent to the server, and matched against the certificate. */
	sent = SSL_set_tlsext_host_name(ssl, host);
	if (sent != 1)
		return EPROTO;
	result = SSL_set1_host(ssl, host);
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

/* Keeps the reason of a failure in a buffer: the certificate's verification, or the library's error queue. */
static void
tls_fail(
	char *reason,
	const char *what,
	SSL *ssl,
	int result)
{
	const char *described;
	unsigned long queued;
	long verified;
	int kind;
	char text[TLS_ERROR_MAX / 2U];

	/* A certificate that did not verify says so. */
	verified = SSL_get_verify_result(ssl);
	if (verified != X509_V_OK) {
		described = X509_verify_cert_error_string(verified);
		tls_set_reason(reason, "%s: certificate verify failed: %s", what, described);
		ERR_clear_error();
		return;
	}

	/* Otherwise the first queued error, or the kind of failure. */
	queued = ERR_get_error();
	if (queued != 0) {
		ERR_error_string_n(queued, text, sizeof(text));
		tls_set_reason(reason, "%s: %s", what, text);
		ERR_clear_error();
		return;
	}

	/* No queued error: the connection ended or failed underneath. */
	kind = SSL_ERROR_SSL;
	if (result <= 0)
		kind = SSL_get_error(ssl, result);
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
