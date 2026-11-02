/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The public SSL_ and SSL_CTX_ functions.
 *
 * A context keeps the settings of the connections made from it; an SSL
 * reads them when its handshake starts (the first SSL_connect, SSL_read,
 * SSL_write), so a change made to the context between SSL_new and that
 * first call is seen.  The version bounds set on the SSL itself (SSL_ctrl)
 * are kept in the SSL's connection configuration until then and override
 * the context's.
 *
 * Every public function that fails queues exactly one error of the SSL
 * library (or of the X509 or system library for a file or a socket) and
 * returns the failure value of its interface; a function that succeeds,
 * and a call that only has to wait for the socket, queue nothing.  What a
 * failed SSL_connect, SSL_read, SSL_write or SSL_shutdown was is stored in
 * the SSL for SSL_get_error, which does not look at the queue again.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "sslc-api.h"

/* The kinds of call whose failure SSL_get_error classifies, which decide how an end of the stream and a closed connection read. */
enum ssl_operation {
	SSL_OPERATION_HANDSHAKE = 1,
	SSL_OPERATION_READ = 2,
	SSL_OPERATION_WRITE = 3,
	SSL_OPERATION_SHUTDOWN = 4
};

/*
 * The reasons libssl-compat queues, in the order of sslc_reason_texts: the
 * SSL_R_ reasons of <compat/ssl/ssl.h>, then the alerts a peer can send,
 * each as SSL_AD_REASON_OFFSET plus the alert's description.
 * Values referenced: OpenSSL 3.5.8 include/openssl/sslerr.h (SSL_R_*),
 * include/openssl/ssl.h.in (SSL_AD_REASON_OFFSET); RFC 8446 Appendix B.2
 * and RFC 5246 7.2.2 (the alert descriptions: close_notify 0,
 * unexpected_message 10, bad_record_mac 20, record_overflow 22,
 * handshake_failure 40, bad_certificate 42, unsupported_certificate 43,
 * certificate_revoked 44, certificate_expired 45, certificate_unknown 46,
 * illegal_parameter 47, unknown_ca 48, access_denied 49, decode_error 50,
 * decrypt_error 51, protocol_version 70, insufficient_security 71,
 * internal_error 80, inappropriate_fallback 86, user_canceled 90,
 * no_renegotiation 100, missing_extension 109, unsupported_extension 110,
 * unrecognized_name 112, bad_certificate_status_response 113,
 * unknown_psk_identity 115, certificate_required 116,
 * no_application_protocol 120).
 */
const unsigned sslc_reason_numbers[] = {
	SSL_R_BAD_ECPOINT,
	SSL_R_BAD_EXTENSION,
	SSL_R_BAD_HANDSHAKE_LENGTH,
	SSL_R_BAD_KEY_SHARE,
	SSL_R_BAD_LENGTH,
	SSL_R_BAD_SIGNATURE,
	SSL_R_BAD_WRITE_RETRY,
	SSL_R_CERTIFICATE_VERIFY_FAILED,
	SSL_R_DECRYPTION_FAILED_OR_BAD_RECORD_MAC,
	SSL_R_DIGEST_CHECK_FAILED,
	SSL_R_EXCESSIVE_MESSAGE_SIZE,
	SSL_R_HTTP_REQUEST,
	SSL_R_LENGTH_MISMATCH,
	SSL_R_NO_PROTOCOLS_AVAILABLE,
	SSL_R_NO_SHARED_CIPHER,
	SSL_R_NO_SUITABLE_KEY_SHARE,
	SSL_R_PACKET_LENGTH_TOO_LONG,
	SSL_R_PROTOCOL_IS_SHUTDOWN,
	SSL_R_RECORD_LENGTH_MISMATCH,
	SSL_R_SHUTDOWN_WHILE_IN_INIT,
	SSL_R_SSL_HANDSHAKE_FAILURE,
	SSL_R_UNINITIALIZED,
	SSL_R_UNEXPECTED_EOF_WHILE_READING,
	SSL_R_UNEXPECTED_MESSAGE,
	SSL_R_UNSUPPORTED_PROTOCOL,
	SSL_R_WRONG_CIPHER_RETURNED,
	SSL_R_WRONG_CURVE,
	SSL_R_WRONG_SIGNATURE_TYPE,
	SSL_R_WRONG_VERSION_NUMBER,
	SSL_AD_REASON_OFFSET + 0,
	SSL_AD_REASON_OFFSET + 10,
	SSL_AD_REASON_OFFSET + 20,
	SSL_AD_REASON_OFFSET + 22,
	SSL_AD_REASON_OFFSET + 40,
	SSL_AD_REASON_OFFSET + 42,
	SSL_AD_REASON_OFFSET + 43,
	SSL_AD_REASON_OFFSET + 44,
	SSL_AD_REASON_OFFSET + 45,
	SSL_AD_REASON_OFFSET + 46,
	SSL_AD_REASON_OFFSET + 47,
	SSL_AD_REASON_OFFSET + 48,
	SSL_AD_REASON_OFFSET + 49,
	SSL_AD_REASON_OFFSET + 50,
	SSL_AD_REASON_OFFSET + 51,
	SSL_AD_REASON_OFFSET + 70,
	SSL_AD_REASON_OFFSET + 71,
	SSL_AD_REASON_OFFSET + 80,
	SSL_AD_REASON_OFFSET + 86,
	SSL_AD_REASON_OFFSET + 90,
	SSL_AD_REASON_OFFSET + 100,
	SSL_AD_REASON_OFFSET + 109,
	SSL_AD_REASON_OFFSET + 110,
	SSL_AD_REASON_OFFSET + 112,
	SSL_AD_REASON_OFFSET + 113,
	SSL_AD_REASON_OFFSET + 115,
	SSL_AD_REASON_OFFSET + 116,
	SSL_AD_REASON_OFFSET + 120
};

