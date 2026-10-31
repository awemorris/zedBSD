/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The helpers every part uses: wiping, constant-time comparison and
 * selection, byte order.
 */

#include "cryc.h"

/*
 * Overwrites size bytes at memory with zeros in a way the compiler does
 * not remove (the stores go through a volatile pointer).
 */
void
cryc_wipe(
	void *memory,
	size_t size)
{
	volatile uint8_t *target;
	size_t index;

	/* A caller that passes no memory has nothing to wipe. */
	if (memory == NULL)
		return;

	/*
	 * Every store goes through a volatile pointer, so the compiler must
	 * keep it even when the memory is never read again.
	 */
	target = (volatile uint8_t *)memory;
	for (index = 0; index < size; index++)
		target[index] = 0;
}

/*
 * Compares size bytes of left and right in constant time.
 *
 * Returns 1 when they are equal and 0 otherwise.
 */
int
cryc_ct_equal(
	const uint8_t *left,
	const uint8_t *right,
	size_t size)
{
	uint32_t difference;
	uint32_t equal;
	size_t index;

	/* Nothing to compare: the empty strings are equal. */
	if (size == 0)
		return 1;

	/* A missing buffer cannot be equal to anything. */
	if (left == NULL || right == NULL)
		return 0;

	/*
	 * Folds every byte-wise difference into one value, so that the loop
	 * visits every byte whether or not an earlier byte differed.
	 */
	difference = 0;
	for (index = 0; index < size; index++)
		difference |= (uint32_t)(left[index] ^ right[index]);

	/*
	 * Turns "difference is zero" into 1 and "difference is 1 to 255" into
	 * 0 with arithmetic only: subtracting one borrows into the bits above
	 * the low byte exactly when difference is zero.
	 */
	equal = ((difference - 1U) >> 8) & 1U;

	/* Reports 1 for equal and 0 for different. */
	return (int)equal;
}

/*
 * Returns 0xffffffff when value is 0 and 0 otherwise, in constant time.
 */
uint32_t
cryc_ct_mask_zero32(
	uint32_t value)
{
	uint32_t nonzero;

	/*
	 * Either value or its two's complement negation has the top bit set
	 * when value is not zero; for zero both are zero.
	 */
	nonzero = (value | (0U - value)) >> 31;

	/* Reports all ones for zero (0 - 1 wraps) and all zeros otherwise. */
	return nonzero - 1U;
}

/*
 * Returns 0xffffffff when left equals right and 0 otherwise, in constant
 * time.
 */
uint32_t
cryc_ct_mask_eq32(
	uint32_t left,
	uint32_t right)
{
	uint32_t difference;
	uint32_t mask;

	/* The values are equal exactly when their bitwise difference is zero. */
	difference = left ^ right;

	/* Turns "difference is zero" into the mask. */
	mask = cryc_ct_mask_zero32(difference);

	/* Reports the mask of equality. */
	return mask;
}

/*
 * Returns 0xffffffff when left is less than right (unsigned) and 0
 * otherwise, in constant time.
 */
uint32_t
cryc_ct_mask_lt32(
	uint32_t left,
	uint32_t right)
{
	uint64_t difference;
	uint32_t borrow;

	/*
	 * Subtracts in 64 bits.  The high word of the result is all ones when
	 * the subtraction borrowed (left is less than right) and zero when it
	 * did not.
	 */
	difference = (uint64_t)left - (uint64_t)right;
	borrow = (uint32_t)(difference >> 32);

	/* Reports the borrow word, which is already the wanted mask. */
	return borrow;
}

/*
 * Copies size bytes into out: from when_set where mask is 0xffffffff, from
 * when_clear where mask is 0; in constant time.
 */
