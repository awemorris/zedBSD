/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Stop accepted command lines at initialization without opening any GPU. */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Mark the first hardware boundary after successful argument validation.
 */
int
vkdemo_initialize(
	uint32_t device_index,
	int offscreen)
{
	/* Expose accepted options without accessing the selected device. */
	printf("VKDEMO-CLI-STUB initialize device-index=%u offscreen=%d\n", device_index, offscreen);
	errno = ENODEV;

	/* Refuse initialization so this fixture never claims to render a frame. */
	return -1;
}

/*
 * Reject unexpected rendering calls from a parser-only host fixture.
 */
int
vkdemo_render(
	uint32_t milliseconds,
	uint32_t frame,
	char digest[65])
{
	/* Keep unused arguments explicit in this deliberately unavailable backend. */
	(void)milliseconds;
	(void)frame;
	(void)digest;
	errno = ENODEV;

	/* Refuse rendering because this fixture never creates Vulkan resources. */
	return -1;
}

/*
 * Accept cleanup after the deliberately failed initialization.
 */
int
vkdemo_close(
	void)
{
	/* Succeeded: this fixture never acquires a GPU descriptor. */
	return 0;
}

/*
 * Name the deliberately absent Vulkan backend.
 */
const char *
vkdemo_error_operation(
	void)
{
	/* Succeeded: identify the boundary intentionally refused by this fixture. */
	return "CLI-initialization";
}

/*
 * Report the initialization failure without any Venus protocol identity.
 */
int32_t
vkdemo_error_code(
	void)
{
	/* Succeeded: expose the unavailable Vulkan backend's diagnostic code. */
	return -3;
}

/*
 * Reject exports because no actual GPU frame exists in this fixture.
 */
int
vkdemo_write_frame(
	const char *path)
{
	/* Keep the deliberate absence of renderer storage explicit. */
	(void)path;
	errno = ENODEV;

	/* Refuse to fabricate an image for accepted command-line options. */
	return -1;
}
