/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws177-p016: the host test of Mail's compatibility with servers and
 * messages (userland/desktop/mailer/), against fake-mail-server.py started
 * with --caps "MOVE UIDPLUS" --large --japanese-folders --smtp-auth LOGIN.
 *
 * 1. Japanese character sets (jis.c through mime.c): ISO-2022-JP,
 *    Shift_JIS and EUC-JP words, the half-width katakana, an encoded-word
 *    subject in ISO-2022-JP, and a byte that is not a character.
 * 2. BODYSTRUCTURE (structure.c): a file before the words, an alternative
 *    inside a mixed, a lone part, and a literal that does not read.
 * 3. Writing (compose.c): a Japanese name in To as an encoded word, long
 *    fields folded before 78 characters, a long Japanese subject in
 *    several words, read back the same.
 * 4. IMAP: the Japanese folders (modified UTF-7, one as a literal, a
 *    \Noselect parent left out), UID MOVE, UID EXPUNGE of one message
 *    alone, and a message of 1.5 MiB fetched by its structure.
 * 5. SMTP: AUTH LOGIN when the server offers it alone.
 *
 *     host-mail-n CA IMAPS SMTPS OUTDIR
 *
 * Prints "PASS name" or "FAIL name [detail]" for each check; exits with 1
 * when one failed.
 */

#include "userland/desktop/mailer/mail.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The checks that failed. */
static int test_failures;

/* The UIDs and sizes a fetch gave, and how many. */
static uint32_t test_uids[16];
static size_t test_sizes[16];
static unsigned test_count;

int main(int argc, char **argv);
static void test_check(const char *name, int passed, const char *detail);
static void test_japanese(void);
static void test_structure(void);
static void test_compose(void);
static void test_imap(const char *imaps);
static void test_smtp(const char *smtps, const char *outdir);
static void test_fetched(void *data, uint32_t uid, unsigned flags, size_t size, const char *raw, size_t length);
static void test_account(struct ml_account_config *account, const char *imaps, const char *smtps);
static int test_lines_fit(const char *raw, size_t header_length);

/*
 * Runs every part of the test.
 */
