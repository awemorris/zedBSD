/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Keeps the daemon's text-only extraction API backed by the shared MIME reader. */
#include "mms.h"
#include "userland/desktop/libmms/mms.h"

/*
 * Extracts only decoded UTF-8 plain text, preserving the existing daemon contract.
 */
int
btd_mms_text(
	const uint8_t *input,
	size_t length,
	char *output,
	size_t size,
	size_t *used,
	int *truncated)
{
	int error;

	/* Delegates MIME selection and transfer decoding to the common implementation. */
	error = mms_extract_text(input, length, output, size, used, truncated);
	if (error != 0)
		return error;

	/* Succeeded: the output contains only text suitable for a renderer. */
	return 0;
}
