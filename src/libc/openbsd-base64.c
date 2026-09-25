/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements base 64, as described by RFC 4648.
 *
 * Three bytes become four characters, each carrying six bits.  A final
 * group of one or two bytes is padded with '=' so that the length of the
 * encoding always says how many bytes it came from.
 */

#include <ctype.h>
#include <resolv.h>
#include <string.h>

static const char alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/*
 * Supports the alphabet value operation.
 *
 * Reports the six bits a character stands for, or -1 when it stands for
 * nothing in this alphabet.
 */
static int
alphabet_value(
	int c)
{
	const char *at;

	/* Handles the padding, which carries no bits. */
	if (c == '=')
		return -2;
	at = memchr(alphabet, c, sizeof(alphabet) - 1U);

	/* Returns the computed result. */
	return at != NULL ? (int)(at - alphabet) : -1;
}

/*
 * Implements the b64 ntop operation.
 */
int
b64_ntop(
	const unsigned char *source,
	size_t length,
	char *target,
	size_t size)
{
	size_t produced;
	size_t index;
	unsigned group;
	unsigned remaining;

	/* Handles the arguments availability. */
	if (source == NULL || target == NULL)
		return -1;
	produced = 0;
	index = 0;

	/* Process each remaining element. */
	while (length - index >= 3U) {
		group = ((unsigned)source[index] << 16) |
			((unsigned)source[index + 1U] << 8) |
			(unsigned)source[index + 2U];

		/* Handles an encoding that does not fit. */
		if (produced + 4U >= size)
			return -1;
		target[produced++] = alphabet[(group >> 18) & 63U];
		target[produced++] = alphabet[(group >> 12) & 63U];
		target[produced++] = alphabet[(group >> 6) & 63U];
		target[produced++] = alphabet[group & 63U];
		index += 3U;
	}

	/* The last one or two bytes, padded so the length still speaks. */
	remaining = (unsigned)(length - index);
	if (remaining != 0U) {
		group = (unsigned)source[index] << 16;
		if (remaining == 2U)
			group |= (unsigned)source[index + 1U] << 8;

		/* Handles an encoding that does not fit. */
		if (produced + 4U >= size)
			return -1;
		target[produced++] = alphabet[(group >> 18) & 63U];
		target[produced++] = alphabet[(group >> 12) & 63U];
		target[produced++] = remaining == 2U ?
				     alphabet[(group >> 6) & 63U] : '=';
		target[produced++] = '=';
	}

	/* Handles a terminator that does not fit. */
	if (produced >= size)
		return -1;
	target[produced] = '\0';

	/* Returns the computed result. */
	return (int)produced;
}

/*
 * Implements the b64 pton operation.
 *
 * Whitespace between characters is ignored, as it is in the records this
 * encoding is carried in.  Anything else that is not part of the alphabet,
 * and any padding that does not end the text, is refused: a decoder that
 * accepted them would let two different texts mean the same bytes.
 */
int
b64_pton(
	const char *source,
	unsigned char *target,
	size_t size)
{
	size_t produced;
	unsigned group;
	unsigned held;
	unsigned padding;
	int value;

	/* Handles the arguments availability. */
	if (source == NULL)
		return -1;
	produced = 0;
	group = 0;
	held = 0;
	padding = 0;

	/* Process each element required by the operation. */
	for (; *source != '\0'; source++) {
		/* Skips the whitespace a record may be wrapped with. */
		if (isspace((unsigned char)*source))
			continue;
		value = alphabet_value((unsigned char)*source);

		/* Handles a character that stands for nothing. */
		if (value == -1)
			return -1;

		/* Counts the padding, which may only end the text. */
		if (value == -2) {
			padding++;
			if (padding > 2U || held == 0U || held == 1U)
				return -1;
			continue;
		}

		/* Rejects a character after the padding has begun. */
		if (padding != 0U)
			return -1;
		group = (group << 6) | (unsigned)value;
		held++;

		/* Four characters make three bytes. */
		if (held == 4U) {
			if (target != NULL && produced + 3U > size)
				return -1;
			if (target != NULL) {
				target[produced] =
				    (unsigned char)((group >> 16) & 0xffU);
				target[produced + 1U] =
				    (unsigned char)((group >> 8) & 0xffU);
				target[produced + 2U] =
				    (unsigned char)(group & 0xffU);
			}
			produced += 3U;
			group = 0;
			held = 0;
		}
	}

	/* A group of two or three characters ends in one or two bytes. */
	if (held == 2U) {
		/* The bits below the byte must be zero, or text was lost. */
		if ((group & 15U) != 0U)
			return -1;
		if (target != NULL && produced + 1U > size)
			return -1;
		if (target != NULL)
			target[produced] =
			    (unsigned char)((group >> 4) & 0xffU);
		produced += 1U;
	} else if (held == 3U) {
		if ((group & 3U) != 0U)
			return -1;
		if (target != NULL && produced + 2U > size)
			return -1;
		if (target != NULL) {
			target[produced] =
			    (unsigned char)((group >> 10) & 0xffU);
			target[produced + 1U] =
			    (unsigned char)((group >> 2) & 0xffU);
		}
		produced += 2U;
	} else if (held != 0U) {
		/* One character alone carries too few bits to be a byte. */
		return -1;
	}

	/* Returns the computed result. */
	return (int)produced;
}
