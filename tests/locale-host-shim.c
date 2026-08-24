/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

static int host_errno;
static const void *thread_locale;

int *
__libc_errno_location(void)
{
	return &host_errno;
}

const void *
__pthread_locale_exchange(const void *locale, int setting)
{
	const void *previous = thread_locale;

	if (setting)
		thread_locale = locale;
	return previous;
}
