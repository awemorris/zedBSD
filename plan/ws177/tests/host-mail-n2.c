/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws177-p015: the host test of Mail's basic operations below the window
 * (userland/desktop/mailer/): run without the tests' CA, so that the fake
 * server's certificate does not verify.
 *
 * 1. TLS: the certificate that does not verify is ML_ERROR_UNTRUSTED with
 *    its fingerprint; the same fingerprint as the server's pin is taken,
 *    another is not; the IMAP and SMTP servers on a plain port that do not
 *    offer STARTTLS are ML_ERROR_NO_TLS (one-shot servers of this test).
 * 2. IMAP: the server's capabilities are read, and a message is deleted
 *    for good (EXPUNGE without UIDPLUS).
 * 3. The store: the short dates follow the day ("Yesterday"), an account
 *    changed and removed (its messages go, the later ones renumbered).
 * 4. The accounts on the disk: the pins kept and read back, and a
 *    password forgotten.
 *
 *     host-mail-n2 IMAPS SMTPS HOME
 *
 * Prints "PASS name" or "FAIL name [detail]" for each check; exits with 1
 * when one failed.
 */

#include "userland/desktop/mailer/mailer.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/* The checks that failed. */
static int test_failures;

/* The UIDs a fetch gave, and how many. */
static uint32_t test_uids[8];
static unsigned test_uid_count;

int main(int argc, char **argv);
static void test_check(const char *name, int passed, const char *detail);
static void test_tls(const char *imaps, const char *smtps);
static void test_delete(const char *imaps, const char *pin);
static void test_no_tls(void);
static void test_store(void);
static void test_disk(const char *home);
static int test_one_shot(const char *const *answers, size_t count, unsigned *port);
static void test_fetched(void *data, uint32_t uid, unsigned flags, size_t size, const char *raw, size_t length);
static void test_account(struct ml_account_config *account, const char *imap, const char *smtp);

/*
 * Runs every part of the test.
 */
int
main(
	int argc,
	char **argv)
{
	/* The arguments. */
	if (argc != 4) {
		fprintf(stderr, "usage: host-mail-n2 IMAPS SMTPS HOME\n");
		return 2;
	}

	/* Each part. */
	test_tls(argv[1], argv[2]);
	test_no_tls();
	test_store();
	test_disk(argv[3]);

	/* The outcome. */
	if (test_failures != 0) {
		printf("host-mail-n2: %d FAILED\n", test_failures);
		return 1;
	}
	printf("host-mail-n2: PASS\n");
	return 0;
}

