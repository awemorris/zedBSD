/*
 * zedBSD freestanding C library
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "libc/stdio-internal.h"

extern uintptr_t zedbsd_stdio_thread_token(void) __attribute__((weak));
extern void zedbsd_stdio_lock_wait(volatile uint32_t *) __attribute__((weak));
extern void zedbsd_stdio_lock_wake(volatile uint32_t *) __attribute__((weak));

static uintptr_t
stdio_owner(void)
{
	uintptr_t owner = zedbsd_stdio_thread_token != NULL ?
	    zedbsd_stdio_thread_token() : 1U;
	return owner != 0 ? owner : 1U;
}

void
flockfile(FILE *stream)
{
	uintptr_t owner;

	if (stream == NULL)
		return;
	owner = stdio_owner();
	if (__atomic_load_n(&stream->lock, __ATOMIC_ACQUIRE) != 0 &&
	    stream->lock_owner == owner) {
		stream->lock_depth++;
		return;
	}
	while (__atomic_exchange_n(&stream->lock, 1U, __ATOMIC_ACQUIRE) != 0) {
		if (zedbsd_stdio_lock_wait != NULL)
			zedbsd_stdio_lock_wait(&stream->lock);
	}
	stream->lock_owner = owner;
	stream->lock_depth = 1;
}

int
ftrylockfile(FILE *stream)
{
	uintptr_t owner;

	if (stream == NULL)
		return -1;
	owner = stdio_owner();
	if (__atomic_load_n(&stream->lock, __ATOMIC_ACQUIRE) != 0 &&
	    stream->lock_owner == owner) {
		stream->lock_depth++;
		return 0;
	}
	if (__atomic_exchange_n(&stream->lock, 1U, __ATOMIC_ACQUIRE) != 0)
		return -1;
	stream->lock_owner = owner;
	stream->lock_depth = 1;
	return 0;
}

void
funlockfile(FILE *stream)
{
	if (stream == NULL || stream->lock_owner != stdio_owner() ||
	    stream->lock_depth == 0)
		return;
	if (--stream->lock_depth != 0)
		return;
	stream->lock_owner = 0;
	__atomic_store_n(&stream->lock, 0U, __ATOMIC_RELEASE);
	if (zedbsd_stdio_lock_wake != NULL)
		zedbsd_stdio_lock_wake(&stream->lock);
}

static int zedbsd_global_errno;
__attribute__((weak)) int *
zedbsd_errno_location(void)
{
	return &zedbsd_global_errno;
}

static FILE standard_input;
static FILE standard_output;
static FILE standard_error;
FILE *stdin = &standard_input;
FILE *stdout = &standard_output;
FILE *stderr = &standard_error;

__attribute__((weak)) size_t
zedbsd_console_write_bytes(const char *bytes, size_t length)
{
	(void)bytes;
	return length;
}

int
printf(const char *format, ...)
{
	char stack_buffer[256];
	char *buffer = stack_buffer;
	va_list arguments;
	int length;
	size_t written;

	flockfile(stdout);
	va_start(arguments, format);
	length = vsnprintf(stack_buffer, sizeof(stack_buffer), format, arguments);
	va_end(arguments);
	if (length < 0)
		goto failed;
	if ((size_t)length >= sizeof(stack_buffer)) {
		buffer = malloc((size_t)length + 1U);
		if (buffer == NULL) {
			errno = ENOMEM;
			goto failed;
		}
		va_start(arguments, format);
		(void)vsnprintf(buffer, (size_t)length + 1U, format, arguments);
		va_end(arguments);
	}
	written = fwrite(buffer, 1, (size_t)length, stdout);
	if (buffer != stack_buffer)
		free(buffer);
	funlockfile(stdout);
	return written == (size_t)length ? length : EOF;
failed:
	funlockfile(stdout);
	return EOF;
}

int
putchar(int character)
{
	return fputc(character, stdout);
}

int
puts(const char *string)
{
	size_t length = strlen(string);
	int result = 0;
	flockfile(stdout);
	if (fwrite(string, 1, length, stdout) != length ||
	    fwrite("\n", 1, 1, stdout) != 1)
		result = EOF;
	funlockfile(stdout);
	if (result == EOF)
		return EOF;
	return 0;
}

__attribute__((weak)) int
getc(FILE *stream)
{
	if (stream != NULL) {
		stream->eof = 1;
		stream->error = 1;
	}
	errno = EIO;
	return EOF;
}

__attribute__((weak)) size_t
fread(void *buffer, size_t size, size_t count, FILE *stream)
{
	(void)buffer;
	(void)size;
	(void)count;
	if (stream != NULL) {
		stream->eof = 1;
		stream->error = 1;
	}
	errno = EIO;
	return 0;
}

__attribute__((weak)) size_t
fwrite(const void *buffer, size_t size, size_t count, FILE *stream)
{
	size_t total;
	if (size != 0 && count > (size_t)-1 / size) {
		errno = EINVAL;
		return 0;
	}
	total = size * count;
	if (stream == stdout || stream == stderr)
		return zedbsd_console_write_bytes(buffer, total) == total ? count : 0;
	if (stream != NULL)
		stream->error = 1;
	errno = EIO;
	return 0;
}

int
fputc(int character, FILE *stream)
{
	unsigned char byte = (unsigned char)character;

	return fwrite(&byte, 1, 1, stream) == 1 ? byte : EOF;
}

int
fgetc(FILE *stream)
{
	return getc(stream);
}

int ferror(FILE *stream) { int value; if (stream == NULL) return 1; flockfile(stream); value = stream->error; funlockfile(stream); return value; }
int feof(FILE *stream) { int value; if (stream == NULL) return 0; flockfile(stream); value = stream->eof; funlockfile(stream); return value; }
void
clearerr(FILE *stream)
{
	if (stream != NULL) {
		flockfile(stream);
		stream->error = 0;
		stream->eof = 0;
		funlockfile(stream);
	}
}

int
fprintf(FILE *stream, const char *format, ...)
{
	char stack_buffer[256];
	char *buffer = stack_buffer;
	va_list arguments;
	int length;
	size_t written;
	if (stream == NULL) {
		errno = EINVAL;
		return EOF;
	}
	flockfile(stream);

	va_start(arguments, format);
	length = vsnprintf(stack_buffer, sizeof(stack_buffer), format, arguments);
	va_end(arguments);
	if (length < 0)
		goto failed;
	if ((size_t)length >= sizeof(stack_buffer)) {
		buffer = malloc((size_t)length + 1U);
		if (buffer == NULL) {
			errno = ENOMEM;
			goto failed;
		}
		va_start(arguments, format);
		(void)vsnprintf(buffer, (size_t)length + 1U, format, arguments);
		va_end(arguments);
	}
	written = fwrite(buffer, 1, (size_t)length, stream);
	if (buffer != stack_buffer)
		free(buffer);
	funlockfile(stream);
	return written == (size_t)length ? length : EOF;
failed:
	funlockfile(stream);
	return EOF;
}

int
sscanf(const char *string, const char *format, ...)
{
	va_list arguments;
	char *end;
	unsigned long value;
	int result = 0;

	while (*format == ' ' || *format == '\t')
		format++;
	if (*format++ != '%')
		return 0;
	va_start(arguments, format);
	if (*format == 'x' || *format == 'X') {
		unsigned int *destination = va_arg(arguments, unsigned int *);
		value = strtoul(string, &end, 16);
		if (end != string) {
			*destination = (unsigned int)value;
			result = 1;
		}
	} else if (*format == 'u') {
		unsigned int *destination = va_arg(arguments, unsigned int *);
		value = strtoul(string, &end, 10);
		if (end != string) {
			*destination = (unsigned int)value;
			result = 1;
		}
	} else if (*format == 'd' || *format == 'i') {
		int *destination = va_arg(arguments, int *);
		long signed_value = strtol(string, &end, *format == 'i' ? 0 : 10);
		if (end != string) {
			*destination = (int)signed_value;
			result = 1;
		}
	}
	va_end(arguments);
	return result;
}

__attribute__((weak)) int
access(const char *path, int mode)
{
	(void)path;
	(void)mode;
	errno = ENOENT;
	return -1;
}

__attribute__((weak)) int isatty(int descriptor) { (void)descriptor; return 0; }
__attribute__((weak)) int fileno(void *stream) { (void)stream; return -1; }
__attribute__((weak)) time_t time(time_t *result) { if (result != NULL) *result = 0; return 0; }

__attribute__((weak, noreturn)) void
zedbsd_libc_panic(const char *message)
{
	(void)message;
	for (;;)
		__asm__ volatile ("" ::: "memory");
}

__attribute__((weak)) void
abort(void)
{
	zedbsd_libc_panic("abort");
}

__attribute__((weak)) void
exit(int status)
{
	(void)status;
	zedbsd_libc_panic("exit");
}

void
zedbsd_assert_fail(const char *expression, const char *file, int line)
{
	char message[160];
	snprintf(message, sizeof(message), "assertion failed: %s (%s:%d)",
		expression, file, line);
	zedbsd_libc_panic(message);
}
