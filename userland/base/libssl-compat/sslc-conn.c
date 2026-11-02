/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * One TLS client connection over a transport.
 */

#include <stdlib.h>
#include <string.h>

#include <compat/ssl/ssl.h>

#include "sslc-conn.h"

/*
 * The room of the reassembly buffer: the largest handshake message plus
 * one more record's plaintext.  A record is appended only when no whole
 * message waits in the buffer, so what is held is then the beginning of a
 * message that is not yet complete (less than SSLC_HANDSHAKE_MAX bytes).
 */
#define SSLC_CONN_MESSAGE_ROOM		(SSLC_HANDSHAKE_MAX + SSLC_RECORD_PLAINTEXT_MAX)

/* The size of an alert's content: its level and its description (RFC 8446 6; RFC 5246 7.2). */
#define SSLC_CONN_ALERT_SIZE		2U

/* The content of a change_cipher_spec record (RFC 8446 5; RFC 5246 7.1). */
#define SSLC_CONN_CCS_VALUE		1U

/*
 * What an SSL_R_ reason says for the alert a handshake failure sent.  The
 * handshake module names an alert and not a reason, so the connection gives
 * the public error queue the nearest reason of <compat/ssl/ssl.h>; the
 * alerts not in the table give SSL_R_SSL_HANDSHAKE_FAILURE, and
 * internal_error gives no reason (0: the errno value says what happened).
 * The failures of the record layer and of the connection itself name their
 * reasons where they are found.
 */
struct sslc_conn_alert_reason {
	unsigned alert;
	unsigned reason;
};

/*
 * The alerts a handshake can fail with that have a nearer reason (the
 * certificate alerts of sslc-hs.h, decode_error, decrypt_error,
 * protocol_version, the extension alerts, unexpected_message).
 * Values referenced: RFC 8446 Appendix B.2 (the alert descriptions);
 * OpenSSL 3.5.8 include/openssl/sslerr.h (the reasons).
 */
static const struct sslc_conn_alert_reason sslc_conn_alert_reasons[] = {
	{ SSLC_ALERT_UNEXPECTED_MESSAGE, SSL_R_UNEXPECTED_MESSAGE },
	{ SSLC_ALERT_HANDSHAKE_FAILURE, SSL_R_SSL_HANDSHAKE_FAILURE },
	{ SSLC_ALERT_BAD_CERTIFICATE, SSL_R_CERTIFICATE_VERIFY_FAILED },
	{ SSLC_ALERT_UNSUPPORTED_CERTIFICATE, SSL_R_CERTIFICATE_VERIFY_FAILED },
	{ SSLC_ALERT_CERTIFICATE_EXPIRED, SSL_R_CERTIFICATE_VERIFY_FAILED },
	{ SSLC_ALERT_CERTIFICATE_UNKNOWN, SSL_R_CERTIFICATE_VERIFY_FAILED },
	{ SSLC_ALERT_UNKNOWN_CA, SSL_R_CERTIFICATE_VERIFY_FAILED },
	{ SSLC_ALERT_DECODE_ERROR, SSL_R_BAD_LENGTH },
	{ SSLC_ALERT_DECRYPT_ERROR, SSL_R_BAD_SIGNATURE },
	{ SSLC_ALERT_PROTOCOL_VERSION, SSL_R_UNSUPPORTED_PROTOCOL },
	{ SSLC_ALERT_MISSING_EXTENSION, SSL_R_BAD_EXTENSION },
	{ SSLC_ALERT_UNSUPPORTED_EXTENSION, SSL_R_BAD_EXTENSION },
	{ SSLC_ALERT_INTERNAL_ERROR, 0U }
};

/* The number of entries of sslc_conn_alert_reasons. */
#define SSLC_CONN_ALERT_REASON_COUNT	(sizeof(sslc_conn_alert_reasons) / sizeof(sslc_conn_alert_reasons[0]))

/* Gives the reason of the public error queue for an alert. */
static unsigned sslc_conn_reason_of(unsigned alert);

/* Marks the connection failed, sends the fatal alert (0: none), and wipes what it holds; returns error. */
static int sslc_conn_fail(struct sslc_conn *conn, int error, unsigned reason, unsigned alert);

/* Fails the connection with the failure the handshake module recorded. */
static int sslc_conn_fail_handshake(struct sslc_conn *conn, int error);

/* Seals one record into the output with a direction's keys. */
static int sslc_conn_seal(struct sslc_conn *conn, struct sslc_record_dir *dir, unsigned type, int first_hello, const uint8_t *data, size_t size);

/* Seals an alert of level and description under the write keys. */
static int sslc_conn_queue_alert(struct sslc_conn *conn, unsigned level, unsigned description);

/* Writes the sealed output to the transport; EAGAIN with *wants, or EIO, without failing the connection. */
static int sslc_conn_write_out(struct sslc_conn *conn, unsigned *wants);

/* Writes the sealed output to the transport; a transport failure fails the connection. */
static int sslc_conn_flush(struct sslc_conn *conn, unsigned *wants);

/* Counts one record without progress; too many in a row fail the connection. */
static int sslc_conn_count_empty(struct sslc_conn *conn);

/* Carries out the actions of the handshake in order; wipes them. */
static int sslc_conn_apply(struct sslc_conn *conn, struct sslc_hs_actions *actions);

/* Seals the handshake bytes of a SEND action as handshake records. */
static int sslc_conn_apply_send(struct sslc_conn *conn, const struct sslc_hs_action *action);

/* Seals the compatibility change_cipher_spec record, which is never protected. */
static int sslc_conn_apply_ccs(struct sslc_conn *conn);

/* Installs the read keys of an action; the record and the buffer must end where the keys change. */
static int sslc_conn_apply_read_keys(struct sslc_conn *conn, const struct sslc_hs_action *action);

/* Hands every whole handshake message in the reassembly buffer to the handshake. */
static int sslc_conn_feed_messages(struct sslc_conn *conn, unsigned *wants);

/* Runs the handshake until it is complete, the peer closes, or the transport must wait. */
static int sslc_conn_run_handshake(struct sslc_conn *conn, unsigned *wants);

