/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The HTML Format clipboard format of Windows.
 */

#include "userland/desktop/libkeiland/win32/cf-html.h"

#include <errno.h>
#include <stddef.h>

/* Not implemented yet. */
int
keiui_win32_cf_html_encode(
	const char *fragment,
	size_t length,
	const char *source_url,
	char *buffer,
	size_t size,
	size_t *out_length)
{
	(void)fragment;
	(void)length;
	(void)source_url;
	(void)buffer;
	(void)size;
	(void)out_length;

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
keiui_win32_cf_html_decode(
	const char *data,
	size_t length,
	const char **fragment,
	size_t *fragment_length,
	const char **source_url,
	size_t *source_url_length)
{
	(void)data;
	(void)length;
	(void)fragment;
	(void)fragment_length;
	(void)source_url;
	(void)source_url_length;

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}
