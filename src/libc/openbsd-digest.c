/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements MD5 and SHA-1, from RFC 1321 and FIPS 180-4.
 *
 * Both work the same way: the message is padded to a whole number of
 * blocks, with its length written into the tail, and each block stirs a
 * fixed-size state.  What differs is the word order in the block, the
 * mixing function, and the constants.
 */

#include <md5.h>
#include <sha1.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* The additive constants of MD5: the integer parts of 2^32 |sin i|. */
static const uint32_t md5_table[64] = {
	0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU,
	0xf57c0fafU, 0x4787c62aU, 0xa8304613U, 0xfd469501U,
	0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU,
	0x6b901122U, 0xfd987193U, 0xa679438eU, 0x49b40821U,
	0xf61e2562U, 0xc040b340U, 0x265e5a51U, 0xe9b6c7aaU,
	0xd62f105dU, 0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U,
	0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU,
	0xa9e3e905U, 0xfcefa3f8U, 0x676f02d9U, 0x8d2a4c8aU,
	0xfffa3942U, 0x8771f681U, 0x6d9d6122U, 0xfde5380cU,
	0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U,
	0x289b7ec6U, 0xeaa127faU, 0xd4ef3085U, 0x04881d05U,
	0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U,
	0xf4292244U, 0x432aff97U, 0xab9423a7U, 0xfc93a039U,
	0x655b59c3U, 0x8f0ccc92U, 0xffeff47dU, 0x85845dd1U,
	0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U,
	0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU, 0xeb86d391U
};

