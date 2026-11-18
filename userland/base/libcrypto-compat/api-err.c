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
#include "cryc-os.h"

/*
 * Removes and returns the oldest code of the calling thread's queue, or 0
 * when the queue is empty.
 */
unsigned long
ERR_get_error(void)
{
	unsigned long code;

	/* Takes the oldest code out of the calling thread's queue. */
	code = cryc_err_get();

	/* The code, or 0 for an empty queue. */
	return code;
}

/*
 * Returns the oldest code without removing it, or 0.
 */
unsigned long
ERR_peek_error(void)
{
	unsigned long code;

	/* Reads the oldest code of the calling thread's queue. */
	code = cryc_err_peek();

	/* The code, or 0 for an empty queue. */
	return code;
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
	/* Writes the text. */
	cryc_err_string(code, buffer, length);
}

/*
 * Empties the calling thread's queue.
 */
void
ERR_clear_error(void)
{
	/* Forgets the queued codes. */
	cryc_err_clear();
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
	/* Without memory there is nothing to overwrite (a NULL with a length is a caller's mistake that is not followed). */
	if (memory == NULL)
		return;

	/* Overwrites the bytes through the volatile stores of the wipe. */
	cryc_wipe(memory, length);
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
	int equal;
	int difference;

	/* Compares the bytes in constant time: 1 when they are equal (also when length is 0), 0 otherwise (also for a NULL with a length). */
	equal = cryc_ct_equal((const uint8_t *)left, (const uint8_t *)right, length);

	/* Turns "equal" into 0 and "different" into 1 with arithmetic only. */
	difference = (int)(1U - (unsigned)equal);

	/* Reports 0 for equal bytes and 1 for different ones. */
	return difference;
}

/*
 * Fills count bytes of buffer with random bytes.
 */
int
RAND_bytes(
	unsigned char *buffer,
	int count)
{
	int error;

	/* A negative count is refused with an error queued. */
	if (count < 0) {
		cryc_err_put(CRYC_LIB_RAND, CRYC_R_PASSED_INVALID_ARGUMENT);
		return 0;
	}

	/* No bytes asked for: nothing to fill, whatever the buffer. */
	if (count == 0)
		return 1;

	/* A missing buffer for a nonzero count is refused with an error queued. */
	if (buffer == NULL) {
		cryc_err_put(CRYC_LIB_RAND, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Asks the operating system for the bytes. */
	error = cryc_os_random(NULL, buffer, (size_t)count);
	if (error != 0) {
		/* The system refused: no partial output stays behind, and the failure is queued. */
		cryc_wipe(buffer, (size_t)count);
		cryc_err_put(CRYC_LIB_RAND, CRYC_R_INTERNAL_ERROR);
		return 0;
	}

	/* Succeeded: buffer is full of random bytes. */
	return 1;
}
