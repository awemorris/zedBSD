/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The request of /sbin/passkey (passkey.h; ws172-p002): one field a line,
 * every line ended by a line end, the exact number of fields its
 * operation has, no NUL and no other control character.
 */

#include "passkey.h"

#include <errno.h>
#include <string.h>

/* An operation's word and how many fields its request has. */
struct request_shape {
	const char *word;
	int operation;
	unsigned fields;
};

/* The operations and their fields (the word, the name, then the rest). */
static const struct request_shape request_shapes[] = {
	{ "auth", PASSKEY_OP_AUTH, 4U },
	{ "styles", PASSKEY_OP_STYLES, 2U },
	{ "enrolled", PASSKEY_OP_ENROLLED, 2U },
	{ "enroll-pin", PASSKEY_OP_ENROLL_PIN, 4U },
	{ "remove-pin", PASSKEY_OP_REMOVE_PIN, 3U },
	{ "enroll-fido2", PASSKEY_OP_ENROLL_FIDO2, 5U },
	{ "remove-fido2", PASSKEY_OP_REMOVE_FIDO2, 4U },
	{ "key-info", PASSKEY_OP_KEY_INFO, 2U },
	{ "key-set-pin", PASSKEY_OP_KEY_SET_PIN, 3U },
	{ "key-change-pin", PASSKEY_OP_KEY_CHANGE_PIN, 4U },
	{ "key-reset", PASSKEY_OP_KEY_RESET, 3U },
	{ "set-options", PASSKEY_OP_SET_OPTIONS, 5U },
	{ "auth-fido2", PASSKEY_OP_AUTH_FIDO2, 4U },
	{ "key-owner", PASSKEY_OP_KEY_OWNER, 2U },
};

/*
 * Takes a request apart in place.  Returns 0, or EINVAL for a request that
 * is not one (an unknown operation, the wrong number of fields, a field too
 * long, a control character, a missing last line end).
 */
int
passkey_request_parse(
	char *text,
	size_t length,
	struct passkey_request *request)
{
	unsigned count;
	size_t index;
	size_t start;
	size_t shape;
	unsigned char character;
	int same;

	/* Bounded, and ended by a line end. */
	memset(request, 0, sizeof(*request));
	if (length == 0U || length > PASSKEY_REQUEST_MAX || text[length - 1U] != '\n')
		return EINVAL;

	/* The fields: the line ends become NULs; a control character refuses the request. */
	count = 0U;
	start = 0U;
	for (index = 0U; index < length; index++) {
		character = (unsigned char)text[index];
		if (character == '\n') {
			if (count == PASSKEY_FIELDS_MAX || index - start > PASSKEY_FIELD_MAX)
				return EINVAL;
			text[index] = '\0';
			request->fields[count++] = text + start;
			start = index + 1U;
			continue;
		}
		if (character < 0x20U || character == 0x7fU)
			return EINVAL;
	}
	request->count = count;

	/* The operation, with its number of fields, and a name. */
	for (shape = 0U; shape < sizeof(request_shapes) / sizeof(request_shapes[0]); shape++) {
		same = strcmp(request->fields[0], request_shapes[shape].word);
		if (same != 0)
			continue;
		if (count != request_shapes[shape].fields)
			return EINVAL;
		if (request->fields[1][0] == '\0' || strlen(request->fields[1]) > 32U)
			return EINVAL;
		request->operation = request_shapes[shape].operation;
		return 0;
	}

	/* An unknown operation. */
	return EINVAL;
}

/* Tells whether text is a PIN: exactly six decimal digits. */
int
passkey_is_pin(
	const char *text)
{
	size_t index;

	/* Six digits and nothing after them. */
	for (index = 0U; index < PASSKEY_PIN_DIGITS; index++) {
		if (text[index] < '0' || text[index] > '9')
			return 0;
	}
	if (text[PASSKEY_PIN_DIGITS] != '\0')
		return 0;

	/* A PIN. */
	return 1;
}

/* Wipes a secret through a volatile pointer, so the stores are not left out. */
void
passkey_wipe(
	void *memory,
	size_t size)
{
	volatile unsigned char *byte;
	size_t index;

	/* Every byte. */
	byte = memory;
	for (index = 0U; index < size; index++)
		byte[index] = 0U;
}
