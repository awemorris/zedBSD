/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/rtld/rtld.h"

size_t rtld_strlen(const char *s)
{
	size_t n = 0;
	if (s != NULL) while (s[n] != '\0') n++;
	return n;
}

int rtld_strcmp(const char *a, const char *b)
{
	while (*a != '\0' && *a == *b) { a++; b++; }
	return (unsigned char)*a - (unsigned char)*b;
}

void *rtld_memcpy(void *destination, const void *source, size_t size)
{
	unsigned char *d = destination;
	const unsigned char *s = source;
	while (size-- != 0) *d++ = *s++;
	return destination;
}

void *rtld_memset(void *destination, int value, size_t size)
{
	unsigned char *d = destination;
	while (size-- != 0) *d++ = (unsigned char)value;
	return destination;
}