/* The texts of those reasons, in libssl-compat's own words, in the same order. */
const char *const sslc_reason_texts[] = {
	"elliptic curve point not valid",
	"extension of a handshake message not valid",
	"handshake message length not valid",
	"key share not valid",
	"length not valid",
	"signature not valid",
	"write retried with a different length",
	"certificate verification failed",
	"record could not be decrypted or did not authenticate",
	"digest check failed",
	"message larger than allowed",
	"peer sent an HTTP request",
	"length does not match",
	"no protocol version available",
	"no cipher suite in common",
	"no usable key share",
	"record length too long",
	"connection is shut down",
	"record length does not match",
	"shutdown before the handshake finished",
	"handshake failed",
	"connection not set up",
	"stream ended without a close_notify",
	"unexpected message",
	"protocol version not supported",
	"server chose a cipher suite that was not offered",
	"server chose a group that was not offered",
	"signature scheme not allowed",
	"version number not valid",
	"peer closed the connection (alert close_notify)",
	"peer sent alert unexpected_message",
	"peer sent alert bad_record_mac",
	"peer sent alert record_overflow",
	"peer sent alert handshake_failure",
	"peer sent alert bad_certificate",
	"peer sent alert unsupported_certificate",
	"peer sent alert certificate_revoked",
	"peer sent alert certificate_expired",
	"peer sent alert certificate_unknown",
	"peer sent alert illegal_parameter",
	"peer sent alert unknown_ca",
	"peer sent alert access_denied",
	"peer sent alert decode_error",
	"peer sent alert decrypt_error",
	"peer sent alert protocol_version",
	"peer sent alert insufficient_security",
	"peer sent alert internal_error",
	"peer sent alert inappropriate_fallback",
	"peer sent alert user_canceled",
	"peer sent alert no_renegotiation",
	"peer sent alert missing_extension",
	"peer sent alert unsupported_extension",
	"peer sent alert unrecognized_name",
	"peer sent alert bad_certificate_status_response",
	"peer sent alert unknown_psk_identity",
	"peer sent alert certificate_required",
	"peer sent alert no_application_protocol"
};

/* The number of reasons in the two tables. */
const size_t sslc_reason_count = sizeof(sslc_reason_numbers) / sizeof(sslc_reason_numbers[0]);

/*
 * The one client method: SSL_CTX_new accepts only the address of this
 * object, so a pointer that is not it is never read.
 */
static const struct ssl_method_st ssl_client_method = { 1 };

/* Calls a user's verify callback for the handshake: records the verdict, then asks the callback. */
static int ssl_verify_adapter(void *context, int preverified, enum cryc_verify_code code);

/* Reads from the SSL's socket for the connection, keeping errno of a failure. */
static int ssl_transport_read(void *context, uint8_t *buffer, size_t size, size_t *done, int *os_error);

/* Writes to the SSL's socket for the connection, keeping errno of a failure. */
static int ssl_transport_write(void *context, const uint8_t *buffer, size_t size, size_t *done, int *os_error);

/* Queues one error of the SSL library. */
static void ssl_queue(unsigned reason);

/* Queues one error of the X509 library for a failed store operation, by what cryc_store_ reported. */
static void ssl_queue_store(int error);

/* Notes a failed call of an SSL: the classification SSL_ERROR_SSL and one queued reason. */
static void ssl_refuse(SSL *ssl, unsigned reason);

/* Begins a call on an SSL: refuses a missing SSL and resets what the last call left; returns 0 or -1. */
static int ssl_enter(SSL *ssl);

/* Makes the connection at the first use: the configuration from the context and the SSL; returns 0, or -1 after refusing. */
static int ssl_start(SSL *ssl);

/* Works out the version range of a handshake from the settings; returns 1, or 0 when no version is left. */
static int ssl_versions(const SSL *ssl, uint16_t *minimum, uint16_t *maximum);

/* Classifies the failure or wait that a connection call reported; returns the public return value. */
static int ssl_finish(SSL *ssl, enum ssl_operation operation, int error, unsigned wants, int was_failed);

/* Tells the verification result of the handshake so far. */
static long ssl_verify_code(const SSL *ssl);

/* Answers a version ctrl of a context or an SSL: sets or gets one bound. */
static long ssl_version_ctrl(uint16_t *minimum, uint16_t *maximum, int command, long number);

/* Sets the SNI name of an SSL from SSL_ctrl's arguments; returns 1 or 0. */
static long ssl_set_server_name(SSL *ssl, long number, void *pointer);

/* Tells whether a text is written as an IP address: 0 not, 1 four decimal fields, 2 contains a colon. */
static int ssl_address_form(const char *text);

/*
 * Returns the client method (a constant object).
 */
const SSL_METHOD *
TLS_client_method(void)
{
	/* Succeeded: the only method there is. */
	return &ssl_client_method;
}

/*
 * Makes a context: the client method only (another is NULL with an error);
 * versions TLS 1.2 to 1.3; SSL_VERIFY_NONE; an empty store.
 */
SSL_CTX *
SSL_CTX_new(
	const SSL_METHOD *method)
{
	SSL_CTX *ctx;
	int error;

	/* Makes the reason texts known to the error queue before any error of this library can be queued. */
	cryc_err_register_ssl(sslc_reason_numbers, sslc_reason_texts, sslc_reason_count);

	/* Refuses a missing method. */
	if (method == NULL) {
		ssl_queue(CRYC_R_PASSED_NULL_PARAMETER);
		return NULL;
	}

	/* Refuses any method but the client method; the pointer is only compared. */
	if (method != &ssl_client_method) {
		ssl_queue(CRYC_R_UNSUPPORTED);
		return NULL;
	}

	/* Allocates the context. */
	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		ssl_queue(CRYC_R_MALLOC_FAILURE);
		return NULL;
	}

	/* Makes the lock that guards the settings. */
	error = cryc_os_mutex_init(&ctx->lock);
	if (error != 0) {
		free(ctx);
		ssl_queue(CRYC_R_MALLOC_FAILURE);
		return NULL;
	}

	/* Makes the empty trust store the context owns (it queues its own error). */
	ctx->store = cryc_x509_store_new();
	if (ctx->store == NULL) {
		cryc_os_mutex_destroy(&ctx->lock);
		free(ctx);
		return NULL;
	}

	/* The creator holds the first reference; versions and options are unset, and no peer is verified. */
	ctx->references = 1L;
	ctx->verify_mode = SSL_VERIFY_NONE;

	/* Succeeded: a context with one reference, owned by the caller. */
	return ctx;
}

/*
 * Drops one reference to a context (each SSL holds one); NULL is ignored.
 */
void
SSL_CTX_free(
	SSL_CTX *ctx)
{
	long count;

	/* Nothing to free. */
	if (ctx == NULL)
		return;

	/* Drops this holder's reference; the call that brings the count to zero frees the context. */
	count = cryc_os_atomic_add(&ctx->references, -1L);
	if (count != 0L)
		return;

	/* Frees the lock, the store reference and the object. */
	cryc_os_mutex_destroy(&ctx->lock);
	cryc_x509_store_free(ctx->store);
	free(ctx);
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
	/* A missing context has no setting to change. */
	if (ctx == NULL) {
		ssl_queue(CRYC_R_PASSED_NULL_PARAMETER);
		return;
	}

	/* Records the two settings together so that a handshake never sees one without the other. */
	cryc_os_mutex_lock(&ctx->lock);

	ctx->verify_mode = mode & SSL_VERIFY_PEER;
	ctx->verify_callback = callback;

	cryc_os_mutex_unlock(&ctx->lock);
}

