/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws169-p003: the host test of Mail's backend (userland/desktop/mailer/
 * mail.h) against fake-mail-server.py.
 *
 * 1. Reading messages (mime.c): a multipart/alternative with
 *    quoted-printable UTF-8 and an encoded-word subject, a multipart/mixed
 *    with a file, an ISO-8859-1 one, an HTML-only one, and the dates.
 * 2. The sign-in code (code.c).
 * 3. Writing a message (compose.c), read back by mime.c.
 * 4. IMAP (imap.c, conn.c, tls.c with the host's OpenSSL) over TLS from
 *    the start and over STARTTLS: login (and a wrong password), the
 *    folders, SELECT, the latest messages, those after a UID, \Seen,
 *    a move to Archive, APPEND to Sent, and IDLE telling a new message.
 * 5. SMTP (smtp.c) over TLS from the start and over STARTTLS: the
 *    message the server got, its envelope, and a dot at a line's start.
 *
 *     host-mail-backend CA-FILE IMAPS IMAP SMTPS SUBMISSION OUTDIR
 *
 * Prints "PASS name" or "FAIL name ..." for each check; exits with 1 when
 * one failed.
 */

#include "userland/desktop/mailer/mail.h"

#include <errno.h>
#include <keiland/keiland_socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most messages a fetch keeps. */
#define TEST_FETCHED_MAX	8U

/* One fetched message: its UID, flags and what mime.c read of it. */
struct test_fetched {
	uint32_t uid;
	unsigned flags;
	struct ml_parsed parsed;
};

/* What the fetches gave. */
struct test_fetches {
	struct test_fetched items[TEST_FETCHED_MAX];
	unsigned count;
};

/* The checks that failed. */
static int test_failures;

int main(int argc, char **argv);
static void test_check(const char *name, int passed, const char *detail);
static void test_mime(void);
static void test_code(void);
static void test_compose(void);
static void test_imap(const struct ml_account_config *account, const char *label);
static void test_smtp(const struct ml_account_config *account, const char *outdir, int number, const char *label);
static void test_fetched(void *data, uint32_t uid, unsigned flags, size_t size, const char *raw, size_t length);
static void test_release(struct test_fetches *fetches);
static char *test_read_file(const char *path, size_t *length);

/*
 * Runs every part of the test.
 */
int
main(
	int argc,
	char **argv)
{
	struct ml_account_config account;
	char server[64];
	int error;

	/* The arguments. */
	if (argc != 7) {
		fprintf(stderr, "usage: host-mail-backend CA-FILE IMAPS IMAP SMTPS SUBMISSION OUTDIR\n");
		return 2;
	}

	/* The tests' own CA. */
	error = ml_tls_add_ca_file(argv[1]);
	test_check("ca-file", error == 0, "");

	/* The parts without a network. */
	test_mime();
	test_code();
	test_compose();

	/* The account, with IMAP over TLS from the start (the port is not 993, so it is said by the test). */
	memset(&account, 0, sizeof(account));
	(void)snprintf(account.name, sizeof(account.name), "Kei Example");
	(void)snprintf(account.address, sizeof(account.address), "kei@example.net");
	(void)snprintf(account.user, sizeof(account.user), "kei@example.net");
	(void)snprintf(account.password, sizeof(account.password), "secret 1");
	(void)snprintf(server, sizeof(server), "localhost:%s", argv[2]);
	error = ml_server_parse(server, 993U, &account.imap);
	account.imap.secure = 1;
	test_check("server-parse", error == 0 && strcmp(account.imap.host, "localhost") == 0, account.imap.host);
	(void)snprintf(server, sizeof(server), "localhost:%s", argv[4]);
	(void)ml_server_parse(server, 465U, &account.smtp);
	account.smtp.secure = 1;
	test_imap(&account, "imaps");
	test_smtp(&account, argv[6], 1, "smtps");

	/* The same over STARTTLS. */
	(void)snprintf(server, sizeof(server), "localhost:%s", argv[3]);
	(void)ml_server_parse(server, 143U, &account.imap);
	(void)snprintf(server, sizeof(server), "localhost:%s", argv[5]);
	(void)ml_server_parse(server, 587U, &account.smtp);
	test_check("server-starttls", account.imap.secure == 0 && account.smtp.secure == 0, "");
	test_imap(&account, "starttls");
	test_smtp(&account, argv[6], 2, "submission");

	/* A wrong password is refused. */
	(void)snprintf(account.password, sizeof(account.password), "wrong");
	{
		struct ml_imap imap;

		error = ml_imap_open(&imap, &account);
		test_check("imap-wrong-password", error == EACCES, imap.error);
	}

	/* The outcome. */
	if (test_failures != 0) {
		printf("host-mail-backend: %d FAILED\n", test_failures);
		return 1;
	}
	printf("host-mail-backend: PASS\n");
	return 0;
}

/* Prints a check's outcome. */
static void
test_check(
	const char *name,
	int passed,
	const char *detail)
{
	/* Passed. */
	if (passed) {
		printf("PASS %s\n", name);
		return;
	}

	/* Failed, with what was seen. */
	printf("FAIL %s [%s]\n", name, detail);
	test_failures++;
}

/* Reads messages of each kind. */
static void
test_mime(void)
{
	static const char alternative[] =
	    "From: =?UTF-8?B?44GK5q+N44GV44KT?= <mother@example.jp>\r\n"
	    "To: kei@example.net\r\n"
	    "Subject: =?UTF-8?Q?=E9=87=8E=E8=8F=9C?= =?UTF-8?B?44KS?= sent\r\n"
	    "Date: Fri, 2 Oct 2026 07:45:00 +0900\r\n"
	    "Message-ID: <m1@example.jp>\r\n"
	    "Content-Type: multipart/alternative;\r\n boundary=\"xyz\"\r\n"
	    "\r\n"
	    "preamble\r\n"
	    "--xyz\r\n"
	    "Content-Type: text/plain; charset=\"UTF-8\"\r\n"
	    "Content-Transfer-Encoding: quoted-printable\r\n"
	    "\r\n"
	    "=E3=81=8A=E3=81=AF=E3=82=88=E3=81=86 a long line that wraps=\r\n"
	    " here\r\n"
	    "--xyz\r\n"
	    "Content-Type: text/html\r\n"
	    "\r\n"
	    "<p>html</p>\r\n"
	    "--xyz--\r\n";
	static const char latin[] =
	    "From: Jos\xe9 <jose@example.es>\r\n"
	    "Subject: =?ISO-8859-1?Q?Caf=E9?=\r\n"
	    "Date: 3 Oct 2026 10:00:00 GMT\r\n"
	    "Content-Type: text/plain; charset=iso-8859-1\r\n"
	    "\r\n"
	    "Un caf\xe9.\r\n";
	static const char html_only[] =
	    "From: News <news@example.com>\r\n"
	    "Subject: News\r\n"
	    "Content-Type: text/html; charset=utf-8\r\n"
	    "Content-Transfer-Encoding: base64\r\n"
	    "\r\n"
	    "PHN0eWxlPnB7Y29sb3I6cmVkfTwvc3R5bGU+PHA+SGVsbG8gJmFtcDsgd2VsY29tZTwvcD48cD5MaW5lJiMzMjsy\r\n"
	    "PC9wPg==\r\n";
	static const char attached[] =
	    "From: a@example.org\r\n"
	    "Subject: File\r\n"
	    "Content-Type: multipart/mixed; boundary=b\r\n"
	    "\r\n"
	    "--b\r\n"
	    "\r\n"
	    "See the file.\r\n"
	    "--b\r\n"
	    "Content-Type: application/pdf; name=\"=?UTF-8?B?5Zyw5Zuz?=.pdf\"\r\n"
	    "Content-Transfer-Encoding: base64\r\n"
	    "\r\n"
	    "AAAAAAAAAAAAAAAA\r\n"
	    "--b--\r\n";
	struct ml_parsed parsed;
	int error;

	/* A multipart/alternative: the plain part, its soft line break, the subject's words joined. */
	error = ml_mime_parse(alternative, sizeof(alternative) - 1U, &parsed);
	test_check("mime-alternative-from", error == 0 &&
	    strcmp(parsed.from_name, "\xe3\x81\x8a\xe6\xaf\x8d\xe3\x81\x95\xe3\x82\x93") == 0 &&
	    strcmp(parsed.from_address, "mother@example.jp") == 0, parsed.from_name);
	test_check("mime-alternative-subject", strcmp(parsed.subject, "\xe9\x87\x8e\xe8\x8f\x9c\xe3\x82\x92 sent") == 0, parsed.subject);
	test_check("mime-alternative-body", parsed.body != NULL &&
	    strcmp(parsed.body, "\xe3\x81\x8a\xe3\x81\xaf\xe3\x82\x88\xe3\x81\x86 a long line that wraps here\n") == 0, parsed.body);
	test_check("mime-date", parsed.date == (time_t)1790894700, "");
	test_check("mime-message-id", strcmp(parsed.message_id, "<m1@example.jp>") == 0, parsed.message_id);
	ml_mime_release(&parsed);

	/* ISO-8859-1. */
	error = ml_mime_parse(latin, sizeof(latin) - 1U, &parsed);
	test_check("mime-latin", error == 0 &&
	    strcmp(parsed.subject, "Caf\xc3\xa9") == 0 &&
	    strcmp(parsed.body, "Un caf\xc3\xa9.\n") == 0, parsed.body);
	test_check("mime-date-gmt", parsed.date == (time_t)1791021600, "");
	ml_mime_release(&parsed);

	/* HTML only, in base64: the style left out, the paragraphs as lines, the entities. */
	error = ml_mime_parse(html_only, sizeof(html_only) - 1U, &parsed);
	test_check("mime-html", error == 0 && strcmp(parsed.body, "Hello & welcome\nLine 2") == 0, parsed.body);
	ml_mime_release(&parsed);

	/* A file carried: its decoded name and size. */
	error = ml_mime_parse(attached, sizeof(attached) - 1U, &parsed);
	test_check("mime-file", error == 0 &&
	    strcmp(parsed.file_name, "\xe5\x9c\xb0\xe5\x9b\xb3.pdf") == 0 &&
	    parsed.file_size == 12U &&
	    strcmp(parsed.body, "See the file.\n") == 0, parsed.file_name);
	ml_mime_release(&parsed);
}

/* Finds codes and not years or order numbers. */
static void
test_code(void)
{
	char code[ML_CODE_MAX];
	int found;

	/* A six-digit code after a date. */
	found = ml_code_find("Your sign-in code", "On 5 October 2026 you asked.\n\n482913\n", code, sizeof(code));
	test_check("code-six", found && strcmp(code, "482913") == 0, code);

	/* A four-digit one, the year skipped. */
	found = ml_code_find("Your verification", "Order 2026 is ready. Your one-time code is 7351.", code, sizeof(code));
	test_check("code-four", found && strcmp(code, "7351") == 0, code);

	/* Japanese words. */
	found = ml_code_find("\xe8\xaa\x8d\xe8\xa8\xbc", "\xe7\x95\xaa\xe5\x8f\xb7\xe3\x81\xaf 123456 \xe3\x81\xa7\xe3\x81\x99", code, sizeof(code));
	test_check("code-japanese", found && strcmp(code, "123456") == 0, code);

	/* No word of a code. */
	found = ml_code_find("Dinner", "See you at 1900 on table 123456.", code, sizeof(code));
	test_check("code-none", !found && code[0] == '\0', code);

	/* Digits inside a word are not a code. */
	found = ml_code_find("code", "Reference AB123456 only.", code, sizeof(code));
	test_check("code-in-word", !found, code);
}

/* Writes a message and reads it back. */
static void
test_compose(void)
{
	struct ml_account_config account;
	struct ml_parsed parsed;
	size_t length;
	char *raw;
	int error;

	/* The account. */
	memset(&account, 0, sizeof(account));
	(void)snprintf(account.name, sizeof(account.name), "\xe6\x99\xaf");
	(void)snprintf(account.address, sizeof(account.address), "kei@example.net");

	/* A reply with a Japanese subject and words with a long line, '=' and a trailing space. */
	error = ml_compose(&account, "Ben <ben@example.com>", "", "Re: \xe9\x87\x8e\xe8\x8f\x9c",
	    "Yes = sure \n.dot line\n\xe3\x81\x82 and a very long line that goes on and on past the seventy-six columns of quoted-printable",
	    "<m1@example.jp>", (time_t)1790894700, &raw, &length);
	test_check("compose", error == 0 && raw != NULL && strstr(raw, "In-Reply-To: <m1@example.jp>\r\n") != NULL &&
	    strstr(raw, "Date: Thu, 1 Oct 2026 22:45:00 +0000\r\n") != NULL, raw);
	if (raw == NULL)
		return;

	/* Read back. */
	error = ml_mime_parse(raw, length, &parsed);
	test_check("compose-roundtrip", error == 0 &&
	    strcmp(parsed.subject, "Re: \xe9\x87\x8e\xe8\x8f\x9c") == 0 &&
	    strcmp(parsed.from_name, "\xe6\x99\xaf") == 0 &&
	    strcmp(parsed.body, "Yes = sure \n.dot line\n\xe3\x81\x82 and a very long line that goes on and on past the seventy-six columns of quoted-printable\n") == 0, parsed.body);
	ml_mime_release(&parsed);
	free(raw);
}

/* Talks IMAP with the fake server. */
static void
test_imap(
	const struct ml_account_config *account,
	const char *label)
{
	static int round;
	char names[ML_FOLDERS][ML_MAILBOX_MAX];
	char name[64];
	struct test_fetches fetches;
	struct ml_imap imap;
	struct kl_socket_wait watched;
	uint32_t exists;
	uint32_t last_uid;
	size_t found;
	int arrived;
	int ready;
	int error;

	/* Logged in. */
	round++;
	error = ml_imap_open(&imap, account);
	(void)snprintf(name, sizeof(name), "%s-open", label);
	test_check(name, error == 0, imap.error);
	if (error != 0)
		return;

	/* The folders by their special use. */
	error = ml_imap_folders(&imap, names);
	(void)snprintf(name, sizeof(name), "%s-folders", label);
	test_check(name, error == 0 && strcmp(names[ML_INBOX], "INBOX") == 0 && strcmp(names[ML_SENT], "Sent") == 0 &&
	    strcmp(names[ML_ARCHIVE], "Archive") == 0 && strcmp(names[ML_TRASH], "Trash") == 0, names[ML_SENT]);

	/* The inbox: three messages on the first round (four and more later, IDLE adds one each time). */
	error = ml_imap_select(&imap, "INBOX", &exists);
	(void)snprintf(name, sizeof(name), "%s-select", label);
	test_check(name, error == 0 && exists >= 3U, "");

	/* The latest two. */
	memset(&fetches, 0, sizeof(fetches));
	error = ml_imap_fetch(&imap, 0U, 2U, test_fetched, &fetches);
	(void)snprintf(name, sizeof(name), "%s-fetch-latest", label);
	test_check(name, error == 0 && fetches.count == 2U, "");
	if (round == 1 && fetches.count == 2U) {
		test_check("imaps-fetch-file", fetches.items[0].uid == 20U &&
		    (fetches.items[0].flags & ML_UNREAD) != 0U &&
		    strcmp(fetches.items[0].parsed.file_name, "river-walk.zip") == 0, fetches.items[0].parsed.file_name);
		test_check("imaps-fetch-seen", fetches.items[1].uid == 30U && fetches.items[1].flags == 0U, "");
	}
	last_uid = 0;
	if (fetches.count != 0U)
		last_uid = fetches.items[fetches.count - 1U].uid;
	test_release(&fetches);

	/* Those after the last UID: none new ("*" gives the last again, dropped). */
	error = ml_imap_fetch(&imap, last_uid + 1U, 50U, test_fetched, &fetches);
	(void)snprintf(name, sizeof(name), "%s-fetch-after", label);
	test_check(name, error == 0 && fetches.count == 0U, "");
	test_release(&fetches);

	/* The first message read, then moved to the archive (first round only). */
	if (round == 1) {
		error = ml_imap_flag(&imap, 10U, "\\Seen", 1);
		test_check("imaps-flag", error == 0, imap.error);
		error = ml_imap_move(&imap, 10U, names[ML_ARCHIVE]);
		test_check("imaps-move", error == 0 && imap.exists == 2U, imap.error);
	}

	/* IDLE: the server tells a new message. */
	error = ml_imap_idle_start(&imap);
	(void)snprintf(name, sizeof(name), "%s-idle-start", label);
	test_check(name, error == 0, imap.error);

	/* Waits for the news: at once when the connection holds it, else on its socket. */
	memset(&watched, 0, sizeof(watched));
	watched.socket = imap.conn.socket;
	watched.events = KL_SOCKET_READABLE;
	found = 0U;
	ready = ml_conn_ready(&imap.conn);
	if (!ready) {
		error = kl_socket_poll(&watched, 1U, 5000, &found);
		if (error == 0 && found > 0U)
			ready = 1;
	}

	/* Takes the news when it came. */
	arrived = 0;
	if (ready)
		error = ml_imap_idle_take(&imap, &arrived);

	/* It came, and it was a new message. */
	(void)snprintf(name, sizeof(name), "%s-idle-arrived", label);
	test_check(name, ready && error == 0 && arrived == 1, "");
	error = ml_imap_idle_stop(&imap);
	(void)snprintf(name, sizeof(name), "%s-idle-stop", label);
	test_check(name, error == 0, imap.error);

	/* The new message by its UID, with its code. */
	error = ml_imap_fetch(&imap, last_uid + 1U, 50U, test_fetched, &fetches);
	(void)snprintf(name, sizeof(name), "%s-fetch-new", label);
	test_check(name, error == 0 && fetches.count == 1U && strcmp(fetches.items[0].parsed.code, "7351") == 0, "");
	test_release(&fetches);

	/* A sent message appended to Sent. */
	error = ml_imap_append(&imap, names[ML_SENT], "Subject: x\r\n\r\nsent\r\n", 21U);
	(void)snprintf(name, sizeof(name), "%s-append", label);
	test_check(name, error == 0, imap.error);
	ml_imap_close(&imap);
}

/* Sends a message with SMTP and checks what the fake server got. */
static void
test_smtp(
	const struct ml_account_config *account,
	const char *outdir,
	int number,
	const char *label)
{
	const char *receivers[2];
	char error_text[ML_TEXT_MAX];
	char path[512];
	char name[64];
	size_t length;
	size_t got_length;
	char *raw;
	char *got;
	char *envelope;
	int error;

	/* The message, a line starting with a dot in it. */
	error = ml_compose(account, "Ben <ben@example.com>", "aiko@example.org", "Hello", "Line one\n.dot\nend", "", (time_t)1790894700, &raw, &length);
	if (error != 0)
		return;
	receivers[0] = "ben@example.com";
	receivers[1] = "aiko@example.org";
	error = ml_smtp_send(account, receivers, 2U, raw, length, error_text, sizeof(error_text));
	(void)snprintf(name, sizeof(name), "%s-send", label);
	test_check(name, error == 0, error_text);

	/* The server got the same bytes (the dot undoubled). */
	(void)snprintf(path, sizeof(path), "%s/smtp-%d.eml", outdir, number);
	got = test_read_file(path, &got_length);
	(void)snprintf(name, sizeof(name), "%s-message", label);
	test_check(name, got != NULL && got_length == length && memcmp(got, raw, length) == 0, path);
	free(got);

	/* And the envelope. */
	(void)snprintf(path, sizeof(path), "%s/smtp-%d.env", outdir, number);
	envelope = test_read_file(path, &got_length);
	(void)snprintf(name, sizeof(name), "%s-envelope", label);
	test_check(name, envelope != NULL && strcmp(envelope, "from=kei@example.net\nto=ben@example.com,aiko@example.org\n") == 0, envelope);
	free(envelope);
	free(raw);
}

/* Keeps a fetched message, read by mime.c. */
static void
test_fetched(
	void *data,
	uint32_t uid,
	unsigned flags,
	size_t size,
	const char *raw,
	size_t length)
{
	struct test_fetches *fetches;

	(void)size;

	/* Kept while there is room. */
	fetches = data;
	if (fetches->count == TEST_FETCHED_MAX)
		return;
	fetches->items[fetches->count].uid = uid;
	fetches->items[fetches->count].flags = flags;
	(void)ml_mime_parse(raw, length, &fetches->items[fetches->count].parsed);
	fetches->count++;
}

/* Frees what the fetches kept. */
static void
test_release(
	struct test_fetches *fetches)
{
	unsigned index;

	/* Each one. */
	for (index = 0; index < fetches->count; index++)
		ml_mime_release(&fetches->items[index].parsed);
	fetches->count = 0;
}

/* Reads a whole file, NUL ended (NULL when it cannot). */
static char *
test_read_file(
	const char *path,
	size_t *length)
{
	FILE *file;
	char *bytes;
	long size;

	/* Opened, its size. */
	file = fopen(path, "rb");
	if (file == NULL)
		return NULL;
	(void)fseek(file, 0L, SEEK_END);
	size = ftell(file);
	(void)fseek(file, 0L, SEEK_SET);

	/* Its bytes. */
	bytes = malloc((size_t)size + 1U);
	if (bytes == NULL) {
		fclose(file);
		return NULL;
	}
	*length = fread(bytes, 1U, (size_t)size, file);
	bytes[*length] = '\0';
	fclose(file);
	return bytes;
}