/* The log of Mail's window, which the store does not use; kept for the link. */
void
ml_log(
	const char *format,
	...)
{
	va_list arguments;

	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	fputc('\n', stderr);
	va_end(arguments);
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

/* Untrusted, pinned, and wrongly pinned certificates over IMAP and SMTP. */
static void
test_tls(
	const char *imaps,
	const char *smtps)
{
	struct ml_account_config account;
	struct ml_imap imap;
	char pin[ML_PIN_MAX];
	char words[ML_TEXT_MAX];
	const char *receivers[1];
	int error;

	/* Not verified: untrusted, with a fingerprint of 64 hex digits. */
	test_account(&account, imaps, smtps);
	error = ml_imap_open(&imap, &account);
	(void)snprintf(pin, sizeof(pin), "%s", ml_tls_fingerprint());
	test_check("imap-untrusted", error == ML_ERROR_UNTRUSTED && strlen(pin) == 64U && strstr(imap.error, "certificate verify failed") != NULL, imap.error);

	/* Another pin: still untrusted. */
	(void)snprintf(account.imap.pin, sizeof(account.imap.pin), "%064d", 0);
	error = ml_imap_open(&imap, &account);
	test_check("imap-wrong-pin", error == ML_ERROR_UNTRUSTED, imap.error);

	/* Its own pin: logged in, its capabilities read (the fake server has none of Mail's). */
	(void)snprintf(account.imap.pin, sizeof(account.imap.pin), "%s", pin);
	error = ml_imap_open(&imap, &account);
	test_check("imap-pinned", error == 0 && imap.capabilities == 0U, imap.error);
	if (error == 0)
		ml_imap_close(&imap);

	/* SMTP untrusted, then pinned. */
	receivers[0] = "ben@example.com";
	error = ml_smtp_send(&account, receivers, 1U, "Subject: x\r\n\r\nx\r\n", 17U, words, sizeof(words));
	test_check("smtp-untrusted", error == ML_ERROR_UNTRUSTED && strlen(ml_tls_fingerprint()) == 64U, words);
	(void)snprintf(account.smtp.pin, sizeof(account.smtp.pin), "%s", ml_tls_fingerprint());
	error = ml_smtp_send(&account, receivers, 1U, "Subject: x\r\n\r\nx\r\n", 17U, words, sizeof(words));
	test_check("smtp-pinned", error == 0, words);

	/* A message deleted for good over the pinned session. */
	test_delete(imaps, pin);
}

/* Deletes the inbox's first message for good and finds one fewer. */
static void
test_delete(
	const char *imaps,
	const char *pin)
{
	struct ml_account_config account;
	struct ml_imap imap;
	uint32_t exists;
	unsigned before;
	uint32_t uid;
	int error;

	/* The session. */
	test_account(&account, imaps, imaps);
	(void)snprintf(account.imap.pin, sizeof(account.imap.pin), "%s", pin);
	error = ml_imap_open(&imap, &account);
	if (error != 0) {
		test_check("delete", 0, "no session");
		return;
	}

	/* The inbox's messages. */
	error = ml_imap_select(&imap, "INBOX", &exists);
	test_uid_count = 0;
	if (error == 0)
		error = ml_imap_fetch(&imap, 0, 50U, test_fetched, NULL);
	before = test_uid_count;
	uid = 0;
	if (before != 0U)
		uid = test_uids[0];

	/* The first one deleted, then the inbox again. */
	if (error == 0)
		error = ml_imap_delete(&imap, uid);
	test_uid_count = 0;
	if (error == 0)
		error = ml_imap_fetch(&imap, 0, 50U, test_fetched, NULL);
	ml_imap_close(&imap);
	test_check("delete", error == 0 && before == 3U && test_uid_count == 2U && test_uids[0] != uid, imap.error);
}

/* Servers on a plain port that refuse STARTTLS (IMAP) or do not offer it (SMTP). */
static void
test_no_tls(void)
{
	static const char *const imap_answers[] = { "* OK fake\r\n", "A0001 BAD no TLS here\r\n" };
	static const char *const smtp_answers[] = { "220 fake\r\n", "250-fake\r\n250 AUTH PLAIN\r\n" };
	struct ml_account_config account;
	struct ml_imap imap;
	char server[32];
	char words[ML_TEXT_MAX];
	const char *receivers[1];
	unsigned port;
	int child;
	int error;
	int status;

	/* IMAP. */
	child = test_one_shot(imap_answers, 2U, &port);
	(void)snprintf(server, sizeof(server), "127.0.0.1:%u", port);
	test_account(&account, server, server);
	error = ml_imap_open(&imap, &account);
	(void)waitpid(child, &status, 0);
	test_check("imap-no-tls", error == ML_ERROR_NO_TLS, imap.error);

	/* SMTP. */
	child = test_one_shot(smtp_answers, 2U, &port);
	(void)snprintf(server, sizeof(server), "127.0.0.1:%u", port);
	test_account(&account, server, server);
	receivers[0] = "ben@example.com";
	error = ml_smtp_send(&account, receivers, 1U, "x\r\n", 3U, words, sizeof(words));
	(void)waitpid(child, &status, 0);
	test_check("smtp-no-tls", error == ML_ERROR_NO_TLS && strstr(words, "STARTTLS") != NULL, words);
}

/* The store's dates, and an account changed and removed. */
static void
test_store(void)
{
	struct ml_account_config account;
	const struct ml_message *messages;
	struct ml_message message;
	time_t now;
	size_t count;
	int changed;
	int index;

	/* Three accounts. */
	test_account(&account, "imap.a.example:993", "smtp.a.example:465");
	(void)ml_store_add_account(&account);
	(void)snprintf(account.name, sizeof(account.name), "B");
	(void)ml_store_add_account(&account);
	(void)snprintf(account.name, sizeof(account.name), "C");
	(void)ml_store_add_account(&account);

	/* A message of each, dated a day before now, its words written for that day. */
	now = time(NULL);
	memset(&message, 0, sizeof(message));
	message.date = now - 86400;
	message.date_short = "09:41";
	message.subject = "s";
	for (index = 0; index < 3; index++) {
		message.account = index;
		message.uid = (uint32_t)index + 1U;
		(void)ml_store_insert(&message);
	}

	/* Today's words: "Yesterday"; the same day again changes nothing. */
	changed = ml_store_redate(now);
	messages = ml_messages(&count);
	test_check("redate", changed == 1 && count == 3U && strcmp(messages[0].date_short, "Yesterday") == 0, messages[0].date_short);
	changed = ml_store_redate(now);
	test_check("redate-same-day", changed == 0, "");

	/* The second account changed: its messages dropped. */
	(void)snprintf(account.name, sizeof(account.name), "B2");
	(void)ml_store_set_account(1, &account);
	ml_store_drop_messages(1);
	messages = ml_messages(&count);
	test_check("drop", count == 2U && messages[0].account == 0 && messages[1].account == 2, "");

	/* The first removed: the third account and its message move up. */
	(void)ml_store_remove_account(0);
	messages = ml_messages(&count);
	test_check("remove", count == 1U && messages[0].account == 1 && messages[0].uid == 3U, "");
	(void)ml_accounts(&count);
	test_check("remove-accounts", count == 2U && strcmp(ml_accounts(&count)[0].name, "B2") == 0 && strcmp(ml_accounts(&count)[1].name, "C") == 0, "");
	ml_store_release();
}

/* The pins on the disk, and a password forgotten. */
static void
test_disk(
	const char *home)
{
	struct ml_account_config accounts[2];
	struct ml_account_config read[ML_ACCOUNTS_MAX];
	char password[ML_TEXT_MAX];
	size_t count;
	int error;

	/* Two accounts, the first with both pins. */
	test_account(&accounts[0], "imap.a.example:993", "smtp.a.example:587");
	(void)snprintf(accounts[0].imap.pin, sizeof(accounts[0].imap.pin), "%064d", 1);
	(void)snprintf(accounts[0].smtp.pin, sizeof(accounts[0].smtp.pin), "%064d", 2);
	test_account(&accounts[1], "imap.b.example:993", "smtp.b.example:465");
	(void)snprintf(accounts[1].address, sizeof(accounts[1].address), "ann@example.org");
	error = ml_accounts_save(home, accounts, 2U);

	/* Read back with the pins. */
	if (error == 0)
		error = ml_accounts_load(home, read, ML_ACCOUNTS_MAX, &count);
	test_check("pins", error == 0 && count == 2U && strcmp(read[0].imap.pin, accounts[0].imap.pin) == 0 &&
	    strcmp(read[0].smtp.pin, accounts[0].smtp.pin) == 0 && read[0].smtp.port == 587U && read[1].imap.pin[0] == '\0', "");

	/* The first address's password forgotten, the second's kept. */
	(void)ml_secret_save(home, accounts[0].address, "");
	error = ml_secret_load(home, accounts[0].address, password, sizeof(password));
	test_check("forget", error == ENOENT, "");
	error = ml_secret_load(home, accounts[1].address, password, sizeof(password));
	test_check("forget-other-kept", error == 0 && strcmp(password, "secret 1") == 0, password);
}

/*
 * Starts a server of one connection in a child: it sends each answer in
 * turn, reading a line from the client before every answer but the
 * first, then closes.  Returns the child, with its port.
 */
static int
test_one_shot(
	const char *const *answers,
	size_t count,
	unsigned *port)
{
	struct sockaddr_in address;
	socklen_t length;
	char line[512];
	ssize_t got;
	size_t index;
	int listener;
	int client;
	int child;

	/* A listener on a free port of the loopback. */
	listener = socket(AF_INET, SOCK_STREAM, 0);
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	(void)bind(listener, (struct sockaddr *)&address, sizeof(address));
	(void)listen(listener, 1);
	length = sizeof(address);
	(void)getsockname(listener, (struct sockaddr *)&address, &length);
	*port = ntohs(address.sin_port);

	/* The child serves one connection. */
	child = fork();
	if (child != 0) {
		(void)close(listener);
		return child;
	}
	client = accept(listener, NULL, NULL);
	for (index = 0; index < count; index++) {
		if (index != 0U)
			got = read(client, line, sizeof(line));
		(void)write(client, answers[index], strlen(answers[index]));
	}
	got = read(client, line, sizeof(line));
	(void)got;
	(void)close(client);
	_exit(0);
}

/* Keeps a fetched message's UID. */
static void
test_fetched(
	void *data,
	uint32_t uid,
	unsigned flags,
	size_t size,
	const char *raw,
	size_t length)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(flags);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(raw);
	UNUSED_PARAMETER(length);

	/* Kept while there is room. */
	if (test_uid_count < sizeof(test_uids) / sizeof(test_uids[0])) {
		test_uids[test_uid_count] = uid;
		test_uid_count++;
	}
}