/* Fails the connection after a record header that was refused. */
static int sslc_conn_header_failure(struct sslc_conn *conn, int error);

/* Fails the connection after a record that did not open. */
static int sslc_conn_record_failure(struct sslc_conn *conn, int error);

/* Reads exactly one record (no byte of a later one) into the input buffer. */
static int sslc_conn_fill(struct sslc_conn *conn, unsigned *wants);

/* Answers the end of the stream. */
static int sslc_conn_end_of_stream(struct sslc_conn *conn);

/* Opens the record in the input buffer and acts on it. */
static int sslc_conn_record(struct sslc_conn *conn);

/* Takes the plaintext of a handshake record into the reassembly buffer. */
static int sslc_conn_on_handshake(struct sslc_conn *conn, const struct sslc_record *record);

/* Takes the plaintext of an alert record. */
static int sslc_conn_on_alert(struct sslc_conn *conn, const struct sslc_record *record);

/* Takes a change_cipher_spec record. */
static int sslc_conn_on_change_cipher_spec(struct sslc_conn *conn, const struct sslc_record *record);

/* Takes the plaintext of an application data record. */
static int sslc_conn_on_data(struct sslc_conn *conn, const struct sslc_record *record);

/*
 * Makes a connection (no I/O yet).
 */
int
sslc_conn_init(
	struct sslc_conn *conn,
	const struct sslc_hs_config *config,
	const struct sslc_transport *transport,
	uint32_t flags)
{
	/* Refuses a call without its parts. */
	if (conn == NULL)
		return EINVAL;
	if (config == NULL)
		return EINVAL;
	if (transport == NULL)
		return EINVAL;
	if (transport->read == NULL)
		return EINVAL;
	if (transport->write == NULL)
		return EINVAL;

	/* Starts from nothing: no byte held, no key, no state. */
	memset(conn, 0, sizeof(*conn));

	/* The configuration and the transport are the connection's own copies. */
	conn->config = *config;
	conn->transport = *transport;

	/* Both directions send and expect plaintext records until the handshake gives keys. */
	sslc_record_init(&conn->reading);
	sslc_record_init(&conn->writing);

	/* Only the flag of the caller's choice is kept. */
	conn->flags = flags & SSLC_CONN_IGNORE_EOF;

	/* Succeeded: nothing is allocated until the handshake reads a message. */
	return 0;
}

/*
 * Runs the handshake until it is done or must wait.
 */
int
sslc_conn_handshake(
	struct sslc_conn *conn,
	unsigned *wants)
{
	struct sslc_hs_actions actions;
	int error;

	/* Refuses a call without its parts. */
	if (conn == NULL)
		return EINVAL;
	if (wants == NULL)
		return EINVAL;
	*wants = 0;

	/* A failed connection takes nothing more. */
	if ((conn->flags & SSLC_CONN_FAILED) != 0U)
		return EPROTO;

	/* The first call sends the ClientHello. */
	if ((conn->flags & SSLC_CONN_STARTED) == 0U) {
		/* Nothing was sent when the start fails, so there is no alert to send. */
		error = sslc_hs_start(&conn->hs, &conn->config, &actions);
		if (error != 0)
			return sslc_conn_fail(conn, error, 0U, 0U);

		/* STARTED tells the public layer that the ClientHello is in the output. */
		conn->flags |= SSLC_CONN_STARTED;
		error = sslc_conn_apply(conn, &actions);
		if (error != 0)
			return error;
	}

	/* Messages and records until the handshake is over. */
	error = sslc_conn_run_handshake(conn, wants);
	if (error != 0)
		return error;

	/* Succeeded: the handshake is complete and the last flight is sent. */
	return 0;
}

/*
 * Reads up to size bytes of application data into buffer, running the
 * handshake first when needed.
 */
int
sslc_conn_read(
	struct sslc_conn *conn,
	uint8_t *buffer,
	size_t size,
	size_t *done,
	unsigned *wants)
{
	size_t count;
	int error;

	/* Refuses a call without its parts. */
	if (conn == NULL)
		return EINVAL;
	if (buffer == NULL)
		return EINVAL;
	if (done == NULL)
		return EINVAL;
	if (wants == NULL)
		return EINVAL;
	*done = 0;
	*wants = 0;

	/* A read of no bytes is a mistake of the caller (the result 0 means an end of stream). */
	if (size == 0U)
		return EINVAL;

	/* A failed connection takes nothing more. */
	if ((conn->flags & SSLC_CONN_FAILED) != 0U)
		return EPROTO;

	/* The handshake comes first. */
	if ((conn->flags & SSLC_CONN_CONNECTED) == 0U) {
		error = sslc_conn_handshake(conn, wants);
		if (error != 0)
			return error;
	}

	/* Application bytes already decrypted, or the next record. */
	while (1) {
		/* Gives what is buffered before touching the transport. */
		if (conn->plain_size > 0U) {
			count = conn->plain_size;
			if (count > size)
				count = size;
			memcpy(buffer, conn->plain, count);
			conn->plain += count;
			conn->plain_size -= count;

			/* The record is used up: the input buffer can take the next one. */
			if (conn->plain_size == 0U) {
				conn->plain = NULL;
				cryc_wipe(conn->in, conn->in_used);
				conn->in_used = 0;
			}
			*done = count;

			/* Succeeded: bytes of application data were given. */
			return 0;
		}

		/* A post-handshake message that waits for room in the output. */
		error = sslc_conn_feed_messages(conn, wants);
		if (error != 0)
			return error;

		/* What those messages answered goes out before the next read. */
		error = sslc_conn_flush(conn, wants);
		if (error != 0)
			return error;

		/* The peer's closing ends the reading side. */
		if ((conn->flags & SSLC_CONN_CLOSE_RECEIVED) != 0U)
			return EPIPE;

		/* The next record, whole, and what it says. */
		error = sslc_conn_fill(conn, wants);
		if (error != 0)
			return error;
		error = sslc_conn_record(conn);
		if (error != 0)
			return error;
	}
}

