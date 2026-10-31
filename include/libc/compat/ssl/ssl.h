/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libssl-compat: zedBSD's own TLS client (TLS 1.3 and TLS 1.2), with the
 * subset of OpenSSL's libssl interface that libbrowser's net/tls.c uses.
 * It holds no OpenSSL code; the names, the signatures and the numeric
 * values are OpenSSL 3.5's, so that the same program works with either
 * library.  It is built on libcrypto-compat (<compat/crypto/...>), which
 * holds the error queue and the certificates.
 *
 * Range: a client only (TLS_client_method); certificate verification
 * against the trust store, the host name or the address; SNI; blocking
 * and non-blocking sockets.  No server, no client certificates, no session
 * resumption, no 0-RTT, no ALPN, no renegotiation, no BIOs.  The behaviour
 * of each function is OpenSSL's for the arguments described here; other
 * arguments (another ctrl command, another option) fail or are ignored as
 * each comment says.
 *
 * Threads: an SSL_CTX may be used by several threads to make SSL objects
 * after it is set up (its settings and its store are read under locks);
 * an SSL is used by one thread at a time.
 */

#ifndef KERN_COMPAT_SSL_SSL_H
#define KERN_COMPAT_SSL_SSL_H

#include <compat/crypto/err.h>
#include <compat/crypto/x509.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The objects (OpenSSL's struct tags). */
typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_method_st SSL_METHOD;

/* A verify callback: preverify_ok is 1 when the chain validated; returns 1 to go on, 0 to fail. */
typedef int (*SSL_verify_cb)(int preverify_ok, X509_STORE_CTX *store);

/*
 * The protocol versions, the verify modes, the results of SSL_get_error,
 * the ctrl commands and the name type.
 * Values referenced: OpenSSL 3.5.8 include/openssl/prov_ssl.h
 * (TLS1_2_VERSION, TLS1_3_VERSION), include/openssl/ssl.h.in
 * (SSL_VERIFY_*, SSL_ERROR_*, SSL_CTRL_*), include/openssl/tls1.h
 * (TLSEXT_NAMETYPE_host_name).
 */
#define TLS1_2_VERSION				0x0303
#define TLS1_3_VERSION				0x0304
#define SSL_VERIFY_NONE				0x00
#define SSL_VERIFY_PEER				0x01
#define SSL_ERROR_NONE				0
#define SSL_ERROR_SSL				1
#define SSL_ERROR_WANT_READ			2
#define SSL_ERROR_WANT_WRITE			3
#define SSL_ERROR_SYSCALL			5
#define SSL_ERROR_ZERO_RETURN			6
#define SSL_CTRL_SET_TLSEXT_HOSTNAME		55
#define SSL_CTRL_SET_MIN_PROTO_VERSION		123
#define SSL_CTRL_SET_MAX_PROTO_VERSION		124
#define SSL_CTRL_GET_MIN_PROTO_VERSION		130
#define SSL_CTRL_GET_MAX_PROTO_VERSION		131
#define TLSEXT_NAMETYPE_host_name		0

/*
 * The options this library acts on, as uint64_t bits.  Other bits are
 * stored and returned by SSL_CTX_set_options but change nothing.
 * Values referenced: OpenSSL 3.5.8 include/openssl/ssl.h.in (SSL_OP_BIT,
 * SSL_OP_IGNORE_UNEXPECTED_EOF, SSL_OP_NO_TLSv1_2, SSL_OP_NO_TLSv1_3).
 */
#define SSL_OP_IGNORE_UNEXPECTED_EOF		(((uint64_t)1) << 7)
#define SSL_OP_NO_TLSv1_2			(((uint64_t)1) << 27)
#define SSL_OP_NO_TLSv1_3			(((uint64_t)1) << 29)

/*
 * The reasons libssl-compat queues (ERR_GET_REASON of an ERR_LIB_SSL
 * code), and the offset of an alert received (reason = offset + the
 * alert's description).
 * Values referenced: OpenSSL 3.5.8 include/openssl/sslerr.h (SSL_R_*),
 * include/openssl/ssl.h.in (SSL_AD_REASON_OFFSET).
 */
