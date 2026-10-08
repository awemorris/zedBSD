/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Mail's SMTP client (WS169 p003, RFC 5321 with submission's RFC 6409;
 * mail.h): one message sent on its own connection -- TLS from the start
 * (465) or after STARTTLS (587), EHLO, AUTH PLAIN (RFC 4616), MAIL FROM,
 * RCPT TO for each receiver, DATA with the lines that start with a dot
 * doubled, and QUIT.
 *
 * ws177-p015: a plain port whose server does not offer STARTTLS is
 * ML_ERROR_NO_TLS (the password is never sent in the clear).
 */

#include "mail.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The longest command line, with its NUL. */
#define SMTP_COMMAND_MAX	1024U

/* The name the client gives in EHLO (no host name is told). */
#define SMTP_HELLO_NAME		"[127.0.0.1]"

/* What a session holds: the connection, the server's last reply's words, and whether it offers STARTTLS. */
struct smtp_session {
	struct ml_conn conn;
	char reply[ML_TEXT_MAX];
	int starttls;
};

static int smtp_command(struct smtp_session *session, int expected, const char *format, ...);
static int smtp_reply(struct smtp_session *session, int expected);
static int smtp_hello(struct smtp_session *session);
static int smtp_login(struct smtp_session *session, const struct ml_account_config *account);
static int smtp_data(struct smtp_session *session, const char *raw, size_t length);

/*
 * Sends a written message (compose.c) from an account to receivers.
 * Returns 0, a network or TLS error, EACCES when the server refuses the
 * login, or EPROTO when it refuses the message; error has the server's
 * words.
 */
int
ml_smtp_send(
	const struct ml_account_config *account,
	const char *const *receivers,
	size_t count,
	const char *raw,
	size_t length,
	char *error_text,
	size_t size)
{
	struct smtp_session session;
	size_t index;
	int error;

	/* The connection. */
	memset(&session, 0, sizeof(session));
	error_text[0] = '\0';
	error = ml_conn_open(&session.conn, &account->smtp);
	if (error != 0) {
		/* OpenSSL's words for a failure of TLS (another failure's are the errno value's). */
		if (error == EPROTO || error == ML_ERROR_UNTRUSTED || error == EPROTONOSUPPORT)
			(void)snprintf(error_text, size, "%s", ml_tls_error());
		return error;
	}

	/* The greeting and EHLO. */
	error = smtp_reply(&session, 220);
	if (error == 0)
		error = smtp_hello(&session);

	/* A plain port whose server does not offer STARTTLS goes no further. */
	if (error == 0 && !account->smtp.secure && !session.starttls) {
		(void)snprintf(session.reply, sizeof(session.reply), "the server does not offer TLS (STARTTLS)");
		error = ML_ERROR_NO_TLS;
	}

	/* A plain port: STARTTLS, then EHLO again over TLS. */
	if (error == 0 && !account->smtp.secure) {
		error = smtp_command(&session, 220, "STARTTLS");
		if (error == 0)
			error = ml_conn_start_tls(&session.conn, &account->smtp);
		if (error == 0)
			error = smtp_hello(&session);
	}

	/* The login. */
	if (error == 0)
		error = smtp_login(&session, account);

	/* The envelope: the sender and each receiver. */
	if (error == 0)
		error = smtp_command(&session, 250, "MAIL FROM:<%s>", account->address);
	for (index = 0; index < count && error == 0; index++)
		error = smtp_command(&session, 250, "RCPT TO:<%s>", receivers[index]);

	/* The message. */
	if (error == 0)
		error = smtp_data(&session, raw, length);

	/* Goodbye (its reply does not matter). */
	if (error == 0)
		(void)smtp_command(&session, 221, "QUIT");
	(void)snprintf(error_text, size, "%s", session.reply);
	if (error != 0 && session.reply[0] == '\0' && (error == EPROTO || error == ML_ERROR_UNTRUSTED || error == EPROTONOSUPPORT))
		(void)snprintf(error_text, size, "%s", ml_tls_error());
	ml_conn_close(&session.conn);
	if (error != 0)
		return error;

	/* Succeeded: the server took the message. */
	error_text[0] = '\0';
	return 0;
}

/* Sends a command and reads its reply, which must have the expected code. */
static int
smtp_command(
	struct smtp_session *session,
	int expected,
	const char *format,
	...)
{
	char command[SMTP_COMMAND_MAX];
	va_list arguments;
	int written;
	int error;

	/* The command and its line end. */
	va_start(arguments, format);
	written = vsnprintf(command, sizeof(command) - 2U, format, arguments);
	va_end(arguments);
	if (written < 0 || (size_t)written >= sizeof(command) - 2U)
		return E2BIG;
	memcpy(command + written, "\r\n", 2U);

	/* Sent. */
	error = ml_conn_write(&session->conn, command, (size_t)written + 2U);
	if (error != 0)
		return error;

	/* Its reply. */
	error = smtp_reply(session, expected);
	if (error != 0)
		return error;

	/* Succeeded: the server took it. */
	return 0;
}