/*
 * Writes size bytes of application data (in records of at most
 * SSLC_RECORD_PLAINTEXT_MAX), running the handshake first when needed.
 */
int
sslc_conn_write(
	struct sslc_conn *conn,
	const uint8_t *buffer,
	size_t size,
	size_t *done,
	unsigned *wants)
{
	size_t part;
	int error;

	/* Refuses a call without its parts. */
	if (conn == NULL)
		return EINVAL;
	if (buffer == NULL && size > 0U)
		return EINVAL;
	if (done == NULL)
		return EINVAL;
	if (wants == NULL)
		return EINVAL;
	*done = 0;
	*wants = 0;

	/* A failed connection takes nothing more. */
	if ((conn->flags & SSLC_CONN_FAILED) != 0U)
		return EPROTO;

	/* A retry that names fewer bytes than were already taken is not the same call. */
	if (conn->taken > size)
		return EINVAL;

	/* The handshake comes first. */
	if ((conn->flags & SSLC_CONN_CONNECTED) == 0U) {
		error = sslc_conn_handshake(conn, wants);
		if (error != 0)
			return error;
	}

	/* After this side's close_notify nothing more may be sent. */
	if ((conn->flags & SSLC_CONN_CLOSE_SENT) != 0U)
		return EPIPE;

	/* The records sealed by an earlier try are sent before anything new is sealed. */
	error = sslc_conn_flush(conn, wants);
	if (error != 0)
		return error;

	/* Seals and sends the bytes not yet taken, one record at a time. */
	while (conn->taken < size) {
		part = size - conn->taken;
		if (part > SSLC_RECORD_PLAINTEXT_MAX)
			part = SSLC_RECORD_PLAINTEXT_MAX;

		/* The record stays whole in the output until it is sent. */
		error = sslc_conn_seal(conn, &conn->writing, SSLC_CT_APPLICATION_DATA, 0, buffer + conn->taken, part);
		if (error != 0)
			return sslc_conn_fail(conn, error, 0U, SSLC_ALERT_INTERNAL_ERROR);
		conn->taken += part;

		/* A record that cannot be sent yet is kept, and counted as taken. */
		error = sslc_conn_flush(conn, wants);
		if (error != 0)
			return error;
	}

	/* Every byte is taken and sent; the next call starts a new write. */
	conn->taken = 0;
	*done = size;

	/* Succeeded: every record of this write is on the transport. */
	return 0;
}

/*
 * Returns the decrypted application bytes buffered (conn->plain_size).
 */
size_t
sslc_conn_pending(
	const struct sslc_conn *conn)
{
	/* A missing connection holds nothing. */
	if (conn == NULL)
		return 0;

	/* The bytes of the opened record not yet given to the caller. */
	return conn->plain_size;
}

/*
 * Sends close_notify once (after flushing what is pending) and reports
 * whether the peer's was received: *both is 1 when it was.
 */
int
sslc_conn_shutdown(
	struct sslc_conn *conn,
	int *both,
	unsigned *wants)
{
	int error;

	/* Refuses a call without its parts. */
	if (conn == NULL)
		return EINVAL;
	if (both == NULL)
		return EINVAL;
	if (wants == NULL)
		return EINVAL;
	*both = 0;
	*wants = 0;

	/* No close_notify follows a fatal alert, and none is sent before the handshake is over. */
	if ((conn->flags & SSLC_CONN_FAILED) != 0U)
		return EPROTO;
	if ((conn->flags & SSLC_CONN_CONNECTED) == 0U)
		return EPROTO;

	/* The bytes sealed earlier go first, then the close_notify, once. */
	if ((conn->flags & SSLC_CONN_CLOSE_SENT) == 0U) {
		error = sslc_conn_flush(conn, wants);
		if (error != 0)
			return error;

		/* CLOSE_SENT is set with the record sealed: a retry only sends what is left of it. */
		error = sslc_conn_queue_alert(conn, SSLC_ALERT_WARNING, SSLC_ALERT_CLOSE_NOTIFY);
		if (error != 0)
			return sslc_conn_fail(conn, error, 0U, SSLC_ALERT_INTERNAL_ERROR);
		conn->flags |= SSLC_CONN_CLOSE_SENT;
	}

	/* The close_notify, sent. */
	error = sslc_conn_flush(conn, wants);
	if (error != 0)
		return error;

	/* Reports whether the peer's close_notify was seen. */
	if ((conn->flags & SSLC_CONN_CLOSE_RECEIVED) != 0U)
		*both = 1;

	/* Succeeded: this side's close_notify is sent. */
	return 0;
}

/*
 * Wipes and frees everything the connection holds (the transport's context
 * is the caller's).
 */
void
sslc_conn_free(
	struct sslc_conn *conn)
{
	/* A missing connection holds nothing. */
	if (conn == NULL)
		return;

	/* The handshake's secrets and certificate copies, and the record keys. */
	sslc_hs_free(&conn->hs);
	sslc_record_clear(&conn->reading);
	sslc_record_clear(&conn->writing);

	/* The reassembly buffer (it held decrypted handshake messages). */
	if (conn->message != NULL)
		cryc_wipe(conn->message, SSLC_CONN_MESSAGE_ROOM);
	free(conn->message);

	/* Everything else: the buffers hold plaintext and ciphertext, and the structure ends up empty. */
	cryc_wipe(conn, sizeof(*conn));
}

/* Gives the reason of the public error queue for an alert. */
static unsigned
sslc_conn_reason_of(
	unsigned alert)
{
	size_t index;

	/* The table's entry for the alert, when there is one. */
	for (index = 0;
	     index < SSLC_CONN_ALERT_REASON_COUNT;
	     index++) {
		if (sslc_conn_alert_reasons[index].alert == alert)
			return sslc_conn_alert_reasons[index].reason;
	}

	/* Any other alert is a failed handshake as far as the caller is concerned. */
	return SSL_R_SSL_HANDSHAKE_FAILURE;
}