#define SSL_R_BAD_ECPOINT				306
#define SSL_R_BAD_EXTENSION				110
#define SSL_R_BAD_HANDSHAKE_LENGTH			332
#define SSL_R_BAD_KEY_SHARE				108
#define SSL_R_BAD_LENGTH				271
#define SSL_R_BAD_SIGNATURE				123
#define SSL_R_CERTIFICATE_VERIFY_FAILED			134
#define SSL_R_DECRYPTION_FAILED_OR_BAD_RECORD_MAC	281
#define SSL_R_DIGEST_CHECK_FAILED			149
#define SSL_R_EXCESSIVE_MESSAGE_SIZE			152
#define SSL_R_HTTP_REQUEST				156
#define SSL_R_LENGTH_MISMATCH				159
#define SSL_R_NO_PROTOCOLS_AVAILABLE			191
#define SSL_R_NO_SHARED_CIPHER				193
#define SSL_R_NO_SUITABLE_KEY_SHARE			101
#define SSL_R_PACKET_LENGTH_TOO_LONG			198
#define SSL_R_PROTOCOL_IS_SHUTDOWN			207
#define SSL_R_RECORD_LENGTH_MISMATCH			213
#define SSL_R_SSL_HANDSHAKE_FAILURE			229
#define SSL_R_UNEXPECTED_EOF_WHILE_READING		294
#define SSL_R_UNEXPECTED_MESSAGE			244
#define SSL_R_UNSUPPORTED_PROTOCOL			258
#define SSL_R_WRONG_CIPHER_RETURNED			261
#define SSL_R_WRONG_CURVE				378
#define SSL_R_WRONG_SIGNATURE_TYPE			370
#define SSL_R_WRONG_VERSION_NUMBER			267
#define SSL_AD_REASON_OFFSET				1000

/* SSL_set_tlsext_host_name as OpenSSL defines it (a ctrl). */
#define SSL_set_tlsext_host_name(ssl, name) \
	SSL_ctrl((ssl), SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, (void *)(name))

/* SSL_CTX_set_min_proto_version and its kin as OpenSSL defines them (ctrls). */
#define SSL_CTX_set_min_proto_version(ctx, version) \
	SSL_CTX_ctrl((ctx), SSL_CTRL_SET_MIN_PROTO_VERSION, (version), NULL)
#define SSL_CTX_set_max_proto_version(ctx, version) \
	SSL_CTX_ctrl((ctx), SSL_CTRL_SET_MAX_PROTO_VERSION, (version), NULL)

/* Returns the client method (a constant object). */
const SSL_METHOD *TLS_client_method(void);

/*
 * Makes a context: the client method only (another is NULL with an
 * error); versions TLS 1.2 to 1.3; SSL_VERIFY_NONE; an empty store.
 * Returns it with one reference, or NULL.
 */
SSL_CTX *SSL_CTX_new(const SSL_METHOD *method);

/* Drops one reference to a context (each SSL holds one); NULL is ignored. */
void SSL_CTX_free(SSL_CTX *ctx);

/* Sets the verify mode (SSL_VERIFY_NONE or SSL_VERIFY_PEER; other bits ignored) and callback (may be NULL) of SSLs made after. */
void SSL_CTX_set_verify(SSL_CTX *ctx, int mode, SSL_verify_cb callback);

/*
 * Adds the CERTIFICATE blocks of the PEM file at file (UTF-8) to the
 * store.  path must be NULL (directories of hashed names are not
 * supported: 0).  Returns 1, or 0 when the file cannot be read or holds
 * no certificate or a bad one (the good ones stay added).
 */
int SSL_CTX_load_verify_locations(SSL_CTX *ctx, const char *file, const char *path);

/*
 * Adds the system's roots: /etc/ssl/cert.pem on zedBSD; the Windows ROOT
 * stores (minus Disallowed) and cert.pem beside the DLL on Windows.
 * Returns 1 even when there are none (OpenSSL's behaviour), 0 on a read
 * error other than a missing file.
 */
int SSL_CTX_set_default_verify_paths(SSL_CTX *ctx);

/* Adds options (OR) and returns the new set. */
uint64_t SSL_CTX_set_options(SSL_CTX *ctx, uint64_t options);

