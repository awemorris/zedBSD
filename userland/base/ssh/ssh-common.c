/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The shared helpers of ssh, sshd and ssh-keygen: byte order, name-lists, base64.
 */

#include <string.h>

#include "ssh-common.h"

/*
 * The 64 characters base64 encodes six bits with, indexed by the six-bit value.
 *
 * Values referenced: RFC 4648 section 4, Table 1 (The Base 64 Alphabet):
 * values 0 to 25 are "A" to "Z", 26 to 51 are "a" to "z", 52 to 61 are
 * "0" to "9", 62 is "+" and 63 is "/".
 */
static const char base64_alphabet[64 + 1] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789"
    "+/";

/*
 * The padding character that completes the last group of four characters.
 *
 * Values referenced: RFC 4648 section 4 ("=" in the encoding of the final
 * quantum) and section 3.2 (padding).
 */
#define BASE64_PAD_CHARACTER	'='

static int base64_value(char character);
static int base64_check(const char *in, size_t size, size_t *decoded_size);

/*
 * Reads a 32-bit big-endian integer from bytes[0..3].
 */
uint32_t
ssh_load_be32(
	const uint8_t *bytes)
{
	uint32_t value;

	/* Joins the four bytes, most significant first; each is widened before it is shifted. */
	value = (uint32_t)bytes[0] << 24;
	value |= (uint32_t)bytes[1] << 16;
	value |= (uint32_t)bytes[2] << 8;
	value |= (uint32_t)bytes[3];

	/* Reports the integer. */
	return value;
}

/*
 * Reads a 64-bit big-endian integer from bytes[0..7].
 */
uint64_t
ssh_load_be64(
	const uint8_t *bytes)
{
	uint32_t high;
	uint32_t low;
	uint64_t value;

	/* Reads the two 32-bit halves, the first four bytes being the high one. */
	high = ssh_load_be32(bytes);
	low = ssh_load_be32(bytes + 4);

	/* Builds the 64-bit integer from its halves. */
	value = (uint64_t)high << 32;
	value |= (uint64_t)low;

	/* Reports the integer. */
	return value;
}

/*
 * Writes value big-endian into bytes[0..3].
 */
void
ssh_store_be32(
	uint8_t *bytes,
	uint32_t value)
{
	/* Stores the four bytes, most significant first. */
	bytes[0] = (uint8_t)((value >> 24) & 0xffU);
	bytes[1] = (uint8_t)((value >> 16) & 0xffU);
	bytes[2] = (uint8_t)((value >> 8) & 0xffU);
	bytes[3] = (uint8_t)(value & 0xffU);
}

/*
 * Writes value big-endian into bytes[0..7].
 */
void
ssh_store_be64(
	uint8_t *bytes,
	uint64_t value)
{
	uint32_t high;
	uint32_t low;

	/* Splits the integer into its 32-bit halves. */
	high = (uint32_t)((value >> 32) & 0xffffffffU);
	low = (uint32_t)(value & 0xffffffffU);

	/* Stores the high half first, then the low half. */
	ssh_store_be32(bytes, high);
	ssh_store_be32(bytes + 4, low);
}

/*
 * Checks that size bytes at list form a valid name-list (RFC 4251 5):
 * names separated by single commas, each 1 to SSH_NAME_MAX bytes of
 * printable US-ASCII other than a comma (0x21 to 0x7e), no empty name,
 * no leading or trailing comma; an empty list (size 0) is valid.
 */
int
ssh_namelist_check(
	const char *list,
	size_t size)
{
	const unsigned char *bytes;
	unsigned char byte;
	size_t index;
	size_t name_length;

	/* Refuses a list longer than a peer may send. */
	if (size > SSH_NAMELIST_MAX)
		return EMSGSIZE;

	/* An empty list holds no name and is valid. */
	if (size == 0)
		return 0;

	/* Refuses a missing list that claims to hold bytes. */
	if (list == NULL)
		return EINVAL;

	/* Walks the bytes, counting the length of the name being read. */
	bytes = (const unsigned char *)list;
	name_length = 0;
	for (index = 0; index < size; index++) {
		byte = bytes[index];

		/*
		 * A comma ends a name, and a name has at least one character,
		 * so a comma first in the list or right after a comma is
		 * malformed.  Any other byte must be printable US-ASCII, and
		 * the name it belongs to must stay within the name limit.
		 */
		if (byte == ',') {
			if (name_length == 0)
				return EINVAL;
			name_length = 0;
		} else if (byte < 0x21U || byte > 0x7eU) {
			return EINVAL;
		} else {
			name_length++;
			if (name_length > SSH_NAME_MAX)
				return EINVAL;
		}
	}

	/* A trailing comma leaves the last name empty. */
	if (name_length == 0)
		return EINVAL;

	/* Succeeded: the list is well formed. */
	return 0;
}

/*
 * Reports whether the name-list of size bytes at list (already checked
 * with ssh_namelist_check) contains name exactly (case-sensitive, RFC
 * 4251 6).
 */