void
cryc_ct_select_bytes(
	uint8_t *out,
	const uint8_t *when_set,
	const uint8_t *when_clear,
	size_t size,
	uint32_t mask)
{
	uint8_t byte_mask;
	uint8_t chosen;
	size_t index;

	/* With no bytes there is nothing to do. */
	if (size == 0)
		return;

	/* Missing buffers are a caller's mistake that must not be dereferenced. */
	if (out == NULL || when_set == NULL || when_clear == NULL)
		return;

	/*
	 * Selects each byte with the same mask: the when_clear byte, changed
	 * to the when_set byte exactly where the mask has ones.  The inputs of
	 * a byte are read before its output is written, so out may be one of
	 * the inputs.
	 */
	byte_mask = (uint8_t)(mask & 0xffU);
	for (index = 0; index < size; index++) {
		chosen = (uint8_t)(when_clear[index] ^ ((when_set[index] ^ when_clear[index]) & byte_mask));
		out[index] = chosen;
	}
}

/*
 * Moves size bytes from source to destination when mask is 0xffffffff
 * and leaves destination unchanged when mask is 0, in constant time (a
 * conditional copy, e.g. of a decrypted value only when its tag was
 * good).
 */
void
cryc_ct_copy_if(
	uint8_t *destination,
	const uint8_t *source,
	size_t size,
	uint32_t mask)
{
	uint8_t byte_mask;
	uint8_t chosen;
	size_t index;

	/* With no bytes there is nothing to do. */
	if (size == 0)
		return;

	/* Missing buffers are a caller's mistake that must not be dereferenced. */
	if (destination == NULL || source == NULL)
		return;

	/*
	 * Changes each destination byte to the source byte exactly where the
	 * mask has ones, and stores every byte either way so that the stores
	 * do not depend on the mask.
	 */
	byte_mask = (uint8_t)(mask & 0xffU);
	for (index = 0; index < size; index++) {
		chosen = (uint8_t)(destination[index] ^ ((source[index] ^ destination[index]) & byte_mask));
		destination[index] = chosen;
	}
}

/*
 * Reads a 16-bit big-endian integer from bytes[0..1].
 */
uint32_t
cryc_load_be16(
	const uint8_t *bytes)
{
	uint32_t value;

	/* The first byte is the more significant one. */
	value = (uint32_t)bytes[0] << 8;
	value |= (uint32_t)bytes[1];

	/* Reports the 16-bit value. */
	return value;
}

/*
 * Reads a 24-bit big-endian integer from bytes[0..2].
 */
uint32_t
cryc_load_be24(
	const uint8_t *bytes)
{
	uint32_t value;

	/* The first byte is the most significant one. */
	value = (uint32_t)bytes[0] << 16;
	value |= (uint32_t)bytes[1] << 8;
	value |= (uint32_t)bytes[2];

	/* Reports the 24-bit value. */
	return value;
}

/*
 * Reads a 32-bit big-endian integer from bytes[0..3].
 */
uint32_t
cryc_load_be32(
	const uint8_t *bytes)
{
	uint32_t value;

	/* The first byte is the most significant one. */
	value = (uint32_t)bytes[0] << 24;
	value |= (uint32_t)bytes[1] << 16;
	value |= (uint32_t)bytes[2] << 8;
	value |= (uint32_t)bytes[3];

	/* Reports the 32-bit value. */
	return value;
}

/*
 * Reads a 64-bit big-endian integer from bytes[0..7].
 */
uint64_t
cryc_load_be64(
	const uint8_t *bytes)
{
	uint64_t value;

	/* The first byte is the most significant one. */
	value = (uint64_t)bytes[0] << 56;
	value |= (uint64_t)bytes[1] << 48;
	value |= (uint64_t)bytes[2] << 40;
	value |= (uint64_t)bytes[3] << 32;
	value |= (uint64_t)bytes[4] << 24;
	value |= (uint64_t)bytes[5] << 16;
	value |= (uint64_t)bytes[6] << 8;
	value |= (uint64_t)bytes[7];

	/* Reports the 64-bit value. */
	return value;
}

