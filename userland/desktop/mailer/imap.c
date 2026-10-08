/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Mail's IMAP4rev1 client (WS169 p003, RFC 3501; mail.h): a session on
 * one connection that logs in (after STARTTLS on a plain port), finds the
 * folders by their special use (RFC 6154) or their usual names, selects
 * one, fetches the latest messages or those after a UID, sets and clears
 * flags, moves a message (COPY, \Deleted, EXPUNGE), appends a sent one,
 * and waits for new mail with IDLE (RFC 2177).
 *
 * ws177-p015: what the server can do is asked after the login
 * (CAPABILITY: MOVE, UIDPLUS, Gmail's extensions); a message is deleted
 * for good (\Deleted, then UID EXPUNGE, or EXPUNGE without UIDPLUS); a
 * plain port whose server refuses STARTTLS is ML_ERROR_NO_TLS; and the
 * words of a server's goodbye (BYE) are kept as the failure's.
 *
 * A command is sent with its tag ("A0001") and its answer read up to the
 * tagged line; the untagged lines before it go to the command's reader.
 * A line that ends in a literal ("{123}") is followed by that many bytes
 * and the rest of the line.
 */

#include "mail.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The longest command line, with its NUL. */
#define IMAP_COMMAND_MAX	1024U

/* The usual names of the folders, when the server does not mark them (any case). */
static const char *const imap_sent_names[] = { "Sent", "Sent Items", "Sent Messages", "Sent Mail" };
static const char *const imap_drafts_names[] = { "Drafts", "Draft" };
static const char *const imap_archive_names[] = { "Archive", "Archives", "All Mail" };
static const char *const imap_trash_names[] = { "Trash", "Deleted Items", "Deleted Messages", "Bin" };

/*
 * What reads the untagged lines of a command: given the session, the line
 * (a literal at its end not read yet), and its own data.  It reads the
 * literal itself when it wants it and returns 1 then; 0 leaves it to be
 * skipped.  An error ends the command.
 */
typedef int (*imap_reader_fn)(struct ml_imap *imap, const char *line, void *data, int *took_literal);

/* What a FETCH's reader carries: the caller's function and data, and the lowest UID wanted. */
struct imap_fetch {
	ml_imap_fetched_fn fetched;
	void *data;
	uint32_t first_uid;
};

/* What LIST's reader fills: the folders' names by special use, and by usual names. */
struct imap_list {
	char marked[ML_FOLDERS][ML_MAILBOX_MAX];
	char named[ML_FOLDERS][ML_MAILBOX_MAX];
};

static int imap_send(struct ml_imap *imap, unsigned *tag, const char *format, ...);
static int imap_wait(struct ml_imap *imap, unsigned tag, imap_reader_fn reader, void *data);
static int imap_run(struct ml_imap *imap, imap_reader_fn reader, void *data, const char *format, ...);
static int imap_literal(const char *line, size_t *size);
static int imap_skip_literal(struct ml_imap *imap, size_t size);
static int imap_read_exists(struct ml_imap *imap, const char *line, void *data, int *took_literal);
static int imap_read_capability(struct ml_imap *imap, const char *line, void *data, int *took_literal);
static int imap_read_list(struct ml_imap *imap, const char *line, void *data, int *took_literal);
static int imap_read_fetch(struct ml_imap *imap, const char *line, void *data, int *took_literal);
static void imap_fetch_items(const char *text, uint32_t *uid, unsigned *flags, size_t *size, int *seen_flags);
static void imap_list_name(const char *line, char *name, size_t size);
static int imap_quote(const char *text, char *quoted, size_t size);
static int imap_same(const char *a, const char *b);
static int imap_has(const char *text, const char *word);

/*
 * Connects and logs in to an account's IMAP server.  Returns 0, a network
 * or TLS error, EACCES when the server refuses the login (imap->error
 * has its words), or EPROTO.
 */
int
ml_imap_open(
	struct ml_imap *imap,
	const struct ml_account_config *account)
{
	char greeting[ML_LINE_MAX];
	char user[ML_TEXT_MAX * 2U];
	char password[ML_TEXT_MAX * 2U];
	int same;
	int error;

	/* Nothing yet. */
	memset(imap, 0, sizeof(imap[0]));
	imap->tag = 1;

	/* The connection. */
	error = ml_conn_open(&imap->conn, &account->imap);
	if (error != 0) {
		/* OpenSSL's words for a failure of TLS (another failure's are the errno value's). */
		if (error == EPROTO || error == ML_ERROR_UNTRUSTED || error == EPROTONOSUPPORT)
			(void)snprintf(imap->error, sizeof(imap->error), "%s", ml_tls_error());
		return error;
	}

	/* The server's greeting. */
	error = ml_conn_line(&imap->conn, greeting, sizeof(greeting));
	if (error != 0) {
		ml_imap_close(imap);
		return error;
	}

	/* A greeting that is not OK (BYE, or PREAUTH, which this client does not take). */
	same = strncmp(greeting, "* OK", 4U);
	if (same != 0) {
		(void)snprintf(imap->error, sizeof(imap->error), "the server turned the connection away: %.200s", greeting);
		ml_imap_close(imap);
		return EPROTO;
	}

	/* A plain port: TLS first, with STARTTLS; a server that refuses it never gets the password. */
	if (!account->imap.secure) {
		error = imap_run(imap, NULL, NULL, "STARTTLS");
		if (error == EACCES || error == EPROTO) {
			(void)snprintf(imap->error, sizeof(imap->error), "the server does not offer TLS (STARTTLS)");
			ml_imap_close(imap);
			return ML_ERROR_NO_TLS;
		}

		/* The handshake over the plain connection. */
		if (error == 0)
			error = ml_conn_start_tls(&imap->conn, &account->imap);
		if (error != 0) {
			(void)snprintf(imap->error, sizeof(imap->error), "%s", ml_tls_error());
			ml_imap_close(imap);
			return error;
		}
	}

	/* The user and the password as quoted strings. */
	error = imap_quote(account->user, user, sizeof(user));
	if (error == 0)
		error = imap_quote(account->password, password, sizeof(password));
	if (error != 0) {
		ml_imap_close(imap);
		return error;
	}

	/* The login. */
	error = imap_run(imap, NULL, NULL, "LOGIN %s %s", user, password);
	memset(password, 0, sizeof(password));
	if (error != 0) {
		ml_imap_close(imap);
		return error;
	}

	/* What the server can do (a server that does not say can do nothing more). */
	imap->capabilities = 0;
	error = imap_run(imap, imap_read_capability, NULL, "CAPABILITY");
	if (error != 0)
		imap->capabilities = 0;

	/* Succeeded: the session is logged in. */
	return 0;
}

/*
 * Finds the names of the folders: INBOX, and the sent, drafts, archive
 * and trash ones by their special use or their usual names (an empty name
 * for one the server does not have).
 */
int
ml_imap_folders(
	struct ml_imap *imap,
	char names[ML_FOLDERS][ML_MAILBOX_MAX])
{
	struct imap_list list;
	int folder;
	int error;

	/* Every folder. */
	memset(&list, 0, sizeof(list));
	error = imap_run(imap, imap_read_list, &list, "LIST \"\" \"*\"");
	if (error != 0)
		return error;

	/* A marked one first, else one of the usual names. */
	for (folder = 0; folder < ML_FOLDERS; folder++) {
		(void)snprintf(names[folder], ML_MAILBOX_MAX, "%s", list.marked[folder]);
		if (names[folder][0] == '\0')
			(void)snprintf(names[folder], ML_MAILBOX_MAX, "%s", list.named[folder]);
	}

	/* The inbox is always INBOX. */
	(void)snprintf(names[ML_INBOX], ML_MAILBOX_MAX, "INBOX");

	/* Succeeded: the folders are named. */
	return 0;
}

/*
 * Selects a folder; *exists is how many messages it has.
 */
int
ml_imap_select(
	struct ml_imap *imap,
	const char *mailbox,
	uint32_t *exists)
{
	char quoted[ML_MAILBOX_MAX * 2U];
	int error;

	/* The name, quoted. */
	error = imap_quote(mailbox, quoted, sizeof(quoted));
	if (error != 0)
		return error;

	/* Selected; its EXISTS counts its messages. */
	imap->exists = 0;
	error = imap_run(imap, imap_read_exists, NULL, "SELECT %s", quoted);
	if (error != 0)
		return error;

	/* Succeeded: the folder is selected. */
	*exists = imap->exists;
	return 0;
}

/*
 * Fetches messages of the selected folder: with first_uid 0 the latest
 * most of them, else those whose UID is first_uid or more.  Each message
 * goes to fetched (up to ML_FETCH_BYTES of it).
 */
int
ml_imap_fetch(
	struct ml_imap *imap,
	uint32_t first_uid,
	unsigned most,
	ml_imap_fetched_fn fetched,
	void *data)
{
	struct imap_fetch fetch;
	uint32_t first;
	int error;

	/* The reader's state. */
	fetch.fetched = fetched;
	fetch.data = data;
	fetch.first_uid = first_uid;

	/* Those after a UID ("*" is always in the range, so a lower UID is dropped by the reader). */
	if (first_uid != 0U) {
		error = imap_run(imap, imap_read_fetch, &fetch, "UID FETCH %lu:* (UID FLAGS RFC822.SIZE BODY.PEEK[]<0.%u>)", (unsigned long)first_uid, ML_FETCH_BYTES);
		if (error != 0)
			return error;
		return 0;
	}

	/* An empty folder has nothing to fetch. */
	if (imap->exists == 0U)
		return 0;

	/* The latest ones by their numbers. */
	first = 1;
	if (imap->exists > most)
		first = imap->exists - most + 1U;
	error = imap_run(imap, imap_read_fetch, &fetch, "FETCH %lu:%lu (UID FLAGS RFC822.SIZE BODY.PEEK[]<0.%u>)", (unsigned long)first, (unsigned long)imap->exists, ML_FETCH_BYTES);
	if (error != 0)
		return error;

	/* Succeeded: the messages went to the caller. */
	return 0;
}

/*
 * Adds or takes away a flag ("\\Seen", "\\Deleted") of a message of the
 * selected folder.
 */
int
ml_imap_flag(
	struct ml_imap *imap,
	uint32_t uid,
	const char *flag,
	int add)
{
	char sign;
	int error;

	/* Added or taken away, without the server telling the new flags. */
	sign = '-';
	if (add)
		sign = '+';
	error = imap_run(imap, NULL, NULL, "UID STORE %lu %cFLAGS.SILENT (%s)", (unsigned long)uid, sign, flag);
	if (error != 0)
		return error;

	/* Succeeded: the flag is set. */
	return 0;
}

/*
 * Moves a message of the selected folder to another: copied, marked
 * deleted and expunged.
 */
int
ml_imap_move(
	struct ml_imap *imap,
	uint32_t uid,
	const char *mailbox)
{
	char quoted[ML_MAILBOX_MAX * 2U];
	int error;

	/* The destination, quoted. */
	error = imap_quote(mailbox, quoted, sizeof(quoted));
	if (error != 0)
		return error;

	/* The copy. */
	error = imap_run(imap, NULL, NULL, "UID COPY %lu %s", (unsigned long)uid, quoted);
	if (error != 0)
		return error;

	/* The original marked deleted. */
	error = ml_imap_flag(imap, uid, "\\Deleted", 1);
	if (error != 0)
		return error;

	/* And gone. */
	error = imap_run(imap, imap_read_exists, NULL, "EXPUNGE");
	if (error != 0)
		return error;

	/* Succeeded: the message is in the other folder. */
	return 0;
}

/*
 * Deletes a message of the selected folder for good: marked deleted and
 * expunged, by its UID alone when the server has UIDPLUS (without it,
 * EXPUNGE also takes the folder's other messages marked deleted).
 */
int
ml_imap_delete(
	struct ml_imap *imap,
	uint32_t uid)
{
	int error;

	/* Marked deleted. */
	error = ml_imap_flag(imap, uid, "\\Deleted", 1);
	if (error != 0)
		return error;

	/* Expunged: this message alone, or every one marked. */
	if ((imap->capabilities & ML_IMAP_CAN_UIDPLUS) != 0U) {
		error = imap_run(imap, imap_read_exists, NULL, "UID EXPUNGE %lu", (unsigned long)uid);
	} else {
		error = imap_run(imap, imap_read_exists, NULL, "EXPUNGE");
	}

	/* Reports a refused expunge. */
	if (error != 0)
		return error;

	/* Succeeded: the message is gone. */
	return 0;
}

/*
 * Appends a message to a folder, read (a sent one to Sent).
 */
int
ml_imap_append(
	struct ml_imap *imap,
	const char *mailbox,
	const char *raw,
	size_t length)
{
	char quoted[ML_MAILBOX_MAX * 2U];
	char line[ML_LINE_MAX];
	unsigned tag;
	int error;

	/* The folder, quoted. */
	error = imap_quote(mailbox, quoted, sizeof(quoted));
	if (error != 0)
		return error;

	/* The command with the literal's size. */
	error = imap_send(imap, &tag, "APPEND %s (\\Seen) {%lu}", quoted, (unsigned long)length);
	if (error != 0)
		return error;

	/* The server's go-ahead ("+ ..."). */
	error = ml_conn_line(&imap->conn, line, sizeof(line));
	if (error != 0)
		return error;
	if (line[0] != '+') {
		(void)snprintf(imap->error, sizeof(imap->error), "%.200s", line);
		return EPROTO;
	}

	/* The message and the command's end. */
	error = ml_conn_write(&imap->conn, raw, length);
	if (error == 0)
		error = ml_conn_write(&imap->conn, "\r\n", 2U);
	if (error != 0)
		return error;

	/* The answer. */
	error = imap_wait(imap, tag, NULL, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the message is appended. */
	return 0;
}

/*
 * Starts waiting for new mail in the selected folder (IDLE); the
 * connection's socket is then read by ml_imap_idle_take when it is ready.
 */
int
ml_imap_idle_start(
	struct ml_imap *imap)
{
	char line[ML_LINE_MAX];
	unsigned tag;
	int error;

	/* The command. */
	error = imap_send(imap, &tag, "IDLE");
	if (error != 0)
		return error;

	/* The server's go-ahead. */
	error = ml_conn_line(&imap->conn, line, sizeof(line));
	if (error != 0)
		return error;
	if (line[0] != '+') {
		(void)snprintf(imap->error, sizeof(imap->error), "%.200s", line);
		return EPROTO;
	}

	/* Succeeded: the session idles. */
	imap->idle_tag = tag;
	return 0;
}

/*
 * Reads what the server said while idling (call when the socket is
 * readable or ml_conn_ready): *arrived is 1 when the folder grew.
 */
int
ml_imap_idle_take(
	struct ml_imap *imap,
	int *arrived)
{
	char line[ML_LINE_MAX];
	uint32_t count;
	int exists;
	int ready;
	int error;

	/* Each line the server sent. */
	*arrived = 0;
	do {
		error = ml_conn_line(&imap->conn, line, sizeof(line));
		if (error != 0)
			return error;

		/* "* N EXISTS": the folder has N messages. */
		exists = 0;
		if (line[0] == '*')
			exists = imap_has(line, " EXISTS");
		if (exists) {
			count = (uint32_t)strtoul(line + 2, NULL, 10);
			if (count > imap->exists)
				*arrived = 1;
			imap->exists = count;
		}

		/* More already read. */
		ready = ml_conn_ready(&imap->conn);
	} while (ready);

	/* Succeeded: the lines are read. */
	return 0;
}

/*
 * Ends the waiting (DONE) and reads IDLE's answer.
 */
int
ml_imap_idle_stop(
	struct ml_imap *imap)
{
	unsigned tag;
	int error;

	/* Not idling. */
	if (imap->idle_tag == 0U)
		return 0;

	/* DONE, then the answer (an EXISTS on the way is counted). */
	tag = imap->idle_tag;
	imap->idle_tag = 0;
	error = ml_conn_write(&imap->conn, "DONE\r\n", 6U);
	if (error != 0)
		return error;
	error = imap_wait(imap, tag, imap_read_exists, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the session takes commands again. */
	return 0;
}

/*
 * Logs out and closes the connection.
 */
void
ml_imap_close(
	struct ml_imap *imap)
{
	unsigned tag;

	/* A logged-in session says goodbye (its answer is not waited for). */
	if (imap->conn.fd >= 0 && imap->idle_tag == 0U)
		(void)imap_send(imap, &tag, "LOGOUT");

	/* The connection. */
	ml_conn_close(&imap->conn);
}

/* Sends a command with the next tag; *tag is that tag's number. */
static int
imap_send(
	struct ml_imap *imap,
	unsigned *tag,
	const char *format,
	...)
{
	char command[IMAP_COMMAND_MAX];
	va_list arguments;
	int prefix;
	int written;
	int error;

	/* The tag. */
	*tag = imap->tag;
	imap->tag++;
	prefix = snprintf(command, sizeof(command), "A%04u ", *tag);

	/* The command after it. */
	va_start(arguments, format);
	written = vsnprintf(command + prefix, sizeof(command) - (size_t)prefix - 2U, format, arguments);
	va_end(arguments);
	if (written < 0 || (size_t)(prefix + written) >= sizeof(command) - 2U)
		return E2BIG;

	/* Its line end, and sent. */
	memcpy(command + prefix + written, "\r\n", 2U);
	error = ml_conn_write(&imap->conn, command, (size_t)(prefix + written) + 2U);
	if (error != 0)
		return error;

	/* Succeeded: the command is sent. */
	return 0;
}

/*
 * Reads a command's answer: the untagged lines to the reader, up to the
 * tagged line, whose OK is 0, NO EACCES and BAD EPROTO (imap->error has
 * the server's words).
 */
static int
imap_wait(
	struct ml_imap *imap,
	unsigned tag,
	imap_reader_fn reader,
	void *data)
{
	char line[ML_LINE_MAX];
	char tag_text[16];
	size_t tag_length;
	size_t literal;
	int took;
	int has_literal;
	int same;
	int error;

	/* The tag's text. */
	(void)snprintf(tag_text, sizeof(tag_text), "A%04u ", tag);
	tag_length = strlen(tag_text);

	/* Each line up to the tagged one. */
	for (;;) {
		error = ml_conn_line(&imap->conn, line, sizeof(line));
		if (error != 0)
			return error;

		/* The tagged line: the outcome. */
		same = strncmp(line, tag_text, tag_length);
		if (same == 0)
			break;

		/* A goodbye's words, kept for the failure that follows (the server closes after it). */
		same = strncmp(line, "* BYE", 5U);
		if (same == 0)
			(void)snprintf(imap->error, sizeof(imap->error), "the server ended the session: %.200s", line + 5);

		/* An untagged line to the reader. */
		took = 0;
		if (reader != NULL && line[0] == '*') {
			error = reader(imap, line, data, &took);
			if (error != 0)
				return error;
		}

		/* A literal the reader did not take is skipped, with the rest of its line. */
		has_literal = imap_literal(line, &literal);
		while (has_literal && !took) {
			error = imap_skip_literal(imap, literal);
			if (error != 0)
				return error;
			error = ml_conn_line(&imap->conn, line, sizeof(line));
			if (error != 0)
				return error;
			has_literal = imap_literal(line, &literal);
		}
	}

	/* OK. */
	same = strncmp(line + tag_length, "OK", 2U);
	if (same == 0)
		return 0;

	/* NO: refused, with the server's words. */
	(void)snprintf(imap->error, sizeof(imap->error), "%.200s", line + tag_length);
	same = strncmp(line + tag_length, "NO", 2U);
	if (same == 0)
		return EACCES;

	/* BAD: the command was not understood. */
	return EPROTO;
}

/* Sends a command and reads its answer. */
static int
imap_run(
	struct ml_imap *imap,
	imap_reader_fn reader,
	void *data,
	const char *format,
	...)
{
	char command[IMAP_COMMAND_MAX];
	va_list arguments;
	unsigned tag;
	int written;
	int error;

	/* The command's words. */
	va_start(arguments, format);
	written = vsnprintf(command, sizeof(command), format, arguments);
	va_end(arguments);
	if (written < 0 || (size_t)written >= sizeof(command))
		return E2BIG;

	/* Sent. */
	error = imap_send(imap, &tag, "%s", command);
	if (error != 0)
		return error;

	/* Its answer. */
	error = imap_wait(imap, tag, reader, data);
	if (error != 0)
		return error;

	/* Succeeded: the command is done. */
	return 0;
}

/* Tells whether a line ends with a literal "{N}" and gives N. */
static int
imap_literal(
	const char *line,
	size_t *size)
{
	const char *open;
	size_t length;
	char *end;

	/* A closing brace at the end, and its opening one. */
	length = strlen(line);
	if (length < 3U || line[length - 1U] != '}')
		return 0;
	open = strrchr(line, '{');
	if (open == NULL)
		return 0;

	/* The number between them. */
	*size = (size_t)strtoul(open + 1, &end, 10);
	if (end != line + length - 1U)
		return 0;

	/* A literal follows. */
	return 1;
}

/* Reads and drops a literal's bytes. */
static int
imap_skip_literal(
	struct ml_imap *imap,
	size_t size)
{
	char chunk[1024];
	size_t part;
	int error;

	/* Chunk by chunk. */
	while (size > 0U) {
		part = size;
		if (part > sizeof(chunk))
			part = sizeof(chunk);
		error = ml_conn_bytes(&imap->conn, chunk, part);
		if (error != 0)
			return error;
		size -= part;
	}

	/* Succeeded: the literal is read. */
	return 0;
}

/* Reads "* N EXISTS" into imap->exists, and counts a "* N EXPUNGE" off it. */
static int
imap_read_exists(
	struct ml_imap *imap,
	const char *line,
	void *data,
	int *took_literal)
{
	int has;

	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(took_literal);

	/* The folder's count. */
	has = imap_has(line, " EXISTS");
	if (has) {
		imap->exists = (uint32_t)strtoul(line + 2, NULL, 10);
		return 0;
	}

	/* A message gone. */
	has = imap_has(line, " EXPUNGE");
	if (has && imap->exists > 0U)
		imap->exists--;
	return 0;
}

/* Reads "* CAPABILITY ..." into what the server can do (ML_IMAP_*). */
static int
imap_read_capability(
	struct ml_imap *imap,
	const char *line,
	void *data,
	int *took_literal)
{
	char word[64];
	size_t start;
	size_t length;
	int same;

	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(took_literal);

	/* Only the capability line. */
	same = strncmp(line, "* CAPABILITY ", 13U);
	if (same != 0)
		return 0;

	/* Each word after it. */
	start = 13;
	while (line[start] != '\0') {
		/* The word, up to a space. */
		length = 0;
		while (line[start + length] != '\0' && line[start + length] != ' ')
			length++;
		if (length < sizeof(word)) {
			memcpy(word, line + start, length);
			word[length] = '\0';
		} else {
			word[0] = '\0';
		}

		/* MOVE. */
		same = imap_same(word, "MOVE");
		if (same)
			imap->capabilities |= ML_IMAP_CAN_MOVE;

		/* UIDPLUS. */
		same = imap_same(word, "UIDPLUS");
		if (same)
			imap->capabilities |= ML_IMAP_CAN_UIDPLUS;

		/* Gmail's extensions. */
		same = imap_same(word, "X-GM-EXT-1");
		if (same)
			imap->capabilities |= ML_IMAP_GMAIL;

		/* The next word. */
		start += length;
		while (line[start] == ' ')
			start++;
	}

	/* Succeeded: the line is read. */
	return 0;
}

/* Reads "* LIST (attributes) delimiter name" into the folders' names. */
static int
imap_read_list(
	struct ml_imap *imap,
	const char *line,
	void *data,
	int *took_literal)
{
	static const char *const *const usual[ML_FOLDERS] = { NULL, imap_sent_names, imap_drafts_names, imap_archive_names, imap_trash_names };
	static const size_t usual_counts[ML_FOLDERS] = { 0, 4, 2, 3, 4 };
	static const char *const marks[ML_FOLDERS] = { NULL, "\\Sent", "\\Drafts", "\\Archive", "\\Trash" };
	struct imap_list *list;
	const char *last;
	char name[ML_MAILBOX_MAX];
	size_t index;
	int folder;
	int marked;
	int same;
	int listed;

	UNUSED_PARAMETER(imap);
	UNUSED_PARAMETER(took_literal);

	/* A LIST line. */
	listed = strncmp(line, "* LIST ", 7U);
	if (listed != 0)
		return 0;
	list = data;

	/* The folder's name, after the delimiter. */
	imap_list_name(line, name, sizeof(name));
	if (name[0] == '\0')
		return 0;

	/* A folder that cannot be selected is not one. */
	marked = imap_has(line, "\\Noselect");
	if (marked)
		return 0;

	/* Each kind: marked by its attribute (Gmail's archive is \All), or by a usual last part of its name. */
	last = strrchr(name, '/');
	if (last == NULL)
		last = strrchr(name, '.');
	if (last == NULL)
		last = name;
	else
		last++;
	for (folder = ML_SENT; folder < ML_FOLDERS; folder++) {
		/* The attribute. */
		marked = imap_has(line, marks[folder]);
		if (!marked && folder == ML_ARCHIVE)
			marked = imap_has(line, "\\All");
		if (marked && list->marked[folder][0] == '\0')
			(void)snprintf(list->marked[folder], ML_MAILBOX_MAX, "%s", name);

		/* A usual name. */
		for (index = 0; index < usual_counts[folder]; index++) {
			same = imap_same(last, usual[folder][index]);
			if (same && list->named[folder][0] == '\0')
				(void)snprintf(list->named[folder], ML_MAILBOX_MAX, "%s", name);
		}
	}

	/* Succeeded: the folder is noted. */
	return 0;
}

/* Reads "* N FETCH (... BODY[]<0> {size}" with its literal and the rest of its line. */
static int
imap_read_fetch(
	struct ml_imap *imap,
	const char *line,
	void *data,
	int *took_literal)
{
	struct imap_fetch *fetch;
	char rest[ML_LINE_MAX];
	size_t literal;
	size_t size;
	uint32_t uid;
	unsigned flags;
	char *raw;
	int has_literal;
	int seen_flags;
	int is_fetch;
	int error;

	/* A FETCH line. */
	is_fetch = imap_has(line, " FETCH (");
	if (!is_fetch)
		return 0;
	fetch = data;

	/* Its items before the literal. */
	uid = 0;
	flags = ML_UNREAD;
	size = 0;
	seen_flags = 0;
	imap_fetch_items(line, &uid, &flags, &size, &seen_flags);

	/* The message's bytes. */
	raw = NULL;
	literal = 0;
	has_literal = imap_literal(line, &literal);
	if (has_literal) {
		*took_literal = 1;
		raw = malloc(literal + 1U);
		if (raw == NULL)
			return ENOMEM;
		error = ml_conn_bytes(&imap->conn, raw, literal);
		if (error != 0) {
			free(raw);
			return error;
		}

		/* Ended for the reader. */
		raw[literal] = '\0';

		/* The rest of the line, whose items may come after the literal. */
		error = ml_conn_line(&imap->conn, rest, sizeof(rest));
		if (error != 0) {
			free(raw);
			return error;
		}

		/* Its items. */
		imap_fetch_items(rest, &uid, &flags, &size, &seen_flags);
	}

	/* A message before the UID wanted ("*" in the range) is not new. */
	if (raw != NULL && uid != 0U && uid >= fetch->first_uid)
		fetch->fetched(fetch->data, uid, flags, size, raw, literal);
	free(raw);
	return 0;
}

/* Reads the items of a FETCH that matter: UID, FLAGS and RFC822.SIZE. */
static void
imap_fetch_items(
	const char *text,
	uint32_t *uid,
	unsigned *flags,
	size_t *size,
	int *seen_flags)
{
	const char *at;
	const char *end;
	char list[512];
	size_t length;
	int seen;

	/* The UID. */
	at = strstr(text, "UID ");
	if (at != NULL)
		*uid = (uint32_t)strtoul(at + 4, NULL, 10);

	/* The size. */
	at = strstr(text, "RFC822.SIZE ");
	if (at != NULL)
		*size = (size_t)strtoul(at + 12, NULL, 10);

	/* The flags: read without \Seen. */
	at = strstr(text, "FLAGS (");
	if (at == NULL)
		return;
	end = strchr(at, ')');
	if (end == NULL)
		return;
	length = (size_t)(end - at);
	if (length >= sizeof(list))
		length = sizeof(list) - 1U;
	memcpy(list, at, length);
	list[length] = '\0';
	*seen_flags = 1;
	*flags = ML_UNREAD;
	seen = imap_has(list, "\\Seen");
	if (seen)
		*flags = 0U;
}

/* Gives a LIST line's name: a quoted string or an atom after the delimiter. */
static void
imap_list_name(
	const char *line,
	char *name,
	size_t size)
{
	const char *at;
	size_t length;

	/* After the attributes' parenthesis. */
	name[0] = '\0';
	at = strchr(line, ')');
	if (at == NULL)
		return;
	at++;
	while (*at == ' ')
		at++;

	/* The delimiter: a quoted character or NIL. */
	if (*at == '"') {
		at++;
		if (*at == '\\')
			at++;
		at++;
		if (*at == '"')
			at++;
	} else if (at[0] == 'N' && at[1] == 'I' && at[2] == 'L') {
		at += 3;
	}
	while (*at == ' ')
		at++;

	/* A quoted name, its escapes taken off. */
	if (*at == '"') {
		at++;
		length = 0;
		while (*at != '\0' && *at != '"' && length + 1U < size) {
			if (*at == '\\' && at[1] != '\0')
				at++;
			name[length] = *at;
			length++;
			at++;
		}

		/* Its end. */
		name[length] = '\0';
		return;
	}

	/* An atom. */
	(void)snprintf(name, size, "%s", at);
}

/* Writes a text as an IMAP quoted string (backslash and quote escaped); E2BIG when it does not fit. */
static int
imap_quote(
	const char *text,
	char *quoted,
	size_t size)
{
	size_t length;
	size_t index;

	/* The opening quote. */
	length = 0;
	quoted[length] = '"';
	length++;

	/* Each byte, escaped when it must be; a line end cannot be quoted. */
	for (index = 0; text[index] != '\0'; index++) {
		if (text[index] == '\r' || text[index] == '\n')
			return EINVAL;
		if (length + 4U > size)
			return E2BIG;
		if (text[index] == '"' || text[index] == '\\') {
			quoted[length] = '\\';
			length++;
		}

		/* The byte. */
		quoted[length] = text[index];
		length++;
	}

	/* The closing quote and the NUL. */
	if (length + 2U > size)
		return E2BIG;
	quoted[length] = '"';
	quoted[length + 1U] = '\0';
	return 0;
}

/* Tells whether two names are the same in any ASCII case. */
static int
imap_same(
	const char *a,
	const char *b)
{
	size_t index;
	int left;
	int right;

	/* Each byte in lower case. */
	for (index = 0;; index++) {
		left = (unsigned char)a[index];
		right = (unsigned char)b[index];
		if (left >= 'A' && left <= 'Z')
			left = left - 'A' + 'a';
		if (right >= 'A' && right <= 'Z')
			right = right - 'A' + 'a';
		if (left != right)
			return 0;
		if (left == '\0')
			return 1;
	}
}

/* Tells whether a text holds a word, ASCII letters in any case. */
static int
imap_has(
	const char *text,
	const char *word)
{
	size_t length;
	size_t at;
	size_t index;
	int left;
	int right;

	/* Each place the word could start. */
	length = strlen(word);
	for (at = 0; text[at] != '\0'; at++) {
		for (index = 0; index < length; index++) {
			left = (unsigned char)text[at + index];
			right = (unsigned char)word[index];
			if (left >= 'A' && left <= 'Z')
				left = left - 'A' + 'a';
			if (right >= 'A' && right <= 'Z')
				right = right - 'A' + 'a';
			if (left != right)
				break;
		}

		/* The whole word. */
		if (index == length)
			return 1;
	}

	/* Not held. */
	return 0;
}