/*
 * Marks the connection failed, sends the fatal alert (0: none), and wipes
 * what it holds; returns error.
 *
 * The alert is sealed after what the output already holds, and sent once
 * if the transport takes it now; whether it went does not matter to the
 * caller.
 */
static int
sslc_conn_fail(
	struct sslc_conn *conn,
	int error,
	unsigned reason,
	unsigned alert)
{
	unsigned wants;

	/* A connection that already failed keeps its first reason. */
	if ((conn->flags & SSLC_CONN_FAILED) != 0U)
		return error;

	/* FAILED makes every later operation answer EPROTO. */
	conn->flags |= SSLC_CONN_FAILED;
	conn->reason = reason;

	/* The fatal alert, once, when the peer is to hear of it. */
	if (alert != 0U) {
		/* Sealed while the keys are still there; a full output or a wrapped sequence leaves it unsent. */
		(void)sslc_conn_queue_alert(conn, SSLC_ALERT_FATAL, alert);
		wants = 0;
		(void)sslc_conn_write_out(conn, &wants);
	}

	/* The secrets are of no use any more. */
	sslc_hs_free(&conn->hs);
	sslc_record_clear(&conn->reading);
	sslc_record_clear(&conn->writing);

	/* The bytes of the record in hand, and the reassembly buffer. */
	cryc_wipe(conn->in, conn->in_used);
	conn->in_used = 0;
	conn->plain = NULL;
	conn->plain_size = 0;
	if (conn->message != NULL)
		cryc_wipe(conn->message, SSLC_CONN_MESSAGE_ROOM);
	free(conn->message);
	conn->message = NULL;
	conn->message_used = 0;

	/* Reports the failure to the caller. */
	return error;
}

/* Fails the connection with the failure the handshake module recorded. */
static int
sslc_conn_fail_handshake(
	struct sslc_conn *conn,
	int error)
{
	unsigned alert;
	unsigned reason;

	/* An alert of 0 is a failure the peer is not to blame for: internal_error. */
	alert = conn->hs.alert;
	if (alert == 0U)
		alert = SSLC_ALERT_INTERNAL_ERROR;
	reason = sslc_conn_reason_of(alert);

	/* The module's own error value goes back to the caller. */
	return sslc_conn_fail(conn, error, reason, alert);
}

/* Seals one record into the output with a direction's keys. */
static int
sslc_conn_seal(
	struct sslc_conn *conn,
	struct sslc_record_dir *dir,
	unsigned type,
	int first_hello,
	const uint8_t *data,
	size_t size)
{
	size_t room;
	size_t written;
	int error;

	/* The sealed record goes after what the output holds. */
	room = sizeof(conn->out) - conn->out_used;
	error = sslc_record_seal(dir, type, first_hello, data, size, conn->out + conn->out_used, room, &written);
	if (error != 0)
		return error;
	conn->out_used += written;

	/* Succeeded: the record is in the output. */
	return 0;
}

/* Seals an alert of level and description under the write keys. */
static int
sslc_conn_queue_alert(
	struct sslc_conn *conn,
	unsigned level,
	unsigned description)
{
	uint8_t alert[SSLC_CONN_ALERT_SIZE];
	int error;

	/* Alert { level, description } (RFC 8446 6). */
	alert[0] = (uint8_t)level;
	alert[1] = (uint8_t)description;
	error = sslc_conn_seal(conn, &conn->writing, SSLC_CT_ALERT, 0, alert, sizeof(alert));
	if (error != 0)
		return error;

	/* Succeeded: the alert is in the output. */
	return 0;
}

/* Writes the sealed output to the transport; EAGAIN with *wants, or EIO, without failing the connection. */
static int
sslc_conn_write_out(
	struct sslc_conn *conn,
	unsigned *wants)
{
	size_t left;
	size_t done;
	int os_error;
	int error;

	/* Sends what is left of the output, in as many writes as the transport takes. */
	while (conn->out_sent < conn->out_used) {
		left = conn->out_used - conn->out_sent;
		done = 0;
		os_error = 0;
		error = conn->transport.write(conn->transport.context, conn->out + conn->out_sent, left, &done, &os_error);

		/* The transport cannot take more now. */
		if (error == EAGAIN) {
			*wants |= SSLC_WANT_WRITE;
			return EAGAIN;
		}

		/* A failure of the transport, with the system's own error value. */
		if (error != 0) {
			conn->os_error = os_error;
			return EIO;
		}

		/* A write that took nothing, or more than it was given, is a transport that cannot be trusted. */
		if (done == 0U || done > left) {
			conn->os_error = 0;
			return EIO;
		}
		conn->out_sent += done;
	}

	/* Everything is sent: the output starts over. */
	conn->out_used = 0;
	conn->out_sent = 0;

	/* Succeeded: the output is empty. */
	return 0;
}

/* Writes the sealed output to the transport; a transport failure fails the connection. */
static int
sslc_conn_flush(
	struct sslc_conn *conn,
	unsigned *wants)
{
	int error;

	/* The transport's answer. */
	error = sslc_conn_write_out(conn, wants);
	if (error == EIO) {
		/* The transport failed: no alert can be sent over it. */
		return sslc_conn_fail(conn, EIO, 0U, 0U);
	}
	if (error != 0)
		return error;

	/* Succeeded: nothing is waiting to be sent. */
	return 0;
}

/* Counts one record without progress; too many in a row fail the connection. */
static int
sslc_conn_count_empty(
	struct sslc_conn *conn)
{
	/*
	 * Records that carry nothing a caller can use (warnings, closure
	 * alerts, change cipher specs, empty data records, post-handshake
	 * messages) are counted in a row; a peer that sends only these is
	 * not making the client loop or rekey for ever.
	 */
	conn->empty++;
	if (conn->empty > SSLC_CONN_MAX_EMPTY)
		return sslc_conn_fail(conn, EPROTO, SSL_R_UNEXPECTED_MESSAGE, SSLC_ALERT_UNEXPECTED_MESSAGE);

	/* Succeeded: still within the limit. */
	return 0;
}

