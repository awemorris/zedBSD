/*
 * zedBSD freestanding C library
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "libc/heap.h"

#include <stddef.h>
#include <errno.h>
#include <string.h>

void *
memcpy(void *destination, const void *source, size_t count)
{
	unsigned char *dst = destination;
	const unsigned char *src = source;

	while (count-- != 0)
		*dst++ = *src++;
	return destination;
}

void *
memmove(void *destination, const void *source, size_t count)
{
	unsigned char *dst = destination;
	const unsigned char *src = source;

	if (dst < src) {
		while (count-- != 0)
			*dst++ = *src++;
	} else if (dst > src) {
		dst += count;
		src += count;
		while (count-- != 0)
			*--dst = *--src;
	}
	return destination;
}

void *
memset(void *destination, int value, size_t count)
{
	unsigned char *dst = destination;

	while (count-- != 0)
		*dst++ = (unsigned char)value;
	return destination;
}

void *
memchr(const void *memory, int character, size_t count)
{
	const unsigned char *bytes = memory;
	unsigned char wanted = (unsigned char)character;

	while (count-- != 0) {
		if (*bytes == wanted)
			return (void *)bytes;
		bytes++;
	}
	return NULL;
}

int
memcmp(const void *left, const void *right, size_t count)
{
	const unsigned char *a = left;
	const unsigned char *b = right;

	while (count-- != 0) {
		if (*a != *b)
			return *a < *b ? -1 : 1;
		a++;
		b++;
	}
	return 0;
}

size_t
strlen(const char *string)
{
	const char *end = string;
	while (*end != '\0')
		end++;
	return (size_t)(end - string);
}

size_t
strnlen(const char *string, size_t maximum)
{
	size_t length = 0;
	while (length < maximum && string[length] != '\0')
		length++;
	return length;
}

int
strcmp(const char *left, const char *right)
{
	while (*left != '\0' && *left == *right) {
		left++;
		right++;
	}
	return *(const unsigned char *)left - *(const unsigned char *)right;
}

int
strncmp(const char *left, const char *right, size_t count)
{
	while (count != 0 && *left != '\0' && *left == *right) {
		left++;
		right++;
		count--;
	}
	if (count == 0)
		return 0;
	return *(const unsigned char *)left - *(const unsigned char *)right;
}

char *
strcpy(char *destination, const char *source)
{
	char *result = destination;
	while ((*destination++ = *source++) != '\0')
		;
	return result;
}

char *
strncpy(char *destination, const char *source, size_t count)
{
	char *result = destination;
	while (count != 0 && *source != '\0') {
		*destination++ = *source++;
		count--;
	}
	while (count-- != 0)
		*destination++ = '\0';
	return result;
}

char *
strcat(char *destination, const char *source)
{
	strcpy(destination + strlen(destination), source);
	return destination;
}

char *
strncat(char *destination, const char *source, size_t count)
{
	char *tail = destination + strlen(destination);
	while (count-- != 0 && *source != '\0')
		*tail++ = *source++;
	*tail = '\0';
	return destination;
}

char *
strchr(const char *string, int character)
{
	char wanted = (char)character;
	for (;;) {
		if (*string == wanted)
			return (char *)string;
		if (*string++ == '\0')
			return NULL;
	}
}

char *
strrchr(const char *string, int character)
{
	const char *last = NULL;
	char wanted = (char)character;
	for (;;) {
		if (*string == wanted)
			last = string;
		if (*string++ == '\0')
			return (char *)last;
	}
}

char *
strstr(const char *haystack, const char *needle)
{
	size_t length = strlen(needle);
	if (length == 0)
		return (char *)haystack;
	while (*haystack != '\0') {
		if (strncmp(haystack, needle, length) == 0)
			return (char *)haystack;
		haystack++;
	}
	return NULL;
}

char *
strdup(const char *string)
{
	return zedbsd_strdup(string);
}

char *
strerror(int error)
{
	switch (error) {
	case EDOM: return "Domain error";
	case ERANGE: return "Range error";
	case EINVAL: return "Invalid argument";
	case ENOMEM: return "Not enough memory";
	case EIO: return "Input/output error";
	case ENOENT: return "No such file or directory";
	case EINTR: return "Interrupted system call";
	case ENOSPC: return "No space left on device";
	case EROFS: return "Read-only file system";
	case EOVERFLOW: return "Value too large";
	case ENAMETOOLONG: return "File name too long";
	case ENXIO: return "No such device or address";
	case ENODEV: return "No such device";
	case ENOTDIR: return "Not a directory";
	case EISDIR: return "Is a directory";
	case EEXIST: return "File exists";
	case EBUSY: return "Device or resource busy";
	case ENOTEMPTY: return "Directory not empty";
	case EBADF: return "Bad file descriptor";
	case ENOSYS: return "Function not implemented";
	case EOPNOTSUPP: return "Operation not supported";
	case ENOEXEC: return "Executable format error";
	case EFAULT: return "Bad address";
	case EAGAIN: return "Resource temporarily unavailable";
	case EACCES: return "Permission denied";
	case ESRCH: return "No such process";
	case ECHILD: return "No child process";
	case E2BIG: return "Argument list too long";
	default: return "Unknown error";
	}
}
