/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * passkey-fido2's pure parts (fido2.h; ws172-p003): base64url (RFC 4648
 * section 5, without padding) for /etc/passkey, hexadecimal for the
 * helper's messages, the messages read back, a key's line of /etc/passkey
 * made and taken apart, and the hashes the login computes.  Nothing here
 * touches a file or a device, so the host test runs it alone.
 */

#include "fido2.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* base64url's alphabet. */
static const char wire_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* The fields of a key's line: name, uid, kind, ID, key, count, relying party, label, date. */
#define WIRE_RECORD_FIELDS	9U

static int wire_base64_value(char character);
static int wire_hex_decode(const char *text, uint8_t *bytes, size_t capacity, size_t *size);
static int wire_hex_value(char character);

/*
 * Writes bytes as base64url without padding, with a NUL.  Returns 0, or
 * ENOSPC when it does not fit.
 */
int
fido2_base64_encode(
	const uint8_t *bytes,
	size_t size,
	char *text,
	size_t capacity)
{
	uint32_t group;
	size_t index;
	size_t used;
	size_t left;

	/* Room for four characters each three bytes, and the NUL. */
	if ((size + 2U) / 3U * 4U + 1U > capacity)
		return ENOSPC;

	/* Each three bytes, the last one or two shorter. */
	used = 0U;
	for (index = 0U; index < size; index += 3U) {
		left = size - index;
		group = (uint32_t)bytes[index] << 16;
		if (left > 1U)
			group |= (uint32_t)bytes[index + 1U] << 8;
		if (left > 2U)
			group |= (uint32_t)bytes[index + 2U];
		text[used++] = wire_alphabet[(group >> 18) & 0x3fU];
		text[used++] = wire_alphabet[(group >> 12) & 0x3fU];
		if (left > 1U)
			text[used++] = wire_alphabet[(group >> 6) & 0x3fU];
		if (left > 2U)
			text[used++] = wire_alphabet[group & 0x3fU];
	}

	/* Succeeded: the text, ended. */
	text[used] = '\0';
	return 0;
}

/*
 * Reads base64url without padding into at most capacity bytes.  Returns
 * 0, EINVAL for a character outside the alphabet or a length no encoding
 * has, or ENOSPC.
 */
int
fido2_base64_decode(
	const char *text,
	uint8_t *bytes,
	size_t capacity,
	size_t *size)
{
	uint32_t group;
	size_t length;
	size_t index;
	size_t used;
	size_t take;
	size_t place;
	int value;

	/* A length an encoding has, and room. */
	length = strlen(text);
	if (length % 4U == 1U)
		return EINVAL;
	if (length / 4U * 3U + (length % 4U) * 3U / 4U > capacity)
		return ENOSPC;

	/* Each four characters, the last two or three shorter. */
	used = 0U;
	for (index = 0U; index < length; index += 4U) {
		take = length - index;
		if (take > 4U)
			take = 4U;
		group = 0U;
		for (place = 0U; place < 4U; place++) {
			value = 0;
			if (place < take)
				value = wire_base64_value(text[index + place]);
			if (value < 0)
				return EINVAL;
			group = group << 6 | (uint32_t)value;
		}

		/* One to three bytes of them. */
		bytes[used++] = (uint8_t)(group >> 16);
		if (take > 2U)
			bytes[used++] = (uint8_t)(group >> 8);
		if (take > 3U)
			bytes[used++] = (uint8_t)group;
	}

	/* Succeeded: the bytes. */
	*size = used;
	return 0;
}

/*
 * Writes bytes as hexadecimal with a NUL.  Returns 0, or ENOSPC.
 */
int
fido2_hex_encode(
	const uint8_t *bytes,
	size_t size,
	char *text,
	size_t capacity)
{
	static const char digits[] = "0123456789abcdef";
	size_t index;

	/* Two digits a byte, and the NUL. */
	if (size * 2U + 1U > capacity)
		return ENOSPC;
	for (index = 0U; index < size; index++) {
		text[2U * index] = digits[bytes[index] >> 4];
		text[2U * index + 1U] = digits[bytes[index] & 0x0fU];
	}

	/* Succeeded: the text, ended. */
	text[2U * size] = '\0';
	return 0;
}