/* Makes the fake server's account with its two servers ("host:port"). */
static void
test_account(
	struct ml_account_config *account,
	const char *imap,
	const char *smtp)
{
	char server[ML_TEXT_MAX];

	/* The user. */
	memset(account, 0, sizeof(*account));
	(void)snprintf(account->name, sizeof(account->name), "Kei");
	(void)snprintf(account->address, sizeof(account->address), "kei@example.net");
	(void)snprintf(account->user, sizeof(account->user), "kei@example.net");
	(void)snprintf(account->password, sizeof(account->password), "secret 1");

	/* The servers, "localhost:N" for a bare port, which is the fake server's TLS from the start. */
	if (strchr(imap, ':') == NULL) {
		(void)snprintf(server, sizeof(server), "localhost:%s", imap);
		(void)ml_server_parse(server, 993U, &account->imap);
		account->imap.secure = 1;
	} else {
		(void)ml_server_parse(imap, 993U, &account->imap);
	}
	if (strchr(smtp, ':') == NULL) {
		(void)snprintf(server, sizeof(server), "localhost:%s", smtp);
		(void)ml_server_parse(server, 465U, &account->smtp);
		account->smtp.secure = 1;
	} else {
		(void)ml_server_parse(smtp, 465U, &account->smtp);
	}
}