int
main(
	int argc,
	char **argv)
{
	int error;

	/* The arguments. */
	if (argc != 5) {
		fprintf(stderr, "usage: host-mail-n CA IMAPS SMTPS OUTDIR\n");
		return 2;
	}
	error = ml_tls_add_ca_file(argv[1]);
	if (error != 0)
		return 2;

	/* Each part. */
	test_japanese();
	test_structure();
	test_compose();
	test_imap(argv[2]);
	test_smtp(argv[3], argv[4]);

	/* The outcome. */
	if (test_failures != 0) {
		printf("host-mail-n: %d FAILED\n", test_failures);
		return 1;
	}
	printf("host-mail-n: PASS\n");
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

/* Words and a subject in the Japanese character sets. */
static void
test_japanese(void)
{
	/* 日本語のメール in ISO-2022-JP, Shift_JIS and EUC-JP; ｶﾅ half-width; an encoded-word subject 件名. */
	static const char iso2022[] =
	    "Subject: =?ISO-2022-JP?B?GyRCN29MPhsoQg==?=\r\n"
	    "Content-Type: text/plain; charset=ISO-2022-JP\r\n\r\n"
	    "\x1b$BF|K\\8l$N%a!<%k\x1b(B ok \x1b(I66\x1b(B\r\n";
	static const char shift[] =
	    "Content-Type: text/plain; charset=Shift_JIS\r\n\r\n"
	    "\x93\xfa\x96\x7b\x8c\xea\x82\xcc\x83\x81\x81\x5b\x83\x8b \xb6\xc5\r\n";
	static const char euc[] =
	    "Content-Type: text/plain; charset=EUC-JP\r\n\r\n"
	    "\xc6\xfc\xcb\xdc\xb8\xec\xa4\xce\xa5\xe1\xa1\xbc\xa5\xeb \x8e\xb6\x8e\xc5 \xff!\r\n";
	struct ml_parsed parsed;
	int error;

	/* ISO-2022-JP, its subject too. */
	error = ml_mime_parse(iso2022, sizeof(iso2022) - 1U, &parsed);
	test_check("iso-2022-jp", error == 0 && strcmp(parsed.body, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe3\x83\xa1\xe3\x83\xbc\xe3\x83\xab ok \xef\xbd\xb6\xef\xbd\xb6\n") == 0, parsed.body);
	test_check("iso-2022-jp-subject", strcmp(parsed.subject, "\xe4\xbb\xb6\xe5\x90\x8d") == 0, parsed.subject);
	ml_mime_release(&parsed);

	/* Shift_JIS. */
	error = ml_mime_parse(shift, sizeof(shift) - 1U, &parsed);
	test_check("shift-jis", error == 0 && strcmp(parsed.body, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe3\x83\xa1\xe3\x83\xbc\xe3\x83\xab \xef\xbd\xb6\xef\xbe\x85\n") == 0, parsed.body);
	ml_mime_release(&parsed);

	/* EUC-JP, with a byte that is not a character. */
	error = ml_mime_parse(euc, sizeof(euc) - 1U, &parsed);
	test_check("euc-jp", error == 0 && strcmp(parsed.body, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe3\x83\xa1\xe3\x83\xbc\xe3\x83\xab \xef\xbd\xb6\xef\xbe\x85 \xef\xbf\xbd!\n") == 0, parsed.body);
	ml_mime_release(&parsed);
}

/* BODYSTRUCTURE's texts. */
static void
test_structure(void)
{
	struct ml_structure structure;
	int error;

	/* A file before the words. */
	error = ml_structure_parse("((\"APPLICATION\" \"OCTET-STREAM\" (\"NAME\" \"big.bin\") NIL NIL \"BASE64\" 2152340 NIL "
	    "(\"ATTACHMENT\" (\"FILENAME\" \"big.bin\")) NIL NIL)(\"TEXT\" \"PLAIN\" (\"CHARSET\" \"iso-2022-jp\") NIL NIL \"7BIT\" 40 1 NIL NIL NIL NIL) \"MIXED\")",
	    &structure);
	test_check("structure-file-first", error == 0 && strcmp(structure.text.section, "2") == 0 && strcmp(structure.text.charset, "iso-2022-jp") == 0 &&
	    strcmp(structure.file_name, "big.bin") == 0 && structure.file_size > 1572000U && structure.file_size < 1574000U, structure.text.section);

	/* An alternative inside a mixed. */
	error = ml_structure_parse("(((\"TEXT\" \"PLAIN\" (\"CHARSET\" \"utf-8\") NIL NIL \"QUOTED-PRINTABLE\" 10 1 NIL NIL NIL NIL)"
	    "(\"TEXT\" \"HTML\" (\"CHARSET\" \"utf-8\") NIL NIL \"BASE64\" 20 1 NIL NIL NIL NIL) \"ALTERNATIVE\" (\"BOUNDARY\" \"b\") NIL NIL)"
	    "(\"IMAGE\" \"PNG\" (\"NAME\" \"a.png\") NIL NIL \"BASE64\" 400 NIL NIL NIL NIL) \"MIXED\" (\"BOUNDARY\" \"m\") NIL NIL)",
	    &structure);
	test_check("structure-nested", error == 0 && strcmp(structure.text.section, "1.1") == 0 && strcmp(structure.text.encoding, "quoted-printable") == 0 &&
	    strcmp(structure.html.section, "1.2") == 0 && strcmp(structure.file_name, "a.png") == 0, structure.text.section);

	/* A lone part. */
	error = ml_structure_parse("(\"TEXT\" \"PLAIN\" (\"CHARSET\" \"us-ascii\") NIL NIL \"7BIT\" 12 1 NIL NIL NIL NIL)", &structure);
	test_check("structure-single", error == 0 && strcmp(structure.text.section, "1") == 0 && strcmp(structure.text.type, "text/plain") == 0, structure.text.section);

	/* A literal is not read. */
	error = ml_structure_parse("(\"TEXT\" \"PLAIN\" {5}", &structure);
	test_check("structure-literal", error == EPROTO, "");
}

/* A message with a Japanese name, long fields and a long Japanese subject. */
static void
test_compose(void)
{
	struct ml_account_config account;
	struct ml_parsed parsed;
	char cc[600];
	char subject[400];
	char *raw;
	char *header_end;
	size_t length;
	int index;
	int error;

	/* The account, the receivers and the subject (田中 愛子; twenty receivers; 長い件名 eleven times). */
	memset(&account, 0, sizeof(account));
	(void)snprintf(account.name, sizeof(account.name), "Kei");
	(void)snprintf(account.address, sizeof(account.address), "kei@example.net");
	cc[0] = '\0';
	for (index = 0; index < 20; index++)
		(void)snprintf(cc + strlen(cc), sizeof(cc) - strlen(cc), "%sperson%02d@example.com", index == 0 ? "" : ", ", index);
	subject[0] = '\0';
	for (index = 0; index < 11; index++)
		(void)snprintf(subject + strlen(subject), sizeof(subject) - strlen(subject), "%s", "\xe9\x95\xb7\xe3\x81\x84\xe4\xbb\xb6\xe5\x90\x8d");
	error = ml_compose(&account, "\"\xe7\x94\xb0\xe4\xb8\xad \xe6\x84\x9b\xe5\xad\x90\" <aiko@example.org>, ben@example.com", cc, subject, "hi", NULL, 1759700000, &raw, &length);
	if (error != 0) {
		test_check("compose", 0, "not written");
		return;
	}

	/* The name in To as an encoded word, and every header line within 78 characters. */
	header_end = strstr(raw, "\r\n\r\n");
	test_check("compose-encoded-name", strstr(raw, "To: =?UTF-8?B?") != NULL && strstr(raw, "?= <aiko@example.org>, ben@example.com") != NULL, raw);
	test_check("compose-folded", header_end != NULL && test_lines_fit(raw, (size_t)(header_end - raw)), raw);

	/* Read back: the same name, receivers and subject. */
	error = ml_mime_parse(raw, length, &parsed);
	test_check("compose-read-back", error == 0 && strcmp(parsed.subject, subject) == 0 && strstr(parsed.to, "\xe7\x94\xb0\xe4\xb8\xad \xe6\x84\x9b\xe5\xad\x90") != NULL &&
	    strstr(parsed.cc, "person19@example.com") != NULL, parsed.subject);
	ml_mime_release(&parsed);
	free(raw);
}

/* The Japanese folders, UID MOVE, UID EXPUNGE, and the large message. */
static void
test_imap(
	const char *imaps)
{
	char folders[ML_FOLDERS][ML_MAILBOX_MAX];
	struct ml_account_config account;
	struct ml_parsed parsed;
	struct ml_imap imap;
	uint32_t exists;
	uint32_t large;
	unsigned index;
	int error;

	/* The session; MOVE and UIDPLUS. */
	test_account(&account, imaps, imaps);
	error = ml_imap_open(&imap, &account);
	test_check("imap-capabilities", error == 0 && (imap.capabilities & (ML_IMAP_CAN_MOVE | ML_IMAP_CAN_UIDPLUS)) == (ML_IMAP_CAN_MOVE | ML_IMAP_CAN_UIDPLUS), imap.error);
	if (error != 0)
		return;

	/* The folders by their Japanese names, the trash's sent as a literal. */
	error = ml_imap_folders(&imap, folders);
	test_check("imap-japanese-folders", error == 0 && strcmp(folders[ML_SENT], "&kAFP4W4IMH8-") == 0 && strcmp(folders[ML_DRAFTS], "&Tgtm+DBN-") == 0 &&
	    strcmp(folders[ML_ARCHIVE], "&MKIw,DCrMKQw1g-") == 0 && strcmp(folders[ML_TRASH], "&MFQwf3ux-") == 0, folders[ML_TRASH]);

	/* The inbox: four messages, the last one large. */
	error = ml_imap_select(&imap, "INBOX", &exists);
	test_count = 0;
	if (error == 0)
		error = ml_imap_fetch(&imap, 0, 50U, test_fetched, NULL);
	test_check("imap-inbox", error == 0 && test_count == 4U && test_sizes[3] > ML_FETCH_BYTES, "");
	if (test_count != 4U) {
		ml_imap_close(&imap);
		return;
	}
	large = test_uids[3];

	/* The large message by its structure: its subject, its words after the file, the file's real size. */
	error = ml_imap_fetch_large(&imap, large, &parsed);
	test_check("imap-large", error == 0 && strcmp(parsed.subject, "\xe5\xa4\xa7\xe3\x81\x8d\xe3\x81\xaa\xe6\xb7\xbb\xe4\xbb\x98") == 0 &&
	    strstr(parsed.body, "\xe5\xa4\xa7\xe3\x81\x8d\xe3\x81\xaa\xe6\xb7\xbb\xe4\xbb\x98\xe3\x81\xae\xe3\x83\xa1\xe3\x83\xbc\xe3\x83\xab") != NULL &&
	    strcmp(parsed.file_name, "big.bin") == 0 && parsed.file_size > 1560000U && parsed.file_size < 1590000U, parsed.body != NULL ? parsed.body : "");
	ml_mime_release(&parsed);

	/* UID MOVE of the first to the archive. */
	error = ml_imap_move(&imap, test_uids[0], folders[ML_ARCHIVE]);
	test_count = 0;
	if (error == 0)
		error = ml_imap_fetch(&imap, 0, 50U, test_fetched, NULL);
	test_check("imap-move", error == 0 && test_count == 3U, "");

	/* The second marked deleted, the third deleted for good: UID EXPUNGE takes the third alone. */
	error = ml_imap_flag(&imap, test_uids[0], "\\Deleted", 1);
	if (error == 0)
		error = ml_imap_delete(&imap, test_uids[1]);
	test_count = 0;
	if (error == 0)
		error = ml_imap_fetch(&imap, 0, 50U, test_fetched, NULL);
	test_check("imap-uid-expunge", error == 0 && test_count == 2U, "");

	/* The archive has the moved one. */
	error = ml_imap_select(&imap, folders[ML_ARCHIVE], &exists);
	test_count = 0;
	if (error == 0)
		error = ml_imap_fetch(&imap, 0, 50U, test_fetched, NULL);
	index = test_count;
	test_check("imap-archive", error == 0 && index == 1U, "");
	ml_imap_close(&imap);
}

/* A message sent through a server that offers AUTH LOGIN alone. */
static void
test_smtp(
	const char *smtps,
	const char *outdir)
{
	struct ml_account_config account;
	const char *receivers[1];
	char words[ML_TEXT_MAX];
	char path[512];
	char line[32];
	FILE *file;
	int error;

	/* Sent. */
	test_account(&account, smtps, smtps);
	receivers[0] = "ben@example.com";
	error = ml_smtp_send(&account, receivers, 1U, "Subject: x\r\n\r\nx\r\n", 17U, words, sizeof(words));

	/* The login the server took. */
	line[0] = '\0';
	(void)snprintf(path, sizeof(path), "%s/logins", outdir);
	file = fopen(path, "r");
	if (file != NULL) {
		if (fgets(line, (int)sizeof(line), file) == NULL)
			line[0] = '\0';
		fclose(file);
	}
	test_check("smtp-auth-login", error == 0 && strcmp(line, "LOGIN\n") == 0, words);
}

/* Keeps a fetched message's UID and size. */
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
	UNUSED_PARAMETER(raw);
	UNUSED_PARAMETER(length);

	/* Kept while there is room. */
	if (test_count < sizeof(test_uids) / sizeof(test_uids[0])) {
		test_uids[test_count] = uid;
		test_sizes[test_count] = size;
		test_count++;
	}
}

/* Makes the fake server's account on its TLS ports. */
static void
test_account(
	struct ml_account_config *account,
	const char *imaps,
	const char *smtps)
{
	char server[64];

	/* The user. */
	memset(account, 0, sizeof(*account));
	(void)snprintf(account->name, sizeof(account->name), "Kei");
	(void)snprintf(account->address, sizeof(account->address), "kei@example.net");
	(void)snprintf(account->user, sizeof(account->user), "kei@example.net");
	(void)snprintf(account->password, sizeof(account->password), "secret 1");

	/* The servers, TLS from the start. */
	(void)snprintf(server, sizeof(server), "localhost:%s", imaps);
	(void)ml_server_parse(server, 993U, &account->imap);
	account->imap.secure = 1;
	(void)snprintf(server, sizeof(server), "localhost:%s", smtps);
	(void)ml_server_parse(server, 465U, &account->smtp);
	account->smtp.secure = 1;
}

/* Tells whether every line of a header is within 78 characters. */
static int
test_lines_fit(
	const char *raw,
	size_t header_length)
{
	size_t start;
	size_t index;

	/* Each line. */
	start = 0;
	for (index = 0; index + 1U < header_length; index++) {
		if (raw[index] != '\r' || raw[index + 1U] != '\n')
			continue;
		if (index - start > 78U)
			return 0;
		start = index + 2U;
	}

	/* The last line. */
	if (header_length - start > 78U)
		return 0;
	return 1;
}