/* Carries out the actions of the handshake in order; wipes them. */
static int
sslc_conn_apply(
	struct sslc_conn *conn,
	struct sslc_hs_actions *actions)
{
	const struct sslc_hs_action *action;
	size_t index;
	int error;

	/* Each action in the order the handshake gave it: a key change takes effect for what follows it. */
	error = 0;
	for (index = 0;
	     index < actions->count && error == 0;
	     index++) {
		action = &actions->list[index];

		/*
		 * After our close_notify nothing more goes out (RFC 8446 6.1, RFC
		 * 5246 7.2.1): an answer the handshake queues to a message read
		 * since (a KeyUpdate, a no_renegotiation warning) and the write keys
		 * that go with it are dropped; reading goes on.
		 */
		if ((conn->flags & SSLC_CONN_CLOSE_SENT) != 0U &&
		    (action->kind == SSLC_HS_SEND || action->kind == SSLC_HS_SEND_CCS ||
		     action->kind == SSLC_HS_SEND_WARNING || action->kind == SSLC_HS_SET_WRITE_KEYS))
			continue;

		/* One kind at a time. */
		switch (action->kind) {
		case SSLC_HS_SEND:
			error = sslc_conn_apply_send(conn, action);
			break;
		case SSLC_HS_SEND_CCS:
			error = sslc_conn_apply_ccs(conn);
			break;
		case SSLC_HS_SEND_WARNING:
			/* A warning the handshake wants sent (no_renegotiation). */
			error = sslc_conn_queue_alert(conn, SSLC_ALERT_WARNING, action->alert);
			if (error != 0)
				error = sslc_conn_fail(conn, error, 0U, SSLC_ALERT_INTERNAL_ERROR);
			break;
		case SSLC_HS_SET_READ_KEYS:
			error = sslc_conn_apply_read_keys(conn, action);
			break;
		case SSLC_HS_SET_WRITE_KEYS:
			/* Everything sealed after this action uses the new keys, with the sequence number 0. */
			error = sslc_record_set_keys(&conn->writing, action->version13, &action->keys);
			if (error != 0)
				error = sslc_conn_fail(conn, error, 0U, SSLC_ALERT_INTERNAL_ERROR);
			break;
		case SSLC_HS_DONE:
			/* CONNECTED lets application data through and ends the handshake's reading. */
			conn->flags |= SSLC_CONN_CONNECTED;
			conn->empty = 0;
			break;
		default:
			/* An action the connection does not know is the handshake's mistake. */
			error = sslc_conn_fail(conn, EINVAL, 0U, SSLC_ALERT_INTERNAL_ERROR);
			break;
		}
	}

	/* The actions hold keys: wiped whatever happened. */
	cryc_wipe(actions, sizeof(*actions));

	/* Reports the first failure, if any. */
	if (error != 0)
		return error;

	/* Succeeded: every action is carried out. */
	return 0;
}

/* Seals the handshake bytes of a SEND action as handshake records. */
static int
sslc_conn_apply_send(
	struct sslc_conn *conn,
	const struct sslc_hs_action *action)
{
	size_t offset;
	size_t part;
	int error;

	/* The bytes must lie inside the handshake's output buffer. */
	if (action->offset > sizeof(conn->hs.out) ||
	    action->size > sizeof(conn->hs.out) - action->offset)
		return sslc_conn_fail(conn, EINVAL, 0U, SSLC_ALERT_INTERNAL_ERROR);

	/* Handshake records of at most a full fragment, under the write keys now in force. */
	offset = 0;
	while (offset < action->size) {
		part = action->size - offset;
		if (part > SSLC_RECORD_PLAINTEXT_MAX)
			part = SSLC_RECORD_PLAINTEXT_MAX;
		error = sslc_conn_seal(conn, &conn->writing, SSLC_CT_HANDSHAKE, action->first_hello, conn->hs.out + action->offset + offset, part);
		if (error != 0)
			return sslc_conn_fail(conn, error, 0U, SSLC_ALERT_INTERNAL_ERROR);
		offset += part;
	}

	/* Succeeded: the messages are in the output. */
	return 0;
}

/* Seals the compatibility change_cipher_spec record, which is never protected. */
static int
sslc_conn_apply_ccs(
	struct sslc_conn *conn)
{
	struct sslc_record_dir plaintext;
	uint8_t content[1];
	int error;

	/*
	 * The record is plaintext whatever keys the write direction has by
	 * now (RFC 8446 5: the compatibility record is not protected; TLS
	 * 1.2's goes before the keys are set), so it is sealed by a
	 * direction of its own without keys, which leaves the sequence
	 * numbers of the real one alone.
	 */
	sslc_record_init(&plaintext);
	content[0] = (uint8_t)SSLC_CONN_CCS_VALUE;
	error = sslc_conn_seal(conn, &plaintext, SSLC_CT_CHANGE_CIPHER_SPEC, 0, content, sizeof(content));
	if (error != 0)
		return sslc_conn_fail(conn, error, 0U, SSLC_ALERT_INTERNAL_ERROR);

	/* Succeeded: the record is in the output. */
	return 0;
}

/* Installs the read keys of an action; the record and the buffer must end where the keys change. */
static int
sslc_conn_apply_read_keys(
	struct sslc_conn *conn,
	const struct sslc_hs_action *action)
{
	int error;

	/*
	 * A key change falls on a record boundary (RFC 8446 5.1): the record
	 * and the reassembly buffer hold nothing after the message that
	 * brought the keys.
	 */
	if (conn->plain_size != 0U || conn->message_used != 0U)
		return sslc_conn_fail(conn, EPROTO, SSL_R_UNEXPECTED_MESSAGE, SSLC_ALERT_UNEXPECTED_MESSAGE);

	/* The next record is read with the new keys, from the sequence number 0. */
	error = sslc_record_set_keys(&conn->reading, action->version13, &action->keys);
	if (error != 0)
		return sslc_conn_fail(conn, error, 0U, SSLC_ALERT_INTERNAL_ERROR);

	/* Succeeded: the reading direction has its new keys. */
	return 0;
}

