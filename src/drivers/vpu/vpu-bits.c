/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The bit writer of the parameter set writers (see vpu-bits.h).
 *
 * Procedure referenced: ITU-T H.264 (08/2021) 7.2 and 9.1, and ITU-T
 * H.265 (V11) 7.2 and 9.2 (u(n), ue(v), se(v)); 7.3.2.11 of each
 * (rbsp_trailing_bits); H.264 7.4.1 and H.265 7.4.2 (emulation
 * prevention).
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/vpu/vpu-bits.h"

/* Starts writing into a zeroed buffer. */
void
vpu_bits_start(
	struct vpu_bits *bits,
	uint8_t *data,
	size_t capacity)
{
	/* The buffer, zeroed, and the first bit. */
	kern_memset(data, 0, capacity);
	bits->data = data;
	bits->capacity_bits = capacity * 8U;
	bits->position = 0;
	bits->error = 0;
}

/* Writes up to 32 bits, most significant first. */
void
vpu_bits_put(
	struct vpu_bits *bits,
	uint32_t value,
	unsigned count)
{
	unsigned index;
	uint32_t bit;

	/* A write past the capacity sets the error and writes nothing. */
	if (bits->error != 0 || bits->position + count > bits->capacity_bits) {
		bits->error = 1;
		return;
	}

	/* One bit at a time into the zeroed buffer. */
	for (index = 0; index < count; index++) {
		bit = (value >> (count - 1U - index)) & 1U;
		if (bit != 0)
			bits->data[bits->position / 8U] |= (uint8_t)(0x80U >> (bits->position % 8U));
		bits->position++;
	}
}

/* Writes one flag bit: whether flags has flag set. */
void
vpu_bits_flag(
	struct vpu_bits *bits,
	uint32_t flags,
	uint32_t flag)
{
	uint32_t bit;

	/* The flag as one bit. */
	bit = 0;
	if ((flags & flag) != 0)
		bit = 1;
	vpu_bits_put(bits, bit, 1);
}

/* Writes an unsigned Exp-Golomb code. */
void
vpu_bits_ue(
	struct vpu_bits *bits,
	uint32_t value)
{
	uint64_t code;
	unsigned length;

	/* value + 1 in binary, after as many zeros as it has bits less one. */
	code = (uint64_t)value + 1U;
	length = 0;
	while ((code >> length) > 1U)
		length++;
	vpu_bits_put(bits, 0, length);
	if (length >= 32U) {
		/* value 0xffffffff: a one, then 32 zero bits. */
		vpu_bits_put(bits, 1, 1);
		vpu_bits_put(bits, 0, 32);
		return;
	}
	vpu_bits_put(bits, (uint32_t)code, length + 1U);
}

/* Writes a signed Exp-Golomb code. */
void
vpu_bits_se(
	struct vpu_bits *bits,
	int32_t value)
{
	/* Positive values take the odd codes, the others the even ones. */
	if (value > 0) {
		vpu_bits_ue(bits, (uint32_t)value * 2U - 1U);
	} else {
		vpu_bits_ue(bits, (uint32_t)(-(int64_t)value) * 2U);
	}
}

/* Writes rbsp_trailing_bits: a one, then zeros to the byte boundary. */
void
vpu_bits_trailing(
	struct vpu_bits *bits)
{
	/* The stop bit and the alignment. */
	vpu_bits_put(bits, 1, 1);
	while ((bits->position % 8U) != 0 && bits->error == 0)
		vpu_bits_put(bits, 0, 1);
}

/*
 * Writes a start code, the header bytes and an RBSP with emulation
 * prevention.
 */
int
vpu_bits_nal(
	uint32_t header,
	unsigned header_bytes,
	const uint8_t *rbsp,
	size_t bytes,
	uint8_t *output,
	size_t capacity,
	size_t *written)
{
	size_t index;
	size_t zeros;
	size_t out;
	unsigned byte;

	/* The four-byte start code and the header, most significant byte first. */
	if (header_bytes == 0 || header_bytes > 2U)
		return EINVAL;
	if (capacity < 4U + header_bytes)
		return ENOSPC;
	output[0] = 0;
	output[1] = 0;
	output[2] = 0;
	output[3] = 1;
	out = 4U;
	for (byte = header_bytes; byte > 0; byte--) {
		output[out] = (uint8_t)(header >> ((byte - 1U) * 8U));
		out++;
	}

	/* The body, escaped. */
	zeros = 0;
	for (index = 0; index < bytes; index++) {
		if (zeros >= 2U && rbsp[index] <= 3U) {
			if (out >= capacity)
				return ENOSPC;
			output[out] = 3;
			out++;
			zeros = 0;
		}

		/* Refuses an output that does not fit. */
		if (out >= capacity)
			return ENOSPC;
		output[out] = rbsp[index];
		out++;
		if (rbsp[index] == 0) {
			zeros++;
		} else {
			zeros = 0;
		}
	}

	/* Succeeded: output holds the unit. */
	*written = out;
	return 0;
}
