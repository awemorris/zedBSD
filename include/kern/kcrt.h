/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernel C runtime (kcrt).
 *
 * The kernel and its drivers do not link the C library.  kcrt supplies the
 * small set of standard C functions they use, under kern_ names, with the
 * meaning of the C function of the same name.  The one difference is
 * kern_snprintf() and kern_vsnprintf(), which format a subset of the printf
 * conversions (see src/kern/kcrt.c).
 *
 * Which runtime to call:
 *
 * - The HAL C runtime (hal_memset, hal_memcpy, hal_strlen, hal_printf) is for
 *   the HAL itself and for the kernel's initialization stage, until
 *   kernel_entry() has set up the kernel heap.  It stays callable later, but
 *   no new kernel call to it is written.
 * - Everything else in the kernel and the drivers calls kcrt.
 * - kcrt never calls the HAL, and the HAL never calls kcrt.  The only link
 *   between them is the compiler: a structure copy or clear in any object,
 *   the HAL's included, may become a call to memcpy or memset, and kcrt
 *   defines those two standard names for it.  Kernel code does not call the
 *   standard names itself.
 *
 * Every kcrt function is pure: it takes no lock, allocates nothing and keeps
 * no state, so it can be called from any context, interrupt handlers
 * included.
 *
 * A host fixture compiles kernel source with the host compiler and links the
 * host C library, whichever ABI headers it reads (the host's, or zedBSD's with
 * KERN_UAPI_NATIVE, see <uapi/hosted.h>).  There the kern_ names are thin
 * wrappers of the host functions, so the fixture needs no kcrt object and the
 * sanitizers still see every copy.  The kernel declarations are chosen by the
 * zedBSD target compiler, which defines __ZEDBSD__; a kernel built by another
 * compiler, and the host test of kcrt itself, define KERN_KCRT_NATIVE.
 */

#ifndef KERN_KERN_KCRT_H
#define KERN_KERN_KCRT_H

#include <stdarg.h>
#include <stddef.h>

#if !defined(__ZEDBSD__) && !defined(KERN_KCRT_NATIVE)

#include <stdio.h>
#include <string.h>

/*
 * The host fixture face: every kern_ function forwards to the host C library
 * function of the same meaning.
 */

static __inline void *
kern_memcpy(
	void *destination,
	const void *source,
	size_t count)
{
	/* Copies with the host C library. */
	return memcpy(destination, source, count);
}

static __inline void *
kern_memmove(
	void *destination,
	const void *source,
	size_t count)
{
	/* Copies with the host C library. */
	return memmove(destination, source, count);
}

static __inline void *
kern_memset(
	void *destination,
	int value,
	size_t count)
{
	/* Fills with the host C library. */
	return memset(destination, value, count);
}

static __inline void *
kern_memset_explicit(
	void *destination,
	int value,
	size_t count)
{
	/* Fills with the host C library. */
	memset(destination, value, count);

	/* Keeps the compiler from deleting the fill as a dead store. */
	__asm__ __volatile__("" : : "r"(destination) : "memory");

	/* Reports the destination as memset() does. */
	return destination;
}

static __inline int
kern_memcmp(
	const void *left,
	const void *right,
	size_t count)
{
	/* Compares with the host C library. */
	return memcmp(left, right, count);
}

static __inline void *
kern_memchr(
	const void *memory,
	int character,
	size_t count)
{
	/* Searches with the host C library. */
	return memchr(memory, character, count);
}

static __inline size_t
kern_strlen(
	const char *string)
{
	/* Measures with the host C library. */
	return strlen(string);
}

static __inline size_t
kern_strnlen(
	const char *string,
	size_t maximum)
{
	size_t length;

	/*
	 * Counts the bytes before the terminator or the limit.  strnlen() is
	 * POSIX, not ISO C, and a fixture built with -std=c11 does not see it.
	 */
	length = 0;
	while (length < maximum && string[length] != '\0')
		length++;

	/* Reports the length, which is maximum when no terminator was seen. */
	return length;
}

static __inline int
kern_strcmp(
	const char *left,
	const char *right)
{
	/* Compares with the host C library. */
	return strcmp(left, right);
}