/* Hands every whole handshake message in the reassembly buffer to the handshake. */
static int
sslc_conn_feed_messages(
	struct sslc_conn *conn,
	unsigned *wants)
{
	struct sslc_hs_actions actions;
	size_t length;
	int error;

	/* One message at a time, until only the beginning of an incomplete one is left. */
	while (conn->message_used >= 4U) {
		/* The header gives the length; a message over the limit is refused before it is complete. */
		length = 4U + (size_t)cryc_load_be24(conn->message + 1);
		if (length > SSLC_HANDSHAKE_MAX)
			return sslc_conn_fail(conn, EMSGSIZE, SSL_R_EXCESSIVE_MESSAGE_SIZE, SSLC_ALERT_DECODE_ERROR);

		/* An incomplete message waits for the rest of its records. */
		if (conn->message_used < length)
			break;

		/* The output is empty before the handshake queues the answer to this message. */
		error = sslc_conn_flush(conn, wants);
		if (error != 0)
			return error;

		/* The handshake takes the whole message and says what to do. */
		error = sslc_hs_message(&conn->hs, conn->message, length, &actions);

		/* The message is used up either way. */
		memmove(conn->message, conn->message + length, conn->message_used - length);
		conn->message_used -= length;
		if (error != 0)
			return sslc_conn_fail_handshake(conn, error);

		/* The answers, the key changes. */
		error = sslc_conn_apply(conn, &actions);
		if (error != 0)
			return error;
	}

	/* A buffer that is empty after the handshake is complete is given back. */
	if (conn->message_used == 0U &&
	    conn->message != NULL &&
	    (conn->flags & SSLC_CONN_CONNECTED) != 0U) {
		cryc_wipe(conn->message, SSLC_CONN_MESSAGE_ROOM);
		free(conn->message);
		conn->message = NULL;
	}

	/* Succeeded: no whole message waits. */
	return 0;
}

/* Runs the handshake until it is complete, the peer closes, or the transport must wait. */
static int
sslc_conn_run_handshake(
	struct sslc_conn *conn,
	unsigned *wants)
{
	int error;

	/* The messages the handshake is owed, the flight it queued, then the next record. */
	while (1) {
		error = sslc_conn_feed_messages(conn, wants);
		if (error != 0)
			return error;

		/* The flight goes out before the connection waits for the peer's answer. */
		error = sslc_conn_flush(conn, wants);
		if (error != 0)
			return error;

		/* The handshake is complete and its last flight is sent. */
		if ((conn->flags & SSLC_CONN_CONNECTED) != 0U)
			return 0;

		/* The peer closed before the handshake was done. */
		if ((conn->flags & SSLC_CONN_CLOSE_RECEIVED) != 0U)
			return EPIPE;

		/* The next record, whole, and what it says. */
		error = sslc_conn_fill(conn, wants);
		if (error != 0)
			return error;
		error = sslc_conn_record(conn);
		if (error != 0)
			return error;
	}
}

/* Fails the connection after a record header that was refused. */
static int
sslc_conn_header_failure(
	struct sslc_conn *conn,
	int error)
{
	uint32_t version;
	size_t length;
	unsigned type;
	int version_wrong;

	/* The three fields the record module looked at. */
	type = conn->in[0];
	version = cryc_load_be16(conn->in + 1);
	length = (size_t)cryc_load_be16(conn->in + 3);

	/* A length over the limit is a record_overflow; a protected record too short for its tag a bad_record_mac. */
	if (error == EMSGSIZE)
		return sslc_conn_fail(conn, EMSGSIZE, SSL_R_PACKET_LENGTH_TOO_LONG, SSLC_ALERT_RECORD_OVERFLOW);
	if (error == EBADMSG)
		return sslc_conn_fail(conn, EBADMSG, SSL_R_DECRYPTION_FAILED_OR_BAD_RECORD_MAC, SSLC_ALERT_BAD_RECORD_MAC);

	/* EPROTO says three things: a content type that does not exist, a wrong version, or an empty plaintext record. */
	if (type < SSLC_CT_CHANGE_CIPHER_SPEC || type > SSLC_CT_APPLICATION_DATA)
		return sslc_conn_fail(conn, EPROTO, SSL_R_UNEXPECTED_MESSAGE, SSLC_ALERT_UNEXPECTED_MESSAGE);

	/* The version a record may carry: any of 0x0301 to 0x0303 before the server's version is known, then 0x0303. */
	version_wrong = 0;
	if (conn->hs.version == 0U) {
		if (version < 0x0301U || version > SSLC_TLS12)
			version_wrong = 1;
	} else {
		if (version != SSLC_TLS12)
			version_wrong = 1;
	}
	if (version_wrong)
		return sslc_conn_fail(conn, EPROTO, SSL_R_WRONG_VERSION_NUMBER, SSLC_ALERT_PROTOCOL_VERSION);

	/* An empty fragment of a plaintext handshake or alert record. */
	if (length == 0U)
		return sslc_conn_fail(conn, EPROTO, SSL_R_BAD_LENGTH, SSLC_ALERT_DECODE_ERROR);

	/* Anything else the module refused (a change_cipher_spec record that is not one byte long). */
	return sslc_conn_fail(conn, EPROTO, SSL_R_UNEXPECTED_MESSAGE, SSLC_ALERT_UNEXPECTED_MESSAGE);
}

/* Fails the connection after a record that did not open. */
static int
sslc_conn_record_failure(
	struct sslc_conn *conn,
	int error)
{
	/* The tag that does not verify is the same bad_record_mac whatever the cause (RFC 8446 5.2). */
	if (error == EBADMSG)
		return sslc_conn_fail(conn, EBADMSG, SSL_R_DECRYPTION_FAILED_OR_BAD_RECORD_MAC, SSLC_ALERT_BAD_RECORD_MAC);

	/* A plaintext over the limit. */
	if (error == EMSGSIZE)
		return sslc_conn_fail(conn, EMSGSIZE, SSL_R_PACKET_LENGTH_TOO_LONG, SSLC_ALERT_RECORD_OVERFLOW);

	/* A record of the wrong shape: an outer type other than application_data, or an inner plaintext of only zeros (5.4). */
	if (error == EPROTO)
		return sslc_conn_fail(conn, EPROTO, SSL_R_UNEXPECTED_MESSAGE, SSLC_ALERT_UNEXPECTED_MESSAGE);

	/* The sequence number about to wrap, or any other failure of the record layer. */
	return sslc_conn_fail(conn, error, 0U, SSLC_ALERT_INTERNAL_ERROR);
}

