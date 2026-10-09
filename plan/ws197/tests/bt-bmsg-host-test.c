/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the bMessage (ws197-p003, plan/ws197/phase003/
 * phase.md section 7), built with the host's compiler under ASan and
 * UBSan.  Every bMessage is written by hand from MAP 1.4.2 section
 * 3.1.3's grammar, each LENGTH counted by hand in the comment above it.
 *
 *   read       a message from a phone: status, type, folder, the
 *              originator's and recipient's numbers and names (FN before
 *              N, N made "given family", TEL's parameters), lines with
 *              spaces before them
 *   forms      the text found by LENGTH counted the profile's way, as the
 *              text alone (with and without its last line break), without
 *              the last line break, by scanning when LENGTH is wrong or
 *              missing; LF-only input; escaped END:MSG lines
 *   envelopes  three nested (the outer recipient kept), four refused
 *   charset    UTF-8 taken; another character set and the network's own
 *              form refused; malformed sequences and NUL made U+FFFD; a
 *              long text cut at 16 KB without splitting a character
 *   refused    no BEGIN:BMSG, no END:BMSG, no text, parts out of order,
 *              past 64 KB
 *   build      the bMessage to send, byte for byte against a hand-written
 *              one; line breaks made CRLF; escapes; the type; what is
 *              refused; it reads back
 *   utf8       well-formed and malformed sequences
 *   fuzz       random changes to the bMessages into the reader
 *
 *   plan/ws197/tests/bt-phone-host-test.sh
 */

#include "userland/base/bluetoothd/bmsg.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The fuzz's rounds. */
#define TEST_FUZZ_ROUNDS	200000U

/* The room of the test's documents. */
#define TEST_DOCUMENT_MAX	(BTD_BMSG_INPUT_MAX + 4096U)

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

/* The message a test reads. */
static struct btd_bmsg message;

/* A document the test builds. */
static char document[TEST_DOCUMENT_MAX];

/* The bMessage a build writes. */
static uint8_t built[BTD_BMSG_BUILD_MAX];

/*
 * A received SMS as a phone gives it, the text "Hello\r\nworld" (12
 * bytes).  LENGTH counted the profile's way: "BEGIN:MSG\r\n" 11 + 12 +
 * "\r\nEND:MSG\r\n" 11 = 34.  The structure lines have spaces before
 * them, as in the profile's example.
 */
static const char received[] =
	"BEGIN:BMSG\r\n"
	"VERSION:1.0\r\n"
	"STATUS:UNREAD\r\n"
	"TYPE:SMS_GSM\r\n"
	"FOLDER:TELECOM/MSG/INBOX\r\n"
	"BEGIN:VCARD\r\n"
	"VERSION:2.1\r\n"
	"N:Doe;John;;;\r\n"
	"TEL;TYPE=CELL:+15551234\r\n"
	"END:VCARD\r\n"
	"  BEGIN:BENV\r\n"
	"    BEGIN:VCARD\r\n"
	"    VERSION:3.0\r\n"
	"    FN:Me Myself\r\n"
	"    N:Self;Me\r\n"
	"    TEL:+15559999\r\n"
	"    END:VCARD\r\n"
	"    BEGIN:BBODY\r\n"
	"    CHARSET:UTF-8\r\n"
	"    LENGTH:34\r\n"
	"BEGIN:MSG\r\n"
	"Hello\r\n"
	"world\r\n"
	"END:MSG\r\n"
	"    END:BBODY\r\n"
	"  END:BENV\r\n"
	"END:BMSG\r\n";

static void check(int condition, const char *what);
static int parse(const char *text);
static int parse_text(const char *head, const char *length, const char *body);
static void test_read(void);
static void test_forms(void);
static void test_envelopes(void);
static void test_charset(void);
static void test_refused(void);
static void test_build(void);
static void test_utf8(void);
static void test_fuzz(void);

/*
 * Runs every part and reports the checks.
 */
int
main(void)
{
	/* Each part. */
	test_read();
	test_forms();
	test_envelopes();
	test_charset();
	test_refused();
	test_build();
	test_utf8();
	test_fuzz();

	/* The count of what failed. */
	printf("bt-bmsg-host-test: %u checks, %u failed\n", checks, failures);
	if (failures != 0U)
		return 1;

	/* Succeeded: every check held. */
	return 0;
}

/* Counts a check, and reports it when it failed. */
static void
check(
	int condition,
	const char *what)
{
	/* One more check. */
	checks++;

	/* A failure is reported. */
	if (!condition) {
		failures++;
		printf("FAIL: %s\n", what);
	}
}

/* Reads a bMessage given as a string into the test's message. */
static int
parse(
	const char *text)
{
	int error;

	/* The bMessage without its NUL. */
	error = btd_bmsg_parse((const uint8_t *)text, strlen(text), &message);

	/* What the reader said. */
	return error;
}

/*
 * Reads a one-recipient SMS whose body has the properties head, the
 * LENGTH line length (none when NULL) and the lines from BEGIN:MSG to
 * END:MSG body.
 */
static int
parse_text(
	const char *head,
	const char *length,
	const char *body)
{
	size_t used;
	int error;

	/* The bMessage around the body. */
	used = (size_t)snprintf(document, sizeof(document), "BEGIN:BMSG\r\nVERSION:1.0\r\nTYPE:SMS_GSM\r\nBEGIN:BENV\r\nBEGIN:BBODY\r\n%s", head);
	if (length != NULL)
		used += (size_t)snprintf(document + used, sizeof(document) - used, "LENGTH:%s\r\n", length);
	snprintf(document + used, sizeof(document) - used, "%sEND:BBODY\r\nEND:BENV\r\nEND:BMSG\r\n", body);
	error = parse(document);

	/* What the reader said. */
	return error;
}

/* A received message, field by field. */
static void
test_read(void)
{
	int error;

	/* The message. */
	error = parse(received);
	check(error == 0, "read: received");
	check(message.status == BTD_BMSG_STATUS_UNREAD, "read: unread");
	check(message.type == BTD_MAP_TYPE_SMS_GSM, "read: type");
	check(strcmp(message.folder, "TELECOM/MSG/INBOX") == 0, "read: folder");

	/* The originator: TEL's parameter passed over, N made "given family". */
	check(strcmp(message.originator_number, "+15551234") == 0, "read: originator number");
	check(strcmp(message.originator_name, "John Doe") == 0, "read: originator name from N");

	/* The recipient: FN before N. */
	check(strcmp(message.recipient_number, "+15559999") == 0, "read: recipient number");
	check(strcmp(message.recipient_name, "Me Myself") == 0, "read: recipient name from FN");

	/* The text, found the profile's way. */
	check(message.form == BTD_BMSG_FORM_SPEC, "read: form 1");
	check(message.text_length == 12U && strcmp(message.text, "Hello\r\nworld") == 0, "read: text");
	check(!message.truncated, "read: not truncated");

	/* A READ status, a CDMA type, an N of the family alone. */
	error = parse("BEGIN:BMSG\r\nSTATUS:READ\r\nTYPE:sms_cdma\r\nBEGIN:VCARD\r\nN:Solo\r\nEND:VCARD\r\n"
		      "BEGIN:BENV\r\nBEGIN:BBODY\r\nBEGIN:MSG\r\nx\r\nEND:MSG\r\nEND:BBODY\r\nEND:BENV\r\nEND:BMSG\r\n");
	check(error == 0 && message.status == BTD_BMSG_STATUS_READ, "read: read");
	check(message.type == BTD_MAP_TYPE_SMS_CDMA, "read: CDMA, case not minded");
	check(strcmp(message.originator_name, "Solo") == 0, "read: N of the family alone");
	check(message.recipient_number[0] == '\0', "read: no recipient");
	check(strcmp(message.text, "x") == 0 && message.form == BTD_BMSG_FORM_SCAN, "read: text by scan without LENGTH");

	/* A second part of the text is passed over. */
	error = parse_text("", NULL, "BEGIN:MSG\r\nfirst\r\nEND:MSG\r\nBEGIN:MSG\r\nsecond\r\nEND:MSG\r\n");
	check(error == 0 && strcmp(message.text, "first") == 0, "read: first part kept");

	/* An empty text: "BEGIN:MSG\r\n" 11 and "END:MSG\r\n" 9, LENGTH 20. */
	error = parse_text("", "20", "BEGIN:MSG\r\nEND:MSG\r\n");
	check(error == 0 && message.text_length == 0U && message.form == BTD_BMSG_FORM_SPEC, "read: empty text");
}

/* The ways the text is found. */
static void
test_forms(void)
{
	int error;

	/* LENGTH as the text alone: "Hello\r\nworld" is 12. */
	error = parse_text("CHARSET:UTF-8\r\n", "12", "BEGIN:MSG\r\nHello\r\nworld\r\nEND:MSG\r\n");
	check(error == 0 && message.form == BTD_BMSG_FORM_TEXT, "forms: text alone");
	check(strcmp(message.text, "Hello\r\nworld") == 0, "forms: text alone read");

	/* LENGTH as the text with its last CRLF: 14. */
	error = parse_text("CHARSET:UTF-8\r\n", "14", "BEGIN:MSG\r\nHello\r\nworld\r\nEND:MSG\r\n");
	check(error == 0 && message.form == BTD_BMSG_FORM_TEXT, "forms: text with its line break");
	check(strcmp(message.text, "Hello\r\nworld") == 0, "forms: text with its line break read");

	/* LENGTH without the last CRLF: 11 + 12 + 2 + 7 = 32. */
	error = parse_text("CHARSET:UTF-8\r\n", "32", "BEGIN:MSG\r\nHello\r\nworld\r\nEND:MSG\r\n");
	check(error == 0 && message.form == BTD_BMSG_FORM_NO_BREAK, "forms: no last line break");
	check(strcmp(message.text, "Hello\r\nworld") == 0, "forms: no last line break read");

	/* LENGTH wrong, or not a number: the first END:MSG line. */
	error = parse_text("CHARSET:UTF-8\r\n", "999", "BEGIN:MSG\r\nHello\r\nworld\r\nEND:MSG\r\n");
	check(error == 0 && message.form == BTD_BMSG_FORM_SCAN, "forms: wrong length scans");
	check(strcmp(message.text, "Hello\r\nworld") == 0, "forms: wrong length read");
	error = parse_text("CHARSET:UTF-8\r\n", "1x", "BEGIN:MSG\r\nHello\r\nEND:MSG\r\n");
	check(error == 0 && message.form == BTD_BMSG_FORM_SCAN && strcmp(message.text, "Hello") == 0, "forms: malformed length scans");

	/*
	 * Escaped lines, found the profile's way: the text "a\r\n/END:MSG\r\n
	 * //END:MSG" is 1 + 2 + 8 + 2 + 9 = 22 bytes, so LENGTH is 44; read,
	 * each loses one slash.
	 */
	error = parse_text("", "44", "BEGIN:MSG\r\na\r\n/END:MSG\r\n//END:MSG\r\nEND:MSG\r\n");
	check(error == 0 && message.form == BTD_BMSG_FORM_SPEC, "forms: escaped lines found");
	check(strcmp(message.text, "a\r\nEND:MSG\r\n/END:MSG") == 0, "forms: escaped lines read");

	/* Escaped lines found by scanning: a slashed line is not the end. */
	error = parse_text("", NULL, "BEGIN:MSG\r\n/END:MSG\r\nEND:MSG\r\n");
	check(error == 0 && strcmp(message.text, "END:MSG") == 0, "forms: escaped line by scan");

	/* A slash elsewhere stays. */
	error = parse_text("", NULL, "BEGIN:MSG\r\n/x /END:MSG\r\nEND:MSG\r\n");
	check(error == 0 && strcmp(message.text, "/x /END:MSG") == 0, "forms: other slashes kept");

	/* LF-only input, the profile's way: "BEGIN:MSG\n" 10 + "Hello\nworld" 11 + "\nEND:MSG\n" 9 = 30. */
	error = parse("BEGIN:BMSG\nBEGIN:BENV\nBEGIN:BBODY\nLENGTH:30\nBEGIN:MSG\nHello\nworld\nEND:MSG\nEND:BBODY\nEND:BENV\nEND:BMSG\n");
	check(error == 0 && message.form == BTD_BMSG_FORM_SPEC, "forms: LF only");
	check(strcmp(message.text, "Hello\nworld") == 0, "forms: LF only read");

	/* Without the last line break of the bMessage. */
	error = parse("BEGIN:BMSG\r\nBEGIN:BENV\r\nBEGIN:BBODY\r\nBEGIN:MSG\r\nhi\r\nEND:MSG\r\nEND:BBODY\r\nEND:BENV\r\nEND:BMSG");
	check(error == 0 && strcmp(message.text, "hi") == 0, "forms: no last line break of the bMessage");

	/* A text whose lines look like structure stays text. */
	error = parse_text("", "41", "BEGIN:MSG\r\nEND:BBODY\r\nEND:BMSG\r\nEND:MSG\r\n");
	check(error == 0 && strcmp(message.text, "END:BBODY\r\nEND:BMSG") == 0, "forms: structure-like text");
}

/* Nested envelopes. */
static void
test_envelopes(void)
{
	int error;

	/* Three deep: the outer envelope's first recipient is the one kept. */
	error = parse("BEGIN:BMSG\r\n"
		      "BEGIN:BENV\r\nBEGIN:VCARD\r\nTEL:111\r\nEND:VCARD\r\nBEGIN:VCARD\r\nTEL:112\r\nEND:VCARD\r\n"
		      "BEGIN:BENV\r\nBEGIN:VCARD\r\nTEL:222\r\nEND:VCARD\r\n"
		      "BEGIN:BENV\r\nBEGIN:VCARD\r\nTEL:333\r\nEND:VCARD\r\n"
		      "BEGIN:BBODY\r\nBEGIN:MSG\r\ndeep\r\nEND:MSG\r\nEND:BBODY\r\n"
		      "END:BENV\r\nEND:BENV\r\nEND:BENV\r\nEND:BMSG\r\n");
	check(error == 0 && strcmp(message.recipient_number, "111") == 0, "envelopes: outer recipient kept");
	check(strcmp(message.text, "deep") == 0, "envelopes: text in the inner envelope");

	/* Four deep is refused. */
	error = parse("BEGIN:BMSG\r\nBEGIN:BENV\r\nBEGIN:BENV\r\nBEGIN:BENV\r\nBEGIN:BENV\r\n"
		      "BEGIN:BBODY\r\nBEGIN:MSG\r\nx\r\nEND:MSG\r\nEND:BBODY\r\n"
		      "END:BENV\r\nEND:BENV\r\nEND:BENV\r\nEND:BENV\r\nEND:BMSG\r\n");
	check(error == EINVAL, "envelopes: four refused");

	/* Envelopes not closed, or closed too often. */
	error = parse("BEGIN:BMSG\r\nBEGIN:BENV\r\nBEGIN:BBODY\r\nBEGIN:MSG\r\nx\r\nEND:MSG\r\nEND:BBODY\r\nEND:BMSG\r\n");
	check(error == EINVAL, "envelopes: unclosed refused");
	error = parse("BEGIN:BMSG\r\nBEGIN:BENV\r\nBEGIN:BBODY\r\nBEGIN:MSG\r\nx\r\nEND:MSG\r\nEND:BBODY\r\nEND:BENV\r\nEND:BENV\r\nEND:BMSG\r\n");
	check(error == EINVAL, "envelopes: closed twice refused");
}

/* The character set of the text. */
static void
test_charset(void)
{
	size_t used;
	size_t index;
	int error;

	/* Another character set, and the network's own form, are refused. */
	error = parse_text("CHARSET:ISO-8859-1\r\n", NULL, "BEGIN:MSG\r\nx\r\nEND:MSG\r\n");
	check(error == EINVAL, "charset: another refused");
	error = parse_text("ENCODING:G-7BIT\r\n", NULL, "BEGIN:MSG\r\n0011000B915121\r\nEND:MSG\r\n");
	check(error == EINVAL, "charset: native refused");

	/* An encoding said with UTF-8 is taken. */
	error = parse_text("ENCODING:8BIT\r\nCHARSET:utf-8\r\n", NULL, "BEGIN:MSG\r\nx\r\nEND:MSG\r\n");
	check(error == 0 && strcmp(message.text, "x") == 0, "charset: UTF-8 with an encoding");

	/* Malformed sequences and a NUL become U+FFFD; good characters stay. */
	memset(document, 0, sizeof(document));
	used = (size_t)snprintf(document, sizeof(document), "BEGIN:BMSG\r\nBEGIN:BENV\r\nBEGIN:BBODY\r\nBEGIN:MSG\r\n");
	memcpy(document + used, "a\xc3\xa9\xff\xc0\x80" "b\0c\xe3\x81" "\r\n", 13U);
	used += 13U;
	used += (size_t)snprintf(document + used, sizeof(document) - used, "END:MSG\r\nEND:BBODY\r\nEND:BENV\r\nEND:BMSG\r\n");
	error = btd_bmsg_parse((const uint8_t *)document, used, &message);
	check(error == 0, "charset: malformed read");
	check(strcmp(message.text, "a\xc3\xa9\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd" "b\xef\xbf\xbd" "c\xef\xbf\xbd\xef\xbf\xbd") == 0, "charset: U+FFFD for each bad byte");

	/* A text of 20000 two-byte characters: cut at 16384 bytes, whole characters. */
	used = (size_t)snprintf(document, sizeof(document), "BEGIN:BMSG\r\nBEGIN:BENV\r\nBEGIN:BBODY\r\nBEGIN:MSG\r\n");
	for (index = 0U; index < 20000U; index++) {
		document[used] = '\xc3';
		document[used + 1U] = '\xa9';
		used += 2U;
	}

	/* The end of the bMessage. */
	snprintf(document + used, sizeof(document) - used, "\r\nEND:MSG\r\nEND:BBODY\r\nEND:BENV\r\nEND:BMSG\r\n");
	error = parse(document);
	check(error == 0 && message.truncated, "charset: long text truncated");
	check(message.text_length == 16384U, "charset: 16384 bytes kept");

	/* A three-byte character that does not fit whole is left out. */
	used = (size_t)snprintf(document, sizeof(document), "BEGIN:BMSG\r\nBEGIN:BENV\r\nBEGIN:BBODY\r\nBEGIN:MSG\r\n");
	memset(document + used, 'a', 16382U);
	used += 16382U;
	memcpy(document + used, "\xe3\x81\x82", 3U);
	used += 3U;
	snprintf(document + used, sizeof(document) - used, "\r\nEND:MSG\r\nEND:BBODY\r\nEND:BENV\r\nEND:BMSG\r\n");
	error = parse(document);
	check(error == 0 && message.truncated && message.text_length == 16382U, "charset: character not split");
}

/* bMessages that are refused. */
static void
test_refused(void)
{
	size_t used;
	int error;

	/* Not a bMessage, or one not closed. */
	error = parse("BEGIN:VCARD\r\nEND:VCARD\r\n");
	check(error == EINVAL, "refused: no BEGIN:BMSG");
	error = parse("");
	check(error == EINVAL, "refused: empty");
	error = parse("BEGIN:BMSG\r\nBEGIN:BENV\r\nBEGIN:BBODY\r\nBEGIN:MSG\r\nx\r\nEND:MSG\r\nEND:BBODY\r\nEND:BENV\r\n");
	check(error == EINVAL, "refused: no END:BMSG");

	/* No text, a text without its end. */
	error = parse("BEGIN:BMSG\r\nBEGIN:BENV\r\nBEGIN:BBODY\r\nEND:BBODY\r\nEND:BENV\r\nEND:BMSG\r\n");
	check(error == EINVAL, "refused: no text");
	error = parse("BEGIN:BMSG\r\nBEGIN:BENV\r\nBEGIN:BBODY\r\nBEGIN:MSG\r\nx\r\n");
	check(error == EINVAL, "refused: text without its end");

	/* Parts out of order. */
	error = parse("BEGIN:BMSG\r\nBEGIN:BBODY\r\nBEGIN:MSG\r\nx\r\nEND:MSG\r\nEND:BBODY\r\nEND:BMSG\r\n");
	check(error == EINVAL, "refused: body outside an envelope");
	error = parse("BEGIN:BMSG\r\nBEGIN:BENV\r\nBEGIN:MSG\r\nx\r\nEND:MSG\r\nEND:BENV\r\nEND:BMSG\r\n");
	check(error == EINVAL, "refused: text outside a body");
	error = parse("BEGIN:BMSG\r\nBEGIN:BENV\r\nBEGIN:BBODY\r\nBEGIN:VCARD\r\nEND:VCARD\r\nEND:BBODY\r\nEND:BENV\r\nEND:BMSG\r\n");
	check(error == EINVAL, "refused: vCard in a body");
	error = parse("BEGIN:BMSG\r\nBEGIN:BMSG\r\nEND:BMSG\r\n");
	check(error == EINVAL, "refused: BMSG twice");
	error = parse("BEGIN:BMSG\r\nEND:VCARD\r\nEND:BMSG\r\n");
	check(error == EINVAL, "refused: END:VCARD outside a vCard");

	/* Past 64 KB. */
	used = (size_t)snprintf(document, sizeof(document), "BEGIN:BMSG\r\n");
	memset(document + used, 'x', BTD_BMSG_INPUT_MAX);
	document[used + BTD_BMSG_INPUT_MAX] = '\0';
	error = parse(document);
	check(error == E2BIG, "refused: past 64 KB");
}

/* The bMessage to send. */
static void
test_build(void)
{
	/*
	 * The text "Hi\nEND:MSG\n/x": its line breaks made CRLF and its
	 * END:MSG line given a slash, "Hi\r\n/END:MSG\r\n/x" is 4 + 10 + 2 =
	 * 16 bytes; LENGTH is 11 + 16 + 11 = 38.
	 */
	static const char expected[] =
		"BEGIN:BMSG\r\n"
		"VERSION:1.0\r\n"
		"STATUS:READ\r\n"
		"TYPE:SMS_GSM\r\n"
		"FOLDER:\r\n"
		"BEGIN:BENV\r\n"
		"BEGIN:VCARD\r\n"
		"VERSION:2.1\r\n"
		"N:\r\n"
		"TEL:+15551234\r\n"
		"END:VCARD\r\n"
		"BEGIN:BBODY\r\n"
		"CHARSET:UTF-8\r\n"
		"LENGTH:38\r\n"
		"BEGIN:MSG\r\n"
		"Hi\r\n"
		"/END:MSG\r\n"
		"/x\r\n"
		"END:MSG\r\n"
		"END:BBODY\r\n"
		"END:BENV\r\n"
		"END:BMSG\r\n";
	size_t used;
	int error;
	int same;

	/* The hand-written bMessage, byte for byte. */
	error = btd_bmsg_build("+15551234", (const uint8_t *)"Hi\nEND:MSG\n/x", 13U, BTD_MAP_TYPE_SMS_GSM, built, sizeof(built), &used);
	check(error == 0 && used == strlen(expected), "build: length");
	same = memcmp(built, expected, strlen(expected));
	check(same == 0, "build: bytes");

	/* It reads back the profile's way, the escape undone. */
	error = btd_bmsg_parse(built, used, &message);
	check(error == 0 && message.form == BTD_BMSG_FORM_SPEC, "build: reads back");
	check(strcmp(message.text, "Hi\r\nEND:MSG\r\n/x") == 0, "build: text back");
	check(strcmp(message.recipient_number, "+15551234") == 0 && message.status == BTD_BMSG_STATUS_READ, "build: recipient back");

	/* A lone CR and a CRLF become CRLF; a CDMA type: "a\r\nb\r\nc" is 7, LENGTH 29. */
	error = btd_bmsg_build("*#06#", (const uint8_t *)"a\rb\r\nc", 6U, BTD_MAP_TYPE_SMS_CDMA, built, sizeof(built), &used);
	check(error == 0, "build: line breaks");
	built[used] = '\0';
	check(strstr((const char *)built, "TYPE:SMS_CDMA\r\n") != NULL, "build: CDMA");
	check(strstr((const char *)built, "LENGTH:29\r\nBEGIN:MSG\r\na\r\nb\r\nc\r\nEND:MSG\r\n") != NULL, "build: CRLF and length");

	/* What is refused: numbers, texts, a type. */
	error = btd_bmsg_build("12a", (const uint8_t *)"x", 1U, BTD_MAP_TYPE_SMS_GSM, built, sizeof(built), &used);
	check(error == EINVAL, "build: letter in a number refused");
	error = btd_bmsg_build("", (const uint8_t *)"x", 1U, BTD_MAP_TYPE_SMS_GSM, built, sizeof(built), &used);
	check(error == EINVAL, "build: empty number refused");
	error = btd_bmsg_build("123456789012345678901234567890123", (const uint8_t *)"x", 1U, BTD_MAP_TYPE_SMS_GSM, built, sizeof(built), &used);
	check(error == EINVAL, "build: 33 digits refused");
	error = btd_bmsg_build("12345678901234567890123456789012", (const uint8_t *)"x", 1U, BTD_MAP_TYPE_SMS_GSM, built, sizeof(built), &used);
	check(error == 0, "build: 32 digits taken");
	error = btd_bmsg_build("1", (const uint8_t *)"x", 0U, BTD_MAP_TYPE_SMS_GSM, built, sizeof(built), &used);
	check(error == EINVAL, "build: empty text refused");
	error = btd_bmsg_build("1", (const uint8_t *)"a\0b", 3U, BTD_MAP_TYPE_SMS_GSM, built, sizeof(built), &used);
	check(error == EINVAL, "build: NUL refused");
	error = btd_bmsg_build("1", (const uint8_t *)"a\xff", 2U, BTD_MAP_TYPE_SMS_GSM, built, sizeof(built), &used);
	check(error == EINVAL, "build: malformed UTF-8 refused");
	error = btd_bmsg_build("1", (const uint8_t *)"x", 1U, BTD_MAP_TYPE_MMS, built, sizeof(built), &used);
	check(error == EINVAL, "build: MMS refused");
	error = btd_bmsg_build("1", (const uint8_t *)"x", 1U, BTD_MAP_TYPE_SMS_GSM, built, 100U, &used);
	check(error == ENOBUFS, "build: no room");

	/* 8192 bytes taken, all line breaks (each two bytes sent), within the build's room. */
	memset(document, '\n', BTD_BMSG_SEND_MAX + 1U);
	error = btd_bmsg_build("1", (const uint8_t *)document, BTD_BMSG_SEND_MAX, BTD_MAP_TYPE_SMS_GSM, built, sizeof(built), &used);
	check(error == 0, "build: 8192 line breaks fit");
	error = btd_bmsg_build("1", (const uint8_t *)document, BTD_BMSG_SEND_MAX + 1U, BTD_MAP_TYPE_SMS_GSM, built, sizeof(built), &used);
	check(error == EINVAL, "build: 8193 bytes refused");

	/* The worst escapes: every line END:MSG, each one more byte. */
	for (used = 0U; used + 8U <= BTD_BMSG_SEND_MAX; used += 8U)
		memcpy(document + used, "END:MSG\n", 8U);
	error = btd_bmsg_build("1", (const uint8_t *)document, used, BTD_MAP_TYPE_SMS_GSM, built, sizeof(built), &used);
	check(error == 0, "build: escapes fit");
	error = btd_bmsg_parse(built, used, &message);
	check(error == 0 && message.form == BTD_BMSG_FORM_SPEC, "build: escapes read back");
}

/* Well-formed and malformed UTF-8. */
static void
test_utf8(void)
{
	int ok;

	/* Characters of one to four bytes. */
	ok = btd_bmsg_utf8_ok((const uint8_t *)"a\xc3\xa9\xe3\x81\x82\xf0\x9f\x98\x80", 10U);
	check(ok, "utf8: well-formed");

	/* Overlong forms, a surrogate, past U+10FFFF, cut short, a stray continuation. */
	ok = btd_bmsg_utf8_ok((const uint8_t *)"\xc0\x80", 2U);
	check(!ok, "utf8: overlong two bytes");
	ok = btd_bmsg_utf8_ok((const uint8_t *)"\xe0\x80\x80", 3U);
	check(!ok, "utf8: overlong three bytes");
	ok = btd_bmsg_utf8_ok((const uint8_t *)"\xed\xa0\x80", 3U);
	check(!ok, "utf8: surrogate");
	ok = btd_bmsg_utf8_ok((const uint8_t *)"\xf4\x90\x80\x80", 4U);
	check(!ok, "utf8: past U+10FFFF");
	ok = btd_bmsg_utf8_ok((const uint8_t *)"\xe3\x81", 2U);
	check(!ok, "utf8: cut short");
	ok = btd_bmsg_utf8_ok((const uint8_t *)"\x80", 1U);
	check(!ok, "utf8: stray continuation");
	ok = btd_bmsg_utf8_ok((const uint8_t *)"\xef\xbf\xbf\xf4\x8f\xbf\xbf", 7U);
	check(ok, "utf8: U+FFFF and U+10FFFF");
}

/* Random changes to the bMessages into the reader (a fixed seed): nothing crashes under ASan and UBSan. */
static void
test_fuzz(void)
{
	static const char specials[] = "\r\n:;/ BEGINDMSGVCARDBENVBBODYLENGTH0123456789\xc3\xe3\x80";
	const char *source;
	size_t length;
	size_t changes;
	size_t change;
	size_t at;
	size_t used;
	unsigned round;
	unsigned seed;
	int error;

	/* The built bMessage, as a second source. */
	error = btd_bmsg_build("+15551234", (const uint8_t *)"Hi\nEND:MSG\n/x", 13U, BTD_MAP_TYPE_SMS_GSM, built, sizeof(built) - 1U, &used);
	check(error == 0, "fuzz: source built");
	built[used] = '\0';

	/* A fixed seed. */
	seed = 0x626d7367U;
	for (round = 0U; round < TEST_FUZZ_ROUNDS; round++) {
		/* One of the two bMessages. */
		source = received;
		if ((round & 1U) != 0U)
			source = (const char *)built;
		length = strlen(source);
		memcpy(document, source, length);

		/* A few bytes changed: to a special byte, to any byte, or the bMessage cut. */
		seed = seed * 1103515245U + 12345U;
		changes = (size_t)((seed >> 16) % 6U) + 1U;
		for (change = 0U; change < changes; change++) {
			seed = seed * 1103515245U + 12345U;
			at = (size_t)((seed >> 8) % length);
			seed = seed * 1103515245U + 12345U;
			if ((seed & 0x30000U) == 0U) {
				length = at + 1U;
			} else if ((seed & 0x40000U) != 0U) {
				document[at] = specials[(seed >> 20) % (sizeof(specials) - 1U)];
			} else {
				document[at] = (char)(seed >> 20);
			}
		}

		/* The reader on it. */
		(void)btd_bmsg_parse((const uint8_t *)document, length, &message);
	}

	/* Nothing crashed. */
	check(1, "fuzz: ran");
}