/*
 * Reads a reply (its lines "250-..." up to "250 ..."): 0 when its code is
 * the expected one, EACCES for a refused login (535), else EPROTO.  The
 * last line's words are kept; a line STARTTLS is noted.
 */
static int
smtp_reply(
	struct smtp_session *session,
	int expected)
{
	char line[ML_LINE_MAX];
	size_t length;
	int code;
	int offers;
	int error;

	/* Each line until the one without a hyphen after its code. */
	for (;;) {
		error = ml_conn_line(&session->conn, line, sizeof(line));
		if (error != 0)
			return error;

		/* An EHLO line offering STARTTLS. */
		length = strlen(line);
		offers = 1;
		if (length >= 12U)
			offers = strncmp(line + 4, "STARTTLS", 8U);
		if (offers == 0)
			session->starttls = 1;

		/* The last line. */
		if (line[0] != '\0' && line[1] != '\0' && line[2] != '\0' && line[3] != '-')
			break;
	}

	/* Its code. */
	(void)snprintf(session->reply, sizeof(session->reply), "%.200s", line);
	code = atoi(line);
	if (code == expected) {
		session->reply[0] = '\0';
		return 0;
	}

	/* A refused login. */
	if (code == 535)
		return EACCES;

	/* Anything else. */
	return EPROTO;
}

/* Says EHLO. */
static int
smtp_hello(
	struct smtp_session *session)
{
	int error;

	/* EHLO with the client's address as its name. */
	session->starttls = 0;
	error = smtp_command(session, 250, "EHLO %s", SMTP_HELLO_NAME);
	if (error != 0)
		return error;

	/* Succeeded: the server greeted back. */
	return 0;
}

/* Logs in with AUTH PLAIN: base64 of "\0user\0password". */
static int
smtp_login(
	struct smtp_session *session,
	const struct ml_account_config *account)
{
	unsigned char plain[ML_TEXT_MAX * 2U + 2U];
	char encoded[ML_TEXT_MAX * 4U];
	size_t user_length;
	size_t password_length;
	size_t encoded_length;
	int error;

	/* The authorization (empty), the user and the password, each after a NUL. */
	user_length = strlen(account->user);
	password_length = strlen(account->password);
	plain[0] = '\0';
	memcpy(plain + 1, account->user, user_length);
	plain[1U + user_length] = '\0';
	memcpy(plain + 2U + user_length, account->password, password_length);

	/* In base64. */
	encoded_length = ml_base64_encode(plain, 2U + user_length + password_length, encoded, sizeof(encoded));
	memset(plain, 0, sizeof(plain));
	if (encoded_length == 0U)
		return E2BIG;

	/* Sent; 235 is a login taken. */
	error = smtp_command(session, 235, "AUTH PLAIN %s", encoded);
	memset(encoded, 0, sizeof(encoded));
	if (error != 0)
		return error;

	/* Succeeded: logged in. */
	return 0;
}

/* Sends DATA, the message with its leading dots doubled and its line ends CR LF, and the closing dot. */
static int
smtp_data(
	struct smtp_session *session,
	const char *raw,
	size_t length)
{
	size_t start;
	size_t end;
	size_t next;
	int error;

	/* DATA: 354 asks for the message. */
	error = smtp_command(session, 354, "DATA");
	if (error != 0)
		return error;

	/* Each line. */
	start = 0;
	while (start < length) {
		/* The line's end (LF), and where the next starts. */
		end = start;
		while (end < length && raw[end] != '\n')
			end++;
		next = end + 1U;
		if (end > start && raw[end - 1U] == '\r')
			end--;

		/* A leading dot doubled. */
		if (raw[start] == '.') {
			error = ml_conn_write(&session->conn, ".", 1U);
			if (error != 0)
				return error;
		}

		/* The line and CR LF. */
		error = ml_conn_write(&session->conn, raw + start, end - start);
		if (error == 0)
			error = ml_conn_write(&session->conn, "\r\n", 2U);
		if (error != 0)
			return error;
		start = next;
	}

	/* The end of the message; 250 is the message taken. */
	error = smtp_command(session, 250, ".");
	if (error != 0)
		return error;

	/* Succeeded: the message is sent. */
	return 0;
}
