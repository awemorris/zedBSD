/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The per-thread error queue.
 */

#include "cryc-err.h"

/*
 * Packs a library and a reason into a code.
 */
unsigned long
cryc_err_pack(
	unsigned library,
	unsigned reason)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(library);
	UNUSED_PARAMETER(reason);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Adds a code to the calling thread's queue (dropping the oldest when
 * full; dropped silently without memory).
 */
void
cryc_err_put(
	unsigned library,
	unsigned reason)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(library);
	UNUSED_PARAMETER(reason);
}

/*
 * Removes and returns the oldest code, or 0 when empty (ERR_get_error).
 */
unsigned long
cryc_err_get(void)
{
	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Returns the oldest code without removing it, or 0 (ERR_peek_error).
 */
unsigned long
cryc_err_peek(void)
{
	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Empties the calling thread's queue (ERR_clear_error).
 */
void
cryc_err_clear(void)
{
}

/*
 * Writes "error:%08lX:<library>::<reason>" for code into buffer (length
 * bytes, NUL-terminated, cut to fit; nothing when length is 0).
 */
void
cryc_err_string(
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
 * Registers libssl-compat's reason texts: table is count pairs of a reason
 * number and its text, constant for the life of the process.
 */
void
cryc_err_register_ssl(
	const unsigned *reasons,
	const char *const *texts,
	size_t count)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(reasons);
	UNUSED_PARAMETER(texts);
	UNUSED_PARAMETER(count);
}
