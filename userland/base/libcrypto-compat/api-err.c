/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The public ERR_, RAND_bytes, OPENSSL_cleanse and CRYPTO_memcmp
 * functions.
 */

#include "cryc-api.h"

/*
 * Removes and returns the oldest code of the calling thread's queue, or 0
 * when the queue is empty.
 */
unsigned long
ERR_get_error(void)
{
	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Returns the oldest code without removing it, or 0.
 */
unsigned long
ERR_peek_error(void)
{
	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Writes the text of code into buffer (length bytes, NUL-terminated, cut
 * to fit; nothing when length is 0).
 */
void
ERR_error_string_n(
	unsigned long code,
	char *buffer,
	size_t length)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(code);
	UNUSED_PARAMETER(buffer);
	UNUSED_PARAMETER(length);
}

/*
 * Empties the calling thread's queue.
 */
void
ERR_clear_error(void)
{
}

/*
 * Overwrites length bytes at memory with zeros in a way the compiler keeps
 * (a NULL memory with length 0 is allowed).
 */
void
OPENSSL_cleanse(
	void *memory,
	size_t length)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(memory);
	UNUSED_PARAMETER(length);
}

/*
 * Compares length bytes in constant time: returns 0 when equal and non-
 * zero otherwise (not an ordering).
 */
int
CRYPTO_memcmp(
	const void *left,
	const void *right,
	size_t length)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(left);
	UNUSED_PARAMETER(right);
	UNUSED_PARAMETER(length);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Fills count bytes of buffer with random bytes.
 */
int
RAND_bytes(
	unsigned char *buffer,
	int count)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(buffer);
	UNUSED_PARAMETER(count);

	/* Not implemented: the failure value. */
	return 0;
}