/*
 * The context ctrls: SSL_CTRL_SET_MIN_PROTO_VERSION and _MAX_ (number 0
 * for the lowest or highest supported, TLS1_2_VERSION or TLS1_3_VERSION;
 * another value returns 0), _GET_MIN_ and _GET_MAX_ (return the setting,
 * 0 when unset).  Another command returns 0.
 */
long SSL_CTX_ctrl(SSL_CTX *ctx, int command, long number, void *pointer);

/* Returns the context's store (owned by the context; X509_STORE_add_cert adds to it). */
X509_STORE *SSL_CTX_get_cert_store(const SSL_CTX *ctx);

/* Makes a connection object from a context (taking a reference to it); NULL on failure. */
SSL *SSL_new(SSL_CTX *ctx);

/* Frees a connection (without sending anything; SSL_shutdown first for a clean close); NULL is ignored. */
void SSL_free(SSL *ssl);

/*
 * Sets the socket: a file descriptor on zedBSD, a SOCKET (as int) on
 * Windows.  Returns 1, or 0 for a negative value.
 */
int SSL_set_fd(SSL *ssl, int descriptor);

/*
 * The connection ctrls: SSL_CTRL_SET_TLSEXT_HOSTNAME with
 * TLSEXT_NAMETYPE_host_name and a host name (the SNI name; 1, or 0 for a
 * name that is not a valid host name), and the version ctrls as
 * SSL_CTX_ctrl.  Another command returns 0.
 */
long SSL_ctrl(SSL *ssl, int command, long number, void *pointer);

/* Sets the host name the certificate must match (X509_VERIFY_PARAM_set1_host on the SSL's parameters). */
int SSL_set1_host(SSL *ssl, const char *host);

/* Returns the SSL's verification parameters (owned by the SSL). */
X509_VERIFY_PARAM *SSL_get0_param(SSL *ssl);

/*
 * Runs the handshake.  Returns 1 when done; 0 or -1 on failure or when
 * the socket would block (SSL_get_error tells which); call again with the
 * same SSL after the socket is ready.
 */
int SSL_connect(SSL *ssl);

/*
 * Reads up to length bytes of application data (running the handshake
 * first when it is not done).  Returns the count (> 0), or 0 or -1
 * (SSL_get_error: WANT_READ, WANT_WRITE, ZERO_RETURN after the peer's
 * close_notify or, with SSL_OP_IGNORE_UNEXPECTED_EOF, after an EOF
 * without it; SYSCALL with errno; SSL otherwise).
 */
int SSL_read(SSL *ssl, void *buffer, int length);

/*
 * Writes length bytes (all of them, in records of at most 16 KiB) and
 * returns length, or 0 or -1 as SSL_read.  After WANT_WRITE or WANT_READ
 * the call must be repeated with the same buffer and length (OpenSSL's
 * rule); the bytes already taken are not taken again.
 */
int SSL_write(SSL *ssl, const void *buffer, int length);

/*
 * Classifies the result of the last SSL_connect, SSL_read, SSL_write or
 * SSL_shutdown on ssl (result is its return value): SSL_ERROR_NONE for a
 * positive result; WANT_READ or WANT_WRITE when the socket would block;
 * ZERO_RETURN for a clean close; SYSCALL for a socket failure (errno set;
 * an EOF without close_notify, without SSL_OP_IGNORE_UNEXPECTED_EOF, is
 * SSL_ERROR_SSL with SSL_R_UNEXPECTED_EOF_WHILE_READING queued, as in
 * OpenSSL 3); SSL_ERROR_SSL otherwise.
 */
int SSL_get_error(const SSL *ssl, int result);

/* Returns the decrypted bytes buffered and not yet read. */
int SSL_pending(const SSL *ssl);

/*
 * Sends close_notify (once).  Returns 0 after sending it when the peer's
 * has not been received, 1 when both have, -1 on failure or blocking
 * (SSL_get_error).
 */
int SSL_shutdown(SSL *ssl);

/* Returns the X509_V_ result of the last handshake's verification (X509_V_OK before one). */
long SSL_get_verify_result(const SSL *ssl);

#ifdef __cplusplus
}
#endif

#endif