/*
 * Takes a helper's message apart (the line without its end; its words are
 * cut in place).  Returns 0, or EINVAL for a line that is not a message.
 */
int
fido2_message_parse(
	char *line,
	struct fido2_message *message)
{
	char *words[5];
	char *place;
	size_t count;
	size_t length;
	char extra;
	int parsed;
	int same;
	int error;

	/* The words, split at spaces. */
	memset(message, 0, sizeof(*message));
	count = 0U;
	place = line;
	while (count < 5U) {
		words[count++] = place;
		place = strchr(place, ' ');
		if (place == NULL)
			break;
		*place = '\0';
		place++;
	}

	/* More than five words is no message. */
	if (place != NULL)
		return EINVAL;

	/* touch. */
	same = strcmp(words[0], "touch") == 0;
	if (same && count == 1U) {
		message->kind = FIDO2_MESSAGE_TOUCH;
		return 0;
	}

	/* fail REASON. */
	same = strcmp(words[0], "fail") == 0;
	length = sizeof(message->reason);
	if (count == 2U)
		length = strlen(words[1]);
	if (same && length < sizeof(message->reason)) {
		message->kind = FIDO2_MESSAGE_FAIL;
		(void)snprintf(message->reason, sizeof(message->reason), "%s", words[1]);
		return 0;
	}

	/* done (a key's PIN set or changed, ws199-p001). */
	same = strcmp(words[0], "done") == 0;
	if (same && count == 1U) {
		message->kind = FIDO2_MESSAGE_DONE;
		return 0;
	}

	/* info COUNT,CARD,INDEX,PIN,RETRIES,MIN (what the one key is, ws199-p001). */
	same = strcmp(words[0], "info") == 0;
	if (same && count == 2U) {
		parsed = sscanf(words[1], "%u,%u,%u,%u,%u,%u%c", &message->info_count, &message->info_card, &message->info_index,
		    &message->info_pin, &message->info_retries, &message->info_min, &extra);
		if (parsed != 6)
			return EINVAL;
		message->kind = FIDO2_MESSAGE_INFO;
		return 0;
	}

	/* reset MASK (the credentials the reset key held, by their places in the job, ws199-p001). */
	same = strcmp(words[0], "reset") == 0;
	if (same && count == 2U) {
		parsed = sscanf(words[1], "%x%c", &message->held, &extra);
		if (parsed != 1)
			return EINVAL;
		message->kind = FIDO2_MESSAGE_RESET;
		return 0;
	}

	/*
	 * owner MASK,CARD (no group held), or owner MASK,CARD ID AUTH-DATA
	 * SIGNATURE (the first group's silent answer, ws199-p001).
	 */
	same = strcmp(words[0], "owner") == 0;
	if (same && (count == 2U || count == 5U)) {
		parsed = sscanf(words[1], "%x,%u%c", &message->held, &message->owner_card, &extra);
		if (parsed != 2 || message->owner_card > 1U)
			return EINVAL;

		/* No group held: nothing more. */
		if (count == 2U && message->held != 0U)
			return EINVAL;
		if (count == 2U) {
			message->kind = FIDO2_MESSAGE_OWNER;
			return 0;
		}

		/* A group held: its answer's bytes. */
		if (message->held == 0U)
			return EINVAL;
		error = wire_hex_decode(words[2], message->id, sizeof(message->id), &message->id_size);
		if (error == 0)
			error = wire_hex_decode(words[3], message->auth_data, sizeof(message->auth_data), &message->auth_data_size);
		if (error == 0)
			error = wire_hex_decode(words[4], message->signature, sizeof(message->signature), &message->signature_size);
		if (error != 0)
			return EINVAL;

		/* Succeeded: the owner's answer. */
		message->kind = FIDO2_MESSAGE_OWNER;
		return 0;
	}

	/* made AUTH-DATA. */
	same = strcmp(words[0], "made") == 0;
	if (same && count == 2U) {
		error = wire_hex_decode(words[1], message->auth_data, sizeof(message->auth_data), &message->auth_data_size);
		if (error != 0)
			return EINVAL;
		message->kind = FIDO2_MESSAGE_MADE;
		return 0;
	}