/* Reads exactly one record (no byte of a later one) into the input buffer. */
static int
sslc_conn_fill(
	struct sslc_conn *conn,
	unsigned *wants)
{
	size_t want;
	size_t total;
	size_t length;
	size_t done;
	unsigned type;
	uint16_t version_check;
	int os_error;
	int error;

	/* The header first, then exactly the length the header gives. */
	while (1) {
		/* What the transport is asked for: the rest of the header, or the rest of the record. */
		if (conn->in_used >= SSLC_RECORD_HEADER) {
			/* Any of 0x0301 to 0x0303 until the server's version is known, then 0x0303. */
			version_check = 0;
			if (conn->hs.version != 0U)
				version_check = SSLC_TLS12;
			error = sslc_record_header(&conn->reading, conn->in, version_check, &type, &length);
			if (error != 0)
				return sslc_conn_header_failure(conn, error);

			/* The record is complete. */
			total = SSLC_RECORD_HEADER + length;
			if (conn->in_used == total)
				return 0;
			want = total - conn->in_used;
		} else {
			want = SSLC_RECORD_HEADER - conn->in_used;
		}

		/* Reads at most the rest of the record: a byte of the next one is never taken. */
		done = 0;
		os_error = 0;
		error = conn->transport.read(conn->transport.context, conn->in + conn->in_used, want, &done, &os_error);

		/* Nothing to read now. */
		if (error == EAGAIN) {
			*wants |= SSLC_WANT_READ;
			return EAGAIN;
		}

		/* A failure of the transport: no alert can be sent over it. */
		if (error != 0) {
			conn->os_error = os_error;
			return sslc_conn_fail(conn, EIO, 0U, 0U);
		}

		/* The end of the stream. */
		if (done == 0U)
			return sslc_conn_end_of_stream(conn);

		/* A read that gave more than it was asked for is a transport that cannot be trusted. */
		if (done > want) {
			conn->os_error = 0;
			return sslc_conn_fail(conn, EIO, 0U, 0U);
		}
		conn->in_used += done;
	}
}

/* Answers the end of the stream. */
static int
sslc_conn_end_of_stream(
	struct sslc_conn *conn)
{
	/* The public error queue's reason when the end was not a close_notify. */
	conn->os_error = 0;
	conn->reason = SSL_R_UNEXPECTED_EOF_WHILE_READING;

	/* During the handshake the peer simply went away. */
	if ((conn->flags & SSLC_CONN_CONNECTED) == 0U)
		return EPIPE;

	/* With SSL_OP_IGNORE_UNEXPECTED_EOF the end counts as the peer's close_notify. */
	if ((conn->flags & SSLC_CONN_IGNORE_EOF) != 0U) {
		conn->flags |= SSLC_CONN_CLOSE_RECEIVED;
		return EPIPE;
	}

	/* Otherwise the connection was cut: the data may have been truncated. */
	return ECONNRESET;
}

/* Opens the record in the input buffer and acts on it. */
static int
sslc_conn_record(
	struct sslc_conn *conn)
{
	struct sslc_record record;
	unsigned outer_type;
	int error;

	/* The outer type, which TLS 1.3 shows as application_data for every protected record. */
	outer_type = conn->in[0];

	/* The tag, the padding and the inner type. */
	error = sslc_record_open(&conn->reading, conn->in, conn->in_used, &record);
	if (error != 0)
		return sslc_conn_record_failure(conn, error);

	/* A protected TLS 1.3 record that says change_cipher_spec inside is not the unprotected compatibility record. */
	if (conn->reading.suite != NULL &&
	    conn->reading.version13 != 0 &&
	    outer_type != SSLC_CT_CHANGE_CIPHER_SPEC &&
	    record.type == SSLC_CT_CHANGE_CIPHER_SPEC)
		return sslc_conn_fail(conn, EPROTO, SSL_R_UNEXPECTED_MESSAGE, SSLC_ALERT_UNEXPECTED_MESSAGE);

	/* A handshake message that is not complete may be followed by nothing but the rest of it (RFC 8446 5.1). */
	if (conn->message_used > 0U && record.type != SSLC_CT_HANDSHAKE)
		return sslc_conn_fail(conn, EPROTO, SSL_R_UNEXPECTED_MESSAGE, SSLC_ALERT_UNEXPECTED_MESSAGE);

	/* By the content type. */
	switch (record.type) {
	case SSLC_CT_HANDSHAKE:
		error = sslc_conn_on_handshake(conn, &record);
		break;
	case SSLC_CT_ALERT:
		error = sslc_conn_on_alert(conn, &record);
		break;
	case SSLC_CT_CHANGE_CIPHER_SPEC:
		error = sslc_conn_on_change_cipher_spec(conn, &record);
		break;
	case SSLC_CT_APPLICATION_DATA:
		error = sslc_conn_on_data(conn, &record);
		break;
	default:
		/* A TLS 1.3 inner type that is none of the four. */
		error = sslc_conn_fail(conn, EPROTO, SSL_R_UNEXPECTED_MESSAGE, SSLC_ALERT_UNEXPECTED_MESSAGE);
		break;
	}
	if (error != 0)
		return error;

	/* A record whose bytes are all used leaves the input buffer free for the next one. */
	if (conn->plain_size == 0U) {
		cryc_wipe(conn->in, conn->in_used);
		conn->in_used = 0;
	}

	/* Succeeded: the record is taken. */
	return 0;
}