static __inline int
kern_strncmp(
	const char *left,
	const char *right,
	size_t count)
{
	/* Compares with the host C library. */
	return strncmp(left, right, count);
}

static __inline char *
kern_strcpy(
	char *destination,
	const char *source)
{
	/* Copies with the host C library. */
	return strcpy(destination, source);
}

static __inline char *
kern_strncpy(
	char *destination,
	const char *source,
	size_t count)
{
	/* Copies with the host C library. */
	return strncpy(destination, source, count);
}

static __inline char *
kern_strcat(
	char *destination,
	const char *source)
{
	/* Appends with the host C library. */
	return strcat(destination, source);
}

static __inline char *
kern_strchr(
	const char *string,
	int character)
{
	/* Searches with the host C library. */
	return strchr(string, character);
}

static __inline char *
kern_strrchr(
	const char *string,
	int character)
{
	/* Searches with the host C library. */
	return strrchr(string, character);
}

static __inline char *
kern_strstr(
	const char *haystack,
	const char *needle)
{
	/* Searches with the host C library. */
	return strstr(haystack, needle);
}

static __inline int
kern_vsnprintf(
	char *buffer,
	size_t capacity,
	const char *format,
	va_list arguments)
__attribute__((format(printf, 3, 0)));

static __inline int
kern_vsnprintf(
	char *buffer,
	size_t capacity,
	const char *format,
	va_list arguments)
{
	/* Formats with the host C library. */
	return vsnprintf(buffer, capacity, format, arguments);
}

static __inline int
kern_snprintf(
	char *buffer,
	size_t capacity,
	const char *format,
	...)
__attribute__((format(printf, 3, 4)));

static __inline int
kern_snprintf(
	char *buffer,
	size_t capacity,
	const char *format,
	...)
{
	va_list arguments;
	int length;

	/* Formats with the host C library. */
	va_start(arguments, format);
	length = vsnprintf(buffer, capacity, format, arguments);
	va_end(arguments);

	/* Reports the length vsnprintf() reported. */
	return length;
}

#else

/*
 * The kernel face: the functions are defined in src/kern/kcrt.c.
 */

void *
kern_memcpy(
	void *destination,
	const void *source,
	size_t count);

void *
kern_memmove(
	void *destination,
	const void *source,
	size_t count);

void *
kern_memset(
	void *destination,
	int value,
	size_t count);

void *
kern_memset_explicit(
	void *destination,
	int value,
	size_t count);

int
kern_memcmp(
	const void *left,
	const void *right,
	size_t count);

void *
kern_memchr(
	const void *memory,
	int character,
	size_t count);

size_t
kern_strlen(
	const char *string);

size_t
kern_strnlen(
	const char *string,
	size_t maximum);

int
kern_strcmp(
	const char *left,
	const char *right);

int
kern_strncmp(
	const char *left,
	const char *right,
	size_t count);

char *
kern_strcpy(
	char *destination,
	const char *source);

char *
kern_strncpy(
	char *destination,
	const char *source,
	size_t count);

char *
kern_strcat(
	char *destination,
	const char *source);

char *
kern_strchr(
	const char *string,
	int character);

char *
kern_strrchr(
	const char *string,
	int character);

char *
kern_strstr(
	const char *haystack,
	const char *needle);

int
kern_vsnprintf(
	char *buffer,
	size_t capacity,
	const char *format,
	va_list arguments)
__attribute__((format(printf, 3, 0)));

int
kern_snprintf(
	char *buffer,
	size_t capacity,
	const char *format,
	...)
__attribute__((format(printf, 3, 4)));

#ifndef KERN_KCRT_NO_STANDARD_NAMES
/*
 * The standard names the compiler may call for a structure copy or clear.
 *
 * Kernel code calls kern_memcpy() and kern_memset().  These declarations
 * exist for the compiler's own calls, which need no declaration, and for the
 * HAL's console output test build, which calls memset() by name.
 */
void *
memcpy(
	void *destination,
	const void *source,
	size_t count);

void *
memset(
	void *destination,
	int value,
	size_t count);
#endif

#endif

#endif
