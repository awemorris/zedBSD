/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the vCard reader of the Phone Book Access Profile
 * (ws197-p005, plan/ws197/phase005/phase.md section 4), built with the
 * host's compiler under ASan and UBSan.  Every card is written by hand,
 * and every key is computed here by the test's own FNV-1a over the text
 * the design says the key is made of.
 *
 *   next       cards cut out of a body: lines before and between them,
 *              a nested AGENT card, LF-only lines, a body cut inside a card;
 *              the cards counted at the body's level
 *   v21        a card of 2.1 as phones write it: quoted-printable names in
 *              UTF-8 with a soft line break inside a character, bare
 *              parameters, a fold that keeps its space, a BASE64 photo
 *   v30        a card of 3.0: escapes, a group, TYPE lists in quotes and
 *              in either case, a tel: URI, folds that lose their space
 *   charset    another character set left out, malformed UTF-8 and
 *              control characters made U+FFFD, a text cut at 256 bytes
 *   numbers    nine numbers (one left out), an empty one, one of 33
 *              digits, a name taken from the first number, no name at all
 *   limits     a card past 16 KB, a line past 4 KB, cards nested nine
 *              deep, cards not opened or never closed
 *   reduce     the reduced vCard byte for byte, every room too small,
 *              the reduced vCard read back to the same contact
 *   keys       the FNV-1a of the test against known values; a contact by
 *              its UID, by its name and sorted numbers
 *   calls      the history of 2.1 and 3.0: the kind by parameter or by
 *              folder, the datetime, the time in UTC and in the local zone,
 *              no datetime, a malformed one, a withheld number
 *   fuzz       random changes to the cards into every reader
 *
 *   plan/ws197/tests/bt-phone-host-test.sh
 */

#include "userland/base/bluetoothd/vcard.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The fuzz's rounds. */
#define TEST_FUZZ_ROUNDS	200000U

/* The room of the test's documents. */
#define TEST_DOCUMENT_MAX	(BTD_VCARD_CARD_MAX + 4096U)

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

/* The contacts and the call a test reads. */
static struct btd_vcard_contact contact;
static struct btd_vcard_contact again;
static struct btd_vcard_call call;

/* A document the test builds. */
static char document[TEST_DOCUMENT_MAX];

/* The reduced vCard a test writes. */
static char reduced[BTD_VCARD_REDUCED_MAX];

/*
 * A card of 2.1 as an Android phone writes it: N and FN in quoted-
 * printable UTF-8 (山田 太郎), FN's soft line break inside the last
 * character, bare parameters, a BASE64 photo with its folded lines and
 * the empty line that ends it, and a NOTE folded with its space kept.
 */
static const char card_21[] =
	"BEGIN:VCARD\r\n"
	"VERSION:2.1\r\n"
	"N;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:=E5=B1=B1=E7=94=B0;=E5=A4=AA=E9=83=8E;;;\r\n"
	"FN;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:=E5=B1=B1=E7=94=B0=20=E5=A4=AA=E9=83=\r\n"
	"=8E\r\n"
	"TEL;CELL:090-1234-5678\r\n"
	"TEL;HOME;PREF:+81 3 1234 5678\r\n"
	"PHOTO;ENCODING=BASE64;JPEG:/9j/4AAQSkZJRgABAQ\r\n"
	" AAAQABAAD/2wBDAAgGBgcGBQgHBwcJ\r\n"
	"\r\n"
	"END:VCARD\r\n";

/*
 * A card of 3.0: an escaped ',' in FN, an escaped ';' inside N's
 * additional names, a group on a TEL with a TYPE list in lower case, a
 * quoted TYPE and a tel: URI with a parameter, a UID folded (its space
 * lost), and a BASE64 photo folded.
 */
static const char card_30[] =
	"BEGIN:VCARD\r\n"
	"VERSION:3.0\r\n"
	"FN:Doe\\, John\r\n"
	"N:Doe;John;Q.\\;X;Dr.;Jr.\r\n"
	"item1.TEL;TYPE=cell,voice:+1 (555) 123-4567\r\n"
	"TEL;TYPE=\"work\":tel:+15550001;ext=12\r\n"
	"UID:abc-\r\n"
	" 123\r\n"
	"PHOTO;ENCODING=b;TYPE=JPEG:QUJD\r\n"
	" REVG\r\n"
	"END:VCARD\r\n";

/* The reduced vCard of card_30, written by hand. */
static const char card_30_reduced[] =
	"BEGIN:VCARD\r\n"
	"VERSION:3.0\r\n"
	"FN:Doe\\, John\r\n"
	"N:Doe;John;Q.\\;X;Dr.;Jr.\r\n"
	"TEL;TYPE=CELL,VOICE:+15551234567\r\n"
	"TEL;TYPE=WORK:+15550001\r\n"
	"UID:abc-123\r\n"
	"END:VCARD\r\n";