int
ssh_namelist_contains(
	const char *list,
	size_t size,
	const char *name)
{
	size_t name_length;
	size_t start;
	size_t index;
	size_t segment_length;
	int difference;

	/* A name or a list that is missing matches nothing. */
	if (name == NULL)
		return 0;
	if (list == NULL)
		return 0;

	/* Finds the length of the name asked for; an empty name is in no list. */
	name_length = strlen(name);
	if (name_length == 0)
		return 0;

	/*
	 * Walks the names of the list: each ends at a comma or at the end of
	 * the list (index == size), and is compared whole, so a name that is
	 * only the beginning of a longer one does not match.
	 */
	start = 0;
	for (index = 0; index <= size; index++) {
		/* Moves on until a comma or the end of the list is reached. */
		if (index < size && list[index] != ',')
			continue;

		/* Compares the name between start and index with the one asked for. */
		segment_length = index - start;
		if (segment_length == name_length) {
			difference = memcmp(list + start, name, name_length);
			if (difference == 0)
				return 1;
		}

		/* The next name begins after the comma. */
		start = index + 1;
	}

	/* No name of the list is equal to the one asked for. */
	return 0;
}

/*
 * Returns the number of characters of base64 (RFC 4648 4, with '='
 * padding) that encode size bytes, not counting a NUL.
 */
size_t
ssh_base64_encoded_size(
	size_t size)
{
	size_t groups;
	size_t largest;

	/* The largest size whose encoded length still fits in a size_t. */
	largest = (SIZE_MAX / 4) * 3 - 3;

	/* Refuses a size whose encoding would not fit. */
	if (size > largest)
		return 0;

	/* Every started group of three bytes becomes four characters. */
	groups = size / 3;
	if (size % 3 != 0)
		groups++;

	/* Reports the number of characters. */
	return groups * 4;
}

/*
 * Encodes size bytes at in as base64 (RFC 4648 4, alphabet of Table 1,
 * '=' padding) into out, followed by a NUL.
 *
 * The room is checked against the padded length even when pad is 0, so a
 * caller that sizes the buffer with ssh_base64_encoded_size always has
 * enough.
 */
int
ssh_base64_encode(
	const uint8_t *in,
	size_t size,
	char *out,
	size_t capacity,
	int pad)
{
	size_t encoded_size;
	size_t in_index;
	size_t out_index;
	size_t left;
	uint32_t group;

	/* Refuses bytes that are missing. */
	if (in == NULL && size != 0)
		return EINVAL;

	/* Refuses a size whose encoding no buffer can hold. */
	encoded_size = ssh_base64_encoded_size(size);
	if (size != 0 && encoded_size == 0)
		return ENOBUFS;

	/* Refuses a buffer too small for the characters and the NUL. */
	if (out == NULL || capacity < encoded_size + 1)
		return ENOBUFS;

	/* Encodes each whole group of three bytes as four characters. */
	in_index = 0;
	out_index = 0;
	while (size - in_index >= 3) {
		group = (uint32_t)in[in_index] << 16;
		group |= (uint32_t)in[in_index + 1] << 8;
		group |= (uint32_t)in[in_index + 2];
		in_index += 3;

		out[out_index] = base64_alphabet[(group >> 18) & 0x3fU];
		out[out_index + 1] = base64_alphabet[(group >> 12) & 0x3fU];
		out[out_index + 2] = base64_alphabet[(group >> 6) & 0x3fU];
		out[out_index + 3] = base64_alphabet[group & 0x3fU];
		out_index += 4;
	}

	/*
	 * Encodes the one or two bytes left over: the missing low bits of the
	 * last character are zero (RFC 4648 3.5), and the group is completed
	 * with '=' unless the caller asked for none.
	 */
	left = size - in_index;
	if (left == 1) {
		group = (uint32_t)in[in_index] << 16;

		out[out_index] = base64_alphabet[(group >> 18) & 0x3fU];
		out[out_index + 1] = base64_alphabet[(group >> 12) & 0x3fU];
		out_index += 2;
		if (pad != 0) {
			out[out_index] = BASE64_PAD_CHARACTER;
			out[out_index + 1] = BASE64_PAD_CHARACTER;
			out_index += 2;
		}
	} else if (left == 2) {
		group = (uint32_t)in[in_index] << 16;
		group |= (uint32_t)in[in_index + 1] << 8;

		out[out_index] = base64_alphabet[(group >> 18) & 0x3fU];
		out[out_index + 1] = base64_alphabet[(group >> 12) & 0x3fU];
		out[out_index + 2] = base64_alphabet[(group >> 6) & 0x3fU];
		out_index += 3;
		if (pad != 0) {
			out[out_index] = BASE64_PAD_CHARACTER;
			out_index++;
		}
	}

	/* Ends the text. */
	out[out_index] = '\0';

	/* Succeeded: the characters and the NUL are in out. */
	return 0;
}

