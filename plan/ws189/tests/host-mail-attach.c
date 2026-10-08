/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of Mail's attachments (ws189-p004, userland/desktop/mailer/
 * compose.c and mime.c compiled unchanged): a message with two files is
 * written, read back by Mail's own parser (its words and its first file's
 * name), and each file's base64 is decoded here and compared with its
 * bytes; a name that is not ASCII is written in RFC 2231's form; a message
 * without files stays a single text part.
 */

#include "userland/desktop/mailer/mail.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void
check(
	int condition,
	const char *what)
{
	if (condition)
		return;
	printf("FAIL %s\n", what);
	failures++;
}

/* Decodes the base64 of the part whose filename parameter has a given text, up to the next boundary line. */
static size_t
decode_part(
	const char *raw,
	const char *marker,
	unsigned char *out,
	size_t size)
{
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	const char *at;
	const char *end;
	const char *found;
	uint32_t group;
	size_t length;
	int bits;

	at = strstr(raw, marker);
	if (at == NULL)
		return 0;
	at = strstr(at, "\r\n\r\n");
	if (at == NULL)
		return 0;
	at += 4;
	end = strstr(at, "\r\n--");
	if (end == NULL)
		return 0;
	length = 0;
	group = 0;
	bits = 0;
	for (; at < end; at++) {
		if (*at == '\r' || *at == '\n' || *at == '=')
			continue;
		found = strchr(alphabet, *at);
		if (found == NULL)
			return 0;
		group = (group << 6) | (uint32_t)(found - alphabet);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (length < size)
				out[length] = (unsigned char)((group >> bits) & 0xffU);
			length++;
		}
	}
	return length;
}

int
main(
	void)
{
	struct ml_account_config account;
	struct ml_attachment files[2];
	struct ml_parsed parsed;
	unsigned char picture[1000];
	unsigned char back[2000];
	char *raw;
	size_t length;
	size_t got;
	size_t index;
	int error;

	memset(&account, 0, sizeof(account));
	snprintf(account.name, sizeof(account.name), "Aiko");
	snprintf(account.address, sizeof(account.address), "aiko@example.org");

	/* A picture of every byte value, and a short text file with a Japanese name. */
	for (index = 0; index < sizeof(picture); index++)
		picture[index] = (unsigned char)(index * 7U);
	memset(files, 0, sizeof(files));
	snprintf(files[0].name, sizeof(files[0].name), "image.png");
	snprintf(files[0].type, sizeof(files[0].type), "image/png");
	files[0].data = picture;
	files[0].length = sizeof(picture);
	snprintf(files[1].name, sizeof(files[1].name), "\xe3\x83\xa1\xe3\x83\xa2.txt");
	snprintf(files[1].type, sizeof(files[1].type), "text/plain");
	files[1].data = (unsigned char *)"ab";
	files[1].length = 2;

	/* The message. */
	error = ml_compose_with(&account, "Ben <ben@example.com>", "", "Files", "See the files.\nBye", "", files, 2U, (time_t)1790894700, &raw, &length);
	check(error == 0, "written");
	if (error != 0)
		return 1;
	check(strstr(raw, "Content-Type: multipart/mixed; boundary=\"=_keiland_") != NULL, "multipart");
	check(strstr(raw, "filename=\"image.png\"") != NULL, "ascii name quoted");
	check(strstr(raw, "filename*=UTF-8''%E3%83%A1%E3%83%A2.txt") != NULL, "japanese name rfc 2231");

	/* The bytes back from the base64. */
	got = decode_part(raw, "filename=\"image.png\"", back, sizeof(back));
	check(got == sizeof(picture) && memcmp(back, picture, sizeof(picture)) == 0, "picture bytes");
	got = decode_part(raw, "filename*=UTF-8''%E3%83%A1", back, sizeof(back));
	check(got == 2 && memcmp(back, "ab", 2) == 0, "text bytes");

	/* Mail's own reading of it. */
	memset(&parsed, 0, sizeof(parsed));
	error = ml_mime_parse(raw, length, &parsed);
	check(error == 0, "parsed");
	if (error == 0) {
		check(parsed.body != NULL && strstr(parsed.body, "See the files.") != NULL, "words read back");
		check(strcmp(parsed.file_name, "image.png") == 0, "first file's name read back");
		printf("parsed file=%s size=%lu\n", parsed.file_name, (unsigned long)parsed.file_size);
		ml_mime_release(&parsed);
	}
	free(raw);

	/* Without files: one text part, as before. */
	error = ml_compose_with(&account, "ben@example.com", "", "Hi", "Hello", "", NULL, 0U, (time_t)1790894700, &raw, &length);
	check(error == 0 && strstr(raw, "multipart") == NULL && strstr(raw, "Content-Type: text/plain; charset=utf-8") != NULL, "single part");
	if (error == 0)
		free(raw);

	/* Too many files. */
	error = ml_compose_with(&account, "ben@example.com", "", "Hi", "Hello", "", files, ML_ATTACH_MAX + 1U, (time_t)0, &raw, &length);
	check(error == EINVAL, "too many refused");

	if (failures != 0) {
		printf("host-mail-attach: %d failures\n", failures);
		return 1;
	}
	printf("host-mail-attach: ok\n");
	return 0;
}
