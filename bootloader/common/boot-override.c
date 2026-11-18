/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The boot keys' rewrite of an assembled boot parameter record.
 *
 * A loader that sees Ctrl held at boot hands the kernel kmsg=console and
 * no logo= token; one that sees Shift held hands it login=console.  The
 * record the configuration produced is rewritten in place: the tokens the
 * keys replace are dropped by name, and the replacements are appended.
 * Dropping comes first because the kernel refuses a known name given twice.
 */

#include "boot-override.h"

/* The token Ctrl appends: the kernel messages are shown on the console. */
#define OVERRIDE_KMSG_TOKEN	"kmsg=console"

/* The token Shift appends: the login is on the console. */
#define OVERRIDE_LOGIN_TOKEN	"login=console"

static size_t override_compact(char *text, size_t length, unsigned keys);
static size_t override_append(char *text, size_t length, const char *token);
static size_t override_token_end(const char *text, size_t length, size_t start);
static int override_name_dropped(const char *token, size_t length, unsigned keys);
static int override_name_is(const char *token, size_t name_length, const char *name);

/*
 * Rewrites an assembled boot parameter record for the keys held at boot.
 *
 * Returns -1 without touching a record whose text is not terminated within
 * the kernel's limit.  Otherwise returns 0, leaving the record alone when no
 * known key bit is set (unknown bits are ignored) and rewriting it when one
 * is.  The bytes from the new terminator up to the old length are zeroed, so
 * the record holds no stale text, and applying the same keys twice gives the
 * same record.  A replacement token that does not fit is left out; the
 * kernel's default for an absent kmsg= or login= is the console anyway.
 */
int
zbl_boot_override_apply(
	struct kern_boot_parameter_record *record,
	unsigned keys)
{
	size_t old_length;
	size_t length;
	size_t index;

	/* Refuses a record whose length is beyond what the kernel takes. */
	if (record->length > KERN_BOOT_PARAMETERS_TEXT_MAX)
		return -1;

	/* Refuses a record whose text does not end where its length says. */
	if (record->text[record->length] != '\0')
		return -1;

	/* Leaves the record alone when neither known key was held. */
	if ((keys & (ZBL_BOOT_OVERRIDE_KMSG | ZBL_BOOT_OVERRIDE_LOGIN)) == 0U)
		return 0;

	/* Drops the tokens the held keys replace. */
	old_length = record->length;
	length = override_compact(record->text, old_length, keys);

	/* Ctrl shows the kernel messages on the console. */
	if ((keys & ZBL_BOOT_OVERRIDE_KMSG) != 0U)
		length = override_append(record->text, length, OVERRIDE_KMSG_TOKEN);

	/* Shift keeps the login on the console. */
	if ((keys & ZBL_BOOT_OVERRIDE_LOGIN) != 0U)
		length = override_append(record->text, length, OVERRIDE_LOGIN_TOKEN);

	/* Terminates the rewritten text. */
	record->text[length] = '\0';

	/* Clears what the dropped tokens left behind the terminator. */
	for (index = length + 1U; index <= old_length; index++)
		record->text[index] = '\0';

	/* The record's length is the rewritten text's. */
	record->length = (uint16_t)length;

	/* Succeeded: the record carries the keys' choices. */
	return 0;
}

/* Drops every token whose name the keys replace and reports the new length. */
static size_t
override_compact(
	char *text,
	size_t length,
	unsigned keys)
{
	size_t read_position;
	size_t write_position;
	size_t end;
	size_t index;
	int dropped;

	/*
	 * Walks the space-separated tokens and moves each kept one down to the
	 * write position.  The write position never passes the read position,
	 * so moving within the one buffer is safe.
	 */
	read_position = 0;
	write_position = 0;
	while (read_position < length) {
		/* Finds the token at the read position and whether the keys replace it. */
		end = override_token_end(text, length, read_position);
		dropped = override_name_dropped(&text[read_position], end - read_position, keys);

		/* An empty token or a replaced one is skipped with its separator. */
		if (end == read_position || dropped) {
			read_position = end + 1U;
			continue;
		}

		/* Separates a kept token from the one kept before it. */
		if (write_position != 0U) {
			text[write_position] = ' ';
			write_position++;
		}

		/* Moves the kept token down. */
		for (index = read_position; index < end; index++) {
			text[write_position] = text[index];
			write_position++;
		}

		/* The next token starts after the separator. */
		read_position = end + 1U;
	}

	/* Reports the length of the kept tokens. */
	return write_position;
}

/* Appends one token when it fits and reports the new length. */
static size_t
override_append(
	char *text,
	size_t length,
	const char *token)
{
	size_t token_length;
	size_t needed;
	size_t index;

	/* Measures the token. */
	token_length = 0;
	while (token[token_length] != '\0')
		token_length++;

	/* A token after another one needs a separating space. */
	needed = token_length;
	if (length != 0U)
		needed++;

	/* Leaves a token out that would pass the kernel's limit. */
	if (length + needed > KERN_BOOT_PARAMETERS_TEXT_MAX)
		return length;

	/* Separates the token from the text before it. */
	if (length != 0U) {
		text[length] = ' ';
		length++;
	}

	/* Copies the token. */
	for (index = 0; index < token_length; index++) {
		text[length] = token[index];
		length++;
	}

	/* Reports the length with the token. */
	return length;
}

/* Finds where the token that starts at start ends (at a space or the end). */
static size_t
override_token_end(
	const char *text,
	size_t length,
	size_t start)
{
	size_t end;

	/* Up to the next space. */
	end = start;
	while (end < length && text[end] != ' ')
		end++;

	/* Reports the end. */
	return end;
}

/* Reports whether the held keys replace the token, judged by its whole name. */
static int
override_name_dropped(
	const char *token,
	size_t length,
	unsigned keys)
{
	size_t name_length;
	int same;

	/* The name runs up to the first '='; a later '=' belongs to the value. */
	name_length = 0;
	while (name_length < length && token[name_length] != '=')
		name_length++;

	/* A token without '=' has no name and is kept. */
	if (name_length == length)
		return 0;

	/* Ctrl replaces kmsg= and drops logo=. */
	if ((keys & ZBL_BOOT_OVERRIDE_KMSG) != 0U) {
		same = override_name_is(token, name_length, "kmsg");
		if (same)
			return 1;

		/* The logo is not drawn when the messages are to be read. */
		same = override_name_is(token, name_length, "logo");
		if (same)
			return 1;
	}

	/* Shift replaces login=. */
	if ((keys & ZBL_BOOT_OVERRIDE_LOGIN) != 0U) {
		same = override_name_is(token, name_length, "login");
		if (same)
			return 1;
	}

	/* Any other name is kept. */
	return 0;
}

/* Reports whether a token's name is the given name, compared whole. */
static int
override_name_is(
	const char *token,
	size_t name_length,
	const char *name)
{
	size_t index;

	/* Compares the characters the token's name has. */
	for (index = 0; index < name_length; index++) {
		if (name[index] != token[index])
			return 0;
	}

	/* The same name only when the given one ends there too. */
	if (name[name_length] != '\0')
		return 0;

	/* Succeeded: the names are the same. */
	return 1;
}