	/* assertion ID AUTH-DATA SIGNATURE. */
	same = strcmp(words[0], "assertion") == 0;
	if (!same || count != 4U)
		return EINVAL;
	error = wire_hex_decode(words[1], message->id, sizeof(message->id), &message->id_size);
	if (error == 0)
		error = wire_hex_decode(words[2], message->auth_data, sizeof(message->auth_data), &message->auth_data_size);
	if (error == 0)
		error = wire_hex_decode(words[3], message->signature, sizeof(message->signature), &message->signature_size);
	if (error != 0)
		return EINVAL;

	/* Succeeded: an assertion. */
	message->kind = FIDO2_MESSAGE_ASSERTION;
	return 0;
}

/*
 * Takes a key's line of /etc/passkey apart (name:uid:fido2:ID:KEY:COUNT:
 * RP:LABEL:DATE).  Returns 0, or EINVAL for a line that is not one of the
 * login's relying party.
 */
int
fido2_record_parse(
	const char *line,
	struct fido2_record *record)
{
	char copy[4096];
	char *fields[WIRE_RECORD_FIELDS];
	char *place;
	char *end;
	unsigned long count;
	size_t index;
	size_t length;
	int written;
	int same;
	int error;

	/* The nine fields, split at colons in a copy. */
	memset(record, 0, sizeof(*record));
	written = snprintf(copy, sizeof(copy), "%s", line);
	if (written < 0 || (size_t)written >= sizeof(copy))
		return EINVAL;
	place = copy;
	for (index = 0U; index < WIRE_RECORD_FIELDS; index++) {
		fields[index] = place;
		place = strchr(place, ':');
		if (place == NULL)
			break;
		*place = '\0';
		place++;
	}

	/* Exactly nine. */
	if (index != WIRE_RECORD_FIELDS - 1U || place != NULL)
		return EINVAL;

	/* A key's line of the login's relying party. */
	same = strcmp(fields[2], "fido2") == 0 && strcmp(fields[6], FIDO2_RP) == 0;
	if (!same)
		return EINVAL;

	/* Its ID and key. */
	error = fido2_base64_decode(fields[3], record->id, sizeof(record->id), &record->id_size);
	if (error == 0)
		error = fido2_base64_decode(fields[4], record->cose_key, sizeof(record->cose_key), &record->cose_key_size);
	length = strlen(fields[3]);
	if (error != 0 || record->id_size == 0U || length >= sizeof(record->id_text))
		return EINVAL;
	(void)snprintf(record->id_text, sizeof(record->id_text), "%s", fields[3]);

	/* Its count. */
	errno = 0;
	count = strtoul(fields[5], &end, 10);
	if (errno != 0 || end == fields[5] || *end != '\0' || count > 0xffffffffUL)
		return EINVAL;
	record->sign_count = (uint32_t)count;

	/* Succeeded: its label too. */
	(void)snprintf(record->label, sizeof(record->label), "%s", fields[7]);
	return 0;
}

/*
 * Writes a key's line of /etc/passkey (without its end).  Returns 0, or
 * ENOSPC.
 */
int
fido2_record_line(
	const char *name,
	uid_t uid,
	const char *id,
	const char *cose_key,
	uint32_t count,
	const char *label,
	const char *date,
	char *line,
	size_t size)
{
	int written;

	/* The fields in their order. */
	written = snprintf(line, size, "%s:%u:fido2:%s:%s:%lu:%s:%s:%s", name, (unsigned)uid, id, cose_key, (unsigned long)count,
	    FIDO2_RP, label, date);
	if (written < 0 || (size_t)written >= size)
		return ENOSPC;

	/* Succeeded: the line. */
	return 0;
}

/*
 * Writes a key's line again with a new signature count (the sixth field),
 * every other byte kept.  Returns 0, EINVAL for a line without the field,
 * or ENOSPC.
 */
