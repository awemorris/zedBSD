/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/rtld/rtld.h"

size_t
rtld_strlen(const char *s)
{
	size_t n = 0;
	if (s != NULL)
		while (s[n] != '\0')
			n++;
	return n;
}

int
rtld_strcmp(const char *a, const char *b)
{
	while (*a != '\0' && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

void *
rtld_memcpy(void *destination, const void *source, size_t size)
{
	unsigned char *d = destination;
	const unsigned char *s = source;
	while (size-- != 0)
		*d++ = *s++;
	return destination;
}

void *
rtld_memset(void *destination, int value, size_t size)
{
	unsigned char *d = destination;
	while (size-- != 0)
		*d++ = (unsigned char)value;
	return destination;
}

/*
 * GCC may lower aggregate copies and clears to the conventional libc entry
 * points even when the runtime linker itself only calls the rtld_* helpers.
 * The interpreter is self-contained, so provide those compiler libcalls here
 * rather than leaving a bootstrap dependency on libc.so.
 */
void *
memcpy(void *destination, const void *source, size_t size)
{
	return rtld_memcpy(destination, source, size);
}

void *
memset(void *destination, int value, size_t size)
{
	return rtld_memset(destination, value, size);
}
