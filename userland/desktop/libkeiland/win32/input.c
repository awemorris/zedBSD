/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pointer and keyboard details of the Windows backend.
 */

#include "userland/desktop/libkeiland/win32/input.h"

#include <errno.h>
#include <stddef.h>

/* Not implemented yet. */
double
keiui_win32_wheel_pixels(
	int delta,
	int horizontal)
{
	(void)delta;
	(void)horizontal;

	/* A skeleton: the function is not written yet. */
	return 0;
}

/* Not implemented yet. */
unsigned
keiui_win32_modifiers(
	unsigned held)
{
	(void)held;

	/* A skeleton: the function is not written yet. */
	return 0;
}

/* Not implemented yet. */
uint32_t
keiui_win32_button_code(
	unsigned button)
{
	(void)button;

	/* A skeleton: the function is not written yet. */
	return 0;
}

/* Not implemented yet. */
void
keiui_win32_point(
	int64_t lparam,
	int32_t *x,
	int32_t *y)
{
	(void)lparam;
	(void)x;
	(void)y;
}

/* Not implemented yet. */
void
keiui_win32_key_flags(
	int64_t lparam,
	struct keiui_win32_key_flags *flags)
{
	(void)lparam;
	(void)flags;
}