/*
 * Reads a 32-bit little-endian integer from bytes[0..3].
 */
uint32_t
cryc_load_le32(
	const uint8_t *bytes)
{
	uint32_t value;

	/* The first byte is the least significant one. */
	value = (uint32_t)bytes[0];
	value |= (uint32_t)bytes[1] << 8;
	value |= (uint32_t)bytes[2] << 16;
	value |= (uint32_t)bytes[3] << 24;

	/* Reports the 32-bit value. */
	return value;
}

/*
 * Writes the low 16 bits of value big-endian into bytes[0..1].
 */
void
cryc_store_be16(
	uint8_t *bytes,
	uint32_t value)
{
	/* The more significant byte goes first; higher bits are dropped. */
	bytes[0] = (uint8_t)((value >> 8) & 0xffU);
	bytes[1] = (uint8_t)(value & 0xffU);
}

/*
 * Writes the low 24 bits of value big-endian into bytes[0..2].
 */
void
cryc_store_be24(
	uint8_t *bytes,
	uint32_t value)
{
	/* The most significant byte goes first; higher bits are dropped. */
	bytes[0] = (uint8_t)((value >> 16) & 0xffU);
	bytes[1] = (uint8_t)((value >> 8) & 0xffU);
	bytes[2] = (uint8_t)(value & 0xffU);
}

/*
 * Writes value big-endian into bytes[0..3].
 */
void
cryc_store_be32(
	uint8_t *bytes,
	uint32_t value)
{
	/* The most significant byte goes first. */
	bytes[0] = (uint8_t)((value >> 24) & 0xffU);
	bytes[1] = (uint8_t)((value >> 16) & 0xffU);
	bytes[2] = (uint8_t)((value >> 8) & 0xffU);
	bytes[3] = (uint8_t)(value & 0xffU);
}

/*
 * Writes value big-endian into bytes[0..7].
 */
void
cryc_store_be64(
	uint8_t *bytes,
	uint64_t value)
{
	/* The most significant byte goes first. */
	bytes[0] = (uint8_t)((value >> 56) & 0xffU);
	bytes[1] = (uint8_t)((value >> 48) & 0xffU);
	bytes[2] = (uint8_t)((value >> 40) & 0xffU);
	bytes[3] = (uint8_t)((value >> 32) & 0xffU);
	bytes[4] = (uint8_t)((value >> 24) & 0xffU);
	bytes[5] = (uint8_t)((value >> 16) & 0xffU);
	bytes[6] = (uint8_t)((value >> 8) & 0xffU);
	bytes[7] = (uint8_t)(value & 0xffU);
}

/*
 * Writes value little-endian into bytes[0..3].
 */
void
cryc_store_le32(
	uint8_t *bytes,
	uint32_t value)
{
	/* The least significant byte goes first. */
	bytes[0] = (uint8_t)(value & 0xffU);
	bytes[1] = (uint8_t)((value >> 8) & 0xffU);
	bytes[2] = (uint8_t)((value >> 16) & 0xffU);
	bytes[3] = (uint8_t)((value >> 24) & 0xffU);
}

/*
 * Writes value little-endian into bytes[0..7].
 */
void
cryc_store_le64(
	uint8_t *bytes,
	uint64_t value)
{
	/* The least significant byte goes first. */
	bytes[0] = (uint8_t)(value & 0xffU);
	bytes[1] = (uint8_t)((value >> 8) & 0xffU);
	bytes[2] = (uint8_t)((value >> 16) & 0xffU);
	bytes[3] = (uint8_t)((value >> 24) & 0xffU);
	bytes[4] = (uint8_t)((value >> 32) & 0xffU);
	bytes[5] = (uint8_t)((value >> 40) & 0xffU);
	bytes[6] = (uint8_t)((value >> 48) & 0xffU);
	bytes[7] = (uint8_t)((value >> 56) & 0xffU);
}