/* A missed call of 2.1, its kind a bare parameter, with no name. */
static const char call_21[] =
	"BEGIN:VCARD\r\n"
	"VERSION:2.1\r\n"
	"N:;;;;\r\n"
	"FN:\r\n"
	"TEL:+15551234\r\n"
	"X-IRMC-CALL-DATETIME;MISSED:20240101T120000\r\n"
	"END:VCARD\r\n";

/* A call made of 3.0, its kind in TYPE, its time in UTC, a name only in N. */
static const char call_30[] =
	"BEGIN:VCARD\r\n"
	"VERSION:3.0\r\n"
	"N:Doe;Jane;;;\r\n"
	"TEL;TYPE=CELL:+1-555-9876\r\n"
	"X-IRMC-CALL-DATETIME;TYPE=DIALED:20240101T120000Z\r\n"
	"END:VCARD\r\n";

static void check(int condition, const char *what);
static int read_contact(const char *text);
static int read_call(const char *text, int folder_kind);
static uint64_t test_fnv(const char *text);
static void test_key(const char *text, char *key);
static int utf8_ok(const char *text);
static int contact_ok(const struct btd_vcard_contact *read);
static void test_next(void);
static void test_v21(void);
static void test_v30(void);
static void test_charset(void);
static void test_numbers(void);
static void test_limits(void);
static void test_reduce(void);
static void test_keys(void);
static void test_calls(void);
static void test_fuzz(void);

/*
 * Runs every part and reports the checks.
 */
