/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Mail's TLS (mail.h): libssl-compat and libcrypto-compat (zedBSD's TLS
 * libraries, with OpenSSL 3.5's API) called directly; the program links
 * the two.
 *
 * A connection is TLS 1.2 or later and verifies the server's chain
 * against the default roots (the ca-certificates package) and any file
 * added with ml_tls_add_ca_file (the host tests' own CA), and the
 * certificate's name against the server's host.  A failure is EPROTO,
 * its reason kept for ml_tls_error.
 *
 * The handshake runs to its end whatever the verification
 * says, and the outcome is decided after it, before a byte is sent: a
 * certificate that does not verify is ML_ERROR_UNTRUSTED, with its
 * SHA-256 fingerprint kept for ml_tls_fingerprint so that the user can be
 * asked whether to trust it (a self-signed server of one's own); a
 * certificate whose fingerprint is the server's pin is taken.
 *
 * Only the connection thread of Mail (sync.c) makes connections; the
 * context is made under a lock so that a second thread would not make it
 * twice.
 */

#include "mail.h"

#include <errno.h>
#include <limits.h>
#include <keiland/keiland_thread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/crypto/evp.h>
#include <compat/ssl/ssl.h>

/* The most extra CA files the tests may add. */
#define TLS_CA_FILES_MAX	4U

/* The length of a SHA-256 digest, in bytes. */
#define TLS_DIGEST_BYTES	32U

/* 0 before the first attempt to make the context, 1 once made, -1 when it cannot be (under tls_lock). */
static int tls_loaded;

/* The context every connection is made from (its roots and settings). */
static SSL_CTX *tls_context;

/* The CA files added by the tests, loaded into the context when it is made (set before the first connection). */
static const char *tls_ca_files[TLS_CA_FILES_MAX];

/* How many entries of tls_ca_files are set. */
static size_t tls_ca_file_count;

/* The reason of the last failure, for ml_tls_error (one connection thread writes it). */
static char tls_reason[ML_TEXT_MAX];

/*
 * The SHA-256 fingerprint (hex) of the last certificate that did not
 * verify, for ml_tls_fingerprint; empty after a connection that did.  The
 * connection thread writes it, and the window reads it from the result
 * the thread copies it into.
 */
static char tls_fingerprint[ML_PIN_MAX];

/* Keeps two threads from making the context at once; held only while it is made. */
static struct kl_mutex tls_lock = KL_MUTEX_INIT;

static int tls_load(void);
static int tls_load_locked(void);
static int tls_make_context(void);
static int tls_set_host(SSL *ssl, const char *host);
static void tls_fail(const char *what, SSL *ssl, int result);
static int tls_verify_later(int preverified, X509_STORE_CTX *store);
static int tls_decide(SSL *ssl, const char *pin);
static int tls_digest(SSL *ssl, char *text, size_t size);

/*
 * Trusts the CA certificates of a PEM file besides the default roots (the
 * host tests' own CA).  Must come before the first connection.
 */
int
ml_tls_add_ca_file(
	const char *path)
{
	/* No room for another. */
	if (tls_ca_file_count == TLS_CA_FILES_MAX)
		return ENOSPC;

	/* Kept until the context is made. */
	tls_ca_files[tls_ca_file_count] = path;
	tls_ca_file_count++;

	/* Succeeded: the file is kept for the context. */
	return 0;
}

/*
 * Starts TLS on a connected socket to a host: the handshake, the chain and
 * the name, or else the pin (the fingerprint of a certificate the user
 * trusts; NULL or empty for none).  Returns 0 with the connection's state,
 * EPROTONOSUPPORT when TLS cannot be set up, ENOMEM, EPROTO, or
 * ML_ERROR_UNTRUSTED (ml_tls_error says why, ml_tls_fingerprint gives the
 * certificate's fingerprint).
 */
int
ml_tls_open(
	intptr_t socket,
	const char *host,
	const char *pin,
	void **tls)
{
	SSL *ssl;
	int result;
	int error;

	/* The shared context. */
	*tls = NULL;
	tls_reason[0] = '\0';
	tls_fingerprint[0] = '\0';
	error = tls_load();
	if (error != 0)
		return error;

	/* The connection's state. */
	ERR_clear_error();
	ssl = SSL_new(tls_context);
	if (ssl == NULL)
		return ENOMEM;

	/* The socket's own handle, which the library reads and writes itself (on the network's thread, where SIGPIPE is blocked). */
	result = SSL_set_fd(ssl, (int)socket);
	if (result != 1) {
		tls_fail("setup", ssl, 0);
		SSL_free(ssl);
		return EPROTO;
	}

	/* The name sent (SNI) and checked. */
	error = tls_set_host(ssl, host);
	if (error != 0) {
		tls_fail("setup", ssl, 0);
		SSL_free(ssl);
		return error;
	}

	/* The handshake, which verifies the chain and the name but leaves the outcome to tls_decide. */
	result = SSL_connect(ssl);
	if (result != 1) {
		tls_fail("handshake", ssl, result);
		SSL_free(ssl);
		return EPROTO;
	}

	/* The certificate verified, or pinned; else nothing is sent over the connection. */
	error = tls_decide(ssl, pin);
	if (error != 0) {
		(void)SSL_shutdown(ssl);
		SSL_free(ssl);
		return error;
	}

	/* Succeeded: the connection is secure. */
	*tls = ssl;
	return 0;
}

/*
 * Reads what the server sent, up to length bytes: *received is 0 at the
 * end of the connection.  Returns 0 or an errno value.
 */
int
ml_tls_read(
	void *tls,
	unsigned char *bytes,
	size_t length,
	size_t *received)
{
	SSL *ssl;
	int count;
	int result;
	int reason;

	/* One read (the library takes an int). */
	ssl = tls;
	*received = 0;
	count = (int)length;
	if (length > INT_MAX)
		count = INT_MAX;

	/* Until a record gives bytes, the end, or a failure. */
	for (;;) {
		result = SSL_read(ssl, bytes, count);
		if (result > 0) {
			*received = (size_t)result;
			return 0;
		}

		/* A record that is not complete yet is read again. */
		reason = SSL_get_error(ssl, result);
		if (reason != SSL_ERROR_WANT_READ && reason != SSL_ERROR_WANT_WRITE)
			break;
	}

	/* The server's close (or a close without close_notify, which the context allows). */
	if (reason == SSL_ERROR_ZERO_RETURN)
		return 0;

	/* A failure of the socket underneath. */
	if (reason == SSL_ERROR_SYSCALL && errno != 0)
		return errno;

	/* Anything else is a broken connection. */
	tls_fail("read", ssl, result);
	return EPROTO;
}

/*
 * Sends all of a buffer.  Returns 0 or an errno value.
 */
int
ml_tls_write(
	void *tls,
	const unsigned char *bytes,
	size_t length)
{
	SSL *ssl;
	size_t done;
	int count;
	int result;
	int reason;

	/* Until everything is sent. */
	ssl = tls;
	done = 0;
	while (done < length) {
		/* One write (the library takes an int). */
		count = (int)(length - done);
		if (length - done > INT_MAX)
			count = INT_MAX;
		result = SSL_write(ssl, bytes + done, count);
		if (result > 0) {
			done += (size_t)result;
			continue;
		}

		/* A write that must wait is tried again. */
		reason = SSL_get_error(ssl, result);
		if (reason == SSL_ERROR_WANT_READ || reason == SSL_ERROR_WANT_WRITE)
			continue;

		/* Anything else ends the connection. */
		tls_fail("write", ssl, result);
		return EPROTO;
	}

	/* Succeeded: everything is sent. */
	return 0;
}

/*
 * Tells whether bytes already decrypted wait to be read (then the socket
 * need not be readable for the next read).
 */
int
ml_tls_pending(
	void *tls)
{
	const SSL *ssl;
	int pending;

	/* The bytes the library holds. */
	ssl = tls;
	pending = SSL_pending(ssl);
	if (pending > 0)
		return 1;

	/* Nothing held. */
	return 0;
}

/*
 * Ends a connection (close_notify once, without waiting for the server's)
 * and frees its state; the socket stays open for the caller to close.
 */
void
ml_tls_close(
	void *tls)
{
	SSL *ssl;

	/* Nothing to end. */
	if (tls == NULL)
		return;

	/* The close, then the state. */
	ssl = tls;
	(void)SSL_shutdown(ssl);
	SSL_free(ssl);
}

/*
 * Reports the reason of the last TLS failure, or "".
 */
const char *
ml_tls_error(void)
{
	/* The text kept by tls_fail. */
	return tls_reason;
}

/*
 * Reports the SHA-256 fingerprint (hex) of the certificate of the last
 * connection that failed with ML_ERROR_UNTRUSTED, or "".
 */
const char *
ml_tls_fingerprint(void)
{
	/* The text kept by tls_decide. */
	return tls_fingerprint;
}

/* Makes the context, once; returns 0 or EPROTONOSUPPORT. */
static int
tls_load(void)
{
	int error;

	/* One maker at a time. */
	kl_mutex_lock(&tls_lock);

	/* Makes the context, or finds the outcome of the first attempt. */
	error = tls_load_locked();

	kl_mutex_unlock(&tls_lock);

	/* Reports why TLS cannot be used. */
	if (error != 0)
		return error;

	/* Succeeded: connections can be made. */
	return 0;
}

/* Makes the context under tls_lock. */
static int
tls_load_locked(void)
{
	int error;

	/* Already done. */
	if (tls_loaded > 0)
		return 0;

	/* Already known to be impossible. */
	if (tls_loaded < 0) {
		snprintf(tls_reason, sizeof(tls_reason), "cannot set up TLS");
		return EPROTONOSUPPORT;
	}

	/* The context. */
	tls_loaded = -1;
	error = tls_make_context();
	if (error != 0)
		return EPROTONOSUPPORT;

	/* Succeeded: the context stays for the program's life. */
	tls_loaded = 1;
	return 0;
}

/* Makes the context: TLS 1.2 or later, the peer verified against the default roots and the added CA files. */
static int
tls_make_context(void)
{
	size_t index;
	int result;

	/* The client context. */
	tls_context = SSL_CTX_new(TLS_client_method());
	if (tls_context == NULL) {
		snprintf(tls_reason, sizeof(tls_reason), "cannot make a TLS context");
		return ENOMEM;
	}

	/* TLS 1.2 or later. */
	result = (int)SSL_CTX_set_min_proto_version(tls_context, TLS1_2_VERSION);
	if (result != 1) {
		snprintf(tls_reason, sizeof(tls_reason), "the TLS library refuses TLS 1.2 as the least version");
		return EPROTO;
	}

	/* The peer is verified, its outcome decided after the handshake (tls_decide); a close without close_notify reads as the end. */
	SSL_CTX_set_verify(tls_context, SSL_VERIFY_PEER, tls_verify_later);
	(void)SSL_CTX_set_options(tls_context, SSL_OP_IGNORE_UNEXPECTED_EOF);

	/* The default roots (a missing file only leaves them empty). */
	(void)SSL_CTX_set_default_verify_paths(tls_context);

	/* The added CA files. */
	for (index = 0; index < tls_ca_file_count; index++) {
		result = SSL_CTX_load_verify_locations(tls_context, tls_ca_files[index], NULL);
		if (result != 1) {
			snprintf(tls_reason, sizeof(tls_reason), "cannot read the CA file %s", tls_ca_files[index]);
			return EPROTO;
		}
	}

	/* Succeeded: connections can be made. */
	return 0;
}

/* Sets the name sent to the server (SNI) and matched against its certificate. */
static int
tls_set_host(
	SSL *ssl,
	const char *host)
{
	long sent;
	int result;

	/* Sent to the server. */
	sent = SSL_set_tlsext_host_name(ssl, host);
	if (sent != 1)
		return EPROTO;

	/* Matched against the certificate. */
	result = SSL_set1_host(ssl, host);
	if (result != 1)
		return EPROTO;

	/* Succeeded: the name is set. */
	return 0;
}

/* Keeps the reason of a failure: the certificate's verification, or the library's error queue. */
static void
tls_fail(
	const char *what,
	SSL *ssl,
	int result)
{
	const char *described;
	unsigned long queued;
	long verified;
	int reason;
	char text[ML_TEXT_MAX / 2U];

	/* A certificate that did not verify says so. */
	verified = SSL_get_verify_result(ssl);
	if (verified != X509_V_OK) {
		described = X509_verify_cert_error_string(verified);
		snprintf(tls_reason, sizeof(tls_reason), "%s: certificate verify failed: %s", what, described);
		ERR_clear_error();
		return;
	}

	/* Otherwise the first queued error. */
	queued = ERR_get_error();
	if (queued != 0) {
		ERR_error_string_n(queued, text, sizeof(text));
		snprintf(tls_reason, sizeof(tls_reason), "%s: %s", what, text);
		ERR_clear_error();
		return;
	}

	/* No queued error: the connection ended or failed underneath. */
	reason = SSL_get_error(ssl, result);
	snprintf(tls_reason, sizeof(tls_reason), "%s: failed (SSL error %d)", what, reason);
}

/*
 * Lets the handshake go on whatever the verification of the chain says:
 * the outcome, kept by the library, is decided after it (tls_decide), so that
 * the certificate of a server that does not verify can be shown.
 */
static int
tls_verify_later(
	int preverified,
	X509_STORE_CTX *store)
{
	UNUSED_PARAMETER(preverified);
	UNUSED_PARAMETER(store);

	/* Go on. */
	return 1;
}

/*
 * Decides a finished handshake: a chain and a name that verified, or a
 * certificate whose fingerprint is the pin.  Returns 0, or
 * ML_ERROR_UNTRUSTED with the reason and the fingerprint kept.
 */
static int
tls_decide(
	SSL *ssl,
	const char *pin)
{
	const char *described;
	long verified;
	int error;
	int same;

	/* Verified. */
	verified = SSL_get_verify_result(ssl);
	if (verified == X509_V_OK)
		return 0;

	/* Not verified: the certificate's fingerprint, for the pin and for the user. */
	described = X509_verify_cert_error_string(verified);
	snprintf(tls_reason, sizeof(tls_reason), "certificate verify failed: %s", described);
	ERR_clear_error();
	error = tls_digest(ssl, tls_fingerprint, sizeof(tls_fingerprint));
	if (error != 0)
		return ML_ERROR_UNTRUSTED;

	/* A certificate the user trusts is taken. */
	if (pin != NULL && pin[0] != '\0') {
		same = strcmp(pin, tls_fingerprint);
		if (same == 0) {
			tls_reason[0] = '\0';
			tls_fingerprint[0] = '\0';
			return 0;
		}
	}

	/* Not trusted. */
	return ML_ERROR_UNTRUSTED;
}

/* Writes the SHA-256 fingerprint of the peer's certificate in hex (64 small letters and digits). */
static int
tls_digest(
	SSL *ssl,
	char *text,
	size_t size)
{
	static const char digits[] = "0123456789abcdef";
	unsigned char bytes[TLS_DIGEST_BYTES];
	unsigned int length;
	X509 *certificate;
	size_t index;
	int result;

	/* Nothing yet. */
	text[0] = '\0';
	if (size < TLS_DIGEST_BYTES * 2U + 1U)
		return ENOSPC;

	/* The certificate the server showed. */
	certificate = SSL_get1_peer_certificate(ssl);
	if (certificate == NULL)
		return ENOENT;

	/* Its digest. */
	length = 0;
	result = X509_digest(certificate, EVP_sha256(), bytes, &length);
	X509_free(certificate);
	if (result != 1 || length != TLS_DIGEST_BYTES)
		return EPROTO;

	/* In hex. */
	for (index = 0; index < TLS_DIGEST_BYTES; index++) {
		text[index * 2U] = digits[bytes[index] >> 4];
		text[index * 2U + 1U] = digits[bytes[index] & 0x0fU];
	}

	/* Its end. */
	text[TLS_DIGEST_BYTES * 2U] = '\0';

	/* Succeeded: the fingerprint is written. */
	return 0;
}
