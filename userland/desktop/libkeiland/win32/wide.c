/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * UTF-16 and UTF-8 for the Windows backend.
 */

#include "userland/desktop/libkeiland/win32/wide.h"

#include <errno.h>
#include <stddef.h>

/* Not implemented yet. */
int
keiui_win32_utf8_from_utf16(
	const uint16_t *units,
	size_t count,
	char *buffer,
	size_t size,
	size_t *consumed)
{
	(void)units;
	(void)count;
	(void)buffer;
	(void)size;
	(void)consumed;

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
keiui_win32_utf16_from_utf8(
	const char *text,
	size_t length,
	uint16_t *units,
	size_t capacity,
	size_t *count)
{
	(void)text;
	(void)length;
	(void)units;
	(void)capacity;
	(void)count;

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
keiui_win32_char_unit(
	struct keiui_win32_surrogate *state,
	uint16_t unit,
	uint32_t *code_point)
{
	(void)state;
	(void)unit;
	(void)code_point;

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}
