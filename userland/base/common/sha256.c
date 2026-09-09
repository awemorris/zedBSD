/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Independent userland copy of zedBSD's SHA-256 implementation. */
#include "sha256.h"
#include <errno.h>
#include <string.h>

static const uint32_t command_sha256_constants[64] = {
	0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
	0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
	0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
	0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
	0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
	0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
	0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
	0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
	0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
	0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
	0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
	0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
	0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

/* Performs the rotate right operation. */
static uint32_t
sha256_rotate_right(
	uint32_t value,
	unsigned amount)
{
	/* Returns the computed result. */
	return (value >> amount) | (value << (32U - amount));
}

/* Performs the get be32 operation. */
static uint32_t
sha256_get_be32(
	const uint8_t *bytes)
{
	/* Returns the computed result. */
	return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
	       ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

/* Performs the put be32 operation. */
static void
sha256_put_be32(
	uint8_t *bytes,
	uint32_t value)
{
	bytes[0] = (uint8_t)(value >> 24);
	bytes[1] = (uint8_t)(value >> 16);
	bytes[2] = (uint8_t)(value >> 8);
	bytes[3] = (uint8_t)value;
}

/* Performs the sha256 transform operation. */
static void
command_sha256_transform(
	struct command_sha256_context *context,
	const uint8_t block[64])
{
	uint32_t s0_local, s1_local;
	uint32_t choose_local, majority_local, s0_local1, s1_local2,
		temporary1_local, temporary2_local;
	uint32_t schedule[64];
	uint32_t a, b, c, d, e, f, g, h;
	unsigned index;

	/* Process each remaining element. */
	for (index = 0; index < 16U; index++)
		schedule[index] = sha256_get_be32(block + index * 4U);
	/* Process each remaining element. */
	for (; index < 64U; index++) {
		s0_local = sha256_rotate_right(schedule[index - 15U], 7U) ^
			   sha256_rotate_right(schedule[index - 15U], 18U) ^
			   (schedule[index - 15U] >> 3);
		s1_local = sha256_rotate_right(schedule[index - 2U], 17U) ^
			   sha256_rotate_right(schedule[index - 2U], 19U) ^
			   (schedule[index - 2U] >> 10);
		schedule[index] = schedule[index - 16U] + s0_local +
				  schedule[index - 7U] + s1_local;
	}

	a = context->state[0];
	b = context->state[1];
	c = context->state[2];
	d = context->state[3];
	e = context->state[4];
	f = context->state[5];
	g = context->state[6];
	h = context->state[7];
	/* Process each remaining element. */
	for (index = 0; index < 64U; index++) {
		s1_local2 = sha256_rotate_right(e, 6U) ^
			    sha256_rotate_right(e, 11U) ^
			    sha256_rotate_right(e, 25U);
		choose_local = (e & f) ^ (~e & g);
		temporary1_local = h + s1_local2 + choose_local +
				   command_sha256_constants[index] +
				   schedule[index];
		s0_local1 = sha256_rotate_right(a, 2U) ^
			    sha256_rotate_right(a, 13U) ^
			    sha256_rotate_right(a, 22U);
		majority_local = (a & b) ^ (a & c) ^ (b & c);
		temporary2_local = s0_local1 + majority_local;
		h = g;
		g = f;
		f = e;
		e = d + temporary1_local;
		d = c;
		c = b;
		b = a;
		a = temporary1_local + temporary2_local;
	}

	context->state[0] += a;
	context->state[1] += b;
	context->state[2] += c;
	context->state[3] += d;
	context->state[4] += e;
	context->state[5] += f;
	context->state[6] += g;
	context->state[7] += h;
	memset(schedule, 0, sizeof(schedule));
}

/* Performs the sha256 init operation. */
void
command_sha256_init(
	struct command_sha256_context *context)
{
	static const uint32_t initial[8] = {
		0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
		0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

	memset(context, 0, sizeof(*context));
	memcpy(context->state, initial, sizeof(initial));
}

/* Performs the sha256 update operation. */
int
command_sha256_update(
	struct command_sha256_context *context,
	const uint8_t *bytes,
	size_t length)
{
	size_t available;
	size_t amount;
	size_t remaining = length;

	/* Handles the bytes availability. */
	if (length != 0U && bytes == NULL)
		return EINVAL;

	/* Handles the uint64 t condition. */
	if ((uint64_t)length > UINT64_MAX / 8U - context->length)
		return EOVERFLOW;
	context->length += (uint64_t)length;
	/* Continue while the operation condition remains true. */
	while (remaining != 0U) {
		available = sizeof(context->block) - context->used;
		amount = remaining < available ? remaining : available;

		memcpy(context->block + context->used, bytes, amount);
		context->used += amount;
		bytes += amount;
		remaining -= amount;

		/* Handles the context condition. */
		if (context->used == sizeof(context->block)) {
			command_sha256_transform(context, context->block);
			context->used = 0U;
		}
	}

	/* Succeeded. */
	return 0;
}

/* Performs the sha256 final operation. */
void
command_sha256_final(
	struct command_sha256_context *context,
	uint8_t digest[32])
{
	uint64_t bit_length = context->length * 8U;
	unsigned index;

	context->block[context->used++] = 0x80U;

	/* Handles the context condition. */
	if (context->used > 56U) {
		memset(context->block + context->used, 0,
		       sizeof(context->block) - context->used);
		command_sha256_transform(context, context->block);
		context->used = 0U;
	}

	memset(context->block + context->used, 0, 56U - context->used);
	/* Process each remaining element. */
	for (index = 0; index < 8U; index++) {
		context->block[63U - index] =
			(uint8_t)(bit_length >> (index * 8U));
	}

	command_sha256_transform(context, context->block);
	/* Process each remaining element. */
	for (index = 0; index < 8U; index++)
		sha256_put_be32(digest + index * 4U, context->state[index]);
	memset(context, 0, sizeof(*context));
}