/* Takes the plaintext of a handshake record into the reassembly buffer. */
static int
sslc_conn_on_handshake(
	struct sslc_conn *conn,
	const struct sslc_record *record)
{
	int error;

	/* TLS 1.3 forbids empty handshake fragments (RFC 8446 5.1); an empty one is useless in TLS 1.2 too. */
	if (record->size == 0U)
		return sslc_conn_fail(conn, EPROTO, SSL_R_BAD_LENGTH, SSLC_ALERT_DECODE_ERROR);

	/* Handshake bytes before the end of the handshake are progress; after it they are not. */
	if ((conn->flags & SSLC_CONN_CONNECTED) != 0U) {
		error = sslc_conn_count_empty(conn);
		if (error != 0)
			return error;
	} else {
		conn->empty = 0;
	}

	/* The buffer is made when the first bytes come. */
	if (conn->message == NULL) {
		conn->message = malloc(SSLC_CONN_MESSAGE_ROOM);
		if (conn->message == NULL)
			return sslc_conn_fail(conn, ENOMEM, 0U, SSLC_ALERT_INTERNAL_ERROR);
		conn->message_used = 0;
	}

	/* The room is enough by construction; this refuses a state that is not. */
	if (record->size > SSLC_CONN_MESSAGE_ROOM - conn->message_used)
		return sslc_conn_fail(conn, EMSGSIZE, SSL_R_EXCESSIVE_MESSAGE_SIZE, SSLC_ALERT_DECODE_ERROR);

	/* The bytes join what is held; messages are cut out of it when they are whole. */
	memcpy(conn->message + conn->message_used, record->data, record->size);
	conn->message_used += record->size;

	/* Succeeded: the bytes are held. */
	return 0;
}

/* Takes the plaintext of an alert record. */
static int
sslc_conn_on_alert(
	struct sslc_conn *conn,
	const struct sslc_record *record)
{
	unsigned level;
	unsigned description;
	int version13;

	/* An alert is exactly a level and a description. */
	if (record->size != SSLC_CONN_ALERT_SIZE)
		return sslc_conn_fail(conn, EPROTO, SSL_R_BAD_LENGTH, SSLC_ALERT_DECODE_ERROR);
	level = record->data[0];
	description = record->data[1];

	/* TLS 1.3 alerts follow RFC 8446 6; before the version is known, and in TLS 1.2, RFC 5246 7.2. */
	version13 = conn->hs.version == SSLC_TLS13;

	/* close_notify ends the reading side (a TLS 1.2 close_notify is a warning). */
	if (description == SSLC_ALERT_CLOSE_NOTIFY && (version13 || level == SSLC_ALERT_WARNING)) {
		conn->flags |= SSLC_CONN_CLOSE_RECEIVED;

		/* During the handshake the closing is also the reason of the failure the caller sees. */
		if ((conn->flags & SSLC_CONN_CONNECTED) == 0U) {
			conn->alert_received = description;
			conn->reason = SSL_AD_REASON_OFFSET + description;
		}
		return 0;
	}

	/* TLS 1.3: user_canceled is a closure alert that is ignored (6.1); every other alert is fatal (6.2) whatever its level. */
	if (version13) {
		if (description == SSLC_ALERT_USER_CANCELED)
			return sslc_conn_count_empty(conn);
	} else if (level == SSLC_ALERT_WARNING) {
		/* TLS 1.2: a warning other than close_notify is ignored. */
		return sslc_conn_count_empty(conn);
	} else if (level != SSLC_ALERT_FATAL) {
		/* TLS 1.2 knows two levels. */
		return sslc_conn_fail(conn, EPROTO, SSL_R_UNEXPECTED_MESSAGE, SSLC_ALERT_ILLEGAL_PARAMETER);
	}

	/* A fatal alert ends the connection; the peer is not answered. */
	conn->alert_received = description;
	return sslc_conn_fail(conn, EPROTO, SSL_AD_REASON_OFFSET + description, 0U);
}

/* Takes a change_cipher_spec record. */
static int
sslc_conn_on_change_cipher_spec(
	struct sslc_conn *conn,
	const struct sslc_record *record)
{
	struct sslc_hs_actions actions;
	int error;

	/* The content is the one byte 1 (RFC 8446 5; RFC 5246 7.1). */
	if (record->size != 1U || record->data[0] != SSLC_CONN_CCS_VALUE)
		return sslc_conn_fail(conn, EPROTO, SSL_R_UNEXPECTED_MESSAGE, SSLC_ALERT_UNEXPECTED_MESSAGE);

	/* No record of another type may come inside a handshake message split over records (RFC 8446 5.1). */
	if (conn->message_used != 0U)
		return sslc_conn_fail(conn, EPROTO, SSL_R_UNEXPECTED_MESSAGE, SSLC_ALERT_UNEXPECTED_MESSAGE);

	/* It counts as a record without progress, in TLS 1.3 where it is dropped and in TLS 1.2 until the Finished follows. */
	error = sslc_conn_count_empty(conn);
	if (error != 0)
		return error;

	/* The handshake decides what it means in its state. */
	error = sslc_hs_change_cipher_spec(&conn->hs, &actions);
	if (error != 0)
		return sslc_conn_fail_handshake(conn, error);

	/* The keys it gives. */
	error = sslc_conn_apply(conn, &actions);
	if (error != 0)
		return error;

	/* Succeeded: the record is taken. */
	return 0;
}

/* Takes the plaintext of an application data record. */
static int
sslc_conn_on_data(
	struct sslc_conn *conn,
	const struct sslc_record *record)
{
	/* No application data before the handshake is complete. */
	if ((conn->flags & SSLC_CONN_CONNECTED) == 0U)
		return sslc_conn_fail(conn, EPROTO, SSL_R_UNEXPECTED_MESSAGE, SSLC_ALERT_UNEXPECTED_MESSAGE);

	/* An empty record is allowed (a traffic analysis countermeasure, RFC 8446 5.1) and gives no progress. */
	if (record->size == 0U)
		return sslc_conn_count_empty(conn);

	/* The bytes stay in the input buffer until the caller has read them. */
	conn->empty = 0;
	conn->plain = record->data;
	conn->plain_size = record->size;

	/* Succeeded: application bytes are waiting for the caller. */
	return 0;
}
