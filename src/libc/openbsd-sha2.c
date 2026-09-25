/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the SHA-2 family, from FIPS 180-4.
 *
 * Two constructions share one shape.  The 256-bit form works on 32-bit
 * words in 64-byte blocks; the 384- and 512-bit forms work on 64-bit words
 * in 128-byte blocks and differ from each other only in their starting
 * state and in how much of the result is reported.  Words are read and
 * written with the most significant byte first throughout.
 */

#include <sha2.h>
#include <string.h>
#include <strings.h>

/* The round constants: the fractional parts of the cube roots of primes. */
static const uint32_t sha256_k[64] = {
	0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
	0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
	0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
	0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
	0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
	0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
	0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
	0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
	0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
	0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
	0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
	0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
	0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
	0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
	0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
	0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static const uint64_t sha512_k[80] = {
	0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL,
	0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
	0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
	0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
	0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
	0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
	0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL,
	0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
	0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
	0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
	0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL,
	0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
	0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL,
	0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
	0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
	0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
	0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL,
	0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
	0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL,
	0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
	0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
	0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
	0xd192e819d6ef5218ULL, 0xd69906245565a910ULL,
	0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
	0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
	0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
	0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
	0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
	0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL,
	0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
	0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL,
	0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
	0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
	0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
	0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
	0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
	0x28db77f523047d84ULL, 0x32caab7b40c72493ULL,
	0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
	0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
	0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

#define ROR32(value, count) \
	((uint32_t)(((value) >> (count)) | ((value) << (32U - (count)))))
#define ROR64(value, count) \
	((uint64_t)(((value) >> (count)) | ((value) << (64U - (count)))))

/* Supports the hex string operation. */
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

/* Supports the sha256 block operation. */
static void
sha256_block(
	uint32_t state[8],
	const uint8_t *block)
{
	uint32_t words[64];
	uint32_t a, b, c, d, e, f, g, h;
	uint32_t first, second, choose, majority;
	unsigned index;

	/* Process each remaining element. */
	for (index = 0; index < 16U; index++) {
		words[index] = ((uint32_t)block[index * 4U] << 24) |
			       ((uint32_t)block[index * 4U + 1U] << 16) |
			       ((uint32_t)block[index * 4U + 2U] << 8) |
			       (uint32_t)block[index * 4U + 3U];
	}

	/* The block is stretched to sixty-four words. */
	for (index = 16U; index < 64U; index++) {
		first = ROR32(words[index - 15U], 7U) ^
			ROR32(words[index - 15U], 18U) ^
			(words[index - 15U] >> 3);
		second = ROR32(words[index - 2U], 17U) ^
			 ROR32(words[index - 2U], 19U) ^
			 (words[index - 2U] >> 10);
		words[index] = words[index - 16U] + first +
			       words[index - 7U] + second;
	}
	a = state[0]; b = state[1]; c = state[2]; d = state[3];
	e = state[4]; f = state[5]; g = state[6]; h = state[7];

	/* Process each remaining element. */
	for (index = 0; index < 64U; index++) {
		second = ROR32(e, 6U) ^ ROR32(e, 11U) ^ ROR32(e, 25U);
		choose = (e & f) ^ (~e & g);
		first = h + second + choose + sha256_k[index] + words[index];
		second = ROR32(a, 2U) ^ ROR32(a, 13U) ^ ROR32(a, 22U);
		majority = (a & b) ^ (a & c) ^ (b & c);
		h = g; g = f; f = e;
		e = d + first;
		d = c; c = b; b = a;
		a = first + second + majority;
	}
	state[0] += a; state[1] += b; state[2] += c; state[3] += d;
	state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

/* Supports the sha512 block operation. */
static void
sha512_block(
	uint64_t state[8],
	const uint8_t *block)
{
	uint64_t words[80];
	uint64_t a, b, c, d, e, f, g, h;
	uint64_t first, second, choose, majority;
	unsigned index;
	unsigned byte;

	/* Process each remaining element. */
	for (index = 0; index < 16U; index++) {
		words[index] = 0;
		for (byte = 0; byte < 8U; byte++) {
			words[index] = (words[index] << 8) |
				       (uint64_t)block[index * 8U + byte];
		}
	}

	/* The block is stretched to eighty words. */
	for (index = 16U; index < 80U; index++) {
		first = ROR64(words[index - 15U], 1U) ^
			ROR64(words[index - 15U], 8U) ^
			(words[index - 15U] >> 7);
		second = ROR64(words[index - 2U], 19U) ^
			 ROR64(words[index - 2U], 61U) ^
			 (words[index - 2U] >> 6);
		words[index] = words[index - 16U] + first +
			       words[index - 7U] + second;
	}
	a = state[0]; b = state[1]; c = state[2]; d = state[3];
	e = state[4]; f = state[5]; g = state[6]; h = state[7];

	/* Process each remaining element. */
	for (index = 0; index < 80U; index++) {
		second = ROR64(e, 14U) ^ ROR64(e, 18U) ^ ROR64(e, 41U);
		choose = (e & f) ^ (~e & g);
		first = h + second + choose + sha512_k[index] + words[index];
		second = ROR64(a, 28U) ^ ROR64(a, 34U) ^ ROR64(a, 39U);
		majority = (a & b) ^ (a & c) ^ (b & c);
		h = g; g = f; f = e;
		e = d + first;
		d = c; c = b; b = a;
		a = first + second + majority;
	}
	state[0] += a; state[1] += b; state[2] += c; state[3] += d;
	state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

/*
 * Supports the sha2 update operation.
 *
 * The block size says which of the two constructions is running, so one
 * routine feeds both.
 */
static void
sha2_update(
	SHA2_CTX *context,
	const uint8_t *data,
	size_t length,
	size_t block_length)
{
	size_t held;
	size_t room;

	held = (size_t)(context->count[0] % block_length);
	context->count[0] += length;

	/* Completes the block that was left part-filled. */
	if (held != 0U) {
		room = block_length - held;
		if (length < room) {
			memcpy(context->buffer + held, data, length);
			return;
		}
		memcpy(context->buffer + held, data, room);
		if (block_length == SHA256_BLOCK_LENGTH)
			sha256_block(context->state.state32, context->buffer);
		else
			sha512_block(context->state.state64, context->buffer);
		data += room;
		length -= room;
	}

	/* Continue while the operation condition remains true. */
	while (length >= block_length) {
		if (block_length == SHA256_BLOCK_LENGTH)
			sha256_block(context->state.state32, data);
		else
			sha512_block(context->state.state64, data);
		data += block_length;
		length -= block_length;
	}
	if (length != 0U)
		memcpy(context->buffer, data, length);
}

/*
 * Supports the sha2 final operation.
 *
 * The 512-bit forms record the length in sixteen bytes rather than eight,
 * which is the only difference in how the message is closed.
 */
static void
sha2_final(
	uint8_t *digest,
	SHA2_CTX *context,
	size_t block_length,
	size_t digest_length)
{
	uint8_t tail[SHA512_BLOCK_LENGTH * 2U];
	uint64_t bits;
	size_t length_bytes;
	size_t held;
	size_t padding;
	unsigned index;
	unsigned byte;

	bits = context->count[0] * 8U;
	length_bytes = block_length == SHA256_BLOCK_LENGTH ? 8U : 16U;
	held = (size_t)(context->count[0] % block_length);
	memcpy(tail, context->buffer, held);
	tail[held] = 0x80U;
	padding = held + 1U;

	/* Pads to where the length is written. */
	while ((padding % block_length) != block_length - length_bytes)
		tail[padding++] = 0;
	for (index = 0; index < length_bytes - 8U; index++)
		tail[padding++] = 0;
	for (index = 0; index < 8U; index++) {
		tail[padding++] = (uint8_t)((bits >> ((7U - index) * 8U)) &
					    0xffU);
	}
	for (index = 0; index < padding; index += block_length) {
		if (block_length == SHA256_BLOCK_LENGTH)
			sha256_block(context->state.state32, tail + index);
		else
			sha512_block(context->state.state64, tail + index);
	}

	/* Handles the digest availability. */
	if (digest != NULL) {
		if (block_length == SHA256_BLOCK_LENGTH) {
			for (index = 0; index < digest_length; index++) {
				digest[index] = (uint8_t)
				    ((context->state.state32[index / 4U] >>
				    ((3U - (index % 4U)) * 8U)) & 0xffU);
			}
		} else {
			for (index = 0; index < digest_length; index++) {
				byte = index % 8U;
				digest[index] = (uint8_t)
				    ((context->state.state64[index / 8U] >>
				    ((7U - byte) * 8U)) & 0xffU);
			}
		}
	}
	explicit_bzero(tail, sizeof(tail));
	explicit_bzero(context, sizeof(*context));
}

/*
 * Implements the SHA256Init operation.
 */
void
SHA256Init(
	SHA2_CTX *context)
{
	context->state.state32[0] = 0x6a09e667U;
	context->state.state32[1] = 0xbb67ae85U;
	context->state.state32[2] = 0x3c6ef372U;
	context->state.state32[3] = 0xa54ff53aU;
	context->state.state32[4] = 0x510e527fU;
	context->state.state32[5] = 0x9b05688cU;
	context->state.state32[6] = 0x1f83d9abU;
	context->state.state32[7] = 0x5be0cd19U;
	context->count[0] = 0;
	context->count[1] = 0;
}

/* Implements the SHA256Update operation. */
void
SHA256Update(SHA2_CTX *context, const uint8_t *data, size_t length)
{
	sha2_update(context, data, length, SHA256_BLOCK_LENGTH);
}

/* Implements the SHA256Final operation. */
void
SHA256Final(uint8_t digest[SHA256_DIGEST_LENGTH], SHA2_CTX *context)
{
	sha2_final(digest, context, SHA256_BLOCK_LENGTH, SHA256_DIGEST_LENGTH);
}

/* Implements the SHA256End operation. */
char *
SHA256End(SHA2_CTX *context, char *out)
{
	uint8_t digest[SHA256_DIGEST_LENGTH];

	SHA256Final(digest, context);
	return hex_string(out, digest, sizeof(digest));
}

/* Implements the SHA256Data operation. */
char *
SHA256Data(const uint8_t *data, size_t length, char *out)
{
	SHA2_CTX context;

	SHA256Init(&context);
	SHA256Update(&context, data, length);
	return SHA256End(&context, out);
}

/*
 * Implements the SHA384Init operation.
 *
 * The starting state is the only thing that separates this from SHA-512,
 * besides reporting the first forty-eight bytes of the result.  Without a
 * different start the shorter digest would simply be a prefix of the
 * longer one.
 */
void
SHA384Init(
	SHA2_CTX *context)
{
	context->state.state64[0] = 0xcbbb9d5dc1059ed8ULL;
	context->state.state64[1] = 0x629a292a367cd507ULL;
	context->state.state64[2] = 0x9159015a3070dd17ULL;
	context->state.state64[3] = 0x152fecd8f70e5939ULL;
	context->state.state64[4] = 0x67332667ffc00b31ULL;
	context->state.state64[5] = 0x8eb44a8768581511ULL;
	context->state.state64[6] = 0xdb0c2e0d64f98fa7ULL;
	context->state.state64[7] = 0x47b5481dbefa4fa4ULL;
	context->count[0] = 0;
	context->count[1] = 0;
}

/* Implements the SHA384Update operation. */
void
SHA384Update(SHA2_CTX *context, const uint8_t *data, size_t length)
{
	sha2_update(context, data, length, SHA384_BLOCK_LENGTH);
}

/* Implements the SHA384Final operation. */
void
SHA384Final(uint8_t digest[SHA384_DIGEST_LENGTH], SHA2_CTX *context)
{
	sha2_final(digest, context, SHA384_BLOCK_LENGTH, SHA384_DIGEST_LENGTH);
}

/* Implements the SHA384End operation. */
char *
SHA384End(SHA2_CTX *context, char *out)
{
	uint8_t digest[SHA384_DIGEST_LENGTH];

	SHA384Final(digest, context);
	return hex_string(out, digest, sizeof(digest));
}

/* Implements the SHA384Data operation. */
char *
SHA384Data(const uint8_t *data, size_t length, char *out)
{
	SHA2_CTX context;

	SHA384Init(&context);
	SHA384Update(&context, data, length);
	return SHA384End(&context, out);
}

/* Implements the SHA512Init operation. */
void
SHA512Init(
	SHA2_CTX *context)
{
	context->state.state64[0] = 0x6a09e667f3bcc908ULL;
	context->state.state64[1] = 0xbb67ae8584caa73bULL;
	context->state.state64[2] = 0x3c6ef372fe94f82bULL;
	context->state.state64[3] = 0xa54ff53a5f1d36f1ULL;
	context->state.state64[4] = 0x510e527fade682d1ULL;
	context->state.state64[5] = 0x9b05688c2b3e6c1fULL;
	context->state.state64[6] = 0x1f83d9abfb41bd6bULL;
	context->state.state64[7] = 0x5be0cd19137e2179ULL;
	context->count[0] = 0;
	context->count[1] = 0;
}

/* Implements the SHA512Update operation. */
void
SHA512Update(SHA2_CTX *context, const uint8_t *data, size_t length)
{
	sha2_update(context, data, length, SHA512_BLOCK_LENGTH);
}

/* Implements the SHA512Final operation. */
void
SHA512Final(uint8_t digest[SHA512_DIGEST_LENGTH], SHA2_CTX *context)
{
	sha2_final(digest, context, SHA512_BLOCK_LENGTH, SHA512_DIGEST_LENGTH);
}

/* Implements the SHA512End operation. */
char *
SHA512End(SHA2_CTX *context, char *out)
{
	uint8_t digest[SHA512_DIGEST_LENGTH];

	SHA512Final(digest, context);
	return hex_string(out, digest, sizeof(digest));
}

/* Implements the SHA512Data operation. */
char *
SHA512Data(const uint8_t *data, size_t length, char *out)
{
	SHA2_CTX context;

	SHA512Init(&context);
	SHA512Update(&context, data, length);
	return SHA512End(&context, out);
}
