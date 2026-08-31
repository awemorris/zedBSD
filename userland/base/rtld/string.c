/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD userland string component.
 */

#include "userland/base/rtld/rtld.h"

/*
 * Implements the rtld strlen operation.
 */
size_t
rtld_strlen(
	const char *s)
{
	size_t n;

	n = 0;

	/* Handles the s availability. */
	if (s != NULL)

		/* Continue while the operation condition remains true. */
		while (s[n] != '\0')
			n++;

	/* Returns the computed result. */
	return n;
}

/*
 * Implements the rtld strcmp operation.
 */
int
rtld_strcmp(
	const char *a,
	const char *b)
{
	/* Continue while the operation condition remains true. */
	while (*a != '\0' && *a == *b) {
		a++;
		b++;
	}

	/* Returns the computed result. */
	return (unsigned char)*a - (unsigned char)*b;
}

/*
 * Implements the rtld memcpy operation.
 */
void *
rtld_memcpy(
	void *destination,
	const void *source,
	size_t size)
{
	unsigned char *d;
	const unsigned char *s;

	/* Process each remaining element. */
	d = destination;
	s = source;
	while (size-- != 0)
		*d++ = *s++;
	/* Returns the computed result. */
	return destination;
}

/*
 * Implements the rtld memset operation.
 */
void *
rtld_memset(
	void *destination,
	int value,
	size_t size)
{
	unsigned char *d;

	/* Process each remaining element. */
	d = destination;
	while (size-- != 0)
		*d++ = (unsigned char)value;
	/* Returns the computed result. */
	return destination;
}

/*
 * GCC may lower aggregate copies and clears to the conventional libc entry points even when the runtime linker itself only calls the rtld_* helpers. The interpreter is self-contained, so provide those compiler libcalls here rather than leaving a bootstrap dependency on libc.so.
 */
void *
memcpy(
	void *destination,
	const void *source,
	size_t size)
{
	void *function_result;

	/* Obtains the rtld memcpy result. */
	function_result = rtld_memcpy(destination, source, size);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the memset operation.
 */
void *
memset(
	void *destination,
	int value,
	size_t size)
{
	void *function_result;

	/* Obtains the rtld memset result. */
	function_result = rtld_memset(destination, value, size);

	/* Returns the computed result. */
	return function_result;
}
