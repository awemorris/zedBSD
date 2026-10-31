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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(memory);
	UNUSED_PARAMETER(size);
}

/*
 * Compares size bytes of left and right in constant time.
 */
int
cryc_ct_equal(
	const uint8_t *left,
	const uint8_t *right,
	size_t size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(left);
	UNUSED_PARAMETER(right);
	UNUSED_PARAMETER(size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Returns 0xffffffff when value is 0 and 0 otherwise, in constant time.
 */
uint32_t
cryc_ct_mask_zero32(
	uint32_t value)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(value);

	/* Not implemented: the failure value. */
	return 0;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(left);
	UNUSED_PARAMETER(right);

	/* Not implemented: the failure value. */
	return 0;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(left);
	UNUSED_PARAMETER(right);

	/* Not implemented: the failure value. */
	return 0;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(when_set);
	UNUSED_PARAMETER(when_clear);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(mask);
}

/*
 * Moves size bytes from source to destination when mask is 0xffffffff and
 * leaves destination unchanged when mask is 0, in constant time (a
 * conditional copy, e.g.
 */
void
cryc_ct_copy_if(
	uint8_t *destination,
	const uint8_t *source,
	size_t size,
	uint32_t mask)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(destination);
	UNUSED_PARAMETER(source);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(mask);
}

/*
 * Reads a 16-bit big-endian integer from bytes[0..1].
 */
uint32_t
cryc_load_be16(
	const uint8_t *bytes)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(bytes);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Reads a 24-bit big-endian integer from bytes[0..2].
 */
uint32_t
cryc_load_be24(
	const uint8_t *bytes)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(bytes);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Reads a 32-bit big-endian integer from bytes[0..3].
 */
uint32_t
cryc_load_be32(
	const uint8_t *bytes)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(bytes);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Reads a 64-bit big-endian integer from bytes[0..7].
 */
uint64_t
cryc_load_be64(
	const uint8_t *bytes)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(bytes);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Reads a 32-bit little-endian integer from bytes[0..3].
 */
uint32_t
cryc_load_le32(
	const uint8_t *bytes)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(bytes);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Writes the low 16 bits of value big-endian into bytes[0..1].
 */
void
cryc_store_be16(
	uint8_t *bytes,
	uint32_t value)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(bytes);
	UNUSED_PARAMETER(value);
}

/*
 * Writes the low 24 bits of value big-endian into bytes[0..2].
 */
void
cryc_store_be24(
	uint8_t *bytes,
	uint32_t value)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(bytes);
	UNUSED_PARAMETER(value);
}

/*
 * Writes value big-endian into bytes[0..3].
 */
void
cryc_store_be32(
	uint8_t *bytes,
	uint32_t value)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(bytes);
	UNUSED_PARAMETER(value);
}

/*
 * Writes value big-endian into bytes[0..7].
 */
void
cryc_store_be64(
	uint8_t *bytes,
	uint64_t value)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(bytes);
	UNUSED_PARAMETER(value);
}

/*
 * Writes value little-endian into bytes[0..3].
 */
void
cryc_store_le32(
	uint8_t *bytes,
	uint32_t value)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(bytes);
	UNUSED_PARAMETER(value);
}

/*
 * Writes value little-endian into bytes[0..7].
 */
void
cryc_store_le64(
	uint8_t *bytes,
	uint64_t value)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(bytes);
	UNUSED_PARAMETER(value);
}