/* The rotation amounts, four per group of sixteen rounds. */
static const unsigned md5_shift[64] = {
	7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
	5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
	4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
	6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

/* Supports the rotate left operation. */
static uint32_t
rotate32(
	uint32_t value,
	unsigned count)
{
	/* Returns the computed result. */
	return (uint32_t)((value << count) | (value >> (32U - count)));
}

/* Supports the hex string operation, shared by every End and Data call. */
static char *
hex_string(
	char *out,
	const uint8_t *digest,
	size_t length)
{
	static const char digits[] = "0123456789abcdef";
	size_t index;

	/* Handles the out availability. */
	if (out == NULL)
		return NULL;

	/* Process each remaining element. */
	for (index = 0; index < length; index++) {
		out[index * 2U] = digits[(digest[index] >> 4) & 15U];
		out[index * 2U + 1U] = digits[digest[index] & 15U];
	}
	out[length * 2U] = '\0';

	/* Returns the computed result. */
	return out;
}

/*
 * Supports the md5 block operation.
 *
 * Stirs the state with one 64-byte block, whose words are read with the
 * least significant byte first.
 */
static void
md5_block(
	uint32_t state[4],
	const uint8_t *block)
{
	uint32_t words[16];
	uint32_t a, b, c, d, f, temporary;
	unsigned index;
	unsigned g;

	/* Process each remaining element. */
	for (index = 0; index < 16U; index++) {
		words[index] = (uint32_t)block[index * 4U] |
			       ((uint32_t)block[index * 4U + 1U] << 8) |
			       ((uint32_t)block[index * 4U + 2U] << 16) |
			       ((uint32_t)block[index * 4U + 3U] << 24);
	}
	a = state[0];
	b = state[1];
	c = state[2];
	d = state[3];

	/* Process each remaining element. */
	for (index = 0; index < 64U; index++) {
		/* Dispatch the selected quarter of the rounds. */
		if (index < 16U) {
			f = (b & c) | (~b & d);
			g = index;
		} else if (index < 32U) {
			f = (d & b) | (~d & c);
			g = (5U * index + 1U) & 15U;
		} else if (index < 48U) {
			f = b ^ c ^ d;
			g = (3U * index + 5U) & 15U;
		} else {
			f = c ^ (b | ~d);
			g = (7U * index) & 15U;
		}
		temporary = d;
		d = c;
		c = b;
		b = b + rotate32(a + f + md5_table[index] + words[g],
				 md5_shift[index]);
		a = temporary;
	}
	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
}

/*
 * Implements the MD5Init operation.
 */
void
MD5Init(
	MD5_CTX *context)
{
	context->state[0] = 0x67452301U;
	context->state[1] = 0xefcdab89U;
	context->state[2] = 0x98badcfeU;
	context->state[3] = 0x10325476U;
	context->count = 0;
}

/*
 * Implements the MD5Update operation.
 */
void
MD5Update(
	MD5_CTX *context,
	const uint8_t *data,
	size_t length)
{
	size_t held;
	size_t room;

	held = (size_t)(context->count % MD5_BLOCK_LENGTH);
	context->count += length;

	/* Completes the block that was left part-filled. */
	if (held != 0U) {
		room = MD5_BLOCK_LENGTH - held;
		if (length < room) {
			memcpy(context->buffer + held, data, length);
			return;
		}
		memcpy(context->buffer + held, data, room);
		md5_block(context->state, context->buffer);
		data += room;
		length -= room;
	}

	/* Continue while the operation condition remains true. */
	while (length >= MD5_BLOCK_LENGTH) {
		md5_block(context->state, data);
		data += MD5_BLOCK_LENGTH;
		length -= MD5_BLOCK_LENGTH;
	}

	/* Keeps what is left for the next call, or for the padding. */
	if (length != 0U)
		memcpy(context->buffer, data, length);
}

/*
 * Implements the MD5Final operation.
 *
 * The message is closed with a single one bit, then zeros, then its length
 * in bits, so that two messages differing only in trailing zeros cannot
 * pad to the same blocks.
 */
void
MD5Final(
	uint8_t digest[MD5_DIGEST_LENGTH],
	MD5_CTX *context)
{
	uint8_t tail[MD5_BLOCK_LENGTH * 2U];
	uint64_t bits;
	size_t held;
	size_t padding;
	unsigned index;

	bits = context->count * 8U;
	held = (size_t)(context->count % MD5_BLOCK_LENGTH);
	memcpy(tail, context->buffer, held);
	tail[held] = 0x80U;
	padding = held + 1U;

	/* Pads to eight bytes short of a block, for the length. */
	while ((padding % MD5_BLOCK_LENGTH) != MD5_BLOCK_LENGTH - 8U)
		tail[padding++] = 0;

	/* The length goes in with its least significant byte first. */
	for (index = 0; index < 8U; index++)
		tail[padding++] = (uint8_t)((bits >> (index * 8U)) & 0xffU);
	for (index = 0; index < padding; index += MD5_BLOCK_LENGTH)
		md5_block(context->state, tail + index);

	/* Handles the digest availability. */
	if (digest != NULL) {
		for (index = 0; index < MD5_DIGEST_LENGTH; index++) {
			digest[index] = (uint8_t)((context->state[index / 4U] >>
			    ((index % 4U) * 8U)) & 0xffU);
		}
	}

	/* The state is a secret in a keyed construction; it is erased. */
	explicit_bzero(tail, sizeof(tail));
	explicit_bzero(context, sizeof(*context));
}

/*
 * Implements the MD5End operation.
 */
char *
MD5End(
	MD5_CTX *context,
	char *out)
{
	uint8_t digest[MD5_DIGEST_LENGTH];

	MD5Final(digest, context);

	/* Returns the computed result. */
	return hex_string(out, digest, sizeof(digest));
}

/*
 * Implements the MD5Data operation.
 */
char *
MD5Data(
	const uint8_t *data,
	size_t length,
	char *out)
{
	MD5_CTX context;

	MD5Init(&context);
	MD5Update(&context, data, length);

	/* Returns the computed result. */
	return MD5End(&context, out);
}

/*
 * Supports the sha1 block operation.
 *
 * Words are read with the most significant byte first, which is the one
 * visible difference from MD5 before the mixing begins.
 */
static void
sha1_block(
	uint32_t state[5],
	const uint8_t *block)
{
	uint32_t words[80];
	uint32_t a, b, c, d, e, f, k, temporary;
	unsigned index;

	/* Process each remaining element. */
	for (index = 0; index < 16U; index++) {
		words[index] = ((uint32_t)block[index * 4U] << 24) |
			       ((uint32_t)block[index * 4U + 1U] << 16) |
			       ((uint32_t)block[index * 4U + 2U] << 8) |
			       (uint32_t)block[index * 4U + 3U];
	}

	/* The block is stretched to eighty words before it is used. */
	for (index = 16U; index < 80U; index++) {
		words[index] = rotate32(words[index - 3U] ^ words[index - 8U] ^
					words[index - 14U] ^ words[index - 16U],
					1U);
	}
	a = state[0];
	b = state[1];
	c = state[2];
	d = state[3];
	e = state[4];

	/* Process each remaining element. */
	for (index = 0; index < 80U; index++) {
		/* Dispatch the selected quarter of the rounds. */
		if (index < 20U) {
			f = (b & c) | (~b & d);
			k = 0x5a827999U;
		} else if (index < 40U) {
			f = b ^ c ^ d;
			k = 0x6ed9eba1U;
		} else if (index < 60U) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8f1bbcdcU;
		} else {
			f = b ^ c ^ d;
			k = 0xca62c1d6U;
		}
		temporary = rotate32(a, 5U) + f + e + k + words[index];
		e = d;
		d = c;
		c = rotate32(b, 30U);
		b = a;
		a = temporary;
	}
	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
}

