/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * One TLS client connection over a transport: the record layer's I/O,
 * the reassembly of handshake messages, the driving of the handshake
 * (sslc-hs.h), application data, alerts and the closing exchange (RFC
 * 8446 5 and 6; RFC 5246 6 and 7.2).
 *
 * The transport may be non-blocking.  Every operation can stop with
 * EAGAIN and *wants telling whether it waits to read or to write; the
 * caller repeats the same operation when the socket is ready, and the
 * connection continues where it stopped (bytes already read are kept;
 * a record already sealed is kept whole until it is sent, so a record is
 * never sent twice or in part followed by another).
 *
 * Reading never runs ahead: the connection asks the transport for at most
 * the rest of the current record (first the 5-byte header, then exactly
 * the length the header gives), so no byte of a later record is ever held
 * inside the connection.  A program that polls the socket whenever
 * SSL_pending() is 0 (libbrowser does) then never waits for bytes that
 * were already read (OpenSSL's default, read_ahead off).
 *
 * Checks besides those of the record and handshake modules:
 *   - handshake messages are reassembled across records up to
 *     SSLC_HANDSHAKE_MAX bytes (a longer one is EMSGSIZE with the alert
 *     decode_error);
 *   - a key change must fall on a record boundary (RFC 8446 5.1): when the
 *     handshake gives SSLC_HS_SET_READ_KEYS, any byte left unread in the
 *     current record or in the reassembly buffer is unexpected_message;
 *     the same for a TLS 1.2 ChangeCipherSpec that arrives while a
 *     handshake message is incomplete;
 *   - TLS 1.3 forbids zero-length handshake and alert fragments and
 *     interleaving (5.1); a record of another type while a handshake
 *     message is incomplete is unexpected_message;
 *   - a TLS 1.2 application data record whose decrypted plaintext is empty
 *     is accepted and counted as a record without progress;
 *   - application data before the handshake ends is unexpected_message;
 *   - alerts: a received close_notify ends the reading side (read returns
 *     EPIPE); in TLS 1.3 user_canceled is a closure alert, ignored and
 *     counted as a record without progress (6.1), and any other alert is
 *     fatal (6.2); in TLS 1.2 a warning alert other than close_notify is
 *     ignored and counted, a fatal one ends the connection; an alert record
 *     that is not exactly 2 bytes is decode_error;
 *   - at most SSLC_CONN_MAX_EMPTY consecutive records that carry no
 *     application data and no handshake progress (warnings,
 *     user_canceled, change cipher spec, empty TLS 1.2 data records,
 *     post-handshake NewSessionTicket and KeyUpdate messages) are
 *     accepted; more is unexpected_message (a peer cannot make the client
 *     loop or rekey forever).
 *
 * On a failure the connection sends the fatal alert the failing module
 * named (internal_error when none; nothing after an alert was received or
 * the transport failed; never a close_notify after a fatal alert), marks
 * itself failed, and every later operation
 * returns EPROTO.  The reason for the public error queue is in
 * conn->reason (an SSL_R_ value of <compat/ssl/ssl.h>, or 1000 plus a
 * received alert's description).
 */

#ifndef LIBSSL_COMPAT_SSLC_CONN_H
#define LIBSSL_COMPAT_SSLC_CONN_H

#include "sslc-hs.h"

/* The most consecutive records without progress (see the file comment). */
#define SSLC_CONN_MAX_EMPTY	32U

/* The bits of *wants: what an EAGAIN waits for. */
#define SSLC_WANT_READ		1U
#define SSLC_WANT_WRITE		2U

/*
 * A byte transport.  read and write move up to size bytes and return 0
 * with *done set (a read of 0 bytes is the end of the stream), EAGAIN, or
 * EIO with *os_error set; they are cryc_os_socket_read and _write over a
 * descriptor in the library, and memory pipes in the unit tests.
 */
struct sslc_transport {
	int (*read)(void *context, uint8_t *buffer, size_t size, size_t *done, int *os_error);
	int (*write)(void *context, const uint8_t *buffer, size_t size, size_t *done, int *os_error);
	void *context;
};

/*
 * A connection.
 *
 * config is a copy of the handshake configuration (owned; its store is
 * borrowed from the SSL_CTX, which outlives the SSL).  hs the handshake;
 * reading and writing the two record directions.  in holds the record
 * being read (in_used bytes so far; a whole record is up to
 * SSLC_RECORD_BUFFER bytes); after it is opened, plain and
 * plain_size are the application data not yet given to the caller (a view
 * into in).  message is the malloc'd reassembly buffer of a handshake
 * message (message_used bytes).  out holds sealed bytes not yet written
 * (out_sent of out_used written).  taken counts the bytes of the current
 * sslc_conn_write call already sealed (OpenSSL's retry rule).
 * flags: SSLC_CONN_ bits.  empty counts records without progress.
 * reason, os_error and alert_received describe the last failure.
 */
struct sslc_conn {
	struct sslc_hs_config config;
	struct sslc_hs hs;
	struct sslc_record_dir reading;
	struct sslc_record_dir writing;
	struct sslc_transport transport;
	uint8_t in[SSLC_RECORD_BUFFER];
	size_t in_used;
	uint8_t *plain;
	size_t plain_size;
	uint8_t *message;
	size_t message_used;
	uint8_t out[SSLC_RECORD_BUFFER + SSLC_HS_OUT_MAX];
	size_t out_used;
	size_t out_sent;
	size_t taken;
	uint32_t flags;
	unsigned empty;
	unsigned reason;
	int os_error;
	unsigned alert_received;
};

/* The bits of flags. */
#define SSLC_CONN_STARTED		0x0001U
#define SSLC_CONN_CONNECTED		0x0002U
#define SSLC_CONN_CLOSE_RECEIVED	0x0004U
#define SSLC_CONN_CLOSE_SENT		0x0008U
#define SSLC_CONN_FAILED		0x0010U
#define SSLC_CONN_IGNORE_EOF		0x0020U

/*
 * Makes a connection (no I/O yet).  config is copied; flags may contain
 * SSLC_CONN_IGNORE_EOF (SSL_OP_IGNORE_UNEXPECTED_EOF).  Returns 0 or
 * ENOMEM.
 */
int sslc_conn_init(struct sslc_conn *conn, const struct sslc_hs_config *config, const struct sslc_transport *transport, uint32_t flags);

/*
 * Runs the handshake until it is done or must wait.
 *
 * Returns 0 when connected; EAGAIN with *wants; EPIPE when the peer
 * closed (EOF or close_notify) during the handshake; EIO for a transport
 * failure (conn->os_error); EPROTO and the other errors of the handshake
 * modules for a TLS failure (an alert was sent).
 */
int sslc_conn_handshake(struct sslc_conn *conn, unsigned *wants);

/*
 * Reads up to size bytes of application data into buffer, running the
 * handshake first when needed.
 *
 * Returns 0 with *done > 0; EAGAIN with *wants; EPIPE after the peer's
 * close_notify (or after an EOF without it when SSLC_CONN_IGNORE_EOF is
 * set); ECONNRESET after an EOF without close_notify otherwise; EIO;
 * EPROTO and the like as sslc_conn_handshake.  size must be at least 1.
 */
int sslc_conn_read(struct sslc_conn *conn, uint8_t *buffer, size_t size, size_t *done, unsigned *wants);

/*
 * Writes size bytes of application data (in records of at most
 * SSLC_RECORD_PLAINTEXT_MAX), running the handshake first when needed.
 *
 * Returns 0 with *done == size when every record is written; EAGAIN with
 * *wants after part of it (repeat with the same buffer and size: the
 * bytes already taken are skipped); EPIPE when this side already sent
 * close_notify; EIO; EPROTO.
 */
int sslc_conn_write(struct sslc_conn *conn, const uint8_t *buffer, size_t size, size_t *done, unsigned *wants);

/* Returns the decrypted application bytes buffered (conn->plain_size). */
size_t sslc_conn_pending(const struct sslc_conn *conn);

/*
 * Sends close_notify once (after flushing what is pending) and reports
 * whether the peer's was received: *both is 1 when it was.  Returns 0;
 * EAGAIN with *wants; EIO.  Does not wait for the peer's close_notify.
 */
int sslc_conn_shutdown(struct sslc_conn *conn, int *both, unsigned *wants);

/* Wipes and frees everything the connection holds (the transport's context is the caller's). */
void sslc_conn_free(struct sslc_conn *conn);

#endif