int
fido2_record_recount(
	const char *line,
	uint32_t count,
	char *output,
	size_t size)
{
	const char *start;
	const char *end;
	unsigned colons;
	int written;

	/* The field: after the fifth colon, up to the sixth. */
	start = line;
	for (colons = 0U; colons < 5U; colons++) {
		start = strchr(start, ':');
		if (start == NULL)
			return EINVAL;
		start++;
	}

	/* Up to the sixth colon. */
	end = strchr(start, ':');
	if (end == NULL)
		return EINVAL;

	/* The line before it, the count, the line after it. */
	written = snprintf(output, size, "%.*s%lu%s", (int)(start - line), line, (unsigned long)count, end);
	if (written < 0 || (size_t)written >= size)
		return ENOSPC;

	/* Succeeded: the new line. */
	return 0;
}

/* Tells whether a label may be kept: 1 to FIDO2_LABEL_MAX bytes, no control character and no colon. */
int
fido2_label_valid(
	const char *label)
{
	size_t length;
	size_t index;
	unsigned char character;

	/* Its length. */
	length = strlen(label);
	if (length == 0U || length > FIDO2_LABEL_MAX)
		return 0;

	/* Its characters. */
	for (index = 0U; index < length; index++) {
		character = (unsigned char)label[index];
		if (character < 0x20U || character == 0x7fU || character == ':')
			return 0;
	}

	/* A label. */
	return 1;
}

/*
 * Makes the login's client data hash: SHA-256 of the relying party, a
 * NUL, the name, a NUL and the challenge.  Returns 0 or an errno value.
 */
int
fido2_client_data_hash(
	const char *name,
	const uint8_t *challenge,
	uint8_t *hash)
{
	struct pk_crypto_part parts[3];
	int error;

	/* The three parts, each string with its NUL. */
	parts[0].data = (const uint8_t *)FIDO2_RP;
	parts[0].size = sizeof(FIDO2_RP);
	parts[1].data = (const uint8_t *)name;
	parts[1].size = strlen(name) + 1U;
	parts[2].data = challenge;
	parts[2].size = FIDO2_CHALLENGE_SIZE;
	error = pk_crypto_sha256(parts, 3U, hash);
	if (error != 0)
		return error;

	/* Succeeded: the hash. */
	return 0;
}

/* Makes a new credential's user ID: the first bytes of the name's SHA-256.  Returns 0 or an errno value. */
int
fido2_user_id(
	const char *name,
	uint8_t *id)
{
	struct pk_crypto_part part;
	uint8_t hash[PK_SHA256_SIZE];
	int error;

	/* The name's hash, cut. */
	part.data = (const uint8_t *)name;
	part.size = strlen(name);
	error = pk_crypto_sha256(&part, 1U, hash);
	if (error != 0)
		return error;
	memcpy(id, hash, FIDO2_USER_ID_SIZE);

	/* Succeeded: the ID. */
	return 0;
}

/* Gives a base64url character's value, or -1. */
static int
wire_base64_value(
	char character)
{
	const char *found;

	/* Its place in the alphabet. */
	if (character == '\0')
		return -1;
	found = strchr(wire_alphabet, character);
	if (found == NULL)
		return -1;
	return (int)(found - wire_alphabet);
}

/* Reads hexadecimal into at most capacity bytes; returns 0 or EINVAL. */
static int
wire_hex_decode(
	const char *text,
	uint8_t *bytes,
	size_t capacity,
	size_t *size)
{
	size_t length;
	size_t index;
	int high;
	int low;

	/* An even number of digits that fits. */
	length = strlen(text);
	if (length % 2U != 0U || length / 2U > capacity)
		return EINVAL;

	/* Two digits a byte. */
	for (index = 0U; index < length / 2U; index++) {
		high = wire_hex_value(text[2U * index]);
		low = wire_hex_value(text[2U * index + 1U]);
		if (high < 0 || low < 0)
			return EINVAL;
		bytes[index] = (uint8_t)(high << 4 | low);
	}

	/* Succeeded: the bytes. */
	*size = length / 2U;
	return 0;
}

/* Gives a hexadecimal digit's value (small letters), or -1. */
static int
wire_hex_value(
	char character)
{
	/* The digits, then the letters. */
	if (character >= '0' && character <= '9')
		return character - '0';
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;

	/* Not a digit. */
	return -1;
}