int
main(void)
{
	/* Each part. */
	test_next();
	test_v21();
	test_v30();
	test_charset();
	test_numbers();
	test_limits();
	test_reduce();
	test_keys();
	test_calls();
	test_fuzz();

	/* The count of what failed. */
	printf("bt-vcard-host-test: %u checks, %u failed\n", checks, failures);
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

/* Reads a card given as a string into the test's contact. */
static int
read_contact(
	const char *text)
{
	int error;

	/* The card without its NUL. */
	error = btd_vcard_contact_read((const uint8_t *)text, strlen(text), &contact);

	/* What the reader said. */
	return error;
}

/* Reads a card of the history given as a string into the test's call. */
static int
read_call(
	const char *text,
	int folder_kind)
{
	int error;

	/* The card without its NUL. */
	error = btd_vcard_call_read((const uint8_t *)text, strlen(text), folder_kind, &call);

	/* What the reader said. */
	return error;
}

/* Computes the 64-bit FNV-1a of a string, written here apart from the reader's. */
static uint64_t
test_fnv(
	const char *text)
{
	uint64_t hash;
	size_t index;

	/* The basis, then each byte. */
	hash = 14695981039346656037ULL;
	for (index = 0U; text[index] != '\0'; index++) {
		hash ^= (uint8_t)text[index];
		hash *= 1099511628211ULL;
	}

	/* The hash. */
	return hash;
}

/* Writes the key of a string: its FNV-1a as 16 lower-case hexadecimal digits. */
static void
test_key(
	const char *text,
	char *key)
{
	unsigned long long hash;

	/* The hash in hexadecimal. */
	hash = (unsigned long long)test_fnv(text);
	snprintf(key, BTD_VCARD_KEY_SIZE, "%016llx", hash);
}

/* Says whether a string is well-formed UTF-8 without control characters. */
static int
utf8_ok(
	const char *text)
{
	const uint8_t *bytes;
	size_t at;
	size_t length;
	size_t index;

	/* Each character. */
	bytes = (const uint8_t *)text;
	at = 0U;
	while (bytes[at] != '\0') {
		/* A byte of ASCII, not a control character. */
		if (bytes[at] < 0x80U) {
			if (bytes[at] < 0x20U || bytes[at] == 0x7fU)
				return 0;
			at++;
			continue;
		}

		/* The length the first byte gives. */
		length = 0U;
		if (bytes[at] >= 0xc2U && bytes[at] <= 0xdfU) {
			length = 2U;
		} else if (bytes[at] >= 0xe0U && bytes[at] <= 0xefU) {
			length = 3U;
		} else if (bytes[at] >= 0xf0U && bytes[at] <= 0xf4U) {
			length = 4U;
		}

		/* A byte that starts no character. */
		if (length == 0U)
			return 0;

		/* The continuation bytes. */
		for (index = 1U; index < length; index++) {
			/* A byte that does not continue it. */
			if (bytes[at + index] < 0x80U || bytes[at + index] > 0xbfU)
				return 0;
		}

		/* The next character. */
		at += length;
	}

	/* Every character is well-formed. */
	return 1;
}

/*
 * Says whether a contact read holds what the reader promises: a name,
 * texts in well-formed UTF-8, numbers of digits and + * #, a key of 16
 * hexadecimal digits.
 */
static int
contact_ok(
	const struct btd_vcard_contact *read)
{
	size_t index;
	size_t at;
	size_t length;
	const char *found;
	int ok;

	/* A name to show. */
	if (read->name[0] == '\0')
		return 0;

	/* Every text well-formed. */
	ok = utf8_ok(read->name);
	if (!ok)
		return 0;
	ok = utf8_ok(read->formatted);
	if (!ok)
		return 0;
	ok = utf8_ok(read->uid);
	if (!ok)
		return 0;
	for (index = 0U; index < BTD_VCARD_N_PARTS; index++) {
		/* A part of N. */
		ok = utf8_ok(read->n[index]);
		if (!ok)
			return 0;
	}

	/* Every number of digits and signs. */
	if (read->tel_count > BTD_VCARD_TELS_MAX)
		return 0;
	for (index = 0U; index < read->tel_count; index++) {
		/* An empty number. */
		if (read->tels[index].number[0] == '\0')
			return 0;

		/* A byte that is not a number's. */
		for (at = 0U; read->tels[index].number[at] != '\0'; at++) {
			/* A digit, +, * or #. */
			found = strchr("0123456789+*#", read->tels[index].number[at]);
			if (found == NULL)
				return 0;
		}
	}

	/* A key of 16 hexadecimal digits. */
	length = strlen(read->key);
	if (length != 16U)
		return 0;
	length = strspn(read->key, "0123456789abcdef");
	if (length != 16U)
		return 0;

	/* Everything holds. */
	return 1;
}

/* Cards cut out of a body. */
static void
test_next(void)
{
	static const char body[] =
		"junk before\r\n"
		"BEGIN:VCARD\r\nVERSION:3.0\r\nFN:A\r\nEND:VCARD\r\n"
		"\r\n"
		"begin:vcard\nVERSION:2.1\nFN:B\nAGENT:\nBEGIN:VCARD\nFN:Inner\nEND:VCARD\nEND:VCARD\n"
		"BEGIN:VCARD\r\nFN:C\r\n";
	size_t at;
	size_t start;
	size_t length;
	unsigned count;
	int error;

	/* The first card, after the line before it. */
	at = 0U;
	error = btd_vcard_next((const uint8_t *)body, strlen(body), &at, &start, &length);
	check(error == 0, "next: first card");
	check(start == 13U, "next: first card starts after the junk");
	check(length == 43U, "next: first card's length with its CRLF");
	check(at == 56U, "next: past the first card");

	/* The second, in LF lines and lower case, the nested card inside it. */
	error = btd_vcard_next((const uint8_t *)body, strlen(body), &at, &start, &length);
	check(error == 0, "next: second card");
	check(start == 58U, "next: second card after the empty line");
	check(length == 77U, "next: second card holds the nested one");
	error = read_contact(body + start);
	check(error == 0, "next: second card reads");
	check(strcmp(contact.name, "B") == 0, "next: the nested card's FN is not the card's");

	/* The third, cut by the body's end. */
	error = btd_vcard_next((const uint8_t *)body, strlen(body), &at, &start, &length);
	check(error == EINVAL, "next: a card the body cuts");
	check(at == strlen(body), "next: at the end after a cut card");

	/* Nothing after the end. */
	error = btd_vcard_next((const uint8_t *)body, strlen(body), &at, &start, &length);
	check(error == ENOENT, "next: no card left");

	/* An empty body. */
	at = 0U;
	error = btd_vcard_next((const uint8_t *)"", 0U, &at, &start, &length);
	check(error == ENOENT, "next: an empty body");

	/* The cards counted at the body's level: the nested one not, the cut one too (ws197-p005 section 5.3). */
	count = btd_vcard_count((const uint8_t *)body, strlen(body));
	check(count == 3U, "next: three cards counted");
	count = btd_vcard_count((const uint8_t *)"END:VCARD\r\nx\r\n", 14U);
	check(count == 0U, "next: no card counted");
}

/* A card of 2.1 as phones write it. */
static void
test_v21(void)
{
	int error;

	/* The card reads. */
	error = read_contact(card_21);
	check(error == 0, "v21: reads");
	check(contact.version == BTD_VCARD_VERSION_21, "v21: version");

	/* The quoted-printable names, the soft line break inside a character joined. */
	check(strcmp(contact.formatted, "\xe5\xb1\xb1\xe7\x94\xb0 \xe5\xa4\xaa\xe9\x83\x8e") == 0, "v21: FN");
	check(strcmp(contact.name, contact.formatted) == 0, "v21: name is FN");
	check(contact.has_n, "v21: has N");
	check(strcmp(contact.n[0], "\xe5\xb1\xb1\xe7\x94\xb0") == 0, "v21: family name");
	check(strcmp(contact.n[1], "\xe5\xa4\xaa\xe9\x83\x8e") == 0, "v21: given name");
	check(contact.n[2][0] == '\0', "v21: no additional names");

	/* The numbers with their bare types; the photo left out. */
	check(contact.tel_count == 2U, "v21: two numbers");
	check(strcmp(contact.tels[0].number, "09012345678") == 0, "v21: cell number");
	check(contact.tels[0].types == BTD_VCARD_TEL_CELL, "v21: cell type");
	check(strcmp(contact.tels[1].number, "+81312345678") == 0, "v21: home number");
	check(contact.tels[1].types == (BTD_VCARD_TEL_HOME | BTD_VCARD_TEL_PREF), "v21: home and pref");
	check(contact.tels_dropped == 0U, "v21: nothing left out");

	/* A fold of 2.1 keeps its space. */
	error = read_contact("BEGIN:VCARD\r\nVERSION:2.1\r\nFN:Long\r\n Name\r\nEND:VCARD\r\n");
	check(error == 0, "v21: folded FN reads");
	check(strcmp(contact.name, "Long Name") == 0, "v21: fold keeps the space");

	/* An '=' at a line's end is a soft line break only in quoted-printable. */
	error = read_contact("BEGIN:VCARD\r\nVERSION:2.1\r\nFN:a=\r\nTEL:1\r\nEND:VCARD\r\n");
	check(error == 0, "v21: '=' outside quoted-printable reads");
	check(strcmp(contact.name, "a=") == 0, "v21: '=' kept");
	check(contact.tel_count == 1U, "v21: the line after is its own");

	/* A bare QUOTED-PRINTABLE parameter, a quoted-printable '=' not a byte, a bare CHARSET of UTF-8. */
	error = read_contact("BEGIN:VCARD\r\nVERSION:2.1\r\nFN;QUOTED-PRINTABLE;CHARSET=utf8:A=3DB=ZZ=4\r\nEND:VCARD\r\n");
	check(error == 0, "v21: bare quoted-printable reads");
	check(strcmp(contact.name, "A=B=ZZ=4") == 0, "v21: '=3D' undone, malformed '=' kept");

	/* LF-only lines and names in lower case. */
	error = read_contact("begin:vcard\nversion:2.1\nfn:Lower\ntel;cell:5\nend:vcard\n");
	check(error == 0, "v21: lower case reads");
	check(strcmp(contact.name, "Lower") == 0, "v21: lower case FN");
	check(contact.tels[0].types == BTD_VCARD_TEL_CELL, "v21: lower case type");
}

/* A card of 3.0. */
static void
test_v30(void)
{
	int error;

	/* The card reads. */
	error = read_contact(card_30);
	check(error == 0, "v30: reads");
	check(contact.version == BTD_VCARD_VERSION_30, "v30: version");

	/* The escapes undone. */
	check(strcmp(contact.name, "Doe, John") == 0, "v30: FN unescaped");
	check(strcmp(contact.n[0], "Doe") == 0, "v30: family name");
	check(strcmp(contact.n[1], "John") == 0, "v30: given name");
	check(strcmp(contact.n[2], "Q.;X") == 0, "v30: an escaped ';' inside a part");
	check(strcmp(contact.n[3], "Dr.") == 0, "v30: prefix");
	check(strcmp(contact.n[4], "Jr.") == 0, "v30: suffix");

	/* The numbers: the group left out, TYPE lists, the URI's scheme and parameter left out. */
	check(contact.tel_count == 2U, "v30: two numbers");
	check(strcmp(contact.tels[0].number, "+15551234567") == 0, "v30: first number");
	check(contact.tels[0].types == (BTD_VCARD_TEL_CELL | BTD_VCARD_TEL_VOICE), "v30: cell and voice");
	check(strcmp(contact.tels[1].number, "+15550001") == 0, "v30: tel: URI");
	check(contact.tels[1].types == BTD_VCARD_TEL_WORK, "v30: quoted type");

	/* The UID folded without its space. */
	check(strcmp(contact.uid, "abc-123") == 0, "v30: folded UID");

	/* A fold of 3.0 loses its space; a TAB folds too. */
	error = read_contact("BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Long\r\n Name\r\n\tX\r\nEND:VCARD\r\n");
	check(error == 0, "v30: folded FN reads");
	check(strcmp(contact.name, "LongNameX") == 0, "v30: fold loses the space");

	/* \n inside a text is a space; an unknown escape keeps its backslash. */
	error = read_contact("BEGIN:VCARD\r\nVERSION:3.0\r\nFN:a\\nb\\Nc\\q\r\nEND:VCARD\r\n");
	check(error == 0, "v30: escapes read");
	check(strcmp(contact.name, "a b c\\q") == 0, "v30: line breaks become spaces");

	/* With no FN, N makes the name, given before family. */
	error = read_contact("BEGIN:VCARD\r\nVERSION:3.0\r\nN:Doe;Jane;;;\r\nEND:VCARD\r\n");
	check(error == 0, "v30: N only reads");
	check(strcmp(contact.name, "Jane Doe") == 0, "v30: name from N");
	check(contact.formatted[0] == '\0', "v30: no FN");
	error = read_contact("BEGIN:VCARD\r\nVERSION:3.0\r\nN:Doe\r\nEND:VCARD\r\n");
	check(error == 0, "v30: family name only reads");
	check(strcmp(contact.name, "Doe") == 0, "v30: name from the family name alone");

	/* The first FN that says something counts. */
	error = read_contact("BEGIN:VCARD\r\nVERSION:3.0\r\nFN:\r\nFN:Second\r\nFN:Third\r\nEND:VCARD\r\n");
	check(error == 0, "v30: several FN read");
	check(strcmp(contact.name, "Second") == 0, "v30: first FN with a name");
}

/* Character sets and malformed texts. */
static void
test_charset(void)
{
	size_t index;
	size_t used;
	int error;

	/* An FN in another character set is left out; N makes the name. */
	error = read_contact("BEGIN:VCARD\r\nVERSION:2.1\r\nFN;CHARSET=ISO-8859-1:Jos\xe9\r\nN:Doe;Jose\r\nEND:VCARD\r\n");
	check(error == 0, "charset: another character set reads");
	check(contact.formatted[0] == '\0', "charset: FN in ISO-8859-1 left out");
	check(strcmp(contact.name, "Jose Doe") == 0, "charset: name from N");

	/* An encoding not known is left out. */
	error = read_contact("BEGIN:VCARD\r\nVERSION:2.1\r\nFN;ENCODING=X-NEW:abc\r\nTEL:1\r\nEND:VCARD\r\n");
	check(error == 0, "charset: unknown encoding reads");
	check(strcmp(contact.name, "1") == 0, "charset: FN of an unknown encoding left out");

	/* A malformed sequence, a lone continuation byte, a control character, NUL by quoted-printable. */
	error = read_contact("BEGIN:VCARD\r\nVERSION:2.1\r\nFN;ENCODING=QUOTED-PRINTABLE:a=C3b=80c=01d=00e=ED=A0=80\r\nEND:VCARD\r\n");
	check(error == 0, "charset: malformed reads");
	check(strcmp(contact.name, "a\xef\xbf\xbd" "b\xef\xbf\xbd" "c\xef\xbf\xbd" "d\xef\xbf\xbd" "e\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd") == 0, "charset: U+FFFD for each");

	/* A TAB is a space, and spaces around a text go. */
	error = read_contact("BEGIN:VCARD\r\nVERSION:3.0\r\nFN:  a\tb  \r\nEND:VCARD\r\n");
	check(error == 0, "charset: spaces read");
	check(strcmp(contact.name, "a b") == 0, "charset: spaces trimmed, TAB a space");

	/* 100 characters of three bytes cut at 255 bytes, no character split. */
	used = (size_t)snprintf(document, sizeof(document), "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:");
	for (index = 0U; index < 100U; index++) {
		/* One "あ". */
		memcpy(document + used, "\xe3\x81\x82", 3U);
		used += 3U;
	}

	/* The card closes, and is read. */
	snprintf(document + used, sizeof(document) - used, "\r\nEND:VCARD\r\n");
	error = read_contact(document);
	check(error == 0, "charset: long FN reads");
	check(strlen(contact.name) == 255U, "charset: cut at 255 bytes");
	check(utf8_ok(contact.name), "charset: cut whole");
}

/* Numbers kept, left out, and a name taken from one. */
static void
test_numbers(void)
{
	size_t index;
	size_t used;
	int error;

	/* Nine numbers: eight kept, one left out. */
	used = (size_t)snprintf(document, sizeof(document), "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Many\r\n");
	for (index = 0U; index < 9U; index++)
		used += (size_t)snprintf(document + used, sizeof(document) - used, "TEL:%u\r\n", (unsigned)index + 1U);
	snprintf(document + used, sizeof(document) - used, "END:VCARD\r\n");
	error = read_contact(document);
	check(error == 0, "numbers: nine read");
	check(contact.tel_count == 8U, "numbers: eight kept");
	check(contact.tels_dropped == 1U, "numbers: one left out");
	check(strcmp(contact.tels[7].number, "8") == 0, "numbers: the eighth kept");

	/* An empty number and one of 33 digits are left out. */
	error = read_contact("BEGIN:VCARD\r\nVERSION:3.0\r\nFN:X\r\nTEL:\r\nTEL:abc\r\n"
			     "TEL:123456789012345678901234567890123\r\nTEL:12345678901234567890123456789012\r\nEND:VCARD\r\n");
	check(error == 0, "numbers: odd numbers read");
	check(contact.tel_count == 1U, "numbers: only 32 digits kept");
	check(contact.tels_dropped == 3U, "numbers: empty, letters and 33 digits left out");
	check(strlen(contact.tels[0].number) == 32U, "numbers: 32 digits");

	/* A number withheld in a contact's place of the name. */
	error = read_contact("BEGIN:VCARD\r\nVERSION:3.0\r\nTEL;TYPE=CELL:*31#+1 555\r\nEND:VCARD\r\n");
	check(error == 0, "numbers: number only reads");
	check(strcmp(contact.name, "*31#+1555") == 0, "numbers: the number stands for the name");

	/* A card with neither a name nor a number. */
	error = read_contact("BEGIN:VCARD\r\nVERSION:3.0\r\nFN:\r\nNOTE:x\r\nEND:VCARD\r\n");
	check(error == ENOENT, "numbers: nothing to keep");
}

/* The limits and the malformed cards. */
static void
test_limits(void)
{
	size_t used;
	int error;
	int depth;

	/* A card past 16 KB. */
	used = (size_t)snprintf(document, sizeof(document), "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Big\r\nNOTE:");
	memset(document + used, 'x', BTD_VCARD_CARD_MAX);
	used += BTD_VCARD_CARD_MAX;
	snprintf(document + used, sizeof(document) - used, "\r\nEND:VCARD\r\n");
	error = read_contact(document);
	check(error == E2BIG, "limits: past 16 KB");
	error = btd_vcard_call_read((const uint8_t *)document, strlen(document), BTD_VCARD_CALL_RECEIVED, &call);
	check(error == E2BIG, "limits: a call past 16 KB");

	/* A line past 4 KB: its number left out, the rest read. */
	used = (size_t)snprintf(document, sizeof(document), "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Wide\r\nTEL:");
	memset(document + used, '1', BTD_VCARD_LINE_MAX);
	used += BTD_VCARD_LINE_MAX;
	snprintf(document + used, sizeof(document) - used, "\r\nTEL:2\r\nEND:VCARD\r\n");
	error = read_contact(document);
	check(error == 0, "limits: a long line reads");
	check(contact.tels_dropped == 1U, "limits: the long number left out");
	check(contact.tel_count == 1U, "limits: the next number kept");

	/* Eight cards deep is taken, nine is not. */
	used = (size_t)snprintf(document, sizeof(document), "BEGIN:VCARD\r\nFN:Deep\r\n");
	for (depth = 1; depth < 8; depth++)
		used += (size_t)snprintf(document + used, sizeof(document) - used, "BEGIN:VCARD\r\n");
	for (depth = 1; depth < 8; depth++)
		used += (size_t)snprintf(document + used, sizeof(document) - used, "END:VCARD\r\n");
	snprintf(document + used, sizeof(document) - used, "END:VCARD\r\n");
	error = read_contact(document);
	check(error == 0, "limits: eight deep");
	used = (size_t)snprintf(document, sizeof(document), "BEGIN:VCARD\r\nFN:Deep\r\n");
	for (depth = 1; depth < 9; depth++)
		used += (size_t)snprintf(document + used, sizeof(document) - used, "BEGIN:VCARD\r\n");
	for (depth = 1; depth < 9; depth++)
		used += (size_t)snprintf(document + used, sizeof(document) - used, "END:VCARD\r\n");
	snprintf(document + used, sizeof(document) - used, "END:VCARD\r\n");
	error = read_contact(document);
	check(error == EINVAL, "limits: nine deep");

	/* Not opened, never closed, empty, a value of BEGIN not VCARD. */
	error = read_contact("FN:A\r\nBEGIN:VCARD\r\nFN:B\r\nEND:VCARD\r\n");
	check(error == EINVAL, "limits: not opened first");
	error = read_contact("BEGIN:VCARD\r\nFN:B\r\n");
	check(error == EINVAL, "limits: never closed");
	check(contact.name[0] == '\0', "limits: nothing kept of a malformed card");
	error = read_contact("");
	check(error == EINVAL, "limits: empty");
	error = read_contact("BEGIN:VCALENDAR\r\nEND:VCALENDAR\r\n");
	check(error == EINVAL, "limits: not a card");
	error = read_call("BEGIN:VCARD\r\nFN:B\r\n", BTD_VCARD_CALL_RECEIVED);
	check(error == EINVAL, "limits: a call never closed");

	/* Lines after the card closes are not read. */
	error = read_contact("BEGIN:VCARD\r\nFN:A\r\nEND:VCARD\r\nFN:B\r\n");
	check(error == 0, "limits: lines after the card");
	check(strcmp(contact.name, "A") == 0, "limits: the card's own FN");
}

/* The reduced vCard. */
static void
test_reduce(void)
{
	size_t used;
	size_t expected;
	size_t room;
	int error;
	int all_short;

	/* card_30, byte for byte. */
	error = read_contact(card_30);
	check(error == 0, "reduce: card reads");
	error = btd_vcard_reduce(&contact, reduced, sizeof(reduced), &used);
	check(error == 0, "reduce: written");
	expected = strlen(card_30_reduced);
	check(used == expected, "reduce: length");
	check(memcmp(reduced, card_30_reduced, expected) == 0, "reduce: bytes");
	check(reduced[used] == '\0', "reduce: NUL after it");

	/* Every room too small, the NUL's included, is refused. */
	all_short = 1;
	for (room = 0U; room <= expected; room++) {
		/* A room one byte or more short. */
		error = btd_vcard_reduce(&contact, reduced, room, &used);
		if (error != ENOBUFS)
			all_short = 0;
	}

	/* None was written. */
	check(all_short, "reduce: rooms too small refused");

	/* Read back, the same contact. */
	error = btd_vcard_reduce(&contact, reduced, sizeof(reduced), &used);
	check(error == 0, "reduce: written again");
	error = btd_vcard_contact_read((const uint8_t *)reduced, used, &again);
	check(error == 0, "reduce: reads back");
	check(strcmp(again.name, contact.name) == 0, "reduce: same name");
	check(strcmp(again.n[2], contact.n[2]) == 0, "reduce: same parts");
	check(again.tel_count == contact.tel_count, "reduce: same numbers");
	check(again.tels[0].types == contact.tels[0].types, "reduce: same types");
	check(strcmp(again.key, contact.key) == 0, "reduce: same key");

	/* card_21 (no UID, no N's other parts), and a number-only card. */
	error = read_contact(card_21);
	check(error == 0, "reduce: 2.1 reads");
	error = btd_vcard_reduce(&contact, reduced, sizeof(reduced), &used);
	check(error == 0, "reduce: 2.1 written");
	check(strstr(reduced, "TEL;TYPE=HOME,PREF:+81312345678\r\n") != NULL, "reduce: two types");
	check(strstr(reduced, "UID") == NULL, "reduce: no UID");
	error = btd_vcard_contact_read((const uint8_t *)reduced, used, &again);
	check(error == 0, "reduce: 2.1 reads back");
	check(strcmp(again.key, contact.key) == 0, "reduce: 2.1 same key");
	error = read_contact("BEGIN:VCARD\r\nVERSION:2.1\r\nTEL:555\r\nEND:VCARD\r\n");
	check(error == 0, "reduce: number only reads");
	error = btd_vcard_reduce(&contact, reduced, sizeof(reduced), &used);
	check(error == 0, "reduce: number only written");
	check(strcmp(reduced, "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:555\r\nTEL:555\r\nEND:VCARD\r\n") == 0, "reduce: number only bytes");
	error = btd_vcard_contact_read((const uint8_t *)reduced, used, &again);
	check(error == 0, "reduce: number only reads back");
	check(strcmp(again.key, contact.key) == 0, "reduce: number only same key");
}

/* The keys. */
static void
test_keys(void)
{
	char key[BTD_VCARD_KEY_SIZE];
	int error;

	/* The test's FNV-1a against known values. */
	check(test_fnv("") == 0xcbf29ce484222325ULL, "keys: FNV-1a of nothing");
	check(test_fnv("a") == 0xaf63dc4c8601ec8cULL, "keys: FNV-1a of a");

	/* A contact by its UID. */
	error = read_contact(card_30);
	check(error == 0, "keys: card_30 reads");
	test_key("u|abc-123", key);
	check(strcmp(contact.key, key) == 0, "keys: by UID");

	/* A contact by its name and its numbers sorted. */
	error = read_contact("BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Ann\r\nTEL:222\r\nTEL:111\r\nTEL:3\r\nEND:VCARD\r\n");
	check(error == 0, "keys: no UID reads");
	test_key("n|Ann|111,222,3", key);
	check(strcmp(contact.key, key) == 0, "keys: by name and sorted numbers");

	/* The same contact with its numbers in another order has the same key. */
	error = read_contact("BEGIN:VCARD\r\nVERSION:2.1\r\nFN:Ann\r\nTEL:3\r\nTEL:222\r\nTEL:111\r\nEND:VCARD\r\n");
	check(error == 0, "keys: reordered reads");
	check(strcmp(contact.key, key) == 0, "keys: order does not matter");

	/* A contact with no numbers. */
	error = read_contact("BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Solo\r\nEND:VCARD\r\n");
	check(error == 0, "keys: no numbers reads");
	test_key("n|Solo|", key);
	check(strcmp(contact.key, key) == 0, "keys: no numbers");
}

/* The cards of the call history. */
static void
test_calls(void)
{
	char key[BTD_VCARD_KEY_SIZE];
	int64_t seconds;
	int zone;
	int error;

	/* A missed call of 2.1: the parameter over the folder, no name. */
	error = read_call(call_21, BTD_VCARD_CALL_RECEIVED);
	check(error == 0, "calls: 2.1 reads");
	check(call.version == BTD_VCARD_VERSION_21, "calls: 2.1 version");
	check(call.kind == BTD_VCARD_CALL_MISSED, "calls: kind by parameter");
	check(strcmp(call.datetime, "20240101T120000") == 0, "calls: datetime");
	check(strcmp(call.number, "+15551234") == 0, "calls: number");
	check(call.name[0] == '\0', "calls: no name");
	test_key("m|20240101T120000|+15551234", key);
	check(strcmp(call.key, key) == 0, "calls: key");

	/* Its time in zedBSD's zone, nine hours east of UTC. */
	error = btd_vcard_call_time(&call, 32400, &seconds, &zone);
	check(error == 0, "calls: local time read");
	check(seconds == 1704078000LL, "calls: local seconds");
	check(zone == BTD_VCARD_ZONE_LOCAL, "calls: local zone");

	/* A call made of 3.0, in UTC, a name from N. */
	error = read_call(call_30, BTD_VCARD_CALL_UNKNOWN);
	check(error == 0, "calls: 3.0 reads");
	check(call.kind == BTD_VCARD_CALL_DIALED, "calls: kind by TYPE");
	check(strcmp(call.name, "Jane Doe") == 0, "calls: name from N");
	check(strcmp(call.number, "+15559876") == 0, "calls: number of 3.0");
	error = btd_vcard_call_time(&call, 32400, &seconds, &zone);
	check(error == 0, "calls: UTC read");
	check(seconds == 1704110400LL, "calls: UTC seconds");
	check(zone == BTD_VCARD_ZONE_PHONE, "calls: the zone the phone gave");

	/* The kind by the folder; no datetime; a withheld number. */
	error = read_call("BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Who\r\nEND:VCARD\r\n", BTD_VCARD_CALL_RECEIVED);
	check(error == 0, "calls: bare call reads");
	check(call.kind == BTD_VCARD_CALL_RECEIVED, "calls: kind by folder");
	check(call.datetime[0] == '\0', "calls: no datetime");
	check(call.number[0] == '\0', "calls: withheld number");
	test_key("r||", key);
	check(strcmp(call.key, key) == 0, "calls: key without datetime or number");
	error = btd_vcard_call_time(&call, 0, &seconds, &zone);
	check(error == ENOENT, "calls: no time");
	check(zone == BTD_VCARD_ZONE_NONE, "calls: zone none");
	check(seconds == 0, "calls: seconds 0");

	/* A datetime not readable, and one with bytes no datetime has. */
	error = read_call("BEGIN:VCARD\r\nX-IRMC-CALL-DATETIME;RECEIVED:2024-01-01\r\nEND:VCARD\r\n", BTD_VCARD_CALL_UNKNOWN);
	check(error == 0, "calls: odd datetime reads");
	check(call.kind == BTD_VCARD_CALL_RECEIVED, "calls: bare RECEIVED");
	error = btd_vcard_call_time(&call, 0, &seconds, &zone);
	check(error == EINVAL, "calls: datetime not readable");
	check(zone == BTD_VCARD_ZONE_NONE, "calls: zone none when not readable");
	error = read_call("BEGIN:VCARD\r\nX-IRMC-CALL-DATETIME:2024 01 01\"x\r\nEND:VCARD\r\n", BTD_VCARD_CALL_DIALED);
	check(error == 0, "calls: datetime with odd bytes reads");
	check(call.datetime[0] == '\0', "calls: odd bytes leave it empty");
	check(call.kind == BTD_VCARD_CALL_DIALED, "calls: the folder's kind stays");

	/* A folder kind out of range is not taken. */
	error = read_call("BEGIN:VCARD\r\nEND:VCARD\r\n", 9);
	check(error == 0, "calls: empty call reads");
	check(call.kind == BTD_VCARD_CALL_UNKNOWN, "calls: unknown kind");
}

/* Random changes to the cards into every reader (a fixed seed): nothing crashes, and what reads holds. */
static void
test_fuzz(void)
{
	static const char specials[] = "\r\n:;=,.\\\" \tBEGINDVCARFTLXQPUO0123456789\xc3\xe3\x80\xbf";
	static const char *const sources[] = { card_21, card_30, call_21, call_30, card_30_reduced };
	size_t length;
	size_t changes;
	size_t change;
	size_t at;
	size_t start;
	size_t card_length;
	size_t used;
	unsigned round;
	unsigned seed;
	unsigned held;
	unsigned read_back;
	int64_t seconds;
	int zone;
	int error;
	int ok;
	int same_name;
	int same_key;

	/* A fixed seed. */
	seed = 0x76636172U;
	held = 0U;
	read_back = 0U;
	for (round = 0U; round < TEST_FUZZ_ROUNDS; round++) {
		/* One of the cards. */
		length = strlen(sources[round % 5U]);
		memcpy(document, sources[round % 5U], length);

		/* A few bytes changed: to a special byte, to any byte, or the card cut. */
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

		/* The cutter on it. */
		at = 0U;
		while (at < length) {
			/* The next card, until none is left. */
			error = btd_vcard_next((const uint8_t *)document, length, &at, &start, &card_length);
			if (error != 0)
				break;
			if (start + card_length > length)
				held++;
		}

		/* The call reader and its time. */
		(void)btd_vcard_call_read((const uint8_t *)document, length, BTD_VCARD_CALL_MISSED, &call);
		(void)btd_vcard_call_time(&call, 3600, &seconds, &zone);
		ok = utf8_ok(call.name);
		if (!ok)
			held++;

		/* The contact reader: what reads holds, and reads back the same from its reduced vCard. */
		error = btd_vcard_contact_read((const uint8_t *)document, length, &contact);
		if (error != 0)
			continue;
		ok = contact_ok(&contact);
		if (!ok)
			held++;
		error = btd_vcard_reduce(&contact, reduced, sizeof(reduced), &used);
		if (error != 0) {
			held++;
			continue;
		}

		/* The reduced vCard reads back to the same name, key and numbers. */
		error = btd_vcard_contact_read((const uint8_t *)reduced, used, &again);
		if (error != 0) {
			held++;
			continue;
		}

		/* The name, the key and the count of numbers compared. */
		same_name = strcmp(again.name, contact.name);
		same_key = strcmp(again.key, contact.key);
		if (same_name != 0 || same_key != 0 || again.tel_count != contact.tel_count) {
			held++;
			continue;
		}

		/* One more read back. */
		read_back++;
	}

	/* Nothing crashed, and nothing broke what the reader promises. */
	check(held == 0U, "fuzz: every card read holds and reads back");
	check(read_back > 1000U, "fuzz: many cards read back");
}