/*
 * Decodes size characters of base64 at in (RFC 4648 4) into out.
 *
 * The whole input is checked before anything is written, so an input that
 * is malformed answers EINVAL whatever the capacity.
 */
int
ssh_base64_decode(
	const char *in,
	size_t size,
	uint8_t *out,
	size_t capacity,
	size_t *out_size)
{
	size_t decoded_size;
	size_t in_index;
	size_t out_index;
	int error;
	int value0;
	int value1;
	int value2;
	int value3;
	uint32_t group;

	/* Nothing has been decoded yet. */
	*out_size = 0;

	/* Refuses characters that are missing. */
	if (in == NULL && size != 0)
		return EINVAL;

	/* Checks the characters, the padding and the unused bits, and finds how many bytes they decode to. */
	error = base64_check(in, size, &decoded_size);
	if (error != 0)
		return error;

	/* Refuses an output buffer too small for the bytes. */
	if (decoded_size > capacity)
		return ENOBUFS;
	if (decoded_size != 0 && out == NULL)
		return ENOBUFS;

	/*
	 * Decodes each group of four characters into up to three bytes.  The
	 * last group may carry padding; the bytes beyond decoded_size are not
	 * stored.
	 */
	in_index = 0;
	out_index = 0;
	while (in_index < size) {
		value0 = base64_value(in[in_index]);
		value1 = base64_value(in[in_index + 1]);
		value2 = base64_value(in[in_index + 2]);
		value3 = base64_value(in[in_index + 3]);

		/* A padding character stands for six zero bits that carry no byte. */
		if (value2 < 0)
			value2 = 0;
		if (value3 < 0)
			value3 = 0;

		group = (uint32_t)value0 << 18;
		group |= (uint32_t)value1 << 12;
		group |= (uint32_t)value2 << 6;
		group |= (uint32_t)value3;

		/* Stores the first byte of the group, which every group has. */
		out[out_index] = (uint8_t)((group >> 16) & 0xffU);
		out_index++;

		/* Stores the second and the third byte unless the group ends earlier. */
		if (out_index < decoded_size) {
			out[out_index] = (uint8_t)((group >> 8) & 0xffU);
			out_index++;
		}
		if (out_index < decoded_size) {
			out[out_index] = (uint8_t)(group & 0xffU);
			out_index++;
		}

		in_index += 4;
	}

	/* Publishes the length. */
	*out_size = decoded_size;

	/* Succeeded: decoded_size bytes are in out. */
	return 0;
}

/*
 * Returns the six-bit value of a base64 character, or -1 for a character
 * outside the alphabet (the padding character included).
 */
static int
base64_value(
	char character)
{
	unsigned char byte;

	byte = (unsigned char)character;

	/* Looks the character up by its range in Table 1 of RFC 4648. */
	if (byte >= 'A' && byte <= 'Z')
		return (int)(byte - 'A');
	if (byte >= 'a' && byte <= 'z')
		return (int)(byte - 'a') + 26;
	if (byte >= '0' && byte <= '9')
		return (int)(byte - '0') + 52;
	if (byte == '+')
		return 62;
	if (byte == '/')
		return 63;

	/* The character is not in the alphabet. */
	return -1;
}

/*
 * Checks size base64 characters and finds how many bytes they decode to.
 *
 * Every character must be in the alphabet, except that the last group of
 * four may end with one or two padding characters; the low bits of the
 * last character before the padding that no byte uses must be zero.
 */
static int
base64_check(
	const char *in,
	size_t size,
	size_t *decoded_size)
{
	size_t index;
	size_t last;
	size_t groups;
	int value;
	int value_last;
	int pad_count;

	/* An empty input decodes to nothing. */
	*decoded_size = 0;
	if (size == 0)
		return 0;

	/* The encoded form is whole groups of four characters. */
	if (size % 4 != 0)
		return EINVAL;

	/* Finds how many padding characters end the input: none, one or two. */
	last = size - 4;
	pad_count = 0;
	if (in[size - 1] == BASE64_PAD_CHARACTER) {
		pad_count = 1;
		if (in[size - 2] == BASE64_PAD_CHARACTER)
			pad_count = 2;
	}

	/* Checks that every character before the padding is in the alphabet. */
	for (index = 0; index < size - (size_t)pad_count; index++) {
		value = base64_value(in[index]);
		if (value < 0)
			return EINVAL;
	}

	/*
	 * Checks the unused bits of the last character before the padding
	 * (RFC 4648 3.5): two '=' leave four bits, one '=' leaves two bits.
	 */
	if (pad_count == 2) {
		value_last = base64_value(in[last + 1]);
		if ((value_last & 0x0f) != 0)
			return EINVAL;
	} else if (pad_count == 1) {
		value_last = base64_value(in[last + 2]);
		if ((value_last & 0x03) != 0)
			return EINVAL;
	}

	/* Three bytes for each group, less one or two bytes for the padding. */
	groups = size / 4;
	*decoded_size = groups * 3 - (size_t)pad_count;

	/* Succeeded: the input is well formed. */
	return 0;
}