/*
 * Implements the SHA1Init operation.
 */
void
SHA1Init(
	SHA1_CTX *context)
{
	context->state[0] = 0x67452301U;
	context->state[1] = 0xefcdab89U;
	context->state[2] = 0x98badcfeU;
	context->state[3] = 0x10325476U;
	context->state[4] = 0xc3d2e1f0U;
	context->count = 0;
}

/*
 * Implements the SHA1Update operation.
 */
void
SHA1Update(
	SHA1_CTX *context,
	const uint8_t *data,
	size_t length)
{
	size_t held;
	size_t room;

	held = (size_t)(context->count % SHA1_BLOCK_LENGTH);
	context->count += length;

	/* Completes the block that was left part-filled. */
	if (held != 0U) {
		room = SHA1_BLOCK_LENGTH - held;
		if (length < room) {
			memcpy(context->buffer + held, data, length);
			return;
		}
		memcpy(context->buffer + held, data, room);
		sha1_block(context->state, context->buffer);
		data += room;
		length -= room;
	}

	/* Continue while the operation condition remains true. */
	while (length >= SHA1_BLOCK_LENGTH) {
		sha1_block(context->state, data);
		data += SHA1_BLOCK_LENGTH;
		length -= SHA1_BLOCK_LENGTH;
	}
	if (length != 0U)
		memcpy(context->buffer, data, length);
}

/*
 * Implements the SHA1Final operation.
 */
void
SHA1Final(
	uint8_t digest[SHA1_DIGEST_LENGTH],
	SHA1_CTX *context)
{
	uint8_t tail[SHA1_BLOCK_LENGTH * 2U];
	uint64_t bits;
	size_t held;
	size_t padding;
	unsigned index;

	bits = context->count * 8U;
	held = (size_t)(context->count % SHA1_BLOCK_LENGTH);
	memcpy(tail, context->buffer, held);
	tail[held] = 0x80U;
	padding = held + 1U;
	while ((padding % SHA1_BLOCK_LENGTH) != SHA1_BLOCK_LENGTH - 8U)
		tail[padding++] = 0;

	/* The length goes in with its most significant byte first. */
	for (index = 0; index < 8U; index++) {
		tail[padding++] = (uint8_t)((bits >> ((7U - index) * 8U)) &
					    0xffU);
	}
	for (index = 0; index < padding; index += SHA1_BLOCK_LENGTH)
		sha1_block(context->state, tail + index);

	/* Handles the digest availability. */
	if (digest != NULL) {
		for (index = 0; index < SHA1_DIGEST_LENGTH; index++) {
			digest[index] = (uint8_t)((context->state[index / 4U] >>
			    ((3U - (index % 4U)) * 8U)) & 0xffU);
		}
	}
	explicit_bzero(tail, sizeof(tail));
	explicit_bzero(context, sizeof(*context));
}

/*
 * Implements the SHA1End operation.
 */
char *
SHA1End(
	SHA1_CTX *context,
	char *out)
{
	uint8_t digest[SHA1_DIGEST_LENGTH];

	SHA1Final(digest, context);

	/* Returns the computed result. */
	return hex_string(out, digest, sizeof(digest));
}

/*
 * Implements the SHA1Data operation.
 */
char *
SHA1Data(
	const uint8_t *data,
	size_t length,
	char *out)
{
	SHA1_CTX context;

	SHA1Init(&context);
	SHA1Update(&context, data, length);

	/* Returns the computed result. */
	return SHA1End(&context, out);
}
