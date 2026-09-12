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
	const char *device)
{
	/* Expose accepted options without accessing the selected device. */
	printf("VKDEMO-CLI-STUB initialize device=%s\n", device);
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
 * Identify failure before any real Venus command was submitted.
 */
uint32_t
vkdemo_active_command(
	void)
{
	/* Succeeded: command zero identifies the intentionally absent backend. */
	return 0;
}
