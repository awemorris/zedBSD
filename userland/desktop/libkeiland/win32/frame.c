/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The geometry of the Windows backend's window frame.
 */

#include "userland/desktop/libkeiland/win32/frame.h"

#include <errno.h>
#include <stddef.h>

/* Not implemented yet. */
uint32_t
keiui_win32_scale(
	uint32_t value,
	unsigned dpi)
{
	(void)value;
	(void)dpi;

	/* A skeleton: the function is not written yet. */
	return 0;
}

/* Not implemented yet. */
void
keiui_win32_frame_buttons(
	const struct keiui_win32_frame *frame,
	struct keiui_win32_rect buttons[3])
{
	(void)frame;
	(void)buttons;
}

/* Not implemented yet. */
unsigned
keiui_win32_frame_hit(
	const struct keiui_win32_frame *frame,
	int32_t x,
	int32_t y)
{
	(void)frame;
	(void)x;
	(void)y;

	/* A skeleton: the function is not written yet. */
	return 0;
}