/*
 * Adds the CERTIFICATE blocks of the PEM file at file (UTF-8) to the
 * store.  path must be NULL (directories of hashed names are not
 * supported: 0).  Returns 1, or 0 when the file cannot be read or holds no
 * certificate or a bad one (the good ones stay added).
 */
int
SSL_CTX_load_verify_locations(
	SSL_CTX *ctx,
	const char *file,
	const char *path)
{
	int error;

	/* Refuses a missing context. */
	if (ctx == NULL) {
		ssl_queue(CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Refuses a directory of hashed names, which is not supported. */
	if (path != NULL) {
		ssl_queue(CRYC_R_UNSUPPORTED);
		return 0;
	}

	/* Refuses a call that names no place at all. */
	if (file == NULL) {
		ssl_queue(CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Reads the file and adds its certificates; the store takes its own lock after the file is read. */
	error = cryc_store_add_file(&ctx->store->store, file);
	if (error != 0) {
		ssl_queue_store(error);
		return 0;
	}

	/* Succeeded: every certificate of the file is a trust anchor. */
	return 1;
}

/*
 * Adds the system's roots: /etc/ssl/cert.pem on zedBSD; on Windows the
 * ROOT stores (minus Disallowed) and cert.pem beside the program's .exe,
 * and the Disallowed certificates are refused in every chain from then on.
 * Returns 1 even when there are none (OpenSSL's behaviour), 0 on a read
 * error other than a missing file.
 */
int
SSL_CTX_set_default_verify_paths(
	SSL_CTX *ctx)
{
	int error;

	/* Refuses a missing context. */
	if (ctx == NULL) {
		ssl_queue(CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Adds what the system trusts; a missing default file is not an error. */
	error = cryc_store_add_system(&ctx->store->store);
	if (error != 0) {
		ssl_queue_store(error);
		return 0;
	}

	/* Succeeded: the system's roots are trust anchors (possibly none). */
	return 1;
}

/*
 * Adds options (OR) and returns the new set.
 */
uint64_t
SSL_CTX_set_options(
	SSL_CTX *ctx,
	uint64_t options)
{
	uint64_t now;

	/* A missing context has no options. */
	if (ctx == NULL) {
		ssl_queue(CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Adds the bits under the lock and takes the set they make. */
	cryc_os_mutex_lock(&ctx->lock);

	ctx->options |= options;
	now = ctx->options;

	cryc_os_mutex_unlock(&ctx->lock);

	/* Reports the options now in force. */
	return now;
}

/*
 * The context ctrls: SSL_CTRL_SET_MIN_PROTO_VERSION and _MAX_ (number 0
 * for the lowest or highest supported, TLS1_2_VERSION or TLS1_3_VERSION;
 * another value returns 0), _GET_MIN_ and _GET_MAX_ (return the setting, 0
 * when unset).  Another command returns 0.
 */
long
SSL_CTX_ctrl(
	SSL_CTX *ctx,
	int command,
	long number,
	void *pointer)
{
	long answer;

	UNUSED_PARAMETER(pointer);

	/* A missing context has nothing to set or get. */
	if (ctx == NULL) {
		ssl_queue(CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Only the four version commands are known. */
	if (command != SSL_CTRL_SET_MIN_PROTO_VERSION &&
	    command != SSL_CTRL_SET_MAX_PROTO_VERSION &&
	    command != SSL_CTRL_GET_MIN_PROTO_VERSION &&
	    command != SSL_CTRL_GET_MAX_PROTO_VERSION)
		return 0;

	/* Sets or gets the bound under the lock, as the SSLs made from this context read them. */
	cryc_os_mutex_lock(&ctx->lock);

	answer = ssl_version_ctrl(&ctx->min_version, &ctx->max_version, command, number);

	cryc_os_mutex_unlock(&ctx->lock);

	/* Reports 1 for a set, the setting for a get, 0 for a value refused. */
	return answer;
}

/*
 * Returns the context's store (owned by the context; X509_STORE_add_cert
 * adds to it).
 */
X509_STORE *
SSL_CTX_get_cert_store(
	const SSL_CTX *ctx)
{
	/* A missing context has no store. */
	if (ctx == NULL) {
		ssl_queue(CRYC_R_PASSED_NULL_PARAMETER);
		return NULL;
	}

	/* Succeeded: the store the context made, which lives as long as the context. */
	return ctx->store;
}

/*
 * Makes a connection object from a context (taking a reference to it);
 * NULL on failure.
 */
SSL *
SSL_new(
	SSL_CTX *ctx)
{
	SSL *ssl;

	/* Refuses a missing context. */
	if (ctx == NULL) {
		ssl_queue(CRYC_R_PASSED_NULL_PARAMETER);
		return NULL;
	}

	/* Allocates the connection object (the connection itself is made at the first use). */
	ssl = calloc(1, sizeof(*ssl));
	if (ssl == NULL) {
		ssl_queue(CRYC_R_MALLOC_FAILURE);
		return NULL;
	}

	/* No socket yet, no failure, and a verification that has not found fault. */
	ssl->descriptor = -1;
	ssl->last_error = SSL_ERROR_NONE;
	ssl->verify_result = X509_V_OK;

	/* The SSL keeps its context, and with it the store, alive until it is freed. */
	cryc_os_atomic_add(&ctx->references, 1L);
	ssl->ctx = ctx;

	/* Succeeded: a connection object, owned by the caller. */
	return ssl;
}

/*
 * Frees a connection (without sending anything; SSL_shutdown first for a
 * clean close), also one never connected; NULL is ignored.
 */
void
SSL_free(
	SSL *ssl)
{
	SSL_CTX *ctx;

	/* Nothing to free. */
	if (ssl == NULL)
		return;

	/* A connection that was started holds buffers and secrets, wiped by its own free. */
	if (ssl->started)
		sslc_conn_free(&ssl->conn);

	/* Frees the object, then gives up its reference to the context. */
	ctx = ssl->ctx;
	free(ssl);
	SSL_CTX_free(ctx);
}

/*
 * Sets the socket: a file descriptor on zedBSD, a SOCKET (as int) on
 * Windows, where the program has called WSAStartup before (as with
 * OpenSSL; the library does not).  Returns 1, or 0 for a negative value.
 */
int
SSL_set_fd(
	SSL *ssl,
	int descriptor)
{
	/* Refuses a missing SSL. */
	if (ssl == NULL) {
		ssl_queue(CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Refuses a value that cannot be a descriptor. */
	if (descriptor < 0) {
		ssl_queue(CRYC_R_PASSED_INVALID_ARGUMENT);
		return 0;
	}

	/* Records the socket the connection reads and writes. */
	ssl->descriptor = (intptr_t)descriptor;

	/* Succeeded: the socket is set. */
	return 1;
}

/*
 * The connection ctrls: SSL_CTRL_SET_TLSEXT_HOSTNAME with
 * TLSEXT_NAMETYPE_host_name and a host name (the SNI name; 1, or 0 for a
 * name that is not a valid host name), and the version ctrls as
 * SSL_CTX_ctrl.  Another command returns 0.
 */
long
SSL_ctrl(
	SSL *ssl,
	int command,
	long number,
	void *pointer)
{
	uint16_t minimum;
	uint16_t maximum;
	long answer;

	/* A missing SSL has nothing to set or get. */
	if (ssl == NULL) {
		ssl_queue(CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* The name for the server name indication. */
	if (command == SSL_CTRL_SET_TLSEXT_HOSTNAME) {
		answer = ssl_set_server_name(ssl, number, pointer);
		return answer;
	}

	/* Only the four version commands are known besides the name. */
	if (command != SSL_CTRL_SET_MIN_PROTO_VERSION &&
	    command != SSL_CTRL_SET_MAX_PROTO_VERSION &&
	    command != SSL_CTRL_GET_MIN_PROTO_VERSION &&
	    command != SSL_CTRL_GET_MAX_PROTO_VERSION)
		return 0;

	/* A bound cannot change once the handshake has begun with the bounds it was given. */
	if (ssl->started &&
	    (command == SSL_CTRL_SET_MIN_PROTO_VERSION || command == SSL_CTRL_SET_MAX_PROTO_VERSION)) {
		ssl_queue(CRYC_R_UNSUPPORTED);
		return 0;
	}

	/* The SSL's own bounds are the connection configuration's until the connection is made; 0 means the context's. */
	minimum = ssl->conn.config.min_version;
	maximum = ssl->conn.config.max_version;
	answer = ssl_version_ctrl(&minimum, &maximum, command, number);

	/* A set that was accepted is kept in the SSL. */
	if (command == SSL_CTRL_SET_MIN_PROTO_VERSION || command == SSL_CTRL_SET_MAX_PROTO_VERSION) {
		if (answer == 1L) {
			ssl->conn.config.min_version = minimum;
			ssl->conn.config.max_version = maximum;
		}
		return answer;
	}

	/* A get answers the SSL's own bound, or the context's when the SSL has none. */
	if (answer == 0L) {
		cryc_os_mutex_lock(&ssl->ctx->lock);

		minimum = ssl->ctx->min_version;
		maximum = ssl->ctx->max_version;
		answer = ssl_version_ctrl(&minimum, &maximum, command, number);

		cryc_os_mutex_unlock(&ssl->ctx->lock);
	}

	/* Reports the bound that applies. */
	return answer;
}

/*
 * Sets the name the certificate must match, as OpenSSL 3.5's
 * SSL_set1_host: clears the host and the address set before, then an IP
 * address literal (X509_VERIFY_PARAM_set1_ip_asc's forms) sets the address
 * and any other text sets the host name (X509_VERIFY_PARAM_set1_host).
 * NULL clears both.  Returns 1, or 0 for a text that is neither.
 */
int
SSL_set1_host(
	SSL *ssl,
	const char *host)
{
	int form;
	int done;

	/* Refuses a missing SSL. */
	if (ssl == NULL) {
		ssl_queue(CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Clears both settings first, so that a failure below leaves no name set. */
	X509_VERIFY_PARAM_set1_host(&ssl->param, NULL, 0);
	memset(ssl->param.params.ip, 0, sizeof(ssl->param.params.ip));
	ssl->param.params.ip_size = 0U;

	/* NULL and the empty text switch the name checks off. */
	if (host == NULL || host[0] == '\0')
		return 1;

	/* Finds out whether the text is written as an address. */
	form = ssl_address_form(host);

	/* An address is set as the address to match (its own error is queued when it is not one). */
	if (form != 0) {
		done = X509_VERIFY_PARAM_set1_ip_asc(&ssl->param, host);
		return done;
	}

	/* Any other text is a host name (the error is queued when it is not a valid one). */
	done = X509_VERIFY_PARAM_set1_host(&ssl->param, host, 0);

	/* Reports whether the name was accepted. */
	return done;
}

/*
 * Returns the SSL's verification parameters (owned by the SSL).
 */
X509_VERIFY_PARAM *
SSL_get0_param(
	SSL *ssl)
{
	/* A missing SSL has no parameters. */
	if (ssl == NULL) {
		ssl_queue(CRYC_R_PASSED_NULL_PARAMETER);
		return NULL;
	}

	/* Succeeded: the parameters inside the SSL. */
	return &ssl->param;
}

/*
 * Runs the handshake.  Returns 1 when done; 0 or -1 on failure or when the
 * socket would block (SSL_get_error tells which); call again with the same
 * SSL after the socket is ready.
 */
int
SSL_connect(
	SSL *ssl)
{
	unsigned wants;
	int was_failed;
	int error;
	int result;

	/* Begins the call: a missing SSL fails, the last call's state is reset. */
	error = ssl_enter(ssl);
	if (error != 0)
		return -1;

	/* A handshake that is already done is done. */
	if (ssl->started && (ssl->conn.flags & SSLC_CONN_CONNECTED) != 0U)
		return 1;

	/* Makes the connection at the first call. */
	error = ssl_start(ssl);
	if (error != 0)
		return -1;

	/* Runs the handshake as far as the socket lets it go. */
	was_failed = (ssl->conn.flags & SSLC_CONN_FAILED) != 0U;
	wants = 0;
	error = sslc_conn_handshake(&ssl->conn, &wants);
	ssl->verify_result = ssl_verify_code(ssl);
	if (error == 0)
		return 1;

	/* Classifies why it stopped. */
	result = ssl_finish(ssl, SSL_OPERATION_HANDSHAKE, error, wants, was_failed);

	/* Reports -1 for a failure or a wait, 0 for a peer that closed. */
	return result;
}

/*
 * Reads up to length bytes of application data (running the handshake
 * first when it is not done).  A length of 0 or less returns -1 with
 * SSL_R_BAD_LENGTH queued (SSL_ERROR_SSL).  Returns the count (> 0), or 0
 * or -1 (SSL_get_error: WANT_READ, WANT_WRITE, ZERO_RETURN after the
 * peer's close_notify or, with SSL_OP_IGNORE_UNEXPECTED_EOF, after an EOF
 * without it; SYSCALL with errno; SSL otherwise).
 */
int
SSL_read(
	SSL *ssl,
	void *buffer,
	int length)
{
	size_t done;
	unsigned wants;
	int was_failed;
	int error;
	int result;

	/* Begins the call: a missing SSL fails, the last call's state is reset. */
	error = ssl_enter(ssl);
	if (error != 0)
		return -1;

	/* A length that is not positive is the caller's mistake, not an end of the stream. */
	if (length <= 0) {
		ssl_refuse(ssl, SSL_R_BAD_LENGTH);
		return -1;
	}

	/* A missing buffer cannot take the data. */
	if (buffer == NULL) {
		ssl_refuse(ssl, CRYC_R_PASSED_NULL_PARAMETER);
		return -1;
	}

	/* Makes the connection at the first call. */
	error = ssl_start(ssl);
	if (error != 0)
		return -1;

	/* Reads the application data, running the handshake first when needed. */
	was_failed = (ssl->conn.flags & SSLC_CONN_FAILED) != 0U;
	wants = 0;
	done = 0;
	error = sslc_conn_read(&ssl->conn, (uint8_t *)buffer, (size_t)length, &done, &wants);
	ssl->verify_result = ssl_verify_code(ssl);
	if (error == 0)
		return (int)done;

	/* Classifies why no data came. */
	result = ssl_finish(ssl, SSL_OPERATION_READ, error, wants, was_failed);

	/* Reports -1 for a failure or a wait, 0 for a peer that closed. */
	return result;
}

/*
 * Writes length bytes (all of them, in records of at most 16 KiB) and
 * returns length, or 0 or -1 as SSL_read.  After WANT_WRITE or WANT_READ
 * the call must be repeated with the same buffer and length (OpenSSL's
 * rule); the bytes already taken are not taken again.  A repeated call
 * with a length smaller than the bytes already taken returns -1 with
 * SSL_R_BAD_WRITE_RETRY (SSL_ERROR_SSL); a length of 0 or less returns -1
 * with SSL_R_BAD_LENGTH.
 */
int
SSL_write(
	SSL *ssl,
	const void *buffer,
	int length)
{
	size_t done;
	unsigned wants;
	int was_failed;
	int error;
	int result;

	/* Begins the call: a missing SSL fails, the last call's state is reset. */
	error = ssl_enter(ssl);
	if (error != 0)
		return -1;

	/* A length that is not positive is the caller's mistake. */
	if (length <= 0) {
		ssl_refuse(ssl, SSL_R_BAD_LENGTH);
		return -1;
	}

	/* A missing buffer has nothing to send. */
	if (buffer == NULL) {
		ssl_refuse(ssl, CRYC_R_PASSED_NULL_PARAMETER);
		return -1;
	}

	/* A retry may not name fewer bytes than the earlier try already took. */
	if (ssl->started && ssl->conn.taken > (size_t)length) {
		ssl_refuse(ssl, SSL_R_BAD_WRITE_RETRY);
		return -1;
	}

	/* Makes the connection at the first call. */
	error = ssl_start(ssl);
	if (error != 0)
		return -1;

	/* Writes the application data, running the handshake first when needed. */
	was_failed = (ssl->conn.flags & SSLC_CONN_FAILED) != 0U;
	wants = 0;
	done = 0;
	error = sslc_conn_write(&ssl->conn, (const uint8_t *)buffer, (size_t)length, &done, &wants);
	ssl->verify_result = ssl_verify_code(ssl);
	if (error == 0)
		return length;

	/* Classifies why the write did not finish. */
	result = ssl_finish(ssl, SSL_OPERATION_WRITE, error, wants, was_failed);

	/* Reports -1 for a failure or a wait, 0 for a peer that closed. */
	return result;
}

/*
 * Classifies the result of the last SSL_connect, SSL_read, SSL_write or
 * SSL_shutdown on ssl (result is its return value).  The classification is
 * the one that call stored (it does not look at the error queue again);
 * for SSL_ERROR_SYSCALL it also sets errno back to the value the failing
 * socket call left, so a program may read errno after it.
 * SSL_ERROR_NONE for a positive result; WANT_READ or WANT_WRITE when the
 * socket would block; ZERO_RETURN for a clean close; SYSCALL for a socket
 * failure (errno set; an EOF without close_notify, without
 * SSL_OP_IGNORE_UNEXPECTED_EOF, is SSL_ERROR_SSL with
 * SSL_R_UNEXPECTED_EOF_WHILE_READING queued, as in OpenSSL 3);
 * SSL_ERROR_SSL otherwise.
 */
int
SSL_get_error(
	const SSL *ssl,
	int result)
{
	/* A missing SSL is reported as a failure of the library. */
	if (ssl == NULL)
		return SSL_ERROR_SSL;

	/* A positive result is never an error. */
	if (result > 0)
		return SSL_ERROR_NONE;

	/* A socket failure gives its errno back to the program. */
	if (ssl->last_error == SSL_ERROR_SYSCALL)
		errno = ssl->last_errno;

	/* Succeeded: the classification the last call stored. */
	return ssl->last_error;
}

/*
 * Returns the decrypted bytes buffered and not yet read.
 */
int
SSL_pending(
	const SSL *ssl)
{
	size_t count;

	/* A missing SSL, or one never started, holds nothing. */
	if (ssl == NULL)
		return 0;
	if (!ssl->started)
		return 0;

	/* The bytes of the opened record not yet given out, cut to what an int holds. */
	count = sslc_conn_pending(&ssl->conn);
	if (count > 0x7fffffffUL)
		count = 0x7fffffffUL;

	/* Reports the buffered application bytes. */
	return (int)count;
}

/*
 * Sends close_notify (once).  Returns 0 after sending it when the peer's
 * has not been received, 1 when both have, -1 on failure or blocking
 * (SSL_get_error).  Before the handshake is complete (never started, or in
 * progress) nothing is sent and -1 is returned with
 * SSL_R_SHUTDOWN_WHILE_IN_INIT (SSL_ERROR_SSL); after a fatal alert was
 * sent or received nothing is sent and -1 is returned with
 * SSL_R_PROTOCOL_IS_SHUTDOWN.
 */
int
SSL_shutdown(
	SSL *ssl)
{
	unsigned wants;
	int both;
	int error;
	int result;

	/* Begins the call: a missing SSL fails, the last call's state is reset. */
	error = ssl_enter(ssl);
	if (error != 0)
		return -1;

	/* Nothing can be closed that was never started. */
	if (!ssl->started) {
		ssl_refuse(ssl, SSL_R_SHUTDOWN_WHILE_IN_INIT);
		return -1;
	}

	/* After a fatal alert the connection is over; no close_notify follows it. */
	if ((ssl->conn.flags & SSLC_CONN_FAILED) != 0U) {
		ssl_refuse(ssl, SSL_R_PROTOCOL_IS_SHUTDOWN);
		return -1;
	}

	/* A handshake still in progress is not closed with a close_notify. */
	if ((ssl->conn.flags & SSLC_CONN_CONNECTED) == 0U) {
		ssl_refuse(ssl, SSL_R_SHUTDOWN_WHILE_IN_INIT);
		return -1;
	}

	/* Sends the close_notify (once) and learns whether the peer's has come. */
	wants = 0;
	both = 0;
	error = sslc_conn_shutdown(&ssl->conn, &both, &wants);
	if (error != 0) {
		result = ssl_finish(ssl, SSL_OPERATION_SHUTDOWN, error, wants, 0);
		return result;
	}

	/* Both close_notify alerts is 1. */
	if (both != 0)
		return 1;

	/* Succeeded: this side's is sent and the peer's is not yet received (0 is not an error). */
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
	long code;

	/* A missing SSL is not a verified one. */
	if (ssl == NULL)
		return X509_V_ERR_UNSPECIFIED;

	/* The verdict of the handshake so far, kept after the handshake failed. */
	code = ssl_verify_code(ssl);

	/* Reports the result. */
	return code;
}

/*
 * Calls a user's verify callback for the handshake: records the verdict,
 * then asks the callback (no callback: the chain's own verdict decides).
 */
static int
ssl_verify_adapter(
	void *context,
	int preverified,
	enum cryc_verify_code code)
{
	SSL *ssl;
	SSL_verify_cb callback;
	long verdict;
	int answer;

	/* The SSL whose handshake asks. */
	ssl = (SSL *)context;

	/* Records the verdict where SSL_get_verify_result reads it, even after the handshake fails. */
	verdict = cryc_verify_code_to_x509(code);
	ssl->verify_result = verdict;

	/* What the callback is told: the verdict and the depth of the certificate it concerns. */
	ssl->store_ctx.error = verdict;
	ssl->store_ctx.depth = (int)ssl->conn.hs.verify.depth;

	/* Reads the callback under the lock, and calls it after the lock is released. */
	cryc_os_mutex_lock(&ssl->ctx->lock);

	callback = ssl->ctx->verify_callback;

	cryc_os_mutex_unlock(&ssl->ctx->lock);

	/* Without a callback the chain's own verdict decides. */
	if (callback == NULL) {
		if (preverified != 0)
			return 1;
		return 0;
	}

	/* The callback decides whether the handshake goes on. */
	answer = callback(preverified, &ssl->store_ctx);
	if (answer != 0)
		return 1;

	/* The callback refused the certificate. */
	return 0;
}

/*
 * Reads from the SSL's socket for the connection, keeping errno of a
 * failure.
 */
static int
ssl_transport_read(
	void *context,
	uint8_t *buffer,
	size_t size,
	size_t *done,
	int *os_error)
{
	SSL *ssl;
	int error;

	/* Reads from the socket the SSL was given. */
	ssl = (SSL *)context;
	error = cryc_os_socket_read(ssl->descriptor, buffer, size, done, os_error);
	if (error == EIO) {
		/* SSL_get_error gives this value back as errno: the call's own, else the system's code, else EIO. */
		ssl->last_errno = errno;
		if (ssl->last_errno == 0)
			ssl->last_errno = *os_error;
		if (ssl->last_errno == 0)
			ssl->last_errno = EIO;
	}

	/* Reports what the socket call reported. */
	return error;
}

/*
 * Writes to the SSL's socket for the connection, keeping errno of a
 * failure.
 */
static int
ssl_transport_write(
	void *context,
	const uint8_t *buffer,
	size_t size,
	size_t *done,
	int *os_error)
{
	SSL *ssl;
	int error;

	/* Writes to the socket the SSL was given. */
	ssl = (SSL *)context;
	error = cryc_os_socket_write(ssl->descriptor, buffer, size, done, os_error);
	if (error == EIO) {
		/* SSL_get_error gives this value back as errno: the call's own, else the system's code, else EIO. */
		ssl->last_errno = errno;
		if (ssl->last_errno == 0)
			ssl->last_errno = *os_error;
		if (ssl->last_errno == 0)
			ssl->last_errno = EIO;
	}

	/* Reports what the socket call reported. */
	return error;
}

/*
 * Queues one error of the SSL library.
 */
static void
ssl_queue(
	unsigned reason)
{
	/* Queues the reason under the SSL library's number. */
	cryc_err_put(CRYC_LIB_SSL, reason);
}

/*
 * Queues one error of the X509 library for a failed store operation, by
 * what cryc_store_ reported.
 */
static void
ssl_queue_store(
	int error)
{
	/* No memory. */
	if (error == ENOMEM) {
		cryc_err_put(CRYC_LIB_X509, CRYC_R_MALLOC_FAILURE);
		return;
	}

	/* The store holds as many certificates as it may. */
	if (error == ENOBUFS) {
		cryc_err_put(CRYC_LIB_X509, CRYC_R_UNSUPPORTED);
		return;
	}

	/* A file that is missing, unreadable, empty of certificates or holding a bad one. */
	cryc_err_put(CRYC_LIB_X509, CRYC_R_PASSED_INVALID_ARGUMENT);
}

/*
 * Notes a failed call of an SSL: the classification SSL_ERROR_SSL and one
 * queued reason.
 */
static void
ssl_refuse(
	SSL *ssl,
	unsigned reason)
{
	/* SSL_get_error will say SSL_ERROR_SSL for this call. */
	ssl->last_error = SSL_ERROR_SSL;

	/* The reason goes to the error queue, once. */
	ssl_queue(reason);
}

/*
 * Begins a call on an SSL: refuses a missing SSL and resets what the last
 * call left; returns 0 or -1.
 */
static int
ssl_enter(
	SSL *ssl)
{
	/* A missing SSL has nowhere to keep a classification; only the queue is told. */
	if (ssl == NULL) {
		ssl_queue(CRYC_R_PASSED_NULL_PARAMETER);
		return -1;
	}

	/* The classification and the errno belong to one call. */
	ssl->last_error = SSL_ERROR_NONE;
	ssl->last_errno = 0;

	/* Succeeded: the call may go on. */
	return 0;
}

/*
 * Makes the connection at the first use: the configuration from the
 * context and the SSL; returns 0, or -1 after refusing.
 */
static int
ssl_start(
	SSL *ssl)
{
	struct sslc_hs_config config;
	struct sslc_transport transport;
	uint32_t flags;
	uint64_t options;
	uint16_t minimum;
	uint16_t maximum;
	int verify_peer;
	int usable;
	int error;

	/* A connection already made is used as it is. */
	if (ssl->started)
		return 0;

	/* A connection without a socket cannot start. */
	if (ssl->descriptor < 0) {
		ssl_refuse(ssl, SSL_R_UNINITIALIZED);
		return -1;
	}

	/* Reads the context's settings together, under its lock. */
	cryc_os_mutex_lock(&ssl->ctx->lock);

	options = ssl->ctx->options;
	verify_peer = (ssl->ctx->verify_mode & SSL_VERIFY_PEER) != 0;

	cryc_os_mutex_unlock(&ssl->ctx->lock);

	/* Works out the versions the handshake may use from the bounds and the options. */
	minimum = 0U;
	maximum = 0U;
	usable = ssl_versions(ssl, &minimum, &maximum);
	if (usable == 0) {
		ssl_refuse(ssl, SSL_R_NO_PROTOCOLS_AVAILABLE);
		return -1;
	}

	/* Fills the handshake configuration: trust anchors, what to verify, the name for SNI, and where randomness comes from. */
	memset(&config, 0, sizeof(config));
	config.min_version = minimum;
	config.max_version = maximum;
	config.store = &ssl->ctx->store->store;
	config.params = ssl->param.params;
	config.server_name = NULL;
	if (ssl->server_name[0] != '\0')
		config.server_name = ssl->server_name;
	config.verify_peer = verify_peer;
	config.callback = ssl_verify_adapter;
	config.callback_context = ssl;
	config.random = cryc_os_random;
	config.random_context = NULL;

	/* The transport reads and writes the SSL's socket. */
	transport.read = ssl_transport_read;
	transport.write = ssl_transport_write;
	transport.context = ssl;

	/* SSL_OP_IGNORE_UNEXPECTED_EOF makes an end of the stream count as the peer's close_notify. */
	flags = 0U;
	if ((options & SSL_OP_IGNORE_UNEXPECTED_EOF) != 0U)
		flags |= SSLC_CONN_IGNORE_EOF;

	/* Makes the connection (the SSL's own bounds in its configuration are overwritten, after they were read above). */
	error = sslc_conn_init(&ssl->conn, &config, &transport, flags);
	if (error == ENOMEM) {
		ssl_refuse(ssl, CRYC_R_MALLOC_FAILURE);
		return -1;
	}
	if (error != 0) {
		ssl_refuse(ssl, CRYC_R_INTERNAL_ERROR);
		return -1;
	}

	/* The connection exists from now on, and SSL_free must free it. */
	ssl->started = 1;

	/* Succeeded: the connection is ready for its first call. */
	return 0;
}

/*
 * Works out the version range of a handshake from the settings; returns 1,
 * or 0 when no version is left.
 */
static int
ssl_versions(
	const SSL *ssl,
	uint16_t *minimum,
	uint16_t *maximum)
{
	uint64_t options;
	uint16_t low;
	uint16_t high;
	int has_12;
	int has_13;

	/* The SSL's own bounds first; 0 leaves the choice to the context, whose bounds and options are read together. */
	low = ssl->conn.config.min_version;
	high = ssl->conn.config.max_version;

	cryc_os_mutex_lock(&ssl->ctx->lock);

	if (low == 0U)
		low = ssl->ctx->min_version;
	if (high == 0U)
		high = ssl->ctx->max_version;
	options = ssl->ctx->options;

	cryc_os_mutex_unlock(&ssl->ctx->lock);

	/* Unset bounds mean the lowest and the highest version this library has. */
	if (low == 0U)
		low = TLS1_2_VERSION;
	if (high == 0U)
		high = TLS1_3_VERSION;

	/* TLS 1.2 is usable when the bounds contain it and it is not switched off. */
	has_12 = 0;
	if (low <= TLS1_2_VERSION && high >= TLS1_2_VERSION && (options & SSL_OP_NO_TLSv1_2) == 0U)
		has_12 = 1;

	/* TLS 1.3 likewise. */
	has_13 = 0;
	if (low <= TLS1_3_VERSION && high >= TLS1_3_VERSION && (options & SSL_OP_NO_TLSv1_3) == 0U)
		has_13 = 1;

	/* No version left. */
	if (!has_12 && !has_13)
		return 0;

	/* The range is the lowest and the highest of the versions left. */
	*minimum = TLS1_2_VERSION;
	if (!has_12)
		*minimum = TLS1_3_VERSION;
	*maximum = TLS1_3_VERSION;
	if (!has_13)
		*maximum = TLS1_2_VERSION;

	/* Succeeded: at least one version is usable. */
	return 1;
}

/*
 * Classifies the failure or wait that a connection call reported; returns
 * the public return value.
 */
static int
ssl_finish(
	SSL *ssl,
	enum ssl_operation operation,
	int error,
	unsigned wants,
	int was_failed)
{
	uint32_t flags;
	unsigned reason;

	/* The flags of the connection after the call. */
	flags = ssl->conn.flags;

	/* The socket cannot go on yet: the call is repeated when it can. */
	if (error == EAGAIN) {
		ssl->last_error = SSL_ERROR_WANT_READ;
		if ((wants & SSLC_WANT_WRITE) != 0U)
			ssl->last_error = SSL_ERROR_WANT_WRITE;
		return -1;
	}

	/* The stream ended, or this side's close_notify is already sent. */
	if (error == EPIPE) {
		/* A write after this side's close_notify is a use of a closed connection. */
		if (operation == SSL_OPERATION_WRITE &&
		    (flags & SSLC_CONN_CLOSE_SENT) != 0U &&
		    (flags & SSLC_CONN_CONNECTED) != 0U) {
			ssl_refuse(ssl, SSL_R_PROTOCOL_IS_SHUTDOWN);
			return -1;
		}

		/* The peer's close_notify (or an end of the stream the option counts as one) is a clean close. */
		if ((flags & SSLC_CONN_CLOSE_RECEIVED) != 0U) {
			ssl->last_error = SSL_ERROR_ZERO_RETURN;
			return 0;
		}

		/* The peer went away during the handshake without a close_notify. */
		reason = ssl->conn.reason;
		if (reason == 0U)
			reason = SSL_R_UNEXPECTED_EOF_WHILE_READING;
		ssl_refuse(ssl, reason);
		return -1;
	}

	/* The stream ended without a close_notify: the data may have been cut short. */
	if (error == ECONNRESET) {
		reason = ssl->conn.reason;
		if (reason == 0U)
			reason = SSL_R_UNEXPECTED_EOF_WHILE_READING;
		ssl_refuse(ssl, reason);
		return -1;
	}

	/* A socket failure: errno is kept for SSL_get_error, and the system's code is queued. */
	if (error == EIO && ssl->last_errno != 0) {
		ssl->last_error = SSL_ERROR_SYSCALL;
		cryc_err_put(CRYC_LIB_SYS, (unsigned)ssl->last_errno);
		return -1;
	}

	/* No memory. */
	if (error == ENOMEM) {
		ssl_refuse(ssl, CRYC_R_MALLOC_FAILURE);
		return -1;
	}

	/* A connection that had already failed before this call answers that it is shut down. */
	if (was_failed) {
		ssl_refuse(ssl, SSL_R_PROTOCOL_IS_SHUTDOWN);
		return -1;
	}

	/* A failure of the protocol, with the reason the connection recorded (none: an internal fault). */
	reason = ssl->conn.reason;
	if (reason == 0U)
		reason = CRYC_R_INTERNAL_ERROR;
	ssl_refuse(ssl, reason);

	/* Reports the failure. */
	return -1;
}

/*
 * Tells the verification result of the handshake so far.
 */
static long
ssl_verify_code(
	const SSL *ssl)
{
	long code;

	/* Before the connection exists, and after a failed handshake wiped it, the result kept by the SSL stands. */
	if (!ssl->started)
		return ssl->verify_result;
	if ((ssl->conn.flags & SSLC_CONN_FAILED) != 0U)
		return ssl->verify_result;

	/* Before the server's certificates are read there is no verdict yet. */
	if (ssl->conn.hs.peer_count == 0U)
		return ssl->verify_result;

	/* The verdict of the chain the handshake validated. */
	code = cryc_verify_code_to_x509(ssl->conn.hs.verify_code);

	/* Reports the code. */
	return code;
}

/*
 * Answers a version ctrl of a context or an SSL: sets or gets one bound.
 */
static long
ssl_version_ctrl(
	uint16_t *minimum,
	uint16_t *maximum,
	int command,
	long number)
{
	/* A bound is set to 0 (the lowest or highest supported) or to one of the two versions; any other value is refused. */
	if (command == SSL_CTRL_SET_MIN_PROTO_VERSION || command == SSL_CTRL_SET_MAX_PROTO_VERSION) {
		if (number != 0L && number != TLS1_2_VERSION && number != TLS1_3_VERSION) {
			ssl_queue(CRYC_R_PASSED_INVALID_ARGUMENT);
			return 0;
		}
	}

	/* Sets the lower bound. */
	if (command == SSL_CTRL_SET_MIN_PROTO_VERSION) {
		*minimum = (uint16_t)number;
		return 1;
	}

	/* Sets the upper bound. */
	if (command == SSL_CTRL_SET_MAX_PROTO_VERSION) {
		*maximum = (uint16_t)number;
		return 1;
	}

	/* Gets the lower bound. */
	if (command == SSL_CTRL_GET_MIN_PROTO_VERSION)
		return (long)*minimum;

	/* Gets the upper bound. */
	return (long)*maximum;
}

/*
 * Sets the SNI name of an SSL from SSL_ctrl's arguments; returns 1 or 0.
 */
static long
ssl_set_server_name(
	SSL *ssl,
	long number,
	void *pointer)
{
	char normalized[CRYC_NAME_HOST_MAX + 1U];
	const char *text;
	int form;
	int error;

	/* Only the host name type exists. */
	if (number != TLSEXT_NAMETYPE_host_name) {
		ssl_queue(CRYC_R_PASSED_INVALID_ARGUMENT);
		return 0;
	}

	/* The hello is sent at the first call; a name set after that would not be the one sent. */
	if (ssl->started) {
		ssl_queue(CRYC_R_UNSUPPORTED);
		return 0;
	}

	/* A missing name switches the extension off. */
	text = (const char *)pointer;
	if (text == NULL) {
		memset(ssl->server_name, 0, sizeof(ssl->server_name));
		return 1;
	}

	/* An address is not a name the extension may carry (RFC 6066 3): nothing is sent for it, and the call is not a failure. */
	form = ssl_address_form(text);
	if (form != 0) {
		memset(ssl->server_name, 0, sizeof(ssl->server_name));
		return 1;
	}

	/* Checks the name and puts it in its sent form (no trailing dot, lower case). */
	error = cryc_name_host_normalize(text, normalized);
	if (error != 0) {
		ssl_queue(CRYC_R_PASSED_INVALID_ARGUMENT);
		return 0;
	}

	/* Keeps the name for the ClientHello. */
	memset(ssl->server_name, 0, sizeof(ssl->server_name));
	memcpy(ssl->server_name, normalized, strlen(normalized));

	/* Succeeded: the name is set. */
	return 1;
}

/*
 * Tells whether a text is written as an IP address: 0 not, 1 four decimal
 * fields, 2 contains a colon.
 */
static int
ssl_address_form(
	const char *text)
{
	const char *cursor;
	unsigned fields;
	unsigned digits;
	unsigned value;

	/* A colon makes it an IPv6 candidate; the parser of X509_VERIFY_PARAM_set1_ip_asc decides. */
	if (strchr(text, ':') != NULL)
		return 2;

	/* Reads four decimal fields of at most 255, without a leading zero, separated by dots. */
	fields = 0U;
	digits = 0U;
	value = 0U;
	for (cursor = text;
	     *cursor != '\0';
	     cursor++) {
		if (*cursor >= '0' && *cursor <= '9') {
			/* A field of two or more digits may not start with 0. */
			if (digits == 1U && value == 0U)
				return 0;

			/* A field has at most three digits and the value 255. */
			value = value * 10U + (unsigned)(*cursor - '0');
			digits++;
			if (digits > 3U || value > 255U)
				return 0;
		} else if (*cursor == '.') {
			/* A dot ends a field that has digits; there are three dots. */
			if (digits == 0U || fields >= 3U)
				return 0;
			fields++;
			digits = 0U;
			value = 0U;
		} else {
			/* Any other character makes it a name. */
			return 0;
		}
	}

	/* The last field has digits and there were three dots. */
	if (digits == 0U || fields != 3U)
		return 0;

	/* Succeeded: an IPv4 address. */
	return 1;
}
