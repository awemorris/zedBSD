/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Independent MIME fixtures cover extraction through the real bMessage reader. */

#include "userland/base/bluetoothd/bmsg.h"
#include "userland/base/bluetoothd/mms.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This process owns its fixtures, result and failure counter. */
static struct btd_bmsg message;

/* One complete bounded MAP document avoids test-only production switches. */
static char document[BTD_BMSG_INPUT_MAX];

/* Counts failed independent expectations for the runner's exit status. */
static unsigned failures;

static void check(int okay, const char *name);
static int parse(const char *body);
static void test_mime(void);
static void test_bounds(void);

/*
 * Exercises text extraction and rejects raw media or malformed MIME.
 */
int
main(void)
{
	/* Runs the normal parser against MIME bodies with known decoded bytes. */
	test_mime();
	test_bounds();
	if (failures != 0U)
		return 1;

	/* Succeeded: all independent expectations held. */
	puts("bt-mms-host-test: PASS");
	return 0;
}

/* Records a failing expectation without printing actual message contents. */
static void
check(
	int okay,
	const char *name)
{
	/* Makes failed scenarios visible and affects the process outcome. */
	if (!okay) {
		fprintf(stderr, "FAIL %s\n", name);
		failures++;
	}
}

/* Wraps a MIME fixture in a complete incoming MAP MMS. */
static int
parse(
	const char *body)
{
	size_t length;
	int written;
	int error;

	/* MIME decoding is exercised before the bMessage text size clamp. */
	written = snprintf(document, sizeof(document), "BEGIN:BMSG\r\nVERSION:1.0\r\nSTATUS:UNREAD\r\nTYPE:MMS\r\nBEGIN:BENV\r\nBEGIN:BBODY\r\nENCODING:8BIT\r\nBEGIN:MSG\r\n%s\r\nEND:MSG\r\nEND:BBODY\r\nEND:BENV\r\nEND:BMSG\r\n", body);
	if (written < 0 || (size_t)written >= sizeof(document))
		return EOVERFLOW;
	length = (size_t)written;
	error = btd_bmsg_parse((const uint8_t *)document, length, &message);
	if (error != 0)
		return error;

	/* Succeeded: the actual bMessage parser selected its text. */
	return 0;
}

/* Verifies transfer encodings, multipart selection and incomplete-message rejection. */
static void
test_mime(void)
{
	static const char mixed[] =
	    "Date: Sat, 10 Oct 2026 17:41:00 +0900\r\n"
	    "Content-Type: Multipart/Related;\r\n boundary=\"part;one\"\r\n\r\n"
	    "preamble\r\n--part;one\r\nContent-Type: application/smil\r\n\r\n<smil/>\r\n"
	    "--part;one\r\nContent-Type: image/jpeg\r\nContent-Transfer-Encoding: base64\r\n\r\nnot decoded media\r\n"
	    "--part;one\r\nContent-Type: text/plain; charset=\"UTF-8\"\r\nContent-Transfer-Encoding: base64\r\n\r\n44GT44KT44Gr44Gh44Gv\r\n"
	    "--part;one--\r\nepilogue";
	static const char nested[] =
	    "Content-Type: multipart/mixed; boundary=outer\n\n"
	    "--outer\nContent-Type: multipart/alternative; boundary=inner\n\n"
	    "--inner\nContent-Type: text/html\n\n<b>ignore</b>\n"
	    "--inner\nContent-Type: text/plain\n\nHello\n--inner--\n"
	    "--outer\nContent-Type: text/plain\nContent-Disposition: attachment; filename=\"note.txt\"\n\nSECRET\n"
	    "--outer\nContent-Type: text/plain\n\nWorld\n--outer--\n";
	int error;
	int same;

	/* The selected Base64 fixture spells Japanese greeting text in UTF-8. */
	error = parse(mixed);
	same = strcmp(message.text, "こんにちは");
	check(error == 0 && same == 0 && message.type == BTD_MAP_TYPE_MMS, "related: text only, UTF-8 decoded");

	/* Nested multipart keeps only inline plain text and separates distinct parts. */
	error = parse(nested);
	same = strcmp(message.text, "Hello\nWorld");
	check(error == 0 && same == 0, "nested: HTML and attached text omitted");

	/* Quoted-printable soft breaks disappear before charset validation. */
	error = parse("Content-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n=E3=81=82=\r\n=E3=81=84=3D_");
	same = strcmp(message.text, "あい=_");
	check(error == 0 && same == 0, "quoted-printable: UTF-8 and soft break");

	/* A supported legacy text charset is converted rather than misread as UTF-8. */
	error = parse("Content-Type: text/plain; charset=iso-8859-1\nContent-Transfer-Encoding: quoted-printable\n\ncaf=E9");
	same = strcmp(message.text, "café");
	check(error == 0 && same == 0, "Latin-1: UTF-8 conversion");

	/* Padded groups may be separated by line breaks. */
	error = parse("Content-Type: text/plain\nContent-Transfer-Encoding: base64\n\n SG Vs bG8=\n");
	same = strcmp(message.text, "Hello");
	check(error == 0 && same == 0, "base64: padding and whitespace");

	/* Handset CRLF at the end is not displayed as missing-glyph boxes. */
	error = parse("Content-Type: text/plain; charset=utf-8\r\n\r\nFirst\r\nSecond\r\n");
	same = strcmp(message.text, "First\nSecond");
	check(error == 0 && same == 0 && message.text_length == 12U, "CRLF: normalized lines without transport tail");

	/* Unsupported media never falls back to its MIME source. */
	error = parse("Content-Type: text/html\n\n<b>not a message</b>");
	check(error == ENODATA && message.text[0] == '\0', "HTML-only: no raw MIME");
	error = parse("Content-Type: image/jpeg\n\nopaque");
	check(error == ENODATA, "attachment-only: no text");
	error = parse("Content-Type: text/plain; charset=shift_jis\n\nopaque");
	check(error == ENOTSUP, "unsupported charset: explicit error");

	/* Malformed encodings and multipart termination do not become accepted text. */
	error = parse("Content-Type: text/plain\nContent-Transfer-Encoding: base64\n\nSGV");
	check(error == EINVAL, "base64: incomplete quartet refused");
	error = parse("Content-Type: text/plain\nContent-Transfer-Encoding: base64\n\nSGVsbG8=AA==");
	check(error == EINVAL, "base64: data after padding refused");
	error = parse("Content-Type: text/plain\nContent-Transfer-Encoding: quoted-printable\n\nwrong=ZZ");
	check(error == EINVAL, "quoted-printable: malformed escape refused");
	error = parse("Content-Type: multipart/mixed; boundary=b\n\n--b\nContent-Type: text/plain\n\npartial");
	check(error == EINVAL && message.text[0] == '\0', "multipart: missing close refused");
	error = parse("Content-Type: multipart/mixed\n\nopaque");
	check(error == EINVAL, "multipart: missing boundary refused");
	error = parse("MIME source without separator");
	check(error == EINVAL, "headers: no raw fallback");
}

/* Verifies later text after a large attachment and character-safe output bounds. */
static void
test_bounds(void)
{
	char body[40000];
	char output[6];
	const char *head;
	const char *tail;
	size_t length;
	size_t used;
	int truncated;
	int same;
	int error;

	/* Text after an attachment beyond 16KB still reaches the MIME reader. */
	head = "Content-Type: multipart/mixed; boundary=b\n\n--b\nContent-Type: image/jpeg\n\n";
	tail = "\n--b\nContent-Type: text/plain\n\nLater\n--b--\n";
	length = strlen(head);
	memcpy(body, head, length);
	memset(body + length, 'A', 20000U);
	length += 20000U;
	memcpy(body + length, tail, strlen(tail) + 1U);
	error = parse(body);
	same = strcmp(message.text, "Later");
	check(error == 0 && same == 0 && !message.truncated, "attachment before text: no early clamp");

	/* A limit between two multibyte characters keeps the first whole character. */
	head = "Content-Type: text/plain; charset=utf-8\n\nあい";
	error = btd_mms_text((const uint8_t *)head, strlen(head), output, sizeof(output), &used, &truncated);
	same = strcmp(output, "あ");
	check(error == 0 && same == 0 && used == 3U && truncated, "UTF-8: truncates only between characters");
}
